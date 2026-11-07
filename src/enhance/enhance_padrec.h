/*
 * enhance_padrec.h - pad recordings from the menu and the shortcuts, and
 * the strip that shows them (pad-recording-ui R3, R4, R6).
 *
 * The session itself lives in src/usb/pad_session.h, beside the pad it
 * drives. This is the layer's side: the padrec.* actions' requests, the
 * recordings folder (<user dir>/recordings), the header a record gets,
 * the keys padrec.clock, padrec.start and overlay.padrec_hud, and the
 * NV2A_OVL_PADREC strip. Every call is on the menu's thread (the pad
 * filter's hook or the flip tick, under the menu-step lock) unless it says
 * otherwise.
 */
#ifndef ENHANCE_PADREC_H
#define ENHANCE_PADREC_H

#include <stddef.h>
#include <stdint.h>

#include "microui.h"
#include "pad_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reads the keys (at the menu's start, so none is "unused" with the menu
 * off) and hands padrec.clock to the session. */
void enhance_padrec_start(void);

/* padrec.start: 1 when a session started from the menu waits for it to
 * close (menu_close, the default), 0 for now. */
int enhance_padrec_start_on_close(void);

/* A menu item's new value for padrec.start, padrec.clock or
 * overlay.padrec_hud: in force for the next session (the strip at once). */
void enhance_padrec_set_key(const char *key, const char *v);

/* A record or playback running, armed or waiting on an anchor: the page's
 * and the shortcuts' guard against starting another over it. */
int enhance_padrec_live(int state);

/* The actions' requests (enhance_actions_padrec_request), from the menu's
 * hook. menu_open: the menu is open, and its page has the same commands,
 * so the requests are dropped (F5 is the menu's Save). */
void enhance_padrec_requests(int menu_open);

/* The menu closed: a session armed by the page starts. */
void enhance_padrec_menu_closed(void);

/* <user dir>/recordings into out: 1, or 0 with no user folder
 * (RECOMP_CONFIG=none). */
int enhance_padrec_dir(char *out, size_t n);

/* One file of the folder. */
#define ENHANCE_PADREC_FILES 64
struct enhance_padrec_file {
    char name[128];             /* the file name, UTF-8 */
    int64_t mtime;              /* seconds */
};
/* The folder's *.pad files, newest first, at most max. The count, or -1
 * when it cannot be read (0 when it does not exist yet). */
int enhance_padrec_list(struct enhance_padrec_file *out, int max);

/* Start a record now or armed, named after the game and the time, with
 * the start point text. 0, or -1 with why. */
int enhance_padrec_record(int armed, const char *start_point, char *why, size_t n);
/* Play a file of the folder (its name) now or armed. 0, or -1 with why. */
int enhance_padrec_play_file(const char *name, int armed, char *why, size_t n);
/* The last file played or recorded ("" for none), for the play action. */
const char *enhance_padrec_last(void);

/* overlay.padrec_hud: auto, off. "on" (env-driven scripts too) waits
 * until the env path reports a status (pad-recording-ui tasks 5). */
enum { PADREC_HUD_AUTO, PADREC_HUD_OFF };

/* The strip (R4): from the panel hook on the flip tick; stopping hides it
 * (any thread). */
void enhance_padrec_strip_tick(int stopping);

/* The strip for st into ctx, for a w x h output, at the top when top. The
 * time comes from st (the session's measured rate). Returns 1 when it drew. */
int enhance_padrec_strip_compose(mu_Context *ctx, const PadrecStatus *st, uint32_t w,
                                 uint32_t h, int top);

/* The strip's lines for st: the state and name, then the times, step and
 * count. For the strip and its test. */
void enhance_padrec_strip_text(const PadrecStatus *st, char *head, size_t hn, char *times,
                               size_t tn);

#ifdef __cplusplus
}
#endif

#endif /* ENHANCE_PADREC_H */
