/*
 * The SDL window's mouse into the mouse (mouse.c), on POSIX hosts.
 *
 * Like keyboard_sdl.c, the presenter's main loop hands every event here and
 * keeps no mouse logic of its own, so tests/input_keyboard_sdl drives these
 * rules with synthetic events.
 *
 * The capture state machine lives in mouse.c and may change on the game's
 * poll thread (the release key arrives through the key table, a script
 * forces it). SDL's relative mode must be set on the window's thread, so
 * this side only mirrors the answer, once per loop pass.
 */
#include "mouse_sdl.h"
#include "mouse.h"
#include "input_names.h"

static int s_relative = -1;     /* the mode last applied; -1 none yet */

void xbox_InputSdlMouseEvent(const SDL_Event *ev)
{
    int n;

    switch (ev->type) {
    case SDL_MOUSEMOTION:
        /* A touch screen reports as a mouse too; it is not one. */
        if (ev->motion.which == SDL_TOUCH_MOUSEID)
            return;
        xbox_InputMouseMotion(ev->motion.xrel, ev->motion.yrel);
        return;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (ev->button.which == SDL_TOUCH_MOUSEID)
            return;
        switch (ev->button.button) {
        case SDL_BUTTON_LEFT:   n = XBOX_MOUSE_LEFT;   break;
        case SDL_BUTTON_RIGHT:  n = XBOX_MOUSE_RIGHT;  break;
        case SDL_BUTTON_MIDDLE: n = XBOX_MOUSE_MIDDLE; break;
        case SDL_BUTTON_X1:     n = XBOX_MOUSE_4;      break;
        case SDL_BUTTON_X2:     n = XBOX_MOUSE_5;      break;
        default:                return;
        }
        /* SDL reports no activation click: on X11 and Wayland the click
         * that focuses the window arrives as an ordinary press right after
         * the focus event, so mouse.c judges it by the two timestamps. On
         * macOS SDL drops that click itself (SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH
         * is off by default). */
        xbox_InputMouseButtonAt(n, ev->type == SDL_MOUSEBUTTONDOWN, ev->button.timestamp);
        return;
    case SDL_MOUSEWHEEL:
        if (ev->wheel.which == SDL_TOUCH_MOUSEID)
            return;
        /* "Natural" scrolling flips the sign SDL reports; the direction
         * field says so, and WheelUp means the wheel turned away from the
         * player on every host. */
        n = ev->wheel.y;
        if (ev->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
            n = -n;
        if (n)
            xbox_InputMouseWheel(n);
        return;
    case SDL_WINDOWEVENT:
        switch (ev->window.event) {
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            xbox_InputMouseFocusGainedAt(ev->window.timestamp);
            break;
        case SDL_WINDOWEVENT_FOCUS_LOST:
        case SDL_WINDOWEVENT_HIDDEN:
        case SDL_WINDOWEVENT_MINIMIZED:
            xbox_InputMouseFocus(1);
            break;
        case SDL_WINDOWEVENT_CLOSE:
            xbox_InputMouseClose();
            break;
        }
        return;
    case SDL_QUIT:
        xbox_InputMouseClose();
        return;
    }
}

int xbox_InputSdlMouseSync(void)
{
    int want = xbox_InputMouseCaptured();

    if (want == s_relative)
        return 0;
    /* Relative mode hides the cursor, confines it and reports motion past
     * the screen edge: the capture in one call. A failure (no mouse, the
     * dummy driver) leaves motion as absolute deltas, which still work
     * until the cursor reaches an edge. */
    SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
    s_relative = want;
    return 1;
}
