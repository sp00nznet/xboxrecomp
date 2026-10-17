/**
 * NV2A vertex program interpreter -- see nv2a_vsh_cpu.h.
 *
 * Fields come from nv2a_vsh_fields.h, the one copy of xemu's vsh.c table
 * that the shader translators' parser (src/d3d/d3d8_vsh_parse.c) reads too.
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
 *
 * The loop is shaped after upstream's nv2a_vsh_interp.c (sp00nznet/xboxrecomp
 * 5dc1e34): each source is read as a vec4 value, swizzled straight from its
 * swizzle byte, and the MAC and ILU units are small functions returning
 * their result. It runs once per vertex for every 3D batch the CPU path
 * draws, so its shape is its speed; the arithmetic is unchanged.
 */
#include "nv2a_vsh_cpu.h"
#include "nv2a_vsh_fields.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
    MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
    MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL
};
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };

typedef struct { float v[4]; } vec4;

static const float k_zero[4] = { 0, 0, 0, 0 };

/* Source `which` of instruction w. r[12] is oPos. */
static inline vec4 read_src(const uint32_t *w, int which,
                            const float (*r)[4],
                            const float c[NV2A_VSH_CONSTANTS][4],
                            const float v[NV2A_VSH_INPUTS][4], int a0)
{
    struct nv2a_vsh_src s;
    const float *p = k_zero;
    vec4 o;

    nv2a_vsh_src_decode(w, which, &s);
    if (s.mux == NV2A_VSH_MUX_R) {
        if (s.reg <= 12)
            p = r[s.reg];
    } else if (s.mux == NV2A_VSH_MUX_V) {
        p = v[NV2A_VSH_INPUT(w)];
    } else if (s.mux == NV2A_VSH_MUX_C) {
        int idx = (int)NV2A_VSH_CONST(w);
        if (NV2A_VSH_A0X(w))
            idx += a0;
        if (idx >= 0 && idx < NV2A_VSH_CONSTANTS)
            p = c[idx];
    }
    o.v[0] = p[NV2A_VSH_SWZ(s.swz, 0)];
    o.v[1] = p[NV2A_VSH_SWZ(s.swz, 1)];
    o.v[2] = p[NV2A_VSH_SWZ(s.swz, 2)];
    o.v[3] = p[NV2A_VSH_SWZ(s.swz, 3)];
    if (s.neg) {
        o.v[0] = -o.v[0]; o.v[1] = -o.v[1];
        o.v[2] = -o.v[2]; o.v[3] = -o.v[3];
    }
    return o;
}

static inline vec4 splat(float x)
{
    vec4 r;
    r.v[0] = r.v[1] = r.v[2] = r.v[3] = x;
    return r;
}

static float rcc(float x)
{
    float r = 1.0f / x, m = fabsf(r);
    if (m < 5.42101e-20f) m = 5.42101e-20f;
    else if (!(m <= 1.84467e+19f)) m = 1.84467e+19f;   /* also catches inf/NaN */
    return signbit(x) ? -m : m;
}

/* The MAC unit's result for op (ARL, NOP: none, *wr stays 0). */
static inline vec4 run_mac(uint32_t op, const vec4 *a, const vec4 *b,
                           const vec4 *c, int *wr)
{
    vec4 r = *a;
    int i;

    *wr = 1;
    switch (op) {
    case MAC_MOV: break;
    case MAC_MUL: for (i = 0; i < 4; i++) r.v[i] = a->v[i] * b->v[i]; break;
    case MAC_ADD: for (i = 0; i < 4; i++) r.v[i] = a->v[i] + c->v[i]; break;
    case MAC_MAD: for (i = 0; i < 4; i++) r.v[i] = a->v[i] * b->v[i] + c->v[i]; break;
    case MAC_DP3: r = splat(a->v[0]*b->v[0] + a->v[1]*b->v[1] + a->v[2]*b->v[2]); break;
    case MAC_DPH: r = splat(a->v[0]*b->v[0] + a->v[1]*b->v[1] + a->v[2]*b->v[2]
                            + b->v[3]); break;
    case MAC_DP4: r = splat(a->v[0]*b->v[0] + a->v[1]*b->v[1] + a->v[2]*b->v[2]
                            + a->v[3]*b->v[3]); break;
    case MAC_DST: r.v[0] = 1.0f; r.v[1] = a->v[1] * b->v[1];
                  r.v[2] = a->v[2]; r.v[3] = b->v[3]; break;
    case MAC_MIN: for (i = 0; i < 4; i++) r.v[i] = a->v[i] < b->v[i] ? a->v[i] : b->v[i]; break;
    case MAC_MAX: for (i = 0; i < 4; i++) r.v[i] = a->v[i] > b->v[i] ? a->v[i] : b->v[i]; break;
    case MAC_SLT: for (i = 0; i < 4; i++) r.v[i] = a->v[i] <  b->v[i] ? 1.0f : 0.0f; break;
    case MAC_SGE: for (i = 0; i < 4; i++) r.v[i] = a->v[i] >= b->v[i] ? 1.0f : 0.0f; break;
    default: *wr = 0; break;            /* NOP; ARL is applied after reads */
    }
    return r;
}

/* The ILU unit's result for op, from source C (NOP: none, *wr stays 0). */
static inline vec4 run_ilu(uint32_t op, const vec4 *cc, int *wr)
{
    float x = cc->v[0];
    vec4 r = *cc;

    *wr = 1;
    switch (op) {
    case ILU_MOV: break;
    case ILU_RCP: r = splat(1.0f / x); break;
    case ILU_RCC: r = splat(rcc(x)); break;
    case ILU_RSQ: r = splat(1.0f / sqrtf(fabsf(x))); break;
    case ILU_EXP: {
        float f = floorf(x);
        r.v[0] = exp2f(f); r.v[1] = x - f; r.v[2] = exp2f(x); r.v[3] = 1.0f;
        break;
    }
    case ILU_LOG: {
        float t = fabsf(x);
        if (t == 0.0f) {
            r.v[0] = -INFINITY; r.v[1] = 1.0f; r.v[2] = -INFINITY;
        } else {
            float e = floorf(log2f(t));
            r.v[0] = e; r.v[1] = t / exp2f(e); r.v[2] = log2f(t);
        }
        r.v[3] = 1.0f;
        break;
    }
    case ILU_LIT: {
        float p = cc->v[3];
        if (p < -127.9961f) p = -127.9961f;
        if (p >  127.9961f) p =  127.9961f;
        r.v[0] = 1.0f;
        r.v[1] = x > 0.0f ? x : 0.0f;
        r.v[2] = x > 0.0f ? powf(cc->v[1] > 0.0f ? cc->v[1] : 0.0f, p) : 0.0f;
        r.v[3] = 1.0f;
        break;
    }
    default: *wr = 0; break;
    }
    return r;
}

static inline void write_masked(float *d, const vec4 *s, uint32_t mask)
{
    if (mask & 8) d[0] = s->v[0];
    if (mask & 4) d[1] = s->v[1];
    if (mask & 2) d[2] = s->v[2];
    if (mask & 1) d[3] = s->v[3];
}

/* Unit result `res` to its output register (orb) or constant. */
static inline void write_out(float (*o)[4], float c[NV2A_VSH_CONSTANTS][4],
                             int const_write, const uint32_t *w, const vec4 *res)
{
    uint32_t addr = NV2A_VSH_O_ADDR(w), mask = NV2A_VSH_O_MASK(w);
    if (NV2A_VSH_ORB(w)) {
        if (addr < NV2A_VSH_O_COUNT)
            write_masked(o[addr], res, mask);
    } else if (const_write && addr < NV2A_VSH_CONSTANTS) {
        write_masked(c[addr], res, mask);
    }
}

void nv2a_vsh_run(const uint32_t prog[NV2A_VSH_SLOTS][4], uint32_t start,
                  float c[NV2A_VSH_CONSTANTS][4], int const_write,
                  const float v[NV2A_VSH_INPUTS][4], Nv2aVshOut *out)
{
    /* R0..R11, then R12, which is o[0] (oPos): the temps live right before
     * the outputs so a temp index of 12 lands on oPos. */
    float regs[12 + NV2A_VSH_O_COUNT][4];
    float (*o)[4] = &regs[12];
    uint32_t pc;
    int a0 = 0, i, steps = 0;

    memset(regs, 0, 12 * sizeof regs[0]);
    for (i = 0; i < NV2A_VSH_O_COUNT; i++) {
        o[i][0] = o[i][1] = o[i][2] = 0.0f;
        o[i][3] = 1.0f;
    }
    out->ok = 0;

    for (pc = start; pc < NV2A_VSH_SLOTS; pc++) {
        const uint32_t *w = prog[pc];
        uint32_t mac = NV2A_VSH_MAC(w), ilu = NV2A_VSH_ILU(w);
        const float (*rr)[4] = (const float (*)[4])regs;
        const float (*cr)[4] = (const float (*)[4])c;
        vec4 a = read_src(w, 0, rr, cr, v, a0);
        vec4 b = read_src(w, 1, rr, cr, v, a0);
        vec4 cc = read_src(w, 2, rr, cr, v, a0);
        vec4 mr, ir;
        int mac_wr = 0, ilu_wr = 0;

        steps++;
        mr = run_mac(mac, &a, &b, &cc, &mac_wr);
        ir = run_ilu(ilu, &cc, &ilu_wr);

        /* Writes, after every read. */
        if (mac == MAC_ARL)
            a0 = (int)floorf(a.v[0] + 0.001f);
        if (mac_wr) {
            uint32_t r = NV2A_VSH_OUT_R(w), mask = NV2A_VSH_MAC_MASK(w);
            if (mask && r <= 12)
                write_masked(regs[r], &mr, mask);
            if (NV2A_VSH_O_MUX(w) == 0 && NV2A_VSH_O_MASK(w))
                write_out(o, c, const_write, w, &mr);
        }
        if (ilu_wr) {
            uint32_t r = mac != MAC_NOP ? 1 : NV2A_VSH_OUT_R(w);
            uint32_t mask = NV2A_VSH_ILU_MASK(w);
            if (mask && r <= 12)
                write_masked(regs[r], &ir, mask);
            if (NV2A_VSH_O_MUX(w) == 1 && NV2A_VSH_O_MASK(w))
                write_out(o, c, const_write, w, &ir);
        }

        if (NV2A_VSH_FINAL(w)) {
            out->ok = 1;
            break;
        }
    }
    memcpy(out->o, o, sizeof out->o);
    out->steps = steps;
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
    struct nv2a_vsh_src s;
    char swz[6] = { 0 };
    uint32_t c[4];
    int k;

    nv2a_vsh_src_decode(w, which, &s);
    for (k = 0; k < 4; k++) c[k] = NV2A_VSH_SWZ(s.swz, k);
    if (c[0] == 0 && c[1] == 1 && c[2] == 2 && c[3] == 3)
        swz[0] = 0;
    else if (c[0] == c[1] && c[1] == c[2] && c[2] == c[3]) {
        swz[0] = '.'; swz[1] = xyzw[c[0]];
    } else {
        swz[0] = '.';
        for (k = 0; k < 4; k++) swz[1 + k] = xyzw[c[k]];
    }
#define ROOM (size > n ? (size_t)(size - n) : 0)
    switch (s.mux) {
    case NV2A_VSH_MUX_R: n += snprintf(b + n, ROOM, ", %sR%d%s", s.neg ? "-" : "", (int)s.reg, swz); break;
    case NV2A_VSH_MUX_V: n += snprintf(b + n, ROOM, ", %sv%u%s", s.neg ? "-" : "", NV2A_VSH_INPUT(w), swz); break;
    case NV2A_VSH_MUX_C:
        n += snprintf(b + n, ROOM, ", %sc[%s%u]%s", s.neg ? "-" : "",
                      NV2A_VSH_A0X(w) ? "a0+" : "", NV2A_VSH_CONST(w), swz);
        break;
    default: n += snprintf(b + n, ROOM, ", ?"); break;
    }
    return n;
}

char *nv2a_vsh_disasm(const uint32_t w[4], char *b, int size)
{
    uint32_t mac = NV2A_VSH_MAC(w), ilu = NV2A_VSH_ILU(w);
    uint32_t out_r = NV2A_VSH_OUT_R(w);
    uint32_t mac_mask = NV2A_VSH_MAC_MASK(w), ilu_mask = NV2A_VSH_ILU_MASK(w);
    uint32_t o_mask = NV2A_VSH_O_MASK(w), orb = NV2A_VSH_ORB(w);
    uint32_t addr = NV2A_VSH_O_ADDR(w), omux = NV2A_VSH_O_MUX(w);
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
    if (NV2A_VSH_FINAL(w))
        n += snprintf(b + n, ROOM, "  ; final");
#undef ROOM
    return b;
}
