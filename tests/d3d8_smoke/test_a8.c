/* Test A8 sampling and transformed-vertex interpolation on D3D11 WARP.
 * No game data, window, or hardware adapter is needed. */
#include "d3d8_internal.h"
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ID3D11Device *device;
static ID3D11DeviceContext *context;
static ID3D11VertexShader *vs;
static ID3D11Buffer *programmable_constants;
static IDirect3DBaseTexture8 *textures[4];
static DWORD states[512], stages[4][64];
static const D3DMATRIX identity = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

ID3D11Device *d3d8_GetD3D11Device(void) { return device; }
ID3D11DeviceContext *d3d8_GetD3D11Context(void) { return context; }
ID3D11RenderTargetView *d3d8_GetDefaultRTV(void) { return NULL; }
IDirect3DDevice8 *xbox_GetD3DDevice(void) { return NULL; }
const DWORD *d3d8_GetPalette(DWORD stage) { (void)stage; return states; }
const DWORD *d3d8_GetRenderStates(void) { return states; }
const DWORD *d3d8_GetTSS(DWORD stage) { return stages[stage]; }
IDirect3DBaseTexture8 *d3d8_GetStageTexture(DWORD stage) { return textures[stage]; }
UINT d3d8_GetBackbufferWidth(void) { return 1; }
UINT d3d8_GetBackbufferHeight(void) { return 1; }
const D3DMATRIX *d3d8_GetTransform(D3DTRANSFORMSTATETYPE type) { (void)type; return &identity; }
const D3DLIGHT8 *d3d8_GetLight(DWORD index) { (void)index; return NULL; }
BOOL d3d8_GetLightEnable(DWORD index) { (void)index; return FALSE; }
const D3DMATERIAL8 *d3d8_GetMaterial(void) { return NULL; }
UINT d3d8_GetNumLights(void) { return 0; }

/* Model successful programmable-VS preparation without Xbox microcode.
 * Bind real D3D11 vertex state so pixel preparation must preserve it. */
BOOL d3d8_vsh_prepare_draw(DWORD handle)
{
    if (handle != 0x10000) return FALSE;
    ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(context, NULL);
    ID3D11DeviceContext_VSSetConstantBuffers(context, 1, 1, &programmable_constants);
    return TRUE;
}

#define REQUIRE(call) do { HRESULT hr = (call); if (FAILED(hr)) { \
    fprintf(stderr, "%s: HRESULT 0x%08lx\n", #call, (unsigned long)hr); exit(1); } } while (0)

/* Keep the real fixed-function VS: varying RHW must affect interpolation,
 * but not the quad's screen position. */
static int check_rhw(ID3D11Texture2D *target, ID3D11Texture2D *readback,
                     ID3D11RenderTargetView *rtv)
{
    struct Vertex { float x, y, z, rhw; DWORD diffuse; float u, v; } vertices[] = {
        {0, 0, 0.5f, 1, 0xFFFFFFFF, 0, 0}, {1, 0, 0.5f, 1, 0xFFFFFFFF, 1, 0},
        {0, 1, 0.5f, 1, 0xFFFFFFFF, 0, 1}, {1, 1, 0.5f, 1, 0xFFFFFFFF, 1, 1}
    };
    static const struct { float left, right; int expected; } cases[] = {
        {1, 1, 128}, {1, 3, 255}, {3, 1, 0}, {2, 2, 128}
    };
    const DWORD row[] = {0xFF000000, 0xFFFFFFFF};
    const float clear[] = {1, 0, 1, 0};
    IDirect3DTexture8 *texture;
    ID3D11ShaderResourceView *srv;
    ID3D11SamplerState *sampler;
    ID3D11Buffer *buffer;
    D3DLOCKED_RECT lock;
    D3D11_BUFFER_DESC bd = {0};
    D3D11_SAMPLER_DESC sd = {0};
    UINT stride = sizeof(vertices[0]), offset = 0;
    unsigned i;
    int failures = 0;

    REQUIRE(d3d8_CreateTextureImpl(2, 2, 1, 0, D3DFMT_LIN_A8R8G8B8, &texture));
    REQUIRE(texture->lpVtbl->LockRect(texture, 0, &lock, NULL, 0));
    memcpy(lock.pBits, row, sizeof(row));
    memcpy((BYTE *)lock.pBits + lock.Pitch, row, sizeof(row));
    REQUIRE(texture->lpVtbl->UnlockRect(texture, 0));
    textures[0] = (IDirect3DBaseTexture8 *)texture;
    srv = d3d8_base_srv(textures[0]);
    ID3D11DeviceContext_PSSetShaderResources(context, 0, 1, &srv);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    REQUIRE(ID3D11Device_CreateSamplerState(device, &sd, &sampler));
    ID3D11DeviceContext_PSSetSamplers(context, 0, 1, &sampler);
    stages[0][D3DTSS_COLOROP] = stages[0][D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
    stages[0][D3DTSS_COLORARG1] = stages[0][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    stages[1][D3DTSS_COLOROP] = D3DTOP_DISABLE;
    bd.ByteWidth = sizeof(vertices);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    REQUIRE(ID3D11Device_CreateBuffer(device, &bd, NULL, &buffer));
    ID3D11DeviceContext_IASetVertexBuffers(context, 0, 1, &buffer, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    d3d8_shaders_prepare_draw(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        BYTE *got;
        vertices[0].rhw = vertices[2].rhw = cases[i].left;
        vertices[1].rhw = vertices[3].rhw = cases[i].right;
        ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource *)buffer, 0, NULL, vertices, 0, 0);
        ID3D11DeviceContext_ClearRenderTargetView(context, rtv, clear);
        ID3D11DeviceContext_Draw(context, 4, 0);
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)readback, (ID3D11Resource *)target);
        REQUIRE(ID3D11DeviceContext_Map(context, (ID3D11Resource *)readback, 0, D3D11_MAP_READ, 0, &mapped));
        got = mapped.pData;
        if (abs(got[0] - cases[i].expected) > 1 || abs(got[1] - cases[i].expected) > 1 ||
            abs(got[2] - cases[i].expected) > 1 || got[3] != 255) {
            fprintf(stderr, "FAIL RHW=%g:%g expected=%d RGBA=%u,%u,%u,%u\n",
                cases[i].left, cases[i].right, cases[i].expected, got[0], got[1], got[2], got[3]);
            failures++;
        }
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)readback, 0);
    }
    textures[0] = NULL;
    srv = NULL;
    ID3D11DeviceContext_PSSetShaderResources(context, 0, 1, &srv);
    texture->lpVtbl->Release(texture);
    ID3D11Buffer_Release(buffer);
    ID3D11SamplerState_Release(sampler);
    printf("d3d8_rhw: %d failures (4 draws)\n", failures);
    return failures;
}

int main(void)
{
    static const char vs_source[] =
        "struct V { float4 p:SV_POSITION; float4 c:COLOR0; float4 s:COLOR1;"
        " float3 t0:TEXCOORD0; float3 t1:TEXCOORD1; float3 t2:TEXCOORD2;"
        " float3 t3:TEXCOORD3; float f:TEXCOORD4; float4 v:TEXCOORD5; };"
        "V main(uint id:SV_VertexID) { V o=(V)0;"
        " o.p=float4(id==2?3:-1,id==1?3:-1,0,1);"
        " o.c=1; o.t0=o.t1=o.t2=o.t3=float3(0.5,0.5,0.5); return o; }";
    static const D3DFORMAT formats[] = {D3DFMT_A8, D3DFMT_LIN_A8, D3DFMT_LIN_A8R8G8B8};
    static const BYTE alphas[] = {0, 1, 127, 255};
    ID3D11Texture2D *target, *readback;
    ID3D11RenderTargetView *rtv;
    ID3D11SamplerState *sampler;
    ID3D11RasterizerState *rasterizer;
    ID3DBlob *blob;
    D3D11_TEXTURE2D_DESC td = {0};
    D3D11_SAMPLER_DESC sd = {0};
    D3D11_RASTERIZER_DESC rd = {0};
    D3D11_BUFFER_DESC cbd = {0};
    D3D11_VIEWPORT viewport = {0, 0, 1, 1, 0, 1};
    unsigned f, a, stage, path, c, kind;
    int failures = 0;

    REQUIRE(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
        D3D11_SDK_VERSION, &device, NULL, &context));
    REQUIRE(d3d8_shaders_init());
    REQUIRE(d3d8_combiners_init());
    REQUIRE(D3DCompile(vs_source, sizeof(vs_source), NULL, NULL, NULL, "main",
        "vs_4_0", 0, 0, &blob, NULL));
    REQUIRE(ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &vs));
    ID3D10Blob_Release(blob);
    cbd.ByteWidth = 16;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    REQUIRE(ID3D11Device_CreateBuffer(device, &cbd, NULL, &programmable_constants));
    td.Width = td.Height = td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    REQUIRE(ID3D11Device_CreateTexture2D(device, &td, NULL, &target));
    REQUIRE(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)target, NULL, &rtv));
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    REQUIRE(ID3D11Device_CreateTexture2D(device, &td, NULL, &readback));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MinLOD = 1;  /* Exercise the last mip of each 2x2 (or 2x2x2) texture. */
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    REQUIRE(ID3D11Device_CreateSamplerState(device, &sd, &sampler));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    REQUIRE(ID3D11Device_CreateRasterizerState(device, &rd, &rasterizer));
    ID3D11DeviceContext_RSSetState(context, rasterizer);
    ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, NULL);
    failures += check_rhw(target, readback, rtv);

    for (kind = 0; kind < 3; kind++)
    for (stage = 0; stage < 4; stage++) {
        for (c = 0; c < 4; c++) {
            stages[c][D3DTSS_COLOROP] = D3DTOP_SELECTARG1;
            stages[c][D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
            stages[c][D3DTSS_COLORARG1] = c == stage ? D3DTA_TEXTURE : D3DTA_CURRENT;
            stages[c][D3DTSS_ALPHAARG1] = c == stage ? D3DTA_TEXTURE : D3DTA_CURRENT;
        }
        /* Final combiner: D.rgb and G.a select the sampled texture. */
        states[D3DRS_PSFINALCOMBINERINPUTSABCD] = (NV2A_REG_T0 + stage) << 24;
        states[D3DRS_PSFINALCOMBINERINPUTSEFG] = (NV2A_REG_T0 + stage) << 16;
        d3d8_combiners_mark_dirty();
        UINT mode = kind == 1 ? NV2A_TEXMODE_CUBEMAP : kind == 2 ? NV2A_TEXMODE_3D : NV2A_TEXMODE_2D;
        d3d8_combiners_set_pixel_shader(1u | (mode << (8 + 4 * stage)));
        for (f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
            IDirect3DBaseTexture8 *tex;
            ID3D11ShaderResourceView *srv;
            if (kind == 1) {
                REQUIRE(d3d8_CreateCubeTextureImpl(2, 2, 0, formats[f], (IDirect3DCubeTexture8 **)&tex));
            } else if (kind == 2) {
                REQUIRE(d3d8_CreateVolumeTextureImpl(2, 2, 2, 2, 0, formats[f], (IDirect3DVolumeTexture8 **)&tex));
            } else {
                REQUIRE(d3d8_CreateTextureImpl(2, 2, 2, 0, formats[f], (IDirect3DTexture8 **)&tex));
            }
            textures[stage] = tex;
            srv = d3d8_base_srv(textures[stage]);
            ID3D11DeviceContext_PSSetShaderResources(context, stage, 1, &srv);
            ID3D11DeviceContext_PSSetSamplers(context, stage, 1, &sampler);
            for (a = 0; a < sizeof(alphas); a++) {
                BYTE pixel[4] = {31, 63, 95, alphas[a]};
                BYTE expected[4] = {255, 255, 255, alphas[a]};
                UINT face;
                if (f == 2) {
                    expected[0] = 95; expected[1] = 63; expected[2] = 31;
                } else {
                    pixel[0] = alphas[a];
                }
                for (face = 0; face < (kind == 1 ? 6u : 1u); face++) {
                    D3DLOCKED_RECT lock;
                    D3DLOCKED_BOX box;
                    IDirect3DCubeTexture8 *cube = (IDirect3DCubeTexture8 *)tex;
                    IDirect3DVolumeTexture8 *volume = (IDirect3DVolumeTexture8 *)tex;
                    IDirect3DTexture8 *flat = (IDirect3DTexture8 *)tex;
                    if (kind == 1) {
                        REQUIRE(cube->lpVtbl->LockRect(cube, face, 1, &lock, NULL, 0));
                    } else if (kind == 2) {
                        REQUIRE(volume->lpVtbl->LockBox(volume, 1, &box, NULL, 0));
                        lock.pBits = box.pBits;
                    } else {
                        REQUIRE(flat->lpVtbl->LockRect(flat, 1, &lock, NULL, 0));
                    }
                    memcpy(lock.pBits, pixel, f == 2 ? 4 : 1);
                    if (kind == 1) REQUIRE(cube->lpVtbl->UnlockRect(cube, face, 1));
                    else if (kind == 2) REQUIRE(volume->lpVtbl->UnlockBox(volume, 1));
                    else REQUIRE(flat->lpVtbl->UnlockRect(flat, 1));
                }
                for (path = 0; path < (kind == 0 ? 3u : 2u); path++) {
                    D3D11_MAPPED_SUBRESOURCE mapped;
                    if (path == 2) {
                        D3D8Texture previous = *(D3D8Texture *)tex;
                        ID3D11VertexShader *bound_vs;
                        ID3D11Buffer *bound_cb;
                        ID3D11InputLayout *bound_layout;
                        /* Seed an FFP draw with the opposite format, then
                         * switch texture and vertex shader before the draw. */
                        previous.d3d8_format = f == 2 ? D3DFMT_A8 : D3DFMT_LIN_A8R8G8B8;
                        textures[stage] = (IDirect3DBaseTexture8 *)&previous;
                        d3d8_shaders_prepare_draw(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
                        textures[stage] = tex;
                        d3d8_shaders_prepare_draw(0x10000);
                        ID3D11DeviceContext_VSGetShader(context, &bound_vs, NULL, NULL);
                        ID3D11DeviceContext_VSGetConstantBuffers(context, 1, 1, &bound_cb);
                        ID3D11DeviceContext_IAGetInputLayout(context, &bound_layout);
                        if (bound_vs != vs || bound_cb != programmable_constants || bound_layout) {
                            fprintf(stderr, "FAIL programmable vertex state overwritten\n");
                            failures++;
                        }
                        if (bound_vs) ID3D11VertexShader_Release(bound_vs);
                        if (bound_cb) ID3D11Buffer_Release(bound_cb);
                        if (bound_layout) ID3D11InputLayout_Release(bound_layout);
                    } else {
                        d3d8_shaders_prepare_draw(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
                        if (path && !d3d8_combiners_prepare_draw()) return 1;
                        ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);
                        ID3D11DeviceContext_IASetInputLayout(context, NULL);
                    }
                    ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    ID3D11DeviceContext_Draw(context, 3, 0);
                    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)readback, (ID3D11Resource *)target);
                    REQUIRE(ID3D11DeviceContext_Map(context, (ID3D11Resource *)readback, 0, D3D11_MAP_READ, 0, &mapped));
                    if (memcmp(mapped.pData, expected, 4)) {
                        BYTE *got = mapped.pData;
                        fprintf(stderr, "FAIL kind=%u stage=%u format=0x%x alpha=%u path=%s: RGBA=%u,%u,%u,%u\n",
                            kind, stage, formats[f], alphas[a], path == 2 ? "programmable-VS" : path ? "combiner" : "fixed",
                            got[0], got[1], got[2], got[3]);
                        failures++;
                    }
                    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)readback, 0);
                }
            }
            textures[stage] = NULL;
            srv = NULL;
            ID3D11DeviceContext_PSSetShaderResources(context, stage, 1, &srv);
            tex->lpVtbl->Release(tex);
        }
    }
    ID3D11DeviceContext_ClearState(context);
    d3d8_combiners_shutdown();
    d3d8_shaders_shutdown();
    ID3D11RasterizerState_Release(rasterizer);
    ID3D11SamplerState_Release(sampler);
    ID3D11VertexShader_Release(vs);
    ID3D11Buffer_Release(programmable_constants);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(target);
    ID3D11Texture2D_Release(readback);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    printf("d3d8_a8: %d failures (336 A8 + 4 RHW draws)\n", failures);
    return failures != 0;
}
