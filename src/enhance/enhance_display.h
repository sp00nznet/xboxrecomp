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

#ifdef __cplusplus
}
#endif

#endif /* ENHANCE_DISPLAY_H */
