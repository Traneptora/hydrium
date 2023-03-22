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

typedef struct HYDAliasEntry {
    size_t count;
    int16_t *cutoffs;
    int16_t *offsets;
    int16_t *original;
} HYDAliasEntry;

typedef struct HYDHybridUintConfig {
    uint8_t split_exponent;
    uint8_t msb_in_token;
    uint8_t lsb_in_token;
} HYDHybridUintConfig;

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
    HYDAliasEntry *alias_table;
    HYDHybridUintConfig *configs;
} HYDEntropyStream;

HYDStatusCode hyd_ans_init_stream(HYDEntropyStream *stream, HYDAllocator *allocator, HYDBitWriter *bw,
                                  size_t symbol_count, const uint8_t *cluster_map, size_t num_dists,
                                  int custom_configs);
HYDStatusCode hyd_set_hybrid_uint_config(HYDEntropyStream *stream, uint8_t min_cluster, uint8_t to_cluster,
                                         int split_exponent, int msb_in_token, int lsb_in_token);
HYDStatusCode hyd_ans_send_symbol(HYDEntropyStream *stream, size_t dist, uint32_t symbol);
HYDStatusCode hyd_ans_write_stream_header(HYDEntropyStream *stream);
HYDStatusCode hyd_ans_finalize_stream(HYDEntropyStream *stream);

#endif /* HYD_ENTROPY_H_ */
