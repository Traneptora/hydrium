/* 
 * Hydrium internal header implementation
 */
#ifndef HYDRIUM_INTERNAL_H_
#define HYDRIUM_INTERNAL_H_

#include "bitwriter.h"
#include "libhydrium/libhydrium.h"

/* opaque structure */
struct HYDEncoder {
    HYDAllocator allocator;
    HYDImageMetadata metadata;

    /* one LF group */
    int16_t *xyb;

    uint8_t *out;
    size_t out_pos;
    size_t out_len;

    HYDBitWriter writer;
    HYDBitWriter working_writer;
    size_t copy_pos;

    size_t tile_count_x;
    size_t tile_count_y;
    size_t lf_group_x;
    size_t lf_group_y;
    size_t lf_group_width;
    size_t lf_group_height;
    size_t lf_varblock_width;
    size_t lf_varblock_height;

    int wrote_header;
    int wrote_frame_header;
    int level10;
};

#endif /* HYDRIUM_INTERNAL_H_ */
