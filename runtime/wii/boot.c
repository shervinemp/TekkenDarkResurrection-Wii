/* Boot a statically recompiled PSP image on the Wii. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "func_table.h"
#include "image_base.h"
#include "gpu/gpu.h"
#include "hle/hle.h"
#include "recomp.h"
#include "wii_platform.h"

void io_init(const char *iso_path, const char *ms_root);
void hle_init(void);
void sched_boot(Ctx *boot_ctx);
void frontend_run(void);

static bool load_image_file(const char *path) {
    if (IMAGE_SIZE == 0 || g_func_count == 0)
        return false;
    if (IMAGE_SIZE > GUEST_RAM_SIZE || IMAGE_BASE < GUEST_RAM_BASE ||
        IMAGE_BASE + IMAGE_SIZE > 0x0A000000u) {
        wii_fatal("WII boot: image does not fit the 32 MiB PSP window "
                  "(base %08X size %08X)\n", IMAGE_BASE, IMAGE_SIZE);
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        wii_fatal("WII boot: cannot open %s\n", path);
        return false;
    }
    size_t n = fread(host(IMAGE_BASE), 1, IMAGE_SIZE, f);
    fclose(f);
    if (n != IMAGE_SIZE) {
        wii_fatal("WII boot: short read %u of %u bytes\n", (unsigned)n, IMAGE_SIZE);
        return false;
    }
    return true;
}

static void app_directory(const char *save_root, char *out, size_t n) {
    snprintf(out, n, "%s", save_root);
    char *slash = strrchr(out, '/');
    if (slash) *slash = 0;
    else snprintf(out, n, "sd:/apps/tkdr");
}

bool wii_game_boot(const WiiData *data) {
    wii_log("WII boot: allocating guest memory\n");
    if (!wii_memory_init()) {
        wii_fatal("WII boot: guest memory allocation failed\n");
        return false;
    }
    wii_log("WII boot: loading %s\n", data->image);
    if (!load_image_file(data->image)) {
        wii_fatal("WII boot: this DOL has no recompiled game code, or the "
                  "image is incompatible.\n"
                  "Build it on a PC with tools/setup_assets.py first.\n");
        return false;
    }

    char app[192];
    app_directory(data->save_root, app, sizeof app);
    if (chdir(app) != 0)
        wii_log("WII boot: chdir %s failed; save paths may be relative\n", app);

    wii_log("WII boot: opening ISO %s\n", data->iso);
    io_init(data->iso, "ms0");
    g_hle_log = 0;
    gpu_set_render_scale(1);
    gpu_set_gpu_renderer(0);

    wii_log("WII boot: initialising GX\n");
    wii_video_game_begin();
    wii_log("WII boot: initialising input\n");
    wii_input_init();
    wii_log("WII boot: initialising audio\n");
    if (!wii_audio_init())
        wii_log("WII boot: audio unavailable, continuing muted\n");

    uint32_t free_start = (IMAGE_BASE + IMAGE_SIZE + 0xFFu) & ~0xFFu;
    sysmem_init(free_start, 0x0A000000u);
    hle_init();

    static Ctx boot;
    memset(&boot, 0, sizeof boot);
    const char *boot_path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    uint32_t argp = 0x08800000u;
    strcpy((char *)host(argp), boot_path);
    boot.r[4] = (uint32_t)strlen(boot_path) + 1;
    boot.r[5] = argp;
#ifdef IMAGE_GP
    boot.r[28] = IMAGE_GP;
#endif

    wii_log("WII boot: entry %08X, image %08X+%X, free %08X..0A000000\n",
            g_entry, IMAGE_BASE, IMAGE_SIZE, free_start);
#ifdef TKDR_AUTOPILOT
    { extern void gpu_vertex_bench(void); gpu_vertex_bench(); }
#endif
#ifdef GPU_SELFTEST
    {   /* Texture-format test vectors through the real big-endian reader. */
        extern int gpu_texture_selftest(void);
        extern int wii_gxscene_selftest(void);
        int failed = gpu_texture_selftest();
        failed += wii_gxscene_selftest();
        wii_log("WII boot: texture self-test %s\n", failed ? "FAILED" : "passed");
    }
#endif
    wii_log("WII boot: starting guest scheduler\n");
    sched_boot(&boot);
    wii_log("WII boot: scheduler returned, entering frontend\n");
    frontend_run();
    return true;
}
