#include <stdio.h>
#include <stdlib.h>
#include <arm_neon.h>

#include <psp2/kernel/clib.h>

#include "neon_fft.hpp"
#include <cstring>

#define printf sceClibPrintf


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
    cfg->cfg = ne10_fft_alloc_r2c_int16(nbsamples);

    cfg->src_buffer = (ne10_int16_t*)malloc(sizeof(ne10_int16_t) * nbsamples);
    if (!cfg->src_buffer) {
        printf("Error allocating ne10 src_buffer\n");
        ne10_fft_destroy_r2c_int16(cfg->cfg);
        free(cfg);
        return nullptr;
    }

    cfg->dst_buffer = (ne10_fft_cpx_int16_t*)malloc(sizeof(ne10_fft_cpx_int16_t) * ((nbsamples / 2) + 1));
    if (!cfg->dst_buffer) {
        printf("Error allocating ne10 dst_buffer\n");
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

    free(cfg);
}

int neon_fft_fill_src_buffer(neon_fft_config *cfg, int16_t *raw_data, int nbsamples)
{
    // raw_data is PCM 16bit little-endian (2 bytes / sample in mono, 4 bytes / sample in stereo) and must be of size nbsamples
    // returns 1 if a new spectrum has been computed

    // We need to fill the src_buffer until we have a full second of data, then we can compute spectrum data on it

    int new_spectrum = 0;
    if (nbsamples == cfg->nbsamples) {
        if (cfg->channel_mode == 1) { // mono, we can copy data directly to the buffer
            memcpy(cfg->src_buffer, raw_data, nbsamples);
        } else if (cfg->channel_mode == 2) { // stereo, we need to get only one sample every two because of left/right channels (we will only use left channel)
            for (int i = 0; i < nbsamples / 8; i++) {
                int16x8_t left_and_right = vld1q_s16(raw_data + i*8);
                int16x4_t left_only = vget_low_s16(left_and_right);
                vst1_s16(cfg->src_buffer + i*4, left_only);
            }
        }
        spectrum_analyser(cfg);
        new_spectrum = 1;
    }

    return new_spectrum;
}

int spectrum_analyser(neon_fft_config *cfg)
{
    // Perform the FFT
    ne10_fft_r2c_1d_int16(cfg->dst_buffer, cfg->src_buffer, cfg->cfg, 0);

    // We have results of FFT inside dst_buffer and we will compute and store magnitude
    // values inside this same buffer (just half less data)
    const int16_t *dst_buffer_ptr = (const int16_t*)cfg->dst_buffer;
    int16_t *result_ptr = (int16_t*)cfg->dst_buffer;

    // Display the results
    for (int i = 0; i <= cfg->nbsamples / (2 * 8); i++)
    {
        // Compute magnitude by doing (cfg->dst_buffer[i].r)^2 + (cfg->dst_buffer[i].i)^2
        // real and imaginary elements are together in memory, we need to load everything and separate those values
        // then we multiply with saturation and add those two numbers
        int16x8_t fft_result = vld1q_s16(dst_buffer_ptr);
        int16x4_t real_numbers = vget_low_s16(fft_result);
        int16x4_t imaginary_numbers = vget_high_s16(fft_result);
        real_numbers = vqdmulh_s16(real_numbers, real_numbers);
        imaginary_numbers = vqdmulh_s16(imaginary_numbers, imaginary_numbers);
        int16x4_t result = vhadd_s16(real_numbers, imaginary_numbers);
        vst1_s16(result_ptr, result);

        dst_buffer_ptr += 8;
        result_ptr += 4;
    }

    return 0;
}
