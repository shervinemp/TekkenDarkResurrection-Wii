// Routes calls to imported stubs to their HLE implementation.
#include <pthread.h>
#include <stdlib.h>
#if !defined(__EMSCRIPTEN__) && !defined(__wii__)
#include <execinfo.h>
#endif

#include "../func_table.h"
#include "hle.h"
#ifdef __wii__
#include "wii_platform.h"
#endif

extern const HleEntry g_hle_table[];
extern const unsigned g_hle_table_count;

int g_hle_log;
volatile int g_in_hle;
volatile unsigned g_hle_calls;
static HleFunc *g_resolved;
static uint8_t *g_warned;

#ifdef HLE_TRACE
uint64_t g_mem_accesses;  // guest memory accesses (diagnostics)
#endif

/* ---------------------------------------------------------------- call depth

   Every guest CALL becomes a C call in the recompiled code, so the depth of
   the call chain is also the amount of host stack the guest is consuming.
   Tracking the high-water mark turns a stack overflow into something the log
   can show, instead of a wild fault at an unrelated address. */
static unsigned g_call_depth, g_call_depth_max;

void hle_call_depth_enter(void) {
    if (++g_call_depth > g_call_depth_max) g_call_depth_max = g_call_depth;
}

void hle_call_depth_leave(void) {
    if (g_call_depth) g_call_depth--;
}

unsigned hle_max_call_depth(void) { return g_call_depth_max; }

/* ---------------------------------------------------------------- storage lock

   On the Wii the ISO and the save data live on the SD card, whose libfat
   driver drives the EXI hardware from an interrupt handler.  That driver is
   not reentrant: two host threads using it at once corrupt its state, and
   the next EXI interrupt dereferences a wild pointer and takes a DSI.  The
   guest threads, the frontend and the logger run on separate host threads,
   so all host file I/O is serialised through this one lock.

   The owner and depth make the lock re-entrant: the same thread may call
   storage_lock() again (for example a save write that logs while the log
   writer holds the lock) without deadlocking against itself.  The depth
   used to be a __thread counter.  On the Wii that was one word shared by
   every thread and by pthread_self()'s own storage (see pthread_lwp.c), so
   the mutex was hardly ever taken, and when it was it could be left held. */
static pthread_mutex_t g_storage_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile uintptr_t g_storage_owner;  // 0: nobody
static int g_storage_depth;                 // only touched by the owner

static uintptr_t storage_self(void) {
#ifdef __wii__
    return (uintptr_t)wii_thread_id();
#else
    return (uintptr_t)pthread_self();
#endif
}

void storage_lock(void) {
    uintptr_t self = storage_self();
    if (g_storage_owner == self) {  // only this thread can have written its own id
        g_storage_depth++;
        return;
    }
    pthread_mutex_lock(&g_storage_mutex);
    g_storage_owner = self;
    g_storage_depth = 1;
}

void storage_unlock(void) {
    if (--g_storage_depth == 0) {
        g_storage_owner = 0;
        pthread_mutex_unlock(&g_storage_mutex);
    }
}

static HleFunc resolve(const char *name) {
    for (unsigned k = 0; k < g_hle_table_count; k++)
        if (!strcmp(g_hle_table[k].name, name)) return g_hle_table[k].fn;
    return NULL;
}

void hle_init(void) {
    g_resolved = calloc(g_import_count, sizeof *g_resolved);
    g_warned = calloc(g_import_count, 1);
    unsigned done = 0;
    for (unsigned k = 0; k < g_import_count; k++)
        if ((g_resolved[k] = resolve(g_imports[k].name))) done++;
    fprintf(stderr, "HLE: %u/%u imports implemented\n", done, g_import_count);
}

#ifdef HLE_TRACE
/* Rolling window of the most recent HLE calls.  The first 300 calls are
   logged verbatim, but a crash can happen thousands of calls later; the ring
   is dumped periodically so the log always shows what the game was doing
   just before it died. */
#define HLE_RING 32
static struct { const char *name; uint32_t ra; uint32_t thread; } g_ring[HLE_RING];
static unsigned g_ring_pos;

static void hle_ring_dump(void) {
    fprintf(stderr, "--- HLE trace (last %u calls, depth max %u) ---\n",
            HLE_RING, hle_max_call_depth());
    for (unsigned k = 0; k < HLE_RING; k++) {
        unsigned i = (g_ring_pos + k) % HLE_RING;
        if (!g_ring[i].name) continue;
        fprintf(stderr, "  HLE %s ra=%08X thr=%u\n", g_ring[i].name, g_ring[i].ra, g_ring[i].thread);
    }
}
void hle_dump_recent_calls(void) { hle_ring_dump(); }
void hle_crash_report(FILE *f) {
    fprintf(f, "HLE ring:\n");
    for (unsigned k = 0; k < HLE_RING; k++) {
        unsigned i = (g_ring_pos + k) % HLE_RING;
        if (!g_ring[i].name) continue;
        fprintf(f, "  %s ra=%08X thr=%u\n", g_ring[i].name, g_ring[i].ra, g_ring[i].thread);
    }
}
#else
void hle_dump_recent_calls(void) { }
void hle_crash_report(FILE *f) { (void)f; }
#endif

#ifdef WII_HEARTBEAT
const char *volatile g_hb_hle;   /* the last HLE call the guest made, for the heartbeat */
volatile uint32_t g_hb_ra;
#endif

#ifdef TKDR_AUTOPILOT
/* Per-call time of every HLE function (raw time base ticks, waits included),
   for the release-speed demo build: hle_prof_report() prints the heaviest. */
#define PROF_MAX 1024
static uint64_t g_prof_ticks[PROF_MAX], g_prof_calls[PROF_MAX], g_prof_ticks0[PROF_MAX], g_prof_calls0[PROF_MAX];
static inline uint32_t prof_tb(void) { uint32_t t; __asm__ volatile("mftb %0" : "=r"(t)); return t; }
void hle_prof_report(uint32_t guest_frames) {
    if (!guest_frames) guest_frames = 1;
    for (int n = 0; n < 7; n++) {
        unsigned best = 0;
        uint64_t bt = 0;
        for (unsigned i = 0; i < g_import_count && i < PROF_MAX; i++) {
            uint64_t d = g_prof_ticks[i] - g_prof_ticks0[i];
            if (d > bt) { bt = d; best = i; }
        }
        if (!bt) break;
        uint64_t dc = g_prof_calls[best] - g_prof_calls0[best];
        fprintf(stderr, "  HLEPROF %-34s %6.1f calls/frame %6.2f ms/frame\n", g_imports[best].name,
                (double)dc / guest_frames, (double)bt / 60750.0 / guest_frames);
        g_prof_ticks0[best] = g_prof_ticks[best];   // consumed: the next pass finds the next one
        g_prof_calls0[best] = g_prof_calls[best];
    }
    for (unsigned i = 0; i < g_import_count && i < PROF_MAX; i++) {
        g_prof_ticks0[i] = g_prof_ticks[i];
        g_prof_calls0[i] = g_prof_calls[i];
    }
}

/* The three HLE calls that took the most time (waits included) since the last call of this function, as text
   in buf when want is set; the HITCH line of a long guest frame. */
void hle_prof_since(char *buf, unsigned n, int want) {
    static uint64_t mt[PROF_MAX], mc[PROF_MAX];
    unsigned top[3] = {0, 0, 0}, lim = g_import_count < PROF_MAX ? g_import_count : PROF_MAX;
    uint64_t tt[3] = {0, 0, 0};
    for (unsigned i = 0; i < lim; i++) {
        const uint64_t d = g_prof_ticks[i] - mt[i];
        if (want && d > tt[2]) {
            int k = d > tt[0] ? 0 : d > tt[1] ? 1 : 2;
            for (int j = 2; j > k; j--) { tt[j] = tt[j - 1]; top[j] = top[j - 1]; }
            tt[k] = d;
            top[k] = i;
        }
    }
    if (want && n) {
        unsigned used = 0;
        buf[0] = 0;
        for (int k = 0; k < 3 && tt[k]; k++)
            used += (unsigned)snprintf(buf + used, used < n ? n - used : 0, "%s%s %.0f ms (%llu)", k ? ", " : "", g_imports[top[k]].name,
                                       (double)tt[k] / 60750.0, (unsigned long long)(g_prof_calls[top[k]] - mc[top[k]]));
    }
    for (unsigned i = 0; i < lim; i++) {
        mt[i] = g_prof_ticks[i];
        mc[i] = g_prof_calls[i];
    }
}
#endif

void hle_call(Ctx *c, uint32_t idx) {
    /* The index comes from the recompiled code.  A stale or corrupt index
       would read past g_imports[]/g_resolved[] and hand back a garbage
       function pointer, which the caller then branches to. */
    if (idx >= g_import_count) {
        fprintf(stderr, "HLE: bad import index %u (count %u) ra=%08X\n",
                idx, g_import_count, c->r[31]);
        c->r[2] = 0;
        return;
    }
    const ImportEntry *e = &g_imports[idx];
    HleFunc fn = g_resolved[idx];
    unsigned call = ++g_hle_calls;
    hle_count_call();
    if (call <= 300)
        fprintf(stderr, "HLE[%u]: %s ra=%08X\n", call, e->name, c->r[31]);
#ifdef HLE_TRACE
    g_ring[g_ring_pos].name = e->name;
    g_ring[g_ring_pos].ra = c->r[31];
    g_ring[g_ring_pos].thread = hle_current_thread_id();
    g_ring_pos = (g_ring_pos + 1) % HLE_RING;
    /* Dumping on every call floods the emulator's EXI/OSReport path, which
       slows the run down and perturbs the very SD traffic under test. */
    if (call % 5000 == 0) hle_ring_dump();
#endif
    HLOG("%s(%08X, %08X, %08X, %08X) ra=%08X\n", e->name, c->r[4], c->r[5], c->r[6], c->r[7],
         c->r[31]);
#if !defined(__EMSCRIPTEN__) && !defined(__wii__)
    // HLE_BT=name: print the guest call chain (host frames are f_<address>)
    static const char *bt_name;
    static int bt_init;
    if (!bt_init) { bt_init = 1; bt_name = getenv("HLE_BT"); }
    if (bt_name && !strcmp(bt_name, e->name)) {
        void *bt[64];
        int n = backtrace(bt, 64);
        fprintf(stderr, "backtrace at %s:\n", e->name);
        backtrace_symbols_fd(bt, n, 2);
    }
#endif
#ifdef WII_HEARTBEAT
    g_hb_hle = e->name;
    g_hb_ra = c->r[31];
#endif
    if (fn) {
        g_in_hle = 1;
#ifdef TKDR_AUTOPILOT
        uint32_t p0 = prof_tb();
        fn(c);
        g_prof_ticks[idx] += (uint32_t)(prof_tb() - p0);
        g_prof_calls[idx]++;
#else
        fn(c);
#endif
        g_in_hle = 0;
    } else {
        if (!g_warned[idx]) {
            g_warned[idx] = 1;
            fprintf(stderr, "HLE: unimplemented %s::%s (returning 0) ra=%08X\n", e->lib, e->name,
                    c->r[31]);
        }
        c->r[2] = 0;
    }
    if (g_hle_log && fn) HLOG("  -> %08X\n", c->r[2]);
    sched_check_events();
}

// Call back into guest code from an HLE function, on the current thread
// (below its stack pointer). The caller's registers are preserved; v0 is
// left holding the guest function's return value.
void hle_call_guest(Ctx *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    Ctx saved = *c;
    c->r[4] = a0;
    c->r[5] = a1;
    c->r[6] = a2;
    c->r[29] = (c->r[29] - 0x100) & ~15u;
    c->r[31] = 0;
    call_indirect(c, fn);
    uint32_t v0 = c->r[2];
    *c = saved;
    c->r[2] = v0;
}
