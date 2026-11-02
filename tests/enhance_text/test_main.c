/*
 * enhance_text: menu text with input glyphs (enhance_text.h, overlay-revamp
 * GL.4).
 *
 *   tokens    {pad:}, {mouse:}, {key:} each draw the glyph the overlay's
 *             lookup gives, positional per family (Nintendo's south is
 *             its "B"); a chord's members joined by "+"
 *   act       an action's binding for the device in use, its padN: and
 *             @ms dropped, else any binding
 *   ctl       a control of the owning mod (a stubbed table), the same pick;
 *             off the pad, a captured control bound on the pad only names
 *             its keys (a stubbed keyboard map), an action control its
 *             chord; no owner, or another mod's, names nothing
 *   text      a token that names nothing draws as its own text; a member
 *             with no glyph draws its name
 *   widths    what enhance_text_width says is what enhance_text_draw draws
 *
 * Glyph choice is checked by comparing draw lists: the token's list
 * against one that draws the expected glyph with nv2a_ovl_ui_glyph.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "enhance_actions.h"
#include "enhance_text.h"
#include "host_pad.h"
#include "input_map.h"
#include "input_names.h"
#include "mod.h"
#include "nv2a_overlay_glyph.h"
#include "nv2a_overlay_int.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* The runtime's pieces nv2a_stats.c reads. */
int xbox_PacingMode(void) { return 1; }
unsigned nv2a_host_render_scale(void) { return 1; }
uint64_t nv2a_tex_pack_hits(void) { return 0; }
uint64_t nv2a_tex_pack_miss_total(void) { return 0; }

/* The first pad's family, for XBOX_PAD_FAMILY_AUTO. */
static int s_family = XBOX_PAD_FAMILY_PLAYSTATION;
int xbox_InputPadFamily(DWORD slot) { (void)slot; return s_family; }

/* A stub action registry: each action and its bindings in order. */
static const struct { const char *name; const char *binds[3]; } k_actions[] = {
    { "menu.open", { "F1", "guide+a@200" } },
    { "kb.only", { "ctrl+F6" } },
    { "pad.only", { "pad2:lb + rb@300" } },
    { "mouse.only", { "mouse1" } },
    { "paddle", { "paddle1" } },
};
#define NACTIONS ((int)(sizeof k_actions / sizeof k_actions[0]))

int enhance_action_find(const char *name)
{
    int i;
    for (i = 0; i < NACTIONS; i++)
        if (!strcmp(name, k_actions[i].name))
            return i;
    return -1;
}

int enhance_action_bindings(int id, char out[][ENHANCE_ACTION_TEXT], int max)
{
    int n = 0;
    if (id < 0 || id >= NACTIONS)
        return 0;
    while (n < max && n < 3 && k_actions[id].binds[n]) {
        snprintf(out[n], ENHANCE_ACTION_TEXT, "%s", k_actions[id].binds[n]);
        n++;
    }
    return n;
}

/* A stub of mod 1's controls: captured ones with keys of their own,
 * captured ones bound on the pad only, and an action control. */
static const struct { const char *id; const char *binds[2]; } k_controls[] = {
    { "move", { "left_stick", "w" } },
    { "rise", { "rt", "space" } },
    { "look", { "right_stick" } },
    { "jump", { "x" } },
    { "boost", { "lb" } },
};
#define NCTL ((int)(sizeof k_controls / sizeof k_controls[0]))
#define CTL_BOOST 4

int xbox_mod_control_action(int i, uint32_t idx)
{
    return i == 1 && idx == CTL_BOOST ? 3 : -1;
}

/* enhance_actions.c's parse, for the sources the stub controls bind. */
int enhance_control_parse(const char *text, struct enhance_ctl_src *out)
{
    memset(out, 0, sizeof *out);
    if (!strcmp(text, "left_stick") || !strcmp(text, "right_stick")) {
        out->type = ENHANCE_CTL_STICK;
        out->idx = text[0] == 'l' ? (int)XBOX_CTL_SRC_LEFT : (int)XBOX_CTL_SRC_RIGHT;
    } else if (!strcmp(text, "x")) {
        out->type = ENHANCE_CTL_ANALOG;
        out->idx = INPUT_X - INPUT_A;
    } else if (!strcmp(text, "lb")) {
        out->type = ENHANCE_CTL_ANALOG;
        out->idx = INPUT_WHITE - INPUT_A;
    }
    return out->type != ENHANCE_CTL_NONE;
}

/* A stub keyboard map: x on Q, white on E, the right stick on I J K L,
 * and no key for the left stick. */
int xbox_InputBindingsOf(int control, int *codes, int *is_mouse, int max)
{
    int code = control == INPUT_X ? 'Q' : control == INPUT_WHITE ? 'E'
             : control == INPUT_RSTICK_UP ? 'I' : control == INPUT_RSTICK_LEFT ? 'J'
             : control == INPUT_RSTICK_DOWN ? 'K' : control == INPUT_RSTICK_RIGHT ? 'L' : 0;
    if (!code || max < 1)
        return 0;
    codes[0] = code;
    is_mouse[0] = 0;
    return 1;
}

int xbox_mod_control_find(int i, const char *id)
{
    for (int k = 0; i == 1 && k < NCTL; k++)
        if (!strcmp(id, k_controls[k].id))
            return k;
    return -1;
}

int xbox_mod_control_bindings(int i, uint32_t idx, char out[][ENHANCE_ACTION_TEXT], int max,
                              const char **src)
{
    int n = 0;
    (void)src;
    if (i != 1 || idx >= (uint32_t)NCTL)
        return 0;
    for (; n < 2 && n < max && k_controls[idx].binds[n]; n++)
        snprintf(out[n], ENHANCE_ACTION_TEXT, "%s", k_controls[idx].binds[n]);
    return n;
}

static mu_Font s_font;
static int s_h, s_gap;
static const mu_Color k_ink = { 230, 230, 230, 255 };

static uint32_t sum_published(void)
{
    const struct nv2a_ovl_list *l;
    nv2a_overlay_ui_publish(NV2A_OVL_MENU);
    l = nv2a_overlay_acquire(NV2A_OVL_MENU);
    return l ? nv2a_overlay_list_sum(l->vtx, l->nvtx, l->idx, l->nidx, l->cmd, l->ncmd) : 0;
}

/* The draw list of text expanded at (10, 10). */
static uint32_t sum_text(const char *text, int family, int device)
{
    mu_Context *ctx = nv2a_overlay_ui_begin(NV2A_OVL_MENU);
    int w = enhance_text_draw(ctx, s_font, text, 10, 10, k_ink, family, device);
    CHECK(w == enhance_text_width(s_font, text, family, device), "%s: drew %d, measured %d",
          text, w, enhance_text_width(s_font, text, family, device));
    return sum_published();
}

/* The draw list of one glyph at (10, 10), as a token at the start draws it. */
static uint32_t sum_glyph(nv2a_ovl_glyph g)
{
    mu_Context *ctx = nv2a_overlay_ui_begin(NV2A_OVL_MENU);
    nv2a_ovl_ui_glyph(ctx, g, mu_rect(10, 10, nv2a_ovl_glyph_width(g, s_font), s_h), k_ink);
    return sum_published();
}

static int width(const char *text, int family, int device)
{
    return enhance_text_width(s_font, text, family, device);
}

static int text_w(const char *s)
{
    return nv2a_ovl_text_width(s_font, s, -1);
}

/* A two-member chord: both glyphs and the "+" between them. */
static int chord_w(nv2a_ovl_glyph a, nv2a_ovl_glyph b)
{
    return nv2a_ovl_glyph_width(a, s_font) + text_w("+") + 2 * s_gap
           + nv2a_ovl_glyph_width(b, s_font);
}

static void test_tokens(void)
{
    const int P = ENHANCE_DEV_PAD, K = ENHANCE_DEV_KEYBOARD;
    nv2a_ovl_glyph b_nin = nv2a_ovl_glyph_src(XBOX_PAD_FAMILY_NINTENDO, INPUT_SRC_SOUTH);
    nv2a_ovl_glyph a_xbox = nv2a_ovl_glyph_src(XBOX_PAD_FAMILY_XBOX, INPUT_SRC_SOUTH);
    int ctrl = 0, f6 = 0, m = 0;

    CHECK(b_nin && a_xbox && b_nin != a_xbox, "south glyphs: 0x%X 0x%X", b_nin, a_xbox);
    /* Positional: south on a Nintendo pad is its "B" glyph. */
    CHECK(sum_text("{pad:south}", XBOX_PAD_FAMILY_NINTENDO, P) == sum_glyph(b_nin),
          "{pad:south} on Nintendo is not its south glyph");
    CHECK(sum_text("{pad:south}", XBOX_PAD_FAMILY_NINTENDO, P) != sum_glyph(a_xbox),
          "{pad:south} on Nintendo draws the Xbox glyph");
    /* A Duke control's name through the identity; the aliases. */
    CHECK(sum_text("{pad:a}", XBOX_PAD_FAMILY_NINTENDO, P) == sum_glyph(b_nin), "{pad:a}");
    CHECK(sum_text("{pad:lb}", XBOX_PAD_FAMILY_XBOX, P)
              == sum_glyph(nv2a_ovl_glyph_src(0, INPUT_SRC_LEFT_SHOULDER)), "{pad:lb}");
    CHECK(sum_text("{pad:dpad}", XBOX_PAD_FAMILY_STEAM, P)
              == sum_glyph(nv2a_ovl_glyph_src(XBOX_PAD_FAMILY_STEAM, NV2A_OVL_SRC_DPAD)),
          "{pad:dpad}");
    /* AUTO is the first pad's family. */
    CHECK(sum_text("{pad:south}", XBOX_PAD_FAMILY_AUTO, P)
              == sum_glyph(nv2a_ovl_glyph_src(s_family, INPUT_SRC_SOUTH)), "AUTO family");
    /* The home glyph on every family. */
    CHECK(sum_text("{pad:home}", XBOX_PAD_FAMILY_PLAYSTATION, P)
              == sum_glyph(nv2a_ovl_glyph_src(XBOX_PAD_FAMILY_XBOX, INPUT_SRC_GUIDE)), "{pad:home}");

    CHECK(sum_text("{mouse:left}", 0, K) == sum_glyph(nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_LEFT)),
          "{mouse:left}");
    CHECK(sum_text("{mouse:wheelup}", 0, K)
              == sum_glyph(nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_WHEEL_UP)), "{mouse:wheelup}");

    /* A one-letter key is a key in {key:}, though "a" is a pad button. */
    CHECK(sum_text("{key:a}", 0, K) == sum_glyph(nv2a_ovl_glyph_key('A')), "{key:a}");
    CHECK(xbox_InputNameToCode("ctrl", &ctrl, &m) && xbox_InputNameToCode("F6", &f6, &m),
          "key names");
    CHECK(width("{key:ctrl+F6}", 0, K)
              == chord_w(nv2a_ovl_glyph_key(ctrl), nv2a_ovl_glyph_key(f6)),
          "{key:ctrl+F6}: %d", width("{key:ctrl+F6}", 0, K));
    /* The cap is labelled by the host's key namer, not the code in hex. */
    CHECK(xbox_InputCodeName(XBOX_VK_BACK, 0)
              && nv2a_ovl_glyph_width(nv2a_ovl_glyph_key(XBOX_VK_BACK), s_font) > s_h,
          "Backspace cap %d for a line of %d",
          nv2a_ovl_glyph_width(nv2a_ovl_glyph_key(XBOX_VK_BACK), s_font), s_h);
    /* A glyph keeps a gap from the text around it. */
    CHECK(width("Jump {pad:south} now", 2, P)
              == text_w("Jump ") + s_gap + s_h + s_gap + text_w(" now"),
          "gaps: %d", width("Jump {pad:south} now", 2, P));
}

static void test_act(void)
{
    const int P = ENHANCE_DEV_PAD, K = ENHANCE_DEV_KEYBOARD, M = ENHANCE_DEV_MOUSE;
    int f1 = 0, m = 0;
    nv2a_ovl_glyph home = nv2a_ovl_glyph_src(XBOX_PAD_FAMILY_NINTENDO, INPUT_SRC_GUIDE);
    nv2a_ovl_glyph b = nv2a_ovl_glyph_src(XBOX_PAD_FAMILY_NINTENDO, INPUT_SRC_SOUTH);
    nv2a_ovl_glyph lb = nv2a_ovl_glyph_src(0, INPUT_SRC_LEFT_SHOULDER);
    nv2a_ovl_glyph rb = nv2a_ovl_glyph_src(0, INPUT_SRC_RIGHT_SHOULDER);
    xbox_InputNameToCode("F1", &f1, &m);

    /* The keyboard and the mouse get the key binding, the pad its chord,
     * the hold dropped. */
    CHECK(sum_text("{act:menu.open}", 2, K) == sum_glyph(nv2a_ovl_glyph_key(f1)), "act on keys");
    CHECK(sum_text("{act:menu.open}", 2, M) == sum_glyph(nv2a_ovl_glyph_key(f1)), "act on mouse");
    CHECK(width("{act:menu.open}", 2, P) == chord_w(home, b), "act on pad: %d",
          width("{act:menu.open}", 2, P));
    CHECK(sum_text("{act:menu.open}", 2, P) != sum_text("{act:menu.open}", 2, K),
          "act: pad and keys alike");
    /* No binding for the device: any binding. padN: and spaces dropped. */
    CHECK(width("{act:pad.only}", 0, K) == chord_w(lb, rb), "pad.only on keys: %d",
          width("{act:pad.only}", 0, K));
    CHECK(sum_text("{act:pad.only}", 0, K) == sum_text("{act:pad.only}", 0, P), "pad.only");
    CHECK(sum_text("{act:kb.only}", 0, P) == sum_text("{key:ctrl+F6}", 0, K), "kb.only on pad");
    CHECK(sum_text("{act:mouse.only}", 0, K)
              == sum_glyph(nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_LEFT)), "a mouse binding");
}

static uint32_t sum_owned(const char *text, int device, int mod)
{
    mu_Context *ctx = nv2a_overlay_ui_begin(NV2A_OVL_MENU);
    int w = enhance_text_draw_owned(ctx, s_font, text, 10, 10, k_ink, 0, device, mod);
    CHECK(w == enhance_text_width_owned(s_font, text, 0, device, mod), "%s: widths", text);
    return sum_published();
}

static void test_ctl(void)
{
    const int P = ENHANCE_DEV_PAD, K = ENHANCE_DEV_KEYBOARD;
    CHECK(sum_owned("{ctl:move}", P, 1) == sum_text("{pad:ls}", 0, P), "ctl move on pad");
    CHECK(sum_owned("{ctl:move}", P, 1) == sum_text("{pad:left_stick}", 0, P), "left_stick");
    CHECK(sum_owned("{ctl:move}", K, 1) == sum_text("{key:w}", 0, K), "ctl move on keys");
    CHECK(sum_owned("{ctl:rise}", P, 1) == sum_text("{pad:rt}", 0, P), "ctl rise");
    /* Bound on the pad only: off the pad, the keys the keyboard map gives
     * its guest control; a stick its direction keys, the right one the
     * mouse too, joined by "/". An action control keeps its pad chord. */
    CHECK(sum_owned("{ctl:jump}", P, 1) == sum_text("{pad:x}", 0, P), "jump on pad");
    CHECK(sum_owned("{ctl:jump}", K, 1) == sum_text("{key:Q}", 0, K), "jump on keys");
    CHECK(sum_owned("{ctl:jump}", ENHANCE_DEV_MOUSE, 1) == sum_text("{key:Q}", 0, K),
          "jump with the mouse");
    CHECK(sum_owned("{ctl:boost}", K, 1) == sum_text("{pad:lb}", 0, K), "action control");
    {
        int kw = 0, sep = text_w("/") + 2 * s_gap;
        const char *keys[] = { "I", "J", "K", "L" };
        for (int i = 0; i < 4; i++) {
            int code, mouse;
            CHECK(xbox_InputNameToCode(keys[i], &code, &mouse), "%s", keys[i]);
            kw += nv2a_ovl_glyph_width(nv2a_ovl_glyph_key(code), s_font) + sep;
        }
        kw += nv2a_ovl_glyph_width(nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_MOVE), s_font);
        CHECK(enhance_text_width_owned(s_font, "{ctl:look}", 0, K, 1) == kw,
              "look on keys: %d, want %d",
              enhance_text_width_owned(s_font, "{ctl:look}", 0, K, 1), kw);
        CHECK(sum_owned("{ctl:look}", K, 1) != sum_owned("{ctl:look}", P, 1), "look differs");
    }
    /* Only the owner's controls: the menu's own text and another mod's
     * name nothing, and draw as text. */
    CHECK(enhance_text_width(s_font, "{ctl:move}", 0, P) == text_w("{ctl:move}"), "no owner");
    CHECK(enhance_text_width_owned(s_font, "{ctl:move}", 0, P, 0) == text_w("{ctl:move}"),
          "another mod");
    CHECK(enhance_text_width_owned(s_font, "{ctl:nope}", 0, P, 1) == text_w("{ctl:nope}"),
          "unknown control");
}

static void test_text(void)
{
    static const char *const literal[] = {
        "{nope:x}", "{pad:nothing}", "{pad:A}", "{act:missing}", "{mouse:mouse4}",
        "{key:nokey}", "{key:ctrl+}", "{pad}", "{", "}{", "{pad:south", "plain text",
    };
    unsigned i;
    for (i = 0; i < sizeof literal / sizeof literal[0]; i++)
        CHECK(width(literal[i], 0, ENHANCE_DEV_PAD) == text_w(literal[i]), "\"%s\": %d, text %d",
              literal[i], width(literal[i], 0, ENHANCE_DEV_PAD), text_w(literal[i]));
    CHECK(width("", 0, 0) == 0 && enhance_text_width(s_font, NULL, 0, 0) == 0, "empty");
    /* The Xbox family has no paddle glyph: the name, as text. The Deck's
     * pack has one. */
    CHECK(width("{pad:paddle1}", XBOX_PAD_FAMILY_XBOX, 0) == text_w("paddle1"), "Xbox paddle");
    CHECK(width("{act:paddle}", XBOX_PAD_FAMILY_XBOX, 0) == text_w("paddle1"), "act paddle");
    CHECK(width("{pad:r4}", XBOX_PAD_FAMILY_STEAM, 0) == s_h, "Deck R4: %d",
          width("{pad:r4}", XBOX_PAD_FAMILY_STEAM, 0));
    /* An unknown token beside a known one. */
    CHECK(width("{x:y}{pad:south}", 0, 0) == text_w("{x:y}") + s_gap + s_h, "mixed: %d",
          width("{x:y}{pad:south}", 0, 0));
    /* Two tokens side by side keep one gap between them, not two. */
    CHECK(width("{pad:south}{pad:east}", 0, 0) == s_h + s_gap + s_h, "adjacent: %d",
          width("{pad:south}{pad:east}", 0, 0));
    CHECK(width("a {pad:south}{pad:east} b", 0, 0)
              == text_w("a ") + s_gap + s_h + s_gap + s_h + s_gap + text_w(" b"),
          "adjacent in text: %d", width("a {pad:south}{pad:east} b", 0, 0));
    /* A '{' with no '}' after it ends the tokens: the rest is text. */
    CHECK(width("{pad:south}{ {{", 0, 0) == s_h + s_gap + text_w("{ {{"), "unclosed: %d",
          width("{pad:south}{ {{", 0, 0));
}

int main(void)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    float s;
    nv2a_overlay_reset_for_test();
    CHECK(nv2a_overlay_arm(0xF, 1080) == 0, "arm");
    s = nv2a_ovl_ui_scale(1080);
    s_font = OVL_FONT(OVL_FONT_BODY, nv2a_ovl_px(s, t->font_body), 0);
    s_h = nv2a_ovl_text_height(s_font);
    s_gap = (s_h + 6) / 8;
    nv2a_overlay_set_placement(0, 0, 1920, 1080);
    enhance_text_init();
    test_tokens();
    test_act();
    test_ctl();
    test_text();
    nv2a_overlay_reset_for_test();
    if (fails) {
        printf("enhance_text: %d failure(s)\n", fails);
        return 1;
    }
    printf("enhance_text: ok\n");
    return 0;
}
