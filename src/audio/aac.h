#include <neaacdec.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int sample_rate;
    int channels;
    int frame_length;
    int header_size;
} adts_header_t;

int parse_adts_header(const uint8_t *data, size_t size, adts_header_t *out);
