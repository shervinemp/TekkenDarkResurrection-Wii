/* Tekken: Dark Resurrection Wii launcher/entry point.
 *
 * Expected layout on SD (preferred) or a FAT/NTFS USB stick:
 *
 *   sd:/apps/tkdr/boot.dol          this DOL
 *   sd:/apps/tkdr/image.bin         generated from your decrypted EBOOT
 *   sd:/apps/tkdr/tekken_dr.iso     your own ULUS10139 dump (ISO)
 *
 * Saves go to the SD card under sd:/apps/tkdr/ms0.
 *
 * NTFS sticks are mounted read-only and are only used for game data; saves
 * always go to the SD card (or a writable FAT USB stick).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gccore.h>
#include <ogc/system.h>

#include "wii_platform.h"

bool wii_game_boot(const WiiData *data);

static void wait_forever(void) {
    for (;;) {
        VIDEO_WaitVSync();
        if (SYS_ResetButtonDown())
            exit(0);
    }
}

static void print_install_help(void) {
    printf("\x1b[2J");
    printf("\x1b[1;33mTekken: Dark Resurrection Wii\x1b[0m\n\n");
    printf("No playable data was found.\n\n");
    printf("Put these files in one of:\n");
    printf("  sd:/apps/tkdr/         (recommended)\n");
    printf("  usb:/apps/tkdr/        (FAT or read-only NTFS)\n\n");
    printf("  image.bin       recompiled executable image\n");
    printf("  tekken_dr.iso   your own Dark Resurrection (ULUS10139) dump\n");
    printf("  (tekken_dr.bin / Tekken DR.iso / ULUS10139.iso also accepted)\n");
    printf("  CSO is not read on Wii; convert it back to ISO first.\n\n");
    printf("image.bin is generated on a PC from your decrypted EBOOT:\n");
    printf("  python tools/setup_assets.py --iso tekken_dr.iso --eboot EBOOT.BIN\n");
    printf("then rebuild this DOL.  The game files are never distributed.\n\n");
    printf("Log: sd:/apps/tkdr/debug.log\n");
}

int main(int argc, char **argv) {
    if (!wii_video_init()) {
        return 1;
    }
    /* Keep stdout on the TV console by default.  Build with
       `make CFLAGS+=-DWII_OSREPORT` to mirror it to Dolphin's OSReport log. */
#ifdef WII_OSREPORT
    SYS_STDIO_Report(true);
#endif
    printf("\x1b[2J");
    printf("\x1b[1;36mTekken: Dark Resurrection Wii\x1b[0m  -  PSP static recompilation\n\n");

    wii_storage_init();
    WiiData data;
    if (!wii_storage_find(&data, argc > 0 ? argv[0] : NULL)) {
        print_install_help();
        wait_forever();
    }

    wii_log("WII main: install=%s\n", data.root);
    wii_log("WII main: image=%s\n", data.image);
    wii_log("WII main: iso=%s%s\n", data.iso, data.iso_on_ntfs ? " (NTFS)" : "");
    wii_log("WII main: saves=%s\n", data.save_root);
    printf("install: %s\n", data.root);
    printf("image:   %s\n", data.image);
    printf("iso:     %s%s\n", data.iso, data.iso_on_ntfs ? " (NTFS, read-only)" : "");
    printf("saves:   %s\n\n", data.save_root);
    printf("starting...\n");
    wii_log("WII main: entering boot\n");

    if (!wii_game_boot(&data)) {
        printf("\nBoot failed; see the log on the SD card.\n");
        wait_forever();
    }
    return 0;
}
