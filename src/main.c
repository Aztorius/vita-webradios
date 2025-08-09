#include <stdint.h>
#include <string.h>
#include <math.h>

#include <psp2/ctrl.h>
#include <psp2/audiodec.h>
#include <psp2/audioout.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/libssl.h>
#include <psp2/net/http.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/paf.h>
#include <psp2/sysmodule.h>

#include "debugScreen.h"
#include "mp3.h"

// #define printf psvDebugScreenPrintf
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
	char *url;
};

static struct player player;
static int audio_mutex;

int play_webradio(char *url) {

	int res, tpl, conn, req;
	SceUInt64 length = 0;

	SceUID fd;
	void *recv_buffer = NULL;

	SceNetInitParam net_init_param;
	net_init_param.size = 0x800000;
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

	res = sceHttpInit(0x800000);
	if(res < 0){
		sceClibPrintf("sceHttpInit failed (0x%X)\n", res);
		goto netctl_term;
	}

	res = sceSslInit(0x800000);
	if(res < 0){
		sceClibPrintf("sceSslInit failed (0x%X)\n", res);
		goto http_term;
	}

	tpl = sceHttpCreateTemplate("PSP2 GITHUB", 2, 1);
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
		unsigned int timeout = 10000000;

		while (player.state == PLAYER_STATE_PLAYING) {
			res = sceHttpReadData(req, recv_buffer, 0x10000);
			if (res <= 0) {
				break;
			}
			sceClibPrintf("sceHttpReadData: %i\n", res);

			sceKernelLockMutex(audio_mutex, 1, &timeout);
			ret = MP3_Feed(recv_buffer, res);
			sceKernelUnlockMutex(audio_mutex, 1);
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

int audio_thread() {
	int port = -1;

	int ret = 0;

	unsigned char outbuffer[BUFFER_LENGTH] = {0};
	int outsize = 0;
	unsigned int timeout = 10000000;

	while (player.state != PLAYER_STATE_STOPPING) {
		sceKernelLockMutex(audio_mutex, 1, &timeout);

		ret = MP3_Decode(NULL, 0, outbuffer, BUFFER_LENGTH, &outsize);
		if (ret == -11) {
			// New format, close old output if necessary
			if (port >= 0) {
				sceAudioOutReleasePort(port);
			}
	
			// int compatible_freqs[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
			int vol = SCE_AUDIO_VOLUME_0DB;
			int channels = MP3_GetChannels();
			int channels_mode = 0;
			if (channels == 1) {
				channels_mode = SCE_AUDIO_OUT_MODE_MONO;
			} else if (channels == 2) {
				channels_mode = SCE_AUDIO_OUT_MODE_STEREO;
			} else {
				printf("Wrong number of channel in stream !");
				goto audio_thread_end;
			}
	
			port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, NSAMPLES, MP3_GetSampleRate(), channels_mode);
			psvDebugScreenPrintf("Playing %s sample_rate %i channels %i\n", player.url, MP3_GetSampleRate(), channels);
			sceAudioOutSetConfig(port, -1, -1, -1);
			sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH |SCE_AUDIO_VOLUME_FLAG_R_CH, (int[]){vol,vol});
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
}

int http_thread() {
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

int main(void) {
	psvDebugScreenInit();

	int res;
	SceUInt32 paf_init_param[6];
	SceSysmoduleOpt sysmodule_opt;

	paf_init_param[0] = 0x4000000;
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

	audio_mutex = sceKernelCreateMutex("audioMutex", 0, 1, NULL);
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

	do {
		ctrl_press = ctrl_peek;
		sceCtrlPeekBufferPositive(0, &ctrl_peek, 1);
		ctrl_press.buttons = ctrl_peek.buttons & ~ctrl_press.buttons;

		if (ctrl_press.buttons & SCE_CTRL_CROSS) {
			if (player.state == PLAYER_STATE_PLAYING) {
				// We are already playing something
				continue;
			}
			player.url = "http://novazz.ice.infomaniak.ch/novazz-128.mp3";
			player.state = PLAYER_STATE_PLAYING;
		} else if (ctrl_press.buttons & SCE_CTRL_CIRCLE) {
			player.state = PLAYER_STATE_WAITING;
		}

		sceKernelDelayThread(100000); // Delay for 100 ms
	} while (ctrl_press.buttons != SCE_CTRL_START);

	player.state = PLAYER_STATE_STOPPING;

	int exitstatus = 0, timeout = 2000000;

	ret = sceKernelWaitThreadEnd(player.player_thread_id, &exitstatus, &timeout);
	if (ret < 0 || exitstatus != 0)
    {
        sceClibPrintf("Error on thread exit. Exit status %i, return code %i\n", exitstatus, ret);
    }

	ret = sceKernelWaitThreadEnd(player.http_thread_id, &exitstatus, &timeout);
    if (ret < 0 || exitstatus != 0)
    {
        sceClibPrintf("Error on thread exit. Exit status %i, return code %i\n", exitstatus, ret);
    }

	sceKernelDeleteThread(player.player_thread_id);
    sceKernelDeleteThread(player.http_thread_id);

	sceKernelDeleteMutex(audio_mutex);

	sceSysmoduleUnloadModule(SCE_SYSMODULE_HTTPS);
	sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PAF);

	return 0;
}
