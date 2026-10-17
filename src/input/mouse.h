/**
 * xbox_input mouse -- the host mouse as part of the port-0 pad.
 *
 * The mouse joins the keyboard (keyboard.h) as one source on port 0. Its
 * motion is a stick (the right one, the camera, by default) and its buttons
 * and wheel are inputs a binding names like keys (input_names.h).
 *
 * Writers: the window's thread, through the event calls below (the SDL loop
 * via mouse_sdl.c, the Win32 windows via xbox_InputMouseWin32Msg). Reader:
 * the game's poll, through the composer in keyboard.c. The accumulators are
 * exchanged with atomics and the button bytes are single loads, so neither
 * side takes a lock.
 *
 * Capture is a small state machine (focus, the release key, a suspend from
 * the enhancements menu, the capture mode). The window thread mirrors its
 * answer, xbox_InputMouseCaptured(), onto the host cursor: SDL's relative
 * mode, or the Win32 primitives in this file's _WIN32 half.
 *
 * No platform header here, so tests/input_bindings drives the model and the
 * state machine on every host.
 */
#ifndef XBOX_INPUT_MOUSE_H
#define XBOX_INPUT_MOUSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { XBOX_MOUSE_LOOK_RIGHT, XBOX_MOUSE_LOOK_LEFT, XBOX_MOUSE_LOOK_NONE };
enum { XBOX_MOUSE_CAPTURE_FOCUS, XBOX_MOUSE_CAPTURE_CLICK, XBOX_MOUSE_CAPTURE_NEVER };

typedef struct xbox_mouse_opts {
    int    look;            /* XBOX_MOUSE_LOOK_*: the stick motion drives */
    double sensitivity;     /* 1.0: 40 counts in one poll is full deflection */
    int    invert_x, invert_y;
    double min_deflection;  /* any motion deflects at least this much */
    int    capture;         /* XBOX_MOUSE_CAPTURE_* */
    int    release_key;     /* key code that toggles the capture; 0: none */
    int    wheel_pulse_ms;  /* how long a wheel notch holds its control */
} xbox_mouse_opts;

/* The code defaults (right stick, 1.0, no invert, 0.45, focus, F9, 100). */
void xbox_InputMouseDefaultOpts(xbox_mouse_opts *o);
/* Set the options; NULL restores the code defaults. */
void xbox_InputMouseOpts(const xbox_mouse_opts *o);
void xbox_InputMouseGetOpts(xbox_mouse_opts *o);

/* Whether the mouse is a source: -1 goes back to RECOMP_MOUSE. */
void xbox_InputMouseEnable(int on);
int  xbox_InputMouseOn(void);

/* ---- the window's side ------------------------------------------------- */
/* Relative counts; mouse forward is negative dy. Ignored while released. */
void xbox_InputMouseMotion(int dx, int dy);
/* Button n (1 left, 2 right, 3 middle, 4, 5). A press while released
 * drives nothing; in "click" mode it captures and is consumed. A release
 * always applies. */
void xbox_InputMouseButton(int n, int down);
/* Wheel notches, positive away from the player (up). */
void xbox_InputMouseWheel(int notches);
/* Focus lost (1, also hide and minimise) or gained (0). */
void xbox_InputMouseFocus(int lost);
/* The next press is the click that activated the window (Win32
 * WM_MOUSEACTIVATE): in "focus" mode it drives nothing. */
void xbox_InputMouseActivateClick(void);
/* The same rule for hosts that say only "focus gained" and then deliver the
 * click (SDL on X11 and Wayland, Wine when the window manager activates the
 * window first): focus gained at now_ms, and a press at now_ms that comes
 * within XBOX_MOUSE_FOCUS_CLICK_MS of it is taken as the focusing click.
 * Only the first press after the focus is looked at. Times are the host's
 * own millisecond clock (an SDL event timestamp, GetTickCount); they wrap. */
#define XBOX_MOUSE_FOCUS_CLICK_MS 250u
void xbox_InputMouseFocusGainedAt(uint32_t now_ms);
void xbox_InputMouseButtonAt(int n, int down, uint32_t now_ms);
/* The window closes: release for good. */
void xbox_InputMouseClose(void);
/* A key edge (keyboard.c calls this for every one): the release key toggles
 * the capture. Returns 1 when vk is the release key. */
int  xbox_InputMouseKey(int vk, int down);
/* 1 while something (the enhancements menu) needs the cursor free; 0 gives
 * it back, captured again if the window still has focus. */
void xbox_InputMouseSuspend(int on);
/* Whether the cursor is captured now: the window mirrors this. */
int  xbox_InputMouseCaptured(void);
/* The injector (kbm_script) captures without a window; with one, the
 * forced capture still follows its focus. */
void xbox_InputMouseForceCapture(int on);

/* ---- the poll's side --------------------------------------------------- */
/* Once per poll, before reading: advances the wheel pulses. */
void xbox_InputMouseTick(uint64_t now_ms, int have_clock);
/* Whether mouse input code (XBOX_MOUSE_*) is held this poll. */
int  xbox_InputMouseHeld(int code);
/* Take the motion since the last poll through the model: *x, *y are stick
 * values for the opts.look stick (0, 0 when none or at rest). Returns the
 * stick (XBOX_MOUSE_LOOK_*). */
int  xbox_InputMouseLook(int *x, int *y);
/* The model alone: counts on one axis to a stick value. */
int  xbox_InputMouseModel(int counts, double sensitivity, double min_deflection);

/* Everything back to start: released, no focus, nothing held (tests). */
void xbox_InputMouseReset(void);

#ifdef _WIN32
/* Both Win32 windows (fb_present.c, nv2a_pb_d3d11.c) pass every message
 * here first; it only observes, so the window procedure goes on as before.
 * Raw input (WM_INPUT) is the motion while captured; WM_MOUSEMOVE against
 * the centre stands in when raw input stays silent. Arguments are HWND,
 * UINT, WPARAM and LPARAM. */
void xbox_InputMouseWin32Msg(void *hwnd, unsigned msg, uintptr_t w, intptr_t l);
/* Once per pump, on the window's thread: applies the capture state to the
 * host cursor (hidden, clipped, centred, raw input registered) or undoes
 * it. */
void xbox_InputMouseWin32Sync(void *hwnd);
/* The window closes or the process ends: release for good and undo the
 * capture now (cursor shown, clip dropped), since no pump follows. Any
 * thread; the window procedures call it before ExitProcess, and the first
 * capture registers it as an exit hook and with atexit. */
void xbox_InputMouseWin32Release(void *hwnd);
#endif

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_MOUSE_H */
