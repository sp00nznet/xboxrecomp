/**
 * xbox_input -- the host -> Duke mapping table and its pure helpers.
 * See pad_map.h. No platform calls here: tests/pad_map links this file
 * alone on every host.
 */
#include "pad_map.h"
#include "input_map.h"
#include "host_pad.h"
#include "recomp_env.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROW_BTN(host, kind, guest, xi, sdl, gname, val) \
    { host, kind, guest, xi, sdl, 0, gname, val }

/* The order is the README's and the one tests/pad_map and the Proton
 * check (scripts/vpad.py seq) walk. The host names are upstream's pad
 * sources (xbox_InputMap_SourceName), positional as SDL's are: south is the
 * bottom face button, an Xbox A. */
const xbox_pad_map_row xbox_pad_map[] = {
    ROW_BTN("south",            XBOX_MAP_ANALOG,  XBOX_BUTTON_A,     XBOX_XI_A,     XBOX_SDL_BUTTON_A,     "bAnalogButtons[A]",     "255 held, 0 released"),
    ROW_BTN("east",             XBOX_MAP_ANALOG,  XBOX_BUTTON_B,     XBOX_XI_B,     XBOX_SDL_BUTTON_B,     "bAnalogButtons[B]",     "255 held, 0 released"),
    ROW_BTN("west",             XBOX_MAP_ANALOG,  XBOX_BUTTON_X,     XBOX_XI_X,     XBOX_SDL_BUTTON_X,     "bAnalogButtons[X]",     "255 held, 0 released"),
    ROW_BTN("north",            XBOX_MAP_ANALOG,  XBOX_BUTTON_Y,     XBOX_XI_Y,     XBOX_SDL_BUTTON_Y,     "bAnalogButtons[Y]",     "255 held, 0 released"),
    ROW_BTN("left_shoulder",    XBOX_MAP_ANALOG,  XBOX_BUTTON_WHITE, XBOX_XI_LEFT_SHOULDER,  XBOX_SDL_BUTTON_LEFTSHOULDER,  "bAnalogButtons[WHITE]", "255 held, 0 released"),
    ROW_BTN("right_shoulder",   XBOX_MAP_ANALOG,  XBOX_BUTTON_BLACK, XBOX_XI_RIGHT_SHOULDER, XBOX_SDL_BUTTON_RIGHTSHOULDER, "bAnalogButtons[BLACK]", "255 held, 0 released"),
    ROW_BTN("start",            XBOX_MAP_DIGITAL, XBOX_GAMEPAD_START,      XBOX_XI_START,      XBOX_SDL_BUTTON_START,      "wButtons START",       "bit set while held"),
    ROW_BTN("back",             XBOX_MAP_DIGITAL, XBOX_GAMEPAD_BACK,       XBOX_XI_BACK,       XBOX_SDL_BUTTON_BACK,       "wButtons BACK",        "bit set while held"),
    ROW_BTN("left_stick_click", XBOX_MAP_DIGITAL, XBOX_GAMEPAD_LEFT_THUMB, XBOX_XI_LEFT_THUMB, XBOX_SDL_BUTTON_LEFTSTICK,  "wButtons LEFT_THUMB",  "bit set while held"),
    ROW_BTN("right_stick_click",XBOX_MAP_DIGITAL, XBOX_GAMEPAD_RIGHT_THUMB,XBOX_XI_RIGHT_THUMB,XBOX_SDL_BUTTON_RIGHTSTICK, "wButtons RIGHT_THUMB", "bit set while held"),
    ROW_BTN("dpad_up",          XBOX_MAP_DIGITAL, XBOX_GAMEPAD_DPAD_UP,    XBOX_XI_DPAD_UP,    XBOX_SDL_BUTTON_DPAD_UP,    "wButtons DPAD_UP",     "bit set while held"),
    ROW_BTN("dpad_down",        XBOX_MAP_DIGITAL, XBOX_GAMEPAD_DPAD_DOWN,  XBOX_XI_DPAD_DOWN,  XBOX_SDL_BUTTON_DPAD_DOWN,  "wButtons DPAD_DOWN",   "bit set while held"),
    ROW_BTN("dpad_left",        XBOX_MAP_DIGITAL, XBOX_GAMEPAD_DPAD_LEFT,  XBOX_XI_DPAD_LEFT,  XBOX_SDL_BUTTON_DPAD_LEFT,  "wButtons DPAD_LEFT",   "bit set while held"),
    ROW_BTN("dpad_right",       XBOX_MAP_DIGITAL, XBOX_GAMEPAD_DPAD_RIGHT, XBOX_XI_DPAD_RIGHT, XBOX_SDL_BUTTON_DPAD_RIGHT, "wButtons DPAD_RIGHT",  "bit set while held"),
    ROW_BTN("left_trigger",     XBOX_MAP_TRIGGER, XBOX_BUTTON_LTRIGGER, XBOX_XI_SRC_LT, XBOX_SDL_AXIS_TRIGGERLEFT,  "bAnalogButtons[LTRIGGER]", "0..255"),
    ROW_BTN("right_trigger",    XBOX_MAP_TRIGGER, XBOX_BUTTON_RTRIGGER, XBOX_XI_SRC_RT, XBOX_SDL_AXIS_TRIGGERRIGHT, "bAnalogButtons[RTRIGGER]", "0..255"),
    { "left_stick_left / left_stick_right",   XBOX_MAP_STICK, 0, XBOX_XI_SRC_LX, XBOX_SDL_AXIS_LEFTX,  0, "sThumbLX", "-32768..32767, right positive" },
    { "left_stick_down / left_stick_up",      XBOX_MAP_STICK, 1, XBOX_XI_SRC_LY, XBOX_SDL_AXIS_LEFTY,  1, "sThumbLY", "-32768..32767, up positive" },
    { "right_stick_left / right_stick_right", XBOX_MAP_STICK, 2, XBOX_XI_SRC_RX, XBOX_SDL_AXIS_RIGHTX, 0, "sThumbRX", "-32768..32767, right positive" },
    { "right_stick_down / right_stick_up",    XBOX_MAP_STICK, 3, XBOX_XI_SRC_RY, XBOX_SDL_AXIS_RIGHTY, 1, "sThumbRY", "-32768..32767, up positive" },
};
const size_t xbox_pad_map_count = sizeof xbox_pad_map / sizeof xbox_pad_map[0];

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

void xbox_PadMapXInput(const XBOX_HOST_XINPUT_GAMEPAD *in, XBOX_GAMEPAD *out)
{
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < xbox_pad_map_count; i++) {
        const xbox_pad_map_row *r = &xbox_pad_map[i];
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

void xbox_PadMapSdl(const XBOX_HOST_SDL_GAMEPAD *in, XBOX_GAMEPAD *out)
{
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < xbox_pad_map_count; i++) {
        const xbox_pad_map_row *r = &xbox_pad_map[i];
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

/* ── the InputRaw adapter ─────────────────────────────────────────────── */

#define RAW_ON 32767

/* The XInput button bits and SDL buttons behind each button source; the
 * order is pad_map[]'s, which tests/pad_map holds the adapter to. */
static const struct { uint8_t src; uint16_t xi; int8_t sdl; } k_raw_btn[] = {
    { INPUT_SRC_SOUTH,             XBOX_XI_A,              XBOX_SDL_BUTTON_A },
    { INPUT_SRC_EAST,              XBOX_XI_B,              XBOX_SDL_BUTTON_B },
    { INPUT_SRC_WEST,              XBOX_XI_X,              XBOX_SDL_BUTTON_X },
    { INPUT_SRC_NORTH,             XBOX_XI_Y,              XBOX_SDL_BUTTON_Y },
    { INPUT_SRC_LEFT_SHOULDER,     XBOX_XI_LEFT_SHOULDER,  XBOX_SDL_BUTTON_LEFTSHOULDER },
    { INPUT_SRC_RIGHT_SHOULDER,    XBOX_XI_RIGHT_SHOULDER, XBOX_SDL_BUTTON_RIGHTSHOULDER },
    { INPUT_SRC_START,             XBOX_XI_START,          XBOX_SDL_BUTTON_START },
    { INPUT_SRC_BACK,              XBOX_XI_BACK,           XBOX_SDL_BUTTON_BACK },
    { INPUT_SRC_LEFT_STICK_CLICK,  XBOX_XI_LEFT_THUMB,     XBOX_SDL_BUTTON_LEFTSTICK },
    { INPUT_SRC_RIGHT_STICK_CLICK, XBOX_XI_RIGHT_THUMB,    XBOX_SDL_BUTTON_RIGHTSTICK },
    { INPUT_SRC_DPAD_UP,           XBOX_XI_DPAD_UP,        XBOX_SDL_BUTTON_DPAD_UP },
    { INPUT_SRC_DPAD_DOWN,         XBOX_XI_DPAD_DOWN,      XBOX_SDL_BUTTON_DPAD_DOWN },
    { INPUT_SRC_DPAD_LEFT,         XBOX_XI_DPAD_LEFT,      XBOX_SDL_BUTTON_DPAD_LEFT },
    { INPUT_SRC_DPAD_RIGHT,        XBOX_XI_DPAD_RIGHT,     XBOX_SDL_BUTTON_DPAD_RIGHT },
};

/* host_pad.h's host-only bits and their sources. */
static const struct { uint32_t bit; uint8_t src; } k_raw_extra[] = {
    { XBOX_HOST_GUIDE,    INPUT_SRC_GUIDE },
    { XBOX_HOST_MISC1,    INPUT_SRC_MISC1 },
    { XBOX_HOST_PADDLE1,  INPUT_SRC_PADDLE1 },
    { XBOX_HOST_PADDLE2,  INPUT_SRC_PADDLE2 },
    { XBOX_HOST_PADDLE3,  INPUT_SRC_PADDLE3 },
    { XBOX_HOST_PADDLE4,  INPUT_SRC_PADDLE4 },
    { XBOX_HOST_TOUCHPAD, INPUT_SRC_TOUCHPAD },
};

#define COUNT(a) (sizeof (a) / sizeof (a)[0])

/* One half-axis as a source: 0 to 32767, -32768 clamped. */
static int16_t half(int v, int positive)
{
    if (!positive)
        v = -v;
    return (int16_t)(v <= 0 ? 0 : v > 32767 ? 32767 : v);
}

static void fill_directions(InputRaw *r)
{
    r->src[INPUT_SRC_LSTICK_UP]    = half(r->ly, 1);
    r->src[INPUT_SRC_LSTICK_DOWN]  = half(r->ly, 0);
    r->src[INPUT_SRC_LSTICK_LEFT]  = half(r->lx, 0);
    r->src[INPUT_SRC_LSTICK_RIGHT] = half(r->lx, 1);
    r->src[INPUT_SRC_RSTICK_UP]    = half(r->ry, 1);
    r->src[INPUT_SRC_RSTICK_DOWN]  = half(r->ry, 0);
    r->src[INPUT_SRC_RSTICK_LEFT]  = half(r->rx, 0);
    r->src[INPUT_SRC_RSTICK_RIGHT] = half(r->rx, 1);
}

static void fill_extra(InputRaw *r, uint32_t extra)
{
    for (size_t i = 0; i < COUNT(k_raw_extra); i++)
        if (extra & k_raw_extra[i].bit)
            r->src[k_raw_extra[i].src] = RAW_ON;
}

/* 0..255 to 0..32767 with >> 7 its exact inverse. */
static int16_t trigger_src(int t)
{
    t = t < 0 ? 0 : t > 255 ? 255 : t;
    return (int16_t)((t << 7) | (t >> 1));
}

void xbox_PadRawXInput(const XBOX_HOST_XINPUT_GAMEPAD *in, uint32_t extra, InputRaw *out)
{
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < COUNT(k_raw_btn); i++)
        if (in->wButtons & k_raw_btn[i].xi)
            out->src[k_raw_btn[i].src] = RAW_ON;
    out->src[INPUT_SRC_LEFT_TRIGGER]  = trigger_src(in->bLeftTrigger);
    out->src[INPUT_SRC_RIGHT_TRIGGER] = trigger_src(in->bRightTrigger);
    out->lx = in->sThumbLX;
    out->ly = in->sThumbLY;
    out->rx = in->sThumbRX;
    out->ry = in->sThumbRY;
    fill_extra(out, extra);
    fill_directions(out);
}

void xbox_PadRawSdl(const XBOX_HOST_SDL_GAMEPAD *in, uint32_t extra, InputRaw *out)
{
    int lt = in->axis[XBOX_SDL_AXIS_TRIGGERLEFT], rt = in->axis[XBOX_SDL_AXIS_TRIGGERRIGHT];

    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < COUNT(k_raw_btn); i++)
        if (in->button[k_raw_btn[i].sdl])
            out->src[k_raw_btn[i].src] = RAW_ON;
    out->src[INPUT_SRC_LEFT_TRIGGER]  = (int16_t)(lt < 0 ? 0 : lt);
    out->src[INPUT_SRC_RIGHT_TRIGGER] = (int16_t)(rt < 0 ? 0 : rt);
    /* SDL's Y points down; (-1 - v) maps -32768 <-> 32767 with no overflow. */
    out->lx = in->axis[XBOX_SDL_AXIS_LEFTX];
    out->ly = (int16_t)(-1 - in->axis[XBOX_SDL_AXIS_LEFTY]);
    out->rx = in->axis[XBOX_SDL_AXIS_RIGHTX];
    out->ry = (int16_t)(-1 - in->axis[XBOX_SDL_AXIS_RIGHTY]);
    fill_extra(out, extra);
    fill_directions(out);
}

void xbox_PadRawFromGamepad(const XBOX_GAMEPAD *g, uint32_t extra, InputRaw *out)
{
    static const uint8_t analog[6] = { INPUT_SRC_SOUTH, INPUT_SRC_EAST, INPUT_SRC_WEST,
        INPUT_SRC_NORTH, INPUT_SRC_RIGHT_SHOULDER, INPUT_SRC_LEFT_SHOULDER };
    static const uint8_t digital[8] = { INPUT_SRC_DPAD_UP, INPUT_SRC_DPAD_DOWN,
        INPUT_SRC_DPAD_LEFT, INPUT_SRC_DPAD_RIGHT, INPUT_SRC_START, INPUT_SRC_BACK,
        INPUT_SRC_LEFT_STICK_CLICK, INPUT_SRC_RIGHT_STICK_CLICK };

    memset(out, 0, sizeof *out);
    for (int b = 0; b < 8; b++)
        if (g->wButtons & (1u << b))
            out->src[digital[b]] = RAW_ON;
    /* XBOX_BUTTON_A.. order: A B X Y BLACK WHITE, and Black is RB. */
    for (int i = 0; i < 6; i++)
        if (g->bAnalogButtons[i])
            out->src[analog[i]] = RAW_ON;
    out->src[INPUT_SRC_LEFT_TRIGGER]  = trigger_src(g->bAnalogButtons[XBOX_BUTTON_LTRIGGER]);
    out->src[INPUT_SRC_RIGHT_TRIGGER] = trigger_src(g->bAnalogButtons[XBOX_BUTTON_RTRIGGER]);
    out->lx = g->sThumbLX;
    out->ly = g->sThumbLY;
    out->rx = g->sThumbRX;
    out->ry = g->sThumbRY;
    fill_extra(out, extra);
    fill_directions(out);
}

static int16_t negate(int16_t v)
{
    return (int16_t)(v == -32768 ? 32767 : -v);
}

void xbox_PadRawInvert(InputRaw *raw, int invert_ly, int invert_ry)
{
    if (!invert_ly && !invert_ry)
        return;
    if (invert_ly)
        raw->ly = negate(raw->ly);
    if (invert_ry)
        raw->ry = negate(raw->ry);
    fill_directions(raw);
}

uint32_t xbox_PadRawExtra(const InputRaw *raw)
{
    uint32_t extra = 0;
    for (size_t i = 0; i < COUNT(k_raw_extra); i++)
        if (raw->src[k_raw_extra[i].src] > 0)
            extra |= k_raw_extra[i].bit;
    return extra;
}

/* ── the setup ([input.pad]) ──────────────────────────────────────────── */

void xbox_PadSetupIdentity(xbox_pad_setup *s)
{
    memset(s, 0, sizeof *s);
    xbox_InputMap_PadMapDefaults(&s->map);
    s->map.source[INPUT_WHITE] = INPUT_SRC_LEFT_SHOULDER;
    s->map.source[INPUT_BLACK] = INPUT_SRC_RIGHT_SHOULDER;
    s->map.deadzone_left = s->map.deadzone_right = 0;
    s->map.trigger_threshold = 0;
    s->alt = s->map;
    memset(s->alt.source, INPUT_SRC_NONE, sizeof s->alt.source);
    s->alt.left_stick = s->alt.right_stick = INPUT_STICK_NONE;
}

int xbox_PadSetupIsIdentity(const xbox_pad_setup *s)
{
    xbox_pad_setup id;
    xbox_PadSetupIdentity(&id);
    return !memcmp(s, &id, sizeof id);
}

static int alt_used(const InputPadMap *m)
{
    for (int c = 0; c < INPUT_PAD_CONTROL_COUNT; c++)
        if (m->source[c] != INPUT_SRC_NONE)
            return 1;
    return m->left_stick != INPUT_STICK_NONE || m->right_stick != INPUT_STICK_NONE;
}

int xbox_PadSetupNeedsExtras(const xbox_pad_setup *s)
{
    for (int c = 0; c < INPUT_PAD_CONTROL_COUNT; c++)
        for (size_t i = 0; i < COUNT(k_raw_extra); i++)
            if (s->map.source[c] == k_raw_extra[i].src || s->alt.source[c] == k_raw_extra[i].src)
                return 1;
    return 0;
}

static void raw_stick(int which, const InputRaw *r, SHORT *x, SHORT *y)
{
    switch (which) {
    case INPUT_STICK_LEFT:  *x = r->lx; *y = r->ly; break;
    case INPUT_STICK_RIGHT: *x = r->rx; *y = r->ry; break;
    default:                *x = 0;     *y = 0;     break;
    }
}

static void map_one(const InputPadMap *m, const InputRaw *raw, XBOX_GAMEPAD *out)
{
    int dz = xbox_InputDeadzone();

    xbox_InputMap_PadMap(m, raw, out);
    if (m->deadzone_left == 0) {
        raw_stick(m->left_stick, raw, &out->sThumbLX, &out->sThumbLY);
        xbox_InputApplyDeadzone(&out->sThumbLX, &out->sThumbLY, dz);
    }
    if (m->deadzone_right == 0) {
        raw_stick(m->right_stick, raw, &out->sThumbRX, &out->sThumbRY);
        xbox_InputApplyDeadzone(&out->sThumbRX, &out->sThumbRY, dz);
    }
}

void xbox_PadSetupApply(const xbox_pad_setup *s, const InputRaw *raw, XBOX_GAMEPAD *out)
{
    map_one(&s->map, raw, out);
    if (alt_used(&s->alt)) {
        XBOX_GAMEPAD a;
        map_one(&s->alt, raw, &a);
        xbox_InputMap_PadMerge(out, &a);
    }
}

/* ── the README table ─────────────────────────────────────────────────── */

static const char *xinput_name(const xbox_pad_map_row *r)
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

static const char *sdl_name(const xbox_pad_map_row *r)
{
    static const char *const btn[XBOX_SDL_BUTTON_COUNT] = { "SOUTH", "EAST",
        "WEST", "NORTH", "BACK", "GUIDE", "START", "LEFT_STICK", "RIGHT_STICK",
        "LEFT_SHOULDER", "RIGHT_SHOULDER", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT",
        "DPAD_RIGHT" };
    static const char *const ax[XBOX_SDL_AXIS_COUNT] = { "LEFTX", "LEFTY",
        "RIGHTX", "RIGHTY", "LEFT_TRIGGER", "RIGHT_TRIGGER" };
    if (r->sdl < 0) return "?";
    if (r->kind == XBOX_MAP_TRIGGER || r->kind == XBOX_MAP_STICK)
        return r->sdl < XBOX_SDL_AXIS_COUNT ? ax[r->sdl] : "?";
    return r->sdl < XBOX_SDL_BUTTON_COUNT ? btn[r->sdl] : "?";
}

/* The [input.bindings] control a row drives, in upstream's names: the
 * wButtons bits are InputControl's first eight in bit order and the analog
 * buttons follow from INPUT_A in the XBOX_BUTTON_* order. */
static const char *control_name(const xbox_pad_map_row *r, char *buf, size_t size)
{
    static const int stick[4][2] = {
        { INPUT_LSTICK_LEFT, INPUT_LSTICK_RIGHT }, { INPUT_LSTICK_DOWN, INPUT_LSTICK_UP },
        { INPUT_RSTICK_LEFT, INPUT_RSTICK_RIGHT }, { INPUT_RSTICK_DOWN, INPUT_RSTICK_UP },
    };
    int c = -1;
    switch (r->kind) {
    case XBOX_MAP_DIGITAL:
        for (int b = 0; b < 8; b++)
            if (r->guest == 1u << b) c = INPUT_DPAD_UP + b;
        break;
    case XBOX_MAP_ANALOG:
    case XBOX_MAP_TRIGGER:
        c = INPUT_A + r->guest;
        break;
    default:
        if (r->guest < 4) {
            snprintf(buf, size, "`%s` / `%s`", xbox_InputMap_ControlName(stick[r->guest][0]),
                     xbox_InputMap_ControlName(stick[r->guest][1]));
            return buf;
        }
    }
    snprintf(buf, size, "`%s`", c >= 0 && xbox_InputMap_ControlName(c)
             ? xbox_InputMap_ControlName(c) : "?");
    return buf;
}

size_t xbox_PadMapReadmeRow(size_t i, char *buf, size_t size)
{
    int n;
    char ctl[64];
    if (i == 0)
        n = snprintf(buf, size, "| Host control | Control | Guest field | Guest value | XInput | SDL3 |");
    else if (i == 1)
        n = snprintf(buf, size, "|---|---|---|---|---|---|");
    else if (i - 2 < xbox_pad_map_count) {
        const xbox_pad_map_row *r = &xbox_pad_map[i - 2];
        n = snprintf(buf, size, "| `%s` | %s | `%s` | %s | `%s` | `%s`%s |",
                     r->host, control_name(r, ctl, sizeof ctl), r->guest_name, r->value,
                     xinput_name(r), sdl_name(r), r->sdl_invert ? " as `-1 - v`" : "");
    } else
        return 0;
    return n < 0 ? 0 : (size_t)n;
}

void xbox_RumbleGateReset(xbox_rumble_gate *g)
{
    memset(g, 0, sizeof *g);
}

void xbox_RumbleGateSet(xbox_rumble_gate *g, uint16_t left, uint16_t right)
{
    g->want_l = left;
    g->want_r = right;
}

/* The level as the pad receives it. The 8BitDo HIDAPI driver writes only
 * the high byte of each motor, and the other HIDAPI drivers send 8 bits or
 * fewer as far as known (PlayStation and Xbox 360 take the high byte, Xbox
 * One scales to 0-100, GameCube and Wii are on or off), so two levels with
 * the same high byte are the same report, and SDL, which compares all 16
 * bits, would still write it. A driver with finer steps (GCController
 * haptics) loses steps under 1/256 of full scale, which no pad motor
 * renders. Any non-zero level stays non-zero, so a stop is never mistaken
 * for a still-running level. */
static unsigned rumble_wire(uint16_t v)
{
    return v ? ((v >> 8) ? (v >> 8) : 1u) : 0u;
}

int xbox_RumbleGatePoll(xbox_rumble_gate *g, uint64_t now_ms, int asked,
                        uint16_t *left, uint16_t *right)
{
    int same = rumble_wire(g->want_l) == rumble_wire(g->sent_l)
            && rumble_wire(g->want_r) == rumble_wire(g->sent_r);
    int stop = !g->want_l && !g->want_r;
    /* 0 means never, so a clock at 0 still counts as a send. */
    uint64_t at = now_ms ? now_ms : 1;

    if (same) {
        /* Still is still, and only the title's asks keep a level alive. */
        if (!asked || stop)
            return 0;
        if (now_ms - g->renew_ms < XBOX_RUMBLE_REFRESH_MS)
            return 0;
        /* Renew with the level SDL already has, so it only extends the
         * duration and writes nothing. A renewal is no change: it leaves
         * sent_ms alone, so it never delays a real change after it. */
        g->renew_ms = at;
        *left = g->sent_l;
        *right = g->sent_r;
        return 1;
    }
    /* A stop goes at once: it is one report, never the stream, and the one
     * that must land (xbox_InputShutdown sends it with no loop after it). */
    if (!stop && g->sent_ms && now_ms - g->sent_ms < XBOX_RUMBLE_MIN_MS)
        return 0;
    g->sent_l = g->want_l;
    g->sent_r = g->want_r;
    g->sent_ms = g->renew_ms = at;
    *left = g->sent_l;
    *right = g->sent_r;
    return 1;
}
