#ifndef HYDRIUM_MEMORY_H_
#define HYDRIUM_MEMORY_H_

#include <stddef.h>
#include <stdlib.h>

#include "libhydrium/libhydrium.h"

static inline void hyd_freep(void *ptr) {
    void **ptrv = ptr;
    if (ptrv && *ptrv) {
        free(*ptrv);
        *ptrv = NULL;
    }
}

void *hyd_malloc_array(size_t nmemb, size_t size);
void *hyd_realloc_array(void *ptr, size_t nmemb, size_t size);

#endif /* HYDRIUM_MEMORY_H_ */
