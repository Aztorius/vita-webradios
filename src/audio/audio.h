#ifndef _ELEVENMPV_AUDIO_H_
#define _ELEVENMPV_AUDIO_H_

#include <psp2/types.h>
#include <psp2/kernel/clib.h>

#define printf sceClibPrintf

static int audio_port_number = -1;

int AudioInitOutput(int samplerate, int nb_channels, int nb_samples);
int AudioFreeOutput();
int AudioOutOutput(const void *buff);

#endif
