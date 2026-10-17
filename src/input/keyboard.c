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
 * pad connected there (xinput_device.c), so a real controller keeps working.
 *
 * Keys come from the game's window, which only receives them while it has
 * the focus, so typing in another window does not drive the game. Losing
 * the focus releases every key, or a key held across Alt-Tab or Cmd-Tab
 * would stay held for ever.
 */
#include "keyboard.h"
#include "input_map.h"
#include <stdlib.h>
#include <string.h>

/* Reading this needs no lock. Each entry is written only by the window's
 * thread and read only by the game's poll, one byte at a time, and a press
 * seen a poll late is indistinguishable from one made a poll later. */
static volatile unsigned char s_key_down[256];

void xbox_InputKeySet(int vk, int down)
{
    if ((unsigned)vk < 256)
        s_key_down[vk] = down ? 1 : 0;
}

void xbox_InputKeysClear(void)
{
    memset((void *)s_key_down, 0, sizeof s_key_down);
}

int xbox_InputWin32SideVk(int vk, uint32_t lparam)
{
    unsigned scan = (lparam >> 16) & 0xFF;
    int extended = (lparam >> 24) & 1;

    switch (vk) {
    case XBOX_VK_SHIFT:
        /* Right Shift is its own scan code, not an extended key. */
        return scan == 0x36 ? XBOX_VK_RSHIFT : XBOX_VK_LSHIFT;
    case XBOX_VK_CONTROL:
        return extended ? XBOX_VK_RCONTROL : XBOX_VK_LCONTROL;
    case XBOX_VK_MENU:
        return extended ? XBOX_VK_RMENU : XBOX_VK_LMENU;
    }
    return 0;
}

int xbox_InputWin32MouseVk(unsigned msg, uint32_t wparam, int *down)
{
    /* winuser.h's message numbers; no window header here. */
    switch (msg) {
    case 0x0201: *down = 1; return XBOX_VK_LBUTTON;   /* WM_LBUTTONDOWN */
    case 0x0202: *down = 0; return XBOX_VK_LBUTTON;   /* WM_LBUTTONUP */
    case 0x0204: *down = 1; return XBOX_VK_RBUTTON;   /* WM_RBUTTONDOWN */
    case 0x0205: *down = 0; return XBOX_VK_RBUTTON;   /* WM_RBUTTONUP */
    case 0x0207: *down = 1; return XBOX_VK_MBUTTON;   /* WM_MBUTTONDOWN */
    case 0x0208: *down = 0; return XBOX_VK_MBUTTON;   /* WM_MBUTTONUP */
    case 0x020B:                                      /* WM_XBUTTONDOWN */
    case 0x020C:                                      /* WM_XBUTTONUP */
        *down = msg == 0x020B;
        switch (wparam >> 16) {                       /* GET_XBUTTON_WPARAM */
        case 1: return XBOX_VK_XBUTTON1;
        case 2: return XBOX_VK_XBUTTON2;
        }
        return 0;
    }
    return 0;
}

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk > 255)
        return 0;
    return s_key_down[vk] != 0;
}

/* The one map, on every host. Z X C V for A B X Y is the usual emulator
 * row. The left thumb is W/S/A/D, because Mac laptops have no numeric
 * keypad and a title may walk on the stick, not the d-pad; keypad 8/2/4/6
 * drive the left stick too. Arrows stay the d-pad: as a stick
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

/* The control a row of the map drives. The digital controls are the
 * wButtons bits in order and the analog ones the bAnalogButtons[] slots
 * (input_map.h), so the row's target says which. */
static int row_control(const xbox_key_row *r)
{
    static const uint8_t axis_control[4][2] = {      /* [axis][sign > 0] */
        [XBOX_KEY_LX] = { INPUT_LSTICK_LEFT, INPUT_LSTICK_RIGHT },
        [XBOX_KEY_LY] = { INPUT_LSTICK_DOWN, INPUT_LSTICK_UP },
        [XBOX_KEY_RX] = { INPUT_RSTICK_LEFT, INPUT_RSTICK_RIGHT },
        [XBOX_KEY_RY] = { INPUT_RSTICK_DOWN, INPUT_RSTICK_UP },
    };
    int bit;

    switch (r->kind) {
    case XBOX_KEY_BUTTON:
        for (bit = 0; bit < 8; bit++)
            if (r->target == 1u << bit)
                return INPUT_DPAD_UP + bit;
        break;
    case XBOX_KEY_ANALOG:
        if (r->target < 8)
            return INPUT_A + r->target;
        break;
    case XBOX_KEY_AXIS:
        if (r->target < 4)
            return axis_control[r->target][r->sign > 0];
        break;
    }
    return -1;
}

void xbox_InputKeyboardBindings(InputBindings *b)
{
    memset(b, 0, sizeof *b);
    /* Full deflection on a diagonal, as this map has always given. */
    b->diagonal = INPUT_DIAGONAL_FULL;
    for (size_t i = 0; i < xbox_key_map_count; i++) {
        const xbox_key_row *r = &xbox_key_map[i];
        int c = row_control(r);
        for (int k = 0; k < 2; k++)
            if (c >= 0 && r->vk[k] && b->count[c] < INPUT_BINDINGS_PER_CONTROL)
                b->key[c][b->count[c]++] = r->vk[k];
    }
}

void xbox_InputKeyboardState(XBOX_INPUT_STATE *out)
{
    static DWORD packet;
    InputBindings b;
    uint8_t down[INPUT_KEY_COUNT];

    /* One snapshot of the table, so a poll sees one set of keys. */
    for (int vk = 0; vk < INPUT_KEY_COUNT; vk++)
        down[vk] = s_key_down[vk];
    xbox_InputKeyboardBindings(&b);

    memset(out, 0, sizeof *out);
    /* Buttons set their bit, analog keys read 255 (a title that reads
     * these as a pressure never sees a press if they are 1), stick keys
     * are full deflection and opposite keys cancel. */
    xbox_InputMap_KeysResolve(&b, down, &out->Gamepad);

    /* The title's input layer looks for button edges, so the packet number
     * has to move whenever the state does or a press is never noticed. */
    out->dwPacketNumber = ++packet;
}

int xbox_InputKeyboardPoll(XBOX_INPUT_STATE *out)
{
    const char *v = getenv("RECOMP_KEYBOARD");
    if (!v || !*v || *v == '0')
        return 0;
    xbox_InputKeyboardState(out);
    return 1;
}
