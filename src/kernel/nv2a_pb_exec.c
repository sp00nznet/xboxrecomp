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
 * has not given us any vertices". RECOMP_FB_DUMP=<prefix> writes the surface to
 * <prefix>NNN.bmp, so the result can be looked at without a display.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */
#include "xbox_memory_layout.h"   /* xbox_Nv2aFrameCounterFlip */
/* The swizzle decoder the D3D8 layer already uses -- one implementation of
 * Morton order, not a second one that can disagree with it. */
#include "../d3d/d3d8_swizzle.h"
#include "nv2a_backend.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowStart(void);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

/* Debug switches, read once. getenv() walks the whole environment, and these
 * were tested per method and per batch -- millions of times a second during a
 * level load, enough to stall the thread that also acknowledges the GPU. */
static int env_once(const char *name, int *cache)
{
    if (*cache < 0)
        *cache = getenv(name) != NULL;
    return *cache;
}
static int pb_verbose(void)   { static int c = -1; return env_once("RECOMP_PB_EXEC_VERBOSE", &c); }
static int pb_ffp_trace(void) { static int c = -1; return env_once("RECOMP_FFP_TRACE", &c); }
void nv2a_pb_exec_report(void);

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
    extern int xbox_ContiguousIsPhysical(uint32_t phys);

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
    if (xbox_ContiguousIsPhysical(offset))
        return XBOX_CONTIG_BASE + offset;
    if (!surface_hits_image(offset, 1))
        return offset;
    if ((uint64_t)offset < XBOX_CONTIG_SIZE)
        return XBOX_CONTIG_BASE + offset;
    return offset;                         /* nothing better to offer */
}

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
#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL   0x0204
#define NV097_SET_SURFACE_FORMAT          0x0208
#define NV097_SET_SURFACE_PITCH           0x020C
#define NV097_SET_SURFACE_COLOR_OFFSET    0x0210
#define NV097_SET_COLOR_CLEAR_VALUE       0x1D90
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
 * ponytail: the flip completes the moment it is asked for, because there is
 * no scanout to be in the middle of. That makes every frame land instantly
 * and a title that paces itself on the flip runs as fast as it can draw.
 * Pacing wants the vblank clock in the kernel, not a sleep in here. */
#define NV097_SET_FLIP_READ               0x0120
#define NV097_SET_FLIP_WRITE              0x0124
#define NV097_SET_FLIP_MODULO             0x0128
#define NV097_FLIP_INCREMENT_WRITE        0x012C
#define NV097_FLIP_STALL                  0x0130
#define NV097_ARRAY_ELEMENT16             0x1800
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
#define NV097_SET_VERTEX_DATA4UB          0x1940   /* + attr*4,  D3DCOLOR */

/* One immediate vertex, as this file packs it for the shared draw path:
 * position float4, diffuse D3DCOLOR, texcoord0 float2. */
#define IMM_VERTEX_DWORDS 7

#define NV097_CLEAR_COLOR_MASK            0xF0   /* R,G,B,A bits */

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t offset;      /* guest address of element 0 */
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

#define NV_VERTEX_ATTRS 16
#define NV_MAX_INDICES  65536
#define NV_MAX_INLINE   65536           /* dwords of INLINE_ARRAY per batch */

/* Texture stage 0, decoded from what the title programmed.
 *
 * Only stage 0: it is the only one the dashboard configures, and a stage
 * nothing writes to is a stage nothing can be sampled from. The rest arrive
 * as unhandled methods and are counted as such, which is how the next title
 * that needs them will say so. */
typedef struct {
    uint32_t offset;                    /* guest address of texel (0,0)  */
    uint32_t width, height;             /* from IMAGE_RECT               */
    uint32_t pitch;                     /* bytes per row, from CONTROL1  */
    uint32_t color;                     /* NV097 colour-format code      */
    uint32_t addr_u, addr_v;            /* wrap mode per axis            */
    int      valid;
} Texture;

static struct {
    VertexAttr attr[NV_VERTEX_ATTRS];
    uint32_t   prim;                    /* SET_BEGIN_END parameter, 0 = ended */
    uint32_t   idx[NV_MAX_INDICES];   /* DRAW_ARRAYS starts are 24-bit */
    uint32_t   idx_count;
    /* INLINE_ARRAY payload: vertices written straight into the pushbuffer
     * instead of into a buffer the title points at. Same vertex format, a
     * different place to read them from. */
    uint32_t   inline_buf[NV_MAX_INLINE];
    uint32_t   inline_count;
    /* Current values of the immediate-mode attributes, and how many complete
     * vertices they have produced in this batch. */
    float      imm_pos[4];
    uint32_t   imm_diffuse;
    float      imm_tex[2];
    uint32_t   imm_count;
    int        inline_active;
    uint32_t   draws, verts, nonzero_draws;
    float      min_x, max_x, min_y, max_y;
    uint32_t color_offset, color_base, pitch, format;
    /* The surface the last batch actually drew into. A double-buffered title
     * has already pointed color_offset at the next buffer and cleared it by
     * the time the flip arrives, so dumping the current one dumps the frame
     * that has not been drawn yet -- which is how a correctly rendered
     * sequence came out as 12 black BMPs. */
    uint32_t drawn_offset;
    uint32_t drawn_pitch;
    int      flip_seen;      /* once flips arrive, the window follows them */
    uint64_t pixels;
    uint32_t pixel_max;   /* brightest value any pixel write carried */
    uint32_t clip_x, clip_w, clip_y, clip_h;
    uint32_t clear_color;
    uint32_t clears, unhandled_total;
    uint32_t flip_read, flip_write, flip_modulo, flips;
    uint32_t tris_drawn, tris_skipped_offscreen, batches_untransformed;
    /* Why a batch came out flat. "Untextured" has two causes that look
     * identical on screen and want opposite fixes: the batch carried no
     * texture coordinates, or it did and the stage was not usable. */
    uint32_t batches_textured, batches_no_uv, batches_no_tex;
    Texture  tex;
    /* Fixed-function transform: the composite matrix (world*view*projection
     * *viewport, as D3D uploads it), the viewport offset added after the
     * perspective divide, and which transform unit is active. */
    uint32_t clip_raw_h, clip_raw_v;    /* SET_SURFACE_CLIP_* as sent */
    Nv2aRenderState rs;                 /* handed to a back end with each batch */
    uint32_t zstencil_clear;            /* SET_ZSTENCIL_CLEAR_VALUE */
    float    aa_sx, aa_sy;              /* anti-aliasing scale of the surface */
    float    composite[16];
    float    vp_offset[4];
    uint32_t xform_mode;                /* SET_TRANSFORM_EXECUTION_MODE */
    int      composite_set;
    uint32_t batches_ffp;
} s_gpu;

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
#define NV_TEX_FIRST 0x1B00
#define NV_TEX_LAST  0x1BFC
static uint32_t s_tex_reg[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];
static uint8_t  s_tex_set[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];

/* Formats whose dimensions come from the format word and whose coordinates
 * arrive normalised, rather than from a pitch and SET_TEXTURE_IMAGE_RECT with
 * coordinates in texels. Swizzled and block-compressed are both in this group,
 * and every place that used to test only for swizzled needs the pair. */
static int tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

static void record_tex_reg(uint32_t method, uint32_t param)
{
    s_tex_reg[(method - NV_TEX_FIRST) / 4] = param;
    s_tex_set[(method - NV_TEX_FIRST) / 4] = 1;
    /* A pitch is a linear texture's property. A swizzled one has no rows and
     * so no pitch, and requiring one here refused every swizzled texture --
     * which is nearly all of them, since swizzled is the Xbox default. That
     * left the title's own textures unsampled and every textured quad drawn in
     * flat vertex colour. */
    s_gpu.tex.valid = s_gpu.tex.offset && s_gpu.tex.width && s_gpu.tex.height
                   && (tex_size_from_format(s_gpu.tex.color)
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

static void note_texture_use(void)
{
    int i;

    if (!s_gpu.tex.valid)
        return;
    for (i = 0; i < s_tex_use_count; i++) {
        if (s_tex_use[i].offset == s_gpu.tex.offset
         && s_tex_use[i].color  == s_gpu.tex.color) {
            s_tex_use[i].batches++;
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

/* The last value written to every Kelvin method, as the GPU's register file
 * would hold it. State the executor does not model explicitly (lights,
 * combiners, material colours) is read from here. */
static uint32_t s_reg[0x2000 / 4];
static float reg_f(uint32_t method) { float f; memcpy(&f, &s_reg[method / 4], 4); return f; }

static uint32_t s_sem_va;
void nv2a_pb_set_semaphore_target(uint32_t guest_va) { s_sem_va = guest_va; }

/* Slot + 1 of each method in s_unhandled: this runs millions of times a
 * second, so no search. The report sorts s_unhandled and rebuilds it. */
static uint16_t s_unhandled_slot[0x2000 / 4];

static void note_unhandled(uint32_t method, uint32_t param)
{
    uint16_t *slot = &s_unhandled_slot[(method & 0x1FFC) / 4];

    s_gpu.unhandled_total++;
    if (*slot) {
        s_unhandled[*slot - 1].count++;
        s_unhandled[*slot - 1].last_param = param;
        return;
    }
    if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
        s_unhandled[s_unhandled_count].method = method;
        s_unhandled[s_unhandled_count].count = 1;
        s_unhandled[s_unhandled_count].last_param = param;
        s_unhandled_count++;
        *slot = (uint16_t)s_unhandled_count;
    }
}

/* Read attribute `a` of vertex `index` as floats. Only the float and the
 * normalised-byte types appear in practice; anything else returns 0 so a
 * caller sees a degenerate vertex rather than reading past the array. */
static int fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->size || !a->stride)
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

    switch (a->type) {
    case 0:                                  /* D3DCOLOR */
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A so
         * callers need not know which format the title chose. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 2:                                  /* float */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    case 1:                                  /* S1: short, normalised */
        for (i = 0; i < a->size && i < 4; i++) {
            float f = (float)((const int16_t *)p)[i] / 32767.0f;
            out[i] = f < -1.0f ? -1.0f : f;
        }
        return 1;
    case 5:                                  /* S32K: short, as-is */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i];
        return 1;
    case 6: {                                /* CMP: packed 11:11:10 normal */
        int32_t v;
        memcpy(&v, p, 4);
        out[0] = (float)((int32_t)((uint32_t)v << 21) >> 21) / 1023.0f;
        out[1] = (float)((int32_t)((uint32_t)v << 10) >> 21) / 1023.0f;
        out[2] = (float)(v >> 22) / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

/* NV097 fixed-function transform methods (not in nv2a_regs.h's NV097 list). */
#define NV097_SET_COMPOSITE_MATRIX_FIRST 0x0680
#define NV097_SET_COMPOSITE_MATRIX_LAST  0x06BC
#define NV097_SET_VIEWPORT_OFFSET_FIRST  0x0A20
#define NV097_SET_VIEWPORT_OFFSET_LAST   0x0A2C
#define NV097_SET_TRANSFORM_EXEC_MODE    0x1E94
#define NV_XFORM_MODE_PROGRAM            2   /* low two bits; 0 = fixed */

/* The surface rectangle in real pixels.
 *
 * SET_SURFACE_CLIP_* is in logical pixels; with anti-aliasing on, the surface
 * is larger by the AA factor in SET_SURFACE_FORMAT bits 12-15 (0 = 1x1,
 * 1 = 2x1 "center corner 2", 2 = 2x2 "square offset 4"), and so are the
 * coordinates D3D's composite matrix produces. X-Men Legends renders its
 * 640x480 menu into a 1280x960 surface (pitch 5120), which read as an
 * 8-bytes-per-pixel surface and drew nothing until this was applied. */
static void surface_apply_clip(void)
{
    uint32_t aa = (s_gpu.format >> 12) & 0xF;
    uint32_t sx = aa ? 2 : 1, sy = aa == 2 ? 2 : 1;

    s_gpu.aa_sx = (float)sx;
    s_gpu.aa_sy = (float)sy;

    s_gpu.clip_x = (s_gpu.clip_raw_h & 0xFFFF) * sx;
    s_gpu.clip_w = ((s_gpu.clip_raw_h >> 16) & 0xFFFF) * sx;
    s_gpu.clip_y = (s_gpu.clip_raw_v & 0xFFFF) * sy;
    s_gpu.clip_h = ((s_gpu.clip_raw_v >> 16) & 0xFFFF) * sy;
}

/* Vertex programs (SET_TRANSFORM_EXECUTION_MODE = program).
 *
 * A title that runs a vertex program hands over object-space attributes; the
 * screen position exists only after the program runs. Drawing attribute 0 as
 * if it were already transformed is what turned every 3D mesh into spikes. So
 * the program is interpreted here, on the CPU, per vertex -- the executor
 * already works one vertex at a time, and the back ends take screen-space
 * vertices.
 *
 * Microcode layout (128 bits per instruction, dwords 1-3 used) as documented
 * by xemu's vsh.c. MAC and ILU read their sources before either writes, as
 * the hardware issues them in parallel. The tail the XDK appends to every
 * program (viewport scale and perspective divide) leaves oPos in screen
 * space with w still the clip w.
 *
 * RECOMP_VP=0 turns it off (program batches are then skipped as untransformed).
 *
 * ponytail: no near-plane clipping -- a triangle with a vertex at w <= 0 is
 * dropped. Clip in clip space when cut-off geometry near the camera shows. */
#define NV097_SET_TRANSFORM_PROGRAM        0x0B00   /* 32 dwords */
#define NV097_SET_TRANSFORM_CONSTANT       0x0B80   /* 32 dwords */
#define NV097_SET_TRANSFORM_PROGRAM_LOAD   0x1E9C
#define NV097_SET_TRANSFORM_PROGRAM_START  0x1EA0
#define NV097_SET_TRANSFORM_CONSTANT_LOAD  0x1EA4
#define VP_SLOTS  136
#define VP_CONSTS 192

static struct {
    uint32_t prog[VP_SLOTS][4];
    float    c[VP_CONSTS][4];
    uint32_t prog_load, prog_start, const_load;
    int      off;                       /* RECOMP_VP=0 */
    uint32_t gen;                       /* per-batch cache stamp */
} s_vp;

typedef struct { float pos[4], d0[4], t0[4]; int ok; } VpOut;

/* Per-batch results by vertex index: a strip or an indexed mesh names most
 * vertices several times, and the program is the expensive part. */
#define VP_CACHE 65536
static uint32_t s_vp_stamp[VP_CACHE];
static VpOut    s_vp_cache[VP_CACHE];

static int vp_method(uint32_t method, uint32_t param)
{
    uint32_t slot;

    if (method >= NV097_SET_TRANSFORM_PROGRAM && method < NV097_SET_TRANSFORM_PROGRAM + 0x80) {
        slot = (method - NV097_SET_TRANSFORM_PROGRAM) / 4;
        if (s_vp.prog_load < VP_SLOTS)
            s_vp.prog[s_vp.prog_load][slot % 4] = param;
        if (slot % 4 == 3)
            s_vp.prog_load++;
        s_vp.gen++;
        return 1;
    }
    if (method >= NV097_SET_TRANSFORM_CONSTANT && method < NV097_SET_TRANSFORM_CONSTANT + 0x80) {
        slot = (method - NV097_SET_TRANSFORM_CONSTANT) / 4;
        if (s_vp.const_load < VP_CONSTS)
            memcpy(&s_vp.c[s_vp.const_load][slot % 4], &param, 4);
        if (slot % 4 == 3)
            s_vp.const_load++;
        s_vp.gen++;
        return 1;
    }
    switch (method) {
    case NV097_SET_TRANSFORM_PROGRAM_LOAD:  s_vp.prog_load = param;  return 1;
    case NV097_SET_TRANSFORM_PROGRAM_START: s_vp.prog_start = param; s_vp.gen++; return 1;
    case NV097_SET_TRANSFORM_CONSTANT_LOAD: s_vp.const_load = param; return 1;
    }
    return 0;
}

static int batch_is_vp(void)
{
    static int init;
    if (!init) {
        const char *e = getenv("RECOMP_VP");
        s_vp.off = e && *e == '0';
        init = 1;
    }
    return !s_vp.off && (s_gpu.xform_mode & 3) == NV_XFORM_MODE_PROGRAM;
}

static uint32_t vpf(const uint32_t *t, int dw, int pos, int bits)
{
    return (t[dw] >> pos) & ((1u << bits) - 1u);
}

/* Source operand A (0), B (1) or C (2), swizzled and negated. */
static void vp_src(const uint32_t *t, int which, float r[13][4],
                   float v[16][4], int a0, float out[4])
{
    static const float zero[4] = {0};
    uint32_t mux, reg, neg, sw;
    const float *s = zero;

    switch (which) {
    case 0:  mux = vpf(t, 2, 26, 2); reg = vpf(t, 2, 28, 4);
             neg = vpf(t, 1, 8, 1);  sw = vpf(t, 1, 0, 8);  break;
    case 1:  mux = vpf(t, 2, 11, 2); reg = vpf(t, 2, 13, 4);
             neg = vpf(t, 2, 25, 1); sw = vpf(t, 2, 17, 8); break;
    default: mux = vpf(t, 3, 28, 2);
             reg = (vpf(t, 2, 0, 2) << 2) | vpf(t, 3, 30, 2);
             neg = vpf(t, 2, 10, 1); sw = vpf(t, 2, 2, 8);  break;
    }
    if (mux == 1) {
        if (reg < 13) s = r[reg];
    } else if (mux == 2) {
        s = v[vpf(t, 1, 9, 4)];
    } else if (mux == 3) {
        int ci = (int)vpf(t, 1, 13, 8) + (vpf(t, 3, 1, 1) ? a0 : 0);
        if (ci >= 0 && ci < VP_CONSTS) s = s_vp.c[ci];
    }
    out[0] = s[(sw >> 6) & 3];
    out[1] = s[(sw >> 4) & 3];
    out[2] = s[(sw >> 2) & 3];
    out[3] = s[sw & 3];
    if (neg) {
        out[0] = -out[0]; out[1] = -out[1]; out[2] = -out[2]; out[3] = -out[3];
    }
}

/* Mask bit 3 is x, bit 0 is w. */
static void vp_write(float *dst, uint32_t mask, const float val[4])
{
    int i;
    for (i = 0; i < 4; i++)
        if (mask & (8u >> i))
            dst[i] = val[i];
}

static void vp_splat(float d[4], float x) { d[0] = d[1] = d[2] = d[3] = x; }

static void vp_run(float v[16][4], VpOut *o)
{
    float r[13][4], out[13][4];       /* r[12] is oPos */
    int a0 = 0;
    uint32_t pc;

    memset(r, 0, sizeof r);
    memset(out, 0, sizeof out);
    out[3][3] = out[9][3] = 1.0f;

    for (pc = s_vp.prog_start; pc < VP_SLOTS; pc++) {
        const uint32_t *t = s_vp.prog[pc];
        uint32_t mac = vpf(t, 1, 21, 4), ilu = vpf(t, 1, 25, 3);
        uint32_t omask = vpf(t, 3, 12, 4);
        float A[4], B[4], C[4], m[4] = {0}, l[4] = {0}, x;
        int i;

        vp_src(t, 0, r, v, a0, A);
        vp_src(t, 1, r, v, a0, B);
        vp_src(t, 2, r, v, a0, C);

        switch (mac) {
        case 1: memcpy(m, A, sizeof m); break;                            /* MOV */
        case 2: for (i = 0; i < 4; i++) m[i] = A[i] * B[i]; break;        /* MUL */
        case 3: for (i = 0; i < 4; i++) m[i] = A[i] + C[i]; break;        /* ADD */
        case 4: for (i = 0; i < 4; i++) m[i] = A[i] * B[i] + C[i]; break; /* MAD */
        case 5: vp_splat(m, A[0]*B[0] + A[1]*B[1] + A[2]*B[2]); break;    /* DP3 */
        case 6: vp_splat(m, A[0]*B[0] + A[1]*B[1] + A[2]*B[2] + B[3]); break; /* DPH */
        case 7: vp_splat(m, A[0]*B[0] + A[1]*B[1] + A[2]*B[2] + A[3]*B[3]); break; /* DP4 */
        case 8: m[0] = 1.0f; m[1] = A[1] * B[1]; m[2] = A[2]; m[3] = B[3]; break; /* DST */
        case 9: for (i = 0; i < 4; i++) m[i] = A[i] < B[i] ? A[i] : B[i]; break;  /* MIN */
        case 10: for (i = 0; i < 4; i++) m[i] = A[i] > B[i] ? A[i] : B[i]; break; /* MAX */
        case 11: for (i = 0; i < 4; i++) m[i] = A[i] < B[i] ? 1.0f : 0.0f; break; /* SLT */
        case 12: for (i = 0; i < 4; i++) m[i] = A[i] >= B[i] ? 1.0f : 0.0f; break;/* SGE */
        }

        x = C[0];
        switch (ilu) {
        case 1: memcpy(l, C, sizeof l); break;                            /* MOV */
        case 2: vp_splat(l, x != 0.0f ? 1.0f / x : 1.884467e+19f); break; /* RCP */
        case 3: {                                                         /* RCC */
            float y = x != 0.0f ? 1.0f / x : 1.884467e+19f;
            float a = fabsf(y);
            if (a < 5.42101e-20f) a = 5.42101e-20f;
            if (a > 1.884467e+19f) a = 1.884467e+19f;
            vp_splat(l, y < 0.0f ? -a : a);
            break;
        }
        case 4: vp_splat(l, x != 0.0f ? 1.0f / sqrtf(fabsf(x)) : 1.884467e+19f); break; /* RSQ */
        case 5: {                                                         /* EXP */
            float f = floorf(x);
            l[0] = exp2f(f); l[1] = x - f; l[2] = exp2f(x); l[3] = 1.0f;
            break;
        }
        case 6: {                                                         /* LOG */
            float a = fabsf(x);
            if (a == 0.0f) {
                l[0] = l[2] = -1.884467e+19f; l[1] = 1.0f;
            } else {
                float e = floorf(log2f(a));
                l[0] = e; l[1] = a / exp2f(e); l[2] = log2f(a);
            }
            l[3] = 1.0f;
            break;
        }
        case 7: {                                                         /* LIT */
            float lx = C[0] > 0.0f ? C[0] : 0.0f, ly = C[1] > 0.0f ? C[1] : 0.0f;
            float w = C[3];
            if (w < -127.996f) w = -127.996f;
            if (w > 127.996f) w = 127.996f;
            l[0] = 1.0f; l[1] = lx;
            l[2] = (lx > 0.0f && ly > 0.0f) ? exp2f(w * log2f(ly)) : 0.0f;
            l[3] = 1.0f;
            break;
        }
        }

        if (mac == 13)                                                    /* ARL */
            a0 = (int)floorf(A[0] + 0.001f);
        else if (mac && vpf(t, 3, 20, 4) < 13)
            vp_write(r[vpf(t, 3, 20, 4)], vpf(t, 3, 24, 4), m);
        if (ilu) {
            uint32_t rd = mac ? 1 : vpf(t, 3, 20, 4);     /* paired ILU -> R1 */
            if (rd < 13)
                vp_write(r[rd], vpf(t, 3, 16, 4), l);
        }
        if (omask) {
            const float *src = vpf(t, 3, 2, 1) ? l : m;
            uint32_t addr = vpf(t, 3, 3, 8);
            if (vpf(t, 3, 11, 1)) {                       /* output register */
                if (addr == 0)
                    vp_write(r[12], omask, src);
                else if (addr < 13)
                    vp_write(out[addr], omask, src);
            } else {                                      /* constant write */
                int ci = (int)addr + (vpf(t, 3, 1, 1) ? a0 : 0);
                if (ci >= 0 && ci < VP_CONSTS)
                    vp_write(s_vp.c[ci], omask, src);
            }
        }
        if (vpf(t, 3, 0, 1))                                              /* FINAL */
            break;
    }
    memcpy(o->pos, r[12], sizeof o->pos);
    memcpy(o->d0, out[3], sizeof o->d0);
    memcpy(o->t0, out[9], sizeof o->t0);
    o->ok = o->pos[3] > 1e-6f && isfinite(o->pos[0]) && isfinite(o->pos[1])
         && isfinite(o->pos[3]);
}

static const VpOut *vp_vertex(uint32_t index)
{
    static VpOut uncached;
    float v[16][4];
    VpOut *o;
    uint32_t a;

    if (index < VP_CACHE && s_vp_stamp[index] == s_vp.gen)
        return &s_vp_cache[index];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        fetch_attr(&s_gpu.attr[a], index, v[a]);
    o = index < VP_CACHE ? &s_vp_cache[index] : &uncached;
    vp_run(v, o);
    if (index < VP_CACHE)
        s_vp_stamp[index] = s_vp.gen;
    return o;
}

/* Is this batch transformed by the fixed-function unit? */
static int batch_is_ffp(void)
{
    return s_gpu.composite_set
        && (s_gpu.xform_mode & 3) != NV_XFORM_MODE_PROGRAM
        && !s_gpu.inline_active;
}

/* Attribute 0 as a screen position.
 *
 * With the fixed-function unit active, attribute 0 is an object-space position
 * and the hardware computes  clip[i] = sum_j M[4i+j] * pos[j]  with the 16
 * floats in the order the methods deliver them (translation in M[3], M[7],
 * M[11]; X-Men Legends' menu matrix ends in the row 0 0 0 1), then
 * screen = clip.xyz / clip.w + viewport offset. The composite matrix contains
 * the viewport scale for the *logical* surface; with anti-aliasing the result
 * is scaled up to the real surface (as xemu does). Otherwise attribute 0 is returned as-is
 * (pre-transformed batches, and vertex programs, which are not run here).
 *
 * ponytail: no clipping. A triangle crossing w = 0 is dropped rather than
 * clipped; title-screen quads never do. Add near-plane clipping with 3D scenes. */
static int fetch_position(uint32_t index, float out[4])
{
    float in[4];
    const float *m = s_gpu.composite;
    float w;
    int i;

    if (batch_is_vp()) {
        const VpOut *o = vp_vertex(index);
        if (!o->ok)
            return 0;
        out[0] = o->pos[0] * s_gpu.aa_sx;
        out[1] = o->pos[1] * s_gpu.aa_sy;
        out[2] = o->pos[2];
        out[3] = 1.0f / o->pos[3];
        return 1;
    }
    if (!fetch_attr(&s_gpu.attr[0], index, in))
        return 0;
    if (!batch_is_ffp()) {
        memcpy(out, in, sizeof in);
        return 1;
    }
    for (i = 0; i < 4; i++)
        out[i] = m[4 * i] * in[0] + m[4 * i + 1] * in[1]
               + m[4 * i + 2] * in[2] + m[4 * i + 3];
    /* A vertex behind the camera (w < 0) is kept: the back end clips in
     * homogeneous space, and rejecting it dropped every batch that reached
     * behind the eye -- X-Men Legends' street and sidewalk meshes, which
     * extend past a top-down camera, never drew. Only w = 0 has no image. */
    w = out[3];
    if (fabsf(w) <= 1e-6f)
        return 0;
    out[0] = (out[0] / w + s_gpu.vp_offset[0]) * s_gpu.aa_sx;
    out[1] = (out[1] / w + s_gpu.vp_offset[1]) * s_gpu.aa_sy;
    out[2] = out[2] / w + s_gpu.vp_offset[2];
    out[3] = 1.0f / w;
    return 1;
}

static uint32_t surface_bpp(void)
{
    /* The pitch and the clip width together give the pixel size, which is more
     * reliable than decoding the format field: the format's colour code is
     * only meaningful alongside a type the title also sets, while the pitch is
     * always exactly how many bytes a row occupies. */
    if (!s_gpu.clip_w)
        return 0;
    return s_gpu.pitch / s_gpu.clip_w;
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
    const char *prefix = getenv("RECOMP_FB_DUMP");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    static int seq;
    char path[512];
    uint32_t w = s_gpu.clip_w, h = s_gpu.clip_h, y, x;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;

    if (!prefix || !w || !h || (bpp != 2 && bpp != 4) || !s_gpu.color_offset)
        return;

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%03d.bmp", prefix, seq++);
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
    if (seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X %ubpp)\n",
                path, w, h, s_gpu.color_offset, bpp);
}

/* Defined below, next to the rest of the rasteriser; the clear path uses it
 * for RECOMP_RASTER_TEST. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2]);

/* ── Render back end (nv2a_backend.h) ───────────────────────── */

static const Nv2aBackend *s_backend;

void nv2a_backend_register(const Nv2aBackend *backend)
{
    s_backend = backend;
}

static void current_surface(Nv2aSurface *out)
{
    out->color_va        = dma_resolve(s_gpu.color_offset);
    out->width           = s_gpu.clip_w;
    out->height          = s_gpu.clip_h;
    out->pitch           = s_gpu.pitch;
    out->bytes_per_pixel = surface_bpp();
    out->aa_sx           = s_gpu.aa_sx > 1.5f ? 2 : 1;
    out->aa_sy           = s_gpu.aa_sy > 1.5f ? 2 : 1;
}

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb);

int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out)
{
    Texture saved = s_gpu.tex;
    uint32_t x, y;
    int ok = 1;

    s_gpu.tex.offset = tex->offset;
    s_gpu.tex.width  = tex->width;
    s_gpu.tex.height = tex->height;
    s_gpu.tex.pitch  = tex->pitch;
    s_gpu.tex.color  = tex->color;
    s_gpu.tex.addr_u = 3;
    s_gpu.tex.addr_v = 3;
    s_gpu.tex.valid  = 1;
    for (y = 0; y < tex->height && ok; y++)
        for (x = 0; x < tex->width; x++)
            if (!sample_texture(x, y, &argb_out[(size_t)y * tex->width + x])) {
                ok = 0;
                break;
            }
    s_gpu.tex = saved;
    return ok;
}

static void clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x;

    /* A back end clears colour and/or depth/stencil itself (flags are the
     * CLEAR_SURFACE bits: Z 0x1, stencil 0x2, colour 0xF0). */
    if (s_backend && s_backend->clear) {
        Nv2aSurface surf;
        if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
            return;
        current_surface(&surf);
        s_backend->clear(&surf, &s_gpu.rs, param, s_gpu.clear_color,
                         s_gpu.zstencil_clear);
        s_gpu.clears++;
        return;
    }
    if (!(param & NV097_CLEAR_COLOR_MASK))
        return;                            /* depth/stencil only */
    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
        return;
    {
        uint32_t base = dma_resolve(s_gpu.color_offset);
        if (surface_write_refused(base,
                                  (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch,
                                  "clear"))
            return;
        s_gpu.color_base = base;
    }

    for (y = 0; y < s_gpu.clip_h; y++) {
        uint8_t *row = mem + s_gpu.color_base
                     + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        if (bpp == 4) {
            uint32_t *p = (uint32_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = s_gpu.clear_color;
        } else if (bpp == 2) {
            /* The clear value is always given as A8R8G8B8; a 16-bit surface
             * takes the same colour reduced to 5:6:5. */
            uint16_t v = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                  | ((s_gpu.clear_color >> 5) & 0x07E0)
                                  | ((s_gpu.clear_color >> 3) & 0x001F));
            uint16_t *p = (uint16_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = v;
        }
    }
    s_gpu.clears++;
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
    if (getenv("RECOMP_RASTER_TEST")) {
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
    if (!s_gpu.flip_seen)
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
 * ponytail: nearest texel, no filtering, whatever SET_TEXTURE_FILTER asked
 * for. Bilinear when a title's output actually depends on it.
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

static uint32_t expand(uint32_t v, uint32_t bits)
{
    return d3d8_expand_channel(v, bits);
}

/* The linear format that decodes the same texels as a swizzled one.
 *
 * Swizzling changes where a texel lives, not what it says: A8R8G8B8 (0x06) and
 * LIN_A8R8G8B8 (0x12) are the same four bytes in the same order. So the whole
 * difference is the address calculation, and one of those lets every format
 * below serve both. Pairs read off the table in d3d8_xbox.h rather than
 * recalled -- the comment above this one is about getting exactly that wrong. */
static uint32_t linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;                /* L8        -> LIN_L8        */
    case 0x02: return 0x10;                /* A1R5G5B5  -> LIN_A1R5G5B5  */
    case 0x03: return 0x1C;                /* X1R5G5B5  -> LIN_X1R5G5B5  */
    case 0x04: return 0x1D;                /* A4R4G4B4  -> LIN_A4R4G4B4  */
    case 0x05: return 0x11;                /* R5G6B5    -> LIN_R5G6B5    */
    case 0x06: return 0x12;                /* A8R8G8B8  -> LIN_A8R8G8B8  */
    case 0x07: return 0x1E;                /* X8R8G8B8  -> LIN_X8R8G8B8  */
    case 0x19: return 0x1F;                /* A8        -> LIN_A8        */
    default:   return fmt;                 /* already linear, or unhandled */
    }
}

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t fmt;

    if (!s_gpu.tex.valid)
        return 0;
    u = wrap_coord(u, s_gpu.tex.width,  s_gpu.tex.addr_u);
    v = wrap_coord(v, s_gpu.tex.height, s_gpu.tex.addr_v);

    fmt = s_gpu.tex.color;
    if (d3d8_format_dxt_block_bytes(fmt))
        return d3d8_dxt_decode_texel(mem + s_gpu.tex.offset, fmt, u, v,
                                     s_gpu.tex.width, argb);
    if (d3d8_format_is_swizzled(fmt)) {
        /* Morton order: a texel's index is interleaved from x and y instead of
         * v*pitch + u, so index from the base of the image. The switch below
         * casts to each format's own width, which makes that index a texel
         * index for every one of them. */
        fmt = linear_twin(fmt);
        p = mem + s_gpu.tex.offset;
        u = swizzle_offset(u, v, s_gpu.tex.width, s_gpu.tex.height);
    } else {
        p = mem + s_gpu.tex.offset + (size_t)v * s_gpu.tex.pitch;
    }

    switch (fmt) {

    /* 32-bit, alpha-red-green-blue in the dword. */
    case 0x12:                                      /* LIN_A8R8G8B8 */
        *argb = ((const uint32_t *)p)[u];
        return 1;
    case 0x1E:                                      /* LIN_X8R8G8B8 */
        *argb = ((const uint32_t *)p)[u] | 0xFF000000u;
        return 1;

    /* 32-bit, other channel orders. The name gives the byte order from the
     * top of the dword down, so each is a permutation of the same four. */
    case 0x3F: {                                    /* LIN_A8B8G8R8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        return 1;
    }
    case 0x40: {                                    /* LIN_B8G8R8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24)                 /* A, from the bottom */
              | (((t >>  8) & 0xFFu) << 16)         /* R */
              | (((t >> 16) & 0xFFu) <<  8)         /* G */
              |  ((t >> 24) & 0xFFu);               /* B, from the top */
        return 1;
    }
    case 0x41: {                                    /* LIN_R8G8B8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        return 1;
    }

    /* 16-bit. */
    case 0x10: {                                    /* LIN_A1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1C: {                                    /* LIN_X1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x11: {                                    /* LIN_R5G6B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 11) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x3F, 6) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1D: {                                    /* LIN_A4R4G4B4 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = (expand((t >> 12) & 0x0F, 4) << 24)
              | (expand((t >>  8) & 0x0F, 4) << 16)
              | (expand((t >>  4) & 0x0F, 4) <<  8)
              |  expand( t        & 0x0F, 4);
        return 1;
    }

    /* 8-bit. */
    case 0x13: {                                    /* LIN_L8 */
        uint32_t t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        return 1;
    }
    case 0x1F:                                      /* LIN_A8 */
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        return 1;

    default:
        return 0;
    }
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
    const char *prefix = getenv("RECOMP_TEX_DUMP");
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

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    uint8_t *row;

    (void)mem;
    if (x < (int)s_gpu.clip_x || x >= (int)(s_gpu.clip_x + s_gpu.clip_w))
        return;
    if (y < (int)s_gpu.clip_y || y >= (int)(s_gpu.clip_y + s_gpu.clip_h))
        return;
    s_gpu.pixels++;
    if ((argb & 0x00FFFFFFu) > (s_gpu.pixel_max & 0x00FFFFFFu))
        s_gpu.pixel_max = argb;
    row = s_surface + (size_t)y * s_gpu.pitch;
    if (bpp == 4) {
        ((uint32_t *)row)[x] = argb;
    } else if (bpp == 2) {
        ((uint16_t *)row)[x] = (uint16_t)(((argb >> 8) & 0xF800)
                                        | ((argb >> 5) & 0x07E0)
                                        | ((argb >> 3) & 0x001F));
    }
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
    float area;
    int minx, maxx, miny, maxy, x, y;
    int textured = uv && s_gpu.tex.valid;

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f)
        return;                            /* degenerate */

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
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
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
                if (su < 0.0f) su = 0.0f;
                if (sv < 0.0f) sv = 0.0f;
                if (sample_texture((uint32_t)su, (uint32_t)sv, &texel)) {
                    put_pixel(mem, bpp, x, y, texel);
                    continue;
                }
            }
            put_pixel(mem, bpp, x, y, argb);
        }
    }
    s_gpu.tris_drawn++;
    s_gpu.drawn_offset = s_gpu.color_offset;
    s_gpu.drawn_pitch = s_gpu.pitch;
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

    if (batch_is_vp())
        memcpy(t, vp_vertex(index)->t0, sizeof t);
    else if (!fetch_attr(texcoord_attr(), index, t))
        return 0;
    out[0] = t[0];
    out[1] = t[1];
    if (tex_size_from_format(s_gpu.tex.color)) {
        out[0] *= (float)s_gpu.tex.width;
        out[1] *= (float)s_gpu.tex.height;
    }
    return 1;
}

/* Fixed-function vertex lighting (SET_LIGHTING_ENABLE).
 *
 * With lighting on, the diffuse colour the combiners see is computed per
 * vertex from the normal and up to eight lights; the vertex's own colour
 * stream is ignored unless SET_COLOR_MATERIAL routes it in. Using the colour
 * stream anyway is what drew every lit mesh -- characters, streets -- black.
 *
 * XDK D3D pre-multiplies material into the light colours and folds
 * emission + material ambient * global ambient into SCENE_AMBIENT_COLOR, so
 * the hardware sum is simply
 *   colour = scene_ambient + sum_i atten_i * (ambient_i + diffuse_i * max(0, N.L_i))
 * with alpha from SET_MATERIAL_ALPHA. Everything is in eye space.
 *
 * ponytail: normals go through the model-view matrix itself rather than the
 * inverse transpose (exact for rotations and uniform scale), no specular, no
 * spot cones, no back-face colours, no skinning. Add them when a mesh looks
 * wrong rather than dark. */
#define NV_LIGHTS 8
static int lit_color(uint32_t index, float out[4])
{
    const float *mv = (const float *)&s_reg[0x0480 / 4];
    float pos[4], nrm[4], vc[4], e[3], n[3], len;
    uint32_t mask = s_reg[0x03BC / 4], colmat = s_reg[0x0298 / 4];
    int i, have_vc;

    if (!s_reg[0x0314 / 4] || !s_gpu.attr[2].size)
        return 0;
    if (!fetch_attr(&s_gpu.attr[0], index, pos) || !fetch_attr(&s_gpu.attr[2], index, nrm))
        return 0;
    have_vc = fetch_attr(color_attr(), index, vc);

    for (i = 0; i < 3; i++) {
        e[i] = mv[4 * i] * pos[0] + mv[4 * i + 1] * pos[1] + mv[4 * i + 2] * pos[2] + mv[4 * i + 3];
        n[i] = mv[4 * i] * nrm[0] + mv[4 * i + 1] * nrm[1] + mv[4 * i + 2] * nrm[2];
    }
    len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }
    /* Light the side the viewer sees. Some meshes carry normals that face
     * away from the camera (X-Men Legends' streets and sidewalks: authentic
     * file data, drawn with culling off), and lighting them as stored left
     * only the light's ambient term -- black ground. Flipping a normal that
     * points away from the eye is two-sided lighting with the back material
     * equal to the front one.
     * ponytail: per vertex, not per face; exact for flat ground, a vertex on
     * a silhouette may pick the other side. */
    if (n[0] * e[0] + n[1] * e[1] + n[2] * e[2] > 0.0f) {
        n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
    }

    out[0] = reg_f(0x0A10); out[1] = reg_f(0x0A14); out[2] = reg_f(0x0A18);
    for (i = 0; i < NV_LIGHTS; i++) {
        uint32_t kind = (mask >> (2 * i)) & 3, b = 0x1000 + (uint32_t)i * 0x80;
        float l[3], att = 1.0f, ndl, amb[3], dif[3];
        int k;

        if (!kind)
            continue;
        if (kind == 1) {                                    /* infinite */
            l[0] = reg_f(b + 0x34); l[1] = reg_f(b + 0x38); l[2] = reg_f(b + 0x3C);
        } else {                                            /* local / spot */
            float d;
            l[0] = reg_f(b + 0x5C) - e[0]; l[1] = reg_f(b + 0x60) - e[1]; l[2] = reg_f(b + 0x64) - e[2];
            d = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
            if (d > reg_f(b + 0x24) && reg_f(b + 0x24) > 0.0f)
                continue;                                   /* out of range */
            if (d > 1e-8f) { l[0] /= d; l[1] /= d; l[2] /= d; }
            att = reg_f(b + 0x68) + reg_f(b + 0x6C) * d + reg_f(b + 0x70) * d * d;
            att = att > 1e-8f ? 1.0f / att : 1.0f;
        }
        ndl = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
        if (ndl < 0.0f) ndl = 0.0f;
        for (k = 0; k < 3; k++) {
            amb[k] = reg_f(b + 0x00 + 4 * k);
            dif[k] = reg_f(b + 0x0C + 4 * k);
            /* COLOR_MATERIAL: 1 in the ambient (bits 2-3) or diffuse (bits
             * 4-5) field takes that material colour from the vertex. */
            if (have_vc && ((colmat >> 2) & 3) == 1) amb[k] *= vc[k];
            if (have_vc && ((colmat >> 4) & 3) == 1) dif[k] *= vc[k];
            out[k] += att * (amb[k] + dif[k] * ndl);
        }
    }
    out[3] = (have_vc && ((colmat >> 4) & 3) == 1) ? vc[3] : reg_f(0x03B4);
    for (i = 0; i < 4; i++)
        out[i] = out[i] < 0.0f ? 0.0f : out[i] > 1.0f ? 1.0f : out[i];
    return 1;
}

static uint32_t vertex_color(uint32_t index)
{
    float c[4];
    int i;

    if (batch_is_vp()) {
        memcpy(c, vp_vertex(index)->d0, sizeof c);
        for (i = 0; i < 4; i++)
            c[i] = c[i] < 0.0f ? 0.0f : c[i] > 1.0f ? 1.0f : c[i];
    } else if (!lit_color(index, c) && !fetch_attr(color_attr(), index, c))
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
    if (!fetch_position(s_gpu.idx[0], p))
        return 0;
    lo_x = hi_x = p[0];
    lo_y = hi_y = p[1];
    for (i = 1; i < s_gpu.idx_count; i++) {
        if (!fetch_position(s_gpu.idx[i], p))
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

    /* Small enough to be model units rather than pixels -- unless the
     * fixed-function unit just turned them into pixels. */
    if (!batch_is_ffp() && !batch_is_vp()
     && hi_x - lo_x < OBJECT_SPACE_SPAN && hi_y - lo_y < OBJECT_SPACE_SPAN)
        return 0;

    return 1;
}

/* NV097 primitive types that are triangles under some winding
 * (NV097_SET_BEGIN_END_OP_* in nv2a_regs.h; 0 is END, 1-4 points and lines).
 * These were one lower, which drew every triangle strip as a fan and every
 * triangle list as a strip: 3D meshes smeared into long spikes. */
#define NV_PRIM_TRIANGLES      5
#define NV_PRIM_TRIANGLE_STRIP 6
#define NV_PRIM_TRIANGLE_FAN   7
#define NV_PRIM_QUADS          8
#define NV_PRIM_QUAD_STRIP     9
#define NV_PRIM_POLYGON        10

/* The batch as a triangle list of vertex indices, `emit` called per triangle.
 *
 * Quads are the trap: a QUADS batch is independent quads, four indices each,
 * not one fan around index 0. Drawn as a fan, every quad after the first
 * became a triangle reaching back to the batch's first vertex -- a character
 * mesh or a row of text came out as spikes radiating from one point. */
static void for_each_triangle(void (*emit)(uint32_t, uint32_t, uint32_t, void *),
                              void *ctx)
{
    const uint32_t *x = s_gpu.idx;
    uint32_t i, n = s_gpu.idx_count;

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3)
            emit(x[i], x[i + 1], x[i + 2], ctx);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        /* Alternate the winding so every strip triangle faces the same way. */
        for (i = 0; i + 2 < n; i++) {
            if (i & 1)
                emit(x[i + 1], x[i], x[i + 2], ctx);
            else
                emit(x[i], x[i + 1], x[i + 2], ctx);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:                   /* convex: a fan */
        for (i = 1; i + 1 < n; i++)
            emit(x[0], x[i], x[i + 1], ctx);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            emit(x[i], x[i + 1], x[i + 2], ctx);
            emit(x[i], x[i + 2], x[i + 3], ctx);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            emit(x[i], x[i + 1], x[i + 3], ctx);
            emit(x[i], x[i + 3], x[i + 2], ctx);
        }
        break;
    default:
        break;                              /* points and lines: not yet */
    }
}

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
    int textured;

    if (!fetch_position(i0, p[0])
     || !fetch_position(i1, p[1])
     || !fetch_position(i2, p[2]))
        return;
    if (p[0][3] < 0.0f || p[1][3] < 0.0f || p[2][3] < 0.0f)
        return;                /* behind the eye: the CPU path cannot clip */

    textured = fetch_texcoord(i0, uv[0])
            && fetch_texcoord(i1, uv[1])
            && fetch_texcoord(i2, uv[2]);

    raster_triangle(p[0], p[1], p[2], argb,
                    textured ? (const float (*)[2])uv : NULL);
}

static void raster_tri(uint32_t i0, uint32_t i1, uint32_t i2, void *ctx)
{
    (void)ctx;
    raster_indexed(i0, i1, i2, vertex_color(i0));
}

/* Hand the batch to the registered back end as a triangle list, using the
 * same topology rules as the CPU path below. */
#define NV_BACKEND_MAX_VERTS (NV_MAX_INDICES * 3)
static Nv2aVertex s_bverts[NV_BACKEND_MAX_VERTS];

static int backend_vertex(uint32_t index, Nv2aVertex *out)
{
    float p[4], uv[2];

    if (!fetch_position(index, p))
        return 0;
    out->x = p[0];
    out->y = p[1];
    out->z = p[2];
    out->rhw = p[3];
    out->diffuse = vertex_color(index);
    if (fetch_texcoord(index, uv)) {
        out->u = uv[0];
        out->v = uv[1];
    } else {
        out->u = out->v = 0.0f;
    }
    return 1;
}

/* Each vertex of a batch is transformed and lit once, not once per triangle
 * that uses it: a strip shares each vertex between three triangles, so this
 * roughly thirds the executor's per-vertex work. Direct-mapped on the low 16
 * bits of the index, tagged with the full index and the batch generation.
 * ponytail: indices 64K apart that collide just recompute. */
static struct { uint32_t batches, dark_lit; } s_vit;

#define NV_VCACHE 65536
typedef struct { uint32_t gen, index; int ok; Nv2aVertex v; } VCacheEntry;
static VCacheEntry s_vcache[NV_VCACHE];
static uint32_t s_vcache_gen;

static int backend_vertex_cached(uint32_t index, Nv2aVertex *out)
{
    VCacheEntry *e = &s_vcache[index & (NV_VCACHE - 1)];

    if (e->gen != s_vcache_gen || e->index != index) {
        e->gen = s_vcache_gen;
        e->index = index;
        e->ok = backend_vertex(index, &e->v);
    }
    if (e->ok)
        *out = e->v;
    return e->ok;
}

static void backend_tri(uint32_t i0, uint32_t i1, uint32_t i2, void *ctx)
{
    uint32_t *n = (uint32_t *)ctx;

    if (*n + 3 > NV_BACKEND_MAX_VERTS)
        return;
    if (backend_vertex_cached(i0, &s_bverts[*n])
     && backend_vertex_cached(i1, &s_bverts[*n + 1])
     && backend_vertex_cached(i2, &s_bverts[*n + 2]))
        *n += 3;
}

static void backend_batch(void)
{
    Nv2aSurface surf;
    Nv2aTexture tex;
    Nv2aBatch batch;
    uint32_t n = 0;

    if (++s_vcache_gen == 0) {               /* wrapped: old tags could match */
        memset(s_vcache, 0, sizeof s_vcache);
        s_vcache_gen = 1;
    }

    for_each_triangle(backend_tri, &n);
    if (!n)
        return;

    current_surface(&surf);
    batch.vertices = s_bverts;
    batch.count = n;
    batch.texture = NULL;
    batch.state = &s_gpu.rs;
    {
        const VertexAttr *tc = texcoord_attr();
        if (s_gpu.tex.valid && tc->offset && tc->stride) {
            tex.offset = s_gpu.tex.offset;
            tex.width  = s_gpu.tex.width;
            tex.height = s_gpu.tex.height;
            tex.pitch  = s_gpu.tex.pitch;
            tex.color  = s_gpu.tex.color;
            tex.addr_u = s_gpu.tex.addr_u;
            tex.addr_v = s_gpu.tex.addr_v;
            batch.texture = &tex;
        }
    }
    s_backend->draw(&surf, &batch);
    s_gpu.tris_drawn += n / 3;
    s_vit.batches++;
    /* A lit batch whose first vertex came out near black: the signature of a
     * lighting bug (normals, light setup) rather than a dark scene, when it
     * climbs suddenly. */
    if (s_reg[0x0314 / 4] && (s_bverts[0].diffuse & 0x00F8F8F8u) == 0)
        s_vit.dark_lit++;
}

/* Renderer totals for a title's vitals monitor: out[] = batches drawn,
 * batches skipped as untransformed, lit batches that came out black,
 * triangles drawn. Monotonic; the caller takes differences. */
void nv2a_pb_exec_get_stats(uint32_t out[4])
{
    out[0] = s_vit.batches;
    out[1] = s_gpu.batches_untransformed;
    out[2] = s_vit.dark_lit;
    out[3] = s_gpu.tris_drawn;
}

static void raster_batch(void)
{
    uint32_t before = s_gpu.tris_drawn;

    s_vp.gen++;
    if (s_gpu.idx_count < 3)
        return;
    if (!batch_is_screen_space()) {
        s_gpu.batches_untransformed++;
        return;
    }
    if (s_backend && s_backend->draw) {
        if (batch_is_ffp())
            s_gpu.batches_ffp++;
        backend_batch();
        s_gpu.drawn_offset = s_gpu.color_offset;
        s_gpu.drawn_pitch = s_gpu.pitch;
        return;
    }
    if (batch_is_ffp()) {
        s_gpu.batches_ffp++;
        /* RECOMP_FFP_TRACE: the matrix and the first vertices of the first few
         * fixed-function batches, raw and transformed -- the one view that
         * tells a wrong matrix layout from wrong vertex data. */
        if (pb_ffp_trace() && s_gpu.batches_ffp <= 4) {
            const float *m = s_gpu.composite;
            float in[4], out[4];
            uint32_t k;
            fprintf(stderr, "  [FFP] batch %u prim %u n %u mode 0x%X vp_off %.2f %.2f %.2f"
                            " clip %ux%u+%u+%u\n"
                            "  [FFP]   M = %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
                    s_gpu.batches_ffp, s_gpu.prim, s_gpu.idx_count, s_gpu.xform_mode,
                    s_gpu.vp_offset[0], s_gpu.vp_offset[1], s_gpu.vp_offset[2],
                    s_gpu.clip_w, s_gpu.clip_h, s_gpu.clip_x, s_gpu.clip_y,
                    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                    m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            for (k = 0; k < s_gpu.idx_count && k < 4; k++) {
                fetch_attr(&s_gpu.attr[0], s_gpu.idx[k], in);
                fetch_position(s_gpu.idx[k], out);
                fprintf(stderr, "  [FFP]   v%u (%g %g %g %g) -> (%g %g %g %g)\n",
                        s_gpu.idx[k], in[0], in[1], in[2], in[3],
                        out[0], out[1], out[2], out[3]);
            }
        }
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

    for_each_triangle(raster_tri, NULL);

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
    if (pb_verbose() && (s_gpu.draws % 200) == 0)
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

    raster_batch();

    if (pb_verbose()) {
        /* RECOMP_TRACE_FLIP=<n>: dump 3000 batches starting at flip n, so a
         * specific screen can be inspected rather than only the boot. The
         * limit covers one full 3D frame, so overlays drawn last show up. */
        static int shown;
        static long from = -1;
        if (from < 0) {
            const char *e = getenv("RECOMP_TRACE_FLIP");
            from = e ? atol(e) : 0;
        }
        if (s_gpu.flips >= (uint32_t)from && shown++ < (from ? 3000 : 6)) {
            fprintf(stderr, "  [GPU] --- batch at flip %u, xform mode %u, tex fmt 0x%08X addr %u/%u\n",
                    s_gpu.flips, s_gpu.xform_mode, s_tex_reg[1], s_gpu.tex.addr_u, s_gpu.tex.addr_v);
            fprintf(stderr, "  [GPU]   light en %u mask 0x%X colmat 0x%X ctl 0x%X ambient %g %g %g"
                            " | comb ctl 0x%X c0 icw 0x%08X ocw 0x%08X a0 icw 0x%08X f0 0x%08X"
                            " fin 0x%08X/0x%08X | tex1 0x%08X ctl0 0x%08X | diffuse0 0x%08X attr2 %u\n",
                    s_reg[0x0314/4], s_reg[0x03BC/4], s_reg[0x0298/4], s_reg[0x0294/4],
                    reg_f(0x0A10), reg_f(0x0A14), reg_f(0x0A18),
                    s_reg[0x1E60/4], s_reg[0x0AC0/4], s_reg[0x1E40/4], s_reg[0x0260/4], s_reg[0x0A60/4],
                    s_reg[0x0288/4], s_reg[0x028C/4], s_reg[0x1B44/4], s_reg[0x1B4C/4],
                    s_gpu.idx_count ? vertex_color(s_gpu.idx[0]) : 0, s_gpu.attr[2].size);
            if (s_reg[0x0314 / 4]) {
                uint32_t li;
                float lc[4] = {0};
                for (li = 0; li < 6; li++) {
                    uint32_t b = 0x1000 + li * 0x80;
                    fprintf(stderr, "  [GPU]   light%u amb %g %g %g dif %g %g %g dir %g %g %g pos %g %g %g att %g %g %g\n",
                            li, reg_f(b), reg_f(b + 4), reg_f(b + 8),
                            reg_f(b + 0xC), reg_f(b + 0x10), reg_f(b + 0x14),
                            reg_f(b + 0x34), reg_f(b + 0x38), reg_f(b + 0x3C),
                            reg_f(b + 0x5C), reg_f(b + 0x60), reg_f(b + 0x64),
                            reg_f(b + 0x68), reg_f(b + 0x6C), reg_f(b + 0x70));
                }
                {
                    const float *mv = (const float *)&s_reg[0x0480 / 4];
                    const float *im = (const float *)&s_reg[0x0580 / 4];
                    float nn[4];
                    fprintf(stderr, "  [GPU]   MV %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
                            mv[0], mv[1], mv[2], mv[3], mv[4], mv[5], mv[6], mv[7],
                            mv[8], mv[9], mv[10], mv[11], mv[12], mv[13], mv[14], mv[15]);
                    fprintf(stderr, "  [GPU]   IMV %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
                            im[0], im[1], im[2], im[3], im[4], im[5], im[6], im[7],
                            im[8], im[9], im[10], im[11], im[12], im[13], im[14], im[15]);
                    if (s_gpu.idx_count && fetch_attr(&s_gpu.attr[2], s_gpu.idx[0], nn)) {
                        float pp[4], ee[3], ne[3];
                        int q;
                        fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], pp);
                        for (q = 0; q < 3; q++) {
                            ee[q] = mv[4*q]*pp[0] + mv[4*q+1]*pp[1] + mv[4*q+2]*pp[2] + mv[4*q+3];
                            ne[q] = mv[4*q]*nn[0] + mv[4*q+1]*nn[1] + mv[4*q+2]*nn[2];
                        }
                        fprintf(stderr, "  [GPU]   normal v0 %g %g %g | eye pos %g %g %g | eye n %g %g %g | n.toEye %g"
                                        " | half0 %g %g %g half1 %g %g %g | twoside %u\n",
                                nn[0], nn[1], nn[2], ee[0], ee[1], ee[2], ne[0], ne[1], ne[2],
                                -(ee[0]*ne[0] + ee[1]*ne[1] + ee[2]*ne[2]),
                                reg_f(0x1028), reg_f(0x102C), reg_f(0x1030),
                                reg_f(0x10A8), reg_f(0x10AC), reg_f(0x10B0), s_reg[0x17C4 / 4]);
                    }
                }
                if (s_gpu.idx_count && lit_color(s_gpu.idx[0], lc))
                    fprintf(stderr, "  [GPU]   lit v0 = %g %g %g %g  mat alpha %g emis %g %g %g\n",
                            lc[0], lc[1], lc[2], lc[3], reg_f(0x03B4),
                            reg_f(0x03A8), reg_f(0x03AC), reg_f(0x03B0));
            }
            fprintf(stderr, "  [GPU]   blend %u %X/%X eq %X | depth %u func %X write %u | atest %u %X ref %u | cull %u\n",
                    s_gpu.rs.blend_enable, s_gpu.rs.blend_src, s_gpu.rs.blend_dst, s_gpu.rs.blend_eq,
                    s_gpu.rs.depth_test_enable, s_gpu.rs.depth_func, s_gpu.rs.depth_write,
                    s_gpu.rs.alpha_test_enable, s_gpu.rs.alpha_func, s_gpu.rs.alpha_ref,
                    s_gpu.rs.cull_enable);
            if (batch_is_vp()) {
                static int prog_shown;
                uint32_t pc;
                if (!prog_shown++) {
                    fprintf(stderr, "  [VP] start %u load %u const_load %u\n",
                            s_vp.prog_start, s_vp.prog_load, s_vp.const_load);
                    for (pc = s_vp.prog_start; pc < VP_SLOTS && pc < s_vp.prog_start + 40; pc++) {
                        fprintf(stderr, "  [VP] %3u: %08X %08X %08X %08X\n", pc,
                                s_vp.prog[pc][0], s_vp.prog[pc][1],
                                s_vp.prog[pc][2], s_vp.prog[pc][3]);
                        if (s_vp.prog[pc][3] & 1)
                            break;
                    }
                    for (pc = 0; pc < VP_CONSTS; pc++)
                        if (s_vp.c[pc][0] != 0.0f || s_vp.c[pc][1] != 0.0f
                         || s_vp.c[pc][2] != 0.0f || s_vp.c[pc][3] != 0.0f)
                            fprintf(stderr, "  [VP] c%-3u %g %g %g %g\n", pc,
                                    s_vp.c[pc][0], s_vp.c[pc][1], s_vp.c[pc][2], s_vp.c[pc][3]);
                }
            }
            {
                uint32_t k;
                float sp[4];
                for (k = 0; k < s_gpu.idx_count && k < 4; k++)
                    if (fetch_position(s_gpu.idx[k], sp))
                        fprintf(stderr, "  [GPU]   screen[%u] = %.1f %.1f z %.1f rhw %g\n",
                                k, sp[0], sp[1], sp[2], sp[3]);
            }
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
        case 1:
        case 5:  bytes = 2 * s_gpu.attr[a].size;   break;  /* short      */
        case 6:  bytes = 4;                        break;  /* packed CMP */
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
        s_gpu.idx[i] = i;
    s_gpu.idx_count = count;

    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

out:
    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
}

/* Draw the vertices SET_VERTEX3F/4F completed.
 *
 * Same trick as draw_inline_array: rather than a second copy of the topology
 * and rasterisation code, describe what was accumulated as an ordinary vertex
 * array and hand it to the existing path. The layout is ours and fixed, so
 * the attribute table is written out here rather than derived from the
 * title's format registers.
 *
 * The title's own table is saved and put back -- it has not stopped using it.
 *
 * ponytail: position, diffuse and texcoord0 only. That is what a 2D quad
 * carries and what this rasteriser samples; a second texcoord set or a normal
 * would need the D3D11 translator, not more slots here.
 */
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
    s_gpu.attr[0].type = 2; s_gpu.attr[0].size = 4;   /* position float4  */
    s_gpu.attr[0].offset = 0;
    s_gpu.attr[3].type = 0; s_gpu.attr[3].size = 4;   /* diffuse D3DCOLOR */
    s_gpu.attr[3].offset = 16;
    s_gpu.attr[9].type = 2; s_gpu.attr[9].size = 2;   /* texcoord0 float2 */
    s_gpu.attr[9].offset = 20;
    s_gpu.attr[0].stride = s_gpu.attr[3].stride = s_gpu.attr[9].stride =
        IMM_VERTEX_DWORDS * 4;

    for (i = 0; i < s_gpu.imm_count && i < NV_MAX_INDICES; i++)
        s_gpu.idx[i] = i;
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

/* A vertex is complete: append it in the layout draw_immediate describes. */
static void imm_emit_vertex(void)
{
    uint32_t at = s_gpu.imm_count * IMM_VERTEX_DWORDS;

    if (!s_gpu.prim || at + IMM_VERTEX_DWORDS > NV_MAX_INLINE)
        return;
    memcpy(&s_gpu.inline_buf[at],     s_gpu.imm_pos, 4 * sizeof(float));
    memcpy(&s_gpu.inline_buf[at + 4], &s_gpu.imm_diffuse, sizeof(uint32_t));
    memcpy(&s_gpu.inline_buf[at + 5], s_gpu.imm_tex, 2 * sizeof(float));
    s_gpu.imm_count++;
}

/* The immediate-mode writes. Returns 1 if `method` was one of them.
 *
 * Split out because it is a range test against five separate bases, and that
 * reads better than five more cases in an already long switch.
 */
static int imm_vertex_method(uint32_t method, uint32_t param)
{
    union { uint32_t u; float f; } v;
    v.u = param;

    if (method >= NV097_SET_VERTEX4F && method < NV097_SET_VERTEX4F + 16) {
        uint32_t c = (method - NV097_SET_VERTEX4F) / 4;
        s_gpu.imm_pos[c] = v.f;
        if (c == 3)                      /* w completes the vertex */
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX3F && method < NV097_SET_VERTEX3F + 12) {
        uint32_t c = (method - NV097_SET_VERTEX3F) / 4;
        s_gpu.imm_pos[c] = v.f;
        if (c == 2) {                    /* z completes it, w is implicitly 1 */
            s_gpu.imm_pos[3] = 1.0f;
            imm_emit_vertex();
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2F_M
            && method < NV097_SET_VERTEX_DATA2F_M + NV_VERTEX_ATTRS * 8) {
        uint32_t off = method - NV097_SET_VERTEX_DATA2F_M;
        if (off / 8 == 9)                /* attribute 9 is texture coord 0 */
            s_gpu.imm_tex[(off % 8) / 4] = v.f;
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M
            && method < NV097_SET_VERTEX_DATA4F_M + NV_VERTEX_ATTRS * 16) {
        uint32_t off = method - NV097_SET_VERTEX_DATA4F_M;
        uint32_t attr = off / 16, c = (off % 16) / 4;
        if (attr == 0) {
            s_gpu.imm_pos[c] = v.f;
            if (c == 3)
                imm_emit_vertex();
        } else if (attr == 9 && c < 2) {
            s_gpu.imm_tex[c] = v.f;
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB
            && method < NV097_SET_VERTEX_DATA4UB + NV_VERTEX_ATTRS * 4) {
        if ((method - NV097_SET_VERTEX_DATA4UB) / 4 == 3)   /* diffuse */
            s_gpu.imm_diffuse = param;
        return 1;
    }
    return 0;
}
/* Render state for a back end (nv2a_backend.h). Returns 1 if consumed. */
static int capture_render_state(uint32_t method, uint32_t param)
{
    Nv2aRenderState *r = &s_gpu.rs;
    float f;

    switch (method) {
    case 0x0300: r->alpha_test_enable = param; return 1; /* SET_ALPHA_TEST_ENABLE */
    case 0x0304: r->blend_enable      = param; return 1; /* SET_BLEND_ENABLE */
    case 0x0308: r->cull_enable       = param; return 1; /* SET_CULL_FACE_ENABLE */
    case 0x030C: r->depth_test_enable = param; return 1; /* SET_DEPTH_TEST_ENABLE */
    case 0x033C: r->alpha_func        = param; return 1; /* SET_ALPHA_FUNC */
    case 0x0340: r->alpha_ref         = param; return 1; /* SET_ALPHA_REF */
    case 0x0344: r->blend_src         = param; return 1; /* SET_BLEND_FUNC_SFACTOR */
    case 0x0348: r->blend_dst         = param; return 1; /* SET_BLEND_FUNC_DFACTOR */
    case 0x034C: r->blend_color       = param; return 1; /* SET_BLEND_COLOR */
    case 0x0350: r->blend_eq          = param; return 1; /* SET_BLEND_EQUATION */
    case 0x0354: r->depth_func        = param; return 1; /* SET_DEPTH_FUNC */
    case 0x0358: r->color_mask        = param; return 1; /* SET_COLOR_MASK */
    case 0x035C: r->depth_write       = param; return 1; /* SET_DEPTH_MASK */
    case 0x039C: r->cull_face         = param; return 1; /* SET_CULL_FACE */
    case 0x03A0: r->front_face        = param; return 1; /* SET_FRONT_FACE */
    case 0x0394: memcpy(&f, &param, 4); r->depth_min = f; return 1; /* SET_CLIP_MIN */
    case 0x0398: memcpy(&f, &param, 4); r->depth_max = f; return 1; /* SET_CLIP_MAX */
    case 0x0214: r->zeta_va = param ? dma_resolve(param) : 0; return 1; /* ZETA_OFFSET */
    case 0x1D8C: s_gpu.zstencil_clear = param; return 1;  /* SET_ZSTENCIL_CLEAR_VALUE */
    default:     return 0;
    }
}

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    static int inited;

    s_reg[(method & 0x1FFCu) / 4] = param;
    if (pb_verbose() && (method == 0x17C4 || (method >= 0x0C00 && method < 0x0C24))) {
        static int shown;
        if (shown++ < 20)
            fprintf(stderr, "  [GPU] back-light method 0x%04X = 0x%08X\n", method, param);
    }
    /* BACK_END_WRITE_SEMAPHORE_RELEASE. The title's pointer already names the
     * semaphore word; SET_SEMAPHORE_OFFSET is not added (in X-Men Legends it
     * held 0xFF000000 at times, which put the write outside guest memory). */
    if (method == 0x1D70 && s_sem_va) {
        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
        *(volatile uint32_t *)(mem + s_sem_va) = param;
        return;
    }
    if (!inited) {
        inited = 1;
        s_gpu.min_x = s_gpu.min_y = 1e30f;
        s_gpu.max_x = s_gpu.max_y = -1e30f;
    }
    /* Bring-up: the first parameters each surface method carries. A wrong
     * pitch or clip is indistinguishable from a method never arriving unless
     * the values are visible. */
    if (pb_verbose()) {
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

    if (subch != 0) {                      /* 3D class lives on subchannel 0 */
        note_unhandled(method, param);
        return;
    }
    switch (method) {
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_raw_h = param;
        surface_apply_clip();
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_raw_v = param;
        surface_apply_clip();
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        surface_apply_clip();
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
    case NV097_CLEAR_SURFACE:
        clear_surface(param);
        break;

    case NV097_SET_BEGIN_END:
        if (param) {
            s_gpu.prim = param;
            s_gpu.idx_count = 0;
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
        for (i = 0; i < count && s_gpu.idx_count < NV_MAX_INDICES; i++)
            s_gpu.idx[s_gpu.idx_count++] = start + i;
        break;
    }

    case NV097_ARRAY_ELEMENT16:
        /* Two 16-bit indices per parameter word. */
        if (s_gpu.prim && s_gpu.idx_count + 2 <= NV_MAX_INDICES) {
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param & 0xFFFF);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param >> 16);
        }
        break;
    case 0x1808:                            /* ARRAY_ELEMENT32: one index */
        if (s_gpu.prim && s_gpu.idx_count < NV_MAX_INDICES)
            s_gpu.idx[s_gpu.idx_count++] = param;
        break;
    case NV097_SET_TEXTURE_OFFSET:
        /* A texture offset is a DMA-object offset, exactly like a surface or a
         * vertex array offset -- physical, and reachable only through the
         * contiguous window when it names contiguous memory. */
        s_gpu.tex.offset = dma_resolve(param);
        record_tex_reg(method, param);
        break;

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
        /* Verbose runs: the ranked unhandled-method report every 600 flips,
         * so a run that is killed rather than exits still leaves one. */
        if (pb_verbose() && s_gpu.flips % 600 == 0)
            nv2a_pb_exec_report();
        return;

    case NV097_FLIP_STALL:
        /* The stall ends when the buffer being read is the one just finished.
         * There is no scanout here to wait for, so that is now. */
        s_gpu.flip_read = s_gpu.flip_write;
        /* And this is a completed swap, which is what a title's own swap
         * counter counts -- see xbox_Nv2aFrameCounterFlip. */
        xbox_Nv2aFrameCounterFlip();
        /* Show the frame just finished: the surface the last batch drew
         * into. Following clears instead showed whatever offscreen target
         * was cleared last -- X-Men Legends' font/icon atlas. */
        if (s_gpu.drawn_offset) {
            s_gpu.flip_seen = 1;
            xbox_FramebufferWindowSet(dma_resolve(s_gpu.drawn_offset),
                                      s_gpu.drawn_pitch);
        }
        if (s_backend && s_backend->flip)
            s_backend->flip();
        if (pb_verbose()) {
            static unsigned n;
            if (n++ < 8) {
                fprintf(stderr, "  [GPU] flip %u: read=%u write=%u\n",
                        s_gpu.flips, s_gpu.flip_read, s_gpu.flip_write);
                fflush(stderr);
            }
        }
        return;

    case NV097_SET_TEXTURE_FORMAT:
        s_gpu.tex.color = (param >> 8) & 0xFF;
        /* A swizzled texture carries its own dimensions here, as log2 in
         * BASE_SIZE_U/V (nv2a_regs.h: 0x00F00000 / 0x0F000000). It has to:
         * SET_TEXTURE_IMAGE_RECT describes a linear image, and a title that
         * only uses swizzled textures never sends one -- this title sends it
         * once and sets a format 3,176 times. Without this the width and
         * height stayed zero and nothing was ever sampled. */
        if (tex_size_from_format((param >> 8) & 0xFF)) {
            s_gpu.tex.width  = 1u << ((param >> 20) & 0xF);
            s_gpu.tex.height = 1u << ((param >> 24) & 0xF);
        }
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_ADDRESS:
        /* Four bits per axis. 1 is wrap, 3 is clamp-to-edge; the rest (mirror,
         * border) fall back to clamp, which is wrong at an edge rather than
         * wrong everywhere. */
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
        if (capture_render_state(method, param))
            break;
        if (method >= NV097_SET_COMPOSITE_MATRIX_FIRST
         && method <= NV097_SET_COMPOSITE_MATRIX_LAST) {
            memcpy(&s_gpu.composite[(method - NV097_SET_COMPOSITE_MATRIX_FIRST) / 4],
                   &param, 4);
            s_gpu.composite_set = 1;
            break;
        }
        if (method >= NV097_SET_VIEWPORT_OFFSET_FIRST
         && method <= NV097_SET_VIEWPORT_OFFSET_LAST) {
            memcpy(&s_gpu.vp_offset[(method - NV097_SET_VIEWPORT_OFFSET_FIRST) / 4],
                   &param, 4);
            break;
        }
        if (method == NV097_SET_TRANSFORM_EXEC_MODE) {
            s_gpu.xform_mode = param;
            break;
        }
        if (vp_method(method, param))
            break;
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
            if (a->size == 7)                   /* SIZE_3W */
                a->size = 3;
            a->stride = (param >> 8)  & 0xFF;
        } else if (!imm_vertex_method(method, param)) {
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
    const char *spec = getenv("RECOMP_PEEK");
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
    const char *spec = getenv("RECOMP_PEEK_CHAIN");
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

void nv2a_pb_exec_report(void)
{
    peek_addresses();
    peek_chain();
    if (getenv("RECOMP_FIND_NAN")) {
        /* Every report, not once: the matrix is fine early on and only turns
         * to NaN later, so a single scan at startup finds nothing and says
         * nothing. */
        find_nan_matrices();
    }
    if (getenv("RECOMP_FIND_QUAD")) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    fprintf(stderr, "[GPU] draws %u (%u with coordinates), %u indices;"
                    " x %.1f..%.1f  y %.1f..%.1f\n",
            s_gpu.draws, s_gpu.nonzero_draws, s_gpu.verts,
            s_gpu.min_x, s_gpu.max_x, s_gpu.min_y, s_gpu.max_y);
    /* One picture per report rather than per clear: a title clears hundreds of
     * times a second and nobody wants that many files. */
    dump_surface_bmp();

    /* Drawn and skipped separately: "nothing appeared" and "every batch needed
     * a vertex program we do not run" look identical on screen, and only one
     * of them means the rasteriser is broken. */
    fprintf(stderr, "[GPU] brightest pixel written 0x%08X\n", s_gpu.pixel_max);
    fprintf(stderr, "[GPU] %llu pixels written; draw surface 0x%08X"
                    " -> 0x%08X, clear surface 0x%08X -> 0x%08X\n",
            (unsigned long long)s_gpu.pixels, s_gpu.drawn_offset,
            dma_resolve(s_gpu.drawn_offset), s_gpu.color_offset,
            dma_resolve(s_gpu.color_offset));
    fprintf(stderr, "[GPU] rasterised %u triangles; %u batches skipped as not"
                    " screen-space, %u triangles fully off-surface;"
                    " %u batches via the fixed-function transform\n",
            s_gpu.tris_drawn, s_gpu.batches_untransformed,
            s_gpu.tris_skipped_offscreen, s_gpu.batches_ffp);

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

    if (getenv("RECOMP_TEX_STATE")) {
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
        int shown = getenv("RECOMP_PB_UNHANDLED_ALL") ? s_unhandled_count : 10;
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
    }
    for (i = 0; i < s_unhandled_count; i++)       /* the sort moved them */
        s_unhandled_slot[(s_unhandled[i].method & 0x1FFC) / 4] = (uint16_t)(i + 1);
    fflush(stderr);
}
