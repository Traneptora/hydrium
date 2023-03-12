#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bitwriter.h"
#include "entropy.h"
#include "osdep.h"

void hyd_init_ans_stream(HYDAnsStream *stream, HYDBitWriter *bw) {
    memset(stream, 0, sizeof(HYDAnsStream));
    stream->bw = bw;
    stream->state = 0x13000;
}

HYDStatusCode hyd_set_cluster_map(HYDAnsStream *stream, const uint8_t *cluster_map, size_t num_dists) {
    if (num_dists > 256 || !num_dists)
        return HYD_INTERNAL_ERROR;
    stream->num_dists = num_dists;
    memcpy(stream->cluster_map, cluster_map, num_dists);
    for (size_t i = 0; i < num_dists; i++) {
        if (stream->cluster_map[i] >= stream->num_clusters)
            stream->num_clusters = stream->cluster_map[i] + 1;
    }
    if (stream->num_clusters > num_dists)
        return HYD_INTERNAL_ERROR;
    
    HYDBitWriter *bw = stream->bw;

    // lz77 = false
    hyd_write_bool(bw, 0);

    if (num_dists == 1)
        return HYD_OK;
    
    int nbits = hyd_cllog2(stream->num_clusters);

    // at least 9 dists
    if (nbits > 3)
        return HYD_INTERNAL_ERROR; // unimplemented

    // simple clustering
    hyd_write_bool(bw, 1);
    hyd_write(bw, nbits, 2);
    for (size_t i = 0; i < num_dists; i++)
        hyd_write(bw, stream->cluster_map[i], nbits);

    return HYD_OK;
}
