#ifndef SKIN_PS_H
#define SKIN_PS_H

#include <stdint.h>

#ifdef __wii__

/* What the paired-single skinning kernel (skin_ps.c) needs for a run of
   skinned draws that share their register state.  The assembly reads the
   fields by offset, so the layout is part of the interface: every offset is
   checked in skin_ps.c. */
typedef struct SkinPS {
    const float *bone;        /*   0: g_ge.bone, 8 matrices of 12 floats (4 columns of xyz) */
    const float *world;       /*   4: g_ge.world */
    uint32_t nw, size;        /*   8: weights per vertex, vertex stride */
    uint32_t off_pos, off_wt; /*  16 */
    uint32_t off_nrm, off_tc; /*  24 */
    uint32_t gqr1, gqr6, gqr7, pad0;   /* 32: quantisation registers the kernel loads */
    float suv[2], ouv[2];     /*  48: texture scale and offset */
    float wh[2];              /*  64: (1, 0) */
    float lo, hi;             /*  72: the normals that need no normalising lie between */
    float c01[2], c2a[2];     /*  80: (base r, g) and (base b, alpha), each plus a half for rounding */
    float lp[3][4];           /*  96: the light directions, (x, y, z, 0) */
    float dp[3][4];           /* 144: the diffuse colours, (r, g, b, 0) */
    float half, zero;         /* 192 */
    float three_half, pad1;   /* 200 */
    float zeros[2];           /* 208 */
    float hp[3][4];           /* 216: the half vectors of the specular lights, (x, y, z, 0) */
    float sc[3][4];           /* 264: their colours, (r, g, b, 0) */
    uint32_t spec_mask;       /* 312: a bit per light with a specular term */
    uint32_t iexp;            /* 316: the specular exponent, a whole number */
    uint32_t rgba_const;      /* 320: the colour of every vertex when there is no lighting */
    uint32_t pad2[3];
} __attribute__((aligned(32))) SkinPS;

/* The fighters' skinned vertices (8-bit weights and normals, 16-bit
   positions, no vertex colour) straight into the vertices the chip is sent,
   with the texture coordinates absent, 8-bit or float.  A variant for each
   way of colouring: one colour for all (c), one to three directional lights
   (d), and lights with a specular term (s).  The fourth lights nothing: its
   vertices (GxVtxL, 28 bytes) carry the normalised world normal instead of
   the colour, for the chip to light (h). */
typedef void (*SkinPSFn)(const SkinPS *k, const uint8_t *vb, void *out, int count);
extern const SkinPSFn g_skin_ps_fn[4][3];   /* [c, d, s, h][no, 8-bit, float texture coordinates] */
/* The h kernels writing to the GX write-gather pipe (a vertex is 26 bytes of the command stream: position, normal, texture
   coordinate): `out` is the pipe's address, and the caller has begun a primitive of `count` vertices.  g_skin_ps_dense
   (-DEXP_PIPECHK) writes the same bytes to a buffer, for comparing. */
extern const SkinPSFn g_skin_ps_pipe[3];
#ifdef EXP_PIPECHK
extern const SkinPSFn g_skin_ps_dense[3];
#endif

#endif
#endif
