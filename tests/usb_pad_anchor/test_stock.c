/*
 * Without a pad script or record, a file open does nothing: no pad comes
 * on the bus and nothing is pressed. (With no RECOMP_PAD_RECORD there is
 * no record path to write.) Its own binary, since the runtime reads
 * RECOMP_DEBUG once per process.
 */
#include "usb_gamepad.h"
#include "xinput_xbox.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-60s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void)
{
    static const uint8_t zero[sizeof(XBOX_GAMEPAD)];
    XBOX_GAMEPAD g;
    uint32_t fl;
    int pressed = 0;

    unsetenv("RECOMP_DEBUG");
    unsetenv("RECOMP_PAD_SCRIPT");
    unsetenv("RECOMP_PAD_RECORD");
    unsetenv("RECOMP_PAD_LIVE");
    unsetenv("RECOMP_PAD_PRESS");
    printf("usb_pad_anchor_stock\n");
    for (fl = 0; fl < 50; fl++) {
        usb_gamepad_flip(fl);
        if (fl == 10)
            usb_gamepad_file_open("d:\\media\\movie.sfd", 0);
        memset(&g, 0, sizeof g);
        usb_gamepad_game_press(0, &g);
        usb_gamepad_game_record(0, &g);
        pressed |= memcmp(&g, zero, sizeof g) != 0;
    }
    check(!pressed, "nothing pressed");
    check(usb_gamepad_game_pads() == 0, "no pad on the bus");
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
