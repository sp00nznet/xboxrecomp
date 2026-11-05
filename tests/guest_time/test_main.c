/*
 * The guest's clocks (guest_time.c) and the timed waits that run on them.
 *
 * Real mode: every clock follows the host. Virtual mode: every clock stands
 * still between two vblanks and moves by exactly one 60 Hz period at each,
 * the clocks agree with each other, they never go backwards over a long run,
 * and a timed wait or delay ends in the step that carries the clock past its
 * due time, however long the host takes between steps.
 */
#include "kernel.h"
#include "guest_time.h"
#include "guest_state_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

#define EPOCH_2004 127173888000000000ULL

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void real_mode(void)
{
    LARGE_INTEGER f;
    int64_t a, b;
    uint32_t t0, t1;
    uint64_t i0, i1;

    xbox_GuestTimeTestMode(0);
    QueryPerformanceFrequency(&f);
    check(xbox_GuestPerfFrequency() == f.QuadPart,
          "real: perf frequency is the host's");
    a = xbox_GuestPerfCounter();
    t0 = xbox_GuestTickCount();
    i0 = xbox_GuestInterruptTime();
    Sleep(50);
    b = xbox_GuestPerfCounter();
    t1 = xbox_GuestTickCount();
    i1 = xbox_GuestInterruptTime();
    check(b > a && (b - a) * 1000 / f.QuadPart >= 40,
          "real: perf counter moves with the host (>= 40 ms over a 50 ms sleep)");
    check(t1 - t0 >= 30, "real: tick count moves with the host");
    check(i1 > i0, "real: interrupt time moves with the host");
    check(xbox_GuestTimeVblanks() == 0, "real: no virtual count");
    {
        uint64_t s = xbox_GuestSystemTime();
        check(s > EPOCH_2004 + 10000000ULL * 86400 * 365 * 10,
              "real: system time is the host's (after 2014)");
    }
}

static void virtual_mode(void)
{
    int ok;
    uint64_t n;

    xbox_GuestTimeTestMode(1);
    check(xbox_GuestPerfFrequency() == 10000000,
          "virtual: perf frequency is a fixed 10 MHz");
    check(xbox_GuestInterruptTime() == 0 && xbox_GuestTickCount() == 0
          && xbox_GuestPerfCounter() == 0 && xbox_ReadTimeStampCounter() == 0,
          "virtual: every clock starts at 0");
    check(xbox_GuestSystemTime() == EPOCH_2004
          && xbox_GuestSystemTimeCoarse() == EPOCH_2004,
          "virtual: system time starts at 2004-01-01");
    Sleep(30);
    check(xbox_GuestInterruptTime() == 0 && xbox_GuestPerfCounter() == 0,
          "virtual: frozen between vblanks (30 ms host sleep)");

    xbox_GuestTimeVblank();
    check(xbox_GuestInterruptTime() == 166666 && xbox_GuestTickCount() == 16
          && xbox_GuestPerfCounter() == 166666
          && xbox_ReadTimeStampCounter() == 12222221,
          "virtual: one vblank is one 60 Hz period on every clock");
    for (n = 1; n < 60; n++)
        xbox_GuestTimeVblank();
    check(xbox_GuestInterruptTime() == 10000000 && xbox_GuestTickCount() == 1000
          && xbox_ReadTimeStampCounter() == 733333333ULL
          && xbox_GuestSystemTime() == EPOCH_2004 + 10000000,
          "virtual: 60 vblanks are exactly one second");

    /* Monotonic, one period per step, over a long run (about 4.6 hours). */
    {
        uint64_t prev_it = xbox_GuestInterruptTime();
        int64_t prev_pc = xbox_GuestPerfCounter();
        uint64_t prev_tsc = xbox_ReadTimeStampCounter();
        ok = 1;
        for (n = 0; n < 1000000 && ok; n++) {
            uint64_t it, tsc;
            int64_t pc;
            xbox_GuestTimeVblank();
            it = xbox_GuestInterruptTime();
            pc = xbox_GuestPerfCounter();
            tsc = xbox_ReadTimeStampCounter();
            ok &= it - prev_it == 166666 || it - prev_it == 166667;
            ok &= pc > prev_pc && tsc > prev_tsc;
            prev_it = it; prev_pc = pc; prev_tsc = tsc;
        }
        check(ok, "virtual: 10^6 steps, monotonic, 166666-166667 x 100 ns each");
    }

    /* Due times: relative from the interrupt time, absolute from the
     * system time, both on the interrupt-time clock. */
    xbox_GuestTimeTestMode(1);
    check(xbox_GuestTimeDue(-250000) == 250000
          && xbox_GuestTimeDue((int64_t)(EPOCH_2004 + 250000)) == 250000
          && xbox_GuestTimeDue((int64_t)EPOCH_2004 - 5) == 0,
          "virtual: relative and absolute due times agree");
}

/* A thread that waits (or delays) 25 ms of guest time: 1.5 periods, so it
 * must still be waiting after one step and done after the second. */
static HANDLE s_event;
static volatile LONG s_done;
static volatile LONG s_status;

static DWORD WINAPI waiter(LPVOID delay)
{
    LARGE_INTEGER t;
    NTSTATUS st;

    t.QuadPart = -250000;
    if (delay)
        st = xbox_KeDelayExecutionThread(0, FALSE, &t);
    else
        st = xbox_KeWaitForSingleObject(s_event, 0, 0, FALSE, &t);
    InterlockedExchange(&s_status, (LONG)st);
    InterlockedExchange(&s_done, 1);
    return 0;
}

static int wait_done(int ms)
{
    while (ms-- > 0) {
        if (InterlockedCompareExchange(&s_done, 0, 0))
            return 1;
        Sleep(1);
    }
    return InterlockedCompareExchange(&s_done, 0, 0) != 0;
}

static void timed_wait(int delay, const char *name)
{
    char what[96];
    HANDLE th;

    xbox_GuestTimeTestMode(1);
    InterlockedExchange(&s_done, 0);
    InterlockedExchange(&s_status, -1);
    th = CreateThread(NULL, 0, waiter, delay ? (LPVOID)1 : NULL, 0, NULL);
    snprintf(what, sizeof what, "virtual %s: still waiting after 100 ms host time", name);
    check(!wait_done(100), what);
    xbox_GuestTimeVblank();
    snprintf(what, sizeof what, "virtual %s: still waiting after one step", name);
    check(!wait_done(60), what);
    xbox_GuestTimeVblank();
    snprintf(what, sizeof what, "virtual %s: ends after the step that crosses the due", name);
    check(wait_done(1000) && InterlockedCompareExchange(&s_status, 0, 0)
              == (LONG)(delay ? STATUS_SUCCESS : STATUS_TIMEOUT), what);
    WaitForSingleObject(th, 2000);
    CloseHandle(th);
}

static void signalled_wait(void)
{
    HANDLE th;

    xbox_GuestTimeTestMode(1);
    InterlockedExchange(&s_done, 0);
    th = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
    Sleep(20);
    SetEvent(s_event);
    check(wait_done(1000) && InterlockedCompareExchange(&s_status, 0, 0) == 0,
          "virtual wait: the object still ends it at once, clock frozen");
    WaitForSingleObject(th, 2000);
    CloseHandle(th);
    ResetEvent(s_event);
}

/* No timer armed, nobody stepping by hand: the stepper bridge init
 * registers (here a stand-in for the timer thread, at 60 Hz) starts with
 * the registration in virtual mode, and a timed wait ends on its steps. */
static volatile LONG s_stepper_run, s_stepper_starts;

static DWORD WINAPI stepper(LPVOID unused)
{
    (void)unused;
    while (InterlockedCompareExchange(&s_stepper_run, 0, 0)) {
        Sleep(16);
        xbox_GuestTimeVblank();
    }
    return 0;
}

static HANDLE s_stepper_thread;

static void start_stepper(void)
{
    InterlockedIncrement(&s_stepper_starts);
    InterlockedExchange(&s_stepper_run, 1);
    s_stepper_thread = CreateThread(NULL, 0, stepper, NULL, 0, NULL);
}

static void wait_without_timer(void)
{
    HANDLE th;

    xbox_GuestTimeTestMode(0);
    xbox_GuestTimeSetStepper(start_stepper);
    check(s_stepper_starts == 0, "real: registering the stepper starts nothing");

    xbox_GuestTimeTestMode(1);
    InterlockedExchange(&s_done, 0);
    InterlockedExchange(&s_status, -1);
    xbox_GuestTimeSetStepper(start_stepper);
    check(s_stepper_starts == 1, "virtual: registering the stepper starts it");
    th = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
    check(wait_done(2000) && InterlockedCompareExchange(&s_status, 0, 0)
              == (LONG)STATUS_TIMEOUT,
          "virtual wait with no timer armed ends on the stepper's steps");
    WaitForSingleObject(th, 2000);
    CloseHandle(th);
    InterlockedExchange(&s_stepper_run, 0);
    WaitForSingleObject(s_stepper_thread, 2000);
    CloseHandle(s_stepper_thread);
}

/* The RAM hash: same bytes, same hash; one flipped bit, another. */
static void state_hash(void)
{
    static uint64_t buf[8192];
    uint64_t a, b, c;
    int i;

    for (i = 0; i < 8192; i++)
        buf[i] = (uint64_t)i * 0x9E3779B97F4A7C15ULL;
    a = xbox_GuestStateHashBytes(buf, sizeof buf);
    b = xbox_GuestStateHashBytes(buf, sizeof buf);
    buf[4097] ^= 1u << 7;
    c = xbox_GuestStateHashBytes(buf, sizeof buf);
    check(a == b && a != c, "state hash: repeatable, and one bit changes it");
}

/* The skip list: a byte inside a skipped range leaves the page's hash as it
 * was, the first byte past it does not; bad items are dropped, not read as
 * zero. */
static void state_hash_skip(void)
{
    static uint8_t page[65536];
    uint64_t base, inside, past;

    check(xbox_GuestStateHashSetSkips("31000-31010,zz,5-3,40000,"
                                      "50000-1FFFFFFFF") == 2,
          "skip list: two good ranges kept, three bad ones dropped");
    check(xbox_GuestStateHashSetDumps("5,abc,10,7x") == 2,
          "dump list: 5 and 10 kept, abc and 7x dropped");
    check(xbox_GuestStateHashSetDumps("abc") == 0,
          "dump list: abc is not poll 0");

    memset(page, 0x5A, sizeof page);
    base = xbox_GuestStateHashPage(page, 0x30000);
    page[0x1000] ^= 0xFF;
    page[0x100F] ^= 0xFF;
    inside = xbox_GuestStateHashPage(page, 0x30000);
    page[0x1010] ^= 0xFF;
    past = xbox_GuestStateHashPage(page, 0x30000);
    check(base == inside, "skip: bytes inside the range leave the hash");
    check(inside != past, "skip: the first byte past the range moves it");
    base = xbox_GuestStateHashPage(page, 0x20000);
    page[0x1005] ^= 0xFF;
    check(xbox_GuestStateHashPage(page, 0x20000) != base,
          "skip: the same offset in another page still counts");
    page[0x1005] ^= 0xFF;

    xbox_GuestStateHashSetSkips(NULL);
    check(xbox_GuestStateHashPage(page, 0x30000) != inside,
          "skip: with no list the masked bytes count again");
    xbox_GuestStateHashSetDumps(NULL);
}

int main(void)
{
    printf("guest_time:\n");
    s_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    real_mode();
    virtual_mode();
    timed_wait(0, "wait");
    timed_wait(1, "delay");
    signalled_wait();
    wait_without_timer();
    state_hash();
    state_hash_skip();
    xbox_GuestTimeTestMode(-1);
    printf("%s (%d failure%s)\n", failures ? "FAIL" : "ok", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
