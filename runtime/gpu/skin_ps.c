/* The skinned fighter vertices on Broadway's paired singles.

   A skinned vertex is a 3x4 matrix times a point for each bone with a weight
   (and a 3x3 one for its normal), the weighted sum of those, the world matrix
   over that, a light and a texture coordinate.  In scalar code that is a dozen
   loads and a dozen multiply-adds per bone and again for the world matrix.
   The paired-single unit does two at a time and loads two floats in one go,
   and its quantised loads and stores read the 8-bit weights and normals, the
   16-bit positions and the 8-bit texture coordinates as they come and write
   the colour as four saturated bytes, which is most of the conversion code.

   The GCC in devkitPPC has no paired-single support, so this is assembly.  It
   uses f0-f13 and r0, r3-r12 only, all volatile, so it needs no saves; the
   quantisation registers 1, 6 and 7 are set on entry (libogc uses 2-5 and 0).

   Register map (floats are pairs):
     f0 matrix column / temp     f1 position xy      f2 normal xy       f3 scalar temp
     f4 (x, nx)  f5 (y, ny)  f6 (z, nz)   the point and the normal, as the three
                                          "rows" the transform runs over
     f7 (1, 0)    adds a matrix translation to the position only
     f8 sum of weighted position xy   f9 sum of normal xy   f10 sum of (z, nz)
     f11 weight   f12 (z, nz) result  f13 (nz, nz)

   One matrix applied to a point and a normal (APPLY): with the point in f4-f6
       position xy  = c0 * x + c1 * y + c2 * z + c3          (columns in pairs)
       normal xy    = c0 * nx + c1 * ny + c2 * nz
       (z, nz)      = (x, nx) * m2 + (y, ny) * m5 + (z, nz) * m8 + (1, 0) * m11
   The matrices are the guest's floats as they lie: column-major, twelve a
   matrix, so a column's xy are adjacent and its z is the next float.

   After the world matrix the colour is the base plus, for each light, the
   diffuse colour times the dot product of the normal and the light's
   direction (zero or above), plus the specular term for lights that have
   one; the result is stored as four bytes, which the quantised store
   saturates to 0..255.  The numbers are those of light_flat() in raster.c. */

#ifdef __wii__

#include <stddef.h>
#include "skin_ps.h"

_Static_assert(offsetof(SkinPS, bone) == 0 && offsetof(SkinPS, world) == 4 && offsetof(SkinPS, nw) == 8 &&
                   offsetof(SkinPS, size) == 12 && offsetof(SkinPS, off_pos) == 16 && offsetof(SkinPS, off_wt) == 20 &&
                   offsetof(SkinPS, off_nrm) == 24 && offsetof(SkinPS, off_tc) == 28 && offsetof(SkinPS, gqr1) == 32 &&
                   offsetof(SkinPS, gqr6) == 36 && offsetof(SkinPS, gqr7) == 40 && offsetof(SkinPS, suv) == 48 &&
                   offsetof(SkinPS, ouv) == 56 && offsetof(SkinPS, wh) == 64 && offsetof(SkinPS, lo) == 72 &&
                   offsetof(SkinPS, hi) == 76 && offsetof(SkinPS, c01) == 80 && offsetof(SkinPS, c2a) == 88 &&
                   offsetof(SkinPS, lp) == 96 && offsetof(SkinPS, dp) == 144 && offsetof(SkinPS, half) == 192 &&
                   offsetof(SkinPS, zero) == 196 && offsetof(SkinPS, three_half) == 200 && offsetof(SkinPS, zeros) == 208 &&
                   offsetof(SkinPS, hp) == 216 && offsetof(SkinPS, sc) == 264 && offsetof(SkinPS, spec_mask) == 312 &&
                   offsetof(SkinPS, iexp) == 316 && offsetof(SkinPS, rgba_const) == 320,
               "SkinPS layout does not match the assembly");

#define APPLY(R)                                                                                                    \
    "psq_l 0,0(" #R "),0,0\n"                                                                                       \
    "ps_muls0 1,0,4\n"                                                                                              \
    "ps_muls1 2,0,4\n"                                                                                              \
    "psq_l 0,12(" #R "),0,0\n"                                                                                      \
    "ps_madds0 1,0,5,1\n"                                                                                           \
    "ps_madds1 2,0,5,2\n"                                                                                           \
    "psq_l 0,24(" #R "),0,0\n"                                                                                      \
    "ps_madds0 1,0,6,1\n"                                                                                           \
    "ps_madds1 2,0,6,2\n"                                                                                           \
    "psq_l 0,36(" #R "),0,0\n"                                                                                      \
    "ps_add 1,1,0\n"                                                                                                \
    "lfs 3,8(" #R ")\n"                                                                                             \
    "ps_muls0 12,4,3\n"                                                                                             \
    "lfs 3,20(" #R ")\n"                                                                                            \
    "ps_madds0 12,5,3,12\n"                                                                                         \
    "lfs 3,32(" #R ")\n"                                                                                            \
    "ps_madds0 12,6,3,12\n"                                                                                         \
    "lfs 3,44(" #R ")\n"                                                                                            \
    "ps_madds0 12,7,3,12\n"

/* The diffuse term of light I: the dot product of the normal (f2, f13) with
   the direction, kept at zero or above (f4), times the diffuse colour, onto
   the colour pairs f8, f9. */
#define DIFFUSE(I)                                                                                                  \
    "psq_l 4," #I "*16+96(3),0,0\n"                                                                                 \
    "psq_l 5," #I "*16+104(3),0,0\n"                                                                                \
    "ps_mul 4,2,4\n"                                                                                                \
    "ps_madds0 4,5,13,4\n"                                                                                          \
    "ps_sum0 4,4,4,4\n"                                                                                             \
    "ps_sel 4,4,4,10\n"                                                                                             \
    "psq_l 5," #I "*16+144(3),0,0\n"                                                                                \
    "psq_l 6," #I "*16+152(3),0,0\n"                                                                                \
    "ps_madds0 8,5,4,8\n"                                                                                           \
    "ps_madds0 9,6,4,9\n"

/* The specular term of light I (BIT is its bit in the mask), after its
   diffuse term: the half vector's dot product with the normal, kept at zero
   or above, to the whole exponent by squaring, counted only where the light
   faces the vertex (f4 above zero), times the specular colour.  f7.ps0 is
   1.0 and f10 zero. */
#define SPECULAR(I, BIT)                                                                                            \
    "lwz 9,312(3)\n andi. 10,9," #BIT "\n beq 4f\n"                                                                 \
    "psq_l 5," #I "*16+216(3),0,0\n psq_l 6," #I "*16+224(3),0,0\n"                                                 \
    "ps_mul 5,2,5\n ps_madds0 5,6,13,5\n ps_sum0 5,5,5,5\n ps_sel 5,5,5,10\n"                                       \
    "ps_mr 6,7\n lwz 9,316(3)\n"                                                                                    \
    "10:\n andi. 10,9,1\n beq 11f\n fmuls 6,6,5\n"                                                                  \
    "11:\n srwi. 9,9,1\n beq 12f\n fmuls 5,5,5\n b 10b\n"                                                           \
    "12:\n"                                                                                                         \
    "ps_neg 5,4\n ps_sel 6,5,10,6\n"                                                                                \
    "psq_l 5," #I "*16+264(3),0,0\n psq_l 11," #I "*16+272(3),0,0\n"                                                \
    "ps_madds0 8,5,6,8\n ps_madds0 9,11,6,9\n"                                                                      \
    "4:\n"

/* The normal's length: normalised unless it is already near one. */
#define NORMALISE                                                                                                   \
    "ps_merge11 13,12,12\n"                                                                                         \
    "ps_mul 3,2,2\n ps_mul 4,13,13\n ps_sum0 3,3,3,3\n ps_add 3,3,4\n"                                              \
    "lfs 5,72(3)\n fcmpu 0,3,5\n blt 6f\n"                                                                          \
    "lfs 6,76(3)\n fcmpu 0,3,6\n ble 7f\n"                                                                          \
    "6:\n"                                                                                                          \
    "lfs 5,196(3)\n fcmpu 0,3,5\n ble 7f\n"                                                                         \
    "frsqrte 5,3\n frsp 5,5\n"                                                                                      \
    "lfs 6,192(3)\n fmuls 6,6,3\n lfs 8,200(3)\n"                                                                   \
    "fmuls 9,5,5\n fnmsubs 9,6,9,8\n fmuls 5,5,9\n"                                                                 \
    "fmuls 9,5,5\n fnmsubs 9,6,9,8\n fmuls 5,5,9\n"                                                                 \
    "ps_muls0 2,2,5\n ps_muls0 13,13,5\n"                                                                           \
    "7:\n"

#define COLOUR_BASE "psq_l 8,80(3),0,0\n psq_l 9,88(3),0,0\n psq_l 10,208(3),0,0\n"
#define COLOUR_STORE "psq_st 8,20(5),0,1\n psq_st 9,22(5),0,1\n"

#define TAIL_DIFFUSE NORMALISE COLOUR_BASE DIFFUSE(0) DIFFUSE(1) DIFFUSE(2) COLOUR_STORE
#define TAIL_SPEC NORMALISE COLOUR_BASE DIFFUSE(0) SPECULAR(0, 1) DIFFUSE(1) SPECULAR(1, 2) DIFFUSE(2) SPECULAR(2, 4) COLOUR_STORE
#define TAIL_CONST "lwz 9,320(3)\n stw 9,20(5)\n"

/* The chip lights the vertex: the normalised world normal goes out as three 16-bit numbers with 14 fraction
   bits (the form the chip reads), in place of the colour.  Quantisation register 6 stores them. */
#define TAIL_NRM NORMALISE "psq_st 2,12(5),0,6\n psq_st 13,16(5),1,6\n"

#define NRM_LOAD                                                                                                    \
    "lwz 10,24(3)\n add 10,4,10\n"                                                                                  \
    "psq_l 2,0(10),0,7\n psq_l 3,2(10),1,7\n"
#define NRM_NONE "psq_l 2,208(3),0,0\n psq_l 3,208(3),0,0\n"

/* The texture coordinates are stored OFF bytes into the vertex: 12 after the position when the colour follows
   it (the chip's vertex, 24 bytes), 20 when a normal does (28 bytes). */
#define TC_NONE(OFF) "psq_l 4,56(3),0,0\n psq_st 4," #OFF "(5),0,0\n"

#define TC_U8(OFF)                                                                                                  \
    "lwz 9,28(3)\n add 9,4,9\n"                                                                                     \
    "psq_l 4,0(9),0,1\n psq_l 5,48(3),0,0\n psq_l 6,56(3),0,0\n"                                                    \
    "ps_madd 4,4,5,6\n psq_st 4," #OFF "(5),0,0\n"

#define TC_F32(OFF)                                                                                                 \
    "lwz 9,28(3)\n add 9,4,9\n li 10,4\n"                                                                           \
    "lwbrx 11,0,9\n lwbrx 12,9,10\n stw 11,16(1)\n stw 12,20(1)\n"                                                  \
    "psq_l 4,16(1),0,0\n psq_l 5,48(3),0,0\n psq_l 6,56(3),0,0\n"                                                   \
    "ps_madd 4,4,5,6\n psq_st 4," #OFF "(5),0,0\n"

/* r3 k, r4 vertex, r5 out (24 bytes a vertex, 28 with a normal), r6 count; r7 weight pointer,
   r8 matrix pointer, r12 the end of the weights, r9-r11 and r0 temporaries.
   NRM loads the normal, TAIL lights it and stores the colour, TC does the
   texture coordinates. */
#define POS_STORE "psq_st 1,0(5),0,0\n psq_st 12,8(5),1,0\n"
#define SKIN_PS_FN(NAME, NRM, TAIL, TCBLOCK, STRIDE) SKIN_PS_FN_X(NAME, NRM, TAIL, TCBLOCK, STRIDE, POS_STORE)
#define SKIN_PS_FN_X(NAME, NRM, TAIL, TCBLOCK, STRIDE, POSST)                                                           \
    __asm__(".text\n.align 5\n.globl " #NAME "\n" #NAME ":\n"                                                       \
            "stwu 1,-48(1)\n"                                                                                       \
            "lwz 7,32(3)\n lwz 8,36(3)\n lwz 9,40(3)\n"                                                             \
            "mtspr 913,7\n mtspr 918,8\n mtspr 919,9\n"                                                             \
            "mtctr 6\n"                                                                                             \
            "lwz 6,12(3)\n"                                                                                         \
            "psq_l 7,64(3),0,0\n"                                                                                   \
            "9:\n"                                                                                                  \
            /* the position: three little-endian halfwords, staged big-endian for the quantised load */             \
            "lwz 9,16(3)\n add 9,4,9\n"                                                                             \
            "li 10,2\n li 11,4\n"                                                                                   \
            "lhbrx 12,0,9\n sth 12,16(1)\n"                                                                         \
            "lhbrx 12,9,10\n sth 12,18(1)\n"                                                                        \
            "lhbrx 12,9,11\n sth 12,20(1)\n"                                                                        \
            "psq_l 0,16(1),0,6\n psq_l 1,20(1),1,6\n"                                                               \
            NRM                                                                                                     \
            "ps_merge00 4,0,2\n ps_merge11 5,0,2\n ps_merge00 6,1,3\n"                                              \
            "ps_sub 8,4,4\n ps_sub 9,4,4\n ps_sub 10,4,4\n"                                                         \
            /* the weighted sum over the bones with a weight */                                                     \
            "lwz 7,20(3)\n add 7,4,7\n lwz 0,8(3)\n add 12,7,0\n lwz 8,0(3)\n"                                      \
            "1:\n"                                                                                                  \
            "lbz 0,0(7)\n cmpwi 0,0\n beq 3f\n"                                                                     \
            "psq_l 11,0(7),1,1\n" APPLY(8)                                                                          \
            "ps_madds0 8,1,11,8\n ps_madds0 9,2,11,9\n ps_madds0 10,12,11,10\n"                                     \
            "3:\n"                                                                                                  \
            "addi 7,7,1\n addi 8,8,48\n cmpw 7,12\n bne 1b\n"                                                       \
            /* the world matrix over the sums */                                                                    \
            "ps_merge00 4,8,9\n ps_merge11 5,8,9\n ps_mr 6,10\n"                                                    \
            "lwz 8,4(3)\n" APPLY(8)                                                                                 \
            POSST                                                                                                   \
            TAIL                                                                                                    \
            TCBLOCK                                                                                                 \
            "add 4,4,6\n addi 5,5," #STRIDE "\n bdnz 9b\n"                                                          \
            "addi 1,1,48\n blr\n")

SKIN_PS_FN(tkdr_skin_ps_cn, NRM_NONE, TAIL_CONST, TC_NONE(12), 24);
SKIN_PS_FN(tkdr_skin_ps_cu, NRM_NONE, TAIL_CONST, TC_U8(12), 24);
SKIN_PS_FN(tkdr_skin_ps_cf, NRM_NONE, TAIL_CONST, TC_F32(12), 24);
SKIN_PS_FN(tkdr_skin_ps_dn, NRM_LOAD, TAIL_DIFFUSE, TC_NONE(12), 24);
SKIN_PS_FN(tkdr_skin_ps_du, NRM_LOAD, TAIL_DIFFUSE, TC_U8(12), 24);
SKIN_PS_FN(tkdr_skin_ps_df, NRM_LOAD, TAIL_DIFFUSE, TC_F32(12), 24);
SKIN_PS_FN(tkdr_skin_ps_sn, NRM_LOAD, TAIL_SPEC, TC_NONE(12), 24);
SKIN_PS_FN(tkdr_skin_ps_su, NRM_LOAD, TAIL_SPEC, TC_U8(12), 24);
SKIN_PS_FN(tkdr_skin_ps_sf, NRM_LOAD, TAIL_SPEC, TC_F32(12), 24);
SKIN_PS_FN(tkdr_skin_ps_hn, NRM_LOAD, TAIL_NRM, TC_NONE(20), 28);
SKIN_PS_FN(tkdr_skin_ps_hu, NRM_LOAD, TAIL_NRM, TC_U8(20), 28);
SKIN_PS_FN(tkdr_skin_ps_hf, NRM_LOAD, TAIL_NRM, TC_F32(20), 28);

/* The chip-lit kernels again, writing the vertex as the chip reads it (position, normal, texture coordinate: 12 + 6 + 8 bytes, no
   padding) to ONE address: the write-gather pipe (0xCC008000), which turns every store to it into the next bytes of the command
   stream, so a draw needs no staging buffer and no loop that copies the staging buffer to the pipe.  The stores keep the order
   of the stream (position, normal, texture coordinate), r5 never moves.  The caller has done GX_Begin for the vertex count. */
#define POS_PIPE "psq_st 1,0(5),0,0\n psq_st 12,0(5),1,0\n"
#define TAIL_NRM_PIPE NORMALISE "psq_st 2,0(5),0,6\n psq_st 13,0(5),1,6\n"
SKIN_PS_FN_X(tkdr_skin_ps_hn_pipe, NRM_LOAD, TAIL_NRM_PIPE, TC_NONE(0), 0, POS_PIPE);
SKIN_PS_FN_X(tkdr_skin_ps_hu_pipe, NRM_LOAD, TAIL_NRM_PIPE, TC_U8(0), 0, POS_PIPE);
SKIN_PS_FN_X(tkdr_skin_ps_hf_pipe, NRM_LOAD, TAIL_NRM_PIPE, TC_F32(0), 0, POS_PIPE);
void tkdr_skin_ps_hn_pipe(const SkinPS *, const uint8_t *, void *, int);
void tkdr_skin_ps_hu_pipe(const SkinPS *, const uint8_t *, void *, int);
void tkdr_skin_ps_hf_pipe(const SkinPS *, const uint8_t *, void *, int);
const SkinPSFn g_skin_ps_pipe[3] = {tkdr_skin_ps_hn_pipe, tkdr_skin_ps_hu_pipe, tkdr_skin_ps_hf_pipe};

#ifdef EXP_PIPECHK
/* The same stores to an ordinary buffer, one after the other (r5 moves by what each store wrote), so that the stream a pipe kernel
   sends can be read back and compared with the staging kernel's vertices. */
#define POS_DENSE "psq_st 1,0(5),0,0\n addi 5,5,8\n psq_st 12,0(5),1,0\n addi 5,5,4\n"
#define TAIL_NRM_DENSE NORMALISE "psq_st 2,0(5),0,6\n addi 5,5,4\n psq_st 13,0(5),1,6\n addi 5,5,2\n"
#define STEP8 "addi 5,5,8\n"
SKIN_PS_FN_X(tkdr_skin_ps_hn_dense, NRM_LOAD, TAIL_NRM_DENSE, TC_NONE(0) STEP8, 0, POS_DENSE);
SKIN_PS_FN_X(tkdr_skin_ps_hu_dense, NRM_LOAD, TAIL_NRM_DENSE, TC_U8(0) STEP8, 0, POS_DENSE);
SKIN_PS_FN_X(tkdr_skin_ps_hf_dense, NRM_LOAD, TAIL_NRM_DENSE, TC_F32(0) STEP8, 0, POS_DENSE);
void tkdr_skin_ps_hn_dense(const SkinPS *, const uint8_t *, void *, int);
void tkdr_skin_ps_hu_dense(const SkinPS *, const uint8_t *, void *, int);
void tkdr_skin_ps_hf_dense(const SkinPS *, const uint8_t *, void *, int);
const SkinPSFn g_skin_ps_dense[3] = {tkdr_skin_ps_hn_dense, tkdr_skin_ps_hu_dense, tkdr_skin_ps_hf_dense};
#endif

void tkdr_skin_ps_cn(const SkinPS *, const uint8_t *, void *, int);
void tkdr_skin_ps_cu(const SkinPS *, const uint8_t *, void *, int);
void tkdr_skin_ps_cf(const SkinPS *, const uint8_t *, void *, int);
void tkdr_skin_ps_dn(const SkinPS *, const uint8_t *, void *, int);
void tkdr_skin_ps_du(const SkinPS *, const uint8_t *, void *, int);
void tkdr_skin_ps_df(const SkinPS *, const uint8_t *, void *, int);
void tkdr_skin_ps_sn(const SkinPS *, const uint8_t *, void *, int);
void tkdr_skin_ps_su(const SkinPS *, const uint8_t *, void *, int);
void tkdr_skin_ps_sf(const SkinPS *, const uint8_t *, void *, int);
void tkdr_skin_ps_hn(const SkinPS *, const uint8_t *, void *, int);
void tkdr_skin_ps_hu(const SkinPS *, const uint8_t *, void *, int);
void tkdr_skin_ps_hf(const SkinPS *, const uint8_t *, void *, int);

const SkinPSFn g_skin_ps_fn[4][3] = {
    {tkdr_skin_ps_cn, tkdr_skin_ps_cu, tkdr_skin_ps_cf},
    {tkdr_skin_ps_dn, tkdr_skin_ps_du, tkdr_skin_ps_df},
    {tkdr_skin_ps_sn, tkdr_skin_ps_su, tkdr_skin_ps_sf},
    {tkdr_skin_ps_hn, tkdr_skin_ps_hu, tkdr_skin_ps_hf},
};

#endif
