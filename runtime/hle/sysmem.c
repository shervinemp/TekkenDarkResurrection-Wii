// User memory partition allocator and small kernel libraries
// (SysMemUserForUser, Kernel_Library, LoadExec, ModuleMgr, Utils, ...).
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "hle.h"
#include "../gpu/gpu.h"

// ---------------------------------------------------------------- allocator

typedef struct Block {
    uint32_t addr, size;
    int used;
    char name[32];
    struct Block *next;
} Block;

static Block *g_blocks;  // sorted by address, covers the whole free range

// Diagnostics: how much guest RAM is still free, and how often an
// allocation failed.  A game whose pool requests start failing spins in its
// own free-list walk, which looks like a hang.
uint64_t g_sysmem_failures, g_sysmem_bytes;
static uint32_t g_free_start, g_free_end;

void sysmem_init(uint32_t start, uint32_t end) {
    g_blocks = calloc(1, sizeof(Block));
    g_blocks->addr = start;
    g_blocks->size = end - start;
    g_free_start = start;
    g_free_end = end;
}

uint32_t sysmem_free_bytes(void) {
    uint32_t total = 0;
    for (Block *b = g_blocks; b; b = b->next)
        if (!b->used) total += b->size;
    return total;
}

void sysmem_debug_dump(void) {
    fprintf(stderr, "sysmem: %llu failures, %llu MiB free, window %08X-%08X\n",
            (unsigned long long)g_sysmem_failures,
            (unsigned long long)(sysmem_free_bytes() >> 20),
            g_free_start, g_free_end);
}

static int split(Block *b, uint32_t at) {
    Block *n = calloc(1, sizeof *n);
    if (!n) return -1;
    n->addr = at;
    n->size = b->addr + b->size - at;
    n->next = b->next;
    b->size = at - b->addr;
    b->next = n;
    return 0;
}

// type: 0 = lowest address, 1 = highest address, 2 = exact address
uint32_t sysmem_alloc(uint32_t size, int type, uint32_t addr, uint32_t align) {
    if (align < 0x100) align = 0x100;
    size = (size + 0xFF) & ~0xFFu;
    Block *pick = NULL;
    uint32_t at = 0;
    for (Block *b = g_blocks; b; b = b->next) {
        if (b->used) continue;
        uint32_t end = b->addr + b->size, a;
        if (type == 2) {
            if (addr < b->addr || addr + size > end) continue;
            a = addr;
        } else if (type == 1) {
            if (size > b->size) continue;
            a = (end - size) & ~(align - 1);
            if (a < b->addr) continue;
        } else {
            a = (b->addr + align - 1) & ~(align - 1);
            if (a + size > end) continue;
        }
        if (type != 1) {  // first fit from the bottom
            pick = b;
            at = a;
            break;
        }
        pick = b;  // keep going: we want the highest fit
        at = a;
    }
    if (!pick) {
        g_sysmem_failures++;
        fprintf(stderr, "sysmem: cannot allocate %x bytes (type %d); blocks:\n", size, type);
        for (Block *b = g_blocks; b; b = b->next)
            fprintf(stderr, "  %08X-%08X %s %s\n", b->addr, b->addr + b->size, b->used ? "used" : "FREE", b->name);
        return 0;
    }
    /* Splitting needs a small record from the heap; with none to spare the
       request fails the way an exhausted partition would. */
    if (at > pick->addr) {
        if (split(pick, at)) { g_sysmem_failures++; return 0; }
        pick = pick->next;
    }
    if (pick->size > size && split(pick, at + size)) {
        /* take the whole block rather than lose it */
        size = pick->size;
    }
    pick->used = 1;
    return at;
}

void sysmem_free(uint32_t addr) {
    for (Block *b = g_blocks; b; b = b->next)
        if (b->addr == addr && b->used) {
            b->used = 0;
            break;
        }
    for (Block *b = g_blocks; b && b->next;) {  // merge free neighbours
        if (!b->used && !b->next->used) {
            Block *n = b->next;
            b->size += n->size;
            b->next = n->next;
            free(n);
        } else {
            b = b->next;
        }
    }
}

// Partition blocks are referenced by uid; keep a tiny table.
#define MAX_BLOCK_UIDS 256
static uint32_t g_block_uid[MAX_BLOCK_UIDS];

void sceKernelAllocPartitionMemory(Ctx *c) {
    uint32_t addr = sysmem_alloc(ARG(3), (int)ARG(2), ARG(4), 0x100);
    HLOG("  alloc '%s' size=%x type=%u -> %08X\n", (char *)host(ARG(1)), ARG(3), ARG(2), addr);
    if (!addr) { RET(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    for (int k = 1; k < MAX_BLOCK_UIDS; k++)
        if (!g_block_uid[k]) {
            g_block_uid[k] = addr;
            RET(0x2000 + k);
            return;
        }
    RET(SCE_KERNEL_ERROR_NO_MEMORY);
}

static int block_slot(uint32_t uid) {
    int k = (int)uid - 0x2000;
    return k > 0 && k < MAX_BLOCK_UIDS && g_block_uid[k] ? k : 0;
}

// The guest asks for the head address of a memory block it allocated with
// sceKernelAllocPartitionMemory.  That is a guest address, and the guest
// dereferences it, so it has to be a real one.
void sceKernelGetBlockHeadAddr(Ctx *c) {
    int k = block_slot(ARG(0));
    RET(k ? g_block_uid[k] : 0);
}

void sceKernelFreePartitionMemory(Ctx *c) {
    int k = block_slot(ARG(0));
    if (!k) { RET(0x800200CCu); return; }
    sysmem_free(g_block_uid[k]);
    g_block_uid[k] = 0;
    RET(0);
}

void sceKernelSetCompiledSdkVersion(Ctx *c) { RET(0); }
void sceKernelSetCompilerVersion(Ctx *c) { RET(0); }
void sceKernelPrintf(Ctx *c) {
    fprintf(stderr, "[guest printf] %s", (char *)host(ARG(0)));
    RET(0);
}

// ---------------------------------------------------------------- interrupts

static uint32_t g_intr_enabled = 1;
void sceKernelCpuSuspendIntr(Ctx *c) {
    RET(g_intr_enabled);
    g_intr_enabled = 0;
}
void sceKernelCpuResumeIntr(Ctx *c) {
    g_intr_enabled = ARG(0);
    RET(0);
}

// ---------------------------------------------------------------- modules
// The game loads Sony's own libraries from USRDIR (sascore, atrac, mpeg...).
// Their exports are implemented here as HLE, so loading just succeeds.
//
// sceKernelGetBlockHeadAddr returns the address of the module's TOC (the
// export/import table).  The guest walks that table to find an imported
// function and then *calls* the address it finds there, so the value has to
// be a real PSP pointer to a real table -- returning a host address, or a
// fixed constant, makes the guest branch to whatever garbage it reads.
//
// We therefore keep one TOC per loaded module in guest RAM.  The TOC is
// zeroed, which the guest reads as "no exports here", and the block-head
// address is a valid guest pointer.

#define MAX_MODULES 16

static int g_next_module = 0x3001;

typedef struct {
    uint32_t id;      // module id handed back to the guest
    uint32_t toc;     // guest address of the TOC
} LoadedModule;

static LoadedModule g_modules[MAX_MODULES];
static int g_module_count;

static uint32_t module_toc_alloc(void) {
    // One page of guest RAM per module is plenty: the TOC is zeroed, so the
    // guest only ever reads zeroes from it.
    uint32_t addr = sysmem_alloc(0x1000, 0, 0, 0x1000);
    if (!addr) return 0;
    memset(host(addr), 0, 0x1000);
    return addr;
}

void sceKernelLoadModule(Ctx *c) {
    HLOG("  load module %s\n", (char *)host(ARG(0)));
    if (g_module_count >= MAX_MODULES) { RET(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    uint32_t toc = module_toc_alloc();
    if (!toc) { RET(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    LoadedModule *m = &g_modules[g_module_count++];
    m->id = g_next_module++;
    m->toc = toc;
    RET(m->id);
}
void sceKernelStartModule(Ctx *c) {
    if (ARG(3)) mem_w32(ARG(3), 0);  // module start status
    RET(0);                          // PSP returns 0 on success, not the UID
}
void sceKernelStopModule(Ctx *c) { RET(0); }
/* Unloading gives the module's slot and TOC page back.  The table used to
   only grow, so after 16 loads in total (6 at boot) every further load
   failed with "no memory", however many modules had been unloaded. */
void sceKernelUnloadModule(Ctx *c) {
    for (int k = 0; k < g_module_count; k++)
        if (g_modules[k].id == ARG(0)) {
            sysmem_free(g_modules[k].toc);
            g_modules[k] = g_modules[--g_module_count];
            break;
        }
    RET(0);
}
void sceKernelSelfStopUnloadModule(Ctx *c) { RET(0); }
void sceKernelGetModuleId(Ctx *c) { RET(0x3000); }

// The module id for an address: the id of the module that contains it, or
// the "no module" id when the address belongs to none of them.
void sceKernelGetModuleIdByAddress(Ctx *c) {
    uint32_t addr = ARG(0);
    for (int k = 0; k < g_module_count; k++) {
        uint32_t toc = g_modules[k].toc;
        if (addr >= toc && addr < toc + 0x1000) { RET(g_modules[k].id); return; }
    }
    RET(0x3000);
}

void sceKernelExitGame(Ctx *c) {
    (void)c;
    fprintf(stderr, "game called sceKernelExitGame\n");
    exit(0);
}
void sceKernelRegisterExitCallback(Ctx *c) { RET(0); }
void scePowerRegisterCallback(Ctx *c) { RET(0); }

// ---------------------------------------------------------------- stdio/utils

void sceKernelStdin(Ctx *c) { RET(0x10); }
void sceKernelStdout(Ctx *c) { RET(0x11); }
void sceKernelStderr(Ctx *c) { RET(0x12); }

void sceKernelDcacheWritebackAll(Ctx *c) { RET(0); }
void sceKernelDcacheWritebackRange(Ctx *c) { RET(0); }
void sceKernelDcacheWritebackInvalidateRange(Ctx *c) { RET(0); }
void sceKernelDcacheInvalidateRange(Ctx *c) { RET(0); }
void sceKernelSetGPO(Ctx *c) { RET(0); }
void sceKernelGetGPI(Ctx *c) { RET(0); }

void sceKernelLibcClock(Ctx *c) { RET((uint32_t)hle_time_us()); }
void sceKernelLibcTime(Ctx *c) {
    uint32_t t = (uint32_t)time(NULL);
#ifdef TKDR_FIXTIME
    t = 1700000000u;   /* repeatable guest random numbers for A/B screenshots */
#endif
    if (ARG(0)) mem_w32(ARG(0), t);
    RET(t);
}
void sceKernelLibcGettimeofday(Ctx *c) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
#ifdef TKDR_FIXTIME
    tv.tv_sec = 1700000000; tv.tv_usec = 0;
#endif
    if (ARG(0)) {
        mem_w32(ARG(0), (uint32_t)tv.tv_sec);
        mem_w32(ARG(0) + 4, (uint32_t)tv.tv_usec);
    }
    RET(0);
}

void sceKernelPowerTick(Ctx *c) { RET(0); }
void sceKernelPowerLock(Ctx *c) { RET(0); }
void sceKernelPowerUnlock(Ctx *c) { RET(0); }

// The 4MB "volatile" memory at 0x08400000 (normally used by the system)
static int g_volatile_locked;
void sceKernelVolatileMemTryLock(Ctx *c) {
    if (g_volatile_locked) { RET(0x802B0200u); return; }
    g_volatile_locked = 1;
    mem_w32(ARG(1), 0x08400000u);
    mem_w32(ARG(2), 0x00400000u);
    RET(0);
}
void sceKernelVolatileMemUnlock(Ctx *c) {
    g_volatile_locked = 0;
    RET(0);
}

void sceDmacMemcpy(Ctx *c) {
    /* Both pointers and the length come from the guest.  Clamp the length to
       the mapped window on each side, otherwise a single call can scribble
       over the host heap. */
    uint32_t n = ARG(2);
    uint32_t dst_span = guest_span(ARG(0), n);
    uint32_t src_span = guest_span(ARG(1), n);
    if (dst_span < n) n = dst_span;
    if (src_span < n) n = src_span;
    if (n) {
        memmove(host(ARG(0)), host(ARG(1)), n);
        /* DMA is how textures reach VRAM: the cached copies of whatever was
           overwritten must be checked again. */
        uint32_t dst = guest_norm(ARG(0));
        gpu_tex_invalidate_range(dst, dst + n);
    }
    RET(0);
}

void sceOpenPSIDGetOpenPSID(Ctx *c) {
    memset(host(ARG(0)), 0x42, 16);
    RET(0);
}
void sceWlanGetSwitchState(Ctx *c) { RET(0); }
