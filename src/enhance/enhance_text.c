/*
 * enhance_text.c - menu text with input glyphs in it (enhance_text.h,
 * overlay-revamp D8).
 *
 * One walker both measures and draws: a NULL context only advances the
 * pen, so a width and the drawing never disagree. Tokens resolve through
 * the input layer's names (input_map.h, input_names.h) and the actions'
 * bindings (enhance_actions.h) into the overlay's glyphs
 * (nv2a_overlay_glyph.h).
 */
#include "enhance_text.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "enhance_actions.h"
#include "host_pad.h"
#include "input_map.h"
#include "input_names.h"
#include "nv2a_overlay_glyph.h"
#include "nv2a_overlay_int.h"
#include "pad_map.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "keyboard.h"
#endif

/* The kernel's glyph lookup numbers families and sources as the input
 * layer does; it cannot include it, so the two are checked here. */
_Static_assert(XBOX_PAD_FAMILY_XBOX == 0 && XBOX_PAD_FAMILY_PLAYSTATION == 1
                   && XBOX_PAD_FAMILY_NINTENDO == 2 && XBOX_PAD_FAMILY_STEAM == 3
                   && XBOX_PAD_FAMILY_GENERIC == 4 && XBOX_PAD_FAMILY_GAMECUBE == 5
                   && XBOX_PAD_FAMILY_COUNT == 6,
               "the input font's blocks are the pad families");
_Static_assert(INPUT_SRC_SOUTH == 1 && INPUT_SRC_GUIDE == 6 && INPUT_SRC_RSTICK_RIGHT == 31
                   && INPUT_SRC_COUNT <= NV2A_OVL_SRC_DPAD,
               "the input font's slots are the InputSource values");

#define MEMBERS 8

/* ── Key names ── */

#ifdef _WIN32
/* The key's name in the keyboard layout in use, as Windows writes it on
 * its own dialogs: "Z" is the key the player presses on any layout.
 * The layout is the window thread's (keyboard.h), not this one's. Names
 * are cached per VK until it changes, since a menu page asks for every
 * cap each frame; only the overlay's thread calls this. */
static const char *win32_key_name(int vk, HKL hkl, char *buf, int n)
{
    WCHAR w[64];
    UINT sc;
    LONG lp;
    int ext = 0;
    switch (vk) {
    case XBOX_VK_KP_ENTER: vk = 0x0D; ext = 1; break;
    case XBOX_VK_RCONTROL: case XBOX_VK_RMENU: ext = 1; break;
    case 0x13:                                           /* Pause */
    case 0x21: case 0x22: case 0x23: case 0x24:          /* PgUp PgDn End Home */
    case 0x25: case 0x26: case 0x27: case 0x28:          /* the arrows */
    case 0x2D: case 0x2E:                                /* Ins Del */
    case 0x5B: case 0x5C: case 0x5D:                     /* LWin RWin Apps */
    case 0x6F: case 0x90:                                /* Numpad/ NumLock */
        ext = 1;
        break;
    }
    sc = MapVirtualKeyExW((UINT)vk, MAPVK_VK_TO_VSC, hkl);
    if (!sc)
        return NULL;
    lp = (LONG)((sc & 0xff) << 16) | (ext ? 1L << 24 : 0);
    if (GetKeyNameTextW(lp, w, (int)(sizeof w / sizeof w[0])) <= 0)
        return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, n, NULL, NULL))
        return NULL;
    return buf;
}

static const char *key_namer(int vk, char *buf, int n)
{
    static uintptr_t s_hkl;
    static char s_name[256][32];
    static unsigned char s_have[256];   /* 0 unknown, 1 named, 2 no name */
    uintptr_t hkl = xbox_InputKeyboardLayout();
    if (vk < 0 || vk > 0xFF)
        return NULL;
    if (hkl != s_hkl) {
        memset(s_have, 0, sizeof s_have);
        s_hkl = hkl;
    }
    if (!s_have[vk])
        s_have[vk] = win32_key_name(vk, hkl ? (HKL)hkl : GetKeyboardLayout(0), s_name[vk],
                                    (int)sizeof s_name[vk])
                         ? 1
                         : 2;
    if (s_have[vk] != 1)
        return NULL;
    snprintf(buf, (size_t)n, "%s", s_name[vk]);
    return buf;
}
#else
/* SDL hosts record keys by keycode (keyboard_sdl.c), which already
 * follows the layout: the key labelled Z is VK 'Z' on AZERTY too. So the
 * code's own name is the key's label, with no SDL call from this layer. */
static const char *key_namer(int vk, char *buf, int n)
{
    const char *s = xbox_InputCodeName(vk, 0);
    if (!s)
        return NULL;
    snprintf(buf, (size_t)n, "%s", s);
    return buf;
}
#endif

void enhance_text_init(void)
{
    nv2a_ovl_glyph_set_key_namer(key_namer);
}

/* ── Tokens to glyphs ── */

/* Short names a prompt writes, positional as the sources are. */
static const struct { const char *name; int src; } k_pad_alias[] = {
    { "lb", INPUT_SRC_LEFT_SHOULDER }, { "rb", INPUT_SRC_RIGHT_SHOULDER },
    { "lt", INPUT_SRC_LEFT_TRIGGER }, { "rt", INPUT_SRC_RIGHT_TRIGGER },
    { "l3", INPUT_SRC_LEFT_STICK_CLICK }, { "r3", INPUT_SRC_RIGHT_STICK_CLICK },
    { "ls", NV2A_OVL_SRC_LSTICK }, { "rs", NV2A_OVL_SRC_RSTICK },
    { "lstick", NV2A_OVL_SRC_LSTICK }, { "rstick", NV2A_OVL_SRC_RSTICK },
    { "dpad", NV2A_OVL_SRC_DPAD },
    { "view", INPUT_SRC_BACK }, { "select", INPUT_SRC_BACK },
    { "menu", INPUT_SRC_START }, { "home", INPUT_SRC_GUIDE },
    /* The Deck's back buttons, as paddle1..4 are read (config.md). */
    { "r4", INPUT_SRC_PADDLE1 }, { "l4", INPUT_SRC_PADDLE2 },
    { "r5", INPUT_SRC_PADDLE3 }, { "l5", INPUT_SRC_PADDLE4 },
};

/* A pad name to its source, 0 if none. Lowercase only, as bindings are
 * ("A" is the key); a Duke control's name gives the source the identity
 * gives it, as enhance_actions.c reads a binding. */
static int pad_src(const char *s)
{
    xbox_pad_setup id;
    size_t i;
    int src, ctl;
    for (const char *p = s; *p; p++)
        if (isupper((unsigned char)*p))
            return 0;
    for (i = 0; i < sizeof k_pad_alias / sizeof k_pad_alias[0]; i++)
        if (!strcmp(s, k_pad_alias[i].name))
            return k_pad_alias[i].src;
    src = xbox_InputMap_SourceFromName(s);
    if (src > INPUT_SRC_NONE)
        return src;
    ctl = xbox_InputMap_ControlFromName(s);
    if (ctl < 0 || ctl >= INPUT_PAD_CONTROL_COUNT)
        return 0;
    xbox_PadSetupIdentity(&id);
    return id.map.source[ctl] > INPUT_SRC_NONE ? id.map.source[ctl] : 0;
}

static nv2a_ovl_glyph mouse_glyph(const char *s)
{
    static const char *const names[] = { "left", "right", "middle", "wheel_up", "wheel_down",
                                         "move", "mouse" };
    int i, code, is_mouse;
    for (i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
        if (!strcmp(s, names[i]))
            return nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_LEFT + i);
    /* The binding names (mouse1, wheelup): buttons 4 and 5 have no glyph. */
    if (xbox_InputNameToCode(s, &code, &is_mouse) && is_mouse) {
        switch (code) {
        case XBOX_MOUSE_LEFT:       return nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_LEFT);
        case XBOX_MOUSE_RIGHT:      return nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_RIGHT);
        case XBOX_MOUSE_MIDDLE:     return nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_MIDDLE);
        case XBOX_MOUSE_WHEEL_UP:   return nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_WHEEL_UP);
        case XBOX_MOUSE_WHEEL_DOWN: return nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_WHEEL_DOWN);
        }
    }
    return 0;
}

static nv2a_ovl_glyph key_glyph(const char *s)
{
    int code, is_mouse;
    if (!xbox_InputNameToCode(s, &code, &is_mouse))
        return 0;
    return is_mouse ? mouse_glyph(s) : nv2a_ovl_glyph_key(code);
}

/* One member of a resolved token: a glyph, or its name as text when the
 * family has no glyph for it. */
struct member {
    nv2a_ovl_glyph g;
    char name[ENHANCE_ACTION_TEXT];
};

/* Split a binding's text into members on '+', dropping a padN: prefix
 * and an @ms hold. Returns the count, 0 for an empty member. */
static int split(const char *text, char out[][ENHANCE_ACTION_TEXT], int max)
{
    char buf[ENHANCE_ACTION_TEXT], *s = buf, *at;
    int n = 0;
    snprintf(buf, sizeof buf, "%s", text);
    while (isspace((unsigned char)*s))
        s++;
    if (!strncmp(s, "pad", 3) && s[3] >= '1' && s[3] <= '4' && s[4] == ':')
        s += 5;
    at = strchr(s, '@');
    if (at)
        *at = 0;
    for (;;) {
        char *plus = strchr(s, '+'), *e;
        if (plus)
            *plus = 0;
        while (isspace((unsigned char)*s))
            s++;
        e = s + strlen(s);
        while (e > s && isspace((unsigned char)e[-1]))
            *--e = 0;
        if (!*s || n == max)
            return 0;
        snprintf(out[n++], ENHANCE_ACTION_TEXT, "%s", s);
        if (!plus)
            return n;
        s = plus + 1;
    }
}

/* A binding's members as glyphs. Returns the count and whether it is a
 * pad binding in *pad, 0 when a member names nothing. keys reads every
 * member as a key, for {key:a}: "a" is a pad button in a binding. */
static int resolve_binding(const char *text, int family, int keys, struct member *m, int *pad)
{
    char part[MEMBERS][ENHANCE_ACTION_TEXT];
    int n = split(text, part, MEMBERS), i, all_pad = n > 0 && !keys;
    for (i = 0; i < n && all_pad; i++)
        if (!pad_src(part[i]))
            all_pad = 0;
    for (i = 0; i < n; i++) {
        snprintf(m[i].name, sizeof m[i].name, "%s", part[i]);
        if (all_pad) {
            m[i].g = nv2a_ovl_glyph_src(family, pad_src(part[i]));
        } else {
            int code, is_mouse;
            if (!xbox_InputNameToCode(part[i], &code, &is_mouse))
                return 0;
            m[i].g = key_glyph(part[i]);
        }
    }
    *pad = all_pad;
    return n;
}

/* {act:name}: the first binding for the device, else the first of any. */
static int resolve_action(const char *name, int family, int device, struct member *m)
{
    char b[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];
    int id = enhance_action_find(name), nb, i, pass;
    if (id < 0)
        return 0;
    nb = enhance_action_bindings(id, b, ENHANCE_ACTION_BINDS);
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < nb; i++) {
            int pad, n = resolve_binding(b[i], family, 0, m, &pad);
            if (n && (pass || pad == (device == ENHANCE_DEV_PAD)))
                return n;
        }
    return 0;
}

/* A token's text between the braces to its members; 0 when it names
 * nothing, and the token then draws as text. */
static int resolve(const char *tok, int len, int family, int device, struct member *m)
{
    char kind[8], name[ENHANCE_ACTION_TEXT];
    const char *colon = memchr(tok, ':', (size_t)len);
    int kl, nl, pad;
    if (!colon)
        return 0;
    kl = (int)(colon - tok);
    nl = len - kl - 1;
    if (kl >= (int)sizeof kind || nl <= 0 || nl >= (int)sizeof name)
        return 0;
    memcpy(kind, tok, (size_t)kl);
    kind[kl] = 0;
    memcpy(name, colon + 1, (size_t)nl);
    name[nl] = 0;
    if (!strcmp(kind, "pad")) {
        int src = pad_src(name);
        if (!src)
            return 0;
        m[0].g = nv2a_ovl_glyph_src(family, src);
        snprintf(m[0].name, sizeof m[0].name, "%s", name);
        return 1;
    }
    if (!strcmp(kind, "mouse")) {
        m[0].g = mouse_glyph(name);
        snprintf(m[0].name, sizeof m[0].name, "%s", name);
        return m[0].g ? 1 : 0;
    }
    if (!strcmp(kind, "key")) {
        return resolve_binding(name, family, 1, m, &pad);
    }
    if (!strcmp(kind, "act"))
        return resolve_action(name, family, device, m);
    return 0;
}

/* ── Layout ── */

static int run(mu_Context *ctx, mu_Font font, const char *s, int len, int x, int y,
               mu_Color color)
{
    if (ctx && len > 0)
        mu_draw_text(ctx, font, s, len, mu_vec2(x, y), color);
    return len > 0 ? nv2a_ovl_text_width(font, s, len) : 0;
}

static int walk(mu_Context *ctx, mu_Font font, const char *text, int x, int y, mu_Color color,
                int family, int device)
{
    int h = nv2a_ovl_text_height(font), gap = (h + 6) / 8, x0 = x;
    const char *s = text, *lit = text;
    if (!text)
        return 0;
    enhance_text_init();
    if (family < 0 || family >= XBOX_PAD_FAMILY_COUNT)
        family = xbox_InputPadFamily(0);
    while (*s) {
        const char *close;
        struct member m[MEMBERS];
        int n, i;
        if (*s != '{') {
            s++;
            continue;
        }
        /* No '}' after this one means none after any later '{' either. */
        if (!(close = strchr(s + 1, '}'))) {
            s += strlen(s);
            break;
        }
        if (!(n = resolve(s + 1, (int)(close - s - 1), family, device, m))) {
            s++;
            continue;
        }
        x += run(ctx, font, lit, (int)(s - lit), x, y, color);
        /* A glyph keeps a small gap from the text around it. Right after
         * another token (s == lit) that token's trailing gap is the one. */
        if (s > lit)
            x += gap;
        for (i = 0; i < n; i++) {
            if (i)
                x += run(ctx, font, "+", 1, x + gap, y, color) + 2 * gap;
            if (m[i].g) {
                int w = nv2a_ovl_glyph_width(m[i].g, font);
                if (ctx)
                    nv2a_ovl_ui_glyph(ctx, m[i].g, mu_rect(x, y, w, h), color);
                x += w;
            } else {
                x += run(ctx, font, m[i].name, (int)strlen(m[i].name), x, y, color);
            }
        }
        s = lit = close + 1;
        if (*s)
            x += gap;
    }
    x += run(ctx, font, lit, (int)(s - lit), x, y, color);
    return x - x0;
}

int enhance_text_draw(mu_Context *ctx, mu_Font font, const char *text, int x, int y,
                      mu_Color color, int family, int device)
{
    return walk(ctx, font, text, x, y, color, family, device);
}

int enhance_text_width(mu_Font font, const char *text, int family, int device)
{
    return walk(NULL, font, text, 0, 0, mu_color(0, 0, 0, 0), family, device);
}
