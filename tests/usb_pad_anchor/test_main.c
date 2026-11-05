/*
 * Pad anchors (f<flip>:@open:<text>) behind a game's own XInputGetState.
 *
 * A pad record times every step by flip, so a press that waited on a game
 * event in the recording can miss in a replay where the event comes later
 * (a movie that takes a skip only once it plays). An anchor holds the steps after it
 * until the named file opens, and taps its retry while the open is late.
 * These checks drive the game tap one flip at a time as usb_pad_game does:
 * the script's anchored step waits for usb_gamepad_file_open, the retry
 * taps until the open and stops at it, a failed open is no open, and the
 * record of that run writes the anchor lines, with the retry it saw, at the
 * flips the opens came.
 */
#include "pad_script.h"
#include "recomp_exit_hook.h"
#include "usb_gamepad.h"
#include "xinput_xbox.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Supplied by recompiled game code in a real build; nothing here calls one. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define REC_PATH "usb_pad_anchor_rec.txt"
#define FLIPS 100
#define MOVIE_OPEN 60   /* 30 flips later than the script's anchor */
#define FAILED_OPEN 50  /* the movie's path, but the open failed */
#define MENU_OPEN 80

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-60s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int read_record(char *text, size_t max)
{
    FILE *f = fopen(REC_PATH, "rb");
    size_t n;

    if (!f)
        return 0;
    n = fread(text, 1, max - 1, f);
    fclose(f);
    text[n] = 0;
    return 1;
}

int main(void)
{
    static PadStep steps[64];
    static PadAnchors anchors;
    static char text[4096];
    static int a_at[FLIPS], b_at[FLIPS];
    XBOX_GAMEPAD g;
    uint32_t fl;
    int nsteps, i, found = 0, a_ok = 1, b_ok = 1;

    remove(REC_PATH);
    /* Set before the first environment read, which caches. The movie is
     * due at flip 30; B 2 flips after it; A every 10 while it is late. */
    setenv("RECOMP_DEBUG",
           "pad_record=" REC_PATH ",pad_anchor=movie|menu,"
           "pad_script=f5:a:2 f30:@open:movie.sfd:a/10 f32:b:3",
           1);

    printf("usb_pad_anchor\n");

    for (fl = 0; fl < FLIPS; fl++) {
        usb_gamepad_flip(fl);
        memset(&g, 0, sizeof g);
        usb_gamepad_game_press(0, &g);
        usb_gamepad_game_record(0, &g);
        a_at[fl] = g.bAnalogButtons[0] == 0xFF;
        b_at[fl] = g.bAnalogButtons[1] == 0xFF;
        /* Opens come after the flip's poll, on the game's loader thread. */
        if (fl == FAILED_OPEN)
            usb_gamepad_file_open("d:\\media\\movie.sfd", 0xC0000034u);
        if (fl == MOVIE_OPEN)
            usb_gamepad_file_open("d:\\media\\Movie.SFD", 0);
        if (fl == MENU_OPEN) {
            usb_gamepad_file_open("d:\\menu_tex.tmp", 0);
            /* A second open of the same file is one anchor. */
            usb_gamepad_file_open("d:\\menu_tex.tmp", 0);
        }
    }
    for (fl = 0; fl < FLIPS; fl++) {
        /* The script's own A at 5-6, then the retry from 35: 5 flips on,
         * 5 off, until the open at 60. */
        int want_a = (fl >= 5 && fl < 7)
                     || (fl >= 35 && fl <= MOVIE_OPEN && (fl - 35) % 10 < 5);
        a_ok &= a_at[fl] == want_a;
        b_ok &= b_at[fl] == (fl >= MOVIE_OPEN + 2 && fl < MOVIE_OPEN + 5);
    }
    check(a_ok, "the retry taps A while the open is late, and stops at it");
    check(b_ok, "the anchored B waits for the open, then lands 2 flips on");
    check(!b_at[32], "B does not press at its recorded flip");

    recomp_exit_hook_run("test");
    check(read_record(text, sizeof text), "the record file is written");
    printf("%s", text);
    check(strstr(text, "f60:@open:Movie.SFD:a/10\n") != NULL,
          "the movie's anchor, at its open, with the retry it saw");
    check(strstr(text, "f50:@open") == NULL, "a failed open is no anchor");
    check(strstr(text, "# retry a/10: pressed at f35 f45 f55") != NULL,
          "the retry's presses, in a comment before it");
    check(strstr(text, "f80:@open:menu_tex.tmp\n") != NULL,
          "the menu's anchor, no retry (the A's came before the movie)");
    {
        const char *t = strstr(text, "@open:menu");
        check(t && !strstr(t + 1, "@open:menu"),
              "a repeat open of the same file is one anchor");
    }
    check(strstr(text, "f62:b:3") != NULL, "the B is recorded where it landed");

    /* The record replays: B is under the movie's anchor. */
    memset(&anchors, 0, sizeof anchors);
    nsteps = pad_script_parse_anchored(text, steps, 0, 64, &anchors);
    check(anchors.n == 2, "the record parses with its two anchors");
    for (i = 0; i < nsteps; i++)
        if (steps[i].at == 62 && steps[i].analog == 0x02)
            found += steps[i].anchor == 1 ? 1 : 100;
    check(found == 1, "the recorded B is under the movie's anchor");

    remove(REC_PATH);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
