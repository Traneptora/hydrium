#include <stddef.h>
#include <stdint.h>

#include "xyb.h"
#include "libhydrium/internal.h"

HYDStatusCode hyd_populate_xyb_buffer(HYDEncoder *encoder, const uint16_t *buffer[3], ptrdiff_t row_stride, ptrdiff_t pixel_stride) {
    for (size_t y = 0; y < 256; y++) {
        for (size_t x = 0; x < 256; x++) {
            const ptrdiff_t offset = y * row_stride + x * pixel_stride;
            const int64_t r = buffer[0][offset];
            const int64_t g = buffer[1][offset];
            const int64_t b = buffer[2][offset];
            
        }
    }
}
