#include <stdio.h>
#include <string.h>

#include <gccore.h>
#include <ogc/pad.h>
#include <wiiuse/wpad.h>

#include "wii_platform.h"

/* PSP SceCtrlData bits */
#define PSP_SELECT   0x0001
#define PSP_START    0x0008
#define PSP_UP       0x0010
#define PSP_RIGHT    0x0020
#define PSP_DOWN     0x0040
#define PSP_LEFT     0x0080
#define PSP_L        0x0100
#define PSP_R        0x0200
#define PSP_TRIANGLE 0x1000
#define PSP_CIRCLE   0x2000
#define PSP_CROSS    0x4000
#define PSP_SQUARE   0x8000

static uint32_t s_buttons;
static uint8_t s_ax = 128, s_ay = 128;
/* The Wii power button is reported through a callback, and the emulated
   console can deliver a stray event.  Keep it as a per-frame state: a real
   power press is held down, so the button bit repeats every frame. */
static volatile int s_power_event;
static int s_home;

static void power_cb(s32 chan) {
    (void)chan;
    s_power_event = 1;
}

static uint8_t axis_from_stick(int v, int center, int range, int invert) {
    int d = v - center;
    if (invert) d = -d;
    if (d > range) d = range;
    if (d < -range) d = -range;
    int out = 128 + d * 127 / (range ? range : 1);
    if (out < 0) out = 0;
    if (out > 255) out = 255;
    return (uint8_t)out;
}

/* The GameCube stick reports -128..127 with +Y up.  The PSP's analog axes are
   0..255 with 128 at rest and +Y down, so the Y axis has to be flipped (the
   Wii extension paths do the same through axis_from_stick's invert flag). */
static uint8_t axis_from_gc(s8 v, int invert) {
    int out = 128 + (invert ? -(int)v : (int)v);
    if (out < 0) out = 0;
    if (out > 255) out = 255;
    return (uint8_t)out;
}

/* Raw stick travel below this is treated as centred, so a worn stick's
   jitter does not override a connected Wii extension's stick. */
#define GC_STICK_DEADZONE 8

void wii_input_init(void) {
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetPowerButtonCallback(power_cb);
    PAD_Init();
}

static void scan_wpad(void) {
    WPAD_ScanPads();
    uint32_t held = WPAD_ButtonsHeld(WPAD_CHAN_0);
    const WPADData *d = WPAD_Data(WPAD_CHAN_0);

    if (held & WPAD_BUTTON_LEFT)  s_buttons |= PSP_LEFT;
    if (held & WPAD_BUTTON_RIGHT) s_buttons |= PSP_RIGHT;
    if (held & WPAD_BUTTON_UP)    s_buttons |= PSP_UP;
    if (held & WPAD_BUTTON_DOWN)  s_buttons |= PSP_DOWN;
    if (held & WPAD_BUTTON_1)     s_buttons |= PSP_SQUARE;
    if (held & WPAD_BUTTON_2)     s_buttons |= PSP_CROSS;
    if (held & WPAD_BUTTON_A)     s_buttons |= PSP_CIRCLE;
    if (held & WPAD_BUTTON_B)     s_buttons |= PSP_TRIANGLE;
    if (held & WPAD_BUTTON_PLUS)  s_buttons |= PSP_START;
    if (held & WPAD_BUTTON_MINUS) s_buttons |= PSP_SELECT;
    if (held & WPAD_BUTTON_HOME)  s_home = 1;

    if (!d) return;

    switch (d->exp.type) {
    case WPAD_EXP_NUNCHUK: {
        const struct nunchuk_t *n = &d->exp.nunchuk;
        if (n->btns_held & WPAD_NUNCHUK_BUTTON_Z) s_buttons |= PSP_L;
        if (n->btns_held & WPAD_NUNCHUK_BUTTON_C) s_buttons |= PSP_R;
        int cx = n->js.center.x, cy = n->js.center.y;
        int rx = n->js.max.x - cx, ry = n->js.max.y - cy;
        if (rx < 1) rx = 1;
        if (ry < 1) ry = 1;
        s_ax = axis_from_stick(n->js.pos.x, cx, rx, 0);
        s_ay = axis_from_stick(n->js.pos.y, cy, ry, 1);
        break;
    }
    case WPAD_EXP_CLASSIC: {
        const struct classic_ctrl_t *c = &d->exp.classic;
        uint32_t b = c->btns_held;
        if (b & WPAD_CLASSIC_BUTTON_UP)    s_buttons |= PSP_UP;
        if (b & WPAD_CLASSIC_BUTTON_RIGHT) s_buttons |= PSP_RIGHT;
        if (b & WPAD_CLASSIC_BUTTON_DOWN)  s_buttons |= PSP_DOWN;
        if (b & WPAD_CLASSIC_BUTTON_LEFT)  s_buttons |= PSP_LEFT;
        if (b & WPAD_CLASSIC_BUTTON_A)     s_buttons |= PSP_CIRCLE;
        if (b & WPAD_CLASSIC_BUTTON_B)     s_buttons |= PSP_CROSS;
        if (b & WPAD_CLASSIC_BUTTON_X)     s_buttons |= PSP_TRIANGLE;
        if (b & WPAD_CLASSIC_BUTTON_Y)     s_buttons |= PSP_SQUARE;
        if (b & (WPAD_CLASSIC_BUTTON_FULL_L | WPAD_CLASSIC_BUTTON_ZL)) s_buttons |= PSP_L;
        if (b & (WPAD_CLASSIC_BUTTON_FULL_R | WPAD_CLASSIC_BUTTON_ZR)) s_buttons |= PSP_R;
        if (b & WPAD_CLASSIC_BUTTON_PLUS)  s_buttons |= PSP_START;
        if (b & WPAD_CLASSIC_BUTTON_MINUS) s_buttons |= PSP_SELECT;
        int cx = c->ljs.center.x, cy = c->ljs.center.y;
        int rx = c->ljs.max.x - cx, ry = c->ljs.max.y - cy;
        if (rx < 1) rx = 1;
        if (ry < 1) ry = 1;
        s_ax = axis_from_stick(c->ljs.pos.x, cx, rx, 0);
        s_ay = axis_from_stick(c->ljs.pos.y, cy, ry, 1);
        break;
    }
    default:
        break;
    }
}

static void scan_gc(void) {
    /* With no pad in port 1 the buttons read 0 and the stick sits inside the
       dead zone, so the Wii extensions' values are left alone. */
    PAD_ScanPads();
    uint16_t b = PAD_ButtonsHeld(PAD_CHAN0);
    if (b & PAD_BUTTON_UP)    s_buttons |= PSP_UP;
    if (b & PAD_BUTTON_RIGHT) s_buttons |= PSP_RIGHT;
    if (b & PAD_BUTTON_DOWN)  s_buttons |= PSP_DOWN;
    if (b & PAD_BUTTON_LEFT)  s_buttons |= PSP_LEFT;
    if (b & PAD_BUTTON_A)     s_buttons |= PSP_CIRCLE;
    if (b & PAD_BUTTON_B)     s_buttons |= PSP_CROSS;
    if (b & PAD_BUTTON_X)     s_buttons |= PSP_TRIANGLE;
    if (b & PAD_BUTTON_Y)     s_buttons |= PSP_SQUARE;
    if (b & PAD_TRIGGER_L)    s_buttons |= PSP_L;
    if (b & PAD_TRIGGER_R)    s_buttons |= PSP_R;
    if (b & PAD_BUTTON_START) s_buttons |= PSP_START;
    if (b & PAD_TRIGGER_Z)    s_buttons |= PSP_SELECT;
    /* The stick is read whether or not a button is held; it used to be
       skipped unless one was. */
    int sx = PAD_StickX(PAD_CHAN0), sy = PAD_StickY(PAD_CHAN0);
    if (sx > GC_STICK_DEADZONE || sx < -GC_STICK_DEADZONE ||
        sy > GC_STICK_DEADZONE || sy < -GC_STICK_DEADZONE) {
        s_ax = axis_from_gc((s8)sx, 0);
        s_ay = axis_from_gc((s8)sy, 1);
    }
}

/* gcc 15.2 dies in cprop/try_forward_edges on this function's CFG (the same
   internal compiler error as draw_line and texreplace.c), so build it at O1. */
__attribute__((optimize("O1")))
void wii_input_scan(void) {
    s_buttons = 0;
    s_ax = 128;
    s_ay = 128;
    s_home = 0;
    scan_wpad();
    scan_gc();
    if (s_power_event) {  // a one-shot event; only trust it while held
        s_power_event = 0;
        s_home = 1;
    }
}

uint32_t wii_input_buttons(void) { return s_buttons; }
void wii_input_analog(uint8_t *x, uint8_t *y) {
    if (x) *x = s_ax;
    if (y) *y = s_ay;
}
bool wii_input_home_pressed(void) { return s_home != 0; }
