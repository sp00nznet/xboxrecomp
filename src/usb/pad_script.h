/*
 * pad_script.h -- the scripted pad's steps, and the recorder that writes them.
 *
 * Kept apart from usb_gamepad.c so the grammar and the recorder can be tested
 * without a USB stack: this file knows a 20-byte report and text, nothing else.
 *
 * A step is <at>:<button>[+<button>...][:<hold>], numbers in decimal. <at>
 * is milliseconds from the first pad report (the original form), or f<flip>:
 * the pushbuffer executor's flip count, the "[GPU] flip N" number, with the
 * hold in flips too (a trailing "f" on the hold is allowed). A millisecond
 * script lands wherever the host's pace has got to, so a fast host reaches a
 * screen hundreds of flips before a slow one at the same wall time. A
 * flip-timed one presses at the same flip count instead. That count is the
 * walker's, not the title's frame counter: on a steady screen a replay lands
 * within about 1-2 flips, but loading screens, movies and waits on the audio
 * clock take a different number of flips per host, so hold presses that
 * wait on a screen generously. The count only moves with the LLE pushbuffer
 * walker (nv2a_pb_exec.c calls usb_gamepad_flip); on the HLE D3D8 present
 * path it stays 0 and a flip-timed step fires at boot.
 *
 * A stick may also be set to a value: lx=<v>, ly, rx, ry, -32768..32767, up
 * positive. "#" starts a comment to the end of the line, for files.
 *
 * An anchor, f<flip>:@open:<text>[:<buttons>/<period>], ties the flip count
 * to a game event: in the record, a file opened at that flip whose name
 * starts with <text> (any case; a text holding \\ or / matches anywhere in
 * the path). A step that starts after it counts its flips from
 * the flip at which that open comes in the replay, and does not press
 * before it. A press that waited on a screen or a movie then lands on it
 * however long the load before it took. When the open is late, the replay
 * taps <buttons> for period/2 flips every <period> flips until it comes
 * (a movie that takes A only a few seconds in, by its decoder's clock).
 * After PAD_ANCHOR_GIVE_UP_FLIPS late the replay gives the open up and goes
 * on as recorded.
 *
 * g<poll> times a step by the title's own polls of its pads instead: the
 * number of port-0 reads so far (usb_gamepad.c counts each one
 * usb_gamepad_report or usb_gamepad_game_record makes), holds in polls.
 * The walker's flip runs beside the title's frame, not with it, so a poll
 * near a flip's edge falls on either side of it from run to run, and a
 * flip-timed replay hands some poll its neighbour's state; a frame-exact
 * route (a wall clip) then misses. A poll-timed step lands on the
 * same read of the pad in every run of a deterministic title. A script is
 * on one clock: the first timed entry's (f or g) sets it, and an entry on
 * the other is refused. Anchors count the same way, g<poll>:@open:<text>,
 * and so do their counts below: a retry's period and the give-up are polls
 * on the poll clock (about one a frame in BLiNX 2, so near the flips').
 *
 * An analog button may also carry its depth, a=<1..255> (lt=97: the
 * trigger 97 of 255 down); a bare name presses it fully.
 *
 * f<flip>:@end or g<poll>:@end marks where a record stopped: it presses
 * nothing and ends on its own count, so RECOMP_DEBUG=pad_quit waits for it
 * and a replay runs on through the idle time after the last release. A
 * record without one ends with its last step, as before.
 */
#ifndef XBOX_USB_PAD_SCRIPT_H
#define XBOX_USB_PAD_SCRIPT_H

#include <stddef.h>
#include <stdint.h>

/* A recorded route of a few minutes is a few hundred button steps, but an
 * analog stick held by hand moves most flips: a long stroll can pass ten
 * thousand steps. On the poll clock every stick change is a step, up to 60 a
 * second while the stick is off centre: room for about 18 minutes of it. */
#define PAD_SCRIPT_MAX 65536

/* An unheld flip-timed step lasts this many flips, the 150 ms default of the
 * millisecond form at 60 flips a second. */
#define PAD_HOLD_FLIPS_DEFAULT 9

typedef struct {
    unsigned long at, hold;  /* ms, or flips when by_flip */
    uint8_t digital;         /* report byte 2 */
    uint8_t analog;          /* bit n = report byte 4 + n */
    uint8_t pad;             /* which controller: "p2-a" is pad 1 */
    uint8_t stick;           /* full deflection: 1 L-left 2 L-right 4 L-up
                                8 L-down, then the same four for the right */
    uint8_t axis_set;        /* bit n: axis[n] (LX LY RX RY) is set */
    int16_t axis[4];
    uint8_t aval[8];         /* analog depth per bit of analog; 0 is full */
    uint8_t by_flip;         /* 1 f<flip>, 2 g<poll>: at and hold are on that
                                clock */
    uint16_t anchor;         /* 0, or 1 + the anchor it counts from */
    uint8_t end;             /* the record's end (@end): hold 0, no buttons */
    int announced;
} PadStep;

/* ---- anchors ------------------------------------------------------------ */

#define PAD_ANCHOR_MAX  256
#define PAD_ANCHOR_TEXT 64
/* A retry longer than this is two presses, not a tap. */
#define PAD_RETRY_MAX_FLIPS 300
/* The recorder writes a retry for this many presses of the same buttons,
 * spaced within PAD_RETRY_SLACK flips of each other. */
#define PAD_RETRY_PRESSES 3
#define PAD_RETRY_SLACK 2
/* An open this late (30 s at 60 flips/s) is not coming: a file the replay
 * never opens, or two opening in the other order. */
#define PAD_ANCHOR_GIVE_UP_FLIPS 1800
/* A late anchor's retry taps this many times, then only waits. */
#define PAD_RETRY_MAX_TAPS 8
/* "# no retry" only for presses this many spacings or fewer before the
 * open: earlier ones are a menu walk, not a wait for it. */
#define PAD_NO_RETRY_NEAR 3

typedef struct {
    char text[PAD_ANCHOR_TEXT];
    unsigned long at;        /* the open's flip in the record */
    unsigned long period;    /* the retry's, in flips; 0 for none */
    PadStep retry;           /* its buttons and pad */
    long long seen_flip;     /* the open's flip in this run (a given-up one's
                                may be before 0 or past now) */
    int seen;
    int logged;              /* 1 the wait, 2 the retry's cap */
    int skip;                /* given up, its skipped steps not yet counted */
    unsigned long skip_clock; /* the clock it was given up on */
} PadAnchor;

/* Sorted by at. Only `armed`, the first one not seen, takes an open: the
 * anchors match in the order the record saw them. */
typedef struct {
    PadAnchor a[PAD_ANCHOR_MAX];
    int n, armed;
    int skip;                /* some anchor's skip is set */
    uint8_t clock;           /* 0 none yet, else the script's by_flip */
} PadAnchors;

/* pad_script_parse, plus anchor entries into *anchors (kept sorted), and
 * each new step tied to its anchor. Without anchors (NULL) an anchor entry
 * is reported and skipped. A new anchor ties every step again, since it can
 * take steps from the one before it. */
int pad_script_parse_anchored(const char *text, PadStep *steps, int len,
                              int max, PadAnchors *anchors);

/* Tie steps[from..len) to the last anchor at or before their start. A step
 * that starts on an anchor's flip counts from it (the recorder moves an
 * anchor a flip on when a press began earlier in its flip). */
void pad_anchors_assign(const PadAnchors *anchors, PadStep *steps, int from, int len);

/* A file opened at flip: the armed anchor, if its text matches the path, is
 * seen. An open that matches a later anchor gives up the ones before it
 * and is seen: one on the armed one's record flip at any time, one up to
 * PAD_ANCHOR_GIVE_UP_FLIPS of record later once the armed one is late. A
 * re-open of the last seen anchor's file is never a later one. Returns the
 * index seen, or -1. Not thread-safe: the caller serialises the opens and
 * the other anchor calls. It does not read the script: the steps a give-up
 * skips are counted at the next pad_anchors_tick. The log goes to
 * log[0..logn) when log is set (the caller prints it once its lock is
 * free), else to stderr. */
int pad_anchors_open(PadAnchors *anchors, const char *path, uint32_t flip,
                     char *log, size_t logn);

/* Give up the armed anchor once PAD_ANCHOR_GIVE_UP_FLIPS late on the clock
 * it is due by: it is seen as if it came on time, its retry stops, and its
 * steps run on that clock. Later anchors that clock has passed go with it,
 * silently. Logs each give-up with the steps it skipped, and the steps an
 * open's give-up skipped since the last tick. Returns how many anchors it
 * gave up. steps[0..nsteps) are the script's; log as for
 * pad_anchors_open. */
int pad_anchors_tick(PadAnchors *anchors, const PadStep *steps, int nsteps,
                     uint32_t flip, char *log, size_t logn);

/* pad_step_active and pad_step_done on the step's anchor's clock: flip read
 * as flips since the open, from the record's flip of it. Before the open a
 * step is neither active nor done. */
int pad_step_active_at(const PadStep *st, const PadAnchors *anchors,
                       unsigned long t_ms, uint32_t flip);
int pad_step_done_at(const PadStep *st, const PadAnchors *anchors,
                     unsigned long t_ms, uint32_t flip);

/* The armed anchor's retry for this pad at flip: 1 and *out to press when
 * the open is late and the retry taps now. Logs the first late flip. */
int pad_anchor_retry(PadAnchors *anchors, int pad, uint32_t flip, PadStep *out,
                     char *log, size_t logn);

/* Parse text (comma or newline separated) and append its steps to
 * steps[len..max). Returns the new length. Bad entries are reported on
 * stderr; parsing stops at an entry without a time, and steps past max are
 * dropped with one warning. */
int pad_script_parse(const char *text, PadStep *steps, int len, int max);

/* Whether st holds at t ms from the first report and at the given flip. */
int pad_step_active(const PadStep *st, unsigned long t_ms, uint32_t flip);

/* Has a step run its course, so a live session can drop it. */
int pad_step_done(const PadStep *st, unsigned long t_ms, uint32_t flip);

/* A flip-timed step whose flip has passed starts at flip instead, so a line
 * appended live with an old time is pressed at once, not dropped. */
void pad_step_not_before(PadStep *st, uint32_t flip);

/* Press st's buttons into a 20-byte report. */
void pad_step_press(const PadStep *st, uint8_t *report);

/* ---- the recorder: RECOMP_DEBUG=pad_record=<file> ---------------------- */

/* What a report presses, in the script's terms: analog buttons and triggers
 * past the XInput trigger threshold count as pressed (a script presses them
 * fully), and the sticks keep their values, so an analog walk, a keyboard
 * stick's ramp or a half-throw walk key replays as the title saw it. */
typedef struct {
    uint8_t digital, analog;
    int16_t ax[4];           /* LX LY RX RY */
    uint8_t av[8];           /* the analog bytes as read (report 4..11) */
} PadBits;

PadBits pad_bits_from_report(const uint8_t *report);

/* The button list a step would name: "start+a". Empty for no buttons. */
void pad_bits_name(PadBits b, char *out, size_t n);

/* The recorder keeps two tracks per pad, the buttons and the sticks, so a
 * stick moving under a held button does not split the button's step. The
 * reports within one flip merge: buttons pressed in any of them, the sticks
 * as the last one had them. */
typedef struct {
    PadBits cur;             /* what the open steps hold */
    uint32_t since, ax_since;
    PadBits pend;            /* the flip being gathered */
    uint32_t pend_flip;
    PadBits press[PAD_RETRY_PRESSES]; /* the last presses' buttons, newest last */
    uint32_t press_at[PAD_RETRY_PRESSES];
    int presses;             /* how many of them there are */
    int pad;
    int started;
    /* On the poll clock (g<poll> steps): every poll is its own step time,
     * so the record is exact too: an analog button is pressed from 1 and
     * written with its depth, and every stick change is a step. */
    int poll;
} PadRecorder;

/* Feed one report's state at a flip (a poll on the poll clock). When a flip's state differs from the
 * one before, the steps it ends are written to out, one per line
 * ("f<flip>:<buttons>:<flips>", "f<flip>:lx=<v>+ly=<v>:<flips>", "p2-" for
 * pads past the first). Returns the number of lines. A stick change smaller
 * than PAD_AXIS_BAND is not a new step (a resting hand's jitter, a pad's
 * off-centre rest), except onto rest and full throw. */
#define PAD_AXIS_BAND 256
int pad_recorder_feed(PadRecorder *r, PadBits b, uint32_t flip, char *out, size_t n);

/* Write the steps still held at the end of a run, as if released at flip
 * (or the flip after the last report, if that is later). */
int pad_recorder_end(PadRecorder *r, uint32_t flip, char *out, size_t n);

/* The record's end line, "f<flip>:@end\n" (g<poll> on the poll clock), at
 * flip or past every pad's last report, whichever is later; call it before
 * pad_recorder_end, which forgets them. Returns the line's length, 0 when
 * no pad reported (nothing was recorded) or out is too small. */
int pad_recorder_end_line(const PadRecorder *recs, int n, uint32_t flip,
                          char *out, size_t size);

/* An anchor line for a file that opened at flip, after the anchor at
 * `since` (0 for none): "f<flip>:@open:<name>[:<buttons>/<period>]\n", the
 * name being the path's file name. The retry comes from the first pad whose
 * newest PAD_RETRY_PRESSES presses came after `since` with the same
 * buttons, evenly spaced (within PAD_RETRY_SLACK flips, at most
 * PAD_RETRY_MAX_FLIPS), the open within one spacing of the last; a
 * "# retry" comment line before the anchor says which presses. Three
 * same-button presses that miss the rule get a "# no retry" line instead,
 * so a person can add one by hand. When a pad's flip under gathering
 * already holds a new press, the anchor goes one flip on, so that press
 * stays before it. Returns the line's length (0 when the name cannot be an
 * anchor: empty, too long, or holding ":,#" or a blank; -1 when out is too
 * small), and its flip in *at. */
int pad_recorder_anchor(const PadRecorder *recs, int n, const char *path,
                        uint32_t flip, uint32_t since, char *out, size_t size,
                        uint32_t *at);

/* The file name in a guest path ("d:\\movie\\x.sfd" -> "x.sfd"). */
const char *pad_path_name(const char *path);

#endif /* XBOX_USB_PAD_SCRIPT_H */
