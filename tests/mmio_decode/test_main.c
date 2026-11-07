/* Windows only: the trapped-MMIO decoder it covers is the x86-64 one under a Win32 VEH;
 * this host uses mmio_decode_a64.h, which tests/mmio_decode_a64 covers.
 */
#if defined(_WIN32)
/*
 * mmio_decode - does the trapped-MMIO instruction decoder service what the
 * XDK's device code emits, and refuse what it does not understand.
 *
 * This is testable without a device or a fault because the decoder only ever
 * reads instruction bytes at ctx->Rip and edits a CONTEXT. Point Rip at a
 * buffer of hand-encoded instructions and the whole thing is a pure function.
 *
 * The cases that matter are the ones where being wrong is silent: an
 * instruction length that leaves Rip mid-instruction, a 32-bit read that does
 * not clear the high half of the destination, flags that send a poll loop the
 * wrong way, and an unrecognised opcode reported as handled -- which steps
 * over an instruction nobody decoded and corrupts the guest with no message.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>

#include "mmio_decode.h"

static int failures;
#define CHECK(name, cond) \
    do { if (cond) {} else { printf("FAIL: %s\n", name); failures++; } } while (0)
#define CHECK_U64(name, got, want) \
    do { uint64_t g_=(uint64_t)(got), w_=(uint64_t)(want); \
         if (g_ == w_) {} else { \
            printf("FAIL: %s (got 0x%llX, want 0x%llX)\n", name, \
                   (unsigned long long)g_, (unsigned long long)w_); \
            failures++; } } while (0)

/* A device that just records what it was asked for. */
static struct {
    uint32_t off; uint64_t val; int size; int reads, writes;
    uint64_t next_read;
} dev;

static uint64_t t_read(void *d, uint32_t off, int size)
{
    (void)d; dev.off = off; dev.size = size; dev.reads++;
    return dev.next_read;
}
static void t_write(void *d, uint32_t off, uint64_t v, int size)
{
    (void)d; dev.off = off; dev.val = v; dev.size = size; dev.writes++;
}

/* Run one instruction. Returns what mmio_emulate returned; `code` is the
 * encoding, and Rip is expected to land exactly past it. */
static int run(const uint8_t *code, size_t len, CONTEXT *ctx)
{
    int r;
    memset(&dev, 0, sizeof dev);
    dev.next_read = 0;
    ctx->Rip = (DWORD64)(uintptr_t)code;
    r = mmio_emulate(ctx, 0x54, NULL, t_read, t_write);
    if (r) {
        uint64_t advanced = ctx->Rip - (DWORD64)(uintptr_t)code;
        if (advanced != len) {
            printf("FAIL: Rip advanced %llu, instruction is %llu bytes\n",
                   (unsigned long long)advanced, (unsigned long long)len);
            failures++;
        }
    }
    return r;
}

/* run() with the device answering `next_read` to the read. */
static int run_rd(const uint8_t *code, size_t len, CONTEXT *ctx,
                  uint64_t next_read)
{
    int r;
    memset(&dev, 0, sizeof dev);
    dev.next_read = next_read;
    ctx->Rip = (DWORD64)(uintptr_t)code;
    r = mmio_emulate(ctx, 0x54, NULL, t_read, t_write);
    if (r && ctx->Rip - (DWORD64)(uintptr_t)code != len) {
        printf("FAIL: Rip advanced %llu, instruction is %llu bytes\n",
               (unsigned long long)(ctx->Rip - (DWORD64)(uintptr_t)code),
               (unsigned long long)len);
        failures++;
    }
    return r;
}

int main(void)
{
    CONTEXT ctx;
    printf("mmio_decode: running\n");
    memset(&ctx, 0, sizeof ctx);

    /* mov [rax], ecx  -- 89 08 */
    { const uint8_t c[] = { 0x89, 0x08 };
      ctx.Rcx = 0xDEADBEEF;
      CHECK("mov r/m,r handled", run(c, sizeof c, &ctx));
      CHECK_U64("mov r/m,r value", dev.val, 0xDEADBEEF);
      CHECK_U64("mov r/m,r size",  dev.size, 4);
      CHECK_U64("mov r/m,r offset", dev.off, 0x54); }

    /* mov [rax], cl   -- 88 08, byte sized */
    { const uint8_t c[] = { 0x88, 0x08 };
      ctx.Rcx = 0xAA;
      CHECK("mov r/m8,r8 handled", run(c, sizeof c, &ctx));
      CHECK_U64("mov r/m8 size", dev.size, 1); }

    /* mov ecx, [rax]  -- 8B 08. A 32-bit read must clear the high half; if it
     * does not, a register holding a stale 64-bit value reads back wrong. */
    { const uint8_t c[] = { 0x8B, 0x08 };
      ctx.Rcx = 0xFFFFFFFFFFFFFFFFULL;
      memset(&dev, 0, sizeof dev);
      ctx.Rip = (DWORD64)(uintptr_t)c;
      dev.next_read = 0x12345678;
      CHECK("mov r,r/m handled", mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
      CHECK_U64("32-bit read clears high half", ctx.Rcx, 0x12345678ULL); }

    /* mov dword [rax], imm32 -- C7 00 EF BE AD DE */
    { const uint8_t c[] = { 0xC7, 0x00, 0xEF, 0xBE, 0xAD, 0xDE };
      CHECK("mov r/m,imm32 handled", run(c, sizeof c, &ctx));
      CHECK_U64("imm32 value", dev.val, 0xDEADBEEF); }

    /* movzx eax, byte [rax] -- 0F B6 00 */
    { const uint8_t c[] = { 0x0F, 0xB6, 0x00 };
      memset(&dev, 0, sizeof dev);
      ctx.Rip = (DWORD64)(uintptr_t)c;
      ctx.Rax = 0xFFFFFFFFFFFFFFFFULL;
      dev.next_read = 0x00000091;
      CHECK("movzx r32,r/m8 handled",
            mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
      CHECK_U64("movzx zero-extends", ctx.Rax, 0x91ULL); }

    /* test [rax], ecx -- 85 08. The whole point is the flags: a poll loop
     * spins on jz/jnz off this. */
    { const uint8_t c[] = { 0x85, 0x08 };
      memset(&dev, 0, sizeof dev);
      ctx.Rip = (DWORD64)(uintptr_t)c;
      ctx.Rcx = 0x00000001; ctx.EFlags = 0;
      dev.next_read = 0x00000001;
      mmio_emulate(&ctx, 0x54, NULL, t_read, t_write);
      CHECK("test bit set -> ZF clear", (ctx.EFlags & 0x40) == 0);

      memset(&dev, 0, sizeof dev);
      ctx.Rip = (DWORD64)(uintptr_t)c;
      dev.next_read = 0x00000002;
      mmio_emulate(&ctx, 0x54, NULL, t_read, t_write);
      CHECK("test bit clear -> ZF set", (ctx.EFlags & 0x40) != 0); }

    /* cmp [rax], ecx -- 39 08. Equal sets ZF; below sets CF. */
    { const uint8_t c[] = { 0x39, 0x08 };
      memset(&dev, 0, sizeof dev);
      ctx.Rip = (DWORD64)(uintptr_t)c;
      ctx.Rcx = 0x10; dev.next_read = 0x10; ctx.EFlags = 0;
      mmio_emulate(&ctx, 0x54, NULL, t_read, t_write);
      CHECK("cmp equal -> ZF", (ctx.EFlags & 0x40) != 0);
      CHECK("cmp equal -> no CF", (ctx.EFlags & 0x01) == 0);

      memset(&dev, 0, sizeof dev);
      ctx.Rip = (DWORD64)(uintptr_t)c;
      ctx.Rcx = 0x20; dev.next_read = 0x10;
      mmio_emulate(&ctx, 0x54, NULL, t_read, t_write);
      CHECK("cmp below -> CF", (ctx.EFlags & 0x01) != 0); }

    /* or [rax], ecx -- 09 08, and and [rax], ecx -- 21 08. Both are
     * read-modify-write: a device that only sees the write loses the bits it
     * already had. */
    { const uint8_t c[] = { 0x09, 0x08 };
      memset(&dev, 0, sizeof dev);
      ctx.Rip = (DWORD64)(uintptr_t)c;
      ctx.Rcx = 0x0F; dev.next_read = 0xF0;
      mmio_emulate(&ctx, 0x54, NULL, t_read, t_write);
      CHECK_U64("or merges existing bits", dev.val, 0xFF);
      CHECK("or reads before writing", dev.reads == 1 && dev.writes == 1); }

    { const uint8_t c[] = { 0x21, 0x08 };
      memset(&dev, 0, sizeof dev);
      ctx.Rip = (DWORD64)(uintptr_t)c;
      ctx.Rcx = 0x0F; dev.next_read = 0xFF;
      mmio_emulate(&ctx, 0x54, NULL, t_read, t_write);
      CHECK_U64("and masks existing bits", dev.val, 0x0F); }

    /* 66 prefix selects 16-bit, REX.W selects 64-bit. */
    { const uint8_t c[] = { 0x66, 0x89, 0x08 };
      ctx.Rcx = 0x1234;
      CHECK("66-prefixed handled", run(c, sizeof c, &ctx));
      CHECK_U64("66 prefix -> 2 bytes", dev.size, 2); }

    { const uint8_t c[] = { 0x48, 0x89, 0x08 };
      CHECK("REX.W handled", run(c, sizeof c, &ctx));
      CHECK_U64("REX.W -> 8 bytes", dev.size, 8); }

    /* disp8 and disp32 forms have to add to the length, or Rip lands inside
     * the instruction and the next decode is garbage.
     * mov [rax+0x10], ecx -- 89 48 10        (mod=01, disp8)
     * mov [rax+0x1000], ecx -- 89 88 00 10 00 00  (mod=10, disp32) */
    { const uint8_t c1[] = { 0x89, 0x48, 0x10 };
      CHECK("disp8 handled", run(c1, sizeof c1, &ctx)); }
    { const uint8_t c2[] = { 0x89, 0x88, 0x00, 0x10, 0x00, 0x00 };
      CHECK("disp32 handled", run(c2, sizeof c2, &ctx)); }
    /* SIB: mov [rax+rbx*1], ecx -- 89 0C 18 */
    { const uint8_t c3[] = { 0x89, 0x0C, 0x18 };
      CHECK("SIB handled", run(c3, sizeof c3, &ctx)); }

    /* --- The ALU and TEST-immediate forms --- */

    /* test dword [rsi+rax+0xFE1FF0], 0x00100000 -- F7 84 06 F0 1F FE 00 00 10
     * 00: ModRM, SIB, disp32 and imm32 in one, the exact instruction DirectSound
     * died on at APU offset 0x2000. 11 bytes; a length short by the SIB or the
     * disp leaves Rip inside it. A clear bit must set ZF, a set one clear it. */
    { const uint8_t c[] = { 0xF7, 0x84, 0x06, 0xF0, 0x1F, 0xFE, 0x00,
                            0x00, 0x00, 0x10, 0x00 };
      CHECK("test m32,imm32 handled", run_rd(c, sizeof c, &ctx, 0));
      CHECK("test bit clear -> ZF", (ctx.EFlags & 0x40) != 0);
      CHECK("test only reads", dev.reads == 1 && dev.writes == 0);
      CHECK_U64("test m32 size", dev.size, 4);
      run_rd(c, sizeof c, &ctx, 0x00100000);
      CHECK("test bit set -> !ZF", (ctx.EFlags & 0x40) == 0); }

    /* test byte [rax], 0x80 -- F6 00 80: SF from bit 7 of a byte. */
    { const uint8_t c[] = { 0xF6, 0x00, 0x80 };
      CHECK("test m8,imm8 handled", run_rd(c, sizeof c, &ctx, 0xFF));
      CHECK("test m8 sign -> SF", (ctx.EFlags & 0x80) != 0);
      CHECK_U64("test m8 size", dev.size, 1); }

    /* test word [rax], 0x8000 -- 66 F7 00 00 80: imm16, not imm32. */
    { const uint8_t c[] = { 0x66, 0xF7, 0x00, 0x00, 0x80 };
      CHECK("test m16,imm16 handled", run_rd(c, sizeof c, &ctx, 0x8000));
      CHECK_U64("test m16 size", dev.size, 2); }

    /* or dword [rax], 0xFFFFFFF0 -- 83 08 F0: imm8 sign-extends. */
    { const uint8_t c[] = { 0x83, 0x08, 0xF0 };
      CHECK("or m32,imm8 handled", run_rd(c, sizeof c, &ctx, 0x5));
      CHECK_U64("imm8 sign-extends", dev.val, 0xFFFFFFF5);
      CHECK("or imm reads then writes", dev.reads == 1 && dev.writes == 1); }

    /* and dword [rax+0x10], 0xFFFF00FF -- 81 60 10 FF 00 FF FF */
    { const uint8_t c[] = { 0x81, 0x60, 0x10, 0xFF, 0x00, 0xFF, 0xFF };
      CHECK("and m32,imm32 handled", run_rd(c, sizeof c, &ctx, 0x12345678));
      CHECK_U64("and imm32 value", dev.val, 0x12340078); }

    /* add word [rax], 0x0102 -- 66 81 00 02 01: imm16 under 66. */
    { const uint8_t c[] = { 0x66, 0x81, 0x00, 0x02, 0x01 };
      CHECK("add m16,imm16 handled", run_rd(c, sizeof c, &ctx, 0xFFFF));
      CHECK_U64("add m16 wraps", dev.val, 0x0101);
      CHECK("add m16 carry -> CF", (ctx.EFlags & 0x01) != 0); }

    /* sub byte [rax], 1 -- 80 28 01 */
    { const uint8_t c[] = { 0x80, 0x28, 0x01 };
      CHECK("sub m8,imm8 handled", run_rd(c, sizeof c, &ctx, 0x00));
      CHECK_U64("sub m8 borrow value", dev.val, 0xFF);
      CHECK("sub m8 borrow -> CF", (ctx.EFlags & 0x01) != 0); }

    /* cmp dword [rax], 3 -- 83 38 03: reads, never writes, flags only. */
    { const uint8_t c[] = { 0x83, 0x38, 0x03 };
      CHECK("cmp m32,imm8 handled", run_rd(c, sizeof c, &ctx, 3));
      CHECK("cmp equal -> ZF", (ctx.EFlags & 0x40) != 0);
      CHECK("cmp never writes", dev.reads == 1 && dev.writes == 0);
      run_rd(c, sizeof c, &ctx, 0x80000000);
      CHECK("cmp INT_MIN-3 -> OF", (ctx.EFlags & 0x800) != 0);
      CHECK("cmp INT_MIN-3 -> !SF", (ctx.EFlags & 0x80) == 0); }

    /* add ecx, [rax] -- 03 08. Register destination: one read, no write,
     * and a 32-bit result clears bits 63:32. */
    { const uint8_t c[] = { 0x03, 0x08 };
      ctx.Rcx = 0xFFFFFFFF00000001ULL;
      CHECK("add r32,m32 handled", run_rd(c, sizeof c, &ctx, 0x10));
      CHECK_U64("add r32 result zero-extends", ctx.Rcx, 0x11);
      CHECK("add r,m never writes", dev.reads == 1 && dev.writes == 0); }

    /* and edx, [rax+rcx] -- 23 14 08: a USB driver masks a status register
     * into a register this way. SIB, register destination, no write. */
    { const uint8_t c[] = { 0x23, 0x14, 0x08 };
      ctx.Rdx = 0xFFFFFFFF000000F0ULL;
      CHECK("and r32,m32 handled", run_rd(c, sizeof c, &ctx, 0x30));
      CHECK_U64("and r32 result zero-extends", ctx.Rdx, 0x30);
      CHECK("and r,m never writes", dev.reads == 1 && dev.writes == 0);
      run_rd(c, sizeof c, &ctx, 0x0F);
      CHECK("and r,m zero -> ZF", (ctx.EFlags & 0x40) != 0); }

    /* cmp ecx, [rax] -- 3B 08: leaves the register alone. */
    { const uint8_t c[] = { 0x3B, 0x08 };
      ctx.Rcx = 0x5;
      CHECK("cmp r32,m32 handled", run_rd(c, sizeof c, &ctx, 0x7));
      CHECK_U64("cmp r,m keeps register", ctx.Rcx, 0x5);
      CHECK("cmp 5-7 -> CF", (ctx.EFlags & 0x01) != 0); }

    /* xor ah, [rax] -- 32 20. Byte register 4 without REX is AH. */
    { const uint8_t c[] = { 0x32, 0x20 };
      ctx.Rax = 0x1122334455AA66CCULL; ctx.Rsp = 0x7777;
      CHECK("xor ah,m8 handled", run_rd(c, sizeof c, &ctx, 0xFF));
      CHECK_U64("xor lands in AH", ctx.Rax, 0x1122334455AA99CCULL);
      CHECK_U64("xor leaves SPL alone", ctx.Rsp, 0x7777);
      ctx.Rax = 0; }

    /* xor spl, [rax] -- 40 32 20. With REX, byte register 4 is SPL. */
    { const uint8_t c[] = { 0x40, 0x32, 0x20 };
      ctx.Rsp = 0x7700;
      CHECK("xor spl,m8 handled", run_rd(c, sizeof c, &ctx, 0x0F));
      CHECK_U64("REX byte reg 4 is SPL", ctx.Rsp, 0x770F); }

    /* mov [rax], bh -- 88 38: writes BH, not DIL. */
    { const uint8_t c[] = { 0x88, 0x38 };
      ctx.Rbx = 0xAB00; ctx.Rdi = 0xCD;
      CHECK("mov m8,bh handled", run(c, sizeof c, &ctx));
      CHECK_U64("mov m8 from BH", dev.val, 0xAB); }

    /* add [rax], ecx / sub [rax], ecx / xor [rax], ecx -- 01 08, 29 08,
     * 31 08: read-modify-write, the device sees the combined value. */
    { const uint8_t c[] = { 0x01, 0x08 };
      ctx.Rcx = 0x10;
      run_rd(c, sizeof c, &ctx, 0x22);
      CHECK_U64("add m,r value", dev.val, 0x32);
      CHECK("add m,r reads then writes", dev.reads == 1 && dev.writes == 1); }
    { const uint8_t c[] = { 0x29, 0x08 };
      ctx.Rcx = 0x10;
      CHECK("sub m,r handled", run_rd(c, sizeof c, &ctx, 0x10));
      CHECK_U64("sub m,r value", dev.val, 0);
      CHECK("sub to zero -> ZF", (ctx.EFlags & 0x40) != 0); }
    { const uint8_t c[] = { 0x31, 0x08 };
      ctx.Rcx = 0xFF;
      CHECK("xor m,r handled", run_rd(c, sizeof c, &ctx, 0x0F));
      CHECK_U64("xor m,r value", dev.val, 0xF0); }

    /* mov word [rax], 0x1234 -- 66 C7 00 34 12: imm16 under 66, so the
     * instruction is 5 bytes, not 7. */
    { const uint8_t c[] = { 0x66, 0xC7, 0x00, 0x34, 0x12 };
      CHECK("mov m16,imm16 handled", run(c, sizeof c, &ctx));
      CHECK_U64("mov m16 imm value", dev.val, 0x1234);
      CHECK_U64("mov m16 size", dev.size, 2); }

    /* A 32-bit write sends 32 bits: mov [rax], ecx with junk in bits 63:32. */
    { const uint8_t c[] = { 0x89, 0x08 };
      ctx.Rcx = 0xFFFFFFFF12345678ULL;
      run(c, sizeof c, &ctx);
      CHECK_U64("mov m32 sends low half", dev.val, 0x12345678); }

    /* What must still be refused: ADC and SBB (the carry is not modelled),
     * and the F7 forms other than TEST. */
    { const uint8_t adc[] = { 0x83, 0x10, 0x01 };
      const uint8_t sbb[] = { 0x19, 0x08 };
      const uint8_t neg[] = { 0xF7, 0x18 };
      ctx.Rip = (DWORD64)(uintptr_t)adc;
      CHECK("adc refused", mmio_emulate(&ctx, 0x54, NULL, t_read, t_write) == 0);
      ctx.Rip = (DWORD64)(uintptr_t)sbb;
      CHECK("sbb refused", mmio_emulate(&ctx, 0x54, NULL, t_read, t_write) == 0);
      ctx.Rip = (DWORD64)(uintptr_t)neg;
      CHECK("neg refused", mmio_emulate(&ctx, 0x54, NULL, t_read, t_write) == 0); }

    /* And the one that must fail. An unknown opcode reported as handled steps
     * over an instruction nobody decoded. */
    { const uint8_t c[] = { 0xF7, 0x10 };                     /* not dword [rax] */
      memset(&dev, 0, sizeof dev);
      ctx.Rip = (DWORD64)(uintptr_t)c;
      CHECK("unknown opcode refused",
            mmio_emulate(&ctx, 0x54, NULL, t_read, t_write) == 0);
      CHECK("refused leaves Rip alone", ctx.Rip == (DWORD64)(uintptr_t)c); }

    if (failures == 0) {
        printf("mmio_decode: ALL PASS\n");
        return 0;
    }
    printf("mmio_decode: %d FAILURE(S)\n", failures);
    return 1;
}

#else
#include <stdio.h>
int main(void) { printf("mmio_decode: Windows-only, skipped\n"); return 77; }
#endif
