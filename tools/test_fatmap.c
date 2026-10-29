/* Checks platform/wii/wii_fatmap.c (reads of one file straight from the sectors of its FAT32 volume) on the PC.

   The "device" is a raw image of a FAT32 card (Dolphin's WiiSD.raw), the reference is the same file on the PC's own
   disk.  The test opens the file by its path, then reads random ranges (unaligned starts and ends, 1 byte to 1 MB,
   the head, the tail and ranges across run boundaries) through fatmap_read() and compares every byte.

   Build and run (from the repository root, with the devkitPro msys2 gcc):
       gcc -O2 -DFATMAP_TEST -o test_fatmap tools/test_fatmap.c
       ./test_fatmap C:/tkdr_raw_user/Load/WiiSD.raw sd:/apps/tkdr/tekken_dr.iso dist-tkdr-sd-full/apps/tkdr/tekken_dr.bin
   (Dolphin's card holds the ISO as one run.)  "./test_fatmap --synthetic" builds a small FAT32 card in memory behind an MBR, with
   long names, a deleted entry and look-alike names beside the file, and a file cut into dozens of runs that are not in order,
   and does the same reads. */
#define _FILE_OFFSET_BITS 64
#include "../platform/wii/wii_fatmap.c"

static FILE *g_img;

static bool dev_read(uint32_t sector, uint32_t count, void *buf) {
    if (fseeko(g_img, (off_t)sector * 512, SEEK_SET)) return false;
    return fread(buf, 512, count, g_img) == count;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}


/* ---------------------------------------------------------------- synthetic card */
static uint8_t *g_card;
static uint64_t g_card_secs;

static bool mem_read(uint32_t sector, uint32_t count, void *buf) {
    if ((uint64_t)sector + count > g_card_secs) return false;
    memcpy(buf, g_card + (size_t)sector * 512, (size_t)count * 512);
    return true;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

/* Appends the entries of one name (long name pieces first, then the 8.3 entry) at dir[*n]. */
static void put_entry(uint8_t *dir, unsigned *n, const char *name, const char sfn[11], int attr, uint32_t cluster, uint32_t size, int plain_sfn, unsigned bad_sum) {
    if (!plain_sfn) {
        unsigned sum = 0;
        for (int k = 0; k < 11; k++) sum = (((sum & 1) << 7) + (sum >> 1) + (uint8_t)sfn[k]) & 0xFF;
        const unsigned len = (unsigned)strlen(name), pieces = (len + 12) / 13;
        for (unsigned pc = pieces; pc >= 1; pc--) {
            uint8_t *e = dir + 32 * (*n)++;
            memset(e, 0xFF, 32);
            e[0] = pc | (pc == pieces ? 0x40 : 0);
            e[11] = 0x0F; e[12] = 0; e[13] = sum ^ bad_sum; put16(e + 26, 0);
            static const uint8_t at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (int k = 0; k < 13; k++) {
                const unsigned i = (pc - 1) * 13 + k;
                put16(e + at[k], i < len ? (uint8_t)name[i] : i == len ? 0 : 0xFFFF);
            }
        }
    }
    uint8_t *e = dir + 32 * (*n)++;
    memset(e, 0, 32);
    memcpy(e, sfn, 11);
    e[11] = attr;
    put16(e + 20, cluster >> 16); put16(e + 26, cluster); put32(e + 28, size);
}

static int synthetic(int reads) {
    const uint32_t part = 2048, spc = 4, res = 32, nfat = 2, nclusters = 70000, cb = spc * 512;
    const uint32_t spf = (nclusters + 2 + 127) / 128, data = part + res + nfat * spf, total = res + nfat * spf + nclusters * spc;
    g_card_secs = part + total;
    g_card = calloc((size_t)g_card_secs, 512);
    uint8_t *mbr = g_card;
    mbr[510] = 0x55; mbr[511] = 0xAA;
    mbr[0x1BE + 4] = 0x0C; put32(mbr + 0x1BE + 8, part); put32(mbr + 0x1BE + 12, total);
    uint8_t *bs = g_card + (size_t)part * 512;
    put16(bs + 11, 512); bs[13] = spc; put16(bs + 14, res); bs[16] = nfat; put32(bs + 32, total); put32(bs + 36, spf); put32(bs + 44, 2);
    bs[510] = 0x55; bs[511] = 0xAA;
    uint8_t *fat = g_card + (size_t)(part + res) * 512;
    uint8_t *fat2 = fat + (size_t)spf * 512;
    #define FATSET(c_, v_) do { put32(fat + 4 * (c_), (v_)); put32(fat2 + 4 * (c_), (v_)); } while (0)
    FATSET(0, 0x0FFFFFF8u); FATSET(1, 0xFFFFFFFFu);
    FATSET(2, 0x0FFFFFFFu);   /* root */
    FATSET(3, 0x0FFFFFFFu);   /* the folder */
    FATSET(4, 0x0FFFFFFFu);   /* a decoy file's single cluster */
    #define CL(c_) (g_card + (size_t)(data + ((c_) - 2) * spc) * 512)

    uint32_t size = 6u * 1024 * 1024 + 1234;
    const uint32_t want = (size + cb - 1) / cb;
    uint32_t *chain = malloc(want * sizeof *chain), got = 0, lo = 10, hi = nclusters + 1;
    for (int run = 0; got < want; run++) {
        uint32_t len = 1 + rnd() % 100;
        if (len > want - got) len = want - got;
        if (run % 3 == 2) {   /* from the top of the card, so the chain goes backwards between runs */
            hi -= len + rnd() % 20;
            for (uint32_t k = 0; k < len; k++) chain[got++] = hi + k + 1;
        } else {
            for (uint32_t k = 0; k < len; k++) chain[got++] = lo + k;
            lo += len + 1 + rnd() % 30;
        }
    }
    for (uint32_t i = 0; i < want; i++) FATSET(chain[i], i + 1 < want ? chain[i + 1] : 0x0FFFFFFFu);
    uint8_t *content = malloc(size);
    for (uint32_t i = 0; i < size; i++) content[i] = (uint8_t)rnd();
    for (uint32_t i = 0; i < want; i++) {
        const uint32_t n = size - i * cb < cb ? size - i * cb : cb;
        memcpy(CL(chain[i]), content + (size_t)i * cb, n);
    }
    unsigned n = 0;
    put_entry(CL(2), &n, "SDCARD", "SDCARD     ", 0x08, 0, 0, 1, 0);
    put_entry(CL(2), &n, "My Games Folder", "MYGAME~1   ", 0x10, 3, 0, 0, 0);
    n = 0;
    put_entry(CL(3), &n, "game image.iso.bak", "GAMEIM~2BAK", 0x20, 4, 100, 0, 0);
    put_entry(CL(3), &n, "game image.is", "GAMEIM~3   ", 0x20, 4, 100, 0, 0);
    put_entry(CL(3), &n, "game image.iso", "OTHER   ISO", 0x20, 4, 100, 0, 0x5A);   /* a stale long name: its checksum is not this 8.3 entry's */
    put_entry(CL(3), &n, "game image.iso", "GAMEIM~1ISO", 0x20, chain[0], size, 0, 0);
    FILE *ref = tmpfile();
    fwrite(content, 1, size, ref);
    fflush(ref);

    int bad = 0;
    const char *paths[] = {"sd:/My Games Folder/game image.iso", "usb:/my games folder/GAME IMAGE.ISO", "sd:/MYGAME~1/GAMEIM~1.ISO"};
    for (int pi = 0; pi < 3; pi++) {
        s_ok = 0;
        int rc = fatmap_open(mem_read, paths[pi], ref);
        printf("%s -> %d, %u runs, expected %u chain clusters\n", paths[pi], rc, s_next, want);
        if (rc) { bad++; continue; }
        uint8_t *a = aligned_alloc(32, (1 << 20) + 64);
        for (int i = 0; i < reads; i++) {
            uint64_t off = rnd() % size, len = i % 2 ? 1 + rnd() % 3000 : 1 + rnd() % 200000;
            if (i % 7 == 0) off = size - 1 - rnd() % 3000;
            uint8_t *dst = a + (i % 3 == 0 ? 1 + i % 29 : 0);
            int64_t got2 = fatmap_read(off, dst, len);
            uint64_t w = off + len > size ? size - off : len;
            if (got2 != (int64_t)w || memcmp(dst, content + off, w)) { if (++bad < 10) printf("bad read %llu+%llu\n", (unsigned long long)off, (unsigned long long)len); }
        }
        free(a);
    }
    s_ok = 0;
    printf("a path that is not there: %d (want -2)\n", fatmap_open(mem_read, "sd:/My Games Folder/game image.isx", NULL));
    printf("synthetic: %d bad\n", bad);
    return bad != 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--synthetic")) return synthetic(argc > 2 ? atoi(argv[2]) : 500);
    if (argc < 4) { fprintf(stderr, "usage: test_fatmap image.raw sd:/path/in/image reference-file [reads]\n"); return 2; }
    g_img = fopen(argv[1], "rb");
    FILE *ref = fopen(argv[3], "rb");
    if (!g_img || !ref) { fprintf(stderr, "cannot open the image or the reference\n"); return 2; }
    const int reads = argc > 4 ? atoi(argv[4]) : 400;
    int rc = fatmap_open(dev_read, argv[2], ref);
    printf("fatmap_open -> %d, %u runs, file %llu bytes, cluster %u bytes\n", rc, s_next, (unsigned long long)s_size, s_cbytes);
    if (rc) return 1;
    uint8_t *a = aligned_alloc(32, (1 << 20) + 64), *b = malloc((1 << 20) + 64);
    int bad = 0;
    for (int i = 0; i < reads; i++) {
        uint64_t off, len;
        switch (i % 5) {
            case 0: off = rnd() % s_size; len = 1 + rnd() % 3000; break;
            case 1: off = (rnd() % s_size) & ~(uint64_t)2047; len = 2048 * (1 + rnd() % 8); break;
            case 2: off = rnd() % s_size; len = 1 + rnd() % (1 << 20); break;
            case 3: off = (rnd() % s_size) & ~(uint64_t)511; len = 512 * (1 + rnd() % 2048); break;
            default: off = s_size - 1 - rnd() % 5000; len = 1 + rnd() % 20000; break;   /* across the end */
        }
        uint8_t *dst = a + (i % 3 == 0 ? 1 : 0) * (1 + i % 31);   /* some destinations are not aligned */
        int64_t got = fatmap_read(off, dst, len);
        uint64_t want = off + len > s_size ? s_size - off : len;
        if (got != (int64_t)want) { printf("read %llu+%llu returned %lld, expected %llu\n", (unsigned long long)off, (unsigned long long)len, (long long)got, (unsigned long long)want); bad++; continue; }
        fseeko(ref, (off_t)off, SEEK_SET);
        if (fread(b, 1, want, ref) != want) { printf("reference short read\n"); return 2; }
        if (memcmp(dst, b, want)) { printf("DIFFERENT at %llu+%llu\n", (unsigned long long)off, (unsigned long long)len); bad++; }
    }
    printf("%d reads, %d bad\n", reads, bad);
    return bad != 0;
}
