// H.264 decoding for movies, on top of a minimal FFmpeg (tools/build_ffmpeg.sh).
#include <libavcodec/avcodec.h>
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <stdio.h>
#include <stdlib.h>

#include "hle.h"

typedef struct {
    AVCodecContext *ctx;
    AVCodecParserContext *parser;
    AVPacket *pkt;
    AVFrame *frame;
    uint8_t *au;  // last access unit returned by avc_next_au
    int au_size, au_cap;
} Avc;

static Avc g_avc;

/* The movie player composes into its own buffer, not the chip's picture, so
   the frontend presents the decoder's latest picture while a movie is active.
   It is limited-range YUV422, the format the VI scans; this buffer lives in
   the MEM2 pool. */
static uint16_t *s_movie_pixels;   /* host build only; the Wii path writes the scaled XFB */
static volatile int s_movie_frame_ready;
static volatile unsigned s_movie_frame_seq;

#ifdef __wii__
uint16_t *wii_video_movie_pixels(void);
int wii_video_convert_movie(const uint8_t *y, int ys, const uint8_t *u, int us,
                            const uint8_t *v, int vs, int w, int h, int full);
#endif

uint16_t *hle_movie_pixels(void) {
#ifdef __wii__
    return wii_video_movie_pixels();
#else
    return s_movie_pixels;
#endif
}
int hle_movie_frame_ready(void) { return s_movie_frame_ready; }
unsigned hle_movie_frame_seq(void) { return s_movie_frame_seq; }

static void avc_init(void) {
#ifndef __wii__
    if (!s_movie_pixels) s_movie_pixels = big_alloc(480u * 272u * sizeof *s_movie_pixels);
#endif
    if (g_avc.ctx) return;
#ifdef EXP_AVCPROBE
    fprintf(stderr, "AVCPROBE init begin" "\n");
#endif
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    g_avc.ctx = avcodec_alloc_context3(codec);
    g_avc.ctx->thread_count = 1;
    /* A PSP stream (Main profile, one reference frame, no B-frames, POC type 2) has no VUI bitstream restriction, which makes
       the decoder assume the largest reordering delay and hold the first pictures back; the flag says to output at once. */
    g_avc.ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    /* The in-loop deblocking filter stays on: it is part of H.264, and the
       intro's fades and low-bitrate scenes show blocking without it.  Speed
       comes from the FFmpeg build (-O2), the direct YUV presentation and, if
       needed, decoder optimisation -- not from decoding a worse picture. */
    int orc = avcodec_open2(g_avc.ctx, codec, NULL);
#ifdef EXP_AVCPROBE
    fprintf(stderr, "AVCPROBE init: decoder %p, open %d, ctx %p\n", (const void *)codec, orc, (void *)g_avc.ctx);
#endif
    (void)orc;
    g_avc.parser = av_parser_init(AV_CODEC_ID_H264);
    g_avc.pkt = av_packet_alloc();
    g_avc.frame = av_frame_alloc();
}

void avc_reset(void) {
    avc_init();
    avcodec_flush_buffers(g_avc.ctx);
    av_parser_close(g_avc.parser);
    g_avc.parser = av_parser_init(AV_CODEC_ID_H264);
    g_avc.au_size = 0;
    s_movie_frame_ready = 0;
    s_movie_frame_seq = 0;
}

// Split the next access unit out of `data`; returns bytes consumed. *got is set
// when a complete AU is ready (it is kept for avc_decode).
int avc_next_au(const uint8_t *data, int len, int *got) {
    avc_init();
    uint8_t *out = NULL;
    int out_size = 0;
    int used = av_parser_parse2(g_avc.parser, g_avc.ctx, &out, &out_size, data, len, AV_NOPTS_VALUE,
                                AV_NOPTS_VALUE, 0);
    *got = 0;
#ifdef EXP_AVCPROBE
    { static unsigned n; if (++n <= 12 || n % 500 == 0) fprintf(stderr, "AVCPROBE next_au %u: len %d used %d out_size %d\n", n, len, used, out_size); }
#endif
    if (out_size > 0) {
        if (out_size + AV_INPUT_BUFFER_PADDING_SIZE > g_avc.au_cap) {
            g_avc.au_cap = out_size + AV_INPUT_BUFFER_PADDING_SIZE;
            g_avc.au = realloc(g_avc.au, (size_t)g_avc.au_cap);
        }
        memcpy(g_avc.au, out, (size_t)out_size);
        memset(g_avc.au + out_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        g_avc.au_size = out_size;
        *got = 1;
    }
    return used;
}

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

// Decode the pending AU and write the picture (if one comes out) to guest
// memory: `stride` in pixels, `mode` 0 565, 1 5551, 2 4444, 3 8888.
void texrep_note_video(uint32_t addr, uint32_t bytes);

/* YUV 4:2:0 to the game's pixel format, two pixels at a time from tables.  The PSP's movies are full range (the stream's
   VUI says video_full_range_flag 1, which FFmpeg reports as YUVJ420P): R = Y + 1.402 (V-128), G = Y - 0.344 (U-128) -
   0.714 (V-128), B = Y + 1.772 (U-128), in 8.8 fixed point; limited range (16..235) is the usual 298 / 409 / 100 / 208 / 516
   set.  Each sum is rounded (+128), shifted by 8 and clamped through one table (the shifted sum lies in -277 .. 534, the
   table starts at -512). */
static int s_tc[2][256], s_tre[2][256], s_tgu[2][256], s_tgv[2][256], s_tbu[2][256];
static uint8_t s_clip[1536];
/* For 8888 written straight to host memory: the clamped channel already shifted into its byte of the host-order word
   (the guest's memory is little-endian, the Wii's big-endian), so a pixel is three loads and three ors. */
#ifdef __wii__
#define CH_R 24
#define CH_G 16
#define CH_B 8
#define CH_A 0x000000FFu
#else
#define CH_R 0
#define CH_G 8
#define CH_B 16
#define CH_A 0xFF000000u
#endif
#define CLIP_N 832
#define CLIP_OFF 288
static uint32_t s_cr[CLIP_N], s_cg[CLIP_N], s_cb[CLIP_N];
static int s_tab_ready;

static void tab_init(void) {
    for (int i = 0; i < 256; i++) {
        s_tc[0][i] = (i - 16) * 298 + 128;  s_tc[1][i] = i * 256 + 128;
        s_tre[0][i] = 409 * (i - 128);      s_tre[1][i] = 359 * (i - 128);
        s_tgu[0][i] = -100 * (i - 128);     s_tgu[1][i] = -88 * (i - 128);
        s_tgv[0][i] = -208 * (i - 128);     s_tgv[1][i] = -183 * (i - 128);
        s_tbu[0][i] = 516 * (i - 128);      s_tbu[1][i] = 454 * (i - 128);
    }
    for (int i = 0; i < 1536; i++) s_clip[i] = clamp8(i - 512);
    for (int i = 0; i < CLIP_N; i++) {
        const uint32_t v = clamp8(i - CLIP_OFF);
        s_cr[i] = v << CH_R; s_cg[i] = v << CH_G; s_cb[i] = v << CH_B;
    }
    s_tab_ready = 1;
}

/* mode 3 into host memory (what the game asks for): `o` is the destination, `stride` in pixels, w even */
static void convert_8888(const AVFrame *f, uint32_t *o, int stride, int w, int h, int full) {
    if (!s_tab_ready) tab_init();
    const int *tc = s_tc[full], *tre = s_tre[full], *tgu = s_tgu[full], *tgv = s_tgv[full], *tbu = s_tbu[full];
    const uint32_t *cr = s_cr + CLIP_OFF, *cg = s_cg + CLIP_OFF, *cb = s_cb + CLIP_OFF;
    for (int y = 0; y < h; y++, o += stride) {
        const uint8_t *Y = f->data[0] + y * f->linesize[0];
        const uint8_t *U = f->data[1] + (y >> 1) * f->linesize[1];
        const uint8_t *V = f->data[2] + (y >> 1) * f->linesize[2];
        for (int x = 0; x < w; x += 2) {
            const int u = U[x >> 1], v = V[x >> 1];
            const int re = tre[v], ge = tgu[u] + tgv[v], be = tbu[u];
            int c = tc[Y[x]];
            o[x] = cr[(c + re) >> 8] | cg[(c + ge) >> 8] | cb[(c + be) >> 8] | CH_A;
            c = tc[Y[x + 1]];
            o[x + 1] = cr[(c + re) >> 8] | cg[(c + ge) >> 8] | cb[(c + be) >> 8] | CH_A;
        }
    }
}

/* The decoder's full-range YUV420 picture, written as the VI's limited-range
   YUV422.  The XFB is not RGB: GX_CopyDisp normally converts the rendered RGB
   frame to YUV422, and a CPU-written XFB has to use the same layout.  Two
   pixels share one U and one V, packed big-endian as (Y0<<8)|U, (Y1<<8)|V.

   Full range (YUVJ420P) -> limited range (BT.601 studio swing):
       Y' = 16 + Y * 219 / 255
       C' = 128 + (C - 128) * 224 / 255
   Chroma stays 4:2:0 horizontally and gains a row vertically (4:2:2); odd
   rows average the two chroma rows so the upsampling is not point-sampled. */
static uint8_t s_y16[256], s_c128[256];
static int s_lim_ready;

static void lim_init(void) {
    for (int i = 0; i < 256; i++) {
        s_y16[i] = (uint8_t)(16 + (i * 219 + 127) / 255);
        const int c = i - 128;
        const int v = 128 + (c * 224 + (c >= 0 ? 127 : -127)) / 255;
        s_c128[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    s_lim_ready = 1;
}

static void convert_yuv422(const AVFrame *f, uint16_t *o, int stride, int w, int h, int full) {
    if (!s_lim_ready) lim_init();
    const int chroma_h = (h + 1) >> 1;
    for (int y = 0; y < h; y++, o += stride) {
        const uint8_t *Y = f->data[0] + y * f->linesize[0];
        const uint8_t *U0 = f->data[1] + (y >> 1) * f->linesize[1];
        const uint8_t *V0 = f->data[2] + (y >> 1) * f->linesize[2];
        const uint8_t *U1 = U0, *V1 = V0;
        if ((y & 1) && (y >> 1) + 1 < chroma_h) {
            U1 = U0 + f->linesize[1];
            V1 = V0 + f->linesize[2];
        }
        for (int x = 0; x < w; x += 2) {
            int u = (U0[x >> 1] + U1[x >> 1] + 1) >> 1;
            int v = (V0[x >> 1] + V1[x >> 1] + 1) >> 1;
            int y0 = Y[x], y1 = Y[x + 1];
            if (full) {
                y0 = s_y16[y0]; y1 = s_y16[y1];
                u = s_c128[u]; v = s_c128[v];
            }
            o[x] = (uint16_t)((y0 << 8) | u);
            o[x + 1] = (uint16_t)((y1 << 8) | v);
        }
    }
}

#ifdef __wii__
#define LE32(v_) __builtin_bswap32(v_)
#define LE16(v_) __builtin_bswap16(v_)
#else
#define LE32(v_) (v_)
#define LE16(v_) (v_)
#endif

/* `base` is the destination as host memory, or NULL when the guest range is not one block of it (every pixel then goes
   through mem_w32 / mem_w16). */
static void convert_frame(const AVFrame *f, uint8_t *base, uint32_t dst, int stride, int mode, int w, int h, int full) {
    if (!s_tab_ready) tab_init();
    const int *tc = s_tc[full], *tre = s_tre[full], *tgu = s_tgu[full], *tgv = s_tgv[full], *tbu = s_tbu[full];
    for (int y = 0; y < h; y++) {
        const uint8_t *Y = f->data[0] + y * f->linesize[0];
        const uint8_t *U = f->data[1] + (y >> 1) * f->linesize[1];
        const uint8_t *V = f->data[2] + (y >> 1) * f->linesize[2];
        uint32_t *o32 = base ? (uint32_t *)(base + (size_t)y * (size_t)stride * 4u) : NULL;
        uint16_t *o16 = base ? (uint16_t *)(base + (size_t)y * (size_t)stride * 2u) : NULL;
        for (int x = 0; x < w; x += 2) {
            const int u = U[x >> 1], v = V[x >> 1];
            const int re = tre[v], ge = tgu[u] + tgv[v], be = tbu[u];
            for (int k = 0; k < 2 && x + k < w; k++) {
                const int c = tc[Y[x + k]];
                const unsigned r = s_clip[512 + ((c + re) >> 8)];
                const unsigned g = s_clip[512 + ((c + ge) >> 8)];
                const unsigned b = s_clip[512 + ((c + be) >> 8)];
                if (mode == 3) {
                    const uint32_t px = r | g << 8 | b << 16 | 0xFF000000u;
                    if (o32) o32[x + k] = LE32(px);
                    else mem_w32(dst + (uint32_t)(y * stride + x + k) * 4u, px);
                } else {
                    const uint16_t px = mode == 0 ? (uint16_t)(r >> 3 | (g >> 2) << 5 | (b >> 3) << 11)
                                      : mode == 1 ? (uint16_t)(r >> 3 | (g >> 3) << 5 | (b >> 3) << 10 | 0x8000)
                                                  : (uint16_t)(r >> 4 | (g >> 4) << 4 | (b >> 4) << 8 | 0xF000);
                    if (o16) o16[x + k] = LE16(px);
                    else mem_w16(dst + (uint32_t)(y * stride + x + k) * 2u, px);
                }
            }
        }
    }
}

#ifdef EXP_AVCPROBE
#include <stdio.h>
#ifdef __wii__
#include <ogc/lwp_watchdog.h>
static uint32_t probe_us(void) { return (uint32_t)ticks_to_microsecs(gettime()); }
#else
static uint32_t probe_us(void) { return 0; }
#endif
static uint32_t s_pn, s_pdec, s_pcvt, s_pmax;
#endif

int avc_decode(uint32_t dst, int stride, int mode) {
    if (!g_avc.au_size) return 0;
#ifdef EXP_AVCPROBE
    const uint32_t t0 = probe_us();
#endif
    g_avc.pkt->data = g_avc.au;
    g_avc.pkt->size = g_avc.au_size;
    g_avc.au_size = 0;
    int src = avcodec_send_packet(g_avc.ctx, g_avc.pkt);
    int rrc = src < 0 ? src : avcodec_receive_frame(g_avc.ctx, g_avc.frame);
#ifdef EXP_AVCPROBE
    { static unsigned n; if (++n <= 12 || n % 100 == 0) fprintf(stderr, "AVCPROBE decode call %u: send %d receive %d\n", n, src, rrc); }
#endif
    if (src < 0 || rrc < 0) return 0;
#ifdef EXP_AVCPROBE
    const uint32_t t1 = probe_us();
#endif
    AVFrame *f = g_avc.frame;
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) return 0;   /* the converter reads 4:2:0 planes */
    int w = f->width < stride ? f->width : stride, h = f->height > 272 ? 272 : f->height;
    const uint32_t bpp = mode == 3 ? 4u : 2u;
    uint8_t *base = guest_host_ptr(dst, (uint32_t)(h * stride) * bpp);
    const int full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
#ifdef __wii__
    /* Convert and scale in the decoder thread, straight into the display
       picture.  The frontend only copies rows and flips, so it no longer
       spends 1.9 ms a frame competing with the movie feeder. */
    if (!wii_video_convert_movie(f->data[0], f->linesize[0],
                                 f->data[1], f->linesize[1],
                                 f->data[2], f->linesize[2], w, h, full))
        return 0;
    __sync_synchronize();
    s_movie_frame_seq++;
    s_movie_frame_ready = 1;
#else
    if (s_movie_pixels) {
        /* The frontend presents this directly; the game's own copy path does
           not reach the GX picture.  The 8888 copy is not needed for that and
           would push the frame over the movie player's budget, so make only
           the one the screen uses. */
        convert_yuv422(f, s_movie_pixels, 480, w, h, full);
        __sync_synchronize();
        s_movie_frame_seq++;
        s_movie_frame_ready = 1;
    } else if (mode == 3 && base && !(w & 1) && !((uintptr_t)base & 3u)) {
        convert_8888(f, (uint32_t *)base, stride, w, h, full);
    } else {
        convert_frame(f, base, dst, stride, mode, w, h, full);
    }
    /* The direct movie path writes s_movie_pixels, not the guest buffer, so
       there is no guest texture range to invalidate. */
    if (!s_movie_pixels) texrep_note_video(dst, (uint32_t)(h * stride) * bpp);
#endif
#ifdef EXP_AVCPROBE
    {
        const uint32_t t2 = probe_us();
        s_pdec += t1 - t0; s_pcvt += t2 - t1;
        if (t2 - t0 > s_pmax) s_pmax = t2 - t0;
        if (++s_pn % 30 == 1 || s_pn <= 3)
            fprintf(stderr, "AVCPROBE ffmpeg frame %u: %ux%u fmt %d mode %d base %p, decode %u us, convert %u us (mean of %u frames: %u + %u us, max %u us)\n",
                    s_pn, (unsigned)f->width, (unsigned)f->height, f->format, mode, (void *)base, t1 - t0, t2 - t1, s_pn, s_pdec / s_pn, s_pcvt / s_pn, s_pmax);
    }
#endif
    return 1;
}

// A minimal FFmpeg build references this AV1 film-grain helper from the H.264
// SEI code without compiling it; nothing here ever allocates film grain data.
void ff_aom_uninit_film_grain_params(void *params);
void ff_aom_uninit_film_grain_params(void *params) { (void)params; }
