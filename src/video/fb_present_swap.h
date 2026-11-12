/**
 * The SDL presenter's window lent to a backend's own swap chain: the D3D11
 * backend over dxvk-native on Linux (d3d11_host_sdl.c), whose DXGI swap
 * chain takes the SDL_Window * in place of an HWND (DXVK_WSI_DRIVER=SDL3).
 *
 * The presenter (fb_present_sdl.c) keeps what the main thread must own: the
 * window, its one event loop, input, the title and present.fullscreen. The
 * backend draws and presents on the NV2A ack thread, and the presenter's loop
 * then neither renders nor reads the overlay (the backend's present is the
 * overlay's one reader, as on Windows).
 *
 * Built only with XBOXRECOMP_DXVK_NATIVE (the dxvk-native build) and chosen
 * with RECOMP_PB_BACKEND=d3d11. Elsewhere the window is never lent: every
 * call below answers "none" and the presenter runs exactly as before.
 */
#ifndef FB_PRESENT_SWAP_H
#define FB_PRESENT_SWAP_H

#ifdef __cplusplus
extern "C" {
#endif

/* The SDL_Window * the presenter opened for a swap chain (with
 * SDL_WINDOW_VULKAN, no SDL_Renderer on it), or NULL when it did not. Set
 * before the guest starts and never changed, so any thread may read it. */
void *xbox_FramebufferWindowSwap(void);
/* The window's drawable in pixels, as the main thread last saw it (window
 * created, resized, moved to a display of another density); 0 x 0: none. */
void xbox_FramebufferWindowSwapSize(unsigned *w, unsigned *h);
/* The window's display, in pixels, when the window was opened; 0 x 0:
 * unknown. */
void xbox_FramebufferWindowSwapDisplay(unsigned *w, unsigned *h);
/* Asks the main thread to make the window's client area w x h pixels
 * (present.surface=internal); a fullscreen window keeps its size. */
void xbox_FramebufferWindowSwapResize(unsigned w, unsigned h);
/* 1 when present.surface=internal: the client area is the frame's size and
 * the backend sizes it; otherwise the window keeps the presenter's size
 * (present.window_scale), as the backend's own window does on Windows. */
int xbox_FramebufferWindowSwapInternal(void);

#ifdef __cplusplus
}
#endif

#endif /* FB_PRESENT_SWAP_H */
