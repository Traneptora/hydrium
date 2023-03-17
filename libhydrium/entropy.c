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

typedef struct StateFlush {
    size_t token_index;
    uint16_t value;
} StateFlush;

static const VLCElement ans_dist_prefix_lengths[14] = {
    {17, 5}, {11, 4}, {15, 4}, {3, 4}, {9, 4},  {7, 4},  {4, 3},
    {2, 3},  {5, 3},  {6, 3},  {0, 3}, {33, 6}, {1, 7}, {65, 7},
};

static HYDStatusCode write_ans_u8(HYDBitWriter *bw, uint8_t b) {
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

static void write_hybrid_uint_configs(HYDEntropyStream *stream, int log_alphabet_size) {
    HYDBitWriter *bw = stream->bw;
    for (size_t i = 0; i < stream->num_clusters; i++) {
        // split_exponent
        hyd_write(bw, 4, 1 + hyd_fllog2(log_alphabet_size));
        // msb_in_token
        hyd_write(bw, 4, 3);
        // lsb_in_token is 0 bits
    }
}

static void write_ans_frequencies(HYDEntropyStream *stream, size_t *frequencies) {
    HYDBitWriter *bw = stream->bw;
    for (size_t i = 0; i < stream->num_clusters; i++) {
        size_t total = 0;
        for (size_t k = 0; k < stream->alphabet_size; k++)
            total += frequencies[i * stream->alphabet_size + k];
        // simple dist and flat dist = 0
        hyd_write(bw, 0, 2);
        // len = 3
        hyd_write(bw, 0x7, 3);
        // shift = 13
        hyd_write(bw, 0x6, 3);
        write_ans_u8(bw, stream->alphabet_size - 3);
        int log_counts[256];
        size_t omit_pos = 0;
        size_t omit_log = 0;
        size_t new_total = 0;
        for (size_t k = 0; k < stream->alphabet_size; k++) {
            size_t index = i * stream->alphabet_size + k;
            frequencies[index] = ((frequencies[index] << 12) / total) & 0xFFFF;
            new_total += frequencies[index];
            if (!omit_pos && frequencies[index])
                omit_pos = index;
        }
        frequencies[omit_pos] += (1 << 12) - new_total;
        for (size_t k = 0; k < stream->alphabet_size; k++) {
            size_t index = i * stream->alphabet_size + k;
            log_counts[k] = frequencies[index] ? 1 + hyd_fllog2(frequencies[index]) : 0;
            hyd_write(bw, ans_dist_prefix_lengths[log_counts[k]].symbol, ans_dist_prefix_lengths[log_counts[k]].length);
            if (log_counts[k] > omit_log) {
                omit_log = log_counts[k];
                omit_pos = k;
            }
        }
        for (size_t k = 0; k < stream->alphabet_size; k++) {
            if (k == omit_pos || log_counts[k] <= 1)
                continue;
            hyd_write(bw, frequencies[k], log_counts[k] - 1);
        }
    }
}

HYDStatusCode hyd_init_entropy_stream(HYDEntropyStream *stream, HYDAllocator *allocator, HYDBitWriter *bw,
                                      size_t symbol_count, const uint8_t *cluster_map, size_t num_dists) {
    memset(stream, 0, sizeof(HYDEntropyStream));
    stream->allocator = allocator;
    stream->bw = bw;
    stream->init_symbol_count = symbol_count;
    stream->alphabet_size = 32;
    stream->cluster_map = HYD_ALLOCA(allocator, num_dists);
    if (!stream->cluster_map)
        return HYD_NOMEM;
    write_cluster_map(stream, cluster_map, num_dists);
    stream->tokens = HYD_ALLOCA(allocator, symbol_count * sizeof(HYDAnsToken));
    if (!stream->tokens) {
        HYD_FREEA(allocator, stream->cluster_map);
        return HYD_NOMEM;
    }
    stream->residues = HYD_ALLOCA(allocator, symbol_count * sizeof(HYDAnsResidue));
    if (!stream->residues) {
        HYD_FREEA(allocator, stream->cluster_map);
        HYD_FREEA(allocator, stream->tokens);
        return HYD_NOMEM;
    }
    return bw->overflow_state;
}

HYDStatusCode hyd_ans_send_symbol(HYDEntropyStream *stream, size_t dist, uint16_t symbol) {
    HYDAnsToken token;
    HYDAnsResidue residue;
    token.cluster = stream->cluster_map[dist];
    if (symbol < 16) {
        token.token = symbol;
        residue.residue = 0;
        residue.bits = 0;
    } else  {
        int n = hyd_fllog2(symbol) - 4;
        token.token = 16 + (((symbol >> n) & 0xF) | (n << 4));
        residue.residue = ~(~UINT16_C(0) << n) & symbol;
        residue.bits = n;
    }
    stream->tokens[stream->symbol_pos] = token;
    stream->residues[stream->symbol_pos++] = residue;
    if (token.token + 1 >= stream->alphabet_size)
        stream->alphabet_size = 1 + token.token;
    if (stream->symbol_pos > stream->init_symbol_count)
        return HYD_INTERNAL_ERROR;
    return HYD_OK;
}

static void generate_alias_mapping(const size_t *frequencies, uint16_t *cutoffs, uint16_t *offsets,
                                   uint16_t *symbols, int alphabet_size, int log_alphabet_size) {
    size_t underfull_pos = 0;
    size_t overfull_pos = 0;
    uint8_t underfull[256];
    uint8_t overfull[256];
    int log_bucket_size = 12 - log_alphabet_size;
    uint16_t bucket_size = 1 << log_bucket_size;
    uint16_t table_size = 1 << log_alphabet_size;
    for (size_t pos = 0; pos < alphabet_size; pos++) {
        cutoffs[pos] = frequencies[pos];
        if (cutoffs[pos] > bucket_size)
            overfull[overfull_pos++] = pos;
        else if (cutoffs[pos] < bucket_size)
            underfull[underfull_pos++] = pos;
    }

    for (uint16_t i = alphabet_size; i < table_size; i++) {
        cutoffs[i] = 0;
        underfull[underfull_pos++] = i;
    }

    while (overfull_pos) {
        uint8_t u = underfull[--underfull_pos];
        uint8_t o = overfull[--overfull_pos];
        int16_t by = bucket_size - cutoffs[u];
        cutoffs[o] -= by;
        symbols[u] = o;
        offsets[u] = cutoffs[o];
        if (cutoffs[o] < bucket_size)
            underfull[underfull_pos++] = o;
        else if (cutoffs[o] > bucket_size)
            overfull[overfull_pos++] = o;
    }

    for (uint16_t sym = 0; sym < table_size; sym++) {
        if (cutoffs[sym] == bucket_size) {
            symbols[sym] = sym;
            cutoffs[sym] = offsets[sym] = 0;
        } else {
            offsets[sym] -= cutoffs[sym];
        }
    }
}

HYDStatusCode hyd_finalize_entropy_stream(HYDEntropyStream *stream) {
    HYDStatusCode ret = HYD_OK;
    HYDBitWriter *bw = stream->bw;
    StateFlush *flushes = NULL;
    int log_alphabet_size = hyd_cllog2(stream->alphabet_size);
    if (log_alphabet_size < 5)
        log_alphabet_size = 5;
    // use prefix codes = false
    hyd_write_bool(bw, 0);
    hyd_write(bw, log_alphabet_size - 5, 2);
    write_hybrid_uint_configs(stream, log_alphabet_size);

    size_t freq_size = stream->num_clusters * stream->alphabet_size * sizeof(size_t);
    size_t *frequencies = HYD_ALLOCA(stream->allocator, freq_size);
    size_t table_size = sizeof(char) << log_alphabet_size;
    size_t alias_table_size = stream->num_clusters * table_size;
    uint16_t *cutoffs = HYD_ALLOCA(stream->allocator, 3 * alias_table_size * sizeof(uint16_t));
    if (!frequencies || !cutoffs) {
        ret = HYD_NOMEM;
        goto end;
    }
    uint16_t *offsets = cutoffs + alias_table_size;
    uint16_t *symbols = offsets + alias_table_size;

    /* populate frequencies */
    memset(frequencies, 0, freq_size);
    for (size_t pos = 0; pos < stream->symbol_pos; pos++)
        frequencies[stream->tokens[pos].cluster * stream->alphabet_size + stream->tokens[pos].token]++;
    write_ans_frequencies(stream, frequencies);

    /* generate alias mappings */
    for (size_t i = 0; i < stream->num_clusters; i++) {
        size_t offset = i * table_size;
        generate_alias_mapping(frequencies + i * stream->alphabet_size, cutoffs + offset, offsets + offset,
                               symbols + offset, stream->alphabet_size, log_alphabet_size);
    }

    uint16_t log_bucket_size = 12 - log_alphabet_size;
    uint16_t pos_mask = ~(~UINT32_C(0) << log_bucket_size);
    flushes = HYD_ALLOCA(stream->allocator, stream->symbol_pos * sizeof(StateFlush));
    if (!flushes) {
        ret = HYD_NOMEM;
        goto end;
    }
    uint32_t state = 0x130000;
    size_t flush_pos = 0;
    for (size_t p2 = 0; p2 < stream->symbol_pos; p2++) {
        size_t p = stream->symbol_pos - p2 - 1;
        uint8_t symbol = stream->tokens[p].token;
        uint8_t cluster = stream->tokens[p].cluster;
        uint16_t freq = frequencies[cluster * stream->alphabet_size + symbol];
        if ((state >> 20) >= freq) {
            flushes[flush_pos++] = (StateFlush){p, state & 0xFFFF};
            state >>= 16;
        }
        uint16_t offset = state % freq;
        uint16_t i = symbol;
        uint16_t pos = offset;
        if (pos >= cutoffs[cluster * table_size + i] || pos > pos_mask) {
            for (i = 0; i < table_size; i++) {
                size_t j = cluster * table_size + i;
                pos = offset - offsets[j];
                if (symbols[j] == symbol && pos <= pos_mask && pos >= cutoffs[j])
                    break;
            }
        }
        if (i == table_size) {
            ret =  HYD_INTERNAL_ERROR;
            goto end;
        }
        state = ((state / freq) << 12) | (i << log_bucket_size) | pos;
    }
    flushes[flush_pos++] = (StateFlush){0, (state >> 16) & 0xFFFF};
    flushes[flush_pos++] = (StateFlush){0, state & 0xFFFF};
    for (size_t p = 0; p < stream->symbol_pos; p++) {
        while (flush_pos > 0 && p >= flushes[flush_pos - 1].token_index)
            hyd_write(bw, flushes[--flush_pos].value, 16);
        hyd_write(bw, stream->residues[p].residue, stream->residues[p].bits);
    }

end:
    HYD_FREEA(stream->allocator, flushes);
    HYD_FREEA(stream->allocator, cutoffs);
    HYD_FREEA(stream->allocator, frequencies);
    HYD_FREEA(stream->allocator, stream->cluster_map);
    HYD_FREEA(stream->allocator, stream->tokens);
    HYD_FREEA(stream->allocator, stream->residues);
    return ret;
}
