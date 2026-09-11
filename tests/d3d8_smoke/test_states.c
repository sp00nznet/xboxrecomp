/* Verify the real state cache through D3D11 WARP, without a window or game. */
#include "d3d8_internal.h"
#include <stdio.h>
#include <string.h>

static ID3D11Device *device;
static ID3D11DeviceContext *context;
static DWORD rs[256];
static int failures;

ID3D11Device *d3d8_GetD3D11Device(void) { return device; }
ID3D11DeviceContext *d3d8_GetD3D11Context(void) { return context; }
const DWORD *d3d8_GetRenderStates(void) { return rs; }
const DWORD *d3d8_GetTSS(DWORD stage) { (void)stage; return NULL; }

#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

static D3D11_DEPTH_STENCIL_DESC apply(void)
{
    D3D11_DEPTH_STENCIL_DESC desc;
    ID3D11DepthStencilState *state = NULL;
    UINT ref = 0;
    memset(&desc, 0, sizeof(desc));
    d3d8_states_apply();
    ID3D11DeviceContext_OMGetDepthStencilState(context, &state, &ref);
    CHECK("depth/stencil state bound", state != NULL);
    CHECK("stencil reference bound", ref == rs[D3DRS_STENCILREF]);
    if (state) {
        ID3D11DepthStencilState_GetDesc(state, &desc);
        ID3D11DepthStencilState_Release(state);
    }
    return desc;
}

static void check_blend_factors(void)
{
    static const struct { DWORD guest; D3D11_BLEND rgb, alpha; } cases[] = {
        {D3DBLEND_SRCALPHA, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_SRC_ALPHA},
        {D3DBLEND_SRCCOLOR, D3D11_BLEND_SRC_COLOR, D3D11_BLEND_SRC_ALPHA},
        {D3DBLEND_INVSRCCOLOR, D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_INV_SRC_ALPHA},
        {D3DBLEND_DESTCOLOR, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_DEST_ALPHA},
        {D3DBLEND_INVDESTCOLOR, D3D11_BLEND_INV_DEST_COLOR, D3D11_BLEND_INV_DEST_ALPHA},
        {D3DBLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ONE},
        {D3DBLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO}
    };
    unsigned side, i;
    rs[D3DRS_ALPHABLENDENABLE] = TRUE;
    for (side = 0; side < 2; side++) {
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            ID3D11BlendState *state = NULL;
            D3D11_BLEND_DESC desc;
            rs[D3DRS_SRCBLEND] = side ? D3DBLEND_ONE : cases[i].guest;
            rs[D3DRS_DESTBLEND] = side ? cases[i].guest : D3DBLEND_ZERO;
            d3d8_states_apply();
            ID3D11DeviceContext_OMGetBlendState(context, &state, NULL, NULL);
            CHECK("blend state bound", state != NULL);
            if (!state) continue;
            ID3D11BlendState_GetDesc(state, &desc);
            /* A failed creation can leave the previous state bound. */
            if (!desc.RenderTarget[0].BlendEnable ||
                desc.RenderTarget[0].SrcBlend != (side ? D3D11_BLEND_ONE : cases[i].rgb) ||
                desc.RenderTarget[0].DestBlend != (side ? cases[i].rgb : D3D11_BLEND_ZERO) ||
                desc.RenderTarget[0].SrcBlendAlpha != (side ? D3D11_BLEND_ONE : cases[i].alpha) ||
                desc.RenderTarget[0].DestBlendAlpha != (side ? cases[i].alpha : D3D11_BLEND_ZERO)) {
                printf("FAIL: blend side=%u factor=%lu\n", side, cases[i].guest);
                failures++;
            }
            ID3D11BlendState_Release(state);
        }
    }
}

int main(void)
{
    D3D11_DEPTH_STENCIL_DESC desc;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
        NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D11CreateDevice(WARP) failed: 0x%08lX\n", hr);
        return 1;
    }

    rs[D3DRS_ZENABLE] = TRUE;
    rs[D3DRS_ZWRITEENABLE] = TRUE;
    rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    rs[D3DRS_STENCILENABLE] = TRUE;
    rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_STENCILMASK] = 0xFF;
    rs[D3DRS_STENCILWRITEMASK] = 0xFF;
    rs[D3DRS_STENCILFAIL] = 1;
    rs[D3DRS_STENCILZFAIL] = 1;
    rs[D3DRS_STENCILPASS] = 1;
    rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    rs[D3DRS_COLORWRITEENABLE] = 0xF;
    rs[D3DRS_CULLMODE] = D3DCULL_NONE;
    rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
    d3d8_states_init();
    desc = apply();
    CHECK("initial write mask", desc.StencilWriteMask == 0xFF);

    rs[D3DRS_STENCILWRITEMASK] = 0x5A;
    desc = apply();
    CHECK("STENCILWRITEMASK", desc.StencilWriteMask == 0x5A);
    rs[D3DRS_STENCILFAIL] = 2;
    desc = apply();
    CHECK("STENCILFAIL", desc.FrontFace.StencilFailOp == D3D11_STENCIL_OP_ZERO &&
        desc.BackFace.StencilFailOp == D3D11_STENCIL_OP_ZERO);
    rs[D3DRS_STENCILZFAIL] = 3;
    desc = apply();
    CHECK("STENCILZFAIL", desc.FrontFace.StencilDepthFailOp == D3D11_STENCIL_OP_REPLACE &&
        desc.BackFace.StencilDepthFailOp == D3D11_STENCIL_OP_REPLACE);
    rs[D3DRS_STENCILPASS] = 6;
    desc = apply();
    CHECK("STENCILPASS", desc.FrontFace.StencilPassOp == D3D11_STENCIL_OP_INVERT &&
        desc.BackFace.StencilPassOp == D3D11_STENCIL_OP_INVERT);

    /* These changes cancel in the old hash: (16 << 14) == (1 << 18). */
    rs[D3DRS_STENCILREF] = 16;
    rs[D3DRS_STENCILMASK] = 0xFE;
    desc = apply();
    CHECK("reference/read-mask hash collision", desc.StencilReadMask == 0xFE);
    rs[D3DRS_STENCILREF] = 17;
    desc = apply();
    CHECK("reference-only update preserves descriptor", desc.StencilReadMask == 0xFE);

    rs[D3DRS_ZWRITEENABLE] = FALSE;
    desc = apply();
    CHECK("ZWRITEENABLE", desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO);
    rs[D3DRS_ZFUNC] = D3DCMP_GREATER;
    desc = apply();
    CHECK("ZFUNC", desc.DepthFunc == D3D11_COMPARISON_GREATER);
    rs[D3DRS_ZENABLE] = FALSE;
    desc = apply();
    CHECK("ZENABLE", desc.DepthEnable == FALSE);
    rs[D3DRS_STENCILFUNC] = D3DCMP_EQUAL;
    desc = apply();
    CHECK("STENCILFUNC", desc.FrontFace.StencilFunc == D3D11_COMPARISON_EQUAL &&
        desc.BackFace.StencilFunc == D3D11_COMPARISON_EQUAL);
    rs[D3DRS_STENCILENABLE] = FALSE;
    desc = apply();
    CHECK("STENCILENABLE", desc.StencilEnable == FALSE);
    rs[D3DRS_STENCILENABLE] = TRUE;
    desc = apply();

    d3d8_states_shutdown();
    ID3D11DeviceContext_ClearState(context);
    d3d8_states_init();
    desc = apply();
    CHECK("cache recreates after shutdown", desc.StencilWriteMask == 0x5A);
    check_blend_factors();
    d3d8_states_shutdown();
    ID3D11DeviceContext_ClearState(context);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    printf("d3d8_states_smoke: %d FAILURE(S)\n", failures);
    return failures ? 1 : 0;
}
