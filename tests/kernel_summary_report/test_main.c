/*
 * The [KERNEL] summary and the [READ] trace gate (kernel_bridge.c).
 *
 * The summary used to print every two seconds on the guest thread. It now
 * prints once, at whichever end of the run gets there first -- a bugcheck,
 * the watchdog, a firmware return, a window close (the exit hook) or atexit
 * -- and RECOMP_TRACE=kernel_summary brings the periodic form back. The
 * [READ] line per NtReadFile sits behind RECOMP_TRACE=file. Both are easy to
 * break without anything failing: a second exit path printing the block
 * again, or a gate that reads the wrong key, only shows as log noise.
 *
 * The calls go through the real dispatcher, over a buffer standing in for
 * guest memory, the way tests/kernel_bridge drives it. Whether exactly one
 * block came out can only be counted after the kernel's own atexit handler
 * has run, so the verdict is given from an atexit handler registered before
 * the first kernel call: handlers run in reverse, so it runs last.
 *
 *   kernel_summary_report_test [--traced] [STDOUT_FILE]
 *
 * --traced sets file and kernel_summary before the first call (both gates
 * are read once) and expects the lines the default run must not print.
 * STDOUT_FILE: write the report there (Proton drops a console exe's stdout).
 *
 * Cross-platform: Win32 backend on Windows and Proton, POSIX elsewhere.
 */
#include "kernel.h"
#include "recomp_env.h"
#include "recomp_exit_hook.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

/* The seam, plus the guest CPU state the dispatcher reads and writes. */
extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

#define THUNK_VA 0x10000u
#define STACK_VA 0x20000u

static int failures, traced;
static char g_log[600];
static uint8_t *mem;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Drop the '\r' the Win32 CRT writes before each '\n' of a text-mode
 * stream, so one expected line matches on every host. */
static void unix_eol(char *b)
{
    char *w = b;
    for (; *b; b++)
        if (*b != '\r') *w++ = *b;
    *w = '\0';
}

/* Occurrences of s in the captured stderr so far. */
static int log_count(const char *s)
{
    FILE *f;
    static char buf[1 << 20];
    size_t n;
    int c = 0;
    const char *p;

    fflush(stderr);
    f = fopen(g_log, "rb");
    if (!f) return -1;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    unix_eol(buf);
    for (p = buf; (p = strstr(p, s)) != NULL; p += strlen(s))
        c++;
    return c;
}

/* One guest call through thunk slot `slot`: a return address, then args. */
static void call(int slot, const uint32_t *args, int nargs)
{
    recomp_func_t fn = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 4 * slot));
    uint32_t *sp = (uint32_t *)(mem + STACK_VA);
    int i;
    sp[0] = 0xBEEF0001u;
    for (i = 0; i < nargs; i++)
        sp[1 + i] = args[i];
    g_esp = STACK_VA;
    if (fn) fn();
}

/* Registered before the first kernel call, so it runs after the kernel's
 * own atexit report: the one place "printed exactly once" can be counted. */
static void verdict_at_exit(void)
{
    printf("after exit\n");
    check(log_count("[KERNEL] summary (") == 1,
          "one summary block over bugcheck, window hook and exit");
    check(log_count("[KERNEL] summary (bugcheck)") == 1,
          "... and it is the first exit's, the bugcheck");
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
           failures == 1 ? "" : "s");
    fflush(stdout);
    _Exit(failures ? 1 : 0);
}

int main(int argc, char **argv)
{
    int a = 1;
    if (a < argc && strcmp(argv[a], "--traced") == 0) { traced = 1; a++; }
    if (a < argc && !freopen(argv[a], "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);

#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        snprintf(g_log, sizeof g_log, "%sksumrep-%lu%s.log", tmp,
                 (unsigned long)GetCurrentProcessId(), traced ? "-t" : "");
    }
#else
    snprintf(g_log, sizeof g_log, "/tmp/ksumrep-%ld%s.log", (long)getpid(),
             traced ? "-t" : "");
#endif
    if (!freopen(g_log, "w", stderr)) return 2;

    mem = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT,
                       PAGE_READWRITE);
    if (!mem) { printf("no guest memory stand-in\n"); return 1; }
    g_xbox_mem_offset = (ptrdiff_t)mem;
    atexit(verdict_at_exit);

    printf("%s\n", traced ? "RECOMP_TRACE=file,kernel_summary" : "defaults");
    if (traced) {
        recomp_env_set(RENV_FILE_TRACE, "1");
        recomp_env_set(RENV_KERNEL_SUMMARY, "1");
    }

    /* Before any kernel call there is nothing to summarise. */
    xbox_kernel_summary_report("bugcheck");
    check(log_count("[KERNEL]") == 0, "no block at call count 0");

    *(uint32_t *)(mem + THUNK_VA) = 0x80000000u | 173u;      /* MmGetPhysicalAddress */
    *(uint32_t *)(mem + THUNK_VA + 4) = 0x80000000u | 219u;  /* NtReadFile */
    xbox_kernel_set_thunk_address(THUNK_VA, 2);
    xbox_kernel_bridge_init();

    /* NtReadFile on no handle, zero bytes: it fails, and the [READ] line is
     * written whatever the status (a failed read is what it is for). */
    {
        uint32_t args[8] = { 0, 0, 0, 0, STACK_VA + 0x100, STACK_VA + 0x200, 0, 0 };
        call(1, args, 8);
    }
    check(log_count("  [READ] ") == (traced ? 1 : 0),
          traced ? "file: NtReadFile writes its [READ] line"
                 : "no [READ] line without RECOMP_TRACE=file");

    /* The periodic block wants more than 200 calls and two seconds between
     * blocks; the first call starts the clock. */
    {
        uint32_t arg = 0x80001000u;
        int i;
        for (i = 0; i < 210; i++)
            call(0, &arg, 1);
        Sleep(2100);
        call(0, &arg, 1);
    }
    check(log_count("[KERNEL] summary:") == (traced ? 1 : 0),
          traced ? "kernel_summary: the periodic block prints"
                 : "no periodic block without RECOMP_TRACE=kernel_summary");

    /* A bugcheck, then a window close, then the process exit: one block. */
    xbox_kernel_summary_report("bugcheck");
    check(log_count("[KERNEL] summary (bugcheck): 212 total calls, latest ordinal 173") == 1,
          "the bugcheck prints the block, with the latest call");
    /* The periodic block (traced) came at the same call, so ranks the same. */
    check(log_count("[KERNEL]   ordinal 173 x211\n") == (traced ? 2 : 1)
          && log_count("[KERNEL]   ordinal 219 x1\n") == (traced ? 2 : 1),
          "... and the ordinals ranked by count");
    recomp_exit_hook_run("window");
    check(log_count("[KERNEL] summary (") == 1, "the window hook adds no second block");
    return 0;
}
