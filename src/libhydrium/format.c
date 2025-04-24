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

static inline uint16_t f32_to_u16(const float x) {
    const int32_t y = (int32_t)(x * 65535.f + 0.5f);
    return hyd_clamp(y, 0, 65535);
}

static inline HYD_vec3_f32 rgb_to_xyb_f32(const HYD_vec3_f32 rgb) {
    const float lgamma = hyd_cbrtf(0.3f * rgb.v0 + 0.622f * rgb.v1 + 0.078f * rgb.v2
        + 0.0037930732552754493f) - 0.155954f;
    const float mgamma = hyd_cbrtf(0.23f * rgb.v0 + 0.692f * rgb.v1 + 0.078f * rgb.v2
        + 0.0037930732552754493f) - 0.155954f;
    const float sgamma = hyd_cbrtf(0.243423f * rgb.v0 + 0.204767f * rgb.v1 + 0.55181f * rgb.v2
        + 0.0037930732552754493f) - 0.155954f;
    const float y = (lgamma + mgamma) * 0.5f;
    const float x = y - mgamma;
    const float b = sgamma - y;
    return (HYD_vec3_f32) { .v0 = x, .v1 = y, .v2 = b, };
}

static inline HYD_vec3_f32 rgb_to_xyb_u16(const float *output_lut, const HYD_vec3_u16 rgb) {
    const float lgamma = output_lut[((19661u * rgb.v0 + 40761u * rgb.v1 + 5112u * rgb.v2) >> 16) & 0xFFFFu];
    const float mgamma = output_lut[((15073u * rgb.v0 + 45350u * rgb.v1 + 5112u * rgb.v2) >> 16) & 0xFFFFu];
    const float sgamma = output_lut[((15953u * rgb.v0 + 13419u * rgb.v1 + 36163u * rgb.v2) >> 16) & 0xFFFFu];
    const float y = (lgamma + mgamma) * 0.5f;
    const float x = y - mgamma;
    const float b = sgamma - y;
    return (HYD_vec3_f32) { .v0 = x, .v1 = y, .v2 = b, };
}

static HYDStatusCode populate_input_lut(uint16_t **lut, const size_t size, const int need_linearize) {
    if (*lut)
        return HYD_OK;
    *lut = hyd_malloc_array(size, sizeof(**lut));
    if (!*lut)
        return HYD_NOMEM;
    const float factor = 1.0f / (size - 1.0f);
    for (size_t i = 0; i < size; i++) {
        const float f = i * factor;
        (*lut)[i] = f32_to_u16(need_linearize ? linearize(f) : f);
    }

    return HYD_OK;
}

static HYDStatusCode populate_output_lut(float **lut, const size_t size) {
    if (*lut)
        return HYD_OK;
    *lut = hyd_malloc_array(size, sizeof(**lut));
    if (!*lut)
        return HYD_NOMEM;
    const float factor = 1.0f / (size - 1.0f);
    for (size_t i = 0; i < size; i++)
        (*lut)[i] = hyd_cbrtf(i * factor + 0.0037930732552754493f) - 0.155954f;
    return HYD_OK;
}

HYDStatusCode hyd_populate_xyb_buffer(HYDEncoder *encoder, const void *const buffer[3],
        ptrdiff_t row_stride, ptrdiff_t pixel_stride, size_t lf_group_id,
        HYDSampleFormat sample_fmt) {
    int need_linearize = !encoder->metadata.linear_light;
    uint16_t *input_lut;
    float *bias_lut;
    HYDStatusCode ret;
    if (sample_fmt == HYD_UINT8 || sample_fmt == HYD_UINT16) {
        uint16_t **lutss = sample_fmt == HYD_UINT8 ? &encoder->input_lut8 : &encoder->input_lut16;
        size_t lutsize = sample_fmt == HYD_UINT8 ? 256 : 65536;
        ret = populate_input_lut(lutss, lutsize, need_linearize);
        if (ret < HYD_ERROR_START)
            return ret;
        input_lut = *lutss;
        ret = populate_output_lut(&encoder->bias_cbrtf_lut, 65536);
        if (ret < HYD_ERROR_START)
            return ret;
        bias_lut = encoder->bias_cbrtf_lut;
    }
    const HYDLFGroup *lfg = &encoder->lf_group[lf_group_id];
    for (size_t y = 0; y < lfg->height; y++) {
        const ptrdiff_t y_off = y * row_stride;
        const size_t row = y * lfg->stride;
        for (size_t x = 0; x < lfg->width; x++) {
            const ptrdiff_t offset = y_off + x * pixel_stride;
            HYD_vec3_f32 rgbf32;
            HYD_vec3_u16 rgbu16;
            /* populate rgb[3] */
            switch (sample_fmt) {
                case HYD_UINT8:
                    rgbu16.v0 = input_lut[((const uint8_t *)buffer[0])[offset]];
                    rgbu16.v1 = input_lut[((const uint8_t *)buffer[1])[offset]];
                    rgbu16.v2 = input_lut[((const uint8_t *)buffer[2])[offset]];
                    break;
                case HYD_UINT16:
                    rgbu16.v0 = input_lut[((const uint16_t *)buffer[0])[offset]];
                    rgbu16.v1 = input_lut[((const uint16_t *)buffer[1])[offset]];
                    rgbu16.v2 = input_lut[((const uint16_t *)buffer[2])[offset]];
                    break;
                case HYD_FLOAT32:
                    rgbf32.v0 = ((const float *)buffer[0])[offset];
                    rgbf32.v1 = ((const float *)buffer[1])[offset];
                    rgbf32.v2 = ((const float *)buffer[2])[offset];
                    if (!hyd_isfinite(rgbf32.v0) || !hyd_isfinite(rgbf32.v1) || !hyd_isfinite(rgbf32.v2)) {
                        encoder->error = "Invalid NaN Float";
                        return HYD_API_ERROR;
                    }
                    break;
                default:
                    encoder->error = "Invalid Sample Format";
                    return HYD_API_ERROR;
            }
            HYD_vec3_f32 xyb;
            /* process rgb[3] */
            if (sample_fmt == HYD_FLOAT32) {
                if (need_linearize) {
                    rgbf32.v0 = linearize(rgbf32.v0);
                    rgbf32.v1 = linearize(rgbf32.v1);
                    rgbf32.v2 = linearize(rgbf32.v2);
                }
                xyb = rgb_to_xyb_f32(rgbf32);
            } else {
                xyb = rgb_to_xyb_u16(bias_lut, rgbu16);
            }
            XYBEntry *entry = &encoder->xyb[row + x];
            entry->xyb[0].f = xyb.v0;
            entry->xyb[1].f = xyb.v1;
            entry->xyb[2].f = xyb.v2;
        }
        const size_t residue_x = 8 - (lfg->width & 0x7u);
        if (residue_x != 8)
            memset(encoder->xyb + row + lfg->width, 0, residue_x * sizeof(XYBEntry));
    }
    const size_t residue_y = 8 - (lfg->height & 0x7u);
    if (residue_y != 8)
        memset(encoder->xyb + lfg->height * lfg->stride, 0, residue_y * lfg->stride * sizeof(XYBEntry));

    return HYD_OK;
}
