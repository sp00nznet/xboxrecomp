/*
 * nv2a_overlay_ui.c - the overlay's look (R8 tokens), its microui
 * contexts, the converter from microui commands to a draw list (R7), and
 * the two pages the presenting thread composes: the stats strip and the
 * toast stack.
 *
 * microui emits rects, text, clips and icons; the overlay adds rounded
 * rects, images, the cursor and atlas symbols (icons, input glyphs) as
 * its own commands. The converter turns
 * all of them into triangles once, on the writer's thread, so every
 * presenter draws the same list and the backends agree by construction.
 */
#include "nv2a_overlay_int.h"
#include "nv2a_stats.h"
#include "kernel_pacing.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned nv2a_host_render_scale(void);     /* nv2a_backend_common.h */
uint64_t nv2a_ovl_now_ns(void);            /* nv2a_overlay.c */

/* ── Theme ── */

#define RGBA(rgb, a) { (unsigned char)((rgb) >> 16), (unsigned char)((rgb) >> 8), \
                       (unsigned char)(rgb), (unsigned char)(a) }

const struct nv2a_ovl_theme nv2a_ovl_theme_default = {
    .bg = RGBA(0x101418, 219),         /* 0.86 */
    .bg_row = RGBA(0x1A2027, 230),     /* 0.9 */
    .bg_hover = RGBA(0x2A3440, 255),
    .accent = RGBA(0x4FB3FF, 255),
    .text = RGBA(0xE8EDF2, 255),
    .text_dim = RGBA(0x9AA7B4, 255),
    .warn = RGBA(0xFFB84F, 255),
    .bad = RGBA(0xFF6B6B, 255),
    .good = RGBA(0x7BD88F, 255),
    .radius = 10, .pad = 16, .gap = 8, .row_h = 44,
    .font_title = 30, .font_body = 22, .font_small = 17,
    .shadow_alpha = 0.35f, .shadow = 12,
};

static float s_ui_mul = 1.0f;

float nv2a_overlay_set_ui_scale(float mul)
{
    if (mul != mul)
        mul = 1.0f;
    if (mul < 0.75f)
        mul = 0.75f;
    if (mul > 2.0f)
        mul = 2.0f;
    s_ui_mul = mul;
    return mul;
}

float nv2a_ovl_ui_scale(uint32_t out_h)
{
    float s = (float)out_h / 1080.0f;
    if (s < 0.5f)
        s = 0.5f;
    if (s > 3.0f)
        s = 3.0f;
    /* The outer clamp: 4K at 2.0 would ask for 4.0, past what the atlas
     * holds; 480 lines at 0.75 for text under the smallest size it packs. */
    s *= s_ui_mul;
    if (s < 0.5f)
        s = 0.5f;
    if (s > 3.0f)
        s = 3.0f;
    return s;
}

int nv2a_ovl_px(float s, int token)
{
    int v = (int)lroundf(s * (float)token);
    return token && v < 1 ? 1 : v;
}

/* ── Custom commands ── */

struct ovl_rrect_cmd { mu_BaseCommand base; mu_Rect rect; int radius; float feather; mu_Color color; };
struct ovl_image_cmd { mu_BaseCommand base; mu_Rect rect; int tex; mu_Color tint; };
struct ovl_cursor_cmd { mu_BaseCommand base; int style, x, y, size; mu_Color fill, edge; };
struct ovl_glyph_cmd { mu_BaseCommand base; mu_Rect rect; int face; uint32_t cp; mu_Color color; };

static const mu_Rect s_unclipped = { 0, 0, 0x1000000, 0x1000000 };

/* As mu_draw_text does: a command that crosses the clip rect is drawn
 * under a clip command, one that misses it is not drawn. */
static void *push_clipped(mu_Context *ctx, int type, int size, mu_Rect bounds, int *clipped)
{
    *clipped = mu_check_clip(ctx, bounds);
    if (*clipped == MU_CLIP_ALL)
        return NULL;
    if (*clipped == MU_CLIP_PART)
        mu_set_clip(ctx, mu_get_clip_rect(ctx));
    return mu_push_command(ctx, type, size);
}

static void pop_clipped(mu_Context *ctx, int clipped)
{
    if (clipped)
        mu_set_clip(ctx, s_unclipped);
}

static mu_Rect grow(mu_Rect r, int d)
{
    return mu_rect(r.x - d, r.y - d, r.w + 2 * d, r.h + 2 * d);
}

void nv2a_ovl_ui_rrect(mu_Context *ctx, mu_Rect r, int radius, float feather, mu_Color c)
{
    int clipped;
    struct ovl_rrect_cmd *cmd;
    if (r.w <= 0 || r.h <= 0)
        return;
    cmd = push_clipped(ctx, OVL_CMD_RRECT, (int)sizeof *cmd,
                       grow(r, (int)ceilf(feather * 0.5f)), &clipped);
    if (!cmd)
        return;
    cmd->rect = r;
    cmd->radius = radius;
    cmd->feather = feather;
    cmd->color = c;
    pop_clipped(ctx, clipped);
}

void nv2a_ovl_ui_shadow(mu_Context *ctx, mu_Rect r, int radius, int size, float alpha)
{
    /* The edge's ramp is centred on r, dropped a third of the size, so the
     * shadow shows most below the panel. */
    mu_Color c = { 0, 0, 0, (unsigned char)lroundf(alpha * 255.0f) };
    r.y += size / 3;
    nv2a_ovl_ui_rrect(ctx, r, radius + size / 2, (float)size, c);
}

void nv2a_ovl_ui_image(mu_Context *ctx, mu_Rect r, uint16_t tex, mu_Color tint)
{
    int clipped;
    struct ovl_image_cmd *cmd = push_clipped(ctx, OVL_CMD_IMAGE, (int)sizeof *cmd, r, &clipped);
    if (!cmd)
        return;
    cmd->rect = r;
    cmd->tex = tex;
    cmd->tint = tint;
    pop_clipped(ctx, clipped);
}

void nv2a_ovl_ui_cursor(mu_Context *ctx, int style, int x, int y, int size,
                        mu_Color fill, mu_Color edge)
{
    int clipped;
    struct ovl_cursor_cmd *cmd = push_clipped(ctx, OVL_CMD_CURSOR, (int)sizeof *cmd,
                                              mu_rect(x - size, y - size, 2 * size, 2 * size),
                                              &clipped);
    if (!cmd)
        return;
    cmd->style = style;
    cmd->x = x;
    cmd->y = y;
    cmd->size = size;
    cmd->fill = fill;
    cmd->edge = edge;
    pop_clipped(ctx, clipped);
}

void nv2a_ovl_ui_symbol(mu_Context *ctx, int face, uint32_t cp, mu_Rect r, mu_Color c)
{
    int clipped, box = r.w < r.h ? r.w : r.h;
    struct ovl_glyph_cmd *cmd;
    if (box <= 0)
        return;
    r = mu_rect(r.x + (r.w - box) / 2, r.y + (r.h - box) / 2, box, box);
    cmd = push_clipped(ctx, OVL_CMD_GLYPH, (int)sizeof *cmd, r, &clipped);
    if (!cmd)
        return;
    cmd->rect = r;
    cmd->face = face;
    cmd->cp = cp;
    cmd->color = c;
    pop_clipped(ctx, clipped);
}

/* ── The converter ── */

static uint32_t premul(mu_Color c)
{
    unsigned char b[4];
    uint32_t v;
    b[0] = (unsigned char)((c.r * c.a + 127) / 255);
    b[1] = (unsigned char)((c.g * c.a + 127) / 255);
    b[2] = (unsigned char)((c.b * c.a + 127) / 255);
    b[3] = c.a;
    memcpy(&v, b, 4);
    return v;
}

struct conv {
    struct nv2a_ovl_build *b;
    int16_t clip[4];
    uint16_t tex;
    int open;              /* the last command takes more triangles */
    float wu, wv;          /* the white texel */
};

/* Room for nv vertices and ni indices under the current clip and texture;
 * starts a command when either changed. 0 when the list is full. */
static int reserve(struct conv *c, uint32_t nv, uint32_t ni)
{
    struct nv2a_ovl_build *b = c->b;
    struct nv2a_ovl_cmd *last = b->ncmd ? &b->cmd[b->ncmd - 1] : NULL;
    int same = c->open && last && last->tex == c->tex
               && !memcmp(last->clip, c->clip, sizeof c->clip);
    if (b->nvtx + nv > b->cap_vtx || b->nidx + ni > b->cap_idx
        || (!same && b->ncmd >= b->cap_cmd))
        return 0;
    if (!same) {
        last = &b->cmd[b->ncmd++];
        last->first = b->nidx;
        last->count = 0;
        memcpy(last->clip, c->clip, sizeof c->clip);
        last->tex = c->tex;
        c->open = 1;
    }
    return 1;
}

static uint16_t vtx(struct conv *c, float x, float y, float u, float v, uint32_t rgba)
{
    struct nv2a_ovl_build *b = c->b;
    struct nv2a_ovl_vtx *p = &b->vtx[b->nvtx];
    p->x = x;
    p->y = y;
    p->u = u;
    p->v = v;
    p->rgba = rgba;
    return (uint16_t)b->nvtx++;
}

static void tri(struct conv *c, uint16_t a, uint16_t b0, uint16_t b1)
{
    struct nv2a_ovl_build *b = c->b;
    b->idx[b->nidx++] = a;
    b->idx[b->nidx++] = b0;
    b->idx[b->nidx++] = b1;
    b->cmd[b->ncmd - 1].count += 3;
}

static int quad(struct conv *c, float x0, float y0, float x1, float y1,
                float u0, float v0, float u1, float v1, uint32_t rgba)
{
    uint16_t a;
    if (!reserve(c, 4, 6))
        return 0;
    a = vtx(c, x0, y0, u0, v0, rgba);
    vtx(c, x1, y0, u1, v0, rgba);
    vtx(c, x1, y1, u1, v1, rgba);
    vtx(c, x0, y1, u0, v1, rgba);
    tri(c, a, (uint16_t)(a + 1), (uint16_t)(a + 2));
    tri(c, a, (uint16_t)(a + 2), (uint16_t)(a + 3));
    return 1;
}

static void set_tex(struct conv *c, uint16_t tex)
{
    if (c->tex != tex) {
        c->tex = tex;
        c->open = 0;
    }
}

static int conv_text(struct conv *c, const mu_TextCommand *t)
{
    const char *s = t->str, *end = s + strlen(t->str);
    float x = (float)t->pos.x;
    /* The baseline on a whole pixel keeps every glyph's rows on the
     * atlas's rows at the baked size. */
    float y = floorf((float)t->pos.y + nv2a_ovl_font_ascent(t->font) + 0.5f);
    uint32_t rgba = premul(t->color);
    set_tex(c, 0);
    while (s < end) {
        struct nv2a_ovl_quad q;
        uint32_t cp = nv2a_ovl_utf8_next(&s, end);
        if (nv2a_ovl_font_glyph(t->font, cp, &x, y, &q)
            && !quad(c, q.x0, q.y0, q.x1, q.y1, q.u0, q.v0, q.u1, q.v1, rgba))
            return 0;
    }
    return 1;
}

static int corner_segs(float r)
{
    return r < 4.0f ? 4 : r < 12.0f ? 6 : 8;
}

static int conv_rrect(struct conv *c, const struct ovl_rrect_cmd *r)
{
    static const float qx[4] = { 1, -1, -1, 1 }, qy[4] = { 1, 1, -1, -1 };
    float f = r->feather > 0 ? r->feather : 1.0f, h = f * 0.5f;
    float x0 = (float)r->rect.x, y0 = (float)r->rect.y;
    float x1 = x0 + (float)r->rect.w, y1 = y0 + (float)r->rect.h;
    float R = (float)r->radius, lim = fminf(x1 - x0, y1 - y0) * 0.5f;
    float cx[4], cy[4];
    uint32_t in = premul(r->color), out = 0;
    int n, k, i, p;
    uint16_t centre, first;
    if (R > lim)
        R = lim;
    if (R < h)
        R = h;
    /* Corner centres, bottom right first, counter-clockwise in screen
     * space (y down): br, bl, tl, tr. */
    cx[0] = x1 - R; cy[0] = y1 - R;
    cx[1] = x0 + R; cy[1] = y1 - R;
    cx[2] = x0 + R; cy[2] = y0 + R;
    cx[3] = x1 - R; cy[3] = y0 + R;
    n = corner_segs(R);
    p = 4 * (n + 1);
    set_tex(c, 0);
    if (!reserve(c, (uint32_t)(1 + 2 * p), (uint32_t)(9 * p)))
        return 0;
    centre = vtx(c, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, c->wu, c->wv, in);
    first = (uint16_t)c->b->nvtx;
    for (k = 0; k < 4; k++)
        for (i = 0; i <= n; i++) {
            float a = (float)i / (float)n * 1.57079633f;
            /* Corner k sweeps a quarter turn from its first axis. */
            float dx = (k & 1) ? sinf(a) : cosf(a), dy = (k & 1) ? cosf(a) : sinf(a);
            dx *= qx[k];
            dy *= qy[k];
            vtx(c, cx[k] + dx * (R - h), cy[k] + dy * (R - h), c->wu, c->wv, in);
            vtx(c, cx[k] + dx * (R + h), cy[k] + dy * (R + h), c->wu, c->wv, out);
        }
    for (i = 0; i < p; i++) {
        uint16_t a = (uint16_t)(first + 2 * i), b = (uint16_t)(first + 2 * ((i + 1) % p));
        tri(c, centre, a, b);
        tri(c, a, (uint16_t)(a + 1), (uint16_t)(b + 1));
        tri(c, a, (uint16_t)(b + 1), b);
    }
    return 1;
}

static int conv_cursor(struct conv *c, const struct ovl_cursor_cmd *k)
{
    float s = (float)k->size, x = (float)k->x, y = (float)k->y;
    if (k->style == OVL_CURSOR_DOT) {
        struct ovl_rrect_cmd r = { { OVL_CMD_RRECT, 0 }, mu_rect(k->x - k->size / 4, k->y - k->size / 4,
                                   k->size / 2, k->size / 2), k->size / 4, 1.0f, k->fill };
        return conv_rrect(c, &r);
    }
    /* The arrow: a 7-point outline, filled as a fan from the tip; the edge
     * is the same shape a pixel larger, drawn first. */
    {
        static const float px[7] = { 0, 0, 0.27f, 0.42f, 0.55f, 0.40f, 0.62f };
        static const float py[7] = { 0, 0.92f, 0.70f, 1.00f, 0.95f, 0.66f, 0.66f };
        uint32_t col[2] = { premul(k->edge), premul(k->fill) };
        int pass, i;
        set_tex(c, 0);
        for (pass = 0; pass < 2; pass++) {
            float g = pass ? 0 : fmaxf(1.0f, s / 24.0f);
            uint16_t a;
            if (!reserve(c, 7, 15))
                return 0;
            a = (uint16_t)c->b->nvtx;
            for (i = 0; i < 7; i++)
                vtx(c, x + px[i] * s + (i ? g * 0.5f : -g), y + py[i] * s + (i ? 0 : -g * 1.5f),
                    c->wu, c->wv, col[pass]);
            for (i = 1; i < 6; i++)
                tri(c, a, (uint16_t)(a + i), (uint16_t)(a + i + 1));
        }
    }
    return 1;
}

int nv2a_ovl_convert(mu_Context *ctx, struct nv2a_ovl_build *b)
{
    struct conv c;
    mu_Command *cmd = NULL;
    memset(&c, 0, sizeof c);
    c.b = b;
    c.clip[2] = (int16_t)b->out_w;
    c.clip[3] = (int16_t)b->out_h;
    nv2a_ovl_font_white_uv(&c.wu, &c.wv);
    while (mu_next_command(ctx, &cmd)) {
        int ok = 1;
        switch (cmd->type) {
        case MU_COMMAND_CLIP: {
            /* Clamped to the output, so a scissor never leaves the rect. */
            int x0 = cmd->clip.rect.x, y0 = cmd->clip.rect.y;
            int x1 = x0 + cmd->clip.rect.w, y1 = y0 + cmd->clip.rect.h;
            int16_t nc[4];
            x0 = x0 < 0 ? 0 : x0 > (int)b->out_w ? (int)b->out_w : x0;
            y0 = y0 < 0 ? 0 : y0 > (int)b->out_h ? (int)b->out_h : y0;
            x1 = x1 < x0 ? x0 : x1 > (int)b->out_w ? (int)b->out_w : x1;
            y1 = y1 < y0 ? y0 : y1 > (int)b->out_h ? (int)b->out_h : y1;
            nc[0] = (int16_t)x0;
            nc[1] = (int16_t)y0;
            nc[2] = (int16_t)(x1 - x0);
            nc[3] = (int16_t)(y1 - y0);
            if (memcmp(nc, c.clip, sizeof nc)) {
                memcpy(c.clip, nc, sizeof nc);
                c.open = 0;
            }
            break;
        }
        case MU_COMMAND_RECT: {
            mu_Rect r = cmd->rect.rect;
            set_tex(&c, 0);
            ok = quad(&c, (float)r.x, (float)r.y, (float)(r.x + r.w), (float)(r.y + r.h),
                      c.wu, c.wv, c.wu, c.wv, premul(cmd->rect.color));
            break;
        }
        case MU_COMMAND_TEXT:
            ok = conv_text(&c, &cmd->text);
            break;
        case MU_COMMAND_ICON:
            /* Only the check mark is drawn (a checkbox's tick, as an inset
             * square); the menu draws its own glyphs for the rest. */
            if (cmd->icon.id == MU_ICON_CHECK) {
                mu_Rect r = cmd->icon.rect;
                int d = r.w / 4;
                set_tex(&c, 0);
                ok = quad(&c, (float)(r.x + d), (float)(r.y + d), (float)(r.x + r.w - d),
                          (float)(r.y + r.h - d), c.wu, c.wv, c.wu, c.wv, premul(cmd->icon.color));
            }
            break;
        case OVL_CMD_RRECT:
            ok = conv_rrect(&c, (const struct ovl_rrect_cmd *)cmd);
            break;
        case OVL_CMD_IMAGE: {
            const struct ovl_image_cmd *im = (const struct ovl_image_cmd *)cmd;
            set_tex(&c, (uint16_t)im->tex);
            ok = quad(&c, (float)im->rect.x, (float)im->rect.y, (float)(im->rect.x + im->rect.w),
                      (float)(im->rect.y + im->rect.h), 0, 0, 1, 1, premul(im->tint));
            break;
        }
        case OVL_CMD_CURSOR:
            ok = conv_cursor(&c, (const struct ovl_cursor_cmd *)cmd);
            break;
        case OVL_CMD_GLYPH: {
            /* From the atlas like text, so the list needs no new texture
             * and the backends draw it as they draw glyphs. */
            const struct ovl_glyph_cmd *g = (const struct ovl_glyph_cmd *)cmd;
            struct nv2a_ovl_quad q;
            if (nv2a_ovl_font_symbol(g->face, g->cp, (float)g->rect.x, (float)g->rect.y,
                                     (float)g->rect.h, &q)) {
                set_tex(&c, 0);
                ok = quad(&c, q.x0, q.y0, q.x1, q.y1, q.u0, q.v0, q.u1, q.v1, premul(g->color));
            }
            break;
        }
        default:
            break;
        }
        if (!ok) {
            b->cut = 1;
            return 1;
        }
    }
    return 0;
}

/* ── Contexts and pages ── */

static void draw_frame(mu_Context *ctx, mu_Rect rect, int colorid)
{
    /* microui's own frames (buttons, the textbox) in the theme: rows. */
    (void)colorid;
    nv2a_ovl_ui_rrect(ctx, rect, nv2a_ovl_theme_default.radius / 2, 1.0f,
                      nv2a_ovl_theme_default.bg_row);
}

mu_Context *nv2a_ovl_ui_new(void)
{
    mu_Context *ctx = malloc(sizeof *ctx);
    if (!ctx)
        return NULL;
    mu_init(ctx);
    ctx->text_width = nv2a_ovl_text_width;
    ctx->text_height = nv2a_ovl_text_height;
    ctx->draw_frame = draw_frame;
    ctx->style->font = OVL_FONT(OVL_FONT_BODY, 0, 0);
    return ctx;
}

/* A run of text in one colour at x; returns the x after it. */
static int run(mu_Context *ctx, mu_Font f, const char *s, int len, int x, int y, mu_Color c)
{
    if (len <= 0)
        return x;
    mu_draw_text(ctx, f, s, len, mu_vec2(x, y), c);
    return x + nv2a_ovl_text_width(f, s, len);
}

void nv2a_ovl_compose_stats(mu_Context *ctx, uint32_t out_w, uint32_t out_h,
                            const char *line, int spike)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    float s = nv2a_ovl_ui_scale(out_h);
    mu_Font f = OVL_FONT(OVL_FONT_SMALL, nv2a_ovl_px(s, t->font_small), 1);
    int pad = nv2a_ovl_px(s, t->pad), tw = nv2a_ovl_text_width(f, line, -1);
    int th = nv2a_ovl_text_height(f), hp = pad * 3 / 4, vp = pad / 3;
    mu_Rect r = mu_rect((int)out_w - pad - tw - 2 * hp, pad, tw + 2 * hp, th + 2 * vp);
    const char *mx = strstr(line, "(max "), *mend = mx ? strchr(mx, ')') : NULL;
    int x = r.x + hp, y = r.y + vp;

    nv2a_ovl_ui_rrect(ctx, r, nv2a_ovl_px(s, t->radius) / 2 + 2, 1.0f, t->bg);
    /* The max turns warn on a spike, the rest stays text. */
    if (spike && mx && mend) {
        x = run(ctx, f, line, (int)(mx - line), x, y, t->text);
        x = run(ctx, f, mx, (int)(mend + 1 - mx), x, y, t->warn);
        run(ctx, f, mend + 1, (int)strlen(mend + 1), x, y, t->text);
    } else {
        run(ctx, f, line, (int)strlen(line), x, y, t->text);
    }
}

void nv2a_ovl_compose_toasts(mu_Context *ctx, uint32_t out_w, uint32_t out_h,
                             const char *const *lines, int n)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    float s = nv2a_ovl_ui_scale(out_h);
    mu_Font f = OVL_FONT(OVL_FONT_BODY, nv2a_ovl_px(s, t->font_body), 0);
    int pad = nv2a_ovl_px(s, t->pad), gap = nv2a_ovl_px(s, t->gap);
    int th = nv2a_ovl_text_height(f), h = th + pad, y = (int)out_h - 2 * pad;
    int i;
    /* Newest at the bottom, the stack growing up. */
    for (i = n - 1; i >= 0; i--) {
        int tw = nv2a_ovl_text_width(f, lines[i], -1), w = tw + 2 * pad;
        mu_Rect r = mu_rect(((int)out_w - w) / 2, y - h, w, h);
        nv2a_ovl_ui_shadow(ctx, r, h / 2, nv2a_ovl_px(s, t->shadow) / 2, t->shadow_alpha);
        nv2a_ovl_ui_rrect(ctx, r, h / 2, 1.0f, t->bg);
        mu_draw_text(ctx, f, lines[i], -1, mu_vec2(r.x + pad, r.y + (h - th) / 2), t->text);
        y -= h + gap;
    }
}

void nv2a_ovl_stats_line(char *out, size_t n, const char *presenter, int *spike)
{
    struct nv2a_stats_snap st;
    float fps;
    nv2a_stats_read(&st, nv2a_ovl_now_ns());
    fps = st.frames && st.ms_avg > 0 ? 1000.0f / st.ms_avg : 0.0f;
    *spike = st.frames && st.ms_max > 1.5f * st.ms_avg;
    snprintf(out, n, "%.1f fps \xC2\xB7 %.1f ms (max %.1f) \xC2\xB7 pacing %s \xC2\xB7 %ux \xC2\xB7 %s",
             fps, st.ms_avg, st.ms_max, xbox_PacingMode() == XBOX_PACING_SPIN ? "spin" : "sleep",
             nv2a_host_render_scale(), presenter ? presenter : "?");
}
