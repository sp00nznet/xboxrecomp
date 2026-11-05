/*
 * Guest priorities on the guest CPU (kernel_guest_cpu.c, guest_sched=1 with
 * RECOMP_GUEST_LOCK=1).
 *
 * The console ran the highest-priority ready thread, and CRI ADX (JSRF,
 * BLiNX 2) locks by raising itself to time critical: no title thread runs
 * until it drops back. The cases, each ordered by the lock's own waiter
 * count rather than by timing where it can be:
 *  - the switch: guest_sched needs the lock and guest_priority not 0; off,
 *    the stored priority and the suspend count are no-ops;
 *  - the stored value: NT's +-16 saturation, the clamp to 1..15, the
 *    previous value returned;
 *  - a release goes to the highest-priority waiter, oldest first among
 *    equals, and equal priorities stay FIFO;
 *  - a time-critical holder keeps the CPU at a spin-wait pass, a back edge,
 *    a kernel call past the quantum and NtYieldExecution, and hands over
 *    once it drops back (the ADX unlock);
 *  - a waiter above the holder raises the preempt flag at once, without
 *    the quantum;
 *  - a short kernel call's CPU is not taken by a lower waiter; one that
 *    lasts past 2 ms, or may block, is, and the holder preempts it back;
 *  - a wait on a signalled object is a short call; one that blocks, or a
 *    call that says it blocks (xbox_GuestCpuBlocked), lets the waiter in;
 *  - an equal waiter takes a short call's CPU; a thread back from a block
 *    (a barger) with the +1 wake boost over an equal holder asks at once,
 *    a lower one does not;
 *  - a suspended waiter is skipped, a thread created suspended waits for
 *    its resume, and a suspended holder stops at its next back edge;
 *  - a freed thread object's record lasts until its thread parts;
 *  - the hang guard (the valve) gives a starved waiter a turn, counted.
 * The cases that need a short call to stay short widen the 2 ms soft window
 * (xbox_GuestSchedTestWindows), so a slow host does not fail them. A
 * watchdog fails the test instead of hanging it.
 */
#include "kernel.h"
#include "recomp_env.h"
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

/* Thread objects stand in for kernel_bridge.c's guest KTHREADs. */
#define OBJ_MAIN 0x80010000u
#define OBJ(i)   (0x80020000u + (uint32_t)(i) * 0x200u)

static int failures;

/* The flag is only ever accessed atomically (kernel.h): a plain load in a
 * polling loop may be hoisted now that it is no longer volatile. */
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
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void config(const char *lock, const char *sched, const char *quantum)
{
    recomp_env_set(RENV_GUEST_LOCK, lock);
    recomp_env_set(RENV_GUEST_SCHED, sched);
    recomp_env_set(RENV_GUEST_QUANTUM, quantum);
    xbox_GuestCpuReloadConfig();
}

static void join(HANDLE t)
{
    WaitForSingleObject(t, 20000);
    CloseHandle(t);
}

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

static int wait_waiters(int n, int ms)
{
    return wait_for((volatile LONG *)&g_xbox_guest_cpu_waiters, n, ms);
}

static DWORD WINAPI watchdog(LPVOID unused)
{
    (void)unused;
    Sleep(60000);
    fprintf(stderr, "guest_sched: watchdog: a case hung\n");
    fflush(stderr);
    _Exit(1);
    return 0;
}

/* The main thread as a guest thread at base priority inc. */
static void main_join(LONG inc)
{
    xbox_GuestSchedThreadNew(OBJ_MAIN);
    xbox_GuestSchedSetBase(OBJ_MAIN, inc);
    xbox_GuestSchedBind(OBJ_MAIN);
    xbox_GuestCpuJoin();
}

/* ── A guest thread that records when it ran ───────────────────────── */

static volatile LONG s_order[8], s_seq, s_ran[8];

static DWORD WINAPI order_thread(LPVOID p)
{
    int id = (int)(intptr_t)p;

    xbox_GuestSchedBind(OBJ(id));
    xbox_GuestCpuJoin();
    s_order[InterlockedIncrement(&s_seq) - 1] = id;
    InterlockedExchange(&s_ran[id], 1);
    xbox_GuestCpuPart();
    return 0;
}

static HANDLE spawn(int id, LONG inc, int suspended)
{
    xbox_GuestSchedThreadNew(OBJ(id));
    xbox_GuestSchedSetBase(OBJ(id), inc);
    if (suspended)
        xbox_GuestSchedSuspend(OBJ(id), 1, NULL);
    return CreateThread(NULL, 0, order_thread, (LPVOID)(intptr_t)id, 0, NULL);
}

static void reset_order(void)
{
    s_seq = 0;
    memset((void *)s_order, 0, sizeof s_order);
    memset((void *)s_ran, 0, sizeof s_ran);
}

/* ── The switch ───────────────────────────────────────────────────── */

static void switch_and_store(void)
{
    ULONG prev = 99;

    config("0", "1", NULL);
    check(!xbox_GuestSchedOn(), "switch: guest_sched alone (no lock) is off");
    config("1", NULL, NULL);
    check(!xbox_GuestSchedOn(), "switch: the lock alone is the FIFO");
    xbox_GuestSchedThreadNew(OBJ(1));
    check(xbox_GuestSchedSetBase(OBJ(1), 16) == 0 && xbox_GuestSchedQueryBase(OBJ(1)) == 0,
          "switch: off, the stored priority is a no-op reading 0");
    check(!xbox_GuestSchedSuspend(OBJ(1), 1, &prev) && prev == 99,
          "switch: off, a suspend is left to the host");
    recomp_env_set(RENV_GUEST_PRIORITY, "0");
    config("1", "1", NULL);
    check(!xbox_GuestSchedOn(), "switch: guest_priority=0 wins over guest_sched");
    recomp_env_set(RENV_GUEST_PRIORITY, NULL);
    config("1", "1", NULL);
    check(xbox_GuestSchedOn(), "switch: lock and guest_sched on");

    xbox_GuestSchedThreadNew(OBJ(1));
    check(xbox_GuestSchedQueryBase(OBJ(1)) == 0, "store: a new thread is at base 0");
    check(xbox_GuestSchedSetBase(OBJ(1), 16) == 0 && xbox_GuestSchedQueryBase(OBJ(1)) == 16,
          "store: +16 (time critical) reads back 16, the previous 0 returned");
    check(xbox_GuestSchedSetBase(OBJ(1), 2) == 16 && xbox_GuestSchedQueryBase(OBJ(1)) == 2,
          "store: +2 replaces it and returns 16");
    xbox_GuestSchedSetBase(OBJ(1), 9);
    check(xbox_GuestSchedQueryBase(OBJ(1)) == 7, "store: +9 clamps to 15 (+7)");
    xbox_GuestSchedSetBase(OBJ(1), -10);
    check(xbox_GuestSchedQueryBase(OBJ(1)) == -7, "store: -10 clamps to 1 (-7)");
    xbox_GuestSchedSetBase(OBJ(1), -20);
    check(xbox_GuestSchedQueryBase(OBJ(1)) == -16, "store: -20 saturates to idle (-16)");
    xbox_GuestSchedThreadNew(OBJ(1));
    check(xbox_GuestSchedQueryBase(OBJ(1)) == 0, "store: a reused object starts at 0 again");
    check(xbox_GuestSchedSuspend(OBJ(1), 1, &prev) && prev == 0
              && xbox_GuestSchedSuspend(OBJ(1), -1, &prev) && prev == 1,
          "store: suspend and resume count, the previous count returned");
    check(!xbox_GuestSchedSuspend(OBJ(1), -1, &prev),
          "store: a resume past 0 goes to the host (created suspended there)");
    xbox_GuestSchedThreadGone(OBJ(1));
    check(xbox_GuestSchedQueryBase(OBJ(1)) == 0, "store: a freed object is forgotten");
}

/* ── Highest waiter first; equals stay FIFO ───────────────────────── */

static void highest_first(void)
{
    HANDLE t[4];
    int q = 1;

    config("1", "1", "1000");
    reset_order();
    main_join(0);
    t[0] = spawn(1, 0, 0);
    q &= wait_waiters(1, 5000);
    t[1] = spawn(2, 2, 0);
    q &= wait_waiters(2, 5000);
    t[2] = spawn(3, 16, 0);
    q &= wait_waiters(3, 5000);
    t[3] = spawn(4, 2, 0);
    q &= wait_waiters(4, 5000);
    check(q, "order: four waiters queued (8, 10, 15, 10)");
    check(preempt_flag(), "order: a waiter above the holder asked for the CPU at once");
    check(xbox_GuestCpuYield(0x1000) == 1, "order: the holder at 8 yields to them");
    s_order[InterlockedIncrement(&s_seq) - 1] = 9;
    check(s_order[0] == 3 && s_order[1] == 2 && s_order[2] == 4 && s_order[3] == 1
              && s_order[4] == 9,
          "order: 15, then the two 10s oldest first, then the older 8, then the yielder");
    xbox_GuestCpuPart();
    for (int i = 0; i < 4; i++)
        join(t[i]);

    reset_order();
    main_join(0);
    for (int i = 0; i < 3; i++) {
        t[i] = spawn(i + 1, 0, 0);
        wait_waiters(i + 1, 5000);
    }
    check(!preempt_flag(), "fifo: equal waiters do not ask before the quantum");
    xbox_GuestCpuYield(0x1000);
    s_order[InterlockedIncrement(&s_seq) - 1] = 9;
    check(s_order[0] == 1 && s_order[1] == 2 && s_order[2] == 3 && s_order[3] == 9,
          "fifo: equal priorities are served oldest first, the yielder last");
    xbox_GuestCpuPart();
    for (int i = 0; i < 3; i++)
        join(t[i]);
}

/* ── A time-critical holder keeps the CPU (the ADX lock) ──────────── */

static void time_critical(void)
{
    HANDLE t;
    XboxGuestCpuStats st;
    int kept = 1;

    config("1", "1", "0");              /* every kernel call is past the quantum */
    reset_order();
    main_join(16);
    xbox_GuestCpuStatsGet(&st, 1);
    t = spawn(1, 0, 0);
    check(wait_waiters(1, 5000), "lock: a waiter at 8 queues behind the holder at 15");
    check(xbox_GuestCpuYield(0x2000) == 0 && xbox_GuestCpuHeld(),
          "lock: a spin-wait pass keeps the CPU");
    preempt_flag_set(1);
    xbox_GuestCpuPreempt(0x2004);
    check(xbox_GuestCpuHeld() && !preempt_flag(),
          "lock: a back edge keeps it and lowers the flag");
    xbox_GuestSchedTestWindows(10000000000ull, 0);
    for (int i = 0; i < 20; i++) {
        xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 238);   /* NtYieldExecution */
        xbox_GuestCpuAcquire();
        xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 160);   /* KfLowerIrql: returns at once */
        xbox_GuestCpuAcquire();
        kept &= !s_ran[1];
    }
    check(kept, "lock: NtYieldExecution and short calls past the quantum keep it");
    Sleep(20);
    check(!s_ran[1], "lock: 20 ms on, the waiter has still not run");
    xbox_GuestSchedTestWindows(0, 0);
    xbox_GuestCpuStatsGet(&st, 0);
    check(st.refused > 0, "lock: the kept hand-offs are counted");
    check(xbox_GuestSchedQueryBase(OBJ_MAIN) == 16, "lock: the holder reads its 15 back (sub_001437B0)");

    /* The unlock: back to 8, and the waiter at 8 gets the next turn. */
    check(xbox_GuestSchedSetBase(OBJ_MAIN, 0) == 16, "unlock: the restore returns the 16");
    check(xbox_GuestCpuYield(0x2008) == 1 && s_ran[1],
          "unlock: at 8 the next pass hands over and the waiter runs");
    xbox_GuestCpuPart();
    join(t);
}

/* ── A waiter above the holder preempts at the back edge ──────────── */

static void preempt_higher(void)
{
    HANDLE t;
    uint64_t t0;
    int handed = 0;

    config("1", "1", "1000");           /* the quantum never comes into it */
    reset_order();
    main_join(0);
    t = spawn(1, 2, 0);
    check(wait_waiters(1, 5000), "preempt: a waiter at 10 queues behind the holder at 8");
    t0 = xbox_HostNowNs();
    while (!s_ran[1] && xbox_HostNowNs() - t0 < 2000000000ull) {
        if (preempt_flag()) {
            xbox_GuestCpuPreempt(0x3000);
            handed = 1;
        }
    }
    check(handed && s_ran[1], "preempt: it ran at the holder's next back edge, no quantum");
    check(xbox_GuestCpuHeld(), "preempt: the holder has the CPU again");
    xbox_GuestCpuPart();
    join(t);
}

/* ── A soft release: short, long and blocking kernel calls ────────── */

static volatile LONG s_sr_ran, s_sr_go, s_sr_preempted;

static DWORD WINAPI soft_waiter(LPVOID p)
{
    (void)p;
    xbox_GuestSchedBind(OBJ(5));
    xbox_GuestCpuJoin();
    InterlockedExchange(&s_sr_ran, 1);
    /* Guest code: loop until told, checking the back edge. */
    while (!s_sr_go) {
        if (preempt_flag()) {
            /* Before the call: it returns only once the holder lets go. */
            InterlockedExchange(&s_sr_preempted, 1);
            xbox_GuestCpuPreempt(0x4000);
        }
    }
    xbox_GuestCpuPart();
    return 0;
}

static void soft_release(void)
{
    HANDLE t;
    int kept = 1;

    config("1", "1", "1000");
    main_join(16);
    s_sr_ran = s_sr_go = s_sr_preempted = 0;
    xbox_GuestSchedThreadNew(OBJ(5));
    t = CreateThread(NULL, 0, soft_waiter, NULL, 0, NULL);
    check(wait_waiters(1, 5000), "soft: a waiter at 8 behind the holder at 15");
    xbox_GuestSchedTestWindows(10000000000ull, 0);
    for (int i = 0; i < 50; i++) {
        xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 160);
        xbox_GuestCpuAcquire();
        kept &= !s_sr_ran;
    }
    xbox_GuestSchedTestWindows(0, 0);
    check(kept, "soft: short calls are not taken by the lower waiter");
    /* A call that lasts: past 2 ms the caller counts as blocked. */
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 160);
    check(wait_for(&s_sr_ran, 1, 2000), "soft: a call past 2 ms lets the lower waiter run");
    xbox_GuestCpuAcquire();             /* back: 15 over 8, preempts it */
    check(s_sr_preempted && xbox_GuestCpuHeld() && wait_waiters(1, 1000),
          "soft: the returning holder preempted it at its back edge");
    InterlockedExchange(&s_sr_go, 1);
    xbox_GuestCpuPart();
    join(t);

    /* A call that blocks (file I/O) lets it run at once. */
    main_join(16);
    s_sr_ran = s_sr_go = s_sr_preempted = 0;
    xbox_GuestSchedThreadNew(OBJ(5));
    t = CreateThread(NULL, 0, soft_waiter, NULL, 0, NULL);
    wait_waiters(1, 5000);
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 219);       /* NtReadFile */
    check(wait_for(&s_sr_ran, 1, 1000), "soft: a call that blocks lets the lower waiter run");
    xbox_GuestCpuAcquire();
    check(s_sr_preempted && xbox_GuestCpuHeld() && wait_waiters(1, 1000),
          "soft: and the holder takes it back on return");
    InterlockedExchange(&s_sr_go, 1);
    xbox_GuestCpuPart();
    join(t);

    /* A zero delay is a yield, a short call; a real delay sleeps. */
    main_join(16);
    s_sr_ran = s_sr_go = s_sr_preempted = 0;
    xbox_GuestSchedThreadNew(OBJ(5));
    t = CreateThread(NULL, 0, soft_waiter, NULL, 0, NULL);
    wait_waiters(1, 5000);
    xbox_GuestSchedTestWindows(10000000000ull, 0);
    kept = 1;
    for (int i = 0; i < 20; i++) {
        LARGE_INTEGER zero = { .QuadPart = 0 };
        xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 99);
        xbox_KeDelayExecutionThread(0, 0, &zero);
        xbox_GuestCpuAcquire();
        kept &= !s_sr_ran;
    }
    check(kept, "soft: a zero delay is not taken by the lower waiter");
    {
        LARGE_INTEGER ms20 = { .QuadPart = -200000 };
        xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 99);
        xbox_KeDelayExecutionThread(0, 0, &ms20);
    }
    check(wait_for(&s_sr_ran, 1, 1000), "soft: a 20 ms delay lets the lower waiter run");
    xbox_GuestSchedTestWindows(0, 0);
    xbox_GuestCpuAcquire();
    check(s_sr_preempted && xbox_GuestCpuHeld() && wait_waiters(1, 1000),
          "soft: and the holder takes it back after the delay");
    InterlockedExchange(&s_sr_go, 1);
    xbox_GuestCpuPart();
    join(t);

    /* Suspending itself (231) is a soft release; the bridge reports it
     * blocked only once the host thread really stops. */
    main_join(16);
    s_sr_ran = s_sr_go = s_sr_preempted = 0;
    xbox_GuestSchedThreadNew(OBJ(5));
    t = CreateThread(NULL, 0, soft_waiter, NULL, 0, NULL);
    wait_waiters(1, 5000);
    xbox_GuestSchedTestWindows(10000000000ull, 0);
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 231);
    Sleep(20);
    check(!s_sr_ran, "soft: a self-suspend keeps the CPU from the lower waiter");
    xbox_GuestCpuBlocked();
    check(wait_for(&s_sr_ran, 1, 1000), "soft: once blocked, the lower waiter runs");
    xbox_GuestSchedTestWindows(0, 0);
    xbox_GuestCpuAcquire();
    check(s_sr_preempted && xbox_GuestCpuHeld() && wait_waiters(1, 1000),
          "soft: and the resumed holder takes it back");
    InterlockedExchange(&s_sr_go, 1);
    xbox_GuestCpuPart();
    join(t);
}

/* ── Suspended threads ────────────────────────────────────────────── */

static volatile LONG s_hs_stage;

static DWORD WINAPI suspender(LPVOID p)
{
    (void)p;
    wait_for(&s_hs_stage, 1, 5000);
    xbox_GuestSchedSuspend(OBJ_MAIN, 1, NULL);
    Sleep(50);
    InterlockedExchange(&s_hs_stage, 2);
    xbox_GuestSchedSuspend(OBJ_MAIN, -1, NULL);
    return 0;
}

static void suspended(void)
{
    HANDLE t[2], h;
    uint64_t t0;
    ULONG prev = 9;

    config("1", "1", "1000");
    reset_order();
    main_join(0);
    t[0] = spawn(1, 2, 1);              /* above the holder, but suspended */
    t[1] = spawn(2, 0, 0);
    check(wait_waiters(2, 5000), "suspend: a suspended 10 and an 8 queue");
    xbox_GuestCpuYield(0x5000);
    check(s_ran[2] && !s_ran[1], "suspend: the pass skips the suspended waiter");
    check(xbox_GuestSchedSuspend(OBJ(1), -1, &prev) && prev == 1, "suspend: the resume counts down");
    check(preempt_flag(), "suspend: resumed above the holder, it asks for the CPU");
    xbox_GuestCpuPreempt(0x5004);
    check(s_ran[1], "suspend: and runs at the next back edge");
    xbox_GuestCpuPart();
    join(t[0]);
    join(t[1]);

    /* A holder suspended by another thread stops at its next back edge
     * even with nobody waiting, and goes on once resumed. */
    main_join(0);
    s_hs_stage = 0;
    h = CreateThread(NULL, 0, suspender, NULL, 0, NULL);
    InterlockedExchange(&s_hs_stage, 1);
    t0 = xbox_HostNowNs();
    while (!preempt_flag() && xbox_HostNowNs() - t0 < 2000000000ull)
        ;
    xbox_GuestCpuPreempt(0x5008);
    check(s_hs_stage == 2 && xbox_GuestCpuHeld(),
          "suspend: a suspended holder stopped at its back edge until resumed");
    xbox_GuestCpuPart();
    join(h);
}

/* ── Waits: signalled is short, blocking lets the waiter in ───────── */

static volatile LONG s_wt_turns, s_wt_go;

/* A guest thread at 8 that counts its turns and gives way at back edges. */
static DWORD WINAPI turn_waiter(LPVOID p)
{
    xbox_GuestSchedBind((uint32_t)(uintptr_t)p);
    xbox_GuestCpuJoin();
    InterlockedIncrement(&s_wt_turns);
    while (!s_wt_go) {
        if (preempt_flag()) {
            xbox_GuestCpuPreempt(0x4100);
            InterlockedIncrement(&s_wt_turns);
        }
    }
    xbox_GuestCpuPart();
    return 0;
}

static HANDLE turn_spawn(uint32_t obj, LONG inc)
{
    s_wt_turns = s_wt_go = 0;
    xbox_GuestSchedThreadNew(obj);
    xbox_GuestSchedSetBase(obj, inc);
    return CreateThread(NULL, 0, turn_waiter, (LPVOID)(uintptr_t)obj, 0, NULL);
}

/* The holder takes the CPU back and the waiter gives way at its back edge. */
static int take_back(LONG turns)
{
    xbox_GuestCpuAcquire();
    return xbox_GuestCpuHeld() && s_wt_turns == turns && wait_waiters(1, 1000);
}

static void waits(void)
{
    HANDLE t, e;
    LARGE_INTEGER to;
    int kept = 1;
    NTSTATUS st;

    config("1", "1", "1000");
    main_join(16);
    t = turn_spawn(OBJ(5), 0);
    check(wait_waiters(1, 5000), "wait: a waiter at 8 behind the holder at 15");
    xbox_GuestSchedTestWindows(10000000000ull, 0);
    e = CreateEventA(NULL, TRUE, TRUE, NULL);
    to.QuadPart = -500000;              /* 50 ms, relative */
    for (int i = 0; i < 50; i++) {
        xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 159);   /* KeWaitForSingleObject */
        st = xbox_KeWaitForSingleObject(e, 0, 0, FALSE, &to);
        xbox_GuestCpuAcquire();
        kept &= st == 0 && !s_wt_turns;
    }
    check(kept, "wait: a wait on a signalled event keeps the CPU (the ADX lock)");

    ResetEvent(e);
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 159);
    st = xbox_KeWaitForSingleObject(e, 0, 0, FALSE, &to);
    check(st == 0x102 && wait_for(&s_wt_turns, 1, 1000),
          "wait: one that blocks till its timeout lets the waiter run");
    check(take_back(1), "wait: and the holder takes it back on return");

    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 160);
    xbox_GuestCpuBlocked();
    check(wait_for(&s_wt_turns, 2, 1000),
          "wait: a short call that says it blocks lets the waiter run");
    check(take_back(2), "wait: and the holder takes it back");
    xbox_GuestSchedTestWindows(0, 0);
    CloseHandle(e);
    InterlockedExchange(&s_wt_go, 1);
    xbox_GuestCpuPart();
    join(t);
}

/* ── Equals: a short call, a barger ───────────────────────────────── */

static volatile LONG s_bg_stage, s_bg_go;

static DWORD WINAPI barger(LPVOID p)
{
    xbox_GuestSchedBind((uint32_t)(uintptr_t)p);
    xbox_GuestCpuJoin();
    InterlockedExchange(&s_bg_stage, 1);
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 219);  /* blocks: the holder takes it */
    wait_for(&s_bg_go, 1, 5000);
    xbox_GuestCpuAcquire();                            /* back from the block */
    InterlockedExchange(&s_bg_stage, 2);
    xbox_GuestCpuPart();
    return 0;
}

static int barge_asks(LONG inc)
{
    HANDLE t;
    int asked;

    s_bg_stage = s_bg_go = 0;
    xbox_GuestSchedThreadNew(OBJ(6));
    xbox_GuestSchedSetBase(OBJ(6), inc);
    main_join(0);
    t = CreateThread(NULL, 0, barger, (LPVOID)(uintptr_t)OBJ(6), 0, NULL);
    wait_waiters(1, 5000);
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 219);  /* the joiner runs */
    wait_for(&s_bg_stage, 1, 5000);
    xbox_GuestCpuAcquire();                            /* after its block */
    preempt_flag_set(0);
    InterlockedExchange(&s_bg_go, 1);
    wait_waiters(1, 5000);
    /* The waiter raises the flag at its queueing or a 2 ms poll; give it
     * 50 polls before calling a no. */
    for (int i = 0; i < 100 && !(asked = preempt_flag()); i++)
        Sleep(1);
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 219);  /* let it finish */
    wait_for(&s_bg_stage, 2, 5000);
    xbox_GuestCpuAcquire();
    xbox_GuestCpuPart();
    join(t);
    return asked;
}

static void equals(void)
{
    HANDLE t;

    config("1", "1", "1000");
    main_join(0);
    t = turn_spawn(OBJ(5), 0);
    check(wait_waiters(1, 5000), "equal: a waiter at 8 behind a holder at 8");
    xbox_GuestSchedTestWindows(10000000000ull, 0);
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 160);
    check(wait_for(&s_wt_turns, 1, 1000), "equal: it takes a short call's CPU");
    check(take_back(1), "equal: the holder, back with its wake boost, takes it back");
    xbox_GuestSchedTestWindows(0, 0);
    InterlockedExchange(&s_wt_go, 1);
    xbox_GuestCpuPart();
    join(t);

    check(barge_asks(0), "barge: back from a block, an equal (+1 boost) asks at once");
    check(!barge_asks(-1), "barge: a lower one (7 + 1) does not");
}

/* ── A freed thread object, the hang guard ─────────────────────────── */

static void gone_and_valve(void)
{
    HANDLE t;
    long v0;
    uint64_t t0;

    config("1", "1", "1000");
    reset_order();
    main_join(0);
    t = spawn(6, 2, 0);
    check(wait_waiters(1, 5000), "gone: a waiter at 10 queues");
    xbox_GuestSchedThreadGone(OBJ(6));
    check(xbox_GuestSchedQueryBase(OBJ(6)) == 2, "gone: freed while queued, its record stays");
    xbox_GuestCpuYield(0x7000);
    join(t);
    check(s_ran[6] && xbox_GuestSchedQueryBase(OBJ(6)) == 0,
          "gone: it ran, and its record went when it parted");
    xbox_GuestCpuPart();

    /* A holder at 15 that never drops back: past the valve's window the
     * waiter at 8 gets a turn anyway, and the firing is counted. */
    xbox_GuestSchedTestWindows(0, 200000000ull);
    reset_order();
    v0 = xbox_GuestSchedValves();
    main_join(16);
    t = spawn(1, 0, 0);
    wait_waiters(1, 5000);
    /* A back-edge spinner (no kernel call, no spin pass): the generated
     * code calls xbox_GuestCpuPreempt only when the flag is up. */
    t0 = xbox_HostNowNs();
    while (!preempt_flag() && xbox_HostNowNs() - t0 < 3000000000ull)
        ;
    check(xbox_HostNowNs() - t0 >= 150000000ull && xbox_GuestSchedValves() == v0 + 1,
          "valve: past the window it fires, raises the flag and is counted");
    check(!s_ran[1], "valve: the waiter has not run before the hand-off");
    xbox_GuestCpuPreempt(0x7004);
    check(s_ran[1], "valve: it runs at the first back edge, before the holder");
    xbox_GuestCpuPart();
    join(t);
    xbox_GuestSchedTestWindows(0, 0);
}

int main(void)
{
    HANDLE wd;

    wd = CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    CloseHandle(wd);

    printf("guest_sched\n");
    switch_and_store();
    highest_first();
    time_critical();
    preempt_higher();
    soft_release();
    waits();
    equals();
    suspended();
    gone_and_valve();
    xbox_GuestCpuReport();
    config(NULL, NULL, NULL);

    if (failures) {
        printf("guest_sched: %d failure(s)\n", failures);
        return 1;
    }
    printf("guest_sched: ok\n");
    return 0;
}
