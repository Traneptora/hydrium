#ifndef HYDRIUM_BITWRITER_H_
#define HYDRIUM_BITWRITER_H_

#include <stdint.h>

#include "libhydrium/hydrium.h"

typedef struct HYDBitWriter {
    uint8_t *buffer;
    size_t buffer_pos;
    size_t buffer_len;
    uint64_t cache;
    int cache_bits;
} HYDBitWriter;

HYDStatusCode hyd_write(HYDBitWriter *bw, uint64_t value, int bits);

HYDStatusCode hyd_write_zero_pad(HYDBitWriter *bw);
HYDStatusCode hyd_write_u32(HYDBitWriter *bw, const uint32_t c[4], const uint32_t u[4], uint32_t value);
HYDStatusCode hyd_write_u64(HYDBitWriter *bw, uint64_t value);
HYDStatusCode hyd_write_bool(HYDBitWriter *bw, int flag);

#endif /* HYDRIUM_BITWRITER_H_ */
