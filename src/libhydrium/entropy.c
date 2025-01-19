#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bitwriter.h"
#include "entropy.h"
#include "internal.h"
#include "math-functions.h"
#include "memory.h"

// State Flush. Monsieur Bond Wins.
typedef struct StateFlush {
    size_t token_index;
    uint16_t value;
} StateFlush;

typedef struct StateFlushChain {
    StateFlush *state_flushes;
    size_t pos;
    size_t capacity;
    struct StateFlushChain *prev_chain;
} StateFlushChain;

typedef struct FrequencyEntry {
    int32_t token;
    uint32_t frequency;
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

static const U32Table min_symbol_table = {
    .cpos = {224, 512, 4096, 8},
    .upos = {0, 0, 0, 15},
};
static const U32Table min_length_table = {
    .cpos = {3, 4, 5, 9},
    .upos = {0, 0, 2, 8},
};

static const uint32_t br_lut[16] = {
    0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
    0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF,
};
static inline uint32_t bitswap32(const uint32_t b) {
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

void hyd_entropy_stream_destroy(HYDEntropyStream *stream) {
    if (stream->frequencies) {
        for (size_t i = 0; i < stream->num_clusters; i++)
            hyd_free(stream->allocator, stream->frequencies[i]);
        hyd_free(stream->allocator, stream->frequencies);
    }
    hyd_free(stream->allocator, stream->cluster_map);
    hyd_free(stream->allocator, stream->symbols);
    hyd_free(stream->allocator, stream->configs);
    if (stream->alias_table) {
        for (size_t i = 0; i < stream->num_clusters; i++) {
            if (stream->alias_table[i]) {
                for (size_t j = 0; j < stream->alphabet_sizes[i]; j++)
                    hyd_free(stream->allocator, stream->alias_table[i][j].cutoffs);
                hyd_free(stream->allocator, stream->alias_table[i]);
            }
        }
        hyd_free(stream->allocator, stream->alias_table);
    }
    if (stream->vlc_table) {
        for (size_t i = 0; i < stream->num_clusters; i++)
            hyd_free(stream->allocator, stream->vlc_table[i]);
        hyd_free(stream->allocator, stream->vlc_table);
    }
    hyd_free(stream->allocator, stream->alphabet_sizes);
    memset(stream, 0, sizeof(HYDEntropyStream));
}

HYDStatusCode hyd_entropy_set_hybrid_config(HYDEntropyStream *stream, uint8_t min_cluster, uint8_t to_cluster,
                                            int split_exponent, int msb_in_token, int lsb_in_token) {
    if (to_cluster && min_cluster >= to_cluster) {
        *stream->error = "min_cluster >= to_cluster";
        return HYD_INTERNAL_ERROR;
    }

    for (uint8_t j = min_cluster; (!to_cluster || j < to_cluster) && j < stream->num_clusters; j++) {
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
        1, 1, 64, 0, stream->error);
    if (ret < HYD_ERROR_START)
        goto fail;
    if ((ret = hyd_entropy_set_hybrid_config(&nested, 0, 0, 4, 1, 0)) < HYD_ERROR_START)
        goto fail;
    uint8_t mtf[256];
    for (int i = 0; i < 256; i++)
        mtf[i] = i;
    for (uint32_t j = 0; j < stream->num_dists; j++) {
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
            uint8_t value = mtf[index];
            memmove(mtf + 1, mtf, index);
            mtf[0] = value;
        }
    }

    if ((ret = hyd_prefix_finalize_stream(&nested)) < HYD_ERROR_START)
        goto fail;

    return bw->overflow_state;

fail:
    hyd_entropy_stream_destroy(&nested);
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
    // lsb_in_token
    hyd_write(bw, config->lsb_in_token,
        hyd_cllog2(1 + config->split_exponent - config->msb_in_token));
    return bw->overflow_state;
}

static HYDStatusCode generate_alias_mapping(HYDEntropyStream *stream, size_t cluster,
        int log_alphabet_size, int32_t uniq_pos) {
    int log_bucket_size = 12 - log_alphabet_size;
    uint32_t bucket_size = 1 << log_bucket_size;
    uint32_t table_size = 1 << log_alphabet_size;
    uint32_t symbols[256] = { 0 };
    uint32_t cutoffs[256] = { 0 };
    uint32_t offsets[256] = { 0 };
    HYDAliasEntry *alias_table = stream->alias_table[cluster];

    if (uniq_pos >= 0) {
        for (uint32_t i = 0; i < table_size; i++) {
            symbols[i] = uniq_pos;
            offsets[i] = i * bucket_size;
        }
        alias_table[uniq_pos].count = table_size;
    } else {
        size_t underfull_pos = 0;
        size_t overfull_pos = 0;
        uint8_t underfull[256];
        uint8_t overfull[256];
        for (size_t pos = 0; pos < stream->alphabet_sizes[cluster]; pos++) {
            cutoffs[pos] = stream->frequencies[cluster][pos];
            if (cutoffs[pos] < bucket_size)
                underfull[underfull_pos++] = pos;
            else if (cutoffs[pos] > bucket_size)
                overfull[overfull_pos++] = pos;
        }

        for (uint32_t i = stream->alphabet_sizes[cluster]; i < table_size; i++)
            underfull[underfull_pos++] = i;

        while (overfull_pos) {
            if (!underfull_pos) {
                *stream->error = "empty underfull during alias table gen";
                return HYD_INTERNAL_ERROR;
            }
            uint8_t u = underfull[--underfull_pos];
            uint8_t o = overfull[--overfull_pos];
            int32_t by = bucket_size - cutoffs[u];
            offsets[u] = (cutoffs[o] -= by);
            symbols[u] = o;
            if (cutoffs[o] < bucket_size)
                underfull[underfull_pos++] = o;
            else if (cutoffs[o] > bucket_size)
                overfull[overfull_pos++] = o;
        }

        for (uint32_t sym = 0; sym < table_size; sym++) {
            if (cutoffs[sym] == bucket_size) {
                symbols[sym] = sym;
                cutoffs[sym] = offsets[sym] = 0;
            } else {
                offsets[sym] -= cutoffs[sym];
            }
            alias_table[symbols[sym]].count++;
        }
    }

    for (uint32_t sym = 0; sym < stream->alphabet_sizes[cluster]; sym++) {
        alias_table[sym].cutoffs = hyd_mallocarray(stream->allocator, 3 * (alias_table[sym].count + 1),
            sizeof(int32_t));
        if (!alias_table[sym].cutoffs)
            return HYD_NOMEM;
        memset(alias_table[sym].cutoffs, -1, 3 * (alias_table[sym].count + 1) * sizeof(int32_t));
        alias_table[sym].offsets = alias_table[sym].cutoffs + alias_table[sym].count + 1;
        alias_table[sym].original = alias_table[sym].offsets + alias_table[sym].count + 1;
        alias_table[sym].offsets[0] = 0;
        alias_table[sym].cutoffs[0] = cutoffs[sym];
        alias_table[sym].original[0] = sym;
    }

    for (uint32_t i = 0; i < table_size; i++) {
        size_t j = 1;
        while (alias_table[symbols[i]].cutoffs[j] >= 0)
            j++;
        alias_table[symbols[i]].cutoffs[j] = cutoffs[i];
        alias_table[symbols[i]].offsets[j] = offsets[i];
        alias_table[symbols[i]].original[j] = i;
    }

    return HYD_OK;
}

static int32_t write_ans_frequencies(HYDEntropyStream *stream, uint32_t *frequencies, uint32_t alphabet_size) {
    HYDBitWriter *bw = stream->bw;
    if (!alphabet_size) {
        // simple dist
        hyd_write(bw, 0x1, 2);
        write_ans_u8(bw, 0);
        return 0;
    }
    size_t total = 0;
    for (size_t k = 0; k < alphabet_size; k++)
        total += frequencies[k];
    if (!total) {
        *stream->error = "All-zero ANS frequencies";
        return HYD_INTERNAL_ERROR;
    }

    size_t new_total = 0;
    for (size_t k = 0; k < alphabet_size; k++) {
        if (!frequencies[k])
            continue;
        frequencies[k] = (((uint64_t)frequencies[k] << 12) / total) & 0xFFFF;
        if (!frequencies[k])
            frequencies[k] = 1;
        new_total += frequencies[k];
    }

    size_t j = alphabet_size - 1;
    while (new_total > (1 << 12)) {
        size_t diff = new_total - (1 << 12);
        if (diff < frequencies[j]) {
            frequencies[j] -= diff;
            new_total -= diff;
            break;
        } else if (frequencies[j] > 1) {
            new_total -= frequencies[j] - 1;
            frequencies[j] = 1;
        }
        j--;
    }

    frequencies[0] += (1 << 12) - new_total;

    int32_t nz1 = -1, nz2 = -1, nzc = 0;

    for (size_t k = 0; k < alphabet_size; k++) {
        if (frequencies[k] == 1 << 12) {
            // simple dist
            hyd_write(bw, 0x1, 2);
            write_ans_u8(bw, k);
            return k;
        }
        if (!frequencies[k])
            continue;
        if (++nzc > 2)
            break;
        if (nz1 < 0) {
            nz1 = k;
        } else if (frequencies[nz1] + frequencies[k] == 1 << 12) {
            nz2 = k;
            break;
        }
    }

    if (nz1 >= 0 && nz2 >= 0) {
        // simple dual peak dist
        hyd_write(bw, 0x3, 2);
        write_ans_u8(bw, nz1);
        write_ans_u8(bw, nz2);
        hyd_write(bw, frequencies[nz1], 12);
        return HYD_DEFAULT;
    }

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
        log_counts[k] = frequencies[k] ? 1 + hyd_fllog2(frequencies[k]) : 0;
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

    return HYD_DEFAULT;
}

HYDStatusCode hyd_entropy_init_stream(HYDEntropyStream *stream, HYDAllocator *allocator, HYDBitWriter *bw,
                                  size_t symbol_count, const uint8_t *cluster_map, size_t num_dists,
                                  int custom_configs, uint32_t lz77_min_symbol, int modular, const char **error) {
    HYDStatusCode ret;
    memset(stream, 0, sizeof(HYDEntropyStream));
    stream->error = error;
    if (!num_dists || !symbol_count) {
        *stream->error = "zero dist count or zero symbol count";
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
    stream->modular = modular;
    stream->symbol_count = symbol_count;
    stream->cluster_map = hyd_malloc(allocator, num_dists);
    stream->symbols = hyd_mallocarray(allocator, stream->symbol_count, sizeof(HYDHybridSymbol));
    if (!stream->cluster_map || !stream->symbols) {
        ret = HYD_NOMEM;
        goto fail;
    }
    memcpy(stream->cluster_map, cluster_map, num_dists - !!lz77_min_symbol);
    for (size_t i = 0; i < num_dists - !!lz77_min_symbol; i++) {
        if (stream->cluster_map[i] >= stream->num_clusters)
            stream->num_clusters = stream->cluster_map[i] + 1;
    }
    if (stream->num_clusters > num_dists) {
        *stream->error = "more clusters than dists";
        ret = HYD_INTERNAL_ERROR;
        goto fail;
    }

    if (lz77_min_symbol)
        stream->cluster_map[num_dists - 1] = stream->num_clusters++;

    stream->configs = hyd_mallocarray(allocator, stream->num_clusters, sizeof(HYDHybridUintConfig));
    stream->alphabet_sizes = hyd_calloc(allocator, stream->num_clusters, sizeof(uint32_t));
    if (!stream->configs || !stream->alphabet_sizes) {
        ret = HYD_NOMEM;
        goto fail;
    }

    if (!custom_configs) {
        hyd_entropy_set_hybrid_config(stream, 0, stream->num_clusters - !!stream->lz77_min_symbol, 4, 1, 1);
        if (stream->lz77_min_symbol)
            hyd_entropy_set_hybrid_config(stream, stream->num_clusters - 1, stream->num_clusters, 7, 0, 0);
    }

    return HYD_OK;

fail:
    hyd_entropy_stream_destroy(stream);
    return ret;
}

static void hybridize(uint32_t symbol, HYDHybridSymbol *hybrid_symbol, const HYDHybridUintConfig *config) {
    int split = 1 << config->split_exponent;
    if (symbol < split) {
        hybrid_symbol->token = symbol;
        hybrid_symbol->residue = hybrid_symbol->residue_bits = 0;
    } else {
        uint32_t n = hyd_fllog2(symbol) - config->lsb_in_token - config->msb_in_token;
        uint32_t low = symbol & ~(~UINT32_C(0) << config->lsb_in_token);
        symbol >>= config->lsb_in_token;
        hybrid_symbol->residue = symbol & ~(~UINT32_C(0) << n);
        symbol >>= n;
        uint32_t high = symbol & ~(~UINT32_C(0) << config->msb_in_token);
        hybrid_symbol->residue_bits = n;
        hybrid_symbol->token = split + (low | (high << config->lsb_in_token) |
                        ((n - config->split_exponent + config->lsb_in_token + config->msb_in_token) <<
                        (config->msb_in_token + config->lsb_in_token)));
    }
}

static HYDStatusCode send_hybridized_symbol(HYDEntropyStream *stream, const HYDHybridSymbol *symbol) {
    if (stream->wrote_stream_header) {
        *stream->error = "Illegal send after stream header";
        return HYD_INTERNAL_ERROR;
    }
    if (stream->symbol_pos >= stream->symbol_count) {
        HYDHybridSymbol *symbols = hyd_reallocarray(stream->allocator, stream->symbols, stream->symbol_count << 1,
            sizeof(HYDHybridSymbol));
        if (!symbols)
            return HYD_NOMEM;
        stream->symbols = symbols;
        stream->symbol_count <<= 1;
    }
    stream->symbols[stream->symbol_pos++] = *symbol;
    if (symbol->token >= stream->max_alphabet_size)
        stream->max_alphabet_size = 1 + symbol->token;
    if (symbol->token >= stream->alphabet_sizes[symbol->cluster])
        stream->alphabet_sizes[symbol->cluster] = 1 + symbol->token;
    return HYD_OK;
}

static HYDStatusCode send_entropy_symbol0(HYDEntropyStream *stream, size_t dist, uint32_t symbol) {
    HYDHybridSymbol hybrid_symbol;
    hybrid_symbol.cluster = stream->cluster_map[dist];
    hybridize(symbol, &hybrid_symbol, &stream->configs[hybrid_symbol.cluster]);
    return send_hybridized_symbol(stream, &hybrid_symbol);
}

static HYDStatusCode flush_lz77(HYDEntropyStream *stream) {
    HYDStatusCode ret;
    uint32_t last_symbol = stream->last_symbol - 1;

    if (stream->lz77_rle_count > stream->lz77_min_length) {
        uint32_t repeat_count = stream->lz77_rle_count - stream->lz77_min_length;
        HYDHybridSymbol hybrid_symbol;
        hybridize(repeat_count, &hybrid_symbol, &lz77_len_conf);
        hybrid_symbol.cluster = stream->cluster_map[stream->last_dist];
        hybrid_symbol.token += stream->lz77_min_symbol;
        if ((ret = send_hybridized_symbol(stream, &hybrid_symbol)) < HYD_ERROR_START)
            return ret;
        if ((ret = send_entropy_symbol0(stream, stream->num_dists - 1, !!stream->modular)) < HYD_ERROR_START)
            return ret;
    } else if (stream->last_symbol && stream->lz77_rle_count) {
        for (uint32_t k = 0; k < stream->lz77_rle_count; k++) {
            if ((ret = send_entropy_symbol0(stream, stream->last_dist, last_symbol)) < HYD_ERROR_START)
                return ret;
        }
    }

    stream->lz77_rle_count = 0;

    return HYD_OK;
}

HYDStatusCode hyd_entropy_send_symbol(HYDEntropyStream *stream, size_t dist, uint32_t symbol) {
    HYDStatusCode ret = HYD_OK;

    if (!stream->lz77_min_symbol)
        return send_entropy_symbol0(stream, dist, symbol);

    if (stream->last_symbol == symbol + 1 &&
            stream->cluster_map[stream->last_dist] == stream->cluster_map[dist]) {
        if (++stream->lz77_rle_count < 128)
            return HYD_OK;
        stream->lz77_rle_count--;
    }

    if ((ret = flush_lz77(stream)) < HYD_ERROR_START)
        return ret;

    stream->last_symbol = symbol + 1;
    stream->last_dist = dist;

    return send_entropy_symbol0(stream, dist, symbol);
}

static HYDStatusCode stream_header_common(HYDEntropyStream *stream, int *las, int prefix_codes) {
    HYDStatusCode ret = HYD_OK;
    HYDBitWriter *bw = stream->bw;
    hyd_write_bool(bw, stream->lz77_min_symbol);
    if (stream->lz77_min_symbol) {
        ret = flush_lz77(stream);
        if (ret < HYD_ERROR_START)
            return ret;
        hyd_write_u32(bw, &min_symbol_table, stream->lz77_min_symbol);
        hyd_write_u32(bw, &min_length_table, stream->lz77_min_length);
        ret = write_hybrid_uint_config(stream, &lz77_len_conf, 8);
    }
    if (ret < HYD_ERROR_START)
        return ret;
    if ((ret = write_cluster_map(stream)) < HYD_ERROR_START)
        return ret;

    int log_alphabet_size = hyd_cllog2(stream->max_alphabet_size);
    if (log_alphabet_size < 5)
        log_alphabet_size = 5;
    *las = log_alphabet_size;

    hyd_write_bool(bw, prefix_codes);
    if (!prefix_codes)
        hyd_write(bw, log_alphabet_size - 5, 2);

    for (size_t i = 0; i < stream->num_clusters; i++) {
        ret = write_hybrid_uint_config(stream, &stream->configs[i], prefix_codes ? 15 : log_alphabet_size);
        if (ret < HYD_ERROR_START)
            return ret;
    }

    /* populate frequencies */
    stream->frequencies = hyd_calloc(stream->allocator, stream->num_clusters, sizeof(uint32_t *));
    if (!stream->frequencies)
        return HYD_NOMEM;
    for (size_t c = 0; c < stream->num_clusters; c++) {
        if (!stream->alphabet_sizes[c])
            continue;
        stream->frequencies[c] = hyd_calloc(stream->allocator, stream->alphabet_sizes[c], sizeof(uint32_t));
        if (!stream->frequencies[c])
            return HYD_NOMEM;
    }
    for (size_t pos = 0; pos < stream->symbol_pos; pos++) {
        const HYDHybridSymbol *sym = &stream->symbols[pos];
        stream->frequencies[sym->cluster][sym->token]++;
    }

    return bw->overflow_state;
}

static inline int huffman_compare(const FrequencyEntry *fa, const FrequencyEntry *fb) {
    return fa->frequency != fb->frequency ?
        (!fb->frequency ? -1 : !fa->frequency ? 1 : fa->frequency - fb->frequency) :
        (!fb->token ? -1 : !fa->token ? 1 : fa->token - fb->token);
}

static int32_t collect(FrequencyEntry *entry) {
    if (!entry)
        return 0;
    int32_t self = ++entry->depth;
    int32_t left = collect(entry->left_child);
    int32_t right = collect(entry->right_child);
    return entry->max_depth = hyd_max3(self, left, right);
}

static HYDStatusCode build_huffman_tree(HYDEntropyStream *stream, const uint32_t *frequencies,
                                        uint32_t *lengths, uint32_t alphabet_size, int32_t max_depth) {
    HYDStatusCode ret = HYD_OK;
    HYDAllocator *allocator = stream->allocator;
    FrequencyEntry *tree = hyd_calloc(allocator, (2 * alphabet_size - 1), sizeof(FrequencyEntry));
    if (!tree) {
        ret = HYD_NOMEM;
        goto end;
    }

    uint32_t nz = 0;
    for (uint32_t token = 0; token < alphabet_size; token++) {
        tree[token].frequency = frequencies[token];
        tree[token].token = 1 + token;
        tree[token].left_child = tree[token].right_child = NULL;
        if (frequencies[token])
            nz++;
    }
    if (!nz) {
        *stream->error = "No nonzero frequencies";
        ret = HYD_INTERNAL_ERROR;
        goto end;
    }

    if (max_depth < 0)
        max_depth = hyd_cllog2(alphabet_size + 1);

    for (uint32_t k = 0; k < alphabet_size - 1; k++) {
        int32_t smallest = -1;
        int32_t second = -1;
        int32_t target = max_depth - hyd_cllog2(nz--) + 1;
        for (uint32_t j = 2 * k; j < alphabet_size + k; j++) {
            if (!tree[j].frequency)
                continue;
            if (tree[j].max_depth >= target)
                continue;
            if (smallest < 0 || huffman_compare(&tree[j], &tree[smallest]) < 0) {
                second = smallest;
                smallest = j;
            } else if (second < 0 || huffman_compare(&tree[j], &tree[second]) < 0) {
                second = j;
            }
        }
        if (smallest < 0) {
            *stream->error = "couldn't find target";
            ret = HYD_INTERNAL_ERROR;
            goto end;
        }
        hyd_swap(FrequencyEntry, tree[smallest], tree[2 * k]);
        if (second < 0)
            break;
        if (second == 2 * k)
            second = smallest;
        smallest = 2 * k;
        hyd_swap(FrequencyEntry, tree[second], tree[2 * k + 1]);
        second = smallest + 1;
        FrequencyEntry *entry = &tree[alphabet_size + k];
        entry->frequency = tree[smallest].frequency + tree[second].frequency;
        entry->left_child = &tree[smallest];
        entry->right_child = &tree[second];
        collect(entry);
    }

    for (uint32_t j = 0; j < 2 * alphabet_size - 1; j++) {
        if (tree[j].token)
            lengths[tree[j].token - 1] = tree[j].depth;
    }

end:
    hyd_free(allocator, tree);
    return ret;
}

static HYDStatusCode build_prefix_table(HYDEntropyStream *stream, HYDVLCElement *table,
                                        const uint32_t *lengths, uint32_t alphabet_size) {
    HYDStatusCode ret = HYD_OK;
    HYDAllocator *allocator = stream->allocator;
    uint32_t *counts = NULL;
    HYDVLCElement *pre_table = NULL;
    size_t csize = hyd_max(alphabet_size + 1, 16);
    counts = hyd_calloc(allocator, csize, sizeof(uint32_t));
    pre_table = hyd_mallocarray(allocator, alphabet_size, sizeof(HYDVLCElement));
    if (!counts || !pre_table) {
        ret = HYD_NOMEM;
        goto end;
    }

    for (uint32_t j = 0; j < alphabet_size; j++)
        counts[lengths[j]]++;
    for (uint32_t j = 1; j < alphabet_size + 1; j++)
        counts[j] += counts[j - 1];
    for (int32_t j = alphabet_size - 1; j >= 0; j--) {
        uint32_t index = --counts[lengths[j]];
        pre_table[index].length = lengths[j];
        pre_table[index].symbol = j;
    }

    uint64_t code = 0;
    for (int32_t j = 0; j < alphabet_size; j++) {
        if (!pre_table[j].length)
            continue;
        uint32_t s = pre_table[j].symbol;
        table[s].symbol = bitswap32(code);
        table[s].length = pre_table[j].length;
        code += UINT64_C(1) << (32 - pre_table[j].length);
    }

    if (code && code != (UINT64_C(1) << 32)) {
        *stream->error = "VLC codes do not add up";
        ret = HYD_INTERNAL_ERROR;
        goto end;
    }

end:
    hyd_free(allocator, counts);
    hyd_free(allocator, pre_table);
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
                                                  const uint32_t *lengths) {
    HYDBitWriter *bw = stream->bw;
    HYDStatusCode ret = HYD_OK;
    HYDVLCElement *level1_table = NULL;

    // hskip = 0
    hyd_write(bw, 0, 2);

    uint32_t level1_freqs[18] = { 0 };

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

    uint32_t level1_lengths[18] = { 0 };
    ret = build_huffman_tree(stream, level1_freqs, level1_lengths, 18, 5);
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
        *stream->error = "level1 code total mismatch";
        ret = HYD_INTERNAL_ERROR;
        goto end;
    }

    level1_table = hyd_calloc(stream->allocator, 18, sizeof(HYDVLCElement));
    if (!level1_table) {
        ret = HYD_NOMEM;
        goto end;
    }

    ret = build_prefix_table(stream, level1_table, level1_lengths, 18);
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
    hyd_free(stream->allocator, level1_table);
    return ret;
}

HYDStatusCode hyd_prefix_write_stream_header(HYDEntropyStream *stream) {
    HYDStatusCode ret;
    int log_alphabet_size;
    HYDBitWriter *bw = stream->bw;
    uint32_t *lengths = NULL;

    ret = stream_header_common(stream, &log_alphabet_size, 1);
    if (ret < HYD_ERROR_START)
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

    lengths = hyd_mallocarray(stream->allocator, stream->max_alphabet_size, sizeof(uint32_t));
    stream->vlc_table = hyd_calloc(stream->allocator, stream->num_clusters, sizeof(HYDVLCElement *));
    if (!lengths || !stream->vlc_table) {
        ret = HYD_NOMEM;
        goto fail;
    }
    for (size_t i = 0; i < stream->num_clusters; i++) {
        stream->vlc_table[i] = hyd_calloc(stream->allocator, stream->alphabet_sizes[i], sizeof(HYDVLCElement));
        if (!stream->vlc_table[i]) {
            ret = HYD_NOMEM;
            goto fail;
        }
    }

    for (size_t i = 0; i < stream->num_clusters; i++) {
        const uint32_t alphabet_size = stream->alphabet_sizes[i];
        if (alphabet_size <= 1)
            continue;
        const uint32_t *freqs = stream->frequencies[i];
        memset(lengths, 0, stream->max_alphabet_size * sizeof(uint32_t));
        HYDVLCElement *table = stream->vlc_table[i];
        ret = build_huffman_tree(stream, freqs, lengths, alphabet_size, 15);
        if (ret < HYD_ERROR_START)
            goto fail;
        uint32_t nsym = 0;
        HYDVLCElement tokens[4] = { 0 };
        for (uint32_t j = 0; j < alphabet_size; j++) {
            if (!lengths[j])
                continue;
            if (nsym < 4) {
                tokens[nsym].symbol = j;
                tokens[nsym].length = lengths[j];
            }
            if (++nsym > 4)
                break;
        }

        if (nsym > 4) {
            ret = write_complex_prefix_lengths(stream, alphabet_size, lengths);
            if (ret < HYD_ERROR_START)
                goto fail;
            ret = build_prefix_table(stream, table, lengths, alphabet_size);
            if (ret < HYD_ERROR_START)
                goto fail;
            continue;
        }

        if (nsym == 0) {
            nsym = 1;
            tokens[0].symbol = alphabet_size - 1;
        }

        // hskip = 1
        hyd_write(bw, 1, 2);
        hyd_write(bw, nsym - 1, 2);
        int log_alphabet_size = hyd_cllog2(alphabet_size);
        if (nsym == 3 && tokens[0].length != 1) {
            if (tokens[1].length == 1) {
                hyd_swap(HYDVLCElement, tokens[0], tokens[1]);
            } else {
                hyd_swap(HYDVLCElement, tokens[0], tokens[2]);
            }
        }
        int tree_select = 0;
        if (nsym == 4) {
            for (int i = 0; i < 4; i++) {
                if (tokens[i].length != 2) {
                    tree_select = 1;
                    break;
                }
            }
            if (tree_select && tokens[0].length != 1) {
                if (tokens[1].length == 1) {
                    hyd_swap(HYDVLCElement, tokens[0], tokens[1]);
                } else if (tokens[2].length == 1) {
                    hyd_swap(HYDVLCElement, tokens[0], tokens[2]);
                } else {
                    hyd_swap(HYDVLCElement, tokens[0], tokens[3]);
                }
            }
            if (tree_select && tokens[1].length != 2) {
                if (tokens[2].length == 2) {
                    hyd_swap(HYDVLCElement, tokens[1], tokens[2]);
                } else {
                    hyd_swap(HYDVLCElement, tokens[1], tokens[3]);
                }
            }
        }
        for (int n = 0; n < nsym; n++)
            hyd_write(bw, tokens[n].symbol, log_alphabet_size);
        if (nsym == 4)
            hyd_write_bool(bw, tree_select);
        ret = build_prefix_table(stream, table, lengths, alphabet_size);
        if (ret < HYD_ERROR_START)
            goto fail;
    }

    hyd_free(stream->allocator, lengths);
    stream->wrote_stream_header = 1;
    return bw->overflow_state;

fail:
    hyd_free(stream->allocator, lengths);
    hyd_entropy_stream_destroy(stream);
    return ret;
}

HYDStatusCode hyd_ans_write_stream_header(HYDEntropyStream *stream) {

    HYDStatusCode ret;
    int log_alphabet_size;
    HYDBitWriter *bw = stream->bw;

    ret = stream_header_common(stream, &log_alphabet_size, 0);
    if (ret < HYD_ERROR_START)
        goto fail;

    stream->alias_table = hyd_calloc(stream->allocator, stream->num_clusters, sizeof(HYDAliasEntry *));
    if (!stream->alias_table) {
        ret = HYD_NOMEM;
        goto fail;
    }
    for (size_t i = 0; i < stream->num_clusters; i++) {
        if (!stream->alphabet_sizes[i])
            continue;
        stream->alias_table[i] = hyd_calloc(stream->allocator, stream->alphabet_sizes[i], sizeof(HYDAliasEntry));
        if (!stream->alias_table[i]) {
            ret = HYD_NOMEM;
            goto fail;
        }
    }
    for (size_t i = 0; i < stream->num_clusters; i++) {
        int32_t uniq_pos = write_ans_frequencies(stream, stream->frequencies[i], stream->alphabet_sizes[i]);
        if (uniq_pos < HYD_ERROR_START) {
            ret = uniq_pos;
            goto fail;
        }
        if (!stream->alphabet_sizes[i])
            continue;
        ret = generate_alias_mapping(stream, i, log_alphabet_size, uniq_pos);
        if (ret < HYD_ERROR_START)
            goto fail;
    }

    stream->wrote_stream_header = 1;

    return bw->overflow_state;

fail:
    hyd_entropy_stream_destroy(stream);
    return ret;
}

HYDStatusCode hyd_prefix_write_stream_symbols(HYDEntropyStream *stream, size_t symbol_start, size_t symbol_count) {
    HYDBitWriter *bw = stream->bw;

    if (symbol_count + symbol_start > stream->symbol_pos) {
        *stream->error = "symbol out of bounds";
        return HYD_INTERNAL_ERROR;
    }

    const HYDHybridSymbol *symbols = stream->symbols + symbol_start;
    for (size_t p = 0; p < symbol_count; p++) {
        size_t cluster = symbols[p].cluster;
        uint32_t token = symbols[p].token;
        const HYDVLCElement *entry = &stream->vlc_table[cluster][token];
        hyd_write(bw, entry->symbol, entry->length);
        hyd_write(bw, symbols[p].residue, symbols[p].residue_bits);
    }

    return bw->overflow_state;
}

HYDStatusCode hyd_prefix_finalize_stream(HYDEntropyStream *stream) {
    HYDStatusCode ret = hyd_prefix_write_stream_header(stream);
    if (ret < HYD_ERROR_START)
        goto end;
    ret = hyd_prefix_write_stream_symbols(stream, 0, stream->symbol_pos);

end:
    hyd_entropy_stream_destroy(stream);
    return ret;
}

static HYDStatusCode append_state_flush(HYDAllocator *allocator, StateFlushChain **flushes,
                                        size_t token_index, uint16_t value) {
    if ((*flushes)->pos == (*flushes)->capacity) {
        StateFlushChain *chain = hyd_malloc(allocator, sizeof(StateFlushChain));
        if (!chain)
            return HYD_NOMEM;
        chain->state_flushes = hyd_mallocarray(allocator, 1 << 10, sizeof(StateFlush));
        if (!chain->state_flushes) {
            hyd_free(allocator, chain);
            return HYD_NOMEM;
        }
        chain->capacity = 1 << 10;
        chain->pos = 0;
        chain->prev_chain = *flushes;
        *flushes = chain;
    }
    (*flushes)->state_flushes[(*flushes)->pos++] = (StateFlush){
        .token_index = token_index,
        .value = value
    };

    return HYD_OK;
}

static StateFlush *pop_state_flush(HYDAllocator *allocator, StateFlushChain **flushes) {
    if ((*flushes)->pos > 0)
        return &(*flushes)->state_flushes[--(*flushes)->pos];
    StateFlushChain *prev_chain = (*flushes)->prev_chain;
    if (!prev_chain)
        return NULL;
    hyd_free(allocator, (*flushes)->state_flushes);
    hyd_free(allocator, *flushes);
    *flushes = prev_chain;
    return pop_state_flush(allocator, flushes);
}

HYDStatusCode hyd_ans_write_stream_symbols(HYDEntropyStream *stream, size_t symbol_start, size_t symbol_count) {
    HYDStatusCode ret = HYD_OK;
    StateFlushChain flushes_base = { 0 }, *flushes = &flushes_base;
    HYDBitWriter *bw = stream->bw;
    int log_alphabet_size = hyd_cllog2(stream->max_alphabet_size);
    if (log_alphabet_size < 5)
        log_alphabet_size = 5;
    const uint32_t log_bucket_size = 12 - log_alphabet_size;
    const uint32_t pos_mask = ~(~UINT32_C(0) << log_bucket_size);
    if (!stream->alias_table) {
        *stream->error = "alias table never generated";
        ret = HYD_INTERNAL_ERROR;
        goto end;
    }

    if (symbol_count + symbol_start > stream->symbol_pos) {
        *stream->error = "symbol out of bounds during ans flush";
        ret = HYD_INTERNAL_ERROR;
        goto end;
    }

    flushes->state_flushes = hyd_mallocarray(stream->allocator, 1 << 10, sizeof(StateFlush));
    if (!flushes->state_flushes) {
        ret = HYD_NOMEM;
        goto end;
    }
    flushes->capacity = 1 << 10;

    uint32_t state = 0x130000;
    const HYDHybridSymbol *symbols = stream->symbols + symbol_start;
    for (size_t p2 = 0; p2 < symbol_count; p2++) {
        const size_t p = symbol_count - p2 - 1;
        const uint8_t symbol = symbols[p].token;
        const size_t cluster = symbols[p].cluster;
        const uint32_t freq = stream->frequencies[cluster][symbol];
        if ((state >> 20) >= freq) {
            ret = append_state_flush(stream->allocator, &flushes, p, state & 0xFFFF);
            if (ret < HYD_ERROR_START)
                goto end;
            state >>= 16;
        }
        const uint32_t div = state / freq;
        const uint32_t offset = state - div * freq;
        uint32_t i, pos, j;
        const HYDAliasEntry *alias = &stream->alias_table[cluster][symbol];
        for (j = 0; j <= alias->count; j++) {
            pos = offset - alias->offsets[j];
            int32_t k = pos - alias->cutoffs[j];
            if (!(pos & ~pos_mask) && (j > 0 ? k >= 0 : k < 0)) {
                i = alias->original[j];
                break;
            }
        }
        if (j > alias->count) {
            *stream->error = "alias table lookup failed";
            ret = HYD_INTERNAL_ERROR;
            goto end;
        }
        state = (div << 12) | (i << log_bucket_size) | pos;
    }
    ret = append_state_flush(stream->allocator, &flushes, 0, (state >> 16) & 0xFFFF);
    if (ret < HYD_ERROR_START)
        goto end;
    ret = append_state_flush(stream->allocator, &flushes, 0, state & 0xFFFF);
    if (ret < HYD_ERROR_START)
        goto end;
    for (size_t p = 0; p < symbol_count; p++) {
        StateFlush *flush;
        while ((flush = pop_state_flush(stream->allocator, &flushes))) {
            if (p >= flush->token_index) {
                hyd_write(bw, flush->value, 16);
            } else {
                flushes->pos++;
                break;
            }
        }
        hyd_write(bw, symbols[p].residue, symbols[p].residue_bits);
    }

    ret = bw->overflow_state;

end:
    while (flushes->prev_chain) {
        StateFlushChain *prev = flushes->prev_chain;
        hyd_free(stream->allocator, flushes->state_flushes);
        hyd_free(stream->allocator, flushes);
        flushes = prev;
    }
    hyd_free(stream->allocator, flushes->state_flushes);
    return ret;
}

HYDStatusCode hyd_ans_finalize_stream(HYDEntropyStream *stream) {
    HYDStatusCode ret = hyd_ans_write_stream_header(stream);
    if (ret < HYD_ERROR_START)
        goto end;
    ret = hyd_ans_write_stream_symbols(stream, 0, stream->symbol_pos);

end:
    hyd_entropy_stream_destroy(stream);
    return ret;
}
