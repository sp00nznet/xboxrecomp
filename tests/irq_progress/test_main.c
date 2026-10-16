/*
 * Interrupt service and DPC completion while the one guest thread spins,
 * apart from any guest CPU hand-off (kernel_hal.c xbox_IrqPost,
 * xbox_IrqSafePoint and xbox_KfLowerIrql; kernel_bridge.c the thunk
 * dispatch and the timer thread's DPC drain).
 *
 * RECOMP_GUEST_LOCK is on and there is one guest thread, so the CPU has
 * nobody to go to: whatever progress is made here is made by interrupt
 * delivery and the DPC drains, and the guest CPU's own counters must show
 * no turn, switch or preemption at the end. The guest thread's kernel calls
 * go through the real thunk dispatch. A device thread posts the APU line as
 * the APU model does, and a timer stand-in runs the timer thread's drain in
 * the timer thread's loop (the timer thread itself needs a title's TLS
 * image). Each case measures the post-to-ISR and post-to-DPC age:
 *  - (a) a spin at PASSIVE with no kernel call: the ISR is delivered at once
 *    on the device's thread, and the DPC completes on the timer thread while
 *    the guest spins, which sees its result;
 *  - (b) a spin at DISPATCH with a kernel call every millisecond: the ISR
 *    runs on the guest thread at its next call, at IRQL 16; the DPC waits
 *    while the thread stays at DISPATCH (the timer drain finds the gate
 *    busy) and completes at the lower;
 *  - (c) a spin at DISPATCH on loop back edges only: the line stays posted,
 *    the wait log names the holder past RECOMP_IRQ_SAFE_MS, and the first
 *    kernel call runs it. With RECOMP_IRQ_SAFE_FORCE=1 a repeat post past
 *    RECOMP_IRQ_SAFE_MS delivers it beside the holder instead;
 *  - (d) raised above DISPATCH: the kernel calls' safe points refuse and
 *    count it, and the lower back to DISPATCH runs it, before any further
 *    kernel call, as the console takes it at the lower.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The tests change the environment the kernel reads its switches from. */
static void test_setenv(const char *name, const char *v)
{
#ifdef _WIN32
    _putenv_s(name, v ? v : "");
#else
    if (v)
        setenv(name, v, 1);
    else
        unsetenv(name);
#endif
}

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
extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

#define MS 1000000ull
#define MEM32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define MEM8(va)  (*(volatile uint8_t *)((uintptr_t)(va) + g_xbox_mem_offset))

#define GUEST_MEM  (16u * 1024 * 1024)
#define THUNK_VA   0x00010000u
#define KDPC_VA    0x00030000u
#define SAFE_MS    20u
/* "At once": generous for a loaded CI host or Wine; the ages are printed. */
#define PROMPT_NS  (50 * MS)

enum { K_QPC, K_RAISE, K_LOWER, K_COUNT };
static const uint32_t k_ordinals[K_COUNT] = {
    126,    /* KeQueryPerformanceCounter: a call with no effect here */
    160,    /* KfRaiseIrql, fastcall */
    161,    /* KfLowerIrql, fastcall */
};

static int failures;

/* The flag is only ever accessed atomically (kernel.h). */
static int32_t preempt_flag(void)
{
    return (int32_t)InterlockedCompareExchange(
        (volatile LONG *)&g_xbox_guest_cpu_preempt, 0, 0);
}

static void check(int ok, const char *what)
{
    printf("  %-70s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static double ms_of(uint64_t ns) { return (double)ns / MS; }

/* A kernel call the way generated code makes one: the return address
 * pushed, the argument in ECX for the fastcall pair, through the thunk. */
static uint32_t kcall(int k, uint32_t ecx)
{
    uint32_t esp0 = g_esp;
    recomp_func_t fn = recomp_lookup_kernel(MEM32(THUNK_VA + 4u * (uint32_t)k));

    g_esp -= 4;
    MEM32(g_esp) = 0xBEEF0000u | (uint32_t)k;
    g_ecx = ecx;
    fn();
    if (g_esp != esp0) {
        printf("  kernel call %u left esp %08X, expected %08X\n",
               k_ordinals[k], g_esp, esp0);
        failures++;
        g_esp = esp0;
    }
    return g_eax;
}

/* RECOMP_BACK_EDGE, as generated code has it at every loop back edge. */
static void back_edge(void)
{
    if (preempt_flag())
        xbox_GuestCpuPreempt(0x00401000u);
}

/* ── The ISR, the DPC and the device ─────────────────────────────── */

static volatile LONG s_isr_runs, s_dpc_runs;
static volatile unsigned long s_isr_tid, s_dpc_tid;
static volatile int s_isr_irql, s_dpc_irql;
static volatile uint64_t s_post_ns, s_isr_ns, s_dpc_ns;

static void apu_isr(void)
{
    s_isr_ns = xbox_HostNowNs();
    s_isr_tid = (unsigned long)GetCurrentThreadId();
    s_isr_irql = xbox_IrqlCurrent();
    xbox_DpcQueue(KDPC_VA, 1, 2);
    InterlockedIncrement(&s_isr_runs);
}

static void dpc_routine(void)
{
    s_dpc_ns = xbox_HostNowNs();
    s_dpc_tid = (unsigned long)GetCurrentThreadId();
    s_dpc_irql = xbox_IrqlCurrent();
    InterlockedIncrement(&s_dpc_runs);
    g_esp += 20;                        /* ret 16, and the dummy return */
}

typedef struct {
    int delay_ms;                       /* before the first post */
    int repost_ms;                      /* 0: post once */
    int max_ms;                         /* stop reposting after */
    int gated, posted, ungated;
    unsigned long tid;
} Device;

/* What the APU model does with xbox_IrqPost's answer. */
static int deliver(Device *d)
{
    int how = xbox_IrqPost(XBOX_IRQ_APU);

    if (how == XBOX_IRQ_GATED || how == XBOX_IRQ_UNGATED) {
        int irql;
        if (how == XBOX_IRQ_UNGATED)
            xbox_IrqNoteUngated(XBOX_IRQ_APU);
        irql = xbox_IrqlEnterInterrupt(16);
        apu_isr();
        xbox_IrqlLeaveInterrupt(irql);
        if (how == XBOX_IRQ_GATED)
            xbox_DispatchGateLeave();
    }
    if (how == XBOX_IRQ_GATED) d->gated++;
    else if (how == XBOX_IRQ_UNGATED) d->ungated++;
    else d->posted++;
    return how;
}

static DWORD WINAPI device_main(LPVOID p)
{
    Device *d = (Device *)p;
    LONG before = s_isr_runs;
    uint64_t t0;

    g_fs_base = 0;
    d->tid = (unsigned long)GetCurrentThreadId();
    Sleep((DWORD)d->delay_ms);
    s_post_ns = t0 = xbox_HostNowNs();
    for (;;) {
        deliver(d);
        if (s_isr_runs != before || !d->repost_ms
            || xbox_HostNowNs() - t0 >= (uint64_t)d->max_ms * MS)
            break;
        Sleep((DWORD)d->repost_ms);
    }
    return 0;
}

static HANDLE device_start(Device *d)
{
    return CreateThread(NULL, 0, device_main, d, 0, NULL);
}

static void device_join(HANDLE t)
{
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
}

/* ── The timer stand-in: the timer thread's loop, minus vblanks and timers ── */

static volatile LONG s_timer_stop, s_timer_busy;
static volatile unsigned long s_timer_tid;

static DWORD WINAPI timer_main(LPVOID p)
{
    uint64_t wait = 10 * MS;
    int slot = xbox_worker_stack_alloc();

    (void)p;
    g_fs_base = 0;
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    s_timer_tid = (unsigned long)GetCurrentThreadId();
    xbox_HostTimerThreadInit();
    while (!s_timer_stop) {
        int ran, busy;
        xbox_HostTimerSleepNs(wait);
        busy = xbox_DpcDrainTimer(&ran);
        if (busy)
            InterlockedIncrement(&s_timer_busy);
        wait = busy ? 1 * MS : 10 * MS;
    }
    xbox_worker_stack_free(slot);
    return 0;
}

/* ── The guest thread's side ──────────────────────────────────────── */

static unsigned long s_main_tid;

static void reset(void)
{
    XboxIrqStats s;

    s_isr_runs = s_dpc_runs = 0;
    s_isr_tid = s_dpc_tid = 0;
    s_isr_irql = s_dpc_irql = -1;
    s_post_ns = s_isr_ns = s_dpc_ns = 0;
    s_timer_busy = 0;
    xbox_IrqStatsGet(&s, 1);
}

/* Back edges only, for `ns` or until *flag moves off `from`. */
static void spin(uint64_t ns, volatile LONG *flag, LONG from)
{
    uint64_t t0 = xbox_HostNowNs();

    while (xbox_HostNowNs() - t0 < ns && (!flag || *flag == from))
        back_edge();
}

/* A loop that makes a kernel call every millisecond. */
static void spin_calls(uint64_t ns, volatile LONG *flag, LONG from)
{
    uint64_t t0 = xbox_HostNowNs();

    while (xbox_HostNowNs() - t0 < ns && (!flag || *flag == from)) {
        spin(1 * MS, NULL, 0);
        kcall(K_QPC, 0);
    }
}

/* At PASSIVE: nothing should be left queued after a case. */
static void settle(void)
{
    spin(200 * MS, &s_dpc_runs, s_dpc_runs);
    check(!xbox_DpcPending(), "    nothing left queued");
}

static void passive_spin(void)
{
    enum { ROUNDS = 20 };
    uint64_t isr_max = 0, dpc_max = 0;
    int i, on_device = 0, on_timer = 0, seen = 0, irql_ok = 0;

    for (i = 0; i < ROUNDS; i++) {
        Device d = {0};
        HANDLE t;

        reset();
        d.delay_ms = 2;
        t = device_start(&d);
        /* The guest waits on the DPC's work with no kernel call. */
        spin(2000 * MS, &s_dpc_runs, 0);
        seen += s_dpc_runs == 1;
        device_join(t);
        /* The device's own thread, or the timer's if it held the gate for a
         * drain just then (its release point): never the guest's. */
        on_device += s_isr_tid && s_isr_tid != s_main_tid;
        on_timer += s_dpc_tid == s_timer_tid;
        irql_ok += s_isr_irql == 16 && s_dpc_irql == DISPATCH_LEVEL;
        if (s_isr_ns - s_post_ns > isr_max) isr_max = s_isr_ns - s_post_ns;
        if (s_dpc_ns - s_post_ns > dpc_max) dpc_max = s_dpc_ns - s_post_ns;
    }
    printf("    %d rounds: post to ISR max %.3f ms, post to DPC max %.3f ms\n",
           ROUNDS, ms_of(isr_max), ms_of(dpc_max));
    check(on_device == ROUNDS, "(a) each ISR ran at once, off the guest thread (the gate was free)");
    check(on_timer == ROUNDS && irql_ok == ROUNDS,
          "(a) each DPC ran on the timer thread at DISPATCH, ISRs at 16");
    check(seen == ROUNDS, "(a) the spinning guest saw each DPC's result, no kernel call made");
    check(isr_max < PROMPT_NS && dpc_max < PROMPT_NS,
          "(a) post to ISR and post to DPC within 50 ms (the timer wake)");
}

static void dispatch_spin_calls(void)
{
    Device d = {0};
    HANDLE t;
    XboxIrqStats st;
    uint32_t old;
    uint64_t lowered;
    LONG busy_at_dispatch;

    reset();
    old = kcall(K_RAISE, DISPATCH_LEVEL);
    d.delay_ms = 5;
    t = device_start(&d);
    spin_calls(2000 * MS, &s_isr_runs, 0);
    device_join(t);
    printf("    post to ISR %.3f ms\n", ms_of(s_isr_ns - s_post_ns));
    check(d.posted == 1 && s_isr_runs == 1, "(b) posted to the guest thread, which ran it");
    check(s_isr_tid == s_main_tid && s_isr_irql == 16,
          "(b) on the guest thread, at IRQL 16, at a kernel call");
    check(s_isr_ns - s_post_ns < PROMPT_NS, "(b) post to ISR within 50 ms (one call period)");
    /* The ISR queued its DPC: the guest stays at DISPATCH and keeps
     * calling, and the timer's drain finds the gate busy each pass. */
    spin_calls(30 * MS, NULL, 0);
    busy_at_dispatch = s_timer_busy;
    check(s_dpc_runs == 0, "(b) the DPC waits while the thread stays at DISPATCH");
    check(busy_at_dispatch > 0, "(b) the timer drain came and found the gate busy");
    lowered = xbox_HostNowNs();
    kcall(K_LOWER, old);
    spin(1000 * MS, &s_dpc_runs, 0);
    printf("    lower to DPC %.3f ms, on the %s thread\n", ms_of(s_dpc_ns - lowered),
           s_dpc_tid == s_main_tid ? "guest" : s_dpc_tid == s_timer_tid ? "timer" : "other");
    check(s_dpc_runs == 1 && s_dpc_ns - lowered < PROMPT_NS,
          "(b) the DPC completes at the lower");
    check(s_dpc_ns - s_post_ns < 200 * MS, "(b) post to DPC bounded by the DISPATCH spin");
    xbox_IrqStatsGet(&st, 0);
    check(st.on_holder[XBOX_IRQ_APU] == 1 && st.forced[XBOX_IRQ_APU] == 0,
          "(b) counted as run on the holder, nothing forced");
    settle();
}

static void dispatch_spin_back_edges(void)
{
    Device d = {0};
    HANDLE t;
    XboxIrqStats st;
    uint32_t old;
    uint64_t age;

    reset();
    old = kcall(K_RAISE, DISPATCH_LEVEL);
    d.delay_ms = 5;
    d.repost_ms = 5;
    d.max_ms = 100;
    t = device_start(&d);
    spin(150 * MS, NULL, 0);
    device_join(t);
    xbox_IrqStatsGet(&st, 0);
    check(s_isr_runs == 0 && d.gated == 0 && d.ungated == 0,
          "(c) back edges only: the line stays posted for 150 ms");
    check(st.wait_logs >= 1, "(c) the wait log named the holder past RECOMP_IRQ_SAFE_MS");
    kcall(K_QPC, 0);
    age = s_isr_ns - s_post_ns;
    printf("    post to ISR %.3f ms (the first kernel call)\n", ms_of(age));
    check(s_isr_runs == 1 && s_isr_tid == s_main_tid && s_isr_irql == 16,
          "(c) the first kernel call ran it on the guest thread");
    kcall(K_LOWER, old);
    spin(1000 * MS, &s_dpc_runs, 0);
    check(s_dpc_runs == 1, "(c) and its DPC completed at the lower");
    settle();

    /* RECOMP_IRQ_SAFE_FORCE: a repeat post past the wait goes beside the holder. */
    test_setenv("RECOMP_IRQ_SAFE_FORCE", "1");
    xbox_IrqReloadConfig();
    reset();
    memset(&d, 0, sizeof d);
    old = kcall(K_RAISE, DISPATCH_LEVEL);
    d.delay_ms = 5;
    d.repost_ms = 5;
    d.max_ms = 1000;
    t = device_start(&d);
    spin(2000 * MS, &s_isr_runs, 0);
    device_join(t);
    age = s_isr_ns - s_post_ns;
    printf("    post to forced ISR %.3f ms (RECOMP_IRQ_SAFE_MS=%u, reposts every 5 ms)\n",
           ms_of(age), SAFE_MS);
    xbox_IrqStatsGet(&st, 0);
    check(s_isr_runs == 1 && s_isr_tid == d.tid && d.ungated == 1
          && st.forced[XBOX_IRQ_APU] == 1,
          "(c) RECOMP_IRQ_SAFE_FORCE=1: delivered beside the holder on the device's thread");
    check(age >= SAFE_MS * MS && age < SAFE_MS * MS + PROMPT_NS,
          "(c) after RECOMP_IRQ_SAFE_MS, within one repost and 50 ms more");
    spin(30 * MS, NULL, 0);
    check(s_dpc_runs == 0, "(c) its DPC still waits for the holder to leave DISPATCH");
    kcall(K_LOWER, old);
    spin(1000 * MS, &s_dpc_runs, 0);
    check(s_dpc_runs == 1, "(c) and completes at the lower");
    test_setenv("RECOMP_IRQ_SAFE_FORCE", NULL);
    xbox_IrqReloadConfig();
    settle();
}

static void above_dispatch(void)
{
    Device d = {0};
    HANDLE t;
    XboxIrqStats st;
    uint32_t old;
    LONG calls_before;

    reset();
    old = kcall(K_RAISE, 26);
    d.delay_ms = 5;
    t = device_start(&d);
    device_join(t);
    spin_calls(20 * MS, NULL, 0);
    xbox_IrqStatsGet(&st, 0);
    check(d.posted == 1 && s_isr_runs == 0, "(d) at IRQL 26 the kernel calls do not run it");
    check(st.refused >= 1, "(d) their safe points refused and counted it");
    calls_before = s_isr_runs;
    kcall(K_LOWER, DISPATCH_LEVEL);
    check(calls_before == 0 && s_isr_runs == 1 && s_isr_tid == s_main_tid
          && s_isr_irql == 16,
          "(d) the lower to DISPATCH ran it, on the guest thread, at 16");
    check(xbox_IrqlCurrent() == DISPATCH_LEVEL, "(d) and left the thread at DISPATCH");
    printf("    post to ISR %.3f ms (20 ms at IRQL 26)\n", ms_of(s_isr_ns - s_post_ns));
    spin(20 * MS, NULL, 0);
    check(s_dpc_runs == 0, "(d) its DPC waits at DISPATCH");
    kcall(K_LOWER, old);
    spin(1000 * MS, &s_dpc_runs, 0);
    check(s_dpc_runs == 1, "(d) and completes at the lower to PASSIVE");

    /* Straight from 26 to PASSIVE: the release point takes it. */
    reset();
    memset(&d, 0, sizeof d);
    old = kcall(K_RAISE, 26);
    d.delay_ms = 5;
    t = device_start(&d);
    device_join(t);
    kcall(K_LOWER, old);
    spin(1000 * MS, &s_dpc_runs, 0);
    check(s_isr_runs == 1 && s_isr_tid == s_main_tid && s_dpc_runs == 1,
          "(d) a lower from 26 to PASSIVE runs the ISR on the guest thread, then the DPC");
    settle();
}

int main(void)
{
    uint8_t *mem;
    XboxGuestCpuStats gs;
    HANDLE timer;
    int slot, i;

    setvbuf(stdout, NULL, _IONBF, 0);
    g_fs_base = 0;
    mem = VirtualAlloc(NULL, GUEST_MEM, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) {
        printf("guest memory stand-in not allocated\n");
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)mem;
    MEM32(KDPC_VA + 12) = DPC_ROUTINE_VA;      /* DeferredRoutine */
    MEM32(KDPC_VA + 16) = 0xC0FFEEu;           /* DeferredContext */
    for (i = 0; i < K_COUNT; i++)
        MEM32(THUNK_VA + 4u * (uint32_t)i) = 0x80000000u | k_ordinals[i];

    test_setenv("RECOMP_GUEST_LOCK", "1");
    test_setenv("RECOMP_DISPATCH_GATE", "1");
    test_setenv("RECOMP_IRQ_SAFE_MS", "20");
    xbox_GuestCpuReloadConfig();
    xbox_IrqReloadConfig();
    xbox_kernel_set_thunk_address(THUNK_VA, K_COUNT);
    /* Marks this thread a guest thread and gives it the guest CPU, as for
     * the title's entry point. */
    xbox_kernel_bridge_init();
    slot = xbox_worker_stack_alloc();
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    s_main_tid = (unsigned long)GetCurrentThreadId();
    xbox_IrqSetHandler(XBOX_IRQ_APU, apu_isr);
    timer = CreateThread(NULL, 0, timer_main, NULL, 0, NULL);
    while (!s_timer_tid)
        Sleep(1);

    printf("irq_progress:\n");
    check(xbox_GuestCpuOn() && xbox_GuestCpuHeld(),
          "RECOMP_GUEST_LOCK=1: the one guest thread holds the guest CPU");
    xbox_GuestCpuStatsGet(&gs, 1);
    passive_spin();
    dispatch_spin_calls();
    dispatch_spin_back_edges();
    above_dispatch();

    xbox_GuestCpuStatsGet(&gs, 0);
    printf("    guest CPU: turns %ld, releases %ld, switches %ld, preempts %ld\n",
           gs.turns, gs.releases, gs.switches, gs.preempts);
    check(gs.turns == 0 && gs.switches == 0 && gs.preempts == 0 && gs.releases > 0,
          "no guest CPU hand-off made any of it: no turn, switch or preemption");
    check(xbox_GuestCpuHeld(), "the guest thread still holds the guest CPU");

    s_timer_stop = 1;
    xbox_HostTimerWake();
    WaitForSingleObject(timer, 10000);
    CloseHandle(timer);
    xbox_GuestCpuPart();
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
