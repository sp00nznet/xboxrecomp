/**
 * The NV2A 2D engine as XDK D3D drives it: NV062 CONTEXT_SURFACES_2D (the
 * source and destination surfaces) and NV09F IMAGE_BLIT (a rectangle copy
 * between them). D3D's CopyRects and its frame-copy paths use it; Quantum
 * Redshift copies its 640x480 back buffer this way mid-frame and refracts the
 * copy through its lens raindrops.
 *
 * This file is the decode and the arithmetic, with no executor state and no
 * guest memory beyond what a caller passes in, so tests/pb_image_blit checks
 * it standalone. nv2a_pb_exec.c owns the object binds (RAMHT lookups), the
 * context DMA objects and the backend call; semantics follow xemu's pgraph.c
 * (pgraph_method, NV_CONTEXT_SURFACES_2D / NV_IMAGE_BLIT) and its blit.c.
 */
#ifndef XBOXRECOMP_NV2A_2D_H
#define XBOXRECOMP_NV2A_2D_H

#include <stdint.h>
#include "nv2a_pb_state.h"   /* struct nv2a_pb_blit */

#ifdef __cplusplus
extern "C" {
#endif

#define NV2A_CLASS_KELVIN       0x97u
#define NV2A_CLASS_SURFACES_2D  0x62u
#define NV2A_CLASS_IMAGE_BLIT   0x9Fu
#define NV2A_CLASS_BETA         0x12u

#define NV2A_SET_OBJECT                     0x0000u
#define NV062_SET_CONTEXT_DMA_IMAGE_SOURCE  0x0184u
#define NV062_SET_CONTEXT_DMA_IMAGE_DESTIN  0x0188u
#define NV062_SET_COLOR_FORMAT              0x0300u
#define NV062_SET_PITCH                     0x0304u
#define NV062_SET_OFFSET_SOURCE             0x0308u
#define NV062_SET_OFFSET_DESTIN             0x030Cu
#define NV012_SET_BETA                      0x0300u
#define NV09F_SET_CONTEXT_SURFACES          0x019Cu
#define NV09F_SET_OPERATION                 0x02FCu
#define NV09F_CONTROL_POINT_IN              0x0300u
#define NV09F_CONTROL_POINT_OUT             0x0304u
#define NV09F_SIZE                          0x0308u

#define NV09F_OP_BLEND_AND  2u
#define NV09F_OP_SRCCOPY    3u

/* What the 2D objects hold, as the title last set it. */
struct nv2a_2d {
    /* Per subchannel: the SET_OBJECT handle, the graphics class its
     * instance names (0 when RAMHT did not hold it), and whether any
     * SET_OBJECT reached it. */
    uint32_t handle[8], cls[8];
    uint8_t  bound[8];
    /* NV062 */
    uint32_t dma_src, dma_dst;          /* context DMA handles */
    uint32_t color_format;              /* NV062_SET_COLOR_FORMAT_LE_* */
    uint32_t src_pitch, dst_pitch;
    uint32_t src_offset, dst_offset;    /* inside their DMA objects */
    /* NV09F */
    uint32_t surfaces;                  /* the NV062 object it names */
    uint32_t operation;
    int      operation_set;
    uint32_t in_x, in_y, out_x, out_y, w, h;
    /* NV012 BETA: BLEND_AND's factor, bits 30:23 (negative reads 0) */
    uint32_t beta;
};

/* The class on `subch`: what its SET_OBJECT bound, or, with no bind seen,
 * XDK D3D's fixed layout (subchannel 0 Kelvin, 2 IMAGE_BLIT, 3
 * SURFACES_2D, as D3D's device init binds them; 0 for any other). A
 * pushbuffer captured after device init still decodes. A bound
 * subchannel whose class could not be read is 0: the title put some
 * other object there, and decoding its methods as the XDK default's
 * would launch blits nobody asked for. */
uint32_t nv2a_2d_class(const struct nv2a_2d *s, uint32_t subch);

/* Record a SET_OBJECT on subch. cls is the instance's class (its first
 * word's low byte), 0 when the caller could not read it. */
void nv2a_2d_bind(struct nv2a_2d *s, uint32_t subch, uint32_t handle, uint32_t cls);

/* Decode one method on a 2D class. Returns 0 when the method is not one
 * this engine takes (the caller counts it unhandled), 1 when it was stored,
 * 2 when it was NV09F_SIZE with a non-empty rectangle: run the blit now
 * (xemu kicks it there too). */
int nv2a_2d_method(struct nv2a_2d *s, uint32_t cls, uint32_t method, uint32_t param);

/* Bytes per pixel of an NV062 colour format, 0 if it has none (the
 * NV04_SURFACE_2D enum: Y8 1; the 15/16-bit ones 2..5; the 32-bit ones
 * 6..0xB; xemu's blit takes 1, 4, 6, 7, 0xA and 0xB). */
uint32_t nv2a_2d_bpp(uint32_t format);

/* The blit the state describes, with the two surfaces at src_base and
 * dst_base (each the DMA object's base plus its SET_OFFSET_*; the caller
 * resolves them to guest addresses into src and dst). 0 when the colour
 * format has no size. */
int nv2a_2d_blit(const struct nv2a_2d *s, uint32_t src_base, uint32_t dst_base,
                 struct nv2a_pb_blit *b);

/* Bytes from a surface's first pixel to the end of the rectangle at (x, y),
 * w x h, at pitch and bpp: (y + h - 1) * pitch + (x + w) * bpp (xemu's
 * source_offset + source_size). 0 for an empty rectangle. */
uint64_t nv2a_2d_extent(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pitch,
                        uint32_t bpp);

/* Do b in memory: mem is the host address of guest 0. SRCCOPY row by row
 * (the copy order is safe for overlapping surfaces); BLEND_AND mixes the
 * three colour bytes of 32-bit pixels by beta (struct nv2a_2d's), as xemu
 * does. beta is ignored by SRCCOPY. */
void nv2a_2d_copy(uint8_t *mem, const struct nv2a_pb_blit *b, uint32_t beta);

/* The host texels of a blit between two render targets of one geometry:
 * guest w x h, pad guest columns a side (Hor+), n host pixels a guest
 * pixel, hw x hh host texels. Fills box = { src x, src y, dst x, dst y,
 * width, height } in host texels. A blit across the whole guest width at
 * x 0 to x 0 takes the pads with it, so a frame copy keeps its Hor+ wings;
 * any narrower one copies its guest columns only. Returns 0 when a rect is
 * not inside the guest w x h, so the caller copies guest memory instead. */
int nv2a_2d_host_box(const struct nv2a_pb_blit *b, uint32_t w, uint32_t h, uint32_t pad,
                     unsigned n, uint32_t hw, uint32_t hh, uint32_t box[6]);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_NV2A_2D_H */
