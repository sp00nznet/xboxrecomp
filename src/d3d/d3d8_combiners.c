/**
 * NV2A Register Combiner to HLSL Pixel Shader Translator - Implementation
 *
 * Translates Xbox NV2A register combiner configurations into HLSL pixel
 * shaders compiled for D3D11. See d3d8_combiners.h for the full model
 * description.
 *
 * Implementation overview:
 *
 * 1. STATE TRACKING
 *    The game sets combiner configuration through either:
 *    (a) SetPixelShader(DWORD token) - a packed DWORD encoding combiner
 *        count and texture modes, with actual stage config in render states
 *    (b) Direct render state writes (D3DRS_PSALPHAINPUTS0..7, etc.)
 *    We parse either path into an NV2ACombinerState structure.
 *
 * 2. HLSL GENERATION
 *    From the combiner state, we emit a complete HLSL pixel shader that:
 *    - Samples textures based on tex_mode per stage
 *    - Walks each active general combiner stage performing AB*CD math
 *    - Executes the final combiner (lerp + add)
 *    - Handles alpha test and fog
 *
 * 3. SHADER CACHE
 *    We hash the full NV2ACombinerState and maintain a fixed-size cache
 *    (128 entries) of compiled ID3D11PixelShader objects. Most Xbox games
 *    use fewer than 20 unique combiner configurations, so this is ample.
 *
 * 4. DRAW INTEGRATION
 *    d3d8_combiners_prepare_draw() is called before each draw. It checks
 *    if state is dirty, rebuilds/looks up the shader, uploads constants,
 *    and binds everything to the D3D11 pipeline.
 */

#include "d3d8_internal.h"
#include "d3d8_combiners.h"
#include <d3dcompiler.h>
#include <string.h>
#include <stdio.h>

#pragma comment(lib, "d3dcompiler.lib")

/* ================================================================
 * Internal State
 * ================================================================ */

/** Current pixel shader token (0 = no combiner shader / fixed-function). */
static DWORD g_ps_token = 0;

/** Current parsed combiner state. */
static NV2ACombinerState g_combiner_state;

/** Dirty flag - set when any PS render state changes. */
static BOOL g_dirty = TRUE;

/** PS constant buffer (uploaded to GPU each draw). */
static ID3D11Buffer *g_combiner_cb = NULL;

/* ================================================================
 * Shader Cache
 *
 * Simple open-addressing hash table with linear probing.
 * 128 entries is generous - most games use <20 unique PS configs.
 * On a full table, the oldest entry is evicted (LRU approximation
 * via frame counter).
 * ================================================================ */

#define COMBINER_CACHE_SIZE 128

typedef struct CombinerCacheEntry {
    BOOL                in_use;
    uint32_t            hash;
    NV2ACombinerState   state;
    ID3D11PixelShader  *shader;
    uint32_t            last_used_frame;
} CombinerCacheEntry;

static CombinerCacheEntry g_cache[COMBINER_CACHE_SIZE];
static uint32_t g_frame_counter = 0;

/* ================================================================
 * Hashing
 *
 * FNV-1a over the combiner state structure. This is fast enough
 * for our purposes and produces good distribution.
 * ================================================================ */

static uint32_t fnv1a_hash(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811C9DC5u;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

static uint32_t combiner_state_hash(const NV2ACombinerState *state)
{
    return fnv1a_hash(state, sizeof(NV2ACombinerState));
}

static BOOL combiner_state_equal(const NV2ACombinerState *a,
                                 const NV2ACombinerState *b)
{
    return memcmp(a, b, sizeof(NV2ACombinerState)) == 0;
}

/* ================================================================
 * Color Helpers
 *
 * Convert D3DCOLOR (ARGB packed DWORD) to float4 (RGBA).
 * D3DCOLOR byte layout in memory: BGRA (little-endian ARGB).
 * ================================================================ */

static void d3dcolor_to_float4(DWORD color, float out[4])
{
    out[0] = ((color >> 16) & 0xFF) / 255.0f; /* R */
    out[1] = ((color >>  8) & 0xFF) / 255.0f; /* G */
    out[2] = ((color >>  0) & 0xFF) / 255.0f; /* B */
    out[3] = ((color >> 24) & 0xFF) / 255.0f; /* A */
}

/* ================================================================
 * Token & Render State Parsing
 * ================================================================ */

void d3d8_combiners_from_render_states(const DWORD *rs,
                                       NV2ACombinerState *state)
{
    uint32_t cicw[8], aicw[8], cocw[8], aocw[8];
    uint32_t control = rs[D3DRS_PSCOMBINERCOUNT];
    int i;

    for (i = 0; i < 8; i++) {
        cicw[i] = rs[D3DRS_PSRGBINPUTS0 + i];
        aicw[i] = rs[D3DRS_PSALPHAINPUTS0 + i];
        cocw[i] = rs[D3DRS_PSRGBOUTPUTS0 + i];
        aocw[i] = rs[D3DRS_PSALPHAOUTPUTS0 + i];
    }
    if ((control & 0xFF) == 0)
        control |= 1;
    d3d8_combiners_from_regs(cicw, aicw, cocw, aocw, control,
                             rs[D3DRS_PSFINALCOMBINERINPUTSABCD],
                             rs[D3DRS_PSFINALCOMBINERINPUTSEFG],
                             rs[D3DRS_PSTEXTUREMODES], state);

    /* The legacy path keeps the constants in the state */
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
        state->c0[i] = rs[D3DRS_PSCONSTANT0_0 + i];
        state->c1[i] = rs[D3DRS_PSCONSTANT1_0 + i];
    }
    state->final_c0 = state->c0[state->num_stages > 0 ? state->num_stages - 1 : 0];
    state->final_c1 = state->c1[state->num_stages > 0 ? state->num_stages - 1 : 0];
}

void d3d8_combiners_parse_token(DWORD token, const DWORD *rs,
                                NV2ACombinerState *state)
{
    int i;

    d3d8_combiners_from_render_states(rs, state);

    /* The legacy token: [3:0] stage count, then 4 bits of NV2ATextureMode
     * per stage from bit 8, then flags. */
    state->num_stages = token & 0xF;
    if (state->num_stages < 1) state->num_stages = 1;
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;
    for (i = 0; i < NV2A_MAX_TEXTURES; i++)
        state->tex_mode[i] = (NV2ATextureMode)((token >> (8 + 4 * i)) & 0xF);
    state->flags = (token >> 24) & 0xFF;
}

/* ================================================================
 * Shader Compilation & Cache
 * ================================================================ */

static ID3D11PixelShader *compile_combiner_shader(const NV2ACombinerState *state)
{
    /* 16KB should be more than enough for any combiner shader */
    char hlsl[16384];
    ID3DBlob *code = NULL;
    ID3DBlob *errors = NULL;
    ID3D11PixelShader *ps = NULL;
    HRESULT hr;
    int len;

    len = d3d8_combiners_generate_hlsl(state, hlsl, sizeof(hlsl));
    if (len < 0) {
        fprintf(stderr, "NV2A combiners: HLSL generation failed (buffer overflow)\n");
        return NULL;
    }

    hr = D3DCompile(hlsl, (SIZE_T)len, "ps_combiner",
                    NULL, NULL, "main", "ps_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &code, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "NV2A combiners: HLSL compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors)
                       : "unknown error");
        /* Dump the generated source for debugging */
        fprintf(stderr, "--- Generated HLSL ---\n%s\n--- End HLSL ---\n", hlsl);
        if (errors) ID3D10Blob_Release(errors);
        return NULL;
    }
    if (errors) ID3D10Blob_Release(errors);

    hr = ID3D11Device_CreatePixelShader(
        d3d8_GetD3D11Device(),
        ID3D10Blob_GetBufferPointer(code),
        ID3D10Blob_GetBufferSize(code),
        NULL, &ps);
    ID3D10Blob_Release(code);

    if (FAILED(hr)) {
        fprintf(stderr, "NV2A combiners: CreatePixelShader failed: 0x%08lX\n", hr);
        return NULL;
    }

    return ps;
}

ID3D11PixelShader *d3d8_combiners_get_shader(const NV2ACombinerState *state)
{
    uint32_t hash = combiner_state_hash(state);
    uint32_t idx = hash & (COMBINER_CACHE_SIZE - 1);
    int probe;

    /* Linear probe lookup */
    for (probe = 0; probe < COMBINER_CACHE_SIZE; probe++) {
        uint32_t slot = (idx + probe) & (COMBINER_CACHE_SIZE - 1);
        CombinerCacheEntry *entry = &g_cache[slot];

        if (!entry->in_use) {
            /* Cache miss - compile and insert */
            ID3D11PixelShader *ps = compile_combiner_shader(state);
            if (!ps) return NULL;

            entry->in_use = TRUE;
            entry->hash = hash;
            memcpy(&entry->state, state, sizeof(NV2ACombinerState));
            entry->shader = ps;
            entry->last_used_frame = g_frame_counter;
            return ps;
        }

        if (entry->hash == hash && combiner_state_equal(&entry->state, state)) {
            /* Cache hit */
            entry->last_used_frame = g_frame_counter;
            return entry->shader;
        }
    }

    /*
     * Table is full - evict the least recently used entry.
     * This is rare in practice (most games use <20 configs).
     */
    {
        uint32_t lru_slot = idx;
        uint32_t lru_frame = UINT32_MAX;
        ID3D11PixelShader *ps;
        CombinerCacheEntry *entry;

        for (probe = 0; probe < COMBINER_CACHE_SIZE; probe++) {
            if (g_cache[probe].last_used_frame < lru_frame) {
                lru_frame = g_cache[probe].last_used_frame;
                lru_slot = probe;
            }
        }

        entry = &g_cache[lru_slot];
        if (entry->shader) {
            ID3D11PixelShader_Release(entry->shader);
        }

        ps = compile_combiner_shader(state);
        if (!ps) return NULL;

        entry->hash = hash;
        memcpy(&entry->state, state, sizeof(NV2ACombinerState));
        entry->shader = ps;
        entry->last_used_frame = g_frame_counter;
        return ps;
    }
}

/* ================================================================
 * Initialization / Shutdown
 * ================================================================ */

HRESULT d3d8_combiners_init(void)
{
    D3D11_BUFFER_DESC cbd;
    HRESULT hr;

    memset(g_cache, 0, sizeof(g_cache));
    memset(&g_combiner_state, 0, sizeof(g_combiner_state));
    g_ps_token = 0;
    g_dirty = TRUE;
    g_frame_counter = 0;

    /* Create the PS constant buffer for combiner shaders.
     * Size must match NV2APSConstants, rounded up to 16-byte alignment. */
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = (sizeof(NV2APSConstants) + 15) & ~15;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &cbd, NULL,
                                   &g_combiner_cb);
    if (FAILED(hr)) {
        fprintf(stderr, "NV2A combiners: Failed to create constant buffer: "
                "0x%08lX\n", hr);
        return hr;
    }

    fprintf(stderr, "NV2A combiners: Initialized (cache size=%d)\n",
            COMBINER_CACHE_SIZE);
    return S_OK;
}

void d3d8_combiners_shutdown(void)
{
    int i;

    /* Release all cached shaders */
    for (i = 0; i < COMBINER_CACHE_SIZE; i++) {
        if (g_cache[i].in_use && g_cache[i].shader) {
            ID3D11PixelShader_Release(g_cache[i].shader);
        }
    }
    memset(g_cache, 0, sizeof(g_cache));

    if (g_combiner_cb) {
        ID3D11Buffer_Release(g_combiner_cb);
        g_combiner_cb = NULL;
    }

    fprintf(stderr, "NV2A combiners: Shut down\n");
}

/* ================================================================
 * Draw Integration
 * ================================================================ */

void d3d8_combiners_set_pixel_shader(DWORD token)
{
    if (token != g_ps_token) {
        g_ps_token = token;
        g_dirty = TRUE;
    }
}

BOOL d3d8_combiners_active(void)
{
    return g_ps_token != 0;
}

void d3d8_combiners_mark_dirty(void)
{
    g_dirty = TRUE;
}

BOOL d3d8_combiners_prepare_draw(void)
{
    ID3D11DeviceContext *ctx;
    ID3D11PixelShader *ps;
    D3D11_MAPPED_SUBRESOURCE mapped;
    const DWORD *rs;
    HRESULT hr;
    int i;

    /* Not using combiner shaders - fall back to fixed-function */
    if (g_ps_token == 0)
        return FALSE;

    ctx = d3d8_GetD3D11Context();
    if (!ctx || !g_combiner_cb)
        return FALSE;

    rs = d3d8_GetRenderStates();

    /* Rebuild combiner state from token + render states if dirty */
    if (g_dirty) {
        d3d8_combiners_parse_token(g_ps_token, rs, &g_combiner_state);
        g_dirty = FALSE;
    }

    /* Get or compile the pixel shader for this combiner state */
    ps = d3d8_combiners_get_shader(&g_combiner_state);
    if (!ps) {
        fprintf(stderr, "NV2A combiners: Failed to get shader, "
                "falling back to FFP\n");
        return FALSE;
    }

    /* Bind the combiner pixel shader */
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);

    /* Update the PS constant buffer with current values.
     *
     * Even though the shader structure doesn't change, the constant
     * values (C0, C1, fog, alpha ref) can change every frame via
     * render state writes. So we always re-upload. */
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_combiner_cb,
                                0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        NV2APSConstants *cb = (NV2APSConstants *)mapped.pData;

        /* Per-stage constants */
        for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
            d3dcolor_to_float4(g_combiner_state.c0[i], cb->c0[i]);
            d3dcolor_to_float4(g_combiner_state.c1[i], cb->c1[i]);
        }

        /* Final combiner constants */
        d3dcolor_to_float4(g_combiner_state.final_c0, cb->final_c0);
        d3dcolor_to_float4(g_combiner_state.final_c1, cb->final_c1);

        /* Fog color from render state */
        d3dcolor_to_float4(rs[D3DRS_FOGCOLOR], cb->fog_color);

        /* Alpha test parameters */
        cb->alpha_ref = rs[D3DRS_ALPHAREF] / 255.0f;
        cb->alpha_func = rs[D3DRS_ALPHAFUNC];
        cb->alpha_test_enable = rs[D3DRS_ALPHATESTENABLE] ? 1 : 0;
        cb->fog_enable = rs[D3DRS_FOGENABLE] ? 1 : 0;
        for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
            D3DFORMAT format = d3d8_base_format(d3d8_GetStageTexture(i));
            cb->alpha_only[i] = format == D3DFMT_A8 || format == D3DFMT_LIN_A8;
            cb->tex_scale[i][0] = cb->tex_scale[i][1] = 1.0f;
            cb->tex_scale[i][2] = cb->tex_scale[i][3] = 1.0f;
            cb->tex_mode[i] = (UINT)g_combiner_state.tex_mode[i];
        }

        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_combiner_cb, 0);
    }

    /* Bind the constant buffer to PS slot 0 */
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_combiner_cb);

    /* Advance frame counter for LRU tracking */
    g_frame_counter++;

    return TRUE;
}
