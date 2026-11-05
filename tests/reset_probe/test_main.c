/*
 * The stop probe (kernel_reset_probe.c, RECOMP_DEBUG=reset_probe): at a
 * listed flip, how long each guest thread takes to reach a point where a
 * reset could stop it. The cases:
 *  - the key's flips: N, A-B, A-B/S, comma-separated; junk lists none;
 *  - off (no key): the hooks track nothing and a flip opens no sample;
 *  - the lock off (Burnout 3): a thread at its loop back edges, one in a
 *    spin-wait loop and one in a kernel wait (stopped at 0) all stop, and
 *    one in a long host call with no stop point is reported not stopped;
 *    the preempt flag the sample raised is down again after it;
 *  - the lock on (BLiNX 2): the holder stops at a back edge and a thread
 *    queued on the lock counts as stopped;
 *  - a thread that ends during the sample counts as stopped.
 * A watchdog fails the test instead of hanging it.
 */
#include "kernel.h"
#include "kernel_pacing.h"
#include "kernel_reset_probe.h"
#include "recomp_env.h"
#include "platform/host_time.h"

#include <stdio.h>
#ifdef _WIN32
#include <mmsystem.h>   /* timeBeginPeriod */
#endif
#include <stdlib.h>

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int32_t preempt_flag(void)
{
    return (int32_t)InterlockedCompareExchange(
        (volatile LONG *)&g_xbox_guest_cpu_preempt, 0, 0);
}

/* As templates/runtime/recomp_types.h defines RECOMP_BACK_EDGE. */
static void back_edge(uint32_t va)
{
    if (preempt_flag())
        xbox_GuestCpuPreempt(va);
}

static void set_lock(const char *v)
{
    recomp_env_set(RENV_GUEST_LOCK, v);
    xbox_GuestCpuReloadConfig();
}

static DWORD WINAPI watchdog(LPVOID unused)
{
    (void)unused;
    Sleep(60000);
    fprintf(stderr, "reset_probe: watchdog: a case hung\n");
    fflush(stderr);
    _Exit(1);
    return 0;
}

static volatile LONG s_stop, s_up, s_stuck_go;

static DWORD WINAPI looper(LPVOID p)
{
    (void)p;
    xbox_GuestCpuJoin();
    InterlockedIncrement(&s_up);
    while (!s_stop)
        back_edge(0x1000);
    xbox_GuestCpuPart();
    return 0;
}

static DWORD WINAPI spinner(LPVOID p)
{
    (void)p;
    xbox_GuestCpuJoin();
    InterlockedIncrement(&s_up);
    while (!s_stop)
        xbox_SpinWait(0x2000);
    xbox_GuestCpuPart();
    return 0;
}

/* KeWaitForSingleObject as the bridge brackets it (kernel_thunk_dispatch). */
static DWORD WINAPI waiter(LPVOID p)
{
    int held;
    (void)p;
    xbox_GuestCpuJoin();
    xbox_ResetProbeState(XBOX_RESET_IN_BRIDGE, 159);
    held = xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 159);
    InterlockedIncrement(&s_up);
    while (!s_stop)
        Sleep(1);
    xbox_ResetProbeState(XBOX_RESET_RUN, 0);
    if (held)
        xbox_GuestCpuAcquire();
    xbox_GuestCpuPart();
    return 0;
}

/* A host call outside any bridge (a host read, say) that outlasts the
 * probe's window: the case a reset cannot stop. */
static DWORD WINAPI stuck(LPVOID p)
{
    (void)p;
    xbox_GuestCpuJoin();
    InterlockedIncrement(&s_up);
    while (!s_stuck_go)
        back_edge(0x3000);
    Sleep(1500);
    while (!s_stop)
        back_edge(0x3000);
    xbox_GuestCpuPart();
    return 0;
}

static DWORD WINAPI ender(LPVOID p)
{
    (void)p;
    xbox_GuestCpuJoin();
    InterlockedIncrement(&s_up);
    /* No stop point until its end: it ends at about 50 ms into the sample. */
    while (!s_stuck_go)
        Sleep(0);
    Sleep(50);
    xbox_GuestCpuPart();
    return 0;
}

static void wait_up(LONG n)
{
    uint64_t t0 = xbox_HostNowNs();
    while (InterlockedCompareExchange(&s_up, 0, 0) < n
           && xbox_HostNowNs() - t0 < 5000000000ull)
        Sleep(1);
}

static void join_all(HANDLE *t, int n)
{
    int i;
    InterlockedExchange(&s_stop, 1);
    for (i = 0; i < n; i++) {
        WaitForSingleObject(t[i], 20000);
        CloseHandle(t[i]);
    }
    InterlockedExchange(&s_stop, 0);
    InterlockedExchange(&s_up, 0);
    InterlockedExchange(&s_stuck_go, 0);
}

static void parser(void)
{
    const char *r = "300-6000/300";
    check(xbox_ResetProbeListed(r, 300) && xbox_ResetProbeListed(r, 600)
          && xbox_ResetProbeListed(r, 6000),
          "key: A-B/S lists A, A+S and B");
    check(!xbox_ResetProbeListed(r, 450) && !xbox_ResetProbeListed(r, 6300)
          && !xbox_ResetProbeListed(r, 0),
          "key: and nothing between, past B or before A");
    check(xbox_ResetProbeListed("5, 7,10-12", 7) && xbox_ResetProbeListed("5,7,10-12", 11)
          && !xbox_ResetProbeListed("5,7,10-12", 9),
          "key: single flips and a range, comma-separated");
    check(!xbox_ResetProbeListed("x", 0) && !xbox_ResetProbeListed("9-3", 5)
          && !xbox_ResetProbeListed("1-9/0", 1),
          "key: junk, a reversed range and a zero step list nothing");
}

static void off(void)
{
    HANDLE t = CreateThread(NULL, 0, looper, NULL, 0, NULL);
    wait_up(1);
    check(!xbox_ResetProbeTracking() && !g_xbox_reset_probe_track,
          "off: no key, no tracking");
    /* The walker's own guard is recomp_env(RENV_RESET_PROBE); past it the
     * flip finds no flips listed either. */
    xbox_ResetProbeFlip(1);
    check(!g_xbox_reset_probe, "off: a flip opens no sample");
    join_all(&t, 1);
}

static void lock_off(void)
{
    XboxResetProbeSample s;
    HANDLE t[4];
    int ok;

    set_lock(NULL);
    t[0] = CreateThread(NULL, 0, looper, NULL, 0, NULL);
    t[1] = CreateThread(NULL, 0, spinner, NULL, 0, NULL);
    t[2] = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
    wait_up(3);
    ok = xbox_ResetProbeSampleNow(100, &s);
    check(ok && s.threads == 3 && s.stopped == 3,
          "lock off: back edge, spin pass and a wait: all three stop");
    check(ok && s.max_ns < 100000000ull && s.late_100ms == 0,
          "lock off: within 100 ms");
    check(!preempt_flag() && !g_xbox_reset_probe,
          "lock off: the flag and the preempt flag are down after it");

    t[3] = CreateThread(NULL, 0, stuck, NULL, 0, NULL);
    wait_up(4);
    InterlockedExchange(&s_stuck_go, 1);
    Sleep(20);
    ok = xbox_ResetProbeSampleNow(200, &s);
    check(ok && s.threads == 4 && s.stopped == 3,
          "lock off: a long host call is reported not stopped");
    join_all(t, 4);
}

static void lock_on(void)
{
    XboxResetProbeSample s;
    HANDLE t[2];
    int ok;

    set_lock("1");
    t[0] = CreateThread(NULL, 0, looper, NULL, 0, NULL);
    t[1] = CreateThread(NULL, 0, looper, NULL, 0, NULL);
    wait_up(1);
    Sleep(20);
    ok = xbox_ResetProbeSampleNow(300, &s);
    check(ok && s.threads == 2 && s.stopped == 2 && s.late_100ms == 0,
          "lock on: the holder and the queued thread stop in 100 ms");
    join_all(t, 2);
    set_lock(NULL);
}

static void ends_in_sample(void)
{
    XboxResetProbeSample s;
    HANDLE t;
    int ok;

    t = CreateThread(NULL, 0, ender, NULL, 0, NULL);
    wait_up(1);
    InterlockedExchange(&s_stuck_go, 1);
    ok = xbox_ResetProbeSampleNow(400, &s);
    check(ok && s.threads == 1 && s.stopped == 1,
          "a thread that ends during the sample counts as stopped");
    join_all(&t, 1);
}

int main(void)
{
    HANDLE wd;

#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    wd = CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    CloseHandle(wd);

    printf("reset_probe\n");
    parser();
    off();
    /* The key is read once, at the first join after it is set: the off
     * case above read it unset, so reset what it cached. */
    recomp_env_set(RENV_RESET_PROBE, "1-9/1");
    xbox_ResetProbeReloadConfig();
    lock_off();
    lock_on();
    ends_in_sample();

    if (failures) {
        printf("reset_probe: %d failure(s)\n", failures);
        return 1;
    }
    printf("reset_probe: all passed\n");
    return 0;
}
