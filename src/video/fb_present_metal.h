/* fb_present_metal.m: the CAMetalLayer present of the SDL window (macOS).
 * Called by fb_present_sdl.c only; see that file and fb_present_metal.m. */
#ifndef FB_PRESENT_METAL_H
#define FB_PRESENT_METAL_H

#include <stddef.h>
#include <stdint.h>
#include <SDL.h>

/* Device, queue, pipeline and the window's layer; 0 with a reason in err. */
int   fbm_create(SDL_Window *win, int vsync, int shots, char *err, size_t errn);
void  fbm_destroy(void);
void  fbm_get(void **device, void **queue);
/* Slot k (0..2) at w x h: its id<MTLTexture>, or NULL. */
void *fbm_slot(int k, uint32_t w, uint32_t h);
int   fbm_upload(int k, const uint8_t *src, uint32_t w, uint32_t h, uint32_t pitch,
                 uint32_t bpp);
int   fbm_present(SDL_Window *win, int k, uint32_t fw, uint32_t fh, int filter,
                  const char *shot_path, uint32_t flip);

#endif
