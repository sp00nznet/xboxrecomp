/**
 * A Metal backend for the pushbuffer executor (struct nv2a_pb_backend), the
 * macOS sibling of nv2a_pb_d3d11.c.
 *
 * The executor (src/kernel/nv2a_pb_exec.c) decodes the title's pushbuffer into
 * the state in nv2a_pb_state.h and calls the hooks of struct nv2a_pb_backend;
 * this file answers them with Metal instead of the CPU rasteriser:
 *
 *   on_clear  -> a render pass with loadAction Clear on the surface at
 *                color_offset (one MTLTexture per guest surface, grown to
 *                its largest clip); a clear of part of it (clip, clear
 *                rect) is a scissored draw of the clear colour
 *   on_draw   -> vertex-program batches through the program as MSL
 *                (d3d8_vsh_msl.c), pre-transformed (screen-space) ones through
 *                a pass-through vertex function; pixels through the register
 *                combiners as MSL (d3d8_combiners_msl.c) when they are
 *                programmed, else MODULATE. Four stages, textured from the
 *                shared decode (nv2a_backend_common.h).
 *   on_flip   -> commit and wait; with the CAMetalLayer window up
 *                (fb_present_metal.m, whose device and queue this backend
 *                adopts), blit the surface the walker picked for this flip
 *                (nv2a_pb_present_state) into the window's back slot and
 *                publish it: no CPU copy. Under metal_present=readback the
 *                texture is read back for the SDL renderer window
 *                (xbox_FramebufferWindowPresentPixels) as before.
 *   sync_guest -> write a surface back to guest memory when the executor
 *                dumps it (RECOMP_FB_DUMP and the report): the write-back is
 *                on demand (metal_writeback=lazy, default), since the title
 *                never reads its present surface; metal_writeback=always,
 *                metal_no_rtt or a read caught by metal_fb_guard write it at
 *                every flip.
 *
 * Opt-in: RECOMP_PB_BACKEND=metal (nv2a_pb_metal_register_from_env). The CPU
 * rasteriser stays the default and the oracle.
 *
 * Rules this file keeps (from the M1 plan):
 *  - Shaders are compiled at runtime (newLibraryWithSource) with fast math off,
 *    so float results match the CPU path's IEEE arithmetic.
 *  - Constant structs are float4/uint4 only on the MSL side; the C structs they
 *    mirror are checked with _Static_assert, as the HLSL ABI is in the D3D11
 *    backend.
 *  - Encoders begin lazily (first clear or draw on a surface) and end
 *    explicitly on a render-target change, before a readback and at the flip;
 *    every pass loads (loadAction Load) unless it starts with a clear.
 *  - Only the present surface is written back to guest memory, on demand
 *    (sync_guest, a texture decode that overlaps it, its eviction), or at
 *    every flip under metal_writeback=always.
 *  - Guest memory is touched on the executor's thread only: no completion
 *    handlers; a write-back waits for the command buffer and then copies.
 *
 * Depth/stencil and render-to-texture as the D3D11 backend (M2); visibility
 * tests (GET_REPORT) from Metal visibility results through the shared
 * tracker (M3, "Visibility tests" below). Near clipping needs nothing extra:
 * the vertex programs hand Metal clip-space positions (w kept, as for D3D11)
 * and the default MTLDepthClipModeClip clips at the near and far planes, as
 * D3D11's DepthClipEnable does. Not yet: swizzled render targets. The mip
 * LOD bias is a sample() argument (MsPsConsts.tex_bias), not a sampler
 * field, so every macOS takes the same path.
 *
 * Everything runs on the NV2A ack thread (or the smoke test's main thread).
 *
 * Off macOS this file compiles to nothing.
 */
#if defined(__APPLE__)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stddef.h>
#include "recomp_env.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <signal.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include "nv2a_pb_state.h"
#include "nv2a_backend_common.h"
#include "nv2a_tex_pack.h"
#include "d3d8_msl.h"
/* xbox_memory_layout.h pulls in the Win32 shim (BOOL, GetCurrentThread),
 * which collides with Objective-C and CoreServices: declare what is used. */
ptrdiff_t xbox_GetMemoryOffset(void);

#if __has_feature(objc_arc)
#error "nv2a_pb_metal.m is manual retain/release: build it without -fobjc-arc"
#endif

#define LOGP "[metal] "
/* Render targets at render.scale stay within Metal's 2D texture limit on
 * Apple GPUs (nv2a_host_size lowers the factor for a larger surface). */
#define METAL_MAX_DIM 16384u

extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch);
extern int  xbox_FramebufferWindowRunning(void);
extern void xbox_FramebufferWindowPresentPixels(const void *px, uint32_t w, uint32_t h,
                                                uint32_t pitch, uint32_t bpp);
/* The CAMetalLayer window (fb_present_metal.m): its device and queue, which
 * this backend adopts so the slot textures and the present pass share them,
 * and the back slot a frame is blitted into. */
extern int   xbox_FramebufferWindowMetal(void **device, void **queue);
extern void *xbox_FramebufferWindowBackTexture(uint32_t w, uint32_t h);
extern void  xbox_FramebufferWindowPublishTexture(void);
static int s_layer;                  /* the window takes GPU slot textures */
static void guard_open(void);        /* metal_fb_guard: open the pages before we touch them */
static int wb_always(void);
static uint64_t s_window_presents;   /* frames sent to the window (slot or readback) */

/* ---- shader ABI ------------------------------------------------------------
 *
 * The MSL side declares only float4/uint4 members, so its layout is the C
 * struct's by construction: every member 16-aligned, no implicit padding.
 * The asserts pin the C structs (nv2a_backend_common.h) to that layout. */

/* VpConsts (MSL): float4 c[3]; uint4 u[2]; */
_Static_assert(sizeof(struct nv2a_vp_consts) == 80, "nv2a_vp_consts is not 5 x 16 bytes");
/* A program's c[] (MSL constant float4 *c, buffer 1) is v->c as it is. */
_Static_assert(sizeof(((struct nv2a_pb_vsh *)0)->c) == NV2A_VS_MAX_CONSTANTS * 16,
               "vertex constants are not 192 float4");
_Static_assert(offsetof(struct nv2a_vp_consts, u) == 48, "nv2a_vp_consts.u moved");

/* PsConsts (MSL): float4 c0[8], c1[8], fc0, fc1, fog_color; uint4 atest
 * (x alpha_ref as float bits, y alpha_func, z alpha_test_enable, w fog_enable);
 * uint4 alpha_only; float4 tex_scale[4]; uint4 tex_mode; float4 bump[4];
 * float4 bumpls[4] (BUMPENVMAP); uint4 clip_mode (CLIPPLANE). */
#define PS_AT(m, off) \
    _Static_assert(offsetof(struct nv2a_ps_consts, m) == (off), "nv2a_ps_consts." #m " moved")
PS_AT(c0, 0);
PS_AT(c1, 128);
PS_AT(fc0, 256);
PS_AT(fc1, 272);
PS_AT(fog_color, 288);
PS_AT(alpha_ref, 304);
PS_AT(alpha_func, 308);
PS_AT(alpha_test_enable, 312);
PS_AT(fog_enable, 316);
PS_AT(alpha_only, 320);
PS_AT(tex_scale, 336);
PS_AT(tex_mode, 400);
PS_AT(bump, 416);
PS_AT(bumpls, 480);
PS_AT(clip_mode, 544);
#undef PS_AT
_Static_assert(sizeof(struct nv2a_ps_consts) == 560, "nv2a_ps_consts size changed");

/* The MSL PsConsts: the shared constants plus Metal's own trailing float4
 * tex_bias (per stage mip LOD bias, passed to sample() as bias()). Kept out
 * of nv2a_ps_consts, whose size the D3D11 backend pins to NV2APSConstants. */
typedef struct {
    struct nv2a_ps_consts b;
    float tex_bias[4];
} MsPsConsts;
_Static_assert(offsetof(MsPsConsts, tex_bias) == 560 && sizeof(MsPsConsts) == 576,
               "MsPsConsts.tex_bias moved");
_Static_assert(sizeof(float) == 4 && sizeof(uint32_t) == 4, "alpha_ref is read as uint bits");

/* One expanded vertex of a pass-through batch: attributes 0, 3, 4, 9..12 as
 * float4, in this order (the MSL reads vb[vid * VTX_F4 + k]). */
#define VTX_F4 7
static const int s_vtx_attr[VTX_F4] = {0, 3, 4, 9, 10, 11, 12};

static const char s_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VpConsts { float4 c[3]; uint4 u[2]; };\n"
    D3D8_MSL_PSCONSTS
    /* The emitters' VOut (d3d8_msl.h), so vs_pass links with a combiner
     * fs_main and a program's vs_main with fs_basic. */
    D3D8_MSL_VOUT
    /* Attribute 0 is already in surface pixels (x, y, z, rhw), snapped to
     * the NV2A's 1/16-pixel grid (nv2a_screen_snap): c[0] is (2/w, -2/h,
     * 1/zmax, 0) and c[1] (-1, 1, 0, 0), as the D3D11 pass-through's
     * vp_scale / vp_off. */
    "vertex VOut vs_pass(uint vid [[vertex_id]],\n"
    "                    const device float4 *vb [[buffer(0)]],\n"
    "                    constant VpConsts &vp [[buffer(1)]]) {\n"
    "    const device float4 *v = vb + vid * 7u;\n"
    "    VOut o;\n"
    "    o.pos = float4((trunc(v[0].xy * 16.0) / 16.0) * vp.c[0].xy + vp.c[1].xy,\n"
    "                   saturate(v[0].z * vp.c[0].z), 1.0);\n"
    "    o.d0 = v[1]; o.d1 = v[2];\n"
    "    o.t0 = v[3]; o.t1 = v[4]; o.t2 = v[5]; o.t3 = v[6];\n"
    "    o.fog = 1.0; o.psize = 1.0;\n"
    "    return o;\n"
    "}\n"
    /* Batches without register combiners (and, in M1, all of them): the CPU
     * path's MODULATE, oD0 * T0 when stage 0 is on, a CLIPPLANE stage 0's
     * kill, then the alpha test (D3DCMP numbering, on the 8-bit values the
     * way the CPU compares them). Line for line the D3D11 backend's s_ps_src. */
    D3D8_MSL_SNAP_UV
    D3D8_MSL_CLIP_KILLS
    "fragment float4 fs_basic(VOut i [[stage_in]],\n"
    "                         constant PsConsts &pc [[buffer(0)]],\n"
    "                         texture2d<float> tex0 [[texture(0)]],\n"
    "                         sampler samp0 [[sampler(0)]]) {\n"
    "    float4 c = i.d0;\n"
    "    if (pc.tex_mode.x == 4u) {\n"
    "        c *= saturate(i.t0);\n"
    "    } else if (pc.tex_mode.x != 0u) {\n"
    "        float2 uv = i.t0.xy;\n"
    "        if (pc.tex_mode.x == 1u && i.t0.w != 0.0 && i.t0.w != 1.0) uv /= i.t0.w;\n"
    "        uv = nv2a_snap_uv(uv, i.pos.xy, pc.tex_scale[0]);\n"
    "        c *= tex0.sample(samp0, uv * pc.tex_scale[0].xy, bias(pc.tex_bias.x));\n"
    "    }\n"
    "    if (nv2a_clip_kills(pc.clip_mode.x, i.t0)) discard_fragment();\n"
    "    if (pc.atest.z != 0u) {\n"
    "        float ref = as_type<float>(pc.atest.x);\n"
    "        uint a = (uint)(saturate(c.a) * 255.0 + 0.5), r = (uint)(ref * 255.0 + 0.5);\n"
    "        bool ok = true;\n"
    "        uint f = pc.atest.y;\n"
    "        if      (f == 1u) ok = false;\n"
    "        else if (f == 2u) ok = a <  r;\n"
    "        else if (f == 3u) ok = a == r;\n"
    "        else if (f == 4u) ok = a <= r;\n"
    "        else if (f == 5u) ok = a >  r;\n"
    "        else if (f == 6u) ok = a != r;\n"
    "        else if (f == 7u) ok = a >= r;\n"
    "        if (!ok) discard_fragment();\n"
    "    }\n"
    "    return c;\n"
    "}\n"
    /* A colour clear of part of a target (metal_on_clear): a full-screen
     * triangle under a scissor, the clear colour as it is. */
    "struct ClearV { float4 pos [[position]]; };\n"
    "vertex ClearV vs_clear(uint vid [[vertex_id]]) {\n"
    "    float2 t = float2((vid << 1) & 2, vid & 2);\n"
    "    ClearV o;\n"
    "    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "fragment float4 fs_clear(constant float4 &c [[buffer(0)]]) { return c; }\n";

/* ---- device ---------------------------------------------------------------- */

static id<MTLDevice>               s_dev;
static id<MTLCommandQueue>         s_queue;
static id<MTLFunction>             s_vs_pass, s_fs_basic;
static id<MTLFunction>             s_vs_clear, s_fs_clear;
static id<MTLRenderPipelineState>  s_clear_pso[2];   /* partial clears: without, with depth */
static MTLCompileOptions          *s_opts;     /* fast math off, kept for M2's shaders */
static id<MTLCommandBuffer>        s_cmd;      /* the frame's, begun lazily */
static id<MTLRenderCommandEncoder> s_enc;      /* open pass, or nil */
static int                         s_failed;
static int                         s_unified;  /* shared textures readable */
static int                         s_bc;       /* BC3 textures sample (pack entries) */

/* Vertex and index rings, reset after every wait (flip or overflow). */
static id<MTLBuffer> s_vb, s_ib;
static NSUInteger    s_vb_size, s_vb_pos, s_ib_size, s_ib_pos;

#define VIS_SLOTS 8192

static uint64_t s_presents, s_clears, s_draws, s_draws_skipped, s_draws_prog,
                s_draws_culled, s_draws_tex, s_tex_uploads, s_tex_rehash,
                s_tex_changed, s_flushes, s_writebacks, s_cmd_errors;

/* ---- render targets: one per guest surface --------------------------------- */

/* Depth/stencil buffers, one per zeta surface and size, as the D3D11
 * backend's: Depth32Float_Stencil8, so the test compares the same normalised
 * float z (depth units / zmax) the CPU path keeps, and the 8 stencil bits
 * are Z24S8's. Never read back to guest memory. */
#define DT_MAX 4
typedef struct {
    uint32_t       zeta, w, h;
    uint32_t       hw, hh;     /* host size: w x h at render.scale */
    id<MTLTexture> tex;
    uint64_t       last_use;
    int            fresh;      /* contents undefined: the next pass clears */
} DepthTarget;

/* A post-process chain can draw more than 8 targets a frame (a glow chain
 * of small off-screen targets, the scene and the back buffers). With 8 the
 * LRU cycled them, every target was made again each frame, and an
 * off-screen one evicted between its draw and the pass that samples it
 * lost its pixels. 16 holds such a chain; it does not remove the limit. */
#define RT_MAX 16
typedef struct {
    uint32_t       offset;     /* color_offset, as the title set it */
    uint32_t       addr;       /* its guest address (nv2a_pb_dma_resolve) */
    uint32_t       w, h;       /* guest size: the cache key, the mapping */
    uint32_t       hw, hh;     /* host size: the texture (render.scale) */
    id<MTLTexture> tex;
    uint64_t       last_use;
    DepthTarget   *dt;         /* the depth buffer its passes attach, or NULL */
    uint32_t       pitch;      /* the guest pitch it was made with */
    int            dirty;      /* drawn since its last write-back */
    int            presented;  /* ever picked by present_target */
    struct nv2a_rt_own own;    /* is the guest memory under it still its own */
} RenderTarget;

static RenderTarget  s_rt[RT_MAX];
static RenderTarget *s_enc_rt;   /* the target s_enc draws into */
static DepthTarget  *s_enc_dt;   /* and its depth attachment, or NULL */
static DepthTarget   s_dt[DT_MAX];
static uint64_t      s_tick;
static uint64_t      s_zclears, s_draws_z, s_rtt_binds, s_self_copies;
static uint64_t      s_stale_drops, s_retired, s_evict_lost;
static uint64_t      s_grows, s_partial_clears;   /* rt_grow, metal_on_clear */

/* Visibility tests (see "Visibility tests" below): the slot counting on the
 * open pass, opened by begin_pass and closed here. */
static void occ_pass_begin(void);
static void occ_pass_end(void);
static void occ_after_wait(void);
static id<MTLBuffer> s_vis;      /* visibility result slots, 8 bytes each */
static struct OccQuery *s_occ_q;     /* the query counting now, or NULL */
static int s_occ_slot = -1;          /* its slot on s_enc */

/* RECOMP_TRACE=metal_prof: where on_flip's time goes, averaged over 300
 * flips: the wait for the frame's last command buffer, the GPU time of every
 * command buffer committed since the last flip, the host readback (scale > 1),
 * the guest write-back, the readback-mode window copy and hand-off, and the
 * layer-mode slot blit. Prints only. */
static int s_prof = -1;
#define PROF_CMDS 128
static id<MTLCommandBuffer> s_prof_cmd[PROF_CMDS];
static int s_prof_ncmd;
static struct {
    uint64_t n, flip, wait, gpu, hostrb, wb, winrb, winpush, blit, ncmd, hash;
} s_pa;

/* The last committed command buffer: a sync between flips waits for it when
 * nothing is open (a GET_REPORT commit may still be running). */
static id<MTLCommandBuffer> s_last_cmd;
static uint64_t s_decode_syncs;

static uint64_t prof_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int prof_on(void)
{
    if (s_prof < 0)
        s_prof = recomp_env(RENV_METAL_PROF) != NULL;
    return s_prof;
}

/* A flip-phase timer: no clock reads unless metal_prof is on. */
static uint64_t prof_t(void)
{
    return prof_on() ? prof_now() : 0;
}

static void prof_add(uint64_t *acc, uint64_t t0)
{
    if (prof_on())
        *acc += prof_now() - t0;
}

static void end_encoder(void)
{
    if (s_enc) {
        occ_pass_end();
        [s_enc endEncoding];
        [s_enc release];
        s_enc = nil;
    }
    s_enc_rt = NULL;
    s_enc_dt = NULL;
}

/* End the open pass, commit the frame's command buffer and (wait) block
 * until the GPU is done with it. After a wait the rings are free again. */
static void flush(int wait)
{
    end_encoder();
    if (!s_cmd) {
        if (wait) {            /* nothing in flight: the rings are free */
            s_vb_pos = 0;
            s_ib_pos = 0;
            s_flushes++;
        }
        return;
    }
    [s_cmd commit];
    [s_last_cmd release];
    s_last_cmd = [s_cmd retain];
    if (wait) {
        [s_cmd waitUntilCompleted];
        if ([s_cmd status] == MTLCommandBufferStatusError) {
            s_cmd_errors++;
            if (s_cmd_errors <= 4)
                fprintf(stderr, LOGP "command buffer error: %s\n",
                        [[[s_cmd error] localizedDescription] UTF8String]);
        }
        s_vb_pos = 0;
        s_ib_pos = 0;
    }
    if (prof_on() && s_prof_ncmd < PROF_CMDS)
        s_prof_cmd[s_prof_ncmd++] = [s_cmd retain];
    [s_cmd release];
    s_cmd = nil;
    s_flushes++;
    if (wait)
        occ_after_wait();
}

static void ensure_cmd(void)
{
    if (!s_cmd)
        s_cmd = [[s_queue commandBuffer] retain];
}

/* Open a pass on rt: with `clear`, loadAction Clear to c; else Load. The
 * pass attaches rt->dt when it has one; zmask (CLEAR_SURFACE's bit 0 depth,
 * bit 1 stencil) clears it to z / st, and a fresh buffer clears to the far
 * plane and stencil 0, where the CPU path starts its own. */
static void begin_pass(RenderTarget *rt, int clear, const double c[4],
                       unsigned zmask, float z, uint32_t st)
{
    MTLRenderPassDescriptor *rp;
    DepthTarget *dt = rt->dt;
    end_encoder();
    ensure_cmd();
    rt->dirty = 1;
    nv2a_rt_own_drawn(&rt->own, (uint32_t)s_presents);
    rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rt->tex;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (clear) {
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
    } else {
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    }
    if (dt) {
        int zc = (zmask & 1u) || dt->fresh, sc = (zmask & 2u) || dt->fresh;
        rp.depthAttachment.texture = dt->tex;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.depthAttachment.loadAction = zc ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.clearDepth = (zmask & 1u) ? z : 1.0;
        rp.stencilAttachment.texture = dt->tex;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
        rp.stencilAttachment.loadAction = sc ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.stencilAttachment.clearStencil = (zmask & 2u) ? (st & 0xFFu) : 0u;
        dt->fresh = 0;
    }
    rp.visibilityResultBuffer = s_vis;
    s_enc = [[s_cmd renderCommandEncoderWithDescriptor:rp] retain];
    s_enc_rt = rt;
    s_enc_dt = dt;
    occ_pass_begin();
}

static void begin_encoder(RenderTarget *rt, int clear, const double c[4])
{
    begin_pass(rt, clear, c, 0, 1.0f, 0);
}

static int init(void)
{
    static int tried;
    NSError *err = nil;
    MTLCompileOptions *opts;
    id<MTLLibrary> lib;

    if (tried)
        return !s_failed;
    tried = 1;
    s_failed = 1;

    @autoreleasepool {
        void *wdev = NULL, *wq = NULL;
        /* The layer window was made before the guest started, with its own
         * device and queue: one device so its slots take this backend's
         * blits, one queue so a present pass runs after the blit it shows. */
        if (xbox_FramebufferWindowMetal(&wdev, &wq) && wdev && wq) {
            s_dev = [(id<MTLDevice>)wdev retain];
            s_queue = [(id<MTLCommandQueue>)wq retain];
            s_layer = 1;
        } else {
            s_dev = MTLCreateSystemDefaultDevice();
            if (s_dev)
                s_queue = [s_dev newCommandQueue];
        }
        if (!s_dev || !s_queue) {
            fprintf(stderr, LOGP "no Metal device\n");
            return 0;
        }
        s_unified = [s_dev hasUnifiedMemory] ? 1 : 0;
        /* Every Mac GPU samples BC; an iPad before the A-series GPUs that
         * report it does not, and its pack BC3 entries go up from the guest. */
#if TARGET_OS_OSX
        s_bc = 1;
#endif
        if (@available(macOS 11.0, iOS 16.4, *))
            s_bc = [s_dev supportsBCTextureCompression] ? 1 : 0;

        opts = [[MTLCompileOptions alloc] init];
        /* Fast math off: the CPU path is IEEE; reassociation and flushed
         * denormals would move pixels across the golden tolerance. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        opts.fastMathEnabled = NO;
#pragma clang diagnostic pop
        if (@available(macOS 15.0, *)) {
            opts.mathMode = MTLMathModeSafe;
            opts.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
        }
        s_opts = opts;
        lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:s_msl]
                                  options:opts error:&err];
        if (!lib) {
            fprintf(stderr, LOGP "MSL compile failed: %s\n",
                    err ? [[err localizedDescription] UTF8String] : "?");
            return 0;
        }
        s_vs_pass = [lib newFunctionWithName:@"vs_pass"];
        s_fs_basic = [lib newFunctionWithName:@"fs_basic"];
        s_vs_clear = [lib newFunctionWithName:@"vs_clear"];
        s_fs_clear = [lib newFunctionWithName:@"fs_clear"];
        [lib release];
        if (!s_vs_pass || !s_fs_basic || !s_vs_clear || !s_fs_clear) {
            fprintf(stderr, LOGP "MSL functions missing\n");
            return 0;
        }

        /* The vertex ring is bounded by the 16-bit vertex range (65536
         * vertices are 1 MB per float4 stream); the index ring by the cap,
         * so any batch the walker accepts fits: three 32-bit list entries
         * per index at most. */
        s_vb_size = 16u << 20;
        s_ib_size = NV_MAX_INDICES * 3u * 4u;
        s_vb = [s_dev newBufferWithLength:s_vb_size options:MTLResourceStorageModeShared];
        s_ib = [s_dev newBufferWithLength:s_ib_size options:MTLResourceStorageModeShared];
        s_vis = [s_dev newBufferWithLength:VIS_SLOTS * 8u
                                   options:MTLResourceStorageModeShared];
        if (!s_vb || !s_ib || !s_vis) {
            fprintf(stderr, LOGP "buffer creation failed\n");
            return 0;
        }
        fprintf(stderr, LOGP "device up: %s (unified memory %d), fast math off; present %s,"
                " write-back %s\n", [[s_dev name] UTF8String], s_unified,
                s_layer ? "CAMetalLayer slots" : "texture readback",
                wb_always() ? "every flip" : "on demand");
    }
    s_failed = 0;
    return 1;
}

/* ---- surfaces --------------------------------------------------------------- */

static void rt_sync(RenderTarget *rt, uint32_t pitch);
static void rt_free(RenderTarget *rt);

/* ---- render-target ownership (nv2a_backend_common.h) -------------------------
 *
 * A target is the title's surface only while the guest memory under it holds
 * what the backend last knew there. A title that frees a target's memory and
 * loads something else into it (a menu leaves 512x512 targets behind
 * whose megabytes the next level refills with textures and tables) would otherwise
 * get the old image sampled in place of its texels, or written back over
 * them. A stale target is dropped without a write-back. A/B:
 * RECOMP_DEBUG=rt_alias_check=0. */
static int alias_check_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_RT_ALIAS_CHECK);
        on = !(e && e[0] == '0');
    }
    return on;
}

static void rt_own_reset(RenderTarget *rt)
{
    uint64_t t;
    if (!alias_check_on() || !rt->pitch)
        return;
    guard_open();   /* our own read, not a title read */
    t = prof_t();
    nv2a_rt_own_reset(&rt->own, (const uint8_t *)xbox_GetMemoryOffset(), rt->addr,
                      rt->pitch, rt->h, (uint32_t)s_presents);
    prof_add(&s_pa.hash, t);
}

/* Drop rt when the title has rewritten its memory; 1 if it was dropped.
 * `why` names the caller for the log: bind, sync, evict, overlap, reuse. */
static int rt_drop_if_stale(RenderTarget *rt, const char *why)
{
    static int said;
    uint64_t t;
    int stale;

    if (!alias_check_on() || !rt->tex || !rt->pitch)
        return 0;
    guard_open();
    t = prof_t();
    stale = nv2a_rt_own_stale(&rt->own, (const uint8_t *)xbox_GetMemoryOffset(), rt->addr,
                              rt->pitch, rt->h, (uint32_t)s_presents);
    prof_add(&s_pa.hash, t);
    if (!stale)
        return 0;
    s_stale_drops++;
    if (said < 20 || recomp_env(RENV_METAL_TRACE)) {
        said++;
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target dropped (%s): the title"
                " rewrote its %u bytes; last drawn at flip %u, now %llu%s\n",
                rt->offset, rt->w, rt->h, why, rt->pitch * rt->h, rt->own.draw_flip,
                (unsigned long long)s_presents, said == 20 ? " (further drops: the"
                " present summary)" : "");
    }
    rt->dirty = 0;   /* the title's bytes win: no write-back */
    rt_free(rt);
    return 1;
}

static void rt_free(RenderTarget *rt)
{
    /* A presented target's bytes are what a new target at that surface seeds
     * from, as when every flip wrote it back. The eviction in surface() has
     * already written back any target it frees. */
    if (rt->dirty && rt->presented && rt_drop_if_stale(rt, "evict"))
        return;
    if (rt->dirty && rt->presented)
        rt_sync(rt, rt->pitch);
    if (s_enc_rt == rt)
        end_encoder();
    [rt->tex release];
    memset(rt, 0, sizeof *rt);
}

/* The depth buffer for the zeta surface now set, at w x h; made on first
 * use (fresh), the least recently used one giving way when all are taken. */
static DepthTarget *depth_target(uint32_t w, uint32_t h)
{
    uint32_t zeta = nv2a_pb_vsh_state()->zeta_offset;
    DepthTarget *e = NULL, *lru = &s_dt[0];
    MTLTextureDescriptor *td;
    int i;

    for (i = 0; i < DT_MAX; i++) {
        DepthTarget *d = &s_dt[i];
        if (d->tex && d->zeta == zeta && d->w == w && d->h == h) {
            d->last_use = s_tick;
            return d;
        }
        if (!d->tex && !e)
            e = d;
        if (d->last_use < lru->last_use)
            lru = d;
    }
    if (!e) {
        e = lru;
        if (s_enc_dt == e)
            end_encoder();
        for (i = 0; i < RT_MAX; i++)
            if (s_rt[i].dt == e)
                s_rt[i].dt = NULL;
        [e->tex release];
        memset(e, 0, sizeof *e);
    }
    nv2a_host_size(w, h, nv2a_host_render_scale(), METAL_MAX_DIM, &e->hw, &e->hh);
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                            width:e->hw height:e->hh mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    e->tex = [s_dev newTextureWithDescriptor:td];
    if (!e->tex)
        return NULL;
    e->zeta = zeta;
    e->w = w;
    e->h = h;
    e->last_use = s_tick;
    e->fresh = 1;
    if (e->hw != w)
        fprintf(stderr, LOGP "zeta 0x%08X: %ux%u depth buffer (%ux%u host)\n",
                zeta, w, h, e->hw, e->hh);
    else
        fprintf(stderr, LOGP "zeta 0x%08X: %ux%u depth buffer\n", zeta, w, h);
    return e;
}

/* A BGRA8 colour texture of hw x hh host pixels, as every target is made. */
static id<MTLTexture> rt_texture_new(uint32_t hw, uint32_t hh)
{
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:hw height:hh
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    /* Managed only for a discrete GPU (Intel Macs); deprecated on 27. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    td.storageMode = s_unified ? MTLStorageModeShared : MTLStorageModeManaged;
#pragma clang diagnostic pop
    return [s_dev newTextureWithDescriptor:td];
}

/* tex (w x h guest, hw x hh host) from the guest's bytes at addr when the
 * surface is 32-bit, as the CPU path draws over whatever memory holds; 0 when
 * it is not (the texture is left as made). */
static int rt_seed(id<MTLTexture> tex, uint32_t addr, uint32_t pitch, uint32_t w,
                    uint32_t h, uint32_t hw, uint32_t hh)
{
    const uint8_t *src = (const uint8_t *)xbox_GetMemoryOffset() + addr;

    if (pitch < w * 4u)
        return 0;
    guard_open();   /* our own read of the seed is not a title read */
    if (hw == w) {
        [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0
                 withBytes:src bytesPerRow:pitch];
    } else {
        /* Scaled: the guest bytes, each pixel an n x n block. */
        unsigned n = hw / w;
        uint8_t *up = (uint8_t *)malloc((size_t)hw * hh * 4u);
        if (up) {
            nv2a_upscale_nearest32(src, pitch, w, h, n, up, hw * 4u);
            [tex replaceRegion:MTLRegionMake2D(0, 0, hw, hh) mipmapLevel:0
                     withBytes:up bytesPerRow:hw * 4u];
            free(up);
        }
    }
    return 1;
}

/* rt made w x h (guest pixels), what it holds kept at its top left: the
 * D3D11 backend's rt_grow. The new area is seeded from the guest bytes, as a
 * new target is; replaceRegion runs now and the blit of the old contents
 * later on the GPU, so the blit wins where both write. A blit cannot
 * rescale: when the larger size takes another host factor (past
 * METAL_MAX_DIM), rt is written back first and the whole texture seeded. */
static int rt_grow(RenderTarget *rt, uint32_t w, uint32_t h)
{
    static int said;
    id<MTLTexture> tex;
    uint32_t hw, hh;
    unsigned n;
    int keep;

    n = nv2a_host_size(w, h, nv2a_host_render_scale(), METAL_MAX_DIM, &hw, &hh);
    keep = rt->hw == rt->w * n && rt->hh == rt->h * n;
    if (!keep)
        rt_sync(rt, rt->pitch);
    /* An encoder must not outlive its attachment's swap, and the passes
     * below need none open. */
    end_encoder();
    tex = rt_texture_new(hw, hh);
    if (!tex)
        return 0;
    if (!rt_seed(tex, rt->addr, rt->pitch, w, h, hw, hh)) {
        /* A new texture's contents are undefined: a 16-bit surface's margin
         * starts black, as D3D11's does. */
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLRenderCommandEncoder> e;
        rp.colorAttachments[0].texture = tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        ensure_cmd();
        e = [s_cmd renderCommandEncoderWithDescriptor:rp];
        [e endEncoding];
    }
    if (keep) {
        id<MTLBlitCommandEncoder> b;
        ensure_cmd();
        b = [s_cmd blitCommandEncoder];
        [b copyFromTexture:rt->tex sourceSlice:0 sourceLevel:0
              sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(rt->hw, rt->hh, 1)
                 toTexture:tex destinationSlice:0 destinationLevel:0
         destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b endEncoding];
    }
    if (said++ < 20)
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target grown to %ux%u\n",
                rt->offset, rt->w, rt->h, w, h);
    [rt->tex release];   /* an encoded command keeps its own reference */
    rt->tex = tex;
    rt->w = w;
    rt->h = h;
    rt->hw = hw;
    rt->hh = hh;
    /* The cached depth buffer is the old size; begin_pass would attach it to
     * the larger colour texture. The next depth draw attaches one at the
     * grown size (depth_target), fresh. */
    rt->dt = NULL;
    rt_own_reset(rt);
    s_grows++;
    return 1;
}

/* The render target for the surface at color_offset, at least the size of
 * its clip extent (the D3D11 backend's rule), made on first use. A new one
 * starts from the guest's bytes (rt_seed). */
static RenderTarget *surface(void)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    uint32_t w = g->clip_x + g->clip_w, h = g->clip_y + g->clip_h;
    RenderTarget *free_rt = NULL, *lru = &s_rt[0];
    uint32_t hw, hh;
    int i;

    if (!g->color_offset || !w || !h || w > 4096 || h > 4096)
        return NULL;
    s_tick++;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->tex && rt->offset == g->color_offset && rt->w == w && rt->h == h) {
            /* First use this flip of a target the title may have rewritten:
             * a stale one is made again from the title's bytes, which the
             * GPU would draw over. */
            if (rt_drop_if_stale(rt, "reuse"))
                break;
            rt->last_use = s_tick;
            return rt;
        }
    }
    /* The same surface under another clip extent: one target per surface,
     * as large as any clip it was drawn with (D3D11's rule, toolkit
     * 4773a01). A title can draw its 640x480 back buffer with clips of
     * 640x464, 623x401, 159x344 and more in one frame; a target per extent
     * made the overlap rule below write back and retire one at every clip
     * change (a GPU wait, and at render.scale > 1 the box filter's loss),
     * and a flip found no target of the frame's size. Draws map guest pixels
     * to NDC through rt->w/h, so a larger target keeps every position; a
     * clear covers only its clear rect or clip (metal_on_clear) and a flip
     * shows only the frame's extent (present_extent). Another pitch at the
     * same offset is a new allocation, not another clip: it gets a target of
     * its own and the overlap rule retires this one. */
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->tex || rt->offset != g->color_offset || rt->pitch != g->pitch)
            continue;
        if (rt_drop_if_stale(rt, "reuse"))
            break;
        if ((rt->w >= w && rt->h >= h)
                || rt_grow(rt, rt->w > w ? rt->w : w, rt->h > h ? rt->h : h)) {
            rt->last_use = s_tick;
            return rt;
        }
        break;
    }
    /* No two live targets share memory: one the new target overlaps is
     * written back first (the seed below then reads what was drawn) or,
     * if the title rewrote it, dropped. */
    if (alias_check_on()) {
        uint32_t addr = nv2a_pb_dma_resolve(g->color_offset);
        for (i = 0; i < RT_MAX; i++) {
            RenderTarget *rt = &s_rt[i];
            if (!rt->tex || !nv2a_rt_overlap(rt->addr, rt->pitch * rt->h, addr, g->pitch * h))
                continue;
            if (rt_drop_if_stale(rt, "overlap"))
                continue;
            rt_sync(rt, rt->pitch);
            if (s_retired++ < 20)
                fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target retired:"
                        " 0x%08X %ux%u overlaps it\n", rt->offset, rt->w, rt->h,
                        g->color_offset, w, h);
            rt_free(rt);
        }
    }
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->tex && !free_rt)
            free_rt = rt;
        if (rt->last_use < lru->last_use)
            lru = rt;
    }
    if (!free_rt) {
        free_rt = lru;
        /* What was drawn goes to guest memory first, presented or not: a
         * later pass that samples this surface decodes it from there
         * (tex_bind) or seeds a new target from it. A target the title
         * rewrote is dropped instead (its bytes win). */
        if (!rt_drop_if_stale(free_rt, "evict")) {
            rt_sync(free_rt, free_rt->pitch);
            if (free_rt->dirty && s_evict_lost++ < 20)
                fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target evicted for"
                        " 0x%08X with no write-back (pitch %u), its pixels lost\n",
                        free_rt->offset, free_rt->w, free_rt->h, g->color_offset,
                        free_rt->pitch);
            rt_free(free_rt);
        }
    }
    nv2a_host_size(w, h, nv2a_host_render_scale(), METAL_MAX_DIM, &hw, &hh);
    free_rt->tex = rt_texture_new(hw, hh);
    if (!free_rt->tex)
        return NULL;
    free_rt->offset = g->color_offset;
    free_rt->addr = nv2a_pb_dma_resolve(g->color_offset);
    free_rt->w = w;
    free_rt->h = h;
    free_rt->hw = hw;
    free_rt->hh = hh;
    free_rt->pitch = g->pitch;
    free_rt->last_use = s_tick;
    rt_own_reset(free_rt);   /* the bytes the seed reads, or would */
    rt_seed(free_rt->tex, free_rt->addr, g->pitch, w, h, hw, hh);
    if (hw != w)
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target (%ux%u host)\n",
                g->color_offset, w, h, hw, hh);
    else
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target\n",
                g->color_offset, w, h);
    return free_rt;
}

static id<MTLDepthStencilState> depth_state(int depth, int stencil);

/* The partial-clear pipeline for a pass with (dt) or without a depth
 * attachment, made on first use; nil if it cannot be made. */
static id<MTLRenderPipelineState> clear_pso(int dt)
{
    static int failed[2];
    MTLRenderPipelineDescriptor *pd;
    NSError *err = nil;

    if (s_clear_pso[dt] || failed[dt])
        return s_clear_pso[dt];
    pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = s_vs_clear;
    pd.fragmentFunction = s_fs_clear;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pd.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
    if (dt) {
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    }
    s_clear_pso[dt] = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    [pd release];
    if (!s_clear_pso[dt]) {
        failed[dt] = 1;
        fprintf(stderr, LOGP "partial-clear pipeline failed (%s): a partial clear clears"
                " the whole target\n", err ? [[err localizedDescription] UTF8String] : "?");
    }
    return s_clear_pso[dt];
}

/* A colour clear covers its clear rect, else its clip (nv2a_clear_box), as on
 * D3D11 and the CPU path: one target serves every clip of its surface
 * (rt_grow). A load action clears a whole attachment, so a box that is the
 * whole target keeps it, and a smaller one is a full-screen triangle of the
 * clear colour under a scissor, in a loading pass. */
static void metal_on_clear(uint32_t param)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    RenderTarget *rt;
    double c[4];
    uint32_t b[4];
    id<MTLRenderPipelineState> pso;

    if (!(param & 0xF0) || !init())
        return;
    @autoreleasepool {
        rt = surface();
        if (!rt || !nv2a_clear_box(g, rt->w, rt->h, b))
            return;
        c[0] = (double)((g->clear_color >> 16) & 0xFF) / 255.0;
        c[1] = (double)((g->clear_color >> 8) & 0xFF) / 255.0;
        c[2] = (double)(g->clear_color & 0xFF) / 255.0;
        c[3] = (double)(g->clear_color >> 24) / 255.0;
        s_clears++;
        if (b[0] == 0 && b[1] == 0 && b[2] == rt->w && b[3] == rt->h) {
            begin_encoder(rt, 1, c);
            return;
        }
        if (!s_enc || s_enc_rt != rt)
            begin_encoder(rt, 0, NULL);
        pso = clear_pso(s_enc_dt ? 1 : 0);
        if (!pso) {
            begin_encoder(rt, 1, c);
            return;
        }
        {
            /* byte/255: BGRA8Unorm stores the bytes the CPU path writes. */
            float fc[4] = {(float)c[0], (float)c[1], (float)c[2], (float)c[3]};
            MTLViewport vp = {0.0, 0.0, (double)rt->hw, (double)rt->hh, 0.0, 1.0};
            MTLScissorRect sr;
            sr.x = (NSUInteger)((uint64_t)b[0] * rt->hw / rt->w);
            sr.y = (NSUInteger)((uint64_t)b[1] * rt->hh / rt->h);
            sr.width = (NSUInteger)((uint64_t)b[2] * rt->hw / rt->w) - sr.x;
            sr.height = (NSUInteger)((uint64_t)b[3] * rt->hh / rt->h) - sr.y;
            [s_enc setRenderPipelineState:pso];
            if (s_enc_dt)
                [s_enc setDepthStencilState:depth_state(0, 0)];
            [s_enc setCullMode:MTLCullModeNone];
            [s_enc setViewport:vp];
            [s_enc setScissorRect:sr];
            [s_enc setFragmentBytes:fc length:sizeof fc atIndex:0];
            /* A clear is not a zpass sample: no counting for this draw. Metal
             * sets a visibility offset once per pass, so counting cannot
             * resume at the same slot: the pass ends, and the next draw's
             * pass takes a new slot in the query (occ_pass_begin), as a
             * query spanning passes does. */
            if (s_occ_slot >= 0)
                [s_enc setVisibilityResultMode:MTLVisibilityResultModeDisabled
                                        offset:(NSUInteger)s_occ_slot * 8u];
            [s_enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            /* The scissor stays: each draw sets its own (metal_on_draw). */
            if (s_occ_slot >= 0)
                end_encoder();
        }
        s_partial_clears++;
    }
}

/* CLEAR_SURFACE with the Z or stencil bit: the value scaled as the CPU path
 * scales it, by the zeta format's range; the stencil its low byte. A pass
 * that loads colour and clears the depth buffer. */
static void metal_on_zclear(uint32_t param)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    RenderTarget *rt;
    DepthTarget *dt;

    if (!(param & 3u) || !init())
        return;
    @autoreleasepool {
        rt = surface();
        if (!rt)
            return;
        dt = depth_target(rt->w, rt->h);
        if (!dt)
            return;
        rt->dt = dt;
        begin_pass(rt, 0, NULL, param & 3u, nv2a_zclear_depth(g->format, v->zclear),
                   v->zclear & 0xFFu);
        if (param & 1u)
            s_zclears++;
    }
}

/* ---- vertices ---------------------------------------------------------------- */

/* One attribute of one vertex as float4, decoded the way the CPU path's
 * fetch_attr does. 0 if the stream has no data for it. */
static int fetch(const struct nv2a_pb_draw *d, const VertexAttr *a,
                 uint32_t index, float out[4])
{
    const uint8_t *p;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (d->inline_data) {
        size_t at = (size_t)a->offset + (size_t)index * a->stride;
        if (at + 4 > d->inline_bytes)
            return 0;
        p = (const uint8_t *)d->inline_buf + at;
    } else {
        if (!a->offset)
            return 0;
        p = (const uint8_t *)xbox_GetMemoryOffset() + a->offset
          + (size_t)index * a->stride;
    }
    return nv2a_vtx_decode(a->type, a->size, p, out);
}

/* `bytes` from a ring, 16-aligned; on overflow the frame so far is flushed
 * (and waited for) and the ring starts over. NULL if it can never fit. */
static void *ring_alloc(id<MTLBuffer> b, NSUInteger size, NSUInteger *pos,
                        NSUInteger bytes, NSUInteger *off)
{
    NSUInteger at = (*pos + 15u) & ~(NSUInteger)15u;
    if (bytes > size)
        return NULL;
    if (at + bytes > size) {
        flush(1);   /* resets both rings */
        at = 0;
    }
    *pos = at + bytes;
    *off = at;
    return (uint8_t *)[b contents] + at;
}

/* ---- pipelines (blend state is part of them in Metal) ----------------------- */

static MTLBlendOperation blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return MTLBlendOperationSubtract;
    case 0x800B:
    case 0xF005: return MTLBlendOperationReverseSubtract;
    case 0x8007: return MTLBlendOperationMin;
    case 0x8008: return MTLBlendOperationMax;
    default:     return MTLBlendOperationAdd;
    }
}

static MTLBlendFactor blend_factor(uint32_t gl, int alpha)
{
    static const MTLBlendFactor map[NV2A_BF_UNKNOWN + 1] = {
        MTLBlendFactorZero, MTLBlendFactorOne,
        MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor,
        MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha,
        MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha,
        MTLBlendFactorDestinationColor, MTLBlendFactorOneMinusDestinationColor,
        MTLBlendFactorSourceAlphaSaturated,
        /* As the D3D11 backend: one RGBA constant, which
         * nv2a_blend_constant() fills for _COLOR or _ALPHA use. */
        MTLBlendFactorBlendColor, MTLBlendFactorOneMinusBlendColor,
        MTLBlendFactorBlendColor, MTLBlendFactorOneMinusBlendColor,
        MTLBlendFactorOne,                      /* unknown */
    };
    uint32_t bf = nv2a_blend_from_gl(gl);
    return map[alpha ? nv2a_blend_for_alpha(bf) : bf];
}

/* A pipeline is its two functions plus the blend state. The functions are
 * cached for the life of the process (vs_cache, fs_cache), so their
 * pointers identify them. */
typedef struct {
    uint64_t vs, fs;
    uint32_t key[6];
} PsoKey;
#define PSO_MAX 512
static struct { PsoKey k; id<MTLRenderPipelineState> pso; } s_pso[PSO_MAX];
static int s_pso_n, s_pso_last;

static id<MTLRenderPipelineState> pipeline(id<MTLFunction> vs, id<MTLFunction> fs)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    MTLRenderPipelineDescriptor *pd;
    MTLRenderPipelineColorAttachmentDescriptor *ca;
    id<MTLRenderPipelineState> pso;
    NSError *err = nil;
    PsoKey pk;
    uint32_t *key = pk.key;
    int i;

    memset(&pk, 0, sizeof pk);
    pk.vs = (uint64_t)(uintptr_t)vs;
    pk.fs = (uint64_t)(uintptr_t)fs;
    key[0] = g->blend_enable ? 1 : 0;
    key[1] = key[0] ? g->blend_sfactor : 0;
    key[2] = key[0] ? g->blend_dfactor : 0;
    key[3] = g->color_keep;             /* SET_COLOR_MASK, inverted */
    key[4] = key[0] ? (uint32_t)blend_op(g->blend_equation) : 0;
    key[5] = s_enc_dt ? 1 : 0;          /* the open pass has a depth buffer */
    if (s_pso_last < s_pso_n && !memcmp(&s_pso[s_pso_last].k, &pk, sizeof pk))
        return s_pso[s_pso_last].pso;
    for (i = 0; i < s_pso_n; i++)
        if (!memcmp(&s_pso[i].k, &pk, sizeof pk)) {
            s_pso_last = i;
            return s_pso[i].pso;
        }

    pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = vs;
    pd.fragmentFunction = fs;
    ca = pd.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
    ca.blendingEnabled = key[0] ? YES : NO;
    if (key[0]) {
        ca.sourceRGBBlendFactor = blend_factor(key[1], 0);
        ca.destinationRGBBlendFactor = blend_factor(key[2], 0);
        ca.sourceAlphaBlendFactor = blend_factor(key[1], 1);
        ca.destinationAlphaBlendFactor = blend_factor(key[2], 1);
        ca.rgbBlendOperation = (MTLBlendOperation)key[4];
        ca.alphaBlendOperation = (MTLBlendOperation)key[4];
    }
    ca.writeMask = ((key[3] & 0x00FF0000u) ? 0 : MTLColorWriteMaskRed)
                 | ((key[3] & 0x0000FF00u) ? 0 : MTLColorWriteMaskGreen)
                 | ((key[3] & 0x000000FFu) ? 0 : MTLColorWriteMaskBlue)
                 | ((key[3] & 0xFF000000u) ? 0 : MTLColorWriteMaskAlpha);
    if (key[5]) {
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    }
    pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    [pd release];
    if (!pso) {
        static int said;
        if (!said++)
            fprintf(stderr, LOGP "pipeline failed: %s\n",
                    err ? [[err localizedDescription] UTF8String] : "?");
        return nil;
    }
    if (s_pso_n < PSO_MAX) {
        i = s_pso_n++;
    } else {
        static unsigned next;
        i = (int)(next++ % PSO_MAX);
        [s_pso[i].pso release];   /* an encoded draw keeps its own reference */
    }
    s_pso[i].k = pk;
    s_pso[i].pso = pso;
    s_pso_last = i;
    return pso;
}

/* ---- textures ------------------------------------------------
 *
 * Decoded to A8R8G8B8 on the CPU by the shared decode (nv2a_tex_decode), the
 * same texels the CPU sampler and the D3D11 backend see, and uploaded as
 * BGRA8Unorm (the same bytes). Cache key and invalidation as the D3D11
 * backend: offset + format + size + pitch + palette + levels; rehashed once
 * per flip and re-uploaded if it changed. A changed texture gets a new
 * MTLTexture rather than a replaceRegion, so a draw already encoded this frame
 * keeps the texels it was encoded with. */

typedef struct nv2a_stage Stage;
static Stage s_stage[4];

#define TEX_MAX_TEXELS (2048u * 2048u)
#define TEX_CACHE 512   /* array bound; entries in use: tex_cache_n() */
typedef struct {
    uint32_t       offset, color, width, height, pitch, palette, levels;
    uint32_t       bytes;       /* nv2a_tex_extent, for tex_cache_stale */
    uint32_t       gen, used;
    uint32_t       pack_gen;    /* nv2a_tex_pack_gen() it was uploaded under */
    uint64_t       hash;
    int            live;
    id<MTLTexture> tex;
} TexEntry;

static TexEntry       s_tc[TEX_CACHE];

/* Entries in use: RECOMP_DEBUG=tex_cache=n, 1..TEX_CACHE. Default 256, this
 * backend's value since its first frames (D3D11 uses 512); neither has been
 * measured against the other. */
static uint32_t tex_cache_n(void)
{
    static long n = -1;
    if (n < 0) {
        n = recomp_env_int(RENV_TEX_CACHE, 256);
        if (n < 1) n = 1;
        if (n > TEX_CACHE) n = TEX_CACHE;
    }
    return (uint32_t)n;
}
static uint32_t       s_tc_clock;

/* A write-back just rewrote guest [addr, addr + bytes): the cache entries over
 * it rehash at their next bind even inside this flip. tex_bind hashes a
 * texture once a flip, so without this a target drawn, sampled, redrawn and
 * sampled again in one flip (Burnout 3's speed blur redraws the 16-bit bloom
 * target and composites it) kept the first decode, and the blur showed the
 * dark bright-pass instead of the scene. */
static void tex_cache_stale(uint32_t addr, uint32_t bytes)
{
    uint32_t n = tex_cache_n(), lo = addr & 0x07FFFFFFu, hi = lo + bytes, i;
    for (i = 0; i < n; i++) {
        TexEntry *c = &s_tc[i];
        uint32_t clo = c->offset & 0x07FFFFFFu;
        if (c->live && clo < hi && lo < clo + c->bytes)
            c->gen = nv2a_pb_gpu_state()->flips - 1u;
    }
}

static uint32_t      *s_texels;
static size_t         s_texels_cap;
static id<MTLTexture> s_black, s_magenta;

static id<MTLTexture> solid_texture(uint32_t argb)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1
                                 mipmapped:NO];
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
    [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&argb
         bytesPerRow:4];
    return t;
}

static int no_mips(void)
{
    static int off = -1;
    if (off < 0)
        off = recomp_env(RENV_METAL_NO_MIPS) != NULL;
    return off;
}

static void build_stages(void)
{
    const uint32_t *regs = nv2a_pb_tex_regs();
    const uint8_t  *sets = nv2a_pb_tex_regs_set();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    int n;

    for (n = 0; n < 4; n++) {
        Stage *t = &s_stage[n];
        nv2a_stage_decode(n, &regs[(0x40u * n) / 4], &sets[(0x40u * n) / 4],
                          v->shader_set, v->shader_prog, nv2a_pb_dma_resolve,
                          NV2A_STAGE_P8_PALETTE | NV2A_STAGE_PITCH_LINEAR
                          | (no_mips() ? 0u : NV2A_STAGE_MIPS), t);
        if (t->mode == 5)
            t->mode = 0;    /* CLIP_PLANE: no texture; the kill is clip_mode's */
    }
}

static uint64_t tex_hash_pal(const uint8_t *mem, const Stage *t, uint32_t bytes)
{
    uint64_t h = nv2a_tex_hash(mem + t->offset, bytes, NV2A_TEX_HASH_SEED);
    if (t->color == NV2A_TEX_P8)
        h ^= nv2a_tex_hash(mem + t->palette, t->pal_len * 4u, NV2A_TEX_HASH_SEED) * 31u;
    return h;
}

/* A texture of `levels` levels from src (level 0 first), or nil. `bc3`: src
 * is BC3 blocks (a pack entry's payload, XRTP_FLAG_BC3), uploaded as they
 * are with a block-row pitch; otherwise A8R8G8B8 texels into BGRA8. */
static id<MTLTexture> tex_make(uint32_t width, uint32_t height, uint32_t levels,
                               const uint32_t *src, int bc3)
{
    MTLTextureDescriptor *td;
    id<MTLTexture> tex;
    const uint8_t *p = (const uint8_t *)src;
    uint32_t l;
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:bc3 ? MTLPixelFormatBC3_RGBA
                                                                     : MTLPixelFormatBGRA8Unorm
                                                            width:width
                                                           height:height
                                                        mipmapped:levels > 1];
    if (levels > 1)
        td.mipmapLevelCount = levels;
    td.usage = MTLTextureUsageShaderRead;
    tex = [s_dev newTextureWithDescriptor:td];
    if (!tex)
        return nil;
    for (l = 0; l < levels; l++) {
        uint32_t w = nv2a_tex_level_dim(width, l), h = nv2a_tex_level_dim(height, l);
        uint32_t row = bc3 ? (w + 3u) / 4u * 16u : w * 4u;
        uint32_t rows = bc3 ? (h + 3u) / 4u : h;
        [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:l withBytes:p
               bytesPerRow:row];
        p += (size_t)row * rows;
    }
    return tex;
}

/* `hash` is the entry's texel hash: with a texture pack loaded
 * (enhancements, textures.pack) an entry it holds is uploaded from the
 * pack's texels at the pack's size instead of the guest's. If the device
 * refuses a texture of the entry's size, the guest texels go up instead:
 * the pack only ever adds detail, never a black texture. */
static id<MTLTexture> tex_upload(const Stage *t, uint64_t hash)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    size_t n = nv2a_tex_texels(t->width, t->height, t->levels);
    const uint32_t *pal = NULL;
    id<MTLTexture> tex = nil;

    if (nv2a_tex_pack_count()) {
        struct nv2a_tex_pack_entry *pe = nv2a_tex_pack_find(hash, t->width, t->height);
        const uint32_t *src = NULL;
        if (pe && pe->bc3 && !s_bc) {
            static int said;
            if (!said++)
                fprintf(stderr, LOGP "textures.pack: this GPU has no BC3; BC3 entries "
                                     "go up from the guest\n");
        } else if (pe) {
            src = nv2a_tex_pack_texels(pe);
        }
        if (src)
            tex = tex_make(pe->width, pe->height, pe->levels, src, pe->bc3);
        /* A refused entry is a miss too, tagged so in the trace. */
        if (!tex)
            nv2a_tex_pack_miss(hash, t->width, t->height, t->color, t->levels);
    }
    if (!tex) {
        if (n > s_texels_cap) {
            uint32_t *p = (uint32_t *)realloc(s_texels, n * 4);
            if (!p)
                return nil;
            s_texels = p;
            s_texels_cap = n;
        }
        if (t->color == NV2A_TEX_P8)
            pal = (const uint32_t *)(mem + t->palette);
        nv2a_tex_decode(mem + t->offset, t->color, t->width, t->height, t->pitch,
                        t->levels, pal, pal ? t->pal_len : 0, s_texels);
        tex = tex_make(t->width, t->height, t->levels, s_texels, 0);
        if (!tex)
            return nil;
    }
    s_tex_uploads++;
    return tex;
}

static id<MTLTexture> tex_bind(const Stage *t)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bytes = 0, i;
    TexEntry *e = NULL, *victim = &s_tc[0];

    if (t->valid && (uint64_t)t->width * t->height <= TEX_MAX_TEXELS)
        bytes = nv2a_tex_extent(t->color, t->width, t->height, t->pitch, t->levels);
    /* The guest bytes are about to be hashed and decoded: a render target
     * over them that rt_texture did not bind (another format, no_rtt, an
     * evicted name) is written back first, so the decode sees what was
     * drawn there. */
    if (bytes) {
        uint32_t lo = t->offset & 0x07FFFFFFu, hi = lo + bytes;
        for (i = 0; i < RT_MAX; i++) {
            RenderTarget *rt = &s_rt[i];
            uint32_t rlo = rt->addr & 0x07FFFFFFu;
            if (rt->tex && rt->dirty && rt->pitch
                    && lo < rlo + rt->pitch * rt->h && rlo < hi) {
                if (rt_drop_if_stale(rt, "sync"))
                    continue;
                s_decode_syncs++;
                if (recomp_env(RENV_METAL_TRACE))
                    fprintf(stderr, LOGP "decode sync: target 0x%08X %ux%u (%u bytes) for"
                            " texture 0x%08X fmt 0x%02X %ux%u (%u bytes), flip %llu\n",
                            rt->offset, rt->w, rt->h, rt->pitch * rt->h, t->offset,
                            t->color, t->width, t->height, bytes,
                            (unsigned long long)s_presents);
                rt_sync(rt, rt->pitch);
            }
        }
    }
    if (!bytes) {
        static uint8_t said[256];
        if ((uint64_t)t->width * t->height > TEX_MAX_TEXELS)
            return s_black;
        if (!said[t->color & 0xFF]) {
            said[t->color & 0xFF] = 1;
            fprintf(stderr, LOGP "\x1b[1;35mtexture format 0x%02X (%ux%u at"
                    " 0x%08X) not decoded: drawn magenta\x1b[0m\n",
                    t->color, t->width, t->height, t->offset);
        }
        return s_magenta;
    }
    uint32_t n = tex_cache_n();
    for (i = 0; i < n; i++) {
        TexEntry *c = &s_tc[i];
        if (c->live && c->offset == t->offset && c->color == t->color
                && c->width == t->width && c->height == t->height
                && c->pitch == t->pitch && c->palette == t->palette
                && c->levels == t->levels) {
            e = c;
            break;
        }
        if (!c->live || (victim->live && c->used < victim->used))
            victim = c;
    }
    if (e) {
        /* A pack toggle (nv2a_tex_pack_gen) changes what the same texels
         * upload as: re-upload as if the hash had moved. */
        uint32_t pg = nv2a_tex_pack_gen();
        if (e->gen != g->flips || e->pack_gen != pg) {
            uint64_t h = tex_hash_pal(mem, t, bytes);
            s_tex_rehash++;
            e->gen = g->flips;
            if (h != e->hash || e->pack_gen != pg) {
                id<MTLTexture> nt = tex_upload(t, h);
                s_tex_changed++;
                /* The hash moves only with the texels: a failed upload keeps
                 * the old texture and is tried again next flip. */
                if (!nt)
                    return s_black;
                e->hash = h;
                e->pack_gen = pg;
                [e->tex release];
                e->tex = nt;
            }
        }
    } else {
        id<MTLTexture> nt;
        uint64_t h = tex_hash_pal(mem, t, bytes);
        e = victim;
        [e->tex release];
        e->tex = nil;
        e->live = 0;
        nt = tex_upload(t, h);
        if (!nt)
            return s_black;
        e->offset = t->offset;
        e->color = t->color;
        e->width = t->width;
        e->height = t->height;
        e->pitch = t->pitch;
        e->palette = t->palette;
        e->levels = t->levels;
        e->bytes = bytes;
        e->gen = g->flips;
        e->hash = h;
        e->pack_gen = nv2a_tex_pack_gen();
        e->tex = nt;
        e->live = 1;
    }
    e->used = ++s_tc_clock;
    return e->tex;
}

/* NV2A wrap modes: 1 wrap, 2 mirror, 3 clamp to edge, 4 border, 5 clamp.
 * Border takes the stage's SET_TEXTURE_BORDER_COLOR (nv2a_stage_decode),
 * but a Metal sampler offers only three border colours: border_colour picks
 * the one the register names, and any other colour stays a clamp to edge
 * (logged once). Burnout 3's is 0x00000000, transparent black. */
static MTLSamplerAddressMode address_mode(uint32_t m, int border)
{
    switch (m) {
    case 1:  return MTLSamplerAddressModeRepeat;
    case 2:  return MTLSamplerAddressModeMirrorRepeat;
    case NV2A_ADDR_BORDER:
        if (border)
            return MTLSamplerAddressModeClampToBorderColor;
        return MTLSamplerAddressModeClampToEdge;
    default: return MTLSamplerAddressModeClampToEdge;
    }
}

/* MTLSamplerBorderColor + 1 for an A8R8G8B8 border Metal can give, else
 * 0. */
static int border_colour(uint32_t argb)
{
    static int said;
    switch (argb) {
    case 0x00000000u: return 1 + MTLSamplerBorderColorTransparentBlack;
    case 0xFF000000u: return 1 + MTLSamplerBorderColorOpaqueBlack;
    case 0xFFFFFFFFu: return 1 + MTLSamplerBorderColorOpaqueWhite;
    }
    if (!said) {
        said = 1;
        fprintf(stderr, LOGP "texture border colour 0x%08X has no Metal sampler"
                " colour: clamping to the edge instead\n", argb);
    }
    return 0;
}

/* SET_TEXTURE_FILTER as the D3D11 backend reads it (filter_linear,
 * filter_mip there). RECOMP_METAL_POINT=1 forces nearest texels. */
#define filter_linear nv2a_tex_filter_linear   /* nv2a_backend_common.h */
#define filter_mip    nv2a_tex_filter_mip
#define lod_bias      nv2a_tex_lod_bias

#define SMP_MAX 128
static struct { uint64_t key; id<MTLSamplerState> s; } s_smp[SMP_MAX];
static int s_smp_n;

static id<MTLSamplerState> sampler(const Stage *t, int bump_source)
{
    static int point = -1;
    MTLSamplerDescriptor *sd;
    id<MTLSamplerState> st;
    uint32_t u = t->addr_u & 0xF, v = t->addr_v & 0xF;
    int mn, mg, mip, i;
    unsigned an;
    uint64_t key;
    int bc = 0;

    if (point < 0)
        point = recomp_env(RENV_METAL_POINT) != NULL;
    if (u == NV2A_ADDR_BORDER || v == NV2A_ADDR_BORDER)
        bc = border_colour(t->border);
    mn = !point && filter_linear(t->filter, 0);
    mg = !point && filter_linear(t->filter, 1);
    mip = t->levels > 1 ? filter_mip(t->filter) : 0;
    /* render.aniso, on the D3D11 sampler's rule (nv2a_stage_aniso) and key
     * bit: a bump source in one draw and plain in the next gets its own. */
    an = nv2a_stage_aniso(t, mip, mn, bump_source);
    key = u | v << 4 | (uint64_t)mn << 8 | (uint64_t)mg << 9 | (uint64_t)mip << 10
        | (uint64_t)an << 35 | (uint64_t)bc << 40;
    if (mip)
        key |= (uint64_t)t->lod_min << 25 | (uint64_t)t->lod_max << 29;
    for (i = 0; i < s_smp_n; i++)
        if (s_smp[i].key == key)
            return s_smp[i].s;
    sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = mn ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.magFilter = mg ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = mip == 2 ? MTLSamplerMipFilterLinear
                 : mip == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterNotMipmapped;
    sd.sAddressMode = address_mode(u, bc);
    sd.tAddressMode = address_mode(v, bc);
    sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
    if (bc)
        sd.borderColor = (MTLSamplerBorderColor)(bc - 1);
    sd.lodMinClamp = mip ? (float)t->lod_min : 0.0f;
    sd.lodMaxClamp = mip ? (float)t->lod_max : 0.0f;
    if (an) {
        /* D3D11_FILTER_ANISOTROPIC filters min, mag and mip linearly; set
         * the same here so both backends apply one filter. */
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.mipFilter = MTLSamplerMipFilterLinear;
        sd.maxAnisotropy = an;
    }
    st = [s_dev newSamplerStateWithDescriptor:sd];
    [sd release];
    if (!st)
        return s_smp_n ? s_smp[0].s : nil;
    if (s_smp_n < SMP_MAX) {
        i = s_smp_n++;
    } else {
        static unsigned next;
        i = (int)(next++ % SMP_MAX);
        [s_smp[i].s release];
    }
    s_smp[i].key = key;
    s_smp[i].s = st;
    return st;
}

/* ---- shaders (M2): vertex programs and register combiners ------------------
 *
 * As the D3D11 backend's vertex_shader and combiner_ps: the program is
 * parsed from the slots it starts at (d3d8_vsh_parse) and emitted as MSL
 * (d3d8_vsh_generate_msl, screen-space epilogue), keyed by
 * nv2a_vsh_program_hash; the combiner registers become NV2ACombinerState
 * (d3d8_combiners_from_regs) and MSL (d3d8_combiners_generate_msl), keyed by
 * the state's bytes. Each compiles once, with s_opts (fast math off). A
 * source that fails to compile is logged with its text, counted
 * (s_compile_errors, in the present log) and remembered as bad. */

static uint64_t s_compile_errors, s_vs_compiled, s_fs_compiled;

static id<MTLFunction> compile_fn(const char *src, NSString *name, const char *what)
{
    NSError *err = nil;
    id<MTLLibrary> lib;
    id<MTLFunction> fn;

    lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:src]
                              options:s_opts error:&err];
    if (!lib) {
        s_compile_errors++;
        if (s_compile_errors <= 4)
            fprintf(stderr, LOGP "\x1b[1;31m%s: MSL compile failed: %s\x1b[0m\n%s\n",
                    what, err ? [[err localizedDescription] UTF8String] : "?", src);
        return nil;
    }
    fn = [lib newFunctionWithName:name];
    [lib release];
    if (!fn) {
        s_compile_errors++;
        fprintf(stderr, LOGP "%s: no function %s\n", what, [name UTF8String]);
    }
    return fn;
}

#define VS_MAX 512
typedef struct {
    uint32_t        hash;
    uint16_t        inputs;
    int             used, bad;
    id<MTLFunction> fn;
} VsEntry;
static VsEntry s_vs[VS_MAX];

static VsEntry *vs_cache(void)
{
    static NV2AVshProgram parsed;
    static char msl[65536];
    static struct { int valid; uint32_t dwords, start; VsEntry *e; } memo;
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t hash;
    VsEntry *e = NULL;
    char what[32];
    int i;

    /* Programs only change by upload (prog_dwords) or a new start slot. */
    if (memo.valid && memo.dwords == v->prog_dwords && memo.start == v->start)
        return memo.e;
    hash = nv2a_vsh_program_hash(v, NULL);
    if (!hash)
        hash = 1;
    for (i = 0; i < VS_MAX; i++) {
        if (s_vs[i].used && s_vs[i].hash == hash) {
            e = &s_vs[i];
            break;
        }
    }
    if (!e) {
        for (i = 0; i < VS_MAX && s_vs[i].used; i++)
            ;
        if (i == VS_MAX)
            return NULL;   /* full: a title with this many programs wants LRU */
        e = &s_vs[i];
        e->used = 1;
        e->hash = hash;
        e->bad = 1;
        d3d8_vsh_parse(&v->prog[v->start][0], NV2A_VSH_SLOTS - (int)v->start, &parsed);
        snprintf(what, sizeof what, "program %08X", hash);
        if (d3d8_vsh_generate_msl(&parsed, D3D8_MSL_SCREEN_SPACE, msl, (int)sizeof msl) <= 0) {
            s_compile_errors++;
            fprintf(stderr, LOGP "%s: no MSL\n", what);
        } else if ((e->fn = compile_fn(msl, @"vs_main", what)) != nil) {
            e->inputs = parsed.inputs_read;
            e->bad = 0;
            s_vs_compiled++;
        }
    }
    memo.valid = 1;
    memo.dwords = v->prog_dwords;
    memo.start = v->start;
    memo.e = e->bad ? NULL : e;
    return memo.e;
}

#define FS_MAX 512
typedef struct {
    uint32_t          hash;
    int               bad;
    NV2ACombinerState st;
    id<MTLFunction>   fn;
} FsEntry;
static FsEntry *s_fs;   /* FS_MAX, allocated on first use */
static int      s_fs_n;

static uint32_t fnv(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 0x811c9dc5u;
    while (n--) {
        h ^= *b++;
        h *= 0x01000193u;
    }
    return h;
}

/* The fragment function for the batch: its register combiners when they
 * are programmed (v->rc_set and a stage count), else fs_basic. As the D3D11
 * setup_pixel, only 2D textures are bound, so every sampling mode is passed
 * to the combiners as 2D. */
static id<MTLFunction> fs_select(const struct nv2a_pb_vsh *v)
{
    static char msl[65536];
    static struct { int valid; uint32_t r[38]; id<MTLFunction> fn; } memo;
    NV2ACombinerState st;
    uint32_t key[38], modes = 0, inputs = 0, hash;
    FsEntry *e = NULL;
    char what[32];
    int n;

    if (!(v->rc_set && (v->rc_ctl & 0xFF)))
        return s_fs_basic;
    for (n = 0; n < 4; n++) {
        /* As D3D11: BUMPENVMAP(_LUMINANCE) from stage 1 on, reading the
         * stage OTHER_STAGE_INPUT names. */
        uint32_t m = nv2a_stage_ps_mode(&s_stage[n], n);
        if (m == 6 || m == 7)
            inputs |= (uint32_t)nv2a_stage_input(n, v->other_stage_input) << (4 * n);
        modes |= m << (5 * n);
    }
    memcpy(key, v->rc_cicw, 32);
    memcpy(key + 8, v->rc_aicw, 32);
    memcpy(key + 16, v->rc_cocw, 32);
    memcpy(key + 24, v->rc_aocw, 32);
    key[32] = v->rc_ctl;
    key[33] = v->rc_fcw0;
    key[34] = v->rc_fcw1;
    key[35] = modes;
    /* render.scale changes at run time: each scale class has its own text */
    key[36] = nv2a_host_render_scale() > 1;
    key[37] = inputs;
    if (memo.valid && !memcmp(memo.r, key, sizeof key))
        return memo.fn;

    memset(&st, 0, sizeof st);
    d3d8_combiners_from_regs(v->rc_cicw, v->rc_aicw, v->rc_cocw, v->rc_aocw,
                             v->rc_ctl, v->rc_fcw0, v->rc_fcw1, modes, &st);
    st.fog_input = 1;   /* FOG.a is the interpolated vertex fog factor */
    st.rect_snap = (int)key[36];   /* nv2a_snap_uv; in the hash via st */
    for (n = 0; n < 4; n++)
        st.tex_input[n] = (int)((inputs >> (4 * n)) & 0xF);
    st.bump_on = 1;
    hash = fnv(&st, sizeof st);
    if (!s_fs && !(s_fs = (FsEntry *)calloc(FS_MAX, sizeof *s_fs)))
        return s_fs_basic;
    for (n = 0; n < s_fs_n; n++)
        if (s_fs[n].hash == hash && !memcmp(&s_fs[n].st, &st, sizeof st)) {
            e = &s_fs[n];
            break;
        }
    if (!e) {
        if (s_fs_n == FS_MAX)
            return s_fs_basic;
        e = &s_fs[s_fs_n++];
        e->hash = hash;
        e->st = st;
        e->bad = 1;
        snprintf(what, sizeof what, "combiners %08X", hash);
        if (d3d8_combiners_generate_msl(&st, msl, (int)sizeof msl) <= 0) {
            s_compile_errors++;
            fprintf(stderr, LOGP "%s: no MSL\n", what);
        } else if ((e->fn = compile_fn(msl, @"fs_main", what)) != nil) {
            e->bad = 0;
            s_fs_compiled++;
        }
    }
    memcpy(memo.r, key, sizeof key);
    memo.valid = 1;
    memo.fn = e->bad ? s_fs_basic : e->fn;   /* as D3D11: the basic PS */
    return memo.fn;
}

/* ---- depth / stencil state ---------------------------------------------------
 *
 * As the D3D11 backend's depth_state: SET_DEPTH_FUNC and SET_STENCIL_FUNC
 * are GL compare codes in MTLCompareFunction's order (nv2a_cmp_from_gl), the
 * stencil ops in MTLStencilOperation's (nv2a_stencil_op_from_gl). Depth and
 * stencil each only when enabled; one state per distinct key. */
#define DSS_MAX 64
static struct { uint64_t key; id<MTLDepthStencilState> dss; } s_dss[DSS_MAX];
static int s_dss_n;

static id<MTLDepthStencilState> depth_state(int depth, int stencil)
{
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t f = nv2a_cmp_from_gl(v->depth_func);
    uint64_t key = 0;
    MTLDepthStencilDescriptor *dd;
    id<MTLDepthStencilState> st;
    int i;

    if (depth)
        key |= 1u | f << 1 | (v->depth_mask ? 1u : 0u) << 4;
    if (stencil)
        key |= (uint64_t)(1u | (v->stencil_func & 7u) << 1
                          | nv2a_stencil_op_from_gl(v->stencil_op[0]) << 4
                          | nv2a_stencil_op_from_gl(v->stencil_op[1]) << 8
                          | nv2a_stencil_op_from_gl(v->stencil_op[2]) << 12
                          | (v->stencil_rmask & 0xFFu) << 16
                          | (v->stencil_wmask & 0xFFu) << 24) << 8;
    for (i = 0; i < s_dss_n; i++)
        if (s_dss[i].key == key)
            return s_dss[i].dss;
    dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = depth ? (MTLCompareFunction)f : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = depth && v->depth_mask ? YES : NO;
    if (stencil) {
        MTLStencilDescriptor *sd = [[MTLStencilDescriptor alloc] init];
        sd.stencilCompareFunction = (MTLCompareFunction)(v->stencil_func & 7u);
        sd.stencilFailureOperation = (MTLStencilOperation)nv2a_stencil_op_from_gl(v->stencil_op[0]);
        sd.depthFailureOperation = (MTLStencilOperation)nv2a_stencil_op_from_gl(v->stencil_op[1]);
        sd.depthStencilPassOperation = (MTLStencilOperation)nv2a_stencil_op_from_gl(v->stencil_op[2]);
        sd.readMask = v->stencil_rmask & 0xFFu;
        sd.writeMask = v->stencil_wmask & 0xFFu;
        dd.frontFaceStencil = sd;               /* GL one-sided stencil */
        dd.backFaceStencil = sd;
        [sd release];
    }
    st = [s_dev newDepthStencilStateWithDescriptor:dd];
    [dd release];
    if (!st)
        return nil;
    if (s_dss_n >= DSS_MAX)
        return st;   /* uncached: a title with this many wants LRU (leaks) */
    s_dss[s_dss_n].key = key;
    s_dss[s_dss_n].dss = st;
    s_dss_n++;
    return st;
}

/* ---- render-to-texture ---------------------------------------------------------
 *
 * A stage whose texture is a surface this backend draws into samples the
 * render target itself, as the D3D11 backend's rt_texture: guest memory
 * under it only changes at a flip's write-back (and then only for the
 * present surface). 32-bit, non-compressed formats only; the low 27 bits of
 * the address compare, so either memory window matches. The target being
 * drawn into is sampled through a copy (s_self), made by a blit between
 * passes. RECOMP_METAL_NO_RTT=1 decodes guest memory instead (A/B). */
static RenderTarget s_self;   /* tex, w, h: the copy; offset: whose */

static RenderTarget *rt_texture(const Stage *t, RenderTarget *cur)
{
    static int off = -1;
    RenderTarget *self = NULL;
    int i;

    if (off < 0)
        off = recomp_env(RENV_METAL_NO_RTT) != NULL;
    if (off || !t->offset || d3d8_format_dxt_block_bytes(t->color)
            || nv2a_tex_texel_bytes(nv2a_tex_linear_twin(t->color)) != 4)
        return NULL;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->tex || ((rt->addr ^ t->offset) & 0x07FFFFFFu))
            continue;
        if (rt_drop_if_stale(rt, "bind"))
            continue;   /* the texture decodes from the title's bytes */
        if (rt == cur) {
            self = rt;
            continue;
        }
        return rt;
    }
    if (!self)
        return NULL;
    /* copyFromTexture:toTexture: wants equal sizes: the source's host size. */
    if (!s_self.tex || s_self.w != self->w || s_self.h != self->h
            || s_self.hw != self->hw || s_self.hh != self->hh) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:self->hw height:self->hh mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        [s_self.tex release];
        s_self.tex = [s_dev newTextureWithDescriptor:td];
        if (!s_self.tex)
            return NULL;
        s_self.w = self->w;
        s_self.h = self->h;
        s_self.hw = self->hw;
        s_self.hh = self->hh;
    }
    {
        id<MTLBlitCommandEncoder> b;
        end_encoder();
        ensure_cmd();
        b = [s_cmd blitCommandEncoder];
        [b copyFromTexture:self->tex toTexture:s_self.tex];
        [b endEncoding];
    }
    s_self.offset = self->offset;
    s_self_copies++;
    {
        static int said;
        if (!said++)
            fprintf(stderr, LOGP "surface 0x%08X sampled while drawn into:"
                    " bound as a copy\n", self->offset);
    }
    return &s_self;
}

/* ---- draws ------------------------------------------------------------------- */

static void metal_on_draw(const struct nv2a_pb_draw *d)
{
    /* The expanded list: at most three entries per index (a fan or a quad
     * strip), grown to the largest batch so far rather than sized to
     * NV_MAX_INDICES. */
    static uint32_t *idx;
    static uint32_t idx_cap;
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    enum nv2a_topology topo;
    MTLPrimitiveType ptype;
    RenderTarget *rt;
    id<MTLRenderPipelineState> pso;
    id<MTLFunction> vfn, ffn;
    struct nv2a_vp_consts vc;
    MsPsConsts mpc;
    NSUInteger vb_off, ib_off, vbytes;
    uint32_t lo = 0xFFFF, hi = 0, count, nidx, i, k, nf4 = 0;
    int slot[NV2A_VS_MAX_INPUTS];
    float *vf;
    void *ip;
    int cull = 0, cull_all = 0;
    unsigned bump_src;
    /* Stencil only on a Z24S8 zeta surface, as on the CPU. */
    int use_s = v->stencil_enable && ((g->format >> 4) & 0xF) == 2;
    int use_z = v->depth_enable || use_s;
    id<MTLTexture> stex[4];

    if (!init())
        return;
    /* A batch that writes nothing is dropped, unless a visibility test is
     * counting it: a test's bounding draws typically mask every write. */
    if (g->color_keep == 0xFFFFFFFFu && !(v->depth_enable && v->depth_mask) && !use_s
            && !s_occ_q)
        return;
    if (!d->idx_count) {
        s_draws_skipped++;
        return;
    }
    @autoreleasepool {
        /* Vertex function and the float4 inputs it reads, in order. */
        if (d->program) {
            VsEntry *e = vs_cache();
            s_draws_prog++;
            if (!e) {
                s_draws_skipped++;
                return;
            }
            vfn = e->fn;
            for (i = 0; i < NV2A_VS_MAX_INPUTS; i++)
                if (e->inputs & (1u << i))
                    slot[nf4++] = (int)i;
        } else {
            vfn = s_vs_pass;
            for (k = 0; k < VTX_F4; k++)
                slot[nf4++] = s_vtx_attr[k];
        }
        rt = surface();
        if (!rt) {
            s_draws_skipped++;
            return;
        }
        build_stages();
        bump_src = nv2a_bump_sources(s_stage, v->rc_set && (v->rc_ctl & 0xFF),
                                     v->other_stage_input);
        {
            static int bump_trace = -1;
            if (bump_trace < 0)
                bump_trace = recomp_env(RENV_BUMP_TRACE) != NULL;
            /* Only a combiner draw samples a bump stage (the basic shader
             * reads every stage as plain 2D), so only those are logged. */
            if (bump_trace && v->rc_set && (v->rc_ctl & 0xFF))
                nv2a_bump_trace(v, s_stage, g->flips);
        }
        nv2a_ps_consts_fill(v, s_stage, &mpc.b);
        for (k = 0; k < 4; k++) {
            /* As the D3D11 MipLODBias: only a mipmapped stage has one. */
            const Stage *t = &s_stage[k];
            float b = t->levels > 1 && filter_mip(t->filter) ? lod_bias(t->filter) : 0.0f;
            mpc.tex_bias[k] = b > 15.99f ? 15.99f : b < -16.0f ? -16.0f : b;
        }
        ffn = fs_select(v);
        for (i = 0; i < d->idx_count; i++) {
            if (d->idx[i] < lo) lo = d->idx[i];
            if (d->idx[i] > hi) hi = d->idx[i];
        }
        count = hi - lo + 1;
        if (d->idx_count * 3u > idx_cap) {
            uint32_t *grown = (uint32_t *)realloc(idx, (size_t)d->idx_count * 12u);
            if (!grown) {
                s_draws_skipped++;
                return;
            }
            idx = grown;
            idx_cap = d->idx_count * 3u;
        }
        nidx = nv2a_prim_to_list(d->prim, d->idx, d->idx_count, lo, idx, &topo);
        if (!nidx) {
            s_draws_skipped++;
            return;
        }
        ptype = topo == NV2A_TOPO_POINTS ? MTLPrimitiveTypePoint
              : topo == NV2A_TOPO_LINES  ? MTLPrimitiveTypeLine
              :                            MTLPrimitiveTypeTriangle;
        if (v->cull_enable) {     /* as the D3D11 backend's rasterizer_state */
            if (v->cull_face == 0x405)      cull = 1;   /* BACK */
            else if (v->cull_face == 0x404) cull = 2;   /* FRONT */
            else if (v->cull_face == 0x408) cull_all = 1;
        }
        if (cull_all && ptype == MTLPrimitiveTypeTriangle) {
            s_draws_culled++;
            return;
        }

        /* Rings first: an overflow flushes, which ends the open pass. */
        vbytes = (NSUInteger)count * nf4 * 16u;
        if (vbytes < 16)
            vbytes = 16;
        vf = (float *)ring_alloc(s_vb, s_vb_size, &s_vb_pos, vbytes, &vb_off);
        {
            uint64_t fl = s_flushes;
            ip = vf ? ring_alloc(s_ib, s_ib_size, &s_ib_pos, (NSUInteger)nidx * 4u, &ib_off)
                    : NULL;
            if (vf && ip && fl != s_flushes) {
                /* The index ring overflowed after the vertices were placed,
                 * and its flush reset the vertex ring: place them again. */
                vf = (float *)ring_alloc(s_vb, s_vb_size, &s_vb_pos, vbytes, &vb_off);
            }
        }
        if (!vf || !ip) {
            s_draws_skipped++;
            return;
        }
        /* Each input as float4, fetched as the CPU path does; a disabled
         * one (or one without data) is its SET_VERTEX_DATA value. Stride 0
         * is an array of one element (fetch_attr). */
        for (k = 0; k < nf4; k++) {
            const VertexAttr *a = &d->attr[slot[k]];
            int on = a->size != 0;
            for (i = 0; i < count; i++) {
                float *o = vf + ((size_t)i * nf4 + k) * 4;
                if (!on || !fetch(d, a, lo + i, o))
                    memcpy(o, v->inl[slot[k]], 16);
            }
        }
        memcpy(ip, idx, (size_t)nidx * 4u);

        nv2a_vp_consts_fill(v, g->format, rt->w, rt->h, &vc);

        /* Depth: the zeta surface's buffer, attached to this target's
         * passes from now on (a new pass if the open one lacks it). */
        if (use_z) {
            DepthTarget *dt = depth_target(rt->w, rt->h);
            if (dt && rt->dt != dt) {
                rt->dt = dt;
                if (s_enc_rt == rt)
                    end_encoder();
            }
        }
        /* Textures before the pass: a self copy is a blit between passes. */
        {
            int textured = 0;
            for (k = 0; k < 4; k++) {
                const Stage *t = &s_stage[k];
                RenderTarget *src;
                stex[k] = s_black;
                if (!t->mode || t->mode == 4)
                    continue;
                textured = 1;
                if ((src = rt_texture(t, rt)) != NULL) {
                    stex[k] = src->tex;
                    nv2a_ps_consts_rt(&mpc.b, k, t, src->w, src->h);
                    s_rtt_binds++;
                } else {
                    stex[k] = tex_bind(t);
                }
            }
            if (textured)
                s_draws_tex++;
        }

        if (!s_enc || s_enc_rt != rt)
            begin_encoder(rt, 0, NULL);
        pso = pipeline(vfn, ffn);
        if (!pso) {
            s_draws_skipped++;
            return;
        }
        [s_enc setRenderPipelineState:pso];
        if (s_enc_dt) {
            id<MTLDepthStencilState> dss = use_z ? depth_state(v->depth_enable, use_s)
                                                 : depth_state(0, 0);
            [s_enc setDepthStencilState:dss];
            [s_enc setStencilReferenceValue:use_s ? (v->stencil_ref & 0xFFu) : 0u];
            if (use_z)
                s_draws_z++;
        }
        {
            MTLViewport vp = {0.0, 0.0, (double)rt->hw, (double)rt->hh, 0.0, 1.0};
            MTLScissorRect sr;
            uint32_t b[4], hb[4];
            [s_enc setViewport:vp];
            /* The surface clip, as the hardware scissors (nv2a_draw_box). */
            if (!nv2a_draw_box(g, rt->w, rt->h, b)) {
                s_draws_skipped++;
                return;
            }
            nv2a_host_box(b, rt->w, rt->h, rt->hw, rt->hh, hb);
            sr.x = hb[0];
            sr.y = hb[1];
            sr.width = hb[2] - hb[0];
            sr.height = hb[3] - hb[1];
            [s_enc setScissorRect:sr];
        }
        [s_enc setFrontFacingWinding:v->front_face != 0x900
                                     ? MTLWindingCounterClockwise : MTLWindingClockwise];
        [s_enc setCullMode:cull == 1 ? MTLCullModeBack
                         : cull == 2 ? MTLCullModeFront : MTLCullModeNone];
        {
            float bf[4];
            (void)nv2a_blend_constant(g->blend_enable, g->blend_sfactor,
                                      g->blend_dfactor, g->blend_color, bf, &(int){0});
            [s_enc setBlendColorRed:bf[0] green:bf[1] blue:bf[2] alpha:bf[3]];
        }
        [s_enc setVertexBuffer:s_vb offset:vb_off atIndex:0];
        if (d->program) {
            /* c[192] is 3 KB: under setVertexBytes' 4 KB. */
            [s_enc setVertexBytes:v->c length:sizeof v->c atIndex:1];
            [s_enc setVertexBytes:&vc length:sizeof vc atIndex:2];
        } else {
            [s_enc setVertexBytes:&vc length:sizeof vc atIndex:1];
        }
        [s_enc setFragmentBytes:&mpc length:sizeof mpc atIndex:0];
        for (k = 0; k < 4; k++) {
            [s_enc setFragmentTexture:stex[k] atIndex:k];
            [s_enc setFragmentSamplerState:sampler(&s_stage[k], bump_src >> k & 1)
                                     atIndex:k];
        }
        [s_enc drawIndexedPrimitives:ptype indexCount:nidx indexType:MTLIndexTypeUInt32
                         indexBuffer:s_ib indexBufferOffset:ib_off];
        s_draws++;
    }
}

/* ---- flip ---------------------------------------------------------------------- */

/* The render target for the surface the walker picked: the one at its
 * offset and pitch (a field buffer and a frame can share an offset),
 * whatever its size. A larger one covers a smaller frame; a smaller
 * one (a buffer drawn only below the display mode's frame) is grown by
 * present_extent. NULL when this backend never drew there; the guest's own
 * bytes are presented then. */
static RenderTarget *present_target(uint32_t surface_offset)
{
    const struct nv2a_pb_present *p = nv2a_pb_present_state();
    int i;

    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->tex && surface_offset && rt->offset == surface_offset
                && (!p->offset || rt->pitch == p->pitch)) {
            rt->presented = 1;
            return rt;
        }
    }
    return NULL;
}

/* The part of rt a flip shows, in guest pixels: the walker's present extent
 * when it names this surface (D3D11's present_extent). A target grown for a
 * larger clip (rt_grow) can be bigger than the frame; the rest of it is not
 * the frame. */
static void present_extent(RenderTarget *rt, uint32_t *w, uint32_t *h)
{
    const struct nv2a_pb_present *p = nv2a_pb_present_state();
    /* The walker publishes the display mode's frame, which a title that only
     * ever drew this buffer at a smaller clip has no target that big for:
     * grow it (as a larger clip would) rather than show less than the CPU
     * path, which reads the whole frame from guest memory. A failed grow
     * shows what there is. */
    if (p->offset && p->offset == rt->offset && p->pitch == rt->pitch && p->w && p->h
            && (p->w > rt->w || p->h > rt->h))
        rt_grow(rt, rt->w > p->w ? rt->w : p->w, rt->h > p->h ? rt->h : p->h);
    *w = rt->w;
    *h = rt->h;
    if (p->offset && p->offset == rt->offset && p->pitch == rt->pitch && p->w && p->h) {
        if (p->w < *w) *w = p->w;
        if (p->h < *h) *h = p->h;
    }
}

static uint32_t s_shown_w, s_shown_h;   /* the last flip's present extent */

/* rt's pixels into the guest surface at `addr`, `pitch` bytes a row: 32-bit
 * as they are (BGRA8 is X8R8G8B8's byte order), 16-bit as R5G6B5. Runs after
 * the wait, on the executor's thread. */
static void write_back(RenderTarget *rt, uint32_t addr, uint32_t pitch)
{
    uint8_t *dst = (uint8_t *)xbox_GetMemoryOffset() + addr;
    uint32_t bpp = pitch / rt->w, w = rt->w, y, x;

    if (bpp >= 4) {
        [rt->tex getBytes:dst bytesPerRow:pitch fromRegion:MTLRegionMake2D(0, 0, w, rt->h)
              mipmapLevel:0];
    } else if (bpp == 2) {
        static uint32_t *row;
        static uint32_t row_cap;
        if (row_cap < w) {
            uint32_t *n = (uint32_t *)realloc(row, (size_t)w * 4);
            if (!n)
                return;
            row = n;
            row_cap = w;
        }
        for (y = 0; y < rt->h; y++) {
            uint16_t *o = (uint16_t *)(dst + (size_t)y * pitch);
            [rt->tex getBytes:row bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, y, w, 1)
                  mipmapLevel:0];
            for (x = 0; x < w; x++) {
                uint32_t c = row[x];
                o[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0)
                                  | ((c >> 3) & 0x001F));
            }
        }
    } else {
        return;
    }
    tex_cache_stale(addr, pitch * rt->h);
    s_writebacks++;
}

/* A scaled rt (render.scale > 1) read back once at the flip, hw x hh BGRA8
 * tightly packed: the window shows it as it is and the write-back boxes it
 * down to guest size, so one getBytes serves both. NULL on no memory. */
static const uint8_t *host_readback(RenderTarget *rt)
{
    static uint8_t *px;
    static size_t cap;
    size_t need = (size_t)rt->hw * rt->hh * 4u;
    if (cap < need) {
        uint8_t *n = (uint8_t *)realloc(px, need);
        if (!n)
            return NULL;
        px = n;
        cap = need;
    }
    [rt->tex getBytes:px bytesPerRow:rt->hw * 4u
           fromRegion:MTLRegionMake2D(0, 0, rt->hw, rt->hh) mipmapLevel:0];
    return px;
}

/* write_back for a scaled rt: the host pixels (host_readback) averaged over
 * each n x n block, so the guest -- RECOMP_FB_DUMP, fb_dump_at, the title --
 * sees a w x h frame at its pitch, 32-bit or R5G6B5 as write_back writes. */
static void write_back_scaled(RenderTarget *rt, const uint8_t *px, uint32_t addr,
                              uint32_t pitch)
{
    uint8_t *dst = (uint8_t *)xbox_GetMemoryOffset() + addr;
    uint32_t bpp = pitch / rt->w, w = rt->w, n = rt->hw / rt->w, y, x;

    if (bpp >= 4) {
        nv2a_downscale_box32(px, rt->hw * 4u, w, rt->h, n, dst, pitch);
    } else if (bpp == 2) {
        static uint32_t *row;
        static uint32_t row_cap;
        if (row_cap < w) {
            uint32_t *nr = (uint32_t *)realloc(row, (size_t)w * 4);
            if (!nr)
                return;
            row = nr;
            row_cap = w;
        }
        for (y = 0; y < rt->h; y++) {
            uint16_t *o = (uint16_t *)(dst + (size_t)y * pitch);
            nv2a_downscale_box32(px + (size_t)y * n * rt->hw * 4u, rt->hw * 4u, w, 1, n,
                                 (uint8_t *)row, w * 4u);
            for (x = 0; x < w; x++) {
                uint32_t c = row[x];
                o[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0)
                                  | ((c >> 3) & 0x001F));
            }
        }
    } else {
        return;
    }
    tex_cache_stale(addr, pitch * rt->h);
    s_writebacks++;
}

/* ---- write-back on demand ---------------------------------------------------
 *
 * Guest memory under a render target is written only when something is about
 * to read it: the executor's dumps (sync_guest), a texture decode over it
 * (tex_bind), the eviction of a target (surface). The title itself
 * was never seen reading its framebuffer (metal-zero-copy-present, design),
 * and D3D11 never writes back at all. metal_writeback=always, or
 * metal_no_rtt, writes the present surface at every flip as before. */
static int s_guard_always;   /* metal_fb_guard saw a title read */

static int wb_always(void)
{
    const char *e = recomp_env(RENV_METAL_WRITEBACK);
    return (e && !strcasecmp(e, "always")) || recomp_env(RENV_METAL_NO_RTT) != NULL
           || s_guard_always;
}

/* rt into guest memory now, at `pitch`, if drawn since the last write-back.
 * Waits for the GPU; the ring positions survive the wait, since a draw may
 * have placed its vertices already (tex_bind runs inside on_draw). */
/* Managed storage (a discrete GPU): queue the copy back of the GPU's
 * contents, which the next wait completes. Nothing on unified memory. */
static void rt_managed_sync(RenderTarget *rt)
{
    id<MTLBlitCommandEncoder> b;

    if (s_unified)
        return;
    end_encoder();
    ensure_cmd();
    b = [s_cmd blitCommandEncoder];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [b synchronizeResource:rt->tex];
#pragma clang diagnostic pop
    [b endEncoding];
}

static void rt_sync(RenderTarget *rt, uint32_t pitch)
{
    NSUInteger vb = s_vb_pos, ib = s_ib_pos;
    const uint8_t *px;

    if (!rt->tex || !rt->dirty || !pitch || pitch < rt->w * 2u)
        return;
    guard_open();
    @autoreleasepool {
        rt_managed_sync(rt);
        if (s_cmd)
            flush(1);
        else if (s_last_cmd && [s_last_cmd status] < MTLCommandBufferStatusCompleted)
            [s_last_cmd waitUntilCompleted];
        s_vb_pos = vb;
        s_ib_pos = ib;
        if (rt->hw == rt->w)
            write_back(rt, rt->addr, pitch);
        else if ((px = host_readback(rt)) != NULL)
            write_back_scaled(rt, px, rt->addr, pitch);
        rt->dirty = 0;
        rt_own_reset(rt);
    }
}

/* nv2a_pb_backend.sync_guest: the target present_target would pick for this
 * surface (offset, covering guest w x h), else the most recently used one at its
 * address (a surface dump of a target drawn at another clip). */
static void metal_sync_guest(uint32_t offset, uint32_t pitch, uint32_t w, uint32_t h)
{
    RenderTarget *rt = NULL;
    uint32_t addr = nv2a_pb_dma_resolve(offset) & 0x07FFFFFFu;
    int i;

    guard_open();
    if (s_failed)
        return;
    for (i = 0; i < RT_MAX && !rt; i++)
        if (s_rt[i].tex && s_rt[i].offset == offset && s_rt[i].w >= w && s_rt[i].h >= h)
            rt = &s_rt[i];
    if (!rt) {
        for (i = 0; i < RT_MAX; i++) {
            RenderTarget *c = &s_rt[i];
            if (c->tex && (c->addr & 0x07FFFFFFu) == addr
                    && (!rt || c->last_use > rt->last_use))
                rt = c;
        }
    }
    if (rt && !rt_drop_if_stale(rt, "sync"))
        rt_sync(rt, pitch);
}

/* For tests/nv2a_backend_smoke: write-backs so far, and the host pixels
 * (hw x hh BGRA8, tightly packed) of the target at a colour offset, read
 * back now, so a test can box-filter them itself. */
uint64_t nv2a_pb_metal_writebacks(void) { return s_writebacks; }

/* For tests/nv2a_backend_smoke: the size of the target at a colour offset
 * and the part of it the last flip showed, 0 when there is none (the shape
 * of nv2a_pb_d3d11_shown). */
int nv2a_pb_metal_shown(uint32_t offset, uint32_t *tw, uint32_t *th,
                        uint32_t *sw, uint32_t *sh)
{
    int i;
    for (i = 0; i < RT_MAX; i++)
        if (s_rt[i].tex && s_rt[i].offset == offset) {
            *tw = s_rt[i].w;
            *th = s_rt[i].h;
            *sw = s_shown_w;
            *sh = s_shown_h;
            return 1;
        }
    return 0;
}
const uint8_t *nv2a_pb_metal_host_pixels(uint32_t offset, uint32_t *hw, uint32_t *hh)
{
    int i;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->tex && rt->offset == offset) {
            rt_managed_sync(rt);
            if (s_cmd)
                flush(1);
            *hw = rt->hw;
            *hh = rt->hh;
            return host_readback(rt);
        }
    }
    return NULL;
}

/* ---- metal_fb_guard ----------------------------------------------------------
 *
 * After each lazy flip the present surface's pages are PROT_NONE; the first
 * access from any thread is recorded by the handler, which opens the pages
 * again and lets it proceed. A title READ then switches the run to
 * metal_writeback=always (that read saw the last write-back, or the seed); a
 * write needs nothing from the GPU (the allocator's bzero on re-allocation is
 * one) and is logged once. Debug only: PROT_NONE turns a kernel-side access to
 * those pages (read() into re-allocated memory) into EFAULT. */
static struct {
    uintptr_t plo, phi;          /* the page span made PROT_NONE */
    uint32_t addr;
    volatile int armed, hit;
    uintptr_t far, pc;
    uint32_t write;
    char th[24];
    struct sigaction prev_bus, prev_segv;
} s_guard;

static void guard_handler(int sig, siginfo_t *si, void *ucv)
{
    uintptr_t f = (uintptr_t)si->si_addr;
    struct sigaction *prev;

    if (s_guard.armed && f >= s_guard.plo && f < s_guard.phi) {
        uint32_t esr = 0;
        uintptr_t pc = 0;
#if defined(__APPLE__) && defined(__aarch64__)
        {
            ucontext_t *uc = (ucontext_t *)ucv;
            esr = uc->uc_mcontext->__es.__esr;
            pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
        }
#endif
        if (!s_guard.hit) {
            s_guard.far = f;
            s_guard.pc = pc;
            s_guard.write = (esr >> 6) & 1u;   /* ESR WnR */
            s_guard.th[0] = 0;
            pthread_getname_np(pthread_self(), s_guard.th, sizeof s_guard.th);
            s_guard.hit = 1;
        }
        mprotect((void *)s_guard.plo, s_guard.phi - s_guard.plo, PROT_READ | PROT_WRITE);
        s_guard.armed = 0;
        return;   /* re-executes the access */
    }
    prev = sig == SIGBUS ? &s_guard.prev_bus : &s_guard.prev_segv;
    if (prev->sa_flags & SA_SIGINFO) {
        if (prev->sa_sigaction) {
            prev->sa_sigaction(sig, si, ucv);
            return;
        }
    } else if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN) {
        prev->sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
}

/* Open the pages before this thread reads or writes them itself. */
static void guard_open(void)
{
    if (s_guard.armed) {
        s_guard.armed = 0;
        mprotect((void *)s_guard.plo, s_guard.phi - s_guard.plo, PROT_READ | PROT_WRITE);
    }
}

/* At the flip: report what the last span saw, then close the new one. */
static void guard_flip(uint32_t addr, uint32_t len)
{
    static int on = -1, said_write;
    uintptr_t pg = (uintptr_t)getpagesize(), lo;

    if (on < 0) {
        on = recomp_env(RENV_METAL_FB_GUARD) != NULL;
        if (on) {
            /* Installed now, after the title's own fault handlers, and
             * chained to them for every fault outside the span. */
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = guard_handler;
            sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGBUS, &sa, &s_guard.prev_bus);
            sigaction(SIGSEGV, &sa, &s_guard.prev_segv);
            fprintf(stderr, LOGP "fb_guard: armed; a title read of the present surface"
                    " switches to metal_writeback=always\n");
        }
    }
    if (!on)
        return;
    guard_open();
    if (s_guard.hit) {
        Dl_info di;
        const char *sym = dladdr((void *)s_guard.pc, &di) && di.dli_sname ? di.dli_sname : "?";
        unsigned long off = sym[0] != '?' ? (unsigned long)(s_guard.pc - (uintptr_t)di.dli_saddr)
                                          : (unsigned long)s_guard.pc;
        uint32_t ga = (uint32_t)(s_guard.far - (uintptr_t)xbox_GetMemoryOffset());
        if (!s_guard.write) {
            fprintf(stderr, LOGP "fb_guard: guest READ of present surface 0x%08X (at 0x%08X)"
                    " by '%s' pc %s+0x%lX; switching to metal_writeback=always\n",
                    s_guard.addr, ga, s_guard.th, sym, off);
            s_guard_always = 1;
        } else if (!said_write++) {
            fprintf(stderr, LOGP "fb_guard: guest WRITE of present surface 0x%08X (at 0x%08X)"
                    " by '%s' pc %s+0x%lX; ignored\n", s_guard.addr, ga, s_guard.th, sym, off);
        }
        s_guard.hit = 0;
    }
    if (s_guard_always || !addr || !len)
        return;
    lo = (uintptr_t)xbox_GetMemoryOffset() + addr;
    s_guard.addr = addr;
    s_guard.plo = lo & ~(pg - 1);
    s_guard.phi = (lo + len + pg - 1) & ~(pg - 1);
    __atomic_store_n(&s_guard.armed, 1, __ATOMIC_SEQ_CST);
    if (mprotect((void *)s_guard.plo, s_guard.phi - s_guard.plo, PROT_NONE) != 0)
        s_guard.armed = 0;
}

/* ---- Visibility tests: Metal visibility-result counting ---------------------
 *
 * The shared tracker (nv2a_occ, nv2a_backend_common.h) as the D3D11 backend
 * drives it: BeginVisibilityTest is CLEAR_REPORT_VALUE +
 * SET_ZPASS_PIXEL_COUNT_ENABLE(1), EndVisibilityTest is ENABLE(0) +
 * GET_REPORT; each stretch with counting on is one query, and a report sums
 * the stretches since the last clear, completing later (poll at every batch
 * and flip, with a flush on the ack thread's idle tick) or as visible after
 * 250 ms. The count is samples passing depth and stencil, alpha-test discard
 * applied.
 *
 * A query here is a list of slots in s_vis, one per render pass it spans:
 * Metal resets, at the start of a pass, every offset that pass names (even
 * in setVisibilityResultMode:Disabled), so a slot never outlives its pass
 * and a stretch over a pass change takes a new one. Slots accumulate within
 * the pass. A result is in once every slot's command buffer has completed,
 * so GET_REPORT commits the open command buffer: a count held to the flip
 * lands a frame later than D3D11's, and light shafts whose brightness
 * comes from the count come out too bright. A slot freed while
 * its command buffer is still in flight retires until the flip's wait.
 *
 * At the end of each walk, before a semaphore release that follows a
 * report (both on_poll) and at the flip after its wait, nv2a_occ_settle
 * waits for the results while the frame's RECOMP_DEBUG=occ_settle_us budget
 * lasts (8000 by default; a query still in the open s_cmd is committed by
 * occ_result's flush), then settles whatever is still pending with the
 * report address's last count (visible the first time), so the title never
 * reads a test as incomplete (nv2a_backend_common.h). At stock
 * almost nothing is left to settle: the reports' command buffers were
 * committed at GET_REPORT. occ_delay_us holds every result back (a test
 * hook for a slow GPU).
 *
 * RECOMP_METAL_OCC=sync waits at GET_REPORT; =fixed leaves reports to the
 * walker (RECOMP_ZPASS_FIXED, every test visible). */
#define OCC_QSLOTS 16
/* Pending and retired reports, a query each at least, and the open ones. */
#define OCC_POOL   (NV2A_OCC_PENDING * 3 + NV2A_OCC_SEG)
typedef struct OccQuery {
    int                   slot[OCC_QSLOTS];
    id<MTLCommandBuffer>  cmd[OCC_QSLOTS];   /* retained */
    int                   n, ovf;
    struct OccQuery      *next;              /* free list */
} OccQuery;
static OccQuery          s_qpool[OCC_POOL];
static OccQuery         *s_qfree;
static int               s_qmade;
static unsigned char     s_vis_used[VIS_SLOTS];
static int               s_vis_next;
static int               s_retire[VIS_SLOTS], s_nretire;
static struct nv2a_occ   s_occ;
static int               s_occ_mode = -1;  /* 0 defer, 1 sync, 2 fixed */
static uint64_t          s_occ_ovf;

static int occ_mode(void)
{
    if (s_occ_mode < 0) {
        const char *e = recomp_env(RENV_METAL_OCC);
        s_occ_mode = !e ? 0 : !strcasecmp(e, "sync") ? 1 : !strcasecmp(e, "fixed") ? 2 : 0;
    }
    return s_occ_mode;
}

static int vis_alloc(void)
{
    int i, k;
    for (k = 0; k < VIS_SLOTS; k++) {
        i = (s_vis_next + k) % VIS_SLOTS;
        if (!s_vis_used[i]) {
            s_vis_used[i] = 1;
            s_vis_next = (i + 1) % VIS_SLOTS;
            ((uint64_t *)[s_vis contents])[i] = 0;
            return i;
        }
    }
    return -1;
}

static void occ_pass_begin(void)
{
    OccQuery *q = s_occ_q;
    int i;
    if (!q || !s_enc || s_occ_slot >= 0)
        return;
    if (q->n >= OCC_QSLOTS || (i = vis_alloc()) < 0) {
        q->ovf = 1;
        s_occ_ovf++;
        return;
    }
    q->slot[q->n] = i;
    q->cmd[q->n] = [s_cmd retain];
    q->n++;
    [s_enc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:(NSUInteger)i * 8u];
    s_occ_slot = i;
}

static void occ_pass_end(void)
{
    if (s_occ_slot >= 0 && s_enc)
        [s_enc setVisibilityResultMode:MTLVisibilityResultModeDisabled
                                offset:(NSUInteger)s_occ_slot * 8u];
    s_occ_slot = -1;
}

static uint64_t occ_now_us(void *ctx)
{
    struct timespec ts;
    (void)ctx;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void *occ_get(void *ctx)
{
    OccQuery *q;
    (void)ctx;
    if (s_qfree) {
        q = s_qfree;
        s_qfree = q->next;
    } else if (s_qmade < OCC_POOL) {
        q = &s_qpool[s_qmade++];
    } else {
        return NULL;
    }
    q->n = 0;
    q->ovf = 0;
    return q;
}

static int cmd_done(id<MTLCommandBuffer> c)
{
    MTLCommandBufferStatus st = [c status];
    return st == MTLCommandBufferStatusCompleted || st == MTLCommandBufferStatusError;
}

static void occ_put(void *ctx, void *qv)
{
    OccQuery *q = (OccQuery *)qv;
    int i;
    (void)ctx;
    if (!q)
        return;
    if (q == s_occ_q) {     /* not expected: the tracker ends a query first */
        occ_pass_end();
        s_occ_q = NULL;
    }
    for (i = 0; i < q->n; i++) {
        if (cmd_done(q->cmd[i]))
            s_vis_used[q->slot[i]] = 0;
        else
            s_retire[s_nretire++] = q->slot[i];
        [q->cmd[i] release];
    }
    q->n = 0;
    q->next = s_qfree;
    s_qfree = q;
}

static void occ_begin(void *ctx, void *qv)
{
    (void)ctx;
    occ_pass_end();
    s_occ_q = (OccQuery *)qv;
    occ_pass_begin();
}

static void occ_end(void *ctx, void *qv)
{
    (void)ctx;
    if (s_occ_q == (OccQuery *)qv) {
        occ_pass_end();
        s_occ_q = NULL;
    }
}

static int occ_result(void *ctx, void *qv, int flush_gpu, uint64_t *count)
{
    OccQuery *q = (OccQuery *)qv;
    const uint64_t *v = (const uint64_t *)[s_vis contents];
    uint64_t sum = 0;
    int i;
    (void)ctx;
    for (i = 0; i < q->n; i++) {
        if (q->cmd[i] == s_cmd) {       /* not committed yet */
            if (!flush_gpu)
                return 0;
            flush(0);
        }
        if (!cmd_done(q->cmd[i]))
            return 0;
    }
    for (i = 0; i < q->n; i++)
        sum += v[q->slot[i]];
    *count = q->ovf ? NV2A_OCC_VISIBLE : sum;
    return 1;
}

static const struct nv2a_occ_ops s_occ_ops = {
    occ_get, occ_put, occ_begin, occ_end, occ_result, occ_now_us, NULL
};

static uint64_t occ_env_us(recomp_env_id id, uint64_t def)
{
    long v = recomp_env_int(id, (long)def);
    return v > 0 ? (uint64_t)v : 0;
}

static int occ_ready_on(void)
{
    if (occ_mode() == 2 || !init())
        return 0;
    if (!s_occ.ops) {
        nv2a_occ_init(&s_occ, &s_occ_ops, (uint8_t *)xbox_GetMemoryOffset());
        nv2a_occ_set_delay(&s_occ, occ_env_us(RENV_OCC_DELAY_US, 0));
        nv2a_occ_set_settle(&s_occ, occ_env_us(RENV_OCC_SETTLE_US,
                                               NV2A_OCC_SETTLE_US));
        nv2a_occ_set_trace(&s_occ, recomp_env(RENV_ZPASS_TRACE) != NULL);
    }
    return 1;
}

static void metal_on_zpass(int enable)
{
    if (occ_ready_on()) {
        @autoreleasepool {
            nv2a_occ_zpass(&s_occ, enable);
        }
    }
}

static void metal_on_zpass_clear(void)
{
    if (occ_ready_on()) {
        @autoreleasepool {
            nv2a_occ_zpass_clear(&s_occ);
        }
    }
}

static uint64_t s_report_commits;    /* flushes at GET_REPORT */

/* Does the report just pushed at va count in the open command buffer? */
static int report_in_cmd(uint32_t va)
{
    const struct nv2a_occ_report *p;
    int k, i;
    if (!s_occ.npend || s_occ.pend[s_occ.npend - 1].va != va)
        return 0;
    p = &s_occ.pend[s_occ.npend - 1];
    for (k = 0; k < p->n; k++) {
        const OccQuery *q = (const OccQuery *)p->q[k];
        for (i = 0; i < q->n; i++)
            if (q->cmd[i] == s_cmd)
                return 1;
    }
    return 0;
}

static int metal_on_report(uint32_t va)
{
    if (!occ_ready_on())
        return 0;
    @autoreleasepool {
        nv2a_occ_report(&s_occ, va, occ_mode() == 1);
        /* Commit now, so the count can land within the frame, as a D3D11
         * query's does; held to the flip, it lands a frame late. Only when
         * the report counts in s_cmd: each commit ends the pass (a tile
         * store and load), and a report whose queries drew nothing, or
         * drew in a buffer already committed, gains nothing from it. */
        if (occ_mode() == 0 && s_cmd && report_in_cmd(va)) {
            s_report_commits++;
            flush(0);
        }
    }
    return 1;
}

static void occ_settle(int flip);

/* At the end of a walk, before a semaphore release after a report, and
 * every ack pass (retired queries only, unflushed, when nothing is
 * pending). */
static void metal_on_poll(void)
{
    if (s_occ.npend || s_occ.nret) {
        @autoreleasepool {
            occ_settle(0);
        }
    }
}

/* After a wait every command buffer is done: retired slots free. */
static void occ_after_wait(void)
{
    while (s_nretire)
        s_vis_used[s_retire[--s_nretire]] = 0;
    if (s_occ.ops)
        nv2a_occ_poll(&s_occ, 0);
}

/* At the end of a walk, before a semaphore release after a report, and at
 * the flip after its wait. Not in occ_after_wait, which also runs at the
 * waits inside a walk (a full ring, a readback), where the title cannot
 * read yet. */
static void occ_settle(int flip)
{
    if (!s_occ.ops)
        return;
    if (flip)
        nv2a_occ_flip(&s_occ);
    else
        nv2a_occ_settle(&s_occ);
}

static void flip_log(uint32_t surface_offset, RenderTarget *rt)
{
    static int on = -1;
    static uint64_t p_draws, p_prog, p_skip, p_cull, p_clears, p_tex, p_up,
                    p_chg, p_re, p_fl, p_occ, p_vis, p_late, p_super, p_sync, p_ovf,
                    p_set, p_stale, p_first;
    static int zt = -1;
    /* With a pack, its hit count every 600 flips, flip trace or not. */
    if (nv2a_tex_pack_count() && s_presents % 600 == 0) {
        fprintf(stderr, LOGP "textures.pack: %llu hits so far\n",
                (unsigned long long)nv2a_tex_pack_hits());
        if (recomp_env(RENV_TEX_PACK_TRACE))
            nv2a_tex_pack_print_misses();
    }
    if (on < 0) {
        on = recomp_env(RENV_FLIP_LOG) != NULL;
        zt = recomp_env(RENV_ZPASS_TRACE) != NULL;
    }
    /* The visibility totals so far: a run killed by its timeout has no
     * exit to print them at, so the last of these is the run's. */
    if ((on || zt) && s_occ.reports && s_presents % 300 == 0)
        nv2a_occ_summary(&s_occ, "[METAL]");
    if (!on)
        return;
    fprintf(stderr, "[METAL] flip %llu batches %llu prog %llu skipped %llu"
            " culled %llu clears %llu tex %llu uploads %llu changed %llu rehash %llu"
            " flushes %llu occ %llu (visible %llu, pending %d, late %llu, superseded %llu,"
            " settled %llu (stale %llu, first %llu), sync %llu us, slot overflow %llu)"
            " present 0x%08X walker 0x%08X\n",
            (unsigned long long)s_presents,
            (unsigned long long)(s_draws - p_draws),
            (unsigned long long)(s_draws_prog - p_prog),
            (unsigned long long)(s_draws_skipped - p_skip),
            (unsigned long long)(s_draws_culled - p_cull),
            (unsigned long long)(s_clears - p_clears),
            (unsigned long long)(s_draws_tex - p_tex),
            (unsigned long long)(s_tex_uploads - p_up),
            (unsigned long long)(s_tex_changed - p_chg),
            (unsigned long long)(s_tex_rehash - p_re),
            (unsigned long long)(s_flushes - p_fl),
            (unsigned long long)(s_occ.reports - p_occ),
            (unsigned long long)(s_occ.vis - p_vis), s_occ.npend,
            (unsigned long long)(s_occ.late - p_late),
            (unsigned long long)(s_occ.super - p_super),
            (unsigned long long)(s_occ.settled - p_set),
            (unsigned long long)(s_occ.stale - p_stale),
            (unsigned long long)(s_occ.first - p_first),
            (unsigned long long)(s_occ.sync_us - p_sync),
            (unsigned long long)(s_occ_ovf - p_ovf),
            rt ? rt->offset : 0, surface_offset);
    p_occ = s_occ.reports; p_vis = s_occ.vis; p_late = s_occ.late;
    p_super = s_occ.super; p_sync = s_occ.sync_us; p_ovf = s_occ_ovf;
    p_set = s_occ.settled; p_stale = s_occ.stale; p_first = s_occ.first;
    p_draws = s_draws; p_prog = s_draws_prog; p_skip = s_draws_skipped;
    p_cull = s_draws_culled; p_clears = s_clears; p_tex = s_draws_tex;
    p_up = s_tex_uploads; p_chg = s_tex_changed; p_re = s_tex_rehash;
    p_fl = s_flushes;
}

static void prof_flip_end(uint64_t t_flip0)
{
    static uint64_t p_wb, p_ds;
    int i;
    if (!prof_on())
        return;
    for (i = 0; i < s_prof_ncmd; i++) {
        id<MTLCommandBuffer> c = s_prof_cmd[i];
        if ([c status] == MTLCommandBufferStatusCompleted) {
            CFTimeInterval a = [c GPUStartTime], b = [c GPUEndTime];
            if (b > a)
                s_pa.gpu += (uint64_t)((b - a) * 1e9);
        }
        [c release];
    }
    s_pa.ncmd += (uint64_t)s_prof_ncmd;
    s_prof_ncmd = 0;
    s_pa.flip += prof_now() - t_flip0;
    if (++s_pa.n == 300) {
        double n = (double)s_pa.n;
        fprintf(stderr, LOGP "prof %llu flips: on_flip %.3f ms = wait %.3f + hostrb %.3f"
                " + wb %.3f + winrb %.3f + winpush %.3f + blit %.3f (+rest); gpu %.3f ms"
                " over %.1f cmd buffers/flip; %llu write-backs, %llu decode syncs;"
                " rt ownership hashes %.3f ms/flip\n",
                (unsigned long long)s_presents,
                s_pa.flip / n / 1e6, s_pa.wait / n / 1e6, s_pa.hostrb / n / 1e6,
                s_pa.wb / n / 1e6, s_pa.winrb / n / 1e6, s_pa.winpush / n / 1e6,
                s_pa.blit / n / 1e6, s_pa.gpu / n / 1e6, s_pa.ncmd / n,
                (unsigned long long)(s_writebacks - p_wb),
                (unsigned long long)(s_decode_syncs - p_ds), s_pa.hash / n / 1e6);
        p_wb = s_writebacks;
        p_ds = s_decode_syncs;
        memset(&s_pa, 0, sizeof s_pa);
    }
}

static void metal_flip(uint32_t surface_offset, uint32_t pitch)
{
    RenderTarget *rt = NULL;
    uint32_t addr = surface_offset ? nv2a_pb_dma_resolve(surface_offset) : 0;
    const uint8_t *hpx = NULL;   /* a scaled rt's host pixels (host_readback) */
    uint64_t t_flip0 = prof_on() ? prof_now() : 0, t;
    int always = wb_always(), win = 0, layer = 0;
    uint32_t cw = 0, ch = 0, hcw = 0, hch = 0;   /* the frame, guest and host */

    guard_open();
    if (init()) {
        @autoreleasepool {
            int wb;
            id<MTLTexture> slot = nil;
            rt = present_target(surface_offset);
            if (rt) {
                present_extent(rt, &cw, &ch);
                hcw = (uint32_t)((uint64_t)cw * rt->hw / rt->w);
                hch = (uint32_t)((uint64_t)ch * rt->hh / rt->h);
                s_shown_w = cw;
                s_shown_h = ch;
            }
            win = rt && xbox_FramebufferWindowRunning();
            /* Layer mode: the frame goes to the window as a GPU copy into the
             * presenter's back slot (fb_present_metal.m), below. */
            layer = win && s_layer;
            wb = rt && addr && pitch && pitch >= rt->w * 2u && always;
            if (rt && (wb || (win && !layer)))
                rt_managed_sync(rt);
            t = prof_t();
            flush(1);
            occ_settle(1);
            prof_add(&s_pa.wait, t);
            if (layer) {
                /* After the wait, in a command buffer of its own that nobody
                 * waits for: the flip's wait never includes the copy, nor a
                 * present pass still reading the slot (tracked hazards order
                 * the two on the GPU). Published once committed, so the main
                 * thread's pass that shows it is committed after it. */
                t = prof_t();
                slot = (id<MTLTexture>)xbox_FramebufferWindowBackTexture(hcw, hch);
                if (slot) {
                    id<MTLBlitCommandEncoder> b;
                    ensure_cmd();
                    b = [s_cmd blitCommandEncoder];
                    [b copyFromTexture:rt->tex sourceSlice:0 sourceLevel:0
                          sourceOrigin:MTLOriginMake(0, 0, 0)
                            sourceSize:MTLSizeMake(hcw, hch, 1)
                             toTexture:slot destinationSlice:0 destinationLevel:0
                     destinationOrigin:MTLOriginMake(0, 0, 0)];
                    [b endEncoding];
                    flush(0);
                    xbox_FramebufferWindowPublishTexture();
                    s_window_presents++;
                }
                prof_add(&s_pa.blit, t);
            }
            /* The host-size readback only when something reads it: the
             * write-back below or the readback-mode window. */
            t = prof_t();
            if (rt && rt->hw != rt->w && (wb || (win && !layer)))
                hpx = host_readback(rt);
            prof_add(&s_pa.hostrb, t);
            if (wb) {
                t = prof_t();
                if (rt->hw == rt->w)
                    write_back(rt, addr, pitch);
                else if (hpx)
                    write_back_scaled(rt, hpx, addr, pitch);
                rt->dirty = 0;
                prof_add(&s_pa.wb, t);
                if (addr == rt->addr && pitch == rt->pitch)
                    rt_own_reset(rt);
            }
        }
    }
    s_presents++;
    flip_log(surface_offset, rt);
    if (s_presents == 1 || (s_presents % 300) == 0)
        fprintf(stderr, LOGP "present #%llu: surface 0x%08X (%s), %llu clears,"
                " %llu draws (%llu vertex-program), %llu skipped, %llu culled,"
                " %llu texture uploads, %llu rehashes, %llu write-backs (%s),"
                " %llu decode syncs, %llu stale drops, %llu evictions lost, %llu grows,"
                " %llu partial clears;"
                " shaders %llu vs + %llu fs, %llu compile errors;"
                " %llu z-clears, %llu depth draws, %llu rt binds (%llu self copies);"
                " %llu window presents (%s); %llu flushes,"
                " %llu at GET_REPORT\n",
                (unsigned long long)s_presents, surface_offset,
                rt ? "drawn here" : "guest bytes",
                (unsigned long long)s_clears, (unsigned long long)s_draws,
                (unsigned long long)s_draws_prog, (unsigned long long)s_draws_skipped,
                (unsigned long long)s_draws_culled,
                (unsigned long long)s_tex_uploads, (unsigned long long)s_tex_rehash,
                (unsigned long long)s_writebacks, always ? "always" : "lazy",
                (unsigned long long)s_decode_syncs, (unsigned long long)s_stale_drops,
                (unsigned long long)s_evict_lost, (unsigned long long)s_grows,
                (unsigned long long)s_partial_clears,
                (unsigned long long)s_vs_compiled, (unsigned long long)s_fs_compiled,
                (unsigned long long)s_compile_errors,
                (unsigned long long)s_zclears, (unsigned long long)s_draws_z,
                (unsigned long long)s_rtt_binds, (unsigned long long)s_self_copies,
                (unsigned long long)s_window_presents,
                s_layer ? "CAMetalLayer slots" : "texture readback",
                (unsigned long long)s_flushes, (unsigned long long)s_report_commits);
    if (!always && rt && addr && pitch)
        guard_flip(addr, pitch * rt->h);
    if (!pitch || !addr || layer) {
        prof_flip_end(t_flip0);
        return;
    }
    xbox_FramebufferWindowSet(addr, pitch);
    /* Readback mode (metal_present=readback, or no CAMetalLayer window): a
     * surface drawn here reaches the window by readback, getBytes of rt->tex
     * after the flip's wait, a copy into the window's slot, then
     * SDL_UpdateTexture. It reads the texture, not the guest bytes. A surface
     * the backend never drew (guest bytes) goes as the CPU path presents it,
     * in either mode. */
    if (rt && hpx && win) {
        /* Scaled: the frame's part of the host-size readback above. */
        t = prof_t();
        xbox_FramebufferWindowPresentPixels(hpx, hcw, hch, rt->hw * 4u, 4u);
        prof_add(&s_pa.winpush, t);
        s_window_presents++;
        prof_flip_end(t_flip0);
        return;
    }
    if (rt && rt->hw == rt->w && win) {
        static uint8_t *px;
        static size_t cap;
        size_t need = (size_t)cw * ch * 4u;
        if (cap < need) {
            uint8_t *n = (uint8_t *)realloc(px, need);
            if (n) {
                px = n;
                cap = need;
            }
        }
        if (cap >= need) {
            t = prof_t();
            [rt->tex getBytes:px bytesPerRow:cw * 4u
                   fromRegion:MTLRegionMake2D(0, 0, cw, ch) mipmapLevel:0];
            prof_add(&s_pa.winrb, t);
            t = prof_t();
            xbox_FramebufferWindowPresentPixels(px, cw, ch, cw * 4u, 4u);
            prof_add(&s_pa.winpush, t);
            s_window_presents++;
            prof_flip_end(t_flip0);
            return;
        }
    }
    /* Guest bytes go to the window: our read, not the title's (the guard
     * may have armed this surface above). Headless, nothing reads them. */
    if (xbox_FramebufferWindowRunning())
        guard_open();
    xbox_FramebufferWindowPresent(addr, pitch);
    prof_flip_end(t_flip0);
}

/* render.scale changed at this flip: every target, depth buffer and texture
 * was made at the old factor. Targets go with the eviction's write-back, so
 * the next one at that surface seeds from what was drawn at the new scale;
 * textures because a render-to-texture input is sized by scale. After the
 * present, so the frame being shown keeps the targets it was drawn in. */
static void scale_drop(void)
{
    int i;
    @autoreleasepool {
        for (i = 0; i < RT_MAX; i++) {
            RenderTarget *rt = &s_rt[i];
            if (!rt->tex)
                continue;
            if (!rt_drop_if_stale(rt, "scale")) {
                rt_sync(rt, rt->pitch);
                rt_free(rt);
            }
        }
        for (i = 0; i < DT_MAX; i++) {
            DepthTarget *d = &s_dt[i];
            if (!d->tex)
                continue;
            if (s_enc_dt == d)
                end_encoder();
            [d->tex release];
            memset(d, 0, sizeof *d);
        }
        for (i = 0; i < TEX_CACHE; i++) {
            [s_tc[i].tex release];
            memset(&s_tc[i], 0, sizeof s_tc[i]);
        }
    }
    /* Queries issued at the old scale are read at it: a count taken at 2x
     * divided by 1 would report four times the samples. Retired results go
     * unread (nv2a_occ_set_scale). */
    if (s_occ.ops) {
        @autoreleasepool {
            occ_settle(0);
        }
        nv2a_occ_set_scale(&s_occ, nv2a_host_render_scale());
    }
    fprintf(stderr, LOGP "render.scale=%u from this flip: targets and textures dropped\n",
            nv2a_host_render_scale());
}

/* Live options (nv2a_host_opts_request) are applied after the present:
 * the one point where no target is bound and no upload is in flight, and
 * the frame shown was drawn at the old render.scale, which the present
 * sizes its source by (applied first, a 2x frame lowered to 1x would show
 * its top-left quarter for one flip). */
static void metal_on_flip(uint32_t surface_offset, uint32_t pitch)
{
    unsigned changed;
    metal_flip(surface_offset, pitch);
    changed = nv2a_host_opts_apply();
    if (changed & NV2A_OPT_SCALE)
        scale_drop();
}

/* ---- registration ---------------------------------------------------------- */

static const struct nv2a_pb_backend s_metal_backend = {
    metal_on_clear,
    metal_on_zclear,
    metal_on_draw,
    metal_on_flip,
    metal_on_zpass,
    metal_on_zpass_clear,
    metal_on_report,
    metal_on_poll,
    1,                  /* writes_back: dumps read guest memory, after sync_guest */
    metal_sync_guest,
};

const struct nv2a_pb_backend *nv2a_pb_metal_backend(void)
{
    if (!s_black && init()) {
        @autoreleasepool {
            s_black = solid_texture(0xFF000000u);
            s_magenta = solid_texture(NV2A_TEX_MAGENTA);
        }
    }
    return &s_metal_backend;
}

/* RECOMP_PB_BACKEND=metal routes the executor here. Called once from the
 * executor's first method, on the ack thread. */
void nv2a_pb_metal_register_from_env(void)
{
    const char *b = recomp_env(RENV_PB_BACKEND);
    if (b && !strcasecmp(b, "metal")) {
        const struct nv2a_pb_backend *mb = nv2a_pb_metal_backend();
        if (s_failed) {
            fprintf(stderr, LOGP "RECOMP_PB_BACKEND=metal: no device, staying on"
                    " the CPU rasteriser\n");
            return;
        }
        nv2a_pb_set_backend(mb);
        fprintf(stderr, LOGP "RECOMP_PB_BACKEND=metal: executor draws through Metal\n");
    }
}

#endif /* __APPLE__ */
