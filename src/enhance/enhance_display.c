/*
 * enhance_display.c - the display probe behind overlay.ui_fit = display
 *: the primary display's size, the OS's UI scale for it, and
 * whether it is a Steam Deck's own panel (SteamDeck=1, or the Deck's
 * SMBIOS names at its 1280x800). The overlay calls it at its first atlas
 * bake (nv2a_overlay_set_display_probe): Win32 answers there; SDL waits
 * for the presenter's video (nv2a_overlay_display_ready), so it never
 * starts the video itself.
 */
#include "enhance_display.h"
#include "enhance.h"

#include "enhance_cfg.h"
#include "recomp_env.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <SDL3/SDL.h>
#if defined(__APPLE__)
#include <pthread.h>
#endif
#endif

static int probe(struct nv2a_ovl_display *d)
{
    const char *deck;
    memset(d, 0, sizeof *d);
#if defined(_WIN32)
    {
        /* Win32 has no SDL here: the window is GDI's (fb_present.c) and
         * the exe declares no DPI awareness (no manifest entry, no
         * SetProcessDpiAwareness*). So these are the virtualised metrics
         * the window is laid out in: 96 dpi and the scaled size under a
         * 175% desktop (3840x2160 reads 2194x1234), and DWM then stretches
         * the whole window by 1.75, the menu with it. The OS scale term is
         * 1 here; only a DPI-aware exe would read the real one, and the
         * window's own size would change with it. Cheap, so it runs at the
         * first bake. */
        HDC dc = GetDC(NULL);
        int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 0;
        if (dc)
            ReleaseDC(NULL, dc);
        d->w = (uint32_t)GetSystemMetrics(SM_CXSCREEN);
        d->h = (uint32_t)GetSystemMetrics(SM_CYSCREEN);
        d->scale = dpi > 0 ? (float)dpi / 96.0f : 0.0f;
    }
#else
    {
        SDL_DisplayID id;
        const SDL_DisplayMode *m;
        /* The first bake comes at enhance init, before the presenter's
         * SDL_Init: starting the video here would bring it up twice (the
         * window system's connection, Cocoa's start). Not yet, then; the
         * presenter calls nv2a_overlay_display_ready once its video is up,
         * and a headless run never asks. */
        if (!SDL_WasInit(SDL_INIT_VIDEO))
            return -1;
#if defined(__APPLE__)
        /* Cocoa's queries are the main thread's. */
        if (!pthread_main_np())
            return -1;
#endif
        id = SDL_GetPrimaryDisplay();
        m = id ? SDL_GetDesktopDisplayMode(id) : NULL;
        if (m) {
            /* The mode is in points on macOS; the window's pixels are
             * pixel_density times them, so the OS scale is the product
             * (SDL_GetWindowDisplayScale's). */
            float dens = m->pixel_density > 0 ? m->pixel_density : 1.0f;
            float cs = SDL_GetDisplayContentScale(id);
            d->w = (uint32_t)((float)m->w * dens + 0.5f);
            d->h = (uint32_t)((float)m->h * dens + 0.5f);
            d->scale = dens * (cs > 0 ? cs : 1.0f);
        }
    }
#endif
    if (!d->w || !d->h)
        return 0;
    /* The SMBIOS names win over an explicit SteamDeck=0 too: whether Steam
     * sets 0 in the Deck's Desktop Mode is not pinned down, and the panel
     * is the same either way. ui_fit = output is the way out. */
    deck = recomp_env(RENV_STEAM_DECK);
    if (!(deck && strcmp(deck, "1") == 0) && xbox_host_deck_board())
        deck = "1";
    nv2a_overlay_display_mark_deck(d, deck);
    return 1;
}

void enhance_display_start(void)
{
    static const char *const k_fit[] = {"display", "output", NULL};
    int fit = enhance_cfg_choice("overlay.ui_fit", k_fit, 0);
    nv2a_overlay_set_ui_fit(fit == 0);
    /* Under ui_fit = output too: the menu's switch to display is live, and
     * gamescope's fit of the window needs the display's size. */
    nv2a_overlay_set_display_probe(probe);
    if (enhance_cfg_source_of("overlay.ui_fit") != ENHANCE_SRC_DEFAULT)
        fprintf(stderr, "[ENHANCE] overlay.ui_fit=%s\n", k_fit[fit]);
}

/* How long the toast stays: two lines to read, not a 2 s "Saved". Two
 * of TOAST_SHOWN's three, so a toast the game raises meanwhile still shows. */
#define STEAM_HINT_MS 12000u

void enhance_display_steam_hint(void)
{
    const char *deck = recomp_env(RENV_STEAM_DECK);
    int on_deck = (deck && strcmp(deck, "1") == 0) || xbox_host_deck_board();
    if (!nv2a_overlay_steam_hint_wanted(on_deck, recomp_env(RENV_STEAM_GAME_ID),
                                        recomp_env(RENV_STEAM_APP_ID),
                                        recomp_env(RENV_STEAM_HINT)))
        return;
    /* Steam keeps its desktop layout on every pad while it runs, and only
     * a game it started gets the pad back (the user's Deck, Desktop Mode,
     * 2026-11-13: the native build from a terminal saw arrows, Enter and
     * Escape; from its Steam shortcut, the pad). */
    fprintf(stderr, "[ENHANCE] warning: started outside Steam on a Steam Deck (no SteamGameId "
                    "or SteamAppId): Steam's desktop controller layout may turn the pad into "
                    "keys (D-pad = arrows, A = Enter, B = Escape); start the game from its Steam "
                    "shortcut\n");
    nv2a_overlay_toast_for(STEAM_HINT_MS, "Started outside Steam: the pad may act as keys "
                                          "(D-pad = arrows, A = Enter).");
    nv2a_overlay_toast_for(STEAM_HINT_MS, "Start the game from its Steam shortcut for the "
                                          "full pad.");
}
