/*
 * The sleeping spin wait (xbox_SpinWait / xbox_SpinWake, kernel_pacing.c).
 *
 * A lowered guest loop calls RECOMP_SPIN_WAIT(va) on each pass whose
 * condition is still true. At "spin" the wait does nothing; at "sleep" it
 * blocks until the kernel signals or 1 ms passes. Checked here:
 *  - at spin the macro never reaches the wait, and a signal is free;
 *  - with no signal, a wait ends by its timeout, about 1 ms;
 *  - a signal between the loop's read and its wait is not lost: the wait
 *    returns at once;
 *  - a thread at DISPATCH_LEVEL never sleeps (the DPC it waits for could not
 *    run);
 *  - every wait is counted as exactly one of immediate, woken, timed out or
 *    skipped at DISPATCH, per site;
 *  - one signal wakes two waiters;
 *  - a loop's exit is counted by how its last pass returned: a burst of
 *    passes, a pause longer than 0.5 ms, and the next wait counts the burst
 *    as one exit on a wake or on the timeout; passes back to back are not
 *    exits.
 */
#include "kernel.h"
#include "kernel_pacing.h"
#include "xbox_memory_layout.h"
#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

/* As templates/runtime/recomp_types.h defines it (that header is the
 * generated code's, and too heavy to pull in here). */
#define RECOMP_SPIN_WAIT(va) \
    do { if (g_xbox_spin_sleep | g_xbox_irq_pending | g_xbox_guest_cpu_waiters) xbox_SpinWait(va); } while (0)

#define MS 1000000ull

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static XboxSpinSiteStats stats(uint32_t va)
{
    XboxSpinSiteStats s = {0};
    xbox_SpinSiteStats(va, &s);
    return s;
}

static int balanced(const XboxSpinSiteStats *s)
{
    return s->waits == s->immediate + s->wakes + s->timeouts + s->dispatch;
}

static void at_spin(void)
{
    uint64_t t0;
    int i;

    check(xbox_PacingMode() == XBOX_PACING_SPIN && !g_xbox_spin_sleep,
          "spin is the default");
    t0 = xbox_HostNowNs();
    for (i = 0; i < 100000; i++)
        RECOMP_SPIN_WAIT(0x1000u);
    xbox_SpinWake();
    check(!xbox_SpinSiteStats(0x1000u, &(XboxSpinSiteStats){0}),
          "spin: the macro never calls the wait");
    check(xbox_HostNowNs() - t0 < 50 * MS, "spin: 10^5 passes cost nothing");
}

static void timeout_bound(void)
{
    uint64_t t0, dt;
    XboxSpinSiteStats s;

    xbox_SpinStatsReset();
    xbox_SpinWait(0x2000u);              /* catch up with any old signal */
    t0 = xbox_HostNowNs();
    xbox_SpinWait(0x2000u);
    dt = xbox_HostNowNs() - t0;
    s = stats(0x2000u);
    check(s.timeouts >= 1, "no signal: the wait times out");
    /* At least most of the 1 ms; at most a host timer tick or two (Wine
     * rounds a timed condition wait up to its scheduler's tick). */
    check(dt >= MS / 2 && dt < 40 * MS, "no signal: it lasts about 1 ms");
    printf("    (timed-out wait: %.2f ms)\n", (double)dt / MS);
}

static void no_lost_wake(void)
{
    uint64_t t0, dt;
    XboxSpinSiteStats s;
    int i, slow = 0;

    xbox_SpinStatsReset();
    for (i = 0; i < 200; i++) {
        xbox_SpinWait(0x3000u);          /* the loop has seen the generation */
        xbox_SpinWake();                 /* the writer, before the next wait */
        t0 = xbox_HostNowNs();
        xbox_SpinWait(0x3000u);
        dt = xbox_HostNowNs() - t0;
        slow += dt > MS / 2;
    }
    s = stats(0x3000u);
    check(s.immediate >= 200 && slow == 0,
          "a signal before the wait: it returns at once, every time");
}

static void dispatch_skip(void)
{
    uint64_t t0, dt;
    XboxSpinSiteStats s;
    int saved, i;

    xbox_SpinStatsReset();
    /* No guest memory here: keep the IRQL out of the (absent) KPCR. */
    g_fs_base = 0;
    saved = xbox_IrqlEnterInterrupt(DISPATCH_LEVEL);
    check(xbox_IrqlThisThreadBlocksDpcs(), "the thread is at DISPATCH_LEVEL");
    t0 = xbox_HostNowNs();
    for (i = 0; i < 100; i++)
        xbox_SpinWait(0x4000u);
    dt = xbox_HostNowNs() - t0;
    xbox_IrqlLeaveInterrupt(saved);
    s = stats(0x4000u);
    check(s.dispatch == 100 && s.timeouts == 0 && s.wakes == 0,
          "at DISPATCH: never sleeps, counted as dispatch");
    check(dt < 20 * MS, "at DISPATCH: 100 waits take no time");
    check(!xbox_IrqlThisThreadBlocksDpcs(), "back below DISPATCH_LEVEL");
}

/* Two threads parked in the same loop, one signal for both. */
static volatile LONG s_go, s_ready;

static DWORD WINAPI waiter(LPVOID p)
{
    uint32_t va = (uint32_t)(uintptr_t)p;

    xbox_SpinWait(va);
    InterlockedIncrement(&s_ready);
    while (!s_go)
        xbox_SpinWait(va);
    return 0;
}

static void two_waiters(void)
{
    const int trials = 50;
    XboxSpinSiteStats a, b;
    int t, joined = 1;

    xbox_SpinStatsReset();
    for (t = 0; t < trials; t++) {
        HANDLE h[2];
        s_go = 0;
        s_ready = 0;
        h[0] = CreateThread(NULL, 0, waiter, (LPVOID)(uintptr_t)0x5000u, 0, NULL);
        h[1] = CreateThread(NULL, 0, waiter, (LPVOID)(uintptr_t)0x5004u, 0, NULL);
        while (s_ready < 2)
            Sleep(0);
        xbox_HostSleepNs(MS / 4);        /* both inside a wait, mostly */
        InterlockedExchange(&s_go, 1);
        xbox_SpinWake();
        joined &= WaitForMultipleObjects(2, h, TRUE, 2000) == WAIT_OBJECT_0;
        CloseHandle(h[0]);
        CloseHandle(h[1]);
    }
    a = stats(0x5000u);
    b = stats(0x5004u);
    check(joined, "two waiters: both leave after the signal");
    /* A waiter that times out just before the signal leaves on its own;
     * most leave on the wake. */
    check(a.wakes >= trials / 2 && b.wakes >= trials / 2,
          "two waiters: one signal wakes both");
    check(balanced(&a) && balanced(&b), "two waiters: counters add up");
    printf("    (wakes %ld/%ld, timeouts %ld/%ld)\n", a.wakes, b.wakes,
           a.timeouts, b.timeouts);
}

static void counters(void)
{
    XboxSpinSiteStats s;

    xbox_SpinStatsReset();
    xbox_SpinWait(0x6000u);
    xbox_SpinWake();
    xbox_SpinWait(0x6000u);              /* immediate */
    xbox_SpinWait(0x6000u);              /* timeout */
    s = stats(0x6000u);
    check(s.va == 0x6000u && s.waits == 3 && balanced(&s) && s.immediate >= 1
          && s.timeouts >= 1, "counters: per site, and they add up");
    check(!xbox_SpinSiteStats(0x7000u, &s), "counters: an unseen site has none");
    xbox_SpinStatsReset();
    check(!xbox_SpinSiteStats(0x6000u, &s), "counters: reset clears them");
}

static void exits(void)
{
    XboxSpinSiteStats s;
    int i;

    xbox_SpinStatsReset();
    /* Burst 1: some timed-out passes, then a signal ends the loop. */
    xbox_SpinWait(0x8000u);
    for (i = 0; i < 3; i++)
        xbox_SpinWait(0x8000u);          /* timeouts, back to back */
    xbox_SpinWake();
    xbox_SpinWait(0x8000u);              /* the last pass: immediate */
    s = stats(0x8000u);
    check(s.exit_wake == 0 && s.exit_timeout == 0,
          "exits: none counted while the loop runs");
    xbox_HostSleepNs(3 * MS);            /* the game's frame work */
    /* Burst 2: the loop leaves on its timeout. */
    xbox_SpinWait(0x8000u);
    s = stats(0x8000u);
    check(s.exit_wake == 1 && s.exit_timeout == 0,
          "exits: a burst that ended on a signal is a wake exit");
    xbox_SpinWait(0x8000u);              /* timeout, the last pass */
    xbox_HostSleepNs(3 * MS);
    xbox_SpinWait(0x8004u);              /* another loop on this thread */
    s = stats(0x8000u);
    check(s.exit_wake == 1 && s.exit_timeout == 1,
          "exits: a burst that ended on the timeout is a timeout exit");
    check(s.timeouts >= 4 && balanced(&s),
          "exits: mid-loop timeouts stay counted as timeouts");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("spin_wait:\n");
    at_spin();
    xbox_PacingSetMode(XBOX_PACING_SLEEP);
    check(g_xbox_spin_sleep && xbox_PacingMode() == XBOX_PACING_SLEEP,
          "sleep mode set");
    timeout_bound();
    no_lost_wake();
    dispatch_skip();
    counters();
    exits();
    two_waiters();
    printf(failures ? "FAILED (%d)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
