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

static HYDStatusCode hyd_write_frame_header(HYDEncoder *encoder, size_t lf_x, size_t lf_y) {
    HYDBitWriter *bw = &encoder->writer;
    HYDStatusCode ret;
    if (bw->overflow_state)
        return bw->overflow_state;

    /* backtracking is illegal */
    if (encoder->lf_group_y > lf_y || !lf_x && encoder->lf_group_x > lf_x)
        return HYD_API_ERROR;

    encoder->lf_group_x = lf_x;
    encoder->lf_group_y = lf_y;
    encoder->lf_group_width = (lf_x + 1) * 2048 > encoder->metadata.width ?
                              encoder->metadata.width - lf_x * 2048 : 2048;
    encoder->lf_group_height = (lf_y + 1) * 2048 > encoder->metadata.height ?
                              encoder->metadata.height - lf_y * 2048 : 2048;
    encoder->lf_group_width += 7 - (encoder->lf_group_width + 7) % 8;
    encoder->lf_group_height += 7 - (encoder->lf_group_height + 7) % 8;

    if (!lf_x && !lf_y && encoder->metadata.width <= 2048 && encoder->metadata.height <= 2048) {
        /* all_default */
        ret = hyd_write_bool(bw, 1);
        encoder->wrote_frame_header = 1;
        return ret;
    }

    hyd_write(bw, 0x8680, 16);
    hyd_write_u32(bw, (const uint32_t[4]){0, 256, 2304, 18688},
                      (const uint32_t[4]){8, 11, 14, 30}, lf_x << 12);
    hyd_write_u32(bw, (const uint32_t[4]){0, 256, 2304, 18688},
                      (const uint32_t[4]){8, 11, 14, 30}, lf_y << 12);
    hyd_write_u32(bw, (const uint32_t[4]){0, 256, 2304, 18688},
                      (const uint32_t[4]){8, 11, 14, 30}, encoder->lf_group_width << 1);
    hyd_write_u32(bw, (const uint32_t[4]){0, 256, 2304, 18688},
                      (const uint32_t[4]){8, 11, 14, 30}, encoder->lf_group_height << 1);

    hyd_write(bw, 0, 4);
    int is_last = (lf_x + 1) << 11 >= encoder->metadata.width && (lf_y + 1) << 11 >= encoder->metadata.height;
    hyd_write_bool(bw, is_last);
    if (!is_last)
        hyd_write(bw, 0, 2);

    ret = hyd_write(bw, 0x4, 6);
    encoder->wrote_frame_header = 1;

    return ret;
}

HYDStatusCode hyd_send_tile(HYDEncoder *encoder, const uint16_t *buffer[3],
                            size_t tile_x, size_t tile_y, ptrdiff_t row_stride, ptrdiff_t pixel_stride) {
    if (encoder->writer.overflow_state)
        return encoder->writer.overflow_state;

    HYDStatusCode ret;
    if (!encoder->wrote_header) {
        if ((ret = hyd_write_header(encoder)) < 0)
            return ret;
    }
    if (!encoder->wrote_frame_header) {
        if ((ret = hyd_write_frame_header(encoder, tile_x >> 3, tile_y >> 3)) < 0)
            return ret;
    }

    hyd_populate_xyb_buffer(encoder, buffer, row_stride, pixel_stride);
    // TODO encode rest of tile
}
