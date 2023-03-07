/*
 * libhydrium/hydrium.c
 * 
 * This is the main libhydrium API entry point, implementation.
 */

#include <stdlib.h>
#include <string.h>

#include "hydrium.h"
#include "internal.h"

void *hyd_alloc(size_t size, void *opaque) {
    return malloc(size);
}

void hyd_free(void *ptr, void *opaque) {
    free(ptr);
}

HYDEncoder *hyd_encoder_new(HYDAllocator *allocator) {
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

HYDStatusCode hyd_encoder_destroy(HYDEncoder *encoder) {
    HYD_FREE(encoder, encoder);

    return HYD_OK;
}

HYDStatusCode hyd_set_metadata(HYDEncoder *encoder, HYDImageMetadata *metadata) {
    if (!metadata->width || !metadata->height)
        return HYD_API_ERROR;
    encoder->metadata = *metadata;

    return HYD_OK;
}

HYDStatusCode hyd_provide_output_buffer(HYDEncoder *encoder, uint8_t *buffer, size_t buffer_len) {
    if (buffer_len < 32)
        return HYD_API_ERROR;
    encoder->out = buffer;
    encoder->out_len = buffer_len;
    encoder->out_pos = 0;
    if (encoder->writer.overflow_pos > 0) {
        memcpy(encoder->out, encoder->writer.overflow, encoder->writer.overflow_pos);
        encoder->out_pos = encoder->writer.overflow_pos;
    }
    hyd_init_bit_writer(&encoder->writer, buffer, buffer_len, encoder->writer.cache, encoder->writer.cache_bits);
}
