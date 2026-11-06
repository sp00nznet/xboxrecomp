/*
 * mmio_decode_x86_64.h -- the x86-64 Linux twin of mmio_decode_a64.h, for
 * trapped MMIO on a POSIX host.
 *
 * The device page is PROT_NONE (or PROT_READ for the AC'97 write trap); the
 * access arrives as SIGSEGV with the faulting data address in si_addr and the
 * thread state in the ucontext. The decoder reads the faulting instruction at
 * RIP, works out direction, width and destination/source register, performs
 * the access through the callbacks, and advances RIP past the instruction.
 * This is the POSIX/Linux x86-64 counterpart of mmio_decode_a64.h.
 *
 * The address itself comes from si_addr, so the ModRM/SIB decode here exists
 * to get the *length* right (a wrong RIP advance is an immediate crash loop),
 * not to reconstruct the effective address. RIP-relative mod=00 rm=101 is
 * decoded with its disp32, so its length is right too.
 *
 * Covered forms: MOV 88/89 8A/8B, MOV moffs A0-A3, MOVZX 0F B6/B7, MOVSX
 * 0F BE/BF, MOV r/m,imm C6/C7, MOVD/MOVQ 0F 6E/7E between XMM and r/m, and
 * MOVUPS/MOVAPS 0F 10/11 (four dword accesses). Anything else returns 0 and
 * logs the faulting RIP plus the first eight opcode bytes once per unique
 * opcode -- stepping an instruction that was not understood corrupts the
 * guest silently, which is far worse than a fault naming the opcode.
 */
#ifndef MMIO_DECODE_X86_64_H
#define MMIO_DECODE_X86_64_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#if defined(__x86_64__) && defined(__linux__)

/* REG_* in <sys/ucontext.h> is glibc's __USE_GNU set. Define it here as well
 * so this header works whether or not the including TU asked for it first;
 * the fallback numbers are the x86-64 Linux gregset ABI and are used when a
 * system header was pulled in under -std=c11 before this point. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <sys/ucontext.h>

#ifndef REG_RIP
#define REG_R8   0
#define REG_R9   1
#define REG_R10  2
#define REG_R11  3
#define REG_R12  4
#define REG_R13  5
#define REG_R14  6
#define REG_R15  7
#define REG_RDI  8
#define REG_RSI  9
#define REG_RBP  10
#define REG_RBX  11
#define REG_RDX  12
#define REG_RAX  13
#define REG_RCX  14
#define REG_RSP  15
#define REG_RIP  16
#define REG_EFL  17
#endif

typedef ucontext_t mmio_ctx_t;

#define MMIO_CTX_PC(uc)        ((uintptr_t)((uc)->uc_mcontext.gregs[REG_RIP]))
#define MMIO_CTX_SET_PC(uc, v) ((uc)->uc_mcontext.gregs[REG_RIP] = (greg_t)(v))

typedef uint64_t (*mmio_x86_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_x86_write_fn)(void *dev, uint32_t off, uint64_t val, int size);

/* x86 encoding order, not gregset order. */
static inline int mmio_x86_greg_index(int r)
{
    static const int map[16] = {
        REG_RAX, REG_RCX, REG_RDX, REG_RBX,
        REG_RSP, REG_RBP, REG_RSI, REG_RDI,
        REG_R8,  REG_R9,  REG_R10, REG_R11,
        REG_R12, REG_R13, REG_R14, REG_R15
    };
    return map[r & 15];
}

static inline uint64_t mmio_x86_get_reg(mmio_ctx_t *uc, int reg)
{
    uint64_t v;

    memcpy(&v, &uc->uc_mcontext.gregs[mmio_x86_greg_index(reg)], sizeof v);
    return v;
}

static inline void mmio_x86_put_reg(mmio_ctx_t *uc, int reg, uint64_t v)
{
    memcpy(&uc->uc_mcontext.gregs[mmio_x86_greg_index(reg)], &v, sizeof v);
}

static inline uint64_t mmio_x86_size_mask(int size)
{
    return size >= 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
}

/* Read a general register at an operand width. Without a REX prefix, byte
 * registers 4-7 are AH, CH, DH and BH -- bits 15:8 of registers 0-3. */
static inline uint64_t mmio_x86_read_operand_reg(mmio_ctx_t *uc, int reg,
                                                 int size, int has_rex)
{
    if (size == 1 && !has_rex && reg >= 4 && reg < 8)
        return (mmio_x86_get_reg(uc, reg - 4) >> 8) & 0xFF;
    return mmio_x86_get_reg(uc, reg) & mmio_x86_size_mask(size);
}

/* Write with x86-64 width rules: a 32-bit write zeroes bits 63:32, an 8- or
 * 16-bit write merges into what was there. */
static inline void mmio_x86_write_operand_reg(mmio_ctx_t *uc, int reg,
                                              uint64_t v, int size,
                                              int has_rex)
{
    uint64_t cur;

    if (size == 1 && !has_rex && reg >= 4 && reg < 8) {
        cur = mmio_x86_get_reg(uc, reg - 4);
        cur = (cur & ~0xFF00ULL) | ((v & 0xFF) << 8);
        mmio_x86_put_reg(uc, reg - 4, cur);
        return;
    }
    if (size == 8) {
        mmio_x86_put_reg(uc, reg, v);
        return;
    }
    if (size == 4) {
        /* A 32-bit write zeroes bits 63:32; a merge would keep stale bits. */
        mmio_x86_put_reg(uc, reg, v & 0xFFFFFFFFu);
        return;
    }
    cur = mmio_x86_get_reg(uc, reg);
    cur = (cur & ~mmio_x86_size_mask(size)) | (v & mmio_x86_size_mask(size));
    mmio_x86_put_reg(uc, reg, cur);
}

/* The XMM file lives in the FXSAVE area. The kernel points fpregs at it on
 * signal delivery; __fpregs_mem is the in-ucontext copy for the case where it
 * does not. */
static inline uint8_t *mmio_x86_xmm(mmio_ctx_t *uc, int reg)
{
    struct _libc_fpstate *fp = uc->uc_mcontext.fpregs;

    if (!fp)
        fp = &uc->__fpregs_mem;
    return (uint8_t *)&fp->_xmm[reg & 15];
}

/* Decode one ModRM byte plus any SIB/displacement that follows it. Returns
 * the operand length in bytes and, for a memory operand, mod/reg/rm and
 * whether mod=00 rm=101 (RIP-relative). For mod=3, *reg and *rm may be
 * register numbers; callers that only service memory reject that. */
static inline int mmio_x86_modrm(const uint8_t *p, int rex_r, int rex_b,
                                 int *mod, int *reg, int *rm, int *rip_rel)
{
    uint8_t b = p[0];
    int m = (b >> 6) & 3;
    int r = (b >> 3) & 7;
    int rmv = b & 7;
    int len = 1;

    *mod = m;
    *reg = r | (rex_r ? 8 : 0);
    *rm  = rmv | (rex_b ? 8 : 0);
    *rip_rel = 0;

    if (m == 3)
        return len;

    if (rmv == 4) {                         /* SIB */
        uint8_t sib = p[1];
        len++;
        if (m == 0 && (sib & 7) == 5)
            len += 4;                       /* no base: disp32 */
    } else if (m == 0 && rmv == 5) {
        *rip_rel = 1;                       /* RIP + disp32 */
        len += 4;
    }
    if (m == 1)
        len += 1;
    else if (m == 2)
        len += 4;
    return len;
}

/* imm8 / imm16 / imm32 (sign-extended for a 64-bit operand). */
static inline uint64_t mmio_x86_imm(const uint8_t *p, int size, int *len)
{
    if (size == 1) {
        *len = 1;
        return p[0];
    }
    if (size == 2) {
        *len = 2;
        return (uint64_t)p[0] | ((uint64_t)p[1] << 8);
    }
    *len = 4;
    return (uint64_t)(int64_t)(int32_t)((uint32_t)p[0]
                                      | ((uint32_t)p[1] << 8)
                                      | ((uint32_t)p[2] << 16)
                                      | ((uint32_t)p[3] << 24));
}

/* One-shot diagnostic: one line per unique opcode prefix, not per fault. A
 * title can take thousands of faults on a form that is not decoded, and a
 * line per fault both floods the log and hides the other forms. */
static inline void mmio_x86_report_undecoded(mmio_ctx_t *uc)
{
    static uint32_t seen[64];
    static int nseen;
    const uint8_t *ip = (const uint8_t *)(uintptr_t)MMIO_CTX_PC(uc);
    uint32_t key = 0;
    uintptr_t page_end = (MMIO_CTX_PC(uc) | 0xFFFu) + 1;
    int n = (int)(page_end - MMIO_CTX_PC(uc));
    int i;

    if (n > 8)
        n = 8;
    if (n <= 0)
        n = 1;
    for (i = 0; i < n && i < 4; i++)
        key |= (uint32_t)ip[i] << (i * 8);
    for (i = 0; i < nseen; i++)
        if (seen[i] == key)
            return;
    if (nseen < (int)(sizeof seen / sizeof seen[0]))
        seen[nseen++] = key;

    fprintf(stderr, "[MMIO] x86-64 undecodable opcode at RIP=%p:",
            (void *)MMIO_CTX_PC(uc));
    for (i = 0; i < n; i++)
        fprintf(stderr, " %02X", ip[i]);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* 1 if the instruction at RIP was serviced against (rd, wr) and RIP advanced.
 * off is the device offset of the faulting data address (from si_addr). */
static inline int mmio_emulate_x86_64(mmio_ctx_t *uc, uint32_t off, void *dev,
                                      mmio_x86_read_fn rd,
                                      mmio_x86_write_fn wr)
{
    const uint8_t *ip = (const uint8_t *)(uintptr_t)MMIO_CTX_PC(uc);
    const uint8_t *op;
    int prefix = 0, has66 = 0, has67 = 0, has_rex = 0, rex = 0;
    int rex_w, rex_r, rex_b, rep = 0;
    int size, mlen, mod, reg, rm, rip_rel, ilen;
    uint64_t v = 0, imm;

    if (!rd || !wr)
        return 0;

    for (;;) {
        uint8_t b = ip[prefix];

        if (b == 0x66) {
            has66 = 1;
            has_rex = 0;
            prefix++;
        } else if (b == 0x67) {
            has67 = 1;
            has_rex = 0;
            prefix++;
        } else if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26
                || b == 0x64 || b == 0x65 || b == 0xF0
                || b == 0xF2 || b == 0xF3) {
            if (b == 0xF2 || b == 0xF3)
                rep = b;
            /* A legacy prefix after REX makes it not a REX prefix. */
            has_rex = 0;
            prefix++;
        } else if (b >= 0x40 && b <= 0x4F) {
            rex = b;
            has_rex = 1;
            prefix++;
        } else {
            break;
        }
    }

    rex_w = has_rex && (rex & 0x08) ? 1 : 0;
    rex_r = has_rex && (rex & 0x04) ? 1 : 0;
    rex_b = has_rex && (rex & 0x01) ? 1 : 0;
    op = ip + prefix;
    size = rex_w ? 8 : (has66 ? 2 : 4);

    switch (op[0]) {
    case 0x88:                                  /* MOV r/m8, r8   (store) */
    case 0x89: {                                /* MOV r/m, r     (store) */
        if (op[0] == 0x88)
            size = 1;
        mlen = mmio_x86_modrm(op + 1, rex_r, rex_b,
                              &mod, &reg, &rm, &rip_rel);
        if (mod == 3)
            goto undecoded;
        v = mmio_x86_read_operand_reg(uc, reg, size, has_rex);
        wr(dev, off, v, size);
        MMIO_CTX_SET_PC(uc, MMIO_CTX_PC(uc) + prefix + 1 + mlen);
        return 1;
    }

    case 0x8A:                                  /* MOV r8, r/m8   (load) */
    case 0x8B: {                                /* MOV r, r/m     (load) */
        if (op[0] == 0x8A)
            size = 1;
        mlen = mmio_x86_modrm(op + 1, rex_r, rex_b,
                              &mod, &reg, &rm, &rip_rel);
        if (mod == 3)
            goto undecoded;
        v = rd(dev, off, size);
        mmio_x86_write_operand_reg(uc, reg, v, size, has_rex);
        MMIO_CTX_SET_PC(uc, MMIO_CTX_PC(uc) + prefix + 1 + mlen);
        return 1;
    }

    case 0xA0:                                  /* MOV AL, moffs8  (load) */
    case 0xA1:                                  /* MOV eAX, moffs  (load) */
    case 0xA2:                                  /* MOV moffs8, AL  (store) */
    case 0xA3: {                                /* MOV moffs, eAX  (store) */
        int asize = has67 ? 4 : 8;              /* 67: 32-bit moffs */

        if (op[0] == 0xA0 || op[0] == 0xA2)
            size = 1;
        if (op[0] == 0xA0 || op[0] == 0xA1) {
            v = rd(dev, off, size);
            mmio_x86_write_operand_reg(uc, 0, v, size, has_rex);
        } else {
            v = mmio_x86_read_operand_reg(uc, 0, size, has_rex);
            wr(dev, off, v, size);
        }
        MMIO_CTX_SET_PC(uc, MMIO_CTX_PC(uc) + prefix + 1 + asize);
        return 1;
    }

    case 0xC6:                                  /* MOV r/m8, imm8  (store) */
    case 0xC7: {                                /* MOV r/m, imm    (store) */
        if (op[0] == 0xC6)
            size = 1;
        mlen = mmio_x86_modrm(op + 1, rex_r, rex_b,
                              &mod, &reg, &rm, &rip_rel);
        if (mod == 3)
            goto undecoded;
        imm = mmio_x86_imm(op + 1 + mlen, size, &ilen);
        wr(dev, off, imm & mmio_x86_size_mask(size), size);
        MMIO_CTX_SET_PC(uc, MMIO_CTX_PC(uc) + prefix + 1 + mlen + ilen);
        return 1;
    }

    case 0x0F:
        if (op[1] == 0xB6 || op[1] == 0xB7    /* MOVZX r, r/m8|16 */
                || op[1] == 0xBE || op[1] == 0xBF) {  /* MOVSX r, r/m8|16 */
            int src_size = (op[1] == 0xB6 || op[1] == 0xBE) ? 1 : 2;
            int dst_size = rex_w ? 8 : (has66 ? 2 : 4);
            int is_signed = (op[1] == 0xBE || op[1] == 0xBF);

            mlen = mmio_x86_modrm(op + 2, rex_r, rex_b,
                                  &mod, &reg, &rm, &rip_rel);
            if (mod == 3)
                goto undecoded;
            v = rd(dev, off, src_size) & mmio_x86_size_mask(src_size);
            if (is_signed) {
                if (src_size == 1)
                    v = (uint64_t)(int64_t)(int8_t)v;
                else
                    v = (uint64_t)(int64_t)(int16_t)v;
            }
            mmio_x86_write_operand_reg(uc, reg, v, dst_size, 1);
            MMIO_CTX_SET_PC(uc, MMIO_CTX_PC(uc) + prefix + 2 + mlen);
            return 1;
        }

        /* MOVD/MOVQ between XMM and r/m. The 66 prefix selects the XMM
         * form; without it the same opcodes address the MMX file, which is
         * not tracked here. F3 0F 7E is the load form MOVQ xmm, r/m64. */
        if (op[1] == 0x6E || op[1] == 0x7E) {
            int load;

            if (op[1] == 0x6E) {
                if (!has66)
                    goto undecoded;
                load = 1;
                size = rex_w ? 8 : 4;
            } else if (rep == 0xF3) {
                load = 1;
                size = 8;
            } else {
                /* F2 0F 7E is another (non-66) form; do not guess its width. */
                if (!has66 || rep == 0xF2)
                    goto undecoded;
                load = 0;
                size = rex_w ? 8 : 4;
            }
            mlen = mmio_x86_modrm(op + 2, rex_r, rex_b,
                                  &mod, &reg, &rm, &rip_rel);
            if (mod == 3)
                goto undecoded;
            if (load) {
                uint8_t *x = mmio_x86_xmm(uc, reg);
                v = rd(dev, off, size);
                memset(x, 0, 16);
                memcpy(x, &v, (size_t)size);
            } else {
                const uint8_t *x = mmio_x86_xmm(uc, reg);
                memcpy(&v, x, (size_t)size);
                wr(dev, off, v, size);
            }
            MMIO_CTX_SET_PC(uc, MMIO_CTX_PC(uc) + prefix + 2 + mlen);
            return 1;
        }

        /* MOVUPS/MOVAPS: 16 bytes, issued as four dword accesses because
         * the device callbacks are dword-granular. F2/F3 are MOVSD/MOVSS
         * (8/4 bytes) and are not these forms. */
        if (op[1] == 0x10 || op[1] == 0x11) {
            int is_load = op[1] == 0x10;
            int i;

            if (rep == 0xF2 || rep == 0xF3)
                goto undecoded;
            mlen = mmio_x86_modrm(op + 2, rex_r, rex_b,
                                  &mod, &reg, &rm, &rip_rel);
            if (mod == 3)
                goto undecoded;
            if (is_load) {
                uint8_t *x = mmio_x86_xmm(uc, reg);
                for (i = 0; i < 4; i++) {
                    uint32_t d = (uint32_t)rd(dev, off + (uint32_t)i * 4, 4);
                    memcpy(x + i * 4, &d, 4);
                }
            } else {
                const uint8_t *x = mmio_x86_xmm(uc, reg);
                for (i = 0; i < 4; i++) {
                    uint32_t d;
                    memcpy(&d, x + i * 4, 4);
                    wr(dev, off + (uint32_t)i * 4, d, 4);
                }
            }
            MMIO_CTX_SET_PC(uc, MMIO_CTX_PC(uc) + prefix + 2 + mlen);
            return 1;
        }
        goto undecoded;

    default:
        goto undecoded;
    }

undecoded:
    mmio_x86_report_undecoded(uc);
    return 0;
}

#endif /* __x86_64__ && __linux__ */
#endif /* MMIO_DECODE_X86_64_H */
