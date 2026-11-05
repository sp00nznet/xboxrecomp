/*
 * CreateSuspended with guest_sched (RECOMP_GUEST_LOCK=1, guest_sched=1),
 * through the bridges: PsCreateSystemThreadEx starts the host thread at
 * once, and the guest CPU holds it at its join until the title resumes it.
 * JSRF's ADX spinner must not run before its lock resumes it. The cases:
 *  - a thread created suspended does not run its start routine;
 *  - NtSuspendThread on its handle counts (returns 1), and one
 *    NtResumeThread (returns 2) leaves it held;
 *  - the second NtResumeThread lets it run, and returns the count 1;
 *  - one created running runs at once.
 * A watchdog fails the test instead of hanging it.
 */
#include "kernel.h"
#include "recomp_env.h"
#include "xbox_memory_layout.h"
#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
static void routine_runs(void);
recomp_func_t recomp_lookup(uint32_t va)
{
    return va == 0x00010000u ? routine_runs : NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

#define G32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))

enum { ORD_NTRESUME = 224, ORD_NTSUSPEND = 231, ORD_PSCREATE = 255 };
static const uint32_t ORDS[] = { ORD_NTRESUME, ORD_NTSUSPEND, ORD_PSCREATE };
#define NORDS (sizeof ORDS / sizeof ORDS[0])

static uint32_t g_thunks, g_stack, g_scratch;
static volatile LONG s_ran;
static int failures;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Runs on the new thread, holding the guest CPU. */
static void routine_runs(void)
{
    InterlockedIncrement(&s_ran);
    g_eax = 0;
}

/* stdcall: return address, then the arguments. */
static uint32_t call(uint32_t ordinal, int argc, const uint32_t *argv)
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
    G32(g_stack) = 0xBEEF0001u;
    for (int i = 0; i < argc; i++)
        G32(g_stack + 4 + 4 * (uint32_t)i) = argv[i];
    g_esp = g_stack;
    g_eax = 0xDEADBEEFu;
    fn();
    return g_eax;
}
#define CALL(ord, ...) call((ord), \
    (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4), (uint32_t[]){__VA_ARGS__})

static uint32_t spawn(int suspended, uint32_t handle_va)
{
    G32(handle_va) = 0;
    /* ThreadHandle, ExtraSize, KStack, Tls, ThreadId, ctx1, ctx2,
     * CreateSuspended, DebugStack, StartRoutine */
    CALL(ORD_PSCREATE, handle_va, 0, 0, 0, 0, 0, 0, (uint32_t)suspended, 0,
         0x00010000u);
    return G32(handle_va);
}

static int wait_ran(LONG v, int ms)
{
    uint64_t t0 = xbox_HostNowNs();
    while (InterlockedCompareExchange(&s_ran, 0, 0) != v) {
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
    fprintf(stderr, "guest_sched_bridge: watchdog: a case hung\n");
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

int main(int argc, char **argv)
{
    size_t n = 0;
    unsigned char *xbe;
    uint32_t h, st;
    HANDLE wd;

    setvbuf(stdout, NULL, _IONBF, 0);
    wd = CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    CloseHandle(wd);
    xbe = load(argc > 1 ? argv[1] : "tools/conformance/test.xbe", &n);
    if (!xbe || !xbox_MemoryLayoutInit(xbe, n)) {
        printf("cannot map guest memory (run tools/conformance/mkxbe.py)\n");
        return 2;
    }
    recomp_env_set(RENV_GUEST_LOCK, "1");
    recomp_env_set(RENV_GUEST_SCHED, "1");
    xbox_GuestCpuReloadConfig();

    g_thunks  = xbox_HeapAlloc(0x1000, 4096);
    g_stack   = xbox_HeapAlloc(0x1000, 4096);
    g_scratch = xbox_HeapAlloc(0x1000, 4096);
    for (unsigned i = 0; i < NORDS; i++)
        G32(g_thunks + 4 * i) = 0x80000000u | ORDS[i];
    G32(g_thunks + 4 * NORDS) = 0;
    xbox_kernel_set_thunk_address(g_thunks, NORDS + 1);
    xbox_kernel_bridge_init();
    xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN);

    printf("guest_sched_bridge\n");
    check(xbox_GuestSchedOn(), "switch: lock and guest_sched on");
    /* The test's thread is the title's main thread: a guest thread at 8. */
    xbox_GuestCpuJoin();

    h = spawn(1, g_scratch + 0x100);
    check(h != 0, "create: PsCreateSystemThreadEx (CreateSuspended) writes a handle");
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 219);   /* NtReadFile: blocks */
    Sleep(200);
    check(!s_ran, "create: 200 ms on, the suspended thread has not run its routine");
    xbox_GuestCpuAcquire();

    /* Another thread's suspend is counted by the guest CPU, not the host. */
    G32(g_scratch + 0x200) = 99;
    st = CALL(ORD_NTSUSPEND, h, g_scratch + 0x200);
    check(st == 0 && G32(g_scratch + 0x200) == 1,
          "suspend: NtSuspendThread on its handle returns the count 1");
    st = CALL(ORD_NTRESUME, h, g_scratch + 0x200);
    check(st == 0 && G32(g_scratch + 0x200) == 2,
          "suspend: the first NtResumeThread returns 2");
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 219);
    Sleep(100);
    check(!s_ran, "suspend: still counted 1, the thread has not run");
    xbox_GuestCpuAcquire();

    G32(g_scratch + 0x200) = 99;
    st = CALL(ORD_NTRESUME, h, g_scratch + 0x200);
    check(st == 0 && G32(g_scratch + 0x200) == 1,
          "resume: NtResumeThread on its handle returns the count 1");
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 219);
    check(wait_ran(1, 5000), "resume: and the thread runs its routine");
    xbox_GuestCpuAcquire();

    h = spawn(0, g_scratch + 0x110);
    xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, 219);
    check(h != 0 && wait_ran(2, 5000), "create: one created running runs at once");
    xbox_GuestCpuAcquire();
    xbox_GuestCpuPart();

    if (failures) {
        printf("guest_sched_bridge: %d failure(s)\n", failures);
        return 1;
    }
    printf("guest_sched_bridge: ok\n");
    return 0;
}
