/*
 * An env chain (RECOMP_PAD_SCRIPT=@head.pad;@tail.pad) behind a
 * game's own XInputGetState: the chain is played by the session code
 * (pad_session.c), so its @open anchors must see the game's file opens as
 * a menu playback's do. They went to the plain script's anchors only, and
 * a head anchored on an open waited until it gave up. Its own binary,
 * since the env is read once per process.
 *
 *  - an open before the first pad poll is taken: the head waits in the
 *    session code's pending slot until that poll, yet a boot head counts
 *    from 0, as a plain script does;
 *  - the head's next step waits for its open, however late;
 *  - after usb_gamepad_file_open it presses on the anchor's clock;
 *  - the head ends and the tail plays its own step.
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

#define HEAD_PATH "usb_pad_game_env_head.pad"
#define TAIL_PATH "usb_pad_game_env_tail.pad"

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-64s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

static XBOX_GAMEPAD poll(void)
{
    XBOX_GAMEPAD g;
    memset(&g, 0, sizeof g);
    usb_gamepad_game_press(0, &g);
    usb_gamepad_game_record(0, &g);
    return g;
}

static int put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fputs(text, f);
    fclose(f);
    return 0;
}

int main(void)
{
    XBOX_GAMEPAD got;
    int i, waited = 1, b_at = -1, x_seen = 0, early_at = -1;

    unsetenv("RECOMP_DEBUG");
    unsetenv("RECOMP_PAD_PRESS");
    unsetenv("RECOMP_PAD_LIVE");
    unsetenv("RECOMP_PAD_RECORD");
    printf("usb_pad_game_env_chain\n");

    /* The head as an exported report's: a boot-origin wait on an open
     * that comes before the first poll (A two polls on), then a wait on a
     * late open (B two polls on). The tail, a setup recorded in a session,
     * presses X: the chain moved on. */
    check(put(HEAD_PATH, "# xbr-export: pad 1\ng0:@open:early.bin\ng2:a:2\n"
                         "g10:@open:x.bin\ng12:b:3\ng20:@end\n") == 0,
          "the head is written");
    check(put(TAIL_PATH, "# xbr-export: pad 1\n# xbr-pad: origin = \"session\"\n"
                         "# xbr-pad: clock = \"poll\"\ng0:x:2\ng5:@end\n") == 0,
          "the tail is written");
    setenv("RECOMP_PAD_SCRIPT", "@" HEAD_PATH ";@" TAIL_PATH, 1);

    /* The first open reads the env and publishes the chain; no poll yet. */
    usb_gamepad_file_open("d:\\media\\early.bin", 0);
    check(padrec_any(), "the env chain is a session");
    for (i = 0; i < 10 && early_at < 0; i++)
        if (poll().bAnalogButtons[XBOX_BUTTON_A] == 0xFF)
            early_at = i;
    check(early_at == 2, "an open before the first poll: A at poll 2");
    for (i = 0; i < 60; i++) {
        got = poll();
        waited &= got.bAnalogButtons[XBOX_BUTTON_B] == 0
               && got.bAnalogButtons[XBOX_BUTTON_X] == 0;
    }
    check(waited, "the head's step waits for the open");
    /* An open that failed is no anchor; one that did not match neither. */
    usb_gamepad_file_open("d:\\data\\x.bin", 0xC0000034u);
    usb_gamepad_file_open("d:\\data\\y.bin", 0);
    for (i = 0; i < 10; i++)
        waited &= poll().bAnalogButtons[XBOX_BUTTON_B] == 0;
    check(waited, "a failed or other open does not fire it");
    usb_gamepad_file_open("d:\\data\\x.bin", 0);
    for (i = 0; i < 10 && b_at < 0; i++) {
        got = poll();
        if (got.bAnalogButtons[XBOX_BUTTON_B] == 0xFF)
            b_at = i;
    }
    check(b_at == 2, "B two polls after the open (the anchor's clock)");
    for (i = 0; i < 40 && !x_seen; i++)
        x_seen = poll().bAnalogButtons[XBOX_BUTTON_X] == 0xFF;
    check(x_seen, "the tail plays after the head's @end");

    padrec_stop();
    poll();
    padrec_reset_for_test();
    remove(HEAD_PATH);
    remove(TAIL_PATH);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
