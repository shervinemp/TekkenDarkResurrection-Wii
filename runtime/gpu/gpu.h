// PSP Graphics Engine (GE): display list processing and software rendering.
#pragma once
#include "../bigmem.h"
#include "../recomp.h"

/* The chip lights the fighters that the flattened lighting covers (diffuse lights only; see
   wii_gxscene_draw_lit()).  -DEXP_NOGXLIGHT lights them on the CPU as before. */
#if defined(__wii__) && !defined(EXP_NOGXLIGHT) && !defined(EXP_GXLIGHT)
#define EXP_GXLIGHT 1
#endif

typedef struct {
    uint32_t fb_addr, stride, format;  // what sceDisplay scans out
} DisplayState;
extern DisplayState g_display;

// Command numbers used by the processor and rasterizer
enum {
    GE_NOP = 0x00, GE_VADDR = 0x01, GE_IADDR = 0x02, GE_PRIM = 0x04, GE_BEZIER = 0x05,
    GE_SPLINE = 0x06, GE_BBOX = 0x07, GE_JUMP = 0x08, GE_BJUMP = 0x09, GE_CALL = 0x0A,
    GE_RET = 0x0B, GE_END = 0x0C, GE_SIGNAL = 0x0E, GE_FINISH = 0x0F, GE_BASE = 0x10,
    GE_VERTEXTYPE = 0x12, GE_OFFSETADDR = 0x13, GE_ORIGIN = 0x14,
    GE_LIGHTINGENABLE = 0x17, GE_LIGHTENABLE0 = 0x18, GE_CULLFACEENABLE = 0x1D,
    GE_TEXTUREMAPENABLE = 0x1E, GE_FOGENABLE = 0x1F, GE_DITHERENABLE = 0x20,
    GE_ALPHABLENDENABLE = 0x21, GE_ALPHATESTENABLE = 0x22, GE_ZTESTENABLE = 0x23,
    GE_STENCILTESTENABLE = 0x24, GE_COLORTESTENABLE = 0x27, GE_LOGICOPENABLE = 0x28,
    GE_BONEMATRIXNUMBER = 0x2A, GE_BONEMATRIXDATA = 0x2B, GE_MORPHWEIGHT0 = 0x2C,
    GE_WORLDMATRIXNUMBER = 0x3A, GE_WORLDMATRIXDATA = 0x3B, GE_VIEWMATRIXNUMBER = 0x3C,
    GE_VIEWMATRIXDATA = 0x3D, GE_PROJMATRIXNUMBER = 0x3E, GE_PROJMATRIXDATA = 0x3F,
    GE_TGENMATRIXNUMBER = 0x40, GE_TGENMATRIXDATA = 0x41,
    GE_VIEWPORTXSCALE = 0x42, GE_VIEWPORTYSCALE = 0x43, GE_VIEWPORTZSCALE = 0x44,
    GE_VIEWPORTXCENTER = 0x45, GE_VIEWPORTYCENTER = 0x46, GE_VIEWPORTZCENTER = 0x47,
    GE_TEXSCALEU = 0x48, GE_TEXSCALEV = 0x49, GE_TEXOFFSETU = 0x4A, GE_TEXOFFSETV = 0x4B,
    GE_OFFSETX = 0x4C, GE_OFFSETY = 0x4D, GE_SHADEMODE = 0x50, GE_REVERSENORMAL = 0x51,
    GE_MATERIALUPDATE = 0x53, GE_MATERIALEMISSIVE = 0x54, GE_MATERIALAMBIENT = 0x55,
    GE_MATERIALDIFFUSE = 0x56, GE_MATERIALSPECULAR = 0x57, GE_MATERIALALPHA = 0x58,
    GE_MATERIALSPECULARCOEF = 0x5B, GE_AMBIENTCOLOR = 0x5C, GE_AMBIENTALPHA = 0x5D,
    GE_LIGHTMODE = 0x5E, GE_LIGHTTYPE0 = 0x5F, GE_LX0 = 0x63, GE_LDX0 = 0x6F, GE_LKA0 = 0x7B,
    GE_LKS0 = 0x87, GE_LKO0 = 0x8B, GE_LAC0 = 0x8F, GE_CULL = 0x9B,
    GE_FRAMEBUFPTR = 0x9C, GE_FRAMEBUFWIDTH = 0x9D, GE_ZBUFPTR = 0x9E, GE_ZBUFWIDTH = 0x9F,
    GE_TEXADDR0 = 0xA0, GE_TEXBUFWIDTH0 = 0xA8, GE_CLUTADDR = 0xB0, GE_CLUTADDRUPPER = 0xB1,
    GE_TRANSFERSRC = 0xB2, GE_TRANSFERSRCW = 0xB3, GE_TRANSFERDST = 0xB4,
    GE_TRANSFERDSTW = 0xB5, GE_TEXSIZE0 = 0xB8, GE_TEXMAPMODE = 0xC0, GE_TEXSHADELS = 0xC1,
    GE_TEXMODE = 0xC2, GE_TEXFORMAT = 0xC3, GE_LOADCLUT = 0xC4, GE_CLUTFORMAT = 0xC5,
    GE_TEXFILTER = 0xC6, GE_TEXWRAP = 0xC7, GE_TEXLEVEL = 0xC8, GE_TEXFUNC = 0xC9,
    GE_TEXENVCOLOR = 0xCA, GE_TEXFLUSH = 0xCB, GE_TEXSYNC = 0xCC, GE_FOG1 = 0xCD,
    GE_FOG2 = 0xCE, GE_FOGCOLOR = 0xCF, GE_FRAMEBUFPIXFORMAT = 0xD2, GE_CLEARMODE = 0xD3,
    GE_SCISSOR1 = 0xD4, GE_SCISSOR2 = 0xD5, GE_MINZ = 0xD6, GE_MAXZ = 0xD7,
    GE_COLORTEST = 0xD8, GE_COLORREF = 0xD9, GE_COLORTESTMASK = 0xDA, GE_ALPHATEST = 0xDB,
    GE_STENCILTEST = 0xDC, GE_STENCILOP = 0xDD, GE_ZTEST = 0xDE, GE_BLENDMODE = 0xDF,
    GE_BLENDFIXEDA = 0xE0, GE_BLENDFIXEDB = 0xE1, GE_DITH0 = 0xE2, GE_LOGICOP = 0xE6,
    GE_ZWRITEDISABLE = 0xE7, GE_MASKRGB = 0xE8, GE_MASKALPHA = 0xE9,
    GE_TRANSFERSTART = 0xEA, GE_TRANSFERSRCPOS = 0xEB, GE_TRANSFERDSTPOS = 0xEC,
    GE_TRANSFERSIZE = 0xEE,
};

typedef struct {
    uint32_t cmd[256];  // last value (24 bits) written to every register
    float world[12], view[12], proj[16], tgen[12], bone[8][12];
    int world_n, view_n, proj_n, tgen_n, bone_n;
    uint32_t base, offset;  // address composition
    uint32_t vaddr, iaddr;
    uint32_t clut[256];  // loaded palette (as 32-bit entries, raw)
    float morph[8];
} GeState;
extern GeState g_ge;

void gpu_set_callbacks(uint32_t signal_fn, uint32_t signal_arg, uint32_t finish_fn,
                       uint32_t finish_arg);
int32_t gpu_enqueue(Ctx *c, uint32_t list, uint32_t stall, int cbid, uint32_t arg, int head);
int32_t gpu_update_stall(Ctx *c, int id, uint32_t stall);
int32_t gpu_sync(Ctx *c, int list_id, int mode);
int gpu_busy(int list_id);
int gpu_run_interrupts(Ctx *c);

// raster.c
void gpu_draw_prim(int prim, int count);
void gpu_block_transfer(void);

/* Half-resolution rendering: the scene is drawn into a 240x136 buffer and
   expanded into the guest's framebuffer at the end of each display list. */
void gpu_half_res_set(int on);
int gpu_half_res_active(void);
void gpu_half_res_upsample(void);
extern uint32_t g_display_fb_addr;  // the framebuffer the guest has on display

/* The GX scene renderer (platform/wii/wii_gxscene.c): the Wii's own
   graphics chip draws the guest's triangles.  The interface is plain so
   that the parts of the runtime that are not Wii specific can use it. */
extern int g_gxr_enabled;     // the GX draws the guest's triangles
extern uint64_t g_gxr_prims;  // the primitives the chip drew
extern uint32_t g_ge_vertex_gen; // changes when transform/lighting/fog state changes
extern uint32_t g_ge_light_gen;  // changes when material/light registers change
extern uint32_t g_ge_pix_gen;    // changes when anything the per-primitive pixel state uses may have
extern uint32_t g_ge_view_gen;   // changes when the view/projection matrices or the viewport registers do
extern uint32_t g_ge_world_gen;   // changes when the world matrix is written
extern uint32_t g_gx_epoch;       // moves with every change to the chip state a draw is set up with
extern uint32_t g_mv_version;     // moves when the rasteriser works out its model-view matrix again
int wii_gxscene_enabled(void);
void wii_gxscene_enable(int on);
void wii_gxscene_init(void);

/* How the chip draws a primitive: the pixel pipeline and the texture. */
typedef struct {
    int blend, bl_eq, bl_src, bl_dst;
    uint32_t fix_a, fix_b;                 // GE fixed blend colours
    int alpha_test, at_func, at_ref, at_mask;
    int z_test, z_func, z_write;
    int cull;                              // 0 off; 1 the GE rejects triangles with a positive screen area (y down), 2 a negative one
    int zbias;                             // EXP_ZBIAS: -1 / +1 moves the chip's depth range toward smaller / larger values, 0 leaves it
    int clear, clear_color, clear_alpha, clear_depth;   // the GE's clear mode
    int fog_enable;                        // vertex fog, disabled in through mode
    float fog_end, fog_slope;              // GE_FOG1 / GE_FOG2
    uint32_t fog_color;                    // r | g<<8 | b<<16
    int dbl_src_alpha;                     // blend factors 6/7: 2 * source alpha
    int logic, logic_op;                   // GE logic-op enable and operation
    int dither;                            // GE dither enable
    int rgb_write, alpha_write;            // exact GX channel-write enables
    int stencil_alpha;                     // >=0: write this constant alpha; -1: preserve
    int tex_on, tex_func, tex_rgba, tex_dbl;
    uint32_t tex_env;                      // r | g<<8 | b<<16
    int tex_clamp_u, tex_clamp_v, tex_linear;
    const uint32_t *tex_pix;               // decoded texels, r | g<<8 | b<<16 | a<<24
    int tex_pw, tex_ph;                    // the size of tex_pix
    int tex_w, tex_h;                      // the size the guest declared (texels, "through" mode)
    uint32_t tex_serial;                   // changes whenever tex_pix does
    int tex_native;                        // GX can use the raw CI4/CI8 source + TLUT
    int tex_fmt;                           // the GE texture format
    int tex_swizzle, tex_stride;
    uint32_t tex_addr;
    int tex_clut_fmt, tex_clut_shift, tex_clut_mask, tex_clut_off;
    int through;                           // texture coordinates are in texels
    uint32_t version;                      // non-zero: changes whenever any field above does
} GxrState;

int gpu_is_display_target(uint32_t fb);  // a double-buffered display surface, not a texture
int gpu_is_presented_buffer(uint32_t a); // an address the guest has presented (the chip never writes it)
void wii_gxscene_list_begin(void);   // start of a display list
void wii_gxscene_present(void);      // end of a display list
void wii_gxscene_dirty(void);        // a primitive wrote pixels
int wii_gxscene_frame_started(void); // the list has drawn into this frame already
/* Positions in world space, with the guest's view matrix (4x3), projection
   matrix (4x4) and viewport: x/y/z scale, x/y/z centre, x/y offset.  gen is
   g_ge_view_gen: the same value means the same matrices and viewport. */
void wii_gxscene_set_world(const float *view, const float *proj, const float *vp, uint32_t gen);
void wii_gxscene_set_screen(void);   // positions in the PSP's screen pixels
void wii_gxscene_set_scissor(int x0, int y0, int x1, int y1);
void wii_gxscene_set_state(const GxrState *s);
/* mode: 0 triangles, 1 triangle strip, 2 triangle fan; x y z u v r g b a each */
void wii_gxscene_draw(int mode, const float *v, int count);
/* A vertex as the chip is sent it: x y z u v as floats, the colour as the bytes r g b a (r in the top
   byte).  The draw cache (vcache.c) keeps draws in this form. */
typedef struct { float x, y, z, u, v; uint32_t rgba; } GxVtx;
/* mv: a 4x3 model-view matrix in the GE's layout when the vertices are in object space, else NULL. */
/* uvt: u scale, v scale, u offset, v offset to apply to the texture coordinates, or NULL. */
void wii_gxscene_draw_packed(int mode, const GxVtx *v, int count, const float *mv, const float *uvt);
#ifdef EXP_GXLIGHT
/* A skinned vertex for the chip to light: the world normal (16 bits, 14 fraction bits) takes the colour's
   place.  28 bytes, with two of padding that are not sent. */
typedef struct { float x, y, z; int16_t nx, ny, nz, pad; float u, v; } GxVtxL;
/* The lights of a run of lit draws: the directions towards up to three directional lights (world space), their
   colours and the ambient colour on the chip's 0..255 scale, and the alpha. */
typedef struct {
    int n; float dir[3][3]; uint8_t col[3][3], amb[3], alpha;
#ifdef EXP_LITTEST
    float colf[3][3], ambf[3];   /* the unrounded colours, for the chip self-test */
#endif
} GxLit;
/* gen identifies the lights (the same value means the same GxLit). */
void wii_gxscene_draw_lit(int mode, const GxVtxL *v, int count, uint32_t gen, const GxLit *lit);
int wii_gxscene_lit_begin(int mode, int count, uint32_t gen, const GxLit *lit);   /* the first half of that, with the GX_Begin: the count to write, 0 for none */
void wii_gxscene_lit_end(void);
/* A vertex of a draw whose vertices all follow one bone at full weight, in the model's own space for the chip to
   transform: the guest's 16-bit position (a fraction of 32768), the normal to be lit (16 bits, 14 fraction bits), the
   texture coordinate.  mv: the 4x3 matrix from the model to the view in the GE's layout; nm: the 3x3 from the model to
   the world (row vectors), which carries the normals into the space the lights are in. */
typedef struct { int16_t x, y, z, nx, ny, nz; float u, v; } GxVtxR;
void wii_gxscene_draw_rigid(int mode, const GxVtxR *v, int count, uint32_t gen, const GxLit *lit, const float *mv, const float *nm, uint32_t mver);
#endif
typedef struct { uint32_t vbase, key, hd, hw, hr, gax, ga, refresh; } VcTok;   /* key: the vertex type and count, see vcache.c; refresh: a hit whose alpha bytes are to be made again for ga */
typedef struct { uint32_t hw, hr, gax; } VcRegs;   /* what a draw's vertices depend on besides their bytes; gax: 0x100 | the global ambient alpha when the draw's alpha can be made again (hr has no alpha then), else 0 */
void vc_regs(VcRegs *r, int objspace, int normals, int refresh_ok);
const GxVtx *vc_lookup2(const uint8_t *vp, uint32_t vbase, int count, int size, uint32_t vt, const VcRegs *r, VcTok *tok);
const GxVtx *vc_lookup(const uint8_t *vp, uint32_t vbase, int count, int size, uint32_t vt, int normals, int objspace, int refresh_ok, VcTok *tok);
GxVtx *vc_store(const VcTok *tok);
void vc_prefetch(uint32_t vbase);   // a hint: the draw at this address is probably next
void vc_reset(void);
uint32_t ge_light_sig(void);   // a hash of the values of the lighting and material registers
uint32_t ge_light_sig_flat(void);   // the same for vertices without normals (lights' directions do not matter)
uint32_t ge_light_sig_flat_noalpha(void);   // that one without the global ambient alpha
/* The decoded textures and the chip's converted copies share one memory
   pool, so each side can make the other give memory back. */
void wii_gxscene_tex_forget(uint32_t serial);  // the decoded texture with this serial is gone
int wii_gxscene_tex_trim(void);                // release every converted texture; 1 if any memory came back
int gpu_tex_evict_lru(const void *keep);       // free the oldest decoded texture other than keep; 0 if none
int gpu_tex_drop_copy(const void *keep);       // free the oldest decoded copy the chip also holds; 0 if none
int wii_gxscene_tex_has(uint32_t serial);      // the chip still holds this texture's converted copy
extern uint64_t g_half_upsample_us, g_half_upsample_calls;
void gpu_load_clut(void);
void gpu_tex_invalidate(void);
void gpu_tex_invalidate_range(uint32_t lo, uint32_t hi);
extern volatile uint32_t g_gpu_write_tick;  // bumped when the guest writes memory behind the runtime's back
void raster_flush(void);  // wait for queued primitives  // texture memory may have changed
uint32_t gpu_stat_prims(void);
// internal resolution (1..4); applied at the end of the next display list
void gpu_set_render_scale(int n);
int gpu_render_scale(void);
void gpu_resolve_all(void);
// GPU renderer (glr.c): preference, and whether a GL context is running it
void gpu_set_gpu_renderer(int on);
int gpu_renderer_wanted(void);
void gpu_glr_available(int on);
int gpu_replay(const char *path, int iters, const char *out_ppm);  // see GE_CAPTURE
int gpu_scanout_scaled(uint32_t addr, uint32_t stride, uint32_t fmt, uint16_t *out, int max_scale);

// texreplace.c: PPSSPP-compatible texture packs (TEX_DIR) and dumping (TEX_DUMP)
uint32_t *texrep_lookup(uint32_t addr, int fmt, int w, int h, uint32_t texbufwidth, uint32_t texsize,
                        uint32_t clutfmt, const uint32_t *decoded, int *rw, int *rh);
void texrep_clut_loaded(uint32_t bytes);
extern volatile int g_texrep_epoch;   // texrep_epoch() without the call
int texrep_epoch(void);  // changes when the pack is reloaded or dumping toggled  // primitives drawn since the last call

// Address of a register-relative pointer: (BASE high byte << 24) | value
static inline uint32_t ge_addr(uint32_t hi_cmd_val, uint32_t lo24) {
    return ((hi_cmd_val & 0x0F0000u) << 8) | (lo24 & 0xFFFFFFu);
}
static inline float ge_float(uint32_t v24) {
    uint32_t u = v24 << 8;
    float f;
    memcpy(&f, &u, 4);
    return f;
}
