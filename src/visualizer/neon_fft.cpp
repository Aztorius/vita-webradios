#include <stdio.h>
#include <stdlib.h>
#include <arm_neon.h>
#include <math.h>
#include <cstring>

#include <psp2/kernel/clib.h>

#include "neon_fft.hpp"

#define printf sceClibPrintf
#define M_PI		3.14159265358979323846

/**
 * Init fft structures
 * 
 * @param nbsamples corresponds to the number of samples that will be analyzed by the fft
 * @param samplerate corresponds to the sample rate of audio data
 * @param channel_mode equals 1 if mono or 2 if stereo
 * 
 */
neon_fft_config *neon_fft_init(int nbsamples, int samplerate, int channel_mode, int bar_count)
{
    neon_fft_config *cfg = (neon_fft_config*)malloc(sizeof(neon_fft_config));

    if (!cfg) {
        printf("Error allocating neon_fft_config\n");
        return nullptr;
    }

    cfg->nbsamples = nbsamples;
    cfg->samplerate = samplerate;
    cfg->bar_count = bar_count;

    if (channel_mode != 1 && channel_mode != 2) {
        printf("Channel mode unsupported\n");
        return nullptr;
    }

    cfg->channel_mode = channel_mode;

    // Initialise Ne10, using hardware auto-detection to set library function pointers
    if (ne10_init() != NE10_OK)
    {
        printf("Failed to initialise Ne10.\n");
        free(cfg);
        return nullptr;
    }    

    // Prepare the real-to-complex single precision floating point FFT configuration
    // structure for inputs of length `nbsamples`. (You need only generate this once for a
    // particular input size.)
    cfg->cfg = ne10_fft_alloc_r2c_float32(nbsamples);

    cfg->src_buffer = (ne10_int16_t*)malloc(sizeof(ne10_int16_t) * nbsamples);
    if (!cfg->src_buffer) {
        printf("Error allocating ne10 src_buffer\n");
        ne10_fft_destroy_r2c_float32(cfg->cfg);
        free(cfg);
        return nullptr;
    }

    cfg->dst_buffer = (ne10_fft_cpx_float32_t*)malloc(sizeof(ne10_fft_cpx_float32_t) * ((nbsamples / 2) + 1));
    if (!cfg->dst_buffer) {
        printf("Error allocating ne10 dst_buffer\n");
        free(cfg->src_buffer);
        ne10_fft_destroy_r2c_float32(cfg->cfg);
        free(cfg);
        return nullptr;
    }

    cfg->visualizer_data = (float*)malloc(sizeof(float) * cfg->bar_count);
    if (!cfg->visualizer_data) {
        printf("Error allocating visualizer_data\n");
        free(cfg->dst_buffer);
        free(cfg->src_buffer);
        ne10_fft_destroy_r2c_float32(cfg->cfg);
        free(cfg);
        return nullptr;
    }

    return cfg;
}

void neon_fft_free(neon_fft_config *cfg)
{
    if (!cfg) {
        return;
    }

    // Free the allocated configuration structure
    ne10_fft_destroy_r2c_float32(cfg->cfg);

    if (cfg->src_buffer) {
        free(cfg->src_buffer);
    }

    if (cfg->dst_buffer) {
        free(cfg->dst_buffer);
    }

    if (cfg->visualizer_data) {
        free(cfg->visualizer_data);
    }

    free(cfg);
}

/**
 * Add samples to the end of src_buffer
 * 
 * @param raw_data is PCM 16bit little-endian (2 bytes / sample in mono, 4 bytes / sample in stereo) and must be of size nbsamples
 */
void neon_fft_fill_buffer(neon_fft_config *cfg, int16_t *raw_data, int nbsamples)
{
    if (nbsamples == 0) {
        printf("neon_fft_fill_src_buffer: nbsamples equals to zero\n");
        return;
    }

    if (nbsamples > cfg->nbsamples) {
        printf("neon_fft_fill_src_buffer: too much samples %i\n", nbsamples);
        return;
    }
 
    if (cfg->channel_mode == 1) {
        memcpy(cfg->src_buffer, raw_data, nbsamples * sizeof(int16_t));
    } else if (cfg->channel_mode == 2) {
        // Only keep left channel for analysis
        for (int i = 0; i < nbsamples; i++) {
            cfg->src_buffer[i] = raw_data[i * 2];
        }
    }
}

// Convert a frequency into an FFT indices
static inline int f_to_bin(neon_fft_config *cfg, float f) {
    int idx = (int)roundf(f * cfg->nbsamples / cfg->samplerate);
    if (idx < 0) idx = 0;
    if (idx > cfg->nbsamples/2) idx = cfg->nbsamples/2;
    return idx;
}

/**
 * Apply FFT on the last 1 second of data and create visualization inside visualizer_data
 */
int spectrum_analyser(neon_fft_config *cfg)
{
    // Apply Hann window on src_buffer
    float *buffer = (float*)malloc(sizeof(float) * cfg->nbsamples);
    if (!buffer) {
        printf("spectrum_analyser: error allocating buffer");
        return -1;
    }

    for (int i = 0; i < cfg->nbsamples; i++) {
        float hann_value = 0.5f * (1 - cosf(2*M_PI*i/(cfg->nbsamples - 1)));
        buffer[i] = hann_value * (float)cfg->src_buffer[i];
    }

    // Perform the FFT
    ne10_fft_r2c_1d_float32_neon(cfg->dst_buffer, buffer, cfg->cfg);

    // Compute power
    float *power = (float*)malloc(sizeof(float) * (cfg->nbsamples / 2 + 1));
    if (!power) {
        printf("spectrum_analyser: error allocating power");
        return -1;
    }

    for (int i = 0; i <= cfg->nbsamples / 2; i++)
    {
        // Compute magnitude by doing (cfg->dst_buffer[i].r)^2 + (cfg->dst_buffer[i].i)^2
        // We devide by 64 to reduce maximum value from 32768 down to 512 (the height of the screen)
        float re = cfg->dst_buffer[i].r;
        float im = cfg->dst_buffer[i].i;
        power[i] = re*re + im*im;
    }

    // Create groups for visualizer
    int nbands = cfg->bar_count;
    float *edges = (float*)malloc(sizeof(float) * (nbands+1));
    if (!edges) {
        printf("spectrum_analyser: error allocating edges");
        return -1;
    }

    float fmin = 20.0f;
    float fmax = cfg->samplerate / 2.0f;
    float log_min = log10f(fmin);
    float log_max = log10f(fmax);
    for (int i = 0; i <= nbands; i++) {
        float frac = (float)i / (float)nbands;
        edges[i] = powf(10.0f, log_min + frac * (log_max - log_min));
    }

    float max_db = 0;
    for (int b = 0; b < nbands; b++) {
        // Compute lower and upper FFT indices of current band
        int i_lo = f_to_bin(cfg, edges[b]);
        int i_hi = f_to_bin(cfg, edges[b+1]);
        cfg->visualizer_data[b] = 0;

        if (i_hi <= i_lo)
            i_hi = i_lo + 1;

        for (int k = i_lo; k < i_hi; k++) {
            cfg->visualizer_data[b] += power[k];
        }

        // Convert to dB
        cfg->visualizer_data[b] = 10.0f * log10f(cfg->visualizer_data[b] + 1e-12f);

        if (cfg->visualizer_data[b] > max_db)
            max_db = cfg->visualizer_data[b];
    }

    // Normalize compared to max band value
    for (int b = 0; b < nbands; b++) {
        cfg->visualizer_data[b] -= max_db;
    }

    free(edges);
    free(power);
    free(buffer);

    return 0;
}
