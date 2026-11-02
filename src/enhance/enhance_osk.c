/*
 * enhance_osk.c - the on-screen keyboard's model, geometry and hit test
 * (enhance_osk.h).
 */
#include "enhance_osk.h"

#include <stdio.h>
#include <string.h>

/* Each row as its ten cells' characters; a capital letter is the shifted
 * form of its lower case one. The last two rows end in wide keys. */
static const char k_rows[OSK_ROWS - 1][OSK_COLS + 1] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl-",
    "zxcvbnm_.\x08",
};

/* The bottom row: Shift over two cells, Space over six, Done over two. */
static const struct { int first, span, kind; const char *label; } k_bottom[] = {
    { 0, 2, OSK_KEY_SHIFT, "Shift" },
    { 2, 6, OSK_KEY_SPACE, "Space" },
    { 8, 2, OSK_KEY_DONE, "Done" },
};

void enhance_osk_open(struct osk *k)
{
    memset(k, 0, sizeof *k);
    k->on = 1;
    k->row = 1;     /* on q: the letters first */
}

void enhance_osk_close(struct osk *k)
{
    k->on = 0;
}

int enhance_osk_key(int row, int col, int shift, int *first, const char **label)
{
    static char one[OSK_ROWS][OSK_COLS][2];
    *label = NULL;
    if (row < 0 || row >= OSK_ROWS || col < 0 || col >= OSK_COLS)
        return -1;
    if (row == OSK_ROWS - 1) {
        for (unsigned i = 0; i < sizeof k_bottom / sizeof k_bottom[0]; i++)
            if (col >= k_bottom[i].first && col < k_bottom[i].first + k_bottom[i].span) {
                *first = k_bottom[i].first;
                *label = k_bottom[i].label;
                return k_bottom[i].kind;
            }
        return -1;
    }
    *first = col;
    if (k_rows[row][col] == '\x08') {
        *label = "Del";
        return OSK_KEY_BACKSPACE;
    }
    one[row][col][0] = k_rows[row][col];
    if (shift && one[row][col][0] >= 'a' && one[row][col][0] <= 'z')
        one[row][col][0] = (char)(one[row][col][0] - 'a' + 'A');
    one[row][col][1] = 0;
    *label = one[row][col];
    return OSK_KEY_CHAR;
}

void enhance_osk_move(struct osk *k, int dx, int dy)
{
    int first;
    const char *label;
    if (dy) {
        /* The column is kept across rows, so Up from Space comes back near
         * where it went down; the key there is the stop. */
        k->row = (k->row + dy + OSK_ROWS) % OSK_ROWS;
        enhance_osk_key(k->row, k->col, k->shift, &first, &label);
        k->col = first;
    }
    if (dx) {
        int c = k->col;
        /* A wide key is one stop: step past its cells. */
        if (dx > 0) {
            do
                c = (c + 1) % OSK_COLS;
            while (enhance_osk_key(k->row, c, 0, &first, &label) >= 0 && first != c);
        } else {
            c = (c - 1 + OSK_COLS) % OSK_COLS;
        }
        enhance_osk_key(k->row, c, 0, &first, &label);
        k->col = first;
    }
}

int enhance_osk_press(struct osk *k, char *out, size_t n)
{
    int first;
    const char *label;
    int kind = enhance_osk_key(k->row, k->col, k->shift, &first, &label);
    if (n)
        out[0] = 0;
    switch (kind) {
    case OSK_KEY_CHAR:
        snprintf(out, n, "%s", label);
        /* One capital, as a phone's shift: the next letter is small again. */
        if (label[0] >= 'A' && label[0] <= 'Z')
            k->shift = 0;
        return OSK_ACT_TEXT;
    case OSK_KEY_SPACE:
        snprintf(out, n, " ");
        return OSK_ACT_TEXT;
    case OSK_KEY_BACKSPACE:
        return OSK_ACT_BACKSPACE;
    case OSK_KEY_SHIFT:
        k->shift = !k->shift;
        return OSK_ACT_NONE;
    case OSK_KEY_DONE:
        return OSK_ACT_DONE;
    default:
        return OSK_ACT_NONE;
    }
}

void enhance_osk_layout(int fx, int fy, int fw, int fh, int aw, int ah, int key, int gap,
                        struct osk_layout *l)
{
    int gw = OSK_COLS * key + (OSK_COLS - 1) * gap, gh = OSK_ROWS * key + (OSK_ROWS - 1) * gap;
    int pw = gw + 2 * gap, ph = gh + 2 * gap, x, y;
    memset(l, 0, sizeof *l);
    (void)fw;
    x = fx;
    if (x + pw > aw)
        x = aw - pw;
    if (x < 0)
        x = 0;
    y = fy + fh + gap;
    if (y + ph > ah)
        y = fy - gap - ph;   /* no room under the field: above it */
    if (y < 0)
        y = 0;
    l->panel = (struct osk_rect){ x, y, pw, ph };
    for (int r = 0; r < OSK_ROWS; r++)
        for (int c = 0; c < OSK_COLS; c++) {
            int first, span = 0;
            const char *label;
            if (enhance_osk_key(r, c, 0, &first, &label) < 0 || first != c)
                continue;
            while (c + span < OSK_COLS && enhance_osk_key(r, c + span, 0, &first, &label) >= 0
                   && first == c)
                span++;
            l->span[r][c] = span;
            l->cell[r][c] = (struct osk_rect){ x + gap + c * (key + gap), y + gap + r * (key + gap),
                                               span * key + (span - 1) * gap, key };
        }
}

int enhance_osk_hit(const struct osk_layout *l, int x, int y, int *row, int *col)
{
    for (int r = 0; r < OSK_ROWS; r++)
        for (int c = 0; c < OSK_COLS; c++) {
            const struct osk_rect *k = &l->cell[r][c];
            if (!l->span[r][c])
                continue;
            if (x >= k->x && x < k->x + k->w && y >= k->y && y < k->y + k->h) {
                *row = r;
                *col = c;
                return 1;
            }
        }
    return 0;
}

const char *enhance_osk_hint(int steam)
{
    return steam ? "{pad:south} Type \xC2\xB7 {pad:west} Delete \xC2\xB7 {pad:east} Cancel"
                   " \xC2\xB7 Steam + X: system keyboard"
                 : "{pad:south} Type \xC2\xB7 {pad:west} Delete \xC2\xB7 {pad:east} Cancel";
}
