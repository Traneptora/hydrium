/*
 * Base encoder implementation
 */
#include <stddef.h>
#include <stdint.h>

#include "internal.h"
#include "libhydrium/color/xyb.h"
#include "libhydrium/writer/bitwriter.h"

static HYDStatusCode hyd_write_header(HYDEncoder *encoder) {
    if (!encoder->out)
        return HYD_NEED_MORE_OUTPUT;
    
    HYDBitWriter *bw = &encoder->writer;
    if (bw->overflow_state)
        return bw->overflow_state;

    if (encoder->level10) {
        hyd_write(bw, 0xC0000000, 32);
        hyd_write(bw, 0x204c584a, 32);
        hyd_write(bw, 0x0a870a0d, 32);
        hyd_write(bw, 0x14000000, 32);
        hyd_write(bw, 0x70797466, 32);
        hyd_write(bw, 0x206c786a, 32);
        hyd_write(bw, 0, 32);
        hyd_write(bw, 0x206c786a, 32);
        hyd_write(bw, 0x09000000, 32);
        hyd_write(bw, 0x6c6c786a, 32);
        hyd_write(bw, 0x0a, 8);
        hyd_write(bw, 0, 32);
        hyd_write(bw, 0x636c786a, 32);
    }

    hyd_write(bw, 0x0AFF, 16);
    if ((encoder->metadata.width & 0x7) || (encoder->metadata.height & 0x7)) {
        hyd_write_bool(bw, 0);
        hyd_write_u32(bw, (const uint32_t[4]){1, 1, 1, 1}, (const uint32_t[4]){9, 13, 18, 30}, encoder->metadata.height);
        hyd_write(bw, 0, 3);
        hyd_write_u32(bw, (const uint32_t[4]){1, 1, 1, 1}, (const uint32_t[4]){9, 13, 18, 30}, encoder->metadata.width);
    } else {
        hyd_write_bool(bw, 1);
        hyd_write(bw, encoder->metadata.height/8 - 1, 5);
        hyd_write(bw, 0, 3);
        hyd_write(bw, encoder->metadata.width/8 - 1, 5);
    }

    /* all_default */
    hyd_write_bool(bw, 1);

    /* default_m */
    hyd_write_bool(bw, 1);

    encoder->wrote_header = 1;
    return bw->overflow_state;
}

static HYDStatusCode hyd_write_frame_header(HYDEncoder *encoder) {
    HYDBitWriter *bw = &encoder->writer;
    HYDStatusCode ret;

    if (bw->overflow_state)
        return bw->overflow_state;

    hyd_write_zero_pad(bw);

    encoder->lf_group_width = (encoder->lf_group_x + 1) * 2048 > encoder->metadata.width ?
                              encoder->metadata.width - encoder->lf_group_x * 2048 : 2048;
    encoder->lf_group_height = (encoder->lf_group_y + 1) * 2048 > encoder->metadata.height ?
                              encoder->metadata.height - encoder->lf_group_y * 2048 : 2048;
    encoder->lf_group_width += 7 - (encoder->lf_group_width + 7) % 8;
    encoder->lf_group_height += 7 - (encoder->lf_group_height + 7) % 8;

    if (encoder->metadata.width <= 2048 && encoder->metadata.height <= 2048) {
        /* all_default = 1, permuted_toc = 0 */
        hyd_write(bw, 0x1, 2);
        ret = hyd_write_zero_pad(bw);
        encoder->wrote_frame_header = 1;
        return ret;
    }

    hyd_write(bw, 0x8680, 16);
    hyd_write_u32(bw, (const uint32_t[4]){0, 256, 2304, 18688},
                      (const uint32_t[4]){8, 11, 14, 30}, encoder->lf_group_x << 12);
    hyd_write_u32(bw, (const uint32_t[4]){0, 256, 2304, 18688},
                      (const uint32_t[4]){8, 11, 14, 30}, encoder->lf_group_y << 12);
    hyd_write_u32(bw, (const uint32_t[4]){0, 256, 2304, 18688},
                      (const uint32_t[4]){8, 11, 14, 30}, encoder->lf_group_width << 1);
    hyd_write_u32(bw, (const uint32_t[4]){0, 256, 2304, 18688},
                      (const uint32_t[4]){8, 11, 14, 30}, encoder->lf_group_height << 1);

    hyd_write(bw, 0, 4);
    int is_last = (encoder->lf_group_x + 1) << 11 >= encoder->metadata.width
        && (encoder->lf_group_y + 1) << 11 >= encoder->metadata.height;
    hyd_write_bool(bw, is_last);
    if (!is_last)
        hyd_write(bw, 0, 2);

    hyd_write(bw, 0x4, 7);
    ret = hyd_write_zero_pad(bw);
    encoder->wrote_frame_header = 1;

    return ret;
}

static HYDStatusCode hyd_flush_lf_group(HYDEncoder *encoder) {

    // TODO write TOC
    // TODO write groups

    if (++encoder->lf_group_x * 2048 >= encoder->metadata.width) {
        encoder->lf_group_x = 0;
        if (++encoder->lf_group_y * 2048 >= encoder->metadata.height) {
            /* this is the last frame */
            return HYD_OK;
        }
    }

    return HYD_NEED_MORE_INPUT;
}

static HYDStatusCode hyd_send_tile_pre(HYDEncoder *encoder) {
    if (encoder->writer.overflow_state)
        return encoder->writer.overflow_state;

    HYDStatusCode ret;
    if (!encoder->wrote_header) {
        if ((ret = hyd_write_header(encoder)) < 0)
            return ret;
    }
    if (!encoder->wrote_frame_header) {
        if ((ret = hyd_write_frame_header(encoder)) < 0)
            return ret;
    }

    return HYD_OK;
}

static HYDStatusCode hyd_send_tile_post(HYDEncoder *encoder, size_t *next_x, size_t *next_y) {

    HYDStatusCode ret = HYD_NEED_MORE_INPUT;

    // TODO encode rest of tile

    if (++encoder->group_x >= (encoder->lf_group_width + 255) / 256) {
        encoder->group_x = 0;
        if (++encoder->group_y >= (encoder->lf_group_height + 255) / 256) {
            encoder->group_y = 0;
            ret = hyd_flush_lf_group(encoder);
        }
    }

    if (ret == HYD_NEED_MORE_INPUT) {
        *next_x = (encoder->lf_group_x << 11) | (encoder->group_x << 8);
        *next_y = (encoder->lf_group_y << 11) | (encoder->group_y << 8);
    } 

    return ret;
}

HYDStatusCode hyd_send_tile(HYDEncoder *encoder, const uint16_t *buffer[3],
                            ptrdiff_t row_stride, ptrdiff_t pixel_stride, size_t *next_x, size_t *next_y) {
    HYDStatusCode ret;
    if ((ret = hyd_send_tile_pre(encoder)) < 0)
        return ret;

    if ((ret = hyd_populate_xyb_buffer(encoder, buffer, row_stride, pixel_stride)) < 0)
        return ret;

    return hyd_send_tile_post(encoder, next_x, next_y);
}

HYDStatusCode hyd_send_tile8(HYDEncoder *encoder, const uint8_t *buffer[3],
                            ptrdiff_t row_stride, ptrdiff_t pixel_stride, size_t *next_x, size_t *next_y) {
    HYDStatusCode ret;
    if ((ret = hyd_send_tile_pre(encoder)) < 0)
        return ret;

    if ((ret = hyd_populate_xyb_buffer8(encoder, buffer, row_stride, pixel_stride)) < 0)
        return ret;

    return hyd_send_tile_post(encoder, next_x, next_y);
}
