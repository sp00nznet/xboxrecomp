/**
 * pb_image_blit: the NV2A 2D engine. Quantum Redshift copies its
 * back buffer with NV09F IMAGE_BLIT between two NV062 CONTEXT_SURFACES_2D
 * surfaces mid-frame; the executor dropped every method off subchannel 0,
 * so the copy never happened and the lens raindrops sampled an empty
 * buffer. Semantics are xemu's pgraph.c.
 *
 *   decode    NV062 and NV09F methods land in struct nv2a_2d (pitch src
 *             low 16 / dst high 16, offsets to 27 bits, points x low / y
 *             high); NV09F_SIZE with a non-empty rectangle kicks the blit
 *   class     no SET_OBJECT seen: XDK D3D's layout (0 Kelvin, 2 blit, 3
 *             surfaces); a bind overrides it, and a bind whose instance
 *             RAMHT does not hold is class 0, not the layout's
 *   ramht     a rebind reads the kept RAMHT slot; a moved entry is scanned
 *             for again
 *   bpp       the colour formats' sizes; an unknown one refuses the blit
 *   extent    (y + h - 1) * pitch + (x + w) * bpp
 *   host box  a whole-width blit takes the Hor+ pads; a narrower one
 *             its guest columns at render scale; one outside the surface
 *             is refused
 *   copy      SRCCOPY row by row, an overlapping move downwards included;
 *             BLEND_AND mixes 32-bit pixels by beta
 *   executor  RAMHT objects as D3D binds them, on subchannels other than
 *             the XDK's (so the class must come from the instance): the
 *             blit copies guest bytes at the DMA objects' bases plus the
 *             offsets, leaves the rest alone, and a blit past the
 *             destination object's limit writes nothing
 *   backend   a backend with on_blit gets the blit, resolved, and the
 *             executor writes nothing itself
 *   hor+      a centred 2D batch over a blit copy is stretched; a 3D one,
 *             or one over no copy, keeps its place, and a stretch or cover
 *             is left alone
 *   uv        nv2a_aspect_uv_tracks: the identity, a small shift and a
 *             slight magnification track; a magnification past an eighth
 *             of the extent, a picture in picture's offset, normalised
 *             coordinates read at size 1, a NaN and no vertices do not.
 *             nv2a_aspect_uv_sample reads a pass-through batch's position
 *             and stage texcoord, spreads a long batch over max, and gives
 *             0 for a stage with no texcoord
 *   pack      nv2a_surf_pack_row: R5G6B5, X1R5G5B5 with X 0 and 1, the
 *             X8R8G8B8 and X1A7R8G8B8 pairs' constant alpha bits, A8R8G8B8
 *             as it is, and 0 for a format with no encoding
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nv2a_pb_state.h"
#include "nv2a_2d.h"
#include "xbox_memory_layout.h"
#include "nv2a_backend_common.h"

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);

/* The kernel library expects these from the title's generated code. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define NV2A    0xFD000000u
#define PRAMIN  (NV2A + 0x700000u)

static int s_fail;

static uint8_t *g8(uint32_t va) { return (uint8_t *)xbox_GetMemoryOffset() + va; }
static volatile uint32_t *g32(uint32_t va) { return (volatile uint32_t *)g8(va); }

static void expect(const char *what, uint64_t got, uint64_t want)
{
    int ok = got == want;
    fprintf(stderr, "[pb_image_blit] %-52s %llu want %llu %s\n", what,
            (unsigned long long)got, (unsigned long long)want, ok ? "PASS" : "FAIL");
    s_fail += !ok;
}

static void build_xbe(uint8_t *xbe, size_t n)
{
    uint32_t v;
    memset(xbe, 0, n);
    memcpy(xbe, "XBEH", 4);
    v = 0x00010000u; memcpy(xbe + 0x104, &v, 4);   /* base address */
    v = (uint32_t)n; memcpy(xbe + 0x108, &v, 4);    /* header size */
    v = 0;           memcpy(xbe + 0x11C, &v, 4);    /* no sections */
    v = 0x00010200u; memcpy(xbe + 0x120, &v, 4);    /* section headers */
    v = 0x00010800u ^ 0x5B6D40B6u; memcpy(xbe + 0x158, &v, 4); /* empty thunks */
}

static void object(uint32_t handle, uint32_t ramht_off, uint32_t inst,
                   uint32_t w0, uint32_t w1, uint32_t w2)
{
    *g32(PRAMIN + ramht_off) = handle;
    *g32(PRAMIN + ramht_off + 4) = 0x80000000u | (inst >> 4);
    *g32(PRAMIN + inst) = w0;
    *g32(PRAMIN + inst + 4) = w1;
    *g32(PRAMIN + inst + 8) = w2;
}

static void check_decode(void)
{
    struct nv2a_2d s;
    struct nv2a_pb_blit b;

    memset(&s, 0, sizeof s);
    expect("class: subchannel 0 without a bind is Kelvin", nv2a_2d_class(&s, 0), 0x97);
    expect("class: subchannel 2 without a bind is IMAGE_BLIT", nv2a_2d_class(&s, 2), 0x9F);
    expect("class: subchannel 3 without a bind is SURFACES_2D", nv2a_2d_class(&s, 3), 0x62);
    expect("class: subchannel 5 without a bind is none", nv2a_2d_class(&s, 5), 0);
    nv2a_2d_bind(&s, 2, 0x11, 0x62);
    expect("class: a bind overrides the layout", nv2a_2d_class(&s, 2), 0x62);
    nv2a_2d_bind(&s, 2, 0x11, 0);
    expect("class: a bind of an unread class is none", nv2a_2d_class(&s, 2), 0);
    nv2a_2d_bind(&s, 6, 0x12, 0);
    expect("class: an unread bind off the layout is none", nv2a_2d_class(&s, 6), 0);
    expect("class: subchannel 3 is still the layout's", nv2a_2d_class(&s, 3), 0x62);

    /* Quantum Redshift's blit at flip 7500, from a method log. */
    expect("decode: SET_COLOR_FORMAT stored",
           nv2a_2d_method(&s, 0x62, NV062_SET_COLOR_FORMAT, 4), 1);
    nv2a_2d_method(&s, 0x62, NV062_SET_PITCH, 0x05000500u);
    nv2a_2d_method(&s, 0x62, NV062_SET_OFFSET_SOURCE, 0xF87C4000u);
    nv2a_2d_method(&s, 0x62, NV062_SET_OFFSET_DESTIN, 0x00A1C000u);
    expect("decode: source pitch (low 16)", s.src_pitch, 0x500);
    expect("decode: destination pitch (high 16)", s.dst_pitch, 0x500);
    expect("decode: source offset to 27 bits", s.src_offset, 0x007C4000u);
    expect("decode: a Kelvin method is not taken",
           nv2a_2d_method(&s, 0x97, NV062_SET_PITCH, 0), 0);
    expect("decode: an unknown NV062 method is not taken",
           nv2a_2d_method(&s, 0x62, 0x0400, 0), 0);
    nv2a_2d_method(&s, 0x9F, NV09F_CONTROL_POINT_IN, (3u << 16) | 5u);
    nv2a_2d_method(&s, 0x9F, NV09F_CONTROL_POINT_OUT, (7u << 16) | 9u);
    expect("decode: point in x", s.in_x, 5);
    expect("decode: point in y", s.in_y, 3);
    expect("decode: point out x", s.out_x, 9);
    expect("decode: point out y", s.out_y, 7);
    expect("decode: an empty SIZE does not kick",
           nv2a_2d_method(&s, 0x9F, NV09F_SIZE, 0x01E00000u), 1);
    expect("decode: SIZE 640x480 kicks",
           nv2a_2d_method(&s, 0x9F, NV09F_SIZE, 0x01E00280u), 2);
    expect("blit: built", nv2a_2d_blit(&s, 0x1000, 0x2000, &b), 1);
    expect("blit: operation SRCCOPY when never set", b.operation, NV09F_OP_SRCCOPY);
    expect("blit: bpp of R5G6B5", b.bpp, 2);
    expect("blit: w", b.w, 640);
    expect("blit: h", b.h, 480);
    nv2a_2d_method(&s, 0x9F, NV09F_SET_OPERATION, NV09F_OP_BLEND_AND);
    nv2a_2d_blit(&s, 0, 0, &b);
    expect("blit: operation as set", b.operation, NV09F_OP_BLEND_AND);
    nv2a_2d_method(&s, 0x62, NV062_SET_COLOR_FORMAT, 0x0C);
    expect("blit: an unknown colour format refuses", nv2a_2d_blit(&s, 0, 0, &b), 0);
    nv2a_2d_method(&s, 0x12, NV012_SET_BETA, 0x80000000u | 0x40000000u);
    expect("beta: a negative factor reads 0", s.beta, 0);
    nv2a_2d_method(&s, 0x12, NV012_SET_BETA, 0x407FFFFFu);
    expect("beta: 8 fraction bits kept", s.beta, 0x40000000u);

    expect("bpp: Y8", nv2a_2d_bpp(1), 1);
    expect("bpp: R5G6B5", nv2a_2d_bpp(4), 2);
    expect("bpp: X8R8G8B8", nv2a_2d_bpp(7), 4);
    expect("bpp: A8R8G8B8", nv2a_2d_bpp(0xA), 4);
    expect("bpp: Y32", nv2a_2d_bpp(0xB), 4);
    expect("bpp: 0 has none", nv2a_2d_bpp(0), 0);

    expect("extent: 640x480 R5G6B5 at pitch 1280",
           nv2a_2d_extent(0, 0, 640, 480, 1280, 2), 614400);
    expect("extent: 10x2 at (4, 3), pitch 100, 4 bpp",
           nv2a_2d_extent(4, 3, 10, 2, 100, 4), 4 * 100 + 14 * 4);
    expect("extent: empty", nv2a_2d_extent(4, 3, 0, 2, 100, 4), 0);
}

static void check_host_box(void)
{
    struct nv2a_pb_blit b;
    uint32_t box[6];

    memset(&b, 0, sizeof b);
    b.w = 640;
    b.h = 480;
    /* 16:9 Hor+ at scale 1: 107 pad columns a side, 854 host texels. */
    expect("host box: a whole frame is accepted",
           nv2a_2d_host_box(&b, 640, 480, 107, 1, 854, 480, box), 1);
    expect("host box: whole frame from x 0", box[0], 0);
    expect("host box: whole frame to x 0", box[2], 0);
    expect("host box: whole frame takes the pads (width)", box[4], 854);
    expect("host box: whole frame height", box[5], 480);

    b.in_x = 10; b.in_y = 20; b.out_x = 30; b.out_y = 40; b.w = 100; b.h = 50;
    expect("host box: a narrow blit at scale 2 is accepted",
           nv2a_2d_host_box(&b, 640, 480, 107, 2, 1708, 960, box), 1);
    expect("host box: narrow source x (x + pad) * n", box[0], (10 + 107) * 2);
    expect("host box: narrow source y * n", box[1], 40);
    expect("host box: narrow destination x", box[2], (30 + 107) * 2);
    expect("host box: narrow destination y", box[3], 80);
    expect("host box: narrow width, guest columns only", box[4], 200);
    expect("host box: narrow height", box[5], 100);

    b.in_x = 0; b.out_x = 0; b.in_y = 0; b.out_y = 0; b.w = 641; b.h = 480;
    expect("host box: wider than the surface refused",
           nv2a_2d_host_box(&b, 640, 480, 107, 1, 854, 480, box), 0);
    b.w = 640; b.out_y = 1;
    expect("host box: past the bottom refused",
           nv2a_2d_host_box(&b, 640, 480, 107, 1, 854, 480, box), 0);
}

static void check_copy(void)
{
    static uint8_t mem[4096];
    struct nv2a_pb_blit b;
    uint32_t i, ok = 1;

    for (i = 0; i < sizeof mem; i++)
        mem[i] = (uint8_t)(i * 7u);
    /* One 16-byte pitch surface at 0, rows 0..7 moved down 2 rows. */
    memset(&b, 0, sizeof b);
    b.src = b.dst = 0;
    b.src_pitch = b.dst_pitch = 16;
    b.bpp = 2;
    b.operation = NV09F_OP_SRCCOPY;
    b.out_y = 2;
    b.w = 8;
    b.h = 8;
    nv2a_2d_copy(mem, &b, 0);
    for (i = 0; i < 8 * 16; i++)
        ok &= mem[2 * 16 + i] == (uint8_t)(i * 7u);
    expect("copy: an overlapping move down keeps every row", ok, 1);
    expect("copy: rows above the destination untouched", mem[17], (uint8_t)(17 * 7u));

    /* BLEND_AND: (s * b + d * (0x7F80 - b)) / 0x7F80 per colour byte. */
    memset(mem, 0, sizeof mem);
    mem[0] = 200; mem[1] = 200; mem[2] = 200; mem[3] = 9;
    mem[64] = 100; mem[65] = 100; mem[66] = 100; mem[67] = 77;
    memset(&b, 0, sizeof b);
    b.src = 0;
    b.dst = 64;
    b.src_pitch = b.dst_pitch = 16;
    b.bpp = 4;
    b.operation = NV09F_OP_BLEND_AND;
    b.w = 1;
    b.h = 1;
    nv2a_2d_copy(mem, &b, 0x40000000u);
    expect("blend: colour mixed by beta", mem[64], 150);
    expect("blend: alpha byte untouched", mem[67], 77);
}

/* A backend that only takes blits. */
static struct nv2a_pb_blit s_got;
static int s_got_n;
static void stub_on_blit(const struct nv2a_pb_blit *b)
{
    s_got = *b;
    s_got_n++;
}

#define SRC_PHYS  0x00200000u
#define DST_PHYS  0x00300000u

static void blit(uint32_t sub_s, uint32_t sub_b, uint32_t fmt, uint32_t sp, uint32_t dp,
                 uint32_t so, uint32_t dof, uint32_t in, uint32_t out, uint32_t size)
{
    nv2a_pb_exec_method(sub_s, NV062_SET_COLOR_FORMAT, fmt);
    nv2a_pb_exec_method(sub_s, NV062_SET_PITCH, (dp << 16) | sp);
    nv2a_pb_exec_method(sub_s, NV062_SET_OFFSET_SOURCE, so);
    nv2a_pb_exec_method(sub_s, NV062_SET_OFFSET_DESTIN, dof);
    nv2a_pb_exec_method(sub_b, NV09F_SET_OPERATION, NV09F_OP_SRCCOPY);
    nv2a_pb_exec_method(sub_b, NV09F_CONTROL_POINT_IN, in);
    nv2a_pb_exec_method(sub_b, NV09F_CONTROL_POINT_OUT, out);
    nv2a_pb_exec_method(sub_b, NV09F_SIZE, size);
}

static void check_executor(void)
{
    uint8_t *src, *dst;
    uint32_t x, y, ok = 1;
    static struct nv2a_pb_backend stub;

    src = g8(nv2a_pb_dma_resolve(SRC_PHYS));
    dst = g8(nv2a_pb_dma_resolve(DST_PHYS));
    for (y = 0; y < 32; y++)
        for (x = 0; x < 256; x++)
            src[y * 256 + x] = (uint8_t)(x ^ (y * 13u));
    memset(dst, 0xEE, 64 * 512);

    nv2a_pb_set_backend(NULL);              /* the CPU rasteriser */
    *g32(NV2A + 0x002210u) = 0x03000000u;   /* PFIFO_RAMHT: PRAMIN 0, 4 KB */
    /* The graphics objects: class in word 0's low byte. */
    object(0x30, 0x60, 0x1200, 0x00000062u, 0, 0);
    object(0x20, 0x68, 0x1210, 0x0000009Fu, 0, 0);
    /* Two context DMA objects: frame in word 2, limit in word 1. */
    object(0x40, 0x70, 0x1220, 0x0002B03Du, 0x000FFFFFu, SRC_PHYS | 3u);
    object(0x41, 0x78, 0x1230, 0x0002B03Du, 0x000FFFFFu, DST_PHYS | 3u);
    object(0x42, 0x80, 0x1240, 0x0002B03Du, 0x000003FFu, DST_PHYS | 3u);

    /* Subchannels 5 and 6, not the XDK's 2 and 3: only the instance's
     * class tells them apart from nothing. */
    nv2a_pb_exec_method(6, NV2A_SET_OBJECT, 0x30);
    nv2a_pb_exec_method(5, NV2A_SET_OBJECT, 0x20);
    nv2a_pb_exec_method(6, NV062_SET_CONTEXT_DMA_IMAGE_SOURCE, 0x40);
    nv2a_pb_exec_method(6, NV062_SET_CONTEXT_DMA_IMAGE_DESTIN, 0x41);
    nv2a_pb_exec_method(5, NV09F_SET_CONTEXT_SURFACES, 0x30);
    /* 64x16 R5G6B5 pixels from (8, 4) at pitch 256 to (2, 1) at pitch
     * 512, offsets 0x100 / 0x200 into their objects. */
    blit(6, 5, 4, 256, 512, 0x100, 0x200, (4u << 16) | 8u, (1u << 16) | 2u,
         (16u << 16) | 64u);
    for (y = 0; y < 16; y++)
        for (x = 0; x < 128; x++)
            ok &= dst[0x200 + (y + 1) * 512 + 4 + x] == src[0x100 + (y + 4) * 256 + 16 + x];
    expect("executor: the rectangle copied at base + offset", ok, 1);
    expect("executor: the byte before the row untouched", dst[0x200 + 512 + 3], 0xEE);
    expect("executor: the byte after the row untouched", dst[0x200 + 512 + 4 + 128], 0xEE);
    expect("executor: the row above untouched", dst[0x200 + 4], 0xEE);
    expect("executor: the row below untouched", dst[0x200 + 17 * 512 + 4], 0xEE);

    /* Past the limit: object 0x42 holds 1 KB, the blit ends far past it. */
    memset(dst, 0xEE, 64 * 512);
    nv2a_pb_exec_method(6, NV062_SET_CONTEXT_DMA_IMAGE_DESTIN, 0x42);
    blit(6, 5, 4, 256, 512, 0x100, 0x200, (4u << 16) | 8u, (1u << 16) | 2u,
         (16u << 16) | 64u);
    expect("executor: past the destination's limit writes nothing",
           dst[0x200 + 512 + 4], 0xEE);
    nv2a_pb_exec_method(6, NV062_SET_CONTEXT_DMA_IMAGE_DESTIN, 0x41);

    /* A rebind reads the kept RAMHT slot, not the table; an entry that
     * moves is found again by the scan, with its new instance words. */
    {
        uint64_t s0, h0, s1, h1;
        nv2a_pb_ramht_stats(&s0, &h0);
        nv2a_pb_exec_method(6, NV062_SET_CONTEXT_DMA_IMAGE_SOURCE, 0x40);
        nv2a_pb_ramht_stats(&s1, &h1);
        expect("ramht: a rebind is a kept-slot hit", h1 - h0, 1);
        expect("ramht: a rebind scans nothing", s1 - s0, 0);
        *g32(PRAMIN + 0x78) = 0;                       /* 0x41 leaves its slot */
        object(0x41, 0x90, 0x1250, 0x0002B03Du, 0x000FFFFFu, DST_PHYS | 3u);
        nv2a_pb_exec_method(6, NV062_SET_CONTEXT_DMA_IMAGE_DESTIN, 0x41);
        nv2a_pb_ramht_stats(&s0, &h0);
        expect("ramht: a moved entry is scanned for", s0 - s1, 1);
        memset(dst, 0xEE, 64 * 512);
        blit(6, 5, 4, 256, 512, 0x100, 0x200, (4u << 16) | 8u, (1u << 16) | 2u,
             (16u << 16) | 64u);
        expect("ramht: the moved destination still takes the blit",
               dst[0x200 + 512 + 4], src[0x100 + 4 * 256 + 16]);
    }

    /* A bound subchannel whose class RAMHT does not hold is no 2D object,
     * even on the XDK's blit subchannel 2. */
    nv2a_pb_exec_method(2, NV2A_SET_OBJECT, 0x77);
    memset(dst, 0xEE, 64 * 512);
    blit(6, 2, 4, 256, 512, 0x100, 0x200, (4u << 16) | 8u, (1u << 16) | 2u,
         (16u << 16) | 64u);
    expect("class: an unread bind on 2 launches no blit", dst[0x200 + 512 + 4], 0xEE);

    /* A backend with on_blit owns the blit. */
    stub.on_blit = stub_on_blit;
    nv2a_pb_set_backend(&stub);
    memset(dst, 0xEE, 64 * 512);
    blit(6, 5, 4, 256, 512, 0x100, 0x200, (4u << 16) | 8u, (1u << 16) | 2u,
         (16u << 16) | 64u);
    expect("backend: on_blit called once", s_got_n, 1);
    expect("backend: source resolved", s_got.src, nv2a_pb_dma_resolve(SRC_PHYS + 0x100));
    expect("backend: destination resolved", s_got.dst, nv2a_pb_dma_resolve(DST_PHYS + 0x200));
    expect("backend: source offset physical", s_got.src_offset, SRC_PHYS + 0x100);
    expect("backend: bpp", s_got.bpp, 2);
    expect("backend: in x", s_got.in_x, 8);
    expect("backend: out y", s_got.out_y, 1);
    expect("backend: the executor wrote nothing", dst[0x200 + 512 + 4], 0xEE);
    nv2a_pb_set_backend(NULL);
}

static void check_place(void)
{
    expect("hor+: a centred 2D batch over a copy is stretched",
           nv2a_aspect_place_copy(NV2A_PLACE_CENTRE, 1, 1), NV2A_PLACE_STRETCH);
    expect("hor+: a 2D batch over no copy stays centred (the HUD)",
           nv2a_aspect_place_copy(NV2A_PLACE_CENTRE, 1, 0), NV2A_PLACE_CENTRE);
    expect("hor+: a 3D batch over a copy stays centred",
           nv2a_aspect_place_copy(NV2A_PLACE_CENTRE, 0, 1), NV2A_PLACE_CENTRE);
    expect("hor+: a cover over a copy is left alone",
           nv2a_aspect_place_copy(NV2A_PLACE_COVER, 1, 1), NV2A_PLACE_COVER);
}

/* A quad's four corners at x0..x1, y0..y1, and its uv by map. */
static void quad(float x0, float y0, float x1, float y1, float xy[4][2])
{
    xy[0][0] = x0; xy[0][1] = y0;
    xy[1][0] = x1; xy[1][1] = y0;
    xy[2][0] = x1; xy[2][1] = y1;
    xy[3][0] = x0; xy[3][1] = y1;
}

static void check_uv(void)
{
    float xy[4][2], uv[4][2];
    float vf[100][2][4];
    float sxy[NV2A_ASPECT_UV_MAX][2], suv[NV2A_ASPECT_UV_MAX][2];
    const int slot[2] = {0, 9};
    int i;

    quad(220, 140, 420, 340, xy);
    memcpy(uv, xy, sizeof uv);
    expect("uv: the identity tracks",
           nv2a_aspect_uv_tracks((const float (*)[2])xy, (const float (*)[2])uv, 4, 1, 1, 640, 480), 1);
    for (i = 0; i < 4; i++) {
        uv[i][0] = xy[i][0] + 3;
        uv[i][1] = xy[i][1] - 2;
    }
    expect("uv: a small shift tracks",
           nv2a_aspect_uv_tracks((const float (*)[2])xy, (const float (*)[2])uv, 4, 1, 1, 640, 480), 1);
    for (i = 0; i < 4; i++) {   /* a drop magnifying 5%: drift 10 < 200/8 + 2 */
        uv[i][0] = 320 + (xy[i][0] - 320) * 0.95f;
        uv[i][1] = 240 + (xy[i][1] - 240) * 0.95f;
    }
    expect("uv: a slight magnification tracks",
           nv2a_aspect_uv_tracks((const float (*)[2])xy, (const float (*)[2])uv, 4, 1, 1, 640, 480), 1);
    for (i = 0; i < 4; i++) {   /* 2x: drift 100 > 27 */
        uv[i][0] = 320 + (xy[i][0] - 320) * 0.5f;
        uv[i][1] = 240 + (xy[i][1] - 240) * 0.5f;
    }
    expect("uv: a magnification past an eighth of the extent does not track",
           nv2a_aspect_uv_tracks((const float (*)[2])xy, (const float (*)[2])uv, 4, 1, 1, 640, 480), 0);
    quad(440, 20, 600, 140, xy);   /* a picture in picture: the whole frame, small */
    for (i = 0; i < 4; i++) {
        uv[i][0] = xy[i][0] - 440;
        uv[i][1] = xy[i][1] - 20;
    }
    expect("uv: a picture in picture's offset does not track",
           nv2a_aspect_uv_tracks((const float (*)[2])xy, (const float (*)[2])uv, 4, 1, 1, 640, 480), 0);
    quad(220, 140, 420, 340, xy);
    for (i = 0; i < 4; i++) {
        uv[i][0] = xy[i][0] / 640.0f;
        uv[i][1] = xy[i][1] / 480.0f;
    }
    expect("uv: normalised coordinates at the texture's size track",
           nv2a_aspect_uv_tracks((const float (*)[2])xy, (const float (*)[2])uv, 4, 640, 480, 640, 480), 1);
    expect("uv: normalised coordinates read as texels do not",
           nv2a_aspect_uv_tracks((const float (*)[2])xy, (const float (*)[2])uv, 4, 1, 1, 640, 480), 0);
    memcpy(uv, xy, sizeof uv);
    uv[2][0] = NAN;
    expect("uv: a NaN does not track",
           nv2a_aspect_uv_tracks((const float (*)[2])xy, (const float (*)[2])uv, 4, 1, 1, 640, 480), 0);
    expect("uv: no vertices do not track",
           nv2a_aspect_uv_tracks((const float (*)[2])xy, (const float (*)[2])uv, 0, 1, 1, 640, 480), 0);

    memset(vf, 0, sizeof vf);
    for (i = 0; i < 100; i++) {
        vf[i][0][0] = (float)i;           /* position, input 0 */
        vf[i][0][1] = (float)(2 * i);
        vf[i][1][0] = (float)i + 0.5f;    /* texcoord 0, input 9 */
        vf[i][1][1] = (float)(3 * i);
    }
    expect("uv sample: a pass-through batch's four vertices",
           nv2a_aspect_uv_sample(NULL, 0, NULL, &vf[0][0][0], 2, slot, 4, 0, sxy, suv,
                                 NV2A_ASPECT_UV_MAX), 4);
    expect("uv sample: vertex 3's y", (uint64_t)sxy[3][1], 6);
    expect("uv sample: vertex 3's t", (uint64_t)suv[3][1], 9);
    expect("uv sample: vertex 3's s, x 3 + 0.5", (uint64_t)(suv[3][0] * 2), 7);
    expect("uv sample: a long batch spread over max",
           nv2a_aspect_uv_sample(NULL, 0, NULL, &vf[0][0][0], 2, slot, 100, 0, sxy, suv,
                                 NV2A_ASPECT_UV_MAX), NV2A_ASPECT_UV_MAX);
    expect("uv sample: every third vertex (100 / 32)", (uint64_t)sxy[1][0], 3);
    expect("uv sample: the last sample inside the batch", (uint64_t)sxy[31][0], 93);
    expect("uv sample: a stage with no texcoord input",
           nv2a_aspect_uv_sample(NULL, 0, NULL, &vf[0][0][0], 2, slot, 4, 1, sxy, suv,
                                 NV2A_ASPECT_UV_MAX), 0);
}

static void check_pack(void)
{
    /* B 0x10, G 0x20, R 0xF8, A 0x40 */
    const uint8_t px[4] = {0x10, 0x20, 0xF8, 0x40};
    uint8_t out[4];
    uint16_t h;
    uint32_t w;

#define PACK16(f, want, what) \
    do { \
        expect(what ": 2 bytes", nv2a_surf_pack_row(f, px, 1, out), 2); \
        memcpy(&h, out, 2); \
        expect(what, h, want); \
    } while (0)
#define PACK32(f, want, what) \
    do { \
        expect(what ": 4 bytes", nv2a_surf_pack_row(f, px, 1, out), 4); \
        memcpy(&w, out, 4); \
        expect(what, w, want); \
    } while (0)
    PACK16(0x3, 0xF902, "pack: R5G6B5");
    PACK16(0x1, 0x7C82, "pack: X1R5G5B5_Z1R5G5B5, X 0");
    PACK16(0x2, 0xFC82, "pack: X1R5G5B5_O1R5G5B5, X 1");
    PACK32(0x4, 0x00F82010u, "pack: X8R8G8B8_Z8R8G8B8, alpha 0");
    PACK32(0x5, 0xFFF82010u, "pack: X8R8G8B8_O8R8G8B8, alpha 0xFF");
    PACK32(0x6, 0x20F82010u, "pack: X1A7R8G8B8_Z1A7R8G8B8, X 0 over alpha's top 7 bits");
    PACK32(0x7, 0xA0F82010u, "pack: X1A7R8G8B8_O1A7R8G8B8, X 1 over alpha's top 7 bits");
    PACK32(0x8, 0x40F82010u, "pack: A8R8G8B8 as it is");
#undef PACK16
#undef PACK32
    memcpy(out, px, 4);
    nv2a_surf_pack_row(0x5, out, 1, out);
    memcpy(&w, out, 4);
    expect("pack: in place", w, 0xFFF82010u);
    expect("pack: B8 has no encoding", nv2a_surf_pack_row(0x9, px, 1, out), 0);
    expect("pack: the format query", nv2a_surf_pack_row(0x3, NULL, 0, NULL), 2);
}

int main(void)
{
    static uint8_t xbe[0x1000];

    setvbuf(stderr, NULL, _IONBF, 0);
    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe) || !xbox_Nv2aAperture()) {
        fprintf(stderr, "[pb_image_blit] FAIL: guest memory init\n");
        return 1;
    }
    check_decode();
    check_host_box();
    check_copy();
    check_executor();
    check_place();
    check_uv();
    check_pack();
    fprintf(stderr, "[pb_image_blit] %s\n", s_fail ? "FAIL" : "PASS");
    return s_fail ? 1 : 0;
}
