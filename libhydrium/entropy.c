#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bitwriter.h"
#include "entropy.h"
#include "internal.h"
#include "math-functions.h"

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

static HYDStatusCode write_cluster_map(HYDEntropyStream *stream) {
    HYDBitWriter *bw = stream->bw;

    if (stream->num_dists == 1)
        return HYD_OK;

    int nbits = hyd_cllog2(stream->num_clusters);

    // at least 9 dists
    if (nbits > 3)
        return HYD_INTERNAL_ERROR;

    // simple clustering
    hyd_write_bool(bw, 1);
    hyd_write(bw, nbits, 2);
    for (size_t i = 0; i < stream->num_dists; i++)
        hyd_write(bw, stream->cluster_map[i], nbits);

    return bw->overflow_state;
}

static void destroy_stream(HYDEntropyStream *stream, void *extra) {
    HYD_FREEA(stream->allocator, extra);
    HYD_FREEA(stream->allocator, stream->frequencies);
    HYD_FREEA(stream->allocator, stream->cluster_map);
    HYD_FREEA(stream->allocator, stream->tokens);
    HYD_FREEA(stream->allocator, stream->residues);
    if (stream->alias_table) {
        for (size_t i = 0; i < stream->alphabet_size * stream->num_clusters; i++)
            HYD_FREEA(stream->allocator, stream->alias_table[i].cutoffs);
    }
    HYD_FREEA(stream->allocator, stream->alias_table);
}

static HYDStatusCode write_hybrid_uint_configs(HYDEntropyStream *stream, int log_alphabet_size) {
    HYDBitWriter *bw = stream->bw;
    for (size_t i = 0; i < stream->num_clusters; i++) {
        // split_exponent
        hyd_write(bw, 4, 1 + hyd_fllog2(log_alphabet_size));
        // msb_in_token
        hyd_write(bw, 4, 3);
        // lsb_in_token is 0 bits
    }

    return bw->overflow_state;
}

static HYDStatusCode generate_alias_mapping(HYDEntropyStream *stream, size_t cluster, int log_alphabet_size, int16_t uniq_pos) {
    int log_bucket_size = 12 - log_alphabet_size;
    uint16_t bucket_size = 1 << log_bucket_size;
    uint16_t table_size = 1 << log_alphabet_size;
    uint16_t symbols[256];
    uint16_t cutoffs[256] = { 0 };
    uint16_t offsets[256];
    HYDAliasEntry *alias_table = stream->alias_table + cluster * stream->alphabet_size;

    if (uniq_pos >= 0) {
        for (uint16_t i = 0; i < table_size; i++) {
            symbols[i] = uniq_pos;
            offsets[i] = i * bucket_size;
        }
        alias_table[uniq_pos].count = table_size;
    } else {
        size_t underfull_pos = 0;
        size_t overfull_pos = 0;
        uint8_t underfull[256];
        uint8_t overfull[256];
        for (size_t pos = 0; pos < stream->alphabet_size; pos++) {
            cutoffs[pos] = stream->frequencies[stream->alphabet_size * cluster + pos];
            if (cutoffs[pos] < bucket_size)
                underfull[underfull_pos++] = pos;
            else if (cutoffs[pos] > bucket_size)
                overfull[overfull_pos++] = pos;
        }

        for (uint16_t i = stream->alphabet_size; i < table_size; i++)
            underfull[underfull_pos++] = i;

        while (overfull_pos) {
            if (!underfull_pos)
                return HYD_INTERNAL_ERROR;
            uint8_t u = underfull[--underfull_pos];
            uint8_t o = overfull[--overfull_pos];
            int16_t by = bucket_size - cutoffs[u];
            offsets[u] = (cutoffs[o] -= by);
            symbols[u] = o;
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
            alias_table[symbols[sym]].count++;
        }
    }

    for (uint16_t sym = 0; sym < stream->alphabet_size; sym++) {
        alias_table[sym].cutoffs = HYD_ALLOCA(stream->allocator, 3 * (alias_table[sym].count + 1) * sizeof(int16_t));
        if (!alias_table[sym].cutoffs)
            return HYD_NOMEM;
        memset(alias_table[sym].cutoffs, -1, 3 * (alias_table[sym].count + 1) * sizeof(int16_t));
        alias_table[sym].offsets = alias_table[sym].cutoffs + alias_table[sym].count + 1;
        alias_table[sym].original = alias_table[sym].offsets + alias_table[sym].count + 1;
        alias_table[sym].offsets[0] = 0;
        alias_table[sym].cutoffs[0] = cutoffs[sym];
        alias_table[sym].original[0] = sym;
    }

    for (uint16_t i = 0; i < table_size; i++) {
        size_t j = 1;
        while (alias_table[symbols[i]].cutoffs[j] >= 0)
            j++;
        alias_table[symbols[i]].cutoffs[j] = cutoffs[i];
        alias_table[symbols[i]].offsets[j] = offsets[i];
        alias_table[symbols[i]].original[j] = i;
    }

    return HYD_OK;
}

static int16_t write_ans_frequencies(HYDEntropyStream *stream, size_t *frequencies) {
    HYDBitWriter *bw = stream->bw;
    size_t total = 0;
    for (size_t k = 0; k < stream->alphabet_size; k++)
        total += frequencies[k];
    size_t new_total = 0;
    int16_t first_pos = -1;
    int16_t second_pos = -1;
    for (size_t k = 0; k < stream->alphabet_size; k++) {
        if (!frequencies[k])
            continue;
        frequencies[k] = ((frequencies[k] << 12) / total) & 0xFFFF;
        if (!frequencies[k])
            frequencies[k] = 1;
        new_total += frequencies[k];
        if (first_pos < 0)
            first_pos = k;
        else if (second_pos < 0)
            second_pos = k;
    }
    if (first_pos < 0)
        return HYD_INTERNAL_ERROR;
    frequencies[first_pos] += (1 << 12) - new_total;
    if (frequencies[first_pos] == 1 << 12) {
        // simple dist
        hyd_write(bw, 0x1, 2);
        write_ans_u8(bw, first_pos);
        return first_pos;
    }
    if (second_pos < 0)
        return HYD_INTERNAL_ERROR;
    if (frequencies[first_pos] + frequencies[second_pos] == 1 << 12) {
        // simple dual peak dist
        hyd_write(bw, 0x3, 2);
        write_ans_u8(bw, first_pos);
        write_ans_u8(bw, second_pos);
        hyd_write(bw, frequencies[first_pos], 12);
        return HYD_DEFAULT;
    }
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
    for (size_t k = 0; k < stream->alphabet_size; k++) {
        log_counts[k] = frequencies[k] ? 1 + hyd_fllog2(frequencies[k]) : 0;
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

    return HYD_DEFAULT;
}

HYDStatusCode hyd_ans_init_stream(HYDEntropyStream *stream, HYDAllocator *allocator, HYDBitWriter *bw,
                                      size_t symbol_count, const uint8_t *cluster_map, size_t num_dists) {
    HYDStatusCode ret;
    memset(stream, 0, sizeof(HYDEntropyStream));
    if (!num_dists || !symbol_count) {
        ret = HYD_INTERNAL_ERROR;
        goto fail;
    }
    stream->num_dists = num_dists;
    stream->allocator = allocator;
    stream->bw = bw;
    stream->init_symbol_count = symbol_count;
    stream->alphabet_size = 32;
    stream->cluster_map = HYD_ALLOCA(allocator, num_dists);
    stream->tokens = HYD_ALLOCA(allocator, symbol_count * sizeof(HYDAnsToken));
    stream->residues = HYD_ALLOCA(allocator, symbol_count * sizeof(HYDAnsResidue));
    if (!stream->cluster_map || !stream->tokens || !stream->residues) {
        ret = HYD_NOMEM;
        goto fail;
    }
    memcpy(stream->cluster_map, cluster_map, num_dists);
    for (size_t i = 0; i < num_dists; i++) {
        if (stream->cluster_map[i] >= stream->num_clusters)
            stream->num_clusters = stream->cluster_map[i] + 1;
    }
    if (stream->num_clusters > num_dists) {
        ret = HYD_INTERNAL_ERROR;
        goto fail;
    }

    return HYD_OK;
fail:
    destroy_stream(stream, NULL);
    return ret;
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
    if (token.token >= stream->alphabet_size)
        stream->alphabet_size = 1 + token.token;
    if (stream->symbol_pos > stream->init_symbol_count)
        return HYD_INTERNAL_ERROR;
    return HYD_OK;
}

HYDStatusCode hyd_ans_write_stream_header(HYDEntropyStream *stream) {
    HYDStatusCode ret = HYD_OK;
    HYDBitWriter *bw = stream->bw;
    int log_alphabet_size = hyd_cllog2(stream->alphabet_size);
    if (log_alphabet_size < 5)
        log_alphabet_size = 5;
    // lz77 = false
    hyd_write_bool(bw, 0);
    if ((ret = write_cluster_map(stream)) < HYD_ERROR_START)
        goto fail;
    // use prefix codes = false
    hyd_write_bool(bw, 0);
    hyd_write(bw, log_alphabet_size - 5, 2);
    if ((ret = write_hybrid_uint_configs(stream, log_alphabet_size)) < HYD_ERROR_START)
        goto fail;

    size_t table_size = stream->num_clusters * stream->alphabet_size;

    /* populate frequencies */
    stream->frequencies = HYD_ALLOCA(stream->allocator, table_size * sizeof(size_t));
    memset(stream->frequencies, 0, table_size * sizeof(size_t));
    for (size_t pos = 0; pos < stream->symbol_pos; pos++)
        stream->frequencies[stream->tokens[pos].cluster * stream->alphabet_size + stream->tokens[pos].token]++;

    /* generate alias mappings */
    stream->alias_table = HYD_ALLOCA(stream->allocator, table_size * sizeof(HYDAliasEntry));
    memset(stream->alias_table, 0, table_size * sizeof(HYDAliasEntry));
    for (size_t i = 0; i < stream->num_clusters; i++) {
        int16_t uniq_pos = write_ans_frequencies(stream, stream->frequencies + i * stream->alphabet_size);
        if (uniq_pos < HYD_ERROR_START) {
            ret = uniq_pos;
            goto fail;
        }
        ret = generate_alias_mapping(stream, i, log_alphabet_size, uniq_pos);
        if (ret < HYD_ERROR_START)
            goto fail;
    }

    return bw->overflow_state;

fail:
    destroy_stream(stream, NULL);
    return ret;
}

HYDStatusCode hyd_ans_finalize_stream(HYDEntropyStream *stream) {
    HYDStatusCode ret = HYD_OK;
    StateFlush *flushes = NULL;
    HYDBitWriter *bw = stream->bw;
    const int log_alphabet_size = hyd_cllog2(stream->alphabet_size);
    const uint16_t log_bucket_size = 12 - log_alphabet_size;
    const uint16_t pos_mask = ~(~UINT32_C(0) << log_bucket_size);
    flushes = HYD_ALLOCA(stream->allocator, stream->symbol_pos * sizeof(StateFlush));
    if (!flushes) {
        ret = HYD_NOMEM;
        goto end;
    }
    uint32_t state = 0x130000;
    size_t flush_pos = 0;
    for (size_t p2 = 0; p2 < stream->symbol_pos; p2++) {
        const size_t p = stream->symbol_pos - p2 - 1;
        const uint8_t symbol = stream->tokens[p].token;
        const uint8_t cluster = stream->tokens[p].cluster;
        const size_t index = cluster * stream->alphabet_size + symbol;
        const uint16_t freq = stream->frequencies[index];
        if ((state >> 20) >= freq) {
            flushes[flush_pos++] = (StateFlush){p, state & 0xFFFF};
            state >>= 16;
        }
        const uint16_t offset = state % freq;
        uint16_t i, pos, j;
        for (j = 0; j <= stream->alias_table[index].count; j++) {
            pos = offset - stream->alias_table[index].offsets[j];
            int16_t k = pos - stream->alias_table[index].cutoffs[j];
            if (pos <= pos_mask && (j > 0 ? k >= 0 : k < 0)) {
                i = stream->alias_table[index].original[j];
                break;
            }
        }
        if (j > stream->alias_table[index].count) {
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

    ret = bw->overflow_state;

end:
    destroy_stream(stream, flushes);
    return ret;
}
