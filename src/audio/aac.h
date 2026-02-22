#include <neaacdec.h>
#include <stdint.h>
#include <stddef.h>
#include <psp2/kernel/clib.h>

#define printf sceClibPrintf

typedef struct {
    int sample_rate;
    int channels;
    int frame_length;
    int header_size;
} adts_header_t;

int parse_adts_header(const uint8_t *data, size_t size, adts_header_t *out);

int AAC_Init(unsigned char *init_buffer, unsigned long init_buffer_size, int *nb_channels, int *samplerate);
int AAC_Free();
int AAC_Decode(unsigned char *buffer, unsigned long buffer_size, unsigned long *samples, void **output_buffer);
