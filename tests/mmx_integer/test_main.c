/* Every MMX integer helper the recompiler emits, against the instruction it
 * stands for, on the same inputs.
 *
 * A codec is a long chain of these. One wrong lane in one helper does not
 * crash and does not warn -- it comes out as a picture that is subtly wrong,
 * days later, with nothing to point at. Reading the helpers proves nothing
 * either: they all look right. The processor is the only authority on what
 * they should do, and it is sitting right here.
 *
 * x86 hosts only, for the obvious reason. Elsewhere it reports that it did
 * not run rather than passing vacuously.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define RECOMP_GENERATED_CODE 1
#include "../../templates/runtime/recomp_types.h"

/* MSVC has no __m64 intrinsics on x64, so _M_X64 is left out. */
#if defined(__x86_64__) || defined(__i386__) || defined(_M_IX86)

#include <mmintrin.h>
#include <xmmintrin.h>

static unsigned checks, failures;

static uint64_t to_u64(__m64 v)
{
    uint64_t r;
    memcpy(&r, &v, 8);
    return r;
}

static __m64 to_m64(uint64_t v)
{
    __m64 r;
    memcpy(&r, &v, 8);
    return r;
}

static void check(const char *name, uint64_t got, uint64_t want,
                  uint64_t a, uint64_t b)
{
    ++checks;
    if (got == want)
        return;
    if (++failures <= 20)
        printf("  %-12s a=%016llx b=%016llx -> %016llx, hardware says %016llx\n",
               name, (unsigned long long)a, (unsigned long long)b,
               (unsigned long long)got, (unsigned long long)want);
}

/* A 64-bit pattern with runs and edges in it, not just uniform noise: the
 * saturating and packing helpers only differ from the wrong answer at the
 * limits, which uniform random bits reach rarely. */
static uint64_t pattern(unsigned i)
{
    static const uint64_t edges[] = {
        0ull, ~0ull, 0x8000800080008000ull, 0x7FFF7FFF7FFF7FFFull,
        0x0080008000800080ull, 0xFF00FF00FF00FF00ull, 0x0001000100010001ull,
        0x8000000080000000ull
    };
    if (i < sizeof edges / sizeof edges[0])
        return edges[i];
    {
        uint64_t v = 0;
        unsigned k;
        for (k = 0; k < 4; k++)
            v = (v << 16) ^ (uint64_t)(rand() & 0xFFFF);
        return v;
    }
}

#define BINARY(HELPER, INTRINSIC)                                         \
    do {                                                                  \
        RecompMmx ra, rb, rr;                                             \
        ra.q = a; rb.q = b;                                               \
        rr = HELPER(ra, rb);                                              \
        check(#HELPER, rr.q, to_u64(INTRINSIC(to_m64(a), to_m64(b))),     \
              a, b);                                                      \
    } while (0)

#define SHIFT(HELPER, INTRINSIC, COUNT)                                   \
    do {                                                                  \
        RecompMmx ra, rr;                                                 \
        ra.q = a;                                                         \
        rr = HELPER(ra, (uint64_t)(COUNT));                               \
        check(#HELPER, rr.q,                                              \
              to_u64(INTRINSIC(to_m64(a), to_m64((uint64_t)(COUNT)))),    \
              a, (uint64_t)(COUNT));                                      \
    } while (0)

int main(void)
{
    unsigned i, j, c;

    srand(20250920u);
    for (i = 0; i < 4096; i++) {
        for (j = 0; j < 8; j++) {
            uint64_t a = pattern(i), b = pattern(i + j + 1);

            BINARY(MMX_PADDB,    _mm_add_pi8);
            BINARY(MMX_PADDW,    _mm_add_pi16);
            BINARY(MMX_PADDD,    _mm_add_pi32);
            BINARY(MMX_PSUBB,    _mm_sub_pi8);
            BINARY(MMX_PSUBW,    _mm_sub_pi16);
            BINARY(MMX_PSUBD,    _mm_sub_pi32);
            BINARY(MMX_PADDSB,   _mm_adds_pi8);
            BINARY(MMX_PADDSW,   _mm_adds_pi16);
            BINARY(MMX_PSUBSB,   _mm_subs_pi8);
            BINARY(MMX_PSUBSW,   _mm_subs_pi16);
            BINARY(MMX_PADDUSB,  _mm_adds_pu8);
            BINARY(MMX_PADDUSW,  _mm_adds_pu16);
            BINARY(MMX_PSUBUSB,  _mm_subs_pu8);
            BINARY(MMX_PSUBUSW,  _mm_subs_pu16);
            BINARY(MMX_PMULLW,   _mm_mullo_pi16);
            BINARY(MMX_PMULHW,   _mm_mulhi_pi16);
            BINARY(MMX_PMADDWD,  _mm_madd_pi16);
            BINARY(MMX_PAVGB,    _mm_avg_pu8);
            BINARY(MMX_PAVGW,    _mm_avg_pu16);
            BINARY(MMX_PMINSW,   _mm_min_pi16);
            BINARY(MMX_PMAXSW,   _mm_max_pi16);
            BINARY(MMX_PCMPEQB,  _mm_cmpeq_pi8);
            BINARY(MMX_PCMPEQW,  _mm_cmpeq_pi16);
            BINARY(MMX_PCMPEQD,  _mm_cmpeq_pi32);
            BINARY(MMX_PCMPGTB,  _mm_cmpgt_pi8);
            BINARY(MMX_PCMPGTW,  _mm_cmpgt_pi16);
            BINARY(MMX_PCMPGTD,  _mm_cmpgt_pi32);
            BINARY(MMX_PUNPCKLBW, _mm_unpacklo_pi8);
            BINARY(MMX_PUNPCKHBW, _mm_unpackhi_pi8);
            BINARY(MMX_PUNPCKLWD, _mm_unpacklo_pi16);
            BINARY(MMX_PUNPCKHWD, _mm_unpackhi_pi16);
            BINARY(MMX_PUNPCKLDQ, _mm_unpacklo_pi32);
            BINARY(MMX_PUNPCKHDQ, _mm_unpackhi_pi32);
            BINARY(MMX_PACKSSWB, _mm_packs_pi16);
            BINARY(MMX_PACKUSWB, _mm_packs_pu16);
            BINARY(MMX_PACKSSDW, _mm_packs_pi32);
            BINARY(MMX_PSADBW,   _mm_sad_pu8);
            BINARY(MMX_PAND,     _mm_and_si64);
            BINARY(MMX_PANDN,    _mm_andnot_si64);
            BINARY(MMX_POR,      _mm_or_si64);
            BINARY(MMX_PXOR,     _mm_xor_si64);

            /* Past the lane width the answer is not "shift by the low bits",
             * which is the mistake the C operator invites. */
            for (c = 0; c <= 70; c += 7) {
                SHIFT(MMX_PSLLW, _mm_sll_pi16, c);
                SHIFT(MMX_PSRLW, _mm_srl_pi16, c);
                SHIFT(MMX_PSRAW, _mm_sra_pi16, c);
                SHIFT(MMX_PSLLD, _mm_sll_pi32, c);
                SHIFT(MMX_PSRLD, _mm_srl_pi32, c);
                SHIFT(MMX_PSRAD, _mm_sra_pi32, c);
                SHIFT(MMX_PSLLQ, _mm_sll_si64, c);
                SHIFT(MMX_PSRLQ, _mm_srl_si64, c);
            }

            /* The shuffle and the lane selectors are encoded in the
             * instruction, so the intrinsics take a literal and the cases
             * have to be written out rather than looped over. */
            {
                RecompMmx ra;
                ra.q = a;
#define PSHUFW_CASE(IMM)                                                  \
                check("MMX_PSHUFW", MMX_PSHUFW(ra, (IMM)).q,              \
                      to_u64(_mm_shuffle_pi16(to_m64(a), (IMM))), a, (IMM))
                PSHUFW_CASE(0x00); PSHUFW_CASE(0x1B); PSHUFW_CASE(0x4E);
                PSHUFW_CASE(0xB1); PSHUFW_CASE(0xE4); PSHUFW_CASE(0x27);
                PSHUFW_CASE(0x93); PSHUFW_CASE(0xFF);
#undef PSHUFW_CASE
/* pextrw zero-extends, but GCC 10's _mm_extract_pi16 returns the lane
 * sign-extended, so the reference is cut back to 16 bits first. */
#define LANE_CASE(IMM)                                                    \
                check("MMX_PEXTRW", MMX_PEXTRW(ra, (IMM)),                \
                      (uint32_t)(uint16_t)_mm_extract_pi16(to_m64(a), (IMM)), \
                      a, (IMM));                                          \
                check("MMX_PINSRW",                                       \
                      MMX_PINSRW(ra, (uint32_t)(b & 0xFFFF), (IMM)).q,    \
                      to_u64(_mm_insert_pi16(to_m64(a),                   \
                                             (int)(b & 0xFFFF), (IMM))),  \
                      a, (IMM))
                LANE_CASE(0); LANE_CASE(1); LANE_CASE(2); LANE_CASE(3);
#undef LANE_CASE
                check("MMX_PMOVMSKB", MMX_PMOVMSKB(ra),
                      (uint32_t)_mm_movemask_pi8(to_m64(a)), a, 0);
            }
        }
    }
    _mm_empty();

    printf("%u checks, %u failures\n", checks, failures);
    return failures != 0;
}

#else

int main(void)
{
    printf("no MMX intrinsics on this host: nothing to compare against, skipped\n");
    return 0;
}

#endif
