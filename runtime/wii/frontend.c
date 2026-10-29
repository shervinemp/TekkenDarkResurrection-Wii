/* Wii frame loop: scan out the PSP framebuffer, route input, present via GX.
 *
 * This runs as one of libogc's cooperative KThreads, at the same priority as
 * the guest host threads.  It must return to the scheduler often: a guest
 * thread only gives up the CPU when it reaches an HLE call that yields, so the
 * frame loop does its work in short slices and yields between them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu/gpu.h"
#include "hle/hle.h"
#include "recomp.h"
#include "wii_platform.h"

extern uint32_t g_buttons;
extern uint8_t g_analog_x, g_analog_y;
extern volatile unsigned g_hle_calls;
extern uint32_t g_frames_presented;
#ifdef TKDR_AUTOPILOT
static uint32_t g_pace[6];
#ifdef AUTOPILOT_MASH
static uint32_t g_mash_frames;   /* frontend frames of this report window with the player-like script pressing buttons */
#endif
#endif
extern volatile uint32_t g_raster_draws;

void script_tick(void);
void frontend_scanout(uint16_t *out);
extern int hle_movie_active(void);
extern uint16_t *hle_movie_pixels(void);
extern int hle_movie_frame_ready(void);
extern unsigned hle_movie_frame_seq(void);
#ifdef HLE_TRACE
extern uint64_t g_present_us;  // diagnostics: time spent presenting
#endif

/* The CPU copy of the guest's framebuffer, used only by the movie/menu
   fallback (the GX path presents the chip's own picture).  It is 255 KiB and
   was a MEM1 static; take it from the MEM2 pool when the fallback is set up. */
static uint16_t *s_pixels;
static uint32_t s_last_guest_frame, s_last_drew;  // what was last presented
static uint32_t s_last_write_tick;                // guest writes to the displayed buffer
static unsigned s_presents;                   // frames actually put on screen
static uint32_t s_last_gx_presents;           // chip copies seen
static unsigned s_gx_idle;                    // frontend frames since the last one
extern volatile uint32_t g_gx_presents, g_gx_lists;

#ifdef __wii__
extern volatile int g_host_threads_started;
#endif

#define FRAME_US 16683u

volatile uint32_t g_frontend_frames;

#ifdef HLE_TRACE
static void frontend_gx_perf(unsigned frames) {
    if (frames % 60) return;
    extern uint64_t g_tex_decodes, g_tex_decode_us;
    extern volatile uint32_t g_gx_texconv, g_gx_binds, g_gx_draws, g_gx_waits;
    extern volatile uint32_t g_gx_lists, g_gx_presents, g_gx_native_tex, g_gx_rgba_tex, g_gx_texconv_us;
    extern uint32_t g_gxr_sw_prims, g_gxr_skipped, g_gxr_rejects[8];
    extern uint32_t g_gxr_rtt_prims, g_gxr_rtt_target;
    extern uint64_t g_gxr_rtt_us;
    extern uint64_t g_tex_dec_conflict, g_tex_dec_changed, g_tex_dec_new, g_tex_flushes;
    extern uint64_t g_hash_bytes, g_hash_us;
    extern uint32_t g_ps_cache_hits, g_ps_cache_misses, g_light_rebuilds, g_tex_copy_drops;
    extern uint64_t g_sas_us, g_atrac_us, g_gxsubmit_us;
    extern uint32_t g_gx_pix_sets, g_gx_tev_sets, g_gx_world_sets, g_gx_batches;
    extern uint32_t g_gxr_paths[3], g_gxr_feat[14], g_gxr_wm[4];
    uint32_t feat_mask = 0;
    for (int i = 0; i < 14; i++) if (g_gxr_feat[i]) feat_mask |= 1u << i;
    extern uint64_t g_gpu_busy_us, g_prim_us;
    extern uint64_t g_iso_read_bytes, g_iso_total_us, g_io_total_us;
    extern volatile unsigned g_hle_calls;
    extern uint64_t g_vformat_us, g_pixstate_us, g_beginstate_us, g_vertex_us, g_emit_us;
    extern uint64_t g_setup_us, g_pixel_us;
    extern uint64_t g_vtx_count, g_vtx_skin, g_vtx_lit, g_vtx_morph, g_vtx_col, g_vtx_nrm;
    extern uint64_t g_vtx_cache_hit, g_vtx_cache_miss;
    fprintf(stderr, "GXPERF f=%u guest=%u texdec=%llu dectime=%llums gxconv=%u binds=%u draws=%u waits=%u "
            "sw=%u skip=%u rej=%u,%u,%u,%u,%u,%u,%u,%u ge=%llums prim=%llums "
            "vf=%llums ps=%llums bs=%llums vs=%llums em=%llums su=%llums px=%llums "
            "gxl=%u gxp=%u nat=%u rgba=%u gxt=%llums hle=%u iso=%lluM isot=%llums iot=%llums vtx=%llu skin=%llu lit=%llu morph=%llu col=%llu nrm=%llu "
            "vhit=%llu vmiss=%llu plan=%u,%u,%u feat=%04x wm=%u,%u,%u,%u rtt=%u/%llums@%08X "
            "dec=%llu/%llu/%llu texflush=%llu hash=%lluK/%llums pool=%uK psc=%u/%u lc=%u drop=%u sas=%llums atr=%llums gxst=%u/%u/%u/%u gxs=%llums\n",
            frames, g_frames_presented, (unsigned long long)g_tex_decodes,
            (unsigned long long)(g_tex_decode_us / 1000), g_gx_texconv, g_gx_binds,
            g_gx_draws, g_gx_waits, g_gxr_sw_prims, g_gxr_skipped,
            g_gxr_rejects[0], g_gxr_rejects[1], g_gxr_rejects[2], g_gxr_rejects[3],
            g_gxr_rejects[4], g_gxr_rejects[5], g_gxr_rejects[6], g_gxr_rejects[7],
            (unsigned long long)(g_gpu_busy_us / 1000),
            (unsigned long long)(g_prim_us / 1000),
            (unsigned long long)(g_vformat_us / 1000),
            (unsigned long long)(g_pixstate_us / 1000),
            (unsigned long long)(g_beginstate_us / 1000),
            (unsigned long long)(g_vertex_us / 1000),
            (unsigned long long)(g_emit_us / 1000),
            (unsigned long long)(g_setup_us / 1000),
            (unsigned long long)(g_pixel_us / 1000),
            g_gx_lists, g_gx_presents, g_gx_native_tex, g_gx_rgba_tex,
            (unsigned long long)(g_gx_texconv_us / 1000),
            g_hle_calls, (unsigned long long)(g_iso_read_bytes >> 20),
            (unsigned long long)(g_iso_total_us / 1000),
            (unsigned long long)(g_io_total_us / 1000),
            (unsigned long long)g_vtx_count, (unsigned long long)g_vtx_skin,
            (unsigned long long)g_vtx_lit, (unsigned long long)g_vtx_morph,
            (unsigned long long)g_vtx_col, (unsigned long long)g_vtx_nrm,
            (unsigned long long)g_vtx_cache_hit, (unsigned long long)g_vtx_cache_miss,
            g_gxr_paths[0], g_gxr_paths[1], g_gxr_paths[2], feat_mask,
            g_gxr_wm[0], g_gxr_wm[1], g_gxr_wm[2], g_gxr_wm[3],
            g_gxr_rtt_prims, (unsigned long long)(g_gxr_rtt_us / 1000), g_gxr_rtt_target,
            (unsigned long long)g_tex_dec_conflict, (unsigned long long)g_tex_dec_changed,
            (unsigned long long)g_tex_dec_new, (unsigned long long)g_tex_flushes,
            (unsigned long long)(g_hash_bytes >> 10), (unsigned long long)(g_hash_us / 1000),
            (unsigned)(wii_big_used() >> 10), g_ps_cache_hits, g_ps_cache_misses, g_light_rebuilds, g_tex_copy_drops,
            (unsigned long long)(g_sas_us / 1000), (unsigned long long)(g_atrac_us / 1000),
            g_gx_pix_sets, g_gx_tev_sets, g_gx_world_sets, g_gx_batches,
            (unsigned long long)(g_gxsubmit_us / 1000));
}

static void frontend_dump_periodic(unsigned frames) {
    if (frames % 600) return;
    { extern void gpu_dump_vformats(void); gpu_dump_vformats(); }
    extern void sched_dump_threads(const char *tag);
    extern void sched_dump_waits(void);
    extern void hle_dump_pc_hist(void);
    sched_dump_threads("periodic");
    sched_dump_waits();
    hle_dump_pc_hist();
}
#endif

#ifdef TKDR_AUTOPILOT
#ifdef EXP_DRAWTRACE
volatile int g_dt_window;   /* the autopilot clock is inside DT_FROM_S..DT_TO_S: raster.c traces the skinned draws */
#ifndef DT_FROM_S
#define DT_FROM_S 75
#define DT_TO_S 80
#endif
#endif
#ifndef AUTOPILOT_SHIFT_MS
#define AUTOPILOT_SHIFT_MS 0
#endif
#ifndef AUTOPILOT_MASH_START_S
#define AUTOPILOT_MASH_START_S 30
#endif
uint64_t g_fe_busy_us;   // time the other threads ran while the frontend yielded
uint64_t g_fe_self_us;   // time the frontend itself ran between yields
uint64_t g_fe_input_us;  // ... of which reading the controllers
uint64_t g_fe_part_us[4];   // ... of which the audio update (1) and the video flip (2)
#endif

void frontend_run(void) {
    unsigned frames = 0;
    wii_heartbeat_start();
    s_pixels = big_alloc(480u * 272u * sizeof *s_pixels);
    if (!s_pixels) {
        wii_log("WII: no memory for the 480x272 framebuffer\n");
        return;
    }
    unsigned home_frames = 0;
    uint64_t next_frame = wii_time_us();

    /* The scene is drawn at the guest's own 480x272 by default.  A build
       with -DHALF_RES draws it at 240x136 instead and expands it into the
       guest's framebuffer: a quarter of the rasterizer's work (about three
       quarters of the emulated CPU) for a softer picture.  In such a build a
       file called "fullres" in the application directory switches back to
       the full resolution rendering. */
    {
        /* The guest is already reading the ISO from the same card; every
           host file access goes through the storage lock. */
        storage_lock();
        static const char *paths[] = {"sd:/apps/tkdr/fullres", "usb:/apps/tkdr/fullres"};
        int full = 0;
        for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
            FILE *f = fopen(paths[i], "rb");
            if (f) {
                fclose(f);
                full = 1;
                wii_log("WII: full resolution requested (%s)\n", paths[i]);
                break;
            }
        }
#ifdef HALF_RES
        gpu_half_res_set(full ? 0 : 1);
#else
        (void)full;
        gpu_half_res_set(0);
#endif
        wii_log("WII: rendering at %s resolution\n",
                gpu_half_res_active() ? "half" : "full");

        /* The GX path: the Wii's own graphics chip draws the guest's
           triangles instead of the CPU.  It is the default now; a file
           called "cpu" in the application directory forces the software
           rasteriser for comparison. */
        {
            static const char *gxp[] = {"sd:/apps/tkdr/gxscene", "usb:/apps/tkdr/gxscene"};
            static const char *cpup[] = {"sd:/apps/tkdr/cpu", "usb:/apps/tkdr/cpu"};
            int on = 1;
            for (unsigned i = 0; i < sizeof cpup / sizeof cpup[0]; i++) {
                FILE *f = fopen(cpup[i], "rb");
                if (f) {
                    fclose(f);
                    on = 0;
                    wii_log("WII: the CPU rasteriser is forced (%s)\n", cpup[i]);
                    break;
                }
            }
            for (unsigned i = 0; i < sizeof gxp / sizeof gxp[0]; i++) {
                FILE *f = fopen(gxp[i], "rb");
                if (f) {
                    fclose(f);
                    on = 1;
                    wii_log("WII: the GX draws the scene (%s)\n", gxp[i]);
                    break;
                }
            }
#ifdef GXSCENE
            on = 1;
#endif
            storage_unlock();
            if (on) {
                /* The chip draws at the display's resolution, so there is
                   no half resolution buffer to expand. */
                gpu_half_res_set(0);
                wii_gxscene_init();
                wii_gxscene_enable(1);
                g_gxr_enabled = 1;
            }
        }
    }

    for (;;) {
        script_tick();
#ifdef __wii__
        /* Let guest/GPU/audio threads run between scanout slices. */
#ifdef TKDR_AUTOPILOT
        {   /* The threads are cooperative, so a yield that comes back at once
               means nothing else was ready (the CPU was idle) and a long one
               is the time the others ran. */
            extern uint64_t g_fe_busy_us, g_fe_self_us;
            static uint64_t y_end;
            uint64_t y0 = wii_time_us();
            if (y_end) g_fe_self_us += y0 - y_end;   /* the frontend's own work since the last yield */
            wii_yield();
            y_end = wii_time_us();
            uint64_t dy = y_end - y0;
            if (dy > 40) g_fe_busy_us += dy;
        }
#else
        wii_yield();
#endif
#endif

        uint64_t now = wii_time_us();
        if (now < next_frame)
            continue;
        next_frame += FRAME_US;
        if (next_frame <= now)
            next_frame = now + FRAME_US;

        g_frontend_frames++;
#ifdef EXP_STAGEID
        {   /* The guest's current stage number: the word at 0x08B756D4, which the stage model's layer table (the bytes at
               0x08B30D88, one per stage) is indexed with.  Every change is logged.  -DEXP_STAGEID_CYCLE=n: when the game
               puts a stage number there (a change, to 0..45, more than 1200 frames after the last one this made), the
               next of the table's stages replaces it, starting at index n, so each fight is on another stage.
               -DEXP_STAGEID_SET=n pins it instead. */
            static const uint8_t ids[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
                                          24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 43, 44, 45};
            static uint32_t last = 0xFFFFFFFEu, held;
            uint32_t v = mem_r32(0x08B756D4u);
#ifdef EXP_STAGEID_SET
            if (v != EXP_STAGEID_SET) { mem_w32(0x08B756D4u, EXP_STAGEID_SET); v = EXP_STAGEID_SET; }
#endif
#ifdef EXP_STAGEID_CYCLE
            static unsigned cyc = EXP_STAGEID_CYCLE;
            if (v != last && v < 46 && (held == 0 || g_frontend_frames - held > 1200)) {
                uint32_t w = ids[cyc++ % (sizeof ids / sizeof ids[0])];
                fprintf(stderr, "STAGEID gf=%u f=%u the game wrote %u, replaced by %u\n", g_frames_presented, g_frontend_frames, v, w);
                mem_w32(0x08B756D4u, w);
                v = w;
                held = g_frontend_frames;
            }
#endif
            if (v != last) {
                fprintf(stderr, "STAGEID gf=%u f=%u now %u\n", g_frames_presented, g_frontend_frames, v);
                last = v;
            }
        }
#endif
#ifdef EXP_STACKPROBE
        if (g_frontend_frames && g_frontend_frames % 300 == 0) {
            extern void wii_stack_probe_report(void);
            wii_stack_probe_report();
        }
#endif
#ifdef TKDR_AUTOPILOT
        {   /* pacing: how many frontend frames (display refreshes) each guest frame stayed up */
            static uint32_t pg, plast;
            if (g_frames_presented != pg) {
                uint32_t iv = g_frontend_frames - plast;
                plast = g_frontend_frames;
                pg = g_frames_presented;
                g_pace[iv > 5 ? 5 : iv]++;
            }
        }
        /* Frame rates for release-speed autopilot runs, which have no GXPERF
           line: the debug probes cost enough per primitive to distort them. */
        if (g_frontend_frames % 300 == 0) {
            extern uint64_t g_gpu_busy_us;
            static uint64_t last_t, last_ge, last_busy;
            static uint32_t last_fe, last_guest;
            uint64_t t = wii_time_us();
            if (last_t) {
                float s = (float)(t - last_t) / 1e6f;
                uint32_t gf = g_frames_presented - last_guest;
                fprintf(stderr, "DEMOFPS fe=%u guest=%u fe/s=%.1f guest/s=%.1f busy=%.0f%% ge=%.1fms/guest-frame "
                        "other=%.1fms/guest-frame\n",
                        g_frontend_frames, g_frames_presented,
                        (float)(g_frontend_frames - last_fe) / s, (float)gf / s,
                        (double)(g_fe_busy_us - last_busy) / 1e4 / s,
                        gf ? (double)(g_gpu_busy_us - last_ge) / 1e3 / gf : 0.0,
                        gf ? (double)((g_fe_busy_us - last_busy) - (g_gpu_busy_us - last_ge)) / 1e3 / gf : 0.0);
#ifdef EXP_HITCHK
                {
                    extern uint32_t g_chk_hit_n, g_chk_hit_bad, g_chk_mv_n, g_chk_mv_bad, g_chk_gx_n, g_chk_gx_bad, g_chk_sk_n, g_chk_sk_bad;
                    fprintf(stderr, "  HITCHK skinned state rebuilt and compared %u stale %u\n", g_chk_sk_n, g_chk_sk_bad);
                    {
                        extern uint32_t g_chk_ps_n, g_chk_ps_bad;
                        fprintf(stderr, "  HITCHK pixel state rebuilt and compared %u stale %u\n", g_chk_ps_n, g_chk_ps_bad);
                    }
                    fprintf(stderr, "  HITCHK cached draws re-decoded %u bad %u; model-view checks %u stale %u; chip slot checks %u stale %u\n",
                            g_chk_hit_n, g_chk_hit_bad, g_chk_mv_n, g_chk_mv_bad, g_chk_gx_n, g_chk_gx_bad);
                }
#endif
                /* The fights are frame-locked at 60 logic frames a second: anything under 60 is slow motion. */
                {
                    extern uint32_t g_pr_copies, g_pr_small, g_pr_dark, g_pr_samples;
                    extern uint64_t g_pr_luma, g_pr_zsum;
                    static uint32_t l_pc, l_ps, l_pd;
                    static uint64_t l_pl, l_pn, l_pz;
                    uint32_t pc = g_pr_copies - l_pc;
                    fprintf(stderr, "  COPIES per guest frame %.2f (fragments under 100 draws %.2f), dark copies %u of %u, chip frame buffer mean luma %.1f, mean depth %.0f\n",
                            gf ? (double)pc / gf : 0.0, gf ? (double)(g_pr_small - l_ps) / gf : 0.0, g_pr_dark - l_pd, pc,
                            g_pr_samples != l_pn ? (double)(g_pr_luma - l_pl) / (double)(g_pr_samples - l_pn) : 0.0,
                            g_pr_samples != l_pn ? (double)(g_pr_zsum - l_pz) / (double)(g_pr_samples - l_pn) : 0.0);
                    l_pc = g_pr_copies; l_ps = g_pr_small; l_pd = g_pr_dark; l_pl = g_pr_luma; l_pn = g_pr_samples; l_pz = g_pr_zsum;
                }
                fprintf(stderr, "  SPEED the game runs at %.0f%% of real time (guest frames a second over 60)\n", (double)gf / s / 60.0 * 100.0);
#ifdef AUTOPILOT_MASH
                fprintf(stderr, "  MASH the player-like script pressed buttons in %u of the window's 300 frontend frames\n", g_mash_frames);
                g_mash_frames = 0;
#endif
                fprintf(stderr, "  PACING guest frames by display refreshes they lasted: 1:%u 2:%u 3:%u 4:%u 5+:%u\n", g_pace[1], g_pace[2],
                        g_pace[3], g_pace[4], g_pace[5]);
                memset(g_pace, 0, sizeof g_pace);
                extern uint64_t g_vfpu_calls, g_vfpu_ticks;
                extern void hle_prof_report(uint32_t guest_frames);
                static uint64_t last_vc, last_vt;
                fprintf(stderr, "  VFPU %.0f calls/frame %.2f ms/frame\n",
                        gf ? (double)(g_vfpu_calls - last_vc) / gf : 0.0,
                        gf ? (double)(g_vfpu_ticks - last_vt) / 60750.0 / gf : 0.0);
                last_vc = g_vfpu_calls;
                last_vt = g_vfpu_ticks;
                {
                    extern uint32_t g_vc_hits, g_vc_misses, g_vc_resets, g_demo_verts, g_demo_draws;
                    extern uint64_t g_tex_decodes;
                    static uint64_t last_td;
                    fprintf(stderr, "  SCENE %u verts, %u draws per guest frame; draw cache hits %u misses %u resets %u; "
                            "texture decodes %u\n", gf ? g_demo_verts / gf : 0, gf ? g_demo_draws / gf : 0,
                            g_vc_hits, g_vc_misses, g_vc_resets, (unsigned)(g_tex_decodes - last_td));
                    last_td = g_tex_decodes;
                    extern uint32_t g_vc_addr_match, g_vc_data_diff, g_vc_world_diff, g_vc_state_diff, g_vc_light_diff;
                    fprintf(stderr, "  VCACHE addr-match %u data-diff %u world-diff %u state-diff %u light-diff %u\n", g_vc_addr_match,
                            g_vc_data_diff, g_vc_world_diff, g_vc_state_diff, g_vc_light_diff);
                    {   /* the draw cache arena: how much of it is handed out, and the draws it had no room for */
                        extern uint32_t g_vc_full, g_vc_gc, vc_arena_used(void), vc_arena_size(void);
                        fprintf(stderr, "  VCARENA %u KB of %u handed out, %u draws not cached (no block), %u collections; MEM2 pool %u KB of 14336 used\n",
                                vc_arena_used() >> 10, vc_arena_size() >> 10, g_vc_full, g_vc_gc, (unsigned)(wii_big_used() >> 10));
                        g_vc_full = g_vc_gc = 0;
                        extern uint32_t g_vc_cls[3], g_vc_occ[5];
                        fprintf(stderr, "  VCMISS no slot for the address %u, other bytes %u, the same bytes under other state %u (of those, the address held in %u/%u/%u/%u slots)\n",
                                g_vc_cls[0], g_vc_cls[1], g_vc_cls[2], g_vc_occ[1], g_vc_occ[2], g_vc_occ[3], g_vc_occ[4]);
                        memset(g_vc_cls, 0, sizeof g_vc_cls);
                        memset(g_vc_occ, 0, sizeof g_vc_occ);
#ifdef EXP_VCWHY
                        {
                            extern uint32_t g_vc_why[2];
                            fprintf(stderr, "  VCWHYSTAT misses of cached bytes under other lighting: only the global ambient alpha changed %u, something else %u\n",
                                    g_vc_why[0], g_vc_why[1]);
                            g_vc_why[0] = g_vc_why[1] = 0;
                        }
#endif
#ifdef EXP_VCDUMP
                        {   /* every third heavy window, up to fourteen: the last 16384 lookups, for tools/perf/vcsim.py */
                            extern void vc_dump_ring(void);
                            static unsigned heavy, dumped;
                            if (gf && g_demo_verts / gf >= 20000 && ++heavy % 3 == 0 && dumped < 14) { dumped++; vc_dump_ring(); }
                        }
#endif
                    }
                    {
                        extern uint32_t g_vc_refresh;
                        fprintf(stderr, "  VCREFRESH %u draw cache hits made their alpha bytes again (the global ambient alpha moved)\n", g_vc_refresh);
                        g_vc_refresh = 0;
                    }
                    g_vc_addr_match = g_vc_data_diff = g_vc_world_diff = g_vc_state_diff = g_vc_light_diff = 0;
                    g_demo_verts = g_demo_draws = g_vc_hits = g_vc_misses = g_vc_resets = 0;
                }
                {
                    extern uint64_t g_dp_ticks[6];
                    double k = gf ? 1.0 / 60750.0 / gf : 0.0;
                    fprintf(stderr, "  DRAWPROF setup %.2f lookup %.2f decode %.2f submit %.2f (cached submit %.2f) all %.2f ms/frame\n",
                            (double)g_dp_ticks[0] * k, (double)g_dp_ticks[1] * k, (double)g_dp_ticks[2] * k,
                            (double)g_dp_ticks[3] * k, (double)g_dp_ticks[5] * k, (double)g_dp_ticks[4] * k);
                    for (int i = 0; i < 6; i++) g_dp_ticks[i] = 0;
                    extern uint32_t g_cat_draws[7], g_cat_verts[7];
                    fprintf(stderr, "  CATS skinned %u/%u through %u/%u idx/morph %u/%u long %u/%u plain %u/%u normals %u/%u (draws/verts per frame)\n",
                            gf ? g_cat_draws[0] / gf : 0, gf ? g_cat_verts[0] / gf : 0, gf ? g_cat_draws[1] / gf : 0,
                            gf ? g_cat_verts[1] / gf : 0, gf ? g_cat_draws[2] / gf : 0, gf ? g_cat_verts[2] / gf : 0,
                            gf ? g_cat_draws[3] / gf : 0, gf ? g_cat_verts[3] / gf : 0, gf ? g_cat_draws[4] / gf : 0,
                            gf ? g_cat_verts[4] / gf : 0, gf ? g_cat_draws[5] / gf : 0, gf ? g_cat_verts[5] / gf : 0);
                    memset(g_cat_draws, 0, sizeof g_cat_draws);
                    memset(g_cat_verts, 0, sizeof g_cat_verts);
                }
                {
                    extern unsigned g_guest_mem_ticks;
                    extern uint64_t g_audio_pull_us;
                    static unsigned last_mt;
                    static uint64_t last_self, last_audio, last_input;
                    extern uint64_t g_fe_input_us;
                    {
                        extern uint32_t g_rs_calls, g_rs_switches, g_rs_ticks_calls;
                        static uint32_t l_rc, l_rs, l_rt, l_hc;
                        fprintf(stderr, "  SCHED %u reschedules, %u switches, %u guest ticks, %u HLE calls per guest frame\n",
                                gf ? (g_rs_calls - l_rc) / gf : 0, gf ? (g_rs_switches - l_rs) / gf : 0,
                                gf ? (g_rs_ticks_calls - l_rt) / gf : 0, gf ? (g_hle_calls - l_hc) / gf : 0);
                        l_rc = g_rs_calls; l_rs = g_rs_switches; l_rt = g_rs_ticks_calls; l_hc = g_hle_calls;
                    }
                    {
                        extern uint32_t g_op_hist[256];
                        uint32_t total = 0;
                        for (int i = 0; i < 256; i++) total += g_op_hist[i];
                        fprintf(stderr, "  GECMDS %u per guest frame; top:", gf ? total / gf : 0);
                        for (int n = 0; n < 12; n++) {
                            int best = 0;
                            for (int i = 1; i < 256; i++) if (g_op_hist[i] > g_op_hist[best]) best = i;
                            if (!g_op_hist[best]) break;
                            fprintf(stderr, " %02X:%u", best, gf ? g_op_hist[best] / gf : 0);
                            g_op_hist[best] = 0;
                        }
                        fprintf(stderr, "\n");
                        memset(g_op_hist, 0, sizeof g_op_hist);
                    }
                    {
                        extern uint32_t g_world_ident[2];
                        fprintf(stderr, "  WORLD skinned draws with identity world: %u of %u\n", g_world_ident[1],
                                g_world_ident[0] + g_world_ident[1]);
                        g_world_ident[0] = g_world_ident[1] = 0;
                    }
                    {
                        extern uint32_t g_sk_why[4];
                        fprintf(stderr, "  SKINREFUSED per frame: format %u lighting-off %u light-setup %u\n", gf ? g_sk_why[0] / gf : 0,
                                gf ? g_sk_why[1] / gf : 0, gf ? g_sk_why[2] / gf : 0);
                        memset(g_sk_why, 0, sizeof g_sk_why);
                        extern uint32_t g_sk_vt[4], g_sk_vtn[4], g_sk_vv[4];
                        fprintf(stderr, "  SKINFMT skinned draws on the general path (vertex type, bit 31 = lighting on: draws/vertices per frame):");
                        for (int i = 0; i < 4; i++) if (g_sk_vt[i]) fprintf(stderr, " %08X:%u/%u", g_sk_vt[i], gf ? g_sk_vtn[i] / gf : 0, gf ? g_sk_vv[i] / gf : 0);
                        fprintf(stderr, "\n");
                        memset(g_sk_vt, 0, sizeof g_sk_vt);
                        memset(g_sk_vtn, 0, sizeof g_sk_vtn);
                        memset(g_sk_vv, 0, sizeof g_sk_vv);
                    }
#ifdef TKDR_FINEPROF
                    {
                        extern uint32_t g_sk_nz[10], g_sk_nwv[10], g_sk_full, g_sk_draws, g_sk_verts, g_sk_mode[5], g_sk_lmode[4], g_sk_spec, g_sk_tc[4];
                        fprintf(stderr, "  SKINLIGHT draws per frame: lightmode 0/1/2/3 %u/%u/%u/%u spec %u; tc kind 0/1/2/3 %u/%u/%u/%u\n",
                                gf ? g_sk_lmode[0] / gf : 0, gf ? g_sk_lmode[1] / gf : 0, gf ? g_sk_lmode[2] / gf : 0, gf ? g_sk_lmode[3] / gf : 0,
                                gf ? g_sk_spec / gf : 0, gf ? g_sk_tc[0] / gf : 0, gf ? g_sk_tc[1] / gf : 0, gf ? g_sk_tc[2] / gf : 0,
                                gf ? g_sk_tc[3] / gf : 0);
                        memset(g_sk_lmode, 0, sizeof g_sk_lmode); memset(g_sk_tc, 0, sizeof g_sk_tc); g_sk_spec = 0;
                        fprintf(stderr, "  SKINSHAPE per frame: draws %u verts %u (mode1 %u mode2 %u) single-255 %u; nonzero weights", gf ? g_sk_draws / gf : 0,
                                gf ? g_sk_verts / gf : 0, gf ? g_sk_mode[1] / gf : 0, gf ? g_sk_mode[2] / gf : 0, gf ? g_sk_full / gf : 0);
                        for (int k = 0; k < 10; k++) if (g_sk_nz[k]) fprintf(stderr, " %d:%u", k, gf ? g_sk_nz[k] / gf : 0);
                        fprintf(stderr, "; nw");
                        for (int k = 0; k < 10; k++) if (g_sk_nwv[k]) fprintf(stderr, " %d:%u", k, gf ? g_sk_nwv[k] / gf : 0);
                        fprintf(stderr, "\n");
                        memset(g_sk_nz, 0, sizeof g_sk_nz); memset(g_sk_nwv, 0, sizeof g_sk_nwv); memset(g_sk_mode, 0, sizeof g_sk_mode);
                        g_sk_full = g_sk_draws = g_sk_verts = 0;
                    }
#endif
                    {
                        extern uint32_t g_other_why[8], g_other_prim[8];
                        fprintf(stderr, "  OTHER3D per frame: idx %u morph %u flat %u prim %u skinned %u long %u normals %u other %u; prims 3:%u 4:%u 5:%u\n",
                                gf ? g_other_why[0] / gf : 0, gf ? g_other_why[1] / gf : 0, gf ? g_other_why[2] / gf : 0,
                                gf ? g_other_why[3] / gf : 0, gf ? g_other_why[4] / gf : 0, gf ? g_other_why[5] / gf : 0,
                                gf ? g_other_why[6] / gf : 0, gf ? g_other_why[7] / gf : 0, gf ? g_other_prim[3] / gf : 0,
                                gf ? g_other_prim[4] / gf : 0, gf ? g_other_prim[5] / gf : 0);
                        memset(g_other_why, 0, sizeof g_other_why);
                        memset(g_other_prim, 0, sizeof g_other_prim);
                    }
                    {
                        extern uint64_t g_ps_rebuild_ticks, g_ps_rebuild_n, g_vf_rebuild_n;
                        static uint64_t l_pt, l_pn, l_vn;
                        uint64_t n = g_ps_rebuild_n - l_pn;
                        fprintf(stderr, "  REBUILDS per frame: pixel state %u (%.2f us each, %.2f ms), vertex format %u\n",
                                gf ? (unsigned)(n / gf) : 0, n ? (double)(g_ps_rebuild_ticks - l_pt) / 60.75 / n : 0.0,
                                gf ? (double)(g_ps_rebuild_ticks - l_pt) / 60750.0 / gf : 0.0,
                                gf ? (unsigned)((g_vf_rebuild_n - l_vn) / gf) : 0);
                        l_pt = g_ps_rebuild_ticks; l_pn = g_ps_rebuild_n; l_vn = g_vf_rebuild_n;
                    }
                    {
                        extern uint64_t g_fast_ticks[3], g_fast_n;
                        static uint64_t l_ft[3], l_fn;
                        uint64_t n = g_fast_n - l_fn;
                        fprintf(stderr, "  FASTPATH per draw (%u draws/frame): lookup+checks %.2f us, chip setup %.2f us, submit %.2f us\n",
                                gf ? (unsigned)(n / gf) : 0, n ? (double)(g_fast_ticks[0] - l_ft[0]) / 60.75 / n : 0.0,
                                n ? (double)(g_fast_ticks[1] - l_ft[1]) / 60.75 / n : 0.0,
                                n ? (double)(g_fast_ticks[2] - l_ft[2]) / 60.75 / n : 0.0);
                        for (int i = 0; i < 3; i++) l_ft[i] = g_fast_ticks[i];
                        l_fn = g_fast_n;
                    }
                    {
                        extern uint32_t g_cam_n, g_cam_ring_n;
                        extern double g_cam_d1, g_cam_d2, g_cam_max1, g_cam_max2;
                        extern float g_cam_ring[32][3];
                        extern double g_cam_a1, g_cam_a2, g_cam_amax2;
                        static uint32_t l_cn;
                        static double l_d1, l_d2, l_a1, l_a2;
                        uint32_t n = g_cam_n - l_cn;
                        fprintf(stderr, "  CAMERA %u frames: mean step %.5f, mean 2nd diff %.5f (ratio %.2f), max step %.4f, max 2nd diff %.4f; last:",
                                n, n ? (g_cam_d1 - l_d1) / n : 0.0, n ? (g_cam_d2 - l_d2) / n : 0.0,
                                g_cam_d1 - l_d1 > 0 ? (g_cam_d2 - l_d2) / (g_cam_d1 - l_d1) : 0.0, g_cam_max1, g_cam_max2);
                        for (uint32_t i = g_cam_ring_n > 8 ? g_cam_ring_n - 8 : 0; i < g_cam_ring_n; i++)
                            fprintf(stderr, " (%.3f %.3f %.3f)", (double)g_cam_ring[i & 31][0], (double)g_cam_ring[i & 31][1], (double)g_cam_ring[i & 31][2]);
                        fprintf(stderr, "\n");
                        fprintf(stderr, "  CAMDIR mean step %.6f rad, mean 2nd diff %.6f (ratio %.2f), max 2nd diff %.6f\n", n ? (g_cam_a1 - l_a1) / n : 0.0,
                                n ? (g_cam_a2 - l_a2) / n : 0.0, g_cam_a1 - l_a1 > 0 ? (g_cam_a2 - l_a2) / (g_cam_a1 - l_a1) : 0.0, g_cam_amax2);
                        l_cn = g_cam_n; l_d1 = g_cam_d1; l_d2 = g_cam_d2; l_a1 = g_cam_a1; l_a2 = g_cam_a2;
                        g_cam_max1 = g_cam_max2 = g_cam_amax2 = 0;
                    }
                    {
                        extern uint64_t g_hr_ticks[2], g_hr_n[2];
                        static uint64_t l_ht[2], l_hn[2];
                        {
                            extern uint32_t g_tgt_refused, g_tgt_nd_draws, g_tgt_fb, g_tgt_nd_fb;
                            extern int gpu_disp_info(uint32_t *a);
                            uint32_t da[4];
                            int dn = gpu_disp_info(da);
                            fprintf(stderr, "  TARGETS draw fb %08X, last non-display fb %08X, presented (%d) %08X %08X %08X %08X; fast path refused %u, non-display draws %u per frame\n",
                                    g_tgt_fb, g_tgt_nd_fb, dn, da[0], da[1], da[2], da[3], gf ? g_tgt_refused / gf : 0, gf ? g_tgt_nd_draws / gf : 0);
                            g_tgt_refused = g_tgt_nd_draws = 0;
                        }
                        fprintf(stderr, "  HOTREFRESH per frame: plain %u x %.2f us = %.2f ms, skinned %u x %.2f us = %.2f ms\n",
                                gf ? (unsigned)((g_hr_n[0] - l_hn[0]) / gf) : 0,
                                g_hr_n[0] != l_hn[0] ? (double)(g_hr_ticks[0] - l_ht[0]) / 60.75 / (g_hr_n[0] - l_hn[0]) : 0.0,
                                gf ? (double)(g_hr_ticks[0] - l_ht[0]) / 60750.0 / gf : 0.0,
                                gf ? (unsigned)((g_hr_n[1] - l_hn[1]) / gf) : 0,
                                g_hr_n[1] != l_hn[1] ? (double)(g_hr_ticks[1] - l_ht[1]) / 60.75 / (g_hr_n[1] - l_hn[1]) : 0.0,
                                gf ? (double)(g_hr_ticks[1] - l_ht[1]) / 60750.0 / gf : 0.0);
                        {
                            extern uint64_t g_hr_sub[2][3], g_hr_subn[2][3];
                            static uint64_t l_hs[2][3], l_hsn[2][3];
                            for (int k = 0; k < 2; k++)
                                fprintf(stderr, "  HOTPARTS %s per call: format %.2f us (%u new/frame), pixel state %.2f us (%u rebuilt/frame), vertex state %.2f us\n",
                                        k ? "skinned" : "plain", gf && g_hr_n[k] != l_hn[k] ? (double)(g_hr_sub[k][0] - l_hs[k][0]) / 60.75 / (g_hr_n[k] - l_hn[k]) : 0.0,
                                        gf ? (unsigned)((g_hr_subn[k][0] - l_hsn[k][0]) / gf) : 0,
                                        g_hr_n[k] != l_hn[k] ? (double)(g_hr_sub[k][1] - l_hs[k][1]) / 60.75 / (g_hr_n[k] - l_hn[k]) : 0.0,
                                        gf ? (unsigned)((g_hr_subn[k][1] - l_hsn[k][1]) / gf) : 0,
                                        g_hr_n[k] != l_hn[k] ? (double)(g_hr_sub[k][2] - l_hs[k][2]) / 60.75 / (g_hr_n[k] - l_hn[k]) : 0.0);
                            memcpy(l_hs, g_hr_sub, sizeof l_hs); memcpy(l_hsn, g_hr_subn, sizeof l_hsn);
                            extern uint64_t g_th_ticks, g_th_n, g_th_bytes, g_tl_ticks, g_tl_n;
                            static uint64_t l_tt, l_tn, l_tb, l_lt, l_ln;
                            fprintf(stderr, "  TEXLOOKUP per frame: %u lookups = %.2f ms; %u re-hashes of %u KB = %.2f ms\n",
                                    gf ? (unsigned)((g_tl_n - l_ln) / gf) : 0, gf ? (double)(g_tl_ticks - l_lt) / 60750.0 / gf : 0.0,
                                    gf ? (unsigned)((g_th_n - l_tn) / gf) : 0, gf ? (unsigned)((g_th_bytes - l_tb) / 1024 / gf) : 0,
                                    gf ? (double)(g_th_ticks - l_tt) / 60750.0 / gf : 0.0);
                            extern uint64_t g_tp_ticks[2];
                            static uint64_t l_tp[2];
                            fprintf(stderr, "  TEXPRE per frame: flush check %.2f ms, surface resolve %.2f ms\n", gf ? (double)(g_tp_ticks[0] - l_tp[0]) / 60750.0 / gf : 0.0,
                                    gf ? (double)(g_tp_ticks[1] - l_tp[1]) / 60750.0 / gf : 0.0);
                            l_tp[0] = g_tp_ticks[0]; l_tp[1] = g_tp_ticks[1];
                            {
                                extern uint64_t g_td_ticks, g_td_n, g_td_texels, g_ts_same;
                                extern struct { uint32_t addr, fmt, wh, n; } g_td_top[16];
                                static uint64_t l_dt, l_dn, l_dx, l_ds;
                                fprintf(stderr, "  TEXDECODE per frame: %u decodes of %u Ktexels = %.2f ms; %u stale lookups found the bytes unchanged; by address:",
                                        gf ? (unsigned)((g_td_n - l_dn) / gf) : 0, gf ? (unsigned)((g_td_texels - l_dx) / 1024 / gf) : 0,
                                        gf ? (double)(g_td_ticks - l_dt) / 60750.0 / gf : 0.0, gf ? (unsigned)((g_ts_same - l_ds) / gf) : 0);
                                for (int i = 0; i < 16 && g_td_top[i].n; i++)
                                    fprintf(stderr, " %08X f%X %ux%u x%u;", g_td_top[i].addr, g_td_top[i].fmt, g_td_top[i].wh & 0xFFFF, g_td_top[i].wh >> 16,
                                            g_td_top[i].n);
                                fprintf(stderr, "\n");
                                {
                                    extern uint64_t g_tl_part[4], g_tl_partn[2];
                                    static uint64_t l_p[4], l_pn[2];
                                    uint64_t n0 = g_tl_partn[0] - l_pn[0], n1 = g_tl_partn[1] - l_pn[1];
                                    fprintf(stderr, "  TEXLOOKUP parts: clean %llu x %.2f us; stale %llu x (preamble %.2f, before hash %.2f, after hash %.2f) us\n",
                                            (unsigned long long)n0, n0 ? (double)(g_tl_part[0] - l_p[0]) / 60.75 / n0 : 0.0, (unsigned long long)n1,
                                            n1 ? (double)(g_tl_part[1] - l_p[1]) / 60.75 / n1 : 0.0, n1 ? (double)(g_tl_part[3] - l_p[3]) / 60.75 / n1 : 0.0,
                                            n1 ? (double)(g_tl_part[2] - l_p[2]) / 60.75 / n1 : 0.0);
                                    for (int i = 0; i < 4; i++) l_p[i] = g_tl_part[i];
                                    l_pn[0] = g_tl_partn[0]; l_pn[1] = g_tl_partn[1];
                                }
                                memset(g_td_top, 0, sizeof g_td_top);
                                l_dt = g_td_ticks; l_dn = g_td_n; l_dx = g_td_texels; l_ds = g_ts_same;
                            }
                            l_tt = g_th_ticks; l_tn = g_th_n; l_tb = g_th_bytes; l_lt = g_tl_ticks; l_ln = g_tl_n;
                        }
#ifdef EXP_GXLIGHT
                        {
                            extern uint32_t g_gx_lit_draws, g_gx_lit_enters, g_gx_lit_loads;
                            static uint32_t l_ld, l_le, l_ll;
                            fprintf(stderr, "  LITSTAT per frame: %u lit draws, %u switches to lit vertices, %u light loads\n", gf ? (g_gx_lit_draws - l_ld) / gf : 0,
                                    gf ? (g_gx_lit_enters - l_le) / gf : 0, gf ? (g_gx_lit_loads - l_ll) / gf : 0);
                            l_ld = g_gx_lit_draws; l_le = g_gx_lit_enters; l_ll = g_gx_lit_loads;
                        }
                        {
                            extern uint32_t g_rg_draws, g_rg_verts, g_rg_notrigid, g_rg_nomat, g_gx_rigid_loads;
                            static uint32_t l_rd, l_rv, l_rn, l_rm, l_rl;
                            fprintf(stderr, "  RIGID per frame: %u draws (%u vertices) transformed by the chip, %u matrix loads; refused: %u not single-bone, %u by the normal matrix\n",
                                    gf ? (g_rg_draws - l_rd) / gf : 0, gf ? (g_rg_verts - l_rv) / gf : 0, gf ? (g_gx_rigid_loads - l_rl) / gf : 0,
                                    gf ? (g_rg_notrigid - l_rn) / gf : 0, gf ? (g_rg_nomat - l_rm) / gf : 0);
                            l_rd = g_rg_draws; l_rv = g_rg_verts; l_rn = g_rg_notrigid; l_rm = g_rg_nomat; l_rl = g_gx_rigid_loads;
                        }
#endif
                        {
                            extern uint32_t g_hr_why[10];
                            fprintf(stderr, "  HOTWHY per frame: pix %u vertex %u light %u view %u world %u vt %u epoch %u frames %u format %u\n",
                                    gf ? g_hr_why[0] / gf : 0, gf ? g_hr_why[1] / gf : 0, gf ? g_hr_why[2] / gf : 0, gf ? g_hr_why[3] / gf : 0,
                                    gf ? g_hr_why[4] / gf : 0, gf ? g_hr_why[5] / gf : 0, gf ? g_hr_why[6] / gf : 0, gf ? g_hr_why[7] / gf : 0,
                                    gf ? g_hr_why[8] / gf : 0);
                            {
                                extern uint64_t g_sw_ticks;
                                static uint64_t l_sw;
                                fprintf(stderr, "  HOTSWITCH per frame: %u vertex type switches without a refresh, %.2f us each (%.2f ms)\n",
                                        gf ? g_hr_why[9] / gf : 0, g_hr_why[9] ? (double)(g_sw_ticks - l_sw) / 60.75 / g_hr_why[9] : 0.0,
                                        gf ? (double)(g_sw_ticks - l_sw) / 60750.0 / gf : 0.0);
                                l_sw = g_sw_ticks;
                            }
                            memset(g_hr_why, 0, sizeof g_hr_why);
                        }
                        for (int i = 0; i < 2; i++) { l_ht[i] = g_hr_ticks[i]; l_hn[i] = g_hr_n[i]; }
                    }
                    {
                        extern uint64_t g_fp_ticks[2][4], g_fp_n[2], g_vc_ticks[2];
                        static uint64_t l_fp[2][4], l_pn[2], l_vc[2];
                        static const char *const nm[2] = {"skinned", "hit"};
                        for (int k = 0; k < 2; k++) {
                            uint64_t n = g_fp_n[k] - l_pn[k];
                            double d[4];
                            for (int i = 0; i < 4; i++) d[i] = n ? (double)(g_fp_ticks[k][i] - l_fp[k][i]) / 60.75 / n : 0.0;
                            fprintf(stderr, "  FASTSPLIT %s (%u/frame): checks %.2f, %s %.2f, setup %.2f, submit %.2f us",
                                    nm[k], gf ? (unsigned)(n / gf) : 0, d[0], k ? "lookup" : "decode", d[1], d[2], d[3]);
                            if (k)
                                fprintf(stderr, " (hash %.2f, search %.2f)",
                                        n ? (double)(g_vc_ticks[0] - l_vc[0]) / 60.75 / n : 0.0,
                                        n ? (double)(g_vc_ticks[1] - l_vc[1]) / 60.75 / n : 0.0);
                            fprintf(stderr, "\n");
                            for (int i = 0; i < 4; i++) l_fp[k][i] = g_fp_ticks[k][i];
                            l_pn[k] = g_fp_n[k];
                        }
                        for (int i = 0; i < 2; i++) l_vc[i] = g_vc_ticks[i];
                    }
#ifdef TKDR_FINEPROF
                    {
                        extern uint64_t g_pm_ticks[3], g_pm_n, g_pm_verts;
                        static uint64_t l_pt[3], l_pn2, l_pv;
                        const uint64_t n = g_pm_n - l_pn2, v = g_pm_verts - l_pv;
                        fprintf(stderr, "  PLAINMISS %u draws, %u vertices a frame: decode %.2f us a draw (%.2f a vertex), store %.2f, pack %.2f us\n",
                                gf ? (unsigned)(n / gf) : 0, gf ? (unsigned)(v / gf) : 0,
                                n ? (double)(g_pm_ticks[0] - l_pt[0]) / 60.75 / n : 0.0, v ? (double)(g_pm_ticks[0] - l_pt[0]) / 60.75 / v : 0.0,
                                n ? (double)(g_pm_ticks[1] - l_pt[1]) / 60.75 / n : 0.0, n ? (double)(g_pm_ticks[2] - l_pt[2]) / 60.75 / n : 0.0);
                        for (int i = 0; i < 3; i++) l_pt[i] = g_pm_ticks[i];
                        l_pn2 = g_pm_n; l_pv = g_pm_verts;
                    }
                    {
                        extern uint64_t g_vc_ticks[2], g_vc_hbytes, g_vc_hlook, g_vc_hbytes_hit, g_vc_hlook_hit, g_vc_hticks_hit;
                        static uint64_t l_t, l_b, l_n, l_bh, l_nh, l_th;
                        const uint64_t n = g_vc_hlook - l_n, nh = g_vc_hlook_hit - l_nh, b = g_vc_hbytes - l_b, bh = g_vc_hbytes_hit - l_bh;
                        const uint64_t t = g_vc_ticks[0] - l_t, th = g_vc_hticks_hit - l_th;
                        const double ms = gf ? 1.0 / 60750.0 / gf : 0.0;
                        fprintf(stderr, "  HASHCOST plain-draw vertex hash per frame: all %u lookups %.1f KB %.2f ms (%.2f us a lookup, %.1f bytes, %.1f cycles a word); "
                                "hits %u lookups %.1f KB %.2f ms (%.2f us a lookup, %.1f bytes)\n",
                                gf ? (unsigned)(n / gf) : 0, gf ? (double)b / 1024.0 / gf : 0.0, t * ms, n ? (double)t / 60.75 / n : 0.0,
                                n ? (double)b / n : 0.0, b ? (double)t * 12.0 / ((double)b / 4.0) : 0.0,
                                gf ? (unsigned)(nh / gf) : 0, gf ? (double)bh / 1024.0 / gf : 0.0, th * ms, nh ? (double)th / 60.75 / nh : 0.0,
                                nh ? (double)bh / nh : 0.0);
                        l_t = g_vc_ticks[0]; l_b = g_vc_hbytes; l_n = g_vc_hlook; l_bh = g_vc_hbytes_hit; l_nh = g_vc_hlook_hit; l_th = g_vc_hticks_hit;
                    }
#endif
                    {
                        extern uint64_t g_cat_ticks[6], g_cat_n[6];
                        static uint64_t l_ct[6], l_cn[6];
                        static const char *const nm[6] = {"fast-skinned", "cache-hit", "cache-miss", "skinned-slow", "other-3D", "2D/sw"};
                        fprintf(stderr, "  DRAWCATS ms/frame (draws/frame):");
                        for (int i = 0; i < 6; i++) {
                            fprintf(stderr, " %s %.2f (%u)", nm[i], gf ? (double)(g_cat_ticks[i] - l_ct[i]) / 60750.0 / gf : 0.0,
                                    gf ? (unsigned)((g_cat_n[i] - l_cn[i]) / gf) : 0);
                            l_ct[i] = g_cat_ticks[i]; l_cn[i] = g_cat_n[i];
                        }
                        fprintf(stderr, "\n");
                    }
                    {
                        extern uint64_t g_op_ticks[256];
                        fprintf(stderr, "  GEOPS ms per guest frame by opcode:");
                        for (int n = 0; n < 10; n++) {
                            int best = 0;
                            for (int i = 1; i < 256; i++) if (g_op_ticks[i] > g_op_ticks[best]) best = i;
                            if (!g_op_ticks[best]) break;
                            fprintf(stderr, " %02X:%.2f", best, gf ? (double)g_op_ticks[best] / 60750.0 / gf : 0.0);
                            g_op_ticks[best] = 0;
                        }
                        fprintf(stderr, "\n");
                        memset(g_op_ticks, 0, sizeof g_op_ticks);
                    }
                    {
                        extern uint64_t g_ge_ticks[3];
                        static uint64_t l_ge[3];
                        double k = gf ? 1.0 / 60750.0 / gf : 0.0;
                        fprintf(stderr, "  GEPHASE run_list %.2f present %.2f finish %.2f ms per guest frame\n",
                                (double)(g_ge_ticks[0] - l_ge[0]) * k, (double)(g_ge_ticks[1] - l_ge[1]) * k,
                                (double)(g_ge_ticks[2] - l_ge[2]) * k);
                        for (int i = 0; i < 3; i++) l_ge[i] = g_ge_ticks[i];
                    }
                    fprintf(stderr, "  INPUT %.2f ms per guest frame\n", gf ? (double)(g_fe_input_us - last_input) / 1e3 / gf : 0.0);
                    last_input = g_fe_input_us;
                    fprintf(stderr, "  GUEST %u memory accesses per guest frame; frontend self %.2f ms, audio stream %.2f ms per guest frame\n",
                            gf ? (g_guest_mem_ticks - last_mt) / gf : 0,
                            gf ? (double)(g_fe_self_us - last_self) / 1e3 / gf : 0.0,
                            gf ? (double)(g_audio_pull_us - last_audio) / 1e3 / gf : 0.0);
                    last_mt = g_guest_mem_ticks;
                    {
                        extern unsigned g_guest_stores;
                        extern uint32_t g_run_draws[8], g_run_n[8];
                        static unsigned last_st;
                        fprintf(stderr, "  GUESTSTORES %u per guest frame; hit runs (runs/draws per frame) by length 1:%u/%u 2-3:%u/%u 4-7:%u/%u 8-15:%u/%u 16-31:%u/%u 32-63:%u/%u 64+:%u/%u\n",
                                gf ? (g_guest_stores - last_st) / gf : 0,
                                gf ? g_run_n[0] / gf : 0, gf ? g_run_draws[0] / gf : 0, gf ? g_run_n[1] / gf : 0, gf ? g_run_draws[1] / gf : 0,
                                gf ? g_run_n[2] / gf : 0, gf ? g_run_draws[2] / gf : 0, gf ? g_run_n[3] / gf : 0, gf ? g_run_draws[3] / gf : 0,
                                gf ? g_run_n[4] / gf : 0, gf ? g_run_draws[4] / gf : 0, gf ? g_run_n[5] / gf : 0, gf ? g_run_draws[5] / gf : 0,
                                gf ? g_run_n[6] / gf : 0, gf ? g_run_draws[6] / gf : 0);
                        last_st = g_guest_stores;
                        memset(g_run_draws, 0, sizeof g_run_draws);
                        memset(g_run_n, 0, sizeof g_run_n);
                    }
                    last_self = g_fe_self_us;
                    last_audio = g_audio_pull_us;
                }
                {
                    extern int g_ab_fast, g_ab_hot, g_ab_list;
                    fprintf(stderr, "  ABMODE the window above ran with the fast draw path %s, its state cache %s, the fast list loop %s\n", g_ab_fast ? "ON" : "OFF",
                            g_ab_hot ? "ON" : "OFF", g_ab_list ? "ON" : "OFF");
#ifdef EXP_GXLIGHT
                    {
                        extern int g_ab_lit;
                        fprintf(stderr, "  ABLIT the window above had the %s light the fighters\n", g_ab_lit ? "chip" : "CPU");
#ifdef EXP_ABLIT
                        g_ab_lit = !g_ab_lit;
#endif
                    }
                    {
                        extern int g_ab_rigid;
                        fprintf(stderr, "  ABRIGID the window above had the %s transform the single-bone draws\n", g_ab_rigid ? "chip" : "CPU");
#ifdef EXP_ABRIGID
                        g_ab_rigid = !g_ab_rigid;
#endif
                    }
#endif
#ifdef EXP_ABCOLFLAT
                    {
                        extern int g_ab_colflat;
                        fprintf(stderr, "  ABCOLFLAT the window above had the vertex-colour skinned draws lit by the %s code\n", g_ab_colflat ? "regrouped" : "light_vertex");
                        g_ab_colflat = !g_ab_colflat;
                    }
#endif
#ifdef EXP_ABREFRESH
                    {
                        extern int g_ab_refresh;
                        fprintf(stderr, "  ABREFRESH the window above had the alpha-only cache differences as %s\n", g_ab_refresh ? "hits" : "misses");
                        g_ab_refresh = !g_ab_refresh;
                    }
#endif
#ifdef EXP_ABPIPE
                    {
                        extern int g_ab_pipe;
                        fprintf(stderr, "  ABPIPE the window above had the lit skinned draws written by the kernel %s\n", g_ab_pipe ? "to the pipe" : "to the staging buffer");
                        g_ab_pipe = !g_ab_pipe;
                    }
#endif
#ifdef EXP_ABCOLNONRM
                    {
                        extern int g_ab_colnonrm;
                        fprintf(stderr, "  ABCOLNONRM the window above had the colour skinned draws without normals on the %s path\n", g_ab_colnonrm ? "whole-draw" : "general");
                        g_ab_colnonrm = !g_ab_colnonrm;
                    }
#endif
#ifdef EXP_ABSKINCOL
                    {
                        extern int g_ab_skincol;
                        fprintf(stderr, "  ABSKINCOL the window above had the vertex-colour skinned draws on the %s path\n", g_ab_skincol ? "whole-draw" : "general");
                        g_ab_skincol = !g_ab_skincol;
                    }
#endif
#ifdef EXP_ABRECT
                    {
                        extern int g_ab_rect;
                        extern uint32_t g_gx_epoch;
                        fprintf(stderr, "  ABRECT the window above %s the chip's face culling to the rectangles\n", g_ab_rect ? "did not apply" : "applied");
                        g_ab_rect = !g_ab_rect;
                        g_gx_epoch++;   /* the chip's setup is made again with the other culling */
                    }
#endif
#ifdef EXP_ABZBIAS
                    {
                        extern int g_ab_zbias;
                        extern uint32_t g_gx_epoch;
                        fprintf(stderr, "  ABZBIAS the window above %s the depth offset to the blended draws that do not write depth\n", g_ab_zbias ? "applied" : "did not apply");
                        g_ab_zbias = !g_ab_zbias;
                        g_gx_epoch++;   /* the chip's setup is made again with the other state */
                    }
#endif
#ifdef EXP_ABSTENCIL
                    {
                        extern int g_ab_stencil;
                        fprintf(stderr, "  ABSTENCIL the window above %s the draws with the NOTEQUAL stencil test\n", g_ab_stencil ? "dropped" : "kept");
                        g_ab_stencil = !g_ab_stencil;
                    }
#endif
#ifdef EXP_ABSND
                    {
                        extern int g_ab_snd;
                        fprintf(stderr, "  ABSND the window above had the %s sound mixer and stream\n", g_ab_snd ? "fast" : "old");
                        g_ab_snd = !g_ab_snd;
                    }
#endif
#ifdef EXP_ABBONE
                    {
                        extern int g_ab_bone;
                        fprintf(stderr, "  ABBONE the window above took the bone matrix runs %s\n", g_ab_bone ? "in the fast command loop" : "in the general code");
                        g_ab_bone = !g_ab_bone;
                    }
#endif
#ifdef EXP_ABMAT
                    {
                        extern int g_ab_mat;
                        fprintf(stderr, "  ABMAT the window above took the world, view and projection matrix words %s\n", g_ab_mat ? "in the fast command loop" : "in the general code");
                        g_ab_mat = !g_ab_mat;
                    }
#endif
#ifdef EXP_ABLIST
                    g_ab_list = !g_ab_list;
#endif
#ifdef EXP_AB
                    g_ab_hot = !g_ab_hot;
#endif
                }
                {
                    extern uint64_t g_fe_part_us[4];
                    static uint64_t l_part[4];
                    fprintf(stderr, "  FEPARTS audio_update %.2f video_flip %.2f ms per guest frame (%u frontend frames)\n",
                            gf ? (double)(g_fe_part_us[1] - l_part[1]) / 1e3 / gf : 0.0,
                            gf ? (double)(g_fe_part_us[2] - l_part[2]) / 1e3 / gf : 0.0, g_frontend_frames - last_fe);
                    for (int i = 0; i < 4; i++) l_part[i] = g_fe_part_us[i];
                }
                {
                    extern uint32_t g_prim_depth[2][6], g_cmds_depth[2];
                    fprintf(stderr, "  PRIMDEPTH per frame, top level: skinned %u hit %u miss %u other %u 2D %u | in a CALL: skinned %u hit %u miss %u other %u 2D %u | commands %u top, %u in calls\n",
                            gf ? g_prim_depth[0][0] / gf : 0, gf ? g_prim_depth[0][1] / gf : 0, gf ? g_prim_depth[0][2] / gf : 0,
                            gf ? (g_prim_depth[0][3] + g_prim_depth[0][4]) / gf : 0, gf ? g_prim_depth[0][5] / gf : 0,
                            gf ? g_prim_depth[1][0] / gf : 0, gf ? g_prim_depth[1][1] / gf : 0, gf ? g_prim_depth[1][2] / gf : 0,
                            gf ? (g_prim_depth[1][3] + g_prim_depth[1][4]) / gf : 0, gf ? g_prim_depth[1][5] / gf : 0,
                            gf ? g_cmds_depth[0] / gf : 0, gf ? g_cmds_depth[1] / gf : 0);
                    memset(g_prim_depth, 0, sizeof g_prim_depth);
                    memset(g_cmds_depth, 0, sizeof g_cmds_depth);
                }
#ifdef TKDR_WATCH
                {
                    extern uint32_t g_wtp_cnt[2][2][2][2];
                    extern struct { uint32_t ra, n; } g_wtp_w[48];
                    uint32_t tot = 0;
                    for (int a = 0; a < 16; a++) tot += ((uint32_t *)g_wtp_cnt)[a];
                    fprintf(stderr, "  WATCH plain draws %u: own 256B blocks written since last frame start: hit %u miss %u | clean: hit %u miss %u | 1KiB neighbours dirty but own clean %u, 4KiB %u\n",
                            tot, g_wtp_cnt[1][0][0][1] + g_wtp_cnt[1][1][0][1] + g_wtp_cnt[1][1][1][1] + g_wtp_cnt[1][0][1][1],
                            g_wtp_cnt[1][0][0][0] + g_wtp_cnt[1][1][0][0] + g_wtp_cnt[1][1][1][0] + g_wtp_cnt[1][0][1][0],
                            g_wtp_cnt[0][0][0][1] + g_wtp_cnt[0][0][1][1] + g_wtp_cnt[0][1][0][1] + g_wtp_cnt[0][1][1][1],
                            g_wtp_cnt[0][0][0][0] + g_wtp_cnt[0][0][1][0] + g_wtp_cnt[0][1][0][0] + g_wtp_cnt[0][1][1][0],
                            g_wtp_cnt[0][1][0][0] + g_wtp_cnt[0][1][0][1] + g_wtp_cnt[0][1][1][0] + g_wtp_cnt[0][1][1][1],
                            g_wtp_cnt[0][0][1][0] + g_wtp_cnt[0][0][1][1] + g_wtp_cnt[0][1][1][0] + g_wtp_cnt[0][1][1][1]);
                    for (int k = 0; k < 48 && g_wtp_w[k].n; k++)
                        fprintf(stderr, "    WRITER %08X %u\n", g_wtp_w[k].ra, g_wtp_w[k].n);
                    memset(g_wtp_cnt, 0, sizeof g_wtp_cnt);
                    memset(g_wtp_w, 0, sizeof g_wtp_w);
                }
#endif
                { extern void guest_prof_report(uint32_t); guest_prof_report(gf); }
                { extern void vfpu_prof_report(uint32_t); vfpu_prof_report(gf); }
                { extern void sound_prof_report(uint32_t); sound_prof_report(gf); }
                hle_prof_report(gf);
            }
            last_t = t;
            last_ge = g_gpu_busy_us;
            last_busy = g_fe_busy_us;
            last_fe = g_frontend_frames;
            last_guest = g_frames_presented;
        }
#endif
#ifdef TKDR_AUTOPILOT
        { extern uint64_t g_fe_input_us; uint64_t i0 = wii_time_us(); wii_input_scan(); g_fe_input_us += wii_time_us() - i0; }
#else
        wii_input_scan();
#endif
        g_buttons = wii_input_buttons();
#if defined(TKDR_AUTOPILOT)
        {
            /* Test-only autopilot for the menu/card corpus: Dolphin's
               PostMessage path cannot drive its DInput keyboard backend.
               Build with EXTRA_CFLAGS=-DTKDR_AUTOPILOT for this path.  It is
               independent of HLE_TRACE, so the same demo can be built at
               release speed without the debug probes. */
            static uint64_t auto_cross, auto_start, auto_next, auto_end, auto_right, auto_tutorial;
            uint64_t t = wii_time_us();
            /* AUTOPILOT_SHIFT_MS moves the whole script, which picks a different
               stage and different fighters: a way to sample other scenes. */
            const uint64_t shift = (uint64_t)AUTOPILOT_SHIFT_MS * 1000ull;
            if (!auto_cross) auto_cross = t + 5ull * 1000000ull + shift;
            if (!auto_start) auto_start = t + 30ull * 1000000ull + shift;
            if (!auto_next) auto_next = t + 37ull * 1000000ull + shift;
            if (!auto_end) auto_end = t + 45ull * 1000000ull + shift;
            if (!auto_right) auto_right = t + 57ull * 1000000ull + shift;
            if (!auto_tutorial) auto_tutorial = t + 59ull * 1000000ull + shift;
#ifdef AUTOPILOT_KEYS
            /* AUTOPILOT_KEYS: the buttons come from a list instead of the fixed menu script below.  "ap_keys.inc"
               (found through -I) holds { start_ms, length_ms, buttons } rows, the times from the first frontend
               frame; tools/perf/autopilot_keys.ps1 writes it.  Buttons are the PSP's: Start 0008, Up 0010, Right
               0020, Down 0040, Left 0080, Triangle 1000, Circle 2000, Cross 4000, Square 8000. */
            {
                static const struct { uint32_t at_ms, len_ms; uint16_t mask; } keys[] = {
#include "ap_keys.inc"
                };
                static uint64_t t0;
                if (!t0) t0 = t;
                const uint32_t ms = (uint32_t)((t - t0) / 1000ull);
                for (unsigned k = 0; k < sizeof keys / sizeof keys[0]; k++)
                    if (ms >= keys[k].at_ms && ms < keys[k].at_ms + keys[k].len_ms) g_buttons |= keys[k].mask;
            }
#else
            if (t >= auto_cross && t < auto_cross + 2ull * 1000000ull) g_buttons |= 0x4000;
            if (t >= auto_start && t < auto_start + 5ull * 1000000ull) g_buttons |= 0x0008;
            if (t >= auto_next && t < auto_next + 2ull * 1000000ull) g_buttons |= 0x4000;
            if (t >= auto_end && t < auto_end + 2ull * 1000000ull) g_buttons |= 0x4000;
#ifndef AUTOPILOT_STEVE
            if (t >= auto_right && t < auto_right + 1ull * 1000000ull) g_buttons |= 0x0020;
            if (t >= auto_tutorial && t < auto_tutorial + 2ull * 1000000ull) g_buttons |= 0x4000;
#endif
#endif
            /* Then tap Cross every three seconds: it picks both fighters and
               the stage, however long each screen took to appear, and the
               run reaches a fight (where Cross is just an attack). */
            int hold = 0;
#ifdef AUTOPILOT_STEVE
            /* AUTOPILOT_STEVE: the fighter grid is up from about 50 s in; no Cross
               (and no Right) until 62 s, and the cursor goes from where it starts
               (Lili) to Steve: left, left, down, at 54, 56 and 58 s.  The first
               Cross after that, at 63 s, picks him. */
            {
                const uint64_t h0 = auto_end + 2ull * 1000000ull + 500000ull;
                if (t >= h0 && t < auto_tutorial + 3ull * 1000000ull) {
                    uint64_t s = t - h0;
                    hold = 1;
                    if ((s >= 6500000ull && s < 6800000ull) || (s >= 8500000ull && s < 8800000ull)) g_buttons |= 0x0080;
                    if (s >= 10500000ull && s < 10800000ull) g_buttons |= 0x0040;
                }
            }
#endif
#ifndef AUTOPILOT_KEYS
            if (!hold && t >= auto_tutorial + 4ull * 1000000ull &&
                (t - auto_tutorial) % 3000000ull < 400000ull) g_buttons |= 0x4000;
#endif
#ifdef AUTOPILOT_MASH
            /* AUTOPILOT_MASH: what a person does in a fight, which the plain script (one Cross every 3 s, the
               other fighter idle, so nothing ever lands) does not: walk up, string punches and kicks, crouch and
               block.  From AUTOPILOT_MASH_START_S seconds after the script's last menu press it alternates 10 s
               of the plain script with 10 s of this, so a report window of one kind sits next to one of the other
               in the same fight.  Player 1 starts on the left, so Right walks toward the opponent. */
            {
                static const uint16_t chain[8] = {0x8000, 0x1000, 0x4000, 0x2000, 0x1000, 0x8000, 0x2000, 0x4000};
                const uint64_t m0 = auto_tutorial + (uint64_t)AUTOPILOT_MASH_START_S * 1000000ull;
                if (!hold && t >= m0 && ((t - m0) / 10000000ull) % 2 == 1) {
                    const uint32_t ms = (uint32_t)(((t - m0) % 10000000ull) / 1000ull) % 4000u;   /* a 4 s pattern */
                    uint32_t b = 0;
                    if (ms < 900) b = 0x0020;                                              /* walk in */
                    else if (ms < 2900) {                                                   /* a string of attacks, forward held half the time */
                        const uint32_t slot = (ms - 900) / 130;
                        if ((ms - 900) % 130 < 70) b = chain[slot & 7];
                        if (slot & 1) b |= 0x0020;
                    } else if (ms < 3400) b = 0x0040 | 0x0080;                              /* crouch, back: block */
                    else b = 0x0040 | 0x2000;                                               /* a low kick */
                    g_buttons = (g_buttons & ~0xFFF0u) | b;
                    g_mash_frames++;
                }
            }
#endif
#ifdef EXP_DRAWTRACE
            {
                const double c = (double)(t - auto_cross) / 1e6 + 5.0 + (double)AUTOPILOT_SHIFT_MS / 1000.0;
                g_dt_window = c >= DT_FROM_S && c < DT_TO_S;
            }
#endif
            {
                static uint32_t last_pressed;
                uint32_t now_pressed = g_buttons & 0xFFF9u;
                if (now_pressed != last_pressed) {
                    fprintf(stderr, "AUTOPILOT t=%.1f s buttons %04X\n", (double)(t - auto_cross) / 1e6 + 5.0 + (double)AUTOPILOT_SHIFT_MS / 1000.0, now_pressed);
                    last_pressed = now_pressed;
                }
            }
        }
#endif
        wii_input_analog(&g_analog_x, &g_analog_y);
#ifdef TKDR_AUTOPILOT
        { extern uint64_t g_fe_part_us[4]; uint64_t a0 = wii_time_us(); wii_audio_update(); g_fe_part_us[1] += wii_time_us() - a0; }
#else
        wii_audio_update();
#endif

        /* Checked before the present paths: the GX path ends each frame with
           `continue`, so a check at the bottom of the loop was never reached
           with the (default) GX renderer and HOME did nothing. */
        if (wii_input_home_pressed()) {
            /* A real power-button press is held down; a stray one-shot event
               must not end the run. */
            if (++home_frames >= 30) {
                wii_log("WII: HOME pressed, exiting\n");
                wii_audio_shutdown();
                exit(0);
            }
        } else {
            home_frames = 0;
        }
#ifdef HLE_TRACE
        { extern void hle_sample_guest_fn(void); hle_sample_guest_fn(); }
#endif

        /* Only present when the picture has actually changed.  The
           frontend runs at about 21 Hz and the guest draws at about 6 Hz,
           so most presents were re-uploading the same picture: a full
           framebuffer conversion, a cache flush and a texture cache
           invalidation, for nothing. */
        uint32_t guest_frame = g_frames_presented;
        uint32_t drew = g_raster_draws;
        if (wii_gxscene_enabled()) {
            /* The chip draws the picture and copies it to a display buffer
               on its own; the newest copy that has landed goes on screen. */
            uint32_t gx_presents = g_gx_presents;
            if (gx_presents != s_last_gx_presents) {
                s_last_gx_presents = gx_presents;
                s_gx_idle = 0;
            } else if (s_gx_idle < 1000) {
                s_gx_idle++;
            }
#ifdef HLE_TRACE
            if (s_gx_idle == 13) {
                extern volatile int g_gx_stage;
                extern void wii_gxscene_chip_status(int *, int *, int *, unsigned *);
                int lag, cmd_idle, read_idle;
                unsigned fifo;
                wii_gxscene_chip_status(&lag, &cmd_idle, &read_idle, &fifo);
                fprintf(stderr, "GXIDLE stage=%d lists=%u presents=%u lag=%d cmd=%d read=%d fifo=%u\n",
                        g_gx_stage, g_gx_lists, g_gx_presents, lag, cmd_idle, read_idle, fifo);
            }
#endif

            /* The game leaves the chip behind for the movies and for menus
               that it composes in its own framebuffer.  Once no new copy has
               arrived for a few frames, show the guest framebuffer directly.
               This writes a display buffer with the CPU and issues no GX
               commands, so it does not race the GE thread's command stream. */
            const int movie = hle_movie_active();
            static unsigned s_movie_last_seq;
            if (movie) {
                /* The movie is decoded into its own picture, not drawn by the
                   GE.  Present each decoded frame; the XFB keeps showing the
                   last one, so there is no need to recopy it at the frontend's
                   higher rate. */
                const uint16_t *movie_pixels = hle_movie_pixels();
                const unsigned seq = hle_movie_frame_seq();
                if (movie_pixels && hle_movie_frame_ready() && seq != s_movie_last_seq) {
                    s_movie_last_seq = seq;
                    wii_video_present_movie(movie_pixels, 640, 362);
                }
                frames++;
                continue;
            }
            const uint32_t write_tick = g_gpu_write_tick;
            if (s_gx_idle > 12 && (guest_frame != s_last_guest_frame || write_tick != s_last_write_tick)) {
                s_last_guest_frame = guest_frame;
                s_last_drew = drew;
                s_last_write_tick = write_tick;
                g_display_fb_addr = g_display.fb_addr;  // the guest may read it back
                int scale = gpu_scanout_scaled(g_display.fb_addr, g_display.stride,
                                               g_display.format, s_pixels, 1);
                if (!scale) frontend_scanout(s_pixels);
                wii_video_present_cpu(s_pixels, 480, 272);
                frames++;
#ifdef HLE_TRACE
                frontend_dump_periodic(frames);
                frontend_gx_perf(frames);
                if (frames % 60 == 0) {
                    fprintf(stderr, "GXSTATE cpu f=%u guest=%u gp=%u idle=%u prims=%llu "
                            "fb=%08X fmt=%d stride=%d\n", frames, g_frames_presented,
                            gx_presents, s_gx_idle, (unsigned long long)g_gxr_prims,
                            g_display.fb_addr, g_display.format, g_display.stride);
                }
#endif
                if (frames == 1 || frames == 10 || frames % 60 == 0) {
                    extern uint64_t g_iso_read_bytes, g_iso_read_calls;
                    wii_log("WII gx cpu: frame %u, guest frames=%u, gx prims=%llu, "
                            "presents=%u, host threads=%d, iso=%llu MiB/%llu reads\n",
                            frames, g_frames_presented, (unsigned long long)g_gxr_prims,
                            gx_presents, g_host_threads_started,
                            (unsigned long long)(g_iso_read_bytes >> 20),
                            (unsigned long long)g_iso_read_calls);
                }
                continue;
            }

            s_last_guest_frame = guest_frame;
            s_last_drew = drew;
#ifdef TKDR_AUTOPILOT
            { extern uint64_t g_fe_part_us[4]; uint64_t f0 = wii_time_us(); wii_video_flip(); g_fe_part_us[2] += wii_time_us() - f0; }
#else
            wii_video_flip();
#endif
            frames++;
#ifdef HLE_TRACE
            frontend_dump_periodic(frames);
            frontend_gx_perf(frames);
            if (frames % 60 == 0) {
                fprintf(stderr, "GXSTATE gx f=%u guest=%u gp=%u idle=%u prims=%llu "
                        "fb=%08X fmt=%d stride=%d\n", frames, g_frames_presented,
                        gx_presents, s_gx_idle, (unsigned long long)g_gxr_prims,
                        g_display.fb_addr, g_display.format, g_display.stride);
            }
#endif
            if (frames == 1 || frames == 10 || frames % 60 == 0) {
                extern uint64_t g_iso_read_bytes, g_iso_read_calls;
                wii_log("WII gx: frame %u, guest frames=%u, gx prims=%llu, presents=%u, "
                        "host threads=%d, iso=%llu MiB/%llu reads\n",
                        frames, g_frames_presented, (unsigned long long)g_gxr_prims,
                        gx_presents, g_host_threads_started,
                        (unsigned long long)(g_iso_read_bytes >> 20),
                        (unsigned long long)g_iso_read_calls);
            }
            continue;
        }
        if (guest_frame != s_last_guest_frame || drew != s_last_drew) {
            s_last_guest_frame = guest_frame;
            s_last_drew = drew;
            g_display_fb_addr = g_display.fb_addr;  // the guest may read it back
            int scale = gpu_scanout_scaled(g_display.fb_addr, g_display.stride,
                                           g_display.format, s_pixels, 1);
            if (!scale) {
                frontend_scanout(s_pixels);
                scale = 1;
            }
            (void)scale;
#ifdef HLE_TRACE
            uint64_t t_present = hle_time_us();
            wii_video_present(s_pixels, 480, 272);
            g_present_us += hle_time_us() - t_present;
#else
            wii_video_present(s_pixels, 480, 272);
#endif
        }

        frames++;
        if (frames == 1 || frames == 10 || frames % 60 == 0) {
            extern uint32_t g_frames_presented;
            extern uint64_t g_iso_read_bytes, g_iso_read_calls;
            wii_log("WII frontend: frame %u, guest frames=%u, HLE calls=%u, "
                    "host threads=%d, iso=%llu MiB/%llu reads\n",
                    frames, g_frames_presented, g_hle_calls,
                    g_host_threads_started,
                    (unsigned long long)(g_iso_read_bytes >> 20),
                    (unsigned long long)g_iso_read_calls);
#ifdef HLE_TRACE
            {
                extern uint64_t g_switch_us, g_prim_calls, g_prim_count_clipped;
                extern uint64_t g_iso_seek_us, g_iso_fread_us, g_iso_reads;
                extern uint64_t g_iso_total_us;
                extern uint64_t g_mem_accesses;
                                extern uint64_t g_gpu_busy_us;
                extern uint64_t g_prim_us, g_flush_wait_us;
                extern uint64_t g_worker_px[64];
                extern uint32_t guest_bad_access_count(void);
                extern uint32_t guest_last_bad_access(void);
                extern void sysmem_debug_dump(void);
                uint32_t bad = guest_bad_access_count();
                uint32_t last = guest_last_bad_access();
                uint64_t px = 0;
                for (int i = 0; i < 64; i++) px += g_worker_px[i];
                wii_log("  iso total=%llu ms (seek=%llu read=%llu) reads=%llu, "
                        "ge=%llu ms, prim=%llu ms, flushwait=%llu ms, mem=%lluM, "
                        "raster px=%lluM\n",
                        (unsigned long long)(g_iso_total_us / 1000),
                        (unsigned long long)(g_iso_seek_us / 1000),
                        (unsigned long long)(g_iso_fread_us / 1000),
                        (unsigned long long)g_iso_reads,
                        (unsigned long long)(g_gpu_busy_us / 1000),
                        (unsigned long long)(g_prim_us / 1000),
                        (unsigned long long)(g_flush_wait_us / 1000),
                        (unsigned long long)(g_mem_accesses / 1000000),
                        (unsigned long long)(px / 1000000));
                extern uint64_t g_prim_count, g_px_count, g_setup_us, g_pixel_us;
                extern uint64_t g_tex_decodes, g_tex_decode_us, g_tex_decode_texels;
                extern uint64_t g_clut_hash_us, g_clut_hash_calls;
                extern uint64_t g_pixstate_us, g_vertex_us;
                extern uint64_t g_sample_us, g_shade_us, g_blend_us;
                extern uint64_t g_emit_us;
                extern uint64_t g_vformat_us, g_beginstate_us;
                extern uint64_t g_fb_fast, g_fb_slow, g_z_fast, g_z_slow;
                extern uint64_t g_persp_tris, g_through_tris;
                extern uint64_t g_blend_px, g_tex_px;
                wii_log("  raster: px=%lluM texdec=%llu (%lluM texels, %llu ms) "
                        "cluthash=%llu/%llums prims=%llu tri_setup=%llums "
                        "pixstate=%llums verts=%llums "
                        "sample=%llums blend=%llums emit=%llums "
                        "vformat=%llums beginstate=%llums\n",
                        (unsigned long long)(px / 1000000),
                        (unsigned long long)g_tex_decodes,
                        (unsigned long long)(g_tex_decode_texels / 1000000),
                        (unsigned long long)(g_tex_decode_us / 1000),
                        (unsigned long long)g_clut_hash_calls,
                        (unsigned long long)(g_clut_hash_us / 1000),
                        (unsigned long long)g_prim_count,
                        (unsigned long long)(g_setup_us / 1000),
                        (unsigned long long)(g_pixstate_us / 1000),
                        (unsigned long long)(g_vertex_us / 1000),
                        (unsigned long long)(g_sample_us / 1000),
                        (unsigned long long)(g_blend_us / 1000),
                        (unsigned long long)(g_emit_us / 1000),
                        (unsigned long long)(g_vformat_us / 1000),
                        (unsigned long long)(g_beginstate_us / 1000));
                wii_log("  half-res expansion: %llu ms in %llu calls\n",
                        (unsigned long long)(g_half_upsample_us / 1000),
                        (unsigned long long)g_half_upsample_calls);
                wii_log("  gx primitives: %llu\n", (unsigned long long)g_gxr_prims);
                wii_log("  frontend present: %llu ms (presents=%u)\n",
                        (unsigned long long)(g_present_us / 1000), s_presents);
                {
                    extern uint64_t g_prim_type[8], g_rect_us, g_line_us, g_point_us;
                    {
                    extern uint64_t g_sample_linear, g_sample_point;
                    wii_log("  samples: linear=%llu point=%llu\n",
                            (unsigned long long)g_sample_linear,
                            (unsigned long long)g_sample_point);
                }
                {
                    extern uint64_t g_feat_texfunc[8], g_feat_blend[8], g_feat_blsrc[16],
                                        g_feat_bldst[16], g_feat_atfunc[8], g_feat_zfunc[8],
                                        g_feat_texfmt[16], g_feat_alpha_test, g_feat_z_test,
                                        g_feat_z_write, g_feat_blend_on, g_feat_through,
                                        g_feat_no_tex, g_feat_rgba, g_feat_dbl;
                    wii_log("  features: tex=%llu notex=%llu rgba=%llu dbl=%llu through=%llu\n",
                            (unsigned long long)(g_feat_texfunc[0] + g_feat_texfunc[1] + g_feat_texfunc[2] + g_feat_texfunc[3]),
                            (unsigned long long)g_feat_no_tex, (unsigned long long)g_feat_rgba,
                            (unsigned long long)g_feat_dbl, (unsigned long long)g_feat_through);
                    wii_log("  texfunc: mod=%llu decal=%llu env=%llu repl=%llu add=%llu\n",
                            (unsigned long long)g_feat_texfunc[0], (unsigned long long)g_feat_texfunc[1],
                            (unsigned long long)g_feat_texfunc[2], (unsigned long long)g_feat_texfunc[3],
                            (unsigned long long)g_feat_texfunc[4]);
                    wii_log("  texfmt: 4bit=%llu 8bit=%llu 16bit=%llu\n",
                            (unsigned long long)g_feat_texfmt[4], (unsigned long long)g_feat_texfmt[5],
                            (unsigned long long)g_feat_texfmt[6]);
                    wii_log("  blend: on=%llu eq_add=%llu src_a=%llu dst_inv_a=%llu\n",
                            (unsigned long long)g_feat_blend_on, (unsigned long long)g_feat_blend[0],
                            (unsigned long long)g_feat_blsrc[2], (unsigned long long)g_feat_bldst[3]);
                    wii_log("  tests: alpha=%llu z=%llu zwrite=%llu\n",
                            (unsigned long long)g_feat_alpha_test, (unsigned long long)g_feat_z_test,
                            (unsigned long long)g_feat_z_write);
                }
                {
                    extern uint32_t g_gxr_paths[3], g_gxr_feat[14], g_gxr_wm[4];
                    wii_log("  gxr plan: native=%u approx=%u skip=%u\n",
                            g_gxr_paths[0], g_gxr_paths[1], g_gxr_paths[2]);
                    wii_log("  gxr features: through=%u tex=%u atest=%u ctest=%u "
                            "stencil=%u simple_stencil=%u logic=%u dither=%u "
                            "blend=%u dbl_alpha=%u fixed_src=%u mask=%u "
                            "alpha_only=%u fixed_dst=%u\n",
                            g_gxr_feat[0], g_gxr_feat[1], g_gxr_feat[2],
                            g_gxr_feat[3], g_gxr_feat[4], g_gxr_feat[5],
                            g_gxr_feat[6], g_gxr_feat[7], g_gxr_feat[8],
                            g_gxr_feat[9], g_gxr_feat[10], g_gxr_feat[11],
                            g_gxr_feat[12], g_gxr_feat[13]);
                }
                wii_log("  prim types: pts=%llu lines=%llu lstrip=%llu tris=%llu "
                            "tstrip=%llu tfan=%llu rects=%llu\n",
                            (unsigned long long)g_prim_type[0], (unsigned long long)g_prim_type[1],
                            (unsigned long long)g_prim_type[2], (unsigned long long)g_prim_type[3],
                            (unsigned long long)g_prim_type[4], (unsigned long long)g_prim_type[5],
                            (unsigned long long)g_prim_type[6]);
                    wii_log("  rect=%llu ms line=%llu ms point=%llu ms\n",
                            (unsigned long long)(g_rect_us / 1000),
                            (unsigned long long)(g_line_us / 1000),
                            (unsigned long long)(g_point_us / 1000));
                }
                wii_log("  draw_prim: calls=%llu switch=%llu ms clipped=%llu\n",
                        (unsigned long long)g_prim_calls,
                        (unsigned long long)(g_switch_us / 1000),
                        (unsigned long long)g_prim_count_clipped);
                {
                    extern uint64_t g_vid_conv_us, g_vid_store_us, g_vid_inval_us, g_vid_copy_us;
                    wii_log("  present parts: conv=%llu ms store=%llu ms inval=%llu ms copy=%llu ms\n",
                            (unsigned long long)(g_vid_conv_us / 1000),
                            (unsigned long long)(g_vid_store_us / 1000),
                            (unsigned long long)(g_vid_inval_us / 1000),
                            (unsigned long long)(g_vid_copy_us / 1000));
                }
                wii_log("  fb fast=%llu slow=%llu, z fast=%llu slow=%llu\n",
                        (unsigned long long)g_fb_fast,
                        (unsigned long long)g_fb_slow,
                        (unsigned long long)g_z_fast,
                        (unsigned long long)g_z_slow);
                wii_log("  pixel loop: %llu ms for %lluM px\n",
                        (unsigned long long)(g_pixel_us / 1000),
                        (unsigned long long)(g_px_count / 1000000));
                wii_log("  persp tris=%llu through tris=%llu, tex px=%lluM blend px=%lluM\n",
                        (unsigned long long)g_persp_tris,
                        (unsigned long long)g_through_tris,
                        (unsigned long long)(g_tex_px / 1000000),
                        (unsigned long long)(g_blend_px / 1000000));
                wii_log("  unmapped guest accesses: %u (last %08X)\n", bad, last);
                sysmem_debug_dump();
            }
#endif
#ifdef HLE_TRACE
            /* Rare: the dump is verbose and every line goes through the
               emulator's OSReport channel. */
            if (frames % 600 == 0) {
                extern void sched_dump_threads(const char *tag);
                extern void sched_dump_waits(void);
                extern void hle_dump_pc_hist(void);
                sched_dump_threads("periodic");
                sched_dump_waits();
                hle_dump_pc_hist();
            }
#endif
        }
    }
}
