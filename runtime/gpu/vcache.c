/* A cache of decoded draws for the GX path.

   A fight draws the stage and the props with the same vertices, the same
   lighting and (in object space) the same everything every frame, and
   decoding them was the largest cost of the frame.  A draw that is not
   skinned or morphed depends only on:

     - its vertex bytes and the vertex type,
     - the world matrix, unless the vertices are kept in object space,
     - the lighting and material registers,

   (the texture coordinate scale and offset are applied when the vertices are
   sent, so a scrolling texture does not defeat the cache; and the chip does
   fog and the view and projection itself).  The cache keeps the vertices in
   the form the chip is sent, keyed by the draw's address and hashes of all
   of the above; a hit skips the decoder and the conversion.

   It is four-way set associative.  A draw's vertices live in an arena in
   blocks of the draw's own size; a slot that is given up hands its block to
   the list of blocks of that size, so the arena does not leak as draws are
   replaced.  When it is full, the slots that have not been used for a while
   are given up and the rest slid together (vc_collect): a fight after a fight
   brings other draws, and the first one's would otherwise keep the arena for
   good.  A draw that still finds no room is not cached. */
#include <stdint.h>
#include <string.h>

#include "gpu.h"
#include "recomp.h"
#include "../bigmem.h"

#ifdef __wii__

#define VC_SET_BITS 12                 /* 4096 sets of four slots, picked by the address (2048 lost 3-9% of the lookups
                                          of the heaviest scenes to sets with more than four draws: tools/perf/vcsim.py) */
#define VC_SETS (1u << VC_SET_BITS)
#define VC_WAYS 4
#ifndef VC_ARENA_BYTES
#define VC_ARENA_BYTES (1024u << 10)   /* the vertices of every cached draw (a smaller one makes the collector run, for tests) */
#endif
#define VC_SLOT_BITS (VC_SET_BITS + 2) /* a slot's index */
_Static_assert(((VC_ARENA_BYTES - 1) >> 2) < (1u << (32 - VC_SLOT_BITS)), "vc_collect packs a word offset over a slot index in 32 bits");
#define VC_MAX 32                      /* the longest draw held (VC_MAX_VERTS in raster.c) */

/* A slot is half a cache line and a set is four lines, aligned: a lookup
   touches four lines and no slot straddles two. */
typedef struct {
    uint32_t vbase;
    uint32_t key;                     /* the vertex type (bits 0-23 and 31) and the vertex count << 24; 0: empty */
    uint32_t hd, hw, hr;              /* hashes of the vertex bytes, the world matrix, the material and lights */
    uint32_t off;                     /* of the vertices in the arena */
    uint32_t age;                     /* when it was last used */
    uint32_t gax;                     /* 0x100 | the global ambient alpha the draw was made under, when it can be made again for another (hr has none then); else 0 */
} VcSlot;

#define KEY_OF(vt_, count_) (((vt_) & 0x80FFFFFFu) | ((uint32_t)(count_) << 24))
#define KEY_COUNT(key_) ((key_) >> 24 & 0x3Fu)

static VcSlot *s_slots;               /* VC_SETS * VC_WAYS, 128-byte aligned */
static uint8_t *s_arena;
static uint32_t s_used;               /* the arena handed out so far */
static uint32_t s_free[VC_MAX + 1];   /* per draw size, the first returned block + 1 (0: none) */
static uint32_t s_clock;
static uint32_t s_gc_clock;           /* s_clock at the last collection */
static uint32_t s_free_bytes;         /* in the free lists */
static int s_failed;
static GxVtx s_scratch[VC_MAX];       /* where a draw goes that could not be cached */
uint32_t g_vc_hits, g_vc_misses, g_vc_resets;
uint32_t g_vc_refresh;   /* hits that differed only by the global ambient alpha (counted in g_vc_hits as well) */
#ifdef EXP_ABREFRESH
int g_ab_refresh = 1;    /* A/B runs: draws whose only change is that alpha are cached as hits, or miss as before */
#endif
uint32_t g_vc_gc;   /* collections of the arena */
#ifdef TKDR_AUTOPILOT
uint32_t g_vc_full;   /* draws that could not be cached because the arena had no block for them (demo statistics) */
uint32_t vc_arena_used(void) { return s_used; }
uint32_t vc_arena_size(void) { return VC_ARENA_BYTES; }
/* Why the lookups missed: 0 no slot for this address, 1 slots for it but with other vertex bytes, 2 a slot with the
   same bytes (the same draw under another world matrix or lighting); and for the last kind, how many slots of the
   set held this address. */
uint32_t g_vc_cls[3], g_vc_occ[5];
#endif
#ifdef TKDR_AUTOPILOT
uint64_t g_vc_ticks[2];   /* hashing the draw, searching the set (TKDR_FINEPROF) */
#endif
#ifdef TKDR_FINEPROF
static inline uint32_t vc_tb(void) { uint32_t t; __asm__ volatile("mftb %0" : "=r"(t)); return t; }
uint64_t g_vc_hbytes, g_vc_hlook, g_vc_hbytes_hit, g_vc_hlook_hit, g_vc_hticks_hit;   /* every lookup's hashed bytes and count; those of hits, and their hash ticks */
#endif
uint32_t g_vc_addr_match, g_vc_data_diff, g_vc_world_diff, g_vc_state_diff, g_vc_light_diff;

#ifdef EXP_VCDUMP
/* The last 16384 lookups -- address, key, the three hashes, what the cache made of it (res: 0 hit, 1 no slot for the
   address, 2 other vertex bytes, 3 the same bytes under other state) and, for a miss, the slot its store gave up
   (the victim's address, key and age in lookups, and its kind: 1 empty, 2 the same address, 3 used within 12000
   lookups, 4 older) -- printed on request with the whole slot table, for tools/perf/vcsim.py. */
#include <stdio.h>
#define VC_RING 16384u
typedef struct { uint32_t vbase, key, hd, hw, hr, res, vv, vk, vage, vcls; } VcRec;
static VcRec *s_ring, *s_cur;
static volatile uint32_t s_ring_n, s_ring_hold;

void vc_dump_ring(void) {
    if (!s_ring || !s_slots) return;
    s_ring_hold = 1;
    const uint32_t n = s_ring_n;
    fprintf(stderr, "VCDUMP begin %u clock %u\n", n, s_clock);
    for (uint32_t i = 0; i < VC_RING; i += 4) {
        char line[256];
        int len = 0;
        for (uint32_t j = 0; j < 4; j++) {
            const VcRec *r = &s_ring[(n + i + j) & (VC_RING - 1)];
            len += snprintf(line + len, sizeof line - (size_t)len, "%x %x %x %x %x %x|", r->vbase, r->key, r->hd, r->hw, r->hr, r->res);
        }
        fprintf(stderr, "VCR %s\n", line);
    }
    for (uint32_t i = 0; i < VC_RING; i++) {
        const VcRec *r = &s_ring[(n + i) & (VC_RING - 1)];
        if (r->res) fprintf(stderr, "VCV %u %x %x %u %u\n", i, r->vv, r->vk, r->vage, r->vcls);
    }
    for (uint32_t i = 0; i < VC_SETS; i++) {
        const VcSlot *st = &s_slots[i * VC_WAYS];
        if (!st[0].key && !st[1].key && !st[2].key && !st[3].key) continue;
        fprintf(stderr, "VCT %u %x %x %u|%x %x %u|%x %x %u|%x %x %u\n", i, st[0].vbase, st[0].key, s_clock - st[0].age, st[1].vbase,
                st[1].key, s_clock - st[1].age, st[2].vbase, st[2].key, s_clock - st[2].age, st[3].vbase, st[3].key, s_clock - st[3].age);
    }
    fprintf(stderr, "VCDUMP end\n");
    s_ring_hold = 0;
}
#endif

static inline uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

#ifdef EXP_VCWHY
#include <stdio.h>
static inline uint32_t mix(uint32_t h, uint32_t w);
uint32_t ge_light_sig_flat_noalpha(void);
uint32_t g_vc_why[2];   /* misses of a draw whose bytes and matrix are cached: only the global ambient alpha differs / something else */
static uint32_t *s_why_alpha, *s_why_hrn;   /* per slot: the global ambient alpha and the lighting key without it, when it was stored */
static uint32_t why_hrn(void) {
    const uint32_t *r = g_ge.cmd;
    uint32_t y = 0x27D4EB2Fu;
    y = mix(y, r[GE_MATERIALAMBIENT]); y = mix(y, r[GE_MATERIALALPHA]);
    y = mix(y, r[GE_LIGHTINGENABLE]); y = mix(y, r[GE_MATERIALUPDATE]);
    return mix(y, ge_light_sig_flat_noalpha());
}
/* A draw whose vertex bytes and world matrix are those of a cached slot but whose lighting signature is not: the
   registers the signature is made of, for the first few such misses and then every 4001st (read them over time to
   see which one moves). */
static void vc_why(uint32_t vbase, uint32_t hr, uint32_t was) {
    static uint32_t n_all, n_seen;
    n_seen++;
    if (n_all >= 8 && (n_seen % 4001 != 0 || n_all > 120)) return;
    n_all++;
    const uint32_t *r = g_ge.cmd;
    uint32_t y = 0x27D4EB2Fu;   /* as vc_regs() works out hr: which signature went into this one? */
    y = mix(y, r[GE_MATERIALAMBIENT]); y = mix(y, r[GE_MATERIALALPHA]);
    y = mix(y, r[GE_LIGHTINGENABLE]); y = mix(y, r[GE_MATERIALUPDATE]);
    fprintf(stderr, "VCWHY used the %s signature (flat would give %08X, full %08X)\n",
            mix(y, ge_light_sig()) == hr ? "full" : mix(y, ge_light_sig_flat()) == hr ? "flat" : "neither", mix(y, ge_light_sig_flat()), mix(y, ge_light_sig()));
    fprintf(stderr, "VCWHY %08X hr %08X was %08X (full sig %08X flat %08X): lighting %X update %X emissive %06X ambient %06X alpha %X "
            "global ambient %06X/%X mode %X | light enable/type/ambient: %X/%X/%06X %X/%X/%06X %X/%X/%06X %X/%X/%06X | light0 pos %X %X %X dir %X %X %X\n",
            vbase, hr, was, ge_light_sig(), ge_light_sig_flat(), r[GE_LIGHTINGENABLE], r[GE_MATERIALUPDATE], r[GE_MATERIALEMISSIVE],
            r[GE_MATERIALAMBIENT], r[GE_MATERIALALPHA], r[GE_AMBIENTCOLOR], r[GE_AMBIENTALPHA], r[GE_LIGHTMODE],
            r[GE_LIGHTENABLE0], r[GE_LIGHTTYPE0], r[GE_LAC0], r[GE_LIGHTENABLE0 + 1], r[GE_LIGHTTYPE0 + 1], r[GE_LAC0 + 3],
            r[GE_LIGHTENABLE0 + 2], r[GE_LIGHTTYPE0 + 2], r[GE_LAC0 + 6], r[GE_LIGHTENABLE0 + 3], r[GE_LIGHTTYPE0 + 3], r[GE_LAC0 + 9],
            r[GE_LX0], r[GE_LX0 + 1], r[GE_LX0 + 2], r[GE_LDX0], r[GE_LDX0 + 1], r[GE_LDX0 + 2]);
}
#endif

static int vc_init(void) {
    if (s_slots) return 1;
    if (s_failed) return 0;
    /* big_alloc blocks are 32-byte aligned: take a little more to align to the set */
    uint8_t *raw = big_alloc(VC_SETS * VC_WAYS * sizeof(VcSlot) + 128);
    s_arena = big_alloc(VC_ARENA_BYTES);
    if (!raw || !s_arena) {
        if (raw) big_free(raw);
        if (s_arena) big_free(s_arena);
        s_arena = NULL;
        s_failed = 1;
        return 0;
    }
    s_slots = (VcSlot *)(((uintptr_t)raw + 127u) & ~(uintptr_t)127u);
    memset(s_slots, 0, VC_SETS * VC_WAYS * sizeof(VcSlot));
    return 1;
}

void vc_reset(void) {
    if (!s_slots) return;
    memset(s_slots, 0, VC_SETS * VC_WAYS * sizeof(VcSlot));
    memset(s_free, 0, sizeof s_free);
    s_free_bytes = 0;
    s_used = 0;
    g_vc_resets++;
}

/* No multiplication: a multiply is what a hash costs on this CPU. */
static inline uint32_t mix(uint32_t h, uint32_t w) { return rotl(h, 5) + (h ^ w) + w; }

/* The vertex bytes: three running sums, each of the one before. */
static inline uint32_t data_hash(const uint8_t *vp, uint32_t bytes) {
    uint32_t s1 = 0x85EBCA6Bu ^ bytes, s2 = 0, s3 = 0;
    const uint32_t *w = (const uint32_t *)vp;
    uint32_t n = bytes >> 2;
    for (uint32_t i = 0; i < n; i++) { s1 += w[i]; s2 += s1; s3 += s2; }
    for (uint32_t i = n << 2; i < bytes; i++) { s1 += vp[i]; s2 += s1; s3 += s2; }
    return s1 ^ rotl(s2, 11) ^ rotl(s3, 21);
}

/* What a plain draw's vertices depend on besides their bytes.  Worked out once
   for a run of draws that share the register state. */
void vc_regs(VcRegs *o, int objspace, int normals, int refresh_ok) {
    uint32_t x = 0xC2B2AE35u;
    if (!objspace) {   /* vertices left in object space do not depend on the world matrix */
        const uint32_t *m = (const uint32_t *)g_ge.world;
        for (int i = 0; i < 12; i++) x = mix(x, m[i]);
    }
    o->hw = x;
    const uint32_t *r = g_ge.cmd;
    uint32_t y = 0x27D4EB2Fu;
    y = mix(y, r[GE_MATERIALAMBIENT]); y = mix(y, r[GE_MATERIALALPHA]);
    y = mix(y, r[GE_LIGHTINGENABLE]); y = mix(y, r[GE_MATERIALUPDATE]);
    o->gax = 0;
    if (normals) {
        o->hr = mix(y, ge_light_sig());
        return;
    }
    /* Without normals the global ambient alpha reaches a vertex only as its alpha byte: the alpha of the material (or of
       the vertex) times it, when lighting is on, and not at all when it is off.  A draw that can be given the alpha again
       keeps it out of the hash and beside it (gax: kept whole, since this hash mixes a lone small number badly: only the
       bits where the other side has zeros count), the others hash it with the rest. */
    const int lit = (r[GE_LIGHTINGENABLE] & 1) != 0;
#ifdef EXP_ABREFRESH
    if (!g_ab_refresh) {   /* the hash as it was */
        o->hr = mix(y, ge_light_sig_flat());
        return;
    }
#endif
    if (!lit || refresh_ok) y = mix(y, ge_light_sig_flat_noalpha());
    else y = mix(y, ge_light_sig_flat());
    o->hr = y;
    if (lit && refresh_ok) o->gax = 0x100u | (r[GE_AMBIENTALPHA] & 0xFFu);
}

static inline VcSlot *set_of(uint32_t vbase) {
    return &s_slots[(((vbase >> 2) * 2654435761u) >> (32 - VC_SET_BITS)) * VC_WAYS];
}

/* A hint: the draw at this address is probably next.  Its cache set and its
   first vertices are fetched while the list is still being read. */
void vc_prefetch(uint32_t vbase) {
    if (!s_slots) return;
    const char *set = (const char *)set_of(vbase);
    __builtin_prefetch(set);
    __builtin_prefetch(set + 32);
    __builtin_prefetch(set + 64);
    __builtin_prefetch(set + 96);
}

/* The cached vertices of this draw, or NULL.  `tok` carries the key to
   vc_store() after a miss. */
const GxVtx *vc_lookup(const uint8_t *vp, uint32_t vbase, int count, int size, uint32_t vt, int normals, int objspace,
                       int refresh_ok, VcTok *tok) {
    VcRegs r;
    vc_regs(&r, objspace, normals, refresh_ok);
    return vc_lookup2(vp, vbase, count, size, vt, &r, tok);
}

const GxVtx *vc_lookup2(const uint8_t *vp, uint32_t vbase, int count, int size, uint32_t vt, const VcRegs *regs,
                        VcTok *tok) {
    if (!vc_init()) return NULL;
#ifdef TKDR_FINEPROF
    uint32_t t0 = vc_tb();
#endif
    const uint32_t hd = data_hash(vp, (uint32_t)(size * count)), hw = regs->hw, hr = regs->hr;
#ifdef TKDR_FINEPROF
    uint32_t t1 = vc_tb();
    g_vc_ticks[0] += (uint32_t)(t1 - t0);
    g_vc_hbytes += (uint32_t)(size * count);
    g_vc_hlook++;
#endif
    const uint32_t key = KEY_OF(vt, count);
#ifdef EXP_VCDUMP
    s_cur = NULL;
    if (!s_ring_hold) {
        if (!s_ring) s_ring = big_alloc(VC_RING * sizeof(VcRec));
        if (s_ring) {
            VcRec *r = &s_ring[s_ring_n++ & (VC_RING - 1)];
            r->vbase = vbase; r->key = key; r->hd = hd; r->hw = hw; r->hr = hr; r->res = r->vv = r->vk = r->vage = r->vcls = 0;
            s_cur = r;
        }
    }
#endif
    VcSlot *set = set_of(vbase);
    const VcSlot *near = NULL;
    tok->refresh = 0;
    for (int k = 0; k < VC_WAYS; k++) {
        VcSlot *s = &set[k];
        if (s->key != key || s->vbase != vbase) continue;
        near = s;
        if (s->hd == hd && s->hw == hw && s->hr == hr && s->gax == regs->gax) {
            s->age = ++s_clock;
            g_vc_hits++;
#ifdef TKDR_FINEPROF
            g_vc_ticks[1] += (uint32_t)(vc_tb() - t1);
            g_vc_hbytes_hit += (uint32_t)(size * count);
            g_vc_hlook_hit++;
            g_vc_hticks_hit += (uint32_t)(t1 - t0);
#endif
            return (const GxVtx *)(s_arena + s->off);
        }
    }
    if (regs->gax) {
        /* The draw as cached but for the global ambient alpha it was stored under: the caller makes the alpha bytes again
           (tok->refresh) and the slot now stands for this alpha. */
        for (int k = 0; k < VC_WAYS; k++) {
            VcSlot *s = &set[k];
            if (s->key != key || s->vbase != vbase || s->hd != hd || s->hw != hw || s->hr != hr || !s->gax) continue;
            s->gax = regs->gax;
            s->age = ++s_clock;
            g_vc_hits++;
            g_vc_refresh++;
            tok->refresh = 1;
            tok->ga = regs->gax & 0xFFu;
            return (const GxVtx *)(s_arena + s->off);
        }
    }
#ifdef TKDR_AUTOPILOT
    {
        int same = 0, var = 0;
        for (int k = 0; k < VC_WAYS; k++)
            if (set[k].key == key && set[k].vbase == vbase) {
                same++;
                if (set[k].hd == hd) var = 1;
            }
        g_vc_cls[same ? (var ? 2 : 1) : 0]++;
#ifdef EXP_VCDUMP
        if (s_cur) s_cur->res = same ? (var ? 3 : 2) : 1;
#endif
        if (var) g_vc_occ[same]++;
    }
#endif
    if (near) {   /* diagnostics: what changed since this address was last drawn */
        g_vc_addr_match++;
        if (near->hd != hd) g_vc_data_diff++;
        if (near->hw != hw) g_vc_world_diff++;
        if (near->hr != hr) g_vc_light_diff++;
#ifdef EXP_VCWHY
        if (near->hd == hd && near->hw == hw && near->hr != hr) {
            const size_t idx = (size_t)(near - s_slots);
            if (s_why_hrn && s_why_hrn[idx] == why_hrn() && s_why_alpha[idx] != g_ge.cmd[GE_AMBIENTALPHA]) g_vc_why[0]++;
            else g_vc_why[1]++;
            vc_why(vbase, hr, near->hr);
        }
#endif
    }
    g_vc_misses++;
    tok->vbase = vbase;
    tok->key = key;
    tok->hd = hd;
    tok->hw = hw;
    tok->hr = hr;
    tok->gax = regs->gax;
    return NULL;
}

#define VC_GC_GAP 4096u        /* lookups between two collections */
#define VC_STALE0 49152u       /* a slot idle for this many lookups is given up (a fight frame makes ~4,000) */
#define VC_WANT (VC_ARENA_BYTES / 16)   /* a collection looks for at least this much */

/* The arena is full and no block of the wanted size is free: give up the slots that have not been used for a
   while (the horizon shortens until a sixteenth of the arena is freed) and slide the others down in the order
   they lie, so that all the free room is one run at the end again.  Returns 0 when there was nothing to gain.
   The draw being stored has not got a block yet and nobody holds a pointer into the arena. */
static int vc_collect(void) {
    static uint32_t *order, *tmp;
    s_gc_clock = s_clock;
    if (!order) {
        order = big_alloc(2 * VC_SETS * VC_WAYS * sizeof(uint32_t));
        if (!order) return 0;
        tmp = order + VC_SETS * VC_WAYS;
    }
    uint32_t horizon = VC_STALE0, freed = s_free_bytes;
    for (;; horizon >>= 1) {
        uint32_t dead = s_free_bytes;
        for (uint32_t i = 0; i < VC_SETS * VC_WAYS; i++) {
            const VcSlot *sl = &s_slots[i];
            if (sl->key && (uint32_t)(s_clock - sl->age) > horizon) dead += KEY_COUNT(sl->key) * (uint32_t)sizeof(GxVtx);
        }
        freed = dead;
        if (dead >= VC_WANT || horizon <= 512) break;
    }
    if (freed < VC_WANT / 4) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < VC_SETS * VC_WAYS; i++) {
        VcSlot *sl = &s_slots[i];
        if (!sl->key) continue;
        if ((uint32_t)(s_clock - sl->age) > horizon) { sl->key = 0; continue; }
        order[n++] = (sl->off >> 2) << VC_SLOT_BITS | i;   /* the offset in words over the slot index: 32 bits */
    }
    /* by offset: two counting passes of nine bits */
    for (int pass = 0; pass < 2; pass++) {
        const int sh = VC_SLOT_BITS + 9 * pass;
        uint32_t cnt[513] = {0};
        for (uint32_t k = 0; k < n; k++) cnt[((order[k] >> sh) & 511u) + 1]++;
        for (int b = 0; b < 512; b++) cnt[b + 1] += cnt[b];
        for (uint32_t k = 0; k < n; k++) tmp[cnt[(order[k] >> sh) & 511u]++] = order[k];
        memcpy(order, tmp, n * sizeof order[0]);
    }
    uint32_t top = 0;
    for (uint32_t k = 0; k < n; k++) {
        VcSlot *sl = &s_slots[order[k] & (VC_SETS * VC_WAYS - 1)];
        const uint32_t len = KEY_COUNT(sl->key) * (uint32_t)sizeof(GxVtx);
        if (sl->off != top) memmove(s_arena + top, s_arena + sl->off, len);
        sl->off = top;
        top += len;
    }
    s_used = top;
    memset(s_free, 0, sizeof s_free);
    s_free_bytes = 0;
    g_vc_gc++;
    return 1;
}

/* Room for the vertices of the draw vc_lookup() just missed.  The slot given
   up is the one with the same address if there is one (the draw changed),
   else an empty one, else the least recently used.  (Keeping a same-address
   slot of the same bytes, the draw under another matrix or lighting, as a
   variant was measured with an A/B run and changed nothing.) */
GxVtx *vc_store(const VcTok *tok) {
    const uint32_t count = KEY_COUNT(tok->key), bytes = count * (uint32_t)sizeof(GxVtx);
    VcSlot *set = set_of(tok->vbase), *v = NULL;
    for (int k = 0; k < VC_WAYS; k++) {
        VcSlot *s = &set[k];
        if (s->key == tok->key && s->vbase == tok->vbase) { v = s; break; }
    }
    for (int k = 0; k < VC_WAYS && !v; k++)
        if (!set[k].key) v = &set[k];
    if (!v) {   /* the least recently used */
        v = &set[0];
        for (int k = 1; k < VC_WAYS; k++)
            if ((int32_t)(set[k].age - v->age) < 0) v = &set[k];
    }
#ifdef EXP_VCDUMP
    if (s_cur && s_cur->vbase == tok->vbase) {
        s_cur->vv = v->vbase; s_cur->vk = v->key; s_cur->vage = s_clock - v->age;
        s_cur->vcls = !v->key ? 1 : (v->key == tok->key && v->vbase == tok->vbase) ? 2 : (uint32_t)(s_clock - v->age) < 12000u ? 3 : 4;
    }
#endif
    if (KEY_COUNT(v->key) != count) {
        if (v->key) {   /* its block goes back to the draws of that size */
            *(uint32_t *)(s_arena + v->off) = s_free[KEY_COUNT(v->key)];
            s_free[KEY_COUNT(v->key)] = v->off + 1;
            s_free_bytes += KEY_COUNT(v->key) * (uint32_t)sizeof(GxVtx);
            v->key = 0;
        }
        uint32_t off;
        for (int again = 0;; again++) {
            if (s_free[count]) {
                off = s_free[count] - 1;
                s_free[count] = *(uint32_t *)(s_arena + off);
                s_free_bytes -= bytes;
            } else if (s_used + bytes <= VC_ARENA_BYTES) {
                off = s_used;
                s_used += bytes;
            } else if (!again && (uint32_t)(s_clock - s_gc_clock) >= VC_GC_GAP && vc_collect()) {
                continue;
            } else {
#ifdef TKDR_AUTOPILOT
                g_vc_full++;
#endif
                return s_scratch;   /* full: this draw is not cached */
            }
            break;
        }
        v->off = off;
    }
    v->vbase = tok->vbase;
    v->key = tok->key;
    v->hd = tok->hd;
    v->hw = tok->hw;
    v->hr = tok->hr;
    v->gax = tok->gax;
    v->age = ++s_clock;
#ifdef EXP_VCWHY
    if (!s_why_alpha) {
        s_why_alpha = big_alloc(2 * VC_SETS * VC_WAYS * sizeof(uint32_t));
        if (s_why_alpha) s_why_hrn = s_why_alpha + VC_SETS * VC_WAYS;
    }
    if (s_why_alpha) {
        s_why_alpha[v - s_slots] = g_ge.cmd[GE_AMBIENTALPHA];
        s_why_hrn[v - s_slots] = why_hrn();
    }
#endif
    return (GxVtx *)(s_arena + v->off);
}

#endif /* __wii__ */
