/*
 * The guest CPU (kernel_guest_cpu.c, RECOMP_GUEST_LOCK=1): one guest thread
 * runs guest code at a time, handed over in FIFO order at kernel calls and
 * at loop back edges past the quantum.
 *
 * A title's level loader and its loader thread can add entries to one
 * global pool with a read-modify-write the console's one CPU never
 * interleaved. The cases, each ordered by events or by the lock's own waiter count rather
 * than run many times:
 *  - off: every call is a no-op, and two guest threads run guest code at
 *    once (the race, recorded);
 *  - off: four threads' unlocked read-modify-writes lose updates (the bug,
 *    recorded); on: none lost, with yields and kernel-call releases between
 *    the iterations;
 *  - a yield (NtYieldExecution) with three waiters serves them oldest
 *    first and the yielder last;
 *  - a kernel call that may block lets a waiter run, and the caller takes
 *    the CPU back only when the waiter has let go; one that returns at once
 *    switches nobody, even with a waiter queued; past the quantum (here 0)
 *    any kernel call hands over, as NtYieldExecution does;
 *  - a bridge-run callback on a guest thread takes the CPU the call let go
 *    of; a host thread's does not;
 *  - the counters: holds, waits, yields, the longest hold and where it ended.
 * A watchdog fails the test instead of hanging it.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;

/* The flag is only ever accessed atomically (kernel.h). */
static int32_t preempt_flag(void)
{
    return (int32_t)InterlockedCompareExchange(
        (volatile LONG *)&g_xbox_guest_cpu_preempt, 0, 0);
}

static void preempt_flag_set(int32_t v)
{
    InterlockedExchange((volatile LONG *)&g_xbox_guest_cpu_preempt, (LONG)v);
}

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void set_env(const char *name, const char *v)
{
#ifdef _WIN32
    _putenv_s(name, v ? v : "");
#else
    if (v)
        setenv(name, v, 1);
    else
        unsetenv(name);
#endif
    xbox_GuestCpuReloadConfig();
}

static void set_lock(const char *v)
{
    set_env("RECOMP_GUEST_LOCK", v);
}

/* The kernel thunk's NtYieldExecution: a hand-off whatever the quantum. */
static void yield_cpu(void)
{
    if (xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 238))
        xbox_GuestCpuAcquire();
}

static void join(HANDLE t)
{
    WaitForSingleObject(t, 20000);
    CloseHandle(t);
}

/* Wait for the guest CPU's queue to reach n waiters; 0 on timeout. */
static int wait_waiting(int n, int ms)
{
    uint64_t t0 = xbox_HostNowNs();
    while (xbox_GuestCpuWaiting() != n) {
        if (xbox_HostNowNs() - t0 > (uint64_t)ms * 1000000ull)
            return 0;
        Sleep(1);
    }
    return 1;
}

/* Wait for a shared word to reach a value; 0 on timeout. */
static int wait_for(volatile LONG *w, LONG v, int ms)
{
    uint64_t t0 = xbox_HostNowNs();
    while (InterlockedCompareExchange(w, 0, 0) != v) {
        if (xbox_HostNowNs() - t0 > (uint64_t)ms * 1000000ull)
            return 0;
        Sleep(0);
    }
    return 1;
}

static DWORD WINAPI watchdog(LPVOID unused)
{
    (void)unused;
    Sleep(60000);
    fprintf(stderr, "guest_cpu: watchdog: a case hung\n");
    fflush(stderr);
    _Exit(1);
    return 0;
}

/* ── Two guest threads in guest code at once (the lock off) ───────── */

static volatile LONG s_in[2], s_saw_other[2];

static DWORD WINAPI overlap_thread(LPVOID p)
{
    int me = (int)(intptr_t)p, other = 1 - me;
    uint64_t t0 = xbox_HostNowNs();

    xbox_GuestCpuJoin();
    InterlockedExchange(&s_in[me], 1);
    /* Guest code that waits for the other thread's guest code: with the
     * lock on and no yield this would never end. */
    while (!s_in[other] && xbox_HostNowNs() - t0 < 2000000000ull)
        Sleep(0);
    InterlockedExchange(&s_saw_other[me], s_in[other]);
    xbox_GuestCpuPart();
    return 0;
}

static void switched_off(void)
{
    HANDLE a, b;

    set_lock(NULL);
    check(!xbox_GuestCpuOn(), "off: the default is off");
    xbox_GuestCpuJoin();
    check(!xbox_GuestCpuHeld() && xbox_GuestCpuIsGuestThread(),
          "off: a join marks the thread and takes nothing");
    check(xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 1) == 0,
          "off: a release has nothing to let go of");
    xbox_GuestCpuAcquire();
    yield_cpu();
    check(!xbox_GuestCpuHeld() && xbox_GuestCpuEnterCallback() == 0,
          "off: acquire, yield and a callback entry are no-ops");
    xbox_GuestCpuPart();

    a = CreateThread(NULL, 0, overlap_thread, (LPVOID)0, 0, NULL);
    b = CreateThread(NULL, 0, overlap_thread, (LPVOID)1, 0, NULL);
    join(a);
    join(b);
    check(s_saw_other[0] && s_saw_other[1],
          "off: two guest threads run guest code at once (the race, recorded)");
}

/* ── The read-modify-write ────────────────────────────────────────── */

#define RMW_THREADS 4
#define RMW_ITERS   60000
static volatile LONG s_shared;
static volatile LONG s_rmw_started;

static void rmw_delay(void)
{
    /* The lifted add is a load, an add and a store; a few dozen host
     * instructions between them is the window a host tick lands in. */
    volatile int k;
    for (k = 0; k < 40; k++) ;
}

static DWORD WINAPI rmw_thread(LPVOID p)
{
    int i;

    (void)p;
    xbox_GuestCpuJoin();
    InterlockedIncrement(&s_rmw_started);
    for (i = 0; i < RMW_ITERS; i++) {
        LONG v = s_shared;                  /* the guest's unlocked RMW */
        rmw_delay();
        s_shared = v + 1;
        /* Between two RMWs, not inside one: a yield is a preemption point,
         * and an RMW across one is the title's own bug on the console too. */
        if ((i & 15) == 15)
            yield_cpu();
        if ((i & 63) == 63) {               /* a kernel call */
            int held = xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 99);
            Sleep(0);
            if (held)
                xbox_GuestCpuAcquire();
        }
    }
    xbox_GuestCpuPart();
    return 0;
}

static LONG run_rmw(void)
{
    HANDLE t[RMW_THREADS];
    int i;

    s_shared = 0;
    s_rmw_started = 0;
    for (i = 0; i < RMW_THREADS; i++)
        t[i] = CreateThread(NULL, 0, rmw_thread, NULL, 0, NULL);
    for (i = 0; i < RMW_THREADS; i++)
        join(t[i]);
    return s_shared;
}

static void exclusion(void)
{
    LONG off, on;
    XboxGuestCpuStats st;

    set_lock(NULL);
    off = run_rmw();
    printf("    lock off: %ld of %d updates kept\n", (long)off,
           RMW_THREADS * RMW_ITERS);
    check(off <= RMW_THREADS * RMW_ITERS,
          "off: the count never exceeds the updates (lost ones recorded above)");

    set_lock("1");
    xbox_GuestCpuStatsGet(&st, 1);
    on = run_rmw();
    xbox_GuestCpuStatsGet(&st, 0);
    check(on == RMW_THREADS * RMW_ITERS,
          "on: four threads' unlocked RMWs lose nothing");
    check(st.turns > 0 && st.waits > 0,
          "on: the threads waited and the holder yielded to them");
}

/* ── FIFO ─────────────────────────────────────────────────────────── */

static volatile LONG s_order[8], s_seq;

static DWORD WINAPI waiter_thread(LPVOID p)
{
    LONG me = (LONG)(intptr_t)p;
    LONG i;

    xbox_GuestCpuJoin();
    i = InterlockedIncrement(&s_seq) - 1;
    s_order[i] = me;
    xbox_GuestCpuPart();
    return 0;
}

static void fifo(void)
{
    HANDLE t[3];
    LONG i;
    int queued = 1;

    set_lock("1");
    s_seq = 0;
    memset((void *)s_order, 0, sizeof s_order);
    xbox_GuestCpuJoin();
    for (i = 0; i < 3; i++) {
        t[i] = CreateThread(NULL, 0, waiter_thread, (LPVOID)(intptr_t)(i + 1), 0, NULL);
        /* Each one queued before the next starts, so the order is known. */
        queued &= wait_waiting(i + 1, 5000);
    }
    check(queued, "fifo: three waiters queued in order");
    yield_cpu();
    i = InterlockedIncrement(&s_seq) - 1;
    s_order[i] = 9;
    check(s_order[0] == 1 && s_order[1] == 2 && s_order[2] == 3 && s_order[3] == 9,
          "fifo: a yield serves the oldest waiter first, the yielder last");
    check(xbox_GuestCpuWaiting() == 0, "fifo: nobody left waiting");
    xbox_GuestCpuPart();
    for (i = 0; i < 3; i++)
        join(t[i]);
}

/* ── A kernel call ────────────────────────────────────────────────── */

static volatile LONG s_kc_ran, s_kc_let_go;

static DWORD WINAPI kernel_call_waiter(LPVOID p)
{
    (void)p;
    xbox_GuestCpuJoin();
    InterlockedExchange(&s_kc_ran, 1);
    Sleep(20);                              /* holds it a while */
    InterlockedExchange(&s_kc_let_go, 1);
    xbox_GuestCpuPart();
    return 0;
}

static void kernel_call(void)
{
    HANDLE t;
    int held, ran_during;

    set_lock("1");
    s_kc_ran = s_kc_let_go = 0;
    xbox_GuestCpuJoin();
    t = CreateThread(NULL, 0, kernel_call_waiter, NULL, 0, NULL);
    check(wait_waiting(1, 5000),
          "kernel call: the other thread waits while this one runs");
    held = xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 233); /* NtWaitForSingleObject */
    ran_during = wait_for(&s_kc_ran, 1, 5000);
    xbox_GuestCpuAcquire();                                /* back to guest code */
    check(held && ran_during, "kernel call: the waiter ran for the call's length");
    check(s_kc_let_go && xbox_GuestCpuHeld(),
          "kernel call: the caller got the CPU back once the waiter let go");
    xbox_GuestCpuPart();
    join(t);
}

/* ── A short kernel call, and the quantum ─────────────────────────── */

static volatile LONG s_short_ran;

static DWORD WINAPI short_waiter(LPVOID p)
{
    (void)p;
    xbox_GuestCpuJoin();
    InterlockedExchange(&s_short_ran, 1);
    xbox_GuestCpuPart();
    return 0;
}

static void short_call(void)
{
    HANDLE t;
    int held;
    XboxGuestCpuStats st;

    set_lock("1");
    s_short_ran = 0;
    xbox_GuestCpuJoin();
    t = CreateThread(NULL, 0, short_waiter, NULL, 0, NULL);
    check(wait_waiting(1, 5000),
          "short call: a waiter is queued");
    /* KfRaiseIrql, say: returns at once. The caller barges back; the waiter
     * is still queued. */
    held = xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 161);
    xbox_GuestCpuAcquire();
    check(held && xbox_GuestCpuHeld() && !s_short_ran && xbox_GuestCpuWaiting() == 1,
          "short call: a call that returns at once switches nobody");
    /* NtYieldExecution hands over whatever the quantum. */
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 238);
    xbox_GuestCpuAcquire();
    check(s_short_ran && xbox_GuestCpuHeld(), "short call: NtYieldExecution hands over");
    xbox_GuestCpuPart();
    join(t);

    /* Quantum 0: any kernel call hands over once a thread waits. */
    set_env("RECOMP_GUEST_QUANTUM", "0");
    s_short_ran = 0;
    xbox_GuestCpuStatsGet(&st, 1);
    xbox_GuestCpuJoin();
    t = CreateThread(NULL, 0, short_waiter, NULL, 0, NULL);
    check(wait_waiting(1, 5000),
          "quantum 0: a waiter is queued");
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 161);
    xbox_GuestCpuAcquire();
    xbox_GuestCpuStatsGet(&st, 0);
    check(s_short_ran && xbox_GuestCpuHeld() && st.turns >= 1,
          "quantum 0: the same call hands over, a turn ended at ordinal 161");
    check(st.max_turn_kind == XBOX_GUEST_CPU_KERNEL && st.max_turn_where == 161,
          "quantum 0: the longest turn names the ordinal");
    xbox_GuestCpuPart();
    join(t);
    set_env("RECOMP_GUEST_QUANTUM", NULL);
}

/* ── A bridge-run callback ────────────────────────────────────────── */

static volatile LONG s_host_took;

static DWORD WINAPI host_thread(LPVOID p)
{
    (void)p;
    s_host_took = xbox_GuestCpuEnterCallback();  /* not a guest thread */
    xbox_GuestCpuLeaveCallback((int)s_host_took);
    return 0;
}

static void callback(void)
{
    HANDLE t;
    int took, took2;

    set_lock("1");
    xbox_GuestCpuJoin();
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 3);        /* in a bridge */
    took = xbox_GuestCpuEnterCallback();
    check(took == 1 && xbox_GuestCpuHeld(),
          "callback: on a guest thread it takes the CPU the call let go of");
    took2 = xbox_GuestCpuEnterCallback();
    check(took2 == 0, "callback: a nested entry finds it held and takes nothing");
    xbox_GuestCpuLeaveCallback(took2);
    check(xbox_GuestCpuHeld(), "callback: the nested leave leaves it held");
    xbox_GuestCpuLeaveCallback(took);
    check(!xbox_GuestCpuHeld(), "callback: the leave gives it back to the bridge");
    xbox_GuestCpuAcquire();
    xbox_GuestCpuPart();
    t = CreateThread(NULL, 0, host_thread, NULL, 0, NULL);
    join(t);
    check(s_host_took == 0, "callback: a host thread's routine takes nothing");
}

/* ── A loop back edge past the quantum (RECOMP_BACK_EDGE) ─────────── */

static volatile LONG s_be_ran;

static DWORD WINAPI back_edge_waiter(LPVOID p)
{
    (void)p;
    xbox_GuestCpuJoin();
    InterlockedExchange(&s_be_ran, 1);
    xbox_GuestCpuPart();
    return 0;
}

/* The holder loops in "lifted code" with no kernel call, checking the flag
 * at the back edge as generated code does. */
static void back_edge(void)
{
    HANDLE t;
    uint64_t t0;
    int preempted = 0, flagged = 0;
    XboxGuestCpuStats st;

    set_lock("1");
    set_env("RECOMP_GUEST_QUANTUM", "4");
    s_be_ran = 0;
    xbox_GuestCpuJoin();
    xbox_GuestCpuStatsGet(&st, 1);
    t = CreateThread(NULL, 0, back_edge_waiter, NULL, 0, NULL);
    check(wait_waiting(1, 5000),
          "back edge: the other thread waits while this one loops");
    t0 = xbox_HostNowNs();
    while (!s_be_ran && xbox_HostNowNs() - t0 < 2000000000ull) {
        if (preempt_flag()) {
            flagged = 1;
            xbox_GuestCpuPreempt(0x00010001u);       /* RECOMP_BACK_EDGE */
            preempted = 1;
        }
    }
    check(flagged, "back edge: past the quantum the waiter raised the preempt flag");
    check(preempted && s_be_ran, "back edge: the holder handed over there and the waiter ran");
    check(xbox_GuestCpuHeld(), "back edge: the holder queued behind it and has the CPU again");
    check(!preempt_flag(), "back edge: the hand-off cleared the flag");
    xbox_GuestCpuStatsGet(&st, 0);
    check(st.preempts >= 1 && st.max_turn_kind == XBOX_GUEST_CPU_PREEMPT,
          "back edge: the turn is counted as a preemption at the edge");
    join(t);
    xbox_GuestCpuPart();
    set_env("RECOMP_GUEST_QUANTUM", NULL);

    /* The lock off: the flag never rises and the check is inert. */
    set_lock("0");
    preempt_flag_set(1);
    xbox_GuestCpuPreempt(0x00010001u);
    check(preempt_flag() == 1 && !xbox_GuestCpuHeld(),
          "back edge: with the lock off the check does nothing");
    preempt_flag_set(0);
}

/* ── A thread back from a wait preempts the holder at once ─────────── */

static volatile LONG s_wk_ran, s_wk_wait_ms = -1, s_wk_go;

/* "Blocks" in a kernel call with the CPU let go, then comes back: the
 * acquire must raise the flag at once, not past the (here 1 s) quantum. */
static DWORD WINAPI wake_waiter(LPVOID p)
{
    uint64_t t0;
    (void)p;
    xbox_GuestCpuJoin();
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 233 /* NtWaitForSingleObject */);
    InterlockedExchange(&s_wk_go, 1);
    Sleep(50);
    t0 = xbox_HostNowNs();
    xbox_GuestCpuAcquire();
    InterlockedExchange(&s_wk_wait_ms, (LONG)((xbox_HostNowNs() - t0) / 1000000u));
    InterlockedExchange(&s_wk_ran, 1);
    xbox_GuestCpuPart();
    return 0;
}

static volatile uint64_t s_bl_ran_ns;

static DWORD WINAPI blocked_waiter(LPVOID p)
{
    (void)p;
    xbox_GuestCpuJoin();
    s_bl_ran_ns = xbox_HostNowNs();
    InterlockedExchange(&s_wk_ran, 2);
    xbox_GuestCpuPart();
    return 0;
}

static void wake(void)
{
    HANDLE t;
    XboxGuestCpuStats st;
    uint64_t t0;
    int preempted = 0, flagged = 0;

    set_lock("1");
    set_env("RECOMP_GUEST_QUANTUM", "1000");
    s_wk_ran = 0;
    s_wk_go = 0;
    xbox_GuestCpuStatsGet(&st, 1);
    t = CreateThread(NULL, 0, wake_waiter, NULL, 0, NULL);
    check(wait_for(&s_wk_go, 1, 5000), "wake: the other thread let go in its call");
    xbox_GuestCpuJoin();
    check(xbox_GuestCpuHeld(), "wake: this thread took the free CPU meanwhile");
    t0 = xbox_HostNowNs();
    while (xbox_HostNowNs() - t0 < 2000000000ull && !s_wk_ran) {
        if (preempt_flag()) {
            flagged = 1;
            xbox_GuestCpuPreempt(0x00010001u);
            preempted = 1;
        }
    }
    check(flagged && preempted && s_wk_ran == 1,
          "wake: back from its call it asked, and the holder handed over at the edge");
    check(s_wk_wait_ms >= 0 && s_wk_wait_ms < 500,
          "wake: it ran well inside the 1 s quantum");
    check(xbox_GuestCpuHeld() && !preempt_flag(),
          "wake: the holder has the CPU back and the flag is clear");
    xbox_GuestCpuStatsGet(&st, 0);
    check(st.preempts >= 1, "wake: the turn is counted as a preemption");
    join(t);

    /* A contended critical section: the holder of the guest CPU lets go in
     * a call that does not wake the waiters, then blocks on the host; the
     * Blocked hook wakes them so one takes the CPU, and the thread takes
     * it back (and preempts) on its return. Without the hook the waiter's
     * 2 ms poll would take the CPU all the same, so the gap from the hook
     * to the waiter's run is what tells: eight rounds, none over 1.5 ms
     * and under 4 ms in all (the poll alone would spend about 8). */
    {
        int i, ok = 1;
        uint64_t sum = 0, worst = 0;
        for (i = 0; i < 8; i++) {
            uint64_t gap;
            s_wk_ran = 0;
            s_bl_ran_ns = 0;
            t = CreateThread(NULL, 0, blocked_waiter, NULL, 0, NULL);
            ok &= wait_waiting(1, 5000);
            xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 277 /* RtlEnterCriticalSection */);
            t0 = xbox_HostNowNs();
            xbox_GuestCpuBlocked();
            ok &= wait_for(&s_wk_ran, 2, 5000);
            join(t);
            gap = s_bl_ran_ns > t0 ? s_bl_ran_ns - t0 : 0;
            sum += gap;
            if (gap > worst)
                worst = gap;
            xbox_GuestCpuAcquire();
            ok &= xbox_GuestCpuHeld();
        }
        check(ok, "blocked: told, the waiter took the free CPU each round, and the thread took it back");
        check(worst < 1500000ull && sum < 4000000ull,
              "blocked: the waiter ran on the hook, not on its poll");
        if (!(worst < 1500000ull && sum < 4000000ull))
            printf("    (worst %.2f ms, sum %.2f ms)\n", worst / 1e6, sum / 1e6);
    }
    xbox_GuestCpuPart();
    set_env("RECOMP_GUEST_QUANTUM", NULL);
    set_lock("0");
}

/* ── A kernel call at DISPATCH never hands over ───────────────────── */

static volatile LONG s_dp_ran;

static DWORD WINAPI dispatch_waiter(LPVOID p)
{
    (void)p;
    xbox_GuestCpuJoin();
    InterlockedExchange(&s_dp_ran, 1);
    xbox_GuestCpuPart();
    return 0;
}

/* The thunk releases at every ordinal, KfLowerIrql and KeReleaseSpinLock
 * included, with the caller still holding the DISPATCH gate: the call must
 * soft-release and barge back, flag or quantum notwithstanding, or the gate
 * holder queues behind waiters whose next KfRaiseIrql blocks on the gate. */
static void dispatch_call(void)
{
    HANDLE t;
    XboxGuestCpuStats st;
    int saved, held;

    set_lock("1");
    set_env("RECOMP_GUEST_QUANTUM", "0");
    s_dp_ran = 0;
    xbox_GuestCpuJoin();
    xbox_GuestCpuStatsGet(&st, 1);
    t = CreateThread(NULL, 0, dispatch_waiter, NULL, 0, NULL);
    check(wait_waiting(1, 5000),
          "dispatch: the other thread waits while this one runs");
    g_fs_base = 0;
    saved = xbox_IrqlEnterInterrupt(DISPATCH_LEVEL);
    preempt_flag_set(1);
    Sleep(5);                           /* past the 0 ms quantum, flag up */
    held = xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 161 /* KfLowerIrql */);
    xbox_GuestCpuAcquire();
    xbox_GuestCpuStatsGet(&st, 0);
    check(held && xbox_GuestCpuHeld() && st.turns == 0 && st.releases == 1,
          "dispatch: the call soft-released and barged back, no hand-off");
    xbox_IrqlLeaveInterrupt(saved);
    preempt_flag_set(0);
    held = xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 161);
    xbox_GuestCpuAcquire();
    xbox_GuestCpuStatsGet(&st, 0);
    check(held && wait_for(&s_dp_ran, 1, 5000) && st.turns >= 1,
          "dispatch: below DISPATCH the same call past the quantum hands over");
    join(t);
    xbox_GuestCpuPart();
    set_env("RECOMP_GUEST_QUANTUM", NULL);
    set_lock("0");
}

int main(void)
{
    HANDLE wd;
    XboxGuestCpuStats st;

    wd = CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    CloseHandle(wd);

    printf("guest_cpu\n");
    switched_off();
    exclusion();
    fifo();
    kernel_call();
    short_call();
    back_edge();
    wake();
    dispatch_call();
    callback();

    /* The dispatch case reset the counters last, so only it and the
     * callback case are in here. */
    xbox_GuestCpuStatsGet(&st, 0);
    check(st.turns > 0 && st.waits > 0 && st.releases > 0,
          "stats: turns, releases and waits were counted since the reset");
    xbox_GuestCpuReport();
    xbox_GuestCpuStatsGet(&st, 1);
    xbox_GuestCpuStatsGet(&st, 0);
    check(st.turns == 0 && st.releases == 0 && st.max_turn_us == 0,
          "stats: a reset clears them");

    if (failures) {
        printf("guest_cpu: %d failure(s)\n", failures);
        return 1;
    }
    printf("guest_cpu: ok\n");
    return 0;
}
