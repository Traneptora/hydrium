#include <stddef.h>
#include <stdint.h>

#include "xyb.h"
#include "libhydrium/internal.h"

static inline int uint64_log2(uint64_t n) {
#define S(k) if (n >= (UINT64_C(1) << k)) { i += k; n >>= k; }
  int i = -(n == 0); S(32); S(16); S(8); S(4); S(2); S(1); return i;
#undef S
}

static int64_t linearize(const int64_t srgb) {
    if (srgb <= 2650)
        return (srgb * UINT64_C(332427809)) >> 32;
    const uint64_t prepow = (srgb + 3604) * UINT64_C(4071059048);
    const uint64_t log = uint64_log2(prepow);
    const uint64_t prepow_d = (prepow >> (log - 20)) & ~(~UINT64_C(0) << 20) | ((0x3FE + log - 47) << 20);
    const uint64_t postpow_d = ((((prepow_d - INT64_C(1072632447)) * 410) >> 10) + INT64_C(1072632447));
    const uint64_t postpow = ((postpow_d & ~(~UINT64_C(0) << 20)) | (UINT64_C(1) << 20))
                             >> (1027 - ((postpow_d >> 20) & 0x3FF));
    const uint64_t prepow_s = prepow >> 32;
    return (((prepow_s * prepow_s) >> 16) * postpow) >> 16;
}

HYDStatusCode hyd_populate_xyb_buffer(HYDEncoder *encoder, const uint16_t *buffer[3], ptrdiff_t row_stride, ptrdiff_t pixel_stride) {
    for (size_t y = 0; y < 256; y++) {
        for (size_t x = 0; x < 256; x++) {
            const ptrdiff_t offset = y * row_stride + x * pixel_stride;
            const int64_t r = buffer[0][offset];
            const int64_t g = buffer[1][offset];
            const int64_t b = buffer[2][offset];
            const int64_t rp = encoder->metadata.linear_light ? r : linearize(r);
            const int64_t gp = encoder->metadata.linear_light ? g : linearize(g);
            const int64_t bp = encoder->metadata.linear_light ? b : linearize(b);

        }

    }
}
