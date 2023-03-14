#ifndef HYD_ENTROPY_H_
#define HYD_ENTROPY_H_

#include <stddef.h>
#include <stdint.h>

#include "bitwriter.h"

typedef struct HYDAnsStream {
    HYDBitWriter *bw;
    uint32_t state;
    size_t num_dists;
    uint8_t cluster_map[256];
    size_t num_clusters;
} HYDAnsStream;

void hyd_init_ans_stream(HYDAnsStream *stream, HYDBitWriter *bw);
HYDStatusCode hyd_write_cluster_map(HYDAnsStream *stream, const uint8_t *cluster_map, size_t num_dists);

#endif /* HYD_ENTROPY_H_ */
