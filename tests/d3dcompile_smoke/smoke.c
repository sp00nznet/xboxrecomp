/* D3DCompile smoke test for the D3D11 pushbuffer backend (nv2a_pb_d3d11.c).
 *
 * The backend compiles HLSL at run time: one vertex shader per NV2A program
 * (d3d8_vsh.c) and one pixel shader per combiner state (d3d8_combiners.c).
 * Under Proton, D3DCompile is Wine's d3dcompiler_47 (vkd3d-shader's HLSL
 * front end), not fxc, and the bytecode it emits then goes through DXVK.
 * That pairing is the backend's main risk, so this checks it on its own:
 *
 *   1. which d3dcompiler_47.dll loads, and from where;
 *   2. a trivial VS/PS pair compiles (vs_5_0 / ps_5_0);
 *   3. a hardware D3D11 device (DXVK) accepts the bytecode;
 *   4. the HLSL d3d8_vsh_generate_hlsl emits compiles, for every program
 *      given on the command line as a RECOMP_VSH_DUMP file;
 *   5. the HLSL d3d8_combiners_generate_hlsl emits for a one-stage
 *      texture * diffuse state compiles;
 *   6. an input layout built from NV2A attribute formats
 *      (d3d8_vsh_nv2a_input_layout) matches each program's signature;
 *   7. the backend's basic pixel shader compiles, stock and with
 *      render.snap_2d's gradient sampling (nv2a_basic_hlsl.h);
 *   8. the texture shader's dot-product and cube-map states
 *      (dot_cases.h) compile, with and without rt_offset;
 *   9. the combiners' W-buffer variant (NV2ACombinerState.w_depth)
 *      compiles to its own bytecode, which keeps SV_Depth and reads the
 *      WDepthCB constants, for each stock state (its non-W twin), and a
 *      vertex program with the zw output compiles; polygon offset, as the
 *      backend translates it (nv2a_depth_bias, nv2a_wdepth_consts_fill),
 *      gives a non-W batch a rasterizer state the device takes and a W
 *      batch a constant buffer it takes.
 *
 * Exit code 0 only if every step passed. Output goes to stdout and, because
 * Proton drops a GUI process's stdio, also to the file named by
 * RECOMP_STDIO_LOG if set. */
#define COBJMACROS
#include <windows.h>
#include "recomp_env.h"
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "d3d8_vsh.h"
#include "d3d8_combiners.h"
#include "nv2a_basic_hlsl.h"
#include "nv2a_backend_common.h"
#include "../d3d8_hlsl_split/dot_cases.h"   /* k_dot_cases, text-checked by d3d8_hlsl_split */

/* The batch info's present offset (nv2a_pb_exec.c): nv2a_backend_common.c's
 * enhance batch filter asks for it, and this test links no executor, so
 * without it an XBOXRECOMP_ENHANCE build (every package) does not link.
 * Never asked here. */
const struct nv2a_pb_present *nv2a_pb_present_state(void)
{
    static struct nv2a_pb_present p;
    return &p;
}

typedef HRESULT (WINAPI *PFN_D3DCOMPILE)(LPCVOID, SIZE_T, LPCSTR,
    const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
    ID3DBlob **, ID3DBlob **);

static FILE *s_log;
static int s_fail;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (s_log) {
        va_start(ap, fmt);
        vfprintf(s_log, fmt, ap);
        va_end(ap);
        fflush(s_log);
    }
}

static PFN_D3DCOMPILE s_compile;
static ID3D11Device *s_dev;

/* Compile one shader; with a device, also create the shader object. */
static int compile_keep(const char *what, const char *src, const char *target,
                        ID3DBlob **keep)
{
    ID3DBlob *code = NULL, *err = NULL;
    LARGE_INTEGER f, t0, t1;
    HRESULT hr;
    double ms;

    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    hr = s_compile(src, strlen(src), what, NULL, NULL, "main", target,
                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    QueryPerformanceCounter(&t1);
    ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    if (FAILED(hr)) {
        say("FAIL compile %-24s %s hr=0x%08lX %.1f ms\n%s\n--- source ---\n%s\n--- end ---\n",
            what, target, (unsigned long)hr, ms,
            err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "(no messages)",
            src);
        if (err) ID3D10Blob_Release(err);
        s_fail++;
        return 0;
    }
    if (err) {
        say("     warnings for %s:\n%s\n", what,
            (const char *)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    }
    say("ok   compile %-24s %s %5zu bytes %.1f ms\n", what, target,
        (size_t)ID3D10Blob_GetBufferSize(code), ms);

    if (s_dev) {
        const void *p = ID3D10Blob_GetBufferPointer(code);
        SIZE_T n = ID3D10Blob_GetBufferSize(code);
        if (target[0] == 'v') {
            ID3D11VertexShader *vs = NULL;
            hr = ID3D11Device_CreateVertexShader(s_dev, p, n, NULL, &vs);
            if (vs) ID3D11VertexShader_Release(vs);
        } else {
            ID3D11PixelShader *ps = NULL;
            hr = ID3D11Device_CreatePixelShader(s_dev, p, n, NULL, &ps);
            if (ps) ID3D11PixelShader_Release(ps);
        }
        if (FAILED(hr)) {
            say("FAIL create  %-24s hr=0x%08lX\n", what, (unsigned long)hr);
            s_fail++;
        } else {
            say("ok   create  %s\n", what);
        }
    }
    if (keep)
        *keep = code;
    else
        ID3D10Blob_Release(code);
    return 1;
}

static int compile(const char *what, const char *src, const char *target)
{
    return compile_keep(what, src, target, NULL);
}

/* An input layout for the program's inputs, from NV2A attribute formats:
 * the shapes the title's streams take (float position, D3DCOLOR, CMP
 * normals, float texcoords), and one stream left disabled. */
static void check_layout(const char *what, const NV2AVshProgram *prog,
                         ID3DBlob *code)
{
    uint32_t type[NV2A_VS_MAX_INPUTS], size[NV2A_VS_MAX_INPUTS];
    D3D11_INPUT_ELEMENT_DESC el[NV2A_VS_MAX_INPUTS];
    ID3D11InputLayout *il = NULL;
    HRESULT hr;
    int i, n, expanded = 0;

    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        int e = 0;
        switch (i) {
        case 0:  type[i] = NV2A_VTX_FLOAT;    size[i] = 3; break;
        case 1:  type[i] = NV2A_VTX_CMP;      size[i] = 1; break;
        case 2:
        case 3:  type[i] = NV2A_VTX_D3DCOLOR; size[i] = 4; break;
        case 4:  type[i] = NV2A_VTX_FLOAT;    size[i] = 2; break;
        default: type[i] = NV2A_VTX_FLOAT;    size[i] = 0; break;
        }
        if (size[i] && (prog->inputs_read & (1u << i))) {
            d3d8_vsh_nv2a_attr_format(type[i], size[i], &e);
            expanded += e;
        }
    }
    n = d3d8_vsh_nv2a_input_layout(prog->inputs_read, type, size, el);
    hr = ID3D11Device_CreateInputLayout(s_dev, el, (UINT)n,
                                        ID3D10Blob_GetBufferPointer(code),
                                        ID3D10Blob_GetBufferSize(code), &il);
    if (FAILED(hr)) {
        say("FAIL layout  %-24s %d elements hr=0x%08lX\n", what, n,
            (unsigned long)hr);
        s_fail++;
    } else {
        say("ok   layout  %-24s %d elements, %d expanded on the CPU\n", what,
            n, expanded);
        ID3D11InputLayout_Release(il);
    }
}

static const char k_vs[] =
    "struct VO { float4 pos : SV_POSITION; float4 col : COLOR0; };\n"
    "VO main(float4 p : ATTR0, float4 c : ATTR3) {\n"
    "    VO o; o.pos = float4(p.xy, 0.5, 1); o.col = c; return o;\n"
    "}\n";

static const char k_ps[] =
    "float4 main(float4 pos : SV_POSITION, float4 col : COLOR0) : SV_TARGET {\n"
    "    return col;\n"
    "}\n";

/* A RECOMP_VSH_DUMP file: start slot, program memory (136 x 4 dwords), then
 * constants and inline attributes, which are not needed here. */
static void compile_dump(const char *path)
{
    static DWORD prog[NV2A_VS_MAX_INSTRUCTIONS][4];
    static NV2AVshProgram parsed;
    static char hlsl[65536];
    uint32_t start = 0;
    const char *name;
    FILE *f = fopen(path, "rb");
    int n;

    if (!f) {
        say("FAIL open %s\n", path);
        s_fail++;
        return;
    }
    if (fread(&start, 4, 1, f) != 1
            || fread(prog, sizeof prog, 1, f) != 1 || start >= 136) {
        say("FAIL read %s\n", path);
        fclose(f);
        s_fail++;
        return;
    }
    fclose(f);
    d3d8_vsh_parse((const uint32_t *)&prog[start][0], NV2A_VS_MAX_INSTRUCTIONS - (int)start,
                   &parsed);
    n = d3d8_vsh_generate_hlsl_ex(&parsed, NV2A_VSH_HLSL_SCREEN_SPACE,
                                  hlsl, sizeof hlsl);
    name = strrchr(path, '\\');
    if (!name) name = strrchr(path, '/');
    name = name ? name + 1 : path;
    say("     %s: start %u, %d insns, inputs 0x%04X, %d bytes of HLSL\n",
        name, start, parsed.length, parsed.inputs_read, n);
    if (n <= 0) {
        s_fail++;
        return;
    }
    if (getenv("SMOKE_PRINT_HLSL"))
        say("--- %s ---\n%s--- end ---\n", name, hlsl);
    {
        ID3DBlob *code = NULL;
        if (compile_keep(name, hlsl, "vs_5_0", &code) && code) {
            if (s_dev)
                check_layout(name, &parsed, code);
            ID3D10Blob_Release(code);
        }
    }
}

/* Stage 0: R0 = T0 * V0, in RGB and alpha; the final combiner outputs R0. */
static void t0_v0_state(NV2ACombinerState *s)
{
    int i;

    memset(s, 0, sizeof *s);
    s->num_stages = 1;
    /* Stage 0: R0 = T0 * V0, in RGB and alpha. */
    s->stages[0].rgb_input[0].reg = NV2A_REG_T0;
    s->stages[0].rgb_input[1].reg = NV2A_REG_V0;
    s->stages[0].alpha_input[0].reg = NV2A_REG_T0;
    s->stages[0].alpha_input[0].alpha_rep = 1;
    s->stages[0].alpha_input[1].reg = NV2A_REG_V0;
    s->stages[0].alpha_input[1].alpha_rep = 1;
    s->stages[0].rgb_output.ab_dst = NV2A_REG_R0;
    s->stages[0].alpha_output.ab_dst = NV2A_REG_R0;
    /* Final: A*B + (1-A)*C + D with A = 1 (zero inverted), B = R0, G = R0.a. */
    s->final_input[0].reg = NV2A_REG_ZERO;
    s->final_input[0].mapping = NV2A_MAP_UNSIGNED_INVERT;
    s->final_input[1].reg = NV2A_REG_R0;
    s->final_input[6].reg = NV2A_REG_R0;
    s->final_input[6].alpha_rep = 1;
    s->tex_mode[0] = NV2A_TEXMODE_2D;
    for (i = 1; i < NV2A_MAX_TEXTURES; i++)
        s->tex_mode[i] = NV2A_TEXMODE_NONE;
}

/* The fog variant the pushbuffer backend draws with: FOG comes in from the
 * vertex shader and the final combiner blends towards the fog colour by
 * FOG.a. */
static void fog_state(NV2ACombinerState *s)
{
    t0_v0_state(s);
    s->fog_input = 1;
    s->final_input[0].reg = NV2A_REG_FOG;
    s->final_input[0].mapping = NV2A_MAP_UNSIGNED_IDENTITY;
    s->final_input[0].alpha_rep = 1;
    s->final_input[2].reg = NV2A_REG_FOG;
}

static void compile_combiner(void)
{
    static NV2ACombinerState st;
    static char hlsl[65536];
    int n;

    t0_v0_state(&st);
    n = d3d8_combiners_generate_hlsl(&st, hlsl, sizeof hlsl);
    say("     combiner T0*V0: %d bytes of HLSL\n", n);
    if (n <= 0) {
        s_fail++;
        return;
    }
    compile("combiner_t0_v0", hlsl, "ps_5_0");

    fog_state(&st);
    n = d3d8_combiners_generate_hlsl(&st, hlsl, sizeof hlsl);
    say("     combiner fog: %d bytes of HLSL\n", n);
    if (n <= 0) {
        s_fail++;
        return;
    }
    compile("combiner_fog", hlsl, "ps_5_0");
}

static void compile_dot(void)
{
    static NV2ACombinerState st;
    static char hlsl[256 * 1024];
    uint32_t seen[DOT_NCASE * 2], bins[DOT_NCASE * 2], base[DOT_NCASE];
    char what[64];
    int i, off, n, nseen = 0, nbins = 0, nbase = 0, distinct = 0, base_distinct = 0;
    int nt0 = 0, want;

    for (i = 0; i < DOT_NCASE; i++)
        for (off = 0; off < 2; off++) {
            dot_case_state(i, off, &st);
            snprintf(what, sizeof what, "%s%s", k_dot_cases[i].name, off ? "-rt-offset" : "");
            n = d3d8_combiners_generate_hlsl(&st, hlsl, (int)sizeof hlsl);
            if (n <= 0) {
                say("FAIL %s: no HLSL\n", what);
                s_fail++;
                continue;
            }
            if (dot_text_seen(seen, nseen, dot_text_hash(hlsl))) {
                say("FAIL %s: the same HLSL as an earlier case\n", what);
                s_fail++;
            }
            seen[nseen++] = dot_text_hash(hlsl);
            {
                ID3DBlob *code = NULL;
                const uint8_t *p;
                uint32_t h = 2166136261u;
                SIZE_T k, len;

                if (!compile_keep(what, hlsl, "ps_5_0", &code))
                    continue;
                p = (const uint8_t *)ID3D10Blob_GetBufferPointer(code);
                len = ID3D10Blob_GetBufferSize(code);
                for (k = 0; k < len; k++)
                    h = (h ^ p[k]) * 16777619u;
                ID3D10Blob_Release(code);
                distinct += !dot_text_seen(bins, nbins, h);
                bins[nbins++] = h;
                if (!off) {
                    if (!dot_text_seen(base, nbase, h))
                        base_distinct++;
                    else if (dot_case_t0_only(i))
                        say("     %s: the bytecode of an earlier t0-only case\n", what);
                    else
                        say("FAIL %s: the same bytecode as an earlier case\n", what);
                    base[nbase++] = h;
                }
            }
        }
    /* D3DCompile strips a texture shader the combiners never read, and every
     * case then compiles to one constant shader: each case must keep its own
     * code. Cases that sample t0 alone may share one (dot_case_t0_only), and
     * an rt_offset variant may match its base case. */
    for (i = 0; i < DOT_NCASE; i++)
        nt0 += dot_case_t0_only(i);
    want = DOT_NCASE - (nt0 > 1 ? nt0 - 1 : 0);
    say("     dot cases: %d of %d compiled to distinct bytecode, %d of %d base cases"
        " (want %d)\n", distinct, nbins, base_distinct, nbase, want);
    if (base_distinct < want) {
        say("FAIL dot cases: %d base cases compiled to distinct bytecode, want %d\n",
            base_distinct, want);
        s_fail++;
    }
}

/* FNV-1a over a blob's bytecode. */
static uint32_t blob_hash(ID3DBlob *code)
{
    const uint8_t *p = (const uint8_t *)ID3D10Blob_GetBufferPointer(code);
    SIZE_T k, len = ID3D10Blob_GetBufferSize(code);
    uint32_t h = 2166136261u;

    for (k = 0; k < len; k++)
        h = (h ^ p[k]) * 16777619u;
    return h;
}

static int blob_has(ID3DBlob *code, const char *s)
{
    const char *p = (const char *)ID3D10Blob_GetBufferPointer(code);
    SIZE_T k, len = ID3D10Blob_GetBufferSize(code), n = strlen(s);

    for (k = 0; k + n <= len; k++)
        if (!memcmp(p + k, s, n))
            return 1;
    return 0;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A signature element in the DXBC container's ISGN or OSGN chunk: 1 if
 * (name, index) is there, with *used its read mask (for an input, the
 * components the shader reads). The container: "DXBC", a 16-byte hash,
 * version, size, the chunk count and their offsets; a chunk: its fourcc,
 * size, then the element count, 8, and 24-byte elements (name offset from
 * the chunk data, index, system value, component type, register, mask,
 * read/write mask). */
static int sig_has(ID3DBlob *code, const char *chunk, const char *name,
                   uint32_t index, unsigned *used)
{
    const uint8_t *b = (const uint8_t *)ID3D10Blob_GetBufferPointer(code);
    uint32_t len = (uint32_t)ID3D10Blob_GetBufferSize(code), nchunk, c, e;

    if (len < 32 || memcmp(b, "DXBC", 4))
        return 0;
    nchunk = rd32(b + 28);
    for (c = 0; c < nchunk && 32 + 4 * c + 4 <= len; c++) {
        uint32_t off = rd32(b + 32 + 4 * c), size, count;
        const uint8_t *d;

        if (off + 8 > len || memcmp(b + off, chunk, 4))
            continue;
        size = rd32(b + off + 4);
        d = b + off + 8;
        if (off + 8 + size > len || size < 8)
            return 0;
        count = rd32(d);
        for (e = 0; e < count && 8 + 24 * e + 24 <= size; e++) {
            const uint8_t *el = d + 8 + 24 * e;
            uint32_t no = rd32(el);

            if (no < size && rd32(el + 4) == index
                    && !strncmp((const char *)d + no, name, size - no)) {
                if (used)
                    *used = el[21];
                return 1;
            }
        }
    }
    return 0;
}

/* One stock state and its W variant: the variant must compile to its own
 * bytecode, write SV_Depth, read zw (TEXCOORD6) and keep WDepthCB. Were the
 * depth dead-stripped, the two would compile alike. */
static void wdepth_twin(const char *name, NV2ACombinerState *st)
{
    static char hlsl[256 * 1024];
    ID3DBlob *stock = NULL, *w = NULL;
    char what[64];
    unsigned used = 0;
    int ok = 1;

    st->w_depth = 0;
    snprintf(what, sizeof what, "%s", name);
    if (d3d8_combiners_generate_hlsl(st, hlsl, (int)sizeof hlsl) <= 0
            || !compile_keep(what, hlsl, "ps_5_0", &stock)) {
        say("FAIL %s: the stock twin did not build\n", what);
        s_fail++;
        return;
    }
    st->w_depth = 1;
    snprintf(what, sizeof what, "%s-w", name);
    if (d3d8_combiners_generate_hlsl(st, hlsl, (int)sizeof hlsl) <= 0
            || !compile_keep(what, hlsl, "ps_5_0", &w)) {
        say("FAIL %s: no W variant\n", what);
        s_fail++;
        st->w_depth = 0;
        ID3D10Blob_Release(stock);
        return;
    }
    st->w_depth = 0;
    if (blob_hash(w) == blob_hash(stock)) {
        say("FAIL %s: the same bytecode as its stock twin\n", what);
        ok = 0;
    }
    if (!sig_has(w, "OSGN", "SV_Depth", 0, NULL)) {
        say("FAIL %s: no SV_Depth output\n", what);
        ok = 0;
    }
    if (sig_has(stock, "OSGN", "SV_Depth", 0, NULL)) {
        say("FAIL %s: the stock twin writes SV_Depth\n", name);
        ok = 0;
    }
    if (!sig_has(w, "ISGN", "TEXCOORD", 6, &used) || !used) {
        say("FAIL %s: zw (TEXCOORD6) is not read\n", what);
        ok = 0;
    }
    if (!blob_has(w, "WDepthCB")) {
        say("FAIL %s: WDepthCB was stripped\n", what);
        ok = 0;
    }
    if (ok)
        say("ok   wdepth  %-24s SV_Depth, zw read 0x%X, %zu vs %zu bytes\n", what, used,
            (size_t)ID3D10Blob_GetBufferSize(w), (size_t)ID3D10Blob_GetBufferSize(stock));
    else
        s_fail++;
    ID3D10Blob_Release(w);
    ID3D10Blob_Release(stock);
}

/* Quantum Redshift's 3D depth state: W-buffered, clip range 4 to
 * 2^24 - 1, cull outside it, and its decals' polygon offset. */
static void qr_depth_state(struct nv2a_pb_vsh *v, int w)
{
    static const uint32_t clip_min = 0x407FFFFBu, clip_max = 0x4B7FFFFFu;

    memset(v, 0, sizeof *v);
    v->control0 = w ? 0x110001u : 0x100001u;
    v->depth_enable = 1;
    memcpy(&v->clip_min, &clip_min, 4);
    memcpy(&v->clip_max, &clip_max, 4);
    v->clip_set = 1;
    v->zminmax_ctl = 1u;
    v->poly_offset = 1;
    v->poly_factor = -0.25f;
    v->poly_bias = -1.0f;
}

static void compile_wdepth(void)
{
    static NV2ACombinerState st;
    static NV2AVshProgram parsed;
    static struct nv2a_pb_vsh v;
    static char hlsl[65536];
    /* MOV oPos, v0 (as d3d8_hlsl_split's wdepth_check). */
    static const uint32_t prog[4] = {0, 0x0020001Bu, 0x0836106Cu, 0x2070F800u | 1u};
    struct nv2a_wdepth_consts wc;
    float zb = 0.0f, zs = 0.0f;

    t0_v0_state(&st);
    wdepth_twin("combiner_t0_v0", &st);
    fog_state(&st);
    wdepth_twin("combiner_fog", &st);
    dot_case_state(0, 0, &st);
    wdepth_twin(k_dot_cases[0].name, &st);

    /* The vertex side: every program outputs the clip w as oZW. */
    d3d8_vsh_parse(prog, 1, &parsed);
    if (d3d8_vsh_generate_hlsl_ex(&parsed, NV2A_VSH_HLSL_SCREEN_SPACE, hlsl,
                                  (int)sizeof hlsl) <= 0 || !strstr(hlsl, "oZW")) {
        say("FAIL vsh_zw: no oZW in the vertex HLSL\n");
        s_fail++;
    } else {
        ID3DBlob *code = NULL;
        if (compile_keep("vsh_zw", hlsl, "vs_5_0", &code) && code) {
            if (!sig_has(code, "OSGN", "TEXCOORD", 6, NULL)) {
                say("FAIL vsh_zw: no TEXCOORD6 output\n");
                s_fail++;
            }
            if (s_dev)
                check_layout("vsh_zw", &parsed, code);
            ID3D10Blob_Release(code);
        }
    }

    /* Polygon offset off the W path: a hardware bias in the rasterizer
     * state, as nv2a_pb_d3d11.c's rs_biased builds it. */
    qr_depth_state(&v, 0);
    if (nv2a_w_depth_on(&v, 1, 1)) {
        say("FAIL poly offset: a non-W state takes the W path\n");
        s_fail++;
    }
    nv2a_depth_bias(&v, 5u, 0x128u, 2, &zb, &zs);
    if (zb == 0.0f || zs == 0.0f) {
        say("FAIL poly offset: no host bias (%g, %g)\n", zb, zs);
        s_fail++;
    } else if (s_dev) {
        D3D11_RASTERIZER_DESC rd;
        ID3D11RasterizerState *rs = NULL;
        HRESULT hr;

        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_BACK;
        rd.DepthClipEnable = TRUE;
        rd.ScissorEnable = TRUE;
        rd.DepthBias = (INT)(zb < 0.0f ? zb - 0.5f : zb + 0.5f);
        rd.SlopeScaledDepthBias = zs;
        hr = ID3D11Device_CreateRasterizerState(s_dev, &rd, &rs);
        if (FAILED(hr)) {
            say("FAIL poly offset: rasterizer state hr=0x%08lX\n", (unsigned long)hr);
            s_fail++;
        } else {
            say("ok   create  poly_offset_rs           DepthBias %d, slope %g\n",
                rd.DepthBias, zs);
            ID3D11RasterizerState_Release(rs);
        }
    }

    /* On the W path the offset goes to the shader's WDepthCB instead. */
    qr_depth_state(&v, 1);
    if (!nv2a_w_depth_on(&v, 1, 1)) {
        say("FAIL poly offset: the W state does not take the W path\n");
        s_fail++;
    }
    nv2a_wdepth_consts_fill(&v, 5u, 0x128u, 2, &wc);
    if (wc.w0[0] == 0.0f || wc.w0[1] == 0.0f || wc.w1[1] != 1.0f) {
        say("FAIL poly offset: W constants (%g, %g, cull %g)\n", wc.w0[0], wc.w0[1],
            wc.w1[1]);
        s_fail++;
    } else if (s_dev) {
        D3D11_BUFFER_DESC bd;
        D3D11_SUBRESOURCE_DATA init;
        ID3D11Buffer *cb = NULL;
        HRESULT hr;

        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof wc;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        init.pSysMem = &wc;
        init.SysMemPitch = init.SysMemSlicePitch = 0;
        hr = ID3D11Device_CreateBuffer(s_dev, &bd, &init, &cb);
        if (FAILED(hr)) {
            say("FAIL poly offset: WDepthCB buffer hr=0x%08lX\n", (unsigned long)hr);
            s_fail++;
        } else {
            say("ok   create  wdepth_cb                bias %g, slope %g, clip %g..%g\n",
                wc.w0[0], wc.w0[1], wc.w0[2], wc.w0[3]);
            ID3D11Buffer_Release(cb);
        }
    }
}

int main(int argc, char **argv)
{
    const char *logpath = recomp_env(RENV_STDIO_LOG);
    HMODULE mod;
    char path[MAX_PATH];
    int i;

    if (logpath)
        s_log = fopen(logpath, "a");

    say("[d3dcompile-smoke] start\n");
    mod = LoadLibraryA("d3dcompiler_47.dll");
    if (!mod) {
        say("FAIL LoadLibrary d3dcompiler_47.dll: error %lu\n",
            (unsigned long)GetLastError());
        return 2;
    }
    GetModuleFileNameA(mod, path, sizeof path);
    say("     d3dcompiler_47.dll: %s\n", path);
    {
        /* Wine's builtins carry this export; native Microsoft DLLs do not. */
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        const char *(CDECL *wine_get_version)(void) = ntdll
            ? (const char *(CDECL *)(void))(void *)GetProcAddress(ntdll, "wine_get_version")
            : NULL;
        say("     wine: %s\n", wine_get_version ? wine_get_version() : "(not wine)");
    }
    s_compile = (PFN_D3DCOMPILE)(void *)GetProcAddress(mod, "D3DCompile");
    if (!s_compile) {
        say("FAIL no D3DCompile export\n");
        return 2;
    }

    {
        D3D_FEATURE_LEVEL fl = 0;
        HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                       NULL, 0, D3D11_SDK_VERSION, &s_dev, &fl,
                                       NULL);
        if (FAILED(hr)) {
            say("FAIL D3D11CreateDevice(HARDWARE) hr=0x%08lX; compiling only\n",
                (unsigned long)hr);
            s_dev = NULL;
            s_fail++;
        } else {
            IDXGIDevice *dxgi = NULL;
            IDXGIAdapter *ad = NULL;
            DXGI_ADAPTER_DESC desc;
            memset(&desc, 0, sizeof desc);
            if (SUCCEEDED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice,
                                                      (void **)&dxgi))) {
                if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &ad))) {
                    IDXGIAdapter_GetDesc(ad, &desc);
                    IDXGIAdapter_Release(ad);
                }
                IDXGIDevice_Release(dxgi);
            }
            say("     device: feature level 0x%X, adapter %ls\n", (unsigned)fl,
                desc.Description);
        }
    }

    compile("trivial_vs", k_vs, "vs_5_0");
    compile("trivial_ps", k_ps, "ps_5_0");
    compile_combiner();
    compile_dot();
    compile_wdepth();
    compile("nv2a_ps_diffuse", NV2A_HLSL_PS_BASIC, "ps_5_0");
    compile("nv2a_ps_diffuse_snap", NV2A_HLSL_PS_BASIC_SNAP, "ps_5_0");
    for (i = 1; i < argc; i++)
        compile_dump(argv[i]);

    say("[d3dcompile-smoke] %s (%d failure%s)\n", s_fail ? "FAILED" : "PASSED",
        s_fail, s_fail == 1 ? "" : "s");
    if (s_dev)
        ID3D11Device_Release(s_dev);
    if (s_log)
        fclose(s_log);
    return s_fail ? 1 : 0;
}
