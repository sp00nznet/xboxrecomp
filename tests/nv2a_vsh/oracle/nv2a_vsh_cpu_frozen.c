/* Frozen oracle for nv2a_vsh_fuzz: src/kernel/nv2a_vsh_cpu.c as it was
 * before the vec4 hot loop and nv2a_vsh_fields.h. Its own field decoder,
 * scalar per component: the fuzz requires the current interpreter to match
 * it bit for bit. Not built into any library. */
#define nv2a_vsh_run    oracle_vsh_run
#define nv2a_vsh_disasm oracle_vsh_disasm
/**
 * NV2A vertex program interpreter -- see nv2a_vsh_cpu.h.
 *
 * Field layout, from xemu's vsh.c (word, shift, bits). Word 0 carries nothing
 * the interpreter needs.
 *
 *   ILU 1,25,3   MAC 1,21,4   CONST 1,13,8   V 1,9,4
 *   A: NEG 1,8,1  SWZ 1,6/4/2/0,2  R 2,28,4  MUX 2,26,2
 *   B: NEG 2,25,1 SWZ 2,23/21/19/17,2  R 2,13,4  MUX 2,11,2
 *   C: NEG 2,10,1 SWZ 2,8/6/4/2,2  R_HIGH 2,0,2  R_LOW 3,30,2  MUX 3,28,2
 *   OUT: MAC_MASK 3,24,4  R 3,20,4  ILU_MASK 3,16,4  O_MASK 3,12,4
 *        ORB 3,11,1  ADDRESS 3,3,8  MUX 3,2,1
 *   A0X 3,1,1   FINAL 3,0,1
 *
 * Semantics worth stating, because each one is a way to be silently wrong:
 *
 *  - MAC and ILU issue together and read the registers as they were before
 *    the instruction; both results are computed before either is written.
 *  - When both units are active, the ILU's temp write goes to R1, not to the
 *    instruction's R field (the XDK epilogue's `+rcc R1.x, R12.w` is that).
 *  - R12 *is* oPos. The XDK epilogue only produces screen coordinates if a
 *    write to oPos is visible to the next instruction's read of R12.
 *  - Masks are xyzw = bits 3..0.
 */
#include "nv2a_vsh_cpu.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t fld(const uint32_t *w, int word, int shift, int bits)
{
    return (w[word] >> shift) & ((1u << bits) - 1u);
}

enum { MUX_NONE = 0, MUX_R = 1, MUX_V = 2, MUX_C = 3 };

enum {
    MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
    MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL
};
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };

typedef struct {
    int neg, swz[4], r, mux;
} Src;

static void decode_src(const uint32_t *w, int which, Src *s)
{
    switch (which) {
    case 0:
        s->neg = fld(w, 1, 8, 1);
        s->swz[0] = fld(w, 1, 6, 2); s->swz[1] = fld(w, 1, 4, 2);
        s->swz[2] = fld(w, 1, 2, 2); s->swz[3] = fld(w, 1, 0, 2);
        s->r = fld(w, 2, 28, 4);
        s->mux = fld(w, 2, 26, 2);
        break;
    case 1:
        s->neg = fld(w, 2, 25, 1);
        s->swz[0] = fld(w, 2, 23, 2); s->swz[1] = fld(w, 2, 21, 2);
        s->swz[2] = fld(w, 2, 19, 2); s->swz[3] = fld(w, 2, 17, 2);
        s->r = fld(w, 2, 13, 4);
        s->mux = fld(w, 2, 11, 2);
        break;
    default:
        s->neg = fld(w, 2, 10, 1);
        s->swz[0] = fld(w, 2, 8, 2); s->swz[1] = fld(w, 2, 6, 2);
        s->swz[2] = fld(w, 2, 4, 2); s->swz[3] = fld(w, 2, 2, 2);
        s->r = (int)((fld(w, 2, 0, 2) << 2) | fld(w, 3, 30, 2));
        s->mux = fld(w, 3, 28, 2);
        break;
    }
}

typedef struct {
    float r[13][4];            /* R0..R11, R12 = oPos (o[0] below) */
    float (*o)[4];
    int   a0;
} State;

static const float *temp_ptr(State *st, int r)
{
    static const float zero[4] = { 0, 0, 0, 0 };
    if (r == 12)
        return st->o[NV2A_VSH_O_POS];
    if (r >= 0 && r < 12)
        return st->r[r];
    return zero;
}

static void read_src(State *st, const uint32_t *w, int which,
                     const float c[NV2A_VSH_CONSTANTS][4],
                     const float v[NV2A_VSH_INPUTS][4], float out[4])
{
    static const float zero[4] = { 0, 0, 0, 0 };
    const float *p = zero;
    Src s;
    int i;

    decode_src(w, which, &s);
    switch (s.mux) {
    case MUX_R:
        p = temp_ptr(st, s.r);
        break;
    case MUX_V:
        p = v[fld(w, 1, 9, 4)];
        break;
    case MUX_C: {
        int idx = (int)fld(w, 1, 13, 8);
        if (fld(w, 3, 1, 1))
            idx += st->a0;
        p = (idx >= 0 && idx < NV2A_VSH_CONSTANTS) ? c[idx] : zero;
        break;
    }
    default:
        break;
    }
    for (i = 0; i < 4; i++)
        out[i] = s.neg ? -p[s.swz[i]] : p[s.swz[i]];
}

static void splat(float d[4], float x) { d[0] = d[1] = d[2] = d[3] = x; }

static float rcc(float x)
{
    float r = 1.0f / x, m = fabsf(r);
    if (m < 5.42101e-20f) m = 5.42101e-20f;
    else if (!(m <= 1.84467e+19f)) m = 1.84467e+19f;   /* also catches inf/NaN */
    return signbit(x) ? -m : m;
}

static void write_masked(float *d, const float s[4], uint32_t mask)
{
    if (mask & 8) d[0] = s[0];
    if (mask & 4) d[1] = s[1];
    if (mask & 2) d[2] = s[2];
    if (mask & 1) d[3] = s[3];
}

void nv2a_vsh_run(const uint32_t prog[NV2A_VSH_SLOTS][4], uint32_t start,
                  float c[NV2A_VSH_CONSTANTS][4], int const_write,
                  const float v[NV2A_VSH_INPUTS][4], Nv2aVshOut *out)
{
    State st;
    uint32_t pc;
    int i;

    memset(&st, 0, sizeof st);
    st.o = out->o;
    for (i = 0; i < NV2A_VSH_O_COUNT; i++) {
        out->o[i][0] = out->o[i][1] = out->o[i][2] = 0.0f;
        out->o[i][3] = 1.0f;
    }
    out->steps = 0;
    out->ok = 0;

    for (pc = start; pc < NV2A_VSH_SLOTS; pc++) {
        const uint32_t *w = prog[pc];
        uint32_t mac = fld(w, 1, 21, 4), ilu = fld(w, 1, 25, 3);
        uint32_t out_r = fld(w, 3, 20, 4);
        uint32_t mac_mask = fld(w, 3, 24, 4), ilu_mask = fld(w, 3, 16, 4);
        uint32_t o_mask = fld(w, 3, 12, 4), orb = fld(w, 3, 11, 1);
        uint32_t addr = fld(w, 3, 3, 8), omux = fld(w, 3, 2, 1);
        float a[4], b[4], cc[4], mr[4], ir[4];
        int mac_wr = 0, ilu_wr = 0;

        out->steps++;
        read_src(&st, w, 0, (const float (*)[4])c, v, a);
        read_src(&st, w, 1, (const float (*)[4])c, v, b);
        read_src(&st, w, 2, (const float (*)[4])c, v, cc);

        switch (mac) {
        case MAC_NOP: break;
        case MAC_MOV: memcpy(mr, a, sizeof mr); mac_wr = 1; break;
        case MAC_MUL: for (i = 0; i < 4; i++) mr[i] = a[i] * b[i]; mac_wr = 1; break;
        case MAC_ADD: for (i = 0; i < 4; i++) mr[i] = a[i] + cc[i]; mac_wr = 1; break;
        case MAC_MAD: for (i = 0; i < 4; i++) mr[i] = a[i] * b[i] + cc[i]; mac_wr = 1; break;
        case MAC_DP3: splat(mr, a[0]*b[0] + a[1]*b[1] + a[2]*b[2]); mac_wr = 1; break;
        case MAC_DPH: splat(mr, a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + b[3]); mac_wr = 1; break;
        case MAC_DP4: splat(mr, a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3]); mac_wr = 1; break;
        case MAC_DST: mr[0] = 1.0f; mr[1] = a[1] * b[1]; mr[2] = a[2]; mr[3] = b[3]; mac_wr = 1; break;
        case MAC_MIN: for (i = 0; i < 4; i++) mr[i] = a[i] < b[i] ? a[i] : b[i]; mac_wr = 1; break;
        case MAC_MAX: for (i = 0; i < 4; i++) mr[i] = a[i] > b[i] ? a[i] : b[i]; mac_wr = 1; break;
        case MAC_SLT: for (i = 0; i < 4; i++) mr[i] = a[i] <  b[i] ? 1.0f : 0.0f; mac_wr = 1; break;
        case MAC_SGE: for (i = 0; i < 4; i++) mr[i] = a[i] >= b[i] ? 1.0f : 0.0f; mac_wr = 1; break;
        case MAC_ARL: break;                          /* applied after reads */
        default: break;
        }

        switch (ilu) {
        case ILU_NOP: break;
        case ILU_MOV: memcpy(ir, cc, sizeof ir); ilu_wr = 1; break;
        case ILU_RCP: splat(ir, 1.0f / cc[0]); ilu_wr = 1; break;
        case ILU_RCC: splat(ir, rcc(cc[0])); ilu_wr = 1; break;
        case ILU_RSQ: splat(ir, 1.0f / sqrtf(fabsf(cc[0]))); ilu_wr = 1; break;
        case ILU_EXP: {
            float f = floorf(cc[0]);
            ir[0] = exp2f(f); ir[1] = cc[0] - f; ir[2] = exp2f(cc[0]); ir[3] = 1.0f;
            ilu_wr = 1;
            break;
        }
        case ILU_LOG: {
            float t = fabsf(cc[0]);
            if (t == 0.0f) {
                ir[0] = -INFINITY; ir[1] = 1.0f; ir[2] = -INFINITY;
            } else {
                float e = floorf(log2f(t));
                ir[0] = e; ir[1] = t / exp2f(e); ir[2] = log2f(t);
            }
            ir[3] = 1.0f;
            ilu_wr = 1;
            break;
        }
        case ILU_LIT: {
            float p = cc[3];
            if (p < -127.9961f) p = -127.9961f;
            if (p >  127.9961f) p =  127.9961f;
            ir[0] = 1.0f;
            ir[1] = cc[0] > 0.0f ? cc[0] : 0.0f;
            ir[2] = cc[0] > 0.0f ? powf(cc[1] > 0.0f ? cc[1] : 0.0f, p) : 0.0f;
            ir[3] = 1.0f;
            ilu_wr = 1;
            break;
        }
        default: break;
        }

        /* Writes, after every read. */
        if (mac == MAC_ARL)
            st.a0 = (int)floorf(a[0] + 0.001f);
        if (mac_wr) {
            if (mac_mask && out_r <= 12)
                write_masked(out_r == 12 ? st.o[NV2A_VSH_O_POS] : st.r[out_r],
                             mr, mac_mask);
            if (omux == 0 && o_mask) {
                if (orb) {
                    if (addr < NV2A_VSH_O_COUNT)
                        write_masked(st.o[addr], mr, o_mask);
                } else if (const_write && addr < NV2A_VSH_CONSTANTS) {
                    write_masked(c[addr], mr, o_mask);
                }
            }
        }
        if (ilu_wr) {
            uint32_t r = mac != MAC_NOP ? 1 : out_r;
            if (ilu_mask && r <= 12)
                write_masked(r == 12 ? st.o[NV2A_VSH_O_POS] : st.r[r],
                             ir, ilu_mask);
            if (omux == 1 && o_mask) {
                if (orb) {
                    if (addr < NV2A_VSH_O_COUNT)
                        write_masked(st.o[addr], ir, o_mask);
                } else if (const_write && addr < NV2A_VSH_CONSTANTS) {
                    write_masked(c[addr], ir, o_mask);
                }
            }
        }

        if (fld(w, 3, 0, 1)) {
            out->ok = 1;
            return;
        }
    }
}

/* ---- disassembly ------------------------------------------------------ */

static const char *const MAC_NAMES[16] = {
    "nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4",
    "dst", "min", "max", "slt", "sge", "arl", "mac?", "mac?"
};
static const char *const ILU_NAMES[8] = {
    "nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit"
};
static const char *const OUT_NAMES[16] = {
    "oPos", "o1?", "o2?", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1",
    "oT0", "oT1", "oT2", "oT3", "o13?", "o14?", "o15?"
};

static int put_mask(char *b, int n, int size, uint32_t m)
{
    if (m == 0xF)
        return n;
    return n + snprintf(b + n, size > n ? (size_t)(size - n) : 0, ".%s%s%s%s",
                        (m & 8) ? "x" : "", (m & 4) ? "y" : "",
                        (m & 2) ? "z" : "", (m & 1) ? "w" : "");
}

static int put_src(char *b, int n, int size, const uint32_t *w, int which)
{
    static const char xyzw[] = "xyzw";
    Src s;
    char swz[6] = { 0 };
    int k;

    decode_src(w, which, &s);
    if (s.swz[0] == 0 && s.swz[1] == 1 && s.swz[2] == 2 && s.swz[3] == 3)
        swz[0] = 0;
    else if (s.swz[0] == s.swz[1] && s.swz[1] == s.swz[2] && s.swz[2] == s.swz[3]) {
        swz[0] = '.'; swz[1] = xyzw[s.swz[0]];
    } else {
        swz[0] = '.';
        for (k = 0; k < 4; k++) swz[1 + k] = xyzw[s.swz[k]];
    }
#define ROOM (size > n ? (size_t)(size - n) : 0)
    switch (s.mux) {
    case MUX_R: n += snprintf(b + n, ROOM, ", %sR%d%s", s.neg ? "-" : "", s.r, swz); break;
    case MUX_V: n += snprintf(b + n, ROOM, ", %sv%u%s", s.neg ? "-" : "", fld(w, 1, 9, 4), swz); break;
    case MUX_C:
        n += snprintf(b + n, ROOM, ", %sc[%s%u]%s", s.neg ? "-" : "",
                      fld(w, 3, 1, 1) ? "a0+" : "", fld(w, 1, 13, 8), swz);
        break;
    default: n += snprintf(b + n, ROOM, ", ?"); break;
    }
    return n;
}

char *nv2a_vsh_disasm(const uint32_t w[4], char *b, int size)
{
    uint32_t mac = fld(w, 1, 21, 4), ilu = fld(w, 1, 25, 3);
    uint32_t out_r = fld(w, 3, 20, 4);
    uint32_t mac_mask = fld(w, 3, 24, 4), ilu_mask = fld(w, 3, 16, 4);
    uint32_t o_mask = fld(w, 3, 12, 4), orb = fld(w, 3, 11, 1);
    uint32_t addr = fld(w, 3, 3, 8), omux = fld(w, 3, 2, 1);
    int n = 0;

    b[0] = 0;
    if (mac) {
        n += snprintf(b + n, ROOM, "%s ", MAC_NAMES[mac]);
        if (mac_mask) {
            n += snprintf(b + n, ROOM, "R%u", out_r);
            n = put_mask(b, n, size, mac_mask);
        }
        if (omux == 0 && o_mask) {
            n += snprintf(b + n, ROOM, "%s", mac_mask ? "+" : "");
            if (orb) n += snprintf(b + n, ROOM, "%s", OUT_NAMES[addr & 15]);
            else     n += snprintf(b + n, ROOM, "c[%u]", addr);
            n = put_mask(b, n, size, o_mask);
        }
        if (mac == MAC_ARL) n += snprintf(b + n, ROOM, "a0");
        if (mac != MAC_NOP) n = put_src(b, n, size, w, 0);
        if (mac == MAC_MUL || mac == MAC_MAD || (mac >= MAC_DP3 && mac <= MAC_SGE))
            n = put_src(b, n, size, w, 1);
        if (mac == MAC_ADD || mac == MAC_MAD)
            n = put_src(b, n, size, w, 2);
    }
    if (ilu) {
        n += snprintf(b + n, ROOM, "%s%s ", mac ? " + " : "", ILU_NAMES[ilu]);
        if (ilu_mask) {
            n += snprintf(b + n, ROOM, "R%u", mac ? 1u : out_r);
            n = put_mask(b, n, size, ilu_mask);
        }
        if (omux == 1 && o_mask) {
            n += snprintf(b + n, ROOM, "%s", ilu_mask ? "+" : "");
            if (orb) n += snprintf(b + n, ROOM, "%s", OUT_NAMES[addr & 15]);
            else     n += snprintf(b + n, ROOM, "c[%u]", addr);
            n = put_mask(b, n, size, o_mask);
        }
        n = put_src(b, n, size, w, 2);
    }
    if (!mac && !ilu)
        n += snprintf(b + n, ROOM, "nop");
    if (fld(w, 3, 0, 1))
        n += snprintf(b + n, ROOM, "  ; final");
#undef ROOM
    return b;
}
