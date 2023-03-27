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

    encoder->group_width = (encoder->group_x + 1) << 8 > encoder->metadata.width ? 
                            encoder->metadata.width - (encoder->group_x << 8) : 256;
    encoder->group_height = (encoder->group_y + 1) << 8 > encoder->metadata.height ? 
                            encoder->metadata.height - (encoder->group_y << 8) : 256;
    encoder->varblock_width = (encoder->group_width + 7) >> 3;
    encoder->varblock_height = (encoder->group_height + 7) >> 3;

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

    int is_last = (encoder->group_x + 1) << 8 >= encoder->metadata.width
        && (encoder->group_y + 1) << 8 >= encoder->metadata.height;
    int have_crop = !is_last || encoder->group_x != 0 || encoder->group_y != 0;

    hyd_write_bool(bw, have_crop);

    if (have_crop) {
        const uint32_t cpos[4] = {0, 256, 2304, 18688};
        const uint32_t upos[4] = {8, 11, 14, 30};
        /* extra factor of 2 here because UnpackSigned */
        hyd_write_u32(bw, cpos, upos, encoder->group_x << 9);
        hyd_write_u32(bw, cpos, upos, encoder->group_y << 9);
        hyd_write_u32(bw, cpos, upos, encoder->group_width);
        hyd_write_u32(bw, cpos, upos, encoder->group_height);
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

    if (tile_x >= (encoder->metadata.width + 255) >> 8 || tile_y >= (encoder->metadata.height + 255) >> 8)
        return HYD_API_ERROR;

    if (encoder->writer.overflow_state)
        return encoder->writer.overflow_state;

    encoder->group_x = tile_x;
    encoder->group_y = tile_y;

    if (!encoder->wrote_header) {
        if ((ret = write_header(encoder)) < HYD_ERROR_START)
            return ret;
    }

    if (!encoder->wrote_frame_header) {
        if ((ret = write_frame_header(encoder)) < HYD_ERROR_START)
            return ret;
    }

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
    if ((ret = hyd_prefix_write_stream_header(&stream)) < HYD_ERROR_START)
        return ret;
    if ((ret = hyd_prefix_finalize_stream(&stream)) < HYD_ERROR_START)
        return ret;
    size_t nb_blocks = encoder->varblock_width * encoder->varblock_height;
    ret = hyd_entropy_init_stream(&stream, &encoder->allocator, bw, 3 * nb_blocks, (const uint8_t[1]){0}, 1, 1, 0);
    if (ret < HYD_ERROR_START)
        return ret;
    hyd_entropy_set_hybrid_config(&stream, 0, 1, 4, 1, 0);
    const int shift[3] = {3, 0, -1};
    for (int i = 0; i < 3; i++) {
        int c = i < 2 ? 1 - i : i;
        for (size_t y = 0; y < encoder->varblock_height; y++) {
            for (size_t x = 0; x < encoder->varblock_width; x++) {
                size_t xv = x << 3;
                size_t yv = y << 3;
                encoder->xyb[c][yv][xv] = shift[c] >= 0 ? encoder->xyb[c][yv][xv] << shift[c]
                    : encoder->xyb[c][yv][xv] >> -shift[c];
                int32_t w = xv > 0 ? encoder->xyb[c][yv][xv - 8] : y > 0 ? encoder->xyb[c][yv - 8][xv] : 0;
                int32_t n = yv > 0 ? encoder->xyb[c][yv - 8][xv] : w;
                int32_t nw = xv > 0 && yv > 0 ? encoder->xyb[c][yv - 8][xv - 8] : w;
                int32_t v = w + n - nw;
                int32_t min = w < n ? w : n;
                int32_t max = w < n ? n : w;
                v = v < min ? min : v > max ? max : v;
                int32_t diff = encoder->xyb[c][yv][xv] - v;
                hyd_entropy_send_symbol(&stream, 0, hyd_pack_signed(diff));
            }
        }
    }
    if ((ret = hyd_prefix_write_stream_header(&stream)) < HYD_ERROR_START)
        return ret;
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
    if ((ret = hyd_prefix_write_stream_header(&stream)) < HYD_ERROR_START)
        return ret;
    if ((ret = hyd_prefix_finalize_stream(&stream)) < HYD_ERROR_START)
        return ret;
    size_t cfl_width = (encoder->varblock_width + 7) >> 3;
    size_t cfl_height = (encoder->varblock_height + 7) >> 3;
    size_t num_z_pre = 2 * cfl_width * cfl_height + nb_blocks;
    size_t num_sym = num_z_pre + 2 * nb_blocks;
    hyd_entropy_init_stream(&stream, &encoder->allocator, bw, num_sym, (const uint8_t[1]){0}, 1, 0, 0);
    for (size_t i = 0; i < num_z_pre; i++)
        hyd_entropy_send_symbol(&stream, 0, 0);
    for (size_t i = 0; i < nb_blocks; i++)
        hyd_entropy_send_symbol(&stream, 0, (hf_mult[i] - 1) * 2);
    for (size_t i = 0; i < nb_blocks; i++)
        hyd_entropy_send_symbol(&stream, 0, 0);
    if ((ret = hyd_prefix_write_stream_header(&stream)) < HYD_ERROR_START)
        return ret;
    if ((ret = hyd_prefix_finalize_stream(&stream)) < HYD_ERROR_START)
        return ret;

    return bw->overflow_state;
}

static void forward_dct(HYDEncoder *encoder) {
    int32_t scratchblock[2][8][8];
    for (size_t c = 0; c < 3; c++) {
        for (size_t by = 0; by < encoder->varblock_height; by++) {
            size_t vy = by << 3;
            for (size_t bx = 0; bx < encoder->varblock_width; bx++) {
                memset(scratchblock, 0, sizeof(scratchblock));
                size_t vx = bx << 3;
                for (size_t y = 0; y < 8; y++) {
                    size_t posy = vy + y;
                    scratchblock[0][y][0] = encoder->xyb[c][posy][vx];
                    for (size_t x = 1; x < 8; x++)
                        scratchblock[0][y][0] += encoder->xyb[c][posy][vx + x];
                    scratchblock[0][y][0] >>= 3;
                    for (size_t k = 1; k < 8; k++) {
                        for (size_t n = 0; n < 8; n++)
                            scratchblock[0][y][k] += encoder->xyb[c][posy][vx + n] * cosine_lut[k - 1][n];
                        scratchblock[0][y][k] = hyd_signed_rshift(scratchblock[0][y][k], 18);
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
                        scratchblock[1][k][x] = hyd_signed_rshift(scratchblock[1][k][x], 18);
                    }
                }
                for (size_t y = 0; y < 8; y++) {
                    size_t posy = vy + y;
                    for (size_t x = 0; x < 8; x++)
                        encoder->xyb[c][posy][vx + x] = scratchblock[1][x][y];
                }
            }
        }
    }
}

static uint8_t get_predicted_non_zeroes(uint8_t nz[32][32], size_t y, size_t x) {
    if (!x && !y)
        return 32;
    if (!x)
        return nz[y - 1][x];
    if (!y)
        return nz[y][x - 1];
    return (nz[y - 1][x] + nz[y][x - 1] + 1) >> 1;
}

static size_t get_non_zero_context(size_t predicted, size_t block_context) {
    if (predicted < 8)
        return block_context + 15 * predicted;
    if (predicted > 64)
        predicted = 64;

    return block_context + 15 * (4 + (predicted >> 1));
}

static int16_t hf_quant(int64_t value, int32_t weight, uint16_t hf_mult) {
    int64_t v = value * weight * hf_mult;
    return hyd_signed_rshift(v, 14);
}

static HYDStatusCode write_hf_coeffs(HYDEncoder *encoder, size_t num_non_zeroes, uint8_t non_zeroes[3][32][32]) {
    HYDEntropyStream stream;
    HYDStatusCode ret;
    uint8_t map[7425];
    for (int k = 0; k < 15; k++) {
        memset(map + 37 * k, k, 37);
        for (int j = 0; j < 229; j++) {
            map[555 + 458 * k + 2 * j] = k + 15;
            map[555 + 458 * k + 2 * j + 1] = k + 30;
        }
    }
    size_t num_syms = 3 * encoder->varblock_width * encoder->varblock_height + num_non_zeroes;
    hyd_entropy_init_stream(&stream, &encoder->allocator, &encoder->working_writer,
                        num_syms, map, 7425, 1, 0);
    hyd_entropy_set_hybrid_config(&stream, 0, 45, 4, 1, 0);
    for (size_t by = 0; by < encoder->varblock_height; by++) {
        size_t vy = by << 3;
        for (size_t bx = 0; bx < encoder->varblock_width; bx++) {
            size_t vx = bx << 3;
            for (int i = 0; i < 3; i++) {
                int c = i < 2 ? 1 - i : i;
                uint8_t predicted = get_predicted_non_zeroes(non_zeroes[c], by, bx);
                size_t block_context = hf_block_cluster_map[13 * i];
                size_t non_zero_context = get_non_zero_context(predicted, block_context);
                uint16_t non_zero_count = non_zeroes[c][by][bx];
                hyd_entropy_send_symbol(&stream, non_zero_context, non_zero_count);
                if (!non_zero_count)
                    continue;
                size_t hist_context = 458 * block_context + 555;
                for (int k = 0; k < 63; k++) {
                    IntPos pos = natural_order[k + 1];
                    IntPos prev_pos = natural_order[k];
                    int prev = k ? !!encoder->xyb[c][vy + prev_pos.y][vx + prev_pos.x] : non_zero_count <= 4;
                    size_t coeff_context = hist_context + prev +
                        ((coeff_num_non_zero_context[non_zero_count] + coeff_freq_context[k + 1]) << 1);
                    int32_t value = hyd_pack_signed(encoder->xyb[c][vy + pos.y][vx + pos.x]);
                    ret = hyd_entropy_send_symbol(&stream, coeff_context, value);
                    if (ret < HYD_ERROR_START)
                        return ret;
                    if (value && !--non_zero_count)
                        break;
                }
            }
        }
    }
    if ((ret = hyd_prefix_write_stream_header(&stream)) < HYD_ERROR_START)
        return ret;
    if ((ret = hyd_prefix_finalize_stream(&stream)) < HYD_ERROR_START)
        return ret;

    return encoder->working_writer.overflow_state;
}

static HYDStatusCode encode_xyb_buffer(HYDEncoder *encoder) {

    HYDStatusCode ret = hyd_init_bit_writer(&encoder->working_writer, encoder->working_buffer,
                                            sizeof(encoder->working_buffer), 0, 0);
    if (ret < HYD_ERROR_START)
        return ret;
    encoder->copy_pos = 0;

    forward_dct(encoder);

    uint8_t non_zeroes[3][32][32] = { 0 };
    uint16_t hf_mult[1024] = { 0 };
    size_t num_non_zeroes = 0;
    for (size_t by = 0; by < encoder->varblock_height; by++) {
        size_t vy = by << 3;
        for (size_t bx = 0; bx < encoder->varblock_width; bx++) {
            size_t vx = bx << 3;
            uint32_t hf = (((((encoder->xyb[1][vy + 7][vx + 7] & INT32_C(0x7FFF)) + (encoder->xyb[2][vy + 7][vx + 7] & INT32_C(0x7FFF))) << 1)
                + (encoder->xyb[1][vy + 7][vx + 6] & INT32_C(0x7FFF)) + (encoder->xyb[1][vy + 6][vx + 7] & INT32_C(0x7FFF))
                + (encoder->xyb[2][vy + 7][vx + 6] & INT32_C(0x7FFF)) + (encoder->xyb[2][vy + 6][vx + 7] & INT32_C(0x7FFF))) >> 14) & 0xF;
            hf_mult[by * encoder->varblock_width + bx] = hf > 5 ? hf : 5;
            for (int i = 0; i < 3; i++) {
                size_t nzc = 0;
                for (int j = 1; j < 64; j++) {
                    IntPos pos = natural_order[j];
                    encoder->xyb[i][vy + pos.y][vx + pos.x] =
                        hf_quant(encoder->xyb[i][vy + pos.y][vx + pos.x], hf_quant_weights[i][j], hf_mult[by * encoder->varblock_width + bx]);
                    if (encoder->xyb[i][vy + pos.y][vx + pos.x]) {
                        non_zeroes[i][by][bx]++;
                        nzc = j;
                    }
                }
                num_non_zeroes += nzc;
            }
        }
    }

    // Output sections to working buffer
    if ((ret = write_lf_global(encoder)) < HYD_ERROR_START)
        return ret;
    if ((ret = write_lf_group(encoder, hf_mult)) < HYD_ERROR_START)
        return ret;
    // default params HFGlobal
    hyd_write_bool(&encoder->working_writer, 1);
    // write HF Pass order
    hyd_write(&encoder->working_writer, 2, 2);

    if ((ret = write_hf_coeffs(encoder, num_non_zeroes, non_zeroes)) < HYD_ERROR_START)
        return ret;

    // write TOC to main buffer
    hyd_bitwriter_flush(&encoder->working_writer);

    hyd_write_zero_pad(&encoder->writer);
    hyd_write_u32(&encoder->writer, (const uint32_t[4]){0, 1024, 17408, 4211712}, (const uint32_t[4]){10, 14, 22, 30}, encoder->working_writer.buffer_pos);
    hyd_write_zero_pad(&encoder->writer);

    ret = hyd_flush(encoder);
    if (ret == HYD_OK)
        encoder->wrote_frame_header = 0;
    return ret;
}

HYDRIUM_EXPORT HYDStatusCode hyd_send_tile(HYDEncoder *encoder, const uint16_t *const buffer[3], uint32_t tile_x, uint32_t tile_y,
                            ptrdiff_t row_stride, ptrdiff_t pixel_stride) {
    HYDStatusCode ret;
    if ((ret = send_tile_pre(encoder, tile_x, tile_y)) < HYD_ERROR_START)
        return ret;

    if ((ret = hyd_populate_xyb_buffer(encoder, buffer, row_stride, pixel_stride)) < HYD_ERROR_START)
        return ret;

    return encode_xyb_buffer(encoder);
}

HYDRIUM_EXPORT HYDStatusCode hyd_send_tile8(HYDEncoder *encoder, const uint8_t *const buffer[3], uint32_t tile_x, uint32_t tile_y,
                            ptrdiff_t row_stride, ptrdiff_t pixel_stride) {
    HYDStatusCode ret;
    if ((ret = send_tile_pre(encoder, tile_x, tile_y)) < HYD_ERROR_START)
        return ret;

    if ((ret = hyd_populate_xyb_buffer8(encoder, buffer, row_stride, pixel_stride)) < HYD_ERROR_START)
        return ret;

    return encode_xyb_buffer(encoder);
}
