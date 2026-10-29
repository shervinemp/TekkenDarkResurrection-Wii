/* Checks runtime/hle/sha1_core.h on the PC.

   1. sha1_block()/sha1_rounds() against the loop the override used before it was unrolled (copied below): the new
      state, the rewritten message schedule and the working variables and temporaries of the last round, on random
      states and blocks (the registers the guest's code leaves are part of the contract of the override).
   2. sha1_buffer() against a plain textbook SHA-1 and the published test vectors, for every length from 0 to 700
      and some longer ones, at all four alignments of the data.

   Build and run (from the repository root, with the devkitPro msys2 gcc through bash -lc):
       gcc -O2 -Wall -o test_sha1 tools/test_sha1.c && ./test_sha1 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/hle/sha1_core.h"

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint32_t rnd32(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

/* ---- the loop of ovr_08A36278 before this change */
static void old_transform(uint32_t w[16], const uint32_t h[5], uint32_t hout[5], uint32_t reg[8]) {
    uint32_t a = h[0], b = h[1], cc = h[2], d = h[3], e = h[4], t13 = 0, t14 = 0, t15 = 0;
    for (int t = 0; t < 80; t++) {
        uint32_t x = w[t & 15], f, k;
        if (t >= 16) {
            x = sha1_rotl(x ^ w[(t + 2) & 15] ^ w[(t + 8) & 15] ^ w[(t + 13) & 15], 1);
            w[t & 15] = x;
        }
        t13 = x + e + sha1_rotl(a, 5);
        if (t < 20) { f = (b & cc) | (~b & d); k = 0x5a827999u; }
        else if (t < 40) { f = b ^ cc ^ d; k = 0x6ed9eba1u; }
        else if (t < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8f1bbcdcu; }
        else { f = b ^ cc ^ d; k = 0xca62c1d6u; }
        t14 = f + k;
        t15 = b << 30;
        const uint32_t na = t13 + t14;
        e = d; d = cc; cc = sha1_rotl(b, 30); b = a; a = na;
    }
    hout[0] = h[0] + a; hout[1] = h[1] + b; hout[2] = h[2] + cc; hout[3] = h[3] + d; hout[4] = h[4] + e;
    reg[0] = a; reg[1] = b; reg[2] = cc; reg[3] = d; reg[4] = e; reg[5] = t13; reg[6] = t14; reg[7] = t15;
}

/* ---- a textbook SHA-1 */
static void ref_sha1(const uint8_t *p, size_t n, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint8_t *m = calloc(1, n + 72);
    memcpy(m, p, n);
    m[n] = 0x80;
    const size_t total = ((n + 8) / 64 + 1) * 64;
    const uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) m[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)m[off + 4 * i] << 24 | (uint32_t)m[off + 4 * i + 1] << 16 | (uint32_t)m[off + 4 * i + 2] << 8 | m[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = sha1_rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
            else { f = b ^ c ^ d; k = 0xca62c1d6u; }
            const uint32_t t = sha1_rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = sha1_rotl(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
    free(m);
}

static void digest_of(const uint8_t *p, uint32_t n, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    Sha1Out o;
    sha1_buffer(p, n, h, &o);
    for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

int main(void) {
    int bad = 0;
    for (int it = 0; it < 200000; it++) {
        uint32_t h[5], w0[16], w1[16], hn[5], hold[5], reg[8];
        for (int i = 0; i < 5; i++) h[i] = rnd32();
        for (int i = 0; i < 16; i++) w0[i] = w1[i] = rnd32();
        old_transform(w0, h, hold, reg);
        memcpy(hn, h, sizeof hn);
        Sha1Out o;
        sha1_block(hn, w1, &o);
        const uint32_t now[8] = {o.a, o.b, o.c, o.d, o.e, o.t13, o.t14, o.t15};
        if (memcmp(hn, hold, sizeof hn) || memcmp(w0, w1, sizeof w0) || memcmp(now, reg, sizeof now)) {
            if (++bad < 5) printf("transform differs on iteration %d\n", it);
        }
    }
    printf("transform: %d bad of 200000\n", bad);

    const struct { const char *msg; const char *hex; } vec[] = {
        {"", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
        {"abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
    };
    for (unsigned v = 0; v < 3; v++) {
        uint8_t d[20];
        char hex[41];
        digest_of((const uint8_t *)vec[v].msg, (uint32_t)strlen(vec[v].msg), d);
        for (int i = 0; i < 20; i++) sprintf(hex + 2 * i, "%02x", d[i]);
        if (strcmp(hex, vec[v].hex)) { printf("test vector %u: got %s\n", v, hex); bad++; }
    }

    uint8_t *buf = malloc(70000 + 8);
    for (size_t i = 0; i < 70000 + 8; i++) buf[i] = (uint8_t)rnd32();
    int lens = 0;
    for (uint32_t n = 0; n < 2000 || n == 2000; n = n < 700 ? n + 1 : n + 37) {
        for (int al = 0; al < 4; al++) {
            uint8_t a[20], b[20];
            digest_of(buf + al, n, a);
            ref_sha1(buf + al, n, b);
            lens++;
            if (memcmp(a, b, 20)) { if (++bad < 10) printf("length %u alignment %d differs\n", n, al); }
        }
        if (n >= 2000) break;
    }
    for (uint32_t n = 4096; n <= 65536; n *= 2) {
        uint8_t a[20], b[20];
        digest_of(buf + 1, n + 3, a);
        ref_sha1(buf + 1, n + 3, b);
        lens++;
        if (memcmp(a, b, 20)) { if (++bad < 10) printf("length %u differs\n", n + 3); }
    }
    printf("digests: %d lengths and alignments checked\n", lens);
    printf("%s\n", bad ? "FAILED" : "all good");
    return bad != 0;
}
