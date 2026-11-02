/*
 * The deterministic guest scheduler (kernel_guest_det.c, RECOMP_DEBUG=
 * deterministic with the guest lock): the guest CPU is a token passed round
 * the guest threads in ring-slot order (creation order until an ended
 * thread's slot is reused), and the clock steps one vblank only
 * when every runnable thread gave the token up without getting anywhere.
 *
 * Each case drives the ring through its public calls, with host threads
 * started in a scrambled order and host sleeps between, so a result that
 * depended on the host's schedule would show:
 *  - off: the calls are no-ops and nobody is in the ring;
 *  - the order: three threads that yield in turn run strictly a, b, c, a,
 *    b, c..., in the order they were announced, not the order they started;
 *  - a vblank wait ends after exactly one step, and the clock moved once;
 *  - a timed wait (due) ends after the vblanks that reach it, and no more;
 *  - a busy thread (yields at new sites each time) is never idle, so the
 *    clock does not step under it;
 *  - a device that did work is a catch-up, not a step: a wait on what the
 *    device writes ends with no vblank;
 *  - a suspended thread is out of the ring until resumed;
 *  - the back-edge quantum: a loop polling at one edge, with no kernel
 *    call, lets the device run and ends;
 *  - a thread's end (PsTerminateSystemThread, ordinal 258) keeps the token
 *    until the thread parts, so what it publishes on the way out (the
 *    thread object's SignalState) is there at the next thread's first turn;
 *  - slots of ended threads are reused: 300 threads made one after another
 *    all get a place in the 256-slot ring;
 *  - a thread whose spawn failed (dropped) is never handed the token;
 *  - a thread leaving while every other one is suspended steps the clock
 *    until one is resumed, and hands it the token;
 *  - a thread made during that step does not get the parting thread's
 *    slot, which still holds the turn, so it does not run during the step;
 *  - when nothing resumes them, the parting thread's steps end after a
 *    minute of guest time and the turn is left empty;
 *  - a timed wait due less than 1 ms ahead is a ring wait that ends on the
 *    next step, not a host poll with the clock frozen.
 * A watchdog fails the test instead of hanging it.
 */
#include "kernel.h"
#include "guest_time.h"
#include "recomp_env.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static DWORD WINAPI watchdog(LPVOID unused)
{
    (void)unused;
    Sleep(60000);
    fprintf(stderr, "guest_det: watchdog: a case hung\n");
    fflush(stderr);
    _Exit(1);
    return 0;
}

static void join(HANDLE t)
{
    WaitForSingleObject(t, 20000);
    CloseHandle(t);
}

/* ── A ring of guest threads ─────────────────────────────────────── */

#define OBJ(i) (0x80010000u + 0x100u * (uint32_t)(i))
#define MAXLOG 256

static char s_log[MAXLOG];
static volatile LONG s_nlog;

static void log_c(char c)
{
    LONG n = InterlockedIncrement(&s_nlog) - 1;
    if (n < MAXLOG - 1)
        s_log[n] = c;
}

typedef struct {
    int id;
    void (*body)(int id);
    int start_delay_ms;
} Guest;

static DWORD WINAPI guest_main(LPVOID p)
{
    Guest *g = (Guest *)p;

    Sleep(g->start_delay_ms);
    xbox_GuestDetBind(OBJ(g->id));
    xbox_GuestDetJoin();
    g->body(g->id);
    xbox_GuestDetPart();
    return 0;
}

/* The test's thread is the title's main: slot 0, joined first. It
 * announces the others (as the creator does at spawn) and lets them run
 * by blocking until done. */
static volatile LONG s_done;

static int all_done(void *ctx)
{
    return InterlockedCompareExchange(&s_done, 0, 0) == *(LONG *)ctx;
}

static void run_ring(int n, void (*body)(int), const int *delays)
{
    Guest g[8];
    HANDLE t[8];
    LONG want = n;
    int i;

    xbox_GuestTimeTestMode(1);
    xbox_GuestDetTestReset(1);
    memset(s_log, 0, sizeof s_log);
    s_nlog = 0;
    s_done = 0;
    xbox_GuestDetBind(OBJ(0));
    xbox_GuestDetJoin();
    for (i = 0; i < n; i++) {
        g[i].id = i + 1;
        g[i].body = body;
        g[i].start_delay_ms = delays ? delays[i] : 0;
        xbox_GuestDetThreadNew(OBJ(i + 1));
    }
    for (i = 0; i < n; i++)
        t[i] = CreateThread(NULL, 0, guest_main, &g[i], 0, NULL);
    xbox_GuestDetBlock(all_done, &want, 0, 0x1000);
    xbox_GuestDetPart();
    for (i = 0; i < n; i++)
        join(t[i]);
}

/* ── Cases ───────────────────────────────────────────────────────── */

static void off(void)
{
    long st, cu, pa;

    xbox_GuestTimeTestMode(0);
    xbox_GuestDetTestReset(0);
    check(!xbox_GuestDetOn(), "off: the mode is off");
    xbox_GuestDetThreadNew(OBJ(1));
    xbox_GuestDetBind(OBJ(1));
    check(!xbox_GuestDetThread(), "off: nobody is in the ring");
    check(xbox_GuestDetSuspend(OBJ(1), 1, NULL) == 0,
          "off: a suspend is left to the host");
    xbox_GuestDetStats(&st, &cu, &pa);
    check(st == 0 && cu == 0 && pa == 0, "off: no steps, catch-ups or passes");
}

/* Busy: a new site every turn, so the yields are progress. */
static void body_order(int id)
{
    int k;

    for (k = 0; k < 4; k++) {
        log_c((char)('a' + id - 1));
        xbox_GuestDetYield(0x2000u + (uint32_t)(id * 16 + k));
    }
    InterlockedIncrement(&s_done);
}

static void order(void)
{
    static const int delays[3] = { 30, 0, 15 };   /* started c, b... */
    long st, cu, pa;

    run_ring(3, body_order, delays);
    s_log[s_nlog < MAXLOG ? s_nlog : MAXLOG - 1] = 0;
    check(strcmp(s_log, "abcabcabcabc") == 0,
          "order: the ring runs in creation order, not start order");
    xbox_GuestDetStats(&st, &cu, &pa);
    check(st == 0, "busy: threads that get somewhere never step the clock");
}

static uint64_t s_vb_before, s_vb_after;

static void body_vblank(int id)
{
    (void)id;
    s_vb_before = xbox_GuestTimeVblanks();
    xbox_GuestDetWaitVblank(s_vb_before, 0x3000);
    s_vb_after = xbox_GuestTimeVblanks();
    InterlockedIncrement(&s_done);
}

static void vblank(void)
{
    long st, cu, pa;

    run_ring(1, body_vblank, NULL);
    xbox_GuestDetStats(&st, &cu, &pa);
    check(s_vb_after == s_vb_before + 1 && st == 1,
          "vblank: a vblank wait ends after exactly one step");
}

static uint64_t s_due_vb;

static void body_due(int id)
{
    uint64_t due;

    (void)id;
    /* 50 ms: three 60 Hz periods (16.67 ms each) reach it. */
    due = xbox_GuestTimeDue(-50 * 10000LL);
    xbox_GuestDetBlock(NULL, NULL, due, 0x4000);
    s_due_vb = xbox_GuestTimeVblanks();
    InterlockedIncrement(&s_done);
}

static void timed(void)
{
    run_ring(1, body_due, NULL);
    check(s_due_vb == 3, "timed: a 50 ms wait ends on the 3rd vblank");
}

/* A device thread as the nv2a walker: works while it has a job. */
static volatile LONG s_dev_job, s_dev_flag, s_dev_quit;

static DWORD WINAPI device_main(LPVOID p)
{
    (void)p;
    for (;;) {
        int worked = 0;
        xbox_GuestDetDeviceWait(XBOX_DET_DEV_NV2A);
        if (InterlockedCompareExchange(&s_dev_quit, 0, 0)) {
            xbox_GuestDetDeviceDone(XBOX_DET_DEV_NV2A, 0);
            return 0;
        }
        if (InterlockedExchange(&s_dev_job, 0)) {
            Sleep(5);                       /* host-timed work */
            InterlockedExchange(&s_dev_flag, 1);
            worked = 1;
        }
        xbox_GuestDetDeviceDone(XBOX_DET_DEV_NV2A, worked);
    }
}

static int flag_set(void *ctx)
{
    (void)ctx;
    return InterlockedCompareExchange(&s_dev_flag, 0, 0) != 0;
}

static void body_device(int id)
{
    (void)id;
    InterlockedExchange(&s_dev_job, 1);
    xbox_GuestDetBlock(flag_set, NULL, 0, 0x5000);
    s_vb_after = xbox_GuestTimeVblanks();
    InterlockedIncrement(&s_done);
}

/* The poll a lifted loop does with no kernel call: back edges only. */
static void body_poll(int id)
{
    (void)id;
    InterlockedExchange(&s_dev_job, 1);
    while (!flag_set(NULL))
        xbox_GuestDetBackEdge(0x6000, 0);
    s_vb_after = xbox_GuestTimeVblanks();
    InterlockedIncrement(&s_done);
}

static void device_case(void (*body)(int), const char *what)
{
    HANDLE d;
    long st, cu, pa;

    s_dev_job = s_dev_flag = s_dev_quit = 0;
    /* run_ring resets the scheduler; the device registers after that, from
     * the ring's own first slot, so start it inside a wrapper. */
    xbox_GuestTimeTestMode(1);
    xbox_GuestDetTestReset(1);
    xbox_GuestDetDeviceAdd(XBOX_DET_DEV_NV2A);
    d = CreateThread(NULL, 0, device_main, NULL, 0, NULL);
    {
        Guest g = { 1, body, 0 };
        HANDLE t;
        LONG want = 1;

        memset(s_log, 0, sizeof s_log);
        s_nlog = 0;
        s_done = 0;
        xbox_GuestDetBind(OBJ(0));
        xbox_GuestDetJoin();
        xbox_GuestDetThreadNew(OBJ(1));
        t = CreateThread(NULL, 0, guest_main, &g, 0, NULL);
        xbox_GuestDetBlock(all_done, &want, 0, 0x1000);
        xbox_GuestDetPart();
        join(t);
    }
    xbox_GuestDetStats(&st, &cu, &pa);
    check(s_dev_flag && cu >= 1 && st == 0 && s_vb_after == 0, what);
    InterlockedExchange(&s_dev_quit, 1);
    /* One more grant to let it see quit: a catch-up round by hand. */
    {
        HANDLE w = d;
        xbox_GuestDetTestReset(1);
        xbox_GuestDetDeviceAdd(XBOX_DET_DEV_NV2A);
        xbox_GuestDetBind(OBJ(0));
        xbox_GuestDetJoin();
        xbox_GuestDetYield(0x7000);     /* alone and idle: a device round */
        xbox_GuestDetPart();
        join(w);
    }
}

/* A loop that waits for a time by reading the clock, with no kernel call
 * and its back edges spread over several sites (an inner loop of varying
 * length), so its quanta never end at one edge twice. */
static void body_clock_poll(int id)
{
    uint64_t due, n = 0;

    (void)id;
    due = xbox_GuestInterruptTime() + 2 * 166667;   /* two vblanks on */
    while (xbox_GuestInterruptTime() < due) {
        uint64_t k;
        for (k = 0; k < (n % 7) + 1; k++)
            xbox_GuestDetBackEdge(0x8000 + (uint32_t)k * 4, 0);
        xbox_GuestDetBackEdge(0x8100, 0);
        n++;
    }
    s_vb_after = xbox_GuestTimeVblanks();
    InterlockedIncrement(&s_done);
}

static void clock_poll(void)
{
    long st, cu, pa;

    s_vb_after = 0;
    run_ring(1, body_clock_poll, NULL);
    xbox_GuestDetStats(&st, &cu, &pa);
    check(s_vb_after >= 2 && st >= 2,
          "clock poll: a loop spinning on the clock sees it move");
}

static volatile LONG s_b_ran;

static void body_suspend(int id)
{
    if (id == 1) {
        /* Suspend b, then spin through turns: b must not run. */
        int k;
        xbox_GuestDetSuspend(OBJ(2), 1, NULL);
        for (k = 0; k < 6; k++) {
            log_c('a');
            xbox_GuestDetYield(0x8000u + (uint32_t)k);
        }
        xbox_GuestDetSuspend(OBJ(2), -1, NULL);
        xbox_GuestDetYield(0x8100);
        log_c('a');
    } else {
        log_c('b');
    }
    InterlockedIncrement(&s_done);
}

static void suspend(void)
{
    static const int delays[2] = { 0, 0 };

    run_ring(2, body_suspend, delays);
    s_log[s_nlog < MAXLOG ? s_nlog : MAXLOG - 1] = 0;
    /* b may run once at its join before a's first turn ends? No: a is
     * slot 1 and holds the token from the main's block until it yields,
     * and by then b is suspended. */
    check(strcmp(s_log, "aaaaaaba") == 0,
          "suspend: a suspended thread is out of the ring until resumed");
}

/* An ending thread, through the guest CPU calls the bridge makes: the
 * kernel call 258 is released, the end published after a host delay (the
 * old order let the next thread run in it), then the thread parts. A
 * watcher polls for the end with yields, as GetExitCodeThread loops do. */
static volatile LONG s_ended, s_end_held;

static DWORD WINAPI end_main(LPVOID p)
{
    (void)p;
    xbox_GuestDetBind(OBJ(1));
    xbox_GuestCpuJoin();
    log_c('a');
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 258);
    InterlockedExchange(&s_end_held, xbox_GuestCpuHeld());
    Sleep(30);
    InterlockedExchange(&s_ended, 1);
    InterlockedIncrement(&s_done);
    xbox_GuestCpuPart();
    return 0;
}

static DWORD WINAPI watch_main(LPVOID p)
{
    uint32_t k = 0;

    (void)p;
    xbox_GuestDetBind(OBJ(2));
    xbox_GuestDetJoin();
    while (!InterlockedCompareExchange(&s_ended, 0, 0) && k < 64) {
        log_c('w');
        xbox_GuestDetYield(0x5000u + k++);
    }
    log_c('E');
    InterlockedIncrement(&s_done);
    xbox_GuestDetPart();
    return 0;
}

static void thread_end(void)
{
    HANDLE t[2];
    LONG want = 2;

    xbox_GuestTimeTestMode(1);
    xbox_GuestDetTestReset(1);
    memset(s_log, 0, sizeof s_log);
    s_nlog = 0;
    s_done = 0;
    s_ended = 0;
    xbox_GuestDetBind(OBJ(0));
    xbox_GuestDetJoin();
    xbox_GuestDetThreadNew(OBJ(1));
    xbox_GuestDetThreadNew(OBJ(2));
    t[1] = CreateThread(NULL, 0, watch_main, NULL, 0, NULL);
    t[0] = CreateThread(NULL, 0, end_main, NULL, 0, NULL);
    xbox_GuestDetBlock(all_done, &want, 0, 0x1000);
    xbox_GuestDetPart();
    join(t[0]);
    join(t[1]);
    s_log[s_nlog < MAXLOG ? s_nlog : MAXLOG - 1] = 0;
    check(s_end_held == 1, "end: the ending thread keeps the token in 258");
    check(strcmp(s_log, "aE") == 0,
          "end: the next thread's first turn sees the end published");
}

/* Slot reuse: threads made one after another, each ending before the next
 * starts, as a title's loader threads are. */
static volatile LONG s_reuse_parted, s_reuse_bad;

static DWORD WINAPI reuse_main(LPVOID p)
{
    xbox_GuestDetBind((uint32_t)(uintptr_t)p);
    xbox_GuestDetJoin();
    if (!xbox_GuestDetThread())
        InterlockedIncrement(&s_reuse_bad);     /* no slot: outside the ring */
    xbox_GuestDetPart();
    InterlockedIncrement(&s_reuse_parted);
    return 0;
}

static int reuse_parted(void *ctx)
{
    return InterlockedCompareExchange(&s_reuse_parted, 0, 0) >= *(LONG *)ctx;
}

static void slot_reuse(void)
{
    LONG k;

    xbox_GuestTimeTestMode(1);
    xbox_GuestDetTestReset(1);
    s_reuse_parted = s_reuse_bad = 0;
    xbox_GuestDetBind(OBJ(0));
    xbox_GuestDetJoin();
    for (k = 0; k < 300; k++) {
        uint32_t obj = 0x80200000u + 0x100u * (uint32_t)k;
        LONG want = k + 1;
        HANDLE t;

        xbox_GuestDetThreadNew(obj);
        t = CreateThread(NULL, 0, reuse_main, (LPVOID)(uintptr_t)obj, 0, NULL);
        xbox_GuestDetBlock(reuse_parted, &want, 0, 0x6000);
        join(t);
    }
    xbox_GuestDetPart();
    check(s_reuse_parted == 300 && s_reuse_bad == 0,
          "reuse: 300 threads in turn each get a slot of the ring");
}

/* A dropped thread (its host thread never started) is not runnable: a
 * vblank wait steps and ends instead of passing the token to it. */
static void drop(void)
{
    uint64_t vb0;

    xbox_GuestTimeTestMode(1);
    xbox_GuestDetTestReset(1);
    xbox_GuestDetBind(OBJ(0));
    xbox_GuestDetJoin();
    xbox_GuestDetThreadNew(OBJ(1));
    xbox_GuestDetThreadDrop(OBJ(1));
    vb0 = xbox_GuestTimeVblanks();
    xbox_GuestDetWaitVblank(vb0, 0x7000);
    check(xbox_GuestTimeVblanks() == vb0 + 1,
          "drop: a failed spawn's slot never takes the token");
    xbox_GuestDetPart();
}

/* The last runnable thread leaves while another is suspended: the parting
 * thread steps the clock (what resumes a thread runs on a step), and the
 * resumed thread gets the token. */
static volatile LONG s_res_ran;
static volatile uint64_t s_res_vb, s_res_vb0;

static DWORD WINAPI resumed_main(LPVOID p)
{
    (void)p;
    xbox_GuestDetBind(OBJ(1));
    xbox_GuestDetJoin();
    s_res_vb = xbox_GuestTimeVblanks();
    InterlockedExchange(&s_res_ran, 1);
    xbox_GuestDetPart();
    return 0;
}

static DWORD WINAPI resumer_main(LPVOID p)
{
    int k;

    (void)p;
    /* A timer DPC in the console's place: resumes after three steps. */
    for (k = 0; k < 10000 && xbox_GuestTimeVblanks() < s_res_vb0 + 3; k++)
        Sleep(1);
    xbox_GuestDetSuspend(OBJ(1), -1, NULL);
    return 0;
}

static void part_steps(void)
{
    HANDLE t[2];

    xbox_GuestTimeTestMode(1);
    xbox_GuestDetTestReset(1);
    s_res_ran = 0;
    s_res_vb = 0;
    xbox_GuestDetBind(OBJ(0));
    xbox_GuestDetJoin();
    s_res_vb0 = xbox_GuestTimeVblanks();
    xbox_GuestDetThreadNew(OBJ(1));
    xbox_GuestDetSuspend(OBJ(1), 1, NULL);
    t[0] = CreateThread(NULL, 0, resumed_main, NULL, 0, NULL);
    t[1] = CreateThread(NULL, 0, resumer_main, NULL, 0, NULL);
    xbox_GuestDetPart();
    join(t[0]);
    join(t[1]);
    check(s_res_ran && s_res_vb >= s_res_vb0 + 3,
          "part: the clock steps under suspended threads until a resume");
}

/* A thread made while the last runnable one parts: the parting thread's
 * slot is dead but still holds the turn while it steps with the lock
 * dropped. A device pass of that step (the GPU, in a DPC's place) makes a
 * thread; it must not get the parting slot, or it would find the turn its
 * own at its join and run during the step. */
static volatile LONG s_pr_quit, s_pr_made, s_pr_ran, s_pr_early;
static HANDLE s_pr_t2;

static DWORD WINAPI pr_new_main(LPVOID p)
{
    (void)p;
    xbox_GuestDetBind(OBJ(2));
    xbox_GuestDetJoin();
    InterlockedExchange(&s_pr_ran, 1);
    xbox_GuestDetSuspend(OBJ(1), -1, NULL);
    xbox_GuestDetPart();
    return 0;
}

static DWORD WINAPI pr_device_main(LPVOID p)
{
    (void)p;
    for (;;) {
        xbox_GuestDetDeviceWait(XBOX_DET_DEV_NV2A);
        if (InterlockedCompareExchange(&s_pr_quit, 0, 0)) {
            xbox_GuestDetDeviceDone(XBOX_DET_DEV_NV2A, 0);
            return 0;
        }
        if (!InterlockedExchange(&s_pr_made, 1)) {
            xbox_GuestDetThreadNew(OBJ(2));
            s_pr_t2 = CreateThread(NULL, 0, pr_new_main, NULL, 0, NULL);
            Sleep(100);                     /* the step still under way */
            InterlockedExchange(&s_pr_early,
                                InterlockedCompareExchange(&s_pr_ran, 0, 0));
        }
        xbox_GuestDetDeviceDone(XBOX_DET_DEV_NV2A, 0);
    }
}

static void part_reuse(void)
{
    HANDLE d, t1;

    xbox_GuestTimeTestMode(1);
    xbox_GuestDetTestReset(1);
    s_pr_quit = s_pr_made = s_pr_ran = s_pr_early = 0;
    s_res_ran = 0;
    s_pr_t2 = NULL;
    xbox_GuestDetDeviceAdd(XBOX_DET_DEV_NV2A);
    d = CreateThread(NULL, 0, pr_device_main, NULL, 0, NULL);
    xbox_GuestDetBind(OBJ(0));
    xbox_GuestDetJoin();
    xbox_GuestDetThreadNew(OBJ(1));
    xbox_GuestDetSuspend(OBJ(1), 1, NULL);
    t1 = CreateThread(NULL, 0, resumed_main, NULL, 0, NULL);
    xbox_GuestDetPart();
    join(t1);
    if (s_pr_t2)
        join(s_pr_t2);
    InterlockedExchange(&s_pr_quit, 1);
    xbox_GuestDetDeviceRemove(XBOX_DET_DEV_NV2A);
    join(d);
    check(s_pr_made && !s_pr_early && s_pr_ran && s_res_ran,
          "part: a thread made during the parting step waits its turn");
}

/* The last runnable thread leaves and the suspended one is never resumed:
 * the parting thread stops stepping after a minute of guest time (3600
 * vblanks) and leaves the turn empty, instead of stepping for good (the
 * main thread's part at shutdown would hang there). */
static void part_bound(void)
{
    uint64_t vb0;
    long st, cu, pa;

    xbox_GuestTimeTestMode(1);
    xbox_GuestDetTestReset(1);
    xbox_GuestDetBind(OBJ(0));
    xbox_GuestDetJoin();
    xbox_GuestDetThreadNew(OBJ(1));
    xbox_GuestDetSuspend(OBJ(1), 1, NULL);
    vb0 = xbox_GuestTimeVblanks();
    xbox_GuestDetPart();
    xbox_GuestDetStats(&st, &cu, &pa);
    check(xbox_GuestTimeVblanks() == vb0 + 3600 && st == 3600,
          "part: no resume to come, the parting steps end after a minute");
}

/* NtWaitForSingleObject with an absolute timeout 0.5 ms ahead: ms rounds
 * to 0 (a relative one rounds up to 1 ms), the due time does not. */
static NTSTATUS s_short_st;
static uint64_t s_short_vb0, s_short_vb;

static void body_short_due(int id)
{
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    LARGE_INTEGER to;

    (void)id;
    to.QuadPart = (LONGLONG)xbox_GuestSystemTimeCoarse() + 5000;
    s_short_vb0 = xbox_GuestTimeVblanks();
    s_short_st = xbox_NtWaitForSingleObject(ev, FALSE, &to);
    s_short_vb = xbox_GuestTimeVblanks();
    CloseHandle(ev);
    InterlockedIncrement(&s_done);
}

static void short_due(void)
{
    run_ring(1, body_short_due, NULL);
    check(s_short_st == STATUS_TIMEOUT && s_short_vb == s_short_vb0 + 1,
          "due: a wait due under 1 ms ahead ends on the next step");
}

int main(void)
{
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    printf("guest_det:\n");
    off();
    order();
    vblank();
    timed();
    device_case(body_device,
                "device: a wait on what the GPU writes ends on a catch-up");
    device_case(body_poll,
                "quantum: a back-edge poll lets the device run, no step");
    suspend();
    clock_poll();
    thread_end();
    slot_reuse();
    drop();
    part_steps();
    part_reuse();
    part_bound();
    short_due();
    xbox_GuestTimeTestMode(-1);
    printf("guest_det: %s (%d failures)\n", failures ? "FAIL" : "PASS",
           failures);
    return failures ? 1 : 0;
}
