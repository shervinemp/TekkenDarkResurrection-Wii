/* The Wii's own graphics chip draws the guest's triangles.

   The PSP's GE and the Wii's GX are both fixed function pipelines, so the
   guest's display lists map onto the GX nearly one to one: the same
   vertices, the same texture, the same blending, the same depth test.
   Moving the per-pixel work out of the CPU is what the frame rate needs.
   The software rasteriser was taking three quarters of the emulated CPU
   and the guest's own code could not get any more of it.

   The geometry comes in two flavours:

     * the transform mode, where the GE would have applied the view and
       projection matrices itself.  The vertices are handed over in world
       space; the GX applies the view matrix as its position matrix and
       the projection matrix as its own.  Neither is the GE's 4x4 as it
       stands: the GX's projection is a sparse matrix whose clip volume
       has z between -w and 0 (the GE's runs from -w to w), so the
       projection is rebuilt in the GX's form (see load_world) and the
       viewport's depth range carries the GE's depth scale across.

     * the "through" mode and the rectangles, where the vertices are
       already screen coordinates.  Those go through an orthographic
       projection that maps the PSP's 480x272 pixels onto the picture.

   The interface is deliberately plain: this file is the only one that
   knows about the GX, and the rasteriser talks to it in floats.

   Threading: the GE thread is the only writer of the chip's command
   stream once the game is running.  The frontend never issues GX
   commands in this mode; it asks the video layer to show the newest
   copy the chip has finished (see wii_video_flip), which only reads the
   chip's token register. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <gccore.h>
#include <ogc/gx.h>
#include <ogc/gu.h>
#include <ogc/cache.h>
#include <ogc/mutex.h>

#include "gpu/gpu.h"
#include "hle/hle.h"
#include "wii_platform.h"

#define PSP_W 480
#define PSP_H 272
#define VERT_FLOATS 9   /* x y z u v r g b a */

/* The comparison functions.  The GE numbers them never, always, equal,
   not equal, less, less-equal, greater and greater-equal; the GX
   numbers them never, less, equal, less-equal, greater, not-equal,
   greater-equal and always. */
static const u8 k_cmp[8] = {
    GX_NEVER, GX_ALWAYS, GX_EQUAL, GX_NEQUAL,
    GX_LESS, GX_LEQUAL, GX_GREATER, GX_GEQUAL
};

/* The blend factors.  Factors 0 and 1 use the colour of the *other* side
   (the destination's for the source factor, the source's for the
   destination factor) and the rest are alpha based.  10 and up is the fixed
   colour: on the source side the TEV has already multiplied the colour by
   it (apply_fixed_source), so the factor is ONE; on the destination side
   white and black are exactly ONE and ZERO, and any other colour has no GX
   equivalent (the classifier keeps such 2D layers off the chip). */
static u8 blend_factor(int f, int is_src, uint32_t fix) {
    if (f >= 10 && !is_src) return (fix & 0xFFFFFFu) == 0 ? GX_BL_ZERO : GX_BL_ONE;
    switch (f) {
    case 0: return is_src ? GX_BL_DSTCLR : GX_BL_SRCCLR;
    case 1: return is_src ? GX_BL_INVDSTCLR : GX_BL_INVSRCCLR;
    case 2: return GX_BL_SRCALPHA;
    case 3: return GX_BL_INVSRCALPHA;
    case 4: return GX_BL_DSTALPHA;
    case 5: return GX_BL_INVDSTALPHA;
    case 6: return GX_BL_SRCALPHA;      /* twice the source alpha: no GX equivalent */
    case 7: return GX_BL_INVSRCALPHA;
    case 8: return GX_BL_DSTALPHA;
    case 9: return GX_BL_INVDSTALPHA;
    default: return GX_BL_ONE;          /* the fixed source colour, applied by the TEV */
    }
}

static GXRModeObj *s_mode_obj;
static int s_ready;
static int s_enabled;

/* Where the GE thread is inside this file, and how much it has done.  The
   heartbeat (WII_HEARTBEAT builds) prints these, so a hang can be placed. */
volatile int g_gx_stage;
void wii_gxscene_chip_status(int *lag, int *cmd_idle, int *read_idle, unsigned *fifo);
volatile uint32_t g_gx_lists, g_gx_presents, g_gx_texconv, g_gx_waits, g_gx_binds, g_gx_draws;
/* How often each block of chip state is sent again: the pixel pipeline, the
   TEV, the world-space matrices, and a batch of triangles. */
uint32_t g_gx_pix_sets, g_gx_tev_sets, g_gx_world_sets, g_gx_batches;
/* Counts every change to the chip state the rasteriser sets up for a draw
   (state, space, scissor, a texture that went away): when it has not moved,
   the same setup would do nothing. */
uint32_t g_gx_epoch;
#ifdef EXP_DARKDBG
volatile int g_dbg_stage;   /* debug, set once a run stays dark: bit 0 no depth test, bit 1 white vertex colours, bit 2 no texture */
#endif
uint32_t g_mv_version = 1;
volatile uint32_t g_gx_native_tex, g_gx_rgba_tex;
volatile uint32_t g_gx_texconv_us;
#define STAGE(n) (g_gx_stage = (n))

/* The picture's rectangle inside the display, and the scale that takes
   the PSP's pixels to the display's. */
static float s_lb_x0, s_lb_y0, s_lb_x1, s_lb_y1, s_kx, s_ky;

int wii_gxscene_enabled(void) { return s_enabled; }
void wii_gxscene_enable(int on) { s_enabled = on; }

#ifdef EXP_TEVFOG
static void fogramp_init(void);
#endif

void wii_gxscene_init(void) {
    if (s_ready) return;
    s_mode_obj = VIDEO_GetPreferredMode(NULL);
    if (!s_mode_obj) return;
    float vw = (float)s_mode_obj->fbWidth, vh = (float)s_mode_obj->efbHeight;
    float dw = vw, dh = dw * (float)PSP_H / (float)PSP_W;
    if (dh > vh) {
        dh = vh;
        dw = dh * (float)PSP_W / (float)PSP_H;
    }
    s_lb_x0 = (vw - dw) * 0.5f;
    s_lb_y0 = (vh - dh) * 0.5f;
    s_lb_x1 = s_lb_x0 + dw;
    s_lb_y1 = s_lb_y0 + dh;
    s_kx = dw / (float)PSP_W;
    s_ky = dh / (float)PSP_H;
#ifdef EXP_TEVFOG
    fogramp_init();
#endif
    s_ready = 1;
}

/* ------------------------------------------------------------- tokens

   The chip runs behind the CPU, so anything it may still be reading (a
   texture's memory) cannot be freed or reused until it is past the point
   where it last used it.  Each display list ends with a token in the
   command stream; the chip reports the last token it has reached.  The
   tokens are 16 bits and compared as a difference, so the wrap is not a
   problem while fewer than 32768 lists are in flight. */
static u16 s_token = 1;   /* the token the current list will end with */

static int token_done(u16 token) {
    return (s16)(GX_GetDrawSync() - token) >= 0;
}

/* Wait until the chip has finished everything written so far: a token of
   our own goes into the stream now and we watch for it, yielding to the
   other threads meanwhile (GX_DrawDone sleeps on an interrupt that this
   thread cannot otherwise be sure to see). */
static void wait_for_chip(void) {
    u16 t = s_token++;
    GX_SetDrawSync(t);
    GX_Flush();
    while (!token_done(t)) LWP_YieldThread();
}

/* ----------------------------------------------------- texture cache

   The guest's textures arrive decoded (RGBA, one 32 bit word per texel)
   from the rasteriser's own cache, with a serial number that changes
   whenever the texels do.  Here they are converted once into the GX's
   RGBA8 layout (4x4 tiles, alpha and red in the first half of a tile,
   green and blue in the second) and kept until they are evicted. */
/* The slots are only bookkeeping; the converted texels live in the MEM2
   pool, which is what really limits how many textures stay converted.  128
   slots were fewer than a fight's working set (stage, both fighters, HUD,
   text), so LRU converted the same textures again every frame. */
#define TEX_SLOTS 512
#define LIMBO_SLOTS 64

typedef struct {
    uint32_t serial;       /* 0: the slot is free */
    int tw, th;            /* the size of the converted texture */
    u8 fmt;                /* GX_TF_* */
    int native;            /* CI4/CI8: buf holds indices, tlut holds colours */
    uint8_t *buf;
    uint8_t *tlut;         /* 256 RGB565/RGB5A3 entries, or NULL */
    u8 tlut_fmt;
    size_t bytes;          /* texture bytes, including the TLUT allocation */
    u16 last_token;        /* the last list that drew with it */
    uint32_t last_use;     /* for the eviction order */
} TexSlot;

static TexSlot s_tex[TEX_SLOTS];
#ifdef TKDR_AUTOPILOT
static uint32_t s_tex_nopix, s_tex_nomem;   /* tex_get() gave up: no decoded texels, no pool memory (autopilot probe) */
static uint32_t s_zero_serial[4], s_zero_n, s_zero_binds, s_zero_logged;   /* textures found empty by tex_probe(), and the draws that use them */
#define TEX_GAVE_UP(c) (c++)
#else
#define TEX_GAVE_UP(c) ((void)0)
#endif
static uint32_t s_use_tick;
static int s_tex_need_inval;   // a buffer was retired/reused since the last upload

/* Buffers released while the chip may still be reading them. */
static struct { uint8_t *buf; size_t bytes; u16 token; } s_limbo[LIMBO_SLOTS];
static int s_nlimbo;
static size_t s_limbo_bytes;

static void limbo_collect(void) {
    int keep = 0;
    for (int i = 0; i < s_nlimbo; i++) {
        if (token_done(s_limbo[i].token)) {
            s_limbo_bytes -= s_limbo[i].bytes;
            wii_big_free(s_limbo[i].buf);
            s_tex_need_inval = 1;   // the allocator may reuse this address
        } else {
            s_limbo[keep++] = s_limbo[i];
        }
    }
    s_nlimbo = keep;
}

/* Give a buffer back once the chip is done with it. */
static void retire(uint8_t *buf, size_t bytes, u16 token) {
    if (!buf) return;
    if (token_done(token)) {
        wii_big_free(buf);
        return;
    }
    if (s_nlimbo == LIMBO_SLOTS || s_limbo_bytes > (4u << 20)) {
        /* Too much memory is waiting on the chip: wait for it. */
        STAGE(4);
        g_gx_waits++;
        wait_for_chip();
        limbo_collect();
        if (token_done(token)) {
            wii_big_free(buf);
            return;
        }
    }
    s_limbo[s_nlimbo].buf = buf;
    s_limbo[s_nlimbo].bytes = bytes;
    s_limbo[s_nlimbo].token = token;
    s_nlimbo++;
    s_limbo_bytes += bytes;
}

/* One 4x4 tile of GX_TF_RGBA8 (64 bytes: the alpha/red pairs of its 16 texels, then the green/blue pairs) from a
   decoded texel (r | g<<8 | b<<16 | a<<24).  The Wii is big endian: a halfword store puts its high byte first. */
static inline void rgba8_texel(uint8_t *ar, uint8_t *gb, int o, uint32_t p) {
    *(uint16_t *)(ar + o) = (uint16_t)((p >> 24) << 8 | (p & 0xFF));
    *(uint16_t *)(gb + o) = (uint16_t)((p >> 8 & 0xFF) << 8 | (p >> 16 & 0xFF));
}

static void convert_rgba8(uint8_t *dst, const uint32_t *src, int w, int h, int tw, int th) {
    for (int ty = 0; ty < th; ty += 4) {
        for (int tx = 0; tx < tw; tx += 4) {
            uint8_t *ar = dst, *gb = dst + 32;
            if (tx + 4 <= w && ty + 4 <= h) {   /* a whole tile inside the texture: no edge to repeat */
                const uint32_t *p = src + (size_t)ty * (size_t)w + (size_t)tx;
                for (int r = 0; r < 4; r++, p += w) {
                    rgba8_texel(ar, gb, (r * 4 + 0) * 2, p[0]);
                    rgba8_texel(ar, gb, (r * 4 + 1) * 2, p[1]);
                    rgba8_texel(ar, gb, (r * 4 + 2) * 2, p[2]);
                    rgba8_texel(ar, gb, (r * 4 + 3) * 2, p[3]);
                }
            } else {
                for (int r = 0; r < 4; r++) {
                    int y = ty + r;
                    if (y >= h) y = h - 1;     /* the padding repeats the edge */
                    for (int c = 0; c < 4; c++) {
                        int x = tx + c;
                        if (x >= w) x = w - 1;
                        rgba8_texel(ar, gb, (r * 4 + c) * 2, src[(size_t)y * (size_t)w + (size_t)x]);
                    }
                }
            }
            dst += 64;
        }
    }
}

#ifdef EXP_CICHK   /* -DEXP_CICHK: convert_rgba8() against the byte-wise version it replaced, on every conversion */
static void convert_rgba8_ref(uint8_t *dst, const uint32_t *src, int w, int h, int tw, int th) {
    for (int ty = 0; ty < th; ty += 4) {
        for (int tx = 0; tx < tw; tx += 4) {
            uint8_t *ar = dst, *gb = dst + 32;
            for (int r = 0; r < 4; r++) {
                int y = ty + r;
                if (y >= h) y = h - 1;     /* the padding repeats the edge */
                for (int c = 0; c < 4; c++) {
                    int x = tx + c;
                    if (x >= w) x = w - 1;
                    uint32_t p = src[(size_t)y * (size_t)w + (size_t)x];   /* r | g<<8 | b<<16 | a<<24 */
                    int o = (r * 4 + c) * 2;
                    ar[o] = (uint8_t)(p >> 24);
                    ar[o + 1] = (uint8_t)p;
                    gb[o] = (uint8_t)(p >> 8);
                    gb[o + 1] = (uint8_t)(p >> 16);
                }
            }
            dst += 64;
        }
    }
}
#endif

/* The game's textures are almost entirely 4-bit or 8-bit paletted (the
   captured menu uses GE formats 4 and 5).  The CPU decodes those to RGBA for
   the software path, but the GX can sample the same source as CI4/CI8 with a
   TLUT.  That keeps four texels in the memory of one RGBA texel and removes
   most of the per-frame texture conversion.  The raw source is read through
   the same swizzle/stride rules as fetch() in raster.c. */
static uint32_t ci_addr(const GxrState *s, int x, int y, int bits) {
    uint32_t row_bytes = (uint32_t)s->tex_stride * (uint32_t)bits / 8u;
    uint32_t xb = (uint32_t)x * (uint32_t)bits / 8u;
    if (!s->tex_swizzle) return s->tex_addr + (uint32_t)y * row_bytes + xb;
    uint32_t blocks_per_row = row_bytes / 16u;
    uint32_t block = ((uint32_t)y / 8u) * blocks_per_row + xb / 16u;
    return s->tex_addr + block * 128u + ((uint32_t)y % 8u) * 16u + xb % 16u;
}

static uint8_t ci_index(const GxrState *s, int x, int y) {
    uint32_t a = ci_addr(s, x, y, s->tex_fmt == 4 ? 4 : 8);
    uint8_t b = mem_r8(a);
    return s->tex_fmt == 4 ? (uint8_t)((x & 1) ? b >> 4 : b & 15) : b;
}

/* GX CI4 tiles are 8x8 texels in 32 bytes (four bytes per row).  The GX
   keeps the left texel of a byte in the high nibble; the PSP keeps it in
   the low one.  (Storing it low swapped every pair of texels.) */
static void convert_ci4(uint8_t *dst, const GxrState *s, int w, int h) {
    for (int ty = 0; ty < h; ty += 8) {
        for (int tx = 0; tx < w; tx += 8) {
            for (int r = 0; r < 8; r++) {
                uint8_t row[4] = {0, 0, 0, 0};
                for (int c = 0; c < 8; c++) {
                    int x = tx + c, y = ty + r;
                    if (x >= w) x = w - 1;
                    if (y >= h) y = h - 1;
                    uint8_t v = ci_index(s, x, y) & 15;
                    if (c & 1) row[c >> 1] |= v;
                    else row[c >> 1] = (uint8_t)(v << 4);
                }
                memcpy(dst, row, 4);
                dst += 4;
            }
        }
    }
}

/* GX CI8 tiles are 8x4 texels in 32 bytes (eight bytes per row). */
static void convert_ci8(uint8_t *dst, const GxrState *s, int w, int h) {
    for (int ty = 0; ty < h; ty += 4) {
        for (int tx = 0; tx < w; tx += 8) {
            for (int r = 0; r < 4; r++) {
                for (int c = 0; c < 8; c++) {
                    int x = tx + c, y = ty + r;
                    if (x >= w) x = w - 1;
                    if (y >= h) y = h - 1;
                    *dst++ = ci_index(s, x, y);
                }
            }
        }
    }
}

static uint16_t clut16_value(const GxrState *s, uint32_t idx) {
    uint16_t v;
    memcpy(&v, (const uint8_t *)g_ge.clut + (idx & 511u) * 2u, 2);
#ifdef __wii__
    v = __builtin_bswap16(v);
#endif
    return v;
}

static uint32_t clut32_value(uint32_t idx) {
    uint32_t v;
    memcpy(&v, (const uint8_t *)g_ge.clut + (idx & 255u) * 4u, 4);
#ifdef __wii__
    v = __builtin_bswap32(v);
#endif
    return v;
}

/* Build a GX TLUT in the order the hardware indexes it.  The GE CLUT can
   shift/mask/offset the index; putting the transformed entry at the plain
   source index lets GX do the lookup directly. */
static void build_tlut(uint16_t *out, const GxrState *s) {
    int fmt = s->tex_clut_fmt;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t idx = ((i >> s->tex_clut_shift) & (uint32_t)s->tex_clut_mask) |
                       (uint32_t)s->tex_clut_off;
        uint16_t gx;
        if (fmt == 3) {
            /* 8888 (red in the low byte) into RGB5A3: nearly opaque entries
               keep 5-bit colour, the rest 4-bit colour and 3-bit alpha.
               Lossy; only the GX_FASTTEX build asks for it. */
            uint32_t c = clut32_value(idx);
            uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
            if (a >= 0xE0)
                gx = (uint16_t)(0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
            else
                gx = (uint16_t)((a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));
            out[i] = gx;
            continue;
        }
        uint16_t v = clut16_value(s, idx);
        if (fmt == 0) {
            /* PSP 565 keeps red in the low bits; GX RGB565 keeps it high. */
            uint32_t r = v & 31, g = (v >> 5) & 63, b = (v >> 11) & 31;
            gx = (uint16_t)(r << 11 | g << 5 | b);
        } else if (fmt == 1) {
            /* GX RGB5A3: bit 15 set is opaque RGB555; clear is 3-bit alpha
               and RGB444.  (The flag used to be inverted, so opaque entries
               read as nearly transparent ones and the picture went dark.) */
            uint32_t r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
            if (v & 0x8000)
                gx = (uint16_t)(0x8000 | r << 10 | g << 5 | b);
            else
                gx = (uint16_t)(((r >> 1) << 8) | ((g >> 1) << 4) | (b >> 1));   /* alpha 0 */
        } else {  /* 4444 */
            uint32_t r = v & 15, g = (v >> 4) & 15, b = (v >> 8) & 15, a = v >> 12;
            if (a == 15)
                gx = (uint16_t)(0x8000 | (r << 1 | r >> 3) << 10 | (g << 1 | g >> 3) << 5 |
                                (b << 1 | b >> 3));
            else
                gx = (uint16_t)((a >> 1) << 12 | r << 8 | g << 4 | b);   /* 3-bit alpha: lossy */
        }
        out[i] = gx;
    }
}

#ifdef GPU_SELFTEST
/* Test vectors for the paletted GX upload (docs/PLAN.md, the black-face
   isolation): PSP-ordered indices go through convert_ci4/convert_ci8 and
   are read back with the GX tile rules written out here independently
   (CI4: 8x8 tiles, left texel in the high nibble; CI8: 8x4 tiles), and
   TLUT entries are checked against hand-encoded RGB565/RGB5A3 values. */
static int st_gx_fail, st_gx_count;

static void st_gx(const char *what, uint32_t got, uint32_t want) {
    st_gx_count++;
    if (got != want) {
        st_gx_fail++;
        fprintf(stderr, "SELFTEST FAIL gx %s: got %04X want %04X\n", what, got, want);
    }
}

int wii_gxscene_selftest(void) {
    const uint32_t src = 0x04180000u;
    static uint8_t out[32 * 8] __attribute__((aligned(32)));
    GxrState s;
    st_gx_fail = st_gx_count = 0;

    /* CI4, 16x8, linear: index (x, y) = (3x + 5y) & 15, PSP nibble order. */
    memset(&s, 0, sizeof s);
    s.tex_fmt = 4; s.tex_stride = 16; s.tex_addr = src;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 16; x += 2) {
            uint8_t lo = (uint8_t)((3 * x + 5 * y) & 15), hi = (uint8_t)((3 * (x + 1) + 5 * y) & 15);
            mem_w8(src + (uint32_t)(y * 8 + x / 2), (uint8_t)(lo | hi << 4));
        }
    convert_ci4(out, &s, 16, 8);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 16; x++) {
            int tile = (y / 8) * 2 + x / 8;
            uint8_t b = out[tile * 32 + (y % 8) * 4 + (x % 8) / 2];
            st_gx("ci4", (x & 1) ? (b & 15u) : (uint32_t)(b >> 4), (uint32_t)((3 * x + 5 * y) & 15));
        }

    /* CI8, 16x8, linear: index (x, y) = 7x + 11y. */
    s.tex_fmt = 5; s.tex_stride = 16;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 16; x++) mem_w8(src + (uint32_t)(y * 16 + x), (uint8_t)(7 * x + 11 * y));
    convert_ci8(out, &s, 16, 8);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 16; x++) {
            int tile = (y / 4) * 2 + x / 8;
            st_gx("ci8", out[tile * 32 + (y % 4) * 8 + (x % 8)], (uint8_t)(7 * x + 11 * y));
        }

    /* CI8, 32x8, PSP-swizzled: 16-byte x 8-row blocks, two per row.
       Texel (20, 3) is in block 1, row 3, byte 4. */
    s.tex_fmt = 5; s.tex_stride = 32; s.tex_swizzle = 1;
    for (uint32_t i = 0; i < 256; i++) mem_w8(src + i, 0);
    mem_w8(src + 128 + 3 * 16 + 4, 0x5A);
    convert_ci8(out, &s, 32, 8);
    st_gx("ci8 swizzled", out[(0 * 4 + 2) * 32 + 3 * 8 + 4], 0x5A);

    /* RGBA8 tiles, written out from the GX rule: 4x4 texels in 64 bytes, the alpha and red bytes of the 16 texels
       first, then the green and blue bytes; a texel outside the picture repeats the edge.  2x2 and 6x5 are padded
       (the whole-tile fast path must not be taken), 8x8 is all whole tiles. */
    {
        static const int rc[3][2] = {{2, 2}, {8, 8}, {6, 5}};
        static uint32_t px[64];
        static uint8_t o[256] __attribute__((aligned(32)));
        for (int k = 0; k < 3; k++) {
            const int w = rc[k][0], h = rc[k][1], tw = (w + 3) & ~3, th = (h + 3) & ~3;
            for (int i = 0; i < w * h; i++) px[i] = 0x11223344u * (uint32_t)(i + 1) ^ 0x80402010u;
            memset(o, 0xEE, sizeof o);
            convert_rgba8(o, px, w, h, tw, th);
            for (int y = 0; y < th; y++)
                for (int x = 0; x < tw; x++) {
                    const uint32_t p = px[(y < h ? y : h - 1) * w + (x < w ? x : w - 1)];
                    const int base = ((y / 4) * (tw / 4) + x / 4) * 64, j = ((y % 4) * 4 + x % 4) * 2;
                    st_gx("rgba8 a", o[base + j], p >> 24);
                    st_gx("rgba8 r", o[base + j + 1], p & 0xFF);
                    st_gx("rgba8 g", o[base + 32 + j], p >> 8 & 0xFF);
                    st_gx("rgba8 b", o[base + 32 + j + 1], p >> 16 & 0xFF);
                }
        }
    }

#ifdef EXP_CICHK   /* the time of convert_rgba8() against the version it replaced, on the same pixels */
    {
        static const int bs[3] = {128, 256, 512};
        uint8_t *bo = (uint8_t *)wii_big_alloc(512u * 512u * 4u);
        uint32_t *bp = (uint32_t *)wii_big_alloc(512u * 512u * 4u);
        for (int k = 0; bo && bp && k < 3; k++) {
            const int n = bs[k];
            for (int i = 0; i < n * n; i++) bp[i] = 0x9E3779B9u * (uint32_t)(i + 1);
            for (int pass = 0; pass < 2; pass++) {
                uint64_t t0 = hle_time_us();
                for (int it = 0; it < 4; it++) {
                    if (pass == 0) convert_rgba8(bo, bp, n, n, n, n);
                    else convert_rgba8_ref(bo, bp, n, n, n, n);
                }
                fprintf(stderr, "RGBABENCH %dx%d %s: %u us\n", n, n, pass == 0 ? "new" : "old", (unsigned)((hle_time_us() - t0) / 4));
            }
        }
    }
#endif

    /* TLUT: PSP 565 keeps red low; GX RGB565 high.  RGB5A3 has bit 15 set
       for opaque RGB555 and clear for 3-bit alpha + RGB444. */
    uint32_t saved_clut[256];
    memcpy(saved_clut, g_ge.clut, sizeof saved_clut);
    uint16_t lut[256];
    static const struct { int fmt; uint16_t psp, gx; const char *what; } tl[] = {
        {0, 0x001F, 0xF800, "tlut 565 red"},
        {0, 0x07E0, 0x07E0, "tlut 565 green"},
        {1, 0x801F, 0xFC00, "tlut 5551 opaque red"},
        {1, 0x001F, 0x0F00, "tlut 5551 clear red"},
        {2, 0xF00F, 0xFC00, "tlut 4444 opaque red"},
        {2, 0x800F, 0x4F00, "tlut 4444 half red"},
    };
    {   /* 8888: opaque red keeps 5-bit colour; half-alpha blue 3/4-bit. */
        static const struct { uint32_t psp; uint16_t gx; const char *what; } t8[] = {
            {0xFF0000FFu, 0xFC00, "tlut 8888 opaque red"},
            {0x80FF0000u, 0x400F, "tlut 8888 half blue"},
        };
        for (unsigned k = 0; k < sizeof t8 / sizeof t8[0]; k++) {
            uint32_t le = __builtin_bswap32(t8[k].psp);   /* as the guest stores it */
            memcpy((uint8_t *)g_ge.clut + 4, &le, 4);      /* entry 1 */
            memset(&s, 0, sizeof s);
            s.tex_clut_fmt = 3; s.tex_clut_mask = 0xFF;
            build_tlut(lut, &s);
            st_gx(t8[k].what, lut[1], t8[k].gx);
        }
    }
    for (unsigned k = 0; k < sizeof tl / sizeof tl[0]; k++) {
        uint16_t le = (uint16_t)(tl[k].psp << 8 | tl[k].psp >> 8);   /* as the guest stores it */
        memcpy((uint8_t *)g_ge.clut + 2, &le, 2);                    /* entry 1 */
        memset(&s, 0, sizeof s);
        s.tex_clut_fmt = tl[k].fmt; s.tex_clut_mask = 0xFF;
        build_tlut(lut, &s);
        st_gx(tl[k].what, lut[1], tl[k].gx);
    }
    memcpy(g_ge.clut, saved_clut, sizeof saved_clut);
    for (uint32_t i = 0; i < 256; i++) mem_w8(src + i, 0);
    fprintf(stderr, "SELFTEST gx paletted: %d/%d passed\n", st_gx_count - st_gx_fail, st_gx_count);
    return st_gx_fail;
}
#endif

/* Memory for a converted texture.  The pool is finite and shared with the
   rasteriser's decoded textures: when it is full, what the chip has finished
   with is released, and then the least recently used textures are given up
   until the request fits. */
static uint8_t *tex_mem(size_t bytes, const void *keep) {
    uint8_t *b = wii_big_alloc(bytes);
    if (b) return b;
    limbo_collect();
    b = wii_big_alloc(bytes);
    if (b) return b;
    /* Decoded copies of textures this side already holds are only kept for
       converting them again: those go before any converted texture. */
    while (gpu_tex_drop_copy(keep)) {
        b = wii_big_alloc(bytes);
        if (b) return b;
    }
    for (;;) {
        TexSlot *lru = NULL;
        for (int i = 0; i < TEX_SLOTS; i++)
            if (s_tex[i].buf && (!lru || s_tex[i].last_use < lru->last_use)) lru = &s_tex[i];
        if (!lru) break;
        retire(lru->buf, lru->bytes, lru->last_token);
        lru->buf = NULL;
        lru->serial = 0;
        s_tex_need_inval = 1;
        b = wii_big_alloc(bytes);
        if (b) return b;
    }
    /* Whatever the chip was still reading is free once it has caught up. */
    STAGE(4);
    g_gx_waits++;
    wait_for_chip();
    limbo_collect();
    b = wii_big_alloc(bytes);
    if (b) return b;
    /* The rest of the pool holds decoded textures.  Their converted copies
       are gone already, so they are only kept for re-conversion: give up the
       oldest, never the one being converted now. */
    while (gpu_tex_evict_lru(keep)) {
        b = wii_big_alloc(bytes);
        if (b) return b;
    }
    return NULL;
}

/* Where each serial was last seen, so a texture that is still converted is
   found without scanning every slot.  A stale hint is simply not trusted. */
#define HINTS 1024
static uint16_t s_hint[HINTS];

/* The slot holding this texture, converting it if it is not there yet. */
static TexSlot *tex_get(const GxrState *s) {
    TexSlot *hinted = &s_tex[s_hint[s->tex_serial & (HINTS - 1)]];
    if (hinted->serial == s->tex_serial && hinted->buf) {
        hinted->last_use = ++s_use_tick;
        hinted->last_token = s_token;
        return hinted;
    }
    TexSlot *lru = NULL, *spare = NULL;
    for (int i = 0; i < TEX_SLOTS; i++) {
        TexSlot *t = &s_tex[i];
        if (t->serial == s->tex_serial && t->buf) {
            t->last_use = ++s_use_tick;
            t->last_token = s_token;
            s_hint[s->tex_serial & (HINTS - 1)] = (uint16_t)i;
            return t;
        }
        if (!t->buf) {
            if (!spare) spare = t;
        } else if (!lru || t->last_use < lru->last_use) {
            lru = t;
        }
    }
    int w = s->tex_pw, h = s->tex_ph;
    /* Which palettes may stay paletted is decided with tex_native (exact 565
       only, unless the lossy GX_FASTTEX build asks for all of them). */
    int native = s->tex_native && (s->tex_fmt == 4 || s->tex_fmt == 5) && w > 0 && h > 0;
#ifdef HLE_TRACE
    if (!native) {
        static uint32_t seen;
        uint32_t key = (uint32_t)s->tex_fmt | (uint32_t)s->tex_clut_fmt << 4 |
                       (uint32_t)(s->tex_native ? 1 : 0) << 8 |
                       (uint32_t)(s->tex_pix ? 1 : 0) << 9;
        if (!(seen & (1u << (key & 31)))) {
            seen |= 1u << (key & 31);
            fprintf(stderr, "TEXNATIVE off key=%03X fmt=%d clut=%d state_native=%d pix=%d w=%d h=%d\n",
                    key, s->tex_fmt, s->tex_clut_fmt, s->tex_native, s->tex_pix != NULL, w, h);
        }
    }
#endif
    int tw, th;
    u8 fmt;
    size_t texbytes, tlutbytes = 0;
    if (native) {
        tw = w;
        th = h;
        fmt = s->tex_fmt == 4 ? GX_TF_CI4 : GX_TF_CI8;
        /* Whole GX tiles (CI4 8x8, CI8 8x4, 32 bytes each): the converters
           write whole tiles, and the TLUT after them must stay 32-byte
           aligned.  Sizing by w * h let a small texture's last tile run into
           the TLUT and left the TLUT misaligned. */
        size_t pw = (size_t)((w + 7) & ~7);
        texbytes = s->tex_fmt == 4 ? pw * (size_t)((h + 7) & ~7) / 2 : pw * (size_t)((h + 3) & ~3);
        tlutbytes = 512;
    } else {
        tw = (w + 3) & ~3;
        th = (h + 3) & ~3;
        fmt = GX_TF_RGBA8;
        texbytes = (size_t)tw * (size_t)th * 4;
    }
    if (w <= 0 || h <= 0 || tw > 1024 || th > 1024 || !s->tex_pix) {
        TEX_GAVE_UP(s_tex_nopix);
        return NULL;
    }
    size_t bytes = texbytes + tlutbytes;
    TexSlot *t = spare;
    if (!t) {   // all the slots are in use: the least recently used one goes
        t = lru;
        retire(t->buf, t->bytes, t->last_token);
        t->buf = NULL;
        t->tlut = NULL;
        t->serial = 0;
        s_tex_need_inval = 1;
    }
    uint8_t *buf = tex_mem(bytes, s->tex_pix);
    if (!buf) {
        TEX_GAVE_UP(s_tex_nomem);
        return NULL;
    }
    STAGE(3);
    g_gx_texconv++;
    if (native) g_gx_native_tex++; else g_gx_rgba_tex++;
    uint32_t tc0 = (uint32_t)hle_time_us();
    if (native) {
        if (s->tex_fmt == 4) convert_ci4(buf, s, w, h);
        else convert_ci8(buf, s, w, h);
        uint16_t *tlut = (uint16_t *)(buf + texbytes);
        build_tlut(tlut, s);
        t->tlut = (uint8_t *)tlut;
        t->tlut_fmt = s->tex_clut_fmt == 0 ? GX_TL_RGB565 : GX_TL_RGB5A3;
    } else {
        convert_rgba8(buf, s->tex_pix, w, h, tw, th);
#ifdef EXP_CICHK
        {
            static unsigned n, bad;
            uint8_t *ref = (uint8_t *)malloc(texbytes);
            if (ref) {
                memset(ref, 0, texbytes);
                convert_rgba8_ref(ref, s->tex_pix, w, h, tw, th);
                n++;
                if (memcmp(ref, buf, texbytes) != 0 && ++bad <= 8)
                    fprintf(stderr, "RGBACHK BAD %dx%d (tile grid %dx%d)\n", w, h, tw, th);
                if ((n & 31) == 1) fprintf(stderr, "RGBACHK %u textures, %u bad\n", n, bad);
                free(ref);
            }
        }
#endif
        t->tlut = NULL;
    }
    g_gx_texconv_us += (uint32_t)(hle_time_us() - tc0);
    DCStoreRange(buf, (u32)bytes);
    /* The chip's texture cache may still hold lines from an older texture
       that lived at this address.  Invalidate once when a buffer has been
       retired or handed back to the allocator, not on every upload. */
    if (s_tex_need_inval) {
        GX_InvalidateTexAll();
        s_tex_need_inval = 0;
    }
    t->serial = s->tex_serial;
    t->tw = tw;
    t->th = th;
    t->fmt = fmt;
    t->native = native;
    t->buf = buf;
    t->bytes = bytes;
    t->last_use = ++s_use_tick;
    t->last_token = s_token;
    s_hint[s->tex_serial & (HINTS - 1)] = (uint16_t)(t - s_tex);
    return t;
}

/* ------------------------------------------------------ state caching

   The rasteriser calls these for every primitive; the chip only hears
   about what actually changed. */
static GxrState s_state;
static int s_state_valid;
static GXTexObj s_texobj;
static GXTlutObj s_tlutobj;
static int s_scissor[4], s_scissor_valid;

/* Draws are batched: the chip is told about one long triangle list per run
   of primitives that share the same state, instead of one GX_Begin/End per
   guest primitive (the guest issues a quarter of a million of them a
   frame).  Any state change flushes the batch first. */
#define BATCH_VERTS 2048
static GxVtx s_batch[BATCH_VERTS];   /* in the form the chip is sent */
static int s_batch_n;
static void flush_batch(void);

/* Give a converted texture back.  A batch not yet sent may still sample
   it, so that goes to the chip first. */
static void slot_free(TexSlot *t) {
    if (t->last_token == s_token) flush_batch();
    retire(t->buf, t->bytes, t->last_token);
    t->buf = NULL;
    t->tlut = NULL;
    t->serial = 0;
    s_tex_need_inval = 1;
}

/* Whether the converted copy of this serial is still here.  Only the hint
   is asked: a serial whose hint was overwritten counts as gone, which costs
   a decode, never a wrong texture. */
int wii_gxscene_tex_has(uint32_t serial) {
    const TexSlot *t = &s_tex[s_hint[serial & (HINTS - 1)]];
    return serial && t->serial == serial && t->buf;
}

/* The decoded texture this serial named was freed or replaced, so no later
   draw can ask for its converted copy again. */
void wii_gxscene_tex_forget(uint32_t serial) {
    if (!serial) return;
    TexSlot *hinted = &s_tex[s_hint[serial & (HINTS - 1)]];
    if (hinted->serial == serial && hinted->buf) {
        slot_free(hinted);
        return;
    }
    for (int i = 0; i < TEX_SLOTS; i++)
        if (s_tex[i].serial == serial && s_tex[i].buf) {
            slot_free(&s_tex[i]);
            return;
        }
}

/* Every converted texture goes back to the pool (the next draws convert
   theirs again); the rasteriser asks for this when its decoded textures
   alone cannot make room. */
int wii_gxscene_tex_trim(void) {
    uint32_t before = wii_big_used();
    flush_batch();
    STAGE(4);
    g_gx_waits++;
    wait_for_chip();
    limbo_collect();
    for (int i = 0; i < TEX_SLOTS; i++)
        if (s_tex[i].buf) slot_free(&s_tex[i]);
    s_state_valid = 0;   // the bound texture is gone: bind it again
    g_gx_epoch++;
    return wii_big_used() < before;
}

#ifdef EXP_TEVFOG
/* The GE's fog in the TEV (EXP_TEVFOG; the chip's own fog is what the build had before).

   The chip's fog works out the distance from the depth it stored, taking that depth for a perspective projection's: z =
   B - A / distance, B at least a quarter.  This game's depth is the other way round and reciprocal (the viewport's depth
   scale is negative: z = 0.0002 + 100 / distance for a near plane of 100), which no setting of the chip's fog terms can
   follow; the fog it made was a few thousandths of a percent everywhere, and a stage's haze (Ominous Mist's light grey from 0
   to 70000, on its ground and small mist strips) never showed.  The GE's fog is linear in the view-space z, so the fog
   amount is that z through the matrix of a second texture coordinate (generated from the position, no vertex data), looked
   up in a ramp texture (clamped to 0..1), and the colour is mixed with the fog colour by that value in a stage of its own,
   after the texture and before the blend, as the GE does:
       visibility = (viewZ + FOG1) * FOG2,   colour = visibility * colour + (1 - visibility) * fogcolour.
   The matrix has to follow the position matrix the draw uses (the view matrix, or a cached draw's model-view). */
#define FOGRAMP_W 256
static u8 s_fogramp[FOGRAMP_W * 4] __attribute__((aligned(32)));
static GXTexObj s_fogtex;
static int s_fogm_valid;
static float s_fogm_key[14];

static inline int fog_tev_on(const GxrState *s) { return s->fog_enable && !s->clear && s->fog_slope != 0.f; }

static void fogramp_init(void) {
    /* I8, 8x4 texels a tile, every row the same: texel x holds (x + 0.5) / 256 so that the linear filter gives the
       coordinate back over 0..1 */
    for (int x = 0; x < FOGRAMP_W; x++) {
        const u8 v = (u8)(((2 * x + 1) * 255 + 256) / 512);
        for (int y = 0; y < 4; y++) s_fogramp[(x >> 3) * 32 + y * 8 + (x & 7)] = v;
    }
    DCFlushRange(s_fogramp, sizeof s_fogramp);
    GX_InitTexObj(&s_fogtex, s_fogramp, FOGRAMP_W, 4, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&s_fogtex, GX_LINEAR, GX_LINEAR);
}

/* The matrix of texture coordinate 1 for the position matrix a draw uses (m: the GE's 4x3 layout, column by column). */
static void fog_sync(const float *m) {
    if (!fog_tev_on(&s_state)) return;
    const float k = s_state.fog_slope, e = s_state.fog_end;
    if (s_fogm_valid && s_fogm_key[12] == e && s_fogm_key[13] == k && !memcmp(s_fogm_key, m, 12 * sizeof(float))) return;
    flush_batch();
    memcpy(s_fogm_key, m, 12 * sizeof(float));
    s_fogm_key[12] = e; s_fogm_key[13] = k;
    s_fogm_valid = 1;
    Mtx t;
    t[0][0] = k * m[2]; t[0][1] = k * m[5]; t[0][2] = k * m[8]; t[0][3] = k * (m[11] + e);
    t[1][0] = 0.f; t[1][1] = 0.f; t[1][2] = 0.f; t[1][3] = 0.5f;
    t[2][0] = 0.f; t[2][1] = 0.f; t[2][2] = 0.f; t[2][3] = 1.f;
    GX_LoadTexMtxImm(t, GX_TEXMTX1, GX_MTX3x4);
}
#else
#define fog_tev_on(s_) 0
#define fog_sync(m_) ((void)0)
#endif

/* GE blend factors 10..13 use the fixed blend colour on the source side.
   The fixed-function chip has no constant blend factor, so the reference
   renderer multiplies the source in its fragment shader; do the same with a
   second TEV stage.  (The destination fixed colour has no GX equivalent and
   still falls back to ONE.) */
static void apply_fixed_source(const GxrState *s) {
    int n = 1;   /* the stages so far: the texture and lighting stage */
#ifdef EXP_TEVFOG
    const int fog = fog_tev_on(s);
    if (fog) {
        GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
        GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_C2, GX_CC_CPREV, GX_CC_TEXA, GX_CC_ZERO);   /* fogcolour + vis * (colour - fogcolour) */
        GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        n = 2;
    }
    GX_SetNumTexGens(fog ? 2 : 1);
#endif
    if (s->bl_src < 10) {
        GX_SetNumTevStages((u8)n);
        return;
    }
    GXColor fix = {(u8)(s->fix_a & 0xFF), (u8)(s->fix_a >> 8 & 0xFF),
                   (u8)(s->fix_a >> 16 & 0xFF), 255};
    GX_SetTevColor(GX_TEVREG1, fix);
    GX_SetNumTevStages((u8)(n + 1));
    GX_SetTevOrder((u8)n, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevColorIn((u8)n, GX_CC_ZERO, GX_CC_CPREV, GX_CC_C1, GX_CC_ZERO);
    GX_SetTevAlphaIn((u8)n, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GX_SetTevColorOp((u8)n, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaOp((u8)n, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

/* What the stage needs: no texture, or a texture and how to combine it. */
static void apply_tev(const GxrState *s, int textured) {
    g_gx_tev_sets++;
    if (s->clear) {
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        GX_SetNumTevStages(1);
#ifdef EXP_TEVFOG
        GX_SetNumTexGens(1);
#endif
        return;
    }
#ifdef EXP_DARKDBG
    if (g_dbg_stage & 4) textured = 0;
#endif
    if (!textured) {
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        if (s->dbl_src_alpha) {
            /* The blend factors 6/7 multiply the source alpha by two.  The
               TEV alpha output is what the blend factor uses; scale it here
               without changing the RGB result. */
            GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO,
                             GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
        }
        apply_fixed_source(s);
        return;
    }
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    int rgba = s->tex_rgba;
    /* TEV: out = d + (1 - c) * a + c * b */
    switch (s->tex_func) {
    case 1:   /* decal */
        if (rgba) GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_RASC, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO);
        else      GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
        break;
    case 2: { /* blend with the environment colour */
        GXColor env = {(u8)(s->tex_env & 0xFF), (u8)(s->tex_env >> 8 & 0xFF),
                       (u8)(s->tex_env >> 16 & 0xFF), 255};
        GX_SetTevColor(GX_TEVREG0, env);
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_RASC, GX_CC_C0, GX_CC_TEXC, GX_CC_ZERO);
        if (rgba) GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
        else      GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
        break;
    }
    case 3:   /* replace */
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        if (rgba) GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
        else      GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
        break;
    case 4:   /* add */
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_TEXC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
        if (rgba) GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
        else      GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
        break;
    default:  /* modulate */
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
        if (rgba) GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
        else      GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
        break;
    }
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO,
                     s->tex_dbl ? GX_CS_SCALE_2 : GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO,
                     s->dbl_src_alpha ? GX_CS_SCALE_2 : GX_CS_SCALE_1,
                     GX_TRUE, GX_TEVPREV);
    apply_fixed_source(s);
}

/* The GE computes a per-vertex visibility `v = (viewZ + fog1) * fog2`,
   with viewZ negative in front of the camera.  GX's linear fog instead takes
   positive eye-space distances and a fog amount rising from 0 to 1.  For
   the usual positive fog2: v = 1 at distance fog1 - 1/fog2 and v = 0 at
   distance fog1, so those are the GX start and end.  (The old mapping passed
   the negative view-space Z straight through, which fogged the entire
   scene with the fog colour.) */
/* GX_SetFog works out the chip's fixed-point fog terms with several
   divisions each time; the fog rarely changes while the rest of the pixel
   state does, so it is only sent when it differs. */
static struct { int valid, type; float start, end, farz; uint32_t color; } s_fog;
static void set_fog(int type, float start, float end, float farz, uint32_t color) {
    if (s_fog.valid && s_fog.type == type && s_fog.start == start && s_fog.end == end &&
        s_fog.farz == farz && s_fog.color == color)
        return;
    s_fog.valid = 1; s_fog.type = type; s_fog.start = start; s_fog.end = end;
    s_fog.farz = farz; s_fog.color = color;
#ifdef EXP_FOGSUB
    {
        extern uint32_t g_frames_presented;
        static int n;
        static float ls, le;
        static uint32_t lc;
        if (type != GX_FOG_NONE && (start != ls || end != le || color != lc) && n++ < 400 && ((ls = start), (le = end), (lc = color), 1))
            fprintf(stderr, "FOGSET f=%u type=%d start=%g end=%g farz=%g color=%06X\n", g_frames_presented, type, start, end, farz, color);
    }
#endif
    GXColor c = {(u8)(color & 0xFF), (u8)(color >> 8 & 0xFF), (u8)(color >> 16 & 0xFF), 255};
    if (type == GX_FOG_NONE) c.a = 0;
    GX_SetFog((u8)type, start, end, type == GX_FOG_NONE ? 0.f : 1.f, farz, c);
}

static void apply_fog(const GxrState *s) {
#ifdef EXP_TEVFOG
    /* the chip's own fog stays off; the colour is the TEV stage's (apply_fixed_source()) */
    set_fog(GX_FOG_NONE, 0.f, 0.f, 0.f, 0);
    if (fog_tev_on(s)) {
        const uint32_t fc = s->fog_color;
        GXColor col = {(u8)(fc & 0xFF), (u8)(fc >> 8 & 0xFF), (u8)(fc >> 16 & 0xFF), 255};
        GX_SetTevColor(GX_TEVREG2, col);
    }
    return;
#endif
    if (!s->fog_enable || s->clear || s->fog_slope == 0.f) {
        set_fog(GX_FOG_NONE, 0.f, 0.f, 0.f, 0);
        return;
    }
    float start = s->fog_end - 1.f / s->fog_slope;
    float end = s->fog_end;
    if (s->fog_slope < 0.f) {
        /* The same relation with the sign reversed: visibility rises away
           from the camera.  Keep GX's start/end ordered; the exact direction
           is handled by the GE vertex factor in the CPU path. */
        start = s->fog_end + 1.f / -s->fog_slope;
        end = s->fog_end;
    }
    if (start > end) { float t = start; start = end; end = t; }
    if (end <= start) end = start + 1.f;
    float farz = end > 100000.f ? end * 2.f : 100000.f;
    set_fog(GX_FOG_LIN, start, end, farz, s->fog_color & 0xFFFFFFu);
}

static void apply_pixel(const GxrState *s) {
    g_gx_pix_sets++;
    {   /* the triangles the GE rejects (GxrState.cull): what the chip calls the front face is the GE's
           positive screen area, as the projection sends them to it (found by trying the other way: the
           stages vanish) */
        static const u8 k_cull[3] = {GX_CULL_NONE, GX_CULL_FRONT, GX_CULL_BACK};
        GX_SetCullMode(k_cull[s->cull < 3 ? s->cull : 0]);
    }
    apply_fog(s);
    GX_SetDither(s->dither ? GX_TRUE : GX_FALSE);
    if (s->clear) {
        /* The GE's clear writes the colour and the depth over the
           primitive regardless of the depth test, the blend and the
           texture. */
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZMode(GX_TRUE, GX_ALWAYS, s->clear_depth ? GX_TRUE : GX_FALSE);
        GX_SetColorUpdate(s->clear_color ? GX_TRUE : GX_FALSE);
        /* The destination alpha is part of the GE's framebuffer and several
           of the game's card/glow effects blend with it.  The chip only
           keeps it when the EFB is RGBA6 and the clear writes it. */
        GX_SetDstAlpha(GX_FALSE, 0);
        GX_SetAlphaUpdate(s->clear_alpha ? GX_TRUE : GX_FALSE);
        return;
    }
    if (s->logic) {
        /* The GE logic operations have the same numbering as the GX's. */
        GX_SetBlendMode(GX_BM_LOGIC, GX_BL_ONE, GX_BL_ZERO, (u8)s->logic_op);
    } else if (s->blend) {
        /* The GX's subtract is destination minus source; the GE's reverse
           subtract (equation 2) is the same thing.  The other equations
           have no GX counterpart and fall back to the plain blend. */
        GX_SetBlendMode(s->bl_eq == 2 ? GX_BM_SUBTRACT : GX_BM_BLEND,
                        blend_factor(s->bl_src, 1, s->fix_a), blend_factor(s->bl_dst, 0, s->fix_b),
                        GX_LO_CLEAR);
    } else {
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    }
    if (s->alpha_test) {
        /* The GE compares the masked alpha with the masked reference; the
           GX compares the alpha directly (the mask is all ones in
           practice). */
        GX_SetAlphaCompare(k_cmp[s->at_func & 7], (u8)s->at_ref, GX_AOP_AND, GX_ALWAYS, 0);
    } else {
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    }
    GX_SetZMode(s->z_test ? GX_TRUE : GX_FALSE, k_cmp[s->z_func & 7],
                (s->z_test && s->z_write) ? GX_TRUE : GX_FALSE);
#ifdef EXP_DARKDBG
    if (g_dbg_stage & 1) GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
#endif
    GX_SetColorUpdate(s->rgb_write ? GX_TRUE : GX_FALSE);
    /* Preserve the destination alpha, as the GL reference does with
       glBlendFuncSeparate(src, dst, GL_ZERO, GL_ONE), unless an alpha-only
       write or the GE stencil operation writes it.  GX's alpha compare and
       blend still use the source alpha; only the stored alpha changes. */
    if (s->stencil_alpha >= 0) {
        GX_SetAlphaUpdate(GX_TRUE);
        GX_SetDstAlpha(GX_TRUE, (u8)s->stencil_alpha);
    } else if (s->alpha_write) {
        GX_SetAlphaUpdate(GX_TRUE);
        GX_SetDstAlpha(GX_FALSE, 0);
    } else {
        GX_SetAlphaUpdate(GX_FALSE);
        GX_SetDstAlpha(GX_FALSE, 0);
    }
}

static int pixel_differs(const GxrState *a, const GxrState *b) {
    return a->clear != b->clear || a->clear_color != b->clear_color ||
           a->clear_alpha != b->clear_alpha || a->clear_depth != b->clear_depth ||
           a->blend != b->blend ||
           a->bl_eq != b->bl_eq || a->bl_src != b->bl_src || a->bl_dst != b->bl_dst ||
           (a->bl_dst >= 10 && a->fix_b != b->fix_b) ||
           a->alpha_test != b->alpha_test || a->at_func != b->at_func ||
           a->at_ref != b->at_ref || a->at_mask != b->at_mask ||
           a->fog_enable != b->fog_enable || a->fog_end != b->fog_end ||
           a->fog_slope != b->fog_slope || a->fog_color != b->fog_color ||
           a->logic != b->logic || a->logic_op != b->logic_op ||
           a->dither != b->dither || a->rgb_write != b->rgb_write ||
           a->alpha_write != b->alpha_write ||
           a->stencil_alpha != b->stencil_alpha ||
           a->z_test != b->z_test || a->z_func != b->z_func ||
           a->z_write != b->z_write || a->cull != b->cull;
}

static int tev_differs(const GxrState *a, int a_tex, const GxrState *b, int b_tex) {
    if (a_tex != b_tex) return 1;
    /* The fixed-source TEV stage depends on these even without a texture. */
    if (a->bl_src != b->bl_src || (a->bl_src >= 10 && a->fix_a != b->fix_a)) return 1;
    if (a->dbl_src_alpha != b->dbl_src_alpha) return 1;
    if (fog_tev_on(a) != fog_tev_on(b)) return 1;
    if (!a_tex) return 0;
    return a->tex_func != b->tex_func || a->tex_rgba != b->tex_rgba ||
           a->tex_dbl != b->tex_dbl || (a->tex_func == 2 && a->tex_env != b->tex_env);
}

static int is_pow2(int v) { return v > 0 && (v & (v - 1)) == 0; }

/* The game sometimes draws one of its display buffers back over the scene as a texture, a full-screen blit with the
   blend off, once a frame for tens of seconds (seen: into the buffer it samples, which changes nothing on a PSP; why
   it does this is not known).  The chip draws into its own frame buffer and never
   writes the guest's VRAM, so those two 512x512 buffers hold nothing, the texture decodes to black, and the blit
   painted the whole 3D scene black with only the HUD drawn after it left (docs/HANDOVER.md B6; found with TEXPROBE,
   the two black 512x512 textures were at 04000000 and 04044000).  The picture so far is copied out of the chip's frame
   buffer into a texture instead, and the blit draws that. */
static uint8_t *s_fbtex;     /* the copy of the picture, RGB565 like the 16-bit buffer it stands for */
static int s_fbtex_w, s_fbtex_h;
static uint32_t s_fbtex_binds;

static int is_display_buffer(const GxrState *s) {
    extern int gpu_disp_info(uint32_t *a);
    if (s->tex_native || s->tex_fmt > 3 || s->through == 0 || s->tex_w < PSP_W || s->tex_h < PSP_H) return 0;
    uint32_t da[4];
    const int n = gpu_disp_info(da);
    for (int i = 0; i < n; i++)
        if (da[i] && (da[i] & 0x1FFFFFFFu) == (s->tex_addr & 0x1FFFFFFFu)) return 1;
    return 0;
}

static int bind_display_copy(const GxrState *s) {
    const int w = ((int)(s_lb_x1 - s_lb_x0 + 0.5f)) & ~3, h = ((int)(s_lb_y1 - s_lb_y0 + 0.5f)) & ~1;
    if (!s_fbtex) {
        s_fbtex = tex_mem(GX_GetTexBufferSize((u16)w, (u16)h, GX_TF_RGB565, GX_FALSE, 0), NULL);
        if (!s_fbtex) return 0;
        s_fbtex_w = w;
        s_fbtex_h = h;
    }
    flush_batch();
    GX_SetTexCopySrc((u16)((int)(s_lb_x0 + 0.5f) & ~1), (u16)((int)(s_lb_y0 + 0.5f) & ~1), (u16)s_fbtex_w, (u16)s_fbtex_h);
    GX_SetTexCopyDst((u16)s_fbtex_w, (u16)s_fbtex_h, GX_TF_RGB565, GX_FALSE);
    GX_CopyTex(s_fbtex, GX_FALSE);
    GX_PixModeSync();
    GX_InvalidateTexAll();
    const u8 filt = s->tex_linear ? GX_LINEAR : GX_NEAR;
    GX_InitTexObj(&s_texobj, s_fbtex, (u16)s_fbtex_w, (u16)s_fbtex_h, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&s_texobj, filt, filt, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GX_LoadTexObj(&s_texobj, GX_TEXMAP0);
    /* the picture is the guest's 480x272 texels, in the "through" mode's texel coordinates */
    Mtx tm;
    memset(tm, 0, sizeof tm);
    tm[0][0] = 1.0f / (float)PSP_W;
    tm[1][1] = 1.0f / (float)PSP_H;
    tm[2][2] = 1.0f;
    GX_LoadTexMtxImm(tm, GX_TEXMTX0, GX_MTX2x4);
#ifdef TKDR_AUTOPILOT
    if (s_fbtex_binds++ < 12 || s_fbtex_binds % 600 == 0) {
        extern uint32_t g_tgt_fb;
        fprintf(stderr, "DISPCOPY %u: the picture so far stands in for the texture at %08X (fmt %d, %dx%d, func %d rgba %d blend %d z %d), drawing into %08X, "
                "colour write %d alpha write %d stencil %d alpha test %d logic %d, draw %u\n",
                s_fbtex_binds, s->tex_addr, s->tex_fmt, s->tex_w, s->tex_h, s->tex_func, s->tex_rgba, s->blend, s->z_test, g_tgt_fb,
                s->rgb_write, s->alpha_write, s->stencil_alpha, s->alpha_test, s->logic, g_gx_draws);
    }
#else
    s_fbtex_binds++;
#endif
    return 1;
}

/* Make the texture current and set the matrix that takes the guest's
   texture coordinates to the GX's. */
static int bind_texture(const GxrState *s) {
    STAGE(2);
    g_gx_binds++;
    if (is_display_buffer(s) && bind_display_copy(s)) return 1;
#ifdef EXP_DARKDBG
    if (g_dbg_stage & 8) {   /* a white 4x4 texture in place of the converted one: texels, or the texture unit? */
        static u8 white[64] __attribute__((aligned(32)));
        static GXTexObj wobj;
        memset(white, 0xFF, sizeof white);
        DCFlushRange(white, sizeof white);
        GX_InvalidateTexAll();
        GX_InitTexObj(&wobj, white, 4, 4, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjLOD(&wobj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
        GX_LoadTexObj(&wobj, GX_TEXMAP0);
        Mtx wm;
        memset(wm, 0, sizeof wm);
        wm[0][0] = 0.5f;
        wm[1][1] = 0.5f;
        wm[2][2] = 1.0f;
        GX_LoadTexMtxImm(wm, GX_TEXMTX0, GX_MTX2x4);
        return 1;
    }
#endif
    TexSlot *t = tex_get(s);
    if (!t) return 0;
#ifdef TKDR_AUTOPILOT
    for (uint32_t z = 0; z < s_zero_n; z++)
        if (t->serial == s_zero_serial[z]) {
            s_zero_binds++;
            if (s_zero_logged++ < 12)
                fprintf(stderr, "ZEROBIND serial %u addr %08X fmt %d %dx%d (declared %dx%d) through %d func %d rgba %d env %06X blend %d eq %d src %d dst %d "
                        "fix %06X/%06X z %d/%d/%d atest %d/%d/%d cull %d clear %d after %u draws\n",
                        t->serial, s->tex_addr, s->tex_fmt, s->tex_pw, s->tex_ph, s->tex_w, s->tex_h, s->through, s->tex_func, s->tex_rgba, s->tex_env,
                        s->blend, s->bl_eq, s->bl_src, s->bl_dst, s->fix_a, s->fix_b, s->z_test, s->z_func, s->z_write,
                        s->alpha_test, s->at_func, s->at_ref, s->cull, s->clear, g_gx_draws);
        }
#endif
    u8 wrap_s = (s->tex_clamp_u || !is_pow2(t->tw)) ? GX_CLAMP : GX_REPEAT;
    u8 wrap_t = (s->tex_clamp_v || !is_pow2(t->th)) ? GX_CLAMP : GX_REPEAT;
    u8 filt = s->tex_linear ? GX_LINEAR : GX_NEAR;
    if (t->native && t->tlut) {
        /* A paletted texture must have its TLUT loaded before the texture
           object is bound.  The palette is part of the texture serial, so a
           palette change creates a different TexSlot.  The object has to be
           made with GX_InitTexObjCI: GX_InitTexObj marks it non-paletted,
           GX_InitTexObjTlut only records the TLUT name, and GX_LoadTexObj
           then never attaches the TLUT, so the texture sampled as black. */
        GX_InitTlutObj(&s_tlutobj, t->tlut, t->tlut_fmt, 256);
        GX_LoadTlut(&s_tlutobj, GX_TLUT0);
        GX_InitTexObjCI(&s_texobj, t->buf, (u16)t->tw, (u16)t->th, t->fmt, wrap_s, wrap_t,
                        GX_FALSE, GX_TLUT0);
    } else {
        GX_InitTexObj(&s_texobj, t->buf, (u16)t->tw, (u16)t->th, t->fmt, wrap_s, wrap_t, GX_FALSE);
    }
    GX_InitTexObjLOD(&s_texobj, filt, filt, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GX_LoadTexObj(&s_texobj, GX_TEXMAP0);
    /* The coordinates arrive in texels in the "through" mode and
       normalised in the transform mode; the GX wants them normalised to
       the size it stores, which is the decoded size rounded up to a
       multiple of four. */
    float su = (float)s->tex_pw / (float)t->tw, sv = (float)s->tex_ph / (float)t->th;
    if (s->through) {
        su /= (float)(s->tex_w > 0 ? s->tex_w : s->tex_pw);
        sv /= (float)(s->tex_h > 0 ? s->tex_h : s->tex_ph);
    }
    Mtx tm;
    memset(tm, 0, sizeof tm);
    tm[0][0] = su;
    tm[1][1] = sv;
    tm[2][2] = 1.0f;
    GX_LoadTexMtxImm(tm, GX_TEXMTX0, GX_MTX2x4);
    return 1;
}

#ifdef EXP_ZBIAS
static int s_zbias;                      /* the way the chip's depth range is shifted now: -1, 0, 1 */
static void zbias_set(int dir);
#endif

#ifdef EXP_BLENDLOG
/* EXP_BLENDLOG: the texels (as the chip is given them) of every texture a blended 3D draw uses from frame 4200 on, once each, one
   row a line: TEXPIX/TEXROW as EXP_NEARLOG's, for whichever layers draw around the fighters. */
extern uint32_t g_frames_presented;
static void blendlog_tex(const GxrState *s) {
    static uint32_t seen[96];
    static int nseen;
    if (g_frames_presented < 4200 || !s->tex_on || !s->tex_pix || !s->blend || s->through || s->clear) return;
    for (int k = 0; k < nseen; k++) if (seen[k] == s->tex_addr) return;
    if (nseen >= 96) return;
    seen[nseen++] = s->tex_addr;
    fprintf(stderr, "TEXPIX tex=%08X pix=%dx%d fmt=%d clut=%d\n", s->tex_addr, s->tex_pw, s->tex_ph, s->tex_fmt, s->tex_clut_fmt);
    for (int y = 0; y < s->tex_ph; y++) {
        fprintf(stderr, "TEXROW %08X %d ", s->tex_addr, y);
        for (int x = 0; x < s->tex_pw; x++) {
            const uint32_t p = s->tex_pix[y * s->tex_pw + x];
            fprintf(stderr, "%02X%02X%02X%02X", p & 255, p >> 8 & 255, p >> 16 & 255, p >> 24);
        }
        fprintf(stderr, "\n");
    }
}
#endif

void wii_gxscene_set_state(const GxrState *s) {
    if (!s_ready) return;
    /* The rasteriser hands the same state again for most primitives: then
       there is nothing to compare. */
    if (s_state_valid && s->version && s->version == s_state.version) return;
    g_gx_epoch++;
#ifdef EXP_ZBIAS
    if (s->zbias != s_zbias) zbias_set(s->zbias);
#endif
    STAGE(1);
    /* tex_on: decoded texels, or the converted copy that stands in for them */
    int textured = s->tex_on && !s->clear;
    int old_textured = s_state_valid && s_state.tex_on && !s_state.clear;
    int tex_changed = textured && (!s_state_valid || !old_textured ||
                                   s_state.tex_serial != s->tex_serial ||
                                   s_state.tex_native != s->tex_native ||
                                   s_state.tex_fmt != s->tex_fmt ||
                                   s_state.tex_clamp_u != s->tex_clamp_u ||
                                   s_state.tex_clamp_v != s->tex_clamp_v ||
                                   s_state.tex_linear != s->tex_linear ||
                                   s_state.through != s->through ||
                                   s_state.tex_w != s->tex_w || s_state.tex_h != s->tex_h ||
                                   s_state.tex_pw != s->tex_pw || s_state.tex_ph != s->tex_ph);
    if (!s_state_valid || pixel_differs(s, &s_state)) {
        flush_batch();
        apply_pixel(s);
    }
    if (textured && tex_changed) {
        flush_batch();
        if (!bind_texture(s)) {
            /* No texture memory: draw the primitive with its vertex colour
               rather than not at all. */
            GxrState plain = *s;
            plain.tex_on = 0;
            apply_tev(&plain, 0);
            s_state = *s;
            s_state_valid = 0;   /* the chip's stage does not match the state */
            STAGE(0);
            return;
        }
    }
    if (!s_state_valid || tev_differs(s, textured, &s_state, old_textured)) {
        flush_batch();
        apply_tev(s, textured);
    }
    s_state = *s;
    s_state_valid = 1;
#ifdef EXP_BLENDLOG
    blendlog_tex(s);
#endif
    STAGE(0);
}

/* ------------------------------------------------- matrices, viewport */

/* The projection matrix in the form the GX can load.

   The GX's projection is not a general 4x4.  Perspective:
       x' = p0 x + p1 z,  y' = p2 y + p3 z,  z' = p4 z + p5,  w' = -z
   and orthographic:
       x' = o0 x + o1,    y' = o2 y + o3,    z' = o4 z + o5,  w' = 1.
   Its clip volume has z between -w and 0, where the GE's runs from -w to
   w, so z' is remapped to (z - w) / 2.  A GE projection of the usual
   form has a bottom row of (0, 0, s, 0) (perspective) or (0, 0, 0, 1)
   (orthographic); dividing the perspective one through by -s makes
   w' = -z.  The signs of the viewport's scales are folded in: the GX
   puts +1 at the right and at the top. */
static void build_projection(const float *P, float xs, float ys, Mtx44 out, u8 *type) {
    float sx = xs < 0.f ? -1.f : 1.f;
    float sy = ys > 0.f ? -1.f : 1.f;
    memset(out, 0, sizeof(Mtx44));
    int persp = fabsf(P[11]) > 1e-6f;
    if (persp) {
        float k = -1.0f / P[11];
        out[0][0] = k * P[0] * sx;
        out[0][2] = k * P[8] * sx;
        out[1][1] = k * P[5] * sy;
        out[1][2] = k * P[9] * sy;
        out[2][2] = 0.5f * (k * P[10] + 1.0f);
        out[2][3] = 0.5f * k * P[14];
        out[3][2] = -1.0f;
        *type = GX_PERSPECTIVE;
    } else {
        out[0][0] = P[0] * sx;
        out[0][3] = P[12] * sx;
        out[1][1] = P[5] * sy;
        out[1][3] = P[13] * sy;
        out[2][2] = 0.5f * P[10];
        out[2][3] = 0.5f * (P[14] - 1.0f);
        out[3][3] = 1.0f;
        *type = GX_ORTHOGRAPHIC;
    }
}

static float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }

/* What the chip is currently set up for: 0 nothing, 2 screen space,
   3 world space (with this view, projection and viewport). */
static int s_space;

#ifdef EXP_ZBIAS
/* The depth bias (GxrState.zbias).  The PSP's depth buffer has 16 bits and a test that ties passes; the chip's has 24.
   A blended layer that does not write depth and lies on the surface under it (ground mist, a decal) passes by a tie on
   the PSP and is a coin toss on the chip.  Those draws are made with the depth range of the viewport moved toward the
   eye by EXP_ZBIAS_Q steps of the PSP's 16 bits (default 1.5: the most a tie can hide is one step), which moves every
   depth they produce by the same amount; the clip planes are the projection's and do not move.  The viewport is sent
   again when the direction changes and only then. */
#if defined(EXP_FOGDBG) || defined(EXP_ZBQCYCLE)
float g_zbias_q = 0.0f;   /* the fog tests (raster.c fogdbg_pre(), zbq_cycle()) set the bias in steps per window */
#define EXP_ZBIAS_Q g_zbias_q
#endif
#ifndef EXP_ZBIAS_Q
#define EXP_ZBIAS_Q 1.5f
#endif
static float s_vp_cur[6];   /* the viewport the chip was given, before the shift: x, y, width, height, near z, far z */
static void load_viewport(float x, float y, float w, float h, float zn, float zf) {
    s_vp_cur[0] = x; s_vp_cur[1] = y; s_vp_cur[2] = w; s_vp_cur[3] = h; s_vp_cur[4] = zn; s_vp_cur[5] = zf;
    if (s_zbias) {
        const float d = (float)s_zbias * (EXP_ZBIAS_Q / 65535.0f);
        zn = clamp01(zn + d);
        zf = clamp01(zf + d);
    }
    GX_SetViewport(x, y, w, h, zn, zf);
}
static void zbias_set(int dir) {
    flush_batch();   /* what waits was drawn for the old range */
    g_gx_epoch++;
    s_zbias = dir;
    if (s_space == 2 || s_space == 3)
        load_viewport(s_vp_cur[0], s_vp_cur[1], s_vp_cur[2], s_vp_cur[3], s_vp_cur[4], s_vp_cur[5]);
}
#else
#define load_viewport(x_, y_, w_, h_, zn_, zf_) GX_SetViewport((x_), (y_), (w_), (h_), (zn_), (zf_))
#endif
static float s_w_view[12], s_w_proj[16], s_w_vp[8];
static uint32_t s_w_gen;   // the g_ge_view_gen they are from

/* vp: x scale, x centre, y scale, y centre, z scale, z centre, x offset, y offset */
void wii_gxscene_set_world(const float *view, const float *proj, const float *vp, uint32_t gen) {
    if (!s_ready) return;
    if (s_space == 3 && gen == s_w_gen) return;   // nothing they are made from was written
    s_w_gen = gen;
    if (s_space == 3 && !memcmp(view, s_w_view, sizeof s_w_view) &&
        !memcmp(proj, s_w_proj, sizeof s_w_proj) && !memcmp(vp, s_w_vp, sizeof s_w_vp))
        return;
    flush_batch();
    g_gx_epoch++;
    g_gx_world_sets++;
    memcpy(s_w_view, view, sizeof s_w_view);
    memcpy(s_w_proj, proj, sizeof s_w_proj);
    memcpy(s_w_vp, vp, sizeof s_w_vp);
    s_space = 3;

    /* The GE's view matrix is 4x3, stored column by column; the GX's
       position matrix is 3x4, stored row by row. */
    Mtx mv;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) mv[r][c] = view[c * 3 + r];
    GX_LoadPosMtxImm(mv, GX_PNMTX0);

    Mtx44 p;
    u8 type;
    build_projection(proj, vp[0], vp[2], p, &type);
    GX_LoadProjectionMtx(p, type);

    float xs = fabsf(vp[0]), ys = fabsf(vp[2]);
    float vx = s_lb_x0 + (vp[1] - vp[6] - xs) * s_kx;
    float vy = s_lb_y0 + (vp[3] - vp[7] - ys) * s_ky;
    /* The GE's depth runs over 0..65535; the GX's over 0..1.  The ends of
       the range are where the clip volume's z = -w and z = w land. */
    float zn = clamp01((vp[5] - vp[4]) * (1.0f / 65535.0f));
    float zf = clamp01((vp[5] + vp[4]) * (1.0f / 65535.0f));
    load_viewport(vx, vy, 2.0f * xs * s_kx, 2.0f * ys * s_ky, zn, zf);
}

/* Draws from the cache keep their vertices in object space; they are drawn
   with a model-view matrix of their own in the chip's second position matrix
   slot, and everything else with the view matrix in the first. */
static int s_cur_mtx;          /* the slot the chip draws with */
static int s_rg_valid;         /* the second slot holds the matrices of a rigid draw (s_rg_ver) */
static int s_model_valid;
static float s_model[12];      /* what the second slot holds, in the GE's layout */
static uint32_t s_model_ver;   /* the g_mv_version it was loaded from */

static void use_view_mtx(void) {
    if (!s_cur_mtx) return;
    flush_batch();
    GX_SetCurrentMtx(GX_PNMTX0);
    s_cur_mtx = 0;
}

static void use_model_mtx(const float *mv) {
    /* The rasteriser's one model-view matrix has a version that moves when
       it is worked out again: until then it is what the slot holds. */
    if (!s_model_valid || s_model_ver != g_mv_version) {
        s_model_ver = g_mv_version;
        if (!s_model_valid || memcmp(s_model, mv, sizeof s_model)) {
            flush_batch();
            Mtx m;
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 4; c++) m[r][c] = mv[c * 3 + r];
            GX_LoadPosMtxImm(m, GX_PNMTX1);
            memcpy(s_model, mv, sizeof s_model);
            s_model_valid = 1;
            s_rg_valid = 0;
            g_gx_world_sets++;
        }
    }
#ifdef EXP_HITCHK
    {   /* the slot must hold the matrix this draw asked for */
        extern uint32_t g_chk_gx_n, g_chk_gx_bad;
        g_chk_gx_n++;
        if (memcmp(s_model, mv, sizeof s_model) && ++g_chk_gx_bad <= 4) fprintf(stderr, "HITCHK GX SLOT STALE\n");
    }
#endif
    if (s_cur_mtx != 1) {
        flush_batch();
        GX_SetCurrentMtx(GX_PNMTX1);
        s_cur_mtx = 1;
    }
}

void wii_gxscene_set_screen(void) {
    if (!s_ready || s_space == 2) return;
    flush_batch();
    g_gx_epoch++;
    s_space = 2;
    Mtx id;
    guMtxIdentity(id);
    GX_LoadPosMtxImm(id, GX_PNMTX0);
    Mtx44 o;
    guOrtho(o, 0.0f, (f32)PSP_H, 0.0f, (f32)PSP_W, 0.0f, 1.0f);
    GX_LoadProjectionMtx(o, GX_ORTHOGRAPHIC);
    load_viewport(s_lb_x0, s_lb_y0, s_lb_x1 - s_lb_x0, s_lb_y1 - s_lb_y0, 0.0f, 1.0f);
}

void wii_gxscene_set_scissor(int x0, int y0, int x1, int y1) {
    if (!s_ready) return;
    int key[4] = {x0, y0, x1, y1};
    if (s_scissor_valid && !memcmp(key, s_scissor, sizeof key)) return;
    flush_batch();
    g_gx_epoch++;
    memcpy(s_scissor, key, sizeof key);
    s_scissor_valid = 1;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    float fw = (float)s_mode_obj->fbWidth, fh = (float)s_mode_obj->efbHeight;
    float sx = s_lb_x0 + (float)x0 * s_kx, sy = s_lb_y0 + (float)y0 * s_ky;
    float ex = s_lb_x0 + (float)(x1 + 1) * s_kx, ey = s_lb_y0 + (float)(y1 + 1) * s_ky;
    if (sx < 0.f) sx = 0.f;
    if (sy < 0.f) sy = 0.f;
    if (ex > fw) ex = fw;
    if (ey > fh) ey = fh;
    if (ex < sx) ex = sx;
    if (ey < sy) ey = sy;
    GX_SetScissor((u32)(sx + 0.5f), (u32)(sy + 0.5f), (u32)(ex - sx + 0.5f), (u32)(ey - sy + 0.5f));
}

/* --------------------------------------------------------- drawing */

static inline u8 c8(float v) {
#ifdef EXP_DARKDBG
    if (g_dbg_stage & 2) return 255;
#endif
    return (u8)(v <= 0.f ? 0.f : v >= 255.f ? 255.f : v + 0.5f);
}

/* Append one vertex to the batch, converting the screen-space depth to the
   eye-space distance the screen ortho expects. */
static inline void batch_vertex(const float *p, int screen) {
    GxVtx *o = s_batch + s_batch_n;
    o->x = p[0]; o->y = p[1];
    o->z = screen ? -clamp01(p[2] * (1.0f / 65535.0f)) : p[2];
    o->u = p[3]; o->v = p[4];
    o->rgba = (u32)c8(p[5]) << 24 | (u32)c8(p[6]) << 16 | (u32)c8(p[7]) << 8 | c8(p[8]);
    s_batch_n++;
}

static void flush_batch(void) {
    if (!s_batch_n) return;
    g_gx_batches++;
    GX_Begin(GX_TRIANGLES, GX_VTXFMT1, (u16)s_batch_n);
    for (int i = 0; i < s_batch_n; i++) {
        const GxVtx *p = s_batch + i;
        GX_Position3f32(p->x, p->y, p->z);
        GX_Color1u32(p->rgba);
        GX_TexCoord2f32(p->u, p->v);
    }
    GX_End();
    s_batch_n = 0;
}

#ifdef TKDR_AUTOPILOT
/* Debug: the draws in screen space that cover most of the picture (a fade or a flash is one), in order, with the
   colour of their first vertex.  Printed with the dark-frame report. */
static struct { uint32_t after; float w, h; uint32_t rgba; } s_big[8];
static int s_big_n, s_big_over;
static uint32_t s_big_draws;
static void big_note(const float *v, int count) {
    s_big_draws++;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < count; i++) {
        const float *p = v + i * 9;
        if (p[0] < x0) x0 = p[0];
        if (p[0] > x1) x1 = p[0];
        if (p[1] < y0) y0 = p[1];
        if (p[1] > y1) y1 = p[1];
    }
    if (x1 - x0 < 400.f || y1 - y0 < 220.f) return;
    if (s_big_n >= 8) { s_big_over++; return; }
    s_big[s_big_n].after = s_big_draws;
    s_big[s_big_n].w = x1 - x0;
    s_big[s_big_n].h = y1 - y0;
    s_big[s_big_n].rgba = (uint32_t)v[5] << 24 | (uint32_t)v[6] << 16 | (uint32_t)v[7] << 8 | (uint32_t)v[8];
    s_big_n++;
}
static void big_dump(const char *why) {
    fprintf(stderr, "BIGQUADS %s: %u screen draws, %d covering the picture%s:", why, s_big_draws, s_big_n + s_big_over, s_big_over ? " (more not listed)" : "");
    for (int i = 0; i < s_big_n; i++)
        fprintf(stderr, " [draw %u, %gx%g, rgba %08X]", s_big[i].after, (double)s_big[i].w, (double)s_big[i].h, s_big[i].rgba);
    fprintf(stderr, "\n");
}
#endif

#ifdef EXP_GXLIGHT
/* ------------------------------------------------ lighting by the chip

   A skinned fighter lit by up to three directional lights can be sent with its
   normal in place of its colour (GxVtxL), and the chip works the colour out: the
   channel is lit with the lights below, the ambient colour is the flattened
   lighting's base, and the material is white.  The chip takes the direction of
   a light with no attenuation from its *position*, so a light at a million
   units along the direction towards it is a directional light; the normals are
   kept in world space (the normal matrix is the identity) and so are the light
   positions, since a million units swamps the view transform of the vertex.
   Everything else is drawn with the vertex colour, so the chip changes between
   the two: the vertex description and the channel (lit_leave() below puts
   them back, and every other draw does that first). */
#define LIT_FAR 1048576.0f
#ifdef EXP_LITTEST
static GxLit s_test_lit, s_test_done;   /* the lights of the latest lit draw; those the chip was last tested with */
static int s_test_have, s_test_runs;
static uint32_t s_test_lists;
#endif
static int s_lit;            /* the chip is set up for lit vertices */
static int s_lit_valid;      /* the lights the chip holds are s_lit_cur */
static GxLit s_lit_cur;
static uint32_t s_lit_gen;   /* the generation s_lit_cur was asked for */
uint32_t g_gx_lit_draws, g_gx_lit_enters, g_gx_lit_loads;

/* The whole vertex description is set again each time: libogc remembers that a draw ever had normals and tells
   the transform unit to expect them even after GX_SetVtxDesc(GX_VA_NRM, GX_NONE) (Dolphin: "Mismatched
   configuration between CP and XF stages"); only GX_ClearVtxDesc() forgets that. */
static void lit_leave(void) {
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    s_lit = 0;
}
#define LIT_LEAVE() do { if (s_lit) lit_leave(); } while (0)

static void lit_ctrl(int n) {
    const u8 mask = (u8)((1 << n) - 1);
    GX_SetChanCtrl(GX_COLOR0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, mask, GX_DF_CLAMP, GX_AF_NONE);
    GX_SetChanCtrl(GX_ALPHA0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
}

static void lit_load(const GxLit *l, uint32_t gen) {
    s_lit_gen = gen;
    if (s_lit_valid && !memcmp(l, &s_lit_cur, sizeof *l)) return;
    g_gx_lit_loads++;
    for (int j = 0; j < l->n; j++) {
        GXLightObj lo;
        GXColor c = {l->col[j][0], l->col[j][1], l->col[j][2], 255};
        GX_InitLightPos(&lo, l->dir[j][0] * LIT_FAR, l->dir[j][1] * LIT_FAR, l->dir[j][2] * LIT_FAR);
        GX_InitLightColor(&lo, c);
        GX_InitLightAttn(&lo, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
        GX_LoadLightObj(&lo, (u8)(GX_LIGHT0 << j));
    }
    GXColor amb = {l->amb[0], l->amb[1], l->amb[2], 0};
    GXColor mat = {255, 255, 255, l->alpha};
    GX_SetChanAmbColor(GX_COLOR0A0, amb);
    GX_SetChanMatColor(GX_COLOR0A0, mat);
    lit_ctrl(l->n);
    s_lit_cur = *l;
    s_lit_valid = 1;
}

/* Everything wii_gxscene_draw_lit() does before it sends a vertex, up to and with the GX_Begin; returns the number of vertices the
   primitive was begun with (0: nothing begun).  The caller writes exactly that many vertices of 26 bytes (position, normal, texture
   coordinate) to the write-gather pipe, then calls wii_gxscene_lit_end(). */
int wii_gxscene_lit_begin(int mode, int count, uint32_t gen, const GxLit *lit) {
    if (!s_ready || count < 3 || (unsigned)mode > 2) return 0;
    STAGE(5);
    use_view_mtx();
    fog_sync(s_w_view);
    g_gx_draws++;
    g_gx_lit_draws++;
    if (!s_lit) {
        flush_batch();   /* still in the colour form */
        GX_ClearVtxDesc();
        GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        s_lit = 1;
        g_gx_lit_enters++;
        lit_ctrl(s_lit_valid ? s_lit_cur.n : lit->n);
        s_lit_gen = gen - 1;   /* the chip still holds the lights from before; look at them again */
    }
#ifdef EXP_LITTEST
    if (s_lit_gen != gen) { s_test_lit = *lit; s_test_have = 1; }
#endif
    if (s_lit_gen != gen) lit_load(lit, gen);
    if (mode == 0) count = count / 3 * 3;
    GX_Begin(mode == 0 ? GX_TRIANGLES : mode == 1 ? GX_TRIANGLESTRIP : GX_TRIANGLEFAN, GX_VTXFMT1, (u16)count);
    return count;
}

void wii_gxscene_lit_end(void) {
    GX_End();
    STAGE(0);
}

void wii_gxscene_draw_lit(int mode, const GxVtxL *v, int count, uint32_t gen, const GxLit *lit) {
    count = wii_gxscene_lit_begin(mode, count, gen, lit);
    if (!count) return;
    for (int i = 0; i < count; i++) {
        const GxVtxL *p = v + i;
        GX_Position3f32(p->x, p->y, p->z);
        GX_Normal3s16(p->nx, p->ny, p->nz);
        GX_TexCoord2f32(p->u, p->v);
    }
    wii_gxscene_lit_end();
}

/* A lit draw of vertices that follow one bone at full weight (raster.c, rigid_try): they are sent as the model has
   them and the chip does what the skinning kernel would have done, with the second position matrix slot holding
   bone x world x view (the 4x3 mv, the GE's layout) and the same slot's normal matrix the bone x world rotation
   (nm), so that the normals arrive in the space of the lights, as draw_lit() has them.  The slot is the object-space
   draws' (use_model_mtx()); each loads its own matrix and clears the other's. */
uint32_t g_gx_rigid_draws, g_gx_rigid_loads;
static uint32_t s_rg_ver;   /* the version (raster.c) of the matrices the second slot holds */

void wii_gxscene_draw_rigid(int mode, const GxVtxR *v, int count, uint32_t gen, const GxLit *lit, const float *mv, const float *nm, uint32_t mver) {
    if (!s_ready || count < 3 || (unsigned)mode > 2) return;
    STAGE(5);
    g_gx_draws++;
    g_gx_lit_draws++;
    g_gx_rigid_draws++;
    if (!s_lit) {
        flush_batch();   /* still in the colour form */
        GX_ClearVtxDesc();
        GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        s_lit = 1;
        g_gx_lit_enters++;
        lit_ctrl(s_lit_valid ? s_lit_cur.n : lit->n);
        s_lit_gen = gen - 1;
    }
#ifdef EXP_LITTEST
    if (s_lit_gen != gen) { s_test_lit = *lit; s_test_have = 1; }
#endif
    if (s_lit_gen != gen) lit_load(lit, gen);
    if (!s_rg_valid || s_rg_ver != mver) {
        Mtx m, n;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) m[r][c] = mv[c * 3 + r];
            for (int c = 0; c < 3; c++) n[r][c] = nm[c * 3 + r];
            n[r][3] = 0.0f;
        }
        GX_LoadPosMtxImm(m, GX_PNMTX1);
        GX_LoadNrmMtxImm(n, GX_PNMTX1);
        s_rg_ver = mver;
        s_rg_valid = 1;
        s_model_valid = 0;
        g_gx_rigid_loads++;
    }
    if (s_cur_mtx != 1) {
        GX_SetCurrentMtx(GX_PNMTX1);
        s_cur_mtx = 1;
    }
    fog_sync(mv);
    if (mode == 0) count = count / 3 * 3;
    GX_Begin(mode == 0 ? GX_TRIANGLES : mode == 1 ? GX_TRIANGLESTRIP : GX_TRIANGLEFAN, GX_VTXFMT2, (u16)count);
    for (int i = 0; i < count; i++) {
        const GxVtxR *p = v + i;
        GX_Position3s16(p->x, p->y, p->z);
        GX_Normal3s16(p->nx, p->ny, p->nz);
        GX_TexCoord2f32(p->u, p->v);
    }
    GX_End();
    STAGE(0);
}

#ifdef EXP_LITTEST
/* The chip's lighting against the CPU's, as the display shows it.  64 small squares, each with a normal of its
   own (spread evenly over the sphere), are drawn through wii_gxscene_draw_lit() with the lights of the latest lit
   draw, the colour straight from the channel (no texture, blend or test), and the frame buffer read back at the
   middle of each.  The CPU's colour for the same normal and lights is the flattened lighting's formula in floats.
   The frame buffer holds six bits a channel, so the comparison is on those: a difference of one step is 4
   levels of 255 and is the most rounding can do; two or more is a lighting that is not the same.
   Runs at the start of a display list, before anything is drawn, where the chip state is reset afterwards. */
static void lit_selftest(void) {
    enum { N = 64, COLS = 16, CELL = 8 };
    const GxLit *l = &s_test_lit;
    const float fw = (float)s_mode_obj->fbWidth, fh = (float)s_mode_obj->efbHeight;
    Mtx id;
    guMtxIdentity(id);
    GX_LoadPosMtxImm(id, GX_PNMTX0);
    Mtx44 o;
    guOrtho(o, 0.0f, fh, 0.0f, fw, 0.0f, 1.0f);
    GX_LoadProjectionMtx(o, GX_ORTHOGRAPHIC);
    GX_SetViewport(0.0f, 0.0f, fw, fh, 0.0f, 1.0f);
    GX_SetScissor(0, 0, (u32)fw, (u32)fh);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);
    GX_SetDstAlpha(GX_FALSE, 0);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetDither(GX_FALSE);
    GXColor black = {0, 0, 0, 0};
    GX_SetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, black);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    float nf[N][3];
    for (int i = 0; i < N; i++) {
        const float z = 1.0f - 2.0f * ((float)i + 0.5f) / (float)N, r = sqrtf(1.0f - z * z), phi = (float)i * 2.3999632f;
        nf[i][0] = r * cosf(phi); nf[i][1] = r * sinf(phi); nf[i][2] = z;
        GxVtxL q[4];
        const float x0 = (float)((i % COLS) * CELL), y0 = (float)((i / COLS) * CELL);
        const float xs[4] = {x0, x0 + CELL, x0, x0 + CELL}, ys[4] = {y0, y0, y0 + CELL, y0 + CELL};
        for (int k = 0; k < 4; k++) {
            q[k].x = xs[k]; q[k].y = ys[k]; q[k].z = -0.5f;
            q[k].nx = (int16_t)(nf[i][0] * 16384.0f); q[k].ny = (int16_t)(nf[i][1] * 16384.0f); q[k].nz = (int16_t)(nf[i][2] * 16384.0f);
            q[k].pad = 0; q[k].u = q[k].v = 0.0f;
        }
        wii_gxscene_draw_lit(1, q, 4, 0xFFFF0000u + (uint32_t)s_test_runs, l);
    }
    GX_DrawDone();
    int hist[5] = {0}, other = 0, max8 = 0, shown = 0;
    double sum6 = 0.0;
    for (int i = 0; i < N; i++) {
        float acc[3] = {l->ambf[0], l->ambf[1], l->ambf[2]};
        for (int j = 0; j < l->n; j++) {
            const float d = nf[i][0] * l->dir[j][0] + nf[i][1] * l->dir[j][1] + nf[i][2] * l->dir[j][2];
            if (d > 0.0f)
                for (int c = 0; c < 3; c++) acc[c] += d * l->colf[j][c];
        }
        GXColor px;
        GX_PeekARGB((u16)((i % COLS) * CELL + CELL / 2), (u16)((i / COLS) * CELL + CELL / 2), &px);
        const int got[3] = {px.r, px.g, px.b};
        for (int c = 0; c < 3; c++) {
            const int ref = acc[c] >= 255.0f ? 255 : acc[c] <= 0.0f ? 0 : (int)(acc[c] + 0.5f);
            const int d6 = (got[c] >> 2) - (ref >> 2), d8 = got[c] > ref ? got[c] - ref : ref - got[c];
            if (d6 >= -2 && d6 <= 2) hist[d6 + 2]++; else other++;
            if (d8 > max8) max8 = d8;
            sum6 += d6;
            if ((d6 > 1 || d6 < -1) && shown++ < 3)
                fprintf(stderr, "LITTEST patch %d channel %d: normal (%.3f %.3f %.3f) expected %d got %d\n", i, c, (double)nf[i][0], (double)nf[i][1], (double)nf[i][2], ref, got[c]);
        }
    }
    fprintf(stderr, "LITTEST run %d: %d lights, ambient %u %u %u; 6-bit steps got - expected: -2:%d -1:%d 0:%d +1:%d +2:%d other:%d; most 8-bit difference %d; mean step %+.3f\n",
            s_test_runs, l->n, l->amb[0], l->amb[1], l->amb[2], hist[0], hist[1], hist[2], hist[3], hist[4], other, max8, sum6 / (3.0 * N));
    if (s_lit) lit_leave();
}
#endif
#else
#define LIT_LEAVE() ((void)0)
#endif

/* mode: 0 triangles, 1 triangle strip, 2 triangle fan.  The vertices are
   nine floats each: x y z u v r g b a.  Triangle lists are appended to the
   batch; a run of them that shares the same state ends with one
   GX_Begin/End.

   Strips and fans go to the chip as strips and fans.  Expanding them into
   the triangle batch sent every interior vertex three times (a 20 vertex
   strip became 54), and the GE thread's time is mostly spent producing and
   sending vertices.  A GX_Begin costs three bytes in the FIFO, so giving a
   strip its own one is far cheaper than the extra vertices. */
void wii_gxscene_draw(int mode, const float *v, int count) {
    if (!s_ready || count < 3 || (unsigned)mode > 2) return;
    STAGE(5);
    LIT_LEAVE();
    use_view_mtx();
    fog_sync(s_w_view);
    g_gx_draws++;
    int screen = s_space == 2;
#ifdef TKDR_AUTOPILOT
    if (screen) big_note(v, count);
#endif
    if (mode != 0) {
        flush_batch();
        GX_Begin(mode == 1 ? GX_TRIANGLESTRIP : GX_TRIANGLEFAN, GX_VTXFMT1, (u16)count);
        for (int i = 0; i < count; i++) {
            const float *p = v + i * 9;
            GX_Position3f32(p[0], p[1], screen ? -clamp01(p[2] * (1.0f / 65535.0f)) : p[2]);
            GX_Color4u8(c8(p[5]), c8(p[6]), c8(p[7]), c8(p[8]));
            GX_TexCoord2f32(p[3], p[4]);
        }
        GX_End();
        STAGE(0);
        return;
    }
    int tris = count / 3;
    for (int t = 0; t < tris; t++) {
        const float *a = v + (t * 3) * 9, *b = a + 9, *c = b + 9;
        if (s_batch_n + 3 > BATCH_VERTS) flush_batch();
        batch_vertex(a, screen);
        batch_vertex(b, screen);
        batch_vertex(c, screen);
    }
    STAGE(0);
}

/* The same for vertices that are already in the form the chip is sent (the
   draw cache's).  Cached draws are never in screen space. */
#ifdef EXP_FOGSUB
/* EXP_FOGSUB: what the chip is sent for the fog sheets' textures (the ground-mist cards on stage 34 use the CLUT4 128x32
   at 040D8260 and the 64x32 at 040D7DA0): the scale and offset the draw cache's vertices get (uvt), the registers at that
   moment, and the first vertex's raw coordinates; up to three draws of the first texture and one of the second a frame. */
extern uint32_t g_frames_presented;
void fogsub_regs(float *r);
/* TEXSTATE: for the fog textures and the grass-like 128x128 one, the texels as the chip is given them (an alpha histogram) and
   the state the chip is set up with, once in a while. */
void fogsub_clut(uint8_t *o64, uint32_t *addr, uint32_t *fmtreg);
static void texstate_log(void) {
    static const uint32_t addrs[4] = {0x040D8260u, 0x040D7DA0u, 0x040D6BA0u, 0x040D8B20u};
    static uint32_t last[4];
    for (int k = 0; k < 4; k++) {
        if (s_state.tex_addr != addrs[k]) continue;
        if (last[k] && g_frames_presented - last[k] < (k < 2 ? 240u : 1800u)) return;
        last[k] = g_frames_presented ? g_frames_presented : 1;
        {
            uint8_t cl[64];
            uint32_t ca, cf;
            fogsub_clut(cl, &ca, &cf);
            fprintf(stderr, "CLUTNOW f=%u tex=%08X clutaddr=%08X fmtreg=%X alpha16=", g_frames_presented, s_state.tex_addr, ca, cf);
            for (int i = 0; i < 16; i++) fprintf(stderr, "%02X", cl[i * 4 + 3]);
            fprintf(stderr, " rgb0..3=%02X%02X%02X,%02X%02X%02X,%02X%02X%02X,%02X%02X%02X\n", cl[0], cl[1], cl[2], cl[4], cl[5], cl[6], cl[8], cl[9], cl[10], cl[12], cl[13], cl[14]);
        }
        unsigned hist[8] = {0};
        double sr = 0, sg = 0, sb = 0, sa = 0;
        unsigned nz = 0, total = 0;
        if (s_state.tex_pix) {
            total = (unsigned)(s_state.tex_pw * s_state.tex_ph);
            for (unsigned i = 0; i < total; i++) {
                const uint32_t p = s_state.tex_pix[i];
                const unsigned a = p >> 24;
                hist[a >> 5]++;
                sa += a;
                if (a) { nz++; sr += p & 255; sg += p >> 8 & 255; sb += p >> 16 & 255; }
            }
        }
        fprintf(stderr, "TEXSTATE f=%u tex=%08X pix=%dx%d decl=%dx%d fmt=%d clut=%d/%d/%d/%d native=%d serial=%u rgba=%d func=%d dbl=%d env=%06X clamp=%d/%d linear=%d | "
                "alpha8bins=%u,%u,%u,%u,%u,%u,%u,%u meanA=%.1f nzA=%u meanRGB(a>0)=%.0f/%.0f/%.0f | blend=%d eq=%d src=%d dst=%d fix=%06X/%06X at=%d/f%d/ref%d/mask%X "
                "z=%d/f%d/w%d cull=%d zbias=%d fog=%d through=%d\n",
                g_frames_presented, s_state.tex_addr, s_state.tex_pw, s_state.tex_ph, s_state.tex_w, s_state.tex_h, s_state.tex_fmt, s_state.tex_clut_fmt,
                s_state.tex_clut_shift, s_state.tex_clut_mask, s_state.tex_clut_off, s_state.tex_native, s_state.tex_serial, s_state.tex_rgba, s_state.tex_func,
                s_state.tex_dbl, (unsigned)s_state.tex_env, s_state.tex_clamp_u, s_state.tex_clamp_v, s_state.tex_linear, hist[0], hist[1], hist[2], hist[3],
                hist[4], hist[5], hist[6], hist[7], total ? sa / total : 0.0, nz, nz ? sr / nz : 0.0, nz ? sg / nz : 0.0, nz ? sb / nz : 0.0, s_state.blend,
                s_state.bl_eq, s_state.bl_src, s_state.bl_dst, (unsigned)s_state.fix_a, (unsigned)s_state.fix_b, s_state.alpha_test, s_state.at_func,
                s_state.at_ref, (unsigned)s_state.at_mask, s_state.z_test, s_state.z_func, s_state.z_write, s_state.cull, s_state.zbias, s_state.fog_enable,
                s_state.through);
        return;
    }
}
static void fogsub_log(int mode, const GxVtx *v, int count, const float *uvt) {
    static uint32_t frame;
    static int n_a, n_b;
    if (s_state.tex_on) texstate_log();
    if (!s_state.tex_on || (s_state.tex_addr != 0x040D8260u && s_state.tex_addr != 0x040D7DA0u)) return;
    if (frame != g_frames_presented) { frame = g_frames_presented; n_a = n_b = 0; }
    const int a = s_state.tex_addr == 0x040D8260u;
    if (a ? n_a++ >= 3 : n_b++ >= 1) return;
    float r[5];
    fogsub_regs(r);
    fprintf(stderr, "FOGSUB f=%u %s n=%d mode=%d uvt=%s%g,%g,%g,%g reg=%g,%g,%g,%g mm=%X v0=%g,%g\n", g_frames_presented, a ? "8260" : "7DA0", count, mode,
            uvt ? "" : "NONE:", uvt ? uvt[0] : 0.f, uvt ? uvt[1] : 0.f, uvt ? uvt[2] : 0.f, uvt ? uvt[3] : 0.f, r[0], r[1], r[2], r[3], (unsigned)r[4],
            v[0].u, v[0].v);
}
#endif

#ifdef EXP_NEARLOG
/* EXP_NEARLOG: the texels of the four layers that draw around the fighters on Ominous Mist, as the chip is given them, one row
   a line (RGBA hex, r first), once each; the same rows as an image say what the layer is. */
extern uint32_t g_frames_presented;
static void nearlog_tex(void) {
    static const uint32_t addrs[6] = {0x040D6BA0u, 0x040D8260u, 0x040D8B20u, 0x040D7DA0u, 0x040DB5E0u, 0x040E31E0u};
    static uint8_t done[6];
    if (!s_state.tex_on || !s_state.tex_pix || g_frames_presented < 4200) return;
    for (int k = 0; k < 6; k++) {
        if (s_state.tex_addr != addrs[k] || done[k]) continue;
        done[k] = 1;
        fprintf(stderr, "TEXPIX tex=%08X pix=%dx%d fmt=%d clut=%d\n", s_state.tex_addr, s_state.tex_pw, s_state.tex_ph, s_state.tex_fmt, s_state.tex_clut_fmt);
        for (int y = 0; y < s_state.tex_ph; y++) {
            fprintf(stderr, "TEXROW %08X %d ", s_state.tex_addr, y);
            for (int x = 0; x < s_state.tex_pw; x++) {
                const uint32_t p = s_state.tex_pix[y * s_state.tex_pw + x];
                fprintf(stderr, "%02X%02X%02X%02X", p & 255, p >> 8 & 255, p >> 16 & 255, p >> 24);
            }
            fprintf(stderr, "\n");
        }
    }
}
#endif

void wii_gxscene_draw_packed(int mode, const GxVtx *v, int count, const float *mv, const float *uvt) {
    if (!s_ready || count < 3 || (unsigned)mode > 2) return;
#ifdef EXP_FOGSUB
    fogsub_log(mode, v, count, uvt);
#endif
#ifdef EXP_NEARLOG
    nearlog_tex();
#endif
    STAGE(5);
    LIT_LEAVE();
    if (mv) use_model_mtx(mv);
    else use_view_mtx();
    fog_sync(mv ? mv : s_w_view);
    g_gx_draws++;
    if (mode != 0) {
        flush_batch();
#ifdef EXP_RIBBONDBG
        /* EXP_RIBBONDBG: the pale ground-mist ribbons (the 128x64 cloud texture at 040D6BA0 with fog on) are drawn solid: red with
           the depth test as the game has it, blue (every other 300 frames) with no depth test. */
        extern uint32_t g_frames_presented;
        const int rd = (s_state.tex_on && s_state.tex_addr == 0x040D6BA0u && s_state.fog_enable) ? 1 + (int)((g_frames_presented / 300u) & 1u) : 0;
        const uint32_t rdcol = rd == 1 ? 0xFF0000FFu : 0x0000FFFFu;
#define RDC(c_) (rd ? rdcol : (c_))
        if (rd == 2) GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
#else
#define RDC(c_) (c_)
#endif
        GX_Begin(mode == 1 ? GX_TRIANGLESTRIP : GX_TRIANGLEFAN, GX_VTXFMT1, (u16)count);
        if (uvt) {
            const float su = uvt[0], sv = uvt[1], ou = uvt[2], ov = uvt[3];
            for (int i = 0; i < count; i++) {
                const GxVtx *p = v + i;
                GX_Position3f32(p->x, p->y, p->z);
                GX_Color1u32(RDC(p->rgba));
                GX_TexCoord2f32(p->u * su + ou, p->v * sv + ov);
            }
        } else {
            for (int i = 0; i < count; i++) {
                const GxVtx *p = v + i;
                GX_Position3f32(p->x, p->y, p->z);
                GX_Color1u32(RDC(p->rgba));
                GX_TexCoord2f32(p->u, p->v);
            }
        }
        GX_End();
#ifdef EXP_RIBBONDBG
        if (rd == 2) GX_SetZMode(s_state.z_test ? GX_TRUE : GX_FALSE, k_cmp[s_state.z_func & 7], (s_state.z_test && s_state.z_write) ? GX_TRUE : GX_FALSE);
#endif
#undef RDC
        STAGE(0);
        return;
    }
    int n = count / 3 * 3;
    if (s_batch_n + n > BATCH_VERTS) flush_batch();
    if (uvt) {
        const float su = uvt[0], sv = uvt[1], ou = uvt[2], ov = uvt[3];
        for (int i = 0; i < n; i++) {
            GxVtx *o = s_batch + s_batch_n + i;
            *o = v[i];
            o->u = v[i].u * su + ou;
            o->v = v[i].v * sv + ov;
        }
    } else {
        memcpy(s_batch + s_batch_n, v, (size_t)n * sizeof(GxVtx));
    }
    s_batch_n += n;
    STAGE(0);
}

/* For the heartbeat: how many display lists the chip is behind, and
   whether it is idle. */
void wii_gxscene_chip_status(int *lag, int *cmd_idle, int *read_idle, unsigned *fifo) {
    u8 hi, lo, ri, ci, bp;
    GX_GetGPStatus(&hi, &lo, &ri, &ci, &bp);
    *lag = (int)(s16)((u16)(s_token - 1) - GX_GetDrawSync());
    *cmd_idle = ci;
    *read_idle = ri;
    GXFifoObj gp;
    GX_GetGPFifo(&gp);
    *fifo = GX_GetFifoCount(&gp);
}

/* ------------------------------------------------------ lists, frames */

/* A rectangle in display pixels, black, for the area around the picture. */
static void border_quad(float x0, float y0, float x1, float y1) {
    GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT1, 4);
    GX_Position3f32(x0, y0, 0.0f); GX_Color4u8(0, 0, 0, 255); GX_TexCoord2f32(0.0f, 0.0f);
    GX_Position3f32(x1, y0, 0.0f); GX_Color4u8(0, 0, 0, 255); GX_TexCoord2f32(0.0f, 0.0f);
    GX_Position3f32(x0, y1, 0.0f); GX_Color4u8(0, 0, 0, 255); GX_TexCoord2f32(0.0f, 0.0f);
    GX_Position3f32(x1, y1, 0.0f); GX_Color4u8(0, 0, 0, 255); GX_TexCoord2f32(0.0f, 0.0f);
    GX_End();
}

/* The chip's frame buffer is the size of the display, and the picture
   only covers the letterbox rectangle, so the rest is painted black.
   The depth is left alone. */
static void clear_border(void) {
    float fw = (float)s_mode_obj->fbWidth, fh = (float)s_mode_obj->efbHeight;
    if (s_lb_x0 < 0.5f && s_lb_y0 < 0.5f) return;
    Mtx id;
    guMtxIdentity(id);
    GX_LoadPosMtxImm(id, GX_PNMTX0);
    Mtx44 o;
    guOrtho(o, 0.0f, fh, 0.0f, fw, 0.0f, 1.0f);
    GX_LoadProjectionMtx(o, GX_ORTHOGRAPHIC);
    GX_SetViewport(0.0f, 0.0f, fw, fh, 0.0f, 1.0f);
    GX_SetScissor(0, 0, (u32)fw, (u32)fh);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    if (s_lb_y0 >= 0.5f) {
        border_quad(0.0f, 0.0f, fw, s_lb_y0);
        border_quad(0.0f, s_lb_y1, fw, fh);
    }
    if (s_lb_x0 >= 0.5f) {
        border_quad(0.0f, s_lb_y0, s_lb_x0, s_lb_y1);
        border_quad(s_lb_x1, s_lb_y0, fw, s_lb_y1);
    }
}

/* The start of a display list: the vertex format and the fixed parts of
   the pipeline, and the border. */
void wii_gxscene_list_begin(void) {
    if (!s_ready) return;
    flush_batch();
    STAGE(7);
    g_gx_lists++;
    limbo_collect();

    /* The vertex: a position, a colour and a texture coordinate, in the
       chip's own fixed order. */
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    /* (component type, component size): RGBA, thirty two bits.  The size
       must match GX_Color4u8, which writes four bytes. */
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
#ifdef EXP_GXLIGHT
    /* The normals of lit vertices: three 16-bit numbers with 14 fraction bits, taken as they are (the identity
       normal matrix of the view slot), and the chip is not drawing them yet. */
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_NRM, GX_NRM_XYZ, GX_S16, 14);
    /* Format 2, the rigid vertices (wii_gxscene_draw_rigid): the position is the guest's 16-bit number, 15 fraction
       bits, and the chip does the rest; the normal and the coordinate are as in format 1. */
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_S16, 15);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_NRM, GX_NRM_XYZ, GX_S16, 14);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    s_rg_valid = 0;
    {
        Mtx nid;
        guMtxIdentity(nid);
        GX_LoadNrmMtxImm(nid, GX_PNMTX0);
    }
    s_lit = 0;
    s_lit_valid = 0;
#endif

    /* One channel that passes the vertex colour through.  Lighting must
       be *disabled* for that: enabled, the channel computes the lighting
       equation, which with no lights and a black ambient colour is black. */
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL,
                   GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
#ifdef EXP_TEVFOG
    /* the fog amount: a coordinate made from the position (fog_sync() loads its matrix), looked up in the ramp */
    GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX3x4, GX_TG_POS, GX_TEXMTX1);
    GX_LoadTexObj(&s_fogtex, GX_TEXMAP1);
    s_fogm_valid = 0;
#endif
    GX_SetNumTevStages(1);
    GX_SetTevDirect(GX_TEVSTAGE0);
    GX_SetCurrentMtx(GX_PNMTX0);
    s_cur_mtx = 0;
    s_model_valid = 0;
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetZCompLoc(GX_FALSE);   /* the depth test comes after the texture and the alpha test */
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);
    GX_SetDstAlpha(GX_FALSE, 0);

    clear_border();
#ifdef EXP_LITTEST
    if (s_test_have && s_test_runs < 12 && ++s_test_lists >= 40 && memcmp(&s_test_lit, &s_test_done, sizeof s_test_lit)) {
        s_test_lists = 0;
        s_test_done = s_test_lit;
        lit_selftest();
        s_test_runs++;
    }
#endif

    /* Everything the rasteriser caches about the chip is stale. */
    s_space = 0;
    s_state_valid = 0;
    s_fog.valid = 0;
    s_scissor_valid = 0;
    g_gx_epoch++;
    STAGE(0);
}

/* The end of a display list: copy the picture to a display buffer.  The
   copy does not clear the frame buffer (the guest's framebuffer keeps
   its pixels between frames, and the guest clears it when it wants to).
   A token after it tells the frontend when the copy has landed.

   The guest often ends a list with a clear of the buffer it will draw
   next.  Drawing that clear would wipe the picture before the copy; the
   rasteriser therefore skips a clear once the list has drawn something,
   and this copy sees the finished frame.  A clear at the start of a
   frame (before anything is drawn) still runs. */
static int s_dirty;   /* something has been drawn since the last copy */

#ifdef TKDR_AUTOPILOT
/* Debug: what each copy of the chip's frame buffer to the display holds.  The
   draws since the last copy tell a whole frame from a fragment of one (a list
   that only draws the HUD); the frame buffer is sampled just before the copy. */
/* What the converted textures hold, to tell a texture that is black (or has no alpha) from a draw that does not
   reach the frame buffer: the whole slot table is scanned once per call. */
static void tex_probe(const char *why) {
    unsigned used = 0, native = 0, no_alpha = 0, black = 0, shown = 0;
    uint64_t bytes = 0;
    for (int i = 0; i < TEX_SLOTS; i++) {
        const TexSlot *t = &s_tex[i];
        if (!t->buf) continue;
        used++;
        bytes += t->bytes;
        if (t->native) { native++; continue; }
        uint32_t an = 0, cn = 0, n = (uint32_t)t->tw * (uint32_t)t->th;
        for (size_t o = 0; o + 64 <= (size_t)n * 4; o += 64) {
            const uint8_t *ar = t->buf + o, *gb = ar + 32;
            for (int k = 0; k < 32; k += 2) {
                an += ar[k] != 0;
                cn += (ar[k + 1] | gb[k] | gb[k + 1]) != 0;
            }
        }
        no_alpha += an == 0;
        black += cn == 0;
        if ((an == 0 || cn == 0) && shown++ < 8) {
            extern void gpu_tex_probe(uint32_t serial);
            if (s_zero_n < 4) s_zero_serial[s_zero_n++] = t->serial;
            fprintf(stderr, "  TEXPROBE slot %d serial %u %dx%d texels with alpha %u, with colour %u\n", i, t->serial, t->tw, t->th, an, cn);
            gpu_tex_probe(t->serial);
        }
    }
    fprintf(stderr, "TEXPROBE %s: %u converted textures (%u paletted), %u KB, %u without any alpha, %u all black; pool %u KB used, %d buffers waiting on the chip (%u KB); %u converts, %u binds, gave up: %u without texels, %u without memory\n",
            why, used, native, (unsigned)(bytes >> 10), no_alpha, black, wii_big_used() >> 10, s_nlimbo, (unsigned)(s_limbo_bytes >> 10),
            g_gx_texconv, g_gx_binds, s_tex_nopix, s_tex_nomem);
}
uint32_t g_pr_copies, g_pr_small, g_pr_dark, g_pr_samples;
uint64_t g_pr_luma, g_pr_zsum;
static int s_pr_prev_dark;
static uint32_t s_pr_last_draws;
static void present_probe(void) {
    uint32_t d = g_gx_draws - s_pr_last_draws;
    s_pr_last_draws = g_gx_draws;
    g_pr_copies++;
    if (d < 100) g_pr_small++;
    flush_batch();
    GX_DrawDone();
    uint32_t sum = 0, n = 0, zsum = 0;
    for (int yy = 0; yy < 4; yy++)
        for (int xx = 0; xx < 6; xx++) {
            GXColor c;
            GX_PeekARGB((u16)(100 + xx * 80), (u16)(150 + yy * 60), &c);
            sum += (299u * c.r + 587u * c.g + 114u * c.b) / 1000u;
            n++;
        }
    g_pr_luma += sum;
    g_pr_samples += n;
    g_pr_zsum += zsum;
#ifdef EXP_DARKDBG
    {   /* a run that stays dark: look at the depth buffer once, then 100 frames each with the debug stages on, one more
           after the other (no depth test; + white vertex colours; + no texture), and say how bright each one is: white
           shapes mean the draws reach the frame buffer and something about depth, colour or texture hides them, black
           that they do not (outside the view, culled, clipped) */
        static uint32_t dark_run, stage_frames;
        static const int k_stages[4] = {1, 3, 11, 7};   /* 11: a white texture in place of every texture */
        if (g_dbg_stage) {
            stage_frames++;
            if (stage_frames % 25 == 0) fprintf(stderr, "DARKDBG stage %d: mean luma %u (frame %u, %u draws)\n", g_dbg_stage, sum / n, stage_frames, d);
            if (stage_frames % 100 == 0) {
                g_dbg_stage = stage_frames / 100 < 4 ? k_stages[stage_frames / 100] : 0;
                if (!g_dbg_stage) dark_run = 0;
            }
        } else {
            dark_run = sum / n < 12 && d >= 1000 ? dark_run + 1 : 0;
            if (dark_run == 100) {
                uint32_t zmin = 0xFFFFFFFFu, zmax = 0, zs = 0;
                for (int yy = 0; yy < 4; yy++)
                    for (int xx = 0; xx < 6; xx++) {
                        u32 z = 0;
                        GX_PeekZ((u16)(100 + xx * 80), (u16)(150 + yy * 60), &z);
                        if (z < zmin) zmin = z;
                        if (z > zmax) zmax = z;
                        zs += z >> 8;
                    }
                fprintf(stderr, "DARKDBG depth buffer after a dark frame: min %06X max %06X mean %06X; stages on\n", zmin, zmax, (zs / 24) << 8);
                g_dbg_stage = k_stages[0];
                stage_frames = 0;
            }
        }
    }
#endif
    int dark = sum / n < 6;
    if (dark) g_pr_dark++;
    {   /* a frame buffer that stays dark with a whole frame drawn into it: what is every guest thread waiting for? */
        static uint32_t dark_frames;
        dark_frames = sum / n < 12 && d >= 1000 ? dark_frames + 1 : 0;   /* a bit more than `dark`: the black 3D flickers between 0 and 8 */
        if (dark_frames == 100 || (dark_frames && dark_frames % 600 == 0)) {
            extern void sched_dump_threads(const char *tag);
            fprintf(stderr, "DARKRUN %u dark frames in a row\n", dark_frames);
            sched_dump_threads("dark");
            tex_probe("dark");
        }
    }
    if (dark != s_pr_prev_dark) {   /* the frame buffer just went dark, or came back */
        extern void gpu_dump_regs(const char *why);
        if (d >= 100) {   /* a frame that drew, not a fade or a loading screen */
            extern void gpu_snap_dump(void);
            extern void gpu_clear_dump(const char *);
            fprintf(stderr, "PRESENT frame buffer %s (mean luma %u, %u draws since the last copy)\n", dark ? "went DARK" : "recovered", sum / n, d);
            gpu_clear_dump(dark ? "the frame that went DARK" : "the frame that recovered");
            if (dark) {
                gpu_snap_dump();
                fprintf(stderr, "SNAP   the chip holds: space %d, view %g %g %g | %g %g %g | %g %g %g | %g %g %g, proj %g %g %g %g | %g %g %g %g, viewport %g %g %g %g %g %g (%g %g)\n",
                        s_space, (double)s_w_view[0], (double)s_w_view[1], (double)s_w_view[2], (double)s_w_view[3], (double)s_w_view[4], (double)s_w_view[5],
                        (double)s_w_view[6], (double)s_w_view[7], (double)s_w_view[8], (double)s_w_view[9], (double)s_w_view[10], (double)s_w_view[11],
                        (double)s_w_proj[0], (double)s_w_proj[5], (double)s_w_proj[10], (double)s_w_proj[11], (double)s_w_proj[12], (double)s_w_proj[13],
                        (double)s_w_proj[14], (double)s_w_proj[15], (double)s_w_vp[0], (double)s_w_vp[1], (double)s_w_vp[2], (double)s_w_vp[3],
                        (double)s_w_vp[4], (double)s_w_vp[5], (double)s_w_vp[6], (double)s_w_vp[7]);
            }
        }
        s_pr_prev_dark = dark;
    } else if (g_pr_copies % 120 == 0) {   /* once in a while, to compare the dark state with the bright one */
        extern void gpu_clear_dump(const char *); extern void gpu_snap_dump(void);
        fprintf(stderr, "SNAP of %s\n", dark ? "a dark frame" : "a bright frame");
        gpu_snap_dump();
    }
    if ((dark && g_pr_copies % 10 == 0) || g_pr_copies % 120 == 0)
        big_dump(dark ? "a dark frame" : "a bright frame");
    if (!dark && g_pr_copies % 1200 == 0) tex_probe("bright");
    s_big_n = s_big_over = 0;
    s_big_draws = 0;
    { extern void gpu_snap_reset(void); gpu_snap_reset(); extern void gpu_clear_reset(void); gpu_clear_reset(); }
}
#endif

static void present_now(void) {
    if (!s_ready) return;
    STAGE(6);
    void *xfb = wii_video_xfb_acquire();
    if (!xfb) return;
    g_gx_presents++;
    flush_batch();
#ifdef TKDR_AUTOPILOT
    present_probe();
#endif
    GX_CopyDisp(xfb, GX_FALSE);
    u16 token = s_token++;
    GX_SetDrawSync(token);
    GX_Flush();   /* the commands still sitting in the CPU's write buffer must reach the chip */
    wii_video_xfb_submit(xfb, token);
    STAGE(0);
}

/* A primitive that writes pixels starts a frame's picture. */
void wii_gxscene_dirty(void) { s_dirty = 1; }

/* True once this display list has drawn something.  A clear that arrives
   before any drawing belongs to this frame and is safe to apply; one that
   arrives after drawing is the guest's clear of its other back buffer and
   must not touch the chip's single frame buffer before the display copy. */
int wii_gxscene_frame_started(void) { return s_dirty; }

/* The end of a display list: copy the picture out if anything was drawn. */
void wii_gxscene_present(void) {
    if (!s_ready) return;
    if (s_dirty) {
        present_now();
        s_dirty = 0;
    }
}
