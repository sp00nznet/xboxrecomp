/*
 * kernel_suspend_trace.c - RECOMP_TRACE=suspend[=va]: the title's thread
 * suspends and resumes, once a second.
 *
 *   [SUSPEND] t=<s> [<va>]=<dword> | <target> S+<n>=<total> prev <lo>-<hi>
 *             @<dword> R+<n>=<total> prev <lo>-<hi> @<dword> | <target> ...
 *
 * One line a second, every target any suspend or resume has named (the
 * guest's handle, or the thread object for the Ke forms; `self` for the
 * current-thread pseudo handle). +n is that second's calls, =total the
 * run's; prev is the range of previous suspend counts the calls returned in
 * the second ("-" when none); @ is the watched dword at the last such call.
 * A failed call adds !<n>. With a va, the line also carries the dword there
 * when it is printed, so a long stretch with no calls still shows it.
 *
 * Why: CRI ADX (JSRF) resumes its lock helper at lock depth 0->1 and
 * suspends it at 1->0. The lifted inc and dec of the depth are plain
 * read-modify-writes, and the lock tests the depth three kernel calls before
 * it increments it; on one host core two guest threads can be preempted in
 * between, which the console's single CPU with exclusive priorities ruled
 * out. A lost decrement leaves the depth at 1 or more and no unlock suspends
 * the helper again. The watched dword with these counts shows whether that
 * is what a long spin is.
 *
 * Lock-free on purpose: the caller of a suspend can itself be suspended by
 * another guest thread at any point, and a lock it held then would stop the
 * next caller (and this report) for good. Slots are claimed by CAS and
 * counters are interlocked; the report reads them unsynchronised, a
 * snapshot. Off, the bridges test one cached word.
 */

#include "kernel.h"
#include "recomp_env.h"
#include "xbox_memory_layout.h"   /* xbox_PeekRead32 */
#include "platform/host_time.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* JSRF names 4 targets; a title with a thread pool may name dozens. */
#define ST_SLOTS 64
#define ST_PREV_CAP 0x7FFFL

typedef struct {
    volatile LONG n;          /* calls, the run's */
    volatile LONG fail;
    /* The prev range this second in one word, so the report takes it with
     * one exchange and a call's prev lands in this line or the next, never
     * in neither. Both halves are kept by max so that zero (the static start
     * and the report's reset) means none: bits 0-15 hold prev + 1, bits
     * 16-30 ST_PREV_CAP - prev. Counts never pass 127 (MAXIMUM_SUSPEND_COUNT). */
    volatile LONG range;
    volatile LONG at;         /* watched dword at the last call */
    volatile LONG at_ok;      /* at was read at least once */
    LONG n_seen, fail_seen;   /* the report's: totals at the last line */
} StKind;

typedef struct {
    volatile LONG key;        /* the target's guest word; 0 = free */
    StKind k[2];              /* XBOX_SUSPEND_TRACE_SUSPEND, _RESUME */
} StSlot;

static int s_on = -1;
static uint32_t s_va;
static StSlot s_slot[ST_SLOTS];
static volatile LONG s_slot_full;
static uint64_t s_t0;
static volatile LONG s_started;

int xbox_SuspendTraceOn(void)
{
    if (s_on < 0) {
        const char *v = recomp_env(RENV_SUSPEND_TRACE);
        unsigned long va = 0;

        /* "suspend" alone is the value "1", not an address. */
        if (v && strcmp(v, "1") != 0)
            va = strtoul(v, NULL, 0);
        s_va = (uint32_t)va;
        s_on = v ? 1 : 0;
    }
    return s_on;
}

uint32_t xbox_SuspendTraceVa(void)
{
    return xbox_SuspendTraceOn() ? s_va : 0;
}

static LONG st_watched(int *ok)
{
    uint32_t v = 0;

    *ok = s_va && xbox_PeekRead32(s_va, &v);
    return (LONG)v;
}

static void st_range(volatile LONG *p, uint32_t prev)
{
    LONG hi1 = (LONG)prev + 1, lo1 = ST_PREV_CAP - (LONG)prev;
    LONG cur = *p;

    for (;;) {
        LONG h = cur & 0xFFFF, l = (cur >> 16) & 0x7FFF;
        LONG want = (hi1 > h ? hi1 : h) | ((lo1 > l ? lo1 : l) << 16);
        LONG seen;

        if (want == cur)
            break;
        seen = InterlockedCompareExchange(p, want, cur);
        if (seen == cur)
            break;
        cur = seen;
    }
}

void xbox_SuspendTraceNote(int kind, uint32_t target, NTSTATUS status,
                           uint32_t prev)
{
    StSlot *s = NULL;
    StKind *k;
    LONG key = (LONG)target;
    int i, ok;

    if (!xbox_SuspendTraceOn() || !target || (kind != 0 && kind != 1))
        return;
    for (i = 0; i < ST_SLOTS && !s; i++) {
        LONG cur = s_slot[i].key;
        if (!cur)   /* free: claim it, unless another caller just did */
            cur = InterlockedCompareExchange(&s_slot[i].key, key, 0);
        if (!cur || cur == key)
            s = &s_slot[i];
    }
    if (!s) {
        if (!InterlockedExchange(&s_slot_full, 1))
            fprintf(stderr, "[SUSPEND] more than %d targets; 0x%08x and"
                    " later ones are not counted\n", ST_SLOTS, target);
        return;
    }
    k = &s->k[kind];
    if (status != STATUS_SUCCESS) {
        InterlockedIncrement(&k->fail);
        return;
    }
    InterlockedIncrement(&k->n);
    if (prev < (uint32_t)ST_PREV_CAP)
        st_range(&k->range, prev);
    {
        LONG w = st_watched(&ok);
        if (ok) {
            InterlockedExchange(&k->at, w);
            InterlockedExchange(&k->at_ok, 1);
        }
    }
}

static size_t st_put(char *buf, size_t n, size_t at, const char *fmt, ...)
{
    va_list ap;
    int w;

    if (at >= n)
        return at;
    va_start(ap, fmt);
    w = vsnprintf(buf + at, n - at, fmt, ap);
    va_end(ap);
    if (w < 0)
        return at;
    return at + (size_t)w < n ? at + (size_t)w : n - 1;
}

size_t xbox_SuspendTraceFormat(char *buf, size_t n, uint32_t t_s)
{
    static const char kc[2] = { 'S', 'R' };
    size_t at = 0;
    int i, j, ok;

    if (!n)
        return 0;
    buf[0] = 0;
    at = st_put(buf, n, at, "[SUSPEND] t=%u", t_s);
    if (s_va) {
        LONG w = st_watched(&ok);
        if (ok)
            at = st_put(buf, n, at, " [%08X]=%08X", s_va, (uint32_t)w);
        else
            at = st_put(buf, n, at, " [%08X]=??", s_va);
    }
    for (i = 0; i < ST_SLOTS; i++) {
        StSlot *s = &s_slot[i];
        LONG key = s->key;

        if (!key)
            continue;
        if ((uint32_t)key == 0xFFFFFFFEu)
            at = st_put(buf, n, at, " | self");
        else
            at = st_put(buf, n, at, " | %08X", (uint32_t)key);
        for (j = 0; j < 2; j++) {
            StKind *k = &s->k[j];
            LONG tot = k->n, fail = k->fail;
            LONG range = InterlockedExchange(&k->range, 0);
            LONG hi1 = range & 0xFFFF, lo1 = (range >> 16) & 0x7FFF;

            at = st_put(buf, n, at, " %c+%ld=%ld", kc[j],
                        (long)(tot - k->n_seen), (long)tot);
            if (hi1 && lo1)
                at = st_put(buf, n, at, " prev %ld-%ld",
                            (long)(ST_PREV_CAP - lo1), (long)(hi1 - 1));
            else
                at = st_put(buf, n, at, " prev -");
            if (s_va && tot && k->at_ok)
                at = st_put(buf, n, at, " @%lX", (unsigned long)(uint32_t)k->at);
            else if (s_va && tot)
                at = st_put(buf, n, at, " @??");
            if (fail != k->fail_seen)
                at = st_put(buf, n, at, " !%ld", (long)(fail - k->fail_seen));
            k->n_seen = tot;
            k->fail_seen = fail;
        }
    }
    return at;
}

static DWORD WINAPI st_report_thread(LPVOID unused)
{
    char line[XBOX_SUSPEND_TRACE_LINE];

    (void)unused;
    xbox_log_thread_role("suspend-trace", 0);
    for (;;) {
        Sleep(1000);
        xbox_SuspendTraceFormat(line, sizeof line,
                                (uint32_t)((xbox_HostNowNs() - s_t0) / 1000000000ull));
        fprintf(stderr, "%s\n", line);
        fflush(stderr);
    }
    return 0;
}

/* From the bridge init: guest memory is mapped by then, so the first line
 * can already read the watched dword. Once per process; the counts are the
 * process's, across resets too (a diagnostic, not guest state). */
void xbox_SuspendTraceStart(void)
{
    HANDLE h;

    /* The key itself, not the cached word: the ctest sets it after the
     * bridge init, to count with no reporter taking the lines from it. */
    if (!recomp_env(RENV_SUSPEND_TRACE) || !xbox_SuspendTraceOn()
            || InterlockedExchange(&s_started, 1))
        return;
    s_t0 = xbox_HostNowNs();
    fprintf(stderr, "[SUSPEND] trace on: per-target NtSuspendThread/"
            "NtResumeThread (and Ke forms) counts once a second%s",
            s_va ? "" : "\n");
    if (s_va)
        fprintf(stderr, ", watching the dword at 0x%08X\n", s_va);
    h = CreateThread(NULL, 0, st_report_thread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
}
