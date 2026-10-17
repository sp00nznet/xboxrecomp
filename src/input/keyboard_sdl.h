/**
 * xbox_input keyboard, SDL side (POSIX): the SDL window's key and focus
 * events into the key table of keyboard.h. See keyboard_sdl.c.
 */
#ifndef XBOX_INPUT_KEYBOARD_SDL_H
#define XBOX_INPUT_KEYBOARD_SDL_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An SDL keycode (SDL_Keycode, unmodified) as a Windows VK, or 0 when the
 * key is not one the table records. */
int  xbox_InputSdlKeyToVk(int sdl_keycode);
/* The same with the sides kept: LShift/RShift, LCtrl/RCtrl, LAlt/RAlt and
 * keypad Enter get input_names.h's own codes. The event path uses this. */
int  xbox_InputSdlKeyToCode(int sdl_keycode);
/* Once the window exists: stop SDL's text input. */
void xbox_InputSdlInit(void);
/* Every event of the main loop: keys, and the window events that release
 * them. Others are ignored. */
void xbox_InputSdlEvent(const SDL_Event *ev);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_KEYBOARD_SDL_H */
