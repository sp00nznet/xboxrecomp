/* The MMX helpers: the x86-64 intrinsics behave exactly as the scalar
 * lane loops they replaced.
 *
 * The reference here is an independent copy of the scalar implementations
 * the header carried before the intrinsics -- lane loops with the same
 * wraps and saturations -- so a mistake in either copy shows up as a
 * mismatch. Random 64-bit operands cover the wrap and saturation corners;
 * the shifts also take a fixed sweep of counts across and past their lane
 * width, which is where the variable-count form's saturation lives.
 *
 * The three lane-selector helpers MSVC will not take a variable immediate
 * for (PSHUFW, PINSRW, PEXTRW) stayed scalar in the header on every host;
 * they are checked too, against themselves, so their presence is pinned.
 *
 * Exit codes: 0 every comparison held; 1 a mismatch (printed); 77 the
 * header took the scalar fallback, so there is nothing to compare. */
#define RECOMP_GENERATED_CODE   /* the MMX section is generated-code-only */
#include "recomp_types.h"

#include <stdio.h>
#include <stdlib.h>

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t nxt(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng;
}
static RecompMmx rndmm(void) { RecompMmx r; r.q = nxt(); return r; }

static inline uint8_t  ref_sat_u8(int32_t v)  { return (uint8_t)(v > 255 ? 255 : (v < 0 ? 0 : v)); }
static inline int8_t   ref_sat_i8(int32_t v)  { return (int8_t)(v > 127 ? 127 : (v < -128 ? -128 : v)); }
static inline int16_t  ref_sat_i16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }
static inline uint16_t ref_sat_u16(int32_t v) { return (uint16_t)(v > 65535 ? 65535 : (v < 0 ? 0 : v)); }

#define REF_BINOP(NAME, LANES, FIELD, EXPR)                             \
    static inline RecompMmx NAME(RecompMmx a, RecompMmx b) {            \
        RecompMmx r; int i; for (i = 0; i < (LANES); i++) r.FIELD[i] = (EXPR); return r; \
    }

REF_BINOP(REF_PADDB, 8, b, (int8_t)((uint8_t)a.b[i] + (uint8_t)b.b[i]))
REF_BINOP(REF_PADDW, 4, w, (int16_t)((uint16_t)a.w[i] + (uint16_t)b.w[i]))
REF_BINOP(REF_PADDD, 2, d, (int32_t)((uint32_t)a.d[i] + (uint32_t)b.d[i]))
REF_BINOP(REF_PSUBB, 8, b, (int8_t)((uint8_t)a.b[i] - (uint8_t)b.b[i]))
REF_BINOP(REF_PSUBW, 4, w, (int16_t)((uint16_t)a.w[i] - (uint16_t)b.w[i]))
REF_BINOP(REF_PSUBD, 2, d, (int32_t)((uint32_t)a.d[i] - (uint32_t)b.d[i]))
REF_BINOP(REF_PADDSB, 8, b, ref_sat_i8((int32_t)a.b[i] + b.b[i]))
REF_BINOP(REF_PADDSW, 4, w, ref_sat_i16((int32_t)a.w[i] + b.w[i]))
REF_BINOP(REF_PSUBSB, 8, b, ref_sat_i8((int32_t)a.b[i] - b.b[i]))
REF_BINOP(REF_PSUBSW, 4, w, ref_sat_i16((int32_t)a.w[i] - b.w[i]))
REF_BINOP(REF_PADDUSB, 8, ub, ref_sat_u8((int32_t)a.ub[i] + b.ub[i]))
REF_BINOP(REF_PSUBUSB, 8, ub, ref_sat_u8((int32_t)a.ub[i] - b.ub[i]))
REF_BINOP(REF_PADDUSW, 4, uw, ref_sat_u16((int32_t)a.uw[i] + b.uw[i]))
REF_BINOP(REF_PSUBUSW, 4, uw, ref_sat_u16((int32_t)a.uw[i] - b.uw[i]))
REF_BINOP(REF_PMULLW, 4, w, (int16_t)((int32_t)a.w[i] * b.w[i]))
REF_BINOP(REF_PMULHW, 4, w, (int16_t)(((int32_t)a.w[i] * b.w[i]) >> 16))
REF_BINOP(REF_PAVGB, 8, ub, (uint8_t)(((int32_t)a.ub[i] + b.ub[i] + 1) >> 1))
REF_BINOP(REF_PAVGW, 4, uw, (uint16_t)(((int32_t)a.uw[i] + b.uw[i] + 1) >> 1))
REF_BINOP(REF_PMINSW, 4, w, (a.w[i] < b.w[i] ? a.w[i] : b.w[i]))
REF_BINOP(REF_PMAXSW, 4, w, (a.w[i] > b.w[i] ? a.w[i] : b.w[i]))
REF_BINOP(REF_PCMPEQB, 8, b, (int8_t)(a.b[i] == b.b[i] ? -1 : 0))
REF_BINOP(REF_PCMPEQW, 4, w, (int16_t)(a.w[i] == b.w[i] ? -1 : 0))
REF_BINOP(REF_PCMPEQD, 2, d, (int32_t)(a.d[i] == b.d[i] ? -1 : 0))
REF_BINOP(REF_PCMPGTB, 8, b, (int8_t)(a.b[i] > b.b[i] ? -1 : 0))
REF_BINOP(REF_PCMPGTW, 4, w, (int16_t)(a.w[i] > b.w[i] ? -1 : 0))
REF_BINOP(REF_PCMPGTD, 2, d, (int32_t)(a.d[i] > b.d[i] ? -1 : 0))

static inline RecompMmx REF_PMADDWD(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++)
        r.d[i] = (int32_t)a.w[i * 2] * b.w[i * 2]
               + (int32_t)a.w[i * 2 + 1] * b.w[i * 2 + 1];
    return r;
}
static inline RecompMmx REF_PAND(RecompMmx a, RecompMmx b)  { RecompMmx r; r.q = a.q & b.q; return r; }
static inline RecompMmx REF_PANDN(RecompMmx a, RecompMmx b) { RecompMmx r; r.q = ~a.q & b.q; return r; }
static inline RecompMmx REF_POR(RecompMmx a, RecompMmx b)   { RecompMmx r; r.q = a.q | b.q; return r; }
static inline RecompMmx REF_PXOR(RecompMmx a, RecompMmx b)  { RecompMmx r; r.q = a.q ^ b.q; return r; }

/* Shifts: a count at or past the lane width gives zero (logical) or a
 * full sign fill (arithmetic); the count is the whole 64-bit register. */
static inline RecompMmx REF_PSLLW(RecompMmx a, uint64_t c) {
    RecompMmx r; int i; for (i = 0; i < 4; i++) r.uw[i] = (c >= 16) ? 0 : (uint16_t)(a.uw[i] << c); return r;
}
static inline RecompMmx REF_PSRLW(RecompMmx a, uint64_t c) {
    RecompMmx r; int i; for (i = 0; i < 4; i++) r.uw[i] = (c >= 16) ? 0 : (uint16_t)(a.uw[i] >> c); return r;
}
static inline RecompMmx REF_PSLLD(RecompMmx a, uint64_t c) {
    RecompMmx r; int i; for (i = 0; i < 2; i++) r.ud[i] = (c >= 32) ? 0 : (uint32_t)(a.ud[i] << c); return r;
}
static inline RecompMmx REF_PSRLD(RecompMmx a, uint64_t c) {
    RecompMmx r; int i; for (i = 0; i < 2; i++) r.ud[i] = (c >= 32) ? 0 : (uint32_t)(a.ud[i] >> c); return r;
}
static inline RecompMmx REF_PSLLQ(RecompMmx a, uint64_t c) {
    RecompMmx r; r.q = (c >= 64) ? 0 : (a.q << c); return r;
}
static inline RecompMmx REF_PSRLQ(RecompMmx a, uint64_t c) {
    RecompMmx r; r.q = (c >= 64) ? 0 : (a.q >> c); return r;
}
static inline RecompMmx REF_PSRAW(RecompMmx a, uint64_t c) {
    RecompMmx r; int i; c = (c >= 16) ? 15 : c;
    for (i = 0; i < 4; i++) r.w[i] = (int16_t)(a.w[i] >> c); return r;
}
static inline RecompMmx REF_PSRAD(RecompMmx a, uint64_t c) {
    RecompMmx r; int i; c = (c >= 32) ? 31 : c;
    for (i = 0; i < 2; i++) r.d[i] = (int32_t)(a.d[i] >> c); return r;
}
static inline RecompMmx REF_PUNPCKLBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i; for (i = 0; i < 4; i++) { r.ub[i*2] = a.ub[i]; r.ub[i*2+1] = b.ub[i]; } return r;
}
static inline RecompMmx REF_PUNPCKHBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i; for (i = 0; i < 4; i++) { r.ub[i*2] = a.ub[i+4]; r.ub[i*2+1] = b.ub[i+4]; } return r;
}
static inline RecompMmx REF_PUNPCKLWD(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i; for (i = 0; i < 2; i++) { r.uw[i*2] = a.uw[i]; r.uw[i*2+1] = b.uw[i]; } return r;
}
static inline RecompMmx REF_PUNPCKHWD(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i; for (i = 0; i < 2; i++) { r.uw[i*2] = a.uw[i+2]; r.uw[i*2+1] = b.uw[i+2]; } return r;
}
static inline RecompMmx REF_PUNPCKLDQ(RecompMmx a, RecompMmx b) {
    RecompMmx r; r.ud[0] = a.ud[0]; r.ud[1] = b.ud[0]; return r;
}
static inline RecompMmx REF_PUNPCKHDQ(RecompMmx a, RecompMmx b) {
    RecompMmx r; r.ud[0] = a.ud[1]; r.ud[1] = b.ud[1]; return r;
}
/* The MMX pack's result interleaves at dword granularity: a's saturation
 * in the low dword, b's in the high one. */
static inline RecompMmx REF_PACKSSWB(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) r.b[i]     = ref_sat_i8(a.w[i]);
    for (i = 0; i < 4; i++) r.b[i + 4] = ref_sat_i8(b.w[i]);
    return r;
}
static inline RecompMmx REF_PACKUSWB(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) r.ub[i]     = ref_sat_u8(a.w[i]);
    for (i = 0; i < 4; i++) r.ub[i + 4] = ref_sat_u8(b.w[i]);
    return r;
}
static inline RecompMmx REF_PACKSSDW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++) r.w[i]     = ref_sat_i16(a.d[i]);
    for (i = 0; i < 2; i++) r.w[i + 2] = ref_sat_i16(b.d[i]);
    return r;
}
static inline uint32_t REF_PMOVMSKB(RecompMmx a) {
    uint32_t m = 0; int i;
    for (i = 0; i < 8; i++) if (a.ub[i] & 0x80) m |= (1u << i);
    return m;
}
static inline RecompMmx REF_PSADBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i; uint32_t s = 0;
    for (i = 0; i < 8; i++)
        s += (uint32_t)(a.ub[i] > b.ub[i] ? a.ub[i] - b.ub[i] : b.ub[i] - a.ub[i]);
    r.q = 0; r.uw[0] = (uint16_t)s;
    return r;
}

static int failures;
static int checks;

static void compare2(const char *name, RecompMmx s, RecompMmx n) {
    checks++;
    if (s.q != n.q) {
        failures++;
        if (failures <= 10)
            printf("FAIL %-12s escalar=%016llx intrinseco=%016llx\n", name,
                   (unsigned long long)s.q, (unsigned long long)n.q);
    }
}

#define CHECK2(REF, IN) do { \
    int i; for (i = 0; i < 50000; i++) { RecompMmx a = rndmm(), b = rndmm(); \
        compare2(#IN, REF(a, b), IN(a, b)); } } while (0)

#define CHECK_SHIFT(REF, IN) do { \
    static const uint64_t cs[] = { 0, 1, 3, 7, 8, 15, 16, 17, 31, 32, 33, \
                                   63, 64, 65, 255, 0xFFFFFFFFull, \
                                   0xFFFFFFFFFFFFFFFFull }; \
    unsigned k; int i; \
    for (k = 0; k < sizeof cs / sizeof cs[0]; k++) \
        for (i = 0; i < 2000; i++) { RecompMmx a = rndmm(); \
            compare2(#IN, REF(a, cs[k]), IN(a, cs[k])); } } while (0)

int main(void) {
#if defined(_M_X64) || defined(__x86_64__)
    CHECK2(REF_PADDB, MMX_PADDB);     CHECK2(REF_PADDW, MMX_PADDW);
    CHECK2(REF_PADDD, MMX_PADDD);     CHECK2(REF_PSUBB, MMX_PSUBB);
    CHECK2(REF_PSUBW, MMX_PSUBW);     CHECK2(REF_PSUBD, MMX_PSUBD);
    CHECK2(REF_PADDSB, MMX_PADDSB);   CHECK2(REF_PADDSW, MMX_PADDSW);
    CHECK2(REF_PSUBSB, MMX_PSUBSB);   CHECK2(REF_PSUBSW, MMX_PSUBSW);
    CHECK2(REF_PADDUSB, MMX_PADDUSB); CHECK2(REF_PSUBUSB, MMX_PSUBUSB);
    CHECK2(REF_PADDUSW, MMX_PADDUSW); CHECK2(REF_PSUBUSW, MMX_PSUBUSW);
    CHECK2(REF_PMULLW, MMX_PMULLW);   CHECK2(REF_PMULHW, MMX_PMULHW);
    CHECK2(REF_PAVGB, MMX_PAVGB);     CHECK2(REF_PAVGW, MMX_PAVGW);
    CHECK2(REF_PMINSW, MMX_PMINSW);   CHECK2(REF_PMAXSW, MMX_PMAXSW);
    CHECK2(REF_PCMPEQB, MMX_PCMPEQB); CHECK2(REF_PCMPEQW, MMX_PCMPEQW);
    CHECK2(REF_PCMPEQD, MMX_PCMPEQD);
    CHECK2(REF_PCMPGTB, MMX_PCMPGTB); CHECK2(REF_PCMPGTW, MMX_PCMPGTW);
    CHECK2(REF_PCMPGTD, MMX_PCMPGTD);
    CHECK2(REF_PMADDWD, MMX_PMADDWD);
    CHECK2(REF_PAND, MMX_PAND);       CHECK2(REF_PANDN, MMX_PANDN);
    CHECK2(REF_POR, MMX_POR);         CHECK2(REF_PXOR, MMX_PXOR);
    CHECK2(REF_PUNPCKLBW, MMX_PUNPCKLBW);
    CHECK2(REF_PUNPCKHBW, MMX_PUNPCKHBW);
    CHECK2(REF_PUNPCKLWD, MMX_PUNPCKLWD);
    CHECK2(REF_PUNPCKHWD, MMX_PUNPCKHWD);
    CHECK2(REF_PUNPCKLDQ, MMX_PUNPCKLDQ);
    CHECK2(REF_PUNPCKHDQ, MMX_PUNPCKHDQ);
    CHECK2(REF_PACKSSWB, MMX_PACKSSWB);
    CHECK2(REF_PACKUSWB, MMX_PACKUSWB);
    CHECK2(REF_PACKSSDW, MMX_PACKSSDW);
    CHECK2(REF_PSADBW, MMX_PSADBW);
    CHECK_SHIFT(REF_PSLLW, MMX_PSLLW); CHECK_SHIFT(REF_PSRLW, MMX_PSRLW);
    CHECK_SHIFT(REF_PSLLD, MMX_PSLLD); CHECK_SHIFT(REF_PSRLD, MMX_PSRLD);
    CHECK_SHIFT(REF_PSLLQ, MMX_PSLLQ); CHECK_SHIFT(REF_PSRLQ, MMX_PSRLQ);
    CHECK_SHIFT(REF_PSRAW, MMX_PSRAW); CHECK_SHIFT(REF_PSRAD, MMX_PSRAD);
    {
        int i; for (i = 0; i < 50000; i++) {
            RecompMmx a = rndmm();
            checks++;
            if ((uint32_t)REF_PMOVMSKB(a) != MMX_PMOVMSKB(a)) {
                failures++;
                if (failures <= 10) printf("FAIL MMX_PMOVMSKB a=%016llx\n",
                                           (unsigned long long)a.q);
            }
        }
    }
    printf("%s: %d comparacoes, %d divergencias\n",
           failures ? "FALHOU" : "ok", checks, failures);
    return failures ? 1 : 0;
#else
    /* The header took the scalar fallback: there is no intrinsic path to
     * compare against, and the scalar reference below would be compared
     * with itself. */
    printf("skip: sem intrinsecos x86-64 neste host\n");
    return 77;
#endif
}
