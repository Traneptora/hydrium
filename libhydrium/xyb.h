#ifndef HYDRIUM_XYB_H_
#define HYDRIUM_XYB_H_

#include <stddef.h>
#include <stdint.h>

#include "libhydrium.h"

HYDStatusCode hyd_populate_xyb_buffer(HYDEncoder *encoder, const uint16_t *const buffer[3],
                                      ptrdiff_t row_stride, ptrdiff_t pixel_stride);
HYDStatusCode hyd_populate_xyb_buffer8(HYDEncoder *encoder, const uint8_t *const buffer[3], ptrdiff_t row_stride,
                                       ptrdiff_t pixel_stride);

#endif /* HYDRIUM_XYB_H_ */
