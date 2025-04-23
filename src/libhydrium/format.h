#ifndef HYD_FORMAT_H_
#define HYD_FORMAT_H_

#include <stddef.h>

#include "libhydrium/libhydrium.h"

typedef struct HYD_vec3_f32 {
    float v0, v1, v2;
} HYD_vec3_f32;

typedef struct HYD_vec3_u16 {
    uint16_t v0, v1, v2;
} HYD_vec3_u16;

HYDStatusCode hyd_populate_xyb_buffer(HYDEncoder *encoder, const void *const buffer[3],
    ptrdiff_t row_stride, ptrdiff_t pixel_stride, size_t lf_group_id,
    HYDSampleFormat sample_fmt);

#endif /* HYD_FORMAT_H_ */
