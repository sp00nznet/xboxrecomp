/*
 * enhance_menu_layout.h - where everything in the in-game menu goes (D1,
 * D2): the panel in each size mode, the header and its buttons, the
 * section toolbar folded and unfolded, the content's intro line, rows and
 * their value controls, the scrollbar, the footer's slots and the unsaved-
 * changes dialog. One computation serves the draw, the text wrap and the
 * hit test, so the three cannot drift apart.
 *
 * Pure C11: no overlay, font or clock. The menu measures its text and
 * passes the sizes in, so tests/enhance_menu_layout can pin the geometry
 * at any output size and ui scale on its own.
 */
#ifndef ENHANCE_MENU_LAYOUT_H
#define ENHANCE_MENU_LAYOUT_H

#ifdef __cplusplus
extern "C" {
#endif

#define MENU_LAYOUT_SECTIONS 16
#define MENU_LAYOUT_ROWS     48   /* rows on screen at once, the clipped one too */
#define MENU_LAYOUT_DLG_BTNS 3

enum menu_size { MENU_SIZE_COMPACT, MENU_SIZE_EXPANDED };

/* The header's buttons, left to right. */
enum { MENU_HB_HINTS, MENU_HB_SIZE, MENU_HB_CLOSE, MENU_HB_COUNT };
/* The footer's buttons. */
enum { MENU_FB_CANCEL, MENU_FB_SAVE, MENU_FB_COUNT };

enum menu_row_kind {
    MENU_ROW_PLAIN,    /* a label and a text value */
    MENU_ROW_HEADER,
    MENU_ROW_TEXT,
    MENU_ROW_TOGGLE,   /* a pill at the right */
    MENU_ROW_RANGE,    /* a track at the right, the value before it */
    MENU_ROW_CHOICE,   /* < value > at the right */
};

enum menu_region {
    MENU_HIT_NONE,      /* outside the menu: the game shows there */
    MENU_HIT_BACKDROP,  /* the expanded mode's dimmed game */
    MENU_HIT_PANEL,     /* the panel, on nothing that acts */
    MENU_HIT_HEADER,    /* index: MENU_HB_*, or -1 */
    MENU_HIT_TOOLBAR,   /* index: the section entry, or -1 */
    MENU_HIT_ROW,       /* index: the row (s_row's, not the screen's) */
    MENU_HIT_SCROLL,
    MENU_HIT_FOOTER,    /* index: MENU_FB_*, or -1 */
    MENU_HIT_DIALOG,    /* index: the button, or -1 */
};

enum menu_part {
    MENU_PART_NONE,
    MENU_PART_PILL,
    MENU_PART_TRACK,
    MENU_PART_PREV,
    MENU_PART_NEXT,
    MENU_PART_VALUE,
    MENU_PART_THUMB,
};

struct menu_rect { int x, y, w, h; };

/* The theme's tokens at 1080 lines (nv2a_ovl_theme_default's). */
struct menu_layout_tokens {
    int pad, gap, row_h, font_body;
};

struct menu_layout_in {
    int w, h;              /* the placement */
    float s;               /* nv2a_ovl_ui_scale(h) */
    struct menu_layout_tokens tok;
    int size;              /* enum menu_size */
    int toolbar_open;      /* the toolbar has the focus, or the LB/RB peek shows */
    int intro;             /* draw the page's intro line */
    /* Measured text: line heights, the widest section label, an icon and
     * a choice arrow. */
    int title_h, body_h, small_h;
    int label_w, icon_w, arrow_w;
    int nsec, sec_sel;     /* toolbar entries and the one shown */
    /* Rows: kinds and (for range and choice) the value's width, nrow each;
     * NULL kinds lay every row out plain. */
    int nrow, scroll, sel;
    const unsigned char *row_kind;
    const int *row_value_w;
    int extra_h;           /* a block under the rows (the Stats graph), or 0 */
    /* The footer: the Cancel and Save labels' widths (glyph and text) and
     * the summary's ("3 unsaved"). */
    int cancel_w, save_w, summary_w;
    /* The same labels without the glyph, for a footer too small for it;
     * 0 when there is no bare form. */
    int cancel_bare_w, save_bare_w;
    /* The dialog: on, its body's line count and its buttons' label widths. */
    int dlg_on, dlg_lines;
    int dlg_btn_w[MENU_LAYOUT_DLG_BTNS];
};

struct menu_layout_row {
    int index;             /* the row */
    int clipped;           /* past the viewport's foot: drawn cut, never hit */
    struct menu_rect r;
    /* Value controls, zero when the kind has none. */
    struct menu_rect pill, track, prev, next, value;
};

struct menu_layout {
    float s;
    int pad, gap, rowh, rgap;
    int narrow;            /* under 640 wide or portrait: the width, icons only */
    struct menu_rect place;     /* the placement, at 0,0 */
    struct menu_rect panel;
    int backdrop;               /* the expanded mode dims place */
    struct menu_rect header, title;
    struct menu_rect hbtn[MENU_HB_COUNT];
    /* The toolbar folded (icons) and unfolded (icons and labels, drawn over
     * the content's left edge, which does not move). bar is the one shown. */
    struct menu_rect bar_folded, bar_open, bar;
    int open;                   /* bar is bar_open */
    int labels;                 /* the unfolded bar has room for labels */
    int nsec;
    struct menu_rect sec[MENU_LAYOUT_SECTIONS];      /* bar's entries */
    struct menu_rect sec_icon[MENU_LAYOUT_SECTIONS];
    struct menu_rect sec_label[MENU_LAYOUT_SECTIONS];/* zero while folded */
    struct menu_rect tooltip;   /* a label beside the shown entry with no room for it, or zero */
    struct menu_rect content, intro, viewport, extra;
    int scroll, vis;            /* the clamped scroll and the whole rows in view */
    int nvrow;
    struct menu_layout_row row[MENU_LAYOUT_ROWS];
    int scrollbar;              /* more rows than fit */
    struct menu_rect sb_track, sb_thumb;
    struct menu_rect footer, hints, summary;
    struct menu_rect fbtn[MENU_FB_COUNT];
    int foot_glyph;             /* the buttons have room for their glyphs */
    int fbtn_pad;               /* a footer button's label inset */
    int dlg_on;
    struct menu_rect dlg, dlg_title;
    struct menu_rect dlg_btn[MENU_LAYOUT_DLG_BTNS];
    int dlg_line_y, dlg_line_h;
    int dlg_btn_pad;            /* a button's label inset */
};

struct menu_hit {
    int region;   /* enum menu_region */
    int index;
    int part;     /* enum menu_part */
};

/* A token scaled and rounded to whole pixels, at least 1 for a non-zero
 * token: nv2a_ovl_px's rule, so the menu and the layout agree. */
int menu_layout_px(float s, int token);

void menu_layout_compute(const struct menu_layout_in *in, struct menu_layout *out);

/* What is under x, y (the placement's pixels). */
struct menu_hit menu_hit(const struct menu_layout *l, int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* ENHANCE_MENU_LAYOUT_H */
