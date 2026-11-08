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
#include <time.h>

#include "microui.h"
#include "pad_header.h"
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
/* The last playback again (Play again), now or armed. 0, or -1 with why. */
int enhance_padrec_again(int armed, char *why, size_t n);
/* Whether a file whose Info read gave rc (rc_why) and *info plays here:
 * read whole, recorded in the game (origin session), and this title's.
 * 1, or 0 with why (Play's help on its page). */
int enhance_padrec_playable(const PadFileInfo *info, int rc, const char *rc_why, char *why,
                            size_t n);
/* The last file played or recorded ("" for none), for the play action. */
const char *enhance_padrec_last(void);

/* ---- setups and chains (R6) ---------------------------------------------- */

/* A setup: a file of <exe dir>/pad-setups/ (shipped with the game, read
 * only, its !<action> steps trusted), or a recording of the folder whose
 * header says kind = "setup" (the player's: its actions are confirmed on
 * Play, as any other file's). */
#define ENHANCE_PADREC_SETUPS 32
struct enhance_padrec_setup {
    char name[128];             /* the file name, UTF-8 */
    int shipped;                /* 1: <exe dir>/pad-setups/ */
};

/* <exe dir>/pad-setups (../Resources/pad-setups in a macOS bundle) into
 * out: 1 when the folder exists. */
int enhance_padrec_setups_dir(char *out, size_t n);

/* The setups, shipped ones first, each group by name, at most max. The
 * count (0 for none). Reads the header of every recording of the folder:
 * on the page's request, never per draw. */
int enhance_padrec_setups(struct enhance_padrec_setup *out, int max);

/* What a chain of setup (NULL: none) then name (NULL: none) would fire
 * and the player has not confirmed: the distinct !<action> names of its
 * untrusted files (every file but a shipped setup), ", " joined into out.
 * Their count; -1 with why when a file cannot be read. */
int enhance_padrec_chain_actions(const struct enhance_padrec_setup *setup, const char *name,
                                 char *out, size_t on, char *why, size_t n);

/* Play setup (NULL: none) then the folder's file name (NULL: none) as a
 * chain, now or armed. record: a new record starts at the chain's end
 * ("Play setup, then record", R3), its header naming the setup and the
 * start point. confirmed: the actions the player saw and confirmed
 * (enhance_padrec_chain_actions' list), so every file's fire, and a file
 * that names another since is refused; NULL: none confirmed. 0; -1 with
 * why; PADREC_E_REQUIRES when a file needs a key that is off
 * (padrec_unmet names it). */
int enhance_padrec_play_chain(const struct enhance_padrec_setup *setup, const char *name,
                              int record, const char *start_point, int armed,
                              const char *confirmed, char *why, size_t n);

/* A setup's file read whole, as enhance_padrec_read reads the folder's
 * (a bug report carries it). 0, or -1 with why. */
int enhance_padrec_read_setup(const struct enhance_padrec_setup *s, char **text, size_t *len,
                              char *why, size_t n);

/* 1 when a requires key is an installed mod's switch (mods.<id>, or the
 * mod's enable setting): the only keys the page offers to turn on, since
 * the key comes from a file that may be someone else's. */
int enhance_padrec_require_turnable(const char *key);

/* Turn a requires key on in the user file, at once and alone, as the
 * menu's layout keys are: a mod's switch through the mod layer (live when
 * the mod can switch live) with its setting's literal. Any other key is
 * refused (-1). 0 when it is on now, 1 when it applies at the next start,
 * -1 with why. */
int enhance_padrec_require_on(const char *key, char *why, size_t n);

/* ---- the files (R5): import, export, rename, delete, Info ---------------- */

/* A new recording's stem: "<game>-<yyyymmdd-hhmm>" at local time t, the
 * game the running title's name as a slug ("pad" without one). */
void enhance_padrec_default_stem(time_t t, char *out, size_t n);

/* Check text (len bytes, as read) for an import into the running title:
 * pad_file_check with its title id and name. 0 with *info, or -1 with
 * why. */
int enhance_padrec_check(const char *text, size_t len, PadFileInfo *info, char *why,
                         size_t n);

/* Copy a checked text into the folder under a name not taken, from the
 * picked file's name (src_name, its .pad dropped; the default stem when
 * fb_name_check refuses it). The name used into out. 0, or -1 with why. */
int enhance_padrec_import(const char *src_name, const char *text, size_t len, char *out,
                          size_t on, char *why, size_t n);

/* A file of the folder (its name), read whole for an export: malloc'd,
 * NUL ended, at most PAD_FILE_MAX bytes. 0, or -1 with why. */
int enhance_padrec_read(const char *name, char **text, size_t *len, char *why, size_t n);

/* A file's header and counts for Info: any title's file shows. 0, or -1
 * with why. */
int enhance_padrec_info(const char *name, PadFileInfo *info, char *why, size_t n);

/* Rename a file to typed (fb_name_check, ".pad" added): the header's name
 * follows, so the strip shows the new one. Refused for the file a record
 * is writing and under an env key. The new name into out. 0, or -1 with
 * why. */
int enhance_padrec_rename(const char *name, const char *typed, char *out, size_t on,
                          char *why, size_t n);

/* Delete a file of the folder: refused as rename is. 0, or -1 with why. */
int enhance_padrec_delete(const char *name, char *why, size_t n);

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
