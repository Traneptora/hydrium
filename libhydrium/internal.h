/* 
 * Hydrium internal header implementation
 */
#ifndef HYDRIUM_INTERNAL_H_
#define HYDRIUM_INTERNAL_H_

#include "bitwriter.h"
#include "libhydrium.h"

#define HYD_ALLOC(encoder, size) ((encoder)->allocator.alloc_func((size), (encoder)->allocator.opaque))
#define HYD_FREE(encoder, ptr) if (ptr) {((encoder)->allocator.free_func((ptr), (encoder)->allocator.opaque)), (ptr) = NULL;}

/* opaque structure */
struct HYDEncoder {
    HYDAllocator allocator;
    HYDImageMetadata metadata;

    /* 256x256 tile */
    int16_t xyb[3][256][256];
    uint8_t working_buffer[262144];

    uint8_t *out;
    size_t out_pos;
    size_t out_len;

    HYDBitWriter writer;
    HYDBitWriter working_writer;
    size_t copy_pos;

    size_t group_x;
    size_t group_y;
    size_t group_width;
    size_t group_height;

    int wrote_header;
    int wrote_frame_header;
    int level10;
};

#endif /* HYDRIUM_INTERNAL_H_ */
