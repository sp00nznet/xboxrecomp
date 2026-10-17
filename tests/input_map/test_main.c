#define _CRT_SECURE_NO_WARNINGS
/* input_map: key names, binding parse and format, key resolution, deadzone
 * arithmetic and pad remapping. */

#include "input_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;
static int g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed++; } } while (0)
#define NEAR(a, b, tol) (abs((int)(a) - (int)(b)) <= (tol))

/* Every name the first version of this module (d9a0a38) wrote or read, with
 * the Windows virtual-key code it means, from winuser.h, written out here
 * rather than taken from the module. Strings saved by that version must keep
 * reading as the same keys. The wheel had no VK and is gone: those two names
 * are now rejected. */
static const struct { const char *name; int vk; } k_names[] = {
    { "mouse1", 0x01 }, { "mouse2", 0x02 }, { "mouse3", 0x04 }, { "mouse4", 0x05 }, { "mouse5", 0x06 },
    { "wheelup", -1 }, { "wheeldown", -1 },
    { "backspace", 0x08 }, { "tab", 0x09 }, { "enter", 0x0D }, { "shift", 0x10 }, { "ctrl", 0x11 },
    { "alt", 0x12 }, { "pause", 0x13 }, { "capslock", 0x14 }, { "escape", 0x1B }, { "space", 0x20 },
    { "pageup", 0x21 }, { "pagedown", 0x22 }, { "end", 0x23 }, { "home", 0x24 },
    { "left", 0x25 }, { "up", 0x26 }, { "right", 0x27 }, { "down", 0x28 },
    { "insert", 0x2D }, { "delete", 0x2E },
    { "numpadmultiply", 0x6A }, { "numpadadd", 0x6B }, { "numpadsubtract", 0x6D },
    { "numpaddecimal", 0x6E }, { "numpaddivide", 0x6F },
    { "lshift", 0xA0 }, { "rshift", 0xA1 }, { "lctrl", 0xA2 }, { "rctrl", 0xA3 },
    { "lalt", 0xA4 }, { "ralt", 0xA5 },
    { "semicolon", 0xBA }, { "equals", 0xBB }, { "comma", 0xBC }, { "minus", 0xBD },
    { "period", 0xBE }, { "slash", 0xBF }, { "grave", 0xC0 },
    { "leftbracket", 0xDB }, { "backslash", 0xDC }, { "rightbracket", 0xDD }, { "quote", 0xDE },
    { "A", 'A' }, { "B", 'B' }, { "C", 'C' }, { "D", 'D' }, { "E", 'E' }, { "F", 'F' }, { "G", 'G' },
    { "H", 'H' }, { "I", 'I' }, { "J", 'J' }, { "K", 'K' }, { "L", 'L' }, { "M", 'M' }, { "N", 'N' },
    { "O", 'O' }, { "P", 'P' }, { "Q", 'Q' }, { "R", 'R' }, { "S", 'S' }, { "T", 'T' }, { "U", 'U' },
    { "V", 'V' }, { "W", 'W' }, { "X", 'X' }, { "Y", 'Y' }, { "Z", 'Z' },
    { "0", '0' }, { "1", '1' }, { "2", '2' }, { "3", '3' }, { "4", '4' },
    { "5", '5' }, { "6", '6' }, { "7", '7' }, { "8", '8' }, { "9", '9' },
    { "F1", 0x70 }, { "F2", 0x71 }, { "F3", 0x72 }, { "F4", 0x73 }, { "F5", 0x74 }, { "F6", 0x75 },
    { "F7", 0x76 }, { "F8", 0x77 }, { "F9", 0x78 }, { "F10", 0x79 }, { "F11", 0x7A }, { "F12", 0x7B },
    { "Numpad0", 0x60 }, { "Numpad1", 0x61 }, { "Numpad2", 0x62 }, { "Numpad3", 0x63 }, { "Numpad4", 0x64 },
    { "Numpad5", 0x65 }, { "Numpad6", 0x66 }, { "Numpad7", 0x67 }, { "Numpad8", 0x68 }, { "Numpad9", 0x69 },
    /* aliases */
    { "return", 0x0D }, { "control", 0x11 }, { "esc", 0x1B },
};
#define N_NAMES ((int)(sizeof(k_names) / sizeof(k_names[0])))
#define N_ALIASES 3

static void test_key_names(void)
{
    int c, i, named = 0;
    char buf[64];
    CHECK(xbox_InputMap_KeyFromName("a") == 'A');
    CHECK(xbox_InputMap_KeyFromName("Z") == 'Z');
    CHECK(xbox_InputMap_KeyFromName("7") == '7');
    CHECK(xbox_InputMap_KeyFromName("F5") == XBOX_VK_F1 + 4);
    CHECK(xbox_InputMap_KeyFromName("f12") == XBOX_VK_F12);
    CHECK(xbox_InputMap_KeyFromName("f13") == -1);
    CHECK(xbox_InputMap_KeyFromName("f05") == -1);
    CHECK(xbox_InputMap_KeyFromName("f0") == -1);
    CHECK(xbox_InputMap_KeyFromName("Numpad4") == XBOX_VK_NUMPAD4);
    CHECK(xbox_InputMap_KeyFromName("numpad10") == -1);
    CHECK(xbox_InputMap_KeyFromName("SPACE") == XBOX_VK_SPACE);
    CHECK(xbox_InputMap_KeyFromName("Esc") == XBOX_VK_ESCAPE);
    CHECK(xbox_InputMap_KeyFromName("mouse1") == XBOX_VK_LBUTTON);
    CHECK(xbox_InputMap_KeyFromName("Mouse3") == XBOX_VK_MBUTTON);
    CHECK(xbox_InputMap_KeyFromName("WheelUp") == -1);
    CHECK(xbox_InputMap_KeyFromName("lshift") == XBOX_VK_LSHIFT);
    CHECK(xbox_InputMap_KeyFromName("") == -1);
    CHECK(xbox_InputMap_KeyFromName("banana") == -1);
    CHECK(xbox_InputMap_KeyFromName("ab") == -1);
    CHECK(xbox_InputMap_KeyFromName("!") == -1);
    /* The key map's own keys (keyboard.c) by name. */
    CHECK(xbox_InputMap_KeyFromName("enter") == XBOX_VK_RETURN);
    CHECK(xbox_InputMap_KeyFromName("backspace") == XBOX_VK_BACK);
    CHECK(xbox_InputMap_KeyFromName("shift") == XBOX_VK_SHIFT);
    CHECK(xbox_InputMap_KeyFromName("ctrl") == XBOX_VK_CONTROL);
    CHECK(xbox_InputMap_KeyFromName("up") == XBOX_VK_UP);
    CHECK(xbox_InputMap_KeyFromName("numpad8") == XBOX_VK_NUMPAD8);

    /* Every name of the earlier version, as the same key, case-insensitive. */
    for (i = 0; i < N_NAMES; i++) {
        int vk = xbox_InputMap_KeyFromName(k_names[i].name);
        CHECK(vk == k_names[i].vk);
        if (vk != k_names[i].vk)
            printf("  %s -> %d, want %d\n", k_names[i].name, vk, k_names[i].vk);
        for (c = 0; k_names[i].name[c] && c < (int)sizeof(buf) - 1; c++)
            buf[c] = (char)(k_names[i].name[c] ^ ((k_names[i].name[c] >= 'a' && k_names[i].name[c] <= 'z')
                                                || (k_names[i].name[c] >= 'A' && k_names[i].name[c] <= 'Z') ? 0x20 : 0));
        buf[c] = 0;
        CHECK(xbox_InputMap_KeyFromName(buf) == k_names[i].vk);
        /* A canonical name is what KeyName writes back, case and all. */
        if (i < N_NAMES - N_ALIASES && k_names[i].vk > 0)
            CHECK(xbox_InputMap_KeyName(k_names[i].vk)
                  && !strcmp(xbox_InputMap_KeyName(k_names[i].vk), k_names[i].name));
    }

    /* Every code with a name reads back as itself, so one code has one
     * canonical name, and the named codes are exactly those above. */
    for (c = 0; c < INPUT_KEY_COUNT; c++) {
        const char *n = xbox_InputMap_KeyName(c);
        int listed = 0;
        for (i = 0; i < N_NAMES; i++) listed |= k_names[i].vk == c;
        CHECK(!n == !listed);
        if (!n) continue;
        named++;
        snprintf(buf, sizeof(buf), "%s", n);
        CHECK(xbox_InputMap_KeyFromName(buf) == c);
    }
    CHECK(named == N_NAMES - N_ALIASES - 2);       /* less the aliases and the wheel */

    /* Side-specific modifiers are their own keys, apart from the aggregate. */
    CHECK(xbox_InputMap_KeyFromName("lshift") != xbox_InputMap_KeyFromName("rshift"));
    CHECK(xbox_InputMap_KeyFromName("lshift") != xbox_InputMap_KeyFromName("shift"));
    CHECK(xbox_InputMap_KeyFromName("lctrl") != xbox_InputMap_KeyFromName("ctrl"));
    CHECK(xbox_InputMap_KeyFromName("ralt") != xbox_InputMap_KeyFromName("alt"));

    /* Control names. */
    for (c = 0; c < INPUT_CONTROL_COUNT; c++) {
        CHECK(xbox_InputMap_ControlFromName(xbox_InputMap_ControlName(c)) == c);
    }
    CHECK(xbox_InputMap_ControlFromName("Left_Stick_Up") == INPUT_LSTICK_UP);
    CHECK(xbox_InputMap_ControlFromName("jump") == -1);
    CHECK(xbox_InputMap_ControlFromName(NULL) == -1);
    CHECK(xbox_InputMap_ControlName(INPUT_CONTROL_COUNT) == NULL);
}

static void test_bindings(void)
{
    InputBindings b;
    char text[128];
    int k;
    memset(&b, 0, sizeof(b));

    CHECK(xbox_InputMap_BindingsParse(&b, INPUT_A, "L, Space,  Mouse1 ") == 0);
    CHECK(b.count[INPUT_A] == 3);
    CHECK(b.key[INPUT_A][0] == 'L' && b.key[INPUT_A][1] == XBOX_VK_SPACE && b.key[INPUT_A][2] == XBOX_VK_LBUTTON);
    CHECK(!strcmp(xbox_InputMap_BindingsFormat(&b, INPUT_A, text, sizeof(text)), "L, space, mouse1"));

    /* Reading the formatted text back gives the same bindings. */
    k = xbox_InputMap_BindingsParse(&b, INPUT_B, text);
    CHECK(k == 0 && b.count[INPUT_B] == 3 && b.key[INPUT_B][1] == XBOX_VK_SPACE);

    /* Parsing replaces; none and empty clear; duplicates are dropped. */
    CHECK(xbox_InputMap_BindingsParse(&b, INPUT_A, "none") == 0 && b.count[INPUT_A] == 0);
    CHECK(!strcmp(xbox_InputMap_BindingsFormat(&b, INPUT_A, text, sizeof(text)), "none"));
    CHECK(xbox_InputMap_BindingsParse(&b, INPUT_A, "J, j, J") == 0 && b.count[INPUT_A] == 1);
    CHECK(xbox_InputMap_BindingsParse(&b, INPUT_A, "") == 0 && b.count[INPUT_A] == 0);
    /* An alias is the same key as its name. */
    CHECK(xbox_InputMap_BindingsParse(&b, INPUT_A, "Enter, return") == 0 && b.count[INPUT_A] == 1);

    /* Unknown names are skipped and counted, known ones kept. The wheel is
     * not a key here. */
    CHECK(xbox_InputMap_BindingsParse(&b, INPUT_X, "K, banana, wheeldown, 99, slash") == 3);
    CHECK(b.count[INPUT_X] == 2 && b.key[INPUT_X][0] == 'K' && b.key[INPUT_X][1] == XBOX_VK_OEM_2);

    /* Four is the most; the fifth is counted as rejected. */
    CHECK(xbox_InputMap_BindingsParse(&b, INPUT_Y, "A,B,C,D,E") == 1);
    CHECK(b.count[INPUT_Y] == 4);

    /* Control names are the setting keys. */
    CHECK(!strcmp(xbox_InputMap_ControlName(INPUT_LSTICK_UP), "left_stick_up"));
}

/* Binding strings as d9a0a38 wrote them still parse to the same keys. */
static void test_saved_strings(void)
{
    static const struct { const char *text; int rejected; int n; int vk[4]; } saved[] = {
        { "L, space, mouse1",              0, 3, { 'L', 0x20, 0x01 } },
        { "lshift",                        0, 1, { 0xA0 } },
        { "rctrl, ralt, lalt, lctrl",      0, 4, { 0xA3, 0xA5, 0xA4, 0xA2 } },
        { "Numpad4, F5, numpadadd",        0, 3, { 0x64, 0x74, 0x6B } },
        { "esc, tab, capslock",            0, 3, { 0x1B, 0x09, 0x14 } },
        { "semicolon, quote, grave",       0, 3, { 0xBA, 0xDE, 0xC0 } },
        { "mouse4, mouse5",                0, 2, { 0x05, 0x06 } },
        { "W, wheelup",                    1, 1, { 'W' } },
        { "wheeldown",                     1, 0, { 0 } },
        { "none",                          0, 0, { 0 } },
    };
    size_t i;
    int j;
    for (i = 0; i < sizeof(saved) / sizeof(saved[0]); i++) {
        InputBindings b;
        memset(&b, 0, sizeof(b));
        CHECK(xbox_InputMap_BindingsParse(&b, INPUT_RSTICK_LEFT, saved[i].text) == saved[i].rejected);
        CHECK(b.count[INPUT_RSTICK_LEFT] == saved[i].n);
        for (j = 0; j < saved[i].n; j++)
            CHECK(b.key[INPUT_RSTICK_LEFT][j] == saved[i].vk[j]);
    }
}

static void test_resolve(void)
{
    InputBindings b;
    uint8_t down[INPUT_KEY_COUNT];
    XBOX_GAMEPAD p;
    memset(&b, 0, sizeof(b));
    memset(down, 0, sizeof(down));
    xbox_InputMap_BindingsParse(&b, INPUT_LSTICK_UP, "W");
    xbox_InputMap_BindingsParse(&b, INPUT_LSTICK_DOWN, "S");
    xbox_InputMap_BindingsParse(&b, INPUT_LSTICK_LEFT, "A");
    xbox_InputMap_BindingsParse(&b, INPUT_LSTICK_RIGHT, "D");
    xbox_InputMap_BindingsParse(&b, INPUT_RSTICK_LEFT, "Left");
    xbox_InputMap_BindingsParse(&b, INPUT_A, "L, Space");
    xbox_InputMap_BindingsParse(&b, INPUT_LEFT_TRIGGER, "Q");
    xbox_InputMap_BindingsParse(&b, INPUT_START, "Enter");
    xbox_InputMap_BindingsParse(&b, INPUT_DPAD_UP, "mouse2");
    xbox_InputMap_BindingsParse(&b, INPUT_BLACK, "rshift");

    xbox_InputMap_KeysResolve(&b, down, &p);
    CHECK(p.wButtons == 0 && p.sThumbLX == 0 && p.sThumbLY == 0 && p.sThumbRX == 0);

    down['W'] = 1;
    xbox_InputMap_KeysResolve(&b, down, &p);
    CHECK(p.sThumbLY == 32767 && p.sThumbLX == 0);

    down['D'] = 1;                                   /* diagonal: full on both, by default */
    xbox_InputMap_KeysResolve(&b, down, &p);
    CHECK(p.sThumbLX == 32767 && p.sThumbLY == 32767);
    b.diagonal = INPUT_DIAGONAL_ROUND;               /* or not faster than straight */
    xbox_InputMap_KeysResolve(&b, down, &p);
    CHECK(p.sThumbLX == 23169 && p.sThumbLY == 23169);

    down['S'] = 1;                                   /* opposite keys cancel */
    xbox_InputMap_KeysResolve(&b, down, &p);
    CHECK(p.sThumbLY == 0 && p.sThumbLX == 32767);
    b.diagonal = INPUT_DIAGONAL_FULL;

    down[XBOX_VK_LEFT] = 1;
    xbox_InputMap_KeysResolve(&b, down, &p);
    CHECK(p.sThumbRX == -32767 && p.sThumbRY == 0);

    down[XBOX_VK_SPACE] = 1;                         /* either bound key presses A */
    down['Q'] = 1;
    down[XBOX_VK_RETURN] = 1;
    down[XBOX_VK_RBUTTON] = 1;
    xbox_InputMap_KeysResolve(&b, down, &p);
    CHECK(p.bAnalogButtons[XBOX_BUTTON_A] == 255);
    CHECK(p.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 255);
    CHECK(p.bAnalogButtons[XBOX_BUTTON_B] == 0);
    CHECK(p.wButtons == (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_UP));

    /* The aggregate Shift does not press a binding to one side. */
    CHECK(p.bAnalogButtons[XBOX_BUTTON_BLACK] == 0);
    down[XBOX_VK_SHIFT] = 1;
    xbox_InputMap_KeysResolve(&b, down, &p);
    CHECK(p.bAnalogButtons[XBOX_BUTTON_BLACK] == 0);
    down[XBOX_VK_RSHIFT] = 1;
    xbox_InputMap_KeysResolve(&b, down, &p);
    CHECK(p.bAnalogButtons[XBOX_BUTTON_BLACK] == 255);

    /* Every digital control is its own wButtons bit, every analog one its
     * own slot. */
    {
        int c;
        for (c = 0; c < INPUT_PAD_CONTROL_COUNT; c++) {
            InputBindings one;
            memset(&one, 0, sizeof(one));
            memset(down, 0, sizeof(down));
            xbox_InputMap_BindingsParse(&one, c, "F9");
            down[XBOX_VK_F1 + 8] = 1;
            xbox_InputMap_KeysResolve(&one, down, &p);
            if (c < INPUT_A) {
                CHECK(p.wButtons == 1u << c);
            } else {
                int s;
                CHECK(p.wButtons == 0);
                for (s = 0; s < 8; s++)
                    CHECK(p.bAnalogButtons[s] == (s == c - INPUT_A ? 255 : 0));
            }
        }
    }
}

static void test_merge(void)
{
    XBOX_GAMEPAD a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.wButtons = XBOX_GAMEPAD_START;
    a.bAnalogButtons[0] = 255;
    a.sThumbLX = 1000;
    b.wButtons = XBOX_GAMEPAD_BACK;
    b.bAnalogButtons[0] = 10;
    b.bAnalogButtons[7] = 90;
    b.sThumbLX = -20000;
    b.sThumbRY = 5;
    xbox_InputMap_PadMerge(&a, &b);
    CHECK(a.wButtons == (XBOX_GAMEPAD_START | XBOX_GAMEPAD_BACK));
    CHECK(a.bAnalogButtons[0] == 255 && a.bAnalogButtons[7] == 90);
    CHECK(a.sThumbLX == -20000);                       /* the larger deflection wins */
    CHECK(a.sThumbRY == 5);
}

static void test_deadzone(void)
{
    SHORT x, y;
    xbox_InputMap_ApplyStick(3000, 0, 15, &x, &y);          /* 15% is 4915 */
    CHECK(x == 0 && y == 0);
    xbox_InputMap_ApplyStick(3000, 3000, 15, &x, &y);       /* magnitude 4243: still inside */
    CHECK(x == 0 && y == 0);
    xbox_InputMap_ApplyStick(32767, 0, 15, &x, &y);         /* full stays full */
    CHECK(x == 32767 && y == 0);
    xbox_InputMap_ApplyStick(-32768, 0, 15, &x, &y);
    CHECK(x == -32767 && y == 0);
    xbox_InputMap_ApplyStick(16383, 0, 15, &x, &y);         /* (16383-4915)/(32767-4915) */
    CHECK(NEAR(x, 13491, 2) && y == 0);
    xbox_InputMap_ApplyStick(0, -16383, 15, &x, &y);
    CHECK(x == 0 && NEAR(y, -13491, 2));
    xbox_InputMap_ApplyStick(20000, 20000, 15, &x, &y);     /* direction preserved */
    CHECK(x == y && x > 0 && x < 32767);
    xbox_InputMap_ApplyStick(32767, 32767, 15, &x, &y);     /* beyond the circle clamps to it */
    CHECK(NEAR(x, 23170, 3) && NEAR(y, 23170, 3));
    xbox_InputMap_ApplyStick(500, 0, 0, &x, &y);            /* no deadzone passes through */
    CHECK(x == 500 && y == 0);
    xbox_InputMap_ApplyStick(0, 0, 15, &x, &y);
    CHECK(x == 0 && y == 0);
    xbox_InputMap_ApplyStick(32767, 0, 120, &x, &y);        /* absurd percentages are clamped */
    CHECK(x == 32767);

    CHECK(xbox_InputMap_ApplyTrigger(0, 5) == 0);
    CHECK(xbox_InputMap_ApplyTrigger(12, 5) == 0);          /* 5% of 255 is 12 */
    CHECK(xbox_InputMap_ApplyTrigger(255, 5) == 255);
    CHECK(NEAR(xbox_InputMap_ApplyTrigger(134, 5), 128, 1));
    CHECK(xbox_InputMap_ApplyTrigger(100, 0) == 100);
    CHECK(xbox_InputMap_ApplyTrigger(300, 5) == 255);
}

static void test_padmap(void)
{
    InputPadMap m;
    InputRaw r;
    XBOX_GAMEPAD p;
    int i;

    CHECK(xbox_InputMap_SourceFromName("south") == INPUT_SRC_SOUTH);
    CHECK(xbox_InputMap_SourceFromName("Left_Trigger") == INPUT_SRC_LEFT_TRIGGER);
    CHECK(xbox_InputMap_SourceFromName("a") == -1);
    for (i = 0; i < INPUT_SRC_COUNT; i++)
        CHECK(xbox_InputMap_SourceFromName(xbox_InputMap_SourceName(i)) == i);

    xbox_InputMap_PadMapDefaults(&m);
    memset(&r, 0, sizeof(r));
    xbox_InputMap_PadMap(&m, &r, &p);
    CHECK(p.wButtons == 0 && p.bAnalogButtons[0] == 0 && p.sThumbLX == 0);

    /* Identity: the bottom face button is A, the shoulders are Black and White. */
    r.src[INPUT_SRC_SOUTH] = 32767;
    r.src[INPUT_SRC_LEFT_SHOULDER] = 32767;
    r.src[INPUT_SRC_START] = 32767;
    r.src[INPUT_SRC_DPAD_LEFT] = 32767;
    r.src[INPUT_SRC_RIGHT_TRIGGER] = 32767;
    r.src[INPUT_SRC_LEFT_TRIGGER] = 128 << 7;
    xbox_InputMap_PadMap(&m, &r, &p);
    CHECK(p.bAnalogButtons[XBOX_BUTTON_A] == 255 && p.bAnalogButtons[XBOX_BUTTON_B] == 0);
    CHECK(p.bAnalogButtons[XBOX_BUTTON_BLACK] == 255 && p.bAnalogButtons[XBOX_BUTTON_WHITE] == 0);
    CHECK(p.wButtons == (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_LEFT));
    CHECK(p.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255);
    CHECK(NEAR(p.bAnalogButtons[XBOX_BUTTON_LTRIGGER], 128, 8));   /* analog, with the threshold applied */

    /* A button below the press threshold does not count; at it, does. */
    memset(&r, 0, sizeof(r));
    r.src[INPUT_SRC_EAST] = 16000;
    xbox_InputMap_PadMap(&m, &r, &p);
    CHECK(p.bAnalogButtons[XBOX_BUTTON_B] == 0);
    r.src[INPUT_SRC_EAST] = 16384;
    xbox_InputMap_PadMap(&m, &r, &p);
    CHECK(p.bAnalogButtons[XBOX_BUTTON_B] == 255);

    /* Remap: swap A and B, put Start on the touchpad, X on the left trigger. */
    m.source[INPUT_A] = INPUT_SRC_EAST;
    m.source[INPUT_B] = INPUT_SRC_SOUTH;
    m.source[INPUT_START] = INPUT_SRC_TOUCHPAD;
    m.source[INPUT_X] = INPUT_SRC_LEFT_TRIGGER;
    memset(&r, 0, sizeof(r));
    r.src[INPUT_SRC_SOUTH] = 32767;
    r.src[INPUT_SRC_TOUCHPAD] = 32767;
    r.src[INPUT_SRC_LEFT_TRIGGER] = 32767;
    xbox_InputMap_PadMap(&m, &r, &p);
    CHECK(p.bAnalogButtons[XBOX_BUTTON_A] == 0 && p.bAnalogButtons[XBOX_BUTTON_B] == 255);
    CHECK(p.wButtons == XBOX_GAMEPAD_START);
    CHECK(p.bAnalogButtons[XBOX_BUTTON_X] == 255);

    /* None leaves a control unbound. */
    xbox_InputMap_PadMapDefaults(&m);
    m.source[INPUT_A] = INPUT_SRC_NONE;
    memset(&r, 0, sizeof(r));
    r.src[INPUT_SRC_SOUTH] = 32767;
    xbox_InputMap_PadMap(&m, &r, &p);
    CHECK(p.bAnalogButtons[XBOX_BUTTON_A] == 0);

    /* Sticks: pass through with the deadzone, swap, or switch off. */
    xbox_InputMap_PadMapDefaults(&m);
    memset(&r, 0, sizeof(r));
    r.lx = 32767; r.ly = 0; r.rx = 0; r.ry = -32767;
    xbox_InputMap_PadMap(&m, &r, &p);
    CHECK(p.sThumbLX == 32767 && p.sThumbRY == -32767 && p.sThumbRX == 0);
    m.left_stick = INPUT_STICK_RIGHT;
    m.right_stick = INPUT_STICK_LEFT;
    xbox_InputMap_PadMap(&m, &r, &p);
    CHECK(p.sThumbLY == -32767 && p.sThumbRX == 32767 && p.sThumbLX == 0);
    m.right_stick = INPUT_STICK_NONE;
    xbox_InputMap_PadMap(&m, &r, &p);
    CHECK(p.sThumbRX == 0 && p.sThumbRY == 0);
    /* Drift inside the deadzone is ignored. */
    xbox_InputMap_PadMapDefaults(&m);
    r.lx = 2500; r.ly = -2500;
    xbox_InputMap_PadMap(&m, &r, &p);
    CHECK(p.sThumbLX == 0 && p.sThumbLY == 0);
}


static void test_api_edges(void)
{
    InputBindings b = {0};
    XBOX_GAMEPAD a = {0}, z = {0}, out;
    InputRaw raw = {0};
    InputPadMap map;
    uint8_t down[INPUT_KEY_COUNT] = {0};
    char long_text[600], small[2] = {'!', '!'}, text[80];
    const char *saved = xbox_InputMap_KeyName('A');
    int i;
    CHECK(xbox_InputMap_KeyFromName(NULL) == -1);
    CHECK(xbox_InputMap_KeyName(0) == NULL);
    CHECK(xbox_InputMap_KeyName(-1) == NULL);
    CHECK(xbox_InputMap_KeyName(INPUT_KEY_COUNT) == NULL);
    CHECK(xbox_InputMap_KeyName(0x03) == NULL);             /* VK_CANCEL: a code, no name */
    for (i = 1; i < INPUT_KEY_COUNT; i++) {
        const char *name = xbox_InputMap_KeyName(i);
        if (name) CHECK(xbox_InputMap_KeyFromName(name) == i);
    }
    CHECK(!strcmp(saved, "A")); /* names remain valid after other lookups */
    CHECK(xbox_InputMap_BindingsParse(NULL, INPUT_A, "A") == -1);
    CHECK(xbox_InputMap_BindingsParse(&b, -1, "A") == -1);
    CHECK(xbox_InputMap_BindingsParse(&b, INPUT_CONTROL_COUNT, "A") == -1);
    CHECK(xbox_InputMap_BindingsParse(&b, INPUT_A, NULL) == 0);
    memset(long_text, 'q', sizeof(long_text));
    long_text[sizeof(long_text)-3] = ',';
    long_text[sizeof(long_text)-2] = 'B';
    long_text[sizeof(long_text)-1] = 0;
    CHECK(xbox_InputMap_BindingsParse(&b, INPUT_A, long_text) == 1);
    CHECK(b.count[INPUT_A] == 1 && b.key[INPUT_A][0] == 'B');
    xbox_InputMap_BindingsFormat(&b, INPUT_A, small, 1);
    CHECK(small[0] == 0 && small[1] == '!');
    CHECK(xbox_InputMap_BindingsFormat(&b, INPUT_A, NULL, 0) == NULL);
    /* Caller-corrupted counts and unnamed codes never read outside bounded
     * storage. */
    b.count[INPUT_A] = 255;
    for (i = 0; i < INPUT_BINDINGS_PER_CONTROL; i++) b.key[INPUT_A][i] = UINT8_MAX;
    xbox_InputMap_BindingsFormat(&b, INPUT_A, text, sizeof(text));
    CHECK(!strcmp(text, "?, ?, ?, ?"));
    down[0] = 1;
    b.key[INPUT_A][0] = 0;
    b.key[INPUT_A][1] = 0;
    b.key[INPUT_A][2] = 0;
    b.key[INPUT_A][3] = 0;
    xbox_InputMap_KeysResolve(&b, down, &out);
    CHECK(out.bAnalogButtons[XBOX_BUTTON_A] == 0);           /* code 0 is never held */
    down[UINT8_MAX] = 1;
    b.key[INPUT_A][3] = UINT8_MAX;
    xbox_InputMap_KeysResolve(&b, down, &out);
    CHECK(out.bAnalogButtons[XBOX_BUTTON_A] == 255);
    b.diagonal = 200;                                        /* unknown: full deflection */
    b.count[INPUT_LSTICK_UP] = 1; b.key[INPUT_LSTICK_UP][0] = 'W';
    b.count[INPUT_LSTICK_LEFT] = 1; b.key[INPUT_LSTICK_LEFT][0] = 'A';
    down['W'] = down['A'] = 1;
    xbox_InputMap_KeysResolve(&b, down, &out);
    CHECK(out.sThumbLX == -32767 && out.sThumbLY == 32767);
    a.sThumbLX = INT16_MIN; a.sThumbLY = INT16_MIN;
    z.sThumbLX = INT16_MAX; z.sThumbLY = INT16_MAX;
    xbox_InputMap_PadMerge(&z, &a);
    CHECK(z.sThumbLX == INT16_MIN && z.sThumbLY == INT16_MIN);
    xbox_InputMap_PadMapDefaults(&map);
    map.source[INPUT_A] = 255;
    raw.src[INPUT_SRC_LEFT_TRIGGER] = -32768;
    xbox_InputMap_PadMap(&map, &raw, &out);
    CHECK(out.bAnalogButtons[XBOX_BUTTON_A] == 0 && out.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 0);
    map.button_threshold = 255;
    raw.src[INPUT_SRC_START] = 32767;
    xbox_InputMap_PadMap(&map, &raw, &out);
    CHECK(out.wButtons == XBOX_GAMEPAD_START);
    raw.src[INPUT_SRC_START] = 32766;
    xbox_InputMap_PadMap(&map, &raw, &out);
    CHECK(out.wButtons == 0);
    CHECK(xbox_InputMap_ApplyTrigger(-32768, -100) == 0);
    CHECK(xbox_InputMap_ApplyTrigger(32767, 255) == 255);
}

/* Independent range/symmetry and format-roundtrip properties. */
static void test_properties(void)
{
    int x, y, dz, i;
    for (dz = 0; dz <= 100; dz += 5)
        for (x = -32767; x <= 32767; x += 8191)
            for (y = -32767; y <= 32767; y += 8191) {
                SHORT a, b, na, nb;
                xbox_InputMap_ApplyStick(x, y, dz, &a, &b);
                xbox_InputMap_ApplyStick(-x, -y, dz, &na, &nb);
                CHECK(a == -na && b == -nb);
                CHECK((int64_t)a*a + (int64_t)b*b <= INT64_C(32769)*32769);
            }
    for (dz = -5; dz <= 100; dz += 5) {
        int last = 0;
        for (i = -1; i <= 256; i++) {
            int v = xbox_InputMap_ApplyTrigger(i, dz);
            CHECK(v >= last); last = v;
        }
        CHECK(last == 255);
    }
    for (i = 1; i < INPUT_KEY_COUNT; i++) {
        InputBindings a = {0}, b = {0};
        char text[80];
        if (!xbox_InputMap_KeyName(i)) continue;
        a.count[INPUT_A] = 1; a.key[INPUT_A][0] = (uint8_t)i;
        xbox_InputMap_BindingsFormat(&a, INPUT_A, text, sizeof(text));
        CHECK(xbox_InputMap_BindingsParse(&b, INPUT_A, text) == 0);
        CHECK(b.count[INPUT_A] == 1 && b.key[INPUT_A][0] == i);
    }
}

int main(void)
{
    test_key_names();
    test_bindings();
    test_saved_strings();
    test_resolve();
    test_merge();
    test_deadzone();
    test_padmap();
    test_api_edges();
    test_properties();
    if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
    printf("input_map: %d checks passed\n", g_checks);
    return 0;
}
