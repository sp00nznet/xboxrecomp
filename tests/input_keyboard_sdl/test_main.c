/*
 * input_keyboard_sdl test: see CMakeLists.txt.
 *
 * Synthetic SDL events through xbox_InputSdlEvent(), checked in the key
 * table: down, auto-repeat, up, Cmd+key, Cmd going down with a key held,
 * focus lost, hidden, minimized, an unmapped key, both sides of a modifier,
 * mouse buttons; and the keycode -> VK rows, written out here independently
 * of keyboard_sdl.c, for every key input_map.h has a name for.
 */
#include "keyboard.h"
#include "keyboard_sdl.h"
#include "input_map.h"

#include <stdio.h>
#include <string.h>

static int g_fail, g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

static void key(Uint32 type, SDL_Keycode sym, Uint16 mod, Uint8 repeat)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
    ev.key.repeat = repeat;
    ev.key.keysym.sym = sym;
    ev.key.keysym.mod = mod;
    xbox_InputSdlEvent(&ev);
}

static void window(Uint8 what)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_WINDOWEVENT;
    ev.window.event = what;
    xbox_InputSdlEvent(&ev);
}

static int held_count(void)
{
    int n = 0;
    for (int vk = 0; vk < 256; vk++)
        n += xbox_FramebufferKeyDown(vk);
    return n;
}

static void test_keycodes(void)
{
    static const struct { SDL_Keycode sym; int vk; } rows[] = {
        { SDLK_a, 'A' }, { SDLK_c, 'C' }, { SDLK_d, 'D' }, { SDLK_e, 'E' },
        { SDLK_i, 'I' }, { SDLK_j, 'J' }, { SDLK_k, 'K' }, { SDLK_l, 'L' },
        { SDLK_q, 'Q' }, { SDLK_s, 'S' }, { SDLK_v, 'V' }, { SDLK_w, 'W' },
        { SDLK_x, 'X' }, { SDLK_z, 'Z' }, { SDLK_1, '1' }, { SDLK_3, '3' },
        { SDLK_RETURN, 0x0D }, { SDLK_KP_ENTER, 0x0D }, { SDLK_BACKSPACE, 0x08 },
        { SDLK_LSHIFT, 0x10 }, { SDLK_RSHIFT, 0x10 },
        { SDLK_LCTRL, 0x11 },  { SDLK_RCTRL, 0x11 },
        { SDLK_LEFT, 0x25 }, { SDLK_UP, 0x26 }, { SDLK_RIGHT, 0x27 }, { SDLK_DOWN, 0x28 },
        { SDLK_KP_2, 0x62 }, { SDLK_KP_4, 0x64 }, { SDLK_KP_6, 0x66 }, { SDLK_KP_8, 0x68 },
        /* Every other key a binding can name (input_map.h). */
        { SDLK_LALT, 0x12 }, { SDLK_RALT, 0x12 },
        { SDLK_TAB, 0x09 }, { SDLK_ESCAPE, 0x1B }, { SDLK_SPACE, 0x20 },
        { SDLK_PAUSE, 0x13 }, { SDLK_CAPSLOCK, 0x14 },
        { SDLK_PAGEUP, 0x21 }, { SDLK_PAGEDOWN, 0x22 }, { SDLK_END, 0x23 }, { SDLK_HOME, 0x24 },
        { SDLK_INSERT, 0x2D }, { SDLK_DELETE, 0x2E },
        { SDLK_b, 'B' }, { SDLK_f, 'F' }, { SDLK_g, 'G' }, { SDLK_h, 'H' }, { SDLK_m, 'M' },
        { SDLK_n, 'N' }, { SDLK_o, 'O' }, { SDLK_p, 'P' }, { SDLK_r, 'R' }, { SDLK_t, 'T' },
        { SDLK_u, 'U' }, { SDLK_y, 'Y' },
        { SDLK_0, '0' }, { SDLK_2, '2' }, { SDLK_4, '4' }, { SDLK_5, '5' }, { SDLK_6, '6' },
        { SDLK_7, '7' }, { SDLK_8, '8' }, { SDLK_9, '9' },
        { SDLK_F1, 0x70 }, { SDLK_F2, 0x71 }, { SDLK_F3, 0x72 }, { SDLK_F4, 0x73 },
        { SDLK_F5, 0x74 }, { SDLK_F6, 0x75 }, { SDLK_F7, 0x76 }, { SDLK_F8, 0x77 },
        { SDLK_F9, 0x78 }, { SDLK_F10, 0x79 }, { SDLK_F11, 0x7A }, { SDLK_F12, 0x7B },
        { SDLK_KP_0, 0x60 }, { SDLK_KP_1, 0x61 }, { SDLK_KP_3, 0x63 }, { SDLK_KP_5, 0x65 },
        { SDLK_KP_7, 0x67 }, { SDLK_KP_9, 0x69 },
        { SDLK_KP_MULTIPLY, 0x6A }, { SDLK_KP_PLUS, 0x6B }, { SDLK_KP_MINUS, 0x6D },
        { SDLK_KP_PERIOD, 0x6E }, { SDLK_KP_DIVIDE, 0x6F },
        { SDLK_SEMICOLON, 0xBA }, { SDLK_EQUALS, 0xBB }, { SDLK_COMMA, 0xBC },
        { SDLK_MINUS, 0xBD }, { SDLK_PERIOD, 0xBE }, { SDLK_SLASH, 0xBF },
        { SDLK_BACKQUOTE, 0xC0 }, { SDLK_LEFTBRACKET, 0xDB }, { SDLK_BACKSLASH, 0xDC },
        { SDLK_RIGHTBRACKET, 0xDD }, { SDLK_QUOTE, 0xDE },
        /* Not keys here. */
        { SDLK_LGUI, 0 }, { SDLK_RGUI, 0 }, { SDLK_F13, 0 }, { SDLK_PRINTSCREEN, 0 },
        { SDLK_APPLICATION, 0 }, { SDLK_KP_EQUALS, 0 }, { SDLK_UNKNOWN, 0 },
    };
    static const struct { SDL_Keycode sym; int vk; } sides[] = {
        { SDLK_LSHIFT, 0xA0 }, { SDLK_RSHIFT, 0xA1 }, { SDLK_LCTRL, 0xA2 },
        { SDLK_RCTRL, 0xA3 },  { SDLK_LALT, 0xA4 },   { SDLK_RALT, 0xA5 },
        { SDLK_a, 0 }, { SDLK_RETURN, 0 }, { SDLK_LGUI, 0 },
    };
    static const struct { int button; int vk; } buttons[] = {
        { SDL_BUTTON_LEFT, 0x01 }, { SDL_BUTTON_RIGHT, 0x02 }, { SDL_BUTTON_MIDDLE, 0x04 },
        { SDL_BUTTON_X1, 0x05 }, { SDL_BUTTON_X2, 0x06 }, { 0, 0 }, { 6, 0 },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
        CHECK(xbox_InputSdlKeyToVk(rows[i].sym) == rows[i].vk,
              "keycode 0x%X -> 0x%02X, want 0x%02X", (unsigned)rows[i].sym,
              xbox_InputSdlKeyToVk(rows[i].sym), rows[i].vk);
    /* Every VK the map reads can be reached from some SDL key. */
    for (size_t r = 0; r < xbox_key_map_count; r++)
        for (int k = 0; k < 2; k++) {
            int vk = xbox_key_map[r].vk[k], hit = 0;
            if (!vk) continue;
            for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
                hit |= rows[i].vk == vk;
            CHECK(hit, "map key vk 0x%02X (%s) has no SDL key here", vk, xbox_key_map[r].keys);
        }
    for (size_t i = 0; i < sizeof sides / sizeof sides[0]; i++)
        CHECK(xbox_InputSdlKeyToSideVk(sides[i].sym) == sides[i].vk,
              "keycode 0x%X -> side 0x%02X, want 0x%02X", (unsigned)sides[i].sym,
              xbox_InputSdlKeyToSideVk(sides[i].sym), sides[i].vk);
    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
        CHECK(xbox_InputSdlButtonToVk(buttons[i].button) == buttons[i].vk,
              "button %d -> 0x%02X, want 0x%02X", buttons[i].button,
              xbox_InputSdlButtonToVk(buttons[i].button), buttons[i].vk);
    /* Every VK a binding can name can be held from some SDL key or button,
     * and the rows above cover every name. */
    for (int vk = 1; vk < 256; vk++) {
        int hit = 0;
        if (!xbox_InputMap_KeyName(vk))
            continue;
        for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
            hit |= rows[i].vk == vk;
        for (size_t i = 0; i < sizeof sides / sizeof sides[0]; i++)
            hit |= sides[i].vk == vk;
        for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
            hit |= buttons[i].vk == vk;
        CHECK(hit, "key \"%s\" (vk 0x%02X) has no SDL key or button here",
              xbox_InputMap_KeyName(vk), vk);
    }
    /* And no two SDL keys give one VK, except the pairs that always have
     * (Return and keypad Enter, the two sides' either-side code). */
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
        for (size_t j = i + 1; j < sizeof rows / sizeof rows[0]; j++) {
            int a = rows[i].vk;
            if (!a || a != rows[j].vk || a == 0x0D || a == 0x10 || a == 0x11 || a == 0x12)
                continue;
            CHECK(rows[i].sym == rows[j].sym, "keycodes 0x%X and 0x%X both give 0x%02X",
                  (unsigned)rows[i].sym, (unsigned)rows[j].sym, a);
        }
}

static void test_events(void)
{
    xbox_InputKeysClear();

    /* Down, repeat, up. */
    key(SDL_KEYDOWN, SDLK_RETURN, 0, 0);
    CHECK(xbox_FramebufferKeyDown(0x0D), "Return down not held");
    key(SDL_KEYDOWN, SDLK_RETURN, 0, 1);
    CHECK(xbox_FramebufferKeyDown(0x0D) && held_count() == 1, "repeat changed the table");
    key(SDL_KEYUP, SDLK_RETURN, 0, 0);
    CHECK(!xbox_FramebufferKeyDown(0x0D), "Return up still held");

    /* A repeat on its own (its down was lost) presses nothing. */
    key(SDL_KEYDOWN, SDLK_z, 0, 1);
    CHECK(held_count() == 0, "repeat-only down pressed a key");

    /* Shift held is a key, and Shift+letter is still the letter. */
    key(SDL_KEYDOWN, SDLK_LSHIFT, KMOD_LSHIFT, 0);
    key(SDL_KEYDOWN, SDLK_w, KMOD_LSHIFT, 0);
    CHECK(xbox_FramebufferKeyDown(0x10) && xbox_FramebufferKeyDown('W'), "Shift+W");
    key(SDL_KEYUP, SDLK_w, KMOD_LSHIFT, 0);
    key(SDL_KEYUP, SDLK_LSHIFT, 0, 0);
    CHECK(held_count() == 0, "Shift+W released");

    /* Cmd+key is a shortcut: ignored. Its key-up still applies. */
    key(SDL_KEYDOWN, SDLK_q, KMOD_LGUI, 0);
    CHECK(!xbox_FramebufferKeyDown('Q'), "Cmd+Q pressed Q");

    /* Cmd going down releases what is held. */
    key(SDL_KEYDOWN, SDLK_d, 0, 0);
    key(SDL_KEYDOWN, SDLK_RGUI, 0, 0);
    CHECK(held_count() == 0, "Cmd down left %d keys held", held_count());
    key(SDL_KEYDOWN, SDLK_d, 0, 0);
    key(SDL_KEYUP, SDLK_d, KMOD_LGUI, 0);
    CHECK(!xbox_FramebufferKeyDown('D'), "key-up with Cmd held not applied");

    /* Unmapped keys do nothing. */
    key(SDL_KEYDOWN, SDLK_F13, 0, 0);
    key(SDL_KEYDOWN, SDLK_PRINTSCREEN, 0, 0);
    CHECK(held_count() == 0, "unmapped key held");

    /* Keys outside the map are held too, for bindings: F5, Escape, Space. */
    key(SDL_KEYDOWN, SDLK_F5, 0, 0);
    key(SDL_KEYDOWN, SDLK_ESCAPE, 0, 0);
    key(SDL_KEYDOWN, SDLK_SPACE, 0, 0);
    CHECK(xbox_FramebufferKeyDown(0x74) && xbox_FramebufferKeyDown(0x1B)
          && xbox_FramebufferKeyDown(0x20) && held_count() == 3, "F5, Esc, Space");
    key(SDL_KEYUP, SDLK_F5, 0, 0);
    key(SDL_KEYUP, SDLK_ESCAPE, 0, 0);
    key(SDL_KEYUP, SDLK_SPACE, 0, 0);
    CHECK(held_count() == 0, "F5, Esc, Space released");

    /* A modifier is its side's key and either side's. The either-side key
     * goes up with whichever side goes up, as before sides were recorded
     * and as WM_KEYUP does on Win32. */
    key(SDL_KEYDOWN, SDLK_RSHIFT, KMOD_RSHIFT, 0);
    CHECK(xbox_FramebufferKeyDown(0x10) && xbox_FramebufferKeyDown(0xA1)
          && !xbox_FramebufferKeyDown(0xA0) && held_count() == 2, "RShift");
    key(SDL_KEYDOWN, SDLK_LCTRL, KMOD_LCTRL, 0);
    key(SDL_KEYDOWN, SDLK_RALT, KMOD_RALT, 0);
    CHECK(xbox_FramebufferKeyDown(0x11) && xbox_FramebufferKeyDown(0xA2)
          && xbox_FramebufferKeyDown(0x12) && xbox_FramebufferKeyDown(0xA5)
          && held_count() == 6, "LCtrl, RAlt");
    key(SDL_KEYUP, SDLK_RSHIFT, 0, 0);
    CHECK(!xbox_FramebufferKeyDown(0x10) && !xbox_FramebufferKeyDown(0xA1), "RShift up");
    key(SDL_KEYUP, SDLK_LCTRL, 0, 0);
    key(SDL_KEYUP, SDLK_RALT, 0, 0);
    CHECK(held_count() == 0, "modifiers released");

    /* Mouse buttons. */
    {
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = SDL_MOUSEBUTTONDOWN;
        ev.button.button = SDL_BUTTON_LEFT;
        xbox_InputSdlEvent(&ev);
        ev.button.button = SDL_BUTTON_X2;
        xbox_InputSdlEvent(&ev);
        CHECK(xbox_FramebufferKeyDown(0x01) && xbox_FramebufferKeyDown(0x06)
              && held_count() == 2, "mouse buttons down");
        ev.type = SDL_MOUSEBUTTONUP;
        xbox_InputSdlEvent(&ev);
        CHECK(xbox_FramebufferKeyDown(0x01) && held_count() == 1, "X2 up");
        ev.type = SDL_MOUSEBUTTONDOWN;
        ev.button.button = SDL_BUTTON_RIGHT;
        xbox_InputSdlEvent(&ev);
        window(SDL_WINDOWEVENT_FOCUS_LOST);
        CHECK(held_count() == 0, "focus lost left mouse buttons held");
    }

    /* A binding to a key outside the map works through the same table. */
    {
        InputBindings b;
        XBOX_GAMEPAD p;
        uint8_t down[INPUT_KEY_COUNT];
        memset(&b, 0, sizeof b);
        xbox_InputMap_BindingsParse(&b, INPUT_A, "space, lshift");
        key(SDL_KEYDOWN, SDLK_LSHIFT, KMOD_LSHIFT, 0);
        for (int vk = 0; vk < INPUT_KEY_COUNT; vk++)
            down[vk] = (uint8_t)xbox_FramebufferKeyDown(vk);
        xbox_InputMap_KeysResolve(&b, down, &p);
        CHECK(p.bAnalogButtons[XBOX_BUTTON_A] == 255, "lshift binding not held");
        key(SDL_KEYUP, SDLK_LSHIFT, 0, 0);
        key(SDL_KEYDOWN, SDLK_RSHIFT, KMOD_RSHIFT, 0);
        for (int vk = 0; vk < INPUT_KEY_COUNT; vk++)
            down[vk] = (uint8_t)xbox_FramebufferKeyDown(vk);
        xbox_InputMap_KeysResolve(&b, down, &p);
        CHECK(p.bAnalogButtons[XBOX_BUTTON_A] == 0, "rshift pressed an lshift binding");
        key(SDL_KEYUP, SDLK_RSHIFT, 0, 0);
    }

    /* Focus lost, hidden, minimized each release everything. */
    static const Uint8 kinds[] = { SDL_WINDOWEVENT_FOCUS_LOST, SDL_WINDOWEVENT_HIDDEN,
                                   SDL_WINDOWEVENT_MINIMIZED };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        key(SDL_KEYDOWN, SDLK_UP, 0, 0);
        key(SDL_KEYDOWN, SDLK_KP_8, 0, 0);
        CHECK(held_count() == 2, "setup for window event %d", kinds[i]);
        window(kinds[i]);
        CHECK(held_count() == 0, "window event %d left keys held", kinds[i]);
    }

    /* Other window events leave keys alone. */
    key(SDL_KEYDOWN, SDLK_UP, 0, 0);
    window(SDL_WINDOWEVENT_FOCUS_GAINED);
    window(SDL_WINDOWEVENT_EXPOSED);
    CHECK(xbox_FramebufferKeyDown(0x26), "focus gained / exposed released a key");
    xbox_InputKeysClear();
}

int main(void)
{
    /* The dummy video driver (set by ctest) is enough for SDL_StopTextInput. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    xbox_InputSdlInit();
    CHECK(!SDL_IsTextInputActive(), "text input still active after init");
    test_keycodes();
    test_events();
    SDL_Quit();
    printf("input_keyboard_sdl: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
