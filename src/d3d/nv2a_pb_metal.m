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

#include <math.h>
#include <stddef.h>
#include "recomp_env.h"
#include "mem_stats.h"
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
#include "nv2a_2d.h"
#include "nv2a_backend_common.h"
#include "nv2a_stats.h"
#include "nv2a_tex_pack.h"
#include "nv2a_tex_dump.h"
#include "d3d8_msl.h"
#include "nv2a_basic_msl.h"
#include "metal_compile.h"
#ifdef XBOXRECOMP_ENHANCE
#include "nv2a_dbg_draw.h"
#endif
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
extern void *xbox_FramebufferWindowBackTexture(uint32_t w, uint32_t h, unsigned n,
                                               uint32_t pad);
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

/* The basic library (fs_basic, vs_pass, the clears) and render.snap_2d's
 * fs_basic_snap, built when it first draws (fs_basic): nv2a_basic_msl.h. */
static const char s_msl[] = NV2A_MSL_BASIC;
static const char s_msl_snap[] = NV2A_MSL_BASIC_SNAP;

/* ---- device ---------------------------------------------------------------- */

static id<MTLDevice>               s_dev;
static id<MTLCommandQueue>         s_queue;
static id<MTLFunction>             s_vs_pass, s_fs_basic;
static id<MTLFunction>             s_vs_clear, s_fs_clear;
static id<MTLRenderPipelineState>  s_clear_pso[2];   /* partial clears: without, with depth */
static id<MTLCommandBuffer>        s_cmd;      /* the frame's, begun lazily */
static id<MTLRenderCommandEncoder> s_enc;      /* open pass, or nil */
static int                         s_failed;
static int                         s_unified;  /* shared textures readable */
static int                         s_bc;       /* BC1 and BC3 textures sample (pack entries) */

/* Vertex and index rings, reset after every wait (flip or overflow). */
static id<MTLBuffer> s_vb, s_ib;
static NSUInteger    s_vb_size, s_vb_pos, s_ib_size, s_ib_pos;

#define VIS_SLOTS 8192

static uint64_t s_presents, s_clears, s_draws, s_draws_skipped, s_draws_prog,
                s_draws_culled, s_draws_tex, s_tex_uploads, s_tex_rehash,
                s_tex_changed, s_flushes, s_writebacks, s_cmd_errors,
                s_draws_stretch;   /* hor+: batches placed off-centre (nv2a_aspect_place) */

/* ---- render targets: one per guest surface --------------------------------- */

/* Depth/stencil buffers, one per zeta surface and size, as the D3D11
 * backend's: Depth32Float_Stencil8, so the test compares the same normalised
 * float z (depth units / zmax) the CPU path keeps, and the 8 stencil bits
 * are Z24S8's. Never read back to guest memory. */
#define DT_MAX 4
typedef struct {
    uint32_t       zeta, w, h;
    uint32_t       hw, hh;     /* host size: (w + 2 pad) x h at render.scale */
    uint32_t       pad;        /* hor+ padding, guest pixels a side (nv2a_host_pad) */
    unsigned       n;          /* the render.scale factor used */
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
    uint32_t       hw, hh;     /* host size: the texture, (w + 2 pad) * n x h * n */
    uint32_t       pad;        /* hor+ padding, guest pixels a side (nv2a_host_pad) */
    unsigned       n;          /* the render.scale factor used */
    id<MTLTexture> tex;
    uint64_t       last_use;
    DepthTarget   *dt;         /* the depth buffer its passes attach, or NULL */
    uint32_t       pitch;      /* the guest pitch it was made with */
    uint32_t       fmt;        /* the SET_SURFACE_FORMAT word it was drawn with */
    int            dirty;      /* drawn since its last write-back */
    int            presented;  /* ever picked by present_target */
    struct nv2a_rt_own own;    /* is the guest memory under it still its own */
    uint32_t       bar_flip[2]; /* hor+ band bars per third (nv2a_aspect_bar_mark) */
    int            copy;       /* made by a 2D blit (metal_on_blit), not drawn since */
    uint32_t       copy_flip;  /* the flip of the blit that last filled it */
} RenderTarget;

static RenderTarget  s_rt[RT_MAX];
static RenderTarget *s_enc_rt;   /* the target s_enc draws into */
static DepthTarget  *s_enc_dt;   /* and its depth attachment, or NULL */
static DepthTarget   s_dt[DT_MAX];
static uint64_t      s_tick;
static uint64_t      s_zclears, s_draws_z, s_rtt_binds, s_self_copies;
static uint64_t      s_stale_drops, s_retired, s_evict_lost;
static uint64_t      s_blits_gpu, s_blits_guest;   /* metal_on_blit */
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
static uint64_t s_pitch_alias_binds;   /* binds over a dirty target nv2a_rt_pitch_alias
                                          * decoded without a write-back */
static struct nv2a_pair_set s_pitch_alias_seen;   /* its RENV_METAL_TRACE pairs */

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

/* metal_shaders: what the first use of a shader, pipeline or texture costs
 * the ack thread, per flip. The clocks are read always: a build is
 * rare and costs milliseconds, so two clock reads around it are noise. */
static int s_sh_trace = -1;
static struct { uint64_t lib_ns, pso_ns, tex_ns; unsigned lib, pso, tex; } s_sh_flip;

static int sh_trace(void)
{
    if (s_sh_trace < 0)
        s_sh_trace = recomp_env(RENV_METAL_SHADERS) != NULL;
    return s_sh_trace;
}

/* Each render encoder made, so a binding remembered against one is never
 * taken for another's (an address can come back after a release). */
static uint64_t s_enc_serial;
/* What vertex buffer 1 of the encoder s_vc1.enc holds: c[] at generation
 * s_vc1.gen while ok. */
static struct { uint64_t enc; uint32_t gen; int ok; } s_vc1;
static uint64_t s_c_uploads;   /* c[] writes to buffer 1; "consts" in the flip trace */

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
    s_enc_serial++;
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

        /* The shared options (metal_compile.h): fast math off, safe math,
         * the MSL 3.0 pin. */
        lib = metal_compile_library(s_dev, [NSString stringWithUTF8String:s_msl],
                                    "the base library", &err);
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
    e->pad = nv2a_host_pad(w, h);
    e->n = nv2a_host_size(w + 2u * e->pad, h, nv2a_host_render_scale(), METAL_MAX_DIM,
                          &e->hw, &e->hh);
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
                    uint32_t h, uint32_t pad, uint32_t hw, uint32_t hh)
{
    const uint8_t *src = (const uint8_t *)xbox_GetMemoryOffset() + addr;

    if (pitch < w * 4u)
        return 0;
    guard_open();   /* our own read of the seed is not a title read */
    if (hw == w) {
        [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0
                 withBytes:src bytesPerRow:pitch];
    } else {
        /* Scaled or padded: the guest bytes, each pixel an n x n block,
         * centred; the hor+ pads start black (calloc). */
        unsigned n = hw / (w + 2u * pad);
        uint8_t *up = (uint8_t *)calloc((size_t)hw * hh, 4u);
        if (up) {
            nv2a_upscale_nearest32(src, pitch, w, h, n, up + (size_t)pad * n * 4u, hw * 4u);
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
    uint32_t hw, hh, pad;
    unsigned n;
    int keep;

    pad = nv2a_host_pad(w, h);
    n = nv2a_host_size(w + 2u * pad, h, nv2a_host_render_scale(), METAL_MAX_DIM, &hw, &hh);
    keep = rt->pad == pad && rt->hw == (rt->w + 2u * pad) * n && rt->hh == rt->h * n;
    if (!keep)
        rt_sync(rt, rt->pitch);
    /* An encoder must not outlive its attachment's swap, and the passes
     * below need none open. */
    end_encoder();
    tex = rt_texture_new(hw, hh);
    if (!tex)
        return 0;
    if (!rt_seed(tex, rt->addr, rt->pitch, w, h, pad, hw, hh)) {
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
    rt->pad = pad;
    rt->n = n;
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
    uint32_t hw, hh, pad;
    unsigned n;
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
            rt->fmt = g->format;
            rt->copy = 0;
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
            rt->fmt = g->format;
            rt->copy = 0;
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
    pad = nv2a_host_pad(w, h);
    n = nv2a_host_size(w + 2u * pad, h, nv2a_host_render_scale(), METAL_MAX_DIM, &hw, &hh);
    free_rt->tex = rt_texture_new(hw, hh);
    if (!free_rt->tex)
        return NULL;
    free_rt->offset = g->color_offset;
    free_rt->addr = nv2a_pb_dma_resolve(g->color_offset);
    free_rt->w = w;
    free_rt->h = h;
    free_rt->hw = hw;
    free_rt->hh = hh;
    free_rt->pad = pad;
    free_rt->n = n;
    free_rt->pitch = g->pitch;
    free_rt->fmt = g->format;
    free_rt->last_use = s_tick;
    rt_own_reset(free_rt);   /* the bytes the seed reads, or would */
    rt_seed(free_rt->tex, free_rt->addr, g->pitch, w, h, pad, hw, hh);
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
    uint32_t argb;
    uint32_t b[4];
    id<MTLRenderPipelineState> pso;

    if (!(param & 0xF0) || !init())
        return;
    @autoreleasepool {
        rt = surface();
        if (!rt || !nv2a_clear_box(g, rt->w, rt->h, b))
            return;
        argb = nv2a_clear_argb(g);   /* in the surface's format */
        c[0] = (double)((argb >> 16) & 0xFF) / 255.0;
        c[1] = (double)((argb >> 8) & 0xFF) / 255.0;
        c[2] = (double)(argb & 0xFF) / 255.0;
        c[3] = (double)(argb >> 24) / 255.0;
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
            uint32_t hb[4];
            /* As a 3D draw's clip: an edge at 0 or w reaches the pads. */
            nv2a_aspect_box(b, rt->w, rt->h, rt->pad, rt->n, NV2A_PLACE_CENTRE, 0, 0, hb);
            sr.x = hb[0];
            sr.y = hb[1];
            sr.width = hb[2] - hb[0];
            sr.height = hb[3] - hb[1];
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
    {
        uint64_t t0 = prof_now(), ns;
        pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        ns = prof_now() - t0;
        s_sh_flip.pso_ns += ns;
        s_sh_flip.pso++;
        if (sh_trace())
            fprintf(stderr, LOGP "shader: pipeline %.2f ms (blend %u, depth %u), flip %llu\n",
                    ns / 1e6, key[0], key[5], (unsigned long long)s_presents);
    }
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
    int            cube;        /* six faces in a cube texture (nv2a_stage_cube_bind) */
    uint32_t       bytes;       /* nv2a_tex_extent, for tex_cache_stale */
    uint32_t       gen, used;
    uint32_t       pack_gen;    /* nv2a_tex_pack_gen() it was uploaded under */
    /* What the sampler follows (nv2a_stage_lod): the entry's scale over the
     * guest texture, and its chain. 1 and the guest's levels for guest texels. */
    uint32_t       pack_scale, pack_levels;
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
static id<MTLTexture> s_black, s_magenta, s_black_cube;

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

/* A 1x1 cube map of argb: the fallback of a stage bound as a cube. */
static id<MTLTexture> solid_cube(uint32_t argb)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm size:1 mipmapped:NO];
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
    uint32_t f;
    for (f = 0; f < 6; f++)
        [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:f withBytes:&argb
             bytesPerRow:4 bytesPerImage:4];
    return t;
}

static int no_mips(void)
{
    static int off = -1;
    if (off < 0)
        off = recomp_env(RENV_METAL_NO_MIPS) != NULL;
    return off;
}

/* Per-batch CPU memos (RECOMP_DEBUG=metal_memo=0 turns them off for A/B),
 * as D3D11's d3d11_memo: each one gives what the full path would. */
static int memo_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_METAL_MEMO);
        on = !(e && *e == '0');
    }
    return on;
}

static void build_stages(void)
{
    extern uint32_t xbox_ContiguousAllocatedBytes(void);
    const uint32_t *regs = nv2a_pb_tex_regs();
    const uint8_t  *sets = nv2a_pb_tex_regs_set();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    /* The stages are a function of the texture registers, the shader
     * program and the DMA resolve, whose answer moves only with the
     * contiguous arena's high-water mark: unchanged, the last decode
     * stands (D3D11's build_stages). */
    static struct { uint32_t gen, prog, contig; int set, ok; } memo;
    uint32_t gen = nv2a_pb_tex_regs_gen(), contig = xbox_ContiguousAllocatedBytes();
    int n;

    if (memo_on() && memo.ok && memo.gen == gen && memo.prog == v->shader_prog
        && memo.set == v->shader_set && memo.contig == contig)
        return;
    memo.gen = gen;
    memo.prog = v->shader_prog;
    memo.set = v->shader_set;
    memo.contig = contig;
    memo.ok = 1;
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

/* nv2a_stage.cube_rt for this draw's stages, against the live targets: a
 * cube whose faces lie under one is read as 2D (nv2a_stage_cube_bind), as
 * the D3D11 backend does, so the key, the shader and the bind agree. Set
 * every draw: the targets move without the stage registers. */
static void stages_cube_rt(void)
{
    int n, i;
    for (n = 0; n < 4; n++) {
        Stage *t = &s_stage[n];
        t->cube_rt = 0;
        for (i = 0; t->cube && i < RT_MAX && !t->cube_rt; i++)
            /* Only a live target: one the title has since overwritten
             * (a level's cube loaded over a menu's leftover target) is
             * dropped here, as rt_texture drops one at its own address,
             * or the cube would read 2D until the target was evicted. */
            t->cube_rt = s_rt[i].tex
                         && nv2a_stage_cube_over_rt(t, s_rt[i].addr, s_rt[i].pitch * s_rt[i].h)
                         && !rt_drop_if_stale(&s_rt[i], "cube");
    }
}

static uint64_t tex_hash_pal(const uint8_t *mem, const Stage *t, uint32_t bytes)
{
    return nv2a_stage_hash(mem, t, bytes);
}

/* A texture of `levels` levels from src (level 0 first), or nil. `bc`:
 * XRTP_BC_*, src is a pack entry's blocks (XRTP_FLAG_BC1, XRTP_FLAG_BC3),
 * uploaded as they are with a block-row pitch; otherwise A8R8G8B8 texels
 * into BGRA8. */
static id<MTLTexture> tex_make(uint32_t width, uint32_t height, uint32_t levels,
                               const uint32_t *src, int bc)
{
    MTLTextureDescriptor *td;
    id<MTLTexture> tex;
    const uint8_t *p = (const uint8_t *)src;
    uint32_t l;
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:bc == XRTP_BC_3 ? MTLPixelFormatBC3_RGBA
                                                                  : bc == XRTP_BC_1 ? MTLPixelFormatBC1_RGBA
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
        uint32_t row = bc ? (w + 3u) / 4u * (bc == XRTP_BC_1 ? 8u : 16u) : w * 4u;
        uint32_t rows = bc ? (h + 3u) / 4u : h;
        [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:l withBytes:p
               bytesPerRow:row];
        p += (size_t)row * rows;
    }
    return tex;
}

/* A cube map of `size` square faces from src: six faces of A8R8G8B8 texels,
 * each with its `levels` levels, in the NV2A's face order (+X -X +Y -Y +Z
 * -Z, Metal's slices 0-5). */
static id<MTLTexture> tex_make_cube(uint32_t size, uint32_t levels, const uint32_t *src)
{
    MTLTextureDescriptor *td;
    id<MTLTexture> tex;
    const uint8_t *p = (const uint8_t *)src;
    uint32_t f, l;
    td = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               size:size
                                                          mipmapped:levels > 1];
    if (levels > 1)
        td.mipmapLevelCount = levels;
    td.usage = MTLTextureUsageShaderRead;
    tex = [s_dev newTextureWithDescriptor:td];
    if (!tex)
        return nil;
    for (f = 0; f < 6; f++)
        for (l = 0; l < levels; l++) {
            uint32_t w = nv2a_tex_level_dim(size, l);
            [tex replaceRegion:MTLRegionMake2D(0, 0, w, w) mipmapLevel:l slice:f withBytes:p
                   bytesPerRow:w * 4u bytesPerImage:w * w * 4u];
            p += (size_t)w * w * 4u;
        }
    return tex;
}

/* `hash` is the entry's texel hash: with a texture pack loaded
 * (enhancements, textures.pack) an entry it holds is uploaded from the
 * pack's texels at the pack's size instead of the guest's. If the device
 * refuses a texture of the entry's size, the guest texels go up instead:
 * the pack only ever adds detail, never a black texture. */
static id<MTLTexture> tex_upload_body(const Stage *t, uint64_t hash, int cube);
/* What the last tex_upload left (TexEntry.pack_scale, .pack_levels). */
static uint32_t s_up_scale, s_up_levels;

static id<MTLTexture> tex_upload(const Stage *t, uint64_t hash, int cube)
{
    uint64_t t0 = prof_now();
    id<MTLTexture> tex = tex_upload_body(t, hash, cube);
    s_sh_flip.tex_ns += prof_now() - t0;
    s_sh_flip.tex++;
    return tex;
}

/* The guest texels of t into s_texels, or 0 when it cannot grow: `faces`
 * of them (6 for a cube map), each nv2a_stage_face_stride on from the
 * last, packed one after another. */
static int tex_decode_guest(const Stage *t, uint32_t faces)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    size_t n = nv2a_tex_texels(t->width, t->height, t->levels);
    const uint32_t *pal = NULL;
    uint32_t f, stride = faces > 1 ? nv2a_stage_face_stride(t) : 0;
    if (n * faces > s_texels_cap) {
        uint32_t *p = (uint32_t *)realloc(s_texels, n * faces * 4);
        if (!p)
            return 0;
        s_texels = p;
        s_texels_cap = n * faces;
    }
    if (t->color == NV2A_TEX_P8)
        pal = (const uint32_t *)(mem + t->palette);
    for (f = 0; f < faces; f++)
        nv2a_tex_decode(mem + t->offset + f * stride, t->color, t->width, t->height, t->pitch,
                        t->levels, pal, pal ? t->pal_len : 0, s_texels + n * f);
    return 1;
}

static id<MTLTexture> tex_upload_body(const Stage *t, uint64_t hash, int cube)
{
    id<MTLTexture> tex = nil;

    s_up_scale = 1;
    s_up_levels = t->levels;
    if (cube) {
        /* No pack entry is a cube map: the six faces go up from the guest.
         * tex_dump writes face 0, the first texels decoded. */
        if (!tex_decode_guest(t, 6))
            return nil;
        if (nv2a_tex_dump_on())
            nv2a_tex_dump_upload(t, hash, s_texels);
        if (!(tex = tex_make_cube(t->width, t->levels, s_texels)))
            return nil;
        s_tex_uploads++;
        nv2a_stats_add(NV2A_STAT_TEX_UPLOADS, 1);
        return tex;
    }
    if (nv2a_tex_pack_count()) {
        struct nv2a_tex_pack_entry *pe = nv2a_tex_pack_find(hash, t->width, t->height);
        const uint32_t *src = NULL;
        if (pe && pe->bc && !s_bc) {
            static int said;
            if (!said++)
                fprintf(stderr, LOGP "textures.pack: this GPU has no BC textures; BC1 and BC3"
                                     " entries go up from the guest\n");
        } else if (pe) {
            src = nv2a_tex_pack_texels(pe);
        }
        if (src)
            tex = tex_make(pe->width, pe->height, pe->levels, src, pe->bc);
        if (tex) {
            s_up_scale = nv2a_stage_pack_scale(t->width, pe->width);
            s_up_levels = pe->levels;
            nv2a_tex_pack_release(pe);
        }
        /* A refused entry is a miss too, tagged so in the trace. */
        if (!tex)
            nv2a_tex_pack_miss(hash, t->width, t->height, t->color, t->levels);
        /* RECOMP_DEBUG=tex_dump writes the guest's texels, never the
         * pack's: under a hit they are decoded for it alone, once. */
        else if (nv2a_tex_dump_on())
            nv2a_tex_dump_upload(t, hash, nv2a_tex_dump_wants(t, hash)
                                          && tex_decode_guest(t, 1) ? s_texels : NULL);
    }
    if (!tex) {
        if (!tex_decode_guest(t, 1))
            return nil;
        if (nv2a_tex_dump_on())
            nv2a_tex_dump_upload(t, hash, s_texels);
        tex = tex_make(t->width, t->height, t->levels, s_texels, 0);
        if (!tex)
            return nil;
    }
    s_tex_uploads++;
    nv2a_stats_add(NV2A_STAT_TEX_UPLOADS, 1);
    return tex;
}

/* The entry tex_bind's last texture came from (NULL for magenta or black). */
static TexEntry *s_tex_last;

/* `cube`: t goes up as a cube map (nv2a_stage_cube_bind), its six faces
 * hashed and decoded together; tex_bind's fallbacks are 2D, so such a
 * stage gets s_black_cube for them. */
static id<MTLTexture> tex_bind_body(const Stage *t, int cube);

static id<MTLTexture> tex_bind(const Stage *t, int cube)
{
    id<MTLTexture> tex = tex_bind_body(t, cube);
    return cube && tex.textureType != MTLTextureTypeCube ? s_black_cube : tex;
}

static id<MTLTexture> tex_bind_body(const Stage *t, int cube)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bytes = 0, i;
    TexEntry *e = NULL, *victim = &s_tc[0];

    s_tex_last = NULL;
    if (t->valid && (uint64_t)t->width * t->height <= TEX_MAX_TEXELS)
        bytes = cube ? nv2a_stage_cube_extent(t)
                     : nv2a_tex_extent(t->color, t->width, t->height, t->pitch, t->levels);
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
                /* A linear read at a narrower pitch than the target's
                 * restacks the target's rows, no picture of it on the
                 * console either: decode the guest bytes as they stand, as
                 * D3D11 does. KUF's front end reads its XDK supersample
                 * backbuffer through a half-pitch header every flip, and
                 * the write-back was a 4.9 MB readback per flip at scale 1. */
                if (nv2a_rt_pitch_alias(t->color, t->pitch, t->height, rt->fmt, rt->pitch)) {
                    s_pitch_alias_binds++;
                    if (recomp_env(RENV_METAL_TRACE)
                            && nv2a_pair_new(&s_pitch_alias_seen, rt->fmt, rt->pitch, t->color,
                                             t->pitch))
                        fprintf(stderr, LOGP "pitch alias: target 0x%08X fmt 0x%X pitch %u,"
                                " texture 0x%08X fmt 0x%02X %ux%u pitch %u decoded without"
                                " a write-back, flip %llu\n",
                                rt->offset, rt->fmt, rt->pitch, t->offset, t->color, t->width,
                                t->height, t->pitch, (unsigned long long)s_presents);
                    continue;
                }
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
                && c->levels == t->levels && c->cube == cube) {
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
                id<MTLTexture> nt = tex_upload(t, h, cube);
                s_tex_changed++;
                /* The hash moves only with the texels: a failed upload keeps
                 * the old texture and is tried again next flip. */
                if (!nt)
                    return s_black;
                e->hash = h;
                e->pack_gen = pg;
                e->pack_scale = s_up_scale;
                e->pack_levels = s_up_levels;
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
        nt = tex_upload(t, h, cube);
        if (!nt)
            return s_black;
        e->offset = t->offset;
        e->color = t->color;
        e->width = t->width;
        e->height = t->height;
        e->pitch = t->pitch;
        e->palette = t->palette;
        e->levels = t->levels;
        e->cube = cube;
        e->bytes = bytes;
        e->gen = g->flips;
        e->hash = h;
        e->pack_gen = nv2a_tex_pack_gen();
        e->pack_scale = s_up_scale;
        e->pack_levels = s_up_levels;
        e->tex = nt;
        e->live = 1;
    }
    e->used = ++s_tc_clock;
    s_tex_last = e;
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

/* What each stage's bind took from its cache entry, by value: a later
 * stage's bind in the same draw can evict the entry at a small tex_cache.
 * The sampler's scale and chain, and the texel hash (0: no cache entry). */
static uint32_t s_stage_pack[4][2];
static uint64_t s_stage_hash[4];

static void stage_from_cache(int k)
{
    if (s_tex_last) {
        s_stage_pack[k][0] = s_tex_last->pack_scale;
        s_stage_pack[k][1] = s_tex_last->pack_levels;
        s_stage_hash[k] = s_tex_last->hash;
    }
}

/* ---- The trace's buffer: a frame's slots and what each draw id had bound.
 * Several frames, since each stays with the GPU until its command buffer is
 * done; the walker polls them at the flip and folds the ones that are, and
 * waits only when the next one is still out. */
#define MAG_FRAMES 4u
static struct {
    id<MTLBuffer>            buf;
    id<MTLCommandBuffer>     cmd;    /* of the frame's last commit; nil: free */
    struct nv2a_tex_mag_draw *rec;
    unsigned                 n;
} s_mag[MAG_FRAMES];
static unsigned s_mag_cur;
static int      s_mag_state;   /* 0 not tried, 1 ok, -1 failed */

static int mag_init(void)
{
    unsigned k;
    if (s_mag_state)
        return s_mag_state > 0;
    s_mag_state = 1;
    for (k = 0; k < MAG_FRAMES; k++) {
        s_mag[k].buf = [s_dev newBufferWithLength:NV2A_TEX_MAG_BYTES
                                          options:MTLResourceStorageModeShared];
        s_mag[k].rec = (struct nv2a_tex_mag_draw *)malloc(NV2A_TEX_MAG_DRAWS
                                                          * sizeof *s_mag[k].rec);
        if (!s_mag[k].buf || !s_mag[k].rec)
            s_mag_state = -1;
        else
            memset([s_mag[k].buf contents], 0, NV2A_TEX_MAG_BYTES);
    }
    if (s_mag_state < 0)
        fprintf(stderr, LOGP "tex_mag: no device buffer; the trace reads nothing\n");
    return s_mag_state > 0;
}

/* The next draw's id, and where in the frame's buffer it writes: no id in
 * the shader, the slots start at the buffer offset. */
static void mag_draw(void)
{
    uint64_t hash[NV2A_TEX_MAG_STAGES];
    uint32_t scale[NV2A_TEX_MAG_STAGES];
    unsigned k, id;
    for (k = 0; k < NV2A_TEX_MAG_STAGES; k++) {
        hash[k] = s_stage_hash[k];
        scale[k] = s_stage_pack[k][0];
    }
    id = nv2a_tex_mag_draw_id(s_mag[s_mag_cur].rec, &s_mag[s_mag_cur].n, hash, scale);
    [s_enc setFragmentBuffer:s_mag[s_mag_cur].buf
                      offset:(NSUInteger)id * NV2A_TEX_MAG_STAGES * 4u
                     atIndex:2];
}

/* A pre-transformed batch's figure: positions in surface pixels (slot 0)
 * and each stage's texcoord (slot 9 + stage), as the vertex ring holds them. */
static void mag_pretransformed(const float *vf, unsigned nf4, const int *slot,
                               const uint32_t *idx, uint32_t nidx)
{
    unsigned rs = nv2a_host_render_scale(), k, a;
    for (k = 0; k < NV2A_TEX_MAG_STAGES; k++) {
        const Stage *t = &s_stage[k];
        int norm = nv2a_tex_size_from_format(t->color), kp = -1, kt = -1;
        uint32_t i;
        for (a = 0; a < nf4; a++) {
            if (slot[a] == 0)
                kp = (int)a;
            if (slot[a] == 9 + (int)k)
                kt = (int)a;
        }
        if (kp < 0 || kt < 0)
            continue;
        /* A few triangles of each: a 2D batch's pieces share their scale. */
        for (i = 0; s_stage_hash[k] && i + 3 <= nidx && i < 3u * 64u; i += 3) {
            float xy[3][2], uv[3][2];
            int c;
            for (c = 0; c < 3; c++) {
                const float *o = vf + (size_t)idx[i + (uint32_t)c] * nf4 * 4u;
                xy[c][0] = o[(size_t)kp * 4u];
                xy[c][1] = o[(size_t)kp * 4u + 1];
                uv[c][0] = o[(size_t)kt * 4u];
                uv[c][1] = o[(size_t)kt * 4u + 1];
                if (norm) {
                    uv[c][0] *= (float)t->width;
                    uv[c][1] *= (float)t->height;
                }
            }
            nv2a_tex_mag_note(s_stage_hash[k], nv2a_tex_mag_triangle((const float (*)[2])xy,
                                                                     (const float (*)[2])uv) * rs,
                              rs);
        }
    }
}

/* The frame goes with the flip's last command buffer; each frame whose
 * command buffer is done is folded, and the next one starts empty. */
static void mag_fold_frame(unsigned k)
{
    nv2a_tex_mag_fold(s_mag[k].rec, s_mag[k].n, (const uint32_t *)[s_mag[k].buf contents],
                      nv2a_host_render_scale());
    memset([s_mag[k].buf contents], 0, NV2A_TEX_MAG_BYTES);
    s_mag[k].n = 0;
    [s_mag[k].cmd release];
    s_mag[k].cmd = nil;
}

static void mag_flip(void)
{
    unsigned k, next = (s_mag_cur + 1u) % MAG_FRAMES;
    if (s_mag_state <= 0)
        return;
    if (s_mag[s_mag_cur].n && s_last_cmd)
        s_mag[s_mag_cur].cmd = [s_last_cmd retain];
    for (k = 0; k < MAG_FRAMES; k++)
        if (s_mag[k].cmd && [s_mag[k].cmd status] >= MTLCommandBufferStatusCompleted)
            mag_fold_frame(k);
    if (s_mag[next].cmd) {
        [s_mag[next].cmd waitUntilCompleted];
        mag_fold_frame(next);
    }
    /* An empty frame stays current; one without a command buffer is too. */
    if (s_mag[s_mag_cur].cmd || !s_mag[s_mag_cur].n)
        s_mag_cur = next;
}

#define SMP_MAX 128
static struct { uint64_t key; id<MTLSamplerState> s; } s_smp[SMP_MAX];
static int s_smp_n;

/* scale, entry_levels: the bound cache entry's (TexEntry.pack_scale,
 * .pack_levels), 1 and 1 for a render target or a black or magenta one: a
 * pack entry moves the clamps (nv2a_stage_lod). */
static id<MTLSamplerState> sampler(const Stage *t, int bump_source, uint32_t scale,
                                   uint32_t entry_levels)
{
    static int point = -1;
    MTLSamplerDescriptor *sd;
    id<MTLSamplerState> st;
    uint32_t u = t->addr_u & 0xF, v = t->addr_v & 0xF;
    int mn, mg, mip, guest_mip, i;
    unsigned an;
    uint64_t key;
    int bc = 0;
    struct nv2a_stage_lod lod;

    if (point < 0)
        point = recomp_env(RENV_METAL_POINT) != NULL;
    if (u == NV2A_ADDR_BORDER || v == NV2A_ADDR_BORDER)
        bc = border_colour(t->border);
    mn = !point && filter_linear(t->filter, 0);
    mg = !point && filter_linear(t->filter, 1);
    guest_mip = t->levels > 1 ? filter_mip(t->filter) : 0;
    lod = nv2a_stage_lod(guest_mip, t->lod_min, t->lod_max, scale, entry_levels);
    mip = lod.mip;
    /* render.aniso, on the D3D11 sampler's rule (nv2a_stage_aniso) and key
     * bit: a bump source in one draw and plain in the next gets its own. */
    an = nv2a_stage_aniso(t, guest_mip, mn, bump_source);
    key = u | v << 4 | (uint64_t)mn << 8 | (uint64_t)mg << 9 | (uint64_t)mip << 10
        | (uint64_t)an << 35 | (uint64_t)bc << 40 | lod.key;
    if (guest_mip)
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
    sd.lodMinClamp = lod.min;
    sd.lodMaxClamp = lod.max;
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
 * the state's bytes. Each compiles once, with the shared options (metal_compile.h). A
 * source that fails to compile is logged with its text, counted
 * (s_compile_errors, in the present log) and remembered as bad. */

static uint64_t s_compile_errors, s_vs_compiled, s_fs_compiled;

static id<MTLFunction> compile_fn(const char *src, NSString *name, const char *what)
{
    static int nonce_read;
    static const char *nonce, *dump;
    NSError *err = nil;
    NSString *text = [NSString stringWithUTF8String:src];
    id<MTLLibrary> lib;
    id<MTLFunction> fn;
    uint64_t t0, ns;

    if (!nonce_read) {
        nonce_read = 1;
        nonce = recomp_env(RENV_METAL_SHADER_NONCE);
        dump = recomp_env(RENV_METAL_SHADER_DUMP);
    }
    if (dump) {
        static int dump_said;
        char path[1024];
        FILE *f = NULL;
        int n = snprintf(path, sizeof path, "%s%s.metal", dump, what);
        if (n > 0 && (size_t)n < sizeof path) {
            for (char *c = path + strlen(dump); *c; c++)
                if (*c == ' ')
                    *c = '_';
            f = fopen(path, "w");
        }
        if (f) {
            fputs(src, f);
            fclose(f);
        } else if (!dump_said++) {
            fprintf(stderr, LOGP "metal_shader_dump: cannot write %s (missing directory or"
                    " path too long); no more dump warnings\n", n > 0 && (size_t)n < sizeof path
                    ? path : dump);
        }
    }
    /* A new entry name is new AIR: both the source and the function caches
     * of macOS miss, as on a player's first launch. */
    if (nonce && *nonce) {
        NSString *nn = [NSString stringWithFormat:@"%@_%s_%d", name, nonce, (int)getpid()];
        text = [text stringByReplacingOccurrencesOfString:
                     [NSString stringWithFormat:@" %@(", name]
                 withString:[NSString stringWithFormat:@" %@(", nn]];
        name = nn;
    }
    t0 = prof_now();
    lib = metal_compile_library(s_dev, text, what, &err);
    ns = prof_now() - t0;
    s_sh_flip.lib_ns += ns;
    s_sh_flip.lib++;
    if (sh_trace())
        fprintf(stderr, LOGP "shader: %s library %.2f ms (%zu bytes), flip %llu\n",
                what, ns / 1e6, strlen(src), (unsigned long long)s_presents);
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
    struct nv2a_aspect_gate hp;   /* hor+: is this program 2D */
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
        e->hp.two_d = -1;
        e->hp.n = 0;
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

/* The fragment function for a batch without combiners: fs_basic, or
 * while render.snap_2d is on fs_basic_snap (s_msl_snap), built the first
 * time it is asked for. fs_basic when it cannot be built. */
static id<MTLFunction> fs_basic(void)
{
    static int tried;
    static id<MTLFunction> fn;

    if (!nv2a_snap_2d_on(nv2a_host_render_scale()))
        return s_fs_basic;
    if (!tried) {
        tried = 1;
        fn = compile_fn(s_msl_snap, @"fs_basic_snap", "fs_basic_snap");
    }
    return fn ? fn : s_fs_basic;
}

/* fs_select returned the combiners' function: only that one declares
 * cube maps, so only then does a stage bind one (nv2a_stage_cube_bind). A
 * batch that falls back to fs_basic (no MSL, a compile failure, FS_MAX)
 * samples face 0 as 2D, as before. */
static int s_fs_comb;

/* fs_select returned the combiners' W variant (nv2a_w_depth_on): the
 * vertex side's W mode (vc.u[7]) and the WDepth buffer go with it, and with
 * nothing else, so a fallback to fs_basic keeps the stock z. */
static int s_fs_w;

/* RECOMP_TRACE=tex_mag (nv2a_tex_dump.h): this batch's combiners take the
 * trace's variant: a program batch. A pre-transformed one has its figure from
 * its vertices (mag_pretransformed), exact, with no atomics to pay. As on
 * D3D11, in the same terms. */
static int s_fs_mag;

static id<MTLFunction> fs_select_body(const struct nv2a_pb_vsh *v, int w);

/* The fragment function for the batch: its register combiners when they
 * are programmed (v->rc_set and a stage count), else fs_basic. As the D3D11
 * setup_pixel, the texture shader's modes are nv2a_stage_ps_mode's: cube
 * maps where a stage binds one, the dot-product family, BUMPENVMAP, and 2D
 * for every other sampling mode. */
static id<MTLFunction> fs_select(const struct nv2a_pb_vsh *v, int w)
{
    id<MTLFunction> fn = fs_select_body(v, w);
    s_fs_comb = fn != s_fs_basic && fn != fs_basic();
    s_fs_w = w && s_fs_comb;
    return fn;
}

static id<MTLFunction> fs_select_body(const struct nv2a_pb_vsh *v, int w)
{
    static char msl[65536];
    static struct { int valid; uint32_t r[38]; id<MTLFunction> fn; } memo;
    NV2ACombinerState st;
    uint32_t key[38], modes = 0, inputs = 0, hash;
    FsEntry *e = NULL;
    char what[32];
    int n;

    if (!(v->rc_set && (v->rc_ctl & 0xFF)))
        return fs_basic();
    nv2a_ps_stage_key(s_stage, v->other_stage_input, v->dot_rgbmapping, &modes, &inputs);
    memcpy(key, v->rc_cicw, 32);
    memcpy(key + 8, v->rc_aicw, 32);
    memcpy(key + 16, v->rc_cocw, 32);
    memcpy(key + 24, v->rc_aocw, 32);
    key[32] = v->rc_ctl;
    key[33] = v->rc_fcw0;
    key[34] = v->rc_fcw1;
    key[35] = modes;
    /* render.scale changes at run time: each scale class has its own text */
    key[36] = (nv2a_host_render_scale() > 1) | (nv2a_host_opts()->aspect_w ? 2u : 0u)
            | (nv2a_snap_2d_on(nv2a_host_render_scale()) ? 4u : 0u)
            | (w ? 8u : 0u)    /* W-buffered: the w_depth variant */
            | (s_fs_mag ? 16u : 0u);   /* the trace's variant */
    key[37] = inputs;
    if (memo.valid && !memcmp(memo.r, key, sizeof key))
        return memo.fn;

    memset(&st, 0, sizeof st);
    d3d8_combiners_from_regs(v->rc_cicw, v->rc_aicw, v->rc_cocw, v->rc_aocw,
                             v->rc_ctl, v->rc_fcw0, v->rc_fcw1, modes, &st);
    st.fog_input = 1;   /* FOG.a is the interpolated vertex fog factor */
    st.rect_snap = (int)(key[36] & 1u);   /* nv2a_snap_uv; in the hash via st */
    st.rt_offset = (int)(key[36] >> 1 & 1u);   /* hor+: tex_scale.w, nv2a_aspect_place */
    st.snap_2d = (int)(key[36] >> 2 & 1u);     /* render.snap_2d: nv2a_snap_uv2d */
    st.w_depth = (int)(key[36] >> 3 & 1u);     /* Z_PERSPECTIVE: nv2a_w_depth_on */
    st.tex_mag = (int)(key[36] >> 4 & 1u);     /* RECOMP_TRACE=tex_mag */
    for (n = 0; n < 4; n++) {
        st.tex_input[n] = (int)((inputs >> (4 * n)) & 0xF);
        st.dot_map[n] = n ? (int)((inputs >> (16 + 4 * (n - 1))) & 0xF) : 0;
        st.tex_cube[n] = (int)((inputs >> (28 + n)) & 1u);
    }
    st.bump_on = 1;
    hash = fnv(&st, sizeof st);
    if (!s_fs && !(s_fs = (FsEntry *)calloc(FS_MAX, sizeof *s_fs)))
        return fs_basic();
    for (n = 0; n < s_fs_n; n++)
        if (s_fs[n].hash == hash && !memcmp(&s_fs[n].st, &st, sizeof st)) {
            e = &s_fs[n];
            break;
        }
    if (!e) {
        if (s_fs_n == FS_MAX)
            return fs_basic();
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
    memo.fn = e->bad ? fs_basic() : e->fn;   /* as D3D11: the basic PS */
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
 * present surface). nv2a_rt_tex_reads says which reads are the target's,
 * 16-bit ones included: a write-back keeps only the stock columns, so under
 * hor+ a 16-bit frame copy decoded from it was a centred 4:3 picture.
 * The low 27 bits of the address compare, so either memory
 * window matches. The target being
 * drawn into is sampled through a copy (s_self), made by a blit between
 * passes. RECOMP_METAL_NO_RTT=1 decodes guest memory instead (A/B). */
static RenderTarget s_self;   /* tex, w, h: the copy; offset: whose */

static int aspect_trace(void);

static RenderTarget *rt_texture(const Stage *t, RenderTarget *cur)
{
    static int off = -1;
    RenderTarget *self = NULL;
    int i;

    if (off < 0)
        off = recomp_env(RENV_METAL_NO_RTT) != NULL;
    if (off || !t->offset)
        return NULL;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->tex || ((rt->addr ^ t->offset) & 0x07FFFFFFu))
            continue;
        if (!nv2a_rt_tex_reads(t->color, t->pitch, rt->fmt, rt->pitch)) {
            /* Decoded from guest memory instead: the write-back's stock
             * columns, or what the title left there. The format
             * word is the last draw's: reuse matches offset and size only. */
            if (aspect_trace() && nv2a_rt_reject_new(rt->fmt, rt->pitch, t->color, t->pitch))
                fprintf(stderr, "[aspect.rt-reject] flip %llu: target 0x%08X surface fmt %X"
                        " pitch %u, texture fmt %X pitch %u\n",
                        (unsigned long long)s_presents, rt->addr, rt->fmt, rt->pitch, t->color,
                        t->pitch);
            continue;
        }
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
    s_self.pad = self->pad;
    s_self.n = self->n;
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

/* RECOMP_TRACE=aspect: one line per vertex program (0: pass-through) and
 * placement the first time a 2D batch of it is placed, with what it
 * samples: the inventory display.aspect's rules and any per-program
 * override are judged by. */
static int aspect_trace(void)
{
    static int on = -1;
    if (on < 0)
        on = recomp_env(RENV_ASPECT_TRACE) != NULL;
    return on;
}

/* Hor+: whether stage k of a 2D batch reads src as a frame copy where the
 * batch lies (nv2a_aspect_place_copy): src is padded, a blit filled it in
 * this flip and nothing has drawn into it since, and the batch's texcoords
 * track its screen position (nv2a_aspect_uv_tracks). A copy from an
 * earlier flip is a still frame: BLiNX 2 copies its back buffer to the
 * display buffer at a few flips (764, 890, 892, 3959 and 3971 of its
 * golden), and that copy must not make a later HUD batch over it a lens. */
static int aspect_copy_tracks(const RenderTarget *src, const Stage *t, unsigned k,
                              const uint32_t (*prog)[4], uint32_t start, float (*c)[4],
                              const float *vf, unsigned nf4, const int *slot, uint32_t count,
                              uint32_t hash)
{
    float xy[NV2A_ASPECT_UV_MAX][2], uv[NV2A_ASPECT_UV_MAX][2];
    uint32_t n;
    int norm, ok;

    if (!src->copy || !src->pad || src->copy_flip != (uint32_t)s_presents)
        return 0;
    n = nv2a_aspect_uv_sample(prog, start, c, vf, nf4, slot, count, k, xy, uv,
                              NV2A_ASPECT_UV_MAX);
    /* A swizzled texture's coordinates are normalised, a linear one's are
     * texels (the NV2A's rect textures). */
    norm = nv2a_tex_size_from_format(t->color);
    ok = nv2a_aspect_uv_tracks((const float (*)[2])xy, (const float (*)[2])uv, n,
                               norm ? (float)t->width : 1.0f, norm ? (float)t->height : 1.0f,
                               src->w, src->h);
    if (aspect_trace() && n) {
        static int said;
        if (said++ < 64)
            fprintf(stderr, LOGP "aspect: vs %08X stage %u reads copy 0x%08X: %u verts,"
                    " xy (%.1f,%.1f) uv (%.3f,%.3f), xy (%.1f,%.1f) uv (%.3f,%.3f): %s\n",
                    hash, k, src->offset, count, xy[0][0], xy[0][1], uv[0][0], uv[0][1],
                    xy[n - 1][0], xy[n - 1][1], uv[n - 1][0], uv[n - 1][1],
                    ok ? "tracks, stretched" : "does not track");
    }
    return ok;
}

static void aspect_log(uint32_t hash, uint32_t count, uint32_t ntri, float lo_x,
                       float hi_x, const RenderTarget *rt, int place, int lift, int wide)
{
    static const char *const names[] = { "centre", "stretch", "cover" };
    /* Said once per program, stage-0 texture and placement: the XDK's
     * fixed function is one program for most of the 2D, so the texture
     * tells the pieces apart. An open-addressed set; full, it stops. */
    enum { SET = 4096 };
    static uint64_t set[SET];
    static unsigned used;
    const Stage *t0 = &s_stage[0];
    RenderTarget *src;
    uint64_t key = ((uint64_t)hash << 32 | (t0->mode ? t0->offset : 0)) * 0x9E3779B97F4A7C15ull
                   + (uint64_t)(place | lift << 4 | wide << 5) + 1u;
    unsigned h = (unsigned)(key >> 52) & (SET - 1u);
    while (set[h] && set[h] != key)
        h = (h + 1u) & (SET - 1u);
    if (set[h] || used >= SET / 2u)
        return;
    set[h] = key;
    used++;
    src = !t0->mode || t0->mode == 4 ? NULL : rt_texture(t0, (RenderTarget *)rt);
    fprintf(stderr, "[aspect.program] %08X flip %llu: %u vertices %u tris x %g..%g on %ux%u"
            " pad %u at 0x%08X pitch %u, stage 0 %s 0x%08X fmt %X %ux%u pitch %u -> %s%s%s\n", hash,
            (unsigned long long)s_presents, count, ntri, lo_x, hi_x, rt->w, rt->h, rt->pad,
            rt->addr, rt->pitch,
            !t0->mode || t0->mode == 4 ? "none" : src ? "rt" : "tex", t0->offset,
            t0->color, t0->width, t0->height, t0->pitch, names[place], lift ? " lift" : "",
            wide ? " wide" : "");
}

/* RECOMP_TRACE=aspect: one line per program and w the first time a flat
 * near layer of it is clipped to the guest columns (nv2a_aspect_flat). */
static void aspect_log_flat(uint32_t hash, uint32_t count, float w, const RenderTarget *rt)
{
    enum { SET = 256 };
    static uint64_t set[SET];
    static unsigned used;
    union { float f; uint32_t u; } wb = { w };
    uint64_t key = ((uint64_t)hash << 32 | wb.u) * 0x9E3779B97F4A7C15ull + 1u;
    unsigned h = (unsigned)(key >> 56) & (SET - 1u);
    while (set[h] && set[h] != key)
        h = (h + 1u) & (SET - 1u);
    if (set[h] || used >= SET / 2u)
        return;
    set[h] = key;
    used++;
    fprintf(stderr, "[aspect.flat] %08X flip %llu: %u vertices at w %g on %ux%u pad %u at"
            " 0x%08X\n", hash, (unsigned long long)s_presents, count, w, rt->w, rt->h, rt->pad,
            rt->addr);
}

/* RECOMP_ASPECT_TRACE: each program and target size the first time
 * nv2a_aspect_place_tex drops a guest picture: a full-width 2D pass that
 * samples a padded target and a picture, placed as a post pass, not a
 * movie (D3D11's aspect_log_post, same line). */
static void aspect_log_post(uint32_t hash, const RenderTarget *rt, int place)
{
    static const char *const names[] = { "centre", "stretch", "cover" };
    enum { SET = 256 };
    static uint64_t set[SET];
    static unsigned used;
    uint64_t key = ((uint64_t)hash << 32 | rt->w << 16 | rt->h) * 0x9E3779B97F4A7C15ull + 1u;
    unsigned h = (unsigned)(key >> 56) & (SET - 1u);
    while (set[h] && set[h] != key)
        h = (h + 1u) & (SET - 1u);
    if (set[h] || used >= SET / 2u)
        return;
    set[h] = key;
    used++;
    fprintf(stderr, "[aspect.post] %08X flip %llu: picture beside a padded target on %ux%u"
            " pad %u at 0x%08X -> %s\n", hash, (unsigned long long)s_presents, rt->w, rt->h,
            rt->pad, rt->addr, names[place]);
}

static void metal_on_draw(const struct nv2a_pb_draw *d)
{
    /* The expanded list: at most three entries per index (a fan or a quad
     * strip), grown to the largest batch so far rather than sized to
     * NV_MAX_INDICES. */
    static uint32_t *idx;
    static uint32_t idx_cap;
    static float (*hxy)[2];        /* hor+: a 2D batch's vertex x, y (full width) */
    static uint32_t hxy_cap;
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
    int full_w = 0, guest_tex = 0, flat_rt = 0, two_d = 0, have_x = 0, tris = 0, flat = 0;
    int padded_rt = 0;
    int lift = 0, wide = 0;
    int vary_z = 0;     /* a program's 2D batch off one oPos.z (nv2a_aspect_gate) */
    int scan;
    int place = NV2A_PLACE_CENTRE;
    float lo_x = 0, hi_x = 0;   /* hor+: a 2D batch's x range, surface pixels */
    const RenderTarget *srcs[4] = { NULL, NULL, NULL, NULL };
    VsEntry *ve = NULL;
    /* Stencil only on a Z24S8 zeta surface, as on the CPU. */
    int use_s = v->stencil_enable && ((g->format >> 4) & 0xF) == 2;
    int use_z = v->depth_enable || use_s;
    id<MTLTexture> stex[4];

    if (!init())
        return;
    /* A mod's batch filter (nv2a_backend_common.h); D3D11 asks it at the
     * same point. Nothing without the enhancements layer. */
    if (nv2a_batch_skip(d, g, v))
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
            s_draws_prog += d->batches;
            if (!e) {
                s_draws_skipped++;
                return;
            }
            ve = e;
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
        stages_cube_rt();
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
        /* The variant declares the buffer: no buffer, no variant. */
        s_fs_mag = nv2a_tex_mag_on() && d->program && mag_init();
        ffn = fs_select(v, nv2a_w_depth_on(v, d->program != 0,
                                           v->rc_set && (v->rc_ctl & 0xFF)));
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

        /* Hor+: which batches are 2D, and their x range (nv2a_aspect_place
         * decides from it). A pass-through batch is 2D, its positions in
         * surface pixels; a program batch is 2D when its own output is
         * orthographic (nv2a_aspect_scan). With render.snap_2d above
         * render scale 1 the 2D test runs without hor+ too: a 2D batch's
         * stages sample at their 1x points (nv2a_snap_flat2d). */
        full_w = 0;
        scan = rt->pad || nv2a_snap_2d_on(rt->n);
        tris = rt->pad && topo == NV2A_TOPO_TRIANGLES;
        if (tris && count > hxy_cap) {
            float (*grown)[2] = (float (*)[2])realloc(hxy, (size_t)count * sizeof *hxy);
            if (grown) {
                hxy = grown;
                hxy_cap = count;
            } else {
                tris = 0;
            }
        }
        if (rt->pad && !d->program && nf4 > 0 && s_vtx_attr[0] == 0 && count) {
            lo_x = hi_x = vf[0];
            for (i = 0; i < count; i++) {
                float x = vf[(size_t)i * nf4 * 4u];
                if (x < lo_x) lo_x = x;
                if (x > hi_x) hi_x = x;
                if (tris) {
                    hxy[i][0] = x;
                    hxy[i][1] = vf[(size_t)i * nf4 * 4u + 1];
                }
            }
            two_d = have_x = 1;
        } else if (!rt->pad && scan && !d->program) {
            two_d = 1;
        } else if (scan && d->program && ve && count) {
            two_d = have_x = nv2a_aspect_scan(&ve->hp, (const uint32_t (*)[4])v->prog, v->start,
                                              (float (*)[4])(uintptr_t)v->c, vf, nf4, slot,
                                              count, NULL, &lo_x, &hi_x);
            vary_z = have_x && ve->hp.vary_z;
            /* Full-width bounds: every vertex, for the per-triangle test
             * (a 2D program is scanned every batch, so the gate passes). */
            if (have_x && tris && nv2a_aspect_full_width(lo_x, hi_x, rt->w))
                nv2a_aspect_scan(&ve->hp, (const uint32_t (*)[4])v->prog, v->start,
                                 (float (*)[4])(uintptr_t)v->c, vf, nf4, slot, count, hxy,
                                 &lo_x, &hi_x);
        }
        if (have_x)
            full_w = nv2a_aspect_full(lo_x, hi_x, tris ? (const float (*)[2])hxy : NULL,
                                      tris ? idx : NULL, nidx / 3u, rt->w, rt->h, &wide);

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
                s_stage_pack[k][0] = s_stage_pack[k][1] = 1;
                s_stage_hash[k] = 0;
                if (!t->mode || t->mode == 4)
                    continue;
                textured = 1;
                /* A cube map's faces are decoded from guest memory: one
                 * over a live target is not bound as a cube (cube_rt).
                 * Only the combiners' shader declares one (s_fs_comb);
                 * fs_basic samples face 0. A dot stage that reads no
                 * texture keeps the black one: nothing to decode. */
                if (s_fs_comb && !nv2a_texmode_samples((int)nv2a_stage_ps_mode(t, k))) {
                    continue;
                } else if (s_fs_comb && nv2a_stage_cube_bind(t, nv2a_stage_ps_mode(t, k))) {
                    stex[k] = tex_bind(t, 1);
                    stage_from_cache(k);
                } else if ((src = rt_texture(t, rt)) != NULL) {
                    stex[k] = src->tex;
                    nv2a_ps_consts_rt(&mpc.b, k, t, src->w, src->h);
                    if (nv2a_rt_no_alpha(nv2a_tex_linear_twin(t->color)))
                        mpc.b.alpha_only[k] = 2;
                    srcs[k] = src;
                    if (!src->pad)
                        flat_rt = 1;
                    else
                        padded_rt = 1;
                    s_rtt_binds++;
                    nv2a_tex_dump_rt(t);
                } else {
                    stex[k] = tex_bind(t, 0);
                    stage_from_cache(k);
                    if (nv2a_aspect_picture(t->width, rt->w, bump_src >> k & 1))
                        guest_tex = 1;
                }
            }
            if (textured)
                s_draws_tex++;
        }
        /* Hor+: a perspective screen quad over padded targets is a
         * full-width pass too (nv2a_aspect_screen_quad) and marks the third
         * it lies in as holding a bar; one drawn without depth in such a
         * third is stretched as a piece of a letterbox band
         * (nv2a_aspect_band). */
        if (rt->pad && d->program && ve && topo == NV2A_TOPO_TRIANGLES && !two_d) {
            uint32_t flips = nv2a_pb_gpu_state()->flips;
            unsigned bars = nv2a_aspect_bars(rt->bar_flip, flips);
            int quad = nv2a_aspect_quad_ok(srcs[0] || srcs[1] || srcs[2] || srcs[3], flat_rt,
                                           guest_tex);
            int band = bars && !v->depth_enable && !flat_rt && !guest_tex;
            float qxy[NV2A_ASPECT_QUAD_MAX][2];
            if ((quad || band)
                && nv2a_aspect_screen_quad((const uint32_t (*)[4])v->prog, v->start,
                                           (float (*)[4])(uintptr_t)v->c, vf, nf4, slot, count,
                                           qxy, &lo_x, &hi_x)) {
                if (quad && nv2a_aspect_quad_span(lo_x, hi_x, rt->w)
                    && nv2a_aspect_full_tris((const float (*)[2])qxy, idx, nidx / 3u, rt->w)) {
                    two_d = full_w = 1;
                    nv2a_aspect_bar_mark(rt->bar_flip,
                                         nv2a_aspect_third((const float (*)[2])qxy, count, rt->h),
                                         flips);
                } else if (band && nv2a_aspect_band((const float (*)[2])qxy, count, rt->w, rt->h,
                                                    bars)) {
                    two_d = full_w = 1;
                }
            }
        }
        /* Hor+: a flat near layer drawn without depth keeps its place but
         * is clipped to the guest columns like the 2D (nv2a_aspect_flat). */
        flat = 0;
        if (rt->pad && d->program && ve && !two_d && !v->depth_enable && count) {
            float fw;
            flat = nv2a_aspect_flat((const uint32_t (*)[4])v->prog, v->start,
                                    (float (*)[4])(uintptr_t)v->c, vf, nf4, slot, count, &fw);
            if (flat && aspect_trace())
                aspect_log_flat(ve->hash, count, fw, rt);
        }
        /* A square target is a reflection, glow or shadow map, never a
         * movie: display.movie applies to the frame only. */
        place = rt->pad ? nv2a_aspect_place(two_d, full_w,
                                            nv2a_aspect_place_tex(guest_tex, padded_rt), flat_rt,
                                            rt->w == rt->h ? NV2A_MOVIE_BLACK
                                                           : nv2a_host_opts()->movie_mode)
                        : NV2A_PLACE_CENTRE;
        if (rt->pad && two_d && place == NV2A_PLACE_CENTRE) {
            int copy_rt = 0;
            for (k = 0; k < 4 && !copy_rt; k++)
                copy_rt = srcs[k]
                          && aspect_copy_tracks(srcs[k], &s_stage[k], k,
                                                d->program ? (const uint32_t (*)[4])v->prog
                                                           : NULL,
                                                v->start, (float (*)[4])(uintptr_t)v->c, vf,
                                                nf4, slot, count, ve ? ve->hash : 0);
            place = nv2a_aspect_place_copy(place, two_d, copy_rt);
        }
        if (nv2a_snap_2d_on(rt->n)) {
            /* render.snap_2d: a centred 2D batch samples guest textures at
             * its guest pixels' 1x points (nv2a_uv_snap), as setup_pixel. */
            unsigned rt_mask = 0;
            for (k = 0; k < 4; k++)
                if (srcs[k])
                    rt_mask |= 1u << k;
            nv2a_ps_consts_snap(&mpc.b, s_stage, rt_mask, rt->n,
                                nv2a_snap_flat2d(two_d && !vary_z, place));
        }
        if (place != NV2A_PLACE_CENTRE)
            s_draws_stretch++;
        /* A HUD mod clipped its elements in guest memory: lift the 4:3
         * clip of the 2D it moved (nv2a_aspect_hud_lift). */
        lift = rt->pad && nv2a_aspect_hud_lift(rt->w, rt->h, two_d, flat, full_w,
                                               have_x ? lo_x : 0.0f,
                                               have_x ? hi_x : (float)rt->w);
        if (two_d && rt->pad && aspect_trace())
            aspect_log(ve ? ve->hash : 0, count, nidx / 3u, lo_x, hi_x, rt, place, lift, wide);
        if (rt->pad && two_d && full_w && guest_tex && padded_rt && aspect_trace())
            aspect_log_post(ve ? ve->hash : 0, rt, place);
        /* A batch sampling a padded target the stock way reads it whole,
         * pads and all, which is what a stretched pass wants, short of a
         * square's overscan (nv2a_rt_stretch_read). Any other reads its
         * guest columns (nv2a_rt_guest_read; tex_scale.w,
         * rt_offset), on an unpadded target too: Burnout 3's 640x464 frame
         * has no pad, but the squares it samples do. */
        for (k = 0; k < 4; k++) {
            const RenderTarget *src = srcs[k];
            float sc, off;
            if (!src || !src->pad)
                continue;
            if (place != NV2A_PLACE_STRETCH && place != NV2A_PLACE_COVER)
                nv2a_rt_guest_read(src->w, src->pad, &sc, &off);
            /* Not a square's overscan: a stretch spans the ratio's pad. */
            else if (!nv2a_rt_stretch_read(src->w, src->h, src->pad, &sc, &off))
                continue;
            mpc.b.tex_scale[k][0] *= sc;
            mpc.b.tex_scale[k][3] = off;
        }
        nv2a_vp_consts_fill(v, g->format, rt->w, rt->h, rt->pad, &vc);
        nv2a_aspect_map(&vc, rt->w, rt->h, rt->pad, place);
        nv2a_vp_tie_fill(&vc, rt->hw, rt->hh);
        vc.u[7] = s_fs_w ? 1u : 0u;   /* spec_ctl.w: the epilogue's W mode */

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
            nv2a_aspect_box(b, rt->w, rt->h, rt->pad, rt->n, place, two_d || flat, lift, hb);
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
            /* Polygon offset (nv2a_depth_bias); a W batch takes it in its
             * shader instead, the hardware bias not reaching a written
             * depth. A new encoder starts at 0, so a batch without offset
             * sets nothing. */
            static uint64_t bias_enc;
            static float bias_set[2];
            float zb = 0.0f, zs = 0.0f;
            if (!s_fs_w)
                nv2a_depth_bias(v, d->prim, g->format, nv2a_host_render_scale(), &zb, &zs);
            if (bias_enc != s_enc_serial) {
                bias_enc = s_enc_serial;
                bias_set[0] = bias_set[1] = 0.0f;
            }
            if (zb != bias_set[0] || zs != bias_set[1]) {
                [s_enc setDepthBias:zb slopeScale:zs clamp:0.0f];
                bias_set[0] = zb;
                bias_set[1] = zs;
            }
        }
        {
            float bf[4];
            (void)nv2a_blend_constant(g->blend_enable, g->blend_sfactor,
                                      g->blend_dfactor, g->blend_color, bf, &(int){0});
            [s_enc setBlendColorRed:bf[0] green:bf[1] blue:bf[2] alpha:bf[3]];
        }
        [s_enc setVertexBuffer:s_vb offset:vb_off atIndex:0];
        if (d->program) {
            /* c[192] is 3 KB: under setVertexBytes' 4 KB. An encoder keeps
             * its bindings from draw to draw, so while it is the same
             * encoder, buffer 1 still holds c[] and the generation says
             * nothing wrote c[] since (nv2a_pb_vsh_consts_gen), the 3 KB
             * copy is skipped: most batches of a busy frame. */
            uint32_t cg = nv2a_pb_vsh_consts_gen();
            if (!memo_on() || !s_vc1.ok || s_vc1.enc != s_enc_serial || s_vc1.gen != cg) {
                [s_enc setVertexBytes:v->c length:sizeof v->c atIndex:1];
                s_vc1.enc = s_enc_serial;
                s_vc1.gen = cg;
                s_vc1.ok = 1;
                s_c_uploads++;
            }
            [s_enc setVertexBytes:&vc length:sizeof vc atIndex:2];
        } else {
            [s_enc setVertexBytes:&vc length:sizeof vc atIndex:1];
            s_vc1.ok = 0;              /* buffer 1 is vc now, not c[] */
        }
        [s_enc setFragmentBytes:&mpc length:sizeof mpc atIndex:0];
        if (s_fs_w) {
            struct nv2a_wdepth_consts wc;
            nv2a_wdepth_consts_fill(v, d->prim, g->format, nv2a_host_render_scale(), &wc);
            [s_enc setFragmentBytes:&wc length:sizeof wc atIndex:1];
        }
        for (k = 0; k < 4; k++) {
            [s_enc setFragmentTexture:stex[k] atIndex:k];
            [s_enc setFragmentSamplerState:sampler(&s_stage[k], bump_src >> k & 1,
                                                   s_stage_pack[k][0], s_stage_pack[k][1])
                                     atIndex:k];
        }
        /* A read of this batch's inputs only: the dump must not move a frame. */
        if (nv2a_tex_dump_on()) {
            int dump_2d = !d->program;
            if (d->program && nv2a_tex_dump_draw_open(s_stage_hash))
                dump_2d = nv2a_aspect_batch_2d((const uint32_t (*)[4])v->prog, v->start,
                                               (float (*)[4])(uintptr_t)v->c, vf, nf4, slot,
                                               count, v->depth_enable);
            nv2a_tex_dump_draw(s_stage_hash, bump_src, dump_2d);
        }
        if (s_fs_mag && s_fs_comb)
            mag_draw();
        else if (nv2a_tex_mag_on() && !d->program && topo == NV2A_TOPO_TRIANGLES)
            mag_pretransformed(vf, nf4, slot, idx, nidx);
        [s_enc drawIndexedPrimitives:ptype indexCount:nidx indexType:MTLIndexTypeUInt32
                         indexBuffer:s_ib indexBufferOffset:ib_off];
        s_draws += d->batches;
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

/* One row of rt's BGRA8 pixels into a 16-bit guest row: in the target's
 * X1R5G5B5 or R5G6B5 format with its constant bits (nv2a_surf_pack_row),
 * R5G6B5 for any other format seen at a 16-bit pitch. */
static void pack16(const RenderTarget *rt, const uint32_t *row, uint16_t *o, uint32_t w)
{
    uint32_t f = rt->fmt & 0xFu, x;

    if (f == 0x1 || f == 0x2 || f == 0x3) {
        nv2a_surf_pack_row(f, (const uint8_t *)row, w, (uint8_t *)o);
        return;
    }
    for (x = 0; x < w; x++) {
        uint32_t c = row[x];
        o[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
    }
}

/* The X8R8G8B8 and X1A7R8G8B8 formats' constant alpha bits over the w x h
 * 32-bit guest pixels just written at dst (nv2a_surf_pack_row, in place):
 * the host alpha is what the combiners made, the guest's what the surface
 * stores. */
static void fix32(const RenderTarget *rt, uint8_t *dst, uint32_t pitch, uint32_t h)
{
    uint32_t f = rt->fmt & 0xFu, y;

    if (f < 0x4 || f > 0x7)
        return;
    for (y = 0; y < h; y++)
        nv2a_surf_pack_row(f, dst + (size_t)y * pitch, rt->w, dst + (size_t)y * pitch);
}

/* rt's pixels into the guest surface at `addr`, `pitch` bytes a row: 32-bit
 * as they are (BGRA8 is X8R8G8B8's byte order) but for the constant alpha
 * bits (fix32), 16-bit packed by pack16. Runs after the wait, on the
 * executor's thread. */
static void write_back(RenderTarget *rt, uint32_t addr, uint32_t pitch)
{
    uint8_t *dst = (uint8_t *)xbox_GetMemoryOffset() + addr;
    uint32_t bpp = pitch / rt->w, w = rt->w, y;

    if (bpp >= 4) {
        [rt->tex getBytes:dst bytesPerRow:pitch fromRegion:MTLRegionMake2D(0, 0, w, rt->h)
              mipmapLevel:0];
        fix32(rt, dst, pitch, rt->h);
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
            pack16(rt, row, o, w);
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
 * sees a w x h frame at its pitch, packed as write_back packs. */
static void write_back_scaled(RenderTarget *rt, const uint8_t *px, uint32_t addr,
                              uint32_t pitch)
{
    uint8_t *dst = (uint8_t *)xbox_GetMemoryOffset() + addr;
    uint32_t bpp = pitch / rt->w, w = rt->w, n = rt->n, y;

    px += (size_t)rt->pad * n * 4u;   /* hor+: the guest columns, centred */
    if (bpp >= 4) {
        nv2a_downscale_box32(px, rt->hw * 4u, w, rt->h, n, dst, pitch);
        fix32(rt, dst, pitch, rt->h);
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
            pack16(rt, row, o, w);
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

/* For tests/nv2a_backend_smoke: blits copied on the GPU and in guest memory,
 * and the live targets. */
void nv2a_pb_metal_blit_stats(uint64_t *gpu, uint64_t *guest, unsigned *targets)
{
    int i;
    *gpu = s_blits_gpu;
    *guest = s_blits_guest;
    *targets = 0;
    for (i = 0; i < RT_MAX; i++)
        *targets += s_rt[i].tex != nil;
}

/* For tests/nv2a_backend_smoke: the target at a colour offset as the Hor+
 * copy rule sees it: -1 none, 0 not a blit copy (or drawn into since), 1
 * a copy a blit filled this flip, 2 one from an earlier flip. */
int nv2a_pb_metal_copy_state(uint32_t offset)
{
    int i;
    for (i = 0; i < RT_MAX; i++)
        if (s_rt[i].tex && s_rt[i].offset == offset)
            return !s_rt[i].copy ? 0 : s_rt[i].copy_flip == (uint32_t)s_presents ? 1 : 2;
    return -1;
}

/* For tests/nv2a_backend_smoke: the MSL version the runtime shaders
 * compile at (major << 16 | minor), as the compile options report it: with
 * metal_msl_version=0 that is the OS default (0x40001 on macOS 27). 0 before
 * init and before macOS 13, where the options have no version. */
unsigned nv2a_pb_metal_msl_version(void)
{
    if (!s_dev)
        return 0;
    if (@available(macOS 13.0, iOS 16.0, *))
        return (unsigned)metal_compile_options().languageVersion;
    return 0;
}

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
                    p_set, p_stale, p_first, p_cup;
    static int zt = -1;
    if (sh_trace() && (s_sh_flip.lib || s_sh_flip.pso || s_sh_flip.tex_ns >= 5000000))
        fprintf(stderr, LOGP "build flip %llu: %u libraries %.1f ms, %u pipelines %.1f ms,"
                " %u texture uploads %.1f ms\n", (unsigned long long)s_presents,
                s_sh_flip.lib, s_sh_flip.lib_ns / 1e6, s_sh_flip.pso, s_sh_flip.pso_ns / 1e6,
                s_sh_flip.tex, s_sh_flip.tex_ns / 1e6);
    memset(&s_sh_flip, 0, sizeof s_sh_flip);
    mag_flip();
    nv2a_tex_dump_flip(s_presents);
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
            " flushes %llu consts %llu occ %llu (visible %llu, pending %d, late %llu, superseded %llu,"
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
            (unsigned long long)(s_c_uploads - p_cup),
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
    p_fl = s_flushes; p_cup = s_c_uploads;
    if (rt && rt->pad && aspect_trace()) {
        static uint64_t p_stretch;
        fprintf(stderr, LOGP "flip %llu hor+: target %ux%u pad %u (%ux%u host),"
                " %llu batches off-centre\n", (unsigned long long)s_presents,
                rt->w, rt->h, rt->pad, rt->hw, rt->hh,
                (unsigned long long)(s_draws_stretch - p_stretch));
        p_stretch = s_draws_stretch;
    }
}

static void prof_flip_end(uint64_t t_flip0)
{
    static uint64_t p_wb, p_ds, p_pa;
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
                " over %.1f cmd buffers/flip; %llu write-backs, %llu decode syncs,"
                " %llu pitch-alias binds; rt ownership hashes %.3f ms/flip\n",
                (unsigned long long)s_presents,
                s_pa.flip / n / 1e6, s_pa.wait / n / 1e6, s_pa.hostrb / n / 1e6,
                s_pa.wb / n / 1e6, s_pa.winrb / n / 1e6, s_pa.winpush / n / 1e6,
                s_pa.blit / n / 1e6, s_pa.gpu / n / 1e6, s_pa.ncmd / n,
                (unsigned long long)(s_writebacks - p_wb),
                (unsigned long long)(s_decode_syncs - p_ds),
                (unsigned long long)(s_pitch_alias_binds - p_pa), s_pa.hash / n / 1e6);
        p_wb = s_writebacks;
        p_ds = s_decode_syncs;
        p_pa = s_pitch_alias_binds;
        memset(&s_pa, 0, sizeof s_pa);
    }
}

/* RECOMP_DEBUG=metal_host_dump=<prefix>: the frame as drawn, host size,
 * every fb_dump_flips flip (20), <prefix>host_NNNNN.bmp by present count. A
 * guest-size dump (fb_dump) shows only the centre of a padded target. */
static int host_dump_due(void)
{
    static const char *prefix;
    static int every = -1;
    if (every < 0) {
        const char *e = recomp_env(RENV_FB_DUMP_FLIPS);
        prefix = recomp_env(RENV_METAL_HOST_DUMP);
        every = e ? atoi(e) : 0;
        if (every <= 0)
            every = 20;
    }
    return prefix && ((s_presents + 1) % (uint64_t)every) == 0;
}

static void host_dump(RenderTarget *rt, uint32_t hcw, uint32_t hch)
{
    const uint8_t *px = host_readback(rt);
    char path[1024];
    uint8_t hdr[54];
    uint32_t row = hcw * 3u, padb = (4u - (row & 3u)) & 3u, size, y, x;
    FILE *f;

    if (!px || !hcw || !hch)
        return;
    snprintf(path, sizeof path, "%shost_%05llu.bmp", recomp_env(RENV_METAL_HOST_DUMP),
             (unsigned long long)(s_presents + 1));
    f = fopen(path, "wb");
    if (!f)
        return;
    size = 54u + (row + padb) * hch;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &hcw, 4);
    memcpy(hdr + 22, &hch, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = hch; y-- > 0; ) {
        const uint8_t *s = px + (size_t)y * rt->hw * 4u;
        for (x = 0; x < hcw; x++)
            fwrite(s + (size_t)x * 4u, 1, 3, f);
        if (padb)
            fwrite(hdr + 1, 1, padb, f);   /* zero bytes of the header */
    }
    fclose(f);
}

#ifdef XBOXRECOMP_ENHANCE
/* ── The mods' 3D debug draw (nv2a_dbg_draw.h, mod API version 8) ─────────
 *
 * As the D3D11 backend's: at the flip, into the target about to be shown,
 * before the sync, the readbacks and the dumps, in a loading pass on the
 * target that attaches its own depth buffer (rt->dt), whose z is the
 * frame's normalised z, the clip z / w of the game's projection. The
 * vertices are clip space already; the shader only narrows x to the
 * guest's columns of a hor+ target. The vertex buffer is reused: the
 * flip's flush(1) waits for the pass that read it. */
static const char s_dbg_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct DV { packed_float4 p; uchar4 c; };\n"
    "struct VO { float4 p [[position]]; float4 c; };\n"
    "vertex VO dbg_vs(uint id [[vertex_id]], device const DV *v [[buffer(0)]],\n"
    "                 constant float &xs [[buffer(1)]]) {\n"
    "    VO o;\n"
    "    float4 p = float4(v[id].p);\n"
    "    o.p = float4(p.x * xs, p.yzw);\n"
    "    o.c = float4(v[id].c) / 255.0;\n"
    "    return o;\n"
    "}\n"
    "fragment float4 dbg_fs(VO i [[stage_in]]) { return i.c; }\n";

static id<MTLRenderPipelineState> s_dbg_pso[2];     /* without, with a depth attachment */
static id<MTLDepthStencilState> s_dbg_dss[3];       /* test and write, test, off */
static id<MTLBuffer> s_dbg_vb;
static size_t s_dbg_vb_cap;
static int s_dbg_failed;

static int dbg_init(void)
{
    id<MTLFunction> vs, fs;
    int k;

    /* s_dbg_failed, not s_dbg_pso[0], says whether the whole pipeline is
     * there: a depth state can fail after both pipelines were made. */
    if (s_dbg_pso[0] || s_dbg_failed)
        return !s_dbg_failed;
    s_dbg_failed = 1;
    vs = compile_fn(s_dbg_msl, @"dbg_vs", "nv2a_dbg_vs");
    fs = compile_fn(s_dbg_msl, @"dbg_fs", "nv2a_dbg_fs");
    if (!vs || !fs)
        return 0;
    for (k = 0; k < 2; k++) {
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
        NSError *err = nil;
        pd.vertexFunction = vs;
        pd.fragmentFunction = fs;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        /* Straight alpha: the mods' colours are 0xAARRGGBB as written. */
        pd.colorAttachments[0].blendingEnabled = YES;
        pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        if (k) {
            pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        }
        s_dbg_pso[k] = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        [pd release];
        if (!s_dbg_pso[k]) {
            fprintf(stderr, LOGP "debug draw pipeline failed (%s); no 3D debug drawing\n",
                    err ? [[err localizedDescription] UTF8String] : "?");
            if (s_dbg_pso[0]) {
                [s_dbg_pso[0] release];
                s_dbg_pso[0] = nil;
            }
            return 0;
        }
    }
    for (k = 0; k < 3; k++) {
        MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
        dd.depthCompareFunction = k < 2 ? MTLCompareFunctionLessEqual : MTLCompareFunctionAlways;
        dd.depthWriteEnabled = k == 0 ? YES : NO;
        s_dbg_dss[k] = [s_dev newDepthStencilStateWithDescriptor:dd];
        [dd release];
        if (!s_dbg_dss[k]) {
            fprintf(stderr, LOGP "debug draw depth state failed; no 3D debug drawing\n");
            return 0;
        }
    }
    s_dbg_failed = 0;
    return 1;
}

static void dbg_present(RenderTarget *rt)
{
    const struct nv2a_dbg_list *l = nv2a_dbg_take();
    const size_t stride = sizeof(struct nv2a_dbg_vtx);
    uint32_t first[NV2A_DBG_SEGS], at = 0;
    float xs;
    int s, z;

    if (!l || !l->total || !rt || !rt->tex || !dbg_init())
        return;
    for (s = 0; s < NV2A_DBG_SEGS; s++) {
        first[s] = at;
        at += l->n[s];
    }
    if ((size_t)at * stride > s_dbg_vb_cap) {
        size_t c = s_dbg_vb_cap ? s_dbg_vb_cap : 65536;
        while (c < (size_t)at * stride)
            c *= 2;
        [s_dbg_vb release];
        s_dbg_vb = [s_dev newBufferWithLength:c options:MTLResourceStorageModeShared];
        s_dbg_vb_cap = s_dbg_vb ? c : 0;
        if (!s_dbg_vb)
            return;
    }
    for (s = 0; s < NV2A_DBG_SEGS; s++)
        if (l->n[s])
            memcpy((uint8_t *)[s_dbg_vb contents] + (size_t)first[s] * stride, l->v[s],
                   (size_t)l->n[s] * stride);
    begin_pass(rt, 0, NULL, 0, 1.0f, 0);
    if (!s_enc)
        return;
    z = s_enc_dt != NULL;
    xs = nv2a_dbg_xscale(rt->w, rt->pad);
    {
        MTLViewport vp = {0.0, 0.0, (double)rt->hw, (double)rt->hh, 0.0, 1.0};
        [s_enc setRenderPipelineState:s_dbg_pso[z]];
        [s_enc setViewport:vp];
        [s_enc setCullMode:MTLCullModeNone];
        [s_enc setVertexBuffer:s_dbg_vb offset:0 atIndex:0];
        [s_enc setVertexBytes:&xs length:sizeof xs atIndex:1];
    }
    for (s = 0; s < NV2A_DBG_SEGS; s++) {
        int xray = s == NV2A_DBG_TRI_XRAY || s == NV2A_DBG_LINE_XRAY;
        int line = s == NV2A_DBG_LINE || s == NV2A_DBG_LINE_XRAY;
        if (!l->n[s] || (!xray && !z))
            continue;
        if (z)
            [s_enc setDepthStencilState:s_dbg_dss[xray ? 2 : s == NV2A_DBG_TRI_OPAQUE ? 0 : 1]];
        [s_enc drawPrimitives:line ? MTLPrimitiveTypeLine : MTLPrimitiveTypeTriangle
                  vertexStart:first[s]
                  vertexCount:l->n[s]];
    }
    end_encoder();
}
#endif /* XBOXRECOMP_ENHANCE */

static void metal_flip(uint32_t surface_offset, uint32_t pitch)
{
    RenderTarget *rt = NULL;
    uint32_t addr = surface_offset ? nv2a_pb_dma_resolve(surface_offset) : 0;
    const uint8_t *hpx = NULL;   /* a scaled rt's host pixels (host_readback) */
    uint64_t t_flip0 = prof_on() ? prof_now() : 0, t;
    int always = wb_always(), win = 0, layer = 0, hdump = 0;
    uint32_t cw = 0, ch = 0, hcw = 0, hch = 0;   /* the frame, guest and host */

    guard_open();
    if (init()) {
        @autoreleasepool {
            int wb;
            id<MTLTexture> slot = nil;
            rt = present_target(surface_offset);
            if (rt) {
                /* The frame's target size, for a HUD canvas on "frame". */
                nv2a_aspect_frame_publish(rt->w, rt->h);
                present_extent(rt, &cw, &ch);
                hcw = (cw + 2u * rt->pad) * rt->n;
                hch = ch * rt->n;
                s_shown_w = cw;
                s_shown_h = ch;
#ifdef XBOXRECOMP_ENHANCE
                dbg_present(rt);
#endif
            }
            win = rt && xbox_FramebufferWindowRunning();
            /* Layer mode: the frame goes to the window as a GPU copy into the
             * presenter's back slot (fb_present_metal.m), below. */
            layer = win && s_layer;
            wb = rt && addr && pitch && pitch >= rt->w * 2u && always;
            hdump = rt && host_dump_due();
            if (rt && (wb || hdump || (win && !layer)))
                rt_managed_sync(rt);
            t = prof_t();
            flush(1);
            occ_settle(1);
            prof_add(&s_pa.wait, t);
            if (hdump)
                host_dump(rt, hcw, hch);
            if (layer) {
                /* After the wait, in a command buffer of its own that nobody
                 * waits for: the flip's wait never includes the copy, nor a
                 * present pass still reading the slot (tracked hazards order
                 * the two on the GPU). Published once committed, so the main
                 * thread's pass that shows it is committed after it. */
                t = prof_t();
                slot = (id<MTLTexture>)xbox_FramebufferWindowBackTexture(hcw, hch, rt->n,
                                                                         rt->pad);
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
    /* The [MEM] exit line's GPU figure: every allocation this device made
     * for the process (the presenter's too, on the shared device). Unified
     * memory has no separate non-local pool. */
    if (s_dev && (s_presents == 1 || s_presents % 60 == 0))
        mem_stats_gpu_note("metal", (uint64_t)[s_dev currentAllocatedSize], MEM_STATS_NA);
    flip_log(surface_offset, rt);
    if (s_presents == 1 || (s_presents % 300) == 0)
        fprintf(stderr, LOGP "present #%llu: surface 0x%08X (%s), %llu clears,"
                " %llu draws (%llu vertex-program), %llu skipped, %llu culled,"
                " %llu texture uploads, %llu rehashes, %llu write-backs (%s),"
                " %llu decode syncs, %llu pitch-alias binds, %llu stale drops,"
                " %llu evictions lost, %llu grows, %llu partial clears;"
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
                (unsigned long long)s_decode_syncs, (unsigned long long)s_pitch_alias_binds,
                (unsigned long long)s_stale_drops, (unsigned long long)s_evict_lost,
                (unsigned long long)s_grows,
                (unsigned long long)s_partial_clears,
                (unsigned long long)s_vs_compiled, (unsigned long long)s_fs_compiled,
                (unsigned long long)s_compile_errors,
                (unsigned long long)s_zclears, (unsigned long long)s_draws_z,
                (unsigned long long)s_rtt_binds, (unsigned long long)s_self_copies,
                (unsigned long long)s_window_presents,
                s_layer ? "CAMetalLayer slots" : "texture readback",
                (unsigned long long)s_flushes, (unsigned long long)s_report_commits);
    if ((s_presents == 1 || (s_presents % 300) == 0) && (s_blits_gpu || s_blits_guest))
        fprintf(stderr, LOGP "present #%llu: 2D blits %llu target copies, %llu in guest"
                " memory\n", (unsigned long long)s_presents, (unsigned long long)s_blits_gpu,
                (unsigned long long)s_blits_guest);
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

/* render.scale changed at this flip: every target and depth buffer was made
 * at the old factor. Targets go with the eviction's write-back, so the next
 * one at that surface seeds from what was drawn at the new scale. After the
 * present, so the frame being shown keeps the targets it was drawn in.
 *
 * The texture cache stays. Its entries are guest texels at guest size, which
 * no scale changes; a render-to-texture input binds the target itself
 * (rt_texture, s_self resized by its own check), and the write-back marks
 * every entry over a target's memory stale (tex_cache_stale). Dropping it
 * made the next flip decode and upload every texture again: 180-550 ms on
 * Jet Set Radio Future's title screen, the freeze seen at each change. */
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
    fprintf(stderr, LOGP "render.scale=%u from this flip: targets dropped\n",
            nv2a_host_render_scale());
}

/* Live options (nv2a_host_opts_request) are applied after the present:
 * the one point where no target is bound and no upload is in flight, and
 * the frame shown was drawn at the old render.scale, which the present
 * sizes its source by (applied first, a 2x frame lowered to 1x would show
 * its top-left quarter for one flip). */
/* Filter, fullscreen and overscan are the SDL presenter's (fb_present_sdl.c
 * reads them each present, on its own thread); aniso and movie are read at
 * each draw, so storing them is the change. */
static void metal_on_flip(uint32_t surface_offset, uint32_t pitch)
{
    unsigned changed;
    metal_flip(surface_offset, pitch);
    changed = nv2a_host_opts_apply();
    if (changed & NV2A_OPT_SCALE)
        scale_drop();
}

/* ---- the 2D engine ---------------------------------------------------------- */

/* nv2a_pb_backend.on_blit. A blit out of a render target copies the
 * target's texture, pads and render scale included, into a target at the
 * destination (the "twin": the source's geometry, made on first use, kept
 * while each blit refills it), which a later draw samples like any target
 * (rt_texture). The guest bytes under the source are stale while it is
 * drawn into, so a guest copy would hand the next pass an old frame; and
 * under Hor+ the wings exist only on the GPU. Quantum Redshift copies its
 * 640x480 R5G6B5 back buffer each frame and refracts the copy through its
 * lens raindrops, which drew black with no copy at all. Anything
 * else (a guest-memory source, a partial copy with no twin to land in,
 * BLEND_AND) runs in guest memory: the source is written back first, the
 * targets over the destination are written back and retired, and the
 * texture cache rehashes the destination. */

static RenderTarget *blit_source(const struct nv2a_pb_blit *b)
{
    int i;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->tex && !((rt->addr ^ b->src) & 0x07FFFFFFu) && !rt_drop_if_stale(rt, "blit"))
            return rt;
    }
    return NULL;
}

static int blit_gpu(const struct nv2a_pb_blit *b, RenderTarget *src)
{
    RenderTarget *dst = NULL, *free_rt = NULL, *lru = NULL;
    id<MTLBlitCommandEncoder> e;
    uint32_t box[6];
    int i, whole;

    /* The source rectangle names the target's pixels only at its pitch. The
     * destination may have another (a copy into a wider surface): the twin
     * takes the destination's pitch, so its guest span and any write-back
     * are the blit's. */
    if (b->operation != NV09F_OP_SRCCOPY || b->src_pitch != src->pitch
            || b->dst_pitch < src->w * b->bpp || b->bpp != nv2a_surf_color_bytes(src->fmt)
            || nv2a_rt_overlap(b->src, src->pitch * src->h, b->dst, b->dst_pitch * src->h)
            || !nv2a_2d_host_box(b, src->w, src->h, src->pad, src->n, src->hw, src->hh, box))
        return 0;
    whole = !b->in_x && !b->in_y && !b->out_x && !b->out_y && b->w == src->w && b->h == src->h;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->tex && !((rt->addr ^ b->dst) & 0x07FFFFFFu) && rt->w == src->w
                && rt->h == src->h && rt->hw == src->hw && rt->hh == src->hh
                && rt->pad == src->pad && rt->n == src->n && rt->pitch == b->dst_pitch
                && rt->fmt == src->fmt && !rt_drop_if_stale(rt, "blit")) {
            dst = rt;
            break;
        }
    }
    /* A partial copy lands in what the destination held: with no twin to
     * hold that, guest memory has it. */
    if (!dst && !whole)
        return 0;
    /* No two live targets share memory (surface()'s rule): what else lies
     * over the destination is written back, then retired. */
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt == dst || rt == src || !rt->tex
                || !nv2a_rt_overlap(rt->addr, rt->pitch * rt->h, b->dst, b->dst_pitch * src->h))
            continue;
        if (!rt_drop_if_stale(rt, "overlap")) {
            rt_sync(rt, rt->pitch);
            rt_free(rt);
        }
    }
    if (!dst) {
        for (i = 0; i < RT_MAX; i++) {
            RenderTarget *rt = &s_rt[i];
            if (!rt->tex && !free_rt)
                free_rt = rt;
            if (rt != src && (!lru || rt->last_use < lru->last_use))
                lru = rt;
        }
        if (!free_rt) {
            free_rt = lru;
            if (!rt_drop_if_stale(free_rt, "evict")) {
                rt_sync(free_rt, free_rt->pitch);
                rt_free(free_rt);
            }
        }
        free_rt->tex = rt_texture_new(src->hw, src->hh);
        if (!free_rt->tex)
            return 0;
        dst = free_rt;
        dst->offset = b->dst_offset;
        dst->addr = b->dst;
        dst->w = src->w;
        dst->h = src->h;
        dst->hw = src->hw;
        dst->hh = src->hh;
        dst->pad = src->pad;
        dst->n = src->n;
        dst->pitch = b->dst_pitch;
        dst->fmt = src->fmt;
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target (%ux%u host), the 2D"
                " blit copy of 0x%08X\n", dst->offset, dst->w, dst->h, dst->hw, dst->hh,
                src->offset);
    }
    end_encoder();
    ensure_cmd();
    e = [s_cmd blitCommandEncoder];
    [e copyFromTexture:src->tex sourceSlice:0 sourceLevel:0
          sourceOrigin:MTLOriginMake(box[0], box[1], 0)
            sourceSize:MTLSizeMake(box[4], box[5], 1)
             toTexture:dst->tex destinationSlice:0 destinationLevel:0
     destinationOrigin:MTLOriginMake(box[2], box[3], 0)];
    [e endEncoding];
    s_tick++;
    dst->last_use = s_tick;
    dst->dirty = 1;   /* the copy is on the GPU only: an eviction writes it back */
    dst->copy = 1;
    dst->copy_flip = (uint32_t)s_presents;
    rt_own_reset(dst);
    return 1;
}

static void metal_on_blit(const struct nv2a_pb_blit *b)
{
    RenderTarget *src;
    uint64_t span = (uint64_t)(b->out_y + b->h) * b->dst_pitch;
    int i;

    guard_open();
    if (!init()) {
        nv2a_pb_blit_guest(b);
        return;
    }
    @autoreleasepool {
        src = blit_source(b);
        if (src && blit_gpu(b, src)) {
            s_blits_gpu++;
            return;
        }
        if (src)
            rt_sync(src, src->pitch);
        for (i = 0; i < RT_MAX; i++) {
            RenderTarget *rt = &s_rt[i];
            if (rt == src || !rt->tex
                    || !nv2a_rt_overlap(rt->addr, rt->pitch * rt->h, b->dst, (uint32_t)span))
                continue;
            if (!rt_drop_if_stale(rt, "overlap")) {
                rt_sync(rt, rt->pitch);   /* a partial blit keeps the rest */
                rt_free(rt);
            }
        }
        nv2a_pb_blit_guest(b);
        tex_cache_stale(b->dst, (uint32_t)span);
        s_blits_guest++;
        {
            static int said;
            if (!said++)
                fprintf(stderr, LOGP "2D blit 0x%08X -> 0x%08X %ux%u in guest memory%s\n",
                        b->src_offset, b->dst_offset, b->w, b->h,
                        src ? " (the source target written back)" : "");
        }
    }
}

/* ---- registration ---------------------------------------------------------- */

/* Under Hor+ or render.snap_2d, at a batch's END: whether the walker may
 * hold it for a merged list (nv2a_merge_placed), as d3d11_merge_placed.
 * The program and stages are the ones its draw would look up (both
 * memoised); a stage counts as reading a target when its offset is one,
 * whatever rt_texture would then make of the format. */
static int metal_merge_placed(int program)
{
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    const VsEntry *ve;
    int n, i, rt = 0;

    if (!program || !v->depth_enable || !init())
        return 0;
    @autoreleasepool {
        ve = vs_cache();
    }
    if (!ve)
        return 0;
    build_stages();
    for (n = 0; n < 4 && !rt; n++) {
        const Stage *t = &s_stage[n];
        if (!t->mode || t->mode == 4 || !t->offset)
            continue;
        for (i = 0; i < RT_MAX && !rt; i++)
            rt = s_rt[i].tex && !((s_rt[i].addr ^ t->offset) & 0x07FFFFFFu);
    }
    return nv2a_merge_placed(program, v->depth_enable, &ve->hp, rt);
}

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
    1,                  /* merge_draws */
    metal_merge_placed,
    metal_on_blit,
};

const struct nv2a_pb_backend *nv2a_pb_metal_backend(void)
{
    if (!s_black && init()) {
        @autoreleasepool {
            s_black = solid_texture(0xFF000000u);
            s_magenta = solid_texture(NV2A_TEX_MAGENTA);
            s_black_cube = solid_cube(0xFF000000u);
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
