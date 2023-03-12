/*
 * Hydrium basic implementation
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "libhydrium/libhydrium.h"

int main(int argc, const char *argv[]) {
    uint64_t width, height;
    uint8_t *buffer, *output_buffer = NULL;
    HYDEncoder *encoder = hyd_encoder_new(NULL);
    HYDStatusCode status = 1;

    if (!encoder)
        return ENOMEM;
    
    if (argc < 3)
        width = height = 256;
    else {
        width = atoll(argv[1]);
        height = atoll(argv[2]);
    }

    if (width > (UINT64_C(1) << 30) || height > (UINT64_C(1) << 30) || width * height > (UINT64_C(1) << 40)) {
        fprintf(stderr, "%s: buffer too big\n", argv[0]);
        return 1;
    }

    const size_t bufsize = width * height * 3;
    buffer = malloc(bufsize);
    if (!buffer)
        goto done;
    if (fread(buffer, bufsize, 1, stdin) < 1)
        goto done;
    output_buffer = malloc(bufsize);
    if (!output_buffer)
        goto done;

    fclose(stdin);

    HYDImageMetadata metadata;
    metadata.width = width;
    metadata.height = height;
    metadata.linear_light = 0;

    hyd_set_metadata(encoder, &metadata);
    hyd_provide_output_buffer(encoder, output_buffer, bufsize);
    const uint32_t tile_width = (width + 255) / 256;
    const uint32_t tile_height = (height + 255) / 256;
    for (uint32_t y = 0; y < tile_height; y++) {
        for (uint32_t x = 0; x < tile_width; x++) {
            uint8_t *buff_offset = buffer + (y << 8) * width * 3 + (x << 8) * 3;
            const uint8_t *const rgb[3] = {buff_offset, buff_offset + 1, buff_offset + 2};
            hyd_send_tile8(encoder, rgb, x, y, width * 3, 3);
            do {
                size_t written;
                hyd_release_output_buffer(encoder, &written);
                fwrite(output_buffer, written, 1, stdout);
                hyd_provide_output_buffer(encoder, output_buffer, bufsize);
                status = hyd_flush(encoder);
            } while (status == HYD_NEED_MORE_OUTPUT);
            if (status != HYD_OK)
                goto done;
        }
    }

    fclose(stdout);

    status = 0;

done:
    if (encoder)
        hyd_encoder_destroy(encoder);
    if (buffer)
        free(buffer);
    if (output_buffer)
        free(output_buffer);

    return status;
}
