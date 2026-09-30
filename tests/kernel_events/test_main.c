/*
 * Kernel events: KEVENTs a title builds by writing the header itself, with no
 * KeInitializeEvent call, checked through the thunk dispatcher like a title's
 * kernel calls.
 *
 * Runs the real memory layout on the small synthetic XBE from
 * tools/conformance, so no title and no game files are needed.
 *
 * One process per mode, because the switch is read once:
 *
 *   default        No switch set. Pins what every title gets today: the guest
 *                  address goes to Win32 as a HANDLE, so a set is lost and a
 *                  wait fails instead of reporting the event's state.
 *   title-kevents  RECOMP_TITLE_KEVENTS=1. Each check fails without it:
 *
 *   1. A synchronization event that is not set times out.
 *   2. KeSetEvent sets it: the next wait succeeds, and SignalState in guest
 *      memory is cleared again (a synchronization wait consumes it).
 *   3. SignalState written to 1 by the title counts: XDK code sets and clears
 *      events by writing the header, not only through the kernel.
 *   4. A notification event stays set across two waits; KeResetEvent clears it.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; this harness has none. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;

enum { S_SET, S_WAIT, S_RESET, N_SLOTS };
static const uint32_t ORD[N_SLOTS] = {
    145,   /* KeSetEvent */
    159,   /* KeWaitForSingleObject */
    138,   /* KeResetEvent */
};

#define STATUS_TIMEOUT 0x00000102u

static uint32_t scratch;                 /* guest VA of a 64 KB block */
#define THUNK_VA (scratch)
#define STACK_VA (scratch + 0x1000)
#define ZERO_VA  (scratch + 0x2000)      /* a LARGE_INTEGER timeout of 0 */
#define SYNC_VA  (scratch + 0x3000)
#define NOTIF_VA (scratch + 0x3100)

static uint32_t slot_va[N_SLOTS];
static int failures;

static uint32_t *G(uint32_t va)
{
    return (uint32_t *)((uintptr_t)va + xbox_GetMemoryOffset());
}

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-66s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static uint32_t call(int slot, int nargs, const uint32_t *args)
{
    uint32_t *sp = G(STACK_VA);
    int i;

    sp[0] = 0xBEEF0001u;
    for (i = 0; i < nargs; i++)
        sp[1 + i] = args[i];
    g_esp = STACK_VA;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(slot_va[slot])();
    return g_eax;
}

/* Header of a KEVENT as XDK code writes it: Type, Absolute, Size (dwords),
 * Inserted, then SignalState and an empty wait list. */
static void build_event(uint32_t va, int type, int signalled)
{
    G(va)[0] = (uint32_t)type | (4u << 16);
    G(va)[1] = signalled ? 1u : 0u;
    G(va)[2] = va + 8;
    G(va)[3] = va + 8;
}

static uint32_t ke_wait(uint32_t va)
{
    uint32_t args[5] = { va, 0, 0, 0, ZERO_VA };
    return call(S_WAIT, 5, args);
}

static uint32_t ke_set(uint32_t va)
{
    uint32_t args[3] = { va, 0, 0 };
    return call(S_SET, 3, args);
}

static uint32_t ke_reset(uint32_t va)
{
    uint32_t args[1] = { va };
    return call(S_RESET, 1, args);
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
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
    const char *mode = argc > 1 ? argv[1] : "default";
    const char *path = argc > 2 ? argv[2] : "tools/conformance/test.xbe";
    int on = !strcmp(mode, "title-kevents");
    size_t xbe_len = 0;
    unsigned char *xbe;
    char d[160];
    uint32_t st;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!on && strcmp(mode, "default")) {
        printf("unknown mode '%s'\n", mode);
        return 2;
    }
    if (on)
        _putenv_s("RECOMP_TITLE_KEVENTS", "1");

    xbe = load_file(path, &xbe_len);
    if (!xbe) {
        printf("cannot read %s -- run: python tools/conformance/mkxbe.py\n", path);
        return 2;
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        puts("xbox_MemoryLayoutInit failed");
        return 2;
    }

    scratch = xbox_HeapAlloc(0x10000, 4096);
    for (i = 0; i < N_SLOTS; i++)
        *G(THUNK_VA + 4 * i) = 0x80000000u | ORD[i];
    xbox_kernel_set_thunk_address(THUNK_VA, N_SLOTS);
    xbox_kernel_bridge_init();
    for (i = 0; i < N_SLOTS; i++) {
        slot_va[i] = *G(THUNK_VA + 4 * i);
        if (!recomp_lookup_kernel(slot_va[i])) {
            printf("ordinal %u did not resolve to a bridge entry point\n", ORD[i]);
            return 2;
        }
    }
    G(ZERO_VA)[0] = G(ZERO_VA)[1] = 0;
    printf("mode: %s\n", mode);

    build_event(SYNC_VA, 1, 0);
    build_event(NOTIF_VA, 0, 0);

    if (!on) {
        ke_set(SYNC_VA);
        st = ke_wait(SYNC_VA);
        snprintf(d, sizeof d, "status 0x%08X", st);
        check(st != 0 && st != STATUS_TIMEOUT,
              "default: a title-built event is not a host event (the wait fails)", d);
    } else {
        /* 1. Not set: times out. */
        st = ke_wait(SYNC_VA);
        snprintf(d, sizeof d, "status 0x%08X", st);
        check(st == STATUS_TIMEOUT, "an unset synchronization event times out", d);

        /* 2. KeSetEvent, then a wait consumes it. */
        ke_set(SYNC_VA);
        st = ke_wait(SYNC_VA);
        snprintf(d, sizeof d, "status 0x%08X, SignalState %u", st, G(SYNC_VA)[1]);
        check(st == 0 && G(SYNC_VA)[1] == 0,
              "KeSetEvent sets it; the wait succeeds and clears SignalState", d);
        st = ke_wait(SYNC_VA);
        snprintf(d, sizeof d, "status 0x%08X", st);
        check(st == STATUS_TIMEOUT, "a second wait times out again", d);

        /* 3. The title sets SignalState itself. */
        G(SYNC_VA)[1] = 1;
        st = ke_wait(SYNC_VA);
        snprintf(d, sizeof d, "status 0x%08X", st);
        check(st == 0, "SignalState written by the title counts", d);

        /* 4. Notification: stays set until reset. */
        ke_set(NOTIF_VA);
        st = ke_wait(NOTIF_VA);
        i = ke_wait(NOTIF_VA) == 0;
        snprintf(d, sizeof d, "status 0x%08X, second %s", st, i ? "ok" : "failed");
        check(st == 0 && i, "a notification event stays set across two waits", d);
        ke_reset(NOTIF_VA);
        st = ke_wait(NOTIF_VA);
        snprintf(d, sizeof d, "status 0x%08X, SignalState %u", st, G(NOTIF_VA)[1]);
        check(st == STATUS_TIMEOUT && G(NOTIF_VA)[1] == 0,
              "KeResetEvent clears it", d);
    }

    xbox_MemoryLayoutShutdown();
    free(xbe);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
