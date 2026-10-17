/*
 * DPCs queued by an interrupt run before guest code resumes below DISPATCH
 * (kernel_hal.c dpc_run_here, kernel_bridge.c xbox_DpcQueue and
 * xbox_DpcDrainHere, host_time.c xbox_HostTimerSleepNs).
 *
 * On the console a DPC queued by an ISR runs when the processor drops below
 * DISPATCH, before the interrupted thread resumes; DirectSound's ISR appends
 * to the list its DPC drains with no linked check because of that. Each case
 * orders its threads with events, so it is deterministic:
 *  - (a) a gated interrupt on a model thread queues a DPC; a guest thread's
 *    raise to DISPATCH runs it before the raise returns, on that thread, at
 *    DISPATCH, with the thread's registers and its return value intact;
 *  - (b) a guest holder takes the posted interrupt at a safe point; the DPC
 *    does not run inside the ISR and runs at the lower that gives the gate
 *    back, on the holder;
 *  - (c) a queued DPC ends the timer thread's sleep at once; a wake before
 *    the sleep is kept for it; an unwoken sleep lasts;
 *  - (d) the model thread's own gate release never runs a queued DPC, nor
 *    does an unmarked thread's raise;
 *  - (e) dpc_on_raise=0: the raise returns with the DPC still queued (the
 *    old ordering, recorded);
 *  - (g) with RECOMP_GUEST_LOCK on, a guest thread that let the guest CPU
 *    go for its kernel call runs the routine without it, as the timer
 *    thread does: a DPC never holds the guest CPU (kernel_hal.c
 *    dpc_run_here: it drains under the gate, and taking the CPU there could
 *    wait on a holder spinning on this DPC's result),
 *  - (f) a routine that queues itself again runs twice in the same drain,
 *    and its own raise and lower start no nested drain; a KDPC queued twice
 *    is queued once.
 */
#include "kernel.h"
#include "recomp_env.h"
#include "xbox_memory_layout.h"
#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The DPC routine is "generated code" looked up by its guest address. */
#define DPC_ROUTINE_VA 0x00001000u
typedef void (*recomp_func_t)(void);
static void dpc_routine(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va)
{
    return xbox_va == DPC_ROUTINE_VA ? dpc_routine : NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_ebp;
extern RECOMP_TLS RecompXmm g_xmm0;
extern uint32_t g_xbox_stack_size;
extern ptrdiff_t g_xbox_mem_offset;

#define MS 1000000ull
#define MEM32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define MEM8(va)  (*(volatile uint8_t *)((uintptr_t)(va) + g_xbox_mem_offset))

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* The KDPC lives just past the worker stack slices, inside what main maps. */
static uint32_t kdpc_va(void)
{
    return XBOX_WORKER_STACK_BASE
           + XBOX_WORKER_STACK_COUNT * XBOX_WORKER_STACK_SIZE + 0x100;
}

/* ── The DPC and the ISR ─────────────────────────────────────────── */

static volatile LONG s_dpc_runs;
static unsigned long s_dpc_tid;
static int s_dpc_irql;
static int s_requeue;                   /* (f): queue itself once */
static int s_dpc_held;                  /* (g): xbox_GuestCpuHeld in the routine */

static void dpc_routine(void)
{
    KIRQL o;

    InterlockedIncrement(&s_dpc_runs);
    s_dpc_tid = (unsigned long)GetCurrentThreadId();
    s_dpc_irql = xbox_IrqlCurrent();
    s_dpc_held = xbox_GuestCpuHeld();
    /* A routine's own raise and lower: no nested drain starts here. */
    o = xbox_KfRaiseIrql(DISPATCH_LEVEL);
    xbox_KfLowerIrql(o);
    if (s_requeue && s_dpc_runs == 1)
        xbox_DpcQueue(kdpc_va(), 7, 8);
    g_esp += 20;                        /* ret 16, and the dummy return */
}

static volatile LONG s_isr_runs;
static int s_isr_saw_dpc;               /* the DPC ran inside the ISR: never */

static void apu_isr(void)
{
    LONG before = s_dpc_runs;

    InterlockedIncrement(&s_isr_runs);
    xbox_DpcQueue(kdpc_va(), 1, 2);
    if (s_dpc_runs != before)
        s_isr_saw_dpc = 1;
}

/* What a device model does with the answer (as the APU frame thread). */
static int deliver(void)
{
    int how = xbox_IrqPost(XBOX_IRQ_APU);

    if (how == XBOX_IRQ_GATED || how == XBOX_IRQ_UNGATED) {
        int irql = xbox_IrqlEnterInterrupt(16);
        apu_isr();
        xbox_IrqlLeaveInterrupt(irql);
        if (how == XBOX_IRQ_GATED)
            xbox_DispatchGateLeave();
    }
    return how;
}

static void reset(void)
{
    s_dpc_runs = s_isr_runs = 0;
    s_dpc_tid = 0;
    s_dpc_irql = -1;
    s_isr_saw_dpc = 0;
    s_requeue = 0;
    MEM8(kdpc_va() + 2) = 0;
}

/* ── A guest thread, driven by events ────────────────────────────── */

typedef struct {
    HANDLE go, mid, resume, done;
    int hold;                   /* raise first, take the ISR at a safe point */
    int mark;                   /* xbox_DpcMarkGuestThread (0: a model thread) */
    int lock;                   /* (g): joined the guest CPU, released for the call */
    unsigned long tid;
    int runs_at_raise, runs_after_point, runs_after_lower;
    int old_ok, regs_ok;
    int held_before, held_after;
} Guest;

static DWORD WINAPI guest_main(LPVOID p)
{
    Guest *g = (Guest *)p;
    RecompXmm x;
    uint32_t esp0;
    KIRQL old;
    int slot;

    g_fs_base = 0;                      /* no KPCR: keep IRQL off guest memory */
    if (g->mark)
        xbox_DpcMarkGuestThread();
    slot = xbox_worker_stack_alloc();
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    g->tid = (unsigned long)GetCurrentThreadId();
    g_eax = 1; g_ecx = 2; g_edx = 3; g_ebx = 4; g_esi = 5; g_edi = 6;
    g_ebp = 7; esp0 = g_esp;
    memset(&g_xmm0, 0x5A, sizeof g_xmm0);
    x = g_xmm0;
    WaitForSingleObject(g->go, 10000);
    if (g->lock) {
        /* What kernel_thunk_dispatch does around the raise bridge. */
        xbox_GuestCpuJoin();
        g->held_before = xbox_GuestCpuHeld();
        xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 161);
    }
    old = xbox_KfRaiseIrql(DISPATCH_LEVEL);
    if (g->lock) {
        g->held_after = xbox_GuestCpuHeld();
        xbox_GuestCpuAcquire();
    }
    g->runs_at_raise = (int)s_dpc_runs;
    g->old_ok = old == PASSIVE_LEVEL && xbox_IrqlCurrent() == DISPATCH_LEVEL;
    g->regs_ok = g_eax == 1 && g_ecx == 2 && g_edx == 3 && g_ebx == 4
                 && g_esi == 5 && g_edi == 6 && g_ebp == 7 && g_esp == esp0
                 && !memcmp(&g_xmm0, &x, sizeof x);
    if (g->hold) {
        SetEvent(g->mid);
        WaitForSingleObject(g->resume, 10000);
        xbox_IrqSafePoint();            /* its next kernel call, say */
        g->runs_after_point = (int)s_dpc_runs;
    }
    xbox_KfLowerIrql(old);
    g->runs_after_lower = (int)s_dpc_runs;
    if (g->lock)
        xbox_GuestCpuPart();
    xbox_worker_stack_free(slot);
    SetEvent(g->done);
    return 0;
}

static HANDLE guest_start(Guest *g)
{
    g->go = CreateEventW(NULL, FALSE, FALSE, NULL);
    g->mid = CreateEventW(NULL, FALSE, FALSE, NULL);
    g->resume = CreateEventW(NULL, FALSE, FALSE, NULL);
    g->done = CreateEventW(NULL, FALSE, FALSE, NULL);
    return CreateThread(NULL, 0, guest_main, g, 0, NULL);
}

static void guest_finish(Guest *g, HANDLE t)
{
    WaitForSingleObject(g->done, 10000);
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
    CloseHandle(g->go);
    CloseHandle(g->mid);
    CloseHandle(g->resume);
    CloseHandle(g->done);
}

/* Main's own drain, for the cases that leave the queue full on purpose. */
static void drain_on_main(void)
{
    xbox_DispatchGateEnter();
    xbox_DpcDrainHere();
    xbox_DispatchGateLeave();
}

/* ── The cases ────────────────────────────────────────────────────── */

static void raise_runs_it(void)
{
    Guest g = {0};
    HANDLE t;

    reset();
    g.mark = 1;
    t = guest_start(&g);
    check(deliver() == XBOX_IRQ_GATED && s_isr_runs == 1, "(a) ISR delivered with the gate; it queued the DPC");
    check(s_dpc_runs == 0, "(d) the model thread's gate release ran nothing");
    SetEvent(g.go);
    guest_finish(&g, t);
    check(g.runs_at_raise == 1, "(a) the guest's raise ran the DPC before returning");
    check(s_dpc_tid == g.tid, "(a) on the guest thread");
    check(s_dpc_irql == DISPATCH_LEVEL, "(a) at DISPATCH_LEVEL");
    check(g.old_ok && g.regs_ok, "(a) the raise's result and the guest's registers are intact");
    check(MEM32(kdpc_va() + 20) == 1 && MEM32(kdpc_va() + 24) == 2,
          "(a) the routine got the ISR's arguments");
    check(g.runs_after_lower == 1 && s_isr_saw_dpc == 0, "(a) once; never inside the ISR");
}

static void raise_holds_cpu(void)
{
    Guest g = {0};
    HANDLE t;

    reset();
    s_dpc_held = -1;
    recomp_env_set(RENV_GUEST_LOCK, "1");
    xbox_GuestCpuReloadConfig();
    g.mark = 1;
    g.lock = 1;
    t = guest_start(&g);
    check(deliver() == XBOX_IRQ_GATED && s_isr_runs == 1, "(g) ISR delivered with the gate; it queued the DPC");
    SetEvent(g.go);
    guest_finish(&g, t);
    check(g.runs_at_raise == 1 && s_dpc_tid == g.tid, "(g) the guest's raise ran the DPC on the guest thread");
    check(g.held_before == 1 && s_dpc_held == 0,
          "(g) the thread let the guest CPU go for the call; the routine ran without it");
    check(g.held_after == 0, "(g) the CPU is still let go when the call returns to its bridge");
    check(g.old_ok && g.regs_ok, "(g) the raise's result and the guest's registers are intact");
    recomp_env_set(RENV_GUEST_LOCK, "0");
    xbox_GuestCpuReloadConfig();
}

static void lower_runs_it(void)
{
    Guest g = {0};
    HANDLE t;

    reset();
    g.mark = 1;
    g.hold = 1;
    t = guest_start(&g);
    SetEvent(g.go);
    WaitForSingleObject(g.mid, 10000);
    check(deliver() == XBOX_IRQ_POSTED && s_isr_runs == 0, "(b) posted to the holder");
    SetEvent(g.resume);
    guest_finish(&g, t);
    check(s_isr_runs == 1 && s_isr_saw_dpc == 0, "(b) the holder's safe point ran the ISR, not the DPC");
    check(g.runs_after_point == 0, "(b) still queued after the safe point (IRQL is up)");
    check(g.runs_after_lower == 1 && s_dpc_tid == g.tid, "(b) the lower ran it, on the holder");
}

static uint64_t s_slept_ns;
static HANDLE ev_started;

static DWORD WINAPI sleeper(LPVOID p)
{
    uint64_t t0;

    SetEvent(ev_started);
    t0 = xbox_HostNowNs();
    xbox_HostTimerSleepNs((uint64_t)(uintptr_t)p * MS);
    s_slept_ns = xbox_HostNowNs() - t0;
    return 0;
}

static void timer_wake(void)
{
    HANDLE t;

    reset();
    /* Each queue so far left a wake pending that no sleeper took (the
     * guest threads drained those DPCs): one sleep takes it. */
    xbox_HostTimerSleepNs(1 * MS);
    ev_started = CreateEventW(NULL, FALSE, FALSE, NULL);
    t = CreateThread(NULL, 0, sleeper, (LPVOID)(uintptr_t)500, 0, NULL);
    WaitForSingleObject(ev_started, 10000);
    Sleep(5);                           /* let it reach the wait */
    xbox_DpcQueue(kdpc_va(), 0, 0);
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
    check(s_slept_ns < 50 * MS, "(c) a queued DPC ends the timer thread's sleep");
    printf("    (woke after %.2f ms)\n", (double)s_slept_ns / MS);
    drain_on_main();
    check(s_dpc_runs == 1, "(c) and the queue drains");

    t = CreateThread(NULL, 0, sleeper, (LPVOID)(uintptr_t)30, 0, NULL);
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
    check(s_slept_ns >= 25 * MS, "(c) an unwoken sleep lasts");

    xbox_HostTimerWake();               /* before the sleep: kept for it */
    t = CreateThread(NULL, 0, sleeper, (LPVOID)(uintptr_t)500, 0, NULL);
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
    check(s_slept_ns < 50 * MS, "(c) a wake before the sleep is not lost");
    CloseHandle(ev_started);
}

static void unmarked_thread(void)
{
    Guest g = {0};
    HANDLE t;

    reset();
    g.mark = 0;                         /* a thread that runs no guest code */
    t = guest_start(&g);
    deliver();
    SetEvent(g.go);
    guest_finish(&g, t);
    check(g.runs_at_raise == 0 && g.runs_after_lower == 0,
          "(d) an unmarked thread's raise and lower run nothing");
    drain_on_main();
    check(s_dpc_runs == 1, "(d) the timer thread's drain would take it");
}

static void old_ordering(void)
{
    Guest g = {0};
    HANDLE t;

    reset();
    recomp_env_set(RENV_DPC_ON_RAISE, "0");
    xbox_IrqReloadConfig();
    check(!xbox_DpcOnRaise(), "(e) dpc_on_raise=0");
    g.mark = 1;
    t = guest_start(&g);
    deliver();
    SetEvent(g.go);
    guest_finish(&g, t);
    check(g.runs_at_raise == 0 && g.runs_after_lower == 0,
          "(e) dpc_on_raise=0: the raise returns with the DPC queued (the old ordering, recorded)");
    recomp_env_set(RENV_DPC_ON_RAISE, NULL);
    xbox_IrqReloadConfig();
    check(xbox_DpcOnRaise(), "(e) the default is on");
    drain_on_main();
}

static void requeue(void)
{
    Guest g = {0};
    HANDLE t;

    reset();
    s_requeue = 1;
    g.mark = 1;
    t = guest_start(&g);
    deliver();
    check(xbox_DpcQueue(kdpc_va(), 3, 4) == 0, "(f) a KDPC already queued is not queued again");
    SetEvent(g.go);
    guest_finish(&g, t);
    check(g.runs_at_raise == 2, "(f) a routine that queues itself runs twice in one drain");
    check(MEM32(kdpc_va() + 20) == 7 && MEM32(kdpc_va() + 24) == 8,
          "(f) with its own arguments the second time");
    check(g.regs_ok, "(f) the routine's own raise and lower left the guest's registers alone");
}

int main(void)
{
    int slot;

    g_fs_base = 0;
    g_xbox_stack_size = 8u * 1024 * 1024;   /* worker stack slices to hand out */
    /* Guest memory where the worker stacks are, plus a page for the KDPC. */
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)calloc(
        XBOX_WORKER_STACK_COUNT * XBOX_WORKER_STACK_SIZE + 0x10000, 1)
        - (ptrdiff_t)XBOX_WORKER_STACK_BASE;
    MEM32(kdpc_va() + 12) = DPC_ROUTINE_VA;    /* DeferredRoutine */
    MEM32(kdpc_va() + 16) = 0xC0FFEEu;         /* DeferredContext */
    slot = xbox_worker_stack_alloc();           /* main's stack for drain_on_main */
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    xbox_IrqSetHandler(XBOX_IRQ_APU, apu_isr);
    printf("dpc_order:\n");
    check(xbox_DpcOnRaise(), "dpc_on_raise is the default");
    raise_runs_it();
    raise_holds_cpu();
    lower_runs_it();
    timer_wake();
    unmarked_thread();
    old_ordering();
    requeue();
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
