/*
 * kernel_prof.c - where the host-run guest routines spend their time.
 *
 * RECOMP_TRACE=dpc. The timer thread is the vblank clock, the DPC drain and
 * the kernel timers in one loop, so a deferred routine that runs long, or
 * waits long for the one-CPU gate before it starts, stretches the frame
 * clock with it: the 600-vblank line reads 20 s instead of 10 and a title
 * that streams once per vblank misses its deadlines (audio streamed from
 * disc turns to noise). The [IRQLHOLD] dump names the thread but not the
 * routine, and its host return address is the bracket in kernel_run_dpc,
 * the same for every DPC.
 *
 * So this keeps, per guest routine, how often it ran, how long, the longest
 * run, and what it waited on: the dispatch gate before it started, the APU
 * model's lock and KeStallExecutionProcessor inside. Waits on a thread that
 * is not running one of these routines (a title thread raising IRQL) go to
 * a bucket of their own. A few loop counters from the timer thread sit next
 * to it. The top of the table is printed under each 600-vblank line and the
 * interval starts over.
 *
 * Off, every hook is one load and a branch; nothing here allocates or takes
 * a lock until the first report is wanted.
 */
#include "kernel.h"
#include "recomp_env.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */
#include <stdio.h>
#include <string.h>

int xbox_prof_state_ = -1;

static void sampler_start(void);

/* Once: the first hooks can come from several threads at the same time, and
 * each would start a sampler of its own. */
static INIT_ONCE s_init_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK prof_init_once(PINIT_ONCE o, PVOID p, PVOID *c)
{
    int on = recomp_env_on(RENV_DPC_PROF) ? 1 : 0;

    (void)o; (void)p; (void)c;
    if (on)
        sampler_start();
    xbox_prof_state_ = on;
    return TRUE;
}

int xbox_ProfInit(void)
{
    InitOnceExecuteOnce(&s_init_once, prof_init_once, NULL, NULL);
    return xbox_prof_state_;
}

typedef struct {
    long long n, tot, max;
} ProfSum;

static void sum_add(ProfSum *s, long long v)
{
    s->n++;
    s->tot += v;
    if (v > s->max)
        s->max = v;
}

#define PROF_ENTRIES 48
typedef struct {
    int      kind;
    uint32_t routine;
    ProfSum  run;
    ProfSum  wait[XBOX_PROF_WAITS];
} ProfEntry;

static ProfEntry s_entries[PROF_ENTRIES];
static int s_used;
static ProfSum s_outside[XBOX_PROF_WAITS];   /* waits on no routine's thread */
static ProfSum s_counters[XBOX_PROF_COUNTERS];
static int s_overflow;

static CRITICAL_SECTION s_lock;
static INIT_ONCE s_lock_once = INIT_ONCE_STATIC_INIT;
static XBOX_THREAD_LOCAL ProfEntry *t_current;

static BOOL CALLBACK prof_lock_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_lock);
    return TRUE;
}

static void prof_lock(void)
{
    InitOnceExecuteOnce(&s_lock_once, prof_lock_init, NULL, NULL);
    EnterCriticalSection(&s_lock);
}

long long xbox_ProfNowUs(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;

    if (!freq.QuadPart && !QueryPerformanceFrequency(&freq))
        return (long long)GetTickCount64() * 1000;
    QueryPerformanceCounter(&now);
    return (long long)(now.QuadPart / freq.QuadPart) * 1000000
           + (long long)(now.QuadPart % freq.QuadPart) * 1000000 / freq.QuadPart;
}

/* What a long DPC is doing. The table above says which routine holds the
 * timer thread, not where in it; the guest has no program counter to
 * sample. So while a timer-thread DPC has been running for more than
 * 20 ms, a sampler thread looks at it once a millisecond: the NV2A words
 * an XDK interrupt DPC loops on (plain RAM here, cleared only by the
 * runtime's ack threads), the last kernel ordinal the DPC called, and the
 * two innermost words on its guest stack that look like code addresses (a
 * rough call stack: a stale word can show up). Identical samples are
 * tallied; the top few are printed with the table. */
extern ptrdiff_t g_xbox_mem_offset;
extern RECOMP_TLS uint32_t g_esp;

#define SAMPLE_AFTER_US 20000

/* A stack word is a return address when it lies in the image's executable
 * sections (g_xbox_code_lo/hi, set as the XBE's sections are mapped); before
 * they are known, anywhere in the image. */
static int guest_code_word(uint32_t w)
{
    uint32_t lo = g_xbox_code_lo, hi = g_xbox_code_hi;
    if (hi <= lo) { lo = g_xbox_image_lo; hi = g_xbox_image_hi; }
    return hi > lo && w >= lo && w < hi;
}

static volatile uint32_t s_watch_routine;
static volatile long long s_watch_t0;
static volatile uint32_t *volatile s_watch_esp;
static volatile uint32_t s_watch_esp0;
static volatile DWORD s_watch_tid;
static volatile unsigned s_watch_ord;

typedef struct {
    uint32_t routine, pmc, pcrtc, pgraph, pgstat, pfifo, ra1, ra2, ord;
    long long n;
} Sample;
#define SAMPLE_SLOTS 64
static Sample s_samples[SAMPLE_SLOTS];
static long long s_samples_total, s_samples_lost;

static void sampler_watch(uint32_t routine)
{
    s_watch_tid = GetCurrentThreadId();
    s_watch_esp = &g_esp;
    s_watch_esp0 = g_esp;
    s_watch_ord = 0;
    s_watch_t0 = xbox_ProfNowUs();
    s_watch_routine = routine;
}

void xbox_ProfKernelCall(unsigned ordinal)
{
    if (t_current && s_watch_tid == GetCurrentThreadId())
        s_watch_ord = ordinal;
}

static uint32_t nv2a_word(uint32_t off)
{
    return *(volatile uint32_t *)((uintptr_t)(0xFD000000u + off)
                                  + g_xbox_mem_offset);
}

static DWORD WINAPI sampler_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sample k;
        uint32_t esp, top, a;
        int i, found = 0;

        Sleep(1);
        k.routine = s_watch_routine;
        if (!k.routine || xbox_ProfNowUs() - s_watch_t0 < SAMPLE_AFTER_US)
            continue;
        memset(&k, 0, sizeof k);
        k.routine = s_watch_routine;
        k.pmc = nv2a_word(0x000100);
        k.pcrtc = nv2a_word(0x600100);
        k.pgraph = nv2a_word(0x400100);
        k.pgstat = nv2a_word(0x400700);
        k.pfifo = nv2a_word(0x002100);
        k.ord = s_watch_ord;
        esp = *s_watch_esp;
        top = s_watch_esp0;
        for (a = esp; a < top && a - esp < 4096 && found < 2; a += 4) {
            uint32_t w = *(volatile uint32_t *)((uintptr_t)a + g_xbox_mem_offset);
            if (guest_code_word(w)) {
                if (found++ == 0) k.ra1 = w; else k.ra2 = w;
            }
        }
        prof_lock();
        s_samples_total++;
        for (i = 0; i < SAMPLE_SLOTS && s_samples[i].n; i++) {
            Sample *s = &s_samples[i];
            if (s->routine == k.routine && s->pmc == k.pmc && s->pcrtc == k.pcrtc
                && s->pgraph == k.pgraph && s->pgstat == k.pgstat
                && s->pfifo == k.pfifo && s->ra1 == k.ra1 && s->ra2 == k.ra2
                && s->ord == k.ord)
                break;
        }
        if (i == SAMPLE_SLOTS) {
            s_samples_lost++;
        } else {
            if (!s_samples[i].n) { s_samples[i] = k; }
            s_samples[i].n++;
        }
        LeaveCriticalSection(&s_lock);
    }
    return 0;
}

static void sampler_start(void)
{
    CloseHandle(CreateThread(NULL, 0, sampler_thread, NULL, 0, NULL));
}

static void sampler_report(void)
{
    Sample snap[SAMPLE_SLOTS];
    long long total, lost;
    int i, j, shown;

    prof_lock();
    memcpy(snap, s_samples, sizeof snap);
    memset(s_samples, 0, sizeof s_samples);
    total = s_samples_total; lost = s_samples_lost;
    s_samples_total = s_samples_lost = 0;
    LeaveCriticalSection(&s_lock);
    if (!total)
        return;
    fprintf(stderr, "  [DPCPROF] long-DPC samples %lld (1/ms past 20 ms; %lld not tallied)\n",
            total, lost);
    for (shown = 0; shown < 6; shown++) {
        int best = -1;
        for (j = 0; j < SAMPLE_SLOTS; j++)
            if (snap[j].n && (best < 0 || snap[j].n > snap[best].n))
                best = j;
        if (best < 0)
            break;
        i = best;
        fprintf(stderr, "  [DPCPROF]   %6lld x %08X pmc %08X pcrtc %08X pgraph %08X"
                " pgstat %08X pfifo %08X ord %u ra %08X %08X\n",
                snap[i].n, snap[i].routine, snap[i].pmc, snap[i].pcrtc,
                snap[i].pgraph, snap[i].pgstat, snap[i].pfifo, snap[i].ord,
                snap[i].ra1, snap[i].ra2);
        snap[i].n = 0;
    }
}

void *xbox_ProfBegin(int kind, uint32_t routine)
{
    ProfEntry *prev = t_current;
    int i;

    prof_lock();
    for (i = 0; i < s_used; i++)
        if (s_entries[i].kind == kind && s_entries[i].routine == routine)
            break;
    if (i == s_used) {
        if (s_used == PROF_ENTRIES) {
            s_overflow++;
            LeaveCriticalSection(&s_lock);
            return prev;
        }
        memset(&s_entries[i], 0, sizeof s_entries[i]);
        s_entries[i].kind = kind;
        s_entries[i].routine = routine;
        s_used++;
    }
    LeaveCriticalSection(&s_lock);
    t_current = &s_entries[i];
    if (!prev && kind <= XBOX_PROF_DPC_QUEUE)
        sampler_watch(routine);
    return prev;
}

void xbox_ProfEnd(void *prev, long long run_us)
{
    ProfEntry *e = t_current;

    if (e) {
        prof_lock();
        sum_add(&e->run, run_us);
        LeaveCriticalSection(&s_lock);
    }
    if (!prev && s_watch_tid == GetCurrentThreadId())
        s_watch_routine = 0;
    t_current = (ProfEntry *)prev;
}

void xbox_ProfWait(int what, long long us)
{
    ProfEntry *e = t_current;

    prof_lock();
    sum_add(e ? &e->wait[what] : &s_outside[what], us);
    LeaveCriticalSection(&s_lock);
}

void xbox_ProfCount(int counter, long long v)
{
    prof_lock();
    sum_add(&s_counters[counter], v);
    LeaveCriticalSection(&s_lock);
}

static const char *const s_kind_name[XBOX_PROF_KINDS] = {
    "Tdpc", "Qdpc", "vblISR", "apuISR", "usbISR", "sync"
};

/* Top routines by run time plus gate wait: a DPC that waits 300 ms for the
 * gate and then runs for 1 ms held the timer thread for 301. */
static long long entry_cost(const ProfEntry *e)
{
    return e->run.tot + e->wait[XBOX_PROF_WAIT_GATE].tot;
}

void xbox_ProfReport(void)
{
    ProfEntry snap[PROF_ENTRIES];
    ProfSum outside[XBOX_PROF_WAITS], counters[XBOX_PROF_COUNTERS];
    int used, overflow, i, j, shown;

    prof_lock();
    used = s_used;
    overflow = s_overflow;
    memcpy(snap, s_entries, sizeof(ProfEntry) * (size_t)used);
    memcpy(outside, s_outside, sizeof outside);
    memcpy(counters, s_counters, sizeof counters);
    for (i = 0; i < used; i++) {
        memset(&s_entries[i].run, 0, sizeof s_entries[i].run);
        memset(s_entries[i].wait, 0, sizeof s_entries[i].wait);
    }
    memset(s_outside, 0, sizeof s_outside);
    memset(s_counters, 0, sizeof s_counters);
    s_overflow = 0;
    LeaveCriticalSection(&s_lock);

    fprintf(stderr, "  [DPCPROF] timer loops %lld, sleep over by %lld ms (max %lld),"
            " vblank restarts %lld, dpc queue max %lld, drains gate-busy %lld,"
            " se_frame %lld x %.3f ms (max %.3f)\n",
            counters[XBOX_PROF_N_LOOPS].n,
            counters[XBOX_PROF_SLEEP_OVER].tot / 1000,
            counters[XBOX_PROF_SLEEP_OVER].max / 1000,
            counters[XBOX_PROF_SCHED_RESTART].n,
            counters[XBOX_PROF_DPCQ_DEPTH].max,
            counters[XBOX_PROF_DRAIN_BUSY].n,
            counters[XBOX_PROF_SE_FRAME].n,
            counters[XBOX_PROF_SE_FRAME].n
                ? (double)counters[XBOX_PROF_SE_FRAME].tot
                  / (double)counters[XBOX_PROF_SE_FRAME].n / 1000.0 : 0.0,
            (double)counters[XBOX_PROF_SE_FRAME].max / 1000.0);
    fprintf(stderr, "  [DPCPROF] waits off any routine (n/total/max ms):"
            " gate %lld/%.1f/%.1f apulock %lld/%.1f/%.1f stall %lld/%.1f/%.1f%s\n",
            outside[XBOX_PROF_WAIT_GATE].n,
            outside[XBOX_PROF_WAIT_GATE].tot / 1000.0,
            outside[XBOX_PROF_WAIT_GATE].max / 1000.0,
            outside[XBOX_PROF_WAIT_APULOCK].n,
            outside[XBOX_PROF_WAIT_APULOCK].tot / 1000.0,
            outside[XBOX_PROF_WAIT_APULOCK].max / 1000.0,
            outside[XBOX_PROF_WAIT_STALL].n,
            outside[XBOX_PROF_WAIT_STALL].tot / 1000.0,
            outside[XBOX_PROF_WAIT_STALL].max / 1000.0,
            overflow ? " (table full: some routines not tracked)" : "");

    /* Selection sort of the top eight by cost; the table is small. */
    for (shown = 0; shown < 8 && shown < used; shown++) {
        int best = shown;
        ProfEntry tmp;
        for (j = shown + 1; j < used; j++)
            if (entry_cost(&snap[j]) > entry_cost(&snap[best]))
                best = j;
        if (entry_cost(&snap[best]) == 0 && snap[best].run.n == 0)
            break;
        tmp = snap[shown]; snap[shown] = snap[best]; snap[best] = tmp;
        fprintf(stderr, "  [DPCPROF]   %-6s %08X n %6lld run %8.1f ms max %7.1f"
                " | gate %8.1f max %7.1f | apulock %7.1f max %6.1f (n %lld)"
                " | stall %6.1f\n",
                s_kind_name[snap[shown].kind], snap[shown].routine,
                snap[shown].run.n, snap[shown].run.tot / 1000.0,
                snap[shown].run.max / 1000.0,
                snap[shown].wait[XBOX_PROF_WAIT_GATE].tot / 1000.0,
                snap[shown].wait[XBOX_PROF_WAIT_GATE].max / 1000.0,
                snap[shown].wait[XBOX_PROF_WAIT_APULOCK].tot / 1000.0,
                snap[shown].wait[XBOX_PROF_WAIT_APULOCK].max / 1000.0,
                snap[shown].wait[XBOX_PROF_WAIT_APULOCK].n,
                snap[shown].wait[XBOX_PROF_WAIT_STALL].tot / 1000.0);
    }
    sampler_report();
    fflush(stderr);
}
