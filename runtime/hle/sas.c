// sceSasCore: Sony's software mixer. 32 voices play VAG (PSP ADPCM) samples or
// noise through an ADSR envelope, resampled by pitch, into a stereo grain.
#include <stdlib.h>

#include "hle.h"

#define NVOICES 32
#define PITCH_BASE 0x1000
#define ENV_MAX 0x40000000LL

enum { CURVE_LIN_INC, CURVE_LIN_DEC, CURVE_LIN_BENT, CURVE_EXP_DEC, CURVE_EXP_INC, CURVE_DIRECT };
enum { ST_OFF, ST_KEYON, ST_KEYON_STEP, ST_ATTACK, ST_DECAY, ST_SUSTAIN, ST_RELEASE };

typedef struct {
    int64_t height;
    int state;
    int rate[4], type[4];  // attack, decay, sustain, release
    int64_t sustain_level;
} Envelope;

typedef struct {
    // VAG stream
    uint32_t vag_addr, vag_size, read;
    int loop_enabled, loop_next, loop_block, block, nblocks, ended;
    int16_t samples[28];
    int cur, s1, s2;
    // voice
    int noise, noise_freq;
    uint32_t noise_state;
    int on, playing, paused;
    int pitch, vol_l, vol_r, eff_l, eff_r;
    uint32_t frac;
    int16_t prev, next;
    Envelope env;
} Voice;

static Voice g_v[NVOICES];
static int g_grain = 256, g_out_mode, g_dry = 1, g_wet;
static int32_t g_mix[2048 * 2], g_send[2048 * 2];

#ifdef TKDR_AUTOPILOT
/* Sound cost probes of the demo build (the SOUND lines): raw time base ticks (60.75 MHz), the call that takes
   them is the guest thread's, so a tick spent while another thread had the CPU is counted too. */
static inline uint32_t snd_tb(void) { uint32_t t; __asm__ volatile("mftb %0" : "=r"(t)); return t; }
/* ticks: 0 the whole of core_mix, 1 held notes at the source rate, 2 held notes resampled, 3 voices whose
   envelope moves, 4 the output loop, 5 the VAG block decoder; count: 0 calls, 1-3 voices of kinds 1-3, 4 noise
   voices, 5 voice samples (voices x grain), 6 VAG blocks, 7 voices with an effect send */
uint64_t g_snd_sas[8], g_snd_sasn[8];
#define SND(stmt) stmt
#else
#define SND(stmt)
#endif

// PSP VAG prediction filters (the PS1 set extended to 16 entries)
static const int k_filter[16][2] = {
    {0, 0}, {60, 0}, {115, 52}, {98, 55}, {122, 60}, {0, 0}, {0, 0}, {52, 0},
    {55, 2}, {60, 125}, {0, 0}, {0, 91}, {0, 0}, {2, 216}, {125, 6}, {0, 151},
};

static int16_t clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

static void vag_start(Voice *v) {
    v->read = v->vag_addr;
    v->nblocks = (int)(v->vag_size / 16);
    v->block = -1;
    v->loop_next = 0;
    v->loop_block = -1;
    v->ended = 0;
    v->cur = 28;
    v->s1 = v->s2 = 0;
}

static void vag_decode_block_impl(Voice *v);
static void vag_decode_block(Voice *v) {
#ifdef TKDR_AUTOPILOT
    uint32_t t0 = snd_tb();
    vag_decode_block_impl(v);
    g_snd_sas[5] += snd_tb() - t0;
    g_snd_sasn[6]++;
#else
    vag_decode_block_impl(v);
#endif
}

static void vag_decode_block_impl(Voice *v) {
    if (v->block == v->nblocks - 1) { v->ended = 1; return; }
    const uint32_t p = v->read;
    /* The block's 16 bytes in one go (a block that does not lie in one window is gathered a byte at a time,
       as the guest accessors do). */
    uint8_t gather[16];
    const uint8_t *b = guest_host_ptr(p, 16);
    if (!b) {
        for (uint32_t i = 0; i < 16; i++) gather[i] = mem_r8(p + i);
        b = gather;
    }
    const int hdr = b[0], flags = b[1];
    const int shift = hdr & 15, pred = hdr >> 4;
    if (flags == 7) { v->ended = 1; return; }
    /* The index of this block (v->block is the previous one).  Storing the
       previous index made a loop that starts at block 0 read as "no loop"
       (-1), so such a sample played once and stopped. */
    if (flags == 6) v->loop_block = v->block + 1;
    else if (flags == 3 && v->loop_enabled) v->loop_next = 1;
    const int c1 = k_filter[pred][0], c2 = -k_filter[pred][1];
    int s1 = v->s1, s2 = v->s2;
    int16_t *const out = v->samples;
    if (!(c1 | c2)) {
        /* No prediction: the clamp of (a + 0) is a itself, which is already a 16-bit number. */
        for (int i = 0; i < 14; i++) {
            const int d = b[2 + i];
            s2 = (int16_t)((d & 0xF) << 12) >> shift;
            s1 = (int16_t)((d & 0xF0) << 8) >> shift;
            out[2 * i] = (int16_t)s2;
            out[2 * i + 1] = (int16_t)s1;
        }
    } else {
        for (int i = 0; i < 14; i++) {
            const int d = b[2 + i];
            const int a = (int16_t)((d & 0xF) << 12) >> shift, bb = (int16_t)((d & 0xF0) << 8) >> shift;
            s2 = clamp16(a + ((s1 * c1 + s2 * c2) >> 6));
            s1 = clamp16(bb + ((s2 * c1 + s1 * c2) >> 6));
            out[2 * i] = (int16_t)s2;
            out[2 * i + 1] = (int16_t)s1;
        }
    }
    v->s1 = s1;
    v->s2 = s2;
    v->cur = 0;
    v->block++;
    v->read = p + 16;
}

static int16_t next_sample(Voice *v) {
    if (v->noise) {
        // LFSR noise; the frequency setting only changes how often it advances
        v->noise_state = v->noise_state * 1103515245u + 12345u;
        return (int16_t)(v->noise_state >> 16);
    }
    if (v->ended) return 0;
    if (v->cur == 28) {
        if (v->loop_next && v->loop_block >= 0) {
            v->read = v->vag_addr + 16 * (uint32_t)v->loop_block;
            v->block = v->loop_block - 1;
            v->loop_next = 0;
        }
        vag_decode_block(v);
        if (v->ended) return 0;
    }
    return v->samples[v->cur++];
}

static void walk(Envelope *e, int type, int rate) {
    int64_t d;
    switch (type) {
    case CURVE_LIN_INC: e->height += rate; break;
    case CURVE_LIN_DEC: e->height -= rate; break;
    case CURVE_LIN_BENT: e->height += e->height <= ENV_MAX * 3 / 4 ? rate : rate / 4; break;
    case CURVE_EXP_DEC:
        d = e->height - ENV_MAX;
        d += (-d * (int64_t)(uint32_t)rate) >> 32;
        e->height = d + ENV_MAX - ((int64_t)(uint32_t)rate + 3) / 4;
        break;
    case CURVE_EXP_INC:
        d = e->height - ENV_MAX;
        d += (-d * (int64_t)(uint32_t)rate) >> 32;
        e->height = d + 0x4000 + ENV_MAX;
        break;
    default: e->height = rate; break;
    }
}

static void set_state(Envelope *e, int s) {
    if (e->height > ENV_MAX) e->height = ENV_MAX;
    e->state = s;
}

static void env_step(Envelope *e) {
    switch (e->state) {
    case ST_ATTACK:
        walk(e, e->type[0], e->rate[0]);
        if (e->height >= ENV_MAX || e->height < 0) set_state(e, ST_DECAY);
        break;
    case ST_DECAY:
        walk(e, e->type[1], e->rate[1]);
        if (e->height < e->sustain_level) set_state(e, ST_SUSTAIN);
        break;
    case ST_SUSTAIN:
        walk(e, e->type[2], e->rate[2]);
        if (e->height <= 0) { e->height = 0; set_state(e, ST_RELEASE); }
        break;
    case ST_RELEASE:
        walk(e, e->type[3], e->rate[3]);
        if (e->height <= 0) { e->height = 0; set_state(e, ST_OFF); }
        break;
    case ST_KEYON:
        e->height = 0;
        set_state(e, ST_KEYON_STEP);
        break;
    case ST_KEYON_STEP:
        if (++e->height >= 31) { e->height = 0; set_state(e, ST_ATTACK); }
        break;
    }
}

#if defined(EXP_SASCHK) || defined(EXP_ABSND)
#include "sas_ref.h"
#endif

/* A voice's samples are mixed with its state in locals for the grain: written through the mix buffers'
   pointers, the compiler has to assume every store could change the voice, and reloaded half of it after each
   sample.  (The envelope never leaves 0..ENV_MAX when it is used, so its level is a 32-bit number there.)
   The source samples of a plain voice come straight out of the block buffer; the block's end, the loop jump
   and the end of the sample go through next_sample().  "ended" is only ever set with cur == 28, so cur < 28
   is the whole test.  A send that adds zero is left out (eff_l and eff_r both zero), and g_send_dirty says
   whether any voice added to the send buffer this call. */
static int held_note(const Envelope *e) {
    return e->state == ST_SUSTAIN && e->rate[2] == 0 && e->type[2] <= CURVE_EXP_DEC && e->height > 0;
}

static int g_send_dirty;

#define FETCH_NEXT()                                                                   \
    do {                                                                               \
        prev = next;                                                                   \
        if (__builtin_expect(cur < 28, 1)) next = smp[cur++];                          \
        else { v->cur = cur; next = next_sample(v); cur = v->cur; }                    \
    } while (0)

static void voice_done(Voice *v) {
    if (!v->noise && v->ended) set_state(&v->env, ST_OFF), v->env.height = 0;
    if (v->env.state == ST_OFF) v->playing = v->on = 0;
}

/* A held note (see held_note(): its level cannot move during the grain). */
static void mix_voice_held(Voice *v, int32_t *mix, int32_t *send) {
    const int grain = g_grain, pitch = v->pitch, vol_l = v->vol_l, vol_r = v->vol_r, eff_l = v->eff_l, eff_r = v->eff_r;
    uint32_t frac = v->frac;
    int prev = v->prev, next = v->next, cur = v->cur;
    const int16_t *const smp = v->samples;
    const int64_t h = v->env.height > ENV_MAX ? ENV_MAX : v->env.height;
    const int env = (int)(((uint32_t)h + (1u << 14)) >> 15);
    int32_t *m = mix;
    if ((eff_l | eff_r) == 0) {
        for (int i = 0; i < grain; i++, m += 2) {
            int s = prev + (((next - prev) * (int)(frac & 0xFFF)) >> 12);
            frac += (uint32_t)pitch;
            while (frac >= PITCH_BASE) {
                frac -= PITCH_BASE;
                FETCH_NEXT();
            }
            s = (s * env + (1 << 14)) >> 15;
            m[0] += (s * vol_l) >> 12;
            m[1] += (s * vol_r) >> 12;
        }
    } else {
        int32_t *sd = send;
        g_send_dirty = 1;
        for (int i = 0; i < grain; i++, m += 2, sd += 2) {
            int s = prev + (((next - prev) * (int)(frac & 0xFFF)) >> 12);
            frac += (uint32_t)pitch;
            while (frac >= PITCH_BASE) {
                frac -= PITCH_BASE;
                FETCH_NEXT();
            }
            s = (s * env + (1 << 14)) >> 15;
            m[0] += (s * vol_l) >> 12;
            m[1] += (s * vol_r) >> 12;
            sd[0] += (s * eff_l) >> 12;
            sd[1] += (s * eff_r) >> 12;
        }
    }
    v->frac = frac;
    v->prev = (int16_t)prev;
    v->next = (int16_t)next;
    v->cur = cur;
    voice_done(v);
}

/* A voice whose envelope moves (attack, decay, release, a sustain with a rate). */
static void mix_voice_env(Voice *v, int32_t *mix, int32_t *send) {
    const int grain = g_grain, pitch = v->pitch, vol_l = v->vol_l, vol_r = v->vol_r, eff_l = v->eff_l, eff_r = v->eff_r;
    const int sends = (eff_l | eff_r) != 0;
    uint32_t frac = v->frac;
    int prev = v->prev, next = v->next, cur = v->cur;
    const int16_t *const smp = v->samples;
    Envelope e = v->env;
    if (sends) g_send_dirty = 1;
    for (int i = 0; i < grain; i++) {
        int s = prev + (((next - prev) * (int)(frac & 0xFFF)) >> 12);
        frac += (uint32_t)pitch;
        while (frac >= PITCH_BASE) {
            frac -= PITCH_BASE;
            FETCH_NEXT();
        }
        int64_t h = e.height > ENV_MAX ? ENV_MAX : e.height < 0 ? 0 : e.height;
        /* A held note: the sustain phase with a linear or exponential-decay curve of rate zero leaves the
           level where it is. */
        if (!held_note(&e)) env_step(&e);
        int env = (int)(((uint32_t)h + (1u << 14)) >> 15);
        s = (s * env + (1 << 14)) >> 15;
        mix[i * 2] += (s * vol_l) >> 12;
        mix[i * 2 + 1] += (s * vol_r) >> 12;
        if (sends) {
            send[i * 2] += (s * eff_l) >> 12;
            send[i * 2 + 1] += (s * eff_r) >> 12;
        }
    }
    v->frac = frac;
    v->prev = (int16_t)prev;
    v->next = (int16_t)next;
    v->cur = cur;
    v->env = e;
    voice_done(v);
}

/* A noise voice: every sample through next_sample(). */
static void mix_voice_noise(Voice *v, int32_t *mix, int32_t *send) {
    const int grain = g_grain, pitch = v->pitch, vol_l = v->vol_l, vol_r = v->vol_r, eff_l = v->eff_l, eff_r = v->eff_r;
    uint32_t frac = v->frac;
    int prev = v->prev, next = v->next;
    Envelope e = v->env;
    if (eff_l | eff_r) g_send_dirty = 1;
    for (int i = 0; i < grain; i++) {
        int s = prev + (((next - prev) * (int)(frac & 0xFFF)) >> 12);
        frac += (uint32_t)pitch;
        while (frac >= PITCH_BASE) {
            frac -= PITCH_BASE;
            prev = next;
            next = next_sample(v);
        }
        int64_t h = e.height > ENV_MAX ? ENV_MAX : e.height < 0 ? 0 : e.height;
        if (!held_note(&e)) env_step(&e);
        int env = (int)(((uint32_t)h + (1u << 14)) >> 15);
        s = (s * env + (1 << 14)) >> 15;
        mix[i * 2] += (s * vol_l) >> 12;
        mix[i * 2 + 1] += (s * vol_r) >> 12;
        send[i * 2] += (s * eff_l) >> 12;
        send[i * 2 + 1] += (s * eff_r) >> 12;
    }
    v->frac = frac;
    v->prev = (int16_t)prev;
    v->next = (int16_t)next;
    v->env = e;
    voice_done(v);
}

static void mix_voice(Voice *v) {
#ifdef TKDR_AUTOPILOT
    const int kind = !held_note(&v->env) ? 3 : (v->pitch == PITCH_BASE && v->frac == 0) ? 1 : 2;
    const uint32_t t0 = snd_tb();
    g_snd_sasn[kind]++;
    g_snd_sasn[4] += v->noise != 0;
    g_snd_sasn[5] += (uint64_t)g_grain;
    g_snd_sasn[7] += (v->eff_l | v->eff_r) != 0;
#endif
    if (v->noise) mix_voice_noise(v, g_mix, g_send);
    else if (held_note(&v->env)) mix_voice_held(v, g_mix, g_send);
    else mix_voice_env(v, g_mix, g_send);
    SND(g_snd_sas[kind] += snd_tb() - t0);
}

static Voice *voice(Ctx *c) {
    int n = (int)ARG(1);
    return n >= 0 && n < NVOICES ? &g_v[n] : NULL;
}
#define VOICE_OR_FAIL                                    \
    Voice *v = voice(c);                                 \
    if (!v) { RET(0x80420010u); return; }

// ---------------------------------------------------------------- HLE

void __sceSasInit(Ctx *c) {
    g_grain = (int)ARG(1);
    if (g_grain > 2048) g_grain = 2048;
    g_out_mode = (int)ARG(3);
    memset(g_v, 0, sizeof g_v);
    for (int i = 0; i < NVOICES; i++) {
        g_v[i].pitch = PITCH_BASE;
        g_v[i].vol_l = g_v[i].vol_r = g_v[i].eff_l = g_v[i].eff_r = 0x1000;
        g_v[i].noise_state = 0x1234 + (uint32_t)i;
        Envelope *e = &g_v[i].env;
        e->type[0] = CURVE_LIN_INC;
        e->type[1] = e->type[2] = e->type[3] = CURVE_LIN_DEC;
    }
    RET(0);
}

#ifdef HLE_TRACE
uint64_t g_sas_us;   // time spent mixing (stats)
#endif
#ifdef EXP_SASCHK
/* Debug: every call mixes a copy of the voices with the old code (sas_ref.h) into host buffers and compares the
   output and each voice with what the fast code did. */
uint32_t g_sas_checks, g_sas_bad, g_sas_cov[8];
static int32_t g_rmix[2048 * 2], g_rsend[2048 * 2];
static int16_t g_rout[2048 * 2];
static int voice_differs(const Voice *a, const Voice *b) {
    return a->vag_addr != b->vag_addr || a->vag_size != b->vag_size || a->read != b->read || a->loop_enabled != b->loop_enabled ||
           a->loop_next != b->loop_next || a->loop_block != b->loop_block || a->block != b->block || a->nblocks != b->nblocks ||
           a->ended != b->ended || memcmp(a->samples, b->samples, sizeof a->samples) || a->cur != b->cur || a->s1 != b->s1 ||
           a->s2 != b->s2 || a->noise != b->noise || a->noise_freq != b->noise_freq || a->noise_state != b->noise_state ||
           a->on != b->on || a->playing != b->playing || a->paused != b->paused || a->pitch != b->pitch || a->vol_l != b->vol_l ||
           a->vol_r != b->vol_r || a->eff_l != b->eff_l || a->eff_r != b->eff_r || a->frac != b->frac || a->prev != b->prev ||
           a->next != b->next || a->env.height != b->env.height || a->env.state != b->env.state ||
           memcmp(a->env.rate, b->env.rate, sizeof a->env.rate) || memcmp(a->env.type, b->env.type, sizeof a->env.type) ||
           a->env.sustain_level != b->env.sustain_level;
}
#endif
#ifdef EXP_ABSND
int g_ab_snd = 1;   /* 1: the fast mixer, 0: the old one (sas_ref.h); the frontend flips it at each report */
#endif

static void core_mix(uint32_t out, uint32_t in, int lvol, int rvol) {
#ifdef HLE_TRACE
    uint64_t t0 = hle_time_us();
#endif
#ifdef EXP_ABSND
    if (!g_ab_snd) {
#ifdef TKDR_AUTOPILOT
        const uint32_t ta = snd_tb();
        g_snd_sasn[0]++;
        for (int i = 0; i < NVOICES; i++)
            if (g_v[i].playing && !g_v[i].paused) g_snd_sasn[5] += (uint64_t)g_grain;
#endif
        ref_core_mix(g_v, g_mix, g_send, out, in, lvol, rvol, NULL);
        SND(g_snd_sas[0] += snd_tb() - ta);
        return;
    }
#endif
#ifdef EXP_SASCHK
    static Voice rv[NVOICES];
    memcpy(rv, g_v, sizeof g_v);
    for (int i = 0; i < NVOICES; i++) g_sas_cov[7] += rv[i].playing && !rv[i].paused && !rv[i].noise && held_note(&rv[i].env);
    ref_core_mix(rv, g_rmix, g_rsend, out, in, lvol, rvol, g_rout);
#endif
#ifdef TKDR_AUTOPILOT
    const uint32_t tc0 = snd_tb();
    g_snd_sasn[0]++;
#endif
    g_send_dirty = 0;
    for (int i = 0; i < NVOICES; i++)
        if (g_v[i].playing && !g_v[i].paused) mix_voice(&g_v[i]);
#ifdef TKDR_AUTOPILOT
    const uint32_t tc1 = snd_tb();
#endif
    const int frames = g_grain;
    const int wet = g_wet && g_send_dirty;
    uint8_t *const hout = in ? NULL : guest_host_ptr(out, (uint32_t)frames * 4);
    if (hout && g_dry && !wet) {
        /* The common case: the dry mix alone, saturated to 16 bits, four bytes a frame in the guest's order. */
        const int32_t *m = g_mix;
        for (int i = 0; i < frames; i++, m += 2) {
            const uint32_t l = (uint16_t)clamp16(m[0]), r = (uint16_t)clamp16(m[1]);
#ifdef __wii__
            const uint32_t w = ((uint32_t)__builtin_bswap16((uint16_t)l) << 16) | __builtin_bswap16((uint16_t)r);
#else
            const uint32_t w = l | (r << 16);
#endif
            memcpy(hout + (size_t)i * 4, &w, 4);
        }
    } else {
        for (int i = 0; i < frames * 2; i += 2) {
            int l = 0, r = 0;
            if (in) {
                l = ((int16_t)mem_r16(in + (uint32_t)i * 2) * lvol) >> 12;
                r = ((int16_t)mem_r16(in + (uint32_t)i * 2 + 2) * rvol) >> 12;
            }
            if (g_dry) { l += g_mix[i]; r += g_mix[i + 1]; }
            if (wet) { l += g_send[i] / 2; r += g_send[i + 1] / 2; }  // no reverb yet: plain send
            mem_w16(out + (uint32_t)i * 2, (uint16_t)clamp16(l));
            mem_w16(out + (uint32_t)i * 2 + 2, (uint16_t)clamp16(r));
        }
    }
    memset(g_mix, 0, sizeof(int32_t) * (size_t)frames * 2);
    if (g_send_dirty) memset(g_send, 0, sizeof(int32_t) * (size_t)frames * 2);
#ifdef TKDR_AUTOPILOT
    {
        const uint32_t tc2 = snd_tb();
        g_snd_sas[4] += tc2 - tc1;
        g_snd_sas[0] += tc2 - tc0;
    }
#endif
#ifdef EXP_SASCHK
    {
        int bad = 0;
        g_sas_checks++;
        for (int i = 0; i < frames * 2 && !bad; i++)
            if ((int16_t)mem_r16(out + (uint32_t)i * 2) != g_rout[i]) bad = 1;
        for (int i = 0; i < NVOICES; i++) {
            if (voice_differs(&rv[i], &g_v[i])) bad = 1;
            g_sas_cov[0] += rv[i].playing != 0;
            g_sas_cov[1] += g_v[i].noise != 0 && g_v[i].playing;
            g_sas_cov[2] += g_v[i].ended != 0;
            g_sas_cov[3] += (g_v[i].eff_l | g_v[i].eff_r) != 0 && g_v[i].playing;
        }
        g_sas_cov[4] += in != 0;
        g_sas_cov[5] += g_wet && g_send_dirty;
        g_sas_cov[6] += !g_dry;
        if (bad && ++g_sas_bad <= 8) fprintf(stderr, "SASCHK MISMATCH call %u\n", g_sas_checks);
        if ((g_sas_checks & 0xFF) == 1 || (bad && g_sas_bad <= 8))
            fprintf(stderr, "SASCHK calls %u bad %u (playing voice-calls %u, noise %u, ended %u, with sends %u, WithMix calls %u, wet %u, dry off %u)\n", g_sas_checks,
                    g_sas_bad, g_sas_cov[0], g_sas_cov[1], g_sas_cov[2], g_sas_cov[3], g_sas_cov[4], g_sas_cov[5], g_sas_cov[6]);
    }
#endif
#ifdef HLE_TRACE
    g_sas_us += hle_time_us() - t0;
#endif
}

#ifdef TKDR_AUTOPILOT
extern uint64_t g_snd_at[4], g_snd_atn[4], g_snd_ring[2], g_snd_ringn[2], g_audio_pull_us;
/* The SOUND lines: the mixer, the AT3 decoder, the ring mix and the feeder, per guest frame, since the last report. */
void sound_prof_report(uint32_t gf) {
    static uint64_t ls[8], lsn[8], la[4], lan[4], lr[2], lrn[2], lpull;
    if (!gf) gf = 1;
    const double ms = 1.0 / 60750.0 / gf, per = 1.0 / gf;
    uint64_t s[8], sn[8], a[4], an[4];
    for (int i = 0; i < 8; i++) { s[i] = g_snd_sas[i] - ls[i]; sn[i] = g_snd_sasn[i] - lsn[i]; ls[i] = g_snd_sas[i]; lsn[i] = g_snd_sasn[i]; }
    for (int i = 0; i < 4; i++) { a[i] = g_snd_at[i] - la[i]; an[i] = g_snd_atn[i] - lan[i]; la[i] = g_snd_at[i]; lan[i] = g_snd_atn[i]; }
    fprintf(stderr, "  SOUND sas: %.2f calls %.3f ms (voices mixed: held@rate %.1f held-resampled %.1f envelope-moving %.1f, %.1f noise, %.1f with sends; "
            "%.0f voice-samples, grain %d); vag %.1f blocks %.3f ms; output loop %.3f ms\n",
            sn[0] * per, s[0] * ms, sn[1] * per, sn[2] * per, sn[3] * per, sn[4] * per, sn[7] * per, sn[5] * per, g_grain, sn[6] * per, s[5] * ms, s[4] * ms);
    fprintf(stderr, "  SOUND sas per voice-sample (ns, vag decode included): held@rate %.1f held-resampled %.1f envelope-moving %.1f; vag block %.0f ns\n",
            sn[1] ? (double)s[1] * 1000.0 / 60.75 / ((double)sn[1] * g_grain) : 0.0, sn[2] ? (double)s[2] * 1000.0 / 60.75 / ((double)sn[2] * g_grain) : 0.0,
            sn[3] ? (double)s[3] * 1000.0 / 60.75 / ((double)sn[3] * g_grain) : 0.0, sn[6] ? (double)s[5] * 1000.0 / 60.75 / (double)sn[6] : 0.0);
    fprintf(stderr, "  SOUND atrac: %.2f decodes (%.2f of them AT3+, %.2f decoder frames) %.3f ms decoding, %.3f ms the whole call; %.0f us a decoder frame; %.1f samples out\n",
            an[0] * per, an[1] * per, an[3] * per, a[1] * ms, a[0] * ms, an[3] ? (double)a[1] / 60.75 / (double)an[3] : 0.0, an[2] * per);
    uint64_t r[2], rn[2];
    for (int i = 0; i < 2; i++) { r[i] = g_snd_ring[i] - lr[i]; rn[i] = g_snd_ringn[i] - lrn[i]; lr[i] = g_snd_ring[i]; lrn[i] = g_snd_ringn[i]; }
    fprintf(stderr, "  SOUND ring: %.2f outputs %.1f samples, mixed into the ring in %.3f ms; the feeder's stream %.3f ms (all per guest frame)\n",
            rn[0] * per, rn[1] * per, r[0] * ms, (double)(g_audio_pull_us - lpull) / 1e3 / gf);
    lpull = g_audio_pull_us;
}
#endif

void __sceSasCore(Ctx *c) {
    core_mix(ARG(1), 0, 0, 0);
    RET(0);
}
void __sceSasCoreWithMix(Ctx *c) {
    core_mix(ARG(1), ARG(1), (int)ARG(2), (int)ARG(3));
    RET(0);
}

void __sceSasGetEndFlag(Ctx *c) {
    uint32_t f = 0;
    for (int i = 0; i < NVOICES; i++)
        if (!g_v[i].playing) f |= 1u << i;
    RET(f);
}

void __sceSasSetVoice(Ctx *c) {
    VOICE_OR_FAIL
    v->vag_addr = ARG(2);
    v->vag_size = ARG(3);
    v->loop_enabled = (int)ARG(4);
    v->noise = 0;
    RET(0);
}

void __sceSasSetNoise(Ctx *c) {
    VOICE_OR_FAIL
    v->noise = 1;
    v->noise_freq = (int)ARG(2);
    RET(0);
}

void __sceSasSetVolume(Ctx *c) {
    VOICE_OR_FAIL
    v->vol_l = (int)ARG(2);
    v->vol_r = (int)ARG(3);
    v->eff_l = (int)ARG(4);
    v->eff_r = (int)ARG(5);
    RET(0);
}

void __sceSasSetPitch(Ctx *c) {
    VOICE_OR_FAIL
    v->pitch = (int)ARG(2) & 0x7FFF;
    RET(0);
}

void __sceSasSetKeyOn(Ctx *c) {
    VOICE_OR_FAIL
    if (!v->noise) vag_start(v);
    v->frac = 0;
    v->prev = v->next = 0;
    v->on = v->playing = 1;
    v->paused = 0;
    set_state(&v->env, ST_KEYON);
    RET(0);
}

void __sceSasSetKeyOff(Ctx *c) {
    VOICE_OR_FAIL
    if (v->playing) set_state(&v->env, ST_RELEASE);
    RET(0);
}

void __sceSasSetPause(Ctx *c) {
    for (int i = 0; i < NVOICES; i++)
        if (ARG(1) & (1u << i)) g_v[i].paused = (int)ARG(2);
    RET(0);
}
void __sceSasGetPauseFlag(Ctx *c) {
    uint32_t f = 0;
    for (int i = 0; i < NVOICES; i++)
        if (g_v[i].paused) f |= 1u << i;
    RET(f);
}

// (core, voice, flag, a, d, s, r): rates
void __sceSasSetADSR(Ctx *c) {
    VOICE_OR_FAIL
    for (int k = 0; k < 4; k++)
        if (ARG(2) & (1u << k)) v->env.rate[k] = (int)ARG(3 + k);
    RET(0);
}
// (core, voice, flag, a, d, s, r): curve types
void __sceSasSetADSRmode(Ctx *c) {
    VOICE_OR_FAIL
    for (int k = 0; k < 4; k++)
        if (ARG(2) & (1u << k)) v->env.type[k] = (int)ARG(3 + k);
    RET(0);
}
void __sceSasSetSL(Ctx *c) {
    VOICE_OR_FAIL
    v->env.sustain_level = (int64_t)ARG(2);
    RET(0);
}
void __sceSasGetEnvelopeHeight(Ctx *c) {
    VOICE_OR_FAIL
    RET((uint32_t)(v->env.height > ENV_MAX ? ENV_MAX : v->env.height < 0 ? 0 : v->env.height));
}

static int simple_rate(int n, int shift) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    int r = ((7 - (n & 3)) << shift) >> (n >> 2);
    return r ? r : 1;
}

// The packed ADSR format used by most games.
void __sceSasSetSimpleADSR(Ctx *c) {
    VOICE_OR_FAIL
    uint32_t e1 = ARG(2), e2 = ARG(3);
    Envelope *e = &v->env;
    e->rate[0] = simple_rate((int)(e1 >> 8), 26);
    e->type[0] = (e1 & 0x8000) ? CURVE_LIN_BENT : CURVE_LIN_INC;
    int dn = (int)((e1 >> 4) & 15);
    e->rate[1] = dn ? (int)(0x80000000u >> dn) : 0x7FFFFFFF;
    e->type[1] = CURVE_EXP_DEC;
    int st = (int)((e2 >> 14) & 3);
    e->type[2] = st;
    e->rate[2] = st == CURVE_EXP_DEC ? simple_rate((int)(e2 >> 6), 24) : simple_rate((int)(e2 >> 6), 26);
    e->type[3] = (e2 & 0x20) ? CURVE_EXP_DEC : CURVE_LIN_DEC;
    int rn = (int)(e2 & 31);
    if (rn == 31) e->rate[3] = 0;
    else if (e->type[3] == CURVE_LIN_DEC) e->rate[3] = rn == 30 ? 0x40000000 : rn == 29 ? 1 : (0x10000000 >> rn);
    else e->rate[3] = rn ? (int)(0x80000000u >> rn) : 0x7FFFFFFF;
    e->sustain_level = (int64_t)(((e1 & 15) + 1) << 26);
    RET(0);
}

// Reverb is not emulated: wet output is the plain effect send.
void __sceSasRevType(Ctx *c) { RET(0); }
void __sceSasRevParam(Ctx *c) { RET(0); }
void __sceSasRevEVOL(Ctx *c) { RET(0); }
void __sceSasRevVON(Ctx *c) {
    g_dry = ARG(1) != 0;
    g_wet = ARG(2) != 0;
    RET(0);
}
