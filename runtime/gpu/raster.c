// Software rasterizer for the PSP GE.
//
// Pipeline: decode vertices -> (skin, morph) -> transform or "through" mode ->
// near-plane clip -> rasterize (triangles, rectangles, lines, points) ->
// texture + texture function -> alpha test -> depth test -> blend -> masks.
// Not yet: lighting, fog, stencil, logic ops, dithering, splines.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "glr.h"
#include "gpu.h"
#include "skin_ps.h"

#define R(reg_) (g_ge.cmd[reg_])

static uint32_t g_prims;
uint32_t g_flush_reason[5];  // stats: list end, texture, transfer, targets full, state wrap
uint64_t hle_time_us(void);

// Diagnostics: where the rasterizer spends its time.
uint64_t g_prim_count, g_px_count, g_setup_us, g_pixel_us;
uint64_t g_sample_us, g_shade_us, g_blend_us;
uint64_t g_emit_us;  // time in emit_triangle outside the pixel loop
uint64_t g_vformat_us, g_beginstate_us;
uint64_t g_gxsubmit_us;  // gxr_prim: chip state and vertices sent to the FIFO
#ifdef EXP_ABRECT
int g_ab_rect = 1;   /* A/B runs (EXP_ABRECT): the chip never culls a rectangle, or culls it like a triangle (the behaviour before), alternating every report */
#define RECT_NOCULL g_ab_rect
#else
#define RECT_NOCULL 1
#endif
#ifdef EXP_ZBIAS
/* EXP_ZBIAS: the chip's depth range is moved toward the eye for the blended draws that do not write depth (GxrState.zbias,
   wii_gxscene.c: EXP_ZBIAS_Q steps of the PSP's 16 bits).  EXP_ABZBIAS alternates it on and off every report. */
#ifdef EXP_ABZBIAS
int g_ab_zbias = 1;
#define ZBIAS_ON g_ab_zbias
#else
#define ZBIAS_ON 1
#endif
#else
#define ZBIAS_ON 0
#endif
#ifdef EXP_ABSTENCIL
/* EXP_ABSTENCIL: the transform-mode draws whose stencil test is NOTEQUAL (the chip has no stencil test, so they are
   drawn without it) are not drawn at all, alternating every report: what the stage looks like without them. */
int g_ab_stencil = 1;
static int s_stencil_drop;
#define STENCIL_DROP_NOW() (s_stencil_drop = g_ab_stencil && (R(GE_STENCILTESTENABLE) & 1) && !(R(GE_VERTEXTYPE) >> 23 & 1) && \
                                              (R(GE_STENCILTEST) & 7) == 3)
#else
#define STENCIL_DROP_NOW() 0
#endif
#ifdef TKDR_AUTOPILOT
uint32_t g_demo_verts, g_demo_draws;
int g_ab_fast = 1;   /* A/B runs: the fast draw path on or off, alternating every report */
#ifdef EXP_COLD
int g_ab_hot = 0;
#else
int g_ab_hot = 1;    /* ... and its state cache (off: the state is worked out for every draw) */
#endif
#ifdef EXP_GXLIGHT
int g_ab_lit = 1;    /* A/B runs: the chip lights the fighters or the CPU does, alternating every report (EXP_ABLIT) */
#endif
int g_ab_skincol = 1;   /* A/B runs (EXP_ABSKINCOL): skinned draws with a vertex colour take the whole-draw decoder or the general path */
int g_ab_colflat = 1;   /* A/B runs (EXP_ABCOLFLAT): the vertex-colour skinned draws are lit by col_lit() or by light_vertex() */
int g_ab_colnonrm = 1;   /* A/B runs (EXP_ABCOLNONRM): of those, the ones without normals take the whole-draw decoder or the general path */
uint32_t g_cat_draws[7], g_cat_verts[7];   /* skinned, through, indexed/morph, long, plain, normals, other */
uint64_t g_dp_ticks[6];
int g_dp_cat;                  /* which kind of draw this is (see gpu_draw_prim) */
static int g_dp_cat_miss;      /* the fast path just had to decode a plain draw */
uint32_t g_world_ident[2];   /* skinned draws with a world matrix that is not / is the identity */
uint32_t g_sk_why[4];   /* skinned draws the whole-draw decoder refused: format, lighting off, light setup */
uint32_t g_sk_vt[4], g_sk_vtn[4], g_sk_vv[4];   /* the skinned draws that take the general path: vertex type (bit 31: lighting on) of the first four seen, draws, vertices */
#ifdef TKDR_FINEPROF
uint32_t g_sk_nz[10], g_sk_nwv[10], g_sk_full, g_sk_draws, g_sk_verts, g_sk_mode[5], g_sk_lmode[4], g_sk_spec, g_sk_tc[4];   /* skinned vertices by nonzero weights and by weight count; draws */
#endif
uint32_t g_other_why[8], g_other_prim[8];   /* why a 3D draw took the general path: idx, morph, flat, prim, skinned, long, normals, other */
uint64_t g_ps_rebuild_ticks, g_ps_rebuild_n, g_vf_rebuild_n;   /* pixel state and vertex format rebuilds */
uint32_t g_run_draws[8], g_run_n[8];   /* runs of consecutive cache-hit draws, by length: 1, 2-3, 4-7, 8-15, 16-31, 32-63, 64+ */
static uint32_t s_runlen;
uint64_t g_fp_ticks[2][4], g_fp_n[2];   /* fast path split: [skinned, cache hit] x [checks, decode or lookup, chip setup, submit] */
uint64_t g_fast_ticks[3], g_fast_n;
uint64_t g_hr_ticks[2], g_hr_n[2];   /* hot_refresh: plain and skinned */
uint32_t g_tgt_refused, g_tgt_nd_draws, g_tgt_fb, g_tgt_nd_fb;   /* draws the chip does not take because the target is not a display buffer */
uint64_t g_tp_ticks[2];   /* the lookup's slow path before the hash: pending-target flush check, surface resolve */
uint64_t g_th_ticks, g_th_n, g_th_bytes, g_tl_ticks, g_tl_n;   /* texture re-hashes in the lookup, and the whole lookup */
uint64_t g_tl_part[4], g_tl_partn[2];   /* lookup parts: clean entry, preamble of a stale one, after the hash, before the hash */
uint64_t g_td_ticks, g_td_n, g_td_texels, g_ts_same;   /* texture decodes (ticks, count, texels), stale lookups whose bytes were the same */
struct { uint32_t addr, fmt, wh, n; } g_td_top[16];     /* which textures were decoded, by address */
uint64_t g_hr_sub[2][3], g_hr_subn[2][3];   /* hot_refresh's parts, [plain, skinned] x [format, pixel state, vertex state] */
uint64_t g_sw_ticks;                 /* hot_switch(): the ticks it took when it applied (count: g_hr_why[9]) */
uint32_t g_hr_why[10];               /* ... by what moved: pix, vertex, light, view, world, vt, epoch, frames, through/format */   /* the fast path: checks and cache lookup, chip state setup, vertex submit */
uint64_t g_cat_ticks[6], g_cat_n[6];   /* fast skinned, fast cache hit, cache miss, skinned (slow), other 3D, 2D/software */   /* time base ticks: setup, cache lookup, decode, GX submit, whole draw, cached draws' submit */
static inline uint32_t dp_tb_real(void) { uint32_t t; __asm__ volatile("mftb %0" : "=r"(t)); return t; }
/* The fine probes (a few time base reads per draw: two or three milliseconds a
   frame together) are only on in TKDR_FINEPROF builds; DC_T, for the whole-draw
   categories, is always on in the autopilot demo. */
#ifdef TKDR_FINEPROF
static inline uint32_t dp_tb(void) { return dp_tb_real(); }
#else
static inline uint32_t dp_tb(void) { return 0; }
#endif
#define DP_T(v_) uint32_t v_ = dp_tb()
#define DC_T(v_) uint32_t v_ = dp_tb_real()
#endif
uint64_t g_tex_decodes, g_tex_decode_us, g_tex_decode_texels;
uint64_t g_clut_hash_us, g_clut_hash_calls;
static uint32_t g_clut_hash;
static int g_clut_dirty = 1;  // set when anything writes the CLUT
/* Counts finished display lists; a texture whose bytes were checked during
   the current list is not hashed again after another TEXFLUSH in it. */
static uint32_t g_tex_list = 1;
uint64_t g_pixstate_us, g_vertex_us;
uint64_t g_fb_fast, g_fb_slow, g_z_fast, g_z_slow;  // which path the pixels took
uint64_t g_persp_tris, g_through_tris;

#ifdef __wii__
/* The GX path: the Wii's own graphics chip draws the guest's triangles.
   The flags are here because the vertex decoder needs to know whether
   the vertices are wanted in object space. */
int g_gxr_enabled;
static int g_gxr_object;        // the vertices are decoded into the GX's world space
static int g_vc_objspace;       // ... or, for the draw cache, left in object space (no world matrix)
static int g_vc_rawuv;          // the draw cache keeps the texture coordinates as the guest gave them
static uint32_t g_draw_pix_gen;  // the pixel-state generation of the primitive being drawn
#ifdef HLE_TRACE
uint32_t g_ps_cache_hits, g_ps_cache_misses, g_light_rebuilds;  // per-draw state reuse
uint32_t g_tex_copy_drops;  // decoded copies given up because the chip held the texture
#endif
uint64_t g_gxr_prims;           // the primitives the chip drew
enum { GXR_REJ_COLOR = 0, GXR_REJ_AT_MASK, GXR_REJ_WRITE_MASK, GXR_REJ_STENCIL,
       GXR_REJ_LOGIC, GXR_REJ_BLEND_EQ, GXR_REJ_BLEND_FACTOR, GXR_REJ_DBL_ALPHA };
#ifdef HLE_TRACE
uint64_t g_vtx_count, g_vtx_skin, g_vtx_lit, g_vtx_morph, g_vtx_col, g_vtx_nrm;
uint64_t g_vtx_cache_hit, g_vtx_cache_miss;
uint32_t g_gxr_sw_prims;        // display-target prims that had to use the CPU
/* GX-mode prims aimed at a non-display target (render to texture): they go to
   the software rasteriser.  The time includes their software raster work. */
uint32_t g_gxr_rtt_prims, g_gxr_rtt_target;
uint64_t g_gxr_rtt_us;
uint32_t g_gxr_skipped;         // GX-mode stencil/mask passes that are not composited
uint32_t g_gxr_rejects[8];      // why the chip could not draw them
static void gxr_reject(int why) { if (why >= 0 && why < 8) g_gxr_rejects[why]++; }
#else
#define gxr_reject(why) ((void)0)
#endif
#endif

/* Half-resolution rendering.
   The scene is drawn into a 240x136 buffer and expanded into the
   guest's framebuffer at the end of each display list.  See
   gpu_half_res_set(). */
#define HALF_W 256
#define HALF_H 136
static uint16_t *g_half_fb, *g_half_z;
static int g_half_res_on;

void gpu_half_res_set(int on) {
    if (on == g_half_res_on) return;
    g_half_res_on = on;
    if (on) {
        if (!g_half_fb) {
            g_half_fb = calloc(HALF_W * HALF_H, sizeof(uint16_t));
            g_half_z = calloc(HALF_W * HALF_H, sizeof(uint16_t));
        }
    } else {
        free(g_half_fb);
        free(g_half_z);
        g_half_fb = g_half_z = NULL;
    }
}

int gpu_half_res_active(void) { return g_half_res_on && g_half_fb != NULL; }
uint64_t g_prim_type[8];          // primitives by type
uint64_t g_feat_texfunc[8];       // texture functions in use
uint64_t g_feat_blend[8];         // blend equations in use
uint64_t g_feat_blsrc[16], g_feat_bldst[16];  // blend factors in use
uint64_t g_feat_atfunc[8];        // alpha test comparisons in use
uint64_t g_feat_zfunc[8];         // depth test comparisons in use
uint64_t g_feat_texfmt[16];       // texture formats in use
uint64_t g_feat_alpha_test, g_feat_z_test, g_feat_z_write, g_feat_blend_on;
uint64_t g_feat_through, g_feat_no_tex, g_feat_rgba, g_feat_dbl;  // counts
uint64_t g_sample_linear, g_sample_point;  // texture samples by kind
uint64_t g_rect_us, g_line_us, g_point_us;  // time in the non-triangle paths
uint64_t g_blend_px, g_tex_px;

uint32_t gpu_stat_prims(void) {
    uint32_t n = g_prims;
    g_prims = 0;
    return n;
}

// ---------------------------------------------------------------- vertices

typedef struct {
    float x, y, z, w;  // clip space (transform) or screen (through)
    float u, v;
    float c[4];  // r g b a, 0..255
    float fog;   // GE fog factor, 0..255 (255 = fully visible)
} Vtx;

static const uint8_t k_tcsize[4] = {0, 2, 4, 8}, k_tcalign[4] = {0, 1, 2, 4};
static const uint8_t k_colsize[8] = {0, 0, 0, 0, 2, 2, 2, 4}, k_colalign[8] = {0, 0, 0, 0, 2, 2, 2, 4};
static const uint8_t k_nrmsize[4] = {0, 3, 6, 12}, k_nrmalign[4] = {0, 1, 2, 4};
static const uint8_t k_possize[4] = {3, 3, 6, 12}, k_posalign[4] = {1, 1, 2, 4};
static const uint8_t k_wtsize[4] = {0, 1, 2, 4}, k_wtalign[4] = {0, 1, 2, 4};

typedef struct {
    uint32_t vt;
    int tc, col, nrm, pos, wt, nw, nmorph, idx, through;
    int off_wt, off_tc, off_col, off_nrm, off_pos, size;
} VFormat;

static uint32_t align_up(uint32_t v, uint32_t a) { return a > 1 ? (v + a - 1) & ~(a - 1) : v; }

static void vformat(VFormat *f, uint32_t vt) {
    f->vt = vt;
    f->tc = vt & 3;
    f->col = (vt >> 2) & 7;
    f->nrm = (vt >> 5) & 3;
    f->pos = (vt >> 7) & 3;
    f->wt = (vt >> 9) & 3;
    f->idx = (vt >> 11) & 3;
    f->nw = ((vt >> 14) & 7) + 1;
    f->nmorph = ((vt >> 18) & 7) + 1;
    f->through = (vt >> 23) & 1;
    uint32_t s = 0, big = 1;
    if (f->wt) {
        f->off_wt = (int)s;
        s += k_wtsize[f->wt] * (uint32_t)f->nw;
        if (k_wtalign[f->wt] > big) big = k_wtalign[f->wt];
    }
    if (f->tc) {
        s = align_up(s, k_tcalign[f->tc]);
        f->off_tc = (int)s;
        s += k_tcsize[f->tc];
        if (k_tcalign[f->tc] > big) big = k_tcalign[f->tc];
    }
    if (f->col) {
        s = align_up(s, k_colalign[f->col]);
        f->off_col = (int)s;
        s += k_colsize[f->col];
        if (k_colalign[f->col] > big) big = k_colalign[f->col];
    }
    if (f->nrm) {
        s = align_up(s, k_nrmalign[f->nrm]);
        f->off_nrm = (int)s;
        s += k_nrmsize[f->nrm];
        if (k_nrmalign[f->nrm] > big) big = k_nrmalign[f->nrm];
    }
    s = align_up(s, k_posalign[f->pos]);
    f->off_pos = (int)s;
    s += k_possize[f->pos];
    if (k_posalign[f->pos] > big) big = k_posalign[f->pos];
    f->size = (int)align_up(s, big);
}

/* Little-endian loads from a host pointer into guest memory (the guest's
   byte order; the Wii is big endian). */
static inline uint16_t ld16le(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
#ifdef __wii__
    v = __builtin_bswap16(v);
#endif
    return v;
}
static inline uint32_t ld32le(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
#ifdef __wii__
    v = __builtin_bswap32(v);
#endif
    return v;
}

/* Vertex components are read through a host pointer to the vertex (see
   decode_vertex): a guarded guest access per component costs a window check,
   a byte swap and a scheduler tick, and a skinned, lit vertex has a dozen
   or more of them. */
static __attribute__((optimize("Ofast", "no-thread-jumps"))) float rd_comp(const uint8_t *p, int kind, int i, int through, int is_signed) {
    switch (kind) {
    case 1: {
        uint8_t b = p[i];
        if (through) return is_signed ? (float)(int8_t)b : (float)b;
        return is_signed ? (float)(int8_t)b / 128.f : (float)b / 128.f;
    }
    case 2: {
        uint16_t h = ld16le(p + i * 2);
        if (through) return is_signed ? (float)(int16_t)h : (float)h;
        return is_signed ? (float)(int16_t)h / 32768.f : (float)h / 32768.f;
    }
    case 3: {
        uint32_t u = ld32le(p + i * 4);
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    }
    return 0.f;
}

static __attribute__((optimize("Ofast", "no-thread-jumps"))) void decode_color(const uint8_t *p, int fmt, float *c) {
    if (fmt == 7) {
        uint32_t v = ld32le(p);
        c[0] = (float)(v & 0xFF); c[1] = (float)(v >> 8 & 0xFF); c[2] = (float)(v >> 16 & 0xFF); c[3] = (float)(v >> 24);
        return;
    }
    uint32_t v = ld16le(p);
    if (fmt == 4) {  // 565
        c[0] = (float)((v & 31) * 255 / 31); c[1] = (float)((v >> 5 & 63) * 255 / 63);
        c[2] = (float)((v >> 11 & 31) * 255 / 31); c[3] = 255.f;
    } else if (fmt == 5) {  // 5551
        c[0] = (float)((v & 31) * 255 / 31); c[1] = (float)((v >> 5 & 31) * 255 / 31);
        c[2] = (float)((v >> 10 & 31) * 255 / 31); c[3] = (v >> 15) ? 255.f : 0.f;
    } else {  // 4444
        c[0] = (float)((v & 15) * 17); c[1] = (float)((v >> 4 & 15) * 17);
        c[2] = (float)((v >> 8 & 15) * 17); c[3] = (float)((v >> 12 & 15) * 17);
    }
}

// 4x3 matrix (column-major, 4 columns of xyz) times point
static __attribute__((optimize("Ofast", "no-thread-jumps"))) void mul43(const float *m, const float *p, float *o) {
    for (int i = 0; i < 3; i++) o[i] = m[i] * p[0] + m[3 + i] * p[1] + m[6 + i] * p[2] + m[9 + i];
}

static void material_color(float *c) {
    uint32_t amb = R(GE_MATERIALAMBIENT);
    c[0] = (float)(amb & 0xFF); c[1] = (float)(amb >> 8 & 0xFF); c[2] = (float)(amb >> 16 & 0xFF);
    c[3] = (float)(R(GE_MATERIALALPHA) & 0xFF);
}

// ---------------------------------------------------------------- lighting

/* 1/sqrt(x), x > 0.  Broadway has no square root instruction, so sqrtf()
   is a software routine; frsqrte's 5-bit estimate and two Newton-Raphson
   steps give about 20 bits, plenty for normals and light distances. */
static inline __attribute__((always_inline)) float fast_rsqrt(float x) {
#ifdef __wii__
    double e;
    __asm__("frsqrte %0,%1" : "=f"(e) : "f"((double)x));
    float y = (float)e;
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    return y;
#else
    return 1.f / sqrtf(x);
#endif
}

static void rgb24(uint32_t v, float *c) {
    c[0] = (float)(v & 0xFF); c[1] = (float)(v >> 8 & 0xFF); c[2] = (float)(v >> 16 & 0xFF);
}

/* Material and light registers change far less often than vertices.  Decode
   them once into this cache; light_vertex() then does only the per-vertex
   arithmetic instead of re-reading/re-expanding dozens of GE registers. */
typedef struct {
    int enabled, kind, comp;
    float L[3], D[3], att[3], cutoff, ex;
    float dinv;    // 1 / |D|, for the spot test
    int iex;       // ex as a whole number for ipow_or_powf(), or 0
    float la[3], ld[3], ls[3];
} GeLight;
typedef struct {
    uint32_t gen;
    int reverse;
    GeLight l[4];
    float ma[4], md[3], ms[3], me[3], ga[4];
    float spec_exp;
    int spec_iexp;     // spec_exp as a whole number for ipow_or_powf(), or 0
    /* With every material colour taken from the registers (MATERIALUPDATE 0,
       or a format without colours, as the fighters' are), each light's colour
       terms are constants: worked out here once for light_registers(). */
    int nlit;          // enabled lights
    struct {
        int idx, spec;          // the light; its specular term is not zero
        float a[3], d[3], s[3]; // ambient, diffuse and specular colour times the material, / 255
    } t[4];
    float base[3], alpha;       // emissive + material ambient * global ambient; the alpha
} LightCache;
static LightCache g_lc;

/* The lighting of the fighters, flattened: one to three directional lights
   and no specular term (everything the fights use).  With the material
   colours from the registers, a vertex's colour is
       base + sum over lights of max(0, n . L) * D
   so the whole thing is three dot products and nine multiply-adds, which
   the decoders do inline.  mode is the number of lights, or 0 when the
   light setup is anything else. */
typedef struct {
    int mode;
    float L[3][3];    /* the directions (negated when the normals are reversed) */
    float D[3][3];    /* diffuse colour times material / 255, per light and channel */
    float base[3];    /* emissive + ambient terms, 0..255 scale */
    float alpha;
    /* The specular term of a directional light: the half vector is constant
       (the viewer is straight down the z axis), so it is a dot product, a
       test and a power (a whole exponent: a few squarings). */
    int spec_mask;    /* bit per light */
    int spec_iexp;    /* the exponent */
    float H[3][3];    /* the normalised half vectors (negated like L) */
    float S[3][3];    /* specular colour times material / 255 */
} LightFlat;
static LightFlat g_lcf;

/* A whole exponent from 1 to 64, for multiplying by squaring; 0 otherwise. */
static int whole_exponent(float e) {
    return e >= 1.f && e <= 64.f && e == (float)(int)e ? (int)e : 0;
}

/* x to the power e.  The fights light with three specular lights and an
   exponent of 8, which made three powf calls (a long libm routine) for every
   lit vertex; a whole exponent is a few multiplications instead. */
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps"))) float ipow_or_powf(float x, float e, int ie) {
    if (!ie) return powf(x, e);
    float r = 1.f;
    for (;;) {
        if (ie & 1) r *= x;
        ie >>= 1;
        if (!ie) return r;
        x *= x;
    }
}

static void build_light_cache(void) {
#ifdef HLE_TRACE
    g_light_rebuilds++;
#endif
    g_lc.gen = g_ge_light_gen;
    g_lc.reverse = (R(GE_REVERSENORMAL) & 1) != 0;
    rgb24(R(GE_MATERIALAMBIENT), g_lc.ma);
    g_lc.ma[3] = (float)(R(GE_MATERIALALPHA) & 0xFF);
    rgb24(R(GE_MATERIALDIFFUSE), g_lc.md);
    rgb24(R(GE_MATERIALSPECULAR), g_lc.ms);
    rgb24(R(GE_MATERIALEMISSIVE), g_lc.me);
    rgb24(R(GE_AMBIENTCOLOR), g_lc.ga);
    g_lc.ga[3] = (float)(R(GE_AMBIENTALPHA) & 0xFF);
    g_lc.spec_exp = ge_float(R(GE_MATERIALSPECULARCOEF));
    g_lc.spec_iexp = whole_exponent(g_lc.spec_exp);
    for (int l = 0; l < 4; l++) {
        typeof(g_lc.l[l]) *d = &g_lc.l[l];
        uint32_t type = R(GE_LIGHTTYPE0 + l);
        d->enabled = (R(GE_LIGHTENABLE0 + l) & 1) != 0;
        d->kind = (int)(type >> 8 & 3);
        d->comp = (int)(type & 3);
        for (int i = 0; i < 3; i++) {
            d->L[i] = ge_float(R(GE_LX0 + 3 * l + i));
            d->D[i] = ge_float(R(GE_LDX0 + 3 * l + i));
            d->att[i] = ge_float(R(GE_LKA0 + 3 * l + i));
        }
        d->cutoff = ge_float(R(GE_LKO0 + l));
        d->ex = ge_float(R(GE_LKS0 + l));
        d->iex = whole_exponent(d->ex);
        /* Per-light constants, worked out once here rather than for every
           vertex: a directional light's vector normalised, and the length of
           a spot light's direction. */
        if (d->kind == 0) {
            float l2 = d->L[0] * d->L[0] + d->L[1] * d->L[1] + d->L[2] * d->L[2];
            if (l2 > 0.f) {
                float inv = 1.f / sqrtf(l2);
                for (int i = 0; i < 3; i++) d->L[i] *= inv;
            }
        }
        float dl2 = d->D[0] * d->D[0] + d->D[1] * d->D[1] + d->D[2] * d->D[2];
        d->dinv = dl2 > 0.f ? 1.f / sqrtf(dl2) : 0.f;
        rgb24(R(GE_LAC0 + 3 * l), d->la);
        rgb24(R(GE_LAC0 + 3 * l + 1), d->ld);
        rgb24(R(GE_LAC0 + 3 * l + 2), d->ls);
    }
    for (int i = 0; i < 3; i++) g_lc.base[i] = g_lc.me[i] + g_lc.ma[i] * g_lc.ga[i] / 255.f;
    g_lc.alpha = g_lc.ma[3] * g_lc.ga[3] / 255.f;
    float ms_sum = g_lc.ms[0] + g_lc.ms[1] + g_lc.ms[2];
    g_lc.nlit = 0;
    for (int l = 0; l < 4; l++) {
        const GeLight *d = &g_lc.l[l];
        if (!d->enabled) continue;
        typeof(g_lc.t[0]) *t = &g_lc.t[g_lc.nlit++];
        t->idx = l;
        t->spec = d->comp == 1 && ms_sum > 0.f && d->ls[0] + d->ls[1] + d->ls[2] > 0.f;
        for (int i = 0; i < 3; i++) {
            t->a[i] = d->la[i] * g_lc.ma[i] / 255.f;
            t->d[i] = d->ld[i] * g_lc.md[i] / 255.f;
            t->s[i] = d->ls[i] * g_lc.ms[i] / 255.f;
        }
    }
    {   /* the flattened form (see LightFlat) */
        int ok = g_lc.nlit >= 1 && g_lc.nlit <= 3;
        int any_spec = 0;
        for (int j = 0; j < g_lc.nlit && ok; j++) {
            const GeLight *d = &g_lc.l[g_lc.t[j].idx];
            ok = d->kind == 0 && d->comp != 2;
            any_spec |= g_lc.t[j].spec;
        }
        /* a specular exponent that is a whole number, or 1, is a few multiplications */
        if (any_spec && !(g_lc.spec_iexp > 0 || g_lc.spec_exp == 1.f)) ok = 0;
        g_lcf.mode = ok ? g_lc.nlit : 0;
        if (ok) {
            for (int i = 0; i < 3; i++) g_lcf.base[i] = g_lc.base[i];
            g_lcf.spec_mask = 0;
            g_lcf.spec_iexp = g_lc.spec_iexp > 0 ? g_lc.spec_iexp : 1;
            for (int j = 0; j < g_lc.nlit; j++) {
                const GeLight *d = &g_lc.l[g_lc.t[j].idx];
                for (int i = 0; i < 3; i++) {
                    g_lcf.L[j][i] = g_lc.reverse ? -d->L[i] : d->L[i];
                    g_lcf.D[j][i] = g_lc.t[j].d[i];
                    g_lcf.base[i] += g_lc.t[j].a[i];
                }
                if (g_lc.t[j].spec) {
                    float h[3] = {d->L[0], d->L[1], d->L[2] + 1.f};
                    float hl2 = h[0] * h[0] + h[1] * h[1] + h[2] * h[2];
                    if (hl2 > 0.f) {
                        float inv = 1.f / sqrtf(hl2);
                        for (int i = 0; i < 3; i++) {
                            g_lcf.H[j][i] = (g_lc.reverse ? -h[i] : h[i]) * inv;
                            g_lcf.S[j][i] = g_lc.t[j].s[i];
                        }
                        g_lcf.spec_mask |= 1 << j;
                    }
                }
            }
            g_lcf.alpha = g_lc.alpha;
        }
    }
#ifdef HLE_TRACE
    {   /* Each distinct light setup once: which light kinds and components
           the game uses decides what the per-vertex lighting has to be fast
           at (the specular and spot exponents are powf calls). */
        static uint32_t seen[24];
        static int nseen;
        uint32_t key = R(GE_MATERIALUPDATE) & 7;
        for (int l = 0; l < 4; l++)
            key |= (uint32_t)(g_lc.l[l].enabled | g_lc.l[l].kind << 1 | g_lc.l[l].comp << 3) << (4 + 5 * l);
        int known = 0;
        for (int i = 0; i < nseen && !known; i++) known = seen[i] == key;
        if (!known && nseen < 24) {
            seen[nseen++] = key;
            fprintf(stderr, "LIGHTCFG key=%06X mu=%u spec_exp=%g", key, R(GE_MATERIALUPDATE) & 7,
                    (double)g_lc.spec_exp);
            for (int l = 0; l < 4; l++)
                if (g_lc.l[l].enabled)
                    fprintf(stderr, " L%d(kind=%d comp=%d ex=%g ls=%g)", l, g_lc.l[l].kind,
                            g_lc.l[l].comp, (double)g_lc.l[l].ex,
                            (double)(g_lc.l[l].ls[0] + g_lc.l[l].ls[1] + g_lc.l[l].ls[2]));
            fprintf(stderr, "\n");
        }
    }
#endif
}

/* The vector to a light (normalised) and the light's attenuation: distance
   and spot for a point or spot light, 1 for a directional one, whose vector
   is normalised in the cache. */
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps")))
float light_dir(const GeLight *d, const float *wp, float *L) {
    L[0] = d->L[0]; L[1] = d->L[1]; L[2] = d->L[2];
    if (d->kind == 0) return 1.f;
    float att = 1.f, spot = 1.f;
    for (int i = 0; i < 3; i++) L[i] -= wp[i];   // from the vertex to the light
    float d2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2];
    float dist = 0.f;
    if (d2 > 0.f) {
        float inv = fast_rsqrt(d2);
        dist = d2 * inv;
        for (int i = 0; i < 3; i++) L[i] *= inv;
    }
    float den = d->att[0] + d->att[1] * dist + d->att[2] * dist * dist;
    att = den > 0.f ? fminf(1.f / den, 1.f) : 0.f;
    if (d->kind == 2) {
        float sd = -(d->D[0] * L[0] + d->D[1] * L[1] + d->D[2] * L[2]) * d->dinv;
        spot = sd >= d->cutoff ? (d->ex == 1.f ? fmaxf(sd, 0.f) : ipow_or_powf(fmaxf(sd, 0.f), d->ex, d->iex)) : 0.f;
    }
    return att * spot;
}

/* The specular factor of a light at a vertex: the GE's half vector uses a
   viewer straight down the z axis. */
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps")))
float light_spec(const float *n, const float *L) {
    float H[3] = {L[0], L[1], L[2] + 1.f};
    float hl2 = H[0] * H[0] + H[1] * H[1] + H[2] * H[2];
    float ndoth = hl2 > 0.f ? (n[0] * H[0] + n[1] * H[1] + n[2] * H[2]) * fast_rsqrt(hl2) : 0.f;
    return ndoth > 0.f ? (g_lc.spec_exp == 1.f ? ndoth : ipow_or_powf(ndoth, g_lc.spec_exp, g_lc.spec_iexp)) : 0.f;
}

static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps")))
float light_diffuse(const GeLight *d, float ndotl) {
    float diff = ndotl > 0.f ? ndotl : 0.f;
    if (d->comp == 2)   // powered diffuse
        diff = g_lc.spec_exp == 1.f ? diff : ipow_or_powf(diff, g_lc.spec_exp, g_lc.spec_iexp);
    return diff;
}

/* Lighting with any material colour taken from the vertex (mu: the
   MATERIALUPDATE bits that apply). */
static __attribute__((optimize("Ofast", "no-thread-jumps"))) void light_general(const float *wp, const float *n, float *col, uint32_t mu) {
    float ma[4], md[3], ms[3];
    if (mu & 1) memcpy(ma, col, sizeof ma);
    else memcpy(ma, g_lc.ma, sizeof ma);
    if (mu & 2) memcpy(md, col, sizeof md); else memcpy(md, g_lc.md, sizeof md);
    if (mu & 4) memcpy(ms, col, sizeof ms); else memcpy(ms, g_lc.ms, sizeof ms);
    const float *me = g_lc.me, *ga = g_lc.ga;
    float out[3], spec[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) out[i] = me[i] + ma[i] * ga[i] / 255.f;
    float alpha = ma[3] * ga[3] / 255.f;
    for (int l = 0; l < 4; l++) {
        const GeLight *d = &g_lc.l[l];
        if (!d->enabled) continue;
        float L[3];
        float k = light_dir(d, wp, L);
        float ndotl = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
        float diff = light_diffuse(d, ndotl);
        for (int i = 0; i < 3; i++) out[i] += k * (d->la[i] * ma[i] + d->ld[i] * md[i] * diff) / 255.f;
        /* Specular is often disabled by a zero material or light colour;
           skip the normalize + pow entirely in that case. */
        if (d->comp == 1 && ndotl > 0.f &&
            (ms[0] + ms[1] + ms[2] > 0.f) && (d->ls[0] + d->ls[1] + d->ls[2] > 0.f)) {
            float sp = light_spec(n, L);
            for (int i = 0; i < 3; i++) spec[i] += k * d->ls[i] * ms[i] * sp / 255.f;
        }
    }
    for (int i = 0; i < 3; i++) {
        float v = out[i] + spec[i];
        col[i] = v > 255.f ? 255.f : v;
    }
    col[3] = alpha > 255.f ? 255.f : alpha;
}

/* The same with every material colour from the registers, using the
   per-light products the cache holds: the fighters, about two thirds of a
   fight's vertices, are lit this way by three lights. */
static __attribute__((optimize("Ofast", "no-thread-jumps"))) void light_registers(const float *wp, const float *n, float *col) {
    float out[3] = {g_lc.base[0], g_lc.base[1], g_lc.base[2]}, spec[3] = {0.f, 0.f, 0.f};
    for (int j = 0; j < g_lc.nlit; j++) {
        const typeof(g_lc.t[0]) *t = &g_lc.t[j];
        const GeLight *d = &g_lc.l[t->idx];
        float L[3];
        float k = light_dir(d, wp, L);
        float ndotl = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
        float diff = light_diffuse(d, ndotl);
        for (int i = 0; i < 3; i++) out[i] += k * (t->a[i] + t->d[i] * diff);
        if (t->spec && ndotl > 0.f) {
            float sp = light_spec(n, L);
            for (int i = 0; i < 3; i++) spec[i] += k * t->s[i] * sp;
        }
    }
    for (int i = 0; i < 3; i++) {
        float v = out[i] + spec[i];
        col[i] = v > 255.f ? 255.f : v;
    }
    col[3] = g_lc.alpha > 255.f ? 255.f : g_lc.alpha;
}

#ifdef HLE_TRACE
/* light_registers() regroups the arithmetic of light_general(), so it is
   checked against it on one call in 64 (within float rounding). */
static uint64_t g_lightfast_checks, g_lightfast_bad;
static void light_check(const float *wp, const float *n, const float *col) {
    float ref[4];
    light_general(wp, n, ref, 0);
    g_lightfast_checks++;
    for (int i = 0; i < 4; i++) {
        float e = col[i] - ref[i];
        if (e < 0.f) e = -e;
        if (e > 0.01f + ref[i] * 1e-4f) {
            if (++g_lightfast_bad <= 8)
                fprintf(stderr, "LIGHTFAST MISMATCH c%d fast=%.6g general=%.6g\n", i, (double)col[i], (double)ref[i]);
            return;
        }
    }
}
#endif

// GE vertex lighting, in world space. col: vertex color (in/out, 0..255).
static __attribute__((optimize("Ofast", "no-thread-jumps"))) void light_vertex(const float *wp, float *n, float *col, int has_color) {
    if (g_lc.gen != g_ge_light_gen) build_light_cache();
    float len2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
    /* Model normals are normally unit length; only pay for the square root
       when skinning/world scaling actually changed the length. */
    if (len2 > 0.f && (len2 < 0.999f || len2 > 1.001f)) {
        float inv = fast_rsqrt(len2);
        for (int i = 0; i < 3; i++) n[i] *= inv;
    }
    if (g_lc.reverse) for (int i = 0; i < 3; i++) n[i] = -n[i];
    uint32_t mu = has_color ? R(GE_MATERIALUPDATE) & 7 : 0;
    if (mu) {
        light_general(wp, n, col, mu);
        return;
    }
    light_registers(wp, n, col);
#ifdef HLE_TRACE
    {
        static uint32_t calls;
        if ((calls++ & 63) == 0) light_check(wp, n, col);
    }
#endif
}

/* The flattened lighting (see LightFlat) with the normal normalised here:
   the skinned normals are only nearly unit length.  The light cache has to
   be current; returns 0 when the light setup is not the flattened kind. */
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps"))) int light_flat(const float *wn, float *col) {
    if (__builtin_expect(g_lc.gen != g_ge_light_gen, 0)) build_light_cache();
    int mode = g_lcf.mode;
    if (!mode) return 0;
    float n0 = wn[0], n1 = wn[1], n2 = wn[2];
    float len2 = n0 * n0 + n1 * n1 + n2 * n2;
    if (len2 > 0.f && (len2 < 0.999f || len2 > 1.001f)) {
        float y = fast_rsqrt(len2);
        n0 *= y; n1 *= y; n2 *= y;
    }
    float d0 = n0 * g_lcf.L[0][0] + n1 * g_lcf.L[0][1] + n2 * g_lcf.L[0][2];
    float o0 = g_lcf.base[0], o1 = g_lcf.base[1], o2 = g_lcf.base[2];
    const int spec = g_lcf.spec_mask;
#define FLAT_SPEC(j_, d_) do {                                                                       \
        float hs_ = n0 * g_lcf.H[j_][0] + n1 * g_lcf.H[j_][1] + n2 * g_lcf.H[j_][2];                 \
        if (hs_ > 0.f) {                                                                             \
            float p_ = 1.f, x_ = hs_;                                                                \
            for (int e_ = g_lcf.spec_iexp;; ) {                                                      \
                if (e_ & 1) p_ *= x_;                                                                \
                e_ >>= 1;                                                                            \
                if (!e_) break;                                                                      \
                x_ *= x_;                                                                            \
            }                                                                                        \
            o0 += p_ * g_lcf.S[j_][0]; o1 += p_ * g_lcf.S[j_][1]; o2 += p_ * g_lcf.S[j_][2];         \
        }                                                                                            \
    } while (0)
    if (d0 > 0.f) {
        o0 += d0 * g_lcf.D[0][0]; o1 += d0 * g_lcf.D[0][1]; o2 += d0 * g_lcf.D[0][2];
        if (spec & 1) FLAT_SPEC(0, d0);
    }
    if (mode > 1) {
        float d1 = n0 * g_lcf.L[1][0] + n1 * g_lcf.L[1][1] + n2 * g_lcf.L[1][2];
        if (d1 > 0.f) {
            o0 += d1 * g_lcf.D[1][0]; o1 += d1 * g_lcf.D[1][1]; o2 += d1 * g_lcf.D[1][2];
            if (spec & 2) FLAT_SPEC(1, d1);
        }
        if (mode > 2) {
            float d2 = n0 * g_lcf.L[2][0] + n1 * g_lcf.L[2][1] + n2 * g_lcf.L[2][2];
            if (d2 > 0.f) {
                o0 += d2 * g_lcf.D[2][0]; o1 += d2 * g_lcf.D[2][1]; o2 += d2 * g_lcf.D[2][2];
                if (spec & 4) FLAT_SPEC(2, d2);
            }
        }
    }
#undef FLAT_SPEC
    col[0] = o0 > 255.f ? 255.f : o0;
    col[1] = o1 > 255.f ? 255.f : o1;
    col[2] = o2 > 255.f ? 255.f : o2;
    col[3] = g_lcf.alpha > 255.f ? 255.f : g_lcf.alpha;
    return 1;
}

/* The lighting of a vertex that has a colour, for the setups the fights use: every enabled light directional and
   none of them a powered-diffuse one.  light_general() with the sums over the lights regrouped:
       out = emissive + ma * (global ambient + sum of light ambient) / 255 + md * (sum of diffuse * max(0, n . L)) / 255
             + (the specular terms, when the material's specular colour comes from the register)
   where ma and md are the material ambient and diffuse, the vertex colour when MATERIALUPDATE says so.  The terms
   are the same, added in another order, so a result can differ from light_general()'s in the last float bit
   (a colour channel by one level at most, as with light_registers()).  Returns 0 for any other setup, and for a
   specular colour taken from the vertex, which is left to light_vertex(). */
typedef struct {
    int nl, spec_mask;
    uint32_t mu;
    float L[4][3], H[4][3], D[4][3], S[4][3];   /* directions, normalised half vectors, diffuse / 255, specular * material / 255 */
    float me[3], A[3], ma[4], md[3], ga3;
} ColLight;
static __attribute__((optimize("Ofast", "no-thread-jumps"))) int col_light_build(ColLight *c, uint32_t mu) {
    if (g_lc.gen != g_ge_light_gen) build_light_cache();
    c->nl = 0; c->spec_mask = 0; c->mu = mu;
    const float ms_sum = g_lc.ms[0] + g_lc.ms[1] + g_lc.ms[2];
    for (int i = 0; i < 3; i++) {
        c->me[i] = g_lc.me[i];
        c->A[i] = g_lc.ga[i] * (1.f / 255.f);
        c->ma[i] = g_lc.ma[i];
        c->md[i] = g_lc.md[i];
    }
    c->ma[3] = g_lc.ma[3];
    c->ga3 = g_lc.ga[3];
    for (int l = 0; l < 4; l++) {
        const GeLight *d = &g_lc.l[l];
        if (!d->enabled) continue;
        if (d->kind != 0 || d->comp == 2) return 0;
        const int j = c->nl++;
        const int sp = d->comp == 1 && d->ls[0] + d->ls[1] + d->ls[2] > 0.f;
        if (sp && (mu & 4)) return 0;
        for (int i = 0; i < 3; i++) {
            c->L[j][i] = d->L[i];
            c->D[j][i] = d->ld[i] * (1.f / 255.f);
            c->A[i] += d->la[i] * (1.f / 255.f);
        }
        if (sp && ms_sum > 0.f) {
            const float h[3] = {d->L[0], d->L[1], d->L[2] + 1.f};
            const float hl2 = h[0] * h[0] + h[1] * h[1] + h[2] * h[2];
            if (hl2 > 0.f) {
                const float r = fast_rsqrt(hl2);
                for (int i = 0; i < 3; i++) {
                    c->H[j][i] = h[i] * r;
                    c->S[j][i] = d->ls[i] * g_lc.ms[i] * (1.f / 255.f);
                }
                c->spec_mask |= 1 << j;
            }
        }
    }
    return 1;
}
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps"))) void col_lit(const ColLight *c, const float *wn, float *col) {
    float n0 = wn[0], n1 = wn[1], n2 = wn[2];
    const float len2 = n0 * n0 + n1 * n1 + n2 * n2;
    if (len2 > 0.f && (len2 < 0.999f || len2 > 1.001f)) {
        const float y = fast_rsqrt(len2);
        n0 *= y; n1 *= y; n2 *= y;
    }
    if (g_lc.reverse) { n0 = -n0; n1 = -n1; n2 = -n2; }
    float e0 = 0.f, e1 = 0.f, e2 = 0.f, s0 = 0.f, s1 = 0.f, s2 = 0.f;
    for (int j = 0; j < c->nl; j++) {
        const float nl = n0 * c->L[j][0] + n1 * c->L[j][1] + n2 * c->L[j][2];
        if (!(nl > 0.f)) continue;
        e0 += nl * c->D[j][0]; e1 += nl * c->D[j][1]; e2 += nl * c->D[j][2];
        if (c->spec_mask >> j & 1) {
            const float nh = n0 * c->H[j][0] + n1 * c->H[j][1] + n2 * c->H[j][2];
            if (nh > 0.f) {
                const float p = g_lc.spec_exp == 1.f ? nh : ipow_or_powf(nh, g_lc.spec_exp, g_lc.spec_iexp);
                s0 += p * c->S[j][0]; s1 += p * c->S[j][1]; s2 += p * c->S[j][2];
            }
        }
    }
    const uint32_t mu = c->mu;
    const float a0 = mu & 1 ? col[0] : c->ma[0], a1 = mu & 1 ? col[1] : c->ma[1], a2 = mu & 1 ? col[2] : c->ma[2];
    const float d0 = mu & 2 ? col[0] : c->md[0], d1 = mu & 2 ? col[1] : c->md[1], d2 = mu & 2 ? col[2] : c->md[2];
    const float a3 = mu & 1 ? col[3] : c->ma[3];
    const float o0 = c->me[0] + a0 * c->A[0] + d0 * e0 + s0, o1 = c->me[1] + a1 * c->A[1] + d1 * e1 + s1,
                o2 = c->me[2] + a2 * c->A[2] + d2 * e2 + s2;
    const float al = a3 * c->ga3 * (1.f / 255.f);
    col[0] = o0 > 255.f ? 255.f : o0;
    col[1] = o1 > 255.f ? 255.f : o1;
    col[2] = o2 > 255.f ? 255.f : o2;
    col[3] = al > 255.f ? 255.f : al;
}

/* Building blocks of the fast decode paths below.

   The vertex formats that make up nearly all of a fight are few and fixed, so
   the fast paths know the component kinds up front.  What they save over the
   general decoder: no switch on the kind for every single component, no morph
   loop that runs once, no eight element weight array, and fewer integer to
   float conversions.  The last one matters on Broadway, which has no direct
   conversion: each one goes through memory (a store pair, a load, a subtract
   and a rounding), so the small integers vertices are made of are looked up in
   tables, and 16-bit values use the cheaper conversion in s16n() and u16n(). */
#define TAB16(e_, b_) e_((b_) + 0), e_((b_) + 1), e_((b_) + 2), e_((b_) + 3), e_((b_) + 4), e_((b_) + 5), \
                      e_((b_) + 6), e_((b_) + 7), e_((b_) + 8), e_((b_) + 9), e_((b_) + 10), e_((b_) + 11), \
                      e_((b_) + 12), e_((b_) + 13), e_((b_) + 14), e_((b_) + 15)
#define TAB256(e_) TAB16(e_, 0), TAB16(e_, 16), TAB16(e_, 32), TAB16(e_, 48), TAB16(e_, 64), TAB16(e_, 80), \
                   TAB16(e_, 96), TAB16(e_, 112), TAB16(e_, 128), TAB16(e_, 144), TAB16(e_, 160), \
                   TAB16(e_, 176), TAB16(e_, 192), TAB16(e_, 208), TAB16(e_, 224), TAB16(e_, 240)
/* Every entry is the very value the general decoder computes at run time.  The
   divisions are by powers of two and so are exact, and the colour widening is
   the same integer arithmetic as decode_color(). */
#define TAB_N_U8(i_) ((float)(i_) / 128.f)
#define TAB_N_S8(i_) ((float)((i_) < 128 ? (i_) : (i_) - 256) / 128.f)
#define TAB_F_U8(i_) ((float)(i_))
#define TAB_C4(i_) ((float)((i_) * 17))
#define TAB_C5(i_) ((float)((i_) * 255 / 31))
#define TAB_C6(i_) ((float)((i_) * 255 / 63))
static const float k_n_u8[256] = {TAB256(TAB_N_U8)};   // unsigned byte / 128 (weights, texture coordinates)
static const float k_n_s8[256] = {TAB256(TAB_N_S8)};   // signed byte / 128 (positions, normals)
static const float k_f_u8[256] = {TAB256(TAB_F_U8)};   // byte as a float (8888 colour, material colour)
static const float k_c4[16] = {TAB16(TAB_C4, 0)};      // a 4-bit channel widened to 8 bits
static const float k_c5[32] = {TAB16(TAB_C5, 0), TAB16(TAB_C5, 16)};
static const float k_c6[64] = {TAB16(TAB_C6, 0), TAB16(TAB_C6, 16), TAB16(TAB_C6, 32), TAB16(TAB_C6, 48)};
static const float k_a1[2] = {0.f, 255.f};             // the 1-bit alpha of 5551

/* (float)(int16_t)n / 32768.f from the raw halfword h, without an integer to
   float conversion.  The float whose bits are 0x43800000 + m is exactly
   256 + m / 32768 for any 23-bit m, so a halfword with its sign bit flipped
   (n + 32768) in the mantissa reads as 257 + n / 32768, and subtracting 257
   leaves n / 32768.  Every step is exact, so the result is the one the
   conversion gives.  The price is one store and one load instead of the store
   pair, load, subtract and rounding of a conversion. */
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps"))) float s16n(uint32_t h) {
    uint32_t bits = 0x43800000u + (h ^ 0x8000u);
    float f;
    memcpy(&f, &bits, 4);
    return f - 257.f;
}
// The unsigned version: (float)h / 32768.f.
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps"))) float u16n(uint32_t h) {
    uint32_t bits = 0x43800000u + h;
    float f;
    memcpy(&f, &bits, 4);
    return f - 256.f;
}
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps"))) float ldf_le(const uint8_t *p) {
    uint32_t u = ld32le(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* A position in transform mode: signed, and scaled to +-1 for the integer
   kinds.  The components come back by value, not through an array: an array
   that is written in several switch arms stays in memory, and the vertex
   would be stored to the stack and loaded back. */
typedef struct { float a, b, c; } Vec3f;
typedef struct { float a, b; } Vec2f;
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps"))) Vec3f rd_pos_fast(const uint8_t *p, int kind) {
    Vec3f r;
    switch (kind) {
    case 1: r.a = k_n_s8[p[0]]; r.b = k_n_s8[p[1]]; r.c = k_n_s8[p[2]]; break;
    case 2: r.a = s16n(ld16le(p)); r.b = s16n(ld16le(p + 2)); r.c = s16n(ld16le(p + 4)); break;
    default: r.a = ldf_le(p); r.b = ldf_le(p + 4); r.c = ldf_le(p + 8); break;
    }
    return r;
}

// A texture coordinate pair in transform mode: unsigned, 1.0 = 128 or 32768 for the integer kinds.
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps"))) Vec2f rd_uv_fast(const uint8_t *p, int kind) {
    Vec2f r;
    switch (kind) {
    case 1: r.a = k_n_u8[p[0]]; r.b = k_n_u8[p[1]]; break;
    case 2: r.a = u16n(ld16le(p)); r.b = u16n(ld16le(p + 2)); break;
    default: r.a = ldf_le(p); r.b = ldf_le(p + 4); break;
    }
    return r;
}

// The same colours as decode_color(), widened through the tables.
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps"))) void rd_color_fast(const uint8_t *p, int fmt, float *c) {
    if (fmt == 7) {  // the bytes in memory are r g b a
        c[0] = k_f_u8[p[0]]; c[1] = k_f_u8[p[1]]; c[2] = k_f_u8[p[2]]; c[3] = k_f_u8[p[3]];
        return;
    }
    uint32_t v = ld16le(p);
    if (fmt == 4) {  // 565
        c[0] = k_c5[v & 31]; c[1] = k_c6[v >> 5 & 63]; c[2] = k_c5[v >> 11 & 31]; c[3] = 255.f;
    } else if (fmt == 5) {  // 5551
        c[0] = k_c5[v & 31]; c[1] = k_c5[v >> 5 & 31]; c[2] = k_c5[v >> 10 & 31]; c[3] = k_a1[v >> 15];
    } else {  // 4444
        c[0] = k_c4[v & 15]; c[1] = k_c4[v >> 4 & 15]; c[2] = k_c4[v >> 8 & 15]; c[3] = k_c4[v >> 12 & 15];
    }
}

// material_color() through the byte table.
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps"))) void material_color_fast(float *c) {
    uint32_t amb = R(GE_MATERIALAMBIENT);
    c[0] = k_f_u8[amb & 0xFF]; c[1] = k_f_u8[amb >> 8 & 0xFF]; c[2] = k_f_u8[amb >> 16 & 0xFF];
    c[3] = k_f_u8[R(GE_MATERIALALPHA) & 0xFF];
}

/* What the per-vertex tail needs from registers that are read through
   ge_float() (a store and a load each): the texture coordinate scale and
   offset, and the fog parameters.  They are registers that bump
   g_ge_vertex_gen when their value changes (vertex_state_op() in ge.c), so
   they are converted again only after that has changed: once per primitive
   at most, rather than once per vertex.  The conversion is out of line so
   that a vertex which finds the values current just loads them as floats. */
typedef struct {
    uint32_t gen;
    float tex_su, tex_sv, tex_ou, tex_ov;
    float fog1, fog2;
} VtxState;
static VtxState g_vs;

static __attribute__((noinline)) void vtx_state_refresh(void) {
    uint32_t gen = g_ge_vertex_gen;
    float su = ge_float(R(GE_TEXSCALEU)), sv = ge_float(R(GE_TEXSCALEV));
    float ou = ge_float(R(GE_TEXOFFSETU)), ov = ge_float(R(GE_TEXOFFSETV));
    /* Texture coordinates from the texture matrix with the UVs as the input
       (texmapmode 0x101): the matrix replaces the offset, and the scale
       multiplies its result (as PPSSPP does).  A matrix that only scales and
       moves (the form a scroll takes: the game's UV animation writes one with
       the rotation slots zero) is the same scale and offset the other mode
       uses, so it goes through that path at no cost per vertex.  A rotating or
       projective one is not done; it keeps the scale and offset as before. */
    if ((R(GE_TEXMAPMODE) & 0x303u) == 0x101u) {
        const float *m = g_ge.tgen;
        if (m[1] == 0.f && m[2] == 0.f && m[3] == 0.f && m[5] == 0.f && m[11] != 0.f) {
            float iq = 1.f / m[11];
            ou = su * m[9] * iq;
            ov = sv * m[10] * iq;
            su = su * m[0] * iq;
            sv = sv * m[4] * iq;
        }
    }
    g_vs.tex_su = su;
    g_vs.tex_sv = sv;
    g_vs.tex_ou = ou;
    g_vs.tex_ov = ov;
    g_vs.fog1 = ge_float(R(GE_FOG1));
    g_vs.fog2 = ge_float(R(GE_FOG2));
    g_vs.gen = gen;
}

/* Called first in a fast path, before anything else is live: the (rare)
   call then costs no registers saved around it. */
static inline __attribute__((always_inline)) const VtxState *vtx_state(void) {
    if (__builtin_expect(g_vs.gen != g_ge_vertex_gen, 0)) vtx_state_refresh();
    return &g_vs;
}

/* The texture coordinate scale and offset as u scale, v scale, u offset,
   v offset, for the vertices the draw cache keeps with raw coordinates. */
static inline const float *vtx_state_uvt(void) { return &vtx_state()->tex_su; }

#ifdef EXP_FOGSUB
/* The texture registers as they are now, for FOGSUB in wii_gxscene_draw_packed(): offset u, v, scale u, v, map mode. */
void fogsub_regs(float *r) {
    r[0] = ge_float(R(GE_TEXOFFSETU)); r[1] = ge_float(R(GE_TEXOFFSETV));
    r[2] = ge_float(R(GE_TEXSCALEU)); r[3] = ge_float(R(GE_TEXSCALEV)); r[4] = (float)R(GE_TEXMAPMODE);
}
/* The first 16 palette entries as the GE holds them now (8888: r g b a bytes), the address they were loaded from, and the
   CLUT format register. */
void fogsub_clut(uint8_t *o64, uint32_t *addr, uint32_t *fmtreg) {
    memcpy(o64, g_ge.clut, 64);
    *addr = (R(GE_CLUTADDR) & 0xFFFFF0u) | ((R(GE_CLUTADDRUPPER) << 8) & 0x0F000000u);
    *fmtreg = R(GE_CLUTFORMAT);
}
#endif

// The GE's fog factor of a view-space depth, as the general decoder works it out.
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps"))) float vtx_fog(float vz, const VtxState *s) {
    float f = (vz + s->fog1) * s->fog2;
    int q = (int)(f * 256.f);
    return (float)(q < 0 ? 0 : q > 255 ? 255 : q);
}

/* The end of every fast path: the world-space position, texture coordinates
   and colour become the output vertex, with the same view/fog/projection and
   texture scale as the general decoder.  Everything is stored last: the output
   may alias the matrices as far as the compiler can tell, so a store in the
   middle would make it reload them. */
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps")))
void vtx_emit(const VtxState *s, const float *wp, float tc0, float tc1, const float *col, Vtx *out) {
    float vp[3] = {0.f, 0.f, 0.f};
    /* The GX path applies view, projection and fog itself, so the vertices it
       is given need no view-space position at all. */
    if (!g_gxr_object) mul43(g_ge.view, wp, vp);
    float fog = (R(GE_FOGENABLE) & 1) && !g_gxr_object ? vtx_fog(vp[2], s) : 255.f;
    float x, y, z, w;
#ifdef __wii__
    if (g_gxr_object) {
        x = wp[0]; y = wp[1]; z = wp[2]; w = 1.f;
    } else
#endif
    {
        const float *p = g_ge.proj;
        x = p[0] * vp[0] + p[4] * vp[1] + p[8] * vp[2] + p[12];
        y = p[1] * vp[0] + p[5] * vp[1] + p[9] * vp[2] + p[13];
        z = p[2] * vp[0] + p[6] * vp[1] + p[10] * vp[2] + p[14];
        w = p[3] * vp[0] + p[7] * vp[1] + p[11] * vp[2] + p[15];
    }
    float u = g_vc_rawuv ? tc0 : tc0 * s->tex_su + s->tex_ou;
    float v = g_vc_rawuv ? tc1 : tc1 * s->tex_sv + s->tex_ov;
    out->x = x; out->y = y; out->z = z; out->w = w;
    out->u = u; out->v = v;
    out->fog = fog;
    out->c[0] = col[0]; out->c[1] = col[1]; out->c[2] = col[2]; out->c[3] = col[3];
}

/* The general decoder: every vertex format, morphing, skinning, lighting.
   decode_vertex() below sends the formats that dominate a fight to the fast
   paths and everything else here.  vb is the vertex's bytes (all morph
   targets) as one host range.  The decoders are kept out of line: inlined
   into one function they made it save and restore two dozen registers on
   every vertex, whichever path the vertex took, and the stage vertices
   need only a handful. */
/* (optimize O1: this function is where GCC 15's try_forward_edges bug keeps
   showing up, whichever way the rest of the file changes.) */
static __attribute__((noinline, optimize("O1", "fast-math", "no-thread-jumps"))) void decode_vertex_generic(const VFormat *f, const uint8_t *vb, Vtx *out) {
    float pos[3] = {0}, tc[2] = {0}, col[4] = {0}, w[8] = {0}, nrm[3] = {0};
    int have_col = f->col != 0;
    /* The common 2D/through vertex: one morph target, no skinning, no
       lighting, and no normals.  It needs a position, optional UV and colour,
       and nothing else; skip the general transform/lighting machinery. */
    if (f->through && f->nmorph == 1 && !f->wt && !f->nrm) {
        const uint8_t *a = vb;
        for (int i = 0; i < 3; i++)
            pos[i] = rd_comp(a + f->off_pos, f->pos, i, 1, f->pos != 1);
        if (f->through && f->pos == 2) pos[2] = (float)(uint16_t)(int16_t)pos[2];
        if (f->tc) for (int i = 0; i < 2; i++) tc[i] = rd_comp(a + f->off_tc, f->tc, i, 1, 0);
        if (have_col) decode_color(a + f->off_col, f->col, col);
        else material_color(col);
        out->x = pos[0]; out->y = pos[1]; out->z = pos[2]; out->w = 1.f;
        out->u = tc[0]; out->v = tc[1];
        out->fog = 255.f;
        memcpy(out->c, col, sizeof col);
        return;
    }
    // morphing: blend every component of the morph targets
    for (int m = 0; m < f->nmorph; m++) {
        const uint8_t *a = vb + m * f->size;
        float k = f->nmorph > 1 ? g_ge.morph[m] : 1.f;
        for (int i = 0; i < 3; i++)
            pos[i] += k * rd_comp(a + f->off_pos, f->pos, i, f->through, f->pos != 1 || !f->through);
        if (f->tc)
            for (int i = 0; i < 2; i++) tc[i] += k * rd_comp(a + f->off_tc, f->tc, i, f->through, 0);
        if (have_col) {
            float c[4];
            decode_color(a + f->off_col, f->col, c);
            for (int i = 0; i < 4; i++) col[i] += k * c[i];
        }
        if (f->wt)
            for (int i = 0; i < f->nw; i++) w[i] += k * rd_comp(a + f->off_wt, f->wt, i, 0, 0);
        /* Normals only matter when lighting is enabled.  Skinning without
           lighting still needs the position, but not three extra fetches. */
        if (f->nrm && (R(GE_LIGHTINGENABLE) & 1))
            for (int i = 0; i < 3; i++) nrm[i] += k * rd_comp(a + f->off_nrm, f->nrm, i, 0, 1);
    }
    if (f->through && f->pos == 2) pos[2] = (float)(uint16_t)(int16_t)pos[2];  // z is unsigned
    if (!have_col) material_color(col);

    out->fog = 255.f;
    if (f->through) {
        out->x = pos[0]; out->y = pos[1]; out->z = pos[2]; out->w = 1.f;
        out->u = tc[0]; out->v = tc[1];
    } else {
        float wp[3], wn[3] = {0, 0, 0}, mn[3] = {nrm[0], nrm[1], nrm[2]};
        int lit = (R(GE_LIGHTINGENABLE) & 1) != 0;
        if (f->wt) {  // skinning: weighted sum of bone transforms
            wp[0] = wp[1] = wp[2] = 0.f;
            float sn[3] = {0, 0, 0};
            for (int i = 0; i < f->nw; i++) {
                /* A vertex format carries up to eight weights, but most
                   vertices use one or two bones; the rest are zero and
                   would add nothing. */
                if (w[i] == 0.f) continue;
                float t[3];
                mul43(g_ge.bone[i], pos, t);
                for (int j = 0; j < 3; j++) wp[j] += w[i] * t[j];
                if (lit) {
                    const float *b = g_ge.bone[i];
                    for (int j = 0; j < 3; j++) sn[j] += w[i] * (b[j] * nrm[0] + b[3 + j] * nrm[1] + b[6 + j] * nrm[2]);
                }
            }
            float t[3] = {wp[0], wp[1], wp[2]};
            mul43(g_ge.world, t, wp);
            memcpy(mn, sn, sizeof mn);
        } else {
            mul43(g_ge.world, pos, wp);
        }
        if (lit) {
            const float *m = g_ge.world;
            for (int j = 0; j < 3; j++) wn[j] = m[j] * mn[0] + m[3 + j] * mn[1] + m[6 + j] * mn[2];
            light_vertex(wp, wn, col, have_col);
        }
        const float *p = g_ge.proj;
        float vp[3] = {0.f, 0.f, 0.f};
        /* The view-space position feeds the projection and the fog.  The GX
           path applies view, projection and fog itself, so it needs none. */
        if (!g_gxr_object) mul43(g_ge.view, wp, vp);
        /* The GE's fog factor is linear in view-space Z and is quantised to
           eight bits per vertex before interpolation.  Compute it here, for
           both renderers: the software path interpolates it, and the GX path
           gets the same values for a fallback primitive. */
        if ((R(GE_FOGENABLE) & 1) && !g_gxr_object) {
            float f = (vp[2] + ge_float(R(GE_FOG1))) * ge_float(R(GE_FOG2));
            int q = (int)(f * 256.f);
            out->fog = (float)(q < 0 ? 0 : q > 255 ? 255 : q);
#ifdef HLE_TRACE
            {
                static int seen;
                if (!seen) {
                    seen = 1;
                    fprintf(stderr, "FOG state z=%f fog1=%f fog2=%f color=%06X q=%d\n",
                            vp[2], ge_float(R(GE_FOG1)), ge_float(R(GE_FOG2)),
                            R(GE_FOGCOLOR) & 0xFFFFFFu, q);
                }
            }
#endif
        }
#ifdef __wii__
        if (g_gxr_object) {
            /* The GX applies the view and projection matrices itself, so it
               gets the world space position and the combined matrix.  Only
               then does its perspective correction divide by the same clip
               w the GE would have used. */
            out->x = wp[0]; out->y = wp[1]; out->z = wp[2]; out->w = 1.f;
        } else
#endif
        {
            out->x = p[0] * vp[0] + p[4] * vp[1] + p[8] * vp[2] + p[12];
            out->y = p[1] * vp[0] + p[5] * vp[1] + p[9] * vp[2] + p[13];
            out->z = p[2] * vp[0] + p[6] * vp[1] + p[10] * vp[2] + p[14];
            out->w = p[3] * vp[0] + p[7] * vp[1] + p[11] * vp[2] + p[15];
        }
        // texture coordinates: scale/offset, or the texture matrix when it is a scale and move (vtx_state_refresh()); others not yet
        if (g_vc_rawuv) {
            out->u = tc[0];
            out->v = tc[1];
        } else {
            const VtxState *vs = vtx_state();
            out->u = tc[0] * vs->tex_su + vs->tex_ou;
            out->v = tc[1] * vs->tex_sv + vs->tex_ov;
        }
    }
    memcpy(out->c, col, sizeof col);
}

/* Fast path 1: transform mode, one morph target, no skinning and no normals.
   This is the stage and environment geometry (tc float or 8-bit, colour
   565/5551/4444/8888, position 8/16-bit or float), the largest share of a
   fight's vertices.  pk, tk and ck are the position, texture coordinate and
   colour kinds: the format's own, or constants when a format has a copy of
   its own.  lit: the fights draw the stage with lighting on, which for a
   format without normals is lighting with a zero normal (the ambient and
   emissive terms), done by the same light_vertex() as the general decoder. */
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps")))
void unlit_body(const VFormat *f, const uint8_t *vb, Vtx *out, int pk, int tk, int ck, int lit) {
    const VtxState *s = vtx_state();
    Vec3f pv = rd_pos_fast(vb + f->off_pos, pk);
    Vec2f tv = {0.f, 0.f};
    if (tk) tv = rd_uv_fast(vb + f->off_tc, tk);
    float pos[3] = {pv.a, pv.b, pv.c}, col[4], wp[3];
    if (ck) rd_color_fast(vb + f->off_col, ck, col);
    else material_color_fast(col);
    if (__builtin_expect(g_vc_objspace, 0)) {
        wp[0] = pos[0]; wp[1] = pos[1]; wp[2] = pos[2];
    } else {
        mul43(g_ge.world, pos, wp);
    }
    if (lit) {
        float wn[3] = {0.f, 0.f, 0.f};
        light_vertex(wp, wn, col, ck != 0);
    }
    vtx_emit(s, wp, tv.a, tv.b, col, out);
}

static __attribute__((noinline, optimize("Ofast", "no-thread-jumps"))) void decode_vertex_unlit(const VFormat *f, const uint8_t *vb, Vtx *out) {
    unlit_body(f, vb, out, f->pos, f->tc, f->col, 0);
}
static __attribute__((noinline, optimize("Ofast", "no-thread-jumps"))) void decode_vertex_lit_nonormal(const VFormat *f, const uint8_t *vb, Vtx *out) {
    unlit_body(f, vb, out, f->pos, f->tc, f->col, 1);
}

/* The vertex type 000113 (16-bit position, float texture coordinates, 565
   colour) is two fifths of a fight's vertices on its own.  With its kinds
   known at compile time there is no switch left to take. */
#define VT_STAGE 0x000113u
static __attribute__((noinline, optimize("Ofast", "no-thread-jumps"))) void decode_vertex_unlit_stage(const VFormat *f, const uint8_t *vb, Vtx *out) {
    unlit_body(f, vb, out, 2, 3, 4, 0);
}
static __attribute__((noinline, optimize("Ofast", "no-thread-jumps"))) void decode_vertex_lit_stage(const VFormat *f, const uint8_t *vb, Vtx *out) {
    unlit_body(f, vb, out, 2, 3, 4, 1);
}

/* One bone's share of a skinned position and normal: the sums the general
   decoder builds, with the same operations in the same order. */
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps")))
void skin_pos(const float *b, float w, const float *pos, float *wp) {
    float t[3];
    mul43(b, pos, t);
    for (int j = 0; j < 3; j++) wp[j] += w * t[j];
}
static inline __attribute__((always_inline, optimize("Ofast", "no-thread-jumps")))
void skin_nrm(const float *b, float w, const float *nrm, float *sn) {
    for (int j = 0; j < 3; j++) sn[j] += w * (b[j] * nrm[0] + b[3 + j] * nrm[1] + b[6 + j] * nrm[2]);
}

/* Fast path 2: transform mode, one morph target, a skinned character: 8-bit
   weights, 8-bit normals and 16-bit positions (the texture coordinates and
   the colour can be anything), with lighting on or off.  The weights are
   bytes, so the zero ones are skipped before any float work, and the normal
   is only fetched when lighting needs it. */
static __attribute__((noinline, optimize("Ofast", "no-thread-jumps"))) void decode_vertex_skinned(const VFormat *f, const uint8_t *vb, Vtx *out) {
    const VtxState *s = vtx_state();
    const uint8_t *pp = vb + f->off_pos, *wb = vb + f->off_wt;
    float pos[3] = {s16n(ld16le(pp)), s16n(ld16le(pp + 2)), s16n(ld16le(pp + 4))};
    Vec2f tv = {0.f, 0.f};
    if (f->tc) tv = rd_uv_fast(vb + f->off_tc, f->tc);
    float col[4], sp[3] = {0.f, 0.f, 0.f}, wp[3];
    int have_col = f->col != 0;
    int nw = f->nw;
    if (R(GE_LIGHTINGENABLE) & 1) {
        const uint8_t *nb = vb + f->off_nrm;
        float nrm[3] = {k_n_s8[nb[0]], k_n_s8[nb[1]], k_n_s8[nb[2]]}, sn[3] = {0.f, 0.f, 0.f}, wn[3];
        for (int i = 0; i < nw; i++) {
            if (wb[i] == 0) continue;  // a zero weight adds nothing
            float w = k_n_u8[wb[i]];
            skin_pos(g_ge.bone[i], w, pos, sp);
            skin_nrm(g_ge.bone[i], w, nrm, sn);
        }
        mul43(g_ge.world, sp, wp);
        const float *m = g_ge.world;
        for (int j = 0; j < 3; j++) wn[j] = m[j] * sn[0] + m[3 + j] * sn[1] + m[6 + j] * sn[2];
        /* Lighting replaces all four channels of the colour, so without a
           vertex colour there is nothing to start it from. */
        if (have_col) rd_color_fast(vb + f->off_col, f->col, col);
        if (have_col || !light_flat(wn, col)) light_vertex(wp, wn, col, have_col);
    } else {
        for (int i = 0; i < nw; i++) {
            if (wb[i] == 0) continue;
            skin_pos(g_ge.bone[i], k_n_u8[wb[i]], pos, sp);
        }
        mul43(g_ge.world, sp, wp);
        if (have_col) rd_color_fast(vb + f->off_col, f->col, col);
        else material_color_fast(col);
    }
    vtx_emit(s, wp, tv.a, tv.b, col, out);
}

#ifdef __wii__
/* What a skinned decode needs that does not depend on the vertices: whether
   the draw is of the kind decoded here at all, its constant colour (lighting
   off, or on with no light) or that the flattened lighting applies, and the
   texture scale and offset.  Worked out once for a run of draws that share
   the register state. */
typedef struct {
    int mode;                  /* 1: one colour for every vertex, 2: flattened lighting, 3: the vertex colour as it is,
                                  4: the vertex colour lit by light_vertex() (formats with a colour; scalar code only) */
    uint32_t rgba_const, alpha;
    float su, sv, ou, ov;
    int ps_ok;                 /* the paired-single kernel can do this run of draws: ps is filled in */
    int ps_kind;               /* which: 0 one colour, 1 lights, 2 lights with a specular term */
#ifdef EXP_GXLIGHT
    int hw;                    /* the chip can do the lighting (draw_prim_fast() only): lit is filled in */
    GxLit lit;
#endif
    SkinPS ps;
} SkinPrep;

/* The vertex formats skin_prepare() takes, and those the paired-single kernel
   takes among them.  A format with a vertex colour (565, 5551, 4444, 8888: the clothes and accessories of some
   fighters, 1-6 ms a frame in the fights that have them when they went through the general path) is decoded by
   the scalar code; the kernel has no colour input. */
static inline int skin_format_ok(const VFormat *f) {
#ifdef EXP_ABSKINCOL
    if (f->col && !g_ab_skincol) return 0;
#endif
#ifdef EXP_ABCOLNONRM
    if (f->col && !f->nrm && !g_ab_colnonrm) return 0;
#endif
    /* (a part with a colour and no normals is lit with a zero normal: the ambient and emissive terms) */
    return f->wt == 1 && (f->nrm == 1 || (f->nrm == 0 && f->col)) && f->pos == 2 && (f->col == 0 || f->col >= 4) && !f->through && f->nmorph == 1;
}
static inline int skin_ps_format_ok(const VFormat *f) {
    return f->col == 0 && (f->tc == 0 || f->tc == 1 || f->tc == 3) && f->nw >= 1 && f->nw <= 8;
}
/* A state prepared for one part of a fighter serves the next part's format when it is the same kind of draw: the
   kernel's or the scalar code's, whose colour handling (none, or per vertex) was fixed in skin_prepare(). */
static inline int skin_fits(const SkinPrep *sp, const VFormat *f) {
    return skin_format_ok(f) && (f->col ? sp->mode >= 3 : sp->ps_ok && skin_ps_format_ok(f));
}

/* The next part of a fighter has its own vertex type (more bones, a different
   stride), and under the same lighting that changes nothing in a prepared
   state but the layout the kernel reads. */
static inline void skin_reformat(SkinPrep *sp, const VFormat *f) {
    SkinPS *k = &sp->ps;
    k->nw = (uint32_t)f->nw; k->size = (uint32_t)f->size;
    k->off_pos = (uint32_t)f->off_pos; k->off_wt = (uint32_t)f->off_wt;
    k->off_nrm = (uint32_t)f->off_nrm; k->off_tc = (uint32_t)f->off_tc;
}

#ifdef EXP_GXLIGHT
static inline uint8_t gx_u8(float v) { return (uint8_t)(v <= 0.f ? 0.f : v >= 255.f ? 255.f : v + 0.5f); }

/* The flattened lighting in the form the chip takes it (see GxLit): the same lights, colours and ambient rounded
   to the chip's 8 bits.  Returns 0, and the chip is not used, when a direction is not a unit vector (the chip
   would take the light's position for the direction of a vector of any length, the flattened lighting does not).
   The light cache has to be current. */
static int gx_lit_build(GxLit *l) {
    memset(l, 0, sizeof *l);
    if (g_lcf.mode < 1 || g_lcf.mode > 3) return 0;
    l->n = g_lcf.mode;
    for (int j = 0; j < l->n; j++) {
        const float *d = g_lcf.L[j];
        const float len2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (!(len2 > 0.98f && len2 < 1.02f)) return 0;
        for (int c = 0; c < 3; c++) {
            l->dir[j][c] = d[c];
            l->col[j][c] = gx_u8(g_lcf.D[j][c]);
#ifdef EXP_LITTEST
            l->colf[j][c] = g_lcf.D[j][c];
#endif
        }
    }
#ifdef EXP_LITTEST
    for (int c = 0; c < 3; c++) l->ambf[c] = g_lcf.base[c];
#endif
    for (int c = 0; c < 3; c++) l->amb[c] = gx_u8(g_lcf.base[c]);
    l->alpha = gx_u8(g_lcf.alpha);
    return 1;
}
#endif

/* Returns 0 when the draw is not that kind. */
static __attribute__((noinline, optimize("Ofast", "no-thread-jumps"))) int skin_prepare(const VFormat *f, SkinPrep *sp) {
#ifdef EXP_NOSKINPK
    return 0;
#endif
    if (!skin_format_ok(f)) {
#ifdef TKDR_AUTOPILOT
        g_sk_why[0]++;
#endif
        return 0;
    }
    if (f->col) {   /* the colour is read from each vertex, and lit (or not) as decode_vertex_skinned() does it */
        const VtxState *s = vtx_state();
        sp->mode = R(GE_LIGHTINGENABLE) & 1 ? 4 : 3;
        sp->rgba_const = 0;
        sp->su = s->tex_su; sp->sv = s->tex_sv; sp->ou = s->tex_ou; sp->ov = s->tex_ov;
        sp->alpha = 0;
        sp->ps_ok = 0;
#ifdef EXP_GXLIGHT
        sp->hw = 0;
#endif
        return 1;
    }
    float probe[4];
    const float zero[3] = {0.f, 0.f, 0.f};
    int flat = light_flat(zero, probe);   /* also brings the light cache up to date */
    /* Lighting off, or on with no light enabled: every vertex has the same
       colour, so no normals are needed at all. */
    sp->mode = 2;
    sp->rgba_const = 0;
    if (!(R(GE_LIGHTINGENABLE) & 1) || (!flat && g_lc.nlit == 0)) {
        float c[4];
        if (R(GE_LIGHTINGENABLE) & 1) {
            for (int k = 0; k < 3; k++) c[k] = g_lc.base[k] > 255.f ? 255.f : g_lc.base[k];
            c[3] = g_lc.alpha > 255.f ? 255.f : g_lc.alpha;
        } else {
            material_color_fast(c);
        }
        sp->rgba_const = (uint32_t)(uint8_t)(c[0] + 0.5f) << 24 | (uint32_t)(uint8_t)(c[1] + 0.5f) << 16 |
                         (uint32_t)(uint8_t)(c[2] + 0.5f) << 8 | (uint32_t)(uint8_t)(c[3] + 0.5f);
        sp->mode = 1;
    } else if (!flat) {
#ifdef TKDR_AUTOPILOT
        g_sk_why[2]++;
        {
            static int shown;
            if (shown < 8 && ++shown) {
                fprintf(stderr, "SKINLIGHT nlit=%d", g_lc.nlit);
                for (int j = 0; j < g_lc.nlit; j++)
                    fprintf(stderr, " L%d(kind=%d comp=%d spec=%d)", g_lc.t[j].idx, g_lc.l[g_lc.t[j].idx].kind,
                            g_lc.l[g_lc.t[j].idx].comp, g_lc.t[j].spec);
                fprintf(stderr, " vt=%06X mu=%u spec_exp=%g\n", f->vt, R(GE_MATERIALUPDATE) & 7, (double)g_lc.spec_exp);
            }
        }
#endif
        return 0;
    }
    const VtxState *s = vtx_state();
    sp->su = s->tex_su; sp->sv = s->tex_sv; sp->ou = s->tex_ou; sp->ov = s->tex_ov;
    sp->alpha = (uint32_t)(uint8_t)(g_lcf.alpha > 255.f ? 255.f : g_lcf.alpha + 0.5f);
    sp->ps_ok = 0;
#ifdef EXP_GXLIGHT
    sp->hw = 0;
#endif
#ifndef EXP_NOSKINPS
    if (skin_ps_format_ok(f)) {
        SkinPS *k = &sp->ps;
        k->bone = &g_ge.bone[0][0];
        k->world = g_ge.world;
        skin_reformat(sp, f);
        k->gqr1 = 0x07040004u;   /* load u8 / 128, store u8 */
        k->gqr6 = 0x0F070E07u;   /* load s16 / 32768, store s16 * 16384 (the chip's normals) */
        k->gqr7 = 0x07060000u;   /* load s8 / 128 */
        k->suv[0] = sp->su; k->suv[1] = sp->sv; k->ouv[0] = sp->ou; k->ouv[1] = sp->ov;
        k->wh[0] = 1.f; k->wh[1] = 0.f;
        k->lo = 0.999f; k->hi = 1.001f;
        k->c01[0] = g_lcf.base[0] + 0.5f; k->c01[1] = g_lcf.base[1] + 0.5f;
        k->c2a[0] = g_lcf.base[2] + 0.5f;
        k->c2a[1] = (g_lcf.alpha > 255.f ? 255.f : g_lcf.alpha) + 0.5f;
        const int lit = sp->mode == 2;
        for (int i = 0; i < 3; i++) {   /* a light that is not there adds nothing: zero direction and colour */
            for (int j = 0; j < 3; j++) {
                const int on = lit && i < g_lcf.mode;
                k->lp[i][j] = on ? g_lcf.L[i][j] : 0.f;
                k->dp[i][j] = on ? g_lcf.D[i][j] : 0.f;
                k->hp[i][j] = on && (g_lcf.spec_mask >> i & 1) ? g_lcf.H[i][j] : 0.f;
                k->sc[i][j] = on && (g_lcf.spec_mask >> i & 1) ? g_lcf.S[i][j] : 0.f;
            }
            k->lp[i][3] = k->dp[i][3] = k->hp[i][3] = k->sc[i][3] = 0.f;
        }
        k->half = 0.5f; k->zero = 0.f; k->three_half = 1.5f; k->pad1 = 0.f;
        k->zeros[0] = k->zeros[1] = 0.f;
        k->spec_mask = lit ? (uint32_t)g_lcf.spec_mask : 0u;
        k->iexp = (uint32_t)(g_lcf.spec_iexp > 0 ? g_lcf.spec_iexp : 1);
        k->rgba_const = sp->rgba_const;
        sp->ps_kind = !lit ? 0 : k->spec_mask ? 2 : 1;
        sp->ps_ok = 1;
#ifdef EXP_GXLIGHT
        sp->hw = lit && !k->spec_mask && gx_lit_build(&sp->lit);
#endif
    }
#endif
    return 1;
}

#ifdef TKDR_FINEPROF
/* What the skinned draws are made of (the kernel and the scalar code both count). */
static void skin_prof(const VFormat *f, const SkinPrep *sp, const uint8_t *vb, int count) {
    const uint8_t *q = vb + f->off_wt;
    g_sk_draws++; g_sk_verts += count; g_sk_mode[sp->mode]++;
    g_sk_lmode[g_lcf.mode & 3]++; g_sk_spec += g_lcf.spec_mask != 0; g_sk_tc[f->tc & 3]++;
    for (int i = 0; i < count; i++, q += f->size) {
        int nz = 0, full = 0;
        for (int k = 0; k < f->nw; k++) { nz += q[k] != 0; full |= q[k] == 128; }
        g_sk_nz[nz]++; g_sk_nwv[f->nw]++;
        g_sk_full += nz == 1 && full;
    }
}
#endif

/* The vertices of a skinned draw, straight into the vertices the chip is sent. */
static __attribute__((noinline, optimize("Ofast", "no-thread-jumps"))) void skin_run_c(const VFormat *f, const SkinPrep *sp, const uint8_t *vb, int count, GxVtx *out) {
    const int nw = f->nw, size = f->size, tcm = f->tc;
    const int off_pos = f->off_pos, off_wt = f->off_wt, off_nrm = f->off_nrm, off_tc = f->off_tc;
    const float su = sp->su, sv = sp->sv, ou = sp->ou, ov = sp->ov;
    const uint32_t alpha = sp->alpha;
    if (sp->mode >= 3) {   /* a vertex colour: decode_vertex_skinned() operation by operation, packed as the chip takes it */
        const int lit = sp->mode == 4, off_col = f->off_col, fcol = f->col, has_nrm = f->nrm != 0;
        ColLight cl;
#ifdef EXP_ABCOLFLAT
        const int flat = lit && g_ab_colflat && col_light_build(&cl, R(GE_MATERIALUPDATE) & 7);
#else
        const int flat = lit && col_light_build(&cl, R(GE_MATERIALUPDATE) & 7);
#endif
        for (int i = 0; i < count; i++, vb += size, out++) {
            const uint8_t *pp = vb + off_pos, *wb = vb + off_wt;
            float pos[3] = {s16n(ld16le(pp)), s16n(ld16le(pp + 2)), s16n(ld16le(pp + 4))};
            float sp_[3] = {0.f, 0.f, 0.f}, wp[3], col[4];
            Vec2f tv = {0.f, 0.f};
            if (tcm) tv = rd_uv_fast(vb + off_tc, tcm);
            if (lit) {
                float wn[3] = {0.f, 0.f, 0.f};
                if (has_nrm) {
                    const uint8_t *nb = vb + off_nrm;
                    float nrm[3] = {k_n_s8[nb[0]], k_n_s8[nb[1]], k_n_s8[nb[2]]}, sn[3] = {0.f, 0.f, 0.f};
                    for (int k = 0; k < nw; k++) {
                        if (wb[k] == 0) continue;
                        float w = k_n_u8[wb[k]];
                        skin_pos(g_ge.bone[k], w, pos, sp_);
                        skin_nrm(g_ge.bone[k], w, nrm, sn);
                    }
                    mul43(g_ge.world, sp_, wp);
                    const float *m = g_ge.world;
                    for (int j = 0; j < 3; j++) wn[j] = m[j] * sn[0] + m[3 + j] * sn[1] + m[6 + j] * sn[2];
                } else {   /* no normals: lit with the zero normal, as decode_vertex_generic() does */
                    for (int k = 0; k < nw; k++) {
                        if (wb[k] == 0) continue;
                        skin_pos(g_ge.bone[k], k_n_u8[wb[k]], pos, sp_);
                    }
                    mul43(g_ge.world, sp_, wp);
                }
                rd_color_fast(vb + off_col, fcol, col);
                if (flat) col_lit(&cl, wn, col);
                else light_vertex(wp, wn, col, 1);
            } else {
                for (int k = 0; k < nw; k++) {
                    if (wb[k] == 0) continue;
                    skin_pos(g_ge.bone[k], k_n_u8[wb[k]], pos, sp_);
                }
                mul43(g_ge.world, sp_, wp);
                rd_color_fast(vb + off_col, fcol, col);
            }
            out->x = wp[0]; out->y = wp[1]; out->z = wp[2];
            out->u = tv.a * su + ou; out->v = tv.b * sv + ov;
            out->rgba = (uint32_t)(uint8_t)(col[0] + 0.5f) << 24 | (uint32_t)(uint8_t)(col[1] + 0.5f) << 16 |
                        (uint32_t)(uint8_t)(col[2] + 0.5f) << 8 | (uint32_t)(uint8_t)(col[3] + 0.5f);
        }
        return;
    }
    if (sp->mode == 1) {
        const uint32_t rgba_const = sp->rgba_const;
        for (int i = 0; i < count; i++, vb += size, out++) {
            const uint8_t *pp = vb + off_pos, *wb = vb + off_wt;
            float pos[3] = {s16n(ld16le(pp)), s16n(ld16le(pp + 2)), s16n(ld16le(pp + 4))};
            float sp_[3] = {0.f, 0.f, 0.f}, wp[3];
            for (int k = 0; k < nw; k++) {
                if (wb[k] == 0) continue;
                skin_pos(g_ge.bone[k], k_n_u8[wb[k]], pos, sp_);
            }
            mul43(g_ge.world, sp_, wp);
            Vec2f tv = {0.f, 0.f};
            if (tcm) tv = rd_uv_fast(vb + off_tc, tcm);
            out->x = wp[0]; out->y = wp[1]; out->z = wp[2];
            out->u = tv.a * su + ou; out->v = tv.b * sv + ov;
            out->rgba = rgba_const;
        }
        return;
    }
    for (int i = 0; i < count; i++, vb += size, out++) {
        const uint8_t *pp = vb + off_pos, *wb = vb + off_wt, *nb = vb + off_nrm;
        float pos[3] = {s16n(ld16le(pp)), s16n(ld16le(pp + 2)), s16n(ld16le(pp + 4))};
        float nrm[3] = {k_n_s8[nb[0]], k_n_s8[nb[1]], k_n_s8[nb[2]]};
        float sp_[3] = {0.f, 0.f, 0.f}, sn[3] = {0.f, 0.f, 0.f}, wp[3], wn[3], col[4] = {0.f, 0.f, 0.f, 0.f};
        for (int k = 0; k < nw; k++) {
            if (wb[k] == 0) continue;
            float w = k_n_u8[wb[k]];
            skin_pos(g_ge.bone[k], w, pos, sp_);
            skin_nrm(g_ge.bone[k], w, nrm, sn);
        }
        mul43(g_ge.world, sp_, wp);
        const float *m = g_ge.world;
        for (int j = 0; j < 3; j++) wn[j] = m[j] * sn[0] + m[3 + j] * sn[1] + m[6 + j] * sn[2];
        light_flat(wn, col);   /* the setup was checked by skin_prepare(): it is the flattened kind */
        Vec2f tv = {0.f, 0.f};
        if (tcm) tv = rd_uv_fast(vb + off_tc, tcm);
        out->x = wp[0]; out->y = wp[1]; out->z = wp[2];
        out->u = tv.a * su + ou; out->v = tv.b * sv + ov;
        uint32_t r = (uint32_t)(uint8_t)(col[0] + 0.5f), g = (uint32_t)(uint8_t)(col[1] + 0.5f), b = (uint32_t)(uint8_t)(col[2] + 0.5f);
        out->rgba = r << 24 | g << 16 | b << 8 | alpha;
    }
}

#ifdef EXP_SKINCHK
/* Debug: the paired-single kernel against the scalar code, vertex by vertex. */
static uint32_t g_skchk_calls, g_skchk_verts, g_skchk_bad;
static float g_skchk_maxp, g_skchk_maxuv, g_skchk_maxabs, g_skchk_maxmag;
static int g_skchk_maxc;
static void skin_check(const VFormat *f, const SkinPrep *sp, const uint8_t *vb, int count, const GxVtx *got) {
    static GxVtx ref[1536];
    if (count > 1536) count = 1536;
    skin_run_c(f, sp, vb, count, ref);
    g_skchk_calls++;
    for (int i = 0; i < count; i++) {
        const float a[3] = {got[i].x, got[i].y, got[i].z}, b[3] = {ref[i].x, ref[i].y, ref[i].z};
        int bad = 0;
        for (int j = 0; j < 3; j++) {
            float m = fabsf(b[j]) > 1.f ? fabsf(b[j]) : 1.f, e = fabsf(a[j] - b[j]) / m;
            if (!(e <= 2e-4f)) bad = 1;
            if (e > g_skchk_maxp) g_skchk_maxp = e;
            if (fabsf(a[j] - b[j]) > g_skchk_maxabs) g_skchk_maxabs = fabsf(a[j] - b[j]);
            if (fabsf(b[j]) > g_skchk_maxmag) g_skchk_maxmag = fabsf(b[j]);
        }
        float eu = fabsf(got[i].u - ref[i].u), ev = fabsf(got[i].v - ref[i].v);
        if (!(eu <= 1e-4f && ev <= 1e-4f)) bad = 1;
        if (eu > g_skchk_maxuv) g_skchk_maxuv = eu;
        if (ev > g_skchk_maxuv) g_skchk_maxuv = ev;
        int dc = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            int d = (int)(got[i].rgba >> sh & 255) - (int)(ref[i].rgba >> sh & 255);
            if (d < 0) d = -d;
            if (d > dc) dc = d;
        }
        if (dc > g_skchk_maxc) g_skchk_maxc = dc;
        if (dc > 1) bad = 1;
        g_skchk_verts++;
        if (bad && ++g_skchk_bad <= 12)
            fprintf(stderr, "SKINCHK MISMATCH v%d/%d nw=%d tc=%d: ps (%g %g %g | %g %g | %08X) c (%g %g %g | %g %g | %08X)\n", i, count, f->nw, f->tc,
                    (double)got[i].x, (double)got[i].y, (double)got[i].z, (double)got[i].u, (double)got[i].v, got[i].rgba,
                    (double)ref[i].x, (double)ref[i].y, (double)ref[i].z, (double)ref[i].u, (double)ref[i].v, ref[i].rgba);
    }
    if ((g_skchk_calls & 4095) == 1)
        fprintf(stderr, "SKINCHK calls %u verts %u bad %u max pos err %g (abs %g of coordinates up to %g) uv err %g colour err %d\n", g_skchk_calls, g_skchk_verts, g_skchk_bad,
                (double)g_skchk_maxp, (double)g_skchk_maxabs, (double)g_skchk_maxmag, (double)g_skchk_maxuv, g_skchk_maxc);
}
#endif

#ifdef EXP_SKINCOLCHK
/* Debug: the vertex-colour skinned vertices of skin_run_c() (modes 3 and 4) against decode_vertex_skinned(), the
   per-vertex path such draws took before: position, texture coordinates and the colour bytes as the chip takes them. */
static uint32_t g_scc_calls, g_scc_verts, g_scc_bad, g_scc_exact, g_scc_mode[5];
static float g_scc_maxp, g_scc_maxuv;
static int g_scc_maxc;
static inline uint32_t scc_c8(float x) { return (uint32_t)(uint8_t)(x <= 0.f ? 0.f : x >= 255.f ? 255.f : x + 0.5f); }
static void skin_col_check(const VFormat *f, const SkinPrep *sp, const uint8_t *vb, int count, const GxVtx *got) {
    g_scc_calls++;
    g_scc_mode[sp->mode]++;
    {
        static uint32_t seen[32];
        static int nseen;
        const uint32_t key = f->vt | (uint32_t)sp->mode << 28;
        int k = 0;
        while (k < nseen && seen[k] != key) k++;
        if (k == nseen && nseen < 32) {
            seen[nseen++] = key;
            fprintf(stderr, "SKINCOLCHK format vt %06X mode %d (col %d nrm %d tc %d nw %d)\n", f->vt, sp->mode, f->col, f->nrm, f->tc, f->nw);
        }
    }
    for (int i = 0; i < count; i++) {
        Vtx r;
        if (f->nrm) decode_vertex_skinned(f, vb + (size_t)i * (size_t)f->size, &r);
        else decode_vertex_generic(f, vb + (size_t)i * (size_t)f->size, &r);
        const float a[3] = {got[i].x, got[i].y, got[i].z}, b[3] = {r.x, r.y, r.z};
        int bad = 0, exact = 1;
        for (int j = 0; j < 3; j++) {
            const float m = fabsf(b[j]) > 1.f ? fabsf(b[j]) : 1.f, e = fabsf(a[j] - b[j]) / m;
            if (!(e <= 2e-4f)) bad = 1;
            if (e > g_scc_maxp) g_scc_maxp = e;
            if (a[j] != b[j]) exact = 0;
        }
        const float eu = fabsf(got[i].u - r.u), ev = fabsf(got[i].v - r.v);
        if (!(eu <= 1e-4f && ev <= 1e-4f)) bad = 1;
        if (eu > g_scc_maxuv) g_scc_maxuv = eu;
        if (ev > g_scc_maxuv) g_scc_maxuv = ev;
        if (eu != 0.f || ev != 0.f) exact = 0;
        const uint32_t want = scc_c8(r.c[0]) << 24 | scc_c8(r.c[1]) << 16 | scc_c8(r.c[2]) << 8 | scc_c8(r.c[3]);
        int dc = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            int d = (int)(got[i].rgba >> sh & 255) - (int)(want >> sh & 255);
            if (d < 0) d = -d;
            if (d > dc) dc = d;
        }
        if (dc > g_scc_maxc) g_scc_maxc = dc;
        if (dc > 1) bad = 1;
        if (dc) exact = 0;
        g_scc_verts++;
        g_scc_exact += exact;
        if (bad && ++g_scc_bad <= 12)
            fprintf(stderr, "SKINCOLCHK MISMATCH v%d/%d mode %d vt %06X: got (%g %g %g | %g %g | %08X) ref (%g %g %g | %g %g | %08X)\n", i, count,
                    sp->mode, f->vt, (double)a[0], (double)a[1], (double)a[2], (double)got[i].u, (double)got[i].v, got[i].rgba,
                    (double)b[0], (double)b[1], (double)b[2], (double)r.u, (double)r.v, want);
    }
    if ((g_scc_calls & 1023) == 1)
        fprintf(stderr, "SKINCOLCHK calls %u (mode3 %u mode4 %u) verts %u bad %u exact %u; max pos err %g uv err %g colour err %d\n", g_scc_calls,
                g_scc_mode[3], g_scc_mode[4], g_scc_verts, g_scc_bad, g_scc_exact, (double)g_scc_maxp, (double)g_scc_maxuv, g_scc_maxc);
}
#endif

#ifdef EXP_GXLIGHT
#ifdef EXP_LITCHK
/* Debug: the vertices the lit kernel makes against the scalar code's.  Positions and texture coordinates must
   agree as they do for the unlit kernel; the colour is what the chip works out from the normal, the lights and
   the ambient colour (its arithmetic: each light's term rounded to a whole number, the sum clamped, then times
   the material colour, which is white) against the colour the CPU computes.  A histogram of the differences
   a channel is off by, and their mean, say how far the two lightings are. */
static uint32_t g_lck_calls, g_lck_verts, g_lck_bad, g_lck_hist[8];
static int64_t g_lck_sum;
static int g_lck_max;
static float g_lck_maxp, g_lck_maxuv, g_lck_maxn;
static void lit_check(const VFormat *f, const SkinPrep *sp, const uint8_t *vb, int count, const GxVtxL *got) {
    static GxVtx ref[1536];
    if (count > 1536) count = 1536;
    skin_run_c(f, sp, vb, count, ref);
    g_lck_calls++;
    const GxLit *l = &sp->lit;
    for (int i = 0; i < count; i++) {
        const float a[3] = {got[i].x, got[i].y, got[i].z}, b[3] = {ref[i].x, ref[i].y, ref[i].z};
        int bad = 0;
        for (int j = 0; j < 3; j++) {
            float m = fabsf(b[j]) > 1.f ? fabsf(b[j]) : 1.f, e = fabsf(a[j] - b[j]) / m;
            if (!(e <= 2e-4f)) bad = 1;
            if (e > g_lck_maxp) g_lck_maxp = e;
        }
        float eu = fabsf(got[i].u - ref[i].u), ev = fabsf(got[i].v - ref[i].v);
        if (!(eu <= 1e-4f && ev <= 1e-4f)) bad = 1;
        if (eu > g_lck_maxuv) g_lck_maxuv = eu;
        if (ev > g_lck_maxuv) g_lck_maxuv = ev;
        const float n[3] = {(float)got[i].nx * (1.f / 16384.f), (float)got[i].ny * (1.f / 16384.f), (float)got[i].nz * (1.f / 16384.f)};
        const float len2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
        if (!(fabsf(len2 - 1.f) < 0.004f)) { bad = 1; if (fabsf(len2 - 1.f) > g_lck_maxn) g_lck_maxn = fabsf(len2 - 1.f); }
        int acc[3] = {l->amb[0], l->amb[1], l->amb[2]};
        for (int j = 0; j < l->n; j++) {
            const float d = n[0] * l->dir[j][0] + n[1] * l->dir[j][1] + n[2] * l->dir[j][2];
            if (d > 0.f)
                for (int c = 0; c < 3; c++) acc[c] += (int)(d * (float)l->col[j][c] + 0.5f);
        }
        for (int c = 0; c < 3; c++) {
            if (acc[c] > 255) acc[c] = 255;
            const int chip = (255 * (acc[c] + (acc[c] >> 7))) >> 8;
            const int cpu = (int)(ref[i].rgba >> (24 - 8 * c) & 255);
            int dd = chip - cpu, ad = dd < 0 ? -dd : dd;
            g_lck_hist[ad > 7 ? 7 : ad]++;
            g_lck_sum += dd;
            if (ad > g_lck_max) g_lck_max = ad;
        }
        g_lck_verts++;
        if (bad && ++g_lck_bad <= 12)
            fprintf(stderr, "LITCHK MISMATCH v%d/%d nw=%d tc=%d: got (%g %g %g | %g %g | n %g %g %g) ref (%g %g %g | %g %g)\n", i, count, f->nw, f->tc,
                    (double)a[0], (double)a[1], (double)a[2], (double)got[i].u, (double)got[i].v, (double)n[0], (double)n[1], (double)n[2],
                    (double)b[0], (double)b[1], (double)b[2], (double)ref[i].u, (double)ref[i].v);
    }
    if ((g_lck_calls & 4095) == 1)
        fprintf(stderr, "LITCHK calls %u verts %u bad %u; pos err %g uv err %g norm err %g; colour channel diffs 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7+:%u, max %d, mean %+.3f\n",
                g_lck_calls, g_lck_verts, g_lck_bad, (double)g_lck_maxp, (double)g_lck_maxuv, (double)g_lck_maxn, g_lck_hist[0], g_lck_hist[1], g_lck_hist[2],
                g_lck_hist[3], g_lck_hist[4], g_lck_hist[5], g_lck_hist[6], g_lck_hist[7], g_lck_max,
                g_lck_verts ? (double)g_lck_sum / (3.0 * (double)g_lck_verts) : 0.0);
}
#endif

/* A skinned draw for the chip to light: the kernel's vertices carry the normal instead of the colour. */
static void skin_run_hw(const VFormat *f, const SkinPrep *sp, const uint8_t *vb, int count, GxVtxL *out) {
#ifdef TKDR_FINEPROF
    skin_prof(f, sp, vb, count);
#endif
    g_skin_ps_fn[3][f->tc == 1 ? 1 : f->tc == 3 ? 2 : 0](&sp->ps, vb, out, count);
#ifdef EXP_LITCHK
    lit_check(f, sp, vb, count, out);
#endif
}
#endif

/* The vertices of a skinned draw: the paired-single kernel when the draw is
   the common kind (see skin_ps.c), the scalar code otherwise. */
static void skin_run(const VFormat *f, const SkinPrep *sp, const uint8_t *vb, int count, GxVtx *out) {
#ifdef TKDR_FINEPROF
    skin_prof(f, sp, vb, count);
#endif
    if (sp->ps_ok) {
        g_skin_ps_fn[sp->ps_kind][f->tc == 1 ? 1 : f->tc == 3 ? 2 : 0](&sp->ps, vb, out, count);
#ifdef EXP_SKINCHK
        skin_check(f, sp, vb, count, out);
#endif
        return;
    }
    skin_run_c(f, sp, vb, count, out);
#ifdef EXP_SKINCOLCHK
    if (sp->mode >= 3) skin_col_check(f, sp, vb, count, out);
#endif
}

/* A skinned draw decoded as a whole straight into the vertices the chip is
   sent.  For the fighters (8-bit weights and normals, 16-bit positions, no
   vertex colour, flattened lighting) this is decode_vertex_skinned() and
   the conversion to the chip's form done in one loop: the invariants are
   read once, there is no call per vertex, and the colour is packed to bytes
   where it is made.  Returns 0 when the draw is not that kind. */
static int decode_skinned_packed(const VFormat *f, const uint8_t *vb, int count, GxVtx *out) {
    SkinPrep sp;
    if (!skin_prepare(f, &sp)) return 0;
    skin_run(f, &sp, vb, count, out);
    return 1;
}
#endif

#ifdef HLE_TRACE
/* Debug check of the fast paths against the general decoder.  One vertex in
   64 of each path is decoded both ways and every output field compared; the
   counters are in the periodic dump (gpu_dump_vformats()). */
static uint64_t g_vtxfast_hits[3], g_vtxfast_checks, g_vtxfast_bad;

/* The same number: equal (+0 and -0 are, as the general decoder's sums start
   from 0), or both NaN.  Compared as integers, since a float comparison would
   not survive the -ffast-math these decoders are built with. */
static int vf_same(float a, float b) {
    uint32_t x, y;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    if (x == y || ((x | y) & 0x7FFFFFFFu) == 0) return 1;
    return (x & 0x7FFFFFFFu) > 0x7F800000u && (y & 0x7FFFFFFFu) > 0x7F800000u;
}

static void vf_fields(const Vtx *v, float *f) {
    f[0] = v->x; f[1] = v->y; f[2] = v->z; f[3] = v->w; f[4] = v->u; f[5] = v->v;
    f[6] = v->c[0]; f[7] = v->c[1]; f[8] = v->c[2]; f[9] = v->c[3]; f[10] = v->fog;
}

static void vtxfast_check(const VFormat *f, const uint8_t *vb, uint32_t base, int path, const Vtx *fast) {
    static const char *const names[11] = {"x", "y", "z", "w", "u", "v", "c0", "c1", "c2", "c3", "fog"};
    Vtx ref;
    float a[11], b[11];
    decode_vertex_generic(f, vb, &ref);
    vf_fields(fast, a);
    vf_fields(&ref, b);
    g_vtxfast_checks++;
    int bad = 0;
    for (int i = 0; i < 11; i++) {
        /* the lit colours are summed in a different order by the flattened lighting */
        float e = a[i] - b[i];
        if (i >= 6 && i <= 9 && e < 0.02f && e > -0.02f) continue;
        if (!vf_same(a[i], b[i])) bad |= 1 << i;
    }
    if (!bad) return;
    g_vtxfast_bad++;
    if (g_vtxfast_bad > 16) return;
    fprintf(stderr, "VTXFAST MISMATCH #%llu path=%s vt=%06X addr=%08X", (unsigned long long)g_vtxfast_bad,
            path == 1 ? "skinned" : path == 2 ? "lit-nonormal" : "unlit", f->vt, base);
    for (int i = 0; i < 11; i++) {
        if (!(bad >> i & 1)) continue;
        uint32_t xa, xb;
        memcpy(&xa, &a[i], 4);
        memcpy(&xb, &b[i], 4);
        fprintf(stderr, " %s fast=%.9g (%08X) generic=%.9g (%08X)", names[i], (double)a[i], xa, (double)b[i], xb);
    }
    fprintf(stderr, "\n");
}
#endif

/* A vertex that is not inside a single mapped window (a bad pointer, or the
   very end of one) is copied out byte by byte first, which reads what the
   guarded accessors would.  This is rare and keeps its buffer out of
   decode_vertex()'s frame; the general decoder gives the same result as the
   fast paths for the bytes. */
static __attribute__((noinline)) void decode_vertex_copied(const VFormat *f, uint32_t base, Vtx *out) {
    uint8_t vcopy[8 * 96];
    uint32_t vbytes = (uint32_t)(f->size * f->nmorph);
    if (vbytes > sizeof vcopy) vbytes = sizeof vcopy;
    for (uint32_t i = 0; i < vbytes; i++) vcopy[i] = mem_r8(base + i);
    decode_vertex_generic(f, vcopy, out);
}

static __attribute__((optimize("Ofast", "no-thread-jumps"))) void decode_vertex(const VFormat *f, uint32_t base, Vtx *out) {
#ifdef HLE_TRACE
    g_vtx_count++;
    if (f->wt) g_vtx_skin++;
    if (f->nmorph > 1) g_vtx_morph++;
    if (f->col) g_vtx_col++;
    if (f->nrm && (R(GE_LIGHTINGENABLE) & 1)) g_vtx_lit++;
    if (f->nrm) g_vtx_nrm++;
#endif
    // The vertex's bytes as one host range.
    const uint8_t *vb = guest_host_ptr(base, (uint32_t)(f->size * f->nmorph));
    if (!vb) {
        decode_vertex_copied(f, base, out);
        return;
    }
    /* The fast paths cover the formats that dominate a fight.  A position of
       kind 0 and the colour kinds 1-3 are not real formats (the general
       decoder reads them as zero, and as a 4444 colour of no size), so they
       stay with it. */
    if (!f->through && f->nmorph == 1 && f->pos && (f->col == 0 || f->col >= 4)) {
        if (!f->wt && !f->nrm) {
            if (!(R(GE_LIGHTINGENABLE) & 1)) {
                if (f->vt == VT_STAGE) decode_vertex_unlit_stage(f, vb, out);
                else decode_vertex_unlit(f, vb, out);
#ifdef HLE_TRACE
                if ((g_vtxfast_hits[0]++ & 63) == 0 && !g_vc_objspace && !g_vc_rawuv) vtxfast_check(f, vb, base, 0, out);
#endif
                return;
            }
            if (f->vt == VT_STAGE) decode_vertex_lit_stage(f, vb, out);
            else decode_vertex_lit_nonormal(f, vb, out);
#ifdef HLE_TRACE
            if ((g_vtxfast_hits[2]++ & 63) == 0 && !g_vc_objspace && !g_vc_rawuv) vtxfast_check(f, vb, base, 2, out);
#endif
            return;
        } else if (f->wt == 1 && f->nrm == 1 && f->pos == 2) {
            decode_vertex_skinned(f, vb, out);
#ifdef HLE_TRACE
            if ((g_vtxfast_hits[1]++ & 63) == 0) vtxfast_check(f, vb, base, 1, out);
#endif
            return;
        }
    }
    decode_vertex_generic(f, vb, out);
}

// ---------------------------------------------------------------- screen mapping

typedef struct {
    float x, y, z;  // screen pixels, depth 0..65535
    float iw;  // 1/w for perspective correction
    float u, v;  // divided by w (transform mode)
    float c[4];  // divided by w
    float fog;   // GE fog factor, interpolated in screen space
} SVtx;

static __attribute__((optimize("Ofast", "no-thread-jumps"))) void to_screen(const Vtx *v, SVtx *s, int through, float scale) {
    if (through) {
        s->x = v->x * scale; s->y = v->y * scale; s->z = v->z; s->iw = 1.f;
        s->u = v->u; s->v = v->v;
        s->fog = v->fog;
        memcpy(s->c, v->c, sizeof s->c);
        return;
    }
    float iw = 1.f / v->w;
    float ox = (float)(R(GE_OFFSETX) & 0xFFFF) / 16.f, oy = (float)(R(GE_OFFSETY) & 0xFFFF) / 16.f;
    s->x = v->x * iw * ge_float(R(GE_VIEWPORTXSCALE)) + ge_float(R(GE_VIEWPORTXCENTER)) - ox;
    s->y = v->y * iw * ge_float(R(GE_VIEWPORTYSCALE)) + ge_float(R(GE_VIEWPORTYCENTER)) - oy;
    s->x *= scale;
    s->y *= scale;
    s->z = v->z * iw * ge_float(R(GE_VIEWPORTZSCALE)) + ge_float(R(GE_VIEWPORTZCENTER));
    s->iw = iw;
    s->u = v->u * iw;
    s->v = v->v * iw;
    s->fog = v->fog;
    for (int i = 0; i < 4; i++) s->c[i] = v->c[i] * iw;
}

// ---------------------------------------------------------------- framebuffer

typedef struct {
    uint32_t fb, fb_stride, fb_fmt, zb, zb_stride;
    int sx1, sy1, sx2, sy2;  // scissor, inclusive (in drawing pixels: scaled with `scale`)
    int scale;               // internal resolution multiplier for this target
    float rscale;            // the screen mapping scale (1, or 0.5 at half resolution)
    int mshift;              // log2(scale) when it is a power of two, else -1
    uint32_t *scol;          // scaled color surface (decoded RGBA, quantized to fb_fmt), or NULL
    uint16_t *sz;            // scaled depth surface, or NULL
    uint8_t *cmark, *zmark;  // drawn-pixel marks (guest resolution) of those surfaces
    int sw;                  // row length of scol/sz in pixels
    uint8_t *fbuf;           // raw host pointer to the guest framebuffer, or NULL
    uint8_t *zbuf;           // raw host pointer to the guest depth buffer, or NULL
    uint32_t fbuf_avail;     // bytes available in the framebuffer's window
    uint32_t zbuf_avail;     // bytes available in the depth buffer's window
} Target;

static void target(Target *t) {
    t->fb = 0x04000000u | (R(GE_FRAMEBUFPTR) & 0xFFFFF0u);
    t->fb_stride = R(GE_FRAMEBUFWIDTH) & 0x7FC;
    t->fb_fmt = R(GE_FRAMEBUFPIXFORMAT) & 3;
    t->zb = 0x04000000u | (R(GE_ZBUFPTR) & 0xFFFFF0u);
    t->zb_stride = R(GE_ZBUFWIDTH) & 0x7FC;
    t->sx1 = (int)(R(GE_SCISSOR1) & 0x3FF);
    t->sy1 = (int)(R(GE_SCISSOR1) >> 10 & 0x3FF);
    t->sx2 = (int)(R(GE_SCISSOR2) & 0x3FF);
    t->sy2 = (int)(R(GE_SCISSOR2) >> 10 & 0x3FF);
    t->scale = 1;
    t->rscale = 1.f;
    t->mshift = 0;
    t->scol = NULL;
    t->sz = NULL;
    t->cmark = t->zmark = NULL;
    t->sw = 0;
    /* Raw host pointers into the guest's VRAM, so the pixel loop does not
       go through mem_r32()/mem_w32() (a byte swap and a scheduler tick per
       pixel).  The framebuffer lives in the 2 MiB VRAM window, so the
       available space is what is left of that window. */
    uint32_t avail;
    t->fbuf = guest_window(t->fb, &avail);
    t->fbuf_avail = t->fbuf ? avail : 0;
    t->zbuf = guest_window(t->zb, &avail);
    t->zbuf_avail = t->zbuf ? avail : 0;
    if (gpu_half_res_active()) {
        /* Draw into the small buffer instead of the guest's framebuffer.
           The vertices are halved by the screen mapping, and the scissor
           with them, so the whole picture lands in the small buffer. */
        t->fb_stride = HALF_W;
        t->fbuf = (uint8_t *)g_half_fb;
        t->fbuf_avail = HALF_W * HALF_H * 2;
        t->zb_stride = HALF_W;
        t->zbuf = (uint8_t *)g_half_z;
        t->zbuf_avail = HALF_W * HALF_H * 2;
        t->sx1 /= 2; t->sx2 /= 2;
        t->sy1 /= 2; t->sy2 /= 2;
        t->rscale = 0.5f;
    }
#ifdef HLE_TRACE
    {   // one-shot report of the target geometry
        static uint32_t last_fb;
        if (t->fb != last_fb) {
            last_fb = t->fb;
            fprintf(stderr, "target: fb=%08X stride=%u fmt=%u scissor=%d..%d,%d..%d "
                    "fbuf=%p avail=%u zbuf=%p zavail=%u\n",
                    t->fb, t->fb_stride, t->fb_fmt, t->sx1, t->sx2, t->sy1, t->sy2,
                    (void *)t->fbuf, t->fbuf_avail, (void *)t->zbuf, t->zbuf_avail);
        }
    }
#endif
}

static uint32_t fb_decode16(uint32_t v, uint32_t fmt) {
    uint32_t r, g, b, al;
    switch (fmt) {
    case 0: r = (v & 31) << 3 | (v & 31) >> 2; g = (v >> 5 & 63) << 2 | (v >> 5 & 63) >> 4;
            b = (v >> 11 & 31) << 3 | (v >> 11 & 31) >> 2; al = 0; break;
    case 1: r = (v & 31) << 3 | (v & 31) >> 2; g = (v >> 5 & 31) << 3 | (v >> 5 & 31) >> 2;
            b = (v >> 10 & 31) << 3 | (v >> 10 & 31) >> 2; al = (v >> 15) ? 255 : 0; break;
    default: r = (v & 15) * 17; g = (v >> 4 & 15) * 17; b = (v >> 8 & 15) * 17; al = (v >> 12) * 17; break;
    }
    return r | g << 8 | b << 16 | al << 24;
}

static uint32_t fb_encode16(uint32_t c, uint32_t fmt) {
    uint32_t r = c & 0xFF, g = c >> 8 & 0xFF, b = c >> 16 & 0xFF, al = c >> 24;
    switch (fmt) {
    case 0: return r >> 3 | (g >> 2) << 5 | (b >> 3) << 11;
    case 1: return r >> 3 | (g >> 3) << 5 | (b >> 3) << 10 | (al >= 0x80 ? 0x8000u : 0u);
    default: return r >> 4 | (g >> 4) << 4 | (b >> 4) << 8 | (al >> 4) << 12;
    }
}

// fb_decode16(fb_encode16(c)), with masks: what a 16-bit pixel can hold.
static inline uint32_t fb_quantize(uint32_t c, uint32_t fmt) {
    switch (fmt) {
    case 0: {  // 565, no alpha
        uint32_t rb = c & 0xF800F8u, g = c & 0xFC00u;
        return rb | (rb >> 5 & 0x070007u) | g | (g >> 6 & 0x300u);
    }
    case 1: {  // 5551
        uint32_t rgb = c & 0xF8F8F8u;
        return rgb | (rgb >> 5 & 0x070707u) | ((c & 0x80000000u) ? 0xFF000000u : 0u);
    }
    default: {  // 4444
        uint32_t v = c & 0xF0F0F0F0u;
        return v | v >> 4;
    }
    }
}

/* The framebuffer read for one pixel.  When the framebuffer lies in a
   mapped window this is a direct pointer access; the general path (a
   framebuffer outside every window) is out of line.

   This runs once per pixel, so it must not be a function call. */
static uint32_t fb_read_slow(const Target *t, int x, int y) {
    uint32_t a = t->fb + (uint32_t)(y * (int)t->fb_stride + x) * (t->fb_fmt == 3 ? 4 : 2);
    if (t->fb_fmt == 3) return mem_r32(a);
    return fb_decode16(mem_r16(a), t->fb_fmt);
}

static inline __attribute__((always_inline)) uint32_t fb_read(const Target *t, int x, int y) {
    uint32_t off = (uint32_t)(y * (int)t->fb_stride + x) * (t->fb_fmt == 3 ? 4u : 2u);
    if (t->fbuf && off + 4 <= t->fbuf_avail) {
#ifdef HLE_TRACE
        g_fb_fast++;
#endif
        const uint8_t *p = t->fbuf + off;
        if (t->fb_fmt == 3) {
            uint32_t v;
            memcpy(&v, p, 4);
            return __builtin_bswap32(v);
        }
        uint16_t v;
        memcpy(&v, p, 2);
        return fb_decode16(__builtin_bswap16(v), t->fb_fmt);
    }
#ifdef HLE_TRACE
    g_fb_slow++;
#endif
    return fb_read_slow(t, x, y);
}

static void fb_write_slow(const Target *t, int x, int y, uint32_t c, uint32_t mask) {
    uint32_t a = t->fb + (uint32_t)(y * (int)t->fb_stride + x) * (t->fb_fmt == 3 ? 4 : 2);
    if (t->fb_fmt == 3) {
        if (mask) c = (c & ~mask) | (mem_r32(a) & mask);
        mem_w32(a, c);
        return;
    }
    uint32_t v = fb_encode16(c, t->fb_fmt);
    if (mask) {
        uint32_t old = mem_r16(a), m16;
        switch (t->fb_fmt) {
        case 0: m16 = (mask & 0xF8 ? 0x1F : 0) | (mask & 0xFC00 ? 0x7E0 : 0) | (mask & 0xF80000 ? 0xF800 : 0); break;
        case 1: m16 = (mask & 0xF8 ? 0x1F : 0) | (mask & 0xF800 ? 0x3E0 : 0) | (mask & 0xF80000 ? 0x7C00 : 0) |
                      (mask & 0x80000000u ? 0x8000 : 0); break;
        default: m16 = (mask & 0xF0 ? 0xF : 0) | (mask & 0xF000 ? 0xF0 : 0) | (mask & 0xF00000 ? 0xF00 : 0) |
                       (mask & 0xF0000000u ? 0xF000 : 0); break;
        }
        v = (v & ~m16) | (old & m16);
    }
    mem_w16(a, (uint16_t)v);
}

/* The framebuffer write for one pixel.  As with fb_read(), this must not
   be a function call: it runs once per pixel. */
static inline __attribute__((always_inline)) void fb_write(const Target *t, int x, int y, uint32_t c, uint32_t mask) {
    uint32_t woff = (uint32_t)(y * (int)t->fb_stride + x) * (t->fb_fmt == 3 ? 4u : 2u);
    if (t->fbuf && woff + 4 <= t->fbuf_avail) {
        uint8_t *p = t->fbuf + woff;
        if (t->fb_fmt == 3) {
            if (mask) {
                uint32_t old;
                memcpy(&old, p, 4);
                c = (c & ~mask) | (__builtin_bswap32(old) & mask);
            }
            uint32_t v = __builtin_bswap32(c);
            memcpy(p, &v, 4);
            return;
        }
        uint32_t v = fb_encode16(c, t->fb_fmt);
        if (mask) {
            uint16_t old;
            memcpy(&old, p, 2);
            uint32_t ov = __builtin_bswap16(old), m16;
            switch (t->fb_fmt) {
            case 0: m16 = (mask & 0xF8 ? 0x1F : 0) | (mask & 0xFC00 ? 0x7E0 : 0) | (mask & 0xF80000 ? 0xF800 : 0); break;
            case 1: m16 = (mask & 0xF8 ? 0x1F : 0) | (mask & 0xF800 ? 0x3E0 : 0) | (mask & 0xF80000 ? 0x7C00 : 0) |
                          (mask & 0x80000000u ? 0x8000 : 0); break;
            default: m16 = (mask & 0xF0 ? 0xF : 0) | (mask & 0xF000 ? 0xF0 : 0) | (mask & 0xF00000 ? 0xF00 : 0) |
                         (mask & 0xF0000000u ? 0xF000 : 0); break;
            }
            v = (v & ~m16) | (ov & m16);
        }
        uint16_t o = __builtin_bswap16((uint16_t)v);
        memcpy(p, &o, 2);
        return;
    }
    fb_write_slow(t, x, y, c, mask);
}

// ---------------------------------------------------------------- internal resolution
// With a render scale N > 1, color and depth targets are drawn into host-side
// surfaces N times larger than the PSP's. Guest VRAM is kept coherent: when a
// display list ends, or before something else reads the target (a texture, a
// block transfer), the surface is downsampled into VRAM ("resolve"). If VRAM
// changed behind our back (CPU writes, transfers, 1x drawing) the surface is
// rebuilt from it before the next draw, detected by hashing.

#define MAX_SURFS 12
#define SURF_MAX_ROWS 512
typedef struct {
    uint32_t addr, stride, fmt;  // fmt 0..3 color, 4 depth
    int rows, scale;              // guest rows covered
    void *pix;                    // uint32_t (color) or uint16_t (depth), (stride*scale) x (rows*scale)
    uint8_t *mark;                // guest pixels drawn since the last resolve (stride x rows)
    int dirty_lo, dirty_hi;       // guest rows drawn since the last resolve (lo > hi: clean)
    int verified;                 // VRAM known to match the surface since the last resolve
    uint64_t vram_hash;           // hash of the guest rows after the last resolve
    uint32_t last_use;
} Surf;

static Surf g_surfs[MAX_SURFS];
static int g_scale = 1, g_scale_req = 0;
static uint32_t g_surf_tick;
static pthread_mutex_t g_surf_lock = PTHREAD_MUTEX_INITIALIZER;  // vs. the frontend's scanout

static uint64_t hash_bytes(const uint8_t *p, size_t n);
static int g_band_shift = 3;  // raster band strip height: 8 rows at 1x, 16 at 2-3x, 32 at 4x

// GPU renderer: wanted (setting), available (a GL context runs glr_execute).
// Switched at the end of a display list (gpu_resolve_all), a safe point.
static volatile int g_gpu_pref = 1, g_gpu_avail;
static void glr_switched(void);
void gpu_set_gpu_renderer(int on) { g_gpu_pref = on != 0; }
int gpu_renderer_wanted(void) { return g_gpu_pref; }
void gpu_glr_available(int on) { g_gpu_avail = on != 0; }

void gpu_set_render_scale(int n) { g_scale_req = n < 1 ? 1 : n > 4 ? 4 : n; }
int gpu_render_scale(void) { return g_scale_req ? g_scale_req : g_scale; }

static uint32_t surf_bpp(const Surf *f) { return f->fmt == 3 ? 4 : 2; }
static uint32_t surf_bytes(const Surf *f) { return f->stride * (uint32_t)f->rows * surf_bpp(f); }

/* (The range test used to compare the address with GUEST_MEM_SIZE, which on
   the Wii is the size of the RAM window, not an address limit: every VRAM
   and RAM address failed it, so the hash was always 0 and a change was
   never seen.) */
static uint64_t surf_vram_hash(const Surf *f) {
    uint32_t n = guest_span(f->addr, surf_bytes(f));
    const uint8_t *p = n ? guest_host_ptr(f->addr, n) : NULL;
    return p ? hash_bytes(p, n) : 0;
}

// Surface <- VRAM (nearest).
static void surf_upload(Surf *f) {
    int S = f->scale, sw = (int)f->stride * S;
    for (int y = 0; y < f->rows; y++)
        for (int x = 0; x < (int)f->stride; x++) {
            uint32_t a = f->addr + (uint32_t)(y * (int)f->stride + x) * surf_bpp(f);
            if (f->fmt == 4) {
                uint16_t z = mem_r16(a);
                uint16_t *d = (uint16_t *)f->pix + (y * S) * sw + x * S;
                for (int j = 0; j < S; j++)
                    for (int i = 0; i < S; i++) d[j * sw + i] = z;
            } else {
                uint32_t c = f->fmt == 3 ? mem_r32(a) : fb_decode16(mem_r16(a), f->fmt);
                uint32_t *d = (uint32_t *)f->pix + (y * S) * sw + x * S;
                for (int j = 0; j < S; j++)
                    for (int i = 0; i < S; i++) d[j * sw + i] = c;
            }
        }
    f->verified = 1;
    f->vram_hash = surf_vram_hash(f);
}

// VRAM <- surface (box filter) for the rows drawn since the last resolve.
static void surf_resolve(Surf *f) {
    if (f->dirty_lo > f->dirty_hi) return;
    int S = f->scale, sw = (int)f->stride * S, n = S * S;
    int lo = f->dirty_lo, hi = f->dirty_hi < f->rows - 1 ? f->dirty_hi : f->rows - 1;
    // only pixels actually drawn: the CPU may have put other data in the rest meanwhile
    for (int y = lo; y <= hi; y++)
        for (int x = 0; x < (int)f->stride; x++) {
            uint8_t *m = &f->mark[y * (int)f->stride + x];
            if (!*m) continue;
            *m = 0;
            uint32_t a = f->addr + (uint32_t)(y * (int)f->stride + x) * surf_bpp(f);
            if (f->fmt == 4) {
                mem_w16(a, ((uint16_t *)f->pix)[(y * S) * sw + x * S]);
                continue;
            }
            const uint32_t *src = (const uint32_t *)f->pix + (y * S) * sw + x * S;
            uint32_t acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < S; j++)
                for (int i = 0; i < S; i++) {
                    uint32_t c = src[j * sw + i];
                    acc[0] += c & 0xFF; acc[1] += c >> 8 & 0xFF; acc[2] += c >> 16 & 0xFF; acc[3] += c >> 24;
                }
            uint32_t c = (acc[0] / n) | (acc[1] / n) << 8 | (acc[2] / n) << 16 | (acc[3] / n) << 24;
            if (f->fmt == 3) mem_w32(a, c);
            else mem_w16(a, (uint16_t)fb_encode16(c, f->fmt));
        }
    f->dirty_lo = 1 << 30;
    f->dirty_hi = -1;
    f->vram_hash = surf_vram_hash(f);
    f->verified = 0;  // check VRAM again before the next draw
}

static void surf_drop(Surf *f) {
    surf_resolve(f);
    free(f->pix);
    free(f->mark);
    memset(f, 0, sizeof *f);
}

// Resolve every surface overlapping [lo, hi) (something is about to read it).
static void surf_resolve_range(uint32_t lo, uint32_t hi) {
    lo &= 0x1FFFFFFF;
    hi &= 0x1FFFFFFF;
    for (int k = 0; k < MAX_SURFS; k++) {
        Surf *f = &g_surfs[k];
        uint32_t a = f->addr & 0x1FFFFFFF;
        if (f->pix && f->dirty_lo <= f->dirty_hi && lo < a + surf_bytes(f) && a < hi) {
            raster_flush();
            pthread_mutex_lock(&g_surf_lock);
            surf_resolve(f);
            pthread_mutex_unlock(&g_surf_lock);
        }
    }
}

// VRAM in [lo, hi) is about to be written by something other than the
// rasterizer: put pending pixels there first so the write lands on top.
static void surf_vram_written(uint32_t lo, uint32_t hi, const void *except) {
    surf_resolve_range(lo, hi);
    lo &= 0x1FFFFFFF;
    hi &= 0x1FFFFFFF;
    for (int k = 0; k < MAX_SURFS; k++) {
        Surf *f = &g_surfs[k];
        uint32_t a = f->addr & 0x1FFFFFFF;
        if (f->pix && f != except && lo < a + surf_bytes(f) && a < hi) f->verified = 0;
    }
}

// The color surface holding guest pixel (x, y) of a buffer at addr/stride, or NULL.
static Surf *surf_at(uint32_t addr, uint32_t stride, uint32_t bpp, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     int *sx, int *sy) {
    uint32_t a0 = (addr & 0x1FFFFFFF) + (y * stride + x) * bpp;
    for (int k = 0; k < MAX_SURFS; k++) {
        Surf *f = &g_surfs[k];
        if (!f->pix || f->fmt == 4 || f->stride != stride || surf_bpp(f) != bpp) continue;
        uint32_t fa = f->addr & 0x1FFFFFFF;
        if (a0 < fa || (a0 - fa) % bpp) continue;
        uint32_t off = (a0 - fa) / bpp, ry = off / stride, rx = off % stride;
        if (rx + w > stride || ry + h > (uint32_t)f->rows) continue;
        *sx = (int)rx;
        *sy = (int)ry;
        return f;
    }
    return NULL;
}

// Block transfer into a surface (already done in VRAM): apply it to the surface
// too, at full resolution when the source is a surface, instead of letting the
// whole surface be rebuilt from VRAM. Returns 1 if handled.
static Surf *g_transfer_target;  // the surface surf_transfer updated

static int surf_transfer(uint32_t src, uint32_t sstride, uint32_t sx, uint32_t sy, uint32_t dst, uint32_t dstride,
                         uint32_t dx, uint32_t dy, uint32_t w, uint32_t h, uint32_t bpp) {
    int fx, fy, gx, gy;
    Surf *d = surf_at(dst, dstride, bpp, dx, dy, w, h, &fx, &fy);
    g_transfer_target = d;
    if (!d) return 0;
    Surf *sf = surf_at(src, sstride, bpp, sx, sy, w, h, &gx, &gy);
    if (sf && (sf->scale != d->scale || sf->fmt != d->fmt)) sf = NULL;
    int S = d->scale, dw = (int)d->stride * S;
    pthread_mutex_lock(&g_surf_lock);
    if (sf) {
        int sw = (int)sf->stride * S;
        for (int y = 0; y < (int)h * S; y++)
            memmove((uint32_t *)d->pix + (fy * S + y) * dw + fx * S, (uint32_t *)sf->pix + (gy * S + y) * sw + gx * S,
                    (size_t)w * (size_t)S * 4);
    } else {
        for (int y = 0; y < (int)h; y++)
            for (int x = 0; x < (int)w; x++) {
                uint32_t a = d->addr + (uint32_t)((fy + y) * (int)d->stride + fx + x) * bpp;
                uint32_t c = d->fmt == 3 ? mem_r32(a) : fb_decode16(mem_r16(a), d->fmt);
                uint32_t *o = (uint32_t *)d->pix + ((fy + y) * S) * dw + (fx + x) * S;
                for (int j = 0; j < S; j++)
                    for (int i = 0; i < S; i++) o[j * dw + i] = c;
            }
    }
    if (d->verified) d->vram_hash = surf_vram_hash(d);  // still in sync
    pthread_mutex_unlock(&g_surf_lock);
    return 1;
}

// End of a display list: make VRAM match what was drawn. Also applies a
// pending scale change. Called on the GPU thread with the queue drained.
void gpu_resolve_all(void) {
    int want = g_gpu_pref && g_gpu_avail && !getenv("HEADLESS");
    if (want != g_glr_active) {
        g_glr_active = want;
        glr_switched();
        fprintf(stderr, "renderer: %s\n", want ? "GPU" : "software");
    }
    g_tex_list++;
    pthread_mutex_lock(&g_surf_lock);
    if (g_scale_req && g_scale_req != g_scale) {
        for (int k = 0; k < MAX_SURFS; k++)
            if (g_surfs[k].pix) surf_drop(&g_surfs[k]);
        g_scale = g_scale_req;
        g_band_shift = g_scale >= 4 ? 5 : g_scale >= 2 ? 4 : 3;  // the queue is drained here
        fprintf(stderr, "render scale %dx\n", g_scale);
    }
    g_scale_req = 0;
    for (int k = 0; k < MAX_SURFS; k++)
        if (g_surfs[k].pix) surf_resolve(&g_surfs[k]);
    pthread_mutex_unlock(&g_surf_lock);
}

// The surface for (addr, stride, fmt) with at least `rows` rows; may flush.
static Surf *surf_get(uint32_t addr, uint32_t stride, uint32_t fmt, int rows) {
    rows = rows <= 272 ? 272 : (rows + 15) & ~15;
    uint32_t bpp = fmt == 3 ? 4 : 2;
    uint32_t lo = addr & 0x1FFFFFFF, hi = lo + stride * (uint32_t)rows * bpp;
    Surf *hit = NULL, *free_slot = NULL, *lru = NULL;
    for (int k = 0; k < MAX_SURFS; k++) {
        Surf *f = &g_surfs[k];
        if (!f->pix) {
            if (!free_slot) free_slot = f;
            continue;
        }
        if (f->addr == addr && f->stride == stride && f->fmt == fmt && f->rows >= rows && f->scale == g_scale) {
            hit = f;
            continue;
        }
        if (!lru || f->last_use < lru->last_use) lru = f;
    }
    // anything else covering the same memory goes (VRAM keeps its pixels)
    for (int k = 0; k < MAX_SURFS; k++) {
        Surf *f = &g_surfs[k];
        uint32_t a = f->addr & 0x1FFFFFFF;
        if (f->pix && f != hit && lo < a + surf_bytes(f) && a < hi) {
            raster_flush();
            pthread_mutex_lock(&g_surf_lock);
            surf_drop(f);
            pthread_mutex_unlock(&g_surf_lock);
            if (!free_slot) free_slot = f;
        }
    }
    if (hit) {
        if (!hit->verified) {
            if (surf_vram_hash(hit) != hit->vram_hash) {
                raster_flush();
                pthread_mutex_lock(&g_surf_lock);
                surf_upload(hit);
                pthread_mutex_unlock(&g_surf_lock);
            }
            hit->verified = 1;
        }
        hit->last_use = ++g_surf_tick;
        return hit;
    }
    Surf *f = free_slot;
    if (!f) {
        raster_flush();
        pthread_mutex_lock(&g_surf_lock);
        surf_drop(lru);
        pthread_mutex_unlock(&g_surf_lock);
        f = lru;
    }
    size_t n = (size_t)stride * (size_t)g_scale * (size_t)rows * (size_t)g_scale;
    void *pix = malloc(n * (fmt == 4 ? 2 : 4));
    uint8_t *mark = calloc((size_t)stride * (size_t)rows, 1);
    if (!pix || !mark) {
        free(pix);
        free(mark);
        return NULL;
    }
    pthread_mutex_lock(&g_surf_lock);
    *f = (Surf){.addr = addr, .stride = stride, .fmt = fmt, .rows = rows, .scale = g_scale, .pix = pix, .mark = mark,
                .dirty_lo = 1 << 30, .dirty_hi = -1, .last_use = ++g_surf_tick};
    surf_upload(f);
    pthread_mutex_unlock(&g_surf_lock);
    return f;
}

// Attach scaled surfaces to a target about to be drawn (GPU thread).
static void target_scale(Target *t, int uses_depth) {
    if (g_scale <= 1 || g_glr_active || t->fb_stride == 0 || t->sy2 >= SURF_MAX_ROWS) return;
    int rows = t->sy2 + 1;
    Surf *c = surf_get(t->fb, t->fb_stride, t->fb_fmt, rows);
    if (!c) return;
    Surf *z = NULL;
    if (uses_depth && t->zb_stride) {
        z = surf_get(t->zb, t->zb_stride, 4, rows);
        if (!z || t->zb_stride != t->fb_stride) return;  // keep it simple: one row length
    }
    // surf_get may have dropped c while making room for z
    if (c->addr != t->fb || c->fmt != t->fb_fmt || !c->pix) return;
    int S = g_scale;
    t->scale = S;
    t->mshift = S == 2 ? 1 : S == 4 ? 2 : -1;
    t->scol = (uint32_t *)c->pix;
    t->sz = z ? (uint16_t *)z->pix : NULL;
    t->cmark = c->mark;
    t->zmark = z ? z->mark : NULL;
    t->sw = (int)t->fb_stride * S;
    t->sx1 *= S;
    t->sy1 *= S;
    t->sx2 = (t->sx2 + 1) * S - 1;
    t->sy2 = (t->sy2 + 1) * S - 1;
    // the surfaces end where the buffer does (VRAM would wrap; we must not overrun)
    if (t->sx2 > (int)t->fb_stride * S - 1) t->sx2 = (int)t->fb_stride * S - 1;
    if (t->sy2 > c->rows * S - 1) t->sy2 = c->rows * S - 1;
    if (z && t->sy2 > z->rows * S - 1) t->sy2 = z->rows * S - 1;
    int lo = t->sy1 / S, hi = t->sy2 / S;
    if (lo < c->dirty_lo) c->dirty_lo = lo;
    if (hi > c->dirty_hi) c->dirty_hi = hi;
    if (z) {
        if (lo < z->dirty_lo) z->dirty_lo = lo;
        if (hi > z->dirty_hi) z->dirty_hi = hi;
    }
}

// Frontend: copy the displayed framebuffer at full internal resolution if a
// surface for it is in sync with VRAM. Returns the scale used (0: none).
int gpu_scanout_scaled(uint32_t addr, uint32_t stride, uint32_t fmt, uint16_t *out, int max_scale) {
    int got = 0;
    pthread_mutex_lock(&g_surf_lock);
    for (int k = 0; k < MAX_SURFS && !got; k++) {
        Surf *f = &g_surfs[k];
        if (!f->pix || f->addr != addr || f->stride != stride || f->fmt != fmt || f->scale > max_scale) continue;
        // The surface is only out of date if something wrote VRAM behind the
        // rasterizer's back, which surf_vram_written() records.  Hashing the
        // whole framebuffer here would cost a megabyte of reads per frame.
        if (f->rows < 272 || !f->verified) break;
        int S = f->scale, sw = (int)stride * S;
        for (int y = 0; y < 272 * S; y++) {
            const uint32_t *src = (const uint32_t *)f->pix + y * sw;
            uint16_t *dst = out + y * 480 * S;
            /* The surface is decoded RGBA; the display path wants the same
               16-bit pixels the guest's own framebuffer would give. */
            for (int x = 0; x < 480 * S; x++) {
                uint32_t p = src[x];
                uint32_t r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
                dst[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
        got = S;
    }
    pthread_mutex_unlock(&g_surf_lock);
    return got;
}

// ---------------------------------------------------------------- textures

typedef struct {
    const uint32_t *pix;  // decoded RGBA8888 texels (texture cache)
    int pw, ph;           // size of pix: w x h, or larger for a replacement
    uint32_t serial;      // identifies these texels (GPU renderer cache)
    float psx, psy;       // pw / w, ph / h
    int enabled, fmt, swizzle, w, h, stride, clamp_u, clamp_v, linear, func, rgba, dbl;
    uint32_t addr;
    float env[3];
    int clut_fmt, clut_shift, clut_mask, clut_off;
    int native_ok;        // raw indices/palette can be uploaded natively to GX
    void *entry;          // the texture cache entry the texels came from (TexEntry *)
    int gx_only;          // pix is NULL: only the chip's converted copy (serial) is left
    int blit;             // pix is NULL: the blit of a presented display buffer, answered with a copy of the chip's picture
} TexState;

static void tex_state(TexState *t) {
    t->enabled = R(GE_TEXTUREMAPENABLE) & 1;
    if (!t->enabled) return;
    t->fmt = (int)(R(GE_TEXFORMAT) & 0xF);
    t->swizzle = R(GE_TEXMODE) & 1;
    t->w = 1 << (R(GE_TEXSIZE0) & 0xF);
    t->h = 1 << (R(GE_TEXSIZE0) >> 8 & 0xF);
    t->stride = (int)(R(GE_TEXBUFWIDTH0) & 0x7FF);
    t->addr = (R(GE_TEXADDR0) & 0xFFFFF0u) | ((R(GE_TEXBUFWIDTH0) << 8) & 0x0F000000u);
    t->clamp_u = R(GE_TEXWRAP) & 1;
    t->clamp_v = R(GE_TEXWRAP) >> 8 & 1;
    t->linear = (R(GE_TEXFILTER) >> 8) & 1;
    t->func = (int)(R(GE_TEXFUNC) & 7);
    t->rgba = R(GE_TEXFUNC) >> 8 & 1;
    t->dbl = R(GE_TEXFUNC) >> 16 & 1;
    uint32_t env = R(GE_TEXENVCOLOR);
    t->env[0] = (float)(env & 0xFF); t->env[1] = (float)(env >> 8 & 0xFF); t->env[2] = (float)(env >> 16 & 0xFF);
    uint32_t cf = R(GE_CLUTFORMAT);
    t->clut_fmt = (int)(cf & 3);
    t->clut_shift = (int)(cf >> 2 & 0x1F);
    t->clut_mask = (int)(cf >> 8 & 0xFF);
    t->clut_off = (int)((cf >> 16 & 0x1F) << 4);
    t->native_ok = 0;
    t->entry = NULL;
    t->gx_only = 0;
    t->blit = 0;
}

static inline __attribute__((always_inline)) uint32_t c16_to_32(uint32_t v, int fmt) {
    uint32_t r, g, b, a;
    switch (fmt) {
    case 0: r = (v & 31) << 3 | (v & 31) >> 2; g = (v >> 5 & 63) << 2 | (v >> 5 & 63) >> 4;
            b = (v >> 11 & 31) << 3 | (v >> 11 & 31) >> 2; a = 255; break;
    case 1: r = (v & 31) << 3 | (v & 31) >> 2; g = (v >> 5 & 31) << 3 | (v >> 5 & 31) >> 2;
            b = (v >> 10 & 31) << 3 | (v >> 10 & 31) >> 2; a = (v >> 15) ? 255 : 0; break;
    default: r = (v & 15) * 17; g = (v >> 4 & 15) * 17; b = (v >> 8 & 15) * 17; a = (v >> 12) * 17; break;
    }
    return r | g << 8 | b << 16 | a << 24;
}

/* The palette is copied byte-for-byte from the guest, which is little
   endian.  The Wii's PowerPC is big endian, so a palette entry must be
   swapped before it is expanded.  (The texture formats above use mem_r16/
   mem_r32, which do this; this path used to cast the raw bytes instead and
   every CLUT texture came out with its red/blue channels exchanged.) */
static inline uint16_t clut16_at(const TexState *t, uint32_t idx) {
    uint16_t v;
    memcpy(&v, (const uint8_t *)g_ge.clut + (idx & 511) * 2, 2);
#ifdef __wii__
    v = __builtin_bswap16(v);
#endif
    return v;
}

static inline uint32_t clut32_at(const TexState *t, uint32_t idx) {
    uint32_t v;
    memcpy(&v, (const uint8_t *)g_ge.clut + (idx & 255) * 4, 4);
#ifdef __wii__
    v = __builtin_bswap32(v);
#endif
    return v;
}

static uint32_t clut_lookup(const TexState *t, uint32_t idx) {
    idx = ((idx >> t->clut_shift) & (uint32_t)t->clut_mask) | (uint32_t)t->clut_off;
    if (t->clut_fmt == 3) return clut32_at(t, idx);
    return c16_to_32(clut16_at(t, idx), t->clut_fmt);
}

// Byte address of texel (x, y) for a texture with `bpp_bits` bits per texel.
static uint32_t texel_addr(const TexState *t, int x, int y, int bpp_bits) {
    uint32_t row_bytes = (uint32_t)t->stride * (uint32_t)bpp_bits / 8;
    uint32_t xb = (uint32_t)x * (uint32_t)bpp_bits / 8;
    if (!t->swizzle) return t->addr + (uint32_t)y * row_bytes + xb;
    // swizzled: 16-byte x 8-row blocks laid out linearly
    uint32_t blocks_per_row = row_bytes / 16;
    uint32_t block = ((uint32_t)y / 8) * blocks_per_row + xb / 16;
    return t->addr + block * 128 + ((uint32_t)y % 8) * 16 + xb % 16;
}

static uint32_t dxt_color(uint32_t a, int x, int y, int alpha_mode);
static uint32_t tex_bytes(const TexState *t);

/* Bulk decode for the formats the game actually uses.  The old path called
   fetch() for every texel; fetch() performs a guarded guest read (and a
   scheduler tick) and, for a paletted texture, another guarded CLUT read and
   channel expansion for every texel.  A 512x256 card texture therefore cost
   two guest-memory ticks per texel, which is visible as a 30-50 ms frame
   whenever the menu loads a new card.  Read the contiguous source once, build
   the 256-entry palette once, and index it directly. */
/* A 16-bit texel is expanded from two 256-entry tables, one for each byte of the little-endian value.  Every bit of
   c16_to_32()'s result is a copy of one input bit (the replicated top bits of a 5- or 6-bit channel, the alpha of a 5551,
   a 4-bit channel times 17) or a constant (the alpha of a 565), so the expansion of a value is the OR of the expansions of
   its two halves; the green of a 565, which straddles them, is no exception.  Built on first use from c16_to_32() itself;
   the self-test compares them for all 65536 values of the three formats. */
static uint32_t s_c16_tab[3][2][256];
static int s_c16_tab_ready;
static void c16_tab_init(void) {
    for (int f = 0; f < 3; f++)
        for (uint32_t i = 0; i < 256; i++) {
            s_c16_tab[f][0][i] = c16_to_32(i, f);
            s_c16_tab[f][1][i] = c16_to_32(i << 8, f);
        }
    s_c16_tab_ready = 1;
}
#ifdef EXP_NOC16TAB   /* the per-texel expansion of the first decode rewrite, for an A/B build */
#define DEC565_(p_) c16_to_32(ld16le(p_), 0)
#define DEC5551_(p_) c16_to_32(ld16le(p_), 1)
#define DEC4444_(p_) c16_to_32(ld16le(p_), 2)
#else
static inline __attribute__((always_inline)) uint32_t c16_tab_dec(const uint8_t *p, int fmt) {
    const uint32_t v = ld16le(p);
    return s_c16_tab[fmt][0][v & 255] | s_c16_tab[fmt][1][v >> 8];
}
#define DEC565_(p_) c16_tab_dec(p_, 0)
#define DEC5551_(p_) c16_tab_dec(p_, 1)
#define DEC4444_(p_) c16_tab_dec(p_, 2)
#endif
static int tex_decode_fast(const TexState *t, uint32_t *dst) {
    if (t->fmt > 5) return 0;              // DXT and CLUT16/32 use the generic path
    uint32_t bytes = tex_bytes(t);
    if (!bytes) return 0;
    const uint8_t *src = guest_host_ptr(t->addr, bytes);
    if (!src) return 0;
    if (t->fmt <= 2 && !s_c16_tab_ready) c16_tab_init();
    uint32_t pal[256];
    if (t->fmt >= 4) {
        for (uint32_t i = 0; i < 256; i++) pal[i] = clut_lookup(t, i);
    }
    if (t->fmt <= 3) {
#ifdef EXP_OLDPALDECODE
        for (int y = 0; y < t->h; y++) {
            uint32_t *row = dst + (size_t)y * (size_t)t->w;
            if (t->fmt == 3) {
                for (int x = 0; x < t->w; x++)
                    row[x] = ld32le(src + texel_addr(t, x, y, 32) - t->addr);
            } else {
                for (int x = 0; x < t->w; x++)
                    row[x] = c16_to_32(ld16le(src + texel_addr(t, x, y, 16) - t->addr), t->fmt);
            }
        }
        return 1;
#else
        /* The same addresses as texel_addr(), without its multiplications and divisions a texel (~100 ns a texel: 27 ms for
           a 512x512 card or background): a row is at y * row_bytes, or, swizzled, in pieces of 16 bytes (8 texels of 16
           bits, 4 of 32) 128 bytes apart in blocks of 8 rows. */
        const int bits = t->fmt == 3 ? 32 : 16;
        const uint32_t row_bytes = (uint32_t)t->stride * (uint32_t)bits / 8;
        const uint32_t blocks_per_row = row_bytes / 16;
        for (int y = 0; y < t->h; y++) {
            uint32_t *row = dst + (size_t)y * (size_t)t->w;
            const uint8_t *s = t->swizzle ? src + ((size_t)((uint32_t)y / 8) * blocks_per_row) * 128 + ((uint32_t)y % 8) * 16
                                          : src + (size_t)y * row_bytes;
#define DEC_ROW(EXPR_) do { \
                if (t->swizzle) { \
                    for (int x = 0; x < t->w; x++) row[x] = EXPR_(s + (x >> SH_) * 128 + (x & MASK_) * BYTES_); \
                } else { \
                    for (int x = 0; x < t->w; x++) row[x] = EXPR_(s + x * BYTES_); \
                } } while (0)
#define SH_ 3
#define MASK_ 7
#define BYTES_ 2
            if (t->fmt == 0) DEC_ROW(DEC565_);
            else if (t->fmt == 1) DEC_ROW(DEC5551_);
            else if (t->fmt == 2) DEC_ROW(DEC4444_);
#undef SH_
#undef MASK_
#undef BYTES_
#define SH_ 2
#define MASK_ 3
#define BYTES_ 4
            else DEC_ROW(ld32le);
#undef SH_
#undef MASK_
#undef BYTES_
#undef DEC_ROW
        }
#ifdef EXP_TEXDECCHK
        {
            static unsigned n, bad;
            unsigned mism = 0;
            for (int y = 0; y < t->h; y++)
                for (int x = 0; x < t->w; x++) {
                    const uint8_t *q = src + texel_addr(t, x, y, bits) - t->addr;
                    const uint32_t want = t->fmt == 3 ? ld32le(q) : c16_to_32(ld16le(q), t->fmt);
                    mism += dst[(size_t)y * (size_t)t->w + (size_t)x] != want;
                }
            n++;
            if (mism && ++bad <= 8)
                fprintf(stderr, "TEXDECCHK16 BAD fmt=%d %dx%d stride=%d swizzle=%d: %u texels differ\n", t->fmt, t->w, t->h, t->stride, t->swizzle, mism);
            if ((n & 15) == 1) fprintf(stderr, "TEXDECCHK16 %u textures, %u bad\n", n, bad);
        }
#endif
        return 1;
#endif
    }
    const int bits = t->fmt == 4 ? 4 : 8;
#ifdef EXP_OLDPALDECODE
    for (int y = 0; y < t->h; y++) {
        uint32_t *row = dst + (size_t)y * (size_t)t->w;
        for (int x = 0; x < t->w; x++) {
            uint32_t off = texel_addr(t, x, y, bits) - t->addr;
            uint8_t b = src[off];
            if (t->fmt == 4) b = (x & 1) ? b >> 4 : b & 15;
            row[x] = pal[b];
        }
    }
    return 1;
#else
    /* The same addresses as texel_addr(), one source row (or one 16-byte piece of a swizzled row) at a time: a byte holds
       one texel (CLUT8) or two (CLUT4, the low nibble first).  The per-texel address arithmetic this replaces was 40-50
       cycles a texel: ~5 ms for a 256x256 CLUT4 texture. */
    const uint32_t row_bytes = (uint32_t)t->stride * (uint32_t)bits / 8;
    const uint32_t wbytes = (uint32_t)(bits == 4 ? (t->w + 1) / 2 : t->w);
    const uint32_t blocks_per_row = row_bytes / 16;
    for (int y = 0; y < t->h; y++) {
        uint32_t *row = dst + (size_t)y * (size_t)t->w;
        if (!t->swizzle) {
            const uint8_t *s = src + (size_t)y * row_bytes;
            if (bits == 8) {
                for (int x = 0; x < t->w; x++) row[x] = pal[s[x]];
            } else {
                int x = 0;
                for (; x + 1 < t->w; x += 2) {
                    const uint8_t b = s[x >> 1];
                    row[x] = pal[b & 15];
                    row[x + 1] = pal[b >> 4];
                }
                if (x < t->w) row[x] = pal[s[x >> 1] & 15];
            }
        } else {
            /* swizzled: blocks of 16 bytes by 8 rows, laid out one after the other */
            const uint8_t *s = src + ((size_t)((uint32_t)y / 8) * blocks_per_row) * 128 + ((uint32_t)y % 8) * 16;
            uint32_t *d = row, *end = row + t->w;
            for (uint32_t done = 0; done < wbytes; done += 16, s += 128) {
                const uint32_t n = wbytes - done < 16 ? wbytes - done : 16;
                if (bits == 8) {
                    for (uint32_t i = 0; i < n; i++) *d++ = pal[s[i]];
                } else {
                    for (uint32_t i = 0; i < n; i++) {
                        const uint8_t b = s[i];
                        *d++ = pal[b & 15];
                        if (d < end) *d++ = pal[b >> 4];
                    }
                }
            }
        }
    }
#ifdef EXP_TEXDECCHK   /* every texel against the per-texel addressing it replaced */
    {
        static unsigned n, bad;
        unsigned mism = 0;
        for (int y = 0; y < t->h; y++)
            for (int x = 0; x < t->w; x++) {
                uint8_t b = src[texel_addr(t, x, y, bits) - t->addr];
                if (t->fmt == 4) b = (x & 1) ? b >> 4 : b & 15;
                mism += dst[(size_t)y * (size_t)t->w + (size_t)x] != pal[b];
            }
        n++;
        if (mism && ++bad <= 8)
            fprintf(stderr, "TEXDECCHK BAD fmt=%d %dx%d stride=%d swizzle=%d: %u texels differ\n", t->fmt, t->w, t->h, t->stride, t->swizzle, mism);
        if ((n & 63) == 1) fprintf(stderr, "TEXDECCHK %u textures, %u bad\n", n, bad);
    }
#endif
    return 1;
#endif
}

static uint32_t fetch(const TexState *t, int x, int y) {
    switch (t->fmt) {
    case 0: case 1: case 2: return c16_to_32(mem_r16(texel_addr(t, x, y, 16)), t->fmt);
    case 3: return mem_r32(texel_addr(t, x, y, 32));
    case 4: {
        uint8_t b = mem_r8(texel_addr(t, x, y, 4));
        return clut_lookup(t, (x & 1) ? b >> 4 : b & 15);
    }
    case 5: return clut_lookup(t, mem_r8(texel_addr(t, x, y, 8)));
    case 6: return clut_lookup(t, mem_r16(texel_addr(t, x, y, 16)));
    case 7: return clut_lookup(t, mem_r32(texel_addr(t, x, y, 32)));
    default: {  // DXT1/3/5: 4x4 blocks
        int bsize = t->fmt == 8 ? 8 : 16;
        uint32_t blocks_per_row = (uint32_t)(t->stride / 4);
        uint32_t a = t->addr + (((uint32_t)y / 4) * blocks_per_row + (uint32_t)x / 4) * (uint32_t)bsize;
        return dxt_color(a, x & 3, y & 3, t->fmt);
    }
    }
}

static uint32_t rgb565(uint16_t v) { return c16_to_32(v, 0) & 0xFFFFFFu; }

static uint32_t dxt_color(uint32_t a, int x, int y, int fmt) {
    uint32_t cblk = fmt == 8 ? a : a + 8;  // DXT3/5: alpha first
    uint16_t c0 = mem_r16(cblk), c1 = mem_r16(cblk + 2);
    uint32_t bits = mem_r32(cblk + 4), code = (bits >> ((y * 4 + x) * 2)) & 3;
    uint32_t p0 = rgb565(c0), p1 = rgb565(c1), col;
#define MIX(a_, b_, n, d) ((((a_ & 0xFF) * n + (b_ & 0xFF) * (d - n)) / d) | ((((a_ >> 8 & 0xFF) * n + (b_ >> 8 & 0xFF) * (d - n)) / d) << 8) | ((((a_ >> 16 & 0xFF) * n + (b_ >> 16 & 0xFF) * (d - n)) / d) << 16))
    if (code == 0) col = p0;
    else if (code == 1) col = p1;
    else if (c0 > c1 || fmt != 8) col = code == 2 ? MIX(p0, p1, 2, 3) : MIX(p0, p1, 1, 3);
    else col = code == 2 ? MIX(p0, p1, 1, 2) : 0;
#undef MIX
    uint32_t alpha = 255;
    if (fmt == 8) alpha = (c0 <= c1 && code == 3) ? 0 : 255;
    else if (fmt == 9) alpha = ((mem_r16(a + (uint32_t)y * 2) >> (x * 4)) & 15) * 17;
    else {
        uint32_t a0 = mem_r8(a), a1 = mem_r8(a + 1);
        uint64_t ab = 0;
        for (int i = 0; i < 6; i++) ab |= (uint64_t)mem_r8(a + 2 + (uint32_t)i) << (8 * i);
        uint32_t ac = (uint32_t)(ab >> ((y * 4 + x) * 3)) & 7;
        if (ac == 0) alpha = a0;
        else if (ac == 1) alpha = a1;
        else if (a0 > a1) alpha = ((8 - ac) * a0 + (ac - 1) * a1) / 7;
        else alpha = ac == 6 ? 0 : ac == 7 ? 255 : ((6 - ac) * a0 + (ac - 1) * a1) / 5;
    }
    return col | alpha << 24;
}

// Fast floor/ceil for the rasterizer.  floorf()/ceilf() are libm calls on
// PowerPC and the rasterizer makes them once per pixel, which dominated the
// frame time; a float-to-int conversion truncates toward zero, so the floor
// is that value (minus one when a fraction was dropped) and the ceil is that
// value (plus one when a fraction was dropped).
static inline __attribute__((always_inline)) int fast_floor(float f) {
    int i = (int)f;
    return i - (f < (float)i);
}
static inline __attribute__((always_inline)) int fast_ceil(float f) {
    int i = (int)f;
    return i + (f > (float)i);
}

static inline __attribute__((always_inline)) int wrap(int c, int size, int clamp) {
    if (clamp) return c < 0 ? 0 : c >= size ? size - 1 : c;
    if (!(size & (size - 1))) return c & (size - 1);
    c %= size;  // replacement textures need not be powers of two
    return c < 0 ? c + size : c;
}

// ---------------------------------------------------------------- texture cache
// Decoded textures are kept across frames.  When something writes texture
// memory (TEXFLUSH, a block transfer) the entries covering that range are
// dropped; the rest stay valid.
//
// The generation counter used to be bumped on every new frame, which threw
// away every decoded texture each frame: the game then re-decoded its whole
// texture set per frame, and the rasterizer spent nearly all of its time
// copying texture data out of guest memory instead of drawing pixels.

/* 1024 entries of about 80 bytes.  A fight's set of textures and palette
   variants outgrew 512 four-way entries (about 40 set evictions a second
   with the texture pool still a quarter empty). */
#define TEX_CACHE 1024
typedef struct {
    uint32_t addr, key_fmt, key_size, key_clut, clut_hash, gen;
    int stale;  // the source may have changed: re-validate before reusing
    int epoch;  // texture pack generation (texrep_epoch)
    uint64_t data_hash;
    uint32_t *pix;
    int pw, ph, cap;  // pixel size (a replacement may be larger), allocated texels
    uint32_t serial;  // changes whenever pix is redecoded
    uint32_t bytes;   // source size in guest memory
    uint32_t used;    // when it was last looked up (eviction order)
    uint32_t checked_list;  // the display list in which its bytes were last hashed
    int native_ok;    // the source can be decoded by the GX as CI4/CI8
    /* The texels were given back to the pool because the chip holds a
       converted copy (under the same serial); a draw that the chip does
       not draw, or one after the chip lost its copy, decodes them again. */
    int gx_only;
#ifdef TKDR_AUTOPILOT
    uint32_t nz0, list0, frame0;   /* at the decode: non-zero source bytes, the display list, the frame (debug) */
#endif
} TexEntry;
/* Set associative: a texture can live in any of TEX_WAYS entries of its set.
   With one entry per set, two textures that hashed to the same slot evicted
   each other on every use; that was 96% of all texture decodes (and as many
   GX conversions) in the attract fights. */
#define TEX_WAYS 8
static TexEntry g_tex_cache[TEX_CACHE];
static uint32_t g_tex_gen = 1;

/* Something wrote guest memory the runtime did not draw itself (a block
   transfer, a CPU copy, the movie decoder).  The frontend uses this to notice
   that the frame buffer it is showing has new pixels even when the guest did
   not call sceDisplaySetFrameBuf again (the PSMF movie writes into the buffer
   it already presented). */
volatile uint32_t g_gpu_write_tick = 1;

// Drop the cached textures whose source lies in [lo, hi).  A full wipe is
// only needed when the range is unknown.
void gpu_tex_invalidate_range(uint32_t lo, uint32_t hi) {
    g_gpu_write_tick++;
    g_ge_pix_gen++;   // a cached pixel state may describe one of these textures
    for (int k = 0; k < TEX_CACHE; k++) {
        TexEntry *e = &g_tex_cache[k];
        if (!e->pix && !e->gx_only) continue;
        uint32_t a = e->addr & 0x1FFFFFFFu, b = a + e->bytes;
        if (a < hi && lo < b) e->stale = 1;  // re-validate before reusing
    }
}

#ifdef HLE_TRACE
uint64_t g_tex_dec_conflict, g_tex_dec_changed, g_tex_dec_new, g_tex_flushes;
#endif
/* TEXFLUSH.  The game issues about 1500 a second, so this only moves the
   generation on: every entry from an older generation is re-validated on
   its next use (marking all 512 entries stale here was redundant). */
void gpu_tex_invalidate(void) {
#ifdef HLE_TRACE
    g_tex_flushes++;
#endif
    g_tex_gen++;
}

/* A change detector for texture and surface memory, not a cryptographic
   hash.  It runs over whole textures every time the guest flushes the
   texture cache, so it is written for the 32-bit Broadway: two independent
   32-bit lanes (multiply, rotate), where the old 64-bit FNV step cost a
   handful of multiplies and a 64-bit shift per eight bytes. */
#ifdef HLE_TRACE
uint64_t g_hash_bytes, g_hash_us;
#endif
static uint64_t hash_bytes(const uint8_t *p, size_t n) {
#ifdef HLE_TRACE
    uint64_t t0 = hle_time_us();
    g_hash_bytes += n;
#endif
    /* Three running sums of the words, the second of the first and the third
       of the second (a Fletcher sum of the third order): any change to the
       data moves all three, by an amount that depends on where the word is,
       and a word costs three additions.  Multiplications, which the mixing
       hash used here before needed two of for every eight bytes, are what a
       texture check spent its time on. */
    uint32_t s1 = 0x811C9DC5u ^ (uint32_t)n, s2 = 0, s3 = 0;
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        uint32_t v0, v1, v2, v3;
        memcpy(&v0, p + i, 4);
        memcpy(&v1, p + i + 4, 4);
        memcpy(&v2, p + i + 8, 4);
        memcpy(&v3, p + i + 12, 4);
        s1 += v0; s2 += s1; s3 += s2;
        s1 += v1; s2 += s1; s3 += s2;
        s1 += v2; s2 += s1; s3 += s2;
        s1 += v3; s2 += s1; s3 += s2;
    }
    for (; i + 4 <= n; i += 4) {
        uint32_t v;
        memcpy(&v, p + i, 4);
        s1 += v; s2 += s1; s3 += s2;
    }
    for (; i < n; i++) { s1 += p[i]; s2 += s1; s3 += s2; }
    /* Fold the sums so that either half depends on every word: the CLUT
       check keeps only the low 32 bits. */
    uint32_t lo = s1 ^ (s2 << 11 | s2 >> 21) ^ s3, hi = s3 ^ (s1 << 7 | s1 >> 25) ^ s2 * 0x9E3779B1u;
#ifdef HLE_TRACE
    g_hash_us += hle_time_us() - t0;
#endif
    return (uint64_t)hi << 32 | lo;
}

static uint32_t tex_bytes(const TexState *t) {
    static const int bpp[16] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0};
    uint32_t rows = (uint32_t)t->h, stride = (uint32_t)(t->stride < t->w ? t->w : t->stride);
    if (t->fmt >= 8) rows = (rows + 3) / 4 * 4;
    /* A swizzled texture is stored in blocks of 8 rows, so a shorter one
       still spans a whole block: texel_addr() reaches into it, and the bulk
       decoder and the change hash must cover it too. */
    else if (t->swizzle) rows = (rows + 7) & ~7u;
    return rows * stride * (uint32_t)bpp[t->fmt] / 8;
}

void raster_flush(void);

// Framebuffer ranges that queued (not yet drawn) primitives write to. A
// texture only needs the queue drained if it reads from one of them.
#define MAX_PENDING_TARGETS 8
static struct { uint32_t lo, hi; } g_pending_targets[MAX_PENDING_TARGETS];
static int g_n_pending_targets;

static void note_target(uint32_t lo, uint32_t hi) {
    lo &= 0x1FFFFFFF;
    hi &= 0x1FFFFFFF;
    for (int k = 0; k < g_n_pending_targets; k++)
        if (g_pending_targets[k].lo == lo && g_pending_targets[k].hi == hi) return;
    if (g_n_pending_targets == MAX_PENDING_TARGETS) {
        g_flush_reason[3]++;
        raster_flush();
    }
    g_pending_targets[g_n_pending_targets].lo = lo;
    g_pending_targets[g_n_pending_targets].hi = hi;
    g_n_pending_targets++;
}

static void flush_if_rendering_to(uint32_t lo, uint32_t hi) {
    lo &= 0x1FFFFFFF;
    hi &= 0x1FFFFFFF;
    for (int k = 0; k < g_n_pending_targets; k++)
        if (lo < g_pending_targets[k].hi && g_pending_targets[k].lo < hi) {
            g_flush_reason[1]++;
            raster_flush();
            return;
        }
}

// Texel buffers replaced while primitives using them may still be queued;
// freed once the queue has drained (raster_flush).
#define MAX_RETIRED 256
static void *g_retired[MAX_RETIRED];
static int g_nretired;
static size_t g_retired_bytes;

static void tex_free_retired(void) {
    for (int k = 0; k < g_nretired; k++) big_free(g_retired[k]);
    g_nretired = 0;
    g_retired_bytes = 0;
}

static void tex_retire(void *pix, int texels) {
    if (!pix) return;
    if (g_nretired == MAX_RETIRED || g_retired_bytes > (64u << 20)) raster_flush();  // frees them
    g_retired[g_nretired++] = pix;
    g_retired_bytes += (size_t)texels * 4;
}

static uint32_t g_tex_serial, g_tex_clock;

/* Hand a decoded texture back.  With no worker threads nothing queued can
   still be sampling it, so it is freed on the spot; otherwise it waits for
   the queue to drain. */
static int g_nworkers;   // raster worker threads (0: everything is drawn inline)
static void tex_release(void *pix, int texels) {
    if (!pix) return;
    if (!g_nworkers) big_free(pix);
    else tex_retire(pix, texels);
}

/* Give an entry's texels back.  On the Wii the chip's converted copy of
   them goes too: it is keyed by the serial, which no later decode reuses,
   so it would otherwise sit in the shared pool until its slot was needed. */
static void tex_drop(TexEntry *e) {
#ifdef __wii__
    if (e->pix || e->gx_only) wii_gxscene_tex_forget(e->serial);
#endif
    tex_release(e->pix, e->cap);
    e->pix = NULL;
    e->cap = 0;
    e->gx_only = 0;
}

/* The least recently used entry holding texels, other than keep. */
static TexEntry *tex_lru(const void *keep) {
    TexEntry *victim = NULL;
    for (int k = 0; k < TEX_CACHE; k++) {
        TexEntry *e = &g_tex_cache[k];
        if (e->pix && e->pix != keep && (!victim || e->used < victim->used)) victim = e;
    }
    return victim;
}

int gpu_tex_evict_lru(const void *keep) {
    TexEntry *victim = tex_lru(keep);
    if (!victim) return 0;
    tex_drop(victim);
    return 1;
}

#ifdef __wii__
/* Every texture the chip draws is held twice: decoded here, and converted
   in the chip's own layout.  When the pool runs short, the decoded copy of a
   texture the chip still holds is the first thing to give up (oldest
   first, never keep): the entry stays valid on the chip's copy, and only a
   draw that needs the texels again decodes them again.  A fight and the
   cards before it did not fit twice, and then either the chip's copies were
   converted again and again or a texture went missing. */
int gpu_tex_drop_copy(const void *keep) {
    TexEntry *victim = NULL;
    for (int k = 0; k < TEX_CACHE; k++) {
        TexEntry *e = &g_tex_cache[k];
        if (e->pix && e->pix != keep && (!victim || e->used < victim->used) &&
            wii_gxscene_tex_has(e->serial))
            victim = e;
    }
    if (!victim) return 0;
    tex_release(victim->pix, victim->cap);
    victim->pix = NULL;
    victim->cap = 0;
    victim->gx_only = 1;
#ifdef HLE_TRACE
    g_tex_copy_drops++;
#endif
    return 1;
}
#endif

/* Memory for a decoded texture.  The pool is finite, so when it is full the
   decoded copies the chip no longer needs go first, then the least recently
   used cache entries, until the request fits; NULL means nothing is left to
   give up.  The pool is shared with the chip's converted textures: once no
   decoded texture is left to free, those are released. */
static uint32_t *tex_alloc(size_t bytes) {
#ifdef __wii__
    int trimmed = 0;
#endif
    for (;;) {
        uint32_t *p = big_alloc(bytes);
        if (p) return p;
#ifdef __wii__
        if (gpu_tex_drop_copy(NULL)) continue;
#endif
        TexEntry *victim = tex_lru(NULL);
        if (!victim) {
#ifdef __wii__
            if (!trimmed++ && wii_gxscene_tex_trim()) continue;
#endif
            return NULL;
        }
        tex_drop(victim);
    }
}

/* A primitive that reuses the cached pixel state (draw_prim) skips the
   lookup, so it refreshes the entry's age here: otherwise a texture drawn
   every frame looked unused and was the first to be evicted. */
static void tex_touch(void *entry) {
    if (entry) ((TexEntry *)entry)->used = ++g_tex_clock;
}

static void tex_use(TexState *t, TexEntry *e) {
    t->entry = e;
    e->used = ++g_tex_clock;
    t->pix = e->pix;
    t->serial = e->serial;
    t->pw = e->pw;
    t->ph = e->ph;
    t->psx = (float)e->pw / (float)t->w;
    t->psy = (float)e->ph / (float)t->h;
    t->native_ok = e->native_ok;
    t->gx_only = e->gx_only;
}

static void glr_note_fb_access(const char *what, uint32_t lo, uint32_t hi);

#ifdef EXP_TEXCHK
/* Every 4-bit paletted texture the lookup hands out is decoded again from
   the guest's memory and palette and compared with the cached texels (the
   cache keys such a texture by 16 palette entries, not the whole palette). */
static uint32_t g_texchk_n, g_texchk_bad;
static void tex_check(const TexState *t) {
    static uint32_t every;
    if (!t->enabled || t->fmt != 4 || !t->pix || !t->native_ok || t->pw != t->w || t->ph != t->h) return;
    if ((every++ & 7) != 0) return;   /* a stale entry is handed out again and again: one lookup in eight is enough */
    uint32_t bytes = tex_bytes(t);
    const uint8_t *src = bytes ? guest_host_ptr(t->addr, bytes) : NULL;
    if (!src) return;
    g_texchk_n++;
    /* A sample of the texels (the full check made loading crawl): every 8th, 16th for a large texture,
       the column shifted from row to row. */
    const int step = t->w * t->h > 4096 ? 16 : 8;
    for (int y = 0; y < t->h; y++)
        for (int x = (y * 5) % step; x < t->w; x += step) {
            uint8_t b = src[texel_addr(t, x, y, 4) - t->addr];
            b = (x & 1) ? b >> 4 : b & 15;
            const uint32_t want = clut_lookup(t, b);
            if (t->pix[y * t->w + x] != want) {
                if (++g_texchk_bad <= 8)
                    fprintf(stderr, "TEXCHK BAD addr=%08X %dx%d at %d,%d have %08X want %08X (clut off %d mask %X shift %d fmt %d) after %u checked\n",
                            t->addr, t->w, t->h, x, y, t->pix[y * t->w + x], want, t->clut_off, t->clut_mask, t->clut_shift, t->clut_fmt, g_texchk_n);
                return;
            }
        }
    if ((g_texchk_n & 0xFF) == 1) fprintf(stderr, "TEXCHK %u textures checked, %u bad\n", g_texchk_n, g_texchk_bad);
}
#define TEXCHK(t_) tex_check(t_)
#else
#define TEXCHK(t_) ((void)0)
#endif

/* chip: the primitive is drawn by the GX, which can use its own copy of a
   texture whose decoded texels were given back; 2 if it is also the blit of
   a display buffer, whose texels are not used. */
static void tex_cache_lookup(TexState *t, int chip) {
#ifdef TKDR_FINEPROF
    DP_T(tc0);
#endif
#if defined(__wii__) && !defined(EXP_OLDBLIT)
    if (chip == 2) {
        /* The blit of a presented display buffer: the chip answers it with a copy of its own picture (wii_gxscene.c,
           bind_display_copy) and never looks at the texels.  The guest's memory does not hold the picture either (the chip
           never writes it), so there is nothing to hash or decode: that was a 512 KB hash and a 262,144-texel decode
           (~27 ms) every time a block transfer touched the buffer, and a scene that blits a buffer once a frame ran at
           24 frames a second.  A new serial a lookup makes the chip copy the picture again, as a new decode did. */
        t->pix = NULL;
        t->entry = NULL;
        t->gx_only = 0;
        t->native_ok = 0;
        t->blit = 1;
        t->pw = t->w; t->ph = t->h;
        t->psx = t->psy = 1.0f;
        t->serial = ++g_tex_serial;
        return;
    }
#endif
    uint32_t key_fmt = (uint32_t)t->fmt | (uint32_t)t->swizzle << 8 | (uint32_t)t->stride << 16;
    uint32_t key_size = (uint32_t)t->w | (uint32_t)t->h << 16;
    uint32_t key_clut = (uint32_t)t->clut_fmt | (uint32_t)t->clut_shift << 4 | (uint32_t)t->clut_mask << 12 |
                        (uint32_t)t->clut_off << 20;
    uint32_t clut_hash = 0;
    if (t->fmt >= 4 && t->fmt <= 7) {
        /* The CLUT is hashed so that a changed palette invalidates the
           cached texture.  It hardly ever changes, so hashing it on every
           textured primitive (119k times a run) was pure waste.  Hash it
           when the CLUT has been written to since the last time. */
#ifdef HLE_TRACE
        uint64_t th0 = hle_time_us();
#endif
#ifndef EXP_FULLCLUTHASH
        if (t->fmt == 4) {
            /* A 4-bit index reaches 16 palette entries from the offset (the
               offset is a multiple of 16, so the index can only add to it):
               the other 240 cannot change what this texture looks like. */
            clut_hash = t->clut_fmt == 3
                ? (uint32_t)hash_bytes((const uint8_t *)g_ge.clut + (t->clut_off & 255) * 4, 16 * 4)
                : (uint32_t)hash_bytes((const uint8_t *)g_ge.clut + (t->clut_off & 511) * 2, 16 * 2);
        } else
#endif
        {
            if (g_clut_dirty) {
                g_clut_hash = (uint32_t)hash_bytes((const uint8_t *)g_ge.clut, sizeof g_ge.clut);
                g_clut_dirty = 0;
            }
            clut_hash = g_clut_hash;
        }
#ifdef HLE_TRACE
        g_clut_hash_us += hle_time_us() - th0;
        g_clut_hash_calls++;
#endif
    }
    uint32_t set = (t->addr * 2654435761u ^ key_fmt ^ key_size * 31u ^
                    clut_hash * 17u) % (TEX_CACHE / TEX_WAYS);
    TexEntry *ways = &g_tex_cache[set * TEX_WAYS], *e = NULL;
    int epoch = texrep_epoch();
    for (int i = 0; i < TEX_WAYS && !e; i++) {
        TexEntry *c = &ways[i];
        if ((c->pix || c->gx_only) && c->addr == t->addr && c->key_fmt == key_fmt && c->key_size == key_size &&
            c->key_clut == key_clut && c->clut_hash == clut_hash && c->epoch == epoch)
            e = c;
    }
    int same_key = e != NULL;
#ifdef __wii__
    /* Only the chip's copy is left: usable if the chip draws this and still
       has it, decoded again otherwise. */
    if (same_key && e->gx_only && !(chip && wii_gxscene_tex_has(e->serial))) same_key = 0;
#else
    (void)chip;
#endif
    if (chip == 2 && same_key && !e->stale) {   /* the blit of a display buffer: no hash of its 512 KB each frame */
        e->gen = g_tex_gen;
        tex_use(t, e);
        return;
    }
    if (!e) {  // the entry to fill: a free way, or the least recently used one
        for (int i = 0; i < TEX_WAYS; i++) {
            TexEntry *c = &ways[i];
            if (!c->pix && !c->gx_only) { e = c; break; }
            if (!e || c->used < e->used) e = c;
        }
    }
    // A cached texture stays valid until something writes its source
    // memory.  A clean entry is reused straight away; a stale one is
    // re-validated by hashing its source, and only re-decoded when the
    // bytes really changed.  (Comparing the generation here meant every
    // invalidated texture was re-hashed and re-resolved on *every*
    // primitive -- with 250k primitives that dominated the run.)
    //
    // A TEXFLUSH moves the generation on, and the game flushes on nearly
    // every texture change, so the bytes are checked at most once per
    // display list: the guest prepares its textures between lists.  A
    // write the runtime sees (a block transfer, a DMA copy) marks the entry
    // stale, which always re-checks it.
    if (same_key && !e->stale && (e->gen == g_tex_gen || e->checked_list == g_tex_list)) {
        e->gen = g_tex_gen;
        tex_use(t, e);
        TEXCHK(t);
#ifdef TKDR_FINEPROF
        { DP_T(tc1); g_tl_part[0] += (uint32_t)(tc1 - tc0); g_tl_partn[0]++; }
#endif
        return;
    }
#ifdef HLE_TRACE
    {   // report each distinct texture once
        static uint32_t seen[16];
        static int nseen;
        for (int i = 0; i < nseen; i++)
            if (seen[i] == (t->w | (t->h << 16))) goto reported;
        if (nseen < 16) seen[nseen++] = t->w | (t->h << 16);
        fprintf(stderr, "texture: %dx%d fmt=%d linear=%d clamp=%d/%d swap=%d/%d\n",
                t->w, t->h, t->fmt, t->linear, t->clamp_u, t->clamp_v,
                t->swizzle, t->swizzle);
reported:;
    }
#endif
#ifdef TKDR_FINEPROF
    DP_T(tp0);
    g_tl_part[1] += (uint32_t)(tp0 - tc0); g_tl_partn[1]++;
#endif
    uint32_t n = tex_bytes(t);
    if (g_glr_active) glr_note_fb_access("texture", t->addr, t->addr + n);
    flush_if_rendering_to(t->addr, t->addr + n);  // the source may still be being drawn
#ifdef TKDR_FINEPROF
    DP_T(tp1);
    g_tp_ticks[0] += (uint32_t)(tp1 - tp0);
#endif
    surf_resolve_range(t->addr, t->addr + n);     // or only exist at the internal resolution
#ifdef TKDR_FINEPROF
    {
        DP_T(tp2);
        g_tp_ticks[1] += (uint32_t)(tp2 - tp1);
    }
#endif
    /* The source as one host range.  This used to test the address against
       GUEST_MEM_SIZE, which on the Wii is the RAM window's size rather than
       an address limit, so the hash was always 0 and a texture rewritten in
       place (same address, format and palette) was never decoded again. */
    uint32_t span = guest_span(t->addr, n);
    const uint8_t *src = span ? guest_host_ptr(t->addr, span) : NULL;
#ifdef TKDR_FINEPROF
    DP_T(th0);
#endif
    uint64_t h = src ? hash_bytes(src, span) : 0;
#ifdef TKDR_FINEPROF
    DP_T(th1);
    g_th_ticks += (uint32_t)(th1 - th0); g_th_n++; g_th_bytes += span;
#endif
    if (same_key && e->data_hash == h) {
#ifdef TKDR_FINEPROF
        g_ts_same++;
        { DP_T(tc2); g_tl_part[2] += (uint32_t)(tc2 - th1); g_tl_part[3] += (uint32_t)(th0 - tp0); }
#endif
        e->gen = g_tex_gen;
        e->stale = 0;
        e->checked_list = g_tex_list;
        tex_use(t, e);
        TEXCHK(t);
        return;
    }
#ifdef TKDR_FINEPROF
    DP_T(td0);
    {
        int k = 0;
        while (k < 15 && g_td_top[k].n && (g_td_top[k].addr != t->addr || g_td_top[k].wh != (t->w | (t->h << 16)))) k++;
        g_td_top[k].addr = t->addr; g_td_top[k].fmt = (uint32_t)t->fmt | (same_key ? 0x100u : 0) | (e->pix ? 0x200u : 0);
        g_td_top[k].wh = t->w | (t->h << 16); g_td_top[k].n++;
    }
#endif
#ifdef HLE_TRACE
    /* Why this texture is decoded: its slot held another texture, its
       bytes changed, or it is new. */
    if (e->pix && !same_key) g_tex_dec_conflict++;
    else if (e->pix) g_tex_dec_changed++;
    else g_tex_dec_new++;
#endif
    // queued primitives may still sample the old pixels: never reuse them in place
    tex_drop(e);
    uint32_t *fresh = tex_alloc((size_t)t->w * (size_t)t->h * 4);
    if (!fresh) {
#if defined(HLE_TRACE) && defined(__wii__)
        extern uint32_t wii_big_used(void);
        fprintf(stderr, "TEXALLOC FAIL %dx%d pool_used=%u\n", t->w, t->h, wii_big_used());
#endif
        /* No memory for the texels: draw this primitive without the
           texture rather than dereference nothing. */
        t->enabled = 0;
        t->pix = NULL;
        return;
    }
    e->pix = fresh;
    e->cap = t->w * t->h;
    e->native_ok = 1;
    t->pix = NULL;  // fetch() decodes from guest memory
#ifdef HLE_TRACE
    uint64_t tdec = hle_time_us();
#endif
    int fast_decoded = tex_decode_fast(t, e->pix);
    if (!fast_decoded) {
        for (int y = 0; y < t->h; y++)
            for (int x = 0; x < t->w; x++) e->pix[y * t->w + x] = fetch(t, x, y);
    }
#ifdef HLE_TRACE
    if (fast_decoded) {   /* sample-check the bulk decoder against fetch() */
        static int checked;
        for (int k = 0; k < 16 && checked < 256; k++, checked++) {
            int x = (int)((uint32_t)k * 2654435761u % (uint32_t)t->w);
            int y = (int)((uint32_t)k * 2246822519u % (uint32_t)t->h);
            uint32_t want = fetch(t, x, y);
            if (e->pix[(size_t)y * (size_t)t->w + (size_t)x] != want) {
                fprintf(stderr, "TEXTURE FAST MISMATCH fmt=%d %dx%d at %d,%d\n", t->fmt, t->w, t->h, x, y);
                break;
            }
        }
    }
    g_tex_decodes++;
    g_tex_decode_texels += (uint64_t)t->w * t->h;
    g_tex_decode_us += hle_time_us() - tdec;
#endif
    e->pw = t->w;
    e->ph = t->h;
    int rw, rh;
    uint32_t *rep = texrep_lookup(t->addr, t->fmt, t->w, t->h, R(GE_TEXBUFWIDTH0), R(GE_TEXSIZE0),
                                  R(GE_CLUTFORMAT), e->pix, &rw, &rh);
    if (rep) {
        big_free(e->pix);
        e->pix = rep;
        e->pw = rw;
        e->ph = rh;
        e->cap = rw * rh;
        e->native_ok = 0;   // a texture-pack replacement is already RGBA
    }
#ifdef TKDR_FINEPROF
    {
        DP_T(td1);
        g_td_ticks += (uint32_t)(td1 - td0); g_td_n++; g_td_texels += (uint32_t)(t->w * t->h);
    }
#endif
    e->stale = 0;
    e->addr = t->addr; e->key_fmt = key_fmt; e->key_size = key_size; e->key_clut = key_clut;
    e->clut_hash = clut_hash; e->gen = g_tex_gen; e->data_hash = h; e->epoch = epoch;
    e->checked_list = g_tex_list;
    e->bytes = n;
    e->serial = ++g_tex_serial;
#ifdef TKDR_AUTOPILOT
    {
        extern uint32_t g_frames_presented;
        uint32_t nz = 0;
        if (src) for (uint32_t i = 0; i < span; i++) nz += src[i] != 0;
        e->nz0 = nz;
        e->list0 = g_tex_list;
        e->frame0 = g_frames_presented;
    }
#endif
    tex_use(t, e);
    TEXCHK(t);
}

#ifdef TKDR_AUTOPILOT
/* Debug: what the texture cache knows about the texture of one serial, and what its source in guest memory holds
   now (the black 3D shows two 512x512 textures that decoded to nothing: was the source empty, or is the entry stale?). */
void gpu_tex_probe(uint32_t serial) {
    extern uint32_t g_frames_presented;
    for (int k = 0; k < TEX_CACHE; k++) {
        const TexEntry *e = &g_tex_cache[k];
        if (e->serial != serial || !(e->pix || e->gx_only)) continue;
        const uint32_t span = guest_span(e->addr, e->bytes);
        const uint8_t *src = span ? guest_host_ptr(e->addr, span) : NULL;
        uint32_t nz = 0, cnz = 0, pnz = 0;
        if (src) for (uint32_t i = 0; i < span; i++) nz += src[i] != 0;
        const uint8_t *cl = (const uint8_t *)g_ge.clut;
        for (uint32_t i = 0; i < sizeof g_ge.clut; i++) cnz += cl[i] != 0;
        if (e->pix) for (int i = 0; i < e->pw * e->ph; i++) pnz += e->pix[i] != 0;
        const uint64_t now = src ? hash_bytes(src, span) : 0;
        fprintf(stderr, "  TEXENTRY serial %u: addr %08X fmt/swz/stride %06X size %08X clut key %08X hash %08X, %u source bytes: %u nonzero now, hash %s, "
                "stale %d, gen %u (now %u), checked in list %u (now %u), %s, %u nonzero texels; the palette now holds %u nonzero bytes; "
                "decoded in list %u, frame %u (now %u) from %u nonzero source bytes\n",
                serial, e->addr, e->key_fmt, e->key_size, e->key_clut, e->clut_hash, e->bytes, nz, now == e->data_hash ? "unchanged" : "CHANGED",
                e->stale, e->gen, g_tex_gen, e->checked_list, g_tex_list, e->pix ? "decoded copy kept" : "GX copy only", pnz, cnz,
                e->list0, e->frame0, g_frames_presented, e->nz0);
        return;
    }
    fprintf(stderr, "  TEXENTRY serial %u: not in the cache\n", serial);
}
#endif

// a + (b - a) * w / 256 on each byte, w in 0..256
static inline uint32_t lerp8(uint32_t a, uint32_t b, uint32_t w) {
    uint32_t iw = 256 - w;
    uint32_t rb = (((a & 0x00FF00FFu) * iw + (b & 0x00FF00FFu) * w + 0x00800080u) >> 8) & 0x00FF00FFu;
    uint32_t ag = ((a >> 8 & 0x00FF00FFu) * iw + (b >> 8 & 0x00FF00FFu) * w + 0x00800080u) & 0xFF00FF00u;
    return rb | ag;
}

// u, v in texels
static inline __attribute__((always_inline)) void sample(const TexState *t, float u, float v, float *out) {
    const uint32_t *p = t->pix;
    int w = t->pw, h = t->ph;
    if (w != t->w) u *= t->psx;
    if (h != t->h) v *= t->psy;
    if (!t->linear) {
#ifdef HLE_TRACE
        g_sample_point++;
#endif
        int x = wrap(fast_floor(u), w, t->clamp_u), y = wrap(fast_floor(v), h, t->clamp_v);
        uint32_t c = p[y * w + x];
        out[0] = (float)(c & 0xFF); out[1] = (float)(c >> 8 & 0xFF);
        out[2] = (float)(c >> 16 & 0xFF); out[3] = (float)(c >> 24);
        return;
    }
#ifdef HLE_TRACE
    g_sample_linear++;
#endif
    u -= 0.5f;
    v -= 0.5f;
    int x0 = fast_floor(u), y0 = fast_floor(v);
    float fx = u - (float)x0, fy = v - (float)y0;
    int xa = wrap(x0, w, t->clamp_u), xb = wrap(x0 + 1, w, t->clamp_u);
    int ya = wrap(y0, h, t->clamp_v) * w, yb = wrap(y0 + 1, h, t->clamp_v) * w;
    // fixed point, two channels per 32-bit lane (weights out of 256)
    uint32_t wx = (uint32_t)(fx * 256.f), wy = (uint32_t)(fy * 256.f);
    uint32_t top = lerp8(p[ya + xa], p[ya + xb], wx), bot = lerp8(p[yb + xa], p[yb + xb], wx);
    uint32_t c = lerp8(top, bot, wy);
    out[0] = (float)(c & 0xFF); out[1] = (float)(c >> 8 & 0xFF);
    out[2] = (float)(c >> 16 & 0xFF); out[3] = (float)(c >> 24);
}

// ---------------------------------------------------------------- per-pixel

typedef struct {
    Target tg;
    TexState tex;
    int clear, clear_color, clear_alpha, clear_depth;
    int alpha_test, at_func, at_ref, at_mask;
    int color_test, ct_func;
    uint32_t ct_ref, ct_mask;
    int stencil_test, st_func, st_ref, st_mask;
    int st_sfail, st_zfail, st_zpass;
    int logic_test, logic_op;
    int dither;
    int fog_enable;
    float fog_color[3];
    int z_test, z_func, z_write;
    int blend, bl_src, bl_dst, bl_eq;
    uint32_t fix_a, fix_b, write_mask;
    int through;
} PixState;

static void pix_state(PixState *p, int through) {
    target(&p->tg);
    tex_state(&p->tex);
#ifdef __wii__
#ifdef TKDR_FINEPROF
    DP_T(tl0);
#endif
    if (p->tex.enabled) {
        /* 2: the full-screen blit of one of the guest's own display buffers.  The chip answers it with a copy of the picture so far
           (wii_gxscene.c, bind_display_copy) and never looks at the texels, which it does not write either, so they are not checked. */
        const int chip = g_gxr_enabled && gpu_is_display_target(p->tg.fb);
        const int blit = chip && through && p->tex.fmt <= 3 && p->tex.w >= 480 && p->tex.h >= 272 && gpu_is_presented_buffer(p->tex.addr);
        tex_cache_lookup(&p->tex, chip + blit);
    }
#ifdef TKDR_FINEPROF
    {
        DP_T(tl1);
        g_tl_ticks += (uint32_t)(tl1 - tl0); g_tl_n++;
    }
#endif
#else
    if (p->tex.enabled) tex_cache_lookup(&p->tex, 0);
#endif
    p->through = through;
    uint32_t cm = R(GE_CLEARMODE);
    int uses_depth = (R(GE_ZTESTENABLE) & 1) || ((cm & 1) && (cm >> 10 & 1));
    target_scale(&p->tg, uses_depth);
    p->clear = cm & 1;
    p->clear_color = cm >> 8 & 1;
    p->clear_alpha = cm >> 9 & 1;
    p->clear_depth = cm >> 10 & 1;
    p->alpha_test = R(GE_ALPHATESTENABLE) & 1;
    p->at_func = (int)(R(GE_ALPHATEST) & 7);
    p->at_ref = (int)(R(GE_ALPHATEST) >> 8 & 0xFF);
    p->at_mask = (int)(R(GE_ALPHATEST) >> 16 & 0xFF);
    /* The GE colour test compares the post-fog source colour with a masked
       reference; it is a source test, not a framebuffer read.  The function
       is only two bits (never, always, equal, not equal). */
    p->color_test = R(GE_COLORTESTENABLE) & 1;
    p->ct_func = (int)(R(GE_COLORTEST) & 3);
    p->ct_ref = R(GE_COLORREF) & 0xFFFFFFu;
    p->ct_mask = R(GE_COLORTESTMASK) & 0xFFFFFFu;
    p->stencil_test = R(GE_STENCILTESTENABLE) & 1;
    p->st_func = (int)(R(GE_STENCILTEST) & 7);
    p->st_ref = (int)(R(GE_STENCILTEST) >> 8 & 0xFF);
    p->st_mask = (int)(R(GE_STENCILTEST) >> 16 & 0xFF);
    p->st_sfail = (int)(R(GE_STENCILOP) & 7);
    p->st_zfail = (int)(R(GE_STENCILOP) >> 8 & 7);
    p->st_zpass = (int)(R(GE_STENCILOP) >> 16 & 7);
    p->logic_test = R(GE_LOGICOPENABLE) & 1;
    p->logic_op = (int)(R(GE_LOGICOP) & 0xF);
    p->dither = R(GE_DITHERENABLE) & 1;
    /* Fog is a vertex-stage effect in the GE: it is not applied in through
       mode, and the colour is mixed after texturing. */
    p->fog_enable = (R(GE_FOGENABLE) & 1) && !through;
    p->fog_color[0] = (float)(R(GE_FOGCOLOR) & 0xFF);
    p->fog_color[1] = (float)(R(GE_FOGCOLOR) >> 8 & 0xFF);
    p->fog_color[2] = (float)(R(GE_FOGCOLOR) >> 16 & 0xFF);
    p->z_test = R(GE_ZTESTENABLE) & 1;
    p->z_func = (int)(R(GE_ZTEST) & 7);
    p->z_write = !(R(GE_ZWRITEDISABLE) & 1);
    p->blend = R(GE_ALPHABLENDENABLE) & 1;
    p->bl_src = (int)(R(GE_BLENDMODE) & 0xF);
    p->bl_dst = (int)(R(GE_BLENDMODE) >> 4 & 0xF);
    p->bl_eq = (int)(R(GE_BLENDMODE) >> 8 & 7);
#ifdef HLE_TRACE
    {   // which GE features does the game actually use?
        if (p->tex.enabled) {
            if (p->tex.func < 8) g_feat_texfunc[p->tex.func]++;
            if (p->tex.fmt < 16) g_feat_texfmt[p->tex.fmt]++;
            if (p->tex.rgba) g_feat_rgba++;
            if (p->tex.dbl) g_feat_dbl++;
        } else {
            g_feat_no_tex++;
        }
        if (p->blend) {
            g_feat_blend_on++;
            if (p->bl_eq < 8) g_feat_blend[p->bl_eq]++;
            if (p->bl_src < 16) g_feat_blsrc[p->bl_src]++;
            if (p->bl_dst < 16) g_feat_bldst[p->bl_dst]++;
        }
        if (p->alpha_test) { g_feat_alpha_test++; if (p->at_func < 8) g_feat_atfunc[p->at_func]++; }
        if (p->z_test) { g_feat_z_test++; if (p->z_func < 8) g_feat_zfunc[p->z_func]++; }
        if (p->z_write) g_feat_z_write++;
        if (p->through) g_feat_through++;
    }
#endif
    p->fix_a = R(GE_BLENDFIXEDA);
    p->fix_b = R(GE_BLENDFIXEDB);
    p->write_mask = (R(GE_MASKRGB) & 0xFFFFFF) | (R(GE_MASKALPHA) & 0xFF) << 24;
}

static inline __attribute__((always_inline)) int cmp(int func, int a, int b) {
    switch (func) {
    case 0: return 0;
    case 1: return 1;
    case 2: return a == b;
    case 3: return a != b;
    case 4: return a < b;
    case 5: return a <= b;
    case 6: return a > b;
    default: return a >= b;
    }
}

// All three channels of a blend factor at once.
static inline __attribute__((always_inline)) void blend_factors(int f, int is_src, const float *s, const float *d, uint32_t fix, float *o) {
    const float k = 1.f * (1.f / 255.f);
    float v;
    switch (f) {
    case 0: for (int i = 0; i < 3; i++) o[i] = (is_src ? d[i] : s[i]) * k; return;
    case 1: for (int i = 0; i < 3; i++) o[i] = 1.f - (is_src ? d[i] : s[i]) * k; return;
    case 2: v = s[3] * k; break;
    case 3: v = 1.f - s[3] * k; break;
    case 4: v = d[3] * k; break;
    case 5: v = 1.f - d[3] * k; break;
    case 6: v = 2.f * s[3] * k; break;
    case 7: v = 1.f - 2.f * s[3] * k; break;
    case 8: v = 2.f * d[3] * k; break;
    case 9: v = 1.f - 2.f * d[3] * k; break;
    default: for (int i = 0; i < 3; i++) o[i] = (float)(fix >> (8 * i) & 0xFF) * k; return;
    }
    o[0] = o[1] = o[2] = v;
}

static float blend_factor(int f, int is_src, const float *s, const float *d, uint32_t fix, int ch) {
    float sa = s[3] / 255.f, da = d[3] / 255.f;
    switch (f) {
    case 0: return is_src ? d[ch] / 255.f : s[ch] / 255.f;
    case 1: return is_src ? 1.f - d[ch] / 255.f : 1.f - s[ch] / 255.f;
    case 2: return sa;
    case 3: return 1.f - sa;
    case 4: return da;
    case 5: return 1.f - da;
    case 6: return 2.f * sa;
    case 7: return 1.f - 2.f * sa;
    case 8: return 2.f * da;
    case 9: return 1.f - 2.f * da;
    default: return (float)(fix >> (8 * ch) & 0xFF) / 255.f;
    }
}

static inline float clamp255(float v) { return v < 0.f ? 0.f : v > 255.f ? 255.f : v; }

/* The GE keeps stencil in the framebuffer's alpha bits: eight bits in
   8888, the top nibble (expanded to 8 bits) in 4444, and the single alpha
   bit in 5551.  565 has no stencil. */
static inline int stencil_get(uint32_t d, uint32_t fmt) {
    if (fmt == 1) return (d >> 24) ? 0xFF : 0;
    if (fmt == 2) return (int)(d >> 24) & 0xFF;
    return (int)(d >> 24);
}

static inline int stencil_op(int op, int old, int ref, uint32_t fmt) {
    switch (op) {
    case 0: return old;                    // keep
    case 1: return 0;                      // zero
    case 2: return ref;                    // replace
    case 3: return (~old) & 0xFF;          // invert
    case 4:                                // increment (saturating)
        if (fmt == 1) return 0xFF;
        if (fmt == 2) return old < 0xF0 ? old + 0x10 : old;
        return old < 0xFF ? old + 1 : old;
    case 5:                                // decrement (saturating)
        if (fmt == 1) return 0;
        if (fmt == 2) return old >= 0x10 ? old - 0x10 : 0;
        return old > 0 ? old - 1 : 0;
    default: return old;
    }
}

/* GE logic operations.  They act on RGB after blending and leave the
   alpha/stencil channel alone. */
static inline uint32_t logic_apply(int op, uint32_t old, uint32_t src) {
    switch (op) {
    case 0: return src & 0xFF000000u;                         // clear
    case 1: return src & (old | 0xFF000000u);                // and
    case 2: return src & (~old | 0xFF000000u);               // and reverse
    case 3: return src;                                      // copy
    case 4: return (~src & (old & 0x00FFFFFFu)) | (src & 0xFF000000u); // and inverted
    case 5: return (old & 0x00FFFFFFu) | (src & 0xFF000000u); // noop
    case 6: return (src ^ (old & 0x00FFFFFFu)) | (src & 0xFF000000u);  // xor
    case 7: return (src | (old & 0x00FFFFFFu)) | (src & 0xFF000000u);  // or
    case 8: return (~(src | old) & 0x00FFFFFFu) | (src & 0xFF000000u); // nor
    case 9: return (~(src ^ old) & 0x00FFFFFFu) | (src & 0xFF000000u); // equiv
    case 10: return (~old & 0x00FFFFFFu) | (src & 0xFF000000u); // inverted
    case 11: return (src | (~old & 0x00FFFFFFu)) | (src & 0xFF000000u); // or reverse
    case 12: return ((~src & 0x00FFFFFFu) | (src & 0xFF000000u)); // copy inverted
    case 13: return ((~src | old) & 0x00FFFFFFu) | (src & 0xFF000000u); // or inverted
    case 14: return (~(src & old) & 0x00FFFFFFu) | (src & 0xFF000000u); // nand
    default: return src | 0x00FFFFFFu;                        // set
    }
}

// ---------------------------------------------------------------- parallel bands
// With RASTER_THREADS > 0, primitives are queued and drawn by worker threads.
// The screen is cut into strips of rows, interleaved over NBANDS bands; each
// band is an in-order stream over the queue that any idle worker can take, so
// faster cores simply draw more bands (Apple chips mix fast and slow cores).
#define NBANDS 32
static int g_nbands = 1;     // 1: no workers, everything drawn inline
uint64_t g_worker_busy_us[64], g_worker_px[64];  // stats (index: worker / band)
uint64_t hle_time_us(void);
#ifdef __wii__
static int tl_band;  /* no raster worker threads on the single-core Wii */
#else
static __thread int tl_band;
#endif
// `band` is read from tl_band once per primitive: thread-locals are slow on some platforms
static inline int owns_row(int y, int band) {
    return g_nbands == 1 || ((y >> g_band_shift) & (NBANDS - 1)) == band;
}
// Does this band own any row in [y0, y1]? Lets workers skip most small primitives before setup.
static inline int owns_any_row(int y0, int y1, int band) {
    if (g_nbands == 1) return 1;
    int b0 = y0 >> g_band_shift, b1 = y1 >> g_band_shift;
    if (b1 - b0 + 1 >= NBANDS) return 1;
    for (int b = b0; b <= b1; b++)
        if ((b & (NBANDS - 1)) == band) return 1;
    return 0;
}

// Pixel access: VRAM, or the scaled surfaces when the target has them.
//
// The framebuffer is read and written once per pixel, so it is accessed
// through a raw host pointer rather than mem_r32()/mem_w32(): those carry a
// scheduler tick and a byte swap that a pixel loop does not need.
static inline __attribute__((always_inline)) int mark_index(const Target *t, int x, int y) {
    if (t->mshift >= 0) return (y >> t->mshift) * (int)t->fb_stride + (x >> t->mshift);
    return (y / t->scale) * (int)t->fb_stride + x / t->scale;
}
static inline __attribute__((always_inline)) uint32_t px_read(const Target *t, int x, int y) {
    if (t->scol) return t->scol[y * t->sw + x];
    if (t->fbuf) {
        uint32_t off = (uint32_t)(y * (int)t->fb_stride + x) * (t->fb_fmt == 3 ? 4u : 2u);
        /* Read the pixel with an explicit load: a memcpy whose length is not a
           compile-time constant becomes a real call, and this runs per pixel. */
        if (t->fb_fmt == 3) {
            if (off + 4 <= t->fbuf_avail) {
                uint32_t v;
                memcpy(&v, t->fbuf + off, 4);
                return __builtin_bswap32(v);
            }
        } else if (off + 2 <= t->fbuf_avail) {
            uint16_t v;
            memcpy(&v, t->fbuf + off, 2);
            return fb_decode16(__builtin_bswap16(v), t->fb_fmt);
        }
    }
    return fb_read(t, x, y);
}
static inline __attribute__((always_inline)) void px_write(const Target *t, int x, int y, uint32_t c, uint32_t mask) {
    if (!t->scol) {
        fb_write(t, x, y, c, mask);
        return;
    }
    uint32_t *d = &t->scol[y * t->sw + x];
    if (mask) c = (c & ~mask) | (*d & mask);
    // store what VRAM would hold, so blending and readback match 1x
    if (t->fb_fmt != 3) c = fb_quantize(c, t->fb_fmt);
    *d = c;
    t->cmark[mark_index(t, x, y)] = 1;
}
static inline __attribute__((always_inline)) int z_read(const Target *t, uint32_t za, int x, int y) {
    if (t->sz) return t->sz[y * t->sw + x];
    uint32_t zoff = (uint32_t)(y * (int)t->zb_stride + x) * 2u;
    if (t->zbuf && zoff + 2 <= t->zbuf_avail) {
#ifdef HLE_TRACE
        g_z_fast++;
#endif
        const uint8_t *p = t->zbuf + zoff;
        uint16_t v;
        memcpy(&v, p, 2);
        return __builtin_bswap16(v);
    }
#ifdef HLE_TRACE
    g_z_slow++;
#endif
    return mem_r16(za);
}
static inline __attribute__((always_inline)) void z_write(const Target *t, uint32_t za, int x, int y, int z) {
    if (t->sz) {
        t->sz[y * t->sw + x] = (uint16_t)z;
        t->zmark[mark_index(t, x, y)] = 1;
    } else if (!t->scol) {
        /* One-to-one rendering: the depth buffer is the guest's own.  (A
           duplicated condition used to make this branch unreachable, so the
           depth buffer was never written at 1x and every depth test ran
           against whatever the buffer started with.) */
        uint32_t zoff = (uint32_t)(y * (int)t->zb_stride + x) * 2u;
        if (t->zbuf && zoff + 2 <= t->zbuf_avail) {
            uint8_t *p = t->zbuf + zoff;
            uint16_t v = __builtin_bswap16((uint16_t)z);
            memcpy(p, &v, 2);
        } else {
            mem_w16(za, (uint16_t)z);
        }
    }
}

// Shade one pixel. col: interpolated vertex color; uv in texels.
//
// Inlined into the pixel loop by default.  As a separate function it was
// called once per pixel, and each call set up a 472-byte stack frame and
// saved the registers the PowerPC ABI makes non-volatile -- 36 memory
// operations a pixel for the call alone.  The setup of a triangle is
// already a separate function, so inlining the shader here keeps the
// loop's register pressure manageable.
#ifdef PERF_NOINLINE
#define SHADE_INLINE
#else
#define SHADE_INLINE __attribute__((always_inline))
#endif
static inline SHADE_INLINE void shade(const PixState *p, int x, int y, float z,
                 float cr, float cg, float cb, float ca, float u, float v,
                 float fog, int z_passed) {
    const Target *t = &p->tg;
    if (x < t->sx1 || x > t->sx2 || y < t->sy1 || y > t->sy2 || x < 0 || y < 0) return;
    if (!t->scol && (x >= 1024 || y >= 1024)) return;
    uint32_t za = t->zb + (uint32_t)(y * (int)t->zb_stride + x) * 2;
    int zi = (int)(z < 0.f ? 0.f : z > 65535.f ? 65535.f : z);

    if (p->clear) {
        uint32_t c = (uint32_t)clamp255(cr) | (uint32_t)clamp255(cg) << 8 |
                     (uint32_t)clamp255(cb) << 16 | (uint32_t)clamp255(ca) << 24;
        uint32_t keep = (p->clear_color ? 0 : 0xFFFFFFu) | (p->clear_alpha ? 0 : 0xFF000000u);
        if (keep != 0xFFFFFFFFu) px_write(t, x, y, c, keep);
        if (p->clear_depth) z_write(t, za, x, y, zi);
        return;
    }

    // early depth test: skip texturing hidden pixels (alpha test must come first otherwise)
    int z_done = z_passed;
#if defined(PERF_NOZ) || defined(PERF_MIN)
    z_done = 1;
#endif
    if (p->z_test && !p->alpha_test && !z_done) {
        if (!cmp(p->z_func, zi, z_read(t, za, x, y))) return;
        z_done = 1;
    }

    float c[4] = {cr, cg, cb, ca};
    if (p->tex.enabled) {
#ifdef HLE_TRACE
        g_tex_px++;
#endif
        float tc[4];
#if defined(PERF_NOSAMPLE) || defined(PERF_MIN)
        tc[0] = tc[1] = tc[2] = tc[3] = 255.f;
#else
        sample(&p->tex, u, v, tc);
#endif
        const TexState *tx = &p->tex;
        switch (tx->func) {
        case 0:  // modulate
            for (int i = 0; i < 3; i++) c[i] = c[i] * tc[i] * (1.f / 255.f);
            c[3] = tx->rgba ? c[3] * tc[3] * (1.f / 255.f) : c[3];
            break;
        case 1:  // decal
            if (tx->rgba) for (int i = 0; i < 3; i++) c[i] = (tc[i] * tc[3] + c[i] * (255.f - tc[3])) * (1.f / 255.f);
            else for (int i = 0; i < 3; i++) c[i] = tc[i];
            break;
        case 2:  // blend with env color
            for (int i = 0; i < 3; i++) c[i] = (c[i] * (255.f - tc[i]) + tx->env[i] * tc[i]) * (1.f / 255.f);
            c[3] = tx->rgba ? c[3] * tc[3] * (1.f / 255.f) : c[3];
            break;
        case 3:  // replace
            for (int i = 0; i < 3; i++) c[i] = tc[i];
            if (tx->rgba) c[3] = tc[3];
            break;
        default:  // add
            for (int i = 0; i < 3; i++) c[i] = clamp255(c[i] + tc[i]);
            c[3] = tx->rgba ? c[3] * tc[3] * (1.f / 255.f) : c[3];
            break;
        }
        if (tx->dbl) for (int i = 0; i < 3; i++) c[i] = clamp255(c[i] * 2.f);
    }

    if (p->alpha_test) {
        int a = (int)clamp255(c[3]) & p->at_mask, ref = p->at_ref & p->at_mask;
        if (!cmp(p->at_func, a, ref)) return;
    }

    /* Fog comes before the colour test and the blend.  The GE keeps an
       eight-bit factor per vertex and interpolates it; the mixed colour is
       (src * f + fog * (255 - f) + 255) / 256, with the round-up the
       hardware uses. */
    if (p->fog_enable) {
        int f = (int)(fog + 0.5f);
        if (f < 0) f = 0;
        else if (f > 255) f = 255;
        for (int i = 0; i < 3; i++)
            c[i] = (clamp255(c[i]) * (float)f + p->fog_color[i] * (float)(255 - f) + 255.f) * (1.f / 256.f);
    }

    if (p->color_test) {
        uint32_t rgb = ((uint32_t)clamp255(c[0]) | (uint32_t)clamp255(c[1]) << 8 |
                        (uint32_t)clamp255(c[2]) << 16) & p->ct_mask;
        uint32_t ref = p->ct_ref & p->ct_mask;
        int pass;
        switch (p->ct_func) {
        case 0: pass = 0; break;          // never
        case 1: pass = 1; break;          // always
        case 2: pass = rgb == ref; break; // equal
        default: pass = rgb != ref; break;// not equal
        }
        if (!pass) return;
    }

    /* Stencil is stored in alpha.  The GE compares the reference against
       the buffer value (the opposite argument order from the alpha test),
       then applies the fail/z-fail/z-pass operation.  Only the stencil
       bits are written on a failure; the colour is untouched. */
    int stencil_en = p->stencil_test && t->fb_fmt != 0;
    uint32_t dst_px = 0;
    int have_dst = 0, stencil = 0;
    if (stencil_en) {
        dst_px = px_read(t, x, y);
        have_dst = 1;
        stencil = stencil_get(dst_px, t->fb_fmt);
        int sv = stencil & p->st_mask, sr = p->st_ref & p->st_mask;
        if (!cmp(p->st_func, sr, sv)) {
            int ns = stencil_op(p->st_sfail, stencil, p->st_ref, t->fb_fmt);
            /* The GE masks are "bits not to write": zero means all bits
               are written.  Stencil lives in alpha, so it is writable when
               no alpha bit is masked off. */
            if (!(p->write_mask & 0xFF000000u))
                px_write(t, x, y, (dst_px & 0x00FFFFFFu) | ((uint32_t)ns << 24), 0xFF000000u);
            return;
        }
    }

    if (p->z_test && !z_done) {
        if (!cmp(p->z_func, zi, z_read(t, za, x, y))) {
            if (stencil_en) {
                int ns = stencil_op(p->st_zfail, stencil, p->st_ref, t->fb_fmt);
                if (!(p->write_mask & 0xFF000000u))
                    px_write(t, x, y, (dst_px & 0x00FFFFFFu) | ((uint32_t)ns << 24), 0xFF000000u);
            }
            return;
        }
        z_done = 1;
        if (stencil_en) stencil = stencil_op(p->st_zpass, stencil, p->st_ref, t->fb_fmt);
    } else if (stencil_en) {
        /* No depth test: the z-pass operation is the one that runs. */
        stencil = stencil_op(p->st_zpass, stencil, p->st_ref, t->fb_fmt);
    }
#if defined(PERF_NOBLEND) || defined(PERF_MIN)
    { int blend_off = p->blend; p = p; (void)blend_off; }
#else
    if (p->blend) {
#ifdef HLE_TRACE
        g_blend_px++;
#endif
        uint32_t dv = have_dst ? dst_px : px_read(t, x, y);
        float d[4] = {(float)(dv & 0xFF), (float)(dv >> 8 & 0xFF), (float)(dv >> 16 & 0xFF), (float)(dv >> 24)};
        float out[3], fa[3], fb[3];
        if (p->bl_eq < 3) {
            blend_factors(p->bl_src, 1, c, d, p->fix_a, fa);
            blend_factors(p->bl_dst, 0, c, d, p->fix_b, fb);
        }
        switch (p->bl_eq) {
        case 0: for (int i = 0; i < 3; i++) out[i] = c[i] * fa[i] + d[i] * fb[i]; break;
        case 1: for (int i = 0; i < 3; i++) out[i] = c[i] * fa[i] - d[i] * fb[i]; break;
        case 2: for (int i = 0; i < 3; i++) out[i] = d[i] * fb[i] - c[i] * fa[i]; break;
        case 3: for (int i = 0; i < 3; i++) out[i] = c[i] < d[i] ? c[i] : d[i]; break;
        case 4: for (int i = 0; i < 3; i++) out[i] = c[i] > d[i] ? c[i] : d[i]; break;
        default: for (int i = 0; i < 3; i++) out[i] = fabsf(c[i] - d[i]); break;
        }
        c[0] = out[0]; c[1] = out[1]; c[2] = out[2];
        c[3] = d[3];  // alpha holds stencil; blending never changes it
    }
#endif  /* PERF_NOBLEND */
    if (p->logic_test) {
        if (!have_dst) { dst_px = px_read(t, x, y); have_dst = 1; }
        uint32_t sc = (uint32_t)clamp255(c[0]) | (uint32_t)clamp255(c[1]) << 8 |
                      (uint32_t)clamp255(c[2]) << 16 | (uint32_t)clamp255(c[3]) << 24;
        uint32_t nc = logic_apply(p->logic_op, dst_px, sc);
        c[0] = (float)(nc & 0xFF);
        c[1] = (float)(nc >> 8 & 0xFF);
        c[2] = (float)(nc >> 16 & 0xFF);
        c[3] = (float)(nc >> 24);
    }
    if (stencil_en) c[3] = (float)stencil;
    uint32_t px = (uint32_t)clamp255(c[0]) | (uint32_t)clamp255(c[1]) << 8 |
                  (uint32_t)clamp255(c[2]) << 16 | (uint32_t)clamp255(c[3]) << 24;
    /* Blending preserves the destination alpha, which in this game is the
       stencil channel.  A stencil write has to survive that preservation. */
    uint32_t mask = p->write_mask;
    if (p->blend && !stencil_en) mask |= 0xFF000000u;
    px_write(t, x, y, px, mask);
#if !defined(PERF_NOZ) && !defined(PERF_MIN)
    if (p->z_write && p->z_test) z_write(t, za, x, y, zi);
#endif
}

// ---------------------------------------------------------------- primitives

static inline float edge(const SVtx *a, const SVtx *b, float x, float y) {
    return (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
}

/* The setup of a triangle (bounding box, edge functions, the perspective
   denominator) is done once per triangle; the pixel loop below runs per
   pixel.  They are separate functions so the optimiser keeps the hot loop
   small -- and so gcc 15.2 does not trip over the combined function. */
typedef struct {
    int x0, x1, y0, y1;
    float e0x, e0y, e1x, e1y, e2x, e2y;
    float r0, r1, r2;
    float px0, py0;
    float d0, dd0;
    int valid;
} TriSetup;

static TriSetup tri_setup(const PixState *p, const SVtx *v0, const SVtx *v1, const SVtx *v2) {
    TriSetup s;
    s.valid = 0;
    const Target *t = &p->tg;
    float miny = fminf(v0->y, fminf(v1->y, v2->y)), maxy = fmaxf(v0->y, fmaxf(v1->y, v2->y));
    s.y0 = fast_floor(miny) > (int)t->sy1 ? fast_floor(miny) : (int)t->sy1;
    s.y1 = fast_ceil(maxy) < (int)t->sy2 ? fast_ceil(maxy) : (int)t->sy2;
    if (s.y1 < s.y0 || !owns_any_row(s.y0, s.y1, tl_band)) return s;
    float area = edge(v0, v1, v2->x, v2->y);
    if (area == 0.f || !isfinite(area)) return s;
    float minx = fminf(v0->x, fminf(v1->x, v2->x)), maxx = fmaxf(v0->x, fmaxf(v1->x, v2->x));
    s.x0 = fast_floor(minx) > (int)t->sx1 ? fast_floor(minx) : (int)t->sx1;
    s.x1 = fast_ceil(maxx) < (int)t->sx2 ? fast_ceil(maxx) : (int)t->sx2;
    if (s.x1 < s.x0 || s.x1 - s.x0 > 2048 * t->scale || s.y1 - s.y0 > 2048 * t->scale) return s;
    float inv = 1.f / area;
    s.e0x = -(v2->y - v1->y) * inv; s.e0y = (v2->x - v1->x) * inv;
    s.e1x = -(v0->y - v2->y) * inv; s.e1y = (v0->x - v2->x) * inv;
    s.e2x = -(v1->y - v0->y) * inv; s.e2y = (v1->x - v0->x) * inv;
    s.px0 = (float)s.x0 + 0.5f; s.py0 = (float)s.y0 + 0.5f;
    s.r0 = edge(v1, v2, s.px0, s.py0) * inv;
    s.r1 = edge(v2, v0, s.px0, s.py0) * inv;
    s.r2 = edge(v0, v1, s.px0, s.py0) * inv;
    s.valid = 1;
    return s;
}

static void draw_triangle(const PixState *p, const SVtx *v0, const SVtx *v1, const SVtx *v2, int flat) {
#ifdef HLE_TRACE
    uint64_t t_setup = hle_time_us();
#endif
    const Target *t = &p->tg;
    TriSetup ts = tri_setup(p, v0, v1, v2);
    if (!ts.valid) return;
    const int x0 = ts.x0, x1 = ts.x1, y0 = ts.y0, y1 = ts.y1;
    const float e0x = ts.e0x, e0y = ts.e0y, e1x = ts.e1x, e1y = ts.e1y, e2x = ts.e2x, e2y = ts.e2y;
    float r0 = ts.r0, r1 = ts.r1, r2 = ts.r2;
    const float px0 = ts.px0, py0 = ts.py0;
    float tw = p->through ? 1.f : (float)p->tex.w, th = p->through ? 1.f : (float)p->tex.h;
    int persp = !p->through;
#ifdef HLE_TRACE
    if (persp) g_persp_tris++;
    else g_through_tris++;
#endif
    float flat_col[4];
    for (int i = 0; i < 4; i++) flat_col[i] = v2->c[i] / v2->iw;
    int band = tl_band;
    // the depth test can run before anything else (the alpha test would have to come first)
    int early_z = p->z_test && !p->alpha_test && !p->stencil_test && !p->clear &&
                  (t->scol || (x1 < 1024 && y1 < 1024));
    for (int y = y0; y <= y1; y++, r0 += e0y, r1 += e1y, r2 += e2y) {
        if (!owns_row(y, band)) continue;        // span of this row where all three weights are >= 0 (w = r + e*(x - x0))
        float lo = 0.f, hi = (float)(x1 - x0);
        const float rr[3] = {r0, r1, r2}, ee[3] = {e0x, e1x, e2x};
        for (int i = 0; i < 3; i++) {
            if (ee[i] > 0.f) lo = fmaxf(lo, -rr[i] / ee[i]);
            else if (ee[i] < 0.f) hi = fminf(hi, rr[i] / -ee[i]);
            else if (rr[i] < 0.f) hi = -1.f;
        }
        if (hi < lo) continue;
        int xs = x0 + fast_ceil(lo - 0.001f), xe = x0 + fast_floor(hi + 0.001f);
        if (xs < x0) xs = x0;
        if (xe > x1) xe = x1;
        if (xe >= xs) g_worker_px[band] += (uint64_t)(xe - xs + 1);
        float dx = (float)(xs - x0);
        float w0 = r0 + e0x * dx, w1 = r1 + e1x * dx, w2 = r2 + e2x * dx;
        // The perspective denominator D = w0*iw0 + w1*iw1 + w2*iw2 is affine
        // in x, so D and its reciprocal can be carried along the scanline.  A
        // float divide per pixel is very expensive on the Broadway's scalar
        // FPU, so seed the reciprocal once and refine it with one
        // Newton-Raphson step per pixel (two multiplies, no divide).
        float d0 = w0 * v0->iw + w1 * v1->iw + w2 * v2->iw;
        float dd0 = e0x * v0->iw + e1x * v1->iw + e2x * v2->iw;  // d(D)/dx
        float rcp = d0 != 0.f ? 1.f / d0 : 0.f;  // one divide per scanline
        for (int x = xs; x <= xe; x++, w0 += e0x, w1 += e1x, w2 += e2x,
                          d0 += dd0, rcp = rcp * (2.f - d0 * rcp)) {
            if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
            float z = w0 * v0->z + w1 * v1->z + w2 * v2->z;
            if (early_z) {  // depth first: hidden pixels skip all the interpolation below
                int zi = (int)(z < 0.f ? 0.f : z > 65535.f ? 65535.f : z);
                uint32_t za = t->zb + (uint32_t)(y * (int)t->zb_stride + x) * 2;
                if (!cmp(p->z_func, zi, z_read(t, za, x, y))) continue;
            }
            float k = persp ? rcp : 1.f;
            float cr, cg, cb, ca;
#if defined(PERF_FLAT)
            cr = flat_col[0]; cg = flat_col[1]; cb = flat_col[2]; ca = flat_col[3];
#else
            if (flat) {
                cr = flat_col[0]; cg = flat_col[1]; cb = flat_col[2]; ca = flat_col[3];
            } else {
                cr = (w0 * v0->c[0] + w1 * v1->c[0] + w2 * v2->c[0]) * k;
                cg = (w0 * v0->c[1] + w1 * v1->c[1] + w2 * v2->c[1]) * k;
                cb = (w0 * v0->c[2] + w1 * v1->c[2] + w2 * v2->c[2]) * k;
                ca = (w0 * v0->c[3] + w1 * v1->c[3] + w2 * v2->c[3]) * k;
            }
#endif
            float u = (w0 * v0->u + w1 * v1->u + w2 * v2->u) * k * tw;
            float v = (w0 * v0->v + w1 * v1->v + w2 * v2->v) * k * th;
            /* The fog factor is interpolated in screen space, not divided by w. */
            float fog = w0 * v0->fog + w1 * v1->fog + w2 * v2->fog;
#ifdef PERF_NOSHADE
            if (z == 12345.f) shade(p, x, y, z, cr, cg, cb, ca, u, v, fog, early_z);
#else
            shade(p, x, y, z, cr, cg, cb, ca, u, v, fog, early_z);
#endif
        }
    }
#ifdef HLE_TRACE
    g_px_count += (uint64_t)(x1 - x0 + 1) * (uint64_t)(y1 - y0 + 1);
    g_setup_us += hle_time_us() - t_setup;
    g_prim_count++;
#endif
}

// RECTANGLES: two corner vertices; color/z from the second one.
static void draw_rect(const PixState *p, const SVtx *a, const SVtx *b) {
#ifdef HLE_TRACE
    uint64_t t0 = hle_time_us();
#endif
    float x0 = fminf(a->x, b->x), x1 = fmaxf(a->x, b->x), y0 = fminf(a->y, b->y), y1 = fmaxf(a->y, b->y);
    int ix0 = (int)ceilf(x0 - 0.5f), ix1 = (int)ceilf(x1 - 0.5f), iy0 = (int)ceilf(y0 - 0.5f), iy1 = (int)ceilf(y1 - 0.5f);
    float col[4];
    for (int i = 0; i < 4; i++) col[i] = b->c[i] / b->iw;
    float du = (b->u - a->u) / (b->x - a->x ? b->x - a->x : 1.f);
    float dv = (b->v - a->v) / (b->y - a->y ? b->y - a->y : 1.f);
    float tw = (float)p->tex.w, th = (float)p->tex.h;
    int band = tl_band;
    for (int y = iy0; y < iy1; y++) {
        if (!owns_row(y, band)) continue;
        for (int x = ix0; x < ix1; x++) {
            float u = a->u + ((float)x + 0.5f - a->x) * du, v = a->v + ((float)y + 0.5f - a->y) * dv;
            if (!p->through) {
                u *= tw;
                v *= th;
            }
            shade(p, x, y, b->z, col[0], col[1], col[2], col[3], u, v, b->fog, 0);
        }
    }
#ifdef HLE_TRACE
    g_rect_us += hle_time_us() - t0;
#endif
}

/* Built at a lower optimisation level: gcc 15 has a bug in its control
   flow clean up (try_forward_edges) that it trips over in this
   function, whose body inlines the whole shader.  The lines are rare,
   so the difference does not show. */
static __attribute__((optimize("O1"))) void draw_line(const PixState *p,
                                                       const SVtx *a, const SVtx *b) {
#ifdef HLE_TRACE
    uint64_t t0 = hle_time_us();
#endif
    float dx = b->x - a->x, dy = b->y - a->y;
    /* The volatile keeps gcc 15 from tripping over its own control flow
       graph here (an internal compiler error in try_forward_edges). */
    volatile int n = (int)fmaxf(fabsf(dx), fabsf(dy));
    int S = p->tg.scale;
    for (int i = 0; i <= n; i++) {
        float t = n ? (float)i / (float)n : 0.f, col[4];
        for (int k = 0; k < 4; k++) col[k] = (a->c[k] / a->iw) * (1 - t) + (b->c[k] / b->iw) * t;
        int px = (int)(a->x + dx * t), py = (int)(a->y + dy * t);
        for (int j = 0; j < S; j++)  // keep the 1-PSP-pixel thickness
            if (owns_row(py + j, tl_band))
                for (int k = 0; k < S; k++)
                    shade(p, px + k, py + j, a->z + (b->z - a->z) * t,
                          col[0], col[1], col[2], col[3], a->u, a->v,
                          a->fog + (b->fog - a->fog) * t, 0);
    }
#ifdef HLE_TRACE
    g_line_us += hle_time_us() - t0;
#endif
}

static void draw_point(const PixState *p, const SVtx *a) {
#ifdef HLE_TRACE
    uint64_t t0 = hle_time_us();
#endif
    float col[4];
    for (int k = 0; k < 4; k++) col[k] = a->c[k] / a->iw;
    int S = p->tg.scale, x = (int)a->x, y = (int)a->y;
    for (int j = 0; j < S; j++)
        if (owns_row(y + j, tl_band))
            for (int i = 0; i < S; i++)
                shade(p, x + i, y + j, a->z, col[0], col[1], col[2], col[3], a->u, a->v, a->fog, 0);
}

// ---------------------------------------------------------------- job queue

enum { JOB_TRI, JOB_RECT, JOB_LINE, JOB_POINT };
typedef struct {
    uint8_t kind, flat;
    uint16_t state;
    SVtx v[3];
} Job;

#ifdef __wii__
/* No worker threads: the inline path never touches the job/state ring. */
#define NJOBS 64
#define NSTATES 64
#else
#define NJOBS 65536
#define NSTATES 8192
#endif
#define MAX_WORKERS 16
static Job g_jobs[NJOBS];
static PixState g_states[NSTATES];
static int g_state_next;
static uint64_t g_job_w, g_band_r[NBANDS];  // published end; per band: drawn up to
static uint8_t g_band_busy[NBANDS];
static uint64_t g_job_pending, g_job_r_min_cache;  // producer side (GPU thread only)
static int g_nworkers;
static pthread_mutex_t g_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_work = PTHREAD_COND_INITIALIZER, g_q_done = PTHREAD_COND_INITIALIZER;
static int g_cur_state = -1;
static const PixState *g_cur_ps;

static void run_job(const Job *j) {
    const PixState *p = &g_states[j->state];
    switch (j->kind) {
    case JOB_TRI: draw_triangle(p, &j->v[0], &j->v[1], &j->v[2], j->flat); break;
    case JOB_RECT: draw_rect(p, &j->v[0], &j->v[1]); break;
    case JOB_LINE: draw_line(p, &j->v[0], &j->v[1]); break;
    default: draw_point(p, &j->v[0]); break;
    }
}

static void *raster_worker(void *arg) {
    int me = (int)(intptr_t)arg;
    pthread_mutex_lock(&g_q_lock);
    for (;;) {
        // take the free band that is furthest behind
        int b = -1;
        for (int k = 0; k < NBANDS; k++)
            if (!g_band_busy[k] && g_band_r[k] < g_job_w && (b < 0 || g_band_r[k] < g_band_r[b])) b = k;
        if (b < 0) {
            pthread_cond_wait(&g_q_work, &g_q_lock);
            continue;
        }
        g_band_busy[b] = 1;
        uint64_t start = g_band_r[b], end = g_job_w;
        pthread_mutex_unlock(&g_q_lock);
        tl_band = b;
        uint64_t t0 = hle_time_us();
        for (uint64_t i = start; i < end; i++) run_job(&g_jobs[i % NJOBS]);
        g_worker_busy_us[me] += hle_time_us() - t0;
        pthread_mutex_lock(&g_q_lock);
        g_band_r[b] = end;
        g_band_busy[b] = 0;
        pthread_cond_broadcast(&g_q_done);
    }
    return NULL;
}

static uint64_t min_read(void) {
    uint64_t m = g_job_w;
    for (int k = 0; k < NBANDS; k++)
        if (g_band_r[k] < m) m = g_band_r[k];
    return m;
}

static void kick(void);

// Wait until every queued primitive has been drawn.
uint64_t g_flush_wait_us, g_prim_us, g_switch_us;  // stats
uint64_t g_prim_count_clipped;    // triangles clipped away
uint64_t g_prim_calls;            // draw_prim calls
uint64_t g_prim_type[8];          // primitives by type
uint64_t g_rect_us, g_line_us, g_point_us;  // time in the non-triangle paths
volatile uint32_t g_raster_draws;  // primitives drawn, for the frontend
uint64_t hle_time_us(void);

void raster_flush(void) {
    if (!g_nworkers) {  // everything was drawn inline already
        tex_free_retired();
        return;
    }
    kick();
    uint64_t t0 = hle_time_us();
    pthread_mutex_lock(&g_q_lock);
    while (min_read() != g_job_w) pthread_cond_wait(&g_q_done, &g_q_lock);
    pthread_mutex_unlock(&g_q_lock);
    g_n_pending_targets = 0;
    tex_free_retired();
    g_flush_wait_us += hle_time_us() - t0;
}

static void raster_init(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *e = getenv("RASTER_THREADS");
#ifdef __wii__
    long n = e ? atol(e) : 0;  /* one Broadway core: draw inline */
#else
    long n = e ? atol(e) : sysconf(_SC_NPROCESSORS_ONLN) - 2;
#endif
    if (n < 0) n = 0;
    if (n > MAX_WORKERS) n = MAX_WORKERS;
    if (n == 1) n = 0;  // one worker buys nothing over drawing inline
    g_nworkers = (int)n;
    g_nbands = n ? NBANDS : 1;
    for (int k = 0; k < g_nworkers; k++) {
        pthread_t t;
        pthread_create(&t, NULL, raster_worker, (void *)(intptr_t)k);
        pthread_detach(t);
    }
    fprintf(stderr, "raster: %d worker threads\n", g_nworkers);
}

// ---------------------------------------------------------------- GPU renderer (glr.h)

static GlrState g_glr_last;
static int g_glr_have_last;
static uint32_t g_glr_fbs[16];  // framebuffers whose FBO was started from VRAM
static int g_glr_nfbs;

// The GPU renderer keeps framebuffers in FBOs: VRAM does not see what it
// draws. Report (once per address) when the game reads a framebuffer region.
static void glr_note_fb_access(const char *what, uint32_t lo, uint32_t hi) {
    lo &= 0x0FFFFFFFu;
    hi &= 0x0FFFFFFFu;
    for (int k = 0; k < g_glr_nfbs; k++) {
        uint32_t f = g_glr_fbs[k];
        if (lo < f + 512 * 272 * 2 && f < hi) {
            static uint32_t seen[64];
            static int nseen;
            uint32_t key = lo ^ (uint32_t)(uintptr_t)what;
            for (int i = 0; i < nseen; i++)
                if (seen[i] == key) return;
            if (nseen < 64) seen[nseen++] = key;
            fprintf(stderr, "glr: %s reads framebuffer %08x (range %08x-%08x): not drawn by the GPU renderer yet\n",
                    what, f, lo, hi);
            return;
        }
    }
}

static void glr_switched(void) {
    g_glr_have_last = 0;
    g_glr_nfbs = 0;  // FBOs restart from VRAM
}

// The FBO of a framebuffer drawn for the first time starts with what VRAM holds.
static void glr_fb_first_use(const Target *t) {
    uint32_t fb = t->fb & 0x0FFFFFF0u;
    for (int k = 0; k < g_glr_nfbs; k++)
        if (g_glr_fbs[k] == fb) return;
    if (g_glr_nfbs < 16) g_glr_fbs[g_glr_nfbs++] = fb;
    /* The GL renderer only (g_glr_active is never set on the Wii); a static array here held 522 KB of MEM1 for nothing. */
    static uint32_t *px;
    if (!px && !(px = big_alloc(480u * 272u * sizeof *px))) return;
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++) px[y * 480 + x] = fb_read(t, x, y);
    glr_fb_init(fb, 480, 272, px);
}

static void glr_begin(const PixState *ps) {
    const Target *t = &ps->tg;
    const TexState *x = &ps->tex;
    glr_fb_first_use(t);
    GlrState g;
    memset(&g, 0, sizeof g);
    g.fb = t->fb;
    g.fb_stride = t->fb_stride;
    g.fb_fmt = t->fb_fmt;
    g.sx1 = (int16_t)t->sx1; g.sy1 = (int16_t)t->sy1; g.sx2 = (int16_t)t->sx2; g.sy2 = (int16_t)t->sy2;
    if (x->enabled && x->pix && !ps->clear) {
        if (!glr_has_texture(x->serial)) glr_texture(x->serial, x->pw, x->ph, x->pix);
        g.tex_serial = x->serial;
        g.tex_linear = (uint8_t)x->linear;
        g.tex_clamp_u = (uint8_t)x->clamp_u;
        g.tex_clamp_v = (uint8_t)x->clamp_v;
        g.tex_func = (uint8_t)x->func;
        g.tex_rgba = (uint8_t)x->rgba;
        g.tex_dbl = (uint8_t)x->dbl;
        g.env = (uint32_t)x->env[0] | (uint32_t)x->env[1] << 8 | (uint32_t)x->env[2] << 16;
    }
    g.alpha_test = (uint8_t)ps->alpha_test; g.at_func = (uint8_t)ps->at_func;
    g.at_ref = (uint8_t)ps->at_ref; g.at_mask = (uint8_t)ps->at_mask;
    g.z_test = (uint8_t)ps->z_test; g.z_func = (uint8_t)ps->z_func; g.z_write = (uint8_t)ps->z_write;
    g.blend = (uint8_t)ps->blend; g.bl_src = (uint8_t)ps->bl_src; g.bl_dst = (uint8_t)ps->bl_dst;
    g.bl_eq = (uint8_t)ps->bl_eq;
    g.clear = (uint8_t)ps->clear; g.clear_color = (uint8_t)ps->clear_color;
    g.clear_alpha = (uint8_t)ps->clear_alpha; g.clear_depth = (uint8_t)ps->clear_depth;
    g.fix_a = ps->fix_a; g.fix_b = ps->fix_b; g.write_mask = ps->write_mask;
    if (g_glr_have_last && !memcmp(&g, &g_glr_last, sizeof g)) return;
    g_glr_last = g;
    g_glr_have_last = 1;
    glr_state(&g);
}

static inline uint8_t col8(float v) { return (uint8_t)(v < 0.f ? 0.f : v > 255.f ? 255.f : v); }

// screen-space vertex -> clip space (FBOs cover 512 x 512 PSP pixels; depth 0..65535)
static void glr_vtx(GlrVtx *o, float x, float y, float z, float w, float u, float v, const float *col) {
    if (z < 0.f) z = 0.f;
    if (z > 65535.f) z = 65535.f;
    o->x = (x / 256.f - 1.f) * w;
    o->y = (y / 256.f - 1.f) * w;
    o->z = (z / 32767.5f - 1.f) * w;
    o->w = w;
    o->u = u;
    o->v = v;
    for (int k = 0; k < 4; k++) o->c[k] = col8(col[k]);
}

static void glr_submit(int kind, const SVtx *a, const SVtx *b, const SVtx *c, int flat) {
    const PixState *p = g_cur_ps;
    // texture coordinates: texels in through mode, else already normalized
    float su = p->through && p->tex.w ? 1.f / (float)p->tex.w : 1.f;
    float sv = p->through && p->tex.h ? 1.f / (float)p->tex.h : 1.f;
    switch (kind) {
    case JOB_TRI: {
        GlrVtx *o = glr_vertices(GLR_TRIANGLES, 3);
        const SVtx *v[3] = {a, b, c};
        float fc[4];
        for (int k = 0; k < 4; k++) fc[k] = c->c[k] / c->iw;
        for (int i = 0; i < 3; i++) {
            float w = 1.f / v[i]->iw, col[4];
            for (int k = 0; k < 4; k++) col[k] = flat ? fc[k] : v[i]->c[k] * w;
            glr_vtx(&o[i], v[i]->x, v[i]->y, v[i]->z, w, v[i]->u * w * su, v[i]->v * w * sv, col);
        }
        break;
    }
    case JOB_RECT: {  // corners a, b; colour and depth from b
        GlrVtx *o = glr_vertices(GLR_TRIANGLES, 6);
        float col[4], ua = a->u / a->iw * su, va = a->v / a->iw * sv, ub = b->u / b->iw * su, vb = b->v / b->iw * sv;
        for (int k = 0; k < 4; k++) col[k] = b->c[k] / b->iw;
        glr_vtx(&o[0], a->x, a->y, b->z, 1.f, ua, va, col);
        glr_vtx(&o[1], b->x, a->y, b->z, 1.f, ub, va, col);
        glr_vtx(&o[2], a->x, b->y, b->z, 1.f, ua, vb, col);
        o[3] = o[1];
        glr_vtx(&o[4], b->x, b->y, b->z, 1.f, ub, vb, col);
        o[5] = o[2];
        break;
    }
    case JOB_LINE: {
        GlrVtx *o = glr_vertices(GLR_LINES, 2);
        const SVtx *v[2] = {a, b};
        for (int i = 0; i < 2; i++) {
            float col[4];
            for (int k = 0; k < 4; k++) col[k] = v[i]->c[k] / v[i]->iw;
            glr_vtx(&o[i], v[i]->x + .5f, v[i]->y + .5f, v[i]->z, 1.f, v[i]->u / v[i]->iw * su,
                    v[i]->v / v[i]->iw * sv, col);
        }
        break;
    }
    default: {  // point: a one-pixel quad
        GlrVtx *o = glr_vertices(GLR_TRIANGLES, 6);
        float col[4], u = a->u / a->iw * su, v = a->v / a->iw * sv, x = (float)fast_floor(a->x), y = (float)fast_floor(a->y);
        for (int k = 0; k < 4; k++) col[k] = a->c[k] / a->iw;
        glr_vtx(&o[0], x, y, a->z, 1.f, u, v, col);
        glr_vtx(&o[1], x + 1, y, a->z, 1.f, u, v, col);
        glr_vtx(&o[2], x, y + 1, a->z, 1.f, u, v, col);
        o[3] = o[1];
        glr_vtx(&o[4], x + 1, y + 1, a->z, 1.f, u, v, col);
        o[5] = o[2];
    }
    }
}

// Snapshot the pixel state for the primitives that follow.
static void begin_state(const PixState *ps) {
    g_cur_ps = ps;
    if (g_glr_active) {
        glr_begin(ps);
        return;
    }
    if (!g_nworkers) return;
    {
        const Target *t = &ps->tg;
        uint32_t bpp = t->fb_fmt == 3 ? 4 : 2, rows = (uint32_t)(t->sy2 / t->scale) + 1;
        note_target(t->fb, t->fb + t->fb_stride * rows * bpp);
        if (ps->z_test || ps->clear_depth) note_target(t->zb, t->zb + t->zb_stride * rows * 2);
    }
    // consecutive primitives usually share their state: reuse the last snapshot
    if (g_cur_state >= 0 && !memcmp(&g_states[g_cur_state], ps, sizeof *ps)) return;
    if (g_state_next == NSTATES) {  // wrapping: states still referenced must be done
        g_flush_reason[4]++;
        raster_flush();
        g_state_next = 0;
    }
    g_states[g_state_next] = *ps;
    g_cur_state = g_state_next++;
}

static void submit(int kind, const SVtx *a, const SVtx *b, const SVtx *c, int flat) {
    if (g_glr_active) {
        glr_submit(kind, a, b, c, flat);
        return;
    }
    if (!g_nworkers) {
        switch (kind) {  // draw inline with the live state
        case JOB_TRI: draw_triangle(g_cur_ps, a, b, c, flat); break;
        case JOB_RECT: draw_rect(g_cur_ps, a, b); break;
        case JOB_LINE: draw_line(g_cur_ps, a, b); break;
        default: draw_point(g_cur_ps, a); break;
        }
        return;
    }
    // Jobs are written past the published end without the lock and made
    // visible in batches by kick(); only a full ring makes us wait.
    if (g_job_pending - g_job_r_min_cache >= NJOBS) {
        pthread_mutex_lock(&g_q_lock);
        g_job_w = g_job_pending;  // publish what we have so workers can drain
        pthread_cond_broadcast(&g_q_work);
        while (g_job_pending - min_read() >= NJOBS) pthread_cond_wait(&g_q_done, &g_q_lock);
        g_job_r_min_cache = min_read();
        pthread_mutex_unlock(&g_q_lock);
    }
    Job *j = &g_jobs[g_job_pending % NJOBS];
    j->kind = (uint8_t)kind;
    j->flat = (uint8_t)flat;
    j->state = (uint16_t)g_cur_state;
    j->v[0] = *a;
    if (b) j->v[1] = *b;
    if (c) j->v[2] = *c;
    g_job_pending++;
}

// Make queued work visible to the workers (end of a primitive batch).
static void kick(void) {
    if (!g_nworkers || g_job_pending == g_job_w) return;
    pthread_mutex_lock(&g_q_lock);
    g_job_w = g_job_pending;
    g_job_r_min_cache = min_read();
    pthread_cond_broadcast(&g_q_work);
    pthread_mutex_unlock(&g_q_lock);
}

// Clip a triangle against the near plane (z > -w) in clip space.
static int clip_near(const Vtx *in, int n, Vtx *out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const Vtx *a = &in[i], *b = &in[(i + 1) % n];
        float da = a->z + a->w, db = b->z + b->w;
        if (da >= 0.f) out[m++] = *a;
        if ((da >= 0.f) != (db >= 0.f)) {
            float t = da / (da - db);
            Vtx *o = &out[m++];
            o->x = a->x + (b->x - a->x) * t; o->y = a->y + (b->y - a->y) * t;
            o->z = a->z + (b->z - a->z) * t; o->w = a->w + (b->w - a->w) * t;
            o->u = a->u + (b->u - a->u) * t; o->v = a->v + (b->v - a->v) * t;
            for (int k = 0; k < 4; k++) o->c[k] = a->c[k] + (b->c[k] - a->c[k]) * t;
        }
    }
    return m;
}

static int cull_reject(const SVtx *a, const SVtx *b, const SVtx *c, int odd) {
    if (!(R(GE_CULLFACEENABLE) & 1)) return 0;
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    int ccw = area > 0.f;
    if (odd) ccw = !ccw;
    int cull_cw = R(GE_CULL) & 1;
    return cull_cw ? !ccw : ccw;
}

static void emit_triangle(const PixState *p, const Vtx *a, const Vtx *b, const Vtx *c, int odd, int flat) {
#ifdef HLE_TRACE
    uint64_t t_emit = hle_time_us();
#endif
    Vtx in[3] = {*a, *b, *c}, clipped[8];
    int n = 3;
    const Vtx *poly = in;
    if (!p->through) {
        n = clip_near(in, 3, clipped);
        poly = clipped;
        if (n < 3) {
#ifdef HLE_TRACE
            g_emit_us += hle_time_us() - t_emit;
            g_prim_count_clipped++;
#endif
            return;
        }
    }
    SVtx s[8];
    for (int i = 0; i < n; i++) to_screen(&poly[i], &s[i], p->through, p->tg.rscale);
    if (!p->through && cull_reject(&s[0], &s[1], &s[2], odd)) {
#ifdef HLE_TRACE
        g_emit_us += hle_time_us() - t_emit;
#endif
        return;
    }
    for (int i = 1; i + 1 < n; i++) submit(JOB_TRI, &s[0], &s[i], &s[i + 1], flat);
#ifdef HLE_TRACE
    g_emit_us += hle_time_us() - t_emit;
#endif
}

#ifdef __wii__
#define MAX_VERTS 32768
#else
#define MAX_VERTS 65536
#endif
static Vtx g_verts[MAX_VERTS];
#ifdef __wii__
#define PK_MAX 1536
#ifdef EXP_GXLIGHT
static GxVtx g_pk[PK_MAX * 7 / 6 + 1];   /* a draw decoded straight into the chip's vertex form (or, lit by the chip, 28 bytes a vertex) */
#else
static GxVtx g_pk[PK_MAX];   /* a draw decoded straight into the chip's vertex form */
#endif
#endif

/* A draw that is not skinned is decoded once and kept (vcache.c).  An earlier
   cache of decoded vertices that was keyed by the generation counters of the
   GE state saw no hits, because those counters never come back to a value;
   this one is keyed by what the vertices are made from. */

static void draw_prim(int prim, int count);
#ifdef __wii__
static int draw_prim_fast(int prim, int count);
static int gxr_skip_2d(const PixState *p);
static void gxr_prim(const PixState *p, int prim, int count, Vtx *v, int through, int flat);
static void gxr_prim_packed(const PixState *p, int prim, int count, const GxVtx *v, const float *mv, const float *uvt);
static int gxr_prim_setup(const PixState *ps, int prim, int through);
static void gxr_pack(GxVtx *o, const Vtx *v);
#define VC_MAX_VERTS 32   /* the longest draw the draw cache holds */

/* The model-view matrix (world, then view) in the GE's layout, kept until
   either changes. */
static const float *gxr_modelview(void) {
    static float mv[12], w[12], v[12];
    static int valid;
    static uint32_t wgen, vgen;
    if (valid && wgen == g_ge_world_gen && vgen == g_ge_view_gen) return mv;   /* neither was written */
    wgen = g_ge_world_gen;
    vgen = g_ge_view_gen;
    if (valid && !memcmp(w, g_ge.world, sizeof w) && !memcmp(v, g_ge.view, sizeof v)) return mv;
    memcpy(w, g_ge.world, sizeof w);
    memcpy(v, g_ge.view, sizeof v);
    valid = 1;
    g_mv_version++;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            mv[i * 3 + j] = w[i * 3] * v[j] + w[i * 3 + 1] * v[3 + j] + w[i * 3 + 2] * v[6 + j];
    for (int j = 0; j < 3; j++) mv[9 + j] = w[9] * v[j] + w[10] * v[3 + j] + w[11] * v[6 + j] + v[9 + j];
    return mv;
}

/* Whether the lights are all directional (or off): then a vertex without a
   normal is lit whatever its position, and can be left in object space. */
static int gxr_lights_position_free(void) {
    static uint32_t gen;
    static int free_;
    if (gen == g_ge_light_gen) return free_;
    if (!(R(GE_LIGHTINGENABLE) & 1)) {
        free_ = 1;
    } else {
        if (g_lc.gen != g_ge_light_gen) build_light_cache();
        free_ = 1;
        for (int l = 0; l < 4; l++)
            if (g_lc.l[l].enabled && g_lc.l[l].kind != 0) free_ = 0;
    }
    gen = g_ge_light_gen;
    return free_;
}
#endif
#ifdef TKDR_AUTOPILOT
/* Camera steadiness: the camera's world position, worked out from the view
   matrix at the first skinned draw of each presented frame.  A smooth camera
   has second differences far below its first differences; shake has them
   at or above. */
uint32_t g_cam_n, g_cam_ring_n;
double g_cam_d1, g_cam_d2, g_cam_max2, g_cam_max1;
double g_cam_a1, g_cam_a2, g_cam_amax2;   /* the viewing direction: step and second difference, radians */
float g_cam_ring[32][3];
static void cam_sample(void) {
    extern uint32_t g_frames_presented;
    static uint32_t last_frame = 0xFFFFFFFFu;
    static float p1[3], p2[3], q1[3], q2[3];
    static int have;
    if (g_frames_presented == last_frame) return;
    last_frame = g_frames_presented;
    const float *v = g_ge.view;
    float c[3];
    for (int i = 0; i < 3; i++) c[i] = -(v[3 * i] * v[9] + v[3 * i + 1] * v[10] + v[3 * i + 2] * v[11]);
    const float q[3] = {v[2], v[5], v[8]};   /* the view z axis in world space */
    if (have >= 2) {
        float a1 = 0.f, a2 = 0.f;
        for (int i = 0; i < 3; i++) {
            float a = q[i] - q1[i], b = q[i] - 2.f * q1[i] + q2[i];
            a1 += a * a;
            a2 += b * b;
        }
        a1 = sqrtf(a1);
        a2 = sqrtf(a2);
        g_cam_a1 += a1;
        g_cam_a2 += a2;
        if (a2 > g_cam_amax2) g_cam_amax2 = a2;
        float d1 = 0.f, d2 = 0.f;
        for (int i = 0; i < 3; i++) {
            float a = c[i] - p1[i], b = c[i] - 2.f * p1[i] + p2[i];
            d1 += a * a;
            d2 += b * b;
        }
        d1 = sqrtf(d1);
        d2 = sqrtf(d2);
        g_cam_n++;
        g_cam_d1 += d1;
        g_cam_d2 += d2;
        if (d1 > g_cam_max1) g_cam_max1 = d1;
        if (d2 > g_cam_max2) g_cam_max2 = d2;
    }
    for (int i = 0; i < 3; i++) { p2[i] = p1[i]; p1[i] = c[i]; q2[i] = q1[i]; q1[i] = q[i]; }
    have++;
    for (int i = 0; i < 3; i++) g_cam_ring[g_cam_ring_n & 31][i] = c[i];
    g_cam_ring_n++;
}
#endif

#ifdef TKDR_AUTOPILOT
/* Debug: the GE registers that decide whether a 3D draw can show, printed when the
   chip's frame buffer goes dark (see present_probe() in wii_gxscene.c). */
void gpu_dump_regs(const char *why) {
    fprintf(stderr, "GEREGS (%s):\n", why);
    fprintf(stderr, "  viewport scale %g %g %g centre %g %g %g offset %04X %04X\n", (double)ge_float(R(GE_VIEWPORTXSCALE)), (double)ge_float(R(GE_VIEWPORTYSCALE)),
            (double)ge_float(R(GE_VIEWPORTZSCALE)), (double)ge_float(R(GE_VIEWPORTXCENTER)), (double)ge_float(R(GE_VIEWPORTYCENTER)),
            (double)ge_float(R(GE_VIEWPORTZCENTER)), R(GE_OFFSETX) & 0xFFFF, R(GE_OFFSETY) & 0xFFFF);
    fprintf(stderr, "  scissor %06X %06X  minz %06X maxz %06X  clearmode %06X  ztest %06X zwritedis %06X ztestenable %06X\n", R(GE_SCISSOR1), R(GE_SCISSOR2),
            R(GE_MINZ), R(GE_MAXZ), R(GE_CLEARMODE), R(GE_ZTEST), R(GE_ZWRITEDISABLE), R(GE_ZTESTENABLE));
    fprintf(stderr, "  fb %06X w %06X  zb %06X w %06X  fmt %06X  maskrgb %06X maskalpha %06X  cull %06X cullenable %06X  shade %06X\n", R(GE_FRAMEBUFPTR),
            R(GE_FRAMEBUFWIDTH), R(GE_ZBUFPTR), R(GE_ZBUFWIDTH), R(GE_FRAMEBUFPIXFORMAT), R(GE_MASKRGB), R(GE_MASKALPHA), R(GE_CULL), R(GE_CULLFACEENABLE),
            R(GE_SHADEMODE));
    fprintf(stderr, "  alpha blend enable %06X mode %06X  alpha test enable %06X test %06X  tex enable %06X  lighting %06X  vt %06X\n", R(GE_ALPHABLENDENABLE),
            R(GE_BLENDMODE), R(GE_ALPHATESTENABLE), R(GE_ALPHATEST), R(GE_TEXTUREMAPENABLE), R(GE_LIGHTINGENABLE), R(GE_VERTEXTYPE));
    fprintf(stderr, "  view row0 %g %g %g  t %g %g %g  proj %g %g %g %g | %g %g %g %g\n", (double)g_ge.view[0], (double)g_ge.view[3], (double)g_ge.view[6],
            (double)g_ge.view[9], (double)g_ge.view[10], (double)g_ge.view[11], (double)g_ge.proj[0], (double)g_ge.proj[5], (double)g_ge.proj[10],
            (double)g_ge.proj[11], (double)g_ge.proj[14], (double)g_ge.proj[15], (double)g_ge.proj[2], (double)g_ge.proj[3]);
}
#endif

/* The GE state at the first draw in world space of each frame, kept to be printed if the frame turns out dark. */
static struct {
    int valid;
    uint32_t r[24], all[256];
    float vp[8], view[12], proj[16], world[12];
    uint32_t prim, count, vt;
} g_snap;
static const uint8_t k_snap_regs[24] = {GE_ZTEST, GE_ZTESTENABLE, GE_ZWRITEDISABLE, GE_MINZ, GE_MAXZ, GE_CLEARMODE, GE_SCISSOR1, GE_SCISSOR2,
                                        GE_FRAMEBUFPTR, GE_FRAMEBUFWIDTH, GE_ZBUFPTR, GE_ZBUFWIDTH, GE_ALPHATESTENABLE, GE_ALPHATEST,
                                        GE_ALPHABLENDENABLE, GE_BLENDMODE, GE_MASKRGB, GE_MASKALPHA, GE_CULL, GE_CULLFACEENABLE,
                                        GE_LIGHTINGENABLE, GE_SHADEMODE, GE_VERTEXTYPE, GE_TEXTUREMAPENABLE};
void gpu_snap_reset(void) { g_snap.valid = 0; }
void gpu_snap_first3d(int prim, int count) {
    if (g_snap.valid) return;
    g_snap.valid = 1;
    for (int i = 0; i < 24; i++) g_snap.r[i] = R(k_snap_regs[i]);
    memcpy(g_snap.all, g_ge.cmd, sizeof g_snap.all);
    g_snap.vp[0] = ge_float(R(GE_VIEWPORTXSCALE)); g_snap.vp[1] = ge_float(R(GE_VIEWPORTYSCALE)); g_snap.vp[2] = ge_float(R(GE_VIEWPORTZSCALE));
    g_snap.vp[3] = ge_float(R(GE_VIEWPORTXCENTER)); g_snap.vp[4] = ge_float(R(GE_VIEWPORTYCENTER)); g_snap.vp[5] = ge_float(R(GE_VIEWPORTZCENTER));
    g_snap.vp[6] = (float)(R(GE_OFFSETX) & 0xFFFF); g_snap.vp[7] = (float)(R(GE_OFFSETY) & 0xFFFF);
    memcpy(g_snap.view, g_ge.view, sizeof g_snap.view);
    memcpy(g_snap.proj, g_ge.proj, sizeof g_snap.proj);
    memcpy(g_snap.world, g_ge.world, sizeof g_snap.world);
    g_snap.prim = (uint32_t)prim; g_snap.count = (uint32_t)count; g_snap.vt = R(GE_VERTEXTYPE);
}
void gpu_snap_dump(void) {
    if (!g_snap.valid) { fprintf(stderr, "SNAP no draw in world space this frame\n"); return; }
    fprintf(stderr, "SNAP first world-space draw: prim %u count %u vt %06X\n", g_snap.prim, g_snap.count, g_snap.vt);
    for (int i = 0; i < 24; i += 6)
        fprintf(stderr, "SNAP   regs %02X=%06X %02X=%06X %02X=%06X %02X=%06X %02X=%06X %02X=%06X\n", k_snap_regs[i], g_snap.r[i], k_snap_regs[i + 1], g_snap.r[i + 1],
                k_snap_regs[i + 2], g_snap.r[i + 2], k_snap_regs[i + 3], g_snap.r[i + 3], k_snap_regs[i + 4], g_snap.r[i + 4], k_snap_regs[i + 5], g_snap.r[i + 5]);
    fprintf(stderr, "SNAP   viewport scale %g %g %g centre %g %g %g offset %g %g\n", (double)g_snap.vp[0], (double)g_snap.vp[1], (double)g_snap.vp[2],
            (double)g_snap.vp[3], (double)g_snap.vp[4], (double)g_snap.vp[5], (double)g_snap.vp[6], (double)g_snap.vp[7]);
    for (int i = 0; i < 256; i += 8)
        fprintf(stderr, "SNAP   all %02X: %06X %06X %06X %06X %06X %06X %06X %06X\n", i, g_snap.all[i], g_snap.all[i + 1], g_snap.all[i + 2], g_snap.all[i + 3],
                g_snap.all[i + 4], g_snap.all[i + 5], g_snap.all[i + 6], g_snap.all[i + 7]);
    for (int i = 0; i < 12; i += 3) fprintf(stderr, "SNAP   view col%d %g %g %g\n", i / 3, (double)g_snap.view[i], (double)g_snap.view[i + 1], (double)g_snap.view[i + 2]);
    for (int i = 0; i < 16; i += 4)
        fprintf(stderr, "SNAP   proj col%d %g %g %g %g\n", i / 4, (double)g_snap.proj[i], (double)g_snap.proj[i + 1], (double)g_snap.proj[i + 2], (double)g_snap.proj[i + 3]);
    for (int i = 0; i < 12; i += 3) fprintf(stderr, "SNAP   world col%d %g %g %g\n", i / 3, (double)g_snap.world[i], (double)g_snap.world[i + 1], (double)g_snap.world[i + 2]);
}

#ifdef EXP_FOGPROBE
/* Ominous Mist's fog on the production path (no EXP_NOSETUPSKIP): in one frame
   of six, every draw that uses one of the stage's three scrolling textures is
   printed with what it was made from -- the first two vertices' bytes and a
   hash of all of them, the world matrix, the lighting, blend and depth
   registers -- and whether the fast path took it.  Read over time it shows
   whether the cards move, fade or repeat. */
typedef struct { int on, size, ndv; uint32_t tex, vt, vaddr, hash; uint8_t v[2][40]; Vtx dv[3]; } FogCap;
static int fog_pre(int count, FogCap *c) {
    extern uint32_t g_frames_presented;
    static uint32_t fp_frame = ~0u, fp_n[3], fp_all[3];
    c->on = 0;
    if (g_frames_presented % 6 || !(R(GE_TEXTUREMAPENABLE) & 1)) return 0;
    const uint32_t vt = R(GE_VERTEXTYPE);
    if (vt >> 23 & 1) return 0;
    const uint32_t addr = (R(GE_TEXADDR0) & 0xFFFFF0u) | ((R(GE_TEXBUFWIDTH0) << 8) & 0x0F000000u);
    const int which = addr == 0x040D8260u ? 0 : addr == 0x040D7DA0u ? 1 : addr == 0x040DB5E0u ? 2 : -1;
    if (which < 0) return 0;
    if (fp_frame != g_frames_presented) {   /* the last frame's totals: all the draws of each texture, logged or not */
        if (fp_frame != ~0u) fprintf(stderr, "FOGCNT f=%u 8260=%u 7DA0=%u B5E0=%u\n", fp_frame, fp_all[0], fp_all[1], fp_all[2]);
        fprintf(stderr, "FOGCAM f=%u view=", g_frames_presented);
        for (int i = 0; i < 12; i++) fprintf(stderr, "%g%c", (double)g_ge.view[i], i < 11 ? ',' : ' ');
        fprintf(stderr, "proj=");
        for (int i = 0; i < 16; i++) fprintf(stderr, "%g%c", (double)g_ge.proj[i], i < 15 ? ',' : ' ');
        fprintf(stderr, "vp=%g,%g,%g,%g,%g,%g zfunc=%u minmaxz=%X/%X scissor=%X/%X clip=%X\n", (double)ge_float(R(GE_VIEWPORTXSCALE)), (double)ge_float(R(GE_VIEWPORTYSCALE)),
                (double)ge_float(R(GE_VIEWPORTZSCALE)), (double)ge_float(R(GE_VIEWPORTXCENTER)), (double)ge_float(R(GE_VIEWPORTYCENTER)),
                (double)ge_float(R(GE_VIEWPORTZCENTER)), R(GE_ZTEST) & 7, R(GE_MINZ) & 0xFFFF, R(GE_MAXZ) & 0xFFFF, R(GE_SCISSOR1) & 0xFFFFFF,
                R(GE_SCISSOR2) & 0xFFFFFF, R(0x1C) & 1);
        fp_frame = g_frames_presented;
        for (int k = 0; k < 3; k++) fp_n[k] = fp_all[k] = 0;
    }
    fp_all[which]++;
    if (fp_n[which]++ >= (which == 2 ? 4u : 40u)) return 0;
    VFormat f;
    vformat(&f, vt);
    const uint8_t *vp = guest_host_ptr(g_ge.vaddr, (uint32_t)(f.size * count));
    if (!vp || f.idx) return 0;
    uint32_t h = 2166136261u;
    for (int i = 0; i < f.size * count; i++) h = (h ^ vp[i]) * 16777619u;
    memset(c->v, 0, sizeof c->v);
    for (int k = 0; k < 2 && k < count; k++) memcpy(c->v[k], vp + k * f.size, f.size < 40 ? (size_t)f.size : 40u);
    c->ndv = 0;
    if (f.nmorph == 1)   /* the first three vertices as the general decoder would send them: clip position, texture, colour, fog */
        for (int k = 0; k < 3 && k < count; k++) { decode_vertex(&f, g_ge.vaddr + (uint32_t)(k * f.size), &c->dv[k]); c->ndv++; }
    c->on = 1; c->size = f.size; c->tex = addr; c->vt = vt; c->vaddr = g_ge.vaddr; c->hash = h;
    return 1;
}
static void fog_post(int prim, int count, const FogCap *c, int fast) {
    extern uint32_t g_frames_presented;
    fprintf(stderr, "FOGDRAW f=%u tex=%08X prim=%d n=%d vt=%06X sz=%d fast=%d va=%08X h=%08X v0=", g_frames_presented, c->tex, prim, count,
            c->vt, c->size, fast, c->vaddr, c->hash);
    for (int i = 0; i < c->size && i < 40; i++) fprintf(stderr, "%02X", c->v[0][i]);
    fprintf(stderr, " v1=");
    for (int i = 0; i < c->size && i < 40; i++) fprintf(stderr, "%02X", c->v[1][i]);
    for (int k = 0; k < c->ndv; k++) {
        const Vtx *d = &c->dv[k];
        const float iw = d->w != 0.f ? 1.f / d->w : 0.f;
        fprintf(stderr, " P%d=%.3g,%.3g,%.3g,w%.4g,fog%.0f,c%.0f/%.0f/%.0f/%.0f,uv%.3g/%.3g", k, (double)(d->x * iw), (double)(d->y * iw), (double)(d->z * iw),
                (double)d->w, (double)d->fog, (double)d->c[0], (double)d->c[1], (double)d->c[2], (double)d->c[3], (double)d->u, (double)d->v);
    }
    fprintf(stderr, " W=");
    for (int i = 0; i < 12; i++) fprintf(stderr, "%g%c", (double)g_ge.world[i], i < 11 ? ',' : ' ');
    fprintf(stderr, "V=%g,%g,%g mat=%02X amb=%06X/%02X mu=%X diff=%06X lt=%d bm=%X at=%X/%X zt=%X zw=%X fog=%d cull=%d fn=%X\n",
            (double)g_ge.view[9], (double)g_ge.view[10], (double)g_ge.view[11], R(GE_MATERIALALPHA) & 0xFF, R(GE_AMBIENTCOLOR) & 0xFFFFFF,
            R(GE_AMBIENTALPHA) & 0xFF, R(GE_MATERIALUPDATE) & 7, R(GE_MATERIALDIFFUSE) & 0xFFFFFF, (int)(R(GE_LIGHTINGENABLE) & 1),
            R(GE_BLENDMODE) & 0xFFFFF, R(GE_ALPHATESTENABLE) & 1, R(GE_ALPHATEST) & 0xFFFFFF, R(GE_ZTESTENABLE) & 1, R(GE_ZWRITEDISABLE) & 1,
            (int)(R(GE_FOGENABLE) & 1), (int)(R(GE_CULLFACEENABLE) & 1), R(GE_TEXFUNC) & 0xFFFFF);
}
#endif

#ifdef EXP_BCEN
/* A census of the blended, textured draws of a frame (one frame in twelve): per texture (and blend mode, depth-write and
   alpha-test registers) the draws, the vertices, the range of the first vertex's clip w (about the distance from the
   eye; draws behind the eye are counted apart), and the position of its first draw among the frame's draws.  One BCEN
   line per row, BCENEND closes a frame with its draw total.  PPSSPP shows ground mist around the fighters' feet that
   the three textures of the fog probe cannot explain: which other textures draw it? */
typedef struct { uint32_t tex, bm, at; int w, h, fmt, clut, n, nv, zw, first, nback, nfront, skin; float wmin, wmax; } BcRow;
static BcRow s_bc[96];
static int s_bc_n;
static uint32_t s_bc_frame = ~0u, s_bc_draws;
static void bcen_flush(void) {
    if (s_bc_frame == ~0u || s_bc_frame % 12) return;
    for (int i = 0; i < s_bc_n; i++) {
        const BcRow *r = &s_bc[i];
        fprintf(stderr, "BCEN f=%u #%d tex=%08X %dx%d fmt=%d clut=%d n=%d verts=%d front=%d back=%d w=%.0f..%.0f zw=%d at=%X bm=%X skin=%d\n", s_bc_frame,
                r->first, r->tex, r->w, r->h, r->fmt, r->clut, r->n, r->nv, r->nfront, r->nback, r->nfront ? (double)r->wmin : 0.0, r->nfront ? (double)r->wmax : 0.0,
                r->zw, r->at, r->bm, r->skin);
    }
    fprintf(stderr, "BCENEND f=%u draws=%u rows=%d\n", s_bc_frame, s_bc_draws, s_bc_n);
}
static void bcen_note(int prim, int count) {
    extern uint32_t g_frames_presented;
    (void)prim;
    if (s_bc_frame != g_frames_presented) { bcen_flush(); s_bc_frame = g_frames_presented; s_bc_n = 0; s_bc_draws = 0; }
    s_bc_draws++;
    if (s_bc_frame % 12 || !(R(GE_ALPHABLENDENABLE) & 1) || !(R(GE_TEXTUREMAPENABLE) & 1)) return;
    const uint32_t vt = R(GE_VERTEXTYPE);
    if (vt >> 23 & 1) return;
    const uint32_t addr = (R(GE_TEXADDR0) & 0xFFFFF0u) | ((R(GE_TEXBUFWIDTH0) << 8) & 0x0F000000u);
    const uint32_t bm = R(GE_BLENDMODE) & 0xFFFFF, at = (R(GE_ALPHATESTENABLE) & 1) ? (R(GE_ALPHATEST) & 0xFFFFFF) : 0xFFFFFFu;
    const int zw = (int)(R(GE_ZWRITEDISABLE) & 1);
    BcRow *r = NULL;
    for (int i = 0; i < s_bc_n; i++)
        if (s_bc[i].tex == addr && s_bc[i].bm == bm && s_bc[i].at == at && s_bc[i].zw == zw) { r = &s_bc[i]; break; }
    if (!r) {
        if (s_bc_n >= 96) return;
        r = &s_bc[s_bc_n++];
        memset(r, 0, sizeof *r);
        r->tex = addr; r->bm = bm; r->at = at; r->zw = zw; r->first = (int)s_bc_draws;
        r->w = 1 << (R(GE_TEXSIZE0) & 15); r->h = 1 << (R(GE_TEXSIZE0) >> 8 & 15);
        r->fmt = (int)(R(GE_TEXFORMAT) & 15); r->clut = (int)(R(GE_CLUTFORMAT) & 3);
        r->wmin = 1e30f; r->wmax = -1e30f;
        r->skin = (int)(vt >> 14 & 7);
    }
    r->n++; r->nv += count;
    VFormat f;
    vformat(&f, vt);
    if (f.nmorph == 1 && !f.idx && guest_host_ptr(g_ge.vaddr, (uint32_t)f.size)) {
        Vtx d;
        decode_vertex(&f, g_ge.vaddr, &d);
        if (d.w > 0.f) { r->nfront++; if (d.w < r->wmin) r->wmin = d.w; if (d.w > r->wmax) r->wmax = d.w; } else r->nback++;
    }
}
#endif

#ifdef EXP_NEARLOG
/* The blended layers that draw around the fighters on Ominous Mist (the census, BCEN, puts them at clip w 1000-16000 where the
   fighters are at 3600-4800): 040EB8A0 (64x32, about 90 draws a frame), 040DAD20 (64x64), 040EDE20 (32x16) and 08AE5C10 (32x16,
   in main RAM, about 280 draws a frame).  In one frame in sixty, the first three draws of each: the vertex type, the lighting and
   blending state, the depth state, the texture state, and the first eight vertices as the general decoder makes them (clip
   position, depth, colour after lighting, texture coordinates), with the rectangle they cover on the screen.  NEAR lines are the
   draws, NEARCNT closes a frame with the number of draws of each texture. */
static void nearlog_note(int prim, int count) {
    extern uint32_t g_frames_presented;
    static const uint32_t k_tex[6] = {0x040D6BA0u, 0x040D8260u, 0x040D8B20u, 0x040D7DA0u, 0x040DB5E0u, 0x040E31E0u};
    static uint32_t frame = ~0u, n[6], all[6];
    if (frame != g_frames_presented) {
        if (frame != ~0u && frame % 60 == 0) fprintf(stderr, "NEARCNT f=%u 6BA0=%u 8260=%u 8B20=%u 7DA0=%u B5E0=%u 31E0=%u\n", frame, all[0], all[1], all[2], all[3], all[4], all[5]);
        frame = g_frames_presented;
        for (int k = 0; k < 6; k++) n[k] = all[k] = 0;
    }
    if (frame % 60 || !(R(GE_TEXTUREMAPENABLE) & 1)) return;
    const uint32_t vt = R(GE_VERTEXTYPE);
    if (vt >> 23 & 1) return;
    const uint32_t addr = (R(GE_TEXADDR0) & 0xFFFFF0u) | ((R(GE_TEXBUFWIDTH0) << 8) & 0x0F000000u);
    int which = -1;
    for (int k = 0; k < 6; k++) if (addr == k_tex[k]) which = k;
    if (which < 0) return;
    all[which]++;
    VFormat f;
    vformat(&f, vt);
    if (frame % 600 == 0 && frame >= 4200 && !f.idx && f.nmorph == 1 && count <= 8) {
        /* Every draw of the four in these frames, as screen pixels (480x272), clip w, colour and alpha per vertex: NQ lines are
           drawn as polygons over a blank frame to see where the layers fall against the fighters. */
        fprintf(stderr, "NQ f=%u L=%d prim=%d n=%d |", frame, which, prim, count);
        for (int k = 0; k < count; k++) {
            Vtx d;
            decode_vertex(&f, g_ge.vaddr + (uint32_t)(k * f.size), &d);
            const float iw = d.w != 0.f ? 1.f / d.w : 0.f;
            fprintf(stderr, " %.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f", (double)(240.f + 240.f * d.x * iw), (double)(136.f - 136.f * d.y * iw), (double)d.w,
                    (double)d.c[3], (double)d.c[0], (double)d.c[1], (double)d.c[2]);
        }
        fprintf(stderr, "\n");
    }
    if (n[which]++ >= 3) return;
    fprintf(stderr, "NEAR f=%u tex=%08X %dx%d fmt=%d clut=%d prim=%d n=%d vt=%06X sz=%d idx=%d nmorph=%d wt=%d | lt=%d mu=%X amb=%06X/%02X matA=%02X emis=%06X lights=%d%d%d%d | blend=%d bm=%X atE=%d at=%X zt=%d zf=%d zwdis=%d cull=%d/%d fog=%d | func=%X wrap=%X filt=%X mapmode=%X off=%g,%g scale=%g,%g",
            g_frames_presented, addr, 1 << (R(GE_TEXSIZE0) & 15), 1 << (R(GE_TEXSIZE0) >> 8 & 15), (int)(R(GE_TEXFORMAT) & 15), (int)(R(GE_CLUTFORMAT) & 3),
            prim, count, vt, f.size, f.idx, f.nmorph, f.wt, (int)(R(GE_LIGHTINGENABLE) & 1), R(GE_MATERIALUPDATE) & 7, R(GE_AMBIENTCOLOR) & 0xFFFFFF,
            R(GE_AMBIENTALPHA) & 0xFF, R(GE_MATERIALALPHA) & 0xFF, R(GE_MATERIALEMISSIVE) & 0xFFFFFF,
            (int)(R(GE_LIGHTENABLE0) & 1), (int)(R(GE_LIGHTENABLE0 + 1) & 1), (int)(R(GE_LIGHTENABLE0 + 2) & 1), (int)(R(GE_LIGHTENABLE0 + 3) & 1),
            (int)(R(GE_ALPHABLENDENABLE) & 1), R(GE_BLENDMODE) & 0xFFFFF, (int)(R(GE_ALPHATESTENABLE) & 1), R(GE_ALPHATEST) & 0xFFFFFF,
            (int)(R(GE_ZTESTENABLE) & 1), (int)(R(GE_ZTEST) & 7), (int)(R(GE_ZWRITEDISABLE) & 1), (int)(R(GE_CULLFACEENABLE) & 1), (int)(R(GE_CULL) & 1),
            (int)(R(GE_FOGENABLE) & 1), R(GE_TEXFUNC) & 0xFFFFF, R(GE_TEXWRAP) & 0xFFFF, R(GE_TEXFILTER) & 0xFFFF, R(GE_TEXMAPMODE) & 0xFFFF,
            (double)ge_float(R(GE_TEXOFFSETU)), (double)ge_float(R(GE_TEXOFFSETV)), (double)ge_float(R(GE_TEXSCALEU)), (double)ge_float(R(GE_TEXSCALEV)));
    const uint8_t *vp = guest_host_ptr(g_ge.vaddr, (uint32_t)(f.size * count));
    if (!vp || f.idx || f.nmorph != 1) { fprintf(stderr, " | (not decoded)\n"); return; }
    float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f, amin = 1e30f, amax = -1e30f;
    int behind = 0, nd = count < 8 ? count : 8;
    const float zs = ge_float(R(GE_VIEWPORTZSCALE)), zc = ge_float(R(GE_VIEWPORTZCENTER));
    fprintf(stderr, " | verts:");
    for (int k = 0; k < nd; k++) {
        Vtx d;
        decode_vertex(&f, g_ge.vaddr + (uint32_t)(k * f.size), &d);
        const float iw = d.w != 0.f ? 1.f / d.w : 0.f;
        if (d.w > 0.f) {
            const float nx = d.x * iw, ny = d.y * iw;
            if (nx < minx) minx = nx;
            if (nx > maxx) maxx = nx;
            if (ny < miny) miny = ny;
            if (ny > maxy) maxy = ny;
        } else behind++;
        if (d.c[3] < amin) amin = d.c[3];
        if (d.c[3] > amax) amax = d.c[3];
        if (k < 4)
            fprintf(stderr, " [w%.0f ndc%.3f,%.3f z16=%.0f c%.0f/%.0f/%.0f/%.0f uv%.3f,%.3f]", (double)d.w, (double)(d.x * iw), (double)(d.y * iw),
                    (double)(zc + zs * d.z * iw), (double)d.c[0], (double)d.c[1], (double)d.c[2], (double)d.c[3], (double)d.u, (double)d.v);
    }
    if (behind == nd) fprintf(stderr, " | all %d behind the eye", nd);
    else fprintf(stderr, " | screen %.0fx%.0f px (ndc x %.2f..%.2f y %.2f..%.2f), %d of %d behind, alpha %.0f..%.0f",
                 (double)((maxx - minx) * 240.f), (double)((maxy - miny) * 136.f), (double)minx, (double)maxx, (double)miny, (double)maxy, behind, nd, (double)amin, (double)amax);
    fprintf(stderr, "\n");
}
#endif

#ifdef EXP_CARDTRACK
/* EXP_CARDTRACK: the mist cards of Ominous Mist (040D8260, 040D7DA0, 040DB5E0) in every frame from 4100 to 4700, one line a
   draw: the number of the draw among its texture's in the frame, the vertex type, the first vertex's raw bytes, the first
   four vertices on the screen (x, y, clip w), the first vertex's texture coordinates, the texture offset and scale registers,
   and the world and view matrices' translation.  Whatever moves the cards (the guest's vertices, the matrices or the UV
   registers) shows as the column that changes from frame to frame. */
static void cardtrack_note(int prim, int count) {
    extern uint32_t g_frames_presented;
    static const uint32_t k_tex[3] = {0x040D8260u, 0x040D7DA0u, 0x040DB5E0u};
    static uint32_t frame = ~0u, n[3];
    if (frame != g_frames_presented) { frame = g_frames_presented; n[0] = n[1] = n[2] = 0; }
    if (frame < 4100 || frame > 4700 || !(R(GE_TEXTUREMAPENABLE) & 1)) return;
    const uint32_t vt = R(GE_VERTEXTYPE);
    if (vt >> 23 & 1) return;
    const uint32_t addr = (R(GE_TEXADDR0) & 0xFFFFF0u) | ((R(GE_TEXBUFWIDTH0) << 8) & 0x0F000000u);
    int which = -1;
    for (int k = 0; k < 3; k++) if (addr == k_tex[k]) which = k;
    if (which < 0) return;
    const uint32_t k = n[which]++;
    if (which == 2 && k >= 8) return;
    VFormat f;
    vformat(&f, vt);
    const uint8_t *vp = guest_host_ptr(g_ge.vaddr, (uint32_t)f.size);
    if (!vp || f.idx || f.nmorph != 1) return;
    fprintf(stderr, "CT f=%u L=%d k=%u prim=%d n=%d vt=%06X raw=", frame, which, k, prim, count, vt);
    for (int i = 0; i < f.size; i++) fprintf(stderr, "%02X", vp[i]);
    fprintf(stderr, " |");
    for (int i = 0; i < count && i < 4; i++) {
        Vtx d;
        decode_vertex(&f, g_ge.vaddr + (uint32_t)(i * f.size), &d);
        const float iw = d.w != 0.f ? 1.f / d.w : 0.f;
        fprintf(stderr, " %.0f,%.0f,%.0f", (double)(240.f + 240.f * d.x * iw), (double)(136.f - 136.f * d.y * iw), (double)d.w);
        if (i == 0) fprintf(stderr, " uv%.3f,%.3f", (double)d.u, (double)d.v);
    }
    fprintf(stderr, " | tex off=%g,%g scale=%g,%g | wm=%g,%g,%g vm=%g,%g,%g\n", (double)ge_float(R(GE_TEXOFFSETU)), (double)ge_float(R(GE_TEXOFFSETV)),
            (double)ge_float(R(GE_TEXSCALEU)), (double)ge_float(R(GE_TEXSCALEV)), (double)g_ge.world[9], (double)g_ge.world[10], (double)g_ge.world[11],
            (double)g_ge.view[9], (double)g_ge.view[10], (double)g_ge.view[11]);
}
#endif

#ifdef EXP_BLENDLOG
/* EXP_BLENDLOG: every textured, blended 3D draw (up to 16 vertices, not indexed) in frames 4800, 5400 and 6000 of Ominous Mist's
   fight: the texture, the blending, depth and alpha test state, whether the vertices are skinned, and the first eight vertices
   on the screen (x, y, clip w, alpha, colour).  PPSSPP shows white wisps of mist drifting over the ground round the fighters'
   feet from a few seconds into the fight; this is the list to find them in. */
static void blendlog_note(int prim, int count) {
    extern uint32_t g_frames_presented;
    static uint32_t frame = ~0u, idx;
    if (frame != g_frames_presented) { frame = g_frames_presented; idx = 0; }
    if (frame != 4800 && frame != 5400 && frame != 6000) return;
    idx++;
    const uint32_t vt = R(GE_VERTEXTYPE);
    if (vt >> 23 & 1 || !(R(GE_TEXTUREMAPENABLE) & 1) || !(R(GE_ALPHABLENDENABLE) & 1)) return;
    VFormat f;
    vformat(&f, vt);
    if (f.idx || f.nmorph != 1 || count < 3 || count > 16 || !guest_host_ptr(g_ge.vaddr, (uint32_t)f.size)) return;
    const uint32_t addr = (R(GE_TEXADDR0) & 0xFFFFF0u) | ((R(GE_TEXBUFWIDTH0) << 8) & 0x0F000000u);
    fprintf(stderr, "NB f=%u #%u T=%08X %dx%d fmt=%d prim=%d n=%d bm=%X atE=%d at=%X zt=%d zf=%d zwdis=%d lt=%d fog=%d wt=%d vt=%06X |", frame, idx, addr,
            1 << (R(GE_TEXSIZE0) & 15), 1 << (R(GE_TEXSIZE0) >> 8 & 15), (int)(R(GE_TEXFORMAT) & 15), prim, count, R(GE_BLENDMODE) & 0xFFFFF,
            (int)(R(GE_ALPHATESTENABLE) & 1), R(GE_ALPHATEST) & 0xFFFFFF, (int)(R(GE_ZTESTENABLE) & 1), (int)(R(GE_ZTEST) & 7), (int)(R(GE_ZWRITEDISABLE) & 1),
            (int)(R(GE_LIGHTINGENABLE) & 1), (int)(R(GE_FOGENABLE) & 1), f.wt, vt);
    for (int k = 0; k < count && k < 8; k++) {
        Vtx d;
        decode_vertex(&f, g_ge.vaddr + (uint32_t)(k * f.size), &d);
        const float iw = d.w != 0.f ? 1.f / d.w : 0.f;
        fprintf(stderr, " %.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f", (double)(240.f + 240.f * d.x * iw), (double)(136.f - 136.f * d.y * iw), (double)d.w,
                (double)d.c[3], (double)d.c[0], (double)d.c[1], (double)d.c[2]);
    }
    fprintf(stderr, "\n");
}
#endif

#ifdef EXP_ALLLOG
/* EXP_ALLLOG: every draw of frames 4800 and 6000 of Ominous Mist's fight, whatever its vertex type (indexed and morphing draws
   included, which the other census probes skip), with the render target, the texture, the state, and the screen box and the
   range of clip w of the vertices (indexed ones through their indices, morphing ones by the first target). */
static void alllog_note(int prim, int count) {
    extern uint32_t g_frames_presented;
    static uint32_t frame = ~0u, idx;
    if (frame != g_frames_presented) { frame = g_frames_presented; idx = 0; }
    if (frame != 4800 && frame != 6000) return;
    idx++;
    const uint32_t vt = R(GE_VERTEXTYPE);
    VFormat f;
    vformat(&f, vt);
    const uint32_t addr = (R(GE_TEXADDR0) & 0xFFFFF0u) | ((R(GE_TEXBUFWIDTH0) << 8) & 0x0F000000u);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, w0 = 1e9f, w1 = -1e9f;
    int behind = 0, nd = 0;
    const uint32_t vbase = g_ge.vaddr, step = (uint32_t)(f.size * f.nmorph);
    if (count <= 2048 && guest_host_ptr(vbase, step)) {
        for (int i = 0; i < count; i++) {
            int k = i;
            if (f.idx == 1) k = mem_r8(g_ge.iaddr + (uint32_t)i);
            else if (f.idx == 2) k = mem_r16(g_ge.iaddr + (uint32_t)i * 2);
            else if (f.idx == 3) k = (int)(mem_r32(g_ge.iaddr + (uint32_t)i * 4) & 0xFFFF);
            Vtx d;
            decode_vertex(&f, vbase + (uint32_t)k * step, &d);
            if (f.through) {
                if (d.x < x0) x0 = d.x; if (d.x > x1) x1 = d.x; if (d.y < y0) y0 = d.y; if (d.y > y1) y1 = d.y;
                nd++;
                continue;
            }
            if (d.w <= 0.f) { behind++; continue; }
            const float iw = 1.f / d.w, sx = 240.f + 240.f * d.x * iw, sy = 136.f - 136.f * d.y * iw;
            if (sx < x0) x0 = sx; if (sx > x1) x1 = sx; if (sy < y0) y0 = sy; if (sy > y1) y1 = sy;
            if (d.w < w0) w0 = d.w; if (d.w > w1) w1 = d.w;
            nd++;
        }
    }
    fprintf(stderr, "AL f=%u #%u fb=%08X T=%08X %dx%d tme=%d prim=%d n=%d vt=%06X idx=%d nm=%d thr=%d wt=%d bl=%d bm=%X atE=%d at=%X zt=%d zf=%d zwdis=%d fog=%d lt=%d clr=%d | box %.0f,%.0f,%.0f,%.0f w %.0f..%.0f behind=%d/%d\n",
            frame, idx, R(GE_FRAMEBUFPTR) & 0xFFFFFFu, addr, 1 << (R(GE_TEXSIZE0) & 15), 1 << (R(GE_TEXSIZE0) >> 8 & 15),
            (int)(R(GE_TEXTUREMAPENABLE) & 1), prim, count, vt, f.idx, f.nmorph, f.through, f.wt, (int)(R(GE_ALPHABLENDENABLE) & 1),
            R(GE_BLENDMODE) & 0xFFFFF, (int)(R(GE_ALPHATESTENABLE) & 1), R(GE_ALPHATEST) & 0xFFFFFF, (int)(R(GE_ZTESTENABLE) & 1),
            (int)(R(GE_ZTEST) & 7), (int)(R(GE_ZWRITEDISABLE) & 1), (int)(R(GE_FOGENABLE) & 1), (int)(R(GE_LIGHTINGENABLE) & 1),
            (int)(R(GE_CLEARMODE) & 1), (double)x0, (double)y0, (double)x1, (double)y1, (double)w0, (double)w1, behind, nd + behind);
}
#endif

#ifdef EXP_PUFFLOG
/* EXP_PUFFLOG: every draw of the soft cloud-puff texture at 040EE2A0 (64x64; PPSSPP's 04160670) from frame 4200 on, in Ominous
   Mist: PPSSPP draws pale puffs of mist round the fighters' feet all through a fight, and the other census probes saw this
   texture in no frame they looked at.  Per draw: state and up to four vertices on the screen (x, y, clip w, alpha, colour);
   PFN lines count the draws of the frame (one line per frame in which there is one). */
static void pufflog_note(int prim, int count) {
    extern uint32_t g_frames_presented;
    static uint32_t frame = ~0u, nframe;
    static int lines;
    if (frame != g_frames_presented) {
        if (nframe) fprintf(stderr, "PFN f=%u n=%u\n", frame, nframe);
        frame = g_frames_presented; nframe = 0;
    }
    if (frame < 4200 || !(R(GE_TEXTUREMAPENABLE) & 1)) return;
    const uint32_t vt = R(GE_VERTEXTYPE);
    const uint32_t addr = (R(GE_TEXADDR0) & 0xFFFFF0u) | ((R(GE_TEXBUFWIDTH0) << 8) & 0x0F000000u);
    if (addr != 0x040EE2A0u) return;
    nframe++;
    if (lines >= 6000) return;
    lines++;
    VFormat f;
    vformat(&f, vt);
#if defined(TKDR_WATCH) && defined(__wii__)
    {
        /* who wrote the vertices: the host return addresses of the stores into the batch's 256-byte blocks (resolve with
           powerpc-eabi-nm -n on the ELF) and the guest frame stamp of the newest one */
        extern uint32_t *g_wtp_ep, *g_wtp_ra, g_wtp_cur;
        const uint32_t off = g_ge.vaddr - GUEST_RAM_BASE;
        if (off < GUEST_RAM_SIZE && f.size) {
            const uint32_t b0 = off >> 8, b1 = (off + (uint32_t)(count * f.size) - 1) >> 8;
            uint32_t seen[4] = {0}, ns = 0;
            fprintf(stderr, "PW f=%u cur=%u va=%08X |", frame, g_wtp_cur, g_ge.vaddr);
            for (uint32_t b = b0; b <= b1 && ns < 4; b++) {
                if (!g_wtp_ep[b]) continue;
                uint32_t k = 0;
                while (k < ns && seen[k] != g_wtp_ra[b]) k++;
                if (k == ns) { seen[ns++] = g_wtp_ra[b]; fprintf(stderr, " ra=%08X(stamp %u)", g_wtp_ra[b], g_wtp_ep[b]); }
            }
            fprintf(stderr, "\n");
        }
    }
#endif
    fprintf(stderr, "PF f=%u prim=%d n=%d vt=%06X bl=%d bm=%X atE=%d at=%X zt=%d zwdis=%d fog=%d lt=%d |", frame, prim, count, vt,
            (int)(R(GE_ALPHABLENDENABLE) & 1), R(GE_BLENDMODE) & 0xFFFFF, (int)(R(GE_ALPHATESTENABLE) & 1), R(GE_ALPHATEST) & 0xFFFFFF,
            (int)(R(GE_ZTESTENABLE) & 1), (int)(R(GE_ZWRITEDISABLE) & 1), (int)(R(GE_FOGENABLE) & 1), (int)(R(GE_LIGHTINGENABLE) & 1));
    if (f.idx || f.nmorph != 1 || f.through || !guest_host_ptr(g_ge.vaddr, (uint32_t)f.size)) { fprintf(stderr, " (not decoded)\n"); return; }
    for (int k = 0; k < count && k < 4; k++) {
        Vtx d;
        decode_vertex(&f, g_ge.vaddr + (uint32_t)(k * f.size), &d);
        const float iw = d.w != 0.f ? 1.f / d.w : 0.f;
        fprintf(stderr, " %.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f", (double)(240.f + 240.f * d.x * iw), (double)(136.f - 136.f * d.y * iw), (double)d.w,
                (double)d.c[3], (double)d.c[0], (double)d.c[1], (double)d.c[2]);
    }
    fprintf(stderr, "\n");
}
#endif

#ifdef EXP_RIBBONLOG
/* EXP_RIBBONLOG: the ground-mist ribbons of Ominous Mist (the 128x64 cloud texture at 040D6BA0 drawn with fog on; the same texture
   without fog is the sky) in one frame in thirty from 4400 on: every vertex in clip space (x, y, z, w) with its alpha and colour,
   so that the part in front of the eye can be clipped and projected afterwards (the near plane is z > -w). */
static void ribbonlog_note(int prim, int count) {
    extern uint32_t g_frames_presented;
    static uint32_t frame = ~0u, idx;
    if (frame != g_frames_presented) { frame = g_frames_presented; idx = 0; }
    if (frame < 4400 || frame % 30 || !(R(GE_TEXTUREMAPENABLE) & 1) || !(R(GE_FOGENABLE) & 1)) return;
    const uint32_t vt = R(GE_VERTEXTYPE);
    if (vt >> 23 & 1) return;
    const uint32_t addr = (R(GE_TEXADDR0) & 0xFFFFF0u) | ((R(GE_TEXBUFWIDTH0) << 8) & 0x0F000000u);
    if (addr != 0x040D6BA0u) return;
    VFormat f;
    vformat(&f, vt);
    if (f.idx || f.nmorph != 1 || count > 16 || !guest_host_ptr(g_ge.vaddr, (uint32_t)f.size)) return;
    fprintf(stderr, "RIB f=%u #%u prim=%d n=%d vt=%06X |", frame, idx++, prim, count, vt);
    for (int k = 0; k < count; k++) {
        Vtx d;
        decode_vertex(&f, g_ge.vaddr + (uint32_t)(k * f.size), &d);
        fprintf(stderr, " %.1f,%.1f,%.1f,%.1f,%.0f,%.0f,%.0f,%.0f", (double)d.x, (double)d.y, (double)d.z, (double)d.w, (double)d.c[3], (double)d.c[0],
                (double)d.c[1], (double)d.c[2]);
    }
    fprintf(stderr, "\n");
}
#endif

#ifdef EXP_LOWLOG
/* EXP_LOWLOG: every 3D draw of frame 4400 (the camera stands still from 4360 to 4470 in the intro of Ominous Mist) that has a
   vertex in the bottom fifth of the screen (y > 200 of 272) in front of the eye, whatever its texture or blending: the order of
   the draw in the frame, the texture, the blending, depth and alpha test state, lighting, fog, vertex type, and the first
   vertices (screen x, y, clip w, colour, alpha).  The ground under the fighters' feet is the sum of these. */
static void lowlog_note(int prim, int count) {
    extern uint32_t g_frames_presented;
    static uint32_t frame = ~0u, idx;
    if (frame != g_frames_presented) { frame = g_frames_presented; idx = 0; }
    if (frame != 4400) return;
    idx++;
    const uint32_t vt = R(GE_VERTEXTYPE);
    if (vt >> 23 & 1) return;
    VFormat f;
    vformat(&f, vt);
    if (f.idx || f.nmorph != 1 || count < 3 || !guest_host_ptr(g_ge.vaddr, (uint32_t)f.size)) return;
    int nd = count < 8 ? count : 8, low = 0;
    Vtx v[8];
    for (int k = 0; k < nd; k++) {
        decode_vertex(&f, g_ge.vaddr + (uint32_t)(k * f.size), &v[k]);
        const float iw = v[k].w != 0.f ? 1.f / v[k].w : 0.f;
        if (v[k].w > 0.f && 136.f - 136.f * v[k].y * iw > 200.f) low = 1;
    }
    if (!low) return;
    const int tex = R(GE_TEXTUREMAPENABLE) & 1;
    const uint32_t addr = (R(GE_TEXADDR0) & 0xFFFFF0u) | ((R(GE_TEXBUFWIDTH0) << 8) & 0x0F000000u);
    fprintf(stderr, "LOW f=%u #%u prim=%d n=%d tex=%08X %dx%d fmt=%d on=%d | blend=%d bm=%X atE=%d at=%X zt=%d zf=%d zwdis=%d lt=%d mu=%X fog=%d cull=%d func=%X vt=%06X |",
            frame, idx, prim, count, tex ? addr : 0, tex ? 1 << (R(GE_TEXSIZE0) & 15) : 0, tex ? 1 << (R(GE_TEXSIZE0) >> 8 & 15) : 0, (int)(R(GE_TEXFORMAT) & 15), tex,
            (int)(R(GE_ALPHABLENDENABLE) & 1), R(GE_BLENDMODE) & 0xFFFFF, (int)(R(GE_ALPHATESTENABLE) & 1), R(GE_ALPHATEST) & 0xFFFFFF, (int)(R(GE_ZTESTENABLE) & 1),
            (int)(R(GE_ZTEST) & 7), (int)(R(GE_ZWRITEDISABLE) & 1), (int)(R(GE_LIGHTINGENABLE) & 1), R(GE_MATERIALUPDATE) & 7, (int)(R(GE_FOGENABLE) & 1),
            (int)(R(GE_CULLFACEENABLE) & 1), R(GE_TEXFUNC) & 0xFFFFF, vt);
    for (int k = 0; k < nd && k < 4; k++) {
        const float iw = v[k].w != 0.f ? 1.f / v[k].w : 0.f;
        fprintf(stderr, " %.0f,%.0f,w%.0f,c%.0f/%.0f/%.0f/%.0f", (double)(240.f + 240.f * v[k].x * iw), (double)(136.f - 136.f * v[k].y * iw), (double)v[k].w,
                (double)v[k].c[0], (double)v[k].c[1], (double)v[k].c[2], (double)v[k].c[3]);
    }
    fprintf(stderr, "\n");
}
#endif

#ifdef EXP_FOGDBG
/* Is Ominous Mist's fog on the screen in the fight, and does the depth test hide it?  From frame 4500 on, in windows
   of 360 frames, the draws of its two sheet textures run (0) as the game gives them, and as opaque white sheets (no
   blending, no alpha test, the depth test kept) with the chip's depth moved toward the eye by (1) 0, (2) 1.5, (3) 6,
   (4) 24, (5) 96 steps of the PSP's 16 bits.  The opaque sheets show where they pass the depth test.  FOGMODE lines
   say when a window starts. */
extern float g_zbias_q;
int g_fd_zb;
static unsigned s_fd_sv[2];
static int fogdbg_pre(void) {
    extern uint32_t g_frames_presented;
    static const float q[6] = {0.f, 0.f, 1.5f, 6.f, 24.f, 96.f};
    const int m = g_frames_presented < 4500 ? 0 : (int)((g_frames_presented - 4500) / 360 % 6);
    static int last = -1;
    if (m != last) { last = m; g_zbias_q = q[m]; fprintf(stderr, "FOGMODE f=%u mode=%d q=%g\n", g_frames_presented, m, (double)q[m]); }
    if (!m || !(R(GE_TEXTUREMAPENABLE) & 1) || (R(GE_VERTEXTYPE) >> 23 & 1)) return 0;
    const uint32_t addr = (R(GE_TEXADDR0) & 0xFFFFF0u) | ((R(GE_TEXBUFWIDTH0) << 8) & 0x0F000000u);
    if (addr != 0x040D8260u && addr != 0x040D7DA0u) return 0;
    s_fd_sv[0] = R(GE_ALPHABLENDENABLE); s_fd_sv[1] = R(GE_ALPHATESTENABLE);
    R(GE_ALPHABLENDENABLE) = 0; R(GE_ALPHATESTENABLE) = 0;
    g_fd_zb = 1;
    g_ge_pix_gen++;
    return 1;
}
static void fogdbg_post(void) {
    R(GE_ALPHABLENDENABLE) = s_fd_sv[0]; R(GE_ALPHATESTENABLE) = s_fd_sv[1];
    g_fd_zb = 0;
    g_ge_pix_gen++;
}
#endif

#ifdef EXP_ZBQCYCLE
/* The depth offset of the blended layers (EXP_ZBIAS, steps of the PSP's 16 bits) cycles: from frame 4200 on, windows of
   240 frames at 0, 4, 12, 24, 48 and 96 steps.  ZBQ lines say when a window starts (shots are paired with them by the
   log's clock). */
extern float g_zbias_q;
static void zbq_cycle(void) {
    extern uint32_t g_frames_presented;
    static const float q[6] = {0.f, 4.f, 12.f, 24.f, 48.f, 96.f};
    const int m = g_frames_presented < 4200 ? 0 : (int)((g_frames_presented - 4200) / 240 % 6);
    static int last = -1;
    if (m != last) { last = m; g_zbias_q = q[m]; fprintf(stderr, "ZBQ f=%u mode=%d q=%g\n", g_frames_presented, m, (double)q[m]); }
}
#endif

#ifdef EXP_SLOW2D
static uint32_t s2d_state_t, s2d_gx_t;   /* ticks of the current draw spent rebuilding the pixel state, and sending it to the chip */
static void slow2d_note(int prim, int count, uint32_t ticks);
#endif
void gpu_draw_prim(int prim, int count) {
    g_raster_draws++;
#ifdef EXP_SLOW2D
    s2d_state_t = s2d_gx_t = 0;
#endif
#ifdef EXP_ZBQCYCLE
    zbq_cycle();
#endif
#ifdef EXP_BCEN
    bcen_note(prim, count);
#endif
#ifdef EXP_NEARLOG
    nearlog_note(prim, count);
#endif
#ifdef EXP_CARDTRACK
    cardtrack_note(prim, count);
#endif
#ifdef EXP_LOWLOG
    lowlog_note(prim, count);
#endif
#ifdef EXP_RIBBONLOG
    ribbonlog_note(prim, count);
#endif
#ifdef EXP_BLENDLOG
    blendlog_note(prim, count);
#endif
#ifdef EXP_ALLLOG
    alllog_note(prim, count);
#endif
#ifdef EXP_PUFFLOG
    pufflog_note(prim, count);
    {
        /* every sprite batch (prim 6) that is not in through mode, whatever its texture, in frames 4400-6600: PS lines */
        extern uint32_t g_frames_presented;
        static int ps_lines;
        if (prim == 6 && !(R(GE_VERTEXTYPE) >> 23 & 1) && g_frames_presented >= 4400 && g_frames_presented < 6600 && ps_lines < 20000) {
            ps_lines++;
            fprintf(stderr, "PS f=%u T=%08X %dx%d tme=%d n=%d vt=%06X bm=%X\n", g_frames_presented,
                    (R(GE_TEXADDR0) & 0xFFFFF0u) | ((R(GE_TEXBUFWIDTH0) << 8) & 0x0F000000u), 1 << (R(GE_TEXSIZE0) & 15), 1 << (R(GE_TEXSIZE0) >> 8 & 15),
                    (int)(R(GE_TEXTUREMAPENABLE) & 1), count, R(GE_VERTEXTYPE), R(GE_BLENDMODE) & 0xFFFFF);
        }
    }
#endif
#ifdef EXP_FOGDBG
    const int fogdbg = fogdbg_pre();
#endif
#ifdef TKDR_AUTOPILOT
    DC_T(dpg0);
    g_dp_cat = 5;
    if ((R(GE_VERTEXTYPE) >> 9 & 3) != 0) cam_sample();
#endif
#ifdef HLE_TRACE
    uint64_t t0 = hle_time_us();
#endif
#ifdef EXP_FOGPROBE
    FogCap fog_cap;
    int fog_fast = 0;
    const int fog_on = fog_pre(count, &fog_cap);
#endif
#ifdef __wii__
#ifdef EXP_FOGPROBE
    if (STENCIL_DROP_NOW() || !(fog_fast = draw_prim_fast(prim, count)))
#else
    if (STENCIL_DROP_NOW() || !draw_prim_fast(prim, count))
#endif
#endif
        draw_prim(prim, count);
#ifdef EXP_FOGPROBE
    if (fog_on) fog_post(prim, count, &fog_cap, fog_fast);
#endif
#ifdef EXP_FOGDBG
    if (fogdbg) fogdbg_post();
#endif
#ifdef HLE_TRACE
    g_prim_us += hle_time_us() - t0;
#endif
#ifdef TKDR_AUTOPILOT
    DC_T(dpg1);
    g_cat_ticks[g_dp_cat] += (uint32_t)(dpg1 - dpg0);
    g_cat_n[g_dp_cat]++;
#ifdef EXP_SLOW2D
    if (g_dp_cat == 5) slow2d_note(prim, count, (uint32_t)(dpg1 - dpg0));
#endif
    if (g_dp_cat == 1) {
        s_runlen++;
    } else if (s_runlen) {
        int b = s_runlen >= 64 ? 6 : s_runlen >= 32 ? 5 : s_runlen >= 16 ? 4 : s_runlen >= 8 ? 3 : s_runlen >= 4 ? 2 : s_runlen >= 2 ? 1 : 0;
        g_run_n[b]++;
        g_run_draws[b] += s_runlen;
        s_runlen = 0;
    }
#endif
}

#ifdef HLE_TRACE
/* Vertices and primitives per vertex type: which formats a fast decode path
   has to cover. */
static struct { uint32_t vt; uint64_t verts, prims; } g_vt_hist[32];
static int g_vt_n;
static void vt_note(uint32_t vt, int count) {
    for (int i = 0; i < g_vt_n; i++)
        if (g_vt_hist[i].vt == vt) { g_vt_hist[i].verts += (uint64_t)count; g_vt_hist[i].prims++; return; }
    if (g_vt_n < 32) {
        g_vt_hist[g_vt_n].vt = vt;
        g_vt_hist[g_vt_n].verts = (uint64_t)count;
        g_vt_hist[g_vt_n].prims = 1;
        g_vt_n++;
    }
}
void gpu_dump_vformats(void) {
    for (int i = 0; i < g_vt_n; i++) {
        VFormat f;
        vformat(&f, g_vt_hist[i].vt);
        fprintf(stderr, "VFMT vt=%06X tc=%d col=%d nrm=%d pos=%d wt=%d nw=%d morph=%d idx=%d through=%d size=%d "
                "verts=%llu prims=%llu\n", g_vt_hist[i].vt, f.tc, f.col, f.nrm, f.pos, f.wt, f.nw, f.nmorph,
                f.idx, f.through, f.size, (unsigned long long)g_vt_hist[i].verts,
                (unsigned long long)g_vt_hist[i].prims);
    }
    fprintf(stderr, "LIGHTFAST checks=%llu mismatches=%llu\n", (unsigned long long)g_lightfast_checks,
            (unsigned long long)g_lightfast_bad);
    fprintf(stderr, "VTXFAST unlit=%llu skinned=%llu lit-nonormal=%llu checks=%llu mismatches=%llu\n",
            (unsigned long long)g_vtxfast_hits[0], (unsigned long long)g_vtxfast_hits[1],
            (unsigned long long)g_vtxfast_hits[2],
            (unsigned long long)g_vtxfast_checks, (unsigned long long)g_vtxfast_bad);
}
#endif

/* Every draw of the small display lists (fewer than 40 draws: transition
   and card screens), printed at the end of the list.  An effect screen can
   then be read off the log draw by draw. */
#define LT_MAX 40
static struct {
    int prim, count, through, clear, tex, fmt, w, h, blend, src, dst, at, atf, zt, zf, zw;
    uint32_t addr, fixa, fixb, mask, fb;
    float x, y, z, a, r;
} g_lt[LT_MAX];
static int g_lt_n, g_lt_over;
static uint32_t g_lt_list, g_lt_lines;
static void list_trace_flush(void) {
    extern uint32_t g_frames_presented;
    /* Only a list whose structure differs from the recent ones: menus
       repeat the same list every frame. */
    uint32_t sig = 2166136261u;
    for (int i = 0; i < g_lt_n; i++) {
        uint32_t k[6] = {(uint32_t)g_lt[i].prim, (uint32_t)g_lt[i].through | (uint32_t)g_lt[i].tex << 1,
                         g_lt[i].addr, (uint32_t)g_lt[i].src | (uint32_t)g_lt[i].dst << 4 | (uint32_t)g_lt[i].blend << 8,
                         g_lt[i].fb, (uint32_t)g_lt[i].zt | (uint32_t)g_lt[i].zw << 1 | (uint32_t)g_lt[i].clear << 2};
        for (int j = 0; j < 6; j++) sig = (sig ^ k[j]) * 16777619u;
    }
    static uint32_t recent[8];
    for (int i = 0; i < 8; i++)
        if (recent[i] == sig) { g_lt_n = 0; g_lt_over = 0; return; }
    static int rp;
    recent[rp++ & 7] = sig;
    if (g_lt_n && !g_lt_over && g_lt_lines < 4000) {
        fprintf(stderr, "LIST guest=%u draws=%d\n", g_frames_presented, g_lt_n);
        for (int i = 0; i < g_lt_n; i++, g_lt_lines++)
            fprintf(stderr, "  D%d prim=%d n=%d thr=%d clr=%d tex=%d %08X fmt=%d %dx%d bl=%d %d/%d fix=%06X/%06X "
                    "at=%d/%d z=%d/%d/%d mask=%08X fb=%08X v0=(%.1f,%.1f,%.1f) a=%.0f r=%.0f\n",
                    i, g_lt[i].prim, g_lt[i].count, g_lt[i].through, g_lt[i].clear, g_lt[i].tex, g_lt[i].addr,
                    g_lt[i].fmt, g_lt[i].w, g_lt[i].h, g_lt[i].blend, g_lt[i].src, g_lt[i].dst, g_lt[i].fixa,
                    g_lt[i].fixb, g_lt[i].at, g_lt[i].atf, g_lt[i].zt, g_lt[i].zf, g_lt[i].zw, g_lt[i].mask,
                    g_lt[i].fb, g_lt[i].x, g_lt[i].y, g_lt[i].z, g_lt[i].a, g_lt[i].r);
    }
    g_lt_n = 0;
    g_lt_over = 0;
}
static void list_trace(const PixState *ps, const VFormat *f, int prim, int count, const Vtx *v) {
    if (g_lt_list != g_tex_list) {   // a new display list began
        list_trace_flush();
        g_lt_list = g_tex_list;
    }
    if (g_lt_n >= LT_MAX) { g_lt_over = 1; return; }
    typeof(g_lt[0]) *e = &g_lt[g_lt_n++];
    e->prim = prim; e->count = count; e->through = f->through; e->clear = ps->clear;
    e->tex = ps->tex.enabled; e->addr = ps->tex.enabled ? ps->tex.addr : 0;
    e->fmt = ps->tex.enabled ? ps->tex.fmt : -1; e->w = ps->tex.enabled ? ps->tex.w : 0;
    e->h = ps->tex.enabled ? ps->tex.h : 0; e->blend = ps->blend; e->src = ps->bl_src; e->dst = ps->bl_dst;
    e->fixa = ps->fix_a & 0xFFFFFFu; e->fixb = ps->fix_b & 0xFFFFFFu; e->at = ps->alpha_test;
    e->atf = ps->at_func; e->zt = ps->z_test; e->zf = ps->z_func; e->zw = ps->z_write;
    e->mask = ps->write_mask; e->fb = ps->tg.fb;
    e->x = v[0].x; e->y = v[0].y; e->z = v[0].z; e->a = v[0].c[3]; e->r = v[0].c[0];
}

#ifdef TKDR_AUTOPILOT
/* Micro-benchmark of the vertex decoders on synthetic fight-like data (run at
   boot, before the guest starts): microseconds per vertex. */
void gpu_vertex_bench(void) {
    const uint32_t base = 0x09F00000u;
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < 64 * 1024; i++) {
        seed = seed * 1664525u + 1013904223u;
        mem_w8(base + i, (uint8_t)(seed >> 24));
    }
    float one[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0.5f, 0.25f, 0.f};
    memcpy(g_ge.world, one, sizeof one);
    for (int b = 0; b < 8; b++) memcpy(g_ge.bone[b], one, sizeof one);
    R(GE_LIGHTINGENABLE) = 1;
    for (int l = 0; l < 3; l++) {
        float d[3] = {0.3f * (float)(l + 1), 0.7f, -0.5f};
        R(GE_LIGHTENABLE0 + l) = 1;
        R(GE_LIGHTTYPE0 + l) = 1;   // directional, ambient/diffuse/specular
        for (int i = 0; i < 3; i++) { uint32_t u; memcpy(&u, &d[i], 4); R(GE_LX0 + 3 * l + i) = u >> 8; }
        R(GE_LAC0 + 3 * l) = 0x404040; R(GE_LAC0 + 3 * l + 1) = 0xFFFFFF; R(GE_LAC0 + 3 * l + 2) = 0;
    }
    R(GE_MATERIALAMBIENT) = 0xFFFFFF; R(GE_MATERIALDIFFUSE) = 0xFFFFFF; R(GE_MATERIALALPHA) = 0xFF;
    R(GE_AMBIENTCOLOR) = 0x808080; R(GE_AMBIENTALPHA) = 0xFF;
    g_ge_light_gen++; g_ge_vertex_gen++;
    g_gxr_object = 1;
    static const uint32_t vts[] = {0x000321, 0x004323, 0x008323, 0x00C323, 0x018323, 0x000113};
    static const char *const names[] = {"skin nw1", "skin nw2", "skin nw3", "skin nw4", "skin nw7", "stage lit"};
    for (int lit = 1; lit >= 0; lit--) {
        R(GE_LIGHTINGENABLE) = (uint32_t)lit;
        g_ge_light_gen++; g_ge_vertex_gen++;
        for (unsigned t = 0; t < sizeof vts / sizeof vts[0]; t++) {
            VFormat f;
            vformat(&f, vts[t]);
            int n = 2048 / f.size;
            if (n > 600) n = 600;
            uint32_t t0 = dp_tb();
            for (int rep = 0; rep < 8; rep++)
                for (int i = 0; i < n; i++) decode_vertex(&f, base + (uint32_t)(i * f.size), &g_verts[i]);
            uint32_t dt = dp_tb() - t0;
            fprintf(stderr, "VBENCH %-10s lit=%d size=%d %.3f us/vertex\n", names[t], lit, f.size,
                    (double)dt / 60.75 / (8.0 * n));
        }
    }
    g_gxr_object = 0;
}
#endif

/* The vertex format and the pixel state of the last draw, kept until what
   they are made from changes (shared by the fast path below). */
static VFormat s_vf;
static uint32_t s_vt = 0xFFFFFFFFu;
static PixState s_ps;
#ifdef EXP_SLOW2D
/* EXP_SLOW2D: the draws of the "2D/sw" category (everything that is neither a skinned nor a cached plain draw), summed by
   texture (address, size, format) and printed every 300 presented frames, the six that took the longest: how many draws,
   how long they took in all, and how much of that was the pixel state (the texture lookup and decode) and how much sending
   them to the chip.  A single draw of more than 2 ms is printed at once. */
static void slow2d_note(int prim, int count, uint32_t ticks) {
    extern uint32_t g_frames_presented;
    typedef struct { uint32_t addr, wh, fmt, n, verts; uint64_t t, st, gx; } K;
    static K tab[24];
    static int nk;
    static uint32_t next_flush = 300, lines;
    const PixState *p = &s_ps;
    const uint32_t wh = (uint32_t)p->tex.w << 16 | (uint32_t)p->tex.h, fmt = p->tex.enabled ? (uint32_t)p->tex.fmt : 99u;
    const uint32_t addr = p->tex.enabled ? p->tex.addr : 0u;
    if (ticks > 121500u && lines++ < 200)
        fprintf(stderr, "SLOW2D f=%u %u us (state %u, chip %u) prim=%d n=%d vt=%06X through=%d tex=%d %dx%d fmt=%d addr=%08X serial=%u blend=%d src=%d dst=%d at=%d z=%d/%d fb=%08X\n",
                g_frames_presented, ticks * 100u / 6075u, s2d_state_t * 100u / 6075u, s2d_gx_t * 100u / 6075u, prim, count, R(GE_VERTEXTYPE),
                p->through, p->tex.enabled, p->tex.w, p->tex.h, p->tex.fmt, p->tex.addr, p->tex.serial, p->blend, p->bl_src, p->bl_dst,
                p->alpha_test, p->z_test, p->z_write, p->tg.fb);
    int i = 0;
    while (i < nk && !(tab[i].addr == addr && tab[i].wh == wh && tab[i].fmt == fmt)) i++;
    if (i == nk) {
        if (nk < 24) { memset(&tab[nk], 0, sizeof tab[nk]); tab[nk].addr = addr; tab[nk].wh = wh; tab[nk].fmt = fmt; nk++; }
        else i = 23;
    }
    tab[i].n++; tab[i].verts += (uint32_t)count; tab[i].t += ticks; tab[i].st += s2d_state_t; tab[i].gx += s2d_gx_t;
    if (g_frames_presented >= next_flush) {
        next_flush = g_frames_presented + 300;
        for (int r = 0; r < 6; r++) {
            int best = -1;
            for (int k = 0; k < nk; k++) if (tab[k].n && (best < 0 || tab[k].t > tab[best].t)) best = k;
            if (best < 0) break;
            fprintf(stderr, "S2D f=%u tex %08X %ux%u fmt=%u: %u draws %u verts, %.2f ms (state %.2f, chip %.2f) per 300 frames\n", g_frames_presented,
                    tab[best].addr, tab[best].wh >> 16, tab[best].wh & 0xFFFFu, tab[best].fmt, tab[best].n, tab[best].verts,
                    (double)tab[best].t / 60750.0, (double)tab[best].st / 60750.0, (double)tab[best].gx / 60750.0);
            tab[best].n = 0;
        }
        nk = 0;
    }
}
#endif
static uint32_t s_ps_gen;
static int s_ps_through = -1, s_ps_epoch;

#ifdef __wii__
/* What the fast path found out about the registers as they are now: whether it
   takes draws in this state, and everything it worked out for them (the vertex
   format, the pixel state, the cache key's register parts, the matrices).  A
   run of draws mostly changes nothing but the vertex address, so the state is
   looked at again only when a register generation, the vertex type, the
   texture pack or the display buffers moved. */
static struct {
    uint32_t sum, sum_nv, vt, frames;   /* sum_nv: the sum without the vertex generation (see hot_quick) */
    int epoch;
    int ok;               /* 0: this state is one the fast path refuses */
    int wt;               /* skinned fighter (else a plain draw through the draw cache) */
    int obj;              /* plain draws: the vertices are kept in object space */
    uint32_t key_vt;      /* the vertex type as the draw cache keys it */
    const float *mv, *uvt;
    uint32_t setup_ep;    /* g_gx_epoch after the chip was last set up for this state (0: not yet) */
    VcRegs regs;
    SkinPrep sk;
    uint32_t sk_light;    /* the light generation sk was worked out for (sk_ok: it is still all there) */
    int sk_ok;
} s_hot;

/* A plain draw that is not in the draw cache, decoded as a whole into the
   chip's vertices and stored for the next frame.  (The guest rewrites even
   static vertices every frame, with the same bytes, so a write stamp cannot say
   which draws will repeat; the hash of the bytes can.) */
#ifdef TKDR_FINEPROF
uint64_t g_pm_ticks[3], g_pm_n, g_pm_verts;   /* plain_miss: decoding, the cache store, packing; calls; vertices */
#endif
/* A draw the cache holds, of a format without normals, lit, that was stored under another global ambient alpha (ga): the
   only thing the alpha reaches is the alpha byte of each vertex, which light_registers() / light_general() make as
   (material or vertex alpha) * ga / 255 and PACK8 rounds, so the byte is (2 * a * ga + 255) / 510 (a * ga / 255 is never
   a half, whatever float error the product has; the self-test compares all 65536 pairs).  The vertex's own alpha counts
   when the material update takes the ambient material from the vertex colour (MATERIALUPDATE bit 0) and the format has one.
   Done here instead of a miss (a decode, a store and a pack: 5-6 us) when only a fading stage's ambient alpha moved. */
static inline uint32_t colour_alpha(const uint8_t *p, int fmt) {   /* rd_color_fast()'s alpha of a colour of kind 4-7 */
    if (fmt == 7) return p[3];
    const uint32_t c = ld16le(p);
    return fmt == 4 ? 255u : fmt == 5 ? (c >> 15 ? 255u : 0u) : (c >> 12) * 17u;
}
static void vc_alpha_refresh(const VFormat *f, const uint8_t *vp, int count, GxVtx *v, uint32_t ga) {
    if (f->col >= 4 && (R(GE_MATERIALUPDATE) & 1)) {
        for (int i = 0; i < count; i++) {
            const uint32_t a = colour_alpha(vp + (size_t)i * (size_t)f->size + (size_t)f->off_col, f->col);
            v[i].rgba = (v[i].rgba & ~0xFFu) | (2u * a * ga + 255u) / 510u;
        }
    } else {
        const uint32_t a = (2u * (R(GE_MATERIALALPHA) & 0xFFu) * ga + 255u) / 510u;
        for (int i = 0; i < count; i++) v[i].rgba = (v[i].rgba & ~0xFFu) | a;
    }
}

static __attribute__((noinline)) const GxVtx *plain_miss(const VFormat *f, uint32_t vbase, int count, int obj,
                                                         const VcTok *tok) {
#ifdef TKDR_FINEPROF
    DP_T(pm0);
#endif
    g_gxr_object = 1;
    g_vc_objspace = obj;
    g_vc_rawuv = 1;
    for (int i = 0; i < count; i++) decode_vertex(f, vbase + (uint32_t)(i * f->size), &g_verts[i]);
    g_vc_objspace = 0;
    g_vc_rawuv = 0;
    g_gxr_object = 0;
#ifdef TKDR_FINEPROF
    DP_T(pm1);
#endif
    GxVtx *dst = vc_store(tok);
#ifdef TKDR_FINEPROF
    DP_T(pm2);
#endif
    for (int i = 0; i < count; i++) gxr_pack(&dst[i], &g_verts[i]);
#ifdef TKDR_FINEPROF
    {
        DP_T(pm3);
        g_pm_ticks[0] += (uint32_t)(pm1 - pm0); g_pm_ticks[1] += (uint32_t)(pm2 - pm1); g_pm_ticks[2] += (uint32_t)(pm3 - pm2);
        g_pm_n++; g_pm_verts += (uint32_t)count;
    }
#endif
    return dst;
}

#ifdef EXP_HITCHK
/* Debug: the geometry the fast path sends against a fresh decode.  Every
   eighth draw-cache hit is decoded again from the guest's bytes and the two
   compared; every plain draw's model-view matrix is multiplied out again and
   compared with the cached one.  Counters are printed with the periodic report. */
uint32_t g_chk_hit_n, g_chk_hit_bad, g_chk_mv_n, g_chk_mv_bad, g_chk_gx_n, g_chk_gx_bad;
static void hit_check(const VFormat *f, uint32_t vbase, int count, int obj, const GxVtx *got) {
    static GxVtx ref[32];
    static uint32_t calls;
    if ((calls++ & 7) || count > 32) return;
    g_gxr_object = 1;
    g_vc_objspace = obj;
    g_vc_rawuv = 1;
    for (int i = 0; i < count; i++) decode_vertex(f, vbase + (uint32_t)(i * f->size), &g_verts[i]);
    g_vc_objspace = 0;
    g_vc_rawuv = 0;
    g_gxr_object = 0;
    for (int i = 0; i < count; i++) gxr_pack(&ref[i], &g_verts[i]);
    g_chk_hit_n++;
    for (int i = 0; i < count; i++) {
        const float a[5] = {got[i].x, got[i].y, got[i].z, got[i].u, got[i].v}, b[5] = {ref[i].x, ref[i].y, ref[i].z, ref[i].u, ref[i].v};
        int bad = got[i].rgba != ref[i].rgba;
        for (int j = 0; j < 5; j++) {
            float m = fabsf(b[j]) > 1.f ? fabsf(b[j]) : 1.f;
            if (!(fabsf(a[j] - b[j]) <= 1e-5f * m)) bad = 1;
        }
        if (bad) {
            if (++g_chk_hit_bad <= 8)
                fprintf(stderr, "HITCHK MISMATCH v%d/%d vt=%06X obj=%d: cached (%g %g %g | %g %g | %08X) fresh (%g %g %g | %g %g | %08X)\n", i, count, f->vt, obj,
                        (double)a[0], (double)a[1], (double)a[2], (double)a[3], (double)a[4], got[i].rgba, (double)b[0], (double)b[1],
                        (double)b[2], (double)b[3], (double)b[4], ref[i].rgba);
            return;
        }
    }
}
static void mv_check(const float *mv) {
    const float *w = g_ge.world, *v = g_ge.view;
    float r[12];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[i * 3 + j] = w[i * 3] * v[j] + w[i * 3 + 1] * v[3 + j] + w[i * 3 + 2] * v[6 + j];
    for (int j = 0; j < 3; j++) r[9 + j] = w[9] * v[j] + w[10] * v[3 + j] + w[11] * v[6 + j] + v[9 + j];
    g_chk_mv_n++;
    for (int i = 0; i < 12; i++)
        if (memcmp(&r[i], &mv[i], 4) && fabsf(r[i] - mv[i]) > 1e-6f * (fabsf(r[i]) + 1.f)) {
            if (++g_chk_mv_bad <= 4) fprintf(stderr, "HITCHK MV STALE [%d] cached %g fresh %g\n", i, (double)mv[i], (double)r[i]);
            return;
        }
}
/* The skinned state the fast path keeps against one worked out from scratch. */
uint32_t g_chk_sk_n, g_chk_sk_bad;
static void skin_state_check(const VFormat *f) {
    static uint32_t calls;
    if (calls++ & 7) return;
    SkinPrep fresh;
    g_gxr_object = 1;
    int ok = skin_prepare(f, &fresh);
    g_gxr_object = 0;
    const SkinPrep *h = &s_hot.sk;
    g_chk_sk_n++;
    int bad = !ok || fresh.mode != h->mode || fresh.rgba_const != h->rgba_const || fresh.alpha != h->alpha || fresh.ps_ok != h->ps_ok ||
              memcmp(&fresh.su, &h->su, 16);
    if (!bad && fresh.ps_ok)
        bad = memcmp((const char *)&fresh.ps + 8, (const char *)&h->ps + 8, 24) ||
              memcmp((const char *)&fresh.ps + 48, (const char *)&h->ps + 48, 324 - 48) || fresh.ps_kind != h->ps_kind;
    if (bad && ++g_chk_sk_bad <= 4)
        fprintf(stderr, "HITCHK SKIN STATE STALE mode %d/%d alpha %u/%u const %08X/%08X su %g/%g ps %d/%d\n", fresh.mode, h->mode, fresh.alpha, h->alpha,
                fresh.rgba_const, h->rgba_const, (double)fresh.su, (double)h->su, fresh.ps_ok, h->ps_ok);
}
/* The pixel state the fast path draws with against one built from the registers now. */
uint32_t g_chk_ps_n, g_chk_ps_bad;
static void ps_state_check(void) {
    static uint32_t calls;
    if (calls++ & 3) return;
    PixState fresh;
    pix_state(&fresh, 0);
    const PixState *h = &s_ps;
    g_chk_ps_n++;
    int bad = 0;
    bad |= (fresh.tg.fb != h->tg.fb || fresh.tg.zb != h->tg.zb || fresh.tg.fb_fmt != h->tg.fb_fmt || fresh.tg.sx1 != h->tg.sx1 ||
            fresh.tg.sy1 != h->tg.sy1 || fresh.tg.sx2 != h->tg.sx2 || fresh.tg.sy2 != h->tg.sy2) << 0;
    bad |= (fresh.tex.enabled != h->tex.enabled || fresh.tex.fmt != h->tex.fmt || fresh.tex.w != h->tex.w || fresh.tex.h != h->tex.h ||
            fresh.tex.addr != h->tex.addr || fresh.tex.stride != h->tex.stride || fresh.tex.swizzle != h->tex.swizzle) << 1;
    bad |= (fresh.tex.func != h->tex.func || fresh.tex.rgba != h->tex.rgba || fresh.tex.dbl != h->tex.dbl || fresh.tex.linear != h->tex.linear ||
            fresh.tex.clamp_u != h->tex.clamp_u || fresh.tex.clamp_v != h->tex.clamp_v || memcmp(fresh.tex.env, h->tex.env, sizeof fresh.tex.env)) << 2;
    bad |= (fresh.tex.clut_fmt != h->tex.clut_fmt || fresh.tex.clut_shift != h->tex.clut_shift || fresh.tex.clut_mask != h->tex.clut_mask ||
            fresh.tex.clut_off != h->tex.clut_off) << 3;
    bad |= (memcmp(&fresh.clear, &h->clear, offsetof(PixState, through) + sizeof fresh.through - offsetof(PixState, clear)) != 0) << 4;
    bad |= (fresh.tex.enabled && fresh.tex.entry != h->tex.entry) << 5;
    bad |= (fresh.tex.enabled && fresh.tex.serial != h->tex.serial) << 6;
    if (bad && ++g_chk_ps_bad <= 6)
        fprintf(stderr, "HITCHK PIXEL STATE STALE bits %02X: z %d/%d/%d vs %d/%d/%d, at %d/%d/%d vs %d/%d/%d, blend %d/%d/%d vs %d/%d/%d, tex %08X vs %08X, serial %u vs %u\n", bad,
                h->z_test, h->z_func, h->z_write, fresh.z_test, fresh.z_func, fresh.z_write, h->alpha_test, h->at_func, h->at_ref, fresh.alpha_test,
                fresh.at_func, fresh.at_ref, h->blend, h->bl_src, h->bl_dst, fresh.blend, fresh.bl_src, fresh.bl_dst, h->tex.addr, fresh.tex.addr,
                h->tex.serial, fresh.tex.serial);
}
#define HITCHK_HIT(f_, vb_, n_, obj_, out_) hit_check((f_), (vb_), (n_), (obj_), (out_))
#define HITCHK_MV(mv_) mv_check(mv_)
#define HITCHK_SKIN(f_) (skin_state_check(f_), ps_state_check())
#else
#define HITCHK_HIT(f_, vb_, n_, obj_, out_) ((void)0)
#define HITCHK_MV(mv_) ((void)0)
#define HITCHK_SKIN(f_) ((void)0)
#endif

#if defined(TKDR_WATCH) && defined(__wii__)
/* Who wrote the vertices of a plain draw, and when: counted by whether the
   draw's own bytes (256-byte blocks), the 1 KiB or the 4 KiB around them were
   stored to since the start of the last guest frame, and whether the draw was
   then a cache hit (the bytes are what they were).  Dirty and hit means the
   guest rewrote them with the same values. */
uint32_t g_wtp_cnt[2][2][2][2];   /* [own blocks dirty][1 KiB dirty][4 KiB dirty][cache hit] */
struct { uint32_t ra, n; } g_wtp_w[48];
static void wtp_probe(uint32_t vbase, uint32_t bytes, int hit) {
    extern uint32_t *g_wtp_ep, *g_wtp_ra, g_wtp_prev;
    uint32_t off = vbase - GUEST_RAM_BASE;
    if (off >= GUEST_RAM_SIZE) return;
    uint32_t b0 = off >> 8, b1 = (off + bytes - 1) >> 8, newest = 0, ra = 0;
    int d256 = 0, d1k = 0, d4k = 0;
    for (uint32_t b = b0; b <= b1; b++)
        if (g_wtp_ep[b] >= g_wtp_prev && g_wtp_ep[b] > newest) { d256 = 1; newest = g_wtp_ep[b]; ra = g_wtp_ra[b]; }
    for (uint32_t b = (b0 >> 2) << 2; b <= (b1 | 3u); b++) if (g_wtp_ep[b] >= g_wtp_prev) { d1k = 1; break; }
    for (uint32_t b = (b0 >> 4) << 4; b <= (b1 | 15u); b++) if (g_wtp_ep[b] >= g_wtp_prev) { d4k = 1; break; }
    g_wtp_cnt[d256][d1k][d4k][hit]++;
    if (d256) {
        int k;
        for (k = 0; k < 47 && g_wtp_w[k].n && g_wtp_w[k].ra != ra; k++) {}
        g_wtp_w[k].ra = ra;
        g_wtp_w[k].n++;
    }
}
#define WTP_PROBE(a_, b_, h_) wtp_probe((a_), (b_), (h_))
#else
#define WTP_PROBE(a_, b_, h_) ((void)0)
#endif

static inline uint32_t hot_sum_nv(void) {
    /* A skinned decode reads the world matrix (and the bones) when it runs, so
       its state does not go stale with them. */
#ifdef EXP_OLDGEN
    return g_ge_pix_gen + g_ge_light_gen + g_ge_view_gen + g_ge_world_gen;
#else
    return g_ge_pix_gen + g_ge_light_gen + g_ge_view_gen + (s_hot.wt ? 0 : g_ge_world_gen);
#endif
}
static inline uint32_t hot_sum(void) { return hot_sum_nv() + g_ge_vertex_gen; }

int g_hot_plain;   /* the state the fast path is in takes plain draws (a hint for the list reader's prefetch) */

/* The vertex generation moves with every texture scale or offset a fighter's
   parts set (800 times a frame), and for a skinned draw it changes nothing but
   those four numbers: the rest of its state (format, pixel state, lights) is
   current, so only they are redone. */
extern uint32_t g_frames_presented;
static inline int hot_quick(uint32_t vt, int cold) {
#ifdef EXP_NOQUICK
    return 0;
#endif
    return !cold && s_hot.ok && s_hot.wt && s_hot.vt == vt && s_hot.epoch == g_texrep_epoch && s_hot.frames == g_frames_presented &&
           s_ps_through == 0 && s_vt == vt && s_hot.sum_nv == hot_sum_nv();
}
static void hot_skin_tex(void) {
    const VtxState *s = vtx_state();
    SkinPrep *sp = &s_hot.sk;
    sp->su = s->tex_su; sp->sv = s->tex_sv; sp->ou = s->tex_ou; sp->ov = s->tex_ov;
    sp->ps.suv[0] = s->tex_su; sp->ps.suv[1] = s->tex_sv; sp->ps.ouv[0] = s->tex_ou; sp->ps.ouv[1] = s->tex_ov;
}

/* Only the vertex type moved, between two formats the skinning kernel takes
   (the next part of a fighter has its own bone count and stride), and the
   registers the prepared state is made from are as they were: the format and
   the kernel's layout are redone and nothing else.  hot_refresh() gets to the
   same state the long way round (about 3 us, some 330 times a frame in a
   fight).  Returns 0, with nothing but the format changed, when it does not
   apply. */
static inline int hot_switch(uint32_t vt, int cold) {
#ifdef EXP_NOSWITCH
    return 0;
#endif
    if (cold || !s_hot.ok || !s_hot.wt || !s_hot.sk_ok || !(s_hot.sk.ps_ok || s_hot.sk.mode >= 3) || s_hot.sk_light != g_ge_light_gen ||
        s_hot.epoch != g_texrep_epoch || s_hot.frames != g_frames_presented || s_ps_through != 0 ||
        s_hot.sum_nv != hot_sum_nv() || (vt >> 23 & 1) || !(R(GE_SHADEMODE) & 1))
        return 0;
    if (vt != s_vt) {
        vformat(&s_vf, vt);
        s_vt = vt;
    }
    const VFormat *f = &s_vf;
    if (f->idx || f->nmorph != 1 || !skin_fits(&s_hot.sk, f)) return 0;
    skin_reformat(&s_hot.sk, f);
    hot_skin_tex();
    s_hot.vt = vt;
    return 1;
}

static __attribute__((noinline)) void hot_refresh(uint32_t vt, uint32_t sum) {
    extern uint32_t g_frames_presented;
    g_hot_plain = 0;
    s_hot.ok = 0;
    s_hot.setup_ep = 0;
    s_hot.sum = sum;
    s_hot.sum_nv = sum - g_ge_vertex_gen;
    s_hot.vt = vt;
    s_hot.frames = g_frames_presented;
    s_hot.epoch = texrep_epoch();
    if ((vt >> 23 & 1) || !(R(GE_SHADEMODE) & 1)) return;   /* through mode, flat shading */
#ifdef TKDR_FINEPROF
    DP_T(h0);
#endif
    const int newfmt = vt != s_vt;
    if (newfmt) {   /* a new format, as when the next part of a fighter has more bones */
        vformat(&s_vf, vt);
        s_vt = vt;
    }
    const VFormat *f = &s_vf;
    if (f->idx || f->nmorph != 1) return;
#ifdef TKDR_FINEPROF
    DP_T(h1);
#endif
    uint32_t gen = g_ge_pix_gen;
    const int px = s_ps_gen != gen || s_ps_through != 0 || s_ps_epoch != s_hot.epoch;
    if (px) {   /* a texture, blend or other state change */
        raster_init();   /* the first draw of the run has not been through draw_prim() */
        pix_state(&s_ps, 0);
        s_ps_gen = gen;
        s_ps_through = 0;
        s_ps_epoch = s_hot.epoch;
    }
#ifdef TKDR_FINEPROF
    DP_T(h2);
    g_hr_sub[f->wt != 0][0] += (uint32_t)(h1 - h0); g_hr_subn[f->wt != 0][0] += newfmt;
    g_hr_sub[f->wt != 0][1] += (uint32_t)(h2 - h1); g_hr_subn[f->wt != 0][1] += px;
#endif
    if (!gpu_is_display_target(s_ps.tg.fb)) {
#ifdef TKDR_AUTOPILOT
        g_tgt_refused++;
        g_tgt_nd_fb = s_ps.tg.fb;
#endif
        return;
    }
#ifdef TKDR_AUTOPILOT
    g_tgt_fb = s_ps.tg.fb;
#endif
    if (f->wt) {
        int ok;
#ifndef EXP_NOSKINKEEP
        if (s_hot.sk_ok && s_hot.sk_light == g_ge_light_gen && skin_fits(&s_hot.sk, f)) {
            /* Only the format (and the pixel state, above) moved: what skin_prepare()
               works out from the lighting is still what it was. */
            skin_reformat(&s_hot.sk, f);
            hot_skin_tex();
            ok = 1;
        } else
#endif
        {
            g_gxr_object = 1;
            ok = skin_prepare(f, &s_hot.sk);
            g_gxr_object = 0;
            s_hot.sk_ok = ok;
            s_hot.sk_light = g_ge_light_gen;
        }
        if (!ok) return;
        s_hot.mv = NULL;
        s_hot.uvt = NULL;
    } else {
        int obj = !f->nrm && f->pos && (f->col == 0 || f->col >= 4) && gxr_lights_position_free();
        s_hot.obj = obj;
        s_hot.key_vt = obj ? (vt | 0x80000000u) : vt;
        vc_regs(&s_hot.regs, obj, f->nrm != 0, !f->nrm && (f->col == 0 || f->col >= 4));
        s_hot.mv = obj ? gxr_modelview() : NULL;
        s_hot.uvt = vtx_state_uvt();
    }
#ifdef TKDR_FINEPROF
    {
        DP_T(h3);
        g_hr_sub[f->wt != 0][2] += (uint32_t)(h3 - h2); g_hr_subn[f->wt != 0][2]++;
    }
#endif
    s_hot.wt = f->wt;
    s_hot.sum = hot_sum();   /* the sum depends on the kind of draw */
    s_hot.sum_nv = hot_sum_nv();
    s_hot.ok = 1;
    g_hot_plain = !f->wt;
}

#ifdef EXP_GXLIGHT
/* Draws whose vertices all follow one bone at full weight (59% of the skinned vertices of a fight; 43% of the draws are
   made of nothing else, 86% of those of one bone) need no skinning: the chip transforms them with bone x world x view
   in the second position matrix slot, and lights them with the normal matrix bone x world, as long as that is a
   rotation (the CPU path renormalises, the chip does not).  What is left for the CPU is to read the vertices as the
   model has them and to bring each normal to unit length.  Every other draw is skinned as before.
   Built and checked (EXP_RIGIDCHK: the chip's inputs against the skinning kernel's outputs, bad = 0 up to float noise;
   the fights look the same on screen) but OFF: -DEXP_RIGID turns it on, and it was slower by 0.2-0.4 ms a frame in
   Dolphin (the draws average five vertices, so what the draw costs outweighs the skinning it saves). */
static GxVtxR g_pkr[PK_MAX];
#ifdef TKDR_AUTOPILOT
int g_ab_rigid = 1;   /* A/B runs (EXP_ABRIGID): the chip transforms the single-bone draws or the CPU skins them */
uint32_t g_rg_draws, g_rg_verts, g_rg_notrigid, g_rg_nomat;
#endif
static struct { float bone[12]; uint32_t mvv, ver; int valid, ok; float mv[12], nm[9]; } s_rg;   /* ver moves with mv and nm */

/* Twelve floats equal: by value and inline (a memcmp call costs more than the rest of a small draw). */
static inline int same12(const float *a, const float *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3] && a[4] == b[4] && a[5] == b[5] && a[6] == b[6] &&
           a[7] == b[7] && a[8] == b[8] && a[9] == b[9] && a[10] == b[10] && a[11] == b[11];
}

/* The slot of the one bone every vertex follows at full weight (the guest's 128), or -1. */
static inline int rigid_scan(const VFormat *f, const uint8_t *vb, int count) {
    const int nw = f->nw, size = f->size;
    const uint8_t *wb = vb + f->off_wt;
    int k = -1;
    for (int j = 0; j < nw; j++)
        if (wb[j]) {
            if (k >= 0) return -1;
            k = j;
        }
    if (k < 0 || wb[k] != 128) return -1;
    for (int i = 1; i < count; i++) {
        wb += size;
        int s = 0;
        for (int j = 0; j < nw; j++) s += wb[j];
        if (s != 128 || wb[k] != 128) return -1;
    }
    return k;
}

/* The matrices of a rigid draw (bone k): model to view for the position, model to world for the normal, and whether
   the second is a rotation.  Kept until the bone, the world or the view changes. */
static int rigid_mats(int k) {
    const float *M = gxr_modelview();   /* world x view, in the GE's layout; moves g_mv_version when it changes */
    const float *B = g_ge.bone[k], *W = g_ge.world;
    if (s_rg.valid && s_rg.mvv == g_mv_version && same12(s_rg.bone, B)) return s_rg.ok;
    s_rg.valid = 1;
    s_rg.mvv = g_mv_version;
    s_rg.ver++;
    memcpy(s_rg.bone, B, sizeof s_rg.bone);
    float *mv = s_rg.mv, *nm = s_rg.nm;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            mv[i * 3 + j] = B[i * 3] * M[j] + B[i * 3 + 1] * M[3 + j] + B[i * 3 + 2] * M[6 + j];
            nm[i * 3 + j] = B[i * 3] * W[j] + B[i * 3 + 1] * W[3 + j] + B[i * 3 + 2] * W[6 + j];
        }
    for (int j = 0; j < 3; j++) mv[9 + j] = B[9] * M[j] + B[10] * M[3 + j] + B[11] * M[6 + j] + M[9 + j];
    const float l0 = nm[0] * nm[0] + nm[1] * nm[1] + nm[2] * nm[2], l1 = nm[3] * nm[3] + nm[4] * nm[4] + nm[5] * nm[5],
                l2 = nm[6] * nm[6] + nm[7] * nm[7] + nm[8] * nm[8];
    const float d01 = nm[0] * nm[3] + nm[1] * nm[4] + nm[2] * nm[5], d02 = nm[0] * nm[6] + nm[1] * nm[7] + nm[2] * nm[8],
                d12 = nm[3] * nm[6] + nm[4] * nm[7] + nm[5] * nm[8];
    /* A rotation, as far as the lighting can tell, up to one scale for all three axes (the world matrix of a fight carries
       one, the fighters are drawn about 2700 times the size of the guest's units): the chip does not renormalise, so
       the scale is taken out of the matrix, and the vertex normals were brought to unit length by the decoder. */
    s_rg.ok = l0 > 1e-6f && fabsf(l1 - l0) < 0.002f * l0 && fabsf(l2 - l0) < 0.002f * l0 && fabsf(d01) < 0.002f * l0 &&
              fabsf(d02) < 0.002f * l0 && fabsf(d12) < 0.002f * l0;
    if (s_rg.ok) {
        const float inv = fast_rsqrt(l0);
        for (int i = 0; i < 9; i++) nm[i] *= inv;
    }
#ifdef EXP_RIGIDCHK
    {
        static int shown;
        if (!s_rg.ok && shown < 10) {
            shown++;
            fprintf(stderr, "RIGIDCHK refused bone %d: row lengths^2 %g %g %g dots %g %g %g; bone3x3 lengths^2 %g %g %g world3x3 lengths^2 %g %g %g\n", k,
                    (double)l0, (double)l1, (double)l2, (double)d01, (double)d02, (double)d12,
                    (double)(B[0] * B[0] + B[1] * B[1] + B[2] * B[2]), (double)(B[3] * B[3] + B[4] * B[4] + B[5] * B[5]), (double)(B[6] * B[6] + B[7] * B[7] + B[8] * B[8]),
                    (double)(W[0] * W[0] + W[1] * W[1] + W[2] * W[2]), (double)(W[3] * W[3] + W[4] * W[4] + W[5] * W[5]), (double)(W[6] * W[6] + W[7] * W[7] + W[8] * W[8]));
        }
    }
#endif
    return s_rg.ok;
}

static __attribute__((noinline, optimize("Ofast", "no-thread-jumps"))) void rigid_decode(const VFormat *f, const SkinPrep *sp, const uint8_t *vb, int count, GxVtxR *out) {
    const int size = f->size, off_pos = f->off_pos, off_nrm = f->off_nrm, off_tc = f->off_tc, tcm = f->tc;
    const float su = sp->su, sv = sp->sv, ou = sp->ou, ov = sp->ov;
    for (int i = 0; i < count; i++, vb += size, out++) {
        const uint8_t *pp = vb + off_pos, *nb = vb + off_nrm;
        out->x = (int16_t)ld16le(pp);
        out->y = (int16_t)ld16le(pp + 2);
        out->z = (int16_t)ld16le(pp + 4);
        const float nx = k_n_s8[nb[0]], ny = k_n_s8[nb[1]], nz = k_n_s8[nb[2]];
        const float l2 = nx * nx + ny * ny + nz * nz;
        const float s = l2 > 1e-6f ? fast_rsqrt(l2) * 16384.f : 0.f;
        out->nx = (int16_t)(nx * s);
        out->ny = (int16_t)(ny * s);
        out->nz = (int16_t)(nz * s);
        Vec2f tv = {0.f, 0.f};
        if (tcm) tv = rd_uv_fast(vb + off_tc, tcm);
        out->u = tv.a * su + ou;
        out->v = tv.b * sv + ov;
    }
}

#ifdef EXP_RIGIDCHK
/* Debug: what the chip will compute from the rigid vertices and the two matrices, against the skinning kernel's. */
static uint32_t g_rck_calls, g_rck_verts, g_rck_bad;
static float g_rck_maxp, g_rck_maxn, g_rck_maxuv;
static void rigid_check(const VFormat *f, const SkinPrep *sp, const uint8_t *vb, int count) {
    static GxVtxL ref[PK_MAX];
    skin_run_hw(f, sp, vb, count, ref);
    g_rck_calls++;
    for (int i = 0; i < count; i++) {
        const float p[3] = {(float)g_pkr[i].x * (1.f / 32768.f), (float)g_pkr[i].y * (1.f / 32768.f), (float)g_pkr[i].z * (1.f / 32768.f)};
        float pv[3], rv[3];
        mul43(s_rg.mv, p, pv);
        const float rw[3] = {ref[i].x, ref[i].y, ref[i].z};
        mul43(g_ge.view, rw, rv);
        int bad = 0;
        for (int j = 0; j < 3; j++) {
            const float m = fabsf(rv[j]) > 1.f ? fabsf(rv[j]) : 1.f, e = fabsf(pv[j] - rv[j]) / m;
            if (e > g_rck_maxp) g_rck_maxp = e;
            if (!(e <= 3e-4f)) bad = 1;
        }
        const float n[3] = {(float)g_pkr[i].nx * (1.f / 16384.f), (float)g_pkr[i].ny * (1.f / 16384.f), (float)g_pkr[i].nz * (1.f / 16384.f)};
        for (int j = 0; j < 3; j++) {
            const float wn = n[0] * s_rg.nm[j] + n[1] * s_rg.nm[3 + j] + n[2] * s_rg.nm[6 + j];
            const float rn = (float)(&ref[i].nx)[j] * (1.f / 16384.f), e = fabsf(wn - rn);
            if (e > g_rck_maxn) g_rck_maxn = e;
            if (!(e <= 0.004f)) bad = 1;
        }
        const float eu = fabsf(g_pkr[i].u - ref[i].u), ev = fabsf(g_pkr[i].v - ref[i].v);
        if (eu > g_rck_maxuv) g_rck_maxuv = eu;
        if (ev > g_rck_maxuv) g_rck_maxuv = ev;
        if (!(eu <= 1e-4f && ev <= 1e-4f)) bad = 1;
        g_rck_verts++;
        if (bad && ++g_rck_bad <= 12)
            fprintf(stderr, "RIGIDCHK MISMATCH v%d/%d: view (%g %g %g) ref (%g %g %g)\n", i, count, (double)pv[0], (double)pv[1], (double)pv[2], (double)rv[0], (double)rv[1], (double)rv[2]);
    }
    if ((g_rck_calls & 4095) == 1)
        fprintf(stderr, "RIGIDCHK calls %u verts %u bad %u; view position err %g (relative), normal err %g, uv err %g\n", g_rck_calls, g_rck_verts, g_rck_bad,
                (double)g_rck_maxp, (double)g_rck_maxn, (double)g_rck_maxuv);
}
#endif

/* Decodes the draw into g_pkr and returns 1 when the chip can do it (rigid_scan, rigid_mats); the caller skins it otherwise. */
static inline int rigid_try(const VFormat *f, const SkinPrep *sp, const uint8_t *vb, int count) {
#ifndef EXP_RIGID
    (void)f; (void)sp; (void)vb; (void)count;
    return 0;   /* off: measured in Dolphin it costs 0.2-0.4 ms a frame more than skinning these draws on the CPU (HANDOVER, P6) */
#endif
#ifdef TKDR_AUTOPILOT
    if (!g_ab_rigid) return 0;
#endif
    const int k = rigid_scan(f, vb, count);
    if (k < 0) {
#ifdef TKDR_AUTOPILOT
        g_rg_notrigid++;
#endif
        return 0;
    }
    if (!rigid_mats(k)) {
#ifdef TKDR_AUTOPILOT
        g_rg_nomat++;
#endif
        return 0;
    }
    rigid_decode(f, sp, vb, count, g_pkr);
#ifdef TKDR_AUTOPILOT
    g_rg_draws++;
    g_rg_verts += (uint32_t)count;
#endif
#ifdef EXP_RIGIDCHK
    rigid_check(f, sp, vb, count);
#endif
    return 1;
}
#endif

#ifdef EXP_CULLCENSUS
/* How many skinned draws could be skipped without a visible change: every vertex outside one clip plane, or the
   whole draw inside a box under one pixel.  Counted on the skinned output (world space) with the GE's view and
   projection; the line is `CULLCENSUS`, a frame average over each 300 frames. */
static uint64_t rc_draws, rc_verts, rc_rv, rc_rd, rc_rdv, rc_bones[5];   /* draws, vertices, single-bone vertices, draws of only single-bone vertices (+ their vertices), those by distinct bones */
static void rigid_census(const VFormat *f, const uint8_t *vp, int count) {
    uint32_t seen = 0;
    int all = 1, rv = 0;
    const uint8_t *q = vp + f->off_wt;
    for (int i = 0; i < count; i++, q += f->size) {
        int nz = 0, which = -1, full = 0;
        for (int k = 0; k < f->nw; k++) if (q[k]) { nz++; which = k; full = q[k] == 128; }
        if (nz == 1 && full) { rv++; seen |= 1u << which; } else all = 0;
    }
    rc_draws++; rc_verts += (uint64_t)count; rc_rv += (uint64_t)rv;
    if (all) {
        int nb = __builtin_popcount(seen);
        rc_rd++; rc_rdv += (uint64_t)count; rc_bones[nb > 4 ? 4 : nb]++;
    }
}
static void cull_census(const uint8_t *v, size_t stride, int count) {
    extern uint32_t g_frames_presented;
    static uint64_t draws, verts, off_d, off_v, tiny_d, tiny_v, behind_d;
    static uint32_t last_frame, frames, start;
    if (!start) start = g_frames_presented;
    if (g_frames_presented != last_frame) { last_frame = g_frames_presented; frames++; }
    const float *V = g_ge.view, *P = g_ge.proj;
    unsigned all = 0x3F, anybehind = 0;
    float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
    for (int i = 0; i < count; i++) {
        const float *p = (const float *)(v + (size_t)i * stride);
        const float ex = p[0] * V[0] + p[1] * V[3] + p[2] * V[6] + V[9];
        const float ey = p[0] * V[1] + p[1] * V[4] + p[2] * V[7] + V[10];
        const float ez = p[0] * V[2] + p[1] * V[5] + p[2] * V[8] + V[11];
        const float cx = ex * P[0] + ey * P[4] + ez * P[8] + P[12];
        const float cy = ex * P[1] + ey * P[5] + ez * P[9] + P[13];
        const float cz = ex * P[2] + ey * P[6] + ez * P[10] + P[14];
        const float cw = ex * P[3] + ey * P[7] + ez * P[11] + P[15];
        all &= (unsigned)(cx < -cw) | (unsigned)(cx > cw) << 1 | (unsigned)(cy < -cw) << 2 | (unsigned)(cy > cw) << 3 |
               (unsigned)(cz < -cw) << 4 | (unsigned)(cz > cw) << 5;
        if (cw > 1e-6f) {
            const float sx = cx / cw * 240.f, sy = cy / cw * 136.f;
            if (sx < minx) minx = sx;
            if (sx > maxx) maxx = sx;
            if (sy < miny) miny = sy;
            if (sy > maxy) maxy = sy;
        } else anybehind = 1;
    }
    draws++; verts += (uint64_t)count;
    if (all) { off_d++; off_v += (uint64_t)count; }
    else if (!anybehind && maxx - minx < 1.f && maxy - miny < 1.f) { tiny_d++; tiny_v += (uint64_t)count; }
    behind_d += anybehind;
    if (frames >= 300) {
        const double n = (double)frames;
        fprintf(stderr, "CULLCENSUS per frame over %u frames: skinned draws %.0f (%.0f verts); outside one clip plane %.1f draws (%.0f verts); under 1 px %.1f draws (%.0f verts); with a vertex behind the eye %.1f\n",
                frames, (double)draws / n, (double)verts / n, (double)off_d / n, (double)off_v / n, (double)tiny_d / n, (double)tiny_v / n, (double)behind_d / n);
        fprintf(stderr, "RIGIDCENSUS per frame: skinned draws %.0f verts %.0f; single-bone full-weight vertices %.0f (%.0f%%); draws made only of them %.0f (%.0f%%, %.0f verts); of those by bones used 1:%.0f 2:%.0f 3:%.0f 4+:%.0f\n",
                (double)rc_draws / n, (double)rc_verts / n, (double)rc_rv / n, 100.0 * (double)rc_rv / (double)(rc_verts ? rc_verts : 1),
                (double)rc_rd / n, 100.0 * (double)rc_rd / (double)(rc_draws ? rc_draws : 1), (double)rc_rdv / n,
                (double)rc_bones[1] / n, (double)rc_bones[2] / n, (double)rc_bones[3] / n, (double)rc_bones[4] / n);
        rc_draws = rc_verts = rc_rv = rc_rd = rc_rdv = 0;
        memset(rc_bones, 0, sizeof rc_bones);
        draws = verts = off_d = off_v = tiny_d = tiny_v = behind_d = 0;
        frames = 0;
    }
}
#endif

#if defined(EXP_GXLIGHT) && !defined(EXP_NOPIPE) && !defined(EXP_RIGID) && !defined(TKDR_FINEPROF) && !defined(EXP_LITCHK) && \
    !defined(EXP_CULLCENSUS) && !defined(EXP_DRAWTRACE) && !defined(EXP_PIPECHK)
/* The chip-lit skinned draws (four in five of a fight's) are written by the skinning kernel straight into the GX write-gather pipe,
   after the draw's GX_Begin (wii_gxscene_lit_begin()), instead of into g_pk and from there, a vertex at a time, into the pipe by
   wii_gxscene_draw_lit(): the same bytes in the same order, without the loads and the loop that copy them.  The probes that read
   g_pk or run the kernel more than once (the lines above) keep the staging path. */
#define SKIN_PIPE 1
#define WG_PIPE ((void *)0xCC008000)
#ifdef EXP_ABPIPE
int g_ab_pipe = 1;   /* A/B runs: the kernel writes to the pipe, or to the staging buffer as before */
#define SKIN_PIPE_ON (g_ab_pipe)
#else
#define SKIN_PIPE_ON 1
#endif
#endif

#ifdef EXP_PIPECHK
/* -DEXP_PIPECHK: the pipe kernel's stream (the same stores into a buffer, one after the other) against the staging kernel's
   vertices, for every chip-lit skinned draw; the draw itself still goes through the staging buffer. */
static uint32_t g_pc_n, g_pc_bad;
static void pipe_check(const VFormat *f, const SkinPrep *sp, const uint8_t *vb, int count, const GxVtxL *got) {
    static uint8_t dense[PK_MAX * 26 + 64] __attribute__((aligned(32)));
    g_skin_ps_dense[f->tc == 1 ? 1 : f->tc == 3 ? 2 : 0](&sp->ps, vb, dense, count);
    g_pc_n++;
    for (int i = 0; i < count; i++) {
        const uint8_t *d = dense + i * 26;
        if (memcmp(d, &got[i].x, 12) || memcmp(d + 12, &got[i].nx, 6) || memcmp(d + 18, &got[i].u, 8)) {
            if (++g_pc_bad <= 8) fprintf(stderr, "PIPECHK MISMATCH v%d/%d nw=%d tc=%d\n", i, count, f->nw, f->tc);
            break;
        }
    }
    if ((g_pc_n & 4095) == 1) fprintf(stderr, "PIPECHK %u draws, %u bad\n", g_pc_n, g_pc_bad);
}
#endif

/* The common draws of a fight, without the general path's weight (a large
   frame to set up, seventeen registers saved, the vertex format copied): a
   skinned fighter, or a plain draw that is already in the draw cache.  It
   does nothing at all and returns 0 for anything else, or when the cached
   pixel state is out of date, and draw_prim() then does the draw. */
static int draw_prim_fast(int prim, int count) {
#ifdef TKDR_AUTOPILOT
    DP_T(dpf0);
    if (!g_ab_fast) return 0;
#endif
#ifdef EXP_NOFAST
    return 0;
#endif
    if (!g_gxr_enabled || prim < 3 || prim > 5 || count <= 0) return 0;
    extern uint32_t g_frames_presented;
    const uint32_t vt = R(GE_VERTEXTYPE), sum = hot_sum();
#ifdef TKDR_FINEPROF
    uint32_t sw_t0;
#endif
#ifdef TKDR_AUTOPILOT
    const int cold = !g_ab_hot;
#else
    const int cold = 0;
#endif
    if (__builtin_expect(cold || s_hot.sum != sum || s_hot.vt != vt || s_hot.epoch != g_texrep_epoch ||
                         s_hot.frames != g_frames_presented || s_ps_through != 0 || s_vt != vt, 0))
    if (hot_quick(vt, cold)) {
        hot_skin_tex();
        s_hot.sum = sum;
    } else if (
#ifdef TKDR_FINEPROF
        (sw_t0 = dp_tb(), hot_switch(vt, cold) ? (g_hr_why[9]++, g_sw_ticks += (uint32_t)(dp_tb() - sw_t0), 1) : 0)
#else
        hot_switch(vt, cold)
#endif
        ) {
        s_hot.sum = sum;
    } else {
#ifdef TKDR_FINEPROF
        {
            static uint32_t l_pix, l_vtx, l_lig, l_view, l_world;
            g_hr_why[0] += l_pix != g_ge_pix_gen;
            g_hr_why[1] += l_vtx != g_ge_vertex_gen;
            g_hr_why[2] += l_lig != g_ge_light_gen;
            g_hr_why[3] += l_view != g_ge_view_gen;
            g_hr_why[4] += l_world != g_ge_world_gen;
            g_hr_why[5] += s_hot.vt != vt;
            g_hr_why[6] += s_hot.epoch != g_texrep_epoch;
            g_hr_why[7] += s_hot.frames != g_frames_presented;
            g_hr_why[8] += s_ps_through != 0 || s_vt != vt;
            l_pix = g_ge_pix_gen; l_vtx = g_ge_vertex_gen; l_lig = g_ge_light_gen; l_view = g_ge_view_gen; l_world = g_ge_world_gen;
        }
        DP_T(hr0);
        hot_refresh(vt, sum);
        DP_T(hr1);
        g_hr_ticks[s_hot.wt != 0] += (uint32_t)(hr1 - hr0);
        g_hr_n[s_hot.wt != 0]++;
#else
        hot_refresh(vt, sum);
#endif
    }
    if (!s_hot.ok) return 0;
    const VFormat *f = &s_vf;
    const PixState *ps = &s_ps;
#ifdef TKDR_AUTOPILOT
    DP_T(dpf_pre);
#endif
    uint32_t vbase = g_ge.vaddr;
    const uint8_t *vp;
    const GxVtx *out;
#ifdef EXP_GXLIGHT
    int lit = 0, rigid = 0;
#ifdef SKIN_PIPE
    int pipe = 0;
#endif
#endif
    if (s_hot.wt) {
        if (count > PK_MAX || !(vp = guest_host_ptr(vbase, (uint32_t)(f->size * count)))) return 0;
        HITCHK_SKIN(f);
#ifdef EXP_CULLCENSUS
        rigid_census(f, vp, count);
#endif
        g_gxr_object = 1;
#ifdef EXP_GXLIGHT
#ifdef TKDR_AUTOPILOT
        lit = s_hot.sk.hw && g_ab_lit;
#else
        lit = s_hot.sk.hw;
#endif
        if (lit) {
#ifdef SKIN_PIPE
            if (SKIN_PIPE_ON && count >= 3) pipe = 1;   /* the kernel runs at the draw, after the setup and the GX_Begin */
            else
#endif
            if (!(rigid = rigid_try(f, &s_hot.sk, vp, count))) skin_run_hw(f, &s_hot.sk, vp, count, (GxVtxL *)g_pk);
#ifdef EXP_PIPECHK
            if (!rigid) pipe_check(f, &s_hot.sk, vp, count, (const GxVtxL *)g_pk);
#endif
        } else
#endif
        skin_run(f, &s_hot.sk, vp, count, g_pk);
        g_gxr_object = 0;
        out = g_pk;
#ifdef EXP_DRAWTRACE
        {
            extern volatile int g_dt_window;
            if (g_dt_window && g_frames_presented % 6 == 0) {
                static uint32_t last_frame, idx;
                if (last_frame != g_frames_presented) { last_frame = g_frames_presented; idx = 0; }
                fprintf(stderr, "DV f=%u #%u va=%08X n=%d tex=%08X z=%d/%d/%d at=%d bl=%d v0=(%.4f %.4f %.4f) v1=(%.4f %.4f %.4f) v2=(%.4f %.4f %.4f) rgba0=%08X uv0=(%.3f %.3f) uv1=(%.3f %.3f) uv2=(%.3f %.3f) nw=%d wt0=%u,%u,%u,%u nrm0=%d,%d,%d nrm1=%d,%d,%d nrm2=%d,%d,%d\n",
                        g_frames_presented, idx++, vbase, count, ps->tex.enabled ? ps->tex.addr : 0u, ps->z_test, ps->z_func, ps->z_write,
                        ps->alpha_test, ps->blend, (double)g_pk[0].x, (double)g_pk[0].y, (double)g_pk[0].z,
                        count > 1 ? (double)g_pk[1].x : 0.0, count > 1 ? (double)g_pk[1].y : 0.0, count > 1 ? (double)g_pk[1].z : 0.0,
                        count > 2 ? (double)g_pk[2].x : 0.0, count > 2 ? (double)g_pk[2].y : 0.0, count > 2 ? (double)g_pk[2].z : 0.0, g_pk[0].rgba,
                        (double)g_pk[0].u, (double)g_pk[0].v, count > 1 ? (double)g_pk[1].u : 0.0, count > 1 ? (double)g_pk[1].v : 0.0,
                        count > 2 ? (double)g_pk[2].u : 0.0, count > 2 ? (double)g_pk[2].v : 0.0,
                        f->nw, vp[f->off_wt], f->nw > 1 ? vp[f->off_wt + 1] : 0, f->nw > 2 ? vp[f->off_wt + 2] : 0, f->nw > 3 ? vp[f->off_wt + 3] : 0,
                        (int8_t)vp[f->off_nrm], (int8_t)vp[f->off_nrm + 1], (int8_t)vp[f->off_nrm + 2],
                        count > 1 ? (int8_t)vp[f->size + f->off_nrm] : 0, count > 1 ? (int8_t)vp[f->size + f->off_nrm + 1] : 0, count > 1 ? (int8_t)vp[f->size + f->off_nrm + 2] : 0,
                        count > 2 ? (int8_t)vp[2 * f->size + f->off_nrm] : 0, count > 2 ? (int8_t)vp[2 * f->size + f->off_nrm + 1] : 0, count > 2 ? (int8_t)vp[2 * f->size + f->off_nrm + 2] : 0);
                if (g_lc.gen != g_ge_light_gen) build_light_cache();
                float wn_[3][3] = {{0}}, fn_[3] = {0, 0, 0};
                for (int q_ = 0; q_ < 3 && q_ < count; q_++) {
                    const uint8_t *vq_ = vp + q_ * f->size, *wb_ = vq_ + f->off_wt, *nb_ = vq_ + f->off_nrm;
                    float nr_[3] = {k_n_s8[nb_[0]], k_n_s8[nb_[1]], k_n_s8[nb_[2]]}, sn_[3] = {0, 0, 0};
                    for (int k_ = 0; k_ < f->nw; k_++) { if (wb_[k_] == 0) continue; skin_nrm(g_ge.bone[k_], k_n_u8[wb_[k_]], nr_, sn_); }
                    const float *m_ = g_ge.world;
                    for (int j_ = 0; j_ < 3; j_++) wn_[q_][j_] = m_[j_] * sn_[0] + m_[3 + j_] * sn_[1] + m_[6 + j_] * sn_[2];
                }
                if (count > 2) {
                    float a_[3] = {g_pk[1].x - g_pk[0].x, g_pk[1].y - g_pk[0].y, g_pk[1].z - g_pk[0].z};
                    float b_[3] = {g_pk[2].x - g_pk[0].x, g_pk[2].y - g_pk[0].y, g_pk[2].z - g_pk[0].z};
                    fn_[0] = a_[1] * b_[2] - a_[2] * b_[1]; fn_[1] = a_[2] * b_[0] - a_[0] * b_[2]; fn_[2] = a_[0] * b_[1] - a_[1] * b_[0];
                    float l_ = sqrtf(fn_[0] * fn_[0] + fn_[1] * fn_[1] + fn_[2] * fn_[2]);
                    if (l_ > 0.f) { fn_[0] /= l_; fn_[1] /= l_; fn_[2] /= l_; }
                }
                fprintf(stderr, "DN f=%u #%u wn0=(%.3f %.3f %.3f) wn1=(%.3f %.3f %.3f) wn2=(%.3f %.3f %.3f) fn=(%.3f %.3f %.3f) wn0.fn=%.3f rev=%d nlit=%d",
                        g_frames_presented, idx - 1, (double)wn_[0][0], (double)wn_[0][1], (double)wn_[0][2], (double)wn_[1][0], (double)wn_[1][1], (double)wn_[1][2],
                        (double)wn_[2][0], (double)wn_[2][1], (double)wn_[2][2], (double)fn_[0], (double)fn_[1], (double)fn_[2],
                        (double)(wn_[0][0] * fn_[0] + wn_[0][1] * fn_[1] + wn_[0][2] * fn_[2]), g_lc.reverse, g_lc.nlit);
                for (int j_ = 0; j_ < g_lc.nlit; j_++)
                    fprintf(stderr, " L%d(k%d c%d L=(%.3f %.3f %.3f) D=(%.1f %.1f %.1f) A=(%.1f %.1f %.1f))", g_lc.t[j_].idx, g_lc.l[g_lc.t[j_].idx].kind, g_lc.l[g_lc.t[j_].idx].comp,
                            (double)g_lc.l[g_lc.t[j_].idx].L[0], (double)g_lc.l[g_lc.t[j_].idx].L[1], (double)g_lc.l[g_lc.t[j_].idx].L[2],
                            (double)g_lc.t[j_].d[0], (double)g_lc.t[j_].d[1], (double)g_lc.t[j_].d[2], (double)g_lc.t[j_].a[0], (double)g_lc.t[j_].a[1], (double)g_lc.t[j_].a[2]);
                fprintf(stderr, " base=(%.1f %.1f %.1f)\n", (double)g_lc.base[0], (double)g_lc.base[1], (double)g_lc.base[2]);
            }
        }
#endif
    } else {
        if (count > VC_MAX_VERTS || !(vp = guest_host_ptr(vbase, (uint32_t)(f->size * count)))) return 0;
        VcTok tok;
        out = vc_lookup2(vp, vbase, count, f->size, s_hot.key_vt, &s_hot.regs, &tok);
        if (out && tok.refresh) vc_alpha_refresh(f, vp, count, (GxVtx *)out, tok.ga);
        WTP_PROBE(vbase, (uint32_t)(f->size * count), out != NULL);
        if (out) HITCHK_HIT(f, vbase, count, s_hot.obj, out);
        if (s_hot.mv) HITCHK_MV(s_hot.mv);
        if (!out) {
#ifdef TKDR_AUTOPILOT
            g_dp_cat_miss = 1;
#endif
            out = plain_miss(f, vbase, count, s_hot.obj, &tok);
        }
    }
    g_prims++;
    g_prim_calls++;
    g_prim_type[prim]++;
    g_draw_pix_gen = s_ps_gen;
    if (ps->tex.enabled) tex_touch(ps->tex.entry);
    g_ge.vaddr = vbase + (uint32_t)(f->size * count);
    g_gxr_prims++;
#ifdef EXP_CULLCENSUS
    if (s_hot.wt) {
#ifdef EXP_GXLIGHT
        cull_census((const uint8_t *)g_pk, lit ? sizeof(GxVtxL) : sizeof(GxVtx), count);
#else
        cull_census((const uint8_t *)g_pk, sizeof(GxVtx), count);
#endif
    }
#endif
#ifdef TKDR_AUTOPILOT
    DP_T(dpf_a);
    g_fast_ticks[0] += (uint32_t)(dpf_a - dpf0);
#endif
    /* The chip is set up for this state already when nothing has touched it
       since (the setup's own changes are in the epoch it left). */
#ifdef EXP_NOSETUPSKIP
    if (1) {
#else
    if (__builtin_expect(s_hot.setup_ep != g_gx_epoch || !s_hot.setup_ep, 0)) {
#endif
        if (!gxr_prim_setup(ps, prim, 0)) return 1;
        s_hot.setup_ep = g_gx_epoch;
    }
#ifdef TKDR_AUTOPILOT
    DP_T(dpf_b);
    g_fast_ticks[1] += (uint32_t)(dpf_b - dpf_a);
#endif
#ifdef EXP_GXLIGHT
    if (rigid) wii_gxscene_draw_rigid(prim - 3, g_pkr, count, s_hot.sk_light, &s_hot.sk.lit, s_rg.mv, s_rg.nm, s_rg.ver);
#ifdef SKIN_PIPE
    else if (pipe) {
        const int n = wii_gxscene_lit_begin(prim - 3, count, s_hot.sk_light, &s_hot.sk.lit);
        if (n) {
            g_skin_ps_pipe[f->tc == 1 ? 1 : f->tc == 3 ? 2 : 0](&s_hot.sk.ps, vp, WG_PIPE, n);
            wii_gxscene_lit_end();
        }
    }
#endif
    else if (lit) wii_gxscene_draw_lit(prim - 3, (const GxVtxL *)g_pk, count, s_hot.sk_light, &s_hot.sk.lit);
    else
#endif
    wii_gxscene_draw_packed(prim - 3, out, count, s_hot.mv, s_hot.uvt);
#ifdef TKDR_AUTOPILOT
    DP_T(dpf_c);
    g_fast_ticks[2] += (uint32_t)(dpf_c - dpf_b);
    g_fast_n++;
    {
        const int k = f->wt ? 0 : 1;
        g_fp_ticks[k][0] += (uint32_t)(dpf_pre - dpf0);
        g_fp_ticks[k][1] += (uint32_t)(dpf_a - dpf_pre);
        g_fp_ticks[k][2] += (uint32_t)(dpf_b - dpf_a);
        g_fp_ticks[k][3] += (uint32_t)(dpf_c - dpf_b);
        g_fp_n[k]++;
    }
    g_dp_cat = f->wt ? 0 : g_dp_cat_miss ? 2 : 1;
    g_dp_cat_miss = 0;
    g_demo_verts += (uint32_t)count;
    g_demo_draws++;
    g_cat_draws[f->wt ? 0 : 4]++;
    g_cat_verts[f->wt ? 0 : 4] += (uint32_t)count;
    DP_T(dpf1);
    g_dp_ticks[4] += (uint32_t)(dpf1 - dpf0);
#endif
    return 1;
}
#endif

static void draw_prim(int prim, int count) {
#ifdef TKDR_AUTOPILOT
    DP_T(dp0);
#endif
    g_prims++;
    g_prim_calls++;
    if ((unsigned)prim < 8) g_prim_type[prim]++;
    if (count <= 0) return;
    if (count > MAX_VERTS) count = MAX_VERTS;
    VFormat f;
#ifdef HLE_TRACE
    uint64_t t_fmt = hle_time_us();
#endif
    /* The vertex format and the pixel state are rebuilt only when what they
       are made from changed.  A fight draws about 5000 primitives a frame,
       five vertices each on average, so per-primitive setup cost as much as
       the vertices; most draws change only bone matrices and pointers. */
    uint32_t vt = R(GE_VERTEXTYPE);
#ifdef HLE_TRACE
    vt_note(vt, count);
#endif
    if (vt != s_vt) {
        vformat(&s_vf, vt);
        s_vt = vt;
#ifdef TKDR_AUTOPILOT
        g_vf_rebuild_n++;
#endif
    }
    f = s_vf;
#ifdef EXP_NODRAW   /* experiment: how fast does the guest run if the GE draws nothing? */
    if (!f.through) {
        if (f.idx) g_ge.iaddr += (uint32_t)count * (f.idx == 1 ? 1u : f.idx == 2 ? 2u : 4u);
        else g_ge.vaddr += (uint32_t)count * (uint32_t)(f.size * f.nmorph);
        return;
    }
#endif
    raster_init();
#ifdef HLE_TRACE
    g_vformat_us += hle_time_us() - t_fmt;
    uint64_t t_state = hle_time_us();
#endif
    /* The generation is read once: a DMA on the guest thread can move it at
       any time, and the state must carry the generation it was built at. */
    uint32_t gen = __atomic_load_n(&g_ge_pix_gen, __ATOMIC_RELAXED);
    int epoch = texrep_epoch();
    /* The primitive is drawn with the cached state itself: copying it (about
       500 bytes) cost more than the comparison that keeps it. */
    const PixState *ps = &s_ps;
    if (s_ps_gen == gen && s_ps_through == f.through && s_ps_epoch == epoch) {
        if (ps->tex.enabled) tex_touch(ps->tex.entry);
#ifdef HLE_TRACE
        g_ps_cache_hits++;
#endif
    } else {
#ifdef HLE_TRACE
        g_ps_cache_misses++;
#endif
#ifdef TKDR_AUTOPILOT
        DP_T(dpp0);
#endif
#ifdef EXP_SLOW2D
        const uint32_t s2d0 = dp_tb_real();
#endif
        pix_state(&s_ps, f.through);
#ifdef EXP_SLOW2D
        s2d_state_t += dp_tb_real() - s2d0;
#endif
#ifdef TKDR_AUTOPILOT
        DP_T(dpp1);
        g_ps_rebuild_ticks += (uint32_t)(dpp1 - dpp0);
        g_ps_rebuild_n++;
#endif
        s_ps_gen = gen;
        s_ps_through = f.through;
        s_ps_epoch = epoch;
    }
    g_draw_pix_gen = s_ps_gen;
#ifdef HLE_TRACE
    g_pixstate_us += hle_time_us() - t_state;
    uint64_t t_begin = hle_time_us();
#endif
    begin_state(ps);
#ifdef HLE_TRACE
    g_beginstate_us += hle_time_us() - t_begin;
#endif
#ifdef TKDR_AUTOPILOT
    DP_T(dp1);
    g_dp_ticks[0] += (uint32_t)(dp1 - dp0);
#endif

    // gather vertices (indexed or sequential)
    uint32_t vbase = g_ge.vaddr;
    int max_index = 0;
#ifdef HLE_TRACE
    uint64_t t_vert = hle_time_us();
#endif
#ifdef __wii__
    int display_target = gpu_is_display_target(ps->tg.fb);
#ifdef TKDR_AUTOPILOT
    if (!display_target) { g_tgt_nd_draws++; g_tgt_nd_fb = ps->tg.fb; }
#endif
    /* GX cannot express the GE's stencil, colour test or a few blend modes.
       In transform mode the primitive is usually 3D geometry: draw it with
       the chip and leave the unsupported *test* off.  It is far better to
       show the geometry with a slightly different edge than to fall into a
       software pass that writes guest VRAM the GX display never shows.
       Through-mode layers are 2D UI/mask passes.  Those were also invisible
       in GX mode before, so skip only the ones the chip cannot represent;
       the visible GX layer stays coherent and the CPU fill disappears. */
    int skip = g_gxr_enabled && display_target && gxr_skip_2d(ps);
#ifdef EXP_ABSTENCIL
    skip |= s_stencil_drop;
    s_stencil_drop = 0;
#endif
    if (skip) {
        /* Even a primitive we do not draw must consume its vertex/index
           stream, or the next primitive reads the wrong vertices.  This was
           what made the following 3D geometry appear untextured/gray. */
        if (f.idx) g_ge.iaddr += (uint32_t)count * (f.idx == 1 ? 1u : f.idx == 2 ? 2u : 4u);
        else g_ge.vaddr += (uint32_t)count * (uint32_t)(f.size * f.nmorph);
#ifdef HLE_TRACE
        g_gxr_skipped++;
#endif
        return;
    }
    /* The chip applies the view and projection matrices itself, so it is
       given the vertices in world space. */
    int use_gx = g_gxr_enabled && display_target;
    g_gxr_object = use_gx && !f.through;
#endif
#ifdef TKDR_AUTOPILOT
    g_demo_verts += (uint32_t)count;
    g_demo_draws++;
    {
        int cat = f.wt ? 0 : f.through ? 1 : (f.idx || f.nmorph != 1) ? 2 : count > VC_MAX_VERTS ? 3 : f.nrm ? 5 : 4;
        g_cat_draws[cat]++;
        g_cat_verts[cat] += (uint32_t)count;
    }
#endif
#ifdef __wii__
    /* A plain draw (not skinned or morphed, not indexed) that is the same as
       it was in an earlier frame comes straight from the draw cache. */
    VcTok vc_tok;
    int vc_try = 0, vc_obj = 0;
#ifdef EXP_NOCACHE
    if (0) {
#else
    if (use_gx && !f.through && !f.wt && f.nmorph == 1 && !f.idx && count <= VC_MAX_VERTS &&
        (R(GE_SHADEMODE) & 1) && prim >= 3 && prim <= 5) {
#endif
        const uint8_t *vp = guest_host_ptr(vbase, (uint32_t)(f.size * count));
        if (vp) {
            /* Without normals, and with lights that do not care where the vertex
               is, the vertices need not know the world matrix: they are kept in
               object space and the chip is given the model-view matrix. */
            vc_obj = !f.nrm && f.pos && (f.col == 0 || f.col >= 4) && gxr_lights_position_free();
            const GxVtx *hit = vc_lookup(vp, vbase, count, f.size, vc_obj ? (vt | 0x80000000u) : vt, f.nrm != 0, vc_obj,
                                         !f.nrm && (f.col == 0 || f.col >= 4), &vc_tok);
            if (hit) {
                if (vc_tok.refresh) vc_alpha_refresh(&f, vp, count, (GxVtx *)hit, vc_tok.ga);
                g_ge.vaddr += (uint32_t)(f.size * count);
                g_gxr_object = 0;
                g_gxr_prims++;
#ifdef TKDR_AUTOPILOT
                g_dp_cat = 1;
#endif
#ifdef TKDR_AUTOPILOT
                DP_T(dp2h);
                g_dp_ticks[1] += (uint32_t)(dp2h - dp1);
#endif
                gxr_prim_packed(ps, prim, count, hit, vc_obj ? gxr_modelview() : NULL, vtx_state_uvt());
                kick();
#ifdef TKDR_AUTOPILOT
                DP_T(dp3h);
                g_dp_ticks[5] += (uint32_t)(dp3h - dp2h);
                g_dp_ticks[4] += (uint32_t)(dp3h - dp0);
#endif
                return;
            }
            vc_try = 1;
#ifdef TKDR_AUTOPILOT
            g_dp_cat = 2;
#endif
            g_vc_objspace = vc_obj;
            g_vc_rawuv = 1;
        }
    }
#endif
#ifdef TKDR_AUTOPILOT
    DP_T(dp2);
    g_dp_ticks[1] += (uint32_t)(dp2 - dp1);
#endif
#ifdef __wii__
    /* A skinned fighter is decoded as a whole into the chip's vertex form. */
    if (use_gx && f.wt && !f.through && !f.idx && f.nmorph == 1 && count <= PK_MAX && (R(GE_SHADEMODE) & 1) &&
        prim >= 3 && prim <= 5) {
        const uint8_t *vp = guest_host_ptr(vbase, (uint32_t)(f.size * count));
        if (vp && decode_skinned_packed(&f, vp, count, g_pk)) {
#ifdef TKDR_AUTOPILOT
            g_dp_cat = 3;
#endif
            g_ge.vaddr += (uint32_t)(f.size * count);
            g_gxr_object = 0;
            g_gxr_prims++;
#ifdef TKDR_AUTOPILOT
            DP_T(dp3s);
            g_dp_ticks[2] += (uint32_t)(dp3s - dp2);
#endif
            gxr_prim_packed(ps, prim, count, g_pk, NULL, NULL);
            kick();
#ifdef TKDR_AUTOPILOT
            DP_T(dp4s);
            g_dp_ticks[3] += (uint32_t)(dp4s - dp3s);
            g_dp_ticks[4] += (uint32_t)(dp4s - dp0);
#endif
            return;
        }
    }
#endif
    for (int i = 0; i < count; i++) {
        int idx = i;
        if (f.idx == 1) idx = mem_r8(g_ge.iaddr + (uint32_t)i);
        else if (f.idx == 2) idx = mem_r16(g_ge.iaddr + (uint32_t)i * 2);
        else if (f.idx == 3) idx = (int)(mem_r32(g_ge.iaddr + (uint32_t)i * 4) & 0xFFFF);
        if (idx > max_index) max_index = idx;
        decode_vertex(&f, vbase + (uint32_t)idx * (uint32_t)(f.size * f.nmorph), &g_verts[i]);
    }
#ifdef HLE_TRACE
    g_vertex_us += hle_time_us() - t_vert;
#endif
#ifdef __wii__
    g_gxr_object = 0;
#endif
    // the vertex/index pointers advance past what was consumed
    if (f.idx) g_ge.iaddr += (uint32_t)count * (f.idx == 1 ? 1u : f.idx == 2 ? 2u : 4u);
    else g_ge.vaddr += (uint32_t)count * (uint32_t)(f.size * f.nmorph);

#ifdef TKDR_AUTOPILOT
    DP_T(dp3);
    g_dp_ticks[2] += (uint32_t)(dp3 - dp2);
    if (g_dp_cat == 5 && use_gx && !f.through) {
        g_dp_cat = 4;
        /* why this draw is not one of the cached or skinned kinds */
        int why = f.idx ? 0 : f.nmorph != 1 ? 1 : !(R(GE_SHADEMODE) & 1) ? 2 : (prim < 3 || prim > 5) ? 3 :
                  f.wt ? 4 : count > VC_MAX_VERTS ? 5 : f.nrm ? 6 : 7;
        g_other_why[why]++;
        g_other_prim[prim & 7]++;
        if (why == 4) {
            const uint32_t key = f.vt | (R(GE_LIGHTINGENABLE) & 1) << 31;
            int i = 0;
            while (i < 3 && g_sk_vt[i] && g_sk_vt[i] != key) i++;
            g_sk_vt[i] = key;
            g_sk_vtn[i]++;
            g_sk_vv[i] += (uint32_t)count;
        }
    }
#endif
    int flat = !(R(GE_SHADEMODE) & 1);
    Vtx *v = g_verts;
#ifdef HLE_TRACE
    list_trace(ps, &f, prim, count, v);
#endif
#ifdef __wii__
    g_vc_objspace = 0;
    g_vc_rawuv = 0;
    if (vc_try) {   /* the cached copy is drawn: the vertices may be in object space, and have raw texture coordinates */
        GxVtx *dst = vc_store(&vc_tok);
        for (int i = 0; i < count; i++) gxr_pack(&dst[i], &v[i]);
        g_gxr_prims++;
        gxr_prim_packed(ps, prim, count, dst, vc_obj ? gxr_modelview() : NULL, vtx_state_uvt());
        kick();
        return;
    }
#endif
#ifdef __wii__
    /* The chip draws into its own frame buffer, so only a display surface
       can go through it.  A texture target (the movie player's decode
       buffer, or any render-to-texture pass) has to be drawn by the
       software rasteriser into guest VRAM, where the later texture reads
       and the guest's own logic expect it. */
    if (use_gx) {
        g_gxr_prims++;
#ifdef HLE_TRACE
        uint64_t t_gx = hle_time_us();
#endif
#ifndef EXP_DECODEONLY   /* experiment: decode the vertices but send nothing to the chip */
#ifdef EXP_SLOW2D
        const uint32_t s2d1 = dp_tb_real();
#endif
        gxr_prim(ps, prim, count, v, f.through, flat);
#ifdef EXP_SLOW2D
        s2d_gx_t += dp_tb_real() - s2d1;
#endif
#endif
#ifdef TKDR_AUTOPILOT
        DP_T(dp4);
        g_dp_ticks[3] += (uint32_t)(dp4 - dp3);
        g_dp_ticks[4] += (uint32_t)(dp4 - dp0);
#endif
#ifdef HLE_TRACE
        g_gxsubmit_us += hle_time_us() - t_gx;
#endif
        kick();
        return;
    }
#ifdef HLE_TRACE
    if (g_gxr_enabled && display_target) g_gxr_sw_prims++;
    if (g_gxr_enabled && !display_target) {
        g_gxr_rtt_prims++;
        g_gxr_rtt_target = ps->tg.fb;
    }
#endif
#endif
#ifdef HLE_TRACE
    uint64_t t_sw = hle_time_us();
#endif
    switch (prim) {
    case 0:  // points
        for (int i = 0; i < count; i++) {
            SVtx s;
            to_screen(&v[i], &s, f.through, ps->tg.rscale);
            submit(JOB_POINT, &s, NULL, NULL, 0);
        }
        break;
    case 1: case 2:  // lines, line strip
        for (int i = 0; i + 1 < count; i += prim == 1 ? 2 : 1) {
            SVtx a, b;
            to_screen(&v[i], &a, f.through, ps->tg.rscale);
            to_screen(&v[i + 1], &b, f.through, ps->tg.rscale);
            submit(JOB_LINE, &a, &b, NULL, 0);
        }
        break;
    case 3:  // triangles
        for (int i = 0; i + 2 < count; i += 3) emit_triangle(ps, &v[i], &v[i + 1], &v[i + 2], 0, flat);
        break;
    case 4:  // strip
        for (int i = 0; i + 2 < count; i++) emit_triangle(ps, &v[i], &v[i + 1], &v[i + 2], i & 1, flat);
        break;
    case 5:  // fan
        for (int i = 1; i + 1 < count; i++) emit_triangle(ps, &v[0], &v[i], &v[i + 1], 0, flat);
        break;
    case 6:  // rectangles
        for (int i = 0; i + 1 < count; i += 2) {
            SVtx a, b;
            to_screen(&v[i], &a, f.through, ps->tg.rscale);
            to_screen(&v[i + 1], &b, f.through, ps->tg.rscale);
            submit(JOB_RECT, &a, &b, NULL, 0);
        }
        break;
    }
#ifdef HLE_TRACE
    g_switch_us += hle_time_us() - t_sw;
#ifdef __wii__
    if (g_gxr_enabled && !display_target) g_gxr_rtt_us += hle_time_us() - t_sw;
#endif
#endif
    (void)max_index;
    kick();
}


/* ---------------------------------------------------------------- the GX path

   The Wii's own graphics chip draws the guest's triangles.  The GE and
   the GX are both fixed function pipelines, so the display lists map
   across almost one to one, and the per-pixel work leaves the CPU: the
   software rasteriser was taking three quarters of the emulated CPU and
   the guest's own code could not get any more of it.

   The vertices are decoded here exactly as for the software path (so the
   morphing, skinning and lighting are shared), except that in the
   transform mode they stay in world space: the chip applies the view and
   projection matrices.  The textures come from the same decoded cache the
   software path samples; the chip's layer converts them once.

   Only the Wii build has the GX, so all of this is compiled out
   elsewhere and the software rasteriser stays the default. */

/* A list, a strip or a fan is cut into pieces of this many vertices.  The
   number is even and a multiple of three: a piece of a triangle list ends
   on a triangle, and a strip's pieces overlap by two vertices and keep the
   parity of the strip. */
#define GXR_CHUNK 1536
static float g_gxr_verts[GXR_CHUNK * 9];

/* A GE stencil state the GX can reproduce: the test always passes, the
   stencil is replaced on a depth pass and kept on the two failure paths.
   The GE stores stencil in the framebuffer alpha bits, so this maps directly
   to GX destination-alpha writes. */
static int gxr_simple_stencil(const PixState *p) {
    return p->stencil_test && !(p->write_mask & 0xFF000000u) &&
           p->st_func == 1 && p->st_sfail == 0 && p->st_zfail == 0 &&
           p->st_zpass == 2;
}

/* The classifier's per-primitive result.  "skip" is only meaningful for
   through-mode layers; transform-mode geometry is always drawn by the chip,
   with the unsupported test/blend approximation disabled. */
typedef struct {
    uint32_t features;
    uint32_t write_mask;
    int path;
    int reason;
} GxrPlan;

enum {
    GXR_PATH_NATIVE = 0,
    GXR_PATH_APPROX,
    GXR_PATH_SKIP,
    GXR_PATH_COUNT
};

enum {
    GXR_FEAT_THROUGH       = 1u << 0,
    GXR_FEAT_TEXTURE       = 1u << 1,
    GXR_FEAT_ALPHA_TEST    = 1u << 2,
    GXR_FEAT_COLOR_TEST    = 1u << 3,
    GXR_FEAT_STENCIL       = 1u << 4,
    GXR_FEAT_SIMPLE_STENCIL = 1u << 5,
    GXR_FEAT_LOGIC         = 1u << 6,
    GXR_FEAT_DITHER        = 1u << 7,
    GXR_FEAT_BLEND         = 1u << 8,
    GXR_FEAT_DBL_ALPHA     = 1u << 9,
    GXR_FEAT_FIXED_SRC     = 1u << 10,
    GXR_FEAT_WRITE_MASK    = 1u << 11,
    GXR_FEAT_ALPHA_ONLY    = 1u << 12,
    GXR_FEAT_FIXED_DST     = 1u << 13,
};

static void gxr_plan_skip(GxrPlan *c, int why) {
    if (c->path != GXR_PATH_SKIP) {
        c->path = GXR_PATH_SKIP;
        c->reason = why;
    }
}

static void gxr_plan_approx(GxrPlan *c) {
    if (c->path == GXR_PATH_NATIVE) c->path = GXR_PATH_APPROX;
}

static void gxr_classify(const PixState *p, GxrPlan *c) {
    memset(c, 0, sizeof *c);
    c->reason = -1;
    c->path = GXR_PATH_NATIVE;
    c->write_mask = p->write_mask;
    if (p->through) c->features |= GXR_FEAT_THROUGH;
    if (p->tex.enabled) c->features |= GXR_FEAT_TEXTURE;

    if (p->alpha_test) {
        c->features |= GXR_FEAT_ALPHA_TEST;
        if (p->at_mask != 0xFF) gxr_plan_skip(c, GXR_REJ_AT_MASK);
    }
    if (p->color_test) {
        c->features |= GXR_FEAT_COLOR_TEST;
        if (p->ct_func != 1) gxr_plan_skip(c, GXR_REJ_COLOR);
    }
    if (p->stencil_test) {
        c->features |= GXR_FEAT_STENCIL;
        if (gxr_simple_stencil(p)) c->features |= GXR_FEAT_SIMPLE_STENCIL;
        else gxr_plan_skip(c, GXR_REJ_STENCIL);
    }
    if (p->logic_test) {
        c->features |= GXR_FEAT_LOGIC;
        /* GX logic ops replace the blend equation; the GE applies logic
           after blending.  Only the logic-only case is exact. */
        if (p->blend) gxr_plan_skip(c, GXR_REJ_LOGIC);
    }
    if (p->dither) c->features |= GXR_FEAT_DITHER;

    uint32_t rgb_mask = p->write_mask & 0xFFFFFFu;
    uint32_t alpha_mask = p->write_mask >> 24;
    if (p->write_mask) c->features |= GXR_FEAT_WRITE_MASK;
    /* Partial RGB/alpha writes have no exact fixed-function GX equivalent.
       They are common in the 2D UI, where a whole-layer skip makes the menu
       invisible.  Draw them with whole-channel writes instead; this is an
       approximation, and the classifier marks it as such. */
    if (rgb_mask != 0 && rgb_mask != 0xFFFFFFu) gxr_plan_approx(c);
    if (alpha_mask != 0 && alpha_mask != 0xFFu) gxr_plan_approx(c);
    if (rgb_mask == 0xFFFFFFu && alpha_mask == 0) c->features |= GXR_FEAT_ALPHA_ONLY;

    if (p->blend) {
        c->features |= GXR_FEAT_BLEND;
        if (p->bl_eq != 0 && p->bl_eq != 2) gxr_plan_skip(c, GXR_REJ_BLEND_EQ);
        /* Destination factors 8/9 double the destination alpha (no GX form
           yet).  10 and up is the fixed destination colour (FIXB): white and
           black are exactly GX ONE and ZERO, which is how the game draws its
           additive 2D effects (source alpha + destination); other colours
           have no GX blend factor. */
        if (p->bl_dst == 8 || p->bl_dst == 9) gxr_plan_skip(c, GXR_REJ_BLEND_FACTOR);
        if (p->bl_dst >= 10) {
            c->features |= GXR_FEAT_FIXED_DST;
            uint32_t fb = p->fix_b & 0xFFFFFFu;
            if (fb != 0 && fb != 0xFFFFFFu) gxr_plan_skip(c, GXR_REJ_BLEND_FACTOR);
        }
        if (p->bl_src >= 8 && p->bl_src < 10) gxr_plan_skip(c, GXR_REJ_BLEND_FACTOR);
        if ((p->bl_src == 6 || p->bl_src == 7 ||
             p->bl_dst == 6 || p->bl_dst == 7) && p->alpha_test) {
            gxr_plan_skip(c, GXR_REJ_DBL_ALPHA);
        } else if (p->bl_src == 6 || p->bl_src == 7 || p->bl_dst == 6 || p->bl_dst == 7) {
            c->features |= GXR_FEAT_DBL_ALPHA;
            gxr_plan_approx(c);
        }
        if (p->bl_src >= 10) {
            c->features |= GXR_FEAT_FIXED_SRC;
            gxr_plan_approx(c);
        }
    }
}

#ifdef HLE_TRACE
uint32_t g_gxr_paths[GXR_PATH_COUNT];
uint32_t g_gxr_feat[14];
uint32_t g_gxr_wm[4];   /* full, alpha-preserved, alpha-only, partial */
static void gxr_plan_note(const GxrPlan *c) {
    if ((unsigned)c->path < GXR_PATH_COUNT) g_gxr_paths[c->path]++;
    for (int i = 0; i < 14; i++)
        if (c->features & (1u << i)) g_gxr_feat[i]++;
    uint32_t rgb = c->write_mask & 0xFFFFFFu;
    uint32_t a = c->write_mask >> 24;
    int wm = 3;
    if (c->write_mask == 0) wm = 0;
    else if (rgb == 0 && a == 0xFF) wm = 1;
    else if (rgb == 0xFFFFFFu && a == 0) wm = 2;
    g_gxr_wm[wm]++;
}
#else
#define gxr_plan_note(c) ((void)0)
#endif

#ifdef HLE_TRACE
/* A census of every GX state, each printed once with the guest frame it first
   appeared in, so an effect screen (the pre-match cards) can be read off the
   log: which blend, masks, tests and depth state it uses, 2D or 3D. */
static void gxr_census(const PixState *p) {
    extern uint32_t g_frames_presented;
    static uint32_t seen[256][3];
    static int nseen;
    uint32_t k0 = (uint32_t)p->blend | (uint32_t)p->bl_eq << 1 | (uint32_t)p->bl_src << 4 |
                  (uint32_t)p->bl_dst << 8 | (uint32_t)p->alpha_test << 12 | (uint32_t)p->at_func << 13 |
                  (uint32_t)p->at_ref << 16 | (uint32_t)p->tex.enabled << 24 |
                  (uint32_t)(p->tex.enabled ? p->tex.fmt : 0) << 25 | (uint32_t)p->stencil_test << 29 |
                  (uint32_t)p->color_test << 30 | (uint32_t)p->through << 31;
    uint32_t k1 = p->write_mask ^ (p->blend && p->bl_dst >= 10 ? p->fix_b * 2654435761u : 0) ^
                  (p->blend && p->bl_src >= 10 ? p->fix_a * 40503u : 0) ^
                  (p->tex.enabled ? (uint32_t)p->tex.func << 3 | (uint32_t)p->tex.rgba : 0);
    uint32_t k2 = (uint32_t)p->z_test | (uint32_t)p->z_func << 1 | (uint32_t)p->z_write << 4 |
                  (uint32_t)p->clear << 5 | (uint32_t)p->clear_color << 6 | (uint32_t)p->clear_alpha << 7 |
                  (uint32_t)p->clear_depth << 8 | (uint32_t)p->logic_test << 9 | (uint32_t)p->fog_enable << 10 |
                  (uint32_t)p->st_func << 12 | (uint32_t)p->st_zpass << 16 | (uint32_t)p->st_ref << 20 |
                  (uint32_t)p->ct_func << 28;
    for (int i = 0; i < nseen; i++)
        if (seen[i][0] == k0 && seen[i][1] == k1 && seen[i][2] == k2) return;
    if (nseen >= 256) return;
    seen[nseen][0] = k0; seen[nseen][1] = k1; seen[nseen][2] = k2; nseen++;
    fprintf(stderr, "GXSTATE2 guest=%u through=%d clear=%d/%d%d%d blend=%d eq=%d src=%d dst=%d fixa=%06X fixb=%06X "
            "atest=%d/%d/%d z=%d/%d/%d mask=%08X tex=%d fmt=%d func=%d rgba=%d clut=%d stencil=%d f%d r%d "
            "ops%d/%d/%d ctest=%d f%d logic=%d/%d fog=%d\n",
            g_frames_presented, p->through, p->clear, p->clear_color, p->clear_alpha, p->clear_depth,
            p->blend, p->bl_eq, p->bl_src, p->bl_dst, p->fix_a & 0xFFFFFFu, p->fix_b & 0xFFFFFFu,
            p->alpha_test, p->at_func, p->at_ref, p->z_test, p->z_func, p->z_write, p->write_mask,
            p->tex.enabled, p->tex.enabled ? p->tex.fmt : -1, p->tex.enabled ? p->tex.func : -1,
            p->tex.enabled ? p->tex.rgba : -1, p->tex.enabled ? p->tex.clut_fmt : -1,
            p->stencil_test, p->st_func, p->st_ref, p->st_sfail, p->st_zfail, p->st_zpass,
            p->color_test, p->ct_func, p->logic_test, p->logic_op, p->fog_enable);
}
#endif

/* The through-mode (2D) layers that the fixed-function chip cannot express.
   These are skipped in GX mode; in CPU mode the software rasteriser still
   executes them.  Transform-mode geometry is never skipped: it is drawn by
   the chip with the unsupported test or blend approximation disabled. */
static int gxr_skip_2d(const PixState *p) {
#ifdef HLE_TRACE
    gxr_census(p);
#endif
    if (!p->through || p->clear) return 0;
    GxrPlan plan;
    gxr_classify(p, &plan);
    gxr_plan_note(&plan);
    if (plan.path != GXR_PATH_SKIP) return 0;
    gxr_reject(plan.reason);
#ifdef HLE_TRACE
    {   /* Each distinct rejected state once: which blend, test or mask the
           layer needed, so the next renderer step is chosen from data. */
        static uint32_t seen[32][3];
        static int nseen;
        uint32_t k0 = (uint32_t)plan.reason | (uint32_t)p->bl_src << 8 | (uint32_t)p->bl_dst << 12 |
                      (uint32_t)p->bl_eq << 16 | (uint32_t)p->blend << 19 | (uint32_t)p->alpha_test << 20 |
                      (uint32_t)p->tex.enabled << 21;
        uint32_t k1 = p->fix_a & 0xFFFFFFu, k2 = p->fix_b & 0xFFFFFFu;
        int known = 0;
        for (int i = 0; i < nseen && !known; i++)
            known = seen[i][0] == k0 && seen[i][1] == k1 && seen[i][2] == k2;
        if (!known && nseen < 32) {
            seen[nseen][0] = k0; seen[nseen][1] = k1; seen[nseen][2] = k2;
            nseen++;
            fprintf(stderr, "GXREJ reason=%d blend=%d eq=%d src=%d dst=%d fixa=%06X fixb=%06X "
                    "atest=%d func=%d ref=%d tex=%d fmt=%d texfunc=%d rgba=%d mask=%08X fb=%08X\n",
                    plan.reason, p->blend, p->bl_eq, p->bl_src, p->bl_dst, k1, k2,
                    p->alpha_test, p->at_func, p->at_ref, p->tex.enabled, p->tex.fmt,
                    p->tex.func, p->tex.rgba, p->write_mask, p->tg.fb);
        }
    }
#endif
    return 1;
}

static __attribute__((optimize("Ofast", "no-thread-jumps"))) void gxr_state(const PixState *ps, int through, int rect, GxrState *g) {
    memset(g, 0, sizeof *g);
    g->blend = ps->blend; g->bl_eq = ps->bl_eq; g->bl_src = ps->bl_src; g->bl_dst = ps->bl_dst;
    g->fix_a = ps->fix_a; g->fix_b = ps->fix_b;
    g->alpha_test = ps->alpha_test; g->at_func = ps->at_func; g->at_ref = ps->at_ref;
    g->at_mask = ps->at_mask;
    g->z_test = ps->z_test; g->z_func = ps->z_func; g->z_write = ps->z_write;
    /* The PSP compares 16-bit depths and a tie passes; the chip compares 24 bits.  A layer that lies on the surface under
       it (ground mist, a decal) and passes there by a tie fails, or flickers, on the chip; a blended layer that does not
       write depth is moved a little toward the eye.  Which way that is depends on the comparison: smaller depths are
       nearer for LESS / LEQUAL, larger ones for GREATER / GEQUAL.  Draws in the through mode (the 2D layers) keep their
       order. */
    if (ZBIAS_ON && !through && !ps->clear && ps->blend && ps->z_test && !ps->z_write) {
        if (ps->z_func == 4 || ps->z_func == 5) g->zbias = -1;
        else if (ps->z_func == 6 || ps->z_func == 7) g->zbias = 1;
    }
#ifdef EXP_FOGDBG
    if (g_fd_zb && ps->z_test && !ps->z_write) g->zbias = 1;   /* the opaque test sheets (the game compares with GEQUAL) */
#endif
#ifdef EXP_BLENDZOFF
    /* test: is a blended layer hidden by something opaque?  Its depth test is dropped (and with it the bias). */
    if (!through && !ps->clear && ps->blend && ps->z_test && !ps->z_write) { g->z_test = 0; g->zbias = 0; }
#endif
#ifndef EXP_NOCULL
    /* Back faces: the GE drops the triangles that wind the wrong way (see cull_reject()), and the
       hair of a fighter has a shell of them behind its strands.  A rectangle (a sprite: two corners) is
       never culled, whichever way round its corners are given: draw_rect() takes their min and max. */
    if (!through && !(rect && RECT_NOCULL) && !ps->clear && (R(GE_CULLFACEENABLE) & 1)) g->cull = (R(GE_CULL) & 1) ? 2 : 1;
#endif
    g->clear = ps->clear; g->clear_color = ps->clear_color;
    g->clear_alpha = ps->clear_alpha; g->clear_depth = ps->clear_depth;
    g->fog_enable = ps->fog_enable;
    g->fog_end = ge_float(R(GE_FOG1));
    g->fog_slope = ge_float(R(GE_FOG2));
    g->fog_color = R(GE_FOGCOLOR) & 0xFFFFFFu;
    g->dbl_src_alpha = ps->blend &&
        (ps->bl_src == 6 || ps->bl_src == 7 || ps->bl_dst == 6 || ps->bl_dst == 7);
    g->logic = ps->logic_test && !ps->blend;   // GX logic replaces the blend equation
    g->logic_op = ps->logic_op;
    g->dither = ps->dither;
    /* The GE masks are "bits not to write": zero means write.  GX has only
       whole-RGB and whole-alpha updates, so exact partial masks are handled
       by the classifier; here all written channels are enabled. */
    uint32_t rgb_mask = ps->write_mask & 0xFFFFFFu;
    uint32_t alpha_mask = ps->write_mask >> 24;
    g->rgb_write = rgb_mask != 0xFFFFFFu;
    /* Destination alpha is the stencil channel.  Write it for an explicit
       alpha-only pass, a partial alpha mask (approximated as a whole-alpha
       write), or the stencil REPLACE case; ordinary colour draws preserve it,
       as the reference GL path does. */
    g->alpha_write = (rgb_mask == 0xFFFFFFu && alpha_mask == 0) ||
                     (alpha_mask != 0 && alpha_mask != 0xFFu);
    if (ps->blend && !gxr_simple_stencil(ps) &&
        !(alpha_mask != 0 && alpha_mask != 0xFFu)) g->alpha_write = 0;
    /* GX has no stencil test.  The simple ALWAYS/REPLACE case can write the
       destination alpha; other cases are skipped for 2D and left off for 3D. */
    g->stencil_alpha = -1;
    if (gxr_simple_stencil(ps)) {
        g->stencil_alpha = ps->st_ref & 0xFF;
        g->alpha_write = 1;
    }
    const TexState *t = &ps->tex;
    g->tex_on = t->enabled && (t->pix != NULL || t->gx_only || t->blit);
    if (g->tex_on) {
        g->tex_func = t->func; g->tex_rgba = t->rgba; g->tex_dbl = t->dbl;
        g->tex_env = (uint32_t)t->env[0] | (uint32_t)t->env[1] << 8 | (uint32_t)t->env[2] << 16;
        g->tex_clamp_u = t->clamp_u; g->tex_clamp_v = t->clamp_v; g->tex_linear = t->linear;
        g->tex_pix = t->pix; g->tex_pw = t->pw; g->tex_ph = t->ph;
        g->tex_w = t->w; g->tex_h = t->h; g->tex_serial = t->serial;
        /* CI4/CI8 textures can stay paletted in GX texture memory when the
           palette is RGB565 (exact in a GX RGB565 TLUT).  Palettes with alpha
           or 8888 entries keep the RGBA8 path until a lossless TLUT format is
           available; RGB5A3 would drop colour/alpha bits without a corpus
           gate proving it acceptable. */
        /* The native CI4/CI8 upload path is kept behind a build flag until a
           corpus scene exercises it.  The captured menu uses 8888 palettes,
           which cannot be represented losslessly by a GX TLUT; forcing them
           through RGB5A3 produced a black screen in the integration test.
           The exact bulk decoder above is independent and stays enabled. */
#if defined(GX_FASTTEX)
        /* Opt-in, lossy: every CI4/CI8 texture stays paletted on the GX, and
           palettes other than 565 go into an RGB5A3 TLUT (5-bit colour when
           opaque, 4-bit colour and 3-bit alpha otherwise). */
        g->tex_native = t->native_ok && (t->fmt == 4 || t->fmt == 5);
#elif defined(GX_NATIVE_TEXTURES)
        g->tex_native = t->native_ok && (t->fmt == 4 || t->fmt == 5) && t->clut_fmt == 0;
#else
        g->tex_native = 0;
#endif
        g->tex_fmt = t->fmt;
        g->tex_swizzle = t->swizzle;
        g->tex_stride = t->stride;
        g->tex_addr = t->addr;
        g->tex_clut_fmt = t->clut_fmt;
        g->tex_clut_shift = t->clut_shift;
        g->tex_clut_mask = t->clut_mask;
        g->tex_clut_off = t->clut_off;
    }
    g->through = through;
}

/* The viewport registers the transform mode needs. */
static void gxr_viewport(float *vp) {
    vp[0] = ge_float(R(GE_VIEWPORTXSCALE)); vp[1] = ge_float(R(GE_VIEWPORTXCENTER));
    vp[2] = ge_float(R(GE_VIEWPORTYSCALE)); vp[3] = ge_float(R(GE_VIEWPORTYCENTER));
    vp[4] = ge_float(R(GE_VIEWPORTZSCALE)); vp[5] = ge_float(R(GE_VIEWPORTZCENTER));
    vp[6] = (float)(R(GE_OFFSETX) & 0xFFFF) / 16.f;
    vp[7] = (float)(R(GE_OFFSETY) & 0xFFFF) / 16.f;
}

/* The screen position of a vertex, for the rectangles, which are drawn in
   screen space: in the transform mode the corners are run through the
   view and projection matrices and the viewport first. */
static void gxr_screen_of(const Vtx *v, SVtx *s, int through) {
    if (through) {
        to_screen(v, s, 1, 1.f);
        return;
    }
    float wp[3] = {v->x, v->y, v->z}, vp[3];
    mul43(g_ge.view, wp, vp);
    const float *p = g_ge.proj;
    Vtx c = *v;
    c.x = p[0] * vp[0] + p[4] * vp[1] + p[8] * vp[2] + p[12];
    c.y = p[1] * vp[0] + p[5] * vp[1] + p[9] * vp[2] + p[13];
    c.z = p[2] * vp[0] + p[6] * vp[1] + p[10] * vp[2] + p[14];
    c.w = p[3] * vp[0] + p[7] * vp[1] + p[11] * vp[2] + p[15];
    to_screen(&c, s, 0, 1.f);
}

/* One vertex of the chip's array: x y z u v r g b a.  The texture
   coordinates are left as the guest gave them (normalised in the
   transform mode, in texels in the "through" mode); the chip's layer
   scales them. */
static void gxr_vertex(float *o, const Vtx *v, const float *col) {
    o[0] = v->x; o[1] = v->y; o[2] = v->z;
    o[3] = v->u; o[4] = v->v;
    o[5] = col[0]; o[6] = col[1]; o[7] = col[2]; o[8] = col[3];
}

/* Triangles with flat shading: every vertex of a triangle takes the colour
   of its last vertex, as the software path's `flat_col` does.  The strips
   and fans are expanded to lists first, since their triangles share
   vertices that would need different colours. */
static void gxr_flat_tris(int prim, int count, const Vtx *v) {
    int n = 0;
    for (int t = 0;; t++) {
        const Vtx *a, *b, *c;
        if (prim == 3) {            // triangle list: three vertices each
            if (3 * t + 2 >= count) break;
            a = &v[3 * t]; b = a + 1; c = a + 2;
        } else if (prim == 4) {     // strip: each vertex starts a triangle
            if (t + 2 >= count) break;
            a = &v[t]; b = &v[t + 1]; c = &v[t + 2];
        } else {                    // fan: the first vertex is the hub
            if (t + 2 >= count) break;
            a = &v[0]; b = &v[t + 1]; c = &v[t + 2];
        }
        if (n + 3 > GXR_CHUNK) {
            wii_gxscene_draw(0, g_gxr_verts, n);
            n = 0;
        }
        gxr_vertex(g_gxr_verts + n * 9, a, c->c);
        gxr_vertex(g_gxr_verts + (n + 1) * 9, b, c->c);
        gxr_vertex(g_gxr_verts + (n + 2) * 9, c, c->c);
        n += 3;
    }
    if (n >= 3) wii_gxscene_draw(0, g_gxr_verts, n);
}

#ifdef TKDR_AUTOPILOT
/* Debug: the clears of the current frame, in order, with the number of primitives that came before each and
   whether the rasteriser skipped it (a clear after drawing).  Printed with the dark-frame report. */
static struct { uint32_t after, flags; int skipped; } s_clr_ev[24];
static int s_clr_n, s_clr_over;
static uint32_t s_clr_prims;
static void gpu_clear_note(const PixState *p, int skipped) {
    if (s_clr_n < 24) {
        s_clr_ev[s_clr_n].after = s_clr_prims;
        s_clr_ev[s_clr_n].flags = (uint32_t)p->clear_color | (uint32_t)p->clear_alpha << 1 | (uint32_t)p->clear_depth << 2;
        s_clr_ev[s_clr_n].skipped = skipped;
        s_clr_n++;
    } else s_clr_over++;
}
void gpu_clear_dump(const char *why) {
    fprintf(stderr, "CLEARS %s: %u prims, %d clears%s:", why, s_clr_prims, s_clr_n + s_clr_over, s_clr_over ? " (more not listed)" : "");
    for (int i = 0; i < s_clr_n; i++)
        fprintf(stderr, " [after %u prims, %s%s%s%s]", s_clr_ev[i].after, s_clr_ev[i].flags & 1 ? "C" : "-", s_clr_ev[i].flags & 2 ? "A" : "-",
                s_clr_ev[i].flags & 4 ? "D" : "-", s_clr_ev[i].skipped ? " SKIPPED" : "");
    fprintf(stderr, "\n");
}
void gpu_clear_reset(void) { s_clr_n = s_clr_over = 0; s_clr_prims = 0; }
#endif

#ifdef EXP_XFORMCENSUS
/* The transform-mode (3D) draws over each 300 presented frames, by what the chip can do with their pixel state:
   gxr_classify()'s verdict for each (native; approximated; "skip", which for a 2D layer would drop it but for geometry
   means it is drawn with the part the chip lacks left out), the features they use, and the translucent states (blended
   without a depth write, which is what a fog layer is) in full, most used first.  Called for every draw, cached or not. */
static void xform_census(const PixState *ps, int prim, int through) {
    extern uint32_t g_frames_presented;
    typedef struct { uint32_t k[4], n; int path, reason, prim, cull, tgm; PixState s; } XfEnt;
    static XfEnt ent[96];
    static int nent;
    static uint32_t last, total, blended, transl, path[3], reasons[8], feat[14], overflow, texgen;
    if (through || ps->clear) return;
#ifdef EXP_TEXALPHA
    if (ps->blend && ps->tex.enabled && ps->tex.pix && !ps->z_write) {
        /* the alpha channel of each texture a translucent draw uses, once: is there a soft ramp or only 0 and 255 */
        static uint32_t seen[128];
        static int nseen;
        int k = 0;
        while (k < nseen && seen[k] != ps->tex.serial) k++;
        if (k == nseen && nseen < 128) {
            seen[nseen++] = ps->tex.serial;
            const uint32_t *p = ps->tex.pix;
            const int w = ps->tex.w, h = ps->tex.h, pw = ps->tex.pw;
            uint32_t hist[5] = {0, 0, 0, 0, 0};
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    const uint32_t a = p[(size_t)y * (size_t)pw + (size_t)x] >> 24;
                    hist[a == 0 ? 0 : a < 64 ? 1 : a < 192 ? 2 : a < 255 ? 3 : 4]++;
                }
            fprintf(stderr, "TEXA serial=%u %dx%d fmt=%d clutfmt=%d blend src=%d dst=%d fixb=%06X rgba=%d func=%d atest=%d/%d/%d a0=%u a1-63=%u a64-191=%u a192-254=%u a255=%u row:",
                    ps->tex.serial, w, h, ps->tex.fmt, ps->tex.clut_fmt, ps->bl_src, ps->bl_dst, ps->fix_b & 0xFFFFFFu, ps->tex.rgba,
                    ps->tex.func, ps->alpha_test, ps->at_func, ps->at_ref, hist[0], hist[1], hist[2], hist[3], hist[4]);
            for (int x = 0; x < w; x += (w >= 16 ? w / 16 : 1))
                fprintf(stderr, " %02X", p[(size_t)(h / 2) * (size_t)pw + (size_t)x] >> 24);
            fprintf(stderr, "\n");
        }
    }
#endif
#ifdef EXP_STENCILSEQ
    {   /* one frame in each 300: the draws that use the stencil, or blend without a depth write, in the order they come
           (needs -DEXP_NOSETUPSKIP, or the fast path does not call this for a draw whose state is the last one's) */
        static uint32_t seq_frame = ~0u, seq_all, seq_runs, run_start, run_n, run_key;
        static char run_txt[240];
        if (g_frames_presented % 300 == 150) {
            if (seq_frame != g_frames_presented) {
                seq_frame = g_frames_presented; seq_all = seq_runs = run_n = 0;
                fprintf(stderr, "STSEQ frame %u\n", g_frames_presented);
            }
            /* a run: the draws in a row with the same stencil / blend / depth state (the texture size of the first is shown) */
            const uint32_t key = (uint32_t)ps->stencil_test | (uint32_t)ps->st_func << 1 | (uint32_t)ps->st_ref << 4 |
                                 (uint32_t)ps->st_zpass << 12 | (uint32_t)ps->blend << 15 | (uint32_t)ps->bl_src << 16 |
                                 (uint32_t)ps->bl_dst << 20 | (uint32_t)ps->z_write << 24 | (uint32_t)ps->z_func << 25 |
                                 (uint32_t)ps->z_test << 28;
            if (run_n && key != run_key) {
                if (seq_runs++ < 300) fprintf(stderr, "STSEQ #%u x%u %s\n", run_start, run_n, run_txt);
                run_n = 0;
            }
            if (!run_n) {
                run_key = key; run_start = seq_all;
                snprintf(run_txt, sizeof run_txt, "stencil=%d f%d r%d m%02X ops%d/%d/%d blend=%d src=%d dst=%d z=%d/%d/%d cull=%d first tex %dx%d fmt%d at%d/%d",
                         ps->stencil_test, ps->st_func, ps->st_ref, ps->st_mask, ps->st_sfail, ps->st_zfail, ps->st_zpass, ps->blend,
                         ps->bl_src, ps->bl_dst, ps->z_test, ps->z_func, ps->z_write, (R(GE_CULLFACEENABLE) & 1) ? 1 + (R(GE_CULL) & 1) : 0,
                         ps->tex.enabled ? ps->tex.w : 0, ps->tex.enabled ? ps->tex.h : 0, ps->tex.enabled ? ps->tex.fmt : -1,
                         ps->alpha_test, ps->at_func);
            }
            run_n++;
            seq_all++;
        }
    }
#endif
    /* the texture mapping mode: bits 0-1 the source (0 the vertex's coordinates, 1 the texture matrix, 2 the environment
       map), bits 8-9 what the matrix takes (position, coordinates, normalised normal, normal).  Nothing reads it. */
    const int tgm = ps->tex.enabled ? (int)(R(GE_TEXMAPMODE) & 0x303u) : 0;
    texgen += (tgm & 3) != 0;
    if (tgm == 0x101) {
        /* the texture matrix draws: per layer (the texture and the matrix's scale slots), the matrix and the scale and offset the
           renderers use, every 20th frame, so that a slide (or a reset) shows in the offsets */
        static struct { uint32_t key; uint32_t last; } mt[8];
        static int nmt;
        const float *m = g_ge.tgen;
        const uint32_t key = (uint32_t)ps->tex.w << 16 ^ (uint32_t)ps->tex.h ^ ((uint32_t)(m[0] * 1000.f) * 2654435761u) ^
                             ((uint32_t)(m[4] * 1000.f) * 40503u) ^ ((uint32_t)ps->blend << 5);
        int i = 0;
        while (i < nmt && mt[i].key != key) i++;
        if (i == nmt && nmt < 8) { mt[nmt].key = key; mt[nmt].last = ~0u; nmt++; }
        if (i < nmt && g_frames_presented % 20 == 0 && mt[i].last != g_frames_presented) {
            mt[i].last = g_frames_presented;
            const VtxState *vs = vtx_state();
            const int folded = m[1] == 0.f && m[2] == 0.f && m[3] == 0.f && m[5] == 0.f && m[11] != 0.f;
            fprintf(stderr, "TEXMTX f=%u layer=%d tex=%dx%d wrap=%d blend=%d src=%d dst=%d zw=%d m=[%g %g %g | %g %g %g | %g %g %g | %g %g %g] "
                    "suvt=%g %g %g %g folded=%d\n", g_frames_presented, i, ps->tex.w, ps->tex.h, (int)(R(GE_TEXWRAP) & 0x101u), ps->blend,
                    ps->bl_src, ps->bl_dst, ps->z_write, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11],
                    vs->tex_su, vs->tex_sv, vs->tex_ou, vs->tex_ov, folded);
        }
    }
    if (ps->tex.enabled && ps->blend && ps->z_test && !ps->z_write && g_frames_presented % 10 == 0) {
        /* the translucent depth-tested draws (mist cards, grass, ...): the texture and the scale and offset in use, every 10th
           frame, one line per distinct texture and offsets (needs -DEXP_NOSETUPSKIP, or draws that keep the last state are
           not seen) */
        static uint32_t uv_frame = ~0u, uv_n, uv_seen[48];
        const VtxState *vs = vtx_state();
        if (uv_frame != g_frames_presented) { uv_frame = g_frames_presented; uv_n = 0; }
        uint32_t b[4], h = (uint32_t)ps->tex.addr * 2654435761u;
        memcpy(b, &vs->tex_su, sizeof b);
        for (int k = 0; k < 4; k++) h = (h ^ b[k]) * 16777619u;
        uint32_t k = 0;
        while (k < uv_n && uv_seen[k] != h) k++;
        if (k == uv_n && uv_n < 48) {
            uv_seen[uv_n++] = h;
            fprintf(stderr, "UVOFF f=%u tex=%08X %dx%d fmt=%d wrap=%03X su=%g sv=%g ou=%g ov=%g tgm=%03X cull=%d at=%d/%d src=%d dst=%d\n",
                    g_frames_presented, (unsigned)ps->tex.addr, ps->tex.w, ps->tex.h, ps->tex.fmt, (unsigned)(R(GE_TEXWRAP) & 0x101u),
                    vs->tex_su, vs->tex_sv, vs->tex_ou, vs->tex_ov, tgm, (R(GE_CULLFACEENABLE) & 1) ? 1 + (int)(R(GE_CULL) & 1) : 0,
                    ps->alpha_test, ps->at_func, ps->bl_src, ps->bl_dst);
        }
    }
    GxrPlan plan;
    gxr_classify(ps, &plan);
    total++;
    blended += ps->blend != 0;
    path[plan.path < 3 ? plan.path : 0]++;
    if (plan.path == GXR_PATH_SKIP && plan.reason >= 0 && plan.reason < 8) reasons[plan.reason]++;
    for (int i = 0; i < 14; i++)
        if (plan.features & (1u << i)) feat[i]++;
    const int tr = ps->blend && !ps->z_write;
    transl += tr;
    if (tr || plan.path != GXR_PATH_NATIVE || (tgm & 3) || prim == 6) {
        const uint32_t k0 = (uint32_t)ps->blend | (uint32_t)ps->bl_eq << 1 | (uint32_t)ps->bl_src << 4 | (uint32_t)ps->bl_dst << 8 |
                            (uint32_t)ps->alpha_test << 12 | (uint32_t)ps->at_func << 13 | (uint32_t)ps->at_ref << 16 |
                            (uint32_t)ps->tex.enabled << 24 | (uint32_t)(ps->tex.enabled ? ps->tex.fmt : 0) << 25 |
                            (uint32_t)ps->stencil_test << 29 | (uint32_t)ps->color_test << 30;
        const uint32_t k1 = ps->write_mask ^ (ps->blend && ps->bl_dst >= 10 ? ps->fix_b * 2654435761u : 0) ^
                            (ps->blend && ps->bl_src >= 10 ? ps->fix_a * 40503u : 0) ^
                            (ps->tex.enabled ? (uint32_t)ps->tex.func << 3 | (uint32_t)ps->tex.rgba : 0);
        const uint32_t k2 = (uint32_t)ps->z_test | (uint32_t)ps->z_func << 1 | (uint32_t)ps->z_write << 4 |
                            (uint32_t)ps->logic_test << 9 | (uint32_t)ps->fog_enable << 10 | (uint32_t)ps->st_func << 12 |
                            (uint32_t)ps->st_zpass << 16 | (uint32_t)ps->st_ref << 20 | (uint32_t)ps->ct_func << 28;
        const int cull = (R(GE_CULLFACEENABLE) & 1) ? 1 + (R(GE_CULL) & 1) : 0;
        const uint32_t k3 = ((uint32_t)prim | (uint32_t)cull << 3 | (uint32_t)(ps->tex.enabled ? ps->tex.w : 0) << 5 |
                             (uint32_t)(ps->tex.enabled ? ps->tex.h : 0) << 17) ^ ((uint32_t)tgm * 2654435761u);
        int i = 0;
        while (i < nent && !(ent[i].k[0] == k0 && ent[i].k[1] == k1 && ent[i].k[2] == k2 && ent[i].k[3] == k3)) i++;
        if (i == nent) {
            if (nent < 96) {
                ent[nent].k[0] = k0; ent[nent].k[1] = k1; ent[nent].k[2] = k2; ent[nent].k[3] = k3;
                ent[nent].n = 0; ent[nent].path = plan.path; ent[nent].reason = plan.reason; ent[nent].prim = prim;
                ent[nent].cull = cull; ent[nent].tgm = tgm; ent[nent].s = *ps;
                nent++;
            } else {
                overflow++;
                i = -1;
            }
        }
        if (i >= 0) ent[i].n++;
    }
    if (g_frames_presented - last >= 300) {
        fprintf(stderr, "XFCENSUS f=%u 3D draws %u: blended %u, translucent %u; chip native %u approx %u skip %u; "
                "features stencil %u (simple %u) ctest %u logic %u atest %u wmask %u blend %u dblalpha %u fixsrc %u fixdst %u "
                "alpha-only %u texture %u texgen %u; skip reasons colour %u atmask %u wmask %u stencil %u logic %u eq %u factor %u dbl %u; "
                "%d states%s\n",
                g_frames_presented, total, blended, transl, path[0], path[1], path[2], feat[4], feat[5], feat[3], feat[6], feat[2],
                feat[11], feat[8], feat[9], feat[10], feat[13], feat[12], feat[1], texgen, reasons[0], reasons[1], reasons[2],
                reasons[3], reasons[4], reasons[5], reasons[6], reasons[7], nent, overflow ? " (table full)" : "");
        for (int shown = 0; shown < 12; shown++) {
            int best = -1;
            for (int j = 0; j < nent; j++)
                if (ent[j].n && (best < 0 || ent[j].n > ent[best].n)) best = j;
            if (best < 0) break;
            const XfEnt *e = &ent[best];
            const PixState *p = &e->s;
            fprintf(stderr, "XFSTATE n=%u %s%s prim=%d cull=%d texmap=%03X blend=%d eq=%d src=%d dst=%d fixa=%06X fixb=%06X atest=%d/%d/%d/%u z=%d/%d/%d "
                    "wmask=%08X tex=%d fmt=%d func=%d rgba=%d %dx%d stencil=%d f%d r%d ops%d/%d/%d ctest=%d f%d logic=%d fog=%d\n",
                    e->n, e->path == GXR_PATH_NATIVE ? "native" : e->path == GXR_PATH_APPROX ? "approx" : "SKIP",
                    e->reason >= 0 ? "!" : "", e->prim, e->cull, e->tgm, p->blend, p->bl_eq, p->bl_src, p->bl_dst, p->fix_a & 0xFFFFFFu,
                    p->fix_b & 0xFFFFFFu, p->alpha_test, p->at_func, p->at_ref, p->at_mask, p->z_test, p->z_func, p->z_write,
                    p->write_mask, p->tex.enabled, p->tex.enabled ? p->tex.fmt : -1, p->tex.enabled ? p->tex.func : -1,
                    p->tex.enabled ? p->tex.rgba : -1, p->tex.enabled ? p->tex.w : 0, p->tex.enabled ? p->tex.h : 0,
                    p->stencil_test, p->st_func, p->st_ref, p->st_sfail, p->st_zfail, p->st_zpass, p->color_test, p->ct_func,
                    p->logic_test, p->fog_enable);
            ent[best].n = 0;
        }
        last = g_frames_presented;
        nent = 0;
        total = blended = transl = overflow = texgen = 0;
        memset(path, 0, sizeof path);
        memset(reasons, 0, sizeof reasons);
        memset(feat, 0, sizeof feat);
    }
}
#endif

/* Draw one primitive with the GX. */
/* The chip state a primitive is drawn with: view, scissor and the pixel
   state.  0 when the primitive is not drawn. */
static __attribute__((optimize("Ofast", "no-thread-jumps"))) int gxr_prim_setup(const PixState *ps, int prim, int through) {
    if (prim < 3 || prim > 6) return 0;   // points and lines are not drawn by the chip
    /* The guest's clears target its own framebuffer, which it uses as a
       back buffer.  A clear at the start of a display list is the
       background of this frame and is safe (and sometimes essential: the
       logo fades use it).  One that arrives after the list has drawn is
       the guest clearing its other back buffer, and must not wipe the
       chip's frame buffer before the deferred display copy. */
#ifdef TKDR_AUTOPILOT
    s_clr_prims++;
    if (ps->clear) gpu_clear_note(ps, wii_gxscene_frame_started());
#endif
#ifdef EXP_DRAWTRACE
    {   /* the skinned draws of every sixth guest frame while the autopilot's trace window is open */
        extern volatile int g_dt_window;
        extern uint32_t g_frames_presented;
        static uint32_t last_frame, idx;
        const uint32_t vt_ = R(GE_VERTEXTYPE);
        if (g_dt_window && g_frames_presented % 6 == 0 && (vt_ >> 9 & 3) && !through) {
            if (last_frame != g_frames_presented) { last_frame = g_frames_presented; idx = 0; }
            fprintf(stderr, "DT f=%u #%u prim=%d vt=%06X va=%08X tex=%d:%08X %dx%d fmt%d z=%d/%d/%d at=%d/%d/%d bl=%d/%d/%d/%d lit=%u cull=%u mask=%06X func=%d tcc=%d dbl=%d\n",
                    g_frames_presented, idx++, prim, vt_, g_ge.vaddr, ps->tex.enabled, ps->tex.enabled ? ps->tex.addr : 0u, ps->tex.w, ps->tex.h, ps->tex.fmt,
                    ps->z_test, ps->z_func, ps->z_write, ps->alpha_test, ps->at_func, ps->at_ref, ps->blend, ps->bl_src, ps->bl_dst, ps->bl_eq,
                    R(GE_LIGHTINGENABLE) & 1, R(GE_CULLFACEENABLE) & 1, ps->write_mask & 0xFFFFFFu, ps->tex.func, ps->tex.rgba, ps->tex.dbl);
            if (ps->tex.enabled && ps->alpha_test && ps->tex.w == 32 && ps->tex.h == 64 && ps->tex.pix) {   /* a hair strand texture, once */
                static int dumped;
                if (!dumped++) for (int y_ = 0; y_ < ps->tex.ph; y_++) {
                    fprintf(stderr, "TX %d:", y_);
                    for (int x_ = 0; x_ < ps->tex.pw; x_++) fprintf(stderr, " %08X", ps->tex.pix[y_ * ps->tex.pw + x_]);
                    fprintf(stderr, "\n");
                }
            }
        }
    }
#endif
    if (ps->clear && wii_gxscene_frame_started()) return 0;
    wii_gxscene_dirty();
#ifdef EXP_XFORMCENSUS
    xform_census(ps, prim, through);
#endif

    /* The steps below are no-ops when what they are made from and the chip
       are as they were after the last time: the epoch is read before them,
       so a step that changed the chip is done once more (a no-op) first. */
    static struct {
        uint32_t epoch, pix_gen, view_gen, tex_serial;
        const uint32_t *tex_pix;
        int screen, through, native, sc[4], valid;
    } s_su;
    const Target *t = &ps->tg;
    const int screen = prim == 6 || through;
    const uint32_t epoch = g_gx_epoch;
#ifdef EXP_NOSETUPSKIP
    s_su.valid = 0;
#endif
    if (__builtin_expect(s_su.valid && s_su.epoch == epoch && s_su.pix_gen == g_draw_pix_gen &&
                         s_su.view_gen == g_ge_view_gen && s_su.screen == screen &&
                         s_su.through == through && s_su.tex_serial == ps->tex.serial &&
                         s_su.tex_pix == ps->tex.pix && s_su.native == ps->tex.native_ok &&
                         s_su.sc[0] == t->sx1 && s_su.sc[1] == t->sy1 &&
                         s_su.sc[2] == t->sx2 && s_su.sc[3] == t->sy2, 1))
        return 1;

    if (screen) {
        wii_gxscene_set_screen();
    } else {
        /* The viewport registers are converted, and the matrices compared,
           only when one of them was written. */
        static float vp[8];
        static uint32_t s_vp_gen;
        if (s_vp_gen != g_ge_view_gen) {
            gxr_viewport(vp);
            s_vp_gen = g_ge_view_gen;
        }
#ifdef TKDR_AUTOPILOT
        { extern void gpu_snap_first3d(int, int); gpu_snap_first3d(prim, 0); }
#endif
        wii_gxscene_set_world(g_ge.view, g_ge.proj, vp, g_ge_view_gen);
    }
    wii_gxscene_set_scissor(t->sx1, t->sy1, t->sx2, t->sy2);
    /* Rebuilt with the pixel state it is made from (see draw_prim).  The
       texture pointer/serial is checked as well: a texture-pack reload or a
       cache eviction can change it without changing a GE register. */
    static GxrState s_gs;
    static uint32_t s_gs_gen;
    static int s_gs_through = -1, s_gs_rect = -1, s_gs_zb = -1;
    static uint32_t s_gs_tex_serial;
    static const uint32_t *s_gs_tex_pix;
    static int s_gs_tex_native = -1;
    if (s_gs_gen != g_draw_pix_gen || s_gs_through != through || s_gs_rect != (prim == 6) * (1 + RECT_NOCULL) ||
        s_gs_zb != ZBIAS_ON || s_gs_tex_serial != ps->tex.serial || s_gs_tex_pix != ps->tex.pix ||
        s_gs_tex_native != ps->tex.native_ok) {
        static uint32_t s_gs_version;
        gxr_state(ps, through, prim == 6, &s_gs);
        if (!++s_gs_version) s_gs_version = 1;
        s_gs.version = s_gs_version;
        s_gs_gen = g_draw_pix_gen;
        s_gs_through = through;
        s_gs_rect = (prim == 6) * (1 + RECT_NOCULL);
        s_gs_zb = ZBIAS_ON;
        s_gs_tex_serial = ps->tex.serial;
        s_gs_tex_pix = ps->tex.pix;
        s_gs_tex_native = ps->tex.native_ok;
    }
    wii_gxscene_set_state(&s_gs);
    s_su.epoch = epoch;
    s_su.pix_gen = g_draw_pix_gen;
    s_su.view_gen = g_ge_view_gen;
    s_su.tex_serial = ps->tex.serial;
    s_su.tex_pix = ps->tex.pix;
    s_su.screen = screen;
    s_su.through = through;
    s_su.native = ps->tex.native_ok;
    s_su.sc[0] = t->sx1; s_su.sc[1] = t->sy1; s_su.sc[2] = t->sx2; s_su.sc[3] = t->sy2;
    s_su.valid = 1;
    return 1;
}

static void gxr_pack(GxVtx *o, const Vtx *v) {
    /* the same rounding as the chip layer's c8() */
#define PACK8(c_) ((uint32_t)(uint8_t)((c_) <= 0.f ? 0.f : (c_) >= 255.f ? 255.f : (c_) + 0.5f))
    o->x = v->x; o->y = v->y; o->z = v->z;
    o->u = v->u; o->v = v->v;
    o->rgba = PACK8(v->c[0]) << 24 | PACK8(v->c[1]) << 16 | PACK8(v->c[2]) << 8 | PACK8(v->c[3]);
#undef PACK8
}

/* A draw from the cache: vertices in the form the chip is sent. */
static void gxr_prim_packed(const PixState *ps, int prim, int count, const GxVtx *v, const float *mv, const float *uvt) {
    if (!gxr_prim_setup(ps, prim, 0)) return;
    wii_gxscene_draw_packed(prim - 3, v, count, mv, uvt);
}

#ifdef EXP_RECTCENSUS
/* The rectangles (sprites) the guest draws, over each 300 presented frames: how many in the transform mode and in
   the through mode, how many of the transform-mode ones have a corner behind the eye (w <= 0, which the screen
   mapping turns into a mirrored position) or far outside the picture, and the pixel state they come with. */
static void rect_census(const Vtx *va, const Vtx *vb, const SVtx *a, const SVtx *b, int through, const PixState *ps) {
    extern uint32_t g_frames_presented;
    static uint32_t last, n[2], behind, far_, blend, ztest, fog, tex, shown, gxcull, cullon, flipped;
    static float zlo = 1e9f, zhi = -1e9f;
    n[through != 0]++;
    if (!through) {
        /* what the chip's face culling would have done to the quad (a, b) as the strip it is drawn as: the area of
           its first triangle is dx * dy, and the GE's cull rule is cull_reject()'s */
        const float area = (b->x - a->x) * (b->y - a->y);
        const int on = (R(GE_CULLFACEENABLE) & 1) != 0, ccw = area > 0.f;
        cullon += on;
        flipped += !ccw;
        gxcull += on && ((R(GE_CULL) & 1) ? !ccw : ccw);
        const int bh = a->iw <= 0.f || b->iw <= 0.f;
        const int ff = fabsf(a->x) > 2000.f || fabsf(a->y) > 2000.f || fabsf(b->x) > 2000.f || fabsf(b->y) > 2000.f;
        behind += bh;
        far_ += ff;
        blend += ps->blend != 0;
        ztest += ps->z_test != 0;
        fog += (R(GE_FOGENABLE) & 1) != 0;
        tex += ps->tex.enabled != 0;
        if (b->z < zlo) zlo = b->z;
        if (b->z > zhi) zhi = b->z;
        {   /* the first sprites of each window, in full: where they land, what colour and texture they carry */
            static uint32_t spr_frame, spr_n;
            if (spr_frame != g_frames_presented / 300) { spr_frame = g_frames_presented / 300; spr_n = 0; }
            if (spr_n < 10) {
                spr_n++;
                fprintf(stderr, "RECTSPR f=%u a=(%.1f %.1f) b=(%.1f %.1f) z=%.0f col=%.0f,%.0f,%.0f,%.0f uv=(%.3f %.3f)-(%.3f %.3f) tex=%d:%08X fmt%d %dx%d func=%d rgba=%d clut%d blend=%d eq%d %d/%d atest=%d/%d/%d z=%d/%d/%d fog=%d wmask=%08X\n",
                        g_frames_presented, (double)a->x, (double)a->y, (double)b->x, (double)b->y, (double)b->z,
                        (double)vb->c[0], (double)vb->c[1], (double)vb->c[2], (double)vb->c[3],
                        (double)va->u, (double)va->v, (double)vb->u, (double)vb->v,
                        ps->tex.enabled, ps->tex.enabled ? ps->tex.addr : 0u, ps->tex.fmt, ps->tex.w, ps->tex.h, ps->tex.func, ps->tex.rgba,
                        ps->tex.clut_fmt, ps->blend, ps->bl_eq, ps->bl_src, ps->bl_dst, ps->alpha_test, ps->at_func, ps->at_ref,
                        ps->z_test, ps->z_func, ps->z_write, ps->fog_enable, ps->write_mask);
            }
        }
        if ((bh || ff) && shown < 12) {
            shown++;
            fprintf(stderr, "RECTODD f=%u a=(%g %g %g iw %g) b=(%g %g %g iw %g) world a=(%g %g %g) b=(%g %g %g) blend=%d z=%d/%d tex=%d\n",
                    g_frames_presented, (double)a->x, (double)a->y, (double)a->z, (double)a->iw, (double)b->x, (double)b->y,
                    (double)b->z, (double)b->iw, (double)va->x, (double)va->y, (double)va->z, (double)vb->x, (double)vb->y,
                    (double)vb->z, ps->blend, ps->z_test, ps->z_func, ps->tex.enabled);
        }
    }
    if (g_frames_presented - last >= 300) {
        if (n[0] || n[1])
            fprintf(stderr, "RECTCENSUS f=%u transform %u (behind %u, far %u; blend %u, ztest %u, fog %u, tex %u; z %g..%g; cull on %u, corners flipped %u, chip would cull %u) through %u\n",
                    g_frames_presented, n[0], behind, far_, blend, ztest, fog, tex, (double)zlo, (double)zhi, cullon, flipped, gxcull, n[1]);
        last = g_frames_presented;
        n[0] = n[1] = behind = far_ = blend = ztest = fog = tex = cullon = flipped = gxcull = 0;
        zlo = 1e9f; zhi = -1e9f;
    }
}
#endif

static __attribute__((optimize("Ofast", "no-thread-jumps"))) void gxr_prim(const PixState *ps, int prim, int count, Vtx *v, int through, int flat) {
    if (!gxr_prim_setup(ps, prim, through)) return;

    if (prim == 6) {
        /* Rectangles: two corners, drawn as a screen space quad with the
           colour and the depth of the second corner. */
        for (int i = 0; i + 1 < count; i += 2) {
            SVtx a, b;
            gxr_screen_of(&v[i], &a, through);
            gxr_screen_of(&v[i + 1], &b, through);
            const Vtx *va = &v[i], *vb = &v[i + 1];
#ifdef EXP_RECTCENSUS
            rect_census(va, vb, &a, &b, through, ps);
#endif
            const float cx[4] = {a.x, b.x, a.x, b.x}, cy[4] = {a.y, a.y, b.y, b.y};
            const float cu[4] = {va->u, vb->u, va->u, vb->u}, cv[4] = {va->v, va->v, vb->v, vb->v};
            for (int k = 0; k < 4; k++) {
                float *o = g_gxr_verts + k * 9;
                o[0] = cx[k]; o[1] = cy[k]; o[2] = b.z;
                o[3] = cu[k]; o[4] = cv[k];
                o[5] = vb->c[0]; o[6] = vb->c[1]; o[7] = vb->c[2]; o[8] = vb->c[3];
            }
            wii_gxscene_draw(1, g_gxr_verts, 4);
        }
        return;
    }

    if (flat) {
        gxr_flat_tris(prim, count, v);
        return;
    }
    switch (prim) {
    case 3:   // triangles
        for (int i = 0; i + 3 <= count; ) {
            int n = count - i < GXR_CHUNK ? (count - i) / 3 * 3 : GXR_CHUNK;
            for (int k = 0; k < n; k++) gxr_vertex(g_gxr_verts + k * 9, &v[i + k], v[i + k].c);
            wii_gxscene_draw(0, g_gxr_verts, n);
            i += n;
        }
        break;
    case 4:   // strip: the pieces overlap by two vertices
        for (int i = 0; i + 3 <= count; i += GXR_CHUNK - 2) {
            int n = count - i < GXR_CHUNK ? count - i : GXR_CHUNK;
            for (int k = 0; k < n; k++) gxr_vertex(g_gxr_verts + k * 9, &v[i + k], v[i + k].c);
            wii_gxscene_draw(1, g_gxr_verts, n);
            if (n < GXR_CHUNK) break;
        }
        break;
    default:  // fan: every piece starts from the first vertex
        for (int i = 1; i + 2 <= count; i += GXR_CHUNK - 2) {
            int n = count - i + 1 < GXR_CHUNK ? count - i + 1 : GXR_CHUNK;   // vertices, with the hub
            gxr_vertex(g_gxr_verts, &v[0], v[0].c);
            for (int k = 1; k < n; k++) gxr_vertex(g_gxr_verts + k * 9, &v[i + k - 1], v[i + k - 1].c);
            wii_gxscene_draw(2, g_gxr_verts, n);
            if (n < GXR_CHUNK) break;
        }
        break;
    }
}

// ---------------------------------------------------------------- transfers

// Copy a rectangle between two VRAM/RAM surfaces (GE_TRANSFERSTART).
void gpu_block_transfer(void) {
    g_flush_reason[2]++;
    raster_flush();
    const uint32_t *r = g_ge.cmd;
    if (g_glr_active) {
        uint32_t s0 = (r[GE_TRANSFERSRC] & 0xFFFFF0u) | ((r[GE_TRANSFERSRCW] & 0xFF0000u) << 8);
        uint32_t d0 = (r[GE_TRANSFERDST] & 0xFFFFF0u) | ((r[GE_TRANSFERDSTW] & 0xFF0000u) << 8);
        glr_note_fb_access("block transfer (source)", s0, s0 + 1);
        glr_note_fb_access("block transfer (destination)", d0, d0 + 1);
    }
    uint32_t src = (r[GE_TRANSFERSRC] & 0xFFFFF0u) | ((r[GE_TRANSFERSRCW] & 0xFF0000u) << 8);
    uint32_t dst = (r[GE_TRANSFERDST] & 0xFFFFF0u) | ((r[GE_TRANSFERDSTW] & 0xFF0000u) << 8);
    uint32_t sstride = r[GE_TRANSFERSRCW] & 0x7F8u, dstride = r[GE_TRANSFERDSTW] & 0x7F8u;
    uint32_t sx = r[GE_TRANSFERSRCPOS] & 0x3FF, sy = (r[GE_TRANSFERSRCPOS] >> 10) & 0x3FF;
    uint32_t dx = r[GE_TRANSFERDSTPOS] & 0x3FF, dy = (r[GE_TRANSFERDSTPOS] >> 10) & 0x3FF;
    uint32_t w = (r[GE_TRANSFERSIZE] & 0x3FF) + 1, h = ((r[GE_TRANSFERSIZE] >> 10) & 0x3FF) + 1;
    uint32_t bpp = (r[GE_TRANSFERSTART] & 1) ? 4 : 2;
#ifdef HLE_TRACE
    {   /* Each distinct block transfer once, with the guest frame and whether
           either end is a display buffer: the GX path draws into its own
           frame buffer, so a copy into or out of a display buffer in guest
           VRAM does not reach the picture. */
        extern uint32_t g_frames_presented;
        static uint32_t seen[64][4], counts[64];
        static int nseen;
        uint32_t key[4] = {src, dst, w | h << 16, sx | sy << 10 | dx << 20};
        int i;
        for (i = 0; i < nseen; i++)
            if (!memcmp(seen[i], key, sizeof key)) break;
        if (i < nseen) {
            if (++counts[i] == 100 || counts[i] == 1000)
                fprintf(stderr, "XFER repeat #%d x%u guest=%u\n", i, counts[i], g_frames_presented);
        } else if (nseen < 64) {
            memcpy(seen[nseen], key, sizeof key);
            counts[nseen] = 1;
            fprintf(stderr, "XFER #%d guest=%u src=%08X(%u,%u) stride=%u disp=%d -> dst=%08X(%u,%u) stride=%u "
                    "disp=%d size=%ux%u bpp=%u\n", nseen, g_frames_presented, src, sx, sy, sstride,
                    gpu_is_display_target(src), dst, dx, dy, dstride, gpu_is_display_target(dst), w, h, bpp);
            nseen++;
        }
    }
#endif
    surf_resolve_range(src + (sy * sstride + sx) * bpp, src + ((sy + h) * sstride) * bpp);
    surf_resolve_range(dst + (dy * dstride + dx) * bpp, dst + ((dy + h) * dstride) * bpp);
    for (uint32_t y = 0; y < h; y++)
        memmove(host(dst + ((dy + y) * dstride + dx) * bpp), host(src + ((sy + y) * sstride + sx) * bpp),
                w * bpp);
    surf_transfer(src, sstride, sx, sy, dst, dstride, dx, dy, w, h, bpp);
    surf_vram_written(dst + (dy * dstride + dx) * bpp, dst + ((dy + h) * dstride) * bpp, g_transfer_target);
}

// LOADCLUT: copy n*32 bytes of palette into the GE's internal palette.
void gpu_load_clut(void) {
    uint32_t addr = (R(GE_CLUTADDR) & 0xFFFFF0u) | ((R(GE_CLUTADDRUPPER) << 8) & 0x0F000000u);
    uint32_t bytes = (R(GE_LOADCLUT) & 0x3F) * 32;
    if (bytes > sizeof g_ge.clut) bytes = sizeof g_ge.clut;
    memcpy(g_ge.clut, host(addr), bytes);
    texrep_clut_loaded(bytes);
    g_clut_dirty = 1;  // the palette changed, so the cached hash is stale
}

/* Expand the half-resolution buffer into the guest's framebuffer.

   The guest expects its framebuffer to hold the finished picture, so the
   small buffer is copied out at the end of every display list.  The
   pixels are already in the guest's own format and byte order, so this
   is a straight copy of each row twice. */
/* The framebuffer the guest has on display.  The game double buffers,
   so the buffer being drawn into is not always the one shown, and the
   guest may read either of them back. */
uint32_t g_display_fb_addr;

static void upsample_into(uint32_t fb, uint32_t stride) {
    uint32_t avail;
    uint8_t *dst = guest_window(fb, &avail);
    if (!dst) return;
    for (int y = 0; y < 272; y++) {
        const uint16_t *src = g_half_fb + (size_t)(y >> 1) * HALF_W;
        uint16_t *d = (uint16_t *)(dst + (size_t)y * stride * 2);
        for (int x = 0; x < 480; x++) d[x] = src[x >> 1];
    }
}

uint64_t g_half_upsample_us;  // time spent expanding into the framebuffer
uint64_t g_half_upsample_calls;

void gpu_half_res_upsample(void) {
    if (!gpu_half_res_active()) return;
    uint64_t t0 = hle_time_us();
    uint32_t fb = 0x04000000u | (R(GE_FRAMEBUFPTR) & 0xFFFFF0u);
    uint32_t stride = R(GE_FRAMEBUFWIDTH) & 0x7FC;
    int fmt = R(GE_FRAMEBUFPIXFORMAT) & 3;
    if (fmt == 3) return;   // a 32-bit framebuffer is not used by this game
    upsample_into(fb, stride);
    /* The buffer on display as well, so the game never reads back a
       stale picture. */
    if (g_display_fb_addr && g_display_fb_addr != fb)
        upsample_into(g_display_fb_addr, stride);
    g_half_upsample_us += hle_time_us() - t0;
    g_half_upsample_calls++;
}

#ifdef GPU_SELFTEST
/* ---------------------------------------------------------------- self-test

   Independent test vectors for the texture reader (docs/PLAN.md, section
   3.5 B).  Synthetic textures in every 16/32-bit and CLUT format are written
   to guest VRAM the way the PSP stores them (little endian) and read back
   through fetch() and the CLUT path, on the Wii's own big-endian build.  The
   expected texels are worked out by hand from the GE formats, not taken from
   the decoder.  Build with EXTRA_CFLAGS=-DGPU_SELFTEST; boot runs it once and
   reports to stderr.  DXT is left out: its channel order has no
   independently confirmed reference here. */

#define ST_TEX  0x04180000u
#define ST_CLUT 0x041F0000u

static int st_fail, st_count;

static void st_tex(int fmt, int log2w, int log2h, int stride, int swizzle) {
    g_ge.cmd[GE_TEXFORMAT] = (uint32_t)fmt;
    g_ge.cmd[GE_TEXMODE] = (uint32_t)swizzle;
    g_ge.cmd[GE_TEXSIZE0] = (uint32_t)log2w | (uint32_t)log2h << 8;
    g_ge.cmd[GE_TEXADDR0] = ST_TEX & 0xFFFFFFu;
    g_ge.cmd[GE_TEXBUFWIDTH0] = (uint32_t)stride | ((ST_TEX >> 24) & 0xF) << 16;
    g_ge.cmd[GE_TEXTUREMAPENABLE] = 1;
}

static void st_clut(int fmt, int shift, int mask, int offset16, int blocks) {
    g_ge.cmd[GE_CLUTADDR] = ST_CLUT & 0xFFFFFFu;
    g_ge.cmd[GE_CLUTADDRUPPER] = ((ST_CLUT >> 24) & 0xF) << 16;
    g_ge.cmd[GE_LOADCLUT] = (uint32_t)blocks;
    g_ge.cmd[GE_CLUTFORMAT] = (uint32_t)fmt | (uint32_t)shift << 2 | (uint32_t)mask << 8 |
                              (uint32_t)offset16 << 16;
    gpu_load_clut();
}

static void st_check(const char *what, int x, int y, uint32_t want) {
    TexState t;
    memset(&t, 0, sizeof t);
    tex_state(&t);
    uint32_t got = fetch(&t, x, y);
    st_count++;
    if (got != want) {
        st_fail++;
        fprintf(stderr, "SELFTEST FAIL %s (%d,%d): got %08X want %08X\n", what, x, y, got, want);
    }
}

int gpu_texture_selftest(void) {
    GeState saved = g_ge;
    st_fail = st_count = 0;

    /* 565: red in bits 0-4, green 5-10, blue 11-15; the channels are widened
       by repeating their top bits, so 0x10 (half red) reads as 0x84. */
    st_tex(0, 3, 3, 8, 0);
    static const uint16_t c565[5] = {0x001F, 0x07E0, 0xF800, 0x0000, 0x0010};
    static const uint32_t w565[5] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFF000000u, 0xFF000084u};
    for (int i = 0; i < 5; i++) mem_w16(ST_TEX + (uint32_t)i * 2, c565[i]);
    for (int i = 0; i < 5; i++) st_check("565", i, 0, w565[i]);

    /* 5551: the alpha bit is bit 15. */
    st_tex(1, 3, 3, 8, 0);
    static const uint16_t c5551[3] = {0x801F, 0x7C00, 0x03E0};
    static const uint32_t w5551[3] = {0xFF0000FFu, 0x00FF0000u, 0x0000FF00u};
    for (int i = 0; i < 3; i++) mem_w16(ST_TEX + (uint32_t)i * 2, c5551[i]);
    for (int i = 0; i < 3; i++) st_check("5551", i, 0, w5551[i]);

    /* 4444: red in the low nibble, alpha in the top one; x17 widening. */
    st_tex(2, 3, 3, 8, 0);
    static const uint16_t c4444[3] = {0xF00F, 0x0F00, 0x1234};
    static const uint32_t w4444[3] = {0xFF0000FFu, 0x00FF0000u, 0x11223344u};
    for (int i = 0; i < 3; i++) mem_w16(ST_TEX + (uint32_t)i * 2, c4444[i]);
    for (int i = 0; i < 3; i++) st_check("4444", i, 0, w4444[i]);

    /* 8888: the bytes in memory are R, G, B, A. */
    st_tex(3, 3, 3, 8, 0);
    mem_w8(ST_TEX + 0, 0x11); mem_w8(ST_TEX + 1, 0x22); mem_w8(ST_TEX + 2, 0x33); mem_w8(ST_TEX + 3, 0x44);
    st_check("8888", 0, 0, 0x44332211u);

    /* Swizzled 8888, 8 texels wide: 16-byte x 8-row blocks, two per row of
       blocks.  Texel (5, 1) is in block 1, row 1, byte 4 of the row. */
    st_tex(3, 3, 3, 8, 1);
    for (uint32_t i = 0; i < 256; i += 4) mem_w32(ST_TEX + i, 0);
    mem_w32(ST_TEX + 128 + 16 + 4, 0xA1B2C3D4u);
    st_check("8888 swizzled", 5, 1, 0xA1B2C3D4u);

    /* CLUT8 with an 8888 palette: entry k stored as bytes k, 255-k, k^0x5A, 0x80. */
    for (uint32_t k = 0; k < 256; k++)
        mem_w32(ST_CLUT + k * 4, k | (255u - k) << 8 | (k ^ 0x5Au) << 16 | 0x80u << 24);
    st_clut(3, 0, 0xFF, 0, 32);
    st_tex(5, 3, 3, 8, 0);
    static const uint8_t idx8[4] = {3, 7, 0xFE, 0x80};
    for (int i = 0; i < 4; i++) mem_w8(ST_TEX + (uint32_t)i, idx8[i]);
    for (int i = 0; i < 4; i++) {
        uint32_t k = idx8[i];
        st_check("clut8/8888", i, 0, k | (255u - k) << 8 | (k ^ 0x5Au) << 16 | 0x80u << 24);
    }

    /* CLUT4 with a 565 palette: the low nibble is the even texel. */
    for (uint32_t k = 0; k < 16; k++) mem_w16(ST_CLUT + k * 2, (uint16_t)(k == 1 ? 0x001F : k == 2 ? 0x07E0 : 0));
    st_clut(0, 0, 0xFF, 0, 1);
    st_tex(4, 3, 3, 8, 0);
    mem_w8(ST_TEX, 0x21);
    st_check("clut4/565 low", 0, 0, 0xFF0000FFu);
    st_check("clut4/565 high", 1, 0, 0xFF00FF00u);

    /* Index shift, mask and offset: ((0x35 >> 4) & 0xF) | 16 = entry 19. */
    for (uint32_t k = 0; k < 256; k++)
        mem_w32(ST_CLUT + k * 4, k | (255u - k) << 8 | (k ^ 0x5Au) << 16 | 0x80u << 24);
    st_clut(3, 4, 0x0F, 1, 32);
    st_tex(5, 3, 3, 8, 0);
    mem_w8(ST_TEX, 0x35);
    st_check("clut8 shift/mask/offset", 0, 0, 19u | 236u << 8 | (19u ^ 0x5Au) << 16 | 0x80u << 24);

    /* tex_decode_fast() against fetch(), the per-texel reader, on pseudo-random bytes: the formats it takes (565, 5551,
       4444, 8888, CLUT4, CLUT8), linear and swizzled, from one piece wide (1-4 texels) up to several blocks, with and
       without a stride wider than the texture, and heights under, at and over the 8 rows of a block.  The fights only
       ever decode paletted textures, so this is what covers the 16 and 32-bit loops. */
    {
        static const int fmts[6] = {0, 1, 2, 3, 4, 5};
        static const int bits[6] = {16, 16, 16, 32, 4, 8};
        uint32_t seed = 0x2545F491u;
        for (uint32_t i = 0; i < 0x4000; i += 4) { seed = seed * 1664525u + 1013904223u; mem_w32(ST_TEX + i, seed); }
        for (uint32_t k = 0; k < 256; k++) { seed = seed * 1664525u + 1013904223u; mem_w32(ST_CLUT + k * 4, seed); }
        st_clut(3, 0, 0xFF, 0, 32);
        int dec_n = 0, dec_texels = 0;
        for (int fi = 0; fi < 6; fi++)
            for (int sw = 0; sw < 2; sw++)
                for (int lw = 0; lw <= 6; lw++)
                    for (int lh = 0; lh <= 4; lh++)
                        for (int wide = 0; wide < 2; wide++) {
                            const int w = 1 << lw, h = 1 << lh;
                            const int unit = 128 / bits[fi];                    /* texels in 16 bytes */
                            const int stride = (w + unit - 1) / unit * unit + (wide ? unit : 0);
                            st_tex(fmts[fi], lw, lh, stride, sw);
                            TexState t;
                            memset(&t, 0, sizeof t);
                            tex_state(&t);
                            uint32_t *dst = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
                            if (!dst) continue;
                            memset(dst, 0xA5, (size_t)w * (size_t)h * 4);
                            int mism = 0;
                            if (!tex_decode_fast(&t, dst)) mism = w * h;
                            else
                                for (int y = 0; y < h; y++)
                                    for (int x = 0; x < w; x++) mism += dst[y * w + x] != fetch(&t, x, y);
                            free(dst);
                            dec_n++;
                            dec_texels += w * h;
                            st_count++;
                            if (mism) {
                                st_fail++;
                                fprintf(stderr, "SELFTEST FAIL decode fmt=%d %dx%d stride=%d swizzle=%d: %d texels differ\n", fmts[fi], w, h, stride, sw, mism);
                            }
                        }
        fprintf(stderr, "SELFTEST decode_fast: %d textures, %d texels checked\n", dec_n, dec_texels);
        {   /* the two-table expansion of a 16-bit texel against c16_to_32(), for every value of the three formats */
            if (!s_c16_tab_ready) c16_tab_init();
            int bad = 0;
            for (int f = 0; f < 3; f++)
                for (uint32_t v = 0; v < 65536; v++)
                    bad += (s_c16_tab[f][0][v & 255] | s_c16_tab[f][1][v >> 8]) != c16_to_32(v, f);
            st_count++;
            if (bad) {
                st_fail++;
                fprintf(stderr, "SELFTEST FAIL c16 tables: %d of 196608 values differ\n", bad);
            }
        }
        {   /* what vc_alpha_refresh() makes against what the lighting code makes: every material (or vertex) alpha with every
               global ambient alpha, through light_registers() (material alpha) and light_general() (vertex alpha), packed as
               gxr_pack() does; and colour_alpha() against rd_color_fast() for every value of the three 16-bit colour kinds */
            const uint32_t save_ma = R(GE_MATERIALALPHA), save_ga = R(GE_AMBIENTALPHA), save_mu = R(GE_MATERIALUPDATE);
            int bad = 0;
            const float wp0[3] = {0.f, 0.f, 0.f};
            for (uint32_t ma = 0; ma < 256; ma++)
                for (uint32_t ga = 0; ga < 256; ga++) {
                    const uint32_t want = (2u * ma * ga + 255u) / 510u;
                    float n[3] = {0.f, 0.f, 0.f}, col[4] = {0.f, 0.f, 0.f, (float)ma};
                    R(GE_MATERIALALPHA) = ma; R(GE_AMBIENTALPHA) = ga; R(GE_MATERIALUPDATE) = 0;
                    g_ge_light_gen++;
                    light_vertex(wp0, n, col, 0);
                    uint32_t got = col[3] <= 0.f ? 0u : col[3] >= 255.f ? 255u : (uint32_t)(uint8_t)(col[3] + 0.5f);
                    bad += got != want;
                    col[0] = col[1] = col[2] = 0.f; col[3] = (float)ma;
                    n[0] = n[1] = n[2] = 0.f;
                    R(GE_MATERIALALPHA) = 77; R(GE_MATERIALUPDATE) = 1;
                    g_ge_light_gen++;
                    light_vertex(wp0, n, col, 1);
                    got = col[3] <= 0.f ? 0u : col[3] >= 255.f ? 255u : (uint32_t)(uint8_t)(col[3] + 0.5f);
                    bad += got != want;
                }
            for (int fmt = 4; fmt <= 7; fmt++)
                for (uint32_t v = 0; v < (fmt == 7 ? 256u : 65536u); v++) {
                    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), 0, (uint8_t)v};
                    float c[4];
                    rd_color_fast(b, fmt, c);
                    bad += colour_alpha(b, fmt) != (uint32_t)(c[3] + 0.5f);
                }
            R(GE_MATERIALALPHA) = save_ma; R(GE_AMBIENTALPHA) = save_ga; R(GE_MATERIALUPDATE) = save_mu;
            g_ge_light_gen++;
            st_count++;
            if (bad) {
                st_fail++;
                fprintf(stderr, "SELFTEST FAIL alpha refresh: %d differences\n", bad);
            }
        }
#ifdef EXP_TEXDECBENCH   /* -DEXP_TEXDECBENCH: the time of tex_decode_fast() a texture; build once with -DEXP_OLDPALDECODE for the old code */
        static const struct { int fmt, lw, lh, sw; const char *what; } bc[] = {
            {4, 8, 8, 0, "CLUT4 256x256 linear"}, {4, 8, 8, 1, "CLUT4 256x256 swizzled"}, {5, 8, 8, 1, "CLUT8 256x256 swizzled"},
            {0, 9, 9, 0, "565 512x512 linear"}, {1, 9, 9, 1, "5551 512x512 swizzled"}, {3, 8, 8, 1, "8888 256x256 swizzled"},
            {4, 7, 7, 1, "CLUT4 128x128 swizzled"},
        };
        uint32_t *bdst = (uint32_t *)big_alloc(512u * 512u * 4u);
        for (int i = 0; bdst && i < (int)(sizeof bc / sizeof bc[0]); i++) {
            const int w = 1 << bc[i].lw;
            st_tex(bc[i].fmt, bc[i].lw, bc[i].lh, w, bc[i].sw);
            TexState t;
            memset(&t, 0, sizeof t);
            tex_state(&t);
            tex_decode_fast(&t, bdst);   /* warm up */
            const uint64_t t0 = hle_time_us();
            for (int it = 0; it < 4; it++) tex_decode_fast(&t, bdst);
            fprintf(stderr, "TEXDECBENCH %s: %u us\n", bc[i].what, (unsigned)((hle_time_us() - t0) / 4));
        }
#endif
    }

    for (uint32_t i = 0; i < 1024; i += 4) { mem_w32(ST_TEX + i, 0); mem_w32(ST_CLUT + i, 0); }
    g_ge = saved;
    g_ge_view_gen++;
    g_ge_light_gen++;
    g_clut_dirty = 1;
    fprintf(stderr, "SELFTEST textures: %d/%d passed\n", st_count - st_fail, st_count);
    return st_fail;
}
#endif
