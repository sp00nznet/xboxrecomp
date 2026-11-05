/*
 * The walker's event wait (nv2a_kick_wake.c).
 *
 * The NV2A ack loop sleeps between ticks until the guest's KickOff pokes it.
 * A lost wake-up there is a kick that waits for the 1 ms tick, a frame that
 * runs late, so the stress case gives the walker a 200 ms timeout and the
 * guest one poke per store, no retries: any lost wake shows as a timeout
 * that finds work pending. The other cases pin the fallback (a sleep nobody
 * ends times out), that a poke with nothing pending and a poke during a held
 * flip do not wake, that a nudge does, that no wake is left latched for the
 * next sleep, the PUT watch and its carry across ticks, and the miss limit
 * that turns the wait off.
 *
 * Two more stress cases take the guest and the timer thread as they really
 * are. The guest's KickOff, at sleep pacing, makes one pass of its lowered
 * spin and then sleeps until the ack: a plain store of the flush bit, then
 * RECOMP_SPIN_WAIT's fence and plain read of `armed` (as recomp_types.h
 * defines it), once. Without that fence the store and the read reorder and
 * about one poke in a thousand is lost (the run prints the count of the
 * unfenced pass beside it, as a check that the case can see the bug). The
 * timer thread counts a vblank and then nudges, while the walker sleeps on
 * a held flip from the count it last saw: a vblank between that read and
 * the arm must still end the sleep. The timer thread pauses now and then,
 * so a nudge lost there is not covered by the next vblank's.
 *
 * A timeout that finds work pending is a lost wake only when the guest had
 * finished its store and its poke before the walker's sleep returned. One
 * that had not was preempted between the two (a CPU-oversubscribed host
 * deschedules it for longer than the sleep), and its store landed after
 * the walker had stopped listening; those are counted, not failed.
 *
 * Every case runs once per waker form the host has. A Windows build
 * (cross-built, run under tests/proton_run.sh) runs the condition variable
 * Wine uses and the native event plus high-resolution timer, which Wine
 * implements too, so the native form's protocol is covered off Windows.
 */
#include "nv2a_kick_wake.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

#define KICK_BIT 0x10000u
#define MS 1000000ull
#define US 1000ull

/* ---- threads ----------------------------------------------------------- */

#ifdef _WIN32
typedef HANDLE test_thread;
typedef struct { void *(*fn)(void *); void *arg; } thunk;
static DWORD WINAPI thread_thunk(LPVOID p)
{
    thunk t = *(thunk *)p;
    free(p);
    t.fn(t.arg);
    return 0;
}
static void thread_start(test_thread *t, void *(*fn)(void *), void *arg)
{
    thunk *p = (thunk *)malloc(sizeof *p);
    p->fn = fn;
    p->arg = arg;
    *t = CreateThread(NULL, 0, thread_thunk, p, 0, NULL);
}
static void thread_join(test_thread t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
typedef pthread_t test_thread;
static void thread_start(test_thread *t, void *(*fn)(void *), void *arg)
{
    pthread_create(t, NULL, fn, arg);
}
static void thread_join(test_thread t) { pthread_join(t, NULL); }
static void sleep_ms(unsigned ms)
{
    struct timespec ts = { 0, (long)ms * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

/* The CPU's counter, for timing the walker's sleeps in the one-pass case:
 * a clock call there shifts the race the case is after, this barely does.
 * Its rate comes from ticks_per_ms(). */
static inline uint64_t ticks(void)
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#elif defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#else
    return xbox_HostNowNs();
#endif
}

static uint64_t ticks_per_ms(void)
{
    uint64_t t0 = xbox_HostNowNs(), c0 = ticks(), t1, c1;

    sleep_ms(20);
    t1 = xbox_HostNowNs();
    c1 = ticks();
    return (c1 - c0) * MS / (t1 - t0);
}

/* ---- harness ----------------------------------------------------------- */

static int failures;
static XboxHostWakerForm g_form;
static volatile int armed;
static volatile uint32_t kick, put, done_put;
static volatile uint32_t vblanks;

static uint32_t vblank_count(void)
{
    return __atomic_load_n(&vblanks, __ATOMIC_SEQ_CST);
}

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static XboxHostWaker *form_waker(XboxHostWakerForm form)
{
    static XboxHostWaker *wakers[3];
    if (!wakers[form])
        wakers[form] = xbox_HostWakerCreateForm(form);
    return wakers[form];
}

static void reset(XboxKickWake *k)
{
    XboxHostWaker *waker = form_waker(g_form);
    /* Drain anything a case left latched. */
    while (xbox_HostWakerSleepNs(waker, 0))
        ;
    armed = 0;
    kick = 0;
    put = done_put = 0;
    vblanks = 0;
    *k = (XboxKickWake){ .armed = &armed, .waker = waker, .kick = &kick,
                         .kick_bit = KICK_BIT, .put = &put, .done_put = &done_put,
                         .vblank_count = vblank_count };
}

/* The guest's progress, for telling a lost wake from a preempted guest:
 * `started` before a store, `done` and `done_ns` after its poke. The clock
 * is read before the store and after the poke, never between, where it
 * would hide the reorder the fence exists for. */
static volatile uint32_t g_started, g_done;
static volatile uint64_t g_done_ns;

static void step_start(uint32_t n)
{
    __atomic_store_n(&g_started, n, __ATOMIC_RELEASE);
}

static void step_done(uint32_t n)
{
    __atomic_store_n(&g_done_ns, xbox_HostNowNs(), __ATOMIC_RELAXED);
    __atomic_store_n(&g_done, n, __ATOMIC_RELEASE);
}

/* After a timeout that found work pending, read at `returned` (the clock
 * right after the sleep came back): 1 when every step the guest started had
 * poked before then, so the poke saw the walker armed and must have woken
 * it. 0 when the guest was still between its store and its poke. */
static int guest_finished_before(uint64_t returned)
{
    uint32_t done = __atomic_load_n(&g_done, __ATOMIC_ACQUIRE);
    uint64_t at = __atomic_load_n(&g_done_ns, __ATOMIC_RELAXED);
    return done == __atomic_load_n(&g_started, __ATOMIC_ACQUIRE) && at < returned;
}

/* ---- stress: no lost wake-up ------------------------------------------ */

#define ROUNDS 20000

static XboxKickWake g_k;
static volatile int g_stop;

/* D3D's KickOff, minus the spin's repeated passes: raise the flush bit and
 * poke once, wait for the ack, store PUT and poke once. */
static void *guest(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < ROUNDS && !g_stop; i++) {
        step_start(2 * i + 1);
        __atomic_fetch_or(&kick, KICK_BIT, __ATOMIC_SEQ_CST);
        xbox_KickWakePoke(&g_k);
        step_done(2 * i + 1);
        while ((__atomic_load_n(&kick, __ATOMIC_ACQUIRE) & KICK_BIT) && !g_stop)
            ;
        step_start(2 * i + 2);
        __atomic_store_n(&put, i + 1, __ATOMIC_SEQ_CST);
        xbox_KickWakePoke(&g_k);
        step_done(2 * i + 2);
    }
    return NULL;
}

static void test_stress(void)
{
    test_thread t;
    long lost = 0, late = 0, idle = 0;
    uint64_t t0;

    reset(&g_k);
    g_stop = 0;
    g_started = g_done = 0;
    t0 = xbox_HostNowNs();
    thread_start(&t, guest, NULL);
    while (__atomic_load_n(&done_put, __ATOMIC_RELAXED) != ROUNDS) {
        uint32_t p = __atomic_load_n(&put, __ATOMIC_ACQUIRE);
        if (p != done_put) {
            __atomic_store_n(&done_put, p, __ATOMIC_RELEASE);   /* "walked" */
            continue;
        }
        if (__atomic_load_n(&kick, __ATOMIC_ACQUIRE) & KICK_BIT) {
            __atomic_fetch_and(&kick, ~KICK_BIT, __ATOMIC_SEQ_CST);
            continue;
        }
        if (!xbox_KickWakeSleep(&g_k, 200 * MS)) {
            uint64_t returned = xbox_HostNowNs();
            if (!xbox_KickWakePending(&g_k))
                idle++;         /* the guest was descheduled between stores */
            else if (guest_finished_before(returned))
                lost++;
            else
                late++;
        }
        if (xbox_HostNowNs() - t0 > 60000 * MS) {
            g_stop = 1;   /* a hang: fail rather than block ctest */
            break;
        }
    }
    thread_join(t);
    printf("  %d kicks in %.0f ms: sleeps %ld poked %ld early %ld; 200 ms timeouts:"
           " %ld idle, %ld guest preempted mid-step, %ld lost\n",
           ROUNDS, (double)(xbox_HostNowNs() - t0) / MS, g_k.sleeps, g_k.poked,
           g_k.early, idle, late, lost);
    check(!g_stop, "stress: every kick was walked (no hang)");
    check(lost == 0, "stress: no timeout found work whose poke had run (no lost wake)");
}

/* ---- stress: the guest's one fenced pass ------------------------------ */

#define PASS_ROUNDS 200000

static int g_fenced;

/* RECOMP_SPIN_WAIT's test of the walker's flag, as the generated code
 * runs it (the other flags are 0 here). 1 when it saw the flag. */
static int guest_pass(void)
{
    if (g_fenced)
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&armed, __ATOMIC_RELAXED)) {
        xbox_KickWakePoke(&g_k);
        return 1;
    }
    return 0;
}

/* Passes finished, with PASS_SAW_FLAG when the last one saw the flag and
 * poked: one preempted inside the poke past the walker's timeout is apart,
 * not lost (the handshake held). Only one plain store after the pass: a
 * second store, a release store or a clock read beside it slows the loop
 * and hides most of the reorder the fenced case is there to catch (about
 * 100 losses a run unfenced on an M-series Mac fall to a few). */
#define PASS_SAW_FLAG 0x80000000u
/* A guest preempted inside its poke for longer than the walker's tick: the
 * Deck sees 0 to 18 in 200k kicks, a Mac at 16 copies a core none. A
 * Poke that skipped its wake would time out on most of the sleeps it
 * should end, thousands a run. */
#define IN_POKE_MAX 200
static volatile uint32_t g_passes;

static void *guest_one_pass(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < PASS_ROUNDS && !g_stop; i++) {
        __atomic_store_n(&kick, KICK_BIT, __ATOMIC_RELAXED);   /* MEM32 */
        uint32_t saw = guest_pass() ? PASS_SAW_FLAG : 0;
        __atomic_store_n(&g_passes, (i + 1) | saw, __ATOMIC_RELAXED);
        while (__atomic_load_n(&kick, __ATOMIC_ACQUIRE) && !g_stop)
            ;                           /* asleep until the ack: no passes */
    }
    g_stop = 1;
    return NULL;
}

/* Walker sleeps of 1 ms (the tick) that found the bit set after the
 * guest's pass for it had finished without seeing the flag: lost pokes, the
 * handshake failing. `late` gets the ones whose pass had not finished (the
 * guest preempted inside it, its store landing after the sleep). `in_poke`
 * gets the ones whose pass had seen the flag: the guest preempted inside
 * the poke past the timeout, its disarm then losing to the walker's. That
 * is also what a Poke that skips its wake looks like, so it is counted on
 * its own and bounded. `early` gets sleeps that returned well before the
 * tick: Wine's condition variable does that now and then, and the guest's
 * whole pass can then run after the walker's disarm, which proves nothing
 * (the ack loop polls and sleeps again), but a waker that always returned
 * early would pass the case without testing it, so it is bounded too. */
typedef struct {
    long lost, late, in_poke, early, sleeps;
} one_pass_counts;

static one_pass_counts one_pass_run(int fenced)
{
    one_pass_counts c = {0};

    test_thread t;
    uint32_t acks = 0;
    uint64_t t0 = xbox_HostNowNs(), per_ms = ticks_per_ms(), start = ticks();

    reset(&g_k);
    g_fenced = fenced;
    g_stop = 0;
    g_passes = 0;
    thread_start(&t, guest_one_pass, NULL);
    /* Nothing between the ack and the sleep's arm: that is the window the
     * guest's store races (a clock read there hides most of it). A sleep's
     * start is the end of the one before: a kick check and an ack away. */
    while (!g_stop) {
        if (__atomic_load_n(&kick, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&kick, 0, __ATOMIC_RELEASE);
            acks++;
            continue;
        }
        int woke = xbox_KickWakeSleep(&g_k, MS);
        c.sleeps++;
        uint32_t passes = __atomic_load_n(&g_passes, __ATOMIC_ACQUIRE);
        uint64_t end = ticks(), slept = end - start;

        start = end;            /* the next sleep's arm is a kick check away */
        if (woke)
            continue;
        if (!__atomic_load_n(&kick, __ATOMIC_ACQUIRE))
            continue;
        /* The bit is kick acks + 1's: was its pass over, flag unseen? */
        if (slept < per_ms / 2)
            c.early++;
        else if (passes == acks + 1)
            c.lost++;
        else if (passes == ((acks + 1) | PASS_SAW_FLAG))
            c.in_poke++;
        else
            c.late++;
        if (xbox_HostNowNs() - t0 > 60000 * MS)
            g_stop = 1;
    }
    thread_join(t);
    return c;
}

static void test_one_pass(void)
{
    one_pass_counts u = one_pass_run(0), f = one_pass_run(1);

    printf("  %d one-pass kicks: %ld lost without the macro's fence, %ld with it"
           " (guest preempted mid-pass: %ld, %ld)\n",
           PASS_ROUNDS, u.lost, f.lost, u.late, f.late);
    printf("  one-pass timeouts after a pass that saw the flag: %ld, %ld;"
           " sleeps back early: %ld of %ld, %ld of %ld\n",
           u.in_poke, f.in_poke, u.early, u.sleeps, f.early, f.sleeps);
    check(f.lost == 0, "one pass, fenced: no poke lost");
    check(f.in_poke <= IN_POKE_MAX,
          "one pass, fenced: few timeouts after a pass that poked");
    check(f.early < f.sleeps / 10 && u.early < u.sleeps / 10,
          "one pass: most sleeps run their tick (the case tests the waker)");
}

/* ---- stress: a vblank between the hold's poll and the arm -------------- */

#define VBLANKS 8192
/* Every PAUSE_EVERY vblanks the timer thread stops for PAUSE_MS, longer
 * than the held sleep's HELD_NS (the ack loop's tick): a nudge lost to the
 * race just before a pause is then not covered by the next vblank, and the
 * sleep times out with the count already past what it was armed for. */
#define PAUSE_EVERY 8
#define PAUSE_MS 3
#define HELD_NS MS

static volatile uint32_t g_nudged;          /* vblanks whose nudge returned */
static uint64_t g_nudged_ns[VBLANKS];       /* when each did */

static void *vblank_thread(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < VBLANKS && !g_stop; i++) {
        uint64_t t0 = xbox_HostNowNs();
        while (xbox_HostNowNs() - t0 < 2 * US)
            ;
        __atomic_fetch_add(&vblanks, 1, __ATOMIC_SEQ_CST);   /* counts... */
        xbox_KickWakeNudge(&g_k);                             /* ...then nudges */
        g_nudged_ns[i] = xbox_HostNowNs();
        __atomic_store_n(&g_nudged, i + 1, __ATOMIC_RELEASE);
        if ((i + 1) % PAUSE_EVERY == 0)
            sleep_ms(PAUSE_MS);
    }
    return NULL;
}

static void test_held_stress(void)
{
    test_thread t;
    long timeouts = 0, lost = 0;
    uint64_t t0;

    reset(&g_k);
    kick = KICK_BIT;                    /* a held flip: the bit stays set */
    g_stop = 0;
    g_nudged = 0;
    t0 = xbox_HostNowNs();
    thread_start(&t, vblank_thread, NULL);
    for (;;) {
        uint32_t seen = vblank_count();
        uint64_t t_arm;
        if (seen == VBLANKS)
            break;
        t_arm = xbox_HostNowNs();
        if (!xbox_KickWakeSleepNudge(&g_k, HELD_NS, seen)) {
            /* A timeout during a pause is the normal case. One whose next
             * vblank (past `seen`) had nudged early in the sleep is a nudge
             * nobody received: that vblank counted before the recheck, or
             * nudged while the walker was armed. Late in the sleep a vblank
             * can count before the timeout's disarm and nudge after it,
             * which is no loss: the next sleep's recheck sees the count. */
            uint32_t n = __atomic_load_n(&g_nudged, __ATOMIC_ACQUIRE);
            timeouts++;
            if (n > seen && g_nudged_ns[seen] < t_arm + HELD_NS / 2)
                lost++;
        }
        if (xbox_HostNowNs() - t0 > 60000 * MS) {
            g_stop = 1;
            break;
        }
    }
    thread_join(t);
    printf("  %d vblanks in %.0f ms: held sleeps %ld nudges %ld timeouts %ld lost %ld\n",
           VBLANKS, (double)(xbox_HostNowNs() - t0) / MS, g_k.held_sleeps,
           g_k.nudges, timeouts, lost);
    check(!g_stop, "held stress: every vblank was seen (no hang)");
    check(lost == 0, "held stress: no held sleep outlasted a vblank's nudge");
}

/* ---- single cases ------------------------------------------------------ */

static void *after_us(void *arg)
{
    /* arg: what to do 2 ms in. */
    uint64_t t0 = xbox_HostNowNs();
    while (xbox_HostNowNs() - t0 < 2 * MS)
        ;
    ((void (*)(void))arg)();
    return NULL;
}

static void do_nudge(void) { xbox_KickWakeNudge(&g_k); }
static void do_poke_idle(void) { xbox_KickWakePoke(&g_k); }
static void do_kick_poke(void)
{
    __atomic_fetch_or(&kick, KICK_BIT, __ATOMIC_SEQ_CST);
    xbox_KickWakePoke(&g_k);
}
static void do_put(void)
{
    /* The PUT store the watch waits for, 2 ms after the "ack". */
    __atomic_store_n(&put, 0x80, __ATOMIC_SEQ_CST);
}

static int sleep_nudge_seen0(XboxKickWake *k, uint64_t ns)
{
    return xbox_KickWakeSleepNudge(k, ns, 0);
}

static uint64_t timed_sleep(int (*fn)(XboxKickWake *, uint64_t), uint64_t ns,
                            void (*other)(void), int *ret)
{
    test_thread t;
    uint64_t t0 = xbox_HostNowNs(), dt;
    if (other)
        thread_start(&t, after_us, (void *)other);
    *ret = fn(&g_k, ns);
    dt = xbox_HostNowNs() - t0;
    if (other)
        thread_join(t);
    return dt;
}

static void test_cases(void)
{
    int r;
    uint64_t dt;

    reset(&g_k);
    dt = timed_sleep(xbox_KickWakeSleep, 5 * MS, NULL, &r);
    check(r == 0 && dt >= 4 * MS && dt < 500 * MS,
          "fallback: a sleep nobody ends times out after its 5 ms");
    check(armed == 0, "fallback: the timeout takes the armed flag back");

    reset(&g_k);
    dt = timed_sleep(xbox_KickWakeSleep, 20 * MS, do_poke_idle, &r);
    check(r == 0 && dt >= 15 * MS, "a poke with nothing pending does not wake");

    reset(&g_k);
    dt = timed_sleep(xbox_KickWakeSleep, 500 * MS, do_kick_poke, &r);
    check(r == 1 && dt < 200 * MS, "a kick's poke wakes the walker");
    dt = timed_sleep(xbox_KickWakeSleep, 0, NULL, &r);
    check(r == 1 && g_k.early == 1, "pending work: the sleep returns at once");
    kick = 0;
    dt = timed_sleep(xbox_KickWakeSleep, 10 * MS, NULL, &r);
    check(r == 0 && dt >= 8 * MS, "no wake is left latched for the next sleep");

    reset(&g_k);
    dt = timed_sleep(xbox_KickWakeSleep, 500 * MS, do_nudge, &r);
    check(r == 1 && dt < 200 * MS, "a nudge (vblank) wakes the walker");

    reset(&g_k);
    kick = KICK_BIT;                        /* held flip: the bit stays set */
    dt = timed_sleep(sleep_nudge_seen0, 20 * MS, do_kick_poke, &r);
    check(r == 0 && dt >= 15 * MS, "held flip: the KickOff's poke does not wake");
    dt = timed_sleep(sleep_nudge_seen0, 500 * MS, do_nudge, &r);
    check(r == 1 && dt < 200 * MS, "held flip: the vblank's nudge wakes");
    vblanks = 1;                            /* counted after the hold's poll */
    dt = timed_sleep(sleep_nudge_seen0, 500 * MS, NULL, &r);
    check(r == 1 && dt < 50 * MS && armed == 0,
          "held flip: a vblank before the arm ends the sleep at once");
    vblanks = 0;
    dt = timed_sleep(sleep_nudge_seen0, 10 * MS, NULL, &r);
    check(r == 0 && dt >= 8 * MS, "held flip: no wake is left latched by the recheck");

    reset(&g_k);
    {
        test_thread t;
        thread_start(&t, after_us, (void *)do_put);
        r = xbox_KickWakeWatchPut(&g_k, 0, 200 * MS);
        thread_join(t);
        check(r == 1 && g_k.watch_hits == 1, "put watch: sees the store after the ack");
    }
    check(!xbox_KickWakePolling(&g_k, 0x80), "put watch: a hit leaves the loop free to sleep");
    r = xbox_KickWakeWatchPut(&g_k, 0x80, 100 * US);
    check(r == 0 && g_k.watch_misses == 1, "put watch: gives up after its 100 us");

    /* The carry: a watch that ran out keeps the loop polling, tick after
     * tick, until PUT moves; then it may sleep again. */
    check(xbox_KickWakePolling(&g_k, 0x80) && xbox_KickWakePolling(&g_k, 0x80),
          "put watch carry: a miss polls on while PUT stays");
    check(!xbox_KickWakePolling(&g_k, 0x100) && !xbox_KickWakePolling(&g_k, 0x80),
          "put watch carry: PUT moving ends it");
    xbox_KickWakeWatchPut(&g_k, 0x80, 100 * US);
    put = 0x100;
    xbox_KickWakeWatchPut(&g_k, 0x80, 100 * US);
    check(!xbox_KickWakePolling(&g_k, 0x80), "put watch carry: a later watch's hit ends it");
    put = 0x80;
    xbox_KickWakeWatchPut(&g_k, 0x80, 100 * US);
    g_k.watch_missed_ns -= XBOX_KICK_WAKE_CARRY_NS;
    check(!xbox_KickWakePolling(&g_k, 0x80), "put watch carry: ends after its bound");

    reset(&g_k);
    check(!xbox_KickWakeMiss(&g_k) && !xbox_KickWakeMiss(&g_k), "miss limit: two misses keep the wait");
    xbox_KickWakeHit(&g_k);
    check(!xbox_KickWakeMiss(&g_k) && !xbox_KickWakeMiss(&g_k) && !g_k.off,
          "miss limit: a poked kick resets the run");
    check(xbox_KickWakeMiss(&g_k) && g_k.off, "miss limit: the third in a row turns it off");
    check(!xbox_KickWakeMiss(&g_k), "miss limit: reported once");
}

int main(void)
{
    static const struct { XboxHostWakerForm form; const char *name; } forms[] = {
#ifdef _WIN32
        { XBOX_WAKER_CONDVAR, "condition variable (Wine's form)" },
        { XBOX_WAKER_EVENT, "auto-reset event + high-resolution timer (native form)" },
#else
        { XBOX_WAKER_CONDVAR, "condition variable" },
#endif
    };
    printf("kick_wake_test\n");
#ifdef _WIN32
    printf("host: %s\n", xbox_HostUnderWine() ? "Wine" : "native Windows");
#endif
    for (size_t i = 0; i < sizeof forms / sizeof forms[0]; i++) {
        g_form = forms[i].form;
        printf("waker: %s\n", forms[i].name);
        if (!form_waker(g_form)) {
            /* Native Windows before 10 1803 (no high-resolution timer):
             * the runtime polls there, so there is nothing to test. */
            printf("  not available on this host: skipped\n");
            continue;
        }
        test_cases();
        test_stress();
        test_one_pass();
        test_held_stress();
    }
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
