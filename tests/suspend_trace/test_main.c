/*
 * RECOMP_TRACE=suspend[=va], through the bridges. The trace is
 * there to tell whether CRI ADX's lock depth (JSRF, 0x25EFA0) sticks at 1 or
 * more while its helper spins, so the line has to count both kinds per
 * target, give the previous counts, and read the watched dword now and at
 * each target's last call. The cases, with "on":
 *  - on a running thread (the host path), two suspends return 0 and 1 and
 *    two resumes 2 and 1, in the guest's out-parameter as without the trace;
 *  - on a thread held at its start gate, the counted suspend and resumes;
 *  - the line: per target S+n=total and R+n=total, the prev ranges, the
 *    watched dword now and at the last call;
 *  - the next line: +0, the totals kept, prev "-", the dword as it is now;
 *  - a failed call is counted as !1, not as a call;
 *  - KeSuspendThread/KeResumeThread on a held thread, by handle: the gate
 *    path, the counts in eax;
 *  - a thread suspending itself through the pseudo handle: its own target
 *    line, prev 0.
 * With "off": the same calls return the same counts and nothing is counted.
 * The key is set after the bridge init, so no reporter thread runs and the
 * test takes every line itself. A watchdog fails the test instead of
 * hanging it.
 */
#include "kernel.h"
#include "kernel_guest_unwind.h"
#include "recomp_env.h"
#include "xbox_memory_layout.h"
#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
static void routine_block(void);
static void routine_self(void);
recomp_func_t recomp_lookup(uint32_t va)
{
    return va == 0x00010000u ? routine_block
         : va == 0x00010010u ? routine_self : NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

#define G32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))

enum { ORD_KERESUME = 140, ORD_KESUSPEND = 152, ORD_NTRESUME = 224,
       ORD_NTSUSPEND = 231, ORD_PSCREATE = 255 };
static const uint32_t ORDS[] = { ORD_KERESUME, ORD_KESUSPEND, ORD_NTRESUME,
                                 ORD_NTSUSPEND, ORD_PSCREATE };
#define NORDS (sizeof ORDS / sizeof ORDS[0])

static uint32_t g_thunks, g_stack, g_self_stack, g_scratch;
static volatile LONG s_ran, s_self_done;
static HANDLE s_release;
static int failures;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Runs on the new thread and stays until the test lets it go, so the
 * suspends have a live thread to land on. */
static void routine_block(void)
{
    InterlockedIncrement(&s_ran);
    WaitForSingleObject(s_release, 30000);
    g_eax = 0;
}

/* stdcall: return address, then the arguments, on the given guest stack
 * (each thread its own). */
static uint32_t call_on(uint32_t stack, uint32_t ordinal, int argc,
                        const uint32_t *argv)
{
    recomp_func_t fn = NULL;

    for (unsigned i = 0; i < NORDS; i++)
        if (ORDS[i] == ordinal)
            fn = recomp_lookup_kernel(G32(g_thunks + 4 * i));
    if (!fn) {
        printf("  ordinal %u did not resolve\n", ordinal);
        failures++;
        return 0xFFFFFFFFu;
    }
    G32(stack) = 0xBEEF0001u;
    for (int i = 0; i < argc; i++)
        G32(stack + 4 + 4 * (uint32_t)i) = argv[i];
    g_esp = stack;
    g_eax = 0xDEADBEEFu;
    fn();
    return g_eax;
}
#define CALL(ord, ...) call_on(g_stack, (ord), \
    (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4), (uint32_t[]){__VA_ARGS__})

/* Suspends itself through the pseudo handle and goes on once resumed. */
static void routine_self(void)
{
    const uint32_t a[] = { 0xFFFFFFFEu, 0 };

    call_on(g_self_stack, ORD_NTSUSPEND, 2, a);
    InterlockedExchange(&s_self_done, 1);
    g_eax = 0;
}

static uint32_t spawn_at(int suspended, uint32_t handle_va, uint32_t start)
{
    G32(handle_va) = 0;
    CALL(ORD_PSCREATE, handle_va, 0, 0, 0, 0, 0, 0, (uint32_t)suspended, 0,
         start);
    return G32(handle_va);
}
#define spawn(suspended, handle_va) spawn_at((suspended), (handle_va), 0x00010000u)

static int wait_ran(LONG v, int ms)
{
    uint64_t t0 = xbox_HostNowNs();
    while (InterlockedCompareExchange(&s_ran, 0, 0) < v) {
        if (xbox_HostNowNs() - t0 > (uint64_t)ms * 1000000ull)
            return 0;
        Sleep(1);
    }
    return 1;
}

static DWORD WINAPI watchdog(LPVOID unused)
{
    (void)unused;
    Sleep(60000);
    fprintf(stderr, "suspend_trace: watchdog: a case hung\n");
    fflush(stderr);
    _Exit(1);
    return 0;
}

static unsigned char *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    rewind(f);
    b = malloc((size_t)len);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)len;
    return b;
}

/* "| <target>" ("self" for the pseudo handle) then the target's fields up to the next " | " or the end. */
static const char *field_of(const char *line, uint32_t target, char *out,
                            size_t n)
{
    char key[16];
    const char *p, *e;

    if (target == 0xFFFFFFFEu)
        snprintf(key, sizeof key, "| self ");
    else
        snprintf(key, sizeof key, "| %08X ", target);
    p = strstr(line, key);
    if (!p)
        return NULL;
    p += strlen(key);
    e = strstr(p, " | ");
    if (!e)
        e = p + strlen(p);
    snprintf(out, n, "%.*s", (int)(e - p), p);
    return out;
}

static int has(const char *line, uint32_t target, const char *want)
{
    char f[256];
    if (!field_of(line, target, f, sizeof f)) {
        printf("    no target %08X in: %s\n", target, line);
        return 0;
    }
    if (strcmp(f, want) != 0) {
        printf("    target %08X: got  \"%s\"\n"
               "                    want \"%s\"\n", target, f, want);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    size_t n = 0;
    unsigned char *xbe;
    uint32_t run, held, kheld, self, st, cnt = 0, watch;
    uint64_t t0;
    char line[XBOX_SUSPEND_TRACE_LINE], want[160], f[256];
    int on;

    setvbuf(stdout, NULL, _IONBF, 0);
    CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    xbe = load(argc > 1 ? argv[1] : "tools/conformance/test.xbe", &n);
    if (!xbe || !xbox_MemoryLayoutInit(xbe, n)) {
        printf("cannot map guest memory (run tools/conformance/mkxbe.py)\n");
        return 2;
    }
    on = !(argc > 2 && !strcmp(argv[2], "off"));
    recomp_env_set(RENV_GUEST_LOCK, "0");
    recomp_env_set(RENV_GUEST_SCHED, "0");
    recomp_env_set(RENV_SUSPEND_TRACE, NULL);
    xbox_GuestCpuReloadConfig();
    xbox_GuestUnwindEnable();
    s_release = CreateEventA(NULL, TRUE, FALSE, NULL);

    g_thunks  = xbox_HeapAlloc(0x1000, 4096);
    g_stack   = xbox_HeapAlloc(0x1000, 4096);
    g_self_stack = xbox_HeapAlloc(0x1000, 4096);
    g_scratch = xbox_HeapAlloc(0x1000, 4096);
    for (unsigned i = 0; i < NORDS; i++)
        G32(g_thunks + 4 * i) = 0x80000000u | ORDS[i];
    G32(g_thunks + 4 * NORDS) = 0;
    xbox_kernel_set_thunk_address(g_thunks, NORDS + 1);
    xbox_kernel_bridge_init();
    xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN);

    /* After the bridge init, so its Start found the key unset. */
    watch = g_scratch + 0x300;
    G32(watch) = 0;
    if (on) {
        static char v[16];
        snprintf(v, sizeof v, "0x%X", watch);
        recomp_env_set(RENV_SUSPEND_TRACE, v);
    }
    printf("suspend_trace (%s)\n", on ? "on" : "off");
    check(xbox_SuspendTraceOn() == on && xbox_SuspendTraceVa() == (on ? watch : 0),
          "switch: the key as asked, the va parsed");

    run = spawn(0, g_scratch + 0x100);
    check(run != 0 && wait_ran(1, 5000), "setup: a running thread");

    /* Host path. The dword stands for the ADX depth: 1 at the suspends,
     * 2 at the resumes. */
    G32(watch) = 1;
    G32(g_scratch + 0x200) = 99;
    st = CALL(ORD_NTSUSPEND, run, g_scratch + 0x200);
    cnt = G32(g_scratch + 0x200);
    G32(g_scratch + 0x200) = 99;
    st |= CALL(ORD_NTSUSPEND, run, g_scratch + 0x200);
    check(st == 0 && cnt == 0 && G32(g_scratch + 0x200) == 1,
          "host: two suspends return the counts 0 and 1");
    G32(watch) = 2;
    G32(g_scratch + 0x200) = 99;
    st = CALL(ORD_NTRESUME, run, g_scratch + 0x200);
    cnt = G32(g_scratch + 0x200);
    G32(g_scratch + 0x200) = 99;
    st |= CALL(ORD_NTRESUME, run, g_scratch + 0x200);
    check(st == 0 && cnt == 2 && G32(g_scratch + 0x200) == 1,
          "host: two resumes return the counts 2 and 1");
    st = CALL(ORD_NTSUSPEND, run, 0);
    st |= CALL(ORD_NTRESUME, run, 0);
    check(st == 0, "host: a suspend and resume with no count pointer");

    /* Gate path: counted, not the host's. */
    held = spawn(1, g_scratch + 0x110);
    G32(watch) = 3;
    G32(g_scratch + 0x200) = 99;
    st = CALL(ORD_NTSUSPEND, held, g_scratch + 0x200);
    check(st == 0 && G32(g_scratch + 0x200) == 1,
          "gate: a suspend on a held thread returns the count 1");
    st = CALL(ORD_NTRESUME, held, g_scratch + 0x200);
    check(st == 0 && G32(g_scratch + 0x200) == 2,
          "gate: the first resume returns 2");
    st = CALL(ORD_NTRESUME, held, g_scratch + 0x200);
    check(st == 0 && G32(g_scratch + 0x200) == 1 && wait_ran(2, 5000),
          "gate: the second returns 1 and the thread runs");

    /* The Ke forms by handle on a held thread: the same gate, the previous
     * count in eax. */
    kheld = spawn(1, g_scratch + 0x120);
    G32(watch) = 5;
    st = CALL(ORD_KESUSPEND, kheld);
    check(st == 1, "gate: KeSuspendThread on a held thread returns 1");
    st = CALL(ORD_KERESUME, kheld);
    cnt = CALL(ORD_KERESUME, kheld);
    check(st == 2 && cnt == 1 && wait_ran(3, 5000),
          "gate: KeResumeThread returns 2 then 1 and the thread runs");

    /* Self: the thread parks in the call. A resume that lands before the
     * park finds count 0 and does nothing, so resume until one returns 1. */
    G32(watch) = 4;
    self = spawn_at(0, g_scratch + 0x130, 0x00010010u);
    t0 = xbox_HostNowNs();
    cnt = 0;
    while (self && cnt < 1 && xbox_HostNowNs() - t0 < 5000000000ull) {
        Sleep(5);
        G32(g_scratch + 0x200) = 99;
        st = CALL(ORD_NTRESUME, self, g_scratch + 0x200);
        cnt = st == 0 ? G32(g_scratch + 0x200) : 0;
    }
    t0 = xbox_HostNowNs();
    while (!InterlockedCompareExchange(&s_self_done, 0, 0)
           && xbox_HostNowNs() - t0 < 5000000000ull)
        Sleep(1);
    check(cnt == 1 && s_self_done, "self: suspended through the pseudo handle, resumed");

    /* A failed call: the POSIX shim has no invalid-handle check to fail
     * through, so the note the bridges make on a failure is made here. */
    xbox_SuspendTraceNote(1, 0x00ABC000u, STATUS_UNSUCCESSFUL, 0);

    G32(watch) = 7;
    xbox_SuspendTraceFormat(line, sizeof line, 5);
    printf("    %s\n", line);
    if (on) {
        snprintf(want, sizeof want, "[SUSPEND] t=5 [%08X]=00000007 | ", watch);
        check(!strncmp(line, want, strlen(want)),
              "line: the time, then the watched dword as it is now");
        check(has(line, run, "S+3=3 prev 0-1 @2 R+3=3 prev 1-2 @2"),
              "line: the running thread's calls, prev ranges, dword at the call");
        check(has(line, held, "S+1=1 prev 1-1 @3 R+2=2 prev 1-2 @3"),
              "line: the held thread's, counted at its gate");
        check(has(line, 0x00ABC000u, "S+0=0 prev - R+0=0 prev - !1"),
              "line: the failed resume as !1, not a call");
        check(has(line, kheld, "S+1=1 prev 1-1 @5 R+2=2 prev 1-2 @5"),
              "line: the Ke forms on a held thread, counted at its gate");
        check(has(line, 0xFFFFFFFEu, "S+1=1 prev 0-0 @4 R+0=0 prev -"),
              "line: the self-suspend under the pseudo handle, prev 0");
        /* The resumes that came before the park return 0; the last one 1. */
        check(field_of(line, self, f, sizeof f) && !strncmp(f, "S+0=0 prev - R+", 15)
              && strstr(f, "-1 @4") != NULL,
              "line: the resumes of the self-suspended thread, the last at 1");

        G32(watch) = 0;
        xbox_SuspendTraceFormat(line, sizeof line, 6);
        printf("    %s\n", line);
        snprintf(want, sizeof want, "[SUSPEND] t=6 [%08X]=00000000 | ", watch);
        check(!strncmp(line, want, strlen(want))
              && has(line, run, "S+0=3 prev - @2 R+0=3 prev - @2")
              && has(line, 0x00ABC000u, "S+0=0 prev - R+0=0 prev -"),
              "next line: +0, totals kept, no prev, the dword now");
    } else {
        check(!strcmp(line, "[SUSPEND] t=5"),
              "off: nothing counted, no dword");
    }

    SetEvent(s_release);
    if (failures) {
        printf("suspend_trace: %d failure(s)\n", failures);
        return 1;
    }
    printf("suspend_trace: ok\n");
    return 0;
}
