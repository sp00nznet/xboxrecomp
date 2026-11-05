/**
 * NV2A Vertex Shader Microcode to HLSL Translator - Implementation
 *
 * Translates NV2A 128-bit vertex shader microcode instructions into
 * HLSL vertex shader source, compiles them, and caches the results.
 *
 * The translation pipeline is:
 *   1. Parse: 128-bit instruction words -> NV2AVshInstruction structs
 *   2. Analyze: determine which input registers (v0-v15) are read
 *   3. Generate HLSL: emit HLSL code mapping NV2A ops to HLSL intrinsics
 *   4. Compile: D3DCompile -> ID3D11VertexShader
 *   5. Cache: hash microcode -> reuse compiled shader on subsequent draws
 *
 * The generated HLSL uses:
 *   - cbuffer at b1: 192 float4 constants (c0-c191)
 *   - Input semantics: ATTR0-ATTR15 mapped to v0-v15
 *   - Output semantics: SV_POSITION, COLOR0/1, TEXCOORD0-3, FOG, PSIZE
 */

#include "d3d8_internal.h"
#include "d3d8_vsh.h"
#include <d3dcompiler.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#pragma comment(lib, "d3dcompiler.lib")

/* ================================================================
 * Module State
 * ================================================================ */

/* Stored shader programs */
static NV2AVshSlot g_vsh_slots[NV2A_VS_MAX_SLOTS];
static int g_vsh_slot_count = 0;

/* Constant registers (192 float4) */
static NV2AVSConstants g_vsh_constants;
static BOOL g_vsh_constants_dirty = TRUE;

/* D3D11 constant buffer for VS constants */
static ID3D11Buffer *g_vsh_cb = NULL;

/* Shader cache: maps microcode hash to compiled shader + input layout */
typedef struct {
    uint32_t            hash;
    int                 in_use;
    ID3D11VertexShader *vs;
    ID3DBlob           *vs_blob;      /* Bytecode for input layout creation */
    ID3D11InputLayout  *layouts[16];  /* Cached layouts per input mask subset */
    uint32_t            layout_masks[16];  /* inputs_read | (texcoord-field<<16) */
    int                 layout_count;
    uint16_t            inputs_read;  /* Which v registers are read */
} VshCacheEntry;

static VshCacheEntry g_vsh_cache[NV2A_VS_CACHE_SIZE];

/* ================================================================
 * Hash Function (FNV-1a)
 * ================================================================ */

static uint32_t fnv1a_hash(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811c9dc5;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193;
    }
    return h;
}

/* ================================================================
 * Input Layout Management
 *
 * When using a programmable VS, we need an input layout that matches
 * the shader's declared inputs. The layout maps vertex buffer elements
 * to the ATTR# semantics declared in the generated HLSL.
 *
 * The mapping from NV2A v# registers to vertex data depends on the
 * game's vertex stream setup. We use a simple mapping:
 *
 *   v0  -> ATTR0  -> POSITION (float4, offset 0)
 *   v1  -> ATTR1  -> BLENDWEIGHT (float4)
 *   v2  -> ATTR2  -> NORMAL (float4)
 *   v3  -> ATTR3  -> DIFFUSE (float4 / D3DCOLOR)
 *   v4  -> ATTR4  -> SPECULAR (float4 / D3DCOLOR)
 *   v5  -> ATTR5  -> FOG (float4)
 *   v6  -> ATTR6  -> POINTSIZE / BACKDIFFUSE (float4)
 *   v7  -> ATTR7  -> BACKSPECULAR (float4)
 *   v8  -> ATTR8  -> TEXCOORD0 (float4)
 *   v9  -> ATTR9  -> TEXCOORD1 (float4)
 *   v10 -> ATTR10 -> TEXCOORD2 (float4)
 *   v11 -> ATTR11 -> TEXCOORD3 (float4)
 *   v12-v15 -> ATTR12-15 -> additional
 *
 * The actual format (float2/3/4, D3DCOLOR, etc.) is determined at
 * draw time from the active stream source FVF/stride. For now, we
 * create a layout assuming the standard Xbox vertex attribute mapping.
 * ================================================================ */

/**
 * Default format for each input register.
 * This is the common Xbox convention; games may vary.
 * Texcoord registers 8-11 use the FVF-declared size (float1/2/3/4).
 */
static UINT vsh_fvf_texcoord_size(DWORD fvf, UINT t)
{
    DWORD field = (fvf >> (16 + t * 2)) & 0x3;
    return field == 0 ? 2 : field;
}

static DXGI_FORMAT default_input_format(int vreg, DWORD fvf)
{
    switch (vreg) {
    case 0:  return DXGI_FORMAT_R32G32B32_FLOAT;    /* Position (xyz) */
    case 1:  return DXGI_FORMAT_R32G32B32A32_FLOAT;  /* Blend weights */
    case 2:  return DXGI_FORMAT_R32G32B32_FLOAT;     /* Normal */
    case 3:  return DXGI_FORMAT_R8G8B8A8_UNORM;      /* Diffuse (D3DCOLOR) */
    case 4:  return DXGI_FORMAT_R8G8B8A8_UNORM;      /* Specular (D3DCOLOR) */
    case 5:  return DXGI_FORMAT_R32_FLOAT;            /* Fog */
    case 6:  return DXGI_FORMAT_R32_FLOAT;            /* Point size */
    case 7:  return DXGI_FORMAT_R8G8B8A8_UNORM;      /* Back specular */
    case 8:  return vsh_fvf_texcoord_size(fvf, 0) == 1 ? DXGI_FORMAT_R32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 0) == 3 ? DXGI_FORMAT_R32G32B32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 0) == 4 ? DXGI_FORMAT_R32G32B32A32_FLOAT
                 : DXGI_FORMAT_R32G32_FLOAT;          /* Texcoord 0 */
    case 9:  return vsh_fvf_texcoord_size(fvf, 1) == 1 ? DXGI_FORMAT_R32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 1) == 3 ? DXGI_FORMAT_R32G32B32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 1) == 4 ? DXGI_FORMAT_R32G32B32A32_FLOAT
                 : DXGI_FORMAT_R32G32_FLOAT;          /* Texcoord 1 */
    case 10: return vsh_fvf_texcoord_size(fvf, 2) == 1 ? DXGI_FORMAT_R32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 2) == 3 ? DXGI_FORMAT_R32G32B32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 2) == 4 ? DXGI_FORMAT_R32G32B32A32_FLOAT
                 : DXGI_FORMAT_R32G32_FLOAT;          /* Texcoord 2 */
    case 11: return vsh_fvf_texcoord_size(fvf, 3) == 1 ? DXGI_FORMAT_R32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 3) == 3 ? DXGI_FORMAT_R32G32B32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 3) == 4 ? DXGI_FORMAT_R32G32B32A32_FLOAT
                 : DXGI_FORMAT_R32G32_FLOAT;          /* Texcoord 3 */
    default: return DXGI_FORMAT_R32G32B32A32_FLOAT;   /* Generic */
    }
}

static UINT input_format_size(DXGI_FORMAT fmt)
{
    switch (fmt) {
    case DXGI_FORMAT_R32_FLOAT:            return 4;
    case DXGI_FORMAT_R32G32_FLOAT:         return 8;
    case DXGI_FORMAT_R32G32B32_FLOAT:      return 12;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:   return 16;
    case DXGI_FORMAT_R8G8B8A8_UNORM:       return 4;
    default:                                return 16;
    }
}

static ID3D11InputLayout *create_vsh_input_layout(
    uint16_t inputs_read, DWORD fvf, ID3DBlob *vs_blob)
{
    D3D11_INPUT_ELEMENT_DESC elems[NV2A_VS_MAX_INPUTS];
    UINT elem_count = 0;
    UINT offset = 0;
    ID3D11InputLayout *layout = NULL;
    HRESULT hr;
    int i;

    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (!(inputs_read & (1u << i)))
            continue;

        DXGI_FORMAT fmt = default_input_format(i, fvf);

        elems[elem_count].SemanticName      = "ATTR";
        elems[elem_count].SemanticIndex      = (UINT)i;
        elems[elem_count].Format             = fmt;
        elems[elem_count].InputSlot          = 0;
        elems[elem_count].AlignedByteOffset  = offset;
        elems[elem_count].InputSlotClass     = D3D11_INPUT_PER_VERTEX_DATA;
        elems[elem_count].InstanceDataStepRate = 0;
        elem_count++;

        offset += input_format_size(fmt);
    }

    if (elem_count == 0) return NULL;

    hr = ID3D11Device_CreateInputLayout(
        d3d8_GetD3D11Device(),
        elems, elem_count,
        ID3D10Blob_GetBufferPointer(vs_blob),
        ID3D10Blob_GetBufferSize(vs_blob),
        &layout);

    if (FAILED(hr)) {
        fprintf(stderr, "D3D8 VSH: CreateInputLayout failed: 0x%08lX\n", hr);
        return NULL;
    }

    return layout;
}

/* ================================================================
 * Input Layouts from NV2A Vertex Attribute Formats
 *
 * The pushbuffer describes each of the 16 attribute streams itself
 * (SET_VERTEX_DATA_ARRAY_FORMAT: type, size, stride), so a backend that
 * draws from the decoded NV2A state needs no FVF. Each attribute gets its
 * own input slot, so the streams keep their own strides and offsets and no
 * CPU gather is needed. The formats D3D11 has no exact match for are
 * expanded to float4 on the CPU, which d3d8_vsh_nv2a_attr_format reports.
 * ================================================================ */

DXGI_FORMAT d3d8_vsh_nv2a_attr_format(uint32_t type, uint32_t size,
                                      int *expand)
{
    *expand = 0;
    switch (type) {
    case NV2A_VTX_D3DCOLOR:
        /* 0xAARRGGBB: B,G,R,A in memory, read as R,G,B,A. */
        if (size == 4)
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
    case NV2A_VTX_FLOAT:
        switch (size) {
        case 1: return DXGI_FORMAT_R32_FLOAT;
        case 2: return DXGI_FORMAT_R32G32_FLOAT;
        case 3: return DXGI_FORMAT_R32G32B32_FLOAT;
        case 4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
        break;
    case NV2A_VTX_UBYTE:
        switch (size) {
        case 1: return DXGI_FORMAT_R8_UNORM;
        case 2: return DXGI_FORMAT_R8G8_UNORM;
        case 4: return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
        break;
    case NV2A_VTX_S1:
        /* SNORM maps -32768 to -1 as well, like the CPU path's clamp. */
        switch (size) {
        case 1: return DXGI_FORMAT_R16_SNORM;
        case 2: return DXGI_FORMAT_R16G16_SNORM;
        case 4: return DXGI_FORMAT_R16G16B16A16_SNORM;
        }
        break;
    default:
        /* S32K (shorts used as is: DXGI has no SSCALED) and CMP (11:11:10
         * signed normalised) have no DXGI format at all. */
        break;
    }
    *expand = 1;
    return DXGI_FORMAT_R32G32B32A32_FLOAT;
}

int d3d8_vsh_nv2a_input_layout(uint16_t inputs_read,
                               const uint32_t type[NV2A_VS_MAX_INPUTS],
                               const uint32_t size[NV2A_VS_MAX_INPUTS],
                               D3D11_INPUT_ELEMENT_DESC out[NV2A_VS_MAX_INPUTS])
{
    int i, n = 0;

    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        int expand;
        if (!(inputs_read & (1u << i)))
            continue;
        out[n].SemanticName         = "ATTR";
        out[n].SemanticIndex        = (UINT)i;
        /* A stream the title did not enable reads its SET_VERTEX_DATA
         * value: one float4 in a stride-0 buffer. */
        out[n].Format               = size[i]
            ? d3d8_vsh_nv2a_attr_format(type[i], size[i], &expand)
            : DXGI_FORMAT_R32G32B32A32_FLOAT;
        out[n].InputSlot            = (UINT)i;
        out[n].AlignedByteOffset    = 0;
        out[n].InputSlotClass       = D3D11_INPUT_PER_VERTEX_DATA;
        out[n].InstanceDataStepRate = 0;
        n++;
    }
    return n;
}

/* ================================================================
 * Shader Compilation and Caching
 * ================================================================ */

static VshCacheEntry *cache_lookup(uint32_t hash)
{
    int idx = (int)(hash % NV2A_VS_CACHE_SIZE);
    int i;
    for (i = 0; i < NV2A_VS_CACHE_SIZE; i++) {
        int probe = (idx + i) % NV2A_VS_CACHE_SIZE;
        if (!g_vsh_cache[probe].in_use)
            return NULL;
        if (g_vsh_cache[probe].hash == hash)
            return &g_vsh_cache[probe];
    }
    return NULL;
}

static VshCacheEntry *cache_insert(uint32_t hash)
{
    int idx = (int)(hash % NV2A_VS_CACHE_SIZE);
    int i;

    /* Find an empty slot or reuse the probed slot */
    for (i = 0; i < NV2A_VS_CACHE_SIZE; i++) {
        int probe = (idx + i) % NV2A_VS_CACHE_SIZE;
        if (!g_vsh_cache[probe].in_use) {
            g_vsh_cache[probe].hash   = hash;
            g_vsh_cache[probe].in_use = 1;
            return &g_vsh_cache[probe];
        }
    }

    /* Cache full: evict the first probed entry */
    {
        VshCacheEntry *evict = &g_vsh_cache[idx];
        int j;

        if (evict->vs)
            ID3D11VertexShader_Release(evict->vs);
        if (evict->vs_blob)
            ID3D10Blob_Release(evict->vs_blob);
        for (j = 0; j < evict->layout_count; j++) {
            if (evict->layouts[j])
                ID3D11InputLayout_Release(evict->layouts[j]);
        }
        memset(evict, 0, sizeof(*evict));
        evict->hash   = hash;
        evict->in_use = 1;
        return evict;
    }
}

/**
 * Compile a vertex shader from microcode.
 *
 * Parses, generates HLSL, compiles, and caches the result.
 * Returns the cache entry (with compiled VS and blob).
 */
static VshCacheEntry *compile_shader(const DWORD *microcode, int num_insns,
                                      uint32_t hash)
{
    NV2AVshProgram program;
    char hlsl_buf[16384];  /* 16KB should be enough for any VS */
    int hlsl_len;
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr;
    VshCacheEntry *entry;

    /* Parse microcode */
    d3d8_vsh_parse((const uint32_t *)microcode, num_insns, &program);

    /* Generate HLSL */
    hlsl_len = d3d8_vsh_generate_hlsl(&program, hlsl_buf, sizeof(hlsl_buf));
    if (hlsl_len <= 0) {
        fprintf(stderr, "D3D8 VSH: HLSL generation failed\n");
        return NULL;
    }

    /* Compile HLSL to bytecode */
    hr = D3DCompile(hlsl_buf, (SIZE_T)hlsl_len, "nv2a_vsh",
                    NULL, NULL, "main", "vs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &code, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8 VSH: Compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "unknown");
        fprintf(stderr, "--- Generated HLSL ---\n%s\n--- End ---\n", hlsl_buf);
        if (errors) ID3D10Blob_Release(errors);
        return NULL;
    }
    if (errors) ID3D10Blob_Release(errors);

    /* Insert into cache */
    entry = cache_insert(hash);
    if (!entry) {
        ID3D10Blob_Release(code);
        return NULL;
    }

    /* Create D3D11 vertex shader */
    hr = ID3D11Device_CreateVertexShader(
        d3d8_GetD3D11Device(),
        ID3D10Blob_GetBufferPointer(code),
        ID3D10Blob_GetBufferSize(code),
        NULL, &entry->vs);

    if (FAILED(hr)) {
        fprintf(stderr, "D3D8 VSH: CreateVertexShader failed: 0x%08lX\n", hr);
        ID3D10Blob_Release(code);
        entry->in_use = 0;
        return NULL;
    }

    entry->vs_blob     = code;
    entry->inputs_read = program.inputs_read;
    entry->layout_count = 0;

    fprintf(stderr, "D3D8 VSH: Compiled shader (hash 0x%08X, %d insns, inputs 0x%04X)\n",
            hash, program.length, program.inputs_read);

    return entry;
}

/**
 * Get the input layout for a cache entry.
 * Creates and caches the layout on first request per (input mask, texcoord sizes).
 */
static ID3D11InputLayout *get_cached_layout(VshCacheEntry *entry, DWORD fvf)
{
    uint16_t mask = entry->inputs_read;
    uint32_t key = (uint32_t)mask | (((uint32_t)(fvf >> 16) & 0xFFFF) << 16);
    int i;

    /* Check if we already created a layout for this mask+texcoord layout */
    for (i = 0; i < entry->layout_count; i++) {
        if (entry->layout_masks[i] == key)
            return entry->layouts[i];
    }

    /* Create new layout */
    if (entry->layout_count >= 16)
        return entry->layouts[0]; /* Fallback to first */

    ID3D11InputLayout *layout = create_vsh_input_layout(mask, fvf, entry->vs_blob);
    entry->layouts[entry->layout_count]      = layout;
    entry->layout_masks[entry->layout_count] = key;
    entry->layout_count++;

    return layout;
}

/* ================================================================
 * Public API Implementation
 * ================================================================ */

HRESULT d3d8_vsh_init(void)
{
    D3D11_BUFFER_DESC cbd;
    HRESULT hr;

    memset(g_vsh_slots, 0, sizeof(g_vsh_slots));
    memset(g_vsh_cache, 0, sizeof(g_vsh_cache));
    memset(&g_vsh_constants, 0, sizeof(g_vsh_constants));
    g_vsh_slot_count = 0;
    g_vsh_constants_dirty = TRUE;

    /* Create the constant buffer for VS constants (192 * float4 = 3072 bytes) */
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth      = sizeof(NV2AVSConstants);
    cbd.Usage           = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags  = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &cbd, NULL, &g_vsh_cb);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8 VSH: Failed to create constant buffer: 0x%08lX\n", hr);
        return hr;
    }

    fprintf(stderr, "D3D8 VSH: Vertex shader translator initialized\n");
    return S_OK;
}

void d3d8_vsh_shutdown(void)
{
    int i, j;

    /* Release all cached shaders and layouts */
    for (i = 0; i < NV2A_VS_CACHE_SIZE; i++) {
        VshCacheEntry *e = &g_vsh_cache[i];
        if (!e->in_use) continue;
        if (e->vs)      ID3D11VertexShader_Release(e->vs);
        if (e->vs_blob) ID3D10Blob_Release(e->vs_blob);
        for (j = 0; j < e->layout_count; j++) {
            if (e->layouts[j])
                ID3D11InputLayout_Release(e->layouts[j]);
        }
    }
    memset(g_vsh_cache, 0, sizeof(g_vsh_cache));

    if (g_vsh_cb) {
        ID3D11Buffer_Release(g_vsh_cb);
        g_vsh_cb = NULL;
    }

    memset(g_vsh_slots, 0, sizeof(g_vsh_slots));
    g_vsh_slot_count = 0;
}

HRESULT d3d8_vsh_create_shader(const DWORD *microcode, int num_insns,
                                DWORD *out_handle)
{
    int slot;

    if (!microcode || num_insns <= 0 || !out_handle)
        return E_INVALIDARG;

    if (num_insns > NV2A_VS_MAX_INSTRUCTIONS)
        num_insns = NV2A_VS_MAX_INSTRUCTIONS;

    /* Find a free slot */
    slot = -1;
    for (int i = 0; i < NV2A_VS_MAX_SLOTS; i++) {
        if (!g_vsh_slots[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        fprintf(stderr, "D3D8 VSH: No free shader slots\n");
        return E_OUTOFMEMORY;
    }

    /* Store microcode (deferred compilation) */
    memcpy(g_vsh_slots[slot].microcode, microcode,
           (size_t)num_insns * 4 * sizeof(DWORD));
    g_vsh_slots[slot].length = num_insns;
    g_vsh_slots[slot].in_use = 1;
    g_vsh_slot_count++;

    /* Generate handle: slot index + 0x10000 to distinguish from FVF codes.
     * Xbox D3D8 uses handles with the high bit set (> 0xFFFF). */
    *out_handle = (DWORD)(slot + 0x10000);

    fprintf(stderr, "D3D8 VSH: Created shader handle 0x%lX (%d instructions)\n",
            *out_handle, num_insns);

    return S_OK;
}

HRESULT d3d8_vsh_delete_shader(DWORD handle)
{
    int slot;

    if (!d3d8_vsh_is_programmable(handle))
        return E_INVALIDARG;

    slot = (int)(handle - 0x10000);
    if (slot < 0 || slot >= NV2A_VS_MAX_SLOTS)
        return E_INVALIDARG;

    if (g_vsh_slots[slot].in_use) {
        g_vsh_slots[slot].in_use = 0;
        g_vsh_slot_count--;
    }

    return S_OK;
}

void d3d8_vsh_set_constant(int start_reg, const float *data, int count)
{
    int end_reg;

    if (!data || start_reg < 0)
        return;

    end_reg = start_reg + count;
    if (end_reg > NV2A_VS_MAX_CONSTANTS)
        end_reg = NV2A_VS_MAX_CONSTANTS;

    for (int i = start_reg; i < end_reg; i++) {
        int src_offset = (i - start_reg) * 4;
        g_vsh_constants.c[i][0] = data[src_offset + 0];
        g_vsh_constants.c[i][1] = data[src_offset + 1];
        g_vsh_constants.c[i][2] = data[src_offset + 2];
        g_vsh_constants.c[i][3] = data[src_offset + 3];
    }

    g_vsh_constants_dirty = TRUE;
}

BOOL d3d8_vsh_is_programmable(DWORD handle)
{
    return (handle >= 0x10000) ? TRUE : FALSE;
}

BOOL d3d8_vsh_prepare_draw(DWORD handle)
{
    ID3D11DeviceContext *ctx;
    int slot;
    NV2AVshSlot *vsh;
    uint32_t hash;
    VshCacheEntry *entry;
    ID3D11InputLayout *layout;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;

    if (!d3d8_vsh_is_programmable(handle))
        return FALSE;

    ctx = d3d8_GetD3D11Context();
    if (!ctx) return FALSE;

    /* Resolve handle to shader slot */
    slot = (int)(handle - 0x10000);
    if (slot < 0 || slot >= NV2A_VS_MAX_SLOTS)
        return FALSE;

    vsh = &g_vsh_slots[slot];
    if (!vsh->in_use)
        return FALSE;

    /* Hash the microcode to look up in cache */
    hash = fnv1a_hash(vsh->microcode, (size_t)vsh->length * 4 * sizeof(DWORD));

    /* Look up in cache */
    entry = cache_lookup(hash);
    if (!entry) {
        /* Cache miss: parse, generate HLSL, compile */
        entry = compile_shader(vsh->microcode, vsh->length, hash);
        if (!entry)
            return FALSE;
    }

    /* Bind the vertex shader */
    ID3D11DeviceContext_VSSetShader(ctx, entry->vs, NULL, 0);

    /* Bind the input layout (sizes texcoords from the bound stream FVF) */
    layout = get_cached_layout(entry, d3d8_GetCurrentFVF());
    if (layout)
        ID3D11DeviceContext_IASetInputLayout(ctx, layout);

    /* Update constant buffer if dirty */
    if (g_vsh_constants_dirty) {
        hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_vsh_cb,
                                     0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, &g_vsh_constants, sizeof(g_vsh_constants));
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_vsh_cb, 0);
        }
        g_vsh_constants_dirty = FALSE;
    }

    /* Bind constant buffer to slot b1 */
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 1, 1, &g_vsh_cb);

    return TRUE;
}
