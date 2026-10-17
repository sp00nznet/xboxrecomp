/* Host input to Xbox controller rules. See input_map.h. */

#include "input_map.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- control names ------------------------------------------------------- */

static const char *const k_control_names[INPUT_CONTROL_COUNT] = {
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "start", "back", "left_thumb", "right_thumb",
    "a", "b", "x", "y", "black", "white",
    "left_trigger", "right_trigger",
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right",
};

const char *xbox_InputMap_ControlName(int control)
{
    return control >= 0 && control < INPUT_CONTROL_COUNT ? k_control_names[control] : NULL;
}

static int ascii_lower(int c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
static int ascii_upper(int c) { return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c; }

static int equal_nocase(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b)) return 0;
    return *a == *b;
}

int xbox_InputMap_ControlFromName(const char *name)
{
    int i;
    if (!name) return -1;
    for (i = 0; i < INPUT_CONTROL_COUNT; i++)
        if (equal_nocase(name, k_control_names[i])) return i;
    return -1;
}

/* ---- merging ------------------------------------------------------------- */

static int64_t stick_mag2(int x, int y) { return (int64_t)x * x + (int64_t)y * y; }

void xbox_InputMap_PadMerge(XBOX_GAMEPAD *dst, const XBOX_GAMEPAD *src)
{
    int i;
    dst->wButtons |= src->wButtons;
    for (i = 0; i < 8; i++)
        if (src->bAnalogButtons[i] > dst->bAnalogButtons[i])
            dst->bAnalogButtons[i] = src->bAnalogButtons[i];
    if (stick_mag2(src->sThumbLX, src->sThumbLY) > stick_mag2(dst->sThumbLX, dst->sThumbLY)) {
        dst->sThumbLX = src->sThumbLX; dst->sThumbLY = src->sThumbLY;
    }
    if (stick_mag2(src->sThumbRX, src->sThumbRY) > stick_mag2(dst->sThumbRX, dst->sThumbRY)) {
        dst->sThumbRX = src->sThumbRX; dst->sThumbRY = src->sThumbRY;
    }
}

/* ---- keys ---------------------------------------------------------------- */

typedef struct KeyName { const char *name; int vk; } KeyName;

/* The first name listed for a code is the one written back. Letters, digits,
 * function keys and the numeric pad's digits are generated rather than listed.
 * The mouse wheel has no virtual-key code, so it has no name here. */
static const KeyName k_keys[] = {
    { "mouse1", XBOX_VK_LBUTTON }, { "mouse2", XBOX_VK_RBUTTON }, { "mouse3", XBOX_VK_MBUTTON },
    { "mouse4", XBOX_VK_XBUTTON1 }, { "mouse5", XBOX_VK_XBUTTON2 },
    { "backspace", XBOX_VK_BACK }, { "tab", XBOX_VK_TAB }, { "enter", XBOX_VK_RETURN }, { "return", XBOX_VK_RETURN },
    { "shift", XBOX_VK_SHIFT }, { "ctrl", XBOX_VK_CONTROL }, { "control", XBOX_VK_CONTROL }, { "alt", XBOX_VK_MENU },
    { "pause", XBOX_VK_PAUSE }, { "capslock", XBOX_VK_CAPITAL },
    { "escape", XBOX_VK_ESCAPE }, { "esc", XBOX_VK_ESCAPE }, { "space", XBOX_VK_SPACE },
    { "pageup", XBOX_VK_PRIOR }, { "pagedown", XBOX_VK_NEXT }, { "end", XBOX_VK_END }, { "home", XBOX_VK_HOME },
    { "left", XBOX_VK_LEFT }, { "up", XBOX_VK_UP }, { "right", XBOX_VK_RIGHT }, { "down", XBOX_VK_DOWN },
    { "insert", XBOX_VK_INSERT }, { "delete", XBOX_VK_DELETE },
    { "numpadmultiply", XBOX_VK_MULTIPLY }, { "numpadadd", XBOX_VK_ADD }, { "numpadsubtract", XBOX_VK_SUBTRACT },
    { "numpaddecimal", XBOX_VK_DECIMAL }, { "numpaddivide", XBOX_VK_DIVIDE },
    { "lshift", XBOX_VK_LSHIFT }, { "rshift", XBOX_VK_RSHIFT }, { "lctrl", XBOX_VK_LCONTROL }, { "rctrl", XBOX_VK_RCONTROL },
    { "lalt", XBOX_VK_LMENU }, { "ralt", XBOX_VK_RMENU },
    { "semicolon", XBOX_VK_OEM_1 }, { "equals", XBOX_VK_OEM_PLUS }, { "comma", XBOX_VK_OEM_COMMA }, { "minus", XBOX_VK_OEM_MINUS },
    { "period", XBOX_VK_OEM_PERIOD }, { "slash", XBOX_VK_OEM_2 }, { "grave", XBOX_VK_OEM_3 },
    { "leftbracket", XBOX_VK_OEM_4 }, { "backslash", XBOX_VK_OEM_5 }, { "rightbracket", XBOX_VK_OEM_6 },
    { "quote", XBOX_VK_OEM_7 },
};
#define KEY_TABLE_COUNT ((int)(sizeof(k_keys) / sizeof(k_keys[0])))

/* "f5" or "numpad3": a fixed prefix and a number with no leading zero. */
static int numbered(const char *name, const char *prefix, int lo, int hi)
{
    size_t n = strlen(prefix);
    char tail[8];
    int v;
    if (strlen(name) <= n || strlen(name) > n + 2) return -1;
    {
        size_t i;
        for (i = 0; i < n; i++)
            if (ascii_lower((unsigned char)name[i]) != prefix[i]) return -1;
    }
    v = atoi(name + n);
    snprintf(tail, sizeof(tail), "%d", v);
    if (strcmp(tail, name + n) || v < lo || v > hi) return -1;
    return v;
}

int xbox_InputMap_KeyFromName(const char *name)
{
    int i, v;
    if (!name || !*name) return -1;
    if (name[1] == 0) {
        /* Win32 letters and digits are their uppercase ASCII codes. */
        char c = (char)ascii_upper((unsigned char)name[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
        return -1;
    }
    v = numbered(name, "f", 1, 12);
    if (v >= 0) return XBOX_VK_F1 + v - 1;
    v = numbered(name, "numpad", 0, 9);
    if (v >= 0) return XBOX_VK_NUMPAD0 + v;
    for (i = 0; i < KEY_TABLE_COUNT; i++)
        if (equal_nocase(name, k_keys[i].name)) return k_keys[i].vk;
    return -1;
}

const char *xbox_InputMap_KeyName(int vk)
{
    static const char *const letters[] = {
        "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"
    };
    static const char *const digits[] = {
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"
    };
    static const char *const functions[] = {
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"
    };
    static const char *const numpad[] = {
        "Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4", "Numpad5", "Numpad6", "Numpad7", "Numpad8", "Numpad9"
    };
    int i;
    if (vk >= 'A' && vk <= 'Z') return letters[vk - 'A'];
    if (vk >= '0' && vk <= '9') return digits[vk - '0'];
    if (vk >= XBOX_VK_F1 && vk <= XBOX_VK_F12) return functions[vk - XBOX_VK_F1];
    if (vk >= XBOX_VK_NUMPAD0 && vk <= XBOX_VK_NUMPAD9) return numpad[vk - XBOX_VK_NUMPAD0];
    for (i = 0; i < KEY_TABLE_COUNT; i++)
        if (k_keys[i].vk == vk) return k_keys[i].name;
    return NULL;
}

int xbox_InputMap_BindingsParse(InputBindings *b, int control, const char *text)
{
    int rejected = 0;
    if (!b || control < 0 || control >= INPUT_CONTROL_COUNT) return -1;
    b->count[control] = 0;
    if (!text) return 0;
    for (;;) {
        const char *end = strchr(text, ',');
        const char *first = text, *last = end ? end : text + strlen(text);
        char token[32];
        size_t len;
        int vk, j, dup = 0;
        while (first < last && isspace((unsigned char)*first)) first++;
        while (last > first && isspace((unsigned char)last[-1])) last--;
        len = (size_t)(last - first);
        if (len) {
            if (len >= sizeof(token)) { rejected++; }
            else {
                memcpy(token, first, len); token[len] = 0;
                if (!equal_nocase(token, "none")) {
                    vk = xbox_InputMap_KeyFromName(token);
                    if (vk < 0) rejected++;
                    else {
                        for (j = 0; j < b->count[control]; j++)
                            if (b->key[control][j] == vk) dup = 1;
                        if (!dup) {
                            if (b->count[control] < INPUT_BINDINGS_PER_CONTROL)
                                b->key[control][b->count[control]++] = (uint8_t)vk;
                            else rejected++;
                        }
                    }
                }
            }
        }
        if (!end) break;
        text = end + 1;
    }
    return rejected;
}

const char *xbox_InputMap_BindingsFormat(const InputBindings *b, int control, char *buf, int size)
{
    int i, used = 0;
    if (!buf || size <= 0) return buf;
    buf[0] = 0;
    if (!b || control < 0 || control >= INPUT_CONTROL_COUNT || !b->count[control]) {
        snprintf(buf, (size_t)size, "none");
        return buf;
    }
    for (i = 0; i < b->count[control] && i < INPUT_BINDINGS_PER_CONTROL; i++) {
        const char *n = xbox_InputMap_KeyName(b->key[control][i]);
        int w = snprintf(buf + used, (size_t)(size - used), "%s%s", i ? ", " : "",
                         n ? n : "?");
        if (w < 0 || w >= size - used) break;
        used += w;
    }
    return buf;
}

static int control_held(const InputBindings *b, const uint8_t *down, int control)
{
    int i;
    for (i = 0; i < b->count[control] && i < INPUT_BINDINGS_PER_CONTROL; i++) {
        int vk = b->key[control][i];
        if (vk && down[vk]) return 1;
    }
    return 0;
}

/* A stick from four keys. Opposite keys cancel. Two directions at once are
 * full deflection on both axes, or with INPUT_DIAGONAL_ROUND a unit-circle
 * deflection, so a diagonal is as fast as a straight line (32767 * 0.7071 on
 * each axis). */
static void keys_stick(int diagonal, int right, int left_k, int up, int down_k,
                       SHORT *ox, SHORT *oy)
{
    int x = (right ? 1 : 0) - (left_k ? 1 : 0);
    int y = (up ? 1 : 0) - (down_k ? 1 : 0);
    if (x && y && diagonal == INPUT_DIAGONAL_ROUND) {
        *ox = (SHORT)(x * 23169);
        *oy = (SHORT)(y * 23169);
    } else {
        *ox = (SHORT)(x * 32767);
        *oy = (SHORT)(y * 32767);
    }
}

void xbox_InputMap_KeysResolve(const InputBindings *b, const uint8_t *down, XBOX_GAMEPAD *out)
{
    int i;

    memset(out, 0, sizeof(*out));
    /* The digital controls are the wButtons bits in order. */
    for (i = 0; i < 8; i++)
        if (control_held(b, down, INPUT_DPAD_UP + i)) out->wButtons |= (WORD)(1u << i);
    /* Analog on the console: a key is 255, not a flag, or a title that reads
     * the value as a pressure never sees a press. INPUT_A + i is
     * bAnalogButtons[i]. */
    for (i = 0; i < 8; i++)
        if (control_held(b, down, INPUT_A + i)) out->bAnalogButtons[XBOX_BUTTON_A + i] = 255;
    keys_stick(b->diagonal,
               control_held(b, down, INPUT_LSTICK_RIGHT), control_held(b, down, INPUT_LSTICK_LEFT),
               control_held(b, down, INPUT_LSTICK_UP), control_held(b, down, INPUT_LSTICK_DOWN),
               &out->sThumbLX, &out->sThumbLY);
    keys_stick(b->diagonal,
               control_held(b, down, INPUT_RSTICK_RIGHT), control_held(b, down, INPUT_RSTICK_LEFT),
               control_held(b, down, INPUT_RSTICK_UP), control_held(b, down, INPUT_RSTICK_DOWN),
               &out->sThumbRX, &out->sThumbRY);
}

/* ---- pad sources ---------------------------------------------------------- */

static const char *const k_source_names[INPUT_SRC_COUNT] = {
    "none",
    "south", "east", "west", "north",
    "back", "guide", "start",
    "left_stick_click", "right_stick_click",
    "left_shoulder", "right_shoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "misc1", "touchpad",
    "paddle1", "paddle2", "paddle3", "paddle4",
    "left_trigger", "right_trigger",
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right",
};

const char *xbox_InputMap_SourceName(int source)
{
    return source >= 0 && source < INPUT_SRC_COUNT ? k_source_names[source] : NULL;
}

int xbox_InputMap_SourceFromName(const char *name)
{
    int i;
    if (!name) return -1;
    for (i = 0; i < INPUT_SRC_COUNT; i++)
        if (equal_nocase(name, k_source_names[i])) return i;
    return -1;
}

void xbox_InputMap_PadMapDefaults(InputPadMap *m)
{
    static const uint8_t identity[INPUT_PAD_CONTROL_COUNT] = {
        INPUT_SRC_DPAD_UP, INPUT_SRC_DPAD_DOWN, INPUT_SRC_DPAD_LEFT, INPUT_SRC_DPAD_RIGHT,
        INPUT_SRC_START, INPUT_SRC_BACK,
        INPUT_SRC_LEFT_STICK_CLICK, INPUT_SRC_RIGHT_STICK_CLICK,
        INPUT_SRC_SOUTH, INPUT_SRC_EAST, INPUT_SRC_WEST, INPUT_SRC_NORTH,
        INPUT_SRC_LEFT_SHOULDER, INPUT_SRC_RIGHT_SHOULDER,
        INPUT_SRC_LEFT_TRIGGER, INPUT_SRC_RIGHT_TRIGGER,
    };
    memcpy(m->source, identity, sizeof(identity));
    m->left_stick = INPUT_STICK_LEFT;
    m->right_stick = INPUT_STICK_RIGHT;
    m->deadzone_left = 15;
    m->deadzone_right = 15;
    m->trigger_threshold = 5;
    m->button_threshold = 50;
}

/* ---- deadzones ------------------------------------------------------------ */

void xbox_InputMap_ApplyStick(int x, int y, int deadzone_pct, SHORT *ox, SHORT *oy)
{
    double dz, mag, scale;
    if (deadzone_pct < 0) deadzone_pct = 0;
    if (deadzone_pct > 95) deadzone_pct = 95;
    dz = 32767.0 * deadzone_pct / 100.0;
    mag = sqrt((double)x * x + (double)y * y);
    if (mag <= dz || mag == 0.0) {
        *ox = 0; *oy = 0;
        return;
    }
    scale = (mag - dz) / (32767.0 - dz);
    if (scale > 1.0) scale = 1.0;
    {
        double fx = x / mag * scale * 32767.0, fy = y / mag * scale * 32767.0;
        *ox = (SHORT)(fx >= 0 ? fx + 0.5 : fx - 0.5);
        *oy = (SHORT)(fy >= 0 ? fy + 0.5 : fy - 0.5);
    }
}

uint8_t xbox_InputMap_ApplyTrigger(int value, int threshold_pct)
{
    int thr;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    if (threshold_pct < 0) threshold_pct = 0;
    if (threshold_pct > 95) threshold_pct = 95;
    thr = 255 * threshold_pct / 100;
    if (value <= thr) return 0;
    return (uint8_t)((value - thr) * 255 / (255 - thr));
}

/* ---- pad mapping ---------------------------------------------------------- */

static int raw_value(const InputRaw *r, int source)
{
    return source > INPUT_SRC_NONE && source < INPUT_SRC_COUNT ? (r->src[source] > 0 ? r->src[source] : 0) : 0;
}

void xbox_InputMap_PadMap(const InputPadMap *m, const InputRaw *raw, XBOX_GAMEPAD *out)
{
    int pct = m->button_threshold > 100 ? 100 : m->button_threshold;
    int press_at = 32767 * pct / 100;
    int i;

    memset(out, 0, sizeof(*out));
    if (press_at < 1) press_at = 1;
    for (i = 0; i < 8; i++)
        if (raw_value(raw, m->source[INPUT_DPAD_UP + i]) >= press_at) out->wButtons |= (WORD)(1u << i);
    /* The face buttons and shoulders are analog on the console but binary
     * here: pressed is 255. The triggers keep the pad's own travel. */
    for (i = 0; i < 6; i++)
        if (raw_value(raw, m->source[INPUT_A + i]) >= press_at)
            out->bAnalogButtons[XBOX_BUTTON_A + i] = 255;
    for (i = 6; i < 8; i++) {
        int v = raw_value(raw, m->source[INPUT_A + i]) >> 7;
        out->bAnalogButtons[XBOX_BUTTON_A + i] =
            xbox_InputMap_ApplyTrigger(v > 255 ? 255 : v, m->trigger_threshold);
    }
    {
        int sx = 0, sy = 0;
        switch (m->left_stick) {
        case INPUT_STICK_LEFT:  sx = raw->lx; sy = raw->ly; break;
        case INPUT_STICK_RIGHT: sx = raw->rx; sy = raw->ry; break;
        default: break;
        }
        xbox_InputMap_ApplyStick(sx, sy, m->deadzone_left, &out->sThumbLX, &out->sThumbLY);
        sx = sy = 0;
        switch (m->right_stick) {
        case INPUT_STICK_LEFT:  sx = raw->lx; sy = raw->ly; break;
        case INPUT_STICK_RIGHT: sx = raw->rx; sy = raw->ry; break;
        default: break;
        }
        xbox_InputMap_ApplyStick(sx, sy, m->deadzone_right, &out->sThumbRX, &out->sThumbRY);
    }
}

/* The layout the loops above rely on. */
#define INPUT_MAP_ASSERT(name, cond) typedef char input_map_assert_##name[(cond) ? 1 : -1]
INPUT_MAP_ASSERT(dpad_up_bit0, XBOX_GAMEPAD_DPAD_UP == 1u << (INPUT_DPAD_UP - INPUT_DPAD_UP));
INPUT_MAP_ASSERT(right_thumb_bit7, XBOX_GAMEPAD_RIGHT_THUMB == 1u << (INPUT_RIGHT_THUMB - INPUT_DPAD_UP));
INPUT_MAP_ASSERT(start_bit4, XBOX_GAMEPAD_START == 1u << (INPUT_START - INPUT_DPAD_UP));
INPUT_MAP_ASSERT(black_slot, XBOX_BUTTON_BLACK == XBOX_BUTTON_A + (INPUT_BLACK - INPUT_A));
INPUT_MAP_ASSERT(white_slot, XBOX_BUTTON_WHITE == XBOX_BUTTON_A + (INPUT_WHITE - INPUT_A));
INPUT_MAP_ASSERT(rtrigger_slot, XBOX_BUTTON_RTRIGGER == XBOX_BUTTON_A + (INPUT_RIGHT_TRIGGER - INPUT_A));
