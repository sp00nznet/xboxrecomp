# D3D8 to D3D11 Translation

How to bridge the Xbox's modified Direct3D 8 API to modern DirectX 11 for rendering recompiled game code.

## Overview

The original Xbox uses a custom version of Direct3D 8, modified by Microsoft for the NV2A GPU. It is not identical to PC D3D8 -- it includes Xbox-specific extensions for push buffers, tiled memory, and GPU-specific features. The recompiled game code calls D3D8 methods through COM vtable pointers, and our translation layer intercepts these calls and routes them to D3D11.

## Architecture

```
  Recompiled Game Code
         |
   COM vtable dispatch (RECOMP_ICALL)
         |
   D3D8 Compatibility Layer (d3d8_*.c)
         |
   ┌─────┴─────┐
   │ d3d8_device.c    │  Device state, render states, transforms
   │ d3d8_resources.c │  Vertex/index buffers, textures, surfaces
   │ d3d8_shaders.c   │  Fixed-function pipeline → HLSL shaders
   │ d3d8_states.c    │  State object caching (blend, rasterizer, etc.)
   └─────┬─────┘
         |
   ID3D11Device / ID3D11DeviceContext
         |
   GPU (actual rendering)
```

## COM Vtable Emulation

Xbox D3D8 uses COM interfaces. Game code calls methods through vtable pointers:

```asm
; eax = IDirect3DDevice8 pointer
mov ecx, [eax]          ; ecx = vtable pointer
call [ecx + 0x40]       ; call method at vtable offset 0x40
```

The compatibility layer provides a struct with function pointers matching the IDirect3DDevice8 vtable layout:

```c
// Resource wrapper -- COM interface must be the first member
typedef struct D3D8VertexBuffer {
    IDirect3DVertexBuffer8  iface;      // COM interface (vtable ptr)
    LONG                    ref_count;
    ID3D11Buffer           *d3d11_buffer;
    UINT                    size;
    DWORD                   fvf;
    DWORD                   usage;
    BYTE                   *sys_mem;    // System memory for Lock
    BOOL                    locked;
    BOOL                    dirty;
} D3D8VertexBuffer;

// The vtable is a static const struct of function pointers:
static const IDirect3DDevice8Vtbl g_device_vtbl = {
    .QueryInterface     = Device_QueryInterface,
    .AddRef             = Device_AddRef,
    .Release            = Device_Release,
    .CreateVertexBuffer = Device_CreateVertexBuffer,
    .SetRenderState     = Device_SetRenderState,
    .DrawIndexedPrimitive = Device_DrawIndexedPrimitive,
    // ... 100+ methods
};

// Global device instance (Xbox has a single D3D device)
static IDirect3DDevice8 g_device = { &g_device_vtbl };
```

## Vertex and Index Buffers

D3D8 buffers use a Lock/Unlock pattern. The D3D11 equivalent uses staging buffers:

### Create

```c
HRESULT d3d8_CreateVertexBufferImpl(UINT Length, DWORD Usage, DWORD FVF,
                                     IDirect3DVertexBuffer8 **ppVB) {
    D3D8VertexBuffer *vb = calloc(1, sizeof(D3D8VertexBuffer));
    vb->iface.lpVtbl = &g_vb_vtbl;
    vb->ref_count = 1;
    vb->size = Length;
    vb->fvf = FVF;
    vb->sys_mem = malloc(Length);  // system memory shadow

    // Create D3D11 buffer (initially empty)
    D3D11_BUFFER_DESC desc = {
        .ByteWidth = Length,
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_VERTEX_BUFFER,
    };
    ID3D11Device_CreateBuffer(d3d11_device, &desc, NULL, &vb->d3d11_buffer);

    *ppVB = (IDirect3DVertexBuffer8 *)vb;
    return S_OK;
}
```

### Lock / Unlock Cycle

```c
// Lock: return pointer to system memory shadow
HRESULT VB_Lock(IDirect3DVertexBuffer8 *iface, UINT Offset, UINT Size,
                BYTE **ppbData, DWORD Flags) {
    D3D8VertexBuffer *vb = (D3D8VertexBuffer *)iface;
    *ppbData = vb->sys_mem + Offset;
    vb->locked = TRUE;
    return S_OK;
}

// Unlock: upload system memory to GPU
HRESULT VB_Unlock(IDirect3DVertexBuffer8 *iface) {
    D3D8VertexBuffer *vb = (D3D8VertexBuffer *)iface;
    vb->locked = FALSE;
    vb->dirty = TRUE;

    // Upload to GPU via UpdateSubresource
    ID3D11DeviceContext_UpdateSubresource(context,
        (ID3D11Resource *)vb->d3d11_buffer,
        0, NULL, vb->sys_mem, 0, 0);

    return S_OK;
}
```

## Fixed-Function Pipeline Emulation

D3D8's fixed-function pipeline is configured through render states and texture stage states. D3D11 has no fixed-function pipeline -- everything is programmable. We emulate the common configurations with HLSL shaders.

### Vertex Shader

Handles two modes: standard transform (World * View * Projection) and pre-transformed vertices (D3DFVF_XYZRHW for screen-space HUD rendering):

```hlsl
cbuffer TransformCB : register(b0) {
    float4x4 WorldViewProj;
    float2   ScreenSize;
    uint     Flags;     // bit 0: pre-transformed, bit 1: has diffuse, bit 2: has tex
    float    _pad;
};

struct VS_IN {
    float4 pos     : POSITION;
    float3 normal  : NORMAL;
    float4 diffuse : COLOR0;
    float2 tex0    : TEXCOORD0;
};

VS_OUT main(VS_IN input) {
    VS_OUT o;

    if (Flags & 1u) {
        // Pre-transformed (XYZRHW): convert screen space to NDC
        o.pos.x = (input.pos.x / ScreenSize.x) * 2.0 - 1.0;
        o.pos.y = 1.0 - (input.pos.y / ScreenSize.y) * 2.0;
        o.pos.z = input.pos.z;
        o.pos.w = 1.0;
    } else {
        // Standard transform
        o.pos = mul(float4(input.pos.xyz, 1.0), WorldViewProj);
    }

    // Diffuse color: use vertex color if present, else white
    if (Flags & 2u)
        o.color = input.diffuse.bgra;  // D3DCOLOR is BGRA, swizzle to RGBA
    else
        o.color = float4(1, 1, 1, 1);

    o.tex0 = input.tex0;
    return o;
}
```

### Pixel Shader

Implements the core texture stage operations:

```hlsl
cbuffer PixelCB : register(b0) {
    float4 TexFactor;
    float  AlphaRef;
    uint   Flags;      // bit 0: texture enabled, bit 1: alpha test
    uint   AlphaFunc;
    float  _pad;
};

float4 main(PS_IN input) : SV_TARGET {
    float4 result = input.color;

    if (Flags & 1u) {
        float4 texel = tex0.Sample(samp0, input.tex0);
        result *= texel;  // D3DTOP_MODULATE: texture * vertex color
    }

    // Alpha test
    if (Flags & 2u) {
        // Compare result.a against AlphaRef using AlphaFunc
        // (NEVER, LESS, EQUAL, LESSEQUAL, GREATER, NOTEQUAL, GREATEREQUAL, ALWAYS)
    }

    return result;
}
```

### Texture Stage States

The key D3D8 texture stage states and how they map to shader flags:

| D3D8 State | Value | Shader Behavior |
|------------|-------|-----------------|
| D3DTSS_COLOROP = D3DTOP_MODULATE (4) | texture * vertex color | Pixel shader flag bit 0 = 1 |
| D3DTSS_COLOROP = D3DTOP_DISABLE (1) | vertex color only | Pixel shader flag bit 0 = 0 |
| D3DTSS_COLOROP = D3DTOP_SELECTARG1 (2) | texture only | Pixel shader output = texel |
| D3DTSS_COLOROP = D3DTOP_SELECTARG2 (3) | diffuse only | Pixel shader output = vertex color |

The D3DTOP_MODULATE case is the most common in Burnout 3 -- it multiplies the texture sample with the vertex color, which provides ambient occlusion baked into vertex colors.

## Render State Translation

D3D8 has over 200 render states. Each maps to one or more D3D11 state objects:

### Blend State

```c
// D3D8 render states that map to D3D11 blend state:
// D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
// D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE

D3D11_BLEND_DESC blend_desc = {
    .RenderTarget[0] = {
        .BlendEnable = render_states[D3DRS_ALPHABLENDENABLE],
        .SrcBlend = d3d8_blend_to_d3d11(render_states[D3DRS_SRCBLEND]),
        .DestBlend = d3d8_blend_to_d3d11(render_states[D3DRS_DESTBLEND]),
        .BlendOp = d3d8_blendop_to_d3d11(render_states[D3DRS_BLENDOP]),
        .RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL,
    },
};
ID3D11Device_CreateBlendState(device, &blend_desc, &blend_state);
```

### Depth-Stencil State

```c
// D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_STENCILENABLE, etc.

D3D11_DEPTH_STENCIL_DESC ds_desc = {
    .DepthEnable = render_states[D3DRS_ZENABLE],
    .DepthWriteMask = render_states[D3DRS_ZWRITEENABLE]
                      ? D3D11_DEPTH_WRITE_MASK_ALL
                      : D3D11_DEPTH_WRITE_MASK_ZERO,
    .DepthFunc = d3d8_cmpfunc_to_d3d11(render_states[D3DRS_ZFUNC]),
    // ... stencil state
};
```

### Rasterizer State

```c
// D3DRS_FILLMODE, D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE

D3D11_RASTERIZER_DESC rast_desc = {
    .FillMode = (render_states[D3DRS_FILLMODE] == D3DFILL_WIREFRAME)
                ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID,
    .CullMode = d3d8_cull_to_d3d11(render_states[D3DRS_CULLMODE]),
    .ScissorEnable = render_states[D3DRS_SCISSORTESTENABLE],
    .DepthClipEnable = TRUE,
};
```

## FVF to Input Layouts

D3D8 uses Flexible Vertex Format (FVF) flags to describe vertex data. D3D11 uses input layout objects. The translation:

```c
D3D11_INPUT_ELEMENT_DESC elements[8];
int n = 0;
UINT offset = 0;

if (fvf & D3DFVF_XYZ) {
    elements[n++] = (D3D11_INPUT_ELEMENT_DESC){
        "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offset, ...
    };
    offset += 12;
}
if (fvf & D3DFVF_XYZRHW) {
    elements[n++] = (D3D11_INPUT_ELEMENT_DESC){
        "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offset, ...
    };
    offset += 16;
}
if (fvf & D3DFVF_NORMAL) {
    elements[n++] = (D3D11_INPUT_ELEMENT_DESC){
        "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offset, ...
    };
    offset += 12;
}
if (fvf & D3DFVF_DIFFUSE) {
    elements[n++] = (D3D11_INPUT_ELEMENT_DESC){
        "COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, offset, ...
    };
    offset += 4;
}
// Texture coordinates: (fvf >> 8) & 0xF = number of tex coord sets
```

## Texture Format Translation

Xbox D3D8 uses DXT compressed textures. D3D11 supports these natively as BC formats:

| D3D8 Format | D3D11/DXGI Format | Bits/Pixel | Notes |
|-------------|-------------------|------------|-------|
| D3DFMT_DXT1 | DXGI_FORMAT_BC1_UNORM | 4 | 1-bit alpha |
| D3DFMT_DXT3 | DXGI_FORMAT_BC2_UNORM | 8 | Explicit alpha |
| D3DFMT_DXT5 | DXGI_FORMAT_BC3_UNORM | 8 | Interpolated alpha |
| D3DFMT_A8R8G8B8 | DXGI_FORMAT_B8G8R8A8_UNORM | 32 | Uncompressed |
| D3DFMT_X8R8G8B8 | DXGI_FORMAT_B8G8R8X8_UNORM | 32 | No alpha |
| D3DFMT_R5G6B5 | DXGI_FORMAT_B5G6R5_UNORM | 16 | |
| D3DFMT_A1R5G5B5 | DXGI_FORMAT_B5G5R5A1_UNORM | 16 | |

The format conversion is straightforward because Xbox uses the same byte ordering as PC for these formats.

## DrawPrimitive Translation

### DrawIndexedPrimitive

```c
HRESULT Device_DrawIndexedPrimitive(
    IDirect3DDevice8 *iface,
    D3DPRIMITIVETYPE PrimitiveType,
    UINT MinVertexIndex, UINT NumVertices,
    UINT StartIndex, UINT PrimitiveCount)
{
    // Flush dirty state (blend, rasterizer, depth-stencil)
    flush_state_objects();

    // Update constant buffers (transforms, texture stage states)
    update_transform_cb();
    update_pixel_cb();

    // Set topology
    D3D11_PRIMITIVE_TOPOLOGY topo;
    UINT index_count;
    switch (PrimitiveType) {
    case D3DPT_TRIANGLELIST:
        topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        index_count = PrimitiveCount * 3;
        break;
    case D3DPT_TRIANGLESTRIP:
        topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        index_count = PrimitiveCount + 2;
        break;
    case D3DPT_LINELIST:
        topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        index_count = PrimitiveCount * 2;
        break;
    // ...
    }

    ID3D11DeviceContext_IASetPrimitiveTopology(context, topo);
    ID3D11DeviceContext_DrawIndexed(context, index_count, StartIndex, 0);
    return S_OK;
}
```

### DrawPrimitiveUP (User Pointer)

D3D8's `DrawPrimitiveUP` draws from a CPU-side buffer without a pre-created vertex buffer. D3D11 has no equivalent, so we use a dynamic buffer:

```c
HRESULT Device_DrawPrimitiveUP(
    IDirect3DDevice8 *iface,
    D3DPRIMITIVETYPE PrimitiveType,
    UINT PrimitiveCount,
    const void *pVertexStreamZeroData,
    UINT VertexStreamZeroStride)
{
    UINT vertex_count = /* compute from PrimitiveType + PrimitiveCount */;
    UINT data_size = vertex_count * VertexStreamZeroStride;

    // Upload to a dynamic staging buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11DeviceContext_Map(context, dynamic_vb, 0,
                            D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    memcpy(mapped.pData, pVertexStreamZeroData, data_size);
    ID3D11DeviceContext_Unmap(context, dynamic_vb, 0);

    // Draw
    UINT stride = VertexStreamZeroStride, offset = 0;
    ID3D11DeviceContext_IASetVertexBuffers(context, 0, 1,
                                           &dynamic_vb, &stride, &offset);
    ID3D11DeviceContext_Draw(context, vertex_count, 0);
    return S_OK;
}
```

## Xbox-Specific D3D8 Quirks

### Push Buffers

The Xbox D3D8 includes a NV2A push buffer interface for direct GPU command submission. These functions write to NV2A registers at 0xFD000000+ and spin-wait for GPU responses. In the recompilation:

- Push buffer creation functions are stubbed to return success.
- Any function that spin-waits on GPU registers (`while (MEM32(0xFD00XXXX) & flag)`) must be stubbed entirely, not just the register read. Allocating the page via VEH prevents the crash but the loop spins forever on zero.

### Tiled Memory

Xbox D3D8 supports tiled render targets for the NV2A's tile-based rendering. These are stubbed -- D3D11 handles render targets internally.

### Vertex Shader Differences

Xbox D3D8 vertex shaders use a different instruction set than PC D3D8 (NV2A microcode vs. standard vs_1_1). All Xbox vertex shader programs must be replaced with HLSL equivalents or emulated through the fixed-function path.

## State Object Caching

D3D11 requires creating state objects (blend, rasterizer, depth-stencil) upfront. Creating them every frame is expensive. The translation layer caches state objects keyed by their descriptor:

```c
// Simplified cache (production code uses hash maps)
#define STATE_CACHE_SIZE 64
static struct {
    D3D11_BLEND_DESC desc;
    ID3D11BlendState *state;
} g_blend_cache[STATE_CACHE_SIZE];

ID3D11BlendState *get_or_create_blend_state(const D3D11_BLEND_DESC *desc) {
    for (int i = 0; i < STATE_CACHE_SIZE; i++) {
        if (g_blend_cache[i].state &&
            memcmp(&g_blend_cache[i].desc, desc, sizeof(*desc)) == 0)
            return g_blend_cache[i].state;  // cache hit
    }
    // Cache miss: create new state object
    ID3D11BlendState *state;
    ID3D11Device_CreateBlendState(device, desc, &state);
    // Insert into cache...
    return state;
}
```

## Present and Window Management

The frame pump presents the backbuffer and processes Windows messages:

```c
void d3d8_PresentFrame(void) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) ExitProcess(0);
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (swap_chain)
        IDXGISwapChain_Present(swap_chain, 1, 0);  // VSync = 1
}
```

This is called from the recompiled game's rendering loop, replacing the original Xbox D3D8 Present call.
