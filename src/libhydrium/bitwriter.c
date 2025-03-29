/*
 * Bit Writer implementation
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bitwriter.h"
#include "internal.h"

HYDStatusCode hyd_init_bit_writer(HYDBitWriter *bw, uint8_t *buffer, size_t buffer_len,
                                  uint64_t cache, int cache_bits) {
    bw->buffer = buffer;
    bw->buffer_len = buffer_len;
    bw->buffer_pos = 0;
    bw->cache = cache;
    bw->cache_bits = cache_bits;
    bw->overflow_state = HYD_OK;
    memset(bw->overflow, 0, sizeof(bw->overflow));
    bw->overflow_pos = 0;
    return HYD_OK;
}

static HYDStatusCode drain_cache(HYDBitWriter *bw) {
    while (bw->cache_bits >= 8) {
        uint8_t *buf = bw->buffer_pos >= bw->buffer_len ?
                       bw->overflow + bw->overflow_pos++ :
                       bw->buffer + bw->buffer_pos++;
        *buf = bw->cache & 0xFF;
        bw->cache >>= 8;
        bw->cache_bits -= 8;
    }

    if (bw->overflow_pos) {
        if (bw->overflow_pos > sizeof(bw->overflow))
            return HYD_INTERNAL_ERROR;
        if (bw->realloc_func) {
            bw->overflow_state = bw->realloc_func(&bw->buffer, &bw->buffer_len);
            if (bw->overflow_state < HYD_ERROR_START)
                return bw->overflow_state;
            memcpy(bw->buffer + bw->buffer_pos, bw->overflow, bw->overflow_pos);
            bw->buffer_pos += bw->overflow_pos;
            bw->overflow_pos = 0;
        } else {
            bw->overflow_state = HYD_NEED_MORE_OUTPUT;
        }
    }

    return bw->overflow_state;
}

HYDStatusCode hyd_write(HYDBitWriter *bw, uint64_t value, int bits) {
    if (bits <= 0)
        return bw->overflow_state;
    if (bits > 56)
        return HYD_API_ERROR;
    if (bits <= 64 - bw->cache_bits) {
        bw->cache |= (value & ~(~UINT64_C(0) << bits)) << bw->cache_bits;
        bw->cache_bits += bits;
        return bw->overflow_state;
    }
    HYDStatusCode ret = drain_cache(bw);
    if (ret < HYD_ERROR_START)
        return ret;
    return hyd_write(bw, value, bits);
}

HYDStatusCode hyd_write_zero_pad(HYDBitWriter *bw) {
    return hyd_write(bw, 0, 7 - ((bw->cache_bits + 7) & 0x7));
}

HYDStatusCode hyd_write_bool(HYDBitWriter *bw, int flag) {
    return hyd_write(bw, !!flag, 1);
}

HYDStatusCode hyd_write_u32(HYDBitWriter *bw, const U32Table *table, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        const uint64_t max = ~(~UINT64_C(0) << table->upos[i]);
        const uint64_t vmc = value - table->cpos[i];
        if (vmc <= max)
            return hyd_write(bw, (vmc << 2) | i, table->upos[i] + 2);
    }
    return HYD_API_ERROR;
}

HYDStatusCode hyd_bitwriter_flush(HYDBitWriter *bw) {
    HYDStatusCode ret;
    ret = hyd_write_zero_pad(bw);
    if (ret < HYD_ERROR_START)
        return ret;
    return drain_cache(bw);
}

HYDStatusCode hyd_write_u64(HYDBitWriter *bw, uint64_t value) {
    if (!value)
        return hyd_write(bw, 0, 2);
    if (value < 17)
        return hyd_write(bw, ((value - 1) << 2) | 1, 4 + 2);
    if (value < 273)
        return hyd_write(bw, ((value - 17) << 2) | 2, 8 + 2);
    hyd_write(bw, ((value & 0xFFF) << 2) | 3, 2 + 12);
    int shift = 12;
    while (1) {
        uint64_t svalue = value >> shift;
        if (!svalue)
            return hyd_write(bw, 0, 1);
        if (shift == 60) {
            return hyd_write(bw, ((svalue & 0xF) << 1) | 1, 1 + 4);
        } else {
            hyd_write(bw, ((svalue & 0xFF) << 1) | 1, 1 + 8);
            shift += 8;
        }
    }
}
