/*
 * input_keyboard_sdl test: see CMakeLists.txt.
 *
 * Synthetic SDL events through xbox_InputSdlEvent(), checked in the key
 * table: down, auto-repeat, up, Cmd+key, Cmd going down with a key held,
 * focus lost, hidden, minimized, an unmapped key; and the keycode -> VK
 * rows, written out here independently of keyboard_sdl.c.
 */
#include "keyboard.h"
#include "keyboard_sdl.h"
#include "input_names.h"
#include "mouse.h"
#include "mouse_sdl.h"

#include <stdio.h>
#include <string.h>

static int g_fail, g_checks;
static Uint32 g_ms = 1000;      /* the timestamp of the next window or button event */

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
    ev.window.timestamp = g_ms;
    ev.window.event = what;
    xbox_InputSdlEvent(&ev);
    xbox_InputSdlMouseEvent(&ev);   /* the presenter hands every event to both */
}

static void motion(int dx, int dy)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_MOUSEMOTION;
    ev.motion.xrel = dx;
    ev.motion.yrel = dy;
    xbox_InputSdlMouseEvent(&ev);
}

static void button(Uint8 b, int down)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    ev.button.timestamp = g_ms;
    ev.button.button = b;
    ev.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    xbox_InputSdlMouseEvent(&ev);
}

static void wheel(int y, Uint32 direction)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_MOUSEWHEEL;
    ev.wheel.y = y;
    ev.wheel.direction = direction;
    xbox_InputSdlMouseEvent(&ev);
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
        { SDLK_LALT, 0x12 }, { SDLK_RALT, 0x12 },
        { SDLK_F1, 0x70 }, { SDLK_F9, 0x78 }, { SDLK_F12, 0x7B },
        { SDLK_ESCAPE, 0x1B }, { SDLK_TAB, 0x09 }, { SDLK_SPACE, 0x20 },
        { SDLK_INSERT, 0x2D }, { SDLK_DELETE, 0x2E }, { SDLK_HOME, 0x24 },
        { SDLK_END, 0x23 }, { SDLK_PAGEUP, 0x21 }, { SDLK_PAGEDOWN, 0x22 },
        { SDLK_KP_0, 0x60 }, { SDLK_KP_5, 0x65 }, { SDLK_KP_9, 0x69 },
        { SDLK_KP_MULTIPLY, 0x6A }, { SDLK_KP_PLUS, 0x6B }, { SDLK_KP_MINUS, 0x6D },
        { SDLK_KP_PERIOD, 0x6E }, { SDLK_KP_DIVIDE, 0x6F },
        { SDLK_SEMICOLON, 0xBA }, { SDLK_EQUALS, 0xBB }, { SDLK_COMMA, 0xBC },
        { SDLK_MINUS, 0xBD }, { SDLK_PERIOD, 0xBE }, { SDLK_SLASH, 0xBF },
        { SDLK_BACKQUOTE, 0xC0 }, { SDLK_LEFTBRACKET, 0xDB }, { SDLK_BACKSLASH, 0xDC },
        { SDLK_RIGHTBRACKET, 0xDD }, { SDLK_QUOTE, 0xDE },
        { SDLK_LGUI, 0 }, { SDLK_RGUI, 0 }, { SDLK_CAPSLOCK, 0 }, { SDLK_PRINTSCREEN, 0 },
    };
    /* The sided codes of the event path (input_names.h). */
    static const struct { SDL_Keycode sym; int code; } sided[] = {
        { SDLK_LSHIFT, 0xA0 }, { SDLK_RSHIFT, 0xA1 }, { SDLK_LCTRL, 0xA2 },
        { SDLK_RCTRL, 0xA3 }, { SDLK_LALT, 0xA4 }, { SDLK_RALT, 0xA5 },
        { SDLK_KP_ENTER, 0x0E }, { SDLK_RETURN, 0x0D }, { SDLK_w, 'W' },
    };
    for (size_t i = 0; i < sizeof sided / sizeof sided[0]; i++)
        CHECK(xbox_InputSdlKeyToCode(sided[i].sym) == sided[i].code,
              "keycode 0x%X -> code 0x%02X, want 0x%02X", (unsigned)sided[i].sym,
              xbox_InputSdlKeyToCode(sided[i].sym), sided[i].code);
    /* Every key the binding vocabulary names is reachable from SDL. */
    {
        const char *name;
        int code, mouse, alias;
        for (int i = 0; xbox_InputNameAt(i, &name, &code, &mouse, &alias); i++) {
            int hit = 0;
            if (mouse || alias)
                continue;
            for (int sym = 0; sym < 128 && !hit; sym++)
                hit = xbox_InputSdlKeyToCode(sym) == code;
            for (int sc = 0; sc < SDL_NUM_SCANCODES && !hit; sc++)
                hit = xbox_InputSdlKeyToCode((int)SDL_SCANCODE_TO_KEYCODE(sc)) == code;
            if (!hit && (code == 0x10 || code == 0x11 || code == 0x12))
                hit = 1;              /* either side: no key of its own */
            CHECK(hit, "name %s (0x%02X) has no SDL key", name, code);
        }
    }
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

    /* The sides: either Shift holds Shift until both are up; keypad Enter
     * holds Return as Win32 reports it, and KPEnter by its own name. */
    key(SDL_KEYDOWN, SDLK_LSHIFT, 0, 0);
    key(SDL_KEYDOWN, SDLK_RSHIFT, 0, 0);
    key(SDL_KEYUP, SDLK_LSHIFT, 0, 0);
    CHECK(xbox_FramebufferKeyDown(0x10) && xbox_FramebufferKeyDown(0xA1)
          && !xbox_FramebufferKeyDown(0xA0), "RShift alone keeps Shift");
    key(SDL_KEYUP, SDLK_RSHIFT, 0, 0);
    CHECK(held_count() == 0, "both Shifts up");
    key(SDL_KEYDOWN, SDLK_KP_ENTER, 0, 0);
    CHECK(xbox_FramebufferKeyDown(0x0D) && xbox_FramebufferKeyDown(0x0E), "KPEnter");
    key(SDL_KEYDOWN, SDLK_RETURN, 0, 0);
    key(SDL_KEYUP, SDLK_KP_ENTER, 0, 0);
    CHECK(xbox_FramebufferKeyDown(0x0D), "Return still held after KPEnter up");
    key(SDL_KEYUP, SDLK_RETURN, 0, 0);
    CHECK(held_count() == 0, "Return and KPEnter up");
    key(SDL_KEYDOWN, SDLK_LALT, 0, 0);
    CHECK(xbox_FramebufferKeyDown(0x12) && xbox_FramebufferKeyDown(0xA4), "LAlt");
    key(SDL_KEYUP, SDLK_LALT, 0, 0);

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
    key(SDL_KEYDOWN, SDLK_CAPSLOCK, 0, 0);
    key(SDL_KEYDOWN, SDLK_PRINTSCREEN, 0, 0);
    CHECK(held_count() == 0, "unmapped key held");

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

static void test_mouse(void)
{
    xbox_mouse_opts o;
    int x, y;

    xbox_InputMouseEnable(1);
    xbox_InputMouseOpts(NULL);
    xbox_InputMouseReset();
    xbox_InputSdlMouseSync();
    CHECK(!xbox_InputSdlMouseSync(), "sync with nothing changed");

    /* Motion before the capture is dropped; focus grabs; motion adds up. */
    motion(30, 0);
    window(SDL_WINDOWEVENT_FOCUS_GAINED);
    CHECK(xbox_InputMouseCaptured(), "focus gained did not capture");
    CHECK(xbox_InputSdlMouseSync() == 1 && SDL_GetRelativeMouseMode() == SDL_TRUE,
          "sync did not turn relative mode on");
    CHECK(!xbox_InputSdlMouseSync(), "sync changed the mode twice");
    motion(10, -4);
    motion(10, -16);
    xbox_InputMouseLook(&x, &y);
    CHECK(x == xbox_InputMouseModel(20, 1.0, 0.45) && y == xbox_InputMouseModel(20, 1.0, 0.45),
          "motion: (%d, %d)", x, y);

    /* The click that focused the window (X11, Wayland: a press right after
     * FOCUS_GAINED) drives nothing; the next press does, and so does a
     * first press that comes well after the focus. */
    button(SDL_BUTTON_LEFT, 1);
    CHECK(!xbox_InputMouseHeld(XBOX_MOUSE_LEFT), "the focusing click drove the pad");
    button(SDL_BUTTON_LEFT, 0);
    g_ms += 40;
    button(SDL_BUTTON_LEFT, 1);
    CHECK(xbox_InputMouseHeld(XBOX_MOUSE_LEFT), "the press after the focusing click");
    button(SDL_BUTTON_LEFT, 0);
    g_ms += 1000;
    window(SDL_WINDOWEVENT_FOCUS_LOST);
    window(SDL_WINDOWEVENT_FOCUS_GAINED);
    g_ms += XBOX_MOUSE_FOCUS_CLICK_MS + 1;
    button(SDL_BUTTON_LEFT, 1);
    CHECK(xbox_InputMouseHeld(XBOX_MOUSE_LEFT), "a first press well after the focus");
    button(SDL_BUTTON_LEFT, 0);
    g_ms += 1000;

    /* Buttons: SDL's middle and right are not in mouse.h's order. */
    button(SDL_BUTTON_RIGHT, 1);
    button(SDL_BUTTON_MIDDLE, 1);
    button(SDL_BUTTON_X1, 1);
    CHECK(xbox_InputMouseHeld(XBOX_MOUSE_RIGHT) && xbox_InputMouseHeld(XBOX_MOUSE_MIDDLE)
          && xbox_InputMouseHeld(XBOX_MOUSE_4) && !xbox_InputMouseHeld(XBOX_MOUSE_LEFT),
          "buttons");
    button(SDL_BUTTON_RIGHT, 0);
    CHECK(!xbox_InputMouseHeld(XBOX_MOUSE_RIGHT), "button up");

    /* The wheel: up is away from the player, flipped or not. */
    wheel(1, SDL_MOUSEWHEEL_NORMAL);
    xbox_InputMouseTick(0, 1);
    CHECK(xbox_InputMouseHeld(XBOX_MOUSE_WHEEL_UP) && !xbox_InputMouseHeld(XBOX_MOUSE_WHEEL_DOWN),
          "wheel up");
    xbox_InputMouseTick(1000, 1);
    xbox_InputMouseTick(1001, 1);
    wheel(1, SDL_MOUSEWHEEL_FLIPPED);
    xbox_InputMouseTick(1002, 1);
    CHECK(xbox_InputMouseHeld(XBOX_MOUSE_WHEEL_DOWN) && !xbox_InputMouseHeld(XBOX_MOUSE_WHEEL_UP),
          "flipped wheel");
    xbox_InputMouseTick(2000, 1);

    /* Focus lost releases the capture and what is held. */
    window(SDL_WINDOWEVENT_FOCUS_LOST);
    CHECK(!xbox_InputMouseCaptured() && !xbox_InputMouseHeld(XBOX_MOUSE_MIDDLE),
          "focus lost kept the capture or a button");
    CHECK(xbox_InputSdlMouseSync() == 1 && SDL_GetRelativeMouseMode() == SDL_FALSE,
          "sync did not turn relative mode off");
    motion(40, 0);
    xbox_InputMouseLook(&x, &y);
    CHECK(!x && !y, "motion while released");

    /* click: the press before the capture grabs and is consumed. */
    xbox_InputMouseDefaultOpts(&o);
    o.capture = XBOX_MOUSE_CAPTURE_CLICK;
    xbox_InputMouseOpts(&o);
    window(SDL_WINDOWEVENT_FOCUS_GAINED);
    CHECK(!xbox_InputMouseCaptured(), "click mode captured on focus");
    button(SDL_BUTTON_LEFT, 1);
    CHECK(xbox_InputMouseCaptured() && !xbox_InputMouseHeld(XBOX_MOUSE_LEFT),
          "the grabbing click was not consumed");
    button(SDL_BUTTON_LEFT, 0);
    button(SDL_BUTTON_LEFT, 1);
    CHECK(xbox_InputMouseHeld(XBOX_MOUSE_LEFT), "after the capture the click drives");
    CHECK(xbox_InputSdlMouseSync() == 1, "sync after the click");

    /* Minimise and hide release too. */
    window(SDL_WINDOWEVENT_MINIMIZED);
    CHECK(!xbox_InputMouseCaptured(), "minimized kept the capture");
    xbox_InputSdlMouseSync();
    xbox_InputMouseOpts(NULL);
    xbox_InputMouseReset();
    xbox_InputMouseEnable(-1);
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
    test_mouse();
    SDL_Quit();
    printf("input_keyboard_sdl: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
