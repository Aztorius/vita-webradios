#include "aac.h"

static const int adts_sample_rates[] = {
    96000, 88200, 64000, 48000, 44100, 32000,
    24000, 22050, 16000, 12000, 11025, 8000, 7350
};

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
