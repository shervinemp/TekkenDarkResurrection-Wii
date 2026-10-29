#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <gccore.h>
#include <ogc/cache.h>
#include <ogc/conf.h>
#include <ogc/console.h>
#include <ogc/gx.h>
#include <ogc/gu.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>

#include "wii_platform.h"

#define FIFO_SIZE (256u << 10)
/* The texture is uploaded every frame, so its size is the per-frame cost:
   only the picture itself has to be converted.  GX wants each dimension to
   be a multiple of 4, and 480x272 already is. */
#define TEX_W 480
#define TEX_H 272
#define PSP_W 480
#define PSP_H 272

/* Three display buffers.  The video interface scans the buffer it was
   told about out over a whole frame, and only switches to a new one at
   the next retrace, so a buffer that has just been replaced as the one on
   display is still being read for up to a frame.  With three, the chip
   always has one that is neither on display nor just taken off it. */
#define XFB_COUNT 3

static GXRModeObj *s_rmode;
static void *s_xfb[XFB_COUNT];
static void *s_con_buf;          // the boot console, kept off screen until a fatal error
static int s_xfb_index;          // the software path's rotation
static void *s_fifo;
static GXTexObj s_texobj;
static uint8_t *s_tex;
static int s_game_started;
static int s_ready;

/* The display buffers of the GX scene renderer (see wii_gxscene.c). */
typedef struct {
    u16 token;        // the draw sync token that follows the copy
    int pending;      // a finished or in-flight copy the frontend may still show
} XfbSlot;
static XfbSlot s_slot[XFB_COUNT];
static int s_shown = -1;         // the buffer on display
static int s_prev = -1;          // the one before it, until a retrace passes
static volatile u32 s_retrace;   // retraces since boot
static u32 s_flip_retrace;       // the retrace count when s_shown was set
static int s_last_acquired;
static mutex_t s_ring_lock = LWP_MUTEX_NULL;
static VIRetraceCallback s_prev_cb;

static void retrace_cb(u32 count) {
    s_retrace = count;
    if (s_prev_cb) s_prev_cb(count);
}

bool wii_video_init(void) {
    VIDEO_Init();
    s_rmode = VIDEO_GetPreferredMode(NULL);
    if (!s_rmode) return false;

    /* Keep a separate pair for the GX game; the boot console gets its own
       buffer so a late diagnostic remains readable if the game never starts. */
    for (int i = 0; i < XFB_COUNT; i++) {
        s_xfb[i] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(s_rmode));
        if (!s_xfb[i]) return false;
        VIDEO_ClearFrameBuffer(s_rmode, s_xfb[i], COLOR_BLACK);
    }

    /* The boot console used to own s_xfb[0], so every boot message was
       visible for a moment as a purple text screen before the game took
       over.  Give it a buffer that is never scanned out; wii_fatal can
       show that buffer if the boot actually fails. */
    s_con_buf = MEM_K0_TO_K1(SYS_AllocateFramebuffer(s_rmode));
    if (!s_con_buf) return false;
    VIDEO_ClearFrameBuffer(s_rmode, s_con_buf, COLOR_BLACK);

    VIDEO_Configure(s_rmode);
    VIDEO_SetNextFramebuffer(s_xfb[0]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (s_rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();

    console_init(s_con_buf, 20, 20, s_rmode->fbWidth, s_rmode->xfbHeight,
                 s_rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    LWP_MutexInit(&s_ring_lock, false);
    s_prev_cb = VIDEO_SetPostRetraceCallback(retrace_cb);
    s_ready = 1;
    return true;
}

int wii_video_width(void) { return PSP_W; }
int wii_video_height(void) { return PSP_H; }

void wii_video_game_begin(void) {
    if (s_game_started) return;
    s_game_started = 1;

    /* The boot console draws into s_xfb[0]; the game owns the display
       buffers from here on, so erase the boot text before the first frame. */
    VIDEO_ClearFrameBuffer(s_rmode, s_xfb[0], COLOR_BLACK);

    s_fifo = memalign(32, FIFO_SIZE);
    if (!s_fifo) {
        wii_fatal("WII video: no memory for the GX FIFO\n");
        return;
    }
    memset(s_fifo, 0, FIFO_SIZE);
    GX_Init(s_fifo, FIFO_SIZE);

    s_tex = memalign(32, TEX_W * TEX_H * 2);
    if (!s_tex) {
        wii_fatal("WII video: no memory for the scanout texture\n");
        return;
    }
    memset(s_tex, 0, TEX_W * TEX_H * 2);

    /* RGBA6, not RGB8: the GE framebuffer has an alpha channel, and the
       game's card/glow effects use it as a destination blend factor.  The
       GX rejects DSTALPHA/INVDSTALPHA on an RGB8 EFB. */
    GX_SetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
    GX_SetCopyClear((GXColor){0, 0, 0, 255}, 0x00FFFFFF);
    GX_SetDispCopySrc(0, 0, s_rmode->fbWidth, s_rmode->efbHeight);
    GX_SetDispCopyDst(s_rmode->fbWidth, s_rmode->efbHeight);
    GX_SetDispCopyGamma(GX_GM_1_0);

    GX_SetNumChans(0);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GX_SetNumTevStages(1);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    GX_SetCullMode(GX_CULL_NONE);
    GX_SetZMode(GX_FALSE, GX_LEQUAL, GX_FALSE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);

    Mtx44 proj;
    guOrtho(proj, 0.0f, (f32)s_rmode->efbHeight, 0.0f, (f32)s_rmode->fbWidth,
            0.0f, 1.0f);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    GX_SetViewport(0, 0, (f32)s_rmode->fbWidth, (f32)s_rmode->efbHeight, 0, 1);
    GX_SetScissor(0, 0, s_rmode->fbWidth, s_rmode->efbHeight);
    GX_SetScissorBoxOffset(0, 0);

    GX_InvalidateTexAll();
}

/* Diagnostics: where the per-frame present cost goes. */
uint64_t g_vid_conv_us, g_vid_store_us, g_vid_inval_us, g_vid_copy_us;

/* Write the display buffer into the GX texture.

   The source is a 16-bit RGB565 image in host order and the GX texture
   holds the same 16-bit pixels in 4x4 tiles, so a tile is four rows of
   four pixels and the bytes are the same: this is a copy, not a
   conversion.  It used to write two bytes at a time and test the image
   bounds for every pixel, which cost 25 ns a pixel -- 8.4 s of a 120 s
   run.  Eight bytes at a time, with the edge tiles handled separately,
   is far cheaper. */
void wii_video_write_texture(const uint16_t *src, int w, int h) {
    uint64_t t0 = wii_time_us();
    if (w > TEX_W) w = TEX_W;
    if (h > TEX_H) h = TEX_H;
    uint8_t *dst = s_tex;
    /* A row of four pixels is eight bytes, and it is eight-byte aligned
       only when the row length is a multiple of four. */
    int aligned = (w & 3) == 0;
    for (int ty = 0; ty < TEX_H; ty += 4) {
        int rows = h - ty;
        if (rows > 4) rows = 4;
        for (int tx = 0; tx < TEX_W; tx += 4) {
            int cols = w - tx;
            if (cols > 4) cols = 4;
            for (int r = 0; r < 4; r++) {
                if (r >= rows) {
                    memset(dst, 0, 8);
                } else if (cols == 4 && aligned) {
                    uint64_t v;
                    memcpy(&v, src + (size_t)(ty + r) * w + tx, 8);
                    memcpy(dst, &v, 8);
                } else {
                    memcpy(dst, src + (size_t)(ty + r) * w + tx, (size_t)cols * 2);
                    memset(dst + cols * 2, 0, (size_t)(4 - cols) * 2);
                }
                dst += 8;
            }
        }
    }
    uint64_t t1 = wii_time_us();
    DCStoreRange(s_tex, TEX_W * TEX_H * 2);
    uint64_t t2 = wii_time_us();
    GX_InvalidateTexAll();
    uint64_t t3 = wii_time_us();
    g_vid_conv_us += t1 - t0;
    g_vid_store_us += t2 - t1;
    g_vid_inval_us += t3 - t2;
}

void wii_video_present(const uint16_t *argb, int w, int h) {
    if (!s_game_started || !s_tex) return;
    wii_video_write_texture(argb, w, h);

    GX_InitTexObj(&s_texobj, s_tex, TEX_W, TEX_H, GX_TF_RGB565,
                  GX_CLAMP, GX_CLAMP, GX_FALSE);
    /* The picture is 480x272 and the display is 640x362, so it is always
       magnified: sample it smoothly instead of with the default nearest,
       which is what made it look blocky. */
    GX_InitTexObjLOD(&s_texobj, GX_LINEAR, GX_LINEAR,
                     0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GX_LoadTexObj(&s_texobj, GX_TEXMAP0);

    /* Fit 480x272 (16:9) into the 4:3 framebuffer with letterboxing. */
    float vw = (float)s_rmode->fbWidth;
    float vh = (float)s_rmode->efbHeight;
    float dw = vw;
    float dh = dw * (float)PSP_H / (float)PSP_W;
    if (dh > vh) {
        dh = vh;
        dw = dh * (float)PSP_W / (float)PSP_H;
    }
    float x0 = (vw - dw) * 0.5f;
    float y0 = (vh - dh) * 0.5f;
    float x1 = x0 + dw;
    float y1 = y0 + dh;
    float u1 = (float)PSP_W / (float)TEX_W;
    float v1 = (float)PSP_H / (float)TEX_H;

    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position2f32(x0, y0);
    GX_TexCoord2f32(0.0f, 0.0f);
    GX_Position2f32(x1, y0);
    GX_TexCoord2f32(u1, 0.0f);
    GX_Position2f32(x1, y1);
    GX_TexCoord2f32(u1, v1);
    GX_Position2f32(x0, y1);
    GX_TexCoord2f32(0.0f, v1);
    GX_End();

    GX_DrawDone();
    uint64_t tc = wii_time_us();
    GX_CopyDisp(s_xfb[s_xfb_index], GX_TRUE);
    g_vid_copy_us += wii_time_us() - tc;
    VIDEO_SetNextFramebuffer(s_xfb[s_xfb_index]);
    VIDEO_Flush();
    s_xfb_index = (s_xfb_index + 1) % XFB_COUNT;
    /* The frontend paces at 60 Hz.  Do not block here: VIDEO_WaitVSync does
       not yield to the guest host threads. */
}

/* ---------------------------------------------- the GX scene renderer

   The GE thread copies each finished display list from the chip's frame
   buffer into one of the display buffers and follows the copy with a token
   in the command stream.  The frontend shows the newest copy whose token
   the chip has reached.  Nothing here waits for the chip. */

static void ring_lock(void) { if (s_ring_lock != LWP_MUTEX_NULL) LWP_MutexLock(s_ring_lock); }
static void ring_unlock(void) { if (s_ring_lock != LWP_MUTEX_NULL) LWP_MutexUnlock(s_ring_lock); }

/* The buffer for the next copy: never the one on display, and never the one
   that was on display until the retrace that replaces it has passed. */
void wii_video_show_console(void) {
    if (!s_ready || !s_con_buf) return;
    DCStoreRange(s_con_buf, (size_t)s_rmode->fbWidth * s_rmode->xfbHeight *
                             VI_DISPLAY_PIX_SZ);
    VIDEO_SetNextFramebuffer(s_con_buf);
    VIDEO_Flush();
}

void *wii_video_xfb_acquire(void) {
    if (!s_game_started) return NULL;
    ring_lock();
    if (s_prev >= 0 && s_retrace != s_flip_retrace) s_prev = -1;
    int pick = -1;
    for (int n = 1; n <= XFB_COUNT; n++) {
        int i = (s_last_acquired + n) % XFB_COUNT;
        if (i != s_shown && i != s_prev) { pick = i; break; }
    }
    if (pick < 0) pick = (s_last_acquired + 1) % XFB_COUNT;   // cannot happen with three
    s_last_acquired = pick;
    s_slot[pick].pending = 0;   // whatever it held is overwritten
    ring_unlock();
    return s_xfb[pick];
}

/* Present a 480x272 RGB565 guest frame without the GX.  This is the
   fallback for the parts the GE does not draw: the PSMF movie frames and the
   menus, which the game writes into its own framebuffer.  It only touches a
   display buffer the chip ring is not using, and it issues no GX commands,
   so it is safe while the GE thread is idle (or still drawing). */
void wii_video_present_cpu(const uint16_t *argb, int w, int h) {
    if (!s_game_started || !argb || w <= 0 || h <= 0) return;
    void *xfb = wii_video_xfb_acquire();
    if (!xfb) return;

    int vw = s_rmode->fbWidth, vh = s_rmode->efbHeight;
    int dw = vw, dh = dw * h / w;
    if (dh > vh) { dh = vh; dw = dh * w / h; }
    int x0 = (vw - dw) / 2, y0 = (vh - dh) / 2;
    uint16_t *dst = (uint16_t *)xfb;

    /* The VI scans xfbHeight lines; the letterbox and the extra lines are
       black.  Clearing the whole buffer is cheap (about 0.6 MiB). */
    for (int y = 0; y < s_rmode->xfbHeight; y++)
        memset(dst + (size_t)y * vw, 0, (size_t)vw * 2);

    static int xmap[1280];
    static int map_w;
    if (map_w != dw) {
        for (int x = 0; x < dw; x++) xmap[x] = x * w / dw;
        map_w = dw;
    }
    for (int y = 0; y < dh; y++) {
        int sy = y * h / dh;
        const uint16_t *srow = argb + (size_t)sy * w;
        uint16_t *drow = dst + (size_t)(y0 + y) * vw + x0;
        for (int x = 0; x < dw; x++) drow[x] = srow[xmap[x]];
    }

    DCStoreRange(xfb, (size_t)vw * s_rmode->xfbHeight * 2);
    ring_lock();
    for (int i = 0; i < XFB_COUNT; i++) {
        if (s_xfb[i] != xfb) continue;
        s_prev = s_shown;
        s_shown = i;
        s_flip_retrace = s_retrace;
        break;
    }
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_Flush();
    ring_unlock();
}

/* Nearest-neighbour indices for the movie upscale.  The movie is 480x272
   and the game's own picture is magnified to 640x362 (4/3); a bilinear
   scaler here cost about 32 ms a frame and starved the movie decoder, so
   the movie uses the cheap resample into the same rectangle. */
#define SCALE_MAX 1280
static uint16_t s_xi[SCALE_MAX], s_yi[SCALE_MAX], s_ci[SCALE_MAX / 2];
static int s_scale_ready, s_scale_sw, s_scale_sh, s_scale_dw, s_scale_dh;

static void scale_make_index(uint16_t *t, int n, int src, int dst) {
    for (int i = 0; i < n; i++) {
        int s = ((2 * i + 1) * src) / (2 * dst);   // nearest pixel centre
        if (s >= src) s = src - 1;
        t[i] = (uint16_t)s;
    }
}

/* The movie is now converted and scaled in one pass on the decoder thread,
   into this 640x362 YUV422 buffer, instead of converting into a 480x272
   intermediate and scaling it on the frontend.  The old split cost 2.44 ms
   conversion + 1.9 ms scale; doing it in one pass also keeps the frontend
   free to flip the frame and let the ringbuffer feeder run. */
#define MOVIE_SCALED_W 640
#define MOVIE_SCALED_H 362
static uint16_t *s_movie_scaled;
static uint8_t s_movie_c[256], s_movie_c_lim[256];
static uint16_t s_movie_yw[256], s_movie_yw_lim[256];
static int s_movie_tab_ready;

static void movie_tab_init(void) {
    for (int i = 0; i < 256; i++) {
        int y = 16 + (i * 219 + 127) / 255;
        int c = i - 128;
        int cv = 128 + (c * 224 + (c >= 0 ? 127 : -127)) / 255;
        if (cv < 0) cv = 0;
        if (cv > 255) cv = 255;
        s_movie_yw[i] = (uint16_t)(y << 8);
        s_movie_yw_lim[i] = (uint16_t)(i << 8);
        s_movie_c[i] = (uint8_t)cv;
        s_movie_c_lim[i] = (uint8_t)i;
    }
    s_movie_tab_ready = 1;
}

/* The 640x362 rectangle the GX path uses for 480x272 content.  The same
   index tables drive the fused converter; the frontend only copies rows. */
static int movie_scaled_setup(int w, int h, int *dw_out, int *dh_out) {
    if (!s_rmode || w <= 0 || h <= 0) return 0;
    int vw = s_rmode->fbWidth, vh = s_rmode->efbHeight;
    int dw = vw, dh = dw * h / w;
    if (dh > vh) { dh = vh; dw = dh * w / h; }
    if (dw > MOVIE_SCALED_W) dw = MOVIE_SCALED_W;
    dw &= ~1;
    if (dh > MOVIE_SCALED_H) dh = MOVIE_SCALED_H;
    if (dh > s_rmode->xfbHeight) dh = s_rmode->xfbHeight;
    if (dw < 2 || dh < 1) return 0;
    if (!s_scale_ready || s_scale_sw != w || s_scale_sh != h ||
        s_scale_dw != dw || s_scale_dh != dh) {
        scale_make_index(s_xi, dw, w, dw);
        scale_make_index(s_yi, dh, h, dh);
        scale_make_index(s_ci, dw / 2, w / 2, dw / 2);
        s_scale_sw = w; s_scale_sh = h; s_scale_dw = dw; s_scale_dh = dh;
        s_scale_ready = 1;
    }
    *dw_out = dw;
    *dh_out = dh;
    return 1;
}

uint16_t *wii_video_movie_pixels(void) { return s_movie_scaled; }

/* Convert the decoder's full-range YUV420 picture straight to the 640x362
   limited-range YUV422 the VI scans.  This runs on the decoder thread; the
   frontend only copies the finished rows into the display buffer. */
int wii_video_convert_movie(const uint8_t *Y, int ys, const uint8_t *U, int us,
                            const uint8_t *V, int vs, int w, int h, int full) {
    if (!s_game_started || !Y || !U || !V || w <= 0 || h <= 0) return 0;
    int dw, dh;
    if (!movie_scaled_setup(w, h, &dw, &dh)) return 0;
    if (!s_movie_scaled) {
        s_movie_scaled = (uint16_t *)wii_big_alloc((size_t)MOVIE_SCALED_W *
                                                   MOVIE_SCALED_H * sizeof *s_movie_scaled);
        if (!s_movie_scaled) return 0;
    }
    if (!s_movie_tab_ready) movie_tab_init();

    const int chroma_h = (h + 1) >> 1;
    const uint16_t *yw = full ? s_movie_yw : s_movie_yw_lim;
    const uint8_t *ct = full ? s_movie_c : s_movie_c_lim;
    for (int y = 0; y < dh; y++) {
        const int sy = s_yi[y];
        const uint8_t *yr = Y + (size_t)sy * ys;
        const int cy = sy >> 1;
        const uint8_t *u0 = U + (size_t)cy * us, *v0 = V + (size_t)cy * vs;
        const uint8_t *u1 = u0, *v1 = v0;
        if ((sy & 1) && cy + 1 < chroma_h) { u1 = u0 + us; v1 = v0 + vs; }
        uint16_t *d = s_movie_scaled + (size_t)y * MOVIE_SCALED_W;
        if (sy & 1) {
            for (int p = 0; p < dw / 2; p++) {
                const int j = s_ci[p], x0 = s_xi[2 * p], x1 = s_xi[2 * p + 1];
                const int u = ct[(u0[j] + u1[j] + 1) >> 1];
                const int v = ct[(v0[j] + v1[j] + 1) >> 1];
                d[2 * p] = (uint16_t)(yw[yr[x0]] | u);
                d[2 * p + 1] = (uint16_t)(yw[yr[x1]] | v);
            }
        } else {
            for (int p = 0; p < dw / 2; p++) {
                const int j = s_ci[p], x0 = s_xi[2 * p], x1 = s_xi[2 * p + 1];
                const int u = ct[u0[j]], v = ct[v0[j]];
                d[2 * p] = (uint16_t)(yw[yr[x0]] | u);
                d[2 * p + 1] = (uint16_t)(yw[yr[x1]] | v);
            }
        }
    }
    __sync_synchronize();
    return 1;
}

/* Present the movie picture.  The decoder thread has already converted and
   scaled it into the same 640x362 rectangle the GX path uses for 480x272
   content, so the movie fills the screen like the rest of the game instead of
   sitting 1:1 in the centre.  This copies the rows into the VI's YUV422
   buffer; the border is limited-range black (Y=16, U=V=128), not zero, which
   would decode as green. */
void wii_video_present_movie(const uint16_t *yuv422, int w, int h) {
    if (!s_game_started || !yuv422 || w <= 0 || h <= 0 || !s_con_buf) return;

    const int vw = s_rmode->fbWidth;
    const int vh = s_rmode->efbHeight;      // layout area, as in wii_video_present
    const int scan_h = s_rmode->xfbHeight;  // rows the VI scans
    uint16_t *dst = (uint16_t *)MEM_K1_TO_K0(s_con_buf);
    static unsigned movie_frames;
    if ((movie_frames++ & 63) == 0) {
        /* The console buffer is free while the game runs.  Clear it to
           limited-range black once per movie frame batch; the centre is
           overwritten on every frame, the border is constant. */
        for (size_t i = 0; i < (size_t)vw * scan_h; i++) dst[i] = 0x1080;
        __sync_synchronize();
        DCStoreRange(dst, (size_t)vw * scan_h * 2);
    }

    int dw = vw, dh = dw * h / w;
    if (dh > vh) { dh = vh; dw = dh * w / h; }
    if (dw > SCALE_MAX) dw = SCALE_MAX;
    dw &= ~1;  // whole Y/U and Y/V pairs
    if (dh > scan_h) dh = scan_h;
    int x0 = (vw - dw) / 2, y0 = (vh - dh) / 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    x0 &= ~1;  // keep each Y/U and Y/V pair aligned
    if (y0 + dh > scan_h) dh = scan_h - y0;

    /* The decoder thread has already converted and scaled this into the
       640x362 YUV422 picture.  Copy the rows into the display buffer; do no
       per-pixel work here, so the frontend does not compete with the movie
       feeder for the CPU. */
    for (int y = 0; y < dh; y++)
        memcpy(dst + (size_t)(y0 + y) * vw + x0, yuv422 + (size_t)y * w,
               (size_t)dw * sizeof *yuv422);

    __sync_synchronize();
    DCStoreRange(dst + (size_t)y0 * vw, (size_t)dh * vw * 2);

    VIDEO_SetNextFramebuffer(s_con_buf);
    VIDEO_Flush();
}

void wii_video_xfb_submit(void *xfb, uint16_t token) {
    ring_lock();
    for (int i = 0; i < XFB_COUNT; i++) {
        if (s_xfb[i] != xfb) continue;
        s_slot[i].token = token;
        s_slot[i].pending = 1;
    }
    ring_unlock();
}

/* The frontend: put the newest finished copy on the screen. */
void wii_video_flip(void) {
    if (!s_game_started) return;
    u16 done = GX_GetDrawSync();
    ring_lock();
    int best = -1;
    for (int i = 0; i < XFB_COUNT; i++) {
        if (!s_slot[i].pending || (s16)(done - s_slot[i].token) < 0) continue;
        if (best < 0 || (s16)(s_slot[i].token - s_slot[best].token) > 0) best = i;
    }
    if (best >= 0) {
        for (int i = 0; i < XFB_COUNT; i++)   // older copies are not wanted any more
            if (i != best && s_slot[i].pending && (s16)(s_slot[best].token - s_slot[i].token) > 0)
                s_slot[i].pending = 0;
        s_slot[best].pending = 0;
        s_prev = s_shown;
        s_shown = best;
        s_flip_retrace = s_retrace;
        VIDEO_SetNextFramebuffer(s_xfb[best]);
        VIDEO_Flush();
    }
    ring_unlock();
}

void wii_video_shutdown(void) {
    /* Nothing to release: the framebuffers and FIFO live for the process. */
}
