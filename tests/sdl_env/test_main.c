/* The driver aliases (sdl_env.h): the SDL2 spelling fills an unset SDL3
 * name, and never overrides one already set. No SDL_Init: the aliases must
 * hold before it. */
#include <stdio.h>
#include <string.h>
#include "sdl_env.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int is(const char *name, const char *want)
{
    const char *v = SDL_getenv(name);
    return want ? v && !strcmp(v, want) : !v;
}

int main(void)
{
    SDL_unsetenv_unsafe("SDL_AUDIO_DRIVER");
    SDL_unsetenv_unsafe("SDL_AUDIODRIVER");
    SDL_unsetenv_unsafe("SDL_VIDEO_DRIVER");
    SDL_unsetenv_unsafe("SDL_VIDEODRIVER");

    /* Neither set: nothing appears. */
    xbox_sdl_env_compat();
    CHECK(is("SDL_AUDIO_DRIVER", NULL));
    CHECK(is("SDL_VIDEO_DRIVER", NULL));

    /* Old name only: copied. */
    SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
    SDL_setenv_unsafe("SDL_VIDEODRIVER", "offscreen", 1);
    xbox_sdl_env_compat();
    CHECK(is("SDL_AUDIO_DRIVER", "dummy"));
    CHECK(is("SDL_VIDEO_DRIVER", "offscreen"));

    /* Both set: the SDL3 name wins, the old one is left alone. */
    SDL_setenv_unsafe("SDL_AUDIO_DRIVER", "coreaudio", 1);
    SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
    xbox_sdl_env_compat();
    CHECK(is("SDL_AUDIO_DRIVER", "coreaudio"));
    CHECK(is("SDL_AUDIODRIVER", "dummy"));

    /* An empty old name is no request. */
    SDL_unsetenv_unsafe("SDL_VIDEO_DRIVER");
    SDL_setenv_unsafe("SDL_VIDEODRIVER", "", 1);
    xbox_sdl_env_compat();
    CHECK(is("SDL_VIDEO_DRIVER", NULL));

    printf("sdl_env: all checks passed\n");
    return 0;
}
