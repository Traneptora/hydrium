/*
 * Base encoder implementation
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bitwriter.h"
#include "entropy.h"
#include "internal.h"
#include "osdep.h"
#include "xyb.h"

static const uint8_t level10_header[49] = {
    0x00, 0x00, 0x00, 0x0c,  'J',  'X',  'L',  ' ',
    0x0d, 0x0a, 0x87, 0x0a, 0x00, 0x00, 0x00, 0x14,
     'f',  't',  'y',  'p',  'j',  'x',  'l',  ' ',
    0x00, 0x00, 0x00, 0x00,  'j',  'x',  'l',  ' ',
    0x00, 0x00, 0x00, 0x09,  'j',  'x',  'l',  'l', 0x0a,
    0x00, 0x00, 0x00, 0x00,  'j',  'x',  'l',  'c',
};

static int32_t cosine_lut[7][8] = {
    {32138, 27245, 18204, 6392, -6392, -18204, -27245, -32138},
    {30273, 12539, -12539, -30273, -30273, -12539, 12539, 30273},
    {27245, -6392, -32138, -18204, 18204, 32138, 6392, -27245},
    {23170, -23170, -23170, 23170, 23170, -23170, -23170, 23170},
    {18204, -32138, 6392, 27245, -27245, -6392, 32138, -18204},
    {12539, -30273, 30273, -12539, -12539, 30273, -30273, 12539},
    {6392, -18204, 27245, -32138, 32138, -27245, 18204, -6392}
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

    /*
     * all_default = 0:1
     * frame_type = 0:2
     * encoding = 0:1
     * flags = 0:2
     * upsampling = 0:2
     * x_qm_scale = 3:3
     * b_qm_scale = 2:3
     * num_passes = 0:2
     */
    hyd_write(bw, 0x1300, 16);

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

    /*
     * name_len = 0:2
     * all_default = 1:1
     * extensions = 0:2
     * permuted_toc = 0:1
     */
    hyd_write(bw, 0x4, 6);

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

    // LF channel correlation all_default
    hyd_write_bool(bw, 1);

    // quantizer globalScale = 32768
    hyd_write_u32(bw, (const uint32_t[4]){1, 2049, 4097, 8193}, (const uint32_t[4]){11, 11, 12, 16}, 32768);
    // quantizer quantLF = 64
    hyd_write_u32(bw, (const uint32_t[4]){16, 1, 1, 1}, (const uint32_t[4]){0, 5, 8, 16}, 64);
    // HF Block Context all_default
    hyd_write_bool(bw, 1);
    // LF Channel Correlation all_default
    hyd_write_bool(bw, 1);
    // GlobalModular have_global_tree
    return hyd_write_bool(bw, 0);
}

static HYDStatusCode write_lf_group(HYDEncoder *encoder) {
    HYDStatusCode ret;
    HYDBitWriter *bw = &encoder->working_writer;
    const int32_t quant[3] = {-2, 1, 2};
    // extra precision = 0
    hyd_write(bw, 0, 2);
    // use global tree
    hyd_write_bool(bw, 0);
    // wp_params all_default
    hyd_write_bool(bw, 1);
    // nb_transforms = 0
    hyd_write(bw, 0, 2);
    HYDEntropyStream stream;
    ret = hyd_ans_init_stream(&stream, &encoder->allocator, bw, 5, (const uint8_t[6]){0, 0, 0, 0, 0, 0}, 6);
    if (ret < HYD_ERROR_START)
        return ret;
    // property = -1
    hyd_ans_send_symbol(&stream, 1, 0);
    // predictor = 5
    hyd_ans_send_symbol(&stream, 2, 5);
    // offset = 0
    hyd_ans_send_symbol(&stream, 3, 0);
    // mul_log = 0
    hyd_ans_send_symbol(&stream, 4, 0);
    // mul_bits = 0
    hyd_ans_send_symbol(&stream, 5, 0);
    if ((ret = hyd_ans_write_stream_header(&stream)) < HYD_ERROR_START)
        return ret;
    if ((ret = hyd_ans_finalize_stream(&stream)) < HYD_ERROR_START)
        return ret;
    size_t varblock_width = (encoder->group_width + 7) >> 3;
    size_t varblock_height = (encoder->group_height + 7) >> 3;
    ret = hyd_ans_init_stream(&stream, &encoder->allocator, bw, 3 * varblock_width * varblock_height,
                                  (const uint8_t[1]){0}, 1);
    if (ret < HYD_ERROR_START)
        return ret;
    for (int i = 0; i < 3; i++) {
        int c = i < 2 ? 1 - i : i;
        for (size_t y = 0; y < varblock_height; y++) {
            for (size_t x = 0; x < varblock_width; x++) {
                size_t xv = x << 3;
                size_t yv = y << 3;
                int16_t w = xv > 0 ? encoder->xyb[c][yv][xv - 8] : y > 0 ? encoder->xyb[c][yv - 8][xv] : 0;
                int16_t n = yv > 0 ? encoder->xyb[c][yv - 8][xv] : w;
                int16_t nw = xv > 0 && yv > 0 ? encoder->xyb[c][yv - 8][xv - 8] : w;
                int16_t v = w + n - nw;
                int16_t min = w < n ? w : n;
                int16_t max = w < n ? n : w;
                v = v < min ? min : v > max ? max : v;
                int32_t diff = encoder->xyb[c][yv][xv] - v;
                diff = quant[c] >= 0 ? diff >> quant[c] : diff << -quant[c];
                uint32_t packed_diff = diff >= 0 ? 2 * diff : (2 * -diff) - 1;
                hyd_ans_send_symbol(&stream, 0, packed_diff);
            }
        }
    }
    if ((ret = hyd_ans_write_stream_header(&stream)) < HYD_ERROR_START)
        return ret;
    if ((ret = hyd_ans_finalize_stream(&stream)) < HYD_ERROR_START)
        return ret;
    size_t nb_blocks = varblock_width * varblock_height;
    hyd_write(bw, nb_blocks - 1, hyd_cllog2(nb_blocks));
    hyd_write_bool(bw, 0);
    hyd_write_bool(bw, 1);
    hyd_write(bw, 0, 2);
    ret = hyd_ans_init_stream(&stream, &encoder->allocator, bw, 5, (const uint8_t[6]){0, 0, 0, 0, 0, 0}, 6);
    if (ret < HYD_ERROR_START)
        return ret;
    hyd_ans_send_symbol(&stream, 1, 0);
    hyd_ans_send_symbol(&stream, 2, 0);
    hyd_ans_send_symbol(&stream, 3, 0);
    hyd_ans_send_symbol(&stream, 4, 0);
    hyd_ans_send_symbol(&stream, 5, 0);
    if ((ret = hyd_ans_write_stream_header(&stream)) < HYD_ERROR_START)
        return ret;
    if ((ret = hyd_ans_finalize_stream(&stream)) < HYD_ERROR_START)
        return ret;
    size_t cfl_width = (varblock_width + 7) >> 3;
    size_t cfl_height = (varblock_height + 7) >> 3;
    size_t num_zeroes = 2 * cfl_width * cfl_height + 2 * nb_blocks + varblock_width * varblock_height;
    hyd_ans_init_stream(&stream, &encoder->allocator, bw, num_zeroes, (const uint8_t[1]){0}, 1);
    for (size_t i = 0; i < num_zeroes; i++)
        hyd_ans_send_symbol(&stream, 0, 0);
    if ((ret = hyd_ans_write_stream_header(&stream)) < HYD_ERROR_START)
        return ret;
    if ((ret = hyd_ans_finalize_stream(&stream)) < HYD_ERROR_START)
        return ret;

    return bw->overflow_state;
}

static void swap_8x8(int32_t block[8][8]) {
    for (size_t y = 1; y < 8; y++) {
        for (size_t x = 0; x < y; x++) {
            block[y][x] ^= block[x][y];
            block[x][y] ^= block[y][x];
            block[y][x] ^= block[x][y];
        }
    }
}

static void forward_dct(HYDEncoder *encoder) {
    int32_t scratchblock[2][8][8];
    size_t varblock_width = (encoder->group_width + 7) >> 3;
    size_t varblock_height = (encoder->group_height + 7) >> 3;
    for (size_t c = 0; c < 3; c++) {
        for (size_t vy = 0; vy < varblock_height; vy++) {
            size_t vy2 = vy << 3;
            for (size_t vx = 0; vx < varblock_width; vx++) {
                memset(scratchblock, 0, sizeof(scratchblock));
                size_t vx2 = vx << 3;
                for (size_t y = 0; y < 8; y++) {
                    size_t by = vy2 + y;
                    scratchblock[0][y][0] = encoder->xyb[c][by][vx2];
                    for (size_t i = 1; i < 8; i++)
                        scratchblock[0][y][0] += encoder->xyb[c][by][vx2 + i];
                    scratchblock[0][y][0] >>= 3;
                    for (size_t i = 1; i < 8; i++) {
                        for (size_t n = 0; n < 8; n++)
                            scratchblock[0][y][i] += encoder->xyb[c][by][vx2 + n] * cosine_lut[i - 1][n];
                        scratchblock[0][y][i] >>= 16;
                    }
                }
                swap_8x8(scratchblock[0]);
                for (size_t y = 0; y < 8; y++) {
                    scratchblock[1][y][0] = scratchblock[0][y][0];
                    for (size_t i = 1; i < 8; i++)
                        scratchblock[1][y][0] += scratchblock[0][y][i];
                    scratchblock[1][y][0] >>= 3;
                    for (size_t i = 1; i < 8; i++) {
                        for (size_t n = 0; n < 8; n++)
                            scratchblock[1][y][i] += scratchblock[0][y][n] * cosine_lut[i - 1][n];
                        scratchblock[1][y][i] >>= 16;
                    }
                }
                for (size_t y = 0; y < 8; y++) {
                    size_t by = vy2 + y;
                    for (size_t x = 0; x < 8; x++)
                        encoder->xyb[c][by][vx2 + x] = scratchblock[1][x][y];
                }
            }
        }
    }
}

static HYDStatusCode write_hf_coeffs(HYDEncoder *encoder) {
    // todo write actual HF coeffs
    HYDEntropyStream stream;
    const uint8_t map[7425] = { 0 };
    size_t num_syms = 3 * ((encoder->group_width + 7) >> 3) * ((encoder->group_height + 7) >> 3);
    hyd_ans_init_stream(&stream, &encoder->allocator, &encoder->working_writer,
                        num_syms, map, 7425);
    for (size_t i = 0; i < num_syms; i++)
        hyd_ans_send_symbol(&stream, 0, 0);
    hyd_ans_write_stream_header(&stream);
    return hyd_ans_finalize_stream(&stream);
}

static HYDStatusCode encode_xyb_buffer(HYDEncoder *encoder) {

    HYDStatusCode ret = hyd_init_bit_writer(&encoder->working_writer, encoder->working_buffer,
                                            sizeof(encoder->working_buffer), 0, 0);
    if (ret < HYD_ERROR_START)
        return ret;
    encoder->copy_pos = 0;

    forward_dct(encoder);

    // Output sections to working buffer
    if ((ret = write_lf_global(encoder)) < HYD_ERROR_START)
        return ret;
    if ((ret = write_lf_group(encoder)) < HYD_ERROR_START)
        return ret;
    // default params HFGLobal
    hyd_write_bool(&encoder->working_writer, 1);
    // write HF Pass order
    hyd_write(&encoder->working_writer, 2, 2);

    if ((ret = write_hf_coeffs(encoder)) < HYD_ERROR_START)
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

HYDStatusCode hyd_send_tile(HYDEncoder *encoder, const uint16_t *const buffer[3], uint32_t tile_x, uint32_t tile_y,
                            ptrdiff_t row_stride, ptrdiff_t pixel_stride) {
    HYDStatusCode ret;
    if ((ret = send_tile_pre(encoder, tile_x, tile_y)) < 0)
        return ret;

    if ((ret = hyd_populate_xyb_buffer(encoder, buffer, row_stride, pixel_stride)) < 0)
        return ret;

    return encode_xyb_buffer(encoder);
}

HYDStatusCode hyd_send_tile8(HYDEncoder *encoder, const uint8_t *const buffer[3], uint32_t tile_x, uint32_t tile_y,
                            ptrdiff_t row_stride, ptrdiff_t pixel_stride) {
    HYDStatusCode ret;
    if ((ret = send_tile_pre(encoder, tile_x, tile_y)) < 0)
        return ret;

    if ((ret = hyd_populate_xyb_buffer8(encoder, buffer, row_stride, pixel_stride)) < 0)
        return ret;

    return encode_xyb_buffer(encoder);
}
