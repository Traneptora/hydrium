/*
 * Base encoder implementation
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bitwriter.h"
#include "internal.h"
#include "xyb.h"

static const uint8_t level10_header[49] = {
    0x00, 0x00, 0x00, 0x0c,  'J',  'X',  'L',  ' ',
    0x0d, 0x0a, 0x87, 0x0a, 0x00, 0x00, 0x00, 0x14,
     'f',  't',  'y',  'p',  'j',  'x',  'l',  ' ',
    0x00, 0x00, 0x00, 0x00,  'j',  'x',  'l',  ' ',
    0x00, 0x00, 0x00, 0x09,  'j',  'x',  'l',  'l', 0x0a,
    0x00, 0x00, 0x00, 0x00,  'j',  'x',  'l',  'c',
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

    int is_last = (encoder->group_x + 1) << 11 >= encoder->metadata.width
        && (encoder->group_y + 1) << 11 >= encoder->metadata.height;
    int have_crop = !is_last || encoder->group_x != 0 || encoder->group_y != 0;

    hyd_write_bool(bw, have_crop);

    if (have_crop) {
        const uint32_t cpos[4] = {0, 256, 2304, 18688};
        const uint32_t upos[4] = {8, 11, 14, 30};
        /* extra factor of 2 here because UnpackSigned */
        hyd_write_u32(bw, cpos, upos, encoder->group_x << 12);
        hyd_write_u32(bw, cpos, upos, encoder->group_y << 12);
        hyd_write_u32(bw, cpos, upos, encoder->group_width << 1);
        hyd_write_u32(bw, cpos, upos, encoder->group_height << 1);
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
     * gab_custom = 0:1
     * extensions = 0:2
     * permuted_toc = 0:1
     */
    hyd_write(bw, 0x4, 7);

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
        if ((ret = write_header(encoder)) < 0)
            return ret;
    }

    if (!encoder->wrote_frame_header) {
        if ((ret = write_frame_header(encoder)) < 0)
            return ret;
    }

    return HYD_OK;
}

static HYDStatusCode write_lf_global(HYDEncoder *encoder) {
    HYDBitWriter *bw = &encoder->working_writer;
    // LF channel correlation AllDefault
    hyd_write_bool(bw, 1);
}

static HYDStatusCode encode_xyb_buffer(HYDEncoder *encoder) {

    HYDStatusCode ret = hyd_init_bit_writer(&encoder->working_writer, encoder->working_buffer,
                                            sizeof(encoder->working_buffer), 0, 0);
    if (ret < 0)
        return ret;

    // Run Forward DCT
    // Compute LF Coefficients

    // Output sections to working buffer
    write_lf_global(encoder);
    // write TOC to main buffer

    ret = hyd_flush(encoder);
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

HYDStatusCode hyd_flush(HYDEncoder *encoder) {
    // dummy stub

    return hyd_bitwriter_flush(&encoder->writer);
}
