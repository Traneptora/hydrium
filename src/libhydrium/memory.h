#ifndef HYDRIUM_MEMORY_H_
#define HYDRIUM_MEMORY_H_

#include <stddef.h>

#include "libhydrium/libhydrium.h"

#define hyd_freep(a, ptr) do {  \
    if((ptr) && *(ptr)) {       \
        hyd_free((a), *(ptr));  \
        *(ptr) = NULL;          \
    }                           \
} while (0)

void *hyd_malloc(HYDAllocator *allocator, size_t size);
void *hyd_mallocarray(HYDAllocator *allocator, size_t nmemb, size_t size);
void *hyd_calloc(HYDAllocator *allocator, size_t nmemb, size_t size);
void *hyd_realloc(HYDAllocator *allocator, void *ptr, size_t size);
void *hyd_recalloc(HYDAllocator *allocator, void *ptr, size_t nmemb, size_t size);
void *hyd_reallocarray(HYDAllocator *allocator, void *ptr, size_t nmemb, size_t size);
void hyd_free(HYDAllocator *allocator, void *ptr);

#endif /* HYDRIUM_MEMORY_H_ */
