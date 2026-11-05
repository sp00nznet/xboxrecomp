/* fb_present_metal.m: the CAMetalLayer present of the SDL window (macOS).
 * Called by fb_present_sdl.c only; see that file and fb_present_metal.m. */
#ifndef FB_PRESENT_METAL_H
#define FB_PRESENT_METAL_H

#include <stddef.h>
#include <stdint.h>
#include <SDL3/SDL.h>

/* Device, queue, pipeline and the window's layer; 0 with a reason in err. */
int   fbm_create(SDL_Window *win, int vsync, int shots, char *err, size_t errn);
void  fbm_destroy(void);
void  fbm_get(void **device, void **queue);
/* Slot k (0..2) at w x h: its id<MTLTexture>, or NULL. */
void *fbm_slot(int k, uint32_t w, uint32_t h);
int   fbm_upload(int k, const uint8_t *src, uint32_t w, uint32_t h, uint32_t pitch,
                 uint32_t bpp);
/* layers: the overlay's visible lists (nv2a_overlay.h), drawn over the
 * frame in the same pass; NULL and 0 when none is visible. */
struct nv2a_ovl_list;
int   fbm_present(SDL_Window *win, int k, uint32_t fw, uint32_t fh, unsigned n,
                  uint32_t pad, int filter, const char *shot_path, uint32_t flip,
                  const struct nv2a_ovl_list *const *layers, int nlayers);

/* The window's NSWindow level: above the menu bar and the Dock
 * (kCGMainMenuWindowLevel + 1, where SDL puts a fullscreen window without
 * Spaces) when top, else the normal level. Any SDL window, on either
 * present path. 1 when the level changed. */
int   fbm_window_level(SDL_Window *win, int top);

#endif
