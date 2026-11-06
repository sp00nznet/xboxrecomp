/*
 * mod.h - the enhancements layer's mods: game code built into the title's
 * executable that reads and steers the guest through one small API
 * (opt-in, XBOXRECOMP_ENHANCE=ON).
 *
 * A mod is a struct xbox_mod. The title lists its mods in a NULL-terminated
 * table and hands it over before xbox_enhance_init:
 *
 *   static const struct xbox_mod *const mods[] = { &free_camera, NULL };
 *   xbox_mod_register_table(mods);
 *   xbox_enhance_init(".", NULL);
 *
 * A call and not a self-registering macro: a static library drops an object
 * nothing names, a linker section is spelled differently for PE, Mach-O and
 * ELF, and the table gives the order. A title that calls nothing has no
 * mods and stays stock.
 *
 * xbox_enhance_init runs each mod's init, in table order, as its last step
 * (after the config files and the [input] keys, so the getters see every
 * file). The guest has not started then and its memory is not mapped yet:
 * a mod registers what it wants and touches the guest from its on_frame
 * callbacks on. A non-zero init leaves that mod out: what it registered is
 * dropped, its capture released and its shutdown never called. Shutdown
 * runs in reverse order, once, at exit (atexit, and recomp_exit_hook for
 * the Win32 windows' ExitProcess) or on xbox_mod_shutdown.
 *
 * Shutdown can run while the guest and the render thread still do: a
 * frame or a filter call may be in flight, or start just before the filter
 * is removed. A mod's shutdown therefore must not free or invalidate
 * anything its on_frame or filter callbacks use; restore guest state, give
 * input back, write files, and leave the memory to the process exit.
 *
 * The title's glue calls two entries: xbox_mod_frame once per guest frame,
 * from the guest's thread, and xbox_mod_input_captured from its pad read,
 * where a captured pad reads as released to the title.
 *
 * With no mods: one line, "[ENHANCE] mods=none" (the mods that run and
 * take effect, for the golden), no callbacks, and the draw paths' one
 * branch (nv2a_batch_skip) is never taken. Docs: docs/runtime/config.md,
 * "Mods".
 */
#ifndef XBOX_ENHANCE_MOD_H
#define XBOX_ENHANCE_MOD_H

#include <stddef.h>
#include <stdint.h>

#include "enhance_actions.h"
#include "enhance_input.h"
#include "host_pad.h"
#include "nv2a_backend_common.h"
#include "recomp_env.h"
#include "recomp_hook_mod.h"        /* struct xbox_mod_call, xbox_mod_hook_fn */

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_MOD_API_VERSION 8u

struct xbox_mod_api;

/* draw3d's vertex: a world-space point and its colour, 0xAARRGGBB
 * (straight alpha). */
struct xbox_dbg_vtx {
    float p[3];
    uint32_t argb;
};

/* draw3d_tris and draw3d_lines flags. */
#define XBOX_DBG_XRAY   0x1u    /* no depth test: seen through walls, drawn last */
#define XBOX_DBG_OPAQUE 0x2u    /* triangles: also write depth, so they hide what is behind */
#define XBOX_DBG_PULL   0x4u    /* nudged toward the eye, so edges on a face are not lost */

/* The HUD layer's kinds (api->hud_table's groups, hud_anchor.h). */
enum {
    XBOX_HUD_CENTRE = 0,        /* stays put, clipped to the 4:3 columns */
    XBOX_HUD_LEFT,              /* moved to the HUD's left edge */
    XBOX_HUD_RIGHT,             /* moved to the HUD's right edge */
    XBOX_HUD_PROPORTIONAL,      /* moved by its distance from the centre */
    XBOX_HUD_WORLD,             /* projected by the game: not moved or clipped */
    XBOX_HUD_STRETCH            /* mapped across the wide frame, as H5's stretch */
};

/* One call that hands a HUD element's vertices over (api->hud_apply). */
struct xbox_hud_call {
    uint32_t vtx_va;            /* guest address of the first vertex */
    uint32_t n;                 /* vertex count */
    uint32_t flags;             /* XBOX_HUD_RESTORE */
    uint32_t queue;             /* the game's queue or layer, for capture; 0 */
    uint64_t tex;               /* the pack's texel hash of its texture, or 0 */
};
#define XBOX_HUD_RESTORE 1u

/* A guest call's arguments (api->guest_call). stack[0] is the first stack
 * argument, at [esp + 4] when the callee starts. */
struct xbox_guest_args {
    uint32_t eax, ecx, edx;     /* register arguments (thiscall, fastcall, custom) */
    const uint32_t *stack;      /* pushed right to left; NULL when n_stack is 0 */
    uint32_t n_stack;           /* at most XBOX_GUEST_MAX_STACK */
};
#define XBOX_GUEST_MAX_STACK 16u
struct xbox_mod_desc;
struct enhance_item;            /* enhance_menu.h */

struct xbox_mod {
    const char *name;           /* the log lines' [MOD name]; the mod's id */
    /* 0 to run; anything else leaves the mod out. May be NULL. */
    int (*init)(const struct xbox_mod_api *api);
    void (*shutdown)(void);     /* may be NULL */
    /* v3: what the mod is and what it reads, as const data the toolkit
     * reads before any init (below). NULL keeps the v2 behaviour. Appended:
     * this is a source contract for mods linked into the game, not a plugin
     * ABI (struct xbox_mod has no size field). */
    const struct xbox_mod_desc *desc;
    /* v4, appended for the same reason; tables written before are zeroed
     * here.
     *
     * needs: what the mod uses (XBOX_MOD_NEED_*). A declaration only for
     * now: nothing reads it until the enable gate (mod-connectivity-layer
     * task 4.2) lands.
     *
     * follows: NULL, or a config key that turns the mod on ("camera.free"):
     * while that key holds its config.default.toml value, the mod is not
     * counted in the [ENHANCE] mods= token even if its init ran, since it
     * does nothing then. The key is read once, at start; a value that does
     * not parse counts as changed, so the mod is listed. It does not gate
     * the init (4.2 again). A mod that reads its own key, or whose effect
     * depends on more than one key, should report api->active() instead:
     * that follows what it actually does, live. */
    uint32_t needs;
    const char *follows;
};

/* struct xbox_mod.needs. */
#define XBOX_MOD_NEED_ASPECT 0x01u
#define XBOX_MOD_NEED_STATE  0x02u
#define XBOX_MOD_NEED_POKE   0x04u
#define XBOX_MOD_NEED_HOOK   0x08u
#define XBOX_MOD_NEED_PLACE  0x10u

/* ---- the descriptor (v3; docs/runtime/config.md, "Mods") ----------------
 *
 * Static data: the Mods page and the descriptor dump (RECOMP_MODS_DESCRIBE)
 * describe a mod that is off or never inited from it, without running its
 * code. Validated once at xbox_mod_init: a duplicate setting name or
 * control id, lo > hi, an enum with no choices, an unknown kind or a second
 * XBOX_SET_ENABLE fails the mod (XBOX_MOD_FAILED, one [MOD] line). Text is
 * UTF-8. The toolkit reads only the fields inside `size`, so fields can be
 * appended.
 *
 * Version 2 adds the kind XBOX_SET_LINK; the structs are unchanged, so a
 * version 1 descriptor reads as before. A toolkit before it refuses the
 * kind (unknown kind): a mod that must build against one puts its link
 * rows under #if XBOX_MOD_DESC_VERSION >= 2. */
#define XBOX_MOD_DESC_VERSION 2u

enum {
    XBOX_SET_BOOL = 0,
    XBOX_SET_INT,
    XBOX_SET_FLOAT,
    XBOX_SET_ENUM,
    /* v2: a row on the mod's page for a toolkit menu item, `key` being
     * the item's ("input.pad_square_left"). The row is that item's own:
     * one value, shown the same on both pages, saved once under the
     * item's key. label and help NULL take the item's; env, aliases and
     * XBOX_SET_ENABLE are refused, the other fields ignored. A key that
     * names no toolkit item (enhance_menu_toolkit_item) refuses the
     * descriptor, and so does a LINK in a version 1 descriptor. The
     * toolkit keeps no value for it: setting reads the item now (a toggle
     * 0/1, a choice its index, a numeric choice or a range its number), a
     * change is clamped and snapped as the item's own row does it,
     * on_setting is never called for it, the mod's Reset leaves it alone
     * and RECOMP_DEBUG=mod_set_at does not take its key. */
    XBOX_SET_LINK
};
#define XBOX_SET_ENABLE 0x1u    /* flags: this setting is the mod's switch */

/* A setting the toolkit owns (all but a LINK): it resolves the value (env
 * row, then the user files' key then aliases, then config.default.toml's
 * key then aliases, then def), clamps it to lo..hi and snaps it to
 * lo + k * step (each change logged "[MOD] <id>.<name>: <value> -> <used>"),
 * and the mod reads it with api->setting. Bool is 0/1, an enum the
 * choice's index. */
struct xbox_mod_setting {
    const char *name;           /* "speed": key mods."<id>.speed" */
    const char *key;            /* NULL, or the full key ("camera.free_speed");
                                   LINK: the toolkit item's */
    const char *env;            /* NULL, or a recomp_env row name ("RECOMP_...") */
    const char *const *aliases; /* retired full keys, NULL-terminated, or NULL */
    const char *label, *help, *unit;
    uint8_t kind;               /* XBOX_SET_* */
    uint8_t live;               /* enhance_item_live: LIVE, LIVE_ACTIVATE, LIVE_RESTART */
    uint8_t flags;              /* XBOX_SET_ENABLE */
    double def, lo, hi, step;   /* int and float: lo..hi, step 0 for none */
    const char *const *choices; /* enum: NULL-terminated; def is an index */
};

enum {
    XBOX_CTL_ACTION = 0,        /* a host hotkey: action "<id>.<control id>" */
    XBOX_CTL_BUTTON,            /* read every frame while the mod captures */
    XBOX_CTL_STICK,
    XBOX_CTL_INFO               /* shown, never bound ("Mouse: look") */
};
#define XBOX_CTL_SRC_LEFT  0u
#define XBOX_CTL_SRC_RIGHT 1u

struct xbox_mod_control {
    const char *id;             /* "toggle": action "<mod>.toggle" */
    const char *label, *help, *when;   /* when: "while flying", or NULL */
    uint8_t kind;               /* XBOX_CTL_* */
    const char *const *defaults;       /* binding strings, NULL-terminated */
    const char *const *aliases;        /* retired full keys, NULL-terminated */
    const char *env;            /* NULL, or a recomp_env row: its value
                                   ("F6,guide+y") replaces the bindings */
};

/* control_value's answer for a captured control. */
struct xbox_ctl_value {
    int16_t raw[2];             /* STICK: the axes as the pad reports them;
                                   BUTTON: raw[0] 0..255, the largest binding */
    uint8_t src;                /* STICK: XBOX_CTL_SRC_LEFT or _RIGHT */
    uint8_t connected;          /* 0: port 0 has no pad and no keyboard source */
};

struct xbox_mod_desc {
    uint32_t version;           /* XBOX_MOD_DESC_VERSION */
    uint32_t size;              /* sizeof(struct xbox_mod_desc) */
    const char *title, *summary, *author, *version_text;
    const struct xbox_mod_setting *settings;
    uint32_t n_settings;
    const struct xbox_mod_control *controls;
    uint32_t n_controls;
};

typedef void (*xbox_mod_frame_fn)(void *user);
/* Non-zero skips the batch (nv2a_backend_common.h, "Batch filter"). */
typedef int (*xbox_mod_batch_fn)(const struct nv2a_batch_info *b, void *user);

struct xbox_mod_api {
    uint32_t version;           /* XBOX_MOD_API_VERSION */
    uint32_t size;              /* sizeof(struct xbox_mod_api): fields are only added */

    /* The guest's memory: guest address va is host address mem + va. Set
     * before the first on_frame or filter call; 0 during init. */
    uintptr_t mem;

    /* The layer's config getters (enhance_cfg.h): env over the files over
     * the default. A mod's keys are flat names under [mods]
     * ("<name>.speed" = 2, beside its switch "<name>" = true). A key no
     * lookup reads is reported as a typo, except under a v2 mod switched
     * off by its key, whose code never ran to read them. A descriptor
     * mod's settings come through `setting`. */
    const char *(*cfg_string)(const char *key, const char *def);
    long long (*cfg_int)(const char *key, long long def);
    double (*cfg_float)(const char *key, double def);
    int (*cfg_bool)(const char *key, int def);
    int (*cfg_choice)(const char *key, const char *const *choices, int def);
    int (*cfg_bind_env)(const char *key, recomp_env_id id);

    /* One stderr line, "[MOD <m->name>] ..."; the newline is added. */
    void (*log)(const struct xbox_mod *m, const char *fmt, ...);

    /* Init only. fn(user) runs once per guest frame from xbox_mod_frame, on
     * the guest's thread, mods in table order and each mod's in the order
     * it asked. Returns 0 when the table is full (XBOX_MOD_MAX_FRAME). */
    int (*on_frame)(xbox_mod_frame_fn fn, void *user);

    /* Port 0 and the mouse as enhance_input_state gives them: the title's
     * last pad read (a mod never polls: a poll moves the keyboard's stick
     * ramp) and, while captured, the mouse's raw motion. Any thread. */
    void (*input_state)(struct enhance_input_state *out);
    /* Takes port 0 from the title (1) or gives it back (0); see
     * enhance_input_capture. The last call wins across mods. */
    void (*input_capture)(int on);

    /* Init only. fn(b, user) is asked before each GPU draw, on the render
     * (ack) thread; any filter's non-zero skips the batch. Returns 0 when
     * the table is full (XBOX_MOD_MAX_FILTER). */
    int (*batch_filter)(xbox_mod_batch_fn fn, void *user);

    /* ---- version 2 (check size before using these) ---------------------
     * Host actions (enhance_actions.h): a named thing the mod does, bound
     * by [input.actions] "<name>" (or its defaults) to keys, host-only
     * buttons and pad chords that never take an input the title uses. */

    /* Init only. name "<mod>.<verb>" in lowercase, label for the menu,
     * defaults a NULL-terminated list ("F6", "guide+y") or NULL. Returns
     * the id, or -1 with a line (duplicate, bad name, full). */
    int (*action_register)(const char *name, const char *label,
                           const char *const *defaults);
    /* Fires since the last call (any thread); held now past its hold. A
     * binding inside a longer one on its port (guide in guide+a, whoever
     * owns that) fires at its release instead and never reads as down, so
     * a hold-to-act action needs a binding no other one contains. */
    int (*action_pressed)(int id);
    int (*action_down)(int id);
    /* Init only. An item (enhance_menu.h) on the mod's own sub-page of the
     * menu's Mods page: entering the mod's row lists them. The item is kept,
     * not copied, so it must outlive the process (a static). Its key
     * persists through Save like the toolkit's. Returns 0 when the table is
     * full (XBOX_MOD_MAX_ITEMS) or the item has no key or label. */
    int (*menu_add)(const struct enhance_item *it);
    /* Init only. fn(on, user) when the Mods page turns the mod off (0) or
     * on (1). The toolkit records the request and calls fn from the next
     * xbox_mod_frame, on the guest's thread, so the mod changes guest
     * memory and its own state from the thread its on_frame uses. Only
     * the last toggle before a frame reaches fn, so fn may be told the
     * state the mod is already in: it must be idempotent. Off stops
     * nothing by itself: on_frame, the filters and armed actions keep
     * being called, and the mod gates them while it is off. A mod that
     * gives none shows "applies at restart" on its row. Returns 0 when fn
     * is NULL or outside init. */
    int (*on_enable)(void (*fn)(int on, void *user), void *user);
    /* A short message on screen; a "[MOD <name>] toast:" line until the
     * overlay lands (phase 3B). */
    void (*toast)(const struct xbox_mod *m, const char *fmt, ...);
    /* Arm the actions hook when the config key (the mod's enable key,
     * "camera.free") is true; returns whether the hook is armed. Nothing
     * fires and nothing is filtered unless some feature key arms it.
     * From init only: a mod left out takes its key with it. */
    int (*actions_arm)(const char *key);

    /* ---- version 3 (check size before using these) ---------------------
     * The descriptor (struct xbox_mod_desc above); desc is the mod's own. */

    /* Setting idx's value now: a relaxed atomic load, any thread. Bool
     * 0/1, an enum the choice's index. 0 for a bad desc or idx. A LINK
     * calls its item's get instead, so it is only as thread-safe as that
     * get (input.pad_square_left's reads the pad setup, safe from any
     * thread); link only items whose get is. The values it reads stay
     * valid until process exit: shutdown frees nothing, and only a re-init
     * (the tests) frees them, so a callback still in flight at shutdown
     * reads safely. */
    double (*setting)(const struct xbox_mod_desc *desc, uint32_t idx);
    /* Init only. fn(idx, value, user) after the menu changes a setting,
     * from the next xbox_mod_frame on the guest's thread (as on_enable).
     * A RESTART setting only records the value and never calls back. */
    int (*on_setting)(const struct xbox_mod_desc *desc,
                      void (*fn)(uint32_t idx, double value, void *user), void *user);
    /* The action id of ACTION control idx (registered by the toolkit
     * before init as "<id>.<control id>"), for action_pressed and
     * action_down; -1 for another kind or a bad idx. */
    int (*control_action)(const struct xbox_mod_desc *desc, uint32_t idx);
    /* A captured control's value, from port 0 as the title would have
     * read it this poll (after [input.pad], [input.bindings] and the
     * consumed-chord mask, before the neutral pad), on the guest's thread.
     * BUTTON: raw[0] 0..255, the most of its bindings (a digital button,
     * a host key or a mouse button reads 255). STICK: raw[0..1] the stick's x and y, src
     * which stick (the caller picks the dead zone by it). connected is 0
     * while no capture holds port 0, before its first poll, and with no
     * pad (a host key still reads). Returns 1 when it read a source: a
     * connected pad, or a host key or mouse button binding. */
    int (*control_value)(const struct xbox_mod_desc *desc, uint32_t idx,
                         struct xbox_ctl_value *out);

    /* ---- version 4 (check size before using these) --------------------- */

    /* Init only. Run pre(c, user) before and post(c, user) after each call
     * of the guest function at va, on the guest thread that calls it,
     * inside the call; they must not block. Either may be NULL, not both.
     * The game must give va a hook slot (game.toml [mods] hook_functions);
     * without one, hook returns 0 with a line. Several mods may hook one
     * VA: the pre callbacks run in table order and the post callbacks in
     * reverse. With XBOX_MOD_HOOK_MAY_SKIP the pre may set c->skip to
     * return c->eax/edx_ret (and c->st0, on a function that returns a
     * float) without running the function; one such hook per VA, a second
     * is refused, as is any other flag bit. A skip from a hook without the
     * flag is ignored, logged once. The later pre callbacks and every post
     * see the skip and may still change the return. c->st0_valid is
     * read-only. Returns 1, or 0. See recomp_hook_mod.h.
     *
     * The hooks follow the mod's state: while it is off (inited dormant,
     * or switched off live) its callbacks are not called and the function
     * runs as if unhooked by it. From shutdown on, none is called. */
    int (*hook)(uint32_t va, xbox_mod_hook_fn pre, xbox_mod_hook_fn post,
                uint32_t flags, void *user);
    /* The mod's own "doing something now" state, for the [ENHANCE] mods=
     * token: a mod that calls this is listed only while it last said 1
     * (a default-on mod that does nothing at 4:3 stays out). One that
     * never calls it is listed while it runs and is on. Any thread. */
    void (*active)(const struct xbox_mod *m, int on);

    /* ---- version 5 (check size before using these) --------------------- */

    /* The HUD layer (hud_anchor.h; openspec change hud-anchors). Init
     * only: parse and register the game's HUD table, TOML text the mod
     * embeds (schema: docs/runtime/config.md, "HUD table"). Every
     * [[wrapper]] must have a hook slot; the layer hooks it for the mod. A
     * duplicate site, an unknown group or kind, a wrapper with no slot or a
     * bad canvas fails with one line and returns < 0. One table per run.
     * Returns the number of sites. The mod that registers it is listed in
     * mods= only while the layer is active (the mod on, the table, a wide
     * display.aspect); it need not call active itself. While a mapping
     * capture runs (debug.hud_capture), your hooks run with the mod off or
     * dormant too, so hud_apply can record its calls: a pre or post that
     * does more than hud_apply and hud_restore (writes guest state, counts,
     * toggles) tests hud_capturing first and does only those two then. */
    int (*hud_table)(const char *toml, size_t len);
    /* From a hook's pre on the call that takes a HUD element's vertices:
     * find the element by return address (the wrappers' call chain, then
     * the call's own) and shift, clip or stretch its vertices for the wide
     * frame. With XBOX_HUD_RESTORE the x and u it changes are saved and
     * put back by hud_restore at that hook's post (the call must copy the
     * vertices). The kind applied, or -1 when the call is left as it is
     * (the layer inactive, or the call skipped). Guest thread. */
    int (*hud_apply)(const struct xbox_mod_call *c, const struct xbox_hud_call *d);
    void (*hud_restore)(const struct xbox_mod_call *c);
    /* Non-zero when m's hooks run only for a mapping capture: one runs
     * (debug.hud_capture) and m owns the HUD table but is off or dormant,
     * so the capture alone calls them. Then a hook does only hud_apply and
     * hud_restore, and the run stays as it would be without the mod. 0
     * otherwise, the mod on included. Any thread; a few loads. */
    int (*hud_capturing)(const struct xbox_mod *m);

    /* ---- version 6 (check size before using these) ---------------------
     * The guest's own functions and memory. Both only on a guest thread
     * inside a mod callback (a hook's pre or post, an on_frame, on_enable
     * or on_setting callback); elsewhere they return 0 with one line. */

    /* Run the lifted guest function at va with a's registers and stack
     * arguments, the frame 0x100 bytes below the current guest esp and the
     * first argument 16-byte aligned. Every guest register (g_ebp,
     * g_seh_ebp, the x87 stack and control, SSE, MMX and mxcsr included) is
     * saved before and put back after, so a hook's function continues as if
     * no call happened, whatever the callee pops or clobbers. *eax_out gets
     * the callee's eax (may be NULL). 1, or 0 with a line: no lifted function
     * at va, more than XBOX_GUEST_MAX_STACK arguments, no guest stack, not a
     * guest thread. The callee runs inside the caller's call: it must not be
     * one that waits for the frame to end. */
    int (*guest_call)(uint32_t va, const struct xbox_guest_args *a, uint32_t *eax_out);
    /* The mod's one block of guest memory: size bytes, zeroed, aligned to
     * align (at least a page), from the kernel's contiguous arena, so the
     * title's heap never hands it out. The first call allocates; a later one
     * returns the same block for a size up to the first and 0 for a larger
     * one. Never freed. Asked from the guest thread at first use, so a mod
     * that stays off leaves the guest's memory map stock. Its guest VA, or
     * 0 with a line. */
    uint32_t (*guest_alloc)(const struct xbox_mod *m, uint32_t size, uint32_t align);

    /* ---- version 7 (check size before using these) ---------------------
     * Input glyphs and the status panel (overlay-revamp D12a). */

    /* The binding of ref on the device the player used last, into out (cap
     * bytes, always ended). ref is a source ("pad:south", "key:F5",
     * "mouse:left"), an action ("act:camera.free.toggle") or one of m's own
     * controls by id ("ctl:move"). flags XBOX_GLYPH_TOKENS writes tokens
     * ("{pad:ls}"), which draw as glyphs in any text the overlay draws;
     * XBOX_GLYPH_TEXT writes names ("Left stick", "F5") for a log line. A
     * chord's members are joined by "+". Off the pad, a captured control
     * bound only on the pad names the keys that drive it, a stick's joined
     * by "/" ("W/A/S/D"; the right stick also "Mouse move"). Returns the
     * length written, or -1: when ref is unbound or names nothing, out
     * holds ref itself, cut to cap; when cap is too short for the whole
     * name, out holds the members that fit whole (maybe none, ""), never
     * part of one. Works once the mods' inits are done: called from init
     * it answers -1 with ref (the device tracking and [input] bindings are
     * not up yet); a status text set from init still expands its tokens
     * at every draw. Any thread; the binding tables are read under their
     * lock. */
    int (*glyph_name)(const struct xbox_mod *m, const char *ref, char *out, size_t cap,
                      uint32_t flags);
    /* m's status panel: a small card at the top left of the screen. text
     * is up to XBOX_MOD_STATUS_LINES lines ('\n') of XBOX_MOD_STATUS_LINE
     * bytes, the first the panel's title; NULL or "" removes it. Tokens
     * ({pad:} {key:} {mouse:} {act:} {ctl:}) are kept as written and
     * expanded at every draw, so the panel follows the device in use and a
     * rebinding without another call. Hidden while the menu is open, and
     * dropped when m is turned off or fails. Any thread; the text is
     * copied. Returns 0, or -1 when it was cut to fit (what fits is set). */
    int (*status)(const struct xbox_mod *m, const char *text);

    /* Version 8: the 3D debug draw (nv2a_dbg_draw.h). World-space
     * triangles and lines with a colour each, drawn at the flip into the
     * frame being presented, after everything the game drew, through the
     * view and projection given to draw3d_begin. Depth-tested against the
     * game's depth buffer unless XBOX_DBG_XRAY; always alpha-blended.
     *
     * The matrices come from the mod, not from the toolkit: the NV2A sees
     * only composite matrices or a game's own vertex program, so there is
     * no game-agnostic "the view" to recover. A mod reads the title's
     * camera (a title often keeps view and projection in guest memory)
     * and passes them as 16 floats, row-major, row-vector D3D (clip = [x y z 1]
     * . view . proj, clip z in [0, w]). The depth test compares that clip
     * z / w with the frame's stored z / zmax, so it holds for a title
     * whose viewport maps z to [0, zmax].
     *
     * The drawing goes into the guest's own colour target, the one being
     * presented (and, for opaque faces, into its depth buffer), not into a
     * copy made for presentation. A title that does not clear that target
     * before its next frame, or that samples or reads back the last frame
     * (a pause backdrop, a motion blur, a screenshot), can capture the
     * drawing in what it draws next. It is a debug view: keep that in
     * mind before using it while such an effect is on screen.
     *
     * One frame: draw3d_begin, any number of draw3d_tris and draw3d_lines,
     * draw3d_end. Each flip draws the last ended frame, so a mod that
     * stops calling leaves its drawing on screen; an empty frame (begin,
     * end) clears it, and so does turning the mod off. One mod draws at a
     * time: begin while another mod's frame is open is refused. Call from
     * one thread, the guest's (an on_frame or a hook); the vertices are
     * copied, and taken to clip space, at the call. Draws nothing on the
     * CPU rasteriser. Returns: begin and end 0, or -1 (refused, not ON);
     * tris and lines the primitives taken (n_tris, n_lines, or fewer past
     * the list's limit), 0 outside a frame or for unknown flags. */
    int (*draw3d_begin)(const struct xbox_mod *m, const float view[16], const float proj[16]);
    int (*draw3d_tris)(const struct xbox_dbg_vtx *v, uint32_t n_tris, uint32_t flags);
    int (*draw3d_lines)(const struct xbox_dbg_vtx *v, uint32_t n_lines, uint32_t flags);
    int (*draw3d_end)(const struct xbox_mod *m);
};

/* glyph_name's flags: one of the two. */
#define XBOX_GLYPH_TOKENS 1u
#define XBOX_GLYPH_TEXT   2u

/* status's limits. */
#define XBOX_MOD_STATUS_LINES 6
#define XBOX_MOD_STATUS_LINE  120

/* Stack argument n (0-based) of a hooked call, in a pre or a post. */
#define XBOX_MOD_ARG32(api, c, n) XBOX_MOD_MEM32(api, (c)->esp + 4 + 4 * (n))
#define XBOX_MOD_ARGF(api, c, n)  XBOX_MOD_MEMF(api, (c)->esp + 4 + 4 * (n))

#define XBOX_MOD_MAX_FRAME   16
#define XBOX_MOD_MAX_FILTER  8
#define XBOX_MOD_MAX_ITEMS   32

/* Guest memory through the API: XBOX_MOD_MEM32(api, 0x0035C2A0) = 1. The
 * guest is little-endian and so is every host the toolkit runs on. */
#define XBOX_MOD_MEM32(api, va) (*(volatile uint32_t *)((api)->mem + (uint32_t)(va)))
#define XBOX_MOD_MEMF(api, va)  (*(volatile float *)((api)->mem + (uint32_t)(va)))

/* The title's table, NULL-terminated and kept (not copied): before
 * xbox_enhance_init. A second call replaces the first; NULL clears it. */
void xbox_mod_register_table(const struct xbox_mod *const *mods);

/* Called by xbox_enhance_init. Runs the inits; returns the mods running. */
int xbox_mod_init(void);

/* The title's glue, once per guest frame on the guest's thread. */
void xbox_mod_frame(void);

/* From the actions layer's flip tick: RECOMP_DEBUG=mod_set_at's requests
 * (mod.c). Nothing without the key. */
void xbox_mod_debug_tick(void);

/* The title's pad glue: whether a mod has port 0 (enhance_input_captured). */
int xbox_mod_input_captured(void);

/* The title's pad glue, once per port-0 poll after xbox_enhance_pad_filter,
 * with the state it is about to use (connected 0: a failed poll): records
 * it for control_value while a mod captures. Returns 1 then, and the glue
 * gives the title a connected, released pad, with a packet number that
 * moves when that starts and ends; 0 otherwise (any other port, or no
 * capture), at the cost of one load. A
 * title on xbox_InputGetState gets this from input_core.c. */
#define XBOX_MOD_PAD_SEEN 1
int xbox_enhance_pad_seen(DWORD port, const XBOX_GAMEPAD *g, int connected);

/* The title's pad glue, after its own merge (keyboard, script): the host
 * pad behind port (host_pad.h; connected = 0 for a script or no pad) and
 * the state the title is about to use, from which a consumed chord's
 * buttons are taken out (enhance_actions.h). Returns at its first test
 * while the hook is disarmed. A title on xbox_InputGetState gets this from
 * input_core.c and does not call it. */
void xbox_enhance_pad_filter(DWORD port, const struct xbox_host_pad *host,
                             XBOX_INPUT_STATE *st);

/* ---- the menu's Mods page ---------------------------------------------
 *
 * The toolkit keeps the table, each init's result and the enable state.
 * `mods.<name>` (bool, default true) is the persisted key: a mod set false
 * is not inited at start. Index i is the table's. */
enum xbox_mod_state {
    XBOX_MOD_ON = 0,
    XBOX_MOD_OFF,           /* off now: by its key at start, or live */
    XBOX_MOD_FAILED,        /* init returned non-zero */
    XBOX_MOD_RESTART        /* the change applies at the next start */
};
int xbox_mod_count(void);                   /* entries in the table */
const char *xbox_mod_name(int i);
enum xbox_mod_state xbox_mod_state(int i);
const char *xbox_mod_state_name(enum xbox_mod_state s);   /* "on", ... */
int xbox_mod_has_on_enable(int i);
int xbox_mod_enabled(int i);                /* the key's value as set now */
/* The Mods page's toggle (any thread): through on_enable at the next
 * xbox_mod_frame when the mod gave one and runs, else recorded for the
 * next start. Returns the new state. */
enum xbox_mod_state xbox_mod_set_enabled(int i, int on);
/* The items mod i added with menu_add, in order; returns the count. */
int xbox_mod_items(int i, const struct enhance_item **out, int max);

/* ---- descriptor mods (the Mods page detail view, the dump) -------------
 *
 * xbox_mod_desc_of: mod i's descriptor, NULL for a v2 mod or a refused one
 * (xbox_mod_fail_reason says why). Settings are by index into it. */
const struct xbox_mod_desc *xbox_mod_desc_of(int i);
const char *xbox_mod_fail_reason(int i);    /* "" unless failed */
/* What the detail view lists: the descriptor of a valid mod, or of a
 * refused one whose tables passed the count checks (its rows are marked
 * unavailable; a name or a label may be NULL there). NULL otherwise. */
const struct xbox_mod_desc *xbox_mod_desc_shown(int i);
/* Setting idx's stock value: config.default.toml's key, then its aliases,
 * else def, adjusted as at load. A Save that stages it removes the key. */
double xbox_mod_setting_stock(int i, uint32_t idx);
/* v as the TOML literal the key is written with ("1.0", "true", "\"keep\"");
 * 0 for a bad index. */
int xbox_mod_setting_literal(int i, uint32_t idx, double v, char *out, size_t n);
double xbox_mod_setting_value(int i, uint32_t idx);
/* The precedence row the value came from: "env", "user", "user_alias",
 * "defaults", "defaults_alias", "default" (or "menu" once set live);
 * "link" for a LINK setting. */
const char *xbox_mod_setting_source(int i, uint32_t idx);
int xbox_mod_setting_forced(int i, uint32_t idx);   /* env-set: read-only */
const char *xbox_mod_setting_key(int i, uint32_t idx);   /* the full key */
/* The menu's change (any thread): clamped and snapped as at load, stored,
 * and the mod's on_setting queued for the next xbox_mod_frame (a RESTART
 * setting only records it). The enable setting switches the mod as
 * xbox_mod_set_enabled does. A LINK sets its item through the item's set,
 * and nothing is queued. Returns the value stored; refused (the old
 * value) when env forces it. */
double xbox_mod_setting_set(int i, uint32_t idx, double v);
/* What the init rule did this run, as the dump spells it: "yes",
 * "dormant", "no" or "failed". */
const char *xbox_mod_init_outcome(int i);
/* The enable setting's index, or -1 (none, or no descriptor). */
int xbox_mod_enable_setting(int i);
/* The action id of mod i's control idx, -1 for none. */
int xbox_mod_control_action(int i, uint32_t idx);

/* 1 when mod i has a descriptor, valid or refused (a refused one fails). */
int xbox_mod_has_desc(int i);
/* The env row bound to setting idx ("RECOMP_..."), or NULL. */
const char *xbox_mod_setting_env(int i, uint32_t idx);
/* Control idx's bindings as this run resolved them, into out (up to max),
 * and *src the precedence row's name ("default", "env", "user", ...): an
 * ACTION's are its action's, a BUTTON's or STICK's come from
 * [input.controls] "<id>.<control id>" (D3). Returns the count. */
int xbox_mod_control_bindings(int i, uint32_t idx, char out[][ENHANCE_ACTION_TEXT], int max,
                              const char **src);
/* The menu's rebinding of a BUTTON or STICK control (the menu's thread):
 * set_bindings stages n (1..ENHANCE_ACTION_BINDS) texts that control_value
 * reads at once, refused (0) for an env-forced control. dirty: they differ
 * from what was loaded or last saved; mark_saved after the Save wrote
 * them; revert goes back. stock is what a reset stages (the defaults
 * file's row, else the descriptor's defaults), is_stock whether the
 * bindings are those, key the full [input.controls] key. */
int xbox_mod_control_set_bindings(int i, uint32_t idx, const char *const *texts, int n);
int xbox_mod_control_dirty(int i, uint32_t idx);
void xbox_mod_control_mark_saved(int i, uint32_t idx);
void xbox_mod_control_revert(int i, uint32_t idx);
int xbox_mod_control_stock(int i, uint32_t idx, char out[][ENHANCE_ACTION_TEXT], int max);
int xbox_mod_control_is_stock(int i, uint32_t idx);
int xbox_mod_control_forced(int i, uint32_t idx);
const char *xbox_mod_control_key(int i, uint32_t idx);
/* Control id's index in mod i's descriptor, or -1. */
int xbox_mod_control_find(int i, const char *id);

/* ---- the status panels and glyph names (version 7) --------------------- */

/* Mod i's status text as set (at most XBOX_MOD_STATUS_LINES lines), into
 * out; "" when none, or while i is not on. Any thread. */
void xbox_mod_status_text(int i, char *out, size_t cap);
/* Moves whenever a status text is set, cleared or dropped. */
uint32_t xbox_mod_status_serial(void);
/* glyph_name's resolver (enhance_mod_status.c installs enhance_text's, with
 * the device in use, after the mods' inits): mod is the caller's table
 * index. It returns the length, -1 for an unbound ref, or
 * XBOX_MOD_GLYPH_CUT for a name cut to whole members. Without one,
 * glyph_name answers -1 with ref's own text. */
#define XBOX_MOD_GLYPH_CUT (-2)
typedef int (*xbox_mod_glyph_namer)(int mod, const char *ref, char *out, size_t cap,
                                    uint32_t flags);
void xbox_mod_set_glyph_namer(xbox_mod_glyph_namer fn);
/* 1 once a mod has called glyph_name or set a status: the device tracking
 * starts then, so a run without either stays stock. */
int xbox_mod_glyphs_used(void);

/* User-file aliases waiting to be rewritten under the current key (D4):
 * the menu's Save writes key = value and removes old. Returns the count;
 * fills up to max of each (pointers into the toolkit's own storage, valid
 * until the next xbox_mod_init). */
int xbox_mod_pending_rewrites(const char **keys, const char **values,
                              const char **olds, int max);

/* The dump's xbox_mod_init: resolve every descriptor, register and
 * dormant-mark the actions and compute the init rule, but run no init
 * (RECOMP_MODS_DESCRIBE). */
int xbox_mod_init_describe(void);

/* ---- the descriptor dump (mod_describe.c; D9) ---------------------------
 *
 * RECOMP_MODS_DESCRIBE=json asks xbox_enhance_init for the dump instead of
 * the start: xbox_mod_init_describe, the bindings built, the JSON written
 * to RECOMP_MODS_DESCRIBE_OUT (replaced) or stdout, and exit with the
 * write's result. */
/* 1 for "json"; any other set value logs one [MOD] line and gives 0. */
int xbox_mod_describe_wanted(void);
/* Schema 1 of every mod in the table, as xbox_mod_init_describe left them,
 * to path (NULL or "" for stdout). 0, or 1 with one [MOD] line when the file
 * cannot be written. */
int xbox_mod_describe_write(const char *path);
/* A number as the config files and the dump spell it: the C locale's '.',
 * the shortest text that reads back to v. Returns the length. */
int xbox_mod_num_text(char *out, size_t n, double v);

/* Removes the filter, stops the frame dispatch, then the mods' shutdowns,
 * in reverse order, and the capture released. Runs once; the exit paths
 * call it. A later xbox_mod_init starts over (tests). */
void xbox_mod_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_ENHANCE_MOD_H */
