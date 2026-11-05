/*
 * The toolkit's pad script and pad record behind a game's own XInputGetState.
 *
 * BLiNX 2 answers the title's XInputGetState in its own layer, so the USB
 * model's report, where RECOMP_PAD_SCRIPT presses and
 * RECOMP_DEBUG=pad_record records, never runs for it: a recording wrote no
 * file at all. usb_gamepad_game_press and usb_gamepad_game_record
 * give such a game both. These checks drive them as the game's poll does,
 * one flip at a time: the script's presses land on the game's state at their
 * flips, the record writes the state the title got, and that record, parsed
 * back as a script, presses the same state at every flip. Also: two polls in
 * one flip merge, a flip the walker skips is no poll, a step still held at
 * the end is written by the exit flush, and a flip counted between a poll's
 * press and its record does not lengthen the hold.
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

#define REC_PATH "usb_pad_game_rec.txt"
#define FLIPS 80
#define TWO_POLLS 50   /* polled twice: B on pad 1's second poll only */
#define SKIPPED   60   /* never polled: the walker counted two at once */
#define BOUNDARY  14   /* the script's A's last flip: the walker counts one
                          between pad 1's press and its record */
#define HELD_END  75   /* pad 1's X from here to the end: only the exit
                          flush writes it */

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-60s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* What the game's own sources hold at a flip: a host press on pad 1, a
 * stick on pad 2, Y on pad 2 across the skipped flip and X on pad 1 to the
 * end. The script adds its presses on top. */
static void game_sources(int pad, uint32_t f, XBOX_GAMEPAD *g)
{
    memset(g, 0, sizeof *g);
    if (pad == 0 && f >= 30 && f < 34)
        g->wButtons = XBOX_GAMEPAD_DPAD_UP;
    if (pad == 1 && f >= 40 && f < 50)
        g->sThumbLX = 20000;
    if (pad == 1 && f >= SKIPPED - 2 && f <= SKIPPED + 2)
        g->bAnalogButtons[3] = 0xFF;   /* Y */
    if (pad == 0 && f >= HELD_END)
        g->bAnalogButtons[2] = 0xFF;   /* X */
}

/* One poll of one pad, as the game's XInputGetState makes it. With bump, the
 * walker counts the next flip between the press and the record, as the
 * render thread can while the game thread is inside its poll. */
static void poll(int pad, uint32_t fl, XBOX_GAMEPAD *g, int bump)
{
    game_sources(pad, fl, g);
    usb_gamepad_game_press(pad, g);
    if (bump)
        usb_gamepad_flip(fl + 1);
    usb_gamepad_game_record(pad, g);
}

static int same_pad(const XBOX_GAMEPAD *a, const XBOX_GAMEPAD *b)
{
    int i;
    if (a->wButtons != b->wButtons || a->sThumbLX != b->sThumbLX
        || a->sThumbLY != b->sThumbLY || a->sThumbRX != b->sThumbRX
        || a->sThumbRY != b->sThumbRY)
        return 0;
    /* The record keeps an analog button as pressed or not, and a script
     * presses it fully: compare them that way. */
    for (i = 0; i < 8; i++)
        if ((a->bAnalogButtons[i] > XBOX_ANALOG_BUTTON_THRESHOLD)
            != (b->bAnalogButtons[i] > XBOX_ANALOG_BUTTON_THRESHOLD))
            return 0;
    return 1;
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
    static XBOX_GAMEPAD seen[FLIPS][2];
    static PadStep steps[256];
    static char text[8192];
    XBOX_GAMEPAD g;
    uint32_t fl;
    int pad, nsteps, i, replay_ok = 1, a_ok = 1, start_ok = 1;

    remove(REC_PATH);
    /* Set before the first environment read, which caches. */
    setenv("RECOMP_DEBUG",
           "pad_record=" REC_PATH ",pad_script=f10:a:5,f20:p2-start+b:3", 1);

    printf("usb_pad_game\n");

    check(usb_gamepad_game_pads() == 3u, "the script's pads are reported (1 and 2)");

    for (fl = 0; fl < FLIPS; fl++) {
        if (fl == SKIPPED) {
            /* No poll saw this flip: the title holds what it last read. */
            seen[fl][0] = seen[fl - 1][0];
            seen[fl][1] = seen[fl - 1][1];
            continue;
        }
        usb_gamepad_flip(fl);
        for (pad = 0; pad < 2; pad++) {
            poll(pad, fl, &g, fl == BOUNDARY && pad == 0);
            seen[fl][pad] = g;
        }
        if (fl == TWO_POLLS) {
            /* A second poll in the same flip, now with B on pad 2: the
             * flip's steps hold what either poll saw. */
            game_sources(1, fl, &g);
            g.bAnalogButtons[1] = 0xFF;
            usb_gamepad_game_press(1, &g);
            usb_gamepad_game_record(1, &g);
            seen[fl][1].bAnalogButtons[1] = 0xFF;
        }
        {
            int a = seen[fl][0].bAnalogButtons[0] == 0xFF;
            int st = (seen[fl][1].wButtons & XBOX_GAMEPAD_START) != 0
                     && seen[fl][1].bAnalogButtons[1] == 0xFF;
            a_ok &= a == (fl >= 10 && fl < 15);
            start_ok &= st == (fl >= 20 && fl < 23);
        }
    }
    check(a_ok, "pad 1: the script's A, flips 10-14 only");
    check(start_ok, "pad 2: the script's start+b, flips 20-22 only");
    check(seen[31][0].wButtons == XBOX_GAMEPAD_DPAD_UP
          && seen[31][0].bAnalogButtons[0] == 0,
          "the game's own press passes through");

    if (!read_record(text, sizeof text))
        return 1;
    check(strstr(text, "f75:") == NULL, "the step held to the end waits for the exit");
    /* What a window's ExitProcess runs (and atexit, which the test skips). */
    recomp_exit_hook_run("test");
    check(read_record(text, sizeof text), "the record file is written");
    printf("%s", text);
    check(strstr(text, "f10:a:5") != NULL,
          "the script's A is recorded at its flip, not lengthened by the bump");
    check(strstr(text, "f75:x:5") != NULL, "the exit flush writes the held step");
    check(strstr(text, "f50:p2-b:1") != NULL, "two polls in one flip merge");
    check(strstr(text, "f58:p2-y:5") != NULL, "a skipped flip keeps the hold");
    check(strstr(text, "f30:up:4") != NULL, "the game's press is recorded");
    check(strstr(text, "p2-") != NULL, "pad 2's steps are recorded");

    /* The record, read back as a script, presses what the title saw. */
    nsteps = pad_script_parse(text, steps, 0, 256);
    check(nsteps >= 4, "the record parses as a pad script");
    for (fl = 0; fl < FLIPS; fl++) {
        for (pad = 0; pad < 2; pad++) {
            uint8_t r[20];
            XBOX_GAMEPAD want = seen[fl][pad], got;
            memset(r, 0, sizeof r);
            for (i = 0; i < nsteps; i++)
                if (steps[i].pad == pad && pad_step_active(&steps[i], 0, fl))
                    pad_step_press(&steps[i], r);
            memset(&got, 0, sizeof got);
            got.wButtons = (WORD)(r[2] | (r[3] << 8));
            for (i = 0; i < 8; i++)
                got.bAnalogButtons[i] = r[4 + i];
            got.sThumbLX = (SHORT)(uint16_t)(r[12] | (r[13] << 8));
            got.sThumbLY = (SHORT)(uint16_t)(r[14] | (r[15] << 8));
            got.sThumbRX = (SHORT)(uint16_t)(r[16] | (r[17] << 8));
            got.sThumbRY = (SHORT)(uint16_t)(r[18] | (r[19] << 8));
            if (!same_pad(&want, &got)) {
                if (replay_ok)
                    printf("  first mismatch: flip %u pad %d\n", (unsigned)fl, pad);
                replay_ok = 0;
            }
        }
    }
    check(replay_ok, "the record replays to the same state at every flip");

    remove(REC_PATH);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
