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
#include "utils.hpp"

extern "C" {
#include "audio/mp3.h"
#include "m3u_parser/m3u.h"

int _newlib_heap_size_user = 54 * 1024 * 1024;
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
	const char *url; // Station URL
	const char *title; // The station name
	char *song_title; // Song title
	bool new_song_title;
	neon_fft_config *visualizer_config;
};

static struct player player;
static int audio_mutex;
static int visualizer_mutex;


int play_webradio(const char *url)
{
	int res, tpl, conn, req;
	unsigned long long length = 0;

	char *responseHeaders;
	unsigned int responseHeaderSize = 0;

	const char *headerFieldValue = NULL;
	unsigned int headerFieldSize = 0;

	int icyMetaint = 0;
	bool icyMetadataEnabled = false;
	int nextIcyMetadataIndex = 0;
	int icyMetadataPartLength = 0;

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

	res = sceSslInit(300 * 1024);
	if(res < 0){
		sceClibPrintf("sceSslInit failed (0x%X)\n", res);
		goto http_term;
	}

	res = sceHttpInit(40 * 1024);
	if(res < 0){
		sceClibPrintf("sceHttpInit failed (0x%X)\n", res);
		goto netctl_term;
	}

	sceHttpsDisableOption(SCE_HTTPS_FLAG_SERVER_VERIFY);

	tpl = sceHttpCreateTemplate("PSVita", SCE_HTTP_VERSION_1_1, SCE_TRUE);
	if(tpl < 0){
		sceClibPrintf("sceHttpCreateTemplate failed (0x%X)\n", tpl);
		goto ssl_term;
	}

	res = sceHttpAddRequestHeader(tpl, "Accept", "audio/mpeg", SCE_HTTP_HEADER_ADD);
	sceClibPrintf("sceHttpAddRequestHeader=0x%X\n", res);
	// Accept Icy-Metadata for getting song title
	res = sceHttpAddRequestHeader(tpl, "Icy-Metadata", "1", SCE_HTTP_HEADER_ADD);
	sceClibPrintf("sceHttpAddRequestHeader=0x%X\n", res);

	conn = sceHttpCreateConnectionWithURL(tpl, url, SCE_TRUE);
	if(conn < 0){
		sceClibPrintf("sceHttpCreateConnectionWithURL failed (0x%X)\n", conn);
		goto http_del_temp;
	}

	req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, url, 0);
	if(req < 0){
		sceClibPrintf("sceHttpCreateRequestWithURL failed (0x%X)\n", req);
		goto http_del_conn;
	}

	res = sceHttpSendRequest(req, NULL, 0);
	if(res < 0){
		sceClibPrintf("sceHttpSendRequest failed (0x%X)\n", res);
		goto http_del_req;
	}

	res = sceHttpGetAllResponseHeaders(req, &responseHeaders, &responseHeaderSize);
	sceClibPrintf("sceHttpGetAllResponseHeaders=%i\n", res);
	sceClibPrintf("responseHeaderSize=%i\n", responseHeaderSize);

	res = sceHttpParseResponseHeader(responseHeaders, responseHeaderSize, "icy-metaint", &headerFieldValue, &headerFieldSize);
	sceClibPrintf("sceHttpParseResponseHeader=%i\n", res);
	if (res >= 0) {
		icyMetaint = strtol(headerFieldValue, NULL, 10);
		icyMetadataEnabled = true;
		nextIcyMetadataIndex = icyMetaint;
		sceClibPrintf("ICY metadata enabled with icy-metaint=%i\n", icyMetaint);
	}

	res = sceHttpGetResponseContentLength(req, &length);
	sceClibPrintf("sceHttpGetResponseContentLength=%i\n", length);

	if (res < 0) {
		recv_buffer = malloc(0x10000);
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
				if (icyMetadataEnabled) {
					// We need to ignore those metadata before feeding it into the MP3 decoder
					// Also we will decode those metadata to get the song title
					if (nextIcyMetadataIndex > res) {
						// We do not have metadata here
						nextIcyMetadataIndex -= res;
						ret = MP3_Feed(recv_buffer, res);
						if (ret) {
							printf("MP3_Feed error: %i\n", ret);
							break;
						}
					} else {
						if (nextIcyMetadataIndex > 0) {
							// Feed buffer before metadata
							ret = MP3_Feed(recv_buffer, nextIcyMetadataIndex);
							if (ret) {
								printf("MP3_Feed error: %i\n", ret);
								break;
							}
						}

						while (nextIcyMetadataIndex < res) {
							// Parse first byte to get metadata size for this chunk
							icyMetadataPartLength = (int)(*((char*)recv_buffer + nextIcyMetadataIndex)) * 16;
							if (icyMetadataPartLength > 0) {
								// We have some metadata to parse !
								sceClibPrintf("icyMetadataPartLength=%i\n", icyMetadataPartLength);
								if (player.song_title) {
									free(player.song_title);
									player.song_title = NULL;
								}
								if (nextIcyMetadataIndex + 1 + icyMetadataPartLength <= res) {
									player.song_title = icy_parse_stream_title((char*)recv_buffer + nextIcyMetadataIndex + 1, icyMetadataPartLength);
									if (player.song_title) {
										player.new_song_title = true;
										sceClibPrintf("Title: %s\n", player.song_title);
									}
								}
							} else if (icyMetadataPartLength < 0) {
								// This is some weird value, exit
								break;
							}

							if (nextIcyMetadataIndex + 1 + icyMetadataPartLength + icyMetaint <= res) {
								// Feed next data
								ret = MP3_Feed((char*)recv_buffer + nextIcyMetadataIndex + 1 + icyMetadataPartLength, icyMetaint);
							} else {
								// Feed until the end of buffer
								ret = MP3_Feed((char*)recv_buffer + nextIcyMetadataIndex + 1 + icyMetadataPartLength, res - (nextIcyMetadataIndex + 1 + icyMetadataPartLength));
							}
							if (ret) {
								printf("MP3_Feed error: %i\n", ret);
								break;
							}

							nextIcyMetadataIndex += 1 + icyMetadataPartLength + icyMetaint;
						}
						nextIcyMetadataIndex -= res;
					}
				} else {
					ret = MP3_Feed(recv_buffer, res);
				}

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

	free(recv_buffer);
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
				sceKernelLockMutex(visualizer_mutex, 1, NULL);
				neon_fft_free(player.visualizer_config);
				player.visualizer_config = nullptr;
				sceKernelUnlockMutex(visualizer_mutex, 1);
			}
	
			channels = MP3_GetChannels();
			int samplerate = MP3_GetSampleRate();
			int nsamples = 0;
			int compatible_freqs[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
			int samplerate_is_compatible = false;

			for (int i = 0; i < sizeof(compatible_freqs); i++) {
				if (samplerate == compatible_freqs[i]) {
					samplerate_is_compatible = true;
					break;
				}
			}

			if (!samplerate_is_compatible) {
				printf("Samplerate %i is not compatible\n", samplerate);
				goto audio_thread_end;
			}

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

			if (sceKernelLockMutex(visualizer_mutex, 1, NULL) >= 0) {
				player.visualizer_config = neon_fft_init(nsamples, samplerate, channels, 16);
				sceKernelUnlockMutex(visualizer_mutex, 1);
			} else {
				printf("visualizer_mutex lock error\n");
			}
	
			port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, nsamples, samplerate, channels_mode);
			printf("Playing %s %s sample_rate %i channels %i\n", player.title, player.url, samplerate, channels);
			sceAudioOutSetConfig(port, -1, -1, channels_mode);
			SceAudioOutChannelFlag flags = (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH & SCE_AUDIO_VOLUME_FLAG_R_CH);
			int vol = SCE_AUDIO_VOLUME_0DB;
			int volumes[2] = {vol, vol};
			if (sceAudioOutSetVolume(port, flags, volumes)) {
				printf("Error setting volume\n");
			}
		}

		sceKernelUnlockMutex(audio_mutex, 1);
		
		if (outsize > 0 && ret != -11) {
			// Only play music if there is some music data
			// Also ignore the first time we receive data (ret==-11) to prevent some bad noise
			if (sceKernelLockMutex(visualizer_mutex, 1, NULL) >= 0) {
				neon_fft_fill_buffer(player.visualizer_config, (int16_t*)outbuffer, outsize / (2 * channels)); // 2 bytes per sample
				sceKernelUnlockMutex(visualizer_mutex, 1);
			} else {
				printf("visualizer_mutex lock error\n");
			}
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

int main(void)
{
	vglInitExtended(0, 960, 544, 0x1800000, SCE_GXM_MULTISAMPLE_4X);

	// Setup ImGui binding
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	ImGui_ImplVitaGL_Init();

	// Setup style
	ImGui::StyleColorsClassic();

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

	sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
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
	player.song_title = nullptr;
	player.new_song_title = false;
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
	static bool show_main_widget = true;
	bool show_settings = false;
	static bool show_visualization = false;
	int title_show_start_time = 0;
	static ImGuiWindowFlags flags = (ImGuiWindowFlags)(ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);
	while (!done) {
		ImGui_ImplVitaGL_NewFrame();

		if (show_main_widget) {
			ImGui::GetIO().MouseDrawCursor = false;
			
    		ImGui::SetNextWindowPos(ImVec2(0, 0));
	    	ImGui::SetNextWindowSize(ImVec2(960, 544));
			
			if (ImGui::Begin("Vita Webradio", &show_main_widget, flags))
			{
				if (ImGui::Button("Webradios", ImVec2(0, 30))) {
					show_settings = false;
				}

				ImGui::SameLine();
				if (ImGui::Button("Visualizer", ImVec2(0, 30))) {
					show_settings = false;
					show_main_widget = false;
					show_visualization = true;
				}

				ImGui::SameLine();
				if (ImGui::Button("About", ImVec2(0, 30))) {
					show_settings = true;
				}

				ImGui::Separator();

				if (show_settings) {
					ImGui::Text("Add your webradios to ux0:/data/webradio/playlist.m3u");
					ImGui::Text("(circle) toggle visualization, (square) stop audio, (cross) play selected radio, (triangle) black screen, (R) next radio");
				} else {
					if (player.song_title && player.title && player.state == PLAYER_STATE_PLAYING) {
						ImGui::Text("Playing \"%s\" from %s", player.song_title, player.title);
					} else if (player.url && player.title && player.state == PLAYER_STATE_PLAYING) {
						ImGui::Text("Playing %s from %s", player.title, player.url);
					} else if (player.url && player.title && player.state == PLAYER_STATE_NEW) {
						ImGui::Text("Connecting to %s from %s", player.title, player.url);
					} else {
						ImGui::Text("Standby");
					}
	
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
							// Show visualization
							show_main_widget = false;
							show_visualization = true;
						}
						drawEntry = drawEntry->next;
					}
				}

				ImGui::End();
			}
		} else if (show_visualization) {
			ImGui::SetNextWindowPos(ImVec2(0, 0));
	    	ImGui::SetNextWindowSize(ImVec2(960, 544));

			if (ImGui::Begin("Vita Webradio Visualizer", &show_visualization, flags)) {
				sceKernelLockMutex(visualizer_mutex, 1, NULL);
				if (player.state == PLAYER_STATE_PLAYING && player.visualizer_config && player.visualizer_config->visualizer_data) {
					spectrum_analyser(player.visualizer_config);
					int bar_length = 960 / player.visualizer_config->bar_count;
					for (int i = 0; i < player.visualizer_config->bar_count; i++) {
						ImGui::GetWindowDrawList()->AddRectFilled(
							ImVec2((float)i * bar_length, 540.0),
							ImVec2((float)(i+1) * bar_length - 1, 540.0 - (player.visualizer_config->visualizer_data[i] - 50.0) * 5.0f),
							IM_COL32(0, 128, 0, 255));
					}
	
					if (player.new_song_title) {
						title_show_start_time = ImGui::GetTime();
						player.new_song_title = false;
					}
	
					if (player.song_title && ImGui::GetTime() - title_show_start_time < 10.0) {
						// Show the song title for 10 seconds
						ImGui::Text("%s", player.song_title);
					}
				} else if (player.state == PLAYER_STATE_WAITING) {
					ImGui::Text("Standbye");
				} else if (player.state == PLAYER_STATE_NEW) {
					ImGui::Text("Connecting...");
				}
				sceKernelUnlockMutex(visualizer_mutex, 1);
				ImGui::End();
			}
		}

		ctrl_press = ctrl_peek;
		sceCtrlPeekBufferPositive(0, &ctrl_peek, 1);
		ctrl_press.buttons = ctrl_peek.buttons & ~ctrl_press.buttons;

		if (ctrl_press.buttons & SCE_CTRL_START) {
			done = true;
		} else if (ctrl_press.buttons & SCE_CTRL_CIRCLE) {
			if (!show_main_widget && !show_visualization) {
				show_main_widget = false;
				show_visualization = true;
			} else {
				show_main_widget = !show_main_widget;
				show_visualization = !show_visualization;
			}
			player.new_song_title = show_visualization; // Show title again
		} else if (ctrl_press.buttons & SCE_CTRL_SQUARE) {
			player.state = PLAYER_STATE_WAITING;
		} else if (ctrl_press.buttons & SCE_CTRL_TRIANGLE) {
			if (!show_main_widget && !show_visualization) {
				// Return to normal state
				show_main_widget = true;
				show_visualization = false;
			} else {
				show_main_widget = false;
				show_visualization = false;
			}
		} else if (ctrl_press.buttons & SCE_CTRL_RTRIGGER) {
			if (current_entry && current_entry->next) {
				current_entry = current_entry->next;
				printf("Playing %s %s\n", current_entry->title, current_entry->url);
				player.url = current_entry->url;
				player.title = current_entry->title;
				player.state = PLAYER_STATE_NEW;
			} else if (m3ufile->first_entry) {
				current_entry = m3ufile->first_entry;
				printf("Playing %s %s\n", current_entry->title, current_entry->url);
				player.url = current_entry->url;
				player.title = current_entry->title;
				player.state = PLAYER_STATE_NEW;
			}
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
	sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);

	m3u_file_free(m3ufile);

	return 0;
}
