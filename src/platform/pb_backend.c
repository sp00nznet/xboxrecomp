/*
 * pb_backend.c - the executor backend for this run (pb_backend.h).
 */
#include "pb_backend.h"

#include <stdio.h>

#include "recomp_env.h"

#if defined(_WIN32)
#include <string.h>
#define pbb_strcasecmp _stricmp
#else
#include <strings.h>
#define pbb_strcasecmp strcasecmp
#endif

/* Set on the main thread before the guest (and so the ack thread that
 * reads it) starts; never cleared. */
static volatile int s_demoted;

const char *pb_backend_default(void)
{
#ifdef XBOXRECOMP_DXVK_NATIVE
    return "d3d11";
#else
    return "cpu";
#endif
}

const char *pb_backend_resolve(const char *env, const char *dflt, int demoted)
{
    const char *b = env && *env ? env : dflt;
    if (demoted && b && !pbb_strcasecmp(b, "d3d11"))
        return "cpu";
    return b;
}

const char *pb_backend(void)
{
    return pb_backend_resolve(recomp_env(RENV_PB_BACKEND), pb_backend_default(),
                              s_demoted);
}

int pb_backend_is(const char *name)
{
    const char *b = pb_backend();
    return b && name && !pbb_strcasecmp(b, name);
}

void pb_backend_demote(const char *why)
{
    if (s_demoted || !pb_backend_is("d3d11"))
        return;
    s_demoted = 1;
    fprintf(stderr, "[GPU] d3d11 backend has no window (%s): drawing with the"
                    " CPU rasteriser instead\n", why ? why : "unknown");
}
