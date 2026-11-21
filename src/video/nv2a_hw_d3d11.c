/**
 * NV2A hardware renderer: the pushbuffer executor's draws on a D3D11 device.
 *
 * xbox_Nv2aHwRendererInstall (RECOMP_HW_RENDER=1) registers this as the
 * executor's render back end (nv2a_backend.h) and asks for vertex-program
 * batches before transform, so a title's vertex programs run as vertex
 * shaders (nv2a_vsh_emit_hlsl) instead of on the CPU, and the GPU
 * rasterises. Fixed-function and pre-transformed batches arrive as surface
 * pixels, as for any back end.
 *
 * Surfaces. A colour surface lives in a D3D11 texture keyed by its guest
 * address and size, and stays there from frame to frame. Each present_sync
 * queues a GPU copy of what the frame drew and writes the previous frame's
 * copy (finished by then) to guest memory, so the window, frame dumps and
 * guest code see what the GPU drew one frame late, and nothing waits for the
 * GPU. A surface is loaded from guest memory only when the guest has written
 * it since the last write-back (a hash of its rows says so) or on first use. Depth lives only on
 * the GPU; the CPU executor keeps it outside guest memory as well. A texture
 * whose address is a surface drawn this frame is sampled from the GPU copy.
 *
 * What the back-end contract carries is what is drawn: the register
 * combiners over the four texture stages and fog when the title has
 * programmed them (one generated pixel shader per combiner program, the
 * executor's nv2a_rc_eval written out as HLSL), else texture stage 0
 * modulated by the diffuse colour; alpha test, blend, depth, stencil, cull
 * and colour mask. Window z is compared as it is (one scale, 2^24 - 1, for
 * Z16 and Z24 alike). Positions follow the executor's own path: surface pixels
 * to clip space with w kept, colours interpolated linearly in screen space,
 * texture coordinates with perspective; program batches are clipped at the
 * executor's planes (w = 0.001 and the near plane) with SV_ClipDistance.
 *
 * A swizzled surface (render-to-texture) is drawn row-major on the GPU and
 * kept in Morton order in guest memory, as the CPU rasteriser keeps it.
 *
 * ponytail: the bump-environment and dot-product stage modes read zero (as on
 * the CPU path), cube maps read white, partial clears are not drawn yet; mip chains are generated from level 0 (the executor does not pass
 * the first ones to a back end, nv2a_backend.h); the frame's surfaces make a
 * round trip through guest memory.
 */
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/nv2a_backend.h"
#include "d3d/d3d8_swizzle.h"
#include "kernel/nv2a_vsh_interp.h"
#include "nv2a_hw_d3d11.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define HW_SURFACES 16
#define HW_DEPTHS   8
#define HW_TEXTURES 128
#define HW_PROGRAMS 256
#define HW_STATES   64
#define HW_PIXEL_SHADERS 256

typedef struct {
    uint32_t va, width, height, pitch, bpp;
    uint32_t swz, sw, sh;               /* swizzled: Morton order in a sw x sh texture */
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    ID3D11Texture2D *staging[2];        /* copies on their way to guest memory */
    int pending[2];                     /* staging[k] holds a copy not yet written */
    int next;                           /* the staging copy the next frame goes to */
    uint64_t written;                   /* hash of the guest bytes last written back */
    int written_valid;
    uint32_t checked;                   /* frame whose guest bytes surf_current hashed */
    int guest_same;                     /* ...and found them as written back */
    uint32_t epoch;                     /* frame that last used it */
    int dirty;                          /* drawn since its last copy */
    uint32_t used;                      /* LRU clock */
} HwSurf;

typedef struct {
    uint32_t va, width, height;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
    uint32_t used;
} HwDepth;

typedef struct {
    uint32_t offset, width, height, pitch, color, palette;
    uint64_t hash;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    uint32_t used;
    uint32_t checked;                   /* frame whose guest bytes were last hashed */
} HwTex;

typedef struct {
    uint64_t key;
    uint32_t len, inputs;
    ID3D11VertexShader *vs;
    ID3D11VertexShader *vs_rc;          /* the same program for combiner batches */
    int rc_failed;
    ID3D11InputLayout *layout;          /* NULL when the program reads no input */
} HwProgram;

typedef struct { unsigned char desc[64]; size_t size; IUnknown *obj; } HwState;

/* A dynamic buffer written front to back with NO_OVERWRITE appends and
 * started over (DISCARD) when it is full, so a draw's data costs one Map that
 * never waits for the GPU. */
typedef struct { ID3D11Buffer *buf; UINT size, off; } HwRing;

/* b0 of both vertex shaders. */
typedef struct {
    float inv_size[2], inv_depth, near_z;
    float aa[2], near_clip, pad0;
    float tex_scale[2], tex_inv[2];
    float fog_on, fog_mode, fog_p[2];   /* combiner batches: the fog unit */
} HwTarget;

/* b0 of the pixel shader. */
typedef struct { uint32_t textured, alpha_func; float alpha_ref; uint32_t pad; } HwPixel;

/* b0 of a combiner pixel shader: what nv2a_rc_eval reads that is not part of
 * the combiner program, so programs that differ only in these share a shader. */
typedef struct {
    float f0[8][4], f1[8][4], final0[4], final1[4], fog[4];
    float tex_inv[4][4];                /* stage k: coordinate -> normalised */
    uint32_t alpha_func;
    float alpha_ref;
    uint32_t pad[2];
} HwRc;

typedef struct { uint64_t key; ID3D11PixelShader *ps; } HwPs;

static struct {
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11VertexShader *vs_pixels;
    ID3D11InputLayout *il_pixels;
    ID3D11PixelShader *ps;
    ID3D11VertexShader *vs_xf;          /* surface pixels with all outputs: combiner batches */
    ID3D11InputLayout *il_xf;
    ID3D11Buffer *cb_target, *cb_consts, *cb_pixel, *cb_rc;
    HwPs rcps[HW_PIXEL_SHADERS];
    HwPs *last_ps;
    HwRc last_rc;
    int rc_valid;
    HwRing vring, iring;
    /* What the constant buffers hold, so an unchanged one is not uploaded again
     * (a title's draws mostly share their constants). */
    float last_consts[NV2A_VSH_CONSTANTS][4];
    HwTarget last_target;
    HwPixel last_pixel;
    int consts_valid, target_valid, pixel_valid;
    HwSurf surf[HW_SURFACES];
    HwDepth depth[HW_DEPTHS];
    HwTex tex[HW_TEXTURES];
    HwProgram prog[HW_PROGRAMS];
    HwState blend[HW_STATES], ds[HW_STATES], raster[HW_STATES], samp[HW_STATES];
    uint32_t epoch, clock;
    uint32_t stats[6];
    /* RECOMP_HW_RENDER timing, reported every 5 s from present_sync: ms spent in
     * draws, texture hashing, texture decode + upload, program compiles, buffer
     * and constant uploads, surface write-backs and loads; counts alongside. */
    double ms_draw, ms_hash, ms_decode, ms_compile, ms_upload, ms_writeback, ms_load, report_at;
    uint32_t n_frames, n_draws, n_programs, n_hashes, n_switches, n_rc, n_rc_shaders;
    /* The last batch's program: kept while the program revision is the same,
     * so a run of batches with one program skips hashing its HLSL text. */
    HwProgram *last_prog;
    uint64_t last_prog_key;
    uint32_t *scratch;                  /* conversions: 4 bytes a pixel */
    size_t scratch_px;
    int told;
} g;

static void hw_log(const char *what, HRESULT h)
{
    if (g.told++ < 16)
        fprintf(stderr, "[HWRENDER] %s failed (0x%08lX)\n", what, (unsigned long)h);
}

static uint8_t *guest(uint32_t va) { return (uint8_t *)xbox_GetMemoryOffset() + va; }

static double now_ms(void)
{
    static double scale;
    LARGE_INTEGER c;

    if (!scale) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        scale = 1000.0 / (double)f.QuadPart;
    }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * scale;
}

static uint32_t *scratch(size_t px)
{
    if (px > g.scratch_px) {
        uint32_t *p = (uint32_t *)realloc(g.scratch, px * 4);
        if (!p)
            return NULL;
        g.scratch = p;
        g.scratch_px = px;
    }
    return g.scratch;
}

/* ---- shaders ------------------------------------------------------------ */

static const char k_target_cb[] =
    "cbuffer HwTarget : register(b0) { float2 inv_size; float inv_depth; float near_z;"
    " float2 aa; float near_clip; float pad0; float2 tex_scale; float2 tex_inv;"
    " float fog_on; float fog_mode; float2 fog_p; };\n"
    /* The fixed-function fog unit on the program's oFog.x: the executor's
     * fog_factor, per vertex (modes 0x800-0x804 as numbers). */
    "float fog_factor(float d)\n{\n"
    "    float f;\n"
    "    if (fog_on == 0.0) return 1.0;\n"
    "    if (fog_mode == 2048.0 || fog_mode == 2050.0) f = fog_p.x + exp2(d * fog_p.y * 16.0) - 1.5;\n"
    "    else if (fog_mode == 2049.0 || fog_mode == 2051.0) f = fog_p.x + exp2(-d * d * fog_p.y * fog_p.y * 32.0) - 1.5;\n"
    "    else f = fog_p.x + d * fog_p.y - 1.0;\n"
    "    if (fog_mode == 2050.0 || fog_mode == 2051.0 || fog_mode == 2052.0) f = abs(f);\n"
    "    if (isnan(f)) f = 1.0;\n"
    "    return saturate(f);\n}\n";

/* Surface pixels to clip space as the executor's rasteriser sees them:
 * x, y in pixels, z in depth units, w = 1/rhw kept for the texcoords. */
static const char k_vs_pixels[] =
    "struct VIn { float4 p : POSITION; float4 c : COLOR0; float2 uv : TEXCOORD0; };\n"
    "struct VOut { float4 pos : SV_Position; noperspective float4 color : COLOR0; float2 uv : TEXCOORD0; };\n"
    "VOut main(VIn i)\n{\n"
    "    VOut o; float w = 1.0 / i.p.w;\n"
    "    o.pos = float4((i.p.x * inv_size.x * 2.0 - 1.0) * w, (1.0 - i.p.y * inv_size.y * 2.0) * w,"
    " i.p.z * inv_depth * w, w);\n"
    "    o.color = i.c; o.uv = i.uv * tex_inv;\n    return o;\n}\n";

/* main() around nv2a_vsh(): the executor's fixes after nv2a_vsh_run (w == +0
 * becomes 1, the AA scale), then as above, with its two clip planes. */
static const char k_vs_program_main[] =
    "struct VOut { float4 pos : SV_Position; noperspective float4 color : COLOR0; float2 uv : TEXCOORD0;"
    " float2 clip : SV_ClipDistance0; };\n"
    "VOut main(Nv2aVshIn I)\n{\n"
    "    Nv2aVshOut r = nv2a_vsh(I);\n"
    "    float4 p = r.pos;\n"
    "    if (asuint(p.w) == 0) p.w = 1.0;\n"
    "    p.x *= aa.x; p.y *= aa.y;\n"
    "    VOut o; float w = p.w;\n"
    "    o.pos = float4((p.x * inv_size.x * 2.0 - 1.0) * w, (1.0 - p.y * inv_size.y * 2.0) * w,"
    " p.z * inv_depth * w, w);\n"
    "    o.color = r.d0; o.uv = r.t0.xy * tex_scale * tex_inv;\n"
    "    o.clip = float2(w - 0.001, near_clip != 0.0 ? (p.z - near_z) * w : 1.0);\n"
    "    return o;\n}\n";

/* Stage 0 times diffuse, and the alpha test as the executor compares it. */
static const char k_ps[] =
    "Texture2D tex0 : register(t0); SamplerState s0 : register(s0);\n"
    "cbuffer HwPixel : register(b0) { uint textured; uint alpha_func; float alpha_ref; uint pad; };\n"
    "struct PIn { float4 pos : SV_Position; noperspective float4 color : COLOR0; float2 uv : TEXCOORD0; };\n"
    "float4 main(PIn i) : SV_Target\n{\n"
    "    float4 c = i.color;\n"
    "    if (textured) c *= tex0.Sample(s0, i.uv);\n"
    "    if (alpha_func != 0x207) {\n"
    "        int v = (int)(saturate(c.a) * 255.0 + 0.5), r = (int)alpha_ref;\n"
    "        bool ok = alpha_func == 0x201 ? v < r : alpha_func == 0x202 ? v == r"
    " : alpha_func == 0x203 ? v <= r : alpha_func == 0x204 ? v > r : alpha_func == 0x205 ? v != r"
    " : alpha_func == 0x206 ? v >= r : false;\n"
    "        if (!ok) discard;\n"
    "    }\n"
    "    return c;\n}\n";

/* Combiner batches: everything nv2a_rc_eval reads per pixel. Colours and fog
 * are interpolated linearly in screen space, as the executor does, and the
 * texture coordinates with perspective. */
static const char k_vs_rc_out[] =
    "struct VOutRc { float4 pos : SV_Position; noperspective float4 d0 : COLOR0; noperspective float4 d1 : COLOR1;"
    " noperspective float fog : FOG; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2;"
    " float4 t3 : TEXCOORD3; float2 clip : SV_ClipDistance0; };\n";

static const char k_vs_program_main_rc[] =
    "VOutRc main(Nv2aVshIn I)\n{\n"
    "    Nv2aVshOut r = nv2a_vsh(I);\n"
    "    float4 p = r.pos;\n"
    "    if (asuint(p.w) == 0) p.w = 1.0;\n"
    "    p.x *= aa.x; p.y *= aa.y;\n"
    "    VOutRc o; float w = p.w;\n"
    "    o.pos = float4((p.x * inv_size.x * 2.0 - 1.0) * w, (1.0 - p.y * inv_size.y * 2.0) * w,"
    " p.z * inv_depth * w, w);\n"
    "    o.d0 = r.d0; o.d1 = r.d1; o.fog = fog_factor(r.fog.x);\n"
    "    o.t0 = r.t0; o.t1 = r.t1; o.t2 = r.t2; o.t3 = r.t3;\n"
    "    o.clip = float2(w - 0.001, near_clip != 0.0 ? (p.z - near_z) * w : 1.0);\n"
    "    return o;\n}\n";

/* A surface-pixel combiner batch: the executor's transformed vertices as they
 * are (Nv2aBatch.xf, Nv2aVshOutput's layout). Every w is positive there. */
static const char k_vs_xf[] =
    "struct XIn { float4 p : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1; float4 f : FOG;"
    " float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
    "VOutRc main(XIn i)\n{\n"
    "    VOutRc o; float w = i.p.w;\n"
    "    o.pos = float4((i.p.x * inv_size.x * 2.0 - 1.0) * w, (1.0 - i.p.y * inv_size.y * 2.0) * w,"
    " i.p.z * inv_depth * w, w);\n"
    "    o.d0 = i.d0; o.d1 = i.d1; o.fog = fog_factor(i.f.x);\n"
    "    o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
    "    o.clip = float2(1.0, 1.0);\n"
    "    return o;\n}\n";

/* The start of every combiner pixel shader; rc_source writes main()'s body. */
static const char k_rc_head[] =
    "cbuffer HwRc : register(b0) { float4 kF0[8]; float4 kF1[8]; float4 kFinal0; float4 kFinal1; float4 kFog;"
    " float4 kTexInv[4]; uint alpha_func; float alpha_ref; uint2 rc_pad; };\n"
    "Texture2D tex0 : register(t0); Texture2D tex1 : register(t1);"
    " Texture2D tex2 : register(t2); Texture2D tex3 : register(t3);\n"
    "SamplerState s0 : register(s0); SamplerState s1 : register(s1);"
    " SamplerState s2 : register(s2); SamplerState s3 : register(s3);\n"
    "struct PIn { float4 pos : SV_Position; noperspective float4 d0 : COLOR0; noperspective float4 d1 : COLOR1;"
    " noperspective float fog : FOG; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2;"
    " float4 t3 : TEXCOORD3; };\n"
    "float4 main(PIn i) : SV_Target\n{\n";

/* RECOMP_HW_SHADER_CACHE=<folder>: compiled shaders kept on disk. D3DCompile
 * takes 10-50 ms a shader, and a title meets most of its shaders on the first
 * frames of a new screen, so each new screen hitched while they compiled. With
 * the folder set, a shader is compiled once ever: the file name is two hashes
 * of the source and its length, the content the DXBC bytecode (written to .tmp
 * and renamed, so a killed process leaves no half file). Unset: off. */
#define HW_COMPILE_FLAGS (D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3)
static uint32_t s_cache_hits, s_cache_compiled;

static int shader_cache_path(char *path, size_t cap, const char *src, size_t len, const char *target)
{
    static const char *dir = (const char *)-1;
    uint64_t a = 1469598103934665603ull, b = 0x84222325CBF29CE4ull;
    char head[64];
    size_t i, n;

    if (dir == (const char *)-1) {
        dir = getenv("RECOMP_HW_SHADER_CACHE");
        if (dir && !*dir)
            dir = NULL;
        if (dir) {
            CreateDirectoryA(dir, NULL);
            fprintf(stderr, "[HWRENDER] shader cache on: %s\n", dir);
        }
    }
    if (!dir)
        return 0;
    n = (size_t)snprintf(head, sizeof head, "%s|%u|main|", target, (unsigned)HW_COMPILE_FLAGS);
    for (i = 0; i < n; i++) {
        a = (a ^ (uint8_t)head[i]) * 1099511628211ull;
        b = (b ^ (uint8_t)head[i]) * 0x100000001B3ull + 0x9E3779B97F4A7C15ull;
    }
    for (i = 0; i < len; i++) {
        a = (a ^ (uint8_t)src[i]) * 1099511628211ull;
        b = (b ^ (uint8_t)src[i]) * 0x100000001B3ull + 0x9E3779B97F4A7C15ull;
    }
    return snprintf(path, cap, "%s\\%s_%016llx_%016llx_%zu.cso", dir, target,
                    (unsigned long long)a, (unsigned long long)b, len) < (int)cap;
}

static ID3DBlob *shader_cache_load(const char *path)
{
    ID3DBlob *code = NULL;
    FILE *f = fopen(path, "rb");
    long size;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 32 && size < (16l << 20)
        && fseek(f, 0, SEEK_SET) == 0 && SUCCEEDED(D3DCreateBlob((SIZE_T)size, &code))) {
        uint8_t *p = (uint8_t *)ID3D10Blob_GetBufferPointer(code);
        if (fread(p, 1, (size_t)size, f) != (size_t)size || memcmp(p, "DXBC", 4) != 0) {
            ID3D10Blob_Release(code);
            code = NULL;
        }
    }
    fclose(f);
    return code;
}

static void shader_cache_store(const char *path, ID3DBlob *code)
{
    char tmp[MAX_PATH + 8];
    FILE *f;
    size_t size = ID3D10Blob_GetBufferSize(code);

    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp || !(f = fopen(tmp, "wb")))
        return;
    if (fwrite(ID3D10Blob_GetBufferPointer(code), 1, size, f) != size) {
        fclose(f);
        DeleteFileA(tmp);
        return;
    }
    fclose(f);
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(tmp);
}

static ID3DBlob *compile(const char *src, size_t len, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    char path[MAX_PATH];
    int cached = shader_cache_path(path, sizeof path, src, len, target);
    HRESULT h;

    if (cached && (code = shader_cache_load(path)) != NULL) {
        s_cache_hits++;
        return code;
    }
    h = D3DCompile(src, len, "nv2a_hw", NULL, NULL, "main", target, HW_COMPILE_FLAGS, 0, &code, &err);
    if (FAILED(h)) {
        if (g.told++ < 16)
            fprintf(stderr, "[HWRENDER] %s compile failed: %s\n", target,
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        code = NULL;
    } else {
        s_cache_compiled++;
        if (cached)
            shader_cache_store(path, code);
    }
    if (err)
        ID3D10Blob_Release(err);
    return code;
}

static ID3D11Buffer *make_cb(UINT bytes)
{
    D3D11_BUFFER_DESC d;
    ID3D11Buffer *b = NULL;

    memset(&d, 0, sizeof d);
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(g.dev, &d, NULL, &b)))
        return NULL;
    return b;
}

/* Appends `bytes` to the ring and returns where they start. */
static int ring_write(HwRing *r, UINT bind, const void *data, UINT bytes, UINT *at)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;

    if (!r->buf || bytes > r->size) {
        D3D11_BUFFER_DESC d;
        UINT want = bytes * 2 < (8u << 20) ? (8u << 20) : bytes * 2;

        if (r->buf)
            ID3D11Buffer_Release(r->buf);
        memset(r, 0, sizeof *r);
        memset(&d, 0, sizeof d);
        d.ByteWidth = want;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = bind;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(g.dev, &d, NULL, &r->buf)))
            return 0;
        r->size = want;
        how = D3D11_MAP_WRITE_DISCARD;
    }
    if (r->off + bytes > r->size) {
        r->off = 0;
        how = D3D11_MAP_WRITE_DISCARD;
    }
    if (FAILED(ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)r->buf, 0, how, 0, &m)))
        return 0;
    memcpy((uint8_t *)m.pData + r->off, data, bytes);
    ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)r->buf, 0);
    *at = r->off;
    r->off = (r->off + bytes + 15u) & ~15u;
    return 1;
}

/* ---- state objects, cached by their description ------------------------- */

static IUnknown *cached(HwState *set, const void *desc, size_t size, int kind)
{
    int i, free_slot = -1;
    IUnknown *obj = NULL;
    HRESULT h;

    for (i = 0; i < HW_STATES; i++) {
        if (set[i].obj && set[i].size == size && !memcmp(set[i].desc, desc, size))
            return set[i].obj;
        if (!set[i].obj && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0) {                          /* full: start over */
        for (i = 0; i < HW_STATES; i++)
            if (set[i].obj) { IUnknown_Release(set[i].obj); set[i].obj = NULL; }
        free_slot = 0;
    }
    switch (kind) {
    case 0: h = ID3D11Device_CreateBlendState(g.dev, (const D3D11_BLEND_DESC *)desc, (ID3D11BlendState **)&obj); break;
    case 1: h = ID3D11Device_CreateDepthStencilState(g.dev, (const D3D11_DEPTH_STENCIL_DESC *)desc, (ID3D11DepthStencilState **)&obj); break;
    case 2: h = ID3D11Device_CreateRasterizerState(g.dev, (const D3D11_RASTERIZER_DESC *)desc, (ID3D11RasterizerState **)&obj); break;
    default: h = ID3D11Device_CreateSamplerState(g.dev, (const D3D11_SAMPLER_DESC *)desc, (ID3D11SamplerState **)&obj); break;
    }
    if (FAILED(h)) {
        hw_log("state object", h);
        return NULL;
    }
    memcpy(set[free_slot].desc, desc, size);
    set[free_slot].size = size;
    set[free_slot].obj = obj;
    return obj;
}

static D3D11_BLEND blend_factor(uint32_t f)
{
    switch (f) {
    case 0:      return D3D11_BLEND_ZERO;
    case 1:      return D3D11_BLEND_ONE;
    case 0x300:  return D3D11_BLEND_SRC_COLOR;
    case 0x301:  return D3D11_BLEND_INV_SRC_COLOR;
    case 0x302:  return D3D11_BLEND_SRC_ALPHA;
    case 0x303:  return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304:  return D3D11_BLEND_DEST_ALPHA;
    case 0x305:  return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306:  return D3D11_BLEND_DEST_COLOR;
    case 0x307:  return D3D11_BLEND_INV_DEST_COLOR;
    case 0x308:  return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;      /* constant colour / alpha */
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

/* The same factor where D3D11 wants an alpha one (no _COLOR factors there). */
static D3D11_BLEND blend_factor_alpha(uint32_t f)
{
    D3D11_BLEND b = blend_factor(f);

    if (b == D3D11_BLEND_SRC_COLOR) return D3D11_BLEND_SRC_ALPHA;
    if (b == D3D11_BLEND_INV_SRC_COLOR) return D3D11_BLEND_INV_SRC_ALPHA;
    if (b == D3D11_BLEND_DEST_COLOR) return D3D11_BLEND_DEST_ALPHA;
    if (b == D3D11_BLEND_INV_DEST_COLOR) return D3D11_BLEND_INV_DEST_ALPHA;
    return b;
}

static D3D11_BLEND_OP blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_STENCIL_OP stencil_op(uint32_t op)
{
    switch (op) {
    case 0:      return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

/* GL_NEVER 0x200 .. GL_ALWAYS 0x207 in the order D3D11 numbers them from 1. */
static D3D11_COMPARISON_FUNC compare(uint32_t f)
{
    return (f >= 0x200 && f <= 0x207) ? (D3D11_COMPARISON_FUNC)(f - 0x200 + 1) : D3D11_COMPARISON_ALWAYS;
}

static void bind_states(const Nv2aRenderState *rs, int has_depth)
{
    D3D11_BLEND_DESC bd;
    D3D11_DEPTH_STENCIL_DESC dd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_RENDER_TARGET_BLEND_DESC *t = &bd.RenderTarget[0];
    float factor[4];
    UINT8 mask = 0;

    memset(&bd, 0, sizeof bd);
    t->BlendEnable = rs->blend_enable != 0;
    t->SrcBlend = blend_factor(rs->blend_src);
    t->DestBlend = blend_factor(rs->blend_dst);
    t->BlendOp = blend_op(rs->blend_eq);
    t->SrcBlendAlpha = blend_factor_alpha(rs->blend_src);
    t->DestBlendAlpha = blend_factor_alpha(rs->blend_dst);
    t->BlendOpAlpha = t->BlendOp;
    if (rs->color_mask & 0x00010000u) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (rs->color_mask & 0x00000100u) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (rs->color_mask & 0x00000001u) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (rs->color_mask & 0x01000000u) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    t->RenderTargetWriteMask = mask;
    factor[0] = (float)((rs->blend_color >> 16) & 0xFF) / 255.0f;
    factor[1] = (float)((rs->blend_color >> 8) & 0xFF) / 255.0f;
    factor[2] = (float)(rs->blend_color & 0xFF) / 255.0f;
    factor[3] = (float)((rs->blend_color >> 24) & 0xFF) / 255.0f;
    if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004 || rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004)
        factor[0] = factor[1] = factor[2] = factor[3];                  /* constant alpha */
    ID3D11DeviceContext_OMSetBlendState(g.ctx, (ID3D11BlendState *)cached(g.blend, &bd, sizeof bd, 0),
                                        factor, 0xFFFFFFFFu);

    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = has_depth && rs->depth_test_enable;
    dd.DepthWriteMask = rs->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = compare(rs->depth_func);
    dd.StencilEnable = has_depth && rs->stencil_enable;
    dd.StencilReadMask = (UINT8)rs->stencil_func_mask;
    dd.StencilWriteMask = (UINT8)rs->stencil_write_mask;
    dd.FrontFace.StencilFailOp = stencil_op(rs->stencil_fail);
    dd.FrontFace.StencilDepthFailOp = stencil_op(rs->stencil_zfail);
    dd.FrontFace.StencilPassOp = stencil_op(rs->stencil_zpass);
    dd.FrontFace.StencilFunc = compare(rs->stencil_func);
    dd.BackFace = dd.FrontFace;
    ID3D11DeviceContext_OMSetDepthStencilState(g.ctx, (ID3D11DepthStencilState *)cached(g.ds, &dd, sizeof dd, 1),
                                               rs->stencil_ref & 0xFF);

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    if (rs->cull_enable)
        rd.CullMode = rs->cull_face == 0x404 ? D3D11_CULL_FRONT : D3D11_CULL_BACK;   /* GL_FRONT / GL_BACK */
    rd.FrontCounterClockwise = rs->front_face == 0x901;                 /* GL_CCW, in surface pixels */
    rd.DepthClipEnable = FALSE;                                         /* the executor clips near only */
    rd.ScissorEnable = TRUE;
    ID3D11DeviceContext_RSSetState(g.ctx, (ID3D11RasterizerState *)cached(g.raster, &rd, sizeof rd, 2));
    if (rs->cull_enable && rs->cull_face == 0x408)                      /* front and back: nothing drawn */
        ID3D11DeviceContext_RSSetScissorRects(g.ctx, 0, NULL);
}

/* ---- colour surfaces ---------------------------------------------------- */

static void surf_release(HwSurf *s)
{
    if (s->staging[0]) ID3D11Texture2D_Release(s->staging[0]);
    if (s->staging[1]) ID3D11Texture2D_Release(s->staging[1]);
    if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->tex) ID3D11Texture2D_Release(s->tex);
    memset(s, 0, sizeof *s);
}

/* Guest rows -> the texture (16-bit R5G6B5 widened to 8 bits a channel). */
static void surf_load(HwSurf *s)
{
    const uint8_t *src = guest(s->va);

    if (s->swz) {                       /* Morton order: walk it with the masks */
        uint32_t *px = scratch((size_t)s->width * s->height), mx, my, x, y, ox, oy = 0;
        if (!px)
            return;
        xbox_swizzle_masks(s->sw, s->sh, &mx, &my);
        for (y = 0; y < s->height; y++, oy = (oy - my) & my)
            for (x = 0, ox = 0; x < s->width; x++, ox = (ox - mx) & mx) {
                uint32_t v;
                if (s->bpp == 4) {
                    v = ((const uint32_t *)src)[ox | oy];
                } else {
                    uint32_t c = ((const uint16_t *)src)[ox | oy];
                    uint32_t r = (c >> 11) & 31, gr = (c >> 5) & 63, b = c & 31;
                    v = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((gr << 2 | gr >> 4) << 8) | (b << 3 | b >> 2);
                }
                px[(size_t)y * s->width + x] = v;
            }
        ID3D11DeviceContext_UpdateSubresource(g.ctx, (ID3D11Resource *)s->tex, 0, NULL, px, s->width * 4, 0);
    } else if (s->bpp == 4) {
        ID3D11DeviceContext_UpdateSubresource(g.ctx, (ID3D11Resource *)s->tex, 0, NULL, src, s->pitch, 0);
    } else {
        uint32_t *px = scratch((size_t)s->width * s->height), x, y;
        if (!px)
            return;
        for (y = 0; y < s->height; y++) {
            const uint16_t *row = (const uint16_t *)(src + (size_t)y * s->pitch);
            for (x = 0; x < s->width; x++) {
                uint32_t v = row[x], r = (v >> 11) & 31, gr = (v >> 5) & 63, b = v & 31;
                px[(size_t)y * s->width + x] = 0xFF000000u | ((r << 3 | r >> 2) << 16)
                                             | ((gr << 2 | gr >> 4) << 8) | (b << 3 | b >> 2);
            }
        }
        ID3D11DeviceContext_UpdateSubresource(g.ctx, (ID3D11Resource *)s->tex, 0, NULL, px, s->width * 4, 0);
    }
    s->epoch = g.epoch;
    g.stats[3]++;
}

/* A hash of a surface's guest rows: written back, then compared at the
 * surface's next use to tell whether the guest has written there since. */
static uint64_t surf_guest_hash(const HwSurf *s)
{
    const uint8_t *p = guest(s->va);
    size_t bytes = s->swz ? (size_t)s->sw * s->sh * s->bpp : (size_t)s->pitch * s->height, i;
    uint64_t h = 1469598103934665603ull;

    for (i = 0; i + 8 <= bytes; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    return h;
}

/* What was drawn since the last copy, copied on the GPU into the next staging
 * texture; it reaches guest memory with surf_drain. */
static void surf_copy(HwSurf *s)
{
    int k = s->next;
    HRESULT h;

    if (!s->dirty)
        return;
    if (!s->staging[k]) {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc(s->tex, &d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(h = ID3D11Device_CreateTexture2D(g.dev, &d, NULL, &s->staging[k]))) {
            hw_log("staging texture", h);
            return;
        }
    }
    ID3D11DeviceContext_CopyResource(g.ctx, (ID3D11Resource *)s->staging[k], (ID3D11Resource *)s->tex);
    s->pending[k] = 1;
    s->next = k ^ 1;
    s->dirty = 0;
}

/* Staging copy k -> guest rows (back to R5G6B5 for 16-bit surfaces). */
static void surf_drain(HwSurf *s, int k)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint8_t *dst = guest(s->va);
    uint32_t x, y;
    HRESULT h;

    if (!s->pending[k])
        return;
    s->pending[k] = 0;
    if (FAILED(h = ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)s->staging[k], 0, D3D11_MAP_READ, 0, &m))) {
        hw_log("surface read-back", h);
        return;
    }
    if (s->swz) {                       /* back to Morton order */
        uint32_t mx, my, ox, oy = 0;
        xbox_swizzle_masks(s->sw, s->sh, &mx, &my);
        for (y = 0; y < s->height; y++, oy = (oy - my) & my) {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
            for (x = 0, ox = 0; x < s->width; x++, ox = (ox - mx) & mx) {
                uint32_t v = row[x];
                if (s->bpp == 4)
                    ((uint32_t *)dst)[ox | oy] = v;
                else
                    ((uint16_t *)dst)[ox | oy] = (uint16_t)(((v >> 8) & 0xF800) | ((v >> 5) & 0x07E0) | ((v >> 3) & 0x001F));
            }
        }
    }
    for (y = 0; !s->swz && y < s->height; y++) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
        if (s->bpp == 4) {
            memcpy(dst + (size_t)y * s->pitch, row, (size_t)s->width * 4);
        } else {
            uint16_t *out = (uint16_t *)(dst + (size_t)y * s->pitch);
            for (x = 0; x < s->width; x++) {
                uint32_t v = row[x];
                out[x] = (uint16_t)(((v >> 8) & 0xF800) | ((v >> 5) & 0x07E0) | ((v >> 3) & 0x001F));
            }
        }
    }
    ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)s->staging[k], 0);
    s->written = surf_guest_hash(s);
    s->written_valid = 1;
    g.stats[4]++;
}

/* Everything drawn, in guest memory now (oldest copy first). For a surface
 * leaving the GPU or read as a texture while being drawn. */
static void surf_write_back(HwSurf *s)
{
    surf_drain(s, s->next ^ 1);         /* the last present's copy, if still pending */
    surf_copy(s);
    surf_drain(s, s->next ^ 1);         /* what was just copied */
}

/* The surface the last draw or clear went to: at a flip, the frame the window
 * is about to show (render-to-texture passes come earlier in a frame). */
static HwSurf *s_last_target;

/* The surface the executor draws into, loaded from guest memory once a frame. */
static HwSurf *surface_for(const Nv2aSurface *ns, int load)
{
    HwSurf *hit = NULL, *victim = NULL;
    int i;

    if (!ns->width || !ns->height || (ns->bytes_per_pixel != 4 && ns->bytes_per_pixel != 2))
        return NULL;
    if (ns->swizzled && (ns->width > ns->swz_width || ns->height > ns->swz_height))
        return NULL;                              /* a clip larger than the surface */
    for (i = 0; i < HW_SURFACES; i++) {
        HwSurf *s = &g.surf[i];
        if (!s->tex) {
            if (!victim || victim->tex)
                victim = s;
            continue;
        }
        if (s->va == ns->color_va && s->width == ns->width && s->height == ns->height
            && s->pitch == ns->pitch && s->bpp == ns->bytes_per_pixel && s->swz == (ns->swizzled != 0)
            && (!s->swz || (s->sw == ns->swz_width && s->sh == ns->swz_height))) {
            hit = s;
            continue;
        }
        if (s->va == ns->color_va) {              /* same memory, other layout: hand it back first */
            surf_write_back(s);
            surf_release(s);
            if (!victim || victim->tex)
                victim = s;
            continue;
        }
        if (!victim || (victim->tex && s->used < victim->used))
            victim = s;
    }
    if (!hit) {
        D3D11_TEXTURE2D_DESC d;
        HRESULT h;

        if (victim->tex) {
            surf_write_back(victim);
            surf_release(victim);
        }
        memset(&d, 0, sizeof d);
        d.Width = ns->width;
        d.Height = ns->height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;        /* A8R8G8B8 bytes as they lie in memory */
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(h = ID3D11Device_CreateTexture2D(g.dev, &d, NULL, &victim->tex))
            || FAILED(h = ID3D11Device_CreateRenderTargetView(g.dev, (ID3D11Resource *)victim->tex, NULL, &victim->rtv))
            || FAILED(h = ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)victim->tex, NULL, &victim->srv))) {
            hw_log("surface", h);
            surf_release(victim);
            return NULL;
        }
        victim->va = ns->color_va;
        victim->width = ns->width;
        victim->height = ns->height;
        victim->pitch = ns->pitch;
        victim->bpp = ns->bytes_per_pixel;
        victim->swz = ns->swizzled != 0;
        victim->sw = ns->swz_width;
        victim->sh = ns->swz_height;
        victim->epoch = g.epoch - 1;
        hit = victim;
    }
    /* First use in a frame: the GPU copy stays unless the guest wrote the
     * surface since our last write-back (or it never had one). A pending copy
     * would then overwrite the guest's bytes, so it is dropped. */
    if (load && hit->epoch != g.epoch && !hit->dirty) {
        double t0 = now_ms();
        if (!hit->written_valid || surf_guest_hash(hit) != hit->written) {
            hit->pending[0] = hit->pending[1] = 0;
            surf_load(hit);
        }
        g.ms_load += now_ms() - t0;
    }
    hit->epoch = g.epoch;
    hit->used = ++g.clock;
    return hit;
}

/* One depth slot per zeta address and size, GPU only. */
static HwDepth *depth_for(uint32_t va, uint32_t w, uint32_t h)
{
    HwDepth *victim = &g.depth[0];
    int i;

    for (i = 0; i < HW_DEPTHS; i++) {
        HwDepth *d = &g.depth[i];
        if (d->tex && d->va == va && d->width == w && d->height == h) {
            d->used = ++g.clock;
            return d;
        }
        if (!d->tex || (victim->tex && d->used < victim->used))
            victim = d;
    }
    if (victim->tex) {
        ID3D11DepthStencilView_Release(victim->dsv);
        ID3D11Texture2D_Release(victim->tex);
        memset(victim, 0, sizeof *victim);
    }
    {
        D3D11_TEXTURE2D_DESC d;
        HRESULT hr;

        memset(&d, 0, sizeof d);
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(hr = ID3D11Device_CreateTexture2D(g.dev, &d, NULL, &victim->tex))
            || FAILED(hr = ID3D11Device_CreateDepthStencilView(g.dev, (ID3D11Resource *)victim->tex, NULL, &victim->dsv))) {
            hw_log("depth surface", hr);
            if (victim->tex) ID3D11Texture2D_Release(victim->tex);
            memset(victim, 0, sizeof *victim);
            return NULL;
        }
        ID3D11DeviceContext_ClearDepthStencilView(g.ctx, victim->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }
    victim->va = va;
    victim->width = w;
    victim->height = h;
    victim->used = ++g.clock;
    return victim;
}

/* ---- textures ----------------------------------------------------------- */

/* Bytes a texel of an uncompressed swizzled format takes (the executor's
 * tex_texel_bytes). */
static uint32_t texel_bytes(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x1C: case 0x1D:
        return 2;
    default:
        return 4;
    }
}

/* A hash of the guest bytes behind a texture (level 0, its exact size, and a
 * P8 texture's palette), so a decoded copy is reused until the title changes
 * it. Hashed at 4 bytes a texel, a DXT texture took in the memory after it
 * too, and anything the title rewrote there each frame -- Raw 2 had a
 * 512x1024 DXT3 decoded again every frame. The span stays inside the RAM or
 * contiguous window the address is in. */
static uint64_t texture_hash(const Nv2aTexture *t)
{
    uint64_t bytes, h = 1469598103934665603ull, i;
    uint32_t end = t->offset < 0x04000000u ? 0x04000000u
                 : (t->offset >= 0x80000000u && t->offset < 0x84000000u) ? 0x84000000u : t->offset;
    uint32_t block = d3d8_format_dxt_block_bytes(t->color);
    const uint8_t *p = guest(t->offset);

    if (t->pitch)
        bytes = (uint64_t)t->pitch * t->height;
    else if (block)
        bytes = (uint64_t)((t->width + 3) / 4) * ((t->height + 3) / 4) * block;
    else
        bytes = (uint64_t)t->width * t->height * texel_bytes(t->color);
    if (t->offset + bytes > end)
        bytes = end - t->offset;
    for (i = 0; i + 8 <= bytes; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    for (; i < bytes; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    if (t->color == 0x0B && t->palette) {
        const uint8_t *pal = guest(t->palette);
        for (i = 0; i < 1024; i += 8) {
            uint64_t w;
            memcpy(&w, pal + i, 8);
            h = (h ^ w) * 1099511628211ull;
        }
    }
    return h ^ bytes;
}

/* One DXT1/3/5 block's sixteen texels, with d3d8_dxt_decode_texel's
 * arithmetic (which decodes the whole block for each texel it is asked for). */
static void dxt_block(const uint8_t *b, uint32_t fmt, uint32_t px[16])
{
    uint32_t alpha[16], c[4], c0, c1, i, bits;
    int alpha_block = fmt != 0x0C;

    for (i = 0; i < 16; i++)
        alpha[i] = 255u;
    if (alpha_block) {
        if (fmt == 0x0E) {                        /* DXT3: 4 bits a texel */
            for (i = 0; i < 16; i++) {
                uint32_t nib = b[i >> 1];
                alpha[i] = d3d8_expand_channel((i & 1u) ? (nib >> 4) : (nib & 0x0Fu), 4);
            }
        } else {                                  /* DXT5: two endpoints, 3-bit index */
            uint32_t a0 = b[0], a1 = b[1], a[8];
            uint64_t sel = 0;
            for (i = 0; i < 6; i++)
                sel |= (uint64_t)b[2 + i] << (8 * i);
            a[0] = a0; a[1] = a1;
            if (a0 > a1) {
                for (i = 1; i < 7; i++)
                    a[i + 1] = ((7 - i) * a0 + i * a1) / 7;
            } else {
                for (i = 1; i < 5; i++)
                    a[i + 1] = ((5 - i) * a0 + i * a1) / 5;
                a[6] = 0; a[7] = 255;
            }
            for (i = 0; i < 16; i++)
                alpha[i] = a[(sel >> (3 * i)) & 7u];
        }
        b += 8;
    }
    c0 = (uint32_t)b[0] | ((uint32_t)b[1] << 8);
    c1 = (uint32_t)b[2] | ((uint32_t)b[3] << 8);
    for (i = 0; i < 2; i++) {
        uint32_t rgb = i ? c1 : c0;
        c[i] = (d3d8_expand_channel((rgb >> 11) & 0x1Fu, 5) << 16)
             | (d3d8_expand_channel((rgb >> 5) & 0x3Fu, 6) << 8)
             |  d3d8_expand_channel(rgb & 0x1Fu, 5);
    }
    c[2] = c[3] = 0;
    for (i = 0; i < 3; i++) {
        uint32_t sh = i * 8, m0 = (c[0] >> sh) & 0xFFu, m1 = (c[1] >> sh) & 0xFFu;
        if (c0 > c1 || alpha_block) {
            c[2] |= ((2 * m0 + m1) / 3) << sh;
            c[3] |= ((m0 + 2 * m1) / 3) << sh;
        } else {
            c[2] |= ((m0 + m1) / 2) << sh;        /* c[3] stays transparent black */
        }
    }
    bits = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    for (i = 0; i < 16; i++) {
        uint32_t idx = (bits >> (2 * i)) & 3u, a = alpha[i];
        if (!alpha_block && c0 <= c1 && idx == 3)
            a = 0;                                /* DXT1's one bit of alpha */
        px[i] = (a << 24) | (c[idx] & 0x00FFFFFFu);
    }
}

/* A whole texture at once for the formats a title rewrites every frame --
 * video (YUY2, UYVY), plain 32-bit -- with the arithmetic of the executor's
 * sample_tex, which nv2a_backend_decode_texture calls once per texel: Raw 2's
 * superstar select spent 21 ms a frame decoding its 576x448 movie that way.
 * Returns 0 for anything else, which then takes the per-texel path. */
static int decode_fast(const Nv2aTexture *t, uint32_t *out)
{
    const uint8_t *base = guest(t->offset);
    uint32_t w = t->width, h = t->height, x, y;
    int pow2 = w && h && !(w & (w - 1)) && !(h & (h - 1));

    switch (t->color) {
    case 0x06: case 0x07:                         /* A8R8G8B8, X8R8G8B8 (swizzled) */
        if (!pow2)
            return 0;
        xbox_unswizzle_rect(out, base, w, h, 4);
        if (t->color == 0x07)
            for (x = 0; x < w * h; x++)
                out[x] |= 0xFF000000u;
        return 1;
    case 0x12: case 0x1E:                         /* LIN_A8R8G8B8, LIN_X8R8G8B8 */
        if (t->pitch < w * 4)
            return 0;
        for (y = 0; y < h; y++) {
            memcpy(out + (size_t)y * w, base + (size_t)y * t->pitch, (size_t)w * 4);
            if (t->color == 0x1E)
                for (x = 0; x < w; x++)
                    out[(size_t)y * w + x] |= 0xFF000000u;
        }
        return 1;
    case 0x0C: case 0x0E: case 0x0F: {            /* DXT1/3/5: each block once, in block order */
        uint32_t bb = d3d8_format_dxt_block_bytes(t->color), bw = (w + 3) / 4, bh = (h + 3) / 4, bx, by, i;
        uint32_t px[16];
        for (by = 0; by < bh; by++)
            for (bx = 0; bx < bw; bx++) {
                dxt_block(base + ((size_t)by * bw + bx) * bb, t->color, px);
                for (i = 0; i < 16; i++) {
                    uint32_t xx = bx * 4 + (i & 3), yy = by * 4 + (i >> 2);
                    if (xx < w && yy < h)
                        out[(size_t)yy * w + xx] = px[i];
                }
            }
        return 1;
    }
    case 0x24: case 0x25: {                       /* YUY2, UYVY: BT.601 as sample_tex */
        uint32_t yoff = t->color == 0x24 ? 0u : 1u;
        if (t->pitch < ((w + 1) & ~1u) * 2)
            return 0;
        for (y = 0; y < h; y++) {
            const uint8_t *row = base + (size_t)y * t->pitch;
            uint32_t *o = out + (size_t)y * w;
            for (x = 0; x < w; x++) {
                const uint8_t *g = row + (size_t)(x & ~1u) * 2;
                int c = (int)g[(x & 1u) ? 2 + yoff : yoff] - 16;
                int cu = (int)g[1 - yoff] - 128, cv = (int)g[3 - yoff] - 128;
                int r = (298 * c + 409 * cv + 128) >> 8;
                int gg = (298 * c - 100 * cu - 208 * cv + 128) >> 8;
                int b = (298 * c + 516 * cu + 128) >> 8;
                r = r < 0 ? 0 : r > 255 ? 255 : r;
                gg = gg < 0 ? 0 : gg > 255 ? 255 : gg;
                b = b < 0 ? 0 : b > 255 ? 255 : b;
                o[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8) | (uint32_t)b;
            }
        }
        return 1;
    }
    default:
        return 0;
    }
}

/* Whether a surface's GPU copy is what the title would read at its address:
 * drawn this frame, or with a write-back still on its way (guest memory is
 * older then), or with guest memory unchanged since the last write-back
 * (hashed at most once a frame). A render target sampled the frame after it
 * was drawn used to be decoded from guest memory, a frame late, every frame. */
static int surf_current(HwSurf *s)
{
    if (s->epoch == g.epoch || s->dirty || s->pending[0] || s->pending[1])
        return 1;
    if (s->checked != g.epoch) {
        s->checked = g.epoch;
        s->guest_same = s->written_valid && surf_guest_hash(s) == s->written;
    }
    return s->guest_same;
}

static ID3D11ShaderResourceView *texture_view(const Nv2aTexture *t, HwSurf *target)
{
    HwTex *victim = &g.tex[0];
    uint64_t hash;
    int i;

    /* A surface drawn on the GPU: sample that copy (not the one being drawn). */
    for (i = 0; i < HW_SURFACES; i++) {
        HwSurf *s = &g.surf[i];
        if (s->tex && s->va == t->offset && s != target && s->width == t->width
            && s->height == t->height && surf_current(s))
            return s->srv;
        if (s->tex && s == target && s->va == t->offset)
            surf_write_back(s);                   /* feedback: read what is drawn so far */
    }
    /* Compared with guest memory once a frame: a title rewrites a texture
     * between frames, and hashing it again for each draw cost more than the
     * draws (a 512x512 texture is 1 MB a draw). */
    for (i = 0; i < HW_TEXTURES; i++) {
        HwTex *x = &g.tex[i];
        if (x->tex && x->checked == g.epoch && x->offset == t->offset && x->width == t->width
            && x->height == t->height && x->pitch == t->pitch && x->color == t->color
            && x->palette == t->palette) {
            x->used = ++g.clock;
            return x->srv;
        }
    }
    {
        double t0 = now_ms();
        hash = texture_hash(t);
        g.ms_hash += now_ms() - t0;
        g.n_hashes++;
    }
    for (i = 0; i < HW_TEXTURES; i++) {
        HwTex *x = &g.tex[i];
        if (x->tex && x->offset == t->offset && x->width == t->width && x->height == t->height
            && x->pitch == t->pitch && x->color == t->color && x->palette == t->palette
            && x->hash == hash) {
            x->used = ++g.clock;
            x->checked = g.epoch;
            return x->srv;
        }
        if (!x->tex || (victim->tex && x->used < victim->used))
            victim = x;
    }
    {
        double t0 = now_ms();
        uint32_t *argb = scratch((size_t)t->width * t->height);
        D3D11_TEXTURE2D_DESC d;
        D3D11_SUBRESOURCE_DATA sd;
        HRESULT h;

        if (!argb || !(decode_fast(t, argb) || nv2a_backend_decode_texture(t, argb))) {
            /* The stage reads white then: say so once per format, or a missing
             * texture only shows as flat colour on screen. */
            static uint32_t told[32];
            static int ntold;
            int k;
            for (k = 0; k < ntold && told[k] != t->color; k++)
                ;
            if (k == ntold && ntold < 32) {
                told[ntold++] = t->color;
                fprintf(stderr, "[HWRENDER] texture format 0x%02X (%ux%u, pitch %u, palette 0x%08X) could not be "
                        "decoded: drawn as white\n", t->color, t->width, t->height, t->pitch, t->palette);
            }
            return NULL;
        }
        {
            /* RECOMP_HW_TEX_DUMP=<folder>: each decoded texture as a BMP, named
             * by address, format and size, to see what a draw was given. */
            static const char *dir = (const char *)-1;
            if (dir == (const char *)-1)
                dir = getenv("RECOMP_HW_TEX_DUMP");
            if (dir && t->width <= 4096 && t->height <= 4096) {
                char path[512];
                FILE *f;
                snprintf(path, sizeof path, "%s/tex_%08X_f%02X_%ux%u_%016llX.bmp", dir, t->offset, t->color,
                         t->width, t->height, (unsigned long long)hash);
                if ((f = fopen(path, "wb")) != NULL) {
                    uint32_t w = t->width, hh = t->height, sz = 54 + w * hh * 4, y;
                    uint8_t hdr[54];
                    memset(hdr, 0, sizeof hdr);
                    hdr[0] = 'B'; hdr[1] = 'M';
                    memcpy(hdr + 2, &sz, 4);
                    hdr[10] = 54; hdr[14] = 40;
                    memcpy(hdr + 18, &w, 4);
                    memcpy(hdr + 22, &hh, 4);
                    hdr[26] = 1; hdr[28] = 32;
                    fwrite(hdr, 1, 54, f);
                    for (y = 0; y < hh; y++)              /* bottom-up rows, BGRA as decoded */
                        fwrite(argb + (size_t)(hh - 1 - y) * w, 4, w, f);
                    fclose(f);
                }
            }
        }
        if (victim->tex) {
            ID3D11ShaderResourceView_Release(victim->srv);
            ID3D11Texture2D_Release(victim->tex);
            memset(victim, 0, sizeof *victim);
        }
        memset(&d, 0, sizeof d);
        d.Width = t->width;
        d.Height = t->height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sd.pSysMem = argb;
        sd.SysMemPitch = t->width * 4;
        sd.SysMemSlicePitch = 0;
        /* A texture the title gives mip levels gets a whole chain, made from
         * level 0 on the GPU: sampled at a distance without one, a texture
         * shimmers. ponytail: the title's own levels are not decoded
         * (nv2a_backend_decode_texture gives level 0); generated ones look the
         * same unless a title paints its levels differently. */
        if (t->levels > 1) {
            d.MipLevels = 0;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        }
        if (FAILED(h = ID3D11Device_CreateTexture2D(g.dev, &d, t->levels > 1 ? NULL : &sd, &victim->tex))
            || FAILED(h = ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)victim->tex, NULL, &victim->srv))) {
            hw_log("texture", h);
            if (victim->tex) ID3D11Texture2D_Release(victim->tex);
            memset(victim, 0, sizeof *victim);
            return NULL;
        }
        if (t->levels > 1) {
            ID3D11DeviceContext_UpdateSubresource(g.ctx, (ID3D11Resource *)victim->tex, 0, NULL, argb, t->width * 4, 0);
            ID3D11DeviceContext_GenerateMips(g.ctx, victim->srv);
        }
        victim->offset = t->offset;
        victim->width = t->width;
        victim->height = t->height;
        victim->pitch = t->pitch;
        victim->color = t->color;
        victim->palette = t->palette;
        victim->hash = hash;
        victim->used = ++g.clock;
        victim->checked = g.epoch;
        g.stats[5]++;
        g.ms_decode += now_ms() - t0;
        return victim->srv;
    }
}

static D3D11_TEXTURE_ADDRESS_MODE address_mode(uint32_t a)
{
    switch (a) {
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3:  return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 4:  return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

/* SET_TEXTURE_FILTER as the title set it: MIN in bits 16-23 (1 nearest,
 * 2 linear, 3-6 the four nearest/linear x mipmap-nearest/linear modes,
 * 7 convolution), MAG in bits 24-27 (1 nearest, 2 linear, 4 convolution).
 * Convolution is drawn as linear. Mip levels are used only when the min
 * filter asks for them and the texture has them. `border_argb` is what the
 * BORDER address mode reads. */
static ID3D11SamplerState *sampler(const Nv2aTexture *t, uint32_t border_argb)
{
    static uint32_t seen[32];
    static int nseen;
    D3D11_SAMPLER_DESC d;
    uint32_t min = (t->filter >> 16) & 0xFF, mag = (t->filter >> 24) & 0xF;
    int min_linear = min == 2 || min == 4 || min == 6 || min == 7;
    int mip_linear = min == 5 || min == 6;
    int mipped = min >= 3 && min <= 6 && t->levels > 1;
    int i;

    for (i = 0; i < nseen && seen[i] != t->filter; i++)
        ;
    if (i == nseen && nseen < 32) {
        seen[nseen++] = t->filter;
        fprintf(stderr, "[HWRENDER] texture filter 0x%08X: min %u mag %u (%u levels, format 0x%02X)\n",
                t->filter, min, mag, t->levels, t->color);
    }
    memset(&d, 0, sizeof d);
    d.Filter = D3D11_ENCODE_BASIC_FILTER(min_linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                         mag == 1 ? D3D11_FILTER_TYPE_POINT : D3D11_FILTER_TYPE_LINEAR,
                                         mip_linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT, 0);
    d.AddressU = address_mode(t->addr_u);
    d.AddressV = address_mode(t->addr_v);
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d.BorderColor[0] = (float)((border_argb >> 16) & 0xFF) / 255.0f;
    d.BorderColor[1] = (float)((border_argb >> 8) & 0xFF) / 255.0f;
    d.BorderColor[2] = (float)(border_argb & 0xFF) / 255.0f;
    d.BorderColor[3] = (float)(border_argb >> 24) / 255.0f;
    d.MaxLOD = mipped ? D3D11_FLOAT32_MAX : 0.0f;
    return (ID3D11SamplerState *)cached(g.samp, &d, sizeof d, 3);
}

/* ---- vertex programs ---------------------------------------------------- */

static uint64_t text_hash(const char *s, uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t i;

    for (i = 0; i < n; i++)
        h = (h ^ (uint8_t)s[i]) * 1099511628211ull;
    return h;
}

/* A program's vertex shader source compiled: the emitted nv2a_vsh() and a
 * main() around it, for the stage-0 pixel shader or (rc) the combiners'. */
static ID3DBlob *program_blob(const Nv2aProgramBatch *pb, int rc)
{
    const char *mid = rc ? k_vs_rc_out : "", *tail = rc ? k_vs_program_main_rc : k_vs_program_main;
    size_t a = strlen(k_target_cb), m = strlen(mid), z = strlen(tail), len = a + pb->hlsl_len + m + z;
    char *src = (char *)malloc(len + 1);
    ID3DBlob *code;
    double t0;

    if (!src)
        return NULL;
    memcpy(src, k_target_cb, a);
    memcpy(src + a, pb->hlsl, pb->hlsl_len);
    memcpy(src + a + pb->hlsl_len, mid, m);
    memcpy(src + a + pb->hlsl_len + m, tail, z + 1);
    t0 = now_ms();
    code = compile(src, len, "vs_5_0");
    g.ms_compile += now_ms() - t0;
    free(src);
    return code;
}

static HwProgram *program_for(const Nv2aProgramBatch *pb)
{
    uint64_t key;
    HwProgram *slot = NULL;
    ID3DBlob *code;
    D3D11_INPUT_ELEMENT_DESC ie[NV2A_VSH_INPUTS];
    UINT n = 0, a;
    HRESULT h;
    int i;

    key = pb->program_key ? pb->program_key : text_hash(pb->hlsl, pb->hlsl_len);
    if (g.last_prog && key == g.last_prog_key)
        return g.last_prog;
    g.n_switches++;
    for (i = 0; i < HW_PROGRAMS; i++) {
        HwProgram *p = &g.prog[i];
        if (p->vs && p->key == key && p->len == pb->hlsl_len) {
            g.last_prog = p;
            g.last_prog_key = key;
            return p;
        }
        if (!p->vs && !slot)
            slot = p;
    }
    if (!slot) {                                  /* full: start over */
        for (i = 0; i < HW_PROGRAMS; i++) {
            if (g.prog[i].layout) ID3D11InputLayout_Release(g.prog[i].layout);
            if (g.prog[i].vs) ID3D11VertexShader_Release(g.prog[i].vs);
            if (g.prog[i].vs_rc) ID3D11VertexShader_Release(g.prog[i].vs_rc);
        }
        memset(g.prog, 0, sizeof g.prog);
        g.last_prog = NULL;
        slot = &g.prog[0];
    }
    if (!(code = program_blob(pb, 0)))
        return NULL;
    for (a = 0; a < NV2A_VSH_INPUTS; a++) {
        if (!(pb->inputs_read & (1u << a)))
            continue;
        ie[n].SemanticName = "ATTR";
        ie[n].SemanticIndex = a;
        ie[n].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        ie[n].InputSlot = 0;
        ie[n].AlignedByteOffset = n * 16;
        ie[n].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        ie[n].InstanceDataStepRate = 0;
        n++;
    }
    if (FAILED(h = ID3D11Device_CreateVertexShader(g.dev, ID3D10Blob_GetBufferPointer(code),
                                                    ID3D10Blob_GetBufferSize(code), NULL, &slot->vs))
        || (n && FAILED(h = ID3D11Device_CreateInputLayout(g.dev, ie, n, ID3D10Blob_GetBufferPointer(code),
                                                           ID3D10Blob_GetBufferSize(code), &slot->layout)))) {
        hw_log("program shader", h);
        if (slot->vs) ID3D11VertexShader_Release(slot->vs);
        memset(slot, 0, sizeof *slot);
        ID3D10Blob_Release(code);
        return NULL;
    }
    ID3D10Blob_Release(code);
    slot->key = key;
    slot->len = pb->hlsl_len;
    slot->inputs = pb->inputs_read;
    g.stats[2]++;
    g.last_prog = slot;
    g.last_prog_key = key;
    return slot;
}

/* The program's vertex shader for combiner batches, made the first time one
 * draws with it. The input signature is the same, so is the layout. */
static ID3D11VertexShader *program_vs_rc(HwProgram *p, const Nv2aProgramBatch *pb)
{
    ID3DBlob *code;
    HRESULT h;

    if (p->vs_rc || p->rc_failed)
        return p->vs_rc;
    if (!(code = program_blob(pb, 1))) {
        p->rc_failed = 1;
        return NULL;
    }
    if (FAILED(h = ID3D11Device_CreateVertexShader(g.dev, ID3D10Blob_GetBufferPointer(code),
                                                    ID3D10Blob_GetBufferSize(code), NULL, &p->vs_rc))) {
        hw_log("combiner program shader", h);
        p->vs_rc = NULL;
        p->rc_failed = 1;
    }
    ID3D10Blob_Release(code);
    return p->vs_rc;
}

/* ---- register combiners ------------------------------------------------- */

/* Source text being built; `over` when it did not fit. */
typedef struct { char buf[65536]; size_t n; int over; } HwSrc;

static void src_put(HwSrc *s, const char *fmt, ...)
{
    va_list ap;
    int k;

    if (s->over)
        return;
    va_start(ap, fmt);
    k = vsnprintf(s->buf + s->n, sizeof s->buf - s->n, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= sizeof s->buf - s->n) {
        s->over = 1;
        return;
    }
    s->n += (size_t)k;
}

/* Stage k is read when combiner register 8 + k is an input of a general stage
 * or of the final combiner. Stage 0 always is: its alpha starts r0.a. */
static void rc_stages_used(const Nv2aCombiner *rc, int used[4])
{
    uint32_t n = rc->control & 0xFF, s, sh;
    int reg[16], k;

    memset(reg, 0, sizeof reg);
    if (n > 8)
        n = 8;
    for (s = 0; s < n; s++)
        for (sh = 0; sh < 32; sh += 8) {
            reg[(rc->color_icw[s] >> sh) & 15] = 1;
            reg[(rc->alpha_icw[s] >> sh) & 15] = 1;
        }
    if (rc->final0 || rc->final1) {
        for (sh = 0; sh < 32; sh += 8)
            reg[(rc->final0 >> sh) & 15] = 1;
        for (sh = 8; sh < 32; sh += 8)
            reg[(rc->final1 >> sh) & 15] = 1;
    }
    for (k = 0; k < 4; k++)
        used[k] = k == 0 || reg[8 + k];
}

/* An input byte as HLSL: register (low nibble), .a or the portion's own
 * channels (bit 4), and the mapping (bits 5-7), as nv2a_rc_eval's map_in. */
static void rc_in(char out[64], uint32_t byte, int alpha_portion)
{
    char x[16];
    unsigned reg = byte & 15, a = (byte >> 4) & 1;

    snprintf(x, sizeof x, "R%u.%s", reg, alpha_portion ? (a ? "a" : "b") : (a ? "aaa" : "rgb"));
    switch ((byte >> 5) & 7) {
    case 0:  snprintf(out, 64, "max(%s, 0.0)", x); break;
    case 1:  snprintf(out, 64, "(1.0 - saturate(%s))", x); break;
    case 2:  snprintf(out, 64, "(2.0 * max(%s, 0.0) - 1.0)", x); break;
    case 3:  snprintf(out, 64, "(-2.0 * max(%s, 0.0) + 1.0)", x); break;
    case 4:  snprintf(out, 64, "(max(%s, 0.0) - 0.5)", x); break;
    case 5:  snprintf(out, 64, "(-max(%s, 0.0) + 0.5)", x); break;
    case 6:  snprintf(out, 64, "(%s)", x); break;
    default: snprintf(out, 64, "(-(%s))", x); break;
    }
}

/* Output scale/bias, as map_out, then the [-1, 1] clamp. */
static void rc_out(char out[96], const char *x, uint32_t m)
{
    switch (m) {
    case 0x08: snprintf(out, 96, "clamp(%s - 0.5, -1.0, 1.0)", x); break;
    case 0x10: snprintf(out, 96, "clamp(%s * 2.0, -1.0, 1.0)", x); break;
    case 0x18: snprintf(out, 96, "clamp((%s - 0.5) * 2.0, -1.0, 1.0)", x); break;
    case 0x20: snprintf(out, 96, "clamp(%s * 4.0, -1.0, 1.0)", x); break;
    case 0x30: snprintf(out, 96, "clamp(%s * 0.5, -1.0, 1.0)", x); break;
    default:   snprintf(out, 96, "clamp(%s, -1.0, 1.0)", x); break;
    }
}

/* The pixel shader for one combiner program: the stage fetches of the
 * executor's rc_stage_fetch, then nv2a_rc_eval statement for statement (its
 * encoding follows xemu's hw/xbox/nv2a/pgraph/glsl/psh.c; see NOTICE).
 * have[k]: stage k has a texture bound to slot k. */
static void rc_source(HwSrc *s, const Nv2aPixelState *ps, const int have[4], const int used[4])
{
    static const char comp[] = "xyzw";
    const Nv2aCombiner *rc = &ps->rc;
    uint32_t n = rc->control & 0xFF, flags = rc->control >> 8, st, k, j;
    char A[64], B[64], C[64], D[64], o0[96], o1[96], o2[96];

    if (n > 8)
        n = 8;
    src_put(s, "%s", k_rc_head);
    for (k = 0; k < 4; k++) {
        switch (ps->stage[k].mode) {
        case 0:                                   /* NONE */
            src_put(s, " float4 T%u = float4(0.0, 0.0, 0.0, 1.0);\n", k);
            break;
        case 1: case 2: case 3:                   /* PROJECT2D / 3D, CUBEMAP */
            if (!used[k])
                src_put(s, " float4 T%u = float4(0.0, 0.0, 0.0, 1.0);\n", k);
            else if (!have[k])                    /* an unusable stage reads white */
                src_put(s, " float4 T%u = 1.0;\n", k);
            else if (ps->stage[k].mode == 3)      /* not a cube: the first two coordinates */
                src_put(s, " float4 T%u = tex%u.Sample(s%u, i.t%u.xy * kTexInv[%u].xy);\n", k, k, k, k, k);
            else
                src_put(s, " float4 T%u = tex%u.Sample(s%u, i.t%u.xy / (i.t%u.w != 0.0 ? i.t%u.w : 1.0)"
                           " * kTexInv[%u].xy);\n", k, k, k, k, k, k, k);
            break;
        case 4:                                   /* PASSTHRU */
            src_put(s, " float4 T%u = saturate(i.t%u);\n", k, k);
            break;
        case 5:                                   /* CLIPPLANE */
            src_put(s, " float4 T%u = 0.0;\n", k);
            for (j = 0; j < 4; j++)
                src_put(s, " if (i.t%u.%c %s 0.0) discard;\n", k, comp[j],
                        ((ps->clip_plane_mode >> (k * 4 + j)) & 1) ? ">=" : "<");
            break;
        default:                                  /* bump-env, dot product: zero, as the CPU path */
            src_put(s, " float4 T%u = 0.0;\n", k);
            break;
        }
    }
    src_put(s, " float4 R0 = 0.0, R1 = 0.0, R2 = 0.0, R3 = float4(kFog.rgb, i.fog), R4 = i.d0, R5 = i.d1;\n"
               " float4 R6 = 0.0, R7 = 0.0, R8 = T0, R9 = T1, R10 = T2, R11 = T3;\n"
               " float4 R12 = float4(0.0, 0.0, 0.0, %s), R13 = 0.0, R14 = 0.0, R15 = 0.0;\n",
            (rc->stage_program & 0x1F) ? "T0.a" : "1.0");
    for (st = 0; st < n; st++) {
        uint32_t icw = rc->color_icw[st], ocw = rc->color_ocw[st];
        uint32_t aicw = rc->alpha_icw[st], aocw = rc->alpha_ocw[st];
        uint32_t fl = ocw >> 12, afl = aocw >> 12;
        uint32_t dab = (ocw >> 4) & 15, dcd = ocw & 15, dsum = (ocw >> 8) & 15;

        src_put(s, " {\n  R1 = kF0[%u]; R2 = kF1[%u];\n", (flags & 0x010) ? st : 0, (flags & 0x100) ? st : 0);
        src_put(s, "  bool mux = %s;\n", (flags & 1) ? "R12.a >= 0.5" : "(((int)(R12.a * 255.0)) & 1) != 0");
        rc_in(A, icw >> 24, 0); rc_in(B, icw >> 16, 0); rc_in(C, icw >> 8, 0); rc_in(D, icw, 0);
        src_put(s, "  float3 A = %s, B = %s, C = %s, D = %s;\n", A, B, C, D);
        rc_in(A, aicw >> 24, 1); rc_in(B, aicw >> 16, 1); rc_in(C, aicw >> 8, 1); rc_in(D, aicw, 1);
        src_put(s, "  float aA = %s, aB = %s, aC = %s, aD = %s;\n", A, B, C, D);
        src_put(s, "  float3 pab = %s;\n", (fl & 2) ? "(A.x * B.x + A.y * B.y + A.z * B.z).xxx" : "A * B");
        src_put(s, "  float3 pcd = %s;\n", (fl & 1) ? "(C.x * D.x + C.y * D.y + C.z * D.z).xxx" : "C * D");
        src_put(s, "  float3 sum = %s;\n", (fl & 4) ? "mux ? pcd : pab" : "pab + pcd");
        rc_out(o0, "pab", fl & 0x38); rc_out(o1, "pcd", fl & 0x38); rc_out(o2, "sum", fl & 0x38);
        src_put(s, "  float3 ab = %s, cd = %s, ms = %s;\n", o0, o1, o2);
        src_put(s, "  float apab = aA * aB, apcd = aC * aD;\n  float asum = %s;\n",
                (afl & 4) ? "mux ? apcd : apab" : "apab + apcd");
        rc_out(o0, "apab", afl & 0x38); rc_out(o1, "apcd", afl & 0x38); rc_out(o2, "asum", afl & 0x38);
        src_put(s, "  float aab = %s, acd = %s, ams = %s;\n", o0, o1, o2);
        /* Writes in the evaluator's order; blue-to-alpha also writes .a. */
        if (dab) {
            src_put(s, "  R%u.rgb = ab;", dab);
            if (fl & 0x80)
                src_put(s, " R%u.a = ab.b;", dab);
            src_put(s, "\n");
        }
        if (dcd) {
            src_put(s, "  R%u.rgb = cd;", dcd);
            if (fl & 0x40)
                src_put(s, " R%u.a = cd.b;", dcd);
            src_put(s, "\n");
        }
        if (dsum)
            src_put(s, "  R%u.rgb = ms;\n", dsum);
        if ((aocw >> 4) & 15)
            src_put(s, "  R%u.a = aab;\n", (aocw >> 4) & 15);
        if (aocw & 15)
            src_put(s, "  R%u.a = acd;\n", aocw & 15);
        if ((aocw >> 8) & 15)
            src_put(s, "  R%u.a = ams;\n", (aocw >> 8) & 15);
        src_put(s, "  R0 = 0.0;\n }\n");                 /* register 0 stays zero */
    }
    if (rc->final0 || rc->final1) {
        uint32_t f0 = rc->final0, f1 = rc->final1, ff = f1 & 0xFF;

        src_put(s, " R1 = kFinal0; R2 = kFinal1;\n");
        src_put(s, " { float3 a = %s, b = %s; R14 = float4(%s, 0.0); }\n",
                (ff & 0x40) ? "1.0 - R5.rgb" : "R5.rgb", (ff & 0x20) ? "1.0 - R12.rgb" : "R12.rgb",
                (ff & 0x80) ? "saturate(a + b)" : "a + b");
        rc_in(A, f1 >> 24, 0); rc_in(B, f1 >> 16, 0);
        src_put(s, " { float3 E = %s, F = %s; R15 = float4(E * F, 0.0); }\n", A, B);
        rc_in(A, f0 >> 24, 0); rc_in(B, f0 >> 16, 0); rc_in(C, f0 >> 8, 0); rc_in(D, f0, 0);
        src_put(s, " float3 fA = %s, fB = %s, fC = %s, fD = %s;\n", A, B, C, D);
        rc_in(A, f1 >> 8, 1);
        src_put(s, " float4 outc = float4(fD + fC * (1.0 - fA) + fB * fA, %s);\n", A);
    } else {
        src_put(s, " float4 outc = R12;\n");
    }
    src_put(s, " outc = saturate(outc);\n"
               " if (alpha_func != 0x207) {\n"
               "  int v = (int)(outc.a * 255.0 + 0.5), r = (int)alpha_ref;\n"
               "  bool ok = alpha_func == 0x201 ? v < r : alpha_func == 0x202 ? v == r"
               " : alpha_func == 0x203 ? v <= r : alpha_func == 0x204 ? v > r : alpha_func == 0x205 ? v != r"
               " : alpha_func == 0x206 ? v >= r : false;\n"
               "  if (!ok) discard;\n"
               " }\n"
               " return outc;\n}\n");
}

/* What makes two combiner shaders differ: the program and which stages have
 * a texture. Constants and texture sizes are in HwRc. */
static uint64_t rc_key(const Nv2aPixelState *ps, const int have[4], const int used[4])
{
    const Nv2aCombiner *rc = &ps->rc;
    uint32_t n = rc->control & 0xFF, i;
    uint64_t h = 1469598103934665603ull;

#define RC_MIX(v) (h = (h ^ (uint64_t)(uint32_t)(v)) * 1099511628211ull)
    if (n > 8)
        n = 8;
    RC_MIX(rc->control); RC_MIX(rc->stage_program); RC_MIX(rc->final0); RC_MIX(rc->final1);
    for (i = 0; i < n; i++) {
        RC_MIX(rc->color_icw[i]); RC_MIX(rc->color_ocw[i]);
        RC_MIX(rc->alpha_icw[i]); RC_MIX(rc->alpha_ocw[i]);
    }
    for (i = 0; i < 4; i++)
        RC_MIX((uint32_t)have[i] | (uint32_t)used[i] << 1);
    RC_MIX(ps->clip_plane_mode);
#undef RC_MIX
    return h ? h : 1;
}

/* The shader for a combiner program, made the first time it is drawn. One
 * that fails stays in the table as NULL and is not tried again. */
static ID3D11PixelShader *rc_shader(const Nv2aPixelState *ps, const int have[4], const int used[4])
{
    static HwSrc src;
    static int shown;
    uint64_t key = rc_key(ps, have, used);
    HwPs *slot = NULL;
    ID3DBlob *code;
    double t0;
    int i;

    if (g.last_ps && g.last_ps->key == key)
        return g.last_ps->ps;
    for (i = 0; i < HW_PIXEL_SHADERS; i++) {
        HwPs *p = &g.rcps[i];
        if (p->key == key) {
            g.last_ps = p;
            return p->ps;
        }
        if (!p->key && !slot)
            slot = p;
    }
    if (!slot) {                                  /* full: start over */
        for (i = 0; i < HW_PIXEL_SHADERS; i++)
            if (g.rcps[i].ps) ID3D11PixelShader_Release(g.rcps[i].ps);
        memset(g.rcps, 0, sizeof g.rcps);
        slot = &g.rcps[0];
    }
    slot->key = key;
    slot->ps = NULL;
    g.last_ps = slot;
    src.n = 0;
    src.over = 0;
    rc_source(&src, ps, have, used);
    if (src.over) {
        if (g.told++ < 16)
            fprintf(stderr, "[HWRENDER] combiner shader source too long\n");
        return NULL;
    }
    t0 = now_ms();
    code = compile(src.buf, src.n, "ps_5_0");
    g.ms_compile += now_ms() - t0;
    if (!code) {
        if (!shown++)
            fprintf(stderr, "[HWRENDER] the combiner shader that failed:\n%.*s\n", (int)src.n, src.buf);
        return NULL;
    }
    if (FAILED(ID3D11Device_CreatePixelShader(g.dev, ID3D10Blob_GetBufferPointer(code),
                                              ID3D10Blob_GetBufferSize(code), NULL, &slot->ps))) {
        hw_log("combiner pixel shader", E_FAIL);
        slot->ps = NULL;
    }
    ID3D10Blob_Release(code);
    g.n_rc_shaders++;
    return slot->ps;
}

/* A combiner batch's pixel shader with its textures, samplers and constants,
 * or NULL (nothing bound) when the shader cannot be made. */
static ID3D11PixelShader *rc_prepare(const Nv2aPixelState *ps, const Nv2aRenderState *rs, HwSurf *target,
                                     ID3D11ShaderResourceView *srv[4], ID3D11SamplerState *smp[4])
{
    static uint32_t seen[32];
    static int nseen;
    ID3D11PixelShader *shader;
    HwRc c;
    int used[4], have[4], k;

    for (k = 0; k < nseen && seen[k] != ps->rc.stage_program; k++)
        ;
    if (k == nseen && nseen < 32) {
        seen[nseen++] = ps->rc.stage_program;
        fprintf(stderr, "[HWRENDER] combiner stage modes %u %u %u %u (%u general stages)\n",
                ps->stage[0].mode, ps->stage[1].mode, ps->stage[2].mode, ps->stage[3].mode,
                ps->rc.control & 0xFF);
    }
    rc_stages_used(&ps->rc, used);
    memset(&c, 0, sizeof c);
    for (k = 0; k < 4; k++) {
        const Nv2aStage *st = &ps->stage[k];
        int fetch = st->mode == 1 || st->mode == 2 || st->mode == 3;

        have[k] = 0;
        c.tex_inv[k][0] = c.tex_inv[k][1] = 1.0f;
        if (fetch && used[k] && !(st->valid && st->tex.width && st->tex.height)) {
            static int told;
            if (told++ < 4)
                fprintf(stderr, "[HWRENDER] stage %d (mode %u) is read with no usable texture bound: white\n",
                        k, st->mode);
        }
        if (!fetch || !used[k] || !st->valid || !st->tex.width || !st->tex.height)
            continue;
        if (st->mode == 3 && st->cube)            /* ponytail: one face decoded -- reads white */
            continue;
        if (!(srv[k] = texture_view(&st->tex, target)))
            continue;
        smp[k] = sampler(&st->tex, (st->border_u || st->border_v) ? st->border_argb : 0);
        have[k] = 1;
        if (!st->size_from_format) {              /* texel coordinates */
            c.tex_inv[k][0] = 1.0f / (float)st->tex.width;
            c.tex_inv[k][1] = 1.0f / (float)st->tex.height;
        }
    }
    if (!(shader = rc_shader(ps, have, used))) {
        memset(srv, 0, 4 * sizeof *srv);
        memset(smp, 0, 4 * sizeof *smp);
        return NULL;
    }
    for (k = 0; k < 8; k++) {
        nv2a_rc_unpack(ps->rc.factor0[k], c.f0[k]);
        nv2a_rc_unpack(ps->rc.factor1[k], c.f1[k]);
    }
    nv2a_rc_unpack(ps->rc.final_c0, c.final0);
    nv2a_rc_unpack(ps->rc.final_c1, c.final1);
    nv2a_rc_unpack(ps->fog_color, c.fog);         /* FOG_COLOR is R in bits 0-7 */
    { float r = c.fog[2]; c.fog[2] = c.fog[0]; c.fog[0] = r; }
    c.alpha_func = rs->alpha_test_enable ? rs->alpha_func : 0x207;
    c.alpha_ref = (float)(rs->alpha_ref & 0xFF);
    if (!g.rc_valid || memcmp(&g.last_rc, &c, sizeof c)) {
        ID3D11DeviceContext_UpdateSubresource(g.ctx, (ID3D11Resource *)g.cb_rc, 0, NULL, &c, 0, 0);
        g.last_rc = c;
        g.rc_valid = 1;
    }
    return shader;
}

/* ---- the back end ------------------------------------------------------- */

static void hw_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    Nv2aSurface ns = *s;
    int i;

    /* A title clears a swizzled surface through a linear view of it (Raw 2
     * does, before every draw into its render targets, depth-only clears
     * included). A fill is the same bytes in either layout, so the clear goes
     * to the surface as it already is; taken as a change of layout it was
     * reloaded row-major from Morton-order memory, and the next draw that
     * sampled it read stripes. */
    for (i = 0; i < HW_SURFACES; i++) {
        HwSurf *x = &g.surf[i];
        if (x->tex && x->va == s->color_va && x->width == s->width && x->height == s->height
            && x->bpp == s->bytes_per_pixel) {
            ns.swizzled = x->swz;
            ns.swz_width = x->sw;
            ns.swz_height = x->sh;
            ns.pitch = x->pitch;
            break;
        }
    }
    if (flags & 0xF0u) {
        HwSurf *t = surface_for(&ns, (flags & 0xF0u) != 0xF0u);   /* a full colour clear needs no load */
        if (t)
            s_last_target = t;
        float c[4] = { (float)((argb >> 16) & 0xFF) / 255.0f, (float)((argb >> 8) & 0xFF) / 255.0f,
                       (float)(argb & 0xFF) / 255.0f, (float)((argb >> 24) & 0xFF) / 255.0f };
        /* SET_COLOR_CLEAR_VALUE is in the surface's own format: a 16-bit
         * surface gets a packed R5G6B5 value. Raw 2 clears its ring-mat light
         * map with 0x0000FFFF (white); read as A8R8G8B8 it was cyan. */
        if (s->bytes_per_pixel == 2) {
            c[0] = (float)((argb >> 11) & 0x1F) / 31.0f;
            c[1] = (float)((argb >> 5) & 0x3F) / 63.0f;
            c[2] = (float)(argb & 0x1F) / 31.0f;
            c[3] = 1.0f;
        }
        if (!t)
            return;
        ID3D11DeviceContext_ClearRenderTargetView(g.ctx, t->rtv, c);
        t->dirty = 1;
    }
    if ((flags & 3u) && rs->zeta_va && s->width && s->height) {   /* depth only: the colour surface is not touched */
        HwDepth *d = depth_for(rs->zeta_va, s->width, s->height);
        if (d) {
            UINT what = ((flags & 1u) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 2u) ? D3D11_CLEAR_STENCIL : 0);
            float z = (float)(rs->zeta_format == 1 ? (zstencil & 0xFFFF) : (zstencil >> 8)) / 16777215.0f;
            ID3D11DeviceContext_ClearDepthStencilView(g.ctx, d->dsv, what, z, (UINT8)(zstencil & 0xFF));
        }
    }
}

static void hw_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    HwSurf *t = surface_for(s, 1);
    if (t)
        s_last_target = t;
    HwDepth *d = NULL;
    HwTarget tc;
    HwPixel pc;
    ID3D11ShaderResourceView *srv[4] = { NULL, NULL, NULL, NULL };
    ID3D11SamplerState *smp[4] = { NULL, NULL, NULL, NULL };
    ID3D11PixelShader *rcps = NULL;
    ID3D11Buffer *cbs[2];
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    const Nv2aRenderState *rs = b->state;
    /* Window z as it is, over the largest format's range: SET_CLIP_MIN/MAX
     * only clip (docs/technical/pushbuffer-executor.md), and scaling each
     * draw by them would sort a narrowed layer wrongly. */
    const float depth_scale = 16777215.0f;
    double t_draw = now_ms(), t_up;

    if (!t)
        return;
    g.stats[0]++;
    g.n_draws++;
    if (rs->zeta_va)
        d = depth_for(rs->zeta_va, t->width, t->height);
    /* The register combiners when the title programmed them and the batch
     * carries what they read; stage 0 times diffuse otherwise, and when the
     * combiner program's shader cannot be made. */
    if (b->pixel && (b->program || b->xf))
        rcps = rc_prepare(b->pixel, rs, t, srv, smp);
    if (!rcps && b->texture && b->texture->width && b->texture->height) {
        srv[0] = texture_view(b->texture, t);
        smp[0] = srv[0] ? sampler(b->texture, 0) : NULL;
    }

    memset(&tc, 0, sizeof tc);
    tc.inv_size[0] = 1.0f / (float)t->width;
    tc.inv_size[1] = 1.0f / (float)t->height;
    tc.inv_depth = 1.0f / depth_scale;
    tc.tex_scale[0] = tc.tex_scale[1] = 1.0f;
    if (!rcps && b->texture && b->texture->width && b->texture->height) {
        tc.tex_inv[0] = 1.0f / (float)b->texture->width;
        tc.tex_inv[1] = 1.0f / (float)b->texture->height;
    }
    if (b->program) {
        tc.near_z = b->program->near_z;
        tc.aa[0] = b->program->aa_sx;
        tc.aa[1] = b->program->aa_sy;
        tc.near_clip = b->program->near_clip ? 1.0f : 0.0f;
        tc.tex_scale[0] = b->program->tex0_scale[0];
        tc.tex_scale[1] = b->program->tex0_scale[1];
    }
    if (rcps) {
        tc.fog_on = b->pixel->fog_enable ? 1.0f : 0.0f;
        tc.fog_mode = (float)b->pixel->fog_mode;
        tc.fog_p[0] = b->pixel->fog_param[0];
        tc.fog_p[1] = b->pixel->fog_param[1];
    }
    if (!g.target_valid || memcmp(&g.last_target, &tc, sizeof tc)) {
        ID3D11DeviceContext_UpdateSubresource(g.ctx, (ID3D11Resource *)g.cb_target, 0, NULL, &tc, 0, 0);
        g.last_target = tc;
        g.target_valid = 1;
    }
    if (!rcps) {
        pc.textured = srv[0] != NULL;
        pc.alpha_func = rs->alpha_test_enable ? rs->alpha_func : 0x207;
        pc.alpha_ref = (float)(rs->alpha_ref & 0xFF);
        pc.pad = 0;
        if (!g.pixel_valid || memcmp(&g.last_pixel, &pc, sizeof pc)) {
            ID3D11DeviceContext_UpdateSubresource(g.ctx, (ID3D11Resource *)g.cb_pixel, 0, NULL, &pc, 0, 0);
            g.last_pixel = pc;
            g.pixel_valid = 1;
        }
    }

    ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 1, &t->rtv, d ? d->dsv : NULL);
    vp.TopLeftX = vp.TopLeftY = 0.0f;
    vp.Width = (float)t->width;
    vp.Height = (float)t->height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g.ctx, 1, &vp);
    sc.left = sc.top = 0;
    sc.right = (LONG)t->width;
    sc.bottom = (LONG)t->height;
    ID3D11DeviceContext_RSSetScissorRects(g.ctx, 1, &sc);
    bind_states(rs, d != NULL);
    ID3D11DeviceContext_PSSetShader(g.ctx, rcps ? rcps : g.ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g.ctx, 0, 1, rcps ? &g.cb_rc : &g.cb_pixel);
    ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 4, srv);
    ID3D11DeviceContext_PSSetSamplers(g.ctx, 0, 4, smp);
    ID3D11DeviceContext_IASetPrimitiveTopology(g.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cbs[0] = g.cb_target;
    cbs[1] = g.cb_consts;
    if (rcps)
        g.n_rc++;

    if (b->program) {
        const Nv2aProgramBatch *pb = b->program;
        HwProgram *p = program_for(pb);
        ID3D11VertexShader *vs;
        UINT ninputs = 0, a, stride, voff, ioff;

        if (!p || !pb->index_count)
            return;
        if (!(vs = rcps ? program_vs_rc(p, pb) : p->vs))
            return;
        for (a = 0; a < NV2A_VSH_INPUTS; a++)
            ninputs += (pb->inputs_read >> a) & 1u;
        stride = ninputs * 16;
        t_up = now_ms();
        if (!g.consts_valid || memcmp(g.last_consts, pb->constants, sizeof g.last_consts)) {
            ID3D11DeviceContext_UpdateSubresource(g.ctx, (ID3D11Resource *)g.cb_consts, 0, NULL, pb->constants, 0, 0);
            memcpy(g.last_consts, pb->constants, sizeof g.last_consts);
            g.consts_valid = 1;
        }
        if (ninputs) {
            if (!ring_write(&g.vring, D3D11_BIND_VERTEX_BUFFER, pb->inputs, pb->input_count * stride, &voff))
                return;
            ID3D11DeviceContext_IASetVertexBuffers(g.ctx, 0, 1, &g.vring.buf, &stride, &voff);
        }
        if (!ring_write(&g.iring, D3D11_BIND_INDEX_BUFFER, pb->indices, pb->index_count * 4, &ioff))
            return;
        g.ms_upload += now_ms() - t_up;
        ID3D11DeviceContext_IASetInputLayout(g.ctx, p->layout);
        ID3D11DeviceContext_IASetIndexBuffer(g.ctx, g.iring.buf, DXGI_FORMAT_R32_UINT, 0);
        ID3D11DeviceContext_VSSetShader(g.ctx, vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(g.ctx, 0, 2, cbs);
        ID3D11DeviceContext_DrawIndexed(g.ctx, pb->index_count, ioff / 4, 0);
        g.stats[1]++;
        g.n_programs++;
    } else if (rcps) {
        UINT stride = 32 * (UINT)sizeof(float), voff;

        t_up = now_ms();
        if (!b->count || !ring_write(&g.vring, D3D11_BIND_VERTEX_BUFFER, b->xf, b->count * stride, &voff))
            return;
        g.ms_upload += now_ms() - t_up;
        ID3D11DeviceContext_IASetInputLayout(g.ctx, g.il_xf);
        ID3D11DeviceContext_IASetVertexBuffers(g.ctx, 0, 1, &g.vring.buf, &stride, &voff);
        ID3D11DeviceContext_VSSetShader(g.ctx, g.vs_xf, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(g.ctx, 0, 1, cbs);
        ID3D11DeviceContext_Draw(g.ctx, b->count, 0);
    } else {
        UINT stride = sizeof(Nv2aVertex), voff;

        t_up = now_ms();
        if (!b->count || !ring_write(&g.vring, D3D11_BIND_VERTEX_BUFFER, b->vertices,
                                     b->count * (UINT)sizeof(Nv2aVertex), &voff))
            return;
        g.ms_upload += now_ms() - t_up;
        ID3D11DeviceContext_IASetInputLayout(g.ctx, g.il_pixels);
        ID3D11DeviceContext_IASetVertexBuffers(g.ctx, 0, 1, &g.vring.buf, &stride, &voff);
        ID3D11DeviceContext_VSSetShader(g.ctx, g.vs_pixels, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(g.ctx, 0, 1, cbs);
        ID3D11DeviceContext_Draw(g.ctx, b->count, 0);
    }
    t->dirty = 1;
    g.ms_draw += now_ms() - t_draw;
}

static void hw_flip(void) {}

/* Every 5 s: where the renderer's time went, per frame. Draw time includes the
 * texture, compile and upload times listed after it. */
static void hw_report(void)
{
    double now = now_ms(), span = now - g.report_at;
    double f = g.n_frames ? (double)g.n_frames : 1.0;

    if (!g.report_at) {
        g.report_at = now;
        return;
    }
    if (span < 5000.0)
        return;
    fprintf(stderr, "[HWRENDER] %u frames in %.1f s (%.1f fps); per frame: %.1f draws (%.1f program, %.1f program switches, "
            "%.1f combiner; %u combiner shaders), "
            "%.2f ms in draws (texture hash %.2f over %.1f checks, decode %.2f, uploads %.2f, compile %.2f), "
            "write-back %.2f ms, loads %.2f ms\n",
            g.n_frames, span / 1000.0, g.n_frames * 1000.0 / span, g.n_draws / f, g.n_programs / f, g.n_switches / f,
            g.n_rc / f, g.n_rc_shaders,
            g.ms_draw / f, g.ms_hash / f, g.n_hashes / f, g.ms_decode / f, g.ms_upload / f, g.ms_compile / f,
            g.ms_writeback / f, g.ms_load / f);
    {
        static uint32_t last_hits, last_compiled;
        if (s_cache_hits != last_hits || s_cache_compiled != last_compiled)
            fprintf(stderr, "[HWRENDER] shaders: %u from the disk cache, %u compiled (totals)\n",
                    s_cache_hits, s_cache_compiled);
        last_hits = s_cache_hits;
        last_compiled = s_cache_compiled;
    }
    g.ms_draw = g.ms_hash = g.ms_decode = g.ms_compile = g.ms_upload = g.ms_writeback = g.ms_load = 0.0;
    g.n_frames = g.n_draws = g.n_programs = g.n_hashes = g.n_switches = g.n_rc = 0;
    g.report_at = now;
}

/* Before the window snapshot: everything the frame drew, back in guest
 * memory; the next frame loads its surfaces again. */
static void hw_present_sync(void)
{
    double t0 = now_ms();
    int i;

    /* Last frame's copies are finished on the GPU by now: write them to guest
     * memory without waiting, then queue this frame's. The window and frame
     * dumps show the guest copy, so they run one frame behind.
     *
     * RECOMP_HW_LOW_LATENCY: except the surface drawn last -- the frame the
     * window shows next -- which is written back now, waiting for the GPU,
     * so the window shows this frame and the player sees a move one frame
     * (16.7 ms) sooner. Read every present, so it can be switched live. */
    const char *ll = getenv("RECOMP_HW_LOW_LATENCY");
    int low = ll && *ll && *ll != '0';

    for (i = 0; i < HW_SURFACES; i++) {
        HwSurf *s = &g.surf[i];
        if (!s->tex)
            continue;
        if (low && s == s_last_target) {
            surf_write_back(s);         /* this frame, now */
            continue;
        }
        surf_drain(s, s->next ^ 1);     /* copied at the last present */
        surf_copy(s);
    }
    g.epoch++;
    g.ms_writeback += now_ms() - t0;
    g.n_frames++;
    hw_report();
}

static const Nv2aBackend s_hw_backend = { hw_clear, hw_draw, hw_flip };

/* ---- setup -------------------------------------------------------------- */

static int create_device(int warp)
{
    D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0, got;
    IDXGIAdapter1 *best = NULL;
    HRESULT h;

    if (!warp) {
        IDXGIFactory1 *f = NULL;
        SIZE_T most = 0;
        UINT i;

        if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) {
            IDXGIAdapter1 *a;
            for (i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
                DXGI_ADAPTER_DESC1 d;
                IDXGIAdapter1_GetDesc1(a, &d);
                if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && (!best || d.DedicatedVideoMemory > most)) {
                    if (best) IDXGIAdapter1_Release(best);
                    best = a;
                    most = d.DedicatedVideoMemory;
                } else {
                    IDXGIAdapter1_Release(a);
                }
            }
            IDXGIFactory1_Release(f);
        }
        if (!best)
            return 0;
    }
    h = D3D11CreateDevice((IDXGIAdapter *)best, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN,
                          NULL, 0, &want, 1, D3D11_SDK_VERSION, &g.dev, &got, &g.ctx);
    if (best)
        IDXGIAdapter1_Release(best);
    if (FAILED(h)) {
        hw_log("D3D11CreateDevice", h);
        return 0;
    }
    return 1;
}

static int create_pipeline(void)
{
    static const D3D11_INPUT_ELEMENT_DESC ie[3] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aVertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, offsetof(Nv2aVertex, diffuse), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Nv2aVertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0}};
    /* Nv2aVshOutput: pos, d0, d1, fog, tex[4]. */
    static const D3D11_INPUT_ELEMENT_DESC xe[8] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"FOG", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    char src[8192];
    ID3DBlob *vs, *ps, *xs;
    int ok;

    snprintf(src, sizeof src, "%s%s", k_target_cb, k_vs_pixels);
    vs = compile(src, strlen(src), "vs_5_0");
    ps = compile(k_ps, strlen(k_ps), "ps_5_0");
    snprintf(src, sizeof src, "%s%s%s", k_target_cb, k_vs_rc_out, k_vs_xf);
    xs = compile(src, strlen(src), "vs_5_0");
    ok = vs && ps
      && SUCCEEDED(ID3D11Device_CreateVertexShader(g.dev, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), NULL, &g.vs_pixels))
      && SUCCEEDED(ID3D11Device_CreateInputLayout(g.dev, ie, 3, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), &g.il_pixels))
      && SUCCEEDED(ID3D11Device_CreatePixelShader(g.dev, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), NULL, &g.ps))
      && (g.cb_target = make_cb(sizeof(HwTarget))) != NULL
      && (g.cb_consts = make_cb(NV2A_VSH_CONSTANTS * 16)) != NULL
      && (g.cb_pixel = make_cb(sizeof(HwPixel))) != NULL
      && xs
      && SUCCEEDED(ID3D11Device_CreateVertexShader(g.dev, ID3D10Blob_GetBufferPointer(xs), ID3D10Blob_GetBufferSize(xs), NULL, &g.vs_xf))
      && SUCCEEDED(ID3D11Device_CreateInputLayout(g.dev, xe, 8, ID3D10Blob_GetBufferPointer(xs), ID3D10Blob_GetBufferSize(xs), &g.il_xf))
      && (g.cb_rc = make_cb(sizeof(HwRc))) != NULL;
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
    if (xs) ID3D10Blob_Release(xs);
    return ok;
}

int nv2a_hw_d3d11_register(int warp)
{
    const char *winding = getenv("RECOMP_STRIP_ALT_WINDING");

    /* The GPU culls from the winding, as the console does, and every second
     * triangle of a strip only winds like the first under
     * RECOMP_STRIP_ALT_WINDING (nv2a_pb_exec.c; the CPU rasteriser never
     * culls, so it is off by default there). Unset, it is turned on here,
     * before the executor first reads it; set to 0, a culled strip loses
     * every second triangle -- a full-screen quad drawn as a strip loses a
     * diagonal half. */
    if (!winding)
        _putenv("RECOMP_STRIP_ALT_WINDING=1");
    else if (!strcmp(winding, "0"))
        fprintf(stderr, "[HWRENDER] RECOMP_STRIP_ALT_WINDING=0: culled triangle strips lose every second triangle\n");
    if (!g.dev && (!create_device(warp) || !create_pipeline())) {
        fprintf(stderr, "[HWRENDER] no D3D11 device: the executor keeps the CPU rasteriser\n");
        return 0;
    }
    memset(g.stats, 0, sizeof g.stats);
    nv2a_backend_register_with_present_sync(&s_hw_backend, hw_present_sync);
    nv2a_backend_set_program_batches(1);
    return 1;
}

int xbox_Nv2aHwRendererInstall(void)
{
    const char *e = getenv("RECOMP_HW_RENDER");

    if (!e)
        return 0;
    if (!strcmp(e, "1"))
        return nv2a_hw_d3d11_register(0);
    if (!strcmp(e, "warp"))
        return nv2a_hw_d3d11_register(1);
    return 0;
}

void nv2a_hw_d3d11_stats(uint32_t out[6])
{
    memcpy(out, g.stats, sizeof g.stats);
}

void nv2a_hw_d3d11_shader_cache_counts(uint32_t out[2])
{
    out[0] = s_cache_hits;
    out[1] = s_cache_compiled;
}
