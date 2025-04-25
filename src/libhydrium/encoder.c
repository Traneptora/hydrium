/*
 * Base encoder implementation
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bitwriter.h"
#include "encoder.h"
#include "entropy.h"
#include "internal.h"
#include "math-functions.h"
#include "memory.h"

typedef struct IntPos {
    uint8_t x, y;
} IntPos;

static const uint8_t level10_header[49] = {
    0x00, 0x00, 0x00, 0x0c,  'J',  'X',  'L',  ' ',
    0x0d, 0x0a, 0x87, 0x0a, 0x00, 0x00, 0x00, 0x14,
     'f',  't',  'y',  'p',  'j',  'x',  'l',  ' ',
    0x00, 0x00, 0x00, 0x00,  'j',  'x',  'l',  ' ',
    0x00, 0x00, 0x00, 0x09,  'j',  'x',  'l',  'l', 0x0a,
    0x00, 0x00, 0x00, 0x00,  'j',  'x',  'l',  'c',
};

static const float cosine_lut[7][8] = {
    {0.17338, 0.146984, 0.0982119, 0.0344874, -0.0344874, -0.0982119, -0.146984, -0.17338},
    {0.16332, 0.0676495, -0.0676495, -0.16332, -0.16332, -0.0676495, 0.0676495, 0.16332},
    {0.146984, -0.0344874, -0.17338, -0.0982119, 0.0982119, 0.17338, 0.0344874, -0.146984},
    {0.125, -0.125, -0.125, 0.125, 0.125, -0.125, -0.125, 0.125},
    {0.0982119, -0.17338, 0.0344874, 0.146984, -0.146984, -0.0344874, 0.17338, -0.0982119},
    {0.0676495, -0.16332, 0.16332, -0.0676495, -0.0676495, 0.16332, -0.16332, 0.0676495},
    {0.0344874, -0.0982119, 0.146984, -0.17338, 0.17338, -0.146984, 0.0982119, -0.0344874},
};

static const IntPos natural_order[64] = {
    {0, 0}, {1, 0}, {0, 1}, {0, 2}, {1, 1}, {2, 0}, {3, 0}, {2, 1},
    {1, 2}, {0, 3}, {0, 4}, {1, 3}, {2, 2}, {3, 1}, {4, 0}, {5, 0},
    {4, 1}, {3, 2}, {2, 3}, {1, 4}, {0, 5}, {0, 6}, {1, 5}, {2, 4},
    {3, 3}, {4, 2}, {5, 1}, {6, 0}, {7, 0}, {6, 1}, {5, 2}, {4, 3},
    {3, 4}, {2, 5}, {1, 6}, {0, 7}, {1, 7}, {2, 6}, {3, 5}, {4, 4},
    {5, 3}, {6, 2}, {7, 1}, {7, 2}, {6, 3}, {5, 4}, {4, 5}, {3, 6},
    {2, 7}, {3, 7}, {4, 6}, {5, 5}, {6, 4}, {7, 3}, {7, 4}, {6, 5},
    {5, 6}, {4, 7}, {5, 7}, {6, 6}, {7, 5}, {7, 6}, {6, 7}, {7, 7},
};

static const size_t coeff_freq_context[64] = {
     0,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22,
    23, 23, 23, 23, 24, 24, 24, 24, 25, 25, 25, 25, 26, 26, 26, 26,
    27, 27, 27, 27, 28, 28, 28, 28, 29, 29, 29, 29, 30, 30, 30, 30,
};

static const size_t coeff_num_non_zero_context[64] = {
      0,   0,  31,  62,  62,  93,  93,  93,  93, 123, 123, 123, 123, 152,
    152, 152, 152, 152, 152, 152, 152, 180, 180, 180, 180, 180, 180, 180,
    180, 180, 180, 180, 180, 206, 206, 206, 206, 206, 206, 206, 206, 206,
    206, 206, 206, 206, 206, 206, 206, 206, 206, 206, 206, 206, 206, 206,
    206, 206, 206, 206, 206, 206, 206, 206,
};

static const size_t hf_block_cluster_map[39] = {
    0, 1, 2, 2,  3,  3,  4,  5,  6,  6,  6,  6,  6,
    7, 8, 9, 9, 10, 11, 12, 13, 14, 14, 14, 14, 14,
    7, 8, 9, 9, 10, 11, 12, 13, 14, 14, 14, 14, 14,
};

static const int32_t hf_quant_weights[3][64] = {
    {
        1969, 1969, 1969, 1962, 1969, 1962, 1655, 1885, 1885, 1655, 1397, 1610, 1704, 1610, 1397, 1178,
        1368, 1494, 1494, 1368, 1178,  994, 1159, 1289, 1340, 1289, 1159,  994,  839,  980, 1104, 1178,
        1178, 1104,  980,  839,  829,  941, 1023, 1054, 1023,  941,  829,  800,  881,  928,  928,  881,
         800,  755,  809,  829,  809,  755,  663,  731,  731,  663,  491,  524,  491,  349,  349,  239,
    },
    {
        280,  280,  280,  279,  280,  279,  245,  271,  271,  245,  214,  239,  250,  239,  214,  188,
        211,  226,  226,  211,  188,  164,  185,  201,  207,  201,  185,  164,  144,  163,  178,  188,
        188,  178,  163,  144,  143,  157,  168,  172,  168,  157,  143,  139,  150,  156,  156,  150,
        139,  133,  140,  143,  140,  133,  125,  129,  129,  125,  116,  118,  116,  107,  107,   98,
    },
    {
        256,  147,  147,   85,  117,   85,   60,   78,   78,   60,   43,   56,   63,   56,   43,   43,
         43,   48,   48,   43,   43,   42,   43,   43,   43,   43,   43,   42,   29,   41,   43,   43,
         43,   43,   41,   29,   29,   37,   43,   43,   43,   37,   29,   27,   33,   36,   36,   33,
         27,   24,   27,   29,   27,   24,   20,   22,   22,   20,   15,   16,   15,   10,   10,    7,
    },
};

static const uint16_t hf_mult = 5;
static const uint64_t zero64 = 0;
static const void *const zerobuf = &zero64;
static const U32Table size_header_u32 = {
    .cpos = {1, 1, 1, 1},
    .upos = {9, 13, 18, 30},
};
static const U32Table frame_size_u32 = {
    .cpos = {0, 256, 2304, 18688},
    .upos = {8, 11, 14, 30},
};
static const U32Table global_scale_table = {
    .cpos = {1, 2049, 4097, 8193},
    .upos = {11, 11, 12, 16},
};
static const U32Table quant_lf_table = {
    .cpos = {16, 1, 1, 1},
    .upos = {0, 5, 8, 16},
};
static const uint32_t lf_ma_tree[][2] = {
    {1, 0}, {2, 5}, {3, 0}, {4, 0}, {5, 0},
};
static const U32Table toc_table = {
    .cpos = {0, 1024, 17408, 4211712},
    .upos = {10, 14, 22, 30},
};

extern const uint8_t hf_cluster_map[7425];

static HYDStatusCode write_header(HYDEncoder *encoder) {

    HYDBitWriter *bw = &encoder->writer;
    if (bw->overflow_state)
        return bw->overflow_state;

    if (encoder->level10) {
        /* skip the cache and copy directly in, as this is always the head of the file */
        memcpy(bw->buffer + bw->buffer_pos, level10_header, sizeof(level10_header));
        bw->buffer_pos += sizeof(level10_header);
    }

    /* signature = 0xFF0A:16 and div8 = 0:1 */
    hyd_write(bw, 0x0AFF, 17);
    hyd_write_u32(bw, &size_header_u32, encoder->metadata.height);
    hyd_write(bw, 0, 3);
    hyd_write_u32(bw, &size_header_u32, encoder->metadata.width);

    /* all_default:1, default_m:1 */
    hyd_write(bw, 0x3, 2);

    encoder->wrote_header = 1;
    return bw->overflow_state;
}

static size_t *calculate_toc_perm(HYDEncoder *encoder, size_t *toc_size) {
    const size_t frame_w = encoder->one_frame ? encoder->metadata.width : encoder->lf_group->width;
    const size_t frame_h = encoder->one_frame ? encoder->metadata.height : encoder->lf_group->height;
    const size_t frame_groups_x = (frame_w + 255) >> 8;
    const size_t frame_groups_y = (frame_h + 255) >> 8;
    const size_t num_frame_groups = frame_groups_x * frame_groups_y;
    *toc_size = num_frame_groups > 1 ? 2 + num_frame_groups + encoder->lf_groups_per_frame : 1;
    if (*toc_size <= 1)
        return NULL;
    size_t *toc = hyd_malloc_array(*toc_size << 1, sizeof(size_t));
    if (!toc)
        return NULL;
    toc[0] = 0; // LFGlobal
    if (*toc_size == 1) {
        toc[1] = 0;
        return toc;
    }
    size_t idx = 1;
    for (size_t sent_lfid = 0; sent_lfid < encoder->lf_groups_per_frame; sent_lfid++) {
        size_t raster_lfid = encoder->lf_group_perm ? encoder->lf_group_perm[sent_lfid] : 0;
        toc[idx++] = 1 + raster_lfid; // LFGroup
    }
    for (size_t sent_lfid = 0; sent_lfid < encoder->lf_groups_per_frame; sent_lfid++) {
        if (sent_lfid == 0)
            toc[idx++] = 1 + encoder->lf_groups_per_frame; // HFGlobal
        size_t raster_lfid = encoder->lf_group_perm ? encoder->lf_group_perm[sent_lfid] : 0;
        const HYDLFGroup *lf_group = &encoder->lf_group[raster_lfid];
        const size_t gcountx = (lf_group->width + 255) >> 8;
        const size_t gcounty = (lf_group->height + 255) >> 8;
        const size_t gcount = gcountx * gcounty;
        for (size_t g = 0; g < gcount; g++) {
            size_t gy = (encoder->one_frame ? (lf_group->y << 3) : 0) + (g / gcountx);
            size_t gx = (encoder->one_frame ? (lf_group->x << 3) : 0) + (g % gcountx);
            toc[idx++] = 2 + encoder->lf_groups_per_frame + gy * frame_groups_x + gx;
        }
    }
    for (size_t j = 0; j < *toc_size; j++)
        toc[*toc_size + toc[j]] = j;

    return toc;
}

static size_t *get_lehmer_sequence(HYDEncoder *encoder, size_t *toc_size) {
    size_t *toc_perm = NULL;
    int32_t *temp = NULL;
    size_t *lehmer = NULL;

    toc_perm = calculate_toc_perm(encoder, toc_size);
    if (!toc_perm)
        goto end;
    temp = hyd_malloc_array(*toc_size, sizeof(int32_t));
    if (!temp)
        goto end;
    for (size_t i = 0; i < *toc_size; i++)
        temp[i] = i;
    lehmer = calloc(*toc_size, sizeof(size_t));
    if (!lehmer)
        goto end;

    for (size_t i = 0; i < *toc_size; i++) {
        size_t k = 0;
        for (size_t j = 0; j < *toc_size; j++) {
            if (temp[j] == toc_perm[*toc_size + i]) {
                lehmer[i] = k;
                temp[j] = -1;
            } else if (temp[j] >= 0) {
                k++;
            }
        }
    }

end:
    hyd_freep(&toc_perm);
    hyd_freep(&temp);
    return lehmer;
}

static HYDStatusCode write_frame_header(HYDEncoder *encoder) {
    HYDBitWriter *bw = &encoder->writer;
    HYDStatusCode ret;
    HYDEntropyStream toc_stream = { 0 };
    size_t *lehmer = NULL;

    if (bw->overflow_state)
        return bw->overflow_state;

    hyd_write_zero_pad(bw);

    int is_last = encoder->one_frame || encoder->last_tile;
    int have_crop = !encoder->one_frame &&
        !(encoder->metadata.width <= encoder->lf_group->width
        && encoder->metadata.height <= encoder->lf_group->height);

    /* all_default = 0 */
    hyd_write(bw, 0, 1);
    /* frame_type = kRegularFrame or kSkipProgressive */
    hyd_write(bw, is_last ? 0 : 3, 2);
    /* frame_encoding = VarDCT */
    hyd_write(bw, 0, 1);
    /* flags = kSkipAdaptiveLFSmoothing */
    hyd_write_u64(bw, 0x80);
    /*
     * upsampling = 0:2
     * x_qm_scale = 3:3
     * b_qm_scale = 2:3
     * num_passes = 0:2
     */
    hyd_write(bw, 0x4C, 10);

    hyd_write_bool(bw, have_crop);

    if (have_crop) {
        // have_crop ==> !encoder->one_frame
        size_t frame_w = encoder->lf_group->tile_count_x << 8;
        size_t frame_h = encoder->lf_group->tile_count_y << 8;
        hyd_write_u32(bw, &frame_size_u32, hyd_pack_signed(encoder->lf_group->x * frame_w));
        hyd_write_u32(bw, &frame_size_u32, hyd_pack_signed(encoder->lf_group->y * frame_h));
        hyd_write_u32(bw, &frame_size_u32, encoder->lf_group->width);
        hyd_write_u32(bw, &frame_size_u32, encoder->lf_group->height);
    }

    /* blending_info.mode = kReplace */
    hyd_write(bw, 0, 2);

    /* blending_info.source = 0 */
    if (have_crop)
        hyd_write(bw, 0, 2);

    hyd_write_bool(bw, is_last);

    /* save_as_reference = 0 */
    if (!is_last)
        hyd_write(bw, 0, 2);

    /* name_len = 0:2 */
    hyd_write(bw, 0, 2);

    /* all_default = 0 */
    hyd_write_bool(bw, 0);
    /* gab = 0 */
    hyd_write_bool(bw, 0);
    /* epf_iters = 0 */
    hyd_write(bw, 0, 2);
    /* extensions = 0 */
    hyd_write(bw, 0, 2);

    /*
     * extensions = 0:2
     */
    hyd_write(bw, 0, 2);

    size_t toc_size = 2;
    lehmer = get_lehmer_sequence(encoder, &toc_size);
    if (!lehmer && toc_size > 1)
        return HYD_NOMEM;

    /* permuted toc */
    if (toc_size > 1) {
        hyd_write_bool(bw, 1);
        ret = hyd_entropy_init_stream(&toc_stream, bw, 1 + toc_size, zerobuf,
                                        8, 0, 0, 0, &encoder->error);
        if (ret < HYD_ERROR_START)
            goto end;
        ret = hyd_entropy_send_symbol(&toc_stream, 0, toc_size);
        if (ret < HYD_ERROR_START)
            goto end;
        for (size_t i = 0; i < toc_size; i++) {
            ret = hyd_entropy_send_symbol(&toc_stream, 0, lehmer[i]);
            if (ret < HYD_ERROR_START)
                goto end;
        }
        ret = hyd_prefix_finalize_stream(&toc_stream);
        if (ret < HYD_ERROR_START)
            goto end;
    } else {
        hyd_write_bool(bw, 0);
    }

    ret = hyd_write_zero_pad(bw);
    encoder->wrote_frame_header = 1;

end:
    hyd_freep(&lehmer);
    return ret;
}

HYDStatusCode hyd_populate_lf_group(HYDEncoder *encoder, HYDLFGroup **lf_group_ptr, uint32_t tile_x, uint32_t tile_y) {
    size_t w, h;

    // check bounds
    if (encoder->one_frame) {
        w = h = 2048;
    } else {
        h = encoder->lf_group->tile_count_y << 8;
        w = encoder->lf_group->tile_count_x << 8;
    }

    if (tile_x >= (encoder->metadata.width + w - 1) / w || tile_y >= (encoder->metadata.height + h - 1) / h) {
        encoder->error = "tile out of bounds";
        return HYD_API_ERROR;
    }

    HYDLFGroup *lf_group = &encoder->lf_group[encoder->one_frame ? tile_y * encoder->lf_group_count_x + tile_x : 0];
    lf_group->y = tile_y;
    lf_group->x = tile_x;

    if (encoder->one_frame) {
        lf_group->tile_count_y = 8;
        lf_group->tile_count_x = 8;
    }

    lf_group->height = (tile_y + 1) * h > encoder->metadata.height ? encoder->metadata.height - tile_y * h : h;
    lf_group->width = (tile_x + 1) * w > encoder->metadata.width ? encoder->metadata.width - tile_x * w : w;
    lf_group->varblock_height = (lf_group->height + 7) >> 3;
    lf_group->varblock_width = (lf_group->width + 7) >> 3;
    lf_group->stride = lf_group->varblock_width << 3;

    if (lf_group_ptr)
        *lf_group_ptr = lf_group;

    return HYD_OK;
}

HYDStatusCode hyd_send_tile_pre(HYDEncoder *encoder, uint32_t tile_x, uint32_t tile_y, int is_last) {
    HYDStatusCode ret;

    HYDLFGroup *lf_group = NULL;
    ret = hyd_populate_lf_group(encoder, &lf_group, tile_x, tile_y);
    if (ret < HYD_ERROR_START)
        return ret;

    encoder->last_tile = is_last < 0 ?
        (tile_x + 1) * (lf_group->tile_count_x << 8) >= encoder->metadata.width &&
        (tile_y + 1) * (lf_group->tile_count_y << 8) >= encoder->metadata.height
            : !!is_last;

    if (encoder->writer.overflow_state)
        return encoder->writer.overflow_state;

    if (!encoder->wrote_header) {
        ret = write_header(encoder);
        if (ret < HYD_ERROR_START)
            return ret;
    }

    if (!encoder->one_frame && !encoder->wrote_frame_header) {
        ret = write_frame_header(encoder);
        if (ret < HYD_ERROR_START)
            return ret;
    }

    size_t xyb_pixels = lf_group->varblock_height * lf_group->varblock_width * 64;
    ret = hyd_realloc_array_p(&encoder->xyb, xyb_pixels, sizeof(XYBEntry));
    if (ret < HYD_ERROR_START)
        return ret;

    return HYD_OK;
}

static HYDStatusCode write_lf_global(HYDEncoder *encoder) {
    HYDBitWriter *bw = &encoder->working_writer;

    // LF channel quantization all_default
    hyd_write_bool(bw, 1);

    // quantizer globalScale = 32768
    hyd_write_u32(bw, &global_scale_table, 32768);
    // quantizer quantLF = 4
    hyd_write_u32(bw, &quant_lf_table, 4);
    // HF Block Context all_default
    hyd_write_bool(bw, 1);
    // LF Channel Correlation
    hyd_write_bool(bw, 1);
    // GlobalModular have_global_tree
    return hyd_write_bool(bw, 0);
}

static HYDStatusCode write_lf_group(HYDEncoder *encoder, HYDLFGroup *lf_group) {
    HYDStatusCode ret;
    HYDBitWriter *bw = &encoder->working_writer;
    // extra precision = 0
    hyd_write(bw, 0, 2);
    // use global tree
    hyd_write_bool(bw, 0);
    // wp_params all_default
    hyd_write_bool(bw, 1);
    // nb_transforms = 0
    hyd_write(bw, 0, 2);

    // write MA Tree
    HYDEntropyStream stream;
    ret = hyd_entropy_init_stream(&stream, bw, hyd_array_size(lf_ma_tree),
                                  zerobuf, 6, 0, 0, 0, &encoder->error);
    if (ret < HYD_ERROR_START)
        return ret;
    for (size_t i = 0; i < hyd_array_size(lf_ma_tree); i++) {
        ret = hyd_entropy_send_symbol(&stream, lf_ma_tree[i][0], lf_ma_tree[i][1]);
        if (ret < HYD_ERROR_START)
            return ret;
    }

    ret = hyd_prefix_finalize_stream(&stream);
    if (ret < HYD_ERROR_START)
        return ret;

    size_t nb_blocks = lf_group->varblock_height * lf_group->varblock_width;
    ret = hyd_entropy_init_stream(&stream, bw, 3 * nb_blocks, zerobuf,
                                  1, 1, 1 << 14, 1, &encoder->error);
    if (ret < HYD_ERROR_START)
        return ret;
    ret = hyd_entropy_set_hybrid_config(&stream, 0, 0, 7, 1, 1);
    if (ret < HYD_ERROR_START)
        return ret;
    const float shift[3] = {8192.f, 1024.f, 512.f};
    for (int i = 0; i < 3; i++) {
        const int c = i < 2 ? 1 - i : i;
        for (size_t vy = 0; vy < lf_group->varblock_height; vy++) {
            const size_t y = vy << 3;
            const size_t row = lf_group->stride * y;
            for (size_t vx = 0; vx < lf_group->varblock_width; vx++) {
                const size_t x = vx << 3;
                XYBEntry *xyb = &encoder->xyb[row + x];
                xyb->xyb[c].i = xyb->xyb[c].f * shift[c];
                const int32_t w = x > 0 ? xyb[-8].xyb[c].i : y > 0 ? xyb[-(lf_group->stride << 3)].xyb[c].i : 0;
                const int32_t n = y > 0 ? xyb[-(lf_group->stride << 3)].xyb[c].i : w;
                const int32_t nw = x > 0 && y > 0 ? xyb[-((lf_group->stride + 1) << 3)].xyb[c].i : w;
                const int32_t vp = w + n - nw;
                const int32_t min = hyd_min(w, n);
                /* a ^ b ^ c, when c == a or c == b, gives the other one */
                const int32_t max = w ^ n ^ min;
                const int32_t v = hyd_clamp(vp, min, max);
                hyd_entropy_send_symbol(&stream, 0, hyd_pack_signed(xyb->xyb[c].i - v));
            }
        }
    }
    ret = hyd_prefix_finalize_stream(&stream);
    if (ret < HYD_ERROR_START)
        return ret;
    hyd_write(bw, nb_blocks - 1, hyd_cllog2(nb_blocks));
    hyd_write(bw, 0x2, 4);
    ret = hyd_entropy_init_stream(&stream, bw, 5, zerobuf,
                                  6, 0, 0, 0, &encoder->error);
    if (ret < HYD_ERROR_START)
        return ret;
    hyd_entropy_send_symbol(&stream, 1, 0);
    hyd_entropy_send_symbol(&stream, 2, 0);
    hyd_entropy_send_symbol(&stream, 3, 0);
    hyd_entropy_send_symbol(&stream, 4, 0);
    hyd_entropy_send_symbol(&stream, 5, 0);
    ret = hyd_prefix_finalize_stream(&stream);
    if (ret < HYD_ERROR_START)
        return ret;
    size_t cfl_height = (lf_group->varblock_height + 7) >> 3;
    size_t cfl_width = (lf_group->varblock_width + 7) >> 3;
    size_t num_z_pre = 2 * cfl_width * cfl_height + nb_blocks;
    size_t num_sym = num_z_pre + 2 * nb_blocks;
    ret = hyd_entropy_init_stream(&stream, bw, num_sym, zerobuf, 1, 0, 29, 1, &encoder->error);
    if (ret < HYD_ERROR_START)
        return ret;
    for (size_t i = 0; i < num_z_pre; i++)
        hyd_entropy_send_symbol(&stream, 0, 0);
    for (size_t i = 0; i < nb_blocks; i++)
        hyd_entropy_send_symbol(&stream, 0, (hf_mult - 1) * 2);
    for (size_t i = 0; i < nb_blocks; i++)
        hyd_entropy_send_symbol(&stream, 0, 0);
    ret = hyd_prefix_finalize_stream(&stream);
    if (ret < HYD_ERROR_START)
        return ret;

    return bw->overflow_state;
}

static void forward_dct(HYDEncoder *encoder, HYDLFGroup *lf_group) {
    float scratchblock[2][8][8];
    for (size_t c = 0; c < 3; c++) {
        for (size_t by = 0; by < lf_group->varblock_height; by++) {
            size_t vy = by << 3;
            for (size_t bx = 0; bx < lf_group->varblock_width; bx++) {
                memset(scratchblock, 0, sizeof(scratchblock));
                size_t vx = bx << 3;
                for (size_t y = 0; y < 8; y++) {
                    const size_t posy = (vy + y) * lf_group->stride + vx;
                    scratchblock[0][y][0] = encoder->xyb[posy].xyb[c].f;
                    for (size_t x = 1; x < 8; x++)
                        scratchblock[0][y][0] += encoder->xyb[posy + x].xyb[c].f;
                    scratchblock[0][y][0] *= 0.125f;
                    for (size_t k = 1; k < 8; k++) {
                        for (size_t n = 0; n < 8; n++)
                            scratchblock[0][y][k] += encoder->xyb[posy + n].xyb[c].f * cosine_lut[k - 1][n];
                    }
                }
                for (size_t x = 0; x < 8; x++) {
                    scratchblock[1][0][x] = scratchblock[0][0][x];
                    for (size_t y = 1; y < 8; y++)
                        scratchblock[1][0][x] += scratchblock[0][y][x];
                    scratchblock[1][0][x] *= 0.125f;
                    for (size_t k = 1; k < 8; k++) {
                        for (size_t n = 0; n < 8; n++)
                            scratchblock[1][k][x] += scratchblock[0][n][x] * cosine_lut[k - 1][n];
                    }
                }
                for (size_t y = 0; y < 8; y++) {
                    size_t posy = (vy + y) * lf_group->stride + vx;
                    for (size_t x = 0; x < 8; x++)
                        encoder->xyb[posy + x].xyb[c].f = scratchblock[1][x][y];
                }
            }
        }
    }
}

static uint8_t get_predicted_non_zeroes(uint8_t *nz, size_t y, size_t x, size_t w, int c) {
    if (!x && !y)
        return 32;
    if (!x)
        return nz[((y - 1) * w) * 3 + c];
    if (!y)
        return nz[(x - 1) * 3 + c];
    return (nz[((y - 1) * w + x) * 3 + c] + (uint32_t)nz[((y * w) + (x - 1)) * 3 + c] + 1) >> 1;
}

static size_t get_non_zero_context(size_t predicted, size_t block_context) {
    if (predicted < 8)
        return block_context + 15 * predicted;
    if (predicted > 64)
        predicted = 64;

    return block_context + 15 * (4 + (predicted >> 1));
}

static HYDStatusCode initialize_hf_coeffs(HYDEncoder *encoder, HYDEntropyStream *stream, HYDLFGroup *lf_group,
                                          size_t num_non_zeroes, size_t *symbol_count, uint8_t *non_zeroes,
                                          size_t gindex) {
    HYDStatusCode ret;
    for (size_t gy = 0; gy < lf_group->tile_count_y; gy++) {
        if (gy << 8 >= lf_group->height)
            break;
        const size_t gh = (gy + 1) << 8 > lf_group->height ?
            lf_group->height - (gy << 8) : 256;
        const size_t gbh = (gh + 7) >> 3;
        for (size_t gx = 0; gx < lf_group->tile_count_x; gx++) {
            if (gx << 8 >= lf_group->width)
                break;
            const size_t gw = (gx + 1) << 8 > lf_group->width ?
                lf_group->width - (gx << 8) : 256;
            const size_t gbw = (gw + 7) >> 3;
            for (size_t by = 0; by < gbh; by++) {
                const size_t vy = (by << 3) + (gy << 8);
                for (size_t bx = 0; bx < gbw; bx++) {
                    const size_t vx = (bx << 3) + (gx << 8);
                    for (int i = 0; i < 3; i++) {
                        int c = i < 2 ? 1 - i : i;
                        uint8_t predicted = get_predicted_non_zeroes(non_zeroes, by, bx, gbw, c);
                        size_t block_context = hf_block_cluster_map[13 * i];
                        size_t non_zero_context = get_non_zero_context(predicted, block_context);
                        uint32_t non_zero_count = non_zeroes[(by * gbw + bx) * 3 + c];
                        ret = hyd_entropy_send_symbol(stream, non_zero_context, non_zero_count);
                        symbol_count[gindex]++;
                        if (ret < HYD_ERROR_START)
                            return ret;
                        if (!non_zero_count)
                            continue;
                        size_t hist_context = 458 * block_context + 555;
                        for (int k = 0; k < 63; k++) {
                            IntPos pos = natural_order[k + 1];
                            IntPos prev_pos = natural_order[k];
                            const size_t prev_pos_s = (vy + prev_pos.y) * lf_group->stride + (vx + prev_pos.x);
                            const size_t pos_s = (vy + pos.y) * lf_group->stride + (vx + pos.x);
                            int prev = k ? !!encoder->xyb[prev_pos_s].xyb[c].i : non_zero_count <= 4;
                            size_t coeff_context = hist_context + prev +
                                ((coeff_num_non_zero_context[non_zero_count] + coeff_freq_context[k + 1]) << 1);
                            uint32_t value = hyd_pack_signed(encoder->xyb[pos_s].xyb[c].i);
                            ret = hyd_entropy_send_symbol(stream, coeff_context, value);
                            symbol_count[gindex]++;
                            if (ret < HYD_ERROR_START)
                                return ret;
                            if (value && !--non_zero_count)
                                break;
                        }
                    }
                }
            }
            non_zeroes += 3 << 10;
            gindex++;
        }
    }

    return encoder->working_writer.overflow_state;
}

static HYDStatusCode realloc_working_buffer(uint8_t **buffer, size_t *buffer_size) {
    size_t new_size = *buffer_size << 1;
    HYDStatusCode ret = hyd_realloc_p(buffer, new_size);
    if (ret < HYD_ERROR_START)
        return ret;
    *buffer_size = new_size;

    return HYD_OK;
}

HYDStatusCode hyd_encode_xyb_buffer(HYDEncoder *encoder, size_t tile_x, size_t tile_y) {
    uint8_t *non_zeroes = NULL;
    HYDStatusCode ret = HYD_OK;
    int need_buffer_init = !encoder->working_writer.buffer || !encoder->one_frame;
    if (!encoder->working_writer.buffer) {
        encoder->working_writer.buffer = malloc(1 << 12);
        if (!encoder->working_writer.buffer) {
            ret = HYD_NOMEM;
            goto end;
        }
        encoder->working_writer.buffer_len = 1 << 12;
    }
    if (need_buffer_init) {
        ret = hyd_init_bit_writer(&encoder->working_writer, encoder->working_writer.buffer,
                                   encoder->working_writer.buffer_len, 0, 0);
        encoder->copy_pos = 0;
        encoder->working_writer.realloc_func = &realloc_working_buffer;
    }

    if (ret < HYD_ERROR_START)
        goto end;

    const size_t lfid = encoder->one_frame ? tile_y * encoder->lf_group_count_x + tile_x : 0;
    HYDLFGroup *lf_group = &encoder->lf_group[lfid];
    forward_dct(encoder, lf_group);
    size_t frame_h = encoder->one_frame ? encoder->metadata.height : encoder->lf_group->height;
    size_t frame_w = encoder->one_frame ? encoder->metadata.width : encoder->lf_group->width;
    size_t frame_groups_y = (frame_h + 255) >> 8;
    size_t frame_groups_x = (frame_w + 255) >> 8;
    size_t num_frame_groups = frame_groups_x * frame_groups_y;
    const size_t num_groups = ((lf_group->width + 255) >> 8) * ((lf_group->height + 255) >> 8);
    non_zeroes = calloc(3072, num_groups);
    if (!non_zeroes) {
        ret = HYD_NOMEM;
        goto end;
    }

    size_t non_zero_count = 0;
    size_t gindex = 0;
    const size_t lf_pad_w = lf_group->varblock_width << 3;
    for (size_t gy = 0; gy < lf_group->tile_count_y; gy++) {
        if (gy << 5 >= lf_group->varblock_height)
            break;
        const size_t gh = ((gy + 1) << 8) > lf_group->height ?
            lf_group->height - (gy << 8) : 256;
        const size_t gbh = (gh + 7) >> 3;
        for (size_t gx = 0; gx < lf_group->tile_count_x; gx++) {
            if (gx << 5 >= lf_group->varblock_width)
                break;
            const size_t gw = (gx + 1) << 8 > lf_group->width ?
                lf_group->width - (gx << 8) : 256;
            const size_t gbw = (gw + 7) >> 3;
            for (size_t by = 0; by < gbh; by++) {
                const size_t vy = (by << 3) + (gy << 8);
                for (size_t bx = 0; bx < gbw; bx++) {
                    const size_t vx = (bx << 3) + (gx << 8);
                    for (int i = 0; i < 3; i++) {
                        size_t nzc = 0;
                        for (int j = 1; j < 64; j++) {
                            const size_t py = vy + natural_order[j].y;
                            const size_t px = vx + natural_order[j].x;
                            XYBEntry *xyb = &encoder->xyb[py * lf_pad_w + px];
                            const int32_t q = (int32_t)(xyb->xyb[i].f * (hf_quant_weights[i][j] * hf_mult));
                            xyb->xyb[i].i = hyd_abs(q) < 2 ? 0 : q;
                            if (xyb->xyb[i].i) {
                                non_zeroes[((gindex << 10) + by * gbw + bx) * 3 + i]++;
                                nzc = j;
                            }
                        }
                        non_zero_count += nzc;
                    }
                }
            }
            gindex++;
        }
    }

    if (!encoder->tiles_sent) {
        if (num_frame_groups > 1) {
            encoder->section_endpos = calloc(2 + encoder->lf_groups_per_frame + num_frame_groups, sizeof(size_t));
            if (!encoder->section_endpos) {
                ret = HYD_NOMEM;
                goto end;
            }
            encoder->section_count = 0;
        }
        ret = write_lf_global(encoder);
        if (ret < HYD_ERROR_START)
            goto end;
        if (num_frame_groups > 1) {
            hyd_bitwriter_flush(&encoder->working_writer);
            encoder->section_endpos[encoder->section_count++] = encoder->working_writer.buffer_pos;
        }
    }

    ret = write_lf_group(encoder, lf_group);
    if (ret < HYD_ERROR_START)
        goto end;

    if (num_frame_groups > 1) {
        hyd_bitwriter_flush(&encoder->working_writer);
        encoder->section_endpos[encoder->section_count++] = encoder->working_writer.buffer_pos;
    }

    if (!encoder->tiles_sent) {
        const size_t num_syms = 1 << 12;
        memset(&encoder->hf_stream, 0, sizeof(HYDEntropyStream));
        ret = hyd_entropy_init_stream(&encoder->hf_stream, &encoder->working_writer,
                                num_syms, hf_cluster_map, 7425, 1, 0, 0, &encoder->error);
        if (ret < HYD_ERROR_START)
            goto end;
        ret = hyd_entropy_set_hybrid_config(&encoder->hf_stream, 0, 0, 4, 1, 0);
        if (ret < HYD_ERROR_START)
            goto end;
    }

    if (!encoder->hf_stream_barrier)
        encoder->hf_stream_barrier = calloc(num_frame_groups, sizeof(size_t));
    if (!encoder->hf_stream_barrier) {
        ret = HYD_NOMEM;
        goto end;
    }

    ret = initialize_hf_coeffs(encoder, &encoder->hf_stream, lf_group, non_zero_count,
            encoder->hf_stream_barrier, non_zeroes, encoder->groups_encoded);
    if (ret < HYD_ERROR_START)
        goto end;
    if (encoder->one_frame)
        encoder->groups_encoded += num_groups;

    if (encoder->one_frame && !encoder->last_tile)
        goto end;

    // default params HFGlobal
    hyd_write_bool(&encoder->working_writer, 1);
    // num hf presets
    hyd_write(&encoder->working_writer, 0, hyd_cllog2(num_frame_groups));
    // HF Pass order
    hyd_write(&encoder->working_writer, 2, 2);
    ret = hyd_ans_write_stream_header(&encoder->hf_stream);
    if (ret < HYD_ERROR_START)
        goto end;
    if (num_frame_groups > 1) {
        hyd_bitwriter_flush(&encoder->working_writer);
        encoder->section_endpos[encoder->section_count++] = encoder->working_writer.buffer_pos;
    }

    size_t soff = 0;
    for (size_t g = 0; g < num_frame_groups; g++) {
        ret = hyd_ans_write_stream_symbols(&encoder->hf_stream, soff, encoder->hf_stream_barrier[g]);
        if (ret < HYD_ERROR_START)
            goto end;
        soff += encoder->hf_stream_barrier[g];
        if (num_frame_groups > 1) {
            hyd_bitwriter_flush(&encoder->working_writer);
            encoder->section_endpos[encoder->section_count++] = encoder->working_writer.buffer_pos;
        }
    }
    encoder->hf_stream.symbol_pos = 0;

    // write TOC to main buffer
    hyd_bitwriter_flush(&encoder->working_writer);

    if (!encoder->wrote_frame_header) {
        ret = write_frame_header(encoder);
        if (ret < HYD_ERROR_START)
            return ret;
    }

    hyd_write_zero_pad(&encoder->writer);

    if (num_frame_groups > 1) {
        size_t last_end_pos = 0;
        for (size_t index = 0; index < encoder->section_count; index++) {
            hyd_write_u32(&encoder->writer, &toc_table, encoder->section_endpos[index] - last_end_pos);
            last_end_pos = encoder->section_endpos[index];
        }
        encoder->section_count = 0;
    } else {
        hyd_write_u32(&encoder->writer, &toc_table, encoder->working_writer.buffer_pos);
    }

    hyd_write_zero_pad(&encoder->writer);

    encoder->wrote_frame_header = 0;
    ret = hyd_flush(encoder);
    hyd_entropy_stream_destroy(&encoder->hf_stream);
    hyd_freep(&encoder->section_endpos);
    hyd_freep(&encoder->hf_stream_barrier);

end:
    hyd_freep(&non_zeroes);
    return ret;
}
