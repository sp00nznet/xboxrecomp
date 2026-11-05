/*
 * nv2a_opts_at.h - the RECOMP_DEBUG=opts_at parser: live option requests
 * made at given flips, as the in-game menu makes them, so a live switch
 * can be checked and shot without the menu. nv2a_pb_exec.c makes the
 * requests; this unit only parses, so a ctest (tests/opts_at) covers it.
 *
 * Syntax: <flip>:<key>=<value>,...  Keys: scale, filter
 * (nearest|linear|integer), fullscreen, aniso, movie (black|fit|fill),
 * overscan, snap_2d (0|1), pacing (spin|sleep).
 */
#ifndef NV2A_OPTS_AT_H
#define NV2A_OPTS_AT_H

#include <stdint.h>

#include "nv2a_backend_common.h"

#define NV2A_OPTS_AT_MAX 32

struct nv2a_opts_at {
    uint32_t flip;
    unsigned mask;              /* NV2A_OPT_* of the field set in o; 0 for pacing */
    struct nv2a_host_opts o;
    int pacing;                 /* XBOX_PACING_* or -1 */
};

/* Parses spec (NULL or "" is no entries) into out[0..NV2A_OPTS_AT_MAX).
 * A bad entry, or one past the cap, is reported on stderr and skipped.
 * Returns the number of entries kept. */
int nv2a_opts_at_parse(const char *spec, struct nv2a_opts_at *out);

#endif
