/*
 * The binding vocabulary. See input_names.h.
 *
 * Keys are named by the label the host layout gives them, as the key table
 * is indexed: Win32 reports a letter's VK and SDL its keycode by layout, so
 * the key labelled Z is `Z` on AZERTY too. Punctuation follows the US
 * labels (`Semicolon` is VK_OEM_1), which is the best a name can do for a
 * key whose label moves between layouts; RECOMP_TRACE=key prints the name
 * a pressed key has, which is how a player finds it.
 */
#include "input_names.h"

#include <stddef.h>

typedef struct {
    const char *name;
    int         code;
    unsigned char is_mouse, alias;
} name_row;

/* Canonical names first, aliases at the end, so xbox_InputCodeName finds
 * the canonical one. */
static const name_row k_names[] = {
    { "A",           0x41, 0, 0 },
    { "B",           0x42, 0, 0 },
    { "C",           0x43, 0, 0 },
    { "D",           0x44, 0, 0 },
    { "E",           0x45, 0, 0 },
    { "F",           0x46, 0, 0 },
    { "G",           0x47, 0, 0 },
    { "H",           0x48, 0, 0 },
    { "I",           0x49, 0, 0 },
    { "J",           0x4A, 0, 0 },
    { "K",           0x4B, 0, 0 },
    { "L",           0x4C, 0, 0 },
    { "M",           0x4D, 0, 0 },
    { "N",           0x4E, 0, 0 },
    { "O",           0x4F, 0, 0 },
    { "P",           0x50, 0, 0 },
    { "Q",           0x51, 0, 0 },
    { "R",           0x52, 0, 0 },
    { "S",           0x53, 0, 0 },
    { "T",           0x54, 0, 0 },
    { "U",           0x55, 0, 0 },
    { "V",           0x56, 0, 0 },
    { "W",           0x57, 0, 0 },
    { "X",           0x58, 0, 0 },
    { "Y",           0x59, 0, 0 },
    { "Z",           0x5A, 0, 0 },
    { "0",           0x30, 0, 0 },
    { "1",           0x31, 0, 0 },
    { "2",           0x32, 0, 0 },
    { "3",           0x33, 0, 0 },
    { "4",           0x34, 0, 0 },
    { "5",           0x35, 0, 0 },
    { "6",           0x36, 0, 0 },
    { "7",           0x37, 0, 0 },
    { "8",           0x38, 0, 0 },
    { "9",           0x39, 0, 0 },
    { "F1",          0x70, 0, 0 },
    { "F2",          0x71, 0, 0 },
    { "F3",          0x72, 0, 0 },
    { "F4",          0x73, 0, 0 },
    { "F5",          0x74, 0, 0 },
    { "F6",          0x75, 0, 0 },
    { "F7",          0x76, 0, 0 },
    { "F8",          0x77, 0, 0 },
    { "F9",          0x78, 0, 0 },
    { "F10",         0x79, 0, 0 },
    { "F11",         0x7A, 0, 0 },
    { "F12",         0x7B, 0, 0 },
    { "Space",       0x20, 0, 0 },
    { "Return",      0x0D, 0, 0 },
    { "Escape",      0x1B, 0, 0 },
    { "Backspace",   0x08, 0, 0 },
    { "Tab",         0x09, 0, 0 },
    { "LShift",      0xA0, 0, 0 },
    { "RShift",      0xA1, 0, 0 },
    { "Shift",       0x10, 0, 0 },
    { "LCtrl",       0xA2, 0, 0 },
    { "RCtrl",       0xA3, 0, 0 },
    { "Ctrl",        0x11, 0, 0 },
    { "LAlt",        0xA4, 0, 0 },
    { "RAlt",        0xA5, 0, 0 },
    { "Alt",         0x12, 0, 0 },
    { "Up",          0x26, 0, 0 },
    { "Down",        0x28, 0, 0 },
    { "Left",        0x25, 0, 0 },
    { "Right",       0x27, 0, 0 },
    { "Insert",      0x2D, 0, 0 },
    { "Delete",      0x2E, 0, 0 },
    { "Home",        0x24, 0, 0 },
    { "End",         0x23, 0, 0 },
    { "PageUp",      0x21, 0, 0 },
    { "PageDown",    0x22, 0, 0 },
    { "KP0",         0x60, 0, 0 },
    { "KP1",         0x61, 0, 0 },
    { "KP2",         0x62, 0, 0 },
    { "KP3",         0x63, 0, 0 },
    { "KP4",         0x64, 0, 0 },
    { "KP5",         0x65, 0, 0 },
    { "KP6",         0x66, 0, 0 },
    { "KP7",         0x67, 0, 0 },
    { "KP8",         0x68, 0, 0 },
    { "KP9",         0x69, 0, 0 },
    { "KPEnter",     0x0E, 0, 0 },
    { "KPPlus",      0x6B, 0, 0 },
    { "KPMinus",     0x6D, 0, 0 },
    { "KPMultiply",  0x6A, 0, 0 },
    { "KPDivide",    0x6F, 0, 0 },
    { "KPPeriod",    0x6E, 0, 0 },
    { "Comma",       0xBC, 0, 0 },
    { "Period",      0xBE, 0, 0 },
    { "Slash",       0xBF, 0, 0 },
    { "Semicolon",   0xBA, 0, 0 },
    { "Quote",       0xDE, 0, 0 },
    { "LBracket",    0xDB, 0, 0 },
    { "RBracket",    0xDD, 0, 0 },
    { "Backslash",   0xDC, 0, 0 },
    { "Minus",       0xBD, 0, 0 },
    { "Equals",      0xBB, 0, 0 },
    { "Grave",       0xC0, 0, 0 },
    { "MouseLeft",   1, 1, 0 },
    { "MouseRight",  2, 1, 0 },
    { "MouseMiddle", 3, 1, 0 },
    { "Mouse4",      4, 1, 0 },
    { "Mouse5",      5, 1, 0 },
    { "WheelUp",     6, 1, 0 },
    { "WheelDown",   7, 1, 0 },
    { "Enter",       0x0D, 0, 1 },
    { "Esc",         0x1B, 0, 1 },
};

static const char *const k_controls[XBOX_CTL_COUNT] = {
    "a", "b", "x", "y", "white", "black", "ltrigger", "rtrigger",
    "start", "back", "lthumb", "rthumb",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "lstick_up", "lstick_down", "lstick_left", "lstick_right",
    "rstick_up", "rstick_down", "rstick_left", "rstick_right",
};

/* ASCII only: every name in both tables is. */
static int same_name(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb)
            return 0;
        if (!ca)
            return 1;
    }
}

int xbox_InputNameToCode(const char *name, int *code, int *is_mouse)
{
    if (!name)
        return 0;
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++)
        if (same_name(name, k_names[i].name)) {
            if (code) *code = k_names[i].code;
            if (is_mouse) *is_mouse = k_names[i].is_mouse;
            return 1;
        }
    return 0;
}

const char *xbox_InputCodeName(int code, int is_mouse)
{
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++)
        if (k_names[i].code == code && k_names[i].is_mouse == (is_mouse != 0)
            && !k_names[i].alias)
            return k_names[i].name;
    return NULL;
}

int xbox_InputControlFromName(const char *name)
{
    if (!name)
        return -1;
    for (int i = 0; i < XBOX_CTL_COUNT; i++)
        if (same_name(name, k_controls[i]))
            return i;
    return -1;
}

const char *xbox_InputControlName(int control)
{
    return control >= 0 && control < XBOX_CTL_COUNT ? k_controls[control] : NULL;
}

int xbox_InputNameAt(int i, const char **name, int *code, int *is_mouse,
                     int *alias)
{
    if (i < 0 || (size_t)i >= sizeof k_names / sizeof k_names[0])
        return 0;
    if (name) *name = k_names[i].name;
    if (code) *code = k_names[i].code;
    if (is_mouse) *is_mouse = k_names[i].is_mouse;
    if (alias) *alias = k_names[i].alias;
    return 1;
}
