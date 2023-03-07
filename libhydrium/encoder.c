/*
 * Base encoder implementation
 */
#include <stddef.h>
#include <stdint.h>

#include "internal.h"
#include "libhydrium/color/xyb.h"

HYDStatusCode hyd_send_tile(HYDEncoder *encoder, const uint16_t *buffer[3],
                            size_t tile_x, size_t tile_y, ptrdiff_t row_stride, ptrdiff_t pixel_stride) {
    hyd_populate_xyb_buffer(encoder, buffer, row_stride, pixel_stride);


}
