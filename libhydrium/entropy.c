#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bitwriter.h"
#include "entropy.h"
#include "internal.h"
#include "osdep.h"

typedef struct VLCElement {
    uint32_t symbol;
    int length;
} VLCElement;

static const VLCElement ans_dist_prefix_lengths[14] = {
    {17, 5}, {11, 4}, {15, 4}, {3, 4}, {9, 4},  {7, 4},  {4, 3},
    {2, 3},  {5, 3},  {6, 3},  {0, 3}, {33, 6}, {1, 7}, {65, 7},
};

static write_ans_u8(HYDBitWriter *bw, uint8_t b) {
    hyd_write_bool(bw, b);
    if (!b)
        return bw->overflow_state;
    int l = hyd_fllog2(b);
    hyd_write(bw, l, 3);
    return hyd_write(bw, b, l);
}

static HYDStatusCode write_cluster_map(HYDEntropyStream *stream, const uint8_t *cluster_map, size_t num_dists) {
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
    
    HYDBitWriter *bw = &stream->encoder->working_writer;

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

static void write_hybrid_uint_configs(HYDEntropyStream *stream, int log_alphabet_size) {
    HYDBitWriter *bw = &stream->encoder->working_writer;
    for (size_t i = 0; i < stream->num_clusters; i++) {
        // split_exponent
        hyd_write(bw, 4, 1 + hyd_fllog2(log_alphabet_size));
        // msb_in_token
        hyd_write(bw, 4, 3);
        // lsb_in_token is 0 bits
    }
}

static void write_ans_frequencies(HYDEntropyStream *stream, size_t *frequencies) {
    size_t alphabet_size = sizeof(char) << stream->log_alphabet_size;
    HYDBitWriter *bw = &stream->encoder->working_writer;
    for (size_t i = 0; i < stream->num_clusters; i++) {
        size_t total = 0;
        for (size_t k = 0; k < alphabet_size; k++)
            total += frequencies[i * alphabet_size + k];
        // simple dist and flat dist = 0
        hyd_write(bw, 0, 2);
        // len = 3
        hyd_write(bw, 0x7, 3);
        // shift = 13
        hyd_write(bw, 0x6, 3);
        write_ans_u8(bw, alphabet_size - 3);
        int log_counts[256];
        size_t omit_pos = 0;
        size_t omit_log = 0;
        for (size_t k = 0; k < alphabet_size; k++) {
            size_t index = i * alphabet_size + k;
            frequencies[index] = ((frequencies[index] << 12) / total) & 0xFFFF;
            log_counts[k] = frequencies[index] ? 1 + hyd_fllog2(frequencies[index]) : 0;
            hyd_write(bw, ans_dist_prefix_lengths[log_counts[k]].symbol, ans_dist_prefix_lengths[log_counts[k]].length);
            if (log_counts[k] > omit_log) {
                omit_log = log_counts[k];
                omit_pos = k;
            }
        }
        for (size_t k = 0; k < alphabet_size; k++) {
            if (k == omit_pos || log_counts[k] <= 1)
                continue;
            hyd_write(bw, frequencies[k], log_counts[k] - 1);
        }
    }
}

HYDStatusCode hyd_init_entropy_stream(HYDEncoder *encoder, HYDEntropyStream *stream, size_t symbol_count,
                                      const uint8_t *cluster_map, size_t num_dists) {
    memset(stream, 0, sizeof(HYDEntropyStream));
    stream->encoder = encoder;
    stream->symbol_count = symbol_count;
    stream->cluster_map = HYD_ALLOC(encoder, num_dists);
    if (!stream->cluster_map)
        return HYD_NOMEM;
    write_cluster_map(stream, cluster_map, num_dists);
    stream->tokens = HYD_ALLOC(encoder, symbol_count * sizeof(HYDAnsToken));
    if (!stream->tokens) {
        HYD_FREE(encoder, stream->cluster_map);
        return HYD_NOMEM;
    }
    stream->residues = HYD_ALLOC(encoder, symbol_count * sizeof(uint16_t));
    if (!stream->residues) {
        HYD_FREE(encoder, stream->cluster_map);
        HYD_FREE(encoder, stream->tokens);
        return HYD_NOMEM;
    }
    return encoder->working_writer.overflow_state;
}

HYDStatusCode hyd_ans_send_symbol(HYDEntropyStream *stream, size_t dist, uint16_t symbol) {
    uint8_t token;
    uint16_t residue;
    if (symbol < 16) {
        token = symbol;
        residue = 0;
    } else  {
        int n = hyd_fllog2(symbol) - 4;
        token = 16 + (((symbol >> n) & 0xF) | (n << 4));
        residue = ~(~UINT16_C(0) << n) & symbol;
    }
    HYDAnsToken clustered_token = {token, stream->cluster_map[dist]};
    stream->tokens[stream->symbol_pos] = clustered_token;
    stream->residues[stream->symbol_pos++] = residue;
    if (token >= (1 << stream->log_alphabet_size))
        stream->log_alphabet_size = 1 + hyd_fllog2(token);
}

static void generate_alias_mapping(const size_t *frequencies, uint16_t *cutoffs, uint16_t *offsets, uint16_t *symbols,
                                   int log_alphabet_size) {
    size_t underfull_pos = 0;
    size_t overfull_pos = 0;
    uint16_t underfull[256];
    uint16_t overfull[256];
    int log_bucket_size = 12 - log_alphabet_size;
    uint16_t bucket_size = 1 << log_bucket_size;
    uint16_t alphabet_size = 1 << log_alphabet_size;
    for (size_t pos = 0; pos < alphabet_size; pos++) {
        cutoffs[pos] = frequencies[pos];
        if (cutoffs[pos] > bucket_size)
            overfull[overfull_pos++] = cutoffs[pos];
        else if (cutoffs[pos] < bucket_size)
            underfull[underfull_pos++] = cutoffs[pos];
    }
    while (overfull_pos > 0) {
        uint16_t u = underfull[--underfull_pos];
        uint16_t o = overfull[--overfull_pos];
        uint16_t by = bucket_size - cutoffs[u];
        offsets[u] = (cutoffs[o] -= by);
        symbols[u] = o;
        if (cutoffs[o] < bucket_size)
            underfull[underfull_pos++] = o;
        else if (cutoffs[o] > bucket_size)
            overfull[overfull_pos++] = o;
    }
    for (uint16_t sym = 0; sym < alphabet_size; sym++) {
        if (cutoffs[sym] == bucket_size) {
            symbols[sym] = sym;
            cutoffs[sym] = offsets[sym] = 0;
        } else {
            offsets[sym] -= cutoffs[sym];
        }
    }
    // uint16_t inv[256];
    // for (uint16_t sym = 0; sym < alphabet_size; sym++)
    //     inv[symbols[sym]] = sym;
    // memcpy(symbols, inv, alphabet_size);
}

HYDStatusCode hyd_finalize_entropy_stream(HYDEntropyStream *stream) {
    HYDStatusCode ret = HYD_OK;
    HYDBitWriter *bw = &stream->encoder->working_writer;
    if (stream->log_alphabet_size < 5)
        stream->log_alphabet_size = 5;
    size_t alphabet_size = sizeof(char) << stream->log_alphabet_size;
    hyd_write(bw, stream->log_alphabet_size - 5, 2);
    write_hybrid_uint_configs(stream, stream->log_alphabet_size);
    size_t freq_size = stream->num_clusters * alphabet_size * sizeof(size_t);
    size_t *frequencies = HYD_ALLOC(stream->encoder, freq_size);
    uint16_t *cutoffs = HYD_ALLOC(stream->encoder, stream->num_clusters * alphabet_size * sizeof(uint16_t));
    uint16_t *offsets = HYD_ALLOC(stream->encoder, stream->num_clusters * alphabet_size * sizeof(uint16_t));
    uint16_t *symbols = HYD_ALLOC(stream->encoder, stream->num_clusters * alphabet_size * sizeof(uint16_t));
    if (!frequencies || !cutoffs || !offsets || !symbols) {
        ret = HYD_NOMEM;
        goto end;
    }
    memset(frequencies, 0, freq_size);
    for (size_t pos = 0; pos < stream->symbol_pos; pos++)
        frequencies[stream->tokens[pos].cluster * alphabet_size + stream->tokens[pos].token]++;
    write_ans_frequencies(stream, frequencies);
    for (size_t i = 0; i < stream->num_clusters; i++) {
        size_t offset = i * alphabet_size;
        generate_alias_mapping(frequencies + offset, cutoffs + offset, offsets + offset, symbols + offset, stream->log_alphabet_size);
    }
    uint32_t state = 0x130000;
    uint16_t log_bucket_size = 12 - stream->log_alphabet_size;
    uint16_t pos_mask = ~(~UINT16_C(0) << log_bucket_size);
    for (size_t p = stream->symbol_pos; p >= 0; p--) {
        uint8_t symbol = stream->tokens[p].token;
        uint8_t cluster = stream->tokens[p].cluster;
        uint16_t freq = frequencies[cluster * alphabet_size + symbol];
        if ((state >> 20) >= freq) {
            hyd_write(bw, state, 16);
            state >>= 16;
        }
        uint16_t offset = state % freq;
        uint16_t i;
        uint16_t pos;
        if (cutoffs[cluster * alphabet_size + symbol] > offset) {
            pos = offset;
            i = symbol;
        } else {
            for (i = 0; i < alphabet_size; i++) {
                size_t j = cluster * alphabet_size + i;
                pos = offset - offsets[j];
                if (symbols[j] == symbol && pos <= pos_mask && pos >= cutoffs[j])
                    break;
            }
        }
        state = ((state / freq) << 12) + ((i << log_bucket_size) | (pos & pos_mask));
    }
    hyd_write(bw, state, 32);
    // TODO hybrid integer

end:
    HYD_FREE(stream->encoder, cutoffs);
    HYD_FREE(stream->encoder, offsets);
    HYD_FREE(stream->encoder, symbols);
    HYD_FREE(stream->encoder, frequencies);
    HYD_FREE(stream->encoder, stream->cluster_map);
    HYD_FREE(stream->encoder, stream->tokens);
    HYD_FREE(stream->encoder, stream->residues);
    return ret;
}