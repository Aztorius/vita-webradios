#include <stdint.h>
#include <math.h>

#include <psp2/ctrl.h>
#include <psp2/audiodec.h>
#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>

#include "debugScreen.h"
#include "mp3.h"

// #define printf psvDebugScreenPrintf
#define printf sceClibPrintf

#define BUFFER_LENGTH 8192
#define NSAMPLES 2048

int main(void) {
	psvDebugScreenInit();

	int freqs[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
	int size = NSAMPLES;
	int freq = 7;
	int mode = SCE_AUDIO_OUT_MODE_STEREO; // SCE_AUDIO_OUT_MODE_MONO;
	int vol = SCE_AUDIO_VOLUME_0DB;

	int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, size, freqs[freq], mode);
	sceAudioOutSetConfig(port, -1, -1, -1);
	sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH |SCE_AUDIO_VOLUME_FLAG_R_CH, (int[]){vol,vol});

	int ret = MP3_Init("test.mp3");
	if (ret) {
		printf("MP3_Init %i\n", ret);
		sceAudioOutReleasePort(port);
		return 0;
	}

	printf("samplerate %u\n", MP3_GetSampleRate());
	printf("channels %u\n", MP3_GetChannels());
	MP3_Seek(0);

	unsigned char buffer[BUFFER_LENGTH] = {0};
	SceCtrlData ctrl_peek, ctrl_press;

	do{
		ctrl_press = ctrl_peek;
		sceCtrlPeekBufferPositive(0, &ctrl_peek, 1);
		ctrl_press.buttons = ctrl_peek.buttons & ~ctrl_press.buttons;

		// if(ctrl_press.buttons == SCE_CTRL_CIRCLE)
		// 	gen=gen_sin;
		// if(ctrl_press.buttons == SCE_CTRL_SQUARE)
		// 	gen=gen_sqr;
		// if(ctrl_press.buttons == SCE_CTRL_TRIANGLE)
		// 	gen=gen_tri;
		// if(ctrl_press.buttons == SCE_CTRL_CROSS)
		// 	gen=gen_nul;
		// if(ctrl_press.buttons & (SCE_CTRL_CROSS|SCE_CTRL_TRIANGLE|SCE_CTRL_SQUARE|SCE_CTRL_CIRCLE))
		// 	wave_set(wave_buf,size,gen);

		// if(ctrl_press.buttons == SCE_CTRL_RIGHT)
		// 	freq = MIN(countof(freqs)-1, freq+1);
		// if(ctrl_press.buttons == SCE_CTRL_LEFT)
		// 	freq = MAX(0, freq-1);
		// if(ctrl_press.buttons == SCE_CTRL_RTRIGGER)
		// 	size = MIN(SCE_AUDIO_MAX_LEN,size+1000);
		// if(ctrl_press.buttons == SCE_CTRL_LTRIGGER)
		// 	size = MAX(SCE_AUDIO_MIN_LEN,size-1000);
		// if(ctrl_press.buttons & (SCE_CTRL_RIGHT|SCE_CTRL_LEFT|SCE_CTRL_LTRIGGER|SCE_CTRL_RTRIGGER)){
		// 	sceAudioOutSetConfig(port, size, freqs[freq], mode);
		// 	wave_set(wave_buf,size,gen);
		// }

		// if(ctrl_press.buttons == SCE_CTRL_UP)
		// 	vol = MIN(vol+1024,SCE_AUDIO_VOLUME_0DB);
		// if(ctrl_press.buttons == SCE_CTRL_DOWN)
		// 	vol = MAX(vol-1024,0);
		// if(ctrl_press.buttons & (SCE_CTRL_UP|SCE_CTRL_DOWN))
		// 	sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH |SCE_AUDIO_VOLUME_FLAG_R_CH, (int[]){vol,vol});

		ret = MP3_Decode(buffer, BUFFER_LENGTH);
		if (ret) {
			printf("MP3_Decode %i", ret);
			break;
		}

		sceAudioOutOutput(port, buffer);
        // printf("Hello world ! Listening...\n");
	}while(MP3_playing() && ctrl_press.buttons != SCE_CTRL_START);

	MP3_Term();
	sceAudioOutReleasePort(port);
	return 0;
}
