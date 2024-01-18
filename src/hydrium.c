/*
 * Hydrium basic implementation
 */

#ifdef _MSC_VER
    #include <io.h>
    #define hyd_isatty(f) _isatty(_fileno(f))
#else
    #define _POSIX_C_SOURCE 1
    #include <unistd.h>
    #define hyd_isatty(f) isatty(fileno(f))
#endif

#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <spng.h>

#include "libhydrium/libhydrium.h"

static void print_usage(const char *argv0) {
    fprintf(stderr, "Usage: %s [options] <input.png> <output.jxl>\n", argv0);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "    --help         Print this message\n");
    fprintf(stderr, "    --tile-size=N  Use Tile Size Shift = N, valid values are 0, 1, 2, 3\n");
    fprintf(stderr, "    --one-frame    Use one frame. Uses more memory but decodes faster.\n");
}

int main(int argc, const char *argv[]) {
    uint64_t width, height;
    void *buffer = NULL, *output_buffer = NULL;
    HYDEncoder *encoder = NULL;
    HYDAllocator *allocator = NULL;
    int ret = 1;
    FILE *fp = stdout, *fin = stdin;
    HYDMemoryProfiler profiler = { 0 };
    const char *error_msg = NULL;

    fprintf(stderr, "libhydrium version %s\n", HYDRIUM_VERSION_STRING);

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    int one_frame = 0;
    long tilesize = 0;
    int argp = 1;
    const char *in_fname = NULL;
    const char *out_fname = NULL;

    while (argp < argc) {
        if (strncmp(argv[argp], "--", 2)) {
            if (!in_fname)
                in_fname = argv[argp];
            else if (!out_fname)
                out_fname = argv[argp];
            else {
                fprintf(stderr, "Invalid trailing arg: %s\n", argv[argp]);
                print_usage(argv[0]);
                return 2;
            }
        } else if (!strcmp(argv[argp], "--help")) {
            print_usage(argv[0]);
            return 0;
        } else if (!argv[argp][2]) {
            argp++;
            break;
        } else if (!strcmp(argv[argp], "--one-frame")) {
            one_frame = 1;
        } else if (!strncmp(argv[argp], "--tile-size=", 12)) {
            errno = 0;
            tilesize = strtol(argv[argp] + 12, NULL, 10);
            if (errno) {
                fprintf(stderr, "Invalid integer: %s\n", argv[argp] + 12);
                print_usage(argv[0]);
                return 2;
            }
            if (tilesize < 0 || tilesize > 3) {
                fprintf(stderr, "Invalid tile size, must be 0-3: %s\n", argv[argp] + 12);
                print_usage(argv[0]);
                return 2;
            }
        }
        argp++;
    }

    spng_ctx *spng_context = spng_ctx_new(0);
    if (!spng_context) {
        fprintf(stderr, "%s: couldn't allocate context\n", argv[0]);
        goto done;
    }

    spng_set_crc_action(spng_context, SPNG_CRC_USE, SPNG_CRC_USE);
    const size_t chunk_limit = 1 << 20;
    spng_set_chunk_limits(spng_context, chunk_limit, chunk_limit);

    if (in_fname && strcmp(in_fname, "-")) {
        fin = fopen(in_fname, "rb");
        if (!fin) {
            fprintf(stderr, "%s: error opening file: %s\n", argv[0], in_fname);
            goto done;
        }
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
    metadata.tile_size_shift_x = one_frame ? -1 : tilesize;
    metadata.tile_size_shift_y = one_frame ? -1 : tilesize;
    const uint32_t size_shift_x = metadata.tile_size_shift_x < 0 ? 3 : metadata.tile_size_shift_x;
    const uint32_t size_shift_y = metadata.tile_size_shift_y < 0 ? 3 : metadata.tile_size_shift_y;
    const uint32_t tile_size_x = 256 << size_shift_x;
    const uint32_t tile_size_y = 256 << size_shift_y;
    const uint32_t tile_width = (width + tile_size_x - 1) >> (8 + size_shift_x);
    const uint32_t tile_height = (height + tile_size_y - 1) >> (8 + size_shift_y);

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

    allocator = hyd_profiling_allocator_new(&profiler);
    if (!allocator) {
        fprintf(stderr, "%s: error allocating encoder\n", argv[0]);
        goto done;
    }

    encoder = hyd_encoder_new(allocator);
    if (!encoder) {
        fprintf(stderr, "%s: error allocating encoder\n", argv[0]);
        goto done;
    }

    if (out_fname && strcmp(out_fname, "-")) {
        fp = fopen(out_fname, "wb");
        if (!fp) {
            fprintf(stderr, "%s: error opening file for writing: %s\n", argv[0], out_fname);
            goto done;
        }
    }

    if (hyd_isatty(fp)) {
        fprintf(stderr, "%s: Not writing compressed data to a terminal.\n", argv[0]);
        print_usage(argv[0]);
        ret = 3;
        goto done;
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
                // We divide by 2 because spng_stride is in bytes, not in uint16_t units
                ret = hyd_send_tile(encoder, rgb, x, y, spng_stride / 2, 4, -1);
            } else {
                const uint8_t *tile_buffer = ((const uint8_t *)buffer) + x * tile_size_x * 3;
                const uint8_t *const rgb[3] = {tile_buffer, tile_buffer + 1, tile_buffer + 2};
                ret = hyd_send_tile8(encoder, rgb, x, y, spng_stride, 3, -1);
            }
            if (ret != HYD_NEED_MORE_OUTPUT && ret < HYD_ERROR_START)
                goto done;
            do {
                ret = hyd_flush(encoder);
                size_t written;
                hyd_release_output_buffer(encoder, &written);
                fwrite(output_buffer, written, 1, fp);
                hyd_provide_output_buffer(encoder, output_buffer, output_bufsize);
            } while (ret == HYD_NEED_MORE_OUTPUT);
            if (ret != HYD_OK)
                goto done;
        }
    }

done:
    if (fp)
        fclose(fp);
    if (fin)
        fclose(fin);
    if (spng_context)
        spng_ctx_free(spng_context);
    error_msg = hyd_error_message_get(encoder);
    hyd_encoder_destroy(encoder);
    if (buffer)
        free(buffer);
    if (output_buffer)
        free(output_buffer);
    hyd_profiling_allocator_destroy(allocator);
    if (ret < HYD_ERROR_START)
        fprintf(stderr, "Hydrium error occurred. Error code: %d\n", ret);
    if (error_msg && *error_msg)
        fprintf(stderr, "Error message: %s\n", error_msg);
    if (!ret)
        fprintf(stderr, "Total libhydrium heap memory: %llu bytes\nMax libhydrium heap memory: %llu bytes\n",
            (long long unsigned)profiler.total_alloced, (long long unsigned)profiler.max_alloced);

    return ret;
}
