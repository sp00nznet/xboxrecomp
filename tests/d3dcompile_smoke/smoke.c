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
 *      (d3d8_vsh_nv2a_input_layout) matches each program's signature.
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

static void compile_combiner(void)
{
    static NV2ACombinerState st;
    static char hlsl[65536];
    int i, n;

    memset(&st, 0, sizeof st);
    st.num_stages = 1;
    /* Stage 0: R0 = T0 * V0, in RGB and alpha. */
    st.stages[0].rgb_input[0].reg = NV2A_REG_T0;
    st.stages[0].rgb_input[1].reg = NV2A_REG_V0;
    st.stages[0].alpha_input[0].reg = NV2A_REG_T0;
    st.stages[0].alpha_input[0].alpha_rep = 1;
    st.stages[0].alpha_input[1].reg = NV2A_REG_V0;
    st.stages[0].alpha_input[1].alpha_rep = 1;
    st.stages[0].rgb_output.ab_dst = NV2A_REG_R0;
    st.stages[0].alpha_output.ab_dst = NV2A_REG_R0;
    /* Final: A*B + (1-A)*C + D with A = 1 (zero inverted), B = R0, G = R0.a. */
    st.final_input[0].reg = NV2A_REG_ZERO;
    st.final_input[0].mapping = NV2A_MAP_UNSIGNED_INVERT;
    st.final_input[1].reg = NV2A_REG_R0;
    st.final_input[6].reg = NV2A_REG_R0;
    st.final_input[6].alpha_rep = 1;
    st.tex_mode[0] = NV2A_TEXMODE_2D;
    for (i = 1; i < NV2A_MAX_TEXTURES; i++)
        st.tex_mode[i] = NV2A_TEXMODE_NONE;

    n = d3d8_combiners_generate_hlsl(&st, hlsl, sizeof hlsl);
    say("     combiner T0*V0: %d bytes of HLSL\n", n);
    if (n <= 0) {
        s_fail++;
        return;
    }
    compile("combiner_t0_v0", hlsl, "ps_5_0");

    /* The pushbuffer backend's variant: FOG comes in from the vertex shader
     * and the final combiner blends towards the fog colour by FOG.a. */
    st.fog_input = 1;
    st.final_input[0].reg = NV2A_REG_FOG;
    st.final_input[0].mapping = NV2A_MAP_UNSIGNED_IDENTITY;
    st.final_input[0].alpha_rep = 1;
    st.final_input[2].reg = NV2A_REG_FOG;
    n = d3d8_combiners_generate_hlsl(&st, hlsl, sizeof hlsl);
    say("     combiner fog: %d bytes of HLSL\n", n);
    if (n <= 0) {
        s_fail++;
        return;
    }
    compile("combiner_fog", hlsl, "ps_5_0");
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
