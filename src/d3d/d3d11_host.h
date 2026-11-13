/**
 * The D3D11 backend's host: what nv2a_pb_d3d11.c needs from the platform
 * around the device, kept out of the backend so one backend serves two hosts.
 *
 *   d3d11_host_win32.c  Windows (and Proton): the backend's own Win32 window,
 *                       pumped on the NV2A ack thread, QPC clocks and
 *                       d3dcompiler_47.
 *   d3d11_host_sdl.c    Linux over dxvk-native: the SDL presenter's window
 *                       (fb_present_sdl.c), which keeps the event loop, input,
 *                       title and fullscreen on the main thread.
 *
 * The calls are made on the ack thread, the backend's one thread. The header
 * names no Windows type: a window is a void * (an HWND, or an SDL_Window *
 * that dxvk-native's SDL3 WSI takes in its place).
 */
#ifndef D3D11_HOST_H
#define D3D11_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- clocks ------------------------------------------------------------- */

/* A monotonic clock in ms, sub-ms resolution: shader compile timing and the
 * present's own timing. */
double d3d11_host_now_ms(void);
/* The same clock in whole µs, for the occlusion settle (nv2a_occ_ops). */
uint64_t d3d11_host_now_us(void);
/* A monotonic ms tick, the overlay's clock (nv2a_overlay_poll). */
uint64_t d3d11_host_ticks_ms(void);

/* ---- thread ----------------------------------------------------------- */

/* Gives the rest of the time slice to another ready thread, if any. */
void d3d11_host_yield(void);
/* A spin-wait hint to the CPU (pause), for the occlusion settle's loop on
 * the fence: it reads, never sleeps, so the core keeps its clock. */
void d3d11_host_cpu_relax(void);

/* ---- HLSL compiler ---------------------------------------------------- */

/* ID3DBlob, the shape the backend keeps its bytecode in; the host's
 * D3D headers define it (mingw-w64's on Windows, dxvk-native's on Linux). */
struct ID3D10Blob;

/* Finds the compiler once, at the device's creation. 0: none (logged), and
 * the backend draws nothing. */
int d3d11_host_compiler_load(void);
/* The compiler's identity for the DXBC cache key (shader_cache.h), written
 * to out: a different compiler must give a different string. 0 when it
 * cannot be told, and the cache stays off. */
int d3d11_host_compiler_id(char *out, size_t n);
/* D3DCompile's contract: HLSL source to DXBC in *code, messages (may be
 * NULL) in *err, an HRESULT (negative: failed). flags1 and flags2 are
 * D3DCompile's. */
int32_t d3d11_host_compile(const void *src, size_t len, const char *name,
                           const char *entry, const char *target,
                           unsigned flags1, unsigned flags2,
                           struct ID3D10Blob **code, struct ID3D10Blob **err);
/* An empty blob of n bytes, for a cache hit. 1 when made. */
int d3d11_host_blob(size_t n, struct ID3D10Blob **out);

/* ---- window ----------------------------------------------------------- */

/* The window the swap chain presents to, with the game's title. popup: a
 * borderless w x h window at the screen's origin; else a framed window
 * whose client area is w x h. 0: none (logged). */
int d3d11_host_window_open(int popup, unsigned w, unsigned h);
/* The swap chain's OutputWindow: an HWND, or the SDL_Window * in its place. */
void *d3d11_host_window(void);
/* The swap chain is made on the window (IDXGISwapChain *). */
void d3d11_host_swap_chain_made(void *swap);
/* The device is up: a quit asked for off this thread may now close the
 * window (recomp_quit_set). */
void d3d11_host_ready(void);
/* Once a flip: the window's events, input and the timed quit. */
void d3d11_host_pump(void);
/* Around the swap chain's Present. On Linux begin never returns once the
 * presenter's window is closing (the thread parks, so exit() tears DXVK
 * down with no call in flight; fb_present_swap.h) and end counts the
 * present; on Windows both do nothing. */
void d3d11_host_present_begin(void);
void d3d11_host_present_end(void);
/* Around every backend callback (nv2a_pb_d3d11.c's vtable): the same
 * park as present_begin, so exit() never runs under an upload, draw, Map or
 * occlusion settle either. Nests with present_begin/end; Windows: no-ops. */
void d3d11_host_work_begin(void);
void d3d11_host_work_end(void);
/* Once a flip: the title bar, when its text is due a change. */
void d3d11_host_title(void);
/* The window's client size as last reported (0 x 0: none yet). */
void d3d11_host_client_size(unsigned *w, unsigned *h);
/* The client area as the window system has it now, for a log line. */
void d3d11_host_client_rect(unsigned *w, unsigned *h);
/* The primary screen's size (0 x 0: unknown). */
void d3d11_host_screen_size(unsigned *w, unsigned *h);
/* The monitor the window is on, as a rectangle (the screen's when unknown). */
void d3d11_host_monitor_rect(int *x, int *y, unsigned *w, unsigned *h);
/* Resizes the client area to w x h, framed or borderless, in place. */
void d3d11_host_set_client_size(unsigned w, unsigned h, int framed);
/* Borderless over x, y, w x h, on top (present.shader's output surface). */
void d3d11_host_popup(int x, int y, unsigned w, unsigned h);
/* Back to a framed window at its size and place (the popup's undo). */
void d3d11_host_framed(void);
/* The window as it is (style, placement, client size), kept for
 * d3d11_host_hold_restore: the output surface's hold puts back
 * exactly what it found, not a size worked out again from the frame. */
void d3d11_host_hold_save(void);
/* Back to what d3d11_host_hold_save kept. restyle 0 (gamescope, which
 * shows only the client area): the style stays as it is, only the client
 * size comes back, so no frame change is made. */
void d3d11_host_hold_restore(int restyle);
/* present.fullscreen switched live: borderless over the monitor (internal:
 * at fw x fh), or framed back where it was (internal, or never framed: a
 * client area of fw x fh or dw x dh). */
void d3d11_host_fullscreen(int full, int internal, unsigned fw, unsigned fh,
                           unsigned dw, unsigned dh);

/* ---- files ------------------------------------------------------------ */

/* Makes a directory; one that exists is fine. */
void d3d11_host_mkdir(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* D3D11_HOST_H */
