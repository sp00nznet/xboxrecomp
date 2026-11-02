/*
 * enhance_text.c - menu text with input glyphs in it (enhance_text.h,
 * overlay-revamp D8).
 *
 * One walker both measures and draws: a NULL context only advances the
 * pen, so a width and the drawing never disagree. Tokens resolve through
 * the input layer's names (input_map.h, input_names.h) and the actions'
 * bindings (enhance_actions.h) into the overlay's glyphs
 * (nv2a_overlay_glyph.h). A {ctl:} token reads the owning mod's
 * controls (mod.h); glyph_name (mod API version 7) writes the same
 * resolution back out as tokens or names.
 */
#include "enhance_text.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "enhance_actions.h"
#include "host_pad.h"
#include "input_map.h"
#include "input_names.h"
#include "keyboard.h"
#include "mod.h"
#include "nv2a_overlay_glyph.h"
#include "nv2a_overlay_int.h"
#include "pad_map.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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
    /* A captured control's whole stick, as mods bind one (mod.h). */
    { "left_stick", NV2A_OVL_SRC_LSTICK }, { "right_stick", NV2A_OVL_SRC_RSTICK },
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
 * family has no glyph for it. kind: 'p' a pad source, 'k' a key (or a
 * mouse button by its binding name), 'm' a mouse token. sep: what joins
 * it to the member before: '+' in a chord, '/' between the keys of one
 * stick, any of which drives it. */
struct member {
    nv2a_ovl_glyph g;
    char kind, sep;
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
        m[i].kind = all_pad ? 'p' : 'k';
        m[i].sep = '+';
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

/* Of a list of bindings, the first for the device, else the first of any. */
static int pick_binding(char b[][ENHANCE_ACTION_TEXT], int nb, int family, int device,
                        struct member *m, int *pad)
{
    int i, pass;
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < nb; i++) {
            int n = resolve_binding(b[i], family, 0, m, pad);
            if (n && (pass || *pad == (device == ENHANCE_DEV_PAD)))
                return n;
        }
    return 0;
}

/* {act:name}: the action's bindings. */
static int resolve_action(const char *name, int family, int device, struct member *m, int *pad)
{
    char b[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];
    int id = enhance_action_find(name);
    if (id < 0)
        return 0;
    return pick_binding(b, enhance_action_bindings(id, b, ENHANCE_ACTION_BINDS), family, device,
                        m, pad);
}

/* The first key the keyboard map binds to guest control ctl, as member
 * *m; 0 when it has none. */
static int map_key(int ctl, char sep, struct member *m)
{
    int code[XBOX_BIND_MAX], mouse[XBOX_BIND_MAX];
    const char *name;
    if (xbox_InputBindingsOf(ctl, code, mouse, XBOX_BIND_MAX) <= 0)
        return 0;
    name = xbox_InputCodeName(code[0], mouse[0]);
    if (!name)
        return 0;
    snprintf(m->name, sizeof m->name, "%s", name);
    m->g = key_glyph(name);
    m->kind = 'k';
    m->sep = sep;
    return 1;
}

/* A captured control's pad binding as the keys that drive it. Such a
 * control reads the guest's pad with the keyboard merged in (mod.h), so
 * the keys the keyboard map (keyboard.h) gives a guest control drive the
 * control bound to it. A stick is its direction keys, up, left, down,
 * right (W/A/S/D on the default map); the right stick takes the mouse
 * too, as the guest's mouse look drives that stick and capture hands the
 * motion to the mod in its place. 0 when the map has no key for it. */
static int map_keys(const char *text, struct member *m)
{
    struct enhance_ctl_src c;
    int n = 0;
    if (!enhance_control_parse(text, &c))
        return 0;
    switch (c.type) {
    case ENHANCE_CTL_BUTTON:
        return map_key(c.idx, '+', m);
    case ENHANCE_CTL_ANALOG:
        return map_key(INPUT_A + c.idx, '+', m);
    case ENHANCE_CTL_STICK: {
        int right = c.idx == (int)XBOX_CTL_SRC_RIGHT;
        int base = right ? INPUT_RSTICK_UP : INPUT_LSTICK_UP;
        static const int dir[4] = { 0, 2, 1, 3 };   /* up, left, down, right */
        for (int i = 0; i < 4; i++)
            n += map_key(base + dir[i], '/', m + n);
        if (right) {
            m[n].g = mouse_glyph("move");
            m[n].kind = 'm';
            m[n].sep = '/';
            snprintf(m[n].name, sizeof m[n].name, "move");
            n++;
        }
        return n;
    }
    }
    return 0;
}

/* {ctl:id}: a control of the mod that owns the string, as it is bound
 * now (an action control's action, a captured control's own list). A
 * captured control bound only on the pad names, off the pad, the keys
 * that drive it (map_keys). An action control's pad chord stays: actions
 * read the host pad itself, which no key presses. */
static int resolve_control(const char *id, int owner, int family, int device, struct member *m,
                           int *pad)
{
    char b[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];
    int k = owner >= 0 ? xbox_mod_control_find(owner, id) : -1, nb, n;
    if (k < 0)
        return 0;
    nb = xbox_mod_control_bindings(owner, (uint32_t)k, b, ENHANCE_ACTION_BINDS, NULL);
    n = pick_binding(b, nb, family, device, m, pad);
    if (n && *pad && device != ENHANCE_DEV_PAD
        && xbox_mod_control_action(owner, (uint32_t)k) < 0) {
        struct member km[MEMBERS];
        for (int i = 0; i < nb; i++) {
            int kn = map_keys(b[i], km);
            if (kn) {
                memcpy(m, km, sizeof km[0] * (size_t)kn);
                *pad = 0;
                return kn;
            }
        }
    }
    return n;
}

/* A token's text between the braces to its members; 0 when it names
 * nothing, and the token then draws as text. */
static int resolve(const char *tok, int len, int family, int device, int owner,
                   struct member *m, int *pad)
{
    char kind[8], name[ENHANCE_ACTION_TEXT];
    const char *colon = memchr(tok, ':', (size_t)len);
    int kl, nl;
    *pad = 0;
    for (int i = 0; i < MEMBERS; i++)
        m[i].sep = '+';
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
        m[0].kind = 'p';
        snprintf(m[0].name, sizeof m[0].name, "%s", name);
        *pad = 1;
        return 1;
    }
    if (!strcmp(kind, "mouse")) {
        m[0].g = mouse_glyph(name);
        m[0].kind = 'm';
        snprintf(m[0].name, sizeof m[0].name, "%s", name);
        return m[0].g ? 1 : 0;
    }
    if (!strcmp(kind, "key")) {
        return resolve_binding(name, family, 1, m, pad);
    }
    if (!strcmp(kind, "act"))
        return resolve_action(name, family, device, m, pad);
    if (!strcmp(kind, "ctl"))
        return resolve_control(name, owner, family, device, m, pad);
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
                int family, int device, int owner)
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
        int n, i, pad;
        if (*s != '{') {
            s++;
            continue;
        }
        /* No '}' after this one means none after any later '{' either. */
        if (!(close = strchr(s + 1, '}'))) {
            s += strlen(s);
            break;
        }
        if (!(n = resolve(s + 1, (int)(close - s - 1), family, device, owner, m, &pad))) {
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
                x += run(ctx, font, &m[i].sep, 1, x + gap, y, color) + 2 * gap;
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
    return walk(ctx, font, text, x, y, color, family, device, -1);
}

int enhance_text_width(mu_Font font, const char *text, int family, int device)
{
    return walk(NULL, font, text, 0, 0, mu_color(0, 0, 0, 0), family, device, -1);
}

int enhance_text_draw_owned(mu_Context *ctx, mu_Font font, const char *text, int x, int y,
                            mu_Color color, int family, int device, int mod)
{
    return walk(ctx, font, text, x, y, color, family, device, mod);
}

int enhance_text_width_owned(mu_Font font, const char *text, int family, int device, int mod)
{
    return walk(NULL, font, text, 0, 0, mu_color(0, 0, 0, 0), family, device, mod);
}

/* ── Names for glyph_name ── */

/* What a pad's controls are called on its family, for a log line. The
 * faces are positional (south is "B" on a Nintendo pad), as the glyphs. */
static const char *pad_label(int family, int src)
{
    static const char *const face[XBOX_PAD_FAMILY_COUNT][4] = {
        [XBOX_PAD_FAMILY_XBOX] = { "A", "B", "X", "Y" },
        [XBOX_PAD_FAMILY_PLAYSTATION] = { "Cross", "Circle", "Square", "Triangle" },
        [XBOX_PAD_FAMILY_NINTENDO] = { "B", "A", "Y", "X" },
        [XBOX_PAD_FAMILY_STEAM] = { "A", "B", "X", "Y" },
        [XBOX_PAD_FAMILY_GENERIC] = { "South", "East", "West", "North" },
        [XBOX_PAD_FAMILY_GAMECUBE] = { "A", "X", "B", "Y" },
    };
    int ps = family == XBOX_PAD_FAMILY_PLAYSTATION, nin = family == XBOX_PAD_FAMILY_NINTENDO;
    if (src >= INPUT_SRC_SOUTH && src <= INPUT_SRC_NORTH)
        return face[family][src - INPUT_SRC_SOUTH];
    switch (src) {
    case INPUT_SRC_BACK:              return ps ? "Create" : nin ? "-" : "View";
    case INPUT_SRC_GUIDE:             return ps ? "PS" : nin ? "Home" : "Guide";
    case INPUT_SRC_START:             return ps ? "Options" : nin ? "+" : "Menu";
    case INPUT_SRC_LEFT_STICK_CLICK:  return ps ? "L3" : "LS click";
    case INPUT_SRC_RIGHT_STICK_CLICK: return ps ? "R3" : "RS click";
    case INPUT_SRC_LEFT_SHOULDER:     return ps ? "L1" : nin ? "L" : "LB";
    case INPUT_SRC_RIGHT_SHOULDER:    return ps ? "R1" : nin ? "R" : "RB";
    case INPUT_SRC_LEFT_TRIGGER:      return ps ? "L2" : nin ? "ZL" : "LT";
    case INPUT_SRC_RIGHT_TRIGGER:     return ps ? "R2" : nin ? "ZR" : "RT";
    case INPUT_SRC_DPAD_UP:           return "D-pad up";
    case INPUT_SRC_DPAD_DOWN:         return "D-pad down";
    case INPUT_SRC_DPAD_LEFT:         return "D-pad left";
    case INPUT_SRC_DPAD_RIGHT:        return "D-pad right";
    case INPUT_SRC_PADDLE1:           return "R4";
    case INPUT_SRC_PADDLE2:           return "L4";
    case INPUT_SRC_PADDLE3:           return "R5";
    case INPUT_SRC_PADDLE4:           return "L5";
    case INPUT_SRC_TOUCHPAD:          return "Touchpad";
    case NV2A_OVL_SRC_DPAD:           return "D-pad";
    case NV2A_OVL_SRC_LSTICK:         return "Left stick";
    case NV2A_OVL_SRC_RSTICK:         return "Right stick";
    }
    if (src >= INPUT_SRC_LSTICK_UP && src <= INPUT_SRC_LSTICK_RIGHT)
        return (const char *const[]){ "Left stick up", "Left stick down", "Left stick left",
                                      "Left stick right" }[src - INPUT_SRC_LSTICK_UP];
    if (src >= INPUT_SRC_RSTICK_UP && src <= INPUT_SRC_RSTICK_RIGHT)
        return (const char *const[]){ "Right stick up", "Right stick down", "Right stick left",
                                      "Right stick right" }[src - INPUT_SRC_RSTICK_UP];
    return NULL;
}

/* A key or mouse member's name for a log line: the canonical name with a
 * capital ("Ctrl", "F5", "Mouse1"). Not the layout's own key name: that
 * one is cached for the overlay's thread, and glyph_name runs on any. */
static void key_label(const char *name, char *out, size_t cap)
{
    int code, is_mouse;
    const char *s = xbox_InputNameToCode(name, &code, &is_mouse)
                        ? xbox_InputCodeName(code, is_mouse)
                        : NULL;
    snprintf(out, cap, "%s", s ? s : name);
    out[0] = (char)toupper((unsigned char)out[0]);
}

int enhance_text_glyph_name(int mod, const char *ref, char *out, size_t cap, uint32_t flags,
                            int family, int device)
{
    struct member m[MEMBERS];
    char tok[ENHANCE_ACTION_TEXT + 8];
    size_t len, at = 0;
    int n, i, pad;
    if (!ref || !out || !cap)
        return -1;
    /* A ref may come with its braces, as it would sit in a string. */
    snprintf(tok, sizeof tok, "%s", ref[0] == '{' ? ref + 1 : ref);
    len = strlen(tok);
    if (len && tok[len - 1] == '}' && ref[0] == '{')
        tok[--len] = 0;
    if (family < 0 || family >= XBOX_PAD_FAMILY_COUNT)
        family = xbox_InputPadFamily(0);
    n = resolve(tok, (int)len, family, device, mod, m, &pad);
    if (!n)
        return -1;
    out[0] = 0;
    for (i = 0; i < n; i++) {
        char label[ENHANCE_ACTION_TEXT], piece[ENHANCE_ACTION_TEXT + 16];
        char sep[2] = { i ? m[i].sep : 0, 0 };
        size_t pl_len;
        if (flags & XBOX_GLYPH_TEXT) {
            const char *pl = m[i].kind == 'p' ? pad_label(family, pad_src(m[i].name)) : NULL;
            if (pl)
                snprintf(label, sizeof label, "%s", pl);
            else if (m[i].kind == 'm')
                snprintf(label, sizeof label, "Mouse %s", m[i].name);
            else if (m[i].kind == 'k')
                key_label(m[i].name, label, sizeof label);
            else
                snprintf(label, sizeof label, "%s", m[i].name);
            snprintf(piece, sizeof piece, "%s%s", sep, label);
        } else {
            /* Tokens: one per member, so a chord of pad and key members
             * (never written, but read) still draws each as its own. */
            const char *kind = m[i].kind == 'p' ? "pad" : m[i].kind == 'm' ? "mouse" : "key";
            snprintf(piece, sizeof piece, "%s{%s:%s}", sep, kind, m[i].name);
        }
        /* Whole members only: a cut one would name another input. */
        pl_len = strlen(piece);
        if (at + pl_len >= cap)
            return XBOX_MOD_GLYPH_CUT;
        memcpy(out + at, piece, pl_len + 1);
        at += pl_len;
    }
    return (int)at;
}
