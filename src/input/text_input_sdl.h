/**
 * xbox_input text, SDL side (POSIX): SDL_EVENT_TEXT_INPUT into the ring of
 * text_input.h, and its request onto SDL's text input. See text_input_sdl.c.
 */
#ifndef XBOX_INPUT_TEXT_INPUT_SDL_H
#define XBOX_INPUT_TEXT_INPUT_SDL_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Every event of the main loop; only SDL_EVENT_TEXT_INPUT is read. */
void xbox_InputSdlTextEvent(const SDL_Event *ev);
/* Once per pass of the main loop, on its thread, beside
 * xbox_InputSdlMouseSync: a new request starts or stops SDL's text input on
 * `win` and sets the field's area. Returns 1 when it applied one. */
int  xbox_InputSdlTextSync(SDL_Window *win);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_TEXT_INPUT_SDL_H */
