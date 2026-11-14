/* Vertex-program batches handed to a back end before transform
 * (nv2a_backend_set_program_batches, Nv2aProgramBatch).
 *
 * A title-style program (DP4 position, colour and texcoord moves, the D3D
 * screen-space epilogue) is uploaded through the pushbuffer methods, and the
 * same indexed batch is drawn twice: once to a back end that takes surface
 * pixels, once to one that takes program batches. The program batch must
 * carry each distinct vertex once, the triangle list in the order the
 * executor rasterises, the constants and the program; running that program
 * on its inputs (nv2a_vsh_run, then the w and AA fixes the batch describes)
 * must give the vertices the first back end got. Then the topologies (strip,
 * fan, quads), and the batches that must stay on the CPU path: constant
 * writes on, fixed function, lines, a back end that did not ask. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"
#include "nv2a_vsh_interp.h"

#define MEM_BYTES 0x10000u
static uint8_t s_mem[MEM_BYTES];

/* ---- what the executor expects the runtime to provide (as nv2a_ffp_backend_test.c) */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

/* ---- the fake back end: keeps what one draw handed it ------------------ */
#define KEEP 64
static struct {
    int draws, programs;
    uint32_t count;
    Nv2aVertex v[KEEP];
    Nv2aProgramBatch pb;
    float inputs[KEEP][16][4];
    uint32_t indices[KEEP];
    float constants[NV2A_VSH_CONSTANTS][4];
    char hlsl[8192];
} s_be;

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    (void)s; (void)rs; (void)flags; (void)argb; (void)zstencil;
}

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    uint32_t i, n = 0, k;

    (void)s;
    s_be.draws++;
    if (!b->program) {
        s_be.count = b->count;
        for (i = 0; i < b->count && i < KEEP; i++)
            s_be.v[i] = b->vertices[i];
        return;
    }
    s_be.programs++;
    s_be.pb = *b->program;
    for (k = 0; k < 16; k++)
        n += (b->program->inputs_read >> k) & 1u;
    for (i = 0; i < b->program->input_count && i < KEEP; i++)
        memcpy(s_be.inputs[i], b->program->inputs[i * n], n * sizeof(float[4]));
    for (i = 0; i < b->program->index_count && i < KEEP; i++)
        s_be.indices[i] = b->program->indices[i];
    memcpy(s_be.constants, b->program->constants, sizeof s_be.constants);
    k = b->program->hlsl_len < sizeof s_be.hlsl - 1 ? b->program->hlsl_len : sizeof s_be.hlsl - 1;
    memcpy(s_be.hlsl, b->program->hlsl, k);
    s_be.hlsl[k] = 0;
}

static void be_flip(void) {}
static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip };

/* ---- driving the executor ---------------------------------------------- */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void put32(uint32_t at, uint32_t v) { memcpy(s_mem + at, &v, 4); }
static void putf(uint32_t at, float f) { memcpy(s_mem + at, &f, 4); }

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

enum { MUX_TEMP = 1, MUX_INPUT = 2, MUX_CONST = 3 };
static void src(uint32_t w[4], int which, uint32_t mux, uint32_t reg, uint32_t swz)
{
    if (which == 0) { w[1] |= swz; w[2] |= reg << 28 | mux << 26; }
    else if (which == 1) { w[2] |= swz << 17 | reg << 13 | mux << 11; }
    else { w[2] |= swz << 2 | (reg >> 2); w[3] |= (reg & 3) << 30 | mux << 28; }
}

static void upload_program(void)
{
    uint32_t p[8][4], i, k;

    memset(p, 0, sizeof p);
    for (i = 0; i < 4; i++) {                    /* dp4 o[0].<xyzw>, v0, c[100+i] */
        p[i][1] = 7u << 21 | (100u + i) << 13 | 0u << 9;
        src(p[i], 0, MUX_INPUT, 0, 0x1B); src(p[i], 1, MUX_CONST, 0, 0x1B);
        p[i][3] |= (8u >> i) << 12 | 1u << 11;
    }
    p[4][1] = 1u << 21 | 3u << 9;                /* mov o[3], v3 */
    src(p[4], 0, MUX_INPUT, 0, 0x1B);
    p[4][3] |= 0xFu << 12 | 1u << 11 | 3u << 3;
    p[5][1] = 1u << 21 | 9u << 9;                /* mov o[9], v9 */
    src(p[5], 0, MUX_INPUT, 0, 0x1B);
    p[5][3] |= 0xFu << 12 | 1u << 11 | 9u << 3;
    p[6][1] = 3u << 25 | 2u << 21 | 58u << 13;   /* mul o[0].xyz, R12, c[58] + rcc R1.x, R12.w */
    src(p[6], 0, MUX_TEMP, 12, 0x1B); src(p[6], 1, MUX_CONST, 0, 0x1B); src(p[6], 2, MUX_TEMP, 12, 0xFF);
    p[6][3] |= 0xEu << 12 | 1u << 11 | 0x8u << 16;
    p[7][1] = 4u << 21 | 59u << 13;              /* mad o[0].xyz, R12, R1.x, c[59] */
    src(p[7], 0, MUX_TEMP, 12, 0x1B); src(p[7], 1, MUX_TEMP, 1, 0x00); src(p[7], 2, MUX_CONST, 0, 0x1B);
    p[7][3] |= 0xEu << 12 | 1u << 11 | 1u;
    m(0x1E9C, 0);                                /* _PROGRAM_LOAD slot 0 */
    for (i = 0; i < 8; i++)
        for (k = 0; k < 4; k++)
            m(0x0B00 + 4 * ((i * 4 + k) % 32), p[i][k]);
    m(0x1EA0, 0);                                /* _PROGRAM_START */
}

static void upload_constant(uint32_t index, const float v[4])
{
    uint32_t k;

    m(0x1EA4, index);                            /* _CONSTANT_LOAD */
    for (k = 0; k < 4; k++)
        m(0x0B80 + 4 * k, f2u(v[k]));
}

static void draw_indexed(uint32_t prim, const uint32_t *idx, uint32_t n)
{
    uint32_t i;

    m(0x17FC, prim);
    for (i = 0; i < n; i++)
        m(0x1808, idx[i]);                       /* ARRAY_ELEMENT32 */
    m(0x17FC, 0);
}

/* The program batch's vertex `slot` through the CPU interpreter, as a back
 * end would run it on the GPU, compared with what the pixel back end got. */
static int matches_cpu(uint32_t slot, const Nv2aVertex *want)
{
    float in[NV2A_VSH_INPUTS][4];
    Nv2aVshOutput o;
    uint32_t k, j = 0, u = s_be.indices[slot];

    memset(in, 0, sizeof in);
    for (k = 0; k < NV2A_VSH_INPUTS; k++)
        if (s_be.pb.inputs_read & (1u << k))
            memcpy(in[k], s_be.inputs[u][j++], sizeof in[k]);
    if (!nv2a_vsh_run((const float (*)[4])in, &o))
        return 0;
    if (o.pos[3] == 0.0f && !signbit(o.pos[3]))
        o.pos[3] = 1.0f;
    o.pos[0] *= s_be.pb.aa_sx;
    o.pos[1] *= s_be.pb.aa_sy;
    if (fabsf(o.pos[0] - want->x) < 1e-3f && fabsf(o.pos[1] - want->y) < 1e-3f
        && fabsf(o.pos[2] - want->z) < 1e-3f && fabsf(1.0f / o.pos[3] - want->rhw) < 1e-6f
        && fabsf(o.tex[0][0] * s_be.pb.tex0_scale[0] - want->u) < 1e-6f
        && fabsf(o.tex[0][1] * s_be.pb.tex0_scale[1] - want->v) < 1e-6f)
        return 1;
    printf("  slot %u: program (%g %g %g rhw %g uv %g %g), pixels (%g %g %g rhw %g uv %g %g)\n", slot,
           o.pos[0], o.pos[1], o.pos[2], 1.0f / o.pos[3], o.tex[0][0], o.tex[0][1],
           want->x, want->y, want->z, want->rhw, want->u, want->v);
    return 0;
}

int main(void)
{
    static const float c58[4] = {320, -240, 1, 0}, c59[4] = {320, 240, 0.5f, 0};
    static const float rows[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0.5f, 0}, {0.1f, 0, 0, 2}};
    static const uint32_t tris[6] = {0, 1, 2, 2, 1, 3};
    uint32_t i;
    int before;

    nv2a_backend_register(&s_backend);
    m(0x0208, 0x2008);                           /* A8R8G8B8, AA 2x2 */
    m(0x0200, 640u << 16);
    m(0x0204, 480u << 16);
    m(0x020C, 5120);
    m(0x0210, 0x8000);

    upload_program();
    upload_constant(58, c58);
    upload_constant(59, c59);
    for (i = 0; i < 4; i++)
        upload_constant(100 + i, rows[i]);
    m(0x1E94, 2);                                /* TRANSFORM_EXECUTION_MODE: program */

    /* Four vertices: v0 float4 position, v3 D3DCOLOR, v9 float2 texcoord. */
    {
        static const float pos[4][4] = {{-1, -1, 0.2f, 1}, {1, -1, 0.4f, 1}, {0, 1, 0.6f, 1}, {1, 1, 0.8f, 1}};
        for (i = 0; i < 4; i++) {
            uint32_t k;
            for (k = 0; k < 4; k++)
                putf(0x2000 + 16 * i + 4 * k, pos[i][k]);
            put32(0x3000 + 4 * i, 0xFF804020u + i);
            putf(0x4000 + 8 * i, 0.25f * (float)i);
            putf(0x4004 + 8 * i, 1.0f - 0.25f * (float)i);
        }
    }
    m(0x1720, 0x2000); m(0x1760, 2 | (4 << 4) | (16 << 8));    /* attr 0: float4 */
    m(0x172C, 0x3000); m(0x176C, 0 | (4 << 4) | (4 << 8));     /* attr 3: D3DCOLOR */
    m(0x1744, 0x4000); m(0x1784, 2 | (2 << 4) | (8 << 8));     /* attr 9: float2 */

    /* 1. Surface pixels, as before: the back end did not ask. */
    draw_indexed(5, tris, 6);
    CHECK(s_be.draws == 1 && s_be.programs == 0 && s_be.count == 6);

    /* 2. The same batch as a program batch. */
    nv2a_backend_set_program_batches(1);
    draw_indexed(5, tris, 6);
    CHECK(s_be.draws == 2 && s_be.programs == 1);
    CHECK(s_be.pb.input_count == 4);                     /* vertices 2 and 1 sent once */
    CHECK(s_be.pb.inputs_read == ((1u << 0) | (1u << 3) | (1u << 9)));
    CHECK(s_be.pb.index_count == 6);
    for (i = 0; i < 6; i++)
        CHECK(s_be.indices[i] == tris[i]);               /* first use = index order here */
    CHECK(s_be.inputs[3][0][0] == 1.0f && s_be.inputs[3][0][2] == 0.8f);   /* v0 of vertex 3 */
    CHECK(fabsf(s_be.inputs[1][1][2] - (0x20 + 1) / 255.0f) < 1e-6f);     /* v3: D3DCOLOR as R,G,B,A: blue in .z */
    CHECK(s_be.inputs[2][2][0] == 0.5f && s_be.inputs[2][2][1] == 0.5f);  /* v9 of vertex 2 */
    CHECK(s_be.constants[101][1] == 1.0f && s_be.constants[59][2] == 0.5f);
    CHECK(s_be.pb.aa_sx == 2.0f && s_be.pb.aa_sy == 2.0f);
    CHECK(s_be.pb.near_z == 0.5f && s_be.pb.near_clip == 1);
    CHECK(strstr(s_be.hlsl, "Nv2aVshOut nv2a_vsh(Nv2aVshIn I)") != NULL);
    CHECK(s_be.pb.hlsl_len == strlen(s_be.hlsl));
    for (i = 0; i < 6; i++)
        CHECK(matches_cpu(i, &s_be.v[i]));               /* same vertices as the CPU path */

    /* 3. Topologies, in the executor's order. */
    {
        static const uint32_t four[4] = {0, 1, 2, 3};
        const int alt = getenv("RECOMP_STRIP_ALT_WINDING") != NULL;

        draw_indexed(6, four, 4);                        /* strip: 0 1 2, then 1 2 3 (2 1 3 alternating) */
        CHECK(s_be.pb.index_count == 6);
        CHECK(s_be.indices[3] == (alt ? 2u : 1u) && s_be.indices[4] == (alt ? 1u : 2u) && s_be.indices[5] == 3);
        draw_indexed(7, four, 4);                        /* fan: 0 1 2, 0 2 3 */
        CHECK(s_be.pb.index_count == 6 && s_be.indices[3] == 0 && s_be.indices[4] == 2 && s_be.indices[5] == 3);
        draw_indexed(8, four, 4);                        /* quad: 0 1 2, 0 2 3 */
        CHECK(s_be.pb.index_count == 6 && s_be.indices[2] == 2 && s_be.indices[5] == 3);
    }

    /* 4. Batches that stay on the CPU path. */
    before = s_be.programs;
    m(0x1E98, 1);                                        /* _CXT_WRITE_EN: a program may write c[] */
    draw_indexed(5, tris, 6);
    CHECK(s_be.programs == before && s_be.count == 6);
    m(0x1E98, 0);
    m(0x1E94, 0);                                        /* fixed function */
    draw_indexed(5, tris, 6);
    CHECK(s_be.programs == before);
    m(0x1E94, 2);
    draw_indexed(2, tris, 6);                            /* lines */
    CHECK(s_be.programs == before);
    nv2a_backend_register(&s_backend);                   /* registering again clears the request */
    draw_indexed(5, tris, 6);
    CHECK(s_be.programs == before && s_be.count == 6);

    if (!s_fail)
        printf("nv2a vertex-program batches through a back end: ok\n");
    return s_fail;
}
