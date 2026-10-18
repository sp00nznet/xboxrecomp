/*
 * The SDL window's keys into the key table (keyboard.c), on POSIX hosts.
 *
 * The presenter's main loop (src/video/fb_present_sdl.c) hands every event
 * here and has no key logic of its own, so tests/input_keyboard_sdl drives
 * these rules with synthetic events without linking the presenter.
 *
 * Keycodes, not scancodes: a Windows VK for a letter follows the keyboard
 * layout (the key labelled Z is 'Z' on AZERTY too), and so does an SDL
 * keycode, so the one map means the same keys on both.
 */
#include "keyboard_sdl.h"
#include "keyboard.h"
#include "input_names.h"

/* SDL2's keycodes are ABI-stable; the letter and digit arithmetic below
 * relies on these. */
_Static_assert(SDLK_a == 'a' && SDLK_z == 'z' && SDLK_0 == '0' && SDLK_9 == '9',
               "SDL letter and digit keycodes are their ASCII codes");
_Static_assert(SDLK_RETURN == '\r' && SDLK_BACKSPACE == '\b',
               "SDL Return and Backspace keycodes are their ASCII codes");

/* The keys every host names the same way: letters, digits, the F-keys, the
 * named keys and the keypad. The sides of Shift, Ctrl and Alt and keypad
 * Enter are left to the two callers below. */
static int common_key(int key)
{
    if (key >= SDLK_a && key <= SDLK_z)
        return key - 'a' + 'A';
    if (key >= SDLK_0 && key <= SDLK_9)
        return key;
    if (key >= SDLK_F1 && key <= SDLK_F12)
        return 0x70 + (key - SDLK_F1);
    switch (key) {
    case SDLK_RETURN:       return XBOX_VK_RETURN;
    case SDLK_BACKSPACE:    return XBOX_VK_BACK;
    case SDLK_TAB:          return 0x09;
    case SDLK_ESCAPE:       return 0x1B;
    case SDLK_SPACE:        return 0x20;
    case SDLK_UP:           return XBOX_VK_UP;
    case SDLK_DOWN:         return XBOX_VK_DOWN;
    case SDLK_LEFT:         return XBOX_VK_LEFT;
    case SDLK_RIGHT:        return XBOX_VK_RIGHT;
    case SDLK_INSERT:       return 0x2D;
    case SDLK_DELETE:       return 0x2E;
    case SDLK_HOME:         return 0x24;
    case SDLK_END:          return 0x23;
    case SDLK_PAGEUP:       return 0x21;
    case SDLK_PAGEDOWN:     return 0x22;
    case SDLK_KP_0:         return 0x60;
    case SDLK_KP_1:         return 0x61;
    case SDLK_KP_2:         return XBOX_VK_NUMPAD2;
    case SDLK_KP_3:         return 0x63;
    case SDLK_KP_4:         return XBOX_VK_NUMPAD4;
    case SDLK_KP_5:         return 0x65;
    case SDLK_KP_6:         return XBOX_VK_NUMPAD6;
    case SDLK_KP_7:         return 0x67;
    case SDLK_KP_8:         return XBOX_VK_NUMPAD8;
    case SDLK_KP_9:         return 0x69;
    case SDLK_KP_MULTIPLY:  return 0x6A;
    case SDLK_KP_PLUS:      return 0x6B;
    case SDLK_KP_MINUS:     return 0x6D;
    case SDLK_KP_PERIOD:    return 0x6E;
    case SDLK_KP_DIVIDE:    return 0x6F;
    /* The OEM keys by their US labels, as input_names.c names them. */
    case SDLK_SEMICOLON:    return 0xBA;
    case SDLK_EQUALS:       return 0xBB;
    case SDLK_COMMA:        return 0xBC;
    case SDLK_MINUS:        return 0xBD;
    case SDLK_PERIOD:       return 0xBE;
    case SDLK_SLASH:        return 0xBF;
    case SDLK_BACKQUOTE:    return 0xC0;
    case SDLK_LEFTBRACKET:  return 0xDB;
    case SDLK_BACKSLASH:    return 0xDC;
    case SDLK_RIGHTBRACKET: return 0xDD;
    case SDLK_QUOTE:        return 0xDE;
    }
    return 0;
}

int xbox_InputSdlKeyToVk(int key)
{
    switch (key) {
    case SDLK_KP_ENTER:  return XBOX_VK_RETURN;   /* both, as Win32 reports */
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:    return XBOX_VK_SHIFT;
    case SDLK_LCTRL:
    case SDLK_RCTRL:     return XBOX_VK_CONTROL;
    case SDLK_LALT:
    case SDLK_RALT:      return XBOX_VK_MENU;
    }
    return common_key(key);
}

int xbox_InputSdlKeyToCode(int key)
{
    switch (key) {
    case SDLK_KP_ENTER:  return XBOX_VK_KP_ENTER;
    case SDLK_LSHIFT:    return XBOX_VK_LSHIFT;
    case SDLK_RSHIFT:    return XBOX_VK_RSHIFT;
    case SDLK_LCTRL:     return XBOX_VK_LCONTROL;
    case SDLK_RCTRL:     return XBOX_VK_RCONTROL;
    case SDLK_LALT:      return XBOX_VK_LMENU;
    case SDLK_RALT:      return XBOX_VK_RMENU;
    }
    return common_key(key);
}

void xbox_InputSdlInit(void)
{
    /* SDL2 starts text input on desktop at init. The game takes no text,
     * and on macOS text input lets the press-and-hold accent popover take a
     * held letter (W held to walk would stop walking). */
    SDL_StopTextInput();
}

void xbox_InputSdlEvent(const SDL_Event *ev)
{
    int vk;

    switch (ev->type) {
    case SDL_KEYDOWN:
        /* Auto-repeat would only re-set a held key; dropping it keeps the
         * trace to real edges. */
        if (ev->key.repeat)
            return;
        /* Cmd+key is an app shortcut (Cmd-Tab, Cmd-H, Cmd-Q), not a game
         * key. Cmd itself releases whatever is held, which is what the
         * shortcut that follows expects; key-ups still apply below. */
        if (ev->key.keysym.sym == SDLK_LGUI || ev->key.keysym.sym == SDLK_RGUI) {
            xbox_InputKeysClear();
            return;
        }
        if (ev->key.keysym.mod & KMOD_GUI)
            return;
        /* The sided code: xbox_InputKeyEvent keeps the either-side
         * entries and traces the edge by name. */
        vk = xbox_InputSdlKeyToCode(ev->key.keysym.sym);
        if (vk)
            xbox_InputKeyEvent(vk, 1);
        return;
    case SDL_KEYUP:
        vk = xbox_InputSdlKeyToCode(ev->key.keysym.sym);
        if (vk)
            xbox_InputKeyEvent(vk, 0);
        return;
    case SDL_WINDOWEVENT:
        /* SDL sends keys only to the focused window, so there is no focus
         * check on the way in; on the way out a held key would stay held
         * for ever (Cmd-Tab, Mission Control, Cmd-H, Cmd-M). */
        if (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST
            || ev->window.event == SDL_WINDOWEVENT_HIDDEN
            || ev->window.event == SDL_WINDOWEVENT_MINIMIZED)
            xbox_InputKeysClear();
        return;
    }
}
