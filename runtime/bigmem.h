/* Big blocks.  On the Wii the heap malloc() draws on is almost gone (MEM1 is
   24 MiB and the program takes nearly all of it), so decoded textures, audio
   files and movie buffers come from a pool in MEM2 instead (see
   platform/wii/wii_memory.c).  big_alloc may return NULL there, and big_free
   also takes pointers from malloc().  Elsewhere they are plain malloc/free. */
#pragma once
#include <stdlib.h>

#ifdef __wii__
void *wii_big_alloc(size_t n);
void wii_big_free(void *p);
#define big_alloc wii_big_alloc
#define big_free wii_big_free
#else
#define big_alloc malloc
#define big_free free
#endif
