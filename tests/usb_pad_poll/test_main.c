/*
 * The pad record and pad script on the title's poll clock.
 *
 * BLiNX 2 polls port 0 about once a flip, but not in step with the walker's
 * flips: two polls can share a flip and the next flip get none. A record
 * timed by flip kept one of the two polls' sticks, and its replay gave both
 * polls one value, so a stick that moves every poll replayed a poll off, and
 * a frame-exact wall clip missed. On the poll clock a step is timed by the
 * title's port-0 polls and holds the exact stick and analog values, so the
 * replay gives every poll what the record saw, however the flips fall.
 *
 * Each run goes in a child process, since RECOMP_DEBUG=pad_quit ends it:
 *  - the poll run: the child polls as the game does, with two polls in
 *    every third flip, writes what each poll gave the title, and stands in
 *    for the window: pad_quit posts the quit to it (recomp_quit_set), and it
 *    leaves with exit(0) as a closed window does. The script's @end holds
 *    the quit past its last release. The parent reads the record back as a
 *    script and checks it presses what the title saw at every poll;
 *  - the control: the same hand recorded on the flip clock, whose replay
 *    must miss some poll, or the poll run proves nothing;
 *  - the headless quit: no window to post to and a script without @end
 *    (a record from before it): pad_quit ends the run on a thread of its
 *    own, with 0, once the last step has ended.
 */
#include "pad_script.h"
#include "recomp_exit_hook.h"
#include "usb_gamepad.h"
#include "xinput_xbox.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Supplied by recompiled game code in a real build; nothing here calls one. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define REC_PATH   "usb_pad_poll_rec.txt"
#define FLIP_PATH  "usb_pad_poll_flip.txt"
#define HEAD_PATH  "usb_pad_poll_head.txt"
#define SEEN_PATH  "usb_pad_poll_seen.bin"
#define POLLS 120     /* the child's polls */
#define LAST  60      /* the script's last step ends here */
#define END   64      /* and its @end here */

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-60s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* The person's hand at poll p: a stick that moves a little every poll (less
 * than the flip record's band), and a trigger pressed lightly. */
static void game_sources(uint32_t p, XBOX_GAMEPAD *g)
{
    memset(g, 0, sizeof *g);
    if (p >= 20 && p < 50) {
        g->sThumbLX = (SHORT)(15000 + 37 * (int)p);
        g->sThumbLY = (SHORT)(31000 - 11 * (int)(p % 7));
    }
    if (p >= 30 && p < 34)
        g->bAnalogButtons[6] = (BYTE)(40 + p);   /* L, half pressed */
}

/* The flip poll p reads: two polls share every third flip. */
static uint32_t flip_of(uint32_t p)
{
    return p - p / 3;
}

static volatile int s_quit_posted;

static void window_close(const char *why)
{
    (void)why;
    s_quit_posted = 1;
}

/* mode 0: the poll run, a window taking the quit; 1: the flip control,
 * which ends on its own; 2: the headless quit. */
static void child(int mode)
{
    FILE *seen = mode == 0 ? fopen(SEEN_PATH, "wb") : NULL;
    XBOX_GAMEPAD g;
    uint32_t p, polls = mode == 2 ? 4000 : POLLS;

    if (mode == 0 && !seen)
        _exit(2);
    if (mode == 0)
        recomp_quit_set(window_close);
    for (p = 0; p < polls; p++) {
        uint32_t f = flip_of(p);
        if (!p || f != flip_of(p - 1))
            usb_gamepad_flip(f);
        game_sources(p, &g);
        usb_gamepad_game_press(0, &g);
        /* The window's loop: the close comes before the title sees more. */
        if (s_quit_posted) {
            if (seen)
                fclose(seen);
            exit(0);
        }
        usb_gamepad_game_record(0, &g);
        if (seen) {
            fwrite(&g, sizeof g, 1, seen);
            fflush(seen);
        }
        if (mode == 2)
            usleep(1000);   /* the quit's own thread gets 4 s */
    }
    if (mode == 1)
        exit(0);            /* atexit writes the held steps */
    _exit(3);               /* pad_quit never came */
}

static int run(int mode, const char *debug)
{
    pid_t pid;
    int status = 0;

    setenv("RECOMP_DEBUG", debug, 1);
    fflush(stdout);
    pid = fork();
    if (pid == 0)
        child(mode);
    if (pid < 0 || waitpid(pid, &status, 0) != pid)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static size_t slurp(const char *path, char *text, size_t size)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    text[0] = 0;
    if (!f)
        return 0;
    n = fread(text, 1, size - 1, f);
    fclose(f);
    text[n] = 0;
    return n;
}

/* Replay a record as a script against what the title saw: the first poll
 * that differs, or -1. clock 2 reads poll p at p, 1 at its flip. */
static long replay_miss(const char *text, const XBOX_GAMEPAD *seen, size_t npolls,
                        int *nsteps_out, int *clock_out)
{
    static PadStep steps[1024];
    static PadAnchors anchors;
    int nsteps, i;
    uint32_t p;

    memset(&anchors, 0, sizeof anchors);
    nsteps = pad_script_parse_anchored(text, steps, 0, 1024, &anchors);
    *nsteps_out = nsteps;
    *clock_out = anchors.clock;
    for (p = 0; p < npolls; p++) {
        uint8_t r[20];
        XBOX_GAMEPAD got;
        uint32_t at = anchors.clock == 2 ? p : flip_of(p);
        memset(r, 0, sizeof r);
        for (i = 0; i < nsteps; i++)
            if (pad_step_active_at(&steps[i], &anchors, 0, at))
                pad_step_press(&steps[i], r);
        memset(&got, 0, sizeof got);
        got.wButtons = (WORD)(r[2] | (r[3] << 8));
        for (i = 0; i < 8; i++)
            got.bAnalogButtons[i] = r[4 + i];
        got.sThumbLX = (SHORT)(uint16_t)(r[12] | (r[13] << 8));
        got.sThumbLY = (SHORT)(uint16_t)(r[14] | (r[15] << 8));
        got.sThumbRX = (SHORT)(uint16_t)(r[16] | (r[17] << 8));
        got.sThumbRY = (SHORT)(uint16_t)(r[18] | (r[19] << 8));
        if (memcmp(&got, &seen[p], sizeof got) != 0)
            return (long)p;
    }
    return -1;
}

int main(void)
{
    static XBOX_GAMEPAD seen[POLLS];
    static char text[16384];
    FILE *f;
    size_t npolls;
    int rc, script_ok, nsteps, clock;
    long miss;
    uint32_t p;

    remove(REC_PATH);
    remove(FLIP_PATH);
    remove(HEAD_PATH);
    remove(SEEN_PATH);
    printf("usb_pad_poll\n");

    /* The poll run. The script presses A and a light R on the poll clock,
     * through the shared flips, and ends at g64:@end. */
    rc = run(0, "pad_clock=poll,pad_quit,pad_record=" REC_PATH
                ",pad_script=g10:a:4,g55:rt=97:5,g64:@end");
    check(rc == 0, "pad_quit ends the run with 0 through the window");

    f = fopen(SEEN_PATH, "rb");
    if (!f)
        return 1;
    npolls = fread(seen, sizeof seen[0], POLLS, f);
    fclose(f);
    /* The quit comes in the press of the @end's poll. */
    check(npolls == END, "the run quits at the script's @end, not its last release");

    script_ok = seen[10].bAnalogButtons[0] == 0xFF && seen[13].bAnalogButtons[0] == 0xFF
                && seen[9].bAnalogButtons[0] == 0 && seen[14].bAnalogButtons[0] == 0
                && seen[55].bAnalogButtons[7] == 97 && seen[59].bAnalogButtons[7] == 97;
    check(script_ok, "g steps press at their polls, R at depth 97");

    slurp(REC_PATH, text, sizeof text);
    printf("%s", text);
    check(strstr(text, "g10:a:4") != NULL, "the record is on the poll clock");
    check(strstr(text, "f1") == NULL && strstr(text, "f2") == NULL,
          "no flip-timed step in a poll record");
    check(strstr(text, "g55:rt=97:5") != NULL, "the record keeps the analog depth");
    check(strstr(text, "g64:@end\n") != NULL, "the record ends with @end where it stopped");

    miss = replay_miss(text, seen, npolls, &nsteps, &clock);
    check(nsteps > 30 && clock == 2, "the record parses as a poll script");
    if (miss >= 0)
        printf("  first mismatch: poll %ld\n", miss);
    check(miss < 0, "the record replays every poll exactly");

    /* The control: the same hand, no script, recorded on the flip clock,
     * loses a poll's stick where two polls share a flip. */
    {
        static XBOX_GAMEPAD hand[POLLS];
        for (p = 0; p < POLLS; p++)
            game_sources(p, &hand[p]);
        rc = run(1, "pad_record=" FLIP_PATH);
        check(rc == 0, "the flip control runs");
        slurp(FLIP_PATH, text, sizeof text);
        miss = replay_miss(text, hand, POLLS, &nsteps, &clock);
        check(clock == 1 && nsteps > 0, "the control records on the flip clock");
        check(miss >= 0, "the flip clock's replay misses a poll (the control)");
        if (miss >= 0)
            printf("  the flip record's first miss: poll %ld\n", miss);
    }

    /* The headless quit, for a script with no @end. */
    rc = run(2, "pad_clock=poll,pad_quit,pad_record=" HEAD_PATH
                ",pad_script=g10:a:4,g55:rt=97:5");
    check(rc == 0, "without a window pad_quit ends the run itself, with 0");
    slurp(HEAD_PATH, text, sizeof text);
    {
        const char *e = strstr(text, ":@end\n");
        unsigned long at = 0;
        if (e) {
            while (e > text && e[-1] != '\n')
                e--;
            at = strtoul(e + 1, NULL, 10);
        }
        check(e && e[0] == 'g' && at >= LAST,
              "a script without @end quits after its last release");
    }

    remove(REC_PATH);
    remove(FLIP_PATH);
    remove(HEAD_PATH);
    remove(SEEN_PATH);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
