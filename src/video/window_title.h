/*
 * window_title.h - the host window's title bar, shared by every window that
 * shows the game: the Win32 framebuffer window (fb_present.c), the D3D11
 * backend's own window (nv2a_pb_d3d11.c) and the SDL window the CPU and
 * Metal paths present through (fb_present_sdl.c).
 *
 * The title bar is the game's name and nothing else. RECOMP_TRACE=title
 * adds the runtime counters, "<name> | FPS: n | draws: n", refreshed once a
 * second. Without it a window's title is written when its name changes and
 * at no other time: a title rewrite is a round trip to the window manager
 * (under Wine, an X property change), so there is no reason to make one a
 * second for text that does not change.
 *
 * Pure: no window system, no clock. The callers pass the time and counts,
 * so the rules can be tested on their own (tests/window_title).
 */
#ifndef XBOX_WINDOW_TITLE_H
#define XBOX_WINDOW_TITLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One window's last title write. Zero it to start. */
struct xbox_title_clock {
    uint64_t t_ms;       /* when the last write was made */
    uint64_t flips;      /* the flip count then */
    unsigned name_gen;   /* the name's generation then */
    int      written;    /* a write has been made */
};

/* Whether the window's title is due for a write, and record it as made when
 * it is. With stats, every second (the first call at once, fps 0); without,
 * only when name_gen differs from the last write's (the first call at
 * once). *fps is the flip rate since the last write, set when due. */
int xbox_title_due(struct xbox_title_clock *c, int stats, uint64_t now_ms,
                   uint64_t flips, unsigned name_gen, double *fps);

/* The title text, NUL-terminated in out (n bytes). Without stats, the name;
 * with them "<name> | FPS: n.n", then " | draws: n" when have_draws. */
void xbox_title_format(char *out, size_t n, const char *name, int stats,
                       double fps, int have_draws, uint32_t draws);

/* An XBE certificate name (UTF-16, max_chars at most, stops at a NUL) as
 * UTF-8 in out (n bytes, always terminated; a character that does not fit
 * is left out whole). No surrogate pairs: a certificate name is 40 BMP
 * characters. Returns the bytes written, not counting the NUL. */
size_t xbox_title_utf16_to_utf8(char *out, size_t n, const uint16_t *name,
                                int max_chars);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_WINDOW_TITLE_H */
