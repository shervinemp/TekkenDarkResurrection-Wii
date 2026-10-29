/* H.264 movie decoding is not built for PowerPC yet.  Rather than leaving the
   AVC AU queue permanently empty (which makes movie players wait forever),
   treat each supplied buffer as one opaque access unit and clear the requested
   output frame to black.  The game's movie clock advances and intro videos are
   skipped visually; ATRAC3+ audio still comes through the normal MPEG path. */
#include <stdint.h>
#include <string.h>

#include "recomp.h"

void avc_reset(void) {
}

/* The common frontend refers to the movie picture even when the H.264
   decoder is not built; the stub has no picture, so answer with an empty
   one and let the guest framebuffer path run. */
uint16_t *hle_movie_pixels(void) { return 0; }
int hle_movie_frame_ready(void) { return 0; }
unsigned hle_movie_frame_seq(void) { return 0; }

#ifdef EXP_AVCPROBE
/* -DEXP_AVCPROBE: what the movie player hands the decoder (the first buffers' NAL units, and how often it asks), for sizing a
   real decoder: AVCPROBE au n len .. nals .. / AVCPROBE decode n at .. ms */
#include <stdio.h>
static unsigned s_pn, s_dn;
static void probe_au(const uint8_t *d, int len) {
    if (++s_pn > 24) return;
    char nal[160]; int k = 0;
    for (int i = 0; i + 4 < len && i < 64 * 1024 && k < 150; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) { k += snprintf(nal + k, 150 - k, "%d@%d ", d[i + 3] & 31, i); i += 3; }
    nal[k] = 0;
    fprintf(stderr, "AVCPROBE au %u len %d first %02x%02x%02x%02x%02x%02x%02x%02x nals %s\n", s_pn, len, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], nal);
    for (int i = 0; i + 4 < len && i < 64 * 1024; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && ((d[i + 3] & 31) == 7 || (d[i + 3] & 31) == 8)) {
            char hx[200]; int h = 0;
            for (int j = 0; j < 40 && i + 3 + j < len; j++) h += snprintf(hx + h, 200 - h, "%02x", d[i + 3 + j]);
            fprintf(stderr, "AVCPROBE nal%d %s\n", d[i + 3] & 31, hx);
        }
}
#endif

int avc_next_au(const uint8_t *data, int len, int *got) {
#ifdef EXP_AVCPROBE
    probe_au(data, len);
#endif
    (void)data;
    if (len < 4) {
        *got = 0;
        return len;
    }
    *got = 1;
    return len;
}

int avc_decode(uint32_t dst, int stride, int mode) {
#ifdef EXP_AVCPROBE
    if (++s_dn <= 5 || (s_dn % 200) == 0) fprintf(stderr, "AVCPROBE decode %u dst %08x stride %d mode %d\n", s_dn, dst, stride, mode);
#endif
    if (!dst) return 0;
    /* The PSP player normally hands us a 480x272 (or 512x272) buffer. */
    int w = 480, h = 272;
    if (stride < w) w = stride;
    if (mode == 3) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                mem_w32(dst + (uint32_t)(y * stride + x) * 4, 0xFF000000u);
    } else {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                mem_w16(dst + (uint32_t)(y * stride + x) * 2, 0);
    }
    return 1;
}
