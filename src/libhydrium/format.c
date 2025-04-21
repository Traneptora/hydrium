/*
 * Pixel format conversion routines
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "libhydrium/libhydrium.h"
#include "format.h"
#include "internal.h"
#include "math-functions.h"
#include "memory.h"

static inline float linearize(const float x) {
    if (x <= 0.0404482362771082f)
        return 0.07739938080495357f * x;
    return 0.003094300919832f + x * (-0.009982599f + x * (0.72007737769f + 0.2852804880f * x));
}

static inline float hyd_cbrtf(const float x) {
    union { float f; uint32_t i; } z = { .f = x };
    z.i = 0x548c39cbu - z.i / 3u;
    z.f *= 1.5015480449f - 0.534850249f * x * z.f * z.f * z.f;
    z.f *= 1.333333985f - 0.33333333f * x * z.f * z.f * z.f;
    return 1.0f / z.f;
}

static inline void rgb_to_xyb(const float rgb[3], float *xyb) {
    const float lgamma = hyd_cbrtf(0.3f * rgb[0] + 0.622f * rgb[1] + 0.078f * rgb[2]
        + 0.0037930732552754493f) - 0.155954f;
    const float mgamma = hyd_cbrtf(0.23f * rgb[0] + 0.692f * rgb[1] + 0.078f * rgb[2]
        + 0.0037930732552754493f) - 0.155954f;
    const float sgamma = hyd_cbrtf(0.243423f * rgb[0] + 0.204767f * rgb[1] + 0.55181f * rgb[2]
        + 0.0037930732552754493f) - 0.155954f;
    xyb[0] = (lgamma - mgamma) * 0.5f;
    const float y = xyb[1] = (lgamma + mgamma) * 0.5f;
    /* chroma-from-luma adds B to Y */
    xyb[2] = sgamma - y;
}

static HYDStatusCode populate_lut(float **lut, const size_t size, const int linear) {
    if (*lut)
        return HYD_OK;
    *lut = hyd_malloc_array(size, sizeof(float));
    if (!*lut)
        return HYD_NOMEM;
    const float factor = (1.0f / (size - 1.0f));
    if (linear) {
        for (size_t i = 0; i < size; i++)
            (*lut)[i] = linearize(i * factor);
    } else {
        for (size_t i = 0; i < size; i++)
            (*lut)[i] = i * factor;
    }

    return HYD_OK;
}

HYDStatusCode hyd_populate_xyb_buffer(HYDEncoder *encoder, const void *const buffer[3],
    ptrdiff_t row_stride, ptrdiff_t pixel_stride, size_t lf_group_id,
    HYDSampleFormat sample_fmt) {
    int need_linearize = !encoder->metadata.linear_light;
    float *lut;
    HYDStatusCode ret;
    if (sample_fmt == HYD_UINT8) {
        ret = populate_lut(&encoder->lut_8bit[need_linearize], 256, need_linearize);
        if (ret < HYD_ERROR_START)
            return ret;
        lut = encoder->lut_8bit[need_linearize];
    } else if (sample_fmt == HYD_UINT16) {
        ret = populate_lut(&encoder->lut_16bit[need_linearize], 65536, need_linearize);
        if (ret < HYD_ERROR_START)
            return ret;
        lut = encoder->lut_16bit[need_linearize];
    }
    for (size_t y = 0; y < encoder->lf_group[lf_group_id].lf_group_height; y++) {
        const ptrdiff_t y_off = y * row_stride;
        const size_t row = y * encoder->lf_group[lf_group_id].stride;
        for (size_t x = 0; x < encoder->lf_group[lf_group_id].lf_group_width; x++) {
            const ptrdiff_t offset = y_off + x * pixel_stride;
            float rgb[3];
            switch (sample_fmt) {
                case HYD_UINT8:
                    rgb[0] = lut[((uint8_t *)buffer[0])[offset]];
                    rgb[1] = lut[((uint8_t *)buffer[1])[offset]];
                    rgb[2] = lut[((uint8_t *)buffer[2])[offset]];
                    break;
                case HYD_UINT16:
                    rgb[0] = lut[((uint16_t *)buffer[0])[offset]];
                    rgb[1] = lut[((uint16_t *)buffer[1])[offset]];
                    rgb[2] = lut[((uint16_t *)buffer[2])[offset]];
                    break;
                case HYD_FLOAT32:
                    rgb[0] = ((float *)buffer[0])[offset];
                    rgb[1] = ((float *)buffer[1])[offset];
                    rgb[2] = ((float *)buffer[2])[offset];
                    if (!hyd_isfinite(rgb[0]) || !hyd_isfinite(rgb[1]) || !hyd_isfinite(rgb[2])) {
                        encoder->error = "Invalid NaN Float";
                        return HYD_API_ERROR;
                    }
                    if (need_linearize) {
                        rgb[0] = linearize(rgb[0]);
                        rgb[1] = linearize(rgb[1]);
                        rgb[2] = linearize(rgb[2]);
                    }
                    break;
                default:
                    encoder->error = "Invalid Sample Format";
                    return HYD_INTERNAL_ERROR;
            }
            rgb_to_xyb(rgb, &encoder->xyb[3 * (row + x)].f);
        }
        const size_t residue_x = encoder->lf_group[lf_group_id].lf_group_width & 0x7u;
        if (residue_x) {
            memset(&encoder->xyb[3 * (row + encoder->lf_group[lf_group_id].lf_group_width)], 0,
                3 * (8 - residue_x) * sizeof(XYBEntry));
        }
    }

    const size_t residue_y = encoder->lf_group[lf_group_id].lf_group_height & 0x7u;
    if (residue_y) {
        memset(&encoder->xyb[3 * (encoder->lf_group[lf_group_id].lf_group_height
            * encoder->lf_group[lf_group_id].stride)], 0,
            3 * (8 - residue_y) * encoder->lf_group[lf_group_id].stride * sizeof(XYBEntry));
    }

    return HYD_OK;
}
