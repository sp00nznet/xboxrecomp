/*
 * kernel_hal.c - Hardware Abstraction Layer
 *
 * Implements IRQL simulation, performance counters, system time,
 * processor stalls, bug checks, floating point state, and hardware stubs.
 *
 * The Xbox HAL provides low-level hardware access that doesn't exist on
 * a standard Windows PC. Most of these functions are either:
 *   - Directly mappable (perf counters, system time)
 *   - Simulated (IRQL tracking via TLS)
 *   - Stubbed (PCI access, SMC, interrupts)
 */

#include "kernel.h"
#include "kernel_missing.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS, g_fs_base */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/host_time.h"
#if defined(_WIN32)
#include <intrin.h>
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#pragma intrinsic(_ReturnAddress)
#define IRQL_CALLER() _ReturnAddress()
#else
#define IRQL_CALLER() __builtin_return_address(0)
#endif

/* ============================================================================
 * IRQL Simulation
 *
 * Xbox uses IRQL (Interrupt Request Level) for synchronization:
 *   PASSIVE_LEVEL (0) - normal thread execution
 *   APC_LEVEL (1) - APC delivery
 *   DISPATCH_LEVEL (2) - scheduler/DPC level, no page faults allowed
 *
 * On Windows, we simulate IRQL with a thread-local variable. Raising to
 * DISPATCH_LEVEL doesn't actually prevent preemption, but the tracking
 * allows code that checks IRQL to function correctly.
 * ============================================================================ */

static XBOX_THREAD_LOCAL KIRQL g_current_irql = PASSIVE_LEVEL;

/* The current IRQL, where guest code reads it: KPCR.Irql, fs:[0x24].
 *
 * The XDK does not always ask the kernel. DirectSound's lock
 * (sub_002F4175 in Burnout 3) reads fs:[0x24] directly and skips its
 * critical section at raised IRQL, because a DPC must never block. Nothing
 * wrote that byte, so it read 0 everywhere: DirectSound's DPC took the
 * critical section, blocked against the thread that held it, and the title
 * froze with IRQL raised on the blocked thread. Every change of
 * g_current_irql is published here. */
extern RECOMP_TLS uint32_t g_fs_base;
/* The offset lifted code adds to every guest address; host code that reads
 * guest memory on its behalf uses the same one. */
extern ptrdiff_t g_xbox_mem_offset;

/* Set by the lower that gave the one-CPU gate back (irql_track_gated): the
 * release point runs once the new level is written, below. */
static XBOX_THREAD_LOCAL int t_release_due;
static void irql_release_due(void);

static void irql_publish(void)
{
    if (g_fs_base)
        *(volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + g_fs_base
                              + 0x24) = (uint8_t)g_current_irql;
    if (t_release_due)
        irql_release_due();
}

/* How many threads are holding IRQL at or above DISPATCH_LEVEL.
 *
 * The level itself is per-thread, which is right for a guest that asks "what
 * is my IRQL". It is wrong for the question a device model has to answer:
 * raising IRQL on hardware masks the interrupt for the whole processor, and
 * the title raises it precisely to keep an ISR out of structures it is in the
 * middle of editing. With the level thread-local, a controller thread sees
 * PASSIVE_LEVEL, calls the ISR anyway, and the two race over exactly the
 * state the guest was protecting -- which surfaces as an intermittent fault
 * on a garbage pointer, far from the code that dropped it.
 *
 * A count rather than a flag, because several threads can be raised at once
 * and the last one out is what re-opens the gate. */
static volatile LONG g_irql_raised_count = 0;

/* Non-zero while any thread is at or above DISPATCH_LEVEL. Device models call
 * this before delivering an interrupt; OHCI and the NV2A are level-triggered,
 * so a deferred interrupt is delivered on the next poll rather than lost. */
int xbox_IrqlBlocksInterrupts(void)
{
    return InterlockedCompareExchange(&g_irql_raised_count, 0, 0) != 0;
}

/* The raw depth, for callers that want to report it. A count that only ever
 * grows is a leak somewhere in the raise/lower pairs, and the number says so
 * where a yes/no cannot. */
int xbox_IrqlRaisedCount(void)
{
    return (int)InterlockedCompareExchange(&g_irql_raised_count, 0, 0);
}

static int  s_trace = -1;
static volatile LONG s_traced = 0;

/* Every crossing of the DISPATCH boundary, ever.
 *
 * The depth on its own cannot tell a leaked raise from a guest that is
 * genuinely sitting there: both read non-zero for as long as you look. A
 * count that stops moving says the first; one that races says the second.
 * That distinction is the whole difference between "the title is busy" and
 * "no device will ever get an interrupt again". */
static volatile LONG s_transitions = 0;

int xbox_IrqlTransitions(void)
{
    return (int)InterlockedCompareExchange(&s_transitions, 0, 0);
}

/* Which threads are holding the boundary up, and where they raised.
 *
 * A depth that stops changing has to be attributed to code, and the raise
 * that did it happened seconds earlier on a thread that has since gone
 * quiet -- there is nothing left to look at by the time anyone notices.
 * Keeping the caller's address per raised thread turns the number into a
 * name: these are host addresses inside the recompiled image, so nm resolves
 * them to the generated function, which is the guest function. */
#define IRQL_HOLDERS 8
static struct {
    volatile LONG tid;
    void *ra;
    uint32_t guest_ra, guest_esp;   /* the host ra only ever names the bridge */
} s_holders[IRQL_HOLDERS];
extern RECOMP_TLS uint32_t g_esp;

static void irql_holder_add(void *ra)
{
    LONG me = (LONG)GetCurrentThreadId();
    int i;

    for (i = 0; i < IRQL_HOLDERS; i++)
        if (InterlockedCompareExchange(&s_holders[i].tid, me, 0) == 0) {
            s_holders[i].ra = ra;
            s_holders[i].guest_esp = g_esp;
            s_holders[i].guest_ra = g_esp
                ? *(uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp) : 0;
            return;
        }
}

static void irql_holder_drop(void)
{
    LONG me = (LONG)GetCurrentThreadId();
    int i;

    for (i = 0; i < IRQL_HOLDERS; i++)
        if (InterlockedCompareExchange(&s_holders[i].tid, 0, me) == me)
            return;
}

void xbox_IrqlDumpHolders(void)
{
    int i;

    fprintf(stderr, "  [IRQLHOLD] depth=%d transitions=%d, raised threads:\n",
            xbox_IrqlRaisedCount(), xbox_IrqlTransitions());
    for (i = 0; i < IRQL_HOLDERS; i++) {
        LONG t = InterlockedCompareExchange(&s_holders[i].tid, 0, 0);
        if (t)
            fprintf(stderr, "  [IRQLHOLD]   tid %lu raised from host %p, "
                    "guest ret %08X (esp %08X)\n",
                    (unsigned long)t, s_holders[i].ra,
                    s_holders[i].guest_ra, s_holders[i].guest_esp);
    }
    fflush(stderr);
}

/* Whether this thread is in g_irql_raised_count. The count moves on this
 * flag, not on the edge alone, so a thread can never be counted twice or
 * taken off a count it was never on. */
static XBOX_THREAD_LOCAL int t_irql_counted;

/* The single-processor DISPATCH_LEVEL guarantee: while one thread (or a DPC)
 * is at DISPATCH_LEVEL or above, no other thread and no DPC runs on the
 * console's one CPU. Titles build their locks on that. DirectSound's lock
 * takes a critical section at PASSIVE_LEVEL and then raises, and its DPC
 * skips the critical section because it is at
 * DISPATCH_LEVEL: the raise is what keeps the DPC out of the voice lists a
 * thread is editing, and keeps a thread out while the DPC walks them.
 *
 * Here threads run in parallel and the level is only a number, so the DPC
 * (on the timer thread) walked a voice list a thread was relinking, found a
 * node pointing back into the list, and looped there forever at DISPATCH:
 * the IRQL depth stuck at one, every APU interrupt sat out the hold-off, the
 * front end stayed trapped and a level load never finished (Proton, about
 * 1 run in 20 once the title's stream threads woke on every vblank).
 *
 * So the transition to DISPATCH_LEVEL takes one process-wide gate and the
 * transition back releases it: thread raises and DPCs are serialized, as on
 * the console. Device interrupt service routines are delivered holding it
 * when it is free and run by its holder when it is not (xbox_IrqPost), never
 * beside the holder: on hardware they preempt DISPATCH-level code, which
 * stays frozen until they return.
 * Recursive, so a thread already through it (a DPC that raises again) never
 * waits on itself.
 *
 * Opt-in: RECOMP_DISPATCH_GATE=1. It changes when DPCs and device interrupts
 * run, which is a scheduling change for every title, and one B3 run in a
 * few stalled at its first movie with it. Off, the gate is not there: an
 * entry is free, raises take nothing, the timer thread runs DPCs and timer
 * routines as it always did, and the device models hold their interrupts
 * off on the raised-IRQL count (xbox_IrqlBlocksInterrupts). */
static int s_gate_on;                          /* RECOMP_DISPATCH_GATE */
static void irq_config_once(void);

int xbox_DispatchGateOn(void)
{
    irq_config_once();
    return s_gate_on;
}

static CRITICAL_SECTION s_dispatch_gate;
static INIT_ONCE s_dispatch_gate_once = INIT_ONCE_STATIC_INIT;
static XBOX_THREAD_LOCAL int t_dispatch_gate_held;
/* Every entry this thread has made into the gate, whoever made it: a raise,
 * a DPC drain, KeSynchronizeExecution, a device model's delivery. Non-zero
 * means this thread is the holder, which is what a safe point asks before it
 * runs a posted interrupt (see xbox_IrqPost). */
static XBOX_THREAD_LOCAL int t_gate_depth;
static volatile LONG s_gate_owner;

static BOOL CALLBACK dispatch_gate_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&s_dispatch_gate);
    return TRUE;
}

static void dispatch_gate_once(void)
{
    InitOnceExecuteOnce(&s_dispatch_gate_once, dispatch_gate_init, NULL, NULL);
}

static void gate_entered(void)
{
    if (t_gate_depth++ == 0)
        InterlockedExchange(&s_gate_owner, (LONG)GetCurrentThreadId());
}

static int gate_try(void)
{
    dispatch_gate_once();
    if (!TryEnterCriticalSection(&s_dispatch_gate))
        return 0;
    gate_entered();
    return 1;
}

static void gate_enter(void)
{
    dispatch_gate_once();
    EnterCriticalSection(&s_dispatch_gate);
    gate_entered();
}

static void gate_leave_raw(void)
{
    if (--t_gate_depth == 0)
        InterlockedExchange(&s_gate_owner, 0);
    LeaveCriticalSection(&s_dispatch_gate);
}

unsigned long xbox_DispatchGateOwner(void)
{
    return (unsigned long)InterlockedCompareExchange(&s_gate_owner, 0, 0);
}

/* For device models delivering an ISR, and KeSynchronizeExecution. On the
 * console an ISR runs with the one CPU to itself: the code it interrupted is
 * frozen. DirectSound counts on that: its DPC raises a counter around each
 * voice callback and the ISR leaves the voice lists alone while it is set,
 * a check-then-act that is only safe when the two never run at once. So an
 * ISR is delivered holding the gate when it is free, and posted to the
 * holder when it is not (xbox_IrqPost). With the gate off these succeed
 * and take nothing, so their callers run as they did before it. */
int xbox_DispatchGateTryEnter(void)
{
    if (!xbox_DispatchGateOn())
        return 1;
    return gate_try();
}

void xbox_DispatchGateEnter(void)
{
    if (xbox_DispatchGateOn())
        gate_enter();
}

static void irq_release_point(void);

/* The release is a safe point: what was posted while this thread held the
 * gate runs here, before anyone else can take it. */
void xbox_DispatchGateLeave(void)
{
    if (!xbox_DispatchGateOn())
        return;
    xbox_IrqSafePoint();
    gate_leave_raw();
    if (t_gate_depth == 0)
        irq_release_point();
}

/* ============================================================================
 * Device interrupts on the gate holder
 *
 * On the console an interrupt runs on the one CPU between two instructions
 * of whatever it interrupted, and that code is frozen until it returns. The
 * device models here run on their own host threads, so an interrupt that
 * found the gate held used to be held off for a while (the APU 50 frames,
 * OHCI 500 polls) and then run anyway, beside the holder; the vblank never
 * waited at all. DSOUND's interrupt appends a voice to the list its DPC is
 * draining, with no linked check: run beside the DPC it inserted one node
 * twice, the node pointed at itself, and the DPC looped on it forever (a
 * results screen that never came back).
 *
 * So a busy gate makes the model post the line instead, and the holder runs
 * the routine on its own thread at its next safe point: a kernel call, or
 * the gate's release before it is let go. Those are where guest code
 * already calls into the host, outside any guest read-modify-write. A
 * serviced MMIO trap is not one: it runs inside a signal handler (POSIX) or
 * a vectored handler (Windows). A guest loop at DISPATCH that polls memory
 * for the interrupt's effect without a kernel call never reaches one; the
 * wait log below names its holder.
 * ============================================================================ */

/* A bit per XBOX_IRQ_* line. */
static volatile LONG s_irq_pending;
static void (*volatile s_irq_run[XBOX_IRQ_LINES])(void);
static uint64_t s_irq_post_ns[XBOX_IRQ_LINES]; /* when the bit went up */
static XBOX_THREAD_LOCAL int t_in_isr;
static void (*s_irq_post_hook)(int line);
static void (*s_irq_release_hook)(void);   /* tests: the lower's window */
static int s_irq_mode;          /* RECOMP_IRQ_SAFE_POINTS, under the gate */
static int s_irq_force;
static int s_dpc_on;            /* RECOMP_DPC_ON_RAISE, under the gate */
/* A thread that runs guest code (guest-main, a spawned worker): the only
 * kind that drains queued DPCs at its raise and release (dpc_run_here). */
static XBOX_THREAD_LOCAL int t_guest_thread;
static XBOX_THREAD_LOCAL int t_in_dpc;
static uint64_t s_irq_wait_ns;
static const char *const s_irq_names[XBOX_IRQ_LINES] = { "vblank", "apu", "ohci" };

static struct {
    volatile LONG posted[XBOX_IRQ_LINES], on_holder[XBOX_IRQ_LINES],
                  second_try[XBOX_IRQ_LINES], forced[XBOX_IRQ_LINES],
                  ungated[XBOX_IRQ_LINES],
                  max_wait_us[XBOX_IRQ_LINES], wait_logs, refused;
} s_irq;
/* Safe points the holder reached with something pending but could not run
 * it at (inside an ISR, above DISPATCH_LEVEL, no worker stack): the "kernel
 * calls" in the wait log. */
static volatile LONG s_irq_points_refused;
static uint64_t s_irq_last_log_ns;

/* RECOMP_DISPATCH_GATE turns the gate on, and with it the two things that
 * need a holder: interrupts posted to it and run at its safe points
 * (RECOMP_IRQ_SAFE_POINTS=0 turns that off again), and queued DPCs drained
 * on the guest thread at its raise and release (RECOMP_DPC_ON_RAISE=0 leaves
 * them to the timer thread). Without the gate both are off whatever they
 * say: a post has nobody to run it, and a drain at a raise is only atomic
 * against the timer thread's because the gate orders the two. */
static void irq_config(void)
{
    const char *v;

    v = getenv("RECOMP_DISPATCH_GATE");
    s_gate_on = (v && *v) ? atoi(v) != 0 : 0;
    v = getenv("RECOMP_IRQ_SAFE_POINTS");
    s_irq_mode = s_gate_on && ((v && *v) ? atoi(v) != 0 : 1);
    v = getenv("RECOMP_IRQ_SAFE_FORCE");
    s_irq_force = (v && *v && *v != '0');
    v = getenv("RECOMP_IRQ_SAFE_MS");
    s_irq_wait_ns = (uint64_t)((v && *v) ? strtoul(v, NULL, 0) : 250u)
                    * 1000000ull;
    v = getenv("RECOMP_DPC_ON_RAISE");
    s_dpc_on = s_gate_on && ((v && *v) ? atoi(v) != 0 : 1);
    if (s_gate_on) {
        fprintf(stderr, "  [IRQ] one-CPU DISPATCH gate on (RECOMP_DISPATCH_GATE),"
                " DPCs on raise %s\n", s_dpc_on ? "on" : "off");
        fflush(stderr);
    }
    if (s_gate_on && (!s_irq_mode || s_irq_force)) {
        fprintf(stderr, "  [IRQ] safe points %s%s\n",
                s_irq_mode ? "on" : "off (RECOMP_IRQ_SAFE_POINTS=0: interrupts "
                                    "run beside the gate holder after a "
                                    "hold-off)",
                s_irq_force ? ", RECOMP_IRQ_SAFE_FORCE=1: delivered beside the "
                              "holder past RECOMP_IRQ_SAFE_MS" : "");
        fflush(stderr);
    }
}

/* Read once, by whichever thread asks first: the device models' threads,
 * the timer thread and the title's ask at once at boot, and a plain lazy
 * init let one of them see the gate on with safe points still unset. The
 * INIT_ONCE completes with a release, and every check below goes through
 * it (one acquire load once done). */
static INIT_ONCE s_irq_config_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK irq_config_run(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    irq_config();
    return TRUE;
}

static void irq_config_once(void)
{
    InitOnceExecuteOnce(&s_irq_config_once, irq_config_run, NULL, NULL);
}

/* Tests only, single-threaded: the environment changed under them. */
void xbox_IrqReloadConfig(void)
{
    irq_config_once();
    irq_config();
}

int xbox_IrqSafePointsOn(void)
{
    irq_config_once();
    return s_irq_mode;
}

int xbox_DpcOnRaise(void)
{
    irq_config_once();
    return s_dpc_on;
}

void xbox_DpcMarkGuestThread(void)
{
    t_guest_thread = 1;
}

void xbox_IrqSetHandler(int line, void (*run)(void))
{
    if (line >= 0 && line < XBOX_IRQ_LINES)
        s_irq_run[line] = run;
}

void xbox_IrqSetPostHook(void (*hook)(int line))
{
    s_irq_post_hook = hook;
}

void xbox_IrqSetReleaseHook(void (*hook)(void))
{
    s_irq_release_hook = hook;
}

static void irq_note_wait(int line, uint64_t posted_ns)
{
    long us = (long)((xbox_HostNowNs() - posted_ns) / 1000u);
    LONG cur;

    while (us > (cur = s_irq.max_wait_us[line]))
        if (InterlockedCompareExchange(&s_irq.max_wait_us[line], us, cur) == cur)
            break;
}

/* The holder's recompiled register state. A safe point at a thunk sits
 * before the bridge reads its arguments, so everything the generated code
 * keeps in these across a call is put back. g_fs_base stays: the
 * routine runs on the holder's TIB, as on the console's one CPU (a device
 * model's thread as holder: see irq_release_point). */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp;
extern RECOMP_TLS uint32_t g_seh_ebp;
extern RECOMP_TLS int g_df, g_fp_top, g_fp_cmp;
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS uint16_t g_fp_control_word, g_fp_cc;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
extern RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;

typedef struct {
    uint32_t eax, ecx, edx, ebx, esi, edi, ebp, esp, seh_ebp;
    int df, fp_top, fp_cmp;
    double fp[8];
    uint16_t fp_cw, fp_cc;
    RecompXmm xmm[8];
    RecompMmx mm[8];
} IrqRegs;

static void irq_regs_save(IrqRegs *r)
{
    r->eax = g_eax; r->ecx = g_ecx; r->edx = g_edx; r->ebx = g_ebx;
    r->esi = g_esi; r->edi = g_edi; r->ebp = g_ebp; r->esp = g_esp;
    r->seh_ebp = g_seh_ebp; r->df = g_df;
    r->fp_top = g_fp_top; r->fp_cmp = g_fp_cmp;
    memcpy(r->fp, g_fp_stack, sizeof r->fp);
    r->fp_cw = g_fp_control_word; r->fp_cc = g_fp_cc;
    r->xmm[0] = g_xmm0; r->xmm[1] = g_xmm1; r->xmm[2] = g_xmm2; r->xmm[3] = g_xmm3;
    r->xmm[4] = g_xmm4; r->xmm[5] = g_xmm5; r->xmm[6] = g_xmm6; r->xmm[7] = g_xmm7;
    r->mm[0] = g_mm0; r->mm[1] = g_mm1; r->mm[2] = g_mm2; r->mm[3] = g_mm3;
    r->mm[4] = g_mm4; r->mm[5] = g_mm5; r->mm[6] = g_mm6; r->mm[7] = g_mm7;
}

static void irq_regs_restore(const IrqRegs *r)
{
    g_eax = r->eax; g_ecx = r->ecx; g_edx = r->edx; g_ebx = r->ebx;
    g_esi = r->esi; g_edi = r->edi; g_ebp = r->ebp; g_esp = r->esp;
    g_seh_ebp = r->seh_ebp; g_df = r->df;
    g_fp_top = r->fp_top; g_fp_cmp = r->fp_cmp;
    memcpy(g_fp_stack, r->fp, sizeof r->fp);
    g_fp_control_word = r->fp_cw; g_fp_cc = r->fp_cc;
    g_xmm0 = r->xmm[0]; g_xmm1 = r->xmm[1]; g_xmm2 = r->xmm[2]; g_xmm3 = r->xmm[3];
    g_xmm4 = r->xmm[4]; g_xmm5 = r->xmm[5]; g_xmm6 = r->xmm[6]; g_xmm7 = r->xmm[7];
    g_mm0 = r->mm[0]; g_mm1 = r->mm[1]; g_mm2 = r->mm[2]; g_mm3 = r->mm[3];
    g_mm4 = r->mm[4]; g_mm5 = r->mm[5]; g_mm6 = r->mm[6]; g_mm7 = r->mm[7];
}

/* Queued DPCs, run on the guest thread that owns "the processor at
 * DISPATCH". On the console a DPC queued by an ISR runs when the processor
 * drops below DISPATCH, before the interrupted thread resumes. DirectSound
 * counts on that: its ISR appends an idle voice to the list its DPC drains
 * with no linked check, since the title cannot Play that voice again before
 * the DPC has run. Here the queue used to wait for the timer thread's next
 * pass (up to 10 ms, longer while a title thread was at DISPATCH); the title
 * re-Played the voice in that window, the APU re-trapped the same handle,
 * the second insert left the node pointing at itself and the DPC looped on
 * it forever (a title hung on a level transition, reproducibly). So a
 * guest thread drains at its raise to DISPATCH, before its
 * critical region, and at its release point. Registers come back as after
 * a posted routine: the raise's bridge has read its argument and writes its
 * result afterwards. Never inside a posted ISR (the exit path drains) or
 * inside another drain (a routine's own raises); never on a device-model
 * thread, whose DSOUND DPC could wait on that very thread (a trapped
 * front-end method cleared by the APU frame thread). */
static void dpc_run_here(void)
{
    IrqRegs saved;

    if (!xbox_DpcPending() || !t_guest_thread || t_in_dpc || t_in_isr)
        return;
    if (!xbox_DpcOnRaise())
        return;
    irq_regs_save(&saved);
    t_in_dpc = 1;
    /* The routines run without the guest CPU (RECOMP_GUEST_LOCK), as on the
     * timer thread: a DPC never takes it. This drain runs under the gate,
     * and taking the CPU here could wait on a holder polling memory for
     * this very DPC's result, while the timer thread's own drain waits for
     * the gate: a cycle. The one-CPU gate is
     * what a DPC's body is atomic against; passive code on another thread
     * may see its intermediate state, as it may on the timer thread. */
    xbox_DpcDrainHere();
    t_in_dpc = 0;
    irq_regs_restore(&saved);
}

/* Run one posted line here, on the holder. 0 when no worker stack was free
 * (the caller puts the bit back). */
static int irq_run_line(int line)
{
    void (*run)(void) = s_irq_run[line];
    IrqRegs saved;
    int slot, irql;

    if (!run)
        return 1;                       /* nothing to run it with: dropped */
    slot = xbox_worker_stack_alloc();
    if (slot < 0) {
        static volatile LONG said;
        if (!InterlockedExchange(&said, 1)) {
            fprintf(stderr, "  [IRQ] no worker stack for the %s interrupt on "
                    "the holder; it stays pending\n", s_irq_names[line]);
            fflush(stderr);
        }
        return 0;
    }
    irq_note_wait(line, s_irq_post_ns[line]);
    irq_regs_save(&saved);
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;
    t_in_isr = 1;
    irql = xbox_IrqlEnterInterrupt(16);
    run();
    xbox_IrqlLeaveInterrupt(irql);
    t_in_isr = 0;
    irq_regs_restore(&saved);
    xbox_worker_stack_free(slot);
    InterlockedIncrement(&s_irq.on_holder[line]);
    return 1;
}

static void irq_run_pending(void)
{
    LONG p;

    /* What is posted while a routine runs is run when it returns, here. */
    while ((p = InterlockedExchange(&s_irq_pending, 0)) != 0) {
        int line;
        for (line = 0; line < XBOX_IRQ_LINES; line++) {
            if (!(p & (1L << line)))
                continue;
            p &= ~(1L << line);
            if (!irq_run_line(line)) {
                InterlockedOr(&s_irq_pending, p | (1L << line));
                InterlockedIncrement(&s_irq_points_refused);
                return;
            }
        }
    }
}

/* One relaxed load when nothing is pending, which is nearly always. Runs
 * only on the gate holder, below the device level and outside a routine it
 * is already running: a CPU at device IRQL takes no interrupt, and a
 * routine's own kernel calls do not nest another. */
void xbox_IrqSafePoint(void)
{
    if (!s_irq_pending)
        return;
    if (!t_gate_depth)
        return;
    if (t_in_isr || g_current_irql > DISPATCH_LEVEL) {
        InterlockedIncrement(&s_irq_points_refused);
        return;
    }
    irq_run_pending();
}

/* After a full release: a post that landed between the release's safe point
 * and the leave saw the gate held, and its poster's second try may have come
 * before the leave too. Take the gate back and run it. Once, so a line that
 * cannot run (no worker stack) does not spin here.
 *
 * The releasing thread can be a device model's (the APU frame thread or the
 * OHCI controller thread, after a gated delivery of their own), and then
 * the routine runs on that thread's TIB. That is no different from the
 * models' own deliveries, which have always run ISRs on a TIB of the
 * delivering thread (xbox_AllocThreadTib), and from the vblank on the timer
 * thread: an ISR here never had a guest-scheduled TIB, and no XDK ISR reads
 * thread-local state. The holder rule is what matters, and this thread holds
 * the gate while the routine runs. */
static void irq_release_point(void)
{
    /* A full-barrier read, not a plain one: the leave just before is a
     * release only, and on arm64 a plain load may be satisfied before the
     * unlock is seen. A poster that tried the gate in that gap and found it
     * held would then post a bit this check had already missed, and the
     * line would wait for the next holder's safe point. */
    LONG pending = InterlockedCompareExchange(&s_irq_pending, 0, 0);

    if ((!pending && !(t_guest_thread && xbox_DpcPending()))
        || t_in_isr || g_current_irql > DISPATCH_LEVEL)
        return;
    if (!gate_try())
        return;                         /* the new holder has it now */
    irq_run_pending();
    /* What those routines queued, and what was queued while this thread
     * held the gate: the ISR-exit path of a posted interrupt. */
    dpc_run_here();
    gate_leave_raw();
}

static void irql_release_due(void)
{
    t_release_due = 0;
    irq_release_point();
}

/* The holder has reached no safe point for RECOMP_IRQ_SAFE_MS with something
 * pending: name it, and again every second while it lasts. The interrupt
 * waits; on hardware it would preempt the holder, but here that is the
 * concurrent run this whole scheme exists to prevent. */
static void irq_wait_check(uint64_t now)
{
    LONG p = s_irq_pending;
    uint64_t oldest = 0;
    unsigned long tid;
    char names[64];
    size_t used = 0;
    int line, i;

    if (!p)
        return;
    names[0] = 0;
    for (line = 0; line < XBOX_IRQ_LINES; line++)
        if (p & (1L << line)) {
            if (!oldest || s_irq_post_ns[line] < oldest)
                oldest = s_irq_post_ns[line];
            used += (size_t)snprintf(names + used, sizeof names - used, "%s%s",
                                     used ? "," : "", s_irq_names[line]);
        }
    if (!oldest || now - oldest < s_irq_wait_ns)
        return;
    if (s_irq_last_log_ns >= oldest && now - s_irq_last_log_ns < 1000000000ull)
        return;
    s_irq_last_log_ns = now;
    InterlockedIncrement(&s_irq.wait_logs);
    tid = xbox_DispatchGateOwner();
    fprintf(stderr, "  [IRQ] wait: %s pending %.0f ms; gate holder tid %lu"
            " has reached no safe point (%ld refused)",
            names, (double)(now - oldest) / 1e6, tid,
            (long)s_irq_points_refused);
    for (i = 0; i < IRQL_HOLDERS; i++)
        if ((unsigned long)s_holders[i].tid == tid && tid)
            fprintf(stderr, ", raised from host %p, guest ret %08X",
                    s_holders[i].ra, s_holders[i].guest_ra);
    fprintf(stderr, "%s\n", s_irq_force ? "; RECOMP_IRQ_SAFE_FORCE delivers it"
                                        : "; a guest loop here makes no "
                                          "kernel call");
    fflush(stderr);
}

/* A device model's interrupt. With the gate free the caller delivers it
 * holding the gate, as before. Held: the line is posted for the holder, and
 * the gate tried once more, which closes the window where the holder
 * released between the first try and the post with no safe point left to
 * see the bit. A set bit coalesces repeats, as a latched line does. */
int xbox_IrqPost(int line)
{
    LONG bit = 1L << line, was;
    uint64_t now;

    if (!xbox_DispatchGateOn())
        return XBOX_IRQ_UNGATED;        /* no gate, nobody to post to */
    if (gate_try())
        goto gated;
    if (!xbox_IrqSafePointsOn())
        return XBOX_IRQ_UNGATED;
    if (s_irq_post_hook)
        s_irq_post_hook(line);
    now = xbox_HostNowNs();
    was = InterlockedOr(&s_irq_pending, bit);
    if (!(was & bit)) {
        s_irq_post_ns[line] = now;
        InterlockedIncrement(&s_irq.posted[line]);
    }
    if (gate_try()) {
        if (InterlockedAnd(&s_irq_pending, ~bit) & bit) {
            InterlockedIncrement(&s_irq.second_try[line]);
            irq_note_wait(line, s_irq_post_ns[line]);
            return XBOX_IRQ_GATED;
        }
        xbox_DispatchGateLeave();       /* someone ran it in between */
        return XBOX_IRQ_POSTED;
    }
    irq_wait_check(now);
    if (s_irq_force && (was & bit) && now - s_irq_post_ns[line] >= s_irq_wait_ns
        && (InterlockedAnd(&s_irq_pending, ~bit) & bit)) {
        InterlockedIncrement(&s_irq.forced[line]);
        return XBOX_IRQ_UNGATED;
    }
    return XBOX_IRQ_POSTED;

gated:
    /* A post of this line still pending is this delivery. */
    if (xbox_IrqSafePointsOn() && (InterlockedAnd(&s_irq_pending, ~bit) & bit))
        irq_note_wait(line, s_irq_post_ns[line]);
    return XBOX_IRQ_GATED;
}

void xbox_IrqStatsGet(XboxIrqStats *out, int reset)
{
    int i;

#define IRQ_TAKE(f) (reset ? InterlockedExchange(&(f), 0) : (f))
    for (i = 0; i < XBOX_IRQ_LINES; i++) {
        out->posted[i] = IRQ_TAKE(s_irq.posted[i]);
        out->on_holder[i] = IRQ_TAKE(s_irq.on_holder[i]);
        out->second_try[i] = IRQ_TAKE(s_irq.second_try[i]);
        out->forced[i] = IRQ_TAKE(s_irq.forced[i]);
        out->ungated[i] = IRQ_TAKE(s_irq.ungated[i]);
        out->max_wait_us[i] = IRQ_TAKE(s_irq.max_wait_us[i]);
    }
    out->wait_logs = IRQ_TAKE(s_irq.wait_logs);
    out->refused = IRQ_TAKE(s_irq_points_refused);
#undef IRQ_TAKE
}

void xbox_IrqNoteUngated(int line)
{
    static volatile LONG said[XBOX_IRQ_LINES];
    unsigned long tid = xbox_DispatchGateOwner();

    if (!tid)
        return;                         /* nobody to run beside */
    InterlockedIncrement(&s_irq.ungated[line]);
    if (InterlockedIncrement(&said[line]) <= 20) {
        fprintf(stderr, "  [IRQ] %s ISR runs ungated beside gate holder"
                " tid %lu (from tid %lu)\n", s_irq_names[line], tid,
                (unsigned long)GetCurrentThreadId());
        fflush(stderr);
    }
}

void xbox_IrqReport(void)
{
    XboxIrqStats s;
    long any = 0;
    int i;

    xbox_IrqStatsGet(&s, 1);
    if (!xbox_IrqSafePointsOn()) {
        if (s.ungated[0] | s.ungated[1] | s.ungated[2]) {
            fprintf(stderr, "  [IRQ] ungated ISRs beside the gate holder"
                    " vbl/apu/ohci %ld/%ld/%ld\n",
                    s.ungated[0], s.ungated[1], s.ungated[2]);
            fflush(stderr);
        }
        return;
    }
    for (i = 0; i < XBOX_IRQ_LINES; i++)
        any |= s.posted[i] | s.on_holder[i] | s.forced[i] | s.ungated[i];
    if (!any && !s.wait_logs)
        return;
    fprintf(stderr, "  [IRQ] posted vbl/apu/ohci %ld/%ld/%ld, run on holder"
            " %ld/%ld/%ld, poster's retry %ld/%ld/%ld, forced %ld/%ld/%ld,"
            " ungated %ld/%ld/%ld,"
            " longest wait %.2f/%.2f/%.2f ms, wait logs %ld, refused %ld\n",
            s.posted[0], s.posted[1], s.posted[2],
            s.on_holder[0], s.on_holder[1], s.on_holder[2],
            s.second_try[0], s.second_try[1], s.second_try[2],
            s.forced[0], s.forced[1], s.forced[2],
            s.ungated[0], s.ungated[1], s.ungated[2],
            s.max_wait_us[0] / 1000.0, s.max_wait_us[1] / 1000.0,
            s.max_wait_us[2] / 1000.0, s.wait_logs, s.refused);
    fflush(stderr);
}

static void irql_track_gated(KIRQL old_level, KIRQL new_level, void *ra,
                             int gate)
{
    int now = (new_level >= DISPATCH_LEVEL);
    /* With the gate off the count moves on the edge, as it did before the
     * gate: the hold-off it drives is the one the device models had. */
    int was = xbox_DispatchGateOn() ? t_irql_counted
                                    : (old_level >= DISPATCH_LEVEL);
    LONG d;

    if (now == was)
        return;
    /* The lower that gives the gate back is a safe point, taken while this
     * thread still holds it and is still at its level. */
    if (!now && t_dispatch_gate_held)
        xbox_IrqSafePoint();
    t_irql_counted = now;

    if (now && gate && xbox_DispatchGateOn()) {
        gate_enter();
        t_dispatch_gate_held = 1;
    }
    d = now ? InterlockedIncrement(&g_irql_raised_count)
            : InterlockedDecrement(&g_irql_raised_count);
    InterlockedIncrement(&s_transitions);
    if (now)
        irql_holder_add(ra);
    else
        irql_holder_drop();
    if (!now && t_dispatch_gate_held) {
        t_dispatch_gate_held = 0;
        if (s_irq_release_hook)
            s_irq_release_hook();
        /* A post caught between the safe point above and this leave saw the
         * gate held twice. The release point that takes it runs only once
         * the caller has written the new level back (irql_publish): run
         * here, the routine's own raise to 16 would be counted against a
         * level that is still the old one. Without it the vblank waited for
         * its next tick, up to 16.7 ms. */
        gate_leave_raw();
        if (!t_gate_depth)
            t_release_due = 1;
    }

    /* RECOMP_IRQL_TRACE prints the first few transitions. The pairing is what
     * matters: a raise to 2 followed by a lower from 2 nets out, and a lower
     * whose old level is not the level the raise set is a calling-convention
     * bug upstream of here, not a title doing something exotic. */
    if (s_trace < 0)
        s_trace = getenv("RECOMP_IRQL_TRACE") ? 1 : 0;
    if (s_trace && InterlockedIncrement(&s_traced) <= 20) {
        fprintf(stderr, "  [IRQL] tid %lu %s %d->%d depth=%ld\n",
                (unsigned long)GetCurrentThreadId(),
                now ? "raise" : "lower", old_level, new_level, (long)d);
        fflush(stderr);
    }
}

static void irql_track(KIRQL old_level, KIRQL new_level, void *ra)
{
    irql_track_gated(old_level, new_level, ra, 1);
}

/* Bracket a host-delivered ISR or DPC.
 *
 * The kernel's interrupt and DPC dispatchers put the processor back at the
 * interrupted IRQL when the routine returns, whatever the routine left it at.
 * The host threads that deliver interrupts and drain DPCs here had no such
 * epilogue, so a routine that returned at DISPATCH_LEVEL left its thread
 * there and the global depth at one -- and every later USB interrupt then
 * sat out the forced-delivery timeout, a pad polled twice a second.
 *
 * They also run the routine at the level it expects -- DISPATCH_LEVEL for a
 * DPC, the device level for an ISR -- and code checks: see irql_publish. */
int xbox_IrqlEnterInterrupt(int level)
{
    int saved = (int)g_current_irql;
    if (g_current_irql != (KIRQL)level) {
        /* A DPC (DISPATCH_LEVEL) goes through the gate; a device ISR does
         * not (see s_dispatch_gate). */
        irql_track_gated(g_current_irql, (KIRQL)level, IRQL_CALLER(),
                         level <= DISPATCH_LEVEL);
        g_current_irql = (KIRQL)level;
        irql_publish();
    }
    return saved;
}

void xbox_IrqlLeaveInterrupt(int saved)
{
    if (g_current_irql != (KIRQL)saved) {
        irql_track(g_current_irql, (KIRQL)saved, IRQL_CALLER());
        g_current_irql = (KIRQL)saved;
        irql_publish();
    }
}

/* Whether this thread is at DISPATCH_LEVEL or above, or holds the one-CPU
 * gate: while it is, no DPC can run, so a wait on this thread must not hand
 * the guest CPU on in the hope of one (kernel_guest_cpu.c). */
int xbox_IrqlThisThreadBlocksDpcs(void)
{
    return t_irql_counted || t_dispatch_gate_held
           || g_current_irql >= DISPATCH_LEVEL;
}

int xbox_IrqlCurrent(void)
{
    return (int)g_current_irql;
}

/*
 * KfRaiseIrql - Raises IRQL to the specified level.
 * Returns the previous IRQL. Uses __fastcall (ECX = NewIrql).
 */
KIRQL __fastcall xbox_KfRaiseIrql(KIRQL NewIrql)
{
    KIRQL old = g_current_irql;

    if (NewIrql < old) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL,
            "KfRaiseIrql: attempt to lower IRQL from %d to %d (use KfLowerIrql)",
            old, NewIrql);
    }

    irql_track(old, NewIrql, IRQL_CALLER());
    g_current_irql = NewIrql;
    irql_publish();
    /* This thread now owns the processor at DISPATCH or above (any raise
     * that crosses DISPATCH from below, to a device IRQL too): what an ISR
     * queued runs before the caller's critical region, as it would have run
     * before the caller resumed on the console. */
    if (old < DISPATCH_LEVEL && NewIrql >= DISPATCH_LEVEL)
        dpc_run_here();
    return old;
}

/*
 * KfLowerIrql - Lowers IRQL to the specified level.
 * Uses __fastcall (ECX = NewIrql).
 */
VOID __fastcall xbox_KfLowerIrql(KIRQL NewIrql)
{
    KIRQL old;

    if (NewIrql > g_current_irql) {
        /* A lower that would raise. The title's own pairs never do this;
         * one that does is restoring a level it read from somewhere other
         * than its matching raise. With the gate on, take the level it asks
         * for but never count it: a raise counted here has no lower to undo
         * it, so it would hold the gate, and the count every device model
         * reads, for the rest of the run. A thread already counted stays
         * counted until it really lowers. With the gate off it is counted
         * as a raise, as before the gate, and the depth stays one too high:
         * loud, because the symptom is a device going quiet much later. */
        static volatile LONG n;
        int gated = xbox_DispatchGateOn();
        if (InterlockedIncrement(&n) <= 20) {
            fprintf(stderr, "  [IRQLBUG] tid %lu KfLowerIrql(%d) while at %d"
                    " -- %s\n",
                    (unsigned long)GetCurrentThreadId(),
                    (int)NewIrql, (int)g_current_irql,
                    gated ? "level set, not counted as a raise"
                          : "counted as a raise, depth is now stuck");
            fflush(stderr);
        }
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL,
            "KfLowerIrql: attempt to raise IRQL from %d to %d (use KfRaiseIrql)",
            g_current_irql, NewIrql);
        if (gated) {
            g_current_irql = NewIrql;
            irql_publish();
            return;
        }
    }

    old = g_current_irql;
    irql_track(old, NewIrql, IRQL_CALLER());
    g_current_irql = NewIrql;
    irql_publish();
    /* Down from a device level to DISPATCH: the thread keeps the gate, and
     * the interrupts the level held off are taken now, as the console takes
     * them at the lower. The thunk's safe point came before this, still at
     * the old level, and the guest may go on at DISPATCH with no kernel call.
     * A lower below DISPATCH takes them at its release point instead. */
    if (old > DISPATCH_LEVEL && NewIrql == DISPATCH_LEVEL)
        xbox_IrqSafePoint();
}

/*
 * KeRaiseIrqlToDpcLevel - Convenience function to raise to DISPATCH_LEVEL.
 */
KIRQL __stdcall xbox_KeRaiseIrqlToDpcLevel(void)
{
    KIRQL old = g_current_irql;

    irql_track(old, DISPATCH_LEVEL, IRQL_CALLER());
    g_current_irql = DISPATCH_LEVEL;
    irql_publish();
    if (old < DISPATCH_LEVEL)
        dpc_run_here();
    return old;
}

/* ============================================================================
 * KeTickCount
 *
 * Exported as a data pointer, not a function. The Xbox kernel increments
 * this every ~1ms (approximating the Xbox tick interval).
 * Updated lazily when read, using GetTickCount.
 * ============================================================================ */

volatile ULONG xbox_KeTickCount = 0;

/* Call this periodically or on-demand to update KeTickCount */
static void xbox_update_tick_count(void)
{
    xbox_KeTickCount = GetTickCount();
}

/* ============================================================================
 * Performance Counters
 *
 * Direct 1:1 mapping to Win32 QueryPerformanceCounter/Frequency.
 * Both Xbox and Windows return LARGE_INTEGER.
 * ============================================================================ */

LARGE_INTEGER __stdcall xbox_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter;
}

LARGE_INTEGER __stdcall xbox_KeQueryPerformanceFrequency(void)
{
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return freq;
}

/* ============================================================================
 * System Time
 *
 * KeQuerySystemTime returns the current time as a FILETIME (100ns since
 * January 1, 1601). Direct Win32 mapping.
 * ============================================================================ */

static LONGLONG s_time_anchor_100ns;
static LONGLONG s_time_anchor_counts;
static LONGLONG s_time_freq_counts;
static INIT_ONCE s_time_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK anchor_system_time(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    FILETIME ft;
    LARGE_INTEGER f, now;
    (void)once; (void)param; (void)ctx;
    QueryPerformanceFrequency(&f);
    GetSystemTimeAsFileTime(&ft);
    QueryPerformanceCounter(&now);
    s_time_freq_counts = f.QuadPart ? f.QuadPart : 1;
    s_time_anchor_100ns = ((LONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    s_time_anchor_counts = now.QuadPart;
    return TRUE;
}

VOID __stdcall xbox_KeQuerySystemTime(PLARGE_INTEGER CurrentTime)
{
    /* Anchored once to the wall clock, advanced by the performance counter.
     *
     * GetSystemTimeAsFileTime alone moves in steps of about 15.6 ms, the
     * host's scheduler tick. The console's clock is far finer, and a title
     * that busy-waits on this -- reading it until enough time has passed --
     * spins for the whole of each step instead of a few iterations.
     *
     * Measured on Shin Megami Tensei: Nine: one such wait called this
     * **11.8 million times in two seconds**, which is most of what the
     * title was doing at that moment, and it came out of the spin in a
     * state where it no longer polled the gamepad.
     *
     * The anchor keeps the absolute value right; the counter supplies the
     * resolution between ticks.
     *
     * Whole seconds and the remainder are scaled separately: scaling the
     * whole count by 10^7 overflows after about a day at 10 MHz. */
    LARGE_INTEGER now;
    LONGLONG delta;

    if (!CurrentTime)
        return;

    InitOnceExecuteOnce(&s_time_once, anchor_system_time, NULL, NULL);
    QueryPerformanceCounter(&now);
    delta = now.QuadPart - s_time_anchor_counts;
    CurrentTime->QuadPart = s_time_anchor_100ns
        + (delta / s_time_freq_counts) * 10000000LL
        + (delta % s_time_freq_counts) * 10000000LL / s_time_freq_counts;
}

/* ============================================================================
 * Processor Stall
 *
 * KeStallExecutionProcessor performs a busy-wait for the given number
 * of microseconds. Used for hardware timing (e.g., waiting for GPU).
 * ============================================================================ */

VOID __stdcall xbox_KeStallExecutionProcessor(ULONG MicroSeconds)
{
    LARGE_INTEGER freq, start, now;

    if (MicroSeconds == 0)
        return;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    LONGLONG target_counts = (freq.QuadPart * MicroSeconds) / 1000000;

    do {
        QueryPerformanceCounter(&now);
    } while ((now.QuadPart - start.QuadPart) < target_counts);
}

/* ============================================================================
 * Floating Point State
 *
 * Xbox kernel requires saving/restoring FP state when kernel code uses
 * floating point. On Windows user-mode this is handled automatically by
 * the OS, so these are no-ops.
 * ============================================================================ */

NTSTATUS __stdcall xbox_KeSaveFloatingPointState(PVOID FloatingPointState)
{
    (void)FloatingPointState;
    /* No-op: Windows user-mode preserves FP state across context switches */
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_KeRestoreFloatingPointState(PVOID FloatingPointState)
{
    (void)FloatingPointState;
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Bug Check (Blue Screen of Death)
 *
 * KeBugCheck/KeBugCheckEx are the Xbox equivalent of BSOD. In our
 * recompilation, we log the error and terminate the process.
 * ============================================================================ */

VOID __stdcall xbox_KeBugCheck(ULONG BugCheckCode)
{
    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_HAL,
        "*** KeBugCheck: code=0x%08X ***", BugCheckCode);
    fprintf(stderr, "[EXIT] KeBugCheck 0x%08X\n", (unsigned)BugCheckCode);

#ifdef _DEBUG
    DebugBreak();
#endif

    /* ExitProcess skips atexit. Guest code calls this, never from inside
     * the report's lock, so the summary's list write may take it. */
    xbox_missing_summary("bugcheck");
    ExitProcess(BugCheckCode);
}

VOID __stdcall xbox_KeBugCheckEx(
    ULONG BugCheckCode,
    ULONG_PTR Param1,
    ULONG_PTR Param2,
    ULONG_PTR Param3,
    ULONG_PTR Param4)
{
    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_HAL,
        "*** KeBugCheckEx: code=0x%08X, params=(0x%p, 0x%p, 0x%p, 0x%p) ***",
        BugCheckCode, (void*)Param1, (void*)Param2, (void*)Param3, (void*)Param4);
    fprintf(stderr, "[EXIT] KeBugCheckEx 0x%08X\n", (unsigned)BugCheckCode);

#ifdef _DEBUG
    DebugBreak();
#endif

    /* ExitProcess skips atexit. Guest code calls this, never from inside
     * the report's lock, so the summary's list write may take it. */
    xbox_missing_summary("bugcheck");
    ExitProcess(BugCheckCode);
}

/* ============================================================================
 * HAL PCI Access
 *
 * HalReadWritePCISpace reads/writes PCI configuration space. The Xbox uses
 * this for GPU and southbridge setup. Not needed on Windows - stub it.
 * ============================================================================ */

VOID __stdcall xbox_HalReadWritePCISpace(
    ULONG BusNumber,
    ULONG SlotNumber,
    ULONG RegisterNumber,
    PVOID Buffer,
    ULONG Length,
    BOOLEAN WritePCISpace)
{
    (void)BusNumber;
    (void)SlotNumber;
    (void)RegisterNumber;
    (void)Length;
    (void)WritePCISpace;

    /* Return zeroed buffer for reads */
    if (!WritePCISpace && Buffer)
        memset(Buffer, 0, Length);

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_HAL,
        "HalReadWritePCISpace: bus=%u slot=%u reg=0x%X len=%u %s (stubbed)",
        BusNumber, SlotNumber, RegisterNumber, Length,
        WritePCISpace ? "WRITE" : "READ");
}

/* ============================================================================
 * HAL Firmware & Shutdown
 *
 * HalReturnToFirmware returns to the Xbox dashboard. For us, this means
 * exit the game cleanly.
 * ============================================================================ */

VOID __stdcall xbox_HalReturnToFirmware(ULONG Routine)
{
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_HAL,
        "HalReturnToFirmware: routine=%u (exiting)", Routine);
    /* stderr too: the kernel log file is rewritten on every run, so a title
     * that ends itself otherwise leaves no trace in the run's own log. */
    fprintf(stderr, "[EXIT] HalReturnToFirmware routine=%u\n", (unsigned)Routine);
    ExitProcess(0);
}

VOID __stdcall xbox_HalInitiateShutdown(void)
{
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_HAL, "HalInitiateShutdown (exiting)");
    fprintf(stderr, "[EXIT] HalInitiateShutdown\n");
    ExitProcess(0);
}

BOOLEAN __stdcall xbox_HalIsResetOrShutdownPending(void)
{
    return FALSE;
}

/* ============================================================================
 * SMC (System Management Controller)
 *
 * HalReadSMCTrayState reads the DVD tray state. No disc tray on PC.
 * ============================================================================ */

ULONG __stdcall xbox_HalReadSMCTrayState(PULONG TrayState, PULONG TrayStateChangeCount)
{
    /* Tray state: 0x10 = media detected (disc present) */
    if (TrayState)
        *TrayState = 0x10;
    if (TrayStateChangeCount)
        *TrayStateChangeCount = 0;
    return 0; /* Success */
}

/* ============================================================================
 * Software Interrupts
 *
 * Used for APC/DPC delivery on Xbox. Stubbed since we don't have real
 * interrupt-driven DPC delivery.
 * ============================================================================ */

VOID __stdcall xbox_HalClearSoftwareInterrupt(KIRQL RequestIrql)
{
    (void)RequestIrql;
}

VOID __stdcall xbox_HalRequestSoftwareInterrupt(KIRQL RequestIrql)
{
    (void)RequestIrql;
}

VOID __stdcall xbox_HalDisableSystemInterrupt(ULONG BusInterruptLevel, KIRQL Irql)
{
    (void)BusInterruptLevel;
    (void)Irql;
}

ULONG __stdcall xbox_HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql)
{
    (void)BusInterruptLevel;
    if (Irql)
        *Irql = PASSIVE_LEVEL;
    return 0;
}

/* ============================================================================
 * Interrupt Objects
 *
 * Used by DSOUND and other drivers for hardware interrupt handling.
 * Since we replace the audio/graphics subsystems entirely, these are stubs.
 * ============================================================================ */

VOID __stdcall xbox_KeInitializeInterrupt(
    PXBOX_KINTERRUPT Interrupt,
    PVOID ServiceRoutine,
    PVOID ServiceContext,
    ULONG Vector,
    KIRQL Irql,
    ULONG InterruptMode,
    BOOLEAN ShareVector)
{
    (void)Vector;
    (void)InterruptMode;
    (void)ShareVector;

    if (!Interrupt)
        return;

    Interrupt->ServiceRoutine = ServiceRoutine;
    Interrupt->ServiceContext = ServiceContext;
    Interrupt->Irql = Irql;
    Interrupt->Connected = FALSE;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
        "KeInitializeInterrupt: interrupt=%p, routine=%p, vector=%u",
        Interrupt, ServiceRoutine, Vector);
}

BOOLEAN __stdcall xbox_KeConnectInterrupt(PXBOX_KINTERRUPT Interrupt)
{
    if (!Interrupt)
        return FALSE;

    Interrupt->Connected = TRUE;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
        "KeConnectInterrupt: interrupt=%p (stubbed - no real HW interrupts)",
        Interrupt);

    return TRUE;
}

BOOLEAN __stdcall xbox_KeDisconnectInterrupt(PXBOX_KINTERRUPT Interrupt)
{
    BOOLEAN was_connected;

    if (!Interrupt)
        return FALSE;

    /* Returns the PREVIOUS connected state, not success. */
    was_connected = Interrupt->Connected;
    Interrupt->Connected = FALSE;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
        "KeDisconnectInterrupt: interrupt=%p was_connected=%d",
        Interrupt, (int)was_connected);

    return was_connected;
}

/* ============================================================================
 * Miscellaneous Port I/O Stubs
 * ============================================================================ */

VOID __stdcall xbox_WRITE_PORT_BUFFER_ULONG(PULONG Port, PULONG Buffer, ULONG Count)
{
    (void)Port;
    (void)Buffer;
    (void)Count;
}

VOID __stdcall xbox_WRITE_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count)
{
    (void)Port;
    (void)Buffer;
    (void)Count;
}

/* ============================================================================
 * System Time (Set)
 *
 * NtSetSystemTime - we don't actually change the system clock, just log it.
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtSetSystemTime(PLARGE_INTEGER SystemTime, PLARGE_INTEGER PreviousTime)
{
    if (PreviousTime)
        GetSystemTimeAsFileTime((LPFILETIME)PreviousTime);

    xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL,
        "NtSetSystemTime: ignored (not setting system clock)");

    return STATUS_SUCCESS;
}

/* ============================================================================
 * Display / AV
 *
 * These are declared in kernel.h for the thunk table but will be fully
 * implemented by the D3D replacement layer. We provide realistic AV pack
 * detection so games can query display capabilities (480p, 720p, widescreen).
 * ============================================================================ */

static ULONG g_av_saved_data_address = 0;
static ULONG g_av_display_mode = 0;

ULONG __stdcall xbox_AvGetSavedDataAddress(void)
{
    return g_av_saved_data_address;
}

VOID __stdcall xbox_AvSendTVEncoderOption(
    PVOID RegisterBase, ULONG Option, ULONG Param, PULONG Result)
{
    (void)RegisterBase;
    (void)Param;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
        "AvSendTVEncoderOption: option=0x%02X param=0x%X", Option, Param);

    if (!Result)
        return;

    switch (Option) {
    case AV_OPTION_QUERY_AVPACK:
        /* Pack type, video standard and refresh rate, in one word.
         *
         * D3D keys its display-mode table on all three: the row flags carry
         * the pack in 0x000000FF, the standard in 0x0000FF00 and the refresh
         * in 0x00C00000 (Half-Life 2's 640x480 60Hz row is 0x00480104).
         * Returning the pack alone left the standard as 0, which matches no
         * row, so the mode scan ran off the end of the table and device
         * creation failed with E_FAIL.
         *
         * HDTV pack keeps 480p/720p available to titles that offer them;
         * NTSC-M and 60Hz are the North American retail default, and match
         * the region reported by ExQueryNonVolatileSetting. */
        *Result = AV_PACK_HDTV
                | (AV_STANDARD_NTSC_M << AV_STANDARD_SHIFT)
                | AV_REFRESH_60Hz;
        break;

    case AV_OPTION_QUERY_MODE:
        /* Return current display mode */
        *Result = g_av_display_mode;
        break;

    case AV_OPTION_QUERY_AV_CAPABILITIES:
        /* Report support for 480i, 480p, 720p, and widescreen */
        *Result = AV_FLAGS_HDTV_480i | AV_FLAGS_HDTV_480p
                | AV_FLAGS_HDTV_720p | AV_FLAGS_WIDESCREEN
                | AV_FLAGS_60Hz;
        break;

    case AV_OPTION_QUERY_ENCODER_TYPE:
        /* Conexant CX25871 (common in retail Xboxes) */
        *Result = 4;
        break;

    case AV_OPTION_QUERY_MODE_CAPS:
        /* Same as capabilities for our purposes */
        *Result = AV_FLAGS_HDTV_480i | AV_FLAGS_HDTV_480p
                | AV_FLAGS_HDTV_720p | AV_FLAGS_WIDESCREEN
                | AV_FLAGS_60Hz;
        break;

    case AV_OPTION_SET_MODE:
        g_av_display_mode = Param;
        *Result = 0;
        break;

    case AV_OPTION_BLANK_SCREEN:
    case AV_OPTION_MACROVISION_MODE:
    case AV_OPTION_FLICKER_FILTER:
    case AV_OPTION_ZERO_MODE:
        *Result = 0;
        break;

    default:
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL,
            "AvSendTVEncoderOption: unknown option 0x%02X", Option);
        *Result = 0;
        break;
    }
}

VOID __stdcall xbox_AvSetSavedDataAddress(ULONG Address)
{
    g_av_saved_data_address = Address;
}

VOID __stdcall xbox_AvSetDisplayMode(
    PVOID RegisterBase, ULONG Step, ULONG Mode,
    ULONG Format, ULONG Pitch, ULONG FrameBuffer)
{
    (void)RegisterBase;
    (void)Step;
    (void)Format;
    (void)Pitch;
    (void)FrameBuffer;

    g_av_display_mode = Mode;

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_HAL,
        "AvSetDisplayMode: step=%u mode=0x%X format=0x%X pitch=%u fb=0x%X",
        Step, Mode, Format, Pitch, FrameBuffer);
}

/* ============================================================================
 * SMBus - HalReadSMBusValue / HalWriteSMBusValue
 *
 * The Xbox SMBus connects the CPU to the System Management Controller (SMC),
 * EEPROM, temperature sensor, and TV encoder. Games use these to detect
 * AV pack type, read EEPROM settings, and check hardware state.
 *
 * We simulate responses for the most commonly queried devices:
 *   - SMC (0x20): firmware version, tray state, AV pack, temperatures
 *   - EEPROM (0xA8): handled separately via ExQueryNonVolatileSetting
 *   - Temperature sensor (0x98): CPU/board temperatures
 * ============================================================================ */

NTSTATUS __stdcall xbox_HalReadSMBusValue(
    UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN ReadWordValue, PULONG DataValue)
{
    if (!DataValue)
        return STATUS_INVALID_PARAMETER;

    *DataValue = 0;

    switch (SlaveAddress) {
    case SMC_SLAVE_ADDRESS:  /* 0x20 - System Management Controller */
        switch (CommandCode) {
        case SMC_CMD_FIRMWARE_VER:
            /* "P01" = production SMC, return 'P' for first byte.
             * Games read version byte-by-byte: P(0x50), 0(0x30), 1(0x31) */
            *DataValue = 0x50; /* 'P' */
            break;
        case SMC_CMD_TRAY_STATE:
            /* 0x60 = media present, tray closed */
            *DataValue = 0x60;
            break;
        case SMC_CMD_AV_PACK:
            /* HDTV/Component pack */
            *DataValue = AV_PACK_HDTV;
            break;
        case SMC_CMD_CPU_TEMP:
            *DataValue = 40; /* 40 degrees C */
            break;
        case SMC_CMD_MB_TEMP:
            *DataValue = 35; /* 35 degrees C */
            break;
        case SMC_CMD_FAN_SPEED:
            *DataValue = 50; /* ~50% fan speed */
            break;
        case SMC_CMD_INTERRUPT_REASON:
            *DataValue = 0;  /* No pending interrupt */
            break;
        case SMC_CMD_ERROR_CODE:
            *DataValue = 0;  /* No error */
            break;
        default:
            xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
                "HalReadSMBusValue: SMC unknown cmd=0x%02X", CommandCode);
            break;
        }
        break;

    case TEMP_SLAVE_ADDRESS:  /* 0x98 - ADM1032 temperature sensor */
        /* CommandCode 0x00 = local temp, 0x01 = remote temp */
        if (CommandCode == 0x00)
            *DataValue = 35;  /* Board: 35C */
        else if (CommandCode == 0x01)
            *DataValue = 40;  /* CPU: 40C */
        else
            *DataValue = 30;
        break;

    case ENCODER_SLAVE_ADDRESS:  /* 0xD4 - TV encoder */
        /* Return 0 for most encoder register reads */
        *DataValue = 0;
        break;

    default:
        xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
            "HalReadSMBusValue: unknown slave=0x%02X cmd=0x%02X",
            SlaveAddress, CommandCode);
        break;
    }

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_HAL,
        "HalReadSMBusValue: slave=0x%02X cmd=0x%02X word=%d -> 0x%X",
        SlaveAddress, CommandCode, ReadWordValue, *DataValue);

    (void)ReadWordValue;
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_HalWriteSMBusValue(
    UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN WriteWordValue, ULONG DataValue)
{
    (void)WriteWordValue;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_HAL,
        "HalWriteSMBusValue: slave=0x%02X cmd=0x%02X word=%d val=0x%X (ignored)",
        SlaveAddress, CommandCode, WriteWordValue, DataValue);

    /* Writes to SMC (LED control, fan speed, etc.) are silently accepted */
    return STATUS_SUCCESS;
}

/* ============================================================================
 * HAL Data Exports
 *
 * Ordinals 40, 41 and 42 are variables, not functions. Games read them
 * directly through the thunk table, so the thunk must hand back the address
 * of real storage -- pointing these at a function is what produced garbage
 * disk metadata before.
 *
 * The strings are counted (Length/MaximumLength), not NUL-terminated, matching
 * the kernel's STRING type. Values describe the virtual disk we present; no
 * real hardware is queried.
 * ============================================================================ */

ULONG xbox_HalDiskCachePartitionCount = 3;

static char g_disk_model[]  = "XBOXRECOMP VIRTUAL HDD";
static char g_disk_serial[] = "XR0000000000";

XBOX_ANSI_STRING xbox_HalDiskModelNumber = {
    sizeof(g_disk_model) - 1,
    sizeof(g_disk_model) - 1,
    g_disk_model
};

XBOX_ANSI_STRING xbox_HalDiskSerialNumber = {
    sizeof(g_disk_serial) - 1,
    sizeof(g_disk_serial) - 1,
    g_disk_serial
};

/*
 * Video mode the SMC reported at boot. 0 lets title code fall back to querying
 * the AV pack, which we answer properly in AvGetSavedDataAddress/SMBus.
 */
ULONG xbox_HalBootSMCVideoMode = 0;

/*
 * IDE channel object. Real kernels export a device object for the ATA channel;
 * drivers only ever pass it back to us, so identity is all that is required.
 */
static ULONG g_idex_channel_data = 0x49444558; /* 'IDEX' */
PVOID xbox_IdexChannelObject = &g_idex_channel_data;

/* ============================================================================
 * Shutdown Notification
 * ============================================================================ */

VOID __stdcall xbox_HalRegisterShutdownNotification(
    PVOID ShutdownRegistration,
    BOOLEAN Register)
{
    /*
     * Registers a callback to run on reboot/shutdown. We never initiate an
     * Xbox-style shutdown -- HalReturnToFirmware terminates the process -- so
     * the callback would never fire. Recorded in the log so a title relying on
     * shutdown cleanup is visible rather than silently ignored.
     */
    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
        "HalRegisterShutdownNotification: %s registration=%p (never invoked)",
        Register ? "register" : "unregister", ShutdownRegistration);
}

/* ============================================================================
 * Unknown Ordinal Stubs
 * ============================================================================ */

VOID __stdcall xbox_Unknown_8(void)
{
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL, "Unknown ordinal 8 called (stubbed)");
}

VOID __stdcall xbox_Unknown_23(void)
{
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL, "Unknown ordinal 23 called (stubbed)");
}

VOID __stdcall xbox_Unknown_42(void)
{
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL, "Unknown ordinal 42 called (stubbed)");
}

/* ============================================================================
 * Debug / Timing
 * ============================================================================ */

VOID __stdcall xbox_DbgBreakPoint(void)
{
    /*
     * Titles call this from assertion paths. Under a debugger this should
     * break; without one, raising a breakpoint exception would terminate the
     * process on a condition the title may well survive. Log loudly and
     * continue, and only actually break when a debugger is attached to catch it.
     */
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL, "DbgBreakPoint called by title");

    if (IsDebuggerPresent())
        DebugBreak();
}

ULONGLONG __stdcall xbox_KeQueryInterruptTime(void)
{
    /*
     * Time since boot in NT 100ns units. GetTickCount64 is milliseconds, so
     * scale by 10,000. Resolution is coarser than the real kernel's, but it is
     * monotonic, which is the property callers actually depend on.
     */
    return (ULONGLONG)GetTickCount64() * 10000ULL;
}

/* ============================================================================
 * Time stamp counter
 *
 * Xbox's QueryPerformanceCounter is a bare `rdtsc`, and its
 * QueryPerformanceFrequency returns the CPU clock as a constant the title
 * compiles in: Half-Life 2's is 0x2BB5C755 (733,333,333 Hz) at 0x0059C6C7.
 * So a frame timer computes seconds as counter / 733333333.
 *
 * Returning the host's own TSC would make that division wrong by the ratio of
 * the two clocks -- a 3.5 GHz host would have the guest believe nearly five
 * seconds had passed for every real one. Scaling the host's performance
 * counter to the console's rate keeps the guest's arithmetic honest.
 *
 * Monotonic and shared by every thread, which is what a TSC is. The first
 * call establishes the origin so the counter starts near zero rather than at
 * whatever the host had been running for.
 * ========================================================================= */
#define XBOX_TSC_HZ 733333333ull

uint64_t xbox_ReadTimeStampCounter(void)
{
    static LARGE_INTEGER freq;
    static LARGE_INTEGER origin;
    LARGE_INTEGER now;

    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&origin);
        if (freq.QuadPart == 0)
            freq.QuadPart = 1;
    }
    QueryPerformanceCounter(&now);

    {
        uint64_t ticks = (uint64_t)(now.QuadPart - origin.QuadPart);
        /* Split the scaling so a long run cannot overflow: whole seconds
         * first, then the remainder. */
        uint64_t secs = ticks / (uint64_t)freq.QuadPart;
        uint64_t rem  = ticks % (uint64_t)freq.QuadPart;
        return secs * XBOX_TSC_HZ
             + (rem * XBOX_TSC_HZ) / (uint64_t)freq.QuadPart;
    }
}

/* =========================================================================
 * x86 port I/O
 *
 * The lifter turns `in`/`out` into these. The MCPX decodes I/O space: its
 * ACPI/PM block sits at 0x8000 and SMBus at 0xC000, and the kernel exports
 * cover SMBus, so a title touches ports directly only for the odd pin.
 *
 * 0x80C0 is the first GPIO register of the ACPI block. Bit 5 is the field pin
 * of the video encoder: D3D's vblank ISR stores !bit5 in [device+0x1DF8], and
 * D3DDevice_GetDisplayFieldStatus reports D3DFIELD_ODD while that is set.
 * Interlaced output alternates fields every vblank, so the pin follows the
 * parity of the vblank count. A constant answer says "odd field" for ever,
 * and the XDK's XMV player only starts its clock on an even one: Burnout 3's
 * EA intro waited on its loading screen indefinitely.
 *
 * The count is xbox_VblankCount, the one vblank count, bumped once per tick
 * by the timer thread. Two regimes follow from it:
 *  - tick-driven (RECOMP_VBLANK=1, once the first vblank is counted): the pin
 *    is the count's parity and holds still between vblanks, however often
 *    it is read;
 *  - no tick (before the first vblank, or the whole run with RECOMP_VBLANK
 *    off): the count is 0, so the pin toggles on every read instead. A
 *    reader waiting for either field still sees it; nothing is implied
 *    about vblank delivery.
 *
 * Every other port reads 0 and is logged on first use, once per port.
 * ========================================================================= */
static int port_first_use(uint32_t port, int is_out)
{
    static uint32_t seen[32];
    static volatile LONG nseen;
    uint32_t key = (port & 0xFFFFu) | (is_out ? 0x10000u : 0u);
    LONG i, n = nseen;

    for (i = 0; i < n && i < 32; i++)
        if (seen[i] == key)
            return 0;
    i = InterlockedIncrement(&nseen) - 1;
    if (i < 32)
        seen[i] = key;
    return i < 32;
}

uint32_t xbox_PortIn(uint32_t port, int width)
{
    uint32_t v = 0;

    port &= 0xFFFFu;
    if (port == 0x80C0u) {
        static volatile LONG reads;
        uint32_t n = xbox_VblankCount();
        if (!n)
            n = (uint32_t)InterlockedIncrement(&reads);
        v = (n & 1u) << 5;
        return v;
    }
    if (port_first_use(port, 0)) {
        fprintf(stderr, "  [PORT] in%c 0x%04X -> 0 (not modelled)\n",
                width == 1 ? 'b' : width == 2 ? 'w' : 'd', port);
        fflush(stderr);
    }
    return v;
}

void xbox_PortOut(uint32_t port, uint32_t value, int width)
{
    port &= 0xFFFFu;
    if (port_first_use(port, 1)) {
        fprintf(stderr, "  [PORT] out%c 0x%04X = 0x%X (ignored)\n",
                width == 1 ? 'b' : width == 2 ? 'w' : 'd', port, value);
        fflush(stderr);
    }
}
