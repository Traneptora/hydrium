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

typedef struct HYDVLCElement {
    int32_t symbol;
    int length;
} HYDVLCElement;

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
    uint32_t max_alphabet_size;
    uint32_t *alphabet_sizes;
    size_t *frequencies;
    HYDHybridUintConfig *configs;

    uint32_t lz77_min_length;
    uint32_t lz77_min_symbol;
    uint32_t last_symbol;
    uint32_t lz77_rle_count;

    // prefix only
    HYDVLCElement *vlc_table;

    // ans only
    HYDAliasEntry *alias_table;
} HYDEntropyStream;

HYDStatusCode hyd_entropy_init_stream(HYDEntropyStream *stream, HYDAllocator *allocator, HYDBitWriter *bw,
                                      size_t symbol_count, const uint8_t *cluster_map, size_t num_dists,
                                      int custom_configs, uint32_t lz77_min_symbol);
HYDStatusCode hyd_entropy_set_hybrid_config(HYDEntropyStream *stream, uint8_t min_cluster, uint8_t to_cluster,
                                            int split_exponent, int msb_in_token, int lsb_in_token);
HYDStatusCode hyd_entropy_send_symbol(HYDEntropyStream *stream, size_t dist, uint32_t symbol);

HYDStatusCode hyd_prefix_write_stream_header(HYDEntropyStream *stream);
HYDStatusCode hyd_prefix_finalize_stream(HYDEntropyStream *stream);

HYDStatusCode hyd_ans_write_stream_header(HYDEntropyStream *stream);
HYDStatusCode hyd_ans_finalize_stream(HYDEntropyStream *stream);

#endif /* HYD_ENTROPY_H_ */
