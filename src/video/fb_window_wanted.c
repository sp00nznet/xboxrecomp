/*
 * fb_window_wanted.c - whether RECOMP_FB_WINDOW opens the GDI framebuffer
 * window (fb_present.c), as a plain function so every host builds and tests
 * it. See video_player.h.
 */
#include "video_player.h"

#if defined(_WIN32)
#include <string.h>
#define fbw_strcasecmp _stricmp
#else
#include <strings.h>
#define fbw_strcasecmp strcasecmp
#endif

int fb_window_wanted(const char *backend, int fb_window)
{
    if (!fb_window)
        return 0;
    /* The D3D11 backend presents in a window of its own
     * (nv2a_pb_d3d11_register_from_env takes "d3d11" in any case), so a GDI
     * window beside it only shows a second copy of the frame. Every other
     * backend keeps it: on Windows "metal" falls back to the CPU rasteriser,
     * whose only window this is, and "null" (the walker alone, which draws
     * nothing) has no window of its own either. */
    return !(backend && !fbw_strcasecmp(backend, "d3d11"));
}
