// PSP kernel threads and synchronisation (ThreadManForUser).
//
// Every PSP thread runs on its own host pthread, but only the thread holding
// the "baton" (g_cur) executes guest code: this reproduces the PSP's single
// core, priority-based, non-time-sliced scheduler. The big lock is held by
// whoever runs guest code; everyone else sleeps on their own condvar.
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __wii__
#include "wii_platform.h"
#endif

#include "../func_table.h"
#include "../gpu/gpu.h"
#include "hle.h"

enum { ST_RUNNING = 1, ST_READY = 2, ST_WAITING = 4, ST_DORMANT = 16, ST_DEAD = 32 };
enum {
    W_NONE, W_SLEEP, W_DELAY, W_THREAD_END, W_SEMA, W_EVENT, W_VBLANK, W_IO, W_GPU,
};
enum { OBJ_THREAD = 1, OBJ_SEMA, OBJ_EVENT, OBJ_FPL, OBJ_CALLBACK };

#define MAX_OBJ 1024
#define OBJ_ID_BASE 0x1000

typedef struct Thread Thread;

typedef struct {
    int type;
    char name[32];
    uint32_t attr;
} ObjHeader;

struct Thread {
    ObjHeader h;
    int id;
    uint32_t entry, init_prio, prio, stack_size, stack, gp;
    uint32_t k0;  // PSP thread context; libc's __getreent() keeps its reent at [k0+4]
    int status;
    uint64_t seq;  // FIFO order among equal priorities
    Ctx ctx;
    pthread_t host;
    pthread_cond_t cv;
    int host_alive, killed;
    // wait state
    int wait_type, wait_id;
    uint64_t deadline;  // 0 = none
    uint32_t timeout_ptr;
    uint32_t wait_a, wait_b, wait_c;  // per-wait parameters
    int32_t wait_result;
    int wakeup_count;
    uint32_t exit_status;
    uint32_t arglen, argp;
    uint32_t hle_calls;  // diagnostics: how busy this thread is
};

typedef struct {
    ObjHeader h;
    int32_t count, max;
} Sema;

typedef struct {
    ObjHeader h;
    uint32_t init, pattern;
} EventFlag;

typedef struct {
    ObjHeader h;
    uint32_t base, block_size, nblocks;
    uint8_t *used;
} Fpl;

typedef struct {
    ObjHeader h;
    uint32_t func, arg;
    int owner;
} Callback;

static void *g_obj[MAX_OBJ];
static pthread_mutex_t g_big = PTHREAD_MUTEX_INITIALIZER;
static Thread *g_cur;
static uint64_t g_seq;
/* Only one guest thread executes at a time (the baton), so the current
   thread pointer is process-global rather than TLS.  It is refreshed on every
   reschedule; host-thread entry points also set it. */
static Thread *tls_self;
static pthread_cond_t g_all_done = PTHREAD_COND_INITIALIZER;
static uint64_t g_boot_ns;
uint64_t g_idle_us;  // time with no runnable guest thread (stats)

// TIME_SCALE=n makes guest time run n times faster than real time (testing).
static double g_time_scale = 1.0;

static double g_until_us;   // TIME_SCALE_UNTIL=seconds: back to real time after that
static int g_time_scaled;   // the scaling path is only used when it is asked for

uint64_t hle_time_us(void) {
#ifdef __wii__
    /* The clock is in microseconds already, so the usual case needs no
       64-bit division (a long libgcc routine on this CPU). */
    static uint64_t boot_us;
    uint64_t now_us = wii_time_us();
    if (boot_us && !g_time_scaled) return now_us - boot_us;
    uint64_t ns = now_us * 1000u;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
    if (!g_boot_ns) {
        g_boot_ns = ns;
#ifdef __wii__
        boot_us = now_us;
#endif
        if (getenv("TIME_SCALE")) { g_time_scale = atof(getenv("TIME_SCALE")); g_time_scaled = 1; }
        if (getenv("TIME_SCALE_UNTIL")) {
            g_until_us = atof(getenv("TIME_SCALE_UNTIL")) * 1e6;
            g_time_scaled = 1;
        }
    }
    /* This function is called several times per primitive, so the normal
       case has to be cheap.  Double-precision arithmetic is not: this CPU
       has no double FPU, so every double operation becomes a call into
       libgcc's soft-float helpers, which are long integer routines. */
    if (!g_time_scaled) return (ns - g_boot_ns) / 1000u;

    double real_us = (double)(ns - g_boot_ns) / 1000.0, t = real_us * g_time_scale;
    if (g_until_us > 0 && t > g_until_us) {
        t = g_until_us + (real_us - g_until_us / g_time_scale);
        g_time_scale = 1.0;
        g_boot_ns = ns - (uint64_t)(t * 1000.0);  // keep the clock continuous
        g_until_us = 0;
    }
    return (uint64_t)t;
}

#define VBLANK_US 16683u
/* The vblank grid: ticks are at g_vb_base + n * VBLANK_US.  The game's frame loop waits for the next
   tick, so a frame that takes 17-33 ms is held to 33 ms (half speed) however little it overran.
   A wait that starts after a tick has gone by with nobody waiting (the frame is late) returns at
   once and moves the grid to now, so the frame rate is 1 / frame time down to 60 Hz instead of
   60, 30, 20: the game's speed follows the frame time (22 ms is 76 %) where the grid held it to
   a steady half speed.  -DTKDR_FIXED_VBLANK keeps the fixed grid. */
static uint64_t g_vb_base;
static uint64_t g_vb_served;   /* the tick count the last vblank wait was let go at */
static uint64_t vblank_count(void) { return (hle_time_us() - g_vb_base) / VBLANK_US; }
uint32_t hle_vblank_count(void) { return (uint32_t)vblank_count(); }

// ---------------------------------------------------------------- objects

static int obj_new(void *o, int type, uint32_t name_ptr, uint32_t attr) {
    for (int k = 1; k < MAX_OBJ; k++)
        if (!g_obj[k]) {
            ObjHeader *h = o;
            h->type = type;
            h->attr = attr;
            if (name_ptr) {
                strncpy(h->name, (const char *)host(name_ptr), 31);
                h->name[31] = 0;
            }
            g_obj[k] = o;
            return OBJ_ID_BASE + k;
        }
    return -1;
}

static void *obj_get(int id, int type) {
    int k = id - OBJ_ID_BASE;
    if (k <= 0 || k >= MAX_OBJ || !g_obj[k]) return NULL;
    return ((ObjHeader *)g_obj[k])->type == type ? g_obj[k] : NULL;
}

static void obj_del(int id) { g_obj[id - OBJ_ID_BASE] = NULL; }

#define FOR_EACH_THREAD(t)                                                          \
    for (int k_ = 1; k_ < MAX_OBJ; k_++)                                            \
        for (Thread *t = g_obj[k_] && ((ObjHeader *)g_obj[k_])->type == OBJ_THREAD \
                             ? g_obj[k_] : NULL; t; t = NULL)

#if defined(HLE_TRACE) || defined(TKDR_AUTOPILOT)
// Dump what every guest thread is doing.  When the game sits on a screen
// without progressing, this shows which thread is blocked and on what.
static const char *wait_name(int w) {
    switch (w) {
    case W_NONE: return "none";
    case W_SLEEP: return "sleep";
    case W_DELAY: return "delay";
    case W_THREAD_END: return "thread_end";
    case W_SEMA: return "sema";
    case W_EVENT: return "event";
    case W_VBLANK: return "vblank";
    case W_IO: return "io";
    case W_GPU: return "gpu";
    default: return "?";
    }
}

void sched_dump_threads(const char *tag) {
    fprintf(stderr, "--- threads (%s) ---\n", tag);
    FOR_EACH_THREAD(t) {
        fprintf(stderr,
                "  thread %d '%s' entry=%08X status=%d wait=%s id=%d a=%08X b=%08X hle=%u pc=%08X\n",
                t->id, t->h.name, t->entry, t->status, wait_name(t->wait_type),
                t->wait_id, t->wait_a, t->wait_b, t->hle_calls, t->ctx.r[31]);
        if (t->host && !wii_pthread_stack_intact(t->host))
            fprintf(stderr, "  *** thread %d host stack canary smashed ***\n", t->id);
    }
}
#endif

// Count an HLE call against the calling guest thread (diagnostics).
void hle_count_call(void) {
    if (tls_self) tls_self->hle_calls++;
}

unsigned hle_current_thread_id(void) { return tls_self ? (unsigned)tls_self->id : 0; }

/* Used by the debug PC sampler.  A frontend sample must not charge the time a
   guest thread spends blocked inside an HLE wait (vblank, file I/O, GPU sync)
   to the guest function that happens to be on top of its stack. */
int hle_guest_running(void) {
    return g_cur && g_cur->status == ST_RUNNING && !g_in_hle;
}

// ---------------------------------------------------------------- scheduler

static void make_ready(Thread *t, int32_t result) {
    t->status = ST_READY;
    t->wait_type = W_NONE;
    t->wait_result = result;
    t->seq = ++g_seq;
    if (t->timeout_ptr) {
        uint64_t now = hle_time_us();
        uint32_t left = t->deadline > now ? (uint32_t)(t->deadline - now) : 0;
        mem_w32(t->timeout_ptr, result == (int32_t)SCE_KERNEL_ERROR_WAIT_TIMEOUT ? 0 : left);
        t->timeout_ptr = 0;
    }
    t->deadline = 0;
}

static Thread *pick_next(void) {
    Thread *best = NULL;
    FOR_EACH_THREAD(t) {
        if (t->status != ST_READY && t->status != ST_RUNNING) continue;
        if (!best || t->prio < best->prio || (t->prio == best->prio && t->seq < best->seq)) best = t;
    }
    return best;
}

static void wake_waiters(int wait_type, int id, int32_t result);
static void events_locked(void);

static void *host_thread_main(void *arg);

static int g_boot_thread_id;

/* Starts the host thread for a guest thread.  Returns 0 on success.
 *
 * A failure here is not fatal: the guest asked for a thread and cannot
 * have one, so that thread fails and the game carries on.  Killing the
 * whole emulator over it (as this used to) meant the game simply stopped. */
static int start_host(Thread *t) {
    pthread_attr_t a;
    pthread_attr_init(&a);
#ifdef __EMSCRIPTEN__
    pthread_attr_setstacksize(&a, 1u << 20);  // lives in wasm linear memory
#elif defined(__wii__)
    /* Guest stacks are emulated in PSP RAM; this only has to hold the host
       call chain of one recompiled function tree.  Every guest CALL is a C
       call, so the chain can be deep.  Giving every guest thread a full
       megabyte exhausted MEM1 when the movie player started its three
       workers: two of them could not get a host thread, and the game later
       jumped through a callback they were supposed to provide.  Scale the
       host stack with the guest's own stack (the main game threads have the
       large PSP stacks), with a floor for the shallow worker threads. */
    uint32_t hs;
    if (t->id == g_boot_thread_id) {
        /* The main game loop and the menu transition have the deepest
           recompiled call chain.  -DEXP_STACKPROBE measured the deepest use
           of this stack at about 5 KiB over menus, movies and fights; 512 KiB
           leaves a hundredfold margin and still fits beside the movie's three
           worker threads.  A 4 MiB request used to fail outright and fall
           back to 1 MiB, so it bought nothing but a fragmented heap. */
        hs = 512u << 10;
    } else {
        /* Worker and callback threads: -DEXP_STACKPROBE measured at most 40 KiB
           (the movie decoder workers); 256 KiB is six times that.  Scaling the
           host stack with the guest's own (up to 2 MiB) only reserved memory
           that was never touched. */
        hs = 256u << 10;
    }
    pthread_attr_setstacksize(&a, hs);
#else
    pthread_attr_setstacksize(&a, 4u << 20);
#endif
    t->host_alive = 1;
    t->killed = 0;
    if (pthread_create(&t->host, &a, host_thread_main, t) != 0) {
        fprintf(stderr, "HLE: cannot start host thread for guest thread %x\n", t->id);
        t->host_alive = 0;
        t->status = ST_DEAD;
        t->exit_status = 0x800201ACu;  // SCE_KERNEL_ERROR_NO_MEMORY
        pthread_attr_destroy(&a);
        return -1;
    }
    pthread_detach(t->host);
    pthread_attr_destroy(&a);
    return 0;
}

/* True when the calling host thread no longer owns `t`.  A guest thread that
   is terminated while it waits and then started again gets a new host
   thread; the old one wakes up later on the same condition variable.  If it
   took the baton (g_cur is the thread again) it would resume the old C call
   chain over the new context, and two host threads would run one guest
   thread.  The leftover has to leave without touching the thread's state. */
static int host_replaced(const Thread *t) { return pthread_self() != t->host; }

// Guest interrupt handlers (GE callbacks) run inside HLE calls; no thread
// switch may happen until they return.
static int g_in_intr;
void hle_intr_enter(void) { g_in_intr++; }
void hle_intr_leave(void) { g_in_intr--; }

// Idling until guest time `wake`. Timed waits can overshoot by a lot on some hosts (Windows rounds them
// to its timer tick, up to 15.6 ms: a missed vblank each time, the game in slow motion), so the overshoot
// is measured (its 90th percentile), the wait ends that much early and the rest is spent polling with the lock released.
// Where timed waits are precise (macOS, Linux) the margin stays near zero. Events from other host
// threads (a GE list done) cut the wait short through sched_wake_idle().
static Thread *g_idler;
static int g_wake_flag;        // set by sched_wake_idle(), read without g_big while polling
static double g_overshoot_us;  // 90th percentile of the last timed waits' overshoot
static uint32_t g_over_ring[32], g_over_n;

static void note_overshoot(uint32_t us) {
    g_over_ring[g_over_n++ % 32] = us < 20000 ? us : 20000;
    uint32_t s[32];
    int n = g_over_n < 32 ? (int)g_over_n : 32;
    memcpy(s, g_over_ring, sizeof s);
    for (int i = 1; i < n; i++)  // insertion sort, 32 entries
        for (int j = i; j > 0 && s[j - 1] > s[j]; j--) {
            uint32_t t = s[j];
            s[j] = s[j - 1];
            s[j - 1] = t;
        }
    g_overshoot_us = s[(n * 9) / 10 < n ? (n * 9) / 10 : n - 1];  // occasional spikes don't count
}

static void idle_until(Thread *self, uint64_t wake, uint64_t now) {
    if (__atomic_exchange_n(&g_wake_flag, 0, __ATOMIC_ACQ_REL)) return;
    double margin = g_overshoot_us + 300.0;
    if ((double)(wake - now) <= margin) {  // poll the clock without the lock, so other threads get in
        pthread_mutex_unlock(&g_big);
#ifdef __wii__
        while (hle_time_us() < wake && !__atomic_load_n(&g_wake_flag, __ATOMIC_ACQUIRE)) wii_yield();
#else
        while (hle_time_us() < wake && !__atomic_load_n(&g_wake_flag, __ATOMIC_ACQUIRE)) sched_yield();
#endif
        __atomic_store_n(&g_wake_flag, 0, __ATOMIC_RELEASE);
        pthread_mutex_lock(&g_big);
        return;
    }
    uint64_t target = wake - (uint64_t)margin;
    uint64_t wns = (uint64_t)((double)(target - now) * 1000.0 / g_time_scale);
    static long tick_ns = -1;  // TIMER_TICK_US=15600: round timed waits up like Windows does (testing)
    if (tick_ns < 0) tick_ns = getenv("TIMER_TICK_US") ? atol(getenv("TIMER_TICK_US")) * 1000 : 0;
    if (tick_ns > 0) wns = (wns + (uint64_t)tick_ns - 1) / (uint64_t)tick_ns * (uint64_t)tick_ns;
    g_idler = self;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ns = (uint64_t)ts.tv_nsec + wns;
    ts.tv_sec += (time_t)(ns / 1000000000u);
    ts.tv_nsec = (long)(ns % 1000000000u);
    int r = pthread_cond_timedwait(&self->cv, &g_big, &ts);
    g_idler = NULL;
    if (r == ETIMEDOUT) {
        uint64_t late = hle_time_us();
        note_overshoot(late > target ? (uint32_t)(late - target) : 0);
    }
}

// From another host thread (not holding g_big): something a waiting guest thread may care about happened.
//
// This deliberately does not take g_big.  The GPU thread calls this while a
// guest thread can be inside an HLE call holding g_big and waiting for the GPU
// list; taking g_big here deadlocks both sides.  The wake flag is atomic and
// the condition variable is signalled without the mutex; idle_until() also
// polls the flag, so a lost signal can only cost one timed-wait slice.
void sched_wake_idle(void) {
    __atomic_store_n(&g_wake_flag, 1, __ATOMIC_RELEASE);
    Thread *idler = __atomic_load_n(&g_idler, __ATOMIC_ACQUIRE);
    if (idler) pthread_cond_broadcast(&idler->cv);
}

// Give the baton to the best ready thread and block until it comes back.
// Called with g_big held by `self` (which may be waiting, dormant or dead).
#ifdef TKDR_AUTOPILOT
uint32_t g_rs_calls, g_rs_switches, g_rs_ticks_calls;   /* demo statistics: scheduler activity */
#endif
static void reschedule(Thread *self) {
    if (g_in_intr) return;
#ifdef TKDR_AUTOPILOT
    g_rs_calls++;
#endif
    for (;;) {
        /* Only one guest thread executes at a time, but host threads take
           turns changing this global while another is asleep.  Restore it
           whenever this thread gets the baton back. */
        tls_self = self;
        gpu_run_interrupts(&self->ctx);  // GE finish/signal callbacks
        events_locked();
        Thread *next = pick_next();
        if (next) {
            if (next == self) {
                self->status = ST_RUNNING;
                g_cur = self;
                return;
            }
#ifdef TKDR_AUTOPILOT
            g_rs_switches++;
#endif
            if (g_cur == self) g_cur = next;
            if (self->status == ST_RUNNING) self->status = ST_READY;
            next->status = ST_RUNNING;
            g_cur = next;
            if (!next->host_alive && start_host(next) != 0) {
                /* The thread could not get a host thread and is already
                   marked dead.  The hand over never happened, so choose
                   again instead of waiting for a baton nobody will pass. */
                continue;
            }
            /* Broadcast: a host thread left over from a terminated run of
               `next` may share the condition variable, and a signal could
               wake it instead of the thread that owns the baton now. */
            pthread_cond_broadcast(&next->cv);
            if (self->status == ST_DORMANT || self->status == ST_DEAD) return;  // exiting
            while (g_cur != self && !self->killed && !host_replaced(self))
                pthread_cond_wait(&self->cv, &g_big);
            if (self->killed || host_replaced(self)) {
                if (!host_replaced(self)) self->host_alive = 0;
                pthread_mutex_unlock(&g_big);
                pthread_exit(NULL);
            }
            continue;  // we hold the baton again; re-check events
        }
        // nobody can run: idle until the next deadline or vblank
        int any = 0;
        uint64_t now = hle_time_us(), wake = g_vb_base + (vblank_count() + 1) * VBLANK_US;
        FOR_EACH_THREAD(t) {
            if (t->status == ST_WAITING) any = 1;
            if (t->status == ST_WAITING && t->deadline && t->deadline < wake) wake = t->deadline;
            if (t->status == ST_WAITING && t->wait_type == W_GPU && now + 4000 < wake) wake = now + 4000;
        }
        // the GPU thread wakes us when a list is done (sched_wake_idle); this is only a safety net
        if (gpu_busy(-1) && now + 4000 < wake) wake = now + 4000;
        if (!any) {
            fprintf(stderr, "all threads finished or deadlocked\n");
            pthread_cond_broadcast(&g_all_done);
            exit(0);
        }
        if (wake > now) {
            g_idle_us += wake - now;  // approximate (stats)
            idle_until(self, wake, now);
        }
    }
}

static uint64_t g_last_vblank;

#ifdef HLE_TRACE
// Timing of the scheduler's waits (diagnostics).
static uint64_t g_vblank_waits, g_vblank_total, g_vblank_max;
static uint64_t g_wait_total, g_wait_count;

void sched_dump_waits(void) {
    fprintf(stderr, "--- waits: vblank n=%llu avg=%.0fus max=%lluus | all n=%llu avg=%.0fus "
            "| overshoot=%.0fus ---\n",
            (unsigned long long)g_vblank_waits,
            g_vblank_waits ? (double)g_vblank_total / (double)g_vblank_waits : 0.0,
            (unsigned long long)g_vblank_max,
            (unsigned long long)g_wait_count,
            g_wait_count ? (double)g_wait_total / (double)g_wait_count : 0.0,
            g_overshoot_us);
}
#endif

static void events_locked(void) {
    uint64_t now = hle_time_us(), vb = vblank_count();
    if (vb != g_last_vblank) {
        g_last_vblank = vb;
        FOR_EACH_THREAD(t) {
            if (t->status == ST_WAITING && t->wait_type == W_VBLANK) g_vb_served = vb;
        }
        wake_waiters(W_VBLANK, 0, 0);
    }
    FOR_EACH_THREAD(t) {
        if (t->status == ST_WAITING && t->wait_type == W_GPU && !gpu_busy(t->wait_id)) make_ready(t, 0);
    }
    FOR_EACH_THREAD(t) {
        if (t->status != ST_WAITING || !t->deadline || t->deadline > now) continue;
        if (t->wait_type == W_DELAY) make_ready(t, 0);
        else make_ready(t, (int32_t)SCE_KERNEL_ERROR_WAIT_TIMEOUT);
    }
}

// Block the calling thread; returns the wait result.
static int32_t wait_on(Ctx *c, int type, int id, uint32_t timeout_ptr) {
    Thread *self = tls_self;
    (void)c;
    uint64_t t0 = hle_time_us();
    self->status = ST_WAITING;
    self->wait_type = type;
    self->wait_id = id;
    self->timeout_ptr = 0;
    self->deadline = 0;
    if (timeout_ptr) {
        /* A zero timeout is a poll.  The movie player's playing state passes
           a zero timeout while the decode threads are still running and
           expects the wait to return so its own loop can continue. */
        self->timeout_ptr = timeout_ptr;
        self->deadline = hle_time_us() + mem_r32(timeout_ptr);
    }
    reschedule(self);
#ifdef HLE_TRACE
    {
        uint64_t dt = hle_time_us() - t0;
        if (type == W_VBLANK) {
            g_vblank_waits++;
            g_vblank_total += dt;
            if (dt > g_vblank_max) g_vblank_max = dt;
        }
        g_wait_total += dt;
        g_wait_count++;
    }
#endif
    return self->wait_result;
}

// After waking someone up, let a higher-priority thread run right away.
static void yield_if_needed(void) {
    Thread *self = tls_self;
    reschedule(self);
}

/* Give a ready thread of the same priority a turn.  A guest thread that runs
   a long stretch of guest code without blocking (the movie decoder, which
   spends ~20 ms per frame inside FFmpeg) otherwise keeps the CPU until it
   blocks, and a peer that only wakes once per vblank (the movie's ringbuffer
   feeder) can be starved for seconds: its 700-packet ring drains, and the
   game's own player takes an empty ring for the end of the stream.  The PSP
   kernel time-slices equal priorities, so this is also the faithful model. */
void hle_yield_peer(void) {
    Thread *self = tls_self;
    if (!self || g_in_intr || g_cur != self || self->status != ST_RUNNING) return;
    self->status = ST_READY;
    self->seq = ++g_seq;   /* the peers that are already ready go first */
    reschedule(self);
}

void sched_check_events(void) {
    if (tls_self) reschedule(tls_self);
#ifdef __wii__
    /* Guest host threads and the frontend are cooperative KThreads at the
       same priority.  Yield periodically so scanout, input and audio run
       while the guest is executing a long stream of HLE calls. */
    static unsigned yields;
    if (!g_in_intr && (++yields & 7u) == 0)
        wii_yield();
#endif
}

static void stack_guard(void);
static void exit_current(uint32_t status, int del);

// Called from guest memory accesses on Wii.  Generated guest code can run for
// a long time without an HLE call; this is its cooperative scheduling point.
// Only the host thread that owns the current guest Thread may reschedule, so
// the frontend/GPU/audio host threads are never mistaken for guest threads.
#ifdef TKDR_AUTOPILOT
/* A sampling profile of the generated code: every time the guest memory
   counter rolls over (2048 accesses) the time since the last one is charged to
   the generated function that made the call.  addr2line on the ELF names it. */
#define GP_N 4096
static struct { uint32_t addr, addr1; uint32_t n; uint64_t ticks; uint32_t ft, fgen; } g_gp[GP_N];   /* ft: ticks of the guest frame fgen */
static uint64_t g_gp_total;
static uint32_t g_gp_gen = 1;
static inline void guest_prof_sample(uint32_t ra, uint32_t ra1) {
    static uint32_t prev;
    uint32_t now, dt;
    __asm__ volatile("mftb %0" : "=r"(now));
    dt = now - prev;
    prev = now;
    if (dt > 120000u) return;   /* a thread switch or an HLE call: not this function's time */
    uint32_t h = ((ra >> 2) ^ (ra1 >> 4)) * 2654435761u >> 20;
    for (int probe = 0; probe < 8; probe++, h = (h + 1) & (GP_N - 1)) {
        if ((g_gp[h].addr == ra && g_gp[h].addr1 == ra1) || !g_gp[h].addr) {
            g_gp[h].addr = ra;
            g_gp[h].addr1 = ra1;
            g_gp[h].n++;
            g_gp[h].ticks += dt;
            if (g_gp[h].fgen != g_gp_gen) { g_gp[h].fgen = g_gp_gen; g_gp[h].ft = 0; }
            g_gp[h].ft += dt;
            g_gp_total += dt;
            return;
        }
    }
    g_gp_total += dt;
}
uint64_t guest_prof_total_ticks(void) { return g_gp_total; }

/* Closes a guest frame for the HITCH line: the four generated functions (return address and its caller) that ran longest
   during it, as text in buf when want is set. */
void guest_prof_frame(char *buf, unsigned n, int want) {
    if (want && n) {
        int top[4] = {-1, -1, -1, -1};
        unsigned used = 0;
        for (int i = 0; i < GP_N; i++) {
            if (g_gp[i].fgen != g_gp_gen || !g_gp[i].ft) continue;
            int k = 0;
            while (k < 4 && top[k] >= 0 && g_gp[top[k]].ft >= g_gp[i].ft) k++;
            if (k == 4) continue;
            for (int j = 3; j > k; j--) top[j] = top[j - 1];
            top[k] = i;
        }
        buf[0] = 0;
        for (int k = 0; k < 4 && top[k] >= 0; k++)
            used += (unsigned)snprintf(buf + used, used < n ? n - used : 0, "%s%08X<%08X %.0f ms", k ? ", " : "", g_gp[top[k]].addr,
                                       g_gp[top[k]].addr1, (double)g_gp[top[k]].ft / 60750.0);
    }
    g_gp_gen++;
}
void guest_prof_report(uint32_t gf) {
#ifdef WII_SDLOG
    for (int k = 0; k < 16; k++) {   /* the SD log is not the place for a function list */
#else
    for (int k = 0; k < 700; k++) {
#endif
        int best = -1;
        for (int i = 0; i < GP_N; i++)
            if (g_gp[i].addr && (best < 0 || g_gp[i].ticks > g_gp[best].ticks)) best = i;
        if (best < 0 || !g_gp[best].ticks) break;
        fprintf(stderr, "    GUESTPROF %08X %08X %5u samples %.2f ms/frame\n", g_gp[best].addr, g_gp[best].addr1, g_gp[best].n,
                gf ? (double)g_gp[best].ticks / 60750.0 / gf : 0.0);
        g_gp[best].ticks = 0;
    }
    fprintf(stderr, "    GUESTPROF total sampled %.2f ms/frame\n", gf ? (double)g_gp_total / 60750.0 / gf : 0.0);
    memset(g_gp, 0, sizeof g_gp);
    g_gp_total = 0;
}
#endif

void guest_tick(void) {
#ifdef TKDR_AUTOPILOT
    guest_prof_sample((uint32_t)(uintptr_t)__builtin_return_address(0), (uint32_t)(uintptr_t)__builtin_return_address(1));
#endif
#ifdef __wii__
    static unsigned ticks;
    if ((++ticks & 0x7FFu) != 0) return;
#ifdef TKDR_AUTOPILOT
    g_rs_ticks_calls++;
#endif
    wii_yield();
    if (!tls_self || tls_self->status != ST_RUNNING) return;
    if (pthread_self() != tls_self->host) return;
    stack_guard();
#ifdef HLE_TRACE
    { extern void hle_sample_guest_pc(uint32_t pc); hle_sample_guest_pc(tls_self->ctx.r[31]); }
#endif
    if (g_in_hle || g_in_intr) return;
    sched_check_events();
#endif
}



Ctx *sched_current_ctx(void) { return tls_self ? &tls_self->ctx : NULL; }

static void thread_finish(Thread *t, uint32_t status, int del) {
    t->exit_status = status;
    t->status = ST_DORMANT;
    FOR_EACH_THREAD(w) {
        if (w->status == ST_WAITING && w->wait_type == W_THREAD_END && w->wait_id == t->id)
            make_ready(w, 0);  // the wait succeeded; the status stays in t->exit_status
    }
    if (del) {
        t->status = ST_DEAD;
        if (t->stack) sysmem_free(t->stack);
        if (t->k0) sysmem_free(t->k0);
        t->stack = 0;
        t->k0 = 0;
        obj_del(t->id);
    }
}

/* How much of the thread's host stack is left.  A guest call chain that
   runs off the end of the stack smashes the return address and the CPU
   branches somewhere wild, which is almost impossible to diagnose; this
   turns it into a message naming the thread. */
#ifdef __wii__
static void stack_guard(void) {
    Thread *self = tls_self;
    if (!self || !self->host) return;
    if (wii_pthread_stack_intact(self->host)) return;
    /* The canary is gone: a guest call chain ran off the end of the host
       stack.  Whatever the thread returns to is already garbage, so stop
       it here rather than let the CPU branch somewhere wild. */
    fprintf(stderr, "HLE: guest thread %x overran its host stack\n", self->id);
    /* Leave the way any thread does: hand the baton on and release the big
       lock.  Exiting without that left every other thread waiting for it. */
    exit_current(0x800201ACu, 0);
}
#endif

static void exit_current(uint32_t status, int del) {
    Thread *self = tls_self;
    thread_finish(self, status, del);
    self->host_alive = 0;
    reschedule(self);  // hands the baton over and returns (we're dormant)
    pthread_mutex_unlock(&g_big);
    if (del) free(self);  // NB: object already unlinked
    pthread_exit(NULL);
}

#ifdef __wii__
volatile int g_host_threads_started;
#endif

static void *host_thread_main(void *arg) {
    Thread *t = arg;
#ifdef __wii__
    g_host_threads_started++;
#endif
    pthread_mutex_lock(&g_big);
    while (g_cur != t && !t->killed && !host_replaced(t)) pthread_cond_wait(&t->cv, &g_big);
    if (t->killed || host_replaced(t)) {  // terminated (or restarted) before it ever ran
        pthread_mutex_unlock(&g_big);
        return NULL;
    }
    tls_self = t;
    t->status = ST_RUNNING;
    call_indirect(&t->ctx, t->entry);
    // returning from the entry = sceKernelExitThread(v0); the kernel deletes
    // the module_start thread (and frees its stack) once it returns
    exit_current(t->ctx.r[2], t->id == g_boot_thread_id);
    return NULL;
}

static Thread *thread_new(uint32_t name, uint32_t entry, uint32_t prio, uint32_t stack_size,
                          uint32_t attr) {
    Thread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->id = obj_new(t, OBJ_THREAD, name, attr);
    if (t->id < 0) {
        free(t);
        return NULL;
    }
    t->entry = entry;
    t->init_prio = t->prio = prio;
    t->stack_size = (stack_size + 0xFF) & ~0xFFu;
    t->status = ST_DORMANT;
    if (t->stack_size) {
        t->stack = sysmem_alloc(t->stack_size, 1, 0, 0x100);
        if (!t->stack) {
            /* The guest gets the error a real PSP would give it. */
            fprintf(stderr, "out of memory for thread stack (%u bytes)\n", t->stack_size);
            obj_del(t->id);
            free(t);
            return NULL;
        }
        memset(host(t->stack), 0xFF, t->stack_size);
    }
    pthread_cond_init(&t->cv, NULL);
    /* The PSP kernel gives each user thread a context pointer in $k0.  Newlib's
       PSP port stores that thread's reent pointer at [k0+4]; without it,
       __getreent() reports "no reent structure" and libc calls fail. */
    t->k0 = sysmem_alloc(0x100, 0, 0, 0x100);
    if (!t->k0) {
        t->k0 = 0x1000u + (uint32_t)(t->id & 0x3F) * 0x100u;
        fprintf(stderr, "thread: no sysmem for k0 context; using low %08X\n", t->k0);
    }
    memset(host(t->k0), 0, 0x100);
    return t;
}

static void thread_prepare(Thread *t, uint32_t arglen, uint32_t argp, uint32_t gp) {
    memset(&t->ctx, 0, sizeof t->ctx);
    uint32_t sp = t->stack + t->stack_size;
    if (argp && arglen) {
        sp -= (arglen + 15) & ~15u;
        memmove(host(sp), host(argp), arglen);
        t->ctx.r[5] = sp;
    }
    t->ctx.r[4] = arglen;
    t->ctx.r[26] = t->k0;
    t->ctx.r[29] = sp - 64;
    t->ctx.r[28] = gp;
    t->ctx.r[31] = 0;
    t->ctx.fcr31 = 0;
    t->ctx.vfpu_ctrl[0] = t->ctx.vfpu_ctrl[1] = 0xE4;  // no source prefix
}

// Boot: run module_start as a thread at priority 32, then hand over to it.
void sched_boot(Ctx *boot_ctx) {
    pthread_mutex_lock(&g_big);
    Thread *t = thread_new(0, g_entry, 32, 0x40000, 0);
    if (!t) {
        fprintf(stderr, "cannot create the module_start thread\n");
        exit(1);
    }
    strcpy(t->h.name, "module_start");
    g_boot_thread_id = t->id;
    thread_prepare(t, boot_ctx->r[4], boot_ctx->r[5], boot_ctx->r[28]);
    t->status = ST_RUNNING;
    t->seq = ++g_seq;
    g_cur = t;
    start_host(t);
    pthread_mutex_unlock(&g_big);  // the main host thread goes on to run the frontend
}

// ---------------------------------------------------------------- waiting lists

static void wake_waiters(int wait_type, int id, int32_t result) {
    FOR_EACH_THREAD(t) {
        if (t->status == ST_WAITING && t->wait_type == wait_type && t->wait_id == id) {
            make_ready(t, result);
        }
    }
}

// ---------------------------------------------------------------- threads

void sceKernelCreateThread(Ctx *c) {
    uint32_t prio = ARG(2);
    if (prio < 8 || prio > 119) {
        RET(SCE_KERNEL_ERROR_ILLEGAL_PRIORITY);
        return;
    }
    Thread *t = thread_new(ARG(0), ARG(1), prio, ARG(3), ARG(4));
    if (!t) {
        RET(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }
    t->gp = c->r[28];
    HLOG("  thread %x '%s' entry=%08X prio=%u stack=%x\n", t->id, t->h.name, t->entry, prio, ARG(3));
    RET(t->id);
}

void sceKernelStartThread(Ctx *c) {
    Thread *t = obj_get((int)ARG(0), OBJ_THREAD);
    if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (t->status != ST_DORMANT) { RET(SCE_KERNEL_ERROR_NOT_DORMANT); return; }
    thread_prepare(t, ARG(1), ARG(2), t->gp);
    t->status = ST_READY;
    t->seq = ++g_seq;
    t->host_alive = 0;
    RET(0);
    yield_if_needed();
}

void sceKernelExitThread(Ctx *c) { exit_current(ARG(0), 0); }
void sceKernelExitDeleteThread(Ctx *c) { exit_current(ARG(0), 1); }

static void terminate(Thread *t, int del) {
    if (t->host_alive) {
        t->killed = 1;
        pthread_cond_broadcast(&t->cv);
    }
    t->host_alive = 0;
    thread_finish(t, 0x800201ACu, del);
}

void sceKernelTerminateThread(Ctx *c) {
    Thread *t = obj_get((int)ARG(0), OBJ_THREAD);
    if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (t == tls_self) { RET(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT); return; }
    terminate(t, 0);
    RET(0);
}

void sceKernelTerminateDeleteThread(Ctx *c) {
    Thread *t = obj_get((int)ARG(0), OBJ_THREAD);
    if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (t == tls_self) { RET(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT); return; }
    terminate(t, 1);
    RET(0);
}

void sceKernelDeleteThread(Ctx *c) {
    Thread *t = obj_get((int)ARG(0), OBJ_THREAD);
    if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (t->status != ST_DORMANT) { RET(SCE_KERNEL_ERROR_NOT_DORMANT); return; }
    if (t->stack) sysmem_free(t->stack);
    if (t->k0) sysmem_free(t->k0);
    t->stack = 0;
    t->k0 = 0;
    obj_del(t->id);
    RET(0);
}

void sceKernelGetThreadId(Ctx *c) { RET(tls_self->id); }

void sceKernelGetThreadCurrentPriority(Ctx *c) { RET(tls_self->prio); }

void sceKernelChangeThreadPriority(Ctx *c) {
    Thread *t = ARG(0) ? obj_get((int)ARG(0), OBJ_THREAD) : tls_self;
    if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    uint32_t prio = ARG(1) ? ARG(1) : tls_self->prio;
    if (prio < 8 || prio > 119) { RET(SCE_KERNEL_ERROR_ILLEGAL_PRIORITY); return; }
    t->prio = prio;
    t->seq = ++g_seq;  // goes to the back of its new priority queue
    RET(0);
    yield_if_needed();
}

void sceKernelChangeCurrentThreadAttr(Ctx *c) {
    tls_self->h.attr = (tls_self->h.attr & ~ARG(0)) | ARG(1);
    RET(0);
}

void sceKernelReferThreadStatus(Ctx *c) {
    Thread *t = ARG(0) ? obj_get((int)ARG(0), OBJ_THREAD) : tls_self;
    if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    uint32_t p = ARG(1);
    uint32_t size = mem_r32(p);
    uint8_t buf[0x68] = {0};
    /* The guest reads these words little-endian.  Copying host words in
       with memcpy() byte-swapped every field on the Wii (a dormant thread
       read back as status 0x10000000). */
#define PUT(off, val) do { uint32_t x_ = (val); buf[(off)] = (uint8_t)x_; buf[(off) + 1] = (uint8_t)(x_ >> 8); \
                           buf[(off) + 2] = (uint8_t)(x_ >> 16); buf[(off) + 3] = (uint8_t)(x_ >> 24); } while (0)
    PUT(0, (uint32_t)sizeof buf);
    memcpy(buf + 4, t->h.name, 32);
    PUT(36, t->h.attr);
    PUT(40, (uint32_t)t->status);
    PUT(44, t->entry);
    PUT(48, t->stack);
    PUT(52, t->stack_size);
    PUT(56, t->gp);
    PUT(60, t->init_prio);
    PUT(64, t->prio);
    PUT(68, (uint32_t)t->wait_type);
    PUT(72, (uint32_t)t->wait_id);
    PUT(76, (uint32_t)t->wakeup_count);
    PUT(80, t->exit_status);
#undef PUT
    uint32_t n = guest_span(p, size < sizeof buf ? size : (uint32_t)sizeof buf);
    if (n) memcpy(host(p), buf, n);
    RET(0);
}

static void wait_thread_end(Ctx *c) {
    Thread *t = obj_get((int)ARG(0), OBJ_THREAD);
    if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    /* This call reports whether the wait itself succeeded; the thread's exit
       status is a separate value (sceKernelReferThreadStatus).  It used to
       return the exit status, so a movie thread that ended with a negative
       status made the game's PSMF player stop state retry forever. */
    /* A deleted thread has ended too.  The movie player leaves one worker
       in ST_DEAD when it stops it, and treating that as "still running"
       made the stop state retry forever. */
    if (t->status == ST_DORMANT || t->status == ST_DEAD) {
#ifdef HLE_TRACE
        { static int n; if (n++ < 80) fprintf(stderr, "WAITEND id=%x '%s' entry=%08X dormant exit=%08X\n", t->id, t->h.name, t->entry, t->exit_status); }
#endif
        RET(0);
        return;
    }
    /* A wait on one of the movie player's worker threads must block until the
       thread ends, exactly as on the PSP: the player uses this call to wait
       for playback to finish, and an immediate timeout makes it tear the
       movie down while the ring still holds video.  Other waits keep the
       historic non-blocking behaviour (arg 1 is a status pointer here, not a
       timeout, so passing it to wait_on made every such wait time out at
       once).  Blocking all of them hangs the boot.  Before the first picture
       the player also joins its workers, so only block once playback has
       really started. */
    const int movie_worker = t->entry == 0x088EEEFCu || t->entry == 0x088EF1C4u ||
                             t->entry == 0x088EF354u;
    extern int hle_movie_started(void);
    const int block = movie_worker && hle_movie_started();
    int32_t r = wait_on(c, W_THREAD_END, t->id, block ? 0 : ARG(1));
#ifdef HLE_TRACE
    { static int n; if (n++ < 80) fprintf(stderr, "WAITEND id=%x '%s' entry=%08X st=%d exit=%08X r=%d\n", t->id, t->h.name, t->entry, t->status, t->exit_status, r); }
#endif
    RET(r == (int32_t)SCE_KERNEL_ERROR_WAIT_TIMEOUT ? r : 0);
}
void sceKernelWaitThreadEnd(Ctx *c) { wait_thread_end(c); }
void sceKernelWaitThreadEndCB(Ctx *c) { wait_thread_end(c); }

void sceKernelSleepThread(Ctx *c) {
    Thread *self = tls_self;
    if (self->wakeup_count > 0) {
        self->wakeup_count--;
        RET(0);
        return;
    }
    RET(wait_on(c, W_SLEEP, 0, 0));
}

void sceKernelWakeupThread(Ctx *c) {
    Thread *t = obj_get((int)ARG(0), OBJ_THREAD);
    if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (t->status == ST_WAITING && t->wait_type == W_SLEEP) make_ready(t, 0);
    else t->wakeup_count++;
    RET(0);
    yield_if_needed();
}

void thread_delay(Ctx *c, uint64_t usec, int allow_cb) {
    (void)allow_cb;
    Thread *self = tls_self;
    if (usec < 200) usec = 200;  // the PSP never delays for less than this
    self->status = ST_WAITING;
    self->wait_type = W_DELAY;
    self->timeout_ptr = 0;
    self->deadline = hle_time_us() + usec;
    reschedule(self);
    RET(0);
}
void sceKernelDelayThread(Ctx *c) { thread_delay(c, ARG(0), 0); }
void sceKernelDelayThreadCB(Ctx *c) { thread_delay(c, ARG(0), 1); }

void hle_wait_gpu(Ctx *c, int list_id) {
    int32_t r = c->r[2];
    wait_on(c, W_GPU, list_id, 0);
    c->r[2] = (uint32_t)r;
}

void hle_wait_vblank(Ctx *c, int allow_cb) {
    (void)allow_cb;
#ifndef TKDR_FIXED_VBLANK
    /* While a movie plays, every vblank wait must really wait.  The shortcut
       below consumes a tick without waking threads already asleep on it, and
       the movie player's ringbuffer feeder then slept through whole seconds of
       vblanks; its ring drained, and the player's own "ring empty with stream
       left" rule ended the film after a few seconds.  Outside movies keep the
       shortcut: it lets the frame rate follow the frame time instead of being
       quantised to 60/30/20. */
    extern int hle_movie_active(void);
    if (!hle_movie_active()) {
        uint64_t now = hle_time_us(), cnt = (now - g_vb_base) / VBLANK_US;
        if (cnt != g_vb_served) {   /* a tick went by with nobody waiting: late, so no waiting for the next */
            g_vb_served = g_last_vblank = cnt;
            g_vb_base = now - cnt * VBLANK_US;
            RET(0);
            return;
        }
    }
#endif
    wait_on(c, W_VBLANK, 0, 0);
    RET(0);
}

// ---------------------------------------------------------------- semaphores

void sceKernelCreateSema(Ctx *c) {
    Sema *s = calloc(1, sizeof *s);
    if (!s) { RET(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    s->count = (int32_t)ARG(2);
    s->max = (int32_t)ARG(3);
    int id = obj_new(s, OBJ_SEMA, ARG(0), ARG(1));
    if (id < 0) {
        free(s);
        RET(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }
    RET(id);
}

void sceKernelDeleteSema(Ctx *c) {
    Sema *s = obj_get((int)ARG(0), OBJ_SEMA);
    if (!s) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    FOR_EACH_THREAD(t) {
        if (t->status == ST_WAITING && t->wait_type == W_SEMA && t->wait_id == (int)ARG(0))
            make_ready(t, (int32_t)SCE_KERNEL_ERROR_WAIT_DELETE);
    }
    obj_del((int)ARG(0));
    free(s);
    RET(0);
    yield_if_needed();
}

// Hand the count to waiters in priority (attr 0x100) or FIFO order.
static void sema_wake(int id, Sema *s) {
    for (;;) {
        Thread *best = NULL;
        FOR_EACH_THREAD(t) {
            if (t->status != ST_WAITING || t->wait_type != W_SEMA || t->wait_id != id) continue;
            if (!best || ((s->h.attr & 0x100) ? t->prio < best->prio : t->seq < best->seq)) best = t;
        }
        if (!best || (int32_t)best->wait_a > s->count) return;
        s->count -= (int32_t)best->wait_a;
        make_ready(best, 0);
    }
}

void sceKernelSignalSema(Ctx *c) {
    Sema *s = obj_get((int)ARG(0), OBJ_SEMA);
    if (!s) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    if (s->count + (int32_t)ARG(1) > s->max) { RET(SCE_KERNEL_ERROR_SEMA_OVF); return; }
    s->count += (int32_t)ARG(1);
    sema_wake((int)ARG(0), s);
    RET(0);
    yield_if_needed();
}

static void wait_sema(Ctx *c) {
    Sema *s = obj_get((int)ARG(0), OBJ_SEMA);
    if (!s) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    int32_t n = (int32_t)ARG(1);
    if (n <= 0 || n > s->max) { RET(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT); return; }
    if (s->count >= n) {
        s->count -= n;
        RET(0);
        return;
    }
    tls_self->wait_a = (uint32_t)n;
    RET(wait_on(c, W_SEMA, (int)ARG(0), ARG(2)));
}
void sceKernelWaitSema(Ctx *c) { wait_sema(c); }
void sceKernelWaitSemaCB(Ctx *c) { wait_sema(c); }

void sceKernelPollSema(Ctx *c) {
    Sema *s = obj_get((int)ARG(0), OBJ_SEMA);
    if (!s) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    int32_t n = (int32_t)ARG(1);
    if (n <= 0) { RET(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT); return; }
    if (s->count < n) { RET(SCE_KERNEL_ERROR_SEMA_ZERO); return; }
    s->count -= n;
    RET(0);
}

// ---------------------------------------------------------------- event flags

#define EVF_AND 0x00
#define EVF_OR 0x01
#define EVF_CLEARALL 0x10
#define EVF_CLEARPAT 0x20

static int evf_match(EventFlag *e, uint32_t bits, uint32_t mode) {
    return (mode & EVF_OR) ? (e->pattern & bits) != 0 : (e->pattern & bits) == bits;
}

static void evf_consume(EventFlag *e, uint32_t bits, uint32_t mode, uint32_t out_ptr) {
    if (out_ptr) mem_w32(out_ptr, e->pattern);
    if (mode & EVF_CLEARALL) e->pattern = 0;
    else if (mode & EVF_CLEARPAT) e->pattern &= ~bits;
}

void sceKernelCreateEventFlag(Ctx *c) {
    EventFlag *e = calloc(1, sizeof *e);
    if (!e) { RET(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    e->init = e->pattern = ARG(2);
    int id = obj_new(e, OBJ_EVENT, ARG(0), ARG(1));
    if (id < 0) {
        free(e);
        RET(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }
    RET(id);
}

void sceKernelDeleteEventFlag(Ctx *c) {
    EventFlag *e = obj_get((int)ARG(0), OBJ_EVENT);
    if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    FOR_EACH_THREAD(t) {
        if (t->status == ST_WAITING && t->wait_type == W_EVENT && t->wait_id == (int)ARG(0))
            make_ready(t, (int32_t)SCE_KERNEL_ERROR_WAIT_DELETE);
    }
    obj_del((int)ARG(0));
    free(e);
    RET(0);
    yield_if_needed();
}

void sceKernelSetEventFlag(Ctx *c) {
    EventFlag *e = obj_get((int)ARG(0), OBJ_EVENT);
    if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    e->pattern |= ARG(1);
    FOR_EACH_THREAD(t) {
        if (t->status != ST_WAITING || t->wait_type != W_EVENT || t->wait_id != (int)ARG(0)) continue;
        if (evf_match(e, t->wait_a, t->wait_b)) {
            evf_consume(e, t->wait_a, t->wait_b, t->wait_c);
            make_ready(t, 0);
        }
    }
    RET(0);
    yield_if_needed();
}

void sceKernelClearEventFlag(Ctx *c) {
    EventFlag *e = obj_get((int)ARG(0), OBJ_EVENT);
    if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    /* sceKernelClearEventFlag(evid, bits) clears the given bits; the old code
       kept only them, which wiped the flag down to the mask. */
    e->pattern &= ~ARG(1);
    RET(0);
}

static void wait_event(Ctx *c) {
    EventFlag *e = obj_get((int)ARG(0), OBJ_EVENT);
    if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    uint32_t bits = ARG(1), mode = ARG(2), out = ARG(3);
    if (!bits) { RET(SCE_KERNEL_ERROR_EVF_ILPAT); return; }
    if (evf_match(e, bits, mode)) {
        evf_consume(e, bits, mode, out);
        RET(0);
        return;
    }
    Thread *self = tls_self;
    self->wait_a = bits;
    self->wait_b = mode;
    self->wait_c = out;
    int32_t r = wait_on(c, W_EVENT, (int)ARG(0), ARG(4));
    if (r && out) {
        /* The flag may have been deleted while this thread waited, so look
           it up again rather than trusting the old pointer. */
        EventFlag *now = obj_get((int)ARG(0), OBJ_EVENT);
        mem_w32(out, now ? now->pattern : 0);
    }
    RET(r);
}
void sceKernelWaitEventFlag(Ctx *c) { wait_event(c); }
void sceKernelWaitEventFlagCB(Ctx *c) { wait_event(c); }

void sceKernelPollEventFlag(Ctx *c) {
    EventFlag *e = obj_get((int)ARG(0), OBJ_EVENT);
    if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    if (!evf_match(e, ARG(1), ARG(2))) {
        if (ARG(3)) mem_w32(ARG(3), e->pattern);
        RET(SCE_KERNEL_ERROR_EVF_COND);
        return;
    }
    evf_consume(e, ARG(1), ARG(2), ARG(3));
    RET(0);
}

void sceKernelReferEventFlagStatus(Ctx *c) {
    EventFlag *e = obj_get((int)ARG(0), OBJ_EVENT);
    if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    uint32_t p = ARG(1), waiting = 0;
    FOR_EACH_THREAD(t) {
        if (t->status == ST_WAITING && t->wait_type == W_EVENT && t->wait_id == (int)ARG(0)) waiting++;
    }
    mem_w32(p, 52);
    memcpy(host(p + 4), e->h.name, 32);
    mem_w32(p + 36, e->h.attr);
    mem_w32(p + 40, e->init);
    mem_w32(p + 44, e->pattern);
    mem_w32(p + 48, waiting);
    RET(0);
}

// ---------------------------------------------------------------- fixed pools

void sceKernelCreateFpl(Ctx *c) {
    uint32_t bs = ARG(3), n = ARG(4);
    uint32_t align = 4;
    if (ARG(5) && mem_r32(ARG(5)) >= 8) align = mem_r32(ARG(5) + 4) ? mem_r32(ARG(5) + 4) : 4;
    uint32_t stride = (bs + align - 1) & ~(align - 1);
    uint32_t base = sysmem_alloc(stride * n, (ARG(2) & 0x4000) ? 1 : 0, 0, align);
    if (!base) { RET(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    Fpl *f = calloc(1, sizeof *f);
    uint8_t *used = calloc(n ? n : 1, 1);
    int id = -1;
    if (f && used) {
        f->base = base;
        f->block_size = stride;
        f->nblocks = n;
        f->used = used;
        id = obj_new(f, OBJ_FPL, ARG(0), ARG(2));
    }
    if (id < 0) {
        free(used);
        free(f);
        sysmem_free(base);
        RET(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }
    RET(id);
}

void sceKernelDeleteFpl(Ctx *c) {
    Fpl *f = obj_get((int)ARG(0), OBJ_FPL);
    if (!f) { RET(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
    sysmem_free(f->base);
    obj_del((int)ARG(0));
    free(f->used);
    free(f);
    RET(0);
}

void sceKernelTryAllocateFpl(Ctx *c) {
    Fpl *f = obj_get((int)ARG(0), OBJ_FPL);
    if (!f) { RET(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
    for (uint32_t k = 0; k < f->nblocks; k++)
        if (!f->used[k]) {
            f->used[k] = 1;
            mem_w32(ARG(1), f->base + k * f->block_size);
            RET(0);
            return;
        }
    RET(SCE_KERNEL_ERROR_NO_FREE);
}

void sceKernelFreeFpl(Ctx *c) {
    Fpl *f = obj_get((int)ARG(0), OBJ_FPL);
    if (!f) { RET(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
    uint32_t k = (ARG(1) - f->base) / f->block_size;
    if (ARG(1) < f->base || k >= f->nblocks || !f->used[k]) {
        RET(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
        return;
    }
    f->used[k] = 0;
    RET(0);
}

// ---------------------------------------------------------------- callbacks

void sceKernelCreateCallback(Ctx *c) {
    Callback *cb = calloc(1, sizeof *cb);
    if (!cb) { RET(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    cb->func = ARG(1);
    cb->arg = ARG(2);
    cb->owner = tls_self->id;
    int id = obj_new(cb, OBJ_CALLBACK, ARG(0), 0);
    if (id < 0) {
        free(cb);
        RET(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }
    RET(id);
}

void sceKernelDeleteCallback(Ctx *c) {
    Callback *cb = obj_get((int)ARG(0), OBJ_CALLBACK);
    if (!cb) { RET(SCE_KERNEL_ERROR_UNKNOWN_CBID); return; }
    obj_del((int)ARG(0));
    free(cb);
    RET(0);
}

// ---------------------------------------------------------------- time

void sceKernelGetSystemTime(Ctx *c) {
    uint64_t t = hle_time_us();
    mem_w32(ARG(0), (uint32_t)t);
    mem_w32(ARG(0) + 4, (uint32_t)(t >> 32));
    RET(0);
}
void sceKernelGetSystemTimeLow(Ctx *c) { RET((uint32_t)hle_time_us()); }
void sceKernelGetSystemTimeWide(Ctx *c) { RET64(hle_time_us()); }

// Split a clock into seconds + microseconds; with no seconds pointer the
// whole value goes to the microseconds output (games rely on this).
static void clock_split(uint64_t t, uint32_t sec_ptr, uint32_t usec_ptr) {
    if (sec_ptr) {
        mem_w32(sec_ptr, (uint32_t)(t / 1000000u));
        if (usec_ptr) mem_w32(usec_ptr, (uint32_t)(t % 1000000u));
    } else if (usec_ptr) {
        mem_w32(usec_ptr, (uint32_t)t);
    }
}
void sceKernelSysClock2USec(Ctx *c) {
    clock_split(mem_r32(ARG(0)) | ((uint64_t)mem_r32(ARG(0) + 4) << 32), ARG(1), ARG(2));
    RET(0);
}
void sceKernelSysClock2USecWide(Ctx *c) {
    clock_split(ARG(0) | ((uint64_t)ARG(1) << 32), ARG(2), ARG(3));
    RET(0);
}
void sceKernelUSec2SysClockWide(Ctx *c) { RET64(ARG(0)); }
