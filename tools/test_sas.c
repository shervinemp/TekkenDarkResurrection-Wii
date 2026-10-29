/* Differential test of the SAS mixer and the audio stream mixer against the plain code they replaced.

   It compiles runtime/hle/sas.c and runtime/hle/audio.c with -DEXP_SASCHK, so every sceSasCore() call mixes a copy
   of the voices with the old algorithm (sas_ref.h) and compares the output grain and every voice field with what
   the fast code produced, and every audio_pull() compares its output and the channels' read positions with the
   old loop.  This driver only has to feed them: random voices (VAG data with random filters, shifts, loop and
   end flags; noise; every envelope curve; pitches from 0 to 8x; negative and large volumes and sends), random
   changes between calls (key off, pause, pitch, volume, reverb switches), WithMix calls, odd grain sizes and
   unaligned output buffers, and random channel rings.

   Build and run (from the repository root, with the devkitPro msys2 gcc):
       gcc -O2 -DEXP_SASCHK -o test_sas tools/test_sas.c
       ./test_sas [seed] [rounds]
   Exit status 0 when every call matched. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/hle/sas.c"
#include "../runtime/hle/audio.c"

/* what the HLE and the platform would otherwise provide */
int g_hle_log;
uint8_t *g_mem, *g_vram, *g_low;
uint64_t hle_time_us(void) { return 0; }
void thread_delay(Ctx *c, uint64_t usec, int allow_cb) { (void)c; (void)usec; (void)allow_cb; }

static uint64_t g_rng = 88172645463325252ull;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}
static uint32_t below(uint32_t n) { return rnd() % n; }
static int chance(uint32_t percent) { return below(100) < percent; }

#define VAG_BASE 0x08900000u
#define OUT_BASE 0x08F00000u
#define IN_BASE 0x08F40000u

static Ctx c;
#define CALL(fn, ...)                                  \
    do {                                               \
        const uint32_t a_[] = {__VA_ARGS__};           \
        memset(c.r, 0, sizeof c.r);                    \
        for (unsigned i_ = 0; i_ < sizeof a_ / sizeof a_[0]; i_++) c.r[4 + i_] = a_[i_];   \
        fn(&c);                                        \
    } while (0)

/* A VAG sample of `nblocks` blocks at `addr`; returns its size in bytes. */
static uint32_t make_vag(uint32_t addr, int nblocks) {
    uint8_t *p = guest_host_ptr(addr, (uint32_t)nblocks * 16u);
    const int loop_start = chance(50) ? (int)below((uint32_t)nblocks) : -1;
    for (int b = 0; b < nblocks; b++) {
        uint8_t *blk = p + b * 16;
        const int pred = chance(30) ? 0 : (int)below(16), shift = chance(80) ? (int)below(13) : (int)below(16);
        blk[0] = (uint8_t)(pred << 4 | shift);
        blk[1] = 0;
        if (b == loop_start) blk[1] = 6;
        if (b == nblocks - 1) blk[1] = loop_start >= 0 ? 3 : (chance(50) ? 1 : 0);
        if (chance(2) && b > 0) blk[1] = 7;   /* the end flag in the middle */
        for (int i = 2; i < 16; i++) blk[i] = (uint8_t)(chance(10) ? 0 : rnd());
    }
    return (uint32_t)nblocks * 16u;
}

static int rate(void) {
    switch (below(5)) {
    case 0: return 0;
    case 1: return (int)below(64);
    case 2: return (int)below(100000);
    case 3: return (int)(rnd() & 0x7FFFFFFF);
    default: return (int)rnd();
    }
}

static void setup_voice(int n) {
    const uint32_t addr = VAG_BASE + (uint32_t)n * 0x1000u, nb = 1u + below(40);
    const uint32_t size = make_vag(addr, (int)nb);
    if (chance(8)) {
        CALL(__sceSasSetNoise, 0, (uint32_t)n, below(64));
    } else {
        CALL(__sceSasSetVoice, 0, (uint32_t)n, addr, size, chance(70));
    }
    uint32_t pitch;
    switch (below(6)) {
    case 0: pitch = 0x1000; break;
    case 1: pitch = below(0x8000); break;
    case 2: pitch = 0x800 + below(0x1000); break;
    case 3: pitch = below(64); break;
    case 4: pitch = 0x7FFF; break;
    default: pitch = 0x400 + below(0x2000); break;
    }
    CALL(__sceSasSetPitch, 0, (uint32_t)n, pitch);
    const int big = chance(10);
    CALL(__sceSasSetVolume, 0, (uint32_t)n, big ? rnd() : below(0x1800), big ? rnd() : below(0x1800),
         chance(50) ? 0u : (big ? rnd() : below(0x1800)), chance(50) ? 0u : (big ? rnd() : below(0x1800)));
    if (chance(35)) {
        /* attack in a sample, a decay that lands in a sustain of rate zero: a held note */
        CALL(__sceSasSetADSRmode, 0, (uint32_t)n, 15u, 0u, chance(70) ? 1u : 3u, chance(80) ? below(4) : 4 + below(2), below(6));
        CALL(__sceSasSetADSR, 0, (uint32_t)n, 15u, 0x40000000u, (1u << 26) + below(1u << 28), 0u, (uint32_t)rate());
        CALL(__sceSasSetSL, 0, (uint32_t)n, (1u << 25) + below(1u << 28));
    } else if (chance(60)) {
        CALL(__sceSasSetSimpleADSR, 0, (uint32_t)n, rnd() & 0xFFFF, rnd() & 0xFFFF);
    } else {
        CALL(__sceSasSetADSRmode, 0, (uint32_t)n, 15u, below(6), below(6), below(6), below(6));
        CALL(__sceSasSetADSR, 0, (uint32_t)n, 15u, (uint32_t)rate(), (uint32_t)rate(), chance(40) ? 0u : (uint32_t)rate(), (uint32_t)rate());
        CALL(__sceSasSetSL, 0, (uint32_t)n, rnd() >> 1);
    }
    CALL(__sceSasSetKeyOn, 0, (uint32_t)n);
}

static void wiggle(void) {
    const uint32_t n = below(NVOICES);
    switch (below(9)) {
    case 0: CALL(__sceSasSetKeyOff, 0, n); break;
    case 1: CALL(__sceSasSetPause, 0, 1u << n, below(2)); break;
    case 2: CALL(__sceSasSetPitch, 0, n, below(0x8000)); break;
    case 3: CALL(__sceSasSetVolume, 0, n, below(0x1800), below(0x1800), below(0x1800), below(0x1800)); break;
    case 4: CALL(__sceSasRevVON, 0, below(4) != 0, below(2)); break;
    case 5: setup_voice((int)n); break;
    case 6: CALL(__sceSasSetSimpleADSR, 0, n, rnd() & 0xFFFF, rnd() & 0xFFFF); break;
    case 7: CALL(__sceSasSetSL, 0, n, rnd() >> 1); break;
    default: break;
    }
}

static void sas_round(void) {
    const uint32_t grains[] = {32, 64, 96, 128, 256, 512, 768, 1024, 2048, 100, 33, 1, 7};
    const uint32_t grain = chance(80) ? grains[below(9)] : grains[below(13)];
    CALL(__sceSasInit, 0, grain, 0, 0, 44100);
    CALL(__sceSasRevVON, 0, 1, 0);
    memset(guest_host_ptr(VAG_BASE, 0x40000), 0, 0x40000);
    const int nv = 1 + (int)below(NVOICES);
    for (int n = 0; n < nv; n++) setup_voice(n);
    const int calls = 20 + (int)below(80);
    for (int k = 0; k < calls; k++) {
        const uint32_t out = OUT_BASE + (chance(80) ? 0u : 2u * below(4));
        if (chance(10)) {
            for (uint32_t i = 0; i < 4096; i++) guest_host_ptr(IN_BASE, 4096)[i] = (uint8_t)rnd();
            CALL(__sceSasCoreWithMix, 0, out, below(0x2000), below(0x2000));
        } else if (chance(5)) {
            /* the output buffer doubles as the input, as the game does */
            for (uint32_t i = 0; i < 4096; i++) guest_host_ptr(OUT_BASE, 4096)[i] = (uint8_t)rnd();
            CALL(__sceSasCoreWithMix, 0, OUT_BASE, below(0x2000), below(0x2000));
        } else {
            CALL(__sceSasCore, 0, out);
        }
        if (chance(30)) wiggle();
        if (chance(10)) setup_voice((int)below(NVOICES));
    }
}

static void audio_round(void) {
    for (int k = 0; k < NCH; k++) {
        Channel *ch = &g_ch[k];
        memset(&ch->w, 0, sizeof ch->w);
        const uint32_t base = chance(10) ? 0xFFFFF000u : rnd();   /* the counters wrap */
        const uint32_t held = chance(30) ? 0u : (chance(50) ? below(40) : below(5000));
        ch->r = base;
        ch->w = base + held;
        for (uint32_t i = 0; i < RING; i++) {
            const int big = chance(10);
            ch->ring[i][0] = (int16_t)(big ? (chance(50) ? 32767 : -32768) : (int)rnd());
            ch->ring[i][1] = (int16_t)(big ? (chance(50) ? 32767 : -32768) : (int)rnd());
        }
    }
    static int16_t out[5000 * 2];
    const int pulls = 1 + (int)below(6);
    for (int p = 0; p < pulls; p++) {
        const int frames = chance(50) ? 1024 : (int)(1 + below(1500));
        audio_pull(out, frames);
        if (chance(40)) {
            const int k = (int)below(NCH);
            g_ch[k].w += below(3000);
        }
    }
}

int main(int argc, char **argv) {
    const uint32_t seed = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 1;
    const int rounds = argc > 2 ? atoi(argv[2]) : 300;
    g_rng ^= (uint64_t)seed * 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 10; i++) rnd();
    g_mem = calloc(GUEST_MEM_SIZE, 1);
    if (!g_mem) { fprintf(stderr, "no memory for the guest window\n"); return 2; }
    for (int r = 0; r < rounds; r++) {
        sas_round();
        audio_round();
    }
    printf("seed %u: %d rounds, sas calls %u bad %u, audio pulls %u bad %u\n", seed, rounds, g_sas_checks, g_sas_bad, g_aud_checks, g_aud_bad);
    printf("coverage: playing voice-calls %u (held notes %u), noise %u, ended %u, with sends %u, WithMix %u, wet %u, dry off %u\n", g_sas_cov[0],
           g_sas_cov[7], g_sas_cov[1], g_sas_cov[2], g_sas_cov[3], g_sas_cov[4], g_sas_cov[5], g_sas_cov[6]);
    return (g_sas_bad || g_aud_bad || !g_sas_checks || !g_aud_checks) ? 1 : 0;
}
