#ifndef HYDRIUM_MATH_FUNCTIONS_H_
#define HYDRIUM_MATH_FUNCTIONS_H_

#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define hyd_fllog2(n) (__builtin_clzll(1) - __builtin_clzll((n)|1))
#elif defined(_MSC_VER)
#include <inttrin.h>
static inline int __builtin_clzll(const unsigned long long x) {
#ifdef _WIN64
    return (int)_lzcnt_u64(x);
#else
    return ((unsigned)(x >> 32)) ? __builtin_clz((unsigned)(x >> 32)) : 32 + __builtin_clz((unsigned)x);
#endif
}
#define hyd_fllog2(n) (__builtin_clzll(1) - __builtin_clzll((n)|1))
#else /* _MSC_VER */
static inline int hyd_fllog2(unsigned long long n) {
    int i = 0;
    if (n >= (1ULL << 32)) {
        i += 32;
        n >>= 32;
    }
    if (n >= (1ULL << 16)) {
        i += 16;
        n >>= 16;
    }
    if (n >= (1ULL << 8)) {
        i += 8;
        n >>= 8;
    }
    if (n >= (1ULL << 4)) {
        i += 4;
        n >>= 4;
    }
    if (n >= (1ULL << 2)) {
        i += 2;
        n >>= 2;
    }
    if (n >= (1ULL << 1))
        ++i;
    return i;
}
#endif /* __GNUC__ || __clang__ || _MSC_VER */

/* ceil(log2(n)) */
static inline int hyd_cllog2(const unsigned long long n) {
    return hyd_fllog2(n) + !!(n & (n - 1));
}

static inline uint32_t hyd_pack_signed(const int32_t v) {
    return ((uint32_t)v << 1) ^ -!!(v & UINT32_C(0x80000000));
}

static inline int hyd_isfinite(const float x) {
    const union { uint32_t i; float f; } z = { .f = x };
    return (z.i & 0x7f800000) != 0x7f800000;
}

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
