#ifndef HYD_FORMAT_H_
#define HYD_FORMAT_H_

#include <stddef.h>

#include "libhydrium/libhydrium.h"

HYDStatusCode hyd_populate_xyb_buffer(HYDEncoder *encoder, const void *const buffer[3],
    ptrdiff_t row_stride, ptrdiff_t pixel_stride, size_t lf_group_id,
    HYDSampleFormat sample_fmt);

#endif /* HYD_FORMAT_H_ */
