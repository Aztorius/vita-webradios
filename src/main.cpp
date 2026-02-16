#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <math.h>

#include <imgui_vita.h>
#include <vitaGL.h>
#include <curl/curl.h>
#include <neaacdec.h>

#include <psp2/ctrl.h>
#include <psp2/audiodec.h>
#include <psp2/audioout.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
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
#include "audio/aac.h"
#include "m3u_parser/m3u.h"

int _newlib_heap_size_user = 54 * 1024 * 1024;
}

#define printf sceClibPrintf

const char sceUserMainThreadName[]	= "vita_webradios";
const int sceUserMainThreadPriority	= 0x60;
const SceSize sceUserMainThreadStackSize	= 0x1000;


enum player_view {
	PLAYER_VIEW_MENU,
	PLAYER_VIEW_SETTINGS,
	PLAYER_VIEW_VISUALIZER_BARS,
	PLAYER_VIEW_VISUALIZER_CIRCLES,
	PLAYER_VIEW_BLACKSCREEN,
};

enum player_state {
	PLAYER_STATE_WAITING,
	PLAYER_STATE_NEW,
	PLAYER_STATE_PLAYING,
	PLAYER_STATE_STOPPING,
};

enum audio_format {
	AUDIO_FORMAT_UNKNOWN,
	AUDIO_FORMAT_MP3,
	AUDIO_FORMAT_AAC,
	AUDIO_FORMAT_OGG,
};

struct player {
	enum player_state state;
	player_view view;

	int http_thread_id;
	int player_thread_id;

	const char *url; // Station URL
	const char *title; // The station name
	char *song_title; // Song title
	bool new_song_title;

	audio_format audio_type;
	int samplerate;
	int nb_channels;
	int nb_samples;

	bool icy_metadata_enabled;
	int icy_metaint;
	int icy_count;
	int icy_part_length;

	neon_fft_config *visualizer_config;
	bool visualizer_rebuild;
};

static struct player player;

#define STREAM_BUFFER_SIZE (2 * 1024 * 1024)

static unsigned char stream_buffer[STREAM_BUFFER_SIZE];
static volatile int write_pos = 0;
static volatile int read_pos = 0;

#define ICY_METADATA_MAX 512

static char icy_metadata[ICY_METADATA_MAX];
static volatile int icy_metadata_ready = 0;

// Mutex
static int audio_mutex;
static int icy_meta_mutex;
static int visualizer_mutex;


int progress_callback(void *clientp,
                      curl_off_t dltotal,
                      curl_off_t dlnow,
                      curl_off_t ultotal,
                      curl_off_t ulnow)
{
    if (player.state != PLAYER_STATE_PLAYING) {
		sceKernelLockMutex(audio_mutex, 1, NULL);
		read_pos = 0;
		write_pos = 0;
		sceKernelUnlockMutex(audio_mutex, 1);

		// stop curl
        return 1;
	}

    return 0;
}

size_t stream_callback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t bytes = size * nmemb;
    unsigned char *data = (unsigned char *)ptr;
	size_t i = 0;

	if (!player.icy_metadata_enabled) {
		sceKernelLockMutex(audio_mutex, 1, NULL);

		while (i < bytes) {

			int next = (write_pos + 1) % STREAM_BUFFER_SIZE;

			// Full buffer
			if (next == read_pos) {
				break;
			}

			stream_buffer[write_pos] = data[i];
			write_pos = next;

			i++;
		}

		sceKernelUnlockMutex(audio_mutex, 1);
	} else {
		// Audio + ICY Metadata
		while (i < bytes) {
			// Audio
			if (player.icy_metaint > 0 && player.icy_count > 0) {
				sceKernelLockMutex(audio_mutex, 1, NULL);

				while (i < bytes && player.icy_count > 0) {
					int next = (write_pos + 1) % STREAM_BUFFER_SIZE;
					if (next != read_pos) {
						stream_buffer[write_pos] = data[i];
						write_pos = next;
					}

					player.icy_count--;
					i++;
				}
	
				sceKernelUnlockMutex(audio_mutex, 1);
			}
	
			// Metadata
			if (i < bytes && player.icy_count == 0 && player.icy_metaint > 0) {
				int meta_len = int(data[i]) * 16;
				i++;
	
				if (meta_len > 0 && meta_len < ICY_METADATA_MAX) {
					sceKernelLockMutex(icy_meta_mutex, 1, NULL);
	
					memcpy(icy_metadata, &data[i], meta_len);
					icy_metadata[meta_len] = 0;
					icy_metadata_ready = 1;
	
					sceKernelUnlockMutex(icy_meta_mutex, 1);
				}
	
				i += meta_len;
				player.icy_count = player.icy_metaint;
			}
		}
    }

    return bytes;
}

size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
    size_t len = size * nitems;

    if (!strncasecmp(buffer, "icy-metaint:", 12)) {
        int metaint = atoi(buffer + 12);
        printf("ICY metaint = %d\n", metaint);

		player.icy_metadata_enabled = true;
		player.icy_metaint = metaint;
		player.icy_count = metaint;
    }

    if (!strncasecmp(buffer, "content-type:", 13)) {
        printf("Content-Type = %.*s", (int)len, buffer);

		if (strstr(buffer, "audio/mpeg")) {
			player.audio_type = AUDIO_FORMAT_MP3;
		} else if (strstr(buffer, "audio/aac")) {
			player.audio_type = AUDIO_FORMAT_AAC;
		} else if (strstr(buffer, "audio/ogg")) {
			player.audio_type = AUDIO_FORMAT_OGG;
		} else {
			printf("Audio type unknown, suppose MP3\n");
			player.audio_type = AUDIO_FORMAT_MP3;
		}

		printf("Audio type detected: %i\n", player.audio_type);
    }

    return len;
}

int network_thread(unsigned int args, void *argp)
{
	while (player.state != PLAYER_STATE_STOPPING) {
		// Wait for a new station
		while (player.state != PLAYER_STATE_NEW) {
			sceKernelDelayThread(100000);
		}

		CURL *curl = curl_easy_init();
	
		curl_easy_setopt(curl, CURLOPT_URL, player.url);
	
		// Headers
		struct curl_slist *headers = NULL;
		headers = curl_slist_append(headers, "User-Agent: VitaWebradios/2.0");
		headers = curl_slist_append(headers, "Icy-MetaData: 1");
		headers = curl_slist_append(headers, "Accept: */*");
		headers = curl_slist_append(headers, "Connection: keep-alive");
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	
		// Headers callback
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);
	
		// Stream callback
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_callback);
		curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 16 * 1024);
		curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
	
		// Progress callback
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, NULL);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	
		// HTTPS (disable checks)
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	
		player.state = PLAYER_STATE_PLAYING;
	
		curl_easy_perform(curl);	// Blocking
	
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
	}

    return 0;
}

#define AUDIO_CHUNK 4096
#define BUFFER_LENGTH 8192

int audio_thread(unsigned int args, void *argp)
{
    unsigned char audio_chunk[AUDIO_CHUNK] = {0};
	unsigned char outbuffer[BUFFER_LENGTH] = {0};
	unsigned int outsize = 0;
	unsigned long channels = 0;
	unsigned long samplerate = 0;
	int port = -1;
	int ret = 0;

	// Init mpg123 for MP3
	ret = MP3_Init();
	if (ret) {
		printf("MP3_Init %i\n", ret);
		return 1;
	}

	bool aac_initialized = false;
	NeAACDecHandle aac_decoder = NULL;

	// Main audio loop
    while (player.state != PLAYER_STATE_STOPPING) {
		sceKernelLockMutex(audio_mutex, 1, NULL);

		do {
			// Consume every MP3 audio remaining without playing it
			ret = MP3_Decode(NULL, 0, outbuffer, BUFFER_LENGTH, &outsize);
		} while (!ret);

		sceKernelUnlockMutex(audio_mutex, 1);

		while (player.state == PLAYER_STATE_PLAYING) {
			int count = 0;

			sceKernelLockMutex(audio_mutex, 1, NULL);

			if (player.audio_type == AUDIO_FORMAT_MP3) {
				while (read_pos != write_pos && count < AUDIO_CHUNK) {
					audio_chunk[count++] = stream_buffer[read_pos];
					read_pos = (read_pos + 1) % STREAM_BUFFER_SIZE;

					if (count == AUDIO_CHUNK || read_pos == write_pos) {
						// We have a full chunk or the last partial chunk
						ret = MP3_Feed(audio_chunk, count);
					}
				}
			} else if (player.audio_type == AUDIO_FORMAT_AAC) {
				while (read_pos != write_pos && count < AUDIO_CHUNK) {
					// Read one byte from stream to the chunk
					audio_chunk[count++] = stream_buffer[read_pos];
					read_pos = (read_pos + 1) % STREAM_BUFFER_SIZE;

					if (count >= 7) {
						// We have enough data to test if ADTS is present
						adts_header_t adts_header;
						if (parse_adts_header(audio_chunk, count, &adts_header) != 0) {
							// No ADTS header, move chunk by one byte to the left
							memmove(audio_chunk, audio_chunk + 1, count - 1);
							count--;
							continue;
						}

						// We have a valid ADTS header
						// We need to wait for a complete frame
						if (count < adts_header.frame_length) {
							// Not a complete frame (yet)
							continue;
						}

						sceKernelUnlockMutex(audio_mutex, 1);

						if (!aac_initialized) {
							// Init faad2 for AAC
							printf("FAAD2 capabilities 0x%x\n", NeAACDecGetCapabilities());
							aac_decoder = NeAACDecOpen();
							NeAACDecConfigurationPtr aac_cfg = NeAACDecGetCurrentConfiguration(aac_decoder);
							aac_cfg->outputFormat = FAAD_FMT_16BIT;
							aac_cfg->downMatrix = 1; // A 5.1 channels should be downmatrixed to 2.0 channels for Vita
							if (!NeAACDecSetConfiguration(aac_decoder, aac_cfg)) {
								printf("Error with NeAACDecSetConfiguration\n");
							}

							long ret = NeAACDecInit(aac_decoder, audio_chunk, count, &samplerate, (unsigned char *)&channels);

							if (ret < 0) {
								NeAACDecClose(aac_decoder);
								aac_decoder = NULL;
								return -1;
							}

							if (port >= 0) {
								sceAudioOutReleasePort(port);
							}

							if (!is_samplerate_vita_compatible(samplerate)) {
								printf("Samplerate %i is not compatible\n", samplerate);
								break;
							}

							int nsamples = 0;

							SceAudioOutMode channels_mode = SCE_AUDIO_OUT_MODE_STEREO;
							if (channels == 1) {
								channels_mode = SCE_AUDIO_OUT_MODE_MONO;
								nsamples = BUFFER_LENGTH >> 1; // 2 bytes per sample in mono mode
							} else {
								channels_mode = SCE_AUDIO_OUT_MODE_STEREO;
								nsamples = BUFFER_LENGTH >> 2; // 4 bytes per sample in stereo mode (2x2)
							}

							player.samplerate = samplerate;
							player.nb_channels = channels;
							player.nb_samples = nsamples;
							player.visualizer_rebuild = true;

							aac_initialized = true;

							// AAC always works with 1024 samples
							port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, 1024, samplerate, channels_mode);
							printf("Playing %s %s sample_rate %i channels %i\n", player.title, player.url, samplerate, channels);
						}

						// We have a complete frame and FAAD2 is initialized, let's decode and play
						NeAACDecFrameInfo aac_frame_info;
						void *pcm = NeAACDecDecode(aac_decoder, &aac_frame_info, audio_chunk, count);
						if (aac_frame_info.error == 0 && aac_frame_info.samples > 0) {
							sceKernelLockMutex(visualizer_mutex, 1, NULL);
							neon_fft_fill_buffer(player.visualizer_config, (int16_t*)pcm, 1024 / channels);
							sceKernelUnlockMutex(visualizer_mutex, 1);

							sceAudioOutOutput(port, pcm);
						}

						count = 0;

						sceKernelLockMutex(audio_mutex, 1, NULL);
					}
				}
			}

			sceKernelUnlockMutex(audio_mutex, 1);

			if (player.audio_type == AUDIO_FORMAT_MP3) {
				ret = MP3_Decode(NULL, 0, outbuffer, BUFFER_LENGTH, &outsize);
	
				if (ret == -11) {
					// New format, close old output if necessary
					if (port >= 0) {
						sceAudioOutReleasePort(port);
					}
	
					channels = MP3_GetChannels();
					samplerate = MP3_GetSampleRate();
					int nsamples = 0;
	
					if (!is_samplerate_vita_compatible(samplerate)) {
						printf("Samplerate %i is not compatible\n", samplerate);
						break;
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
						break;
					}
	
					player.samplerate = samplerate;
					player.nb_channels = channels;
					player.nb_samples = nsamples;
					player.visualizer_rebuild = true;
	
					port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, nsamples, samplerate, channels_mode);
					printf("Playing %s %s sample_rate %i channels %i\n", player.title, player.url, samplerate, channels);
					sceAudioOutSetConfig(port, -1, -1, channels_mode);
					SceAudioOutChannelFlag flags = (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH & SCE_AUDIO_VOLUME_FLAG_R_CH);
					int vol = SCE_AUDIO_VOLUME_0DB;
					int volumes[2] = {vol, vol};
					if (sceAudioOutSetVolume(port, flags, volumes)) {
						printf("Error setting volume\n");
					}
	
					sceKernelDelayThread(500000);
				}

				if (outsize > 0) {
					// Only play music if there is some music data
					sceKernelLockMutex(visualizer_mutex, 1, NULL);
					neon_fft_fill_buffer(player.visualizer_config, (int16_t*)outbuffer, outsize / (2 * channels));
					sceKernelUnlockMutex(visualizer_mutex, 1);
	
					if (port >= 0) {
						sceAudioOutOutput(port, outbuffer);
					}
				} else {
					sceKernelDelayThread(100000); // 100ms delay if nothing to play
				}
			}

			sceKernelDelayThread(1000);
		}

		aac_initialized = false;

		sceKernelDelayThread(100000);
    }

audio_thread_end:
	if (port >= 0) {
		sceAudioOutReleasePort(port);
	}

	MP3_Term();
	if (aac_decoder) {
		NeAACDecClose(aac_decoder);
	}
	return 0;
}

void parse_icy_metadata()
{
    if (!icy_metadata_ready)
        return;

    char *title = strstr(icy_metadata, "StreamTitle='");
    if (title) {
        title += strlen("StreamTitle='");
        char *end = strchr(title, '\'');
        if (end) {
            *end = 0;
            printf("🎵 Now Playing: %s\n", title);
			if (player.song_title) {
				free(player.song_title);
				player.song_title = nullptr;
			}

			int title_size = end - title + 1;
			player.song_title = (char*)malloc(end - title + 1);
			memcpy(player.song_title, title, title_size);
			player.new_song_title = true;
        }
    }

    icy_metadata_ready = 0;
}



int main(void)
{
	vglInitExtended(0, 960, 544, 0x1000000, SCE_GXM_MULTISAMPLE_4X);

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

	SceNetInitParam net_init_param;
	net_init_param.size = 16 * 1024;
	net_init_param.flags = 0;

	SceUID memid = sceKernelAllocMemBlock("SceNetMemory", 0x0C20D060, net_init_param.size, NULL);
	if(memid < 0){
		sceClibPrintf("sceKernelAllocMemBlock failed (0x%X)\n", memid);
		return memid;
	}

	sceKernelGetMemBlockBase(memid, &net_init_param.memory);

	int res;
	res = sceNetInit(&net_init_param);
	if(res < 0){
		sceClibPrintf("sceNetInit failed (0x%X)\n", res);
		return 1;
	}

	res = sceNetCtlInit();
	if(res < 0){
		sceClibPrintf("sceNetCtlInit failed (0x%X)\n", res);
		return 1;
	}

	audio_mutex = sceKernelCreateMutex("audio_mutex", 0, 0, NULL);
	if (audio_mutex < 0) {
		printf("Error creating mutex\n");
		return 1;
	}

	icy_meta_mutex = sceKernelCreateMutex("icy_meta_mutex", 0, 0, NULL);
	if (icy_meta_mutex < 0) {
		printf("Error creating mutex\n");
		return 1;
	}

	visualizer_mutex = sceKernelCreateMutex("visualizerMutex", 0, 0, NULL);
	if (visualizer_mutex < 0) {
		printf("Error creating mutex\n");
		return 1;
	}

	SceCtrlData ctrl_peek, ctrl_press;

	int thid = 0;
	thid = sceKernelCreateThread("httpThread", network_thread, 0x10000100, 0x10000, 0, 0, NULL);
	if (thid < 0) {
		sceClibPrintf("Error creating thread with id %i\n", thid);
		return 1;
	}

	player.view = PLAYER_VIEW_MENU;
	player.http_thread_id = thid;

	thid = sceKernelCreateThread("audioThread", audio_thread, 0x10000100, 0x10000, 0, 0, NULL);
	if (thid < 0) {
		sceClibPrintf("Error creating thread with id %i\n", thid);
		return 1;
	}

	player.player_thread_id = thid;
	player.state = PLAYER_STATE_WAITING;
	player.visualizer_config = nullptr;
	player.visualizer_rebuild = false;
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
	static bool show_visualization = false;
	int title_show_start_time = 0;
	static ImGuiWindowFlags flags = (ImGuiWindowFlags)(ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);
	while (!done) {
		ImGui_ImplVitaGL_NewFrame();

		parse_icy_metadata();

		if (player.visualizer_rebuild) {
			sceKernelLockMutex(visualizer_mutex, 1, NULL);
	
			if (player.visualizer_config) {
				neon_fft_free(player.visualizer_config);
				player.visualizer_config = nullptr;
			}

			player.visualizer_config = neon_fft_init(player.nb_samples, player.samplerate, player.nb_channels, 16);
			sceKernelUnlockMutex(visualizer_mutex, 1);

			player.visualizer_rebuild = false;
		}

		if (player.view == PLAYER_VIEW_MENU || player.view == PLAYER_VIEW_SETTINGS) {
			ImGui::GetIO().MouseDrawCursor = false;
			
    		ImGui::SetNextWindowPos(ImVec2(0, 0));
	    	ImGui::SetNextWindowSize(ImVec2(960, 544));
			
			if (ImGui::Begin("Vita Webradio", &show_main_widget, flags))
			{
				if (ImGui::Button("Webradios", ImVec2(0, 30))) {
					player.view = PLAYER_VIEW_MENU;
				}

				ImGui::SameLine();
				if (ImGui::Button("Bars", ImVec2(0, 30))) {
					player.view = PLAYER_VIEW_VISUALIZER_BARS;
				}

				ImGui::SameLine();
				if (ImGui::Button("Circles", ImVec2(0, 30))) {
					player.view = PLAYER_VIEW_VISUALIZER_CIRCLES;
				}

				ImGui::SameLine();
				if (ImGui::Button("About", ImVec2(0, 30))) {
					player.view = PLAYER_VIEW_SETTINGS;
				}

				ImGui::Separator();

				if (player.view == PLAYER_VIEW_SETTINGS) {
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
							player.view = PLAYER_VIEW_VISUALIZER_BARS;
						}
						drawEntry = drawEntry->next;
					}
				}

				ImGui::End();
			}
		} else if (player.view == PLAYER_VIEW_VISUALIZER_BARS || player.view == PLAYER_VIEW_VISUALIZER_CIRCLES) {
			ImGui::SetNextWindowPos(ImVec2(0, 0));
	    	ImGui::SetNextWindowSize(ImVec2(960, 544));

			if (ImGui::Begin("Vita Webradio Visualizer", &show_visualization, flags)) {
				sceKernelLockMutex(visualizer_mutex, 1, NULL);
				if (player.state == PLAYER_STATE_PLAYING && player.visualizer_config && player.visualizer_config->visualizer_data) {
					spectrum_analyser(player.visualizer_config);

					if (player.view == PLAYER_VIEW_VISUALIZER_BARS) {
						int bar_length = 960 / player.visualizer_config->bar_count;
						for (int i = 0; i < player.visualizer_config->bar_count; i++) {
							float y_upper = 540.0 - (player.visualizer_config->visualizer_data[i] - 60.0) * 5.0f;
							if (y_upper <= 0.0 || y_upper > 544.0) {
								continue;
							}

							ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
								ImVec2((float)(i+1) * bar_length - 1, y_upper),
								ImVec2((float)i * bar_length, 540.0),
								IM_COL32(255 - (int)(y_upper / 3.0), 200, 0, 255),
								IM_COL32(255 - (int)(y_upper / 3.0), 200, 0, 255),
								IM_COL32(0, 200, 0, 255),
								IM_COL32(0, 200, 0, 255));
						}
					} else { // PLAYER_VIEW_VISUALIZER_CIRCLES
						for (int i = 0; i < player.visualizer_config->bar_count / 2; i++) {
							float value = (player.visualizer_config->visualizer_data[i*2] + player.visualizer_config->visualizer_data[i*2+1]) / 2.0;
							if (value < 0.0) {
								value = 0.0;
							}

							ImGui::GetWindowDrawList()->AddCircle(
								ImVec2(960.0 / 2.0, 544.0 / 2.0),
								value * 1.5,
								IM_COL32(255 - i * 2 * 255 / player.visualizer_config->bar_count, 200, 0, 255),
								24,
								4.0
							);
						}
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

		if (ctrl_press.buttons & SCE_CTRL_TRIANGLE) {
			switch (player.view)
			{
			case PLAYER_VIEW_BLACKSCREEN:
				player.view = PLAYER_VIEW_MENU;
				break;
			default:
				player.view = PLAYER_VIEW_BLACKSCREEN;
				break;
			}
		} else if (player.view == PLAYER_VIEW_BLACKSCREEN) {
			// Do nothing, buttons are disabled in this view
		} else if (ctrl_press.buttons & SCE_CTRL_START) {
			done = true;
		} else if (ctrl_press.buttons & SCE_CTRL_CIRCLE) {
			switch (player.view)
			{
			case PLAYER_VIEW_VISUALIZER_CIRCLES:
				player.view = PLAYER_VIEW_MENU;
				break;
			case PLAYER_VIEW_VISUALIZER_BARS:
				player.view = PLAYER_VIEW_VISUALIZER_CIRCLES;
				break;
			default:
				player.view = PLAYER_VIEW_VISUALIZER_BARS;
				break;
			}
			player.new_song_title = true; // Show title again
		} else if (ctrl_press.buttons & SCE_CTRL_SQUARE) {
			player.state = PLAYER_STATE_WAITING;
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

	int ret = 0;
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
	sceKernelDeleteMutex(icy_meta_mutex);
	sceKernelDeleteMutex(visualizer_mutex);

	// Cleanup
	ImGui::DestroyContext();
	glEnd();

	sceNetCtlTerm();
	sceNetTerm();

	sceSysmoduleUnloadModule(SCE_SYSMODULE_HTTPS);
	sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);

	m3u_file_free(m3ufile);

	return 0;
}
