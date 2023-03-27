#ifndef HYDRIUM_BITWRITER_H_
#define HYDRIUM_BITWRITER_H_

#include <stddef.h>
#include <stdint.h>

#include "libhydrium.h"

typedef struct HYDBitWriter {
    uint8_t *buffer;
    size_t buffer_pos;
    size_t buffer_len;
    uint64_t cache;
    int cache_bits;
    uint8_t overflow[32];
    size_t overflow_pos;
    int overflow_state;
} HYDBitWriter;

HYDStatusCode hyd_init_bit_writer(HYDBitWriter *bw, uint8_t *buffer, size_t buffer_len, uint64_t cache, int cache_bits);
HYDStatusCode hyd_write(HYDBitWriter *bw, uint64_t value, int bits);

HYDStatusCode hyd_write_zero_pad(HYDBitWriter *bw);
HYDStatusCode hyd_write_u32(HYDBitWriter *bw, const uint32_t c[4], const uint32_t u[4], uint32_t value);
HYDStatusCode hyd_write_u64(HYDBitWriter *bw, uint64_t value);
HYDStatusCode hyd_write_bool(HYDBitWriter *bw, int flag);
HYDStatusCode hyd_bitwriter_flush(HYDBitWriter *bw);

#endif /* HYDRIUM_BITWRITER_H_ */
