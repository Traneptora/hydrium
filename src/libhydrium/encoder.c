/*
 * Base encoder implementation
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bitwriter.h"
#include "entropy.h"
#include "internal.h"
#include "math-functions.h"
#include "memory.h"
#include "xyb.h"

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

static const int32_t cosine_lut[7][8] = {
    {45450,  38531,  25745,   9040,  -9040, -25745, -38531, -45450},
    {42813,  17733, -17733, -42813, -42813, -17733,  17733,  42813},
    {38531,  -9040, -45450, -25745,  25745,  45450,   9040, -38531},
    {32767, -32767, -32767,  32767,  32767, -32767, -32767,  32767},
    {25745, -45450,   9040,  38531, -38531,  -9040,  45450, -25745},
    {17733, -42813,  42813, -17733, -17733,  42813, -42813,  17733},
    {9040,  -25745,  38531, -45450,  45450, -38531,  25745,  -9040},
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
    hyd_write_u32(bw, (const uint32_t[4]){1, 1, 1, 1}, (const uint32_t[4]){9, 13, 18, 30}, encoder->metadata.height);
    hyd_write(bw, 0, 3);
    hyd_write_u32(bw, (const uint32_t[4]){1, 1, 1, 1}, (const uint32_t[4]){9, 13, 18, 30}, encoder->metadata.width);

    /* all_default:1, default_m:1 */
    hyd_write(bw, 0x3, 2);

    encoder->wrote_header = 1;
    return bw->overflow_state;
}

static HYDStatusCode write_frame_header(HYDEncoder *encoder) {
    HYDBitWriter *bw = &encoder->writer;
    HYDStatusCode ret;

    if (bw->overflow_state)
        return bw->overflow_state;

    hyd_write_zero_pad(bw);

    size_t w = encoder->tile_count_x << 8;
    size_t h = encoder->tile_count_y << 8;

    encoder->lf_group_width = (encoder->lf_group_x + 1) * w > encoder->metadata.width ?
                            encoder->metadata.width - encoder->lf_group_x * w : w;
    encoder->lf_group_height = (encoder->lf_group_y + 1) * h > encoder->metadata.height ?
                            encoder->metadata.height - encoder->lf_group_y * h : h;
    encoder->lf_varblock_width = (encoder->lf_group_width + 7) >> 3;
    encoder->lf_varblock_height = (encoder->lf_group_height + 7) >> 3;

    /*
     * all_default = 0:1
     * frame_type = 0:2
     * encoding = 0:1
     */
    hyd_write(bw, 0, 4);
    /* flags = kSkipAdaptiveLFSmoothing */
    hyd_write_u64(bw, 0x80);
    /*
     * upsampling = 0:2
     * x_qm_scale = 3:3
     * b_qm_scale = 2:3
     * num_passes = 0:2
     */
    hyd_write(bw, 0x4C, 10);

    int is_last = (encoder->lf_group_x + 1) * w >= encoder->metadata.width
        && (encoder->lf_group_y + 1) * h >= encoder->metadata.height;
    int have_crop = !is_last || encoder->lf_group_x != 0 || encoder->lf_group_y != 0;

    hyd_write_bool(bw, have_crop);

    if (have_crop) {
        const uint32_t cpos[4] = {0, 256, 2304, 18688};
        const uint32_t upos[4] = {8, 11, 14, 30};
        hyd_write_u32(bw, cpos, upos, hyd_pack_signed(encoder->lf_group_x * w));
        hyd_write_u32(bw, cpos, upos, hyd_pack_signed(encoder->lf_group_y * h));
        hyd_write_u32(bw, cpos, upos, encoder->lf_group_width);
        hyd_write_u32(bw, cpos, upos, encoder->lf_group_height);
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
     * permuted_toc = 0:1
     */
    hyd_write(bw, 0, 3);

    ret = hyd_write_zero_pad(bw);
    encoder->wrote_frame_header = 1;

    return ret;
}

static HYDStatusCode send_tile_pre(HYDEncoder *encoder, uint32_t tile_x, uint32_t tile_y) {
    HYDStatusCode ret;

    size_t w = encoder->tile_count_x << 8;
    size_t h = encoder->tile_count_y << 8;

    if (tile_x >= (encoder->metadata.width + w - 1) / w || tile_y >= (encoder->metadata.height + h - 1) / h)
        return HYD_API_ERROR;

    if (encoder->writer.overflow_state)
        return encoder->writer.overflow_state;

    encoder->lf_group_x = tile_x;
    encoder->lf_group_y = tile_y;

    if (!encoder->wrote_header) {
        if ((ret = write_header(encoder)) < HYD_ERROR_START)
            return ret;
    }

    if (!encoder->wrote_frame_header) {
        if ((ret = write_frame_header(encoder)) < HYD_ERROR_START)
            return ret;
    }

    if (!encoder->xyb)
        encoder->xyb = hyd_malloc(&encoder->allocator,
            3 * encoder->lf_group_height * encoder->lf_group_width * sizeof(int16_t));
    if (!encoder->xyb)
        return HYD_NOMEM;

    return HYD_OK;
}

static HYDStatusCode write_lf_global(HYDEncoder *encoder) {
    HYDBitWriter *bw = &encoder->working_writer;

    // LF channel quantization all_default
    hyd_write_bool(bw, 1);

    // quantizer globalScale = 32768
    hyd_write_u32(bw, (const uint32_t[4]){1, 2049, 4097, 8193}, (const uint32_t[4]){11, 11, 12, 16}, 32768);
    // quantizer quantLF = 64
    hyd_write_u32(bw, (const uint32_t[4]){16, 1, 1, 1}, (const uint32_t[4]){0, 5, 8, 16}, 64);
    // HF Block Context all_default
    hyd_write_bool(bw, 1);
    // LF Channel Correlation
    hyd_write_bool(bw, 1);
    // GlobalModular have_global_tree
    return hyd_write_bool(bw, 0);
}

static HYDStatusCode write_lf_group(HYDEncoder *encoder, const uint16_t *hf_mult) {
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
    HYDEntropyStream stream;
    ret = hyd_entropy_init_stream(&stream, &encoder->allocator, bw, 5, (const uint8_t[6]){0, 0, 0, 0, 0, 0}, 6, 0, 0);
    if (ret < HYD_ERROR_START)
        return ret;
    // property = -1
    hyd_entropy_send_symbol(&stream, 1, 0);
    // predictor = 5
    hyd_entropy_send_symbol(&stream, 2, 5);
    // offset = 0
    hyd_entropy_send_symbol(&stream, 3, 0);
    // mul_log = 0
    hyd_entropy_send_symbol(&stream, 4, 0);
    // mul_bits = 0
    hyd_entropy_send_symbol(&stream, 5, 0);
    if ((ret = hyd_prefix_finalize_stream(&stream)) < HYD_ERROR_START)
        return ret;
    size_t nb_blocks = encoder->lf_varblock_width * encoder->lf_varblock_height;
    ret = hyd_entropy_init_stream(&stream, &encoder->allocator, bw, 3 * nb_blocks, (const uint8_t[1]){0}, 1, 1, 0);
    if (ret < HYD_ERROR_START)
        return ret;
    hyd_entropy_set_hybrid_config(&stream, 0, 0, 7, 1, 0);
    const int shift[3] = {3, 0, -1};
    for (int i = 0; i < 3; i++) {
        int c = i < 2 ? 1 - i : i;
        for (size_t vy = 0; vy < encoder->lf_varblock_height; vy++) {
            const size_t y = vy << 3;
            const size_t row = encoder->lf_group_width * y;
            for (size_t vx = 0; vx < encoder->lf_varblock_width; vx++) {
                const size_t x = vx << 3;
                int16_t *xyb = encoder->xyb + ((row + x) * 3 + c);
                *xyb = shift[c] >= 0 ? *xyb << shift[c] : *xyb >> -shift[c];
                int32_t w = x > 0 ? *(xyb - 24) : y > 0 ? *(xyb - 24 * encoder->lf_group_width) : 0;
                int32_t n = y > 0 ? *(xyb - 24 * encoder->lf_group_width) : w;
                int32_t nw = x > 0 && y > 0 ? *(xyb - 24 * (encoder->lf_group_width + 1)) : w;
                int32_t v = w + n - nw;
                int32_t min = hyd_min(w, n);
                int32_t max = hyd_max(w, n);
                v = hyd_clamp(v, min, max);
                int32_t diff = *xyb - v;
                hyd_entropy_send_symbol(&stream, 0, hyd_pack_signed(diff));
            }
        }
    }
    if ((ret = hyd_prefix_finalize_stream(&stream)) < HYD_ERROR_START)
        return ret;
    hyd_write(bw, nb_blocks - 1, hyd_cllog2(nb_blocks));
    hyd_write(bw, 0x2, 4);
    ret = hyd_entropy_init_stream(&stream, &encoder->allocator, bw, 5, (const uint8_t[6]){0, 0, 0, 0, 0, 0}, 6, 0, 0);
    if (ret < HYD_ERROR_START)
        return ret;
    hyd_entropy_send_symbol(&stream, 1, 0);
    hyd_entropy_send_symbol(&stream, 2, 0);
    hyd_entropy_send_symbol(&stream, 3, 0);
    hyd_entropy_send_symbol(&stream, 4, 0);
    hyd_entropy_send_symbol(&stream, 5, 0);
    if ((ret = hyd_prefix_finalize_stream(&stream)) < HYD_ERROR_START)
        return ret;
    size_t cfl_width = (encoder->lf_varblock_width + 7) >> 3;
    size_t cfl_height = (encoder->lf_varblock_height + 7) >> 3;
    size_t num_z_pre = 2 * cfl_width * cfl_height + nb_blocks;
    size_t num_sym = num_z_pre + 2 * nb_blocks;
    hyd_entropy_init_stream(&stream, &encoder->allocator, bw, num_sym, (const uint8_t[1]){0}, 1, 0, 0);
    for (size_t i = 0; i < num_z_pre; i++)
        hyd_entropy_send_symbol(&stream, 0, 0);
    for (size_t i = 0; i < nb_blocks; i++)
        hyd_entropy_send_symbol(&stream, 0, (hf_mult[i] - 1) * 2);
    for (size_t i = 0; i < nb_blocks; i++)
        hyd_entropy_send_symbol(&stream, 0, 0);
    if ((ret = hyd_prefix_finalize_stream(&stream)) < HYD_ERROR_START)
        return ret;

    return bw->overflow_state;
}

static void forward_dct(HYDEncoder *encoder) {
    int32_t scratchblock[2][8][8];
    for (size_t c = 0; c < 3; c++) {
        for (size_t by = 0; by < encoder->lf_varblock_height; by++) {
            size_t vy = by << 3;
            for (size_t bx = 0; bx < encoder->lf_varblock_width; bx++) {
                memset(scratchblock, 0, sizeof(scratchblock));
                size_t vx = bx << 3;
                for (size_t y = 0; y < 8; y++) {
                    const size_t posy = (vy + y) * encoder->lf_group_width + vx;
                    scratchblock[0][y][0] = encoder->xyb[posy * 3 + c];
                    for (size_t x = 1; x < 8; x++)
                        scratchblock[0][y][0] += encoder->xyb[(posy + x) * 3 + c];
                    scratchblock[0][y][0] >>= 3;
                    for (size_t k = 1; k < 8; k++) {
                        for (size_t n = 0; n < 8; n++)
                            scratchblock[0][y][k] += encoder->xyb[(posy + n) * 3 + c] * cosine_lut[k - 1][n];
                        scratchblock[0][y][k] = hyd_signed_rshift32(scratchblock[0][y][k], 18);
                    }
                }
                for (size_t x = 0; x < 8; x++) {
                    scratchblock[1][0][x] = scratchblock[0][0][x];
                    for (size_t y = 1; y < 8; y++)
                        scratchblock[1][0][x] += scratchblock[0][y][x];
                    scratchblock[1][0][x] >>= 3;
                    for (size_t k = 1; k < 8; k++) {
                        for (size_t n = 0; n < 8; n++)
                            scratchblock[1][k][x] += scratchblock[0][n][x] * cosine_lut[k - 1][n];
                        scratchblock[1][k][x] = hyd_signed_rshift32(scratchblock[1][k][x], 18);
                    }
                }
                for (size_t y = 0; y < 8; y++) {
                    size_t posy = (vy + y) * encoder->lf_group_width + vx;
                    for (size_t x = 0; x < 8; x++)
                        encoder->xyb[(posy + x) * 3 + c] = scratchblock[1][x][y];
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

static int16_t hf_quant(int64_t value, int32_t weight, int32_t hf_mult) {
    const int64_t v = hyd_signed_rshift64(value * weight * hf_mult, 14);
    return v < hf_mult - 3 && -v < hf_mult - 3 ? 0 : v;
}

static HYDStatusCode initialize_hf_coeffs(HYDEncoder *encoder, HYDEntropyStream *stream,
                                          size_t num_non_zeroes, size_t *symbol_count, uint8_t *non_zeroes) {
    HYDStatusCode ret;
    uint8_t map[7425];
    for (int k = 0; k < 15; k++) {
        memset(map + 37 * k, k, 37);
        for (int j = 0; j < 229; j++) {
            map[555 + 458 * k + 2 * j] = k + 15;
            map[555 + 458 * k + 2 * j + 1] = k + 30;
        }
    }
    const size_t num_syms = 3 * encoder->lf_varblock_width * encoder->lf_varblock_height + num_non_zeroes;
    hyd_entropy_init_stream(stream, &encoder->allocator, &encoder->working_writer,
                        num_syms, map, 7425, 1, 0);
    hyd_entropy_set_hybrid_config(stream, 0, 0, 4, 1, 0);
    size_t gindex = 0;
    for (size_t gy = 0; gy < encoder->tile_count_y; gy++) {
        if (gy << 8 >= encoder->lf_group_height)
            break;
        const size_t gh = (gy + 1) << 8 > encoder->lf_group_height ?
            encoder->lf_group_height - (gy << 8) : 256;
        const size_t gbh = (gh + 7) >> 3;
        for (size_t gx = 0; gx < encoder->tile_count_x; gx++) {
            if (gx << 8 >= encoder->lf_group_width)
                break;
            const size_t gw = (gx + 1) << 8 > encoder->lf_group_width ?
                encoder->lf_group_width - (gx << 8) : 256;
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
                            const size_t prev_pos_s = (vy + prev_pos.y) * encoder->lf_group_width + (vx + prev_pos.x);
                            const size_t pos_s = (vy + pos.y) * encoder->lf_group_width + (vx + pos.x);
                            int prev = k ? !!encoder->xyb[prev_pos_s * 3 + c] : non_zero_count <= 4;
                            size_t coeff_context = hist_context + prev +
                                ((coeff_num_non_zero_context[non_zero_count] + coeff_freq_context[k + 1]) << 1);
                            uint32_t value = hyd_pack_signed(encoder->xyb[pos_s * 3 + c]);
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

    if ((ret = hyd_ans_write_stream_header(stream)) < HYD_ERROR_START)
        return ret;

    return encoder->working_writer.overflow_state;
}

static HYDStatusCode realloc_working_buffer(HYDAllocator *allocator, uint8_t **buffer, size_t *buffer_size) {
    size_t new_size = *buffer_size << 1;
    uint8_t *new_buffer = hyd_realloc(allocator, *buffer, new_size);
    if (!new_buffer)
        return HYD_NOMEM;
    *buffer = new_buffer;
    *buffer_size = new_size;

    return HYD_OK;
}

static HYDStatusCode encode_xyb_buffer(HYDEncoder *encoder) {

    size_t *section_endpos = NULL;
    uint8_t *non_zeroes = NULL;
    uint16_t *hf_mult = NULL;
    HYDEntropyStream hf_stream = { 0 };
    HYDStatusCode ret = HYD_OK;
    if (!encoder->working_writer.buffer) {
        encoder->working_writer.buffer = hyd_malloc(&encoder->allocator, 1 << 12);
        if (!encoder->working_writer.buffer) {
            ret = HYD_NOMEM;
            goto end;
        }
        ret = hyd_init_bit_writer(&encoder->working_writer, encoder->working_writer.buffer, 1 << 12, 0, 0);
    } else {
        ret = hyd_init_bit_writer(&encoder->working_writer, encoder->working_writer.buffer,
                                   encoder->working_writer.buffer_len, 0, 0);
    }
    if (ret < HYD_ERROR_START)
        goto end;
    encoder->working_writer.allocator = &encoder->allocator;
    encoder->working_writer.realloc_func = &realloc_working_buffer;
    encoder->copy_pos = 0;

    forward_dct(encoder);

    const size_t num_groups = ((encoder->lf_group_width + 255) >> 8) * ((encoder->lf_group_height + 255) >> 8);

    const size_t nzero_size = (num_groups << 11) + (num_groups << 10);
    non_zeroes = hyd_calloc(&encoder->allocator, 1, nzero_size); // num_groups * 3 * 32 * 32
    hf_mult = hyd_malloc(&encoder->allocator,
        sizeof(uint16_t) * encoder->lf_varblock_width * encoder->lf_varblock_height);
    if (!non_zeroes || !hf_mult) {
        ret = HYD_NOMEM;
        goto end;
    }

    size_t non_zero_count = 0;
    size_t gindex = 0;
    for (size_t gy = 0; gy < encoder->tile_count_y; gy++) {
        if (gy << 8 >= encoder->lf_group_height)
            break;
        const size_t gh = (gy + 1) << 8 > encoder->lf_group_height ?
            encoder->lf_group_height - (gy << 8) : 256;
        const size_t gbh = (gh + 7) >> 3;
        for (size_t gx = 0; gx < encoder->tile_count_x; gx++) {
            if (gx << 8 >= encoder->lf_group_width)
                break;
            const size_t gw = (gx + 1) << 8 > encoder->lf_group_width ?
                encoder->lf_group_width - (gx << 8) : 256;
            const size_t gbw = (gw + 7) >> 3;
            for (size_t by = 0; by < gbh; by++) {
                const size_t vy = (by << 3) + (gy << 8);
                const size_t vy7 = (vy + 7) * encoder->lf_group_width;
                const size_t vy6 = (vy + 6) * encoder->lf_group_width;
                for (size_t bx = 0; bx < gbw; bx++) {
                    const size_t vx = (bx << 3) + (gx << 8);
                    const uint32_t hf = (((((encoder->xyb[3 * (vy7 + vx + 7) + 1] & UINT32_C(0x7FFF))
                                        + (encoder->xyb[3 * (vy7 + vx + 7) + 2] & UINT32_C(0x7FFF))) << 1)
                                        + (encoder->xyb[3 * (vy7 + vx + 6) + 1] & UINT32_C(0x7FFF))
                                        + (encoder->xyb[3 * (vy6 + vx + 7) + 1] & UINT32_C(0x7FFF))
                                        + (encoder->xyb[3 * (vy7 + vx + 6) + 2] & UINT32_C(0x7FFF))
                                        + (encoder->xyb[3 * (vy6 + vx + 7) + 2] & UINT32_C(0x7FFF))) >> 14) & UINT32_C(0xF);
                    const size_t hf_mult_pos = (vy >> 3) * encoder->lf_varblock_width + (vx >> 3);
                    hf_mult[hf_mult_pos] = hyd_max(hf, 5);
                    for (int i = 0; i < 3; i++) {
                        size_t nzc = 0;
                        for (int j = 1; j < 64; j++) {
                            const size_t py = vy + natural_order[j].y;
                            const size_t px = vx + natural_order[j].x;
                            int16_t *xyb = encoder->xyb + ((py * encoder->lf_group_width + px) * 3 + i);
                            *xyb = hf_quant(*xyb, hf_quant_weights[i][j], hf_mult[hf_mult_pos]);
                            if (*xyb) {
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

    // Output sections to working buffer

    section_endpos = num_groups > 1 ? hyd_calloc(&encoder->allocator, (2 + 1 + num_groups), sizeof(size_t)) : NULL;
    if (num_groups > 1 && !section_endpos) {
        ret = HYD_NOMEM;
        goto end;
    }

    size_t section = 0;

    if ((ret = write_lf_global(encoder)) < HYD_ERROR_START)
        goto end;

    if (num_groups > 1) {
        hyd_bitwriter_flush(&encoder->working_writer);
        section_endpos[section++] = encoder->working_writer.buffer_pos;
    }

    if ((ret = write_lf_group(encoder, hf_mult)) < HYD_ERROR_START)
        goto end;

    if (num_groups > 1) {
        hyd_bitwriter_flush(&encoder->working_writer);
        section_endpos[section++] = encoder->working_writer.buffer_pos;
    }

    // default params HFGlobal
    hyd_write_bool(&encoder->working_writer, 1);
    // num hf presets
    hyd_write(&encoder->working_writer, 0, hyd_cllog2(num_groups));
    // HF Pass order
    hyd_write(&encoder->working_writer, 2, 2);

    size_t symbol_count[64] = { 0 };

    if ((ret = initialize_hf_coeffs(encoder, &hf_stream, non_zero_count, symbol_count, non_zeroes)) < HYD_ERROR_START)
        goto end;

    if (num_groups > 1) {
        hyd_bitwriter_flush(&encoder->working_writer);
        section_endpos[section++] = encoder->working_writer.buffer_pos;
    }

    size_t soff = 0;
    gindex = 0;
    for (size_t gy = 0; gy < encoder->tile_count_y; gy++) {
        if (gy << 8 >= encoder->lf_group_height)
            break;
        for (size_t gx = 0; gx < encoder->tile_count_x; gx++) {
            if (gx << 8 >= encoder->lf_group_width)
                break;
            if ((ret = hyd_ans_write_stream_symbols(&hf_stream, soff, symbol_count[gindex])) < HYD_ERROR_START)
                goto end;
            soff += symbol_count[gindex];
            if (num_groups > 1) {
                hyd_bitwriter_flush(&encoder->working_writer);
                section_endpos[section++] = encoder->working_writer.buffer_pos;
            }
            gindex++;
        }
    }

    // write TOC to main buffer
    hyd_bitwriter_flush(&encoder->working_writer);

    hyd_write_zero_pad(&encoder->writer);
    if (num_groups > 1) {
        size_t last_end_pos = 0;
        for (size_t index = 0; index < section; index++) {
            hyd_write_u32(&encoder->writer, (const uint32_t[4]){0, 1024, 17408, 4211712},
                                            (const uint32_t[4]){10, 14, 22, 30},
                                            section_endpos[index] - last_end_pos);
            last_end_pos = section_endpos[index];
        }
    } else {
        hyd_write_u32(&encoder->writer, (const uint32_t[4]){0, 1024, 17408, 4211712},
                                        (const uint32_t[4]){10, 14, 22, 30},
                                        encoder->working_writer.buffer_pos);
    }

    hyd_write_zero_pad(&encoder->writer);

    ret = hyd_flush(encoder);
    if (ret == HYD_OK)
        encoder->wrote_frame_header = 0;

end:
    hyd_entropy_stream_destroy(&hf_stream);
    hyd_free(&encoder->allocator, section_endpos);
    hyd_free(&encoder->allocator, hf_mult);
    hyd_free(&encoder->allocator, non_zeroes);
    return ret;
}

HYDRIUM_EXPORT HYDStatusCode hyd_send_tile(HYDEncoder *encoder, const uint16_t *const buffer[3],
                                           uint32_t tile_x, uint32_t tile_y,
                                           ptrdiff_t row_stride, ptrdiff_t pixel_stride) {
    HYDStatusCode ret;
    if ((ret = send_tile_pre(encoder, tile_x, tile_y)) < HYD_ERROR_START)
        return ret;

    if ((ret = hyd_populate_xyb_buffer(encoder, buffer, row_stride, pixel_stride)) < HYD_ERROR_START)
        return ret;

    return encode_xyb_buffer(encoder);
}

HYDRIUM_EXPORT HYDStatusCode hyd_send_tile8(HYDEncoder *encoder, const uint8_t *const buffer[3],
                                            uint32_t tile_x, uint32_t tile_y,
                                            ptrdiff_t row_stride, ptrdiff_t pixel_stride) {
    HYDStatusCode ret;
    if ((ret = send_tile_pre(encoder, tile_x, tile_y)) < HYD_ERROR_START)
        return ret;

    if ((ret = hyd_populate_xyb_buffer8(encoder, buffer, row_stride, pixel_stride)) < HYD_ERROR_START)
        return ret;

    return encode_xyb_buffer(encoder);
}
