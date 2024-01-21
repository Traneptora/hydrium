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

static const char pfm_sig[4] = "PF\n";

static void print_usage(const char *argv0) {
    fprintf(stderr, "Usage: %s [options] <input.png> <output.jxl>\n", argv0);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "    --help         Print this message\n");
    fprintf(stderr, "    --tile-size=N  Use Tile Size Shift = N, valid values are 0, 1, 2, 3\n");
    fprintf(stderr, "    --one-frame    Use one frame. Uses more memory but decodes faster.\n");
}

static int init_spng_stream(spng_ctx **ctx, const char **error_msg, FILE *fin, struct spng_ihdr *ihdr) {

    spng_ctx *spng_context = spng_ctx_new(0);
    if (!spng_context) {
        *error_msg = "couldn't allocate context";
        return -1;
    }

    *ctx = spng_context;

    spng_set_crc_action(spng_context, SPNG_CRC_USE, SPNG_CRC_USE);
    const size_t chunk_limit = 1 << 20;
    spng_set_chunk_limits(spng_context, chunk_limit, chunk_limit);

    spng_set_png_file(spng_context, fin);

    int ret = spng_get_ihdr(spng_context, ihdr);
    if (ret) {
        *error_msg = spng_strerror(ret);
        return -1;
    }

    return 0;
}

int main(int argc, const char *argv[]) {
    uint64_t width = 0, height = 0;
    void *buffer = NULL, *output_buffer = NULL;
    HYDEncoder *encoder = NULL;
    HYDAllocator *allocator = NULL;
    int ret = 1;
    FILE *fp = stdout, *fin = stdin;
    HYDMemoryProfiler profiler = { 0 };
    const char *error_msg = NULL;
    spng_ctx *spng_context = NULL;

    fprintf(stderr, "libhydrium version %s\n", HYDRIUM_VERSION_STRING);

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    int one_frame = 0;
    int pfm = 0;
    int linear = 0;
    int endianness = 0;
    long tilesize = 0;
    int argp = 0;
    const char *in_fname = NULL;
    const char *out_fname = NULL;

    while (++argp < argc) {
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
        } else if (!strcmp(argv[argp], "--pfm")) {
            pfm = 1;
        } else if (!strcmp(argv[argp], "--linear")) {
            linear = 1;
        }
    }

    if (in_fname && strcmp(in_fname, "-")) {
        fin = fopen(in_fname, "rb");
        if (!fin) {
            fprintf(stderr, "%s: error opening file: %s\n", argv[0], in_fname);
            goto done;
        }
    }

    struct spng_ihdr ihdr;

    if (!pfm) {
        ret = init_spng_stream(&spng_context, &error_msg, fin, &ihdr);
        if (ret < 0)
            goto done;
        width = ihdr.width;
        height = ihdr.height;
    } else {
        int c;
        char sig[3];
        size_t r = fread(sig, 3, 1, fin);
        if (!r || memcmp(sig, pfm_sig, 3)) {
            error_msg = "not a color PFM file";
            goto done;
        }
        while (1) {
            c = fgetc(fin);
            if (c >= '0' && c <= '9')
                width = width * 10 + (c - '0');
            else if (c == ' ')
                break;
            else {
                error_msg = "invalid PFM width";
                goto done;
            }
            if (width > (UINT64_C(1) << 30))
                break;
        }
        while (1) {
            c = fgetc(fin);
            if (c >= '0' && c <= '9')
                height = height * 10 + (c - '0');
            else if (c == '\n')
                break;
            else {
                error_msg = "invalid PFM height";
                goto done;
            }
            if (height > (UINT64_C(1) << 30))
                break;
        }
        c = fgetc(fin);
        if (c != '-')
            endianness = 1;
        else
            endianness = -1;
        while (1) {
            c = fgetc(fin);
            if (c == '\n')
                break;
            if (c < 0) {
                error_msg = "invalid PFM endianness";
                goto done;
            }
        }
    }

    if (pfm) {
        const int i = 1;
        int little = *((char *)&i);
        int ne = (little && endianness < 0) || (!little && endianness > 0);
        endianness = ne ? 1 : -1;
    }

    if (width > (UINT64_C(1) << 30) || height > (UINT64_C(1) << 30) || width * height > (UINT64_C(1) << 40)) {
        fprintf(stderr, "%s: buffer too big\n", argv[0]);
        goto done;
    }

    enum spng_format sample_fmt = 0;
    ptrdiff_t buffer_stride;
    size_t decoded_image_buffer_size;
    if (!pfm) {
        sample_fmt = ihdr.bit_depth > 8 ? SPNG_FMT_RGBA16 : SPNG_FMT_RGB8;
        ret = spng_decoded_image_size(spng_context, sample_fmt, &decoded_image_buffer_size);
        if (ret) {
            fprintf(stderr, "%s: spng error: %s\n", argv[0], spng_strerror(ret));
            goto done;
        }
        buffer_stride = decoded_image_buffer_size / height;
    } else {
        buffer_stride = 12 * width;
        decoded_image_buffer_size = buffer_stride * height;
    }

    HYDImageMetadata metadata;
    metadata.width = width;
    metadata.height = height;
    metadata.linear_light = linear;
    metadata.tile_size_shift_x = one_frame ? -1 : tilesize;
    metadata.tile_size_shift_y = one_frame ? -1 : tilesize;
    const uint32_t size_shift_x = metadata.tile_size_shift_x < 0 ? 3 : metadata.tile_size_shift_x;
    const uint32_t size_shift_y = metadata.tile_size_shift_y < 0 ? 3 : metadata.tile_size_shift_y;
    const uint32_t tile_size_x = 256 << size_shift_x;
    const uint32_t tile_size_y = 256 << size_shift_y;
    const uint32_t tile_width = (width + tile_size_x - 1) >> (8 + size_shift_x);
    const uint32_t tile_height = (height + tile_size_y - 1) >> (8 + size_shift_y);

    if (!pfm) {
        buffer = malloc(ihdr.interlace_method != SPNG_INTERLACE_NONE ?
            decoded_image_buffer_size : buffer_stride * tile_size_y);
    } else {
        buffer = malloc(buffer_stride * tile_size_y);
    }

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

    if (!pfm) {
        if (ihdr.interlace_method != SPNG_INTERLACE_NONE)
            ret = spng_decode_image(spng_context, buffer, decoded_image_buffer_size, sample_fmt, 0);
        else
            ret = spng_decode_image(spng_context, NULL, 0, sample_fmt, SPNG_DECODE_PROGRESSIVE);
        if (ret) {
            fprintf(stderr, "%s: spng error: %s\n", argv[0], spng_strerror(ret));
            goto done;
        }
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
    for (uint32_t yk = 0; yk < tile_height; yk++) {
        uint32_t y = pfm ? tile_height - yk - 1 : yk;
        uint32_t this_tile_height = tile_size_y;
        if (!pfm && ihdr.interlace_method == SPNG_INTERLACE_NONE) {
            size_t gy = 0;
            do {
                ret = spng_get_row_info(spng_context, &row_info);
                if (ret)
                    break;
                ret = spng_decode_row(spng_context, buffer + gy * buffer_stride, buffer_stride);
            } while (!ret && ++gy < tile_size_y);

            if (ret && ret != SPNG_EOI) {
                fprintf(stderr, "%s: spng error: %s\n", argv[0], spng_strerror(ret));
                goto done;
            }
        } else if (pfm) {
            if (!yk)
                this_tile_height = height - ((height - 1) / tile_size_y) * tile_size_y;
            for (size_t gy = 0; gy < this_tile_height; gy++) {
                void *row = buffer + gy * buffer_stride;
                size_t read = fread(row, buffer_stride, 1, fin);
                if (!read) {
                    fprintf(stderr, "%s: incomplete pfm read\n", argv[0]);
                    goto done;
                }
                if (endianness < 0) {
                    uint32_t *irow = row;
                    for (size_t gx = 0; gx < 3 * width; gx++) {
                        const uint32_t n = irow[gx];
                        /* gcc generates a single bswap instruction here */
                        irow[gx] = ((n & 0xff000000) >> 24) | ((n & 0x00ff0000) >> 8)
                            | ((n & 0x0000ff00) << 8) | ((n & 0xff) << 24);
                    }
                }
            }
        }
        for (uint32_t x = 0; x < tile_width; x++) {
            if (!pfm && ihdr.bit_depth > 8) {
                const uint16_t *tile_buffer = ((const uint16_t *)buffer) + x * tile_size_x * 4;
                const void *const rgb[3] = {tile_buffer, tile_buffer + 1, tile_buffer + 2};
                // We divide by 2 because spng_stride is in bytes, not in uint16_t units
                ret = hyd_send_tile(encoder, rgb, x, y, buffer_stride / 2, 4, -1, HYD_UINT16);
            } else if (!pfm) {
                const uint8_t *tile_buffer = ((const uint8_t *)buffer) + x * tile_size_x * 3;
                const void *const rgb[3] = {tile_buffer, tile_buffer + 1, tile_buffer + 2};
                ret = hyd_send_tile(encoder, rgb, x, y, buffer_stride, 3, -1, HYD_UINT8);
            } else {
                void *buffer_bottom = buffer + buffer_stride * (this_tile_height - 1);
                const float *tile_buffer = ((const float *)buffer_bottom) + x * tile_size_x * 3;
                const void *const rgb[3] = {tile_buffer, tile_buffer + 1, tile_buffer + 2};
                ret = hyd_send_tile(encoder, rgb, x, y, -buffer_stride / 4, 3, y == 0 && x == tile_width - 1,
                        HYD_FLOAT32);
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
    if (encoder) {
        error_msg = hyd_error_message_get(encoder);
        hyd_encoder_destroy(encoder);
    }
    free(buffer);
    free(output_buffer);
    hyd_profiling_allocator_destroy(allocator);
    if (ret < HYD_ERROR_START)
        fprintf(stderr, "Hydrium error occurred. Error code: %d\n", ret);
    if (error_msg && *error_msg)
        fprintf(stderr, "Error message: %s\n", error_msg);
    if (!ret)
        fprintf(stderr, "Total libhydrium heap memory: %zu bytes\nMax libhydrium heap memory: %zu bytes\n",
            profiler.total_alloced, profiler.max_alloced);

    return ret;
}
