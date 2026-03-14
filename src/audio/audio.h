#ifndef _ELEVENMPV_AUDIO_H_
#define _ELEVENMPV_AUDIO_H_

#include <psp2/types.h>
#include <psp2/kernel/clib.h>

#define printf sceClibPrintf

enum audio_format {
	AUDIO_FORMAT_UNKNOWN,
	AUDIO_FORMAT_MP3,
	AUDIO_FORMAT_AAC,
	AUDIO_FORMAT_OGG,
};

static int audio_port_number = -1;

int AudioInitOutput(int samplerate, int nb_channels, int nb_samples);
int AudioChangeOutputConfig(int samplerate, int nb_channels, int nb_samples);
int AudioFreeOutput();
int AudioOutOutput(const void *buff);
int AudioSetVolumeOutput(int volume);

char *AudioFormatToString(enum audio_format format);

#endif
