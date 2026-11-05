/*
 * mmio_decode.h -- one x86-64 instruction decoder for trapped MMIO.
 *
 * A device whose registers need semantics cannot be plain memory: the reads
 * have to be answered and the writes have to be seen. The way that works here
 * is to leave the page PAGE_NOACCESS, catch the access in a vectored handler,
 * decode the faulting instruction, service it against the device model, and
 * step over it.
 *
 * That decoder existed twice before this file -- once in nv2a_mmio_hook.c and
 * once in apu_mmio_hook.c -- as the same opcode table written out against two
 * different pairs of accessors. This is the same logic with the accessors
 * passed in, so a third device does not need a third copy. The APU hook has
 * since moved onto it; nv2a_mmio_hook.c still carries its own copy.
 *
 * Header-only and static inline: one function, one caller per device, and a
 * library for it would be more build wiring than code.
 *
 * The instructions covered are what the XDK device code actually emits against
 * registers -- moves both ways, the immediate forms, the zero-extending loads,
 * and the ALU, read-modify-write and flag-setting forms a poll loop is built
 * from (DirectSound polls with `test dword [mem], imm32`, and a C compiler
 * folds `x += REG` into an ALU op with a memory operand).
 * Anything outside that set returns 0 rather than guessing: stepping over an
 * instruction that was not understood corrupts the guest silently, which is
 * far worse than a fault naming the opcode.
 */
#ifndef MMIO_DECODE_H
#define MMIO_DECODE_H

#include <stdint.h>

#if defined(_WIN32) || (defined(__linux__) && defined(__x86_64__))
#define MMIO_DECODE_X64 1

#if defined(_WIN32)
#include <windows.h>

/* The register state the decoder edits: a VEH's CONTEXT on Windows. */
typedef CONTEXT mmio_ctx_t;
#define MMIO_RIP(c) ((c)->Rip)
#define MMIO_EFL(c) ((c)->EFlags)
#else
#include <ucontext.h>

/* ...and a SIGSEGV handler's ucontext_t on x86-64 Linux, whose general
 * registers sit in uc_mcontext.gregs in the kernel's sigcontext order. The
 * REG_* names need _GNU_SOURCE before the first libc header, which a caller
 * cannot always arrange, so the indices are spelled out and checked against
 * the names whenever they are visible. */
typedef ucontext_t mmio_ctx_t;
enum { MMIO_GREG_R8 = 0, MMIO_GREG_R9, MMIO_GREG_R10, MMIO_GREG_R11,
       MMIO_GREG_R12, MMIO_GREG_R13, MMIO_GREG_R14, MMIO_GREG_R15,
       MMIO_GREG_RDI, MMIO_GREG_RSI, MMIO_GREG_RBP, MMIO_GREG_RBX,
       MMIO_GREG_RDX, MMIO_GREG_RAX, MMIO_GREG_RCX, MMIO_GREG_RSP,
       MMIO_GREG_RIP, MMIO_GREG_EFL, MMIO_GREG_CSGSFS, MMIO_GREG_ERR,
       MMIO_GREG_TRAPNO };
#if defined(REG_RIP)
_Static_assert(REG_RIP == MMIO_GREG_RIP && REG_EFL == MMIO_GREG_EFL
               && REG_RAX == MMIO_GREG_RAX && REG_R8 == MMIO_GREG_R8
               && REG_ERR == MMIO_GREG_ERR && REG_TRAPNO == MMIO_GREG_TRAPNO,
               "x86-64 gregs order");
#endif
#define MMIO_RIP(c) ((c)->uc_mcontext.gregs[MMIO_GREG_RIP])
#define MMIO_EFL(c) ((c)->uc_mcontext.gregs[MMIO_GREG_EFL])
/* Bit 1 of the page-fault error code the kernel passes in REG_ERR: the
 * access was a write. REG_ERR holds a page-fault code only for trap 14 (#PF);
 * a SIGSEGV from #GP leaves something else there, so that reads as a read. */
#define MMIO_FAULT_IS_WRITE(c) \
    ((c)->uc_mcontext.gregs[MMIO_GREG_TRAPNO] == 14 \
     && ((c)->uc_mcontext.gregs[MMIO_GREG_ERR] & 2) != 0)
#endif

/* Service one register access. dev is passed straight back to the callbacks. */
typedef uint64_t (*mmio_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_write_fn)(void *dev, uint32_t off, uint64_t val, int size);

#if !defined(_WIN32)
static inline uint64_t *mmio_ctx_reg(mmio_ctx_t *c, int reg)
{
    static const unsigned char greg[16] = {
        MMIO_GREG_RAX, MMIO_GREG_RCX, MMIO_GREG_RDX, MMIO_GREG_RBX,
        MMIO_GREG_RSP, MMIO_GREG_RBP, MMIO_GREG_RSI, MMIO_GREG_RDI,
        MMIO_GREG_R8,  MMIO_GREG_R9,  MMIO_GREG_R10, MMIO_GREG_R11,
        MMIO_GREG_R12, MMIO_GREG_R13, MMIO_GREG_R14, MMIO_GREG_R15 };
    return (uint64_t *)&c->uc_mcontext.gregs[greg[reg & 0xF]];
}
#else
static inline uint64_t *mmio_ctx_reg(mmio_ctx_t *c, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t *)&c->Rax;   case 1:  return (uint64_t *)&c->Rcx;
    case 2:  return (uint64_t *)&c->Rdx;   case 3:  return (uint64_t *)&c->Rbx;
    case 4:  return (uint64_t *)&c->Rsp;   case 5:  return (uint64_t *)&c->Rbp;
    case 6:  return (uint64_t *)&c->Rsi;   case 7:  return (uint64_t *)&c->Rdi;
    case 8:  return (uint64_t *)&c->R8;    case 9:  return (uint64_t *)&c->R9;
    case 10: return (uint64_t *)&c->R10;   case 11: return (uint64_t *)&c->R11;
    case 12: return (uint64_t *)&c->R12;   case 13: return (uint64_t *)&c->R13;
    case 14: return (uint64_t *)&c->R14;   case 15: return (uint64_t *)&c->R15;
    default: return NULL;
    }
}
#endif

static inline int mmio_modrm_len(const uint8_t *ip, int rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm  = (modrm & 7) | (rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3) return 1;
    if ((rm & 7) == 4) len += 1;                    /* SIB    */
    if (mod == 0 && (rm & 7) == 5) len += 4;        /* disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/* The ALU operations, numbered as the reg field of group 1 (80/81/83) and as
 * bits 5:3 of the classic two-operand opcodes. TEST has no slot of its own. */
enum { MMIO_ALU_ADD = 0, MMIO_ALU_OR = 1, MMIO_ALU_AND = 4, MMIO_ALU_SUB = 5,
       MMIO_ALU_XOR = 6, MMIO_ALU_CMP = 7, MMIO_ALU_TEST = 8 };

static inline uint64_t mmio_size_mask(int size)
{
    return size >= 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
}

/* a OP b at the given width, with the flags a following jcc reads: ZF, SF, CF
 * and OF. PF and AF are left alone; nothing branches on them after a register
 * read. The logical operations clear CF and OF, as the hardware does. */
static inline uint64_t mmio_alu(mmio_ctx_t *ctx, int op, uint64_t a, uint64_t b,
                                int size)
{
    uint64_t m = mmio_size_mask(size), sign = 1ULL << (size * 8 - 1), r;
    int cf = 0, of = 0;

    a &= m; b &= m;
    switch (op) {
    case MMIO_ALU_ADD:
        r  = (a + b) & m;
        cf = r < a;
        of = ((~(a ^ b) & (a ^ r)) & sign) != 0;
        break;
    case MMIO_ALU_SUB: case MMIO_ALU_CMP:
        r  = (a - b) & m;
        cf = a < b;
        of = (((a ^ b) & (a ^ r)) & sign) != 0;
        break;
    case MMIO_ALU_OR:  r = a | b; break;
    case MMIO_ALU_XOR: r = a ^ b; break;
    default:           r = a & b; break;                /* AND, TEST */
    }
    MMIO_EFL(ctx) &= ~(0x0001u | 0x0040u | 0x0080u | 0x0800u);
    if (cf)       MMIO_EFL(ctx) |= 0x0001u;               /* CF */
    if (r == 0)   MMIO_EFL(ctx) |= 0x0040u;               /* ZF */
    if (r & sign) MMIO_EFL(ctx) |= 0x0080u;               /* SF */
    if (of)       MMIO_EFL(ctx) |= 0x0800u;               /* OF */
    return r;
}

/* A general register at an operand width. Without a REX prefix, byte
 * registers 4-7 are AH, CH, DH and BH -- bits 15:8 of registers 0-3. */
static inline uint64_t mmio_get_reg(mmio_ctx_t *c, int reg, int size, int has_rex)
{
    if (size == 1 && !has_rex && reg >= 4 && reg < 8)
        return (*mmio_ctx_reg(c, reg - 4) >> 8) & 0xFF;
    return *mmio_ctx_reg(c, reg) & mmio_size_mask(size);
}

/* Write with x86-64 width rules: a 32-bit write zeroes bits 63:32, an 8- or
 * 16-bit write merges into what was there. */
static inline void mmio_set_reg(mmio_ctx_t *c, int reg, uint64_t v, int size,
                                int has_rex)
{
    uint64_t *d;
    if (size == 1 && !has_rex && reg >= 4 && reg < 8) {
        d  = mmio_ctx_reg(c, reg - 4);
        *d = (*d & ~0xFF00ULL) | ((v & 0xFF) << 8);
        return;
    }
    d = mmio_ctx_reg(c, reg);
    if (size == 1)      *d = (*d & ~0xFFULL)   | (v & 0xFF);
    else if (size == 2) *d = (*d & ~0xFFFFULL) | (v & 0xFFFF);
    else if (size == 4) *d = v & 0xFFFFFFFFULL;
    else                *d = v;
}

/* The immediate of an instruction whose operand size is `size`: imm8 for byte
 * forms, imm16 under a 66 prefix, otherwise imm32 sign-extended (REX.W). */
static inline uint64_t mmio_imm(const uint8_t *p, int size, int *len)
{
    if (size == 1) { *len = 1; return p[0]; }
    if (size == 2) { *len = 2; return (uint64_t)p[0] | ((uint64_t)p[1] << 8); }
    *len = 4;
    return (uint64_t)(int64_t)(int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)
                                      | ((uint32_t)p[2] << 16)
                                      | ((uint32_t)p[3] << 24));
}

/* 1 if the instruction at ctx->Rip was serviced and Rip advanced past it.
 *
 * Covered: MOV both ways (88/89 8A/8B C6/C7), MOVZX (0F B6/B7), the
 * two-operand ALU forms in both directions for ADD/OR/AND/SUB/XOR/CMP
 * (00-03 08-0B 20-23 28-2B 30-33 38-3B), group 1 with an immediate
 * (80/81/83, not ADC/SBB), and TEST against a register or an immediate
 * (84/85, F6/F7 /0 and /1). A register-destination form reads the device
 * once; a memory-destination form reads, then writes (CMP and TEST only
 * read). ADC/SBB, the rest of F6/F7 (NOT/NEG/MUL/DIV), string moves and
 * anything else return 0. */
static inline int mmio_emulate(mmio_ctx_t *ctx, uint32_t off, void *dev,
                               mmio_read_fn rd, mmio_write_fn wr)
{
    const uint8_t *ip = (const uint8_t *)(uintptr_t)MMIO_RIP(ctx);
    int prefix = 0, has66 = 0, rex = 0, has_rex = 0;
    const uint8_t *op;
    int size, rex_w, rex_r, rex_b, mlen, reg, ilen, alu;
    uint64_t m, v, imm;

    if (!rd || !wr)
        return 0;

    for (;;) {
        uint8_t b = ip[prefix];
        if (b == 0x66)                   { has66 = 1; prefix++; }
        else if (b == 0xF2 || b == 0xF3) { prefix++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix++; }
        else break;
    }
    rex_w = has_rex && (rex & 0x08);
    rex_r = has_rex && (rex & 0x04);
    rex_b = has_rex && (rex & 0x01);

    op   = ip + prefix;
    size = rex_w ? 8 : (has66 ? 2 : 4);

    /* Two-operand ALU: 00-03 ADD, 08-0B OR, 20-23 AND, 28-2B SUB, 30-33 XOR,
     * 38-3B CMP. Bit 0 clear is the byte form; bit 1 set makes the register
     * the destination. 10-1B (ADC/SBB) are not decoded. */
    if (op[0] < 0x40 && (op[0] & 7) < 4) {
        alu = op[0] >> 3;
        if (alu == 2 || alu == 3)
            return 0;
        if (!(op[0] & 1)) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        if (op[0] & 2) {                             /* op r, r/m      */
            v = mmio_alu(ctx, alu, mmio_get_reg(ctx, reg, size, has_rex), m,
                         size);
            if (alu != MMIO_ALU_CMP)
                mmio_set_reg(ctx, reg, v, size, has_rex);
        } else {                                     /* op r/m, r      */
            v = mmio_alu(ctx, alu, m, mmio_get_reg(ctx, reg, size, has_rex),
                         size);
            if (alu != MMIO_ALU_CMP)
                wr(dev, off, v, size);
        }
        MMIO_RIP(ctx) += prefix + 1 + mlen;
        return 1;
    }

    switch (op[0]) {
    case 0x88: case 0x89:                            /* MOV r/m, r   (write) */
        if (op[0] == 0x88) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, mmio_get_reg(ctx, reg, size, has_rex), size);
        MMIO_RIP(ctx) += prefix + 1 + mlen;
        return 1;

    case 0xC6: case 0xC7:                            /* MOV r/m, imm         */
        if (op[0] == 0xC6) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        imm  = mmio_imm(op + 1 + mlen, size, &ilen);
        wr(dev, off, imm & mmio_size_mask(size), size);
        MMIO_RIP(ctx) += prefix + 1 + mlen + ilen;
        return 1;

    case 0x8A: case 0x8B:                            /* MOV r, r/m   (read)  */
        if (op[0] == 0x8A) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        mmio_set_reg(ctx, reg, rd(dev, off, size), size, has_rex);
        MMIO_RIP(ctx) += prefix + 1 + mlen;
        return 1;

    case 0x84: case 0x85:                            /* TEST r/m, r          */
        if (op[0] == 0x84) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        mmio_alu(ctx, MMIO_ALU_TEST, rd(dev, off, size),
                 mmio_get_reg(ctx, reg, size, has_rex), size);
        MMIO_RIP(ctx) += prefix + 1 + mlen;
        return 1;

    case 0x80: case 0x81: case 0x83:                 /* group 1 r/m, imm     */
        alu = (op[1] >> 3) & 7;
        if (alu == 2 || alu == 3)                    /* ADC, SBB             */
            return 0;
        if (op[0] == 0x80) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        if (op[0] == 0x83) {                         /* imm8, sign-extended  */
            imm  = (uint64_t)(int64_t)(int8_t)op[1 + mlen];
            ilen = 1;
        } else {
            imm = mmio_imm(op + 1 + mlen, size, &ilen);
        }
        v = mmio_alu(ctx, alu, rd(dev, off, size), imm, size);
        if (alu != MMIO_ALU_CMP)
            wr(dev, off, v, size);
        MMIO_RIP(ctx) += prefix + 1 + mlen + ilen;
        return 1;

    case 0xF6: case 0xF7:                            /* TEST r/m, imm        */
        if (((op[1] >> 3) & 7) > 1)                  /* /1 aliases /0        */
            return 0;
        if (op[0] == 0xF6) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        imm  = mmio_imm(op + 1 + mlen, size, &ilen);
        mmio_alu(ctx, MMIO_ALU_TEST, rd(dev, off, size), imm, size);
        MMIO_RIP(ctx) += prefix + 1 + mlen + ilen;
        return 1;

    case 0x0F:
        if (op[1] == 0xB6 || op[1] == 0xB7) {        /* MOVZX r, r/m8|16     */
            int s = (op[1] == 0xB6) ? 1 : 2;
            mlen  = mmio_modrm_len(op + 2, rex_b);
            reg   = ((op[2] >> 3) & 7) | (rex_r ? 8 : 0);
            mmio_set_reg(ctx, reg, rd(dev, off, s) & mmio_size_mask(s),
                         rex_w ? 8 : (has66 ? 2 : 4), 1);
            MMIO_RIP(ctx) += prefix + 2 + mlen;
            return 1;
        }
        return 0;

    default:
        return 0;
    }
}

#endif /* MMIO_DECODE_X64 */
#endif /* MMIO_DECODE_H */
