#include "audio.h"

#include <psp2/audioout.h>

int AudioIsSamplerateVitaCompatible(int samplerate)
{
	int compatible_freqs[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
	int samplerate_is_compatible = 0;
	int nsamples = 0;

	for (int i = 0; i < sizeof(compatible_freqs); i++) {
		if (samplerate == compatible_freqs[i]) {
			samplerate_is_compatible = 1;
			break;
		}
	}

	return samplerate_is_compatible;
}

int AudioInitOutput(int samplerate, int nb_channels, int nb_samples)
{
    if (!AudioIsSamplerateVitaCompatible(samplerate)) {
        printf("Samplerate %i is not compatible\n", samplerate);
        return 1;
    }

    int channels_mode = nb_channels >= 2 ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO;

    audio_port_number = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, nb_samples, samplerate, channels_mode);
    if (audio_port_number < 0) {
        printf("Error while opening port\n");
        return 1;
    }

    AudioSetVolumeOutput(SCE_AUDIO_VOLUME_0DB);

    return 0;
}

int AudioSetVolumeOutput(int volume)
{
    if (volume > SCE_AUDIO_VOLUME_0DB) {
        volume = SCE_AUDIO_VOLUME_0DB;
    }

    if (audio_port_number < 0) {
        printf("Cannot set volume without open port\n");
        return 1;
    }

    SceAudioOutChannelFlag flags = (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH & SCE_AUDIO_VOLUME_FLAG_R_CH);
    int volumes[2] = {volume, volume};
    if (sceAudioOutSetVolume(audio_port_number, flags, volumes)) {
        printf("Error setting volume\n");
        return 1;
    }

    return 0;
}

int AudioChangeOutputConfig(int samplerate, int nb_channels, int nb_samples)
{
    int channels_mode = nb_channels >= 2 ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO;
    if (!sceAudioOutSetConfig(audio_port_number, nb_samples, samplerate, channels_mode)) {
        printf("Error changing audio output config\n");
    }

    AudioSetVolumeOutput(SCE_AUDIO_VOLUME_0DB);

    return 0;
}

int AudioFreeOutput()
{
    if (audio_port_number >= 0) {
        sceAudioOutReleasePort(audio_port_number);
        audio_port_number = -1;
        printf("Audio port closed\n");
    }

    return 0;
}

int AudioOutOutput(const void *buff)
{
    if (audio_port_number >= 0) {
        sceAudioOutOutput(audio_port_number, buff);
    }

    return 0;
}
