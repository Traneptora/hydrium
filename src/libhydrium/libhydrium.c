/*
 * libhydrium/hydrium.c
 * 
 * This is the main libhydrium API entry point, implementation.
 */

#include <stdlib.h>
#include <string.h>

#include "encoder.h"
#include "format.h"
#include "internal.h"
#include "memory.h"

HYDRIUM_EXPORT HYDEncoder *hyd_encoder_new(void) {
    HYDEncoder *ret = calloc(1, sizeof(HYDEncoder));
    return ret;
}

HYDRIUM_EXPORT HYDStatusCode hyd_encoder_destroy(HYDEncoder *encoder) {
    if (!encoder)
        return HYD_OK;
    hyd_entropy_stream_destroy(&encoder->hf_stream);
    hyd_freep(&encoder->section_endpos);
    hyd_freep(&encoder->hf_stream_barrier);
    hyd_freep(&encoder->working_writer.buffer);
    hyd_freep(&encoder->xyb);
    hyd_freep(&encoder->lf_group);
    hyd_freep(&encoder->lf_group_perm);
    hyd_freep(&encoder->input_lut8);
    hyd_freep(&encoder->input_lut16);
    hyd_freep(&encoder);
    return HYD_OK;
}

HYDRIUM_EXPORT HYDStatusCode hyd_set_metadata(HYDEncoder *encoder, const HYDImageMetadata *metadata) {
    HYDStatusCode ret = HYD_OK;
    if (!metadata->width || !metadata->height) {
        encoder->error = "invalid zero-width or zero-height";
        return HYD_API_ERROR;
    }
    const uint64_t width64 = metadata->width;
    const uint64_t height64 = metadata->height;
    if (width64 > UINT64_C(1) << 30 || height64 > UINT64_C(1) << 30) {
        encoder->error = "width or height out of bounds";
        return HYD_API_ERROR;
    }

    /* won't overflow due to above check */
    if (width64 * height64 > UINT64_C(1) << 40) {
        encoder->error = "width times height out of bounds";
        return HYD_API_ERROR;
    }

    encoder->metadata = *metadata;

    if (width64 > (1 << 20) || height64 > (1 << 20) || width64 * height64 > (1 << 28))
        encoder->level10 = 1;

    if (metadata->tile_size_shift_x < -1 || metadata->tile_size_shift_x > 3) {
        encoder->error = "tile_size_shift_y must be between -1 and 3";
        return HYD_API_ERROR;
    }
    if (metadata->tile_size_shift_y < -1 || metadata->tile_size_shift_y > 3) {
        encoder->error = "tile_size_shift_y must be between -1 and 3";
        return HYD_API_ERROR;
    }

    encoder->one_frame = metadata->tile_size_shift_x < 0 || metadata->tile_size_shift_y < 0;
    encoder->lf_group_count_x = (metadata->width + 2047) >> 11;
    encoder->lf_group_count_y = (metadata->height + 2047) >> 11;
    encoder->lf_groups_per_frame = encoder->one_frame ? encoder->lf_group_count_x * encoder->lf_group_count_y : 1;
    void *temp = hyd_realloc_array(encoder->lf_group, encoder->lf_groups_per_frame, sizeof(HYDLFGroup));
    if (!temp)
        return HYD_NOMEM;
    encoder->lf_group = temp;

    if (encoder->one_frame) {
        temp = hyd_realloc_array(encoder->lf_group_perm, encoder->lf_groups_per_frame, sizeof(size_t));
        if (!temp)
            return HYD_NOMEM;
        encoder->lf_group_perm = temp;
        for (size_t y = 0; y < encoder->lf_group_count_y; y++) {
            for (size_t x = 0; x < encoder->lf_group_count_x; x++) {
                ret = hyd_populate_lf_group(encoder, NULL, x, y);
                if (ret < HYD_ERROR_START)
                    return ret;
            }
        }
    } else {
        encoder->lf_group->tile_count_x = 1 << metadata->tile_size_shift_x;
        encoder->lf_group->tile_count_y = 1 << metadata->tile_size_shift_y;
    }

    return HYD_OK;
}

HYDRIUM_EXPORT HYDStatusCode hyd_provide_output_buffer(HYDEncoder *encoder, uint8_t *buffer, size_t buffer_len) {
    if (buffer_len < 64) {
        encoder->error = "provided buffer must be at least 64 bytes long";
        return HYD_API_ERROR;
    }
    if (encoder->out) {
        encoder->error = "buffer was already provided";
        return HYD_API_ERROR;
    }
    if (!buffer) {
        encoder->error = "buffer may not be null";
        return HYD_API_ERROR;
    }
    encoder->out = buffer;
    encoder->out_len = buffer_len;
    encoder->out_pos = 0;
    if (encoder->writer.overflow_pos > 0) {
        memcpy(encoder->out, encoder->writer.overflow, encoder->writer.overflow_pos);
        encoder->out_pos = encoder->writer.overflow_pos;
    }
    return hyd_init_bit_writer(&encoder->writer, buffer, buffer_len, encoder->writer.cache, encoder->writer.cache_bits);
}

HYDRIUM_EXPORT HYDStatusCode hyd_release_output_buffer(HYDEncoder *encoder, size_t *written) {
    if (!encoder->out) {
        encoder->error = "buffer was never provided";
        return HYD_API_ERROR;
    }
    *written = encoder->writer.buffer_pos;
    encoder->out = NULL;
    return encoder->writer.overflow_state;
}

HYDRIUM_EXPORT HYDStatusCode hyd_flush(HYDEncoder *encoder) {
    if (encoder->one_frame && !encoder->last_tile)
        return HYD_OK;
    if (!encoder->out) {
        encoder->error = "buffer was never provided";
        return HYD_API_ERROR;
    }
    hyd_bitwriter_flush(&encoder->writer);
    size_t tocopy = encoder->writer.buffer_len - encoder->writer.buffer_pos;
    if (tocopy > encoder->working_writer.buffer_pos - encoder->copy_pos)
        tocopy = encoder->working_writer.buffer_pos - encoder->copy_pos;
    memcpy(encoder->writer.buffer + encoder->writer.buffer_pos,
        encoder->working_writer.buffer + encoder->copy_pos, tocopy);
    encoder->writer.buffer_pos += tocopy;
    encoder->copy_pos += tocopy;
    if (encoder->copy_pos >= encoder->working_writer.buffer_pos)
        return HYD_OK;

    return HYD_NEED_MORE_OUTPUT;
}

HYDRIUM_EXPORT const char *hyd_error_message_get(HYDEncoder *encoder) {
    return encoder->error;
}

HYDRIUM_EXPORT HYDStatusCode hyd_send_tile(HYDEncoder *encoder, const void *const buffer[3],
    uint32_t tile_x, uint32_t tile_y, ptrdiff_t row_stride,
    ptrdiff_t pixel_stride, int is_last, HYDSampleFormat sample_fmt) {
    HYDStatusCode ret;

    if (sample_fmt != HYD_UINT8 && sample_fmt != HYD_UINT16 && sample_fmt != HYD_FLOAT32) {
        encoder->error = "Invalid Sample Format";
        return HYD_API_ERROR;
    }

    ret = hyd_send_tile_pre(encoder, tile_x, tile_y, is_last);
    if (ret < HYD_ERROR_START)
        return ret;

    size_t lfid = encoder->one_frame ? tile_y * encoder->lf_group_count_x + tile_x : 0;

    ret = hyd_populate_xyb_buffer(encoder, buffer, row_stride, pixel_stride, lfid, sample_fmt);
    if (ret < HYD_ERROR_START)
        return ret;

    if (encoder->one_frame)
        encoder->lf_group_perm[encoder->tiles_sent] = lfid;

    ret = hyd_encode_xyb_buffer(encoder, tile_x, tile_y);
    if (ret < HYD_ERROR_START)
        return ret;

    if (encoder->one_frame)
        encoder->tiles_sent++;

    return HYD_OK;
}
