/*
 * pad_session.h -- a pad recording or playback started in the game
 * (pad-recording-ui R2, R3, R7).
 *
 * The env keys (RECOMP_PAD_SCRIPT, RECOMP_PAD_LIVE, RECOMP_PAD_PRESS,
 * pad_record) drive the pad from the command line, for a whole run. A
 * session is the same thing started and stopped from the menu or a
 * shortcut: one record, or one playback, at a time, with its own steps,
 * anchors and clock. The env keys win: while one is set no session starts
 * (padrec_env_driven), so one owner drives the pad at a time.
 *
 * Threads. The menu thread (the caller of padrec_record_start, padrec_play
 * and the rest) builds a session, file read and parsed, and publishes it in
 * a pending slot. The pad thread swaps it in at the start of a port-0 poll
 * (padrec_sync), under the pad lock, so the ports of one poll all read one
 * session and a file open inside pad_anchors_open never sees it change. The
 * session it replaces is retired and freed at a later swap, under the same
 * lock; the menu thread frees only a pending session it took back before
 * the pad thread saw it. Stop is a flag the pad thread acts on at its next
 * poll, as pad_record_end does for the env record. The page
 * and the strip read a copy of the state under a seqlock (padrec_status),
 * never the session.
 *
 * Counts. A session's steps are timed from its own first poll or flip
 * (origin = "session" in the header): a record started at poll 1000 writes
 * g0 for that poll, and a playback started at poll 5000 presses g0 there.
 * A file with no header is an env record, counted from boot, and is refused
 * here (it plays from the start of a run, through RECOMP_PAD_SCRIPT).
 */
#ifndef XBOX_USB_PAD_SESSION_H
#define XBOX_USB_PAD_SESSION_H

#include <stddef.h>
#include <stdint.h>

/* The pad lock: the env anchors and record, and the session, are read
 * under it on the pad thread and in file opens (usb_gamepad.c). */
void pad_lock(void);
void pad_unlock(void);

/* RECOMP_DEBUG=pad_anchor: an open whose path holds one of the texts. */
int pad_anchor_wanted(const char *path);

/* The clocks of the poll a call belongs to. */
typedef struct {
    uint32_t polls;     /* the title's port-0 polls so far (g<poll>) */
    uint32_t flip;      /* the walker's flips (f<flip>); 0 on the HLE path */
    unsigned long ms;   /* pad_ms: host or guest milliseconds */
    int game_tap;       /* 1: the game's own XInputGetState, 0: the USB model */
} PadrecClocks;

/* ---- the pad thread (usb_gamepad.c) ------------------------------------ */

/* A session was ever started: the clocks are worth reading. One load. */
int padrec_any(void);

/* The start of a port-0 poll: a pending session is swapped in, a stop is
 * acted on, a finished playback ends. Once per poll; the second call of
 * one poll (the record after the press) does nothing. One atomic load when
 * no session was ever started. */
void padrec_sync(const PadrecClocks *c);

/* A playback's steps on pad's report (the 20-byte one): the host's state on
 * the pads the session names is zeroed first, so a resting thumb or a mouse
 * look never mixes into the replay (R3). Nothing without a playback. */
void padrec_press(int pad, uint8_t *report, const PadrecClocks *c);

/* A record's step for pad's report as the title got it. */
void padrec_feed(int pad, const uint8_t *report, const PadrecClocks *c);

/* The pads a playback names, pending ones included, so the game's tap
 * presses them (usb_gamepad_game_pads) and the USB model plugs them in. */
uint32_t padrec_pads(void);

/* 1 while a playback runs: the title gets the replay, not a mod's capture
 * (xbox_enhance_pad_seen) nor the host. */
int padrec_playing(void);

/* 1 when file opens matter to a session: one is active or pending, or
 * pad_anchor is set (start_open). One load each, for every guest open. */
int padrec_wants_opens(void);

/* A guest open: the last pad_anchor match (lock-free, start_open), and
 * under the pad lock (the caller holds it) the playback's anchors and the
 * record's anchor lines. Refusals and waits go to log. */
void padrec_open_name(const char *path);
void padrec_file_open(const char *path, const PadrecClocks *c, char *log, size_t logn);

/* ---- the menu thread --------------------------------------------------- */

/* What a record's header says that the toolkit cannot know. Any may be
 * NULL or empty. */
typedef struct {
    const char *game, *title, *toolkit, *game_build;
    uint32_t title_id;
    const char *name, *start_point, *notes;
} PadrecMeta;

/* Flags for padrec_record_start and padrec_play. */
#define PADREC_ARMED 1u      /* wait for padrec_release (the menu closing) */

/* Return 0, or -1 with a reason in why (for a toast). */
int padrec_record_start(const char *path, const PadrecMeta *meta, unsigned flags,
                        char *why, size_t whyn);
int padrec_play(const char *const *paths, int n, unsigned flags, char *why, size_t whyn);
/* The last playback again, from the next padrec_release (it does not
 * rewind the game: the player goes back to the start point first). */
int padrec_again(unsigned flags, char *why, size_t whyn);
/* The running title (id, name), set before a play: padrec_play refuses a
 * file whose header names another title_id. 0 checks nothing. */
void padrec_set_title(uint32_t title_id, const char *title);
/* A recording was renamed (to its new path) or deleted (to NULL): the
 * last playback follows, so padrec_again plays it under its new name or
 * says nothing was played. */
void padrec_path_moved(const char *from, const char *to);
/* Stop whatever runs or waits: a record ends with @end. */
void padrec_stop(void);
void padrec_record_stop(void);
/* The menu has closed: an ARMED session starts at the next poll. */
void padrec_release(void);

/* padrec.clock from config.toml (auto, poll, flip), from the menu's
 * config read; without it RECOMP_PADREC_CLOCK alone. The string must live
 * for the run. */
void padrec_set_clock_key(const char *v);

/* The env key that drives the pad, or NULL. */
const char *padrec_env_driven(void);

enum {
    PADREC_IDLE = 0,
    PADREC_REC,
    PADREC_PLAY,
    PADREC_ARMED_REC,   /* waits for the menu to close */
    PADREC_ARMED_PLAY,
    PADREC_WAIT,        /* a playback waits on a late anchor */
    PADREC_ENDED,       /* a playback ran to its end */
    PADREC_STOPPED      /* stopped, or a record saved */
};

#define PADREC_NAME_MAX 64
#define PADREC_WARN_MAX 96

typedef struct {
    uint32_t serial;            /* bumped by each session; 0 none yet */
    int state;
    int clock;                  /* 1 flip, 2 poll, 0 milliseconds */
    uint32_t count;             /* the session's count now */
    uint32_t total;             /* a playback's last count (its @end) */
    uint32_t ms;                /* host ms since the session started */
    uint32_t end_ms;            /* pad ms when it ended (ENDED, STOPPED) */
    int step, nsteps;           /* a playback's steps started, and all */
    int full;                   /* the record reached PAD_STEPS_MAX */
    char name[PADREC_NAME_MAX];
    char warn[PADREC_WARN_MAX];
} PadrecStatus;

void padrec_status(PadrecStatus *out);

/* The last pad_anchor open's name, "" before any (for start_open). */
void padrec_last_open(char *out, size_t n);

/* ---- for tests ----------------------------------------------------------- */

/* The session's start clock pick, for the [PADREC] line and its test:
 * "poll" or "flip", and why. */
const char *padrec_clock_pick(int game_tap, uint32_t flips, const char **why);
/* Drop every session and status (single-threaded tests). */
void padrec_reset_for_test(void);

#endif /* XBOX_USB_PAD_SESSION_H */
