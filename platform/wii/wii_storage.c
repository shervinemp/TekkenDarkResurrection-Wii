#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fat.h>
#include <gccore.h>
#include <ntfs.h>
#include <ogc/usb.h>
#include <ogc/usbstorage.h>

#include "wii_platform.h"

extern DISC_INTERFACE __io_wiisd;

typedef int (ntfs_log_handler_t)(const char *function, const char *file, int line,
                                  unsigned int level, void *data, const char *format,
                                  va_list arguments);
extern void ntfs_log_set_handler(ntfs_log_handler_t *handler);
extern unsigned int ntfs_log_set_levels(unsigned int levels);

#define NTFS_LOG_WARNING  (1u << 6)
#define NTFS_LOG_ERROR    (1u << 7)
#define NTFS_LOG_PERROR   (1u << 8)
#define NTFS_LOG_CRITICAL (1u << 9)

static char s_logbuf[512];
static int s_sd_mounted;
static int s_usb_mounted;
static int s_usb_ntfs;

const char *wii_storage_log(void) { return s_logbuf; }

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static void mkdir_p(const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *scan = strchr(tmp, ':');
    scan = scan ? scan + 1 : tmp;
    while (*scan == '/') scan++;
    for (; *scan; scan++) {
        if (*scan != '/') continue;
        *scan = 0;
        mkdir(tmp, 0777);
        *scan = '/';
    }
    mkdir(tmp, 0777);
}

static int ntfs_log(const char *function, const char *file, int line,
                    unsigned int level, void *data, const char *format,
                    va_list arguments) {
    (void)file; (void)line; (void)data;
    char msg[256];
    vsnprintf(msg, sizeof msg, format, arguments);
    snprintf(s_logbuf, sizeof s_logbuf, "NTFS %s: %s", function, msg);
    return 1;
}

static bool mount_sd(void) {
    if (fatMountSimple("sd", &__io_wiisd)) {
        s_sd_mounted = 1;
        return true;
    }
    return false;
}

static bool mount_usb(void) {
    USB_Initialize();
    for (int attempt = 0; attempt < 25; attempt++) {
        if (attempt) usleep(100000);

        if (fatMountSimple("usb", &__io_usbstorage)) {
            s_usb_mounted = 1;
            return true;
        }

        if ((attempt & 3) == 3) {
            sec_t *parts = NULL;
            int n = ntfsFindPartitions(&__io_usbstorage, &parts);
            bool ok = false;
            if (n > 0 && parts) {
                ntfs_log_set_handler(ntfs_log);
                ntfs_log_set_levels(NTFS_LOG_WARNING | NTFS_LOG_ERROR |
                                    NTFS_LOG_PERROR | NTFS_LOG_CRITICAL);
                ok = ntfsMount("usb", &__io_usbstorage, parts[0],
                               CACHE_DEFAULT_PAGE_COUNT, CACHE_DEFAULT_PAGE_SIZE,
                               NTFS_FORCE | NTFS_READ_ONLY | NTFS_IGNORE_CASE);
            }
            if (parts) free(parts);
            if (ok) {
                s_usb_mounted = 1;
                s_usb_ntfs = 1;
                return true;
            }
        }
    }
    return false;
}

bool wii_storage_init(void) {
    /* fatInitDefault mounts the SD card when one is present.  It may also
       mount USB on newer libfat; the explicit mount below is harmless. */
    fatInitDefault();
    DIR *sd = opendir("sd:/");
    if (sd) {
        closedir(sd);
        s_sd_mounted = 1;
    } else {
        mount_sd();
    }

    mkdir_p("sd:/apps/tkdr");
    mkdir_p("sd:/apps/tkdr/ms0");
    wii_log_init();
    wii_log("WII storage: sd=%s\n", s_sd_mounted ? "mounted" : "absent");
    if (!s_sd_mounted)
        wii_log("WII storage: no SD card; USB will be tried for data only\n");
    return true;
}

static bool find_in_root(const char *root, WiiData *out) {
    char image[256], iso[256], eboot[256];
    snprintf(image, sizeof image, "%s/image.bin", root);
    snprintf(iso, sizeof iso, "%s/tekken_dr.iso", root);
    snprintf(eboot, sizeof eboot, "%s/EBOOT.BIN", root);
    if (!file_exists(image)) {
        snprintf(image, sizeof image, "%s/data/image.bin", root);
        snprintf(iso, sizeof iso, "%s/data/tekken_dr.iso", root);
    }
    if (!file_exists(image)) return false;

    if (!file_exists(iso)) {
        static const char *names[] = {
            "tekken_dr.iso", "tekken_dr.bin", "Tekken DR.iso",
            "ULUS10139.iso", "a-tekken.iso", "game.iso",
        };
        for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
            snprintf(iso, sizeof iso, "%s/%s", root, names[i]);
            if (file_exists(iso)) break;
            snprintf(iso, sizeof iso, "%s/data/%s", root, names[i]);
            if (file_exists(iso)) break;
        }
    }
    if (!file_exists(iso)) return false;

    if (!file_exists(eboot)) {
        snprintf(eboot, sizeof eboot, "%s/ULUS10139_EBOOT.BIN", root);
        if (!file_exists(eboot))
            snprintf(eboot, sizeof eboot, "%s/PSP_GAME/SYSDIR/EBOOT.BIN", root);
    }

    snprintf(out->root, sizeof out->root, "%s", root);
    snprintf(out->image, sizeof out->image, "%s", image);
    snprintf(out->iso, sizeof out->iso, "%s", iso);
    snprintf(out->eboot, sizeof out->eboot, "%s", eboot);
    if (!strncmp(root, "usb", 3)) {
        s_usb_mounted = 1;
        out->iso_on_ntfs = s_usb_ntfs;
    }
    return true;
}

bool wii_storage_find(WiiData *out, const char *launch_path) {
    memset(out, 0, sizeof *out);
    char launch[192] = {0};

    if (launch_path) {
        const char *slash = strrchr(launch_path, '/');
        if (slash && strchr(launch_path, ':')) {
            size_t n = (size_t)(slash - launch_path);
            if (n && n < sizeof launch) {
                memcpy(launch, launch_path, n);
                launch[n] = 0;
            }
        }
    }

    const char *roots[] = {
        launch,
        "sd:/apps/tkdr",
        "usb:/apps/tkdr",
        "sd:/apps/tekken-dr",
        "usb:/apps/tekken-dr",
        "sd:/tekken-dr",
        "usb:/tekken-dr",
    };
    for (unsigned i = 0; i < sizeof roots / sizeof roots[0]; i++) {
        if (!roots[i] || !roots[i][0]) continue;
        if (find_in_root(roots[i], out)) {
            wii_log("WII storage: data found in %s\n", roots[i]);
            goto found;
        }
        char sub[224];
        snprintf(sub, sizeof sub, "%s/data", roots[i]);
        if (find_in_root(sub, out)) {
            wii_log("WII storage: data found in %s\n", sub);
            goto found;
        }
    }

    /* USB is only touched when the SD card does not already have the data:
       initialising the USB stack can take seconds (and is pointless on a
       console whose SD install is complete). */
    if (!s_usb_mounted) {
        wii_log("WII storage: trying USB for game data\n");
        mount_usb();
    }
    if (s_usb_mounted) {
        static const char *usb_roots[] = {
            "usb:/apps/tkdr", "usb:/apps/tekken-dr", "usb:/tekken-dr",
        };
        for (unsigned i = 0; i < sizeof usb_roots / sizeof usb_roots[0]; i++) {
            if (find_in_root(usb_roots[i], out)) {
                wii_log("WII storage: data found in %s (%s)\n", usb_roots[i],
                        s_usb_ntfs ? "NTFS" : "FAT");
                goto found;
            }
            char sub[224];
            snprintf(sub, sizeof sub, "%s/data", usb_roots[i]);
            if (find_in_root(sub, out)) {
                wii_log("WII storage: data found in %s (%s)\n", sub,
                        s_usb_ntfs ? "NTFS" : "FAT");
                goto found;
            }
        }
        mkdir_p("usb:/apps/tkdr");
    } else {
        wii_log("WII storage: usb not mounted\n");
    }

    wii_log("WII storage: no image.bin + ISO pair found\n");
    return false;

found:
    /* Saves go to the SD card when one is present; a read-only NTFS volume
       can never hold them.  Only with no SD do they follow the game to a FAT
       USB stick.  The actual write is attempted lazily by the save HLE. */
    char parent[192];
    snprintf(parent, sizeof parent, "%s", out->root);
    char *last = strrchr(parent, '/');
    if (last && !strcmp(last, "/data"))
        *last = 0;
    const char *base = strrchr(parent, '/');
    base = base ? base + 1 : parent;
    if (s_sd_mounted) {
        if (!strncmp(parent, "sd:", 3))
            snprintf(out->save_root, sizeof out->save_root, "%s/ms0", parent);
        else
            snprintf(out->save_root, sizeof out->save_root, "sd:/apps/%s/ms0", base);
    } else if (s_usb_mounted && !s_usb_ntfs) {
        if (!strncmp(parent, "usb", 3))
            snprintf(out->save_root, sizeof out->save_root, "%s/ms0", parent);
        else
            snprintf(out->save_root, sizeof out->save_root, "usb:/apps/%s/ms0", base);
    } else {
        snprintf(out->save_root, sizeof out->save_root, "sd:/apps/%s/ms0", base);
    }
    mkdir_p(out->save_root);
    wii_log("WII storage: saves in %s\n", out->save_root);
    return true;
}
