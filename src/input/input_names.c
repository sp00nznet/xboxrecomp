/*
 * The binding vocabulary. See input_names.h.
 *
 * Keys are named by the label the host layout gives them, as the key table
 * is indexed: Win32 reports a letter's VK and SDL its keycode by layout, so
 * the key labelled Z is `Z` on AZERTY too. Punctuation follows the US
 * labels (`semicolon` is VK_OEM_1), which is the best a name can do for a
 * key whose label moves between layouts; RECOMP_TRACE=key prints the name
 * a pressed key has, which is how a player finds it.
 */
#include "input_names.h"

#include <stddef.h>

/* Upstream gives the mouse buttons their Win32 VKs; the fork keeps the
 * mouse apart from the key table (mouse.c), so these become its 1..5. */
static const int k_mouse_vk[XBOX_MOUSE_5 + 1] = {
    0, XBOX_VK_LBUTTON, XBOX_VK_RBUTTON, XBOX_VK_MBUTTON,
    XBOX_VK_XBUTTON1, XBOX_VK_XBUTTON2,
};

/* What upstream has no virtual-key code for, in its spelling. */
static const struct { const char *name; int code, is_mouse; } k_extra[] = {
    { "numpadenter", XBOX_VK_KP_ENTER,      0 },
    { "wheelup",     XBOX_MOUSE_WHEEL_UP,   1 },
    { "wheeldown",   XBOX_MOUSE_WHEEL_DOWN, 1 },
};
#define EXTRA_COUNT (sizeof k_extra / sizeof k_extra[0])

/* The pad sources a host action names that no guest control has, in
 * XBOX_HOST_* bit order. */
static const int k_host_src[XBOX_HOST_BUTTONS] = {
    INPUT_SRC_GUIDE, INPUT_SRC_MISC1, INPUT_SRC_PADDLE1, INPUT_SRC_PADDLE2,
    INPUT_SRC_PADDLE3, INPUT_SRC_PADDLE4, INPUT_SRC_TOUCHPAD,
};

/* ASCII only: every name is. */
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

static int mouse_of_vk(int vk)
{
    for (int m = XBOX_MOUSE_LEFT; m <= XBOX_MOUSE_5; m++)
        if (k_mouse_vk[m] == vk)
            return m;
    return 0;
}

int xbox_InputNameToCode(const char *name, int *code, int *is_mouse)
{
    int vk, m;
    if (!name)
        return 0;
    for (size_t i = 0; i < EXTRA_COUNT; i++)
        if (same_name(name, k_extra[i].name)) {
            if (code) *code = k_extra[i].code;
            if (is_mouse) *is_mouse = k_extra[i].is_mouse;
            return 1;
        }
    vk = xbox_InputMap_KeyFromName(name);
    if (vk <= 0)
        return 0;
    m = mouse_of_vk(vk);
    if (code) *code = m ? m : vk;
    if (is_mouse) *is_mouse = m != 0;
    return 1;
}

const char *xbox_InputCodeName(int code, int is_mouse)
{
    for (size_t i = 0; i < EXTRA_COUNT; i++)
        if (k_extra[i].code == code && k_extra[i].is_mouse == (is_mouse != 0))
            return k_extra[i].name;
    if (is_mouse)
        return code >= XBOX_MOUSE_LEFT && code <= XBOX_MOUSE_5
            ? xbox_InputMap_KeyName(k_mouse_vk[code]) : NULL;
    /* A mouse button's VK is no key in the fork's key space. */
    if (code <= 0 || code > 0xFF || mouse_of_vk(code))
        return NULL;
    return xbox_InputMap_KeyName(code);
}

int xbox_InputNameAt(int i, const char **name, int *code, int *is_mouse)
{
    int n = 0;
    if (i < 0)
        return 0;
    for (int m = 0; m < 2; m++)
        for (int c = 1; c <= (m ? XBOX_MOUSE_INPUTS : 0xFF); c++) {
            const char *s = xbox_InputCodeName(c, m);
            if (!s || n++ != i)
                continue;
            if (name) *name = s;
            if (code) *code = c;
            if (is_mouse) *is_mouse = m;
            return 1;
        }
    return 0;
}

int xbox_InputHostButtonFromName(const char *name)
{
    int src = xbox_InputMap_SourceFromName(name);
    for (int i = 0; src >= 0 && i < XBOX_HOST_BUTTONS; i++)
        if (k_host_src[i] == src)
            return i;
    return -1;
}

const char *xbox_InputHostButtonName(int index)
{
    return index >= 0 && index < XBOX_HOST_BUTTONS
        ? xbox_InputMap_SourceName(k_host_src[index]) : NULL;
}
