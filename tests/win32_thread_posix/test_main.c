/*
 * The Win32 thread shim on a POSIX host -- written to fail.
 *
 * pthreads cannot stop another thread mid-run, so the shim's SuspendThread
 * only counts. A thread that suspends *itself* is at a known point, though,
 * and Win32 parks it there until a ResumeThread. Worker pools idle that way
 * (NtSuspendThread on the current thread, resumed when work is queued); a
 * SuspendThread that returns at once turns the idle loop into a busy spin.
 *
 *   1. SuspendThread(own handle) does not return until ResumeThread
 *   2. the same through the current-thread pseudo-handle
 *   3. ResumeThread reports the count the park left (1)
 *
 * A thread another thread suspends cannot be stopped where it is, so it
 * parks at its next w32_suspend_point instead. JSRF's ADX lock suspends its
 * idle helper from the locker; without the park the helper yielded on the
 * guest core for the whole session natively.
 *
 *   4. w32_suspend_point returns at once while nobody suspended the thread
 *   5. a thread another thread suspended stops at the point
 *   6. ResumeThread starts it again, and the point reports that it parked
 *   7. a resume before the thread reaches its point: the point returns 0
 *   8. a nested suspend: the first ResumeThread (2) leaves it parked, the
 *      second (1) wakes it
 *   9. the host main thread has no shim object: the point returns 0
 *
 * Cases 7 and 8 hold the thread on an event until the suspends are in, so
 * the order does not depend on sleeps; a short wait only shows a thread
 * that should stay parked has not finished.
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

static volatile LONG g_past_suspend;
static volatile LONG g_use_pseudo;

static DWORD WINAPI parks_itself(LPVOID unused)
{
    (void)unused;
    SuspendThread(g_use_pseudo ? (HANDLE)(LONG_PTR)-2 : GetCurrentThread());
    InterlockedExchange(&g_past_suspend, 1);
    return 0;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void run(int pseudo, const char *what)
{
    DWORD tid, prev, w;
    HANDLE h;

    g_past_suspend = 0;
    g_use_pseudo = pseudo;
    h = CreateThread(NULL, 0, parks_itself, NULL, 0, &tid);
    check(h != NULL, "CreateThread", NULL);
    if (!h)
        return;
    sleep_ms(100);
    check(g_past_suspend == 0, what,
          "SuspendThread returned at once; an idle pool spins");
    prev = ResumeThread(h);
    check(prev == 1, "ResumeThread reports the parked count (1)", NULL);
    w = WaitForSingleObject(h, 5000);
    check(w == WAIT_OBJECT_0 && g_past_suspend == 1,
          "the thread runs on after ResumeThread", NULL);
    CloseHandle(h);
}

static volatile LONG g_spins, g_parked, g_stop;

static DWORD WINAPI spins_at_point(LPVOID unused)
{
    (void)unused;
    while (!g_stop) {
        InterlockedIncrement(&g_spins);
        if (w32_suspend_point())
            InterlockedIncrement(&g_parked);
        SwitchToThread();
    }
    return 0;
}

/* Polls instead of one fixed sleep, so a loaded host only makes it slower:
 * waits up to 5 s for the spin count to pass `than`. */
static int spins_pass(LONG than)
{
    for (int i = 0; i < 500; i++) {
        if (g_spins > than)
            return 1;
        sleep_ms(10);
    }
    return 0;
}

/* Waits up to 5 s for the spinner to stop: no step in 50 ms. */
static LONG spins_settle(void)
{
    LONG a = g_spins;
    for (int i = 0; i < 100; i++) {
        sleep_ms(50);
        if (g_spins == a)
            break;
        a = g_spins;
    }
    return a;
}

static void run_suspend_point(void)
{
    DWORD tid, w;
    LONG a, b;
    HANDLE h;

    g_spins = g_parked = g_stop = 0;
    h = CreateThread(NULL, 0, spins_at_point, NULL, 0, &tid);
    check(h != NULL, "CreateThread", NULL);
    if (!h)
        return;
    check(spins_pass(0) && spins_pass(g_spins) && g_parked == 0,
          "w32_suspend_point returns at once with no suspend", NULL);
    check(SuspendThread(h) == 0, "SuspendThread from another thread reports 0",
          NULL);
    a = spins_settle();
    sleep_ms(100);
    b = g_spins;
    check(a == b, "a suspended thread stops at its suspend point",
          "it kept spinning: the suspend only counted");
    check(ResumeThread(h) == 1, "ResumeThread reports the count (1)", NULL);
    check(spins_pass(b) && g_parked == 1,
          "it runs on after ResumeThread and reports the park", NULL);
    InterlockedExchange(&g_stop, 1);
    w = WaitForSingleObject(h, 5000);
    check(w == WAIT_OBJECT_0, "the thread exits", NULL);
    CloseHandle(h);
}

static HANDLE g_ready, g_go;
static volatile LONG g_point_ret, g_point_done;

/* Says it is running, waits for the go, then calls the point once. It must
 * be past its start first: a suspend before that holds it at the
 * trampoline's gate, as CREATE_SUSPENDED does, and never at the point. */
static DWORD WINAPI point_once(LPVOID unused)
{
    (void)unused;
    SetEvent(g_ready);
    WaitForSingleObject(g_go, INFINITE);
    InterlockedExchange(&g_point_ret, w32_suspend_point());
    InterlockedExchange(&g_point_done, 1);
    return 0;
}

static HANDLE start_point_once(void)
{
    HANDLE h;

    g_point_ret = -1;
    g_point_done = 0;
    g_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_go = CreateEventA(NULL, TRUE, FALSE, NULL);
    h = CreateThread(NULL, 0, point_once, NULL, 0, NULL);
    check(g_ready && g_go && h, "CreateThread", NULL);
    if (h && WaitForSingleObject(g_ready, 5000) != WAIT_OBJECT_0) {
        check(0, "the thread starts", NULL);
        return NULL;
    }
    return h;
}

static void run_resume_before_point(void)
{
    HANDLE h = start_point_once();
    DWORD w;

    if (!h)
        return;
    check(SuspendThread(h) == 0 && ResumeThread(h) == 1,
          "suspend then resume before the point (0, 1)", NULL);
    SetEvent(g_go);
    w = WaitForSingleObject(h, 5000);
    check(w == WAIT_OBJECT_0 && g_point_done && g_point_ret == 0,
          "a resume before the point: it returns 0 and runs on",
          "the point parked on a count already gone");
    CloseHandle(h);
    CloseHandle(g_go);
    CloseHandle(g_ready);
}

static void run_nested_suspend(void)
{
    HANDLE h = start_point_once();
    DWORD w;

    if (!h)
        return;
    check(SuspendThread(h) == 0 && SuspendThread(h) == 1,
          "two suspends count 0, then 1", NULL);
    SetEvent(g_go);
    w = WaitForSingleObject(h, 100);
    check(w == WAIT_TIMEOUT && !g_point_done,
          "a thread suspended twice parks at the point", NULL);
    check(ResumeThread(h) == 2, "the first ResumeThread reports 2", NULL);
    w = WaitForSingleObject(h, 100);
    check(w == WAIT_TIMEOUT && !g_point_done,
          "after one resume it stays parked (count 1)",
          "one ResumeThread woke a thread suspended twice");
    check(ResumeThread(h) == 1, "the second ResumeThread reports 1", NULL);
    w = WaitForSingleObject(h, 5000);
    check(w == WAIT_OBJECT_0 && g_point_done && g_point_ret == 1,
          "the second resume wakes it, and the point reports the park", NULL);
    CloseHandle(h);
    CloseHandle(g_go);
    CloseHandle(g_ready);
}

int main(void)
{
    printf("win32 thread shim (POSIX)\n");
    run(0, "SuspendThread(own handle) parks the thread");
    run(1, "SuspendThread(pseudo-handle) parks the thread");
    run_suspend_point();
    run_resume_before_point();
    run_nested_suspend();
    check(w32_suspend_point() == 0,
          "the host main thread (no shim object): the point returns 0", NULL);
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
