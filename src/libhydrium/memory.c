/*
 * libhydrium memory.c
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "libhydrium/libhydrium.h"
#include "memory.h"

#define total_size_check(n, s, retv) \
size_t total_size = (n) * (s); \
if ((s) && total_size / (s) != (n)) \
    return (retv);

void *hyd_malloc_array(size_t nmemb, size_t size) {
    total_size_check(nmemb, size, NULL);
    return malloc(total_size);
}

void *hyd_realloc_array(void *ptr, size_t nmemb, size_t size) {
    total_size_check(nmemb, size, NULL);
    return realloc(ptr, total_size);
}

HYDStatusCode hyd_realloc_p(void *buffer, size_t buffer_size) {
    void **bufferp = buffer;
    void *new_buffer = realloc(*bufferp, buffer_size);
    if (!new_buffer)
        return HYD_NOMEM;
    *bufferp = new_buffer;

    return HYD_OK;
}

HYDStatusCode hyd_realloc_array_p(void *buffer, size_t nmemb, size_t size) {
    total_size_check(nmemb, size, HYD_NOMEM);
    return hyd_realloc_p(buffer, total_size);
}

HYDStatusCode hyd_malloc_arraybuffer_p(size_t nmemb, size_t size, void *array,
        size_t sizeof_array, void *ptrp) {
    void **ptr = ptrp;
    total_size_check(nmemb, size, HYD_NOMEM);
    if (total_size > sizeof_array) {
        *ptr = malloc(total_size);
        if (!*ptr)
            return HYD_NOMEM;
    } else {
        *ptr = array;
    }

    return HYD_OK;
}

HYDStatusCode hyd_calloc_arraybuffer_p(size_t nmemb, size_t size, void *array,
        size_t sizeof_array, void *ptrp) {
    void **ptr = ptrp;
    total_size_check(nmemb, size, HYD_NOMEM);
    if (total_size > sizeof_array) {
        *ptr = calloc(nmemb, size);
        if (!*ptr)
            return HYD_NOMEM;
    } else {
        memset(array, 0, sizeof_array);
        *ptr = array;
    }

    return HYD_OK;
}
