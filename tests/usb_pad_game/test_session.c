/*
 * A pad session (pad-recording-ui R2, R3) behind a game's own
 * XInputGetState, with no env key: the path BLiNX 2's cat_pad_get_state
 * takes. Its own binary, since the runtime reads RECOMP_DEBUG once per
 * process and the env keys would win over a session.
 *
 *  - a record started from the menu writes the state the title got, not
 *    the host's: a stretch where the title got an idle pad records idle,
 *    though the host held X. The test zeroes that pad itself, as the
 *    glue's input is after the pad filter. The filter's neutral pad lives
 *    in the enhancements layer, which this build leaves out; input_actions
 *    checks it;
 *  - its playback: usb_gamepad_game_pads() names port 0 as soon as it is
 *    pending, so the game's glue presses it; the host's own presses never
 *    reach the title; the record's A comes back at its polls;
 *  - a playback with an anchor waits for usb_gamepad_file_open, then
 *    presses its step on the anchor's clock.
 */
#include "pad_session.h"
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

#define REC_PATH "usb_pad_game_session.pad"
#define ANCHOR_PATH "usb_pad_game_anchor.pad"

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-64s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

static int state(void)
{
    PadrecStatus st;
    padrec_status(&st);
    return st.state;
}

/* One port-0 poll as the game makes it. host: what the host's pad holds;
 * neutral: the title gets an idle pad (what a filter that hides the host,
 * the menu's say, leaves). The state the title got is returned. */
static XBOX_GAMEPAD poll(const XBOX_GAMEPAD *host, int neutral)
{
    XBOX_GAMEPAD g;
    if (neutral || !host)
        memset(&g, 0, sizeof g);
    else
        g = *host;
    usb_gamepad_game_press(0, &g);
    usb_gamepad_game_record(0, &g);
    return g;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (!b) {
        fclose(f);
        return NULL;
    }
    n = (long)fread(b, 1, (size_t)n, f);
    b[n] = 0;
    fclose(f);
    return b;
}

int main(void)
{
    XBOX_GAMEPAD host, got;
    const char *one[1];
    char why[160], *txt;
    int i, a_ok = 1, host_out = 1, waited = 1, b_at = -1;
    FILE *f;

    remove(REC_PATH);
    remove(ANCHOR_PATH);
    unsetenv("RECOMP_DEBUG");
    unsetenv("RECOMP_PAD_SCRIPT");
    unsetenv("RECOMP_PAD_PRESS");
    unsetenv("RECOMP_PAD_LIVE");
    printf("usb_pad_game_session\n");

    check(usb_gamepad_game_pads() == 0u, "no session: no pads pressed");
    for (i = 0; i < 100; i++)
        poll(NULL, 0);

    /* The record: A for 5 polls, then X held while the title gets idle. */
    check(padrec_record_start(REC_PATH, NULL, 0, why, sizeof why) == 0, "a record starts");
    memset(&host, 0, sizeof host);
    host.bAnalogButtons[XBOX_BUTTON_A] = 0xFF;
    for (i = 0; i < 5; i++)
        poll(&host, 0);
    memset(&host, 0, sizeof host);
    host.bAnalogButtons[XBOX_BUTTON_X] = 0xFF;
    for (i = 0; i < 5; i++)
        poll(&host, 1);
    padrec_record_stop();
    poll(NULL, 0);
    check(state() == PADREC_STOPPED, "the record stops at the next poll");
    txt = slurp(REC_PATH);
    check(txt && strstr(txt, "\ng0:a:5") != NULL, "the host's A at g0, as the title got it");
    check(txt && strstr(txt, ":x") == NULL, "the title's idle pad records idle, not the host's X");
    check(txt && strstr(txt, "@end") != NULL, "the record ends with @end");

    /* Its playback: pending already names port 0, the host's B is dropped. */
    one[0] = REC_PATH;
    check(padrec_play(one, 1, 0, why, sizeof why) == 0, "the record plays");
    check((usb_gamepad_game_pads() & 1u) != 0, "a pending playback's port is named");
    memset(&host, 0, sizeof host);
    host.bAnalogButtons[XBOX_BUTTON_B] = 0xFF;
    host.sThumbLX = 12000;
    for (i = 0; i < 5; i++) {
        got = poll(&host, 0);
        a_ok &= got.bAnalogButtons[XBOX_BUTTON_A] == 0xFF;
        host_out &= got.bAnalogButtons[XBOX_BUTTON_B] == 0 && got.sThumbLX == 0;
    }
    check(padrec_playing(), "playing");
    check(a_ok, "the record's A reaches the title for its 5 polls");
    check(host_out, "the host's B and stick never reach the title");
    for (i = 0; i < 20 && state() != PADREC_ENDED; i++)
        poll(&host, 0);
    check(state() == PADREC_ENDED, "the playback ends at @end");
    got = poll(&host, 0);
    check(got.bAnalogButtons[XBOX_BUTTON_B] == 0xFF, "after it, the host's pad again");

    /* An anchor: B 2 polls after x.bin opens, however late. The header is
     * the record's own. */
    f = fopen(ANCHOR_PATH, "wb");
    if (f && txt) {
        const char *p = txt;
        while (*p == '#') {
            const char *e = strchr(p, '\n');
            if (!e)
                break;
            fwrite(p, 1, (size_t)(e - p + 1), f);
            p = e + 1;
        }
        fputs("g0:@open:x.bin\ng2:b:3\ng10:@end\n", f);
    }
    if (f)
        fclose(f);
    free(txt);
    one[0] = ANCHOR_PATH;
    check(padrec_play(one, 1, 0, why, sizeof why) == 0, "the anchored file plays");
    for (i = 0; i < 30; i++) {
        got = poll(NULL, 0);
        waited &= got.bAnalogButtons[XBOX_BUTTON_B] == 0;
    }
    check(waited, "its step waits for the open");
    usb_gamepad_file_open("d:\\data\\x.bin", 0);
    for (i = 0; i < 10 && b_at < 0; i++) {
        got = poll(NULL, 0);
        if (got.bAnalogButtons[XBOX_BUTTON_B] == 0xFF)
            b_at = i;
    }
    check(b_at == 2, "B two polls after the open (the anchor's clock)");
    padrec_stop();
    poll(NULL, 0);
    check(!padrec_playing(), "stopped");

    padrec_reset_for_test();
    remove(REC_PATH);
    remove(ANCHOR_PATH);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
