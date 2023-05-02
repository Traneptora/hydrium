#include <stddef.h>
#include <stdint.h>

#include "internal.h"
#include "math-functions.h"
#include "xyb.h"

static int64_t linearize(const int64_t srgb) {
    if (srgb <= 2650)
        return (srgb * UINT64_C(332427809)) >> 32;
    const uint64_t prepow = (srgb + 3604) * UINT64_C(4071059048);
    const uint64_t log = hyd_fllog2(prepow);
    const uint64_t prepow_d = ((prepow >> (log - 20)) & ~(~UINT64_C(0) << 20)) | ((0x3FE + log - 47) << 20);
    const uint64_t postpow_d = ((((prepow_d - INT64_C(1072632447)) * 410) >> 10) + INT64_C(1072632447));
    const uint64_t postpow = ((postpow_d & ~(~UINT64_C(0) << 20)) | (UINT64_C(1) << 20))
                             >> (1027 - ((postpow_d >> 20) & 0x3FF));
    const uint64_t prepow_s = prepow >> 32;
    return (((prepow_s * prepow_s) >> 16) * postpow) >> 16;
}

static uint64_t icbrt(uint64_t x) {
    uint64_t y = 0;
    for (int s = 63; s >= 0; s -= 3) {
        y <<= 1;
        const uint64_t b = (3 * y * (y + 1) + 1) << s;
        if (x >= b) {
            x -= b;
            y++;
        }
    }

    return y;
}

static int64_t pow_one_third(int64_t mix) {
    if (mix < 0)
        return -pow_one_third(-mix);
    return icbrt(((uint64_t)mix) << 32);
}

static void rgb_to_xyb(HYDEncoder *encoder, const size_t off, const int64_t r, const int64_t g, const int64_t b) {
    const int64_t rp = encoder->metadata.linear_light ? r : linearize(r);
    const int64_t gp = encoder->metadata.linear_light ? g : linearize(g);
    const int64_t bp = encoder->metadata.linear_light ? b : linearize(b);
    const int64_t lgamma = pow_one_third(((rp * 307) >> 10) + ((gp * 637) >> 10) + ((bp * 319) >> 12) + 249) - 10220;
    const int64_t mgamma = pow_one_third(((rp * 471) >> 11) + ((gp * 45351) >> 16) + ((bp * 319) >> 12) + 249) - 10220;
    const int64_t sgamma = pow_one_third(((rp * 997) >> 12) + ((gp * 3355) >> 14) + ((bp * 565) >> 10) + 249) - 10220;
    encoder->xyb[3 * off] = hyd_signed_rshift64(lgamma - mgamma, 3);
    encoder->xyb[3 * off + 1] = hyd_signed_rshift64(lgamma + mgamma, 3);
    /* chroma-from-luma adds B to Y */
    encoder->xyb[3 * off + 2] = hyd_signed_rshift64(sgamma, 2) - encoder->xyb[3 * off + 1];
}

HYDStatusCode hyd_populate_xyb_buffer(HYDEncoder *encoder, const uint16_t *const buffer[3], ptrdiff_t row_stride, ptrdiff_t pixel_stride, size_t lf_group_id) {
    for (size_t y = 0; y < encoder->lf_group[lf_group_id].lf_group_height; y++) {
        const ptrdiff_t y_off = y * row_stride;
        const size_t row = y * encoder->lf_group[lf_group_id].lf_group_width;
        for (size_t x = 0; x < encoder->lf_group[lf_group_id].lf_group_width; x++) {
            const ptrdiff_t offset = y_off + x * pixel_stride;
            rgb_to_xyb(encoder, row + x, buffer[0][offset], buffer[1][offset], buffer[2][offset]);
        }
    }

    return HYD_OK;
}

HYDStatusCode hyd_populate_xyb_buffer8(HYDEncoder *encoder, const uint8_t *const buffer[3], ptrdiff_t row_stride, ptrdiff_t pixel_stride, size_t lf_group_id) {
    for (size_t y = 0; y < encoder->lf_group[lf_group_id].lf_group_height; y++) {
        const ptrdiff_t y_off = y * row_stride;
        const size_t row = y * encoder->lf_group[lf_group_id].lf_group_width;
        for (size_t x = 0; x < encoder->lf_group[lf_group_id].lf_group_width; x++) {
            const ptrdiff_t offset = y_off + x * pixel_stride;
            rgb_to_xyb(encoder, row + x, buffer[0][offset] * UINT64_C(257), buffer[1][offset] * UINT64_C(257),
                buffer[2][offset] * UINT64_C(257));
        }
    }

    return HYD_OK;
}
