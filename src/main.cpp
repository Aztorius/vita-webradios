#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include <imgui_vita.h>
#include <vitaGL.h>

#include <psp2/ctrl.h>
#include <psp2/audiodec.h>
#include <psp2/audioout.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>
#include <psp2/libssl.h>
#include <psp2/net/http.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/paf.h>
#include <psp2/sysmodule.h>

#include "visualizer/neon_fft.hpp"

extern "C" {
#include "audio/mp3.h"
#include "m3u_parser/m3u.h"

int _newlib_heap_size_user = 48 * 1024 * 1024;
}

#define printf sceClibPrintf

#define BUFFER_LENGTH 8192

enum player_state {
	PLAYER_STATE_WAITING,
	PLAYER_STATE_NEW,
	PLAYER_STATE_PLAYING,
	PLAYER_STATE_STOPPING,
};

struct player {
	enum player_state state;
	int http_thread_id;
	int player_thread_id;
	const char *url;
	const char *title;
	neon_fft_config *visualizer_config;
};

static struct player player;
static int audio_mutex;
static int visualizer_mutex;

int play_webradio(const char *url)
{
	int res, tpl, conn, req;
	SceUInt64 length = 0;

	SceUID fd;
	void *recv_buffer = NULL;

	SceNetInitParam net_init_param;
	net_init_param.size = 0x100000;
	net_init_param.flags = 0;

	SceUID memid = sceKernelAllocMemBlock("SceNetMemory", 0x0C20D060, net_init_param.size, NULL);
	if(memid < 0){
		sceClibPrintf("sceKernelAllocMemBlock failed (0x%X)\n", memid);
		return memid;
	}

	sceKernelGetMemBlockBase(memid, &net_init_param.memory);

	res = sceNetInit(&net_init_param);
	if(res < 0){
		sceClibPrintf("sceNetInit failed (0x%X)\n", res);
		goto free_memblk;
	}

	res = sceNetCtlInit();
	if(res < 0){
		sceClibPrintf("sceNetCtlInit failed (0x%X)\n", res);
		goto net_term;
	}

	res = sceHttpInit(0x100000);
	if(res < 0){
		sceClibPrintf("sceHttpInit failed (0x%X)\n", res);
		goto netctl_term;
	}

	res = sceSslInit(0x100000);
	if(res < 0){
		sceClibPrintf("sceSslInit failed (0x%X)\n", res);
		goto http_term;
	}

	tpl = sceHttpCreateTemplate("PSVita", 2, 1);
	if(tpl < 0){
		sceClibPrintf("sceHttpCreateTemplate failed (0x%X)\n", tpl);
		goto ssl_term;
	}

	res = sceHttpAddRequestHeader(tpl, "Accept", "audio/mpeg", SCE_HTTP_HEADER_ADD);
	sceClibPrintf("sceHttpAddRequestHeader=0x%X\n", res);

	conn = sceHttpCreateConnectionWithURL(tpl, url, 0);
	if(conn < 0){
		sceClibPrintf("sceHttpCreateConnectionWithURL failed (0x%X)\n", conn);
		goto http_del_temp;
	}

	req = sceHttpCreateRequestWithURL(conn, 0, url, 0);
	if(req < 0){
		sceClibPrintf("sceHttpCreateRequestWithURL failed (0x%X)\n", req);
		goto http_del_conn;
	}

	res = sceHttpSendRequest(req, NULL, 0);
	if(res < 0){
		sceClibPrintf("sceHttpSendRequest failed (0x%X)\n", res);
		goto http_del_req;
	}

	res = sceHttpGetResponseContentLength(req, &length);
	sceClibPrintf("sceHttpGetResponseContentLength=%i\n", res);

	if (res < 0){
		recv_buffer = sce_paf_memalign(0x40, 0x10000);
		if (recv_buffer == NULL) {
			sceClibPrintf("sce_paf_memalign return to NULL\n");
			goto http_abort_req;
		}

		int ret = 0;
		player.state = PLAYER_STATE_PLAYING;

		while (player.state == PLAYER_STATE_PLAYING && player.url == url) {
			res = sceHttpReadData(req, recv_buffer, 0x10000);
			if (res <= 0) {
				break;
			}

			if (sceKernelLockMutex(audio_mutex, 1, NULL) < 0) {
				ret = 0;
				printf("Cannot lock mutex in http thread, skipping audio\n");
			} else {
				ret = MP3_Feed(recv_buffer, res);
				sceKernelUnlockMutex(audio_mutex, 1);
			}
			if (ret) {
				printf("MP3_Feed error: %i\n", ret);
				break;
			}
		}
	} else {
		sceClibPrintf("length=0x%llX\n", length);
		printf("This is a fixed size request ?! Aborting");
	}

http_abort_req:
	sceHttpAbortRequest(req);

http_del_req:
	sceHttpDeleteRequest(req);
	req = -1;

http_del_conn:
	sceHttpDeleteConnection(conn);
	conn = -1;

http_del_temp:
	sceHttpDeleteTemplate(tpl);
	tpl = -1;

ssl_term:
	sceSslTerm();

http_term:
	sceHttpTerm();

netctl_term:
	sceNetCtlTerm();

net_term:
	sceNetTerm();

free_memblk:
	sceKernelFreeMemBlock(memid);
	memid = -1;

	sce_paf_free(recv_buffer);
	recv_buffer = NULL;

	return 0;
}

int audio_thread(unsigned int args, void *argp) {
	int port = -1;

	int ret = 0;

	unsigned char outbuffer[BUFFER_LENGTH] = {0};
	unsigned int outsize = 0;
	int channels = 0;

	while (player.state != PLAYER_STATE_STOPPING) {
		if (sceKernelLockMutex(audio_mutex, 1, NULL) < 0) {
			sceKernelDelayThread(100000); // Delay for 100 ms
			printf("audio_thread: error locking mutex\n");
			continue;
		}

		if (player.state == PLAYER_STATE_WAITING || player.state == PLAYER_STATE_NEW) {
			do {
				// Consume everything
				ret = MP3_Decode(NULL, 0, outbuffer, BUFFER_LENGTH, &outsize);
			} while (!ret);
			sceKernelUnlockMutex(audio_mutex, 1);
			sceKernelDelayThread(100000); // Delay for 100 ms
			continue;
		}

		ret = MP3_Decode(NULL, 0, outbuffer, BUFFER_LENGTH, &outsize);
		if (ret == -11) {
			// New format, close old output if necessary
			if (port >= 0) {
				sceAudioOutReleasePort(port);
			}

			if (player.visualizer_config) {
				neon_fft_free(player.visualizer_config);
				player.visualizer_config = nullptr;
			}
	
			int vol = SCE_AUDIO_VOLUME_0DB;
			channels = MP3_GetChannels();
			int samplerate = MP3_GetSampleRate();
			int nsamples = 0;
			// int compatible_freqs[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
			SceAudioOutMode channels_mode = SCE_AUDIO_OUT_MODE_STEREO;
			if (channels == 1) {
				channels_mode = SCE_AUDIO_OUT_MODE_MONO;
				nsamples = BUFFER_LENGTH >> 1; // 2 bytes per sample in mono mode
			} else if (channels == 2) {
				channels_mode = SCE_AUDIO_OUT_MODE_STEREO;
				nsamples = BUFFER_LENGTH >> 2; // 4 bytes per sample in stereo mode (2x2)
			} else {
				printf("Wrong number of channel in stream !");
				goto audio_thread_end;
			}

			sceKernelLockMutex(visualizer_mutex, 1, NULL);
			player.visualizer_config = neon_fft_init(nsamples, samplerate, channels);
			sceKernelUnlockMutex(visualizer_mutex, 1);
	
			port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, nsamples, samplerate, channels_mode);
			printf("Playing %s %s sample_rate %i channels %i\n", player.title, player.url, samplerate, channels);
			sceAudioOutSetConfig(port, -1, -1, channels_mode);
			SceAudioOutChannelFlag flags = (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH & SCE_AUDIO_VOLUME_FLAG_R_CH);
			int volumes[2] = {vol, vol};
			sceAudioOutSetVolume(port, flags, volumes);
		}

		sceKernelUnlockMutex(audio_mutex, 1);

		if (outsize > 0) {
			sceKernelLockMutex(visualizer_mutex, 1, NULL);
			neon_fft_fill_src_buffer(player.visualizer_config, (int16_t*)outbuffer, outsize / (2 * channels));
			sceKernelUnlockMutex(visualizer_mutex, 1);
		}
	
		if (outsize > 0) {
			sceAudioOutOutput(port, outbuffer);
		} else {
			sceKernelDelayThread(100000); // Delay for 100 ms
		}
	}

audio_thread_end:
	if (port >= 0) {
		sceAudioOutReleasePort(port);
	}

	MP3_Term();
	return 0;
}

int http_thread(unsigned int args, void *argp) {
	int playing = 0;
	while (player.state != PLAYER_STATE_STOPPING)
	{
		if (!playing && player.state == PLAYER_STATE_NEW) {
			playing = 1;
			play_webradio(player.url);
			playing = 0;
		}

		sceKernelDelayThread(200000); // Delay for 200 ms
	}

	return 0;
}

int copyfile(const char *destfile, const char *srcfile)
{
	FILE *fout = fopen(destfile, "wb");
	if (!fout) {
		printf("Cannot create file %s\n", destfile);
		return -1;
	}

	FILE *fin = fopen(srcfile, "rb");
	if (!fin) {
		printf("Cannot open file %s\n", srcfile);
		return -1;
	}

	char buf[512];
	int nread = 0;

	do {
		nread = fread(buf, 1, 512, fin);
		if (nread > 0) {
			fwrite(buf, 1, nread, fout);
		}
	} while (nread > 0);

	fclose(fin);
	fclose(fout);
	return 0;
}

int main(void)
{
	vglInitExtended(0, 960, 544, 0x1800000, SCE_GXM_MULTISAMPLE_4X);

	// Setup ImGui binding
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	ImGui_ImplVitaGL_Init();

	// Setup style
	ImGui::StyleColorsDark();

	bool show_demo_window = true;
	bool show_another_window = false;
	ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);

	ImGui_ImplVitaGL_TouchUsage(true);
	ImGui_ImplVitaGL_UseIndirectFrontTouch(false);
	ImGui_ImplVitaGL_UseRearTouch(false);
	ImGui_ImplVitaGL_GamepadUsage(true);
	ImGui_ImplVitaGL_MouseStickUsage(false);

	// Get or write playlist
	struct m3u_file *m3ufile = NULL;
	if (m3u_parse("ux0:/data/webradio/playlist.m3u", &m3ufile)) {
		m3u_file_free(m3ufile);
		m3ufile = NULL;
		
		// Playlist missing, creating default playlist
		sceIoMkdir("ux0:/data", 0777);
		sceIoMkdir("ux0:/data/webradio", 0777);

		// Copying playlist to correct location
		copyfile("ux0:/data/webradio/playlist.m3u", "default_playlist.m3u");
		if (m3u_parse("ux0:/data/webradio/playlist.m3u", &m3ufile)) {
			printf("Error on parsing default playlist !");
		}
	}

	int res;
	SceUInt32 paf_init_param[6];
	SceSysmoduleOpt sysmodule_opt;

	paf_init_param[0] = 0x1100000;
	paf_init_param[1] = 0;
	paf_init_param[2] = 0;
	paf_init_param[3] = 0;
	paf_init_param[4] = 0x400;
	paf_init_param[5] = 1;

	res = ~0;
	sysmodule_opt.flags  = 0;
	sysmodule_opt.result = &res;

	sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof(paf_init_param), &paf_init_param, &sysmodule_opt);
	sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);

	audio_mutex = sceKernelCreateMutex("audioMutex", 0, 0, NULL);
	if (audio_mutex < 0) {
		printf("Error creating mutex\n");
		return 1;
	}

	visualizer_mutex = sceKernelCreateMutex("visualizerMutex", 0, 0, NULL);
	if (visualizer_mutex < 0) {
		printf("Error creating mutex\n");
		return 1;
	}

	int ret = MP3_Init();
	if (ret) {
		printf("MP3_Init %i\n", ret);
		return 1;
	}

	SceCtrlData ctrl_peek, ctrl_press;

	int thid = 0;
	thid = sceKernelCreateThread("httpThread", http_thread, 0x10000100, 0x10000, 0, 0, NULL);
	if (thid < 0) {
		sceClibPrintf("Error creating thread with id %i\n", thid);
		return 1;
	}

	player.http_thread_id = thid;

	thid = sceKernelCreateThread("audioThread", audio_thread, 0x10000100, 0x10000, 0, 0, NULL);
	if (thid < 0) {
		sceClibPrintf("Error creating thread with id %i\n", thid);
		return 1;
	}

	player.player_thread_id = thid;
	player.state = PLAYER_STATE_WAITING;
	player.visualizer_config = nullptr;
	sceKernelStartThread(player.player_thread_id, 0, 0);
	sceKernelStartThread(player.http_thread_id, 0, 0);

	struct m3u_entry *current_entry = m3ufile->first_entry;
	if (!current_entry) {
		printf("File has no URL\n");
		return 1;
	}

	player.url = current_entry->url;
	player.title = current_entry->title;
	player.state = PLAYER_STATE_NEW;
 
	// Main loop
	bool done = false;
	static bool show_app = false;
	bool show_main_widget = true;
	static ImGuiWindowFlags flags = (ImGuiWindowFlags)(ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
	while (!done) {
		ImGui_ImplVitaGL_NewFrame();

		if (show_main_widget) {
			ImGui::GetIO().MouseDrawCursor = false;
			
    		ImGui::SetNextWindowPos(ImVec2(0, 0));
	    	ImGui::SetNextWindowSize(ImVec2(960, 544));

			if (ImGui::Begin("Vita Webradio", &show_app, flags))
			{
				if (player.url && player.title && player.state == PLAYER_STATE_PLAYING) {
					ImGui::Text("Playing %s from %s", player.title, player.url);
				} else if (player.url && player.title && player.state == PLAYER_STATE_NEW) {
					ImGui::Text("Connecting to %s from %s", player.title, player.url);
				} else {
					ImGui::Text("Standby");
				}

				ImGui::Separator();

				ImGui::Text("Add your webradios to ux0:/data/webradio/playlist.m3u");
				ImGui::Text("(circle) show/hide user interface, (square) stop audio, (cross) play selected radio");

				ImGui::Separator();
				m3u_entry *drawEntry = m3ufile->first_entry;
				while (drawEntry) {
					const char *button_text = NULL;
					if (drawEntry->title) {
						button_text = drawEntry->title;
					} else if (drawEntry->url) {
						button_text = drawEntry->url;
					} else {
						button_text = "Unknown";
					}

					if (ImGui::Button(button_text, ImVec2(960, 30))) {
						current_entry = drawEntry;
						printf("Playing %s %s\n", current_entry->title, current_entry->url);
						player.url = current_entry->url;
						player.title = current_entry->title;
						player.state = PLAYER_STATE_NEW;
					}
					drawEntry = drawEntry->next;
				}

				ImGui::End();
			}
		} else {
			ImGui::SetNextWindowPos(ImVec2(0, 0));
	    	ImGui::SetNextWindowSize(ImVec2(960, 544));

			ImGui::Begin("Vita Webradio Visualizer", &show_app, flags);
			sceKernelLockMutex(visualizer_mutex, 1, NULL);
			if (player.state == PLAYER_STATE_PLAYING && player.visualizer_config && player.visualizer_config->dst_buffer) {
				for (int i = 0; i < player.visualizer_config->nbsamples / 2; i++) {
					float value = sqrt((float)(((int16_t*)player.visualizer_config->dst_buffer)[i])); // Reduce to max value 512
					ImGui::GetWindowDrawList()->AddLine(ImVec2((float)i, 540.0), ImVec2((float)i, value), IM_COL32(0, 128, 0, 128));
				}
			}
			sceKernelUnlockMutex(visualizer_mutex, 1);
			ImGui::End();
		}

		ctrl_press = ctrl_peek;
		sceCtrlPeekBufferPositive(0, &ctrl_peek, 1);
		ctrl_press.buttons = ctrl_peek.buttons & ~ctrl_press.buttons;

		if (ctrl_press.buttons & SCE_CTRL_START) {
			done = true;
		} else if (ctrl_press.buttons & SCE_CTRL_CIRCLE) {
			show_main_widget = !show_main_widget;
		} else if (ctrl_press.buttons & SCE_CTRL_SQUARE) {
			player.state = PLAYER_STATE_WAITING;
		}

		// Rendering
		glViewport(0, 0, (int)ImGui::GetIO().DisplaySize.x, (int)ImGui::GetIO().DisplaySize.y);
		glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui::Render();
		ImGui_ImplVitaGL_RenderDrawData(ImGui::GetDrawData());
		vglSwapBuffers(GL_FALSE);
	}

	player.state = PLAYER_STATE_STOPPING;

	int exitstatus = 0;
	SceUInt timeout = 10000000;

	ret = sceKernelWaitThreadEnd(player.http_thread_id, &exitstatus, &timeout);
    if (ret < 0 || exitstatus != 0)
    {
        sceClibPrintf("Error on http_thread exit. Exit status %i, return code %i\n", exitstatus, ret);
    }

	ret = sceKernelWaitThreadEnd(player.player_thread_id, &exitstatus, &timeout);
	if (ret < 0 || exitstatus != 0)
    {
        sceClibPrintf("Error on player_thread exit. Exit status %i, return code %i\n", exitstatus, ret);
    }

	sceKernelDeleteThread(player.player_thread_id);
    sceKernelDeleteThread(player.http_thread_id);

	sceKernelDeleteMutex(audio_mutex);
	sceKernelDeleteMutex(visualizer_mutex);

	// Cleanup
	ImGui::DestroyContext();
	vglEnd();

	sceSysmoduleUnloadModule(SCE_SYSMODULE_HTTPS);
	sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PAF);

	m3u_file_free(m3ufile);

	return 0;
}
