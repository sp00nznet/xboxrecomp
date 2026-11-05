/**
 * Function tracing for recompiled code.
 *
 * recomp_types.h declares these and tools.recomp emits calls to them under
 * --trace-functions, but until now nothing defined them: the definitions lived
 * in one game project, so enabling tracing anywhere else failed at link time
 * with three unresolved symbols and no hint that the fix was to go and copy a
 * file. They belong with the runtime that declares them.
 *
 * Output goes to stderr, unbuffered, because the question tracing answers is
 * usually "what was the last thing that happened before it died".
 */

#include <stdio.h>
#include "recomp_env.h"
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#include "xbox_memory_layout.h"

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;

/* A run that recurses produces trace lines without limit, and the useful
 * window is rarely the first few thousand. The budget stops a diagnostic from
 * filling a disk, and is deliberately generous: a budget that runs out before
 * the interesting part turns "no trace here" into a false negative, which is
 * worse than a large file. Override with RECOMP_TRACE_BUDGET. */
static long trace_budget(void)
{
    static long budget = -1;
    if (budget < 0) {
        const char *env = recomp_env(RENV_TRACE_BUDGET);
        budget = env ? strtol(env, NULL, 0) : 400000;
        if (budget < 0) budget = 0;
    }
    return budget > 0 ? budget-- : 0;
}

/* Where a title spends its calls.
 *
 * A recompiled title that is CPU-bound gives no clue which guest code is
 * responsible: the native profile is a wall of sub_XXXX, and the guest has no
 * program counter to sample. Counting entries does answer it, and the trace
 * hook is already on every function the run was generated to trace -- so
 * tracing everything and tallying instead of printing turns the existing
 * mechanism into a profile for the cost of an array increment.
 *
 * Counts, not time: a function called once that loops for a minute does not
 * appear here, and one called ten million times cheaply does. It says where
 * the calls go, which is the first question, not the last.
 *
 * Enable with RECOMP_TRACE_PROFILE=1. The report goes to stderr at exit,
 * hottest first.
 */
#define PROF_SLOTS 8192                 /* open addressing, power of two */

static struct { uint32_t va; unsigned long long hits; } g_prof[PROF_SLOTS];
static const char *g_prof_name[PROF_SLOTS];
static int g_prof_used, g_prof_full;
static unsigned long long g_prof_calls;

static void prof_report(void)
{
    int taken[40], ntaken = 0;
    int i, j, shown;

    if (!g_prof_used)
        return;
    fprintf(stderr, "\n[PROFILE] %d functions entered%s, hottest first:\n",
            g_prof_used, g_prof_full ? " (table full, some dropped)" : "");
    for (shown = 0; shown < 40; shown++) {
        int best = -1;
        for (i = 0; i < PROF_SLOTS; i++) {
            if (!g_prof[i].hits)
                continue;
            for (j = 0; j < ntaken; j++)
                if (taken[j] == i)
                    break;
            if (j < ntaken)
                continue;
            if (best < 0 || g_prof[i].hits > g_prof[best].hits)
                best = i;
        }
        if (best < 0)
            break;
        taken[ntaken++] = best;
        fprintf(stderr, "  %14llu  %s (0x%08X)\n",
                g_prof[best].hits, g_prof_name[best] ? g_prof_name[best] : "?",
                g_prof[best].va);
    }
    fflush(stderr);
}

/* RECOMP_TRACE_PROFILE=1 profiles; a larger number is also how often to
 * report, in calls. The default suits a title burning a core in a spin loop;
 * a title that no longer has one may never reach it, and then the only report
 * is the one at exit -- which a killed run never gets. */
static unsigned long long prof_interval(void)
{
    static unsigned long long every;
    if (!every) {
        const char *v = recomp_env(RENV_TRACE_PROFILE);
        unsigned long long n = v ? strtoull(v, NULL, 0) : 0;
        every = n > 1 ? n : 20000000ull;
    }
    return every;
}

static int prof_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        on = recomp_env(RENV_TRACE_PROFILE) ? 1 : 0;
        if (on)
            atexit(prof_report);
    }
    return on;
}

static void prof_count(const char *name, uint32_t va)
{
    unsigned i = (va * 2654435761u) & (PROF_SLOTS - 1);
    unsigned n;

    /* A title being profiled for a hang or a slowdown is a title that gets
     * killed rather than exited, and a kill does not reach atexit. Report as
     * it goes, so there is always a recent one. */
    if (++g_prof_calls % prof_interval() == 0)
        prof_report();

    for (n = 0; n < PROF_SLOTS; n++) {
        unsigned k = (i + n) & (PROF_SLOTS - 1);
        if (g_prof[k].va == va && g_prof[k].hits) { g_prof[k].hits++; return; }
        if (!g_prof[k].hits) {
            g_prof[k].va = va;
            g_prof[k].hits = 1;
            g_prof_name[k] = name;
            g_prof_used++;
            return;
        }
    }
    g_prof_full = 1;
}

void recomp_trace_enter(const char *name, uint32_t va)
{
    if (prof_enabled()) { prof_count(name, va); return; }
    if (!trace_budget()) return;
    /* The return address as well as the registers: at entry it is still at
     * [esp], and it names the call site, which is the thing a trace of "who
     * reached this" actually needs. Reading a guest stack dump for it works
     * only when the frames above are still live. */
    fprintf(stderr, "[TRACE] -> %s (0x%08X)  from=%08X esp=%08X eax=%08X "
            "ecx=%08X esi=%08X edi=%08X ebx=%08X\n",
            name, va,
            *(const uint32_t *)((uintptr_t)g_esp + xbox_GetMemoryOffset()),
            g_esp, g_eax, g_ecx, g_esi, g_edi, g_ebx);

    /* The stack arguments too, when asked. Registers alone do not say which
     * argument arrived null, and for a function with a long argument list,
     * counting pushes back from the call site is guesswork. */
    if (recomp_env(RENV_TRACE_ARGS)) {
        const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
        int n = atoi(recomp_env(RENV_TRACE_ARGS));
        int i;

        if (n <= 0 || n > 32)
            n = 8;
        fprintf(stderr, "         args:");
        for (i = 1; i <= n; i++)
            fprintf(stderr, " %d=%08X", i,
                    *(const uint32_t *)(mem + g_esp + i * 4));
        fprintf(stderr, "\n");
        /* And the object eax points at. A matrix of NaNs says the maths went
         * wrong; whether its inputs were already zero says whether the maths
         * is at fault or the data behind it was never built. */
        /* Follow the pointer arguments one level. A matrix that arrives as
         * NaN was copied from somewhere, and the object it came from is what
         * needs looking at -- the value alone says only that it is wrong. */
        if (recomp_env(RENV_TRACE_DEREF)) {
            for (i = 1; i <= n; i++) {
                uint32_t a = *(const uint32_t *)(mem + g_esp + i * 4);
                int k;
                if (a < 0x00010000u || a >= 0x04000000u)
                    continue;
                fprintf(stderr, "         arg%d -> [%08X]:", i, a);
                for (k = 0; k < 12; k++)
                    fprintf(stderr, " %08X",
                            *(const uint32_t *)(mem + a + k * 4));
                fprintf(stderr, "\n");
            }
        }
        if (g_eax > 0x00010000u && g_eax < 0x04000000u) {
            fprintf(stderr, "         [eax=%08X]:", g_eax);
            for (i = 0; i < 24; i++)
                fprintf(stderr, " %08X",
                        *(const uint32_t *)(mem + g_eax + i * 4));
            fprintf(stderr, "\n");
        }
    }
    fflush(stderr);
}

/* Entry values answer "what was it called with"; only exit values answer "what
 * did the caller get back", which is the question when a callee-saved register
 * comes back wrong. */
void recomp_trace_exit(const char *name, uint32_t va)
{
    if (prof_enabled()) return;     /* entries alone carry the count */
    if (!trace_budget()) return;
    fprintf(stderr, "[TRACE] <- %s (0x%08X)  esp=%08X eax=%08X ecx=%08X "
            "esi=%08X edi=%08X ebx=%08X\n",
            name, va, g_esp, g_eax, g_ecx, g_esi, g_edi, g_ebx);
    fflush(stderr);
}

/* esp at a specific point inside a traced function. The epilogue's
 * `mov esp, ebp` hides drift from any return-time sample, so a leak of a few
 * bytes per call is only visible from a sample taken before it. */
void recomp_trace_esp(const char *name, const char *tag)
{
    if (prof_enabled()) return;
    if (!trace_budget()) return;
    fprintf(stderr, "[ESP] %s @%s  esp=%08X esi=%08X edi=%08X\n",
            name, tag, g_esp, g_esi, g_edi);
    fflush(stderr);
}

/* ---------------------------------------------------------------------------
 * Guest debug output (INT 2D / DebugService).
 *
 * The Xbox kernel debug trap. eax selects the service and ecx carries its
 * argument; service 1 is "print this ANSI_STRING", which is what
 * OutputDebugStringA and the XDK's DbgPrint compile down to. On hardware the
 * kernel consumes the trap and resumes at the int3 that follows, skipping it.
 *
 * Printing it is the whole point: this is the title telling us what it thinks
 * is happening, and during bring-up that is the most valuable output there is.
 * ------------------------------------------------------------------------- */

/* Defined in xbox_memory_layout.c; declared extern per consumer, as
 * kernel_bridge.c and nv2a_pb_replay.c already do. */
extern ptrdiff_t g_xbox_mem_offset;

void recomp_debug_service(uint32_t service, uint32_t arg_va)
{
    const uint8_t *mem = (const uint8_t *)g_xbox_mem_offset;
    uint16_t length;
    uint32_t buffer_va;

    if (service != 1) {
        fprintf(stderr, "[GUEST] DebugService %u (arg 0x%08X), ignored\n",
                (unsigned)service, arg_va);
        fflush(stderr);
        return;
    }

    /* ANSI_STRING { USHORT Length; USHORT MaximumLength; PCHAR Buffer; } */
    if (!arg_va)
        return;
    length    = *(const uint16_t *)(mem + arg_va);
    buffer_va = *(const uint32_t *)(mem + arg_va + 4);
    if (!buffer_va || !length)
        return;

    fprintf(stderr, "[GUEST] %.*s", (int)length, (const char *)(mem + buffer_va));
    if (length && ((const char *)(mem + buffer_va))[length - 1] != '\n')
        fputc('\n', stderr);
    fflush(stderr);
}
