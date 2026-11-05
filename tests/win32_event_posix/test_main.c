/*
 * Manual-reset events on the POSIX shim -- written to fail.
 *
 * Win32 and the NT kernel release every waiter of a manual-reset event at the
 * moment it is set. The shim's waiters wake later and re-read the state, so a
 * reset in between sent them back to sleep. XDK D3D resets the vblank event
 * before each wait, and with two threads on it one of them never ran.
 * w32_event_release_on_set restores the kernel's rule per event:
 *
 *   1. a set followed at once by a reset still releases a waiting thread
 *   2. so does PulseEvent
 *   3. a wait that starts after the set and reset still times out
 *   4. an unmarked event keeps its old behaviour for a fresh wait (state only)
 *   5. a multi-object wait sees a set followed by a reset between two polls
 *   6. a waiter that resets the event as it wakes does not rob a second one
 *   7. the mark leaves an auto-reset event alone: set+reset releases nobody
 */
#include "win32_compat.h"

#include <stdio.h>
#include <time.h>

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-58s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static HANDLE g_ev, g_other;
static volatile LONG g_result = -1;
static DWORD g_wait_ms = 2000;  /* bounded so a regression fails, not hangs */
static int g_multi;

static DWORD WINAPI waiter(LPVOID unused)
{
    (void)unused;
    if (g_multi) {
        HANDLE hs[2] = { g_other, g_ev };
        InterlockedExchange(&g_result, (LONG)WaitForMultipleObjectsEx(
            2, hs, FALSE, g_wait_ms, FALSE));
    } else {
        InterlockedExchange(&g_result,
                            (LONG)WaitForSingleObject(g_ev, g_wait_ms));
    }
    return 0;
}

/* Waits, then resets the event at once, as XDK D3D does before its next
 * vblank wait. */
static volatile LONG g_result_a = -1;
static DWORD WINAPI resetting_waiter(LPVOID unused)
{
    DWORD r;
    (void)unused;
    r = WaitForSingleObject(g_ev, g_wait_ms);
    if (r == WAIT_OBJECT_0)
        ResetEvent(g_ev);
    InterlockedExchange(&g_result_a, (LONG)r);
    return 0;
}

/* Starts a waiter, lets it block, then runs `kick` and reports what the
 * waiter's wait returned. */
static DWORD wait_through(int pulse)
{
    HANDLE t;
    g_result = -1;
    t = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
    sleep_ms(100);                                 /* the waiter is asleep */
    if (pulse) {
        PulseEvent(g_ev);
    } else {
        SetEvent(g_ev);
        ResetEvent(g_ev);                          /* before the waiter runs */
    }
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
    return (DWORD)g_result;
}

int main(void)
{
    g_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    w32_event_release_on_set(g_ev);

    check(wait_through(0) == WAIT_OBJECT_0, "set then reset releases a waiter",
          "the reset swallowed the set");
    check(wait_through(1) == WAIT_OBJECT_0, "PulseEvent releases a waiter",
          "the pulse was lost");
    check(WaitForSingleObject(g_ev, 50) == WAIT_TIMEOUT,
          "a wait begun after set+reset blocks", "an old set satisfied a new wait");

    /* The multi-object wait polls; set and reset land inside one gap. */
    g_other = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_multi = 1;
    check(wait_through(0) == WAIT_OBJECT_0 + 1,
          "multi-wait: set then reset releases a waiter",
          "the reset between polls swallowed the set");
    g_multi = 0;
    CloseHandle(g_other);

    {   /* Two waiters; A resets the event as soon as it wakes. */
        HANDLE ta, tb;
        g_result = -1;
        g_result_a = -1;
        ta = CreateThread(NULL, 0, resetting_waiter, NULL, 0, NULL);
        tb = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
        sleep_ms(100);
        SetEvent(g_ev);
        WaitForSingleObject(ta, 5000);
        WaitForSingleObject(tb, 5000);
        CloseHandle(ta);
        CloseHandle(tb);
        check(g_result_a == WAIT_OBJECT_0 && g_result == WAIT_OBJECT_0,
              "a waker's reset does not rob the second waiter",
              "the second waiter missed the set");
    }
    CloseHandle(g_ev);

    /* Auto-reset: the mark is for manual-reset events only, so a set taken
     * back before the waiter runs releases nobody, as before. */
    g_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    w32_event_release_on_set(g_ev);
    g_wait_ms = 300;
    check(wait_through(0) == WAIT_TIMEOUT,
          "auto-reset with the mark: set+reset still blocks",
          "the mark applied to an auto-reset event");
    g_wait_ms = 2000;
    CloseHandle(g_ev);

    g_ev = CreateEventA(NULL, TRUE, FALSE, NULL);  /* unmarked */
    SetEvent(g_ev);
    ResetEvent(g_ev);
    check(WaitForSingleObject(g_ev, 50) == WAIT_TIMEOUT,
          "unmarked: a reset event blocks a new wait", NULL);
    CloseHandle(g_ev);

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
