/*
 * nv2a_overlay_font.c - the overlay's text: Atkinson Hyperlegible Regular
 * baked by stb_truetype into one atlas at the three text tokens' sizes
 * (R7, R8), and the metrics microui asks for.
 *
 * The atlas is coverage expanded to premultiplied white, so a glyph quad
 * takes its colour from the vertex. Rows 0..3 hold a white block whose
 * centre texel is the solid fill of every rect, so a whole layer draws
 * from one texture. Row 4 stays empty, so a glyph on the first packed row
 * sampled bilinearly (scaled text) never picks up the block's white.
 *
 * The bake is in display pixels: a size drawn at other than its
 * baked size (a window smaller than the display, render.scale's larger
 * back buffer) is the baked glyphs scaled, which the presenters' linear
 * sampler keeps legible. A change of overlay.ui_scale or ui_fit bakes
 * again (nv2a_overlay_rebake).
 *
 * Two symbol faces share the atlas (overlay-revamp D7, D8): the Lucide
 * icon subset at the three text sizes, and the input-prompt font made by
 * tools/overlay/glyphs_to_ttf.py at small, body and twice body (the
 * rebinding prompt). Both keep their glyphs in the private use area and
 * pack only the code points they have. Only the menu draws them, so they
 * are baked only for an overlay with the menu layer: the stats strip and
 * the toasts alone keep the text-only atlas and its bake time. They pack
 * without oversampling: a symbol is drawn on whole pixels at its baked
 * size, and the atlas at effective scale 3.0 must still fit 2048x2048
 * with the text in it. Near that scale it fits only once large text drops
 * its oversampling too, which the third 2048 try does; at 3.0 the last try
 * also drops the input face's twice-body size, so the rebinding prompt
 * draws the body glyph grown (tests/overlay pins the fit).
 *
 * Each try only places the glyph boxes, which takes microseconds; the
 * glyphs are rendered once, into the first size where every box fits.
 * A try that does not fit so costs no rendering.
 */
#include "nv2a_overlay_int.h"
#include "stb_truetype.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char nv2a_ovl_font_regular[];
extern const unsigned long nv2a_ovl_font_regular_len;
extern const unsigned char nv2a_ovl_font_icons[];
extern const unsigned char nv2a_ovl_font_input[];

/* nv2a_overlay_stb.c: stbrp_rect is an incomplete type outside it. */
stbrp_rect *nv2a_ovl_stb_rects(int n);
stbrp_rect *nv2a_ovl_stb_rect_at(stbrp_rect *base, int i);
int nv2a_ovl_stb_all_packed(const stbrp_rect *r, int n);

/* The baked code points (R7): Basic Latin, Latin-1 and Latin Extended-A,
 * the dashes and quotes of General Punctuation, the arrows. U+FFFF is no
 * character and so maps to .notdef, the box drawn for anything else. It
 * goes first: stb_truetype packs the missing glyph once per call, for the
 * first code point that lacks one, and leaves the later ones empty. */
static const struct { int first, n; } s_ranges[] = {
    { 0xFFFF, 1 }, { 0x20, 95 }, { 0xA0, 224 }, { 0x2010, 23 }, { 0x2190, 6 },
};
#define NRANGES ((int)(sizeof s_ranges / sizeof s_ranges[0]))
#define WHITE_ROWS 4
#define GLYPH_ROW0 (WHITE_ROWS + 1)  /* one empty row of padding */

struct slot {
    int px;                                   /* the baked size */
    stbtt_packedchar *ch[NRANGES];
    float tab_adv;                            /* the widest digit, baked px */
};

/* A symbol face: the code points it has in the private use area, sorted,
 * and its glyphs packed at OVL_SYM_SIZES sizes. */
#define PUA_FIRST 0xE000
#define PUA_LAST 0xF8FF
struct sym_face {
    stbtt_fontinfo info;
    int loaded;                               /* atomic: 0, 1 loading, 2 valid, -1 bad */
    int n;
    int *cps;
    int px[OVL_SYM_SIZES];
    stbtt_packedchar *ch[OVL_SYM_SIZES];
};

static struct {
    stbtt_fontinfo info;
    float ascent;                             /* of a 1 px line */
    struct slot slot[OVL_FONT_SLOTS];
    stbtt_packedchar *store;
    struct sym_face sym[OVL_FACES];
    stbtt_packedchar *sym_store;
    int symbols;                              /* the bake packed the symbol faces */
    uint32_t *px;
    struct nv2a_ovl_atlas atlas;
    uint64_t gen;
} s_font;

static const int *token_px(void)
{
    static int px[OVL_FONT_SLOTS];
    px[OVL_FONT_SMALL] = nv2a_ovl_theme_default.font_small;
    px[OVL_FONT_BODY] = nv2a_ovl_theme_default.font_body;
    px[OVL_FONT_TITLE] = nv2a_ovl_theme_default.font_title;
    return px;
}

static const unsigned char *sym_data(int face)
{
    return face == OVL_FACE_ICONS ? nv2a_ovl_font_icons : nv2a_ovl_font_input;
}

/* The face's metrics, loaded on first use: the glyph lookups ask before
 * any bake (and in the ctests, without one), from the menu's and the
 * stats' writer threads as well as the bake's. The first caller loads
 * them; one that comes while it does waits the microseconds it takes. */
static struct sym_face *sym_face(int face)
{
    struct sym_face *f;
    int st = 0;
    if (face < 0 || face >= OVL_FACES)
        return NULL;
    f = &s_font.sym[face];
    if (__atomic_compare_exchange_n(&f->loaded, &st, 1, 0, __ATOMIC_ACQUIRE,
                                    __ATOMIC_ACQUIRE)) {
        st = stbtt_InitFont(&f->info, sym_data(face),
                            stbtt_GetFontOffsetForIndex(sym_data(face), 0)) ? 2 : -1;
        __atomic_store_n(&f->loaded, st, __ATOMIC_RELEASE);
    }
    while (st == 1)
        st = __atomic_load_n(&f->loaded, __ATOMIC_ACQUIRE);
    return st == 2 ? f : NULL;
}

/* After a bake without the symbol faces (no menu layer), none is drawn:
 * a prompt writes the control's name instead. */
int nv2a_ovl_font_has_symbol(int face, uint32_t cp)
{
    struct sym_face *f = sym_face(face);
    if (s_font.px && !s_font.symbols)
        return 0;
    return f && cp >= PUA_FIRST && cp <= PUA_LAST && stbtt_FindGlyphIndex(&f->info, (int)cp);
}

int nv2a_ovl_font_symbols(void)
{
    return s_font.px && s_font.symbols;
}

void nv2a_ovl_font_free(void)
{
    int i;
    free(s_font.store);
    free(s_font.sym_store);
    free(s_font.px);
    s_font.store = NULL;
    s_font.sym_store = NULL;
    s_font.symbols = 0;
    s_font.px = NULL;
    for (i = 0; i < OVL_FACES; i++) {
        free(s_font.sym[i].cps);
        s_font.sym[i].cps = NULL;
        s_font.sym[i].n = 0;
        memset(s_font.sym[i].px, 0, sizeof s_font.sym[i].px);
        memset(s_font.sym[i].ch, 0, sizeof s_font.sym[i].ch);
    }
    memset(&s_font.atlas, 0, sizeof s_font.atlas);
    memset(s_font.slot, 0, sizeof s_font.slot);
}

/* Text at or above this size packs without horizontal oversampling when
 * the largest atlas is short of room: at that size a glyph spans enough
 * pixels that the half-pixel positions add little, and it halves the
 * text's share (effective scale 3.0 with both symbol faces). */
#define OVERSAMPLE_MAX_PX 40

/* One stbtt_PackFontRanges call split into its steps, so a size can be
 * tried without rendering: gather and place the boxes of one call's
 * ranges (rects), in the order the one call would. */
struct pack_group {
    const stbtt_fontinfo *info;
    stbtt_pack_range *r;
    int nr, over;
    stbrp_rect *rects;
    int n;
};

static int place(stbtt_pack_context *pc, struct pack_group *g)
{
    int i, j;
    for (i = 0; i < g->nr; i++)
        for (j = 0; j < g->r[i].num_chars; j++)
            g->r[i].chardata_for_range[j].x0 = g->r[i].chardata_for_range[j].y0 =
                g->r[i].chardata_for_range[j].x1 = g->r[i].chardata_for_range[j].y1 = 0;
    stbtt_PackSetOversampling(pc, (unsigned)g->over, 1);
    g->n = stbtt_PackFontRangesGatherRects(pc, g->info, g->r, g->nr, g->rects);
    stbtt_PackFontRangesPackRects(pc, g->rects, g->n);
    return nv2a_ovl_stb_all_packed(g->rects, g->n);
}

/* Text: one group per size, so each size gets its own .notdef; then one
 * per symbol face, at 1x. groups holds room for all. Returns the groups.
 * mode 1 packs large text at 1x; mode 2 also leaves out the input face's
 * last size (twice body). */
static int pack_groups(struct pack_group *g, stbrp_rect *rects, stbtt_pack_range *r, int nr,
                       stbtt_pack_range *sr, int mode)
{
    const int big_1x = mode >= 1;
    int n = 0, i, k, at = 0;
    for (i = 0; i < nr; i += NRANGES, n++) {
        g[n].info = &s_font.info;
        g[n].r = r + i;
        g[n].nr = NRANGES;
        g[n].over = big_1x && r[i].font_size >= (float)OVERSAMPLE_MAX_PX ? 1 : 2;
    }
    for (i = 0; i < OVL_FACES; i++)
        if (s_font.symbols && s_font.sym[i].n) {
            g[n].info = &s_font.sym[i].info;
            g[n].r = sr + i * OVL_SYM_SIZES;
            g[n].nr = mode >= 2 && i == OVL_FACE_INPUT ? OVL_SYM_SIZES - 1 : OVL_SYM_SIZES;
            g[n].over = 1;
            n++;
        }
    for (i = 0; i < n; i++) {
        g[i].rects = nv2a_ovl_stb_rect_at(rects, at);
        for (k = 0; k < g[i].nr; k++)
            at += g[i].r[k].num_chars;
    }
    return n;
}

/* Places every box in a w x h atlas; when all fit, renders the glyphs
 * into cov. */
static int try_pack(unsigned char *cov, int w, int h, struct pack_group *g, int ng)
{
    stbtt_pack_context pc;
    int ok, i;
    if (!stbtt_PackBegin(&pc, NULL, w, h - GLYPH_ROW0, w, 1, NULL))
        return 0;
    for (i = 0, ok = 1; i < ng && ok; i++)
        ok = place(&pc, &g[i]);
    if (ok) {
        memset(cov, 0, (size_t)w * h);
        pc.pixels = cov + (size_t)w * GLYPH_ROW0;
        for (i = 0; i < ng && ok; i++) {
            stbtt_PackSetOversampling(&pc, (unsigned)g[i].over, 1);
            ok = stbtt_PackFontRangesRenderIntoRects(&pc, g[i].info, g[i].r, g[i].nr,
                                                     g[i].rects);
        }
    }
    stbtt_PackEnd(&pc);
    return ok;
}

/* The symbol faces' code points and their packing ranges into sr. Returns
 * 0 or -1 when out of memory; a face that does not load is left out and
 * its glyphs fall back to their text names. */
static int sym_ranges(float s, stbtt_pack_range *sr, int *nglyphs)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    const int tok[OVL_FACES][OVL_SYM_SIZES] = {
        [OVL_FACE_ICONS] = { t->font_small, t->font_body, t->font_title },
        [OVL_FACE_INPUT] = { t->font_small, t->font_body, 2 * t->font_body },
    };
    int total = 0, i, k, cp;
    stbtt_packedchar *p;
    memset(sr, 0, sizeof *sr * OVL_FACES * OVL_SYM_SIZES);
    for (i = 0; i < OVL_FACES; i++) {
        struct sym_face *f = sym_face(i);
        if (!f)
            continue;
        f->cps = malloc(sizeof *f->cps * (PUA_LAST - PUA_FIRST + 1));
        if (!f->cps)
            return -1;
        for (cp = PUA_FIRST; cp <= PUA_LAST; cp++)
            if (stbtt_FindGlyphIndex(&f->info, cp))
                f->cps[f->n++] = cp;
        total += f->n * OVL_SYM_SIZES;
    }
    *nglyphs = total;
    s_font.sym_store = calloc((size_t)total + 1, sizeof *s_font.sym_store);
    if (!s_font.sym_store)
        return -1;
    p = s_font.sym_store;
    for (i = 0; i < OVL_FACES; i++) {
        struct sym_face *f = &s_font.sym[i];
        for (k = 0; k < OVL_SYM_SIZES && f->n; k++) {
            stbtt_pack_range *pr = &sr[i * OVL_SYM_SIZES + k];
            f->px[k] = nv2a_ovl_px(s, tok[i][k]);
            pr->font_size = (float)f->px[k];
            pr->array_of_unicode_codepoints = f->cps;
            pr->num_chars = f->n;
            pr->chardata_for_range = p;
            f->ch[k] = p;
            p += f->n;
        }
    }
    return 0;
}

int nv2a_ovl_font_bake(uint32_t out_h, int symbols)
{
    /* w, h, and the pack_groups mode: 1 drops large text's oversampling,
     * 2 also the input face's twice-body size. */
    static const int sizes[][3] = {
        { 512, 1024, 0 }, { 1024, 1024, 0 }, { 2048, 2048, 0 }, { 2048, 2048, 1 },
        { 2048, 2048, 2 },
    };
    const int *tok = token_px();
    /* At the display's height when it is known, the size a
     * fullscreen menu draws at, so the Deck's text is not grown from a
     * 1080-line bake. Both scales include the display floor and
     * overlay.ui_scale and stop at OVL_UI_SCALE_MAX, the bound test_fit
     * proves fits 2048x2048 with the symbols (a Retina floor of 2 at
     * ui_scale 1.5 or more reaches it). Past 2.0 the bake also keeps to
     * the output's own scale: a small window on a 4K or Retina display
     * draws smaller than the display's size, and baking past that only
     * costs bake time and the large text's oversampling. */
    uint32_t lines = nv2a_ovl_bake_lines(out_h);
    float s = nv2a_ovl_ui_scale_shown(lines), s0 = nv2a_ovl_ui_scale_shown(out_h);
    int per = 0, i, k, si, asc, desc, gap;
    stbtt_pack_range r[OVL_FONT_SLOTS * NRANGES];
    stbtt_pack_range sr[OVL_FACES * OVL_SYM_SIZES];
    struct pack_group g[OVL_FONT_SLOTS + OVL_FACES];
    stbrp_rect *rects = NULL;
    unsigned char *cov = NULL;
    int w = 0, h = 0, packed = 0, nsym = 0, ng, one = 0;
    uint64_t t0 = nv2a_ovl_now_ns();

    if (s > s0 && s > 2.0f)
        s = s0 > 2.0f ? s0 : 2.0f;
    nv2a_ovl_font_free();
    if (!stbtt_InitFont(&s_font.info, nv2a_ovl_font_regular,
                        stbtt_GetFontOffsetForIndex(nv2a_ovl_font_regular, 0))) {
        fprintf(stderr, OVL_LOGP "font: the embedded face does not load\n");
        return -1;
    }
    stbtt_GetFontVMetrics(&s_font.info, &asc, &desc, &gap);
    s_font.ascent = (float)asc / (float)(asc - desc);

    for (i = 0; i < NRANGES; i++)
        per += s_ranges[i].n;
    s_font.store = calloc((size_t)per * OVL_FONT_SLOTS, sizeof *s_font.store);
    if (!s_font.store)
        goto oom;
    {
        stbtt_packedchar *p = s_font.store;
        for (si = 0; si < OVL_FONT_SLOTS; si++) {
            s_font.slot[si].px = nv2a_ovl_px(s, tok[si]);
            for (i = 0; i < NRANGES; i++) {
                stbtt_pack_range *pr = &r[si * NRANGES + i];
                memset(pr, 0, sizeof *pr);
                pr->font_size = (float)s_font.slot[si].px;
                pr->first_unicode_codepoint_in_range = s_ranges[i].first;
                pr->num_chars = s_ranges[i].n;
                pr->chardata_for_range = p;
                s_font.slot[si].ch[i] = p;
                p += s_ranges[i].n;
            }
        }
    }
    s_font.symbols = !!symbols;
    memset(sr, 0, sizeof sr);
    if (symbols && sym_ranges(s, sr, &nsym))
        goto oom;
    rects = nv2a_ovl_stb_rects(per * OVL_FONT_SLOTS + nsym);
    if (!rects)
        goto oom;
    for (k = 0; k < (int)(sizeof sizes / sizeof sizes[0]) && !packed; k++) {
        if (!cov || w != sizes[k][0] || h != sizes[k][1]) {
            free(cov);
            w = sizes[k][0];
            h = sizes[k][1];
            cov = malloc((size_t)w * h);
        }
        if (!cov)
            goto oom;
        ng = pack_groups(g, rects, r, OVL_FONT_SLOTS * NRANGES, sr, sizes[k][2]);
        packed = try_pack(cov, w, h, g, ng);
        one = sizes[k][2];
    }
    free(rects);
    rects = NULL;
    if (!packed) {
        fprintf(stderr, OVL_LOGP "font: the glyphs do not fit a %dx%d atlas\n", w, h);
        free(cov);
        nv2a_ovl_font_free();
        return -1;
    }
    /* Unpacked, so sym_lookup must not pick it. */
    if (one >= 2)
        s_font.sym[OVL_FACE_INPUT].px[OVL_SYM_SIZES - 1] = 0;
    s_font.px = malloc((size_t)w * h * 4);
    if (!s_font.px)
        goto oom;
    for (i = 0; i < w * h; i++)
        s_font.px[i] = 0x01010101u * cov[i];
    free(cov);
    cov = NULL;
    for (i = 0; i < WHITE_ROWS; i++)
        for (k = 0; k < WHITE_ROWS; k++)
            s_font.px[i * w + k] = 0xffffffffu;

    /* Tabular digits: every digit advanced by the widest, so the strip's
     * numbers do not shuffle as they change. */
    for (si = 0; si < OVL_FONT_SLOTS; si++) {
        float m = 0;
        for (i = '0'; i <= '9'; i++)
            if (s_font.slot[si].ch[1][i - 0x20].xadvance > m)
                m = s_font.slot[si].ch[1][i - 0x20].xadvance;
        s_font.slot[si].tab_adv = m;
    }
    s_font.atlas.px = s_font.px;
    s_font.atlas.w = (uint32_t)w;
    s_font.atlas.h = (uint32_t)h;
    s_font.atlas.gen = ++s_font.gen;
    if (symbols)
        fprintf(stderr, OVL_LOGP "atlas %dx%d %.1f ms (%d glyphs at %d/%d/%d px%s, %d icons, "
                "%d input glyphs, for %u lines)\n",
                w, h, (double)(nv2a_ovl_now_ns() - t0) / 1e6, per,
                s_font.slot[OVL_FONT_SMALL].px, s_font.slot[OVL_FONT_BODY].px,
                s_font.slot[OVL_FONT_TITLE].px,
                one >= 2 ? ", large at 1x, no 2x prompt" : one ? ", large at 1x" : "",
                s_font.sym[OVL_FACE_ICONS].n, s_font.sym[OVL_FACE_INPUT].n, lines);
    else
        fprintf(stderr, OVL_LOGP "atlas %dx%d %.1f ms (%d glyphs at %d/%d/%d px%s for %u lines)\n",
                w, h, (double)(nv2a_ovl_now_ns() - t0) / 1e6, per,
                s_font.slot[OVL_FONT_SMALL].px, s_font.slot[OVL_FONT_BODY].px,
                s_font.slot[OVL_FONT_TITLE].px, one ? ", large at 1x" : "", lines);
    return 0;
oom:
    free(rects);
    free(cov);
    nv2a_ovl_font_free();
    fprintf(stderr, OVL_LOGP "font: out of memory\n");
    return -1;
}

const struct nv2a_ovl_atlas *nv2a_ovl_font_atlas(void)
{
    return s_font.px ? &s_font.atlas : NULL;
}

void nv2a_ovl_font_white_uv(float *u, float *v)
{
    *u = s_font.atlas.w ? 2.0f / (float)s_font.atlas.w : 0;
    *v = s_font.atlas.h ? 2.0f / (float)s_font.atlas.h : 0;
}

uint32_t nv2a_ovl_utf8_next(const char **sp, const char *end)
{
    const unsigned char *s = (const unsigned char *)*sp;
    uint32_t c;
    int n, i;
    if (s >= (const unsigned char *)end)
        return 0;
    c = *s;
    if (c < 0x80) {
        *sp += 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0) { n = 1; c &= 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; c &= 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; c &= 0x07; }
    else { *sp += 1; return 0xFFFD; }
    for (i = 1; i <= n; i++) {
        if (s + i >= (const unsigned char *)end || (s[i] & 0xC0) != 0x80) {
            *sp += i;
            return 0xFFFD;
        }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *sp += n + 1;
    return c;
}

static const stbtt_packedchar *lookup(int slot, uint32_t cp)
{
    int i;
    /* Basic Latin is whole in the face; a baked code point beyond it may
     * still be missing from it (Atkinson Hyperlegible has no arrows), and
     * its packed entry is then empty. */
    if (cp >= 0x20 && cp < 0x7F)
        return &s_font.slot[slot].ch[1][cp - 0x20];
    if (stbtt_FindGlyphIndex(&s_font.info, (int)cp))
        for (i = 1; i < NRANGES; i++)
            if (cp >= (uint32_t)s_ranges[i].first
                && cp < (uint32_t)(s_ranges[i].first + s_ranges[i].n))
                return &s_font.slot[slot].ch[i][cp - (uint32_t)s_ranges[i].first];
    return &s_font.slot[slot].ch[0][0];   /* .notdef */
}

static int slot_of(mu_Font f)
{
    int s = OVL_FONT_SLOT(f);
    return s < OVL_FONT_SLOTS ? s : OVL_FONT_BODY;
}

static int px_of(mu_Font f)
{
    int px = OVL_FONT_PX(f);
    return px ? px : s_font.slot[slot_of(f)].px;
}

static int is_digit(uint32_t cp) { return cp >= '0' && cp <= '9'; }

int nv2a_ovl_font_glyph(mu_Font font, uint32_t cp, float *x, float y,
                        struct nv2a_ovl_quad *q)
{
    int slot = slot_of(font);
    const struct slot *sl = &s_font.slot[slot];
    const stbtt_packedchar *b;
    float k, adv, off = 0;
    if (!s_font.px || !sl->px)
        return 0;
    b = lookup(slot, cp);
    k = (float)px_of(font) / (float)sl->px;
    adv = b->xadvance;
    if (OVL_FONT_TAB(font) && is_digit(cp)) {
        off = (sl->tab_adv - adv) * 0.5f;
        adv = sl->tab_adv;
    }
    q->x0 = *x + (b->xoff + off) * k;
    q->x1 = *x + (b->xoff2 + off) * k;
    q->y0 = y + b->yoff * k;
    q->y1 = y + b->yoff2 * k;
    q->u0 = (float)b->x0 / (float)s_font.atlas.w;
    q->u1 = (float)b->x1 / (float)s_font.atlas.w;
    q->v0 = (float)(b->y0 + GLYPH_ROW0) / (float)s_font.atlas.h;
    q->v1 = (float)(b->y1 + GLYPH_ROW0) / (float)s_font.atlas.h;
    *x += adv * k;
    return b->x1 > b->x0 && b->y1 > b->y0;
}

float nv2a_ovl_font_ascent(mu_Font font)
{
    return s_font.ascent * (float)px_of(font);
}

int nv2a_ovl_text_width(mu_Font font, const char *str, int len)
{
    const char *end;
    float x = 0;
    int slot = slot_of(font);
    const struct slot *sl = &s_font.slot[slot];
    if (!s_font.px || !sl->px)
        return 0;
    if (len < 0)
        len = (int)strlen(str);
    end = str + len;
    while (str < end) {
        uint32_t cp = nv2a_ovl_utf8_next(&str, end);
        x += OVL_FONT_TAB(font) && is_digit(cp) ? sl->tab_adv : lookup(slot, cp)->xadvance;
    }
    return (int)ceilf(x * (float)px_of(font) / (float)sl->px);
}

int nv2a_ovl_text_height(mu_Font font)
{
    return px_of(font);
}

/* The packed glyph of cp in face, at the baked size that suits a box of
 * box pixels: the smallest at least as large (a symbol shrunk stays
 * crisper than one grown), else the largest. */
static const stbtt_packedchar *sym_lookup(int face, uint32_t cp, float box, int *px)
{
    const struct sym_face *f;
    int lo = 0, hi, k, at = -1, best = -1;
    if (face < 0 || face >= OVL_FACES || !s_font.px)
        return NULL;
    f = &s_font.sym[face];
    hi = f->n - 1;
    while (lo <= hi && at < 0) {
        int mid = (lo + hi) / 2;
        if (f->cps[mid] == (int)cp)
            at = mid;
        else if (f->cps[mid] < (int)cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    if (at < 0)
        return NULL;
    for (k = 0; k < OVL_SYM_SIZES; k++)
        if (f->px[k] && (float)f->px[k] + 0.5f >= box && (best < 0 || f->px[k] < f->px[best]))
            best = k;
    if (best < 0)
        for (k = 0; k < OVL_SYM_SIZES; k++)
            if (best < 0 || f->px[k] > f->px[best])
                best = k;
    *px = f->px[best];
    return &f->ch[best][at];
}

int nv2a_ovl_font_symbol(int face, uint32_t cp, float x, float y, float box,
                         struct nv2a_ovl_quad *q)
{
    int px = 0;
    const stbtt_packedchar *b = sym_lookup(face, cp, box, &px);
    float k, w, h;
    if (!b || b->x1 <= b->x0 || b->y1 <= b->y0)
        return 0;
    /* Centred on its ink: Lucide's glyphs sit on the baseline at x 0, and
     * an input glyph's own margins differ by shape. Whole pixels, so a
     * symbol at its baked size maps texel for texel. */
    k = box / (float)px;
    w = (b->xoff2 - b->xoff) * k;
    h = (b->yoff2 - b->yoff) * k;
    q->x0 = floorf(x + (box - w) * 0.5f + 0.5f);
    q->y0 = floorf(y + (box - h) * 0.5f + 0.5f);
    q->x1 = q->x0 + w;
    q->y1 = q->y0 + h;
    q->u0 = (float)b->x0 / (float)s_font.atlas.w;
    q->u1 = (float)b->x1 / (float)s_font.atlas.w;
    q->v0 = (float)(b->y0 + GLYPH_ROW0) / (float)s_font.atlas.h;
    q->v1 = (float)(b->y1 + GLYPH_ROW0) / (float)s_font.atlas.h;
    return 1;
}
