/*
 * enhance_osk.h - the on-screen keyboard (overlay-revamp D4): a key grid
 * docked under a text field, for a player on a pad. It opens when a text
 * field takes focus while the pad is the latest device, and it is the one
 * way to type on a pad where no system keyboard shows (Proton in Game
 * Mode until the Steam keyboard can be asked for, a desktop with no
 * keyboard).
 *
 * The grid is ten cells wide and five rows deep: digits, three rows of
 * letters with - _ and ., then Shift, Space and Done over several cells.
 * The d-pad moves between keys (a wide key is one stop), A presses, X
 * deletes, B cancels the field (the menu's rule, as on the field).
 *
 * The model, the geometry and the hit test are here, pure C11 with no
 * overlay or clock, so the ctest pins them; enhance_menu.c draws it.
 */
#ifndef ENHANCE_OSK_H
#define ENHANCE_OSK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OSK_COLS 10
#define OSK_ROWS 5

enum osk_kind { OSK_KEY_CHAR, OSK_KEY_BACKSPACE, OSK_KEY_SHIFT, OSK_KEY_SPACE, OSK_KEY_DONE };

/* What a press did. */
enum osk_act { OSK_ACT_NONE, OSK_ACT_TEXT, OSK_ACT_BACKSPACE, OSK_ACT_DONE };

struct osk {
    int on;
    int row, col;   /* the focused key's first cell */
    int shift;      /* the letters are capitals until the next letter */
};

struct osk_rect { int x, y, w, h; };

struct osk_layout {
    struct osk_rect panel;
    struct osk_rect cell[OSK_ROWS][OSK_COLS];   /* a wide key: its first cell spans it */
    int span[OSK_ROWS][OSK_COLS];               /* cells the key starting here covers, 0 inside one */
};

void enhance_osk_open(struct osk *k);
void enhance_osk_close(struct osk *k);

/* The key whose cells include row, col: its first column, kind and label
 * ("a", "A" when shifted, "Space"); NULL label for none. */
int  enhance_osk_key(int row, int col, int shift, int *first, const char **label);

/* One d-pad step: dx, dy in -1..1. Rows wrap, as columns do. */
void enhance_osk_move(struct osk *k, int dx, int dy);

/* Press the focused key. OSK_ACT_TEXT writes its UTF-8 into out. Shift
 * toggles and returns NONE. */
int  enhance_osk_press(struct osk *k, char *out, size_t n);

/* The grid under the field fx, fy, fw, fh, inside the area aw x ah (above
 * the field when there is no room under it); key is the cell's side at the
 * menu's scale, gap the space between cells. */
void enhance_osk_layout(int fx, int fy, int fw, int fh, int aw, int ah, int key, int gap,
                        struct osk_layout *out);

/* The key under x, y: 1 with its row and first column, else 0. */
int  enhance_osk_hit(const struct osk_layout *l, int x, int y, int *row, int *col);

/* The footer's hint for the keyboard; steam names Steam's own keyboard
 * (Steam + X) as the other way. Glyph tokens (enhance_text.h). */
const char *enhance_osk_hint(int steam);

#ifdef __cplusplus
}
#endif

#endif /* ENHANCE_OSK_H */
