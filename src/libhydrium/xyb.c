#include <stddef.h>
#include <stdint.h>
#include <math.h>

#include "internal.h"
#include "math-functions.h"
#include "xyb.h"

static inline float linearize(const float srgb) {
    if (srgb <= 0.0404482362771082f)
        return 0.07739938080495357f * srgb;

    return powf((srgb + 0.055f) * 0.9478672985781991f, 2.4f);
}

static inline void rgb_to_xyb(HYDEncoder *encoder, const size_t off, const float r, const float g, const float b) {
    const float lgamma = cbrtf(0.3f * r + 0.622f * g + 0.078f * b + 0.0037930732552754493f) - 0.155954f;
    const float mgamma = cbrtf(0.23f * r + 0.692f * g + 0.078f * b + 0.0037930732552754493f) - 0.155954f;
    const float sgamma = cbrtf(0.243423f * r + 0.204767f * g + 0.55181f * b + 0.0037930732552754493f) - 0.155954f;
    encoder->xyb[off].f = (lgamma - mgamma) * 0.5f;
    encoder->xyb[off + 1].f = (lgamma + mgamma) * 0.5f;
    /* chroma-from-luma adds B to Y */
    encoder->xyb[off + 2].f = sgamma - encoder->xyb[off + 1].f;
}

HYDStatusCode hyd_populate_xyb_buffer(HYDEncoder *encoder, const uint16_t *const buffer[3], ptrdiff_t row_stride, ptrdiff_t pixel_stride, size_t lf_group_id) {
    for (size_t y = 0; y < encoder->lf_group[lf_group_id].lf_group_height; y++) {
        const ptrdiff_t y_off = y * row_stride;
        const size_t row = y * encoder->lf_group[lf_group_id].stride;
        for (size_t x = 0; x < encoder->lf_group[lf_group_id].lf_group_width; x++) {
            const ptrdiff_t offset = y_off + x * pixel_stride;
            float r = buffer[0][offset] * (1.0f / 65535.0f);
            float g = buffer[1][offset] * (1.0f / 65535.0f);
            float b = buffer[2][offset] * (1.0f / 65535.0f);
            if (!encoder->metadata.linear_light) {
                r = linearize(r);
                g = linearize(g);
                b = linearize(b);
            }
            rgb_to_xyb(encoder, 3 * (row + x), r, g, b);
        }
    }

    return HYD_OK;
}

HYDStatusCode hyd_populate_xyb_buffer8(HYDEncoder *encoder, const uint8_t *const buffer[3], ptrdiff_t row_stride, ptrdiff_t pixel_stride, size_t lf_group_id) {
    for (size_t y = 0; y < encoder->lf_group[lf_group_id].lf_group_height; y++) {
        const ptrdiff_t y_off = y * row_stride;
        const size_t row = y * encoder->lf_group[lf_group_id].stride;
        for (size_t x = 0; x < encoder->lf_group[lf_group_id].lf_group_width; x++) {
            const ptrdiff_t offset = y_off + x * pixel_stride;
            float r = buffer[0][offset] * (1.0f / 255.0f);
            float g = buffer[1][offset] * (1.0f / 255.0f);
            float b = buffer[2][offset] * (1.0f / 255.0f);
            if (!encoder->metadata.linear_light) {
                r = linearize(r);
                g = linearize(g);
                b = linearize(b);
            }
            rgb_to_xyb(encoder, 3 * (row + x), r, g, b);
        }
    }

    return HYD_OK;
}
