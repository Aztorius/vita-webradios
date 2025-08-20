
#include <NE10.h>

#define FFT_BUFFER_LENGTH    2048

struct neon_fft_config {
    ne10_fft_r2c_cfg_float32_t cfg;
    ne10_int16_t *src_buffer;
    ne10_fft_cpx_float32_t *dst_buffer;
    int nbsamples;
    int samplerate;
    int channel_mode;
    float *visualizer_data; // (nbsamples / 2) + 1 values
};

neon_fft_config *neon_fft_init(int nbsamples, int samplerate, int channel_mode);
void neon_fft_free(neon_fft_config *cfg);
int spectrum_analyser(neon_fft_config *cfg);
void neon_fft_fill_buffer(neon_fft_config *cfg, int16_t *raw_data, int nbsamples);
