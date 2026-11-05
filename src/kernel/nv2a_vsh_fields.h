/**
 * NV2A vertex-program microcode: the field table, in one place.
 *
 * Both readers of vertex-program microcode decode through this header: the
 * CPU interpreter (nv2a_vsh_cpu.c) and the parser the HLSL and MSL
 * translators share (src/d3d/d3d8_vsh_parse.c). Two copies of the table could
 * disagree, and when one did (an earlier parser read a made-up layout) every
 * program decoded as v0 reads with nothing failing to say so.
 *
 * The table is xemu's (hw/xbox/nv2a/pgraph/vsh.c), as (word, shift, bits).
 * Word 0 carries nothing a program needs.
 *
 *   ILU 1,25,3   MAC 1,21,4   CONST 1,13,8   V 1,9,4
 *   A: NEG 1,8,1  SWZ 1,0,8   R 2,28,4  MUX 2,26,2
 *   B: NEG 2,25,1 SWZ 2,17,8  R 2,13,4  MUX 2,11,2
 *   C: NEG 2,10,1 SWZ 2,2,8   R_HIGH 2,0,2  R_LOW 3,30,2  MUX 3,28,2
 *   OUT: MAC_MASK 3,24,4  R 3,20,4  ILU_MASK 3,16,4  O_MASK 3,12,4
 *        ORB 3,11,1  ADDRESS 3,3,8  MUX 3,2,1
 *   A0X 3,1,1   FINAL 3,0,1
 *
 * A swizzle byte holds x in its top two bits, then y, z, w. A source MUX of
 * 1 reads a temp R, 2 an input v, 3 a constant c (0 reads zero). Masks are
 * xyzw = bits 3..0. The output MUX says which unit writes the output
 * register (0 MAC, 1 ILU); ORB says whether that is an output register (1)
 * or a constant (0). Portable C, no graphics API.
 */
#ifndef XBOXRECOMP_NV2A_VSH_FIELDS_H
#define XBOXRECOMP_NV2A_VSH_FIELDS_H

#include <stdint.h>

#define NV2A_VSH_FIELD(w, word, shift, bits) \
    (((uint32_t)(w)[word] >> (shift)) & ((1u << (bits)) - 1u))

#define NV2A_VSH_ILU(w)       NV2A_VSH_FIELD(w, 1, 25, 3)
#define NV2A_VSH_MAC(w)       NV2A_VSH_FIELD(w, 1, 21, 4)
#define NV2A_VSH_CONST(w)     NV2A_VSH_FIELD(w, 1, 13, 8)
#define NV2A_VSH_INPUT(w)     NV2A_VSH_FIELD(w, 1,  9, 4)
#define NV2A_VSH_MAC_MASK(w)  NV2A_VSH_FIELD(w, 3, 24, 4)
#define NV2A_VSH_OUT_R(w)     NV2A_VSH_FIELD(w, 3, 20, 4)
#define NV2A_VSH_ILU_MASK(w)  NV2A_VSH_FIELD(w, 3, 16, 4)
#define NV2A_VSH_O_MASK(w)    NV2A_VSH_FIELD(w, 3, 12, 4)
#define NV2A_VSH_ORB(w)       NV2A_VSH_FIELD(w, 3, 11, 1)
#define NV2A_VSH_O_ADDR(w)    NV2A_VSH_FIELD(w, 3,  3, 8)
#define NV2A_VSH_O_MUX(w)     NV2A_VSH_FIELD(w, 3,  2, 1)
#define NV2A_VSH_A0X(w)       NV2A_VSH_FIELD(w, 3,  1, 1)
#define NV2A_VSH_FINAL(w)     NV2A_VSH_FIELD(w, 3,  0, 1)

enum { NV2A_VSH_MUX_NONE = 0, NV2A_VSH_MUX_R = 1, NV2A_VSH_MUX_V = 2,
       NV2A_VSH_MUX_C = 3 };

/* Source A (0), B (1) or C (2) of an instruction, raw. */
struct nv2a_vsh_src {
    uint32_t neg, swz, reg, mux;   /* swz: the 8-bit swizzle byte */
};

static inline void nv2a_vsh_src_decode(const uint32_t *w, int which,
                                       struct nv2a_vsh_src *s)
{
    if (which == 0) {
        s->neg = NV2A_VSH_FIELD(w, 1, 8, 1);  s->swz = NV2A_VSH_FIELD(w, 1, 0, 8);
        s->reg = NV2A_VSH_FIELD(w, 2, 28, 4); s->mux = NV2A_VSH_FIELD(w, 2, 26, 2);
    } else if (which == 1) {
        s->neg = NV2A_VSH_FIELD(w, 2, 25, 1); s->swz = NV2A_VSH_FIELD(w, 2, 17, 8);
        s->reg = NV2A_VSH_FIELD(w, 2, 13, 4); s->mux = NV2A_VSH_FIELD(w, 2, 11, 2);
    } else {
        s->neg = NV2A_VSH_FIELD(w, 2, 10, 1); s->swz = NV2A_VSH_FIELD(w, 2, 2, 8);
        s->reg = (NV2A_VSH_FIELD(w, 2, 0, 2) << 2) | NV2A_VSH_FIELD(w, 3, 30, 2);
        s->mux = NV2A_VSH_FIELD(w, 3, 28, 2);
    }
}

/* Component k (0 x .. 3 w) of a swizzle byte: which source component. */
#define NV2A_VSH_SWZ(swz, k) (((swz) >> (6 - 2 * (k))) & 3u)

#endif
