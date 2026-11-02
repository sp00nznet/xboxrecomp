/**
 * xbox_input host_open, SDL side (host_open_sdl.h).
 */
#include "host_open_sdl.h"
#include "host_open.h"

#include <SDL3/SDL.h>
#include <stdio.h>

int xbox_HostOpenSdlSync(void)
{
    char path[XBOX_HOST_OPEN_PATH_MAX], url[3 * XBOX_HOST_OPEN_PATH_MAX + 8];
    if (!xbox_HostOpenTake(path, sizeof path))
        return 0;
    if (!xbox_HostOpenFileUrl(path, url, sizeof url)) {
        fprintf(stderr, "[INPUT] opening %s failed: it cannot be made a file URL\n", path);
        return 0;
    }
    if (!SDL_OpenURL(url)) {
        fprintf(stderr, "[INPUT] opening %s failed: %s\n", path, SDL_GetError());
        return 0;
    }
    return 1;
}
