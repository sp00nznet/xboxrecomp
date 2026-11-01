/* The menu's geometry (enhance_menu_layout.h) at every D2 output size and
 * ui scale: the panel in each size mode, the toolbar folded and unfolded
 * over rows that do not move, the hit test from each row's centre back to
 * the row, clipped rows out of it, and the dialog inside the panel. */
#include <stdio.h>
#include <string.h>
#include "enhance_menu_layout.h"

static int s_fail;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d (%dx%d ui %.2f %s): %s\n", __LINE__, \
                                         g_w, g_h, g_u, g_mode ? "expanded" : "compact", #x); \
                                  s_fail++; } } while (0)

static int g_w, g_h, g_mode;
static float g_u;

static int within(struct menu_rect in, struct menu_rect out)
{
    return in.x >= out.x && in.y >= out.y && in.x + in.w <= out.x + out.w
        && in.y + in.h <= out.y + out.h;
}

static int overlap(struct menu_rect a, struct menu_rect b)
{
    return a.w > 0 && b.w > 0 && a.h > 0 && b.h > 0 && a.x < b.x + b.w && b.x < a.x + a.w
        && a.y < b.y + b.h && b.y < a.y + a.h;
}

/* clamp(clamp(h / 1080, 0.5, 3) * ui, 0.5, 3): nv2a_ovl_ui_scale's rule. */
static float eff_scale(int h, float ui)
{
    float s = (float)h / 1080.0f;
    s = s < 0.5f ? 0.5f : s > 3.0f ? 3.0f : s;
    s *= ui;
    return s < 0.5f ? 0.5f : s > 3.0f ? 3.0f : s;
}

#define NROW 40
static unsigned char s_kind[NROW];
static int s_vw[NROW];

static void input(struct menu_layout_in *in, int w, int h, float s, int mode, int open)
{
    memset(in, 0, sizeof *in);
    in->w = w;
    in->h = h;
    in->s = s;
    in->tok.pad = 16;
    in->tok.gap = 8;
    in->tok.row_h = 44;
    in->tok.font_body = 22;
    in->size = mode;
    in->toolbar_open = open;
    in->intro = 1;
    in->title_h = menu_layout_px(s, 36);
    in->body_h = menu_layout_px(s, 27);
    in->small_h = menu_layout_px(s, 21);
    in->label_w = menu_layout_px(s, 96);
    in->icon_w = menu_layout_px(s, 24);
    in->arrow_w = menu_layout_px(s, 10);
    in->nsec = 9;
    in->sec_sel = 2;
    in->nrow = NROW;
    in->sel = 0;
    in->row_kind = s_kind;
    in->row_value_w = s_vw;
    in->cancel_w = menu_layout_px(s, 90);
    in->save_w = menu_layout_px(s, 70);
    in->summary_w = menu_layout_px(s, 80);
    in->cancel_bare_w = menu_layout_px(s, 60);
    in->save_bare_w = menu_layout_px(s, 40);
}

/* The footer at any size: its buttons inside the panel's pads, apart, the
 * summary and the hints clear of them; with labels wide enough, each step
 * of the give-way (summary, glyph, shrink) shows. */
static void footer(int w, int h, float ui, int mode, int wide)
{
    struct menu_layout_in in;
    struct menu_layout l;
    struct menu_rect P;
    float s = eff_scale(h, ui);
    g_w = w, g_h = h, g_u = ui, g_mode = mode;
    input(&in, w, h, s, mode, 0);
    in.cancel_w *= wide;
    in.save_w *= wide;
    in.summary_w *= wide;
    menu_layout_compute(&in, &l);
    P = l.panel;
    for (int b = 0; b < MENU_FB_COUNT; b++) {
        CHECK(l.fbtn[b].x >= P.x + l.pad && l.fbtn[b].x + l.fbtn[b].w <= P.x + P.w - l.pad);
        CHECK(within(l.fbtn[b], l.footer) && l.fbtn[b].w >= 0);
        CHECK(!overlap(l.summary, l.fbtn[b]) && !overlap(l.hints, l.fbtn[b]));
    }
    CHECK(!overlap(l.fbtn[0], l.fbtn[1]) && !overlap(l.hints, l.summary));
    CHECK(l.hints.w >= 0 && l.summary.w >= 0 && l.hints.x == P.x + l.pad);
    /* The steps run in order: no glyph while a summary still shows. */
    if (l.summary.w > 0)
        CHECK(l.foot_glyph);
    if (l.foot_glyph)
        CHECK(l.fbtn[MENU_FB_CANCEL].w == in.cancel_w + 2 * l.fbtn_pad);
}

/* A short bar: entries no shorter than an icon and a little air, their
 * icons apart, the ones that do not fit zero, the shown one always there. */
static void toolbar(int w, int h, float ui, int mode, int open, int sel)
{
    struct menu_layout_in in;
    struct menu_layout l;
    struct menu_hit ht;
    float s = eff_scale(h, ui);
    int shown = 0;
    g_w = w, g_h = h, g_u = ui, g_mode = mode;
    input(&in, w, h, s, mode, open);
    in.sec_sel = sel;
    menu_layout_compute(&in, &l);
    CHECK(l.sec[sel].h > 0);
    for (int i = 0; i < l.nsec; i++) {
        if (!l.sec[i].h) {
            CHECK(!l.sec_icon[i].w && !l.sec_label[i].w);
            continue;
        }
        shown++;
        CHECK(l.sec[i].h >= in.icon_w + l.pad / 2);
        CHECK(within(l.sec[i], l.bar) && within(l.sec_icon[i], l.sec[i]));
        for (int j = 0; j < i; j++)
            CHECK(!overlap(l.sec_icon[i], l.sec_icon[j]) && !overlap(l.sec[i], l.sec[j]));
        ht = menu_hit(&l, l.sec[i].x + l.sec[i].w / 2, l.sec[i].y + l.sec[i].h / 2);
        CHECK(ht.region == MENU_HIT_TOOLBAR && ht.index == i);
    }
    CHECK(shown >= 1);
    if (l.tooltip.w)
        CHECK(l.tooltip.y == l.sec[sel].y);
}

static void one(int w, int h, float ui, int mode)
{
    struct menu_layout_in in;
    struct menu_layout l, lo;
    float s = eff_scale(h, ui);
    struct menu_hit ht;

    g_w = w, g_h = h, g_u = ui, g_mode = mode;
    input(&in, w, h, s, mode, 0);
    menu_layout_compute(&in, &l);
    input(&in, w, h, s, mode, 1);
    menu_layout_compute(&in, &lo);

    /* The panel. */
    CHECK(within(l.panel, l.place));
    if (w < 640 || h > w) {
        CHECK(l.narrow && l.panel.w == w - 2 * l.pad && !l.labels);
    } else if (mode == MENU_SIZE_COMPACT) {
        /* Today's panel: 40% up to 720 px at 1080. */
        int pw = (int)((float)w * 0.40f);
        if (pw > menu_layout_px(s, 720))
            pw = menu_layout_px(s, 720);
        CHECK(l.panel.x == l.pad && l.panel.y == l.pad && l.panel.w == pw
              && l.panel.h == h - 2 * l.pad && !l.backdrop);
    } else {
        int m = menu_layout_px(s, 32);
        CHECK(l.backdrop && l.panel.y == m && l.panel.h == h - 2 * m);
        CHECK(l.panel.w <= h * 16 / 9);
        CHECK(l.panel.x * 2 + l.panel.w >= w - 1 && l.panel.x * 2 + l.panel.w <= w);
        /* From 720 lines on there is room for the unfolded bar's labels. */
        if (h >= 720)
            CHECK(l.labels);
    }

    /* Header, footer and body in order, inside the panel. */
    CHECK(within(l.header, l.panel) && within(l.footer, l.panel));
    CHECK(l.header.y + l.header.h <= l.content.y && l.content.y + l.content.h < l.footer.y);
    for (int b = 0; b < MENU_HB_COUNT; b++) {
        CHECK(within(l.hbtn[b], l.header));
        CHECK(!overlap(l.hbtn[b], l.title));
        ht = menu_hit(&l, l.hbtn[b].x + l.hbtn[b].w / 2, l.hbtn[b].y + l.hbtn[b].h / 2);
        CHECK(ht.region == MENU_HIT_HEADER && ht.index == b);
    }
    for (int b = 0; b < MENU_FB_COUNT; b++) {
        CHECK(within(l.fbtn[b], l.footer));
        ht = menu_hit(&l, l.fbtn[b].x + l.fbtn[b].w / 2, l.fbtn[b].y + l.fbtn[b].h / 2);
        CHECK(ht.region == MENU_HIT_FOOTER && ht.index == b);
    }
    CHECK(!overlap(l.fbtn[0], l.fbtn[1]) && !overlap(l.summary, l.fbtn[0]));
    CHECK(l.fbtn[MENU_FB_SAVE].x + l.fbtn[MENU_FB_SAVE].w == l.panel.x + l.panel.w - l.pad);

    /* Toolbar: folded beside the content, unfolded over it; the rows are
     * where they were. */
    CHECK(!l.open && l.bar.w == l.bar_folded.w);
    CHECK(l.bar_folded.x + l.bar_folded.w <= l.content.x);
    CHECK(lo.open == lo.labels);
    if (lo.labels) {
        CHECK(lo.bar.w == lo.bar_open.w && lo.bar_open.w > lo.bar_folded.w);
        CHECK(overlap(lo.bar_open, lo.content));
        /* The content keeps 60% of the panel. */
        CHECK(lo.bar_open.w * 10 <= lo.panel.w * 4);
        CHECK(lo.tooltip.w == 0);
        for (int i = 0; i < lo.nsec; i++)
            CHECK(lo.sec_label[i].w > 0 && within(lo.sec_label[i], lo.bar_open));
    } else {
        CHECK(lo.bar_open.w == lo.bar_folded.w && lo.tooltip.w > 0);
    }
    CHECK(lo.content.x == l.content.x && lo.content.w == l.content.w && lo.nvrow == l.nvrow);
    for (int i = 0; i < l.nvrow; i++)
        CHECK(!memcmp(&lo.row[i], &l.row[i], sizeof l.row[i]));
    for (int i = 0; i < l.nsec; i++) {
        CHECK(within(l.sec[i], l.bar) && within(l.sec_icon[i], l.sec[i]));
        CHECK(l.sec_label[i].w == 0);
        ht = menu_hit(&l, l.sec[i].x + l.sec[i].w / 2, l.sec[i].y + l.sec[i].h / 2);
        CHECK(ht.region == MENU_HIT_TOOLBAR && ht.index == i);
        if (lo.labels) {
            /* Unfolded, the label's column is the toolbar's, not a row's. */
            struct menu_rect e = lo.sec_label[i];
            ht = menu_hit(&lo, e.x + e.w / 2, e.y + e.h / 2);
            CHECK(ht.region == MENU_HIT_TOOLBAR && ht.index == i);
        }
    }

    /* Rows: inside the viewport and back from their centre; the clipped one
     * is never hit. */
    CHECK(l.nvrow >= l.vis && l.vis >= 1);
    CHECK(within(l.intro, l.content) && l.intro.y + l.intro.h <= l.viewport.y);
    for (int i = 0; i < l.nvrow; i++) {
        const struct menu_layout_row *vr = &l.row[i];
        int cx = vr->r.x + vr->r.w / 2, cy = vr->r.y + vr->r.h / 2;
        CHECK(vr->index == l.scroll + i);
        CHECK(!overlap(vr->r, l.bar_folded));
        if (vr->clipped) {
            CHECK(!within(vr->r, l.viewport));
            if (cy < l.viewport.y + l.viewport.h) {
                ht = menu_hit(&l, cx, cy);
                CHECK(ht.region != MENU_HIT_ROW);
            }
            continue;
        }
        CHECK(within(vr->r, l.viewport));
        ht = menu_hit(&l, cx, cy);
        CHECK(ht.region == MENU_HIT_ROW && ht.index == vr->index);
        if (vr->pill.w) {
            CHECK(within(vr->pill, vr->r));
            ht = menu_hit(&l, vr->pill.x + vr->pill.w / 2, vr->pill.y + vr->pill.h / 2);
            CHECK(ht.part == MENU_PART_PILL && ht.index == vr->index);
        }
        if (vr->track.w) {
            CHECK(within(vr->track, vr->r) && within(vr->value, vr->r));
            ht = menu_hit(&l, vr->track.x + 1, vr->r.y + 1);
            CHECK(ht.part == MENU_PART_TRACK && ht.index == vr->index);
        }
        if (vr->next.w) {
            CHECK(within(vr->prev, vr->r) && vr->prev.x + vr->prev.w <= vr->value.x
                  && vr->value.x + vr->value.w <= vr->next.x);
            ht = menu_hit(&l, vr->prev.x + vr->prev.w / 2, vr->r.y + vr->r.h / 2);
            CHECK(ht.part == MENU_PART_PREV);
            ht = menu_hit(&l, vr->next.x + vr->next.w / 2, vr->r.y + vr->r.h / 2);
            CHECK(ht.part == MENU_PART_NEXT);
        }
    }
    if (l.nvrow && l.row[l.nvrow - 1].clipped)
        CHECK(l.nvrow == l.vis + 1);
    CHECK(l.scrollbar && within(l.sb_thumb, l.sb_track) && within(l.sb_track, l.viewport));

    /* The selection stays in view: the last row selected scrolls to the end. */
    input(&in, w, h, s, mode, 0);
    in.sel = NROW - 1;
    menu_layout_compute(&in, &lo);
    CHECK(lo.scroll == NROW - lo.vis);
    CHECK(lo.sb_thumb.y + lo.sb_thumb.h == lo.sb_track.y + lo.sb_track.h);

    /* A graph under the rows takes its height from them. */
    in.sel = 0;
    in.extra_h = menu_layout_px(s, 110);
    menu_layout_compute(&in, &lo);
    CHECK(within(lo.extra, lo.content) && lo.viewport.y + lo.viewport.h < lo.extra.y);
    CHECK(lo.vis <= l.vis);

    /* The dialog: inside the panel, its buttons inside it, modal. */
    in.extra_h = 0;
    in.dlg_on = 1;
    in.dlg_lines = 3;
    for (int b = 0; b < MENU_LAYOUT_DLG_BTNS; b++)
        in.dlg_btn_w[b] = menu_layout_px(s, 70 + 10 * b);
    menu_layout_compute(&in, &lo);
    CHECK(within(lo.dlg, lo.panel) && lo.dlg.w >= lo.panel.w * 3 / 4);
    CHECK(lo.dlg_line_y + 3 * lo.dlg_line_h <= lo.dlg_btn[0].y);
    for (int b = 0; b < MENU_LAYOUT_DLG_BTNS; b++) {
        /* Unless the labels alone are wider than the panel (480 lines at
         * ui 2 in a 256 px panel). */
        int need = 2 * lo.pad + 2 * lo.gap;
        for (int k = 0; k < MENU_LAYOUT_DLG_BTNS; k++)
            need += in.dlg_btn_w[k];
        if (need > lo.panel.w)
            break;
        CHECK(within(lo.dlg_btn[b], lo.dlg));
        if (b)
            CHECK(!overlap(lo.dlg_btn[b], lo.dlg_btn[b - 1]));
        ht = menu_hit(&lo, lo.dlg_btn[b].x + lo.dlg_btn[b].w / 2,
                      lo.dlg_btn[b].y + lo.dlg_btn[b].h / 2);
        CHECK(ht.region == MENU_HIT_DIALOG && ht.index == b);
    }
    ht = menu_hit(&lo, lo.hbtn[MENU_HB_CLOSE].x + 1, lo.hbtn[MENU_HB_CLOSE].y + 1);
    CHECK(ht.region == MENU_HIT_PANEL);
    ht = menu_hit(&lo, w + 5, 5);
    CHECK(ht.region == MENU_HIT_NONE);
}

int main(void)
{
    static const int k_size[][2] = {{640, 480},   {1280, 720},  {1280, 800}, {1920, 1080},
                                    {2560, 1080}, {3440, 1440}, {3840, 2160}, {800, 1280}};
    static const float k_ui[] = {0.75f, 1.0f, 2.0f};
    struct menu_layout_in in;
    struct menu_layout l;

    for (int i = 0; i < NROW; i++) {
        s_kind[i] = (unsigned char)(i % 6);
        s_vw[i] = 40 + i;
    }
    for (size_t z = 0; z < sizeof k_size / sizeof k_size[0]; z++)
        for (size_t u = 0; u < sizeof k_ui / sizeof k_ui[0]; u++)
            for (int m = 0; m < 2; m++)
                one(k_size[z][0], k_size[z][1], k_ui[u], m);

    /* Review: small outputs at ui 2 and narrow ones with long labels. */
    {
        static const int k_small[][2] = {{640, 360}, {640, 480}, {854, 480},
                                         {1080, 1920}, {400, 2000}};
        for (size_t z = 0; z < sizeof k_small / sizeof k_small[0]; z++)
            for (size_t u = 0; u < sizeof k_ui / sizeof k_ui[0]; u++)
                for (int m = 0; m < 2; m++) {
                    for (int wide = 1; wide <= 4; wide++)
                        footer(k_small[z][0], k_small[z][1], k_ui[u], m, wide);
                    for (int sel = 0; sel < 9; sel++)
                        for (int o = 0; o < 2; o++)
                            toolbar(k_small[z][0], k_small[z][1], k_ui[u], m, o, sel);
                }
    }
    /* 640x360 at ui 2: the summary goes first, then the glyphs; the bar's
     * entries keep their height and scroll to the shown one. */
    g_w = 640, g_h = 360, g_u = 2.0f, g_mode = 0;
    input(&in, 640, 360, eff_scale(360, 2.0f), MENU_SIZE_COMPACT, 1);
    in.cancel_w = menu_layout_px(in.s, 140);
    in.save_w = menu_layout_px(in.s, 110);
    menu_layout_compute(&in, &l);
    CHECK(l.summary.w == 0);
    in.cancel_w = menu_layout_px(in.s, 400);
    menu_layout_compute(&in, &l);
    CHECK(l.summary.w == 0 && !l.foot_glyph);
    CHECK(l.fbtn[MENU_FB_CANCEL].x >= l.panel.x + l.pad);
    in.cancel_bare_w = in.save_bare_w = menu_layout_px(in.s, 400);
    menu_layout_compute(&in, &l);
    CHECK(l.fbtn[MENU_FB_CANCEL].x >= l.panel.x + l.pad
          && l.fbtn[MENU_FB_CANCEL].w == l.fbtn[MENU_FB_SAVE].w);
    g_mode = 1;
    input(&in, 640, 360, eff_scale(360, 2.0f), MENU_SIZE_EXPANDED, 1);
    in.sec_sel = 8;
    menu_layout_compute(&in, &l);
    CHECK(l.sec[0].h == 0 && l.sec[8].h >= in.icon_w + l.pad / 2);
    /* A choice whose value is wider than its row: the prev arrow stops at
     * the row's pad, the value between the arrows. */
    for (int i = 0; i < NROW; i++)
        if (s_kind[i] == MENU_ROW_CHOICE)
            s_vw[i] = 4000;
    input(&in, 640, 360, eff_scale(360, 2.0f), MENU_SIZE_COMPACT, 0);
    menu_layout_compute(&in, &l);
    for (int i = 0; i < l.nvrow; i++) {
        const struct menu_layout_row *vr = &l.row[i];
        if (!vr->next.w)
            continue;
        CHECK(vr->prev.x == vr->r.x + l.pad && vr->value.w >= 0);
        CHECK(vr->prev.x + vr->prev.w <= vr->value.x && vr->value.x + vr->value.w <= vr->next.x);
    }
    for (int i = 0; i < NROW; i++)
        s_vw[i] = 40 + i;

    /* Pinned at 1080, ui 1: the compact panel the menu always had, the bar
     * and the first row. */
    g_w = 1920, g_h = 1080, g_u = 1.0f, g_mode = 0;
    input(&in, 1920, 1080, 1.0f, MENU_SIZE_COMPACT, 0);
    menu_layout_compute(&in, &l);
    CHECK(l.panel.x == 16 && l.panel.y == 16 && l.panel.w == 720 && l.panel.h == 1048);
    CHECK(l.bar_folded.w == 56 && l.content.x == 72 && l.content.w == 656);
    CHECK(l.labels && l.bar_open.w == 56 + 8 * 22);
    CHECK(l.row[0].r.y == l.viewport.y && l.row[0].r.h == 44);
    /* Expanded on the ultrawide: held to 16:9, centred. */
    g_w = 2560, g_mode = 1;
    input(&in, 2560, 1080, 1.0f, MENU_SIZE_EXPANDED, 0);
    menu_layout_compute(&in, &l);
    CHECK(l.panel.w == 1920 - 64 && l.panel.x == (2560 - 1856) / 2);
    /* The floor: 720 lines at ui 0.75 is scale 0.5, a 360 px panel that
     * still has room for the labels. */
    g_w = 1280, g_h = 720, g_u = 0.75f, g_mode = 0;
    input(&in, 1280, 720, eff_scale(720, 0.75f), MENU_SIZE_COMPACT, 1);
    menu_layout_compute(&in, &l);
    CHECK(l.s == 0.5f && l.panel.w == 360 && l.labels && l.open);
    /* 480 lines: under 640 wide is not narrow, but 8 em of labels would
     * take more than 40% of a 256 px panel, so the bar stays icons and the
     * label is a tooltip. */
    g_w = 640, g_h = 480, g_u = 1.0f;
    input(&in, 640, 480, eff_scale(480, 1.0f), MENU_SIZE_COMPACT, 1);
    menu_layout_compute(&in, &l);
    CHECK(!l.labels && !l.open && l.tooltip.w > 0);

    if (s_fail) {
        printf("%d failures\n", s_fail);
        return 1;
    }
    printf("enhance_menu_layout: ok\n");
    return 0;
}
