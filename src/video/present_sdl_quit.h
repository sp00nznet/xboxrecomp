/*
 * What the SDL presenter (fb_present_sdl.c) shuts down when its window
 * closes: video only. The guest, the USB thread and the APU frame thread keep
 * running until exit(), and the last two still use SDL. SDL_Quit would close
 * the open game controllers under the USB thread, and free the audio stream
 * under the frame thread, which queries it every period (apu_sdl.c); exit()
 * reclaims both. tests/apu_sdl_quit runs this teardown against a thread that
 * feeds and queries the stream.
 */
#ifndef XBOX_PRESENT_SDL_QUIT_H
#define XBOX_PRESENT_SDL_QUIT_H

#include <SDL3/SDL.h>

#define XBOX_PRESENT_SDL_QUIT SDL_INIT_VIDEO

#endif
