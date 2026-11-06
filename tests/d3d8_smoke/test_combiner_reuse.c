/* Combiner pixel shaders on D3D11 WARP: one compile per distinct HLSL text,
 * and a draw with unchanged state skips the lookup.
 *
 *   - 200 combiner states that differ only in their constant colours (a
 *     title fading or tinting with PSCONSTANT) generate one HLSL text; they
 *     must share one compiled shader, and still do after the 128-entry state
 *     cache has evicted the first of them;
 *   - a state with a different text gets a different shader;
 *   - d3d8_combiners_prepare_draw binds the shader the lookup gives for the
 *     current state, on the first draw after a change and on the ones after.
 *
 * Then the cost of the lookup each draw used to make is timed, and the
 * compile time of 20 constant-only variants is printed. With
 * RECOMP_PS_TEXT_CACHE=0 the sharing checks are skipped and the compile time
 * is that of compiling every state. No game data, window or GPU is needed. */
#include "d3d8_internal.h"
#include "d3d8_combiners.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ID3D11Device *device;
static ID3D11DeviceContext *context;
static DWORD states[512];
static int failures;

ID3D11Device *d3d8_GetD3D11Device(void) { return device; }
ID3D11DeviceContext *d3d8_GetD3D11Context(void) { return context; }
const DWORD *d3d8_GetRenderStates(void) { return states; }
IDirect3DBaseTexture8 *d3d8_GetStageTexture(DWORD stage) { (void)stage; return NULL; }
D3DFORMAT d3d8_base_format(IDirect3DBaseTexture8 *texture) { (void)texture; return D3DFMT_UNKNOWN; }

#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

static double now_ms(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* One general stage, r0 = t0 * c0, then the final combiner outputs r0. */
static void set_tint_states(DWORD c0)
{
    memset(states, 0, sizeof(states));
    states[D3DRS_PSCOMBINERCOUNT] = 1;
    states[D3DRS_PSRGBINPUTS0] = 0x08010000u;      /* A = t0, B = c0 (rgb) */
    states[D3DRS_PSALPHAINPUTS0] = 0x18111000u;
    states[D3DRS_PSRGBOUTPUTS0] = 0x00000C00u;     /* AB -> r0 */
    states[D3DRS_PSALPHAOUTPUTS0] = 0x00000C00u;
    states[D3DRS_PSFINALCOMBINERINPUTSABCD] = 0x0000000Cu;
    states[D3DRS_PSCONSTANT0_0] = c0;
}

static NV2ACombinerState tint_state(DWORD c0)
{
    NV2ACombinerState s;
    set_tint_states(c0);
    memset(&s, 0, sizeof(s));
    d3d8_combiners_from_render_states(states, &s);
    return s;
}

int main(void)
{
    const char *off = getenv("RECOMP_PS_TEXT_CACHE");
    const int text_cache = !(off && off[0] == '0');
    D3D_FEATURE_LEVEL level;
    ID3D11PixelShader *first, *ps, *bound = NULL;
    NV2ACombinerState s;
    HRESULT hr;
    int i, shared = 1;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
                           D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr)) {
        printf("FAIL: WARP device 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    if (FAILED(d3d8_combiners_init())) {
        printf("FAIL: combiner init\n");
        return 1;
    }

    /* 200 constant-only variants: more than the 128-entry state cache. */
    s = tint_state(0xFF000000u);
    first = d3d8_combiners_get_shader(&s);
    CHECK("first state compiles", first != NULL);
    for (i = 1; i < 200; i++) {
        s = tint_state(0xFF000000u | (DWORD)i * 0x010203u);
        ps = d3d8_combiners_get_shader(&s);
        CHECK("constant variant compiles", ps != NULL);
        if (ps != first)
            shared = 0;
    }
    /* The first variant was evicted from the state cache; it comes back. */
    s = tint_state(0xFF000000u);
    ps = d3d8_combiners_get_shader(&s);
    CHECK("evicted state comes back", ps != NULL);
    if (ps != first)
        shared = 0;
    if (text_cache)
        CHECK("constant-only variants share one shader", shared);

    /* A different text: B = c1 instead of c0. */
    set_tint_states(0xFF336699u);
    states[D3DRS_PSRGBINPUTS0] = 0x08020000u;
    memset(&s, 0, sizeof(s));
    d3d8_combiners_from_render_states(states, &s);
    ps = d3d8_combiners_get_shader(&s);
    CHECK("different text, different shader", ps != NULL && ps != first);

    /* prepare_draw binds what the lookup gives, before and after reuse. */
    set_tint_states(0xFF102030u);
    d3d8_combiners_set_pixel_shader(1);    /* any token: the render states define it */
    d3d8_combiners_mark_dirty();
    memset(&s, 0, sizeof(s));
    d3d8_combiners_parse_token(1, states, &s);
    for (i = 0; i < 3; i++) {
        CHECK("prepare_draw binds a shader", d3d8_combiners_prepare_draw());
        ID3D11DeviceContext_PSGetShader(context, &bound, NULL, NULL);
        CHECK("bound shader is the lookup's", bound == d3d8_combiners_get_shader(&s));
        if (bound)
            ID3D11PixelShader_Release(bound);
    }

    /* What each draw used to spend on the lookup: hash and compare the whole
     * state, a hit in the cache. */
    {
        const int n = 1000000;
        volatile ID3D11PixelShader *sink = NULL;
        double t0 = now_ms(), t;
        for (i = 0; i < n; i++)
            sink = d3d8_combiners_get_shader(&s);
        t = now_ms() - t0;
        (void)sink;
        printf("lookup (hash + compare of a %u-byte state): %.0f ns a draw\n",
               (unsigned)sizeof(NV2ACombinerState), t * 1e6 / n);
    }

    /* 20 fresh constant-only variants of a third text (B = c0, alpha c1). */
    {
        double t0 = now_ms();
        for (i = 0; i < 20; i++) {
            s = tint_state(0xFE000000u | (DWORD)i * 0x030507u);
            s.stages[0].alpha_input[1] = s.stages[0].rgb_input[2];
            d3d8_combiners_get_shader(&s);
        }
        printf("20 constant-only variants of a new text: %.1f ms of compiling (%s)\n",
               now_ms() - t0, text_cache ? "text cache on" : "RECOMP_PS_TEXT_CACHE=0");
    }

    d3d8_combiners_shutdown();
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    printf("d3d8_combiner_reuse: %d failures\n", failures);
    return failures != 0;
}
