/**
 * Execute the parts of the title's pushbuffer that produce visible pixels.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; without
 * something consuming them the framebuffer stays whatever it was, which is how
 * a fully booted title renders a black screen. This walks the same command
 * stream nv2a_pb_scan.c surveys and carries out the subset that decides what is
 * on screen: which surface is being drawn into, and clearing it.
 *
 * It also rasterises geometry, but only the part that can be drawn honestly:
 * batches whose attribute 0 is already in screen space, flat-shaded, straight
 * into the same guest framebuffer the clear writes. Titles draw their UI, HUD
 * and 2D overlays that way, so it is the first geometry to appear. Batches that
 * need a vertex program executed are counted and skipped rather than drawn
 * somewhere wrong -- see raster_batch(). Texturing, depth and vertex programs
 * are still a renderer, not a command decoder; the upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c.
 *
 * Everything this does not handle is counted and ranked by
 * nv2a_pb_exec_report(), so what remains is a list rather than a guess.
 *
 * Enabled with RECOMP_PB_EXEC. RECOMP_RASTER_TEST draws one known triangle
 * after every clear, which separates "the pixel path is broken" from "the title
 * has not given us any vertices". RECOMP_DEBUG=fb_dump=<prefix> writes the surface to
 * <prefix>NNN.bmp, so the result can be looked at without a display.
 */
#include <math.h>
#include "recomp_env.h"
#include "recomp_exit_hook.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */
#include "kernel_pacing.h"
#include "xbox_memory_layout.h"   /* xbox_Nv2aFrameCounterFlip */
#include "nv2a_flip_hold.h"
#include "platform/host_time.h"   /* xbox_HostNowNs */
/* The swizzle decoder the D3D8 layer already uses -- one implementation of
 * Morton order, not a second one that can disagree with it. */
#include "../d3d/d3d8_swizzle.h"
#include "nv2a_vsh_cpu.h"
#include "nv2a_vsh_cdump.h"
#include "nv2a_fb_ring.h"
#include "nv2a_combiner.h"
#include "nv2a_pb_state.h"
#include "nv2a_backend_common.h"
#include "nv2a_tex_pack.h"
#include "nv2a_zbuf_cache.h"
#include "../video/video_player.h"   /* xbox_FramebufferWindowFrameStats */
#if defined(_WIN32)
#include <windows.h>
#define NV_TLS __declspec(thread)
#else
#include <pthread.h>
#include <unistd.h>
#define NV_TLS __thread
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowStart(void);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

/* Would writing this surface land on the title's own image?
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset within the colour DMA object,
 * not a guest virtual address, and this executor has always used it as one.
 * That is harmless while the two happen to agree and catastrophic when they do
 * not: the Xbox Dashboard names surface 0x00088000 at 1280x960x4, so clearing
 * it wrote 4.9 MB of opaque black from 0x00088000 to 0x00538000 -- straight
 * over its own code, its D3D context at 0x000BBFC0 and the register-block
 * pointer at 0x000BE2C4. The symptom was a title that submitted one perfect
 * frame and then spun forever in a pushbuffer-full loop, three layers away,
 * with every D3D global reading 0xFF000000: the clear colour.
 *
 * So refuse, and say so. Getting the address right needs the DMA object base
 * this ignores (NV097_SET_CONTEXT_DMA_COLOR); until that exists, writing
 * nothing is strictly better than writing over the guest, and a title that
 * cannot draw is easier to debug than one that has been overwritten.
 */
static int surface_hits_image(uint32_t base, uint32_t bytes)
{
    if (!g_xbox_image_hi || !bytes)
        return 0;
    return base < g_xbox_image_hi && base + bytes > g_xbox_image_lo;
}

/* Where a DMA-object offset actually lives.
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset inside the colour DMA object,
 * and for a framebuffer that object covers physical memory -- so the offset
 * is a physical address, not a guest VA. Those are the same number in this
 * runtime, which is why treating it as a VA works until it does not: on
 * hardware the image is mapped at VA 0x00010000 from arbitrary physical
 * pages, so a framebuffer at physical 0x84000 does not overlap it. Here it
 * would.
 *
 * The title tells us which it is by where it allocated. Half-Life 2's
 * framebuffer comes from MmAllocateContiguousMemory, which this runtime
 * serves from the window at XBOX_CONTIG_BASE, so physical P is visible at
 * XBOX_CONTIG_BASE + P -- clear of the image, and the same bytes the title's
 * own writes and the framebuffer window reach.
 *
 * So: use the offset as a VA when that is credible, and fall back to the
 * physical mirror exactly when it is not. Titles whose surfaces already sit
 * in ordinary RAM (Wreckless renders to the tiled alias of physical
 * 0x01954000) keep the first path and are unaffected.
 */
static uint32_t dma_resolve(uint32_t offset)
{
    extern uint32_t xbox_ContiguousAllocatedBytes(void);

    /* Did this runtime hand the offset out as contiguous memory? Then the
     * bytes live in the window, and that is not a guess: the arena is a bump
     * allocator from XBOX_CONTIG_BASE, so everything below its high-water
     * mark is memory some MmAllocateContiguousMemory call returned. The
     * title's own writes go through the window, so the executor's must too.
     *
     * Checking this BEFORE the image test is the whole point. The image test
     * only catches an offset that would land on the title's code, and whether
     * it does is an accident of where the image happens to end: Half-Life 2's
     * colour surface is physical 0x00A6C000, which clears the image by 700 KB.
     * So it looked like an ordinary VA, and the executor cleared 1.2 MB of
     * black straight through the guest heap -- which faulted the title three
     * frames later on a pointer that had been overwritten, while the real
     * framebuffer at 0x80A6C000 stayed untouched and the screen stayed black. */
    if (offset < xbox_ContiguousAllocatedBytes())
        return XBOX_CONTIG_BASE + offset;
    if (!surface_hits_image(offset, 1))
        return offset;
    if ((uint64_t)offset < XBOX_CONTIG_SIZE)
        return XBOX_CONTIG_BASE + offset;
    return offset;                         /* nothing better to offer */
}

/* Where a context DMA object starts, and how far it reaches.
 *
 * An offset method (SET_SEMAPHORE_OFFSET, GET_REPORT) is an offset inside
 * the object the matching SET_CONTEXT_DMA_* method bound. XDK D3D makes its
 * surface, texture and vertex objects cover physical memory from 0, which
 * dma_resolve assumes, but it aims the semaphore object at its fence page:
 * BLiNX 2's at physical 0x1000, Burnout 3's at 0x3E6000, Spartan's (XDK
 * 5849) at 0x2000, object 8 = { 0x0002B03D, limit 0x20, frame 0x00002003 }.
 * Read as a bare offset, every release went to physical 0. BLiNX 2 and
 * Burnout 3 got by on their fence mirrors; Spartan registers none, and D3D's
 * fence wait (sub_00228FA0) spun during device creation.
 *
 * The object is found the way PFIFO finds it: the handle in RAMHT (base and
 * size from PFIFO_RAMHT), whose context word holds the instance offset in
 * PRAMIN (aperture 0x700000). Its base is the frame page (word 2, bits
 * 31:12) plus the adjust (word 0, bits 31:20); its limit is word 1. RAMHT
 * is searched rather than hashed: the hash takes the channel id, which this
 * runtime does not model, and handles are unique in XDK D3D's one channel.
 * The table is 4 to 32 KB and is read once per bind. Returns 0 when the
 * aperture is unmapped or the handle is not there (a title that never built
 * RAMHT); the caller then keeps the bare offset with no limit. */
#define NV2A_PFIFO_RAMHT  0x002210u
#define NV2A_PRAMIN       0x700000u
#define NV2A_PRAMIN_SIZE  0x100000u

struct ctxdma_hit { uint32_t ramht_off, inst, w0, w1, w2; };

static int ctxdma_lookup(uint32_t handle, uint32_t *base, uint32_t *limit,
                         struct ctxdma_hit *hit, uint32_t *ramht_reg)
{
    const volatile uint8_t *ap = xbox_Nv2aAperture();
    uint32_t ramht, rbase, size, i, ctx, inst;

    *ramht_reg = 0;
    if (!ap)
        return 0;
#define AP32(off) (*(const volatile uint32_t *)(ap + (off)))
    ramht = *ramht_reg = AP32(NV2A_PFIFO_RAMHT);
    rbase = ((ramht >> 4) & 0x1Fu) << 12;            /* BASE_ADDRESS [8:4] */
    size = 0x1000u << ((ramht >> 16) & 0x3u);        /* SIZE [17:16]: 4-32 KB */
    for (i = 0; i + 8 <= size && rbase + i + 8 <= NV2A_PRAMIN_SIZE; i += 8) {
        if (AP32(NV2A_PRAMIN + rbase + i) != handle)
            continue;
        ctx = AP32(NV2A_PRAMIN + rbase + i + 4);
        if (!(ctx & 0x80000000u))                    /* not valid */
            continue;
        inst = (ctx & 0xFFFFu) << 4;
        if (inst + 12 > NV2A_PRAMIN_SIZE)
            continue;
        hit->ramht_off = rbase + i;
        hit->inst = inst;
        hit->w0 = AP32(NV2A_PRAMIN + inst);
        hit->w1 = AP32(NV2A_PRAMIN + inst + 4);
        hit->w2 = AP32(NV2A_PRAMIN + inst + 8);
        *base = (hit->w2 & 0xFFFFF000u) | (hit->w0 >> 20);
        *limit = hit->w1;
        return 1;
    }
#undef AP32
    return 0;
}

/* One bound offset object (semaphore, report), resolved when it is bound,
 * as PFIFO does: a re-bind re-reads instance memory, nothing else does. */
struct ctxdma_bind {
    uint32_t handle, base, limit;
    int found;
    int said;                     /* bind lines printed, capped at 8 */
    int refused_said;             /* the refused-write line, once per purpose */
    int have_last;
    uint32_t last_handle, last_base, last_limit;
    int last_found;
};

static void ctxdma_bind(struct ctxdma_bind *b, const char *what, uint32_t handle)
{
    struct ctxdma_hit hit;
    uint32_t ramht = 0;

    b->handle = handle;
    b->base = 0;
    b->limit = 0;
    b->found = ctxdma_lookup(handle, &b->base, &b->limit, &hit, &ramht);
    if (b->have_last && b->last_handle == handle && b->last_found == b->found
            && b->last_base == b->base && b->last_limit == b->limit)
        return;                   /* the same answer: said already */
    b->have_last = 1;
    b->last_handle = handle;
    b->last_found = b->found;
    b->last_base = b->base;
    b->last_limit = b->limit;
    if (b->said >= 8)
        return;
    b->said++;
    if (b->found)
        fprintf(stderr, "  [GPU] %s context DMA 0x%08X: base 0x%08X limit "
                "0x%08X (RAMHT +0x%X, instance 0x%X: %08X %08X %08X)\n",
                what, handle, b->base, b->limit, hit.ramht_off, hit.inst,
                hit.w0, hit.w1, hit.w2);
    else
        fprintf(stderr, "  [GPU] %s context DMA 0x%08X not in RAMHT "
                "(PFIFO_RAMHT 0x%08X): the bare offset, no limit\n",
                what, handle, ramht);
    fflush(stderr);
}

/* The physical address of [offset, offset + bytes) in a bound object, or
 * refuse it (return 0) when it ends past the object's limit: the hardware
 * raises a DMA protection error and writes nothing. */
static int ctxdma_place(struct ctxdma_bind *b, const char *what,
                        uint32_t offset, uint32_t bytes, uint32_t *phys)
{
    if (b->found && (uint64_t)offset + bytes > (uint64_t)b->limit + 1u) {
        if (!b->refused_said++)
            fprintf(stderr, "  [GPU] REFUSING %s write at offset 0x%08X (%u "
                    "bytes): past the limit 0x%08X of context DMA 0x%08X\n",
                    what, offset, bytes, b->limit, b->handle);
        return 0;
    }
    *phys = b->base + offset;
    return 1;
}

static struct ctxdma_bind s_sema_bind, s_report_bind;

static int surface_write_refused(uint32_t base, uint32_t bytes, const char *what)
{
    static int said;

    if (!surface_hits_image(base, bytes))
        return 0;
    if (!said) {
        said = 1;
        fprintf(stderr,
                "  [GPU] REFUSING to %s surface 0x%08X..0x%08X: that overlaps "
                "the loaded image (0x%08X..0x%08X).\n"
                "  [GPU]   SET_SURFACE_COLOR_OFFSET is a DMA-object offset, not "
                "a guest VA, and this executor treats it as one. Writing here "
                "would destroy the title's own code and globals.\n",
                what, base, base + bytes, g_xbox_image_lo, g_xbox_image_hi);
        fflush(stderr);
    }
    return 1;
}

/* NV097 methods this executor acts on. */
/* Blending. The pair this title programs, read from its own pushbuffer
 * rather than guessed: BLEND_ENABLE written 1168 times and left on,
 * SFACTOR 0x0302 (SRC_ALPHA) and DFACTOR 0x0303 (ONE_MINUS_SRC_ALPHA).
 * ALPHA_TEST_ENABLE is written 390 times and left at zero, so this is
 * blending and not an alpha test. */
#define NV097_SET_BLEND_ENABLE            0x0304
#define NV097_SET_BLEND_FUNC_SFACTOR      0x0344
#define NV097_SET_BLEND_FUNC_DFACTOR      0x0348
#define NV097_SET_BLEND_COLOR             0x034C
#define NV097_SET_BLEND_EQUATION          0x0350
#define NV_BLEND_SRC_ALPHA                0x0302
#define NV_BLEND_ONE_MINUS_SRC_ALPHA      0x0303

#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL   0x0204
#define NV097_SET_SURFACE_FORMAT          0x0208
#define NV097_SET_SURFACE_PITCH           0x020C
#define NV097_SET_SURFACE_COLOR_OFFSET    0x0210
#define NV097_SET_COLOR_CLEAR_VALUE       0x1D90
#define NV097_SET_CLEAR_RECT_HORIZONTAL   0x1D98
#define NV097_SET_CLEAR_RECT_VERTICAL     0x1D9C
#define NV097_CLEAR_SURFACE               0x1D94
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720   /* +i*4, 16 attributes */
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760   /* +i*4 */
#define NV097_SET_BEGIN_END               0x17FC
#define NV097_SET_TEXTURE_OFFSET          0x1B00   /* +i*0x40 */
#define NV097_SET_TEXTURE_FORMAT          0x1B04
#define NV097_SET_TEXTURE_ADDRESS         0x1B08
#define NV097_SET_TEXTURE_CONTROL1        0x1B10
#define NV097_SET_TEXTURE_IMAGE_RECT      0x1B1C
/* The buffer flip. A title double-buffers by telling the GPU which buffer
 * the CRTC reads and which it draws into, advancing the write index and
 * then stalling until the flip has happened. Ignoring these means the
 * stall never clears: Half-Life 2's loader submits its initialisation,
 * asks for a flip, and waits for it in a loop that makes no kernel calls
 * and burns no dispatch, which reads as a hang with no cause.
 *
 * The flip completes here at once, because there is no scanout to be in the
 * middle of; what the console's stall did to the title (wait for the vblank
 * the swap asked for) is the flip hold's job: FLIP_STALL arms it, and the
 * ack loop holds the next walk and the KickOff ack until the guest's vblank
 * count allows the flip (nv2a_flip_hold.h). The swap's interval comes from
 * the NO_OPERATION D3D pushes before it, software method 1. */
#define NV097_NO_OPERATION                0x0100
#define NV097_SET_FLIP_READ               0x0120
#define NV097_SET_FLIP_WRITE              0x0124
#define NV097_SET_FLIP_MODULO             0x0128
#define NV097_FLIP_INCREMENT_WRITE        0x012C
#define NV097_FLIP_STALL                  0x0130
#define NV097_ARRAY_ELEMENT16             0x1800
#define NV097_ARRAY_ELEMENT32             0x1808
/* Draw a run of vertices straight out of the arrays, with no index list:
 * bits 0..23 are the first vertex, bits 24..31 the count minus one. It may
 * appear several times inside one BEGIN_END to draw a longer run. */
#define NV097_DRAW_ARRAYS                 0x1810
#define NV097_INLINE_ARRAY                0x1818
/* Immediate-mode vertices. SET_VERTEX3F/4F carry the position, and writing
 * its last component completes a vertex using whatever the SET_VERTEX_DATA*
 * registers currently hold for the other attributes. This is how Half-Life
 * 2's Xbox loader and the game's own 2D drawing submit every quad -- neither
 * uses INLINE_ARRAY -- so without these the executor saw SET_BEGIN_END pairs
 * with nothing attached and reported `draws 0` while a million and a half
 * textured quads a minute went past it. */
#define NV097_SET_VERTEX3F                0x1500   /* +0..0x08, 3 floats */
#define NV097_SET_VERTEX4F                0x1518   /* +0..0x0C, 4 floats */
#define NV097_SET_VERTEX_DATA2F_M         0x1880   /* + attr*8,  2 floats */
#define NV097_SET_VERTEX_DATA4F_M         0x1A00   /* + attr*16, 4 floats */
#define NV097_SET_VERTEX_DATA4UB          0x1940   /* + attr*4,  4 x u8 */

/* One immediate vertex, as this file packs it for the shared draw path:
 * all 16 attributes as float4 (upstream 5c3a42c). 1024 such vertices fit
 * the 64K-dword inline buffer. */
#define IMM_VERTEX_DWORDS (NV_VERTEX_ATTRS * 4)

#define NV097_CLEAR_COLOR_MASK            0xF0   /* R,G,B,A bits */
#define NV097_SET_COLOR_MASK              0x0358

/* GPU completion fences. D3D names a semaphore DMA object once, points the
 * semaphore at a word inside it once, and from then on asks the back end to
 * write the fence value it has just submitted -- a title typically sends the
 * first two once and the release a few times a frame. The title's fence wait spins on that
 * word, so writing it HERE, when the executor reaches it in the stream, is
 * what makes the fence mean "the GPU got this far". The old answer, mirroring
 * "submitted" into "completed" from the ack thread (xbox_Nv2aMirrorFence),
 * said the work was done before anything had read it. */
#define NV097_SET_CONTEXT_DMA_SEMAPHORE   0x01A4
#define NV097_SET_SEMAPHORE_OFFSET        0x1D6C
#define NV097_BACK_END_WRITE_SEMAPHORE_RELEASE 0x1D70

/* VertexAttr, Texture and the decoded state's layout: nv2a_pb_state.h. */
static struct nv2a_pb_gpu s_gpu;

/* Releases written, and where the last one landed. fence_mirrors_tick reads
 * them to decide whether its mirror is still needed; both run on the NV2A ack
 * thread, which is also the thread that drives this executor. */
static uint32_t s_inline_overflow;      /* INLINE_ARRAY dwords past NV_MAX_INLINE */
static uint32_t s_idx_overflow;         /* batches that lost indices past NV_MAX_INDICES */
static int      s_idx_cut;              /* the open batch has lost some */
static uint32_t s_idx_max;              /* largest idx_count drawn */
static uint32_t s_idx_over_ffff;        /* indices past 0xFFFF, truncated to 16 bits */
static uint32_t s_xv_fail;              /* program batches dropped: no scratch */

/* Indices past NV_MAX_INDICES are dropped, which cuts the far end off a big
 * mesh with no other sign: say so once, and count the batch at its END. */
static void note_idx_overflow(void)
{
    static int warned;
    s_idx_cut = 1;
    if (!warned++)
        fprintf(stderr, "  [GPU] index batch over %u indices: the rest is dropped\n",
                (unsigned)NV_MAX_INDICES);
}

/* For the backend smoke test: the counters the summary line prints. */
void nv2a_pb_exec_idx_stats(uint32_t *max, uint32_t *overflowed, uint32_t *over_ffff)
{
    if (max) *max = s_idx_max;
    if (overflowed) *overflowed = s_idx_overflow;
    if (over_ffff) *over_ffff = s_idx_over_ffff;
}

static uint32_t s_sema_releases;
static uint32_t s_sema_last_va, s_sema_last_value;

/* Visibility tests: D3DDevice_BeginVisibilityTest / EndVisibilityTest, which
 * are NV2A pixel-count reports. Begin sends CLEAR_REPORT_VALUE and
 * SET_ZPASS_PIXEL_COUNT_ENABLE(1). End sends ZPASS_PIXEL_COUNT_ENABLE(0) and
 * GET_REPORT(type 1 << 24 | offset): the GPU then writes a 16-byte report at
 * that offset in the report DMA object, {timestamp (8), count (4), status
 * (4)}. D3D writes 0xFFFFFFFF to the status first, and
 * GetVisibilityTestResult returns D3DERR_TESTINCOMPLETE until it changes.
 *
 * With no GET_REPORT the status never changed, so every test stayed
 * incomplete. A title can decide from these tests whether to draw the
 * player character, which then was not drawn at all on either backend.
 *
 * The count is xemu's: fragments that pass the depth, stencil and alpha
 * tests while counting is enabled. The CPU rasteriser counts them
 * (put_pixel). A backend that draws on a GPU cannot report a count to the
 * walker yet, so its tests report every test as visible (RECOMP_ZPASS_FIXED,
 * default 0x10000), the same as a GL occlusion query that saw the whole
 * object. The offset is inside the report DMA object, whose base and limit
 * are read from instance memory when SET_CONTEXT_DMA_REPORT binds it
 * (ctxdma_bind), as the semaphore's are. */
#define NV097_SET_CONTEXT_DMA_REPORT        0x01A8
#define NV097_CLEAR_REPORT_VALUE            0x17C8
#define NV097_SET_ZPASS_PIXEL_COUNT_ENABLE  0x17CC
#define NV097_GET_REPORT                    0x17D0
static uint32_t s_zpass_count;           /* since the last CLEAR_REPORT_VALUE */
static int      s_zpass_enable;

/* The rasteriser's per-pixel counters, when a worker thread draws rows of a
 * triangle (fast_rows_parallel): each thread counts into its own, merged
 * into s_gpu, s_vsh and s_zpass_count afterwards. NULL on the executor
 * thread outside that, where the globals are written directly. */
typedef struct {
    uint64_t pixels, z_rejected, a_rejected;
    uint32_t zpass, pixel_max;
    int      max_y;                     /* row pixel_max was set on, -1 none */
} RasterCount;
static NV_TLS RasterCount *t_cnt;
static uint32_t s_reports, s_reports_zero;
uint32_t nv2a_pb_exec_semaphore_releases(void) { return s_sema_releases; }
uint32_t nv2a_pb_exec_semaphore_last_va(void) { return s_sema_last_va; }
uint32_t nv2a_pb_exec_semaphore_last_value(void) { return s_sema_last_value; }

/* The flip hold (nv2a_flip_hold.h). The executor arms it at FLIP_STALL; the
 * ack loop in xbox_memory_layout.c polls it each pass and, while it holds,
 * walks nothing and leaves the KickOff flush unacknowledged. Both run on the
 * walker's thread, so the state needs no lock. */
static XboxFlipHold s_flip_hold;
static uint32_t s_vbl_count;        /* the vblank count last seen to move */
static uint64_t s_vbl_moved_ns;
/* The vblank count when the walk that reaches a FLIP_STALL began, which is
 * when the title kicked the segment holding its Swap: the title waits in
 * KickOff until the walker has caught up, so a walk starts within ~50 us of
 * the kick. Sampled at the FLIP_STALL instead, the segment's own draw time
 * (several ms on Metal) moved a frame the title swapped just before a
 * vblank past it; the title's next frame then counted as a second flip in
 * the same vblank and was held for most of a frame. */
static uint32_t s_walk_vbl;

void nv2a_pb_exec_walk_begin(void)
{
    s_walk_vbl = xbox_VblankCount();
}

/* RECOMP_DEBUG=flip_pacing: 0 off (flips complete at once, frame counters
 * bumped unconditionally, as before), edge (D3D's quantising rule), else on. */
int nv2a_pb_exec_flip_pacing(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = recomp_env(RENV_FLIP_PACING);
        mode = !e ? 1 : !strcmp(e, "0") ? 0 : !strcmp(e, "edge") ? 2 : 1;
        s_flip_hold.edge = mode == 2;
        if (e)
            fprintf(stderr, "  [GPU] flip pacing %s (RECOMP_DEBUG=flip_pacing=%s)\n",
                    mode == 0 ? "off" : mode == 2 ? "edge" : "on", e);
    }
    return mode;
}

/* The guest vblank count, and whether it is a running clock: vblanks are on
 * and the count moved within XBOX_FLIP_CLOCK_STALE_NS. Without it a hold
 * would only ever end on its timeout. */
static uint32_t flip_clock(uint64_t now_ns, int *ok)
{
    uint32_t c = xbox_VblankCount();
    if (c != s_vbl_count) {
        s_vbl_count = c;
        s_vbl_moved_ns = now_ns;
    }
    *ok = s_vbl_moved_ns && now_ns - s_vbl_moved_ns < XBOX_FLIP_CLOCK_STALE_NS;
    return c;
}

/* Once per pass of the ack loop: 1 while a flip is held. */
int nv2a_pb_exec_flip_poll(void)
{
    uint64_t now;
    uint32_t c;
    int ok;

    if (!s_flip_hold.held)
        return 0;
    now = xbox_HostNowNs();
    c = flip_clock(now, &ok);
    return xbox_FlipHoldPoll(&s_flip_hold, c, now, ok);
}

/* A walk that stopped short of PUT: whatever it was going to hold, the
 * title is out of step with it, so nothing waits on it. */
void nv2a_pb_exec_flip_cancel(void)
{
    xbox_FlipHoldCancel(&s_flip_hold, xbox_HostNowNs());
}

/* For the RECOMP_TRACE=pacing window line, then reset. */
void nv2a_pb_exec_flip_report(void)
{
    XboxFlipHold *h = &s_flip_hold;
    if (nv2a_pb_exec_flip_pacing() == 0)
        return;
    fprintf(stderr, "[PACING]   flip hold: holds %u held_ms %.1f hold_timeouts %u"
            " clock_stops %u iv0 %u iv1 %u iv2 %u iv3 %u nonop %u"
            " counters_owned %u\n", h->holds, h->held_ns / 1e6, h->timeouts,
            h->clock_stops, h->iv[0], h->iv[1], h->iv[2], h->iv[3], h->nonop,
            xbox_Nv2aFrameCountersOwned());
    h->holds = h->timeouts = h->clock_stops = h->nonop = 0;
    h->iv[0] = h->iv[1] = h->iv[2] = h->iv[3] = 0;
    h->held_ns = 0;
}

/* Switches come from recomp_env(): read once at startup, an array load per
 * lookup. The walker consults them per method, batch and triangle, and a
 * getenv there (a locked linear scan under Wine) was most of its time. */

/* Unhandled methods, ranked. The interesting output is not that something was
 * skipped but which things dominate, because that is the order to implement
 * them in. */
#define PB_EXEC_MAX_UNHANDLED 2048
typedef struct { uint32_t method, count, last_param; } PbUnhandled;
static PbUnhandled s_unhandled[PB_EXEC_MAX_UNHANDLED];
static int s_unhandled_count;

/* Every texture-stage register, as the title last set it.
 * Texturing is not implemented yet; knowing which formats and sizes a title
 * actually programs is what decides which ones are worth implementing. */
static uint32_t s_tex_reg[NV_TEX_REGS];
static uint8_t  s_tex_set[NV_TEX_REGS];

static void record_tex_reg(uint32_t method, uint32_t param)
{
    s_tex_reg[(method - NV_TEX_FIRST) / 4] = param;
    s_tex_set[(method - NV_TEX_FIRST) / 4] = 1;
    /* Resolved on write, not at sample time, as SET_TEXTURE_OFFSET is. */
    if (method == 0x1B20u)              /* stage 0 SET_TEXTURE_PALETTE */
        nv2a_tex_palette_decode(param, dma_resolve, &s_gpu.tex.palette,
                                &s_gpu.tex.pal_len);
    if (method == 0x1B24u)              /* stage 0 SET_TEXTURE_BORDER_COLOR */
        s_gpu.tex.border = param;
    /* A pitch is a linear texture's property. A swizzled one has no rows and
     * so no pitch, and requiring one here refused every swizzled texture --
     * which is nearly all of them, since swizzled is the Xbox default. That
     * left the title's own textures unsampled and every textured quad drawn in
     * flat vertex colour. */
    s_gpu.tex.valid = s_gpu.tex.offset && s_gpu.tex.width && s_gpu.tex.height
                   && (nv2a_tex_size_from_format(s_gpu.tex.color)
                       || s_gpu.tex.pitch);
}

/* Every distinct texture a batch was drawn with, and how many batches used it.
 *
 * The per-draw verbose print shows the first few draws of the first frame,
 * which is enough to see that texturing works at all and not enough to answer
 * "is a font page ever bound". This is the same shape as the unhandled-method
 * table below it: a small set, ranked, printed with the rest of the report. */
#define PB_EXEC_MAX_TEXTURES 64
typedef struct {
    uint32_t offset, color, width, height, batches;
} PbTexUse;
static PbTexUse s_tex_use[PB_EXEC_MAX_TEXTURES];
static int s_tex_use_count;

/* Defined below, next to the sampler it goes through. */
static void dump_texture_bmp(uint32_t seq);

/* Repeat dumps are numbered from well past the first-use sequence, so a
 * listing sorts them after the textures they came from and no first-use file
 * is ever overwritten by one. */
#define TEX_DUMP_SEQ_BASE 1000u
#define TEX_DUMP_SEQ_MAX  40u

static void note_texture_use(void)
{
    int i;

    if (!s_gpu.tex.valid)
        return;
    for (i = 0; i < s_tex_use_count; i++) {
        if (s_tex_use[i].offset == s_gpu.tex.offset
         && s_tex_use[i].color  == s_gpu.tex.color) {
            s_tex_use[i].batches++;
            /* Dump a surface that is redrawn, every Nth time it is bound.
             *
             * First use alone cannot tell a decode error that is wrong in
             * every frame from one that accumulates across them. A block
             * transform that is wrong is wrong on its own, in the keyframe
             * as much as anywhere; motion compensation that is wrong starts
             * from a clean keyframe and smears further with each predicted
             * frame after it. In a single frame the two look identical, and
             * in a sequence they look nothing alike -- so the sequence is
             * what has to be captured.
             *
             * It belongs on this side of the return: a video surface keeps
             * one address for the whole film, so after the first frame it is
             * only ever found here, and the first-use dump below never fires
             * for it again. RECOMP_TEX_DUMP_EVERY=<n> sets the interval, and
             * RECOMP_TEX_DUMP still names the files. */
            {
                static int every = -1;
                static unsigned binds, seq;
                if (every < 0) {
                    const char *e = recomp_env(RENV_TEX_DUMP_EVERY);
                    every = e ? atoi(e) : 0;
                }
                if (every > 0 && ++binds % (unsigned)every == 0
                    && seq < TEX_DUMP_SEQ_MAX)
                    dump_texture_bmp(TEX_DUMP_SEQ_BASE + seq++);
            }
            return;
        }
    }
    if (s_tex_use_count < PB_EXEC_MAX_TEXTURES) {
        s_tex_use[s_tex_use_count].offset  = s_gpu.tex.offset;
        s_tex_use[s_tex_use_count].color   = s_gpu.tex.color;
        s_tex_use[s_tex_use_count].width   = s_gpu.tex.width;
        s_tex_use[s_tex_use_count].height  = s_gpu.tex.height;
        s_tex_use[s_tex_use_count].batches = 1;
        s_tex_use_count++;
        dump_texture_bmp((uint32_t)s_tex_use_count - 1);
    }
}

/* s_unhandled's slot for each method, plus one (0: not seen yet). Methods
 * are dword offsets below 0x2000, so method / 4 indexes it; the table itself
 * keeps first-seen order, which is what the report ranks from. A linear
 * search here ran for every unhandled method the title sends, tens of
 * thousands a frame. */
static uint16_t s_unhandled_slot[0x2000 / 4];

static void note_unhandled(uint32_t method, uint32_t param)
{
    int fits = method < 0x2000 && !(method & 3), i;

    s_gpu.unhandled_total++;
    if (fits) {
        i = (int)s_unhandled_slot[method >> 2] - 1;
        if (i >= 0) {
            s_unhandled[i].count++;
            s_unhandled[i].last_param = param;
            return;
        }
    } else {
        /* An incrementing run carried past 0x1FFC: rare, searched. */
        for (i = 0; i < s_unhandled_count; i++) {
            if (s_unhandled[i].method == method) {
                s_unhandled[i].count++;
                s_unhandled[i].last_param = param;
                return;
            }
        }
    }
    if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
        s_unhandled[s_unhandled_count].method = method;
        s_unhandled[s_unhandled_count].count = 1;
        s_unhandled[s_unhandled_count].last_param = param;
        s_unhandled_count++;
        if (fits)
            s_unhandled_slot[method >> 2] = (uint16_t)s_unhandled_count;
    }
}

/* Read attribute `a` of vertex `index` as floats; 0, and (0,0,0,1), for a
 * type with no decoder or a vertex past the array. */
static int fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    /* Size 0 is the only inline state. Stride 0 is still an array: every
     * vertex reads the same element (a per-batch constant from memory).
     * SSX 3's rider passes its mesh offset in v4 this way; reading the
     * inline value instead picked up another program's v4 and threw the
     * body off screen. */
    if (!a->size)
        return 0;
    if (s_gpu.inline_active) {
        /* The batch arrived as INLINE_ARRAY, so `offset` is a byte offset into
         * the buffered payload rather than a guest address -- and 0 is a legal
         * one there, which is why the offset test is on the other side of this
         * branch. */
        size_t at = (size_t)a->offset + (size_t)index * a->stride;
        if (at + 4 > (size_t)s_gpu.inline_count * 4)
            return 0;
        p = (const uint8_t *)s_gpu.inline_buf + at;
    } else {
        if (!a->offset)
            return 0;
        p = mem + a->offset + (size_t)index * a->stride;
    }

    return nv2a_vtx_decode(a->type, a->size, p, out);
}

static uint32_t surf_fmt_bpp(uint32_t format);

static uint32_t surface_bpp(void)
{
    /* The surface format says it outright (SET_SURFACE_FORMAT's colour
     * field). Pitch / clip width -- the only way before -- is right only
     * while the clip spans the whole surface: Burnout 3 narrows the clip to
     * a 250-pixel window for its option values, 2560 / 250 came out as 10
     * bytes a pixel, and every such text quad was refused (upstream
     * 777c1cd). The pitch rule stays as the fallback for a colour code the
     * table does not know. */
    uint32_t bpp = surf_fmt_bpp(s_gpu.format);
    if (bpp)
        return bpp;
    if (!s_gpu.clip_w)
        return 0;
    return s_gpu.pitch / s_gpu.clip_w;
}


/* Write w x h pixels of a surface at host address `base` as a 24-bit BMP.
 * `swz` is a swizzled target's log2 width/height pair (0 for a pitch one). */
static int write_bmp_at(const char *path, const uint8_t *base, uint32_t pitch,
                        uint32_t w, uint32_t h, uint32_t bpp, uint32_t swz)
{
    uint32_t row_bytes = w * 3, pad = (4 - (row_bytes & 3)) & 3, y, x;
    uint32_t filesz = 54 + (row_bytes + pad) * h, mx = 0, my = 0;
    uint8_t hdr[54];
    FILE *f = fopen(path, "wb");

    if (!f)
        return 0;
    if (swz)
        xbox_swizzle_masks(1u << (swz & 0xFF), 1u << (swz >> 8), &mx, &my);
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = h; y-- > 0; ) {
        for (x = 0; x < w; x++) {
            uint8_t bgr[3];
            size_t i = swz ? (size_t)(swizzle_deposit(x, mx) | swizzle_deposit(y, my))
                           : (size_t)y * (pitch / bpp) + x;
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)base)[i];
                bgr[0] = (uint8_t)v; bgr[1] = (uint8_t)(v >> 8); bgr[2] = (uint8_t)(v >> 16);
            } else {
                uint16_t v = ((const uint16_t *)base)[i];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    return 1;
}

/* ── Present surface ─────────────────────────────────────────────────────
 *
 * Which colour surface a flip puts on screen, decided here for every backend
 * (nv2a_pb_present_state() in nv2a_pb_state.h).
 *
 * Not color_offset: by FLIP_STALL the XDK has already pointed that at the
 * next back buffer. Not the last surface drawn into either: a title renders
 * glow, shadow and downsample targets too, sometimes last -- one title
 * ends each frame in a 320x240 downsample, and presenting that put
 * an inset on screen (D3D11) or dumped the wrong buffer (CPU).
 *
 * The rule is size first. A surface is the colour offset plus the size the
 * clip gives it, as the draws and clears that used it saw it. The size wanted
 * is the display's: that of the surfaces the title scans out -- the one at
 * the address AvSetDisplayMode set and those the last present_stale flips
 * presented (the largest, if the title used them at more than one size) --
 * or, with no display mode yet, of the largest surface seen; and the pitch is
 * the display's too, once there is a display mode. A run of draws with the
 * clip at 640x480 but offset and pitch still those of the 320x240 target
 * (seen on the odd frame) is 640x480 but nothing the display could
 * scan out. Of the surfaces that size, the one drawn into most recently
 * since the last flip wins; if none
 * was, the one cleared most recently; if none was either, the frame on
 * screen stays. Nothing carries over from one flip to the next, so a stray
 * flip with an odd target bound decides that flip and no other.
 *
 * Three guards on that rule:
 *
 *  - The display address and a surface's offset are compared as physical
 *    addresses (low 28 bits). AvSetDisplayMode resolves the address it is
 *    given by its own rule (kernel_bridge.c: XBOX_CONTIG_BASE + fb whenever
 *    fb is below 64 MB), dma_resolve by another (the bare offset above the
 *    arena), and the two must not have to agree for the pick to work.
 *
 *  - Only surfaces the display could scan out set the size: the pitch test
 *    applies to the size scan too, and a surface not drawn or cleared for
 *    present_stale flips (RECOMP_DEBUG, default 120) is stale. At the display address the largest
 *    fresh surface sets the size; when only stale ones sit there, the
 *    largest of those still does, so no candidate qualifies and the screen
 *    stays (a pause screen, or a smaller surface drawn elsewhere while the
 *    back buffers idle, cannot take over). Only when nothing at all sits at
 *    the display address does the largest fresh surface anywhere set it.
 *    Without the staleness test an entry left from an earlier, larger display
 *    mode at a reused address would pin the size for the rest of the run, no
 *    surface of that size is ever drawn again, and the screen freezes on the
 *    last pick before the mode change.
 *
 *  - Field rendering (D3DPRESENTFLAG_FIELD): half-height back buffers at
 *    twice the display's pitch. When no surface since the last display mode
 *    set has had the display's own pitch (once seen, s_pitch_seen latches it,
 *    stale or later evicted; a mode set clears it, since buffers drawn
 *    before it say nothing about the new mode), a surface at twice it
 *    qualifies instead, with a warning the first time. The runtime does not interleave fields anywhere, so such a
 *    pick shows one field; it beats presenting the buffer about to be drawn.
 *
 * The scanout set. A title that flips through the pushbuffer leaves the
 * display address where AvSetDisplayMode put it and alternates its back
 * buffers by PCRTC_START, which nothing here sees; with only the display
 * address in the size scan, that one buffer had to carry every size alone.
 * One title records each back buffer at three clips a frame (640x480 for a
 * full-screen composite, 640x464 for the scene, 640x463 for its race-start
 * card) and touches 13 offscreen targets besides: 19 entries in a 16-entry
 * table, so a frame into the other buffer evicted the display buffer's
 * 640x480 entry and the size alternated 480/464 flip by flip, which every
 * scaling present showed as the picture jumping at 60 Hz. So the offsets
 * the last present_stale flips presented (s_scanout, as physical addresses)
 * set the size too (only picks sized from the display address or the set
 * join it: a pass-1 pick is just the largest surface anywhere), the set empties at a display-mode change as the field
 * latch does, and when the table is full eviction takes an entry that is
 * neither at a scanout offset nor at the display address (at the display or
 * field pitch) first, plain LRU only once every entry is one. The table
 * holds 32, so that frame fits outright; the protection is for a title with
 * more targets than that. Which surface of the chosen size is presented is
 * unchanged: the newest draw, as above. The set widens an old quirk of the
 * size-first rule: a fresh larger entry at the display pitch held the
 * screen when it sat at the display address, and now does at any offset in
 * the set too, until it goes stale.
 *
 * The extent. All of the above decides which surface a flip shows; the size
 * it shows is the display mode's. AvSetDisplayMode sets one timing, and the
 * CRTC scans that many lines from PCRTC_START whatever rows the title drew,
 * so one title's 640x464 menus are 640x480 frames on the TV and its races are
 * too: presenting the clip instead made every menu-to-race change a size step
 * under any scaling present. So a pick at the display pitch publishes the
 * mode's frame (xbox_GetDisplayMode), one at twice it (fields) half the
 * frame's lines, and only without a known mode size, or at another pitch,
 * the entry's clip extent. The clip still ranks the candidates: it is what
 * tells a back buffer from an odd target at the same offset. */
#define PRESENT_MAX 32
#define SCANOUT_MAX 8
/* The staleness window in flips: RECOMP_DEBUG=present_stale=n, default 120
 * (two seconds at 60 Hz: longer than a pause screen's idle back buffers
 * sit untouched, shorter than a mode change's leftovers should linger). */
static uint32_t present_stale_flips(void)
{
    static long n = -1;
    if (n < 0) {
        n = recomp_env_int(RENV_PRESENT_STALE, 120);
        if (n < 1) n = 1;
    }
    return (uint32_t)n;
}
static struct {
    uint32_t offset, w, h, pitch, format;
    uint32_t last_flip;                  /* s_gpu.flips at its last use */
    uint64_t last_use, last_draw, last_clear;
} s_psurf[PRESENT_MAX];
static uint64_t s_psurf_seq, s_psurf_flip_seq;
static struct nv2a_pb_present s_present;

/* The scanout set: the physical form of each offset a flip presented (the
 * compare the display address uses) and the flip that last presented it.
 * Fed by present_pick with the picks sized from the display address or the
 * set itself, never a pass-1 pick; an entry not presented for present_stale flips no
 * longer counts, and a display-mode change (s_scanout_gen) empties it.
 * phys 0 marks an empty slot: physical page 0 is never a framebuffer. */
static struct { uint32_t phys, last_flip; } s_scanout[SCANOUT_MAX];
static uint32_t s_scanout_gen;
static uint32_t s_present_want;     /* the pitch the last pick wanted, 0 without a mode */

/* RECOMP_PRESENT_TRACE=1: the order this frame used its surfaces in (runs of
 * draws and clears per surface), printed at a flip whose pick is not the
 * surface picked two flips before -- a double-buffered title alternates, so
 * that is the flip worth explaining. With it, the scanout set at that flip,
 * the first evictions the scanout protection decided, and every 1000 flips
 * and at exit how often it did: whether the protection was ever the deciding
 * factor in a run. */
#define PTRACE_MAX 48
static struct { uint32_t offset, w, h, pitch, draws, clears; } s_ptrace[PTRACE_MAX];
static int s_ptrace_n, s_ptrace_on = -1;
static uint32_t s_evict_n, s_evict_spared, s_evict_protected;

static void present_trace_line(const char *why)
{
    fprintf(stderr, "[PRESENT] summary (%s): %u flips, %u evictions, %u spared a"
            " scanout entry, %u with all %d entries protected (plain LRU)\n",
            why, s_gpu.flips, s_evict_n, s_evict_spared, s_evict_protected,
            PRESENT_MAX);
    fflush(stderr);
}

static void present_trace_summary(const char *why)
{
    static int done;
    if (!__atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL))
        present_trace_line(why);
}

/* For the backend smoke test (declared there): the eviction counters the
 * summary prints. Counted with or without the trace. */
void nv2a_pb_exec_present_stats(uint32_t *evictions, uint32_t *spared, uint32_t *protected_)
{
    if (evictions) *evictions = s_evict_n;
    if (spared) *spared = s_evict_spared;
    if (protected_) *protected_ = s_evict_protected;
}

static void present_trace_at_exit(void)
{
    present_trace_summary("exit");
}

/* A mode set says nothing about the buffers presented before it (the reason
 * the field latch resets there too). */
static void scanout_sync_gen(uint32_t gen)
{
    if (gen != s_scanout_gen) {
        s_scanout_gen = gen;
        memset(s_scanout, 0, sizeof s_scanout);
    }
}

/* In use (phys 0 is an empty slot) and presented within present_stale
 * flips, with the same <= as present_fresh. */
static int scanout_live(int k)
{
    return s_scanout[k].phys
        && s_gpu.flips - s_scanout[k].last_flip <= present_stale_flips();
}

/* Whether physical address `phys` is the display's (disp_phys, 0 without a
 * mode) or a live scanout entry. */
static int present_scanout(uint32_t phys, uint32_t disp_phys)
{
    int k;
    if (disp_phys && phys == disp_phys)
        return 1;
    for (k = 0; k < SCANOUT_MAX; k++)
        if (s_scanout[k].phys == phys && scanout_live(k))
            return 1;
    return 0;
}

/* The table is full and s_psurf[lru] is the plain LRU victim: take the LRU
 * of the entries the display could not be scanning out instead. Protected is
 * at the display address or a scanout offset and at the display pitch, or at
 * the field pitch while the last pick wanted it, so a stray entry at a back
 * buffer's offset with an offscreen pitch is not. The display pitch stays
 * protected under the field latch: the first pick after every mode set wants
 * the field pitch (no surface has had the display's yet), and the frame
 * after it draws the display buffer at the display pitch. Without a display
 * mode nothing is protected: the pick then never consults the scanout set.
 * Plain LRU when every entry is protected, so the table never starves. Only
 * on this path is dma_resolve paid per entry. */
static int present_victim(int lru)
{
    uint32_t disp_pitch = 0, disp = xbox_GetDisplayFramebuffer(&disp_pitch);
    uint32_t disp_phys = disp & 0x0FFFFFFFu;
    int i, v = -1;

    if (!disp) {
        s_evict_n++;
        return lru;
    }
    scanout_sync_gen(xbox_DisplayModeGeneration());
    for (i = 0; i < PRESENT_MAX; i++) {
        uint32_t pitch = s_psurf[i].pitch;
        if ((pitch == disp_pitch
                    || (s_present_want == disp_pitch << 1 && pitch == s_present_want))
                && present_scanout(dma_resolve(s_psurf[i].offset) & 0x0FFFFFFFu,
                                   disp_phys))
            continue;
        if (v < 0 || s_psurf[i].last_use < s_psurf[v].last_use)
            v = i;
    }
    s_evict_n++;
    if (v == lru)
        return v;
    if (v >= 0)
        s_evict_spared++;
    else
        s_evict_protected++;
    if (s_ptrace_on > 0 && s_evict_spared + s_evict_protected <= 20)
        fprintf(stderr, "[PRESENT] flip %u: table full, %s 0x%08X %ux%u pitch %u"
                " (LRU 0x%08X %ux%u pitch %u)\n", s_gpu.flips,
                v >= 0 ? "spared a scanout entry, evicting" : "all protected, evicting",
                s_psurf[v >= 0 ? v : lru].offset, s_psurf[v >= 0 ? v : lru].w,
                s_psurf[v >= 0 ? v : lru].h, s_psurf[v >= 0 ? v : lru].pitch,
                s_psurf[lru].offset, s_psurf[lru].w, s_psurf[lru].h,
                s_psurf[lru].pitch);
    return v >= 0 ? v : lru;
}

/* After a pick: add or refresh its offset, else take an empty slot (phys
 * 0), evicting the entry presented longest ago when the set is full. */
static void scanout_note(uint32_t offset)
{
    uint32_t phys = dma_resolve(offset) & 0x0FFFFFFFu;
    int k, slot = -1;

    for (k = 0; k < SCANOUT_MAX && slot < 0; k++)
        if (s_scanout[k].phys == phys)
            slot = k;
    for (k = 0; k < SCANOUT_MAX && slot < 0; k++)
        if (!s_scanout[k].phys)
            slot = k;
    if (slot < 0) {
        slot = 0;
        for (k = 1; k < SCANOUT_MAX; k++)
            if (s_gpu.flips - s_scanout[k].last_flip
                    > s_gpu.flips - s_scanout[slot].last_flip)
                slot = k;
    }
    s_scanout[slot].phys = phys;
    s_scanout[slot].last_flip = s_gpu.flips;
}

/* A draw (clear = 0) or colour clear (clear = 1) into the current surface. */
static void present_note(int clear)
{
    uint32_t w = s_gpu.clip_x + s_gpu.clip_w, h = s_gpu.clip_y + s_gpu.clip_h;
    int i, e = -1, lru = 0;

    if (!s_gpu.color_offset || !w || !h)
        return;
    for (i = 0; i < PRESENT_MAX; i++) {
        if (s_psurf[i].offset == s_gpu.color_offset
                && s_psurf[i].w == w && s_psurf[i].h == h) {
            e = i;
            break;
        }
        if (s_psurf[i].last_use < s_psurf[lru].last_use)
            lru = i;
    }
    if (e < 0) {
        /* A free slot has last_use 0, so lru is one while any is left. */
        e = s_psurf[lru].offset ? present_victim(lru) : lru;
        memset(&s_psurf[e], 0, sizeof s_psurf[e]);
        s_psurf[e].offset = s_gpu.color_offset;
        s_psurf[e].w = w;
        s_psurf[e].h = h;
    }
    s_psurf[e].pitch = s_gpu.pitch;
    s_psurf[e].format = s_gpu.format;
    s_psurf[e].last_flip = s_gpu.flips;
    s_psurf[e].last_use = ++s_psurf_seq;
    if (s_ptrace_on < 0) {
        s_ptrace_on = recomp_env(RENV_PRESENT_TRACE) != NULL;
        if (s_ptrace_on) {
            atexit(present_trace_at_exit);
            recomp_exit_hook_add(present_trace_summary);   /* the windows' ExitProcess */
        }
    }
    if (s_ptrace_on) {
        int n = s_ptrace_n;
        if (!n || s_ptrace[n - 1].offset != s_gpu.color_offset
                || s_ptrace[n - 1].w != w || s_ptrace[n - 1].h != h
                || s_ptrace[n - 1].pitch != s_gpu.pitch) {
            if (n == PTRACE_MAX)
                n = PTRACE_MAX - 1;      /* keep the tail: the end of the frame */
            else
                s_ptrace_n++;
            s_ptrace[n].offset = s_gpu.color_offset;
            s_ptrace[n].w = w; s_ptrace[n].h = h; s_ptrace[n].pitch = s_gpu.pitch;
            s_ptrace[n].draws = s_ptrace[n].clears = 0;
        }
        if (clear)
            s_ptrace[s_ptrace_n - 1].clears++;
        else
            s_ptrace[s_ptrace_n - 1].draws++;
    }
    if (clear)
        s_psurf[e].last_clear = s_psurf_seq;
    else
        s_psurf[e].last_draw = s_psurf_seq;
}

/* Whether s_psurf[i] is in use and (with a display mode) at the pitch
 * `want` -- the display's, or twice it for fields. */
static int present_pitch_ok(int i, uint32_t want)
{
    return s_psurf[i].offset && (!want || s_psurf[i].pitch == want);
}

/* Drawn or cleared within the last present_stale flips. */
static int present_fresh(int i)
{
    return s_gpu.flips - s_psurf[i].last_flip <= present_stale_flips();
}

/* At FLIP_STALL: pick what it presents, into s_present. */
static void present_pick(void)
{
    static int warned_field;
    static uint32_t s_pitch_seen;        /* a display pitch some surface had */
    static uint32_t s_mode_gen, s_mode_flip;
    uint32_t gen = xbox_DisplayModeGeneration();
    uint32_t disp_pitch = 0, disp_w = 0, disp_h = 0;
    uint32_t disp = xbox_GetDisplayMode(&disp_pitch, &disp_w, &disp_h);
    uint32_t tw = 0, th = 0, want = disp_pitch, ew = 0, eh = 0;
    uint64_t area = 0, stale_area = 0;
    uint32_t stale_w = 0, stale_h = 0;
    int i, pass, at_disp, best = -1, rule = 0;

    /* Field pitch only if no surface since the last mode set has had the
     * display's own. */
    if (gen != s_mode_gen) {
        s_mode_gen = gen;
        s_mode_flip = s_gpu.flips;
        s_pitch_seen = 0;
    }
    scanout_sync_gen(gen);
    for (i = 0; i < SCANOUT_MAX; i++)
        if (!scanout_live(i))
            s_scanout[i].phys = 0;
    for (i = 0; i < PRESENT_MAX && disp_pitch && s_pitch_seen != disp_pitch; i++)
        if (s_psurf[i].offset && s_psurf[i].pitch == disp_pitch
                && s_psurf[i].last_flip >= s_mode_flip)
            s_pitch_seen = disp_pitch;
    if (disp_pitch && s_pitch_seen != disp_pitch)
        want = disp_pitch << 1;

    s_present_want = disp ? want : 0;

    /* Size, pass 0: the largest fresh surface at the display address or a
     * scanout offset, else the largest stale one there (which leaves no
     * candidate). */
    for (i = 0; disp && i < PRESENT_MAX; i++) {
        uint64_t a = (uint64_t)s_psurf[i].w * s_psurf[i].h;
        if (!present_pitch_ok(i, want)
                || !present_scanout(dma_resolve(s_psurf[i].offset) & 0x0FFFFFFFu,
                                    disp & 0x0FFFFFFFu))
            continue;
        if (present_fresh(i)) {
            if (a > area) {
                area = a;
                tw = s_psurf[i].w;
                th = s_psurf[i].h;
            }
        } else if (a > stale_area) {
            stale_area = a;
            stale_w = s_psurf[i].w;
            stale_h = s_psurf[i].h;
        }
    }
    at_disp = area || stale_area;
    if (!area && stale_area) {
        area = stale_area;
        tw = stale_w;
        th = stale_h;
    }
    /* Pass 1, only with nothing at the display address or a scanout offset
     * (always, without a display mode): the largest fresh surface anywhere. */
    for (i = 0; !at_disp && i < PRESENT_MAX; i++) {
        uint64_t a = (uint64_t)s_psurf[i].w * s_psurf[i].h;
        if (a > area && present_pitch_ok(i, want) && present_fresh(i)) {
            area = a;
            tw = s_psurf[i].w;
            th = s_psurf[i].h;
        }
    }
    if (area && disp_pitch && want != disp_pitch && !warned_field) {
        warned_field = 1;
        fprintf(stderr, "  [GPU] present: no surface since display mode set %u"
                " (flip %u) has the display pitch %u; presenting %ux%u at pitch"
                " %u (field rendering?) -- one field only, fields are not"
                " interleaved\n", s_mode_gen, s_mode_flip, disp_pitch, tw, th,
                want);
    }
    /* Of that size and pitch: the newest draw since the last flip, else the
     * newest clear. */
    for (pass = 0; pass < 2 && area && best < 0; pass++) {
        uint64_t top = s_psurf_flip_seq;
        for (i = 0; i < PRESENT_MAX; i++) {
            uint64_t when = pass == 0 ? s_psurf[i].last_draw : s_psurf[i].last_clear;
            if (s_psurf[i].w == tw && s_psurf[i].h == th
                    && present_pitch_ok(i, want) && when > top) {
                top = when;
                best = i;
            }
        }
    }
    /* The extent the pick publishes: the display mode's frame, as the CRTC
     * scans it, whatever clip the title drew the buffer with (rule 1); one
     * field of it for a buffer at twice the display pitch (rule 2); the
     * entry's clip without a known mode size, at any other pitch, or for a
     * pass-1 pick (rule 3). A pass-1 pick is not at the display address or
     * a scanout offset, so nothing says the mode's frame is there: an
     * offscreen target at the display pitch grown to it would claim memory
     * past its own buffer. */
    if (best >= 0) {
        if (at_disp && disp_w && s_psurf[best].pitch == disp_pitch) {
            ew = disp_w;
            eh = disp_h;
            rule = 1;
        } else if (at_disp && disp_w && s_psurf[best].pitch == disp_pitch << 1) {
            ew = disp_w;
            eh = disp_h / 2;
            rule = 2;
        } else {
            ew = s_psurf[best].w;
            eh = s_psurf[best].h;
            rule = 3;
        }
    }
    if (s_ptrace_on > 0) {
        static uint32_t pick[2];
        uint32_t now = best >= 0 ? s_psurf[best].offset : s_present.offset;
        if (pick[0] && now != pick[0]) {
            if (best >= 0)
                fprintf(stderr, "[PRESENT] flip %u picks 0x%08X (2 flips ago 0x%08X),"
                        " size %ux%u at 0x%08X, mode %ux%u: extent %ux%u (rule %d);"
                        " this frame's surfaces:\n",
                        s_gpu.flips, now, pick[0], tw, th, disp, disp_w, disp_h,
                        ew, eh, rule);
            else
                fprintf(stderr, "[PRESENT] flip %u holds 0x%08X %ux%u (2 flips ago"
                        " 0x%08X), mode %ux%u at 0x%08X: held, nothing drawn since the"
                        " last flip; this frame's surfaces:\n",
                        s_gpu.flips, now, s_present.w, s_present.h, pick[0],
                        disp_w, disp_h, disp);
            for (i = 0; i < s_ptrace_n; i++)
                fprintf(stderr, "[PRESENT]   0x%08X %ux%u pitch %u: %u draws %u clears\n",
                        s_ptrace[i].offset, s_ptrace[i].w, s_ptrace[i].h,
                        s_ptrace[i].pitch, s_ptrace[i].draws, s_ptrace[i].clears);
            fprintf(stderr, "[PRESENT]   scanout set (phys, last flip):");
            for (i = 0; i < SCANOUT_MAX; i++)
                if (s_scanout[i].phys)
                    fprintf(stderr, " 0x%08X@%u", s_scanout[i].phys,
                            s_scanout[i].last_flip);
            fputc('\n', stderr);
        }
        pick[0] = pick[1];
        pick[1] = now;
        s_ptrace_n = 0;
        /* Benches and goldens stop the game with SIGINT, which never reaches
         * atexit, so the counts also print as the run goes. */
        if (s_gpu.flips % 1000 == 0)
            present_trace_line("running");
    }
    if (best >= 0) {
        /* One assignment, so the pick is never half old, half new (see the
         * thread contract in nv2a_pb_state.h). */
        struct nv2a_pb_present pick = {
            s_psurf[best].offset, ew, eh,
            s_psurf[best].pitch, s_psurf[best].format,
        };
        s_present = pick;
        /* Only a pick sized from the display address or the set joins the
         * set: a pass-1 pick is whatever surface was largest, and once in
         * the set it would count as scanout, take the mode's frame (rule 1)
         * and be grown past its own buffer. */
        if (at_disp)
            scanout_note(pick.offset);
    }
    s_psurf_flip_seq = s_psurf_seq;
}

/* RECOMP_FB_DUMP at a flip: the surface that flip presents, at its own size
 * and pitch, as <prefix>NNN.bmp in the same sequence as dump_surface_bmp. */
static int s_bmp_seq;
static int write_present_bmp(const char *path)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = s_present.w ? s_present.pitch / s_present.w : 0;
    const struct nv2a_pb_backend *be;

    if (!s_present.offset || (bpp != 2 && bpp != 4))
        return 0;
    /* The backend makes guest memory current first (sync_guest). */
    be = nv2a_pb_get_backend();
    if (be->sync_guest)
        be->sync_guest(s_present.offset, s_present.pitch, s_present.w, s_present.h);
    return write_bmp_at(path, mem + dma_resolve(s_present.offset), s_present.pitch,
                        s_present.w, s_present.h, bpp, 0);
}

static void dump_present_bmp(void)
{
    const char *prefix = recomp_env(RENV_FB_DUMP);
    uint32_t bpp = s_present.w ? s_present.pitch / s_present.w : 0;
    char path[512];

    /* Check the format before taking a number, so the numbering has no gaps. */
    if (!prefix || !s_present.offset || (bpp != 2 && bpp != 4))
        return;
    snprintf(path, sizeof path, "%s%03d.bmp", prefix, s_bmp_seq++);
    if (write_present_bmp(path) && s_bmp_seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X)\n",
                path, s_present.w, s_present.h, s_present.offset);
}

/* RECOMP_FB_DUMP_AT=<flips>: the frame each listed flip presents, as
 * <prefix>flip_NNNNN.bmp named by the flip number (the number
 * RECOMP_FLIP_LOG prints), whatever else dumps in between. <flips> is a comma
 * list of numbers and ranges (a-b), up to DUMP_AT_MAX items; the rest are
 * counted and reported, not dumped. Flip N is the frame
 * RECOMP_D3D11_DUMP writes as present N, so a golden-frame script can ask for
 * dump k as flip 60k+1, or for a window around an event. The CPU path and
 * backends that write back (Metal) are dumped here, from guest memory, with
 * the RECOMP_FB_DUMP prefix; a backend that presents from its own surfaces
 * (D3D11) dumps the listed flips itself (nv2a_pb_dump_at_listed). */
#define DUMP_AT_MAX 256
static int s_dump_at_n = -1;
static uint32_t s_dump_at_lo[DUMP_AT_MAX], s_dump_at_hi[DUMP_AT_MAX];

static void dump_at_parse(void)
{
    const char *e = recomp_env(RENV_FB_DUMP_AT);
    int n = 0, dropped = 0;

    while (e && *e) {
        char *end;
        unsigned long a = strtoul(e, &end, 10), b = a;
        if (end == e)
            break;
        if (*end == '-') {
            e = end + 1;
            b = strtoul(e, &end, 10);
            if (end == e)
                break;
        }
        if (b >= a && n == DUMP_AT_MAX) {
            dropped++;
        } else if (b >= a) {
            s_dump_at_lo[n] = (uint32_t)a;
            s_dump_at_hi[n] = (uint32_t)b;
            n++;
        }
        e = *end == ',' ? end + 1 : end;
        if (*end != ',')
            break;
    }
    s_dump_at_n = n;
    if (dropped)
        fprintf(stderr, "  [GPU] fb_dump_at: %d ranges past the first %d ignored\n",
                dropped, DUMP_AT_MAX);
}

/* RECOMP_DEBUG=fb_dump_prog (nv2a_fb_ring.h): armed at the first flip,
 * fired by the first draw of a listed program (fb_prog_draw), which makes
 * flips lo..hi dump as if fb_dump_at listed them and has the ring written
 * at the next flip. */
static struct {
    int state;          /* 0 unread, 1 armed, 2 fired (ring to write), 3 done, -1 off */
    int here;           /* the walker dumps (CPU, write-back backend) */
    struct nv2a_vsh_cdump_cfg cfg;
    uint32_t after, lo, hi;
    struct nv2a_fb_ring ring;
} s_fbp;

static void fb_prog_init(void);

/* Armed: keep the frame this flip presents, read back as write_present_bmp
 * reads it. */
static void fb_prog_keep(uint32_t flip)
{
    static int warned;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = s_present.w ? s_present.pitch / s_present.w : 0;
    const struct nv2a_pb_backend *be;

    if (!s_present.offset || (bpp != 2 && bpp != 4))
        return;
    be = nv2a_pb_get_backend();
    if (be->sync_guest)
        be->sync_guest(s_present.offset, s_present.pitch, s_present.w, s_present.h);
    if (nv2a_fb_ring_push(&s_fbp.ring, flip, mem + dma_resolve(s_present.offset),
                          s_present.pitch, s_present.w, s_present.h, bpp)
            && !warned) {
        warned = 1;
        fprintf(stderr, "  [GPU] fb_dump_prog: no memory for flip %u's frame;"
                        " the ring has a gap\n", flip);
    }
}

/* Fired: the flips before the trigger, oldest first, then the ring goes. */
static void fb_prog_write_ring(const char *prefix)
{
    uint32_t i;
    char path[512];

    for (i = 0; prefix && i < s_fbp.ring.count; i++) {
        const struct nv2a_fb_frame *f = nv2a_fb_ring_get(&s_fbp.ring, i);
        snprintf(path, sizeof path, "%sflip_%05u.bmp", prefix, f->flip);
        if (!write_bmp_at(path, f->px, f->pitch, f->w, f->h, f->bpp, 0))
            fprintf(stderr, "  [GPU] flip %u: fb_dump_prog dump failed\n", f->flip);
    }
    nv2a_fb_ring_free(&s_fbp.ring);
}

int nv2a_pb_dump_at_listed(uint32_t flip)
{
    int i;

    if (s_dump_at_n < 0)
        dump_at_parse();
    for (i = 0; i < s_dump_at_n; i++)
        if (flip >= s_dump_at_lo[i] && flip <= s_dump_at_hi[i])
            return 1;
    return s_fbp.state >= 2 && flip >= s_fbp.lo && flip <= s_fbp.hi;
}

static void dump_at_flip(uint32_t flip)
{
    static int checked;
    static int here;    /* this path dumps (CPU, write-back backend) */
    const char *prefix = recomp_env(RENV_FB_DUMP);
    char path[512];

    if (!checked) {
        const struct nv2a_pb_backend *be = nv2a_pb_get_backend();
        checked = 1;
        if (s_dump_at_n < 0)
            dump_at_parse();
        here = be == nv2a_pb_cpu_backend() || be->writes_back;
        if (s_dump_at_n) {
            fprintf(stderr, "  [GPU] fb_dump_at: %d flip ranges%s\n", s_dump_at_n,
                    here ? "" : ", dumped by the backend");
            if (here && !prefix)
                fprintf(stderr, "  [GPU] WARNING: fb_dump_at is set without"
                        " fb_dump; no flips will be dumped\n");
        }
    }
    if (s_fbp.state == 0)
        fb_prog_init();
    if (s_fbp.state == 1 && s_fbp.ring.size)
        fb_prog_keep(flip);
    else if (s_fbp.state == 2) {
        fb_prog_write_ring(here ? prefix : NULL);
        s_fbp.state = 3;
    }
    if (!here || !prefix || !nv2a_pb_dump_at_listed(flip))
        return;
    snprintf(path, sizeof path, "%sflip_%05u.bmp", prefix, flip);
    if (!write_present_bmp(path))
        fprintf(stderr, "  [GPU] flip %u: fb_dump_at dump failed\n", flip);
}

/* Write the current surface out as a 24-bit BMP.
 *
 * A framebuffer window needs someone watching it. A file does not, which makes
 * this the only way to check what a title actually rendered on a machine you
 * are not sitting at -- and the only way to put a picture in a bug report.
 *
 * ponytail: bottom-up 24bpp BMP, no palette, no compression. That is the one
 * format every viewer reads and it is 30 lines; PNG would need a dependency.
 */
static void dump_surface_bmp(void)
{
    const char *prefix = recomp_env(RENV_FB_DUMP);
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    char path[512];
    uint32_t w = s_gpu.clip_w, h = s_gpu.clip_h, y, x;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;

    if (!prefix || !w || !h || (bpp != 2 && bpp != 4) || !s_gpu.color_offset)
        return;
    {
        const struct nv2a_pb_backend *be = nv2a_pb_get_backend();
        if (be->sync_guest)
            be->sync_guest(s_gpu.drawn_offset ? s_gpu.drawn_offset : s_gpu.color_offset,
                           s_gpu.pitch, s_gpu.clip_x + w, s_gpu.clip_y + h);
    }

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%03d.bmp", prefix, s_bmp_seq++);
    f = fopen(path, "wb");
    if (!f)
        return;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    /* BMP rows run bottom-up. */
    for (y = h; y-- > 0; ) {
        const uint8_t *row = mem + dma_resolve(s_gpu.drawn_offset
                                                ? s_gpu.drawn_offset
                                                : s_gpu.color_offset)
                           + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        for (x = 0; x < w; x++) {
            uint8_t bgr[3];
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)row)[s_gpu.clip_x + x];
                bgr[0] = (uint8_t)(v);
                bgr[1] = (uint8_t)(v >> 8);
                bgr[2] = (uint8_t)(v >> 16);
            } else {
                uint16_t v = ((const uint16_t *)row)[s_gpu.clip_x + x];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    if (s_bmp_seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X %ubpp)\n",
                path, w, h, s_gpu.color_offset, bpp);
}

/* Defined below, next to the rest of the rasteriser; the clear path uses it
 * for RECOMP_RASTER_TEST. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2]);

/* Surfaces the CPU has written, as byte ranges with the sequence number of
 * the last write, so the RECOMP_PB_FAST texture cache can tell when a texture
 * it holds is a render target drawn since it was decoded (tc_bind). Its own
 * per-flip check cannot see that: the title draws the frame and then samples
 * it, both in one flip. */
#define SURF_WR_MAX 32
static struct { uint32_t lo, hi, seq; } s_surf_wr[SURF_WR_MAX];
static uint32_t s_surf_wr_seq;

static void surf_mark_written(void)
{
    uint32_t lo = s_gpu.color_offset ? dma_resolve(s_gpu.color_offset) : 0;
    uint32_t hi = lo + (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch;
    int i, victim = 0;

    if (!lo || hi <= lo)
        return;
    s_surf_wr_seq++;
    for (i = 0; i < SURF_WR_MAX; i++) {
        if (s_surf_wr[i].lo == lo) {
            if (hi > s_surf_wr[i].hi)
                s_surf_wr[i].hi = hi;
            s_surf_wr[i].seq = s_surf_wr_seq;
            return;
        }
        if (s_surf_wr[i].seq < s_surf_wr[victim].seq)
            victim = i;
    }
    /* A range pushed out is forgotten; its writes are older than any the
     * table still holds, and the per-flip refingerprint covers a new flip. */
    s_surf_wr[victim].lo = lo;
    s_surf_wr[victim].hi = hi;
    s_surf_wr[victim].seq = s_surf_wr_seq;
}

/* The latest write sequence number overlapping [lo, hi), 0 if none. */
static uint32_t surf_written_since(uint32_t lo, uint32_t hi)
{
    uint32_t best = 0;
    int i;
    for (i = 0; i < SURF_WR_MAX; i++)
        if (s_surf_wr[i].seq && s_surf_wr[i].lo < hi && lo < s_surf_wr[i].hi
                && s_surf_wr[i].seq > best)
            best = s_surf_wr[i].seq;
    return best;
}

/* A swizzled render target (SET_SURFACE_FORMAT type 2) keeps its pixels in
 * the same Morton order a swizzled texture does, at the log2 width and height
 * in the format's top half. A title renders its 256x256 A8R8G8B8 post
 * targets that way and samples them back as swizzled textures (format 0x06), so
 * writing them in rows hands every later pass a scrambled image. Returns the
 * pixel's index from the start of the surface, or -1 if it is outside. */
static long surface_swizzle_index(int x, int y)
{
    static uint32_t key, mx, my;
    uint32_t w = nv2a_swizzle_side((s_gpu.format >> 16) & 0x1F);
    uint32_t h = nv2a_swizzle_side((s_gpu.format >> 24) & 0x1F);
    if ((uint32_t)x >= w || (uint32_t)y >= h)
        return -1;
    if (key != (s_gpu.format & 0xFFFF0000u)) {
        key = s_gpu.format & 0xFFFF0000u;
        xbox_swizzle_masks(w, h, &mx, &my);
    }
    return (long)(swizzle_deposit((uint32_t)x, mx) | swizzle_deposit((uint32_t)y, my));
}

static int surface_swizzled(void)
{
    return ((s_gpu.format >> 8) & 0xF) == 2;
}

static void clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x, b[4];
    /* The memory the clear may write (nv2a_clear_box clamps to the same
     * surface): a swizzled one is its power-of-two size, a pitch one ends
     * at the clip's far corner. */
    uint32_t sw = s_gpu.clip_x + s_gpu.clip_w, sh = s_gpu.clip_y + s_gpu.clip_h;

    if (!(param & NV097_CLEAR_COLOR_MASK))
        return;                            /* depth/stencil only */
    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
        return;
    if (surface_swizzled()) {
        sw = nv2a_swizzle_side((s_gpu.format >> 16) & 0x1F);
        sh = nv2a_swizzle_side((s_gpu.format >> 24) & 0x1F);
    }
    {
        uint32_t base = dma_resolve(s_gpu.color_offset);
        if (surface_write_refused(base,
                                  surface_swizzled() ? sw * sh * bpp : sh * s_gpu.pitch,
                                  "clear"))
            return;
        s_gpu.color_base = base;
    }
    /* SET_CLEAR_RECT, else the clip (nv2a_clear_box), as the GPU backends
     * clear: Burnout 3 clears 159x344 regions of its back buffer. */
    if (!nv2a_clear_box(&s_gpu, sw, sh, b))
        b[0] = b[1] = b[2] = b[3] = 0;   /* nothing to fill; still a clear */

    for (y = b[1]; y < b[3]; y++) {
        uint8_t *row = mem + s_gpu.color_base + (size_t)y * s_gpu.pitch;
        if (surface_swizzled()) {
            /* A clear of part of a swizzled target has to find each pixel;
             * a whole-surface clear lands the same either way. */
            uint8_t *base = mem + s_gpu.color_base;
            for (x = b[0]; x < b[2]; x++) {
                long i = surface_swizzle_index((int)x, (int)y);
                if (i < 0)
                    continue;
                if (bpp == 4)
                    ((uint32_t *)base)[i] = s_gpu.clear_color;
                else if (bpp == 2)
                    ((uint16_t *)base)[i] = (uint16_t)(
                        ((s_gpu.clear_color >> 8) & 0xF800)
                      | ((s_gpu.clear_color >> 5) & 0x07E0)
                      | ((s_gpu.clear_color >> 3) & 0x001F));
            }
            continue;
        }
        if (bpp == 4) {
            uint32_t *p = (uint32_t *)row;
            for (x = b[0]; x < b[2]; x++)
                p[x] = s_gpu.clear_color;
        } else if (bpp == 2) {
            /* The clear value is always given as A8R8G8B8; a 16-bit surface
             * takes the same colour reduced to 5:6:5. */
            uint16_t v = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                  | ((s_gpu.clear_color >> 5) & 0x07E0)
                                  | ((s_gpu.clear_color >> 3) & 0x001F));
            uint16_t *p = (uint16_t *)row;
            for (x = b[0]; x < b[2]; x++)
                p[x] = v;
        }
    }
    s_gpu.clears++;
    /* A colour clear is drawing too. drawn_offset is what a flip presents, and
     * only the triangle rasteriser used to set it, so a frame that was a clear
     * plus non-triangle work left it on the last surface a triangle touched.
     * Seen under Proton: every flip republished one surface while the
     * executor was clearing and drawing another, and the window was black. */
    s_gpu.drawn_offset = s_gpu.color_offset;
    surf_mark_written();
    /* Progress markers, interleaved with everything else in the log. The
     * summary says drawing stopped; only a marker next to the surrounding
     * activity says what the title was doing when it stopped. */
    if ((s_gpu.clears % 100) == 0)
        fprintf(stderr, "  [GPU] clear #%u\n", s_gpu.clears);
    /* Distinct clear colours actually used. "Cleared to black" and "the clear
     * never ran" look identical in the framebuffer, and only one of them is a
     * bug -- so record what was asked for, not just how often. */
    {
        static uint32_t seen[8];
        static int n;
        int i;
        for (i = 0; i < n; i++)
            if (seen[i] == s_gpu.clear_color) break;
        if (i == n && n < 8) {
            seen[n++] = s_gpu.clear_color;
            fprintf(stderr, "  [GPU] clear colour 0x%08X -> surface 0x%08X"
                            " (%ubpp)\n",
                    s_gpu.clear_color, s_gpu.color_offset, surface_bpp());
        }
    }

    /* Prove the pixel path end to end, independent of whether the title has
     * given us any geometry yet.
     *
     * "Nothing on screen" has three very different causes -- the surface
     * address or pitch is wrong, the rasteriser is broken, or the title's
     * vertex buffers are empty -- and they are indistinguishable from a black
     * window. RECOMP_RASTER_TEST draws one known triangle into the surface
     * just cleared, so a visible triangle rules out the first two and leaves
     * only the third. On Wreckless it is the third: attribute 0 decodes
     * correctly (float, size 2, stride 16) and the buffer it points at stays
     * zero.
     *
     * ponytail: bring-up aid, not a feature. It costs one branch per clear. */
    if (recomp_env(RENV_RASTER_TEST)) {
        static int announced;
        /* Every clear, not once: the title clears each frame and double-buffers,
         * so a triangle drawn a single time is erased before anyone sees it. */
        if (s_gpu.clip_w && s_gpu.clip_h) {
            float a[2], b[2], c[2];
            a[0] = s_gpu.clip_w * 0.5f; a[1] = s_gpu.clip_h * 0.15f;
            b[0] = s_gpu.clip_w * 0.85f; b[1] = s_gpu.clip_h * 0.85f;
            c[0] = s_gpu.clip_w * 0.15f; c[1] = s_gpu.clip_h * 0.85f;
            raster_triangle(a, b, c, 0xFFFF00FFu, NULL);  /* magenta: never a clear colour */
            if (announced++ == 0)
            fprintf(stderr, "  [GPU] raster self-test: triangle (%.0f,%.0f)"
                            " (%.0f,%.0f) (%.0f,%.0f) into 0x%08X %ubpp\n",
                    a[0], a[1], b[0], b[1], c[0], c[1],
                    s_gpu.color_offset, surface_bpp());
        }
    }

    /* Show the surface actually being drawn into. A title that double-buffers
     * renders into the back buffer, so following AvSetDisplayMode's address
     * would show the one nothing is writing. */
    /* The window has to read where the pixels actually are, which is the
     * resolved address rather than the DMA-object offset. */
    /* Only until the title flips. Following the draw surface on every scan
     * shows the buffer being written right now, half a frame at a time; past
     * the first flip the window is repointed at the finished one instead. */
    if (s_gpu.flips == 0)
        xbox_FramebufferWindowSet(dma_resolve(s_gpu.color_offset), s_gpu.pitch);

    /* And open the window, rather than waiting for AvSetDisplayMode to do it.
     *
     * That was the only caller, so a title which draws before setting a display
     * mode -- or never sets one at all -- got no window however much it
     * rendered. The Xbox Dashboard clears a 1280x960 surface at 0x00088000 on
     * its first frame and had not called AvSetDisplayMode by then, so
     * RECOMP_FB_WINDOW=1 was set, the executor knew the address and the pitch,
     * and nothing appeared.
     *
     * Here is the better trigger anyway: this runs when a surface address is
     * known to be real, because a clear just used it. Idempotent and gated on
     * RECOMP_FB_WINDOW, so the cost is one interlocked compare per clear. */
    xbox_FramebufferWindowStart();
}


/* ── Rasteriser ──────────────────────────────────────────────────────────
 *
 * Fills triangles straight into the guest framebuffer, the same memory
 * clear_surface() writes and the framebuffer window already shows. That is the
 * whole reason it is done on the CPU rather than through D3D: nothing new has
 * to be plumbed for the result to be visible.
 *
 * ponytail: flat-shaded, no depth buffer, no texturing, no perspective
 * correction, and only batches whose attribute 0 is already in screen space.
 * A title running a vertex program hands over object-space positions that mean
 * nothing without executing the program, so those batches are counted and
 * skipped rather than drawn somewhere wrong. Upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c once vertex programs are
 * translated; this exists to get the first geometry on screen for every title,
 * which in practice is UI, HUD and 2D overlays -- all pre-transformed.
 */

/* One texel, in the title's own format.
 *
 * The codes are the NV097 colour field, which is the Xbox D3DFMT_ enum --
 * src/d3d/d3d8_xbox.h is the table, and it is the table to check against
 * rather than recollection: 0x1E is LIN_X8R8G8B8 and not, as this first read
 * it, a byte-reversed BGRA. Getting that one wrong turned an opaque black
 * render target into a screen of pure blue, which is the kind of wrong that
 * looks like content.
 *
 * Only the linear (LIN_) formats are read. A swizzled texture stores its
 * texels in Morton order rather than in rows, so reading one as if it had a
 * pitch does not give a slightly wrong colour, it gives a different image --
 * and inventing that image is exactly what this is not for. An unsupported
 * format samples nothing and the caller keeps the vertex colour, which is
 * visibly wrong rather than quietly wrong.
 *
 * Filtering is the stage sampler's (stage_sample_2d): nearest, or bilinear
 * when SET_TEXTURE_FILTER asks for TENT, on the mip level(s) the pixel's LOD
 * picks when MIN mipmaps (stage_sample_mip).
 */
/* Off the edge of the texture, the way the title asked for.
 *
 * Refusing to sample instead is not neutral: it hands the caller back the
 * vertex colour, so a pass whose coordinates reach the last texel by half a
 * texel gets a bright line down the edge of the screen. The dashboard's
 * resolve does exactly that -- its last column and last row, 1119 pixels of
 * white on a black frame, from a rounding step at the boundary.
 */
static uint32_t wrap_coord(uint32_t c, uint32_t size, uint32_t mode)
{
    if (!size)
        return 0;
    if (mode == 1)                         /* wrap */
        return c % size;
    return c >= size ? size - 1 : c;       /* clamp, and everything else */
}

/* BORDER on an axis whose coordinate is off the texture: the tap is the
 * border colour, not a texel (nv2a_stage_decode has the rule). A negative
 * coordinate arrives here as a large unsigned one, as wrap_signed's -1
 * does. */
static int tex_border_tap(const Texture *t, uint32_t u, uint32_t v)
{
    return t->border_on
        && ((t->addr_u == NV2A_ADDR_BORDER && u >= t->width)
            || (t->addr_v == NV2A_ADDR_BORDER && v >= t->height));
}

static uint64_t s_tex_fail[256];        /* texels refused, by format code */

/* s_tex_fail[k]++, returning the old count: raster threads sample too. */
static uint64_t tex_fail_count(uint64_t *c)
{
#if defined(_WIN32)
    return (uint64_t)InterlockedIncrement64((volatile LONG64 *)c) - 1u;
#else
    return __atomic_fetch_add(c, 1, __ATOMIC_RELAXED);
#endif
}

static int sample_tex(const Texture *t, uint32_t u, uint32_t v, uint32_t *argb)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t fmt;

    if (!t->valid)
        return 0;
    if (tex_border_tap(t, u, v)) {
        *argb = t->border;
        return 1;
    }
    u = wrap_coord(u, t->width,  t->addr_u);
    v = wrap_coord(v, t->height, t->addr_v);

    fmt = t->color;
    if (d3d8_format_dxt_block_bytes(fmt))
        return d3d8_dxt_decode_texel(mem + t->offset, fmt, u, v,
                                     t->width, argb);
    if (d3d8_format_is_swizzled(fmt)) {
        /* Morton order: a texel's index is interleaved from x and y instead of
         * v*pitch + u, so index from the base of the image. The switch below
         * casts to each format's own width, which makes that index a texel
         * index for every one of them. */
        fmt = nv2a_tex_linear_twin(fmt);
        p = mem + t->offset;
        u = swizzle_offset(u, v, t->width, t->height);
    } else {
        p = mem + t->offset + (size_t)v * t->pitch;
    }

    if (fmt == NV2A_TEX_P8)
        return nv2a_tex_decode_texel(fmt, p, u,
                                     t->palette ? (const uint32_t *)(mem + t->palette)
                                                : NULL,
                                     t->pal_len, argb);
    if (nv2a_tex_decode_texel(fmt, p, u, NULL, 0, argb))
        return 1;
    /* Magenta, so an undecoded format shows up as itself rather than as the
     * vertex colour (which made P8 and G8B8 surfaces look like
     * plain white boxes). Logged once per format. */
    if (!tex_fail_count(&s_tex_fail[t->color & 0xFF]))
        fprintf(stderr, "  [GPU] texture format 0x%02X has no decoder;"
                        " sampling magenta\n", t->color);
    *argb = NV2A_TEX_MAGENTA;
    return 1;
}

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb)
{
    return sample_tex(&s_gpu.tex, u, v, argb);
}

/* Write a bound texture out as a BMP, through the sampler rather than around it.
 *
 * "Which texture is this" is not answerable from an address and a format, and
 * it is the question behind most of the ones that matter -- is that a font
 * page or an icon atlas, did the swizzle decode, is the alpha inverted. Going
 * through sample_texture means the file shows exactly what the rasteriser
 * sees, so a decode bug appears here rather than only as a wrong-looking
 * triangle.
 *
 * ponytail: RGB only, alpha dropped. A glyph page is alpha and would come out
 * black, so alpha is composited onto mid-grey to stay legible; that is a
 * viewing choice, not a decode. One file per distinct texture, first use only.
 */
static void dump_texture_bmp(uint32_t seq)
{
    const char *prefix = recomp_env(RENV_TEX_DUMP);
    uint32_t w = s_gpu.tex.width, h = s_gpu.tex.height, x, y;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    char path[512];
    FILE *f;

    if (!prefix || !w || !h || w > 4096 || h > 4096)
        return;
    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%02u_%08X_fmt%02X.bmp",
             prefix, seq, s_gpu.tex.offset, s_gpu.tex.color);
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t argb = 0, a;
            uint8_t px[3];
            if (!sample_texture(x, h - 1 - y, &argb))
                argb = 0;
            a = (argb >> 24) & 0xFFu;
            /* over mid-grey, so an alpha-only page is visible either way */
            px[0] = (uint8_t)(((argb & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[1] = (uint8_t)((((argb >> 8) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[2] = (uint8_t)((((argb >> 16) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            fwrite(px, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    fprintf(stderr, "  [TEXDUMP] %s (%ux%u fmt 0x%02X)\n",
            path, w, h, s_gpu.tex.color);
    fflush(stderr);
}

/* The surface, resolved once per batch.
 *
 * dma_resolve consults the contiguous arena's high-water mark and
 * surface_hits_image walks the image range; both were being done per pixel
 * -- dma_resolve twice -- which cost more than the rasterisation they
 * guarded. Neither answer can change inside a batch, because the colour
 * offset arrives as a method and a method cannot arrive mid-triangle.
 *
 * This is not a micro-optimisation for its own sake: the loader's video
 * paces on frames actually presented, so the rasteriser's throughput is the
 * playback rate. */
static uint8_t *s_surface;          /* host address of surface row 0 */

static int surface_begin_batch(const uint8_t *mem)
{
    uint32_t base = dma_resolve(s_gpu.color_offset);

    if (surface_hits_image(base, (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch))
        return 0;
    s_surface = (uint8_t *)mem + base;
    return 1;
}

/* A GL blend factor the CPU blender computes as GL does. SRC_ALPHA_SATURATE
 * included (upstream 0e9cff9): nv2a_blend_factor_u8 gives min(As, 1 - Ad)
 * for the colour channels and 1 for alpha; excluded, the pair was written
 * opaque. */
static int blend_known(uint32_t f)
{
    return nv2a_blend_from_gl(f) != NV2A_BF_UNKNOWN;
}

/* A GL blend factor for the channel at bit `sh` of an ARGB word, 0..255. */
static uint32_t blend_factor(uint32_t f, uint32_t src, uint32_t dst, uint32_t sh)
{
    return nv2a_blend_factor_u8(nv2a_blend_from_gl(f), src, dst,
                                s_gpu.blend_color, sh);
}

/* SET_BLEND_EQUATION folded to what the blender does: 0 ADD (also the
 * reset value 0 and 0xF006 ADD_SIGNED, done unsigned), 1 SUBTRACT
 * (src - dst), 2 REVERSE_SUBTRACT (dst - src; also 0xF005, its signed
 * variant), 3 MIN, 4 MAX. MIN and MAX ignore the factors, as in GL. */
static int blend_eq(void)
{
    switch (s_gpu.blend_equation) {
    case 0x800A: return 1;
    case 0x800B: case 0xF005: return 2;
    case 0x8007: return 3;
    case 0x8008: return 4;
    default:     return 0;
    }
}

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    uint8_t *row;

    (void)mem;
    if (x < (int)s_gpu.clip_x || x >= (int)(s_gpu.clip_x + s_gpu.clip_w))
        return;
    if (y < (int)s_gpu.clip_y || y >= (int)(s_gpu.clip_y + s_gpu.clip_h))
        return;
    if (surface_swizzled()) {
        long i = surface_swizzle_index(x, y);
        if (i < 0)
            return;
        row = s_surface;
        x = (int)i;
    } else {
        row = s_surface + (size_t)y * s_gpu.pitch;
    }
    if (t_cnt) {
        RasterCount *cn = t_cnt;
        cn->zpass += (uint32_t)s_zpass_enable;
        if (s_gpu.color_keep == 0xFFFFFFFFu)
            return;
        cn->pixels++;
        if ((argb & 0x00FFFFFFu) > (cn->pixel_max & 0x00FFFFFFu)) {
            cn->pixel_max = argb;
            cn->max_y = y;
        }
    } else {
        s_zpass_count += (uint32_t)s_zpass_enable;   /* passed every test */
        if (s_gpu.color_keep == 0xFFFFFFFFu)
            return;                        /* SET_COLOR_MASK: no channel written */
        s_gpu.pixels++;
        if ((argb & 0x00FFFFFFu) > (s_gpu.pixel_max & 0x00FFFFFFu))
            s_gpu.pixel_max = argb;
    }

    /* src*srcAlpha + dst*(1-srcAlpha) first, on its own path, because it was
     * the only pair here for a long time and the first goldens were checked
     * against exactly this arithmetic. The other GL factor pairs follow it.
     *
     * Fully opaque is left alone deliberately. It is the same arithmetic,
     * but skipping it keeps the full-screen quads -- which are drawn with
     * blending enabled and alpha 255 -- on exactly the path they were on
     * before, so this cannot change what they produce. */
    if (s_gpu.blend_enable && blend_eq() == 0
        && s_gpu.blend_sfactor == NV_BLEND_SRC_ALPHA
        && s_gpu.blend_dfactor == NV_BLEND_ONE_MINUS_SRC_ALPHA
        && (argb >> 24) != 0xFF) {
        uint32_t sa = argb >> 24;
        uint32_t dst = 0;
        if (sa == 0)
            return;                        /* nothing of the source survives */
        if (bpp == 4) {
            dst = ((const uint32_t *)row)[x];
        } else if (bpp == 2) {
            uint32_t t = ((const uint16_t *)row)[x];
            dst = (((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5)
                 | ((t & 0x001Fu) << 3));
        }
        {
            uint32_t r = (((argb >> 16) & 0xFF) * sa
                        + ((dst >> 16) & 0xFF) * (255u - sa) + 127u) / 255u;
            uint32_t g = (((argb >>  8) & 0xFF) * sa
                        + ((dst >>  8) & 0xFF) * (255u - sa) + 127u) / 255u;
            uint32_t b = (((argb      ) & 0xFF) * sa
                        + ((dst      ) & 0xFF) * (255u - sa) + 127u) / 255u;
            /* Alpha blends with the same factors, as GL and the GPU backends
             * do: As*As + Ad*(1-As). It used to be written 0xFF, and a later
             * pass reading DST_ALPHA (the shadow volumes) saw the wrong
             * value (upstream 0e9cff9). A 16-bit target has no destination
             * alpha and reads it as 1. */
            uint32_t da = bpp == 4 ? dst >> 24 : 255u;
            uint32_t a = (sa * sa + da * (255u - sa) + 127u) / 255u;
            argb = (a << 24) | (r << 16) | (g << 8) | b;
        }
    } else if (s_gpu.blend_enable
               && (blend_eq() == 3 || blend_eq() == 4      /* MIN/MAX: no factors */
                   || ((blend_eq() != 0
                        || !(s_gpu.blend_sfactor == NV_BLEND_SRC_ALPHA
                             && s_gpu.blend_dfactor == NV_BLEND_ONE_MINUS_SRC_ALPHA))
                       && blend_known(s_gpu.blend_sfactor)
                       && blend_known(s_gpu.blend_dfactor)))) {
        /* Every other GL factor pair, and any pair with an equation other
         * than ADD. Additive glows (SRC_ALPHA, ONE) are what a loading
         * screen's sprites use, and an opaque write turns each one into a
         * black square. Character shadows reverse-subtract the
         * front faces of their shadow volumes from destination alpha. */
        uint32_t dst = 0, out = 0, sh;
        int eq = blend_eq();
        if (bpp == 4) {
            dst = ((const uint32_t *)row)[x];
        } else if (bpp == 2) {
            uint32_t t = ((const uint16_t *)row)[x];
            dst = 0xFF000000u | (((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5)
                 | ((t & 0x001Fu) << 3));
        }
        for (sh = 0; sh <= 24; sh += 8) {
            uint32_t sc = (argb >> sh) & 0xFF, dc = (dst >> sh) & 0xFF;
            uint32_t v;
            if (eq == 3) {
                v = sc < dc ? sc : dc;
            } else if (eq == 4) {
                v = sc > dc ? sc : dc;
            } else {
                uint32_t fs = blend_factor(s_gpu.blend_sfactor, argb, dst, sh);
                uint32_t fd = blend_factor(s_gpu.blend_dfactor, argb, dst, sh);
                uint32_t s_ = (sc * fs + 127u) / 255u, d_ = (dc * fd + 127u) / 255u;
                if (eq == 1)      v = s_ > d_ ? s_ - d_ : 0;
                else if (eq == 2) v = d_ > s_ ? d_ - s_ : 0;
                else              v = (sc * fs + dc * fd + 127u) / 255u;
            }
            out |= (v > 255u ? 255u : v) << sh;
        }
        argb = out;
    }

    if (bpp == 4) {
        uint32_t keep = s_gpu.color_keep;
        if (keep)
            argb = (argb & ~keep) | (((const uint32_t *)row)[x] & keep);
        ((uint32_t *)row)[x] = argb;
    } else if (bpp == 2) {
        uint16_t v = (uint16_t)(((argb >> 8) & 0xF800)
                              | ((argb >> 5) & 0x07E0)
                              | ((argb >> 3) & 0x001F));
        uint32_t keep = s_gpu.color_keep;
        if (keep & 0x00FFFFFFu) {
            uint16_t k = (uint16_t)(((keep & 0x00FF0000u) ? 0xF800u : 0)
                                  | ((keep & 0x0000FF00u) ? 0x07E0u : 0)
                                  | ((keep & 0x000000FFu) ? 0x001Fu : 0));
            v = (uint16_t)((v & ~k) | (((const uint16_t *)row)[x] & k));
        }
        ((uint16_t *)row)[x] = v;
    }
}

/* Top-left fill rule (D3D's, and the NV2A's). With positions on the 1/16
 * grid a vertex often lands exactly on a pixel centre -- the XDK's 0.53125
 * offset puts every rectangle edge on x.5 -- so an inclusive test would draw
 * both triangles' copy of a shared edge, and a full-screen quad would light
 * the column and row past its right and bottom edges. A pixel centre on an
 * edge belongs to the triangle whose interior is to its right (a left edge),
 * or below it on a horizontal edge (a top edge): exactly one of two triangles
 * sharing that edge owns it. s is the sign of the area; (dx, dy) runs along
 * the edge in the winding the edge functions use. */
static inline int edge_owns_tie(float s, float dx, float dy)
{
    float gx = -s * dy, gy = s * dx;   /* points into the interior */
    return gx > 0.0f || (gx == 0.0f && gy > 0.0f);
}

static inline int edge_in(float s, float w, int owns_tie)
{
    return s * w > 0.0f || (w == 0.0f && owns_tie);
}

/* Half-space fill. Barycentric edge functions rather than scanline slopes:
 * the same test decides both windings, so a title that emits clockwise
 * triangles does not silently render nothing. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2])
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area, s_;
    int minx, maxx, miny, maxy, x, y, t0, t1, t2;
    int textured = uv && s_gpu.tex.valid;

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f)
        return;                            /* degenerate */
    s_ = area > 0.0f ? 1.0f : -1.0f;
    t0 = edge_owns_tie(s_, b[0] - a[0], b[1] - a[1]);
    t1 = edge_owns_tie(s_, c[0] - b[0], c[1] - b[1]);
    t2 = edge_owns_tie(s_, a[0] - c[0], a[1] - c[1]);

    /* Where this batch writes. The same check the per-pixel path made, made
     * once: a surface address landing on the title's own image is no safer
     * one pixel at a time than 4.9 MB at once. */
    if (!surface_begin_batch(mem))
        return;

    minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));

    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            if (!(edge_in(s_, w0, t0) && edge_in(s_, w1, t1)
                  && edge_in(s_, w2, t2)))
                continue;
            if (textured) {
                /* Barycentric, straight from the edge functions already
                 * computed: w1 is the area opposite a, w2 opposite b, w0
                 * opposite c, and the three sum to the whole triangle.
                 *
                 * No perspective divide. These are screen-space vertices with
                 * no w to divide by -- which is exactly the case a full-screen
                 * pass is, and the only case that reaches here. */
                uint32_t texel;
                float su = (w1 * uv[0][0] + w2 * uv[1][0] + w0 * uv[2][0]) / area;
                float sv = (w1 * uv[0][1] + w2 * uv[1][1] + w0 * uv[2][1]) / area;
                /* Left of or above the texture: the edge texel, or for a
                 * border axis an off-texture coordinate (tex_border_tap). */
                int bu = s_gpu.tex.border_on && s_gpu.tex.addr_u == NV2A_ADDR_BORDER;
                int bv = s_gpu.tex.border_on && s_gpu.tex.addr_v == NV2A_ADDR_BORDER;
                uint32_t iu = su < 0.0f ? (bu ? UINT32_MAX : 0u) : (uint32_t)su;
                uint32_t iv = sv < 0.0f ? (bv ? UINT32_MAX : 0u) : (uint32_t)sv;
                if (sample_texture(iu, iv, &texel)) {
                    put_pixel(mem, bpp, x, y, texel);
                    continue;
                }
            }
            put_pixel(mem, bpp, x, y, argb);
        }
    }
    s_gpu.tris_drawn++;
    s_gpu.drawn_offset = s_gpu.color_offset;
    surf_mark_written();
}

/* Attribute 3 is diffuse colour in every NV2A layout that sets one. Absent it,
 * white -- a visible wrong colour beats an invisible correct one during
 * bring-up. */
/* Which attribute carries the colour.
 *
 * Slot 3 is diffuse by convention and titles that follow it are read straight
 * from there. Half-Life 2 does not: its vertex is position, colour, texcoord
 * at stride 24, and the colour arrives in slot 5. So fall back to the format
 * rather than the slot number -- D3DCOLOR is the one attribute type that is
 * only ever a colour, which makes it a stronger signal than the convention. */
static const VertexAttr *color_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[3].offset && s_gpu.attr[3].stride)
        return &s_gpu.attr[3];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 0 && s_gpu.attr[a].size == 4
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[3];
}

/* Attribute 9 is texture coordinate 0 in the NV2A vertex layout, the same way
 * 0 is position and 3 is diffuse -- for a title that follows the convention.
 *
 * Half-Life 2 does not, in either place. Its menu and HUD vertex is position,
 * colour, texcoord at stride 24, with the colour in slot 5 and the texcoords
 * in slot 7, so reading slot 9 found nothing and every batch drew untextured.
 * That is invisible rather than wrong-looking: the menu paints a full-screen
 * quad and then draws its text over it, and with no sampling both come out
 * white, so the screen is blank white and nothing suggests the text was ever
 * drawn.
 *
 * Falling back to the format works because the three attributes of such a
 * vertex are distinguishable: position is float3, colour is D3DCOLOR, and a
 * float2 is a texture coordinate and nothing else.
 *
 * ponytail: takes the first float2 it finds, so a title with two texcoord sets
 * gets stage 0's -- which is what this single-texture rasteriser samples
 * anyway. Multi-texture wants the D3D11 translator, not another heuristic. */
static const VertexAttr *texcoord_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[9].offset && s_gpu.attr[9].stride)
        return &s_gpu.attr[9];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 2 && s_gpu.attr[a].size == 2
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[9];
}

/* Texel coordinates, whichever convention the title used.
 *
 * The two are not interchangeable and the format decides which is in force: a
 * swizzled texture is addressed in [0,1], a linear one in texels. Both are
 * scaled to texels here so that everything downstream -- the barycentric
 * interpolation and the sampler -- works in one unit.
 *
 * This mattered the moment swizzled formats became samplable. Normalised
 * coordinates truncated to a texel index land on texel 0 for any coordinate
 * below 1.0, so a whole quad sampled a single texel and came out flat: the
 * background painted one near-black colour, which looks like a texture that
 * decoded wrong rather than one that was never indexed. */
static int fetch_texcoord(uint32_t index, float out[2])
{
    float t[4];

    if (!fetch_attr(texcoord_attr(), index, t))
        return 0;
    out[0] = t[0];
    out[1] = t[1];
    if (nv2a_tex_size_from_format(s_gpu.tex.color)) {
        out[0] *= (float)s_gpu.tex.width;
        out[1] *= (float)s_gpu.tex.height;
    }
    return 1;
}

static uint32_t vertex_color(uint32_t index)
{
    float c[4];

    if (!fetch_attr(color_attr(), index, c))
        return 0xFFFFFFFFu;
    return ((uint32_t)(c[3] * 255.0f) << 24)
         | ((uint32_t)(c[0] * 255.0f) << 16)
         | ((uint32_t)(c[1] * 255.0f) <<  8)
         |  (uint32_t)(c[2] * 255.0f);
}

/* An untransformed batch drawn as if it were screen space smears a few pixels
 * into the corner, so the batch has to be classified before it is rasterised.
 *
 * This used to demand that every vertex land inside the surface, which is a
 * different question and the wrong one: geometry that extends past the
 * viewport is ordinary, and clipping it is raster_triangle's job (it clamps
 * its span to the clip rect). The dashboard is exactly the case that exposed
 * it -- a full-screen pass drawn as one oversized triangle, vertices at
 * (-0.5,-0.5), (2*w,-0.5), (-0.5,2*h), all correct and all rejected.
 *
 * What actually separates the two is scale. Object-space positions are model
 * units, a handful either side of the origin; screen-space ones are measured
 * in pixels of a surface hundreds of pixels wide. So: the batch has to be able
 * to touch the surface at all, and it has to be bigger than object space.
 *
 * ponytail: a genuinely tiny screen-space sprite reads as object space and is
 * skipped. It is counted as skipped rather than silently dropped, and the
 * unambiguous answer needs the vertex-program state, which is not tracked yet.
 */
#define OBJECT_SPACE_SPAN 8.0f

static int batch_is_screen_space(void)
{
    float p[4], lo_x, hi_x, lo_y, hi_y;
    uint32_t i;

    if (!s_gpu.clip_w || !s_gpu.clip_h || !s_gpu.idx_count)
        return 0;
    if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p))
        return 0;
    lo_x = hi_x = p[0];
    lo_y = hi_y = p[1];
    for (i = 1; i < s_gpu.idx_count; i++) {
        if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], p))
            return 0;
        if (p[0] < lo_x) lo_x = p[0];
        if (p[0] > hi_x) hi_x = p[0];
        if (p[1] < lo_y) lo_y = p[1];
        if (p[1] > hi_y) hi_y = p[1];
    }

    /* Entirely off the surface: nothing to draw under either reading. */
    if (hi_x < (float)s_gpu.clip_x
     || lo_x > (float)(s_gpu.clip_x + s_gpu.clip_w)
     || hi_y < (float)s_gpu.clip_y
     || lo_y > (float)(s_gpu.clip_y + s_gpu.clip_h))
        return 0;

    /* Small enough to be model units rather than pixels. */
    if (hi_x - lo_x < OBJECT_SPACE_SPAN && hi_y - lo_y < OBJECT_SPACE_SPAN)
        return 0;

    return 1;
}

/* NV_PRIM_*: nv2a_pb_state.h. */

/* How many post-draw captures to keep: enough to see whether the geometry
 * is stable from frame to frame, few enough not to fill a directory. */
#define FB_DUMP_AFTER_DRAW 8
static int s_drawn_dumps;

static void dump_surface_bmp(void);

/* One triangle by vertex index: gather position and, if the batch has one,
 * texture coordinate 0. A vertex whose position cannot be read is not drawn;
 * a batch whose texcoords cannot be read is drawn untextured rather than not
 * at all, so a missing coordinate stream costs the colour and not the shape.
 */
static void raster_indexed(uint32_t i0, uint32_t i1, uint32_t i2, uint32_t argb)
{
    float p[3][4], uv[3][2];
    int textured, k;

    if (!fetch_attr(&s_gpu.attr[0], i0, p[0])
     || !fetch_attr(&s_gpu.attr[0], i1, p[1])
     || !fetch_attr(&s_gpu.attr[0], i2, p[2]))
        return;
    for (k = 0; k < 3; k++) {   /* the NV2A's sub-pixel grid, as the GPU paths */
        p[k][0] = nv2a_screen_snap(p[k][0]);
        p[k][1] = nv2a_screen_snap(p[k][1]);
    }

    textured = fetch_texcoord(i0, uv[0])
            && fetch_texcoord(i1, uv[1])
            && fetch_texcoord(i2, uv[2]);

    raster_triangle(p[0], p[1], p[2], argb,
                    textured ? (const float (*)[2])uv : NULL);
}

/* ── Vertex programs ─────────────────────────────────────────────────────
 *
 * The title-stage geometry is drawn through NV2A vertex programs: attribute 0
 * is object space and only the program knows where it lands. So the executor
 * keeps the transform unit's state -- 136 program slots, 192 constants, the
 * execution mode and the inline attribute values -- and runs the program per
 * vertex on the CPU (nv2a_vsh_cpu.c). XDK D3D appends a viewport epilogue to
 * every program, so oPos arrives already in surface pixels with the clip-space
 * w beside it, which is everything the rasteriser needs.
 *
 * A batch takes this path when SET_TRANSFORM_EXECUTION_MODE says PROGRAM.
 * Fixed-function mode keeps the old screen-space path: no title run so far
 * sends a fixed-function matrix (no SET_MODEL_VIEW/COMPOSITE_MATRIX in the
 * stream), so there is nothing to synthesise yet, and the report says if that
 * changes.
 *
 * RECOMP_PB_VSH=0 turns the path off, which is the A/B switch for comparing
 * dumps against the old executor.
 *
 * Texture stages 0..3 and the register combiners are evaluated per pixel (see
 * rc_eval); RECOMP_PB_RC=0 falls back to stage 0 modulated by oD0.
 *
 * Triangles that reach behind the eye (w <= 0) or far off the surface are
 * clipped in homogeneous space before the divide (clip_tri, next to
 * raster_batch_vsh_one). Depth is a private float buffer, not the title's
 * zeta surface, which nothing else here reads.
 */
#define NV097_SET_TRANSFORM_PROGRAM_M        0x0B00   /* +0..0x7C */
#define NV097_SET_TRANSFORM_CONSTANT_M       0x0B80   /* +0..0x7C */
#define NV097_SET_TRANSFORM_EXECUTION_MODE_M 0x1E94
#define NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN_M 0x1E98
#define NV097_SET_TRANSFORM_PROGRAM_LOAD_M   0x1E9C
#define NV097_SET_TRANSFORM_PROGRAM_START_M  0x1EA0
#define NV097_SET_TRANSFORM_CONSTANT_LOAD_M  0x1EA4
#define NV097_SET_VIEWPORT_OFFSET_M          0x0A20
#define NV097_SET_VIEWPORT_SCALE_M           0x0AF0
#define NV097_SET_MODEL_VIEW_MATRIX_M        0x0480
#define NV097_SET_COMPOSITE_MATRIX_M         0x0680
#define NV097_SET_PROJECTION_MATRIX_M        0x0440
#define NV097_SET_CULL_FACE_ENABLE_M         0x0308
#define NV097_SET_CONTROL0_M                 0x0290
#define NV097_SET_DEPTH_TEST_ENABLE_M        0x030C
#define NV097_SET_DEPTH_FUNC_M               0x0354
#define NV097_SET_DEPTH_MASK_M               0x035C
#define NV097_SET_CULL_FACE_M                0x039C
#define NV097_SET_FRONT_FACE_M               0x03A0
#define NV097_SET_SURFACE_ZETA_OFFSET_M      0x0214
#define NV097_SET_ZSTENCIL_CLEAR_VALUE_M     0x1D8C
#define NV097_SET_STENCIL_TEST_ENABLE_M      0x032C
#define NV097_SET_STENCIL_MASK_M             0x0360   /* write mask */
#define NV097_SET_STENCIL_FUNC_M             0x0364
#define NV097_SET_STENCIL_FUNC_REF_M         0x0368
#define NV097_SET_STENCIL_FUNC_MASK_M        0x036C   /* read mask */
#define NV097_SET_STENCIL_OP_FAIL_M          0x0370   /* ..ZFAIL 0x374, ZPASS 0x378 */
#define NV097_SET_VERTEX_DATA2S_M            0x1900   /* + attr*4, 2 shorts */
#define NV097_SET_VERTEX_DATA4S_M_M          0x1980   /* + attr*8, 4 shorts */

#define NV097_SET_COMBINER_ALPHA_ICW_M      0x0260   /* +stage*4 */
#define NV097_SET_COMBINER_SPECULAR_FOG_CW0_M 0x0288
#define NV097_SET_COMBINER_SPECULAR_FOG_CW1_M 0x028C
#define NV097_SET_FOG_ENABLE_M              0x02A4
#define NV097_SET_FOG_COLOR_M               0x02A8
#define NV097_SET_ALPHA_TEST_ENABLE_M       0x0300
#define NV097_SET_ALPHA_FUNC_M              0x033C
#define NV097_SET_ALPHA_REF_M               0x0340
#define NV097_SET_COMBINER_FACTOR0_M        0x0A60
#define NV097_SET_COMBINER_FACTOR1_M        0x0A80
#define NV097_SET_COMBINER_ALPHA_OCW_M      0x0AA0
#define NV097_SET_COMBINER_COLOR_ICW_M      0x0AC0
#define NV097_SET_SPECULAR_FOG_FACTOR_M     0x1E20   /* +0, +4 */
#define NV097_SET_COMBINER_COLOR_OCW_M      0x1E40
#define NV097_SET_COMBINER_CONTROL_M        0x1E60
#define NV097_SET_SHADER_STAGE_PROGRAM_M    0x1E70
#define NV097_SET_SHADER_OTHER_STAGE_INPUT_M 0x1E78
#define NV097_SET_POLY_OFFSET_FILL_ENABLE_M 0x0338
#define NV097_SET_POLYGON_OFFSET_SCALE_FACTOR_M 0x0384
#define NV097_SET_POLYGON_OFFSET_BIAS_M     0x0388

/* Transform-context rows (xemu nv2a_regs.h NV_IGRAPH_XF_XFCTX_*). */
#define XF_VPSCL 0x3A
#define XF_VPOFF 0x3B

/* XfVert and the s_vsh layout: nv2a_pb_state.h. */
static struct nv2a_pb_vsh s_vsh;

/* Distinct programs, by hash of the slots from START to FINAL. */
#define VSH_MAX_PROGS 256
static struct { uint32_t hash, start, len, batches; } s_vsh_progs[VSH_MAX_PROGS];
static int s_vsh_prog_count;

static uint64_t s_vsh_ab_batches, s_vsh_ab_diff_batches, s_vsh_ab_diff_px;
static float s_vsh_last_d0[4];
static XfVert s_vsh_last_v0;
static float *s_zbuf;
static uint8_t *s_sbuf;                 /* stencil, same extent as s_zbuf */
static uint32_t s_zbuf_w, s_zbuf_h;

static float u2f(uint32_t u) { union { uint32_t u; float f; } v; v.u = u; return v.f; }

static int vsh_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_PB_VSH);
        on = !(e && e[0] == '0');
    }
    return on;
}

/* The transform-unit methods. Returns 1 if `method` was one of them. */
static int vsh_method(uint32_t method, uint32_t param)
{
    switch (method) {
    case 0x03B8: s_vsh.spec_enable = param != 0; return 1;  /* SET_SPECULAR_ENABLE */
    case 0x0294: s_vsh.light_ctl = param; return 1;         /* SET_LIGHT_CONTROL */
    case 0x029C: s_vsh.fog_mode = param; return 1;          /* SET_FOG_MODE */
    case 0x02A0: s_vsh.fog_gen = param; return 1;           /* SET_FOG_GEN_MODE */
    case 0x09C0: case 0x09C4: case 0x09C8:          /* SET_FOG_PARAMS */
        s_vsh.fog_param[(method - 0x09C0) / 4] = u2f(param); return 1;
    case 0x17F8: s_vsh.clip_plane_mode = param; return 1; /* SET_SHADER_CLIP_PLANE_MODE */
    default: break;
    }
    if (method >= NV097_SET_TRANSFORM_PROGRAM_M
            && method < NV097_SET_TRANSFORM_PROGRAM_M + 0x80) {
        uint32_t comp = ((method - NV097_SET_TRANSFORM_PROGRAM_M) / 4) & 3;
        if (s_vsh.load < NV2A_VSH_SLOTS)
            s_vsh.prog[s_vsh.load][comp] = param;
        if (comp == 3)
            s_vsh.load++;
        s_vsh.prog_dwords++;
        return 1;
    }
    if (method >= NV097_SET_TRANSFORM_CONSTANT_M
            && method < NV097_SET_TRANSFORM_CONSTANT_M + 0x80) {
        uint32_t comp = ((method - NV097_SET_TRANSFORM_CONSTANT_M) / 4) & 3;
        if (s_vsh.cload < NV2A_VSH_CONSTANTS)
            s_vsh.c[s_vsh.cload][comp] = u2f(param);
        if (comp == 3)
            s_vsh.cload++;
        s_vsh.const_dwords++;
        return 1;
    }
    /* The viewport, matrix, plane and eye methods are not separate registers:
     * they write the transform context, which is the same memory as the
     * program constants (xemu pgraph.c, NV_IGRAPH_XF_XFCTX_*). In particular
     * SET_VIEWPORT_SCALE/OFFSET land in c[58]/c[59], the two constants the
     * XDK epilogue reads; dropping them turned every world vertex into (0,0). */
    if (method >= NV097_SET_VIEWPORT_OFFSET_M && method < NV097_SET_VIEWPORT_OFFSET_M + 16) {
        uint32_t k = (method - NV097_SET_VIEWPORT_OFFSET_M) / 4;
        s_vsh.vp_off[k] = u2f(param);
        s_vsh.c[XF_VPOFF][k] = u2f(param);
        s_vsh.vp_dwords++;
        return 1;
    }
    if (method >= NV097_SET_VIEWPORT_SCALE_M && method < NV097_SET_VIEWPORT_SCALE_M + 16) {
        uint32_t k = (method - NV097_SET_VIEWPORT_SCALE_M) / 4;
        s_vsh.vp_scale[k] = u2f(param);
        s_vsh.c[XF_VPSCL][k] = u2f(param);
        s_vsh.vp_dwords++;
        return 1;
    }
    {
        /* method base, bytes, first context row, rows per 16-dword block */
        static const struct { uint32_t base, bytes, row, stride; } xf[] = {
            { 0x0680, 0x040, 0x00, 0 },   /* COMPOSITE_MATRIX         CMAT0 */
            { 0x0440, 0x040, 0x04, 0 },   /* PROJECTION_MATRIX        PMAT0 */
            { 0x0480, 0x100, 0x08, 8 },   /* MODEL_VIEW_MATRIX[4]     MMAT0 */
            { 0x0580, 0x100, 0x0C, 8 },   /* INVERSE_MODEL_VIEW[4]    IMMAT0 */
            { 0x09D0, 0x010, 0x39, 0 },   /* FOG_PLANE                FOG */
            { 0x0A50, 0x010, 0x38, 0 },   /* EYE_POSITION             EYEP */
            { 0x0840, 0x100, 0x40, 8 },   /* TEXGEN_PLANE_S..Q[4]     TG0MAT */
            { 0x06C0, 0x100, 0x44, 8 },   /* TEXTURE_MATRIX[4]        T0MAT */
        };
        uint32_t i;
        for (i = 0; i < sizeof xf / sizeof xf[0]; i++) {
            if (method >= xf[i].base && method < xf[i].base + xf[i].bytes) {
                uint32_t slot = (method - xf[i].base) / 4;
                uint32_t row = xf[i].row + (slot / 16) * xf[i].stride + (slot % 16) / 4;
                s_vsh.c[row][slot % 4] = u2f(param);
                if (i < 4)
                    s_vsh.ff_matrix_dwords++;
                return 1;
            }
        }
    }
    if (method >= NV097_SET_COMBINER_ALPHA_ICW_M && method < NV097_SET_COMBINER_ALPHA_ICW_M + 32) {
        s_vsh.rc_aicw[(method - NV097_SET_COMBINER_ALPHA_ICW_M) / 4] = param; return 1;
    }
    if (method >= NV097_SET_COMBINER_FACTOR0_M && method < NV097_SET_COMBINER_FACTOR0_M + 32) {
        s_vsh.rc_f0[(method - NV097_SET_COMBINER_FACTOR0_M) / 4] = param; return 1;
    }
    if (method >= NV097_SET_COMBINER_FACTOR1_M && method < NV097_SET_COMBINER_FACTOR1_M + 32) {
        s_vsh.rc_f1[(method - NV097_SET_COMBINER_FACTOR1_M) / 4] = param; return 1;
    }
    if (method >= NV097_SET_COMBINER_ALPHA_OCW_M && method < NV097_SET_COMBINER_ALPHA_OCW_M + 32) {
        s_vsh.rc_aocw[(method - NV097_SET_COMBINER_ALPHA_OCW_M) / 4] = param; return 1;
    }
    if (method >= NV097_SET_COMBINER_COLOR_ICW_M && method < NV097_SET_COMBINER_COLOR_ICW_M + 32) {
        s_vsh.rc_cicw[(method - NV097_SET_COMBINER_COLOR_ICW_M) / 4] = param; return 1;
    }
    if (method >= NV097_SET_COMBINER_COLOR_OCW_M && method < NV097_SET_COMBINER_COLOR_OCW_M + 32) {
        s_vsh.rc_cocw[(method - NV097_SET_COMBINER_COLOR_OCW_M) / 4] = param; return 1;
    }
    switch (method) {
    case NV097_SET_COMBINER_CONTROL_M:  s_vsh.rc_ctl = param; s_vsh.rc_set = 1; return 1;
    case NV097_SET_COMBINER_SPECULAR_FOG_CW0_M: s_vsh.rc_fcw0 = param; return 1;
    case NV097_SET_COMBINER_SPECULAR_FOG_CW1_M: s_vsh.rc_fcw1 = param; return 1;
    case NV097_SET_SPECULAR_FOG_FACTOR_M:     s_vsh.rc_sf[0] = param; return 1;
    case NV097_SET_SPECULAR_FOG_FACTOR_M + 4: s_vsh.rc_sf[1] = param; return 1;
    case NV097_SET_SHADER_STAGE_PROGRAM_M:
        s_vsh.shader_prog = param; s_vsh.shader_set = 1; return 1;
    case NV097_SET_SHADER_OTHER_STAGE_INPUT_M:
        s_vsh.other_stage_input = param; return 1;
    case NV097_SET_FOG_ENABLE_M:        s_vsh.fog_enable = param != 0;  return 1;
    case NV097_SET_FOG_COLOR_M:         s_vsh.fog_color = param;        return 1;
    case NV097_SET_ALPHA_TEST_ENABLE_M: s_vsh.atest_enable = param != 0; return 1;
    case NV097_SET_ALPHA_FUNC_M:        s_vsh.atest_func = param;       return 1;
    case NV097_SET_ALPHA_REF_M:         s_vsh.atest_ref = param & 0xFF; return 1;
    case NV097_SET_POLY_OFFSET_FILL_ENABLE_M: s_vsh.poly_offset = param != 0; return 1;
    case NV097_SET_POLYGON_OFFSET_SCALE_FACTOR_M: s_vsh.poly_factor = u2f(param); return 1;
    case NV097_SET_POLYGON_OFFSET_BIAS_M: s_vsh.poly_bias = u2f(param); return 1;
    case NV097_SET_TRANSFORM_EXECUTION_MODE_M:
        s_vsh.mode = param;
        s_vsh.mode_sets++;
        return 1;
    case NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN_M:
        s_vsh.cxt_write = param != 0;
        return 1;
    case NV097_SET_TRANSFORM_PROGRAM_LOAD_M:
        s_vsh.load = param;
        s_vsh.prog_loads++;
        return 1;
    case NV097_SET_TRANSFORM_PROGRAM_START_M:
        s_vsh.start = param;
        s_vsh.prog_starts++;
        return 1;
    case NV097_SET_TRANSFORM_CONSTANT_LOAD_M:
        s_vsh.cload = param;
        s_vsh.const_loads++;
        return 1;
    case NV097_SET_CULL_FACE_ENABLE_M:  s_vsh.cull_enable = param != 0; return 1;
    case NV097_SET_CONTROL0_M:          s_vsh.control0 = param;         return 1;
    case NV097_SET_CULL_FACE_M:         s_vsh.cull_face = param;        return 1;
    case NV097_SET_FRONT_FACE_M:        s_vsh.front_face = param;       return 1;
    case NV097_SET_DEPTH_TEST_ENABLE_M: s_vsh.depth_enable = param != 0; return 1;
    case NV097_SET_DEPTH_FUNC_M:        s_vsh.depth_func = param;       return 1;
    case NV097_SET_DEPTH_MASK_M:        s_vsh.depth_mask = param != 0;  return 1;
    case NV097_SET_SURFACE_ZETA_OFFSET_M: s_vsh.zeta_offset = param;    return 1;
    case NV097_SET_ZSTENCIL_CLEAR_VALUE_M: s_vsh.zclear = param;        return 1;
    case NV097_SET_STENCIL_TEST_ENABLE_M: s_vsh.stencil_enable = param != 0; return 1;
    case NV097_SET_STENCIL_MASK_M:      s_vsh.stencil_wmask = param & 0xFFu; return 1;
    case NV097_SET_STENCIL_FUNC_M:      s_vsh.stencil_func = param & 0xFu;   return 1;
    case NV097_SET_STENCIL_FUNC_REF_M:  s_vsh.stencil_ref = param & 0xFFu;   return 1;
    case NV097_SET_STENCIL_FUNC_MASK_M: s_vsh.stencil_rmask = param & 0xFFu; return 1;
    case NV097_SET_STENCIL_OP_FAIL_M:
    case NV097_SET_STENCIL_OP_FAIL_M + 4:
    case NV097_SET_STENCIL_OP_FAIL_M + 8:
        s_vsh.stencil_op[(method - NV097_SET_STENCIL_OP_FAIL_M) / 4] = param;
        return 1;
    default:
        return 0;
    }
}

/* Inline attribute values, the inputs a program reads for any attribute with
 * no array behind it. Called for every SET_VERTEX_DATA* write, alongside the
 * immediate-mode bookkeeping that already consumes some of them. */
static void vsh_inline_attr(uint32_t method, uint32_t param)
{
    if (method >= NV097_SET_VERTEX_DATA2F_M
            && method < NV097_SET_VERTEX_DATA2F_M + NV_VERTEX_ATTRS * 8) {
        uint32_t off = method - NV097_SET_VERTEX_DATA2F_M, a = off / 8;
        s_vsh.inl[a][(off % 8) / 4] = u2f(param);
        s_vsh.inl[a][2] = 0.0f; s_vsh.inl[a][3] = 1.0f;
    } else if (method >= NV097_SET_VERTEX_DATA4F_M
            && method < NV097_SET_VERTEX_DATA4F_M + NV_VERTEX_ATTRS * 16) {
        uint32_t off = method - NV097_SET_VERTEX_DATA4F_M;
        s_vsh.inl[off / 16][(off % 16) / 4] = u2f(param);
    } else if (method >= NV097_SET_VERTEX_DATA4UB
            && method < NV097_SET_VERTEX_DATA4UB + NV_VERTEX_ATTRS * 4) {
        float *d = s_vsh.inl[(method - NV097_SET_VERTEX_DATA4UB) / 4];
        /* Bytes in register order, x from the low byte (xemu pgraph.c,
         * upstream 5c3a42c) -- not a D3DCOLOR array element's B,G,R,A. The
         * XDK's SetVertexDataColor swaps R and B before it issues this
         * method, so reading it as D3DCOLOR swapped them back. */
        d[0] = (float)( param        & 0xFF) / 255.0f;
        d[1] = (float)((param >>  8) & 0xFF) / 255.0f;
        d[2] = (float)((param >> 16) & 0xFF) / 255.0f;
        d[3] = (float)( param >> 24        ) / 255.0f;
    } else if (method >= NV097_SET_VERTEX_DATA2S_M
            && method < NV097_SET_VERTEX_DATA2S_M + NV_VERTEX_ATTRS * 4) {
        float *d = s_vsh.inl[(method - NV097_SET_VERTEX_DATA2S_M) / 4];
        d[0] = (float)(int16_t)(param & 0xFFFF);
        d[1] = (float)(int16_t)(param >> 16);
        d[2] = 0.0f; d[3] = 1.0f;
    } else if (method >= NV097_SET_VERTEX_DATA4S_M_M
            && method < NV097_SET_VERTEX_DATA4S_M_M + NV_VERTEX_ATTRS * 8) {
        uint32_t off = method - NV097_SET_VERTEX_DATA4S_M_M;
        float *d = s_vsh.inl[off / 8];
        uint32_t k = ((off % 8) / 4) * 2;
        d[k]     = (float)(int16_t)(param & 0xFFFF);
        d[k + 1] = (float)(int16_t)(param >> 16);
    } else if (method >= NV097_SET_VERTEX4F && method < NV097_SET_VERTEX4F + 16) {
        s_vsh.inl[0][(method - NV097_SET_VERTEX4F) / 4] = u2f(param);
    } else if (method >= NV097_SET_VERTEX3F && method < NV097_SET_VERTEX3F + 12) {
        s_vsh.inl[0][(method - NV097_SET_VERTEX3F) / 4] = u2f(param);
        s_vsh.inl[0][3] = 1.0f;
    }
}

static int vsh_batch_active(void)
{
    return vsh_enabled() && (s_vsh.mode & 3u) == 2u;
}

/* Track the program a batch runs, for the report and for RECOMP_VSH_TRACE.
 * Returns the program's length in slots. */
static uint32_t vsh_note_program(void)
{
    uint32_t len, pc, h = nv2a_vsh_program_hash(&s_vsh, &len);
    int i;

    /* A linear scan, up to VSH_MAX_PROGS per draw on the CPU path: cheaper
     * than the hash above, which walks the whole program every draw. */
    for (i = 0; i < s_vsh_prog_count; i++)
        if (s_vsh_progs[i].hash == h && s_vsh_progs[i].start == s_vsh.start) {
            s_vsh_progs[i].batches++;
            return len;
        }
    if (s_vsh_prog_count >= VSH_MAX_PROGS) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [VSH] %d programs noted; later ones are neither"
                            " counted nor dumped\n", VSH_MAX_PROGS);
        }
        return len;
    }
    s_vsh_progs[s_vsh_prog_count].hash = h;
    s_vsh_progs[s_vsh_prog_count].start = s_vsh.start;
    s_vsh_progs[s_vsh_prog_count].len = len;
    s_vsh_progs[s_vsh_prog_count].batches = 1;
    s_vsh_prog_count++;

    if (recomp_env(RENV_VSH_TRACE)) {
        char buf[160];
        fprintf(stderr, "  [VSH] program #%d hash %08X start %u len %u"
                        " (cxt_write %d)\n",
                s_vsh_prog_count - 1, h, s_vsh.start, len, s_vsh.cxt_write);
        for (pc = s_vsh.start; pc < s_vsh.start + len && pc < NV2A_VSH_SLOTS; pc++)
            fprintf(stderr, "  [VSH]   %3u: %08X %08X %08X  %s\n", pc,
                    s_vsh.prog[pc][1], s_vsh.prog[pc][2], s_vsh.prog[pc][3],
                    nv2a_vsh_disasm(s_vsh.prog[pc], buf, sizeof buf));
    }
    /* Program memory, constants and the attribute table, raw, so the
     * interpreter can be run offline against exactly what the title sent. */
    {
        const char *pre = recomp_env(RENV_VSH_DUMP);
        if (pre) {
            char path[512];
            FILE *f;
            snprintf(path, sizeof path, "%s%02d_%08X.bin", pre,
                     s_vsh_prog_count - 1, h);
            f = fopen(path, "wb");
            if (f) {
                fwrite(&s_vsh.start, 4, 1, f);
                fwrite(s_vsh.prog, sizeof s_vsh.prog, 1, f);
                fwrite(s_vsh.c, sizeof s_vsh.c, 1, f);
                fwrite(s_vsh.inl, sizeof s_vsh.inl, 1, f);
                fclose(f);
            }
        }
    }
    return len;
}

/* Gather v0..v15 for one vertex: array data where the title enabled an array,
 * the inline value everywhere else. */
static void vsh_inputs(uint32_t index, float v[NV2A_VSH_INPUTS][4])
{
    uint32_t a;

    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        const VertexAttr *at = &s_gpu.attr[a];
        if (at->size
                && (s_gpu.inline_active || at->offset)
                && fetch_attr(at, index, v[a])) {
            /* An array of fewer than four components fills the rest with
             * (0, 0, 1) like every other vertex fetch. */
            continue;
        }
        memcpy(v[a], s_vsh.inl[a], sizeof v[a]);
    }
}

/* The fog factor for one vertex, from the program's oFog.x: xemu vsh.c,
 * including its offsets (fogParam.x is 1.5 for the EXP modes and the factor
 * comes out of exp2 shifted by it). A program-mode batch takes the fog
 * distance from oFog.x whatever SET_FOG_GEN_MODE says. Disabled fog is 1:
 * the final combiner's FOG alpha then leaves the colour alone. */
static float vsh_fog_factor(float d)
{
    return nv2a_fog_factor(s_vsh.fog_enable, s_vsh.fog_mode,
                           s_vsh.fog_param[0], s_vsh.fog_param[1], d);
}

static void vsh_transform(uint32_t index, XfVert *xv)
{
    float v[NV2A_VSH_INPUTS][4];
    Nv2aVshOut o;
    int k;

    vsh_inputs(index, v);
    nv2a_vsh_run((const uint32_t (*)[4])s_vsh.prog, s_vsh.start, s_vsh.c,
                 s_vsh.cxt_write, (const float (*)[4])v, &o);
    s_vsh.verts_run++;
    xv->x = nv2a_screen_snap(o.o[NV2A_VSH_O_POS][0]);
    xv->y = nv2a_screen_snap(o.o[NV2A_VSH_O_POS][1]);
    xv->z = o.o[NV2A_VSH_O_POS][2];
    xv->w = o.o[NV2A_VSH_O_POS][3];
    /* oPos.w == 0 means a program that never divided by w (x, y, z are
     * already surface units): use w = 1, as the D3D11/Metal epilogues do and
     * as xemu's clampAwayZeroInf keeps such a vertex in front of the eye.
     * 1 rather than xemu's 2^-64: 1/w then cannot overflow the CPU LOD
     * quotient, and the triangle keeps the unit-w fast path. */
    if (xv->w == 0.0f)
        xv->w = 1.0f;
    for (k = 0; k < 4; k++) {
        float c = o.o[NV2A_VSH_O_D0][k], s = o.o[NV2A_VSH_O_D1][k];
        int t;
        xv->d0[k] = c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c;
        xv->d1[k] = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
        for (t = 0; t < 4; t++)
            xv->t[t][k] = o.o[NV2A_VSH_O_T0 + t][k];
    }
    /* What xemu's vsh.c does after the program: vtxD1 is (0, 0, 0, 1) unless
     * specular is enabled, and then its alpha is 1 unless SET_LIGHT_CONTROL
     * asks for ALPHA_FROM_MATERIAL_SPECULAR (bit 17). */
    if (!s_vsh.spec_enable) {
        xv->d1[0] = xv->d1[1] = xv->d1[2] = 0.0f;
        xv->d1[3] = 1.0f;
    } else if (!(s_vsh.light_ctl & (1u << 17))) {
        xv->d1[3] = 1.0f;
    }
    xv->fog = vsh_fog_factor(o.o[NV2A_VSH_O_FOG][0]);
    /* ok means usable: the program finished and the position is finite. A
     * vertex behind the eye (w <= 0) or far outside the surface is usable --
     * clip_tri cuts the triangle down to the part that is in front. */
    xv->ok = o.ok && isfinite(xv->x) && isfinite(xv->y) && isfinite(xv->z)
          && isfinite(xv->w);
    if (!xv->ok)
        s_vsh.verts_bad++;
}

/* Depth buffer: one float per pixel of the clip rectangle's extent, in [0,1].
 * A colour clear that also names Z resets it (see vsh_clear_depth). */
static float vsh_zmax(void)
{
    return nv2a_zmax(s_gpu.format);
}

/* One depth/stencil buffer per zeta offset and clip extent, picked by
 * zb_select (nv2a_zbuf_cache.h, which also says why one shared buffer is not
 * enough, and what the key leaves out). s_zbuf/s_sbuf/s_zbuf_w/s_zbuf_h name the live entry. */
static ZbCache  s_zbc;
static float   *s_zb_z[ZB_MAX];
static uint8_t *s_zb_s[ZB_MAX];

/* Slots in use: RECOMP_DEBUG=zbuf_slots=n, 1..ZB_MAX, default 8. A title
 * with more live (zeta, extent) pairs than that in one frame evicts, and the
 * first eviction says so below. */
static int zb_slots(void)
{
    static int n;
    if (!n) {
        long v = recomp_env_int(RENV_ZBUF_SLOTS, 8);
        n = v < 1 ? 1 : v > ZB_MAX ? ZB_MAX : (int)v;
    }
    return n;
}

static int vsh_zbuf_ready(void)
{
    uint32_t w = s_gpu.clip_x + s_gpu.clip_w, h = s_gpu.clip_y + s_gpu.clip_h, i;
    ZbPick pk;

    if (!w || !h || w > 4096 || h > 4096)
        return 0;
    pk = zb_select_n(&s_zbc, zb_slots(), s_vsh.zeta_offset, w, h);
    if (pk.evicted && s_zbc.evictions == 1)
        fprintf(stderr, "[VSH] depth buffer cache full (%d zeta/extent keys): evicting"
                        " the least recently used; depth may be lost (#36;"
                        " RECOMP_DEBUG=zbuf_slots=n raises it)\n", zb_slots());
    if (pk.fresh || !s_zb_z[pk.index] || !s_zb_s[pk.index]) {
        free(s_zb_z[pk.index]);
        free(s_zb_s[pk.index]);
        s_zb_z[pk.index] = (float *)malloc((size_t)w * h * sizeof(float));
        s_zb_s[pk.index] = (uint8_t *)malloc((size_t)w * h);
        if (!s_zb_z[pk.index] || !s_zb_s[pk.index]) {
            free(s_zb_z[pk.index]); free(s_zb_s[pk.index]);
            s_zb_z[pk.index] = NULL; s_zb_s[pk.index] = NULL;
            s_zbc.slot[pk.index].used = 0;
            s_zbuf = NULL; s_sbuf = NULL; s_zbuf_w = s_zbuf_h = 0;
            return 0;
        }
        for (i = 0; i < w * h; i++)
            s_zb_z[pk.index][i] = 1.0f;
        memset(s_zb_s[pk.index], 0, (size_t)w * h);
    }
    s_zbuf = s_zb_z[pk.index]; s_sbuf = s_zb_s[pk.index];
    s_zbuf_w = w; s_zbuf_h = h;
    return 1;
}

/* The stencil test is live: enabled, and the zeta surface is Z24S8 (a Z16
 * surface has no stencil, and GL then passes every fragment untouched). */
static int vsh_stencil_ready(void)
{
    return s_vsh.stencil_enable && ((s_gpu.format >> 4) & 0xF) == 2
        && vsh_zbuf_ready();
}

static int stencil_pass(uint8_t stored)
{
    uint32_t m = s_vsh.stencil_rmask, r = s_vsh.stencil_ref & m, v = stored & m;
    uint32_t f = s_vsh.stencil_func;
    return nv2a_cmp_test_u(f <= NV2A_CMP_ALWAYS ? f : NV2A_CMP_ALWAYS, r, v);
}

static void stencil_apply(uint8_t *sp, uint32_t op)
{
    uint32_t v = *sp, n, sop = nv2a_stencil_op_from_gl(op);
    if (sop == NV2A_SOP_KEEP)
        return;                            /* KEEP, and anything unset */
    n = nv2a_stencil_op_apply(sop, v, s_vsh.stencil_ref);
    *sp = (uint8_t)((v & ~s_vsh.stencil_wmask) | (n & s_vsh.stencil_wmask));
}

/* Stencil, then depth, for one fragment before it is shaded, in GL's order:
 * a stencil failure applies the fail op and skips the depth test, a depth
 * failure applies the zfail op. 0: shade it. 1: discarded. 2: failed, but the
 * alpha test runs first (a fragment it kills touches no stencil), so shade
 * it and apply *op if it survives. */
static int depth_pass(float z, float stored);

static int zs_early(int use_s, int use_z, float z, const float *zp,
                    const uint8_t *sp, uint32_t *op)
{
    if (use_s && !stencil_pass(*sp))
        *op = s_vsh.stencil_op[0];
    else if (use_z && !depth_pass(z, *zp)) {
        if (t_cnt) t_cnt->z_rejected++; else s_vsh.z_rejected++;
        if (!use_s)
            return 1;
        *op = s_vsh.stencil_op[1];
    } else
        return 0;
    if (s_vsh.atest_enable)
        return 2;
    stencil_apply((uint8_t *)sp, *op);
    return 1;
}

static void vsh_clear_depth(uint32_t param)
{
    float z;
    uint32_t i;

    s_vsh.clears_seen++;
    if (recomp_env(RENV_VSH_ZLOG) && s_vsh.batches_prog < 1200)
        fprintf(stderr, "  [ZLOG] clear 0x%X colour 0x%X zeta 0x%X zclear 0x%X\n",
                param, s_gpu.color_offset, s_vsh.zeta_offset, s_vsh.zclear);
    if (!(param & 3u) || !vsh_zbuf_ready())
        return;
    if (param & 2u)                        /* stencil: the value's low byte */
        memset(s_sbuf, (int)(s_vsh.zclear & 0xFFu), (size_t)s_zbuf_w * s_zbuf_h);
    if (!(param & 1u))
        return;
    s_vsh.zclears++;
    s_vsh.zclear_color_off = s_gpu.color_offset;
    s_vsh.zclear_zeta_off = s_vsh.zeta_offset;
    s_vsh.zclear_batch = s_vsh.batches_prog;
    z = nv2a_zclear_depth(s_gpu.format, s_vsh.zclear);
    for (i = 0; i < s_zbuf_w * s_zbuf_h; i++)
        s_zbuf[i] = z;
}

static int depth_pass(float z, float stored)
{
    return nv2a_cmp_test_f(nv2a_cmp_from_gl(s_vsh.depth_func), z, stored);
}

/* A stage tap's coordinate on one axis: wrapped, clamped, or -1 off the
 * texture under BORDER, which texel_live and texel_cached read as the
 * border colour. */
static int wrap_signed(int c, uint32_t size, uint32_t mode)
{
    if (!size)
        return 0;
    if (mode == 1) {                       /* wrap */
        int m = c % (int)size;
        return m < 0 ? m + (int)size : m;
    }
    if (mode == NV2A_ADDR_BORDER && (c < 0 || c >= (int)size))
        return -1;
    if (c < 0) return 0;
    return c >= (int)size ? (int)size - 1 : c;
}

/* ---- Raster timing (RECOMP_FLIP_LOG) ------------------------------------
 *
 * Where the CPU rasteriser's time goes, per flip: the whole of raster_batch,
 * and inside it the vertex transform, per-batch stage and combiner setup, and
 * texture decode into the RECOMP_PB_FAST cache. What is left is the pixel
 * loop. Read and reset by the RECOMP_FLIP_LOG line. Timestamps are taken per
 * batch and per cache tile, never per pixel. */
#if defined(_WIN32)
static uint64_t rt_now(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#else
static uint64_t rt_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif
static struct {
    uint64_t total, xform, setup, tex;   /* ns since the last flip line */
    uint32_t yuv_batches;                /* batches sampling a YUY2/UYVY stage 0 */
} s_rt;

/* Walker timing (RECOMP_FLIP_LOG, the [PB-PERF] line): host time spent in
 * nv2a_pb_scan (the walk, decode and every backend hook it calls) and, inside
 * that, in the backend's on_draw, per flip. nv2a_pb_scan.c brackets each scan
 * with nv2a_pb_perf_scan_begin/end; the line is printed and reset at the flip,
 * which arrives inside a scan, so the open scan is counted up to that point. */
static struct {
    uint64_t scan, draw, flip, scan_t0, flip_t0;
    uint32_t methods, batches;
} s_perf;

uint64_t nv2a_pb_now_ns(void) { return rt_now(); }
void nv2a_pb_perf_scan_begin(void) { s_perf.scan_t0 = rt_now(); }
void nv2a_pb_perf_scan_end(void)
{
    uint64_t now = rt_now();
    s_perf.scan += now - s_perf.scan_t0;
    s_perf.scan_t0 = now;
}

/* ---- Texture stages and register combiners ----------------------------
 *
 * Per batch, each of the four stages is decoded from the texture registers
 * the title last wrote (s_tex_reg), and given a texture-shader mode from
 * SET_SHADER_STAGE_PROGRAM. Per pixel, the stages are sampled into T0..T3 and
 * the general combiners then the final combiner run on them, after xemu's
 * psh.c. That is what decides whether a texel is modulated by oD0, added to
 * it, ignored, or used only for its alpha -- which a fixed MODULATE got wrong
 * for every batch whose oD0 is zero (a movie-backed intro is all of them).
 *
 * ponytail: point sampling, 2D only. Cube, 3D, dot-product and bump-map
 * shader modes sample as plain 2D at (s, t); the report counts them. Fog is
 * fog colour with factor 1 (no fog). */
static Texture s_stage_tex[4];
static int     s_stage_mode[4];         /* 0 none, 1 2D, 4 pass-through, other */
static float   s_stage_scale[4][2];     /* normalised -> texels */
static int     s_stage_tent[4];         /* bilinear: SET_TEXTURE_FILTER asks TENT */

/* Mip selection, per stage (stage_lod). A stage mipmaps when its texture
 * has more than one level (nv2a_stage_decode with NV2A_STAGE_MIPS, as the
 * GPU paths count them) and MIN asks for a mip mode; it is never a cube
 * map or a volume. Level 0 of such a stage is the same texture
 * s_stage_tex holds. */
#define MIP_MAX_LEVELS 16
typedef struct {
    int      mip;                       /* 0 off, 1 nearest level, 2 blend two */
    int      mag_lin, min_lin;          /* bilinear within a level */
    uint32_t levels;
    float    lod_min, lod_max, bias;
    uint32_t off[MIP_MAX_LEVELS];       /* byte offset of each level */
} StageMip;
static StageMip s_stage_mip[4];

/* The screen-space derivatives a triangle's pixels need for their LOD
 * (lod_tri_setup): for each mipmapped stage, those of the perspective
 * numerators of s, t and q (tc[3]) and of their shared denominator. */
typedef struct {
    int   on;                           /* some stage mipmaps */
    float qx, qy;                       /* d(sum la/w)/dx, /dy */
    float nx[4][3], ny[4][3];           /* d(sum la*t[k]/w)/dx, /dy, k = s,t,q */
} LodTri;

/* RECOMP_TEX_LOG=1: every distinct texture a program-mode batch samples,
 * per stage, with the raw format word decoded (dimensionality, cube map, mip
 * levels, log2 sizes), the pitch and image rect, whether the format is
 * swizzled, and whether sample_tex can decode it at all. A line on first
 * sight and the table, with batch counts, in the report. */
#define TEX_LOG_MAX 1024
static struct {
    uint32_t offset, fmtw, ctl1, rect, width, height, pitch, color;
    uint32_t stages, batches;
    int      decodes;
} s_tex_log[TEX_LOG_MAX];
static int s_tex_log_count;
static uint32_t s_tex_log_overflow;

static int tex_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_TEX_LOG);
        on = e && e[0] && e[0] != '0';
    }
    return on;
}

static int sample_tex(const Texture *t, uint32_t u, uint32_t v, uint32_t *argb);

static void tex_log_line(const char *tag, int i)
{
    uint32_t f = s_tex_log[i].fmtw;
    fprintf(stderr, "  [TEXLOG] %s 0x%08X fmt 0x%02X %s %ux%u pitch %u"
                    " (fmtword 0x%08X: dim %u cube %u mips %u log2 %u,%u,%u dma %u;"
                    " ctl1 0x%08X rect 0x%08X) stages 0x%X decode %s batches %u\n",
            tag, s_tex_log[i].offset, s_tex_log[i].color,
            d3d8_format_dxt_block_bytes(s_tex_log[i].color) ? "dxt"
                : d3d8_format_is_swizzled(s_tex_log[i].color) ? "swz" : "lin",
            s_tex_log[i].width, s_tex_log[i].height, s_tex_log[i].pitch, f,
            (f >> 4) & 0xF, (f >> 2) & 1, (f >> 16) & 0xF, (f >> 20) & 0xF,
            (f >> 24) & 0xF, (f >> 28) & 0xF, f & 3, s_tex_log[i].ctl1,
            s_tex_log[i].rect, s_tex_log[i].stages,
            s_tex_log[i].decodes ? "ok" : "UNHANDLED", s_tex_log[i].batches);
}

static void tex_log_note(int stage, const Texture *t, uint32_t fmtw,
                         uint32_t ctl1, uint32_t rect)
{
    int i;
    uint32_t dummy;
    if (!tex_log_on())
        return;
    if (nv2a_tex_size_from_format(t->color)) {
        /* A swizzled or compressed texture has no pitch or image rect; the
         * registers keep whatever a linear one last left there. */
        ctl1 = rect = 0;
    }
    for (i = 0; i < s_tex_log_count; i++)
        if (s_tex_log[i].offset == t->offset && s_tex_log[i].fmtw == fmtw
                && s_tex_log[i].ctl1 == ctl1
                && s_tex_log[i].width == t->width
                && s_tex_log[i].height == t->height) {
            s_tex_log[i].batches++;
            s_tex_log[i].stages |= 1u << stage;
            return;
        }
    if (s_tex_log_count >= TEX_LOG_MAX) {
        s_tex_log_overflow++;
        return;
    }
    i = s_tex_log_count++;
    s_tex_log[i].offset = t->offset; s_tex_log[i].fmtw = fmtw;
    s_tex_log[i].ctl1 = ctl1; s_tex_log[i].rect = rect;
    s_tex_log[i].width = t->width; s_tex_log[i].height = t->height;
    s_tex_log[i].pitch = ctl1 >> 16; s_tex_log[i].color = t->color;
    s_tex_log[i].stages = 1u << stage; s_tex_log[i].batches = 1;
    s_tex_log[i].decodes = t->valid && sample_tex(t, 0, 0, &dummy);
    tex_log_line("new", i);
}

/* And the render targets program-mode batches draw into, the same way:
 * offset, SET_SURFACE_FORMAT, pitch and clip, with the bytes per pixel the
 * rasteriser derives from them (pitch / clip width) beside the one the format
 * names. */
#define SURF_LOG_MAX 64
static struct { uint32_t offset, format, pitch, cx, cy, cw, ch, batches; } s_surf_log[SURF_LOG_MAX];
static int s_surf_log_count;

static uint32_t surface_bpp(void);

static uint32_t surf_fmt_bpp(uint32_t format)
{
    switch (format & 0xF) {
    case 0x1: case 0x2: case 0x3: return 2;    /* X1R5G5B5 x2, R5G6B5 */
    case 0x4: case 0x5: case 0x6: case 0x7:
    case 0x8: case 0xC: return 4;              /* X8R8G8B8 x2, X1A7.., A8R8G8B8, A8B8G8R8 */
    case 0x9: return 1;                        /* B8 */
    case 0xA: case 0xB: return 2;              /* G8B8 */
    default:  return 0;
    }
}

static void surf_log_line(const char *tag, int i)
{
    uint32_t f = s_surf_log[i].format;
    fprintf(stderr, "  [SURFLOG] %s 0x%08X fmt 0x%08X (colour %u zeta %u type %u aa %u"
                    " log2 %ux%u) pitch %u clip %ux%u+%u+%u -> bpp %u by pitch,"
                    " %u by format; batches %u\n", tag,
            s_surf_log[i].offset, f, f & 0xF, (f >> 4) & 0xF, (f >> 8) & 0xF,
            (f >> 12) & 0xF, (f >> 16) & 0xFF, (f >> 24) & 0xFF,
            s_surf_log[i].pitch, s_surf_log[i].cw, s_surf_log[i].ch,
            s_surf_log[i].cx, s_surf_log[i].cy,
            s_surf_log[i].cw ? s_surf_log[i].pitch / s_surf_log[i].cw : 0,
            surf_fmt_bpp(f), s_surf_log[i].batches);
}

static void surf_log_note(void)
{
    int i;
    if (!tex_log_on())
        return;
    for (i = 0; i < s_surf_log_count; i++)
        if (s_surf_log[i].offset == s_gpu.color_offset
                && s_surf_log[i].format == s_gpu.format
                && s_surf_log[i].pitch == s_gpu.pitch
                && s_surf_log[i].cx == s_gpu.clip_x && s_surf_log[i].cy == s_gpu.clip_y
                && s_surf_log[i].cw == s_gpu.clip_w && s_surf_log[i].ch == s_gpu.clip_h) {
            s_surf_log[i].batches++;
            return;
        }
    if (s_surf_log_count >= SURF_LOG_MAX)
        return;
    i = s_surf_log_count++;
    s_surf_log[i].offset = s_gpu.color_offset; s_surf_log[i].format = s_gpu.format;
    s_surf_log[i].pitch = s_gpu.pitch;
    s_surf_log[i].cx = s_gpu.clip_x; s_surf_log[i].cy = s_gpu.clip_y;
    s_surf_log[i].cw = s_gpu.clip_w; s_surf_log[i].ch = s_gpu.clip_h;
    s_surf_log[i].batches = 1;
    surf_log_line("new", i);
}

static void tex_log_report(void)
{
    int i, k;
    if (!tex_log_on())
        return;
    for (i = 0; i < s_surf_log_count; i++)
        surf_log_line("surf", i);
    fprintf(stderr, "[TEXLOG] %d distinct textures sampled by program-mode"
                    " batches%s\n", s_tex_log_count,
            s_tex_log_overflow ? " (table full, more not listed)" : "");
    /* Per format: how many textures and batches, and whether it decodes.
     * RECOMP_TEX_LOG=2 lists every texture as well. */
    for (k = 0; k < 256; k++) {
        uint32_t ntex = 0, nb = 0, bad = 0;
        for (i = 0; i < s_tex_log_count; i++)
            if (s_tex_log[i].color == (uint32_t)k) {
                ntex++; nb += s_tex_log[i].batches; bad += !s_tex_log[i].decodes;
            }
        if (ntex)
            fprintf(stderr, "[TEXLOG] format 0x%02X %s: %u textures, %u stage-batches,"
                            " %u not decodable\n", k,
                    d3d8_format_dxt_block_bytes((uint32_t)k) ? "dxt"
                        : d3d8_format_is_swizzled((uint32_t)k) ? "swz" : "lin",
                    ntex, nb, bad);
    }
    if (recomp_env(RENV_TEX_LOG)[0] == '2')
        for (i = 0; i < s_tex_log_count; i++)
            tex_log_line("tex", i);
    for (k = 0; k < 256; k++)
        if (s_tex_fail[k])
            fprintf(stderr, "[TEXLOG] format 0x%02X: %llu texels refused"
                            " (no decoder)\n", k,
                    (unsigned long long)s_tex_fail[k]);
}

/* Bilinear when SET_TEXTURE_FILTER asks for a linear filter, magnifying or
 * minifying (as upstream 13c64d3; linear as the GPU backends read it,
 * nv2a_tex_filter_linear). A 64x32 sky gradient stretched over the screen
 * is blocks with nearest texels and a gradient with four.
 * A stage that mipmaps (stage_mip_build) picks MAG or MIN per pixel from
 * its LOD instead.
 * RECOMP_DEBUG=pb_bilinear=0 samples nearest whatever the filter says. */
static int pb_bilinear(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_PB_BILINEAR);
        on = !(e && e[0] == '0');
    }
    return on;
}

static int tex_filter_tent(uint32_t filter)
{
    return pb_bilinear()
        && (nv2a_tex_filter_linear(filter, 1) || nv2a_tex_filter_linear(filter, 0));
}

/* RECOMP_DEBUG=pb_mips=0: level 0 only, whatever MIN asks (A/B). */
static int pb_mips(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_PB_MIPS);
        on = !(e && e[0] == '0');
    }
    return on;
}

static void stage_mip_build(int n, const struct nv2a_stage *st)
{
    StageMip *m = &s_stage_mip[n];
    uint32_t l;

    m->mip = 0;
    /* nv2a_tex_level_offset walks a 2D chain; a volume texture's levels
     * are each a stack of slices, so its offsets past level 0 would land
     * mid-slice. Such a stage samples level 0, as before mips. */
    if (!pb_mips() || st->levels < 2 || st->cube || st->dims == 3
            || st->mode == 0 || st->mode == 4
            || !nv2a_tex_size_from_format(st->color))
        return;
    m->mip = nv2a_tex_filter_mip(st->filter);
    if (!m->mip)
        return;
    m->levels = st->levels < MIP_MAX_LEVELS ? st->levels : MIP_MAX_LEVELS;
    m->lod_min = (float)st->lod_min;
    m->lod_max = (float)st->lod_max;
    m->bias = nv2a_tex_lod_bias(st->filter);
    m->mag_lin = pb_bilinear() && nv2a_tex_filter_linear(st->filter, 1);
    m->min_lin = pb_bilinear() && nv2a_tex_filter_linear(st->filter, 0);
    for (l = 0; l < m->levels; l++)
        m->off[l] = nv2a_tex_level_offset(st->color, st->width, st->height, l);
}

static void vsh_build_stages(void)
{
    int n;
    for (n = 0; n < 4; n++) {
        const uint32_t *r = &s_tex_reg[(0x40u * n) / 4];
        Texture *t = &s_stage_tex[n];
        struct nv2a_stage st;
        int from_fmt;

        /* With NV2A_STAGE_MIPS only levels and the LOD clamps differ. */
        nv2a_stage_decode(n, r, &s_tex_set[(0x40u * n) / 4], s_vsh.shader_set,
                          s_vsh.shader_prog, dma_resolve, NV2A_STAGE_MIPS, &st);
        memset(t, 0, sizeof *t);
        t->offset = st.offset;
        t->width  = st.width;
        t->height = st.height;
        t->pitch  = st.pitch;
        t->color  = st.color;
        t->addr_u = st.addr_u;
        t->addr_v = st.addr_v;
        t->border = st.border;
        t->border_on = 1;   /* the decode keeps BORDER only with the colour */
        t->palette = st.palette;
        t->pal_len = st.pal_len;
        t->filter = st.filter;
        t->face_stride = st.cube ? nv2a_tex_face_stride(st.color, st.width, st.height,
                                                        st.pitch, st.fmt_levels) : 0;
        t->valid  = st.valid;
        s_stage_tent[n] = tex_filter_tent(st.filter);
        s_vsh.stage_mode_seen[n][st.raw_mode]++;
        s_stage_mode[n] = st.mode;
        if (st.mode != 0 && st.mode != 4 && st.mode != 5)
            tex_log_note(n, t, r[0x04 / 4], r[0x10 / 4], r[0x1C / 4]);
        from_fmt = nv2a_tex_size_from_format(t->color);
        s_stage_scale[n][0] = from_fmt ? (float)t->width : 1.0f;
        s_stage_scale[n][1] = from_fmt ? (float)t->height : 1.0f;
        stage_mip_build(n, &st);
    }
}

static void rc_unpack(uint32_t c, float o[4])
{
    nv2a_argb_to_float4(c, o);
}

static float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

/* The combiner registers as nv2a_combiner.c takes them; rc_load copies them
 * from s_vsh before a triangle (reference path) or a batch (plan). */
static Nv2aCombiner s_rc;

static void rc_load(void)
{
    memcpy(s_rc.color_icw, s_vsh.rc_cicw, sizeof s_rc.color_icw);
    memcpy(s_rc.alpha_icw, s_vsh.rc_aicw, sizeof s_rc.alpha_icw);
    memcpy(s_rc.color_ocw, s_vsh.rc_cocw, sizeof s_rc.color_ocw);
    memcpy(s_rc.alpha_ocw, s_vsh.rc_aocw, sizeof s_rc.alpha_ocw);
    memcpy(s_rc.factor0, s_vsh.rc_f0, sizeof s_rc.factor0);
    memcpy(s_rc.factor1, s_vsh.rc_f1, sizeof s_rc.factor1);
    s_rc.final0 = s_vsh.rc_fcw0;
    s_rc.final1 = s_vsh.rc_fcw1;
    s_rc.final_c0 = s_vsh.rc_sf[0];
    s_rc.final_c1 = s_vsh.rc_sf[1];
    s_rc.control = s_vsh.rc_ctl;
    s_rc.stage_program = s_vsh.shader_prog;
}

/* General combiners then the final combiner, on r[] (register-code indexed);
 * the result is written to out[4] as r,g,b,a. */
static void rc_eval(float r[16][4], float out[4])
{
    nv2a_rc_eval_regs(&s_rc, r, out, NULL);
}

static int vsh_rc_active(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_PB_RC);
        on = !(e && e[0] == '0');
    }
    return on && s_vsh.rc_set && (s_vsh.rc_ctl & 0xFF);
}

static int alpha_test_pass(float a)
{
    uint32_t v = (uint32_t)(clamp01(a) * 255.0f + 0.5f), ref = s_vsh.atest_ref;
    return nv2a_cmp_test_u(nv2a_cmp_from_gl(s_vsh.atest_func), v, ref);
}

/* Texel (iu, iv), already wrapped, of `t` as floats; black, opaque, when
 * the texture cannot be read. */
static void texel_live(const Texture *t, int iu, int iv, float o[4])
{
    uint32_t texel;
    if (sample_tex(t, (uint32_t)iu, (uint32_t)iv, &texel))
        rc_unpack(texel, o);
    else
        o[0] = o[1] = o[2] = 0.0f, o[3] = 1.0f;
}

/* The four taps' weights, the same expression for the live and cached
 * samplers so their results agree bit for bit. */
static void bilerp(const float c[4][4], float wu, float wv, float o[4])
{
    int j;
    for (j = 0; j < 4; j++)
        o[j] = (c[0][j] * (1.0f - wu) + c[1][j] * wu) * (1.0f - wv)
             + (c[2][j] * (1.0f - wu) + c[3][j] * wu) * wv;
}

/* Bilinear setup: the top-left tap and the weights, at texel centres, for
 * coordinates scaled to texels by (sw, sh). */
static void tent_setup(float sw, float sh, float s, float t, int *iu, int *iv,
                       float *wu, float *wv)
{
    float fu = s * sw - 0.5f, fv = t * sh - 0.5f;
    *iu = (int)floorf(fu);
    *iv = (int)floorf(fv);
    *wu = fu - (float)*iu;
    *wv = fv - (float)*iv;
}

/* Texture `tx` at coordinates s, t scaled to texels by (sw, sh): bilinear
 * (lin) or nearest, read live. */
static void sample_2d_live(const Texture *tx, float sw, float sh, int lin,
                           float s, float t, float o[4])
{
    int iu, iv;
    if (lin) {
        float wu, wv, c[4][4];
        int k;
        tent_setup(sw, sh, s, t, &iu, &iv, &wu, &wv);
        for (k = 0; k < 4; k++)
            texel_live(tx, wrap_signed(iu + (k & 1), tx->width, tx->addr_u),
                       wrap_signed(iv + (k >> 1), tx->height, tx->addr_v), c[k]);
        bilerp(c, wu, wv, o);
        return;
    }
    iu = wrap_signed((int)floorf(s * sw), tx->width,  tx->addr_u);
    iv = wrap_signed((int)floorf(t * sh), tx->height, tx->addr_v);
    texel_live(tx, iu, iv, o);
}

/* Stage n's texture `tx` (the stage's own, or one cube face of it) at
 * normalised (swizzled, DXT) or texel (linear) coordinates s, t. */
static void stage_sample_2d(int n, const Texture *tx, float s, float t, float o[4])
{
    sample_2d_live(tx, s_stage_scale[n][0], s_stage_scale[n][1], s_stage_tent[n],
                   s, t, o);
}

/* A triangle's LodTri. la, lb, lc are the edge functions over the area,
 * linear in the pixel position; their per-pixel steps, over 1/w, give the
 * derivatives of the perspective numerators and of their sum (the pixel's
 * ps). Both rasterisers call this with the same operands. */
static void lod_tri_setup(const XfVert *a, const XfVert *b, const XfVert *c,
                          float area, const float inv_w[3], LodTri *L)
{
    const XfVert *v[3];
    float dx[3], dy[3];
    int n, i, j;
    static const int comp[3] = { 0, 1, 3 };

    L->on = 0;
    for (n = 0; n < 4; n++)
        L->on |= s_stage_mip[n].mip != 0;
    if (!L->on)
        return;
    v[0] = a; v[1] = b; v[2] = c;
    /* la = w1 / area, w1 = (c.x - b.x)(py - b.y) - (c.y - b.y)(px - b.x);
     * lb from w2 (a, c), lc from w0 (b, a). Each times its 1/w. */
    dx[0] = -(c->y - b->y) / area * inv_w[0]; dy[0] = (c->x - b->x) / area * inv_w[0];
    dx[1] = -(a->y - c->y) / area * inv_w[1]; dy[1] = (a->x - c->x) / area * inv_w[1];
    dx[2] = -(b->y - a->y) / area * inv_w[2]; dy[2] = (b->x - a->x) / area * inv_w[2];
    L->qx = dx[0] + dx[1] + dx[2];
    L->qy = dy[0] + dy[1] + dy[2];
    for (n = 0; n < 4; n++) {
        if (!s_stage_mip[n].mip)
            continue;
        for (j = 0; j < 3; j++) {
            float sx = 0.0f, sy = 0.0f;
            for (i = 0; i < 3; i++) {
                sx += dx[i] * v[i]->t[n][comp[j]];
                sy += dy[i] * v[i]->t[n][comp[j]];
            }
            L->nx[n][j] = sx;
            L->ny[n][j] = sy;
        }
    }
}

/* Stage n's LOD at a pixel whose interpolated coordinate is tc and whose
 * perspective sum is q: the coordinate's screen derivatives from the
 * quotient rule, d(N/q) = (dN - (N/q) dq) / q, again through the projective
 * divide for a 2D projective stage, in level-0 texels. */
static float stage_lod(int n, const LodTri *L, const float tc[4], float q)
{
    const StageMip *m = &s_stage_mip[n];
    float iq, sx, sy, tx, ty;

    if (q == 0.0f)
        return m->lod_min;
    iq = 1.0f / q;
    sx = (L->nx[n][0] - tc[0] * L->qx) * iq;
    sy = (L->ny[n][0] - tc[0] * L->qy) * iq;
    tx = (L->nx[n][1] - tc[1] * L->qx) * iq;
    ty = (L->ny[n][1] - tc[1] * L->qy) * iq;
    if (s_stage_mode[n] == 1 && tc[3] != 0.0f && tc[3] != 1.0f) {
        float rx = (L->nx[n][2] - tc[3] * L->qx) * iq;
        float ry = (L->ny[n][2] - tc[3] * L->qy) * iq;
        float ir = 1.0f / tc[3], ps = tc[0] * ir, pt = tc[1] * ir;
        sx = (sx - ps * rx) * ir; sy = (sy - ps * ry) * ir;
        tx = (tx - pt * rx) * ir; ty = (ty - pt * ry) * ir;
    }
    sx *= s_stage_scale[n][0]; sy *= s_stage_scale[n][0];
    tx *= s_stage_scale[n][1]; ty *= s_stage_scale[n][1];
    return nv2a_tex_lod(sx * sx + tx * tx, sy * sy + ty * ty, m->bias,
                       m->lod_min, m->lod_max);
}

/* The two levels' samples, blended by f (trilinear); the same expression
 * for the live and cached samplers. */
static void mip_blend(float o[4], const float o1[4], float f)
{
    int j;
    for (j = 0; j < 4; j++)
        o[j] = o[j] * (1.0f - f) + o1[j] * f;
}

/* Level l of stage n's (mipmapped, so swizzled or DXT) texture. */
static void stage_level_tex(int n, uint32_t l, Texture *lt)
{
    *lt = s_stage_tex[n];
    lt->offset += s_stage_mip[n].off[l];
    lt->width = nv2a_tex_level_dim(lt->width, l);
    lt->height = nv2a_tex_level_dim(lt->height, l);
}

/* A mipmapped stage at (s, t), read live: the LOD picks MAG or MIN and the
 * level(s). */
static void stage_sample_mip(int n, const LodTri *L, const float tc[4], float q,
                             float s, float t, float o[4])
{
    const StageMip *m = &s_stage_mip[n];
    float lod = stage_lod(n, L, tc, q), f, o1[4];
    int lin = lod > 0.0f ? m->min_lin : m->mag_lin;
    uint32_t l0, l1;
    Texture lt;

    nv2a_tex_mip_levels(m->mip, lod, m->levels, &l0, &l1, &f);
    stage_level_tex(n, l0, &lt);
    sample_2d_live(&lt, (float)lt.width, (float)lt.height, lin, s, t, o);
    if (f > 0.0f) {
        stage_level_tex(n, l1, &lt);
        sample_2d_live(&lt, (float)lt.width, (float)lt.height, lin, s, t, o1);
        mip_blend(o, o1, f);
    }
}

/* Stage n at the interpolated coordinate tc. L and q (the pixel's
 * perspective sum, ps) give a mipmapped stage its LOD; with L NULL it
 * samples level 0. */
/* Texture-shader mode 5, CLIPPLANE (xemu psh.c; upstream 13c64d3). The
 * stage carries no texture: each coordinate component is a user clip plane
 * distance, and SET_SHADER_CLIP_PLANE_MODE (0x17F8) picks per stage and
 * component whether the pixel dies at >= 0 or at < 0. Returns 1 if this
 * pixel is killed; the stage's register reads 0 either way. */
static int stage_clip_kills(int n, const float tc[4])
{
    int j;
    for (j = 0; j < 4; j++) {
        int ge = (s_vsh.clip_plane_mode >> (n * 4 + j)) & 1;
        if (ge ? tc[j] >= 0.0f : tc[j] < 0.0f)
            return 1;
    }
    return 0;
}

/* Returns 0 when the stage kills the pixel (CLIPPLANE), else 1 with the
 * stage's value in o. */
static int stage_sample(int n, const float tc[4], float q, const LodTri *L,
                        float o[4])
{
    int mode = s_stage_mode[n];
    float s, t;

    /* No texture reads (0, 0, 0, 1), as xemu's psh.c (and GL's unbound
     * sampler, for a stage whose texture is off): titles use its alpha as a
     * constant 1, e.g. a water combiner weighting its reflections by the
     * alpha of a stage left off goes black at 0. CLIPPLANE still reads 0. */
    if (mode == 0) { o[0] = o[1] = o[2] = 0.0f; o[3] = 1.0f; return 1; }
    if (mode == 4) {                       /* pass-through: the coordinate */
        int k;
        for (k = 0; k < 4; k++) o[k] = clamp01(tc[k]);
        return 1;
    }
    if (mode == 5) {                       /* clip plane: no texture */
        o[0] = o[1] = o[2] = o[3] = 0.0f;
        return !stage_clip_kills(n, tc);
    }
    if (mode == 3 && s_stage_tex[n].face_stride) {   /* cube map */
        Texture f = s_stage_tex[n];
        f.offset += nv2a_cube_face(tc, &s, &t) * f.face_stride;
        stage_sample_2d(n, &f, s, t, o);
        return 1;
    }
    s = tc[0]; t = tc[1];
    if (mode == 1 && tc[3] != 0.0f && tc[3] != 1.0f) {   /* 2D projective */
        s /= tc[3]; t /= tc[3];
    }
    if (L && s_stage_mip[n].mip) {
        stage_sample_mip(n, L, tc, q, s, t, o);
        return 1;
    }
    stage_sample_2d(n, &s_stage_tex[n], s, t, o);
    return 1;
}

static void raster_tri_xf(const XfVert *a, const XfVert *b, const XfVert *c,
                          int textured)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area, inv_w[3], zscale = 1.0f / vsh_zmax(), s_;
    int minx, maxx, miny, maxy, x, y, use_z, use_s, use_rc, n, t0, t1, t2;
    LodTri lod;

    if (bpp != 4 && bpp != 2)
        return;
    if (!a->ok || !b->ok || !c->ok) {
        s_vsh.tris_clipped++;
        return;
    }
    area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0.0f)
        return;
    if (s_vsh.cull_enable) {
        /* area > 0 is clockwise as displayed (y down). FRONT_FACE CW means a
         * clockwise triangle faces the viewer. */
        int cw = area > 0.0f;
        int front = (s_vsh.front_face == 0x900) ? cw : !cw;
        if (recomp_env(RENV_PB_CULL_FLIP))
            front = !front;
        if ((s_vsh.cull_face == 0x405 && !front)
         || (s_vsh.cull_face == 0x404 && front)
         || s_vsh.cull_face == 0x408) {
            s_vsh.tris_culled++;
            return;
        }
    }
    if (!surface_begin_batch(mem))
        return;
    use_z = s_vsh.depth_enable && vsh_zbuf_ready();
    use_s = vsh_stencil_ready();
    use_rc = vsh_rc_active();
    if (use_rc)
        rc_load();

    minx = (int)floorf(fminf(a->x, fminf(b->x, c->x)));
    maxx = (int)ceilf (fmaxf(a->x, fmaxf(b->x, c->x)));
    miny = (int)floorf(fminf(a->y, fminf(b->y, c->y)));
    maxy = (int)ceilf (fmaxf(a->y, fmaxf(b->y, c->y)));
    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    inv_w[0] = 1.0f / a->w; inv_w[1] = 1.0f / b->w; inv_w[2] = 1.0f / c->w;
    lod_tri_setup(a, b, c, area, inv_w, &lod);
    s_ = area > 0.0f ? 1.0f : -1.0f;
    t0 = edge_owns_tie(s_, b->x - a->x, b->y - a->y);
    t1 = edge_owns_tie(s_, c->x - b->x, c->y - b->y);
    t2 = edge_owns_tie(s_, a->x - c->x, a->y - c->y);

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
            float w1 = (c->x - b->x) * (py - b->y) - (c->y - b->y) * (px - b->x);
            float w2 = (a->x - c->x) * (py - c->y) - (a->y - c->y) * (px - c->x);
            float la, lb, lc, pa, pb, pc, ps, col[4], z = 0.0f;
            float *zp = NULL;
            uint8_t *sp = NULL;
            uint32_t argb, zs_op = 0;
            int k, zs = 0;

            if (!(edge_in(s_, w0, t0) && edge_in(s_, w1, t1)
                  && edge_in(s_, w2, t2)))
                continue;
            /* w1 is opposite a, w2 opposite b, w0 opposite c. */
            la = w1 / area; lb = w2 / area; lc = w0 / area;

            if (use_z) {
                z = (la * a->z + lb * b->z + lc * c->z) * zscale;
                zp = &s_zbuf[(size_t)y * s_zbuf_w + (size_t)x];
            }
            if (use_s)
                sp = &s_sbuf[(size_t)y * s_zbuf_w + (size_t)x];
            if ((use_z || use_s)
                    && (zs = zs_early(use_s, use_z, z, zp, sp, &zs_op)) == 1)
                continue;

            /* Perspective-correct weights. */
            pa = la * inv_w[0]; pb = lb * inv_w[1]; pc = lc * inv_w[2];
            ps = pa + pb + pc;
            if (ps != 0.0f) { pa /= ps; pb /= ps; pc /= ps; }

            for (k = 0; k < 4; k++)
                col[k] = pa * a->d0[k] + pb * b->d0[k] + pc * c->d0[k];

            if (use_rc) {
                float r[16][4], tc[4];
                memset(r, 0, sizeof r);
                memcpy(r[4], col, sizeof col);
                for (k = 0; k < 4; k++)
                    r[5][k] = pa * a->d1[k] + pb * b->d1[k] + pc * c->d1[k];
                rc_unpack(s_vsh.fog_color, r[3]);
                { float t_ = r[3][0]; r[3][0] = r[3][2]; r[3][2] = t_; } /* RGBA */
                r[3][3] = s_vsh.fog_enable
                        ? clamp01(pa * a->fog + pb * b->fog + pc * c->fog) : 1.0f;
                for (n = 0; n < 4; n++) {
                    if (!s_stage_mode[n]) {
                        /* NONE reads (0, 0, 0, 1): see stage_sample */
                        r[8 + n][0] = r[8 + n][1] = r[8 + n][2] = 0.0f;
                        r[8 + n][3] = 1.0f;
                        continue;
                    }
                    for (k = 0; k < 4; k++)
                        tc[k] = pa * a->t[n][k] + pb * b->t[n][k] + pc * c->t[n][k];
                    if (!stage_sample(n, tc, ps, &lod, r[8 + n]))
                        break;                 /* CLIPPLANE killed the pixel */
                }
                if (n < 4)
                    continue;
                r[12][3] = r[8][3];
                rc_eval(r, col);
            } else if (textured) {
                float tc[4], tx[4];
                for (k = 0; k < 4; k++)
                    tc[k] = pa * a->t[0][k] + pb * b->t[0][k] + pc * c->t[0][k];
                if (s_stage_mode[0]) {
                    if (!stage_sample(0, tc, ps, &lod, tx))
                        continue;
                    for (k = 0; k < 4; k++)
                        col[k] *= tx[k];
                }
            }
            if (s_vsh.atest_enable && !alpha_test_pass(col[3])) {
                s_vsh.a_rejected++;
                continue;
            }
            if (zs == 2) {                 /* failed stencil or depth */
                stencil_apply(sp, zs_op);
                continue;
            }
            if (use_s)
                stencil_apply(sp, s_vsh.stencil_op[2]);
            if (use_z && s_vsh.depth_mask)
                *zp = z;
            argb = ((uint32_t)(clamp01(col[3]) * 255.0f + 0.5f) << 24)
                 | ((uint32_t)(clamp01(col[0]) * 255.0f + 0.5f) << 16)
                 | ((uint32_t)(clamp01(col[1]) * 255.0f + 0.5f) <<  8)
                 |  (uint32_t)(clamp01(col[2]) * 255.0f + 0.5f);
            put_pixel(mem, bpp, x, y, argb);
        }
    }
    s_vsh.tris++;
    s_gpu.tris_drawn++;
    s_gpu.drawn_offset = s_gpu.color_offset;
    surf_mark_written();
}

/* ---- RECOMP_PB_FAST: the same arithmetic, reordered --------------------
 *
 * Three fast paths for raster_tri_xf, on by default; RECOMP_PB_FAST=0 keeps
 * the reference loop above. Each one moves work out of the pixel loop without
 * changing a single operation that produces a pixel, so the two paths agree
 * bit for bit -- RECOMP_PB_FAST_AB=1 checks exactly that, per batch.
 *
 *  (a) Decoded-texture cache. A bound texture is decoded to ARGB8888 once, in
 *      8x8 tiles on first touch, through sample_tex itself (so the decode is
 *      the reference decode). Key and invalidation follow nv2a_pb_state.h:
 *      offset + format + width + height + pitch; a new flip generation
 *      re-fingerprints the texture and drops the tiles if it changed. (The
 *      shared policy says first and last page; that misses a letterboxed
 *      movie, see tc_fingerprint, so this hashes every byte.) Texels are widened to floats through a 256-entry table holding
 *      exactly what rc_unpack computes.
 *  (b) Combiner setup hoisted. rc_eval re-decodes every input word per pixel
 *      and copies the 256-byte register file twice per stage. The plan below
 *      is decoded once per batch; per pixel the registers are evaluated in
 *      place (every input of a stage is read before any output is written,
 *      which is what the nr copy guaranteed), and only registers something
 *      writes are reset between pixels.
 *  (c) Edge setup per row. Each edge function is monotonic along a row, so the
 *      columns that can pass the inside test are found per row from an
 *      estimate refined with the exact per-pixel expression, and the columns
 *      outside are never visited. Inside, the edge functions are the reference
 *      expressions with their per-triangle operands hoisted, which leaves the
 *      rounding where it was. Additive stepping would round differently and is
 *      deliberately not used. With all three w == 1 (the pretransformed
 *      passthrough quads) the 1/w weights are skipped, and the normalising
 *      divide is skipped whenever its divisor is exactly 1.
 *
 * ponytail: a texture rewritten between two batches of one flip is not seen
 * (the policy's known gap); RECOMP_PB_FAST_AB reports it if it ever happens.
 */
static int s_fast_force = -1;            /* -1: env; 0/1: A/B override */

static int pb_fast(void)
{
    static int on = -1;
    if (s_fast_force >= 0)
        return s_fast_force;
    if (on < 0) {
        const char *e = recomp_env(RENV_PB_FAST);
        on = !(e && e[0] == '0');
    }
    return on;
}

static float s_u8f[256];                 /* k / 255.0f, as rc_unpack has it */

static void fast_init_tables(void)
{
    static int done;
    int k;
    if (done)
        return;
    for (k = 0; k < 256; k++)
        s_u8f[k] = (float)k / 255.0f;
    done = 1;
}

static void rc_unpack_fast(uint32_t c, float o[4])
{
    o[0] = s_u8f[(c >> 16) & 0xFF];
    o[1] = s_u8f[(c >>  8) & 0xFF];
    o[2] = s_u8f[ c        & 0xFF];
    o[3] = s_u8f[ c >> 24        ];
}

/* (a) ------------------------------------------------------------------- */
#define TC_ENTRIES   32
#define TC_TILE_LOG  3
#define TC_MAX_TEXELS (2048u * 2048u)

typedef struct {
    uint32_t offset, color, width, height, pitch;   /* key */
    uint32_t levels;                    /* held, not keyed: see tc_bind */
    uint32_t gen, used;
    uint32_t wseq;                      /* s_surf_wr_seq when last validated */
    uint64_t fp;
    uint32_t *texels;                   /* level 0, then each further level */
    uint8_t  *tile_ok;                  /* level 0's tiles, then the next's */
    uint32_t tiles, cap_texels, cap_tiles;
    /* Per level: size, first texel, first tile, tiles per row, byte offset
     * in guest memory from level 0. */
    uint32_t lw[MIP_MAX_LEVELS], lh[MIP_MAX_LEVELS];
    uint32_t ltex[MIP_MAX_LEVELS], ltile[MIP_MAX_LEVELS], ltx[MIP_MAX_LEVELS];
    uint32_t lbyte[MIP_MAX_LEVELS];
    int      live;
} TexCacheEntry;

static TexCacheEntry  s_tc[TC_ENTRIES];
static TexCacheEntry *s_stage_tc[4];
static int            s_fast_serial;    /* this batch draws on one thread */
static uint32_t       s_tc_clock;
static uint64_t       s_tc_tiles_filled, s_tc_refp, s_tc_dropped, s_tc_binds;
static uint64_t       s_tc_builds;      /* entries (re)built: a bind that missed */
static uint64_t       s_tc_feedback;    /* binds read live: texture is the target */

/* Bytes the sampler can read for this texture's first `levels` levels, 0
 * if it is not cacheable: not P8, whose palette is not in the key. */
static uint32_t tc_extent(const Texture *t, uint32_t levels)
{
    if (!t->valid || !t->width || !t->height || t->color == NV2A_TEX_P8
            || (uint64_t)t->width * t->height > TC_MAX_TEXELS)
        return 0;
    return nv2a_tex_extent(t->color, t->width, t->height, t->pitch, levels);
}

/* The cached levels a stage samples: its mip chain, or level 0. */
static uint32_t stage_tc_levels(int n)
{
    return s_stage_mip[n].mip ? s_stage_mip[n].levels : 1u;
}

/* The texture's bytes, all of them (nv2a_pb_state.h's policy).
 *
 * An earlier policy fingerprinted the first and the last page, and
 * RECOMP_PB_FAST_AB showed that is not enough: a letterboxed attract movie
 * (YUY2, 1024x512) has black bars in its first and last pages that never
 * change while the picture between them does, and the cache kept showing an
 * old frame. Hashing everything costs ~0.1 ms per megabyte per flip, and a
 * frame's textures total about that. */
static uint64_t tc_fingerprint(const Texture *t, uint32_t bytes)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset() + t->offset;
    return nv2a_tex_hash(mem, bytes, NV2A_TEX_HASH_SEED);
}

static TexCacheEntry *tc_bind(const Texture *t, uint32_t levels)
{
    uint32_t bytes = tc_extent(t, levels), gen = s_gpu.flips, i, l, tiles, texels;
    TexCacheEntry *e = NULL, *victim = &s_tc[0];
    uint64_t t0;

    if (!bytes)
        return NULL;
    /* YUY2/UYVY are movie frames, which the title's decoder thread rewrites
     * whenever it likes -- including between this bind and the draw, which
     * no per-flip or per-surface check can see (FAST_AB caught one: a whole
     * 1024x512 frame replaced mid-batch). They change every frame anyway, so
     * a cached copy buys nothing; read them live. */
    if (t->color == 0x24 || t->color == 0x25)
        return NULL;
    t0 = rt_now();
    s_tc_binds++;
    /* The level count is not part of the key: an entry holds level 0 first,
     * so one with the whole chain also serves a level-0-only bind of the
     * same texture (a stage that stops mipmapping, or another stage that
     * never did). One that holds fewer levels than asked is rebuilt in its
     * own slot, so the texture never has two entries duplicating level 0. */
    for (i = 0; i < TC_ENTRIES; i++) {
        TexCacheEntry *c = &s_tc[i];
        if (c->live && c->offset == t->offset && c->color == t->color
                && c->width == t->width && c->height == t->height
                && c->pitch == t->pitch) {
            if (c->levels >= levels)
                e = c;
            else
                victim = c;
            break;
        }
        if (!c->live || (victim->live && c->used < victim->used))
            victim = c;
    }
    if (e) {
        /* Re-check every level the entry holds, not only those asked for:
         * its fingerprint covers them all. */
        bytes = tc_extent(t, e->levels);
        if (e->gen != gen
                || surf_written_since(t->offset, t->offset + bytes) > e->wseq) {
            uint64_t fp = tc_fingerprint(t, bytes);
            s_tc_refp++;
            if (fp != e->fp) {
                memset(e->tile_ok, 0, e->tiles);
                e->fp = fp;
                s_tc_dropped++;
            }
            e->gen = gen;
            e->wseq = s_surf_wr_seq;
        }
    } else {
        e = victim;
        e->live = 0;
        s_tc_builds++;
        texels = tiles = 0;
        for (l = 0; l < levels; l++) {
            uint32_t w = nv2a_tex_level_dim(t->width, l), h = nv2a_tex_level_dim(t->height, l);
            e->lw[l] = w; e->lh[l] = h;
            e->ltex[l] = texels; e->ltile[l] = tiles;
            e->ltx[l] = (w + 7u) >> TC_TILE_LOG;
            e->lbyte[l] = nv2a_tex_level_offset(t->color, t->width, t->height, l);
            texels += w * h;
            tiles += e->ltx[l] * ((h + 7u) >> TC_TILE_LOG);
        }
        if (e->cap_texels < texels) {
            uint32_t *n = (uint32_t *)realloc(e->texels, (size_t)texels * 4u);
            if (!n) { s_rt.tex += rt_now() - t0; return NULL; }
            e->texels = n;
            e->cap_texels = texels;
        }
        if (e->cap_tiles < tiles) {
            uint8_t *n = (uint8_t *)realloc(e->tile_ok, tiles);
            if (!n) { s_rt.tex += rt_now() - t0; return NULL; }
            e->tile_ok = n;
            e->cap_tiles = tiles;
        }
        e->offset = t->offset; e->color = t->color;
        e->width = t->width; e->height = t->height; e->pitch = t->pitch;
        e->levels = levels;
        e->tiles = tiles;
        memset(e->tile_ok, 0, tiles);
        e->fp = tc_fingerprint(t, bytes);
        e->gen = gen;
        e->wseq = s_surf_wr_seq;
        e->live = 1;
    }
    e->used = ++s_tc_clock;
    s_rt.tex += rt_now() - t0;
    return e;
}

void nv2a_pb_exec_tc_stats(struct nv2a_pb_tc_stats *out)
{
    uint32_t i;
    memset(out, 0, sizeof *out);
    for (i = 0; i < TC_ENTRIES; i++)
        out->entries += s_tc[i].live != 0;
    out->binds = s_tc_binds;
    out->builds = s_tc_builds;
    /* Every counted bind either found an entry or built one. */
    out->hits = s_tc_binds - s_tc_builds;
    out->dropped = s_tc_dropped;
}

/* A tile's ready flag, read and published across raster threads: the
 * texels a filled flag guards must be visible before the flag is. */
#if defined(_MSC_VER) && !defined(__clang__)
static inline int tile_ready(const uint8_t *f)
{
    return ReadAcquire8((const volatile CHAR *)f);    /* winnt.h, any arch */
}
static inline void tile_publish(uint8_t *f)
{
    WriteRelease8((volatile CHAR *)f, 1);
}
#else
static inline int tile_ready(const uint8_t *f)
{
    return __atomic_load_n(f, __ATOMIC_ACQUIRE);
}
static inline void tile_publish(uint8_t *f)
{
    __atomic_store_n(f, (uint8_t)1, __ATOMIC_RELEASE);
}
#endif

/* Tile fills from raster threads take turns; the executor thread alone
 * binds, drops and resizes entries, between triangles. */
#if defined(_WIN32)
static SRWLOCK s_tc_lock = SRWLOCK_INIT;
#define TC_LOCK()   AcquireSRWLockExclusive(&s_tc_lock)
#define TC_UNLOCK() ReleaseSRWLockExclusive(&s_tc_lock)
#else
static pthread_mutex_t s_tc_lock = PTHREAD_MUTEX_INITIALIZER;
#define TC_LOCK()   pthread_mutex_lock(&s_tc_lock)
#define TC_UNLOCK() pthread_mutex_unlock(&s_tc_lock)
#endif

static void tc_fill_locked(TexCacheEntry *e, const Texture *t, uint32_t l,
                           uint32_t tile);

/* Tile `tile` (index within level l) of level l of entry e, whose level 0
 * is texture t. */
static void tc_fill(TexCacheEntry *e, const Texture *t, uint32_t l, uint32_t tile)
{
    TC_LOCK();
    if (!e->tile_ok[e->ltile[l] + tile])    /* another thread may have */
        tc_fill_locked(e, t, l, tile);
    TC_UNLOCK();
}

static void tc_fill_locked(TexCacheEntry *e, const Texture *t, uint32_t l,
                           uint32_t tile)
{
    uint32_t w = e->lw[l], h = e->lh[l];
    uint32_t x0 = (tile % e->ltx[l]) << TC_TILE_LOG;
    uint32_t y0 = (tile / e->ltx[l]) << TC_TILE_LOG;
    uint32_t x1 = x0 + 8u < w ? x0 + 8u : w;
    uint32_t y1 = y0 + 8u < h ? y0 + 8u : h;
    uint32_t x, y, *out = e->texels + e->ltex[l];
    uint64_t t0 = rt_now();
    Texture lt = *t;

    lt.offset += e->lbyte[l];
    lt.width = w;
    lt.height = h;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            uint32_t texel = 0;
            sample_tex(&lt, x, y, &texel); /* cacheable formats always decode */
            out[(size_t)y * w + x] = texel;
        }
    tile_publish(&e->tile_ok[e->ltile[l] + tile]);
    s_tc_tiles_filled++;
    s_rt.tex += rt_now() - t0;
}

/* Texel (iu, iv) of level l, already wrapped, from the cache; -1 on
 * either axis is the border colour (wrap_signed). */
static inline void texel_cached(TexCacheEntry *e, int n, uint32_t l, int iu, int iv,
                                float o[4])
{
    uint32_t tile;
    if ((iu | iv) < 0) {
        rc_unpack_fast(s_stage_tex[n].border, o);
        return;
    }
    tile = ((uint32_t)iv >> TC_TILE_LOG) * e->ltx[l] + ((uint32_t)iu >> TC_TILE_LOG);
    if (!tile_ready(&e->tile_ok[e->ltile[l] + tile]))
        tc_fill(e, &s_stage_tex[n], l, tile);
    rc_unpack_fast(e->texels[e->ltex[l] + (size_t)iv * e->lw[l] + (uint32_t)iu], o);
}

/* Level l of the stage-n entry e at (s, t), bilinear (lin) or nearest: the
 * live sampler's arithmetic on cached texels. Level 0 keeps the stage's
 * scale (texels for a linear texture); a further level is always swizzled
 * or DXT, so normalised.
 *
 * A bilinear tap whose weights are both 0 -- a pixel centre on a texel
 * centre, as in a 1:1 screen quad -- is that one texel: the four-tap blend
 * of the live sampler gives exactly it (the other taps are weighted by an
 * exact 0), so the other three are not read. */
static inline void fast_level(TexCacheEntry *e, int n, uint32_t l, int lin,
                              float s, float t, float o[4])
{
    const Texture *tx = &s_stage_tex[n];
    uint32_t w = e->lw[l], h = e->lh[l];
    float sw = l ? (float)w : s_stage_scale[n][0];
    float sh = l ? (float)h : s_stage_scale[n][1];
    int iu, iv;

    if (lin) {
        float wu, wv, c[4][4];
        int k;
        int u[2], v[2];
        tent_setup(sw, sh, s, t, &iu, &iv, &wu, &wv);
        u[0] = wrap_signed(iu, w, tx->addr_u);
        v[0] = wrap_signed(iv, h, tx->addr_v);
        if (wu == 0.0f && wv == 0.0f) {
            texel_cached(e, n, l, u[0], v[0], o);
            return;
        }
        u[1] = wrap_signed(iu + 1, w, tx->addr_u);
        v[1] = wrap_signed(iv + 1, h, tx->addr_v);
        if (u[1] == u[0] + 1 && v[1] == v[0] + 1
                && (u[0] & 7) != 7 && (v[0] & 7) != 7) {
            /* all four in one tile: one check, adjacent texels */
            uint32_t tile = ((uint32_t)v[0] >> TC_TILE_LOG) * e->ltx[l]
                          + ((uint32_t)u[0] >> TC_TILE_LOG);
            const uint32_t *p0;
            if (!tile_ready(&e->tile_ok[e->ltile[l] + tile]))
                tc_fill(e, tx, l, tile);
            p0 = &e->texels[e->ltex[l] + (size_t)v[0] * w + (uint32_t)u[0]];
            rc_unpack_fast(p0[0], c[0]);
            rc_unpack_fast(p0[1], c[1]);
            rc_unpack_fast(p0[w], c[2]);
            rc_unpack_fast(p0[w + 1], c[3]);
        } else {
            for (k = 0; k < 4; k++)
                texel_cached(e, n, l, u[k & 1], v[k >> 1], c[k]);
        }
        bilerp(c, wu, wv, o);
        return;
    }
    iu = wrap_signed((int)floorf(s * sw), w, tx->addr_u);
    iv = wrap_signed((int)floorf(t * sh), h, tx->addr_v);
    texel_cached(e, n, l, iu, iv, o);
}

/* stage_sample, reading texels from the cache when the stage has an entry.
 * A cube stage has none (fast_batch_setup) and samples live. */
static int stage_sample_fast(int n, const float tc[4], float q, const LodTri *L,
                             float o[4])
{
    TexCacheEntry *e = s_stage_tc[n];
    int mode = s_stage_mode[n];
    float s, t;

    if (!e || mode == 0 || mode == 4 || mode == 5)
        return stage_sample(n, tc, q, L, o);
    s = tc[0]; t = tc[1];
    if (mode == 1 && tc[3] != 0.0f && tc[3] != 1.0f) {   /* 2D projective */
        s /= tc[3]; t /= tc[3];
    }
    if (s_stage_mip[n].mip) {              /* as stage_sample_mip */
        const StageMip *m = &s_stage_mip[n];
        float lod = stage_lod(n, L, tc, q), f, o1[4];
        int lin = lod > 0.0f ? m->min_lin : m->mag_lin;
        uint32_t l0, l1;
        nv2a_tex_mip_levels(m->mip, lod, m->levels, &l0, &l1, &f);
        fast_level(e, n, l0, lin, s, t, o);
        if (f > 0.0f) {
            fast_level(e, n, l1, lin, s, t, o1);
            mip_blend(o, o1, f);
        }
        return 1;
    }
    fast_level(e, n, 0, s_stage_tent[n], s, t, o);
    return 1;
}

/* (b) -------------------------------------------------------------------
 *
 * The combiner plan (nv2a_rc_plan_build, nv2a_combiner.c): decoded once per
 * batch, evaluated per pixel in place, bit-identical to rc_eval. */
static Nv2aRcPlan s_rcp;

static void rc_plan_build(void)
{
    float fog[4];

    /* What the reference loop builds before sampling: fog in r3, RGBA. */
    rc_load();
    rc_unpack(s_vsh.fog_color, fog);
    { float t_ = fog[0]; fog[0] = fog[2]; fog[2] = t_; }
    fog[3] = 1.0f;
    nv2a_rc_plan_build(&s_rcp, &s_rc, fog, s_vsh.fog_enable);
}

/* (c) ------------------------------------------------------------------- */

/* An edge function at column x of a row, exactly as raster_tri_xf has it:
 * (dx) * (py - vy) - (dy) * (px - vx), with py - vy passed in as `p`. */
#define EDGE_AT(dx, p, dy, vx, x) ((dx) * (p) - (dy) * (((float)(x) + 0.5f) - (vx)))

/* First column in [lo, hi] at which a predicate that is true on a prefix of
 * the row turns false (hi if it never does), starting from an estimate. The
 * walk makes the answer exact whatever the estimate; the estimate only makes
 * it short. */
#define PREFIX_END(pred, est, lo, hi, out) do { \
        int b_ = (est); \
        if (b_ < (lo)) b_ = (lo); \
        if (b_ > (hi)) b_ = (hi); \
        while (b_ > (lo) && !(pred(b_ - 1))) b_--; \
        while (b_ < (hi) && (pred(b_))) b_++; \
        (out) = b_; } while (0)

/* Columns of [lo, hi) where one edge is >= 0 (sign > 0) or <= 0 (sign < 0),
 * as [*a, *b). An edge function is monotonic along a row, so this is one
 * interval. */
static void edge_interval(float dx, float p, float dy, float vx, int sign,
                          int lo, int hi, int *a, int *b)
{
    int end, est;
    double root;

    if (dy == 0.0f) {                     /* constant along the row */
        float w = EDGE_AT(dx, p, dy, vx, lo);
        int in = sign > 0 ? w >= 0 : w <= 0;
        *a = lo; *b = in ? hi : lo;
        return;
    }
    /* w(x) = 0 at px = vx + dx*p/dy. */
    root = (double)vx + (double)dx * (double)p / (double)dy - 0.5;
    if (root < (double)lo - 1.0) est = lo;
    else if (root > (double)hi + 1.0) est = hi;
    else est = (int)floor(root) + 1;

    /* dy > 0: w falls with x; dy < 0: it rises. */
    if ((dy > 0.0f) == (sign > 0)) {
        /* the predicate holds on a prefix */
#define P_(x) (sign > 0 ? EDGE_AT(dx, p, dy, vx, (x)) >= 0 : EDGE_AT(dx, p, dy, vx, (x)) <= 0)
        PREFIX_END(P_, est, lo, hi, end);
        *a = lo; *b = end;
    } else {
        /* on a suffix: it ends where its negation's prefix ends */
#define N_(x) (!P_(x))
        PREFIX_END(N_, est, lo, hi, end);
        *a = end; *b = hi;
#undef N_
#undef P_
    }
}

/* Everything a triangle's pixels need, computed once per triangle, so the
 * pixel loop can run over any subset of rows on any thread. */
typedef struct {
    const XfVert *a, *b, *c;
    uint8_t *mem;
    uint32_t bpp;
    float area, inv_w[3], zscale;
    float e0dx, e0dy, e1dx, e1dy, e2dx, e2dy, s_;
    int t0, t1, t2;   /* edge_owns_tie per edge */
    int minx, maxx, miny, maxy, use_z, use_s, use_rc, textured, unit_w;
    LodTri lod;
} FastTri;

/* Index of the lowest set bit of a non-zero word. */
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static inline int ctz32(uint32_t v)
{
    unsigned long i;
    _BitScanForward(&i, v);
    return (int)i;
}
#else
static inline int ctz32(uint32_t v) { return __builtin_ctz(v); }
#endif

/* Rows y0, y0 + step, ... < maxy of triangle T. */
static void fast_rows(const FastTri *T, int y0, int step)
{
    const XfVert *a = T->a, *b = T->b, *c = T->c;
    const float area = T->area, zscale = T->zscale;
    const float e0dx = T->e0dx, e0dy = T->e0dy, e1dx = T->e1dx, e1dy = T->e1dy;
    const float e2dx = T->e2dx, e2dy = T->e2dy;
    const int minx = T->minx, maxx = T->maxx, use_z = T->use_z, use_s = T->use_s;
    const int use_rc = T->use_rc;
    const float s_ = T->s_;
    const int t0 = T->t0, t1 = T->t1, t2 = T->t2;
    float r[16][4];
    int x, y, n;

    if (use_rc)
        memcpy(r, s_rcp.tmpl, sizeof r);

    for (y = y0; y < T->maxy; y += step) {
        float py = (float)y + 0.5f;
        float p0 = py - a->y, p1 = py - b->y, p2 = py - c->y;
        int lo, hi, ga, gb, la_, lb_, ia, ib;

        /* Columns where all three are >= 0, and where all three are <= 0:
         * inclusive, so a superset of what the top-left test below keeps. */
        edge_interval(e0dx, p0, e0dy, a->x, 1, minx, maxx, &ga, &gb);
        edge_interval(e1dx, p1, e1dy, b->x, 1, ga, gb > ga ? gb : ga, &ia, &ib);
        ga = ia; gb = ib;
        if (gb > ga) {
            edge_interval(e2dx, p2, e2dy, c->x, 1, ga, gb, &ia, &ib);
            ga = ia; gb = ib;
        }
        edge_interval(e0dx, p0, e0dy, a->x, -1, minx, maxx, &la_, &lb_);
        if (lb_ > la_) {
            edge_interval(e1dx, p1, e1dy, b->x, -1, la_, lb_, &ia, &ib);
            la_ = ia; lb_ = ib;
        }
        if (lb_ > la_) {
            edge_interval(e2dx, p2, e2dy, c->x, -1, la_, lb_, &ia, &ib);
            la_ = ia; lb_ = ib;
        }
        if (gb <= ga && lb_ <= la_)
            continue;
        if (gb <= ga)        { lo = la_; hi = lb_; }
        else if (lb_ <= la_) { lo = ga;  hi = gb;  }
        else { lo = ga < la_ ? ga : la_; hi = gb > lb_ ? gb : lb_; }

        for (x = lo; x < hi; x++) {
            float px = (float)x + 0.5f;
            float w0 = e0dx * p0 - e0dy * (px - a->x);
            float w1 = e1dx * p1 - e1dy * (px - b->x);
            float w2 = e2dx * p2 - e2dy * (px - c->x);
            float la, lb, lc, pa, pb, pc, ps, col[4], z = 0.0f;
            float *zp = NULL;
            uint8_t *sp = NULL;
            uint32_t argb, zs_op = 0;
            int k, zs = 0;

            if (!(edge_in(s_, w0, t0) && edge_in(s_, w1, t1)
                  && edge_in(s_, w2, t2)))
                continue;
            la = w1 / area; lb = w2 / area; lc = w0 / area;

            if (use_z) {
                z = (la * a->z + lb * b->z + lc * c->z) * zscale;
                zp = &s_zbuf[(size_t)y * s_zbuf_w + (size_t)x];
            }
            if (use_s)
                sp = &s_sbuf[(size_t)y * s_zbuf_w + (size_t)x];
            if ((use_z || use_s)
                    && (zs = zs_early(use_s, use_z, z, zp, sp, &zs_op)) == 1)
                continue;

            if (T->unit_w) {
                pa = la; pb = lb; pc = lc;           /* x * (1/1) == x */
            } else {
                pa = la * T->inv_w[0]; pb = lb * T->inv_w[1]; pc = lc * T->inv_w[2];
            }
            ps = pa + pb + pc;
            if (ps != 0.0f && ps != 1.0f) { pa /= ps; pb /= ps; pc /= ps; }

            for (k = 0; k < 4; k++)
                col[k] = pa * a->d0[k] + pb * b->d0[k] + pc * c->d0[k];

            if (use_rc) {
                float tc[4];
                uint32_t d = s_rcp.dirty;
                while (d) {
                    int reg = ctz32(d);
                    memcpy(r[reg], s_rcp.tmpl[reg], sizeof r[reg]);
                    d &= d - 1;
                }
                memcpy(r[4], col, sizeof col);
                for (k = 0; k < 4; k++)
                    r[5][k] = pa * a->d1[k] + pb * b->d1[k] + pc * c->d1[k];
                if (s_rcp.fog_var)
                    r[3][3] = clamp01(pa * a->fog + pb * b->fog + pc * c->fog);
                for (n = 0; n < 4; n++) {
                    if (!s_stage_mode[n]) {
                        /* NONE reads (0, 0, 0, 1): see stage_sample */
                        r[8 + n][0] = r[8 + n][1] = r[8 + n][2] = 0.0f;
                        r[8 + n][3] = 1.0f;
                        continue;
                    }
                    for (k = 0; k < 4; k++)
                        tc[k] = pa * a->t[n][k] + pb * b->t[n][k] + pc * c->t[n][k];
                    if (!stage_sample_fast(n, tc, ps, &T->lod, r[8 + n]))
                        break;                 /* CLIPPLANE killed the pixel */
                }
                if (n < 4)
                    continue;
                r[12][3] = r[8][3];
                nv2a_rc_plan_eval(&s_rcp, r, col);
            } else if (T->textured) {
                float tc[4], tx[4];
                for (k = 0; k < 4; k++)
                    tc[k] = pa * a->t[0][k] + pb * b->t[0][k] + pc * c->t[0][k];
                if (s_stage_mode[0]) {
                    if (!stage_sample_fast(0, tc, ps, &T->lod, tx))
                        continue;
                    for (k = 0; k < 4; k++)
                        col[k] *= tx[k];
                }
            }
            if (s_vsh.atest_enable && !alpha_test_pass(col[3])) {
                if (t_cnt) t_cnt->a_rejected++; else s_vsh.a_rejected++;
                continue;
            }
            if (zs == 2) {                 /* failed stencil or depth */
                stencil_apply(sp, zs_op);
                continue;
            }
            if (use_s)
                stencil_apply(sp, s_vsh.stencil_op[2]);
            if (use_z && s_vsh.depth_mask)
                *zp = z;
            argb = ((uint32_t)(clamp01(col[3]) * 255.0f + 0.5f) << 24)
                 | ((uint32_t)(clamp01(col[0]) * 255.0f + 0.5f) << 16)
                 | ((uint32_t)(clamp01(col[1]) * 255.0f + 0.5f) <<  8)
                 |  (uint32_t)(clamp01(col[2]) * 255.0f + 0.5f);
            put_pixel(T->mem, T->bpp, x, y, argb);
        }
    }
}

/* Worker threads for big triangles (upstream's xf_rows_parallel, on Win32
 * and POSIX threads).
 *
 * A full-screen pass is two triangles of 300,000 pixels through the
 * combiners, and it splits cleanly by row: a pixel reads and writes only its
 * own colour, depth and stencil, so interleaved rows on N threads need no
 * locking and give the same pixels as one thread. What they share is
 * handled apart: the counters go to a per-thread RasterCount (t_cnt) merged
 * afterwards, in the order one thread would have produced them; a texture
 * cache tile is filled under s_tc_lock and published with a release store.
 * A batch that samples the surface it draws into reads pixels other rows
 * write, so it stays on one thread (s_fast_serial), as does the reference
 * rasteriser. Small triangles stay on the executor thread, where waking
 * workers would cost more than it saves.
 * RECOMP_RASTER_THREADS=<n> sets the count (1 = off); the default leaves a
 * few cores for the title and the host. */
#define NV_RASTER_MAX_THREADS 16
#define NV_RASTER_MT_MIN_PIXELS 8192

static struct {
    int n;                                  /* threads incl. the caller */
    const FastTri *tri;
    /* One slot per thread, 128 bytes apart so no two threads' counters
     * share a cache line however the array happens to be aligned. */
    union { RasterCount c; char pad[128]; } cnt[NV_RASTER_MAX_THREADS];
#if defined(_WIN32)
    HANDLE start[NV_RASTER_MAX_THREADS], done;
    volatile LONG pending;
#else
    pthread_mutex_t mu;
    pthread_cond_t go, done;
    unsigned gen;
    int pending;
#endif
} s_pool;

/* Thread k's share: rows miny + k, miny + k + n, ... */
static void raster_share(int k)
{
    RasterCount *cn = &s_pool.cnt[k].c;
    memset(cn, 0, sizeof *cn);
    cn->pixel_max = s_gpu.pixel_max;
    cn->max_y = -1;
    t_cnt = cn;
    fast_rows(s_pool.tri, s_pool.tri->miny + k, s_pool.n);
    t_cnt = NULL;
}

#if defined(_WIN32)
static DWORD WINAPI raster_worker(LPVOID arg)
{
    int k = (int)(intptr_t)arg;
    for (;;) {
        WaitForSingleObject(s_pool.start[k], INFINITE);
        raster_share(k);
        if (InterlockedDecrement(&s_pool.pending) == 0)
            SetEvent(s_pool.done);
    }
    return 0;
}
#else
static void *raster_worker(void *arg)
{
    int k = (int)(intptr_t)arg;
    unsigned seen = 0;
    pthread_mutex_lock(&s_pool.mu);
    for (;;) {
        while (s_pool.gen == seen)
            pthread_cond_wait(&s_pool.go, &s_pool.mu);
        seen = s_pool.gen;
        pthread_mutex_unlock(&s_pool.mu);
        raster_share(k);
        pthread_mutex_lock(&s_pool.mu);
        if (--s_pool.pending == 0)
            pthread_cond_signal(&s_pool.done);
    }
    return NULL;
}
#endif

static int raster_pool_size(void)
{
    static int init;
    if (!init) {
        const char *e = recomp_env(RENV_RASTER_THREADS);
        int n, k, ncpu;
#if defined(_WIN32)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        ncpu = (int)si.dwNumberOfProcessors;
#else
        ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
        init = 1;
        n = e ? atoi(e) : ncpu - 4;
        if (n < 1) n = 1;
        if (n > NV_RASTER_MAX_THREADS) n = NV_RASTER_MAX_THREADS;
#if defined(_WIN32)
        if (n > 1 && !(s_pool.done = CreateEventA(NULL, FALSE, FALSE, NULL)))
            n = 1;
        if (n > 1) {
            for (k = 0; k < n - 1; k++) {
                HANDLE h = NULL;
                s_pool.start[k] = CreateEventA(NULL, FALSE, FALSE, NULL);
                if (s_pool.start[k])
                    h = CreateThread(NULL, 0, raster_worker, (LPVOID)(intptr_t)k, 0, NULL);
                if (!h) { n = k + 1; break; }   /* workers 0..k-1 and the caller */
                CloseHandle(h);
            }
        }
#else
        if (n > 1) {
            pthread_mutex_init(&s_pool.mu, NULL);
            pthread_cond_init(&s_pool.go, NULL);
            pthread_cond_init(&s_pool.done, NULL);
            for (k = 0; k < n - 1; k++) {
                pthread_t th;
                if (pthread_create(&th, NULL, raster_worker, (void *)(intptr_t)k)) {
                    n = k + 1;
                    break;
                }
                pthread_detach(th);
            }
        }
#endif
        s_pool.n = n;
        if (n > 1)
            fprintf(stderr, "[GPU] raster: %d threads for triangles of %d+ pixels\n",
                    n, NV_RASTER_MT_MIN_PIXELS);
    }
    return s_pool.n;
}

/* The per-thread counters into the globals, as one thread would have left
 * them: sums, and the brightest pixel -- the first one, in raster order, of
 * the brightest colour, which is the one on the lowest row. */
static void raster_merge(int n)
{
    int k, best = -1;
    for (k = 0; k < n; k++) {
        const RasterCount *cn = &s_pool.cnt[k].c;
        s_gpu.pixels += cn->pixels;
        s_zpass_count += cn->zpass;
        s_vsh.z_rejected += cn->z_rejected;
        s_vsh.a_rejected += cn->a_rejected;
        if (cn->max_y < 0)
            continue;                      /* never beat the starting value */
        if (best < 0
                || (cn->pixel_max & 0x00FFFFFFu) > (s_pool.cnt[best].c.pixel_max & 0x00FFFFFFu)
                || ((cn->pixel_max & 0x00FFFFFFu) == (s_pool.cnt[best].c.pixel_max & 0x00FFFFFFu)
                    && cn->max_y < s_pool.cnt[best].c.max_y))
            best = k;
    }
    if (best >= 0)
        s_gpu.pixel_max = s_pool.cnt[best].c.pixel_max;
}

static void fast_rows_parallel(const FastTri *T)
{
    int n, k;
    long px = (long)(T->maxx - T->minx) * (T->maxy - T->miny);

    if (s_fast_serial || px < NV_RASTER_MT_MIN_PIXELS
            || (n = raster_pool_size()) <= 1 || T->maxy - T->miny < n) {
        fast_rows(T, T->miny, 1);
        return;
    }
    s_pool.tri = T;
#if defined(_WIN32)
    s_pool.pending = n - 1;
    for (k = 0; k < n - 1; k++)
        SetEvent(s_pool.start[k]);
    raster_share(n - 1);
    WaitForSingleObject(s_pool.done, INFINITE);
#else
    pthread_mutex_lock(&s_pool.mu);
    s_pool.pending = n - 1;
    s_pool.gen++;
    pthread_cond_broadcast(&s_pool.go);
    pthread_mutex_unlock(&s_pool.mu);
    raster_share(n - 1);
    pthread_mutex_lock(&s_pool.mu);
    while (s_pool.pending)
        pthread_cond_wait(&s_pool.done, &s_pool.mu);
    pthread_mutex_unlock(&s_pool.mu);
#endif
    (void)k;
    raster_merge(n);
}

static void raster_tri_xf_fast(const XfVert *a, const XfVert *b, const XfVert *c,
                               int textured)
{
    FastTri T;
    float area;
    int minx, maxx, miny, maxy;

    T.mem = (uint8_t *)xbox_GetMemoryOffset();
    T.bpp = surface_bpp();
    if (T.bpp != 4 && T.bpp != 2)
        return;
    if (!a->ok || !b->ok || !c->ok) {
        s_vsh.tris_clipped++;
        return;
    }
    area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0.0f)
        return;
    if (s_vsh.cull_enable) {
        int cw = area > 0.0f;
        int front = (s_vsh.front_face == 0x900) ? cw : !cw;
        if (recomp_env(RENV_PB_CULL_FLIP))
            front = !front;
        if ((s_vsh.cull_face == 0x405 && !front)
         || (s_vsh.cull_face == 0x404 && front)
         || s_vsh.cull_face == 0x408) {
            s_vsh.tris_culled++;
            return;
        }
    }
    if (!surface_begin_batch(T.mem))
        return;
    T.use_z = s_vsh.depth_enable && vsh_zbuf_ready();
    T.use_s = vsh_stencil_ready();
    T.use_rc = vsh_rc_active();
    T.textured = textured;
    T.zscale = 1.0f / vsh_zmax();

    minx = (int)floorf(fminf(a->x, fminf(b->x, c->x)));
    maxx = (int)ceilf (fmaxf(a->x, fmaxf(b->x, c->x)));
    miny = (int)floorf(fminf(a->y, fminf(b->y, c->y)));
    maxy = (int)ceilf (fmaxf(a->y, fmaxf(b->y, c->y)));
    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    T.a = a; T.b = b; T.c = c;
    T.area = area;
    T.minx = minx; T.maxx = maxx; T.miny = miny; T.maxy = maxy;
    T.inv_w[0] = 1.0f / a->w; T.inv_w[1] = 1.0f / b->w; T.inv_w[2] = 1.0f / c->w;
    T.unit_w = a->w == 1.0f && b->w == 1.0f && c->w == 1.0f;
    lod_tri_setup(a, b, c, area, T.inv_w, &T.lod);
    T.e0dx = b->x - a->x; T.e0dy = b->y - a->y;
    T.e1dx = c->x - b->x; T.e1dy = c->y - b->y;
    T.e2dx = a->x - c->x; T.e2dy = a->y - c->y;
    T.s_ = area > 0.0f ? 1.0f : -1.0f;
    T.t0 = edge_owns_tie(T.s_, T.e0dx, T.e0dy);
    T.t1 = edge_owns_tie(T.s_, T.e1dx, T.e1dy);
    T.t2 = edge_owns_tie(T.s_, T.e2dx, T.e2dy);

    fast_rows_parallel(&T);

    s_vsh.tris++;
    s_gpu.tris_drawn++;
    s_gpu.drawn_offset = s_gpu.color_offset;
    surf_mark_written();
}

/* Per-batch setup for the fast path: cache entries for the sampled stages and
 * the combiner plan. */
static void fast_batch_setup(void)
{
    int n;
    s_fast_serial = 0;
    uint32_t lo = s_gpu.color_offset ? dma_resolve(s_gpu.color_offset) : 0;
    uint32_t hi = lo + (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch;
    fast_init_tables();
    for (n = 0; n < 4; n++) {
        int mode = s_stage_mode[n];
        const Texture *t = &s_stage_tex[n];
        uint32_t lv = stage_tc_levels(n);
        s_stage_tc[n] = (mode != 0 && mode != 4 && mode != 5
                         && !(mode == 3 && t->face_stride))
                      ? tc_bind(t, lv) : NULL;
        /* A stage reading the surface this batch draws into sees its own
         * earlier pixels: the reference sampler reads memory as it changes,
         * a cached copy would not. A fade can draw the two back
         * buffers with themselves as texture, and from the cache it
         * came out one step darker on ~4000 pixels per pass. Read those
         * live, like the reference does. */
        if (s_stage_tc[n] && lo && t->offset < hi
                && lo < t->offset + tc_extent(t, lv)) {
            s_stage_tc[n] = NULL;
            s_tc_feedback++;
        }
        /* Any stage that may read the target -- cached or not -- would see
         * pixels other rows are writing: draw that batch on one thread. */
        if (mode != 0 && mode != 4 && lo && t->valid && t->offset < hi
                && lo < t->offset + (t->face_stride ? 6u * t->face_stride
                        : nv2a_tex_extent(t->color, t->width, t->height, t->pitch, lv)))
            s_fast_serial = 1;
    }
    if (vsh_rc_active())
        rc_plan_build();
}

#define NV097_SET_TEXTURE_CONTROL0_M 0x1B0C

/* ---- Clipping -----------------------------------------------------------
 *
 * The XDK epilogue leaves oPos as (x/w * scale + offset, ..., w): screen
 * pixels, already divided, with the clip-space w beside them. Multiplying
 * back by w gives (x*w, y*w, z*w, w), which is an affine image of clip space
 * (the viewport offset times w is linear in w), so a point on an edge in clip
 * space is the same linear blend here, and Sutherland-Hodgman can run on it
 * directly. The divide's reciprocal is rcc, which keeps the sign, so this
 * holds for w < 0 too.
 *
 * Planes, in order:
 *   w >= CLIP_W_EPS                     the near plane the hardware clips to
 *   -CLIP_GUARD <= x, y <= CLIP_GUARD   a guard band, so a vertex just in
 *                                       front of the eye does not hand the
 *                                       edge functions 1e8-pixel operands
 *   z >= 0                              always: the NV2A clips at the near
 *                                       plane whatever the depth test says
 *                                       (xemu's ZCLAMP CULL; the GPU backends
 *                                       clip there too), so a depth-off batch
 *                                       crossing it must not draw the part
 *                                       in front of it (upstream #211)
 *   z <= zmax                           with depth on only, matching what
 *                                       this path drew before: the NV2A culls
 *                                       fragments beyond zmax
 *                                       (ZMIN_MAX_CONTROL CULL_NEAR_FAR, the
 *                                       XDK default), and since screen z is
 *                                       affine in x, y, clipping here is
 *                                       exactly that cull. Whether it culls
 *                                       with depth off too is open: xemu's
 *                                       ZCLAMP CULL is not gated on the depth
 *                                       test, and the GPU backends clip far
 *                                       with depth off
 *
 * A triangle with all three vertices inside every plane goes to the
 * rasteriser untouched, so nothing that drew before changes by a bit. Only
 * the vertices clipping creates are interpolated; the polygon is fanned from
 * its first vertex, which keeps its winding, so culling still sees the
 * triangle's real facing. Attributes blend linearly in clip space, exactly as
 * the hardware's clipper does.
 *
 * RECOMP_PB_CLIP=0 restores the old rule (drop any triangle with a vertex at
 * w <= 0 or beyond 1e6 pixels), for before/after comparisons.
 */
#define CLIP_W_EPS  1.0e-5f
#define CLIP_GUARD  16384.0f
#define CLIP_MAXV   16

typedef struct { XfVert v; float h[4]; } ClipVert;

static uint64_t s_clip_tris, s_clip_out, s_clip_gone, s_clip_unusable;
static uint64_t s_clip_why[4];
static uint64_t s_clip_origin;     /* clipped tris with all three at screen (0, 0) */      /* tris clipped: some w<=0, all w<=0, guard, z */

static int clip_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_PB_CLIP);
        on = !(e && e[0] == '0');
    }
    return on;
}

static float clip_dist(const ClipVert *p, int plane, float zmax)
{
    switch (plane) {
    case 0:  return p->h[3] - CLIP_W_EPS;
    case 1:  return p->h[0] + CLIP_GUARD * p->h[3];
    case 2:  return CLIP_GUARD * p->h[3] - p->h[0];
    case 3:  return p->h[1] + CLIP_GUARD * p->h[3];
    case 4:  return CLIP_GUARD * p->h[3] - p->h[1];
    case 5:  return p->h[2];
    default: return zmax * p->h[3] - p->h[2];
    }
}

static int clip_inside_all(const XfVert *v, int nplanes, float zmax)
{
    if (!(v->w >= CLIP_W_EPS))
        return 0;
    if (fabsf(v->x) > CLIP_GUARD || fabsf(v->y) > CLIP_GUARD)
        return 0;
    if (nplanes > 5 && v->z < 0.0f)
        return 0;
    if (nplanes > 6 && v->z > zmax)
        return 0;
    return 1;
}

static void clip_lerp(const ClipVert *a, const ClipVert *b, float t, ClipVert *o,
                      int plane, float zmax)
{
    int k, n;
    if (!(t >= 0.0f)) t = 0.0f;
    if (t > 1.0f)     t = 1.0f;
    o->v.fog = a->v.fog + t * (b->v.fog - a->v.fog);
    for (k = 0; k < 4; k++) {
        o->h[k] = a->h[k] + t * (b->h[k] - a->h[k]);
        o->v.d0[k] = a->v.d0[k] + t * (b->v.d0[k] - a->v.d0[k]);
        o->v.d1[k] = a->v.d1[k] + t * (b->v.d1[k] - a->v.d1[k]);
        for (n = 0; n < 4; n++)
            o->v.t[n][k] = a->v.t[n][k] + t * (b->v.t[n][k] - a->v.t[n][k]);
    }
    /* Put the new vertex exactly on the plane. The blend alone can miss by
     * more than the plane's own margin: w = 1e-5 between w = -168 and w = 415
     * is below a float's resolution there, and came out as 8e-15 or 0, which
     * the divide below turned into NaN. */
    switch (plane) {
    case 0: o->h[3] = CLIP_W_EPS;               break;
    case 1: o->h[0] = -CLIP_GUARD * o->h[3];    break;
    case 2: o->h[0] =  CLIP_GUARD * o->h[3];    break;
    case 3: o->h[1] = -CLIP_GUARD * o->h[3];    break;
    case 4: o->h[1] =  CLIP_GUARD * o->h[3];    break;
    case 5: o->h[2] = 0.0f;                     break;
    default: o->h[2] = zmax * o->h[3];          break;
    }
    o->v.w = o->h[3];
    o->v.x = o->h[0] / o->h[3];
    o->v.y = o->h[1] / o->h[3];
    o->v.z = o->h[2] / o->h[3];
    o->v.ok = 1;
}

static void clip_tri(const XfVert *a, const XfVert *b, const XfVert *c,
                     int textured, int fast)
{
    static ClipVert buf[2][CLIP_MAXV];
    const XfVert *in3[3];
    ClipVert *src = buf[0], *dst = buf[1];
    float zmax = vsh_zmax();
    int nplanes = s_vsh.depth_enable ? 7 : 6;   /* far with depth on only */
    int n = 3, plane, i, k;

    if (!a->ok || !b->ok || !c->ok) {
        s_clip_unusable++;
        s_vsh.tris_clipped++;
        return;
    }
    if (!clip_enabled()) {
        /* The old rule. */
        if (!(a->w > 0.0f && b->w > 0.0f && c->w > 0.0f
              && fabsf(a->x) < 1.0e6f && fabsf(a->y) < 1.0e6f
              && fabsf(b->x) < 1.0e6f && fabsf(b->y) < 1.0e6f
              && fabsf(c->x) < 1.0e6f && fabsf(c->y) < 1.0e6f)) {
            s_vsh.tris_clipped++;
            return;
        }
        if (fast) raster_tri_xf_fast(a, b, c, textured);
        else      raster_tri_xf(a, b, c, textured);
        return;
    }
    if (clip_inside_all(a, nplanes, zmax) && clip_inside_all(b, nplanes, zmax)
            && clip_inside_all(c, nplanes, zmax)) {
        if (fast) raster_tri_xf_fast(a, b, c, textured);
        else      raster_tri_xf(a, b, c, textured);
        return;
    }

    s_clip_tris++;
    {
        int behind = (a->w < CLIP_W_EPS) + (b->w < CLIP_W_EPS) + (c->w < CLIP_W_EPS);
        if (behind == 3)      s_clip_why[1]++;
        else if (behind)      s_clip_why[0]++;
        else if (fabsf(a->x) > CLIP_GUARD || fabsf(a->y) > CLIP_GUARD
              || fabsf(b->x) > CLIP_GUARD || fabsf(b->y) > CLIP_GUARD
              || fabsf(c->x) > CLIP_GUARD || fabsf(c->y) > CLIP_GUARD)
                              s_clip_why[2]++;
        else                  s_clip_why[3]++;
    }
    if (a->x == 0.0f && a->y == 0.0f && b->x == 0.0f && b->y == 0.0f
            && c->x == 0.0f && c->y == 0.0f)
        s_clip_origin++;
    in3[0] = a; in3[1] = b; in3[2] = c;
    for (i = 0; i < 3; i++) {
        src[i].v = *in3[i];
        src[i].h[0] = in3[i]->x * in3[i]->w;
        src[i].h[1] = in3[i]->y * in3[i]->w;
        src[i].h[2] = in3[i]->z * in3[i]->w;
        src[i].h[3] = in3[i]->w;
    }

    for (plane = 0; plane < nplanes && n >= 3; plane++) {
        float d[CLIP_MAXV];
        int m = 0, any_out = 0;
        for (i = 0; i < n; i++) {
            d[i] = clip_dist(&src[i], plane, zmax);
            any_out |= !(d[i] >= 0.0f);
        }
        if (!any_out)
            continue;
        for (i = 0; i < n && m < CLIP_MAXV - 1; i++) {
            const ClipVert *p = &src[i], *q = &src[(i + 1) % n];
            float dp = d[i], dq = d[(i + 1) % n];
            int pin = dp >= 0.0f, qin = dq >= 0.0f;
            if (pin)
                dst[m++] = *p;
            if (pin != qin)
                clip_lerp(p, q, dp / (dp - dq), &dst[m++], plane, zmax);
        }
        n = m;
        { ClipVert *t = src; src = dst; dst = t; }
    }
    if (recomp_env(RENV_CLIP_TRACE)) {
        static int shown;
        /* Skip the triangles a program put wholly at the screen origin; they
         * have no area either way and drown out the rest. */
        int origin = a->x == 0.0f && a->y == 0.0f && b->x == 0.0f
                  && b->y == 0.0f && c->x == 0.0f && c->y == 0.0f;
        if (!origin && shown++ < 60) {
            fprintf(stderr, "  [CLIP] in (%.1f %.1f %.4g w %.4g) (%.1f %.1f %.4g w %.4g)"
                            " (%.1f %.1f %.4g w %.4g) -> %d verts:",
                    a->x, a->y, a->z, a->w, b->x, b->y, b->z, b->w,
                    c->x, c->y, c->z, c->w, n);
            for (k = 0; k < n; k++)
                fprintf(stderr, " (%.1f %.1f %.4g w %.4g)", src[k].v.x,
                        src[k].v.y, src[k].v.z, src[k].v.w);
            fprintf(stderr, "\n");
        }
    }
    if (n < 3) {
        s_clip_gone++;
        s_vsh.tris_clipped++;
        return;
    }
    for (k = 1; k + 1 < n; k++) {
        s_clip_out++;
        if (fast) raster_tri_xf_fast(&src[0].v, &src[k].v, &src[k + 1].v, textured);
        else      raster_tri_xf(&src[0].v, &src[k].v, &src[k + 1].v, textured);
    }
}

static void raster_batch_vsh_one(void)
{
    /* One transformed vertex per index, grown to the largest batch so far
     * rather than sized to NV_MAX_INDICES (60 MB at 120 bytes an index). */
    static XfVert *xv;
    static uint32_t xv_cap;
    uint32_t i, n = s_gpu.idx_count, a;
    uint32_t before = s_gpu.tris_drawn, plen;
    int textured, fast = pb_fast();

    if (n > xv_cap) {
        XfVert *grown = (XfVert *)realloc(xv, (size_t)n * sizeof *xv);
        if (!grown) {
            s_xv_fail++;
            return;
        }
        xv = grown;
        xv_cap = n;
    }

    s_vsh.batches_prog++;
    surf_log_note();
    plen = vsh_note_program();
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size)
            s_vsh.fmt_seen[a][s_gpu.attr[a].type & 7]++;

    {
        uint64_t t0 = rt_now();
        for (i = 0; i < n; i++)
            vsh_transform(s_gpu.idx[i], &xv[i]);
        s_rt.xform += rt_now() - t0;
    }
    if (recomp_env(RENV_VSH_ZLOG) && s_vsh.batches_prog < 1200 && n)
        fprintf(stderr, "  [ZLOG] batch %u colour 0x%X depth %d func 0x%X mask %d"
                        " poly %d %g %g | v0 z %g w %g tex 0x%X fmt %02X\n",
                s_vsh.batches_prog, s_gpu.color_offset, s_vsh.depth_enable,
                s_vsh.depth_func, s_vsh.depth_mask, s_vsh.poly_offset,
                s_vsh.poly_factor, s_vsh.poly_bias, xv[0].z, xv[0].w,
                s_gpu.tex.offset, s_gpu.tex.color);
    if (plen > 12 && recomp_env(RENV_VSH_TRACE)) {
        static int shown3d;
        if (shown3d++ < 6) {
            float v[NV2A_VSH_INPUTS][4];
            fprintf(stderr, "  [VSH] 3D batch: prim %u n %u start %u c58 %g %g %g %g"
                            " c59 %g %g %g %g\n", s_gpu.prim, n, s_vsh.start,
                    s_vsh.c[58][0], s_vsh.c[58][1], s_vsh.c[58][2], s_vsh.c[58][3],
                    s_vsh.c[59][0], s_vsh.c[59][1], s_vsh.c[59][2], s_vsh.c[59][3]);
            for (i = 0; i < n && i < 6; i++) {
                vsh_inputs(s_gpu.idx[i], v);
                fprintf(stderr, "  [VSH]   v%u idx %u in v0 %g %g %g %g ->"
                                " (%g, %g, %g, w %g) ok %d\n", i, s_gpu.idx[i],
                        v[0][0], v[0][1], v[0][2], v[0][3],
                        xv[i].x, xv[i].y, xv[i].z, xv[i].w, xv[i].ok);
            }
        }
    }
    if (n)
        memcpy(s_vsh_last_d0, xv[0].d0, sizeof s_vsh_last_d0), s_vsh_last_v0 = xv[0];

    {
        uint32_t ctl0 = s_tex_reg[(NV097_SET_TEXTURE_CONTROL0_M - NV_TEX_FIRST) / 4];
        int stage_on = !s_tex_set[(NV097_SET_TEXTURE_CONTROL0_M - NV_TEX_FIRST) / 4]
                    || (ctl0 & (1u << 30));
        uint64_t t0 = rt_now();
        textured = stage_on && s_gpu.tex.valid;
        vsh_build_stages();
        if (s_stage_mode[0] && (s_stage_tex[0].color == 0x24 || s_stage_tex[0].color == 0x25))
            s_rt.yuv_batches++;
        if (fast)
            fast_batch_setup();
        if (vsh_rc_active()) s_vsh.batches_rc++; else s_vsh.batches_norc++;
        if (textured) {
            s_gpu.batches_textured++;
            note_texture_use();
        } else {
            s_gpu.batches_no_tex++;
        }
        s_rt.setup += rt_now() - t0;
    }

#define RASTER_TRI(p, q, r_, t) clip_tri(p, q, r_, t, fast)
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3)
            RASTER_TRI(&xv[i], &xv[i+1], &xv[i+2], textured);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        /* Odd triangles of a strip are wound the other way; swap two of them
         * back so culling sees one winding for the whole strip. */
        for (i = 0; i + 2 < n; i++) {
            if (i & 1)
                RASTER_TRI(&xv[i+1], &xv[i], &xv[i+2], textured);
            else
                RASTER_TRI(&xv[i], &xv[i+1], &xv[i+2], textured);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++)
            RASTER_TRI(&xv[0], &xv[i], &xv[i+1], textured);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            RASTER_TRI(&xv[i], &xv[i+1], &xv[i+2], textured);
            RASTER_TRI(&xv[i], &xv[i+2], &xv[i+3], textured);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            RASTER_TRI(&xv[i], &xv[i+1], &xv[i+3], textured);
            RASTER_TRI(&xv[i], &xv[i+3], &xv[i+2], textured);
        }
        break;
    default:
        break;
    }
#undef RASTER_TRI
    if (textured)
        s_vsh.tris_textured += s_gpu.tris_drawn - before;

    if (recomp_env(RENV_VSH_TRACE)) {
        static int shown;
        if (shown < 24 && n) {
            shown++;
            fprintf(stderr, "  [VSH] batch prim %u n %u start %u tex %d:"
                            " v0 -> (%.1f, %.1f, %.4g, w %.4g) d0 %.2f %.2f %.2f %.2f"
                            " t0 %.3f %.3f ok %d\n",
                    s_gpu.prim, n, s_vsh.start, textured,
                    xv[0].x, xv[0].y, xv[0].z, xv[0].w,
                    xv[0].d0[0], xv[0].d0[1], xv[0].d0[2], xv[0].d0[3],
                    xv[0].t[0][0], xv[0].t[0][1], xv[0].ok);
        }
    }

    if (s_gpu.tris_drawn != before && s_drawn_dumps < FB_DUMP_AFTER_DRAW) {
        s_drawn_dumps++;
        dump_surface_bmp();
    }
    /* Longer than the pretransformed passthroughs (7 and 10 slots): a real
     * transform. Dump every 400th such batch, first included, up to 12, so a
     * run that reaches 3D leaves frames of it behind. */
    if (plen > 12 && s_gpu.tris_drawn != before) {
        s_vsh.batches_3d++;
        if (s_vsh.batches_3d % 400 == 1 && s_vsh.dumps_3d < 12) {
            s_vsh.dumps_3d++;
            fprintf(stderr, "  [VSH] 3D batch %u (program len %u): dumping\n",
                    s_vsh.batches_3d, plen);
            dump_surface_bmp();
        }
    }
}

/* RECOMP_PB_FAST_AB=1: every program-mode batch twice, from the same starting
 * surface and depth buffer -- the reference loop, then RECOMP_PB_FAST -- and
 * the two compared byte for byte (colour surface), bit for bit (depth) and
 * counter for counter. The fast result is kept. Per batch rather than per
 * frame because runs are not deterministic per flip index.
 *
 * ponytail: the texture-use and program tables count each batch twice. */
static uint64_t s_fab_batches, s_fab_bad, s_fab_bad_px, s_fab_bad_z, s_fab_bad_ctr;

static void fast_ab_summary(const char *why)
{
    fprintf(stderr, "[FAST-AB] %s: %llu batches compared, %llu mismatched"
                    " (%llu px, %llu with depth, %llu with counters);"
                    " cache %llu binds, %llu refingerprints, %llu dropped,"
                    " %llu tiles decoded, %llu read live (texture is the"
                    " target)\n", why,
            (unsigned long long)s_fab_batches, (unsigned long long)s_fab_bad,
            (unsigned long long)s_fab_bad_px, (unsigned long long)s_fab_bad_z,
            (unsigned long long)s_fab_bad_ctr, (unsigned long long)s_tc_binds,
            (unsigned long long)s_tc_refp, (unsigned long long)s_tc_dropped,
            (unsigned long long)s_tc_tiles_filled,
            (unsigned long long)s_tc_feedback);
}

static void raster_batch_fast_ab(void)
{
    static uint8_t *save, *ref;
    static float *zsave, *zref;
    static uint8_t *ssave, *sref;
    static size_t cap, zcap, scap;
    static struct nv2a_pb_gpu gpu0, gpu_ref;
    static struct nv2a_pb_vsh vsh0, vsh_ref;
    uint32_t zpass0, zpass_ref;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    size_t size = (size_t)(s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch, zn = 0, sn = 0, k;
    uint32_t bpp = surface_bpp(), bad_px = 0, first = 0;
    int dumps = s_drawn_dumps, zbad, cbad;

    if (!size || !surface_begin_batch(mem) || (bpp != 4 && bpp != 2)) {
        raster_batch_vsh_one();
        return;
    }
    if (size > cap) {
        save = (uint8_t *)realloc(save, size);
        ref  = (uint8_t *)realloc(ref, size);
        cap = save && ref ? size : 0;
        if (!cap) { raster_batch_vsh_one(); return; }
    }
    if (s_vsh.depth_enable && vsh_zbuf_ready())
        zn = (size_t)s_zbuf_w * s_zbuf_h;
    if (zn > zcap) {
        zsave = (float *)realloc(zsave, zn * sizeof(float));
        zref  = (float *)realloc(zref, zn * sizeof(float));
        zcap = zsave && zref ? zn : 0;
        if (!zcap) { raster_batch_vsh_one(); return; }
    }
    if (vsh_stencil_ready())
        sn = (size_t)s_zbuf_w * s_zbuf_h;
    if (sn > scap) {
        ssave = (uint8_t *)realloc(ssave, sn);
        sref  = (uint8_t *)realloc(sref, sn);
        scap = ssave && sref ? sn : 0;
        if (!scap) { raster_batch_vsh_one(); return; }
    }
    memcpy(save, s_surface, size);
    if (zn) memcpy(zsave, s_zbuf, zn * sizeof(float));
    if (sn) memcpy(ssave, s_sbuf, sn);
    gpu0 = s_gpu; vsh0 = s_vsh; zpass0 = s_zpass_count;

    /* reference */
    s_fast_force = 0;
    s_drawn_dumps = FB_DUMP_AFTER_DRAW;
    s_vsh.dumps_3d = 12;
    raster_batch_vsh_one();
    s_drawn_dumps = dumps;
    surface_begin_batch(mem);
    memcpy(ref, s_surface, size);
    if (zn) memcpy(zref, s_zbuf, zn * sizeof(float));
    if (sn) memcpy(sref, s_sbuf, sn);
    gpu_ref = s_gpu; vsh_ref = s_vsh; zpass_ref = s_zpass_count;

    /* fast, from the same start */
    memcpy(s_surface, save, size);
    if (zn) memcpy(s_zbuf, zsave, zn * sizeof(float));
    if (sn) memcpy(s_sbuf, ssave, sn);
    s_gpu = gpu0; s_vsh = vsh0; s_zpass_count = zpass0;
    s_fast_force = 1;
    raster_batch_vsh_one();
    s_fast_force = -1;
    surface_begin_batch(mem);

    s_fab_batches++;
    if (memcmp(ref, s_surface, size))
        for (k = 0; k + bpp <= size; k += bpp)
            if (memcmp(ref + k, s_surface + k, bpp)) {
                if (!bad_px++) first = (uint32_t)k;
            }
    zbad = (zn && memcmp(zref, s_zbuf, zn * sizeof(float)))
        || (sn && memcmp(sref, s_sbuf, sn));   /* "depth" covers stencil */
    cbad = gpu_ref.pixels != s_gpu.pixels || gpu_ref.pixel_max != s_gpu.pixel_max
        || gpu_ref.tris_drawn != s_gpu.tris_drawn
        || gpu_ref.tris_skipped_offscreen != s_gpu.tris_skipped_offscreen
        || vsh_ref.tris != s_vsh.tris || vsh_ref.tris_culled != s_vsh.tris_culled
        || vsh_ref.tris_clipped != s_vsh.tris_clipped
        || vsh_ref.z_rejected != s_vsh.z_rejected
        || vsh_ref.a_rejected != s_vsh.a_rejected
        || zpass_ref != s_zpass_count;
    if (bad_px || zbad || cbad) {
        s_fab_bad++;
        s_fab_bad_px += bad_px;
        s_fab_bad_z += zbad != 0;
        s_fab_bad_ctr += cbad != 0;
        if (s_fab_bad <= 8) {
            uint32_t rv = 0, fv = 0;
            if (bad_px) { memcpy(&rv, ref + first, bpp); memcpy(&fv, s_surface + first, bpp); }
            fprintf(stderr, "[FAST-AB] MISMATCH batch %llu flip %u prim %u n %u:"
                            " %u px differ, first (%u, %u) ref 0x%08X fast 0x%08X;"
                            " depth %s; counters %s (px %llu/%llu zrej %llu/%llu"
                            " arej %llu/%llu tris %u/%u) | tex %08X fmt %02X %ux%u"
                            " rc %d ctl 0x%X mode %d,%d,%d,%d\n",
                    (unsigned long long)s_fab_batches, s_gpu.flips, s_gpu.prim,
                    s_gpu.idx_count, bad_px,
                    (uint32_t)((first % s_gpu.pitch) / bpp), (uint32_t)(first / s_gpu.pitch),
                    rv, fv, zbad ? "DIFFERS" : "same", cbad ? "DIFFER" : "same",
                    (unsigned long long)gpu_ref.pixels, (unsigned long long)s_gpu.pixels,
                    (unsigned long long)vsh_ref.z_rejected, (unsigned long long)s_vsh.z_rejected,
                    (unsigned long long)vsh_ref.a_rejected, (unsigned long long)s_vsh.a_rejected,
                    vsh_ref.tris, s_vsh.tris,
                    s_stage_tex[0].offset, s_stage_tex[0].color,
                    s_stage_tex[0].width, s_stage_tex[0].height,
                    vsh_rc_active(), s_vsh.rc_ctl,
                    s_stage_mode[0], s_stage_mode[1], s_stage_mode[2], s_stage_mode[3]);
            {   /* Is the cached copy what memory holds now? */
                TexCacheEntry *e = s_stage_tc[0];
                uint32_t u, v, stale = 0, seen = 0, tv;
                if (e)
                    for (v = 0; v < e->height; v++)
                        for (u = 0; u < e->width; u++) {
                            uint32_t tile = (v >> TC_TILE_LOG) * e->ltx[0] + (u >> TC_TILE_LOG);
                            if (!e->tile_ok[tile]) continue;
                            seen++;
                            if (sample_tex(&s_stage_tex[0], u, v, &tv)
                                    && tv != e->texels[(size_t)v * e->width + u])
                                stale++;
                        }
                fprintf(stderr, "[FAST-AB]   target 0x%08X base 0x%08X; stage 0 cache %s,"
                                " %u of %u decoded texels stale; wseq %u written %u gen %u flips %u\n",
                        s_gpu.color_offset, s_gpu.color_base, e ? "bound" : "none",
                        stale, seen, e ? e->wseq : 0,
                        e ? surf_written_since(e->offset, e->offset + tc_extent(&s_stage_tex[0], e->levels)) : 0,
                        e ? e->gen : 0, s_gpu.flips);
            }
        }
    }
    if (s_fab_batches % 2000 == 0)
        fast_ab_summary("running");
}

static void raster_batch_vsh(void)
{
    static int ab = -1;
    if (ab < 0) {
        const char *e = recomp_env(RENV_PB_FAST_AB);
        ab = e && e[0] && e[0] != '0';
    }
    if (ab)
        raster_batch_fast_ab();
    else
        raster_batch_vsh_one();
}


static void vsh_report(void)
{
    int i;
    uint32_t a, t;

    fprintf(stderr, "[VSH] mode 0x%X (set %u) | program: %u dwords, %u loads,"
                    " %u starts, start %u | constants: %u dwords, %u loads |"
                    " viewport %u dwords, ff-matrix %u dwords\n",
            s_vsh.mode, s_vsh.mode_sets, s_vsh.prog_dwords, s_vsh.prog_loads,
            s_vsh.prog_starts, s_vsh.start, s_vsh.const_dwords,
            s_vsh.const_loads, s_vsh.vp_dwords, s_vsh.ff_matrix_dwords);
    fprintf(stderr, "[VSH] batches: %u program, %u fixed-function; %u vertices"
                    " run (%u unusable); %u tris drawn (%u textured), %u culled,"
                    " %u dropped (clipped away or unusable), %llu pixels depth-rejected%s\n",
            s_vsh.batches_prog, s_vsh.batches_fixed, s_vsh.verts_run,
            s_vsh.verts_bad, s_vsh.tris, s_vsh.tris_textured, s_vsh.tris_culled,
            s_vsh.tris_clipped, (unsigned long long)s_vsh.z_rejected,
            vsh_enabled() ? "" : "  [pb_vsh=0: path off]");
    tex_log_report();
    fprintf(stderr, "[VSH] clipping %s: %llu tris clipped (%llu partly behind"
                    " the eye, %llu wholly behind, %llu guard band, %llu z) ->"
                    " %llu drawn pieces, %llu clipped away; %llu with an unusable"
                    " vertex; %llu had all three vertices at screen (0, 0)\n",
            clip_enabled() ? "on" : "OFF (pb_clip=0)",
            (unsigned long long)s_clip_tris, (unsigned long long)s_clip_why[0],
            (unsigned long long)s_clip_why[1], (unsigned long long)s_clip_why[2],
            (unsigned long long)s_clip_why[3], (unsigned long long)s_clip_out,
            (unsigned long long)s_clip_gone, (unsigned long long)s_clip_unusable,
            (unsigned long long)s_clip_origin);
    fprintf(stderr, "[VSH] viewport offset %.2f %.2f %.2f %.2f scale %.2f %.2f"
                    " %.2f %.2f | c58 %.2f %.2f %.2f c59 %.2f %.2f %.2f |"
                    " cull %d face 0x%X front 0x%X | depth %d func 0x%X mask %d"
                    " | surface fmt 0x%X zeta 0x%X, %u clears (%u with Z),"
                    " %llu depth buffer evictions\n",
            s_vsh.vp_off[0], s_vsh.vp_off[1], s_vsh.vp_off[2], s_vsh.vp_off[3],
            s_vsh.vp_scale[0], s_vsh.vp_scale[1], s_vsh.vp_scale[2], s_vsh.vp_scale[3],
            s_vsh.c[58][0], s_vsh.c[58][1], s_vsh.c[58][2],
            s_vsh.c[59][0], s_vsh.c[59][1], s_vsh.c[59][2],
            s_vsh.cull_enable, s_vsh.cull_face, s_vsh.front_face,
            s_vsh.depth_enable, s_vsh.depth_func, s_vsh.depth_mask,
            s_gpu.format, s_vsh.zeta_offset, s_vsh.clears_seen, s_vsh.zclears,
            (unsigned long long)s_zbc.evictions);
    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        int any = 0;
        for (t = 0; t < 8; t++) any |= s_vsh.fmt_seen[a][t] != 0;
        if (!any) continue;
        fprintf(stderr, "[VSH]   v%-2u batches by type:", a);
        for (t = 0; t < 8; t++)
            if (s_vsh.fmt_seen[a][t])
                fprintf(stderr, " %s=%u",
                        t == 0 ? "ub_d3d" : t == 1 ? "s1" : t == 2 ? "f"
                        : t == 4 ? "ub_ogl" : t == 5 ? "s32k" : t == 6 ? "cmp" : "?",
                        s_vsh.fmt_seen[a][t]);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "[VSH] combiners: %u batches evaluated, %u fixed MODULATE%s;"
                    " ctl 0x%X fcw 0x%08X/0x%08X; alpha test %d func 0x%X ref %u"
                    " (%llu px rejected); fog %d\n",
            s_vsh.batches_rc, s_vsh.batches_norc,
            recomp_env(RENV_PB_RC) && recomp_env(RENV_PB_RC)[0] == '0'
                ? " [pb_rc=0]" : "",
            s_vsh.rc_ctl, s_vsh.rc_fcw0, s_vsh.rc_fcw1, s_vsh.atest_enable,
            s_vsh.atest_func, s_vsh.atest_ref,
            (unsigned long long)s_vsh.a_rejected, s_vsh.fog_enable);
    for (a = 0; a < 4; a++) {
        fprintf(stderr, "[VSH]   stage %u shader modes (batches):", a);
        for (t = 0; t < 32; t++)
            if (s_vsh.stage_mode_seen[a][t])
                fprintf(stderr, " %u=%u", t, s_vsh.stage_mode_seen[a][t]);
        fprintf(stderr, "\n");
    }
    if (s_fab_batches)
        fast_ab_summary("total");
    if (s_vsh_ab_batches)
        fprintf(stderr, "[VSH] A/B vs screen-space path: %llu batches, %llu differ,"
                        " %llu px differ\n",
                (unsigned long long)s_vsh_ab_batches,
                (unsigned long long)s_vsh_ab_diff_batches,
                (unsigned long long)s_vsh_ab_diff_px);
    for (i = 0; i < s_vsh_prog_count; i++)
        fprintf(stderr, "[VSH]   program %08X start %u len %u: %u batches\n",
                s_vsh_progs[i].hash, s_vsh_progs[i].start, s_vsh_progs[i].len,
                s_vsh_progs[i].batches);
}

/* RECOMP_PB_VSH_AB=1: draw every program-mode batch twice, the old
 * screen-space way and through the vertex program, keep the program result,
 * and count the pixels where the two disagree. This is the before/after check
 * on a movie-backed scene, made per batch inside one run so movie timing
 * cannot move it. */
static int s_vsh_ab_inner;

static void raster_batch(void);

static void ab_write_bmp(const char *path, const uint8_t *surf, uint32_t bpp)
{
    uint32_t w = s_gpu.clip_x + s_gpu.clip_w, h = s_gpu.clip_y + s_gpu.clip_h, x, y;
    uint32_t row = w * 3, pad = (4 - (row & 3)) & 3, fsz = 54 + (row + pad) * h;
    uint8_t hdr[54] = {0}, px[3], z[3] = {0};
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    hdr[0] = 'B'; hdr[1] = 'M'; memcpy(hdr + 2, &fsz, 4); hdr[10] = 54;
    hdr[14] = 40; memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    for (y = h; y-- > 0; ) {
        const uint8_t *r = surf + (size_t)y * s_gpu.pitch;
        for (x = 0; x < w; x++) {
            if (bpp == 4) {
                px[0] = r[x*4]; px[1] = r[x*4+1]; px[2] = r[x*4+2];
            } else {
                uint16_t t = ((const uint16_t *)r)[x];
                px[0] = (uint8_t)((t & 0x1F) << 3);
                px[1] = (uint8_t)(((t >> 5) & 0x3F) << 2);
                px[2] = (uint8_t)((t >> 11) << 3);
            }
            fwrite(px, 1, 3, f);
        }
        fwrite(z, 1, pad, f);
    }
    fclose(f);
}

static void raster_batch_ab(void)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    static uint8_t *save, *old;
    static size_t cap;
    size_t size = (size_t)(s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch, k;
    uint32_t bpp = surface_bpp(), diff = 0, maxd = 0;
    int dumps = s_drawn_dumps;
    uint64_t zrej0;

    if (!size || !surface_begin_batch(mem) || (bpp != 4 && bpp != 2)) {
        raster_batch_vsh();
        return;
    }
    if (size > cap) {
        save = (uint8_t *)realloc(save, size);
        old  = (uint8_t *)realloc(old, size);
        cap = save && old ? size : 0;
        if (!cap) { raster_batch_vsh(); return; }
    }
    memcpy(save, s_surface, size);
    s_vsh_ab_inner = 1;
    s_drawn_dumps = FB_DUMP_AFTER_DRAW;
    raster_batch();                        /* old path */
    s_drawn_dumps = dumps;
    s_vsh_ab_inner = 0;
    surface_begin_batch(mem);
    memcpy(old, s_surface, size);
    memcpy(s_surface, save, size);
    zrej0 = s_vsh.z_rejected;
    raster_batch_vsh();
    surface_begin_batch(mem);
    for (k = 0; k + bpp <= size; k += bpp) {
        uint32_t d = 0, c;
        for (c = 0; c < (bpp == 4 ? 3u : 2u); c++) {
            int e = (int)s_surface[k + c] - (int)old[k + c];
            if (e < 0) e = -e;
            if ((uint32_t)e > d) d = (uint32_t)e;
        }
        if (d > 2) diff++;
        if (d > maxd) maxd = d;
    }
    s_vsh_ab_batches++;
    if (diff) {
        /* RECOMP_PB_VSH_AB_DUMP=<prefix>: both versions of the first
         * differing batch per texture, as <prefix>NNN_old/new.bmp. */
        const char *pre = recomp_env(RENV_PB_VSH_AB_DUMP);
        static uint32_t seen[16];
        static int nseen;
        int j, have = 0;
        for (j = 0; j < nseen; j++) have |= seen[j] == s_gpu.tex.offset;
        static int zdumps;
        int zd = pre && s_vsh.z_rejected != zrej0 && zdumps < 4;
        zdumps += zd;
        if ((pre && !have && nseen < 16) || zd) {
            char path[512];
            if (!have && nseen < 16)
                seen[nseen++] = s_gpu.tex.offset;
            snprintf(path, sizeof path, "%s%03llu_old.bmp", pre,
                     (unsigned long long)s_vsh_ab_batches);
            ab_write_bmp(path, old, bpp);
            snprintf(path, sizeof path, "%s%03llu_new.bmp", pre,
                     (unsigned long long)s_vsh_ab_batches);
            ab_write_bmp(path, s_surface, bpp);
        }
        s_vsh_ab_diff_batches++;
        s_vsh_ab_diff_px += diff;
        static int zshown;
        int zlog = s_vsh.z_rejected != zrej0 && zshown < 24;
        zshown += zlog;
        if (s_vsh_ab_diff_batches <= 16 || s_vsh_ab_diff_batches % 64 == 0 || zlog
                || (pre && !have && nseen <= 16 && seen[nseen - 1] == s_gpu.tex.offset))
            fprintf(stderr, "  [VSH-AB] batch %llu prim %u n %u: %u px differ"
                            " (max %u); d0 %.2f %.2f %.2f %.2f tex %08X fmt %02X"
                            " %ux%u blend %u %X/%X | z %.4g w %.4g zrej %llu"
                            " t0 %.3f %.3f | colour 0x%X zeta 0x%X; last Z clear"
                            " colour 0x%X zeta 0x%X %u batches ago\n",
                    (unsigned long long)s_vsh_ab_batches, s_gpu.prim,
                    s_gpu.idx_count, diff, maxd,
                    s_vsh_last_d0[0], s_vsh_last_d0[1], s_vsh_last_d0[2],
                    s_vsh_last_d0[3], s_gpu.tex.offset, s_gpu.tex.color,
                    s_gpu.tex.width, s_gpu.tex.height, s_gpu.blend_enable,
                    s_gpu.blend_sfactor, s_gpu.blend_dfactor,
                    s_vsh_last_v0.z, s_vsh_last_v0.w,
                    (unsigned long long)(s_vsh.z_rejected - zrej0),
                    s_vsh_last_v0.t[0][0], s_vsh_last_v0.t[0][1],
                    s_gpu.color_offset, s_vsh.zeta_offset, s_vsh.zclear_color_off,
                    s_vsh.zclear_zeta_off, s_vsh.batches_prog - s_vsh.zclear_batch);
    }
}

static void raster_batch(void)
{
    uint32_t i;
    uint32_t before = s_gpu.tris_drawn;

    if (s_gpu.idx_count < 3)
        return;
    if (!s_vsh_ab_inner && vsh_batch_active()) {
        static int ab = -1;
        if (ab < 0)
            ab = recomp_env(RENV_PB_VSH_AB) != NULL;
        if (ab)
            raster_batch_ab();
        else
            raster_batch_vsh();
        return;
    }
    if ((s_vsh.mode & 3u) != 2u)
        s_vsh.batches_fixed++;
    if (!batch_is_screen_space()) {
        s_gpu.batches_untransformed++;
        return;
    }

    /* Count why, once per batch: the texture stage cannot change inside one. */
    {
        const VertexAttr *tc = texcoord_attr();

        if (!(tc->offset && tc->stride))
            s_gpu.batches_no_uv++;
        else if (!s_gpu.tex.valid)
            s_gpu.batches_no_tex++;
        else {
            s_gpu.batches_textured++;
            note_texture_use();
        }
    }

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count; i += 3)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[0], s_gpu.idx[i], s_gpu.idx[i+1],
                           vertex_color(s_gpu.idx[0]));
        break;
    case NV_PRIM_QUADS:
        /* Independent quads, four vertices each. A batch of eight is two
         * quads, not one six-triangle fan around the first vertex; with
         * exactly four the two agreed, which is why sharing the fan arm
         * looked right. */
        for (i = 0; i + 3 < s_gpu.idx_count; i += 4) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+2], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        /* Each vertex pair past the first closes another quad against the
         * pair before it. */
        for (i = 0; i + 3 < s_gpu.idx_count; i += 2) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+3], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    default:
        break;                             /* points and lines: not yet */
    }

    if (s_gpu.tris_drawn && (s_gpu.tris_drawn % 500) == 0)
        fprintf(stderr, "  [GPU] %u triangles rasterised\n", s_gpu.tris_drawn);

    /* Capture the surface while the geometry is still on it.
     *
     * The periodic report dumps too, but a title clears every frame and draws
     * in only some of them, so a report almost always lands on a surface that
     * was wiped a moment ago -- which reads as "nothing was drawn" when the
     * triangles went down correctly just before it. A few frames that actually
     * contain geometry are worth more than any number of clears. */
    if (s_gpu.tris_drawn != before && s_drawn_dumps < FB_DUMP_AFTER_DRAW) {
        s_drawn_dumps++;
        dump_surface_bmp();
    }
}

/* ── Backend hooks (nv2a_pb_state.h) ─────────────────────────────────────
 *
 * The walker decodes; a backend draws. The CPU rasteriser below is the
 * default and reads s_gpu directly, so its on_draw ignores the descriptor. */
static void cpu_on_draw(const struct nv2a_pb_draw *draw)
{
    uint64_t t0 = rt_now();
    (void)draw;
    raster_batch();
    s_rt.total += rt_now() - t0;
}

/* Hand the window a copy of the frame just finished. Copying here, rather
 * than letting the window read guest memory on its own clock, is what stops
 * it showing a surface the rasteriser is still writing. */
static void cpu_on_flip(uint32_t surface, uint32_t pitch)
{
    extern void xbox_FramebufferWindowPresent(uint32_t, uint32_t);

    if (!pitch || !surface)
        return;
    xbox_FramebufferWindowSet(dma_resolve(surface), pitch);
    xbox_FramebufferWindowPresent(dma_resolve(surface), pitch);
}

static const struct nv2a_pb_backend s_cpu_backend = {
    clear_surface,                       /* on_clear  */
    vsh_clear_depth,                     /* on_zclear */
    cpu_on_draw,                         /* on_draw   */
    cpu_on_flip,                         /* on_flip   */
};
static const struct nv2a_pb_backend *s_backend = &s_cpu_backend;
/* A GET_REPORT the backend took and has not been polled for since. */
static int s_report_owed;

/* The title can read a deferred report as soon as the walker hands back
 * anything it waits on: a semaphore release, GET at the end of the walk.
 * On the console the count is written ahead of both, so the backend's
 * on_poll settles it here (nv2a_backend_common.h). */
static void reports_settle(void)
{
    if (s_report_owed) {
        s_report_owed = 0;
        if (s_backend->on_poll)
            s_backend->on_poll();
    }
}

/* nv2a_pb_scan, at the end of each walk, before GET is stored. */
void nv2a_pb_exec_walk_end(void)
{
    reports_settle();
}

static void get_report(uint32_t param)
{
    static int fixed_set = -1;
    static uint32_t fixed = 0x10000;
    uint32_t va, value, phys;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();

    if (fixed_set < 0) {
        const char *e = recomp_env(RENV_ZPASS_FIXED);
        fixed_set = e != NULL;
        if (e)
            fixed = (uint32_t)strtoul(e, NULL, 0);
    }
    if ((param >> 24) != 1) {
        note_unhandled(NV097_GET_REPORT, param);
        return;
    }
    if (!ctxdma_place(&s_report_bind, "report", param & 0x00FFFFFFu, 16, &phys))
        return;
    va = dma_resolve(phys);
    if (surface_hits_image(va, 16)) {
        note_unhandled(NV097_GET_REPORT, param);
        return;
    }
    if (!fixed_set && s_backend->on_report && s_backend->on_report(va)) {
        s_reports++;                       /* the backend completes it */
        s_report_owed = 1;
        return;
    }
    value = (fixed_set || s_backend != &s_cpu_backend) ? fixed : s_zpass_count;
    s_reports++;
    s_reports_zero += value == 0;
    if (recomp_env(RENV_ZPASS_TRACE) && s_reports <= 64)
        fprintf(stderr, "  [GPU] GET_REPORT 0x%08X -> 0x%08X count %u\n",
                param, va, value);
    nv2a_report_write(mem, va, value);
}

/* RECOMP_PB_BACKEND=null: the walker and its decode, drawing nothing. Times
 * the platform-independent part of a frame on its own (the [PB-PERF] line). */
/* Batches handed to a GPU backend that writes its frame back to guest memory
 * (writes_back): the CPU rasteriser's tris_drawn stays put under one, and
 * RECOMP_FB_DUMP_FLIPS reads "drew something" from both. */
static uint32_t s_gpu_backend_batches;
static int s_null_draw_us;  /* RECOMP_PB_NULL_DRAW_US: a backend's cost, simulated */
static void null_on_draw(const struct nv2a_pb_draw *d)
{
    uint64_t end = rt_now() + (uint64_t)s_null_draw_us * 1000u;
    (void)d;
    while (rt_now() < end) { }
}
static struct nv2a_pb_backend s_null_backend = { NULL, NULL, NULL, NULL };

void nv2a_pb_set_backend(const struct nv2a_pb_backend *backend)
{
    s_backend = backend ? backend : &s_cpu_backend;
}

const struct nv2a_pb_backend *nv2a_pb_get_backend(void) { return s_backend; }
const struct nv2a_pb_backend *nv2a_pb_cpu_backend(void) { return &s_cpu_backend; }

const struct nv2a_pb_gpu *nv2a_pb_gpu_state(void) { return &s_gpu; }
const struct nv2a_pb_vsh *nv2a_pb_vsh_state(void) { return &s_vsh; }
const uint32_t *nv2a_pb_tex_regs(void)            { return s_tex_reg; }
const uint8_t  *nv2a_pb_tex_regs_set(void)        { return s_tex_set; }
uint32_t nv2a_pb_dma_resolve(uint32_t offset)     { return dma_resolve(offset); }
const struct nv2a_pb_present *nv2a_pb_present_state(void) { return &s_present; }

/* RECOMP_TRACE=px=x,y[;x,y],px_flips=a-b,px_max=n on the CPU path: each batch
 * that changes one of the pixels prints its colour and depth before and
 * after, with the state the backends read (blend, depth, stencil, alpha test,
 * fog, texture-shader and clip-plane modes, the combiner registers and every
 * enabled stage's registers). The D3D11 backend has its own probe on the same
 * keys; this one is the reference to hold it, and Metal, against. Flips are
 * the walker's count, the "[GPU] flip N" number. */
static int s_cpx_n = -1, s_cpx_x[4], s_cpx_y[4], s_cpx_max;
static uint32_t s_cpx_f0, s_cpx_f1;

static int cpu_px_on(void)
{
    if (s_cpx_n < 0) {
        const char *e = recomp_env(RENV_D3D11_PX), *f = recomp_env(RENV_D3D11_PX_FLIPS);
        const char *m = recomp_env(RENV_D3D11_PX_MAX);
        s_cpx_n = 0;
        while (e && *e && s_cpx_n < 4) {
            int x, y;
            if (sscanf(e, "%d,%d", &x, &y) != 2)
                break;
            s_cpx_x[s_cpx_n] = x;
            s_cpx_y[s_cpx_n] = y;
            s_cpx_n++;
            e = strchr(e, ';');
            if (e) e++;
        }
        if (f) {
            unsigned a = 0, b = 0;
            int k = sscanf(f, "%u-%u", &a, &b);
            s_cpx_f0 = a;
            s_cpx_f1 = k == 2 ? b : a;
        }
        s_cpx_max = m ? atoi(m) : 400;
    }
    return s_cpx_n > 0 && s_cpx_max > 0 && s_backend == &s_cpu_backend
        && s_gpu.flips >= s_cpx_f0 && s_gpu.flips <= s_cpx_f1;
}

static uint32_t cpu_px_read(int n)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t x = (uint32_t)s_cpx_x[n], y = (uint32_t)s_cpx_y[n];
    const uint8_t *p;
    size_t at;

    if (!s_gpu.color_offset || (bpp != 2 && bpp != 4)
            || x >= s_gpu.clip_x + s_gpu.clip_w || y >= s_gpu.clip_y + s_gpu.clip_h)
        return 0xDEADBEEFu;
    if (surface_swizzled()) {       /* a swizzled target has no rows */
        long i = surface_swizzle_index((int)x, (int)y);
        if (i < 0)
            return 0xDEADBEEFu;
        at = (size_t)i * bpp;
    } else {
        if (!s_gpu.pitch)
            return 0xDEADBEEFu;
        at = (size_t)y * s_gpu.pitch + (size_t)x * bpp;
    }
    p = mem + dma_resolve(s_gpu.color_offset) + at;
    return bpp == 4 ? *(const uint32_t *)p : *(const uint16_t *)p;
}

static float cpu_px_z(int n)
{
    uint32_t x = (uint32_t)s_cpx_x[n], y = (uint32_t)s_cpx_y[n];
    if (!s_zbuf || x >= s_zbuf_w || y >= s_zbuf_h)
        return -1.0f;
    return s_zbuf[(size_t)y * s_zbuf_w + x];
}

static void cpu_px_report(const uint32_t before[4], const float zb[4])
{
    int n, st;
    uint32_t k;

    for (n = 0; n < s_cpx_n; n++) {
        uint32_t after = cpu_px_read(n);
        if (after == before[n] || s_cpx_max <= 0)
            continue;
        s_cpx_max--;
        fprintf(stderr, "[CPU-PX] flip %u batch %u px (%d,%d) %08X -> %08X z %.6f -> %.6f"
                        " | surf 0x%08X fmt 0x%X | prim %u n %u prog %d start %u"
                        " | blend %d %X/%X eq %X keep %08X | z %d func %X mask %d"
                        " zeta 0x%X | sten %d | atest %d %X %u | cull %d %X | fog %d %X"
                        " | shader 0x%X clip 0x%X ctl 0x%X fcw %08X/%08X | ctl0 %X\n",
                s_gpu.flips, s_gpu.draws, s_cpx_x[n], s_cpx_y[n], before[n], after,
                zb[n], cpu_px_z(n), s_gpu.color_offset, s_gpu.format, s_gpu.prim,
                s_gpu.idx_count, vsh_batch_active(), s_vsh.start, s_gpu.blend_enable,
                s_gpu.blend_sfactor, s_gpu.blend_dfactor, s_gpu.blend_equation,
                s_gpu.color_keep, s_vsh.depth_enable, s_vsh.depth_func, s_vsh.depth_mask,
                s_vsh.zeta_offset, s_vsh.stencil_enable, s_vsh.atest_enable,
                s_vsh.atest_func, s_vsh.atest_ref, s_vsh.cull_enable, s_vsh.cull_face,
                s_vsh.fog_enable, s_vsh.fog_mode, s_vsh.shader_prog, s_vsh.clip_plane_mode,
                s_vsh.rc_ctl, s_vsh.rc_fcw0, s_vsh.rc_fcw1, s_vsh.control0);
        for (k = 0; k < (s_vsh.rc_ctl & 0xF) && k < 8; k++)
            fprintf(stderr, "[CPU-PX]   rc%u ci %08X ai %08X co %08X ao %08X f0 %08X f1 %08X\n",
                    k, s_vsh.rc_cicw[k], s_vsh.rc_aicw[k], s_vsh.rc_cocw[k],
                    s_vsh.rc_aocw[k], s_vsh.rc_f0[k], s_vsh.rc_f1[k]);
        for (st = 0; st < 4; st++) {
            const uint32_t *r = &s_tex_reg[(0x40u * st) / 4];
            if (!(r[3] & 0x40000000u))      /* CONTROL0 enable */
                continue;
            fprintf(stderr, "[CPU-PX]   stage %d off %08X fmt %08X addr %08X ctl0 %08X"
                            " ctl1 %08X filt %08X rect %08X\n",
                    st, r[0], r[1], r[2], r[3], r[4], r[5], r[7]);
        }
    }
}

/* RECOMP_DEBUG=vsh_const_dump: the constants this program batch runs with,
 * here before the backend draws it, so every backend dumps the same records.
 * The cap keeps an unbounded window from filling the disk: at ~3 KB a record
 * and a few hundred skinned batches a frame, every flip is ~1 MB. */
static struct {
    FILE *f;
    int state;                          /* 0 unread, 1 on, -1 off */
    struct nv2a_vsh_cdump_cfg cfg;
    uint64_t bytes;
} s_vcd;

static void vsh_const_dump_close(const char *why)
{
    fprintf(stderr, "[VSH-CONST] done at flip %u: %llu bytes%s\n", s_gpu.flips,
            (unsigned long long)s_vcd.bytes, why);
    fclose(s_vcd.f);
    s_vcd.f = NULL;
    s_vcd.state = -1;
}

/* At FLIP_INCREMENT_WRITE: the flip's records are complete. Flushed here
 * because runs are usually ended by a kill, not an exit, and closed once the
 * window is past so its last flip is on disk too. */
static void vsh_const_dump_flip(void)
{
    if (s_vcd.state <= 0)
        return;
    if (s_gpu.flips > s_vcd.cfg.last)
        vsh_const_dump_close("");
    else if (s_gpu.flips > s_vcd.cfg.first)
        fflush(s_vcd.f);
}

static void vsh_const_dump_draw(void)
{
    const uint64_t cap = 256ull << 20;
    uint32_t len, h;

    if (s_vcd.state == 0) {
        const char *path = recomp_env(RENV_VSH_CONST_DUMP);
        s_vcd.state = -1;
        if (!path)
            return;
        if (!vsh_enabled())
            /* No program batches without the interpreter's tracking. */
            fprintf(stderr, "[VSH-CONST] needs pb_vsh (pb_vsh=0 is set); no dump\n");
        else if (nv2a_vsh_cdump_parse(recomp_env(RENV_VSH_CONST_FLIPS),
                                      recomp_env(RENV_VSH_CONST_PROG), &s_vcd.cfg))
            fprintf(stderr, "[VSH-CONST] vsh_const_flips or vsh_const_prog"
                            " malformed; no dump\n");
        else if (!(s_vcd.f = fopen(path, "wb")))
            fprintf(stderr, "[VSH-CONST] cannot open %s; no dump\n", path);
        else {
            s_vcd.state = 1;
            fprintf(stderr, "[VSH-CONST] dumping to %s, flips %u-%u every %u,"
                            " %d program filter(s)\n", path, s_vcd.cfg.first,
                    s_vcd.cfg.last, s_vcd.cfg.every, s_vcd.cfg.nprog);
        }
    }
    if (s_vcd.state < 0 || !vsh_batch_active())
        return;
    if (s_gpu.flips > s_vcd.cfg.last) {
        vsh_const_dump_close("");
        return;
    }
    if (s_vcd.bytes + NV2A_VSH_CDUMP_RECORD > cap) {
        vsh_const_dump_close(" (cap)");
        return;
    }
    if (s_gpu.flips < s_vcd.cfg.first)
        return;
    h = nv2a_vsh_program_hash(&s_vsh, &len);
    if (!nv2a_vsh_cdump_wanted(&s_vcd.cfg, s_gpu.flips, h))
        return;
    if (nv2a_vsh_cdump_write(s_vcd.f, s_gpu.flips, s_gpu.draws, h, s_vsh.start,
                             len, (const float (*)[4])s_vsh.c)) {
        fprintf(stderr, "[VSH-CONST] write failed at flip %u; no more dumps\n",
                s_gpu.flips);
        fclose(s_vcd.f);
        s_vcd.f = NULL;
        s_vcd.state = -1;
        return;
    }
    s_vcd.bytes += NV2A_VSH_CDUMP_RECORD;
}

/* A frame count for fb_dump_prog_before/_after: digits only, so a typo turns
 * the trigger off instead of silently meaning 0. */
static int fb_prog_count(const char *key, const char *e, uint32_t *out)
{
    char *end;
    unsigned long v;

    if (!e)
        return 0;
    v = strtoul(e, &end, 10);
    if (end == e || *end || *e == '-' || v > 0xFFFFFFFFul) {
        fprintf(stderr, "  [GPU] %s=%s is not a frame count; fb_dump_prog off\n", key, e);
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

/* Each dumped flip is a full BMP (about 1.2 MB at 640x480): a typo such as
 * fb_dump_prog_after=60000 would otherwise write tens of GB. */
#define FB_PROG_AFTER_MAX 240

static void fb_prog_init(void)
{
    const char *progs = recomp_env(RENV_FB_DUMP_PROG);
    const struct nv2a_pb_backend *be = nv2a_pb_get_backend();
    uint32_t before = 30;

    s_fbp.state = -1;
    if (!progs)
        return;
    s_fbp.after = 60;
    if (fb_prog_count("fb_dump_prog_before", recomp_env(RENV_FB_DUMP_PROG_BEFORE), &before)
            || fb_prog_count("fb_dump_prog_after", recomp_env(RENV_FB_DUMP_PROG_AFTER),
                             &s_fbp.after))
        return;
    if (s_fbp.after > FB_PROG_AFTER_MAX) {
        fprintf(stderr, "  [GPU] fb_dump_prog_after=%u: capped at %d\n", s_fbp.after,
                FB_PROG_AFTER_MAX);
        s_fbp.after = FB_PROG_AFTER_MAX;
    }
    s_fbp.here = be == nv2a_pb_cpu_backend() || be->writes_back;
    if (!vsh_enabled()) {
        fprintf(stderr, "  [GPU] fb_dump_prog needs pb_vsh (pb_vsh=0 is set); off\n");
        return;
    }
    if (nv2a_vsh_cdump_parse(NULL, progs, &s_fbp.cfg) || !s_fbp.cfg.nprog) {
        fprintf(stderr, "  [GPU] fb_dump_prog malformed; off\n");
        return;
    }
    /* D3D11 presents from its own surfaces: it dumps the flips after the
     * trigger (nv2a_pb_dump_at_listed) but keeps no ring. */
    if (nv2a_fb_ring_init(&s_fbp.ring, s_fbp.here ? before : 0)) {
        fprintf(stderr, "  [GPU] fb_dump_prog_before=%u: 0..%d frames; off\n",
                before, NV2A_FB_RING_MAX);
        return;
    }
    s_fbp.state = 1;
    fprintf(stderr, "  [GPU] fb_dump_prog: armed for %d program(s), %u flips"
                    " before%s, %u after\n", s_fbp.cfg.nprog, s_fbp.ring.size,
            s_fbp.here || !before ? "" : " (no ring on this backend)", s_fbp.after);
    /* Said at arming, not at the trigger, which may come minutes later. */
    if (s_fbp.here && !recomp_env(RENV_FB_DUMP))
        fprintf(stderr, "  [GPU] WARNING: fb_dump_prog is set without fb_dump;"
                        " no flips will be dumped\n");
    else if (!s_fbp.here && !recomp_env(RENV_D3D11_DUMP))
        fprintf(stderr, "  [GPU] WARNING: fb_dump_prog is set without d3d11_dump;"
                        " no flips will be dumped\n");
}

/* The trigger: a draw at flip count k is presented by flip k+1. */
static void fb_prog_draw(void)
{
    uint32_t h, first;

    if (s_fbp.state == 0)
        fb_prog_init();
    if (s_fbp.state != 1 || !vsh_batch_active())
        return;
    h = nv2a_vsh_program_hash(&s_vsh, NULL);
    if (!nv2a_vsh_cdump_wanted(&s_fbp.cfg, s_gpu.flips, h))
        return;
    nv2a_fb_trigger_range(s_gpu.flips, s_fbp.ring.count, s_fbp.after,
                          &first, &s_fbp.lo, &s_fbp.hi);
    s_fbp.state = 2;
    fprintf(stderr, "  [GPU] fb_dump_prog: %08X first drawn at flip %u (draw %u);"
                    " dumping flips %u-%u\n", h, s_gpu.flips, s_gpu.draws,
            s_fbp.ring.count ? first : s_fbp.lo, s_fbp.hi);
}

/* vsh_dump and the vsh trace on a GPU backend, where raster_batch_vsh_one
 * (which notes the program on the CPU path) never runs. Only when asked: the
 * hash walks the whole program, every draw. */
static void vsh_note_gpu(void)
{
    static int on = -1;

    if (on < 0)
        on = s_backend != &s_cpu_backend
          && (recomp_env(RENV_VSH_DUMP) || recomp_env(RENV_VSH_TRACE));
    if (on && vsh_batch_active())
        (void)vsh_note_program();
}

/* What a batch actually contains. Before anything can be rasterised, the
 * question is what space attribute 0 arrives in: a title running a vertex
 * program hands over object-space positions that mean nothing without running
 * it, while pre-transformed screen-space coordinates can be drawn directly. */
static void draw_primitive(void)
{
    float v[4];
    uint32_t i;

    if (!s_gpu.prim || !s_gpu.idx_count)
        return;
    s_gpu.draws++;
    if (s_gpu.idx_count > s_idx_max)
        s_idx_max = s_gpu.idx_count;
    /* ~14 lines a second at 2.8k batches a frame, each a locked write to
     * stderr from the walker: verbose only. */
    if ((s_gpu.draws % 200) == 0 && recomp_env(RENV_PB_EXEC_VERBOSE))
        fprintf(stderr, "  [GPU] draw #%u\n", s_gpu.draws);
    s_gpu.verts += s_gpu.idx_count;

    /* How many batches carry coordinates at all, and what range they span.
     * A pipeline that decodes perfectly and draws nothing is indistinguishable
     * from one that never ran, unless the vertices themselves are measured. */
    {
        float p[4];
        if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p)) {
            if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) {
                s_gpu.nonzero_draws++;
                if (p[0] < s_gpu.min_x) s_gpu.min_x = p[0];
                if (p[0] > s_gpu.max_x) s_gpu.max_x = p[0];
                if (p[1] < s_gpu.min_y) s_gpu.min_y = p[1];
                if (p[1] > s_gpu.max_y) s_gpu.max_y = p[1];
            }
        }
    }

    if (s_gpu.color_keep != 0xFFFFFFFFu)   /* a depth/stencil-only pass draws no colour */
        present_note(0);
    vsh_const_dump_draw();
    vsh_note_gpu();
    fb_prog_draw();
    if (s_backend->on_draw) {
        uint64_t t0 = rt_now();
        struct nv2a_pb_draw d;
        d.prim         = s_gpu.prim;
        d.attr         = s_gpu.attr;
        d.idx          = s_gpu.idx;
        d.idx_count    = s_gpu.idx_count;
        d.inline_data  = s_gpu.inline_active;
        d.inline_buf   = s_gpu.inline_buf;
        d.inline_bytes = s_gpu.inline_count * 4;
        d.program      = vsh_batch_active();
        if (cpu_px_on()) {
            uint32_t before[4] = {0};
            float zb[4] = {0};
            int n;
            for (n = 0; n < s_cpx_n; n++) {
                before[n] = cpu_px_read(n);
                zb[n] = cpu_px_z(n);
            }
            s_backend->on_draw(&d);
            cpu_px_report(before, zb);
        } else
            s_backend->on_draw(&d);
        s_perf.draw += rt_now() - t0;
        if (s_backend->writes_back)
            s_gpu_backend_batches++;
    }
    s_perf.batches++;

    if (recomp_env(RENV_PB_EXEC_VERBOSE)) {
        static int shown;
        if (shown++ < 6) {
            fprintf(stderr, "  [GPU] prim %u, %u indices, pos attr:"
                            " off 0x%08X type %u size %u stride %u\n",
                    s_gpu.prim, s_gpu.idx_count, s_gpu.attr[0].offset,
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride);
            /* The texture stage, for either kind of batch. This used to print
             * only for inline batches, which meant a title drawing through
             * vertex arrays -- Half-Life 2's menu, for one -- showed no
             * texture state at all, and the reason a quad sampled flat was
             * invisible. */
            fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                            " colour 0x%02X swizzled %d valid %d\n",
                    s_gpu.tex.offset, s_gpu.tex.width, s_gpu.tex.height,
                    s_gpu.tex.pitch, s_gpu.tex.color,
                    d3d8_format_is_swizzled(s_gpu.tex.color), s_gpu.tex.valid);
            {
                uint32_t k;
                for (k = 0; k < s_gpu.idx_count && k < 3; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
            }
            /* An inline batch has no guest buffer to go and look at -- the
             * vertices are the payload -- so print the payload too. */
            if (s_gpu.inline_active) {
                uint32_t k;
                fprintf(stderr, "  [GPU]   inline %u dwords:", s_gpu.inline_count);
                for (k = 0; k < s_gpu.inline_count && k < 16; k++)
                    fprintf(stderr, " %08X", s_gpu.inline_buf[k]);
                fprintf(stderr, "\n");
                for (k = 0; k < s_gpu.idx_count && k < 4; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
                fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                                " colour 0x%02X valid %d\n",
                        s_gpu.tex.offset, s_gpu.tex.width, s_gpu.tex.height,
                        s_gpu.tex.pitch, s_gpu.tex.color, s_gpu.tex.valid);
            }
            {
                /* Every attribute the batch has, not just position. If the
                 * other streams carry data and position does not, the problem
                 * is one buffer rather than the whole vertex path. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t a, k;
                for (a = 0; a < NV_VERTEX_ATTRS; a++) {
                    const VertexAttr *at = &s_gpu.attr[a];
                    uint32_t nz = 0;
                    if (!at->offset || !at->size)
                        continue;
                    for (k = 0; k < 64; k++)
                        if (mem[at->offset + k]) nz++;
                    fprintf(stderr, "  [GPU]   attr%-2u off 0x%08X type %u"
                                    " size %u stride %-3u  %u/64 bytes set\n",
                            a, at->offset, at->type, at->size, at->stride, nz);
                }
            }
            {
                /* Raw bytes at the array, in case the values read as zero:
                 * that looks the same whether the offset is wrong or the
                 * buffer genuinely has not been filled yet. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t k;
                fprintf(stderr, "  [GPU]   bytes @0x%08X:", s_gpu.attr[0].offset);
                for (k = 0; k < 32; k++)
                    fprintf(stderr, " %02X", mem[s_gpu.attr[0].offset + k]);
                fprintf(stderr, "\n");
                fprintf(stderr, "  [GPU]   indices:");
                for (k = 0; k < s_gpu.idx_count && k < 8; k++)
                    fprintf(stderr, " %u", s_gpu.idx[k]);
                fprintf(stderr, "\n");
            }
            for (i = 0; i < s_gpu.idx_count && i < 3; i++) {
                if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], v))
                    fprintf(stderr, "  [GPU]   v[%u] = %.3f %.3f %.3f %.3f\n",
                            s_gpu.idx[i], v[0], v[1], v[2], v[3]);
            }
        }
    }
}

/* Draw the vertices the title wrote straight into the pushbuffer.
 *
 * INLINE_ARRAY carries no offsets and no indices: the dwords between BEGIN and
 * END *are* the vertex buffer, packed in attribute order using the same
 * SET_VERTEX_DATA_ARRAY_FORMAT registers an ordinary array would use. So the
 * whole batch is describable as a vertex array whose base happens to be that
 * payload, which means synthesising the layout and handing it to the existing
 * path -- rather than a second copy of the topology and rasterisation code.
 *
 * The title's own attribute table is saved and put back: these offsets and
 * strides are ours, and it has not stopped using its.
 *
 * ponytail: each attribute is padded to a whole dword. That is exact for the
 * float and D3DCOLOR formats every inline batch actually uses; a packed
 * sub-dword attribute would need the unpadded layout.
 */
static void draw_inline_array(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t off = 0, a, i, vsize, count;

    memcpy(saved, s_gpu.attr, sizeof saved);

    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        uint32_t bytes;
        if (!s_gpu.attr[a].size)
            continue;
        switch (s_gpu.attr[a].type) {
        case 0:  bytes = 4;                        break;  /* D3DCOLOR   */
        case 2:  bytes = 4 * s_gpu.attr[a].size;   break;  /* float      */
        case 4:  bytes = s_gpu.attr[a].size;       break;  /* ubyte norm */
        default: bytes = 4 * s_gpu.attr[a].size;   break;
        }
        s_gpu.attr[a].offset = off;
        off += (bytes + 3u) & ~3u;
    }
    vsize = off;
    if (!vsize)
        goto out;

    count = (s_gpu.inline_count * 4) / vsize;
    if (count < 3 || count > NV_MAX_INDICES)
        goto out;
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size)
            s_gpu.attr[a].stride = vsize;

    for (i = 0; i < count; i++)
        s_gpu.idx[i] = (uint16_t)i;
    s_gpu.idx_count = count;

    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

out:
    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
}

/* Draw the vertices immediate mode completed.
 *
 * Same trick as draw_inline_array: rather than a second copy of the topology
 * and rasterisation code, describe what was accumulated as an ordinary vertex
 * array and hand it to the existing path. The layout is ours and fixed, so
 * the attribute table is written out here rather than derived from the
 * title's format registers.
 *
 * The title's own table is saved and put back -- it has not stopped using it.
 *
 * Every attribute is a float4 (upstream 5c3a42c). This used to carry
 * position, an attr-3 D3DCOLOR and texcoord 0 only, and read every other
 * attribute once per batch, so a quad with a second texture coordinate got
 * the last vertex's value on all four corners: Burnout 3 composites its
 * whole 3D scene into the frame with exactly such a quad. The D3D11 and
 * Metal backends read the same descriptor, so they had the same bug. */
static void draw_immediate(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t i;

    if (s_gpu.imm_count < 3)
        return;

    memcpy(saved, s_gpu.attr, sizeof saved);
    memset(s_gpu.attr, 0, sizeof s_gpu.attr);
    /* Offsets are byte offsets into inline_buf here, not guest addresses --
     * fetch_attr reads them that way while inline_active is set, which is
     * also why 0 is a legal offset for position. */
    for (i = 0; i < NV_VERTEX_ATTRS; i++) {
        s_gpu.attr[i].type = NV2A_VTX_FLOAT;
        s_gpu.attr[i].size = 4;
        s_gpu.attr[i].offset = i * 16;
        s_gpu.attr[i].stride = IMM_VERTEX_DWORDS * 4;
    }

    for (i = 0; i < s_gpu.imm_count && i < NV_MAX_INDICES; i++)
        s_gpu.idx[i] = (uint16_t)i;
    s_gpu.idx_count = i;

    /* fetch_attr bounds-checks against inline_count dwords. */
    s_gpu.inline_count = s_gpu.imm_count * IMM_VERTEX_DWORDS;
    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
    s_gpu.inline_count = 0;
}

/* A vertex is complete: append every attribute's current inline value
 * (s_vsh.inl, kept by vsh_inline_attr for every SET_VERTEX_DATA* form) in
 * the layout draw_immediate describes. An attribute the batch never set
 * carries its standing value, as on the GPU. */
static void imm_emit_vertex(void)
{
    uint32_t at = s_gpu.imm_count * IMM_VERTEX_DWORDS;

    if (!s_gpu.prim || at + IMM_VERTEX_DWORDS > NV_MAX_INLINE)
        return;
    memcpy(&s_gpu.inline_buf[at], s_vsh.inl, sizeof s_vsh.inl);
    s_gpu.imm_count++;
}

/* The immediate-mode writes (xemu pgraph.c SET_VERTEX_DATA*). Returns 1 if
 * `method` was one of them. The values themselves are recorded by
 * vsh_inline_attr, which runs first; this decides when a vertex is complete:
 * the last component of attribute 0, the position, in whichever form it
 * arrives (VERTEX3F/4F, DATA2F/4F/2S/4S/4UB).
 *
 * Split out because it is a range test against seven separate bases, and
 * that reads better than seven more cases in an already long switch.
 */
static int imm_vertex_method(uint32_t method, uint32_t param)
{
    uint32_t off;
    (void)param;

    if (method >= NV097_SET_VERTEX4F && method < NV097_SET_VERTEX4F + 16) {
        if ((method - NV097_SET_VERTEX4F) / 4 == 3)   /* w completes it */
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX3F && method < NV097_SET_VERTEX3F + 12) {
        if ((method - NV097_SET_VERTEX3F) / 4 == 2)   /* z; w is 1 */
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2F_M
            && method < NV097_SET_VERTEX_DATA2F_M + NV_VERTEX_ATTRS * 8) {
        off = method - NV097_SET_VERTEX_DATA2F_M;
        if (off / 8 == 0 && (off % 8) / 4 == 1)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M
            && method < NV097_SET_VERTEX_DATA4F_M + NV_VERTEX_ATTRS * 16) {
        off = method - NV097_SET_VERTEX_DATA4F_M;
        if (off / 16 == 0 && (off % 16) / 4 == 3)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2S_M
            && method < NV097_SET_VERTEX_DATA2S_M + NV_VERTEX_ATTRS * 4) {
        if ((method - NV097_SET_VERTEX_DATA2S_M) / 4 == 0)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB
            && method < NV097_SET_VERTEX_DATA4UB + NV_VERTEX_ATTRS * 4) {
        if ((method - NV097_SET_VERTEX_DATA4UB) / 4 == 0)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4S_M_M
            && method < NV097_SET_VERTEX_DATA4S_M_M + NV_VERTEX_ATTRS * 8) {
        off = method - NV097_SET_VERTEX_DATA4S_M_M;
        if (off / 8 == 0 && (off % 8) / 4 == 1)
            imm_emit_vertex();
        return 1;
    }
    return 0;
}
void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    static int inited;
    if (!inited) {
        int a;
        inited = 1;
        s_gpu.min_x = s_gpu.min_y = 1e30f;
        s_gpu.max_x = s_gpu.max_y = -1e30f;
        /* An attribute no SET_VERTEX_DATA* ever wrote reads (0, 0, 0, 1),
         * as on the GPU (xemu's inline_value reset), for the program path
         * and for the immediate-mode snapshot alike. */
        for (a = 0; a < NV_VERTEX_ATTRS; a++)
            s_vsh.inl[a][3] = 1.0f;
#ifdef _WIN32
        {   /* RECOMP_PB_BACKEND=d3d11: src/d3d/nv2a_pb_d3d11.c */
            extern void nv2a_pb_d3d11_register_from_env(void);
            nv2a_pb_d3d11_register_from_env();
        }
#endif
#ifdef __APPLE__
        {   /* RECOMP_PB_BACKEND=metal: src/d3d/nv2a_pb_metal.m */
            extern void nv2a_pb_metal_register_from_env(void);
            nv2a_pb_metal_register_from_env();
        }
#endif
        {
            const char *b = recomp_env(RENV_PB_BACKEND);
            if (b && !strcmp(b, "null")) {
                const char *us = recomp_env(RENV_PB_NULL_DRAW_US);
                s_null_draw_us = us ? atoi(us) : 0;
                if (s_null_draw_us > 0)
                    s_null_backend.on_draw = null_on_draw;
                nv2a_pb_set_backend(&s_null_backend);
                fprintf(stderr, "  [GPU] RECOMP_PB_BACKEND=null: walker only, nothing drawn\n");
            }
        }
        /* render.scale is a GPU-backend feature: the CPU rasteriser draws
         * into guest memory at the guest pitch, so it stays 1x. Said once,
         * so a shared config.toml is not a silent no-op here. */
        if (nv2a_host_render_scale() > 1
                && (s_backend == &s_cpu_backend || s_backend == &s_null_backend))
            fprintf(stderr, "[ENHANCE] render.scale=%u ignored: the CPU rasteriser"
                    " draws into guest memory at the guest size\n",
                    nv2a_host_render_scale());
        /* A texture pack is a host-side replacement at upload; the CPU
         * rasteriser samples guest memory and has no upload to replace. */
        if (nv2a_tex_pack_count()
                && (s_backend == &s_cpu_backend || s_backend == &s_null_backend))
            fprintf(stderr, "[ENHANCE] textures.pack ignored: the CPU rasteriser"
                    " samples guest memory\n");
    }
    s_perf.methods++;
    /* Bring-up: the first parameters each surface method carries. A wrong
     * pitch or clip is indistinguishable from a method never arriving unless
     * the values are visible. Read once: this runs for every method. */
    static int verbose = -1;
    if (verbose < 0)
        verbose = recomp_env(RENV_PB_EXEC_VERBOSE) != NULL;
    if (verbose) {
        static int shown[8];
        int slot = -1;
        switch (method) {
        case NV097_SET_SURFACE_CLIP_HORIZONTAL: slot = 0; break;
        case NV097_SET_SURFACE_CLIP_VERTICAL:   slot = 1; break;
        case NV097_SET_SURFACE_FORMAT:          slot = 2; break;
        case NV097_SET_SURFACE_PITCH:           slot = 3; break;
        case NV097_SET_SURFACE_COLOR_OFFSET:    slot = 4; break;
        case NV097_SET_COLOR_CLEAR_VALUE:       slot = 5; break;
        case NV097_CLEAR_SURFACE:               slot = 6; break;
        default: break;
        }
        if (slot >= 0 && shown[slot]++ < 4)
            fprintf(stderr, "  [GPU] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
    }

    /* RECOMP_SURF_TRACE=<flip>, or =0x<colour offset> to start when that
     * surface is first selected: from then on, the surface and clear
     * methods as they arrive (up to 600), to see which clip, pitch and clear
     * rect go with which render target. */
    if (subch == 0 && ((method >= 0x0200 && method <= 0x0214)
                       || (method >= 0x1D94 && method <= 0x1D9C))) {
        static long from = -2;
        static int lines;
        if (from == -2) {
            const char *e = recomp_env(RENV_SURF_TRACE);
            from = e ? strtol(e, NULL, 0) : -1;
        }
        if (from >= 0 && method == 0x0210 && param == (uint32_t)from)
            from = 0;                    /* =0x...: from that colour offset on */
        if (from >= 0 && from < 0x10000 && s_gpu.flips >= (uint32_t)from
                && lines < 600) {
            lines++;
            fprintf(stderr, "  [SURFTRACE] flip %u method 0x%04X param 0x%08X\n",
                    s_gpu.flips, method, param);
        }
    }
    if (subch != 0) {                      /* 3D class lives on subchannel 0 */
        note_unhandled(method, param);
        return;
    }
    if (vsh_method(method, param))
        return;
    switch (method) {
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_x = param & 0xFFFF;
        s_gpu.clip_w = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_y = param & 0xFFFF;
        s_gpu.clip_h = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        break;
    case NV097_SET_SURFACE_PITCH:
        s_gpu.pitch = param & 0xFFFF;      /* colour pitch; zeta is the top half */
        break;
    case NV097_SET_SURFACE_COLOR_OFFSET:
        s_gpu.color_offset = param;
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        s_gpu.clear_color = param;
        break;
    case NV097_SET_CLEAR_RECT_HORIZONTAL:
        s_gpu.clear_rect_h = param;
        s_gpu.clear_rect_set = 1;
        break;
    case NV097_SET_CLEAR_RECT_VERTICAL:
        s_gpu.clear_rect_v = param;
        s_gpu.clear_rect_set = 1;
        break;
    case NV097_SET_BLEND_ENABLE:
        s_gpu.blend_enable = param;
        break;
    case NV097_SET_BLEND_FUNC_SFACTOR:
        s_gpu.blend_sfactor = param;
        break;
    case NV097_SET_BLEND_FUNC_DFACTOR:
        s_gpu.blend_dfactor = param;
        break;
    case NV097_SET_BLEND_COLOR:
        s_gpu.blend_color = param;
        s_gpu.blend_color_writes++;
        break;
    case NV097_SET_BLEND_EQUATION:
        s_gpu.blend_equation = param;
        break;
    case NV097_SET_COLOR_MASK:
        /* One enable bit per channel (xemu pgraph.c): B bit 0, G bit 8,
         * R bit 16, A bit 24. A title can draw a stencil-only quad round the
         * player with all four off; written anyway it is a white square. */
        s_gpu.color_keep = ((param & (1u << 24)) ? 0 : 0xFF000000u)
                         | ((param & (1u << 16)) ? 0 : 0x00FF0000u)
                         | ((param & (1u <<  8)) ? 0 : 0x0000FF00u)
                         | ((param & (1u <<  0)) ? 0 : 0x000000FFu);
        break;
    case NV097_CLEAR_SURFACE:
        if (param & 0xF0)
            present_note(1);
        if (s_backend->on_zclear)
            s_backend->on_zclear(param);
        if (s_backend->on_clear)
            s_backend->on_clear(param);
        break;

    case NV097_SET_BEGIN_END:
        if (param) {
            s_gpu.prim = param;
            s_gpu.idx_count = 0;
            s_idx_cut = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        } else {
            /* Three ways a batch can have arrived, and only one is in use at
             * a time: vertices completed by SET_VERTEX4F, a payload written
             * with INLINE_ARRAY, or indices into the title's own arrays. */
            if (s_gpu.imm_count)
                draw_immediate();
            else if (s_gpu.inline_count)
                draw_inline_array();
            else
                draw_primitive();
            if (s_idx_cut)
                s_idx_overflow++;
            s_idx_cut = 0;
            s_gpu.prim = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        }
        break;

    case NV097_INLINE_ARRAY:
        /* Vertex data, not a pointer to it. Buffered rather than decoded here
         * because the format is only fully known at END. */
        if (s_gpu.prim && s_gpu.inline_count < NV_MAX_INLINE)
            s_gpu.inline_buf[s_gpu.inline_count++] = param;
        else if (s_gpu.prim && !s_inline_overflow++)
            fprintf(stderr, "  [GPU] INLINE_ARRAY batch over %u dwords:"
                            " the rest is dropped\n", (unsigned)NV_MAX_INLINE);
        break;

    case NV097_DRAW_ARRAYS: {
        /* The method this title actually draws with, and the reason the
         * executor reported zero draws while geometry was being submitted the
         * whole time: BEGIN_END arrived, END arrived, and in between came a
         * run description rather than the index list the draw path wanted, so
         * every batch ended with idx_count == 0 and was dropped in silence.
         *
         * Expanded into indices because that is what the rasteriser consumes,
         * and an implicit run is just the indices start..start+count-1. */
        uint32_t start = param & 0x00FFFFFFu;
        uint32_t count = ((param >> 24) & 0xFFu) + 1u;
        uint32_t i;

        if (!s_gpu.prim)
            break;
        /* The hardware faults on a start past 0xFFFF, so no title sends one:
         * a count here is a decode bug, not a title to support. */
        if (start + count - 1u > 0xFFFFu)
            s_idx_over_ffff++;
        for (i = 0; i < count && s_gpu.idx_count < NV_MAX_INDICES; i++)
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(start + i);
        if (i < count)
            note_idx_overflow();
        break;
    }

    case NV097_ARRAY_ELEMENT16:
        /* Two 16-bit indices per parameter word. DRAW_ARRAYS and
         * ARRAY_ELEMENT32 can leave the count odd, so with one slot left the
         * first half still goes in. */
        if (s_gpu.prim && s_gpu.idx_count + 2 <= NV_MAX_INDICES) {
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param & 0xFFFF);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param >> 16);
        } else if (s_gpu.prim) {
            if (s_gpu.idx_count < NV_MAX_INDICES)
                s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param & 0xFFFF);
            note_idx_overflow();
        }
        break;
    case NV097_ARRAY_ELEMENT32:
        /* One index per word: the XDK sends the last index of an odd-count
         * DrawIndexedVertices this way, and without it the batch loses its
         * last primitive. The word may carry a wider index; idx is 16-bit
         * (the vertex range DRAW_ARRAYS faults past), so one past 0xFFFF is
         * truncated and counted rather than silently aliased. */
        if (s_gpu.prim && s_gpu.idx_count < NV_MAX_INDICES) {
            if (param > 0xFFFFu)
                s_idx_over_ffff++;
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)param;
        } else if (s_gpu.prim) {
            note_idx_overflow();
        }
        break;
    case NV097_SET_TEXTURE_OFFSET:
        /* A texture offset is a DMA-object offset, exactly like a surface or a
         * vertex array offset -- physical, and reachable only through the
         * contiguous window when it names contiguous memory. */
        s_gpu.tex.offset = dma_resolve(param);
        record_tex_reg(method, param);
        break;

    case NV097_SET_CONTEXT_DMA_REPORT:
        ctxdma_bind(&s_report_bind, "report", param);
        return;
    case NV097_CLEAR_REPORT_VALUE:
        s_zpass_count = 0;
        if (s_backend->on_zpass_clear)
            s_backend->on_zpass_clear();
        return;
    case NV097_SET_ZPASS_PIXEL_COUNT_ENABLE:
        s_zpass_enable = param != 0;
        if (s_backend->on_zpass)
            s_backend->on_zpass(s_zpass_enable);
        return;
    case NV097_GET_REPORT:
        get_report(param);
        return;

    case NV097_SET_CONTEXT_DMA_SEMAPHORE:
        s_gpu.sema_dma = param;
        ctxdma_bind(&s_sema_bind, "semaphore", param);
        return;

    case NV097_SET_SEMAPHORE_OFFSET:
        s_gpu.sema_offset = param;
        s_gpu.sema_offset_set = 1;
        return;

    case NV097_BACK_END_WRITE_SEMAPHORE_RELEASE: {
        /* The semaphore offset is an offset inside the semaphore's context
         * DMA object, whose base and limit were read from instance memory
         * when it was bound (ctxdma_bind); base plus offset is then a
         * physical address like every other one here and goes through the
         * same resolution. The first release is logged. */
        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
        uint32_t va, phys;

        if (!s_gpu.sema_offset_set) {
            /* No offset yet: nowhere honest to write. */
            note_unhandled(method, param);
            return;
        }
        if (!ctxdma_place(&s_sema_bind, "semaphore", s_gpu.sema_offset, 4,
                          &phys))
            return;
        va = dma_resolve(phys);
        if (surface_hits_image(va, 4)) {
            static int said;
            if (!said++)
                fprintf(stderr, "  [GPU] REFUSING semaphore release to 0x%08X"
                        " (offset 0x%08X): inside the loaded image\n",
                        va, s_gpu.sema_offset);
            return;
        }
        reports_settle();  /* the release orders after the reports */
        *(volatile uint32_t *)(mem + va) = param;
        s_sema_last_va = va;
        s_sema_last_value = param;
        xbox_SpinWake();   /* a fence wait may be sleeping on this word */
        if (s_sema_releases++ == 0 || recomp_env(RENV_PB_SEMA_TRACE)) {
            static unsigned shown;
            if (shown++ < 16) {
                fprintf(stderr, "  [GPU] semaphore release 0x%08X -> 0x%08X"
                        " (dma 0x%08X offset 0x%08X)\n", param, va,
                        s_gpu.sema_dma, s_gpu.sema_offset);
                fflush(stderr);
            }
        }
        return;
    }

    case NV097_NO_OPERATION:
        /* Non-zero, it is one of D3D's software methods, which the GPU traps
         * to D3D's interrupt handler on the console. Only the swap's interval
         * matters here (nv2a_flip_hold.h); the rest stays a no-op. */
        if (param)
            xbox_FlipHoldSwap(&s_flip_hold, param);
        return;

    case NV097_SET_FLIP_READ:
        s_gpu.flip_read = param;
        return;

    case NV097_SET_FLIP_WRITE:
        s_gpu.flip_write = param;
        return;

    case NV097_SET_FLIP_MODULO:
        s_gpu.flip_modulo = param;
        return;

    case NV097_FLIP_INCREMENT_WRITE:
        s_gpu.flip_write = s_gpu.flip_modulo
                         ? (s_gpu.flip_write + 1) % s_gpu.flip_modulo
                         : s_gpu.flip_write + 1;
        s_gpu.flips++;
        vsh_const_dump_flip();
        return;

    case NV097_FLIP_STALL: {
        /* The flip is measured from the vblank the title swapped in (the
         * walk's start, s_walk_vbl); the hold's clock starts here, before
         * the backend's present: a vsynced present can hold this thread for
         * a host frame, and that time counts against the hold. */
        uint64_t stall_ns = xbox_HostNowNs();
        int clock_ok;
        uint32_t stall_vbl;
        (void)flip_clock(stall_ns, &clock_ok);
        stall_vbl = s_walk_vbl;
        /* The stall ends when the buffer being read is the one just finished.
         * There is no scanout here to wait for, so that is now; when the
         * title may go on is the flip hold's (below). */
        s_gpu.flip_read = s_gpu.flip_write;
        /* And this is a completed swap, which is what a title's own swap
         * counter counts -- see xbox_Nv2aFrameCounterFlip. */
        xbox_Nv2aFrameCounterFlip();
        /* RECOMP_TRACE=pacing: the flip's time, here where every backend
         * passes, with the draws since the last (CPU and GPU paths). */
        {
            static uint32_t prev;
            uint32_t b = s_gpu.batches_textured + s_gpu.batches_no_uv
                       + s_gpu.batches_no_tex + s_gpu_backend_batches;
            xbox_PacingFlip(s_gpu.flips, b - prev);
            prev = b;
        }
        /* Present the frame just finished: present_pick says which surface
         * that is, for every backend. Before anything qualifies (no draw or
         * clear yet), the old answer: the last surface drawn, else the
         * current one. */
        present_pick();
        /* The GPU backends apply live options (nv2a_host_opts_request) in
         * their on_flip, after the present; these two have none that does,
         * so it is done here and a request never stays pending. Scale still does not
         * apply to them, said again as at startup. */
        if (s_backend == &s_cpu_backend || s_backend == &s_null_backend) {
            unsigned changed = nv2a_host_opts_apply();
            if ((changed & NV2A_OPT_SCALE) && nv2a_host_render_scale() > 1)
                fprintf(stderr, "[ENHANCE] render.scale=%u ignored: the CPU rasteriser"
                        " draws into guest memory at the guest size\n",
                        nv2a_host_render_scale());
        }
        if (s_backend->on_flip) {
            uint64_t t0 = rt_now();
            if (s_present.offset)
                s_backend->on_flip(s_present.offset, s_present.pitch);
            else
                s_backend->on_flip(s_gpu.drawn_offset ? s_gpu.drawn_offset
                                                      : s_gpu.color_offset,
                                   s_gpu.pitch);
            s_perf.flip += rt_now() - t0;   /* Present, for the [PB-PERF] line */
        }
        /* The window title's FPS and draws: one call per flip, with the
         * draws of the frame just finished. */
        {
            static uint32_t last_draws;
            xbox_FramebufferWindowFrameStats(s_gpu.draws - last_draws);
            last_draws = s_gpu.draws;
        }
        dump_at_flip(s_gpu.flips);
        /* RECOMP_FB_DUMP_FLIPS=<n>: with RECOMP_FB_DUMP, the finished frame
         * at every nth flip that drew something (up to 200), so a run leaves
         * whole frames behind rather than only the mid-frame 3D dumps. */
        {
            static int every = -1;
            static unsigned dumped;
            static uint32_t prev_tris;
            uint32_t drew = s_gpu.tris_drawn + s_gpu_backend_batches;
            if (every < 0) {
                const char *e = recomp_env(RENV_FB_DUMP_FLIPS);
                every = e ? atoi(e) : 0;
            }
            if (every > 0 && s_gpu.flips % (uint32_t)every == 0
                    && drew != prev_tris && dumped < 200) {
                dumped++;
                fprintf(stderr, "  [GPU] flip %u: frame dump\n", s_gpu.flips);
                dump_present_bmp();
            }
            if (every > 0 && s_gpu.flips % (uint32_t)every == 0)
                prev_tris = drew;
        }
        /* RECOMP_FLIP_LOG=1: every flip, with the batches drawn since the last. */
        {
            static int on = -1;
            static uint32_t prev_b, prev_t;
            static uint64_t prev_px, prev_zr, prev_clip;
            static uint32_t prev_tris, prev_drop;
            if (on < 0)
                on = recomp_env(RENV_FLIP_LOG) != NULL;
            if (on) {
                uint32_t b = s_gpu.batches_textured + s_gpu.batches_no_uv
                           + s_gpu.batches_no_tex;
                uint32_t done = s_present.offset ? s_present.offset
                              : s_gpu.drawn_offset ? s_gpu.drawn_offset
                                                   : s_gpu.color_offset;
                fprintf(stderr, "[GPU] flip %u %lu ms fence 0x%08X batches %u"
                        " tex %u presents 0x%08X %ux%u raster %.3f ms (xf %.3f setup"
                        " %.3f texdec %.3f px %.3f) yuv %u wr %llu zrej %llu"
                        " tris %u drop %u clip %llu\n",
                        s_gpu.flips,
                        xbox_log_ms(), s_sema_last_value, b - prev_b,
                        s_gpu.batches_textured - prev_t, done,
                        s_present.w, s_present.h,
                        s_rt.total / 1e6, s_rt.xform / 1e6, s_rt.setup / 1e6,
                        s_rt.tex / 1e6,
                        (double)(int64_t)(s_rt.total - s_rt.xform - s_rt.setup
                                          - s_rt.tex) / 1e6,
                        s_rt.yuv_batches,
                        (unsigned long long)(s_gpu.pixels - prev_px),
                        (unsigned long long)(s_vsh.z_rejected - prev_zr),
                        s_vsh.tris - prev_tris, s_vsh.tris_clipped - prev_drop,
                        (unsigned long long)(s_clip_tris - prev_clip));
                prev_tris = s_vsh.tris; prev_drop = s_vsh.tris_clipped;
                prev_clip = s_clip_tris;
                prev_b = b; prev_t = s_gpu.batches_textured;
                prev_px = s_gpu.pixels; prev_zr = s_vsh.z_rejected;
                memset(&s_rt, 0, sizeof s_rt);
                {
                    uint64_t now = rt_now(), kwait = 0;
                    uint64_t scan = s_perf.scan + (now - s_perf.scan_t0);
                    uint32_t kicks = xbox_Nv2aKickStats(&kwait);
                    extern uint32_t nv2a_pb_scan_stops(void);
                    static uint32_t prev_stops;
                    uint32_t stops = nv2a_pb_scan_stops();
                    /* decode is walk minus draw minus present; kicks are the
                     * title's KickOff flushes acknowledged since the last
                     * flip, kickwait how long it spun on them in all (on
                     * native Windows it under-reads by up to 0.5 ms a kick:
                     * the observer naps on the timer and sees kicks late), and
                     * stops the walks that stopped short of PUT since then
                     * (the lifetime total is in the [PB] report). */
                    fprintf(stderr, "[PB-PERF] flip %u wall %.2f ms walk %.2f ms"
                            " (draw %.2f, decode %.2f) methods %u batches %u"
                            " present %.2f kicks %u kickwait %.2f stops %u\n",
                            s_gpu.flips,
                            s_perf.flip_t0 ? (now - s_perf.flip_t0) / 1e6 : 0.0,
                            scan / 1e6, s_perf.draw / 1e6,
                            (double)(int64_t)(scan - s_perf.draw - s_perf.flip) / 1e6,
                            s_perf.methods, s_perf.batches,
                            s_perf.flip / 1e6, kicks, kwait / 1e6,
                            stops - prev_stops);
                    prev_stops = stops;
                    s_perf.scan = s_perf.draw = s_perf.flip = 0;
                    s_perf.methods = s_perf.batches = 0;
                    s_perf.scan_t0 = s_perf.flip_t0 = now;
                }
            }
        }
        /* The first few always, so a log says whether flips happen at all
         * and which surface each one put on screen; more with VERBOSE. */
        {
            static unsigned n;
            unsigned limit = recomp_env(RENV_PB_EXEC_VERBOSE) ? 8u : 4u;
            if (n++ < limit) {
                uint32_t done = s_present.offset ? s_present.offset
                              : s_gpu.drawn_offset ? s_gpu.drawn_offset
                                                   : s_gpu.color_offset;
                fprintf(stderr, "  [GPU] flip %u: read=%u write=%u presents"
                        " 0x%08X (-> 0x%08X)\n", s_gpu.flips, s_gpu.flip_read,
                        s_gpu.flip_write, done, done ? dma_resolve(done) : 0u);
                fflush(stderr);
            }
        }
        /* Arm the hold. Not inside a pushbuffer CALL: the walk's return
         * address is not kept across walks, so it could not resume there;
         * that flip completes at once and still sets the count the next one
         * is measured from. */
        if (nv2a_pb_exec_flip_pacing()) {
            extern int nv2a_pb_scan_in_call(void);
            xbox_FlipHoldStall(&s_flip_hold, stall_vbl, stall_ns,
                               clock_ok && !nv2a_pb_scan_in_call());
        }
        return;
    }

    case NV097_SET_TEXTURE_FORMAT:
        s_gpu.tex.color = (param >> 8) & 0xFF;
        s_gpu.tex.border_on = (param >> 3) & 1;   /* BORDER_SOURCE: colour */
        /* A swizzled texture carries its own dimensions here, as log2 in
         * BASE_SIZE_U/V (nv2a_regs.h: 0x00F00000 / 0x0F000000). It has to:
         * SET_TEXTURE_IMAGE_RECT describes a linear image, and a title that
         * only uses swizzled textures never sends one -- one title sends it
         * once in a run and sets a format thousands of times. Without this the width and
         * height stayed zero and nothing was ever sampled. */
        if (nv2a_tex_size_from_format((param >> 8) & 0xFF)) {
            s_gpu.tex.width  = 1u << ((param >> 20) & 0xF);
            s_gpu.tex.height = 1u << ((param >> 24) & 0xF);
        }
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_ADDRESS:
        /* Four bits per axis. 1 is wrap, 3 is clamp-to-edge, 4 is border
         * (sample_tex); mirror falls back to clamp, which is wrong at an edge
         * rather than wrong everywhere. */
        s_gpu.tex.addr_u =  param        & 0xF;
        s_gpu.tex.addr_v = (param >>  8) & 0xF;
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_CONTROL1:
        /* Pitch lives in the top half. Only meaningful for a linear format; a
         * swizzled texture has no pitch because it has no rows. */
        s_gpu.tex.pitch = param >> 16;
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_IMAGE_RECT:
        s_gpu.tex.width  = param >> 16;
        s_gpu.tex.height = param & 0xFFFF;
        record_tex_reg(method, param);
        break;

    default:
        if (method >= NV_TEX_FIRST && method <= NV_TEX_LAST)
            record_tex_reg(method, param);
        if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET
                && method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + NV_VERTEX_ATTRS * 4) {
            /* Resolved here, once, so every consumer -- the rasteriser's
             * attribute reads and the diagnostics alike -- sees the same
             * address. A vertex array offset is a DMA-object offset exactly
             * like a surface offset: physical, and addressable only through
             * the window when it names contiguous memory. */
            s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4].offset =
                dma_resolve(param);
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT
                && method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
            a->type   =  param        & 0x0F;
            a->size   = (param >> 4)  & 0x0F;
            a->stride = (param >> 8)  & 0xFF;
        } else if (vsh_inline_attr(method, param), !imm_vertex_method(method, param)) {
            note_unhandled(method, param);
        }
        break;
    }
}

/* Find where the title actually wrote its quad.
 *
 * The GPU is pointed at a buffer that stays zero, which says the data went
 * somewhere else -- and the only way to find somewhere else is to look for the
 * data. A screen-space quad for a 640x480 target contains 640.0f and 480.0f as
 * floats, which is a distinctive enough pair to search guest RAM for. Whatever
 * address that turns up is where the title's writes are landing, and the
 * difference from the programmed offset is the bug. */
static void find_quad_vertices(void)
{
    const uint32_t W = 0x44200000u;   /* 640.0f */
    const uint32_t H = 0x43F00000u;   /* 480.0f */
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for 640.0f/480.0f pairs...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 8 && hits < 12; i++) {
        if (ram[i] != W && ram[i] != H)
            continue;
        /* Both values within a few words of each other: a lone 640.0f is
         * common, the pair much less so. */
        {
            int has_w = 0, has_h = 0;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                if (ram[i + k] == W) has_w = 1;
                if (ram[i + k] == H) has_h = 1;
            }
            if (!has_w || !has_h)
                continue;
        }
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 8;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none found -- the quad is not in RAM in"
                        " that form\n");
    fflush(stderr);
}

/* Locate NaN-filled transform matrices in guest RAM.
 *
 * A matrix arriving as NaN says the maths went wrong somewhere upstream, and
 * the only way to find where is to find the matrix and watch who writes it.
 * Three consecutive real-indefinite values is a distinctive enough signature:
 * ordinary data does not contain runs of 0xFFC00000. */
static void find_nan_matrices(void)
{
    const uint32_t NAN_NEG = 0xFFC00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for NaN matrices...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 20 && hits < 10; i++) {
        if (ram[i] != NAN_NEG || ram[i + 1] != NAN_NEG || ram[i + 2] != NAN_NEG)
            continue;
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 16;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none in RAM -- the NaNs are computed into"
                        " registers, not stored\n");
    fflush(stderr);
}

/* Print guest dwords named by RECOMP_PEEK, as hex and as float.
 *
 * Chasing a value backwards means reading it, and a value that is only wrong
 * for one frame in a thousand cannot be caught by stopping. Both
 * interpretations are printed because the question is usually "is this a
 * pointer or a number", and guessing wrong costs a run. */
static void peek_addresses(void)
{
    const char *spec = recomp_env(RENV_PEEK);
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256];
    char *tok, *ctx = NULL;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t va = (uint32_t)strtoul(tok, NULL, 0);
        uint32_t v;
        float f;
        if (va < 0x1000u || va >= 0x04000000u)
            continue;
        v = *(const uint32_t *)(mem + va);
        memcpy(&f, &v, 4);
        fprintf(stderr, "  [PEEK] 0x%08X = %08X  (%g)\n", va, v, f);
    }
    fflush(stderr);
}

/* Walk a pointer chain and print every step.
 *
 * RECOMP_PEEK_CHAIN="0x1315A8,8,0x10,0" starts at that address, and for each
 * offset dereferences the current pointer and adds it. Following a chain by
 * hand costs one run per level; this costs one run for the whole chain, and
 * prints where it goes wrong when a level is null.
 */
static void peek_chain(void)
{
    const char *spec = recomp_env(RENV_PEEK_CHAIN);
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256], *tok, *ctx = NULL;
    uint32_t cur = 0;
    int step = 0;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t off = (uint32_t)strtoul(tok, NULL, 0);
        if (step == 0) {
            cur = off;
            fprintf(stderr, "  [CHAIN] start 0x%08X\n", cur);
        } else {
            if (cur < 0x1000u || cur + 4 >= 0x04000000u) {
                fprintf(stderr, "  [CHAIN] step %d: 0x%08X is not a guest"
                                " pointer -- chain ends\n", step, cur);
                return;
            }
            cur = *(const uint32_t *)(mem + cur) + off;
            fprintf(stderr, "  [CHAIN] step %d: deref +0x%X -> 0x%08X\n",
                    step, off, cur);
        }
        step++;
    }
    if (cur >= 0x1000u && cur + 4 < 0x04000000u)
        fprintf(stderr, "  [CHAIN] final value at 0x%08X = 0x%08X\n",
                cur, *(const uint32_t *)(mem + cur));
    fflush(stderr);
}

/* The report's RECOMP_FB_DUMP picture on its own, for a run that asked for
 * pictures but not for the [GPU] text. One picture per report rather than
 * per clear: a title clears hundreds of times a second and nobody wants that
 * many files. The last flip's surface, once there is one: mid-frame, the
 * current target is as likely an offscreen one. */
void nv2a_pb_exec_report_dump(void)
{
    if (s_present.offset)
        dump_present_bmp();
    else
        dump_surface_bmp();
}

void nv2a_pb_exec_report(void)
{
    peek_addresses();
    peek_chain();
    if (recomp_env(RENV_FIND_NAN)) {
        /* Every report, not once: the matrix is fine early on and only turns
         * to NaN later, so a single scan at startup finds nothing and says
         * nothing. */
        find_nan_matrices();
    }
    if (recomp_env(RENV_FIND_QUAD)) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    if (s_reports)
        fprintf(stderr, "[GPU] visibility reports %u (%u with count 0)\n",
                s_reports, s_reports_zero);
    fprintf(stderr, "[GPU] draws %u (%u with coordinates), %u indices;"
                    " x %.1f..%.1f  y %.1f..%.1f\n",
            s_gpu.draws, s_gpu.nonzero_draws, s_gpu.verts,
            s_gpu.min_x, s_gpu.max_x, s_gpu.min_y, s_gpu.max_y);
    /* Whether a title ever needs a big batch, without a probe. */
    fprintf(stderr, "[GPU] index batches: max %u, %u overflowed", s_idx_max,
            s_idx_overflow);
    if (s_idx_over_ffff)
        fprintf(stderr, ", %u indices over 0xFFFF", s_idx_over_ffff);
    if (s_xv_fail)
        fprintf(stderr, ", %u dropped for scratch", s_xv_fail);
    fputc('\n', stderr);
    vsh_report();
    nv2a_pb_exec_report_dump();

    /* Drawn and skipped separately: "nothing appeared" and "every batch needed
     * a vertex program we do not run" look identical on screen, and only one
     * of them means the rasteriser is broken. */
    fprintf(stderr, "[GPU] semaphore: %u releases, last 0x%08X at 0x%08X\n",
            s_sema_releases, s_sema_last_value, s_sema_last_va);
    fprintf(stderr, "[GPU] brightest pixel written 0x%08X\n", s_gpu.pixel_max);
    fprintf(stderr, "[GPU] %llu pixels written; draw surface 0x%08X"
                    " -> 0x%08X, clear surface 0x%08X -> 0x%08X\n",
            (unsigned long long)s_gpu.pixels, s_gpu.drawn_offset,
            dma_resolve(s_gpu.drawn_offset), s_gpu.color_offset,
            dma_resolve(s_gpu.color_offset));
    fprintf(stderr, "[GPU] rasterised %u triangles; %u batches skipped as not"
                    " screen-space, %u triangles fully off-surface\n",
            s_gpu.tris_drawn, s_gpu.batches_untransformed,
            s_gpu.tris_skipped_offscreen);

    /* And of the batches that did rasterise, how many sampled anything. A menu
     * that draws its background from one texture and its text from another
     * shows both as flat colour if either half is missing, so the split is
     * what says which half. */
    fprintf(stderr, "[GPU] batches: %u textured, %u with no texcoords,"
                    " %u with texcoords but no usable stage\n",
            s_gpu.batches_textured, s_gpu.batches_no_uv, s_gpu.batches_no_tex);
    for (i = 0; i < s_tex_use_count; i++)
        fprintf(stderr, "  [TEXUSE] 0x%08X %ux%u fmt 0x%02X%s: %u batches\n",
                s_tex_use[i].offset, s_tex_use[i].width, s_tex_use[i].height,
                s_tex_use[i].color,
                d3d8_format_dxt_block_bytes(s_tex_use[i].color) ? " dxt"
                    : d3d8_format_is_swizzled(s_tex_use[i].color) ? " swz" : " lin",
                s_tex_use[i].batches);

    if (recomp_env(RENV_TEX_STATE)) {
        uint32_t k;
        for (k = 0; k < sizeof s_tex_set / sizeof s_tex_set[0]; k++)
            if (s_tex_set[k])
                fprintf(stderr, "  [TEX] 0x%04X = 0x%08X\n",
                        (unsigned)(NV_TEX_FIRST + k * 4), s_tex_reg[k]);
    }

    /* Top ten by frequency: selection sort over a small table, once every few
     * seconds, is not worth a better algorithm.
     *
     * RECOMP_PB_UNHANDLED_ALL lists every one instead. Ten is the right
     * default -- the tail is a long list of state registers nobody needs to
     * read -- but when a title stops and the question is which method it
     * stopped on, the answer is as likely to be the one seen twice as the
     * one seen a thousand times, and ten hides it. */
    {
        int shown = recomp_env(RENV_PB_UNHANDLED_ALL) ? s_unhandled_count : 10;
    for (i = 0; i < shown && i < s_unhandled_count; i++) {
        int best = i;
        for (j = i + 1; j < s_unhandled_count; j++)
            if (s_unhandled[j].count > s_unhandled[best].count)
                best = j;
        if (best != i) {
            PbUnhandled t = s_unhandled[i];
            s_unhandled[i] = s_unhandled[best];
            s_unhandled[best] = t;
        }
        {
            /* The value as well as the count. A method nobody decoded is
             * a guess until you see what it carried: screen coordinates,
             * a 0..1 texcoord and a packed colour are told apart at a
             * glance, and that is what says which vertex encoding a title
             * is using. */
            union { uint32_t u; float f; } v;
            v.u = s_unhandled[i].last_param;
            fprintf(stderr, "  [GPU]   0x%04X x%-8u last=0x%08X (%.4f)\n",
                    s_unhandled[i].method, s_unhandled[i].count,
                    v.u, v.f);
        }
    }
    }    fflush(stderr);
}
