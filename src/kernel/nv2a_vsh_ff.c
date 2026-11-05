/**
 * NV2A fixed-function transform and lighting as microcode -- see
 * nv2a_vsh_ff.h.
 *
 * The encoder writes one unit per slot (a MAC op, or an ILU op with the MAC
 * idle), so no instruction depends on the paired-issue rules, and every
 * temp the ILU writes is the instruction's own R field. An instruction has
 * one input and one constant index, which the operand order below respects:
 * no slot reads two different v or two different c registers.
 *
 * Temps: R0 eye-space position, R1 ILU results, R2 eye-space normal, R3 and
 * R4 the diffuse and specular sums, R5 the light's scalars (x N.L, y N.H,
 * z specular power, w attenuation), R7 the eye position divided by w, R8
 * the eye vector, R6 and R9..R11 scratch. R12 is oPos.
 */
#include "nv2a_vsh_ff.h"

#include <math.h>
#include <string.h>

enum {
    MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
    MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE
};
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ };
enum { MUX_R = 1, MUX_V = 2, MUX_C = 3 };
enum { O_POS = 0, O_D0 = 3, O_D1 = 4, O_FOG = 5, O_PTS = 6, O_B0 = 7, O_B1 = 8, O_T0 = 9 };

/* Transform-context rows (xemu nv2a_regs.h NV_IGRAPH_XF_XFCTX_*). */
enum {
    XF_CMAT0 = 0x00, XF_MMAT0 = 0x08, XF_IMMAT0 = 0x0C, XF_EYEP = 0x38,
    XF_FOG = 0x39, XF_VPOFF = 0x3B, XF_TG0MAT = 0x40, XF_T0MAT = 0x44
};

/* Light-constant rows, relative to NV2A_FF_LC_BASE. */
enum {
    LC_K = 0, LC_SCENE = 1, LC_EMIS = 2, LC_SPEC = 3, LC_PTS = 4, LC_PP0 = 5,
    LC_PP1 = 6, LC_LIGHT0 = 7
};
enum { L_AMB, L_DIF, L_SPC, L_DIR, L_HALF, L_POS, L_ATT, L_SPOT, L_FALL };

#define LC(row) (NV2A_FF_LC_BASE + (row))
#define LROW(i, k) LC(LC_LIGHT0 + (i) * NV2A_FF_LROWS + (k))

/* Raw SET_TEXGEN_* values. */
#define TG_DISABLE    0x0000u
#define TG_EYE        0x2400u
#define TG_OBJECT     0x2401u
#define TG_SPHERE     0x2402u
#define TG_REFLECTION 0x8512u
#define TG_NORMAL     0x8511u

/* A source operand: mux, register (temp, input or constant index), swizzle
 * byte (x in the top two bits) and negate. */
typedef struct { uint8_t mux, reg, swz, neg; } Src;

#define SWZ(x, y, z, w) (uint8_t)(((x) << 6) | ((y) << 4) | ((z) << 2) | (w))
#define XYZW SWZ(0, 1, 2, 3)

static Src R(int n) { Src s = { MUX_R, (uint8_t)n, XYZW, 0 }; return s; }
static Src V(int n) { Src s = { MUX_V, (uint8_t)n, XYZW, 0 }; return s; }
static Src C(int n) { Src s = { MUX_C, (uint8_t)n, XYZW, 0 }; return s; }
static Src neg(Src s) { s.neg ^= 1; return s; }
/* One component, replicated. */
static Src rep(Src s, int k) { s.swz = SWZ(k, k, k, k); return s; }
static Src swz(Src s, int x, int y, int z, int w) { s.swz = SWZ(x, y, z, w); return s; }

/* A destination: temp R (mask tmask) and/or output o (mask omask). */
typedef struct { int r, tmask, o, omask; } Dst;

static Dst T(int r, int mask) { Dst d = { r, mask, -1, 0 }; return d; }
static Dst O(int o, int mask) { Dst d = { 0, 0, o, mask }; return d; }

#define MX 8
#define MY 4
#define MZ 2
#define MW 1
#define MXYZ 0xE
#define MALL 0xF

typedef struct {
    uint32_t (*p)[4];
    int n, max, bad;
} Asm;

static void put_src(uint32_t w[4], int which, Src s)
{
    uint32_t reg = s.mux == MUX_R ? s.reg : 0;
    if (which == 0) {
        w[1] |= ((uint32_t)s.neg << 8) | s.swz;
        w[2] |= (reg << 28) | ((uint32_t)s.mux << 26);
    } else if (which == 1) {
        w[2] |= ((uint32_t)s.neg << 25) | ((uint32_t)s.swz << 17)
              | (reg << 13) | ((uint32_t)s.mux << 11);
    } else {
        w[2] |= ((uint32_t)s.neg << 10) | ((uint32_t)s.swz << 2) | (reg >> 2);
        w[3] |= ((reg & 3u) << 30) | ((uint32_t)s.mux << 28);
    }
}

/* The shared CONST and INPUT fields: every c (v) source of a slot must name
 * the same register, or the slot cannot be encoded. */
static void put_index(Asm *a, uint32_t w[4], const Src *s, int n)
{
    int i, ci = -1, vi = -1;
    for (i = 0; i < n; i++) {
        if (s[i].mux == MUX_C) {
            if (ci >= 0 && ci != s[i].reg) a->bad = 1;
            ci = s[i].reg;
        } else if (s[i].mux == MUX_V) {
            if (vi >= 0 && vi != s[i].reg) a->bad = 1;
            vi = s[i].reg;
        }
    }
    if (ci >= 0) w[1] |= (uint32_t)ci << 13;
    if (vi >= 0) w[1] |= (uint32_t)vi << 9;
}

static uint32_t *slot(Asm *a)
{
    uint32_t *w;
    if (a->n >= a->max) {
        a->bad = 1;
        return NULL;
    }
    w = a->p[a->n++];
    w[0] = w[1] = w[2] = w[3] = 0;
    return w;
}

/* A MAC op. Operand slots follow the unit: MOV reads A, ADD reads A and C,
 * MAD reads all three, the rest A and B. */
static void mac(Asm *a, int op, Dst d, Src x, Src y, Src z)
{
    uint32_t *w = slot(a);
    Src all[3];
    int n = 0;

    if (!w) return;
    all[n++] = x;
    if (op == MAC_ADD) all[n++] = z;
    else if (op != MAC_MOV) {
        all[n++] = y;
        if (op == MAC_MAD) all[n++] = z;
    }
    put_index(a, w, all, n);
    w[1] |= (uint32_t)op << 21;
    put_src(w, 0, x);
    if (op == MAC_ADD) put_src(w, 2, z);
    else if (op != MAC_MOV) {
        put_src(w, 1, y);
        if (op == MAC_MAD) put_src(w, 2, z);
    }
    if (d.tmask) w[3] |= ((uint32_t)d.tmask << 24) | ((uint32_t)d.r << 20);
    else w[3] |= 7u << 20;             /* no temp write: R7 with an empty mask */
    if (d.o >= 0 && d.omask)
        w[3] |= ((uint32_t)d.omask << 12) | (1u << 11) | ((uint32_t)d.o << 3);
}

static Src none(void) { Src s = { 0, 0, XYZW, 0 }; return s; }

static void mov(Asm *a, Dst d, Src x) { mac(a, MAC_MOV, d, x, none(), none()); }
static void add(Asm *a, Dst d, Src x, Src z) { mac(a, MAC_ADD, d, x, none(), z); }
static void op2(Asm *a, int op, Dst d, Src x, Src y) { mac(a, op, d, x, y, none()); }
static void mad(Asm *a, Dst d, Src x, Src y, Src z) { mac(a, MAC_MAD, d, x, y, z); }

/* An ILU op on x's first component (after its swizzle), into temp R1. */
static void ilu(Asm *a, int op, Src x)
{
    uint32_t *w = slot(a);
    if (!w) return;
    put_index(a, w, &x, 1);
    w[1] |= (uint32_t)op << 25;
    put_src(w, 2, x);
    w[3] |= (8u << 16) | (1u << 20);   /* R1.x */
}

/* dst.xyz = normalize(v.xyz), through R1 and scratch temp s. */
static void normalize3(Asm *a, int dst, Src v, int s)
{
    op2(a, MAC_DP3, T(s, MW), v, v);
    ilu(a, ILU_RSQ, rep(R(s), 3));
    op2(a, MAC_MUL, T(dst, MXYZ), v, rep(R(1), 0));
}

/* The input carrying a colour source: 1 diffuse (v3), 2 specular (v4). */
static int color_input(uint32_t src) { return src == 2 ? 4 : 3; }

void nv2a_ff_init(struct nv2a_ff_regs *r)
{
    memset(r, 0, sizeof *r);
    /* xemu pgraph_init: the specular power curve before any SET_SPECULAR_PARAMS. */
    r->spec_params[0] = 0.0f;
    r->spec_params[1] = -0.494592f;
    r->spec_params[2] = 1.494592f;
}

static float pf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

int nv2a_ff_method(struct nv2a_ff_regs *r, uint32_t m, uint32_t p)
{
    if (m >= 0x1000 && m < 0x1400) {          /* SET_LIGHT_*[8], 0x80 apart */
        struct nv2a_ff_light *l = &r->l[(m - 0x1000) / 0x80];
        uint32_t o = (m - 0x1000) % 0x80;
        float f = pf(p);
        if (o < 0x0C) l->amb[o / 4] = f;
        else if (o < 0x18) l->dif[(o - 0x0C) / 4] = f;
        else if (o < 0x24) l->spc[(o - 0x18) / 4] = f;
        else if (o == 0x24) l->range = f;
        else if (o < 0x34) l->half[(o - 0x28) / 4] = f;
        else if (o < 0x40) l->dir[(o - 0x34) / 4] = f;
        else if (o < 0x4C) l->falloff[(o - 0x40) / 4] = f;
        else if (o < 0x5C) l->spot[(o - 0x4C) / 4] = f;
        else if (o < 0x68) l->pos[(o - 0x5C) / 4] = f;
        else if (o < 0x74) l->atten[(o - 0x68) / 4] = f;
        else return 0;
        r->written++;
        return 1;
    }
    if (m >= 0x03C0 && m < 0x0400) {          /* SET_TEXGEN_S..Q[4] */
        r->texgen[(m - 0x03C0) / 16][((m - 0x03C0) % 16) / 4] = p;
        r->written++;
        return 1;
    }
    if (m >= 0x0420 && m < 0x0430) {          /* SET_TEXTURE_MATRIX_ENABLE[4] */
        r->texmat[(m - 0x0420) / 4] = p;
        r->written++;
        return 1;
    }
    if (m >= 0x09E0 && m < 0x09F8) {          /* SET_SPECULAR_PARAMS[6] */
        r->spec_params[(m - 0x09E0) / 4] = pf(p);
        r->written++;
        return 1;
    }
    if (m >= 0x0A10 && m < 0x0A1C) {          /* SET_SCENE_AMBIENT_COLOR */
        r->scene_amb[(m - 0x0A10) / 4] = pf(p);
        r->written++;
        return 1;
    }
    if (m >= 0x0A30 && m < 0x0A50) {          /* SET_POINT_PARAMS[8] */
        r->point_params[(m - 0x0A30) / 4] = pf(p);
        r->written++;
        return 1;
    }
    if (m >= 0x03A8 && m < 0x03B4) {          /* SET_MATERIAL_EMISSION */
        r->emission[(m - 0x03A8) / 4] = pf(p);
        r->written++;
        return 1;
    }
    switch (m) {
    case 0x0314: r->lighting = p != 0; break;        /* SET_LIGHTING_ENABLE */
    case 0x0328: r->skin = p & 7u; break;            /* SET_SKIN_MODE */
    case 0x03A4: r->normalize = p != 0; break;       /* SET_NORMALIZATION_ENABLE */
    case 0x03B4: r->mat_alpha = pf(p); break;        /* SET_MATERIAL_ALPHA */
    case 0x03BC: r->light_mask = p & 0xFFFFu; break; /* SET_LIGHT_ENABLE_MASK */
    case 0x0298: r->color_material = p; break;       /* SET_COLOR_MATERIAL */
    case 0x09CC: r->texgen_view = p & 1u; break;     /* SET_TEXGEN_VIEW_MODEL */
    case 0x0318: r->point_params_enable = p != 0; break; /* SET_POINT_PARAMS_ENABLE */
    case 0x043C:                                     /* SET_POINT_SIZE */
        /* xemu drops a size over V_MAX (0x1FF) and keeps the old one. */
        if (p <= 0x1FFu)
            r->point_size = p;
        break;
    default: return 0;
    }
    r->written++;
    return 1;
}

static int needs_normal_tg(uint32_t t)
{
    return t == TG_SPHERE || t == TG_REFLECTION || t == TG_NORMAL;
}

void nv2a_ff_key_fields(const struct nv2a_ff_regs *r, uint32_t f[NV2A_FF_KEY_WORDS])
{
    int n = 0, i, j;

    memset(f, 0, sizeof(uint32_t) * NV2A_FF_KEY_WORDS);

    f[n++] = r->lighting;
    f[n++] = r->skin;
    f[n++] = r->normalize;
    f[n++] = r->lighting ? r->light_mask : 0;
    f[n++] = r->lighting ? r->color_material & 0xFFu : 0;
    f[n++] = r->texgen_view;
    /* Separate specular, local eye, alpha from material specular. */
    f[n++] = r->light_ctl & 0x30001u;
    f[n++] = r->spec_enable != 0;
    f[n++] = r->point_params_enable;
    f[n++] = r->fog_enable ? 0x100u | (r->fog_gen & 0xFFu) : 0;
    for (i = 0; i < 4; i++) {
        f[n++] = r->texmat[i] ? 1u : 0u;
        for (j = 0; j < 4; j++)
            f[n++] = r->texgen[i][j];
    }
}

uint64_t nv2a_ff_key(const struct nv2a_ff_regs *r)
{
    /* FNV-1a over the fields that pick instructions. */
    uint32_t f[NV2A_FF_KEY_WORDS];
    uint64_t h = 1469598103934665603ull;
    int i;

    nv2a_ff_key_fields(r, f);
    for (i = 0; i < NV2A_FF_KEY_WORDS; i++) {
        h ^= f[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void put3(float d[4], const float s[3], float w)
{
    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = w;
}

/* The light constants. Directions are normalised here, once, where xemu
 * normalises them per vertex. */
void nv2a_ff_consts(const struct nv2a_ff_regs *r, float lc[NV2A_FF_LC_ROWS][4])
{
    int i;

    memset(lc, 0, sizeof(float) * 4 * NV2A_FF_LC_ROWS);
    lc[LC_K][0] = 0.0f; lc[LC_K][1] = 1.0f; lc[LC_K][2] = 0.5f;
    lc[LC_K][3] = r->mat_alpha;
    put3(lc[LC_SCENE], r->scene_amb, 0.0f);
    put3(lc[LC_EMIS], r->emission, 0.0f);
    put3(lc[LC_SPEC], r->spec_params, 10000.0f);
    /* xemu: MAX(1, size) for the fixed size; the point-params clamp is
     * min(p7, 63.875) .. min(p3 + that, 63.875). */
    {
        float sz = (float)r->point_size / 8.0f;
        float lo = fminf(r->point_params[7], 63.875f);
        lc[LC_PTS][0] = sz > 1.0f ? sz : 1.0f;
        lc[LC_PTS][1] = lo;
        lc[LC_PTS][2] = fminf(r->point_params[3] + lo, 63.875f);
        memcpy(lc[LC_PP0], &r->point_params[0], sizeof lc[LC_PP0]);
        memcpy(lc[LC_PP1], &r->point_params[4], sizeof lc[LC_PP1]);
    }
    for (i = 0; i < NV2A_FF_LIGHTS; i++) {
        const struct nv2a_ff_light *l = &r->l[i];
        float (*row)[4] = &lc[LC_LIGHT0 + i * NV2A_FF_LROWS];
        float d = sqrtf(l->dir[0] * l->dir[0] + l->dir[1] * l->dir[1]
                        + l->dir[2] * l->dir[2]);
        put3(row[L_AMB], l->amb, 0.0f);
        put3(row[L_DIF], l->dif, 0.0f);
        put3(row[L_SPC], l->spc, 0.0f);
        put3(row[L_DIR], l->dir, 0.0f);
        if (d > 0.0f) {
            row[L_DIR][0] /= d; row[L_DIR][1] /= d; row[L_DIR][2] /= d;
        }
        put3(row[L_HALF], l->half, 0.0f);
        put3(row[L_POS], l->pos, l->range);
        put3(row[L_ATT], l->atten, 0.0f);
        memcpy(row[L_SPOT], l->spot, sizeof row[L_SPOT]);
        put3(row[L_FALL], l->falloff, 0.0f);
    }
}

/* Skinning, as xemu's append_skinning_code: count matrices, and with mix
 * the last weight is 1 minus the others. Writes R0 (position) and, with
 * normal, R2.xyz. */
static void skin(Asm *a, uint32_t mode, int normal)
{
    static const int count_of[7] = { 0, 2, 2, 3, 3, 4, 4 };
    static const int mix_of[7]   = { 0, 1, 0, 1, 0, 1, 0 };
    int count = count_of[mode], mix = mix_of[mode], i, k;

    if (mix) {                                  /* R11.x = 1 - sum of weights */
        mov(a, T(11, MX), rep(C(LC(LC_K)), 1));
        for (i = 0; i < count - 1; i++)
            add(a, T(11, MX), rep(R(11), 0), neg(rep(V(1), i)));
    }
    for (i = 0; i < count; i++) {
        Src wt = (mix && i == count - 1) ? rep(R(11), 0) : rep(V(1), i);
        for (k = 0; k < 4; k++)
            op2(a, MAC_DP4, T(6, MX >> k), V(0), C(XF_MMAT0 + 8 * i + k));
        if (i == 0) op2(a, MAC_MUL, T(0, MALL), R(6), wt);
        else mad(a, T(0, MALL), R(6), wt, R(0));
        if (normal) {
            for (k = 0; k < 3; k++)
                op2(a, MAC_DP3, T(9, MX >> k), V(2), C(XF_IMMAT0 + 8 * i + k));
            if (i == 0) op2(a, MAC_MUL, T(2, MXYZ), R(9), wt);
            else mad(a, T(2, MXYZ), R(9), wt, R(2));
        }
    }
}

/* oD0 += src * c[row] * s (s a replicated scalar), for colour source src. */
static void acc_term(Asm *a, int dst, uint32_t src, int row, Src s)
{
    if (src == 0) {
        mad(a, T(dst, MXYZ), C(row), s, R(dst));
    } else {
        op2(a, MAC_MUL, T(9, MXYZ), V(color_input(src)), s);
        mad(a, T(dst, MXYZ), R(9), C(row), R(dst));
    }
}

static void light(Asm *a, const struct nv2a_ff_regs *r, int i, uint32_t type,
                  int local_eye, int specular)
{
    uint32_t cm = r->color_material;
    Src k0 = rep(C(LC(LC_K)), 0), k1 = rep(C(LC(LC_K)), 1);

    if (type == 1) {                            /* infinite */
        mov(a, T(5, MW), k1);
        op2(a, MAC_DP3, T(5, MX), R(2), C(LROW(i, L_DIR)));
        if (local_eye) {
            add(a, T(6, MXYZ), R(8), C(LROW(i, L_DIR)));
            normalize3(a, 6, R(6), 10);
            op2(a, MAC_DP3, T(5, MY), R(2), R(6));
        } else {
            op2(a, MAC_DP3, T(5, MY), R(2), C(LROW(i, L_HALF)));
        }
    } else {                                    /* local, spot */
        Src pos = C(LROW(i, L_POS));
        add(a, T(6, MXYZ), pos, neg(R(7)));                 /* VP */
        op2(a, MAC_DP3, T(9, MX), R(6), R(6));              /* d^2 */
        ilu(a, ILU_RSQ, rep(R(9), 0));                      /* 1/d */
        op2(a, MAC_MUL, T(9, MY), rep(R(9), 0), rep(R(1), 0)); /* d */
        op2(a, MAC_SGE, T(9, MZ), rep(pos, 3), rep(R(9), 1));  /* d <= range */
        op2(a, MAC_MUL, T(6, MXYZ), R(6), rep(R(1), 0));    /* VP normalised */
        mov(a, T(10, MX), k1);
        mov(a, T(10, MY), rep(R(9), 1));
        mov(a, T(10, MZ), rep(R(9), 0));
        op2(a, MAC_DP3, T(10, MW), R(10), C(LROW(i, L_ATT)));
        ilu(a, ILU_RCP, rep(R(10), 3));
        op2(a, MAC_MUL, T(5, MW), rep(R(1), 0), rep(R(9), 2));
        if (local_eye) {
            add(a, T(10, MXYZ), R(6), R(8));
            normalize3(a, 10, R(10), 11);
        } else {
            mov(a, T(10, MXYZ), R(6));
        }
        op2(a, MAC_DP3, T(5, MX), R(2), R(6));
        op2(a, MAC_DP3, T(5, MY), R(2), R(10));
        if (type == 3) {
            /* Spot factor, xemu's rational falloff (US6417851B1):
             * s = spot.xyz . VP + spot.w; 0 at s <= 0, 1 at s >= 1, else
             * clamp((s + L) / (M s + N), 0, 1). */
            Src fall = C(LROW(i, L_FALL));
            op2(a, MAC_DPH, T(9, MX), R(6), C(LROW(i, L_SPOT)));
            add(a, T(9, MY), rep(R(9), 0), rep(fall, 0));
            mad(a, T(9, MZ), rep(R(9), 0), rep(fall, 1), rep(fall, 2));
            ilu(a, ILU_RCC, rep(R(9), 2));
            op2(a, MAC_MUL, T(9, MY), rep(R(9), 1), rep(R(1), 0));
            op2(a, MAC_MAX, T(9, MY), rep(R(9), 1), k0);
            op2(a, MAC_MIN, T(9, MY), rep(R(9), 1), k1);
            op2(a, MAC_SGE, T(9, MZ), rep(R(9), 0), k1);
            op2(a, MAC_MAX, T(9, MY), rep(R(9), 1), rep(R(9), 2));
            op2(a, MAC_SLT, T(9, MZ), k0, rep(R(9), 0));
            op2(a, MAC_MUL, T(9, MY), rep(R(9), 1), rep(R(9), 2));
            op2(a, MAC_MUL, T(5, MW), rep(R(5), 3), rep(R(9), 1));
        }
    }
    op2(a, MAC_MAX, T(5, MX | MY), R(5), k0);

    if (specular) {
        /* pf = clamp((N.H + L) / (M N.H + N), 0, 10000) where N.L > 0. */
        Src sp = C(LC(LC_SPEC));
        add(a, T(9, MX), rep(R(5), 1), rep(sp, 0));
        mad(a, T(9, MY), rep(R(5), 1), rep(sp, 1), rep(sp, 2));
        ilu(a, ILU_RCC, rep(R(9), 1));
        op2(a, MAC_MUL, T(9, MX), rep(R(9), 0), rep(R(1), 0));
        op2(a, MAC_MAX, T(9, MX), rep(R(9), 0), k0);
        op2(a, MAC_MIN, T(9, MX), rep(R(9), 0), rep(sp, 3));
        op2(a, MAC_SLT, T(9, MY), k0, rep(R(5), 0));
        op2(a, MAC_MUL, T(5, MZ), rep(R(9), 0), rep(R(9), 1));
    }

    /* ambient * att, diffuse * att * N.L, specular * att * pf */
    acc_term(a, 3, (cm >> 2) & 3u, LROW(i, L_AMB), rep(R(5), 3));
    op2(a, MAC_MUL, T(11, MX | MY), rep(R(5), 3), swz(R(5), 0, 2, 0, 0));
    acc_term(a, 3, (cm >> 4) & 3u, LROW(i, L_DIF), rep(R(11), 0));
    if (specular)
        acc_term(a, 4, (cm >> 6) & 3u, LROW(i, L_SPC), rep(R(11), 1));
}

int nv2a_ff_build(const struct nv2a_ff_regs *r, uint32_t (*prog)[4],
                  int max_slots, float lc[NV2A_FF_LC_ROWS][4])
{
    Asm a = { prog, 0, max_slots, 0 };
    Src k0 = rep(C(LC(LC_K)), 0);
    int lit = r->lighting != 0, skinned = r->skin != 0 && r->skin <= 6;
    int local_eye = lit && (r->light_ctl & 0x10000u);
    int separate = (r->light_ctl & 1u) != 0;
    int fog = r->fog_enable ? (int)(r->fog_gen & 7u) : -1;
    int need_eye = lit || skinned || fog == 1 || fog == 2 || fog == 3;
    int need_normal = lit, need_refl = 0, i, j;

    if (lc)
        nv2a_ff_consts(r, lc);
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            uint32_t t = r->texgen[i][j];
            if (t == TG_EYE) need_eye = 1;
            if (needs_normal_tg(t)) need_normal = 1;
            if (t == TG_SPHERE || t == TG_REFLECTION) {
                need_refl = 1;
                if (!r->texgen_view) need_eye = 1;
            }
        }

    /* Eye-space position and normal (xemu: tPosition, tNormal). */
    if (skinned) {
        skin(&a, r->skin, need_normal);
    } else {
        if (need_eye)
            for (j = 0; j < 4; j++)
                op2(&a, MAC_DP4, T(0, MX >> j), V(0), C(XF_MMAT0 + j));
        if (need_normal)
            for (j = 0; j < 3; j++)
                op2(&a, MAC_DP3, T(2, MX >> j), V(2), C(XF_IMMAT0 + j));
    }
    if (need_normal && r->normalize)
        normalize3(&a, 2, R(2), 6);

    if (lit) {
        uint32_t cm = r->color_material;
        uint32_t em = cm & 3u, amb = (cm >> 2) & 3u, dif = (cm >> 4) & 3u;
        int specular = r->spec_enable != 0;

        /* oD0 starts as xemu's emission and scene-ambient terms. */
        if (em == 0 || amb == 0) {
            uint32_t added = em != 0 ? em : amb;
            mov(&a, T(3, MXYZ), C(LC(LC_SCENE)));
            if (added)
                mad(&a, T(3, MXYZ), V(color_input(added)), C(LC(LC_EMIS)), R(3));
        } else {
            op2(&a, MAC_MUL, T(6, MXYZ), V(color_input(amb)), C(LC(LC_EMIS)));
            add(&a, T(3, MXYZ), R(6), V(color_input(em)));
        }
        if (dif == 0) mov(&a, T(3, MW), rep(C(LC(LC_K)), 3));
        else mov(&a, T(3, MW), rep(V(color_input(dif)), 3));
        mov(&a, T(4, MXYZ), k0);
        mov(&a, T(4, MW), rep(V(4), 3));

        if (r->light_mask) {
            /* R7 = position / w, for local lights and the eye vector. */
            ilu(&a, ILU_RCP, rep(R(0), 3));
            op2(&a, MAC_MUL, T(7, MXYZ), R(0), rep(R(1), 0));
        }
        if (local_eye) {
            ilu(&a, ILU_RCP, rep(C(XF_EYEP), 3));
            op2(&a, MAC_MUL, T(6, MXYZ), C(XF_EYEP), rep(R(1), 0));
            add(&a, T(8, MXYZ), R(6), neg(R(7)));
            normalize3(&a, 8, R(8), 6);
        }
        for (i = 0; i < NV2A_FF_LIGHTS; i++) {
            uint32_t type = (r->light_mask >> (2 * i)) & 3u;
            if (type)
                light(&a, r, i, type, local_eye, specular);
        }
        if (specular && !separate)
            add(&a, T(3, MXYZ), R(3), R(4));
        if (specular)
            mov(&a, O(O_D1, MALL), separate ? R(4) : V(4));
        mov(&a, O(O_D0, MALL), R(3));
    } else {
        mov(&a, O(O_D0, MALL), V(3));
        if (r->spec_enable)
            mov(&a, O(O_D1, MALL), V(4));
    }
    mov(&a, O(O_B0, MALL), V(7));
    /* xemu: with specular off both specular outputs are (0, 0, 0, 1), lit
     * or not; with it on, LIGHT_CONTROL bit 17 (alpha from material
     * specular) clear means the specular alpha is ignored, forced to 1. */
    if (!r->spec_enable) {
        mov(&a, O(O_D1, MALL), swz(C(LC(LC_K)), 0, 0, 0, 1));
        mov(&a, O(O_B1, MALL), swz(C(LC(LC_K)), 0, 0, 0, 1));
    } else {
        mov(&a, O(O_B1, MALL), V(8));
        if (!(r->light_ctl & 0x20000u)) {
            mov(&a, O(O_D1, MW), rep(C(LC(LC_K)), 1));
            mov(&a, O(O_B1, MW), rep(C(LC(LC_K)), 1));
        }
    }

    /* Texgen, then the texture matrix (xemu: oT = oT * texMat). */
    if (need_refl) {
        /* R10 = reflect(u, N), u the eye ray (0, 0, 1 for an infinite viewer). */
        if (r->texgen_view)
            mov(&a, T(10, MXYZ), swz(C(LC(LC_K)), 0, 0, 1, 1));
        else
            normalize3(&a, 10, R(0), 11);
        op2(&a, MAC_DP3, T(11, MX), R(2), R(10));
        add(&a, T(11, MX), rep(R(11), 0), rep(R(11), 0));
        mad(&a, T(10, MXYZ), neg(R(2)), rep(R(11), 0), R(10));
        /* R11.w = 1 / (2 |r + (0, 0, 1)|), the sphere map's scale. */
        add(&a, T(11, MXYZ), R(10), swz(C(LC(LC_K)), 0, 0, 1, 1));
        op2(&a, MAC_DP3, T(11, MW), R(11), R(11));
        ilu(&a, ILU_RSQ, rep(R(11), 3));
        op2(&a, MAC_MUL, T(11, MW), rep(R(1), 0), rep(C(LC(LC_K)), 2));
    }
    for (i = 0; i < 4; i++) {
        int any = 0;
        for (j = 0; j < 4; j++)
            any |= r->texgen[i][j] != TG_DISABLE;
        if (!any && !r->texmat[i]) {
            mov(&a, O(O_T0 + i, MALL), V(9 + i));
            continue;
        }
        mov(&a, T(9, MALL), V(9 + i));
        for (j = 0; j < 4; j++) {
            int m = MX >> j;
            switch (r->texgen[i][j]) {
            case TG_EYE:
                op2(&a, MAC_DP4, T(9, m), R(0), C(XF_TG0MAT + 8 * i + j));
                break;
            case TG_OBJECT:
                op2(&a, MAC_DP4, T(9, m), V(0), C(XF_TG0MAT + 8 * i + j));
                break;
            case TG_SPHERE:
                mad(&a, T(9, m), R(10), rep(R(11), 3), rep(C(LC(LC_K)), 2));
                break;
            case TG_REFLECTION:
                mov(&a, T(9, m), R(10));
                break;
            case TG_NORMAL:
                mov(&a, T(9, m), R(2));
                break;
            default:
                break;
            }
        }
        if (r->texmat[i])
            for (j = 0; j < 4; j++)
                op2(&a, MAC_DP4, O(O_T0 + i, MX >> j), R(9), C(XF_T0MAT + 8 * i + j));
        else
            mov(&a, O(O_T0 + i, MALL), R(9));
    }

    /* Fog distance in oFog.x; the backends' fog stage takes it from there,
     * as it does for a program (xemu vsh.c). */
    switch (fog) {
    case 0:                                     /* specular alpha */
        op2(&a, MAC_MAX, T(6, MX), rep(V(4), 3), k0);
        op2(&a, MAC_MIN, O(O_FOG, MX), rep(R(6), 0), rep(C(LC(LC_K)), 1));
        break;
    case 1:                                     /* radial */
        op2(&a, MAC_DP3, T(6, MX), R(0), R(0));
        ilu(&a, ILU_RSQ, rep(R(6), 0));
        op2(&a, MAC_MUL, O(O_FOG, MX), rep(R(6), 0), rep(R(1), 0));
        break;
    case 2:                                     /* planar */
        op2(&a, MAC_DPH, O(O_FOG, MX), R(0), C(XF_FOG));
        break;
    case 3:                                     /* abs planar */
        op2(&a, MAC_DPH, T(6, MX), R(0), C(XF_FOG));
        op2(&a, MAC_MAX, O(O_FOG, MX), rep(R(6), 0), neg(rep(R(6), 0)));
        break;
    case 6:                                     /* fog coordinate */
        mov(&a, O(O_FOG, MX), rep(V(5), 0));
        break;
    default:
        break;
    }

    /* Point size (xemu): the fixed size, or with point params the
     * attenuation by the eye distance |position * modelView0| (unskinned,
     * as xemu), clamped. NV2A's rsq takes |x| where GLSL gives NaN. */
    if (r->point_params_enable) {
        for (j = 0; j < 4; j++)
            op2(&a, MAC_DP4, T(6, MX >> j), V(0), C(XF_MMAT0 + j));
        op2(&a, MAC_DP4, T(6, MX), R(6), R(6));
        ilu(&a, ILU_RSQ, rep(R(6), 0));
        op2(&a, MAC_MUL, T(6, MY), rep(R(6), 0), rep(R(1), 0));
        mad(&a, T(6, MZ), rep(R(6), 1), rep(C(LC(LC_PP0)), 2), rep(C(LC(LC_PP0)), 1));
        mad(&a, T(6, MZ), rep(R(6), 2), rep(R(6), 1), rep(C(LC(LC_PP0)), 0));
        ilu(&a, ILU_RSQ, rep(R(6), 2));
        add(&a, T(6, MW), rep(R(1), 0), rep(C(LC(LC_PP1)), 2));
        op2(&a, MAC_MUL, T(6, MW), rep(R(6), 3), rep(C(LC(LC_PP0)), 3));
        add(&a, T(6, MW), rep(R(6), 3), rep(C(LC(LC_PP1)), 3));
        op2(&a, MAC_MAX, T(6, MW), rep(R(6), 3), rep(C(LC(LC_PTS)), 1));
        op2(&a, MAC_MIN, O(O_PTS, MX), rep(R(6), 3), rep(C(LC(LC_PTS)), 2));
    } else {
        mov(&a, O(O_PTS, MX), rep(C(LC(LC_PTS)), 0));
    }

    /* Position. Unskinned, the composite matrix already holds the
     * model-view; skinned, it takes the blended eye-space position. Then the
     * XDK epilogue's divide, with only the xy offset added (xemu adds
     * VPOFF.xy and divides z by w). */
    for (j = 0; j < 4; j++)
        op2(&a, MAC_DP4, O(O_POS, MX >> j), skinned ? R(0) : V(0), C(XF_CMAT0 + j));
    ilu(&a, ILU_RCC, rep(R(12), 3));
    mad(&a, O(O_POS, MX | MY), R(12), rep(R(1), 0), C(XF_VPOFF));
    op2(&a, MAC_MUL, O(O_POS, MZ), R(12), rep(R(1), 0));
    if (a.bad || a.n == 0)
        return 0;
    a.p[a.n - 1][3] |= 1u;                      /* FINAL */
    return a.n;
}
