// Common helpers for HLE (high-level emulation) of PSP system libraries.
#pragma once
#include <stdio.h>
#include <stdio.h>

#include "../bigmem.h"
#include "../recomp.h"

#define ARG(n) (c->r[4 + (n)])
#define RET(v) (c->r[2] = (uint32_t)(v))
#define RET64(v) do { uint64_t r64_ = (uint64_t)(v); c->r[2] = (uint32_t)r64_; c->r[3] = (uint32_t)(r64_ >> 32); } while (0)

typedef void (*HleFunc)(Ctx *c);
typedef struct {
    const char *name;
    HleFunc fn;
} HleEntry;

// PSP kernel error codes used across modules
#define SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT 0x800200D2u
#define SCE_KERNEL_ERROR_NO_MEMORY 0x80020190u
#define SCE_KERNEL_ERROR_ILLEGAL_PRIORITY 0x80020193u
#define SCE_KERNEL_ERROR_UNKNOWN_THID 0x80020198u
#define SCE_KERNEL_ERROR_UNKNOWN_SEMID 0x80020199u
#define SCE_KERNEL_ERROR_UNKNOWN_EVFID 0x8002019Au
#define SCE_KERNEL_ERROR_UNKNOWN_FPLID 0x8002019Bu
#define SCE_KERNEL_ERROR_UNKNOWN_CBID 0x800201A1u
#define SCE_KERNEL_ERROR_NOT_DORMANT 0x800201A4u
#define SCE_KERNEL_ERROR_DORMANT 0x800201A2u
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT 0x800201A8u
#define SCE_KERNEL_ERROR_WAIT_CANCEL 0x800201A9u
#define SCE_KERNEL_ERROR_WAIT_DELETE 0x800201B5u
#define SCE_KERNEL_ERROR_SEMA_ZERO 0x800201ADu
#define SCE_KERNEL_ERROR_SEMA_OVF 0x800201AEu
#define SCE_KERNEL_ERROR_EVF_COND 0x800201AFu
#define SCE_KERNEL_ERROR_EVF_MULTI 0x800201B0u
#define SCE_KERNEL_ERROR_EVF_ILPAT 0x800201B1u
#define SCE_KERNEL_ERROR_NO_FREE 0x800201B2u  // fpl empty

extern int g_hle_log;  // log every HLE call
#define HLOG(...) do { if (g_hle_log) fprintf(stderr, __VA_ARGS__); } while (0)

/* High-water mark of the recompiled call depth (guest CALLs become C calls,
   so this also tracks host stack consumption). */
unsigned hle_max_call_depth(void);
void hle_call_depth_enter(void);
void hle_call_depth_leave(void);

/* Serialises host file I/O.  On the Wii the ISO and the save data live on
   the SD card, whose libfat driver drives the EXI hardware from an interrupt
   handler and is not reentrant. */
void storage_lock(void);
void storage_unlock(void);

uint64_t hle_time_us(void);  // microseconds since boot
uint32_t hle_vblank_count(void);  // the guest's vcount (see the vblank grid in thread.c)
void hle_count_call(void);   // attribute an HLE call to the calling thread
unsigned hle_current_thread_id(void);
int hle_guest_running(void);   // the current guest thread is executing guest code

// sysmem.c
uint32_t sysmem_alloc(uint32_t size, int type, uint32_t addr, uint32_t align);
void sysmem_free(uint32_t addr);
void sysmem_init(uint32_t free_start, uint32_t free_end);

// thread.c
void sched_boot(Ctx *boot_ctx);
void sched_check_events(void);  // timeouts/vblank; may switch threads
extern volatile int g_in_hle;
extern volatile unsigned g_hle_calls;
void guest_tick(void);
void sched_wake_idle(void);     // from host threads: an event a guest thread may wait for
Ctx *sched_current_ctx(void);
void hle_dump_recent_calls(void);  // debug: the ring of the last HLE calls
void hle_crash_report(FILE *f);    // debug: the same ring, to a crash file
void thread_delay(Ctx *c, uint64_t usec, int allow_cb);
void hle_wait_vblank(Ctx *c, int allow_cb);
