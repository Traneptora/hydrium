/*
 * Hydrium basic implementation
 */

#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libhydrium/libhydrium.h"
#include "lodepng.h"

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
    uint8_t *buffer, *output_buffer = NULL;
    HYDEncoder *encoder = NULL;
    HYDStatusCode status = 1;
    FILE *fp = stdout;
    struct memory_holder holder = { 0 };

    if (argc < 2 || !strcmp(argv[1], "--help")) {
        fprintf(stderr, "Usage: %s <input.png> [output.jxl]\n", argv[0]);
        return argc >= 2;
    }
    unsigned w, h;
    unsigned ret = lodepng_decode24_file(&buffer, &w, &h, argv[1]);

    if (ret) {
        fprintf(stderr, "%s: error reading PNG file\n", argv[0]);
        goto done;
    }

    width = w;
    height = h;

    if (width > (UINT64_C(1) << 30) || height > (UINT64_C(1) << 30) || width * height > (UINT64_C(1) << 40)) {
        fprintf(stderr, "%s: buffer too big\n", argv[0]);
        goto done;
    }

    size_t bufsize = 3 * width * height;
    output_buffer = malloc(bufsize);
    if (!output_buffer)
        goto done;

    HYDAllocator allocator;

    allocator.alloc_func = &mem_alloc;
    allocator.free_func = &mem_free;
    allocator.opaque = &holder;

    encoder = hyd_encoder_new(&allocator);

    HYDImageMetadata metadata;
    metadata.width = width;
    metadata.height = height;
    metadata.linear_light = 0;

    if (argc > 2) {
        fp = fopen(argv[2], "wb");
        if (!fp) {
            fprintf(stderr, "%s: error opening file for writing: %s\n", argv[0], argv[2]);
            goto done;
        }
    }

    hyd_set_metadata(encoder, &metadata);
    hyd_provide_output_buffer(encoder, output_buffer, bufsize);
    const uint32_t tile_width = (width + 255) / 256;
    const uint32_t tile_height = (height + 255) / 256;
    for (uint32_t y = 0; y < tile_height; y++) {
        for (uint32_t x = 0; x < tile_width; x++) {
            uint8_t *buff_offset = buffer + (y << 8) * width * 3 + (x << 8) * 3;
            const uint8_t *const rgb[3] = {buff_offset, buff_offset + 1, buff_offset + 2};
            status = hyd_send_tile8(encoder, rgb, x, y, width * 3, 3);
            if (status != HYD_NEED_MORE_OUTPUT && status < HYD_ERROR_START)
                goto done;
            do {
                size_t written;
                hyd_release_output_buffer(encoder, &written);
                fwrite(output_buffer, written, 1, fp);
                hyd_provide_output_buffer(encoder, output_buffer, bufsize);
                status = hyd_flush(encoder);
            } while (status == HYD_NEED_MORE_OUTPUT);
            if (status != HYD_OK)
                goto done;
        }
    }

    status = 0;

done:
    if (fp)
        fclose(fp);
    if (encoder)
        hyd_encoder_destroy(encoder);
    if (buffer)
        free(buffer);
    if (output_buffer)
        free(output_buffer);

    fprintf(stderr, "Total libhydrium heap memory: %lu bytes\nMax libhydrium heap memory: %lu bytes\n",
        holder.total_alloced, holder.max_alloced);

    return status;
}
