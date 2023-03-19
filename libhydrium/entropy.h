#ifndef HYD_ENTROPY_H_
#define HYD_ENTROPY_H_

#include <stddef.h>
#include <stdint.h>

#include "bitwriter.h"

typedef struct HYDAnsToken {
    uint8_t token;
    uint8_t cluster;
} HYDAnsToken;

typedef struct HYDAnsResidue {
    uint16_t residue;
    uint8_t bits;
} HYDAnsResidue;

typedef struct HYDEntropyStream {
    HYDAllocator *allocator;
    HYDBitWriter *bw;
    size_t num_dists;
    uint8_t *cluster_map;
    size_t num_clusters;
    size_t init_symbol_count;
    size_t symbol_pos;
    HYDAnsToken *tokens;
    HYDAnsResidue *residues;
    int alphabet_size;
    size_t *frequencies;
    uint16_t *cutoffs;
    uint16_t *offsets;
    uint16_t *symbols;
} HYDEntropyStream;

HYDStatusCode hyd_ans_init_stream(HYDEntropyStream *stream, HYDAllocator *allocator, HYDBitWriter *bw,
                                      size_t symbol_count, const uint8_t *cluster_map, size_t num_dists);
HYDStatusCode hyd_ans_send_symbol(HYDEntropyStream *stream, size_t dist, uint16_t symbol);
HYDStatusCode hyd_ans_write_stream_header(HYDEntropyStream *stream);
HYDStatusCode hyd_ans_finalize_stream(HYDEntropyStream *stream);

#endif /* HYD_ENTROPY_H_ */
