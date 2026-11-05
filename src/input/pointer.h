/**
 * xbox_input pointer -- where the host cursor is, for the enhancements
 * menu's mouse (overlay-revamp D3).
 *
 * The mouse in mouse.c is relative: counts that become a look stick. A
 * menu needs the cursor's place instead, so the window side also keeps a
 * snapshot of the absolute pointer:
 *   - x, y in the window's pixels: Win32 client coordinates, and SDL
 *     window coordinates times the window's pixel density. The overlay's
 *     placement is in the output's pixels, which differ when the output is
 *     scaled into the window (present.surface=internal):
 *     nv2a_overlay_to_output maps between them;
 *   - the held buttons (XBOX_POINTER_*);
 *   - the wheel steps since the last take (positive: away from the
 *     player);
 *   - the buttons pressed since the last take, with where each was
 *     pressed, so a press and its release that both land between two
 *     takes still count (a trackpad's tap, a touch screen, a renderer
 *     that pumps the window only at flips);
 *   - seq, which moves on every pointer event, so a reader can tell a
 *     still pointer from one that came back to the same place.
 *
 * One writer, the window's thread (one window is up at a time), stores
 * under a seqlock; the reader, the menu on the guest's thread, takes one
 * consistent snapshot per step. It is fed always, a few stores a message,
 * and read only while the menu is open.
 *
 * No platform header here: the Win32 decode takes the raw message numbers
 * and the SDL writer takes plain floats, so tests/input_pointer feeds both
 * formats on every host.
 */
#ifndef XBOX_INPUT_POINTER_H
#define XBOX_INPUT_POINTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XBOX_POINTER_LEFT = 1u << 0,
    XBOX_POINTER_RIGHT = 1u << 1,
    XBOX_POINTER_MIDDLE = 1u << 2,
};
#define XBOX_POINTER_BUTTONS 3   /* press[] is indexed by the bit's number */

struct xbox_pointer {
    int x, y;           /* window pixels; -1, -1 before the first event */
    unsigned buttons;   /* XBOX_POINTER_* held */
    int wheel;          /* steps since the last take */
    uint32_t seq;       /* events since the start; 0 for none */
    unsigned pressed;   /* XBOX_POINTER_* pressed since the last take, held or let go */
    struct { int x, y; } press[XBOX_POINTER_BUTTONS];   /* each one's latest press */
};

/* ---- the writer: the window's thread ----------------------------------- */

void xbox_InputPointerMove(int x, int y);
/* A button (XBOX_POINTER_*) pressed or let go at x, y. */
void xbox_InputPointerButton(unsigned button, int down, int x, int y);
void xbox_InputPointerWheel(int steps);
/* The window lost focus: no button stays held. */
void xbox_InputPointerFocusLost(void);

/* One Win32 message (WM_MOUSEMOVE, the button messages, WM_MOUSEWHEEL,
 * WM_KILLFOCUS), decoded from its numbers; others are ignored. Returns 1
 * when it was a pointer message. WM_MOUSEMOVE's MK_* flags set the held
 * buttons, so a button let go outside the client area, whose up message
 * went elsewhere, is not held at the next move inside. mouse.c's Win32 observer calls it, so both
 * game windows feed it. */
int  xbox_InputPointerWin32Msg(unsigned msg, uintptr_t w, intptr_t l);

/* SDL's formats: window coordinates in points as floats, times the
 * window's pixel density (SDL_GetWindowPixelDensity), and the wheel in
 * fractions of a step, whose rest carries into the next event. */
void xbox_InputPointerSdlMove(float x, float y, float density);
void xbox_InputPointerSdlButton(unsigned button, int down, float x, float y, float density);
void xbox_InputPointerSdlWheel(float y);

/* ---- the reader: the menu ---------------------------------------------- */

/* The snapshot, with the wheel steps and the presses gathered since the
 * last take (taken: the next take starts from none). One reader. */
void xbox_InputPointerTake(struct xbox_pointer *out);

/* Back to the start (tests). */
void xbox_InputPointerReset(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_POINTER_H */
