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
#include "math-functions.h"
#include "memory.h"

HYDRIUM_EXPORT HYDEncoder *hyd_encoder_new(void) {
    HYDEncoder *ret = calloc(1, sizeof(HYDEncoder));
    return ret;
}

HYDRIUM_EXPORT HYDStatusCode hyd_encoder_destroy(HYDEncoder *encoder) {
    if (!encoder)
        return HYD_OK;
    hyd_entropy_stream_destroy(&encoder->hf_stream);
    if (encoder->section_endpos != encoder->section_endpos_array)
        hyd_freep(&encoder->section_endpos);
    hyd_freep(&encoder->hf_stream_barrier);
    hyd_freep(&encoder->working_writer.buffer);
    hyd_freep(&encoder->xyb);
    if (encoder->lfg != encoder->lfg_array)
        hyd_freep(&encoder->lfg);
    if (encoder->lfg_perm != encoder->lfg_perm_array)
        hyd_freep(&encoder->lfg_perm);
    hyd_freep(&encoder->input_lut8);
    hyd_freep(&encoder->input_lut16);
    hyd_freep(&encoder->bias_cbrtf_lut);
    hyd_freep(&encoder->icc_data);
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
    encoder->lfg_count_y = (metadata->height + 2047) >> 11;
    encoder->lfg_count_x = (metadata->width + 2047) >> 11;
    encoder->lfg_per_frame = encoder->one_frame ? encoder->lfg_count_y * encoder->lfg_count_x : 1;
    if (encoder->lfg != encoder->lfg_array)
        hyd_freep(&encoder->lfg);
    if (encoder->lfg_per_frame > hyd_array_size(encoder->lfg_array)) {
        encoder->lfg = hyd_malloc_array(encoder->lfg_per_frame, sizeof(*encoder->lfg));
        if (!encoder->lfg)
            return HYD_NOMEM;
    } else {
        encoder->lfg = encoder->lfg_array;
    }

    if (encoder->one_frame) {
        if (encoder->lfg_perm != encoder->lfg_perm_array)
            hyd_freep(&encoder->lfg_perm);
        if (encoder->lfg_per_frame > hyd_array_size(encoder->lfg_perm_array)) {
            encoder->lfg_perm = hyd_malloc_array(encoder->lfg_per_frame, sizeof(*encoder->lfg_perm));
            if (!encoder->lfg_perm)
                return HYD_NOMEM;
        } else {
            encoder->lfg_perm = encoder->lfg_perm_array;
        }
        for (size_t y = 0; y < encoder->lfg_count_y; y++) {
            for (size_t x = 0; x < encoder->lfg_count_x; x++) {
                ret = hyd_populate_lf_group(encoder, NULL, x, y);
                if (ret < HYD_ERROR_START)
                    return ret;
            }
        }
    } else {
        encoder->lfg->tile_count_y = 1 << metadata->tile_size_shift_y;
        encoder->lfg->tile_count_x = 1 << metadata->tile_size_shift_x;
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

    size_t lfid = encoder->one_frame ? tile_y * encoder->lfg_count_x + tile_x : 0;

    ret = hyd_populate_xyb_buffer(encoder, buffer, row_stride, pixel_stride, lfid, sample_fmt);
    if (ret < HYD_ERROR_START)
        return ret;

    if (encoder->one_frame)
        encoder->lfg_perm[encoder->tiles_sent] = lfid;

    ret = hyd_encode_xyb_buffer(encoder, tile_x, tile_y);
    if (ret < HYD_ERROR_START)
        return ret;

    if (encoder->one_frame)
        encoder->tiles_sent++;

    return HYD_OK;
}

static inline uint8_t header_predict(const uint8_t *header, uint32_t icc_size, unsigned int i)
{
    if (i < 4)
        return (icc_size >> (8 * (3 - i))) & 0xff;
    if (i == 8)
        return 4;
    if (i >= 12 && i < 24)
        return "mntrRGB XYZ "[i - 12];
    if (i >= 36 && i < 40)
        return "acsp"[i - 36];
    if (i >= 41 && i < 44) {
        if (header[40] == 'A')
            return "PPL"[i - 41];
        if (header[40] == 'M')
            return "SFT"[i - 41];
        if (header[40] == 'S') {
            if (header[41] == 'G')
                return "I "[i - 42];
            if (header[41] == 'U')
                return "NW"[i - 42];
        }
    }
    if (i == 70)
        return 246;
    if (i == 71)
        return 214;
    if (i == 73)
        return 1;
    if (i == 78)
        return 211;
    if (i == 79)
        return 45;
    if (i >= 80 && i < 84)
        return header[i - 76];
    return 0;
}

HYDRIUM_EXPORT HYDStatusCode hyd_set_suggested_icc_profile(HYDEncoder *encoder,
    const uint8_t *icc_data, size_t icc_size)
{
    if (!icc_data && !icc_size) {
        hyd_freep(&encoder->icc_data);
        encoder->icc_size = 0;
        return HYD_OK;
    }

    if (!icc_size || !icc_data || icc_size > UINT32_MAX)
        return HYD_API_ERROR;

    /* three varints and two bytes */
    /* varint caps out at 10 bytes for ~0ul */
    size_t mangled_buffer_size = icc_size + 10 + 10 + 2 + 10;
    uint8_t *mangled_icc = malloc(mangled_buffer_size);
    if (!mangled_icc)
        return HYD_NOMEM;
    HYDBitWriter bws, *bw = &bws;
    hyd_init_bit_writer(bw, mangled_icc, mangled_buffer_size, 0, 0);

    size_t header_size = hyd_min(icc_size, 128);

    uint8_t header[128];
    for (unsigned int i = 0; i < header_size; i++)
        header[i] = (icc_data[i] - header_predict(icc_data, icc_size, i)) & 0xff;

    size_t remaining_size = icc_size - header_size;
    hyd_write_icc_varint(bw, icc_size);
    hyd_write_icc_varint(bw, remaining_size ? 3 + hyd_fllog2(remaining_size) / 7 : 0);

    /* nontrivial command stream */
    if (remaining_size) {
        /* taglist length */
        /* this uses 1 byte */
        hyd_write_icc_varint(bw, 0);
        /* command == 1 */
        hyd_write(bw, 1, 8);
        hyd_write_icc_varint(bw, remaining_size);
    }

    hyd_bitwriter_flush(bw);
    memcpy(bw->buffer + bw->buffer_pos, header, header_size);
    bw->buffer_pos += header_size;

    if (remaining_size) {
        /* should have allocated enough extra space up top */
        hyd_bitwriter_flush(bw);
        memcpy(bw->buffer + bw->buffer_pos, icc_data + header_size, remaining_size);
        bw->buffer_pos += remaining_size;
    }

    hyd_bitwriter_flush(bw);
    encoder->icc_data = bw->buffer;
    encoder->icc_size = bw->buffer_pos;
    return HYD_OK;
}
