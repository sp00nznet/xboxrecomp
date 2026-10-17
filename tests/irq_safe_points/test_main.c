/*
 * Device interrupts on the one-CPU gate holder (kernel_hal.c, xbox_IrqPost
 * and xbox_IrqSafePoint).
 *
 * A device model whose interrupt finds the gate held posts it, and the
 * holder runs it on its own thread at its next safe point. Each case orders
 * its threads with events, so it is deterministic rather than a race run
 * many times:
 *  - the race: a holder parked half-way through a list edit; the interrupt
 *    posted meanwhile runs only at the holder's next safe point, on the
 *    holder, with the edit finished. With irq_safe_points=0 it runs at once,
 *    beside the holder, and sees the edit half-done (the bug, recorded);
 *  - the gate's release is a safe point, taken before the gate is free;
 *  - a thread that does not hold the gate never runs a posted routine;
 *  - the holder's registers and IRQL come back; the routine ran at 16;
 *  - a routine's own safe point does not nest another; that one runs after;
 *  - three posts of one line run once;
 *  - a holder that releases between the poster's first try and its post:
 *    the poster's second try delivers;
 *  - a post that lands inside the lower that gives the gate back, after its
 *    safe point and before the leave: the release point runs it on the
 *    holder once the new level is written, not at the poster's next try;
 *  - a holder that reaches no safe point: logged at irq_safe_ms, never run;
 *    with irq_safe_force=1 delivered beside it after irq_safe_ms;
 *  - 100k rounds of edits and random posts leave the list intact.
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

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_ebp;
extern RECOMP_TLS RecompXmm g_xmm0;
extern uint32_t g_xbox_stack_size;
extern ptrdiff_t g_xbox_mem_offset;

#define MS 1000000ull

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* ── The guest's list, and the routine that edits it ─────────────── */

typedef struct Node { struct Node *next, *prev; } Node;
static Node s_head = { &s_head, &s_head };
static Node s_pool[64];
static volatile LONG s_canary;          /* set while an edit is half-done */

static void list_insert(Node *n)
{
    n->next = s_head.next;
    n->prev = &s_head;
    s_head.next->prev = n;
    s_head.next = n;
}

static void list_remove(Node *n)
{
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->next = n->prev = n;
}

static int list_count_ok(void)
{
    int f = 0, b = 0;
    Node *n;

    for (n = s_head.next; n != &s_head && f < 1000; n = n->next) f++;
    for (n = s_head.prev; n != &s_head && b < 1000; n = n->prev) b++;
    return f == b && f < 1000 ? f : -1;
}

/* What a run of the routine saw. */
static volatile LONG s_runs, s_saw_canary, s_irql_bad, s_ohci_runs;
static volatile LONG s_ohci_runs_inside;
static unsigned long s_run_tid;
static int s_mode;                      /* what the routine does besides */
static HANDLE ev_a, ev_b;               /* the routine's own hand-offs */

enum { R_PLAIN, R_GATE_PROBE, R_CLOBBER, R_NEST, R_STRESS };

static void apu_routine(void)
{
    InterlockedIncrement(&s_runs);
    s_run_tid = (unsigned long)GetCurrentThreadId();
    if (s_canary)
        InterlockedIncrement(&s_saw_canary);
    switch (s_mode) {
    case R_GATE_PROBE:
        SetEvent(ev_a);                     /* "probe the gate now" */
        WaitForSingleObject(ev_b, 5000);
        break;
    case R_CLOBBER:
        if (xbox_IrqlCurrent() != 16)
            InterlockedIncrement(&s_irql_bad);
        g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0xBADBADu;
        g_ebp = 0xBADBADu;
        g_esp -= 64;
        memset(&g_xmm0, 0xEE, sizeof g_xmm0);
        break;
    case R_NEST:
        SetEvent(ev_a);                     /* "post the second line now" */
        WaitForSingleObject(ev_b, 5000);
        xbox_IrqSafePoint();                /* must not run it here */
        s_ohci_runs_inside = s_ohci_runs;
        break;
    case R_STRESS: {
        /* An ISR's own edit: one node out, one in, whole. */
        static int k;
        Node *n = &s_pool[32 + (k++ & 31)];
        if (n->next && n->next != n)
            list_remove(n);
        else
            list_insert(n);
        break;
    }
    default:
        break;
    }
}

static void ohci_routine(void)
{
    InterlockedIncrement(&s_ohci_runs);
}

static void reset_counts(void)
{
    XboxIrqStats st;
    s_runs = s_saw_canary = s_irql_bad = s_ohci_runs = 0;
    s_ohci_runs_inside = -1;
    s_run_tid = 0;
    s_mode = R_PLAIN;
    xbox_IrqStatsGet(&st, 1);
}

static void set_env(recomp_env_id id, const char *v)
{
    recomp_env_set(id, v);
    xbox_IrqReloadConfig();
}

/* ── A holder thread, driven by events ───────────────────────────── */

typedef struct {
    HANDLE go_mid, resume, done;  /* holder -> main, main -> holder, holder -> main */
    int half_edit;                /* park half-way through an edit */
    int release_only;             /* park, then lower without a safe point call */
    unsigned long tid;
    int after_point_runs;         /* s_runs right after its safe point call */
    int runs_at_resume;
} Holder;

static DWORD WINAPI holder_main(LPVOID p)
{
    Holder *h = (Holder *)p;
    int saved;
    Node *n = &s_pool[0];

    g_fs_base = 0;                      /* no guest memory: keep IRQL off the KPCR */
    h->tid = (unsigned long)GetCurrentThreadId();
    saved = xbox_IrqlEnterInterrupt(DISPATCH_LEVEL);   /* takes the gate */
    if (h->half_edit) {
        /* Step one of two: linked from the node, not yet from the list. */
        n->next = s_head.next;
        n->prev = &s_head;
        InterlockedExchange(&s_canary, 1);
    }
    SetEvent(h->go_mid);
    WaitForSingleObject(h->resume, 10000);
    h->runs_at_resume = (int)s_runs;
    if (h->half_edit) {
        s_head.next->prev = n;
        s_head.next = n;
        InterlockedExchange(&s_canary, 0);
    }
    if (!h->release_only) {
        xbox_IrqSafePoint();            /* its next kernel call, say */
        h->after_point_runs = (int)s_runs;
    }
    xbox_IrqlLeaveInterrupt(saved);     /* the gate's release */
    if (h->half_edit)
        list_remove(n);
    SetEvent(h->done);
    return 0;
}

static HANDLE holder_start(Holder *h)
{
    HANDLE t;

    h->go_mid = CreateEventW(NULL, FALSE, FALSE, NULL);
    h->resume = CreateEventW(NULL, FALSE, FALSE, NULL);
    h->done = CreateEventW(NULL, FALSE, FALSE, NULL);
    t = CreateThread(NULL, 0, holder_main, h, 0, NULL);
    WaitForSingleObject(h->go_mid, 10000);
    return t;
}

static void holder_finish(Holder *h, HANDLE t)
{
    SetEvent(h->resume);
    WaitForSingleObject(h->done, 10000);
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
    CloseHandle(h->go_mid);
    CloseHandle(h->resume);
    CloseHandle(h->done);
}

/* What a device model does with the answer. */
static int deliver(int line)
{
    int how = xbox_IrqPost(line);

    if (how == XBOX_IRQ_GATED || how == XBOX_IRQ_UNGATED) {
        int irql = xbox_IrqlEnterInterrupt(16);
        (line == XBOX_IRQ_APU ? apu_routine : ohci_routine)();
        xbox_IrqlLeaveInterrupt(irql);
        if (how == XBOX_IRQ_GATED)
            xbox_DispatchGateLeave();
    }
    return how;
}

/* ── The cases ────────────────────────────────────────────────────── */

static void race_shown(void)
{
    Holder h = {0};
    HANDLE t;
    int how;

    /* Today's model: the interrupt runs beside the holder. */
    reset_counts();
    set_env(RENV_IRQ_SAFE_POINTS, "0");
    h.half_edit = 1;
    t = holder_start(&h);
    how = deliver(XBOX_IRQ_APU);
    check(how == XBOX_IRQ_UNGATED && s_runs == 1 && s_saw_canary == 1,
          "irq_safe_points=0: routine runs mid-edit (the bug, recorded)");
    holder_finish(&h, t);
    set_env(RENV_IRQ_SAFE_POINTS, NULL);

    /* Posted: the routine waits for the holder's safe point. */
    reset_counts();
    memset(&h, 0, sizeof h);
    h.half_edit = 1;
    t = holder_start(&h);
    how = deliver(XBOX_IRQ_APU);
    check(how == XBOX_IRQ_POSTED && s_runs == 0, "posted: nothing runs beside the holder");
    holder_finish(&h, t);
    check(h.runs_at_resume == 0 && h.after_point_runs == 1,
          "posted: runs inside the holder's next safe point");
    check(s_run_tid == h.tid && s_saw_canary == 0,
          "posted: on the holder's thread, with the edit finished");
    check(list_count_ok() == 0, "the list is whole again");
}

static DWORD WINAPI gate_prober(LPVOID p)
{
    volatile LONG *busy = (volatile LONG *)p;

    WaitForSingleObject(ev_a, 10000);
    if (xbox_DispatchGateTryEnter()) {
        xbox_DispatchGateLeave();
        *busy = 0;
    } else {
        *busy = 1;
    }
    SetEvent(ev_b);
    return 0;
}

static void gate_release(void)
{
    Holder h = {0};
    HANDLE t, probe;
    volatile LONG busy = -1;

    reset_counts();
    s_mode = R_GATE_PROBE;
    h.release_only = 1;
    t = holder_start(&h);
    check(deliver(XBOX_IRQ_APU) == XBOX_IRQ_POSTED, "release: posted");
    probe = CreateThread(NULL, 0, gate_prober, (LPVOID)&busy, 0, NULL);
    holder_finish(&h, t);
    WaitForSingleObject(probe, 10000);
    CloseHandle(probe);
    check(s_runs == 1 && s_run_tid == h.tid, "release: the lower runs it, on the holder");
    check(busy == 1, "release: the gate is still held while it runs");
}

static void non_holder(void)
{
    Holder h = {0};
    HANDLE t;

    reset_counts();
    t = holder_start(&h);
    check(deliver(XBOX_IRQ_APU) == XBOX_IRQ_POSTED, "non-holder: posted");
    xbox_IrqSafePoint();                /* this thread is at PASSIVE */
    check(s_runs == 0, "non-holder: a safe point here runs nothing");
    holder_finish(&h, t);
    check(s_runs == 1 && s_run_tid == h.tid, "non-holder: the holder runs it");
}

static DWORD WINAPI clobber_holder(LPVOID p)
{
    int *ok = (int *)p, saved;
    RecompXmm x;

    g_fs_base = 0;
    saved = xbox_IrqlEnterInterrupt(DISPATCH_LEVEL);
    g_eax = 1; g_ecx = 2; g_edx = 3; g_ebx = 4; g_esi = 5; g_edi = 6;
    g_ebp = 7; g_esp = 0x123450;
    memset(&g_xmm0, 0x5A, sizeof g_xmm0);
    x = g_xmm0;
    SetEvent(ev_a);
    WaitForSingleObject(ev_b, 10000);
    xbox_IrqSafePoint();
    ok[0] = s_runs == 1;
    ok[1] = g_eax == 1 && g_ecx == 2 && g_edx == 3 && g_ebx == 4
            && g_esi == 5 && g_edi == 6 && g_ebp == 7 && g_esp == 0x123450
            && !memcmp(&g_xmm0, &x, sizeof x);
    ok[2] = xbox_IrqlCurrent() == DISPATCH_LEVEL;
    xbox_IrqlLeaveInterrupt(saved);
    return 0;
}

static void registers(void)
{
    int ok[3] = {0, 0, 0};
    HANDLE t;

    reset_counts();
    s_mode = R_CLOBBER;
    t = CreateThread(NULL, 0, clobber_holder, ok, 0, NULL);
    WaitForSingleObject(ev_a, 10000);
    check(deliver(XBOX_IRQ_APU) == XBOX_IRQ_POSTED, "registers: posted");
    SetEvent(ev_b);
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
    check(ok[0] && s_irql_bad == 0, "registers: the routine ran at IRQL 16");
    check(ok[1], "registers: the holder's registers come back");
    check(ok[2], "registers: the holder is back at DISPATCH_LEVEL");
}

static DWORD WINAPI nest_poster(LPVOID p)
{
    int *how = (int *)p;

    WaitForSingleObject(ev_a, 10000);
    *how = deliver(XBOX_IRQ_OHCI);
    SetEvent(ev_b);
    return 0;
}

static void nesting(void)
{
    Holder h = {0};
    HANDLE t, poster;
    int how2 = -1;

    reset_counts();
    s_mode = R_NEST;
    t = holder_start(&h);
    check(deliver(XBOX_IRQ_APU) == XBOX_IRQ_POSTED, "nesting: first line posted");
    poster = CreateThread(NULL, 0, nest_poster, &how2, 0, NULL);
    holder_finish(&h, t);
    WaitForSingleObject(poster, 10000);
    CloseHandle(poster);
    check(how2 == XBOX_IRQ_POSTED, "nesting: second line posted during the routine");
    check(s_ohci_runs_inside == 0, "nesting: not run inside the routine");
    check(s_ohci_runs == 1 && h.after_point_runs == 1,
          "nesting: run after it, in the same safe point");
}

static void coalescing(void)
{
    Holder h = {0};
    HANDLE t;
    XboxIrqStats st;
    int i, posted = 1;

    reset_counts();
    t = holder_start(&h);
    for (i = 0; i < 3; i++)
        posted &= deliver(XBOX_IRQ_APU) == XBOX_IRQ_POSTED;
    holder_finish(&h, t);
    xbox_IrqStatsGet(&st, 0);
    check(posted && s_runs == 1 && st.posted[XBOX_IRQ_APU] == 1,
          "coalescing: three posts, one run");
}

/* The lost-post window: the hook runs between the poster's first try and
 * its post, and lets the holder release the gate there. */
static Holder *s_lost_holder;
static void lost_hook(int line)
{
    (void)line;
    SetEvent(s_lost_holder->resume);
    WaitForSingleObject(s_lost_holder->done, 10000);
}

static void lost_post(void)
{
    Holder h = {0};
    HANDLE t;
    XboxIrqStats st;
    int how;

    reset_counts();
    h.release_only = 1;
    t = holder_start(&h);
    s_lost_holder = &h;
    xbox_IrqSetPostHook(lost_hook);
    how = deliver(XBOX_IRQ_APU);
    xbox_IrqSetPostHook(NULL);
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
    CloseHandle(h.go_mid); CloseHandle(h.resume); CloseHandle(h.done);
    xbox_IrqStatsGet(&st, 0);
    check(how == XBOX_IRQ_GATED && s_runs == 1 && s_run_tid != h.tid
          && st.second_try[XBOX_IRQ_APU] == 1,
          "lost-post window: the poster's second try delivers it");
}

/* The lower's window: the hook runs on the holder inside the lower, after
 * its safe point and before the gate is left, and lets the poster post. */
static void window_hook(void)
{
    SetEvent(ev_a);
    WaitForSingleObject(ev_b, 10000);
}

static DWORD WINAPI window_holder(LPVOID p)
{
    int *after = (int *)p, saved;

    g_fs_base = 0;
    after[2] = (int)GetCurrentThreadId();
    saved = xbox_IrqlEnterInterrupt(DISPATCH_LEVEL);
    xbox_IrqSetReleaseHook(window_hook);
    xbox_IrqlLeaveInterrupt(saved);         /* the window is in here */
    xbox_IrqSetReleaseHook(NULL);
    after[0] = (int)s_runs;
    after[1] = xbox_IrqlCurrent();
    return 0;
}

static void lower_window(void)
{
    int after[3] = {-1, -1, 0};
    HANDLE t;
    int how;

    reset_counts();
    t = CreateThread(NULL, 0, window_holder, after, 0, NULL);
    WaitForSingleObject(ev_a, 10000);       /* holder is inside its lower */
    how = deliver(XBOX_IRQ_APU);
    SetEvent(ev_b);
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
    check(how == XBOX_IRQ_POSTED, "lower window: posted while the gate is still held");
    check(after[0] == 1 && s_run_tid == (unsigned long)after[2],
          "lower window: run by the release point, on the holder");
    check(after[1] == PASSIVE_LEVEL, "lower window: the holder ends at its new level");
}

static void wait_and_log(void)
{
    Holder h = {0};
    HANDLE t;
    XboxIrqStats st;
    uint64_t t0, forced_at = 0;
    int how, all_posted = 1;

    reset_counts();
    set_env(RENV_IRQ_SAFE_MS, "20");
    h.release_only = 1;
    t = holder_start(&h);
    t0 = xbox_HostNowNs();
    while (xbox_HostNowNs() - t0 < 45 * MS) {
        all_posted &= deliver(XBOX_IRQ_APU) == XBOX_IRQ_POSTED;
        Sleep(1);
    }
    xbox_IrqStatsGet(&st, 0);
    check(all_posted && s_runs == 0, "wait: no safe point, no run");
    check(st.wait_logs >= 1, "wait: logged past irq_safe_ms");

    /* irq_safe_force=1: beside the holder once irq_safe_ms is up. The bit
     * is still pending from above, so it is overdue at once; post anew. */
    set_env(RENV_IRQ_SAFE_FORCE, "1");
    how = deliver(XBOX_IRQ_APU);
    check(how == XBOX_IRQ_UNGATED && s_runs == 1,
          "force: an overdue post is delivered beside the holder");
    t0 = xbox_HostNowNs();
    while (xbox_HostNowNs() - t0 < 200 * MS) {
        if (deliver(XBOX_IRQ_APU) == XBOX_IRQ_UNGATED) {
            forced_at = xbox_HostNowNs() - t0;
            break;
        }
        Sleep(1);
    }
    check(forced_at >= 19 * MS, "force: not before irq_safe_ms");
    holder_finish(&h, t);
    set_env(RENV_IRQ_SAFE_FORCE, NULL);
    set_env(RENV_IRQ_SAFE_MS, NULL);
    printf("    (forced after %.1f ms)\n", (double)forced_at / MS);
}

/* ── Stress ───────────────────────────────────────────────────────── */

static volatile LONG s_stop;

static DWORD WINAPI stress_poster(LPVOID p)
{
    unsigned r = 12345;
    (void)p;
    g_fs_base = 0;
    while (!s_stop) {
        r = r * 1103515245u + 12345u;
        deliver(XBOX_IRQ_APU);
        if (r & 0x100)
            Sleep(0);
    }
    return 0;
}

static void stress(void)
{
    HANDLE t;
    int round, saved, i;

    reset_counts();
    s_mode = R_STRESS;
    s_stop = 0;
    t = CreateThread(NULL, 0, stress_poster, NULL, 0, NULL);
    for (round = 0; round < 100000; round++) {
        Node *n = &s_pool[round & 31];
        saved = xbox_IrqlEnterInterrupt(DISPATCH_LEVEL);
        for (i = 0; i < 2; i++) {
            /* Half an edit, a canary, the other half: nothing may see it. */
            if (n->next && n->next != n) {
                InterlockedExchange(&s_canary, 1);
                n->prev->next = n->next;
                n->next->prev = n->prev;
                n->next = n->prev = n;
                InterlockedExchange(&s_canary, 0);
            } else {
                n->next = s_head.next;
                n->prev = &s_head;
                InterlockedExchange(&s_canary, 1);
                s_head.next->prev = n;
                s_head.next = n;
                InterlockedExchange(&s_canary, 0);
            }
            xbox_IrqSafePoint();        /* between two guest operations */
        }
        xbox_IrqlLeaveInterrupt(saved);
    }
    s_stop = 1;
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
    check(s_saw_canary == 0, "stress: no routine saw a half-done edit");
    check(list_count_ok() >= 0, "stress: the list is intact");
    printf("    (%ld routine runs)\n", (long)s_runs);
}

int main(void)
{
    g_fs_base = 0;
    g_xbox_stack_size = 8u * 1024 * 1024;   /* worker stack slices to hand out */
    /* Guest memory only where the worker stacks are: a routine run from
     * PASSIVE_LEVEL (the lower window) raises to 16 there, and the raise
     * records the guest return address at its esp. */
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)calloc(
        XBOX_WORKER_STACK_COUNT, XBOX_WORKER_STACK_SIZE)
        - (ptrdiff_t)XBOX_WORKER_STACK_BASE;
    ev_a = CreateEventW(NULL, FALSE, FALSE, NULL);
    ev_b = CreateEventW(NULL, FALSE, FALSE, NULL);
    xbox_IrqSetHandler(XBOX_IRQ_APU, apu_routine);
    xbox_IrqSetHandler(XBOX_IRQ_OHCI, ohci_routine);

    printf("irq_safe_points:\n");
    check(xbox_IrqSafePointsOn(), "safe points are the default");
    race_shown();
    gate_release();
    non_holder();
    registers();
    nesting();
    coalescing();
    lost_post();
    lower_window();
    wait_and_log();
    stress();
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
