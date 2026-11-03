/*
 * Millisecond-timed pad script steps on the virtual clock.
 *
 * Under RECOMP_DEBUG=deterministic a step timed in milliseconds is read on
 * guest milliseconds (usb_gamepad.c, pad_ms), so it lands on the same vblank
 * in every run. Two things can go wrong there that the host clock hides: the
 * script's origin is taken at the first poll, which on the virtual clock can
 * read 0 ms -- an origin kept as "t0 == 0 means unset" would be taken again
 * at every poll and no later step would ever fire -- and a step's time falls
 * between vblanks, where it must wait for the first vblank at or past it.
 * The clock is stepped here by hand, one vblank per poll, as the timer
 * thread does under the mode.
 */
#include "guest_time.h"
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

#define VBLANKS 600            /* 10 s of virtual time */

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-60s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void)
{
    static int a_on[VBLANKS], b_on[VBLANKS], x_on[VBLANKS];
    static uint64_t ms[VBLANKS];
    int v, first_b = -1, first_x = -1, want_b = -1, want_x = -1;
    int a_ok = 1, b_ok = 1, x_ok = 1;

    /* Set before the first environment read, which caches. A from 0 ms for
     * 50 ms, B from 8000 ms (a vblank boundary: 480 * 1000/60) for 20 ms,
     * X from 8010 ms (between vblanks 480 and 481) for 10 ms. */
    setenv("RECOMP_DEBUG", "pad_script=0:a:50,8000:b:20,8010:x:10", 1);
    xbox_GuestTimeTestMode(1);

    printf("usb_pad_virtual\n");

    for (v = 0; v < VBLANKS; v++) {
        XBOX_GAMEPAD g;

        if (v)
            xbox_GuestTimeVblank();
        ms[v] = xbox_GuestTimeMs();
        memset(&g, 0, sizeof g);
        usb_gamepad_game_press(0, &g);
        a_on[v] = g.bAnalogButtons[0] == 0xFF;
        b_on[v] = g.bAnalogButtons[1] == 0xFF;
        x_on[v] = g.bAnalogButtons[2] == 0xFF;
        if (b_on[v] && first_b < 0)
            first_b = v;
        if (x_on[v] && first_x < 0)
            first_x = v;
        if (want_b < 0 && ms[v] >= 8000)
            want_b = v;
        if (want_x < 0 && ms[v] >= 8010)
            want_x = v;
    }

    check(ms[0] == 0 && ms[VBLANKS - 1] > 9900, "the clock reads 0 at the first poll");
    check(a_on[0], "a 0 ms step fires at the first poll (virtual 0)");
    for (v = 0; v < VBLANKS; v++) {
        a_ok &= a_on[v] == (ms[v] < 50);
        b_ok &= b_on[v] == (ms[v] >= 8000 && ms[v] < 8020);
        x_ok &= x_on[v] == (ms[v] >= 8010 && ms[v] < 8020);
    }
    printf("  B first at vblank %d (%llu ms), X at %d (%llu ms)\n",
           first_b, first_b >= 0 ? (unsigned long long)ms[first_b] : 0ULL,
           first_x, first_x >= 0 ? (unsigned long long)ms[first_x] : 0ULL);
    check(a_ok, "A held for its 50 ms of virtual time only");
    check(first_b == want_b && want_b == 480,
          "an 8000 ms step lands on the first vblank at or past 8000");
    check(first_x == want_x && want_x == 481,
          "an 8010 ms step waits for the next vblank (481)");
    check(b_ok && x_ok, "each step holds for its virtual milliseconds only");

    printf("usb_pad_virtual: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
