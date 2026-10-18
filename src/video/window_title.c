/*
 * window_title.c - the title bar's text and when to write it. See
 * window_title.h.
 */
#include "window_title.h"

#include <stdio.h>

int xbox_title_due(struct xbox_title_clock *c, int stats, uint64_t now_ms,
                   uint64_t flips, unsigned name_gen, double *fps)
{
    if (stats) {
        if (c->written && now_ms - c->t_ms < 1000)
            return 0;
        *fps = c->written ? (double)(flips - c->flips) * 1000.0
                            / (double)(now_ms - c->t_ms) : 0.0;
    } else {
        if (c->written && c->name_gen == name_gen)
            return 0;
        *fps = 0.0;
    }
    c->t_ms = now_ms;
    c->flips = flips;
    c->name_gen = name_gen;
    c->written = 1;
    return 1;
}

void xbox_title_format(char *out, size_t n, const char *name, int stats,
                       double fps, int have_draws, uint32_t draws)
{
    if (!n)
        return;
    if (!stats)
        snprintf(out, n, "%s", name);
    else if (have_draws)
        snprintf(out, n, "%s | FPS: %.1f | draws: %u", name, fps,
                 (unsigned)draws);
    else
        snprintf(out, n, "%s | FPS: %.1f", name, fps);
}

size_t xbox_title_utf16_to_utf8(char *out, size_t n, const uint16_t *name,
                                int max_chars)
{
    size_t o = 0;
    int i;

    if (!n)
        return 0;
    for (i = 0; i < max_chars && name[i]; i++) {
        unsigned c = name[i];
        if (c < 0x80) {
            if (o + 1 >= n) break;
            out[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= n) break;
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= n) break;
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
    return o;
}
