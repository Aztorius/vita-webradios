
#include <NE10.h>

struct neon_fft_config {
    ne10_fft_r2c_cfg_float32_t cfg;
    ne10_int16_t *src_buffer;
    ne10_fft_cpx_float32_t *dst_buffer;
    int nbsamples; // number of samples that will be analyzed for graph generation
    int samplerate; // 44100 for example
    int channel_mode; // 1 if mono, 2 if stereo
    int bar_count; // number of bars to generate for visualization
    float *visualizer_data; // bar_count values values
};

neon_fft_config *neon_fft_init(int nbsamples, int samplerate, int channel_mode, int bar_count = 8);
void neon_fft_free(neon_fft_config *cfg);
int spectrum_analyser(neon_fft_config *cfg);
void neon_fft_fill_buffer(neon_fft_config *cfg, int16_t *raw_data, int nbsamples);
