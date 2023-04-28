/*
 * libhydrium/hydrium.c
 * 
 * This is the main libhydrium API entry point, implementation.
 */

#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "memory.h"

static void *malloc_default(size_t size, void *opaque) {
    return malloc(size);
}

static void *calloc_default(size_t nmemb, size_t size, void *opaque) {
    return calloc(nmemb, size);
}

static void *realloc_default(void *ptr, size_t size, void *opaque) {
    return realloc(ptr, size);
}

static void free_default(void *ptr, void *opaque) {
    free(ptr);
}

HYDRIUM_EXPORT HYDEncoder *hyd_encoder_new(const HYDAllocator *allocator) {
    HYDEncoder *ret;

    if (allocator)
        ret = allocator->calloc_func(1, sizeof(HYDEncoder), allocator->opaque);
    else
        ret = calloc(1, sizeof(HYDEncoder));

    if (!ret)
        return NULL;

    if (allocator) {
        ret->allocator = *allocator;
    } else {
        ret->allocator.opaque = NULL;
        ret->allocator.malloc_func = &malloc_default;
        ret->allocator.calloc_func = &calloc_default;
        ret->allocator.realloc_func = &realloc_default;
        ret->allocator.free_func = &free_default;
    }

    return ret;
}

HYDRIUM_EXPORT HYDStatusCode hyd_encoder_destroy(HYDEncoder *encoder) {
    hyd_free(&encoder->allocator, encoder->working_writer.buffer);
    hyd_free(&encoder->allocator, encoder->xyb);
    hyd_free(&encoder->allocator, encoder);
    return HYD_OK;
}

HYDRIUM_EXPORT HYDStatusCode hyd_set_metadata(HYDEncoder *encoder, const HYDImageMetadata *metadata) {
    if (!metadata->width || !metadata->height)
        return HYD_API_ERROR;
    const uint64_t width64 = metadata->width;
    const uint64_t height64 = metadata->height;
    if (width64 > UINT64_C(1) << 30 || height64 > UINT64_C(1) << 30)
        return HYD_API_ERROR;
    /* won't overflow due to above check */
    if (width64 * height64 > UINT64_C(1) << 40)
        return HYD_API_ERROR;
    encoder->metadata = *metadata;
    if (width64 > (1 << 20) || height64 > (1 << 20) || width64 * height64 > (1 << 28))
        encoder->level10 = 1;

    if (metadata->tile_size_shift_x < 0 || metadata->tile_size_shift_x > 3)
        return HYD_API_ERROR;
    if (metadata->tile_size_shift_y < 0 || metadata->tile_size_shift_y > 3)
        return HYD_API_ERROR;

    encoder->tile_count_x = 1 << metadata->tile_size_shift_x;
    encoder->tile_count_y = 1 << metadata->tile_size_shift_y;
    return HYD_OK;
}

HYDRIUM_EXPORT HYDStatusCode hyd_provide_output_buffer(HYDEncoder *encoder, uint8_t *buffer, size_t buffer_len) {
    if (buffer_len < 64)
        return HYD_API_ERROR;
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
    *written = encoder->writer.buffer_pos;
    return encoder->writer.overflow_state;
}

HYDRIUM_EXPORT HYDStatusCode hyd_flush(HYDEncoder *encoder) {
    hyd_bitwriter_flush(&encoder->writer);
    size_t tocopy = encoder->writer.buffer_len - encoder->writer.buffer_pos;
    if (tocopy > encoder->working_writer.buffer_pos - encoder->copy_pos)
        tocopy = encoder->working_writer.buffer_pos - encoder->copy_pos;
    memcpy(encoder->writer.buffer + encoder->writer.buffer_pos, encoder->working_writer.buffer + encoder->copy_pos, tocopy);
    encoder->writer.buffer_pos += tocopy;
    encoder->copy_pos += tocopy;
    if (encoder->copy_pos >= encoder->working_writer.buffer_pos)
        return HYD_OK;
    
    return HYD_NEED_MORE_OUTPUT;
}
