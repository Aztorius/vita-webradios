#include "aac.h"

static const int adts_sample_rates[] = {
    96000, 88200, 64000, 48000, 44100, 32000,
    24000, 22050, 16000, 12000, 11025, 8000, 7350
};

static NeAACDecHandle aac_decoder = NULL;

int parse_adts_header(const uint8_t *data, size_t size,
                      adts_header_t *out)
{
    if (size < 7)
        return -1;

    if (data[0] != 0xFF || (data[1] & 0xF0) != 0xF0)
        return -1;

    int protection_absent = data[1] & 0x01;
    int sf_index = (data[2] >> 2) & 0x0F;

    if (sf_index > 12)
        return -1;

    out->sample_rate = adts_sample_rates[sf_index];

    out->channels =
        ((data[2] & 0x01) << 2) |
        ((data[3] >> 6) & 0x03);

    out->frame_length =
        ((data[3] & 0x03) << 11) |
        (data[4] << 3) |
        ((data[5] >> 5) & 0x07);

    out->header_size = protection_absent ? 7 : 9;

    return 0;
}

int AAC_Init(unsigned char *init_buffer, unsigned long init_buffer_size, int *nb_channels, int *samplerate)
{
    AAC_Free();

    // Init faad2 for AAC
    aac_decoder = NeAACDecOpen();
    NeAACDecConfigurationPtr aac_cfg = NeAACDecGetCurrentConfiguration(aac_decoder);
    aac_cfg->outputFormat = FAAD_FMT_16BIT;
    aac_cfg->downMatrix = 1; // A 5.1 channels should be downmatrixed to 2.0 channels for Vita
    if (!NeAACDecSetConfiguration(aac_decoder, aac_cfg)) {
        printf("Error with NeAACDecSetConfiguration\n");
        return 1;
    }

    unsigned long raw_samplerate;
    unsigned char raw_channels;
    long ret = NeAACDecInit(aac_decoder, init_buffer, init_buffer_size, &raw_samplerate, &raw_channels);

    if (ret < 0) {
        printf("Error NeAACDecInit\n");
        NeAACDecClose(aac_decoder);
        aac_decoder = NULL;
        return 1;
    }

    if (samplerate) {
        *samplerate = raw_samplerate;
    }

    if (nb_channels) {
        *nb_channels = raw_channels;
    }

    printf("AAC initialized: samplerate=%i,channels=%i\n", raw_samplerate, raw_channels);

    return 0;
}

int AAC_Free()
{
    if (aac_decoder) {
        NeAACDecClose(aac_decoder);
        aac_decoder = NULL;
    }

    return 0;
}

int AAC_Decode(unsigned char *buffer, unsigned long buffer_size, unsigned long *samples, void **output_buffer)
{
    if (!aac_decoder) {
        printf("Cannot decode on unitialized AAC\n");
        return 1;
    }

    NeAACDecFrameInfo aac_frame_info;
	void *pcm = NeAACDecDecode(aac_decoder, &aac_frame_info, buffer, buffer_size);

    if (aac_frame_info.error) {
        return 1;
    }

    if (samples) {
        *samples = aac_frame_info.samples;
    }

    if (output_buffer) {
        *output_buffer = pcm;
    }

    return 0;
}
