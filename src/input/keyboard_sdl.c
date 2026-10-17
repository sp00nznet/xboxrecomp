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
#include <stdio.h>
#include <stdlib.h>

/* SDL2's keycodes are ABI-stable; the letter and digit arithmetic below
 * relies on these. */
_Static_assert(SDLK_a == 'a' && SDLK_z == 'z' && SDLK_0 == '0' && SDLK_9 == '9',
               "SDL letter and digit keycodes are their ASCII codes");
_Static_assert(SDLK_RETURN == '\r' && SDLK_BACKSPACE == '\b',
               "SDL Return and Backspace keycodes are their ASCII codes");
_Static_assert(SDLK_F12 - SDLK_F1 == 11, "SDL F1..F12 keycodes are one run");

int xbox_InputSdlKeyToVk(int key)
{
    if (key >= SDLK_a && key <= SDLK_z)
        return key - 'a' + 'A';
    if (key >= SDLK_0 && key <= SDLK_9)
        return key;
    if (key >= SDLK_F1 && key <= SDLK_F12)
        return XBOX_VK_F1 + (key - SDLK_F1);
    switch (key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  return XBOX_VK_RETURN;   /* both, as Win32 reports */
    case SDLK_BACKSPACE: return XBOX_VK_BACK;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:    return XBOX_VK_SHIFT;
    case SDLK_LCTRL:
    case SDLK_RCTRL:     return XBOX_VK_CONTROL;
    case SDLK_LALT:
    case SDLK_RALT:      return XBOX_VK_MENU;
    case SDLK_UP:        return XBOX_VK_UP;
    case SDLK_DOWN:      return XBOX_VK_DOWN;
    case SDLK_LEFT:      return XBOX_VK_LEFT;
    case SDLK_RIGHT:     return XBOX_VK_RIGHT;
    case SDLK_TAB:       return XBOX_VK_TAB;
    case SDLK_ESCAPE:    return XBOX_VK_ESCAPE;
    case SDLK_SPACE:     return XBOX_VK_SPACE;
    case SDLK_PAUSE:     return XBOX_VK_PAUSE;
    case SDLK_CAPSLOCK:  return XBOX_VK_CAPITAL;
    case SDLK_PAGEUP:    return XBOX_VK_PRIOR;
    case SDLK_PAGEDOWN:  return XBOX_VK_NEXT;
    case SDLK_END:       return XBOX_VK_END;
    case SDLK_HOME:      return XBOX_VK_HOME;
    case SDLK_INSERT:    return XBOX_VK_INSERT;
    case SDLK_DELETE:    return XBOX_VK_DELETE;
    /* The keypad's digits are not one run of SDL keycodes (0 comes after 9). */
    case SDLK_KP_0:      return XBOX_VK_NUMPAD0;
    case SDLK_KP_1:      return XBOX_VK_NUMPAD0 + 1;
    case SDLK_KP_2:      return XBOX_VK_NUMPAD2;
    case SDLK_KP_3:      return XBOX_VK_NUMPAD0 + 3;
    case SDLK_KP_4:      return XBOX_VK_NUMPAD4;
    case SDLK_KP_5:      return XBOX_VK_NUMPAD0 + 5;
    case SDLK_KP_6:      return XBOX_VK_NUMPAD6;
    case SDLK_KP_7:      return XBOX_VK_NUMPAD0 + 7;
    case SDLK_KP_8:      return XBOX_VK_NUMPAD8;
    case SDLK_KP_9:      return XBOX_VK_NUMPAD9;
    case SDLK_KP_MULTIPLY: return XBOX_VK_MULTIPLY;
    case SDLK_KP_PLUS:   return XBOX_VK_ADD;
    case SDLK_KP_MINUS:  return XBOX_VK_SUBTRACT;
    case SDLK_KP_PERIOD: return XBOX_VK_DECIMAL;
    case SDLK_KP_DIVIDE: return XBOX_VK_DIVIDE;
    /* Punctuation: the keycode is the character the layout puts on the
     * key, so these are the keys that type it, as the Win32 OEM codes are
     * on a US layout. */
    case SDLK_SEMICOLON:    return XBOX_VK_OEM_1;
    case SDLK_EQUALS:       return XBOX_VK_OEM_PLUS;
    case SDLK_COMMA:        return XBOX_VK_OEM_COMMA;
    case SDLK_MINUS:        return XBOX_VK_OEM_MINUS;
    case SDLK_PERIOD:       return XBOX_VK_OEM_PERIOD;
    case SDLK_SLASH:        return XBOX_VK_OEM_2;
    case SDLK_BACKQUOTE:    return XBOX_VK_OEM_3;
    case SDLK_LEFTBRACKET:  return XBOX_VK_OEM_4;
    case SDLK_BACKSLASH:    return XBOX_VK_OEM_5;
    case SDLK_RIGHTBRACKET: return XBOX_VK_OEM_6;
    case SDLK_QUOTE:        return XBOX_VK_OEM_7;
    }
    return 0;
}

int xbox_InputSdlKeyToSideVk(int key)
{
    switch (key) {
    case SDLK_LSHIFT: return XBOX_VK_LSHIFT;
    case SDLK_RSHIFT: return XBOX_VK_RSHIFT;
    case SDLK_LCTRL:  return XBOX_VK_LCONTROL;
    case SDLK_RCTRL:  return XBOX_VK_RCONTROL;
    case SDLK_LALT:   return XBOX_VK_LMENU;
    case SDLK_RALT:   return XBOX_VK_RMENU;
    }
    return 0;
}

int xbox_InputSdlButtonToVk(int button)
{
    switch (button) {
    case SDL_BUTTON_LEFT:   return XBOX_VK_LBUTTON;
    case SDL_BUTTON_RIGHT:  return XBOX_VK_RBUTTON;
    case SDL_BUTTON_MIDDLE: return XBOX_VK_MBUTTON;
    case SDL_BUTTON_X1:     return XBOX_VK_XBUTTON1;
    case SDL_BUTTON_X2:     return XBOX_VK_XBUTTON2;
    }
    return 0;
}

void xbox_InputSdlInit(void)
{
    /* SDL2 starts text input on desktop at init. The game takes no text,
     * and on macOS text input lets the press-and-hold accent popover take a
     * held letter (W held to walk would stop walking). */
    SDL_StopTextInput();
}

/* RECOMP_KEY_TRACE: each key-down as it arrives, the first 40, the same
 * line the GDI window prints. */
static void trace_down(int vk)
{
    static unsigned n;
    if (getenv("RECOMP_KEY_TRACE") && n++ < 40) {
        fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)vk);
        fflush(stderr);
    }
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
        vk = xbox_InputSdlKeyToVk(ev->key.keysym.sym);
        if (vk) {
            xbox_InputKeySet(vk, 1);
            trace_down(vk);
        }
        /* A Shift, Ctrl or Alt is both keys, the side's and either side's,
         * as on the Win32 window, so "lshift" and "shift" both bind. */
        vk = xbox_InputSdlKeyToSideVk(ev->key.keysym.sym);
        if (vk)
            xbox_InputKeySet(vk, 1);
        return;
    case SDL_KEYUP:
        vk = xbox_InputSdlKeyToVk(ev->key.keysym.sym);
        if (vk)
            xbox_InputKeySet(vk, 0);
        vk = xbox_InputSdlKeyToSideVk(ev->key.keysym.sym);
        if (vk)
            xbox_InputKeySet(vk, 0);
        return;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        /* Mouse buttons are in the same table, for bindings that name
         * them. SDL captures the mouse while a button is down, so the
         * release arrives even outside the window. */
        vk = xbox_InputSdlButtonToVk(ev->button.button);
        if (vk)
            xbox_InputKeySet(vk, ev->type == SDL_MOUSEBUTTONDOWN);
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
