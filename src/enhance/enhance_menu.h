/*
 * enhance_menu.h - the in-game enhancements menu (opt-in: menu.enabled,
 * RECOMP_MENU; built with -DXBOXRECOMP_ENHANCE=ON).
 *
 * The menu is a page of rows drawn into the overlay's MENU layer
 * (nv2a_overlay.h). The menu.open action (F10, guide+a, paddle1 by
 * default; enhance_actions.h) opens and closes it. While it is open the
 * title reads a neutral pad with a frozen packet number, and the menu reads
 * the host pad and keys itself; the guest keeps running (no pause hook).
 *
 * Items are a flat table: the toolkit's own (render, pack, stats, the mouse
 * group), a title's (enhance_menu_add, on the Game page) and a mod's
 * (mod.h menu_add, on the mod's sub-page). Each item persists to its
 * config key through Save, which writes the differences only
 * (recomp_cfg_patch on enhance_cfg_user_path): a value equal to the
 * defaults file's, or to the item's code default when the defaults file
 * has none, removes the key.
 *
 * Threads: every step runs on the thread that called the actions hook
 * (the title's poll, or the flip tick while the title does not poll),
 * under the overlay's menu-step lock; a second thread that finds the lock
 * taken skips its step. set() runs there too, so an item's setter only
 * stores an atomic or queues a request (nv2a_host_opts_request).
 *
 * Docs: docs/runtime/config.md, "In-game menu".
 */
#ifndef ENHANCE_MENU_H
#define ENHANCE_MENU_H

#include <stddef.h>
#include <stdint.h>

#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

enum enhance_item_kind {
    ENHANCE_ITEM_TOGGLE,    /* "true" / "false": a pill */
    ENHANCE_ITEM_CHOICE,    /* one of choices: < value > */
    ENHANCE_ITEM_RANGE,     /* a number lo..hi by step: a slider */
    ENHANCE_ITEM_INFO,      /* read only: get() is the text */
    ENHANCE_ITEM_ACTION     /* set(it, NULL) on A / Enter */
};

/* When a change takes effect (design R1): the menu shows no mark for LIVE
 * and FLUSH (the common "applies now"), "next stage" for STAGE, "next
 * activation" for ACTIVATE and "applies at restart" for RESTART. */
enum enhance_item_live {
    ENHANCE_LIVE = 0,       /* L: at the next flip */
    ENHANCE_LIVE_FLUSH,     /* F: at the flip, rebuilding a cache or a target */
    ENHANCE_LIVE_STAGE,     /* S: at a later game point */
    ENHANCE_LIVE_RESTART,   /* R: at the next start; a set() only records it */
    /* A: when the mod next turns its feature on (the free camera reads its
     * HUD choice as flight starts); a mod setting's class (mod-settings D2).
     * Last, so the earlier values keep their numbers. */
    ENHANCE_LIVE_ACTIVATE
};

#define ENHANCE_ITEM_NUMERIC 0x1u   /* CHOICE values are numbers: saved bare */
#define ENHANCE_ITEM_NOSAVE  0x2u   /* this session only: Save skips it */

struct enhance_item {
    const char *key;        /* "fx.glow": the config key it persists to */
    const char *label;      /* "Glow" */
    const char *page;       /* "Game" for a title's; a mod's is ignored */
    enum enhance_item_kind kind;
    const char *const *choices;     /* CHOICE: NULL-terminated */
    double lo, hi, step;            /* RANGE */
    const char *note;               /* shown dimmed after the label */
    /* The new value as text ("true", "linear", "2"); NULL for an ACTION.
     * Called whenever it is non-NULL; a RESTART item's set() only records
     * the value for the next start. NULL set: the value is only stored. */
    void (*set)(const struct enhance_item *it, const char *value);
    /* The value in force, into buf; NULL get: the config's value. */
    const char *(*get)(const struct enhance_item *it, char *buf, size_t n);
    enum enhance_item_live live;
    unsigned flags;                 /* ENHANCE_ITEM_* */
    const char *def;                /* the code default as text ("false") */
    void *user;
    /* What the setting does, as the row's hint shows it; NULL: none.
     * Appended, so a table written before it keeps its meaning. */
    const char *help;
};

#define ENHANCE_MENU_MAX_ITEMS 128

/* Set where struct enhance_item has .help and enhance_menu_page_intro()
 * exists, so a game can use them and still build on an older toolkit. */
#define ENHANCE_MENU_HELP 1

/* A title's item, from its init (before the first step): kept, not copied,
 * so it must be static. Returns 0 when the table is full or the item has
 * no key or label. */
int enhance_menu_add(const struct enhance_item *it);

/* The intro line shown under a section's tabs ("Game": what the title's
 * options are about). A title calls it from its init, as for its items;
 * text is kept, not copied. Returns 0 for an unknown section. */
int enhance_menu_page_intro(const char *page, const char *text);

/* From xbox_enhance_init, after the overlay arm: when menu.enabled is on,
 * arms the MENU layer, sets the actions hook and logs
 * "[ENHANCE] menu.enabled=on (F10, guide+a, paddle1)"; otherwise logs
 * "[ENHANCE] menu.enabled=off" and does nothing more. */
void enhance_menu_start(void);

/* The actions hook (enhance_actions_set_menu_hook): one step. Opens on a
 * menu.open request; while open, reads the host input, steps the menu,
 * draws it and neutralises *st (NULL from the flip tick). */
void enhance_menu_hook(unsigned port, uint32_t held, XBOX_INPUT_STATE *st,
                       uint64_t now_ms);

/* ---- the state machine (the ctest drives it without a window) -------- */

enum enhance_menu_event {
    ENHANCE_EV_NONE = 0,
    ENHANCE_EV_UP, ENHANCE_EV_DOWN, ENHANCE_EV_LEFT, ENHANCE_EV_RIGHT,
    ENHANCE_EV_OK,          /* A, Enter, Space: toggle, enter, run */
    ENHANCE_EV_BACK,        /* B, Esc, Backspace: leave a sub-list, else close */
    ENHANCE_EV_PAGE_PREV,   /* LB, Q, PgUp */
    ENHANCE_EV_PAGE_NEXT,   /* RB, E, PgDn */
    ENHANCE_EV_SAVE,        /* Y, F5 */
    ENHANCE_EV_CLEAR,       /* X, Delete: clear an action's bindings */
    ENHANCE_EV_CLOSE,       /* the header's close: asks first when unsaved */
    ENHANCE_EV_CANCEL,      /* View (on release), Shift+Esc: leave, asking first */
    ENHANCE_EV_HINTS,       /* L3 (on release): hints inline or as a tooltip */
    ENHANCE_EV_SIZE         /* R3 (on release), the header's size button: compact or expanded */
};

int  enhance_menu_open(uint64_t now_ms);
/* Closes at once, with no question: the leave requests (B at the top, the
 * close, Cancel, the toggle) go through the unsaved-changes dialog. */
void enhance_menu_close(void);
int  enhance_menu_is_open(void);
/* One event; returns 1 when the page must be drawn again. */
int  enhance_menu_event(enum enhance_menu_event ev, uint64_t now_ms);
/* Compose and publish the MENU layer now. */
void enhance_menu_draw(uint64_t now_ms);

/* Introspection for the ctest. */
int  enhance_menu_page_count(void);
const char *enhance_menu_page_name(int page);
int  enhance_menu_page(void);
int  enhance_menu_set_page(const char *name);   /* 1 when found and shown */
/* 0 for a page left out of the tab strip (Shaders, where none can run). */
int  enhance_menu_page_shown(int page);
int  enhance_menu_rows(void);
int  enhance_menu_selected(void);
/* Row i's label and value text (value "" for none); 0 when i is out of
 * range. *selectable may be NULL. */
int  enhance_menu_row(int i, char *label, size_t ln, char *value, size_t vn,
                      int *selectable);
/* Row i's help, as the footer shows it while the row is selected ("" for
 * none); NULL when i is out of range. */
const char *enhance_menu_row_help(int i);
/* Row i's change class (an enhance_item_live) for a setting row, else -1. */
int  enhance_menu_row_live(int i);
int  enhance_menu_select_row(const char *label);  /* 1 when found */
int  enhance_menu_capturing(void);
const char *enhance_menu_status(void);           /* the last message */
int  enhance_menu_unsaved(void);
/* Every unsaved change back to what was loaded or saved last: slots (live
 * ones through set()), bindings, the shader overrides and the shader
 * profiles' edits. A deleted profile stays deleted. */
void enhance_menu_revert(void);
/* Row i's markers (design D10), 0 when i is out of range. */
#define ENHANCE_ROW_UNSAVED    0x1  /* changed since the last load or save */
#define ENHANCE_ROW_NONDEFAULT 0x2  /* differs from the stock value */
#define ENHANCE_ROW_FORCED     0x4  /* the environment sets it: read only */
int  enhance_menu_row_state(int i);
/* The unsaved-changes dialog: -1 when closed, else the focused button
 * (0 Exit without saving, 1 Exit and Save, 2 Go Back). */
int  enhance_menu_dialog(void);
const char *enhance_menu_dialog_text(void);     /* its body, "" when closed */
/* A section's intro, NULL for none. */
const char *enhance_menu_page_intro_text(int page);
/* 1 when hints show inline under the rows, 0 as a tooltip (L3 flips it). */
int  enhance_menu_hints_inline(void);
/* The size mode (D2): 0 compact, 1 expanded. R3 and the header's button
 * flip it; overlay.menu_size keeps it across runs. */
int  enhance_menu_size(void);
/* The section toolbar holds the focus (Q10): it unfolds with its labels.
 * Phase C's focus model drives it; until then the ctest and the
 * screenshots set it. */
void enhance_menu_set_toolbar_focus(int on);
/* 1 while the toolbar shows its labels: the focus, or the 1.5 s after a
 * section change (the LB/RB peek). */
int  enhance_menu_toolbar_open(uint64_t now_ms);
/* The key and its TOML literal for each pending change, as Save would
 * write them (NULL value: the key is removed); returns the count. */
int  enhance_menu_pending(const char **keys, const char **values, int max);
void enhance_menu_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* ENHANCE_MENU_H */
