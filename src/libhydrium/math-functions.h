#ifndef HYDRIUM_MATH_FUNCTIONS_H_
#define HYDRIUM_MATH_FUNCTIONS_H_

#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)

#define hyd_fllog2_ull(n) ((int)((sizeof(unsigned long long) << 3) - 1) - __builtin_clzll(n))
#define hyd_fllog2_ul(n) ((int)((sizeof(unsigned long) << 3) - 1) - __builtin_clzl(n))
#define hyd_fllog2_u(n) ((int)((sizeof(unsigned int) << 3) - 1) - __builtin_clz(n))

#define hyd_fllog2(n) _Generic((n),        \
    long: hyd_fllog2_ul(n),                \
    unsigned long: hyd_fllog2_ul(n),       \
    int: hyd_fllog2_u(n),                  \
    unsigned int: hyd_fllog2_u(n),         \
    default: hyd_fllog2_ull(n)             \
)

#elif defined(_MSC_VER)

#include <inttrin.h>
static inline int __builtin_clzll(const unsigned long long x) {
    const unsigned int y = x >> 32;
    return y ? __builtin_clz(y) : 32 + __builtin_clz(x);
}
#define hyd_fllog2_ull(n) ((int)((sizeof(unsigned long long) << 3) - 1) - __builtin_clzll(n))
#define hyd_fllog2_u(n) ((int)((sizeof(unsigned int) << 3) - 1) - __builtin_clz(n))

#define hyd_fllog2(n) _Generic((n),        \
    int: hyd_fllog2_u(n),                  \
    unsigned int: hyd_fllog2_u(n),         \
    default: hyd_fllog2_ull(n)             \
)

#else /* _MSC_VER */
static inline int hyd_fllog2(unsigned long long n) {
    int i = 0;
    if (n >= (1llu << 32)) {
        i += 32;
        n >>= 32;
    }
    if (n >= (1llu << 16)) {
        i += 16;
        n >>= 16;
    }
    if (n >= (1llu << 8)) {
        i += 8;
        n >>= 8;
    }
    if (n >= (1llu << 4)) {
        i += 4;
        n >>= 4;
    }
    if (n >= (1llu << 2)) {
        i += 2;
        n >>= 2;
    }
    if (n >= (1llu << 1))
        ++i;
    return i;
}
#endif /* __GNUC__ || __clang__ || _MSC_VER */

/* ceil(log2(n)) */

#define hyd_cllog2(n) (hyd_fllog2(n) + !!((n) & ((n) - 1)))

static inline uint32_t hyd_pack_signed(const int32_t v) {
    const uint32_t w = v;
    return (w << 1) ^ -(w >> 31);
}

static inline int hyd_isfinite(const float x) {
    const union { uint32_t i; float f; } z = { .f = x };
    return (z.i & 0x7f800000u) != 0x7f800000u;
}

#define hyd_ceil_div(num, den) (((num) + (den) - 1) / (den))
#define hyd_abs(a) ((a) < 0 ? -(a) : (a))
#define hyd_array_size(a) (sizeof((a))/sizeof(*(a)))
#define hyd_max(a, b) ((a) > (b) ? (a) : (b))
#define hyd_min(a, b) ((a) < (b) ? (a) : (b))
#define hyd_clamp(v, min, max) ((v) < (min) ? (min) : (v) > (max) ? (max) : (v))
#define hyd_max3(a, b, c) hyd_max((a), hyd_max((b), (c)))
#define hyd_swap(type, a, b) do {\
    const type __hyd_swap_temp = (b); (b) = (a); (a) = __hyd_swap_temp;\
} while (0)

#endif /* HYDRIUM_MATH_FUNCTIONS_H_ */
