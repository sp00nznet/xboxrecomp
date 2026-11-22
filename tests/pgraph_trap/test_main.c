/*
 * Software-method traps (RECOMP_PGRAPH_TRAP=1).
 *
 * The pushbuffer executor latches a NO_OPERATION carrying a value into
 * PGRAPH's trap registers and raises the graphics interrupt, and the title's
 * own handler acknowledges it by writing PGRAPH_INTR's bits back. The
 * aperture is plain memory, so write-1-to-clear is modelled by a read-only
 * page whose writes single-step. Driven here against the real memory layout
 * on the synthetic XBE from tools/conformance, with a second thread playing
 * the title's handler. No title and no game files.
 *
 * One process per mode, because the switches are read once:
 *
 *   on        RECOMP_PGRAPH_TRAP=1.
 *     1. With PGRAPH_INTR_EN at 0 nothing is latched or raised.
 *     2. With it set, the raise latches the method and value, sets NSOURCE,
 *        PGRAPH_INTR and PMC_INTR_0's PGRAPH bit, and the ack thread leaves
 *        those bits alone.
 *     3. A write of 0 to PGRAPH_INTR clears nothing; a write of the bit
 *        clears it, PMC_INTR_0's bit drops, and the wait returns 1.
 *     4. A trap raised with the FIFO open ends only when the handler has
 *        cleared PGRAPH_INTR and opened the FIFO again.
 *     5. A trap nobody acknowledges is cleared at the timeout; the wait
 *        returns 0.
 *   off       No switch: nothing is latched, and the page stays writable.
 *   spin-off  RECOMP_PGRAPH_TRAP=1, RECOMP_TRAP_SPIN=0: check 3 with the
 *             2 ms waits.
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

#define PMC_INTR_0          0x000100u
#define PGRAPH_INTR         0x400100u
#define PGRAPH_NSOURCE      0x400108u
#define PGRAPH_INTR_EN      0x400140u
#define PGRAPH_TRAPPED_ADDR 0x400704u
#define PGRAPH_TRAPPED_DATA 0x400708u
#define PGRAPH_FIFO         0x400720u
#define INTR_NOTIFY         (1u << 0)
#define PMC_PGRAPH          (1u << 12)

/* A NO_OPERATION on subchannel 0 carrying 0x304, as the executor raises it. */
#define TRAP_ADDR           0x00000100u
#define TRAP_DATA           0x00000304u

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

static volatile uint32_t *reg(uint32_t off)
{
    return xbox_Nv2aRegPtr(off);
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

#ifdef _WIN32
static long long now_us(void)
{
    LARGE_INTEGER f, c;

    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000000 / f.QuadPart;
}

/* The title's handler. Each write to the PGRAPH page goes through the
 * single-step path, as a recompiled handler's would. */
enum { ACK_PLAIN, ACK_FIFO };
static volatile LONG s_ack_kind;
static volatile long long s_fifo_opened_us;

static DWORD WINAPI handler_thread(LPVOID unused)
{
    (void)unused;
    Sleep(20);
    if (s_ack_kind == ACK_FIFO) {
        *reg(PGRAPH_FIFO) = 0;                  /* closed for the service */
        *reg(PGRAPH_INTR) = INTR_NOTIFY;        /* acknowledged */
        Sleep(20);                              /* still reading registers */
        s_fifo_opened_us = now_us();
        *reg(PGRAPH_FIFO) = 1;
    } else {
        *reg(PGRAPH_INTR) = 0;                  /* clears nothing */
        Sleep(5);
        *reg(PGRAPH_INTR) = INTR_NOTIFY;
    }
    return 0;
}

static HANDLE start_handler(LONG kind)
{
    s_ack_kind = kind;
    return CreateThread(NULL, 0, handler_thread, NULL, 0, NULL);
}

static void raise_and_ack(void)
{
    HANDLE h;
    long long t0;
    int r;

    check(xbox_Nv2aPgraphRaise(TRAP_ADDR, TRAP_DATA) == 1, "raise latches the trap");
    check(*reg(PGRAPH_INTR) == INTR_NOTIFY, "before the handler runs, PGRAPH_INTR holds the bit");
    h = start_handler(ACK_PLAIN);
    t0 = now_us();
    r = xbox_Nv2aPgraphWaitAck(1000);
    check(r == 1, "the wait returns 1 on the acknowledgement");
    check(now_us() - t0 < 500000, "  and well before its timeout");
    check(*reg(PGRAPH_INTR) == 0, "PGRAPH_INTR is clear");
    check(!(*reg(PMC_INTR_0) & PMC_PGRAPH), "PMC_INTR_0's PGRAPH bit is down");
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
}

static void run_on(void)
{
    HANDLE h;
    int r;
    long long t0, waited;

    check(xbox_Nv2aPgraphTrapOn() == 1, "traps are on");

    /* 1 */
    *reg(PGRAPH_INTR_EN) = 0;
    check(xbox_Nv2aPgraphRaise(TRAP_ADDR, TRAP_DATA) == 0,
          "1. PGRAPH_INTR_EN 0: the raise returns 0");
    check(*reg(PGRAPH_INTR) == 0 && *reg(PGRAPH_TRAPPED_ADDR) == 0,
          "   and latches nothing");

    /* 2 */
    *reg(PGRAPH_INTR_EN) = INTR_NOTIFY;
    *reg(PGRAPH_FIFO) = 0;
    check(xbox_Nv2aPgraphRaise(TRAP_ADDR, TRAP_DATA) == 1, "2. the raise returns 1");
    check(*reg(PGRAPH_TRAPPED_ADDR) == TRAP_ADDR, "   TRAPPED_ADDR holds the method");
    check(*reg(PGRAPH_TRAPPED_DATA) == TRAP_DATA, "   TRAPPED_DATA holds the value");
    check(*reg(PGRAPH_NSOURCE) == 1, "   NSOURCE is NOTIFY");
    Sleep(50);  /* several passes of the ack thread */
    check(*reg(PGRAPH_INTR) == INTR_NOTIFY, "   PGRAPH_INTR is set, and the ack thread leaves it");
    check((*reg(PMC_INTR_0) & PMC_PGRAPH) != 0, "   PMC_INTR_0's PGRAPH bit is set, and stays");
    *reg(PGRAPH_INTR) = INTR_NOTIFY;
    check(*reg(PGRAPH_INTR) == 0, "   a write of the bit clears it");
    check(xbox_Nv2aPgraphWaitAck(100) == 1, "   so the wait returns at once");

    /* 3 */
    printf("3. a handler that writes 0, then the bit\n");
    raise_and_ack();

    /* 4 */
    *reg(PGRAPH_FIFO) = 1;
    check(xbox_Nv2aPgraphRaise(TRAP_ADDR, TRAP_DATA) == 1, "4. raised with the FIFO open");
    s_fifo_opened_us = 0;
    h = start_handler(ACK_FIFO);
    r = xbox_Nv2aPgraphWaitAck(1000);
    t0 = now_us();
    check(r == 1, "   the wait returns 1");
    check(s_fifo_opened_us != 0 && t0 >= s_fifo_opened_us,
          "   only after the handler opened the FIFO again");
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);

    /* 5 */
    check(xbox_Nv2aPgraphRaise(TRAP_ADDR, TRAP_DATA) == 1, "5. raised, never acknowledged");
    t0 = now_us();
    r = xbox_Nv2aPgraphWaitAck(50);
    waited = now_us() - t0;
    check(r == 0, "   the wait returns 0");
    check(waited >= 45000 && waited < 1000000, "   at its timeout");
    check(*reg(PGRAPH_INTR) == 0, "   PGRAPH_INTR is cleared");
    check(!(*reg(PMC_INTR_0) & PMC_PGRAPH), "   PMC_INTR_0's PGRAPH bit is down");
}

static void run_off(void)
{
    check(xbox_Nv2aPgraphTrapOn() == 0, "traps are off");
    *reg(PGRAPH_INTR_EN) = INTR_NOTIFY;     /* a plain write: page writable */
    check(*reg(PGRAPH_INTR_EN) == INTR_NOTIFY, "the PGRAPH page is plain memory");
    check(xbox_Nv2aPgraphRaise(TRAP_ADDR, TRAP_DATA) == 0, "the raise returns 0");
    check(*reg(PGRAPH_TRAPPED_ADDR) == 0 && *reg(PGRAPH_INTR) == 0, "and latches nothing");
}

static void run_spin_off(void)
{
    check(xbox_Nv2aPgraphTrapOn() == 1, "traps are on");
    *reg(PGRAPH_INTR_EN) = INTR_NOTIFY;
    *reg(PGRAPH_FIFO) = 0;
    printf("3. a handler that writes 0, then the bit (2 ms waits)\n");
    raise_and_ack();
}
#endif

static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "on";
    const char *path = argc > 2 ? argv[2] : "tools/conformance/test.xbe";
    size_t xbe_len = 0;
    unsigned char *xbe;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (strcmp(mode, "on") && strcmp(mode, "off") && strcmp(mode, "spin-off")) {
        printf("unknown mode '%s'\n", mode);
        return 2;
    }
    if (strcmp(mode, "off"))
        set_env("RECOMP_PGRAPH_TRAP", "1");
    if (!strcmp(mode, "spin-off"))
        set_env("RECOMP_TRAP_SPIN", "0");

    xbe = load_file(path, &xbe_len);
    if (!xbe) {
        printf("cannot read %s -- run: python tools/conformance/mkxbe.py\n", path);
        return 2;
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_len) || !reg(PGRAPH_INTR)) {
        puts("xbox_MemoryLayoutInit failed");
        return 2;
    }
    printf("mode: %s\n", mode);

#ifdef _WIN32
    if (!strcmp(mode, "on"))
        run_on();
    else if (!strcmp(mode, "off"))
        run_off();
    else
        run_spin_off();
#else
    /* Traps need the Windows page handler; elsewhere they stay off. */
    check(xbox_Nv2aPgraphTrapOn() == 0, "traps are off on this host");
    check(xbox_Nv2aPgraphRaise(TRAP_ADDR, TRAP_DATA) == 0, "the raise returns 0");
    check(xbox_Nv2aPgraphWaitAck(10) == 1, "the wait has nothing to wait for");
#endif

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
