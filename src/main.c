#include <stdint.h>
#include <math.h>

#include <psp2/ctrl.h>
#include <psp2/audiodec.h>
#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>

#include "debugScreen.h"

// #define countof(A) sizeof(A)/sizeof(*A)
// #define MIN(A,B) ((A)<(B)?(A):(B))
// #define MAX(A,B) ((A)>(B)?(A):(B))

// #define printf psvDebugScreenPrintf

int printf(const char* str, ...) {
    psvDebugScreenPrintf(str);
    sceClibPrintf(str);
}

typedef double (*wav_gen)(double);

// void wave_set(int16_t*buffer, size_t size,  wav_gen generator){
// 	for (size_t smpl = 0; smpl < size; ++smpl)
// 		buffer[smpl] = 0x7FFF*generator((float)smpl/(float)size);
// }

int main(void) {
	psvDebugScreenInit();

	int freqs[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
	int size = 256;
	int freq = 8;
	int mode = SCE_AUDIO_OUT_MODE_MONO;
	int vol = SCE_AUDIO_VOLUME_0DB;

	int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, size, freqs[freq], mode);
	sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH |SCE_AUDIO_VOLUME_FLAG_R_CH, (int[]){vol,vol});

    int handle = fopen("test.mp3");
    if (!handle) {
        printf("ERROR: test.mp3 not found !");
        sceAudioOutReleasePort(port);
        return 0;
    }
    sceAudiodecInitParam param;

    if (sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_MP3, &param) < 0) {
        printf("ERROR: sceAudiodecInitLibrary");
        sceAudioOutReleasePort(port);
        return 0;
    }
    sceAudiodecCtrl audioCtrl;
    audioCtrl.handle = handle;
    if (sceAudiodecCreateDecoder(&audioCtrl, SCE_AUDIODEC_TYPE_MP3) < 0) {
        printf("ERROR: sceAudiodecCreateDecoder");
        sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_MP3);
        sceAudioOutReleasePort(port);
        return 0;
    }

	int16_t wave_buf[SCE_AUDIO_MAX_LEN]={0};
	wav_gen gen=gen_nul;
	SceCtrlData ctrl_peek, ctrl_press;
	do{
		// ctrl_press = ctrl_peek;
		// sceCtrlPeekBufferPositive(0, &ctrl_peek, 1);
		// ctrl_press.buttons = ctrl_peek.buttons & ~ctrl_press.buttons;

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


		sceAudioOutOutput(port, wave_buf);
        printf("Hello world ! Listening...");
	}while(ctrl_press.buttons != SCE_CTRL_START);

    sceAudiodecDeleteDecoder(&audioCtrl);
    sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_MP3);

    fclose(handle);

	sceAudioOutReleasePort(port);
	return 0;
}
