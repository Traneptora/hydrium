#ifndef HYDRIUM_MEMORY_H_
#define HYDRIUM_MEMORY_H_

#include <stddef.h>

#include "libhydrium/libhydrium.h"

void *hyd_malloc(HYDAllocator *allocator, size_t size);
void *hyd_calloc(HYDAllocator *allocator, size_t nmemb, size_t size);
void *hyd_realloc(HYDAllocator *allocator, void *ptr, size_t size);
void hyd_free(HYDAllocator *allocator, void *ptr);

#endif /* HYDRIUM_MEMORY_H_ */
