/**
 * xbox_input mouse, SDL side (POSIX): the SDL window's mouse and focus
 * events into the mouse of mouse.h, and its capture state onto SDL's
 * relative mode. See mouse_sdl.c.
 */
#ifndef XBOX_INPUT_MOUSE_SDL_H
#define XBOX_INPUT_MOUSE_SDL_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Every event of the main loop: motion, buttons, the wheel and the window
 * events that move the capture. Others are ignored. */
void xbox_InputSdlMouseEvent(const SDL_Event *ev);
/* Once per pass of the main loop, on its thread: relative mode on `win` on
 * or off to match xbox_InputMouseCaptured(). Returns 1 when it changed the
 * mode. */
int  xbox_InputSdlMouseSync(SDL_Window *win);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_MOUSE_SDL_H */
