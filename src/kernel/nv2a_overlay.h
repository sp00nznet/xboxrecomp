/*
 * nv2a_overlay.h - the host overlay drawn over the presented frame: the
 * stats strip, toasts, the enhancements menu and its cursor.
 *
 * Built only with -DXBOXRECOMP_ENHANCE=ON. A build without it has no call
 * site, and a run that never arms it (every golden: RECOMP_CONFIG=none)
 * allocates nothing, creates no UI context and bakes no atlas.
 *
 * Four layers, drawn in this order. Each has one writer at a time and the
 * presenting thread as its reader:
 *
 *   STATS   the presenting thread, at 2 Hz (nv2a_overlay_poll)
 *   TOAST   the presenting thread, from a ring any thread pushes to
 *   MENU    the poll thread, or the flip tick, under the menu-step lock
 *   CURSOR  the presenting thread (phase 3F)
 *
 * A layer is a triple buffer of draw lists. The writer fills its back slot
 * and publishes it by one atomic exchange with the "ready" slot (a packed
 * index word with a fresh bit); the reader swaps "ready" with its "front"
 * slot when the fresh bit is set. Neither side waits and no slot is
 * written while it is read.
 *
 * A draw list is textured, coloured triangles in output pixels of the
 * placement rect it was built for (out_w x out_h, origin at the rect's
 * top left), with one scissor and texture per command and premultiplied
 * alpha. Texture 0 is the font atlas, which has a white texel for solid
 * fills; 1.. are image slots. A presenter draws a list into its current
 * placement rect, so a list built for another size is scaled by the ratio
 * for the frames until its writer rebuilds it.
 *
 * Zero cost while closed: a presenter reads nv2a_overlay_visible() (one
 * relaxed load) and does nothing more when it is 0.
 */
#ifndef NV2A_OVERLAY_H
#define NV2A_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mu_Context mu_Context;   /* third_party/microui/microui.h */

enum nv2a_overlay_layer {
    NV2A_OVL_STATS, NV2A_OVL_TOAST, NV2A_OVL_MENU, NV2A_OVL_CURSOR,
    NV2A_OVL_COUNT
};

/* One vertex, in output pixels of the list's placement. rgba is
 * premultiplied, bytes r, g, b, a in memory order (R8G8B8A8_UNORM). */
struct nv2a_ovl_vtx { float x, y, u, v; uint32_t rgba; };

/* One draw: count indices from first, scissored to clip (x, y, w, h in
 * the list's output pixels), sampling texture tex (0 = atlas). */
struct nv2a_ovl_cmd { uint32_t first, count; int16_t clip[4]; uint16_t tex; };

struct nv2a_ovl_list {
    const struct nv2a_ovl_vtx *vtx;
    const uint16_t *idx;
    const struct nv2a_ovl_cmd *cmd;
    uint32_t nvtx, nidx, ncmd;
    uint32_t out_w, out_h;      /* the placement the list was built for */
    uint32_t layer;             /* enum nv2a_overlay_layer: a renderer's cache key */
    uint64_t gen, atlas_gen;    /* gen moves at every publish of the layer */
    uint32_t sum;               /* the writer's checksum; 0 when not set */
};

/* The font atlas: premultiplied white (each texel's coverage in all four
 * bytes), so a vertex colour times the texel is the glyph's colour. */
struct nv2a_ovl_atlas { const uint32_t *px; uint32_t w, h; uint64_t gen; };

/* An image slot (1..NV2A_OVL_IMAGES): premultiplied RGBA8. */
#define NV2A_OVL_IMAGES 8
struct nv2a_ovl_image { const uint32_t *px; uint32_t w, h; uint64_t gen; };

/* ── Arming (the init thread) ── */

/* Allocates the lists of the layers in mask (1 << layer), their UI
 * contexts and the atlas, baked for out_h output lines. Once per process;
 * a later call only adds layers, and may run while the presenter reads
 * (each layer is published armed with release), but two arming threads
 * at once are not supported. Returns 0, or -1 when out of memory or the
 * font fails (one log line; the overlay stays disarmed). enhance.c arms
 * STATS and TOAST at init, hidden, when a feature key arms the actions
 * hook; stock leaves the overlay disarmed. */
int nv2a_overlay_arm(uint32_t layers, uint32_t out_h);
/* overlay.ui_scale (D2): the player's multiplier on the output's scale,
 * clamped to 0.75..2.0 (returned). Set before the first arm: the atlas
 * is baked once, at the scale of that moment, so a later value would
 * draw glyphs at sizes the atlas does not hold. enhance.c sets it at
 * init; unset it is 1.0. */
float nv2a_overlay_set_ui_scale(float mul);
int nv2a_overlay_armed(void);
/* Whether layer l is armed (the menu: menu.enabled at enhance init). */
int nv2a_overlay_layer_armed(enum nv2a_overlay_layer l);

/* The stats strip shown or hidden (overlay.stats; the action of that
 * name). Arms STATS first if needed; a no-op while disarmed. */
void nv2a_overlay_set_stats(int on);
int  nv2a_overlay_stats(void);

/* The game's name for the menu's header ("Enhancements · <name>"), UTF-8,
 * cut at 127 bytes: the window's title setters pass it on (a title's own
 * xbox_HostWindowSetTitle, else the XBE certificate's name). The kernel
 * holds it so the window code does not link the enhance layer. "" for
 * none. */
void nv2a_overlay_set_game(const char *utf8);
const char *nv2a_overlay_game(void);

/* ── Reader side (the presenting thread) ── */

/* Bit (1 << layer) per visible layer. One relaxed load. */
uint32_t nv2a_overlay_visible(void);

/* Composes STATS and TOAST when due, from the last placement. Returns a
 * value that moves whenever what the overlay would draw moves (a publish
 * or the visible mask), so a presenter that only redraws on change (the
 * SDL loop) can tell. */
uint64_t nv2a_overlay_poll(uint64_t now_ms);

/* The newest published list of a visible layer, or NULL. */
const struct nv2a_ovl_list *nv2a_overlay_acquire(enum nv2a_overlay_layer l);

/* The visible layers' lists in draw order; returns how many. */
int nv2a_overlay_lists(const struct nv2a_ovl_list *out[NV2A_OVL_COUNT]);

const struct nv2a_ovl_atlas *nv2a_overlay_atlas(void);
const struct nv2a_ovl_image *nv2a_overlay_image(uint16_t tex);

/* The frame's placement rect in output pixels, written at each present.
 * Every overlay size comes from it, never from the window. */
void nv2a_overlay_set_placement(int x, int y, int w, int h);
void nv2a_overlay_placement(int *x, int *y, int *w, int *h);

/* The presenter's name for the strip ("Metal", "D3D11", "SDL"). */
void nv2a_overlay_set_presenter(const char *name);
const char *nv2a_overlay_presenter(void);   /* NULL before the first present */
/* Called from any thread when what the overlay draws moved (a publish,
 * the visible mask, a toast): a presenter that sleeps between frames
 * wakes its loop. It must not block or take the overlay's calls. */
void nv2a_overlay_set_wake(void (*fn)(void));

/* ── Writer side (the layer's one writer at a time) ── */

/* A microui frame sized from the placement: mu_begin done, the caller
 * draws, then publishes (mu_end, the conversion and the publish). NULL
 * when the layer is not armed. */
mu_Context *nv2a_overlay_ui_begin(enum nv2a_overlay_layer l);
void nv2a_overlay_ui_publish(enum nv2a_overlay_layer l);
void nv2a_overlay_hide(enum nv2a_overlay_layer l);

/* The menu-step lock: one writer of MENU at a time (the poll thread, or
 * the flip tick when the poll is late). A step that misses it skips. */
int  nv2a_overlay_menu_trylock(void);
void nv2a_overlay_menu_unlock(void);

/* Image slot tex (1..NV2A_OVL_IMAGES), copied; the slot's storage is made
 * at its first set and a larger image later is refused. Returns 0 or -1. */
int nv2a_overlay_image_set(uint16_t tex, const uint32_t *rgba, int w, int h);

/* Any thread: a line shown for 2 s at the bottom (at most three at once).
 * It goes to an 8-line ring, the oldest dropped, that the presenting
 * thread drains. Always logged as "[TOAST] <line>"; shown only while the
 * overlay is armed. */
void nv2a_overlay_toast(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* ── Low level: the back slot itself (the converter and the ctest) ── */

struct nv2a_ovl_build {
    struct nv2a_ovl_vtx *vtx;
    uint16_t *idx;
    struct nv2a_ovl_cmd *cmd;
    uint32_t nvtx, nidx, ncmd;
    uint32_t cap_vtx, cap_idx, cap_cmd;
    uint32_t out_w, out_h;
    int cut;                    /* set when a command did not fit */
};
struct nv2a_ovl_build *nv2a_overlay_build_begin(enum nv2a_overlay_layer l,
                                                uint32_t out_w, uint32_t out_h);
void nv2a_overlay_build_publish(enum nv2a_overlay_layer l, uint32_t sum);

/* FNV-1a over a list's vertices (positions and UVs at 1/16 px), indices
 * and commands: the ctest's checksum. */
uint32_t nv2a_overlay_list_sum(const struct nv2a_ovl_vtx *vtx, uint32_t nvtx,
                               const uint16_t *idx, uint32_t nidx,
                               const struct nv2a_ovl_cmd *cmd, uint32_t ncmd);

/* ctest only: back to disarmed, everything freed. */
void nv2a_overlay_reset_for_test(void);
/* ctest only: seen gets each toast line the presenter's drain takes;
 * mid_push runs inside a push, after the head moved and before the line
 * is written. */
void nv2a_overlay_toast_hook_for_test(void (*seen)(const char *line), void (*mid_push)(void));

#ifdef __cplusplus
}
#endif

#endif /* NV2A_OVERLAY_H */
