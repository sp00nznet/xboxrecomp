/**
 * xbox_input keyboard, SDL side (POSIX): the SDL window's key, mouse-button
 * and focus events into the key table of keyboard.h. See keyboard_sdl.c.
 */
#ifndef XBOX_INPUT_KEYBOARD_SDL_H
#define XBOX_INPUT_KEYBOARD_SDL_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An SDL keycode (SDL_Keycode, unmodified) as a Windows VK, or 0 when the
 * key is not one the table records. Every key input_map.h has a name for
 * is recorded; a left or right modifier gives the either-side VK here. */
int  xbox_InputSdlKeyToVk(int sdl_keycode);
/* For a left or right Shift, Ctrl or Alt, its side-specific VK (VK_LSHIFT
 * and so on), which is recorded beside the one above; else 0. */
int  xbox_InputSdlKeyToSideVk(int sdl_keycode);
/* An SDL mouse button (SDL_BUTTON_LEFT ...) as its VK (VK_LBUTTON ...), or 0. */
int  xbox_InputSdlButtonToVk(int sdl_button);
/* Once the window exists: stop SDL's text input. */
void xbox_InputSdlInit(void);
/* Every event of the main loop: keys, mouse buttons, and the window events
 * that release them. Others are ignored. */
void xbox_InputSdlEvent(const SDL_Event *ev);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_KEYBOARD_SDL_H */
