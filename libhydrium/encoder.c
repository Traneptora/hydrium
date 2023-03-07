/*
 * Base encoder implementation
 */
#include <stddef.h>
#include <stdint.h>

#include "internal.h"
#include "libhydrium/color/xyb.h"
#include "libhydrium/writer/bitwriter.h"

HYDStatusCode hyd_write_header(HYDEncoder *encoder) {
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

HYDStatusCode hyd_write_frame_header(HYDEncoder *encoder, size_t lf_x, size_t lf_y) {
    HYDBitWriter *bw = &encoder->writer;
    if (bw->overflow_state)
        return bw->overflow_state;
    
    if (!lf_x && !lf_y && encoder->metadata.width <= 2048 && encoder->metadata.height <= 2048) {
        /* all_default */
        HYDStatusCode ret = hyd_write_bool(bw, 1);
        encoder->wrote_frame_header = 1;
        return ret;
    }

    hyd_write(bw, 0x8680, 16);
    // TODO write crop
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

}
