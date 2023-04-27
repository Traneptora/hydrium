/*
 * Hydrium basic implementation
 */

#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <spng.h>

#include "libhydrium/libhydrium.h"

struct memory_holder {
    size_t total_alloced;
    size_t current_alloced;
    size_t max_alloced;
};

static void *mem_alloc(size_t size, void *opaque) {
    struct memory_holder *holder = opaque;
    size += sizeof(size_t);
    void *ptr = malloc(size);
    if (!ptr)
        return NULL;
    holder->total_alloced += size;
    holder->current_alloced += size;
    if (holder->current_alloced > holder->max_alloced)
        holder->max_alloced = holder->current_alloced;
    *(size_t *)ptr = size;
    return ptr + sizeof(size_t);
}

static void mem_free(void *ptr, void *opaque) {
    if (!ptr)
        return;
    struct memory_holder *holder = opaque;
    void *og_ptr = ptr - sizeof(size_t);
    holder->current_alloced -= *(size_t *)og_ptr;
    free(og_ptr);
}

int main(int argc, const char *argv[]) {
    uint64_t width, height;
    void *buffer = NULL, *output_buffer = NULL;
    HYDEncoder *encoder = NULL;
    int ret = 1;
    FILE *fp = stdout, *fin = NULL;
    struct memory_holder holder = { 0 };

    fprintf(stderr, "libhydrium version %s\n", HYDRIUM_VERSION_STRING);

    if (argc < 2 || !strcmp(argv[1], "--help")) {
        fprintf(stderr, "Usage: %s <input.png> [output.jxl]\n", argv[0]);
        return argc < 2;
    }

    spng_ctx *spng_context = spng_ctx_new(0);
    if (!spng_context) {
        fprintf(stderr, "%s: couldn't allocate context\n", argv[0]);
        goto done;
    }

    spng_set_crc_action(spng_context, SPNG_CRC_USE, SPNG_CRC_USE);
    const size_t chunk_limit = 1 << 20;
    spng_set_chunk_limits(spng_context, chunk_limit, chunk_limit);

    fin = fopen(argv[1], "rb");
    if (!fin) {
        fprintf(stderr, "%s: error opening file: %s\n", argv[0], argv[1]);
        goto done;
    }
    spng_set_png_file(spng_context, fin);

    struct spng_ihdr ihdr;
    ret = spng_get_ihdr(spng_context, &ihdr);
    if (ret) {
        fprintf(stderr, "%s: spng_get_ihdr() error: %s\n", argv[0], spng_strerror(ret));
        goto done;
    }

    width = ihdr.width;
    height = ihdr.height;

    if (width > (UINT64_C(1) << 30) || height > (UINT64_C(1) << 30) || width * height > (UINT64_C(1) << 40)) {
        fprintf(stderr, "%s: buffer too big\n", argv[0]);
        goto done;
    }

    enum spng_format sample_fmt = ihdr.bit_depth > 8 ? SPNG_FMT_RGBA16 : SPNG_FMT_RGB8;

    size_t decoded_png_size;
    ret = spng_decoded_image_size(spng_context, sample_fmt, &decoded_png_size);
    if (ret) {
        fprintf(stderr, "%s: spng error: %s\n", argv[0], spng_strerror(ret));
        goto done;
    }

    size_t spng_stride = decoded_png_size / height;

    HYDImageMetadata metadata;
    metadata.width = width;
    metadata.height = height;
    metadata.linear_light = 0;
    metadata.tile_size_shift_x = 3;
    metadata.tile_size_shift_y = 3;
    const uint32_t tile_size_x = 256 << metadata.tile_size_shift_x;
    const uint32_t tile_size_y = 256 << metadata.tile_size_shift_y;
    const uint32_t tile_width = (width + tile_size_x - 1) >> (8 + metadata.tile_size_shift_x);
    const uint32_t tile_height = (height + tile_size_y - 1) >> (8 + metadata.tile_size_shift_y);

    if (ihdr.interlace_method != SPNG_INTERLACE_NONE)
        buffer = malloc(decoded_png_size);
    else
        buffer = malloc(spng_stride * tile_size_y);

    if (!buffer) {
        fprintf(stderr, "%s: not enough memory\n", argv[0]);
        goto done;
    }

    size_t output_bufsize = 1 << 20;
    output_buffer = malloc(output_bufsize);
    if (!output_buffer) {
        fprintf(stderr, "%s: not enough memory\n", argv[0]);
        goto done;
    }

    if (ihdr.interlace_method != SPNG_INTERLACE_NONE)
        ret = spng_decode_image(spng_context, buffer, decoded_png_size, sample_fmt, 0);
    else
        ret = spng_decode_image(spng_context, NULL, 0, sample_fmt, SPNG_DECODE_PROGRESSIVE);
    if (ret) {
        fprintf(stderr, "%s: spng error: %s\n", argv[0], spng_strerror(ret));
        goto done;
    }

    HYDAllocator allocator;

    allocator.alloc_func = &mem_alloc;
    allocator.free_func = &mem_free;
    allocator.opaque = &holder;

    encoder = hyd_encoder_new(&allocator);
    if (!encoder) {
        fprintf(stderr, "%s: error allocating encoder\n", argv[0]);
        goto done;
    }

    if (argc > 2) {
        fp = fopen(argv[2], "wb");
        if (!fp) {
            fprintf(stderr, "%s: error opening file for writing: %s\n", argv[0], argv[2]);
            goto done;
        }
    }

    if ((ret = hyd_set_metadata(encoder, &metadata)) < HYD_ERROR_START)
        goto done;

    hyd_provide_output_buffer(encoder, output_buffer, output_bufsize);
    struct spng_row_info row_info = {0};
    for (uint32_t y = 0; y < tile_height; y++) {
        if (ihdr.interlace_method == SPNG_INTERLACE_NONE) {
            size_t gy = 0;
            do {
                ret = spng_get_row_info(spng_context, &row_info);
                if (ret)
                    break;
                ret = spng_decode_row(spng_context, buffer + gy * spng_stride, spng_stride);
            } while (!ret && ++gy < tile_size_y);

            if (ret && ret != SPNG_EOI) {
                fprintf(stderr, "%s: spng error: %s\n", argv[0], spng_strerror(ret));
                goto done;
            }
        }
        for (uint32_t x = 0; x < tile_width; x++) {
            if (ihdr.bit_depth > 8) {
                const uint16_t *tile_buffer = ((const uint16_t *)buffer) + x * tile_size_x * 4;
                const uint16_t *const rgb[3] = {tile_buffer, tile_buffer + 1, tile_buffer + 2};
                ret = hyd_send_tile(encoder, rgb, x, y, width * 4, 4);
            } else {
                const uint8_t *tile_buffer = ((const uint8_t *)buffer) + x * tile_size_x * 3;
                const uint8_t *const rgb[3] = {tile_buffer, tile_buffer + 1, tile_buffer + 2};
                ret = hyd_send_tile8(encoder, rgb, x, y, width * 3, 3);
            }
            if (ret != HYD_NEED_MORE_OUTPUT && ret < HYD_ERROR_START)
                goto done;
            do {
                size_t written;
                hyd_release_output_buffer(encoder, &written);
                fwrite(output_buffer, written, 1, fp);
                hyd_provide_output_buffer(encoder, output_buffer, output_bufsize);
                ret = hyd_flush(encoder);
            } while (ret == HYD_NEED_MORE_OUTPUT);
            if (ret != HYD_OK)
                goto done;
        }
    }

    ret = 0;

done:
    if (fp)
        fclose(fp);
    if (fin)
        fclose(fin);
    if (spng_context)
        spng_ctx_free(spng_context);
    if (encoder)
        hyd_encoder_destroy(encoder);
    if (buffer)
        free(buffer);
    if (output_buffer)
        free(output_buffer);

    fprintf(stderr, "Total libhydrium heap memory: %llu bytes\nMax libhydrium heap memory: %llu bytes\n",
        (long long unsigned)holder.total_alloced, (long long unsigned)holder.max_alloced);

    return ret;
}
