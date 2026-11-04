/*
 * The guest unwind (kernel_guest_unwind.c): a reset request brings every
 * tracked guest thread back to its call-in, wherever it is in guest code.
 * Each thread here is set up the way bridge_thread_main sets up a title's
 * worker (counted by its creator, joined, guest code under a call-in, then
 * the guest CPU's part and the unwind's). The cases:
 *  - inert before the enable: a call-in runs its function, a request does
 *    nothing and the flag stays down;
 *  - the lock off (Burnout 3): threads at a loop back edge, in a spin-wait
 *    loop, in a kernel wait (one object, two for any, two for all), in a
 *    delay (plain and alertable), owning a guest critical section twice,
 *    blocked on that section, owning a host mutex (a guest mutant) and
 *    blocked on it all unwind, each at its kind of point; another thread
 *    takes the section afterwards and nobody is left counted; every thread
 *    is back at its call-in's IRQL and gate depth;
 *  - a thread's part forgets its lock records, so a thread given its id
 *    later does not leave them;
 *  - at DISPATCH with the one-CPU gate on (RECOMP_DISPATCH_GATE): the
 *    unwind puts the IRQL and the gate back, and the gate is free after;
 *  - a thread holding a host resource (xbox_GuestUnwindHold) is refused
 *    until it lets go, then unwinds at its next point;
 *  - the lock on (BLiNX 2): the holder and a thread queued on the lock both
 *    unwind, and the CPU is free after;
 *  - the per-thread stats records are given back at each part: three rounds
 *    of threads, more than the table holds, leave none taken;
 *  - the join race: a thread counted by its creator but not yet joined
 *    keeps the reset waiting, and when it joins it runs no guest code;
 *  - a boot loop parks after its unwind, lets go of the CPU while parked,
 *    and runs its guest code again after the resume;
 *  - the deterministic ring: threads waiting for the token unwind, and
 *    their parts leave the ring to the main thread;
 *  - a 65th thread the registry cannot take is counted as untracked, and a
 *    request refuses while it lives;
 *  - a host call-in running guest code keeps the reset waiting until it
 *    ends, and a new one is skipped while the reset is pending.
 * A watchdog fails the test instead of hanging it.
 */
#include "kernel.h"
#include "kernel_pacing.h"
#include "kernel_guest_unwind.h"
#include "xbox_memory_layout.h"   /* g_fs_base */
#include "guest_time.h"
#include "recomp_env.h"
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

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static DWORD WINAPI watchdog(LPVOID unused)
{
    (void)unused;
    Sleep(60000);
    fprintf(stderr, "guest_unwind: watchdog: a case hung\n");
    fflush(stderr);
    _Exit(1);
    return 0;
}

/* As templates/runtime/recomp_types.h defines RECOMP_BACK_EDGE. */
static void back_edge(uint32_t va)
{
    if (InterlockedCompareExchange((volatile LONG *)&g_xbox_guest_cpu_preempt, 0, 0))
        xbox_GuestCpuPreempt(va);
}

static void set_lock(const char *v)
{
    recomp_env_set(RENV_GUEST_LOCK, v);
    xbox_GuestCpuReloadConfig();
}

/* ── A guest thread, as bridge_thread_main runs one ──────────────── */

typedef struct Guest Guest;
struct Guest {
    void (*body)(Guest *g);
    uint32_t obj;               /* the ring's thread object (det case) */
    int counted;
    volatile LONG ran, unwound, returned, mark_ok;
};

static volatile LONG s_stop, s_up;

static void guest_body(void *p)
{
    Guest *g = (Guest *)p;

    if (g->obj)
        xbox_GuestDetBind(g->obj);
    xbox_GuestCpuJoin();
    InterlockedIncrement(&g->ran);
    InterlockedIncrement(&s_up);
    g->body(g);
    InterlockedIncrement(&g->returned);
}

static int mark_eq(const XboxIrqlMark *a, const XboxIrqlMark *b)
{
    return a->irql == b->irql && a->gate_depth == b->gate_depth
        && a->gate_held == b->gate_held && a->counted == b->counted
        && a->in_isr == b->in_isr && a->in_dpc == b->in_dpc;
}

static DWORD WINAPI guest_thread(LPVOID p)
{
    Guest *g = (Guest *)p;
    XboxIrqlMark m0, m1;

    xbox_GuestUnwindJoin(g->counted);
    xbox_IrqlUnwindMark(&m0);
    if (xbox_GuestUnwindCallIn(guest_body, g))
        InterlockedIncrement(&g->unwound);
    xbox_IrqlUnwindMark(&m1);
    InterlockedExchange(&g->mark_ok, mark_eq(&m0, &m1));
    xbox_GuestCpuPart();
    xbox_GuestUnwindPart();
    return 0;
}

static HANDLE spawn(Guest *g)
{
    g->counted = xbox_GuestUnwindSpawnBegin();
    return CreateThread(NULL, 0, guest_thread, g, 0, NULL);
}

static void wait_up(LONG n)
{
    uint64_t t0 = xbox_HostNowNs();
    while (InterlockedCompareExchange(&s_up, 0, 0) < n
           && xbox_HostNowNs() - t0 < 5000000000ull)
        Sleep(1);
}

static int join_all(HANDLE *t, int n)
{
    int i, ok = 1;
    for (i = 0; i < n; i++) {
        ok &= WaitForSingleObject(t[i], 20000) == WAIT_OBJECT_0;
        CloseHandle(t[i]);
    }
    InterlockedExchange(&s_stop, 0);
    InterlockedExchange(&s_up, 0);
    return ok;
}

static long kind(int k)
{
    XboxGuestUnwindStats s;
    xbox_GuestUnwindStatsGet(&s);
    return s.unwound[k];
}

/* ── Bodies ──────────────────────────────────────────────────────── */

static void body_loop(Guest *g)
{
    (void)g;
    while (!s_stop)
        back_edge(0x1000);
}

static void body_spin(Guest *g)
{
    (void)g;
    while (!s_stop)
        xbox_SpinWait(0x2000);
}

static HANDLE s_never;          /* an event nobody signals */

/* KeWaitForSingleObject as the bridge brackets it: the CPU let go. */
static void body_wait(Guest *g)
{
    int held;
    (void)g;
    held = xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 159);
    xbox_KeWaitForSingleObject(s_never, 0, 0, FALSE, NULL);
    if (held)
        xbox_GuestCpuAcquire();
}

static HANDLE s_never2;

/* KeWaitForMultipleObjects: two objects, any (WaitType 1) and all (0). On
 * Windows the first takes the unwind event as a third handle and the second
 * polls; on POSIX both are wait_multi's abortable wait. */
static void body_wait_any(Guest *g)
{
    PVOID objs[2];
    (void)g;
    objs[0] = s_never; objs[1] = s_never2;
    xbox_KeWaitForMultipleObjects(2, objs, 1, 0, 0, FALSE, NULL, NULL);
}

static void body_wait_all(Guest *g)
{
    PVOID objs[2];
    (void)g;
    objs[0] = s_never; objs[1] = s_never2;
    xbox_KeWaitForMultipleObjects(2, objs, 0, 0, 0, FALSE, NULL, NULL);
}

static void body_delay(Guest *g)
{
    LARGE_INTEGER li;
    (void)g;
    li.QuadPart = -600000000LL;                /* 60 s */
    xbox_KeDelayExecutionThread(0, FALSE, &li);
}

static void body_delay_alertable(Guest *g)
{
    LARGE_INTEGER li;
    (void)g;
    li.QuadPart = -600000000LL;
    xbox_KeDelayExecutionThread(0, TRUE, &li);
}

static RTL_CRITICAL_SECTION s_gcs;  /* a guest lock; its address is the key */
static volatile LONG s_cs_held;

static void body_cs_owner(Guest *g)
{
    (void)g;
    xbox_RtlEnterCriticalSection(&s_gcs);
    xbox_RtlEnterCriticalSection(&s_gcs);
    InterlockedExchange(&s_cs_held, 1);
    while (!s_stop)
        back_edge(0x3000);
}

static void body_cs_waiter(Guest *g)
{
    (void)g;
    while (!InterlockedCompareExchange(&s_cs_held, 0, 0))
        Sleep(1);
    xbox_RtlEnterCriticalSection(&s_gcs);
    xbox_RtlLeaveCriticalSection(&s_gcs);
}

static HANDLE s_mutant;
static volatile LONG s_mutant_held;

static void body_mutant_owner(Guest *g)
{
    (void)g;
    WaitForSingleObject(s_mutant, INFINITE);
    InterlockedExchange(&s_mutant_held, 1);
    while (!s_stop)
        back_edge(0x4000);
}

static void body_mutant_waiter(Guest *g)
{
    (void)g;
    while (!InterlockedCompareExchange(&s_mutant_held, 0, 0))
        Sleep(1);
    xbox_KeWaitForSingleObject(s_mutant, 0, 0, FALSE, NULL);
    ReleaseMutex(s_mutant);
}

static volatile LONG s_hold_refused, s_cs_taken;

static DWORD WINAPI cs_taker(LPVOID p)
{
    (void)p;
    xbox_RtlEnterCriticalSection(&s_gcs);
    xbox_RtlLeaveCriticalSection(&s_gcs);
    InterlockedExchange(&s_cs_taken, 1);
    return 0;
}

/* An ISR run on the thread, say: the worker stack it borrowed is the host
 * resource. Points inside refuse; the first one past the region unwinds. */
static void body_hold(Guest *g)
{
    XboxGuestUnwindStats s;
    uint64_t t0;
    (void)g;
    xbox_GuestUnwindHold(1);
    while (!g_xbox_reset_pending && !s_stop)
        Sleep(1);
    t0 = xbox_HostNowNs();
    do {
        xbox_GuestUnwindPoint(XBOX_UNWIND_BACK_EDGE, 0x7000);
        xbox_GuestUnwindStatsGet(&s);
    } while (s.refused == 0 && xbox_HostNowNs() - t0 < 2000000000ull);
    InterlockedExchange(&s_hold_refused, s.refused != 0);
    xbox_GuestUnwindHold(-1);
    while (!s_stop)
        back_edge(0x7000);
}

/* ── Cases ───────────────────────────────────────────────────────── */

static volatile LONG s_inert_ran;

static void inert_fn(void *p)
{
    (void)p;
    InterlockedIncrement(&s_inert_ran);
}

static void inert(void)
{
    XboxGuestUnwindStats s;

    xbox_GuestUnwindJoin(0);
    check(xbox_GuestUnwindCallIn(inert_fn, NULL) == 0 && s_inert_ran == 1,
          "inert: a call-in runs its function");
    check(xbox_GuestUnwindRequest() == 0 && !g_xbox_reset_pending
          && xbox_GuestUnwindHostBegin() == 1,
          "inert: a request raises nothing, host calls run");
    check(xbox_GuestUnwindWaitStopped(10) == 1, "inert: nothing to wait for");
    xbox_GuestUnwindStatsGet(&s);
    check(s.live == 0 && s.starting == 0 && xbox_GuestUnwindSpawnBegin() == 0,
          "inert: no thread tracked or counted");
}

static void lock_off(void)
{
#define N_OFF 12
    static Guest g[N_OFF];
    HANDLE t[N_OFF];
    void (*bodies[N_OFF])(Guest *) = {
        body_loop, body_spin, body_wait, body_delay, body_delay_alertable,
        body_cs_owner, body_cs_waiter, body_mutant_owner, body_mutant_waiter,
        body_hold, body_wait_any, body_wait_all,
    };
    long k0[XBOX_UNWIND_KINDS];
    XboxGuestUnwindStats s;
    int i, all;

    set_lock(NULL);
    s_never = CreateEventA(NULL, TRUE, FALSE, NULL);
    s_never2 = CreateEventA(NULL, TRUE, FALSE, NULL);
    s_mutant = CreateMutexA(NULL, FALSE, NULL);
    for (i = 0; i < XBOX_UNWIND_KINDS; i++)
        k0[i] = kind(i);
    memset(g, 0, sizeof g);
    for (i = 0; i < N_OFF; i++) {
        g[i].body = bodies[i];
        t[i] = spawn(&g[i]);
    }
    wait_up(N_OFF);
    Sleep(150);                 /* the waiters into their host waits */
    xbox_GuestUnwindStatsGet(&s);
    check(s.live == N_OFF && s.starting == 0,
          "lock off: twelve threads tracked");

    check(xbox_GuestUnwindRequest() == 1, "lock off: the request is taken");
    check(xbox_GuestUnwindWaitStopped(5000) == 1,
          "lock off: every thread unwinds within 5 s");
    all = 1;
    for (i = 0; i < N_OFF; i++)
        all &= g[i].unwound == 1 && g[i].returned == 0;
    check(all, "lock off: each one came back to its call-in, none returned");
    check(kind(XBOX_UNWIND_BACK_EDGE) - k0[XBOX_UNWIND_BACK_EDGE] >= 4,
          "lock off: back edges (loop, two owners, the hold)");
    check(kind(XBOX_UNWIND_SPIN) - k0[XBOX_UNWIND_SPIN] == 1,
          "lock off: the spin-wait");
    check(kind(XBOX_UNWIND_WAIT) - k0[XBOX_UNWIND_WAIT] >= 3,
          "lock off: the kernel waits (one object, any of two, all of two)");
    check(kind(XBOX_UNWIND_DELAY) - k0[XBOX_UNWIND_DELAY] == 2,
          "lock off: both delays");
    check(kind(XBOX_UNWIND_CS) - k0[XBOX_UNWIND_CS]
          + kind(XBOX_UNWIND_WAIT) - k0[XBOX_UNWIND_WAIT] >= 3,
          "lock off: the section and mutant waiters, at a section or wait");
    xbox_GuestUnwindStatsGet(&s);
    check(s.cs_released >= 2, "lock off: the owner's two recursions let go");
    check(s_hold_refused == 1, "hold: refused while held, unwound after");
    check(join_all(t, N_OFF), "lock off: every thread ended");
    all = 1;
    for (i = 0; i < N_OFF; i++)
        all &= g[i].mark_ok == 1;
    check(all, "lock off: each one back at its call-in's IRQL and gate");
    xbox_GuestUnwindStatsGet(&s);
    check(s.live == 0 && s.starting == 0 && s.parked == 0,
          "lock off: nobody left counted");
    xbox_GuestUnwindResume();
    check(!g_xbox_reset_pending, "resume: the flag is down");

    /* Freed, not abandoned: another thread takes it straight away. */
    {
        HANDLE h = CreateThread(NULL, 0, cs_taker, NULL, 0, NULL);
        check(WaitForSingleObject(h, 2000) == WAIT_OBJECT_0 && s_cs_taken,
              "lock off: another thread takes the section after");
        CloseHandle(h);
    }
    CloseHandle(s_never);
    CloseHandle(s_never2);
    CloseHandle(s_mutant);
    InterlockedExchange(&s_cs_held, 0);
    InterlockedExchange(&s_mutant_held, 0);
}

/* A thread that ends holding a lock (no unwind): its part drops the
 * records, so ReleaseOwned on a thread with its id finds none to leave. */
static RTL_CRITICAL_SECTION s_gcs2;
static volatile LONG s_forget_ok;

static DWORD WINAPI forget_thread(LPVOID p)
{
    (void)p;
    xbox_RtlEnterCriticalSection(&s_gcs2);
    xbox_RtlCsForgetOwned();
    InterlockedExchange(&s_forget_ok, xbox_RtlCsReleaseOwned() == 0);
    xbox_RtlLeaveCriticalSection(&s_gcs2);
    return 0;
}

static void forget(void)
{
    HANDLE h = CreateThread(NULL, 0, forget_thread, NULL, 0, NULL);
    check(WaitForSingleObject(h, 2000) == WAIT_OBJECT_0 && s_forget_ok,
          "forget: a part's records are gone, no lock left by its id");
    CloseHandle(h);
}

/* At DISPATCH under the one-CPU gate: the jump skips the lower, and the
 * call-in has to give the gate back. */
static void body_raised(Guest *g)
{
    (void)g;
    g_fs_base = 0;              /* no guest KPCR to mirror the IRQL into */
    xbox_KfRaiseIrql(DISPATCH_LEVEL);
    InterlockedExchange(&s_cs_held, 1);
    while (!s_stop)
        back_edge(0xA000);
}

static void raised(void)
{
    static Guest g;
    HANDLE t;
    KIRQL old;

    recomp_env_set(RENV_DISPATCH_GATE, "1");
    xbox_IrqReloadConfig();
    set_lock(NULL);
    memset(&g, 0, sizeof g);
    g.body = body_raised;
    InterlockedExchange(&s_cs_held, 0);
    t = spawn(&g);
    wait_up(1);
    while (!InterlockedCompareExchange(&s_cs_held, 0, 0))
        Sleep(1);
    check(xbox_DispatchGateOn(), "raised: the gate is on");
    xbox_GuestUnwindRequest();
    check(xbox_GuestUnwindWaitStopped(5000) == 1 && join_all(&t, 1),
          "raised: the thread at DISPATCH unwinds");
    check(g.unwound == 1 && g.mark_ok == 1,
          "raised: its IRQL, gate depth and count are back at the mark");
    xbox_GuestUnwindResume();
    /* The gate is free: this thread takes it (a hang trips the watchdog). */
    g_fs_base = 0;
    old = xbox_KfRaiseIrql(DISPATCH_LEVEL);
    xbox_KfLowerIrql(old);
    check(1, "raised: the gate is free after");
    InterlockedExchange(&s_cs_held, 0);
    recomp_env_set(RENV_DISPATCH_GATE, NULL);
    xbox_IrqReloadConfig();
}

static void lock_on(int rounds)
{
    static Guest g[6];
    HANDLE t[6];
    int r, i, all = 1;

    recomp_env_set(RENV_GUEST_CPU_TRACE, "1000000");  /* the stats records */
    set_lock("1");
    for (r = 0; r < rounds; r++) {
        s_never = CreateEventA(NULL, TRUE, FALSE, NULL);
        memset(g, 0, sizeof g);
        for (i = 0; i < 6; i++) {
            g[i].body = i == 5 ? body_wait : i == 4 ? body_spin : body_loop;
            t[i] = spawn(&g[i]);
        }
        /* One holds the CPU, the others queue for it or wait. */
        Sleep(100);
        xbox_GuestUnwindRequest();
        all &= xbox_GuestUnwindWaitStopped(5000) == 1;
        all &= join_all(t, 6);
        for (i = 0; i < 6; i++)
            all &= g[i].unwound == 1;
        all &= xbox_GuestCpuOwner() == 0;
        xbox_GuestUnwindResume();
        CloseHandle(s_never);
    }
    check(all, "lock on: holder and queued threads unwind, the CPU is free");
    check(kind(XBOX_UNWIND_ACQUIRE) + kind(XBOX_UNWIND_CALL_IN) > 0,
          "lock on: some unwound in the lock's queue or before guest code");
    check(xbox_GuestCpuThreadRecsUsed() == 0,
          "records: 18 threads in a 16-slot table, none left taken");
    recomp_env_set(RENV_GUEST_CPU_TRACE, NULL);
    set_lock(NULL);
}

static void join_race(void)
{
    static Guest g;
    HANDLE t;
    long c0 = kind(XBOX_UNWIND_CALL_IN);
    int counted;

    memset(&g, 0, sizeof g);
    g.body = body_loop;
    counted = xbox_GuestUnwindSpawnBegin();
    xbox_GuestUnwindRequest();
    check(counted && xbox_GuestUnwindWaitStopped(100) == 0,
          "join race: a counted thread not yet joined keeps the reset waiting");
    g.counted = counted;
    t = CreateThread(NULL, 0, guest_thread, &g, 0, NULL);
    check(xbox_GuestUnwindWaitStopped(5000) == 1,
          "join race: it joins, sees the flag and parts");
    WaitForSingleObject(t, 20000);
    CloseHandle(t);
    check(g.ran == 0 && g.unwound == 1
          && kind(XBOX_UNWIND_CALL_IN) - c0 == 1,
          "join race: its call-in ran no guest code");
    xbox_GuestUnwindResume();

    counted = xbox_GuestUnwindSpawnBegin();
    xbox_GuestUnwindSpawnCancel(counted);
    {
        XboxGuestUnwindStats s;
        xbox_GuestUnwindStatsGet(&s);
        check(s.starting == 0, "join race: a failed create is uncounted");
    }
}

static volatile LONG s_boot_runs;

static void boot_body(void *p)
{
    (void)p;
    InterlockedIncrement(&s_boot_runs);
    InterlockedIncrement(&s_up);
    while (!s_stop)
        back_edge(0x9000);
}

static DWORD WINAPI boot_thread(LPVOID p)
{
    int counted = (int)(intptr_t)p;

    xbox_GuestUnwindJoin(counted);
    xbox_GuestCpuJoin();
    while (xbox_GuestUnwindCallIn(boot_body, NULL))
        xbox_GuestUnwindPark();
    xbox_GuestCpuPart();
    xbox_GuestUnwindPart();
    return 0;
}

static void park(void)
{
    XboxGuestUnwindStats s;
    HANDLE t;
    int counted;

    set_lock("1");
    s_boot_runs = 0;
    counted = xbox_GuestUnwindSpawnBegin();
    t = CreateThread(NULL, 0, boot_thread, (LPVOID)(intptr_t)counted, 0, NULL);
    wait_up(1);
    xbox_GuestUnwindRequest();
    check(xbox_GuestUnwindWaitStopped(5000) == 1,
          "park: a parked boot loop counts as stopped");
    xbox_GuestUnwindStatsGet(&s);
    check(s.parked == 1 && s.live == 1 && xbox_GuestCpuOwner() == 0,
          "park: it let go of the guest CPU while parked");
    InterlockedExchange(&s_up, 0);
    xbox_GuestUnwindResume();
    wait_up(1);
    check(s_boot_runs == 2, "park: after the resume it runs guest code again");
    InterlockedExchange(&s_stop, 1);
    check(WaitForSingleObject(t, 20000) == WAIT_OBJECT_0,
          "park: and returns normally when its code does");
    CloseHandle(t);
    InterlockedExchange(&s_stop, 0);
    InterlockedExchange(&s_up, 0);
    set_lock(NULL);
}

/* ── The deterministic ring ──────────────────────────────────────── */

#define OBJ(i) (0x80010000u + 0x100u * (uint32_t)(i))

static void body_det(Guest *g)
{
    uint32_t k = 0;
    (void)g;
    while (!s_stop)
        xbox_GuestDetYield(0x5000u + k++);
}

static int all_parted(void *ctx)
{
    XboxGuestUnwindStats s;
    (void)ctx;
    xbox_GuestUnwindStatsGet(&s);
    return s.live == 0 && s.starting == 0;
}

static void det(void)
{
    static Guest g[3];
    HANDLE t[3];
    int i, all = 1, round;
    long d0 = kind(XBOX_UNWIND_DET);

    set_lock("1");
    xbox_GuestTimeTestMode(1);
    for (round = 0; round < 2; round++) {
        xbox_GuestDetTestReset(1);
        xbox_GuestDetBind(OBJ(0));
        xbox_GuestDetJoin();
        memset(g, 0, sizeof g);
        for (i = 0; i < 3; i++) {
            g[i].body = body_det;
            g[i].obj = OBJ(i + 1);
            xbox_GuestDetThreadNew(g[i].obj);
            t[i] = spawn(&g[i]);
        }
        /* The main thread hands the token round until they all run. */
        while (InterlockedCompareExchange(&s_up, 0, 0) < 3)
            xbox_GuestDetYield(0x100u);
        xbox_GuestUnwindRequest();
        xbox_GuestDetBlock(all_parted, NULL, 0, 0x200u);
        all &= xbox_GuestUnwindWaitStopped(5000) == 1;
        xbox_GuestDetPart();
        all &= join_all(t, 3);
        for (i = 0; i < 3; i++)
            all &= g[i].unwound == 1;
        xbox_GuestUnwindResume();
    }
    check(all, "det: ring threads unwind and part, twice over");
    check(kind(XBOX_UNWIND_DET) - d0 >= 2,
          "det: the waiters for the token unwound at the ring's turn");
    xbox_GuestDetTestReset(0);
    xbox_GuestTimeTestMode(0);
    set_lock(NULL);
}

/* ── The registry's limit ────────────────────────────────────────── */

static void cap(void)
{
    static Guest g[65];
    HANDLE t[65];
    XboxGuestUnwindStats s;
    int i;

    set_lock(NULL);
    memset(g, 0, sizeof g);
    for (i = 0; i < 65; i++) {
        g[i].body = body_loop;
        t[i] = spawn(&g[i]);
    }
    wait_up(65);
    xbox_GuestUnwindStatsGet(&s);
    check(s.live == 64 && s.untracked == 1,
          "cap: 64 tracked, the 65th counted as untracked");
    check(xbox_GuestUnwindRequest() == 0 && !g_xbox_reset_pending,
          "cap: a request refuses while it lives");
    InterlockedExchange(&s_stop, 1);
    check(join_all(t, 65), "cap: all 65 end");
    xbox_GuestUnwindStatsGet(&s);
    check(s.live == 0 && s.untracked == 0, "cap: nobody left counted");
    check(xbox_GuestUnwindRequest() == 1 && xbox_GuestUnwindWaitStopped(1000),
          "cap: then a request is taken");
    xbox_GuestUnwindResume();
}

/* ── Host call-ins ───────────────────────────────────────────────── */

static volatile LONG s_host_in;

static DWORD WINAPI host_caller(LPVOID p)
{
    int tok = xbox_GuestUnwindHostBegin();
    (void)p;
    InterlockedExchange(&s_host_in, tok);
    Sleep(300);                 /* the guest call */
    xbox_GuestUnwindHostEnd(tok);
    return 0;
}

static void host_calls(void)
{
    XboxGuestUnwindStats s;
    HANDLE h;
    int tok;

    InterlockedExchange(&s_host_in, 0);
    h = CreateThread(NULL, 0, host_caller, NULL, 0, NULL);
    while (!InterlockedCompareExchange(&s_host_in, 0, 0))
        Sleep(1);
    xbox_GuestUnwindRequest();
    xbox_GuestUnwindStatsGet(&s);
    check(s.host_calls == 1 && xbox_GuestUnwindWaitStopped(50) == 0,
          "host: a call-in running keeps the reset waiting");
    check(xbox_GuestUnwindWaitStopped(5000) == 1,
          "host: and it is stopped once the call ends");
    check(xbox_GuestUnwindHostBegin() == 0,
          "host: a new call-in is skipped while the reset is pending");
    xbox_GuestUnwindStatsGet(&s);
    check(s.host_calls == 0, "host: the skipped one is not counted");
    xbox_GuestUnwindResume();
    tok = xbox_GuestUnwindHostBegin();
    check(tok == 2, "host: after the resume it runs, counted");
    xbox_GuestUnwindHostEnd(tok);
    WaitForSingleObject(h, 5000);
    CloseHandle(h);
}

int main(void)
{
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    printf("guest_unwind:\n");
    inert();
    xbox_GuestUnwindEnable();
    lock_off();
    forget();
    raised();
    lock_on(3);
    join_race();
    park();
    det();
    cap();
    host_calls();
    printf("guest_unwind: %s (%d failures)\n", failures ? "FAIL" : "PASS",
           failures);
    return failures ? 1 : 0;
}
