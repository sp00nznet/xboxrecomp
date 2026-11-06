/* win32_waitable_timer_posix: SetWaitableTimer's period on the POSIX shim.
 *
 * A synchronization timer armed with a period must expire again one period
 * later, every time its waiter takes an expiry; the shim used to treat every
 * timer as one-shot, so the second wait blocked forever. A cancelled timer
 * stops, and a timer with no period still fires once. */
#include "win32_compat.h"

#include <stdio.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(void)
{
    int fails = 0, i;
    LARGE_INTEGER due;
    HANDLE h, once;
    double t0, el;

    due.QuadPart = -10LL * 10000;               /* 10 ms, relative */

    h = CreateWaitableTimerW(NULL, FALSE, NULL);
    SetWaitableTimer(h, &due, 10, NULL, NULL, FALSE);
    t0 = now_ms();
    for (i = 0; i < 5; i++)
        if (WaitForSingleObject(h, 200) != WAIT_OBJECT_0) {
            printf("FAIL: periodic wait %d timed out\n", i);
            fails++;
        }
    el = now_ms() - t0;
    if (el < 40 || el > 150) {
        printf("FAIL: 5 periods of 10 ms took %.1f ms\n", el);
        fails++;
    }
    CancelWaitableTimer(h);
    if (WaitForSingleObject(h, 50) != WAIT_TIMEOUT) {
        printf("FAIL: a cancelled timer still fires\n");
        fails++;
    }

    once = CreateWaitableTimerW(NULL, FALSE, NULL);
    SetWaitableTimer(once, &due, 0, NULL, NULL, FALSE);
    if (WaitForSingleObject(once, 200) != WAIT_OBJECT_0) {
        printf("FAIL: one-shot timer did not fire\n");
        fails++;
    }
    if (WaitForSingleObject(once, 50) != WAIT_TIMEOUT) {
        printf("FAIL: one-shot timer fired twice\n");
        fails++;
    }

    if (!fails)
        printf("win32_waitable_timer_posix: all checks passed\n");
    return fails != 0;
}
