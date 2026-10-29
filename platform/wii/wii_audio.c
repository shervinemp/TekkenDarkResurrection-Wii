#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include <aesndlib.h>
#include <gccore.h>
#include <ogc/cache.h>
#include <ogc/lwp.h>

#include "wii_platform.h"

/* PSP audio is 44.1 kHz; the Wii DSP runs at 48 kHz.  The DSP does the rate
   conversion: one AESND voice in stream mode plays the HLE mixer's output at
   44.1 kHz, and the DSP's own resampler takes it to the 48 kHz output.

   The voice asks for its next buffer from the DSP interrupt (the stream
   callback), which hands over a buffer the feeder has already filled and
   wakes the feeder to fill the one that has just finished playing.  Nothing
   on the CPU resamples, and the feeder sleeps between buffers.  (It used to
   resample 441 -> 480 frames in floating point every 10 ms and spin on
   LWP_YieldThread in between, which cost the guest CPU time at every turn
   of the scheduler.) */
#define PSP_RATE    44100.0f
#define BUF_FRAMES  1024u                /* 23 ms at 44.1 kHz */
#define BUF_BYTES   (BUF_FRAMES * 4u)    /* stereo, 16 bit */
#define NBUF        4u                   /* about 70 ms of buffered audio */

void audio_pull(int16_t *out, int frames);

static int16_t *s_buf[NBUF];
static int16_t *s_silence;
/* Buffers handed to the DSP and buffers filled, as running counts.  The
   interrupt only advances s_head and the feeder only advances s_tail. */
static volatile uint32_t s_head, s_tail;
static volatile int s_running;
static volatile uint32_t s_underruns;
static lwp_t s_feeder;
static lwpq_t s_wake = LWP_TQUEUE_NULL;
static AESNDPB *s_voice;

/* DSP interrupt: the voice has finished its buffer and wants the next one. */
static void voice_callback(AESNDPB *pb, u32 state) {
    if (state != VOICE_STATE_STREAM) return;
    uint32_t head = s_head;
    if (s_tail != head) {
        AESND_SetVoiceBuffer(pb, s_buf[head % NBUF], BUF_BYTES);
        s_head = head + 1;
    } else {
        /* The feeder is behind: play a short gap rather than old samples. */
        AESND_SetVoiceBuffer(pb, s_silence, BUF_BYTES);
        s_underruns++;
    }
    LWP_ThreadSignal(s_wake);
}

/* Room for another filled buffer.  The one handed over last (s_head - 1)
   may still be playing, so it is never refilled. */
static int can_fill(void) {
    return s_tail - s_head < NBUF - 1u;
}

#ifdef TKDR_AUTOPILOT
uint64_t g_audio_pull_us;   /* time spent producing the stream (demo statistics) */
#endif
static void fill_next(void) {
    int16_t *b = s_buf[s_tail % NBUF];
#ifdef TKDR_AUTOPILOT
    uint64_t t0 = wii_time_us();
#endif
    audio_pull(b, (int)BUF_FRAMES);
    DCFlushRange(b, BUF_BYTES);
    s_tail = s_tail + 1;
#ifdef TKDR_AUTOPILOT
    g_audio_pull_us += wii_time_us() - t0;
#endif
}

static void *feeder_thread(void *arg) {
    (void)arg;
    while (s_running) {
        while (s_running && can_fill()) fill_next();
        /* A signal that arrives before the sleep is not lost for long: the
           voice asks again as soon as the next buffer has played. */
        LWP_ThreadSleep(s_wake);
    }
    return NULL;
}

bool wii_audio_init(void) {
    for (unsigned i = 0; i < NBUF; i++) {
        s_buf[i] = memalign(32, BUF_BYTES);
        if (!s_buf[i]) {
            wii_log("WII audio: no memory for the stream buffers\n");
            return false;
        }
        memset(s_buf[i], 0, BUF_BYTES);
        DCFlushRange(s_buf[i], BUF_BYTES);
    }
    s_silence = memalign(32, BUF_BYTES);
    if (!s_silence) {
        wii_log("WII audio: no memory for the stream buffers\n");
        return false;
    }
    memset(s_silence, 0, BUF_BYTES);
    DCFlushRange(s_silence, BUF_BYTES);
    s_head = s_tail = 0;

    AESND_Init();
    s_voice = AESND_AllocateVoice(voice_callback);
    if (!s_voice) {
        wii_log("WII audio: no AESND voice\n");
        return false;
    }
    LWP_InitQueue(&s_wake);

    /* Start the voice on silence; the first stream request takes the first
       buffer the feeder has filled by then. */
    AESND_SetVoiceFormat(s_voice, VOICE_STEREO16);
    AESND_SetVoiceFrequency(s_voice, PSP_RATE);
    AESND_SetVoiceVolume(s_voice, 255, 255);
    AESND_SetVoiceStream(s_voice, true);
    AESND_SetVoiceBuffer(s_voice, s_silence, BUF_BYTES);

    s_running = 1;
    void *stack = memalign(32, 32u << 10);
    if (!stack || LWP_CreateThread(&s_feeder, feeder_thread, NULL, stack,
                                   32u << 10, 64) < 0) {
        s_running = 0;
        AESND_FreeVoice(s_voice);
        s_voice = NULL;
        wii_log("WII audio: feeder thread failed\n");
        return false;
    }
    AESND_SetVoiceStop(s_voice, false);
    AESND_Pause(false);
    wii_log("WII audio: 44.1 kHz AESND stream voice (DSP rate conversion)\n");
    return true;
}

void wii_audio_update(void) {
    /* The feeder is a host thread driven by the DSP; nothing to do per frame. */
}

void wii_audio_shutdown(void) {
    s_running = 0;
    if (s_wake != LWP_TQUEUE_NULL) LWP_ThreadSignal(s_wake);
    if (s_voice) AESND_SetVoiceStop(s_voice, true);
    AESND_Pause(true);
}

uint32_t wii_audio_underruns(void) { return s_underruns; }
