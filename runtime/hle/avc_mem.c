/* The allocator FFmpeg's decoder uses on the Wii.  The libraries that tools/build_ffmpeg.sh builds have their references to
   malloc, free, realloc and memalign renamed to tkdr_ff_* (objcopy --redefine-sym), and these are the functions that answer:
   the newlib heap lives in MEM1, where about 1 MB is left (runtime/bigmem.h), and the decoder wants a few MB of frames and
   tables, so its blocks come from the MEM2 pool instead.  Every block is 32-byte aligned (the pool's blocks are, and the
   header is 32 bytes), which covers every alignment FFmpeg asks for here (16).  The size sits in the header, for realloc. */
#ifdef TKDR_FFMPEG
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bigmem.h"

#define FFHDR 32u

void *tkdr_ff_malloc(size_t n);
void tkdr_ff_free(void *p);
void *tkdr_ff_realloc(void *p, size_t n);
void *tkdr_ff_memalign(size_t align, size_t n);

void *tkdr_ff_malloc(size_t n) {
    if (n > 0x7FFFFFFFu - FFHDR) return NULL;
    uint8_t *p = (uint8_t *)big_alloc(n + FFHDR);
    if (!p) return NULL;
    *(size_t *)p = n;
    return p + FFHDR;
}

void tkdr_ff_free(void *q) {
    if (q) big_free((uint8_t *)q - FFHDR);
}

void *tkdr_ff_realloc(void *q, size_t n) {
    if (!q) return tkdr_ff_malloc(n);
    if (!n) { tkdr_ff_free(q); return NULL; }
    size_t old = *(size_t *)((uint8_t *)q - FFHDR);
    if (n <= old) return q;
    void *r = tkdr_ff_malloc(n);
    if (!r) return NULL;
    memcpy(r, q, old);
    tkdr_ff_free(q);
    return r;
}

void *tkdr_ff_memalign(size_t align, size_t n) {
    if (align > 32) {
        fprintf(stderr, "FFMEM alignment %u not supported\n", (unsigned)align);
        return NULL;
    }
    return tkdr_ff_malloc(n);
}
#endif
