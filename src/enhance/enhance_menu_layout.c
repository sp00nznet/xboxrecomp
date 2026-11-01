/*
 * enhance_menu_layout.c - the menu's geometry (enhance_menu_layout.h).
 */
#include "enhance_menu_layout.h"

#include <string.h>

int menu_layout_px(float s, int token)
{
    /* lroundf for the positive tokens the theme has, without libm. */
    float f = s * (float)token;
    int v = (int)(f < 0 ? f - 0.5f : f + 0.5f);
    return token && v < 1 ? 1 : v;
}

static struct menu_rect rect(int x, int y, int w, int h)
{
    struct menu_rect r = {x, y, w > 0 ? w : 0, h > 0 ? h : 0};
    return r;
}

static int inside(struct menu_rect r, int x, int y)
{
    return r.w > 0 && r.h > 0 && x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* R8 and D1: compact is a panel on the left, 40% of the width up to 720 px
 * at 1080 (the panel the menu always had); expanded is centred with a
 * margin, its width held to a 16:9 area so an ultrawide output does not
 * stretch the rows across the screen. A narrow or portrait output gets
 * the whole width in both. */
static void panel_of(const struct menu_layout_in *in, struct menu_layout *l)
{
    int w = in->w, h = in->h;
    l->narrow = w < 640 || h > w;
    l->backdrop = 0;
    if (l->narrow) {
        l->panel = rect(l->pad, l->pad, w - 2 * l->pad, h - 2 * l->pad);
    } else if (in->size == MENU_SIZE_EXPANDED) {
        int m = menu_layout_px(l->s, 32), pw = w - 2 * m, cap = h * 16 / 9 - 2 * m;
        if (pw > cap)
            pw = cap;
        l->panel = rect((w - pw) / 2, m, pw, h - 2 * m);
        l->backdrop = 1;
    } else {
        int pw = (int)((float)w * 0.40f);
        if (pw > menu_layout_px(l->s, 720))
            pw = menu_layout_px(l->s, 720);
        l->panel = rect(l->pad, l->pad, pw, h - 2 * l->pad);
    }
}

static void row_controls(const struct menu_layout_in *in, struct menu_layout *l,
                         struct menu_layout_row *vr)
{
    struct menu_rect r = vr->r;
    int kind = in->row_kind ? in->row_kind[vr->index] : MENU_ROW_PLAIN;
    int vw = in->row_value_w ? in->row_value_w[vr->index] : 0;
    int right = r.x + r.w - l->pad, cy = r.y + r.h / 2;
    float s = l->s;
    if (kind == MENU_ROW_TOGGLE) {
        int pw = menu_layout_px(s, 44), ph = menu_layout_px(s, 24);
        vr->pill = rect(right - pw, r.y + (r.h - ph) / 2, pw, ph);
    } else if (kind == MENU_ROW_RANGE) {
        int tw = menu_layout_px(s, 160), tk = menu_layout_px(s, 4);
        vr->track = rect(right - tw, cy - tk / 2, tw, tk);
        vr->value = rect(vr->track.x - menu_layout_px(s, 12) - vw, r.y, vw, r.h);
        /* A long value on a cramped row stops at the row's pad. */
        if (vr->value.x < r.x + l->pad) {
            vr->value.w -= r.x + l->pad - vr->value.x;
            vr->value.x = r.x + l->pad;
        }
    } else if (kind == MENU_ROW_CHOICE) {
        int aw = in->arrow_w, sp = menu_layout_px(s, 10);
        vr->next = rect(right - aw, r.y, aw, r.h);
        vr->value = rect(vr->next.x - sp - vw, r.y, vw, r.h);
        vr->prev = rect(vr->value.x - sp - aw, r.y, aw, r.h);
        /* As for a range: the prev arrow stops at the row's pad and the
         * value takes what is left between the arrows. */
        if (vr->prev.x < r.x + l->pad) {
            vr->prev.x = r.x + l->pad;
            vr->value.x = vr->prev.x + aw + sp;
            vr->value.w = vr->next.x - sp - vr->value.x;
            if (vr->value.w < 0)
                vr->value.w = 0;
        }
    }
}

void menu_layout_compute(const struct menu_layout_in *in, struct menu_layout *l)
{
    float s = in->s;
    struct menu_rect P;
    int bsz, bx, body_y, body_b, tbf, tbo, eh, em, bpad, btn_h, vy, vh, step;

    memset(l, 0, sizeof *l);
    l->s = s;
    l->pad = menu_layout_px(s, in->tok.pad);
    l->gap = menu_layout_px(s, in->tok.gap);
    l->rowh = menu_layout_px(s, in->tok.row_h);
    l->rgap = menu_layout_px(s, 4);
    l->place = rect(0, 0, in->w, in->h);
    panel_of(in, l);
    P = l->panel;

    /* Header: the title at the pad, its buttons square to its line at the
     * right. */
    l->header = rect(P.x, P.y, P.w, l->pad + in->title_h + l->gap);
    bsz = in->title_h;
    bx = P.x + P.w - l->pad;
    for (int b = MENU_HB_COUNT - 1; b >= 0; b--) {
        bx -= bsz;
        l->hbtn[b] = rect(bx, P.y + l->pad, bsz, bsz);
        bx -= l->gap;
    }
    l->title = rect(P.x + l->pad, P.y + l->pad, bx - (P.x + l->pad), in->title_h);

    /* Footer (D9): hints | summary | Cancel Save, the buttons tall enough
     * for an input glyph. */
    bpad = menu_layout_px(s, 10);
    btn_h = in->small_h + menu_layout_px(s, 10);
    l->footer = rect(P.x, P.y + P.h - btn_h - l->pad, P.w, btn_h + l->pad);
    l->foot_glyph = 1;
    l->fbtn_pad = bpad;
    {
        /* On a small panel (ui 2 at 360 lines, or a narrow output with long
         * labels) the buttons give way in steps: the summary goes, then the
         * glyphs, then the buttons share what is left, and last they stop
         * at the panel's pad. */
        int fx = P.x + P.w - l->pad, by = l->footer.y + l->pad / 2, sw, inner = P.w - 2 * l->pad;
        int cw = in->cancel_w + 2 * bpad, vw = in->save_w + 2 * bpad, cx;
        sw = in->summary_w < inner / 2 ? in->summary_w : inner / 2;
        if (cw + vw + 2 * l->gap + sw > inner)
            sw = 0;
        if (cw + vw + l->gap > inner && in->cancel_bare_w > 0 && in->save_bare_w > 0) {
            l->foot_glyph = 0;
            cw = in->cancel_bare_w + 2 * bpad;
            vw = in->save_bare_w + 2 * bpad;
        }
        if (cw + vw + l->gap > inner) {
            cw = vw = (inner - l->gap) / 2 > 0 ? (inner - l->gap) / 2 : 0;
            l->fbtn_pad = bpad / 2;
        }
        l->fbtn[MENU_FB_SAVE] = rect(fx - vw, by, vw, btn_h);
        cx = fx - vw - l->gap - cw;
        if (cx < P.x + l->pad) {
            cw -= P.x + l->pad - cx;
            cx = P.x + l->pad;
            if (cw < 0)
                cw = 0;
        }
        l->fbtn[MENU_FB_CANCEL] = rect(cx, by, cw, btn_h);
        l->summary = rect(cx - (sw ? l->gap + sw : 0), by, sw, btn_h);
        l->hints = rect(P.x + l->pad, by, l->summary.x - (sw ? l->gap : 0) - l->gap - (P.x + l->pad), btn_h);
        if (l->hints.w < 0)
            l->hints.w = 0;
    }

    body_y = l->header.y + l->header.h;
    body_b = l->footer.y - l->gap;

    /* Toolbar (Q10): icons while it does not have the focus, icons and
     * labels when it does, the labels laid over the content so its rows
     * do not reflow. Labels need icon plus 8 em, and the unfolded bar may
     * cover at most 40% of the panel (the content keeps 60%); without the
     * room the bar stays icons and the shown entry's label is a tooltip. */
    em = menu_layout_px(s, in->tok.font_body);
    tbf = in->icon_w + 2 * l->pad;
    tbo = tbf + (in->label_w > 8 * em ? in->label_w : 8 * em);
    l->labels = !l->narrow && tbo * 10 <= P.w * 4;
    if (!l->labels)
        tbo = tbf;
    l->bar_folded = rect(P.x, body_y, tbf, body_b - body_y);
    l->bar_open = rect(P.x, body_y, tbo, body_b - body_y);
    l->open = in->toolbar_open && l->labels;
    l->bar = l->open ? l->bar_open : l->bar_folded;
    l->nsec = in->nsec < MENU_LAYOUT_SECTIONS ? in->nsec : MENU_LAYOUT_SECTIONS;
    /* An entry never gets shorter than its icon plus a little air; the
     * entries that do not fit then scroll with the shown one, and the rest
     * are zero (not drawn, not hit). */
    eh = l->rowh;
    if (l->nsec && l->nsec * eh > l->bar.h)
        eh = l->bar.h / l->nsec;
    if (eh < in->icon_w + l->pad / 2)
        eh = in->icon_w + l->pad / 2;
    {
        int fit = eh > 0 ? l->bar.h / eh : l->nsec, first = 0;
        if (fit < 1)
            fit = 1;
        if (fit < l->nsec && in->sec_sel >= fit)
            first = in->sec_sel - fit + 1;
        if (first > l->nsec - fit)
            first = l->nsec - fit > 0 ? l->nsec - fit : 0;
        for (int i = 0; i < l->nsec; i++) {
            int y = body_y + (i - first) * eh, ih = in->icon_w;
            if (i < first || i >= first + fit)
                continue;
            l->sec[i] = rect(l->bar.x + l->pad / 2, y, l->bar.w - l->pad, eh);
            l->sec_icon[i] = rect(P.x + (tbf - in->icon_w) / 2, y + (eh - ih) / 2, in->icon_w, ih);
            if (l->open)
                l->sec_label[i] = rect(P.x + tbf, y, tbo - tbf - l->pad, eh);
        }
    }
    if (in->toolbar_open && !l->labels && in->sec_sel >= 0 && in->sec_sel < l->nsec
        && l->sec[in->sec_sel].h > 0) {
        struct menu_rect e = l->sec[in->sec_sel];
        l->tooltip = rect(e.x + e.w + l->gap, e.y, in->label_w + 2 * l->pad, e.h);
    }

    /* Content: right of the folded bar, the intro line, the rows, then the
     * extra block. */
    l->content = rect(P.x + tbf, body_y, P.x + P.w - l->pad / 2 - (P.x + tbf), body_b - body_y);
    vy = body_y;
    if (in->intro) {
        l->intro = rect(l->content.x + l->pad, vy, l->content.w - 2 * l->pad, in->small_h);
        vy += in->small_h + l->gap;
    }
    vh = body_b - vy;
    if (in->extra_h > 0) {
        vh -= in->extra_h + l->gap;
        l->extra = rect(l->content.x + l->pad, vy + vh + l->gap, l->content.w - 2 * l->pad,
                        in->extra_h);
    }
    l->viewport = rect(l->content.x, vy, l->content.w, vh);
    vh = l->viewport.h;
    step = l->rowh + l->rgap;
    l->vis = vh / step;
    if (l->vis < 1)
        l->vis = 1;
    l->scroll = in->scroll;
    if (in->sel < l->scroll)
        l->scroll = in->sel;
    if (in->sel >= l->scroll + l->vis)
        l->scroll = in->sel - l->vis + 1;
    if (l->scroll > in->nrow - l->vis)
        l->scroll = in->nrow - l->vis > 0 ? in->nrow - l->vis : 0;
    if (l->scroll < 0)
        l->scroll = 0;
    for (int i = l->scroll; i < in->nrow && l->nvrow < MENU_LAYOUT_ROWS; i++) {
        struct menu_layout_row *vr = &l->row[l->nvrow];
        int y = vy + (i - l->scroll) * step;
        if (y >= vy + vh)
            break;
        vr->index = i;
        vr->r = rect(l->content.x, y, l->content.w, l->rowh);
        vr->clipped = y + l->rowh > vy + vh;
        row_controls(in, l, vr);
        l->nvrow++;
    }
    if (in->nrow > l->vis) {
        int bw = menu_layout_px(s, 4), th = vh * l->vis / in->nrow;
        int ty = vy + (vh - th) * l->scroll / (in->nrow - l->vis);
        l->scrollbar = 1;
        l->sb_track = rect(l->content.x + l->content.w - bw - menu_layout_px(s, 4), vy, bw, vh);
        l->sb_thumb = rect(l->sb_track.x, ty, bw, th);
    }

    /* The dialog (D9a): three quarters of the panel, centred on it. */
    l->dlg_on = in->dlg_on;
    if (in->dlg_on) {
        int lh = in->small_h + menu_layout_px(s, 4), bh = in->small_h + menu_layout_px(s, 14);
        int w = P.w * 3 / 4, nl = in->dlg_lines > 0 ? in->dlg_lines : 1, h, x, y;
        int labels = 0, total, bp = l->pad;
        for (int b = 0; b < MENU_LAYOUT_DLG_BTNS; b++)
            labels += in->dlg_btn_w[b];
        /* Wider than three quarters when the buttons need it (a large ui
         * scale), up to the panel; past that the buttons' own pads give. */
        total = labels + 2 * bp * MENU_LAYOUT_DLG_BTNS + 2 * l->gap;
        if (total + 2 * l->pad > w)
            w = total + 2 * l->pad < P.w ? total + 2 * l->pad : P.w;
        if (total + 2 * l->pad > w) {
            bp = (w - 2 * l->pad - 2 * l->gap - labels) / (2 * MENU_LAYOUT_DLG_BTNS);
            bp = bp > 0 ? bp : 0;
            total = labels + 2 * bp * MENU_LAYOUT_DLG_BTNS + 2 * l->gap;
        }
        h = l->pad + in->body_h + l->gap + nl * lh + l->gap + bh + l->pad;
        x = P.x + (P.w - w) / 2;
        y = P.y + (P.h - h) / 2;
        l->dlg = rect(x, y, w, h);
        l->dlg_title = rect(x + l->pad, y + l->pad, w - 2 * l->pad, in->body_h);
        l->dlg_line_y = y + l->pad + in->body_h + l->gap;
        l->dlg_line_h = lh;
        l->dlg_btn_pad = bp;
        bx = x + w - l->pad - total;
        for (int b = 0; b < MENU_LAYOUT_DLG_BTNS; b++) {
            int bw = in->dlg_btn_w[b] + 2 * bp;
            l->dlg_btn[b] = rect(bx, y + h - l->pad - bh, bw, bh);
            bx += bw + l->gap;
        }
    }
}

struct menu_hit menu_hit(const struct menu_layout *l, int x, int y)
{
    struct menu_hit h = {MENU_HIT_NONE, -1, MENU_PART_NONE};
    if (!inside(l->place, x, y))
        return h;
    if (!inside(l->panel, x, y)) {
        h.region = l->backdrop ? MENU_HIT_BACKDROP : MENU_HIT_NONE;
        return h;
    }
    h.region = MENU_HIT_PANEL;
    /* The dialog is modal: the panel under it does not act. */
    if (l->dlg_on) {
        if (inside(l->dlg, x, y)) {
            h.region = MENU_HIT_DIALOG;
            for (int b = 0; b < MENU_LAYOUT_DLG_BTNS; b++)
                if (inside(l->dlg_btn[b], x, y))
                    h.index = b;
        }
        return h;
    }
    if (inside(l->header, x, y)) {
        h.region = MENU_HIT_HEADER;
        for (int b = 0; b < MENU_HB_COUNT; b++)
            if (inside(l->hbtn[b], x, y))
                h.index = b;
        return h;
    }
    if (inside(l->footer, x, y)) {
        h.region = MENU_HIT_FOOTER;
        for (int b = 0; b < MENU_FB_COUNT; b++)
            if (inside(l->fbtn[b], x, y))
                h.index = b;
        return h;
    }
    /* The bar shown, folded or open: open it covers the rows' left edge. */
    if (inside(l->bar, x, y)) {
        h.region = MENU_HIT_TOOLBAR;
        for (int i = 0; i < l->nsec; i++)
            if (inside(l->sec[i], x, y))
                h.index = i;
        return h;
    }
    if (l->scrollbar) {
        /* A pad either side of the thin track, so a pointer can find it. */
        struct menu_rect t = l->sb_track;
        t.x -= l->pad / 2;
        t.w += l->pad;
        if (inside(t, x, y)) {
            h.region = MENU_HIT_SCROLL;
            if (y >= l->sb_thumb.y && y < l->sb_thumb.y + l->sb_thumb.h)
                h.part = MENU_PART_THUMB;
            return h;
        }
    }
    if (inside(l->viewport, x, y)) {
        for (int i = 0; i < l->nvrow; i++) {
            const struct menu_layout_row *vr = &l->row[i];
            struct menu_rect band;
            if (vr->clipped || !inside(vr->r, x, y))
                continue;
            h.region = MENU_HIT_ROW;
            h.index = vr->index;
            /* The track is thin: its whole column of the row counts. */
            band = rect(vr->track.x, vr->r.y, vr->track.w, vr->r.h);
            if (inside(vr->pill, x, y))
                h.part = MENU_PART_PILL;
            else if (inside(band, x, y))
                h.part = MENU_PART_TRACK;
            else if (inside(vr->prev, x, y))
                h.part = MENU_PART_PREV;
            else if (inside(vr->next, x, y))
                h.part = MENU_PART_NEXT;
            else if (inside(vr->value, x, y))
                h.part = MENU_PART_VALUE;
            return h;
        }
    }
    return h;
}
