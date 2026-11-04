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
#include "kernel_guest_unwind.h"
#include "xbox_memory_layout.h"
#include "recomp_regs.h"
#include "platform/host_time.h"
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
    /* det_profile: the last give-up's kind and guest call sites. */
    char pf_kind;
    uint32_t pf_site;
    uint32_t pf_bt[4];
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
/* The back-edge quantum's state (xbox_GuestDetBackEdge), zeroed at the
 * join: a host thread reused for a new guest thread starts afresh. The
 * quanta a thread runs at dispatch level (a DPC or an interrupt on the gate
 * holder) count into the same state as its own code's, so a DPC's loop
 * takes a place in the thread's edge cache. */
static XBOX_THREAD_LOCAL unsigned t_edges;
static XBOX_THREAD_LOCAL XboxDetEdge t_edge;

/* RECOMP_DEBUG=det_profile=<path>: where a game frame's vblanks
 * go. A frame spanned 6-7 steps in the mode against 1-2 on the console, and
 * the step comes only when every thread is idle, so the question is what
 * each thread sat in when the clock moved. One S line per step: the
 * vblank, the executor's flips so far, catch-ups and hand-offs since the
 * last step, the wall time since then split into ring (guest code and
 * hand-offs), each device's rounds and the per-poll state hash, then each
 * live thread's last give-up (kind, site, guest call sites). One W line per
 * wait or yield that spanned a step: what it was, how it ended and how many
 * steps it took. Kinds: w timed wait, u untimed wait, d delay, v the
 * vblank wait, c critical section, s self-suspend, y yield (a zero delay
 * or NtYieldExecution), p a lowered spin-wait's pass, e a back edge
 * repeating, k a clock poll, b a busy quantum (no give-up). */
extern RECOMP_TLS uint32_t g_xbox_kernel_caller;
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;
static FILE *s_prof;
static uint64_t s_pf_last_ns, s_pf_dev_ns[XBOX_DET_DEV_COUNT];
static volatile LONGLONG s_pf_hash_ns;
static volatile LONG s_pf_flips, s_pf_held;
static long s_pf_catch0, s_pf_pass0;
static XBOX_THREAD_LOCAL char t_pf_kind;

int xbox_GuestDetProfileOn(void)
{
    return s_prof != NULL;
}

void xbox_GuestDetProfileFlip(void)
{
    if (s_prof)
        InterlockedIncrement(&s_pf_flips);
}

void xbox_GuestDetProfileFlipHeld(int held)
{
    if (s_prof)
        InterlockedExchange(&s_pf_held, held);
}

void xbox_GuestDetProfileHashNs(unsigned long long ns)
{
    /* prof_step takes and clears it under s_lock, which this does not
     * take. Only the 64-bit compare-exchange is in the POSIX shim. */
    LONGLONG o;

    if (!s_prof)
        return;
    do
        o = s_pf_hash_ns;
    while (InterlockedCompareExchange64(&s_pf_hash_ns, o + (LONGLONG)ns, o) != o);
}

static void prof_open(void)
{
    const char *path = recomp_env(RENV_DET_PROFILE);

    if (!path || !*path)
        return;
    s_prof = fopen(path, "w");
    if (!s_prof) {
        fprintf(stderr, "  [DET] cannot write %s; det_profile off\n", path);
        fflush(stderr);
        return;
    }
    fprintf(s_prof, "# S step vblank flips held catchups passes wall_us ring_us"
                    " nv2a_us timer_us ohci_us apu_us hash_us | slot:kind[!]:site:"
                    "bt0,bt1,bt2,bt3 ...\n# W step slot kind site how(o obj,"
                    " t due, r resumed, y yield) spanned bt0,bt1,bt2,bt3\n");
    s_pf_last_ns = xbox_HostNowNs();
}

/* The holder's give-up: what it is and where in the guest it came from. A
 * kernel call's own return address first, then the code addresses on the
 * guest stack above it (a lifted call pushes its return address; stale
 * words can look like frames, as in kernel_rtl.c's backtrace). */
static void prof_note(uint32_t site)
{
    DetSlot *sl = &s_slot[t_slot];
    char kind = t_pf_kind ? t_pf_kind : 'y';
    uint32_t sp = g_esp, first;
    int i, n = 0;

    sl->pf_kind = kind;
    sl->pf_site = site;
    memset(sl->pf_bt, 0, sizeof sl->pf_bt);
    first = (kind == 'e' || kind == 'k' || kind == 'b' || kind == 'p')
            ? site : g_xbox_kernel_caller;
    if (kind == 'k')
        first = 0;
    if (first)
        sl->pf_bt[n++] = first;
    if (!sp || !g_xbox_code_hi)
        return;
    /* Only guest RAM: a thread whose esp sits near the top (or outside it,
     * in a host-made context) must not read past the mapping. */
    if ((uint64_t)sp >= g_xbox_total_ram)
        return;
    for (i = 0; i < 64 && n < 4
                && (uint64_t)sp + i * 4 + 4 <= g_xbox_total_ram; i++) {
        uint32_t w = *(const uint32_t *)((uintptr_t)(sp + i * 4)
                                         + g_xbox_mem_offset);
        if (w >= g_xbox_code_lo && w < g_xbox_code_hi && w != first)
            sl->pf_bt[n++] = w;
    }
}

static void prof_bt(FILE *f, const uint32_t *bt)
{
    fprintf(f, "%x,%x,%x,%x", bt[0], bt[1], bt[2], bt[3]);
}

/* Lock held: a wait or yield of this thread that spanned a step. */
static void prof_wait_end(long steps0, char how)
{
    DetSlot *sl;

    if (!s_prof || t_slot < 0 || s_steps == steps0)
        return;
    sl = &s_slot[t_slot];
    fprintf(s_prof, "W %ld %d %c %x %c %ld ", s_steps, t_slot,
            sl->pf_kind ? sl->pf_kind : '?', sl->pf_site, how, s_steps - steps0);
    prof_bt(s_prof, sl->pf_bt);
    fputc('\n', s_prof);
}

/* The step's line; called by the stepping thread after the devices' round. */
static void prof_step(void)
{
    uint64_t now = xbox_HostNowNs(), wall, dev = 0, hash;
    int i;

    EnterCriticalSection(&s_lock);
    wall = now - s_pf_last_ns;
    do
        hash = (uint64_t)s_pf_hash_ns;
    while ((uint64_t)InterlockedCompareExchange64(&s_pf_hash_ns, 0, (LONGLONG)hash) != hash);
    for (i = 0; i < XBOX_DET_DEV_COUNT; i++)
        dev += s_pf_dev_ns[i];
    fprintf(s_prof, "S %ld %llu %ld %ld %ld %ld %llu %llu %llu %llu %llu %llu %llu |",
            s_steps, (unsigned long long)xbox_GuestTimeVblanks(),
            (long)s_pf_flips, (long)s_pf_held, s_catchups - s_pf_catch0,
            s_passes - s_pf_pass0, (unsigned long long)(wall / 1000),
            (unsigned long long)((wall > dev + hash ? wall - dev - hash : 0) / 1000),
            (unsigned long long)(s_pf_dev_ns[XBOX_DET_DEV_NV2A] / 1000),
            (unsigned long long)(s_pf_dev_ns[XBOX_DET_DEV_TIMER] / 1000),
            (unsigned long long)(s_pf_dev_ns[XBOX_DET_DEV_OHCI] / 1000),
            (unsigned long long)(s_pf_dev_ns[XBOX_DET_DEV_APU] / 1000),
            (unsigned long long)(hash / 1000));
    for (i = 0; i < s_count; i++) {
        DetSlot *sl = &s_slot[i];
        if (sl->state != DET_NEW && sl->state != DET_LIVE)
            continue;
        fprintf(s_prof, " %d:%c%s:%x:", i, sl->pf_kind ? sl->pf_kind : '-',
                sl->suspend ? "!" : "", sl->pf_site);
        prof_bt(s_prof, sl->pf_bt);
    }
    fputc('\n', s_prof);
    if (s_steps % 64 == 0)
        fflush(s_prof);
    memset(s_pf_dev_ns, 0, sizeof s_pf_dev_ns);
    s_pf_catch0 = s_catchups;
    s_pf_pass0 = s_passes;
    s_pf_last_ns = xbox_HostNowNs();
    LeaveCriticalSection(&s_lock);
}

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
            prof_open();
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

/* Lock held: wait until the token is this thread's. A reset unwinds a
 * waiting thread from here, out of the lock (kernel_guest_unwind.c); its
 * part marks the slot dead, and the token was never its own. */
static void wait_turn(void)
{
    int waited = 0;

    while (s_turn != t_slot) {
        if (xbox_GuestUnwindArmed()) {
            LeaveCriticalSection(&s_lock);
            xbox_GuestUnwindSlow(XBOX_UNWIND_DET, (uint32_t)t_slot);
            EnterCriticalSection(&s_lock);
            continue;
        }
        if (!SleepConditionVariableCS(&s_cv, &s_lock, 1000)
            && ++waited % 10 == 0)
            watchdog_line();
    }
}

void xbox_GuestDetUnwindWake(void)
{
    if (s_on != 1)
        return;
    EnterCriticalSection(&s_lock);
    WakeAllConditionVariable(&s_cv);
    LeaveCriticalSection(&s_lock);
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
        t_edges = 0;
        memset(&t_edge, 0, sizeof t_edge);
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
    uint64_t t0 = s_prof ? xbox_HostNowNs() : 0;

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
    if (s_prof)
        s_pf_dev_ns[id] += xbox_HostNowNs() - t0;
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
    /* The USB controller's pass for this vblank, on the new time: its
     * frame counter, the pads' reports and their interrupt. A title's
     * XAPI then reads its pads at one point of guest progress per frame,
     * which is what a pad script or a record on the poll clock counts. */
    device_round(XBOX_DET_DEV_OHCI);
    /* The APU's frames for this vblank, after its interrupt and DPCs. */
    {
        int k;
        for (k = 0; k < XBOX_DET_APU_FRAMES_PER_VBLANK; k++)
            if (device_round(XBOX_DET_DEV_APU) < 0)
                break;
    }
    trace_ram('S', s_steps);
    if (s_prof)
        prof_step();
}

/* ── Giving the token up ─────────────────────────────────── */

/* Lock held, the token this thread's. Counts the turn as idle, and either
 * steps (everyone idle) or passes and waits for the token. Returns with the
 * lock held and the token this thread's again. */
static void give_up_locked(uint32_t site, int self_suspended, int known)
{
    int n, nxt;

    if (s_prof)
        prof_note(site);

    /* A thread giving up somewhere new left its last wait or loop: it got
     * somewhere since. Not so for an idle back-edge verdict (known): the
     * thread came back to a loop it was in, and a thread whose quanta
     * alternate between two idle loops would otherwise reset the count at
     * every turn and the clock would never step (D24). */
    if (site != s_slot[t_slot].site) {
        s_slot[t_slot].site = site;
        if (!known)
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
static XBOX_THREAD_LOCAL uint64_t t_clock_mark;

/* Is a quantum that ran out at va the end of an idle turn? The same edge
 * twice in a row was taken for a poll loop (D7), but a compute loop whose
 * hot inner loop holds most of its edges (a list walk, a vertex loop) ends
 * two quanta there just as often: BLiNX 2's frame code was counted idle
 * mid-frame and the clock stepped under it, 6-7 vblanks a frame
 * (D24). A poll re-reads memory nobody wrote into the same registers; a
 * compute loop usually has its cursor or count in one of them. So the
 * edge must come back with the guest registers it had the last time a
 * quantum ended there. The test is blind to what it does not compare: ebp
 * (a local of the lifted function), x87 and SSE state, memory, and an index
 * masked to a period that divides the quantum; a compute loop that moves
 * only those still reads as a poll, as under D7.
 *
 * Each of the last DET_EDGE_SITES sites keeps its own record. BLiNX 1's
 * boot waits for the GPU in a loop that calls a 400-pass delay loop: most
 * quanta end in the delay with its count moving, one in 25 at the wait's
 * edge with the same registers each time. With one record the sites reset
 * each other, nothing was ever idle and the clock never stepped. A site
 * whose registers keep moving (a poll that counts its spins) turns idle
 * after DET_EDGE_MOVED quanta there and stays idle until the clock steps:
 * a reset at each verdict would let two counting pollers out of phase
 * never be idle in the same round. After the step its moving visits are
 * busy again, so a long load in one hot loop (a decompressor, a copy) is
 * left to the DET_EDGE_BUSY bound and steps the clock once per bound, not
 * at each quantum past DET_EDGE_MOVED. That verdict (2) is not a still
 * one (1): the bound keeps counting through it. The
 * records go when the thread gives its turn up in a kernel wait (a wait
 * that is satisfied at once keeps them, or a poll that takes a free mutex
 * each pass would never be idle). The least recently ended site makes room
 * for a new one; more sites than the cache holds are caught by the
 * DET_EDGE_BUSY bound (edge_bound). Every input is guest state, so the
 * verdict is the same on every run. */
int xbox_GuestDetEdgeIdle(XboxDetEdge *e, uint32_t va, const uint32_t regs[7],
                          long step)
{
    unsigned i, lru = 0;
    int same_regs;

    e->tick++;
    for (i = 0; i < DET_EDGE_SITES; i++) {
        if (e->at[i].used && e->at[i].site == va)
            break;
        if (e->at[i].used < e->at[lru].used)
            lru = i;
    }
    if (i == DET_EDGE_SITES) {
        i = lru;
        e->at[i].site = va;
        memcpy(e->at[i].regs, regs, sizeof e->at[i].regs);
        e->at[i].moved = 0;
        e->at[i].stuck_at = 0;
        e->at[i].used = e->tick;
        return 0;
    }
    e->at[i].used = e->tick;
    same_regs = memcmp(regs, e->at[i].regs, sizeof e->at[i].regs) == 0;
    memcpy(e->at[i].regs, regs, sizeof e->at[i].regs);
    /* A still visit is idle and keeps the moved count: one still pass must
     * not cost a sticky site another DET_EDGE_MOVED quanta. */
    if (same_regs)
        return 1;
    if (e->at[i].moved >= DET_EDGE_MOVED)
        return e->at[i].stuck_at == step ? 2 : 0;
    if (++e->at[i].moved < DET_EDGE_MOVED)
        return 0;
    e->at[i].stuck_at = step;
    return 2;
}

/* The clock reads since the last quantum ran out, in this vblank. */
static int clock_polled(void)
{
    uint64_t r = xbox_GuestTimeFrozenReads();
    uint64_t d = r >= t_clock_mark ? r - t_clock_mark : r;

    t_clock_mark = r;
    return d >= DET_CLOCK_POLL;
}

/* The bound (D24): a thread that has run DET_EDGE_BUSY quanta in a row
 * with no still verdict and no wait that gave its turn up (more cycling
 * loops than the cache holds, a counting site past its step, long guest
 * work) counts as idle until it has a still verdict or the clock steps,
 * then counts again. Idle until a step, so two such threads are idle in
 * the same round; counting again after it, so a long load steps the clock
 * once per DET_EDGE_BUSY quanta, not at each. It bounds one thread's busy
 * run; it does not make the ring step on its own, since a busy quantum or
 * a satisfied wait of another thread still resets the idle count. idle is
 * the verdict: 1 still (the bound starts again), 2 a counting site before
 * its step (the bound counts on through it), 0 busy. s_steps only moves in
 * a step, which the token holder (this thread) makes, so it is stable here
 * and guest progress like the rest of the verdict. */
static int edge_bound(int idle)
{
    if (idle == 1) {
        t_edge.busy = 0;
        return 1;
    }
    if (t_edge.busy >= DET_EDGE_BUSY && s_steps != t_edge.bound_at)
        t_edge.busy = 0;
    if (t_edge.busy < DET_EDGE_BUSY && ++t_edge.busy == DET_EDGE_BUSY)
        t_edge.bound_at = s_steps;
    return idle || t_edge.busy >= DET_EDGE_BUSY;
}

void xbox_GuestDetBackEdge(uint32_t va, int at_dispatch)
{
    int again, nxt;

    if (t_slot < 0 || ++t_edges < DET_QUANTUM)
        return;
    t_edges = 0;
    {
        uint32_t regs[7] = { g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp };
        if (clock_polled())
            va = DET_SITE_CLOCK;
        /* The clock site takes no place in the cache. */
        again = edge_bound(va == DET_SITE_CLOCK
                           ? 1 : xbox_GuestDetEdgeIdle(&t_edge, va, regs, s_steps));
    }
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
    {
        long steps0 = s_steps;
        t_pf_kind = va == DET_SITE_CLOCK ? 'k' : again ? 'e' : 'b';
        if (again) {
            give_up_locked(va, 0, 1);
        } else {
            /* Busy: progress since the last give-up, so nobody is idle yet. */
            s_slot[t_slot].site = va;
            s_idle = 0;
            if (s_prof)
                prof_note(va);
            nxt = next_runnable(t_slot);
            if (nxt >= 0 && nxt != t_slot) {
                pass_locked(nxt);
                wait_turn();
            }
        }
        prof_wait_end(steps0, 'y');
        t_pf_kind = 0;
    }
    LeaveCriticalSection(&s_lock);
}

static int vblank_moved(void *ctx);

/* det_profile's kind of a blocking give-up, from its site (the callers'
 * fixed ones: 99 delay, 231 self-suspend, 233/235 waits; a critical
 * section's site is its address). */
static char prof_block_kind(XboxDetCheck check, uint64_t due, uint32_t site)
{
    if (check == vblank_moved)
        return 'v';
    if (site == DET_SITE_DELAY)
        return 'd';
    if (site == DET_SITE_SUSPEND)
        return 's';
    if (site == DET_SITE_WAIT_ONE || site == DET_SITE_WAIT_MANY)
        return due ? 'w' : 'u';
    return 'c';
}

static int det_block(XboxDetCheck check, void *ctx, uint64_t due,
                     uint32_t site, char *how);

int xbox_GuestDetBlock(XboxDetCheck check, void *ctx, uint64_t due,
                       uint32_t site)
{
    long steps0;
    char how = 'o';
    int r;

    if (!s_prof)
        return det_block(check, ctx, due, site, &how);
    EnterCriticalSection(&s_lock);
    steps0 = s_steps;
    LeaveCriticalSection(&s_lock);
    t_pf_kind = prof_block_kind(check, due, site);
    r = det_block(check, ctx, due, site, &how);
    EnterCriticalSection(&s_lock);
    prof_wait_end(steps0, how);
    LeaveCriticalSection(&s_lock);
    t_pf_kind = 0;
    return r;
}

static int det_block(XboxDetCheck check, void *ctx, uint64_t due,
                     uint32_t site, char *how)
{
    for (;;) {
        int suspended;

        xbox_GuestUnwindPoint(XBOX_UNWIND_DET, site);
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
            if (due && xbox_GuestTimeReached(due)) {
                *how = 't';
                return 0;
            }
            if (!check && !due) {
                *how = 'r';
                return 1;               /* a self-suspend, resumed */
            }
        }
        /* A thread that gives its turn up in a wait has left its loops:
         * their edge records go (D24). A wait satisfied at once above keeps
         * them. */
        memset(&t_edge, 0, sizeof t_edge);
        EnterCriticalSection(&s_lock);
        give_up_locked(site, suspended, 0);
        LeaveCriticalSection(&s_lock);
    }
}

void xbox_GuestDetYield(uint32_t site)
{
    long steps0;

    EnterCriticalSection(&s_lock);
    steps0 = s_steps;
    /* 99 a zero delay, 238 the yield call, else a lowered spin's pass. */
    t_pf_kind = site == DET_SITE_DELAY || site == DET_SITE_YIELD ? 'y' : 'p';
    give_up_locked(site, 0, 0);
    prof_wait_end(steps0, 'y');
    t_pf_kind = 0;
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
    xbox_GuestDetBlock(NULL, NULL, 0, DET_SITE_SUSPEND);
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
