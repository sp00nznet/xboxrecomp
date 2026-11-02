/*
 * enhance_menu.c - the in-game enhancements menu; see enhance_menu.h.
 *
 * Three parts:
 *   - the slots: one per item (the toolkit's, a title's, a mod's, plus the
 *     Mods page's enable toggle per mod), each with its value as text and
 *     the value as last loaded or saved, so Save writes only what moved;
 *   - the state machine: pages, rows, selection, capture, Save and Reset,
 *     driven by enhance_menu_event (the ctest calls it directly);
 *   - the hook: the host input into events (D4: first repeat at 150 ms,
 *     then every 60 ms), the neutral pad for the title, and the draw.
 *
 * Every step runs under the overlay's menu-step lock: the title's poll and
 * the flip tick may both call the hook, and only one of them may step.
 */
#include "enhance_menu.h"
#include "enhance_mod_status.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "enhance.h"
#include "enhance_actions.h"
#include "enhance_cfg.h"
#include "enhance_menu_input.h"
#include "enhance_menu_layout.h"
#include "enhance_osk.h"
#include "enhance_text.h"
#include "host_pad.h"
#include "hud_anchor.h"
#include "input_map.h"
#include "input_names.h"
#include "kernel_pacing.h"
#include "keyboard.h"
#include "mod.h"
#include "mouse.h"
#include "nv2a_backend_common.h"
#include "nv2a_overlay.h"
#include "nv2a_overlay_glyph.h"
#include "nv2a_overlay_int.h"
#include "nv2a_present_chain.h"
#include "nv2a_stats.h"
#include "nv2a_tex_pack.h"
#include "pad_map.h"
#include "pointer.h"
#include "shader_param_kinds.h"
#include "shader_profile_edit.h"
#include "recomp_cfg.h"
#include "recomp_env.h"
#include "text_input.h"

#define LOGP "[ENHANCE] "

#define PORTS        4
#define VAL_MAX      96
#define ROWS_MAX     192
#define REPEAT_FIRST 150u   /* D4: a held direction repeats after 150 ms */
#define REPEAT_NEXT  60u    /* ... then every 60 ms */
#define CHORD_MS     400u   /* a captured pad chord is the set held this long */
#define CAPTURE_MS   10000u /* a capture with no input gives up */
#define CLOSE_MS     1000u  /* the neutral pad after closing, at most */
#define LIVE_DRAW_MS 250u   /* a page of live numbers redraws this often */
#define STATUS_MS    3000u  /* a message stays in the footer this long */
#define PEEK_MS      1500u  /* Q10: LB/RB show the toolbar's labels this long */

enum page {
    PAGE_RENDER, PAGE_SHADERS, PAGE_PACK, PAGE_GAME, PAGE_MODS, PAGE_STATS, PAGE_CONTROLS,
    PAGE_DEBUG, PAGE_FILES, PAGE_COUNT
};
static const char *const s_page_name[PAGE_COUNT] = {
    "Render", "Shaders", "Pack", "Game", "Mods", "Stats", "Controls", "Debug", "Files"
};
/* Each section's intro line (A.8): the toolkit's below, a title's through
 * enhance_menu_page_intro. Held for the section header to draw. */
static const char *s_page_intro[PAGE_COUNT];
static const char *const k_page_intro[PAGE_COUNT] = {
    "How the game is drawn and shown: resolution, filtering, aspect and pacing.",
    "Post-processing shaders over the picture, saved as profiles.",
    "The texture pack: replacement art, font and text.",
    "Options this game adds.",
    "Mods built into this game: turn them on or off and set their options.",
    "Live numbers: frame rate, frame time and the draw counts.",
    "What each action is bound to, on the pad and the keyboard, and the mouse.",
    "The renderer's state and the trace channels, for bug reports.",
    "Where the settings are saved, and the way back to the defaults.",
};

/* ---- the toolkit's items ------------------------------------------------ */

static const char *const k_aniso[] = { "0", "2", "4", "8", "16", NULL };
static const char *const k_filter[] = { "nearest", "linear", "integer", NULL };
static const char *const k_pacing[] = { "spin", "sleep", NULL };
static const char *const k_aspect[] = { "4:3", "16:9", "16:10", "21:9", NULL };
static const char *const k_movie[] = { "black", "fit", "fill", NULL };
static const char *const k_surface[] = { "auto", "window", "internal", NULL };
static const char *const k_onoff[] = { "off", "on", NULL };
/* input.mouse_capture in the menu's order; the enum's is focus, click, never. */
static const char *const k_capture[] = { "click", "focus", "never", NULL };

/* pack_off as the menu last asked for it: the pack toggle and the two kind
 * toggles share the field, and a request only lands at the next flip, so
 * reading nv2a_host_opts back between two changes would lose the first. */
static unsigned s_pack_off;

static int on_text(const char *v)
{
    return v && (!strcmp(v, "true") || !strcmp(v, "on"));
}

static void req(void (*edit)(struct nv2a_host_opts *, const char *), const char *v,
                unsigned mask)
{
    struct nv2a_host_opts o = *nv2a_host_opts();
    edit(&o, v);
    nv2a_host_opts_request(&o, mask);
}

static void e_scale(struct nv2a_host_opts *o, const char *v) { o->render_scale = (unsigned)atoi(v); }
static void e_aniso(struct nv2a_host_opts *o, const char *v) { o->aniso = (unsigned)atoi(v); }
static void e_snap(struct nv2a_host_opts *o, const char *v) { o->snap_2d = on_text(v); }
static void e_full(struct nv2a_host_opts *o, const char *v) { o->fullscreen = on_text(v); }
static void e_overscan(struct nv2a_host_opts *o, const char *v) { o->overscan = (unsigned)atoi(v); }
static void e_pack(struct nv2a_host_opts *o, const char *v) { (void)v; o->pack_off = s_pack_off; }

static int index_of(const char *const *c, const char *v)
{
    for (int i = 0; c[i]; i++)
        if (!strcmp(c[i], v))
            return i;
    return -1;
}

static void e_filter(struct nv2a_host_opts *o, const char *v)
{
    int i = index_of(k_filter, v);
    o->present_filter = i < 0 ? NV2A_PRESENT_NEAREST : i;
}

static void e_movie(struct nv2a_host_opts *o, const char *v)
{
    int i = index_of(k_movie, v);
    o->movie_mode = i < 0 ? NV2A_MOVIE_BLACK : i;
}

static void set_scale(const struct enhance_item *it, const char *v) { (void)it; req(e_scale, v, NV2A_OPT_SCALE); }
static void set_aniso(const struct enhance_item *it, const char *v) { (void)it; req(e_aniso, v, NV2A_OPT_ANISO); }
static void set_snap(const struct enhance_item *it, const char *v) { (void)it; req(e_snap, v, NV2A_OPT_SNAP2D); }
static void set_filter(const struct enhance_item *it, const char *v) { (void)it; req(e_filter, v, NV2A_OPT_FILTER); }
static void set_full(const struct enhance_item *it, const char *v) { (void)it; req(e_full, v, NV2A_OPT_FULLSCREEN); }
static void set_overscan(const struct enhance_item *it, const char *v) { (void)it; req(e_overscan, v, NV2A_OPT_OVERSCAN); }
static void set_movie(const struct enhance_item *it, const char *v) { (void)it; req(e_movie, v, NV2A_OPT_MOVIE); }

static void set_pacing(const struct enhance_item *it, const char *v)
{
    (void)it;
    xbox_enhance_set_pacing(strcmp(v, "spin") ? XBOX_PACING_SLEEP : XBOX_PACING_SPIN);
}

/* The pack toggle and the kinds: bits of the one pack_off field. */
static void set_pack_bit(const struct enhance_item *it, const char *v)
{
    unsigned bit = (unsigned)(uintptr_t)it->user;
    if (on_text(v))
        s_pack_off &= ~bit;
    else
        s_pack_off |= bit;
    req(e_pack, v, NV2A_OPT_PACK);
}

static void set_stats(const struct enhance_item *it, const char *v)
{
    (void)it;
    nv2a_overlay_set_stats(on_text(v));
}

static void set_capture(const struct enhance_item *it, const char *v)
{
    xbox_mouse_opts o;
    int i = index_of(k_capture, v);
    (void)it;
    xbox_InputMouseGetOpts(&o);
    o.capture = i == 0 ? XBOX_MOUSE_CAPTURE_CLICK
              : i == 2 ? XBOX_MOUSE_CAPTURE_NEVER : XBOX_MOUSE_CAPTURE_FOCUS;
    /* Through the options ring: the window thread applies it. */
    xbox_InputMouseOpts(&o);
}

static const char *get_scale(const struct enhance_item *it, char *b, size_t n)
{
    (void)it;
    snprintf(b, n, "%u", nv2a_host_render_scale());
    return b;
}

static const char *get_aniso(const struct enhance_item *it, char *b, size_t n)
{
    (void)it;
    snprintf(b, n, "%u", nv2a_host_opts()->aniso);
    return b;
}

static const char *get_snap(const struct enhance_item *it, char *b, size_t n)
{
    (void)it;
    snprintf(b, n, "%s", nv2a_host_opts()->snap_2d ? "true" : "false");
    return b;
}

static const char *get_filter(const struct enhance_item *it, char *b, size_t n)
{
    int f = nv2a_host_opts()->present_filter;
    (void)it;
    snprintf(b, n, "%s", k_filter[f >= 0 && f <= 2 ? f : 0]);
    return b;
}

static const char *get_full(const struct enhance_item *it, char *b, size_t n)
{
    (void)it;
    snprintf(b, n, "%s", nv2a_host_opts()->fullscreen ? "true" : "false");
    return b;
}

static const char *get_pacing(const struct enhance_item *it, char *b, size_t n)
{
    (void)it;
    snprintf(b, n, "%s", xbox_PacingMode() == XBOX_PACING_SPIN ? "spin" : "sleep");
    return b;
}

static const char *get_overscan(const struct enhance_item *it, char *b, size_t n)
{
    (void)it;
    snprintf(b, n, "%u", nv2a_host_opts()->overscan);
    return b;
}

static const char *get_movie(const struct enhance_item *it, char *b, size_t n)
{
    int m = nv2a_host_opts()->movie_mode;
    (void)it;
    snprintf(b, n, "%s", k_movie[m >= 0 && m <= 2 ? m : 0]);
    return b;
}

static const char *get_pack_bit(const struct enhance_item *it, char *b, size_t n)
{
    unsigned bit = (unsigned)(uintptr_t)it->user;
    snprintf(b, n, "%s", (s_pack_off & bit) ? "off" : "on");
    if (it->kind == ENHANCE_ITEM_TOGGLE)
        snprintf(b, n, "%s", (s_pack_off & bit) ? "false" : "true");
    return b;
}

static const char *get_stats(const struct enhance_item *it, char *b, size_t n)
{
    (void)it;
    snprintf(b, n, "%s", nv2a_overlay_stats() ? "true" : "false");
    return b;
}

static const char *get_capture(const struct enhance_item *it, char *b, size_t n)
{
    xbox_mouse_opts o;
    (void)it;
    xbox_InputMouseGetOpts(&o);
    snprintf(b, n, "%s", o.capture == XBOX_MOUSE_CAPTURE_CLICK ? "click"
                         : o.capture == XBOX_MOUSE_CAPTURE_NEVER ? "never" : "focus");
    return b;
}

/* display.hud_margin: a plain store the HUD layer reads per call. */
static void set_hud_margin(const struct enhance_item *it, const char *v)
{
    (void)it;
    hud_layer_set_margin(atoi(v));
}

static const char *get_hud_margin(const struct enhance_item *it, char *b, size_t n)
{
    (void)it;
    snprintf(b, n, "%d", hud_layer_margin());
    return b;
}

#define ITEM(k, l, p, kd) .key = k, .label = l, .page = p, .kind = kd

/* Listed only when a mod registered a HUD table (build_slots): without
 * one the key changes nothing. */
static const struct enhance_item k_hud_margin = {
    ITEM("display.hud_margin", "HUD margin", "Render", ENHANCE_ITEM_RANGE), .lo = 0, .hi = 64,
    .step = 4, .note = "wide aspect", .set = set_hud_margin, .get = get_hud_margin,
    .live = ENHANCE_LIVE, .def = "0",
    .help = "Pulls the HUD in from the screen edges on a wide picture"
};

static const struct enhance_item k_items[] = {
    { ITEM("render.scale", "Render scale", "Render", ENHANCE_ITEM_RANGE), .lo = 1, .hi = 4,
      .step = 1, .set = set_scale, .get = get_scale, .live = ENHANCE_LIVE_FLUSH, .def = "1",
      .help = "Draws the 3D at a multiple of the Xbox resolution: sharper, slower" },
    { ITEM("render.snap_2d", "2D on guest grid", "Render", ENHANCE_ITEM_TOGGLE),
      .note = "scale 2+", .set = set_snap, .get = get_snap, .def = "false",
      .help = "Keeps 2D art on the Xbox pixel grid so scaled menus do not shimmer" },
    { ITEM("render.aniso", "Anisotropic filtering", "Render", ENHANCE_ITEM_CHOICE),
      .choices = k_aniso, .set = set_aniso, .get = get_aniso, .flags = ENHANCE_ITEM_NUMERIC,
      .def = "0", .help = "Sharper textures on surfaces seen at an angle" },
    { ITEM("present.filter", "Scaling filter", "Render", ENHANCE_ITEM_CHOICE),
      .choices = k_filter, .set = set_filter, .get = get_filter, .def = "nearest",
      .help = "How the picture is stretched to the window: nearest is crisp, linear smooth" },
    { ITEM("present.fullscreen", "Fullscreen", "Render", ENHANCE_ITEM_TOGGLE), .set = set_full,
      .get = get_full, .live = ENHANCE_LIVE_FLUSH, .def = "false",
      .help = "Fills the screen with a borderless window" },
    { ITEM("present.pacing", "Frame pacing", "Render", ENHANCE_ITEM_CHOICE),
      .choices = k_pacing, .set = set_pacing, .get = get_pacing, .def = "sleep",
      .help = "How the frame rate is held steady: sleep saves power, spin is tighter" },
    { ITEM("display.aspect", "Aspect ratio", "Render", ENHANCE_ITEM_CHOICE),
      .choices = k_aspect, .live = ENHANCE_LIVE_RESTART, .def = "4:3",
      .help = "The picture shape the game is told to draw for" },
    { ITEM("display.overscan", "Overscan crop", "Render", ENHANCE_ITEM_RANGE), .lo = 0,
      .hi = NV2A_OVERSCAN_MAX, .step = 4, .note = "4:3 only", .set = set_overscan,
      .get = get_overscan, .def = "0",
      .help = "Crops the border a TV would hide, in pixels per side" },
    { ITEM("display.movie", "Movies", "Render", ENHANCE_ITEM_CHOICE), .choices = k_movie,
      .set = set_movie, .get = get_movie, .def = "black",
      .help = "What fills the screen around full-motion video" },
    { ITEM("present.surface", "Present surface", "Render", ENHANCE_ITEM_CHOICE),
      .choices = k_surface, .live = ENHANCE_LIVE_RESTART, .def = "auto",
      .help = "The window surface the picture is shown on; auto picks the best one" },
    /* The atlas is baked at the first overlay arm, so a new scale waits
     * for the next run. */
    { ITEM("overlay.ui_scale", "Overlay scale", "Render", ENHANCE_ITEM_RANGE), .lo = 0.75,
      .hi = 2.0, .step = 0.25, .live = ENHANCE_LIVE_RESTART, .def = "1",
      .help = "Sizes the menu and the overlays: larger for a TV across the room" },
    /* textures.pack names the file: the toggle is this session's only. */
    { ITEM("textures.pack", "Texture pack", "Pack", ENHANCE_ITEM_TOGGLE), .set = set_pack_bit,
      .get = get_pack_bit, .live = ENHANCE_LIVE_FLUSH, .flags = ENHANCE_ITEM_NOSAVE,
      .def = "true", .user = (void *)(uintptr_t)NV2A_PACK_OFF_ALL,
      .help = "Uses the replacement textures for this session" },
    { ITEM("textures.font", "Pack font", "Pack", ENHANCE_ITEM_CHOICE), .choices = k_onoff,
      .set = set_pack_bit, .get = get_pack_bit, .live = ENHANCE_LIVE_FLUSH, .def = "on",
      .user = (void *)(uintptr_t)NV2A_PACK_OFF_FONT,
      .help = "Uses the pack's font in place of the game's" },
    { ITEM("textures.text", "Pack text", "Pack", ENHANCE_ITEM_CHOICE), .choices = k_onoff,
      .set = set_pack_bit, .get = get_pack_bit, .live = ENHANCE_LIVE_FLUSH, .def = "on",
      .user = (void *)(uintptr_t)NV2A_PACK_OFF_TEXT,
      .help = "Uses the pack's text images in place of the game's" },
    { ITEM("overlay.stats", "Stats strip", "Stats", ENHANCE_ITEM_TOGGLE), .set = set_stats,
      .get = get_stats, .def = "false",
      .help = "Shows frame time and rate in a strip over the game" },
    { ITEM("input.mouse_capture", "Capture", "Controls", ENHANCE_ITEM_CHOICE),
      .choices = k_capture, .set = set_capture, .get = get_capture,
      .def = "focus", .help = "When the game takes the mouse: on focus, on a click, or never" },
};
#define N_ITEMS ((int)(sizeof k_items / sizeof k_items[0]))

/* ---- slots ---------------------------------------------------------------- */

struct slot {
    const struct enhance_item *it;
    char key[64];
    char cur[VAL_MAX];          /* the value in force as the menu sees it */
    char base[VAL_MAX];         /* as loaded or last saved */
    int page;
    int mod;                    /* -1, or the mod an item or toggle is for */
    int toggle;                 /* the mod's enable toggle */
    int env;                    /* forced by the environment: read only */
    int dset;                   /* a descriptor setting's index, else -1 */
    const char *envname;        /* the env row that forces it, or NULL */
    const char *help;           /* the footer's text for its row, or NULL */
};

#define MODS_MAX 32
#define DSET_MAX 256            /* descriptor settings over every mod */
#define SLOTS_MAX (ENHANCE_MENU_MAX_ITEMS + 2 * XBOX_MOD_MAX_ITEMS + MODS_MAX + DSET_MAX + 16)
static struct slot s_slot[SLOTS_MAX];
/* The Mods page's toggle per mod, synthesized at the first open: its key
 * is mods.<name>, or for a descriptor mod with an enable setting that
 * setting's key (D4), which Save writes in the setting's own type. */
static struct enhance_item s_mod_toggle[MODS_MAX];
static char s_mod_key[MODS_MAX][64];
static int s_nslot, s_built;
static const struct enhance_item *s_title[ENHANCE_MENU_MAX_ITEMS];
static int s_ntitle;

int enhance_menu_add(const struct enhance_item *it)
{
    if (!it || !it->key || !it->label || s_ntitle >= ENHANCE_MENU_MAX_ITEMS)
        return 0;
    s_title[s_ntitle++] = it;
    return 1;
}

static int page_by_name(const char *name, int def)
{
    for (int p = 0; name && p < PAGE_COUNT; p++)
        if (!strcmp(name, s_page_name[p]))
            return p;
    return def;
}

/* The value as the config has it, as text in the item's own spelling. */
static void cfg_value(const struct enhance_item *it, const char *key, char *out, size_t n)
{
    const char *def = it->def ? it->def : "";
    switch (it->kind) {
    case ENHANCE_ITEM_TOGGLE:
        snprintf(out, n, "%s", enhance_cfg_bool(key, on_text(def)) ? "true" : "false");
        break;
    case ENHANCE_ITEM_CHOICE:
        if (it->flags & ENHANCE_ITEM_NUMERIC)
            snprintf(out, n, "%lld", enhance_cfg_int(key, atoll(def)));
        else
            snprintf(out, n, "%s", enhance_cfg_string(key, def));
        break;
    case ENHANCE_ITEM_RANGE:
        snprintf(out, n, "%g", enhance_cfg_float(key, atof(def)));
        break;
    default:
        out[0] = 0;
        break;
    }
}

static void slot_read(struct slot *s)
{
    char buf[VAL_MAX];
    const char *v = s->it->get ? s->it->get(s->it, buf, sizeof buf) : NULL;
    if (v)
        snprintf(s->cur, sizeof s->cur, "%s", v);
    else
        cfg_value(s->it, s->key, s->cur, sizeof s->cur);
}

static struct slot *slot_add(const struct enhance_item *it, int page, int mod)
{
    struct slot *s;
    if (s_nslot >= SLOTS_MAX)
        return NULL;
    s = &s_slot[s_nslot++];
    memset(s, 0, sizeof *s);
    s->dset = -1;
    s->it = it;
    snprintf(s->key, sizeof s->key, "%s", it->key);
    s->page = page;
    s->mod = mod;
    s->env = enhance_cfg_source_of(s->key) == ENHANCE_SRC_ENV;
    slot_read(s);
    memcpy(s->base, s->cur, sizeof s->base);
    return s;
}

static void mod_toggle_set(const struct enhance_item *it, const char *v)
{
    xbox_mod_set_enabled((int)(intptr_t)it->user, on_text(v));
}

static const char *mod_toggle_get(const struct enhance_item *it, char *b, size_t n)
{
    snprintf(b, n, "%s", xbox_mod_enabled((int)(intptr_t)it->user) ? "true" : "false");
    return b;
}

/* ---- descriptor settings (D2): one slot each, on the mod's page ---------- */

/* The slot's text is the value as the row shows it, without the unit: a
 * bool true/false, an enum its choice, a number at the step's decimals,
 * so a value the toolkit snapped reads back as the text it came from. */
static struct enhance_item s_set_item[DSET_MAX];
static int s_nset;

static const struct xbox_mod_setting *dset_of(const struct enhance_item *it, int *m,
                                              uint32_t *k)
{
    int u = (int)(intptr_t)it->user;
    *m = u >> 8;
    *k = (uint32_t)(u & 0xff);
    return &xbox_mod_desc_of(*m)->settings[*k];
}

/* A float with no step moves by a hundredth of its range. */
static double dset_step(const struct xbox_mod_setting *st)
{
    if (st->step > 0)
        return st->step;
    if (st->kind == XBOX_SET_INT || !(st->hi > st->lo))
        return 1;
    return (st->hi - st->lo) / 100;
}

/* The decimals a step needs: 0.05 shows 1.00, 0.5 shows 1.5. With no step
 * the computed one decides (0..0.01 moves by 0.0001: 4), so the text, which
 * is also the saved literal, is the value in use rather than a rounding. */
static int dset_decimals(const struct xbox_mod_setting *st)
{
    double x = dset_step(st);
    int d = 0;
    while (d < 6 && fabs(x - floor(x + 0.5)) > 1e-6) {
        x *= 10;
        d++;
    }
    return d;
}

static void dset_text(const struct xbox_mod_setting *st, double v, char *out, size_t n)
{
    int c = (int)v;
    switch (st->kind) {
    case XBOX_SET_BOOL:
        snprintf(out, n, "%s", v ? "true" : "false");
        return;
    case XBOX_SET_ENUM:
        snprintf(out, n, "%s", st->choices && c >= 0 ? st->choices[c] : "");
        return;
    case XBOX_SET_INT:
        snprintf(out, n, "%lld", (long long)v);
        return;
    default:
        snprintf(out, n, "%.*f", dset_decimals(st), v);
        return;
    }
}

static double dset_parse(const struct xbox_mod_setting *st, const char *v)
{
    int c;
    switch (st->kind) {
    case XBOX_SET_BOOL:
        return on_text(v);
    case XBOX_SET_ENUM:
        c = index_of(st->choices, v);
        return c < 0 ? 0 : c;
    default:
        return atof(v);
    }
}

static void dset_set(const struct enhance_item *it, const char *v)
{
    int m;
    uint32_t k;
    const struct xbox_mod_setting *st = dset_of(it, &m, &k);
    xbox_mod_setting_set(m, k, dset_parse(st, v));
}

static const char *dset_get(const struct enhance_item *it, char *b, size_t n)
{
    int m;
    uint32_t k;
    const struct xbox_mod_setting *st = dset_of(it, &m, &k);
    dset_text(st, xbox_mod_setting_value(m, k), b, n);
    return b;
}

/* Mod m's settings but its switch (the Enabled row is that), in the
 * descriptor's order. A failed mod gets none: its page lists them as
 * unavailable. */
static void dset_slots(int m)
{
    const struct xbox_mod_desc *d = xbox_mod_desc_of(m);
    if (!d || xbox_mod_state(m) == XBOX_MOD_FAILED)
        return;
    for (uint32_t k = 0; k < d->n_settings; k++) {
        const struct xbox_mod_setting *st = &d->settings[k];
        struct enhance_item *it;
        struct slot *s;
        if ((st->flags & XBOX_SET_ENABLE) || !xbox_mod_setting_key(m, k))
            continue;
        if (s_nset >= DSET_MAX) {
            fprintf(stderr, LOGP "menu: more than %d mod settings; %s not listed\n", DSET_MAX,
                    xbox_mod_setting_key(m, k));
            continue;
        }
        it = &s_set_item[s_nset++];
        memset(it, 0, sizeof *it);
        it->key = xbox_mod_setting_key(m, k);
        it->label = st->label ? st->label : st->name;
        it->kind = st->kind == XBOX_SET_BOOL   ? ENHANCE_ITEM_TOGGLE
                 : st->kind == XBOX_SET_ENUM ? ENHANCE_ITEM_CHOICE
                                             : ENHANCE_ITEM_RANGE;
        it->choices = st->choices;
        it->lo = st->lo;
        it->hi = st->hi;
        it->step = dset_step(st);
        it->set = dset_set;
        it->get = dset_get;
        it->live = (enum enhance_item_live)st->live;
        it->user = (void *)(intptr_t)(m << 8 | (int)k);
        s = slot_add(it, PAGE_MODS, m);
        if (!s)
            break;
        s->dset = (int)k;
        s->help = st->help;
        s->env = xbox_mod_setting_forced(m, k);
        s->envname = s->env ? xbox_mod_setting_env(m, k) : NULL;
    }
}

/* Mod m's slot for setting k, -1 when it has none. */
static int dset_slot(int m, uint32_t k)
{
    for (int i = 0; i < s_nslot; i++)
        if (s_slot[i].mod == m && s_slot[i].dset == (int)k)
            return i;
    return -1;
}

static void build_slots(void)
{
    s_nslot = 0;
    s_nset = 0;
    s_pack_off = nv2a_host_opts()->pack_off;
    for (int i = 0; i < N_ITEMS; i++)
        slot_add(&k_items[i], page_by_name(k_items[i].page, PAGE_RENDER), -1);
    if (hud_layer_has_mod_table())
        slot_add(&k_hud_margin, PAGE_RENDER, -1);
    for (int i = 0; i < s_ntitle; i++)
        slot_add(s_title[i], page_by_name(s_title[i]->page, PAGE_GAME), -1);
    for (int m = 0; m < xbox_mod_count() && m < MODS_MAX; m++) {
        const struct enhance_item *items[XBOX_MOD_MAX_ITEMS];
        int n = xbox_mod_items(m, items, XBOX_MOD_MAX_ITEMS);
        struct enhance_item *t = &s_mod_toggle[m];
        int e = xbox_mod_enable_setting(m);
        if (e >= 0)
            snprintf(s_mod_key[m], sizeof s_mod_key[m], "%s",
                     xbox_mod_setting_key(m, (uint32_t)e));
        else
            snprintf(s_mod_key[m], sizeof s_mod_key[m], "mods.%s", xbox_mod_name(m));
        memset(t, 0, sizeof *t);
        t->key = s_mod_key[m];
        t->label = xbox_mod_name(m);
        t->kind = ENHANCE_ITEM_TOGGLE;
        t->set = mod_toggle_set;
        t->get = mod_toggle_get;
        /* Without on_enable the mod reads its key at init only. */
        t->live = xbox_mod_has_on_enable(m) ? ENHANCE_LIVE : ENHANCE_LIVE_RESTART;
        t->def = "true";
        t->user = (void *)(intptr_t)m;
        slot_add(t, PAGE_MODS, m);
        if (s_nslot) {
            struct slot *ts = &s_slot[s_nslot - 1];
            ts->toggle = 1;
            /* The switch is the enable setting: name its variable when that
             * forces it, as the setting rows do (D5). */
            if (e >= 0 && xbox_mod_setting_forced(m, (uint32_t)e)) {
                ts->env = 1;
                ts->envname = xbox_mod_setting_env(m, (uint32_t)e);
            }
        }
        dset_slots(m);
        for (int i = 0; i < n; i++)
            slot_add(items[i], PAGE_MODS, m);
    }
    s_built = 1;
}

static int slot_saveable(const struct slot *s)
{
    return !(s->it->flags & ENHANCE_ITEM_NOSAVE) && !s->env
        && s->it->kind != ENHANCE_ITEM_INFO && s->it->kind != ENHANCE_ITEM_ACTION;
}

/* The stock value: the defaults file's, else the item's code default. 0
 * when neither has one. */
static double enable_stock(int m, int e, const char *key);

static int slot_stock(const struct slot *s, char *out, size_t n)
{
    const recomp_cfg *d = enhance_cfg_defaults();
    const struct enhance_item *it = s->it;
    if (s->dset >= 0) {
        int m;
        uint32_t k;
        dset_text(dset_of(it, &m, &k), xbox_mod_setting_stock(m, k), out, n);
        return 1;
    }
    if (s->toggle && xbox_mod_enable_setting(s->mod) >= 0) {
        int e = xbox_mod_enable_setting(s->mod);
        snprintf(out, n, "%s", enable_stock(s->mod, e, s->key) ? "true" : "false");
        return 1;
    }
    if (d && recomp_cfg_type_of(d, s->key) != RCFG_NONE) {
        switch (it->kind) {
        case ENHANCE_ITEM_TOGGLE:
            snprintf(out, n, "%s", recomp_cfg_bool(d, s->key, 0) ? "true" : "false");
            return 1;
        case ENHANCE_ITEM_CHOICE:
            if (it->flags & ENHANCE_ITEM_NUMERIC)
                snprintf(out, n, "%lld", recomp_cfg_int(d, s->key, 0));
            else
                snprintf(out, n, "%s", recomp_cfg_string(d, s->key, ""));
            return 1;
        case ENHANCE_ITEM_RANGE:
            snprintf(out, n, "%g", recomp_cfg_float(d, s->key, 0));
            return 1;
        default:
            break;
        }
    }
    if (!it->def)
        return 0;
    snprintf(out, n, "%s", it->def);
    return 1;
}

static void slot_set(struct slot *s, const char *v)
{
    snprintf(s->cur, sizeof s->cur, "%s", v);
    /* A RESTART item's set() records the value for the next start (a
     * mod's toggle); most have none and the value is only stored. */
    if (s->it->set)
        s->it->set(s->it, s->cur);
    /* A setting comes back clamped and snapped: show what is in force. */
    if (s->dset >= 0)
        slot_read(s);
}

/* ---- actions the Controls page changed ---------------------------------- */

typedef char bind_list[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];

/* Each action's bindings as loaded or last saved, as the slots keep base:
 * an action is unsaved while its bindings differ from these, so binding a
 * key and then binding it back leaves nothing to save (D10). */
static bind_list s_abase[ENHANCE_ACTIONS_MAX];
static int s_abase_n[ENHANCE_ACTIONS_MAX];
static unsigned char s_abase_have[ENHANCE_ACTIONS_MAX];

/* The stock bindings: the defaults file's [input.actions] entry, else the
 * code defaults. */
static int action_stock(int id, bind_list out)
{
    const recomp_cfg *d = enhance_cfg_defaults();
    char key[96];
    int n = 0;
    snprintf(key, sizeof key, "input.actions.%s", enhance_action_name(id));
    if (d && recomp_cfg_type_of(d, key) == RCFG_ARRAY) {
        size_t len = recomp_cfg_array_len(d, key);
        for (size_t i = 0; i < len && n < ENHANCE_ACTION_BINDS; i++)
            snprintf(out[n++], ENHANCE_ACTION_TEXT, "%s",
                     recomp_cfg_array_string(d, key, i, ""));
        return n;
    }
    return enhance_action_default_bindings(id, out, ENHANCE_ACTION_BINDS);
}

static int lists_equal(bind_list a, int na, bind_list b, int nb)
{
    if (na != nb)
        return 0;
    for (int i = 0; i < na; i++)
        if (strcmp(a[i], b[i]))
            return 0;
    return 1;
}

static int list_has(bind_list l, int n, const char *v)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(l[i], v))
            return 1;
    return 0;
}

/* The same set in any order: a capture appends, so clearing a key and
 * binding it again puts it last, and that changes nothing the player sees. */
static int same_binds(bind_list a, int na, bind_list b, int nb)
{
    if (na != nb)
        return 0;
    for (int i = 0; i < na; i++)
        if (!list_has(b, nb, a[i]) || !list_has(a, na, b[i]))
            return 0;
    return 1;
}

static void abase_take(int id)
{
    s_abase_n[id] = enhance_action_bindings(id, s_abase[id], ENHANCE_ACTION_BINDS);
    s_abase_have[id] = 1;
}

static int binds_dirty(int id)
{
    bind_list cur;
    int n;
    if (id < 0 || id >= ENHANCE_ACTIONS_MAX || !s_abase_have[id] || !enhance_action_name(id))
        return 0;
    n = enhance_action_bindings(id, cur, ENHANCE_ACTION_BINDS);
    return !same_binds(cur, n, s_abase[id], s_abase_n[id]);
}

static void action_set(int id, bind_list l, int n)
{
    const char *t[ENHANCE_ACTION_BINDS];
    if (id >= 0 && id < ENHANCE_ACTIONS_MAX && !s_abase_have[id])
        abase_take(id);
    for (int i = 0; i < n; i++)
        t[i] = l[i];
    enhance_action_set_bindings(id, t, n);
}

/* Two values of one item as the player sees them: a range within half a
 * step (0.5 and 0.50 or a snapped 0.4999), a number by value, a toggle as
 * on or off, anything else as its text. */
static int vals_same(const struct enhance_item *it, const char *a, const char *b)
{
    switch (it->kind) {
    case ENHANCE_ITEM_TOGGLE:
        return on_text(a) == on_text(b);
    case ENHANCE_ITEM_RANGE:
        return fabs(atof(a) - atof(b)) < (it->step > 0 ? it->step : 1e-6) / 2;
    case ENHANCE_ITEM_CHOICE:
        if (it->flags & ENHANCE_ITEM_NUMERIC)
            return atoll(a) == atoll(b);
        return !strcmp(a, b);
    default:
        return !strcmp(a, b);
    }
}

static int slot_unsaved(const struct slot *s)
{
    return !vals_same(s->it, s->cur, s->base);
}

static const char *mod_title(int m);

/* Who an action belongs to, as the Controls page heads its group:
 * "Free camera: Toggle flight" for a mod's, the label alone otherwise. */
static void action_who(int id, char *out, size_t n)
{
    int g = enhance_action_mod(id);
    const char *label = enhance_action_label(id);
    if (g >= 0)
        snprintf(out, n, "%s: %s", mod_title(g), label ? label : "?");
    else
        snprintf(out, n, "%s", label ? label : "another action");
}

/* A row value's glyph form (D8) grows beside its text: a binding as a
 * {kind:name} token, words as they are. A name with a brace in it would
 * end the token early, so it stays text. An overflow empties the form,
 * and the row then draws its text. */
struct vbuf {
    char *p;
    size_t n, at;
};

static void vb_put(struct vbuf *b, const char *kind, const char *s)
{
    int k;
    if (!b->p || b->at >= b->n)
        return;
    k = kind && !strpbrk(s, "{}") ? snprintf(b->p + b->at, b->n - b->at, "{%s:%s}", kind, s)
                                 : snprintf(b->p + b->at, b->n - b->at, "%s", s);
    b->at += (size_t)k;
    if (b->at >= b->n) {
        b->p[0] = 0;
        b->n = 0;
    }
}

/* The bindings in effect, and each one the table refuses marked with why:
 * a player who sees "F6" must not think F6 does it when another action
 * holds F6 (one input fires one action). glyph (NULL for none) takes the
 * same as tokens. */
static void join_bindings(int id, char *out, size_t n, char *glyph, size_t gn)
{
    bind_list l, bad;
    int k = enhance_action_bindings(id, l, ENHANCE_ACTION_BINDS);
    int nb = enhance_action_refused(id, bad, ENHANCE_ACTION_BINDS);
    size_t at = 0;
    struct vbuf g = { glyph, gn, 0 };
    out[0] = 0;
    if (glyph && gn)
        glyph[0] = 0;
    if (!k) {
        snprintf(out, n, "none");
        return;
    }
    for (int i = 0; i < k && at < n; i++) {
        int refused = 0;
        char tail[160] = "";
        for (int j = 0; j < nb && !refused; j++)
            refused = !strcmp(l[i], bad[j]);
        if (refused) {
            char bound[ENHANCE_ACTION_TEXT], who[96];
            int o = enhance_action_owner(l[i], id, bound, sizeof bound);
            if (o >= 0) {
                action_who(o, who, sizeof who);
                snprintf(tail, sizeof tail, " (refused: %s has it)", who);
            } else {
                snprintf(tail, sizeof tail, " (refused)");
            }
        }
        at += (size_t)snprintf(out + at, n - at, "%s%s%s", i ? ", " : "", l[i], tail);
        vb_put(&g, NULL, i ? ", " : "");
        vb_put(&g, "bind", l[i]);
        vb_put(&g, NULL, tail);
    }
}

/* ---- state --------------------------------------------------------------- */

enum { ST_CLOSED, ST_OPEN, ST_CLOSING };
enum row_type {
    R_SLOT, R_INFO, R_HEADER, R_BIND, R_MOD, R_RESET,
    R_FILEIO,           /* Files: import (ref 0) or export (1), hidden for now */
    R_TEXT,             /* one wrapped line of a mod's Author, About or State */
    R_CTL,              /* a mod's captured control (BUTTON, STICK): rebound by capture */
    R_MODRESET,         /* "Reset this mod" */
    R_CTLRESET,         /* "Reset <mod> controls": its actions and captured controls */
    R_PARAM,            /* a shader parameter: ref indexes s_ptab */
    R_PRESET,           /* "Reset to preset" */
    R_PROFILE,          /* the Shaders page's Profile picker */
    R_STAGE,            /* a chain stage: ref is its index */
    R_ADD,              /* "Add shader" */
    R_PICK,             /* a preset in the Add picker: ref indexes s_pick */
    R_ACT,              /* a profile or stage action: ref is ACT_* */
    R_NAME              /* "Rename": the text entry */
};
enum { TINT_NONE, TINT_BAD, TINT_WARN };
/* The Files page's import and export rows wait for the file browser. */
static const int k_files_io = 0;

struct row {
    int type;
    int ref;            /* slot, action id, mod index; R_CTL mod << 8 | control */
    int sel;
    char label[64];
    char value[VAL_MAX];
    /* A binding row's value as tokens (D8), drawn as glyphs when they fit
     * (bind_value); "" for every other row. */
    char glyph[VAL_MAX * 2];
    int tint;           /* an info or text value's colour: TINT_* */
    const char *note;   /* dim words after the label ("while flying"), or NULL */
    int dim;            /* a row that does nothing now (Add at the limit) */
    char help[160];     /* the footer's text while it is selected */
};

static int s_state = ST_CLOSED;     /* written under the lock, read relaxed */
static int s_page;
static int s_sel[PAGE_COUNT];
/* The Shaders page is left out where no shader can run (the user,
 * 2026-10-29): no chain presenter, or librashader missing or failing to
 * load (a dll it imports is absent; the log names it). Decided at
 * each open; the reason is said once. A supported build shows it with
 * the profile Off, since the picker lives there. */
static int s_shaders_off;
static int s_shaders_said;

static int page_shown(int p)
{
    return p != PAGE_SHADERS || !s_shaders_off;
}

static void shaders_check(void)
{
    char why[700];
    s_shaders_off = !nv2a_present_chain_supported(why, sizeof why);
    if (s_shaders_off && !s_shaders_said) {
        fprintf(stderr, "[SHADER] the menu's Shaders page is hidden: %s\n", why);
        s_shaders_said = 1;
    }
    if (!page_shown(s_page))
        s_page = PAGE_RENDER;
}
static int s_scroll[PAGE_COUNT];
static int s_mod_sub = -1;          /* the mod whose items the Mods page lists */
static int s_confirm;               /* the Reset row asks again */
/* D9a: the unsaved-changes dialog; while on it takes every event. */
enum { DLG_DISCARD, DLG_SAVE, DLG_BACK, DLG_BUTTONS };
#define DLG_SWALLOW_MS 150u  /* = REPEAT_FIRST: the key that opened it waits */

static struct {
    int on, focus;
    int failed;             /* Exit and Save failed: inert until reopened */
    uint64_t t0;
    char err[160];          /* a failed Exit and Save's reason */
    char body[512];
} s_dlg;

static int s_hints;                 /* enum menu_hints (D11), overlay.menu_hints */
static int s_size;                  /* enum menu_size (D2), overlay.menu_size */
static int s_focus;                 /* enum menu_focus (D3): where the pad and keys act */
static int s_foot_sel;              /* MENU_FB_* while the footer has the focus */
static uint64_t s_peek_t;           /* the last section change, 0 for none */
static int s_peek_drawn;            /* the draw after the peek ran out is done */
static int s_need_draw;
static char s_status[128];
static uint64_t s_status_t, s_now;

static struct row s_row[ROWS_MAX];
static int s_nrow;

/* capture */
enum { CAP_OFF, CAP_RELEASE, CAP_ARMED };
/* What the capture binds: an action (id its id), or a mod's captured
 * control (id the R_CTL ref), a BUTTON or a STICK row. */
enum { CAP_ACTION, CAP_CTL_BUTTON, CAP_CTL_STICK };
static struct {
    int phase, id, what;
    uint64_t t0, t_set;
    uint32_t max, last;
    unsigned clicks;        /* XBOX_POINTER_* pressed while armed (pointer_step) */
} s_cap;

/* Every phase change goes through here: while a capture runs the actions
 * fire nothing, so the input being captured never also does what it is
 * bound to now. */
static void cap_phase(int phase)
{
    s_cap.phase = phase;
    s_cap.clicks = 0;
    enhance_actions_quiet(phase != CAP_OFF);
}

/* input */
static uint32_t s_held[PORTS], s_prev_held;
static uint32_t s_rel_armed, s_rel_taint;  /* View and L3 wait for release */
static unsigned char s_kprev[256];
static DWORD s_pkt[PORTS], s_fpkt[PORTS];
static int s_dir, s_kbd;
/* The mouse leads (D3): set by real motion, a click or the wheel, cleared
 * by the first pad or key event; the latest device is MOUSE while set, else
 * KEYBOARD when s_kbd, else PAD. */
static int s_mouse;
/* A key has reached the menu since the game started: the player has a
 * physical keyboard, so a rename types there and the OSK stays shut. */
static int s_kbd_seen;
/* The pointer (D3): the last snapshot's sequence, where real motion was
 * last measured from, the held buttons, a drag on a slider row, and the
 * wheel's own scroll (the selection stays; the next key re-anchors it). */
static uint32_t s_ptr_seq;
static int s_ptr_ax, s_ptr_ay, s_ptr_x = -1, s_ptr_y = -1;
static unsigned s_ptr_btn;
static int s_drag_row = -1;
static int s_free_scroll;
static int s_hover_row = -1;
/* D11's tooltip: when the selection or the hover last settled. */
static uint64_t s_tip_t;
static int s_tip_sel = -1, s_tip_page = -1;
/* The layout the last draw used, for the hit test and a text field's rect. */
static struct menu_layout s_lay;
static int s_lay_ok;
static int s_tip_drawn;             /* the last draw showed the card */

/* RECOMP_TRACE=menu_input (C.9). */
#define MTRACE(...) \
    do { \
        if (recomp_env(RENV_MENU_INPUT_TRACE)) { \
            fprintf(stderr, LOGP "menu_input: " __VA_ARGS__); \
            fflush(stderr); \
        } \
    } while (0)

static const char *const k_dev_name[] = {"pad", "keyboard", "mouse"};
static const char *const k_focus_name[] = {"content", "toolbar", "footer", "header"};

static int menu_device(void)
{
    return s_mouse ? ENHANCE_DEV_MOUSE : s_kbd ? ENHANCE_DEV_KEYBOARD : ENHANCE_DEV_PAD;
}

/* A pad (kbd 0) or key (1) event: that device leads, and the cursor hides
 * until the mouse moves again (C.6). */
static void dev_set(int kbd)
{
    int was = menu_device();
    s_mouse = 0;
    s_kbd = kbd != 0;
    if (kbd)
        s_kbd_seen = 1;
    xbox_InputMouseCursorHide(1);
    if (menu_device() != was) {
        MTRACE("device %s\n", k_dev_name[menu_device()]);
        s_need_draw = 1;
    }
}

static void dev_mouse(void)
{
    if (!s_mouse) {
        MTRACE("device mouse\n");
        s_need_draw = 1;
    }
    s_mouse = 1;
    xbox_InputMouseCursorHide(0);
}

static int any_key_down(void);

static void focus_to(int f)
{
    if (f == s_focus)
        return;
    MTRACE("focus %s -> %s\n", k_focus_name[s_focus], k_focus_name[f]);
    s_focus = f;
    s_need_draw = 1;
}
static uint64_t s_dir_next, s_close_t, s_draw_t;
static uint64_t s_extra_t[INPUT_SRC_COUNT];
static int s_snap_on, s_stats_on;
static int s_draw_w, s_draw_h;

/* rates over one-second windows (Stats, Debug) */
static uint64_t s_rate_t0, s_rate_c0[NV2A_STAT_COUNT];
static float s_rate[NV2A_STAT_COUNT];

static void status(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
static void status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof s_status, fmt, ap);
    va_end(ap);
    s_status_t = s_now ? s_now : 1;
}

/* ---- rows ---------------------------------------------------------------- */

static struct row *row_add(int type, int ref, int sel, const char *label)
{
    struct row *r;
    if (s_nrow >= ROWS_MAX)
        return NULL;
    r = &s_row[s_nrow++];
    memset(r, 0, sizeof *r);
    r->type = type;
    r->ref = ref;
    r->sel = sel;
    snprintf(r->label, sizeof r->label, "%s", label ? label : "");
    return r;
}

static void info(const char *label, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
static void info(const char *label, const char *fmt, ...)
{
    struct row *r = row_add(R_INFO, -1, 0, label);
    va_list ap;
    if (!r)
        return;
    va_start(ap, fmt);
    vsnprintf(r->value, sizeof r->value, fmt, ap);
    va_end(ap);
}

static void header(const char *label)
{
    row_add(R_HEADER, -1, 0, label);
}

static void slot_rows(int page, int mod)
{
    for (int i = 0; i < s_nslot; i++) {
        struct slot *s = &s_slot[i];
        struct row *r;
        if (s->page != page || s->mod != mod || s->toggle || s->dset >= 0)
            continue;
        r = row_add(R_SLOT, i, s->it->kind != ENHANCE_ITEM_INFO, s->it->label);
        if (r && s->it->kind == ENHANCE_ITEM_INFO)
            slot_read(s);
        if (!r)
            continue;
        snprintf(r->value, sizeof r->value, "%s", s->cur);
        if (s->help || s->it->help)
            snprintf(r->help, sizeof r->help, "%s", s->help ? s->help : s->it->help);
    }
}

/* ---- the panel's geometry (the draw, the wrap and the hit test share it) - */

/* The section toolbar's icon for each page (Q10). */
static const uint32_t k_page_icon[PAGE_COUNT] = {
    NV2A_OVL_ICON_SLIDERS, NV2A_OVL_ICON_SPARKLES, NV2A_OVL_ICON_PACKAGE, NV2A_OVL_ICON_GAMEPAD,
    NV2A_OVL_ICON_PUZZLE, NV2A_OVL_ICON_ACTIVITY, NV2A_OVL_ICON_KEYBOARD, NV2A_OVL_ICON_BUG,
    NV2A_OVL_ICON_FOLDER
};

/* Q11: the footer's buttons carry the shortcut that presses them. */
static const char *const k_foot_pad[MENU_FB_COUNT] = {"{pad:view} Cancel", "{pad:north} Save"};
static const char *const k_foot_key[MENU_FB_COUNT] = {"{key:shift+esc} Cancel", "{key:F5} Save"};
/* A footer too small for the glyphs shows the bare words. */
static const char *const k_foot_bare[MENU_FB_COUNT] = {"Cancel", "Save"};

/* The glyph family follows the latest device (D8); the mouse's player has
 * the keys at hand, so the mouse shows the keys' shortcuts. */
static int foot_device(void)
{
    return menu_device();
}

static const char *foot_label(int b)
{
    return (menu_device() == ENHANCE_DEV_PAD ? k_foot_pad : k_foot_key)[b];
}

/* The layout's inputs that do not hang on the rows: the placement, the
 * size mode, the text measured at this scale, the toolbar's entries (the
 * pages shown, each one's page in pages[]) and the footer's buttons. The
 * draw adds the rows, the summary and the dialog. */
static void layout_base(struct menu_layout_in *in, int pages[PAGE_COUNT])
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    int px, py, w, h;
    mu_Font ft, fb, fs;
    memset(in, 0, sizeof *in);
    nv2a_overlay_placement(&px, &py, &w, &h);
    if (w <= 0 || h <= 0)
        w = 1920, h = 1080;
    in->w = w;
    in->h = h;
    in->s = nv2a_ovl_ui_scale((uint32_t)h);
    in->tok.pad = t->pad;
    in->tok.gap = t->gap;
    in->tok.row_h = t->row_h;
    in->tok.font_body = t->font_body;
    in->size = s_size;
    ft = OVL_FONT(OVL_FONT_TITLE, nv2a_ovl_px(in->s, t->font_title), 0);
    fb = OVL_FONT(OVL_FONT_BODY, nv2a_ovl_px(in->s, t->font_body), 0);
    fs = OVL_FONT(OVL_FONT_SMALL, nv2a_ovl_px(in->s, t->font_small), 0);
    in->title_h = nv2a_ovl_text_height(ft);
    in->body_h = nv2a_ovl_text_height(fb);
    in->small_h = nv2a_ovl_text_height(fs);
    in->icon_w = nv2a_ovl_px(in->s, t->font_body);
    in->arrow_w = nv2a_ovl_text_width(fb, "<", -1);
    in->sec_sel = -1;
    for (int p = 0; p < PAGE_COUNT; p++) {
        int lw;
        if (!page_shown(p))
            continue;
        lw = nv2a_ovl_text_width(fb, s_page_name[p], -1);
        if (lw > in->label_w)
            in->label_w = lw;
        if (p == s_page)
            in->sec_sel = in->nsec;
        pages[in->nsec++] = p;
    }
    in->intro = enhance_menu_page_intro_text(s_page) != NULL;
    in->cancel_w = enhance_text_width(fs, foot_label(MENU_FB_CANCEL), XBOX_PAD_FAMILY_AUTO,
                                      foot_device());
    in->save_w = enhance_text_width(fs, foot_label(MENU_FB_SAVE), XBOX_PAD_FAMILY_AUTO,
                                    foot_device());
    in->cancel_bare_w = nv2a_ovl_text_width(fs, k_foot_bare[MENU_FB_CANCEL], -1);
    in->save_bare_w = nv2a_ovl_text_width(fs, k_foot_bare[MENU_FB_SAVE], -1);
}

/* An R_TEXT row's text starts here, past its label. */
static int text_col(float s)
{
    return nv2a_ovl_px(s, 150);
}

/* ---- text with tokens (D8) ---- */

/* Whose {ctl:} the text names: the mod's own on its page, else none (the
 * token then draws as its text). */
static int tok_owner(void)
{
    return s_page == PAGE_MODS ? s_mod_sub : -1;
}

static int tok_w(mu_Font f, const char *s)
{
    return enhance_text_width_owned(f, s, XBOX_PAD_FAMILY_AUTO, menu_device(), tok_owner());
}

/* The width of the first len bytes of s, tokens expanded. */
static int tok_wn(mu_Font f, const char *s, size_t len)
{
    char b[512];
    snprintf(b, sizeof b, "%.*s", (int)len, s);
    return tok_w(f, b);
}

static void tok_draw(mu_Context *ctx, mu_Font f, const char *s, int x, int y, mu_Color c)
{
    enhance_text_draw_owned(ctx, f, s, x, y, c, XBOX_PAD_FAMILY_AUTO, menu_device(), tok_owner());
}

/* The bytes of p that fit in w on one line, up to max: broken after the
 * last word that fits, and a word wider than the line cut at a character.
 * Stops at a newline. Measured with the tokens expanded, so a line of
 * glyphs is not wrapped at its braces' width. */
static size_t wrap_take(mu_Font f, const char *p, int w, size_t max)
{
    size_t best = 0, i = 0;
    while (p[i] && p[i] != '\n') {
        size_t k = i;
        while (p[k] == ' ')
            k++;
        while (p[k] && p[k] != ' ' && p[k] != '\n')
            k++;
        if (k <= max && tok_wn(f, p, k) <= w) {
            best = i = k;
            continue;
        }
        if (best)
            return best;
        /* The first word alone is too wide: as many characters as fit. */
        for (k = 0; p[k] && p[k] != '\n';) {
            size_t nx = k + 1;
            while (((unsigned char)p[nx] & 0xC0) == 0x80)
                nx++;
            if (k && (nx > max || tok_wn(f, p, nx) > w))
                break;
            k = nx;
        }
        return k;
    }
    return best;
}

/* One R_TEXT row per line of text, wrapped to the panel at the body
 * font's glyph widths (the atlas's), with no cap on the lines: an About
 * cut to one line was the complaint. The label goes on the first. */
static void text_rows(const char *label, const char *text, int tint)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    static struct menu_layout l;    /* kilobytes: off the stack */
    struct menu_layout_in in;
    int pages[PAGE_COUNT];
    mu_Font fb;
    const char *p = text ? text : "";
    int w;
    /* The content's width does not hang on the rows, which are still
     * being built. */
    layout_base(&in, pages);
    menu_layout_compute(&in, &l);
    fb = OVL_FONT(OVL_FONT_BODY, nv2a_ovl_px(l.s, t->font_body), 0);
    /* The row's room (draw_row: a pad each side of the row, the label
     * column first). */
    w = l.content.w - 2 * l.pad - text_col(l.s);
    for (int first = 1;; first = 0) {
        size_t n = wrap_take(fb, p, w, VAL_MAX - 1);
        struct row *r = row_add(R_TEXT, -1, 0, first ? label : "");
        if (r) {
            snprintf(r->value, sizeof r->value, "%.*s", (int)n, p);
            r->tint = tint;
        }
        p += n;
        while (*p == ' ')
            p++;
        if (*p == '\n')
            p++;
        if (!*p || !r)
            break;
    }
}

static int slot_find(const char *key)
{
    for (int i = 0; i < s_nslot; i++)
        if (!strcmp(s_slot[i].key, key))
            return i;
    return -1;
}

/* Mod m's Enabled toggle slot, whichever key it carries. */
static int toggle_slot(int m)
{
    for (int i = 0; i < s_nslot; i++)
        if (s_slot[i].toggle && s_slot[i].mod == m)
            return i;
    return -1;
}

static void slot_row(const char *key)
{
    int i = slot_find(key);
    struct row *r;
    if (i < 0)
        return;
    r = row_add(R_SLOT, i, 1, s_slot[i].it->label);
    if (!r)
        return;
    snprintf(r->value, sizeof r->value, "%s", s_slot[i].cur);
    if (s_slot[i].help || s_slot[i].it->help)
        snprintf(r->help, sizeof r->help, "%s",
                 s_slot[i].help ? s_slot[i].help : s_slot[i].it->help);
}

static const char *miss_why(uint32_t w)
{
    switch (w) {
    case NV2A_TEX_PACK_MISS_PACK_OFF: return "pack off";
    case NV2A_TEX_PACK_MISS_KIND_OFF: return "kind off";
    case NV2A_TEX_PACK_MISS_REFUSED: return "refused";
    default: return "absent";
    }
}

static void miss_rows(int max)
{
    struct nv2a_tex_pack_miss m[8];
    unsigned n = nv2a_tex_pack_misses_snapshot(m, (unsigned)(max < 8 ? max : 8));
    if (!n) {
        info("Top misses", "none yet");
        return;
    }
    header("Top misses");
    for (unsigned i = 0; i < n; i++) {
        char label[32];
        snprintf(label, sizeof label, "%016llx", (unsigned long long)m[i].hash);
        info(label, "%ux%u fmt 0x%02x \xC3\x97%u %s", m[i].width, m[i].height, m[i].color,
             m[i].count, miss_why(m[i].why));
    }
}

static const char *backend_name(void)
{
    const char *b = recomp_env(RENV_PB_BACKEND);
    return b && *b ? b : "default";
}

static void rates(uint64_t now)
{
    uint64_t c[NV2A_STAT_COUNT];
    for (int k = 0; k < NV2A_STAT_COUNT; k++)
        c[k] = nv2a_stats_count((enum nv2a_stat)k);
    if (!s_rate_t0 || now < s_rate_t0) {
        s_rate_t0 = now ? now : 1;
        memcpy(s_rate_c0, c, sizeof c);
        return;
    }
    if (now - s_rate_t0 < 1000)
        return;
    for (int k = 0; k < NV2A_STAT_COUNT; k++)
        s_rate[k] = (float)(c[k] - s_rate_c0[k]) * 1000.0f / (float)(now - s_rate_t0);
    s_rate_t0 = now;
    memcpy(s_rate_c0, c, sizeof c);
}

static void names_of(uint32_t bits, char *out, size_t n)
{
    size_t at = 0;
    out[0] = 0;
    for (int src = 1; src < INPUT_SRC_COUNT && at < n; src++)
        if (bits & (1u << src))
            at += (size_t)snprintf(out + at, n - at, "%s%s", at ? ", " : "",
                                   xbox_InputMap_SourceName(src));
    if (!at)
        snprintf(out, n, "none");
}

static const char *trace_value(const char *v)
{
    return !v ? "off" : (!*v || !strcmp(v, "1")) ? "on" : v;
}

/* ---- the Mods page's detail view (D5) ------------------------------------- */

static const char *mod_title(int m)
{
    const struct xbox_mod_desc *d = xbox_mod_desc_shown(m);
    return d && d->title && *d->title ? d->title : xbox_mod_name(m);
}

static void state_text(int m, char *out, size_t n, int *tint)
{
    char sw[96];
    const char *why;
    *tint = TINT_NONE;
    switch (xbox_mod_state(m)) {
    case XBOX_MOD_ON:
        snprintf(out, n, "On");
        return;
    case XBOX_MOD_OFF:
        /* The switch that holds it off, so the player knows which key to
         * change: a descriptor mod's own setting may say on. */
        snprintf(sw, sizeof sw, "mods.%s", xbox_mod_name(m));
        if (!enhance_cfg_bool(sw, 1))
            snprintf(out, n, "Off (mods.\"%s\" = false)", xbox_mod_name(m));
        else
            snprintf(out, n, "Off");
        return;
    case XBOX_MOD_FAILED:
        why = xbox_mod_fail_reason(m);
        snprintf(out, n, "Failed%s%s", why && *why ? ": " : "", why ? why : "");
        *tint = TINT_BAD;
        return;
    case XBOX_MOD_RESTART:
        snprintf(out, n, "%s at restart", xbox_mod_enabled(m) ? "On" : "Off");
        *tint = TINT_WARN;
        return;
    }
    out[0] = 0;
}

/* The keys [input.bindings] gives a guest control a captured binding
 * names ("left_stick": W A S D), appended to out at *at; nothing for a
 * host key. */
static void ctl_keys(const char *b, char *out, size_t n, size_t *at, int *nkeys,
                     struct vbuf *g)
{
    static const struct { const char *name; int ctl[4]; } k_alias[] = {
        { "lt", { INPUT_LEFT_TRIGGER, -1, -1, -1 } },
        { "rt", { INPUT_RIGHT_TRIGGER, -1, -1, -1 } },
        { "left_stick", { INPUT_LSTICK_UP, INPUT_LSTICK_LEFT, INPUT_LSTICK_DOWN, INPUT_LSTICK_RIGHT } },
        { "right_stick", { INPUT_RSTICK_UP, INPUT_RSTICK_LEFT, INPUT_RSTICK_DOWN, INPUT_RSTICK_RIGHT } },
    };
    int ctl[4] = { xbox_InputMap_ControlFromName(b), -1, -1, -1 };
    for (unsigned a = 0; a < sizeof k_alias / sizeof k_alias[0]; a++)
        if (!strcmp(b, k_alias[a].name))
            memcpy(ctl, k_alias[a].ctl, sizeof ctl);
    for (int c = 0; c < 4 && ctl[c] >= 0; c++) {
        int codes[XBOX_BIND_MAX], mouse[XBOX_BIND_MAX];
        int k = xbox_InputBindingsOf(ctl[c], codes, mouse, XBOX_BIND_MAX);
        /* A stick's four directions read best one key each: W A S D. */
        for (int j = 0; j < k && j < XBOX_BIND_MAX && *at < n; j++) {
            const char *name = xbox_InputCodeName(codes[j], mouse[j]);
            if (!name)
                continue;
            *at += (size_t)snprintf(out + *at, n - *at, "%s%s",
                                    !*nkeys ? "" : c && !j ? " " : ", ", name);
            vb_put(g, NULL, !*nkeys ? "" : c && !j ? " " : ", ");
            vb_put(g, "key", name);
            ++*nkeys;
            if (ctl[1] >= 0)
                break;
        }
    }
}

/* A captured control's row value: its bindings, then the keys the guest
 * controls among them come from ("RT, X (keys R, E)"); glyph the same as
 * tokens. */
static void ctl_value(int m, uint32_t k, char *out, size_t n, char *glyph, size_t gn)
{
    bind_list l;
    char keys[VAL_MAX], gkeys[VAL_MAX * 2];
    size_t at = 0, kat = 0;
    int nb = xbox_mod_control_bindings(m, k, l, ENHANCE_ACTION_BINDS, NULL), nk = 0;
    struct vbuf g = { glyph, gn, 0 }, gk = { gkeys, sizeof gkeys, 0 };
    out[0] = keys[0] = gkeys[0] = 0;
    if (glyph && gn)
        glyph[0] = 0;
    for (int i = 0; i < nb && at < n; i++) {
        at += (size_t)snprintf(out + at, n - at, "%s%s", i ? ", " : "", l[i]);
        vb_put(&g, NULL, i ? ", " : "");
        vb_put(&g, "bind", l[i]);
        /* ctl_keys puts the separator before each key it adds, so a host
         * key, which adds none, leaves no stray comma. */
        if (kat < sizeof keys)
            ctl_keys(l[i], keys, sizeof keys, &kat, &nk, &gk);
    }
    if (!nb) {
        snprintf(out, n, "none");
    } else if (nk && at < n) {
        snprintf(out + at, n - at, " (key%s %s)", nk == 1 ? "" : "s", keys);
        vb_put(&g, NULL, nk == 1 ? " (key " : " (keys ");
        /* The keys' own form overflowed: the row draws its text. */
        if (!gk.n && g.p && g.n) {
            g.p[0] = 0;
            g.n = 0;
        }
        vb_put(&g, NULL, gkeys);
        vb_put(&g, NULL, ")");
    }
}

static void unavailable(const char *label, const char *help)
{
    struct row *r = row_add(R_INFO, -1, 0, label ? label : "?");
    if (!r)
        return;
    snprintf(r->value, sizeof r->value, "unavailable");
    snprintf(r->help, sizeof r->help, "%s", help ? help : "");
    r->tint = TINT_WARN;
}

static void ctl_row(int m, uint32_t k, int ok)
{
    const struct xbox_mod_control *c = &xbox_mod_desc_shown(m)->controls[k];
    const char *label = c->label ? c->label : c->id;
    struct row *r;
    int id;
    /* A failed mod's rows, INFO ones too: none of them is in effect. */
    if (!ok) {
        unavailable(label, c->help);
        return;
    }
    if (c->kind == XBOX_CTL_INFO) {
        r = row_add(R_INFO, -1, 0, label);
        if (r) {
            snprintf(r->value, sizeof r->value, "%s",
                     c->defaults && c->defaults[0] ? c->defaults[0] : "");
            r->note = c->when;
        }
        return;
    }
    id = c->kind == XBOX_CTL_ACTION ? xbox_mod_control_action(m, k) : -1;
    if (c->kind == XBOX_CTL_ACTION && id < 0) {
        unavailable(label, c->help);
        return;
    }
    if (c->kind == XBOX_CTL_ACTION) {
        r = row_add(R_BIND, id, 1, label);
        if (!r)
            return;
        join_bindings(id, r->value, sizeof r->value, r->glyph, sizeof r->glyph);
        if (enhance_action_dormant(id)) {
            strncat(r->value, " (mod off)", sizeof r->value - strlen(r->value) - 1);
            if (r->glyph[0])
                strncat(r->glyph, " (mod off)", sizeof r->glyph - strlen(r->glyph) - 1);
        }
        snprintf(r->help, sizeof r->help, "%s", c->help ? c->help : "");
        r->note = c->when;
        return;
    }
    r = row_add(R_CTL, m << 8 | (int)k, 1, label);
    if (!r)
        return;
    ctl_value(m, k, r->value, sizeof r->value, r->glyph, sizeof r->glyph);
    r->note = c->when;
    snprintf(r->help, sizeof r->help, "%s%s%s",
             c->help ? c->help : "", c->help && *c->help ? " \xC2\xB7 " : "",
             c->kind == XBOX_CTL_STICK ? "A: push a stick to swap"
                                       : "A: add a key, a click or a game button \xC2\xB7 X: drop the last");
}

/* The keys under the mod's names no lookup read: typos the player can
 * see here as well as in the log. Kept as they are by Save and Reset. */
static void unknown_rows(int m)
{
    const char *keys[16];
    int n = enhance_cfg_unused_of(xbox_mod_name(m), keys, 16);
    if (!n)
        return;
    header("Unknown keys");
    for (int i = 0; i < n && i < 16; i++) {
        struct row *r = row_add(R_INFO, -1, 0, keys[i]);
        if (!r)
            continue;
        snprintf(r->value, sizeof r->value, "not read; kept");
        r->tint = TINT_WARN;
    }
}

/* "Reset free camera controls": the group's own reset, after its rows,
 * for a mod with a control to rebind. The title is mid-sentence, so its
 * first letter goes lowercase (a title like "LOD" keeps its caps). */
static void ctl_reset_row(int m, const struct xbox_mod_desc *d, int ok)
{
    char label[64];
    const char *title = mod_title(m);
    int any = 0;
    struct row *r;
    for (uint32_t k = 0; k < d->n_controls && !any; k++)
        any = d->controls[k].kind != XBOX_CTL_INFO;
    if (!ok || !any)
        return;
    snprintf(label, sizeof label, "Reset %s controls", title);
    /* "Free camera" reads "Reset free camera controls" mid-sentence; a
     * title whose second letter is a capital too ("HUD", "VMU fix") is an
     * acronym and keeps its case. */
    if (isupper((unsigned char)label[6]) && !isupper((unsigned char)label[7]))
        label[6] = (char)tolower((unsigned char)label[6]);
    r = row_add(R_CTLRESET, m, 1, label);
    if (r)
        snprintf(r->help, sizeof r->help, "%s",
                 "Puts back the stock binding of every control above; the settings stay.");
}

static void detail_rows(int m)
{
    const struct xbox_mod_desc *d = xbox_mod_desc_shown(m);
    int ok = xbox_mod_desc_of(m) && xbox_mod_state(m) != XBOX_MOD_FAILED;
    int t = toggle_slot(m), tint, nset = 0;
    char st[VAL_MAX * 2];
    struct row *r = row_add(R_HEADER, -1, 0, mod_title(m));
    if (r && d && d->version_text && *d->version_text)
        snprintf(r->value, sizeof r->value, "v%s", d->version_text);
    if (d && d->author && *d->author)
        text_rows("Author", d->author, TINT_NONE);
    if (d && d->summary && *d->summary)
        text_rows("About", d->summary, TINT_NONE);
    state_text(m, st, sizeof st, &tint);
    text_rows("State", st, tint);
    if (t >= 0) {
        r = row_add(R_SLOT, t, 1, "Enabled");
        if (r)
            snprintf(r->value, sizeof r->value, "%s", s_slot[t].cur);
    }
    if (!d) {
        slot_rows(PAGE_MODS, m);
        unknown_rows(m);
        return;
    }
    if (d->n_controls)
        header("Controls");
    for (uint32_t k = 0; k < d->n_controls; k++)
        ctl_row(m, k, ok);
    ctl_reset_row(m, d, ok);
    for (uint32_t k = 0; k < d->n_settings; k++)
        nset += !(d->settings[k].flags & XBOX_SET_ENABLE);
    for (int i = 0; i < s_nslot && !nset; i++)
        nset = s_slot[i].mod == m && !s_slot[i].toggle;
    if (nset)
        header("Settings");
    for (uint32_t k = 0; k < d->n_settings; k++) {
        const struct xbox_mod_setting *ds = &d->settings[k];
        int i = dset_slot(m, k);
        if (ds->flags & XBOX_SET_ENABLE)
            continue;
        if (i < 0) {
            unavailable(ds->label ? ds->label : ds->name, ds->help);
            continue;
        }
        r = row_add(R_SLOT, i, 1, s_slot[i].it->label);
        if (!r)
            continue;
        snprintf(r->value, sizeof r->value, "%s", s_slot[i].cur);
        snprintf(r->help, sizeof r->help, "%s",
                 s_slot[i].help ? s_slot[i].help : s_slot[i].it->help ? s_slot[i].it->help : "");
    }
    slot_rows(PAGE_MODS, m);
    if (ok)
        row_add(R_MODRESET, m, 1, "Reset this mod");
    unknown_rows(m);
}

/* The Controls page: the actions grouped by owner, the toolkit's first,
 * then the game's, then each mod's under its title; a group of actions
 * that never fire while the mod is off says so. */
static void action_groups(void)
{
    int n = enhance_action_count();
    for (int g = -1; g >= -2 || g < xbox_mod_count();) {
        int any = 0, dormant = 0;
        char head[96];
        for (int id = 0; id < n; id++)
            if (enhance_action_name(id) && enhance_action_mod(id) == g) {
                any = 1;
                dormant |= enhance_action_dormant(id);
            }
        if (any) {
            if (g == ENHANCE_ACT_TOOLKIT)
                snprintf(head, sizeof head, "Toolkit");
            else if (g == ENHANCE_ACT_GAME)
                snprintf(head, sizeof head, "Game");
            else
                snprintf(head, sizeof head, "%s%s", mod_title(g), dormant ? " (mod off)" : "");
            header(head);
            for (int id = 0; id < n; id++) {
                struct row *r;
                if (!enhance_action_name(id) || enhance_action_mod(id) != g)
                    continue;
                r = row_add(R_BIND, id, 1, enhance_action_label(id));
                if (r)
                    join_bindings(id, r->value, sizeof r->value, r->glyph, sizeof r->glyph);
            }
        }
        g = g == ENHANCE_ACT_TOOLKIT ? ENHANCE_ACT_GAME : g == ENHANCE_ACT_GAME ? 0 : g + 1;
        if (g >= xbox_mod_count())
            break;
    }
}

/* ---- the Shaders page: the active preset's parameters ------------------- */

/* The table as build_rows last read it, and a range item per row for the
 * draw (the slider) and the footer's tag. The store keeps the values and
 * the overrides (nv2a_present_chain.h): the rows are no slots, and Save
 * writes the store's overrides as present.shader_params.<preset>.<name>. */
static struct nv2a_present_param s_ptab[NV2A_PRESENT_PARAMS_MAX];
static struct enhance_item s_pitem[NV2A_PRESENT_PARAMS_MAX];
static int s_nptab;
static char s_pkey_active[64];

#define SHADER_PARAMS "present.shader_params."

/* A step's decimals, as for a mod setting: 0.05 shows 0.05, 1 shows 1. A
 * preset with no step moves by a hundredth of its range. */
static double param_step(const struct nv2a_present_param *p)
{
    return p->step > 0 ? p->step : p->max > p->min ? (p->max - p->min) / 100 : 1;
}

static int param_decimals(const struct nv2a_present_param *p)
{
    double x = param_step(p);
    int d = 0;
    while (d < 6 && fabs(x - floor(x + 0.5)) > 1e-6) {
        x *= 10;
        d++;
    }
    return d;
}

/* The page's sub-lists (design D5): the main list, one stage's (0..7), or
 * the Add picker. A confirm (Remove, Delete) waits on one row; a selection
 * an action asked for is taken when the rows are built next. */
#define SH_MAIN (-1)
#define SH_ADD 99
enum { ACT_NEW, ACT_DUP, ACT_DEL, ACT_UP, ACT_DOWN, ACT_REMOVE, ACT_RESET_STAGE };
static int s_sh_sub = SH_MAIN;
static int s_sh_conf, s_sh_conf_type, s_sh_conf_ref;
static int s_sh_want, s_sh_want_type, s_sh_want_ref;
static char s_pick[SHADER_PROFILE_STAGES_MAX + 64][SHADER_PROFILE_REF_MAX];
static int s_npick;
static char s_pnote[NV2A_PRESENT_PARAMS_MAX][32];
/* Each row's kind (shader_param_kinds.h): its label and names live here, as
 * the item points at them. */
static struct shader_param_kind_info s_pkind[NV2A_PRESENT_PARAMS_MAX];
static const char *s_pchoice[NV2A_PRESENT_PARAMS_MAX][SHADER_PARAM_CHOICES_MAX + 1];

/* Rename's text entry: the name being edited and the cursor, in bytes. */
static int s_name_on;
static char s_name[SHADER_PROFILE_NAME_MAX + 1];
static int s_name_cur, s_name_dir;
static uint64_t s_name_next;
/* The on-screen keyboard over the name field (D4), and where the last draw
 * put its keys, for a click. */
static struct osk s_osk;
static struct osk_layout s_osk_lay;
static int s_osk_lay_ok;

static const struct enhance_item k_profile_item = {
    .label = "Profile", .kind = ENHANCE_ITEM_CHOICE, .live = ENHANCE_LIVE
};

static void sh_select(int type, int ref)
{
    s_sh_want = 1;
    s_sh_want_type = type;
    s_sh_want_ref = ref;
}

static int sh_confirming(int type, int ref)
{
    return s_sh_conf && s_sh_conf_type == type && s_sh_conf_ref == ref;
}

static void sh_reset_state(void)
{
    s_sh_sub = SH_MAIN;
    s_sh_conf = 0;
    s_sh_want = 0;
}

/* The edit is modal, as a capture is: the actions fire nothing meanwhile,
 * so a typed letter never also does what it is bound to. */
/* The selected row's band in the last draw's layout, in the menu's pixels;
 * the panel when it was not drawn. */
static struct menu_rect sel_row_rect(void)
{
    struct menu_rect r = {0, 0, 0, 0};
    if (!s_lay_ok)
        return r;
    r = s_lay.panel;
    for (int i = 0; i < s_lay.nvrow; i++)
        if (s_lay.row[i].index == s_sel[s_page])
            r = s_lay.row[i].r;
    return r;
}

static void name_osk(int on)
{
    if (on && !s_osk.on) {
        enhance_osk_open(&s_osk);
        MTRACE("osk open\n");
    } else if (!on && s_osk.on) {
        enhance_osk_close(&s_osk);
        MTRACE("osk close\n");
    }
    s_need_draw = 1;
}

/* The edit is modal, as a capture is: the actions fire nothing meanwhile,
 * so a typed letter never also does what it is bound to. Text arrives as
 * the host typed it, in the player's layout (D4, text_input.h); a pad
 * player gets the on-screen keyboard. */
static void name_begin(const char *name)
{
    struct menu_rect f = sel_row_rect();
    int px, py, w, h;
    snprintf(s_name, sizeof s_name, "%s", name);
    s_name_cur = (int)strlen(s_name);
    s_name_on = 1;
    s_name_dir = 0;
    enhance_actions_quiet(1);
    nv2a_overlay_placement(&px, &py, &w, &h);
    /* The host places its IME and the system keyboard by this rect, in the
     * window's pixels. */
    {
        int x0 = px + f.x, y0 = py + f.y, x1 = x0 + f.w, y1 = y0 + f.h;
        nv2a_overlay_to_window(&x0, &y0);
        nv2a_overlay_to_window(&x1, &y1);
        xbox_InputTextBegin(x0, y0, x1 - x0, y1 - y0);
    }
    /* The pad has no other way to type; a rename by mouse or touch gets it
     * too, unless a keyboard has been used (a touch screen, a Deck in
     * desktop mode, a mouse-only player). */
    name_osk(menu_device() == ENHANCE_DEV_PAD || !s_kbd_seen);
}

static void name_end(void)
{
    if (!s_name_on)
        return;
    s_name_on = 0;
    xbox_InputTextEnd();
    enhance_osk_close(&s_osk);
    s_osk_lay_ok = 0;
    enhance_actions_quiet(0);
}

/* The parameter rows of the table as the chain published it: every one, or
 * those stage `only` declares (its sub-list) with the shared note. */
/* only: a stage's own names, -1 every row, PARAMS_CHAIN the rows no stage
 * lists (a chain's main page shows them, as no stage's list does). */
#define PARAMS_CHAIN (-2)

/* A saved profile's [param_kinds], when the table is that profile's. */
static const struct shader_param_kinds *param_profile_kinds(void)
{
    const struct shader_edit_entry *e;
    char dir[1024], path[1200];
    if (!nv2a_present_chain_params_profile())
        return NULL;
    e = shader_edit_entry(shader_edit_active());
    if (!e || !e->file[0] || !shader_profile_dir(dir, sizeof dir))
        return NULL;
    shader_profile_path(dir, e->file, path, sizeof path);
    return shader_param_kinds_profile_file(path);
}

static void param_list(int only)
{
    int nn = 0;
    const char (*names)[64] = only >= 0 ? shader_edit_stage_names(only, &nn) : NULL;
    const struct shader_param_kinds *pk = param_profile_kinds();
    int any = 0;
    for (int i = 0; i < s_nptab; i++) {
        const struct nv2a_present_param *p = &s_ptab[i];
        struct enhance_item *it = &s_pitem[i];
        struct row *r;
        float v = p->initial;
        int mine = only == -1 || (only == PARAMS_CHAIN && p->stage < 0);
        for (int k = 0; k < nn && !mine; k++)
            mine = !strcmp(names[k], p->name);
        if (!mine)
            continue;
        if (!any++)
            header("Parameters");
        memset(it, 0, sizeof *it);
        it->key = p->name;
        /* A toggle or a list of modes where the preset meant one (design
         * D5); the value stays the preset's number either way. */
        shader_param_kind_resolve(p, pk, shader_param_kinds_builtin(), &s_pkind[i]);
        it->label = s_pkind[i].label;
        it->kind = s_pkind[i].kind == SHADER_PARAM_TOGGLE   ? ENHANCE_ITEM_TOGGLE
                 : s_pkind[i].kind == SHADER_PARAM_CHOICE ? ENHANCE_ITEM_CHOICE
                                                          : ENHANCE_ITEM_RANGE;
        if (it->kind == ENHANCE_ITEM_CHOICE) {
            for (int k = 0; k < s_pkind[i].nnames; k++)
                s_pchoice[i][k] = s_pkind[i].name[k];
            s_pchoice[i][s_pkind[i].nnames] = NULL;
            it->choices = s_pchoice[i];
        }
        it->lo = p->min;
        it->hi = p->max;
        it->step = param_step(p);
        it->live = ENHANCE_LIVE;
        /* One value per name across the chain (decision 1): the note names
         * the first other stage that declares it. */
        s_pnote[i][0] = 0;
        if (only >= 0 && p->shared) {
            const struct shader_edit_entry *e = shader_edit_entry(shader_edit_active());
            for (int t = 0; e && t < e->nstages && !s_pnote[i][0]; t++) {
                int tn = 0;
                const char (*tnames)[64] = t == only ? NULL : shader_edit_stage_names(t, &tn);
                for (int k = 0; k < tn; k++)
                    if (!strcmp(tnames[k], p->name)) {
                        snprintf(s_pnote[i], sizeof s_pnote[i], "shared with stage %d", t + 1);
                        break;
                    }
            }
            if (s_pnote[i][0])
                it->note = s_pnote[i];
        }
        r = row_add(R_PARAM, i, 1, it->label);
        if (!r)
            break;
        nv2a_present_chain_param_get(p->name, &v);
        if (it->kind == ENHANCE_ITEM_TOGGLE)
            snprintf(r->value, sizeof r->value, "%s",
                     shader_param_toggle_on(p, v) ? "true" : "false");
        else if (it->kind == ENHANCE_ITEM_CHOICE)
            snprintf(r->value, sizeof r->value, "%s",
                     s_pkind[i].name[shader_param_kind_index(p, v)]);
        else
            snprintf(r->value, sizeof r->value, "%.*f", param_decimals(p), v);
        /* The declared help first, then the bounds and the preset's value
         * as the preset wrote them: they need not sit on the step grid
         * (warpX ends at 0.125, step 0.01). A toggle names its on state. */
        {
            char on[48] = "", tail[sizeof r->help];
            size_t room, n = strlen(s_pkind[i].help);
            if (it->kind == ENHANCE_ITEM_TOGGLE && s_pkind[i].nnames == 2)
                snprintf(on, sizeof on, " \xC2\xB7 on: %s", s_pkind[i].name[1]);
            snprintf(tail, sizeof tail, "%s \xC2\xB7 %g..%g \xC2\xB7 preset %g%s", p->name, p->min,
                     p->max, p->initial, on);
            /* The help gives way, at a character, so the bounds stay whole. */
            room = sizeof r->help - 1 - strlen(tail);
            room = room > 4 ? room - 4 : 0;
            if (n > room) {
                n = room;
                while (n && ((unsigned char)s_pkind[i].help[n] & 0xC0) == 0x80)
                    n--;
            }
            snprintf(r->help, sizeof r->help, "%.*s%s%s", (int)n, s_pkind[i].help,
                     n ? " \xC2\xB7 " : "", tail);
        }
    }
    if (!any && s_pkey_active[0] && only != PARAMS_CHAIN)
        info("Parameters", "none in this %s", only >= 0 ? "stage" : "preset");
}

static struct row *act_row(int act, const char *label, const char *help)
{
    struct row *r = row_add(R_ACT, act, 1, label);
    if (r && help)
        snprintf(r->help, sizeof r->help, "%s", help);
    return r;
}

/* A chain edit an env var refuses: dimmed, the help names the var. */
static void forced_dim(struct row *r, const char *forced)
{
    if (!r || !forced)
        return;
    r->dim = 1;
    snprintf(r->help, sizeof r->help, "Set by %s", forced);
}

/* A row that asks again, as the Save page's Reset does. */
static struct row *confirm_row(int type, int ref, const char *label, const char *ask,
                               const char *help)
{
    struct row *r;
    char l[64];
    if (sh_confirming(type, ref))
        snprintf(l, sizeof l, "%s? A / Enter: yes", ask);
    else
        snprintf(l, sizeof l, "%s", label);
    r = row_add(type, ref, 1, l);
    if (!r)
        return NULL;
    if (sh_confirming(type, ref))
        snprintf(r->value, sizeof r->value, "B / Esc: no");
    if (help)
        snprintf(r->help, sizeof r->help, "%s", help);
    return r;
}

static void stage_rows(const struct shader_edit_entry *e, int st)
{
    char path[1024], err[SHADER_PROFILE_ERR_MAX], ask[64];
    const char *forced = shader_edit_forced();
    struct row *r;
    header(e->stage[st]);
    info("Preset", "%s", e->stage[st]);
    r = row_add(R_INFO, -1, 0, "Path");
    if (r) {
        if (shader_profile_stage_path(e->stage[st], shader_profile_root(), NULL, path,
                                      sizeof path, err, sizeof err) > 0) {
            snprintf(r->value, sizeof r->value, "%s", path);
        } else {
            snprintf(r->value, sizeof r->value, "%s", err);
            r->tint = TINT_BAD;
        }
    }
    if (s_pkey_active[0])
        param_list(st);
    header("Stage");
    r = act_row(ACT_UP, "Move up", st == 0 ? "It is the first stage" : "One place earlier");
    if (r)
        r->dim = st == 0;
    forced_dim(r, forced);
    r = act_row(ACT_DOWN, "Move down",
                st == e->nstages - 1 ? "It is the last stage" : "One place later");
    if (r)
        r->dim = st == e->nstages - 1;
    forced_dim(r, forced);
    snprintf(ask, sizeof ask, "Remove %.40s", e->stage[st]);
    forced_dim(confirm_row(R_ACT, ACT_REMOVE, "Remove", ask,
                           e->nstages == 1 ? "The last stage stays: pick Off instead" : NULL),
               forced);
    act_row(ACT_RESET_STAGE, "Reset this stage", "The values this stage declares first");
}

static void add_rows(void)
{
    header("Add shader");
    s_npick = shader_edit_presets(s_pick, (int)(sizeof s_pick / sizeof s_pick[0]));
    for (int i = 0; i < s_npick; i++) {
        struct row *r = row_add(R_PICK, i, 1, s_pick[i]);
        if (r)
            snprintf(r->help, sizeof r->help, "A adds it as the last stage \xC2\xB7 B back");
    }
}

/* The Shaders page (design D5). */
static void shader_rows(void)
{
    const struct shader_edit_entry *e = shader_edit_entry(shader_edit_active());
    const char *forced = shader_edit_forced();
    struct shader_profile_compose ci;
    char ierr[SHADER_PROFILE_ERR_MAX], perr[SHADER_PROFILE_ERR_MAX], req[VAL_MAX], lim[96];
    int ok, user = e->kind == SHADER_EDIT_USER;
    struct row *r;

    s_nptab = nv2a_present_chain_params(s_ptab, NV2A_PRESENT_PARAMS_MAX, s_pkey_active,
                                        sizeof s_pkey_active);
    if (e->kind == SHADER_EDIT_OFF || e->state != SHADER_PROFILE_OK)
        s_sh_sub = SH_MAIN;
    if (s_sh_sub >= e->nstages && s_sh_sub != SH_ADD)
        s_sh_sub = SH_MAIN;
    if (s_sh_sub == SH_ADD) {
        add_rows();
        return;
    }
    if (s_sh_sub >= 0) {
        stage_rows(e, s_sh_sub);
        return;
    }
    r = row_add(R_PROFILE, -1, 1, "Profile");
    if (r) {
        snprintf(r->value, sizeof r->value, "%s", e->name);
        r->tint = e->state != SHADER_PROFILE_OK ? TINT_BAD : TINT_NONE;
        if (forced)
            snprintf(r->help, sizeof r->help, "Set by %s", forced);
        else
            snprintf(r->help, sizeof r->help, "Left / Right pick a profile");
    }
    if (e->kind == SHADER_EDIT_OFF) {
        info("Parameters", "no shader");
        return;
    }
    if (e->state != SHADER_PROFILE_OK) {
        r = row_add(R_HEADER, -1, 0, "Chain");
        if (r) {
            snprintf(r->value, sizeof r->value, "%s", e->err);
            r->tint = TINT_BAD;
        }
        if (user)
            forced_dim(confirm_row(R_ACT, ACT_DEL, "Delete", "Delete this profile",
                                   "Removes the file now, not at Save"),
                       forced);
        return;
    }
    /* The header says why the chain shows stock, else how far it got. */
    ok = shader_edit_info(&ci, ierr, sizeof ierr);
    nv2a_present_chain_requested(req, sizeof req);
    r = row_add(R_HEADER, -1, 0, "Chain");
    if (r) {
        if (nv2a_present_chain_profile_error_get(perr, sizeof perr)) {
            snprintf(r->value, sizeof r->value, "%s", perr);
            r->tint = TINT_BAD;
        } else if (!ok && ierr[0]) {
            snprintf(r->value, sizeof r->value, "%s", ierr);
            r->tint = TINT_BAD;
        } else if (strcmp(req, "none") && !nv2a_present_chain_wanted()) {
            snprintf(r->value, sizeof r->value, "the shader failed to load (see the log)");
            r->tint = TINT_BAD;
        } else if (!s_pkey_active[0]) {
            snprintf(r->value, sizeof r->value, "building");
        } else if (ok) {
            snprintf(r->value, sizeof r->value, "%d pass%s", ci.passes,
                     ci.passes == 1 ? "" : "es");
        }
    }
    for (int st = 0; st < e->nstages; st++) {
        char l[64], ask[64];
        snprintf(l, sizeof l, "%d  %.56s", st + 1, e->stage[st]);
        snprintf(ask, sizeof ask, "Remove %.40s", e->stage[st]);
        if (sh_confirming(R_STAGE, st)) {
            confirm_row(R_STAGE, st, l, ask, NULL);
            continue;
        }
        r = row_add(R_STAGE, st, 1, l);
        if (!r)
            break;
        if (shader_edit_stage_missing(st)) {
            snprintf(r->value, sizeof r->value, "missing");
            r->tint = TINT_BAD;
        }
        if (ok && (ci.stage[st].flags & SHADER_PROFILE_ORIGINAL))
            r->note = "reads the game frame";
        else if (ok && (ci.stage[st].flags & SHADER_PROFILE_SCALED))
            r->note = "sees the scaled picture";
        if (forced)
            snprintf(r->help, sizeof r->help, "A options \xC2\xB7 Set by %s", forced);
        else
            snprintf(r->help, sizeof r->help, "%s",
                     e->nstages == 1
                         ? "A options \xC2\xB7 X remove"
                         : "A options \xC2\xB7 Left / Right move it \xC2\xB7 X remove");
    }
    r = row_add(R_ADD, -1, 1, "Add shader");
    if (r) {
        r->dim = shader_edit_add_full(lim, sizeof lim);
        snprintf(r->help, sizeof r->help, "%s", r->dim ? lim : "A picks a preset to add");
    }
    if (s_pkey_active[0])
        param_list(e->nstages == 1 ? -1 : PARAMS_CHAIN);
    header("Actions");
    forced_dim(act_row(ACT_NEW, "Save as new", "A copy of this chain and its values, as Profile N"),
               forced);
    if (user) {
        r = row_add(R_NAME, -1, 1, "Rename");
        if (r) {
            if (s_name_on) {
                snprintf(r->value, sizeof r->value, "%.*s|%s", s_name_cur, s_name,
                         s_name + s_name_cur);
                r->tint = TINT_WARN;
            } else {
                snprintf(r->value, sizeof r->value, "%s", e->name);
            }
            snprintf(r->help, sizeof r->help, "A types a new name");
        }
        forced_dim(r, forced);
    }
    forced_dim(act_row(ACT_DUP, "Duplicate", "As Save as new, named \"<name> copy\""), forced);
    if (user)
        forced_dim(confirm_row(R_ACT, ACT_DEL, "Delete", "Delete this profile",
                               "Removes the file now, not at Save"),
                   forced);
    r = row_add(R_PRESET, -1, 1, "Reset to preset");
    if (r)
        snprintf(r->help, sizeof r->help, "Every stage's values back to the presets'");
}

/* Left and right by a step, snapped to the step's grid from the minimum so
 * float steps do not drift; OK past the top wraps to the bottom, as a range
 * slot does. */
static void change_param(int i, int dir, int ok)
{
    const struct nv2a_present_param *p;
    double step, x;
    float v;
    if (i < 0 || i >= s_nptab)
        return;
    p = &s_ptab[i];
    step = param_step(p);
    v = p->initial;
    nv2a_present_chain_param_get(p->name, &v);
    /* A toggle flips and a choice wraps both ways, as their slots do; the
     * store gets the number the preset declares for the state. */
    if (s_pitem[i].kind == ENHANCE_ITEM_TOGGLE) {
        nv2a_present_chain_param_set(p->name,
                                     shader_param_toggle_value(p, !shader_param_toggle_on(p, v)));
        return;
    }
    if (s_pitem[i].kind == ENHANCE_ITEM_CHOICE) {
        nv2a_present_chain_param_set(p->name,
                                     shader_param_choice_next(p, s_pkind[i].nnames, v, dir));
        return;
    }
    x = v + (dir < 0 ? -step : step);
    x = p->min + floor((x - p->min) / step + 0.5) * step;
    if (x > p->max + step * 1e-3)
        x = ok ? p->min : p->max;
    if (x < p->min - step * 1e-3)
        x = p->min;
    nv2a_present_chain_param_set(p->name, (float)x);
}

/* A float literal TOML reads back as a float: always a point. */
static void float_literal(float v, char *out, size_t n)
{
    snprintf(out, n, "%.6g", (double)v);
    if (!strpbrk(out, ".eEn"))
        strncat(out, ".0", n - strlen(out) - 1);
}

static void build_rows(void)
{
    char buf[VAL_MAX];
    s_nrow = 0;
    switch (s_page) {
    case PAGE_RENDER:
        slot_rows(PAGE_RENDER, -1);
        break;
    case PAGE_PACK: {
        unsigned n = nv2a_tex_pack_count();
        if (!strcmp(backend_name(), "cpu"))
            info("CPU path", "pack ignored");
        if (!n) {
            info("No pack loaded", "textures.pack");
        } else {
            slot_row("textures.pack");
            if (nv2a_tex_pack_kind_count(XRTP_KIND_FONT))
                slot_row("textures.font");
            else
                info("Pack font", "none in this pack");
            if (nv2a_tex_pack_kind_count(XRTP_KIND_TEXT))
                slot_row("textures.text");
            else
                info("Pack text", "none in this pack");
            info("Entries", "%u (%u art, %u font, %u text)", n,
                 nv2a_tex_pack_kind_count(XRTP_KIND_ART), nv2a_tex_pack_kind_count(XRTP_KIND_FONT),
                 nv2a_tex_pack_kind_count(XRTP_KIND_TEXT));
            info("Scale", "%ux, text %ux", nv2a_tex_pack_scale(), nv2a_tex_pack_text_scale());
            info("BC3 entries", "%u", nv2a_tex_pack_bc3_count());
            info("Hits", "%llu", (unsigned long long)nv2a_tex_pack_hits());
        }
        info("Misses", "%llu", (unsigned long long)nv2a_tex_pack_miss_total());
        miss_rows(4);
        break;
    }
    case PAGE_SHADERS:
        slot_rows(PAGE_SHADERS, -1);
        shader_rows();
        break;
    case PAGE_GAME:
        slot_rows(PAGE_GAME, -1);
        if (!s_nrow)
            info("No game options", "");
        break;
    case PAGE_MODS:
        if (s_mod_sub >= 0) {
            detail_rows(s_mod_sub);
            break;
        }
        /* The title, as the detail header and the Controls groups show it;
         * the id is for config keys and logs (design: "The id stays"). */
        for (int m = 0; m < xbox_mod_count(); m++) {
            struct row *r = row_add(R_MOD, m, 1, mod_title(m));
            if (r)
                snprintf(r->value, sizeof r->value, "%s", xbox_mod_state_name(xbox_mod_state(m)));
        }
        if (!s_nrow)
            info("No mods in this build", "");
        break;
    case PAGE_STATS: {
        struct nv2a_stats_snap st;
        const char *pres = nv2a_overlay_presenter();
        slot_rows(PAGE_STATS, -1);
        nv2a_stats_read(&st, nv2a_ovl_now_ns());
        info("Frame rate", "%.1f fps", st.frames && st.ms_avg > 0 ? 1000.0f / st.ms_avg : 0.0f);
        info("Frame time", "%.1f ms (max %.1f)", st.ms_avg, st.ms_max);
        info("Pacing", "%s", xbox_PacingMode() == XBOX_PACING_SPIN ? "spin" : "sleep");
        info("Flips", "%.0f/s", s_rate[NV2A_STAT_FLIPS]);
        info("Presents", "%.0f/s", s_rate[NV2A_STAT_PRESENTS]);
        info("Draws", "%.0f/s", s_rate[NV2A_STAT_DRAWS]);
        info("Texture uploads", "%.0f/s", s_rate[NV2A_STAT_TEX_UPLOADS]);
        info("Render scale", "%ux", nv2a_host_render_scale());
        info("Presenter", "%s", pres ? pres : "none yet");
        break;
    }
    case PAGE_CONTROLS: {
        xbox_mouse_opts mo;
        const char *rk;
        action_groups();
        header("Mouse");
        slot_row("input.mouse_capture");
        xbox_InputMouseGetOpts(&mo);
        rk = mo.release_key ? xbox_InputCodeName(mo.release_key, 0) : NULL;
        info("Release key", "%s", rk ? rk : "none");
        info("Free keys", "none (3F)");
        if (xbox_InputMouseCaptured())
            info("State", "captured");
        else if (mo.capture == XBOX_MOUSE_CAPTURE_CLICK)
            info("State", "free: click to capture");
        else
            info("State", "free: %s", rk ? rk : "no release key");
        break;
    }
    case PAGE_DEBUG: {
        const struct nv2a_ovl_atlas *a = nv2a_overlay_atlas();
        const char *pres = nv2a_overlay_presenter();
        uint32_t extra = 0, held = 0;
        static const int k_extra[] = {
            INPUT_SRC_GUIDE, INPUT_SRC_MISC1, INPUT_SRC_TOUCHPAD, INPUT_SRC_PADDLE1,
            INPUT_SRC_PADDLE2, INPUT_SRC_PADDLE3, INPUT_SRC_PADDLE4
        };
        for (unsigned i = 0; i < sizeof k_extra / sizeof k_extra[0]; i++)
            if (s_extra_t[k_extra[i]] && s_now - s_extra_t[k_extra[i]] < 1000)
                extra |= 1u << k_extra[i];
        for (int p = 0; p < PORTS; p++)
            held |= s_held[p];
        info("Backend", "%s", backend_name());
        info("Presenter", "%s", pres ? pres : "none yet");
        info("Host options gen", "%u", nv2a_host_opts_gen());
        info("Texture uploads", "%.0f/s", s_rate[NV2A_STAT_TEX_UPLOADS]);
        info("Pack misses", "%llu", (unsigned long long)nv2a_tex_pack_miss_total());
        miss_rows(3);
        names_of(extra, buf, sizeof buf);
        info("Extra buttons (1 s)", "%s", buf);
        names_of(held, buf, sizeof buf);
        info("Held", "%s", buf);
        if (a)
            info("Atlas", "%ux%u", a->w, a->h);
        else
            info("Atlas", "none");
        info("Missing glyphs", "n/a (3E)");
        info("Window shot", "RECOMP_WINDOW_SHOT only");
        header("Trace channels (RECOMP_TRACE)");
#define TRACE_ROW_(id, tier, name, legacy, help) \
        if (RECOMP_ENV_TIER_IS_TRACE_##tier) \
            info(name, "%s", trace_value(recomp_env(RENV_##id)));
#define RECOMP_ENV_TIER_IS_TRACE_TRACE 1
#define RECOMP_ENV_TIER_IS_TRACE_CONFIG 0
#define RECOMP_ENV_TIER_IS_TRACE_DEBUG 0
        RECOMP_ENV_KEYS(TRACE_ROW_)
#undef TRACE_ROW_
        break;
    }
    case PAGE_FILES: {
        /* Save lives in the footer (D9): this page is about the file. */
        const char *path = enhance_cfg_user_path();
        struct row *r;
        info("Config file", "%s", path ? path : "off (RECOMP_CONFIG=none)");
        if (s_nrow)
            snprintf(s_row[s_nrow - 1].help, sizeof s_row[s_nrow - 1].help,
                     "Save (the footer, Y or F5) writes the changes here");
        r = row_add(R_RESET, -1, 1, s_confirm ? "Reset everything? A / Enter: yes" : "Reset to defaults");
        if (r) {
            snprintf(r->value, sizeof r->value, "%s", s_confirm ? "B / Esc: no" : "");
            snprintf(r->help, sizeof r->help,
                     "Every setting, binding and shader back to stock; Save keeps it");
        }
        /* Import and export land with the file browser (phase FB): the
         * rows are here, hidden, so the page keeps its order then. */
        if (k_files_io) {
            row_add(R_FILEIO, 0, 1, "Import settings");
            row_add(R_FILEIO, 1, 1, "Export settings");
        }
        break;
    }
    default:
        break;
    }
    /* The selection on a selectable row, the first one when it is gone. */
    {
        int *sel = &s_sel[s_page];
        if (s_page == PAGE_SHADERS && s_sh_want) {
            s_sh_want = 0;
            for (int i = 0; i < s_nrow; i++)
                if (s_row[i].type == s_sh_want_type && s_row[i].ref == s_sh_want_ref) {
                    *sel = i;
                    break;
                }
        }
        if (*sel >= s_nrow)
            *sel = s_nrow - 1;
        if (*sel < 0)
            *sel = 0;
        if (s_nrow && !s_row[*sel].sel) {
            int i;
            for (i = 0; i < s_nrow && !s_row[i].sel; i++)
                ;
            *sel = i < s_nrow ? i : 0;
        }
    }
}

/* ---- Save and Reset ------------------------------------------------------- */

/* + MODS_MAX: a descriptor toggle saved on also drops a stale
 * mods."<id>" = false (enable_save). */
/* + 2 * REWRITES_MAX: each user-file alias the mods read is written once
 * under its current key, the alias removed (D4). */
#define REWRITES_MAX 64
/* + 2: the profile choice, present.profile and present.shader (D5). */
/* + CTL_PENDING_MAX: the mods' captured controls ([input.controls]). */
#define CTL_PENDING_MAX 64
#define PENDING_MAX (SLOTS_MAX + ENHANCE_ACTIONS_MAX + MODS_MAX + 2 * REWRITES_MAX \
                     + NV2A_PRESENT_OVERRIDES_MAX + 2 + CTL_PENDING_MAX)
static char s_pkey[PENDING_MAX][96];
static char s_pval[PENDING_MAX][VAL_MAX * 4];
static int s_pnull[PENDING_MAX];
static int s_rw_done;           /* the rewrites went out with a Save */
static int s_rw_staged;         /* renamed keys the last collect staged */

/* The stock of mod m's enable setting e: the defaults file's key, else
 * the descriptor's default. */
static double enable_stock(int m, int e, const char *key)
{
    const struct xbox_mod_setting *st = &xbox_mod_desc_of(m)->settings[e];
    const recomp_cfg *d = enhance_cfg_defaults();
    double dv = st->def;

    if (d && st->kind == XBOX_SET_ENUM && recomp_cfg_type_of(d, key) == RCFG_STRING) {
        for (int c = 0; st->choices[c]; c++)
            if (!strcmp(st->choices[c], recomp_cfg_string(d, key, "")))
                dv = c;
    } else if (d && st->kind == XBOX_SET_BOOL && recomp_cfg_type_of(d, key) == RCFG_BOOL) {
        dv = recomp_cfg_bool(d, key, 0);
    }
    return dv;
}

/* The value text of mod m's enable setting e at v, as the config holds
 * it: an enum's choice quoted, a bool true/false. */
static void enable_text(int m, int e, double v, char *out, size_t n)
{
    const struct xbox_mod_setting *st = &xbox_mod_desc_of(m)->settings[e];
    if (st->kind == XBOX_SET_ENUM)
        recomp_cfg_quote(st->choices[(int)v], out, n);
    else
        snprintf(out, n, "%s", v ? "true" : "false");
}

/* A descriptor mod's toggle saves its enable setting, never mods."<id>"
 * (D4): the value the setting now holds (an enum keeps the choice the
 * player had), NULL when that is the stock (the defaults file's, else the
 * descriptor's). Turning it on also removes mods."<id>" = false from the
 * user file, which would hold the mod off at the next start. Returns the
 * entries added at n. */
static int enable_save(const struct slot *s, int n)
{
    int m = s->mod, e = xbox_mod_enable_setting(m), k = 0;
    char stock[VAL_MAX * 4], sw[96];

    snprintf(s_pkey[n], sizeof s_pkey[n], "%s", s->key);
    enable_text(m, e, xbox_mod_setting_value(m, (uint32_t)e), s_pval[n], sizeof s_pval[n]);
    enable_text(m, e, enable_stock(m, e, s->key), stock, sizeof stock);
    s_pnull[n] = !strcmp(stock, s_pval[n]);
    k++;
    snprintf(sw, sizeof sw, "mods.%s", xbox_mod_name(m));
    if (on_text(s->cur) && enhance_cfg_source_of(sw) == ENHANCE_SRC_FILE
        && !enhance_cfg_bool(sw, 1)) {
        snprintf(s_pkey[n + k], sizeof s_pkey[n + k], "%s", sw);
        s_pnull[n + k] = 1;
        k++;
    }
    return k;
}

static int pending_has(int n, const char *key)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(s_pkey[i], key))
            return 1;
    return 0;
}

/* 1 when value is the literal of the stock of the mod setting named key:
 * writing it would leave a key the player never set ("no stray keys"). */
static int rewrite_is_stock(const char *key, const char *value)
{
    for (int m = 0; m < xbox_mod_count(); m++) {
        const struct xbox_mod_desc *d = xbox_mod_desc_of(m);
        for (uint32_t k = 0; d && k < d->n_settings; k++) {
            const char *full = xbox_mod_setting_key(m, k);
            char lit[VAL_MAX * 4];
            if (!full || strcmp(full, key))
                continue;
            return xbox_mod_setting_literal(m, k, xbox_mod_setting_stock(m, k), lit, sizeof lit)
                   && !strcmp(lit, value);
        }
    }
    return 0;
}

/* The aliases a mod read from the user file (D4): any Save, even one with
 * nothing else, writes each value under its current key and removes the
 * old one, unless the player's change of that key already went in. A
 * value that is the stock is dropped rather than written. They are not
 * counted as unsaved changes: the file reads the same either way. */
static int rewrites(int n)
{
    const char *keys[REWRITES_MAX], *vals[REWRITES_MAX], *olds[REWRITES_MAX];
    int k = xbox_mod_pending_rewrites(keys, vals, olds, REWRITES_MAX);
    s_rw_staged = 0;
    for (int i = 0; i < k && i < REWRITES_MAX && n + 2 <= PENDING_MAX; i++) {
        int add = 0;
        if (!pending_has(n, keys[i]) && !rewrite_is_stock(keys[i], vals[i])) {
            snprintf(s_pkey[n], sizeof s_pkey[n], "%s", keys[i]);
            snprintf(s_pval[n], sizeof s_pval[n], "%s", vals[i]);
            s_pnull[n++] = 0;
            add = 1;
        }
        if (!pending_has(n, olds[i])) {
            snprintf(s_pkey[n], sizeof s_pkey[n], "%s", olds[i]);
            s_pnull[n++] = 1;
            add = 1;
        }
        s_rw_staged += add;
    }
    return n;
}

/* Bindings as the TOML array a Save writes. */
static void list_literal(bind_list l, int nl, char *out, size_t n)
{
    size_t at = (size_t)snprintf(out, n, "[");
    for (int k = 0; k < nl && at < n; k++) {
        char q[ENHANCE_ACTION_TEXT * 2 + 4];
        recomp_cfg_quote(l[k], q, sizeof q);
        at += (size_t)snprintf(out + at, n - at, "%s%s", k ? ", " : "", q);
    }
    if (at < n)
        snprintf(out + at, n - at, "]");
}

/* Mod m's control count (the captured ones answer xbox_mod_control_key). */
static uint32_t ctl_count(int m)
{
    const struct xbox_mod_desc *d = xbox_mod_desc_of(m);
    return d ? d->n_controls : 0;
}

static int ctl_dirty(int only)
{
    int n = 0;
    for (int m = 0; m < xbox_mod_count(); m++)
        for (uint32_t k = 0; (only < 0 || only == m) && k < ctl_count(m); k++)
            n += xbox_mod_control_dirty(m, k);
    return n;
}

static int collect(void)
{
    int n = 0;
    for (int i = 0; i < s_nslot; i++) {
        struct slot *s = &s_slot[i];
        char stock[VAL_MAX];
        if (!slot_saveable(s) || !slot_unsaved(s))
            continue;
        if (s->toggle && xbox_mod_enable_setting(s->mod) >= 0) {
            n += enable_save(s, n);
            continue;
        }
        snprintf(s_pkey[n], sizeof s_pkey[n], "%s", s->key);
        s_pnull[n] = slot_stock(s, stock, sizeof stock) && vals_same(s->it, stock, s->cur);
        if (s->dset >= 0) {
            /* The literal the toolkit writes for the setting's kind: a
             * float keeps its point ("1.0"), an enum is quoted. */
            const struct xbox_mod_setting *st = &xbox_mod_desc_of(s->mod)->settings[s->dset];
            xbox_mod_setting_literal(s->mod, (uint32_t)s->dset, dset_parse(st, s->cur), s_pval[n],
                                     sizeof s_pval[n]);
        } else if (s->it->kind == ENHANCE_ITEM_CHOICE && !(s->it->flags & ENHANCE_ITEM_NUMERIC))
            recomp_cfg_quote(s->cur, s_pval[n], sizeof s_pval[n]);
        else
            snprintf(s_pval[n], sizeof s_pval[n], "%s", s->cur);
        n++;
    }
    for (int id = 0; id < enhance_action_count() && id < ENHANCE_ACTIONS_MAX; id++) {
        bind_list cur, stock;
        int nc, ns;
        if (!binds_dirty(id))
            continue;
        nc = enhance_action_bindings(id, cur, ENHANCE_ACTION_BINDS);
        ns = action_stock(id, stock);
        snprintf(s_pkey[n], sizeof s_pkey[n], "input.actions.%s", enhance_action_name(id));
        s_pnull[n] = lists_equal(cur, nc, stock, ns);
        list_literal(cur, nc, s_pval[n], sizeof s_pval[n]);
        n++;
    }
    /* A captured control back at its stock removes its key, as an action
     * does. */
    for (int m = 0, c = 0; m < xbox_mod_count(); m++)
        for (uint32_t k = 0; k < ctl_count(m) && c < CTL_PENDING_MAX; k++) {
            bind_list cur;
            int nc;
            if (!xbox_mod_control_dirty(m, k))
                continue;
            nc = xbox_mod_control_bindings(m, k, cur, ENHANCE_ACTION_BINDS, NULL);
            snprintf(s_pkey[n], sizeof s_pkey[n], "%s", xbox_mod_control_key(m, k));
            s_pnull[n] = xbox_mod_control_is_stock(m, k);
            list_literal(cur, nc, s_pval[n], sizeof s_pval[n]);
            n++;
            c++;
        }
    /* The shader parameters that moved: a removed override deletes its
     * key, so the file holds differences from the preset only. */
    for (int i = 0;; i++) {
        char key[64], name[64];
        float v;
        int has, dirty, w;
        if (!nv2a_present_chain_override_at(i, key, sizeof key, name, sizeof name, &v, &has,
                                            &dirty))
            break;
        if (!dirty || n >= PENDING_MAX)
            continue;
        w = snprintf(s_pkey[n], sizeof s_pkey[n], SHADER_PARAMS "%s.%s", key, name);
        if (w < 0 || (size_t)w >= sizeof s_pkey[n]) {
            fprintf(stderr, LOGP "menu: " SHADER_PARAMS "%s.%s is too long to save\n", key,
                    name);
            continue;
        }
        float_literal(v, s_pval[n], sizeof s_pval[n]);
        s_pnull[n++] = !has;
    }
    {
        const char *ck[2], *cv[2];
        int k = shader_edit_config(ck, cv, 2);
        for (int i = 0; i < k && n < PENDING_MAX; i++, n++) {
            snprintf(s_pkey[n], sizeof s_pkey[n], "%s", ck[i]);
            snprintf(s_pval[n], sizeof s_pval[n], "%s", cv[i] ? cv[i] : "");
            s_pnull[n] = !cv[i];
        }
    }
    s_rw_staged = 0;
    if (!s_rw_done)
        n = rewrites(n);
    return n;
}

int enhance_menu_pending(const char **keys, const char **values, int max)
{
    int n = collect();
    for (int i = 0; i < n && i < max; i++) {
        keys[i] = s_pkey[i];
        values[i] = s_pnull[i] ? NULL : s_pval[i];
    }
    return n;
}

int enhance_menu_unsaved(void)
{
    int n = 0;
    for (int i = 0; i < s_nslot; i++)
        if (slot_saveable(&s_slot[i]) && slot_unsaved(&s_slot[i]))
            n++;
    for (int id = 0; id < ENHANCE_ACTIONS_MAX; id++)
        n += binds_dirty(id);
    return n + ctl_dirty(-1) + nv2a_present_chain_overrides_dirty() + shader_edit_unsaved();
}

/* Every missing directory above `path`, outermost first: a user config
 * path can sit several levels under a folder that does not exist yet.
 * Errors are left to the write that follows, which names the path. */
static void mkdir_parent(const char *path)
{
    char dir[1024];
    size_t n;
    snprintf(dir, sizeof dir, "%s", path);
    n = strlen(dir);
    for (size_t i = 1; i < n; i++) {
        int sep = dir[i] == '/';
#ifdef _WIN32
        sep = sep || dir[i] == '\\';
        if (i == 2 && dir[1] == ':')
            continue;   /* "C:\" is no directory to make */
#endif
        if (!sep || dir[i - 1] == '/' || dir[i - 1] == '\\')
            continue;
        dir[i] = 0;
#ifdef _WIN32
        _mkdir(dir);
#else
        mkdir(dir, 0755);
#endif
        dir[i] = path[i];
    }
}

/* 0 when everything is on disk (or there was nothing to write), -1 when
 * it is not: the status says why, and the unsaved-changes dialog keeps
 * itself open over a failure. */
static int save(void)
{
    const char *path = enhance_cfg_user_path();
    const char *keys[sizeof s_pkey / sizeof s_pkey[0]];
    const char *vals[sizeof s_pkey / sizeof s_pkey[0]];
    /* A refusal names the config path once, then each key left out with
     * its table and advice (~120 bytes a key): room for a full macOS
     * path and several keys in the log line. */
    char err[2048];
    /* The status counts what the player changed; the renamed keys D4
     * writes along are named apart, since the player made none of them. */
    int edits = enhance_menu_unsaved(), n, rc, renamed, wrote;
    if (!path) {
        status("Save is off (RECOMP_CONFIG=none)");
        return -1;
    }
    /* The profiles first: the config may name one that has no file yet. */
    wrote = shader_edit_save(err, sizeof err);
    if (wrote < 0) {
        status("Save failed: %s", err);
        fprintf(stderr, LOGP "menu: saving a shader profile failed: %s\n", err);
        return -1;
    }
    n = enhance_menu_pending(keys, vals, (int)(sizeof keys / sizeof keys[0]));
    if (!n) {
        if (wrote)
            status("Saved %d change%s", edits, edits == 1 ? "" : "s");
        else
            status("Nothing to save");
        return 0;
    }
    renamed = s_rw_staged;
    mkdir_parent(path);
    rc = recomp_cfg_patch(path, keys, vals, (size_t)n, err, sizeof err);
    if (rc < 0) {
        status("Save failed: %s", err);
        fprintf(stderr, LOGP "menu: save to %s failed: %s\n", path, err);
        return -1;
    }
    /* Keys left out for a [table] in the file (rc > 0) are named in the
     * log; the player fixes the file, so they are not kept pending to
     * fail again. */
    for (int i = 0; i < s_nslot; i++)
        memcpy(s_slot[i].base, s_slot[i].cur, sizeof s_slot[i].base);
    for (int id = 0; id < enhance_action_count() && id < ENHANCE_ACTIONS_MAX; id++)
        abase_take(id);
    for (int m = 0; m < xbox_mod_count(); m++)
        for (uint32_t k = 0; k < ctl_count(m); k++)
            xbox_mod_control_mark_saved(m, k);
    nv2a_present_chain_overrides_saved();
    shader_edit_config_saved();
    s_rw_done = 1;
    if (rc > 0) {
        fprintf(stderr, LOGP "menu: saved to %s, %d key%s left out: %s\n", path, rc,
                rc == 1 ? "" : "s", err);
        /* The status line is short: the count, and the log has the rest. */
        status("Saved; %d key%s left out, a [table] in the file holds %s (see the log)", rc,
               rc == 1 ? "" : "s", rc == 1 ? "it" : "them");
        return 0;
    }
    fprintf(stderr, LOGP "menu: saved %d key%s to %s\n", n, n == 1 ? "" : "s", path);
    if (renamed && edits)
        status("Saved %d change%s, %d renamed key%s", edits, edits == 1 ? "" : "s", renamed,
               renamed == 1 ? "" : "s");
    else if (renamed)
        status("Saved %d renamed key%s", renamed, renamed == 1 ? "" : "s");
    else
        status("Saved %d change%s", edits, edits == 1 ? "" : "s");
    return 0;
}

static const char *const k_menu_size[] = {"compact", "expanded", NULL};

/* D2: the size is the player's layout, not a setting to review, so it is
 * written at once, alone: the unsaved changes stay pending for Save. With
 * RECOMP_CONFIG=none it holds for the session. */
static void layout_key_write(const char *key, const char *value, const char *what)
{
    const char *path = enhance_cfg_user_path();
    char q[32], err[512];
    const char *val = q;
    s_need_draw = 1;
    if (!path)
        return;
    recomp_cfg_quote(value, q, sizeof q);
    mkdir_parent(path);
    if (recomp_cfg_patch(path, &key, &val, 1, err, sizeof err) != 0) {
        fprintf(stderr, LOGP "menu: keeping %s in %s failed: %s\n", key, path, err);
        status("%s not kept: %s", what, err);
    }
}

static void size_toggle(void)
{
    s_size = s_size == MENU_SIZE_EXPANDED ? MENU_SIZE_COMPACT : MENU_SIZE_EXPANDED;
    layout_key_write("overlay.menu_size", k_menu_size[s_size], "Size");
}

/* D11: the header's ?, L3 and F1 flip between the tooltip and inline
 * hints (off comes back on inline: the player asked to see them), written
 * at once and alone, as the size is. */
static const char *const k_menu_hints[] = {"tooltip", "inline", "off", NULL};

static void hints_toggle(void)
{
    s_hints = s_hints == MENU_HINTS_INLINE ? MENU_HINTS_TOOLTIP : MENU_HINTS_INLINE;
    layout_key_write("overlay.menu_hints", k_menu_hints[s_hints], "Hints");
}

/* Stage the stock of every slot and action (m -1), or of mod m's own
 * settings and actions but its Enabled row: Reset this mod keeps the mod
 * as it is and puts its options back. slots 0 leaves the settings, for
 * "Reset <mod> controls". */
static void reset_parts(int m, int slots)
{
    for (int i = 0; i < s_nslot && slots; i++) {
        struct slot *s = &s_slot[i];
        char stock[VAL_MAX];
        if (m >= 0 && (s->mod != m || s->toggle))
            continue;
        if (s->env || s->it->kind == ENHANCE_ITEM_INFO || s->it->kind == ENHANCE_ITEM_ACTION
            || !slot_stock(s, stock, sizeof stock) || vals_same(s->it, stock, s->cur))
            continue;
        slot_set(s, stock);
    }
    for (int id = 0; id < enhance_action_count() && id < ENHANCE_ACTIONS_MAX; id++) {
        bind_list cur, stock;
        int nc, ns;
        if (!enhance_action_name(id) || (m >= 0 && enhance_action_mod(id) != m))
            continue;
        nc = enhance_action_bindings(id, cur, ENHANCE_ACTION_BINDS);
        ns = action_stock(id, stock);
        if (!lists_equal(cur, nc, stock, ns))
            action_set(id, stock, ns);
    }
    for (int g = 0; g < xbox_mod_count(); g++)
        for (uint32_t k = 0; (m < 0 || g == m) && k < ctl_count(g); k++) {
            bind_list stock;
            const char *t[ENHANCE_ACTION_BINDS];
            int ns;
            if (xbox_mod_control_forced(g, k) || xbox_mod_control_is_stock(g, k))
                continue;
            ns = xbox_mod_control_stock(g, k, stock, ENHANCE_ACTION_BINDS);
            for (int b = 0; b < ns; b++)
                t[b] = stock[b];
            xbox_mod_control_set_bindings(g, k, t, ns);
        }
}

static void reset_scope(int m)
{
    reset_parts(m, 1);
}

static void reset_all(void)
{
    reset_scope(-1);
    nv2a_present_chain_params_reset(NULL);
    /* The picker to Off; the profile files stay (D5). */
    shader_edit_reset_defaults();
    sh_reset_state();
    status("Reset to defaults; Save keeps it");
}

/* ---- the state machine ---------------------------------------------------- */

static void stats_page_use(void)
{
    int want = s_state == ST_OPEN && (s_page == PAGE_STATS || s_page == PAGE_DEBUG);
    if (want != s_stats_on) {
        nv2a_stats_use(NV2A_STATS_PAGE, want);
        s_stats_on = want;
        s_rate_t0 = 0;
    }
}

static void snapshot_use(int on)
{
    if (on != s_snap_on) {
        nv2a_tex_pack_snapshot_want(on);
        s_snap_on = on;
    }
}

int enhance_menu_open(uint64_t now_ms)
{
    s_now = now_ms;
    if (!s_built)
        build_slots();
    shaders_check();
    /* What changed elsewhere since (the stats action, a mod's toggle)
     * shows as it is now; a change of the menu's own waits for Save. */
    for (int i = 0; i < s_nslot; i++) {
        struct slot *s = &s_slot[i];
        if (!slot_unsaved(s)) {
            slot_read(s);
            memcpy(s->base, s->cur, sizeof s->base);
        }
    }
    /* Bindings likewise: a mod registers its actions late, and another
     * path (a title's rebind) may move one between two opens. */
    for (int id = 0; id < enhance_action_count() && id < ENHANCE_ACTIONS_MAX; id++)
        if (!binds_dirty(id))
            abase_take(id);
    s_state = ST_OPEN;
    enhance_input_menu_hold(1);
    /* D3: the focus starts in the content, as before; the pointer's place
     * now is no motion, so a resting mouse takes nothing. */
    focus_to(MENU_FOCUS_CONTENT);
    s_free_scroll = 0;
    s_drag_row = -1;
    s_hover_row = -1;
    {
        struct xbox_pointer pt;
        xbox_InputPointerTake(&pt);
        s_ptr_seq = pt.seq;
        s_ptr_ax = s_ptr_x = pt.x;
        s_ptr_ay = s_ptr_y = pt.y;
        s_ptr_btn = pt.buttons;
    }
    {
        /* The device that opened it leads: a pad held (the chord), a key
         * down (F10), else the pad when one is connected. */
        uint32_t all = 0;
        for (int p = 0; p < PORTS; p++)
            all |= s_held[p];
        s_mouse = 0;
        s_kbd = all ? 0 : any_key_down() ? 1 : !xbox_InputIsConnected(0);
        if (!all && any_key_down())
            s_kbd_seen = 1;
        MTRACE("open device %s\n", k_dev_name[menu_device()]);
    }
    xbox_InputMouseCursorHide(1);
    s_tip_t = now_ms ? now_ms : 1;
    s_tip_sel = s_tip_page = -1;
    s_confirm = 0;
    s_dlg.on = 0;
    s_peek_t = 0;
    s_rel_armed = s_rel_taint = 0;
    cap_phase(CAP_OFF);
    name_end();
    sh_reset_state();
    /* The profiles folder is read when the page shows, never before. */
    if (s_page == PAGE_SHADERS)
        shader_edit_refresh();
    /* One new number for the released pad, far from the real sequence: a
     * pad that changed once while the menu was open would otherwise come
     * back on this same number, and a title that skips a repeated number
     * would miss that real state (a stick held through the close). */
    for (int p = 0; p < PORTS; p++)
        s_fpkt[p] = s_pkt[p] + 0x40000000u;
    snapshot_use(1);
    stats_page_use();
    s_need_draw = 1;
    return 1;
}

void enhance_menu_close(void)
{
    if (s_state != ST_OPEN)
        return;
    s_state = ST_CLOSING;
    xbox_InputMouseCursorHide(0);
    enhance_input_menu_hold(0);
    s_close_t = s_now;
    s_dlg.on = 0;
    cap_phase(CAP_OFF);
    name_end();
    s_confirm = 0;
    snapshot_use(0);
    stats_page_use();
    nv2a_overlay_hide(NV2A_OVL_MENU);
}

int enhance_menu_is_open(void)
{
    return __atomic_load_n(&s_state, __ATOMIC_RELAXED) == ST_OPEN;
}

static void page_to(int p)
{
    /* A hidden page is stepped over in the direction of travel. */
    int d = p < s_page ? -1 : 1;
    p = (p + PAGE_COUNT) % PAGE_COUNT;
    for (int k = 0; k < PAGE_COUNT && !page_shown(p); k++)
        p = (p + d + PAGE_COUNT) % PAGE_COUNT;
    s_page = p;
    s_mod_sub = -1;
    s_confirm = 0;
    s_free_scroll = 0;
    name_end();
    sh_reset_state();
    if (p == PAGE_SHADERS)
        shader_edit_refresh();
    stats_page_use();
}

/* The next selectable row in direction d; 0 at either end (Q11: no wrap,
 * Down past the last row goes to the footer instead). */
static int move_sel(int d)
{
    int *sel = &s_sel[s_page];
    for (int i = *sel + d; i >= 0 && i < s_nrow; i += d)
        if (s_row[i].sel) {
            *sel = i;
            return 1;
        }
    return 0;
}

static void change_slot(struct slot *s, int dir, int ok)
{
    const struct enhance_item *it = s->it;
    char v[VAL_MAX];
    if (s->env) {
        if (s->envname)
            status("Set by %s", s->envname);
        else
            status("Set by the environment");
        return;
    }
    switch (it->kind) {
    case ENHANCE_ITEM_TOGGLE:
        slot_set(s, on_text(s->cur) ? "false" : "true");
        break;
    case ENHANCE_ITEM_CHOICE: {
        int n = 0, i;
        if (!it->choices || !it->choices[0])
            return;
        while (it->choices[n])
            n++;
        i = index_of(it->choices, s->cur);
        i = i < 0 ? (dir < 0 ? n - 1 : 0) : (i + (dir < 0 ? -1 : 1) + n) % n;
        slot_set(s, it->choices[i]);
        break;
    }
    case ENHANCE_ITEM_RANGE: {
        double x = atof(s->cur), step = it->step > 0 ? it->step : 1;
        x += dir < 0 ? -step : step;
        if (x > it->hi + 1e-9)
            x = ok ? it->lo : it->hi;
        if (x < it->lo - 1e-9)
            x = it->lo;
        snprintf(v, sizeof v, "%g", x);
        slot_set(s, v);
        break;
    }
    case ENHANCE_ITEM_ACTION:
        if (ok && it->set)
            it->set(it, NULL);
        break;
    default:
        break;
    }
    if (it->live == ENHANCE_LIVE_RESTART && it->kind != ENHANCE_ITEM_ACTION)
        status("%s applies at restart", it->label);
}

/* X on a captured control: its last binding goes, but one stays, or the
 * control could never be moved again. */
static void ctl_drop_last(const struct row *r)
{
    int m = r->ref >> 8, n;
    uint32_t k = (uint32_t)(r->ref & 0xff);
    bind_list l;
    const char *t[ENHANCE_ACTION_BINDS];
    if (xbox_mod_control_forced(m, k)) {
        status("%s: set by its env row", r->label);
        return;
    }
    n = xbox_mod_control_bindings(m, k, l, ENHANCE_ACTION_BINDS, NULL);
    if (n <= 1) {
        status("%s keeps one binding: bind another first", r->label);
        return;
    }
    for (int b = 0; b < n - 1; b++)
        t[b] = l[b];
    xbox_mod_control_set_bindings(m, k, t, n - 1);
    status("%s: %s dropped", r->label, l[n - 1]);
}

static void clear_binding(int id)
{
    const char *name = enhance_action_name(id);
    if (name && !strcmp(name, "menu.open")) {
        status("menu.open keeps a binding: the menu could not open again");
        return;
    }
    if (id >= 0 && id < ENHANCE_ACTIONS_MAX && !s_abase_have[id])
        abase_take(id);
    enhance_action_set_bindings(id, NULL, 0);
    status("%s: unbound", enhance_action_label(id));
}

/* X or Delete on a setting: its stock value, staged as any change. */
static void clear_slot(struct slot *s)
{
    char stock[VAL_MAX];
    if (s->it->kind == ENHANCE_ITEM_INFO || s->it->kind == ENHANCE_ITEM_ACTION)
        return;
    if (s->env) {
        status("Set by %s", s->envname ? s->envname : "the environment");
        return;
    }
    if (!slot_stock(s, stock, sizeof stock) || vals_same(s->it, stock, s->cur))
        return;
    slot_set(s, stock);
    status("%s: reset to default (%s)", s->it->label, stock);
}

/* After a chain edit: a bundled preset or a path forked into a profile
 * of its own, which the status names; a refusal says why. */
static void sh_after(int rc, int kind_before, const char *msg)
{
    const struct shader_edit_entry *e = shader_edit_entry(shader_edit_active());
    if (rc < 0) {
        if (msg[0])
            status("%s", msg);
        return;
    }
    if (kind_before != SHADER_EDIT_USER && e && e->kind == SHADER_EDIT_USER)
        status("Now editing %s; Save writes it", e->name);
}

static void sh_action(int act, int stage)
{
    const struct shader_edit_entry *e = shader_edit_entry(shader_edit_active());
    int kind = e ? e->kind : SHADER_EDIT_OFF, rc;
    char msg[SHADER_PROFILE_ERR_MAX * 2];
    msg[0] = 0;
    switch (act) {
    case ACT_NEW:
    case ACT_DUP:
        rc = shader_edit_new(act == ACT_DUP, msg, sizeof msg);
        if (rc < 0 && msg[0]) {
            status("%s", msg);
        } else if (rc >= 0) {
            e = shader_edit_entry(shader_edit_active());
            status("%s: new, Save writes it", e->name);
            sh_select(R_PROFILE, -1);
        }
        break;
    case ACT_DEL:
        shader_edit_delete(msg, sizeof msg);
        if (msg[0])
            status("%s", msg);
        sh_select(R_PROFILE, -1);
        break;
    case ACT_UP:
    case ACT_DOWN:
        rc = shader_edit_move(stage, act == ACT_UP ? -1 : 1, msg, sizeof msg);
        sh_after(rc, kind, msg);
        if (rc >= 0) {
            int to = stage + (act == ACT_UP ? -1 : 1);
            e = shader_edit_entry(shader_edit_active());
            if (to >= 0 && to < e->nstages)
                s_sh_sub = to;
        }
        break;
    case ACT_REMOVE:
        rc = shader_edit_remove(stage, msg, sizeof msg);
        sh_after(rc, kind, msg);
        if (rc >= 0) {
            e = shader_edit_entry(shader_edit_active());
            s_sh_sub = SH_MAIN;
            sh_select(R_STAGE, stage < e->nstages ? stage : e->nstages - 1);
            if (kind == SHADER_EDIT_USER)
                status("Stage removed; Save keeps it");
        }
        break;
    case ACT_RESET_STAGE:
        shader_edit_reset_stage(stage);
        status("%s: preset values; Save keeps it", e->stage[stage]);
        break;
    default:
        break;
    }
}

/* One Shaders page row's Left, Right or OK: 1 when it was one. */
static int sh_event(const struct row *r, int dir, int ok)
{
    const struct shader_edit_entry *e = shader_edit_entry(shader_edit_active());
    char msg[SHADER_PROFILE_ERR_MAX * 2];
    int kind = e->kind, rc;
    msg[0] = 0;
    switch (r->type) {
    case R_PROFILE:
        if (shader_edit_step(dir, msg, sizeof msg) < 0 && msg[0])
            status("%s", msg);
        sh_reset_state();
        return 1;
    case R_STAGE:
        if (ok && sh_confirming(R_STAGE, r->ref)) {
            s_sh_conf = 0;
            sh_action(ACT_REMOVE, r->ref);
        } else if (ok) {
            s_sh_sub = r->ref;
            s_sel[PAGE_SHADERS] = 0;
            s_scroll[PAGE_SHADERS] = 0;
        } else {
            int to = r->ref + dir;
            if (to < 0 || to >= e->nstages)
                return 1;
            rc = shader_edit_move(r->ref, dir, msg, sizeof msg);
            sh_after(rc, kind, msg);
            if (rc >= 0)
                sh_select(R_STAGE, to);
        }
        return 1;
    case R_ADD:
        if (!ok)
            return 1;
        if (shader_edit_add_full(msg, sizeof msg)) {
            status("%s", msg);
            return 1;
        }
        s_sh_sub = SH_ADD;
        s_sel[PAGE_SHADERS] = 0;
        s_scroll[PAGE_SHADERS] = 0;
        return 1;
    case R_PICK:
        if (!ok || r->ref < 0 || r->ref >= s_npick)
            return 1;
        rc = shader_edit_add(s_pick[r->ref], msg, sizeof msg);
        sh_after(rc, kind, msg);
        if (rc >= 0) {
            e = shader_edit_entry(shader_edit_active());
            s_sh_sub = SH_MAIN;
            sh_select(R_STAGE, e->nstages - 1);
            if (kind == SHADER_EDIT_USER)
                status("%s added; Save keeps it", s_pick[r->ref]);
        }
        return 1;
    case R_ACT:
        if (!ok)
            return 1;
        if (r->dim) {
            /* The help names the var; the status repeats it at the press. */
            if (shader_edit_forced())
                status("Set by %s", shader_edit_forced());
            return 1;
        }
        if (r->ref == ACT_DEL || r->ref == ACT_REMOVE) {
            if (!sh_confirming(R_ACT, r->ref)) {
                if (r->ref == ACT_REMOVE && e->nstages <= 1) {
                    status("The last stage stays: pick Off instead");
                    return 1;
                }
                s_sh_conf = 1;
                s_sh_conf_type = R_ACT;
                s_sh_conf_ref = r->ref;
                return 1;
            }
            s_sh_conf = 0;
        }
        sh_action(r->ref, s_sh_sub);
        return 1;
    case R_NAME:
        if (ok && shader_edit_forced())
            status("Set by %s", shader_edit_forced());
        else if (ok)
            name_begin(e->name);
        return 1;
    case R_PRESET:
        if (ok) {
            shader_edit_reset_values();
            status("%s: preset values; Save keeps it", e->name);
        }
        return 1;
    default:
        return 0;
    }
}

/* ---- revert, the row markers and the unsaved-changes dialog (D9, D10) --- */

void enhance_menu_revert(void)
{
    for (int i = 0; i < s_nslot; i++) {
        struct slot *s = &s_slot[i];
        char v[VAL_MAX];
        if (!slot_saveable(s) || !slot_unsaved(s))
            continue;
        /* Through set(), as a change in the menu: a live item shows the
         * old value now, a RESTART one records it for the next start. */
        memcpy(v, s->base, sizeof v);
        slot_set(s, v);
    }
    for (int id = 0; id < enhance_action_count() && id < ENHANCE_ACTIONS_MAX; id++) {
        const char *t[ENHANCE_ACTION_BINDS];
        if (!binds_dirty(id))
            continue;
        for (int k = 0; k < s_abase_n[id]; k++)
            t[k] = s_abase[id][k];
        enhance_action_set_bindings(id, t, s_abase_n[id]);
    }
    for (int m = 0; m < xbox_mod_count(); m++)
        for (uint32_t k = 0; k < ctl_count(m); k++)
            xbox_mod_control_revert(m, k);
    nv2a_present_chain_override_revert();
    shader_edit_revert();
    sh_reset_state();
    s_confirm = 0;
    s_need_draw = 1;
}

static int slot_state(const struct slot *s)
{
    char stock[VAL_MAX];
    int st = 0;
    if (s->it->kind == ENHANCE_ITEM_INFO || s->it->kind == ENHANCE_ITEM_ACTION)
        return 0;
    if (s->env)
        st |= ENHANCE_ROW_FORCED;
    else if (slot_saveable(s) && slot_unsaved(s))
        st |= ENHANCE_ROW_UNSAVED;
    if (slot_stock(s, stock, sizeof stock) && !vals_same(s->it, stock, s->cur))
        st |= ENHANCE_ROW_NONDEFAULT;
    return st;
}

static int row_state(const struct row *r)
{
    int st = 0;
    switch (r->type) {
    case R_SLOT:
        return slot_state(&s_slot[r->ref]);
    case R_MOD: {
        int t = toggle_slot(r->ref);
        return t < 0 ? 0 : slot_state(&s_slot[t]);
    }
    case R_BIND: {
        bind_list cur, stock;
        int nc = enhance_action_bindings(r->ref, cur, ENHANCE_ACTION_BINDS);
        int ns = action_stock(r->ref, stock);
        if (enhance_action_source(r->ref, NULL, 0) == ENHANCE_ACT_SRC_ENV)
            st |= ENHANCE_ROW_FORCED;
        else if (binds_dirty(r->ref))
            st |= ENHANCE_ROW_UNSAVED;
        if (!same_binds(cur, nc, stock, ns))
            st |= ENHANCE_ROW_NONDEFAULT;
        return st;
    }
    case R_CTL: {
        int m = r->ref >> 8;
        uint32_t k = (uint32_t)(r->ref & 0xff);
        if (xbox_mod_control_forced(m, k))
            st |= ENHANCE_ROW_FORCED;
        else if (xbox_mod_control_dirty(m, k))
            st |= ENHANCE_ROW_UNSAVED;
        if (xbox_mod_control_key(m, k) && !xbox_mod_control_is_stock(m, k))
            st |= ENHANCE_ROW_NONDEFAULT;
        return st;
    }
    case R_PARAM: {
        const struct nv2a_present_param *p = &s_ptab[r->ref];
        float v = p->initial;
        nv2a_present_chain_param_get(p->name, &v);
        if (nv2a_present_chain_param_dirty(p->name))
            st |= ENHANCE_ROW_UNSAVED;
        if (fabs((double)v - p->initial) >= param_step(p) / 2)
            st |= ENHANCE_ROW_NONDEFAULT;
        return st;
    }
    case R_PROFILE: {
        const struct shader_edit_entry *e = shader_edit_entry(shader_edit_active());
        if (shader_edit_forced())
            st |= ENHANCE_ROW_FORCED;
        else if (shader_edit_choice_dirty() || shader_edit_dirty(shader_edit_active()))
            st |= ENHANCE_ROW_UNSAVED;
        if (e && e->kind != SHADER_EDIT_OFF)
            st |= ENHANCE_ROW_NONDEFAULT;
        return st;
    }
    default:
        return 0;
    }
}

/* A section's tab carries the unsaved dot while anything on it is. */
static int page_unsaved(int p)
{
    for (int i = 0; i < s_nslot; i++)
        if (s_slot[i].page == p && slot_saveable(&s_slot[i]) && slot_unsaved(&s_slot[i]))
            return 1;
    if (p == PAGE_CONTROLS)
        for (int id = 0; id < ENHANCE_ACTIONS_MAX; id++)
            if (binds_dirty(id))
                return 1;
    if (p == PAGE_MODS)
        return ctl_dirty(-1) > 0;
    if (p == PAGE_SHADERS)
        return nv2a_present_chain_overrides_dirty() + shader_edit_unsaved() > 0;
    return 0;
}

static const char *const k_dlg_button[DLG_BUTTONS] = {
    "Exit without saving", "Exit and Save", "Go Back"
};

/* Exit and Save does nothing where nothing can be written. */
static int dlg_save_off(void)
{
    return !enhance_cfg_user_path();
}

/* The dialog's Save: also off once it failed, so a second press cannot
 * repeat the write that just failed and close nothing (D9a). */
static int dlg_save_dead(void)
{
    return dlg_save_off() || s_dlg.failed;
}

static void dlg_label(char *out, size_t n, int *k, const char *label)
{
    size_t at = strlen(out);
    if ((*k)++ < 3)
        snprintf(out + at, n - at, "%s%s", at ? ", " : "", label ? label : "?");
}

/* "3 settings changed since the last save.", then up to three of them. */
static void dlg_compose(void)
{
    char names[256] = "";
    int un = enhance_menu_unsaved(), k = 0;
    size_t at;
    for (int i = 0; i < s_nslot; i++)
        if (slot_saveable(&s_slot[i]) && slot_unsaved(&s_slot[i]))
            dlg_label(names, sizeof names, &k, s_slot[i].toggle ? mod_title(s_slot[i].mod)
                                                                : s_slot[i].it->label);
    for (int id = 0; id < ENHANCE_ACTIONS_MAX; id++)
        if (binds_dirty(id))
            dlg_label(names, sizeof names, &k, enhance_action_label(id));
    for (int m = 0; m < xbox_mod_count(); m++)
        for (uint32_t c = 0; c < ctl_count(m); c++)
            if (xbox_mod_control_dirty(m, c)) {
                const struct xbox_mod_control *ct = &xbox_mod_desc_of(m)->controls[c];
                dlg_label(names, sizeof names, &k, ct->label ? ct->label : ct->id);
            }
    for (int i = 0;; i++) {
        char key[64], name[64];
        float v;
        int has, dirty;
        if (!nv2a_present_chain_override_at(i, key, sizeof key, name, sizeof name, &v, &has,
                                            &dirty))
            break;
        if (dirty)
            dlg_label(names, sizeof names, &k, name);
    }
    if (shader_edit_unsaved())
        dlg_label(names, sizeof names, &k, "Shader profile");
    at = (size_t)snprintf(s_dlg.body, sizeof s_dlg.body, "%d setting%s changed since the last save.",
                          un, un == 1 ? "" : "s");
    if (names[0] && at < sizeof s_dlg.body)
        at += (size_t)snprintf(s_dlg.body + at, sizeof s_dlg.body - at, "\n%s%s", names,
                               k > 3 ? ", \xE2\x80\xA6" : "");
    if (s_dlg.err[0] && at < sizeof s_dlg.body)
        at += (size_t)snprintf(s_dlg.body + at, sizeof s_dlg.body - at, "\n%s", s_dlg.err);
    if (shader_edit_deleted() && at < sizeof s_dlg.body)
        snprintf(s_dlg.body + at, sizeof s_dlg.body - at,
                 "\nDeleted profiles are gone already; this does not bring them back.");
}

static void dlg_open(void)
{
    s_dlg.on = 1;
    s_dlg.focus = DLG_BACK;
    s_dlg.t0 = s_now;
    s_dlg.failed = 0;
    s_dlg.err[0] = 0;
    s_confirm = 0;
    dlg_compose();
    s_need_draw = 1;
}

/* Every way out of the menu but a host quit: ask while anything is
 * unsaved, so the menu never closes over silent pending changes. */
static void leave_request(void)
{
    if (enhance_menu_unsaved() > 0)
        dlg_open();
    else
        enhance_menu_close();
}

static void dlg_activate(int b)
{
    switch (b) {
    case DLG_DISCARD:
        enhance_menu_revert();
        s_dlg.on = 0;
        enhance_menu_close();
        break;
    case DLG_SAVE:
        if (dlg_save_dead())
            break;
        if (save() < 0) {
            s_dlg.failed = 1;
            snprintf(s_dlg.err, sizeof s_dlg.err, "%s", s_status);
            s_dlg.focus = DLG_BACK;
            dlg_compose();
            break;
        }
        s_dlg.on = 0;
        enhance_menu_close();
        break;
    default:
        s_dlg.on = 0;
        break;
    }
    s_need_draw = 1;
}

/* The dialog is up and past the swallow, so a press that opened it (a
 * reflex second B, a double click) cannot answer it. */
static int dlg_ready(void)
{
    return s_dlg.on && s_now - s_dlg.t0 >= DLG_SWALLOW_MS;
}

/* The dialog takes every event while it is up (D9a). */
static int dlg_event(enum enhance_menu_event ev)
{
    if (!dlg_ready())
        return 1;
    switch (ev) {
    case ENHANCE_EV_LEFT:
    case ENHANCE_EV_UP:
        if (s_dlg.focus > 0)
            s_dlg.focus--;
        break;
    case ENHANCE_EV_RIGHT:
    case ENHANCE_EV_DOWN:
        if (s_dlg.focus < DLG_BUTTONS - 1)
            s_dlg.focus++;
        break;
    case ENHANCE_EV_OK:
        dlg_activate(s_dlg.focus);
        break;
    case ENHANCE_EV_BACK:
    case ENHANCE_EV_CANCEL:
    case ENHANCE_EV_CLOSE:
        /* Asking to leave again never picks for the player. */
        dlg_activate(DLG_BACK);
        break;
    default:
        /* Y (Save) is no shortcut here: a reflex Y never closes the menu. */
        break;
    }
    s_need_draw = 1;
    return 1;
}

/* The keyboard's own dialog keys: Tab and Shift+Tab move focus, N, S and
 * G are the buttons' access keys. 1 when the key was the dialog's. */
static int dlg_key(int vk, int shift)
{
    if (!dlg_ready())
        return 0;
    switch (vk) {
    case 0x09:
        if (shift ? s_dlg.focus > 0 : s_dlg.focus < DLG_BUTTONS - 1)
            s_dlg.focus += shift ? -1 : 1;
        break;
    case 'N': dlg_activate(DLG_DISCARD); break;
    case 'S': dlg_activate(DLG_SAVE); break;
    case 'G': dlg_activate(DLG_BACK); break;
    default: return 0;
    }
    s_need_draw = 1;
    return 1;
}

/* A footer button (D9, Q11), by the pad's A, Enter or a click. */
static void foot_press(int b)
{
    MTRACE("footer %s\n", b == MENU_FB_SAVE ? "save" : "cancel");
    if (b == MENU_FB_CANCEL) {
        leave_request();
        return;
    }
    if (enhance_menu_unsaved() > 0 && !dlg_save_off())
        save();
}

/* The wheel scrolled the rows away from the selection: the next key or pad
 * press brings it back to the first row in view (D3) before it acts.
 * Returns 1 when the press was spent on that. */
static int re_anchor(void)
{
    int *sel = &s_sel[s_page], sc = s_scroll[s_page], away;
    if (!s_free_scroll)
        return 0;
    s_free_scroll = 0;
    away = s_lay_ok && (*sel < sc || *sel >= sc + s_lay.vis);
    if (!away)
        return 0;
    for (int i = sc; i < s_nrow && i < sc + s_lay.vis; i++)
        if (s_row[i].sel) {
            *sel = i;
            break;
        }
    /* No selectable row in view: the selection stays and the rows scroll
     * back to it. Either way the press is spent, since any press (OK, Left)
     * on a row the player cannot see yet would act on it unseen. */
    MTRACE("re-anchor row %d\n", *sel);
    return 1;
}

int enhance_menu_event(enum enhance_menu_event ev, uint64_t now_ms)
{
    struct row *r;
    s_now = now_ms;
    if (s_state != ST_OPEN || ev == ENHANCE_EV_NONE)
        return 0;
    if (s_dlg.on)
        return dlg_event(ev);
    build_rows();
    /* D3's regions: the toolbar and the footer take the section buttons;
     * the global ones (Save, Cancel, the size and hints, LB/RB) fall
     * through to the switch below, the same on every region. */
    if (s_focus == MENU_FOCUS_TOOLBAR) {
        switch (ev) {
        case ENHANCE_EV_UP:
        case ENHANCE_EV_PAGE_PREV:
            page_to(s_page - 1);
            return 1;
        case ENHANCE_EV_DOWN:
        case ENHANCE_EV_PAGE_NEXT:
            page_to(s_page + 1);
            return 1;
        case ENHANCE_EV_OK:
        case ENHANCE_EV_RIGHT:
            /* Into the section on the row it remembers. */
            focus_to(MENU_FOCUS_CONTENT);
            return 1;
        case ENHANCE_EV_BACK:
            leave_request();
            return 1;
        case ENHANCE_EV_LEFT:
        case ENHANCE_EV_CLEAR:
            return 1;
        default:
            break;
        }
    } else if (s_focus == MENU_FOCUS_FOOTER) {
        switch (ev) {
        case ENHANCE_EV_LEFT:
            s_foot_sel = MENU_FB_CANCEL;
            return 1;
        case ENHANCE_EV_RIGHT:
            s_foot_sel = MENU_FB_SAVE;
            return 1;
        case ENHANCE_EV_OK:
            foot_press(s_foot_sel);
            return 1;
        case ENHANCE_EV_UP:
        case ENHANCE_EV_BACK:
            focus_to(MENU_FOCUS_CONTENT);
            return 1;
        case ENHANCE_EV_DOWN:
        case ENHANCE_EV_CLEAR:
            return 1;
        default:
            break;
        }
    } else if (ev == ENHANCE_EV_UP || ev == ENHANCE_EV_DOWN || ev == ENHANCE_EV_LEFT
               || ev == ENHANCE_EV_RIGHT || ev == ENHANCE_EV_OK || ev == ENHANCE_EV_CLEAR) {
        if (re_anchor())
            return 1;
    }
    r = s_nrow ? &s_row[s_sel[s_page]] : NULL;
    if (r && !r->sel)
        r = NULL;
    /* Leaving the Reset row or answering anything but OK cancels the ask. */
    if (s_confirm && !(r && r->type == R_RESET && ev == ENHANCE_EV_OK)) {
        s_confirm = 0;
        if (ev == ENHANCE_EV_BACK)
            return 1;
    }
    if (s_sh_conf && !(r && sh_confirming(r->type, r->ref) && ev == ENHANCE_EV_OK)) {
        s_sh_conf = 0;
        if (ev == ENHANCE_EV_BACK)
            return 1;
    }
    switch (ev) {
    case ENHANCE_EV_UP: move_sel(-1); break;
    case ENHANCE_EV_DOWN:
        if (!move_sel(1)) {
            /* Q11: past the last row, the footer, on Save when there is
             * something to save. */
            s_foot_sel = enhance_menu_unsaved() > 0 ? MENU_FB_SAVE : MENU_FB_CANCEL;
            focus_to(MENU_FOCUS_FOOTER);
        }
        break;
    case ENHANCE_EV_PAGE_PREV:
    case ENHANCE_EV_PAGE_NEXT:
        page_to(ev == ENHANCE_EV_PAGE_PREV ? s_page - 1 : s_page + 1);
        /* Q10: the folded toolbar names where LB/RB went for a moment. */
        s_peek_t = now_ms ? now_ms : 1;
        s_peek_drawn = 0;
        break;
    case ENHANCE_EV_SIZE: size_toggle(); break;
    case ENHANCE_EV_SAVE: save(); break;
    case ENHANCE_EV_CLOSE:
    case ENHANCE_EV_CANCEL: leave_request(); break;
    case ENHANCE_EV_HINTS:
        hints_toggle();
        break;
    case ENHANCE_EV_BACK:
        if (s_page == PAGE_SHADERS && s_sh_sub != SH_MAIN) {
            /* Back to the row the sub-list opened from. */
            if (s_sh_sub == SH_ADD)
                sh_select(R_ADD, -1);
            else
                sh_select(R_STAGE, s_sh_sub);
            s_sh_sub = SH_MAIN;
        } else if (s_mod_sub >= 0) {
            s_sel[PAGE_MODS] = s_mod_sub;
            s_mod_sub = -1;
        } else {
            /* Q12: B, Esc, Backspace and the right button go up a level,
             * to the section's entry in the toolbar; a second one there
             * leaves (the toolbar's BACK, asking first when unsaved). */
            focus_to(MENU_FOCUS_TOOLBAR);
        }
        break;
    case ENHANCE_EV_CLEAR:
        if (r && r->type == R_BIND)
            clear_binding(r->ref);
        if (r && r->type == R_CTL)
            ctl_drop_last(r);
        if (r && r->type == R_SLOT)
            clear_slot(&s_slot[r->ref]);
        if (r && r->type == R_STAGE) {
            const struct shader_edit_entry *e = shader_edit_entry(shader_edit_active());
            if (shader_edit_forced()) {
                status("Set by %s", shader_edit_forced());
            } else if (e->nstages <= 1) {
                status("The last stage stays: pick Off instead");
            } else {
                s_sh_conf = 1;
                s_sh_conf_type = R_STAGE;
                s_sh_conf_ref = r->ref;
            }
        }
        break;
    case ENHANCE_EV_LEFT:
    case ENHANCE_EV_RIGHT:
    case ENHANCE_EV_OK: {
        int dir = ev == ENHANCE_EV_LEFT ? -1 : 1, ok = ev == ENHANCE_EV_OK;
        if (!r)
            break;
        if (sh_event(r, dir, ok))
            break;
        switch (r->type) {
        case R_SLOT:
            change_slot(&s_slot[r->ref], dir, ok);
            break;
        case R_MOD: {
            /* OK opens the mod's page (D5); left and right still switch it
             * from the list. */
            int t;
            if (ok) {
                s_mod_sub = r->ref;
                s_sel[PAGE_MODS] = 0;
                s_scroll[PAGE_MODS] = 0;
                break;
            }
            t = toggle_slot(r->ref);
            if (t >= 0)
                change_slot(&s_slot[t], dir, ok);
            break;
        }
        case R_CTL:
            if (ok) {
                int m = r->ref >> 8;
                uint32_t k = (uint32_t)(r->ref & 0xff);
                const struct xbox_mod_control *c = &xbox_mod_desc_shown(m)->controls[k];
                if (xbox_mod_control_forced(m, k)) {
                    status("%s: set by %s", r->label, c->env ? c->env : "the environment");
                    break;
                }
                cap_phase(CAP_RELEASE);
                s_cap.id = r->ref;
                s_cap.what = c->kind == XBOX_CTL_STICK ? CAP_CTL_STICK : CAP_CTL_BUTTON;
                s_cap.t0 = now_ms;
                s_cap.max = s_cap.last = 0;
                status(s_cap.what == CAP_CTL_STICK ? "Push a stick (Esc cancels)"
                                                   : "Press a key, a mouse button or a pad button "
                                                     "(Esc cancels)");
            }
            break;
        case R_PARAM:
            change_param(r->ref, dir, ok);
            break;
        case R_MODRESET:
            if (ok) {
                reset_scope(r->ref);
                status("%s: reset to defaults; Save keeps it", mod_title(r->ref));
            }
            break;
        case R_CTLRESET:
            if (ok) {
                reset_parts(r->ref, 0);
                status("%s: controls reset to defaults; Save keeps it", mod_title(r->ref));
            }
            break;
        case R_BIND:
            if (ok) {
                cap_phase(CAP_RELEASE);
                s_cap.id = r->ref;
                s_cap.what = CAP_ACTION;
                s_cap.t0 = now_ms;
                s_cap.max = s_cap.last = 0;
                status("Press a key or hold a pad chord (Esc cancels)");
            }
            break;
        case R_RESET:
            if (!ok)
                break;
            if (s_confirm) {
                s_confirm = 0;
                reset_all();
            } else {
                s_confirm = 1;
            }
            break;
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
    return 1;
}

/* ---- capture -------------------------------------------------------------- */

static int is_modifier(int vk)
{
    return vk == XBOX_VK_SHIFT || vk == XBOX_VK_CONTROL || vk == XBOX_VK_MENU
        || (vk >= XBOX_VK_LSHIFT && vk <= XBOX_VK_RMENU);
}

static int any_key_down(void)
{
    /* 1..6 are the mouse buttons as VKs: a click is no key. */
    for (int vk = 8; vk < 256; vk++)
        if (xbox_FramebufferKeyDown(vk))
            return 1;
    return 0;
}

static const uint32_t k_host_only = (1u << INPUT_SRC_GUIDE) | (1u << INPUT_SRC_MISC1)
                                  | (1u << INPUT_SRC_TOUCHPAD) | (1u << INPUT_SRC_PADDLE1)
                                  | (1u << INPUT_SRC_PADDLE2) | (1u << INPUT_SRC_PADDLE3)
                                  | (1u << INPUT_SRC_PADDLE4);

/* 1 when two bindings name the same chord in any order and case: "a+guide"
 * is "guide+a". The hold ("@150") must match as well. */
static int chord_same(const char *a, const char *b)
{
    char ma[8][ENHANCE_ACTION_TEXT], mb[8][ENHANCE_ACTION_TEXT];
    const char *ha = strchr(a, '@'), *hb = strchr(b, '@');
    int na = 0, nb = 0;
    if ((ha ? strcmp(ha, hb ? hb : "") : hb != NULL) != 0)
        return 0;
    for (int side = 0; side < 2; side++) {
        const char *p = side ? b : a, *end = side ? (hb ? hb : b + strlen(b)) : (ha ? ha : a + strlen(a));
        char (*m)[ENHANCE_ACTION_TEXT] = side ? mb : ma;
        int *nm = side ? &nb : &na;
        while (p < end) {
            const char *q = memchr(p, '+', (size_t)(end - p));
            size_t len = (size_t)((q ? q : end) - p), i;
            if (*nm == 8 || len >= ENHANCE_ACTION_TEXT)
                return !strcmp(a, b);
            for (i = 0; i < len; i++)
                m[*nm][i] = (char)((p[i] >= 'A' && p[i] <= 'Z') ? p[i] - 'A' + 'a' : p[i]);
            m[(*nm)++][len] = 0;
            p = q ? q + 1 : end;
        }
    }
    if (na != nb)
        return 0;
    for (int i = 0; i < na; i++) {
        int found = 0;
        for (int j = 0; j < nb && !found; j++)
            if (mb[j][0] && !strcmp(ma[i], mb[j])) {
                mb[j][0] = 0;   /* each member counts once */
                found = 1;
            }
        if (!found)
            return 0;
    }
    return 1;
}

static void capture_finish(const char *text)
{
    bind_list l;
    char why[128], bound[ENHANCE_ACTION_TEXT];
    int n, id = s_cap.id, owner;
    cap_phase(CAP_OFF);
    if (!enhance_action_check(text, why, sizeof why)) {
        status("%s refused: %s", text, why);
        nv2a_overlay_toast("%s refused: %s", text, why);
        return;
    }
    n = enhance_action_bindings(id, l, ENHANCE_ACTION_BINDS);
    for (int i = 0; i < n; i++)
        if (chord_same(l[i], text)) {
            status("%s is bound already", text);
            return;
        }
    /* One input, one action: a key or chord another action has would fire
     * both (a Deck grip that Steam Input sends as F10, menu.open's). Clear
     * it there first. */
    owner = enhance_action_owner(text, id, bound, sizeof bound);
    if (owner >= 0) {
        /* A dormant action keeps its input: the player must see
         * whose it is, a mod that is off says nothing on the Controls page
         * of its own. */
        char who[96];
        const char *off = enhance_action_dormant(owner) ? " (mod off)" : "";
        action_who(owner, who, sizeof who);
        status("%s refused: %s has it (%s)%s; clear it there first", text, who, bound, off);
        nv2a_overlay_toast("%s refused: %s has it (%s)%s; clear it there first", text, who,
                           bound, off);
        return;
    }
    /* A key a mod's captured control names directly is the mod's while it
     * captures (D3 conflict rule 2): an action there would fire mid-flight. */
    if (enhance_action_control_clash(text, why, sizeof why)) {
        status("%s refused: %s", text, why);
        nv2a_overlay_toast("%s refused: %s", text, why);
        return;
    }
    if (n >= ENHANCE_ACTION_BINDS) {
        status("%s refused: %d bindings at most", text, ENHANCE_ACTION_BINDS);
        return;
    }
    snprintf(l[n], ENHANCE_ACTION_TEXT, "%s", text);
    action_set(id, l, n + 1);
    status("%s: bound %s", enhance_action_label(id), text);
}

static void capture_pad_text(uint32_t set, char *out, size_t n)
{
    size_t at = 0;
    int members = 0;
    /* Host-only members first, as the defaults spell them ("guide+a"). */
    for (int pass = 0; pass < 2; pass++)
        for (int src = 1; src < INPUT_SRC_COUNT && at < n; src++)
            if ((set & (1u << src)) && ((k_host_only & (1u << src)) != 0) == (pass == 0)) {
                at += (size_t)snprintf(out + at, n - at, "%s%s", at ? "+" : "",
                                       xbox_InputMap_SourceName(src));
                members++;
            }
    /* A guest chord needs its hold (rule 3); a host-only member makes the
     * chord the host's at once, and one source has no chord to hold. */
    if (members > 1 && !(set & k_host_only) && at < n)
        snprintf(out + at, n - at, "@%u", CHORD_MS);
}

static void ctl_refuse(const char *text, const char *why)
{
    status("%s refused: %s", text, why);
    nv2a_overlay_toast("%s refused: %s", text, why);
}

/* A captured control's capture ends with text, a key name or a guest
 * source (D3): a BUTTON row adds it, a STICK row replaces its stick and
 * gives the old one to the row that had the new one. */
static void ctl_finish(const char *text)
{
    int m = s_cap.id >> 8, other, n;
    uint32_t k = (uint32_t)(s_cap.id & 0xff);
    const struct xbox_mod_desc *d = xbox_mod_desc_of(m);
    const struct xbox_mod_control *c;
    const char *label, *t[ENHANCE_ACTION_BINDS];
    struct enhance_ctl_src v, h;
    bind_list l;
    char why[128];

    cap_phase(CAP_OFF);
    if (!d || k >= d->n_controls)
        return;
    c = &d->controls[k];
    label = c->label ? c->label : c->id;
    if (!enhance_control_parse(text, &v)) {
        ctl_refuse(text, "no key or game button has that name");
        return;
    }
    if ((c->kind == XBOX_CTL_STICK) != (v.type == ENHANCE_CTL_STICK)) {
        ctl_refuse(text, c->kind == XBOX_CTL_STICK ? "a stick row takes a stick"
                                                   : "a stick goes on a stick row");
        return;
    }
    /* The pointer reaches the menu with input.mouse off, but the mouse
     * then records no press, so the binding would never fire. */
    if (v.type == ENHANCE_CTL_MOUSE && !xbox_InputMouseOn()) {
        ctl_refuse(text, "the mouse is off; turn on input.mouse first");
        return;
    }
    n = xbox_mod_control_bindings(m, k, l, ENHANCE_ACTION_BINDS, NULL);
    for (int b = 0; b < n; b++)
        if (enhance_control_parse(l[b], &h) && enhance_control_same(&v, &h)) {
            status("%s is bound already", text);
            return;
        }
    if (enhance_control_conflict(m, k, text, &other, why, sizeof why)) {
        /* Two STICK rows trade sticks: the swap the rule leaves them. */
        if (c->kind == XBOX_CTL_STICK && other >= 0
            && d->controls[other].kind == XBOX_CTL_STICK && n == 1) {
            const struct xbox_mod_control *o = &d->controls[other];
            const char *mine[1] = { l[0] }, *theirs[1] = { text };
            if (xbox_mod_control_forced(m, (uint32_t)other)) {
                ctl_refuse(text, why);
                return;
            }
            xbox_mod_control_set_bindings(m, (uint32_t)other, mine, 1);
            xbox_mod_control_set_bindings(m, k, theirs, 1);
            status("%s: %s, %s: %s", label, text, o->label ? o->label : o->id, l[0]);
            return;
        }
        ctl_refuse(text, why);
        return;
    }
    if (c->kind == XBOX_CTL_STICK) {
        t[0] = text;
        xbox_mod_control_set_bindings(m, k, t, 1);
        status("%s: %s", label, text);
        return;
    }
    if (n >= ENHANCE_ACTION_BINDS) {
        status("%s refused: %d bindings at most", text, ENHANCE_ACTION_BINDS);
        return;
    }
    for (int b = 0; b < n; b++)
        t[b] = l[b];
    t[n] = text;
    xbox_mod_control_set_bindings(m, k, t, n + 1);
    status("%s: bound %s", label, text);
}

/* The pad sources a captured control's capture saw, as one guest name:
 * every source must name the same one (a stick pushed on a diagonal),
 * else the capture is refused. */
static void ctl_finish_pad(uint32_t set)
{
    char name[ENHANCE_ACTION_TEXT] = "", one[ENHANCE_ACTION_TEXT], why[96];
    int stick = s_cap.what == CAP_CTL_STICK;

    for (int src = 1; src < INPUT_SRC_COUNT; src++) {
        if (!(set & (1u << src)))
            continue;
        if (!enhance_control_from_host(src, stick, one, sizeof one, why, sizeof why)) {
            cap_phase(CAP_OFF);
            ctl_refuse(xbox_InputMap_SourceName(src), why);
            return;
        }
        if (name[0] && strcmp(name, one)) {
            cap_phase(CAP_OFF);
            ctl_refuse(name, "a fly control takes one input at a time");
            return;
        }
        snprintf(name, sizeof name, "%s", one);
    }
    if (name[0])
        ctl_finish(name);
}

/* The stick directions in held come from a small threshold, which a
 * resting stick on a worn pad can pass: a captured control's capture
 * takes a direction only past 0.6 (D3), from the host pads themselves. */
#define CTL_STICK_MIN 19661
static uint32_t ctl_held(uint32_t held)
{
    uint32_t dirs = 0;
    for (int src = INPUT_SRC_LSTICK_UP; src <= INPUT_SRC_RSTICK_RIGHT; src++)
        held &= ~(1u << src);
    for (DWORD slot = 0; slot < PORTS; slot++) {
        InputRaw raw;
        xbox_InputHostRaw(slot, &raw);
        for (int src = INPUT_SRC_LSTICK_UP; src <= INPUT_SRC_RSTICK_RIGHT; src++)
            if (raw.src[src] >= CTL_STICK_MIN)
                dirs |= 1u << src;
    }
    return held | dirs;
}

/* One capture step: 1 when it used the input. */
static int capture_step(uint32_t held, uint64_t now)
{
    char text[ENHANCE_ACTION_TEXT];
    if (s_cap.phase == CAP_OFF)
        return 0;
    if (now - s_cap.t0 >= CAPTURE_MS) {
        cap_phase(CAP_OFF);
        status("Capture timed out");
        return 1;
    }
    if (s_cap.phase == CAP_RELEASE) {
        /* The press that started the capture (A, Enter, a click) must go
         * first. */
        if (!held && !any_key_down() && !s_ptr_btn)
            cap_phase(CAP_ARMED);
        return 1;
    }
    for (int vk = 8; vk < 256; vk++) {
        int down = xbox_FramebufferKeyDown(vk) != 0;
        size_t at = 0;
        const char *name;
        /* A fly control takes any key alone, Shift too (fast, slow). */
        if (!down || s_kprev[vk] || (s_cap.what == CAP_ACTION && is_modifier(vk)))
            continue;
        if (vk == 0x1B) {   /* Esc */
            cap_phase(CAP_OFF);
            status("Capture cancelled");
            return 1;
        }
        name = xbox_InputCodeName(vk, 0);
        if (!name)
            continue;
        if (s_cap.what != CAP_ACTION) {
            ctl_finish(name);
            return 1;
        }
        text[0] = 0;
        if (xbox_FramebufferKeyDown(XBOX_VK_CONTROL))
            at += (size_t)snprintf(text + at, sizeof text - at, "%s+", xbox_InputCodeName(XBOX_VK_CONTROL, 0));
        if (xbox_FramebufferKeyDown(XBOX_VK_SHIFT))
            at += (size_t)snprintf(text + at, sizeof text - at, "%s+", xbox_InputCodeName(XBOX_VK_SHIFT, 0));
        if (xbox_FramebufferKeyDown(XBOX_VK_MENU))
            at += (size_t)snprintf(text + at, sizeof text - at, "%s+", xbox_InputCodeName(XBOX_VK_MENU, 0));
        snprintf(text + at, sizeof text - at, "%s", name);
        capture_finish(text);
        return 1;
    }
    if (s_cap.clicks) {
        /* A click, by its binding name: a fly button takes it, a stick row
         * and an action refuse it with their reason (actions take keys and
         * pad buttons). The pointer has the first three buttons. */
        static const int k_btn[XBOX_POINTER_BUTTONS] = { XBOX_MOUSE_LEFT, XBOX_MOUSE_RIGHT,
                                                         XBOX_MOUSE_MIDDLE };
        const char *name;
        int b = 0;
        while (b < XBOX_POINTER_BUTTONS - 1 && !(s_cap.clicks & (1u << b)))
            b++;
        name = xbox_InputCodeName(k_btn[b], 1);
        s_cap.clicks = 0;
        if (name) {
            if (s_cap.what != CAP_ACTION)
                ctl_finish(name);
            else
                capture_finish(name);
            return 1;
        }
    }
    if (s_cap.what != CAP_ACTION)
        held = ctl_held(held);
    if (held) {
        if ((held | s_cap.max) != s_cap.max || held != s_cap.last)
            s_cap.t_set = now;
        s_cap.max |= held;
        s_cap.last = held;
        if (held == s_cap.max && now - s_cap.t_set >= CHORD_MS) {
            if (s_cap.what != CAP_ACTION) {
                ctl_finish_pad(s_cap.max);
                return 1;
            }
            capture_pad_text(s_cap.max, text, sizeof text);
            capture_finish(text);
        }
    } else if (s_cap.max) {
        if (s_cap.what != CAP_ACTION) {
            ctl_finish_pad(s_cap.max);
            return 1;
        }
        capture_pad_text(s_cap.max, text, sizeof text);
        capture_finish(text);
    }
    return 1;
}

int enhance_menu_capturing(void)
{
    return s_cap.phase != CAP_OFF;
}

/* ---- the input step ------------------------------------------------------- */

static const uint32_t k_up = (1u << INPUT_SRC_DPAD_UP) | (1u << INPUT_SRC_LSTICK_UP);
static const uint32_t k_down = (1u << INPUT_SRC_DPAD_DOWN) | (1u << INPUT_SRC_LSTICK_DOWN);
static const uint32_t k_left = (1u << INPUT_SRC_DPAD_LEFT) | (1u << INPUT_SRC_LSTICK_LEFT);
static const uint32_t k_right = (1u << INPUT_SRC_DPAD_RIGHT) | (1u << INPUT_SRC_LSTICK_RIGHT);

static int key_edge(int vk)
{
    return xbox_FramebufferKeyDown(vk) && !s_kprev[vk];
}

static void keys_latch(void)
{
    for (int vk = 0; vk < 256; vk++)
        s_kprev[vk] = xbox_FramebufferKeyDown(vk) != 0;
}

/* ---- View and L3: on release, never as part of the menu's chord (A.6) ----
 * Cancel, the hints toggle and the size (R3) sit on buttons a title may use in its
 * menu.open chord (BLiNX 2: back+left_thumb@400). They fire when let go,
 * and not at all once a chord mate was held with them or the toggle fired
 * while they were down: that hold was the chord, not a press. */

static const uint32_t k_rel_buttons = (1u << INPUT_SRC_BACK) | (1u << INPUT_SRC_LEFT_STICK_CLICK)
                                   | (1u << INPUT_SRC_RIGHT_STICK_CLICK);

/* A binding's name as the pad source it means, 0 for a key or a mouse
 * button (enhance_actions.c reads names the same way). */
static int pad_src_of(const char *name)
{
    xbox_pad_setup id;
    int src, ctl;
    for (const char *p = name; *p; p++)
        if (isupper((unsigned char)*p))
            return 0;   /* "A" is a key, "a" the pad's south button */
    src = xbox_InputMap_SourceFromName(name);
    if (src > INPUT_SRC_NONE)
        return src;
    ctl = xbox_InputMap_ControlFromName(name);
    if (ctl < 0 || ctl >= INPUT_PAD_CONTROL_COUNT)
        return 0;
    xbox_PadSetupIdentity(&id);
    return id.map.source[ctl];
}

/* The pad buttons held with src in one of menu.open's pad chords. */
static uint32_t menu_chord_mates(int src)
{
    bind_list l;
    uint32_t mates = 0;
    int id = -1, n;
    for (int i = 0; i < enhance_action_count() && id < 0; i++)
        if (enhance_action_name(i) && !strcmp(enhance_action_name(i), "menu.open"))
            id = i;
    if (id < 0)
        return 0;
    n = enhance_action_bindings(id, l, ENHANCE_ACTION_BINDS);
    for (int b = 0; b < n; b++) {
        char tok[ENHANCE_ACTION_TEXT];
        uint32_t bits = 0;
        int pad = 1;
        const char *p = l[b];
        while (*p && *p != '@' && pad) {
            size_t k = 0;
            while (*p && *p != '+' && *p != '@' && k + 1 < sizeof tok)
                tok[k++] = *p++;
            tok[k] = 0;
            if (*p == '+')
                p++;
            k = (size_t)pad_src_of(tok);
            if (k)
                bits |= 1u << k;
            else
                pad = 0;
        }
        if (pad && (bits & (1u << src)) && (bits & ~(1u << src)))
            mates |= bits & ~(1u << src);
    }
    return mates;
}

/* The release events of this step, as ENHANCE_EV_* bits by source. */
static uint32_t release_step(uint32_t held, uint32_t press)
{
    uint32_t fired = 0;
    s_rel_armed |= press & k_rel_buttons;
    for (int src = 0; src < INPUT_SRC_COUNT; src++) {
        uint32_t b = 1u << src;
        if (!(s_rel_armed & b))
            continue;
        if (held & b) {
            if (held & menu_chord_mates(src))
                s_rel_taint |= b;
            continue;
        }
        if (!(s_rel_taint & b))
            fired |= b;
        s_rel_armed &= ~b;
        s_rel_taint &= ~b;
    }
    return fired;
}

/* ---- the name editor -------------------------------------------------------
 * Rename types into s_name. The keyboard's text comes from the host's
 * ring (text_input.h) in the player's layout, IME included; the editing
 * keys (Backspace, Delete, the arrows, Home, End) stay key edges. A pad
 * types on the on-screen keyboard (D4). Enter, A on Done or a pad's Start
 * keeps the name; Esc or B drops it. */

static void name_insert_utf8(const char *u)
{
    size_t len = strlen(s_name), n = strlen(u);
    if (!n || len + n > SHADER_PROFILE_NAME_MAX)
        return;
    memmove(s_name + s_name_cur + n, s_name + s_name_cur, len - (size_t)s_name_cur + 1);
    memcpy(s_name + s_name_cur, u, n);
    s_name_cur += (int)n;
}

/* The UTF-8 character before or at the cursor: its start and length. */
static int name_prev(int at)
{
    if (at <= 0)
        return 0;
    at--;
    while (at > 0 && ((unsigned char)s_name[at] & 0xC0) == 0x80)
        at--;
    return at;
}

static int name_next(int at)
{
    int len = (int)strlen(s_name);
    if (at >= len)
        return len;
    at++;
    while (at < len && ((unsigned char)s_name[at] & 0xC0) == 0x80)
        at++;
    return at;
}

static void name_cut(int from, int to)
{
    if (to <= from)
        return;
    memmove(s_name + from, s_name + to, strlen(s_name + to) + 1);
    s_name_cur = from;
}

static void name_keep(void)
{
    char msg[SHADER_PROFILE_ERR_MAX * 2];
    msg[0] = 0;
    if (shader_edit_rename(s_name, msg, sizeof msg) < 0) {
        status("%s", msg[0] ? msg : "That name is refused");
        return;
    }
    name_end();
    status("Renamed to %s; Save writes it", s_name);
}

static void name_cancel(void)
{
    name_end();
    status("Rename cancelled");
}

/* The focused key of the on-screen keyboard, pressed. */
static void osk_press(void)
{
    char t[8];
    switch (enhance_osk_press(&s_osk, t, sizeof t)) {
    case OSK_ACT_TEXT: name_insert_utf8(t); break;
    case OSK_ACT_BACKSPACE: name_cut(name_prev(s_name_cur), s_name_cur); break;
    case OSK_ACT_DONE: name_keep(); break;
    default: break;
    }
    s_need_draw = 1;
}

/* One step of the name editor: 1 while it owns the input. */
static int name_step(uint32_t held, uint32_t press, uint64_t now)
{
    int dir = 0, pad_dir = 0;
    char u[8];
    if (!s_name_on)
        return 0;
    /* The host's typed text first: what came before an Enter is the
     * name's. A keyboard player typing puts the on-screen keys away. */
    for (int k = 0; k < 64 && xbox_InputTextPop(u, sizeof u) > 0; k++) {
        name_insert_utf8(u);
        dev_set(1);
        if (s_osk.on)
            name_osk(0);
    }
    if (key_edge(0x1B)) {
        dev_set(1);
        name_cancel();
        return 1;
    }
    if (key_edge(0x0D)) {
        dev_set(1);
        name_keep();
        return 1;
    }
    if (key_edge(0x08)) {
        name_cut(name_prev(s_name_cur), s_name_cur);
        dev_set(1);
    }
    if (key_edge(0x2E)) {
        name_cut(s_name_cur, name_next(s_name_cur));
        dev_set(1);
    }
    if (key_edge(0x24)) {   /* Home */
        s_name_cur = 0;
        dev_set(1);
    }
    if (key_edge(0x23)) {   /* End */
        s_name_cur = (int)strlen(s_name);
        dev_set(1);
    }
    if (press) {
        dev_set(0);
        if (!s_osk.on) {
            /* A pad picked up mid-edit: its keyboard, the press spent on
             * showing it (B still cancels). */
            if (press & (1u << INPUT_SRC_EAST)) {
                name_cancel();
                return 1;
            }
            name_osk(1);
            s_name_dir = (held & (k_up | k_down | k_left | k_right)) ? -1 : 0;
        } else {
            if (press & (1u << INPUT_SRC_EAST)) {
                name_cancel();
                return 1;
            }
            if (press & (1u << INPUT_SRC_START)) {
                name_keep();
                return 1;
            }
            if (press & (1u << INPUT_SRC_WEST))
                name_cut(name_prev(s_name_cur), s_name_cur);
            if (press & (1u << INPUT_SRC_SOUTH)) {
                osk_press();
                if (!s_name_on)
                    return 1;
            }
            /* LB and RB move the text cursor, the d-pad the keys. */
            if (press & (1u << INPUT_SRC_LEFT_SHOULDER))
                s_name_cur = name_prev(s_name_cur);
            if (press & (1u << INPUT_SRC_RIGHT_SHOULDER))
                s_name_cur = name_next(s_name_cur);
        }
    }
    /* Directions repeat as on the rows; the menu's own s_dir follows them
     * so nothing fires when the editor lets go. The pad's move the
     * on-screen keys, the arrow keys the text cursor. */
    if (s_osk.on && (held & k_up))
        dir = ENHANCE_EV_UP, pad_dir = 1;
    else if (s_osk.on && (held & k_down))
        dir = ENHANCE_EV_DOWN, pad_dir = 1;
    else if (s_osk.on && (held & k_left))
        dir = ENHANCE_EV_LEFT, pad_dir = 1;
    else if (s_osk.on && (held & k_right))
        dir = ENHANCE_EV_RIGHT, pad_dir = 1;
    else if (xbox_FramebufferKeyDown(0x25))
        dir = ENHANCE_EV_LEFT;
    else if (xbox_FramebufferKeyDown(0x27))
        dir = ENHANCE_EV_RIGHT;
    if (s_name_dir < 0) {
        /* The direction that showed the keyboard is spent until let go. */
        if (dir)
            dir = -1;
        else
            s_name_dir = 0;
    }
    if (dir >= 0 && (dir != s_name_dir || (dir && now >= s_name_next))) {
        s_name_next = now + (dir != s_name_dir ? REPEAT_FIRST : REPEAT_NEXT);
        s_name_dir = dir;
        if (pad_dir) {
            enhance_osk_move(&s_osk, dir == ENHANCE_EV_LEFT ? -1 : dir == ENHANCE_EV_RIGHT ? 1 : 0,
                             dir == ENHANCE_EV_UP ? -1 : dir == ENHANCE_EV_DOWN ? 1 : 0);
        } else if (dir == ENHANCE_EV_LEFT) {
            s_name_cur = name_prev(s_name_cur);
            dev_set(1);
        } else if (dir == ENHANCE_EV_RIGHT) {
            s_name_cur = name_next(s_name_cur);
            dev_set(1);
        }
    }
    s_dir = dir > 0 ? dir : 0;
    s_dir_next = now + REPEAT_FIRST;
    return 1;
}

/* ---- the tooltip (D11) ------------------------------------------------------ */

/* The selection's settle time: a new row or section starts the wait. */
static void tip_track(uint64_t now)
{
    if (s_sel[s_page] != s_tip_sel || s_page != s_tip_page) {
        s_tip_sel = s_sel[s_page];
        s_tip_page = s_page;
        s_tip_t = now ? now : 1;
    }
}

/* The selected row's help shows as a card: hints on tooltip, the content
 * focused with nothing modal over it, and the selection settled. */
static int tip_shown(uint64_t now)
{
    const struct row *r;
    if (s_state != ST_OPEN || s_hints != MENU_HINTS_TOOLTIP || s_focus != MENU_FOCUS_CONTENT
        || s_dlg.on || s_name_on || s_cap.phase != CAP_OFF || !s_nrow
        || s_tip_sel != s_sel[s_page] || s_tip_page != s_page)
        return 0;
    r = &s_row[s_sel[s_page]];
    if (!r->sel || !r->help[0])
        return 0;
    /* The mouse's wait counts from the hover; a wheel scroll that moved the
     * row out of view shows no card for it. */
    if (s_free_scroll && s_lay_ok
        && (s_sel[s_page] < s_lay.scroll || s_sel[s_page] >= s_lay.scroll + s_lay.vis))
        return 0;
    return menu_tip_due(menu_device(), s_tip_t, now);
}

/* ---- the pointer (D3) ------------------------------------------------------- */

static const struct enhance_item *row_item(const struct row *rw, const struct slot **sl);

/* The layout's section entries as pages, from the last draw. */
static int s_lay_pages[PAGE_COUNT];

/* A range row's value dragged to x: set once, snapped to the item's step
 * from its low end and clamped to its ends, as Left and Right would leave
 * it; a value the environment holds stays, with the reason in the status. */
static void drag_to(int ri, int x)
{
    const struct menu_layout_row *vr = NULL;
    const struct slot *csl;
    const struct enhance_item *it;
    const struct row *r;
    double f, want, step;
    char v[VAL_MAX];
    if (ri < 0 || ri >= s_nrow)
        return;
    r = &s_row[ri];
    it = row_item(r, &csl);
    if (!it || it->kind != ENHANCE_ITEM_RANGE || it->hi <= it->lo)
        return;
    for (int i = 0; i < s_lay.nvrow; i++)
        if (s_lay.row[i].index == ri)
            vr = &s_lay.row[i];
    if (!vr || vr->track.w <= 0)
        return;
    f = (double)(x - vr->track.x) / (double)vr->track.w;
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    step = it->step > 0 ? it->step : 1;
    want = it->lo + floor(f * (it->hi - it->lo) / step + 0.5) * step;
    if (want > it->hi)
        want = it->hi;
    if (fabs(want - atof(r->value)) < step * 1e-3)
        return;
    /* A value change answers a pending Reset or remove ask with "no", as
     * the pad's step does in enhance_menu_event. */
    s_confirm = 0;
    s_sh_conf = 0;
    s_sel[s_page] = ri;
    if (r->type == R_PARAM) {
        nv2a_present_chain_param_set(s_ptab[r->ref].name, (float)want);
    } else if (r->type == R_SLOT) {
        struct slot *sl = &s_slot[r->ref];
        if (sl->env) {
            if (sl->envname)
                status("Set by %s", sl->envname);
            else
                status("Set by the environment");
            return;
        }
        snprintf(v, sizeof v, "%g", want);
        slot_set(sl, v);
        if (it->live == ENHANCE_LIVE_RESTART)
            status("%s applies at restart", it->label);
    } else {
        return;
    }
    build_rows();
}

/* A left click at a hit. */
static void click(struct menu_hit h, int x)
{
    MTRACE("click region %d index %d part %d\n", h.region, h.index, h.part);
    switch (h.region) {
    case MENU_HIT_DIALOG:
        if (h.index >= 0 && dlg_ready()) {
            s_dlg.focus = h.index;
            dlg_activate(h.index);
        }
        break;
    case MENU_HIT_HEADER:
        if (h.index == MENU_HB_HINTS)
            hints_toggle();
        else if (h.index == MENU_HB_SIZE)
            size_toggle();
        else if (h.index == MENU_HB_CLOSE)
            leave_request();
        break;
    case MENU_HIT_FOOTER:
        if (h.index >= 0) {
            s_foot_sel = h.index;
            foot_press(h.index);
        }
        break;
    case MENU_HIT_TOOLBAR:
        if (h.index >= 0 && h.index < s_lay.nsec) {
            int p = s_lay_pages[h.index];
            if (p != s_page) {
                page_to(p);
                s_peek_t = 0;
            }
            focus_to(MENU_FOCUS_CONTENT);
        }
        break;
    case MENU_HIT_ROW:
        if (h.index < 0 || h.index >= s_nrow || !s_row[h.index].sel)
            break;
        focus_to(MENU_FOCUS_CONTENT);
        s_sel[s_page] = h.index;
        switch (h.part) {
        case MENU_PART_PREV: enhance_menu_event(ENHANCE_EV_LEFT, s_now); break;
        case MENU_PART_NEXT: enhance_menu_event(ENHANCE_EV_RIGHT, s_now); break;
        case MENU_PART_TRACK:
            s_drag_row = h.index;
            drag_to(h.index, x);
            break;
        default:
            /* The pill, the label, the value: the row's A. */
            enhance_menu_event(ENHANCE_EV_OK, s_now);
            break;
        }
        break;
    default:
        /* The backdrop, the panel's bare parts, outside: nothing (D3). */
        break;
    }
    s_need_draw = 1;
}

/* Real motion over a hit: the pointer selects what it rests on. */
static void hover(struct menu_hit h)
{
    switch (h.region) {
    case MENU_HIT_DIALOG:
        if (h.index >= 0 && h.index != s_dlg.focus) {
            s_dlg.focus = h.index;
            s_need_draw = 1;
        }
        break;
    case MENU_HIT_ROW:
        s_hover_row = h.index;
        if (h.index >= 0 && h.index < s_nrow && s_row[h.index].sel
            && (h.index != s_sel[s_page] || s_focus != MENU_FOCUS_CONTENT)) {
            s_sel[s_page] = h.index;
            s_confirm = 0;
            focus_to(MENU_FOCUS_CONTENT);
            s_need_draw = 1;
        }
        break;
    case MENU_HIT_FOOTER:
        if (h.index >= 0 && (s_focus != MENU_FOCUS_FOOTER || s_foot_sel != h.index)) {
            s_foot_sel = h.index;
            focus_to(MENU_FOCUS_FOOTER);
            s_need_draw = 1;
        }
        break;
    default:
        s_hover_row = -1;
        break;
    }
}

/* The wheel over a slider's or a choice's control (the track, the arrows,
 * the value) steps it; anywhere else, the label included, it scrolls, so
 * a page of sliders still scrolls under the pointer. */
static int wheel_steps(struct menu_hit h)
{
    const struct slot *sl;
    const struct enhance_item *it;
    if (h.region != MENU_HIT_ROW || h.index < 0 || h.index >= s_nrow || !s_row[h.index].sel)
        return 0;
    if (h.part != MENU_PART_TRACK && h.part != MENU_PART_PREV && h.part != MENU_PART_NEXT
        && h.part != MENU_PART_VALUE)
        return 0;
    it = row_item(&s_row[h.index], &sl);
    return it && (it->kind == ENHANCE_ITEM_RANGE || it->kind == ENHANCE_ITEM_CHOICE);
}

/* One pointer snapshot: motion past the threshold, the buttons' edges and
 * the wheel. modal: a capture or the name editor owns the input, so only
 * the on-screen keys take a click. */
static void pointer_step(int modal)
{
    struct xbox_pointer p;
    unsigned down, up;
    int x, y, lx, ly, px, py, w, h, moved, any;
    float s = s_lay_ok ? s_lay.s : 1.0f;
    xbox_InputPointerTake(&p);
    if (p.seq == s_ptr_seq && !p.wheel)
        return;
    s_ptr_seq = p.seq;
    /* A press and its release can both land between two steps (a tap on a
     * trackpad or a touch screen, a window pumped only at flips): the
     * snapshot's pressed bits keep it, at the place it was pressed. */
    down = (p.buttons & ~s_ptr_btn) | p.pressed;
    up = s_ptr_btn & ~p.buttons;
    s_ptr_btn = p.buttons;
    if (p.x < 0 || p.y < 0)
        return;
    nv2a_overlay_placement(&px, &py, &w, &h);
    /* Window pixels to the output's (present.surface=internal scales the
     * output into the window after the present), then to the placement's. */
    x = p.x;
    y = p.y;
    nv2a_overlay_to_output(&x, &y);
    x -= px;
    y -= py;
    lx = x;
    ly = y;
    if (p.pressed & XBOX_POINTER_LEFT) {
        lx = p.press[0].x;
        ly = p.press[0].y;
        nv2a_overlay_to_output(&lx, &ly);
        lx -= px;
        ly -= py;
    }
    moved = menu_pointer_moved(s_ptr_ax, s_ptr_ay, p.x, p.y, s);
    if (moved) {
        s_ptr_ax = p.x;
        s_ptr_ay = p.y;
    }
    s_ptr_x = x;
    s_ptr_y = y;
    if (up & XBOX_POINTER_LEFT)
        s_drag_row = -1;
    any = moved || down || p.wheel;
    if (!any)
        return;
    dev_mouse();
    if (modal) {
        int r, c;
        /* A capture takes a click as the input to bind (capture_step). */
        if (s_cap.phase == CAP_ARMED)
            s_cap.clicks |= down;
        if (s_cap.phase != CAP_OFF || !s_name_on)
            return;
        if ((down & XBOX_POINTER_LEFT) && s_osk.on && s_osk_lay_ok
            && enhance_osk_hit(&s_osk_lay, lx, ly, &r, &c)) {
            s_osk.row = r;
            s_osk.col = c;
            osk_press();
        } else if (down & XBOX_POINTER_RIGHT) {
            name_cancel();
        }
        s_need_draw = 1;
        return;
    }
    if (!s_lay_ok)
        return;
    {
        struct menu_hit ht = menu_hit(&s_lay, x, y);
        if (moved && s_drag_row >= 0 && (p.buttons & XBOX_POINTER_LEFT)) {
            drag_to(s_drag_row, x);
            s_need_draw = 1;
        } else if (moved && !p.buttons) {
            hover(ht);
        }
        if (down & XBOX_POINTER_LEFT)
            click(lx == x && ly == y ? ht : menu_hit(&s_lay, lx, ly), lx);
        /* A tap's drag ended in the same step it began. */
        if (!(p.buttons & XBOX_POINTER_LEFT))
            s_drag_row = -1;
        if (down & XBOX_POINTER_RIGHT) {
            /* The pad's B: up a level, then out; in the dialog, Go Back. */
            MTRACE("right click\n");
            if (s_dlg.on) {
                if (dlg_ready())
                    dlg_activate(DLG_BACK);
            } else {
                enhance_menu_event(ENHANCE_EV_BACK, s_now);
            }
            s_need_draw = 1;
        }
        if (p.wheel && !s_dlg.on && wheel_steps(ht)) {
            /* Over a slider's or a choice's control: one step a notch,
             * away from the player up, as Left and Right on the pad. */
            int n = p.wheel < 0 ? -p.wheel : p.wheel;
            MTRACE("wheel %d step row %d\n", p.wheel, ht.index);
            focus_to(MENU_FOCUS_CONTENT);
            int type = s_row[ht.index].type, ref = s_row[ht.index].ref;
            for (int i = 0; i < n; i++) {
                s_sel[s_page] = ht.index;
                enhance_menu_event(p.wheel > 0 ? ENHANCE_EV_RIGHT : ENHANCE_EV_LEFT, s_now);
                /* A step can change the rows (a choice that shows or
                 * hides others): the hit's index may name another row
                 * now, so the rest of the notches stop here. */
                if (ht.index >= s_nrow || s_row[ht.index].type != type
                    || s_row[ht.index].ref != ref)
                    break;
            }
            s_need_draw = 1;
        } else if (p.wheel && !s_dlg.on && ht.region != MENU_HIT_NONE
                   && ht.region != MENU_HIT_BACKDROP) {
            /* Away from the player scrolls up; the selection stays where it
             * is until a key or the pad moves it (re_anchor). */
            int max = s_nrow - s_lay.vis, *sc = &s_scroll[s_page];
            *sc -= p.wheel;
            if (*sc > max)
                *sc = max;
            if (*sc < 0)
                *sc = 0;
            s_free_scroll = 1;
            MTRACE("wheel %d scroll %d\n", p.wheel, *sc);
            s_need_draw = 1;
        }
    }
}

static void input_step(uint64_t now)
{
    uint32_t held = 0, press;
    int dir = 0, redraw = 0, n_pad;
    const struct menu_pad_event *pad_ev = menu_pad_events(&n_pad);
    for (int p = 0; p < PORTS; p++)
        held |= s_held[p];
    for (int src = 1; src < INPUT_SRC_COUNT; src++)
        if ((held & (1u << src)) & k_host_only)
            s_extra_t[src] = now ? now : 1;
    press = held & ~s_prev_held;

    if (s_cap.phase != CAP_OFF || s_name_on)
        pointer_step(1);
    if (capture_step(held, now) || name_step(held, press, now)) {
        /* A button that started there is no press of the menu's, and a
         * Cmd+S typed into a name or a capture is no later Save. */
        xbox_InputShortcutTake('S');
        s_rel_armed = s_rel_taint = 0;
        s_need_draw = 1;
        s_prev_held = held;
        keys_latch();
        tip_track(now);
        return;
    }
    pointer_step(0);

    if (press)
        dev_set(0);
#define EV(e) do { redraw |= enhance_menu_event((e), now); } while (0)
    {
        /* D3's owner table: the pad's buttons and their events, the menu's
         * chord buttons on release (A.6). The directions repeat below. */
        uint32_t rel = release_step(held, press);
        for (int i = 0; i < n_pad; i++) {
            uint32_t b = 1u << pad_ev[i].src;
            if (pad_ev[i].ev >= ENHANCE_EV_UP && pad_ev[i].ev <= ENHANCE_EV_RIGHT)
                continue;
            if (pad_ev[i].on_release ? (rel & b) : (press & b))
                EV((enum enhance_menu_event)pad_ev[i].ev);
        }
    }
    {
        static const struct { int vk; enum enhance_menu_event ev; } k_keys[] = {
            { 0x0D, ENHANCE_EV_OK }, { 0x20, ENHANCE_EV_OK },
            { 0x1B, ENHANCE_EV_BACK }, { 0x08, ENHANCE_EV_BACK },
            { 'Q', ENHANCE_EV_PAGE_PREV }, { 0x21, ENHANCE_EV_PAGE_PREV },
            { 'E', ENHANCE_EV_PAGE_NEXT }, { 0x22, ENHANCE_EV_PAGE_NEXT },
            { 0x74, ENHANCE_EV_SAVE }, { 0x2E, ENHANCE_EV_CLEAR },
            { 0x70, ENHANCE_EV_HINTS },
        };
        int shift = xbox_FramebufferKeyDown(XBOX_VK_SHIFT) != 0;
        int ctrl = xbox_FramebufferKeyDown(XBOX_VK_CONTROL) != 0;
        /* The dialog's Tab and access keys first: N, S and G are its own. */
        static const int k_dlg_keys[] = { 0x09, 'N', 'S', 'G' };
        int cmd_s = xbox_InputShortcutTake('S');
        for (unsigned i = 0; i < sizeof k_dlg_keys / sizeof k_dlg_keys[0]; i++)
            if (key_edge(k_dlg_keys[i]) && !(ctrl && k_dlg_keys[i] == 'S')
                && dlg_key(k_dlg_keys[i], shift)) {
                dev_set(1);
                redraw = 1;
            }
        /* Tab and Shift+Tab: the toolbar, the content and the footer, round
         * (D3); the dialog took it first when it is up. */
        if (s_state == ST_OPEN && !s_dlg.on && key_edge(0x09)) {
            int f = menu_focus_cycle(s_focus, shift ? -1 : 1);
            dev_set(1);
            if (f == MENU_FOCUS_FOOTER && s_focus != MENU_FOCUS_FOOTER)
                s_foot_sel = enhance_menu_unsaved() > 0 ? MENU_FB_SAVE : MENU_FB_CANCEL;
            focus_to(f);
            redraw = 1;
        }
        for (unsigned i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
            if (key_edge(k_keys[i].vk)) {
                dev_set(1);
                if (s_state == ST_OPEN)
                    /* Shift+Esc is Cancel: leave, asking first. */
                    EV(k_keys[i].vk == 0x1B && shift ? ENHANCE_EV_CANCEL : k_keys[i].ev);
            }
        /* Ctrl+S, and Cmd+S on macOS (posted by the host: the key table
         * never sees a Cmd key). In the dialog both are swallowed: the
         * save reflex is no answer to "leave?", only a bare S is. */
        if (s_state == ST_OPEN && !s_dlg.on && ((ctrl && key_edge('S')) || cmd_s)) {
            dev_set(1);
            EV(ENHANCE_EV_SAVE);
        }
    }
    /* Directions repeat while held. */
    if ((held & k_up) || xbox_FramebufferKeyDown(0x26))
        dir = ENHANCE_EV_UP;
    else if ((held & k_down) || xbox_FramebufferKeyDown(0x28))
        dir = ENHANCE_EV_DOWN;
    else if ((held & k_left) || xbox_FramebufferKeyDown(0x25))
        dir = ENHANCE_EV_LEFT;
    else if ((held & k_right) || xbox_FramebufferKeyDown(0x27))
        dir = ENHANCE_EV_RIGHT;
    if (dir && xbox_FramebufferKeyDown(dir == ENHANCE_EV_UP ? 0x26 : dir == ENHANCE_EV_DOWN ? 0x28
                                       : dir == ENHANCE_EV_LEFT ? 0x25 : 0x27))
        dev_set(1);
    if (dir != s_dir) {
        s_dir = dir;
        s_dir_next = now + REPEAT_FIRST;
        if (dir && s_state == ST_OPEN)
            EV((enum enhance_menu_event)dir);
    } else if (dir && now >= s_dir_next) {
        s_dir_next = now + REPEAT_NEXT;
        if (s_state == ST_OPEN)
            EV((enum enhance_menu_event)dir);
    }
#undef EV
    tip_track(now);
    if (redraw)
        s_need_draw = 1;
    s_prev_held = held;
    keys_latch();
}

/* ---- drawing -------------------------------------------------------------- */

static mu_Color with_alpha(mu_Color c, int a)
{
    c.a = (unsigned char)a;
    return c;
}

static int text_w(mu_Font f, const char *s)
{
    return nv2a_ovl_text_width(f, s, -1);
}

/* s cut to fit w with an ellipsis; returns the bytes to draw (out holds
 * the cut text). */
static const char *fit(mu_Font f, const char *s, int w, char *out, size_t n)
{
    size_t len = strlen(s);
    if (text_w(f, s) <= w)
        return s;
    while (len > 0) {
        len--;
        /* Back to a UTF-8 lead byte. */
        while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80)
            len--;
        snprintf(out, n, "%.*s\xE2\x80\xA6", (int)len, s);
        if (text_w(f, out) <= w)
            return out;
    }
    snprintf(out, n, "\xE2\x80\xA6");
    return out;
}

/* fit for text with tokens: cut only outside a token, so a glyph is
 * whole or gone. */
static const char *tok_fit(mu_Font f, const char *s, int w, char *out, size_t n)
{
    size_t len = strlen(s);
    if (tok_w(f, s) <= w)
        return s;
    while (len > 0) {
        const char *open;
        len--;
        while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80)
            len--;
        /* Inside a token: back to its brace. */
        for (open = s + len; open > s && open[-1] != '{' && open[-1] != '}'; open--)
            ;
        if (open > s && open[-1] == '{')
            len = (size_t)(open - 1 - s);
        snprintf(out, n, "%.*s\xE2\x80\xA6", (int)len, s);
        if (tok_w(f, out) <= w)
            return out;
    }
    snprintf(out, n, "\xE2\x80\xA6");
    return out;
}

static void draw_pill(mu_Context *ctx, mu_Rect r, int on, int dim)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    int k = r.h - 6, kx = on ? r.x + r.w - k - 3 : r.x + 3;
    nv2a_ovl_ui_rrect(ctx, r, r.h / 2, 1.0f, on ? (dim ? t->text_dim : t->accent) : t->bg_row);
    nv2a_ovl_ui_rrect(ctx, mu_rect(kx, r.y + 3, k, k), k / 2, 1.0f, on ? t->text : t->text_dim);
}

static mu_Rect mr(struct menu_rect r)
{
    return mu_rect(r.x, r.y, r.w, r.h);
}

/* The item a row sets, NULL for none; *sl its slot or NULL. */
static const struct enhance_item *row_item(const struct row *rw, const struct slot **sl)
{
    *sl = rw->type == R_SLOT ? &s_slot[rw->ref] : NULL;
    return *sl ? (*sl)->it : rw->type == R_PARAM ? &s_pitem[rw->ref]
         : rw->type == R_PROFILE ? &k_profile_item : NULL;
}

/* A row's value as drawn: with the setting's unit, a toggle's aside. */
static void row_uv(const struct row *rw, char *uv, size_t n)
{
    const struct slot *sl;
    const struct enhance_item *it = row_item(rw, &sl);
    const char *unit = NULL;
    if (sl && sl->dset >= 0) {
        int m;
        uint32_t k;
        unit = dset_of(it, &m, &k)->unit;
    }
    if (unit && *unit && it->kind != ENHANCE_ITEM_TOGGLE)
        snprintf(uv, n, "%s%s", rw->value, unit);
    else
        snprintf(uv, n, "%s", rw->value);
}

/* The row's control for the layout, and the value's width beside it. */
static int row_kind(const struct row *rw, mu_Font fb, mu_Font fv, int *vw)
{
    const struct slot *sl;
    const struct enhance_item *it = row_item(rw, &sl);
    char uv[VAL_MAX + 32];
    *vw = 0;
    if (rw->type == R_HEADER)
        return MENU_ROW_HEADER;
    if (rw->type == R_TEXT)
        return MENU_ROW_TEXT;
    if (rw->type == R_MOD || (it && it->kind == ENHANCE_ITEM_TOGGLE))
        return MENU_ROW_TOGGLE;
    if (it && (it->kind == ENHANCE_ITEM_RANGE || it->kind == ENHANCE_ITEM_CHOICE)) {
        row_uv(rw, uv, sizeof uv);
        *vw = text_w(it->kind == ENHANCE_ITEM_RANGE ? fv : fb, uv);
        return it->kind == ENHANCE_ITEM_RANGE ? MENU_ROW_RANGE : MENU_ROW_CHOICE;
    }
    return MENU_ROW_PLAIN;
}

static void draw_row(mu_Context *ctx, const struct row *rw, const struct menu_layout_row *vr,
                     int selected, float s)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    mu_Font fb = OVL_FONT(OVL_FONT_BODY, nv2a_ovl_px(s, t->font_body), 0);
    mu_Font fs = OVL_FONT(OVL_FONT_SMALL, nv2a_ovl_px(s, t->font_small), 0);
    mu_Font fv = OVL_FONT(OVL_FONT_BODY, nv2a_ovl_px(s, t->font_body), 1);
    mu_Rect r = mr(vr->r);
    int pad = nv2a_ovl_px(s, t->pad), th = nv2a_ovl_text_height(fb);
    int ty = r.y + (r.h - th) / 2, x = r.x + pad, right = r.x + r.w - pad;
    const struct slot *sl;
    const struct enhance_item *it = row_item(rw, &sl);
    const char *envname = sl ? sl->envname : rw->type == R_PROFILE ? shader_edit_forced() : NULL;
    int ro = (sl && sl->env) || (rw->type == R_PROFILE && envname);
    mu_Color tint = rw->tint == TINT_BAD ? t->bad : rw->tint == TINT_WARN ? t->warn : t->text_dim;
    char cut[VAL_MAX * 2], forced[96], uv[VAL_MAX + 32];
    const char *v;
    /* D10: a dot before the label while unsaved, and the value bright only
     * where it differs from stock, so a page shows what was changed. */
    int marked = rw->type == R_SLOT || rw->type == R_MOD || rw->type == R_BIND
              || rw->type == R_CTL || rw->type == R_PARAM || rw->type == R_PROFILE;
    int st = marked ? row_state(rw) : 0;
    mu_Color vcol = ro ? t->text_dim : !marked || (st & ENHANCE_ROW_NONDEFAULT) ? t->text
                                                                                : t->text_dim;

    if (rw->type == R_HEADER) {
        int hs = nv2a_ovl_text_height(fs), hy = r.y + r.h - hs - nv2a_ovl_px(s, 4);
        mu_draw_text(ctx, fs, rw->label, -1, mu_vec2(x, hy), t->accent);
        /* Cut to the room the label leaves: the Chain header's error holds
         * a path, and drawn whole it ran over the label and out of the
         * panel. */
        if (rw->value[0]) {
            v = fit(fs, rw->value, right - x - text_w(fs, rw->label) - nv2a_ovl_px(s, 12), cut,
                    sizeof cut);
            mu_draw_text(ctx, fs, v, -1, mu_vec2(right - text_w(fs, v), hy), tint);
        }
        return;
    }
    if (rw->type == R_TEXT) {
        /* The label column, then the wrapped line text_rows cut to fit. */
        mu_draw_text(ctx, fb, rw->label, -1, mu_vec2(x, ty), t->text);
        tok_draw(ctx, fb, rw->value, x + text_col(s), ty, rw->tint ? tint : t->text);
        return;
    }
    if (selected) {
        /* The inline hint is the row's too. */
        mu_Rect hl = r;
        if (vr->hint.h > 0)
            hl.h = vr->hint.y + vr->hint.h - r.y;
        nv2a_ovl_ui_rrect(ctx, hl, nv2a_ovl_px(s, t->radius) / 2, 1.0f, t->bg_hover);
        mu_draw_rect(ctx, mu_rect(hl.x, hl.y + nv2a_ovl_px(s, 6), nv2a_ovl_px(s, 3),
                                  hl.h - 2 * nv2a_ovl_px(s, 6)), t->accent);
    }
    if (vr->hint.h > 0 && rw->help[0]) {
        char hc[300];
        tok_draw(ctx, fs, tok_fit(fs, rw->help, vr->hint.w, hc, sizeof hc), vr->hint.x,
                 vr->hint.y, t->text_dim);
    }
    if (st & ENHANCE_ROW_UNSAVED) {
        /* In the row's left pad, between the selection bar and the label. */
        int d = nv2a_ovl_px(s, 6);
        nv2a_ovl_ui_rrect(ctx, mu_rect(x - d - nv2a_ovl_px(s, 4), r.y + (r.h - d) / 2, d, d), d / 2,
                          1.0f, t->accent);
    }
    /* The label, then its class: nothing for a change that shows now (the
     * common case), words for one that waits. */
    mu_draw_text(ctx, fb, rw->label, -1, mu_vec2(x, ty), ro || rw->dim ? t->text_dim : t->text);
    x += text_w(fb, rw->label) + nv2a_ovl_px(s, 8);
    if (it && it->kind != ENHANCE_ITEM_INFO) {
        int fsh = nv2a_ovl_text_height(fs), fy = r.y + (r.h - fsh) / 2;
        if (ro) {
            if (envname)
                snprintf(forced, sizeof forced, "set by %s", envname);
            else
                snprintf(forced, sizeof forced, "(environment)");
            mu_draw_text(ctx, fs, forced, -1, mu_vec2(x, fy), t->text_dim);
            x += text_w(fs, forced) + nv2a_ovl_px(s, 8);
        } else if (it->live == ENHANCE_LIVE || it->live == ENHANCE_LIVE_FLUSH) {
            /* Applies now: no mark (D10 keeps the dot for "unsaved"). */
        } else {
            const char *w = it->live == ENHANCE_LIVE_RESTART ? "applies at restart"
                          : it->live == ENHANCE_LIVE_ACTIVATE ? "next activation"
                          : "next stage";
            mu_draw_text(ctx, fs, w, -1, mu_vec2(x, fy),
                         it->live == ENHANCE_LIVE_RESTART ? t->warn : t->text_dim);
            x += text_w(fs, w) + nv2a_ovl_px(s, 8);
        }
        if (it->note) {
            mu_draw_text(ctx, fs, it->note, -1, mu_vec2(x, fy), t->text_dim);
            x += text_w(fs, it->note) + nv2a_ovl_px(s, 8);
        }
    } else if (rw->note && *rw->note) {
        /* A control's "while flying". */
        int fsh = nv2a_ovl_text_height(fs), fy = r.y + (r.h - fsh) / 2;
        mu_draw_text(ctx, fs, rw->note, -1, mu_vec2(x, fy), t->text_dim);
        x += text_w(fs, rw->note) + nv2a_ovl_px(s, 8);
    }
    row_uv(rw, uv, sizeof uv);

    if (rw->type == R_MOD || (it && it->kind == ENHANCE_ITEM_TOGGLE)) {
        int on = rw->type == R_MOD ? xbox_mod_enabled(rw->ref) : on_text(rw->value);
        int pw = vr->pill.w;
        draw_pill(ctx, mr(vr->pill), on, ro);
        if (rw->type == R_MOD) {
            enum xbox_mod_state st = xbox_mod_state(rw->ref);
            const char *w = st == XBOX_MOD_FAILED ? "failed"
                          : st == XBOX_MOD_RESTART || !xbox_mod_has_on_enable(rw->ref)
                              ? "applies at restart" : NULL;
            int fsh = nv2a_ovl_text_height(fs), fy = r.y + (r.h - fsh) / 2;
            if (w)
                mu_draw_text(ctx, fs, w, -1, mu_vec2(x, fy),
                             st == XBOX_MOD_FAILED ? t->bad : t->warn);
            /* Every mod has a page (D5). */
            mu_draw_text(ctx, fb, ">", -1, mu_vec2(right - pw - nv2a_ovl_px(s, 20), ty),
                         t->text_dim);
        }
        return;
    }
    if (it && it->kind == ENHANCE_ITEM_RANGE) {
        struct menu_rect tr = vr->track;
        int kn = nv2a_ovl_px(s, 14), cy = tr.y + tr.h / 2;
        double f = it->hi > it->lo ? (atof(rw->value) - it->lo) / (it->hi - it->lo) : 0;
        int fx;
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        fx = tr.x + (int)(f * tr.w + 0.5);
        nv2a_ovl_ui_rrect(ctx, mr(tr), tr.h / 2, 1.0f, t->bg_row);
        nv2a_ovl_ui_rrect(ctx, mu_rect(tr.x, tr.y, fx - tr.x, tr.h), tr.h / 2, 1.0f,
                          ro ? t->text_dim : t->accent);
        nv2a_ovl_ui_rrect(ctx, mu_rect(fx - kn / 2, cy - kn / 2, kn, kn), kn / 2, 1.0f, t->text);
        v = fit(fv, uv, vr->value.w, cut, sizeof cut);
        mu_draw_text(ctx, fv, v, -1, mu_vec2(vr->value.x + vr->value.w - text_w(fv, v), ty), vcol);
        return;
    }
    if (it && it->kind == ENHANCE_ITEM_CHOICE) {
        mu_draw_text(ctx, fb, ">", -1, mu_vec2(vr->next.x, ty), t->text_dim);
        mu_draw_text(ctx, fb, uv, -1, mu_vec2(vr->value.x, ty),
                     ro ? t->text_dim : rw->tint ? tint : vcol);
        mu_draw_text(ctx, fb, "<", -1, mu_vec2(vr->prev.x, ty), t->text_dim);
        return;
    }
    /* Text values (info, bindings, Save's path): right-aligned, cut to the
     * room the label left. */
    if (rw->type == R_CTL && s_cap.phase != CAP_OFF && s_cap.what != CAP_ACTION
        && s_cap.id == rw->ref) {
        v = fit(fb, s_cap.what == CAP_CTL_STICK ? "push a stick\xE2\x80\xA6"
                                               : "press a key, a click or a button\xE2\x80\xA6",
                right - x, cut, sizeof cut);
        mu_draw_text(ctx, fb, v, -1, mu_vec2(right - text_w(fb, v), ty), t->warn);
        return;
    }
    if (rw->type == R_BIND && s_cap.phase != CAP_OFF && s_cap.what == CAP_ACTION
        && s_cap.id == rw->ref) {
        v = fit(fb, "press a key or a chord\xE2\x80\xA6", right - x, cut, sizeof cut);
        mu_draw_text(ctx, fb, v, -1, mu_vec2(right - text_w(fb, v), ty), t->warn);
        return;
    }
    if (!rw->value[0])
        return;
    /* A binding row: its glyphs while they fit, else the names. */
    if (rw->glyph[0] && tok_w(fb, rw->glyph) <= right - x) {
        tok_draw(ctx, fb, rw->glyph, right - tok_w(fb, rw->glyph), ty,
                 marked && !rw->tint ? vcol : tint);
        return;
    }
    v = fit(fb, rw->value, right - x, cut, sizeof cut);
    mu_draw_text(ctx, fb, v, -1, mu_vec2(right - text_w(fb, v), ty),
                 marked && !rw->tint ? vcol : tint);
}

/* D9a: the unsaved-changes dialog over a dimmed panel. Go Back has the
 * focus when it opens; the access letter of each button is underlined. */
static void draw_dialog(mu_Context *ctx, const struct menu_layout *l)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    float s = l->s;
    mu_Font ft = OVL_FONT(OVL_FONT_BODY, nv2a_ovl_px(s, t->font_body), 1);
    mu_Font fs = OVL_FONT(OVL_FONT_SMALL, nv2a_ovl_px(s, t->font_small), 0);
    int pad = l->pad, lh = nv2a_ovl_text_height(fs), ly = l->dlg_line_y;
    struct menu_rect d = l->dlg;
    static const char k_access[DLG_BUTTONS] = {'N', 'S', 'G'};
    const char *c = s_dlg.body;
    nv2a_ovl_ui_rrect(ctx, mr(l->panel), nv2a_ovl_px(s, t->radius), 1.0f, with_alpha(t->bg, 170));
    nv2a_ovl_ui_shadow(ctx, mr(d), nv2a_ovl_px(s, t->radius), nv2a_ovl_px(s, t->shadow),
                       t->shadow_alpha);
    nv2a_ovl_ui_rrect(ctx, mr(d), nv2a_ovl_px(s, t->radius), 1.0f, t->bg_row);
    mu_draw_text(ctx, ft, "Unsaved changes", -1, mu_vec2(l->dlg_title.x, l->dlg_title.y), t->text);
    while (*c) {
        const char *e = strchr(c, '\n');
        char line[256], cut[300];
        size_t n = e ? (size_t)(e - c) : strlen(c);
        if (n >= sizeof line)
            n = sizeof line - 1;
        memcpy(line, c, n);
        line[n] = 0;
        mu_draw_text(ctx, fs, fit(fs, line, d.w - 2 * pad, cut, sizeof cut), -1,
                     mu_vec2(d.x + pad, ly),
                     s_dlg.err[0] && !strcmp(line, s_dlg.err) ? t->bad : t->text_dim);
        ly += l->dlg_line_h;
        c = e ? e + 1 : c + n;
    }
    for (int b = 0; b < DLG_BUTTONS; b++) {
        struct menu_rect br = l->dlg_btn[b];
        int off = b == DLG_SAVE && dlg_save_dead(), foc = b == s_dlg.focus;
        const char *lab = k_dlg_button[b], *a = strchr(lab, k_access[b]);
        mu_Color tc = off ? t->text_dim : t->text;
        int tx = br.x + l->dlg_btn_pad, ty = br.y + (br.h - lh) / 2;
        nv2a_ovl_ui_rrect(ctx, mr(br), nv2a_ovl_px(s, 6), 1.0f,
                          foc ? with_alpha(t->accent, off ? 60 : 110) : t->bg_hover);
        if (foc)
            mu_draw_rect(ctx, mu_rect(br.x, br.y + br.h - nv2a_ovl_px(s, 3), br.w, nv2a_ovl_px(s, 3)),
                         t->accent);
        mu_draw_text(ctx, fs, lab, -1, mu_vec2(tx, ty), tc);
        if (a) {
            /* Underline the access key: the width of the text before it. */
            char pre[64], one[2] = {*a, 0};
            size_t n = (size_t)(a - lab) < sizeof pre ? (size_t)(a - lab) : sizeof pre - 1;
            memcpy(pre, lab, n);
            pre[n] = 0;
            mu_draw_rect(ctx, mu_rect(tx + text_w(fs, pre), ty + lh - nv2a_ovl_px(s, 2),
                                      text_w(fs, one), nv2a_ovl_px(s, 1)),
                         tc);
        }
    }
}

static void draw_graph(mu_Context *ctx, mu_Rect g, float s)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    float ms[120], top = 33.4f;
    unsigned n = nv2a_stats_frames(ms, 120);
    int bw;
    nv2a_ovl_ui_rrect(ctx, g, nv2a_ovl_px(s, t->radius) / 2, 1.0f, t->bg_row);
    if (!n)
        return;
    for (unsigned i = 0; i < n; i++)
        if (ms[i] > top)
            top = ms[i];
    bw = g.w / 120 > 0 ? g.w / 120 : 1;
    for (unsigned i = 0; i < n; i++) {
        int h = (int)(ms[i] / top * (float)(g.h - 4) + 0.5f);
        mu_Color c = ms[i] < 17.5f ? t->good : ms[i] < 34.0f ? t->warn : t->bad;
        if (h < 1)
            h = 1;
        mu_draw_rect(ctx, mu_rect(g.x + g.w - (int)(n - i) * bw, g.y + g.h - 2 - h, bw - (bw > 2), h), c);
    }
}

/* Q10: the section toolbar, folded to icons or open with its labels over
 * the content's left edge. A dot marks a section Save has not written
 * (D10). */
static void draw_toolbar(mu_Context *ctx, const struct menu_layout *l, const int *pages)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    float s = l->s;
    mu_Font fb = OVL_FONT(OVL_FONT_BODY, nv2a_ovl_px(s, t->font_body), 0);
    int th = nv2a_ovl_text_height(fb), d = nv2a_ovl_px(s, 6), rad = nv2a_ovl_px(s, t->radius);
    if (l->open) {
        nv2a_ovl_ui_shadow(ctx, mr(l->bar), rad, nv2a_ovl_px(s, t->shadow), t->shadow_alpha);
        /* Opaque: it lies over the rows, whose text must not show through
         * the panel's translucent fill. */
        nv2a_ovl_ui_rrect(ctx, mr(l->bar), rad, 1.0f, with_alpha(t->bg, 255));
    }
    /* The rule between the icons and the section. */
    mu_draw_rect(ctx, mu_rect(l->bar.x + l->bar.w - 1, l->bar.y, 1, l->bar.h), t->bg_row);
    for (int i = 0; i < l->nsec; i++) {
        int p = pages[i], on = p == s_page;
        struct menu_rect e = l->sec[i], ic = l->sec_icon[i];
        if (e.h <= 0)
            continue; /* scrolled out of a short bar */
        if (on) {
            /* The pad's focus on the toolbar (Q12) lights the entry. */
            nv2a_ovl_ui_rrect(ctx, mr(e), rad / 2, 1.0f,
                              s_focus == MENU_FOCUS_TOOLBAR ? with_alpha(t->accent, 110)
                                                            : t->bg_hover);
            mu_draw_rect(ctx, mu_rect(e.x, e.y + nv2a_ovl_px(s, 6), nv2a_ovl_px(s, 3),
                                      e.h - 2 * nv2a_ovl_px(s, 6)), t->accent);
        }
        nv2a_ovl_ui_glyph(ctx, nv2a_ovl_glyph_icon(k_page_icon[p]), mr(ic),
                          on ? t->text : t->text_dim);
        if (page_unsaved(p))
            nv2a_ovl_ui_rrect(ctx, mu_rect(ic.x + ic.w - d / 2, ic.y - d / 2, d, d), d / 2, 1.0f,
                              t->accent);
        if (l->sec_label[i].w > 0) {
            char cut[64];
            struct menu_rect lr = l->sec_label[i];
            mu_draw_text(ctx, fb, fit(fb, s_page_name[p], lr.w, cut, sizeof cut), -1,
                         mu_vec2(lr.x, lr.y + (lr.h - th) / 2), on ? t->text : t->text_dim);
        }
    }
    /* No room for labels: the shown section's name beside its icon. */
    if (l->tooltip.w > 0) {
        struct menu_rect tr = l->tooltip;
        int sec = -1;
        for (int i = 0; i < l->nsec; i++)
            if (pages[i] == s_page)
                sec = i;
        if (sec >= 0) {
            if (tr.x + tr.w > l->panel.x + l->panel.w)
                tr.w = l->panel.x + l->panel.w - tr.x;
            nv2a_ovl_ui_shadow(ctx, mr(tr), rad / 2, nv2a_ovl_px(s, t->shadow), t->shadow_alpha);
            nv2a_ovl_ui_rrect(ctx, mr(tr), rad / 2, 1.0f, with_alpha(t->bg_row, 255));
            mu_draw_text(ctx, fb, s_page_name[s_page], -1,
                         mu_vec2(tr.x + l->pad, tr.y + (tr.h - th) / 2), t->text);
        }
    }
}

/* The header (D1): the title and the game, then ? (hints), the size and
 * close as icons. */
static void draw_header(mu_Context *ctx, const struct menu_layout *l)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    mu_Font ft = OVL_FONT(OVL_FONT_TITLE, nv2a_ovl_px(l->s, t->font_title), 0);
    char title[200], cut[260];
    static const uint32_t k_hb[MENU_HB_COUNT] = {
        NV2A_OVL_ICON_HELP, NV2A_OVL_ICON_MAXIMIZE, NV2A_OVL_ICON_CLOSE
    };
    const char *game = nv2a_overlay_game();
    if (game[0])
        snprintf(title, sizeof title, "Enhancements \xC2\xB7 %s", game);
    else
        snprintf(title, sizeof title, "Enhancements");
    mu_draw_text(ctx, ft, fit(ft, title, l->title.w, cut, sizeof cut), -1,
                 mu_vec2(l->title.x, l->title.y), t->text);
    for (int b = 0; b < MENU_HB_COUNT; b++) {
        uint32_t cp = b == MENU_HB_SIZE && s_size == MENU_SIZE_EXPANDED ? NV2A_OVL_ICON_MINIMIZE
                                                                         : k_hb[b];
        /* The hints button is lit while they show inline. */
        mu_Color c = b == MENU_HB_HINTS && s_hints == MENU_HINTS_INLINE ? t->accent : t->text_dim;
        nv2a_ovl_ui_glyph(ctx, nv2a_ovl_glyph_icon(cp), mr(l->hbtn[b]), c);
    }
}

/* The footer's control pairs per device (D9): the first ones matter most,
 * as the narrow footer keeps them and drops from the right. */
#define FOOT_PAIRS_MAX 8
static const char *const k_hint_pad[FOOT_PAIRS_MAX] = {
    "{pad:south} Select", "{pad:east} Back", "{pad:lb}{pad:rb} Section", "{pad:west} Clear",
    "{pad:l3} Hints", NULL,
};
static const char *const k_hint_key[FOOT_PAIRS_MAX] = {
    "{key:enter} Select", "{key:esc} Back", "{key:tab} Focus", "{key:q}{key:e} Section",
    "{key:F1} Hints", NULL,
};
static const char *const k_hint_mouse[FOOT_PAIRS_MAX] = {
    "{mouse:left} Select", "{mouse:right} Back", "{mouse:wheel_up} Scroll", "{key:F1} Hints", NULL,
};
static const char *const k_hint_name[] = {
    "Type a name", "{key:enter} Keep", "{key:esc} Cancel",
};

/* Steam runs the game (Steam + X raises its keyboard, D4). */
static int steam_present(void)
{
    const char *id = recomp_env(RENV_STEAM_GAME_ID), *deck = recomp_env(RENV_STEAM_DECK);
    return (id && *id) || (deck && !strcmp(deck, "1"));
}

/* D11's card: the selected row's help, under the row (above it when the
 * viewport's foot is near), up to three lines. */
static void draw_tooltip(mu_Context *ctx, const struct menu_layout *l, const struct row *rw)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    mu_Font fs = OVL_FONT(OVL_FONT_SMALL, nv2a_ovl_px(l->s, t->font_small), 0);
    const struct menu_layout_row *vr = NULL;
    const char *p = rw->help;
    size_t take[3];
    int nl = 0, lh = nv2a_ovl_text_height(fs), pad = l->pad, w, h, x, y, tw = 0;
    int rad = nv2a_ovl_px(l->s, t->radius) / 2;
    for (int i = 0; i < l->nvrow; i++)
        if (l->row[i].index == s_sel[s_page])
            vr = &l->row[i];
    if (!vr)
        return;
    w = l->content.w * 3 / 4;
    while (*p && nl < 3) {
        char line[200];
        take[nl] = wrap_take(fs, p, w - 2 * pad, sizeof line - 1);
        if (!take[nl])
            break;
        snprintf(line, sizeof line, "%.*s", (int)take[nl], p);
        if (tok_w(fs, line) > tw)
            tw = tok_w(fs, line);
        p += take[nl++];
        while (*p == ' ' || *p == '\n')
            p++;
    }
    if (!nl)
        return;
    w = tw + 2 * pad;
    h = nl * lh + 2 * (pad / 2);
    x = vr->r.x + pad;
    y = vr->r.y + vr->r.h + l->gap / 2;
    if (y + h > l->viewport.y + l->viewport.h)
        y = vr->r.y - l->gap / 2 - h;
    nv2a_ovl_ui_shadow(ctx, mu_rect(x, y, w, h), rad, nv2a_ovl_px(l->s, t->shadow), t->shadow_alpha);
    nv2a_ovl_ui_rrect(ctx, mu_rect(x, y, w, h), rad, 1.0f, with_alpha(t->bg_row, 255));
    p = rw->help;
    for (int i = 0; i < nl; i++) {
        char line[200];
        snprintf(line, sizeof line, "%.*s", (int)take[i], p);
        tok_draw(ctx, fs, line, x + pad, y + pad / 2 + i * lh, t->text);
        p += take[i];
        while (*p == ' ' || *p == '\n')
            p++;
    }
}

/* D4's keyboard, docked under the name's row (above it near the foot). */
static void draw_osk(mu_Context *ctx, const struct menu_layout *l)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    float s = l->s;
    mu_Font fs = OVL_FONT(OVL_FONT_SMALL, nv2a_ovl_px(s, t->font_small), 0);
    struct menu_rect f = sel_row_rect();
    int lh = nv2a_ovl_text_height(fs), rad = nv2a_ovl_px(s, 6);
    enhance_osk_layout(f.x, f.y, f.w, f.h, l->place.w, l->place.h, nv2a_ovl_px(s, 44),
                       nv2a_ovl_px(s, 6), &s_osk_lay);
    s_osk_lay_ok = 1;
    {
        mu_Rect pr = mu_rect(s_osk_lay.panel.x, s_osk_lay.panel.y, s_osk_lay.panel.w,
                             s_osk_lay.panel.h);
        nv2a_ovl_ui_shadow(ctx, pr, rad, nv2a_ovl_px(s, t->shadow), t->shadow_alpha);
        nv2a_ovl_ui_rrect(ctx, pr, rad, 1.0f, with_alpha(t->bg, 255));
    }
    for (int r = 0; r < OSK_ROWS; r++)
        for (int c = 0; c < OSK_COLS; c++) {
            const struct osk_rect *k = &s_osk_lay.cell[r][c];
            const char *lab;
            int first, kind, foc = r == s_osk.row && c == s_osk.col, lw;
            if (!s_osk_lay.span[r][c])
                continue;
            kind = enhance_osk_key(r, c, s_osk.shift, &first, &lab);
            nv2a_ovl_ui_rrect(ctx, mu_rect(k->x, k->y, k->w, k->h), rad, 1.0f,
                              foc ? with_alpha(t->accent, 110)
                              : kind == OSK_KEY_SHIFT && s_osk.shift ? with_alpha(t->accent, 60)
                              : t->bg_row);
            if (foc)
                mu_draw_rect(ctx, mu_rect(k->x, k->y + k->h - nv2a_ovl_px(s, 3), k->w,
                                          nv2a_ovl_px(s, 3)), t->accent);
            if (!lab)
                continue;
            lw = text_w(fs, lab);
            mu_draw_text(ctx, fs, lab, -1, mu_vec2(k->x + (k->w - lw) / 2, k->y + (k->h - lh) / 2),
                         t->text);
        }
}

void enhance_menu_draw(uint64_t now_ms)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    static unsigned char kinds[ROWS_MAX];
    static int vw[ROWS_MAX];
    const struct menu_layout *l = &s_lay;
    struct menu_layout_in in;
    mu_Context *ctx;
    int pages[PAGE_COUNT], sel, un, news, nl = 1;
    float s;
    mu_Font fb, fs, fv;
    char right[160];

    s_now = now_ms;
    if (s_state != ST_OPEN)
        return;
    ctx = nv2a_overlay_ui_begin(NV2A_OVL_MENU);
    if (!ctx)
        return;
    rates(now_ms);
    build_rows();
    /* One layout for the draw, the wrap (text_rows) and the hit test: two
     * copies could drift and clip or overlap a wrapped row. */
    layout_base(&in, pages);
    s = in.s;
    fb = OVL_FONT(OVL_FONT_BODY, nv2a_ovl_px(s, t->font_body), 0);
    fs = OVL_FONT(OVL_FONT_SMALL, nv2a_ovl_px(s, t->font_small), 0);
    fv = OVL_FONT(OVL_FONT_BODY, nv2a_ovl_px(s, t->font_body), 1);
    for (int i = 0; i < s_nrow; i++)
        kinds[i] = (unsigned char)row_kind(&s_row[i], fb, fv, &vw[i]);
    in.toolbar_open = enhance_menu_toolbar_open(now_ms);
    in.nrow = s_nrow;
    in.scroll = s_scroll[s_page];
    in.sel = s_sel[s_page];
    in.free_scroll = s_free_scroll;
    in.hint_h = s_hints == MENU_HINTS_INLINE ? in.small_h + nv2a_ovl_px(s, 4) : 0;
    in.row_kind = kinds;
    in.row_value_w = vw;
    in.extra_h = s_page == PAGE_STATS ? nv2a_ovl_px(s, 110) : 0;
    un = enhance_menu_unsaved();
    news = s_status[0] && s_status_t && now_ms - s_status_t < STATUS_MS;
    if (news)
        snprintf(right, sizeof right, "%s", s_status);
    else if (un)
        snprintf(right, sizeof right, "%d unsaved", un);
    else
        snprintf(right, sizeof right, "All saved");
    in.summary_w = text_w(fs, right);
    in.dlg_on = s_dlg.on;
    for (const char *c = s_dlg.body; *c; c++)
        nl += *c == '\n';
    in.dlg_lines = nl;
    for (int b = 0; b < DLG_BUTTONS; b++)
        in.dlg_btn_w[b] = text_w(fs, k_dlg_button[b]);
    menu_layout_compute(&in, &s_lay);
    s_lay_ok = 1;
    memcpy(s_lay_pages, pages, sizeof s_lay_pages);
    s_scroll[s_page] = l->scroll;
    s_draw_w = in.w;
    s_draw_h = in.h;
    sel = s_sel[s_page];

    /* Expanded (D1) covers the game; compact leaves it beside the panel,
     * so a live setting shows as it changes. */
    if (l->backdrop)
        mu_draw_rect(ctx, mr(l->place), with_alpha(t->bg, 217));
    nv2a_ovl_ui_shadow(ctx, mr(l->panel), nv2a_ovl_px(s, t->radius), nv2a_ovl_px(s, t->shadow),
                       t->shadow_alpha);
    nv2a_ovl_ui_rrect(ctx, mr(l->panel), nv2a_ovl_px(s, t->radius), 1.0f, t->bg);
    draw_header(ctx, l);

    /* The section's intro (A.8), then its rows. */
    if (l->intro.w > 0) {
        const char *intro = enhance_menu_page_intro_text(s_page);
        char cut[400];
        tok_draw(ctx, fs, tok_fit(fs, intro, l->intro.w, cut, sizeof cut), l->intro.x,
                 l->intro.y, t->text_dim);
    }
    mu_push_clip_rect(ctx, mr(l->viewport));
    for (int i = 0; i < l->nvrow; i++) {
        int r = l->row[i].index;
        draw_row(ctx, &s_row[r], &l->row[i], r == sel && s_row[r].sel, s);
    }
    mu_pop_clip_rect(ctx);
    if (l->scrollbar)
        nv2a_ovl_ui_rrect(ctx, mr(l->sb_thumb), l->sb_thumb.w / 2, 1.0f, t->text_dim);
    if (l->extra.h > 0)
        draw_graph(ctx, mr(l->extra), s);
    /* After the rows: open, the bar lies over their left edge. */
    draw_toolbar(ctx, l, pages);

    /* The footer (D9): the device's controls on the left as glyph pairs,
     * dropped from the right when the room runs out, then the unsaved
     * count (or a status for STATUS_MS) and the Cancel and Save buttons
     * with the shortcut that presses each (Q11). */
    {
        const char *const *pairs;
        const char *one[1];
        int np, dev = menu_device(), fit_n, sep = nv2a_ovl_px(s, 16), pw[FOOT_PAIRS_MAX];
        char cut[200];
        const char *rt;
        int lh = nv2a_ovl_text_height(fs), fy = l->hints.y + (l->hints.h - lh) / 2;
        int save_off = !un || dlg_save_off(), hx = l->hints.x;
        if (s_name_on && s_osk.on) {
            one[0] = enhance_osk_hint(steam_present());
            pairs = one;
            np = 1;
        } else if (s_name_on) {
            pairs = k_hint_name;
            np = (int)(sizeof k_hint_name / sizeof k_hint_name[0]);
        } else {
            pairs = dev == ENHANCE_DEV_PAD ? k_hint_pad : dev == ENHANCE_DEV_MOUSE ? k_hint_mouse
                                                                                 : k_hint_key;
            for (np = 0; np < FOOT_PAIRS_MAX && pairs[np]; np++)
                ;
        }
        for (int i = 0; i < np; i++)
            pw[i] = enhance_text_width(fs, pairs[i], XBOX_PAD_FAMILY_AUTO, dev);
        fit_n = menu_hints_fit(pw, np, sep, l->hints.w);
        if (!fit_n && s_name_on && s_osk.on) {
            /* No room for Steam's own keyboard too: the keys alone. */
            one[0] = enhance_osk_hint(0);
            pw[0] = enhance_text_width(fs, one[0], XBOX_PAD_FAMILY_AUTO, dev);
            fit_n = menu_hints_fit(pw, 1, sep, l->hints.w);
        }
        for (int i = 0; i < fit_n; i++) {
            enhance_text_draw(ctx, fs, pairs[i], hx, fy, t->text_dim, XBOX_PAD_FAMILY_AUTO, dev);
            hx += pw[i] + sep;
        }
        if (l->summary.w > 0) {
            rt = fit(fs, right, l->summary.w, cut, sizeof cut);
            mu_draw_text(ctx, fs, rt, -1,
                         mu_vec2(l->summary.x + l->summary.w - text_w(fs, rt), fy),
                         news ? t->text : un ? t->accent : t->text_dim);
        }
        for (int b = 0; b < MENU_FB_COUNT; b++) {
            struct menu_rect br = l->fbtn[b];
            int off = b == MENU_FB_SAVE && save_off;
            int foc = s_focus == MENU_FOCUS_FOOTER && b == s_foot_sel;
            const char *lab = foot_label(b);
            int lw;
            if (br.w <= 0)
                continue;
            nv2a_ovl_ui_rrect(ctx, mr(br), nv2a_ovl_px(s, 6), 1.0f,
                              foc ? with_alpha(t->accent, off ? 60 : 110)
                              : b == MENU_FB_SAVE && !off ? with_alpha(t->accent, 60) : t->bg_row);
            if (foc)
                mu_draw_rect(ctx, mu_rect(br.x, br.y + br.h - nv2a_ovl_px(s, 3), br.w,
                                          nv2a_ovl_px(s, 3)), t->accent);
            if (!l->foot_glyph) {
                /* No room for the glyph: the bare word, cut to the button. */
                lab = fit(fs, k_foot_bare[b], br.w - 2 * l->fbtn_pad, cut, sizeof cut);
                lw = text_w(fs, lab);
                mu_draw_text(ctx, fs, lab, -1, mu_vec2(br.x + (br.w - lw) / 2, br.y + (br.h - lh) / 2),
                             off ? t->text_dim : t->text);
                continue;
            }
            lw = enhance_text_width(fs, lab, XBOX_PAD_FAMILY_AUTO, foot_device());
            enhance_text_draw(ctx, fs, lab, br.x + (br.w - lw) / 2, br.y + (br.h - lh) / 2,
                              off ? t->text_dim : t->text, XBOX_PAD_FAMILY_AUTO, foot_device());
        }
    }
    s_tip_drawn = 0;
    if (tip_shown(now_ms)) {
        draw_tooltip(ctx, l, &s_row[sel]);
        s_tip_drawn = 1;
    }
    if (s_name_on && s_osk.on)
        draw_osk(ctx, l);
    else
        s_osk_lay_ok = 0;
    if (l->dlg_on)
        draw_dialog(ctx, l);
    nv2a_overlay_ui_publish(NV2A_OVL_MENU);
    s_draw_t = now_ms ? now_ms : 1;
    s_need_draw = 0;
    if (s_peek_t && now_ms - s_peek_t >= PEEK_MS)
        s_peek_drawn = 1;
}

/* ---- the hook --------------------------------------------------------------- */

static void neutral(unsigned port, XBOX_INPUT_STATE *st)
{
    memset(&st->Gamepad, 0, sizeof st->Gamepad);
    st->dwPacketNumber = s_fpkt[port];
}

void enhance_menu_hook(unsigned port, uint32_t held, XBOX_INPUT_STATE *st, uint64_t now_ms)
{
    int release = 0;
    if (port >= PORTS)
        return;
    if (!nv2a_overlay_menu_trylock()) {
        /* The other thread steps; the title still reads no pad. */
        if (st && __atomic_load_n(&s_state, __ATOMIC_RELAXED) != ST_CLOSED)
            neutral(port, st);
        return;
    }
    s_now = now_ms;
    s_held[port] = held;
    switch (s_state) {
    case ST_CLOSED:
        if (st)
            s_pkt[port] = st->dwPacketNumber;
        /* A Cmd+S while the menu is shut is the host's, not a later Save. */
        xbox_InputShortcutTake('S');
        if (enhance_actions_menu_request()) {
            uint32_t all = 0;
            for (int p = 0; p < PORTS; p++)
                all |= s_held[p];
            enhance_menu_open(now_ms);
            /* What is held as it opens (the chord that opened it) is no
             * press. */
            s_prev_held = all;
            s_dir = -1;
            keys_latch();
        }
        break;
    case ST_OPEN:
        /* Quiet keeps menu.open from firing during a capture; the phase
         * test stays in case a request was queued before it began. */
        if (enhance_actions_menu_request() && s_cap.phase == CAP_OFF) {
            /* The toggle asks first, as B does; in the dialog it is Go
             * Back. What it held is spent: no Cancel when it lets go. */
            s_rel_taint |= s_rel_armed;
            if (s_dlg.on)
                dlg_event(ENHANCE_EV_CLOSE);
            else
                leave_request();
        } else {
            input_step(now_ms);
        }
        break;
    case ST_CLOSING:
        /* A Cmd+S as the menu shuts is spent, not a Save when it reopens. */
        xbox_InputShortcutTake('S');
        if (enhance_actions_menu_request()) {
            enhance_menu_open(now_ms);
            keys_latch();
        }
        break;
    default:
        break;
    }
    if (s_state == ST_OPEN) {
        /* Shaders: the parameters appear once the backend built the chain. */
        int w, h, px, py, live = s_page == PAGE_STATS || s_page == PAGE_DEBUG
                                 || s_page == PAGE_SHADERS
                                 || s_page == PAGE_PACK || s_page == PAGE_CONTROLS;
        nv2a_overlay_placement(&px, &py, &w, &h);
        if (w <= 0 || h <= 0)
            w = 1920, h = 1080;
        if (s_need_draw || w != s_draw_w || h != s_draw_h
            || (live && now_ms - s_draw_t >= LIVE_DRAW_MS)
            || (s_status_t && now_ms - s_status_t >= STATUS_MS && now_ms - s_draw_t >= STATUS_MS)
            || (s_peek_t && !s_peek_drawn && now_ms - s_peek_t >= PEEK_MS)
            || tip_shown(now_ms) != s_tip_drawn)
            enhance_menu_draw(now_ms);
    }
    if (s_state == ST_CLOSING) {
        uint32_t all = 0;
        for (int p = 0; p < PORTS; p++)
            all |= s_held[p];
        if ((!all && !any_key_down()) || now_ms - s_close_t >= CLOSE_MS) {
            s_state = ST_CLOSED;
            release = 1;
        }
    }
    /* The closing poll too: the filter took this poll's mask before the
     * release below marks the held buttons. */
    if (st && (s_state != ST_CLOSED || release))
        neutral(port, st);
    nv2a_overlay_menu_unlock();
    /* A button still down past the close is the title's again only once
     * it is let go, so the closing press never reaches the game. */
    if (release)
        for (unsigned p = 0; p < PORTS; p++)
            enhance_actions_mask_until_release(p, 0xFFFFFFFFu);
}

/* ---- start ------------------------------------------------------------------ */

void enhance_menu_start(void)
{
    char binds[160], l[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];
    int id, n;
    size_t at = 0;
    /* Read before the off return: a menu that is off still owns the key,
     * so it is no "unused key". */
    s_size = enhance_cfg_choice("overlay.menu_size", k_menu_size, MENU_SIZE_COMPACT);
    /* The mods' panels and glyph names work with the menu off too. */
    enhance_mod_status_start();
    s_hints = enhance_cfg_choice("overlay.menu_hints", k_menu_hints, MENU_HINTS_TOOLTIP);
    if (!enhance_actions_keyed("menu.enabled")) {
        fprintf(stderr, LOGP "menu.enabled=off\n");
        return;
    }
    if (nv2a_overlay_arm(1u << NV2A_OVL_MENU, 1080) < 0) {
        fprintf(stderr, LOGP "menu.enabled=on but the overlay could not arm; no menu\n");
        return;
    }
    enhance_actions_set_menu_hook(enhance_menu_hook);
    id = enhance_action_find("menu.open");
    n = id >= 0 ? enhance_action_bindings(id, l, ENHANCE_ACTION_BINDS) : 0;
    binds[0] = 0;
    for (int i = 0; i < n && at < sizeof binds; i++)
        at += (size_t)snprintf(binds + at, sizeof binds - at, "%s%s", i ? ", " : "", l[i]);
    fprintf(stderr, LOGP "menu.enabled=on (%s)\n", n ? binds : "menu.open unbound");
}

/* ---- introspection ---------------------------------------------------------- */

int enhance_menu_page_count(void) { return PAGE_COUNT; }

const char *enhance_menu_page_name(int page)
{
    return page >= 0 && page < PAGE_COUNT ? s_page_name[page] : NULL;
}

int enhance_menu_page(void) { return s_page; }

int enhance_menu_page_shown(int page)
{
    return page >= 0 && page < PAGE_COUNT && page_shown(page);
}

int enhance_menu_set_page(const char *name)
{
    int p = page_by_name(name, -1);
    if (p < 0 || !page_shown(p))
        return 0;
    page_to(p);
    return 1;
}

int enhance_menu_rows(void)
{
    build_rows();
    return s_nrow;
}

int enhance_menu_selected(void)
{
    build_rows();
    return s_sel[s_page];
}

int enhance_menu_row(int i, char *label, size_t ln, char *value, size_t vn, int *selectable)
{
    if (i < 0 || i >= s_nrow)
        return 0;
    if (label && ln)
        snprintf(label, ln, "%s", s_row[i].label);
    if (value && vn) {
        if (s_row[i].type == R_MOD)
            snprintf(value, vn, "%s", xbox_mod_enabled(s_row[i].ref) ? "true" : "false");
        else
            snprintf(value, vn, "%s", s_row[i].value);
    }
    if (selectable)
        *selectable = s_row[i].sel;
    return 1;
}

int enhance_menu_row_live(int i)
{
    if (i >= 0 && i < s_nrow && (s_row[i].type == R_PARAM || s_row[i].type == R_PROFILE
                                 || s_row[i].type == R_STAGE))
        return ENHANCE_LIVE;
    if (i < 0 || i >= s_nrow || s_row[i].type != R_SLOT)
        return -1;
    return (int)s_slot[s_row[i].ref].it->live;
}

const char *enhance_menu_row_help(int i)
{
    return i >= 0 && i < s_nrow ? s_row[i].help : NULL;
}

const char *enhance_menu_row_glyph(int i)
{
    return i >= 0 && i < s_nrow ? s_row[i].glyph : NULL;
}

int enhance_menu_select_row(const char *label)
{
    build_rows();
    for (int i = 0; i < s_nrow; i++)
        if (s_row[i].sel && !strcmp(s_row[i].label, label)) {
            s_sel[s_page] = i;
            return 1;
        }
    return 0;
}

const char *enhance_menu_status(void) { return s_status; }

int enhance_menu_row_state(int i)
{
    return i >= 0 && i < s_nrow ? row_state(&s_row[i]) : 0;
}

int enhance_menu_dialog(void)
{
    return s_dlg.on ? s_dlg.focus : -1;
}

const char *enhance_menu_dialog_text(void)
{
    return s_dlg.on ? s_dlg.body : "";
}

int enhance_menu_page_intro(const char *page, const char *text)
{
    int p = page_by_name(page, -1);
    if (p < 0)
        return 0;
    s_page_intro[p] = text;
    return 1;
}

const char *enhance_menu_page_intro_text(int page)
{
    if (page < 0 || page >= PAGE_COUNT)
        return NULL;
    return s_page_intro[page] ? s_page_intro[page] : k_page_intro[page];
}

int enhance_menu_hints_inline(void)
{
    return s_hints == MENU_HINTS_INLINE;
}

int enhance_menu_osk(void)
{
    return s_name_on && s_osk.on;
}

const struct menu_layout *enhance_menu_last_layout(void)
{
    return s_lay_ok ? &s_lay : NULL;
}

int enhance_menu_hints(void)
{
    return s_hints;
}

int enhance_menu_focus(void)
{
    return s_focus;
}

int enhance_menu_footer_selected(void)
{
    return s_focus == MENU_FOCUS_FOOTER ? s_foot_sel : -1;
}

int enhance_menu_device(void)
{
    return menu_device();
}

int enhance_menu_tooltip(uint64_t now_ms)
{
    return tip_shown(now_ms);
}

int enhance_menu_size(void)
{
    return s_size;
}

void enhance_menu_set_toolbar_focus(int on)
{
    focus_to(on ? MENU_FOCUS_TOOLBAR : MENU_FOCUS_CONTENT);
    s_need_draw = 1;
}

int enhance_menu_toolbar_open(uint64_t now_ms)
{
    return s_focus == MENU_FOCUS_TOOLBAR || (s_peek_t && now_ms - s_peek_t < PEEK_MS);
}

void enhance_menu_reset_for_test(void)
{
    if (s_snap_on)
        nv2a_tex_pack_snapshot_want(0);
    if (s_stats_on)
        nv2a_stats_use(NV2A_STATS_PAGE, 0);
    s_snap_on = s_stats_on = 0;
    enhance_input_menu_hold(0);
    s_state = ST_CLOSED;
    s_page = 0;
    s_shaders_off = s_shaders_said = 0;
    memset(s_sel, 0, sizeof s_sel);
    memset(s_scroll, 0, sizeof s_scroll);
    s_mod_sub = -1;
    s_confirm = 0;
    shader_edit_reset_for_test();
    sh_reset_state();
    s_name_on = 0;
    memset(&s_osk, 0, sizeof s_osk);
    s_osk_lay_ok = 0;
    xbox_InputTextReset();
    s_npick = 0;
    s_status[0] = 0;
    s_status_t = s_now = 0;
    s_nrow = 0;
    memset(&s_cap, 0, sizeof s_cap);
    enhance_actions_quiet(0);
    memset(s_held, 0, sizeof s_held);
    s_prev_held = 0;
    memset(s_kprev, 0, sizeof s_kprev);
    memset(s_pkt, 0, sizeof s_pkt);
    memset(s_fpkt, 0, sizeof s_fpkt);
    s_dir = 0;
    s_kbd = 0;
    s_mouse = 0;
    s_kbd_seen = 0;
    s_ptr_seq = 0;
    s_ptr_ax = s_ptr_ay = 0;
    s_ptr_x = s_ptr_y = -1;
    s_ptr_btn = 0;
    s_drag_row = -1;
    s_free_scroll = 0;
    s_hover_row = -1;
    s_tip_t = 0;
    s_tip_sel = s_tip_page = -1;
    s_lay_ok = 0;
    s_tip_drawn = 0;
    s_focus = MENU_FOCUS_CONTENT;
    s_foot_sel = MENU_FB_CANCEL;
    xbox_InputMouseCursorHide(0);
    s_dir_next = s_close_t = s_draw_t = 0;
    memset(s_extra_t, 0, sizeof s_extra_t);
    s_need_draw = 0;
    s_draw_w = s_draw_h = 0;
    s_rate_t0 = 0;
    memset(s_rate, 0, sizeof s_rate);
    memset(s_abase_have, 0, sizeof s_abase_have);
    memset(s_abase_n, 0, sizeof s_abase_n);
    memset(&s_dlg, 0, sizeof s_dlg);
    memset(s_page_intro, 0, sizeof s_page_intro);
    s_hints = MENU_HINTS_TOOLTIP;
    s_size = MENU_SIZE_COMPACT;
    s_peek_t = 0;
    s_peek_drawn = 0;
    s_rel_armed = s_rel_taint = 0;
    s_nslot = 0;
    s_nset = 0;
    s_nptab = 0;
    s_pkey_active[0] = 0;
    s_rw_done = 0;
    s_built = 0;
    s_ntitle = 0;
    s_pack_off = 0;
}
