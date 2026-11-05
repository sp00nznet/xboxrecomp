/*
 * The kernel bridge.
 *
 * A title does not call the kernel by name. Its indirect calls land on a
 * thunk table that xbox_kernel_bridge_init() has rewritten to synthetic VAs,
 * and kernel_thunk_dispatch turns each of those into a bridge_* function.
 * That is the path the runtime actually executes, and it is easy to leave
 * untested: bridge_* functions are static and read guest CPU state rather
 * than taking arguments, so they cannot be called directly.
 *
 * They are reachable all the same. recomp_lookup_kernel is not static: given
 * a buffer standing in for guest memory it selects the slot and hands back
 * the dispatcher. tests/kernel_directory uses the same seam for the directory
 * ABI; this one covers kernel memory ordinals.
 *
 * Ordinal 255 must also write a non-zero id through its ThreadId argument;
 * tests/kernel_thread_ids checks ids on real worker threads.
 *
 * Checked here with ordinal 173, MmGetPhysicalAddress, which had two
 * implementations that disagreed -- the bridge translating through the
 * contiguous window and xbox_MmGetPhysicalAddress returning its argument
 * unchanged. The bridge now calls the latter, and that delegation is what
 * these checks pin: asserting only on xbox_MmGetPhysicalAddress leaves a
 * bridge that stops delegating entirely green.
 */
/* kernel.h brings the NT type vocabulary: <windows.h> on Windows, the
 * shim in platform/xbox_winnt.h elsewhere. Nothing below is
 * platform-specific. */
#include "kernel.h"
#include "recomp_env.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; a regression harness has no title, and a
 * static archive resolves only what gets pulled in. Nothing here calls a
 * guest function. */
typedef void (*recomp_func_t)(void);
/* The title's main routine, "generated code" at MAIN_VA: the first
 * PsCreateSystemThreadEx runs it inline, and with RECOMP_GUEST_LOCK on it
 * must hold the guest CPU (the call it runs inside let the CPU go). */
#define MAIN_VA 0x30000u
static int s_main_ran, s_main_held;
static void main_routine(void)
{
    s_main_ran = 1;
    s_main_held = xbox_GuestCpuHeld();
}
/* A worker at WORKER_VA, run inline (RECOMP_WORKERS=inline) by ordinal
 * 254: bridge_run_thread_inline holds the guest CPU for it. */
#define WORKER_VA 0x30100u
static int s_worker_ran, s_worker_held;
static void worker_routine(void)
{
    s_worker_ran = 1;
    s_worker_held = xbox_GuestCpuHeld();
}
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va)
{
    return xbox_va == MAIN_VA ? main_routine
         : xbox_va == WORKER_VA ? worker_routine : NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

/* The seam, plus the guest CPU state the dispatcher reads and writes. */
extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-58s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    const uint32_t THUNK_VA = 0x10000, STACK_VA = 0x20000;

    uint8_t *mem = VirtualAlloc(NULL, 16 * 1024 * 1024,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(mem != NULL, "guest memory stand-in allocated", NULL);
    if (!mem) {
        printf("\n%d failure(s); the rest needs guest memory.\n", failures);
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)mem;

    /* The imports, in the form an XBE stores them: 0x80000000 | ordinal. */
    *(uint32_t *)(mem + THUNK_VA) = 0x80000000u | 173u;
    *(uint32_t *)(mem + THUNK_VA + 4) = 0x80000000u | 197u;
    *(uint32_t *)(mem + THUNK_VA + 8) = 0x80000000u | 255u;
    *(uint32_t *)(mem + THUNK_VA + 12) = 0x80000000u | 254u;
    *(uint32_t *)(mem + THUNK_VA + 16) = 0x80000000u | 143u;
    *(uint32_t *)(mem + THUNK_VA + 20) = 0x80000000u | 124u;
    xbox_kernel_set_thunk_address(THUNK_VA, 6);
    /* The lock on before init: its join makes this thread the guest main. */
    recomp_env_set(RENV_GUEST_LOCK, "1");
    xbox_GuestCpuReloadConfig();
    xbox_kernel_bridge_init();
    check(xbox_GuestCpuHeld(), "bridge init joined the guest CPU (lock on)", NULL);

    /* bridge_init rewrites the entry in place to a synthetic VA. */
    recomp_func_t fn = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA));
    check(fn != NULL, "ordinal 173 resolves to a bridge entry point",
          "the thunk entry was not rewritten to a synthetic VA");

    if (fn) {
        /* stdcall: the guest pushed a return address, then the argument. */
        uint32_t *sp = (uint32_t *)(mem + STACK_VA);
        sp[0] = 0xBEEF0001u;          /* guest return address */
        sp[1] = 0x80001000u;          /* inside the contiguous window */
        g_esp = STACK_VA;
        g_eax = 0xDEADBEEFu;
        fn();

        char detail[128];
        snprintf(detail, sizeof detail,
                 "bridge returned 0x%08X, expected 0x00001000", g_eax);
        check(g_eax == 0x00001000u,
              "ordinal 173 translates through the contiguous window", detail);

        /* And the argument must come off the guest stack. A wrong
         * stdcall_args_for_ordinal entry corrupts the caller's frame away
         * from the call, with nothing naming the ordinal. */
        snprintf(detail, sizeof detail, "esp is 0x%08X, expected 0x%08X",
                 g_esp, STACK_VA + 8);
        check(g_esp == STACK_VA + 8,
              "ordinal 173 pops its stdcall argument", detail);
    }

    /* Ordinal 197, NtDuplicateObject(Source, &Target, Options), on the
     * guest's pseudo-handles. They are 32-bit -1 (current process) and -2
     * (current thread); widened without sign extension they are no handle
     * at all, and the duplicate fails (Windows) or faults (POSIX shim). A
     * title duplicating NtCurrentThread() to keep a handle to itself is the
     * case that found this. */
    fn = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 4));
    check(fn != NULL, "ordinal 197 resolves to a bridge entry point", NULL);
    if (fn) {
        static const struct { uint32_t src; const char *what; } pseudo[] = {
            { 0xFFFFFFFEu, "NtDuplicateObject(NtCurrentThread()) succeeds" },
            { 0xFFFFFFFFu, "NtDuplicateObject(NtCurrentProcess()) succeeds" },
        };
        const uint32_t TARGET_VA = 0x30000;
        for (size_t k = 0; k < sizeof pseudo / sizeof pseudo[0]; k++) {
            uint32_t *sp = (uint32_t *)(mem + STACK_VA);
            sp[0] = 0xBEEF0002u;      /* guest return address */
            sp[1] = pseudo[k].src;    /* SourceHandle */
            sp[2] = TARGET_VA;        /* TargetHandle (out) */
            sp[3] = 0x2u;             /* DUPLICATE_SAME_ACCESS */
            *(uint32_t *)(mem + TARGET_VA) = 0;
            g_esp = STACK_VA;
            g_eax = 0xDEADBEEFu;
            fn();

            char detail[128];
            snprintf(detail, sizeof detail,
                     "status 0x%08X, target 0x%08X", g_eax,
                     *(uint32_t *)(mem + TARGET_VA));
            check(g_eax == 0 && *(uint32_t *)(mem + TARGET_VA) != 0,
                  pseudo[k].what, detail);
            snprintf(detail, sizeof detail, "esp is 0x%08X, expected 0x%08X",
                     g_esp, STACK_VA + 16);
            check(g_esp == STACK_VA + 16,
                  "ordinal 197 pops its three stdcall arguments", detail);
        }
    }

    /* PsCreateSystemThreadEx #1 runs the title's main routine inline. */
    fn = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 8));
    check(fn != NULL, "ordinal 255 resolves to a bridge entry point", NULL);
    if (fn) {
        uint32_t *sp = (uint32_t *)(mem + STACK_VA);
        memset(sp, 0, 11 * 4);
        sp[0] = 0xBEEF0003u;          /* guest return address */
        sp[1] = 0x40000u;             /* ThreadHandle out */
        sp[5] = 0x40100u;             /* ThreadId out (arg 4) */
        sp[10] = MAIN_VA;             /* StartRoutine (arg 9) */
        *(uint32_t *)(mem + 0x40100u) = 0;
        g_esp = STACK_VA;
        fn();
        check(s_main_ran, "ordinal 255's first call ran the main routine inline", NULL);
        check(*(uint32_t *)(mem + 0x40100u) != 0,
              "ordinal 255 writes the main thread's id (non-zero)",
              "ThreadId was left 0");
        check(s_main_held == 1, "the main routine held the guest CPU", NULL);
        check(xbox_GuestCpuHeld() == 1, "the caller holds it again after the call", NULL);
    }

    /* PsCreateSystemThread after the first call: a worker, run inline. */
    recomp_env_set(RENV_WORKERS, "inline");
    fn = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 12));
    check(fn != NULL, "ordinal 254 resolves to a bridge entry point", NULL);
    if (fn) {
        uint32_t *sp = (uint32_t *)(mem + STACK_VA);
        memset(sp, 0, 8 * 4);
        sp[0] = 0xBEEF0004u;          /* guest return address */
        sp[1] = 0x40004u;             /* ThreadHandle out */
        sp[7] = WORKER_VA;            /* StartRoutine (arg 6) */
        g_esp = STACK_VA;
        fn();
        check(s_worker_ran, "ordinal 254 ran the inline worker", NULL);
        check(s_worker_held == 1, "the inline worker held the guest CPU", NULL);
        check(xbox_GuestCpuHeld() == 1, "the caller holds it again after the worker", NULL);
    }

    /* KeSetBasePriorityThread (143) and KeQueryBasePriorityThread (124) take
     * a guest KTHREAD, the object ObReferenceObjectByHandle hands out (XAPI's
     * SetThreadPriority does exactly that). The bridges used to pass that guest
     * VA on as a host thread handle: Windows failed the call and left every
     * guest thread at normal priority (SSX 3's music crackled). The
     * object here is shaped like a POSIX shim thread (first word K_THREAD = 3,
     * every other word a marker priority of 7), so the old bridge would have
     * written its priority into these bytes and read the marker back. A VA no
     * host thread stands behind must be left alone: the bytes stay as they
     * were and the query reads 0. */
    /* guest_priority=2, so the bridges try to resolve the object on every
     * host: the checks below are about that resolve path. */
    recomp_env_set(RENV_GUEST_PRIORITY, "2");
    xbox_GuestPriorityReloadConfig();
    {
        enum { OBJ_VA = 0x00050000u, OBJ_WORDS = 0x100 };
        uint32_t *obj = (uint32_t *)(mem + OBJ_VA);
        uint32_t before[OBJ_WORDS];
        for (int i = 0; i < OBJ_WORDS; i++)
            obj[i] = i ? 7u : 3u;
        memcpy(before, obj, sizeof before);

        fn = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 16));
        check(fn != NULL, "ordinal 143 resolves to a bridge entry point", NULL);
        if (fn) {
            uint32_t *sp = (uint32_t *)(mem + STACK_VA);
            sp[0] = 0xBEEF0005u;          /* guest return address */
            sp[1] = OBJ_VA;               /* a guest object, no host thread */
            sp[2] = 2;                    /* Increment: highest */
            g_esp = STACK_VA;
            g_eax = 0xDEADBEEFu;
            fn();
            check(g_eax == 0, "KeSetBasePriorityThread without a host thread is a no-op", NULL);
            check(g_esp == STACK_VA + 12, "ordinal 143 pops its two stdcall arguments", NULL);
            check(memcmp(before, obj, sizeof before) == 0,
                  "KeSetBasePriorityThread does not write the guest object as a host thread", NULL);
        }
        fn = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 20));
        check(fn != NULL, "ordinal 124 resolves to a bridge entry point", NULL);
        if (fn) {
            uint32_t *sp = (uint32_t *)(mem + STACK_VA);
            sp[0] = 0xBEEF0006u;
            sp[1] = OBJ_VA;
            g_esp = STACK_VA;
            g_eax = 0xDEADBEEFu;
            fn();
            check(g_eax == 0, "KeQueryBasePriorityThread does not read the guest object as a host thread", NULL);
            check(g_esp == STACK_VA + 8, "ordinal 124 pops its stdcall argument", NULL);
        }
    }
    recomp_env_set(RENV_GUEST_PRIORITY, NULL);
    xbox_GuestPriorityReloadConfig();

    /* Priorities reach the host thread, and read back, only where
     * a time-critical thread runs alone. CRI's ADX in JSRF locks by going
     * time critical and drops its locks with "while (GetThreadPriority(self)
     * == TIME_CRITICAL) unlock()"; where the host does not exclude (Wine,
     * the POSIX shim), two threads crossed its one saved priority and one
     * stayed time critical with no lock left, spinning there for good. */
    check(xbox_GuestPriorityPolicy(1, 1, 1, 0) == 1,
          "guest_priority default: native Windows on one core applies", NULL);
    check(xbox_GuestPriorityPolicy(1, 0, 1, 0) == 0,
          "guest_priority default: Wine or POSIX leaves priorities", NULL);
    check(xbox_GuestPriorityPolicy(1, 1, 0, 0) == 0,
          "guest_priority default: threads on every core leave them", NULL);
    /* The guest CPU lock hands over FIFO whatever the priority, so a
     * time-critical holder does not run alone under it (BLiNX 2). */
    check(xbox_GuestPriorityPolicy(1, 1, 1, 1) == 0,
          "guest_priority default: the guest CPU lock leaves them", NULL);
    check(xbox_GuestPriorityPolicy(0, 1, 1, 0) == 0, "guest_priority=0 never applies", NULL);
    check(xbox_GuestPriorityPolicy(2, 0, 0, 1) == 1, "guest_priority=2 always applies", NULL);
    /* The setting as the bridge reads it. The POSIX shim records a
     * priority and enforces none, so its default leaves them alone. */
    recomp_env_set(RENV_GUEST_PRIORITY, "0");
    xbox_GuestPriorityReloadConfig();
    check(xbox_GuestPriorityOn() == 0, "guest_priority=0 reaches the bridge", NULL);
    recomp_env_set(RENV_GUEST_PRIORITY, "2");
    xbox_GuestPriorityReloadConfig();
    check(xbox_GuestPriorityOn() == 1, "guest_priority=2 reaches the bridge", NULL);
#ifndef _WIN32
    recomp_env_set(RENV_GUEST_PRIORITY, NULL);
    xbox_GuestPriorityReloadConfig();
    check(xbox_GuestPriorityOn() == 0, "default on POSIX leaves priorities alone", NULL);
#endif
    recomp_env_set(RENV_GUEST_PRIORITY, NULL);
    xbox_GuestPriorityReloadConfig();

    VirtualFree(mem, 0, MEM_RELEASE);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
