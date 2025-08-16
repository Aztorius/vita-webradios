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

extern "C" {
#include "audio/mp3.h"
#include "m3u_parser/m3u.h"

int _newlib_heap_size_user = 32 * 1024 * 1024;
}

#define printf sceClibPrintf

#define BUFFER_LENGTH 8192
#define NSAMPLES 2048

enum player_state {
	PLAYER_STATE_WAITING,
	PLAYER_STATE_PLAYING,
	PLAYER_STATE_STOPPING,
};

struct player {
	enum player_state state;
	int http_thread_id;
	int player_thread_id;
	const char *url;
	const char *title;
};

static struct player player;
static int audio_mutex;

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
	const char *current_url = NULL;

	while (player.state != PLAYER_STATE_STOPPING) {
		if (sceKernelLockMutex(audio_mutex, 1, NULL) < 0) {
			sceKernelDelayThread(100000); // Delay for 100 ms
			printf("audio_thread: error locking mutex\n");
			continue;
		}

		if (player.state == PLAYER_STATE_WAITING || player.url != current_url) {
			current_url = player.url;
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
	
			// int compatible_freqs[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
			int vol = SCE_AUDIO_VOLUME_0DB;
			int channels = MP3_GetChannels();
			SceAudioOutMode channels_mode = SCE_AUDIO_OUT_MODE_STEREO;
			if (channels == 1) {
				channels_mode = SCE_AUDIO_OUT_MODE_MONO;
			} else if (channels == 2) {
				channels_mode = SCE_AUDIO_OUT_MODE_STEREO;
			} else {
				printf("Wrong number of channel in stream !");
				goto audio_thread_end;
			}
	
			port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, NSAMPLES, MP3_GetSampleRate(), channels_mode);
			printf("Playing %s %s sample_rate %i channels %i\n", player.title, player.url, MP3_GetSampleRate(), channels);
			sceAudioOutSetConfig(port, -1, -1, channels_mode);
			SceAudioOutChannelFlag flags = (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH & SCE_AUDIO_VOLUME_FLAG_R_CH);
			int volumes[2] = {vol, vol};
			sceAudioOutSetVolume(port, flags, volumes);
		}

		sceKernelUnlockMutex(audio_mutex, 1);
	
		if (outsize > 0)
			sceAudioOutOutput(port, outbuffer);
		else
			sceKernelDelayThread(100000); // Delay for 100 ms
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
		if (!playing && player.state == PLAYER_STATE_PLAYING) {
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
	ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

	ImGui_ImplVitaGL_TouchUsage(true);
	ImGui_ImplVitaGL_UseIndirectFrontTouch(false);
	ImGui_ImplVitaGL_UseRearTouch(false);
	ImGui_ImplVitaGL_GamepadUsage(true);
	ImGui_ImplVitaGL_MouseStickUsage(false);

	// Get or write playlist
	struct m3u_file *m3ufile = NULL;
	if (m3u_parse("ux0:/data/webradio/playlist.m3u", &m3ufile)) {
		m3u_file_free(m3ufile);
		
		// Playlist missing, creating default playlist
		sceIoMkdir("ux0:/data", 0777);
		sceIoMkdir("ux0:/data/webradio", 0777);

		// Copying playlist to correct location
		copyfile("ux0:/data/webradio/playlist.m3u", "default_playlist.m3u");
		m3u_parse("ux0:/data/webradio/playlist.m3u", &m3ufile);
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
	sceKernelStartThread(player.player_thread_id, 0, 0);
	sceKernelStartThread(player.http_thread_id, 0, 0);

	struct m3u_entry *current_entry = m3ufile->first_entry;
	if (!current_entry) {
		printf("File has no URL\n");
		return 1;
	}

	player.url = current_entry->url;
	player.title = current_entry->title;
	player.state = PLAYER_STATE_PLAYING;
 
	// Main loop
	bool done = false;
	static bool show_app = false;
	static ImGuiWindowFlags flags = (ImGuiWindowFlags)(ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
	while (!done) {
		ImGui_ImplVitaGL_NewFrame();

		{
			ImGui::GetIO().MouseDrawCursor = false;
			
    		ImGui::SetNextWindowPos(ImVec2(0, 0));
	    	ImGui::SetNextWindowSize(ImVec2(960, 544));

			if (ImGui::Begin("Example: Fullscreen window", &show_app, flags))
			{
				if (player.url && player.title && player.state == PLAYER_STATE_PLAYING) {
					ImGui::Text("Playing %s from %s", player.title, player.url);
				} else {
					ImGui::Text("Standby");
				}

				ImGui::Separator();
				m3u_entry *drawEntry = m3ufile->first_entry;
				while (drawEntry) {
					if (drawEntry->title) {
						ImGui::Button(drawEntry->title, ImVec2(960, 30));
					} else if (drawEntry->url) {
						ImGui::Button(drawEntry->url, ImVec2(960, 30));
					}
					drawEntry = drawEntry->next;
				}
				ImGui::End();
			}
		}

		ctrl_press = ctrl_peek;
		sceCtrlPeekBufferPositive(0, &ctrl_peek, 1);
		ctrl_press.buttons = ctrl_peek.buttons & ~ctrl_press.buttons;

		if (ctrl_press.buttons & SCE_CTRL_CROSS) {
			if (player.state == PLAYER_STATE_PLAYING) {
				// Stopping
				player.state = PLAYER_STATE_WAITING;
			} else {
				printf("Playing %s %s\n", current_entry->title, current_entry->url);
				player.url = current_entry->url;
				player.title = current_entry->title;
				player.state = PLAYER_STATE_PLAYING;
			}
		} else if (ctrl_press.buttons & SCE_CTRL_CIRCLE) {
			if (current_entry->next) {
				current_entry = current_entry->next;
				printf("Playing %s %s\n", current_entry->title, current_entry->url);
				player.url = current_entry->url;
				player.title = current_entry->title;
				player.state = PLAYER_STATE_PLAYING;
			}
		} else if (ctrl_press.buttons & SCE_CTRL_START) {
			done = true;
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

	// Cleanup
	ImGui_ImplVitaGL_Shutdown();
	ImGui::DestroyContext();
	vglEnd();

	sceSysmoduleUnloadModule(SCE_SYSMODULE_HTTPS);
	sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PAF);

	m3u_file_free(m3ufile);

	return 0;
}
