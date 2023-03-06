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
