/**
 * The SDL window's CAMetalLayer present (macOS): the zero-copy half of
 * fb_present_sdl.c, used when the Metal backend draws (RECOMP_PB_BACKEND=metal,
 * not RECOMP_DEBUG=metal_present=readback).
 *
 * The window has no SDL_Renderer then. fb_present_sdl.c keeps the window, the
 * event loop and the three-slot hand-off; this file owns what the slots point
 * at and how a slot reaches the screen:
 *
 *   device, queue  made here at window creation, before the guest starts; the
 *                  Metal backend adopts both at its lazy init
 *                  (xbox_FramebufferWindowMetal), so a slot texture takes the
 *                  backend's blit and a present pass on the same queue runs
 *                  after the blit it shows (commit order, tracked hazards).
 *   slots          three BGRA8 textures, frame-sized (render.scale's host
 *                  size). The backend blits its present target into the back
 *                  slot on the GPU; a guest-bytes frame is uploaded into it.
 *   present        on the main thread: the layer's next drawable, cleared to
 *                  black, the slot drawn into nv2a_present_rect's rectangle
 *                  with one full-screen triangle, nearest or linear as the
 *                  SDL path filters (and as D3D11's scaling blit does).
 *   shot           RECOMP_WINDOW_SHOT: the drawable read back after the pass,
 *                  framebufferOnly being off only when shots are asked for.
 *   internal       present.surface=internal: the drawable stays the frame's
 *                  size, the slot is drawn into it 1:1, and the layer scales
 *                  it to the view (aspect fit, the present filter's sampler),
 *                  as gamescope scales D3D11's frame-sized swap chain.
 *   overscan       display.overscan > 0: a second pipeline (vs_crop) samples
 *                  nv2a_present_crop's part of the slot. At 0 the stock
 *                  pipeline draws, so nothing changes.
 *   overlay        enhancements builds: the lists fb_present_sdl.c took from
 *                  nv2a_overlay (the SDL loop stays the overlay's one reader)
 *                  drawn in the same pass after the frame, premultiplied, so
 *                  the drawable shot has them and the Metal frame dump (the
 *                  backend's render target) never does.
 *   shader         present.shader (enhancements builds): librashader's Metal
 *                  filter chain on this queue draws the slot into the same
 *                  rectangle in place of the triangle; the overlay follows
 *                  in a second pass. Built on the main thread when the
 *                  request's generation moves; "none" never loads it.
 *
 * Manual retain/release, like nv2a_pb_metal.m. Every entry point is wrapped in
 * an autorelease pool: the main loop and the ack thread have none of their own.
 */
#if defined(__APPLE__)

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kernel/nv2a_backend_common.h"
#ifdef XBOXRECOMP_ENHANCE
#include "kernel/nv2a_overlay.h"
#include "kernel/nv2a_present_chain.h"
#define LIBRA_RUNTIME_METAL
#include "librashader.h"
#endif
#include "fb_present_metal.h"
#include "metal_compile.h"

#if __has_feature(objc_arc)
#error "fb_present_metal.m is manual retain/release: build it without -fobjc-arc"
#endif

static id<MTLDevice>               s_dev;
static id<MTLCommandQueue>         s_queue;
static id<MTLRenderPipelineState>  s_pso;
static id<MTLRenderPipelineState>  s_pso_crop; /* display.overscan > 0 */
static id<MTLSamplerState>         s_smp[2];   /* nearest, linear */
static SDL_MetalView               s_view;
static CAMetalLayer               *s_layer;
static int                         s_unified;
static id<MTLTexture>              s_slot[3];
/* The last present pass that sampled each slot: an upload (a CPU write) into
 * a slot waits for it; a GPU blit needs no wait (tracked hazards). Written by
 * the main thread while the slot is front, read by the ack thread once the
 * hand-off's lock has made it back. */
static id<MTLCommandBuffer>        s_slot_cb[3];
static uint32_t                   *s_conv;     /* R5G6B5 -> BGRA8 rows */
static size_t                      s_conv_cap;
static int                         s_internal;  /* present.surface=internal */
static int                         s_layer_filter = -1;
#ifdef XBOXRECOMP_ENHANCE
static id<MTLRenderPipelineState>  s_pso_lines; /* present.shader: guest-line resolve */
static id<MTLRenderPipelineState>  s_pso_ovl;  /* the overlay, premultiplied */
static id<MTLTexture>              s_ovl_tex[1 + NV2A_OVL_IMAGES];
static uint64_t                    s_ovl_tex_gen[1 + NV2A_OVL_IMAGES];
/* Each layer's vertices and indices, copied in only when its gen moves.
 * Three sets a layer, so a new list never overwrites one a frame still in
 * flight reads; the last command buffer that used a set is waited on
 * before the set is written again. */
#define OVL_SETS 3
static struct ovl_set {
    id<MTLBuffer> vb, ib;
    NSUInteger vcap, icap;
    uint64_t gen;
    id<MTLCommandBuffer> cb;
} s_ovl_set[NV2A_OVL_COUNT][OVL_SETS];
static int s_ovl_cur[NV2A_OVL_COUNT];
#endif

static const char s_blit_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VO { float4 p [[position]]; float2 t; };\n"
    "static VO tri(uint i) {\n"
    "    VO o;\n"
    "    o.t = float2((i << 1) & 2, i & 2);\n"
    "    o.p = float4(o.t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "vertex VO vs_blit(uint i [[vertex_id]]) { return tri(i); }\n"
    "vertex VO vs_crop(uint i [[vertex_id]], constant float4 &crop [[buffer(0)]]) {\n"
    "    VO o = tri(i);\n"
    "    o.t = o.t * crop.xy + crop.zw;\n"
    "    return o;\n"
    "}\n"
    "fragment float4 fs_blit(VO i [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "                        sampler s [[sampler(0)]]) {\n"
    "    return float4(t.sample(s, i.t).rgb, 1);\n"
    "}\n"
    /* present.shader's resolve to the guest's lines: each row the mean of
     * its n host rows, at the whole host width (D3D11's s_lrs_res_src). */
    "fragment float4 fs_lines(VO i [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "                         constant uint &n [[buffer(0)]]) {\n"
    "    uint2 q = uint2(i.p.xy);\n"
    "    float3 c = 0;\n"
    "    for (uint k = 0; k < n; k++)\n"
    "        c += t.read(uint2(q.x, q.y * n + k)).rgb;\n"
    "    return float4(c / float(n), 1);\n"
    "}\n"
    /* The overlay: a list's vertices and indices as they are (20-byte
     * vertices, uint16 indices), each index read by vertex_id, so a command
     * is drawPrimitives over its index range. xf maps the list's output
     * pixels into the placement in clip space. */
    "struct OV { packed_float2 p; packed_float2 t; uint c; };\n"
    "struct OO { float4 p [[position]]; float2 t; float4 c; };\n"
    "vertex OO vs_ovl(uint i [[vertex_id]], const device OV *v [[buffer(0)]],\n"
    "                 const device ushort *ix [[buffer(1)]],\n"
    "                 constant float4 &xf [[buffer(2)]]) {\n"
    "    OV a = v[ix[i]];\n"
    "    OO o;\n"
    "    o.p = float4(float2(a.p) * xf.xy + xf.zw, 0, 1);\n"
    "    o.t = float2(a.t);\n"
    "    o.c = unpack_unorm4x8_to_float(a.c);\n"
    "    return o;\n"
    "}\n"
    "fragment float4 fs_ovl(OO i [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "                       sampler s [[sampler(0)]]) {\n"
    "    return t.sample(s, i.t) * i.c;\n"
    "}\n";

static int fail(char *err, size_t n, const char *what)
{
    snprintf(err, n, "%s", what);
    return 0;
}

int fbm_create(SDL_Window *win, int vsync, int shots, char *err, size_t errn)
{
    NSError *e = nil;
    id<MTLLibrary> lib;
    MTLRenderPipelineDescriptor *pd;
    MTLSamplerDescriptor *sd;
    int w = 0, h = 0, k;

    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev)
            return fail(err, errn, "no Metal device");
        s_queue = [s_dev newCommandQueue];
        if (!s_queue)
            return fail(err, errn, "no command queue");
        s_unified = [s_dev hasUnifiedMemory] ? 1 : 0;
        /* The backend's options (metal_compile.h): one language version and
         * math mode per process, and fs_blit's texture arguments compile in
         * milliseconds at the pin instead of about 100 ms more. */
        lib = metal_compile_library(s_dev, [NSString stringWithUTF8String:s_blit_msl],
                                    "the present library", &e);
        if (!lib)
            return fail(err, errn, "present shader failed to compile");
        pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = [[lib newFunctionWithName:@"vs_blit"] autorelease];
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_blit"] autorelease];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        s_pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        pd.vertexFunction = [[lib newFunctionWithName:@"vs_crop"] autorelease];
        s_pso_crop = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
#ifdef XBOXRECOMP_ENHANCE
        pd.vertexFunction = [[lib newFunctionWithName:@"vs_blit"] autorelease];
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_lines"] autorelease];
        s_pso_lines = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!s_pso_lines)
            fprintf(stderr, "[SHADER] the guest-line resolve failed; the chain reads the host rows\n");
        pd.vertexFunction = [[lib newFunctionWithName:@"vs_ovl"] autorelease];
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_ovl"] autorelease];
        pd.colorAttachments[0].blendingEnabled = YES;
        pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        s_pso_ovl = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!s_pso_ovl)
            fprintf(stderr, "[PRESENT] overlay pipeline failed; the layer draws no overlay\n");
#endif
        [pd release];
        [lib release];
        if (!s_pso || !s_pso_crop)
            return fail(err, errn, "present pipeline failed");
        for (k = 0; k < 2; k++) {
            sd = [[MTLSamplerDescriptor alloc] init];
            sd.minFilter = sd.magFilter = k ? MTLSamplerMinMagFilterLinear
                                             : MTLSamplerMinMagFilterNearest;
            sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
            s_smp[k] = [s_dev newSamplerStateWithDescriptor:sd];
            [sd release];
        }

        s_view = SDL_Metal_CreateView(win);
        if (!s_view)
            return fail(err, errn, SDL_GetError());
        s_layer = (CAMetalLayer *)SDL_Metal_GetLayer(s_view);
        if (!s_layer)
            return fail(err, errn, "SDL_Metal_GetLayer returned NULL");
        [s_layer retain];
        s_layer.device = s_dev;
        s_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        /* The SDL renderer this replaces draws SDR content as sRGB. */
        {
            CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            s_layer.colorspace = cs;
            CGColorSpaceRelease(cs);
        }
        s_layer.framebufferOnly = shots ? NO : YES;
        s_layer.displaySyncEnabled = vsync ? YES : NO;
        s_layer.maximumDrawableCount = 3;
        s_layer.allowsNextDrawableTimeout = YES;
        s_internal = nv2a_host_opts()->present_surface == NV2A_PRESENT_SURFACE_INTERNAL;
        if (s_internal) {
            /* Letterboxed like nv2a_present_rect's fit; fbm_present keeps
             * the drawable at the frame's size. */
            s_layer.contentsGravity = kCAGravityResizeAspect;
            w = (int)nv2a_host_window_w(640, 480, nv2a_host_render_scale());
            h = 480 * (int)nv2a_host_render_scale();
        } else {
            SDL_GetWindowSizeInPixels(win, &w, &h);
        }
        if (w > 0 && h > 0)
            s_layer.drawableSize = CGSizeMake(w, h);
    }
#ifdef XBOXRECOMP_ENHANCE
    /* Every backend presents through here on macOS, the CPU rasteriser's
     * frames too, so the menu's Shaders page is shown for all of them. */
    nv2a_present_chain_host("Metal");
#endif
    return 1;
}

/* present.surface=internal: the layer's scaling filter follows the present
 * filter (integer has no layer equivalent and scales linearly, as its
 * non-whole fallback does). */
static void layer_filter(int filter)
{
    NSString *f;
    if (filter == s_layer_filter)
        return;
    s_layer_filter = filter;
    f = filter == NV2A_PRESENT_NEAREST ? kCAFilterNearest : kCAFilterLinear;
    s_layer.magnificationFilter = f;
    s_layer.minificationFilter = f;
}

#ifdef XBOXRECOMP_ENHANCE
static void chain_free(void);
#endif

void fbm_destroy(void)
{
    int k;
    @autoreleasepool {
        for (k = 0; k < 3; k++) {
            if (s_slot_cb[k])
                [s_slot_cb[k] waitUntilCompleted];
            [s_slot_cb[k] release];
            s_slot_cb[k] = nil;
            [s_slot[k] release];
            s_slot[k] = nil;
        }
        [s_layer release];
        s_layer = nil;
        if (s_view)
            SDL_Metal_DestroyView(s_view);
        s_view = NULL;
        [s_pso release];
        s_pso = nil;
        [s_pso_crop release];
        s_pso_crop = nil;
#ifdef XBOXRECOMP_ENHANCE
        chain_free();
        [s_pso_lines release];
        s_pso_lines = nil;
        [s_pso_ovl release];
        s_pso_ovl = nil;
        for (k = 0; k <= NV2A_OVL_IMAGES; k++) {
            [s_ovl_tex[k] release];
            s_ovl_tex[k] = nil;
        }
        for (k = 0; k < NV2A_OVL_COUNT * OVL_SETS; k++) {
            struct ovl_set *o = &s_ovl_set[k / OVL_SETS][k % OVL_SETS];
            [o->vb release];
            [o->ib release];
            [o->cb release];
            memset(o, 0, sizeof *o);
        }
#endif
        [s_smp[0] release];
        [s_smp[1] release];
        s_smp[0] = s_smp[1] = nil;
        /* The device and queue stay: the backend holds its own references
         * and the process is about to exit. On the fallback path (setup
         * failed, the window goes back to the SDL renderer) that leaks the
         * device and queue made here for the run: the backend has not
         * started yet and creates its own. */
    }
}

void fbm_get(void **device, void **queue)
{
    *device = (void *)s_dev;
    *queue = (void *)s_queue;
}

/* Slot k at w x h, made or re-made at that size. Releasing the old texture
 * while a present pass reads it is fine: a committed command buffer retains
 * what it uses. */
void *fbm_slot(int k, uint32_t w, uint32_t h)
{
    @autoreleasepool {
        if (!s_slot[k] || [s_slot[k] width] != w || [s_slot[k] height] != h) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                             width:w height:h mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            /* Shared: an upload needs replaceRegion, which a private texture
             * refuses, and a blit destination gains nothing from private.
             * Managed on a discrete GPU, as the backend's targets. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            td.storageMode = s_unified ? MTLStorageModeShared : MTLStorageModeManaged;
#pragma clang diagnostic pop
            [s_slot[k] release];
            s_slot[k] = [s_dev newTextureWithDescriptor:td];
        }
    }
    return (void *)s_slot[k];
}

/* A guest-bytes frame (bpp 4 = X8R8G8B8, which is BGRA8's byte order;
 * bpp 2 = R5G6B5) into slot k. */
int fbm_upload(int k, const uint8_t *src, uint32_t w, uint32_t h, uint32_t pitch,
               uint32_t bpp)
{
    id<MTLTexture> t;
    uint32_t x, y;

    @autoreleasepool {
        if (s_slot_cb[k]) {
            [s_slot_cb[k] waitUntilCompleted];
            [s_slot_cb[k] release];
            s_slot_cb[k] = nil;
        }
        t = (id<MTLTexture>)fbm_slot(k, w, h);
        if (!t)
            return 0;
        if (bpp == 4) {
            [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:src
                 bytesPerRow:pitch];
        } else {
            if (s_conv_cap < w) {
                uint32_t *n = (uint32_t *)realloc(s_conv, (size_t)w * 4);
                if (!n)
                    return 0;
                s_conv = n;
                s_conv_cap = w;
            }
            for (y = 0; y < h; y++) {
                const uint16_t *r = (const uint16_t *)(src + (size_t)y * pitch);
                for (x = 0; x < w; x++) {
                    uint32_t v = r[x];
                    uint32_t b = (v & 0x1F) << 3, g = ((v >> 5) & 0x3F) << 2,
                             rr = ((v >> 11) & 0x1F) << 3;
                    s_conv[x] = 0xFF000000u | (rr << 16) | (g << 8) | b;
                }
                [t replaceRegion:MTLRegionMake2D(0, y, w, 1) mipmapLevel:0 withBytes:s_conv
                     bytesPerRow:w * 4];
            }
        }
    }
    return 1;
}

static void save_shot(id<MTLBuffer> buf, uint32_t w, uint32_t h, const char *path,
                      uint32_t flip)
{
    SDL_Surface *surf = SDL_CreateSurface((int)w, (int)h, SDL_PIXELFORMAT_BGR24);
    const uint8_t *px = (const uint8_t *)[buf contents];
    uint32_t x, y;

    if (!surf)
        return;
    for (y = 0; y < h; y++) {
        uint8_t *o = (uint8_t *)surf->pixels + (size_t)y * surf->pitch;
        const uint8_t *in = px + (size_t)y * w * 4;
        for (x = 0; x < w; x++) {
            /* BGRA in the drawable; BGR24 is the bytes B, G, R. */
            o[x * 3 + 0] = in[x * 4 + 0];
            o[x * 3 + 1] = in[x * 4 + 1];
            o[x * 3 + 2] = in[x * 4 + 2];
        }
    }
    if (SDL_SaveBMP(surf, path))
        fprintf(stderr, "[PRESENT] window shot: %s (%ux%u, flip %u)\n", path, w, h, flip);
    else
        fprintf(stderr, "[PRESENT] window shot at flip %u failed: %s\n", flip, SDL_GetError());
    SDL_DestroySurface(surf);
}

/* Slot k (fw x fh, drawn at render scale n with hor+ pad pad) into the
 * window. 0 when no drawable came (timeout, display asleep, window hidden):
 * the caller keeps the frame and tries again. */
#ifdef XBOXRECOMP_ENHANCE
/* Texture k of the overlay (0 the atlas, 1.. the image slots), re-made when
 * its gen moves. */
static id<MTLTexture> ovl_texture(uint16_t k)
{
    const uint32_t *px;
    uint32_t w, h;
    uint64_t gen;
    MTLTextureDescriptor *td;
    if (k == 0) {
        const struct nv2a_ovl_atlas *a = nv2a_overlay_atlas();
        if (!a)
            return nil;
        px = a->px, w = a->w, h = a->h, gen = a->gen;
    } else {
        const struct nv2a_ovl_image *im = k <= NV2A_OVL_IMAGES ? nv2a_overlay_image(k) : NULL;
        if (!im || !im->px)
            return nil;
        px = im->px, w = im->w, h = im->h, gen = im->gen;
    }
    if (s_ovl_tex[k] && s_ovl_tex_gen[k] == gen)
        return s_ovl_tex[k];
    [s_ovl_tex[k] release];
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                            width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    s_ovl_tex[k] = [s_dev newTextureWithDescriptor:td];
    s_ovl_tex_gen[k] = gen;
    [s_ovl_tex[k] replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0
                      withBytes:px bytesPerRow:(NSUInteger)w * 4];
    return s_ovl_tex[k];
}

/* Grows b to hold n bytes, doubling from 4 KiB as D3D11's ovl_fill does. */
static int ovl_grow(id<MTLBuffer> *b, NSUInteger *cap, NSUInteger n)
{
    NSUInteger c;
    if (n <= *cap)
        return 1;
    for (c = *cap ? *cap : 4096; c < n; c *= 2)
        ;
    [*b release];
    *b = [s_dev newBufferWithLength:c options:MTLResourceStorageModeShared];
    *cap = *b ? c : 0;
    return *b != nil;
}

/* The set holding list l's current gen, filled when the gen moved; the
 * command buffer cb now uses it. */
static struct ovl_set *ovl_set(const struct nv2a_ovl_list *l, id<MTLCommandBuffer> cb)
{
    int y = (int)l->layer;
    struct ovl_set *o;
    if (y < 0 || y >= NV2A_OVL_COUNT)
        return NULL;
    o = &s_ovl_set[y][s_ovl_cur[y]];
    if (o->gen != l->gen) {
        s_ovl_cur[y] = (s_ovl_cur[y] + 1) % OVL_SETS;
        o = &s_ovl_set[y][s_ovl_cur[y]];
        if (o->cb && [o->cb status] < MTLCommandBufferStatusCompleted)
            [o->cb waitUntilCompleted];
        o->gen = 0;
        if (!ovl_grow(&o->vb, &o->vcap, l->nvtx * sizeof *l->vtx)
            || !ovl_grow(&o->ib, &o->icap, l->nidx * sizeof *l->idx))
            return NULL;
        memcpy([o->vb contents], l->vtx, l->nvtx * sizeof *l->vtx);
        memcpy([o->ib contents], l->idx, l->nidx * sizeof *l->idx);
        o->gen = l->gen;
    }
    [o->cb release];
    o->cb = [cb retain];
    return o;
}

/* The lists over the frame at r, in a drawable of dw x dh. */
static void ovl_draw(id<MTLRenderCommandEncoder> enc, id<MTLCommandBuffer> cb,
                     const struct nv2a_ovl_list *const *ls, int nls, struct nv2a_rect r,
                     int dw, int dh)
{
    int i;
    uint32_t c;
    MTLViewport vp = { 0, 0, dw, dh, 0.0, 1.0 };
    [enc setRenderPipelineState:s_pso_ovl];
    [enc setViewport:vp];
    [enc setFragmentSamplerState:s_smp[1] atIndex:0];
    for (i = 0; i < nls; i++) {
        const struct nv2a_ovl_list *l = ls[i];
        float sx = l->out_w ? (float)r.w / (float)l->out_w : 1.0f;
        float sy = l->out_h ? (float)r.h / (float)l->out_h : 1.0f;
        float xf[4] = { sx * 2.0f / dw, -sy * 2.0f / dh,
                        r.x * 2.0f / dw - 1.0f, 1.0f - r.y * 2.0f / dh };
        struct ovl_set *o = ovl_set(l, cb);
        if (o) {
            [enc setVertexBuffer:o->vb offset:0 atIndex:0];
            [enc setVertexBuffer:o->ib offset:0 atIndex:1];
            [enc setVertexBytes:xf length:sizeof xf atIndex:2];
            for (c = 0; c < l->ncmd; c++) {
                const struct nv2a_ovl_cmd *k = &l->cmd[c];
                id<MTLTexture> t = ovl_texture(k->tex);
                long x0 = r.x + (long)(k->clip[0] * sx), y0 = r.y + (long)(k->clip[1] * sy);
                long x1 = r.x + (long)(k->clip[0] * sx + k->clip[2] * sx + 0.999f);
                long y1 = r.y + (long)(k->clip[1] * sy + k->clip[3] * sy + 0.999f);
                MTLScissorRect sc;
                if (!t || !k->count)
                    continue;
                x0 = x0 < 0 ? 0 : x0 > dw ? dw : x0;
                y0 = y0 < 0 ? 0 : y0 > dh ? dh : y0;
                x1 = x1 < x0 ? x0 : x1 > dw ? dw : x1;
                y1 = y1 < y0 ? y0 : y1 > dh ? dh : y1;
                if (x1 == x0 || y1 == y0)
                    continue;
                sc.x = (NSUInteger)x0;
                sc.y = (NSUInteger)y0;
                sc.width = (NSUInteger)(x1 - x0);
                sc.height = (NSUInteger)(y1 - y0);
                [enc setScissorRect:sc];
                [enc setFragmentTexture:t atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:k->first
                        vertexCount:k->count];
            }
        }
    }
}
#endif /* XBOXRECOMP_ENHANCE */

#ifdef XBOXRECOMP_ENHANCE
/* present.shader's chain: the one built for s_chain_gen, or NULL (off, or
 * failed for that generation). Main thread only, like the rest. */
static libra_mtl_filter_chain_t s_chain;
static unsigned s_chain_gen;
static size_t s_chain_frames;
static PFN_libra_mtl_filter_chain_frame s_chain_ff;   /* with the chain */
static PFN_libra_mtl_filter_chain_set_param s_chain_sp;
static unsigned s_chain_pgen;       /* the parameter generation applied */
static int s_chain_ov_said;
static int s_chain_lines;           /* the preset draws per source line */
static int s_chain_said;            /* the chain's first frame said how it was fed */
/* The chain's input resolved to the guest's lines (nv2a_present_chain_source),
 * and its output at the rectangle's size, drawn 1:1 into the drawable after:
 * librashader gives the last pass the whole texture's size as OutputSize
 * and FinalViewportSize, not the viewport's, so drawn straight into a
 * letterboxed rectangle the mask was laid out for the whole drawable and
 * squeezed into it (D3D11's s_lrs_out). */
static id<MTLTexture> s_chain_src, s_chain_out;

/* *t at w x h (BGRA8, private, a render target that is read after), made
 * again when the size changed. A command buffer still in flight keeps the
 * old one. */
static id<MTLTexture> chain_tex(id<MTLTexture> *t, uint32_t w, uint32_t h)
{
    if (!*t || [*t width] != w || [*t height] != h) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        [*t release];
        *t = [s_dev newTextureWithDescriptor:td];
    }
    return *t;
}

static void chain_free(void)
{
    const struct nv2a_lrs *l;
    PFN_libra_mtl_filter_chain_free fr;
    if (!s_chain)
        return;
    l = nv2a_present_chain_lib();
    fr = l ? (PFN_libra_mtl_filter_chain_free)l->sym("libra_mtl_filter_chain_free") : NULL;
    if (fr)
        fr(&s_chain);
    s_chain = NULL;
    s_chain_ff = NULL;
    s_chain_sp = NULL;
    [s_chain_src release];
    [s_chain_out release];
    s_chain_src = s_chain_out = nil;
    nv2a_present_chain_params_clear();
}

/* nv2a_present_chain_params_apply's setter. librashader refuses a name the
 * chain lacks (UnknownShaderParameter), but the names come from the table
 * read off this chain's own preset, so none is unknown; an error here would
 * repeat on every change, so it is dropped unlogged. */
static void chain_set_param(void *ctx, const char *name, float v)
{
    libra_error_t e;
    char msg[256];
    if (!s_chain_sp)
        return;
    e = s_chain_sp((libra_mtl_filter_chain_t *)ctx, name, v);
    if (e)
        nv2a_present_chain_error(e, msg, sizeof msg);
}

/* The chain for the current request, (re)built when its generation moved:
 * NULL for "none", a missing library or a preset that failed (said once,
 * then not retried until the next request). */
static libra_mtl_filter_chain_t chain_get(void)
{
    unsigned gen = nv2a_present_chain_gen();
    const struct nv2a_lrs *l;
    PFN_libra_preset_create pc;
    PFN_libra_preset_free pf;
    PFN_libra_mtl_filter_chain_create cc;
    libra_shader_preset_t preset = NULL;
    libra_error_t e;
    filter_chain_mtl_opt_t co = { LIBRASHADER_CURRENT_VERSION, false };
    char path[1024], msg[512];
    int found;

    if (gen == s_chain_gen)
        return s_chain;
    chain_free();
    s_chain_gen = gen;
    s_chain_frames = 0;
    if (!nv2a_present_chain_wanted())
        return NULL;
    found = nv2a_present_chain_preset_lines(path, sizeof path, NULL, &s_chain_lines);
    s_chain_said = 0;
    l = found > 0 ? nv2a_present_chain_lib() : NULL;
    if (!l) {
        nv2a_present_chain_failed(gen);
        return NULL;
    }
    pc = (PFN_libra_preset_create)l->sym("libra_preset_create");
    pf = (PFN_libra_preset_free)l->sym("libra_preset_free");
    cc = (PFN_libra_mtl_filter_chain_create)l->sym("libra_mtl_filter_chain_create");
    s_chain_ff = (PFN_libra_mtl_filter_chain_frame)l->sym("libra_mtl_filter_chain_frame");
    s_chain_sp = (PFN_libra_mtl_filter_chain_set_param)l->sym("libra_mtl_filter_chain_set_param");
    if (!pc || !pf || !cc || !s_chain_ff) {
        fprintf(stderr, "[SHADER] present.shader off: the library has no Metal runtime\n");
        nv2a_present_chain_failed(gen);
        return NULL;
    }
    e = pc(path, &preset);
    /* The parameters before the chain: creating it consumes the preset. */
    if (!e)
        nv2a_present_chain_params_publish(&preset, gen);
    if (!e)
        e = cc(&preset, s_queue, &co, &s_chain);
    if (preset)
        pf(&preset);
    if (e) {
        nv2a_present_chain_error(e, msg, sizeof msg);
        fprintf(stderr, "[SHADER] present.shader off: %s: %s\n", path, msg);
        s_chain = NULL;
        nv2a_present_chain_params_clear();
        nv2a_present_chain_failed(gen);
        return NULL;
    }
    /* A new chain starts at the preset's values: apply every override. */
    s_chain_pgen = nv2a_present_chain_params_gen() - 1;
    fprintf(stderr, "[SHADER] present.shader: %s (Metal)\n", path);
    return s_chain;
}
#endif /* XBOXRECOMP_ENHANCE */

int fbm_present(SDL_Window *win, int k, uint32_t fw, uint32_t fh, unsigned n,
                uint32_t pad, int filter, const char *shot_path, uint32_t flip,
                const struct nv2a_ovl_list *const *layers, int nlayers)
{
    int ok = 0;

    @autoreleasepool {
        id<CAMetalDrawable> d;
        id<MTLCommandBuffer> cb;
        id<MTLRenderCommandEncoder> enc;
        MTLRenderPassDescriptor *rp;
        id<MTLBuffer> shot = nil;
        struct nv2a_rect r = {0, 0, 0, 0};
        int whole, dw = 0, dh = 0;
        unsigned ov = nv2a_host_opts()->overscan;
        CGSize cur = s_layer.drawableSize;
        int internal = s_internal;
#ifdef XBOXRECOMP_ENHANCE
        libra_mtl_filter_chain_t chain = NULL;
        int chained = 0;

        if (nv2a_present_chain_wanted() || s_chain)
            chain = chain_get();
        if (chain) {
            nv2a_present_chain_params_apply(&s_chain_pgen, chain_set_param, &s_chain);
            /* The chain draws its mask and scanlines per output pixel: it
             * must draw at the window's size. Drawn at the frame's and
             * scaled after (by the layer here, by gamescope on the Deck),
             * they are resampled into colour fringes and moire. */
            internal = 0;
        }
        /* The chain reads the whole slot: a crop has no place in it. */
        if (chain && ov) {
            if (!s_chain_ov_said)
                fprintf(stderr, "[SHADER] display.overscan=%u ignored while"
                        " present.shader runs\n", ov);
            s_chain_ov_said = 1;
            ov = 0;
        }
#endif

        /* Checked every present, not only on resize events: a move between
         * displays of another scale need not arrive as SIZE_CHANGED. Under
         * present.surface=internal the drawable is the frame instead (a
         * render.scale change from the menu resizes it). */
        if (internal) {
            layer_filter(filter);
            if (fw && fh) {
                dw = (int)fw;
                dh = (int)fh;
            } else {
                dw = (int)cur.width;
                dh = (int)cur.height;
            }
        } else {
            SDL_GetWindowSizeInPixels(win, &dw, &dh);
        }
        if (dw > 0 && dh > 0 && ((int)cur.width != dw || (int)cur.height != dh))
            s_layer.drawableSize = CGSizeMake(dw, dh);
        d = [s_layer nextDrawable];
        if (!d)
            return 0;
        dw = (int)[d.texture width];
        dh = (int)[d.texture height];
        cb = [s_queue commandBuffer];
        rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = d.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        enc = [cb renderCommandEncoderWithDescriptor:rp];
        if (s_slot[k] && fw && fh) {
            struct nv2a_crop c = {0};
            /* The slot is exactly the frame, so the texture size is fw x fh. */
            if (ov)
                nv2a_present_crop(fw, fh, fw, fh, n, pad, ov, filter, &c);
            whole = nv2a_present_rect(ov ? c.fit_w : fw, ov ? c.fit_h : fh, (uint32_t)dw,
                                      (uint32_t)dh, filter, &r);
#ifdef XBOXRECOMP_ENHANCE
            /* The chain fills what the stock present shows: under
             * internal the layer's fit, not an integer multiple. */
            if (chain)
                whole = nv2a_present_chain_rect(fw, fh, (uint32_t)dw, (uint32_t)dh, filter,
                                                s_internal, &r);
#endif
            if (internal) {
                /* 1:1: the layer does the scaling. A crop is zoomed into the
                 * whole drawable with the present filter's sampler, except
                 * integer, which stays nearest (D3D11 matches). */
                r.x = r.y = 0;
                r.w = dw;
                r.h = dh;
                whole = 1;
                if (!ov || filter == NV2A_PRESENT_INTEGER)
                    filter = NV2A_PRESENT_NEAREST;
            }
#ifdef XBOXRECOMP_ENHANCE
            if (chain && r.w > 0 && r.h > 0
                && chain_tex(&s_chain_out, (uint32_t)r.w, (uint32_t)r.h)) {
                /* This pass only clears; the chain draws after it into
                 * s_chain_out, which the next pass draws 1:1 into the
                 * rectangle before the overlay. */
                libra_viewport_t lv = { 0.0f, 0.0f, (uint32_t)r.w, (uint32_t)r.h };
                frame_mtl_opt_t fo;
                libra_error_t e;
                char msg[512];
                id<MTLTexture> src = s_slot[k];
                uint32_t sw, sh;
                int resolved = 0;

                memset(&fo, 0, sizeof fo);
                fo.version = LIBRASHADER_CURRENT_VERSION;
                fo.total_subframes = 1;
                fo.current_subframe = 1;
                fo.frames_per_second =
                    nv2a_present_chain_rate(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
                [enc endEncoding];
                enc = nil;
                nv2a_present_chain_source(fw, fh, n, s_chain_lines, &sw, &sh);
                if (sh < fh && s_pso_lines && chain_tex(&s_chain_src, sw, sh)) {
                    MTLRenderPassDescriptor *lp = [MTLRenderPassDescriptor renderPassDescriptor];
                    id<MTLRenderCommandEncoder> le;
                    uint32_t rows = fh / sh;
                    lp.colorAttachments[0].texture = s_chain_src;
                    lp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                    lp.colorAttachments[0].storeAction = MTLStoreActionStore;
                    le = [cb renderCommandEncoderWithDescriptor:lp];
                    [le setRenderPipelineState:s_pso_lines];
                    [le setFragmentTexture:s_slot[k] atIndex:0];
                    [le setFragmentBytes:&rows length:sizeof rows atIndex:0];
                    [le drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    [le endEncoding];
                    src = s_chain_src;
                    resolved = 1;
                }
                /* Said at the first frame, once the source is known: a
                 * height that is no multiple of the scale, or a resolve
                 * that did not build, feeds the host rows even to a CRT. */
                if (s_chain_lines && !s_chain_said) {
                    s_chain_said = 1;
                    if (resolved || n <= 1)
                        fprintf(stderr, "[SHADER] present.shader: fed the guest's lines"
                                " (%u of %u rows)\n", resolved ? sh : fh, fh);
                    else
                        fprintf(stderr, "[SHADER] present.shader: fed the host's %u rows (%s)\n",
                                fh, sh < fh ? "the guest-line resolve failed"
                                            : "not a multiple of render.scale");
                }
                e = s_chain_ff(&chain, cb, s_chain_frames++, src, s_chain_out, &lv, NULL, &fo);
                rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
                enc = [cb renderCommandEncoderWithDescriptor:rp];
                if (e) {
                    nv2a_present_chain_error(e, msg, sizeof msg);
                    fprintf(stderr, "[SHADER] present.shader off: frame: %s\n", msg);
                    nv2a_present_chain_failed(s_chain_gen);
                    chain_free();   /* the command buffer keeps what it uses */
                } else {
                    /* Same size, so nearest copies it texel for texel. */
                    MTLViewport vp = { r.x, r.y, r.w, r.h, 0.0, 1.0 };
                    [enc setRenderPipelineState:s_pso];
                    [enc setViewport:vp];
                    [enc setFragmentTexture:s_chain_out atIndex:0];
                    [enc setFragmentSamplerState:s_smp[0] atIndex:0];
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                }
                chained = 1;
            }
            if (!chained && r.w > 0 && r.h > 0) {
#else
            if (r.w > 0 && r.h > 0) {
#endif
                MTLViewport vp = { r.x, r.y, r.w, r.h, 0.0, 1.0 };
                int lin = filter == NV2A_PRESENT_LINEAR
                          || (filter == NV2A_PRESENT_INTEGER && !whole);
                if (ov) {
                    float crop[4] = { c.u_scale, c.v_scale, c.u_off, c.v_off };
                    [enc setRenderPipelineState:s_pso_crop];
                    [enc setVertexBytes:crop length:sizeof crop atIndex:0];
                } else {
                    [enc setRenderPipelineState:s_pso];
                }
                [enc setViewport:vp];
                [enc setFragmentTexture:s_slot[k] atIndex:0];
                [enc setFragmentSamplerState:s_smp[lin] atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
        }
#ifdef XBOXRECOMP_ENHANCE
        /* Every overlay size comes from the frame's rectangle (the whole
         * drawable before the first frame). */
        if (r.w <= 0 || r.h <= 0) {
            r.x = r.y = 0;
            r.w = dw;
            r.h = dh;
        }
        nv2a_overlay_set_placement(r.x, r.y, r.w, r.h);
        /* Under internal the layer fits the drawable into the window after
         * this pass, so the pointer's window pixels map through that fit
         * (D3D11's stretch is the same call); else the two are one. */
        if (internal) {
            int ww = 0, wh = 0, v[4];
            SDL_GetWindowSizeInPixels(win, &ww, &wh);
            nv2a_overlay_view_fit(ww, wh, dw, dh, v);
            nv2a_overlay_set_view(v[0], v[1], v[2], v[3], dw, dh);
        } else {
            nv2a_overlay_set_view(0, 0, 0, 0, 0, 0);
        }
        if (nlayers && s_pso_ovl)
            ovl_draw(enc, cb, layers, nlayers, r, dw, dh);
#else
        (void)layers;
        (void)nlayers;
#endif
        [enc endEncoding];
        if (shot_path) {
            shot = [s_dev newBufferWithLength:(NSUInteger)dw * dh * 4
                                      options:MTLResourceStorageModeShared];
            if (shot) {
                id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                [b copyFromTexture:d.texture sourceSlice:0 sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake((NSUInteger)dw, (NSUInteger)dh, 1)
                          toBuffer:shot destinationOffset:0
                 destinationBytesPerRow:(NSUInteger)dw * 4
               destinationBytesPerImage:(NSUInteger)dw * dh * 4];
                [b endEncoding];
            }
        }
        [cb presentDrawable:d];
        [cb commit];
        [s_slot_cb[k] release];
        s_slot_cb[k] = [cb retain];
        if (shot) {
            [cb waitUntilCompleted];
            save_shot(shot, (uint32_t)dw, (uint32_t)dh, shot_path, flip);
            [shot release];
        }
        ok = 1;
    }
    return ok;
}

int fbm_window_level(SDL_Window *win, int top)
{
    NSWindow *w = (NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(win),
                                                     SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    NSWindowLevel want = top ? kCGMainMenuWindowLevel + 1 : NSNormalWindowLevel;

    if (!w || [w level] == want)
        return 0;
    [w setLevel:want];
    return 1;
}

#endif /* __APPLE__ */
