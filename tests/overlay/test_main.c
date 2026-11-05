/*
 * overlay: the enhancements overlay (nv2a_overlay.h, R2, R7, R8).
 *
 *   disarmed  no context, no atlas, no list; a toast only logs
 *   frames    the draw lists of fixed frames (the stats strip, a toast
 *             stack, a menu-like page with every command kind) at 720,
 *             1080 and 2160 lines: checksums, and layout facts
 *   raster    the CPU reference raster of the same lists: checksums
 *   layers    publish, gen, hide, the visible mask, toast expiry
 *   threads   two writers on two layers and a reader: never a torn list
 *   toasts    a drain in the middle of a push takes the line later;
 *             two toast producers and a polling reader: every line once,
 *             whole and in each producer's order
 *   lock      the menu-step try-lock between two threads
 *   cut       a page larger than the menu's room is cut at a command
 *   stats     every site inert with the flag clear
 *   glyphs    the input glyph per family and source against glyphs.txt,
 *             the mouse and key glyphs, every icon present; a page of pad
 *             glyphs, icons and key caps through the reference raster
 *   fit       the atlas at effective scale 3.0 (3240 lines) with the text
 *             and both symbol faces fits 2048x2048
 *
 * A checksum of 0 below means "not pinned yet": the run prints the value.
 * A change to the converter, the font or the theme moves them: look at the
 * pictures (OVERLAY_REF_OUT) before pinning the new values.
 */
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_overlay_glyph.h"
#include "nv2a_overlay_int.h"
#include "nv2a_stats.h"
#include "nv2a_tex_pack.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* The runtime's pieces nv2a_stats.c and the strip read. */
int xbox_PacingMode(void) { return 1; }
unsigned nv2a_host_render_scale(void) { return 2; }
uint64_t nv2a_tex_pack_hits(void) { return 7; }
uint64_t nv2a_tex_pack_miss_total(void) { return 7; }

static const char *s_ref_out;

static void pin(const char *what, uint32_t got, uint32_t want)
{
    if (!want)
        printf("unpinned %s = 0x%08X\n", what, got);
    else
        CHECK(got == want, "%s: 0x%08X, want 0x%08X", what, got, want);
}

static uint32_t sum_of(const struct nv2a_ovl_list *l)
{
    return nv2a_overlay_list_sum(l->vtx, l->nvtx, l->idx, l->nidx, l->cmd, l->ncmd);
}

static void bounds(const struct nv2a_ovl_list *l, float b[4])
{
    uint32_t i;
    b[0] = b[1] = 1e9f;
    b[2] = b[3] = -1e9f;
    for (i = 0; i < l->nvtx; i++) {
        if (l->vtx[i].x < b[0]) b[0] = l->vtx[i].x;
        if (l->vtx[i].y < b[1]) b[1] = l->vtx[i].y;
        if (l->vtx[i].x > b[2]) b[2] = l->vtx[i].x;
        if (l->vtx[i].y > b[3]) b[3] = l->vtx[i].y;
    }
}

/* Every command within the list, every index within the vertices. */
static int well_formed(const struct nv2a_ovl_list *l)
{
    uint32_t i, end = 0;
    for (i = 0; i < l->ncmd; i++) {
        if (l->cmd[i].first != end || l->cmd[i].count % 3)
            return 0;
        end += l->cmd[i].count;
    }
    if (end != l->nidx)
        return 0;
    for (i = 0; i < l->nidx; i++)
        if (l->idx[i] >= l->nvtx)
            return 0;
    return 1;
}

static uint32_t raster(const char *name, const struct nv2a_ovl_list *l, int w, int h)
{
    uint32_t *px = calloc((size_t)w * h, 4), s;
    nv2a_ovl_ref_draw(px, w, h, l, nv2a_overlay_atlas());
    s = nv2a_overlay_list_sum(NULL, 0, (const uint16_t *)px, (uint32_t)w * h * 2, NULL, 0);
    if (s_ref_out) {
        char path[1024];
        FILE *f;
        int i;
        snprintf(path, sizeof path, "%s/%s.ppm", s_ref_out, name);
        f = fopen(path, "wb");
        if (f) {
            /* Over mid grey, as a frame behind it. */
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (i = 0; i < w * h; i++) {
                const unsigned char *p = (const unsigned char *)&px[i];
                unsigned char o[3];
                int k;
                for (k = 0; k < 3; k++)
                    o[k] = (unsigned char)(p[k] + 96 * (255 - p[3]) / 255);
                fwrite(o, 1, 3, f);
            }
            fclose(f);
        }
    }
    free(px);
    return s;
}

static void test_disarmed(void)
{
    int l;
    nv2a_overlay_reset_for_test();
    nv2a_overlay_toast("disarmed %d", 1);
    nv2a_overlay_set_stats(1);
    CHECK(!nv2a_overlay_armed(), "armed");
    CHECK(nv2a_overlay_visible() == 0, "visible 0x%x", nv2a_overlay_visible());
    CHECK(nv2a_overlay_atlas() == NULL, "an atlas");
    CHECK(nv2a_overlay_stats() == 0, "stats on");
    for (l = 0; l < NV2A_OVL_COUNT; l++) {
        CHECK(nv2a_ovl_ctx((enum nv2a_overlay_layer)l) == NULL, "a context for %d", l);
        CHECK(nv2a_overlay_acquire((enum nv2a_overlay_layer)l) == NULL, "a list for %d", l);
        CHECK(nv2a_overlay_ui_begin((enum nv2a_overlay_layer)l) == NULL, "a frame for %d", l);
    }
    CHECK(nv2a_overlay_image_set(1, (const uint32_t *)"\0\0\0\0", 1, 1) < 0, "an image");
    CHECK(nv2a_stats_enabled() == 0, "stats counting");
}

static const char k_line[] = "59.9 fps \xC2\xB7 16.7 ms (max 33.4) \xC2\xB7 pacing sleep \xC2\xB7 2x \xC2\xB7 Metal";

static const struct nv2a_ovl_list *frame_stats(uint32_t w, uint32_t h, int spike)
{
    mu_Context *ctx;
    nv2a_overlay_set_placement(0, 0, (int)w, (int)h);
    ctx = nv2a_overlay_ui_begin(NV2A_OVL_STATS);
    nv2a_ovl_compose_stats(ctx, w, h, k_line, spike);
    nv2a_overlay_ui_publish(NV2A_OVL_STATS);
    return nv2a_overlay_acquire(NV2A_OVL_STATS);
}

static const struct nv2a_ovl_list *frame_toasts(uint32_t w, uint32_t h)
{
    static const char *const lines[] = { "render.scale=3", "Saved to config.toml",
                                         "\xE2\x86\x90 Caf\xC3\xA9 \xE2\x80\x94 \xE2\x9C\x93" };
    mu_Context *ctx;
    nv2a_overlay_set_placement(0, 0, (int)w, (int)h);
    ctx = nv2a_overlay_ui_begin(NV2A_OVL_TOAST);
    nv2a_ovl_compose_toasts(ctx, w, h, lines, 3);
    nv2a_overlay_ui_publish(NV2A_OVL_TOAST);
    return nv2a_overlay_acquire(NV2A_OVL_TOAST);
}

/* A page with every command kind: microui's own widgets (a frame, text,
 * a check mark), clips, rounded rects, a shadow, an image, both cursors. */
static const struct nv2a_ovl_list *frame_menu(uint32_t w, uint32_t h)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    float s = nv2a_ovl_ui_scale(h);
    int pad = nv2a_ovl_px(s, t->pad), check = 1;
    mu_Rect panel = mu_rect(pad, pad, (int)w * 2 / 5, (int)h - 2 * pad);
    mu_Context *ctx;
    static uint32_t img[16 * 16];
    int i;
    for (i = 0; i < 16 * 16; i++)
        img[i] = (i & 1) ? 0xff00ff00u : 0x80000080u;
    nv2a_overlay_image_set(1, img, 16, 16);
    nv2a_overlay_set_placement(0, 0, (int)w, (int)h);
    ctx = nv2a_overlay_ui_begin(NV2A_OVL_MENU);
    nv2a_ovl_ui_shadow(ctx, panel, nv2a_ovl_px(s, t->radius), nv2a_ovl_px(s, t->shadow),
                       t->shadow_alpha);
    nv2a_ovl_ui_rrect(ctx, panel, nv2a_ovl_px(s, t->radius), 1.0f, t->bg);
    mu_push_clip_rect(ctx, panel);
    mu_draw_text(ctx, OVL_FONT(OVL_FONT_TITLE, nv2a_ovl_px(s, t->font_title), 0), "Video", -1,
                 mu_vec2(panel.x + pad, panel.y + pad), t->text);
    mu_draw_text(ctx, OVL_FONT(OVL_FONT_BODY, nv2a_ovl_px(s, t->font_body), 0),
                 "A row whose text runs past the panel's right edge, clipped there", -1,
                 mu_vec2(panel.x + panel.w - 6 * pad, panel.y + 3 * pad), t->text_dim);
    mu_layout_set_next(ctx, mu_rect(panel.x + pad, panel.y + 5 * pad, panel.w - 2 * pad,
                                    nv2a_ovl_px(s, t->row_h)), 0);
    mu_checkbox(ctx, "Fullscreen", &check);
    mu_pop_clip_rect(ctx);
    nv2a_ovl_ui_image(ctx, mu_rect(panel.x + pad, panel.y + 9 * pad, 8 * pad, 8 * pad), 1,
                      mu_color(255, 255, 255, 255));
    nv2a_ovl_ui_cursor(ctx, OVL_CURSOR_ARROW, (int)w / 2, (int)h / 2, nv2a_ovl_px(s, 24),
                       t->text, mu_color(0, 0, 0, 255));
    nv2a_ovl_ui_cursor(ctx, OVL_CURSOR_DOT, (int)w * 3 / 4, (int)h / 2, nv2a_ovl_px(s, 24),
                       t->accent, mu_color(0, 0, 0, 255));
    nv2a_overlay_ui_publish(NV2A_OVL_MENU);
    return nv2a_overlay_acquire(NV2A_OVL_MENU);
}

static void test_frames(void)
{
    static const struct { uint32_t w, h, stats, toasts, menu, r_stats, r_toasts, r_menu; } k[] = {
        { 1280, 720, 0xFD8E9965, 0xC53253A5, 0x8D3BDF3E, 0x12641917, 0xEBC8FED3, 0xFCE76C3F },
        { 1920, 1080, 0xDD4A23F1, 0xA0D4E102, 0xCB4EB246, 0x43F3F099, 0xFC74F6D4, 0xD50FC312 },
        { 3840, 2160, 0x9D985883, 0x5F2183B5, 0x19B3A385, 0xA795B36D, 0x776C110B, 0xAC983065 },
    };
    const struct nv2a_ovl_atlas *a;
    unsigned i;
    nv2a_overlay_reset_for_test();
    CHECK(nv2a_overlay_arm(0xF, 1080) == 0, "arm");
    a = nv2a_overlay_atlas();
    CHECK(a && a->px && a->w >= 512 && a->h >= 512, "no atlas");
    if (!a)
        return;
    CHECK(a->px[2 * a->w + 2] == 0xffffffffu, "white texel 0x%08X", a->px[2 * a->w + 2]);
    CHECK(nv2a_ovl_text_height(OVL_FONT(OVL_FONT_SMALL, 17, 0)) == 17, "line height");
    CHECK(nv2a_ovl_text_width(OVL_FONT(OVL_FONT_SMALL, 17, 1), "1111", -1)
              == nv2a_ovl_text_width(OVL_FONT(OVL_FONT_SMALL, 17, 1), "8888", -1),
          "tabular digits differ");
    CHECK(nv2a_ovl_text_width(OVL_FONT(OVL_FONT_SMALL, 34, 0), "fps", -1)
              > nv2a_ovl_text_width(OVL_FONT(OVL_FONT_SMALL, 17, 0), "fps", -1) * 3 / 2,
          "a scaled size is not wider");
    /* A code point the face lacks (an arrow, a check mark) draws the box. */
    CHECK(nv2a_ovl_text_width(OVL_FONT(OVL_FONT_BODY, 22, 0), "\xE2\x9C\x93", -1) > 0
              && nv2a_ovl_text_width(OVL_FONT(OVL_FONT_BODY, 22, 0), "\xE2\x86\x90", -1)
                     == nv2a_ovl_text_width(OVL_FONT(OVL_FONT_BODY, 22, 0), "\xE2\x9C\x93", -1),
          "a missing glyph is not the box");

    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        char what[64];
        const struct nv2a_ovl_list *l;
        float b[4], s = nv2a_ovl_ui_scale(k[i].h);
        int pad = nv2a_ovl_px(s, nv2a_ovl_theme_default.pad);

        l = frame_stats(k[i].w, k[i].h, 1);
        CHECK(l && well_formed(l), "stats at %u: malformed", k[i].h);
        if (!l)
            continue;
        CHECK(l->out_w == k[i].w && l->out_h == k[i].h, "stats placement");
        bounds(l, b);
        /* The strip's right edge one pad in from the frame's, its top one
         * pad down, give or take the half-pixel feather; it is one line. */
        CHECK(b[2] <= (float)k[i].w - (float)pad + 1.0f && b[2] >= (float)k[i].w - (float)pad - 1.0f,
              "stats at %u: right %.1f, want %d", k[i].h, b[2], (int)k[i].w - pad);
        CHECK(b[1] >= (float)pad - 1.0f && b[1] <= (float)pad + 0.5f, "stats at %u: top %.1f",
              k[i].h, b[1]);
        CHECK(b[3] - b[1] < 3.0f * nv2a_ovl_px(s, 17), "stats at %u: %.1f tall", k[i].h,
              b[3] - b[1]);
        CHECK(l->sum == sum_of(l), "stats sum");
        snprintf(what, sizeof what, "stats list at %u", k[i].h);
        pin(what, l->sum, k[i].stats);
        snprintf(what, sizeof what, "stats-%u", k[i].h);
        pin(what, raster(what, l, (int)k[i].w, (int)k[i].h), k[i].r_stats);

        l = frame_toasts(k[i].w, k[i].h);
        CHECK(l && well_formed(l), "toasts at %u: malformed", k[i].h);
        if (!l)
            continue;
        bounds(l, b);
        /* Bottom centre: symmetric about the middle, above two pads. */
        CHECK(b[3] <= (float)k[i].h - 2.0f * (float)pad + 8.0f * s, "toasts at %u: bottom %.1f",
              k[i].h, b[3]);
        CHECK(b[0] + b[2] > (float)k[i].w - 4.0f && b[0] + b[2] < (float)k[i].w + 4.0f,
              "toasts at %u: not centred (%.1f..%.1f)", k[i].h, b[0], b[2]);
        snprintf(what, sizeof what, "toast list at %u", k[i].h);
        pin(what, l->sum, k[i].toasts);
        snprintf(what, sizeof what, "toasts-%u", k[i].h);
        pin(what, raster(what, l, (int)k[i].w, (int)k[i].h), k[i].r_toasts);

        l = frame_menu(k[i].w, k[i].h);
        CHECK(l && well_formed(l), "menu at %u: malformed", k[i].h);
        if (!l)
            continue;
        {
            uint32_t c, img = 0, clipped = 0;
            for (c = 0; c < l->ncmd; c++) {
                img += l->cmd[c].tex == 1;
                clipped += l->cmd[c].clip[2] < (int16_t)k[i].w;
                CHECK(l->cmd[c].clip[0] >= 0 && l->cmd[c].clip[1] >= 0
                          && l->cmd[c].clip[0] + l->cmd[c].clip[2] <= (int)k[i].w
                          && l->cmd[c].clip[1] + l->cmd[c].clip[3] <= (int)k[i].h,
                      "menu at %u: a clip leaves the output", k[i].h);
            }
            CHECK(img == 1, "menu at %u: %u image commands", k[i].h, img);
            CHECK(clipped >= 1, "menu at %u: no clipped command", k[i].h);
        }
        snprintf(what, sizeof what, "menu list at %u", k[i].h);
        pin(what, l->sum, k[i].menu);
        snprintf(what, sizeof what, "menu-%u", k[i].h);
        pin(what, raster(what, l, (int)k[i].w, (int)k[i].h), k[i].r_menu);
    }

    /* A list drawn at another size is the same picture scaled: the 1080
     * strip drawn into 2160 lines covers the same fraction. */
    {
        const struct nv2a_ovl_list *l = frame_stats(1920, 1080, 0);
        uint32_t *px = calloc((size_t)3840 * 2160, 4);
        int x, y, minx = 1 << 30, miny = 1 << 30;
        nv2a_ovl_ref_draw(px, 3840, 2160, l, nv2a_overlay_atlas());
        for (y = 0; y < 2160; y++)
            for (x = 0; x < 3840; x++)
                if (px[(size_t)y * 3840 + x] >> 24 > 128) {
                    if (x < minx) minx = x;
                    if (y < miny) miny = y;
                }
        {
            float b[4];
            bounds(l, b);
            CHECK(abs(minx - (int)(b[0] * 2)) <= 3 && abs(miny - (int)(b[1] * 2)) <= 3,
                  "scaled: ink from (%d, %d), list from (%.1f, %.1f) x2", minx, miny, b[0], b[1]);
        }
        free(px);
    }
}

static void test_layers(void)
{
    const struct nv2a_ovl_list *l;
    struct nv2a_ovl_build *b;
    uint64_t g, e0, e1;
    nv2a_overlay_reset_for_test();
    /* The cursor layer for the raw publishes: a visible STATS layer is the
     * strip, which the poll recomposes. */
    CHECK(nv2a_overlay_arm(1u << NV2A_OVL_STATS | 1u << NV2A_OVL_TOAST | 1u << NV2A_OVL_CURSOR,
                           1080) == 0, "arm");
    CHECK(nv2a_ovl_ctx(NV2A_OVL_MENU) == NULL, "menu armed with it");
    CHECK(nv2a_overlay_visible() == 0, "visible before a publish");
    nv2a_overlay_set_placement(0, 0, 1280, 720);
    e0 = nv2a_overlay_poll(0);

    b = nv2a_overlay_build_begin(NV2A_OVL_CURSOR, 1280, 720);
    nv2a_overlay_build_publish(NV2A_OVL_CURSOR, 11);
    CHECK(nv2a_overlay_visible() == 1u << NV2A_OVL_CURSOR, "visible 0x%x", nv2a_overlay_visible());
    l = nv2a_overlay_acquire(NV2A_OVL_CURSOR);
    CHECK(l && l->gen == 1 && l->sum == 11, "first publish");
    g = l ? l->gen : 0;
    CHECK(nv2a_overlay_acquire(NV2A_OVL_CURSOR) == l, "a second acquire moved");
    e1 = nv2a_overlay_poll(0);
    CHECK(e1 != e0, "poll did not move on a publish");
    (void)b;
    nv2a_overlay_build_begin(NV2A_OVL_CURSOR, 1280, 720);
    nv2a_overlay_build_publish(NV2A_OVL_CURSOR, 12);
    l = nv2a_overlay_acquire(NV2A_OVL_CURSOR);
    CHECK(l && l->gen == g + 1 && l->sum == 12, "second publish");
    nv2a_overlay_hide(NV2A_OVL_CURSOR);
    CHECK(nv2a_overlay_visible() == 0, "visible after hide");
    CHECK(nv2a_overlay_acquire(NV2A_OVL_CURSOR) == NULL, "a hidden list");
    CHECK(nv2a_overlay_poll(0) != e1, "poll did not move on a hide");

    /* Toasts: shown from the push, gone 2 s after the drain. */
    nv2a_overlay_toast("one");
    CHECK(nv2a_overlay_visible() & 1u << NV2A_OVL_TOAST, "toast not visible");
    nv2a_overlay_poll(1000);
    l = nv2a_overlay_acquire(NV2A_OVL_TOAST);
    CHECK(l && l->nidx > 0, "no toast list");
    nv2a_overlay_toast("two");
    nv2a_overlay_poll(2500);
    nv2a_overlay_poll(3000);
    CHECK(nv2a_overlay_visible() & 1u << NV2A_OVL_TOAST, "two expired early");
    nv2a_overlay_poll(4600);
    CHECK(!(nv2a_overlay_visible() & 1u << NV2A_OVL_TOAST), "toasts outlived 2 s");
    {
        /* Ten pushes before a drain: the ring keeps the newest 8, the
         * stack the newest 3. */
        int i;
        for (i = 0; i < 10; i++)
            nv2a_overlay_toast("burst %d", i);
        nv2a_overlay_poll(5000);
        l = nv2a_overlay_acquire(NV2A_OVL_TOAST);
        CHECK(l && l->nidx > 0, "burst not shown");
    }

    /* The strip through the action path: set_stats and the 2 Hz poll. */
    nv2a_overlay_set_presenter("Test");
    nv2a_overlay_set_stats(1);
    CHECK(nv2a_overlay_stats() && nv2a_stats_enabled(), "stats on");
    nv2a_overlay_poll(6000);
    l = nv2a_overlay_acquire(NV2A_OVL_STATS);
    CHECK(l && l->nidx > 0, "no strip");
    g = l ? l->gen : 0;
    nv2a_overlay_poll(6100);
    l = nv2a_overlay_acquire(NV2A_OVL_STATS);
    CHECK(l && l->gen == g, "the strip recomposed before 500 ms");
    nv2a_overlay_poll(6600);
    l = nv2a_overlay_acquire(NV2A_OVL_STATS);
    CHECK(l && l->gen == g + 1, "the strip did not recompose at 500 ms");
    nv2a_overlay_set_stats(0);
    CHECK(!nv2a_overlay_stats() && !nv2a_stats_enabled(), "stats off");
    CHECK(!(nv2a_overlay_visible() & 1u << NV2A_OVL_STATS), "strip visible when off");
}

/* ── Two writers, one reader ── */

static volatile int s_stop;

static void *writer(void *arg)
{
    enum nv2a_overlay_layer l = (enum nv2a_overlay_layer)(intptr_t)arg;
    uint32_t seed = 1u + (uint32_t)l, n = 0;
    while (!s_stop) {
        struct nv2a_ovl_build *b = nv2a_overlay_build_begin(l, 640, 480);
        uint32_t i, k;
        seed = seed * 1103515245u + 12345u;
        k = 1 + (seed >> 16) % (b->cap_vtx / 4);
        b->ncmd = 1;
        b->cmd[0].first = 0;
        b->cmd[0].tex = 0;
        memset(b->cmd[0].clip, 0, sizeof b->cmd[0].clip);
        for (i = 0; i < 4 * k; i++) {
            b->vtx[i].x = (float)(seed ^ i);
            b->vtx[i].y = (float)n;
            b->vtx[i].u = b->vtx[i].v = 0;
            b->vtx[i].rgba = seed + i;
        }
        for (i = 0; i < 6 * k; i++)
            b->idx[i] = (uint16_t)(i % (4 * k));
        b->nvtx = 4 * k;
        b->nidx = 6 * k;
        b->cmd[0].count = 6 * k;
        nv2a_overlay_build_publish(l, nv2a_overlay_list_sum(b->vtx, b->nvtx, b->idx, b->nidx,
                                                            b->cmd, b->ncmd));
        n++;
    }
    return NULL;
}

static void test_threads(void)
{
    pthread_t t[2];
    int i, torn = 0, seen = 0;
    nv2a_overlay_reset_for_test();
    CHECK(nv2a_overlay_arm(1u << NV2A_OVL_MENU | 1u << NV2A_OVL_CURSOR, 1080) == 0, "arm");
    s_stop = 0;
    pthread_create(&t[0], NULL, writer, (void *)(intptr_t)NV2A_OVL_MENU);
    pthread_create(&t[1], NULL, writer, (void *)(intptr_t)NV2A_OVL_CURSOR);
    for (i = 0; i < 20000; i++) {
        const struct nv2a_ovl_list *out[NV2A_OVL_COUNT];
        int n = nv2a_overlay_lists(out), j;
        for (j = 0; j < n; j++) {
            seen++;
            if (out[j]->sum != sum_of(out[j]))
                torn++;
        }
    }
    s_stop = 1;
    pthread_join(t[0], NULL);
    pthread_join(t[1], NULL);
    CHECK(seen > 0, "the reader saw nothing");
    CHECK(torn == 0, "%d torn lists of %d", torn, seen);
}

/* Two producers push TOAST_N lines each while the main thread polls. They
 * keep at most four lines in flight, half the ring, so none is lapped and
 * every push must reach the drain: a line still being written when the
 * drain passes is taken at a later poll, never dropped. */
#define TOAST_N 400
static unsigned s_pushed, s_taken;  /* atomic */
static int s_next[2], s_bad, s_toast_stop;

static void toast_seen(const char *line)
{
    int w, n, len;
    char want[96];
    if (sscanf(line, "w%d n%d%n", &w, &n, &len) != 2 || w < 0 || w > 1) {
        s_bad++;
        return;
    }
    /* The tail repeats the head: a torn line would not match. */
    snprintf(want, sizeof want, "w%d n%d |%064d", w, n, n);
    if (strcmp(line, want) != 0 || n != s_next[w])
        s_bad++;
    s_next[w] = n + 1;
    __atomic_add_fetch(&s_taken, 1, __ATOMIC_RELEASE);
}

static void poll_mid_push(void)
{
    nv2a_overlay_poll(1);
}

static void *toaster(void *arg)
{
    int w = (int)(intptr_t)arg, n;
    for (n = 0; n < TOAST_N; n++) {
        while (__atomic_load_n(&s_pushed, __ATOMIC_ACQUIRE)
                   - __atomic_load_n(&s_taken, __ATOMIC_ACQUIRE) >= 4) {
            if (__atomic_load_n(&s_toast_stop, __ATOMIC_ACQUIRE))
                return NULL;        /* lines were lost: the check below fails */
            sched_yield();
        }
        __atomic_add_fetch(&s_pushed, 1, __ATOMIC_ACQ_REL);
        nv2a_overlay_toast("w%d n%d |%064d", w, n, n);
    }
    return NULL;
}

static void test_toasts(void)
{
    pthread_t t[2];
    long spins = 0;
    nv2a_overlay_reset_for_test();
    CHECK(nv2a_overlay_arm(1u << NV2A_OVL_TOAST, 1080) == 0, "arm");
    nv2a_overlay_set_placement(0, 0, 1280, 720);
    s_pushed = s_taken = 0;
    s_next[0] = s_next[1] = 0;
    s_bad = 0;
    /* The head is bumped before the entry is written: a drain between the
     * two must leave the entry for the next poll. */
    nv2a_overlay_toast_hook_for_test(toast_seen, NULL);
    nv2a_overlay_toast("w0 n0 |%064d", 0);
    nv2a_overlay_poll(1);
    nv2a_overlay_toast_hook_for_test(toast_seen, poll_mid_push);
    nv2a_overlay_toast("w0 n1 |%064d", 1);
    nv2a_overlay_toast_hook_for_test(toast_seen, NULL);
    CHECK(s_taken == 1, "the drain took %u lines mid-push", s_taken);
    nv2a_overlay_poll(1);
    CHECK(s_taken == 2 && s_bad == 0, "the line pushed under a drain was lost (%u, %d)",
          s_taken, s_bad);
    nv2a_overlay_reset_for_test();
    CHECK(nv2a_overlay_arm(1u << NV2A_OVL_TOAST, 1080) == 0, "arm");
    nv2a_overlay_set_placement(0, 0, 1280, 720);
    nv2a_overlay_toast_hook_for_test(toast_seen, NULL);
    s_pushed = s_taken = 0;
    s_next[0] = s_next[1] = 0;
    s_bad = 0;
    s_toast_stop = 0;
    pthread_create(&t[0], NULL, toaster, (void *)(intptr_t)0);
    pthread_create(&t[1], NULL, toaster, (void *)(intptr_t)1);
    /* now_ms stays at 1: nothing expires, so the layer stays visible. */
    while (__atomic_load_n(&s_taken, __ATOMIC_ACQUIRE) < 2 * TOAST_N && spins < 50000000L) {
        nv2a_overlay_poll(1);
        spins++;
    }
    __atomic_store_n(&s_toast_stop, 1, __ATOMIC_RELEASE);
    pthread_join(t[0], NULL);
    pthread_join(t[1], NULL);
    nv2a_overlay_poll(1);
    CHECK(s_taken == 2 * TOAST_N, "%u of %d toasts reached the drain", s_taken, 2 * TOAST_N);
    CHECK(s_bad == 0, "%d torn or out-of-order toasts", s_bad);
    CHECK(s_next[0] == TOAST_N && s_next[1] == TOAST_N, "last lines %d, %d", s_next[0],
          s_next[1]);
}

static int s_held, s_other;

static void *other_locker(void *arg)
{
    (void)arg;
    s_other = nv2a_overlay_menu_trylock();
    if (s_other)
        nv2a_overlay_menu_unlock();
    return NULL;
}

static void test_lock(void)
{
    pthread_t t;
    s_held = nv2a_overlay_menu_trylock();
    CHECK(s_held, "first trylock");
    pthread_create(&t, NULL, other_locker, NULL);
    pthread_join(t, NULL);
    CHECK(!s_other, "a second writer took the held lock");
    nv2a_overlay_menu_unlock();
    pthread_create(&t, NULL, other_locker, NULL);
    pthread_join(t, NULL);
    CHECK(s_other, "the released lock refused");
}

static void test_cut(void)
{
    mu_Context *ctx;
    const struct nv2a_ovl_list *l;
    int i;
    nv2a_overlay_reset_for_test();
    CHECK(nv2a_overlay_arm(1u << NV2A_OVL_MENU, 1080) == 0, "arm");
    nv2a_overlay_set_placement(0, 0, 1920, 1080);
    ctx = nv2a_overlay_ui_begin(NV2A_OVL_MENU);
    /* 600 lines of 40 glyphs: 96k vertices for a room of 16k. */
    for (i = 0; i < 600; i++)
        mu_draw_text(ctx, OVL_FONT(OVL_FONT_SMALL, 17, 0), "0123456789abcdefghijABCDEFGHIJ0123456789",
                     -1, mu_vec2(0, i), mu_color(255, 255, 255, 255));
    nv2a_overlay_ui_publish(NV2A_OVL_MENU);
    l = nv2a_overlay_acquire(NV2A_OVL_MENU);
    CHECK(l && well_formed(l), "the cut list is malformed");
    CHECK(l && l->nvtx <= 16384 && l->nvtx > 16384 - 4 * 40, "cut at %u vertices",
          l ? l->nvtx : 0);
    CHECK(l && l->nvtx % 4 == 0, "a glyph cut in half");
}

/* glyphs.txt, the generator's own listing of the input font: U+XXXX,
 * block, slot name, source. */
#define MAX_ROWS 256
static uint32_t s_rows[MAX_ROWS];
static int s_nrows;

static int load_glyphs_txt(void)
{
    FILE *f = fopen(GLYPHS_TXT, "r");
    char line[256];
    if (!f)
        return 0;
    s_nrows = 0;
    while (fgets(line, sizeof line, f) && s_nrows < MAX_ROWS) {
        unsigned cp;
        if (sscanf(line, "U+%X", &cp) == 1)
            s_rows[s_nrows++] = cp;
    }
    fclose(f);
    return s_nrows;
}

static int listed(uint32_t cp)
{
    int i;
    for (i = 0; i < s_nrows; i++)
        if (s_rows[i] == cp)
            return 1;
    return 0;
}

static const char *test_namer(int vk, char *buf, int n)
{
    switch (vk) {
    case 0x1B: snprintf(buf, (size_t)n, "Esc"); return buf;
    case 0x20: snprintf(buf, (size_t)n, "Space"); return buf;
    case 0x70: snprintf(buf, (size_t)n, "F1"); return buf;
    }
    return NULL;
}

/* The pad glyphs of every family, the mouse, the icons and three key
 * caps (two named, one in hex) on one panel. */
static const struct nv2a_ovl_list *frame_glyphs(uint32_t w, uint32_t h)
{
    static const int srcs[] = { 1, 2, 3, 4, 6, 7, 10, 22, NV2A_OVL_SRC_DPAD, NV2A_OVL_SRC_LSTICK };
    static const uint32_t icons[] = { NV2A_OVL_ICON_SLIDERS, NV2A_OVL_ICON_GAMEPAD,
                                      NV2A_OVL_ICON_KEYBOARD, NV2A_OVL_ICON_CHECK,
                                      NV2A_OVL_ICON_CLOSE, NV2A_OVL_ICON_FOLDER };
    static const int keys[] = { 0x1B, 0x20, 0x70, 0x41 };
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    float s = nv2a_ovl_ui_scale(h);
    int pad = nv2a_ovl_px(s, t->pad), line = nv2a_ovl_px(s, t->font_body);
    mu_Font body = OVL_FONT(OVL_FONT_BODY, line, 0);
    mu_Rect panel = mu_rect(pad, pad, (int)w / 2, (int)h - 2 * pad);
    mu_Context *ctx;
    int fam, i, x, y = panel.y + pad;
    nv2a_overlay_set_placement(0, 0, (int)w, (int)h);
    ctx = nv2a_overlay_ui_begin(NV2A_OVL_MENU);
    nv2a_ovl_ui_rrect(ctx, panel, nv2a_ovl_px(s, t->radius), 1.0f, t->bg);
    for (fam = 0; fam < 5; fam++, y += line + pad / 2) {
        x = panel.x + pad;
        for (i = 0; i < (int)(sizeof srcs / sizeof srcs[0]); i++, x += line + pad / 2)
            nv2a_ovl_ui_glyph(ctx, nv2a_ovl_glyph_src(fam, srcs[i]), mu_rect(x, y, line, line),
                              t->text);
    }
    x = panel.x + pad;
    for (i = 0; i < NV2A_OVL_MOUSE_COUNT; i++, x += line + pad / 2)
        nv2a_ovl_ui_glyph(ctx, nv2a_ovl_glyph_mouse(i), mu_rect(x, y, line, line), t->text);
    y += line + pad / 2;
    x = panel.x + pad;
    for (i = 0; i < (int)(sizeof icons / sizeof icons[0]); i++, x += line + pad / 2)
        nv2a_ovl_ui_glyph(ctx, nv2a_ovl_glyph_icon(icons[i]), mu_rect(x, y, line, line), t->accent);
    y += line + pad / 2;
    x = panel.x + pad;
    for (i = 0; i < (int)(sizeof keys / sizeof keys[0]); i++) {
        nv2a_ovl_glyph g = nv2a_ovl_glyph_key(keys[i]);
        int gw = nv2a_ovl_glyph_width(g, body);
        nv2a_ovl_ui_glyph(ctx, g, mu_rect(x, y, gw, line), t->text);
        x += gw + pad / 2;
    }
    y += line + pad / 2;
    /* The rebinding prompt's size: twice body. */
    nv2a_ovl_ui_glyph(ctx, nv2a_ovl_glyph_src(2, 1), mu_rect(panel.x + pad, y, 2 * line, 2 * line),
                      t->text);
    nv2a_overlay_ui_publish(NV2A_OVL_MENU);
    return nv2a_overlay_acquire(NV2A_OVL_MENU);
}

static void test_glyphs(void)
{
    static const struct { uint32_t w, h, list, ref; } k[] = {
        { 1920, 1080, 0xC52EFB2D, 0xB1EF752D },
        { 3840, 2160, 0xE13B42EC, 0x661A4547 },
    };
    int fam, src, i;
    unsigned j;
    CHECK(load_glyphs_txt() > 100, "glyphs.txt at %s: %d rows", GLYPHS_TXT, s_nrows);
    for (i = 0; i < s_nrows; i++)
        CHECK(nv2a_ovl_font_has_symbol(OVL_FACE_INPUT, s_rows[i]), "U+%04X listed, not in the font",
              s_rows[i]);
    /* Per family and source: the family's own glyph, the home glyph for
     * the guide, the Xbox glyph for what the generic family lacks and the
     * Nintendo one for what the GameCube lacks, else none. Positional:
     * Nintendo's south is its "B". The GameCube's block is 7. */
    for (fam = 0; fam < 6; fam++)
        for (src = 1; src <= NV2A_OVL_SRC_RSTICK; src++) {
            uint32_t blk = fam == 5 ? 7u : (uint32_t)fam;
            uint32_t own = 0xE000u + 0x40u * blk + (uint32_t)src, want = 0;
            nv2a_ovl_glyph g = nv2a_ovl_glyph_src(fam, src);
            if (src == 6)
                want = 0xE180;
            else if (listed(own))
                want = own;
            else if (fam == 4 && listed(0xE000u + (uint32_t)src))
                want = 0xE000u + (uint32_t)src;
            else if (fam == 5 && listed(0xE080u + (uint32_t)src))
                want = 0xE080u + (uint32_t)src;
            CHECK(want ? g == NV2A_OVL_GLYPH(NV2A_OVL_GLYPH_INPUT, want) : g == 0,
                  "family %d source %d: 0x%08X, want U+%04X", fam, src, g, want);
        }
    CHECK(nv2a_ovl_glyph_src(2, 1) == NV2A_OVL_GLYPH(NV2A_OVL_GLYPH_INPUT, 0xE081),
          "Switch south is not B");
    CHECK(nv2a_ovl_glyph_src(5, 1) == NV2A_OVL_GLYPH(NV2A_OVL_GLYPH_INPUT, 0xE1C1)
              && nv2a_ovl_glyph_src(5, 3) == NV2A_OVL_GLYPH(NV2A_OVL_GLYPH_INPUT, 0xE1C3)
              && nv2a_ovl_glyph_src(5, 5) == nv2a_ovl_glyph_src(2, 5),
          "GameCube: its own A south and B west, the Switch's minus for back");
    CHECK(nv2a_ovl_glyph_src(4, 1) == nv2a_ovl_glyph_src(0, 1) && nv2a_ovl_glyph_src(4, 5)
              == NV2A_OVL_GLYPH(NV2A_OVL_GLYPH_INPUT, 0xE105), "generic south and back");
    CHECK(nv2a_ovl_glyph_src(0, 18) == 0 && nv2a_ovl_glyph_src(3, 18) != 0,
          "paddle1: Xbox has none, the Deck has R4");
    CHECK(nv2a_ovl_glyph_src(-1, 1) == nv2a_ovl_glyph_src(0, 1)
              && nv2a_ovl_glyph_src(9, 1) == nv2a_ovl_glyph_src(0, 1), "out-of-range family");
    CHECK(nv2a_ovl_glyph_src(0, 0) == 0 && nv2a_ovl_glyph_src(0, NV2A_OVL_SRC_RSTICK + 1) == 0,
          "out-of-range source");
    for (i = 0; i < NV2A_OVL_MOUSE_COUNT; i++)
        CHECK(nv2a_ovl_glyph_mouse(i) == NV2A_OVL_GLYPH(NV2A_OVL_GLYPH_INPUT, 0xE140u + (uint32_t)i),
              "mouse %d", i);
    CHECK(nv2a_ovl_glyph_mouse(-1) == 0 && nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_COUNT) == 0,
          "out-of-range mouse");
    CHECK(nv2a_ovl_glyph_key(0x01) == nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_LEFT)
              && nv2a_ovl_glyph_key(0x02) == nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_RIGHT)
              && nv2a_ovl_glyph_key(0x04) == nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_MIDDLE),
          "mouse button keys");
    CHECK(nv2a_ovl_glyph_key(0x41) == NV2A_OVL_GLYPH(NV2A_OVL_GLYPH_KEY, 0x41)
              && nv2a_ovl_glyph_key(0) == 0 && nv2a_ovl_glyph_key(0x100) == 0, "key glyphs");
    {
        static const uint32_t icons[] = {
            NV2A_OVL_ICON_SLIDERS, NV2A_OVL_ICON_SPARKLES, NV2A_OVL_ICON_PACKAGE,
            NV2A_OVL_ICON_GAMEPAD, NV2A_OVL_ICON_PUZZLE, NV2A_OVL_ICON_ACTIVITY,
            NV2A_OVL_ICON_KEYBOARD, NV2A_OVL_ICON_BUG, NV2A_OVL_ICON_FOLDER,
            NV2A_OVL_ICON_HELP, NV2A_OVL_ICON_MAXIMIZE, NV2A_OVL_ICON_MINIMIZE,
            NV2A_OVL_ICON_CLOSE, NV2A_OVL_ICON_CHECK, NV2A_OVL_ICON_CHEVRON_LEFT,
            NV2A_OVL_ICON_CHEVRON_RIGHT, NV2A_OVL_ICON_RESET, NV2A_OVL_ICON_UPLOAD,
            NV2A_OVL_ICON_DOWNLOAD,
        };
        for (j = 0; j < sizeof icons / sizeof icons[0]; j++)
            CHECK(nv2a_ovl_glyph_icon(icons[j]) == NV2A_OVL_GLYPH(NV2A_OVL_GLYPH_ICON, icons[j]),
                  "icon U+%04X missing", icons[j]);
        CHECK(nv2a_ovl_glyph_icon(0x41) == 0, "a letter is an icon");
    }

    nv2a_overlay_reset_for_test();
    CHECK(nv2a_overlay_arm(0xF, 1080) == 0, "arm");
    {
        mu_Font f = OVL_FONT(OVL_FONT_BODY, 22, 0);
        nv2a_ovl_glyph_set_key_namer(NULL);
        CHECK(nv2a_ovl_glyph_width(nv2a_ovl_glyph_src(0, 1), f) == 22, "a symbol is a square");
        CHECK(nv2a_ovl_glyph_width(nv2a_ovl_glyph_key(0x41), f) >= 22, "a cap narrower than tall");
        nv2a_ovl_glyph_set_key_namer(test_namer);
        CHECK(nv2a_ovl_glyph_width(nv2a_ovl_glyph_key(0x20), f)
                  > nv2a_ovl_glyph_width(nv2a_ovl_glyph_key(0x70), f),
              "Space is not wider than F1");
    }
    for (j = 0; j < sizeof k / sizeof k[0]; j++) {
        char what[64];
        const struct nv2a_ovl_list *l = frame_glyphs(k[j].w, k[j].h);
        uint32_t c, glyph_tex = 0;
        CHECK(l && well_formed(l), "glyphs at %u: malformed", k[j].h);
        if (!l)
            continue;
        for (c = 0; c < l->ncmd; c++)
            glyph_tex += l->cmd[c].tex == 0;
        CHECK(glyph_tex >= 1, "glyphs at %u: nothing from the atlas", k[j].h);
        snprintf(what, sizeof what, "glyph list at %u", k[j].h);
        pin(what, l->sum, k[j].list);
        snprintf(what, sizeof what, "glyphs-%u", k[j].h);
        pin(what, raster(what, l, (int)k[j].w, (int)k[j].h), k[j].ref);
    }
    nv2a_ovl_glyph_set_key_namer(NULL);
}

/* The menu packs three text sizes, the icons at those and the input
 * glyphs at small, body and twice body. At 3240 lines (effective scale
 * 3.0: 1080 lines at render scale 3) it must fit 2048x2048, the twice-body
 * input size left out if need be, and every glyph must have been packed. */
static void test_fit(void)
{
    const struct nv2a_ovl_atlas *a;
    struct nv2a_ovl_quad q;
    int i;
    nv2a_overlay_reset_for_test();
    CHECK(nv2a_ovl_font_bake(3240, 1) == 0, "no atlas at 3240 lines");
    a = nv2a_ovl_font_atlas();
    CHECK(a && a->w <= 2048 && a->h <= 2048, "atlas %ux%u at 3240 lines", a ? a->w : 0,
          a ? a->h : 0);
    if (a)
        printf("atlas at 3240 lines: %ux%u\n", a->w, a->h);
    for (i = 0; i < s_nrows; i++)
        CHECK(nv2a_ovl_font_symbol(OVL_FACE_INPUT, s_rows[i], 0, 0, 96, &q)
                  && q.x1 > q.x0 && q.u1 > q.u0, "U+%04X not packed at 3240", s_rows[i]);
    CHECK(nv2a_ovl_font_symbol(OVL_FACE_ICONS, NV2A_OVL_ICON_GAMEPAD, 0, 0, 96, &q),
          "an icon not packed at 3240");
    nv2a_ovl_font_free();

    /* The largest reachable bakes under ui_fit = display: the
     * display's floor and overlay.ui_scale both feed the bake's scale, so
     * a Retina display (floor 2) at ui_scale 1.5 and 2.0, fullscreen or
     * in a small window, and a 4K desktop at 150% asks for
     * OVL_UI_SCALE_MAX. Each must bake there and fit with every symbol. */
    {
        static const struct {
            struct nv2a_ovl_display d;
            float mul;
            uint32_t out_h;
        } k_max[] = {
            { { 2560, 1600, 2.0f, 0, 0 }, 1.5f, 1600 },
            { { 2560, 1600, 2.0f, 0, 0 }, 2.0f, 1600 },
            { { 3456, 2234, 2.0f, 0, 0 }, 2.0f, 1200 },
            { { 3840, 2160, 1.5f, 0, 0 }, 2.0f, 2160 },
        };
        for (i = 0; i < (int)(sizeof k_max / sizeof k_max[0]); i++) {
            int j;
            nv2a_overlay_reset_for_test();
            nv2a_overlay_set_display(&k_max[i].d);
            nv2a_overlay_set_ui_scale(k_max[i].mul);
            CHECK(nv2a_ovl_ui_scale(nv2a_ovl_bake_lines(k_max[i].out_h)) == OVL_UI_SCALE_MAX,
                  "case %d does not reach the max: %.3f", i,
                  nv2a_ovl_ui_scale(nv2a_ovl_bake_lines(k_max[i].out_h)));
            CHECK(nv2a_ovl_font_bake(k_max[i].out_h, 1) == 0, "no atlas in max case %d", i);
            a = nv2a_ovl_font_atlas();
            CHECK(a && a->w <= 2048 && a->h <= 2048 && nv2a_ovl_font_symbols(),
                  "max case %d: atlas %ux%u", i, a ? a->w : 0, a ? a->h : 0);
            for (j = 0; j < s_nrows; j++)
                CHECK(nv2a_ovl_font_symbol(OVL_FACE_INPUT, s_rows[j], 0, 0, 96, &q)
                          && q.x1 > q.x0 && q.u1 > q.u0, "U+%04X not packed in max case %d",
                      s_rows[j], i);
            CHECK(nv2a_ovl_font_symbol(OVL_FACE_ICONS, NV2A_OVL_ICON_GAMEPAD, 0, 0, 96, &q),
                  "an icon not packed in max case %d", i);
            nv2a_ovl_font_free();
        }
    }

    /* The strip and the toasts alone keep the text-only atlas of before
     * the symbol faces, and draw no symbol. */
    nv2a_overlay_reset_for_test();
    CHECK(nv2a_overlay_arm(1u << NV2A_OVL_STATS | 1u << NV2A_OVL_TOAST, 1080) == 0, "arm");
    a = nv2a_ovl_font_atlas();
    CHECK(a && a->w == 512 && a->h == 1024, "stats-only atlas %ux%u", a ? a->w : 0,
          a ? a->h : 0);
    CHECK(!nv2a_ovl_font_symbols()
              && !nv2a_ovl_font_has_symbol(OVL_FACE_ICONS, NV2A_OVL_ICON_GAMEPAD)
              && !nv2a_ovl_font_symbol(OVL_FACE_ICONS, NV2A_OVL_ICON_GAMEPAD, 0, 0, 22, &q),
          "symbols in a stats-only atlas");
    /* The menu armed next, before anything drew, bakes them in. */
    CHECK(nv2a_overlay_arm(1u << NV2A_OVL_MENU, 1080) == 0, "arm the menu");
    CHECK(nv2a_ovl_font_symbols()
              && nv2a_ovl_font_symbol(OVL_FACE_ICONS, NV2A_OVL_ICON_GAMEPAD, 0, 0, 22, &q),
          "no symbols once the menu armed");
    nv2a_overlay_reset_for_test();
}


static void test_stats(void)
{
    struct nv2a_stats_snap s;
    int i;
    nv2a_stats_reset_for_test();
    for (i = 0; i < 10; i++) {
        nv2a_stats_add(NV2A_STAT_PRESENTS, 1);
        nv2a_stats_add(NV2A_STAT_TEX_UPLOADS, 3);
        nv2a_stats_flip(100, 1000000000ull + (uint64_t)i * 16666667ull);
    }
    nv2a_stats_read(&s, 1200000000ull);
    for (i = 0; i < NV2A_STAT_COUNT; i++)
        CHECK(s.count[i] == 0, "stat %d counted with the flag clear: %llu", i,
              (unsigned long long)s.count[i]);
    CHECK(s.frames == 0, "frames with the flag clear");

    nv2a_stats_use(NV2A_STATS_TITLE, 1);
    for (i = 0; i < 61; i++) {
        nv2a_stats_add(NV2A_STAT_PRESENTS, 1);
        nv2a_stats_flip(100, 2000000000ull + (uint64_t)i * 16666667ull);
    }
    nv2a_stats_read(&s, 2000000000ull + 60ull * 16666667ull);
    CHECK(s.count[NV2A_STAT_FLIPS] == 61 && s.count[NV2A_STAT_DRAWS] == 6100
              && s.count[NV2A_STAT_PRESENTS] == 61, "counts %llu %llu %llu",
          (unsigned long long)s.count[NV2A_STAT_FLIPS], (unsigned long long)s.count[NV2A_STAT_DRAWS],
          (unsigned long long)s.count[NV2A_STAT_PRESENTS]);
    CHECK(s.frames == 60, "%u frames in the last second", s.frames);
    CHECK(s.ms_avg > 16.6f && s.ms_avg < 16.7f && s.ms_max < 16.7f, "%.3f ms (max %.3f)",
          s.ms_avg, s.ms_max);
    CHECK(s.pack_hits == 7 && s.pack_misses == 7, "pack %llu/%llu",
          (unsigned long long)s.pack_hits, (unsigned long long)s.pack_misses);
    nv2a_stats_use(NV2A_STATS_TITLE, 0);
    CHECK(!nv2a_stats_enabled(), "flag left on");
}

/* overlay.ui_scale (D2): the multiplier over the output's factor, clamped
 * twice; the outer clamp holds 4K at 2.0 to the atlas's 3.0 and 480 lines
 * at 0.75 to 0.5. */
static void test_ui_scale(void)
{
    CHECK(nv2a_ovl_ui_scale(1080) == 1.0f && nv2a_ovl_ui_scale(2160) == 2.0f, "stock scale");
    CHECK(nv2a_overlay_set_ui_scale(2.0f) == 2.0f, "2.0 kept");
    CHECK(nv2a_ovl_ui_scale(2160) == 3.0f, "3840x2160 at 2.0: %.3f", nv2a_ovl_ui_scale(2160));
    CHECK(nv2a_ovl_ui_scale(1080) == 2.0f, "1080 at 2.0");
    CHECK(nv2a_overlay_set_ui_scale(0.75f) == 0.75f, "0.75 kept");
    CHECK(nv2a_ovl_ui_scale(480) == 0.5f, "640x480 at 0.75: %.3f", nv2a_ovl_ui_scale(480));
    CHECK(nv2a_ovl_ui_scale(1080) == 0.75f, "1080 at 0.75");
    CHECK(nv2a_overlay_set_ui_scale(5.0f) == 2.0f && nv2a_overlay_set_ui_scale(0.1f) == 0.75f,
          "the key's range");
    CHECK(nv2a_overlay_set_ui_scale(1.0f) == 1.0f && nv2a_ovl_ui_scale(1080) == 1.0f,
          "back to stock");
}

/* The window-to-output view (present.surface = internal): the identity by
 * default, Metal's aspect fit (letterboxed or pillarboxed), D3D11's
 * stretch of the back buffer to the client area. A window pixel maps to
 * the output pixel under its centre; one past an edge lands outside. */
static void test_view(void)
{
    int x, y, r[4];
    x = 1234, y = -5;
    nv2a_overlay_to_output(&x, &y);
    CHECK(x == 1234 && y == -5, "identity moved to %d,%d", x, y);
    nv2a_overlay_to_window(&x, &y);
    CHECK(x == 1234 && y == -5, "identity back to %d,%d", x, y);

    /* 640x480 in a 1920x1200 window: bars left and right. */
    nv2a_overlay_view_fit(1920, 1200, 640, 480, r);
    CHECK(r[0] == 160 && r[1] == 0 && r[2] == 1600 && r[3] == 1200, "fit %d,%d %dx%d", r[0],
          r[1], r[2], r[3]);
    nv2a_overlay_set_view(r[0], r[1], r[2], r[3], 640, 480);
    {
        static const int k[][4] = {
            { 160, 0, 0, 0 },       { 159, -1, -1, -1 },     { 960, 600, 320, 240 },
            { 1759, 1199, 639, 479 }, { 1760, 1200, 640, 480 }, { 0, 0, -64, 0 },
        };
        for (int i = 0; i < 6; i++) {
            x = k[i][0], y = k[i][1];
            nv2a_overlay_to_output(&x, &y);
            CHECK(x == k[i][2] && y == k[i][3], "letterbox %d,%d -> %d,%d", k[i][0], k[i][1], x,
                  y);
        }
    }
    x = 320, y = 240;
    nv2a_overlay_to_window(&x, &y);
    CHECK(x == 960 && y == 600, "letterbox back %d,%d", x, y);
    x = 0, y = 0;
    nv2a_overlay_to_window(&x, &y);
    CHECK(x == 160 && y == 0, "letterbox origin %d,%d", x, y);

    /* The other way: bars top and bottom, the height rounded. */
    nv2a_overlay_view_fit(1000, 1000, 640, 480, r);
    CHECK(r[0] == 0 && r[1] == 125 && r[2] == 1000 && r[3] == 750, "pillar fit %d,%d %dx%d", r[0],
          r[1], r[2], r[3]);
    nv2a_overlay_view_fit(1001, 1000, 640, 480, r);
    CHECK(r[2] == 1001 && r[3] == 751 && r[1] == 124, "rounded fit %d,%d %dx%d", r[0], r[1],
          r[2], r[3]);
    nv2a_overlay_view_fit(0, 1000, 640, 480, r);
    CHECK(r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 1000, "no window");

    /* Stretched: 640x480 over a 1280x720 client, 2x across, 1.5x down. */
    nv2a_overlay_set_view(0, 0, 1280, 720, 640, 480);
    x = 640, y = 360;
    nv2a_overlay_to_output(&x, &y);
    CHECK(x == 320 && y == 240, "stretch centre %d,%d", x, y);
    x = 1279, y = 719;
    nv2a_overlay_to_output(&x, &y);
    CHECK(x == 639 && y == 479, "stretch corner %d,%d", x, y);
    x = 1, y = 1;
    nv2a_overlay_to_output(&x, &y);
    CHECK(x == 0 && y == 1, "stretch 1,1 -> %d,%d", x, y);   /* centres 0.75 and 1.0 */
    x = 2, y = 2;
    nv2a_overlay_to_output(&x, &y);
    CHECK(x == 1 && y == 1, "stretch 2,2 -> %d,%d", x, y);
    x = 320, y = 240;
    nv2a_overlay_to_window(&x, &y);
    CHECK(x == 640 && y == 360, "stretch back %d,%d", x, y);

    /* A size of 0 is the identity again (a minimised window). */
    nv2a_overlay_set_view(0, 0, 0, 720, 640, 480);
    x = 77, y = 88;
    nv2a_overlay_to_output(&x, &y);
    CHECK(x == 77 && y == 88, "zero view moved to %d,%d", x, y);
    nv2a_overlay_set_view(0, 0, 1280, 720, 640, 480);
    nv2a_overlay_reset_for_test();
    x = 77, y = 88;
    nv2a_overlay_to_output(&x, &y);
    CHECK(x == 77 && y == 88, "reset kept the view: %d,%d", x, y);
}

/* The display's floor: the Deck's 1280x800 7" panel at 216 ppi
 * grows the menu by 1.35 (held to 800 / 600); docked on a 1080p TV, or
 * with SteamDeck unset, it is a desktop again. The OS's scale lifts a
 * desktop; a small window keeps its 600 lines; under the internal surface
 * the floor is in the output's pixels; ui_fit = output is the old rule. */
static int deck_probe(struct nv2a_ovl_display *d)
{
    d->w = 1280, d->h = 800, d->scale = 1.0f;
    nv2a_overlay_display_mark_deck(d, "1");
    return 1;
}

static int near(float a, float b) { return a > b - 0.002f && a < b + 0.002f; }

static void test_display_scale(void)
{
    struct nv2a_ovl_display d = { 1280, 800, 1.0f, 0, 0 };
    nv2a_overlay_reset_for_test();
    CHECK(nv2a_ovl_display_floor(NULL) == 0 && nv2a_ovl_display_floor(&d) == 1.0f,
          "desktop floor %.3f", nv2a_ovl_display_floor(&d));
    nv2a_overlay_display_mark_deck(&d, NULL);
    CHECK(!d.handheld, "SteamDeck unset is not a handheld");
    nv2a_overlay_display_mark_deck(&d, "0");
    CHECK(!d.handheld, "SteamDeck=0 is not a handheld");
    nv2a_overlay_display_mark_deck(&d, "1");
    CHECK(d.handheld && d.diag_in == 7.0f, "the Deck's panel");
    CHECK(near(nv2a_ovl_display_floor(&d), 1.348f), "Deck floor %.3f",
          nv2a_ovl_display_floor(&d));
    CHECK(nv2a_overlay_is_deck_board("Valve\n", "Jupiter\n") &&
              nv2a_overlay_is_deck_board("Valve", "Galileo"),
          "the Deck's SMBIOS names, LCD and OLED");
    CHECK(!nv2a_overlay_is_deck_board("Valve", "Index") &&
              !nv2a_overlay_is_deck_board("ValveX", "Jupiter") &&
              !nv2a_overlay_is_deck_board("Valve", "Jupiter2") &&
              !nv2a_overlay_is_deck_board("", "") && !nv2a_overlay_is_deck_board(NULL, "Jupiter"),
          "other boards are not a Deck");
    {
        struct nv2a_ovl_display tv = { 1920, 1080, 1.0f, 0, 0 };
        nv2a_overlay_display_mark_deck(&tv, "1");
        CHECK(!tv.handheld, "a docked Deck is a desktop");
    }

    /* Nothing known: the stock rule. */
    CHECK(nv2a_ovl_ui_scale(800) == 800.0f / 1080.0f, "no display at 800: %.3f",
          nv2a_ovl_ui_scale(800));
    CHECK(nv2a_ovl_bake_lines(1080) == 1080, "no display bakes for 1080");

    /* The Deck: 1.35 held to 800 / 600, baked for its 800 lines. */
    nv2a_overlay_set_display(&d);
    CHECK(near(nv2a_ovl_ui_scale(800), 1.333f), "Deck at 800: %.3f", nv2a_ovl_ui_scale(800));
    CHECK(nv2a_ovl_bake_lines(1080) == 800, "Deck bakes for 800: %u", nv2a_ovl_bake_lines(1080));
    CHECK(nv2a_overlay_set_ui_scale(0.75f) == 0.75f && near(nv2a_ovl_ui_scale(800), 1.0f),
          "Deck at 0.75: %.3f", nv2a_ovl_ui_scale(800));
    nv2a_overlay_set_ui_scale(1.0f);
    /* A 640x480 output stretched to the panel: 0.6 output pixels per
     * window pixel, the floor 0.81 held to 480 / 600. */
    nv2a_overlay_set_view(0, 0, 1280, 800, 640, 480);
    CHECK(near(nv2a_ovl_ui_scale(480), 0.8f), "Deck internal 480: %.3f", nv2a_ovl_ui_scale(480));
    nv2a_overlay_set_view(0, 0, 0, 0, 0, 0);

    /* ui_fit = output: the display is ignored. */
    nv2a_overlay_set_ui_fit(0);
    CHECK(nv2a_ovl_ui_scale(800) == 800.0f / 1080.0f, "output fit at 800: %.3f",
          nv2a_ovl_ui_scale(800));
    CHECK(nv2a_ovl_bake_lines(1080) == 1080, "output fit bakes for 1080");
    nv2a_overlay_set_ui_fit(1);

    /* A desktop at 150%: 1.5 at 1080, the base at 2160; a 480-line window
     * keeps 600 token lines. */
    {
        struct nv2a_ovl_display desk = { 2560, 1440, 1.5f, 0, 0 };
        nv2a_overlay_set_display(&desk);
        CHECK(near(nv2a_ovl_ui_scale(1080), 1.5f), "150%% at 1080: %.3f",
              nv2a_ovl_ui_scale(1080));
        CHECK(nv2a_ovl_ui_scale(2160) == 2.0f, "150%% at 2160: %.3f", nv2a_ovl_ui_scale(2160));
        CHECK(near(nv2a_ovl_ui_scale(480), 0.8f), "150%% at 480: %.3f", nv2a_ovl_ui_scale(480));
        desk.scale = 1.0f;
        nv2a_overlay_set_display(&desk);
        CHECK(near(nv2a_ovl_ui_scale(480), 0.8f), "100%% at 480: %.3f", nv2a_ovl_ui_scale(480));
        CHECK(nv2a_ovl_ui_scale(1080) == 1.0f, "100%% at 1080");
    }

    /* The probe runs once, at the first bake's lines. */
    nv2a_overlay_set_display_probe(deck_probe);
    CHECK(nv2a_ovl_bake_lines(1080) == 800 && near(nv2a_ovl_ui_scale(800), 1.333f),
          "probed Deck");
    nv2a_overlay_reset_for_test();
    CHECK(nv2a_ovl_bake_lines(1080) == 1080, "reset dropped the display");

    /* A pad glyph in small text is drawn at the body size; an icon keeps
     * the line's. */
    {
        const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
        mu_Font sm = OVL_FONT(OVL_FONT_SMALL, 17, 0), body = OVL_FONT(OVL_FONT_BODY, 22, 0);
        nv2a_ovl_glyph a = nv2a_ovl_glyph_src(2, 1);
        int want = (nv2a_ovl_text_height(sm) * t->font_body + t->font_small / 2) / t->font_small;
        CHECK(a != 0, "a pad glyph");
        CHECK(nv2a_ovl_glyph_box(a, sm) == want && want > nv2a_ovl_text_height(sm),
              "small pad glyph %d, want %d", nv2a_ovl_glyph_box(a, sm), want);
        CHECK(nv2a_ovl_glyph_width(a, sm) == want, "its width");
        CHECK(nv2a_ovl_glyph_box(a, body) == nv2a_ovl_text_height(body), "body pad glyph");
        CHECK(nv2a_ovl_glyph_box(NV2A_OVL_GLYPH(NV2A_OVL_GLYPH_ICON, 0xe000), sm) == nv2a_ovl_text_height(sm),
              "a small icon");
        /* The menu's footer buttons are the small line plus 10 token px
         * (enhance_menu_layout.c), and the taller glyph is centred on the
         * line: it stays inside at every scale. */
        for (int q = 2; q <= 12; q++) {
            float sc = (float)q / 4.0f;
            int px = (int)((float)t->font_small * sc + 0.5f), btn = px + (int)(10.0f * sc + 0.5f);
            int gh = nv2a_ovl_glyph_box(a, OVL_FONT(OVL_FONT_SMALL, px, 0));
            CHECK(gh <= btn, "footer glyph at %.2f: %d in %d", (double)sc, gh, btn);
        }
    }
}

/* A probe that is not ready at the first arm (SDL before the presenter's
 * SDL_Init) leaves the stock bake; display_ready probes and bakes again
 * while nothing has drawn, and only once. */
static int s_ready_calls, s_ready_up;
static int late_probe(struct nv2a_ovl_display *d)
{
    s_ready_calls++;
    if (!s_ready_up)
        return -1;
    return deck_probe(d);
}

static void test_display_ready(void)
{
    const struct nv2a_ovl_atlas *a;
    uint32_t gen;
    nv2a_overlay_reset_for_test();
    s_ready_calls = s_ready_up = 0;
    nv2a_overlay_set_display_probe(late_probe);
    CHECK(nv2a_overlay_display_ready() == 0, "ready before any arm bakes nothing");
    s_ready_calls = 0;
    CHECK(nv2a_overlay_arm(1u << NV2A_OVL_STATS, 1080) == 0, "arm");
    a = nv2a_overlay_atlas();
    gen = a ? a->gen : 0;
    CHECK(s_ready_calls == 1 && nv2a_ovl_bake_lines(1080) == 1080, "not ready: the stock bake");
    s_ready_up = 1;
    CHECK(nv2a_overlay_display_ready() == 1, "ready re-bakes");
    a = nv2a_overlay_atlas();
    CHECK(a && a->gen != gen && nv2a_ovl_bake_lines(1080) == 800, "baked for the Deck's 800 lines");
    {
        /* The arm's, the stock-bake check's (each bake asks while it is
         * not ready) and display_ready's: none after it answered. */
        int r = nv2a_overlay_display_ready();
        CHECK(r == 0 && s_ready_calls == 3, "probed once it answered: %d, %d calls", r, s_ready_calls);
    }
    nv2a_overlay_reset_for_test();
}

/* The menu's size on screen follows the display, never the frame.
 * The Deck under gamescope (present.surface=internal): at render.scale 1
 * the 640x480 window is fitted to the 1280x800 screen by gamescope; at
 * render.scale 3 the 1920x1440 back buffer is stretched into a window held
 * to the screen (1067x800); with present.shader the window is the
 * screen's size. All three show the same size, and a touch at the held
 * window's far corner still lands in the back buffer. */
static void test_shown_size(void)
{
    struct nv2a_ovl_display deck = { 1280, 800, 1.0f, 0, 0 };
    float a, b, c;
    int x, y;
    nv2a_overlay_reset_for_test();
    nv2a_overlay_display_mark_deck(&deck, "1");
    nv2a_overlay_set_display(&deck);

    CHECK(nv2a_ovl_compositor_fit(0, 0, 1280, 800) == 1.0f, "no compositor: 1");
    CHECK(near(nv2a_ovl_compositor_fit(640, 480, 1280, 800), 1.6667f), "640x480 fitted: %.3f",
          nv2a_ovl_compositor_fit(640, 480, 1280, 800));

    for (int q = 0; q < 3; q++) {
        static const float mul[] = { 1.0f, 0.75f, 2.0f };
        nv2a_overlay_set_ui_scale(mul[q]);
        nv2a_overlay_set_view(0, 0, 0, 0, 0, 0);
        nv2a_overlay_set_compositor(640, 480);
        a = nv2a_ovl_ui_scale(480) * 800.0f / 480.0f;
        nv2a_overlay_set_view(0, 0, 1067, 800, 1920, 1440);
        nv2a_overlay_set_compositor(1067, 800);
        b = nv2a_ovl_ui_scale(1440) * 800.0f / 1440.0f;
        nv2a_overlay_set_view(0, 0, 0, 0, 0, 0);
        nv2a_overlay_set_compositor(1280, 800);
        c = nv2a_ovl_ui_scale(800);
        CHECK(fabsf(a - c) < 0.01f * c && fabsf(b - c) < 0.01f * c,
              "ui %.2f shown at rs1 %.3f, rs3 %.3f, shader %.3f", (double)mul[q], (double)a,
              (double)b, (double)c);
    }
    nv2a_overlay_set_ui_scale(1.0f);
    c = nv2a_ovl_ui_scale(800);
    CHECK(near(c, 1.333f), "the Deck's own size: %.3f", (double)c);

    /* The held window's pointer: its far corner is the back buffer's. */
    nv2a_overlay_set_view(0, 0, 1067, 800, 1920, 1440);
    x = 1066, y = 799;
    nv2a_overlay_to_output(&x, &y);
    CHECK(x >= 1915 && x < 1920 && y >= 1435 && y < 1440, "far corner at %d,%d", x, y);
    x = 533, y = 400;
    nv2a_overlay_to_output(&x, &y);
    CHECK(abs(x - 959) <= 2 && abs(y - 720) <= 2, "centre at %d,%d", x, y);
    nv2a_overlay_reset_for_test();
}

/* overlay.ui_scale live: the poll bakes again once the menu's
 * lock is free, the strip composes again from the new atlas and a list
 * composed before it is not drawn. */
static void test_rebake(void)
{
    const struct nv2a_ovl_list *ls[NV2A_OVL_COUNT];
    const struct nv2a_ovl_atlas *at;
    mu_Font body = OVL_FONT(OVL_FONT_BODY, 0, 0);
    uint64_t g0, g1;
    int px0, n, menu = 0, stats = 0;
    mu_Context *ctx;
    nv2a_overlay_reset_for_test();
    CHECK(nv2a_overlay_arm(1u << NV2A_OVL_STATS | 1u << NV2A_OVL_MENU, 1080) == 0, "arm");
    nv2a_overlay_set_stats(1);
    nv2a_overlay_set_placement(0, 0, 1280, 720);
    nv2a_overlay_poll(1);
    ctx = nv2a_overlay_ui_begin(NV2A_OVL_MENU);
    nv2a_ovl_compose_stats(ctx, 1280, 720, k_line, 0);
    nv2a_overlay_ui_publish(NV2A_OVL_MENU);
    at = nv2a_overlay_atlas();
    g0 = at ? at->gen : 0;
    px0 = nv2a_ovl_text_height(body);
    CHECK(g0 && nv2a_overlay_atlas_gen() == g0 && nv2a_overlay_lists(ls) == 2, "two lists");

    nv2a_overlay_set_ui_scale(2.0f);
    nv2a_overlay_rebake();
    CHECK(nv2a_overlay_menu_trylock(), "lock");
    nv2a_overlay_poll(2);
    CHECK(nv2a_overlay_atlas_gen() == g0, "no bake under the menu's lock");
    nv2a_overlay_menu_unlock();
    nv2a_overlay_poll(3);
    CHECK(nv2a_overlay_atlas_gen() != g0 && nv2a_ovl_font_symbols(), "baked again, symbols kept");
    CHECK(nv2a_ovl_text_height(body) >= 2 * px0 - 1, "body %d px, was %d",
          nv2a_ovl_text_height(body), px0);
    n = nv2a_overlay_lists(ls);
    for (int i = 0; i < n; i++) {
        menu |= ls[i]->layer == NV2A_OVL_MENU;
        stats |= ls[i]->layer == NV2A_OVL_STATS && ls[i]->atlas_gen == nv2a_overlay_atlas_gen();
    }
    CHECK(!menu && stats, "the old menu list left out (%d), the strip composed again (%d)", menu,
          stats);
    g1 = nv2a_overlay_atlas_gen();
    nv2a_overlay_poll(4);
    CHECK(nv2a_overlay_atlas_gen() == g1, "one bake per ask");
    nv2a_overlay_set_ui_scale(1.0f);
    nv2a_overlay_reset_for_test();
}

int main(void)
{
    s_ref_out = getenv("OVERLAY_REF_OUT");
    test_disarmed();
    test_frames();
    test_layers();
    test_threads();
    test_toasts();
    test_lock();
    test_cut();
    test_stats();
    test_glyphs();
    test_fit();
    test_ui_scale();
    test_view();
    test_display_scale();
    test_display_ready();
    test_shown_size();
    test_rebake();
    nv2a_overlay_reset_for_test();
    if (fails) {
        printf("overlay: %d failure(s)\n", fails);
        return 1;
    }
    printf("overlay: ok\n");
    return 0;
}
