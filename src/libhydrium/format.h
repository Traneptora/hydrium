#ifndef HYD_FORMAT_H_
#define HYD_FORMAT_H_

#include <stddef.h>

#include "libhydrium/libhydrium.h"

typedef struct {
    uint16_t r;
    uint16_t g;
    uint16_t b;
} HYDrgb3u16_t;

typedef struct {
    float r;
    float g;
    float b;
} HYDrgb3f32_t;

HYDStatusCode hyd_populate_xyb_buffer(HYDEncoder *encoder, const void *const buffer[3],
    ptrdiff_t row_stride, ptrdiff_t pixel_stride, size_t lf_group_id,
    HYDSampleFormat sample_fmt);

#endif /* HYD_FORMAT_H_ */
