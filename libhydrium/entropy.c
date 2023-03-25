#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bitwriter.h"
#include "entropy.h"
#include "internal.h"
#include "math-functions.h"

typedef struct StateFlush {
    size_t token_index;
    uint16_t value;
} StateFlush;

typedef struct FrequencyEntry {
    int32_t token;
    size_t frequency;
    int32_t depth;
    int32_t max_depth;
    struct FrequencyEntry *left_child;
    struct FrequencyEntry *right_child;
} FrequencyEntry;

static const HYDVLCElement ans_dist_prefix_lengths[14] = {
    {17, 5}, {11, 4}, {15, 4}, {3, 4}, {9, 4},  {7, 4},  {4, 3},
    {2, 3},  {5, 3},  {6, 3},  {0, 3}, {33, 6}, {1, 7}, {65, 7},
};

static const HYDHybridUintConfig lz77_len_conf = {7, 0, 0};

static const uint32_t prefix_zig_zag[18] = {1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15};

static const HYDVLCElement prefix_level0_table[6] = {
    {0, 2}, {7, 4}, {3, 3}, {2, 2}, {1, 2}, {15, 4},
};

static const uint32_t br_lut[16] = {
    0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
    0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF,
};

static uint32_t bit_reverse(const uint32_t b) {
    uint32_t c = 0;
    for (unsigned i = 0; i < 32; i += 4)
        c |= br_lut[(b >> i) & 0xF] << (28 - i);
    return c;
}

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
    HYD_FREEA(stream->allocator, stream->configs);
    HYD_FREEA(stream->allocator, stream->alphabet_sizes);
    if (stream->alias_table) {
        for (size_t i = 0; i < stream->max_alphabet_size * stream->num_clusters; i++)
            HYD_FREEA(stream->allocator, stream->alias_table[i].cutoffs);
    }
    HYD_FREEA(stream->allocator, stream->alias_table);
    HYD_FREEA(stream->allocator, stream->vlc_table);
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
    ret = hyd_entropy_init_stream(&nested, stream->allocator, bw, stream->num_dists, (const uint8_t[]){0},
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
    if ((ret = hyd_prefix_write_stream_header(&nested)) < HYD_ERROR_START)
        goto fail;
    if ((ret = hyd_prefix_finalize_stream(&nested)) < HYD_ERROR_START)
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
    if (config->split_exponent == log_alphabet_size)
        return bw->overflow_state;
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
    uint16_t symbols[256] = { 0 };
    uint16_t cutoffs[256] = { 0 };
    uint16_t offsets[256] = { 0 };
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

static int16_t write_ans_frequencies(HYDEntropyStream *stream, uint16_t *frequencies) {
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
        frequencies[k] = (((uint32_t)frequencies[k] << 12) / total) & 0xFFFF;
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
    stream->cluster_map = HYD_ALLOCA(allocator, num_dists);
    stream->tokens = HYD_ALLOCA(allocator, symbol_count * sizeof(HYDAnsToken));
    stream->residues = HYD_ALLOCA(allocator, symbol_count * sizeof(HYDAnsResidue));
    if (!stream->cluster_map || !stream->tokens || !stream->residues) {
        ret = HYD_NOMEM;
        goto fail;
    }
    memcpy(stream->cluster_map, cluster_map, num_dists - !!lz77_min_symbol);
    for (size_t i = 0; i < num_dists - !!lz77_min_symbol; i++) {
        if (stream->cluster_map[i] >= stream->num_clusters)
            stream->num_clusters = stream->cluster_map[i] + 1;
    }
    if (stream->num_clusters > num_dists) {
        ret = HYD_INTERNAL_ERROR;
        goto fail;
    }

    if (lz77_min_symbol)
        stream->cluster_map[num_dists - 1] = stream->num_clusters++;

    stream->configs = HYD_ALLOCA(allocator, stream->num_clusters * sizeof(HYDHybridUintConfig));
    stream->alphabet_sizes = HYD_ALLOCA(allocator, stream->num_clusters * sizeof(uint16_t));
    if (!stream->configs || !stream->alphabet_sizes) {
        ret = HYD_NOMEM;
        goto fail;
    }
    memset(stream->alphabet_sizes, 0, stream->num_clusters * sizeof(uint16_t));

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
    if (token->token >= stream->alphabet_sizes[token->cluster])
        stream->alphabet_sizes[token->cluster] = 1 + token->token;
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

static HYDStatusCode stream_header_common(HYDEntropyStream *stream, int *las, int prefix_codes) {
    HYDStatusCode ret = HYD_OK;
    HYDBitWriter *bw = stream->bw;
    int log_alphabet_size = hyd_cllog2(stream->max_alphabet_size);
    if (log_alphabet_size < 5)
        log_alphabet_size = 5;
    *las = log_alphabet_size;
    hyd_write_bool(bw, stream->lz77_min_symbol);
    if (stream->lz77_min_symbol) {
        flush_lz77(stream, 0);
        hyd_write_u32(bw, (const uint32_t[4]){224, 512, 4096, 8}, (const uint32_t[4]){0, 0, 0, 15},
            stream->lz77_min_symbol);
        hyd_write_u32(bw, (const uint32_t[4]){3, 4, 5, 9}, (const uint32_t[4]){0, 0, 2, 8},
            stream->lz77_min_length);
        ret = write_hybrid_uint_config(stream, &lz77_len_conf, 8);
    }
    if (ret < HYD_ERROR_START)
        return ret;
    if ((ret = write_cluster_map(stream)) < HYD_ERROR_START)
        return ret;

    hyd_write_bool(bw, prefix_codes);
    if (!prefix_codes)
        hyd_write(bw, log_alphabet_size - 5, 2);

    for (size_t i = 0; i < stream->num_clusters; i++) {
        ret = write_hybrid_uint_config(stream, &stream->configs[i], prefix_codes ? 15 : log_alphabet_size);
        if (ret < HYD_ERROR_START)
            return ret;
    }

    size_t table_size = stream->num_clusters * stream->max_alphabet_size;

    /* populate frequencies */
    stream->frequencies = HYD_ALLOCA(stream->allocator, table_size * sizeof(uint16_t));
    if (!stream->frequencies)
        return HYD_NOMEM;
    memset(stream->frequencies, 0, table_size * sizeof(uint16_t));
    for (size_t pos = 0; pos < stream->symbol_pos; pos++)
        stream->frequencies[stream->tokens[pos].cluster * stream->max_alphabet_size + stream->tokens[pos].token]++;

    return bw->overflow_state;
}

static int symbol_compare(const void *a, const void *b) {
    const HYDVLCElement *vlc_a = a;
    const HYDVLCElement *vlc_b = b;
    if (vlc_a->length == vlc_b->length)
        return vlc_a->symbol - vlc_b->symbol;

    return !vlc_b->length ? -1 : !vlc_a->length ? 1 : vlc_a->length - vlc_b->length;
}

static int huffman_compare(const void *a, const void *b) {
    const FrequencyEntry *fa = a;
    const FrequencyEntry *fb = b;
    const ptrdiff_t pb = fb->frequency;
    const ptrdiff_t pa = fa->frequency;
    const int32_t ta = fa->token;
    const int32_t tb = fb->token;
    return pa != pb ? (!pb ? -1 : !pa ? 1 : pa - pb) : (!tb ? -1 : !ta ? 1 : ta - tb);
}

static int32_t collect(FrequencyEntry *entry) {
    if (!entry)
        return 0;
    int32_t self = ++entry->depth;
    int32_t left = collect(entry->left_child);
    int32_t right = collect(entry->right_child);
    return entry->max_depth = hyd_max3(self, left, right);
}

static HYDStatusCode build_huffman_tree(HYDAllocator *allocator, const uint16_t *frequencies,
                                        uint16_t *lengths, uint32_t alphabet_size, int32_t max_depth) {
    HYDStatusCode ret = HYD_OK;
    FrequencyEntry *tree = HYD_ALLOCA(allocator, (2 * alphabet_size - 1) * sizeof(FrequencyEntry));
    if (!tree) {
        ret = HYD_NOMEM;
        goto fail;
    }

    uint32_t count = 0;
    for (uint32_t token = 0; token < alphabet_size; token++) {
        tree[token].frequency = frequencies[token];
        tree[token].token = 1 + token;
        tree[token].left_child = tree[token].right_child = NULL;
        tree[token].depth = 0;
        tree[token].max_depth = 0;
        if (tree[token].frequency)
            count++;
    }
    memset(tree + alphabet_size, 0, (alphabet_size - 1) * sizeof(FrequencyEntry));
    for (uint32_t k = 0; k < alphabet_size - 1; k++) {
        qsort(tree + 2 * k, alphabet_size - k, sizeof(FrequencyEntry), &huffman_compare);
        FrequencyEntry *smallest = &tree[2 * k];
        FrequencyEntry *second = &tree[2 * k + 1];
        if (!second->frequency)
            break;
        if (max_depth > 0) {
            int32_t target = max_depth - hyd_cllog2(count);
            while (smallest->max_depth >= target || !smallest->frequency)
                smallest = second++;
            while (second->max_depth >= target || !second->frequency)
                second++;
            hyd_swap(FrequencyEntry, *smallest, tree[2 * k]);
            hyd_swap(FrequencyEntry, *second, tree[2 * k + 1]);
            smallest = tree + 2 * k;
            second = smallest + 1;
        }
        FrequencyEntry *entry = &tree[alphabet_size + k];
        entry->frequency = smallest->frequency + second->frequency;
        entry->left_child = smallest;
        entry->right_child = second;
        collect(entry);
        count--;
    }

    for (uint32_t j = 0; j < 2 * alphabet_size - 1; j++) {
        if (tree[j].token > 0)
            lengths[tree[j].token - 1] = tree[j].depth;
    }

    HYD_FREEA(allocator, tree);
    return HYD_OK;

fail:
    HYD_FREEA(allocator, tree);
    return ret;
}

static HYDStatusCode build_prefix_table(HYDAllocator *allocator, HYDVLCElement *table,
                                        const uint16_t *lengths, uint32_t alphabet_size) {
    HYDStatusCode ret = HYD_OK;
    HYDVLCElement *pre_table = HYD_ALLOCA(allocator, alphabet_size * sizeof(HYDVLCElement));
    if (!pre_table)
        return HYD_NOMEM;

    for (int32_t j = 0; j < alphabet_size; j++) {
        pre_table[j].length = lengths[j];
        pre_table[j].symbol = j;
    }

    qsort(pre_table, alphabet_size, sizeof(HYDVLCElement), &symbol_compare);

    uint64_t code = 0;
    for (int32_t j = 0; j < alphabet_size; j++) {
        int l = pre_table[j].length;
        if (!l)
            continue;
        uint32_t s = pre_table[j].symbol;
        table[s].symbol = bit_reverse(code);
        table[s].length = l;
        code += UINT64_C(1) << (32 - l);
    }

    if (code && code != (UINT64_C(1) << 32))
        ret = HYD_INTERNAL_ERROR;

    HYD_FREEA(allocator, pre_table);
    return ret;
}

static void flush_zeroes(HYDBitWriter *bw, const HYDVLCElement *level1_table, uint32_t num_zeroes) {
    if (num_zeroes >= 3) {
        int32_t k = 0;
        uint32_t nz_residues[8];
        while (num_zeroes > 10) {
            uint32_t new_num_zeroes = (num_zeroes + 13) / 8;
            nz_residues[k++] = num_zeroes - 8 * new_num_zeroes + 16;
            num_zeroes = new_num_zeroes;
        }
        nz_residues[k++] = num_zeroes;
        for (int32_t l = k - 1; l >= 0; l--) {
            hyd_write(bw, level1_table[17].symbol, level1_table[17].length);
            uint32_t res = nz_residues[l];
            hyd_write(bw, res - 3, 3);
        }
    } else if (num_zeroes) {
        for (uint32_t k = 0; k < num_zeroes; k++)
            hyd_write(bw, level1_table[0].symbol, level1_table[0].length);
    }
}

static HYDStatusCode write_complex_prefix_lengths(HYDEntropyStream *stream, uint32_t alphabet_size,
                                                  const uint16_t *lengths) {
    HYDBitWriter *bw = stream->bw;
    HYDStatusCode ret = HYD_OK;
    HYDVLCElement *level1_table = NULL;

    // hskip = 0
    hyd_write(bw, 0, 2);

    uint16_t level1_freqs[18] = { 0 };

    uint32_t num_zeroes = 0;
    for (uint32_t j = 0; j < alphabet_size; j++) {
        uint32_t code = lengths[j];
        if (!code) {
            num_zeroes++;
            continue;
        }
        if (num_zeroes >= 3) {
            while (num_zeroes > 10) {
                level1_freqs[17]++;
                num_zeroes = (num_zeroes + 13) / 8;
            }
            level1_freqs[17]++;
        } else {
            level1_freqs[0] += num_zeroes;
        }
        num_zeroes = 0;
        level1_freqs[code]++;
    }

    uint16_t level1_lengths[18] = { 0 };
    ret = build_huffman_tree(stream->allocator, level1_freqs, level1_lengths, 18, 5);
    if (ret < HYD_ERROR_START)
        goto end;

    uint32_t total_code = 0;
    for (uint32_t j = 0; j < 18; j++) {
        uint32_t code = level1_lengths[prefix_zig_zag[j]];
        hyd_write(bw, prefix_level0_table[code].symbol,
                      prefix_level0_table[code].length);
        if (code)
            total_code += 32 >> code;
        if (total_code >= 32)
            break;
    }
    if (total_code && total_code != 32) {
        ret = HYD_INTERNAL_ERROR;
        goto end;
    }

    level1_table = HYD_ALLOCA(stream->allocator, 18 * sizeof(HYDVLCElement));
    if (!level1_table) {
        ret = HYD_NOMEM;
        goto end;
    }
    memset(level1_table, 0, 18 * sizeof(HYDVLCElement));

    ret = build_prefix_table(stream->allocator, level1_table, level1_lengths, 18);
    if (ret < HYD_ERROR_START)
        goto end;

    total_code = 0;
    num_zeroes = 0;
    for (uint32_t j = 0; j < alphabet_size; j++) {
        uint32_t code = lengths[j];
        if (!code) {
            num_zeroes++;
            continue;
        }
        flush_zeroes(bw, level1_table, num_zeroes);
        num_zeroes = 0;
        hyd_write(bw, level1_table[code].symbol, level1_table[code].length);
        total_code += 32768 >> code;
        if (total_code == 32768)
            break;
    }
    flush_zeroes(bw, level1_table, num_zeroes);

end:
    HYD_FREEA(stream->allocator, level1_table);
    return ret;
}

HYDStatusCode hyd_prefix_write_stream_header(HYDEntropyStream *stream) {
    HYDStatusCode ret;
    int log_alphabet_size;
    HYDBitWriter *bw = stream->bw;
    uint16_t *global_lengths = NULL;

    if ((ret = stream_header_common(stream, &log_alphabet_size, 1)) < HYD_ERROR_START)
        goto fail;

    for (size_t i = 0; i < stream->num_clusters; i++) {
        if (stream->alphabet_sizes[i] <= 1) {
            hyd_write_bool(bw, 0);
            continue;
        }
        hyd_write_bool(bw, 1);
        int n = hyd_fllog2(stream->alphabet_sizes[i] - 1);
        hyd_write(bw, n, 4);
        hyd_write(bw, stream->alphabet_sizes[i] - 1, n);
    }

    size_t table_size = stream->num_clusters * stream->max_alphabet_size;

    global_lengths = HYD_ALLOCA(stream->allocator, table_size * sizeof(uint16_t));
    stream->vlc_table = HYD_ALLOCA(stream->allocator, table_size * sizeof(HYDVLCElement));
    if (!global_lengths || !stream->vlc_table) {
        ret = HYD_NOMEM;
        goto fail;
    }
    memset(global_lengths, 0, table_size * sizeof(uint16_t));
    memset(stream->vlc_table, 0, table_size * sizeof(HYDVLCElement));

    for (size_t i = 0; i < stream->num_clusters; i++) {
        if (stream->alphabet_sizes[i] <= 1)
            continue;
        uint16_t *lengths = global_lengths + i * stream->max_alphabet_size;
        const uint16_t *freqs = stream->frequencies + i * stream->max_alphabet_size;
        if ((ret = build_huffman_tree(stream->allocator, freqs, lengths, stream->alphabet_sizes[i], 15)) < HYD_ERROR_START)
            return ret;
        uint32_t nsym = 0;
        HYDVLCElement tokens[4] = { 0 };
        for (uint32_t j = 0; j < stream->alphabet_sizes[i]; j++) {
            if (lengths[j]) {
                if (nsym < 4) {
                    tokens[nsym].symbol = j;
                    tokens[nsym].length = lengths[j];
                }
                nsym++;
            }
        }

        if (nsym > 4) {
            if ((ret = write_complex_prefix_lengths(stream, stream->alphabet_sizes[i], lengths)) < HYD_ERROR_START)
                goto fail;
            continue;
        }

        if (nsym == 0) {
            nsym = 1;
            tokens[0].symbol = stream->alphabet_sizes[i] - 1;
        }

        // hskip = 1
        hyd_write(bw, 1, 2);
        hyd_write(bw, nsym - 1, 2);
        int log_alphabet_size = hyd_cllog2(stream->alphabet_sizes[i]);
        if (nsym > 1)
            qsort(tokens, 4, sizeof(HYDVLCElement), &symbol_compare);
        for (int n = 0; n < nsym; n++)
            hyd_write(bw, tokens[n].symbol, log_alphabet_size);
        if (nsym == 4)
            hyd_write_bool(bw, tokens[3].length == 3);
    }

    for (size_t i = 0; i < stream->num_clusters; i++) {
        HYDVLCElement *table = stream->vlc_table + i * stream->max_alphabet_size;
        const uint16_t *lengths = global_lengths + i * stream->max_alphabet_size;
        ret = build_prefix_table(stream->allocator, table, lengths, stream->alphabet_sizes[i]);
        if (ret < HYD_ERROR_START)
            goto fail;
    }

    HYD_FREEA(stream->allocator, global_lengths);
    return bw->overflow_state;

fail:
    destroy_stream(stream, global_lengths);
    return ret;
}

HYDStatusCode hyd_ans_write_stream_header(HYDEntropyStream *stream) {

    HYDStatusCode ret;
    int log_alphabet_size;
    HYDBitWriter *bw = stream->bw;

    if ((ret = stream_header_common(stream, &log_alphabet_size, 0)) < HYD_ERROR_START)
        goto fail;

    size_t table_size = stream->num_clusters * stream->max_alphabet_size;

    /* generate alias mappings */
    stream->alias_table = HYD_ALLOCA(stream->allocator, table_size * sizeof(HYDAliasEntry));
    if (!stream->alias_table) {
        ret = HYD_NOMEM;
        goto fail;
    }
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

HYDStatusCode hyd_prefix_finalize_stream(HYDEntropyStream *stream) {
    HYDBitWriter *bw = stream->bw;

    for (size_t p = 0; p < stream->symbol_pos; p++) {
        size_t cluster = stream->tokens[p].cluster;
        uint32_t token = stream->tokens[p].token;
        const HYDVLCElement *entry = &stream->vlc_table[cluster * stream->max_alphabet_size + token];
        hyd_write(bw, entry->symbol, entry->length);
        hyd_write(bw, stream->residues[p].residue, stream->residues[p].bits);
    }

    destroy_stream(stream, NULL);
    return bw->overflow_state;
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
