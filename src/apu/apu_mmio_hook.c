/*
 * MCPX APU MMIO Hook - trapped APU register access
 *
 * Decodes the faulting instruction with the shared trapped-MMIO decoders
 * (src/platform/mmio_decode.h on x86-64 under a VEH or, on Linux, a SIGSEGV
 * handler; mmio_decode_a64.h on POSIX arm64 under SIGBUS/SIGSEGV) and routes
 * reads/writes through the MCPX APU register handlers.
 */

#include "apu.h"
#include "recomp_env.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

/* Global APU state pointer -- referenced from main.c regardless of which
 * platform's MMIO hook is active, so define it before the #if guard. */
MCPXAPUState *g_apu_state = NULL;

/* Windows: a VEH hands over the CONTEXT. POSIX arm64 and x86-64 Linux: a
 * SIGBUS/SIGSEGV handler hands over the ucontext (apu_hook_handle_mmio_posix,
 * at the end). Other POSIX hosts have no trap. */
#if defined(_WIN32)
#include <windows.h>
#include "../platform/mmio_decode.h"

/* APU MMIO base in Xbox VA space */
#define APU_MMIO_BASE  0xFE800000u
#define APU_MMIO_SIZE  0x00080000u  /* 512KB */

/* (g_apu_state is defined above, outside the Win32 guard) */

/* Statistics */
static int g_apu_mmio_read_count = 0;
static int g_apu_mmio_write_count = 0;
static int g_apu_mmio_decode_fail = 0;

/* The value the decoder last handed the model, so the RECOMP_APU_TRACE
 * histogram can show what was written (a read-back of a PIO method offset
 * does not return it). Racy across faulting threads; diagnostic only. */
static volatile uint32_t s_last_wval;

/* ============================================================
 * Instruction decoding: the shared trapped-MMIO decoder
 * (src/platform/mmio_decode.h, covered by tests/mmio_decode), with the
 * APU register model behind it. It also serves the ALU and TEST-immediate
 * forms DirectSound polls with (test dword [reg+disp], imm32), which a
 * private MOV/TEST/CMP/OR/AND decoder here did not.
 * ============================================================ */

/* The model's registers are 32-bit and its read/write ignore the size, so an
 * 8-byte access (the decoder emits size 8 for REX.W) is two 4-byte accesses
 * here; handing it over whole drops the high dword on a store and returns
 * zero for it on a load. The POSIX arm64 hook below splits the same way. */
static uint64_t apu_mmio_rd(void *dev, uint32_t off, int size)
{
    g_apu_mmio_read_count++;
    if (size == 8) {
        uint64_t lo = mcpx_apu_mmio_read((MCPXAPUState *)dev, off, 4);
        uint64_t hi = mcpx_apu_mmio_read((MCPXAPUState *)dev, off + 4, 4);
        return lo | (hi << 32);
    }
    return mcpx_apu_mmio_read((MCPXAPUState *)dev, off, (unsigned)size);
}

static void apu_mmio_wr(void *dev, uint32_t off, uint64_t v, int size)
{
    g_apu_mmio_write_count++;
    s_last_wval = (uint32_t)v;
    if (size == 8) {
        mcpx_apu_mmio_write((MCPXAPUState *)dev, off, (uint32_t)v, 4);
        mcpx_apu_mmio_write((MCPXAPUState *)dev, off + 4, (uint32_t)(v >> 32), 4);
        return;
    }
    mcpx_apu_mmio_write((MCPXAPUState *)dev, off, v, (unsigned)size);
}

static bool apu_decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset, int is_write)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;

    (void)is_write;
    if (!g_apu_state) return false;
    if (mmio_emulate(ctx, mmio_offset, g_apu_state, apu_mmio_rd, apu_mmio_wr))
        return true;

    /* Unrecognized */
    g_apu_mmio_decode_fail++;
    if (g_apu_mmio_decode_fail <= 20) {
        fprintf(stderr, "[APU] MMIO decode fail at RIP=%p offset=0x%X: %02X %02X %02X %02X %02X %02X\n",
                (void*)ctx->Rip, mmio_offset, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return false;
}

/* ============================================================
 * Public API (called from VEH in main.c)
 * ============================================================ */

/* RECOMP_APU_TRACE statistics.
 *
 * The 400-line trace shows the first accesses only, and the decoder's own
 * counters were never printed, so neither the steady-state rate nor which
 * GP/EP registers a title writes could be read from a log. With the trace on,
 * every access is tallied per dword offset and direction, and a reporter
 * thread prints a stats line every 10 s and the per-offset table every 30 s
 * (periodic, because a watchdog exit or a SIGINT never reaches atexit):
 *
 *   [APUMMIO-STATS] t=..s reads=N (+n/s) writes=N (+n/s) direct=N fails=N
 *                   handler=..us
 *   [APUMMIO-HIST] 0xOFFSET r=N w=N lastw=VALUE
 *
 * reads and writes count every access; direct is the part that came through
 * apu_hook_direct_rd/_wr (a constant-address mov in generated code) rather
 * than a fault, so reads + writes - direct is what still traps. handler is
 * the time spent inside the decoder and model per access; the exception
 * round trip around it is not visible from here (see apu_hook_fault_bench). */
#define APU_HIST_SLOTS (APU_MMIO_SIZE / 4)
static int s_trace = -1;               /* looked up once, not per access */
static volatile LONG s_hist_r[APU_HIST_SLOTS], s_hist_w[APU_HIST_SLOTS];
static volatile uint32_t s_hist_lastw[APU_HIST_SLOTS];
static volatile LONG64 s_tot_r, s_tot_w, s_direct, s_handler_ticks;
static volatile LONG s_reporter_started;
/* Set by apu_hook_fault_bench (apu_fault_bench.c) while it runs. */
volatile LONG g_apu_hook_bench_active;
static LARGE_INTEGER s_qpf;

static void apu_trace_report(int with_table, double t)
{
    static LONG64 prev_r, prev_w;
    static double prev_t;
    LONG64 r = s_tot_r, w = s_tot_w, d = s_direct, ticks = s_handler_ticks;
    double dt = t - prev_t > 0 ? t - prev_t : 1;
    fprintf(stderr, "[APUMMIO-STATS] t=%.1fs reads=%lld (+%.0f/s) writes=%lld"
            " (+%.0f/s) direct=%lld fails=%d handler=%.2fus/access\n", t,
            (long long)r, (r - prev_r) / dt, (long long)w, (w - prev_w) / dt,
            (long long)d, g_apu_mmio_decode_fail,
            (r + w) ? (double)ticks * 1e6 / (double)s_qpf.QuadPart / (double)(r + w)
                    : 0.0);
    prev_r = r; prev_w = w; prev_t = t;
    if (with_table) {
        for (uint32_t i = 0; i < APU_HIST_SLOTS; i++)
            if (s_hist_r[i] || s_hist_w[i])
                fprintf(stderr, "[APUMMIO-HIST] 0x%05X r=%ld w=%ld lastw=%08X\n",
                        i * 4, (long)s_hist_r[i], (long)s_hist_w[i],
                        s_hist_lastw[i]);
    }
    fflush(stderr);
}

static DWORD WINAPI apu_trace_reporter(LPVOID unused)
{
    LARGE_INTEGER t0, now;
    unsigned k = 0;
    (void)unused;
    QueryPerformanceCounter(&t0);
    for (;;) {
        Sleep(10000);
        QueryPerformanceCounter(&now);
        apu_trace_report(++k % 3 == 0,
                         (double)(now.QuadPart - t0.QuadPart) / (double)s_qpf.QuadPart);
    }
    return 0;
}

static int apu_trace_on(void)
{
    if (s_trace < 0) {
        s_trace = recomp_env_on(RENV_APU_TRACE);
        QueryPerformanceFrequency(&s_qpf);
    }
    return s_trace;
}

/* Start the RECOMP_APU_TRACE reporter (every 10 s). Called once from
 * mcpx_apu_init_standalone, not from the fault handler: creating a thread
 * inside a vectored exception handler takes the loader lock. */
void apu_hook_trace_start(void)
{
    if (!apu_trace_on())
        return;
    if (!InterlockedExchange(&s_reporter_started, 1)) {
        HANDLE h = CreateThread(NULL, 0, apu_trace_reporter, NULL, 0, NULL);
        if (h)
            CloseHandle(h);
    }
}

/* Tally one traced access, faulted or direct. rval is the value a read
 * returned when the caller has it; a fault handler does not (the decoder put
 * it straight in a register), so the listing reads the model again. */
static void apu_trace_tally(uint32_t mmio_offset, int is_write, bool ok,
                            const uint32_t *rval)
{
    if (mmio_offset < APU_MMIO_SIZE) {
        uint32_t slot = mmio_offset >> 2;
        if (is_write) {
            InterlockedIncrement64(&s_tot_w);
            InterlockedIncrement(&s_hist_w[slot]);
            s_hist_lastw[slot] = s_last_wval;
        } else {
            InterlockedIncrement64(&s_tot_r);
            InterlockedIncrement(&s_hist_r[slot]);
        }
    }
    /* What the title actually asks the APU for. The DSPs are stubbed here, so
     * a title that waits on one waits forever, and the only way to work out
     * what it is waiting for is to see the register traffic that precedes the
     * wait. */
    {
        static unsigned n;
        if (n++ < 400) {
            /* The value as well as the offset: finding which register carries
             * the command-block address means recognising the address when it
             * goes past, and an offset alone never shows it. A write shows the
             * value written; a read, what the model returned. */
            uint64_t v = is_write ? s_last_wval
                       : rval ? *rval
                       : g_apu_state
                       ? mcpx_apu_mmio_read(g_apu_state, mmio_offset, 4) : 0;
            fprintf(stderr, "  [APUMMIO] %s 0x%05X = %08X%s\n",
                    is_write ? "write" : "read ", mmio_offset,
                    (uint32_t)v, ok ? "" : "  (decode failed)");
        }
    }
}

bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write)
{
    uint32_t mmio_offset = fault_xbox_va - APU_MMIO_BASE;
    LARGE_INTEGER a, b;
    int trace = apu_trace_on() && !g_apu_hook_bench_active;

    (void)fault_addr;
    if (trace)
        QueryPerformanceCounter(&a);
    bool ok = apu_decode_and_handle(ctx, mmio_offset, is_write);
    if (!trace)
        return ok;
    QueryPerformanceCounter(&b);
    InterlockedExchangeAdd64(&s_handler_ticks, b.QuadPart - a.QuadPart);
    apu_trace_tally(mmio_offset, is_write, ok, NULL);
    return ok;
}

uint32_t apu_hook_direct_rd(uint32_t off, int size)
{
    LARGE_INTEGER a, b;
    int trace = apu_trace_on() && !g_apu_hook_bench_active;
    uint32_t v;

    if (trace)
        QueryPerformanceCounter(&a);
    v = (uint32_t)apu_mmio_rd(g_apu_state, off, size);
    if (!trace)
        return v;
    QueryPerformanceCounter(&b);
    InterlockedExchangeAdd64(&s_handler_ticks, b.QuadPart - a.QuadPart);
    InterlockedIncrement64(&s_direct);
    apu_trace_tally(off, 0, true, &v);
    return v;
}

void apu_hook_direct_wr(uint32_t off, uint32_t v, int size)
{
    LARGE_INTEGER a, b;
    int trace = apu_trace_on() && !g_apu_hook_bench_active;

    if (trace)
        QueryPerformanceCounter(&a);
    apu_mmio_wr(g_apu_state, off, v, size);
    if (!trace)
        return;
    QueryPerformanceCounter(&b);
    InterlockedExchangeAdd64(&s_handler_ticks, b.QuadPart - a.QuadPart);
    InterlockedIncrement64(&s_direct);
    apu_trace_tally(off, 1, true, NULL);
}

#endif /* _WIN32 */

#if !defined(_WIN32)
#include "../platform/mmio_decode.h"
#endif
#if !defined(_WIN32) && (defined(__aarch64__) || defined(MMIO_DECODE_X64))
/* POSIX: the same hook over sigaction + ucontext, with the A64 decoder
 * (src/platform/mmio_decode_a64.h) on arm64 and the Windows x86-64 one
 * (mmio_decode.h, over the ucontext gregs) on x86-64 Linux. The host's fault
 * handler reaches it through xbox_PosixMmioFault before it reports a crash. */
#include <pthread.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include "../platform/mmio_decode_a64.h"

#define APU_MMIO_SIZE  0x00080000u

static uint64_t s_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int s_trace = -1;
static volatile uint64_t s_tot_r, s_tot_w, s_direct, s_handler_ns;
static volatile uint32_t s_hist_r[APU_MMIO_SIZE / 4], s_hist_w[APU_MMIO_SIZE / 4];
static volatile uint32_t s_hist_lastw[APU_MMIO_SIZE / 4];
static int g_apu_mmio_decode_fail;
static volatile uint32_t s_last_wval;
volatile int g_apu_hook_bench_active;

/* The model's registers are 32-bit and its read/write ignore the size, so an
 * 8-byte access (ldr/str x, a d register, each half of a q register) is two
 * 4-byte accesses here, as on Windows. */
static uint64_t apu_mmio_rd(void *dev, uint32_t off, int size)
{
    if (size == 8) {
        uint64_t lo = mcpx_apu_mmio_read((MCPXAPUState *)dev, off, 4);
        uint64_t hi = mcpx_apu_mmio_read((MCPXAPUState *)dev, off + 4, 4);
        return lo | (hi << 32);
    }
    return mcpx_apu_mmio_read((MCPXAPUState *)dev, off, (unsigned)size);
}

static void apu_mmio_wr(void *dev, uint32_t off, uint64_t v, int size)
{
    s_last_wval = (uint32_t)v;
    if (size == 8) {
        mcpx_apu_mmio_write((MCPXAPUState *)dev, off, (uint32_t)v, 4);
        mcpx_apu_mmio_write((MCPXAPUState *)dev, off + 4, (uint32_t)(v >> 32), 4);
        return;
    }
    mcpx_apu_mmio_write((MCPXAPUState *)dev, off, v, (unsigned)size);
}

static void *apu_trace_reporter(void *unused)
{
    uint64_t t0 = s_now_ns(), prev_r = 0, prev_w = 0;
    unsigned k = 0;
    (void)unused;
    for (;;) {
        struct timespec ts = { 10, 0 };
        uint64_t r, w, ns, now;
        nanosleep(&ts, NULL);
        r = s_tot_r; w = s_tot_w; ns = s_handler_ns; now = s_now_ns();
        fprintf(stderr, "[APUMMIO-STATS] t=%.1fs reads=%llu (+%.0f/s) writes=%llu"
                " (+%.0f/s) direct=%llu fails=%d handler=%.2fus/access\n",
                (now - t0) / 1e9, (unsigned long long)r, (r - prev_r) / 10.0,
                (unsigned long long)w, (w - prev_w) / 10.0,
                (unsigned long long)s_direct, g_apu_mmio_decode_fail,
                (r + w) ? (double)ns / 1e3 / (double)(r + w) : 0.0);
        prev_r = r; prev_w = w;
        if (++k % 3 == 0) {
            for (uint32_t i = 0; i < APU_MMIO_SIZE / 4; i++)
                if (s_hist_r[i] || s_hist_w[i])
                    fprintf(stderr, "[APUMMIO-HIST] 0x%05X r=%u w=%u lastw=%08X\n",
                            i * 4, s_hist_r[i], s_hist_w[i], s_hist_lastw[i]);
        }
        fflush(stderr);
    }
    return NULL;
}

void apu_hook_trace_start(void)
{
    static int started;
    pthread_t th;
    if (s_trace < 0)
        s_trace = recomp_env_on(RENV_APU_TRACE);
    if (!s_trace || started)
        return;
    started = 1;
    if (pthread_create(&th, NULL, apu_trace_reporter, NULL) == 0)
        pthread_detach(th);
}

/* Tally one traced access, faulted or direct; rval as on Win32. The fault
 * handler runs in a signal handler and the direct path on the guest thread,
 * so plain increments (diagnostic counts, racy as before). */
static void apu_trace_tally(uint32_t off, int w, const uint32_t *rval)
{
    uint32_t slot = off >> 2;
    if (w) { s_tot_w++; s_hist_w[slot]++; s_hist_lastw[slot] = s_last_wval; }
    else   { s_tot_r++; s_hist_r[slot]++; }
    {
        static unsigned n;
        if (n++ < 400)
            fprintf(stderr, "  [APUMMIO] %s 0x%05X = %08X\n", w ? "write" : "read ",
                    off, w ? s_last_wval
                           : rval ? *rval
                           : (uint32_t)mcpx_apu_mmio_read(g_apu_state, off, 4));
    }
}

static int apu_trace_now(void)
{
    if (s_trace < 0)
        s_trace = recomp_env_on(RENV_APU_TRACE);
    return s_trace && !g_apu_hook_bench_active;
}

uint32_t apu_hook_direct_rd(uint32_t off, int size)
{
    int trace = apu_trace_now();
    uint64_t a = trace ? s_now_ns() : 0;
    uint32_t v = (uint32_t)apu_mmio_rd(g_apu_state, off, size);
    if (trace) {
        s_handler_ns += s_now_ns() - a;
        s_direct++;
        apu_trace_tally(off, 0, &v);
    }
    return v;
}

void apu_hook_direct_wr(uint32_t off, uint32_t v, int size)
{
    int trace = apu_trace_now();
    uint64_t a = trace ? s_now_ns() : 0;
    apu_mmio_wr(g_apu_state, off, v, size);
    if (trace) {
        s_handler_ns += s_now_ns() - a;
        s_direct++;
        apu_trace_tally(off, 1, NULL);
    }
}

/* Service one trapped APU access from a SIGBUS/SIGSEGV handler. ucv is the
 * handler's ucontext; is_write is advisory (the decoder reads the opcode).
 * Returns true when the access was emulated and pc advanced. */
bool apu_hook_handle_mmio_posix(void *ucv, uintptr_t fault_addr,
                                uint32_t fault_xbox_va, int is_write)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    uint32_t off = fault_xbox_va - 0xFE800000u;
    uint64_t a = 0;
    int trace, ok;

    (void)fault_addr; (void)is_write;
    if (!g_apu_state || off >= APU_MMIO_SIZE)
        return false;
    trace = apu_trace_now();
    if (trace)
        a = s_now_ns();
#if defined(__aarch64__)
    ok = mmio_emulate_a64(uc, off, g_apu_state, apu_mmio_rd, apu_mmio_wr);
#else
    /* The page-fault error code says which way before the decoder runs. */
    is_write = MMIO_FAULT_IS_WRITE(uc);
    ok = mmio_emulate(uc, off, g_apu_state, apu_mmio_rd, apu_mmio_wr);
#endif
    if (!ok) {
        g_apu_mmio_decode_fail++;
        /* stdio in a signal handler is not async-signal-safe. The process is
         * about to die of this fault anyway, and the line is what says why;
         * capped at 20, as on Windows. */
#if defined(__aarch64__)
        if (g_apu_mmio_decode_fail <= 20)
            fprintf(stderr, "[APU] MMIO decode fail at PC=%p insn=%08X offset=0x%X\n",
                    (void *)(uintptr_t)A64_CTX_PC(uc),
                    *(const uint32_t *)(uintptr_t)A64_CTX_PC(uc), off);
#else
        if (g_apu_mmio_decode_fail <= 20) {
            const uint8_t *ip = (const uint8_t *)(uintptr_t)MMIO_RIP(uc);
            fprintf(stderr, "[APU] MMIO decode fail at RIP=%p bytes=%02X %02X %02X"
                            " %02X %02X %02X offset=0x%X\n", (const void *)ip,
                    ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], off);
        }
#endif
        return false;
    }
    if (!trace)
        return true;
    s_handler_ns += s_now_ns() - a;
#if defined(__aarch64__)
    {
        /* Direction from the opcode: bit 22 clear is a store for the
         * single-register forms; LDP/STP carry L in bit 22 as well. */
        uint32_t insn = *(const uint32_t *)(uintptr_t)(A64_CTX_PC(uc) - 4);
        apu_trace_tally(off, !((insn >> 22) & 1), NULL);
    }
#else
    apu_trace_tally(off, is_write, NULL);
#endif
    return true;
}
#elif !defined(_WIN32)
/* Other POSIX hosts (x86-64 macOS): there is no fault decoder here, but
 * with RECOMP_AC97_READY=1 the compat VirtualProtect (mprotect) still arms
 * the trap, so xbox_MmioRead/Write route constant-address movs to these
 * untraced calls into the model (a fault there would have no handler). */
void apu_hook_trace_start(void) {}

uint32_t apu_hook_direct_rd(uint32_t off, int size)
{
    return (uint32_t)mcpx_apu_mmio_read(g_apu_state, off, (unsigned)size);
}

void apu_hook_direct_wr(uint32_t off, uint32_t v, int size)
{
    mcpx_apu_mmio_write(g_apu_state, off, v, (unsigned)size);
}
#endif
