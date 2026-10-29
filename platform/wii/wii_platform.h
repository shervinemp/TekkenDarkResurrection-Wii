/* Wii platform services shared by the boot layer and the recomp runtime. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    char root[192];       /* install directory, with device prefix */
    char image[256];      /* relocated executable image (image.bin) */
    char iso[256];        /* game ISO/CSD */
    char eboot[256];      /* decrypted EBOOT, if present */
    char save_root[192];  /* writable ms0 root */
    bool iso_on_ntfs;
} WiiData;

/* storage */
bool wii_storage_init(void);
bool wii_storage_find(WiiData *out, const char *launch_path);
const char *wii_storage_log(void);

/* video */
bool wii_video_init(void);
void wii_video_game_begin(void);
void wii_video_present(const uint16_t *argb, int w, int h);
/* Show the guest's framebuffer when the chip has stopped producing copies
   (the intro movie and the menus are drawn outside the GE).  This writes the
   display buffer with the CPU and issues no GX commands, so it cannot race
   the GE thread's command stream. */
void wii_video_present_cpu(const uint16_t *argb, int w, int h);
/* Present a 480x272 limited-range YUV422 movie picture 1:1 in the centre of
   the XFB.  The VI scans YUV422, so this path writes the same format the GPU
   would have produced, without an RGB565 round trip. */
void wii_video_present_movie(const uint16_t *yuv422, int w, int h);
uint16_t *wii_video_movie_pixels(void);
int wii_video_convert_movie(const uint8_t *y, int ys, const uint8_t *u, int us,
                            const uint8_t *v, int vs, int w, int h, int full);
/* Bring the boot console back to the display for a fatal error. */
void wii_video_show_console(void);

/* The GX scene renderer's display buffers.  The GE thread takes one, has
   the chip copy the picture into it and submits it with the token that
   follows the copy; the frontend flips to the newest copy the chip has
   finished. */
void *wii_video_xfb_acquire(void);
void wii_video_xfb_submit(void *xfb, uint16_t token);
void wii_video_flip(void);
void wii_video_shutdown(void);
int wii_video_width(void);
int wii_video_height(void);

/* input: PSP button bits and analog values */
void wii_input_init(void);
void wii_input_scan(void);
uint32_t wii_input_buttons(void);
void wii_input_analog(uint8_t *x, uint8_t *y);
bool wii_input_home_pressed(void);

/* audio */
bool wii_audio_init(void);
void wii_audio_shutdown(void);
void wii_audio_update(void);
uint32_t wii_audio_underruns(void);  /* stream buffers the DSP had to play as silence */

/* time/log */
uint64_t wii_time_us(void);
void wii_sleep_us(uint64_t us);
void wii_yield(void);

/* libogc's handle for the calling thread; unique among live threads and
   never 0.  (__thread is not per-thread on this platform; see pthread_lwp.c.) */
uint32_t wii_thread_id(void);

/* The host stack a thread runs on, so the runtime can notice an overflow.
   The base is the low address; the stack grows down from the top. */
void *wii_pthread_stack_base(void *thread);
size_t wii_pthread_stack_size(void *thread);
/* False once a guest call chain has smashed the canary at the bottom of
   the thread's host stack. */
bool wii_pthread_stack_intact(void *thread);

/* A thread that reports once a second what the other threads are doing
   (WII_HEARTBEAT builds): a hang then shows where it is. */
void wii_heartbeat_start(void);

void wii_log_init(void);
void wii_log(const char *fmt, ...);
void wii_fatal(const char *fmt, ...);

/* memory */
bool wii_memory_init(void);
/* Big blocks (decoded textures) come from a pool in MEM2; MEM1's heap is
   nearly full.  The blocks are 32 byte aligned.  wii_big_alloc returns NULL
   when the pool cannot satisfy the request; wii_big_free takes pool blocks
   and anything malloc() returned. */
void *wii_big_alloc(size_t n);
void wii_big_free(void *p);
uint32_t wii_big_used(void);
extern uint8_t *g_mem;   /* 32 MiB PSP user RAM window (0x08000000) */
extern uint8_t *g_vram;  /* 2 MiB PSP EDRAM window (0x04000000) */
extern uint8_t *g_low;   /* 2 MiB scratch window (the PSP-2000/3000 extended RAM) */
