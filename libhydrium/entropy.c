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

HYDStatusCode hyd_write_cluster_map(HYDAnsStream *stream, const uint8_t *cluster_map, size_t num_dists) {
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

HYDStatusCode hyd_set_frequencies(HYDAnsStream *stream, const uint16_t **frequencies, int log_alphbet_size) {
    HYDBitWriter *bw = stream->bw;
    if (log_alphbet_size < 5 || log_alphbet_size > 8)
        return HYD_INTERNAL_ERROR;
    hyd_write(bw, log_alphbet_size - 5, 2);
    for (size_t i = 0; i < stream->num_clusters; i++) {
        // split_exponent
        hyd_write(bw, 4, 1 + hyd_fllog2(log_alphbet_size));
        // msb_in_token
        hyd_write(bw, 4, 3);
        // lsb_in_token is 0 bits
    }
    for (size_t i = 0; i < stream->num_clusters; i++) {
        uint32_t total = 0;
        size_t alphabet_size = sizeof(char) << log_alphbet_size;
        for (size_t k = 0; k < alphabet_size; k++)
            total += frequencies[i][k];
        for (size_t k = 0; k < alphabet_size; k++) {
            // simple dist and flat dist = 0
            hyd_write(bw, 0, 2);
        }
    }
}
