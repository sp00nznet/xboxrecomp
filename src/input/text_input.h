/**
 * xbox_input text -- typed characters for the enhancements menu's text
 * fields, as Unicode codepoints in the player's layout.
 *
 * The menu runs on the guest's thread; the SDL loop and the Win32 window
 * procs run on the window's thread. So:
 *   - the ring of typed codepoints has one producer (the window's thread,
 *     through the Push calls) and one consumer (the guest's thread, through
 *     Pop), and takes no lock;
 *   - Begin and End only *request* text input. They store the wanted state,
 *     the field's rect and a generation atomically; the window's thread
 *     reads the request (xbox_InputTextRequest) and applies it, as the
 *     mouse capture is applied (xbox_InputSdlTextSync in text_input_sdl.c).
 *     Win32 needs no host call: WM_CHAR arrives anyway, and the window
 *     procs read the gate (xbox_InputTextWin32Char).
 *
 * Control characters (below 0x20, and 0x7F) are dropped at the producer:
 * Tab, Enter, Esc, Backspace and the Ctrl+letter codes reach the menu as key
 * edges instead.
 *
 * No platform header here, so tests/input_text drives the ring on every
 * host.
 */
#ifndef XBOX_INPUT_TEXT_INPUT_H
#define XBOX_INPUT_TEXT_INPUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ring's size in codepoints; input past it is dropped and counted. */
#define XBOX_TEXT_RING 64

/* ---- the consumer: the guest's thread (the menu) ----------------------- */

/* Text input on, for a field at x, y, w, h in placement pixels (the frame's
 * rect in the window's pixels, nv2a_overlay_set_placement). Empties the
 * ring. A second Begin moves the field. */
void xbox_InputTextBegin(int x, int y, int w, int h);
/* Text input off; empties the ring. */
void xbox_InputTextEnd(void);
/* One codepoint as UTF-8 into utf8[n], NUL-terminated. Returns its length
 * in bytes, 0 when the ring is empty, -1 when n is too small (the codepoint
 * stays queued; 5 bytes always fit). */
int  xbox_InputTextPop(char *utf8, size_t n);

/* ---- the producer: the window's thread --------------------------------- */

/* 1 while text input is on: the gate the producers read. */
int  xbox_InputTextActive(void);
/* One codepoint. Returns 1 when queued, 0 when dropped (the gate is
 * closed, a control character, not a Unicode scalar, or the ring is full;
 * only the last is counted). */
int  xbox_InputTextPushCodepoint(uint32_t cp);
/* A NUL-terminated UTF-8 string (SDL_EVENT_TEXT_INPUT); ill-formed bytes
 * are skipped. Returns the codepoints queued. */
int  xbox_InputTextPushUtf8(const char *s);
/* One UTF-16 unit (WM_CHAR on a Unicode window): a high surrogate waits for
 * its low one, and a lone surrogate is dropped. Returns 1 when a codepoint
 * was queued. */
int  xbox_InputTextPushUtf16(uint16_t unit);
/* The window lost focus: forget a half-received character (a high
 * surrogate, a DBCS lead byte), so it never pairs with what comes after. */
void xbox_InputTextFocusLost(void);

/* ---- the request, read by the window's thread --------------------------- */

/* The current request: *on, and the field rect (x, y, w, h) when on, read
 * together (a reader never gets one request's rect with another's
 * generation). Returns its generation, which changes with each Begin and
 * End; a reader that saw the same generation last time has nothing to
 * apply. */
unsigned xbox_InputTextRequest(int *on, int rect[4]);
/* Codepoints dropped because the ring was full, since the start. */
unsigned xbox_InputTextDropped(void);

/* Back to the start state (tests). */
void xbox_InputTextReset(void);

#ifdef _WIN32
/* WM_CHAR from either game window (fb_present.c, nv2a_pb_d3d11.c), on the
 * window's thread. Returns 1 when the message was taken (text input is on),
 * 0 to pass it on to DefWindowProc. Both windows are ANSI today, so their
 * WM_CHAR carries the input language's code page and is converted here; a
 * Unicode window's carries UTF-16. */
int  xbox_InputTextWin32Char(void *hwnd, uintptr_t w, intptr_t l);
/* One ANSI WM_CHAR byte in code page cp, repeated `repeat` times once it
 * completes a character; a DBCS lead byte waits for its trail byte.
 * Returns the codepoints queued. Split out so the tests can name the code
 * page. */
int  xbox_InputTextWin32Ansi(unsigned cp, unsigned char b, int repeat);
#endif

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_TEXT_INPUT_H */
