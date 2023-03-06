/*
 * Bit Writer implementation
 */
#include "libhydrium/hydrium.h"
#include "bitwriter.h"

HYDStatusCode hyd_write(HYDBitWriter *bw, uint64_t value, int bits) {
    if (bits <= 0)
        return HYD_OK;
    if (bits > 56)
        return HYD_API_ERROR;
    if (bits <= 64 - bw->cache_bits) {
        bw->cache |= (value & ~(~UINT64_C(0) << bits)) << bw->cache_bits;
        bw->cache_bits += bits;
        return bw->buffer_pos > bw->buffer_len ? HYD_NEED_MORE_OUTPUT : HYD_OK;
    }
    while (bw->cache_bits >= 8) {
        bw->buffer[bw->buffer_pos++] = bw->cache & 0xFF;
        bw->cache >>= 8;
        bw->cache_bits -= 8;
    }
    return hyd_write(bw, value, bits);
}

HYDStatusCode hyd_write_zero_pad(HYDBitWriter *bw) {
    bw->cache_bits += 7 - (bw->cache_bits + 7) % 8;
    return HYD_OK;
}
