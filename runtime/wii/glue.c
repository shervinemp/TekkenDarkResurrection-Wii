/* Glue between the static recompiler's generated code and the Wii host.
   Mirrors runtime/main.c, minus SDL, signals, headless tooling and the web
   dynamic-code path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "func_table.h"
#include "gpu/gpu.h"
#include "hle/hle.h"
#include "recomp.h"
#include "wii_platform.h"

extern uint32_t g_buttons;
extern uint8_t g_analog_x, g_analog_y;

static RecompFunc find_func(uint32_t addr) {
    addr &= 0x1FFFFFFFu;
    unsigned lo = 0, hi = g_func_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_funcs[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_func_count && g_funcs[lo].addr == addr ? g_funcs[lo].fn : NULL;
}

void recomp_trap(Ctx *c, uint32_t pc, const char *why) {
    wii_fatal("TRAP at %08X: %s (ra=%08X sp=%08X)\n", pc, why, c->r[31], c->r[29]);
    /* Keep the screen on the last frame and stop.  Resetting would hide the
       diagnostic; the log on the SD card carries the full line. */
    for (;;) {
        wii_input_scan();
        wii_sleep_us(100000);
    }
}

void recomp_call_miss(Ctx *c, uint32_t addr) {
    for (unsigned k = 0; k < g_import_count; k++)
        if (g_imports[k].stub == (addr & 0x1FFFFFFFu)) {
            hle_call(c, k);
            return;
        }
    recomp_trap(c, addr, "indirect call to unknown address");
}

uint32_t g_trace_ra;

/* Guest PC histogram.  A recompiled guest function can spin for a long time
   without making an HLE call, so the HLE trace alone does not show where the
   guest spends its time.  Sampling the guest's own return address on every
   call gives a picture of the hot code. */
#ifdef HLE_TRACE
#define PC_SAMPLES 2048
static uint32_t g_pc_addr[PC_SAMPLES];
static uint32_t g_pc_count[PC_SAMPLES];
static uint32_t g_pc_hist_n;

/* The guest function currently executing.  The frontend samples it every
   frame, so the histogram is weighted by time rather than by call count:
   a guest spin loop shows up even though it makes few calls. */
static uint32_t g_cur_fn;
static unsigned g_guest_depth;
static uint64_t g_guest_us;

static void pc_sample(uint32_t pc) {
    for (uint32_t i = 0; i < g_pc_hist_n; i++)
        if (g_pc_addr[i] == pc) { g_pc_count[i]++; return; }
    if (g_pc_hist_n < PC_SAMPLES) {
        g_pc_addr[g_pc_hist_n] = pc;
        g_pc_count[g_pc_hist_n] = 1;
        g_pc_hist_n++;
    }
}

// Called by the frontend once per frame: attribute the time since the last
// sample to the guest function that was running.  A thread blocked in an HLE
// wait is not running guest code, so do not charge its wait time to the
// function that made the call.
void hle_sample_guest_fn(void) {
    if (hle_guest_running()) pc_sample(g_cur_fn);
}

// Called from the guest thread's own scheduling point: a time-weighted
// sample of where the guest is executing.  The frontend is often starved
// while the guest runs, so it cannot sample often enough on its own.
void hle_sample_guest_pc(uint32_t pc) { pc_sample(pc); }

void hle_dump_pc_hist(void) {
    fprintf(stderr, "--- guest functions (%u distinct) ---\n", g_pc_hist_n);
    for (unsigned k = 0; k < 12 && k < g_pc_hist_n; k++) {
        uint32_t best = 0, best_i = 0;
        for (uint32_t i = 0; i < g_pc_hist_n; i++)
            if (g_pc_count[i] > best) { best = g_pc_count[i]; best_i = i; }
        if (!best) break;
        fprintf(stderr, "  fn %08X x%u\n", g_pc_addr[best_i], best);
        g_pc_count[best_i] = 0;
    }
    fprintf(stderr, "  guest compute total: %llu ms\n", (unsigned long long)(g_guest_us / 1000));
}
#else
void hle_dump_pc_hist(void) { }
void hle_sample_guest_fn(void) { }
void hle_sample_guest_pc(uint32_t pc) { }
#endif

void call_indirect(Ctx *c, uint32_t addr) {
    /* The EBOOT's module teardown calls a callback through a pointer that is
       null in this recompilation (the module manager's export was not
       loaded).  A real call would branch to zero and take the host down;
       treat it as an optional callback instead. */
    if (!addr) return;
    RecompFunc fn = find_func(addr);
    if (fn) {
        static int logged;
        if (!logged) {
            logged = 1;
            wii_log("WII recomp: first dispatch %08X (ra=%08X sp=%08X)\n",
                    addr, c->r[31], c->r[29]);
        }
        uint32_t ra = c->r[31];
        hle_call_depth_enter();
#ifdef HLE_TRACE
        uint32_t prev = g_cur_fn;
        g_cur_fn = addr;
        uint64_t t0 = hle_time_us();
        int top = (g_guest_depth == 0);
        g_guest_depth++;
        fn(c);
        g_guest_depth--;
        if (top) g_guest_us += hle_time_us() - t0;
        g_cur_fn = prev;
#else
        fn(c);
#endif
        hle_call_depth_leave();
        (void)ra;
        return;
    }
    recomp_call_miss(c, addr);
}

/* Addresses outside the user RAM window: the VRAM window, or the scratch
   region for the kernel-only and extended-RAM addresses.  The scratch region
   covers 0x0A000000..0x0A1FFFFFF (the "extended" user RAM of a PSP-2000 or
   PSP-3000) plus the 0x08000000..0x087FFFFF kernel partition, so distinct
   guest addresses map to distinct host bytes.  Aliasing them would make a
   table read through one address return unrelated data, which the guest
   then uses as a function pointer. */
/* Guest addresses that fall outside every mapped window.  The PSP's own
   kernel lives at 0x00000000..0x001FFFFF and its I/O registers at
   0x1F000000.., so the guest does touch addresses we cannot back with real
   memory.  Those reads must not silently return another address' bytes, or
   a bad pointer turns into a wild branch further down the line. */
#ifdef HLE_TRACE
static uint32_t g_bad_access_count;
static uint32_t g_last_bad_access;
#endif

static uint8_t *scratch_fallback(uint32_t a) {
#ifdef HLE_TRACE
    g_bad_access_count++;
    g_last_bad_access = a;
    if (g_bad_access_count <= 32)
        fprintf(stderr, "guest: unmapped access at %08X\n", a);
#endif
    /* A stable slice per address (modulo the scratch size), so a bad
       pointer reads back what was written through it instead of whatever
       happens to be nearby. */
    return g_low + (a % GUEST_LOW_SIZE);
}
uint8_t *host_slow(uint32_t a) {
    a = guest_norm(a);
    /* The fast path only recognises the canonical user RAM addresses; the
       uncached (0x4xxxxxxx) and kernel (0x8xxxxxxx) mirrors of it land
       here after the segment bits are dropped. */
    if (a >= GUEST_RAM_BASE && a < GUEST_RAM_BASE + GUEST_RAM_SIZE)
        return g_mem + (a - GUEST_RAM_BASE);
    if (a >= GUEST_VRAM_BASE && a < GUEST_VRAM_BASE + GUEST_VRAM_SIZE)
        return g_vram + (a - GUEST_VRAM_BASE);
    if (a >= GUEST_LOW_BASE && a < GUEST_LOW_BASE + GUEST_LOW_SIZE)
        return g_low + (a - GUEST_LOW_BASE);
    return scratch_fallback(a);
}

unsigned g_guest_mem_ticks;
#ifdef TKDR_AUTOPILOT
unsigned g_guest_stores;   /* counted with -DTKDR_STORECOUNT */
#endif

/* Diagnostics: guest accesses that fell outside every mapped window. */
#ifdef HLE_TRACE
static uint32_t g_bad_access_count;
static uint32_t g_last_bad_access;
uint32_t guest_bad_access_count(void) { return g_bad_access_count; }
uint32_t guest_last_bad_access(void) { return g_last_bad_access; }

/* Diagnostics: time the frontend spends presenting a frame. */
uint64_t g_present_us;
#endif

void recomp_probe(Ctx *c, uint32_t func) {
    (void)c;
    (void)func;
}

void recomp_probe_ret(Ctx *c, uint32_t func) {
    (void)c;
    (void)func;
}

void check_saved(Ctx *c, const uint32_t *saved, uint32_t func, uint32_t at) {
    (void)c; (void)saved; (void)func; (void)at;
}

void script_tick(void) {
    /* The native build's INPUT script is a debugging aid; the Wii uses real
       controllers.  Kept as a no-op so the frontend can share the frame path. */
}

void web_persist(void) {
    /* Native/web save flush hook; files are already written to FAT/NTFS. */
}

/* Copy the guest framebuffer out for display.

   The pixels are handed on in the guest's own 16-bit format, so the video
   layer does a single conversion into the GX texture.  Converting here as
   well (to 32-bit ARGB) doubled the per-frame work: 130k pixels converted
   twice, and GX_CopyDisp then waited for it.

   The guest writes big-endian words, so they are byte-swapped on the way
   out.  A 32-bit framebuffer (format 3) is narrowed to 16 bits here,
   because the GX texture holds one 16-bit pixel per texel. */
void frontend_scanout(uint16_t *out) {
    uint32_t base = g_display.fb_addr, stride = g_display.stride;
    int fmt = g_display.format;
    /* Read the framebuffer in bulk.  Going through mem_r32() per pixel costs
       130k guest memory accesses per frame, and every one of them ticks the
       scheduler, which is what starves the game of CPU. */
    uint32_t bpp = (fmt == 3) ? 4 : 2;
    uint64_t need = (uint64_t)stride * 272 * bpp;
    uint8_t *src = need <= 0x1000000u ? guest_host_ptr(base, (uint32_t)need) : NULL;
    if (src) {
        for (int y = 0; y < 272; y++) {
            const uint8_t *row = src + (uint64_t)y * stride * bpp;
            uint16_t *dst = out + y * 480;
            if (fmt == 3) {
                /* 32-bit framebuffer: narrow to 16 bits.  The video layer
                   converts to RGB565, so hand it the same 16-bit layout the
                   guest would have written: R in the top 5 bits, G in the
                   middle 6, B in the low 5. */
                /* The guest word is A B G R from the top byte down, so red
                   is bits 3..7 (this used to take blue for red and swap
                   the two channels). */
                const uint32_t *p = (const uint32_t *)row;
                for (int x = 0; x < 480; x++) {
                    uint32_t v = __builtin_bswap32(p[x]);
                    dst[x] = (uint16_t)(((v >> 3) & 0x1F) << 11 |
                                        ((v >> 10) & 0x3F) << 5 |
                                        ((v >> 19) & 0x1F));
                }
            } else if (fmt == 0) {
                /* 16-bit 565 with red in the low bits. */
                const uint16_t *p = (const uint16_t *)row;
                for (int x = 0; x < 480; x++) {
                    uint32_t v = __builtin_bswap16(p[x]);
                    dst[x] = (uint16_t)(((v & 31) << 11) | (((v >> 5) & 63) << 5) |
                                        ((v >> 11) & 31));
                }
            } else if (fmt == 1) {
                /* 16-bit 5551 with red in the low bits.  Green is five bits
                   here and six in RGB565, so it is doubled rather than
                   truncated. */
                const uint16_t *p = (const uint16_t *)row;
                for (int x = 0; x < 480; x++) {
                    uint32_t v = __builtin_bswap16(p[x]);
                    dst[x] = (uint16_t)(((v & 31) << 11) | (((v >> 5) & 31) << 6) |
                                        ((v >> 10) & 31));
                }
            } else {
                /* 16-bit 4444: red and blue double, green quadruples. */
                const uint16_t *p = (const uint16_t *)row;
                for (int x = 0; x < 480; x++) {
                    uint32_t v = __builtin_bswap16(p[x]);
                    dst[x] = (uint16_t)(((v & 15) << 12) | (((v >> 4) & 15) << 7) |
                                        (((v >> 8) & 15) << 1));
                }
            }
        }
        return;
    }
    /* Slow path (the framebuffer straddles a window): the same RGB565 as
       above.  It used to store r | g << 8 truncated to 16 bits, which is
       not a pixel format the video layer reads. */
    for (int y = 0; y < 272; y++) {
        for (int x = 0; x < 480; x++) {
            uint32_t a = base + (uint32_t)(y * (int)stride + x) * bpp;
            uint32_t v = (fmt == 3) ? mem_r32(a) : mem_r16(a);
            uint32_t r, g, b;
            if (fmt == 3) {
                r = v & 255; g = (v >> 8) & 255; b = (v >> 16) & 255;
            } else if (fmt == 0) {
                r = (v & 31) << 3; g = ((v >> 5) & 63) << 2; b = ((v >> 11) & 31) << 3;
            } else if (fmt == 1) {
                r = (v & 31) << 3; g = ((v >> 5) & 31) << 3; b = ((v >> 10) & 31) << 3;
            } else {
                r = (v & 15) << 4; g = ((v >> 4) & 15) << 4; b = ((v >> 8) & 15) << 4;
            }
            out[y * 480 + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}
