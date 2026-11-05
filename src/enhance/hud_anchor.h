/*
 * hud_anchor.h - the HUD layer: a game's HUD elements moved to the edges of
 * a wide display.aspect, in guest memory, where the game hands over their
 * vertices (openspec change hud-anchors).
 *
 * A mod registers the game's HUD table (TOML text it embeds, the schema in
 * docs/runtime/config.md) through api->hud_table: the canvas the game's
 * HUD coordinates live in, the vertex layout, the wrapper functions whose
 * callers name an element, the groups with their kind, and one row per
 * call site. From the pre callback of its hook on the call that takes the
 * vertices it calls api->hud_apply, which finds the element by return
 * address and shifts, clips or stretches the vertices for this frame; with
 * XBOX_HUD_RESTORE they are put back by api->hud_restore at that hook's
 * post, so a buffer the game keeps never accumulates a shift.
 *
 * The layer is game-agnostic: every VA, size and kind comes from the table.
 * It is active only while its owner mod is on, a table is registered and
 * the canvas's target has a Hor+ pad (display.aspect wider than 4:3). While
 * it is not, every entry point returns at its first test and the backends
 * see no lift (nv2a_aspect_hud_set), so stock and 4:3 are unchanged.
 *
 * Threads: hud_layer_table on the init thread; hud_layer_frame on the
 * guest's frame thread; the wrappers, apply and restore on any guest thread
 * (the call chain and the saved vertices are per thread).
 */
#ifndef HUD_ANCHOR_H
#define HUD_ANCHOR_H

#include <stddef.h>
#include <stdint.h>

#include "mod.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Vertices saved for XBOX_HUD_RESTORE; a call with more is left alone. */
#define HUD_RESTORE_MAX 1024u
/* RESTORE calls open at once per thread (nested); a deeper one is
 * skipped. */
#define HUD_SAVE_MAX 3u
/* The call chain's depth per thread; a deeper push is dropped. */
#define HUD_CHAIN_MAX 8u

/* The hook service the table's wrappers are hooked through (mod.c passes
 * its api->hook; a test, its own). */
typedef int (*hud_hook_fn)(uint32_t va, xbox_mod_hook_fn pre, xbox_mod_hook_fn post,
                           uint32_t flags, void *user);

/* Parse and register a table. Each [[wrapper]] is hooked through `hook`.
 * The number of sites, or < 0 with one "[hud] ..." line and nothing kept:
 * a parse error, an unknown version, table, key, group or kind, a
 * duplicate ret_va, a bad canvas or layout, a wrapper with no hook slot,
 * or a table already registered. */
int hud_layer_table(const char *toml, size_t len, hud_hook_fn hook);

/* Where guest memory is mapped: a pointer to a base that may still be 0
 * (mod.c's api.mem, which it fills on the first frame or hook). */
void hud_layer_set_mem(const uintptr_t *base);

/* The keys (enhance.c, at start): display.hud_aspect as aw:ah (0:0 =
 * display, the frame's pad) and display.hud_margin. Any thread. */
void hud_layer_set_keys(unsigned aw, unsigned ah, int margin);
/* display.hud_margin alone, live (the menu). Clamped to 0..64. */
void hud_layer_set_margin(int margin);
int hud_layer_margin(void);

/* Once per guest frame (xbox_mod_frame), on the guest's thread, outside
 * every wrapper: bumps the call chain's epoch, reads the canvas and the
 * target, and sets the active flag from owner_on (the owner mod on), the
 * table and the pad, telling the backends (nv2a_aspect_hud_set). Returns
 * the flag. */
int hud_layer_frame(int owner_on);

/* api->hud_apply and api->hud_restore (mod.h). */
int hud_layer_apply(const struct xbox_mod_call *c, const struct xbox_hud_call *d);
void hud_layer_restore(const struct xbox_mod_call *c);
/* A post of the owner's hook while the owner is off (mod.c): the wrapper
 * pop and the restore the mod's own post would have done. */
void hud_layer_post_off(const struct xbox_mod_call *c);

int hud_layer_active(void);
int hud_layer_has_table(void);
/* The table was loaded by the capture (hud_capture_table): the player's
 * HUD margin item and the hud_pad line stay off, as they are with no mod. */
void hud_layer_table_is_capture(void);
int hud_layer_has_mod_table(void);
/* The pad the edge kinds move by now, guest pixels: 0 while inactive. */
int hud_layer_pad_hud(void);
/* The canvas's target pad, guest pixels (0 at 4:3). */
int hud_layer_pad(void);

/* ---- the mapping capture (hud_capture.c; design A15) --------------------
 *
 * The capture records and changes nothing. While it runs, the active flag
 * is set for recording only: the wrappers keep their chain and
 * hud_layer_apply reports each call to the recorder, but only the mod part
 * (the owner on and a pad) shifts vertices or lifts batches, as without a
 * capture. */

/* One hud_layer_apply call, before any change to its vertices. */
struct hud_record {
    uint32_t chain[HUD_CHAIN_MAX];  /* the wrappers' return addresses, outermost first */
    unsigned nchain;
    uint32_t ret;                   /* the hooked call's own [esp] */
    uint32_t site;                  /* the matched row's ret_va, 0 when unlisted */
    const char *element, *group;    /* the row's, NULL when unlisted */
    uint32_t n, prims;              /* vertices; n / layout.prim */
    int has_bbox;
    float bbox[4];                  /* x0, y0, x1, y1 in canvas units */
    uint32_t queue;
    uint64_t tex;
};
typedef void (*hud_record_fn)(const struct hud_record *r);

/* The capture's part of the active flag, read at the next hud_layer_frame.
 * Init thread. */
void hud_layer_set_capture(int on);
/* The recorder hud_layer_apply calls (NULL: none). Any thread. */
void hud_layer_set_recorder(hud_record_fn fn);

/* This frame's canvas (the snapshot apply reads). 0 without a table. */
struct hud_layer_canvas {
    float w, h, view[4];            /* canvas units; view in guest px */
    uint32_t tw, th, pad;           /* the target, guest px */
    int top_left, y_up;
};
int hud_layer_canvas(struct hud_layer_canvas *out);
/* The site row for a return address: 1 with its element and group names. */
int hud_layer_site(uint32_t ret, const char **element, const char **group);
/* The layout: bytes per vertex, x's offset, vertices per primitive. */
void hud_layer_layout(uint32_t *stride, uint32_t *x_off, uint32_t *prim);
/* The bounds of n vertices at va, `stride` bytes apart (x at x_off, y
 * right after it when the stride holds it), in the units they are written
 * in; up to 4096 vertices, non-finite ones skipped. 0 when none is finite
 * or guest memory is not mapped. */
int hud_layer_bbox(uint32_t va, uint32_t n, uint32_t stride, uint32_t x_off, float out[4]);

/* The table's [capture] draws (level 0): read with the table and never
 * hooked by the layer. A row the runtime cannot use keeps va 0 and says
 * why; such a row never fails the table, so a mod's table is unchanged by
 * what its capture rows say. */
enum { HUD_LOC_NONE = 0, HUD_LOC_ECX, HUD_LOC_EDX, HUD_LOC_STACK, HUD_LOC_CONST };
struct hud_loc {
    int kind;
    uint32_t v;                     /* STACK: byte offset past the return address; CONST */
};
#define HUD_DRAWS_MAX 64u
struct hud_draw {
    uint32_t va;                    /* 0: not usable, see why */
    char spec[64];                  /* the row's fn/va as written */
    char name[48];
    struct hud_loc vtx, count, stride;
    uint32_t prim;                  /* vertices per primitive */
    const char *why;
};
int hud_layer_draws(const struct hud_draw **out);

/* Tests and shutdown: drop the table and every state. The wrappers stay
 * hooked in the hook service, and do nothing until a new table lists
 * them again. */
void hud_layer_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* HUD_ANCHOR_H */
