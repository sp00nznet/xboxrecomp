/*
 * MCPX APU MMIO Hook - trapped APU register access
 *
 * Decodes the faulting instruction with the shared trapped-MMIO decoders
 * (src/platform/mmio_decode.h on x86-64 under a VEH, mmio_decode_a64.h on
 * POSIX arm64, mmio_decode_x86_64.h on POSIX x86-64 Linux -- both under
 * SIGBUS/SIGSEGV) and routes reads/writes through the MCPX APU register
 * handlers.
 */

#include "apu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

/* Global APU state pointer -- referenced from main.c regardless of which
 * platform's MMIO hook is active, so define it before the #if guard. */
MCPXAPUState *g_apu_state = NULL;

/* Windows: a VEH hands over the CONTEXT. POSIX arm64 and x86-64 Linux: a
 * SIGBUS/SIGSEGV handler hands over the ucontext (apu_hook_handle_mmio_posix,
 * at the end). */
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

bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write)
{
    uint32_t mmio_offset = fault_xbox_va - APU_MMIO_BASE;
    bool ok = apu_decode_and_handle(ctx, mmio_offset, is_write);

    /* What the title actually asks the APU for. The DSPs are stubbed here, so
     * a title that waits on one waits forever, and the only way to work out
     * what it is waiting for is to see the register traffic that precedes the
     * wait. */
    if (apu_env_on("RECOMP_APU_TRACE")) {
        static unsigned n;
        if (n++ < 400) {
            /* The value as well as the offset: finding which register carries
             * the command-block address means recognising the address when it
             * goes past, and an offset alone never shows it. */
            uint64_t v = g_apu_state
                       ? mcpx_apu_mmio_read(g_apu_state, mmio_offset, 4) : 0;
            fprintf(stderr, "  [APUMMIO] %s 0x%05X = %08X%s\n",
                    is_write ? "write" : "read ", mmio_offset,
                    (uint32_t)v, ok ? "" : "  (decode failed)");
        }
    }
    return ok;
}

#endif /* _WIN32 */

#if !defined(_WIN32) && (defined(__aarch64__) || (defined(__x86_64__) && defined(__linux__)))
/* POSIX: the same hook over sigaction + ucontext, with the A64 decoder
 * (src/platform/mmio_decode_a64.h) on arm64 and the x86-64 decoder
 * (src/platform/mmio_decode_x86_64.h) on Linux. The host's fault handler
 * reaches it through xbox_PosixMmioFault before it reports a crash. */
#include <signal.h>
#if defined(__aarch64__)
#include "../platform/mmio_decode_a64.h"
#define MMIO_CTX_PC(uc) A64_CTX_PC(uc)
#else
#include "../platform/mmio_decode_x86_64.h"
#endif

#define APU_MMIO_BASE  0xFE800000u
#define APU_MMIO_SIZE  0x00080000u

static int g_apu_mmio_decode_fail;

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
    if (size == 8) {
        mcpx_apu_mmio_write((MCPXAPUState *)dev, off, (uint32_t)v, 4);
        mcpx_apu_mmio_write((MCPXAPUState *)dev, off + 4, (uint32_t)(v >> 32), 4);
        return;
    }
    mcpx_apu_mmio_write((MCPXAPUState *)dev, off, v, (unsigned)size);
}

/* Service one trapped APU access from a SIGBUS/SIGSEGV handler. ucv is the
 * handler's ucontext; is_write is advisory (the decoder reads the opcode).
 * Returns true when the access was emulated and pc advanced. */
bool apu_hook_handle_mmio_posix(void *ucv, uintptr_t fault_addr,
                                uint32_t fault_xbox_va, int is_write)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    uint32_t off = fault_xbox_va - APU_MMIO_BASE;

    (void)fault_addr; (void)is_write;
    if (!g_apu_state || off >= APU_MMIO_SIZE)
        return false;
#if defined(__aarch64__)
    if (mmio_emulate_a64(uc, off, g_apu_state, apu_mmio_rd, apu_mmio_wr))
#else
    if (mmio_emulate_x86_64(uc, off, g_apu_state, apu_mmio_rd, apu_mmio_wr))
#endif
        return true;
    g_apu_mmio_decode_fail++;
    /* stdio in a signal handler is not async-signal-safe. The process is
     * about to die of this fault anyway, and the line is what says why;
     * capped at 20, as on Windows. */
    if (g_apu_mmio_decode_fail <= 20)
        fprintf(stderr, "[APU] MMIO decode fail at PC=%p insn=%08X offset=0x%X\n",
                (void *)(uintptr_t)MMIO_CTX_PC(uc),
                *(const uint32_t *)(uintptr_t)MMIO_CTX_PC(uc), off);
    return false;
}
#endif
