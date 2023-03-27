/*
 * libhydrium/hydrium.c
 * 
 * This is the main libhydrium API entry point, implementation.
 */

#include <stdlib.h>
#include <string.h>

#include "internal.h"

static void *hyd_alloc(size_t size, void *opaque) {
    return malloc(size);
}

static void hyd_free(void *ptr, void *opaque) {
    free(ptr);
}

HYDRIUM_EXPORT HYDEncoder *hyd_encoder_new(const HYDAllocator *allocator) {
    HYDEncoder *ret;

    if (allocator)
        ret = allocator->alloc_func(sizeof(HYDEncoder), allocator->opaque);
    else
        ret = hyd_alloc(sizeof(HYDEncoder), NULL);

    if (!ret)
        return NULL;

    memset(ret, 0, sizeof(HYDEncoder));

    if (allocator) {
        ret->allocator = *allocator;
    } else {
        ret->allocator.opaque = NULL;
        ret->allocator.alloc_func = &hyd_alloc;
        ret->allocator.free_func = &hyd_free;
    }

    return ret;
}

HYDRIUM_EXPORT HYDStatusCode hyd_encoder_destroy(HYDEncoder *encoder) {
    HYD_FREE(encoder, encoder);

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
    memcpy(encoder->writer.buffer + encoder->writer.buffer_pos, encoder->working_buffer + encoder->copy_pos, tocopy);
    encoder->writer.buffer_pos += tocopy;
    encoder->copy_pos += tocopy;
    if (encoder->copy_pos >= encoder->working_writer.buffer_pos)
        return HYD_OK;
    
    return HYD_NEED_MORE_OUTPUT;
}
