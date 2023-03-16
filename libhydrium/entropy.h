#ifndef HYD_ENTROPY_H_
#define HYD_ENTROPY_H_

#include <stddef.h>
#include <stdint.h>

#include "bitwriter.h"

typedef struct HYDAnsToken {
    uint8_t token;
    uint8_t cluster;
} HYDAnsToken;

typedef struct HYDEntropyStream {
    HYDEncoder *encoder;
    size_t num_dists;
    uint8_t *cluster_map;
    size_t num_clusters;
    size_t symbol_count;
    size_t symbol_pos;
    HYDAnsToken *tokens;
    uint16_t *residues;
    int log_alphabet_size;
} HYDEntropyStream;

HYDStatusCode hyd_init_entropy_stream(HYDEncoder *encoder, HYDEntropyStream *stream, size_t symbol_count,
                                      const uint8_t *cluster_map, size_t num_dists);
HYDStatusCode hyd_ans_send_symbol(HYDEntropyStream *stream, size_t dist, uint16_t symbol);
HYDStatusCode hyd_finalize_entropy_stream(HYDEntropyStream *stream);

#endif /* HYD_ENTROPY_H_ */
