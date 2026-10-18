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
 *
 * Manual retain/release, like nv2a_pb_metal.m. Every entry point is wrapped in
 * an autorelease pool: the main loop and the ack thread have none of their own.
 */
#if defined(__APPLE__)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <SDL.h>
#include <SDL_metal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/nv2a_backend_common.h"
#include "fb_present_metal.h"

#if __has_feature(objc_arc)
#error "fb_present_metal.m is manual retain/release: build it without -fobjc-arc"
#endif

static id<MTLDevice>               s_dev;
static id<MTLCommandQueue>         s_queue;
static id<MTLRenderPipelineState>  s_pso;
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

static const char s_blit_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VO { float4 p [[position]]; float2 t; };\n"
    "vertex VO vs_blit(uint i [[vertex_id]]) {\n"
    "    VO o;\n"
    "    o.t = float2((i << 1) & 2, i & 2);\n"
    "    o.p = float4(o.t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "fragment float4 fs_blit(VO i [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "                        sampler s [[sampler(0)]]) {\n"
    "    return float4(t.sample(s, i.t).rgb, 1);\n"
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
    MTLCompileOptions *opts;
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
        opts = [[MTLCompileOptions alloc] init];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        opts.fastMathEnabled = NO;
#pragma clang diagnostic pop
        lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:s_blit_msl]
                                  options:opts error:&e];
        [opts release];
        if (!lib)
            return fail(err, errn, "present shader failed to compile");
        pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = [[lib newFunctionWithName:@"vs_blit"] autorelease];
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_blit"] autorelease];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        s_pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        [pd release];
        [lib release];
        if (!s_pso)
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
            w = 640 * (int)nv2a_host_render_scale();
            h = 480 * (int)nv2a_host_render_scale();
        } else {
            SDL_Metal_GetDrawableSize(win, &w, &h);
        }
        if (w > 0 && h > 0)
            s_layer.drawableSize = CGSizeMake(w, h);
    }
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
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, (int)w, (int)h, 24,
                                                       SDL_PIXELFORMAT_BGR24);
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
    if (SDL_SaveBMP(surf, path) == 0)
        fprintf(stderr, "[PRESENT] window shot: %s (%ux%u, flip %u)\n", path, w, h, flip);
    else
        fprintf(stderr, "[PRESENT] window shot at flip %u failed: %s\n", flip, SDL_GetError());
    SDL_FreeSurface(surf);
}

/* Slot k (fw x fh) into the window. 0 when no drawable came (timeout, display
 * asleep, window hidden): the caller keeps the frame and tries again. */
int fbm_present(SDL_Window *win, int k, uint32_t fw, uint32_t fh, int filter,
                const char *shot_path, uint32_t flip)
{
    int ok = 0;

    @autoreleasepool {
        id<CAMetalDrawable> d;
        id<MTLCommandBuffer> cb;
        id<MTLRenderCommandEncoder> enc;
        MTLRenderPassDescriptor *rp;
        id<MTLBuffer> shot = nil;
        struct nv2a_rect r;
        int whole, dw = 0, dh = 0;
        CGSize cur = s_layer.drawableSize;

        /* Checked every present, not only on resize events: a move between
         * displays of another scale need not arrive as SIZE_CHANGED. Under
         * present.surface=internal the drawable is the frame instead (a
         * render.scale change from the menu resizes it). */
        if (s_internal) {
            layer_filter(filter);
            if (fw && fh) {
                dw = (int)fw;
                dh = (int)fh;
            } else {
                dw = (int)cur.width;
                dh = (int)cur.height;
            }
        } else {
            SDL_Metal_GetDrawableSize(win, &dw, &dh);
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
            whole = nv2a_present_rect(fw, fh, (uint32_t)dw, (uint32_t)dh, filter, &r);
            if (s_internal) {
                /* 1:1: the layer does the scaling. */
                r.x = r.y = 0;
                r.w = dw;
                r.h = dh;
                whole = 1;
                filter = NV2A_PRESENT_NEAREST;
            }
            if (r.w > 0 && r.h > 0) {
                MTLViewport vp = { r.x, r.y, r.w, r.h, 0.0, 1.0 };
                int lin = filter == NV2A_PRESENT_LINEAR
                          || (filter == NV2A_PRESENT_INTEGER && !whole);
                [enc setRenderPipelineState:s_pso];
                [enc setViewport:vp];
                [enc setFragmentTexture:s_slot[k] atIndex:0];
                [enc setFragmentSamplerState:s_smp[lin] atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
        }
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

#endif /* __APPLE__ */
