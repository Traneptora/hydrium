/* 
 * Hydrium internal header implementation
 */
#ifndef HYDRIUM_INTERNAL_H_
#define HYDRIUM_INTERNAL_H_

#include "libhydrium/libhydrium.h"

#include "bitwriter.h"
#include "entropy.h"
#include "format.h"

typedef struct HYDLFGroup {
    size_t tile_count_x, tile_count_y;
    size_t x, y;
    size_t width, height;
    size_t varblock_width, varblock_height;
    size_t stride;
} HYDLFGroup;

typedef struct XYBEntry {
    union {
        int32_t i;
        float f;
    } xyb[3];
} XYBEntry;

typedef struct HFBarrier {
    size_t barrier_index;
    uint8_t preset;
} HFBarrier;

/* opaque structure */
struct HYDEncoder {
    HYDImageMetadata metadata;
    HYDEntropyStream hf_stream;

    XYBEntry *xyb;

    int one_frame;
    int last_tile;

    size_t lfg_count_y, lfg_count_x;
    size_t lfg_per_frame;

    HYDLFGroup lfg_array[64];
    HYDLFGroup *lfg;

    size_t lfg_perm_array[64];
    size_t *lfg_perm;

    uint8_t *out;
    size_t out_pos;
    size_t out_len;

    HYDBitWriter writer;
    HYDBitWriter working_writer;
    size_t copy_pos;

    int wrote_header;
    int wrote_frame_header;
    size_t tiles_sent;
    int level10;

    size_t section_endpos_array[64];
    size_t *section_endpos;
    size_t section_count;

    HFBarrier *hf_stream_barrier;
    size_t groups_encoded;

    const char *error;

    uint16_t *input_lut8;
    uint16_t *input_lut16;
    float *bias_cbrtf_lut;
};

HYDStatusCode hyd_populate_lf_group(HYDEncoder *encoder, HYDLFGroup **lf_group, uint32_t tile_x, uint32_t tile_y);

#endif /* HYDRIUM_INTERNAL_H_ */
