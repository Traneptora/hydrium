/* 
 * Hydrium internal header implementation
 */
#ifndef HYDRIUM_INTERNAL_H_
#define HYDRIUM_INTERNAL_H_

#include "hydrium.h"

#define HYD_ALLOC(encoder, size) (encoder)->allocator.alloc_func((size), (encoder)->allocator.opaque)
#define HYD_FREE(encoder, ptr) (encoder)->allocator.free_func((ptr), (encoder)->allocator.opaque)

/* opaque structure */
struct HYDEncoder {
    HYDAllocator allocator;
    HYDImageMetadata metadata;
    /* 256x256 tile */
    uint16_t xyb[3][256][256];

    uint8_t *out;
    size_t out_len;
};

#endif /* HYDRIUM_INTERNAL_H_ */
