#ifndef HYDRIUM_MEMORY_H_
#define HYDRIUM_MEMORY_H_

#include <stddef.h>
#include <stdlib.h>

#include "libhydrium/libhydrium.h"

static inline void hyd_freep(void *ptrp) {
    void **ptrv = ptrp;
    if (ptrv && *ptrv) {
        free(*ptrv);
        *ptrv = NULL;
    }
}

static inline void hyd_free_arraybuffer_p(void *array, void *ptrp) {
    void **ptrv = ptrp;
    if (array != *ptrv)
        hyd_freep(ptrv);
}

void *hyd_malloc_array(size_t nmemb, size_t size);
void *hyd_realloc_array(void *ptr, size_t nmemb, size_t size);
HYDStatusCode hyd_realloc_p(void *buffer, size_t buffer_size);
HYDStatusCode hyd_realloc_array_p(void *buffer, size_t nmemb, size_t size);
HYDStatusCode hyd_malloc_arraybuffer_p(size_t nmemb, size_t size, void *array,
    size_t sizeof_array, void *ptrp);
HYDStatusCode hyd_calloc_arraybuffer_p(size_t nmemb, size_t size, void *array,
    size_t sizeof_array, void *ptrp);

#endif /* HYDRIUM_MEMORY_H_ */
