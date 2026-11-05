/* When RECOMP_FB_WINDOW opens the GDI framebuffer window (video_player.h):
 * the CPU paths keep it, the D3D11 backend in any case does not, and without
 * the key there is none. */
#include <stdio.h>
#include "video_player.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void)
{
    /* With RECOMP_FB_WINDOW: a window for every backend but d3d11. "metal"
     * falls back to the CPU rasteriser on Windows, and "null" (walker only,
     * draws nothing) has no window of its own, so neither loses this one. */
    CHECK(fb_window_wanted(NULL, 1));
    CHECK(fb_window_wanted("", 1));
    CHECK(fb_window_wanted("cpu", 1));
    CHECK(fb_window_wanted("null", 1));
    CHECK(fb_window_wanted("metal", 1));
    CHECK(!fb_window_wanted("d3d11", 1));
    CHECK(!fb_window_wanted("D3D11", 1));
    CHECK(fb_window_wanted("d3d11x", 1));   /* not the backend's name */

    /* Without it: never. */
    CHECK(!fb_window_wanted(NULL, 0));
    CHECK(!fb_window_wanted("cpu", 0));
    CHECK(!fb_window_wanted("metal", 0));
    CHECK(!fb_window_wanted("d3d11", 0));

    printf("fb_window_wanted: ok\n");
    return 0;
}
