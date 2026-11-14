/* nv2a_vsh_emit_hlsl checked against nv2a_vsh_run.
 *
 * Each program is loaded into the interpreter, run on the CPU vertex by
 * vertex, then emitted as HLSL, compiled for vs_5_0 and run on D3D11 through
 * stream output; all 32 output floats of every vertex are compared.
 *
 *  - no program: nothing is emitted, as nv2a_vsh_run refuses it too;
 *  - the Xbox D3D screen-space epilogue (see tests/nv2a_vsh): position within
 *    the GPU's division precision, and only v0 read;
 *  - random programs of MAC ops only (every op, swizzle, negate, write mask,
 *    temps 13..15, a0-relative and out-of-range c[] reads): bit for bit;
 *  - each ILU op alone on varied inputs: within the GPU's precision
 *    (4e-6 relative; LIT's power 3e-5, see test_ilu_ops).
 *
 * Zero and denormal compare equal: D3D11 flushes float32 denormals.
 * WARP by default; `--hardware` uses the default hardware adapter. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_vsh_interp.h"

#define MAXV 1024
static float s_in[MAXV][NV2A_VSH_INPUTS][4];
static float s_cpu[MAXV][32], s_gpu[MAXV][32];
static uint32_t s_prog[NV2A_VSH_SLOTS][4];
static float s_c[NV2A_VSH_CONSTANTS][4];
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static int failures;

static uint32_t s_rng = 20261114u;
static uint32_t rnd32(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
static uint32_t R(uint32_t n) { return rnd32() % n; }
static float rndf(float lo, float hi) { return lo + (hi - lo) * (float)(rnd32() >> 8) / 16777216.0f; }

static void check(int ok, const char *what)
{
    if (!ok) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static void load(uint32_t start)
{
    uint32_t i;

    for (i = 0; i < NV2A_VSH_SLOTS; i++)
        nv2a_vsh_set_instruction(i, s_prog[i]);
    nv2a_vsh_set_start_slot(start);
    for (i = 0; i < NV2A_VSH_CONSTANTS; i++)
        nv2a_vsh_set_constant(i, s_c[i]);
    nv2a_vsh_set_cxt_write(0);
}

static int run_cpu(int n)
{
    int v;

    for (v = 0; v < n; v++) {
        Nv2aVshOutput o;
        if (!nv2a_vsh_run((const float (*)[4])s_in[v], &o))
            return 0;
        memcpy(s_cpu[v], &o, sizeof o);
    }
    return 1;
}

static const char k_main[] =
    "struct SOut { float4 pos : OPOS; float4 d0 : OD0; float4 d1 : OD1; float4 fog : OFOG;"
    " float4 t0 : OT0; float4 t1 : OT1; float4 t2 : OT2; float4 t3 : OT3; };\n"
    "SOut main(Nv2aVshIn I) { Nv2aVshOut r = nv2a_vsh(I); SOut o; o.pos = r.pos; o.d0 = r.d0;"
    " o.d1 = r.d1; o.fog = r.fog; o.t0 = r.t0; o.t1 = r.t1; o.t2 = r.t2; o.t3 = r.t3; return o; }\n";

static void die(const char *what, HRESULT h)
{
    printf("FAIL %s 0x%08lX\n", what, (unsigned long)h);
    exit(2);
}
#define CK(x) do { HRESULT h_ = (x); if (FAILED(h_)) die(#x, h_); } while (0)

/* The loaded program on the GPU for n vertices of s_in, into s_gpu. */
static int run_gpu(int n)
{
    static char src[1 << 18];
    static const D3D11_SO_DECLARATION_ENTRY so[8] = {
        {0, "OPOS", 0, 0, 4, 0}, {0, "OD", 0, 0, 4, 0}, {0, "OD", 1, 0, 4, 0}, {0, "OFOG", 0, 0, 4, 0},
        {0, "OT", 0, 0, 4, 0}, {0, "OT", 1, 0, 4, 0}, {0, "OT", 2, 0, 4, 0}, {0, "OT", 3, 0, 4, 0}};
    D3D11_INPUT_ELEMENT_DESC ie[NV2A_VSH_INPUTS];
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA sd;
    D3D11_QUERY_DESC qd = {D3D11_QUERY_SO_STATISTICS, 0};
    D3D11_QUERY_DATA_SO_STATISTICS st;
    D3D11_MAPPED_SUBRESOURCE map;
    ID3DBlob *code = NULL, *err = NULL;
    ID3D11VertexShader *vs; ID3D11GeometryShader *gs; ID3D11InputLayout *il;
    ID3D11Buffer *vb, *cb, *sob, *stg, *none = NULL;
    ID3D11Query *q;
    UINT stride = 128, vstride = sizeof s_in[0], zero = 0, a;
    size_t len = nv2a_vsh_emit_hlsl(src, sizeof src - sizeof k_main);
    HRESULT h;

    if (!len)
        return 0;
    memcpy(src + len, k_main, sizeof k_main);
    len += sizeof k_main - 1;
    h = D3DCompile(src, len, "nv2a_vsh", NULL, NULL, "main", "vs_5_0",
                   D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(h)) {
        printf("FAIL compile: %s\n%s", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?", src);
        exit(2);
    }
    CK(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &vs));
    CK(ID3D11Device_CreateGeometryShaderWithStreamOutput(dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
                                                         so, 8, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, NULL, &gs));
    for (a = 0; a < NV2A_VSH_INPUTS; a++) {
        D3D11_INPUT_ELEMENT_DESC e = {"ATTR", a, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, a * 16, D3D11_INPUT_PER_VERTEX_DATA, 0};
        ie[a] = e;
    }
    CK(ID3D11Device_CreateInputLayout(dev, ie, NV2A_VSH_INPUTS, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), &il));
    memset(&bd, 0, sizeof bd); memset(&sd, 0, sizeof sd);
    bd.ByteWidth = (UINT)n * vstride; bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    sd.pSysMem = s_in; CK(ID3D11Device_CreateBuffer(dev, &bd, &sd, &vb));
    bd.ByteWidth = sizeof s_c; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; sd.pSysMem = s_c;
    CK(ID3D11Device_CreateBuffer(dev, &bd, &sd, &cb));
    bd.ByteWidth = (UINT)n * stride; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    CK(ID3D11Device_CreateBuffer(dev, &bd, NULL, &sob));
    bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CK(ID3D11Device_CreateBuffer(dev, &bd, NULL, &stg));
    CK(ID3D11Device_CreateQuery(dev, &qd, &q));
    ID3D11DeviceContext_IASetInputLayout(ctx, il);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &vstride, &zero);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 1, 1, &cb);       /* register(b1) */
    ID3D11DeviceContext_GSSetShader(ctx, gs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_SOSetTargets(ctx, 1, &sob, &zero);
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)q);
    ID3D11DeviceContext_Draw(ctx, (UINT)n, 0);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q);
    ID3D11DeviceContext_SOSetTargets(ctx, 1, &none, &zero);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stg, (ID3D11Resource *)sob);
    while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q, &st, sizeof st, 0) == S_FALSE)
        ;
    if (st.NumPrimitivesWritten != (UINT64)n)
        die("stream output vertex count", E_FAIL);
    CK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &map));
    memcpy(s_gpu, map.pData, (size_t)n * stride);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg, 0);
    ID3D11Query_Release(q); ID3D11Buffer_Release(stg); ID3D11Buffer_Release(sob); ID3D11Buffer_Release(cb);
    ID3D11Buffer_Release(vb); ID3D11InputLayout_Release(il); ID3D11GeometryShader_Release(gs);
    ID3D11VertexShader_Release(vs); ID3D10Blob_Release(code);
    if (err)
        ID3D10Blob_Release(err);
    return 1;
}

static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static int same(float a, float b)
{
    uint32_t ua = bits(a), ub = bits(b);
    return ua == ub || (isnan(a) && isnan(b)) || (!(ua & 0x7f800000u) && !(ub & 0x7f800000u));
}

/* Floats differing from the CPU: bit for bit (tol 0), or beyond
 * tol * max(1, |a|, |b|); a finite value against an infinity or NaN always counts. */
static int differing(int n, double tol, double *worst)
{
    int v, k, bad = 0;

    *worst = 0;
    for (v = 0; v < n; v++)
        for (k = 0; k < 32; k++) {
            float a = s_cpu[v][k], b = s_gpu[v][k];
            double e;
            if (same(a, b))
                continue;
            if (!isfinite(a) || !isfinite(b) || tol == 0) {
                bad++;
                continue;
            }
            e = fabs((double)a - b) / fmax(1.0, fmax(fabs((double)a), fabs((double)b)));
            if (e > *worst)
                *worst = e;
            if (e > tol)
                bad++;
        }
    return bad;
}

enum { MUX_TEMP = 1, MUX_INPUT = 2, MUX_CONST = 3 };    /* source mux */

static void put_src(uint32_t w[4], int which, uint32_t mux, uint32_t reg, uint32_t neg, uint32_t swz)
{
    if (which == 0) { w[1] |= neg << 8 | swz; w[2] |= reg << 28 | mux << 26; }
    else if (which == 1) { w[2] |= neg << 25 | swz << 17 | reg << 13 | mux << 11; }
    else { w[2] |= neg << 10 | swz << 2 | (reg >> 2); w[3] |= (reg & 3) << 30 | mux << 28; }
}

static void test_no_program(void)
{
    char buf[256];

    memset(s_prog, 0, sizeof s_prog);
    load(0);
    check(nv2a_vsh_emit_hlsl(buf, sizeof buf) == 0, "no program: nothing emitted");
    check(nv2a_vsh_inputs_read() == 0, "no program: no inputs");
}

/* mov o[0], v0 ; mul o[0].xyz, R12, c[58] + rcc R1.x, R12.w ; mad o[0].xyz, R12, R1.x, c[59] */
static void test_epilogue(void)
{
    static const float c58[4] = {320, -240, 1000, 0}, c59[4] = {320, 240, 0, 0};
    double worst;
    int v, bad;

    memset(s_prog, 0, sizeof s_prog); memset(s_c, 0, sizeof s_c);
    memcpy(s_c[58], c58, sizeof c58); memcpy(s_c[59], c59, sizeof c59);
    s_prog[0][1] = 1u << 21;
    put_src(s_prog[0], 0, MUX_INPUT, 0, 0, 0x1B);
    s_prog[0][3] |= 0xFu << 12 | 1u << 11;
    s_prog[1][1] = 3u << 25 | 2u << 21 | 58u << 13;
    put_src(s_prog[1], 0, MUX_TEMP, 12, 0, 0x1B); put_src(s_prog[1], 1, MUX_CONST, 0, 0, 0x1B);
    put_src(s_prog[1], 2, MUX_TEMP, 12, 0, 0xFF);
    s_prog[1][3] |= 0xEu << 12 | 1u << 11 | 7u << 20 | 0x8u << 16;   /* temp field R7; the rcc must land in R1 */
    s_prog[2][1] = 4u << 21 | 59u << 13;
    put_src(s_prog[2], 0, MUX_TEMP, 12, 0, 0x1B); put_src(s_prog[2], 1, MUX_TEMP, 1, 0, 0x00);
    put_src(s_prog[2], 2, MUX_CONST, 0, 0, 0x1B);
    s_prog[2][3] |= 0xEu << 12 | 1u << 11 | 1u;
    for (v = 0; v < 256; v++) {
        memset(s_in[v], 0, sizeof s_in[v]);
        s_in[v][0][0] = rndf(-50, 50); s_in[v][0][1] = rndf(-50, 50);
        s_in[v][0][2] = rndf(0, 50); s_in[v][0][3] = rndf(0.5f, 50);
    }
    load(0);
    check(nv2a_vsh_inputs_read() == 1u, "epilogue: reads v0 only");
    check(run_cpu(256) && run_gpu(256), "epilogue: runs");
    bad = differing(256, 1e-6, &worst);
    printf("epilogue: %d of %d floats beyond 1e-6 (worst %.3g)\n", bad, 256 * 32, worst);
    check(bad == 0, "epilogue: position within 1e-6");
}

static void random_program(int ilu_op)
{
    static const uint32_t oaddrs[] = {0, 0, 3, 4, 5, 9, 10, 11, 12, 1, 2, 7, 8, 13, 200};
    uint32_t len = ilu_op ? 1 : 1 + R(32), start = ilu_op ? 0 : R(4), i;
    int v, k;

    memset(s_prog, 0, sizeof s_prog);
    for (i = 0; i < len; i++) {
        uint32_t *w = s_prog[start + i];
        uint32_t mac = ilu_op ? 0 : (R(10) ? 1 + R(12) : 13), ilu = ilu_op ? (uint32_t)ilu_op : (R(4) == 0);
        uint32_t s;

        w[1] |= ilu << 25 | mac << 21 | R(R(8) ? 192 : 256) << 13 | R(16) << 9;
        if (ilu_op) {
            put_src(w, 2, MUX_INPUT, 0, 0, 0x1B);
            w[1] &= ~(0xFu << 9);                                     /* C = v0.xyzw */
            w[3] |= 0xFu << 12 | 1u << 11 | 9u << 3 | 1u << 2;          /* o[9] = ILU */
        } else {
            for (s = 0; s < 3; s++)
                put_src(w, (int)s, R(8) ? 1 + R(3) : 0, R(R(8) ? 13 : 16), R(2), R(4) ? R(256) : 0x1B);
            w[3] |= (1 + R(15)) << 24 | R(R(8) ? 13 : 16) << 20 | (1 + R(15)) << 16
                  | (R(5) ? 1 + R(15) : 0) << 12 | (R(6) ? 1u : 0u) << 11
                  | (oaddrs[R(sizeof oaddrs / sizeof oaddrs[0])] & 0xFF) << 3 | (uint32_t)(R(5) == 0) << 1;
        }
        if (i + 1 == len)
            w[3] |= 1u;
    }
    for (i = 0; i < NV2A_VSH_CONSTANTS; i++)
        for (k = 0; k < 4; k++)
            s_c[i][k] = rndf(-4, 4);
    for (v = 0; v < MAXV; v++)
        for (i = 0; i < NV2A_VSH_INPUTS; i++)
            for (k = 0; k < 4; k++) {
                int r = (v + k) % 4;
                s_in[v][i][k] = !ilu_op ? (R(4) ? rndf(-4, 4) : rndf(-1000, 1000))
                              : r == 0 ? rndf(-4, 4) : r == 1 ? rndf(-1000, 1000)
                              : r == 2 ? rndf(-1, 1) * 1e-6f : rndf(0, 2);
            }
    load(start);
}

static void test_random_mac(void)
{
    int p, bad_programs = 0;
    double worst;

    for (p = 0; p < 200; p++) {
        random_program(0);
        if (!run_cpu(256) || !run_gpu(256)) {
            bad_programs++;
            continue;
        }
        if (differing(256, 0, &worst))
            bad_programs++;
    }
    printf("random MAC programs: %d of 200 differ\n", bad_programs);
    check(bad_programs == 0, "random MAC programs: bit for bit");
}

static void test_ilu_ops(void)
{
    static const char *names[8] = {"", "", "rcp", "rcc", "rsq", "exp", "log", "lit"};
    int op;

    for (op = 2; op <= 7; op++) {
        double worst;
        int bad;
        char what[64];
        /* LIT's power is exp2(p * log2(x)) on the GPU, so its error grows
         * with the exponent (up to 128 here): a wider bound for that op. */
        const double tol = op == 7 ? 3e-5 : 4e-6;

        random_program(op);
        check(run_cpu(MAXV) && run_gpu(MAXV), "ILU op runs");
        bad = differing(MAXV, tol, &worst);
        printf("ILU %s: %d of %d floats beyond %.0e (worst %.3g)\n", names[op], bad, MAXV * 32, tol, worst);
        snprintf(what, sizeof what, "ILU %s within %.0e", names[op], tol);
        check(bad == 0, what);
    }
}

int main(int argc, char **argv)
{
    D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0, got;
    int hardware = argc > 1 && !strcmp(argv[1], "--hardware");

    CK(D3D11CreateDevice(NULL, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, NULL, 0,
                         &want, 1, D3D11_SDK_VERSION, &dev, &got, &ctx));
    printf("device: %s\n", hardware ? "hardware" : "WARP");
    test_no_program();
    test_epilogue();
    test_random_mac();
    test_ilu_ops();
    printf("%s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
