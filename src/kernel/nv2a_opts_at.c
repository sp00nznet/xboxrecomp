/*
 * nv2a_opts_at.c - see nv2a_opts_at.h.
 */
#include "nv2a_opts_at.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int word_index(const char *v, size_t n, const char *const *names)
{
    int i;
    for (i = 0; names[i]; i++)
        if (strlen(names[i]) == n && !strncmp(v, names[i], n))
            return i;
    return -1;
}

int nv2a_opts_at_parse(const char *p, struct nv2a_opts_at *out)
{
    static const char *const filters[] = { "nearest", "linear", "integer", NULL };
    static const char *const movies[] = { "black", "fit", "fill", NULL };
    static const char *const pacings[] = { "spin", "sleep", NULL };
    int n_out = 0;

    while (p && *p) {
        const char *end = strchr(p, ','), *eq, *colon, *v;
        size_t len = end ? (size_t)(end - p) : strlen(p), kn, vn;
        struct nv2a_opts_at e;
        char *num_end;
        int ok = 1, w = 0;

        memset(&e, 0, sizeof e);
        e.pacing = -1;
        colon = memchr(p, ':', len);
        eq = colon ? memchr(colon, '=', len - (size_t)(colon - p)) : NULL;
        if (!colon || !eq || n_out == NV2A_OPTS_AT_MAX)
            ok = 0;
        if (ok) {
            e.flip = (uint32_t)strtoul(p, &num_end, 10);
            kn = (size_t)(eq - colon - 1);
            v = eq + 1;
            vn = len - (size_t)(v - p);
            /* a flip is digits only: strtoul would take "-1" or " 5" */
            ok = num_end == colon && p < colon && p[0] >= '0' && p[0] <= '9';
#define OPTS_AT_KEY(k) (kn == sizeof(k) - 1 && !strncmp(colon + 1, k, kn))
            if (!ok) {
            } else if (OPTS_AT_KEY("filter")) {
                ok = (w = word_index(v, vn, filters)) >= 0;
                e.o.present_filter = w;
                e.mask = NV2A_OPT_FILTER;
            } else if (OPTS_AT_KEY("movie")) {
                ok = (w = word_index(v, vn, movies)) >= 0;
                e.o.movie_mode = w;
                e.mask = NV2A_OPT_MOVIE;
            } else if (OPTS_AT_KEY("pacing")) {
                ok = (e.pacing = word_index(v, vn, pacings)) >= 0;
            } else {
                unsigned long n = strtoul(v, &num_end, 10);
                ok = num_end == v + vn && vn && v[0] >= '0' && v[0] <= '9';
                if (OPTS_AT_KEY("scale")) {
                    e.o.render_scale = (unsigned)n;
                    e.mask = NV2A_OPT_SCALE;
                } else if (OPTS_AT_KEY("fullscreen")) {
                    e.o.fullscreen = n != 0;
                    e.mask = NV2A_OPT_FULLSCREEN;
                } else if (OPTS_AT_KEY("aniso")) {
                    e.o.aniso = (unsigned)n;
                    e.mask = NV2A_OPT_ANISO;
                } else if (OPTS_AT_KEY("overscan")) {
                    e.o.overscan = (unsigned)n;
                    e.mask = NV2A_OPT_OVERSCAN;
                } else if (OPTS_AT_KEY("snap_2d")) {
                    e.o.snap_2d = n != 0;
                    e.mask = NV2A_OPT_SNAP2D;
                } else {
                    ok = 0;
                }
            }
#undef OPTS_AT_KEY
        }
        if (ok)
            out[n_out++] = e;
        else
            fprintf(stderr, "[PB] opts_at: entry '%.*s' skipped (flip:key=value%s)\n",
                    (int)len, p, n_out == NV2A_OPTS_AT_MAX ? "; 32 at most" : "");
        p = end ? end + 1 : NULL;
    }
    return n_out;
}
