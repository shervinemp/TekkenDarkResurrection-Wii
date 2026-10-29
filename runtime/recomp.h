// Shared definitions between generated code and the runtime.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef union {
    float f;
    uint32_t u;
    int32_t s;
} FReg;

typedef struct Ctx {
    uint32_t r[32];
    uint32_t hi, lo;
    FReg f[32];
    uint32_t fcr31;
    uint32_t fcc;  // FPU condition bit
    // VFPU: 128 single-precision registers + control registers
    FReg v[128];
    uint32_t vfpu_ctrl[16];  // [0..2] prefixes s/t/d, [3] cc, ...
    uint32_t ic;  // interrupt-enable flag (mfic/mtic)
} Ctx;

typedef void (*RecompFunc)(Ctx *);

/* The guest memory accessors are called once per guest load/store.  They are
   kept small enough to inline: as out-of-line calls they cost a full
   prologue, a byte swap and a scheduler tick each, which dominates the run
   time.  `inline` (not always_inline) keeps the generated code compact; the
   inliner still folds the common case into the caller. */
#if defined(__GNUC__)
#define GUEST_INLINE static inline
/* The pieces of a guest memory access.  At -Os the compiler left even these
   out of line, so a guest load or store was a call that made three more calls
   (the tick, the address check, the write stamp), each with its own frame:
   some 40 instructions where a dozen do. */
#define GUEST_ALWAYS static inline __attribute__((always_inline))
#else
#define GUEST_INLINE static inline
#define GUEST_ALWAYS static inline
#endif

// Guest memory.  The PSP's main RAM is 0x08000000..0x09FFFFFF (32 MiB): the
// kernel partition is 0x08000000..0x087FFFFF and the user partition, which
// the game actually sees, is 0x08800000..0x09FFFFFF (24 MiB).  EDRAM is a
// separate 2 MiB window at 0x04000000.  The Wii has no 160 MiB flat window,
// so the used ranges are carved out of MEM2 and the rest of the PSP address
// space aliases the low guard page.
//
// The window has to span the whole 32 MiB: the game is loaded at 0x08804000
// and allocates from the user partition above it, so a window that stops at
// 0x0A000000 would leave it with a few MiB where a real PSP gives it 24.
// A game that cannot satisfy its own heap walks its free list forever, which
// looks like a hang rather than an allocation failure.
//
// A PSP-2000/3000 also exposes 0x0A000000..0x0A1FFFFFF as "extended" user
// RAM, which some titles use.  Addresses in that range are mapped into the
// low scratch region, one byte per guest byte, so distinct guest addresses
// never alias onto each other: a table read through one of them returns
// the bytes written to it rather than unrelated data, and a stray
// pointer cannot turn into a wild branch.
#define GUEST_RAM_BASE  0x08000000u
#define GUEST_RAM_SIZE  0x02000000u
#define GUEST_VRAM_BASE 0x04000000u
#define GUEST_VRAM_SIZE 0x00200000u
extern uint8_t *g_mem;
extern uint8_t *g_vram;
extern uint8_t *g_low;

#ifdef __wii__
/* Addresses outside the user RAM window: the VRAM window, or a scratch region
   for the "extended" user RAM that a PSP-2000/3000 exposes at
   0x0A000000..0x0A1FFFFFF.  The scratch region covers that span one byte
   per guest byte, so distinct guest addresses map to distinct host bytes
   instead of aliasing onto each other: a table read through one of them then
   returns the bytes written to it, rather than unrelated data that the guest
   goes on to use as a function pointer.  It must not index beyond g_low, or
   it would corrupt Wii-side libogc data. */
#define GUEST_LOW_BASE  0x0A000000u
#define GUEST_LOW_SIZE  0x00200000u
#define GUEST_MEM_SIZE  GUEST_RAM_SIZE

uint8_t *host_slow(uint32_t a);

/* The PSP decodes an address's top three bits as a segment, not as address
   lines: 0x4xxxxxxx is the uncached mirror of 0x0xxxxxxx, and 0x8xxxxxxx /
   0xAxxxxxxx are the kernel and kernel-uncached views.  Games build DMA and
   I/O buffers through those mirrors (0x48800000 is user RAM, 0x44000000 is
   VRAM), so every helper below resolves the segment first.  Treating a
   mirrored address as unmapped silently dropped the data a file read was
   asked to deliver. */
static inline uint32_t guest_norm(uint32_t a) { return a & 0x1FFFFFFFu; }

/* Number of bytes starting at guest address `a` that lie inside a mapped
   window.  Guest code hands us raw pointers and lengths; without this check a
   length that runs off the end of the window writes straight into the host
   heap, where libogc keeps its EXI/IRQ tables.  A copy is clamped to this
   span instead of overflowing. */
static inline uint32_t guest_span(uint32_t a, uint32_t len) {
    if (len == 0) return 0;
    a = guest_norm(a);
    uint64_t end = (uint64_t)a + len;
    if (a >= GUEST_RAM_BASE && a < (uint64_t)GUEST_RAM_BASE + GUEST_RAM_SIZE) {
        uint64_t lim = (uint64_t)GUEST_RAM_BASE + GUEST_RAM_SIZE;
        return (uint32_t)((end > lim ? lim : end) - a);
    }
    if (a >= GUEST_VRAM_BASE && a < (uint64_t)GUEST_VRAM_BASE + GUEST_VRAM_SIZE) {
        uint64_t lim = (uint64_t)GUEST_VRAM_BASE + GUEST_VRAM_SIZE;
        return (uint32_t)((end > lim ? lim : end) - a);
    }
    if (a >= GUEST_LOW_BASE && a < (uint64_t)GUEST_LOW_BASE + GUEST_LOW_SIZE) {
        uint64_t lim = (uint64_t)GUEST_LOW_BASE + GUEST_LOW_SIZE;
        return (uint32_t)((end > lim ? lim : end) - a);
    }
    return 0;
}

/* Raw host pointer for a guest range that lies entirely inside one mapped
   window, or NULL if the range straddles a window boundary.  Used by the
   scanout to read a framebuffer in bulk instead of one guest memory access
   per pixel (each of those would tick the scheduler). */
static inline uint8_t *guest_host_ptr(uint32_t a, uint32_t len) {
    a = guest_norm(a);
    uint64_t end = (uint64_t)a + len;
    if (a >= GUEST_RAM_BASE && end <= (uint64_t)GUEST_RAM_BASE + GUEST_RAM_SIZE)
        return g_mem + (a - GUEST_RAM_BASE);
    if (a >= GUEST_VRAM_BASE && end <= (uint64_t)GUEST_VRAM_BASE + GUEST_VRAM_SIZE)
        return g_vram + (a - GUEST_VRAM_BASE);
    if (a >= GUEST_LOW_BASE && end <= (uint64_t)GUEST_LOW_BASE + GUEST_LOW_SIZE)
        return g_low + (a - GUEST_LOW_BASE);
    return NULL;
}

/* Host pointer to the start of the window containing `a`, together with the
   number of bytes still available in that window.  A caller that only needs
   "the rest of this window" uses this; guest_host_ptr() above needs an
   exact length. */
static inline uint8_t *guest_window(uint32_t a, uint32_t *avail) {
    a = guest_norm(a);
    if (a >= GUEST_RAM_BASE && a < GUEST_RAM_BASE + GUEST_RAM_SIZE) {
        *avail = GUEST_RAM_BASE + GUEST_RAM_SIZE - a;
        return g_mem + (a - GUEST_RAM_BASE);
    }
    if (a >= GUEST_VRAM_BASE && a < GUEST_VRAM_BASE + GUEST_VRAM_SIZE) {
        *avail = GUEST_VRAM_BASE + GUEST_VRAM_SIZE - a;
        return g_vram + (a - GUEST_VRAM_BASE);
    }
    *avail = 0;
    return NULL;
}

/* Fast path for the common case (a load or store inside the user RAM
   window); anything else goes through the general out-of-line version.  The
   generated code does millions of these per frame, so it has to be a couple of
   instructions, not a call. */
GUEST_ALWAYS uint8_t *host_fast(uint32_t a) {
    if (a >= GUEST_RAM_BASE && a < GUEST_RAM_BASE + GUEST_RAM_SIZE)
        return g_mem + (a - GUEST_RAM_BASE);
    return host_slow(a);
}
#else
/* The host build maps the whole 32-bit space into one flat buffer. */
#define GUEST_MEM_SIZE 0x0A000000u

static inline uint32_t guest_norm(uint32_t a) { return a & 0x1FFFFFFFu; }
GUEST_INLINE uint8_t *host_fast(uint32_t a) { return g_mem + guest_norm(a); }
static inline uint32_t guest_span(uint32_t a, uint32_t len) { (void)a; return len; }
static inline uint8_t *guest_host_ptr(uint32_t a, uint32_t len) { (void)len; return g_mem + guest_norm(a); }
static inline uint8_t *guest_window(uint32_t a, uint32_t *avail) {
    *avail = 0x10000000u;
    return g_mem + guest_norm(a);
}
#endif

/* General entry point for callers that are not on the hot path. */
static inline uint8_t *host(uint32_t a) { return host_fast(a); }

/* The PSP is little-endian; the Wii's PowerPC is big-endian.  Guest memory
   therefore has to be swapped on the way in and out on Wii.  Byte accesses
   and raw memcpy() of byte strings are already endian-independent. */
#ifdef __wii__
void guest_tick(void);
/* Guest code is recompiled into ordinary host functions.  A long guest loop
   can therefore run without reaching an HLE call, starving the frontend and
   the other cooperative host threads.  Make memory traffic a cheap
   scheduling point; this is the Wii's equivalent of a timer interrupt at a
   safe C boundary.  The counter is a single global so the check is one load
   and one branch, not a per-function static. */
/* Only the guest thread updates this counter; it is a scheduling hint, not
   cross-thread state.  Keeping it non-volatile lets the compiler keep the
   counter in a register across memory accesses instead of a global store on
   every load and store. */
extern unsigned g_guest_mem_ticks;
/* The scheduling point.  It used to be in every load and store; it is now in
   the generated code at the head of each function and of each loop (the
   places a long-running guest routine has to pass through), so a memory access
   is a leaf with no frame. */
GUEST_ALWAYS void guest_loop_tick(void) {
    if (__builtin_expect((++g_guest_mem_ticks & 0x3FFu) == 0, 0)) {
        __asm__ volatile("" ::: "memory");
        guest_tick();
    }
}
GUEST_ALWAYS void guest_mem_tick(void) {
    if (__builtin_expect((++g_guest_mem_ticks & 0x7FFu) == 0, 0)) {
        /* The generated functions take their register file as `restrict` and
           keep its registers in host registers between calls; guest_tick() may
           switch threads or run a guest interrupt handler on this very
           context, which has to see the registers as they are now. */
        __asm__ volatile("" ::: "memory");
        guest_tick();
    }
}
#else
static inline void guest_mem_tick(void) {}
static inline void guest_loop_tick(void) {}
#endif
#ifdef HLE_TRACE
extern uint64_t g_mem_accesses;  // diagnostics: guest memory traffic
#define MEM_COUNT() (g_mem_accesses++)
#else
#define MEM_COUNT() ((void)0)
#endif
/* TKDR_WATCH (debug): every guest store stamps its 256-byte block with the frame
   it happened in and the place it came from, so a draw's vertices can be asked
   who wrote them and when. */
#if defined(TKDR_WATCH) && defined(__wii__)
extern uint32_t *g_wtp_ep, *g_wtp_ra;
extern uint32_t g_wtp_cur;
static inline __attribute__((always_inline)) void wtp_mark(uint32_t a, uint32_t ra) {
    uint32_t off = a - GUEST_RAM_BASE;
    if (off < GUEST_RAM_SIZE) { g_wtp_ep[off >> 8] = g_wtp_cur; g_wtp_ra[off >> 8] = ra; }
}
#define STORE_WATCH(a_) wtp_mark((a_), (uint32_t)(uintptr_t)__builtin_return_address(0))
#define STORE_WATCH_INL(a_) do { uint32_t pc_; __asm__ volatile("bl 1f\n1: mflr %0" : "=r"(pc_) :: "lr"); wtp_mark((a_), pc_); } while (0)
#else
#define STORE_WATCH(a_) ((void)0)
#define STORE_WATCH_INL(a_) ((void)0)
#endif
#ifdef TKDR_STORECOUNT
extern unsigned g_guest_stores;   /* diagnostics: guest stores per frame (about 50k in a fight) */
#define STORE_COUNT() (g_guest_stores++)
#else
#define STORE_COUNT() ((void)0)
#endif
/* The generated code calls these once per guest load/store, so they must be
   inlined: as out-of-line calls they cost a full prologue, a byte swap and a
   scheduler tick each, which dominates the run time. */
GUEST_INLINE uint32_t mem_r32(uint32_t a) {
    MEM_COUNT();
    uint32_t v; memcpy(&v, host_fast(a), 4);
#ifdef __wii__
    v = __builtin_bswap32(v);
#endif
    return v;
}
GUEST_INLINE uint16_t mem_r16(uint32_t a) {
    MEM_COUNT();
    uint16_t v; memcpy(&v, host_fast(a), 2);
#ifdef __wii__
    v = __builtin_bswap16(v);
#endif
    return v;
}
GUEST_INLINE uint8_t mem_r8(uint32_t a) { MEM_COUNT(); return *host_fast(a); }
GUEST_INLINE void mem_w32(uint32_t a, uint32_t v) {
    MEM_COUNT();
    STORE_COUNT();
    STORE_WATCH(a);
#ifdef __wii__
    v = __builtin_bswap32(v);
#endif
    memcpy(host_fast(a), &v, 4);
}
GUEST_INLINE void mem_w16(uint32_t a, uint16_t v) {
    MEM_COUNT();
    STORE_COUNT();
    STORE_WATCH(a);
#ifdef __wii__
    v = __builtin_bswap16(v);
#endif
    memcpy(host_fast(a), &v, 2);
}
GUEST_INLINE void mem_w8(uint32_t a, uint8_t v) { MEM_COUNT(); STORE_COUNT(); STORE_WATCH(a); *host_fast(a) = v; }

/* The same accessors expanded in place.  The generated code uses them for the
   instructions inside loops, where a call's argument setup and the reloads of
   guest registers after it cost more than the access; everywhere else a call to
   the shared copy keeps the code small (the instruction cache is 32 KiB). */
GUEST_ALWAYS uint32_t memi_r32(uint32_t a) {
    uint32_t v; memcpy(&v, host_fast(a), 4);
#ifdef __wii__
    v = __builtin_bswap32(v);
#endif
    return v;
}
GUEST_ALWAYS uint16_t memi_r16(uint32_t a) {
    uint16_t v; memcpy(&v, host_fast(a), 2);
#ifdef __wii__
    v = __builtin_bswap16(v);
#endif
    return v;
}
GUEST_ALWAYS uint8_t memi_r8(uint32_t a) { return *host_fast(a); }
GUEST_ALWAYS void memi_w32(uint32_t a, uint32_t v) {
    STORE_WATCH_INL(a);
#ifdef __wii__
    v = __builtin_bswap32(v);
#endif
    memcpy(host_fast(a), &v, 4);
}
GUEST_ALWAYS void memi_w16(uint32_t a, uint16_t v) {
    STORE_WATCH_INL(a);
#ifdef __wii__
    v = __builtin_bswap16(v);
#endif
    memcpy(host_fast(a), &v, 2);
}
GUEST_ALWAYS void memi_w8(uint32_t a, uint8_t v) { STORE_WATCH_INL(a); *host_fast(a) = v; }

// Unaligned word access (lwl/lwr/swl/swr), little-endian semantics.
static inline uint32_t do_lwl(uint32_t rt, uint32_t a) {
    uint32_t sh = (a & 3) * 8, w = mem_r32(a & ~3u);
    return (rt & (0x00FFFFFFu >> sh)) | (w << (24 - sh));
}
static inline uint32_t do_lwr(uint32_t rt, uint32_t a) {
    uint32_t sh = (a & 3) * 8, w = mem_r32(a & ~3u);
    return (rt & ~(0xFFFFFFFFu >> sh)) | (w >> sh);
}
static inline void do_swl(uint32_t rt, uint32_t a) {
    uint32_t sh = (a & 3) * 8, w = mem_r32(a & ~3u);
    mem_w32(a & ~3u, (w & ~(0xFFFFFFFFu >> (24 - sh))) | (rt >> (24 - sh)));
}
static inline void do_swr(uint32_t rt, uint32_t a) {
    uint32_t sh = (a & 3) * 8, w = mem_r32(a & ~3u);
    mem_w32(a & ~3u, (w & ~(0xFFFFFFFFu << sh)) | (rt << sh));
}

static inline void do_div(Ctx *c, int32_t a, int32_t b) {
    if (b == 0) { c->lo = a < 0 ? 1 : 0xFFFFFFFFu; c->hi = (uint32_t)a; }
    else if (a == INT32_MIN && b == -1) { c->lo = (uint32_t)a; c->hi = 0; }
    else { c->lo = (uint32_t)(a / b); c->hi = (uint32_t)(a % b); }
}
static inline void do_divu(Ctx *c, uint32_t a, uint32_t b) {
    if (b == 0) { c->lo = a <= 0xFFFFu ? 0xFFFFu : 0xFFFFFFFFu; c->hi = a; }
    else { c->lo = a / b; c->hi = a % b; }
}
static inline void acc_add(Ctx *c, int64_t v) {
    int64_t acc = (int64_t)(((uint64_t)c->hi << 32) | c->lo) + v;
    c->hi = (uint32_t)((uint64_t)acc >> 32); c->lo = (uint32_t)acc;
}

static inline uint32_t clz32(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32; }
static inline uint32_t bitrev32(uint32_t x) {
    x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
    x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
    x = ((x >> 4) & 0x0F0F0F0Fu) | ((x & 0x0F0F0F0Fu) << 4);
    return __builtin_bswap32(x);
}

// float -> int32 with MIPS saturation; mode 0 nearest, 1 trunc, 2 ceil, 3 floor.
// The GameCube/Wii has no double hardware, and a float-to-int conversion is a
// single fctiwz instruction.  Build the four rounding modes from that cast
// instead of calling the double/float libm routines: the generated menu/UI
// code performs many of these conversions per frame.
static inline int32_t f2i(float f, int mode) {
    if (isnan(f)) return 0x7FFFFFFF;
    /* 2^31 is the first float outside the representable int32 range. */
    if (f >= 2147483648.0f) return 0x7FFFFFFF;
    if (f <= -2147483648.0f) return (int32_t)0x80000000;
    int32_t i = (int32_t)f;          // truncate toward zero
    if (mode == 1) return i;
    float frac = f - (float)i;
    if (mode == 3) return frac < 0.f ? i - 1 : i;       // floor
    if (mode == 2) return frac > 0.f ? i + 1 : i;       // ceil
    /* Round to nearest, ties to even, matching the MIPS default mode. */
    if (frac > 0.5f || (frac == 0.5f && (i & 1))) return i + 1;
    if (frac < -0.5f || (frac == -0.5f && (i & 1))) return i - 1;
    return i;
}
static inline int fcmp(float a, float b, int cond) {
    int un = isnan(a) || isnan(b);
    return ((cond & 1) && un) || ((cond & 2) && !un && a == b) || ((cond & 4) && !un && a < b);
}

// Provided by the runtime.
void recomp_probe(Ctx *c, uint32_t func);
void recomp_probe_ret(Ctx *c, uint32_t func);
void check_saved(Ctx *c, const uint32_t *saved, uint32_t func, uint32_t at);
void call_indirect(Ctx *c, uint32_t addr);
void hle_call(Ctx *c, uint32_t import_index);
void vfpu_exec(Ctx *c, uint32_t pc, uint32_t insn);

/* VFPU instructions translated to straight-line C (recomp/vfpu_emit.py) use
   these.  The dot products add the products to 0.f in order, the way the
   interpreter does, so that the results agree bit for bit. */
static inline float vfpu_clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline float vfpu_bits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
#define vfpu_dot1(a) (0.f + (a))
#define vfpu_dot2(a, b) ((0.f + (a)) + (b))
#define vfpu_dot3(a, b, c_) (((0.f + (a)) + (b)) + (c_))
#define vfpu_dot4(a, b, c_, d) ((((0.f + (a)) + (b)) + (c_)) + (d))
float vfpu_qsin(float x);   // angles are in quarter turns
float vfpu_qcos(float x);
void vfpu_lvq(Ctx *c, uint32_t addr, int i0, int i1, int i2, int i3);
void vfpu_svq(Ctx *c, uint32_t addr, int i0, int i1, int i2, int i3);
/* vf2in / vf2iz / vf2iu / vf2id: float to fixed point with a scale of 2^sh. */
static inline uint32_t vfpu_f2i(float x, int sh, int mode) {
    if (isnan(x)) return 0x7FFFFFFFu;
    double v = (double)x * ldexp(1.0, sh), q;
    if (v > 2147483647.0) return 0x7FFFFFFFu;
    if (v <= -2147483648.0) return 0x80000000u;
    switch (mode) {
    case 0: q = nearbyint(v); break;
    case 1: q = trunc(v); break;
    case 2: q = ceil(v); break;
    default: q = floor(v); break;
    }
    return (uint32_t)(int32_t)q;
}
#define vfpu_f2in(x, sh) vfpu_f2i(x, sh, 0)
#define vfpu_f2iz(x, sh) vfpu_f2i(x, sh, 1)
#define vfpu_f2iu(x, sh) vfpu_f2i(x, sh, 2)
#define vfpu_f2id(x, sh) vfpu_f2i(x, sh, 3)
void recomp_trap(Ctx *c, uint32_t pc, const char *why);
#ifdef EXP_OVRCHK
void recomp_ovr_check(const char *name, const Ctx *native, const Ctx *original);   /* overrides.c */
#endif
