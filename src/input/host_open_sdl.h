/**
 * xbox_input host_open, SDL side (POSIX): the menu's request to show a
 * folder, applied on the window's thread with SDL_OpenURL. See host_open.h.
 */
#ifndef XBOX_INPUT_HOST_OPEN_SDL_H
#define XBOX_INPUT_HOST_OPEN_SDL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Once per pass of the main loop, on its thread, beside
 * xbox_InputSdlTextSync. Returns 1 when it opened a folder. */
int xbox_HostOpenSdlSync(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_HOST_OPEN_SDL_H */
