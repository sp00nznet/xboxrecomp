/*
 * kernel_guest_unwind.c - the guest unwind (see kernel_guest_unwind.h).
 *
 * The registry knows each tracked guest thread by a slot with a manual
 * reset event: the request sets every slot's event, so a thread blocked in
 * a kernel wait (which waits on it too) wakes and unwinds instead of
 * waiting for an object nobody will signal now. A creator counts a thread
 * it starts (SpawnBegin) until the thread joins, so the reset never sees
 * "everyone stopped" while a CreateThread is in flight; a thread that joins
 * while a reset is pending finds its event set and its call-in refuses to
 * run guest code.
 *
 * The jump: the thread's call-in made a jmp_buf with setjmp. Lifted frames
 * own no host resources (they are C over the guest's registers and memory),
 * so a longjmp past them leaks nothing. What host state a point can sit
 * inside is the one-CPU gate and the IRQL (put back from the call-in's mark),
 * the guest critical sections this thread owns (let go before the jump, so
 * their waiters wake), the guest CPU (let go by the thread's exit, or by a
 * park) and the regions that hold more (xbox_GuestUnwindHold), where a point
 * refuses.
 */
#include "kernel.h"
#include "kernel_guest_unwind.h"
#include "kernel_reset_probe.h"
#include "platform/host_time.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* No signal mask on POSIX: _setjmp saves only the registers. On Windows
 * x64 (MSVC and mingw alike) longjmp is a structured unwind (RtlUnwindEx):
 * it walks every frame between the point and the call-in by its unwind
 * info. Compiled C has that; a hand-written asm frame on the path must
 * carry its own (.seh_* directives), or stay off a path that reaches a
 * point. */
#ifdef _WIN32
#define UNWIND_SETJMP(b)  setjmp(b)
#define UNWIND_LONGJMP(b) longjmp(b, 1)
#else
#define UNWIND_SETJMP(b)  _setjmp(b)
#define UNWIND_LONGJMP(b) _longjmp(b, 1)
#endif

volatile int32_t g_xbox_reset_pending;
volatile int32_t g_xbox_unwind_on;

#define UNWIND_SLOTS 64

typedef struct {
    int used;
    unsigned long tid;
    HANDLE ev;                  /* manual reset, kept for the slot's next thread */
} UnwindSlot;

static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION s_lock;
static CONDITION_VARIABLE s_cv;
static UnwindSlot s_slot[UNWIND_SLOTS];
static int s_live, s_starting, s_parked;
static volatile LONG s_unwound[XBOX_UNWIND_KINDS];
static volatile LONG s_refused, s_cs_released, s_full_said, s_foreign;
static volatile LONG s_untracked, s_host_calls;

static XBOX_THREAD_LOCAL int t_slot1;           /* slot + 1; 0 untracked */
static XBOX_THREAD_LOCAL int t_untracked;       /* counted in s_untracked */
static XBOX_THREAD_LOCAL HANDLE t_ev;
static XBOX_THREAD_LOCAL jmp_buf *t_target;     /* the outermost call-in's */
static XBOX_THREAD_LOCAL int t_hold;

static BOOL CALLBACK unwind_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_lock);
    InitializeConditionVariable(&s_cv);
    return TRUE;
}

static void unwind_once(void)
{
    InitOnceExecuteOnce(&s_once, unwind_init, NULL, NULL);
}

void xbox_GuestUnwindEnable(void)
{
    unwind_once();
    InterlockedExchange((volatile LONG *)&g_xbox_unwind_on, 1);
}

int xbox_GuestUnwindEnabled(void)
{
    return InterlockedCompareExchange((volatile LONG *)&g_xbox_unwind_on, 0, 0) != 0;
}

static const char *kind_name(int kind)
{
    switch (kind) {
    case XBOX_UNWIND_BACK_EDGE: return "back_edge";
    case XBOX_UNWIND_SPIN:      return "spin";
    case XBOX_UNWIND_KICK:      return "spin_kick";
    case XBOX_UNWIND_ACQUIRE:   return "cpu_acquire";
    case XBOX_UNWIND_BRIDGE:    return "bridge";
    case XBOX_UNWIND_WAIT:      return "wait";
    case XBOX_UNWIND_DELAY:     return "delay";
    case XBOX_UNWIND_CS:        return "critical_section";
    case XBOX_UNWIND_DET:       return "det_turn";
    case XBOX_UNWIND_CALL_IN:   return "call_in";
    default:                    return "?";
    }
}

/* The guest CPU lock and the ring clear their flags at a hand-off; while a
 * reset is pending they must stay up, so every back edge and spin pass of a
 * thread that holds the CPU calls in. The waiters flag is written without
 * the guest CPU lock, as a hint: a hand-off that clears it again is
 * undone by WaitStopped, which raises both every pass. */
static void raise_flags(void)
{
    InterlockedExchange((volatile LONG *)&g_xbox_guest_cpu_preempt, 1);
    g_xbox_guest_cpu_waiters = 1;
}

/* ── The point ───────────────────────────────────────────── */

int xbox_GuestUnwindArmed(void)
{
    return g_xbox_reset_pending && t_target && t_hold == 0;
}

void xbox_GuestUnwindSlow(int kind, uint32_t where)
{
    jmp_buf *b = t_target;

    if (!b || !g_xbox_reset_pending)
        return;
    if (t_hold > 0) {
        if (InterlockedIncrement(&s_refused) <= 8) {
            fprintf(stderr, "[RESET] unwind refused: tid %lu at %s 0x%X holds"
                            " a host resource; it unwinds at its next point\n",
                    (unsigned long)GetCurrentThreadId(), kind_name(kind),
                    (unsigned)where);
            fflush(stderr);
        }
        return;
    }
    if (kind > 0 && kind < XBOX_UNWIND_KINDS)
        InterlockedIncrement(&s_unwound[kind]);
    fprintf(stderr, "[RESET] tid %lu unwinds at %s 0x%X\n",
            (unsigned long)GetCurrentThreadId(), kind_name(kind),
            (unsigned)where);
    fflush(stderr);
    xbox_ResetProbeState(XBOX_RESET_RUN, 0);
    /* The owner leaving its critical sections is defined behaviour, and it
     * wakes the threads blocked in them; a guest mutant it owns stays
     * owned (abandoned), its waiters woken by their own unwind events. */
    InterlockedExchangeAdd(&s_cs_released, (LONG)xbox_RtlCsReleaseOwned());
    UNWIND_LONGJMP(*b);
}

void xbox_GuestUnwindForeign(int delta)
{
    InterlockedExchangeAdd(&s_foreign, delta);
}

void xbox_GuestUnwindHold(int delta)
{
    t_hold += delta;
}

/* ── The call-in ─────────────────────────────────────────── */

int xbox_GuestUnwindCallIn(void (*fn)(void *), void *arg)
{
    jmp_buf buf;
    XboxIrqlMark mark;

    if (!t_slot1 || t_target) {
        fn(arg);
        return 0;
    }
    if (g_xbox_reset_pending) {
        InterlockedIncrement(&s_unwound[XBOX_UNWIND_CALL_IN]);
        return 1;
    }
    xbox_IrqlUnwindMark(&mark);
    if (UNWIND_SETJMP(buf)) {
        t_target = NULL;
        xbox_IrqlUnwindRestore(&mark);
        return 1;
    }
    t_target = &buf;
    fn(arg);
    t_target = NULL;
    return 0;
}

/* ── A thread's life ─────────────────────────────────────── */

int xbox_GuestUnwindSpawnBegin(void)
{
    if (!xbox_GuestUnwindEnabled())
        return 0;
    EnterCriticalSection(&s_lock);
    s_starting++;
    LeaveCriticalSection(&s_lock);
    return 1;
}

void xbox_GuestUnwindSpawnCancel(int counted)
{
    if (!counted)
        return;
    EnterCriticalSection(&s_lock);
    if (s_starting > 0)
        s_starting--;
    WakeAllConditionVariable(&s_cv);
    LeaveCriticalSection(&s_lock);
}

void xbox_GuestUnwindJoin(int counted)
{
    int i;

    if (!xbox_GuestUnwindEnabled())
        return;
    EnterCriticalSection(&s_lock);
    if (counted && s_starting > 0)
        s_starting--;
    if (t_slot1 || t_untracked) {
        LeaveCriticalSection(&s_lock);
        return;
    }
    for (i = 0; i < UNWIND_SLOTS; i++)
        if (!s_slot[i].used)
            break;
    if (i < UNWIND_SLOTS && !s_slot[i].ev)
        s_slot[i].ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (i == UNWIND_SLOTS || !s_slot[i].ev) {
        /* Not tracked, so a reset could not stop it: counted, and a
         * request refuses while it lives. */
        int full = i == UNWIND_SLOTS;
        s_untracked++;
        t_untracked = 1;
        WakeAllConditionVariable(&s_cv);
        LeaveCriticalSection(&s_lock);
        if (!full || !InterlockedExchange(&s_full_said, 1)) {
            if (full)
                fprintf(stderr, "[RESET] more than %d guest threads: tid %lu"
                                " is not tracked; no reset while it runs\n",
                        UNWIND_SLOTS, (unsigned long)GetCurrentThreadId());
            else
                fprintf(stderr, "[RESET] no unwind event for tid %lu (error"
                                " %lu): not tracked; no reset while it runs\n",
                        (unsigned long)GetCurrentThreadId(),
                        (unsigned long)GetLastError());
            fflush(stderr);
        }
        return;
    }
    s_slot[i].used = 1;
    s_slot[i].tid = (unsigned long)GetCurrentThreadId();
    if (g_xbox_reset_pending)
        SetEvent(s_slot[i].ev);
    else
        ResetEvent(s_slot[i].ev);
    s_live++;
    t_slot1 = i + 1;
    t_ev = s_slot[i].ev;
    WakeAllConditionVariable(&s_cv);
    LeaveCriticalSection(&s_lock);
}

void xbox_GuestUnwindPart(void)
{
    int i = t_slot1 - 1;

    if (t_untracked) {
        xbox_RtlCsForgetOwned();
        EnterCriticalSection(&s_lock);
        s_untracked--;
        LeaveCriticalSection(&s_lock);
        t_untracked = 0;
        return;
    }
    if (i < 0)
        return;
    /* Locks it still owns (a thread that ends holding one) stay held, as
     * before; only the records that would let a thread with its id leave
     * them go. */
    xbox_RtlCsForgetOwned();
    EnterCriticalSection(&s_lock);
    s_slot[i].used = 0;
    s_slot[i].tid = 0;
    ResetEvent(s_slot[i].ev);
    s_live--;
    WakeAllConditionVariable(&s_cv);
    LeaveCriticalSection(&s_lock);
    t_slot1 = 0;
    t_ev = NULL;
    t_target = NULL;
    t_hold = 0;
}

/* ── The waits ───────────────────────────────────────────── */

static DWORD plain_wait(DWORD n, const HANDLE *h, BOOL all, DWORD ms,
                        BOOL alertable)
{
    if (n == 1)
        return WaitForSingleObjectEx(h[0], ms, alertable);
    return WaitForMultipleObjectsEx(n, h, all, ms, alertable);
}

#ifdef _WIN32
/* Win32 has no way to add an event to a wait-all: that one, and a wait on
 * the most objects there are, poll the flag in 20 ms slices. */
#define UNWIND_SLICE_MS 20u

static DWORD unwind_wait_host(HANDLE ev, DWORD n, const HANDLE *h, BOOL all,
                              DWORD ms, BOOL alertable, int *aborted)
{
    DWORD r;

    if (!all && n < MAXIMUM_WAIT_OBJECTS) {
        HANDLE hs[MAXIMUM_WAIT_OBJECTS];
        memcpy(hs, h, n * sizeof *h);
        hs[n] = ev;
        r = WaitForMultipleObjectsEx(n + 1, hs, FALSE, ms, alertable);
        *aborted = r == WAIT_OBJECT_0 + n;
        return r;
    }
    for (;;) {
        DWORD s = ms == INFINITE || ms > UNWIND_SLICE_MS ? UNWIND_SLICE_MS : ms;
        r = plain_wait(n, h, all, s, alertable);
        if (r != WAIT_TIMEOUT)
            return r;
        if (g_xbox_reset_pending) {
            *aborted = 1;
            return r;
        }
        if (ms != INFINITE) {
            ms -= s;
            if (!ms)
                return WAIT_TIMEOUT;
        }
    }
}
#else
static DWORD unwind_wait_host(HANDLE ev, DWORD n, const HANDLE *h, BOOL all,
                              DWORD ms, BOOL alertable, int *aborted)
{
    DWORD r = w32_wait_abortable(ev, n, h, all, ms, alertable);

    *aborted = r == WAIT_OBJECT_0 + n;
    return r;
}
#endif

DWORD xbox_GuestUnwindWait(DWORD n, const HANDLE *h, BOOL all, DWORD ms,
                           BOOL alertable)
{
    HANDLE ev = t_ev;
    int aborted = 0;
    DWORD r;

    if (!ev || !t_target || !n)
        return plain_wait(n, h, all, ms, alertable);
    xbox_GuestUnwindPoint(XBOX_UNWIND_WAIT, 0);
    r = unwind_wait_host(ev, n, h, all, ms, alertable, &aborted);
    if (!aborted)
        return r;
    xbox_GuestUnwindSlow(XBOX_UNWIND_WAIT, 1);
    /* Refused (a held region): wait as asked, without the event, which
     * stays set until the reset ends. */
    return plain_wait(n, h, all, ms, alertable);
}

DWORD xbox_GuestUnwindSleep(DWORD ms, BOOL alertable)
{
    HANDLE ev = t_ev;
    DWORD r;

    /* A zero sleep is a yield, which a wait on an event is not. */
    if (!ev || !t_target || !ms)
        return SleepEx(ms, alertable);
    xbox_GuestUnwindPoint(XBOX_UNWIND_DELAY, 0);
    r = WaitForSingleObjectEx(ev, ms, alertable);
    if (r == WAIT_IO_COMPLETION)
        return WAIT_IO_COMPLETION;
    if (r != WAIT_OBJECT_0)
        return 0;
    xbox_GuestUnwindSlow(XBOX_UNWIND_DELAY, 1);
    return SleepEx(ms, alertable);
}

/* ── The request ─────────────────────────────────────────── */

static void set_events(void)
{
    int i;

    for (i = 0; i < UNWIND_SLOTS; i++)
        if (s_slot[i].used)
            SetEvent(s_slot[i].ev);
}

int xbox_GuestUnwindRequest(void)
{
    if (!xbox_GuestUnwindEnabled())
        return 0;
    if (InterlockedCompareExchange(&s_untracked, 0, 0) > 0) {
        fprintf(stderr, "[RESET] refused: %ld guest threads are not tracked"
                        " and would run on through the reset\n",
                (long)s_untracked);
        fflush(stderr);
        return 0;
    }
    if (InterlockedCompareExchange(&s_foreign, 0, 0) > 0) {
        fprintf(stderr, "[RESET] %ld guest threads from xbox_PsCreateSystemThreadEx"
                        " are live; they have no call-in to unwind to\n",
                (long)s_foreign);
        fflush(stderr);
        abort();
    }
    InterlockedExchange((volatile LONG *)&g_xbox_reset_pending, 1);
    raise_flags();
    EnterCriticalSection(&s_lock);
    set_events();
    LeaveCriticalSection(&s_lock);
    xbox_GuestDetUnwindWake();
    return 1;
}

/* ── Host call-ins ───────────────────────────────────────── */

int xbox_GuestUnwindHostBeginSlow(void)
{
    InterlockedIncrement(&s_host_calls);
    if (!InterlockedCompareExchange((volatile LONG *)&g_xbox_reset_pending, 0, 0))
        return 2;
    xbox_GuestUnwindHostEndSlow();
    return 0;
}

void xbox_GuestUnwindHostEndSlow(void)
{
    if (InterlockedDecrement(&s_host_calls) == 0 && g_xbox_reset_pending) {
        EnterCriticalSection(&s_lock);
        WakeAllConditionVariable(&s_cv);
        LeaveCriticalSection(&s_lock);
    }
}

int xbox_GuestUnwindWaitStopped(uint32_t timeout_ms)
{
    uint64_t end = xbox_HostNowNs() + (uint64_t)timeout_ms * 1000000ull;
    int done;

    if (!xbox_GuestUnwindEnabled())
        return 1;
    EnterCriticalSection(&s_lock);
    for (;;) {
        /* An untracked thread that joined after the request's check never
         * stops: the reset times out rather than run past it. */
        done = s_live == s_parked && s_starting == 0 && s_untracked == 0
               && InterlockedCompareExchange(&s_host_calls, 0, 0) == 0;
        if (done || xbox_HostNowNs() >= end)
            break;
        /* A hand-off since the last look cleared them. */
        raise_flags();
        set_events();
        LeaveCriticalSection(&s_lock);
        xbox_GuestDetUnwindWake();
        EnterCriticalSection(&s_lock);
        SleepConditionVariableCS(&s_cv, &s_lock, 10);
    }
    if (!done) {
        int i;
        fprintf(stderr, "[RESET] %d of %d guest threads still running after"
                        " %u ms (%d starting, %ld untracked, %ld host calls):",
                s_live - s_parked, s_live, (unsigned)timeout_ms, s_starting,
                (long)s_untracked, (long)s_host_calls);
        for (i = 0; i < UNWIND_SLOTS; i++)
            if (s_slot[i].used)
                fprintf(stderr, " %lu", s_slot[i].tid);
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    LeaveCriticalSection(&s_lock);
    return done;
}

void xbox_GuestUnwindPark(void)
{
    int how;

    if (!t_slot1)
        return;
    how = xbox_GuestCpuUnwindDrop();
    EnterCriticalSection(&s_lock);
    s_parked++;
    WakeAllConditionVariable(&s_cv);
    while (g_xbox_reset_pending)
        SleepConditionVariableCS(&s_cv, &s_lock, 50);
    s_parked--;
    ResetEvent(t_ev);
    LeaveCriticalSection(&s_lock);
    xbox_GuestCpuUnwindRetake(how);
}

void xbox_GuestUnwindResume(void)
{
    int i;

    if (!xbox_GuestUnwindEnabled())
        return;
    EnterCriticalSection(&s_lock);
    InterlockedExchange((volatile LONG *)&g_xbox_reset_pending, 0);
    for (i = 0; i < UNWIND_SLOTS; i++)
        if (s_slot[i].used)
            ResetEvent(s_slot[i].ev);
    WakeAllConditionVariable(&s_cv);
    LeaveCriticalSection(&s_lock);
}

void xbox_GuestUnwindStatsGet(XboxGuestUnwindStats *out)
{
    int k;

    memset(out, 0, sizeof *out);
    for (k = 0; k < XBOX_UNWIND_KINDS; k++)
        out->unwound[k] = s_unwound[k];
    out->refused = s_refused;
    out->cs_released = s_cs_released;
    out->untracked = (int)s_untracked;
    out->host_calls = (int)s_host_calls;
    if (!xbox_GuestUnwindEnabled())
        return;
    EnterCriticalSection(&s_lock);
    out->live = s_live;
    out->starting = s_starting;
    out->parked = s_parked;
    LeaveCriticalSection(&s_lock);
}
