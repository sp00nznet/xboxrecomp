/*
 * kernel_guest_det.c - the deterministic guest scheduler (RECOMP_DEBUG=
 * deterministic with RECOMP_GUEST_LOCK on; see kernel.h).
 *
 * The virtual clock (guest_time.c) makes every clock the title reads a
 * count of vblanks. That alone did not make two runs alike: the count still
 * stepped on the host's 60 Hz schedule, the guest CPU changed hands at a
 * 4 ms host quantum, and the GPU walker, the timer thread and their DPCs
 * wrote guest memory whenever the host ran them. Two runs of one input
 * script differed from the first pad poll (openspec deterministic-mode,
 * phase 1).
 *
 * So here the guest CPU is a token passed round a ring of the guest
 * threads, in the order of their ring slots. A creator takes the new
 * thread's slot while it holds the token (the lowest slot an ended thread
 * left, else a new one at the end), so the order is not creation order once
 * slots are reused, but it is the same on every run:
 *
 *  - the holder keeps it through every kernel call that does not block,
 *    with no quantum and no preemption;
 *  - a wait, a delay, a contended critical section, a self-suspend and a
 *    lowered spin-wait's pass give it to the next thread in the ring that
 *    is not suspended; a waiting thread checks its object only when the
 *    token comes back to it;
 *  - when every runnable thread has had the token once without getting
 *    anywhere, the guest is idle: the devices catch up (the GPU walks to
 *    PUT), and when they had nothing to do the clock steps one vblank and
 *    the timer thread runs its pass (the vblank interrupt, the DPCs, the
 *    due timers), all while every guest thread is parked.
 *
 * Time therefore moves when the guest has nothing left to do, never on the
 * host's schedule, and nothing writes guest memory at a host-timed moment.
 * It cannot deadlock on time: a guest waiting for a timeout or a vblank is
 * idle, and the step comes.
 */
#include "kernel.h"
#include "guest_time.h"
#include "recomp_env.h"
#include "guest_state_hash.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;

#define DET_SLOTS 256

enum { DET_FREE, DET_NEW, DET_LIVE, DET_DEAD };

typedef struct {
    uint32_t obj;               /* guest thread object VA, 0 until bound */
    int state;
    int suspend;                /* the guest's suspend count */
    unsigned long tid;          /* host tid, for the watchdog's line */
    uint32_t site;              /* where it last gave the token up */
} DetSlot;

typedef struct {
    int present;                /* the device's thread exists */
    int grant, done, worked;
    long rounds;
} DetDevice;

static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION s_lock;
static CONDITION_VARIABLE s_cv;
static volatile LONG s_on = -1;
static volatile LONG s_armed;

static DetSlot s_slot[DET_SLOTS];
static int s_count;
static int s_turn = -1;                 /* the slot holding the token */
static int s_idle;                      /* turns given up in a row with no progress */
static DetDevice s_dev[XBOX_DET_DEV_COUNT];
static long s_steps, s_catchups, s_passes;

/* RECOMP_DEBUG=det_trace=<path>: one line per hand-off, step and catch-up,
 * and at each step and catch-up the hash of every 64 KB page of guest RAM
 * (<path>.pages, binary). Two runs diffed name the first event they part
 * at and the memory that differs there. */
static FILE *s_trace, *s_trace_pages;
static char *s_trace_path;
static char s_trace_dump[16][24];       /* det_dump: S<n> / C<n> events */
static int s_trace_ndumps;
static uint64_t *s_trace_hash;
static size_t s_trace_npages;
/* det_snap: a few pages after every event inside a vblank window. A full
 * dump lands only on events named in advance, and a divergence that moves
 * from run to run slips between them; a window of every event over the
 * pages that part names the first event, and the hand-off, that wrote it. */
static FILE *s_snap;
static uint64_t s_snap_from, s_snap_to;
static uint32_t s_snap_va[8];
static int s_snap_n;

static XBOX_THREAD_LOCAL int t_slot = -1;
static XBOX_THREAD_LOCAL uint32_t t_bind;

static BOOL CALLBACK det_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_lock);
    InitializeConditionVariable(&s_cv);
    return TRUE;
}

static void trace_open(void)
{
    const char *path = recomp_env(RENV_DET_TRACE);
    char *pages;

    if (!path || !*path)
        return;
    pages = malloc(strlen(path) + 8);
    s_trace_npages = g_xbox_total_ram >> 16;
    s_trace_hash = calloc(s_trace_npages ? s_trace_npages : 1, sizeof *s_trace_hash);
    if (!pages || !s_trace_hash) {
        free(pages);
        return;
    }
    sprintf(pages, "%s.pages", path);
    s_trace_path = malloc(strlen(path) + 1);
    if (s_trace_path)
        strcpy(s_trace_path, path);
    {
        const char *d = recomp_env(RENV_DET_DUMP);
        while (d && *d && s_trace_ndumps < 16) {
            size_t n = strcspn(d, ",");
            if (n > 0 && n < sizeof s_trace_dump[0]) {
                memcpy(s_trace_dump[s_trace_ndumps], d, n);
                s_trace_dump[s_trace_ndumps++][n] = 0;
            }
            d += n + (d[n] == ',');
        }
    }
    {
        const char *d = recomp_env(RENV_DET_SNAP);
        char *end;
        if (d && *d) {
            s_snap_from = strtoull(d, &end, 0);
            if (*end == '-')
                s_snap_to = strtoull(end + 1, &end, 0);
            while (*end == ':') {
                unsigned long va = strtoul(end + 1, &end, 0);
                /* Guest RAM, or the NV2A register window, which is plain
                 * memory here and outside the RAM hash. */
                if (s_snap_n < 8 && (va < g_xbox_total_ram
                                     || (va >= 0xFD000000ul && va < 0xFE000000ul)))
                    s_snap_va[s_snap_n++] = (uint32_t)va & ~0xFFFFu;
            }
            if (s_snap_n) {
                sprintf(pages, "%s.snap", path);
                s_snap = fopen(pages, "wb");
            }
        }
    }
    s_trace = fopen(path, "w");
    sprintf(pages, "%s.pages", path);
    s_trace_pages = fopen(pages, "wb");
    free(pages);
    if (!s_trace || !s_trace_pages) {
        fprintf(stderr, "  [DET] cannot write %s(.pages); det_trace off\n",
                path);
        fflush(stderr);
        if (s_trace)
            fclose(s_trace);
        s_trace = NULL;
        return;
    }
    fprintf(s_trace, "# P pass from to site | S step|C catchup n vblank"
                     " ram_hash; .pages: per S/C u64 event + u64 per page\n");
}

static int snap_window(void)
{
    uint64_t vb;

    if (!s_snap)
        return 0;
    vb = xbox_GuestTimeVblanks();
    return vb >= s_snap_from && vb <= s_snap_to;
}

/* One det_snap record: u64 event (kind in the top byte), u64 vblank, then
 * 64 KB per page in the order given. */
static void trace_snap(char kind, long n)
{
    uint64_t vb, ev;
    int i;

    if (!snap_window())
        return;
    vb = xbox_GuestTimeVblanks();
    ev = ((uint64_t)(unsigned char)kind << 56) | (uint64_t)n;
    fwrite(&ev, 8, 1, s_snap);
    fwrite(&vb, 8, 1, s_snap);
    for (i = 0; i < s_snap_n; i++)
        fwrite((const void *)(uintptr_t)(g_xbox_mem_offset + s_snap_va[i]),
               1, 0x10000, s_snap);
}

/* A kernel call and what it returned, inside the det_snap window: a pair of
 * runs whose RAM agrees at a hand-off but whose next pass differs took the
 * difference from somewhere outside RAM, and a call's result is the usual
 * door. */
void xbox_GuestDetTraceCall(uint32_t ordinal, uint32_t caller, uint32_t ret)
{
    if (!s_trace || !snap_window())
        return;
    fprintf(s_trace, "K %d %u %08x %08x\n", t_slot, ordinal, caller, ret);
}

/* The holder's event; the token serialises the callers. */
static void trace_pass(int from, int to, uint32_t site)
{
    if (s_trace && snap_window()) {
        /* Inside the det_snap window each pass also carries the whole
         * RAM's hash, so a pair of runs shows which hand-off a divergence
         * entered at, not only the vblank. */
        uint64_t all = 0xCBF29CE484222325ULL;
        size_t p;
        for (p = 0; p < s_trace_npages; p++)
            all = (all ^ xbox_GuestStateHashPage(
                       (const void *)(uintptr_t)(g_xbox_mem_offset + (p << 16)),
                       (uint32_t)(p << 16))) * 0x100000001B3ULL;
        fprintf(s_trace, "P %ld %d %d %08x %016llx\n", s_passes, from, to,
                site, (unsigned long long)all);
    } else if (s_trace)
        fprintf(s_trace, "P %ld %d %d %08x\n", s_passes, from, to, site);
    trace_snap('P', s_passes);
}

static void trace_ram(char kind, long n)
{
    uint64_t all = 0xCBF29CE484222325ULL, ev;
    size_t p;

    if (!s_trace)
        return;
    for (p = 0; p < s_trace_npages; p++) {
        uint32_t va = (uint32_t)(p << 16);
        uint64_t h = xbox_GuestStateHashPage(
            (const void *)(uintptr_t)(g_xbox_mem_offset + va), va);
        s_trace_hash[p] = h;
        all = (all ^ h) * 0x100000001B3ULL;
    }
    ev = ((uint64_t)(unsigned char)kind << 56) | (uint64_t)n;
    fwrite(&ev, 8, 1, s_trace_pages);
    fwrite(s_trace_hash, 8, s_trace_npages, s_trace_pages);
    trace_snap(kind, n);
    fprintf(s_trace, "%c %ld %llu %016llx\n", kind, n,
            (unsigned long long)xbox_GuestTimeVblanks(),
            (unsigned long long)all);
    fflush(s_trace);
    if (s_trace_ndumps && s_trace_path) {
        char ev_name[24], *name;
        int i;
        snprintf(ev_name, sizeof ev_name, "%c%ld", kind, n);
        for (i = 0; i < s_trace_ndumps; i++) {
            FILE *f;
            if (strcmp(ev_name, s_trace_dump[i]) != 0)
                continue;
            name = malloc(strlen(s_trace_path) + 32);
            if (!name)
                break;
            sprintf(name, "%s.%s.ram", s_trace_path, ev_name);
            f = fopen(name, "wb");
            if (f) {
                fwrite((const void *)(uintptr_t)g_xbox_mem_offset, 1,
                       s_trace_npages << 16, f);
                fclose(f);
            }
            free(name);
        }
    }
}

/* The first guest join (the main thread's, at bridge init) decides: the
 * game's main sets RECOMP_GUEST_LOCK after the memory layout starts the
 * device threads, and reading the lock's config before that would fix it
 * off for the whole run. Until then the mode is off. */
void xbox_GuestDetArm(void)
{
    InterlockedExchange(&s_armed, 1);
}

/* Without the deterministic key the mode is off from the first question,
 * so a stock run never sees the undecided state (its device threads would
 * nap until the first join). */
static int decided_off_early(void)
{
    if (xbox_GuestTimeVirtual())
        return 0;
    InterlockedCompareExchange(&s_on, 0, -1);
    return 1;
}

int xbox_GuestDetOn(void)
{
    LONG on = s_on;

    if (on < 0) {
        if (!s_armed) {
            decided_off_early();
            return 0;
        }
        InitOnceExecuteOnce(&s_once, det_init, NULL, NULL);
        on = xbox_GuestTimeVirtual() && xbox_GuestCpuOn();
        if (InterlockedCompareExchange(&s_on, on, -1) == -1 && on) {
            trace_open();
            fprintf(stderr, "  [DET] deterministic guest scheduler: the guest"
                            " CPU passes round the threads in ring-slot order,"
                            " and the clock steps when they are all idle\n");
            fflush(stderr);
        }
        on = s_on;
    }
    return (int)on;
}

/* A device thread that sleeps on its own condition before the mode is
 * decided would miss its first grant: it naps briefly until then. */
int xbox_GuestDetUndecided(void)
{
    return s_on < 0 && !decided_off_early();
}

void xbox_GuestDetTestReset(int on)
{
    InitOnceExecuteOnce(&s_once, det_init, NULL, NULL);
    EnterCriticalSection(&s_lock);
    memset(s_slot, 0, sizeof s_slot);
    memset(s_dev, 0, sizeof s_dev);
    s_count = 0;
    s_turn = -1;
    s_idle = 0;
    s_steps = s_catchups = s_passes = 0;
    LeaveCriticalSection(&s_lock);
    InterlockedExchange(&s_on, on);
    t_slot = -1;
    t_bind = 0;
}

int xbox_GuestDetThread(void)
{
    return t_slot >= 0;
}

static int runnable(int i)
{
    return (s_slot[i].state == DET_NEW || s_slot[i].state == DET_LIVE)
           && s_slot[i].suspend == 0;
}

static int runnable_count(void)
{
    int i, n = 0;

    for (i = 0; i < s_count; i++)
        n += runnable(i);
    return n;
}

/* Threads alive but held by a suspend. */
static int suspended_count(void)
{
    int i, n = 0;

    for (i = 0; i < s_count; i++)
        n += (s_slot[i].state == DET_NEW || s_slot[i].state == DET_LIVE)
             && s_slot[i].suspend > 0;
    return n;
}

/* The next runnable slot after from, from itself last; -1 when none. */
static int next_runnable(int from)
{
    int k;

    for (k = 1; k <= s_count; k++) {
        int j = (from + k) % s_count;
        if (runnable(j))
            return j;
    }
    return -1;
}

/* The lowest dead slot, else a new one at the end. Threads end at fixed
 * points of the ring now (a thread's end is published under the token), so
 * which slot a new thread reuses is the same on every run; never reusing
 * them held a run to DET_SLOTS thread creations, and a title that starts
 * loader threads on every stage load ran past it. Never the turn's slot: a
 * parting thread marks its slot dead and still holds the turn while it
 * steps with the lock dropped (xbox_GuestDetPart), and a thread given that
 * slot would find the turn its own at its join and run beside it. */
static int slot_new(uint32_t obj)
{
    int i;

    for (i = 0; i < s_count; i++)
        if (s_slot[i].state == DET_DEAD && i != s_turn)
            break;
    if (i == s_count) {
        if (s_count == DET_SLOTS) {
            fprintf(stderr, "  [DET] more than %d live guest threads: the"
                            " ring is full\n", DET_SLOTS);
            fflush(stderr);
            return -1;
        }
        s_count++;
    }
    memset(&s_slot[i], 0, sizeof s_slot[i]);
    s_slot[i].obj = obj;
    s_slot[i].state = DET_NEW;
    return i;
}

static int slot_find(uint32_t obj)
{
    int i;

    if (!obj)
        return -1;
    for (i = s_count - 1; i >= 0; i--)
        if (s_slot[i].obj == obj && s_slot[i].state != DET_DEAD)
            return i;
    return -1;
}

/* The ring's slots, for a stall report. Lock held. */
static void slots_line(void)
{
    int i;

    for (i = 0; i < s_count; i++)
        fprintf(stderr, "  [DET]   slot %d obj 0x%08X tid %lu state %d"
                        " suspend %d last gave up at 0x%08X\n", i,
                s_slot[i].obj, s_slot[i].tid, s_slot[i].state,
                s_slot[i].suspend, s_slot[i].site);
    fflush(stderr);
}

/* Names who holds the token when it has not moved for a long while: a host
 * wait the scheduler does not know is a hang here, and this says whose. */
static void watchdog_line(void)
{
    fprintf(stderr, "  [DET] the token has not come back for 10 s: turn %d,"
                    " idle %d, steps %ld\n", s_turn, s_idle, s_steps);
    slots_line();
}

/* Lock held: wait until the token is this thread's. */
static void wait_turn(void)
{
    int waited = 0;

    while (s_turn != t_slot)
        if (!SleepConditionVariableCS(&s_cv, &s_lock, 1000)
            && ++waited % 10 == 0)
            watchdog_line();
}

/* Lock held: hand the token on. */
static void pass_locked(int to)
{
    trace_pass(s_turn, to, s_turn >= 0 ? s_slot[s_turn].site : 0);
    s_turn = to;
    s_passes++;
    WakeAllConditionVariable(&s_cv);
}

static void idle_step(void);

/* ── Threads ─────────────────────────────────────────────── */

void xbox_GuestDetThreadNew(uint32_t obj)
{
    if (!xbox_GuestDetOn() || !obj)
        return;
    EnterCriticalSection(&s_lock);
    if (slot_find(obj) < 0)
        slot_new(obj);
    LeaveCriticalSection(&s_lock);
}

/* The creator's host thread could not start: the slot it took would count
 * as runnable for good. */
void xbox_GuestDetThreadDrop(uint32_t obj)
{
    int i;

    if (!xbox_GuestDetOn() || !obj)
        return;
    EnterCriticalSection(&s_lock);
    i = slot_find(obj);
    if (i >= 0 && s_slot[i].state == DET_NEW)
        s_slot[i].state = DET_DEAD;
    LeaveCriticalSection(&s_lock);
}

void xbox_GuestDetBind(uint32_t obj)
{
    if (!xbox_GuestDetOn())
        return;
    EnterCriticalSection(&s_lock);
    if (t_slot >= 0) {
        /* The main thread joined before it had an object. */
        if (!s_slot[t_slot].obj)
            s_slot[t_slot].obj = obj;
    } else {
        t_bind = obj;
    }
    LeaveCriticalSection(&s_lock);
}

void xbox_GuestDetJoin(void)
{
    int i;

    EnterCriticalSection(&s_lock);
    i = slot_find(t_bind);
    if (i < 0) {
        /* Not announced by a creator: the first thread (the title's main),
         * or a path that spawns without xbox_GuestDetThreadNew. The latter
         * joins the ring at a host-timed moment; say so. */
        if (s_count > 0) {
            fprintf(stderr, "  [DET] a guest thread (obj 0x%08X) joined"
                            " without a slot: its place in the ring is"
                            " host-timed\n", t_bind);
            fflush(stderr);
        }
        i = slot_new(t_bind);
    }
    if (i >= 0) {
        t_slot = i;
        s_slot[i].state = DET_LIVE;
        s_slot[i].tid = (unsigned long)GetCurrentThreadId();
        if (s_turn < 0)
            s_turn = i;
        wait_turn();
        s_idle = 0;
    }
    LeaveCriticalSection(&s_lock);
    /* Every lowered spin pass and every back edge calls in, so it can give
     * the token up (xbox_GuestDetBackEdge counts the edges). */
    g_xbox_guest_cpu_waiters = 1;
    InterlockedExchange((volatile LONG *)&g_xbox_guest_cpu_preempt, 1);
}

/* How many idle steps a parting thread takes for threads that are all
 * suspended before it gives up on a resume: a minute of guest time. */
#define DET_PART_STEPS 3600

void xbox_GuestDetPart(void)
{
    int nxt, steps = 0;

    if (t_slot < 0)
        return;
    EnterCriticalSection(&s_lock);
    s_slot[t_slot].state = DET_DEAD;
    if (s_turn == t_slot) {
        /* Every thread left is suspended: nobody would step the clock, and
         * what resumes them (a DPC, a timer) runs only on a step. Step here
         * until one is runnable, or none is left to wait for. A resume that
         * never comes would hold this thread here for good (the main
         * thread's part at shutdown with it): after DET_PART_STEPS the
         * stall is reported as the watchdog reports one, and the turn is
         * left empty, which a later resume takes (xbox_GuestDetSuspend). */
        while ((nxt = next_runnable(t_slot)) < 0 && suspended_count() > 0) {
            if (steps++ == DET_PART_STEPS) {
                fprintf(stderr, "  [DET] a parting thread stepped %d times"
                                " and every thread left is still suspended:"
                                " the turn is left empty, steps %ld\n",
                        DET_PART_STEPS, s_steps);
                slots_line();
                break;
            }
            LeaveCriticalSection(&s_lock);
            idle_step();
            EnterCriticalSection(&s_lock);
        }
        s_idle = 0;
        pass_locked(nxt);
    }
    LeaveCriticalSection(&s_lock);
    t_slot = -1;
}

int xbox_GuestDetSuspend(uint32_t obj, int delta, ULONG *prev)
{
    int i, handled = 0;

    if (!xbox_GuestDetOn())
        return 0;
    EnterCriticalSection(&s_lock);
    i = slot_find(obj);
    if (i >= 0 && (delta > 0 || s_slot[i].suspend > 0)) {
        if (prev)
            *prev = (ULONG)s_slot[i].suspend;
        s_slot[i].suspend += delta > 0 ? 1 : -1;
        handled = 1;
        /* A resume readies a thread: progress for the idle count. With the
         * ring empty (the turn at -1), the resumed thread takes the turn. */
        if (delta < 0) {
            s_idle = 0;
            if (s_turn < 0 && runnable(i))
                pass_locked(i);
        }
    }
    LeaveCriticalSection(&s_lock);
    return handled;
}

/* ── Devices ─────────────────────────────────────────────── */

void xbox_GuestDetDeviceAdd(int id)
{
    if (id < 0 || id >= XBOX_DET_DEV_COUNT)
        return;
    /* Before the mode is decided (the nv2a-ack thread starts first): a
     * device that is present is granted passes only when it is on. */
    InitOnceExecuteOnce(&s_once, det_init, NULL, NULL);
    EnterCriticalSection(&s_lock);
    s_dev[id].present = 1;
    LeaveCriticalSection(&s_lock);
}

/* A device that ends: its passes are no longer waited for, one being waited
 * for now ends, and its thread, if it waits for a grant, gets one to see
 * that it is leaving. */
void xbox_GuestDetDeviceRemove(int id)
{
    if (id < 0 || id >= XBOX_DET_DEV_COUNT)
        return;
    InitOnceExecuteOnce(&s_once, det_init, NULL, NULL);
    EnterCriticalSection(&s_lock);
    s_dev[id].present = 0;
    s_dev[id].done = 1;
    s_dev[id].grant = 1;
    s_dev[id].worked = 0;
    WakeAllConditionVariable(&s_cv);
    LeaveCriticalSection(&s_lock);
}

void xbox_GuestDetDeviceWait(int id)
{
    EnterCriticalSection(&s_lock);
    while (!s_dev[id].grant)
        SleepConditionVariableCS(&s_cv, &s_lock, INFINITE);
    s_dev[id].grant = 0;
    LeaveCriticalSection(&s_lock);
}

void xbox_GuestDetDeviceDone(int id, int worked)
{
    EnterCriticalSection(&s_lock);
    s_dev[id].worked = worked;
    s_dev[id].done = 1;
    s_dev[id].rounds++;
    WakeAllConditionVariable(&s_cv);
    LeaveCriticalSection(&s_lock);
}

/* One pass of a device thread, waited for. 1 when it did work; -1 when the
 * device has no thread. */
static int device_round(int id)
{
    int worked;

    EnterCriticalSection(&s_lock);
    if (!s_dev[id].present) {
        LeaveCriticalSection(&s_lock);
        return -1;
    }
    s_dev[id].done = 0;
    s_dev[id].grant = 1;
    WakeAllConditionVariable(&s_cv);
    while (!s_dev[id].done)
        SleepConditionVariableCS(&s_cv, &s_lock, INFINITE);
    s_dev[id].grant = 0;
    worked = s_dev[id].worked;
    LeaveCriticalSection(&s_lock);
    return worked;
}

/* Every runnable thread is waiting: the GPU first, which may end a wait on
 * a fence or on GET; when it had nothing to walk, one vblank. */
static void idle_step(void)
{
    if (device_round(XBOX_DET_DEV_NV2A) > 0) {
        s_catchups++;
        trace_ram('C', s_catchups);
        return;
    }
    s_steps++;
    if (device_round(XBOX_DET_DEV_TIMER) < 0)
        xbox_GuestTimeVblank();         /* no timer thread yet */
    /* The APU's frames for this vblank, after its interrupt and DPCs. */
    {
        int k;
        for (k = 0; k < XBOX_DET_APU_FRAMES_PER_VBLANK; k++)
            if (device_round(XBOX_DET_DEV_APU) < 0)
                break;
    }
    trace_ram('S', s_steps);
}

/* ── Giving the token up ─────────────────────────────────── */

/* Lock held, the token this thread's. Counts the turn as idle, and either
 * steps (everyone idle) or passes and waits for the token. Returns with the
 * lock held and the token this thread's again. */
static void give_up_locked(uint32_t site, int self_suspended)
{
    int n, nxt;

    /* A thread giving up somewhere new left its last wait or loop: it got
     * somewhere since. */
    if (site != s_slot[t_slot].site) {
        s_slot[t_slot].site = site;
        s_idle = 0;
    }
    if (!self_suspended)
        s_idle++;
    n = runnable_count();
    if (n == 0 || s_idle >= n) {
        s_idle = 0;
        LeaveCriticalSection(&s_lock);
        idle_step();
        EnterCriticalSection(&s_lock);
        return;
    }
    nxt = next_runnable(t_slot);
    if (nxt < 0 || nxt == t_slot)
        return;
    pass_locked(nxt);
    wait_turn();
}

/* Lifted loops' back edges (RECOMP_BACK_EDGE; the preempt flag stays set in
 * this mode): the console's timer tick, counted in back edges rather than
 * host time. A thread past DET_QUANTUM edges hands the turn on. Two quanta
 * running out at the same edge in a row are a loop polling something
 * (GET, a busy bit) with no kernel call and no lowered spin: that counts as
 * idle, which is what lets the device it polls catch up. So is a quantum
 * that read the clock DET_CLOCK_POLL times without it moving: a loop
 * waiting for a time (an XDK library's timeout spin, raising and lowering
 * IRQL each turn) ends its quanta at a different edge each time, and
 * counted busy it held the clock frozen for good. At DISPATCH no thread
 * is switched; the GPU still gets its pass there. */
#define DET_QUANTUM 4096u
#define DET_CLOCK_POLL 64u
#define DET_SITE_CLOCK 0xFFFFFFC1u      /* the give-up site of a clock poll */
static XBOX_THREAD_LOCAL unsigned t_edges;
static XBOX_THREAD_LOCAL uint32_t t_edge_site;
static XBOX_THREAD_LOCAL uint64_t t_clock_mark;

/* The clock reads since the last quantum ran out, in this vblank. */
static int clock_polled(void)
{
    uint64_t r = xbox_GuestTimeFrozenReads();
    uint64_t d = r >= t_clock_mark ? r - t_clock_mark : r;

    t_clock_mark = r;
    return d >= DET_CLOCK_POLL;
}

void xbox_GuestDetBackEdge(uint32_t va, int at_dispatch)
{
    int again, nxt;

    if (t_slot < 0 || ++t_edges < DET_QUANTUM)
        return;
    t_edges = 0;
    if (clock_polled())
        va = DET_SITE_CLOCK;
    again = va == t_edge_site || va == DET_SITE_CLOCK;
    t_edge_site = va;
    if (at_dispatch) {
        if (again && device_round(XBOX_DET_DEV_NV2A) > 0) {
            EnterCriticalSection(&s_lock);
            s_catchups++;
            LeaveCriticalSection(&s_lock);
            trace_ram('C', s_catchups);
        }
        return;
    }
    EnterCriticalSection(&s_lock);
    if (again) {
        give_up_locked(va, 0);
    } else {
        /* Busy: progress since the last give-up, so nobody is idle yet. */
        s_slot[t_slot].site = va;
        s_idle = 0;
        nxt = next_runnable(t_slot);
        if (nxt >= 0 && nxt != t_slot) {
            pass_locked(nxt);
            wait_turn();
        }
    }
    LeaveCriticalSection(&s_lock);
}

int xbox_GuestDetBlock(XboxDetCheck check, void *ctx, uint64_t due,
                       uint32_t site)
{
    for (;;) {
        int suspended;

        EnterCriticalSection(&s_lock);
        suspended = s_slot[t_slot].suspend > 0;
        LeaveCriticalSection(&s_lock);
        if (!suspended) {
            if (check && check(ctx)) {
                EnterCriticalSection(&s_lock);
                s_idle = 0;
                LeaveCriticalSection(&s_lock);
                return 1;
            }
            if (due && xbox_GuestTimeReached(due))
                return 0;
            if (!check && !due)
                return 1;               /* a self-suspend, resumed */
        }
        EnterCriticalSection(&s_lock);
        give_up_locked(site, suspended);
        LeaveCriticalSection(&s_lock);
    }
}

void xbox_GuestDetYield(uint32_t site)
{
    EnterCriticalSection(&s_lock);
    give_up_locked(site, 0);
    LeaveCriticalSection(&s_lock);
}

int xbox_GuestDetSuspendSelf(ULONG *prev)
{
    if (t_slot < 0)
        return 0;
    EnterCriticalSection(&s_lock);
    if (prev)
        *prev = (ULONG)s_slot[t_slot].suspend;
    s_slot[t_slot].suspend++;
    LeaveCriticalSection(&s_lock);
    xbox_GuestDetBlock(NULL, NULL, 0, 231);
    return 1;
}

static int vblank_moved(void *ctx)
{
    return xbox_GuestTimeVblanks() != *(const uint64_t *)ctx;
}

int xbox_GuestDetWaitVblank(uint64_t seen, uint32_t site)
{
    if (t_slot < 0)
        return 0;
    xbox_GuestDetBlock(vblank_moved, &seen, 0, site);
    return 1;
}

void xbox_GuestDetStats(long *steps, long *catchups, long *passes)
{
    EnterCriticalSection(&s_lock);
    *steps = s_steps;
    *catchups = s_catchups;
    *passes = s_passes;
    LeaveCriticalSection(&s_lock);
}
