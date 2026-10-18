/*
 * The keyboard as a pad on port 0. See keyboard.h.
 *
 * A title that waits on PRESS START is unreachable on a machine with no
 * controller plugged in, which is most machines someone brings this up on.
 * The whole point of a recompilation is to be able to look at the thing
 * running, and a build nobody can press a button in cannot be looked at.
 *
 * Off by default, because a keyboard silently acting as player 1 is
 * surprising when a real pad is what you meant to use. RECOMP_KEYBOARD=1
 * turns it on. It only ever answers for port 0, and is merged on top of a
 * pad connected there (input_core.c), so a real controller keeps working.
 *
 * Keys come from the game's window, which only receives them while it has
 * the focus, so typing in another window does not drive the game. Losing
 * the focus releases every key, or a key held across Alt-Tab or Cmd-Tab
 * would stay held for ever.
 */
#include "keyboard.h"
#include "input_names.h"
#include "mouse.h"
#include "recomp_env.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Reading this needs no lock. Each entry is written only by the window's
 * thread and read only by the game's poll, one byte at a time, and a press
 * seen a poll late is indistinguishable from one made a poll later. */
static volatile unsigned char s_key_down[256];
/* Which Return is held: the main key and keypad Enter both hold VK_RETURN,
 * as Win32 reports them, and either alone must keep it held. */
static volatile unsigned char s_return_main, s_return_kp;

void xbox_InputKeySet(int vk, int down)
{
    if ((unsigned)vk < 256)
        s_key_down[vk] = down ? 1 : 0;
}

void xbox_InputKeysClear(void)
{
    memset((void *)s_key_down, 0, sizeof s_key_down);
    s_return_main = s_return_kp = 0;
}

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk > 255)
        return 0;
    return s_key_down[vk] != 0;
}

/* The one map, on every host. Z X C V for A B X Y is the usual emulator
 * row. The left thumb is W/S/A/D, because Mac laptops have no numeric
 * keypad and the game walks on the stick, not the d-pad; the keypad row of
 * the first map stays as a second one. Arrows stay the d-pad: as a stick
 * too they would feed the menus two sources for one press. */
const xbox_key_row xbox_key_map[] = {
    { "Up arrow",        { XBOX_VK_UP },     XBOX_KEY_BUTTON, XBOX_GAMEPAD_DPAD_UP,     0, "D-pad up" },
    { "Down arrow",      { XBOX_VK_DOWN },   XBOX_KEY_BUTTON, XBOX_GAMEPAD_DPAD_DOWN,   0, "D-pad down" },
    { "Left arrow",      { XBOX_VK_LEFT },   XBOX_KEY_BUTTON, XBOX_GAMEPAD_DPAD_LEFT,   0, "D-pad left" },
    { "Right arrow",     { XBOX_VK_RIGHT },  XBOX_KEY_BUTTON, XBOX_GAMEPAD_DPAD_RIGHT,  0, "D-pad right" },
    { "Enter",           { XBOX_VK_RETURN }, XBOX_KEY_BUTTON, XBOX_GAMEPAD_START,       0, "START" },
    { "Backspace",       { XBOX_VK_BACK },   XBOX_KEY_BUTTON, XBOX_GAMEPAD_BACK,        0, "BACK" },
    { "Shift",           { XBOX_VK_SHIFT },  XBOX_KEY_BUTTON, XBOX_GAMEPAD_LEFT_THUMB,  0, "Left stick click" },
    { "Ctrl",            { XBOX_VK_CONTROL },XBOX_KEY_BUTTON, XBOX_GAMEPAD_RIGHT_THUMB, 0, "Right stick click" },
    { "Z",               { 'Z' },            XBOX_KEY_ANALOG, XBOX_BUTTON_A,            0, "A" },
    { "X",               { 'X' },            XBOX_KEY_ANALOG, XBOX_BUTTON_B,            0, "B" },
    { "C",               { 'C' },            XBOX_KEY_ANALOG, XBOX_BUTTON_X,            0, "X" },
    { "V",               { 'V' },            XBOX_KEY_ANALOG, XBOX_BUTTON_Y,            0, "Y" },
    { "Q",               { 'Q' },            XBOX_KEY_ANALOG, XBOX_BUTTON_WHITE,        0, "White" },
    { "E",               { 'E' },            XBOX_KEY_ANALOG, XBOX_BUTTON_BLACK,        0, "Black" },
    { "1",               { '1' },            XBOX_KEY_ANALOG, XBOX_BUTTON_LTRIGGER,     0, "Left trigger" },
    { "3",               { '3' },            XBOX_KEY_ANALOG, XBOX_BUTTON_RTRIGGER,     0, "Right trigger" },
    { "W or keypad 8",   { 'W', XBOX_VK_NUMPAD8 }, XBOX_KEY_AXIS, XBOX_KEY_LY, +1, "Left stick up" },
    { "S or keypad 2",   { 'S', XBOX_VK_NUMPAD2 }, XBOX_KEY_AXIS, XBOX_KEY_LY, -1, "Left stick down" },
    { "A or keypad 4",   { 'A', XBOX_VK_NUMPAD4 }, XBOX_KEY_AXIS, XBOX_KEY_LX, -1, "Left stick left" },
    { "D or keypad 6",   { 'D', XBOX_VK_NUMPAD6 }, XBOX_KEY_AXIS, XBOX_KEY_LX, +1, "Left stick right" },
    { "I",               { 'I' },            XBOX_KEY_AXIS,   XBOX_KEY_RY,             +1, "Right stick up" },
    { "K",               { 'K' },            XBOX_KEY_AXIS,   XBOX_KEY_RY,             -1, "Right stick down" },
    { "J",               { 'J' },            XBOX_KEY_AXIS,   XBOX_KEY_RX,             -1, "Right stick left" },
    { "L",               { 'L' },            XBOX_KEY_AXIS,   XBOX_KEY_RX,             +1, "Right stick right" },
};
const size_t xbox_key_map_count = sizeof xbox_key_map / sizeof xbox_key_map[0];

/* ---- the key trace ------------------------------------------------------ */

void xbox_InputTraceEdge(const char *fmt, ...)
{
    static unsigned n;
    char buf[96];
    va_list ap;

    if (!recomp_env(RENV_KEY_TRACE))
        return;
    n++;
    if (n > 200 && n % 50)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[KEY] %s\n", buf);
    fflush(stderr);
}

/* ---- host key edges ------------------------------------------------------ */

void xbox_InputKeyEvent(int code, int down)
{
    const char *name;
    int was;

    if (code <= 0 || code > 255)
        return;
    down = down != 0;
    was = s_key_down[code];
    switch (code) {
    case XBOX_VK_RETURN:   s_return_main = (unsigned char)down; break;
    case XBOX_VK_KP_ENTER: s_return_kp = (unsigned char)down;   break;
    }
    s_key_down[code] = (unsigned char)down;
    /* The either-side names follow the sides, so the compiled-in map's
     * Shift and Ctrl rows and a binding of `Shift` hear both keys. */
    switch (code) {
    case XBOX_VK_LSHIFT: case XBOX_VK_RSHIFT:
        s_key_down[XBOX_VK_SHIFT] = s_key_down[XBOX_VK_LSHIFT] | s_key_down[XBOX_VK_RSHIFT];
        break;
    case XBOX_VK_LCONTROL: case XBOX_VK_RCONTROL:
        s_key_down[XBOX_VK_CONTROL] = s_key_down[XBOX_VK_LCONTROL] | s_key_down[XBOX_VK_RCONTROL];
        break;
    case XBOX_VK_LMENU: case XBOX_VK_RMENU:
        s_key_down[XBOX_VK_MENU] = s_key_down[XBOX_VK_LMENU] | s_key_down[XBOX_VK_RMENU];
        break;
    case XBOX_VK_RETURN: case XBOX_VK_KP_ENTER:
        s_key_down[XBOX_VK_RETURN] = s_return_main | s_return_kp;
        break;
    }
    if (was == down)
        return;                          /* auto-repeat */
    xbox_InputMouseKey(code, down);
    name = xbox_InputCodeName(code, 0);
    if (name)
        xbox_InputTraceEdge("%s %s", down ? "down" : "up", name);
    else
        xbox_InputTraceEdge("%s vk=0x%02X", down ? "down" : "up", (unsigned)code);
}

void xbox_InputKeyEventWin32(int vk, unsigned long lparam, int down)
{
    unsigned scancode = (unsigned)(lparam >> 16) & 0xFF;
    int extended = (int)(lparam >> 24) & 1;

    /* WM_KEY* folds both sides into one VK; the scancode (Shift) and the
     * extended bit (Ctrl, Alt, keypad Enter) tell them apart. */
    switch (vk) {
    case XBOX_VK_SHIFT:   vk = scancode == 0x36 ? XBOX_VK_RSHIFT : XBOX_VK_LSHIFT; break;
    case XBOX_VK_CONTROL: vk = extended ? XBOX_VK_RCONTROL : XBOX_VK_LCONTROL;     break;
    case XBOX_VK_MENU:    vk = extended ? XBOX_VK_RMENU : XBOX_VK_LMENU;           break;
    case XBOX_VK_RETURN:  vk = extended ? XBOX_VK_KP_ENTER : XBOX_VK_RETURN;       break;
    }
    /* With both Shifts held, Windows sends WM_KEYUP for the last one released
     * only, so the first side would stay down (and LShift drives a trigger in
     * the game's bindings). Any Shift up releases both, as GLFW does. */
    if (!down && (vk == XBOX_VK_LSHIFT || vk == XBOX_VK_RSHIFT)) {
        xbox_InputKeyEvent(XBOX_VK_LSHIFT, 0);
        xbox_InputKeyEvent(XBOX_VK_RSHIFT, 0);
        return;
    }
    xbox_InputKeyEvent(vk, down);
}

/* ---- the binding tables -------------------------------------------------- */

typedef struct {
    unsigned char n;
    unsigned char code[XBOX_BIND_MAX];
    unsigned char mouse[XBOX_BIND_MAX];
} bind_set;

/* What each control drives. */
static const struct { uint8_t kind; uint16_t target; int8_t sign; } k_ctl[XBOX_CTL_COUNT] = {
    [XBOX_CTL_A]           = { XBOX_KEY_ANALOG, XBOX_BUTTON_A, 0 },
    [XBOX_CTL_B]           = { XBOX_KEY_ANALOG, XBOX_BUTTON_B, 0 },
    [XBOX_CTL_X]           = { XBOX_KEY_ANALOG, XBOX_BUTTON_X, 0 },
    [XBOX_CTL_Y]           = { XBOX_KEY_ANALOG, XBOX_BUTTON_Y, 0 },
    [XBOX_CTL_WHITE]       = { XBOX_KEY_ANALOG, XBOX_BUTTON_WHITE, 0 },
    [XBOX_CTL_BLACK]       = { XBOX_KEY_ANALOG, XBOX_BUTTON_BLACK, 0 },
    [XBOX_CTL_LTRIGGER]    = { XBOX_KEY_ANALOG, XBOX_BUTTON_LTRIGGER, 0 },
    [XBOX_CTL_RTRIGGER]    = { XBOX_KEY_ANALOG, XBOX_BUTTON_RTRIGGER, 0 },
    [XBOX_CTL_START]       = { XBOX_KEY_BUTTON, XBOX_GAMEPAD_START, 0 },
    [XBOX_CTL_BACK]        = { XBOX_KEY_BUTTON, XBOX_GAMEPAD_BACK, 0 },
    [XBOX_CTL_LTHUMB]      = { XBOX_KEY_BUTTON, XBOX_GAMEPAD_LEFT_THUMB, 0 },
    [XBOX_CTL_RTHUMB]      = { XBOX_KEY_BUTTON, XBOX_GAMEPAD_RIGHT_THUMB, 0 },
    [XBOX_CTL_DPAD_UP]     = { XBOX_KEY_BUTTON, XBOX_GAMEPAD_DPAD_UP, 0 },
    [XBOX_CTL_DPAD_DOWN]   = { XBOX_KEY_BUTTON, XBOX_GAMEPAD_DPAD_DOWN, 0 },
    [XBOX_CTL_DPAD_LEFT]   = { XBOX_KEY_BUTTON, XBOX_GAMEPAD_DPAD_LEFT, 0 },
    [XBOX_CTL_DPAD_RIGHT]  = { XBOX_KEY_BUTTON, XBOX_GAMEPAD_DPAD_RIGHT, 0 },
    [XBOX_CTL_LSTICK_UP]   = { XBOX_KEY_AXIS, XBOX_KEY_LY, +1 },
    [XBOX_CTL_LSTICK_DOWN] = { XBOX_KEY_AXIS, XBOX_KEY_LY, -1 },
    [XBOX_CTL_LSTICK_LEFT] = { XBOX_KEY_AXIS, XBOX_KEY_LX, -1 },
    [XBOX_CTL_LSTICK_RIGHT]= { XBOX_KEY_AXIS, XBOX_KEY_LX, +1 },
    [XBOX_CTL_RSTICK_UP]   = { XBOX_KEY_AXIS, XBOX_KEY_RY, +1 },
    [XBOX_CTL_RSTICK_DOWN] = { XBOX_KEY_AXIS, XBOX_KEY_RY, -1 },
    [XBOX_CTL_RSTICK_LEFT] = { XBOX_KEY_AXIS, XBOX_KEY_RX, -1 },
    [XBOX_CTL_RSTICK_RIGHT]= { XBOX_KEY_AXIS, XBOX_KEY_RX, +1 },
};

/* The control a compiled-in row drives. */
static int row_control(const xbox_key_row *r)
{
    for (int c = 0; c < XBOX_CTL_COUNT; c++)
        if (k_ctl[c].kind == r->kind && k_ctl[c].target == r->target
            && (r->kind != XBOX_KEY_AXIS || k_ctl[c].sign == r->sign))
            return c;
    return -1;
}

static void bind_add(bind_set *t, int control, int code, int is_mouse)
{
    bind_set *b = &t[control];
    if (b->n < XBOX_BIND_MAX) {
        b->code[b->n] = (unsigned char)code;
        b->mouse[b->n] = (unsigned char)(is_mouse != 0);
        b->n++;
    }
}

static void compiled_bindings(bind_set *t)
{
    memset(t, 0, sizeof(bind_set) * XBOX_CTL_COUNT);
    for (size_t i = 0; i < xbox_key_map_count; i++) {
        int c = row_control(&xbox_key_map[i]);
        if (c < 0)
            continue;
        bind_add(t, c, xbox_key_map[i].vk[0], 0);
        if (xbox_key_map[i].vk[1])
            bind_add(t, c, xbox_key_map[i].vk[1], 0);
    }
}

/* Written before the first poll (the config layer, a title), read by the
 * poll. */
static bind_set s_bind[XBOX_CTL_COUNT];
static int      s_bind_ready;

static void bindings_ready(void)
{
    if (!s_bind_ready) {
        compiled_bindings(s_bind);
        s_bind_ready = 1;
    }
}

void xbox_InputBindingsReset(void)
{
    compiled_bindings(s_bind);
    s_bind_ready = 1;
}

void xbox_InputBindingsClear(int control)
{
    if (control < 0 || control >= XBOX_CTL_COUNT)
        return;
    bindings_ready();
    s_bind[control].n = 0;
}

int xbox_InputBind(int control, int code, int is_mouse)
{
    if (control < 0 || control >= XBOX_CTL_COUNT || code <= 0
        || code > (is_mouse ? XBOX_MOUSE_INPUTS : 255))
        return -1;
    bindings_ready();
    if (s_bind[control].n >= XBOX_BIND_MAX)
        return -1;
    bind_add(s_bind, control, code, is_mouse);
    return 0;
}

int xbox_InputBindingsOf(int control, int *codes, int *is_mouse, int max)
{
    if (control < 0 || control >= XBOX_CTL_COUNT)
        return 0;
    bindings_ready();
    for (int i = 0; i < s_bind[control].n && i < max; i++) {
        if (codes) codes[i] = s_bind[control].code[i];
        if (is_mouse) is_mouse[i] = s_bind[control].mouse[i];
    }
    return s_bind[control].n;
}

/* ---- options, clock, enable ----------------------------------------------- */

static int      s_ramp_ms;
static int      s_walk_code;
static double   s_walk_scale = 1.0;
static uint64_t (*s_clock)(void);
static int      s_kb_enable = -1;

void xbox_InputKeyboardOpts(int stick_ramp_ms, int walk_code, double walk_scale)
{
    s_ramp_ms = stick_ramp_ms > 0 ? stick_ramp_ms : 0;
    s_walk_code = walk_code > 0 && walk_code < 256 ? walk_code : 0;
    s_walk_scale = walk_scale > 0.0 && walk_scale <= 1.0 ? walk_scale : 1.0;
}

void xbox_InputKbmSetClock(uint64_t (*now_ms)(void))
{
    s_clock = now_ms;
}

void xbox_InputKeyboardEnable(int on)
{
    __atomic_store_n(&s_kb_enable, on < 0 ? -1 : on != 0, __ATOMIC_RELEASE);
}

int xbox_InputKeyboardOn(void)
{
    int e = __atomic_load_n(&s_kb_enable, __ATOMIC_ACQUIRE);
    return e >= 0 ? e : recomp_env_on(RENV_KEYBOARD);
}

/* ---- the composer -------------------------------------------------------- */

typedef struct {
    int    ramp_ms;          /* 0: each key full deflection, diagonals too */
    int    walk_code;
    double walk_scale;
} compose_opts;

static int input_held(const bind_set *b, int i, int keys, int mouse)
{
    if (b->mouse[i])
        return mouse && xbox_InputMouseHeld(b->code[i]);
    return keys && s_key_down[b->code[i]];
}

static void compose(XBOX_INPUT_STATE *out, const bind_set *t,
                    const compose_opts *o, int keys, int mouse)
{
    static DWORD packet;
    /* Poll thread only: per thumb axis, the way its keys last pointed and
     * since when, for the ramp. */
    static int      s_dir[4];
    static uint64_t s_since[4];
    int have_clock = s_clock != NULL;
    uint64_t now = have_clock ? s_clock() : 0;
    int pos[4] = { 0, 0, 0, 0 }, neg[4] = { 0, 0, 0, 0 };
    double v[4];

    memset(out, 0, sizeof *out);
    if (mouse)
        xbox_InputMouseTick(now, have_clock);
    for (int c = 0; c < XBOX_CTL_COUNT; c++) {
        const bind_set *b = &t[c];
        int held = 0;
        for (int i = 0; i < b->n && !held; i++)
            held = input_held(b, i, keys, mouse);
        if (!held)
            continue;
        switch (k_ctl[c].kind) {
        case XBOX_KEY_BUTTON:
            out->Gamepad.wButtons |= k_ctl[c].target;
            break;
        case XBOX_KEY_ANALOG:
            /* Analog on the console, so a key is 255 rather than a flag --
             * a title that reads these as a pressure never sees a press if
             * they are 1. */
            out->Gamepad.bAnalogButtons[k_ctl[c].target] = 255;
            break;
        case XBOX_KEY_AXIS:
            if (k_ctl[c].sign > 0) pos[k_ctl[c].target] = 1; else neg[k_ctl[c].target] = 1;
            break;
        }
    }
    for (int a = 0; a < 4; a++) {
        /* Opposite keys together cancel. */
        int dir = pos[a] - neg[a];
        double mag = dir ? 1.0 : 0.0;
        if (o->ramp_ms > 0 && have_clock) {
            /* The floor sits just above the title's 0.4 deadzone, so the
             * first poll after a press already moves; then linear to full. */
            if (dir != s_dir[a]) {
                s_dir[a] = dir;
                s_since[a] = now;
            }
            if (dir) {
                mag = XBOX_KBM_STICK_FLOOR + (1.0 - XBOX_KBM_STICK_FLOOR)
                      * (double)(now - s_since[a]) / o->ramp_ms;
                if (mag > 1.0)
                    mag = 1.0;
            }
        }
        if (dir && keys && o->walk_code && s_key_down[o->walk_code]
            && mag > o->walk_scale)
            mag = o->walk_scale;
        v[a] = dir * mag;
    }
    if (o->ramp_ms > 0) {
        /* Two perpendicular keys give one key's magnitude, as a stick
         * pushed into a corner does. With the ramp off the keys stay what
         * the compiled-in map always gave: full on each axis. */
        for (int s = 0; s < 4; s += 2)
            if (v[s] != 0.0 && v[s + 1] != 0.0) {
                v[s] *= 0.70710678118654752440;   /* 1/sqrt 2 */
                v[s + 1] *= 0.70710678118654752440;   /* 1/sqrt 2 */
            }
    }
    out->Gamepad.sThumbLX = (SHORT)lround(v[XBOX_KEY_LX] * 32767.0);
    out->Gamepad.sThumbLY = (SHORT)lround(v[XBOX_KEY_LY] * 32767.0);
    out->Gamepad.sThumbRX = (SHORT)lround(v[XBOX_KEY_RX] * 32767.0);
    out->Gamepad.sThumbRY = (SHORT)lround(v[XBOX_KEY_RY] * 32767.0);

    if (mouse) {
        /* The axis the mouse moved replaces the keys' for this poll. */
        int mx, my, look = xbox_InputMouseLook(&mx, &my);
        if (look == XBOX_MOUSE_LOOK_RIGHT) {
            if (mx) out->Gamepad.sThumbRX = (SHORT)mx;
            if (my) out->Gamepad.sThumbRY = (SHORT)my;
        } else if (look == XBOX_MOUSE_LOOK_LEFT) {
            if (mx) out->Gamepad.sThumbLX = (SHORT)mx;
            if (my) out->Gamepad.sThumbLY = (SHORT)my;
        }
    }

    /* The title's input layer looks for button edges, so the packet number
     * has to move whenever the state does or a press is never noticed. */
    out->dwPacketNumber = ++packet;
}

void xbox_InputKbmState(XBOX_INPUT_STATE *out, int keys, int mouse)
{
    compose_opts o = { s_ramp_ms, s_walk_code, s_walk_scale };
    bindings_ready();
    compose(out, s_bind, &o, keys, mouse);
}

int xbox_InputKbmPoll(XBOX_INPUT_STATE *out)
{
    int keys, mouse;
    uint64_t now = s_clock ? s_clock() : 0;

    xbox_InputKbmScriptPoll(now, s_clock != NULL);
    keys = xbox_InputKeyboardOn();
    mouse = xbox_InputMouseOn();
    if (!keys && !mouse)
        return 0;
    xbox_InputKbmState(out, keys, mouse);
    return 1;
}

/* ---- the legacy calls: the compiled-in map, keys only -------------------- */

void xbox_InputKeyboardState(XBOX_INPUT_STATE *out)
{
    static bind_set compiled[XBOX_CTL_COUNT];
    static int ready;
    static const compose_opts o = { 0, 0, 1.0 };

    if (!ready) {
        compiled_bindings(compiled);
        ready = 1;
    }
    compose(out, compiled, &o, 1, 0);
}

int xbox_InputKeyboardPoll(XBOX_INPUT_STATE *out)
{
    if (!xbox_InputKeyboardOn())
        return 0;
    xbox_InputKeyboardState(out);
    return 1;
}

static const char *const k_readme_head[] = {
    "| Key | Guest control |",
    "|---|---|",
};

size_t xbox_InputKeyMapReadmeRow(size_t i, char *buf, size_t size)
{
    const size_t nh = sizeof k_readme_head / sizeof k_readme_head[0];
    int n;

    if (!buf || !size)
        return 0;
    if (i < nh)
        n = snprintf(buf, size, "%s", k_readme_head[i]);
    else if (i - nh < xbox_key_map_count)
        n = snprintf(buf, size, "| %s | %s |",
                     xbox_key_map[i - nh].keys, xbox_key_map[i - nh].guest);
    else
        return 0;
    return n > 0 ? (size_t)n : 0;
}
