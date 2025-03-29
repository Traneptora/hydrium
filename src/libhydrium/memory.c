/*
 * libhydrium memory.c
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "libhydrium/libhydrium.h"
#include "memory.h"

void *hyd_malloc_array(size_t nmemb, size_t size) {
    size_t total_size = nmemb * size;
    if (size && total_size / size != nmemb)
        return NULL;
    return malloc(total_size);
}

void *hyd_realloc_array(void *ptr, size_t nmemb, size_t size) {
    size_t total_size = nmemb * size;
    if (size && total_size / size != nmemb)
        return NULL;
    return realloc(ptr, total_size);
}
