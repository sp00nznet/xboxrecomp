/*
 * The worker stack cap.
 *
 * Starts 32 workers that idle, as a title's I/O workers wait for requests,
 * then a 33rd. PsCreateSystemThreadEx used to run a worker with no stack
 * inline on its caller, so the 33rd never returned and the caller hung. It
 * now prints why and ends the process; the CMake test passes on that line.
 *
 * Runs the real memory layout on the small synthetic XBE from
 * tools/conformance, so no title and no game files are needed.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#define idle_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define idle_ms(ms) usleep((ms) * 1000)
#endif

typedef void (*recomp_func_t)(void);
static void worker_idles(void);
recomp_func_t recomp_lookup(uint32_t va);
recomp_func_t recomp_lookup(uint32_t va)
{
    return va == 0x00010000u ? worker_idles : NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t va);
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;

#define ORD_PSCREATE 255u
#define CAP 32

static uint32_t g_thunk, g_stack, g_handle;

static uint32_t *G(uint32_t va)
{
    return (uint32_t *)((uintptr_t)va + xbox_GetMemoryOffset());
}

/* Never returns, like a worker waiting for requests that never come. */
static void worker_idles(void)
{
    for (;;)
        idle_ms(1000);
}

/* PsCreateSystemThreadEx(ThreadHandle, ExtraSize, KStack, Tls, ThreadId,
 * ctx1, ctx2, CreateSuspended, DebugStack, StartRoutine), stdcall. */
static uint32_t spawn(void)
{
    uint32_t args[10] = { g_handle, 0, 0, 0, 0, 0, 0, 0, 0, 0x00010000u };
    uint32_t *sp = G(g_stack);
    int i;

    *G(g_handle) = 0;
    sp[0] = 0xBEEF0001u;
    for (i = 0; i < 10; i++)
        sp[1 + i] = args[i];
    g_esp = g_stack;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(*G(g_thunk))();
    return *G(g_handle);
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tools/conformance/test.xbe";
    size_t xbe_len = 0;
    unsigned char *xbe;
    int i, live, cap;

    setvbuf(stdout, NULL, _IONBF, 0);
    xbe = load_file(path, &xbe_len);
    if (!xbe) {
        printf("cannot read %s -- run: python tools/conformance/mkxbe.py\n", path);
        return 2;
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        puts("xbox_MemoryLayoutInit failed");
        return 2;
    }

    g_thunk  = xbox_HeapAlloc(0x1000, 4096);
    g_stack  = xbox_HeapAlloc(0x1000, 4096);
    g_handle = xbox_HeapAlloc(0x1000, 4096);
    *G(g_thunk) = 0x80000000u | ORD_PSCREATE;
    *G(g_thunk + 4) = 0;
    xbox_kernel_set_thunk_address(g_thunk, 2);
    xbox_kernel_bridge_init();
    xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN);

    for (i = 0; i < CAP; i++) {
        if (!spawn()) {
            printf("worker %d: no handle FAIL\n", i + 1);
            return 1;
        }
    }
    xbox_ThreadStackCounts(&live, &cap);
    printf("%d workers spawned, %d stacks live, cap %d\n", CAP, live, cap);
    if (live != CAP || cap != CAP) {
        puts("stack count FAIL");
        return 1;
    }
    fflush(stderr);

    spawn();   /* ends the process */
    puts("the 33rd worker returned to its caller FAIL");
    return 1;
}
