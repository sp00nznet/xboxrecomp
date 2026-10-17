/**
 * xbox_input -- the host -> Duke mapping table and its pure helpers.
 * See input_map.h. No platform calls here: tests/input_map links this file
 * alone on every host.
 */
#include "input_map.h"
#include "recomp_env.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROW_BTN(host, kind, guest, xi, sdl, gname, val) \
    { host, kind, guest, xi, sdl, 0, gname, val }

/* The order is the README's and the one tests/input_map and the Proton
 * check (scripts/vpad.py seq) walk. */
const xbox_input_map_row xbox_input_map[] = {
    ROW_BTN("A",                XBOX_MAP_ANALOG,  XBOX_BUTTON_A,     XBOX_XI_A,     XBOX_SDL_BUTTON_A,     "bAnalogButtons[A]",     "255 held, 0 released"),
    ROW_BTN("B",                XBOX_MAP_ANALOG,  XBOX_BUTTON_B,     XBOX_XI_B,     XBOX_SDL_BUTTON_B,     "bAnalogButtons[B]",     "255 held, 0 released"),
    ROW_BTN("X",                XBOX_MAP_ANALOG,  XBOX_BUTTON_X,     XBOX_XI_X,     XBOX_SDL_BUTTON_X,     "bAnalogButtons[X]",     "255 held, 0 released"),
    ROW_BTN("Y",                XBOX_MAP_ANALOG,  XBOX_BUTTON_Y,     XBOX_XI_Y,     XBOX_SDL_BUTTON_Y,     "bAnalogButtons[Y]",     "255 held, 0 released"),
    ROW_BTN("Left shoulder (LB)",  XBOX_MAP_ANALOG, XBOX_BUTTON_WHITE, XBOX_XI_LEFT_SHOULDER,  XBOX_SDL_BUTTON_LEFTSHOULDER,  "bAnalogButtons[WHITE]", "255 held, 0 released"),
    ROW_BTN("Right shoulder (RB)", XBOX_MAP_ANALOG, XBOX_BUTTON_BLACK, XBOX_XI_RIGHT_SHOULDER, XBOX_SDL_BUTTON_RIGHTSHOULDER, "bAnalogButtons[BLACK]", "255 held, 0 released"),
    ROW_BTN("Start / Menu",     XBOX_MAP_DIGITAL, XBOX_GAMEPAD_START,      XBOX_XI_START,      XBOX_SDL_BUTTON_START,      "wButtons START",       "bit set while held"),
    ROW_BTN("Back / View",      XBOX_MAP_DIGITAL, XBOX_GAMEPAD_BACK,       XBOX_XI_BACK,       XBOX_SDL_BUTTON_BACK,       "wButtons BACK",        "bit set while held"),
    ROW_BTN("Left stick click", XBOX_MAP_DIGITAL, XBOX_GAMEPAD_LEFT_THUMB, XBOX_XI_LEFT_THUMB, XBOX_SDL_BUTTON_LEFTSTICK,  "wButtons LEFT_THUMB",  "bit set while held"),
    ROW_BTN("Right stick click",XBOX_MAP_DIGITAL, XBOX_GAMEPAD_RIGHT_THUMB,XBOX_XI_RIGHT_THUMB,XBOX_SDL_BUTTON_RIGHTSTICK, "wButtons RIGHT_THUMB", "bit set while held"),
    ROW_BTN("D-pad up",         XBOX_MAP_DIGITAL, XBOX_GAMEPAD_DPAD_UP,    XBOX_XI_DPAD_UP,    XBOX_SDL_BUTTON_DPAD_UP,    "wButtons DPAD_UP",     "bit set while held"),
    ROW_BTN("D-pad down",       XBOX_MAP_DIGITAL, XBOX_GAMEPAD_DPAD_DOWN,  XBOX_XI_DPAD_DOWN,  XBOX_SDL_BUTTON_DPAD_DOWN,  "wButtons DPAD_DOWN",   "bit set while held"),
    ROW_BTN("D-pad left",       XBOX_MAP_DIGITAL, XBOX_GAMEPAD_DPAD_LEFT,  XBOX_XI_DPAD_LEFT,  XBOX_SDL_BUTTON_DPAD_LEFT,  "wButtons DPAD_LEFT",   "bit set while held"),
    ROW_BTN("D-pad right",      XBOX_MAP_DIGITAL, XBOX_GAMEPAD_DPAD_RIGHT, XBOX_XI_DPAD_RIGHT, XBOX_SDL_BUTTON_DPAD_RIGHT, "wButtons DPAD_RIGHT",  "bit set while held"),
    ROW_BTN("Left trigger",     XBOX_MAP_TRIGGER, XBOX_BUTTON_LTRIGGER, XBOX_XI_SRC_LT, XBOX_SDL_AXIS_TRIGGERLEFT,  "bAnalogButtons[LTRIGGER]", "0..255"),
    ROW_BTN("Right trigger",    XBOX_MAP_TRIGGER, XBOX_BUTTON_RTRIGGER, XBOX_XI_SRC_RT, XBOX_SDL_AXIS_TRIGGERRIGHT, "bAnalogButtons[RTRIGGER]", "0..255"),
    { "Left stick X",  XBOX_MAP_STICK, 0, XBOX_XI_SRC_LX, XBOX_SDL_AXIS_LEFTX,  0, "sThumbLX", "-32768..32767, right positive" },
    { "Left stick Y",  XBOX_MAP_STICK, 1, XBOX_XI_SRC_LY, XBOX_SDL_AXIS_LEFTY,  1, "sThumbLY", "-32768..32767, up positive" },
    { "Right stick X", XBOX_MAP_STICK, 2, XBOX_XI_SRC_RX, XBOX_SDL_AXIS_RIGHTX, 0, "sThumbRX", "-32768..32767, right positive" },
    { "Right stick Y", XBOX_MAP_STICK, 3, XBOX_XI_SRC_RY, XBOX_SDL_AXIS_RIGHTY, 1, "sThumbRY", "-32768..32767, up positive" },
};
const size_t xbox_input_map_count = sizeof xbox_input_map / sizeof xbox_input_map[0];

/* ── deadzone ─────────────────────────────────────────────────────────── */

static int g_deadzone = -1;          /* -1: not read yet */

int xbox_InputDeadzone(void)
{
    if (g_deadzone < 0) {
        const char *v = recomp_env(RENV_PAD_DEADZONE);
        long n = (v && *v) ? strtol(v, NULL, 10) : 0;
        g_deadzone = n < 0 ? 0 : n > 32767 ? 32767 : (int)n;
    }
    return g_deadzone;
}

void xbox_InputSetDeadzone(int dz)
{
    g_deadzone = dz < 0 ? -1 : dz > 32767 ? 32767 : dz;
}

static SHORT clamp16(double v)
{
    v = v < 0 ? ceil(v - 0.5) : floor(v + 0.5);
    return (SHORT)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

void xbox_InputApplyDeadzone(SHORT *x, SHORT *y, int dz)
{
    double fx = *x, fy = *y, m, mc, f;

    if (dz <= 0)
        return;
    if (dz > 32766)
        dz = 32766;
    m = sqrt(fx * fx + fy * fy);
    if (m <= dz) {
        *x = 0;
        *y = 0;
        return;
    }
    mc = m > 32767 ? 32767 : m;
    f = (mc - dz) / (32767.0 - dz) * (32767.0 / mc);
    *x = clamp16(fx * f);
    *y = clamp16(fy * f);
}

/* ── the two helpers ──────────────────────────────────────────────────── */

static void put_stick(XBOX_GAMEPAD *g, int axis, SHORT v)
{
    switch (axis) {
    case 0: g->sThumbLX = v; break;
    case 1: g->sThumbLY = v; break;
    case 2: g->sThumbRX = v; break;
    default: g->sThumbRY = v; break;
    }
}

static void finish(XBOX_GAMEPAD *g)
{
    int dz = xbox_InputDeadzone();
    xbox_InputApplyDeadzone(&g->sThumbLX, &g->sThumbLY, dz);
    xbox_InputApplyDeadzone(&g->sThumbRX, &g->sThumbRY, dz);
}

void xbox_InputMapXInput(const XBOX_HOST_XINPUT_GAMEPAD *in, XBOX_GAMEPAD *out)
{
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < xbox_input_map_count; i++) {
        const xbox_input_map_row *r = &xbox_input_map[i];
        switch (r->kind) {
        case XBOX_MAP_DIGITAL:
            if (in->wButtons & r->xinput) out->wButtons |= r->guest;
            break;
        case XBOX_MAP_ANALOG:
            out->bAnalogButtons[r->guest] = (in->wButtons & r->xinput) ? 255 : 0;
            break;
        case XBOX_MAP_TRIGGER:
            out->bAnalogButtons[r->guest] =
                r->xinput == XBOX_XI_SRC_LT ? in->bLeftTrigger : in->bRightTrigger;
            break;
        case XBOX_MAP_STICK: {
            int16_t v = r->xinput == XBOX_XI_SRC_LX ? in->sThumbLX
                      : r->xinput == XBOX_XI_SRC_LY ? in->sThumbLY
                      : r->xinput == XBOX_XI_SRC_RX ? in->sThumbRX
                      : in->sThumbRY;
            put_stick(out, r->guest, v);
            break;
        }
        }
    }
    finish(out);
}

void xbox_InputMapSdl(const XBOX_HOST_SDL_GAMEPAD *in, XBOX_GAMEPAD *out)
{
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < xbox_input_map_count; i++) {
        const xbox_input_map_row *r = &xbox_input_map[i];
        switch (r->kind) {
        case XBOX_MAP_DIGITAL:
            if (in->button[r->sdl]) out->wButtons |= r->guest;
            break;
        case XBOX_MAP_ANALOG:
            out->bAnalogButtons[r->guest] = in->button[r->sdl] ? 255 : 0;
            break;
        case XBOX_MAP_TRIGGER: {
            /* SDL triggers are 0..32767; >> 7 is 0..255. */
            int v = in->axis[r->sdl];
            out->bAnalogButtons[r->guest] = (BYTE)(v < 0 ? 0 : v >> 7);
            break;
        }
        case XBOX_MAP_STICK: {
            int v = in->axis[r->sdl];
            /* (-1 - v): -32768 <-> 32767, no overflow. */
            put_stick(out, r->guest, (SHORT)(r->sdl_invert ? -1 - v : v));
            break;
        }
        }
    }
    finish(out);
}

/* ── the README table ─────────────────────────────────────────────────── */

static const char *xinput_name(const xbox_input_map_row *r)
{
    static const struct { uint16_t bit; const char *name; } bits[] = {
        { XBOX_XI_DPAD_UP, "DPAD_UP" }, { XBOX_XI_DPAD_DOWN, "DPAD_DOWN" },
        { XBOX_XI_DPAD_LEFT, "DPAD_LEFT" }, { XBOX_XI_DPAD_RIGHT, "DPAD_RIGHT" },
        { XBOX_XI_START, "START" }, { XBOX_XI_BACK, "BACK" },
        { XBOX_XI_LEFT_THUMB, "LEFT_THUMB" }, { XBOX_XI_RIGHT_THUMB, "RIGHT_THUMB" },
        { XBOX_XI_LEFT_SHOULDER, "LEFT_SHOULDER" },
        { XBOX_XI_RIGHT_SHOULDER, "RIGHT_SHOULDER" },
        { XBOX_XI_A, "A" }, { XBOX_XI_B, "B" }, { XBOX_XI_X, "X" }, { XBOX_XI_Y, "Y" },
    };
    static const char *const src[] = { "bLeftTrigger", "bRightTrigger",
        "sThumbLX", "sThumbLY", "sThumbRX", "sThumbRY" };
    if (r->kind == XBOX_MAP_TRIGGER || r->kind == XBOX_MAP_STICK)
        return r->xinput < 6 ? src[r->xinput] : "?";
    for (size_t i = 0; i < sizeof bits / sizeof bits[0]; i++)
        if (bits[i].bit == r->xinput) return bits[i].name;
    return "?";
}

static const char *sdl_name(const xbox_input_map_row *r)
{
    static const char *const btn[XBOX_SDL_BUTTON_COUNT] = { "A", "B", "X", "Y",
        "BACK", "GUIDE", "START", "LEFTSTICK", "RIGHTSTICK", "LEFTSHOULDER",
        "RIGHTSHOULDER", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT" };
    static const char *const ax[XBOX_SDL_AXIS_COUNT] = { "LEFTX", "LEFTY",
        "RIGHTX", "RIGHTY", "TRIGGERLEFT", "TRIGGERRIGHT" };
    if (r->sdl < 0) return "?";
    if (r->kind == XBOX_MAP_TRIGGER || r->kind == XBOX_MAP_STICK)
        return r->sdl < XBOX_SDL_AXIS_COUNT ? ax[r->sdl] : "?";
    return r->sdl < XBOX_SDL_BUTTON_COUNT ? btn[r->sdl] : "?";
}

size_t xbox_InputMapReadmeRow(size_t i, char *buf, size_t size)
{
    int n;
    if (i == 0)
        n = snprintf(buf, size, "| Host control | Guest field | Guest value | XInput | SDL2 |");
    else if (i == 1)
        n = snprintf(buf, size, "|---|---|---|---|---|");
    else if (i - 2 < xbox_input_map_count) {
        const xbox_input_map_row *r = &xbox_input_map[i - 2];
        n = snprintf(buf, size, "| %s | `%s` | %s | `%s` | `%s`%s |",
                     r->host, r->guest_name, r->value, xinput_name(r),
                     sdl_name(r), r->sdl_invert ? " as `-1 - v`" : "");
    } else
        return 0;
    return n < 0 ? 0 : (size_t)n;
}
