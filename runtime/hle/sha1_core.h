/* SHA-1 for runtime/hle/overrides.c (the guest's 0x08A36278 transform and 0x08A363E0 one-shot hash) and
   tools/test_sha1.c.

   sha1_rounds() is the 80 rounds the guest's transform runs, with the message schedule kept in a 16-word
   circular buffer that is rewritten in place (the guest leaves it that way in memory), and it returns what the
   guest's code leaves in registers besides the new state: the working variables after round 79 and the
   temporaries of that last round.  The rounds are written out five at a time so that the five variables rotate
   by renaming instead of by moves, and the logical function of each group of twenty is fixed, so there are no
   branches; 4.6 us a block as a loop, about a third of that unrolled.

   sha1_buffer() is the whole hash as the guest's one-shot function does it, including its limit: the bit count
   is a 32-bit word (word 14 of the last block is always 0). */
#ifndef SHA1_CORE_H
#define SHA1_CORE_H

#include <stdint.h>
#include <string.h>

#ifndef SHA1_TICK
#define SHA1_TICK() ((void)0)   /* called once a block: the cooperative scheduling point of the guest's loops */
#endif

typedef struct { uint32_t a, b, c, d, e, t13, t14, t15; } Sha1Out;

static inline uint32_t sha1_rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

#define SHA1_F0(b, c, d) ((d) ^ ((b) & ((c) ^ (d))))
#define SHA1_F1(b, c, d) ((b) ^ (c) ^ (d))
#define SHA1_F2(b, c, d) (((b) & (c)) | ((d) & ((b) | (c))))
#define SHA1_K0 0x5a827999u
#define SHA1_K1 0x6ed9eba1u
#define SHA1_K2 0x8f1bbcdcu
#define SHA1_K3 0xca62c1d6u

/* The word of round i: the input word for the first sixteen rounds, the next word of the schedule after that. */
#define SHA1_W(i) ((i) < 16 ? w[(i) & 15] : (w[(i) & 15] = sha1_rotl(w[((i) + 13) & 15] ^ w[((i) + 8) & 15] ^ w[((i) + 2) & 15] ^ w[(i) & 15], 1)))
#define SHA1_ROUND(a, b, c, d, e, F, K, i) do { e += sha1_rotl(a, 5) + F(b, c, d) + (K) + SHA1_W(i); b = sha1_rotl(b, 30); } while (0)
#define SHA1_FIVE(F, K, i)                       \
    SHA1_ROUND(a, b, c, d, e, F, K, (i));        \
    SHA1_ROUND(e, a, b, c, d, F, K, (i) + 1);    \
    SHA1_ROUND(d, e, a, b, c, F, K, (i) + 2);    \
    SHA1_ROUND(c, d, e, a, b, F, K, (i) + 3);    \
    SHA1_ROUND(b, c, d, e, a, F, K, (i) + 4)

/* The state h goes in; the working variables after the last round come out in *o, and the caller adds them to h.
   Round 79 is written out to give its temporaries: the new a is t13 + t14, t15 is the old b shifted left by 30. */
static inline void sha1_rounds(uint32_t w[16], const uint32_t h[5], Sha1Out *o) {
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    SHA1_FIVE(SHA1_F0, SHA1_K0, 0);
    SHA1_FIVE(SHA1_F0, SHA1_K0, 5);
    SHA1_FIVE(SHA1_F0, SHA1_K0, 10);
    SHA1_FIVE(SHA1_F0, SHA1_K0, 15);
    SHA1_FIVE(SHA1_F1, SHA1_K1, 20);
    SHA1_FIVE(SHA1_F1, SHA1_K1, 25);
    SHA1_FIVE(SHA1_F1, SHA1_K1, 30);
    SHA1_FIVE(SHA1_F1, SHA1_K1, 35);
    SHA1_FIVE(SHA1_F2, SHA1_K2, 40);
    SHA1_FIVE(SHA1_F2, SHA1_K2, 45);
    SHA1_FIVE(SHA1_F2, SHA1_K2, 50);
    SHA1_FIVE(SHA1_F2, SHA1_K2, 55);
    SHA1_FIVE(SHA1_F1, SHA1_K3, 60);
    SHA1_FIVE(SHA1_F1, SHA1_K3, 65);
    SHA1_FIVE(SHA1_F1, SHA1_K3, 70);
    SHA1_ROUND(a, b, c, d, e, SHA1_F1, SHA1_K3, 75);
    SHA1_ROUND(e, a, b, c, d, SHA1_F1, SHA1_K3, 76);
    SHA1_ROUND(d, e, a, b, c, SHA1_F1, SHA1_K3, 77);
    SHA1_ROUND(c, d, e, a, b, SHA1_F1, SHA1_K3, 78);
    /* round 79, whose roles are (b, c, d, e, a) */
    const uint32_t x = SHA1_W(79);
    o->t13 = x + a + sha1_rotl(b, 5);
    o->t14 = SHA1_F1(c, d, e) + SHA1_K3;
    o->t15 = c << 30;
    a = o->t13 + o->t14;
    c = sha1_rotl(c, 30);
    o->a = a; o->b = b; o->c = c; o->d = d; o->e = e;
}

static inline void sha1_block(uint32_t h[5], uint32_t w[16], Sha1Out *o) {
    sha1_rounds(w, h, o);
    h[0] += o->a; h[1] += o->b; h[2] += o->c; h[3] += o->d; h[4] += o->e;
}

/* The big-endian word at p, whatever p's alignment (a misaligned lwz is slow, and traps across a page), by two
   aligned loads: it may read up to three bytes past the last one it needs. */
typedef uint32_t __attribute__((may_alias)) sha1_u32a;
static inline uint32_t sha1_be32(const uint8_t *p) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    const uintptr_t a = (uintptr_t)p;
    const sha1_u32a *q = (const sha1_u32a *)(a & ~(uintptr_t)3);
    const unsigned s = (unsigned)(a & 3) * 8;
    return s ? (q[0] << s) | (q[1] >> (32 - s)) : q[0];
#else
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
#endif
}

/* The 5 words the hash starts from go in h; the digest words come out in h.  *o is the last block's outputs. */
static inline void sha1_buffer(const uint8_t *p, uint32_t n, uint32_t h[5], Sha1Out *o) {
    uint32_t w[16];
    uint32_t left = n;
    while (left >= 64) {
        for (int i = 0; i < 16; i++) w[i] = sha1_be32(p + 4 * i);
        sha1_block(h, w, o);
        SHA1_TICK();
        p += 64;
        left -= 64;
    }
    uint8_t tail[64];
    for (uint32_t i = 0; i < left; i++) tail[i] = p[i];
    tail[left] = 0x80;
    for (uint32_t i = left + 1; i < 64; i++) tail[i] = 0;
    for (int i = 0; i < 16; i++) w[i] = sha1_be32(tail + 4 * i);
    if (left >= 56) {   /* the length does not fit after the marker: a second block holds it alone */
        sha1_block(h, w, o);
        SHA1_TICK();
        for (int i = 0; i < 16; i++) w[i] = 0;
    }
    w[14] = 0;
    w[15] = n << 3;
    sha1_block(h, w, o);
}
#endif
