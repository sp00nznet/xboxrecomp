/*
 * The SDL window's typed text into the ring (text_input.c), on POSIX hosts.
 *
 * SDL_StartTextInput, SDL_StopTextInput and SDL_SetTextInputArea must run
 * on the window's thread, and the menu asks for text input from the
 * guest's, so the menu only stores a request and this side applies it once
 * per loop pass, as mouse_sdl.c mirrors the capture. On the Steam Deck in
 * Game Mode, SDL opens Steam's on-screen keyboard from SDL_StartTextInput
 * and places it by the area.
 */
#include "text_input_sdl.h"
#include "text_input.h"

static unsigned s_applied;      /* the request generation last applied */

void xbox_InputSdlTextEvent(const SDL_Event *ev)
{
    if (ev->type == SDL_EVENT_TEXT_INPUT)
        xbox_InputTextPushUtf8(ev->text.text);
}

int xbox_InputSdlTextSync(SDL_Window *win)
{
    int on, rect[4];
    unsigned gen = xbox_InputTextRequest(&on, rect);

    bool ok = true;

    if (gen == s_applied)
        return 0;
    /* Not applied without a window: the next pass with one applies it. */
    if (!win)
        return 0;
    if (on) {
        /* The field is in placement pixels (the window's pixels); the area
         * is in window coordinates, which differ on a HiDPI display. */
        int ww = 0, wh = 0, pw = 0, ph = 0;
        SDL_Rect r;
        SDL_GetWindowSize(win, &ww, &wh);
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        if (pw <= 0 || ph <= 0 || ww <= 0 || wh <= 0) {
            pw = ww = 1;
            ph = wh = 1;
        }
        r.x = (int)((long long)rect[0] * ww / pw);
        r.y = (int)((long long)rect[1] * wh / ph);
        r.w = (int)((long long)rect[2] * ww / pw);
        r.h = (int)((long long)rect[3] * wh / ph);
        ok = SDL_SetTextInputArea(win, &r, 0);
        if (ok && !SDL_TextInputActive(win))
            ok = SDL_StartTextInput(win);
    } else if (SDL_TextInputActive(win)) {
        ok = SDL_StopTextInput(win);
    }
    /* A failed call is retried on the next pass rather than taken as
     * applied. */
    if (!ok)
        return 0;
    s_applied = gen;
    return 1;
}
