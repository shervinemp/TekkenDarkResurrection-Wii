// GE display list processor. Like the real hardware it runs in parallel with
// the CPU: lists are queued and executed in order by a dedicated host thread.
// Finish/signal callbacks are raised as "interrupts" that the scheduler runs
// on whichever guest thread is current (see hle_run_interrupts).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle/hle.h"
#include "glr.h"
#include "gpu.h"

GeState g_ge;
uint32_t g_ge_vertex_gen = 1;
uint32_t g_ge_light_gen = 1;
/* Changes whenever a register that the rasteriser's per-primitive pixel
   state (target, texture, tests, blend, masks, fog colour) depends on is
   written, or the texture memory it describes may have changed.  The
   rasteriser rebuilds that state only when this moves: a fight issues about
   5000 draws a frame, and most of them change nothing but bone matrices and
   vertex pointers. */
uint32_t g_ge_pix_gen = 1;
uint32_t g_ge_view_gen = 1;
uint32_t g_ge_world_gen = 1;   // changes when the world matrix is written

/* Registers that only feed the vertex stage (transform, lighting, texture
   coordinate scaling, viewport) and so leave the pixel state alone.  The
   vertex type is here too: the one bit of it the pixel state uses (through
   mode) is compared separately. */
static int vertex_only_op(uint32_t op) {
    if (op == GE_VERTEXTYPE || op == GE_SHADEMODE || op == GE_REVERSENORMAL) return 1;
    if (op == GE_OFFSETADDR || op == GE_ORIGIN) return 1;          // vertex source/offset
    if (op == GE_BONEMATRIXNUMBER || op == GE_BONEMATRIXDATA) return 1;
    if (op >= GE_LIGHTINGENABLE && op <= 0x1C) return 1;          // lighting, light enables
    if (op >= GE_MORPHWEIGHT0 && op < GE_MORPHWEIGHT0 + 8) return 1;
    if (op >= GE_VIEWPORTXSCALE && op <= GE_OFFSETY) return 1;     // viewport, tex scale/offset, offsets
    if (op >= GE_MATERIALUPDATE && op <= GE_LAC0 + 11) return 1;    // material and light registers
    return 0;
}

static int light_state_op(uint32_t op) {
    if (op == GE_LIGHTINGENABLE || op == GE_REVERSENORMAL) return 1;
    if (op >= GE_LIGHTENABLE0 && op <= 0x1C) return 1;
    if (op >= GE_MATERIALUPDATE && op <= GE_LAC0 + 11) return 1;   // material, light types/positions/colours
    return 0;
}

/* A hash of the values of the registers light_state_op() lists, which
   changes when the lighting does and, unlike g_ge_light_gen, comes back
   when the same values do.  Worked out again only when the generation moved. */
uint32_t ge_light_sig(void) {
    static uint32_t gen, sig;
    if (gen == g_ge_light_gen && gen) return sig;
    gen = g_ge_light_gen;
    uint32_t h = 2166136261u;
    for (uint32_t op = 0; op < 256; op++)
        if (light_state_op(op)) h = (h ^ g_ge.cmd[op]) * 16777619u;
    sig = h;
    return sig;
}

/* The same for vertices without normals.  A zero normal makes every
   directional light's diffuse and specular terms zero whatever the light's
   direction and colour (the direction changes with the camera in a fight),
   so only what the lighting adds without them is in this signature:
   enables, kinds, the ambient colours, and the material and global ambient.
   A point or spot light also depends on where the vertex is, so its whole
   state is in. */
uint32_t ge_light_sig_flat(void) {
    static uint32_t gen, sig;
    if (gen == g_ge_light_gen && gen) return sig;
    gen = g_ge_light_gen;
    const uint32_t *r = g_ge.cmd;
    uint32_t h = 2166136261u;
#define MIX(v_) (h = (h ^ (v_)) * 16777619u)
    MIX(r[GE_LIGHTINGENABLE]); MIX(r[GE_MATERIALUPDATE]); MIX(r[GE_MATERIALEMISSIVE]);
    MIX(r[GE_MATERIALAMBIENT]); MIX(r[GE_MATERIALALPHA]); MIX(r[GE_AMBIENTCOLOR]); MIX(r[GE_AMBIENTALPHA]);
    MIX(r[GE_LIGHTMODE]);
    for (int l = 0; l < 4; l++) {
        uint32_t type = r[GE_LIGHTTYPE0 + l];
        MIX(r[GE_LIGHTENABLE0 + l]); MIX(type); MIX(r[GE_LAC0 + 3 * l]);
        if (!(r[GE_LIGHTENABLE0 + l] & 1)) continue;
        if ((type >> 8 & 3) != 0) {   // point or spot
            for (int i = 0; i < 3; i++) { MIX(r[GE_LX0 + 3 * l + i]); MIX(r[GE_LDX0 + 3 * l + i]); MIX(r[GE_LKA0 + 3 * l + i]); }
            MIX(r[GE_LKO0 + l]); MIX(r[GE_LKS0 + l]);
        }
        if ((type & 3) == 2) {   // powered diffuse: a zero exponent makes even a zero dot product count
            MIX(r[GE_MATERIALSPECULARCOEF]); MIX(r[GE_LAC0 + 3 * l + 1]);
        }
    }
#undef MIX
    sig = h;
    return sig;
}

/* ge_light_sig_flat() without the global ambient alpha.  A stage fades a group of draws by moving that alpha every frame;
   it reaches the vertices only as the alpha byte (vc_alpha_refresh() in raster.c), so the draw cache keeps the draws apart
   by this signature and the alpha, and makes the alpha again on a draw that differs only by it. */
uint32_t ge_light_sig_flat_noalpha(void) {
    static uint32_t gen, sig;
    if (gen == g_ge_light_gen && gen) return sig;
    gen = g_ge_light_gen;
    const uint32_t *r = g_ge.cmd;
    uint32_t h = 2166136261u;
#define MIX(v_) (h = (h ^ (v_)) * 16777619u)
    MIX(r[GE_LIGHTINGENABLE]); MIX(r[GE_MATERIALUPDATE]); MIX(r[GE_MATERIALEMISSIVE]);
    MIX(r[GE_MATERIALAMBIENT]); MIX(r[GE_MATERIALALPHA]); MIX(r[GE_AMBIENTCOLOR]);
    MIX(r[GE_LIGHTMODE]);
    for (int l = 0; l < 4; l++) {
        uint32_t type = r[GE_LIGHTTYPE0 + l];
        MIX(r[GE_LIGHTENABLE0 + l]); MIX(type); MIX(r[GE_LAC0 + 3 * l]);
        if (!(r[GE_LIGHTENABLE0 + l] & 1)) continue;
        if ((type >> 8 & 3) != 0) {
            for (int i = 0; i < 3; i++) { MIX(r[GE_LX0 + 3 * l + i]); MIX(r[GE_LDX0 + 3 * l + i]); MIX(r[GE_LKA0 + 3 * l + i]); }
            MIX(r[GE_LKO0 + l]); MIX(r[GE_LKS0 + l]);
        }
        if ((type & 3) == 2) { MIX(r[GE_MATERIALSPECULARCOEF]); MIX(r[GE_LAC0 + 3 * l + 1]); }
    }
#undef MIX
    sig = h;
    return sig;
}

/* Registers that change what decode_vertex() produces.  Texture, blend,
   scissor and framebuffer changes deliberately do not invalidate the decoded
   vertex cache. */
static int vertex_state_op(uint32_t op) {
    if (op == GE_VERTEXTYPE || op == GE_BASE || op == GE_OFFSETADDR || op == GE_ORIGIN) return 1;
    if (op == GE_LIGHTINGENABLE || (op >= GE_LIGHTENABLE0 && op <= 0x1C)) return 1;
    if (op >= GE_BONEMATRIXNUMBER && op <= GE_MORPHWEIGHT0 + 7) return 1;
    if (op >= GE_WORLDMATRIXNUMBER && op <= GE_TGENMATRIXDATA) return 1;
    if (op >= GE_TEXSCALEU && op <= GE_TEXOFFSETV) return 1;
    if (op == GE_TEXMAPMODE) return 1;   // vtx_state_refresh() folds a texture matrix in only in its matrix mode
    if (op == GE_REVERSENORMAL) return 1;
    if (op >= GE_MATERIALUPDATE && op <= GE_LAC0 + 11) return 1;
    if (op == GE_FOGENABLE || op == GE_FOG1 || op == GE_FOG2) return 1;
    return 0;
}

typedef struct {
    int used, done, queued;
    uint64_t seq;  // queue order
    uint32_t pc, stall;
    uint32_t stack[32][2];  // CALL return address + saved offset
    int sp;
    int cbid;
    uint32_t arg;
    int started;
} GeList;

#define MAX_LISTS 64
static GeList g_lists[MAX_LISTS];
static uint32_t g_cb_signal, g_cb_signal_arg, g_cb_finish, g_cb_finish_arg;
#ifdef HLE_TRACE
/* Set GE_TRACE=1 to log every list the guest enqueues. */
static int g_ge_trace;
#endif

void gpu_set_callbacks(uint32_t signal_fn, uint32_t signal_arg, uint32_t finish_fn,
                       uint32_t finish_arg) {
    g_cb_signal = signal_fn;
    g_cb_signal_arg = signal_arg;
    g_cb_finish = finish_fn;
    g_cb_finish_arg = finish_arg;
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work = PTHREAD_COND_INITIALIZER;
static uint64_t g_next_seq;

// Pending guest callbacks raised by the GPU thread
#define MAX_INTR 64
static struct { uint32_t fn, a0, a1; } g_intr[MAX_INTR];
static int g_intr_n;

static void raise_interrupt(uint32_t fn, uint32_t a0, uint32_t a1) {
    if (!fn) return;
    pthread_mutex_lock(&g_lock);
    if (g_intr_n < MAX_INTR) {
        g_intr[g_intr_n].fn = fn;
        g_intr[g_intr_n].a0 = a0;
        g_intr[g_intr_n].a1 = a1;
        g_intr_n++;
    }
    pthread_mutex_unlock(&g_lock);
    sched_wake_idle();  // the callbacks run on a guest thread
}

void hle_intr_enter(void);
void hle_intr_leave(void);

// Called by the scheduler (with the big lock held) on the current guest thread.
// Returns 1 if any callback ran.
int gpu_run_interrupts(Ctx *c) {
    int ran = 0;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        if (!g_intr_n) {
            pthread_mutex_unlock(&g_lock);
            return ran;
        }
        uint32_t fn = g_intr[0].fn, a0 = g_intr[0].a0, a1 = g_intr[0].a1;
        memmove(&g_intr[0], &g_intr[1], sizeof g_intr[0] * (size_t)(--g_intr_n));
        pthread_mutex_unlock(&g_lock);
        Ctx tmp = *c;  // run below the thread's stack, as an interrupt would
        tmp.r[4] = a0;
        tmp.r[5] = a1;
        tmp.r[29] = (c->r[29] - 0x800) & ~15u;
        tmp.r[31] = 0;
        hle_intr_enter();
        call_indirect(&tmp, fn);
        hle_intr_leave();
        ran = 1;
    }
}

#ifdef __wii__
extern int g_hot_plain;   /* raster.c: the fast path is taking plain draws through the draw cache */
static inline void ge_prefetch_ram(uint32_t a) {
    uint32_t off = a - GUEST_RAM_BASE;
    if (off < GUEST_RAM_SIZE) __builtin_prefetch(g_mem + off);
}
#endif

static uint32_t rel_addr(uint32_t data) {
    uint32_t a = ((g_ge.cmd[GE_BASE] & 0x0F0000u) << 8) | (data & 0xFFFFFFu);
    return (a + g_ge.offset) & 0x0FFFFFFFu;
}

/* Which generations a register write moves (the three tests above), looked
   up per command instead of evaluated: the GE thread runs through a few
   hundred thousand commands a fight frame. */
#define OPC_VERTEX 1
#define OPC_LIGHT 2
#define OPC_PIX 4
#define OPC_VIEW 8   // the viewport: g_ge_view_gen
static uint8_t g_op_class[256];
#ifdef TKDR_AUTOPILOT
#ifdef EXP_NOLISTFAST
int g_ab_list = 0;
#else
int g_ab_list = 1;   /* A/B runs: the fast command loop on or off, alternating every report */
#endif
#define AB_LIST g_ab_list
#else
#define AB_LIST 1
#endif
#ifdef EXP_ABBONE
int g_ab_bone = 1;   /* A/B runs: bone matrix runs taken in the fast command loop (1) or by the general code (0), alternating every report */
#define AB_BONE g_ab_bone
#else
#define AB_BONE 1
#endif
#ifdef EXP_ABMAT
int g_ab_mat = 1;   /* A/B runs: world, view and projection matrix words and the matrix number commands taken in the fast command loop (1) or by the general code (0), alternating every report */
#define AB_MAT g_ab_mat
#else
#define AB_MAT 1
#endif
static uint8_t g_op_special[256];   /* the commands run_list() handles by name (the fast loop leaves them to it) */
static void op_class_init(void) {
    static const uint8_t special[] = {GE_IADDR, GE_BEZIER, GE_SPLINE, GE_JUMP, GE_BJUMP, GE_END, GE_SIGNAL, GE_FINISH,
                                      GE_OFFSETADDR, GE_ORIGIN, GE_BONEMATRIXNUMBER, GE_BONEMATRIXDATA,
                                      GE_WORLDMATRIXNUMBER, GE_WORLDMATRIXDATA, GE_VIEWMATRIXNUMBER, GE_VIEWMATRIXDATA,
                                      GE_PROJMATRIXNUMBER, GE_PROJMATRIXDATA, GE_TGENMATRIXNUMBER, GE_TGENMATRIXDATA,
                                      GE_LOADCLUT, GE_TEXFLUSH, GE_TRANSFERSTART};
    for (unsigned i = 0; i < sizeof special; i++) g_op_special[special[i]] = 1;
    for (uint32_t op = 0; op < 256; op++) {
        int view = (op >= GE_VIEWPORTXSCALE && op <= GE_VIEWPORTZCENTER) || op == GE_OFFSETX || op == GE_OFFSETY;
        g_op_class[op] = (uint8_t)((vertex_state_op(op) ? OPC_VERTEX : 0) | (light_state_op(op) ? OPC_LIGHT : 0) |
                                   (!vertex_only_op(op) ? OPC_PIX : 0) | (view ? OPC_VIEW : 0));
    }
}

static void load_matrix_word(float *m, int *n, int max, uint32_t data) {
    if (*n < max) m[*n] = ge_float(data);
    (*n)++;
}

#ifdef TKDR_AUTOPILOT
uint32_t g_op_hist[256];   /* demo statistics: how often each command is executed */
uint64_t g_op_ticks[256];  /* ... and how long, in time base ticks */
uint64_t g_ge_ticks[3];    /* time base ticks: running the list, presenting it, finishing it */
uint32_t g_prim_depth[2][6];   /* primitives at the top level of a list / inside a CALL, by draw category */
uint32_t g_cmds_depth[2];      /* commands at the top level / inside a CALL */
static inline uint32_t ge_tb(void) { uint32_t t; __asm__ volatile("mftb %0" : "=r"(t)); return t; }
#endif

#if defined(__wii__) && !defined(TKDR_FINEPROF)
/* A run of matrix data words (twelve a bone, world or view matrix, sixteen the projection) read on a host
   pointer: the words go in as the general code puts them in, one at a time, and the first word of another
   command ends the run, or the stall address (the pointer comes back there).  *np counts the words, past
   the end of the matrix too, as the general code does. */
static inline __attribute__((always_inline)) const uint32_t *matrix_run(const uint32_t *p, const uint32_t *sp, uint32_t op,
                                                                       float *m, int *np, int max) {
    int n = *np;
    uint32_t v = __builtin_bswap32(*p);
    for (;;) {
        if (n < max) {
            const uint32_t u = v << 8;
            memcpy(&m[n], &u, 4);
        }
        n++;
        if (++p == sp) break;
        v = __builtin_bswap32(*p);
        if ((v >> 24) != op) break;
    }
    *np = n;
    return p;
}
#ifdef EXP_MATCHK
/* matrix_run() against the general code's own load_matrix_word() on a copy, word by word. */
static const uint32_t *matrix_run_chk(const uint32_t *p, const uint32_t *sp, uint32_t op, float *m, int *np, int max) {
    static uint32_t runs, bad;
    float ref[96];
    memcpy(ref, m, (size_t)max * sizeof(float));
    int rn = *np, ok = 1;
    const uint32_t *r = matrix_run(p, sp, op, m, np, max);
    for (const uint32_t *q = p; q < r; q++) {
        const uint32_t w = __builtin_bswap32(*q);
        ok &= (w >> 24) == op;
        load_matrix_word(ref, &rn, max, w & 0xFFFFFFu);
    }
    ok &= rn == *np && memcmp(ref, m, (size_t)max * sizeof(float)) == 0 && r > p;
    runs++;
    if (!ok && ++bad <= 8) fprintf(stderr, "MATCHK MISMATCH op %02X run of %d words\n", op, (int)(r - p));
    if ((runs & 0x1FFFF) == 0) fprintf(stderr, "MATCHK %u runs, %u bad\n", runs, bad);
    return r;
}
#define MATRIX_RUN matrix_run_chk
#else
#define MATRIX_RUN matrix_run
#endif
#endif

// Execute until END/FINISH or the stall address. Returns 1 when the list ended.
static int run_list(GeList *l) {
    static int classes;
    if (!classes) {
        op_class_init();
        classes = 1;
    }
    /* The commands are read straight from the host memory behind the list:
       the window the PC is in is looked up once, and again only when the PC
       leaves it (a jump, a call, the end of the window). */
    const uint8_t *win = NULL;
    uint32_t win_pc = 0, win_len = 0;
    int slow1 = 0;   /* the fast loop has handed the next command to the general code */
    for (;;) {
        if (l->stall && l->pc == __atomic_load_n(&l->stall, __ATOMIC_RELAXED)) return 0;
        uint32_t off = l->pc - win_pc;
        if (win_len < 4 || off > win_len - 4) {
            /* The GE addresses a 28-bit space.  A PC outside the mapped guest
               windows means the list was built from a bad pointer; reading it
               would take whatever the scratch page holds for a command word.
               (The old test, pc >= end of RAM && pc < end of VRAM, could never
               be true, so it never fired.) */
            if (!guest_span(l->pc, 4)) {
                fprintf(stderr, "GE: list pc %08X outside the guest windows\n", l->pc);
                return 1;
            }
            win_pc = l->pc;
            win = guest_window(l->pc, &win_len);
            off = 0;
#ifdef __wii__
            /* Lists live in guest RAM: take the whole RAM as the window, so a
               call or a return to another part of it needs no new lookup. */
            if (win && l->pc >= GUEST_RAM_BASE && l->pc < GUEST_RAM_BASE + GUEST_RAM_SIZE) {
                win_pc = GUEST_RAM_BASE;
                win = g_mem;
                win_len = GUEST_RAM_SIZE;
                off = l->pc - GUEST_RAM_BASE;
            }
#endif
        }
#if defined(__wii__) && !defined(TKDR_FINEPROF)
        if (slow1) {
            slow1 = 0;
        } else if (AB_LIST && win == g_mem && win_pc == GUEST_RAM_BASE && !(off & 3)) {
            /* A list in guest RAM: the commands that make up nearly all of a
               fight's lists (the state writes, VADDR, BASE, PRIM, CALL and RET)
               run here on a host pointer, with the base, the offset and the
               stall address in locals.  The list's pc is written back only when
               this loop hands a command to the general code below (which
               executes exactly that one), or stops. */
            const uint32_t *p = (const uint32_t *)(win + off);
            const uint32_t stall = l->stall ? __atomic_load_n(&l->stall, __ATOMIC_RELAXED) : 0;
            const uint32_t *const sp = stall - GUEST_RAM_BASE < GUEST_RAM_SIZE ? (const uint32_t *)(g_mem + (stall - GUEST_RAM_BASE)) : NULL;
            uint32_t base_hi = (g_ge.cmd[GE_BASE] & 0x0F0000u) << 8, goff = g_ge.offset;
#define FAST_ADDR(d_) (((base_hi | (d_)) + goff) & 0x0FFFFFFFu)
#define FAST_PC(p_) (GUEST_RAM_BASE + (uint32_t)((const uint8_t *)(p_) - g_mem))
            for (;;) {
                if (__builtin_expect(p == sp, 0)) break;
                const uint32_t w = __builtin_bswap32(*p);
                const uint32_t op = w >> 24, data = w & 0xFFFFFFu;
                if (op == GE_VADDR) {
                    g_ge.vaddr = FAST_ADDR(data);
                    ge_prefetch_ram(g_ge.vaddr);
                    ge_prefetch_ram(g_ge.vaddr + 32);
                    if (g_hot_plain) vc_prefetch(g_ge.vaddr);
                    p++;
                } else if (op == GE_PRIM) {
                    gpu_draw_prim((int)((data >> 16) & 7), (int)(data & 0xFFFF));
#ifdef TKDR_AUTOPILOT
                    { extern int g_dp_cat; g_prim_depth[l->sp > 0][g_dp_cat]++; }
#endif
                    p++;
                } else if (op == GE_BASE) {
                    g_ge.cmd[GE_BASE] = data;
                    g_ge_vertex_gen++;
                    base_hi = (data & 0x0F0000u) << 8;
                    p++;
                } else if (op == GE_CALL) {
                    if (l->sp < 32) {
                        l->stack[l->sp][0] = FAST_PC(p + 1);
                        l->stack[l->sp][1] = goff;
                        l->sp++;
                    }
                    const uint32_t t = FAST_ADDR(data) & ~3u;
                    if (__builtin_expect(t - GUEST_RAM_BASE >= GUEST_RAM_SIZE, 0)) { l->pc = t; slow1 = 0; goto fast_left; }
                    p = (const uint32_t *)(g_mem + (t - GUEST_RAM_BASE));
                    ge_prefetch_ram(t);
                    ge_prefetch_ram(t + 32);
                } else if (op == GE_RET) {
                    if (l->sp > 0) {
                        l->sp--;
                        const uint32_t t = l->stack[l->sp][0];
                        goff = l->stack[l->sp][1];
                        g_ge.offset = goff;
                        if (__builtin_expect(t - GUEST_RAM_BASE >= GUEST_RAM_SIZE, 0)) { l->pc = t; slow1 = 0; goto fast_left; }
                        p = (const uint32_t *)(g_mem + (t - GUEST_RAM_BASE));
                    } else {
                        p++;
                    }
                } else if (op == GE_BONEMATRIXDATA && AB_BONE) {
                    /* A bone matrix is a run of twelve of these.  Taking the run here stores the same words as
                       the general code below and saves leaving and entering this loop for every matrix (about
                       1,800 a frame in a fight). */
                    p = MATRIX_RUN(p, sp, GE_BONEMATRIXDATA, &g_ge.bone[0][0], &g_ge.bone_n, 96);
#ifndef EXP_OLDGEN
                } else if (AB_MAT && op == GE_WORLDMATRIXDATA) {
                    p = MATRIX_RUN(p, sp, GE_WORLDMATRIXDATA, g_ge.world, &g_ge.world_n, 12);
                    g_ge_world_gen++;   /* once a run: the generations are only compared */
                } else if (AB_MAT && op == GE_VIEWMATRIXDATA) {
                    p = MATRIX_RUN(p, sp, GE_VIEWMATRIXDATA, g_ge.view, &g_ge.view_n, 12);
                    g_ge_view_gen++;
                } else if (AB_MAT && op == GE_PROJMATRIXDATA) {
                    p = MATRIX_RUN(p, sp, GE_PROJMATRIXDATA, g_ge.proj, &g_ge.proj_n, 16);
                    g_ge_view_gen++;
                } else if (AB_MAT && op == GE_WORLDMATRIXNUMBER) {
                    g_ge.world_n = (int)(data & 0xF); p++;
                } else if (AB_MAT && op == GE_VIEWMATRIXNUMBER) {
                    g_ge.view_n = (int)(data & 0xF); p++;
                } else if (AB_MAT && op == GE_PROJMATRIXNUMBER) {
                    g_ge.proj_n = (int)(data & 0xF); p++;
                } else if (AB_MAT && op == GE_BONEMATRIXNUMBER) {
                    g_ge.bone_n = (int)(data & 0x7F); p++;
#endif
                } else if (g_op_special[op]) {
                    l->pc = FAST_PC(p);   /* the general code executes this one */
                    slow1 = 1;
                    goto fast_left;
                } else {
                    if (op >= GE_MORPHWEIGHT0 && op < GE_MORPHWEIGHT0 + 8) g_ge.morph[op - GE_MORPHWEIGHT0] = ge_float(data);
                    if (g_ge.cmd[op] != data) {
                        g_ge.cmd[op] = data;
                        const unsigned k = g_op_class[op];
                        if (k & OPC_VERTEX) g_ge_vertex_gen++;
                        if (k & OPC_LIGHT) g_ge_light_gen++;
                        if (k & OPC_PIX) g_ge_pix_gen++;
                        if (k & OPC_VIEW) g_ge_view_gen++;
                    }
                    p++;
                }
            }
            l->pc = FAST_PC(p);   /* at the stall address: the code above decides whether the list stops */
#undef FAST_ADDR
#undef FAST_PC
        fast_left:
            continue;
        }
#endif
        uint32_t w;
        if (win) {
            memcpy(&w, win + off, 4);
#ifdef __wii__
            w = __builtin_bswap32(w);
#endif
        } else {
            w = mem_r32(l->pc);   // a window guest_window does not hand out
            win_len = 0;
        }
        uint32_t op = w >> 24, data = w & 0xFFFFFFu;
#ifdef TKDR_FINEPROF
        {   /* the time between two commands is charged to the first */
            static uint32_t prev_op, prev_t;
            uint32_t now_t = ge_tb();
            g_op_ticks[prev_op] += now_t - prev_t;
            prev_op = op;
            prev_t = now_t;
        }
        g_op_hist[op]++;
#endif
        uint32_t here = l->pc;
        l->pc += 4;
#ifdef TKDR_FINEPROF
        g_cmds_depth[l->sp > 0]++;
#endif
        switch (op) {
        case GE_JUMP:
            l->pc = rel_addr(data) & ~3u;
            continue;
        case GE_BJUMP:  // jump if the last bounding box test failed (we never cull)
            continue;
        case GE_CALL:
            if (l->sp < 32) {
                l->stack[l->sp][0] = l->pc;
                l->stack[l->sp][1] = g_ge.offset;
                l->sp++;
            }
            l->pc = rel_addr(data) & ~3u;
#ifdef __wii__
            ge_prefetch_ram(l->pc);
            ge_prefetch_ram(l->pc + 32);
#endif
            continue;
        case GE_RET:
            if (l->sp > 0) {
                l->sp--;
                l->pc = l->stack[l->sp][0];
                g_ge.offset = l->stack[l->sp][1];
            }
            continue;
        case GE_SIGNAL: {
            uint32_t next = mem_r32(l->pc);  // always paired with an END
            l->pc += 4;
            uint32_t behaviour = (data >> 16) & 0xFF;
            if (behaviour >= 1 && behaviour <= 3 && l->cbid >= 0)  // handler signals
                raise_interrupt(g_cb_signal, data & 0xFFFF, g_cb_signal_arg);
            else if (behaviour == 0x10)  // jump
                l->pc = (((data & 0xFFFF) << 16) | (next & 0xFFFF)) & ~3u;
            else if (behaviour == 0x11) {  // call
                if (l->sp < 32) { l->stack[l->sp][0] = l->pc; l->stack[l->sp][1] = g_ge.offset; l->sp++; }
                l->pc = (((data & 0xFFFF) << 16) | (next & 0xFFFF)) & ~3u;
            } else if (behaviour == 0x12 && l->sp > 0) {  // return
                l->sp--;
                l->pc = l->stack[l->sp][0];
                g_ge.offset = l->stack[l->sp][1];
            }
            continue;
        }
        case GE_FINISH:
            g_ge.cmd[op] = data;
            continue;
        case GE_END:
            if ((mem_r32(here - 4) >> 24) == GE_FINISH) {
                /* Only lists queued with a callback id raise the callback.
                   The game queues some lists with -1; running its finish
                   handler for those as well calls its frame-end hooks (and
                   its GE timing) once per extra list. */
                if (l->cbid >= 0)
                    raise_interrupt(g_cb_finish, g_ge.cmd[GE_FINISH] & 0xFFFF, g_cb_finish_arg);
                return 1;
            }
            continue;  // an END without FINISH just continues
        case GE_BASE:
            g_ge.cmd[op] = data;
            g_ge_vertex_gen++;
            continue;
        case GE_OFFSETADDR:
            g_ge.offset = data << 8;
            g_ge_vertex_gen++;
            continue;
        case GE_ORIGIN:
            g_ge.offset = here;
            g_ge_vertex_gen++;
            continue;
        case GE_VADDR:
            g_ge.vaddr = rel_addr(data);
#ifdef __wii__
            /* Hints for the primitive that follows (no effect on the result): its
               vertices, and the draw cache set it will be looked up in. */
            ge_prefetch_ram(g_ge.vaddr);
            ge_prefetch_ram(g_ge.vaddr + 32);
            if (g_hot_plain) vc_prefetch(g_ge.vaddr);
#endif
            continue;
        case GE_IADDR:
            g_ge.iaddr = rel_addr(data);
            continue;
        case GE_PRIM:
            gpu_draw_prim((int)((data >> 16) & 7), (int)(data & 0xFFFF));
#ifdef TKDR_AUTOPILOT
            { extern int g_dp_cat; g_prim_depth[l->sp > 0][g_dp_cat]++; }
#endif
            continue;
        case GE_BEZIER:
        case GE_SPLINE:
            continue;  // not used by the game so far
        /* The world, view, projection and bone matrices have generations of
           their own, or (the bones) are read when a skinned draw is decoded:
           none of them is part of the texture and fog state that
           g_ge_vertex_gen stands for, and bumping it made the fast draw path
           redo its cached state after every bone upload. */
#ifdef EXP_OLDGEN
#define MATGEN() g_ge_vertex_gen++
#else
#define MATGEN() ((void)0)
#endif
        case GE_WORLDMATRIXNUMBER: g_ge.world_n = (int)(data & 0xF); MATGEN(); continue;
        case GE_WORLDMATRIXDATA:
            load_matrix_word(g_ge.world, &g_ge.world_n, 12, data);
            MATGEN();
            g_ge_world_gen++;
            continue;
        case GE_VIEWMATRIXNUMBER: g_ge.view_n = (int)(data & 0xF); MATGEN(); continue;
        case GE_VIEWMATRIXDATA:
            load_matrix_word(g_ge.view, &g_ge.view_n, 12, data);
            MATGEN();
            g_ge_view_gen++;
            continue;
        case GE_PROJMATRIXNUMBER: g_ge.proj_n = (int)(data & 0xF); MATGEN(); continue;
        case GE_PROJMATRIXDATA:
            load_matrix_word(g_ge.proj, &g_ge.proj_n, 16, data);
            MATGEN();
            g_ge_view_gen++;
            continue;
        case GE_TGENMATRIXNUMBER: g_ge.tgen_n = (int)(data & 0xF); g_ge_vertex_gen++; continue;
        case GE_TGENMATRIXDATA: load_matrix_word(g_ge.tgen, &g_ge.tgen_n, 12, data); g_ge_vertex_gen++; continue;
        case GE_BONEMATRIXNUMBER: g_ge.bone_n = (int)(data & 0x7F); MATGEN(); continue;
        case GE_BONEMATRIXDATA: {
            /* A matrix arrives as a run of these (twelve words a bone, and a
               skinned draw sets up to eight bones): take the whole run here
               instead of dispatching every word. */
            int n = g_ge.bone_n;
            float *bone = &g_ge.bone[0][0];
            if (n < 96) bone[n] = ge_float(data);
            n++;
            if (win) {
                uint32_t off2 = l->pc - win_pc;
                uint32_t stall = l->stall ? __atomic_load_n(&l->stall, __ATOMIC_RELAXED) : 0;
                while (n < 96 && off2 + 4 <= win_len && !(stall && l->pc == stall)) {
                    uint32_t w2;
                    memcpy(&w2, win + off2, 4);
#ifdef __wii__
                    w2 = __builtin_bswap32(w2);
#endif
                    if ((w2 >> 24) != GE_BONEMATRIXDATA) break;
                    bone[n++] = ge_float(w2 & 0xFFFFFFu);
                    l->pc += 4;
                    off2 += 4;
                }
            }
            g_ge.bone_n = n;
            MATGEN();
            continue;
        }
        case GE_TRANSFERSTART: {
            g_ge.cmd[op] = data;
            g_ge_pix_gen++;
            gpu_block_transfer();
            // The transfer wrote guest memory: drop only the cached textures
            // that live in the destination range.
            const uint32_t *rc = g_ge.cmd;
            uint32_t dst = (rc[GE_TRANSFERDST] & 0xFFFFF0u) | ((rc[GE_TRANSFERDSTW] & 0xFF0000u) << 8);
            uint32_t dstride = rc[GE_TRANSFERDSTW] & 0x7F8u;
            uint32_t dx = rc[GE_TRANSFERDSTPOS] & 0x3FF, dy = (rc[GE_TRANSFERDSTPOS] >> 10) & 0x3FF;
            uint32_t tw = (rc[GE_TRANSFERSIZE] & 0x3FF) + 1, th = ((rc[GE_TRANSFERSIZE] >> 10) & 0x3FF) + 1;
            uint32_t tbpp = (rc[GE_TRANSFERSTART] & 1) ? 4 : 2;
            gpu_tex_invalidate_range(dst + (dy * dstride + dx) * tbpp,
                                     dst + ((dy + th) * dstride) * tbpp + tw * tbpp);
            continue;
        }
        case GE_TEXFLUSH:
            g_ge_pix_gen++;
            gpu_tex_invalidate();
            continue;
        case GE_LOADCLUT:
            g_ge.cmd[op] = data;
            g_ge_pix_gen++;
            gpu_load_clut();
            continue;
        default:
            if (op >= GE_MORPHWEIGHT0 && op < GE_MORPHWEIGHT0 + 8) g_ge.morph[op - GE_MORPHWEIGHT0] = ge_float(data);
            /* The GU library emits a command for every state call, so most of
               these writes repeat the value already there.  The caches keyed
               by the generations depend only on the register values (the
               commands with side effects are handled above), so only a value
               that really changed may invalidate them. */
            if (g_ge.cmd[op] == data) continue;
            g_ge.cmd[op] = data;
            {
                unsigned k = g_op_class[op];
                if (k & OPC_VERTEX) g_ge_vertex_gen++;
                if (k & OPC_LIGHT) g_ge_light_gen++;
                if (k & OPC_PIX) g_ge_pix_gen++;
                if (k & OPC_VIEW) g_ge_view_gen++;
            }
            continue;
        }
    }
}

// The list the GPU thread should work on next: oldest queued one.
static GeList *next_list(void) {
    GeList *best = NULL;
    for (int k = 0; k < MAX_LISTS; k++)
        if (g_lists[k].queued && (!best || g_lists[k].seq < best->seq)) best = &g_lists[k];
    return best;
}

uint64_t g_gpu_busy_us;  // time spent executing lists (stats)
/* The same time as a clock reading: the pass that is running counts up to `now` (s_ge_seq is odd while one runs).  The Wii has one
   CPU, so the emulated GE takes time the PSP's GE took on its own chip; the game's frame stopwatch leaves it out (overrides.c). */
static volatile uint32_t s_ge_seq;
static volatile uint64_t s_ge_t0;
uint64_t ge_busy_us(uint64_t now) {
    for (;;) {
        uint32_t q = s_ge_seq;
        uint64_t b = g_gpu_busy_us, t0 = s_ge_t0;
        if (q != s_ge_seq) continue;
        return (q & 1) && now > t0 ? b + (now - t0) : b;
    }
}

// ---------------------------------------------------------------- frame capture
// GE_CAPTURE=seconds: save the first display list started after that guest time,
// with the GE state at its start, VRAM at its start and RAM at its end, to
// GE_CAPTURE_FILE (default ge_capture.bin). gpu_replay() draws it again offline:
// repeatable benchmarks and before/after image comparisons.

#define CAP_VRAM 0x04000000u
#define CAP_VRAM_SIZE 0x00200000u
#define CAP_RAM 0x08000000u
#define CAP_RAM_SIZE 0x02000000u
static GeList *g_cap_list;
static GeState g_cap_ge;
static uint32_t g_cap_pc;
static uint8_t *g_cap_vram;

static void capture_begin(GeList *l) {
    static int done;
    const char *at = getenv("GE_CAPTURE");
    if (done || !at || (double)hle_time_us() / 1e6 < atof(at)) return;
    done = 1;
    g_cap_list = l;
    g_cap_ge = g_ge;
    g_cap_pc = l->pc;
    g_cap_vram = malloc(CAP_VRAM_SIZE);
    memcpy(g_cap_vram, host(CAP_VRAM), CAP_VRAM_SIZE);
}

static void capture_end(GeList *l) {
    if (l != g_cap_list) return;
    g_cap_list = NULL;
    const char *path = getenv("GE_CAPTURE_FILE") ? getenv("GE_CAPTURE_FILE") : "ge_capture.bin";
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t hdr[4] = {0x43454754u /* "TGEC" */, 1, g_cap_pc, (uint32_t)sizeof g_cap_ge};
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(&g_cap_ge, sizeof g_cap_ge, 1, f);
    fwrite(g_cap_vram, CAP_VRAM_SIZE, 1, f);
    fwrite(host(CAP_RAM), CAP_RAM_SIZE, 1, f);
    fclose(f);
    free(g_cap_vram);
    fprintf(stderr, "GE list at %08x captured to %s\n", g_cap_pc, path);
}

// Draw a captured list `iters` times (VRAM/RAM restored once, GE state each
// time) and report the time per frame; optionally save the result as PPM.
int gpu_replay(const char *path, int iters, const char *out_ppm) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "replay: cannot open %s\n", path);
        return 1;
    }
    uint32_t hdr[4];
    if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != 0x43454754u || hdr[3] != sizeof(GeState)) {
        fprintf(stderr, "replay: not a capture from this build\n");
        fclose(f);
        return 1;
    }
    GeState ge;
    size_t ok = fread(&ge, sizeof ge, 1, f);
    ok += fread(host(CAP_VRAM), CAP_VRAM_SIZE, 1, f);
    ok += fread(host(CAP_RAM), CAP_RAM_SIZE, 1, f);
    fclose(f);
    if (ok != 3) {
        fprintf(stderr, "replay: truncated capture\n");
        return 1;
    }
    uint64_t best = ~0ull, total = 0;
    for (int i = 0; i < iters; i++) {
        g_ge = ge;
        g_ge_vertex_gen++;
        g_ge_light_gen++;
        g_ge_pix_gen++;
        g_ge_view_gen++;
        g_ge_world_gen++;
        GeList l = {.used = 1, .queued = 1, .pc = hdr[2]};
        uint64_t t0 = hle_time_us();
        run_list(&l);
        raster_flush();
        gpu_resolve_all();
        uint64_t dt = hle_time_us() - t0;
        total += dt;
        if (dt < best) best = dt;
    }
    fprintf(stderr, "replay: %d frames at %dx, best %.2f ms, mean %.2f ms\n", iters, gpu_render_scale(),
            best / 1000.0, total / 1000.0 / iters);
    if (out_ppm) {
        uint32_t fb = 0x04000000u | (g_ge.cmd[GE_FRAMEBUFPTR] & 0xFFFFF0u);
        uint32_t stride = g_ge.cmd[GE_FRAMEBUFWIDTH] & 0x7FC, fmt = g_ge.cmd[GE_FRAMEBUFPIXFORMAT] & 3;
        static uint16_t px[480 * 4 * 272 * 4];
        int S = gpu_scanout_scaled(fb, stride, fmt, px, 4);
        if (!S) {  // 1x: straight from VRAM
            S = 1;
            for (int y = 0; y < 272; y++)
                for (int x = 0; x < 480; x++) {
                    uint32_t a = fb + (uint32_t)(y * (int)stride + x) * (fmt == 3 ? 4u : 2u), v, r, g, b;
                    if (fmt == 3) { v = mem_r32(a); r = v & 255; g = v >> 8 & 255; b = v >> 16 & 255; }
                    else {
                        v = mem_r16(a);
                        if (fmt == 0) { r = (v & 31) << 3; g = (v >> 5 & 63) << 2; b = (v >> 11 & 31) << 3; }
                        else if (fmt == 1) { r = (v & 31) << 3; g = (v >> 5 & 31) << 3; b = (v >> 10 & 31) << 3; }
                        else { r = (v & 15) << 4; g = (v >> 4 & 15) << 4; b = (v >> 8 & 15) << 4; }
                    }
                    px[y * 480 + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                }
        }
        FILE *o = fopen(out_ppm, "wb");
        if (o) {
            fprintf(o, "P6\n%d %d\n255\n", 480 * S, 272 * S);
            for (int i = 0; i < 480 * S * 272 * S; i++) {
                uint16_t c = px[i];
                uint8_t rgb[3] = {(uint8_t)(((c >> 11) & 0x1F) * 255 / 31),
                                  (uint8_t)(((c >> 5) & 0x3F) * 255 / 63),
                                  (uint8_t)((c & 0x1F) * 255 / 31)};
                fwrite(rgb, 3, 1, o);
            }
            fclose(o);
        }
    }
    return 0;
}

static void *gpu_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        GeList *l = next_list();
        if (!l || (l->stall && l->pc == l->stall)) {
            pthread_cond_wait(&g_work, &g_lock);
            continue;
        }
        pthread_mutex_unlock(&g_lock);
        uint64_t t0 = hle_time_us();
        s_ge_t0 = t0; s_ge_seq++;
        if (!l->started) {
            l->started = 1;
            g_ge_pix_gen++;   // the guest may have written textures before this list
            capture_begin(l);
#ifdef __wii__
            /* A list that stalls is resumed by a later pass through here,
               so the chip's per-list setup belongs to the first pass. */
            if (g_gxr_enabled) wii_gxscene_list_begin();
#endif
        }
#ifdef TKDR_AUTOPILOT
        uint32_t tb0 = ge_tb();
#endif
        int done = run_list(l);
        extern uint32_t g_flush_reason[5];
#ifdef TKDR_AUTOPILOT
        uint32_t tb1 = ge_tb();
        g_ge_ticks[0] += tb1 - tb0;
#endif
#ifdef __wii__
        if (done && g_gxr_enabled) wii_gxscene_present();
#endif
#ifdef TKDR_AUTOPILOT
        uint32_t tb2 = ge_tb();
        g_ge_ticks[1] += tb2 - tb1;
#endif
        if (done) {  // the list is complete only once its pixels are
            g_flush_reason[0]++;
            raster_flush();
            gpu_resolve_all();  // and VRAM holds what was drawn at the internal resolution
            gpu_half_res_upsample();  // and the guest's framebuffer holds the picture
            capture_end(l);
            if (g_glr_active) glr_list_end();  // hand the list to the GPU side
        }
#ifdef TKDR_AUTOPILOT
        g_ge_ticks[2] += ge_tb() - tb2;
#endif
        g_gpu_busy_us += hle_time_us() - t0;
        s_ge_seq++;
        pthread_mutex_lock(&g_lock);
        if (done) {
            l->done = 1;
            l->queued = 0;
        }
        pthread_cond_broadcast(&g_work);
        if (done) {  // a guest thread may be waiting for it (sceGeDrawSync, sceGeListSync)
            pthread_mutex_unlock(&g_lock);
            sched_wake_idle();
            pthread_mutex_lock(&g_lock);
        }
    }
    return NULL;
}

static void start_gpu_thread(void) {
    static int started;
    if (started) return;
    started = 1;
#ifdef HLE_TRACE
    { const char *e = getenv("GE_TRACE"); g_ge_trace = e && *e != '0'; }
#endif
    pthread_t t;
    pthread_create(&t, NULL, gpu_thread, NULL);
    pthread_detach(t);
}

int32_t gpu_enqueue(Ctx *c, uint32_t list, uint32_t stall, int cbid, uint32_t arg, int head) {
    (void)c;
    (void)head;
    start_gpu_thread();
    pthread_mutex_lock(&g_lock);
    int id = -1;
    for (int k = 0; k < MAX_LISTS; k++)
        if (!g_lists[k].used || g_lists[k].done) { id = k; break; }
    if (id < 0) {
        pthread_mutex_unlock(&g_lock);
        return (int32_t)0x80000021u;
    }
    GeList *l = &g_lists[id];
    memset(l, 0, sizeof *l);
    l->used = 1;
    l->queued = 1;
    l->seq = ++g_next_seq;
    /* The GE addresses a 28-bit space (see rel_addr()), so the list start has
       to keep bit 27 -- that bit is what places it in the PSP's user RAM.
       Masking to 24 bits moved the list 0x08000000 bytes below where the
       guest put it, and the GE then read its commands from unrelated memory
       and branched through whatever it found there. */
    l->pc = list & 0x0FFFFFFFu;
    l->stall = stall & 0x0FFFFFFFu;
#ifdef HLE_TRACE
    if (g_ge_trace)
        fprintf(stderr, "GE list enqueue: pc=%08X stall=%08X (list=%08X)\n",
                l->pc, l->stall, list);
#endif
    l->cbid = cbid;
    l->arg = arg;
    pthread_cond_broadcast(&g_work);
    pthread_mutex_unlock(&g_lock);
    return id;
}

int32_t gpu_update_stall(Ctx *c, int id, uint32_t stall) {
    (void)c;
    if (id < 0 || id >= MAX_LISTS || !g_lists[id].used) return (int32_t)0x80000100u;
    pthread_mutex_lock(&g_lock);
    __atomic_store_n(&g_lists[id].stall, stall & 0x0FFFFFFFu, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_work);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

// 1 while any list is queued (or stalled), or interrupts are waiting to run.
int gpu_busy(int list_id) {
    pthread_mutex_lock(&g_lock);
    int busy = 0;
    if (list_id >= 0 && list_id < MAX_LISTS) busy = g_lists[list_id].queued;
    else
        for (int k = 0; k < MAX_LISTS; k++) busy |= g_lists[k].queued;
    busy |= g_intr_n > 0;
    pthread_mutex_unlock(&g_lock);
    return busy;
}

void hle_wait_gpu(Ctx *c, int list_id);

// mode 0: wait until done; mode 1: just report (0 done, 2 drawing)
int32_t gpu_sync(Ctx *c, int list_id, int mode) {
    if (mode == 1) return gpu_busy(list_id) ? 2 : 0;
    if (gpu_busy(list_id)) hle_wait_gpu(c, list_id);
    return 0;
}
