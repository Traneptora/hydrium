#ifndef HYDRIUM_OSDEP_H_
#define HYDRIUM_OSDEP_H_

#define hyd_fllog2__(n) (__builtin_clzll(1) - __builtin_clzll((n)|1))

#if defined(__GNUC__) || defined(__clang__)
#define hyd_fllog2(n) hyd_fllog2__(n)
#else
#ifdef _MSC_VER
#include <inttrin.h>
static inline int __builtin_clzll(unsigned long long x) {
#ifdef _WIN64
    return (int)_lzcnt_u64(x);
#else
    return !!unsigned(x >> 32) ? __builtin_clz((unsigned)(x >> 32)) : 32 + __builtin_clz((unsigned)x);
#endif
}
#define hyd_fllog2(n) hyd_fllog2__(n)
#else /* _MSC_VER */
static inline int hyd_fllog2(unsigned long long n) {
    #define S(k) if (n >= (1ULL << k)) { i += k; n >>= k; }
    int i = -(n == 0ULL); S(32); S(16); S(8); S(4); S(2); S(1); return i;
    #undef S
}
#endif /* _MSC_VER */
#endif /* __GNUC__ || __clang__ */

#endif /* HYDRIUM_OSDEP_H_ */
