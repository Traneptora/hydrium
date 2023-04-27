#ifndef HYD_ENTROPY_H_
#define HYD_ENTROPY_H_

#include <stddef.h>
#include <stdint.h>

#include "bitwriter.h"

typedef struct HYDHybridSymbol {
    uint16_t token;
    uint8_t cluster;
    uint8_t residue_bits;
    uint32_t residue;
} HYDHybridSymbol;

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
    HYDHybridSymbol *symbols;
    uint16_t max_alphabet_size;
    uint16_t *alphabet_sizes;
    uint16_t *frequencies;
    HYDHybridUintConfig *configs;

    // lz77 only
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
HYDStatusCode hyd_prefix_write_stream_symbols(HYDEntropyStream *stream, size_t symbol_start, size_t symbol_count);
HYDStatusCode hyd_prefix_finalize_stream(HYDEntropyStream *stream);

HYDStatusCode hyd_ans_write_stream_header(HYDEntropyStream *stream);
HYDStatusCode hyd_ans_write_stream_symbols(HYDEntropyStream *stream, size_t symbol_offset, size_t symbol_count);

void hyd_entropy_stream_destroy(HYDEntropyStream *stream);

#endif /* HYD_ENTROPY_H_ */
