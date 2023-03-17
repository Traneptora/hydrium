#include <stdio.h>
#include <stdlib.h>

#include "bitwriter.h"
#include "entropy.h"
#include "internal.h"
#include "osdep.h"

static void *malloc2(size_t size, void *opaque) {
    return malloc(size);
}

static void free2(void *ptr, void *opaque) {
    free(ptr);
}


int main(void) {
    HYDAllocator allocator;
    HYDEntropyStream stream;
    HYDBitWriter bw;
    uint8_t buffer[4096];
    allocator.alloc_func = &malloc2;
    allocator.free_func = &free2;
    allocator.opaque = NULL;
    hyd_init_bit_writer(&bw, buffer, sizeof(buffer), 0, 0);
    hyd_init_entropy_stream(&stream, &allocator, &bw, 256, (const uint8_t[1]){0}, 1);
    for (int i = 0; i < 256; i++)
        hyd_ans_send_symbol(&stream, 0, hyd_fllog2(1 + i));
    hyd_finalize_entropy_stream(&stream);
    hyd_bitwriter_flush(&bw);
    fwrite(buffer, bw.buffer_pos, 1, stdout);
    fclose(stdout);
    return ferror(stdout);
}
