/**
 * xbox_input host_open -- show a folder in the desktop's file manager, for
 * the menu's "Open exports folder" (openspec overlay-revamp, D6).
 *
 * Windows opens it with ShellExecuteW on a short thread of its own (under
 * Proton that is Wine's explorer, not the host's file manager). SDL wants SDL_OpenURL on the window's
 * thread, as its text input, so on POSIX the menu only stores a request
 * and the main loop applies it (xbox_HostOpenSdlSync in host_open_sdl.c,
 * beside xbox_InputSdlTextSync). The menu hides the row in Game Mode,
 * where there is no file manager to show.
 *
 * No platform header here, so the menu's ctest takes the request.
 */
#ifndef XBOX_INPUT_HOST_OPEN_H
#define XBOX_INPUT_HOST_OPEN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_HOST_OPEN_PATH_MAX 1024

/* Open the folder at path (UTF-8). 1 when it was opened (Windows) or asked
 * for (POSIX); 0 when the path is too long, the call failed, or a request
 * is being read. A request not yet applied is replaced. */
int xbox_HostOpenFolder(const char *path);

/* The window's thread: the folder asked for into out, once, and 1; 0 when
 * there is none. */
int xbox_HostOpenTake(char *out, size_t n);

/* POSIX: path (relative ones resolved to absolute) as a file:// URL,
 * percent-encoded, into out; 0 when it does not fit. */
int xbox_HostOpenFileUrl(const char *path, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_HOST_OPEN_H */
