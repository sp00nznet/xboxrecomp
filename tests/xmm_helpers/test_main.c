/* The XMM float helpers: the x86-64 intrinsics behave exactly as the
 * scalar lane loops they replaced.
 *
 * The reference here is an independent copy of the scalar implementations
 * the header carried before the intrinsics -- lane loops over the same
 * union -- so a mistake in either copy shows up as a mismatch. Random
 * 128-bit operands cover the ordinary range; a directed pool of lane
 * values (NaNs with payloads, signed zeros, denormals, the infinities,
 * the 2^31 boundary) is swept pairwise, which is where the compare
 * predicates' unordered behaviour and MINPS/MAXPS's second-operand
 * tie-break actually live.
 *
 * The compare predicates are additionally pinned against the hardware
 * semantics stated independently of either implementation: for a pair
 * where either lane is NaN, EQ/LT/LE/ORD are false and UNORD/NEQ/NLT/NLE
 * are true. Arithmetic with a NaN input must produce a NaN output; which
 * operand's payload survives is unspecified in both forms (the scalar
 * loop's operand order is the compiler's pick, the intrinsic's is the
 * instruction's), so both-NaN lanes are tallied separately instead of
 * failing -- see lane_equal below. The conversions take a directed sweep
 * of the rounding corners (exact halves for cvtps2dq's round-to-nearest-
 * even, the +-2^31 boundary, int->float ties at 2^24), and XMM_SHUFFLE
 * -- which stayed scalar, MSVC refusing a runtime immediate for shufps
 * -- is swept across all 256 immediates so its lane table stays pinned
 * too.
 *
 * Exit codes: 0 every comparison held; 1 a mismatch (printed); 77 the
 * header took the scalar fallback, so there is nothing to compare. */
#define RECOMP_GENERATED_CODE   /* cvtps2dq/cvtpi2ps are generated-code-only */
#include "recomp_types.h"

#include <stdio.h>
#include <stdlib.h>

static uint64_t rng = 0x2545F4914F6CDD1Dull;
static uint64_t nxt(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng;
}

/* A random register: raw bits in both halves. */
static RecompXmm rndx(void) {
    RecompXmm r;
    r.u[0] = (uint32_t)nxt(); r.u[1] = (uint32_t)(nxt() >> 32);
    r.u[2] = (uint32_t)nxt(); r.u[3] = (uint32_t)(nxt() >> 32);
    return r;
}

/* A random register biased toward finite, moderate floats: the top bits
 * of an exponent are the inf/NaN range, so mask them down. */
static RecompXmm rndfinite(void) {
    RecompXmm r = rndx(); int i;
    for (i = 0; i < 4; i++) r.u[i] &= 0x3FFFFFFFu;   /* 0 <= |f| < 2^63 */
    return r;
}

/* Directed lane pool: NaNs (quiet and signalling, both signs), zeros,
 * denormals, the infinities, FLT_MAX, exact halves and the 2^31
 * conversion boundary. Handled as raw bits throughout -- a signalling
 * NaN must reach the intrinsics unquieted, and no float arithmetic
 * here may touch it first. */
static const uint32_t pool[] = {
    0x00000000u, 0x80000000u,   /* +-0 */
    0x3F800000u, 0xBF800000u,   /* +-1 */
    0x3F000000u, 0xBF000000u,   /* +-(0.5) */
    0x40000000u,                /* 2 */
    0x00000001u, 0x007FFFFFu, 0x807FFFFFu, /* denormals */
    0x7F7FFFFFu, 0xFF7FFFFFu,   /* +-FLT_MAX */
    0x7F800000u, 0xFF800000u,   /* +-inf */
    0x7FC00000u, 0xFFC00000u,   /* +-QNaN */
    0x7F800001u, 0xFFC00001u,   /* SNaNs with payload */
    0x7FFFFFFFu,                /* SNaN, all payload bits */
    0x4F000000u, 0xCF000000u,   /* +-2^31 */
    0xCF000001u,                /* first float below -2^31 */
    0x4EFFFFFFu,                /* last float inside int32 range */
};
#define POOL_N ((int)(sizeof pool / sizeof pool[0]))

static RecompXmm pool_lane(unsigned idx) {
    RecompXmm r;
    uint64_t q0 = ((uint64_t)pool[(idx * 7 + 0) % POOL_N] << 32)
                | pool[(idx * 5 + 3) % POOL_N];
    uint64_t q1 = ((uint64_t)pool[(idx * 3 + 1) % POOL_N] << 32)
                | pool[(idx * 11 + 2) % POOL_N];
    r.q[0] = q0; r.q[1] = q1;   /* bits only; no float reads */
    return r;
}

static int failures;
static long checks;
static long nanlanes;

/* SSE arithmetic on NaN inputs returns the first source operand,
 * quieted -- and which operand is "first" is the compiler's choice in
 * the scalar loop (MSVC emits b+a for a.f[i]+b.f[i]) and the dst/src
 * order in the intrinsic. The old scalar helpers never pinned a NaN
 * payload either, so a lane where both sides produced *a* NaN counts
 * as equal here and is tallied separately; every other bit must match.
 * Bitwise ops never need the dispensation: they copy operand bits
 * unchanged, NaN patterns included. */
static int lane_equal(uint32_t x, uint32_t y) {
    if (x == y) return 1;
    if (((x & 0x7F800000u) == 0x7F800000u && (x & 0x007FFFFFu) != 0)
     && ((y & 0x7F800000u) == 0x7F800000u && (y & 0x007FFFFFu) != 0)) {
        nanlanes++;
        return 1;
    }
    return 0;
}

static void comparex(const char *name, RecompXmm s, RecompXmm n) {
    checks++;
    if (!lane_equal(s.u[0], n.u[0]) || !lane_equal(s.u[1], n.u[1])
     || !lane_equal(s.u[2], n.u[2]) || !lane_equal(s.u[3], n.u[3])) {
        failures++;
        if (failures <= 10)
            printf("FAIL %-14s escalar=%08x %08x %08x %08x "
                   "intrinseco=%08x %08x %08x %08x\n", name,
                   s.u[0], s.u[1], s.u[2], s.u[3],
                   n.u[0], n.u[1], n.u[2], n.u[3]);
    }
}

#if defined(_M_X64) || defined(__x86_64__)

/* -- independent scalar references: the loops the header had --------- */

#define REF_F2(NAME, EXPR)                                               \
    static inline RecompXmm NAME(RecompXmm a, RecompXmm b) {             \
        RecompXmm r; int i;                                              \
        for (i = 0; i < 4; i++) r.f[i] = (EXPR);                         \
        return r;                                                        \
    }
#define REF_U2(NAME, EXPR)                                               \
    static inline RecompXmm NAME(RecompXmm a, RecompXmm b) {             \
        RecompXmm r; int i;                                              \
        for (i = 0; i < 4; i++) r.u[i] = (uint32_t)(EXPR);               \
        return r;                                                        \
    }

REF_F2(REF_ADD, a.f[i] + b.f[i])
REF_F2(REF_SUB, a.f[i] - b.f[i])
REF_F2(REF_MUL, a.f[i] * b.f[i])
REF_F2(REF_DIV, a.f[i] / b.f[i])
/* The hardware tie-break: src2 when unordered or equal. */
REF_F2(REF_MIN, (a.f[i] < b.f[i]) ? a.f[i] : b.f[i])
REF_F2(REF_MAX, (a.f[i] > b.f[i]) ? a.f[i] : b.f[i])

REF_U2(REF_AND,  a.u[i] & b.u[i])
REF_U2(REF_OR,   a.u[i] | b.u[i])
REF_U2(REF_XOR,  a.u[i] ^ b.u[i])
REF_U2(REF_ANDN, (~a.u[i]) & b.u[i])

REF_F2(REF_SQRT,  sqrtf(b.f[i]))
/* The packed forms are the per-lane .ss approximation, which is what
 * the scalar helpers always were. */
static inline float ref_rsqrtss(float x) {
    return _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(x)));
}
static inline float ref_rcpss(float x) {
    return _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x)));
}
REF_F2(REF_RSQRT, ref_rsqrtss(b.f[i]))
REF_F2(REF_RCP,   ref_rcpss(b.f[i]))

REF_U2(REF_CMP_EQ,  (a.f[i] == b.f[i]) ? 0xFFFFFFFFu : 0u)
REF_U2(REF_CMP_LT,  (a.f[i] <  b.f[i]) ? 0xFFFFFFFFu : 0u)
REF_U2(REF_CMP_LE,  (a.f[i] <= b.f[i]) ? 0xFFFFFFFFu : 0u)
REF_U2(REF_CMP_NEQ, (a.f[i] == b.f[i]) ? 0u : 0xFFFFFFFFu)

/* The predicate switch as it stood in the header. */
static inline int ref_cmp_pred(float a, float b, int p) {
    switch (p & 7) {
    case 0:  return a == b;
    case 1:  return a < b;
    case 2:  return a <= b;
    case 3:  return a != a || b != b;
    case 4:  return !(a == b);
    case 5:  return !(a < b);
    case 6:  return !(a <= b);
    default: return !(a != a || b != b);
    }
}
static inline RecompXmm REF_CMP_PRED(RecompXmm a, RecompXmm b, int p) {
    RecompXmm r; int i;
    for (i = 0; i < 4; i++)
        r.u[i] = ref_cmp_pred(a.f[i], b.f[i], p) ? 0xFFFFFFFFu : 0u;
    return r;
}

static inline RecompXmm REF_UNPACK_LOW(RecompXmm a, RecompXmm b) {
    RecompXmm r;
    r.u[0] = a.u[0]; r.u[1] = b.u[0]; r.u[2] = a.u[1]; r.u[3] = b.u[1];
    return r;
}
static inline RecompXmm REF_UNPACK_HIGH(RecompXmm a, RecompXmm b) {
    RecompXmm r;
    r.u[0] = a.u[2]; r.u[1] = b.u[2]; r.u[2] = a.u[3]; r.u[3] = b.u[3];
    return r;
}
static inline uint32_t REF_MOVEMASK(RecompXmm a) {
    return ((a.u[0] >> 31) & 1u) | (((a.u[1] >> 31) & 1u) << 1)
         | (((a.u[2] >> 31) & 1u) << 2) | (((a.u[3] >> 31) & 1u) << 3);
}
/* Stays scalar in the header; pinned here so the lane table cannot
 * drift silently. */
static inline RecompXmm REF_SHUFFLE(RecompXmm a, RecompXmm b, uint32_t imm) {
    RecompXmm r;
    r.u[0] = a.u[(imm >> 0) & 3u]; r.u[1] = a.u[(imm >> 2) & 3u];
    r.u[2] = b.u[(imm >> 4) & 3u]; r.u[3] = b.u[(imm >> 6) & 3u];
    return r;
}

/* cvtps2dq/cvttps2dq: the per-lane cvtss2si/cvttss2si the header loops
 * over, MXCSR rounding and integer indefinite included. */
static inline int32_t ref_f2i(float v, int truncate) {
    return truncate ? _mm_cvttss_si32(_mm_set_ss(v))
                    : _mm_cvtss_si32(_mm_set_ss(v));
}
static inline RecompXmm REF_CVT_PS2DQ(RecompXmm v, int truncate) {
    RecompXmm r; int i;
    for (i = 0; i < 4; i++) r.i[i] = ref_f2i(v.f[i], truncate);
    return r;
}
static inline RecompXmm REF_FROM_PI(RecompXmm dst, RecompMmx src) {
    RecompXmm r = dst;
    r.f[0] = (float)src.d[0];
    r.f[1] = (float)src.d[1];
    return r;
}

/* -- directed NaN semantics, stated independently of both copies -----
 * For a pair where either lane is NaN the SSE predicates read:
 * EQ/LT/LE/ORD false, UNORD/NEQ/NLT/NLE true. Anything else is a bug
 * in the reference itself, so check it before trusting the sweep. */
static int expected_nan_pred(int p) {
    switch (p & 7) {
    case 0: case 1: case 2: case 7: return 0;
    default: return 1;   /* 3 UNORD, 4 NEQ, 5 NLT, 6 NLE */
    }
}

static int check_nan_truth_table(void) {
    int bad = 0; unsigned x, y;
    for (x = 0; x < POOL_N; x++) {
        for (y = 0; y < POOL_N; y++) {
            float fa, fb;
            memcpy(&fa, &pool[x], 4); memcpy(&fb, &pool[y], 4);
            if (!(fa != fa || fb != fb)) continue;   /* not a NaN pair */
            {
                int p;
                for (p = 0; p < 8; p++) {
                    checks++;
                    if (ref_cmp_pred(fa, fb, p) != expected_nan_pred(p)) {
                        bad++;
                        if (bad <= 5)
                            printf("FAIL truth-table %08x %08x pred %d: "
                                   "referencia discorda do hardware\n",
                                   pool[x], pool[y], p);
                    }
                }
            }
        }
    }
    return bad;
}

#define CHECK2(REF, IN) do {                                              \
    long i;                                                               \
    for (i = 0; i < 50000; i++) {                                         \
        RecompXmm a = rndx(), b = rndx();                                 \
        comparex(#IN, REF(a, b), IN(a, b));                               \
    }                                                                     \
    for (i = 0; i < 50000; i++) {                                         \
        RecompXmm a = rndfinite(), b = rndfinite();                       \
        comparex(#IN, REF(a, b), IN(a, b));                               \
    }                                                                     \
    {   /* directed: pool lanes in a, pseudo-pool in b, and swapped */     \
        unsigned k;                                                        \
        for (k = 0; k < (unsigned)POOL_N * POOL_N; k++) {                  \
            RecompXmm a, b;                                                \
            memcpy(&a.u[0], &pool[k / POOL_N], 4);                         \
            memcpy(&a.u[1], &pool[k % POOL_N], 4);                         \
            a.u[2] = pool[(k * 7 + 5) % POOL_N];                           \
            a.u[3] = pool[(k * 5 + 11) % POOL_N];                          \
            b = pool_lane(k);                                              \
            comparex(#IN, REF(a, b), IN(a, b));                            \
            comparex(#IN, REF(b, a), IN(b, a));                            \
        }                                                                  \
    }                                                                      \
} while (0)

/* MINPS/MAXPS with a NaN anywhere in the pair must hand back the second
 * operand bit for bit -- src2 is the hardware's answer to unordered. */
static int check_min_max_nan_rule(void) {
    int bad = 0; unsigned x, y;
    for (x = 0; x < POOL_N; x++) {
        for (y = 0; y < POOL_N; y++) {
            float fa, fb;
            memcpy(&fa, &pool[x], 4); memcpy(&fb, &pool[y], 4);
            if (!(fa != fa || fb != fb)) continue;
            {
                RecompXmm a, b, nmin, nmax;
                int lane;
                memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
                for (lane = 0; lane < 4; lane++) {
                    memcpy(&a.f[lane], lane & 1 ? &pool[x] : &pool[y], 4);
                    memcpy(&b.f[lane], lane & 1 ? &pool[y] : &pool[x], 4);
                }
                nmin = XMM_MIN(a, b);
                nmax = XMM_MAX(a, b);
                for (lane = 0; lane < 4; lane++) {
                    checks += 2;
                    if (nmin.u[lane] != b.u[lane]
                     || nmax.u[lane] != b.u[lane]) {
                        bad++;
                        if (bad <= 5)
                            printf("FAIL min/max NaN %08x %08x lane %d: "
                                   "esperado src2, veio %08x/%08x\n",
                                   pool[x], pool[y], lane,
                                   nmin.u[lane], nmax.u[lane]);
                    }
                }
            }
        }
    }
    return bad;
}

int main(void) {
    int bad = check_nan_truth_table();
    CHECK2(REF_ADD, XMM_ADD);
    CHECK2(REF_SUB, XMM_SUB);
    CHECK2(REF_MUL, XMM_MUL);
    CHECK2(REF_DIV, XMM_DIV);
    CHECK2(REF_MIN, XMM_MIN);
    CHECK2(REF_MAX, XMM_MAX);
    CHECK2(REF_AND,  XMM_AND);
    CHECK2(REF_OR,   XMM_OR);
    CHECK2(REF_XOR,  XMM_XOR);
    CHECK2(REF_ANDN, XMM_ANDN);
    CHECK2(REF_SQRT,  XMM_SQRT);
    CHECK2(REF_RSQRT, XMM_RSQRT);
    CHECK2(REF_RCP,   XMM_RCP);
    CHECK2(REF_CMP_EQ,  XMM_CMP_EQ);
    CHECK2(REF_CMP_LT,  XMM_CMP_LT);
    CHECK2(REF_CMP_LE,  XMM_CMP_LE);
    CHECK2(REF_CMP_NEQ, XMM_CMP_NEQ);
    CHECK2(REF_UNPACK_LOW,  XMM_UNPACK_LOW);
    CHECK2(REF_UNPACK_HIGH, XMM_UNPACK_HIGH);
    failures += bad;
    bad = check_min_max_nan_rule();
    failures += bad;

    /* The full predicate switch: every code, random registers, plus the
     * 8..15 aliases that must fold to the same predicates. */
    {
        long i; int p;
        for (p = 0; p < 16; p++) {
            for (i = 0; i < 50000; i++) {
                RecompXmm a = rndfinite(), b = rndfinite();
                comparex("XMM_CMP_PRED", REF_CMP_PRED(a, b, p),
                         XMM_CMP_PRED(a, b, p));
            }
            for (i = 0; i < 20000; i++) {
                RecompXmm a = rndx(), b = rndx();
                comparex("XMM_CMP_PRED", REF_CMP_PRED(a, b, p),
                         XMM_CMP_PRED(a, b, p));
            }
        }
    }

    /* movmskps */
    {
        long i;
        for (i = 0; i < 50000; i++) {
            RecompXmm a = rndx();
            checks++;
            if (REF_MOVEMASK(a) != XMM_MOVEMASK(a)) {
                failures++;
                if (failures <= 10) printf("FAIL XMM_MOVEMASK\n");
            }
        }
    }

    /* shufps: every immediate, both operands random. */
    {
        unsigned imm;
        for (imm = 0; imm < 256; imm++) {
            long i;
            for (i = 0; i < 400; i++) {
                RecompXmm a = rndx(), b = rndx();
                comparex("XMM_SHUFFLE", REF_SHUFFLE(a, b, imm),
                         XMM_SHUFFLE(a, b, imm));
            }
        }
    }

    /* cvtps2dq / cvttps2dq: randoms plus the rounding corners. */
    {
        static const uint32_t corners[] = {
            0x3F000000u, 0x3FC00000u, 0x40200000u,   /* 0.5, 1.5, 2.5 */
            0xBF000000u, 0xBFC00000u, 0xC0200000u,   /* -0.5, -1.5, -2.5 */
            0x4EFFFFFFu, 0x4F000000u,                 /* int32 edge, 2^31 */
            0xCF000000u, 0xCF000001u,                 /* -2^31, below it */
            0x7F800000u, 0xFF800000u,                 /* +-inf */
            0x7FC00000u, 0x7F800001u,                 /* QNaN, SNaN */
            0x00000001u, 0x807FFFFFu,                 /* denormals */
            0x00000000u, 0x80000000u,                 /* +-0 */
            0x4B7FFFFFu, 0x4B800000u,                 /* 2^23-1, 2^24 */
        };
        int truncate;
        for (truncate = 0; truncate < 2; truncate++) {
            long i;
            for (i = 0; i < 50000; i++) {
                RecompXmm v = rndfinite();
                comparex("XMM_CVT_PS2DQ", REF_CVT_PS2DQ(v, truncate),
                         XMM_CVT_PS2DQ(v, truncate));
            }
            for (i = 0; i < 20000; i++) {
                RecompXmm v = rndx();
                comparex("XMM_CVT_PS2DQ", REF_CVT_PS2DQ(v, truncate),
                         XMM_CVT_PS2DQ(v, truncate));
            }
            {
                unsigned k;
                for (k = 0; k < sizeof corners / sizeof corners[0]; k++) {
                    RecompXmm v; unsigned j;
                    for (j = 0; j < 4; j++)
                        memcpy(&v.u[j], &corners[(k + j) %
                            (sizeof corners / sizeof corners[0])], 4);
                    comparex("XMM_CVT_PS2DQ", REF_CVT_PS2DQ(v, truncate),
                             XMM_CVT_PS2DQ(v, truncate));
                }
            }
        }
    }

    /* cvtpi2ps: lanes 2-3 of the destination must survive, and the int
     * -> float ties at 2^24 must round the same both ways. */
    {
        static const int32_t ints[] = {
            0, 1, -1, 2147483647, (-2147483647 - 1),
            16777216, 16777217, 16777219, -16777217, -16777219,
            1073741825, 123456789, -123456789, 0x55555555,
        };
        long i;
        for (i = 0; i < 50000; i++) {
            RecompXmm dst = rndx(); RecompMmx src;
            src.q = nxt();
            comparex("XMM_FROM_PI", REF_FROM_PI(dst, src),
                     XMM_FROM_PI(dst, src));
        }
        for (i = 0; i < (long)(sizeof ints / sizeof ints[0]); i++) {
            RecompXmm dst = pool_lane((unsigned)i);
            RecompMmx src;
            int j;
            src.d[0] = ints[i];
            src.d[1] = ints[(i + 1) % (int)(sizeof ints / sizeof ints[0])];
            for (j = 0; j < 2; j++) {
                comparex("XMM_FROM_PI", REF_FROM_PI(dst, src),
                         XMM_FROM_PI(dst, src));
                src.q = ~src.q;   /* same dwords, other sign combination */
            }
        }
    }

    printf("%s: %ld comparacoes (%ld lanes), %d divergencias, "
           "%ld lanes NaN-para-NaN (payload nao especificado)\n",
           failures ? "FALHOU" : "ok", checks, checks * 4, failures,
           nanlanes);
    return failures ? 1 : 0;
}

#else /* x86-64 */
int main(void) {
    /* The header took the scalar fallback: there is no intrinsic path to
     * compare against, and the scalar reference would be compared with
     * itself. */
    printf("skip: sem intrinsecos x86-64 neste host\n");
    return 77;
}
#endif /* x86-64 */
