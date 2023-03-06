/*
 * libhydrium/hydrium.h
 * 
 * This is the main libhydrium API entry point.
 */

#ifndef HYDRIUM_H_
#define HYDRIUM_H_

#include <stddef.h>
#include <stdint.h>

typedef enum HYDStatusCode {
    HYD_OK = 0,
    HYD_NEED_MORE_OUTPUT = -1,
    HYD_NEED_MORE_INPUT = -2,
    HYD_NOMEM = -3,
} HYDStatusCode;

typedef struct HYDAllocator {
    void *(*alloc_func)(size_t size, void *opaque);
    void (*free_func)(void *ptr, void *opaque);
} HYDAllocator;

typedef struct HYDImageMetadata {
    size_t width;
    size_t height;
    int linear_light;
} HYDImageMetadata;

typedef struct HYDEncoder {
    HYDImageMetadata metadata;
    /* 256x256 tile */
    uint16_t *yxb[3];

    uint8_t *out;
    size_t out_len;
} HYDEncoder;

/* 
 * default implementations for HYDAllocator
 * these just call malloc/free
 */
void *hyd_alloc(size_t size, void *opaque);
void hyd_free(void *ptr, void *opaque);

HYDEncoder *hyd_encoder_new(HYDAllocator *allocator);

HYDStatusCode hyd_set_metadata(HYDEncoder *encoder, HYDImageMetadata *metadata);

HYDStatusCode hyd_send_tile(HYDEncoder *encoder, const uint16_t *buffer,
                            size_t tile_x, size_t tile_y, ptrdiff_t row_stride, ptrdiff_t plane_stride);

HYDStatusCode hyd_send_tile8(HYDEncoder *encoder, const uint8_t *buffer,
                             size_t tile_x, size_t tile_y, ptrdiff_t row_stride, ptrdiff_t plane_stride);

HYDStatusCode hyd_receive_output(HYDEncoder *encoder, uint8_t *buffer, size_t buffer_len, size_t *received);

#endif
