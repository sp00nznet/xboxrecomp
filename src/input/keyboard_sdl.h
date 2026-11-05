/**
 * xbox_input keyboard, SDL side (POSIX): the SDL window's key, mouse-button
 * and focus events into the key table of keyboard.h. See keyboard_sdl.c.
 */
#ifndef XBOX_INPUT_KEYBOARD_SDL_H
#define XBOX_INPUT_KEYBOARD_SDL_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An SDL keycode (SDL_Keycode, unmodified) as a Windows VK, or 0 when the
 * key is not one the table records. Every key input_map.h has a name for
 * is recorded; a left or right modifier gives the either-side VK here. */
int  xbox_InputSdlKeyToVk(int sdl_keycode);
/* The same with the sides kept: LShift/RShift, LCtrl/RCtrl, LAlt/RAlt and
 * keypad Enter get input_names.h's own codes. The event path uses this. */
int  xbox_InputSdlKeyToCode(int sdl_keycode);
/* Once the window exists: stop SDL's text input on it. */
void xbox_InputSdlInit(SDL_Window *win);
/* Every event of the main loop: keys, and the window events that release
 * them. Others are ignored. */
void xbox_InputSdlEvent(const SDL_Event *ev);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_KEYBOARD_SDL_H */
