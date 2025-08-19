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
 * @param nbsamples corresponds to the number of samples that will be used by the fft
 * @param samplerate corresponds to the sample rate of audio data
 * @param channel_mode equals 1 if mono or 2 if stereo
 * 
 */
neon_fft_config *neon_fft_init(int nbsamples, int samplerate, int channel_mode)
{
    neon_fft_config *cfg = (neon_fft_config*)malloc(sizeof(neon_fft_config));

    if (!cfg) {
        printf("Error allocating neon_fft_config\n");
        return nullptr;
    }

    cfg->nbsamples = nbsamples;
    cfg->samplerate = samplerate;

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
    cfg->cfg = ne10_fft_alloc_r2c_int16(FFT_BUFFER_LENGTH);

    cfg->src_buffer = (ne10_int16_t*)malloc(sizeof(ne10_int16_t) * FFT_BUFFER_LENGTH);
    if (!cfg->src_buffer) {
        printf("Error allocating ne10 src_buffer\n");
        ne10_fft_destroy_r2c_int16(cfg->cfg);
        free(cfg);
        return nullptr;
    }

    cfg->dst_buffer = (ne10_fft_cpx_int16_t*)malloc(sizeof(ne10_fft_cpx_int16_t) * ((FFT_BUFFER_LENGTH / 2) + 1));
    if (!cfg->dst_buffer) {
        printf("Error allocating ne10 dst_buffer\n");
        free(cfg->src_buffer);
        ne10_fft_destroy_r2c_int16(cfg->cfg);
        free(cfg);
        return nullptr;
    }

    cfg->visualizer_data = (float*)malloc(sizeof(float) * ((FFT_BUFFER_LENGTH / 2) + 1));
    if (!cfg->visualizer_data) {
        printf("Error allocating visualizer_data\n");
        free(cfg->dst_buffer);
        free(cfg->src_buffer);
        ne10_fft_destroy_r2c_int16(cfg->cfg);
        free(cfg);
        return nullptr;
    }

    cfg->saved_buffer = (ne10_int16_t*)malloc(sizeof(ne10_int16_t) * FFT_BUFFER_LENGTH);
    if (!cfg->saved_buffer) {
        printf("Error allocating saved_buffer\n");
        free(cfg->visualizer_data);
        free(cfg->dst_buffer);
        free(cfg->src_buffer);
        ne10_fft_destroy_r2c_int16(cfg->cfg);
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
    ne10_fft_destroy_r2c_int16(cfg->cfg);

    if (cfg->src_buffer) {
        free(cfg->src_buffer);
    }

    if (cfg->dst_buffer) {
        free(cfg->dst_buffer);
    }

    if (cfg->visualizer_data) {
        free(cfg->visualizer_data);
    }

    if (cfg->saved_buffer) {
        free(cfg->saved_buffer);
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

    if (nbsamples > FFT_BUFFER_LENGTH) {
        printf("neon_fft_fill_src_buffer: too much samples %i\n", nbsamples);
        return;
    }
 
    // We copy samples from raw_data into end of src_buffer taking into account channel_mode (mono/stereo)
    memmove(cfg->saved_buffer, cfg->saved_buffer + nbsamples, (FFT_BUFFER_LENGTH - nbsamples) * sizeof(int16_t));
    if (cfg->channel_mode == 1) {
        memcpy(cfg->saved_buffer + FFT_BUFFER_LENGTH - nbsamples, raw_data, nbsamples * sizeof(int16_t));
    } else if (cfg->channel_mode == 2) {
        // Only keep left channel for analysis
        for (int i = 0; i < nbsamples; i++) {
            int16_t data = (int16_t)(raw_data[i * 2]);
            cfg->saved_buffer[FFT_BUFFER_LENGTH - nbsamples + i] = data;
        }
    }
}

/**
 * Apply FFT on the last 1 second of data and create visualization inside visualizer_data
 */
int spectrum_analyser(neon_fft_config *cfg)
{
    // Perform the FFT
    memcpy(cfg->src_buffer, cfg->saved_buffer, FFT_BUFFER_LENGTH * sizeof(int16_t));
    ne10_fft_r2c_1d_int16_neon(cfg->dst_buffer, cfg->src_buffer, cfg->cfg, 0);

    // Display the results
    for (int i = 0; i <= FFT_BUFFER_LENGTH / 2; i++)
    {
        // Compute magnitude by doing (cfg->dst_buffer[i].r)^2 + (cfg->dst_buffer[i].i)^2
        // We devide by 64 to reduce maximum value from 32768 down to 512 (the height of the screen)
        cfg->visualizer_data[i] = sqrtf((float)(cfg->dst_buffer[i].r) * cfg->dst_buffer[i].r + (float)(cfg->dst_buffer[i].i) * cfg->dst_buffer[i].i) / 64.0;
    }

    return 0;
}
