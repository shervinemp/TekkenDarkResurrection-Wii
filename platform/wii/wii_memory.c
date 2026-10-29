#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/system.h>

#include "wii_platform.h"

/* The PSP user partition is 0x08800000..0x09FFFFFF (24 MiB) and EDRAM is
   2 MiB at 0x04000000; the kernel partition is 0x08000000..0x087FFFFF.
   The Wii does not have that much RAM, so the used windows are carved out of
   MEM2 and the rest of the PSP address space lands in a scratch region.

   The window must cover the whole 0x08000000..0x09FFFFFF range: the game is
   loaded at 0x08804000 and allocates from the user partition above it, so
   stopping at 0x0A000000 would leave it with a fraction of the RAM it
   expects.  32 MiB of guest RAM plus 2 MiB of VRAM fits in MEM2's ~51 MiB
   arena (IOS keeps the top of MEM2).

   The scratch region backs 0x0A000000..0x0A1FFFFFF, the "extended" user RAM
   of a PSP-2000/3000, one byte per guest byte.  A PSP-1000 has no memory
   there, so a title that reads it sees zeroes rather than another address'
   bytes. */
#define GUEST_RAM_BYTES  (32u << 20)
#define GUEST_VRAM_BYTES (2u << 20)
#define GUEST_LOW_BYTES  (2u << 20)

uint8_t *g_mem;
uint8_t *g_vram;
uint8_t *g_low;
#ifdef TKDR_WATCH
uint32_t *g_wtp_ep, *g_wtp_ra;
uint32_t g_wtp_cur = 1, g_wtp_prev;
void wtp_frame(void) { g_wtp_prev = g_wtp_cur; g_wtp_cur++; }
#endif

static uint8_t *arena2_carve(size_t bytes) {
    uintptr_t lo = (uintptr_t)SYS_GetArena2Lo();
    uintptr_t hi = (uintptr_t)SYS_GetArena2Hi();
    lo = (lo + 31u) & ~(uintptr_t)31u;
    if (lo + bytes > hi) {
        printf("WII memory: MEM2 exhausted (need %u, free %u)\n",
               (unsigned)bytes, (unsigned)(hi > lo ? hi - lo : 0));
        return NULL;
    }
    SYS_SetArena2Lo((void *)(lo + bytes));
    memset((void *)lo, 0, bytes);
    return (uint8_t *)lo;
}

/* ----------------------------------------------------------- big blocks

   MEM1 is 24 MiB and the program image, its bss and the host thread stacks
   take nearly all of it: the heap that malloc() draws on has well under a
   megabyte to spare.  The decoded textures (hundreds of kilobytes each) are
   far too big for that, so they come from a pool carved out of MEM2.  The
   pool is a boundary tag allocator: every block starts with a 32 byte
   header, so a block's payload is 32 byte aligned (what the GX wants of a
   texture), and a freed block merges with its free neighbours. */
#define POOL_BYTES (14u << 20)

typedef struct {
    uint32_t size;   /* of the whole block, header included */
    uint32_t prev;   /* of the block before it, 0 for the first */
    uint32_t used;
    uint32_t pad[5];
} PoolBlk;

static uint8_t *s_pool;
static uint32_t s_pool_size;
static mutex_t s_pool_lock = LWP_MUTEX_NULL;
static uint32_t s_pool_used;

static PoolBlk *pool_next(PoolBlk *b) {
    uint8_t *n = (uint8_t *)b + b->size;
    return n < s_pool + s_pool_size ? (PoolBlk *)n : NULL;
}

static void pool_init(void) {
    s_pool = arena2_carve(POOL_BYTES);
    if (!s_pool) return;
    s_pool_size = POOL_BYTES;
    PoolBlk *b = (PoolBlk *)s_pool;
    b->size = s_pool_size;
    b->prev = 0;
    b->used = 0;
    LWP_MutexInit(&s_pool_lock, false);
}

void *wii_big_alloc(size_t n) {
    if (!s_pool || n == 0 || n > s_pool_size) return NULL;
    uint32_t need = (((uint32_t)n + 31u) & ~31u) + (uint32_t)sizeof(PoolBlk);
    void *out = NULL;
    LWP_MutexLock(s_pool_lock);
    for (PoolBlk *b = (PoolBlk *)s_pool; b; b = pool_next(b)) {
        if (b->used || b->size < need) continue;
        if (b->size - need >= 2 * sizeof(PoolBlk)) {   /* split off the rest */
            PoolBlk *r = (PoolBlk *)((uint8_t *)b + need);
            r->size = b->size - need;
            r->prev = need;
            r->used = 0;
            b->size = need;
            PoolBlk *after = pool_next(r);
            if (after) after->prev = r->size;
        }
        b->used = 1;
        s_pool_used += b->size;
        out = (uint8_t *)b + sizeof(PoolBlk);
        break;
    }
    LWP_MutexUnlock(s_pool_lock);
    return out;
}

/* Frees a pool block, or hands anything else to free(). */
void wii_big_free(void *p) {
    if (!p) return;
    uint8_t *q = p;
    if (!s_pool || q < s_pool || q >= s_pool + s_pool_size) {
        free(p);
        return;
    }
    LWP_MutexLock(s_pool_lock);
    PoolBlk *b = (PoolBlk *)(q - sizeof(PoolBlk));
    if (b->used) {
        b->used = 0;
        s_pool_used -= b->size;
        PoolBlk *n = pool_next(b);
        if (n && !n->used) {   /* merge with the block after */
            b->size += n->size;
            PoolBlk *after = pool_next(b);
            if (after) after->prev = b->size;
        }
        if (b->prev) {          /* merge into the block before */
            PoolBlk *pb = (PoolBlk *)((uint8_t *)b - b->prev);
            if (!pb->used) {
                pb->size += b->size;
                PoolBlk *after = pool_next(pb);
                if (after) after->prev = pb->size;
            }
        }
    }
    LWP_MutexUnlock(s_pool_lock);
}

uint32_t wii_big_used(void) { return s_pool_used; }

bool wii_memory_init(void) {
    printf("WII memory: arena2 %p..%p (%u MiB)\n", (void *)SYS_GetArena2Lo(),
           (void *)SYS_GetArena2Hi(),
           (unsigned)((uintptr_t)SYS_GetArena2Hi() - (uintptr_t)SYS_GetArena2Lo()) >> 20);
    printf("WII memory: arena1 %p..%p (%u KiB left in MEM1)\n", (void *)SYS_GetArena1Lo(), (void *)SYS_GetArena1Hi(),
           (unsigned)(((uintptr_t)SYS_GetArena1Hi() - (uintptr_t)SYS_GetArena1Lo()) >> 10));
    g_mem = arena2_carve(GUEST_RAM_BYTES);
    g_vram = arena2_carve(GUEST_VRAM_BYTES);
    g_low = arena2_carve(GUEST_LOW_BYTES);
#ifdef TKDR_WATCH
    g_wtp_ep = (uint32_t *)arena2_carve((GUEST_RAM_BYTES >> 8) * sizeof(uint32_t));
    g_wtp_ra = (uint32_t *)arena2_carve((GUEST_RAM_BYTES >> 8) * sizeof(uint32_t));
    if (!g_wtp_ep || !g_wtp_ra) return false;
#endif
    if (!g_mem || !g_vram || !g_low)
        return false;
    printf("WII memory: guest RAM %p (%u MiB), VRAM %p (2 MiB), low %p\n",
           g_mem, GUEST_RAM_BYTES >> 20, g_vram, g_low);
    pool_init();
    printf("WII memory: texture pool %p (%u MiB)\n", (void *)s_pool, s_pool ? POOL_BYTES >> 20 : 0);
    return true;
}
