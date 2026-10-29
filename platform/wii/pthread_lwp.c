/* pthreads on top of libogc's LWP scheduler.
 *
 * devkitPPC's libsysbase pthread implementation routes through the newer
 * KThread subsystem, which (on this toolchain) does not schedule a thread
 * created from a homebrew main loop.  libogc's own threads all use LWP, so the
 * port maps the small pthread subset the recomp runtime needs onto LWP.
 *
 * Types come from newlib's <pthread.h>; this file defines the functions and
 * stores LWP handles in the standard structs' opaque storage.
 */
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ogc/machine/processor.h>
#include <ogc/mutex.h>
#include <ogc/cond.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>

#include "wii_platform.h"

#define MUTEX_MAGIC 0x4D555458u  /* "MUTX" */
#define COND_MAGIC  0x434F4E44u  /* "COND" */
#define THREAD_PRIO 64

/* The stack canary: a pattern at the bottom of every host thread stack. */
#define STACK_CANARY_BYTE 0xA5
#define STACK_CANARY_BYTES 4096

struct __pthread_t {
    lwp_t lwp;
    void *(*fn)(void *);
    void *arg;
    void *ret;
    void *stack;
    uint32_t stack_size;
    jmp_buf jb;
    int detached;
};

/* Each host LWP needs its own identity: the scheduler uses pthread_self() to
   tell a guest thread's host thread from the frontend, the GE thread and a
   host thread left over from a terminated run.

   This must not use __thread.  libogc keeps r2 (the PowerPC thread pointer)
   at the small-data base for every thread, and with -fdata-sections the
   linker leaves each .tbss.* section unmerged at offset 0, so every __thread
   variable in the program was one shared word.  pthread_self() returned
   whichever thread had started last (NULL after any thread exited), and the
   storage lock's depth counter lived in that same word.  The registry below
   maps libogc's own thread handle to its pthread instead. */
#define MAX_THREADS 64
static struct __pthread_t *g_threads[MAX_THREADS];

static void registry_add(struct __pthread_t *t) {
    u32 level;
    _CPU_ISR_Disable(level);
    for (int i = 0; i < MAX_THREADS; i++)
        if (!g_threads[i]) { g_threads[i] = t; break; }
    _CPU_ISR_Restore(level);
}

static void registry_remove(struct __pthread_t *t) {
    u32 level;
    _CPU_ISR_Disable(level);
    for (int i = 0; i < MAX_THREADS; i++)
        if (g_threads[i] == t) { g_threads[i] = NULL; break; }
    _CPU_ISR_Restore(level);
}

#ifdef EXP_STACKPROBE
/* -DEXP_STACKPROBE: every host thread stack is filled with the canary byte when the thread starts; STACKPROBE lines
   give how deep each thread has been (when it ends, and every 300 frontend frames) against the size it was given. */
#include <stdio.h>
#include <ogc/system.h>
static void stack_probe_print(const struct __pthread_t *t, const char *when) {
    const uint32_t *w = (const uint32_t *)t->stack;
    const uint32_t n = t->stack_size / 4;
    uint32_t i = 0;
    while (i < n && w[i] == 0xA5A5A5A5u) i++;
    fprintf(stderr, "STACKPROBE %s arg %p size %u used %u\n", when, t->arg, (unsigned)t->stack_size, (unsigned)(t->stack_size - i * 4));
}
void wii_stack_probe_report(void) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (g_threads[i]) stack_probe_print(g_threads[i], "live");
    {
        const struct mallinfo mi = mallinfo();
        fprintf(stderr, "STACKPROBE heap: from the system %d, in use %d, free in it %d; arena1 %p..%p\n", mi.arena, mi.uordblks, mi.fordblks,
                (void *)SYS_GetArena1Lo(), (void *)SYS_GetArena1Hi());
    }
}
#endif

static struct __pthread_t *registry_find(lwp_t self) {
    for (int i = 0; i < MAX_THREADS; i++) {
        struct __pthread_t *t = g_threads[i];
        if (t && t->lwp == self) return t;
    }
    return NULL;
}

uint32_t wii_thread_id(void) { return (uint32_t)LWP_GetSelf(); }

static void mutex_init_once(pthread_mutex_t *m) {
    if (m->type == (int)MUTEX_MAGIC)
        return;
    LWP_MutexInit(&m->normal, false);
    m->type = (int)MUTEX_MAGIC;
}

static void cond_init_once(pthread_cond_t *c) {
    if (c->clock_id == (clockid_t)COND_MAGIC)
        return;
    LWP_CondInit(&c->cond);
    c->clock_id = (clockid_t)COND_MAGIC;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    (void)a;
    m->type = 0;
    m->normal = 0;
    mutex_init_once(m);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
    if (m->type == (int)MUTEX_MAGIC)
        LWP_MutexDestroy(m->normal);
    m->type = 0;
    m->normal = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    mutex_init_once(m);
    return LWP_MutexLock(m->normal) == 0 ? 0 : EINVAL;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    mutex_init_once(m);
    return LWP_MutexTryLock(m->normal) == 0 ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    mutex_init_once(m);
    return LWP_MutexUnlock(m->normal) == 0 ? 0 : EINVAL;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    (void)a;
    c->clock_id = 0;
    c->cond = 0;
    cond_init_once(c);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) {
    if (c->clock_id == (clockid_t)COND_MAGIC)
        LWP_CondDestroy(c->cond);
    c->clock_id = 0;
    c->cond = 0;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    cond_init_once(c);
    mutex_init_once(m);
    return LWP_CondWait(c->cond, m->normal) == 0 ? 0 : EINVAL;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abs) {
    cond_init_once(c);
    mutex_init_once(m);
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t rel_ns = ((int64_t)abs->tv_sec - (int64_t)now.tv_sec) * 1000000000ll +
                     ((int64_t)abs->tv_nsec - (int64_t)now.tv_nsec);
    if (rel_ns <= 0)
        return ETIMEDOUT;
    struct timespec rel = {
        .tv_sec = (time_t)(rel_ns / 1000000000ll),
        .tv_nsec = (long)(rel_ns % 1000000000ll),
    };
    s32 r = LWP_CondTimedWait(c->cond, m->normal, &rel);
    if (r == 0) return 0;
    return (r == -1 || r == ETIMEDOUT) ? ETIMEDOUT : EINVAL;
}

int pthread_cond_signal(pthread_cond_t *c) {
    cond_init_once(c);
    LWP_CondSignal(c->cond);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    cond_init_once(c);
    LWP_CondBroadcast(c->cond);
    return 0;
}

int pthread_attr_init(pthread_attr_t *a) {
    memset(a, 0, sizeof *a);
    /* Every guest CALL becomes a C call in the recompiled code, so a guest
       call chain consumes this stack directly.  The chain can be thousands of
       frames deep for a deep expression tree, and overflowing it corrupts the
       return address, which the CPU then jumps to: a wild branch rather than
       a diagnosable fault. */
    /* MEM1 on the Wii is 24 MiB and the game runs about a dozen threads, so
       the stacks have to fit.  -DEXP_STACKPROBE measured the runtime's own
       threads (the GE thread, audio, workers) at a few KiB each; 256 KiB is
       ample and pthread_create() still drops it further if MEM1 is short. */
    a->stacksize = 256u << 10;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *a) {
    (void)a;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *a, size_t size) {
    if (size < (32u << 10)) size = 32u << 10;
    if (size > (4u << 20)) size = 4u << 20;
    a->stacksize = (int)size;
    return 0;
}

static void *thread_trampoline(void *arg) {
    struct __pthread_t *t = arg;
    /* LWP_CreateThread stores the handle in t->lwp, but only after the
       thread exists; take it from the thread itself before registering. */
    t->lwp = LWP_GetSelf();
    registry_add(t);
    if (setjmp(t->jb) == 0)
        t->ret = t->fn(t->arg);
#ifdef EXP_STACKPROBE
    stack_probe_print(t, "end");
#endif
    registry_remove(t);
    void *ret = t->ret;
    if (t->detached) {
        /* This thread is still running on t->stack, and keeps running on it
           until libogc switches away at the exit.  Free it as the very last
           step: free(t) could block on the malloc lock, and another thread
           could then be handed the stack memory while this one still uses it. */
        void *stack = t->stack;
        free(t);
        free(stack);
    }
    return ret;
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr,
                   void *(*fn)(void *), void *arg) {
    struct __pthread_t *t = calloc(1, sizeof *t);
    if (!t) return ENOMEM;
    t->fn = fn;
    t->arg = arg;
    t->stack_size = attr ? (uint32_t)attr->stacksize : (256u << 10);
    if (t->stack_size < (32u << 10)) t->stack_size = 32u << 10;
    /* MEM1 is finite.  If the stack the caller asked for does not fit,
       take a smaller one rather than failing the thread outright: a guest
       thread that cannot be created takes the whole game down with it. */
    for (;;) {
        t->stack = memalign(32, t->stack_size);
        if (t->stack || t->stack_size <= (256u << 10)) break;
        t->stack_size >>= 1;
    }
    if (!t->stack) {
        free(t);
        return ENOMEM;
    }
    /* Canary at the bottom of the stack.  The stack grows down, so this is
       the first thing an overrunning guest call chain smashes; the runtime
       checks it to turn a wild branch into a clear message. */
    {
        uint32_t *c = (uint32_t *)t->stack;
#ifdef EXP_STACKPROBE
        memset(t->stack, STACK_CANARY_BYTE, t->stack_size);
#endif
        for (unsigned i = 0; i < STACK_CANARY_BYTES / 4; i++) c[i] = 0xA5A5A5A5u;
    }
    s32 r = LWP_CreateThread(&t->lwp, thread_trampoline, t, t->stack,
                             t->stack_size, THREAD_PRIO);
    if (r < 0) {
        free(t->stack);
        free(t);
        return EAGAIN;
    }
    *out = t;
    return 0;
}

int pthread_join(pthread_t t, void **ret) {
    if (!t) return EINVAL;
    void *v = NULL;
    LWP_JoinThread(t->lwp, &v);
    if (ret) *ret = v;
    free(t->stack);
    free(t);
    return 0;
}

int pthread_detach(pthread_t t) {
    if (t) t->detached = 1;
    return 0;
}

void pthread_exit(void *ret) {
    struct __pthread_t *self = registry_find(LWP_GetSelf());
    if (self) {
        self->ret = ret;
        longjmp(self->jb, 1);
    }
    for (;;) LWP_YieldThread();
}

/* NULL for the threads this layer did not create (main, logger, audio). */
pthread_t pthread_self(void) {
    return registry_find(LWP_GetSelf());
}

int pthread_once(pthread_once_t *once, void (*init)(void)) {
    if (!once || !init) return EINVAL;
    if (once->status == 0) {
        once->status = 1;
        init();
        once->status = 2;
    }
    return 0;
}

/* The stack a thread runs on, so the runtime can notice an overflow.
   libogc's LWP_CreateThread takes the stack base and the size, and the
   stack grows down from the top, so the base is the low address. */
void *wii_pthread_stack_base(void *thread) {
    if (!thread) return NULL;
    return ((struct __pthread_t *)thread)->stack;
}

size_t wii_pthread_stack_size(void *thread) {
    if (!thread) return 0;
    return ((struct __pthread_t *)thread)->stack_size;
}

/* True when the thread's stack still has its canary, i.e. no guest call
   chain has run off the end of it.  Compared a word at a time: this runs
   often enough that a byte loop would show up in the profile. */
bool wii_pthread_stack_intact(void *thread) {
    if (!thread) return true;
    const struct __pthread_t *t = (const struct __pthread_t *)thread;
    if (!t->stack) return true;
    const uint32_t *p = (const uint32_t *)t->stack;
    const uint32_t canary = 0xA5A5A5A5u;
    for (unsigned i = 0; i < STACK_CANARY_BYTES / 4; i++)
        if (p[i] != canary) return false;
    return true;
}
