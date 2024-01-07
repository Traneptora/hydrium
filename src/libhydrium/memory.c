/*
 * libhydrium memory.c
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "libhydrium/libhydrium.h"
#include "memory.h"

void *hyd_malloc(HYDAllocator *allocator, size_t size) {
    return allocator->malloc_func(size, allocator->opaque);
}

void *hyd_calloc(HYDAllocator *allocator, size_t nmemb, size_t size) {
    return allocator->calloc_func(nmemb, size, allocator->opaque);
}

void *hyd_realloc(HYDAllocator *allocator, void *ptr, size_t size) {
    return allocator->realloc_func(ptr, size, allocator->opaque);
}

void hyd_free(HYDAllocator *allocator, void *ptr) {
    if (!ptr)
        return;
    allocator->free_func(ptr, allocator->opaque);
}

void *hyd_recalloc(HYDAllocator *allocator, void *ptr, size_t nmemb, size_t size) {
    size_t total_size = nmemb * size;
    if (size && total_size / size != nmemb)
        return NULL;
    void *ret = hyd_realloc(allocator, ptr, total_size);
    if (!ret)
        return NULL;
    memset(ret, 0, total_size);
    return ret;
}

void *hyd_mallocarray(HYDAllocator *allocator, size_t nmemb, size_t size) {
    size_t total_size = nmemb * size;
    if (size && total_size / size != nmemb)
        return NULL;
    return hyd_malloc(allocator, total_size);
}

void *hyd_reallocarray(HYDAllocator *allocator, void *ptr, size_t nmemb, size_t size) {
    size_t total_size = nmemb * size;
    if (size && total_size / size != nmemb)
        return NULL;
    return hyd_realloc(allocator, ptr, total_size);
}

static void *profiling_malloc(size_t size, void *profilerv) {
    HYDMemoryProfiler *profiler = profilerv;
    size += sizeof(size_t);
    void *ptr = malloc(size);
    if (!ptr)
        return NULL;
    profiler->total_alloced += size;
    profiler->current_alloced += size;
    if (profiler->current_alloced > profiler->max_alloced)
        profiler->max_alloced = profiler->current_alloced;
    *(size_t *)ptr = size;
    return ptr + sizeof(size_t);
}

static void *profiling_calloc(size_t nmemb, size_t size, void *profilerv) {
    HYDMemoryProfiler *profiler = profilerv;
    size_t total_size = nmemb * size;
    // check overflow
    if (size && total_size / size != nmemb)
        return NULL;
    total_size += sizeof(size_t);
    // calloc should outperform malloc + memset(0)
    void *ptr = calloc(1, total_size);
    if (!ptr)
        return NULL;
    profiler->total_alloced += size;
    profiler->current_alloced += size;
    if (profiler->current_alloced > profiler->max_alloced)
        profiler->max_alloced = profiler->current_alloced;
    *(size_t *)ptr = size;
    return ptr + sizeof(size_t);
}

static void *profiling_realloc(void *ptr, size_t size, void *profilerv) {
    if (!ptr)
        return profiling_malloc(size, profilerv);
    HYDMemoryProfiler *profiler = profilerv;
    size += sizeof(size_t);
    void *og_ptr = ptr - sizeof(size_t);
    void *new_ptr = realloc(og_ptr, size);
    if (!new_ptr)
        return NULL;
    profiler->current_alloced += size;
    profiler->total_alloced += size;
    profiler->current_alloced -= *(size_t *)new_ptr;
    if (new_ptr == og_ptr)
        profiler->total_alloced -= *(size_t *)new_ptr;
    if (profiler->current_alloced > profiler->max_alloced)
        profiler->max_alloced = profiler->current_alloced;
    *(size_t *)new_ptr = size;
    return new_ptr + sizeof(size_t);
}

static void profiling_free(void *ptr, void *profilerv) {
    HYDMemoryProfiler *profiler = profilerv;
    if (!ptr)
        return;
    void *og_ptr = ptr - sizeof(size_t);
    profiler->current_alloced -= *(size_t *)og_ptr;
    free(og_ptr);
}

HYDRIUM_EXPORT HYDAllocator *hyd_profiling_allocator_new(HYDMemoryProfiler *profiler) {
    HYDAllocator *allocator = profiling_malloc(sizeof(HYDAllocator), profiler);
    allocator->opaque = profiler;
    allocator->malloc_func = &profiling_malloc;
    allocator->calloc_func = &profiling_calloc;
    allocator->realloc_func = &profiling_realloc;
    allocator->free_func = &profiling_free;
    return allocator;
}

HYDRIUM_EXPORT void hyd_profiling_allocator_destroy(HYDAllocator *allocator) {
    if (!allocator)
        return;
    allocator->free_func(allocator, allocator->opaque);
}
