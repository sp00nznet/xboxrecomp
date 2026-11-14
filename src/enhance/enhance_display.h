/*
 * enhance_display.h - the display probe for the overlay's scale
 * (overlay.ui_fit; nv2a_overlay_set_display_probe).
 */
#ifndef ENHANCE_DISPLAY_H
#define ENHANCE_DISPLAY_H

#include "nv2a_overlay.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reads overlay.ui_fit and, for display (the default), installs the
 * probe. Before the first overlay arm (xbox_enhance_init). */
void enhance_display_start(void);

/* On a Steam Deck started outside Steam (no SteamGameId nor SteamAppId),
 * a warning line and a toast: Steam's desktop controller layout then turns
 * the pad into keys. RECOMP_STEAM_HINT=0 (the harnesses) keeps
 * both out. After the toast layer's arm (xbox_enhance_init). */
void enhance_display_steam_hint(void);

#ifdef __cplusplus
}
#endif

#endif /* ENHANCE_DISPLAY_H */
