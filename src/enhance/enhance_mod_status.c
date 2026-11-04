/*
 * enhance_mod_status.c - the mods' status panels and the device glyph_name
 * names a binding for (enhance_mod_status.h, overlay-revamp D12a).
 *
 * mod.c keeps each mod's text under its lock; the flip tick copies the
 * texts out, and composes the cards on NV2A_OVL_STATUS when a text, the
 * device, the pad family or the placement changed, or every REFRESH_MS
 * so a rebinding in the menu shows without anyone telling the panel.
 */
#include "enhance_mod_status.h"

#include <stdio.h>
#include <string.h>

#include "enhance_actions.h"
#include "enhance_menu.h"
#include "enhance_text.h"
#include "host_pad.h"
#include "keyboard.h"
#include "mod.h"
#include "nv2a_overlay.h"
#include "nv2a_overlay_int.h"

/* How often a shown panel is drawn again with nothing changed: the
 * bindings it expands can change under it (the menu's rebinding). */
#define REFRESH_MS 250u
/* A stick past this (of 32767) is the player using the pad; the drift of
 * a worn stick stays under it. */
#define STICK_USE 8000
/* A pad source at half travel counts as pressed (triggers included). */
#define SRC_USE 16384

#define PADS 4
#define VKS 256

static int s_device = ENHANCE_DEV_PAD;
static unsigned char s_src_down[PADS][INPUT_SRC_COUNT];
static unsigned char s_stick_out[PADS][2];
static unsigned char s_vk_down[VKS];

/* The tick's own state: the flip thread is its only caller. */
static uint32_t s_serial = ~0u;
static int s_shown_device = -1, s_shown_family = -2, s_shown_w, s_shown_h;
static uint64_t s_shown_atlas;      /* the atlas generation of the last compose */
static uint64_t s_shown_ms;
static int s_shown;
/* Bumped by each stop, from whichever thread disarmed: a tick that sees
 * it move starts over, and one in flight across it hides what it drew. */
static uint32_t s_stop_gen, s_seen_gen;

int enhance_mod_status_device(void)
{
    return __atomic_load_n(&s_device, __ATOMIC_RELAXED);
}

void enhance_mod_status_set_device(int device)
{
    __atomic_store_n(&s_device, device, __ATOMIC_RELAXED);
}

static int is_mouse_vk(int vk)
{
    return vk == 0x01 || vk == 0x02 || vk == 0x04 || vk == 0x05 || vk == 0x06;
}

/* The device the player used last: whichever saw a press (or a stick
 * pushed out) most recently. Edges only, so a key held down does not
 * keep winning over a pad in use. */
static void track_device(void)
{
    int dev = -1;
    for (int p = 0; p < PADS; p++) {
        InputRaw raw;
        int out[2];
        xbox_InputHostRaw((DWORD)p, &raw);
        for (int k = 1; k < INPUT_SRC_COUNT; k++) {
            int down = raw.src[k] >= SRC_USE;
            if (down && !s_src_down[p][k])
                dev = ENHANCE_DEV_PAD;
            s_src_down[p][k] = (unsigned char)down;
        }
        out[0] = raw.lx > STICK_USE || raw.lx < -STICK_USE || raw.ly > STICK_USE
                 || raw.ly < -STICK_USE;
        out[1] = raw.rx > STICK_USE || raw.rx < -STICK_USE || raw.ry > STICK_USE
                 || raw.ry < -STICK_USE;
        for (int s = 0; s < 2; s++) {
            if (out[s] && !s_stick_out[p][s])
                dev = ENHANCE_DEV_PAD;
            s_stick_out[p][s] = (unsigned char)out[s];
        }
    }
    for (int vk = 1; vk < VKS; vk++) {
        int down = xbox_FramebufferKeyDown(vk) != 0;
        if (down && !s_vk_down[vk])
            dev = is_mouse_vk(vk) ? ENHANCE_DEV_MOUSE : ENHANCE_DEV_KEYBOARD;
        s_vk_down[vk] = (unsigned char)down;
    }
    if (dev >= 0)
        enhance_mod_status_set_device(dev);
}

/* api->glyph_name's work, for the device in use. */
static int namer(int mod, const char *ref, char *out, size_t cap, uint32_t flags)
{
    return enhance_text_glyph_name(mod, ref, out, cap, flags, XBOX_PAD_FAMILY_AUTO,
                                   enhance_mod_status_device());
}

/* ── The cards ── */

/* The title-safe inset: 5% of each side, as a TV's title-safe area. */
static int safe_inset(int side)
{
    return side * 5 / 100;
}

int enhance_mod_status_compose(mu_Context *ctx, uint32_t out_w, uint32_t out_h)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    float s = nv2a_ovl_ui_scale(out_h);
    mu_Font ft = OVL_FONT(OVL_FONT_BODY, nv2a_ovl_px(s, t->font_body), 0);
    mu_Font fl = OVL_FONT(OVL_FONT_SMALL, nv2a_ovl_px(s, t->font_small), 0);
    int pad = nv2a_ovl_px(s, t->pad), gap = nv2a_ovl_px(s, t->gap);
    int radius = nv2a_ovl_px(s, t->radius), shadow = nv2a_ovl_px(s, t->shadow) / 2;
    int th = nv2a_ovl_text_height(ft), lh = nv2a_ovl_text_height(fl);
    int dot = th / 3 > 2 ? th / 3 : 2, dev = enhance_mod_status_device();
    int x = safe_inset((int)out_w), y = safe_inset((int)out_h), drawn = 0;
    int bottom = (int)out_h - safe_inset((int)out_h);

    for (int i = 0; i < xbox_mod_count(); i++) {
        char text[XBOX_MOD_STATUS_LINES * (XBOX_MOD_STATUS_LINE + 1)];
        char line[XBOX_MOD_STATUS_LINES][XBOX_MOD_STATUS_LINE + 1];
        const char *p;
        int n = 0, w, h, ly;
        xbox_mod_status_text(i, text, sizeof text);
        if (!text[0])
            continue;
        for (p = text; n < XBOX_MOD_STATUS_LINES;) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (len > XBOX_MOD_STATUS_LINE)
                len = XBOX_MOD_STATUS_LINE;
            memcpy(line[n], p, len);
            line[n++][len] = 0;
            if (!nl)
                break;
            p = nl + 1;
        }
        /* The card's size: the title with its dot, then the hints. */
        w = dot + gap + enhance_text_width_owned(ft, line[0], XBOX_PAD_FAMILY_AUTO, dev, i);
        for (int k = 1; k < n; k++) {
            int lw = enhance_text_width_owned(fl, line[k], XBOX_PAD_FAMILY_AUTO, dev, i);
            if (lw > w)
                w = lw;
        }
        w += 2 * pad;
        h = 2 * pad * 3 / 4 + th + (n - 1) * (lh + gap / 2);
        /* A card that would leave the safe area is not drawn, nor any
         * after it: the stack is cut, never squeezed. */
        if (y + h > bottom)
            break;
        if (ctx) {
            mu_Rect r = mu_rect(x, y, w, h);
            nv2a_ovl_ui_shadow(ctx, r, radius, shadow, t->shadow_alpha);
            nv2a_ovl_ui_rrect(ctx, r, radius, 1.0f, t->bg);
            ly = y + pad * 3 / 4;
            nv2a_ovl_ui_rrect(ctx, mu_rect(x + pad, ly + (th - dot) / 2, dot, dot), dot / 2,
                              1.0f, t->accent);
            enhance_text_draw_owned(ctx, ft, line[0], x + pad + dot + gap, ly, t->text,
                                    XBOX_PAD_FAMILY_AUTO, dev, i);
            ly += th + gap / 2;
            for (int k = 1; k < n; k++, ly += lh + gap / 2)
                enhance_text_draw_owned(ctx, fl, line[k], x + pad, ly, t->text_dim,
                                        XBOX_PAD_FAMILY_AUTO, dev, i);
        }
        y += h + gap;
        drawn++;
    }
    return drawn;
}

static int any_panel(void)
{
    char c[2];
    for (int i = 0; i < xbox_mod_count(); i++) {
        xbox_mod_status_text(i, c, sizeof c);
        if (c[0])
            return 1;
    }
    return 0;
}

static void hide(void)
{
    if (s_shown)
        nv2a_overlay_hide(NV2A_OVL_STATUS);
    s_shown = 0;
}

/* The tick stopped: hide the layer from here (another thread than the
 * tick's). The bump comes first, so a tick in flight that publishes
 * after this hide sees it and hides again. */
static void stop(void)
{
    __atomic_add_fetch(&s_stop_gen, 1, __ATOMIC_SEQ_CST);
    nv2a_overlay_hide(NV2A_OVL_STATUS);
}

void enhance_mod_status_tick(int stopping)
{
    uint32_t serial, gen;
    uint64_t now;
    int px, py, w, h, dev, family;
    mu_Context *ctx;

    if (stopping) {
        stop();
        return;
    }
    gen = __atomic_load_n(&s_stop_gen, __ATOMIC_SEQ_CST);
    if (gen != s_seen_gen) {
        /* Stopped since the last tick: the layer is hidden, draw anew. */
        s_seen_gen = gen;
        s_shown = 0;
        s_serial = ~0u;
        s_shown_device = -1;
    }
    /* Nothing to track or draw until a mod uses version 7. */
    if (!xbox_mod_glyphs_used())
        return;
    track_device();
    if (!nv2a_overlay_armed())
        return;
    if (enhance_menu_is_open()) {
        hide();
        return;
    }
    serial = xbox_mod_status_serial();
    dev = enhance_mod_status_device();
    family = xbox_InputPadFamily(0);
    nv2a_overlay_placement(&px, &py, &w, &h);
    if (w <= 0 || h <= 0)
        w = 1920, h = 1080;
    now = xbox_InputNowMs();
    if (s_shown && serial == s_serial && dev == s_shown_device && family == s_shown_family
        && w == s_shown_w && h == s_shown_h && nv2a_overlay_atlas_gen() == s_shown_atlas
        && now - s_shown_ms < REFRESH_MS)
        return;
    if (serial != s_serial && !any_panel()) {
        s_serial = serial;
        hide();
        return;
    }
    if (!s_shown && serial == s_serial && !any_panel())
        return;
    if (!nv2a_overlay_layer_armed(NV2A_OVL_STATUS)
        && nv2a_overlay_arm(1u << NV2A_OVL_STATUS, 1080) < 0)
        return;
    /* The menu's lock: the key-name cache behind the caps is the menu's
     * thread's alone. A frame that misses it tries on the next. */
    if (!nv2a_overlay_menu_trylock())
        return;
    ctx = nv2a_overlay_ui_begin(NV2A_OVL_STATUS);
    if (ctx) {
        int n = enhance_mod_status_compose(ctx, (uint32_t)w, (uint32_t)h);
        nv2a_overlay_ui_publish(NV2A_OVL_STATUS);
        if (!n || __atomic_load_n(&s_stop_gen, __ATOMIC_SEQ_CST) != gen)
            nv2a_overlay_hide(NV2A_OVL_STATUS);
        s_shown = n > 0;
    }
    nv2a_overlay_menu_unlock();
    s_serial = serial;
    s_shown_device = dev;
    s_shown_family = family;
    s_shown_w = w;
    s_shown_h = h;
    s_shown_atlas = nv2a_overlay_atlas_gen();
    s_shown_ms = now;
}

void enhance_mod_status_start(void)
{
    /* A fresh start (a test's re-init): nothing shown yet. */
    s_serial = ~0u;
    s_shown_device = -1;
    s_shown_family = -2;
    s_shown = 0;
    s_seen_gen = __atomic_load_n(&s_stop_gen, __ATOMIC_SEQ_CST);
    xbox_mod_set_glyph_namer(namer);
    enhance_actions_set_panel_hook(enhance_mod_status_tick);
}
