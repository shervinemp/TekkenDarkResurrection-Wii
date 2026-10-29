#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <malloc.h>
#include <unistd.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>
#include <ogc/system.h>

#include "wii_platform.h"
#include "hle/hle.h"

/* The log file lives on the SD card, the same device the guest reads its ISO
   from, and libfat's SD driver is not reentrant (see the storage lock in
   runtime/hle/dispatch.c).  Writing the log inline would block the frontend
   for the whole duration of a guest file read, and the frontend is the
   thread that samples the controller: a blocked frontend misses button
   presses.  So the log lines are queued here and written by a dedicated
   thread that owns the file. */
#ifdef WII_SDLOG
#define LOG_LINES 256   /* a report is ~60 lines and must not be dropped */
#else
#define LOG_LINES 64
#endif
#define LOG_LINE_MAX 512

static FILE *g_log;
static char g_log_path[256];

static char g_queue[LOG_LINES][LOG_LINE_MAX];
static volatile uint32_t g_q_head, g_q_tail;  // head = next write, tail = next fill
static volatile int g_q_stop;
static lwp_t g_log_thread;
/* Any thread may log, so producers take turns at the queue; the writer
   thread is the only consumer and needs no lock. */
static mutex_t g_q_lock = LWP_MUTEX_NULL;

static void *log_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t tail = g_q_tail;
        if (g_q_head == tail) {
            if (g_q_stop) break;
            usleep(2000);
            continue;
        }
        char line[LOG_LINE_MAX];
        uint32_t n = strlen(g_queue[tail]);
        if (n >= LOG_LINE_MAX) n = LOG_LINE_MAX - 1;
        memcpy(line, g_queue[tail], n);
        line[n] = 0;
        g_q_tail = (tail + 1) % LOG_LINES;
        storage_lock();
        fputs(line, g_log);
#ifdef WII_SDLOG
        if (g_q_head == g_q_tail) {   /* a report goes out in one write, and to the card */
            fflush(g_log);
            fsync(fileno(g_log));
        }
#else
        fflush(g_log);
#endif
        storage_unlock();
    }
    return NULL;
}

#ifdef WII_SDLOG
/* The per-report lines of the demo and profiling builds (DEMOFPS, SPEED,
   DRAWCATS ...) are printed to stderr, which only Dolphin's OSReport mirror
   shows.  A build with -DWII_SDLOG sends stderr to the same SD-card log as
   wii_log(), so a run on a console leaves the numbers in debug.log.  Lines
   are not dropped when the queue is full: the writer is waited for. */
#include <sys/iosupport.h>
static const devoptab_t *s_err_orig;   /* the device stderr had (Dolphin's OSReport, if mirrored) */
static char s_err_line[LOG_LINE_MAX];
static size_t s_err_n;
static mutex_t s_err_lock = LWP_MUTEX_NULL;

static void queue_line(const char *line);
static ssize_t sdlog_write(struct _reent *r, void *fd, const char *ptr, size_t len) {
    if (s_err_orig && s_err_orig->write_r) s_err_orig->write_r(r, fd, ptr, len);
    if (s_err_lock != LWP_MUTEX_NULL) LWP_MutexLock(s_err_lock);
    for (size_t i = 0; i < len; i++) {
        const char c = ptr[i];
        s_err_line[s_err_n++] = c;
        if (c == '\n' || s_err_n >= LOG_LINE_MAX - 1) {
            s_err_line[s_err_n] = 0;
            for (int w = 0; w < 1000 && (g_q_head + 1) % LOG_LINES == g_q_tail; w++) usleep(2000);
            queue_line(s_err_line);
            s_err_n = 0;
        }
    }
    if (s_err_lock != LWP_MUTEX_NULL) LWP_MutexUnlock(s_err_lock);
    return (ssize_t)len;
}
static devoptab_t s_sdlog_dotab = {.name = "sdlog", .structSize = 0, .write_r = sdlog_write};
#endif

void wii_log_init(void) {
    /* Prefer the SD card; fall back to USB (FAT only).  The directory is
       created by the storage layer before this is called. */
    static const char *paths[] = {
        "sd:/apps/tkdr/debug.log",
        "usb:/apps/tkdr/debug.log",
        "sd:/apps/tekken-dr/debug.log",
        "usb:/apps/tekken-dr/debug.log",
    };
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        FILE *f = fopen(paths[i], "wb");
        if (f) {
            g_log = f;
            snprintf(g_log_path, sizeof g_log_path, "%s", paths[i]);
            setvbuf(f, NULL, _IOLBF, 1024);
            break;
        }
    }
    if (!g_log) return;
    LWP_MutexInit(&g_q_lock, false);
#ifdef WII_SDLOG
    LWP_MutexInit(&s_err_lock, false);
    s_err_orig = devoptab_list[STD_ERR];
    devoptab_list[STD_ERR] = &s_sdlog_dotab;
#endif
    void *stack = memalign(32, 32u << 10);
    if (!stack || LWP_CreateThread(&g_log_thread, log_thread, NULL, stack,
                                   32u << 10, 64) < 0) {
        wii_log("WII log: writer thread failed; logging to screen only\n");
    }
}

#ifdef WII_HEARTBEAT
extern volatile int g_gx_stage;
void wii_gxscene_chip_status(int *lag, int *cmd_idle, int *read_idle, unsigned *fifo);
extern volatile uint32_t g_gx_lists, g_gx_presents, g_gx_texconv, g_gx_waits, g_gx_binds, g_gx_draws;
extern volatile unsigned g_hle_calls;
extern volatile uint32_t g_frontend_frames;
extern const char *volatile g_hb_hle;
extern volatile uint32_t g_hb_ra;
extern uint64_t g_iso_read_bytes, g_iso_read_calls;
static lwp_t s_hb_thread;

static void *heartbeat_thread(void *arg) {
    (void)arg;
    for (;;) {
        usleep(1000000);
        struct mallinfo mi = mallinfo();
        int lag = 0, cmd_idle = 0, read_idle = 0;
        unsigned fifo = 0;
        wii_gxscene_chip_status(&lag, &cmd_idle, &read_idle, &fifo);
        wii_log("HB: fe=%u hle=%u gx stage=%d lists=%u pres=%u draws=%u binds=%u conv=%u waits=%u "
                "chip lag=%d idle=%d/%d fifo=%u pool=%u heapfree=%d last=%s ra=%08x iso=%llu/%llu "
                "audio_underruns=%u\n",
               g_frontend_frames, g_hle_calls, g_gx_stage, g_gx_lists, g_gx_presents,
               g_gx_draws, g_gx_binds, g_gx_texconv, g_gx_waits, lag, cmd_idle, read_idle, fifo,
               (unsigned)wii_big_used(), mi.fordblks, g_hb_hle ? g_hb_hle : "-", g_hb_ra,
               (unsigned long long)g_iso_read_bytes, (unsigned long long)g_iso_read_calls,
               (unsigned)wii_audio_underruns());
    }
    return NULL;
}

#endif  /* WII_HEARTBEAT */

/* libogc's own panic handler draws the crash on a framebuffer nobody is
   looking at and then spins, so a fault looks exactly like a hang.  This one
   reports to the log first.  It is installed in every build, not just the
   heartbeat one. */
#include <tuxedo/ppc/exception.h>

static void panic_handler(unsigned exid, PPCContext *ctx) {
    u32 dar, dsisr;
    __asm__ volatile("mfspr %0, 19" : "=r"(dar));
    __asm__ volatile("mfspr %0, 18" : "=r"(dsisr));
    printf("EXC: id=%u pc=%08x lr=%08x sp=%08x dar=%08x dsisr=%08x r3=%08x r4=%08x r5=%08x\n",
           exid, (unsigned)ctx->pc, (unsigned)ctx->lr, (unsigned)ctx->gpr[1], (unsigned)dar,
           (unsigned)dsisr, (unsigned)ctx->gpr[3], (unsigned)ctx->gpr[4], (unsigned)ctx->gpr[5]);
    wii_video_show_console();
#ifdef HLE_TRACE
    hle_dump_recent_calls();
    {
        Ctx *g = sched_current_ctx();
        if (g)
            fprintf(stderr, "GUEST ra=%08X sp=%08X r4=%08X r5=%08X r6=%08X r7=%08X r28=%08X r29=%08X\n",
                    g->r[31], g->r[29], g->r[4], g->r[5], g->r[6], g->r[7], g->r[28], g->r[29]);
        else
            fprintf(stderr, "GUEST: no current guest thread\n");
        fprintf(stderr, "EXCTX ctr=%08X lr=%08X\n", (unsigned)ctx->ctr, (unsigned)ctx->lr);
        if ((uintptr_t)ctx->gpr[1] > 0x80000000u) {
            const uint32_t *sp = (const uint32_t *)(uintptr_t)ctx->gpr[1];
            fprintf(stderr, "HOSTSTACK:\n");
            for (int i = 0; i < 48; i++)
                fprintf(stderr, "  %08X: %08X\n", (unsigned)(uintptr_t)&sp[i], sp[i]);
        }
        /* The OSReport path is not reliable from an exception handler; put
           the same state on the SD card so it survives the screen. */
        FILE *cf = fopen("sd:/apps/tkdr/crash.txt", "wb");
        if (!cf) cf = fopen("sd:/apps/tekken-dr/crash.txt", "wb");
        if (cf) {
            fprintf(cf, "EXC id=%u pc=%08x lr=%08x sp=%08x dar=%08x dsisr=%08x\n",
                    exid, (unsigned)ctx->pc, (unsigned)ctx->lr, (unsigned)ctx->gpr[1],
                    (unsigned)dar, (unsigned)dsisr);
            if (g)
                fprintf(cf, "GUEST ra=%08X sp=%08X r4=%08X r5=%08X r6=%08X r7=%08X r28=%08X r29=%08X\n",
                        g->r[31], g->r[29], g->r[4], g->r[5], g->r[6], g->r[7], g->r[28], g->r[29]);
            else
                fprintf(cf, "GUEST: no current guest thread\n");
            hle_crash_report(cf);
            fclose(cf);
        }
    }
#endif
    for (;;) { }
}

void wii_heartbeat_start(void) {
    PPCExcptCurPanicFn = panic_handler;
#ifdef WII_HEARTBEAT
    void *stack = memalign(32, 16u << 10);
    if (stack) LWP_CreateThread(&s_hb_thread, heartbeat_thread, NULL, stack, 16u << 10, 127);
#endif
}

static void queue_line(const char *line) {
    if (!g_log) return;
    if (g_q_lock != LWP_MUTEX_NULL) LWP_MutexLock(g_q_lock);
    uint32_t head = g_q_head;
    uint32_t next = (head + 1) % LOG_LINES;
    if (next != g_q_tail) {  // when the queue is full the line is dropped
        snprintf(g_queue[head], LOG_LINE_MAX, "%s", line);
        g_q_head = next;
    }
    if (g_q_lock != LWP_MUTEX_NULL) LWP_MutexUnlock(g_q_lock);
}

void wii_log(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    /* The log goes to the SD card only.  Drawing it on the TV console costs
       a full console redraw per line and, worse, the console owns one of the
       game's display buffers. */
    queue_line(line);
}

void wii_fatal(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("\x1b[31m%s\x1b[0m", line);
    wii_video_show_console();  // the console is normally off screen
    queue_line(line);
    if (g_log) {  // a fatal message is the last one; make sure it lands
        storage_lock();
        fputs(line, g_log);
        fflush(g_log);
        storage_unlock();
    }
}
