/* Reads of one big file straight from the sectors of its FAT32 volume.

   libfat finds the cluster of a seek target by walking the FAT chain, one entry at a time,
   from the file's first cluster (a seek backwards) or from the current position.  The game
   reads its 1.7 GB UMD image all over, so a far seek walked up to 430,000 entries of an SD
   card with 4 KB clusters (a cheaper but still long chain on 16 or 32 KB clusters) and took
   40 to 80 ms in Dolphin for a read that itself takes 2: a hitch of three to five frames
   on every stream that was not close to the last one.

   This reads the volume's boot sector, finds the file by its path, follows its cluster chain
   once and keeps it as a list of runs of consecutive clusters (a file copied onto an empty
   card is one or a few runs).  After that an offset in the file is a binary search and a
   multiply, and the data comes from the device's own readSectors().  The file is only ever
   read here, so libfat's caches stay valid.  Anything unexpected (a volume that is not
   FAT32 with 512-byte sectors, a path that does not resolve, more than MAX_EXTENTS runs, or
   a single byte that differs from what libfat returns at the checked offsets) makes
   fatmap_open() fail and the caller keeps reading through libfat as before.

   The caller serialises this with every other use of the card (storage_lock). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FATMAP_TEST
#include <ogc/disc_io.h>
#include <ogc/usbstorage.h>
extern DISC_INTERFACE __io_wiisd;
#endif

#define SEC 512u
#define MAX_EXTENTS 1024u
#define MAX_REQUEST_SECTORS 128u    /* sectors in one device request (64 KB) */
#define BOUNCE_SECTORS 64u

typedef bool (*FatReadFn)(uint32_t sector, uint32_t count, void *buf);

static FatReadFn s_rd;
static uint32_t s_spc, s_fat_lba, s_fat_secs, s_data_lba, s_root, s_nclusters, s_cbytes;
static uint64_t s_size;
static struct { uint32_t file_idx, first, count; } s_ext[MAX_EXTENTS];   /* runs: file cluster index, first cluster, length */
static unsigned s_next;
static int s_ok;
static uint8_t s_sec[SEC] __attribute__((aligned(32)));
static uint8_t s_fat[16 * SEC] __attribute__((aligned(32)));
static uint8_t s_bounce[BOUNCE_SECTORS * SEC] __attribute__((aligned(32)));
static uint32_t s_fat_base = ~0u;

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | (le16(p + 2) << 16); }

static uint32_t cluster_lba(uint32_t c) { return s_data_lba + (c - 2) * s_spc; }

/* The FAT entry of cluster c (the next cluster of a chain), or 0xFFFFFFFF if the FAT cannot be read. */
static uint32_t fat_next(uint32_t c) {
    const uint32_t rel = c / (SEC / 4);
    const uint32_t base = rel & ~15u;
    if (base != s_fat_base) {
        uint32_t n = s_fat_secs - base;
        if (n > 16) n = 16;
        if (!s_rd(s_fat_lba + base, n, s_fat)) return 0xFFFFFFFFu;
        s_fat_base = base;
    }
    return le32(s_fat + (rel - base) * SEC + (c % (SEC / 4)) * 4) & 0x0FFFFFFFu;
}

/* Is there a FAT32 volume whose boot sector is at lba?  Sets the geometry if so. */
static int vol_at(uint32_t lba) {
    if (!s_rd(lba, 1, s_sec)) return 0;
    const uint8_t *b = s_sec;
    if (b[510] != 0x55 || b[511] != 0xAA) return 0;
    const uint32_t bps = le16(b + 11), spc = b[13], res = le16(b + 14), nfat = b[16];
    const uint32_t rootent = le16(b + 17), tot16 = le16(b + 19), spf16 = le16(b + 22);
    const uint32_t tot32 = le32(b + 32), spf32 = le32(b + 36), root = le32(b + 44);
    if (bps != SEC || !spc || (spc & (spc - 1)) || spc > 128 || !res || !nfat || rootent || spf16 || !spf32 || root < 2) return 0;
    const uint32_t total = tot16 ? tot16 : tot32;
    if (total <= res + nfat * spf32) return 0;
    const uint32_t clusters = (total - res - nfat * spf32) / spc;
    if (clusters < 65525) return 0;   /* FAT12 or FAT16 */
    s_spc = spc;
    s_cbytes = spc * SEC;
    s_fat_lba = lba + res;
    if ((b[40] & 0x80) && (b[40] & 0xF) < nfat) s_fat_lba += (b[40] & 0xF) * spf32;   /* mirroring off: one FAT is the live one */
    s_fat_secs = spf32;
    s_data_lba = lba + res + nfat * spf32;
    s_root = root;
    s_nclusters = clusters;
    s_fat_base = ~0u;
    return 1;
}

static int find_volume(void) {
    if (vol_at(0)) return 1;
    if (!s_rd(0, 1, s_sec) || s_sec[510] != 0x55 || s_sec[511] != 0xAA) return 0;
    uint32_t start[4], type[4];
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = s_sec + 0x1BE + 16 * i;
        type[i] = e[4];
        start[i] = le32(e + 8);
    }
    for (int i = 0; i < 4; i++)
        if ((type[i] == 0x0B || type[i] == 0x0C) && start[i] && vol_at(start[i])) return 1;
    return 0;
}

static int ieq(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* Looks name up in the directory that starts at cluster dir.  1 found, 0 not there, -1 error. */
static int dir_find(uint32_t dir, const char *name, uint32_t *cluster, uint64_t *size, int *is_dir) {
    char lfn[272];
    int lfn_ok = 0;
    unsigned expect = 0, sum = 0;
    uint32_t c = dir;
    for (unsigned guard = 0; c >= 2 && c < s_nclusters + 2 && guard < (1u << 20); guard++) {
        for (uint32_t s = 0; s < s_spc; s++) {
            if (!s_rd(cluster_lba(c) + s, 1, s_sec)) return -1;
            for (int e = 0; e < 16; e++) {
                const uint8_t *d = s_sec + 32 * e;
                if (d[0] == 0) return 0;
                if (d[0] == 0xE5) { lfn_ok = 0; continue; }
                if (d[11] == 0x0F) {   /* a piece of a long name; they come last piece first */
                    const unsigned ord = d[0] & 0x3F;
                    if (d[0] & 0x40) {
                        if (!ord || ord > 20) { lfn_ok = 0; continue; }
                        memset(lfn, 0, sizeof lfn);
                        lfn_ok = 1;
                        expect = ord;
                        sum = d[13];
                    } else if (!lfn_ok || ord != expect || d[13] != sum) {
                        lfn_ok = 0;
                        continue;
                    }
                    static const uint8_t at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                    for (int k = 0; k < 13; k++) {
                        const uint32_t ch = le16(d + at[k]);
                        if (ch == 0 || ch == 0xFFFF) continue;
                        lfn[(ord - 1) * 13 + k] = ch < 127 ? (char)ch : '?';
                    }
                    expect = ord - 1;
                    continue;
                }
                if (d[11] & 0x08) { lfn_ok = 0; continue; }   /* the volume label */
                unsigned chk = 0;
                for (int k = 0; k < 11; k++) chk = (((chk & 1) << 7) + (chk >> 1) + d[k]) & 0xFF;   /* the checksum is 8 bits wide at every step */
                const int long_ok = lfn_ok && expect == 0 && chk == sum;
                lfn_ok = 0;
                char sfn[16];
                int n = 0;
                for (int k = 0; k < 8 && d[k] != ' '; k++) sfn[n++] = (char)d[k];
                if (d[8] != ' ') {
                    sfn[n++] = '.';
                    for (int k = 8; k < 11 && d[k] != ' '; k++) sfn[n++] = (char)d[k];
                }
                sfn[n] = 0;
                if ((long_ok && ieq(lfn, name)) || ieq(sfn, name)) {
                    *cluster = (le16(d + 20) << 16) | le16(d + 26);
                    *size = le32(d + 28);
                    *is_dir = (d[11] & 0x10) != 0;
                    return 1;
                }
            }
        }
        c = fat_next(c);
        if (c >= 0x0FFFFFF7u) break;
    }
    return 0;
}

static int resolve(const char *path, uint32_t *cluster, uint64_t *size) {
    const char *p = strchr(path, ':');
    p = p ? p + 1 : path;
    uint32_t dir = s_root;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) return 0;
        char name[256];
        unsigned n = 0;
        while (*p && *p != '/') {
            if (n >= sizeof name - 1) return 0;
            name[n++] = *p++;
        }
        name[n] = 0;
        while (*p == '/') p++;
        uint32_t c = 0;
        uint64_t sz = 0;
        int is_dir = 0;
        if (dir_find(dir, name, &c, &sz, &is_dir) != 1) return 0;
        if (!*p) {
            if (is_dir) return 0;
            *cluster = c;
            *size = sz;
            return 1;
        }
        if (!is_dir) return 0;
        dir = c ? c : s_root;
    }
}

/* Follows the file's chain and keeps it as runs of consecutive clusters. */
static int build_extents(uint32_t first, uint64_t size) {
    const uint64_t want = (size + s_cbytes - 1) / s_cbytes;
    uint64_t idx = 0;
    uint32_t c = first;
    s_next = 0;
    while (idx < want) {
        if (c < 2 || c >= s_nclusters + 2) return 0;
        if (s_next && s_ext[s_next - 1].first + s_ext[s_next - 1].count == c) {
            s_ext[s_next - 1].count++;
        } else {
            if (s_next == MAX_EXTENTS) return 0;
            s_ext[s_next].file_idx = (uint32_t)idx;
            s_ext[s_next].first = c;
            s_ext[s_next].count = 1;
            s_next++;
        }
        idx++;
        c = fat_next(c);
        if (c == 0xFFFFFFFFu) return 0;
        if (idx < want && c >= 0x0FFFFFF7u) return 0;
    }
    return c >= 0x0FFFFFF8u;
}

static unsigned ext_find(uint32_t file_cluster) {
    unsigned lo = 0, hi = s_next;
    while (hi - lo > 1) {
        const unsigned mid = (lo + hi) / 2;
        if (s_ext[mid].file_idx <= file_cluster) lo = mid;
        else hi = mid;
    }
    return lo;
}

/* Reads n bytes at off, clamped to the end of the file; returns the count, or -1 if the device failed. */
int64_t fatmap_read(uint64_t off, void *buf, size_t n) {
    if (!s_ok) return -1;
    if (off >= s_size) return 0;
    if (n > s_size - off) n = (size_t)(s_size - off);
    uint8_t *dst = (uint8_t *)buf;
    size_t done = 0;
    while (done < n) {
        const uint64_t o = off + done;
        const uint32_t ci = (uint32_t)(o / s_cbytes);
        const unsigned x = ext_find(ci);
        const uint32_t within = (uint32_t)(o % s_cbytes);
        const uint64_t run_end = (uint64_t)(s_ext[x].file_idx + s_ext[x].count) * s_cbytes;
        size_t chunk = n - done;
        if (chunk > run_end - o) chunk = (size_t)(run_end - o);
        const uint32_t sector = cluster_lba(s_ext[x].first + (ci - s_ext[x].file_idx)) + within / SEC;
        const uint32_t head = within % SEC;
        if (head == 0 && chunk >= SEC && ((uintptr_t)(dst + done) & 31) == 0) {
            size_t secs = chunk / SEC;
            if (secs > MAX_REQUEST_SECTORS) secs = MAX_REQUEST_SECTORS;
            if (!s_rd(sector, (uint32_t)secs, dst + done)) return -1;
            done += secs * SEC;
        } else {
            size_t secs = (head + chunk + SEC - 1) / SEC;
            if (secs > BOUNCE_SECTORS) {
                secs = BOUNCE_SECTORS;
                chunk = secs * SEC - head;
            }
            if (!s_rd(sector, (uint32_t)secs, s_bounce)) return -1;
            memcpy(dst + done, s_bounce + head, chunk);
            done += chunk;
        }
    }
    return (int64_t)done;
}

/* Builds the map of path on the volume read through rd, and checks it against what verify (the same file
   opened through libfat) returns at a few places.  0 on success. */
int fatmap_open(FatReadFn rd, const char *path, FILE *verify) {
    s_ok = 0;
    s_rd = rd;
    uint32_t first = 0;
    uint64_t size = 0;
    if (!find_volume()) return -1;
    if (!resolve(path, &first, &size) || !size || !build_extents(first, size)) return -2;
    s_size = size;
    s_ok = 1;
    if (verify) {
        /* Two 32-byte aligned buffers for the comparison, taken from the heap for the length of the check (statics held
           200 KB of MEM1 for good). */
        enum { VBUF = (100000 + 8 + 31) & ~31 };
        uint8_t *const raw = malloc(2 * VBUF + 32);
        if (!raw) { s_ok = 0; return -3; }
        uint8_t *const a = (uint8_t *)(((uintptr_t)raw + 31u) & ~(uintptr_t)31u);
        uint8_t *const b = a + VBUF;
        const struct { uint64_t off; uint32_t len; } probe[] = {
            {0, 4096}, {size / 4 + 777, 70000}, {size / 2 + 3, 5000}, {(size / 4 * 3) & ~(uint64_t)4095, 100000}, {size - 3000, 3000},
        };
        int rc = 0;
        for (unsigned i = 0; i < sizeof probe / sizeof probe[0] && !rc; i++) {
            if (probe[i].off + probe[i].len > size) continue;
            if (fseeko(verify, (off_t)probe[i].off, SEEK_SET) || fread(b, 1, probe[i].len, verify) != probe[i].len) rc = -3;
            else if (fatmap_read(probe[i].off, a, probe[i].len) != (int64_t)probe[i].len || memcmp(a, b, probe[i].len)) rc = -4;
        }
        free(raw);
        if (rc) { s_ok = 0; return rc; }
    }
    return 0;
}

unsigned fatmap_extents(void) { return s_next; }

#ifndef FATMAP_TEST
static const DISC_INTERFACE *s_io;
static bool wii_rd(uint32_t sector, uint32_t count, void *buf) { return s_io->readSectors(sector, count, buf); }

/* The ISO at path (sd:/... or usb:/...), also open as verify.  0: reads of it can use fatmap_read(). */
int wii_fatmap_open(const char *path, FILE *verify) {
    if (!strncmp(path, "sd:", 3)) s_io = &__io_wiisd;
    else if (!strncmp(path, "usb:", 4)) s_io = &__io_usbstorage;
    else return -9;
    return fatmap_open(wii_rd, path, verify);
}

int64_t wii_fatmap_read(uint64_t off, void *buf, size_t n) { return fatmap_read(off, buf, n); }
unsigned wii_fatmap_extents(void) { return s_next; }
void wii_fatmap_disable(void) { s_ok = 0; }
#endif
