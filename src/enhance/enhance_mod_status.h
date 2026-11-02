/*
 * enhance_mod_status.h - the mods' status panels (overlay-revamp D12a,
 * mod API version 7).
 *
 * A mod sets its panel with api->status (mod.c keeps the text); this
 * draws them: one card per mod at the top left inside the title-safe
 * area, stacked in load order, its first line the title with the accent
 * dot and the rest hint lines. Tokens ({act:} {ctl:} {pad:} ...) expand
 * at every draw, for the device the player used last, so a panel follows
 * rebinding and a change of device without the mod calling again. Hidden
 * while the menu is open; a mod that is switched off or fails loses its
 * panel.
 *
 * The panels go on their own overlay layer (NV2A_OVL_STATUS), armed the
 * first time a panel is set, so a run that never sets one (stock, the
 * goldens) draws and arms nothing more.
 */
#ifndef ENHANCE_MOD_STATUS_H
#define ENHANCE_MOD_STATUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install the panel step on the flip tick and the glyph namer behind
 * api->glyph_name. enhance_menu_start calls it, menu on or off. */
void enhance_mod_status_start(void);

/* The flip tick's step (enhance_actions_set_panel_hook): track the
 * device, then compose or hide the layer. stopping: the tick stopped
 * (a disarm), hide the layer. */
void enhance_mod_status_tick(int stopping);

/* The device glyph_name and the panels use now (ENHANCE_DEV_*). */
int enhance_mod_status_device(void);
/* Tests: set the device as if the player had used it. */
void enhance_mod_status_set_device(int device);

/* Compose the panels into ctx for an out_w x out_h placement (the tick
 * does this between ui_begin and ui_publish). Returns how many panels
 * were drawn. */
typedef struct mu_Context mu_Context;
int enhance_mod_status_compose(mu_Context *ctx, uint32_t out_w, uint32_t out_h);

#ifdef __cplusplus
}
#endif

#endif /* ENHANCE_MOD_STATUS_H */
