/* 
 * Hydrium internal header implementation
 */
#ifndef HYDRIUM_INTERNAL_H_
#define HYDRIUM_INTERNAL_H_

#include "bitwriter.h"
#include "entropy.h"
#include "libhydrium/libhydrium.h"

typedef struct HYDLFGroup {
    size_t tile_count_x;
    size_t tile_count_y;
    size_t lf_group_x;
    size_t lf_group_y;
    size_t lf_group_width;
    size_t lf_group_height;
    size_t lf_varblock_width;
    size_t lf_varblock_height;
    size_t stride;
} HYDLFGroup;

/* opaque structure */
struct HYDEncoder {
    HYDAllocator allocator;
    HYDImageMetadata metadata;
    HYDEntropyStream hf_stream;

    int16_t *xyb;

    int one_frame;
    int last_tile;
    HYDLFGroup *lf_group;
    size_t lf_group_count_x;
    size_t lf_group_count_y;
    size_t lf_groups_per_frame;

    uint8_t *out;
    size_t out_pos;
    size_t out_len;

    HYDBitWriter writer;
    HYDBitWriter working_writer;
    size_t copy_pos;

    int wrote_header;
    int wrote_frame_header;
    int tile_sent;
    int level10;

    size_t *section_endpos;
    size_t section_count;
    size_t *hf_stream_barrier;

    size_t groups_encoded;
};

#endif /* HYDRIUM_INTERNAL_H_ */
