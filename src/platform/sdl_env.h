/* SDL's own driver variables, under both spellings.
 *
 * SDL3 reads SDL_AUDIO_DRIVER and SDL_VIDEO_DRIVER; SDL2 read SDL_AUDIODRIVER
 * and SDL_VIDEODRIVER. Launch scripts, golden runs and players' habits still
 * spell the old names, and a headless run that loses SDL_AUDIODRIVER=dummy
 * would play sound. So every SDL init site calls this first: an old name is
 * copied to the new one when the new one is unset, and a new name already set
 * wins.
 *
 * These are SDL's variables, not recomp_env rows, so this goes through SDL's
 * environment API (which SDL itself reads), never libc getenv. The _unsafe
 * setter is fine here: the first call is xbox_HostWindowMain's, on the main
 * thread before SDL_Init. The later ones (the APU thread's xa2_init, the pad
 * poll thread) find the new names already set and write nothing. */
#ifndef XBOX_SDL_ENV_H
#define XBOX_SDL_ENV_H

#include <SDL3/SDL.h>

static inline void xbox_sdl_env_compat(void)
{
    static const char *const names[][2] = {
        { "SDL_AUDIODRIVER", "SDL_AUDIO_DRIVER" },
        { "SDL_VIDEODRIVER", "SDL_VIDEO_DRIVER" },
    };
    size_t i;

    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char *old = SDL_getenv(names[i][0]);
        if (old && *old)
            SDL_setenv_unsafe(names[i][1], old, 0);
    }
}

#endif
