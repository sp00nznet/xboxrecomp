/* The fixed-function pipeline as microcode (nv2a_vsh_ff.c), run through the
 * CPU interpreter and the translators' parser.
 *
 * Each case sets fixed-function registers through nv2a_ff_method, the way the
 * pushbuffer does, builds the program, runs one vertex and checks the
 * outputs against values worked out by hand from xemu's vsh-ff.c. The
 * parser check matters as much: a program the parser reads differently
 * from the interpreter would draw on the GPUs and not in this test. */
#include "nv2a_vsh_cpu.h"
#include "nv2a_vsh_ff.h"
#include "d3d8_vsh_parse.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t prog[NV2A_VSH_SLOTS][4];
static float c[NV2A_VSH_CONSTANTS][4];
static float in[NV2A_VSH_INPUTS][4];
static float lc[NV2A_FF_LC_ROWS][4];
static NV2AVshProgram parsed;
static int failures;

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void setf(struct nv2a_ff_regs *r, uint32_t m, float f)
{
    nv2a_ff_method(r, m, fbits(f));
}

static void row(int n, float x, float y, float z, float w)
{
    c[n][0] = x; c[n][1] = y; c[n][2] = z; c[n][3] = w;
}

static void vin(int n, float x, float y, float z, float w)
{
    in[n][0] = x; in[n][1] = y; in[n][2] = z; in[n][3] = w;
}

static void identity(int base)
{
    int i;
    for (i = 0; i < 4; i++)
        row(base + i, i == 0, i == 1, i == 2, i == 3);
}

/* The transform context as a title leaves it: identity model-view and
 * inverse, identity composite, the viewport offset. */
static void reset(struct nv2a_ff_regs *r)
{
    nv2a_ff_init(r);
    memset(c, 0, sizeof c);
    memset(in, 0, sizeof in);
    identity(0x00);
    identity(0x08);
    identity(0x0C);
    row(0x3B, 0.53f, 0.53f, 0, 0);
    vin(0, 0, 0, 0, 1);
    vin(3, 0.1f, 0.2f, 0.3f, 0.4f);
    vin(4, 0.5f, 0.6f, 0.7f, 0.8f);
    vin(9, 0.25f, 0.5f, 0, 1);
}

static int run(const char *name, struct nv2a_ff_regs *r, Nv2aVshOut *o)
{
    int n = nv2a_ff_build(r, prog, NV2A_VSH_SLOTS, lc), k;

    if (n <= 0) {
        printf("FAIL %s: build returned %d\n", name, n);
        failures++;
        return 0;
    }
    memcpy(c[NV2A_FF_LC_BASE], lc, sizeof lc);
    nv2a_vsh_run((const uint32_t (*)[4])prog, 0, c, 0,
                 (const float (*)[4])in, o);
    if (!o->ok) {
        printf("FAIL %s: program did not reach FINAL\n", name);
        failures++;
        return 0;
    }
    d3d8_vsh_parse(&prog[0][0], n, &parsed);
    if (parsed.length != n || !parsed.insns[n - 1].is_final) {
        printf("FAIL %s: parser read %d slots of %d\n", name, parsed.length, n);
        failures++;
    }
    for (k = 0; k < n; k++)
        if (parsed.insns[k].const_index >= NV2A_VSH_CONSTANTS) {
            printf("FAIL %s: slot %d reads c[%d]\n", name, k,
                   parsed.insns[k].const_index);
            failures++;
        }
    return n;
}

static void expect(const char *name, const float *got, float x, float y,
                   float z, float w)
{
    const float want[4] = { x, y, z, w };
    int k;
    for (k = 0; k < 4; k++)
        if (!(fabsf(got[k] - want[k]) <= 1e-4f * (1.0f + fabsf(want[k])))) {
            printf("FAIL %s: got %g %g %g %g, want %g %g %g %g\n", name,
                   got[0], got[1], got[2], got[3], x, y, z, w);
            failures++;
            return;
        }
}

static void expect1(const char *name, float got, float want)
{
    if (!(fabsf(got - want) <= 1e-4f * (1.0f + fabsf(want)))) {
        printf("FAIL %s: got %g, want %g\n", name, got, want);
        failures++;
    }
}

/* Unlit: the composite transform, divide and xy offset; colours and
 * texture coordinates pass through, the specular pair only with specular
 * on (xemu: off, both are 0, 0, 0, 1). */
static void test_unlit(void)
{
    struct nv2a_ff_regs r;
    Nv2aVshOut o;

    reset(&r);
    row(0, 2, 0, 0, 1);
    row(1, 0, 3, 0, 0);
    row(2, 0, 0, 4, 0);
    row(3, 0, 0, 1, 0);                    /* w = z */
    vin(0, 1, 2, 3, 1);
    vin(7, 1, 2, 3, 4);
    vin(8, 0.3f, 0.2f, 0.1f, 0.6f);
    vin(10, 5, 6, 7, 8);
    if (!run("unlit", &r, &o)) return;
    /* x = (2+1)/3 + 0.53, y = 6/3 + 0.53, z = 12/3, w = 3 */
    expect("unlit oPos", o.o[NV2A_VSH_O_POS], 1.53f, 2.53f, 4, 3);
    expect("unlit oD0", o.o[NV2A_VSH_O_D0], 0.1f, 0.2f, 0.3f, 0.4f);
    expect("unlit oD1", o.o[NV2A_VSH_O_D1], 0, 0, 0, 1);
    expect("unlit oB0", o.o[NV2A_VSH_O_B0], 1, 2, 3, 4);
    expect("unlit oB1", o.o[NV2A_VSH_O_B1], 0, 0, 0, 1);
    expect("unlit oT0", o.o[NV2A_VSH_O_T0], 0.25f, 0.5f, 0, 1);
    expect("unlit oT1", o.o[NV2A_VSH_O_T0 + 1], 5, 6, 7, 8);

    /* Specular on, alpha from material specular (LIGHT_CONTROL bit 17). */
    r.spec_enable = 1;
    r.light_ctl = 0x20000;
    if (!run("unlit specular", &r, &o)) return;
    expect("unlit specular oD1", o.o[NV2A_VSH_O_D1], 0.5f, 0.6f, 0.7f, 0.8f);
    expect("unlit specular oB1", o.o[NV2A_VSH_O_B1], 0.3f, 0.2f, 0.1f, 0.6f);

    /* Bit 17 clear: the specular alpha is ignored, forced to 1. */
    r.light_ctl = 0;
    if (!run("unlit ignore alpha", &r, &o)) return;
    expect("unlit ignore alpha oD1", o.o[NV2A_VSH_O_D1], 0.5f, 0.6f, 0.7f, 1);
    expect("unlit ignore alpha oB1", o.o[NV2A_VSH_O_B1], 0.3f, 0.2f, 0.1f, 1);
}

static void material(struct nv2a_ff_regs *r)
{
    nv2a_ff_method(r, 0x0314, 1);          /* lighting */
    nv2a_ff_method(r, 0x03A4, 1);          /* normalization */
    setf(r, 0x0A10, 0.1f); setf(r, 0x0A14, 0.1f); setf(r, 0x0A18, 0.1f);
    setf(r, 0x03A8, 0.05f);                /* emission: unused, see below */
    setf(r, 0x03B4, 0.7f);                 /* material alpha */
    vin(2, 0, 0, 2, 0);                    /* normal, unnormalised */
}

/* One infinite light, every colour from the material. xemu starts oD0 at
 * the scene ambient and adds the material emission only when a vertex
 * colour feeds emission or ambient (the XDK folds it into the scene
 * ambient). */
static void test_infinite(void)
{
    struct nv2a_ff_regs r;
    Nv2aVshOut o;

    reset(&r);
    material(&r);
    nv2a_ff_method(&r, 0x03BC, 1);         /* light 0 infinite */
    setf(&r, 0x1000, 0.2f);                /* ambient */
    setf(&r, 0x1010, 0.5f);                /* diffuse g */
    setf(&r, 0x1020, 1.0f);                /* specular b */
    setf(&r, 0x1030, 1.0f);                /* half vector z */
    setf(&r, 0x103C, 3.0f);                /* direction z, unnormalised */
    r.spec_enable = 1;
    r.light_ctl = 0x20001;                 /* separate, alpha from specular */
    if (!run("infinite", &r, &o)) return;
    expect("infinite oD0", o.o[NV2A_VSH_O_D0], 0.3f, 0.6f, 0.1f, 0.7f);
    expect("infinite oD1", o.o[NV2A_VSH_O_D1], 0, 0, 1, 0.8f);

    /* Specular off: no specular in oD0, oD1 is 0, 0, 0, 1. */
    r.spec_enable = 0;
    if (!run("infinite no specular", &r, &o)) return;
    expect("infinite no specular oD0", o.o[NV2A_VSH_O_D0], 0.3f, 0.6f, 0.1f, 0.7f);
    expect("infinite no specular oD1", o.o[NV2A_VSH_O_D1], 0, 0, 0, 1);
    r.spec_enable = 1;

    /* Not separate: the specular sum joins oD0, oD1 is the vertex specular. */
    r.light_ctl = 0x20000;
    if (!run("combined", &r, &o)) return;
    expect("combined oD0", o.o[NV2A_VSH_O_D0], 0.3f, 0.6f, 1.1f, 0.7f);
    expect("combined oD1", o.o[NV2A_VSH_O_D1], 0.5f, 0.6f, 0.7f, 0.8f);

    /* Ambient and diffuse from the vertex diffuse colour: oD0 = scene +
     * v3 * emission, then v3 * ambient + v3 * diffuse, alpha v3.w. */
    nv2a_ff_method(&r, 0x0298, (1u << 2) | (1u << 4));
    r.light_ctl = 1;
    if (!run("colour material", &r, &o)) return;
    expect("colour material oD0", o.o[NV2A_VSH_O_D0],
           0.1f + 0.1f * 0.05f + 0.1f * 0.2f, 0.1f + 0.2f * 0.5f, 0.1f, 0.4f);
}

/* A local light: distance attenuation and the range cut. */
static void test_local(void)
{
    struct nv2a_ff_regs r;
    Nv2aVshOut o;

    reset(&r);
    material(&r);
    setf(&r, 0x0A10, 0); setf(&r, 0x0A14, 0); setf(&r, 0x0A18, 0);
    nv2a_ff_method(&r, 0x03BC, 2);         /* light 0 local */
    setf(&r, 0x100C, 1); setf(&r, 0x1010, 1); setf(&r, 0x1014, 1);
    setf(&r, 0x1024, 100);                 /* range */
    setf(&r, 0x1064, 10);                  /* position z */
    setf(&r, 0x1068, 1); setf(&r, 0x106C, 0.1f);   /* 1 + 0.1 d */
    if (!run("local", &r, &o)) return;
    expect("local oD0", o.o[NV2A_VSH_O_D0], 0.5f, 0.5f, 0.5f, 0.7f);

    setf(&r, 0x1024, 5);                   /* out of range */
    if (!run("local range", &r, &o)) return;
    expect("local range oD0", o.o[NV2A_VSH_O_D0], 0, 0, 0, 0.7f);
}

/* A spot light: s = spot.xyz . VP + spot.w through the falloff curve,
 * 0 at s <= 0 and 1 at s >= 1. */
static void test_spot(void)
{
    static const float ws[3] = { 1.5f, 0.5f, 3.0f }, want[3] = { 0.5f, 0, 1 };
    struct nv2a_ff_regs r;
    Nv2aVshOut o;
    int k;

    for (k = 0; k < 3; k++) {
        reset(&r);
        material(&r);
        setf(&r, 0x0A10, 0); setf(&r, 0x0A14, 0); setf(&r, 0x0A18, 0);
        nv2a_ff_method(&r, 0x03BC, 3);     /* light 0 spot */
        setf(&r, 0x100C, 1); setf(&r, 0x1010, 1); setf(&r, 0x1014, 1);
        setf(&r, 0x1024, 100);
        setf(&r, 0x1064, 10);
        setf(&r, 0x1068, 1);               /* no attenuation */
        setf(&r, 0x1048, 1);               /* falloff N: (s + 0) / 1 */
        setf(&r, 0x1054, -1);              /* spot direction z */
        setf(&r, 0x1058, ws[k]);
        if (!run("spot", &r, &o)) return;
        expect("spot oD0", o.o[NV2A_VSH_O_D0], want[k], want[k], want[k], 0.7f);
    }
}

/* Eye-linear texgen into S, then the texture matrix. */
static void test_texgen(void)
{
    struct nv2a_ff_regs r;
    Nv2aVshOut o;
    int k;

    reset(&r);
    nv2a_ff_method(&r, 0x03C0, 0x2400);    /* stage 0 S eye linear */
    nv2a_ff_method(&r, 0x0420, 1);         /* stage 0 texture matrix */
    row(0x40, 1, 2, 0, 0);                 /* S plane */
    for (k = 0; k < 4; k++)
        row(0x44 + k, 2.0f * (k == 0), 2.0f * (k == 1), 2.0f * (k == 2),
            2.0f * (k == 3));
    vin(0, 1, 1, 0, 1);
    if (!run("texgen", &r, &o)) return;
    expect("texgen oT0", o.o[NV2A_VSH_O_T0], 6, 1, 0, 2);
}

/* Sphere map for an infinite viewer, normal (0.6, 0, 0.8). */
static void test_sphere(void)
{
    struct nv2a_ff_regs r;
    Nv2aVshOut o;

    reset(&r);
    nv2a_ff_method(&r, 0x03C0, 0x2402);
    nv2a_ff_method(&r, 0x03C4, 0x2402);
    nv2a_ff_method(&r, 0x09CC, 1);
    vin(2, 0.6f, 0, 0.8f, 0);
    if (!run("sphere", &r, &o)) return;
    /* r = (-0.96, 0, -0.28), 1 / (2 |r + (0, 0, 1)|) = 1 / 2.4 */
    expect("sphere oT0", o.o[NV2A_VSH_O_T0], 0.1f, 0.5f, 0, 1);
}

static void test_fog(void)
{
    static const uint32_t gen[4] = { 1, 2, 3, 6 };
    static const float want[4] = { 5, 5, 3, 7 };
    struct nv2a_ff_regs r;
    Nv2aVshOut o;
    int k;

    for (k = 0; k < 4; k++) {
        reset(&r);
        r.fog_enable = 1;
        r.fog_gen = gen[k];
        row(0x39, 0, 0, gen[k] == 3 ? -1.0f : 1.0f, gen[k] == 3 ? 0.0f : 2.0f);
        vin(0, gen[k] == 1 ? 3.0f : 0.0f, gen[k] == 1 ? 4.0f : 0.0f,
            gen[k] == 1 ? 0.0f : 3.0f, 1);
        vin(5, 7, 0, 0, 0);
        if (!run("fog", &r, &o)) return;
        expect1("fog oFog.x", o.o[NV2A_VSH_O_FOG][0], want[k]);
    }
}

/* Two-matrix skinning with the implied weight: 0.25 of the identity and
 * 0.75 of a translate by 10 along x. */
static void test_skin(void)
{
    struct nv2a_ff_regs r;
    Nv2aVshOut o;

    reset(&r);
    nv2a_ff_method(&r, 0x0328, 1);         /* SKIN_MODE_2G */
    identity(0x10);
    row(0x10, 1, 0, 0, 10);
    vin(0, 1, 0, 0, 1);
    vin(1, 0.25f, 0, 0, 0);
    if (!run("skin", &r, &o)) return;
    expect("skin oPos", o.o[NV2A_VSH_O_POS], 8.5f + 0.53f, 0.53f, 0, 1);
}

/* Eight local-eye spot lights with specular do not fit 136 slots: the
 * build must say so rather than run off the end. */
static void test_overflow(void)
{
    struct nv2a_ff_regs r;
    int n;

    reset(&r);
    material(&r);
    nv2a_ff_method(&r, 0x03BC, 0xFFFF);
    r.light_ctl = 0x10000;
    r.spec_enable = 1;
    n = nv2a_ff_build(&r, prog, NV2A_VSH_SLOTS, lc);
    if (n != 0) {
        printf("FAIL overflow: build returned %d slots\n", n);
        failures++;
    }
}

/* The fixed point size (SET_POINT_SIZE, pixels * 8, at least 1; a value
 * over 0x1FF is dropped), then xemu's point-params attenuation by the eye
 * distance, which counts w: |(0, 0, 10, 1)| = sqrt(101). */
static void test_points(void)
{
    struct nv2a_ff_regs r;
    Nv2aVshOut o;
    float d, want;

    reset(&r);
    nv2a_ff_method(&r, 0x043C, 24);
    if (!run("point size", &r, &o)) return;
    expect1("point size oPts.x", o.o[NV2A_VSH_O_PTS][0], 3);
    nv2a_ff_method(&r, 0x043C, 0x200);
    if (!run("point size over max", &r, &o)) return;
    expect1("point size over max oPts.x", o.o[NV2A_VSH_O_PTS][0], 3);
    nv2a_ff_method(&r, 0x043C, 4);
    if (!run("point size small", &r, &o)) return;
    expect1("point size small oPts.x", o.o[NV2A_VSH_O_PTS][0], 1);

    nv2a_ff_method(&r, 0x0318, 1);
    setf(&r, 0x0A30, 1);                   /* constant */
    setf(&r, 0x0A34, 0.05f);               /* linear */
    setf(&r, 0x0A38, 0.01f);               /* quadratic */
    setf(&r, 0x0A3C, 10);                  /* scale (p3) */
    setf(&r, 0x0A48, 0.25f);               /* p6 */
    setf(&r, 0x0A4C, 1);                   /* p7: min size and bias */
    vin(0, 0, 0, 10, 1);
    if (!run("point params", &r, &o)) return;
    d = sqrtf(101.0f);
    want = (1.0f / sqrtf(1.0f + 0.05f * d + 0.01f * d * d) + 0.25f) * 10.0f + 1.0f;
    expect1("point params oPts.x", o.o[NV2A_VSH_O_PTS][0], want);

    setf(&r, 0x0A3C, 100);                 /* clamps at min(p3 + 1, 63.875) */
    if (!run("point params clamp", &r, &o)) return;
    expect1("point params clamp oPts.x", o.o[NV2A_VSH_O_PTS][0], 63.875f);
}

/* Two states that differ only in constants share a key; a shape change
 * does not. */
static void test_key(void)
{
    struct nv2a_ff_regs a, b;

    nv2a_ff_init(&a);
    nv2a_ff_init(&b);
    setf(&b, 0x0A10, 0.5f);
    if (nv2a_ff_key(&a) != nv2a_ff_key(&b)) {
        printf("FAIL key: a constant changed the key\n");
        failures++;
    }
    nv2a_ff_method(&b, 0x043C, 24);        /* point size: a constant */
    {
        uint32_t fa[NV2A_FF_KEY_WORDS], fb[NV2A_FF_KEY_WORDS];
        nv2a_ff_key_fields(&a, fa);
        nv2a_ff_key_fields(&b, fb);
        if (memcmp(fa, fb, sizeof fa)) {
            printf("FAIL key: a constant changed the key fields\n");
            failures++;
        }
    }
    nv2a_ff_method(&b, 0x0314, 1);
    if (nv2a_ff_key(&a) == nv2a_ff_key(&b)) {
        printf("FAIL key: lighting did not change the key\n");
        failures++;
    }
    nv2a_ff_init(&b);
    nv2a_ff_method(&b, 0x0318, 1);
    if (nv2a_ff_key(&a) == nv2a_ff_key(&b)) {
        printf("FAIL key: point params did not change the key\n");
        failures++;
    }
    nv2a_ff_init(&b);
    b.light_ctl = 0x20000;
    if (nv2a_ff_key(&a) == nv2a_ff_key(&b)) {
        printf("FAIL key: alpha from material specular did not change the key\n");
        failures++;
    }
}

int main(void)
{
    test_unlit();
    test_infinite();
    test_local();
    test_spot();
    test_texgen();
    test_sphere();
    test_fog();
    test_skin();
    test_points();
    test_overflow();
    test_key();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}
