// sceDisplay, sceGe_user (list bookkeeping), sceCtrl, sceUmdUser, sceRtc.
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "hle.h"
#include "../gpu/gpu.h"

// ---------------------------------------------------------------- display

DisplayState g_display = {.fb_addr = 0x04000000u, .stride = 512, .format = 3};

void sceDisplaySetMode(Ctx *c) { RET(0); }

uint32_t g_frames_presented;

/* The game double buffers its display.  Keep the addresses it has presented
   so the rasteriser can tell a display surface from a render target (the
   movie player renders into textures, which the chip cannot emulate without
   render-to-texture support). */
static uint32_t s_disp_addrs[4] = {0x04000000u, 0, 0, 0};
static int s_disp_n = 1;

static void note_display_addr(uint32_t a) {
    for (int i = 0; i < s_disp_n; i++)
        if (s_disp_addrs[i] == a) return;
    if (s_disp_n < 4) s_disp_addrs[s_disp_n++] = a;
    else {
        memmove(&s_disp_addrs[0], &s_disp_addrs[1], sizeof(uint32_t) * 3);
        s_disp_addrs[3] = a;
    }
}

/* Debug: the addresses the guest has presented. */
int gpu_disp_info(uint32_t *a) {
    for (int i = 0; i < 4; i++) a[i] = i < s_disp_n ? s_disp_addrs[i] : 0;
    return s_disp_n;
}

/* A surface the guest has presented, by address in any of VRAM's mirrors.  The chip draws into its own frame buffer and never writes
   one of these, so a texture at such an address is not the picture the guest means. */
int gpu_is_presented_buffer(uint32_t a) {
    for (int i = 0; i < s_disp_n; i++)
        if (s_disp_addrs[i] && (s_disp_addrs[i] & 0x1FFFFFFFu) == (a & 0x1FFFFFFFu)) return 1;
    return 0;
}

int gpu_is_display_target(uint32_t fb) {
    for (int i = 0; i < s_disp_n; i++)
        if (s_disp_addrs[i] == fb) return 1;
    /* The other half of a double buffer is one surface away. */
    uint32_t bpp = g_display.format == 3 ? 4u : 2u;
    uint32_t size = (uint32_t)g_display.stride * 272u * bpp;
    for (int i = 0; i < s_disp_n; i++)
        if (fb == s_disp_addrs[i] + size || fb == s_disp_addrs[i] - size) return 1;
    return 0;
}

void sceDisplaySetFrameBuf(Ctx *c) {
    g_frames_presented++;
#ifdef TKDR_WATCH
    { extern void wtp_frame(void); wtp_frame(); }
#endif
#ifdef HLE_TRACE
    /* Frame-pacing histogram.  This separates "the guest really needs more
       than 16.7 ms per frame" from "the guest occasionally misses a vblank":
       a steady 22 ms stream is a different problem from alternating 16/33 ms
       intervals, and they need different fixes. */
    {
        static uint64_t last_us;
        static uint32_t n, h[5];
        uint64_t now = hle_time_us();
        if (last_us) {
            uint64_t dt = now - last_us;
            int b = dt < 17500 ? 0 : dt < 25000 ? 1 : dt < 34000 ? 2 : dt < 50000 ? 3 : 4;
            h[b]++;
            if (dt >= 25000) {
                extern uint64_t g_iso_read_bytes, g_iso_read_calls;
                extern uint64_t g_iso_total_us, g_io_total_us;
                extern uint64_t g_tex_decodes;
                extern volatile uint32_t g_gx_texconv;
                static unsigned spikes;
                if (g_frames_presented >= 1800 && spikes++ < 60)
                    fprintf(stderr, "SPIKE dt=%lluus guest=%u iso=%lluM/%llu isot=%llums iot=%llums "
                            "texdec=%llu gxconv=%u\n",
                            (unsigned long long)dt, g_frames_presented,
                            (unsigned long long)(g_iso_read_bytes >> 20),
                            (unsigned long long)g_iso_read_calls,
                            (unsigned long long)(g_iso_total_us / 1000),
                            (unsigned long long)(g_io_total_us / 1000),
                            (unsigned long long)g_tex_decodes, g_gx_texconv);
            }
        }
        last_us = now;
        if (++n % 600 == 0)
            fprintf(stderr, "FRAMETIME n=%u <17.5=%u 17.5-25=%u 25-34=%u 34-50=%u >50=%u\n",
                    n, h[0], h[1], h[2], h[3], h[4]);
    }
#endif
#ifdef TKDR_AUTOPILOT
    {   /* A guest frame of 50 ms or more (three refreshes): where the time went.  HITCH lines of the demo builds. */
        extern uint64_t g_ge_ticks[3], g_iso_read_bytes, g_iso_read_calls, g_iso_ticks;
        extern uint64_t guest_prof_total_ticks(void);
        extern void hle_prof_since(char *buf, unsigned n, int want);
        extern void guest_prof_frame(char *buf, unsigned n, int want);
        extern uint64_t g_iso_seek_ticks;
        static uint32_t last_tb, hitches;
        static uint64_t l_ge[3], l_gp, l_iso, l_isoc, l_isot, l_isosk;
        uint32_t now;
        __asm__ volatile("mftb %0" : "=r"(now));
        const uint32_t dt = now - last_tb;
        const int hit = last_tb && dt >= 3037500u && hitches < 400;
        char who[200], fns[200];
        hle_prof_since(who, sizeof who, hit);
        guest_prof_frame(fns, sizeof fns, hit);
        if (hit) {
            hitches++;
            const uint64_t gp = guest_prof_total_ticks();
            const uint64_t gcode = gp >= l_gp ? gp - l_gp : 0;   /* the report resets the sampler */
            fprintf(stderr, "HITCH guest=%u dt=%.0f ms: guest code %.0f ms, ge list %.0f present %.0f finish %.0f ms, iso %lluK in %llu reads took %.0f ms (seeks %.0f) | %s | %s\n",
                    g_frames_presented, (double)dt / 60750.0, (double)gcode / 60750.0, (double)(g_ge_ticks[0] - l_ge[0]) / 60750.0,
                    (double)(g_ge_ticks[1] - l_ge[1]) / 60750.0, (double)(g_ge_ticks[2] - l_ge[2]) / 60750.0,
                    (unsigned long long)((g_iso_read_bytes - l_iso) >> 10), (unsigned long long)(g_iso_read_calls - l_isoc), (double)(g_iso_ticks - l_isot) / 60750.0, (double)(g_iso_seek_ticks - l_isosk) / 60750.0, who, fns);
        }
        last_tb = now;
        l_gp = guest_prof_total_ticks();
        l_ge[0] = g_ge_ticks[0]; l_ge[1] = g_ge_ticks[1]; l_ge[2] = g_ge_ticks[2];
        l_iso = g_iso_read_bytes; l_isoc = g_iso_read_calls; l_isot = g_iso_ticks; l_isosk = g_iso_seek_ticks;
    }
#endif
    /* Presenting a frame does not change texture memory, so the texture
       cache stays valid; invalidating it here made the game re-decode every
       texture each frame, which dominated the run time. */
    if (ARG(0)) {
        g_display.fb_addr = ARG(0);
        g_display.stride = ARG(1);
        g_display.format = ARG(2);
        note_display_addr(ARG(0));
    }
    RET(0);
}

void sceDisplayWaitVblankStart(Ctx *c) { hle_wait_vblank(c, 0); }
void sceDisplayWaitVblankStartCB(Ctx *c) { hle_wait_vblank(c, 1); }
void sceDisplayGetVcount(Ctx *c) { RET(hle_vblank_count()); }

// ---------------------------------------------------------------- GE

void sceGeEdramGetAddr(Ctx *c) { RET(0x04000000u); }
void sceGeEdramGetSize(Ctx *c) { RET(0x00200000u); }

void sceGeSetCallback(Ctx *c) {
    gpu_set_callbacks(mem_r32(ARG(0)), mem_r32(ARG(0) + 4), mem_r32(ARG(0) + 8),
                      mem_r32(ARG(0) + 12));
    RET(1);
}

void sceGeListEnQueue(Ctx *c) { RET(gpu_enqueue(c, ARG(0), ARG(1), (int)ARG(2), ARG(3), 0)); }
void sceGeListEnQueueHead(Ctx *c) { RET(gpu_enqueue(c, ARG(0), ARG(1), (int)ARG(2), ARG(3), 1)); }
void sceGeListUpdateStallAddr(Ctx *c) { RET(gpu_update_stall(c, (int)ARG(0), ARG(1))); }
void sceGeListSync(Ctx *c) { RET(gpu_sync(c, (int)ARG(0), (int)ARG(1))); }
void sceGeDrawSync(Ctx *c) { RET(gpu_sync(c, -1, (int)ARG(0))); }

// ---------------------------------------------------------------- controller

uint32_t g_buttons;  // PSP button bits, set by the frontend
uint8_t g_analog_x = 128, g_analog_y = 128;

void sceCtrlSetSamplingCycle(Ctx *c) { RET(0); }
void sceCtrlSetSamplingMode(Ctx *c) { RET(0); }

void sceCtrlReadBufferPositive(Ctx *c) {
    uint32_t p = ARG(0), n = ARG(1) ? ARG(1) : 1;
    for (uint32_t k = 0; k < n; k++, p += 16) {
        memset(host(p), 0, 16);
        mem_w32(p, (uint32_t)hle_time_us());
        mem_w32(p + 4, g_buttons);
        mem_w8(p + 8, g_analog_x);
        mem_w8(p + 9, g_analog_y);
    }
#ifdef HLE_TRACE
    /* Report the buttons the guest actually receives, so a test can tell
       "the press never arrived" from "the guest ignored it". */
    static uint32_t last;
    if (g_buttons != last) {
        last = g_buttons;
        fprintf(stderr, "CTRL: buttons %04X\n", g_buttons);
    }
#endif
    RET(n);
}

// ---------------------------------------------------------------- UMD

void sceUmdCheckMedium(Ctx *c) { RET(1); }
void sceUmdActivate(Ctx *c) { RET(0); }
void sceUmdDeactivate(Ctx *c) { RET(0); }
void sceUmdWaitDriveStatCB(Ctx *c) { RET(0); }
void sceUmdRegisterUMDCallBack(Ctx *c) { RET(0); }

// ---------------------------------------------------------------- RTC
// Ticks are microseconds since 0001-01-01.

#define RTC_UNIX_EPOCH 62135596800000000ull

static uint64_t now_tick(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
#ifdef TKDR_FIXTIME
    tv.tv_sec = 1700000000; tv.tv_usec = 0;
#endif
    return RTC_UNIX_EPOCH + (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

static void write_dt(uint32_t p, uint64_t tick, int local) {
    uint64_t us = tick - RTC_UNIX_EPOCH;
    time_t t = (time_t)(us / 1000000u);
    struct tm tm;
    if (local) localtime_r(&t, &tm);
    else gmtime_r(&t, &tm);
    mem_w16(p, (uint16_t)(tm.tm_year + 1900));
    mem_w16(p + 2, (uint16_t)(tm.tm_mon + 1));
    mem_w16(p + 4, (uint16_t)tm.tm_mday);
    mem_w16(p + 6, (uint16_t)tm.tm_hour);
    mem_w16(p + 8, (uint16_t)tm.tm_min);
    mem_w16(p + 10, (uint16_t)tm.tm_sec);
    mem_w32(p + 12, (uint32_t)(us % 1000000u));
}

static uint64_t read_dt(uint32_t p) {
    struct tm tm = {0};
    tm.tm_year = mem_r16(p) - 1900;
    tm.tm_mon = mem_r16(p + 2) - 1;
    tm.tm_mday = mem_r16(p + 4);
    tm.tm_hour = mem_r16(p + 6);
    tm.tm_min = mem_r16(p + 8);
    tm.tm_sec = mem_r16(p + 10);
    tm.tm_isdst = 0;
#ifdef __wii__
    return RTC_UNIX_EPOCH + (uint64_t)mktime(&tm) * 1000000u + mem_r32(p + 12);
#else
    return RTC_UNIX_EPOCH + (uint64_t)timegm(&tm) * 1000000u + mem_r32(p + 12);
#endif
}

static void w64(uint32_t p, uint64_t v) {
    mem_w32(p, (uint32_t)v);
    mem_w32(p + 4, (uint32_t)(v >> 32));
}
static uint64_t r64(uint32_t p) { return mem_r32(p) | ((uint64_t)mem_r32(p + 4) << 32); }

void sceRtcGetCurrentTick(Ctx *c) { w64(ARG(0), now_tick()); RET(0); }
void sceRtcGetCurrentClock(Ctx *c) {
    write_dt(ARG(0), now_tick() + (uint64_t)(int64_t)(int32_t)ARG(1) * 60000000ll, 0);
    RET(0);
}
void sceRtcGetCurrentClockLocalTime(Ctx *c) { write_dt(ARG(0), now_tick(), 1); RET(0); }
void sceRtcGetTick(Ctx *c) { w64(ARG(1), read_dt(ARG(0))); RET(0); }
void sceRtcSetTick(Ctx *c) { write_dt(ARG(0), r64(ARG(1)), 0); RET(0); }
void sceRtcGetTime_t(Ctx *c) {
    mem_w32(ARG(1), (uint32_t)((read_dt(ARG(0)) - RTC_UNIX_EPOCH) / 1000000u));
    RET(0);
}
void sceRtcSetTime_t(Ctx *c) {
    write_dt(ARG(0), RTC_UNIX_EPOCH + (uint64_t)ARG(1) * 1000000u, 0);
    RET(0);
}
void sceRtcConvertUtcToLocalTime(Ctx *c) {
#ifdef __wii__
    /* newlib on libogc has no tm_gmtoff; the console's clock is UTC. */
    w64(ARG(1), r64(ARG(0)));
#else
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    w64(ARG(1), r64(ARG(0)) + (uint64_t)(int64_t)lt.tm_gmtoff * 1000000);
#endif
    RET(0);
}
