#ifndef HYD_ENTROPY_H_
#define HYD_ENTROPY_H_

#include "bitwriter.h"

typedef struct HYDAnsStream {
    HYDBitWriter *bw;
    uint32_t state;
    size_t num_dists;
    uint8_t cluster_map[256];
    size_t num_clusters;
} HYDAnsStream;

void hyd_init_ans_stream(HYDAnsStream *stream, HYDBitWriter *bw);

#endif /* HYD_ENTROPY_H_ */
