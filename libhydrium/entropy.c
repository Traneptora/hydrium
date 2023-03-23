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

static const HYDHybridUintConfig lz77_len_conf = {7, 0, 0};

static HYDStatusCode write_ans_u8(HYDBitWriter *bw, uint8_t b) {
    hyd_write_bool(bw, b);
    if (!b)
        return bw->overflow_state;
    int l = hyd_fllog2(b);
    hyd_write(bw, l, 3);
    return hyd_write(bw, b, l);
}

static void destroy_stream(HYDEntropyStream *stream, void *extra) {
    HYD_FREEA(stream->allocator, extra);
    HYD_FREEA(stream->allocator, stream->frequencies);
    HYD_FREEA(stream->allocator, stream->cluster_map);
    HYD_FREEA(stream->allocator, stream->tokens);
    HYD_FREEA(stream->allocator, stream->residues);
    if (stream->alias_table) {
        for (size_t i = 0; i < stream->max_alphabet_size * stream->num_clusters; i++)
            HYD_FREEA(stream->allocator, stream->alias_table[i].cutoffs);
    }
    HYD_FREEA(stream->allocator, stream->alias_table);
    HYD_FREEA(stream->allocator, stream->configs);
}

HYDStatusCode hyd_entropy_set_hybrid_config(HYDEntropyStream *stream, uint8_t min_cluster, uint8_t to_cluster,
                                         int split_exponent, int msb_in_token, int lsb_in_token) {
    if (min_cluster >= to_cluster)
        return HYD_INTERNAL_ERROR;

    for (uint8_t j = min_cluster; j < to_cluster && j < stream->num_clusters; j++) {
        stream->configs[j].split_exponent = split_exponent;
        stream->configs[j].msb_in_token = msb_in_token;
        stream->configs[j].lsb_in_token = lsb_in_token;
    }

    return HYD_OK;
}

static HYDStatusCode write_cluster_map(HYDEntropyStream *stream) {
    HYDBitWriter *bw = stream->bw;

    if (stream->num_dists == 1)
        return HYD_OK;

    int nbits = hyd_cllog2(stream->num_clusters);

    if (nbits <= 3 && stream->num_dists * nbits <= 32) {
        // simple clustering
        hyd_write_bool(bw, 1);
        hyd_write(bw, nbits, 2);
        for (size_t i = 0; i < stream->num_dists; i++)
            hyd_write(bw, stream->cluster_map[i], nbits);
        return bw->overflow_state;
    }

    HYDEntropyStream nested;
    HYDStatusCode ret;
    // non simple clustering
    hyd_write_bool(bw, 0);
    // use mtf = true
    hyd_write_bool(bw, 1);
    ret = hyd_entropy_init_stream(&nested, stream->allocator, stream->bw, stream->num_dists, (const uint8_t[]){0, 0},
        1, 0, 32);
    if (ret < HYD_ERROR_START)
        goto fail;
    uint8_t mtf[256];
    for (int i = 0; i < 256; i++)
        mtf[i] = i;
    for (uint16_t j = 0; j < stream->num_dists; j++) {
        uint8_t index = 0;
        for (int k = 0; k < 256; k++) {
            if (mtf[k] == stream->cluster_map[j]) {
                index = k;
                break;
            }
        }
        if ((ret = hyd_entropy_send_symbol(&nested, 0, index)) < HYD_ERROR_START)
            goto fail;
        if (index) {
            memmove(mtf + 1, mtf, index);
            mtf[0] = index;
        }
    }

    if ((ret = hyd_ans_write_stream_header(&nested)) < HYD_ERROR_START)
        goto fail;
    if ((ret = hyd_ans_finalize_stream(&nested)) < HYD_ERROR_START)
        goto fail;

    return bw->overflow_state;

fail:
    destroy_stream(&nested, NULL);
    return ret;
}

static HYDStatusCode write_hybrid_uint_config(HYDEntropyStream *stream, const HYDHybridUintConfig *config,
                                              int log_alphabet_size) {
    HYDBitWriter *bw = stream->bw;

    // split_exponent
    hyd_write(bw, config->split_exponent, hyd_cllog2(1 + log_alphabet_size));
    // msb_in_token
    hyd_write(bw, config->msb_in_token,
        hyd_cllog2(1 + config->split_exponent));
    // lsb_in_token is 0 bits
    hyd_write(bw, config->lsb_in_token,
        hyd_cllog2(1 + config->split_exponent - config->msb_in_token));
    return bw->overflow_state;
}

static HYDStatusCode generate_alias_mapping(HYDEntropyStream *stream, size_t cluster, int log_alphabet_size, int16_t uniq_pos) {
    int log_bucket_size = 12 - log_alphabet_size;
    uint16_t bucket_size = 1 << log_bucket_size;
    uint16_t table_size = 1 << log_alphabet_size;
    uint16_t symbols[256];
    uint16_t cutoffs[256] = { 0 };
    uint16_t offsets[256];
    HYDAliasEntry *alias_table = stream->alias_table + cluster * stream->max_alphabet_size;

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
        for (size_t pos = 0; pos < stream->max_alphabet_size; pos++) {
            cutoffs[pos] = stream->frequencies[stream->max_alphabet_size * cluster + pos];
            if (cutoffs[pos] < bucket_size)
                underfull[underfull_pos++] = pos;
            else if (cutoffs[pos] > bucket_size)
                overfull[overfull_pos++] = pos;
        }

        for (uint16_t i = stream->max_alphabet_size; i < table_size; i++)
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

    for (uint16_t sym = 0; sym < stream->max_alphabet_size; sym++) {
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
    for (size_t k = 0; k < stream->max_alphabet_size; k++)
        total += frequencies[k];
    size_t new_total = 0;
    int16_t first_pos = -1;
    int16_t second_pos = -1;
    for (size_t k = 0; k < stream->max_alphabet_size; k++) {
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

    // empty cluster
    if (first_pos < 0)
        first_pos = 0;

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
    write_ans_u8(bw, stream->max_alphabet_size - 3);
    int log_counts[256];
    size_t omit_pos = 0;
    size_t omit_log = 0;
    for (size_t k = 0; k < stream->max_alphabet_size; k++) {
        log_counts[k] = frequencies[k] ? 1 + hyd_fllog2(frequencies[k]) : 0;
        hyd_write(bw, ans_dist_prefix_lengths[log_counts[k]].symbol, ans_dist_prefix_lengths[log_counts[k]].length);
        if (log_counts[k] > omit_log) {
            omit_log = log_counts[k];
            omit_pos = k;
        }
    }
    for (size_t k = 0; k < stream->max_alphabet_size; k++) {
        if (k == omit_pos || log_counts[k] <= 1)
            continue;
        hyd_write(bw, frequencies[k], log_counts[k] - 1);
    }

    return HYD_DEFAULT;
}

HYDStatusCode hyd_entropy_init_stream(HYDEntropyStream *stream, HYDAllocator *allocator, HYDBitWriter *bw,
                                  size_t symbol_count, const uint8_t *cluster_map, size_t num_dists,
                                  int custom_configs, uint32_t lz77_min_symbol) {
    HYDStatusCode ret;
    memset(stream, 0, sizeof(HYDEntropyStream));
    if (!num_dists || !symbol_count) {
        ret = HYD_INTERNAL_ERROR;
        goto fail;
    }
    if (lz77_min_symbol) {
        num_dists++;
        stream->lz77_min_length = 3;
        stream->lz77_min_symbol = lz77_min_symbol;
    }
    stream->num_dists = num_dists;
    stream->allocator = allocator;
    stream->bw = bw;
    stream->init_symbol_count = symbol_count;
    stream->max_alphabet_size = 1;
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

    stream->configs = HYD_ALLOCA(allocator, stream->num_clusters * sizeof(HYDHybridUintConfig));
    if (!stream->configs) {
        ret = HYD_NOMEM;
        goto fail;
    }

    if (!custom_configs) {
        hyd_entropy_set_hybrid_config(stream, 0, stream->num_clusters - !!stream->lz77_min_symbol, 4, 1, 1);
        if (stream->lz77_min_symbol)
            hyd_entropy_set_hybrid_config(stream, stream->num_clusters - 1, stream->num_clusters, 4, 0, 0);
    }

    return HYD_OK;
fail:
    destroy_stream(stream, NULL);
    return ret;
}

static void hybridize(uint32_t symbol, uint8_t *token, HYDAnsResidue *residue, const HYDHybridUintConfig *config) {
    int split = 1 << config->split_exponent;
    if (symbol < split) {
        *token = symbol;
        residue->bits = residue->residue = 0;
    } else {
        uint32_t n = hyd_fllog2(symbol) - config->lsb_in_token - config->msb_in_token;
        uint32_t low = symbol & ~(~UINT32_C(0) << config->lsb_in_token);
        symbol >>= config->lsb_in_token;
        residue->residue = symbol & ~(~UINT32_C(0) << n);
        symbol >>= n;
        uint32_t high = symbol & ~(~UINT32_C(0) << config->msb_in_token);
        residue->bits = n;
        *token = split + (low | (high << config->lsb_in_token) |
                        ((n - config->split_exponent + config->lsb_in_token + config->msb_in_token) <<
                        (config->msb_in_token + config->lsb_in_token)));
    }
}

static HYDStatusCode send_hybridized_symbol(HYDEntropyStream *stream, const HYDAnsToken *token,
                                            const HYDAnsResidue *residue) {
    stream->tokens[stream->symbol_pos] = *token;
    stream->residues[stream->symbol_pos] = *residue;
    if (token->token >= stream->max_alphabet_size)
        stream->max_alphabet_size = 1 + token->token;
    if (++stream->symbol_pos > stream->init_symbol_count)
        return HYD_INTERNAL_ERROR;
    return HYD_OK;
}

static HYDStatusCode send_entropy_symbol0(HYDEntropyStream *stream, size_t dist, uint32_t symbol,
                                          const HYDHybridUintConfig *extra_config) {
    HYDAnsToken token;
    HYDAnsResidue residue;
    token.cluster = stream->cluster_map[dist];
    const HYDHybridUintConfig *config = extra_config ? extra_config : &stream->configs[token.cluster];
    hybridize(symbol, &token.token, &residue, config);
    return send_hybridized_symbol(stream, &token, &residue);
}

static HYDStatusCode flush_lz77(HYDEntropyStream *stream, size_t dist) {
    HYDStatusCode ret;
    uint32_t last_symbol = stream->last_symbol - 1;

    if (stream->lz77_rle_count > stream->lz77_min_length) {
        uint32_t repeat_count = stream->lz77_rle_count - stream->lz77_min_length;
        HYDAnsToken token;
        HYDAnsResidue residue;
        hybridize(repeat_count, &token.token, &residue, &lz77_len_conf);
        token.cluster = stream->cluster_map[dist];
        token.token += stream->lz77_min_symbol;
        if ((ret = send_hybridized_symbol(stream, &token, &residue)) < HYD_ERROR_START)
            return ret;
        if ((ret = send_entropy_symbol0(stream, stream->num_clusters - 1, 0, NULL)) < HYD_ERROR_START)
            return ret;
    } else if (stream->last_symbol) {
        for (uint32_t k = 0; k < stream->lz77_rle_count; k++) {
            if ((ret = send_entropy_symbol0(stream, dist, last_symbol, NULL)) < HYD_ERROR_START)
                return ret;
        }
    }

    stream->lz77_rle_count = 0;

    return HYD_OK;
}

HYDStatusCode hyd_entropy_send_symbol(HYDEntropyStream *stream, size_t dist, uint32_t symbol) {
    HYDStatusCode ret = HYD_OK;

    if (!stream->lz77_min_symbol)
        return send_entropy_symbol0(stream, dist, symbol, NULL);

    if (stream->last_symbol == symbol + 1) {
        if (++stream->lz77_rle_count < 128)
            return HYD_OK;
        stream->lz77_rle_count--;
    }

    if ((ret = flush_lz77(stream, dist)) < HYD_ERROR_START)
        return ret;

    stream->last_symbol = symbol + 1;

    return send_entropy_symbol0(stream, dist, symbol, NULL);
}

HYDStatusCode hyd_ans_write_stream_header(HYDEntropyStream *stream) {
    HYDStatusCode ret = HYD_OK;
    HYDBitWriter *bw = stream->bw;
    int log_alphabet_size = hyd_cllog2(stream->max_alphabet_size);
    if (log_alphabet_size < 5)
        log_alphabet_size = 5;
    hyd_write_bool(bw, stream->lz77_min_symbol);
    if (stream->lz77_min_symbol) {
        flush_lz77(stream, 0);
        hyd_write_u32(bw, (const uint32_t[4]){224, 512, 4096, 8}, (const uint32_t[4]){0, 0, 0, 15},
            stream->lz77_min_symbol);
        hyd_write_u32(bw, (const uint32_t[4]){3, 4, 5, 9}, (const uint32_t[4]){0, 0, 2, 8},
            stream->lz77_min_length);
        write_hybrid_uint_config(stream, &lz77_len_conf, 8);
    }
    if ((ret = write_cluster_map(stream)) < HYD_ERROR_START)
        goto fail;
    // use prefix codes = false
    hyd_write_bool(bw, 0);
    hyd_write(bw, log_alphabet_size - 5, 2);
    for (size_t i = 0; i < stream->num_clusters; i++) {
        if ((ret = write_hybrid_uint_config(stream, &stream->configs[i], log_alphabet_size)) < HYD_ERROR_START)
            goto fail;
    }

    size_t table_size = stream->num_clusters * stream->max_alphabet_size;

    /* populate frequencies */
    stream->frequencies = HYD_ALLOCA(stream->allocator, table_size * sizeof(size_t));
    memset(stream->frequencies, 0, table_size * sizeof(size_t));
    for (size_t pos = 0; pos < stream->symbol_pos; pos++)
        stream->frequencies[stream->tokens[pos].cluster * stream->max_alphabet_size + stream->tokens[pos].token]++;
    /* generate alias mappings */
    stream->alias_table = HYD_ALLOCA(stream->allocator, table_size * sizeof(HYDAliasEntry));
    memset(stream->alias_table, 0, table_size * sizeof(HYDAliasEntry));
    for (size_t i = 0; i < stream->num_clusters; i++) {
        int16_t uniq_pos = write_ans_frequencies(stream, stream->frequencies + i * stream->max_alphabet_size);
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
    int log_alphabet_size = hyd_cllog2(stream->max_alphabet_size);
    if (log_alphabet_size < 5)
        log_alphabet_size = 5;
    const uint16_t log_bucket_size = 12 - log_alphabet_size;
    const uint16_t pos_mask = ~(~UINT32_C(0) << log_bucket_size);
    if (!stream->alias_table) {
        ret = HYD_INTERNAL_ERROR;
        goto end;
    }
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
        const size_t index = cluster * stream->max_alphabet_size + symbol;
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
            ret = HYD_INTERNAL_ERROR;
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
