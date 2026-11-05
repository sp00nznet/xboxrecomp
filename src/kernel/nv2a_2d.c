/* The NV2A 2D engine's decode and arithmetic (nv2a_2d.h). */
#include "nv2a_2d.h"
#include <string.h>

uint32_t nv2a_2d_class(const struct nv2a_2d *s, uint32_t subch)
{
    subch &= 7u;
    if (s->bound[subch])
        return s->cls[subch];
    /* No bind seen: the layout XDK D3D's device init sets up. Quantum
     * Redshift (XDK 4721) sends its surfaces on 3 and its blit on 2. */
    switch (subch) {
    case 0: return NV2A_CLASS_KELVIN;
    case 2: return NV2A_CLASS_IMAGE_BLIT;
    case 3: return NV2A_CLASS_SURFACES_2D;
    default: return 0;
    }
}

void nv2a_2d_bind(struct nv2a_2d *s, uint32_t subch, uint32_t handle, uint32_t cls)
{
    subch &= 7u;
    s->handle[subch] = handle;
    s->cls[subch] = cls;
    s->bound[subch] = 1;
}

int nv2a_2d_method(struct nv2a_2d *s, uint32_t cls, uint32_t method, uint32_t param)
{
    if (cls == NV2A_CLASS_SURFACES_2D) {
        switch (method) {
        case NV062_SET_CONTEXT_DMA_IMAGE_SOURCE: s->dma_src = param; return 1;
        case NV062_SET_CONTEXT_DMA_IMAGE_DESTIN: s->dma_dst = param; return 1;
        case NV062_SET_COLOR_FORMAT:             s->color_format = param; return 1;
        case NV062_SET_PITCH:
            s->src_pitch = param & 0xFFFFu;
            s->dst_pitch = param >> 16;
            return 1;
        /* 27 bits, as xemu keeps them: the offset of 128 MB of memory. */
        case NV062_SET_OFFSET_SOURCE: s->src_offset = param & 0x07FFFFFFu; return 1;
        case NV062_SET_OFFSET_DESTIN: s->dst_offset = param & 0x07FFFFFFu; return 1;
        default: return 0;
        }
    }
    if (cls == NV2A_CLASS_BETA && method == NV012_SET_BETA) {
        /* Sign bit, 31 fraction bits; the hardware keeps 8 of them and
         * clamps a negative factor to 0 (xemu). */
        s->beta = param & 0x80000000u ? 0 : param & 0x7F800000u;
        return 1;
    }
    if (cls == NV2A_CLASS_IMAGE_BLIT) {
        switch (method) {
        case NV09F_SET_CONTEXT_SURFACES: s->surfaces = param; return 1;
        case NV09F_SET_OPERATION:
            s->operation = param;
            s->operation_set = 1;
            return 1;
        case NV09F_CONTROL_POINT_IN:
            s->in_x = param & 0xFFFFu;
            s->in_y = param >> 16;
            return 1;
        case NV09F_CONTROL_POINT_OUT:
            s->out_x = param & 0xFFFFu;
            s->out_y = param >> 16;
            return 1;
        case NV09F_SIZE:
            s->w = param & 0xFFFFu;
            s->h = param >> 16;
            return s->w && s->h ? 2 : 1;
        default: return 0;
        }
    }
    return 0;
}

uint32_t nv2a_2d_bpp(uint32_t format)
{
    if (format == 0x01u)
        return 1;
    if (format >= 0x02u && format <= 0x05u)
        return 2;
    if (format >= 0x06u && format <= 0x0Bu)
        return 4;
    return 0;
}

int nv2a_2d_blit(const struct nv2a_2d *s, uint32_t src_base, uint32_t dst_base,
                 struct nv2a_pb_blit *b)
{
    memset(b, 0, sizeof *b);
    b->bpp = nv2a_2d_bpp(s->color_format);
    if (!b->bpp)
        return 0;
    b->src_offset = src_base;
    b->dst_offset = dst_base;
    b->src = src_base;
    b->dst = dst_base;
    b->src_pitch = s->src_pitch;
    b->dst_pitch = s->dst_pitch;
    b->format = s->color_format;
    /* SRCCOPY unless the title chose: D3D sets it once at device init,
     * which a pushbuffer picked up later never shows. */
    b->operation = s->operation_set ? s->operation : NV09F_OP_SRCCOPY;
    b->in_x = s->in_x;
    b->in_y = s->in_y;
    b->out_x = s->out_x;
    b->out_y = s->out_y;
    b->w = s->w;
    b->h = s->h;
    return 1;
}

uint64_t nv2a_2d_extent(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pitch,
                        uint32_t bpp)
{
    if (!w || !h)
        return 0;
    return (uint64_t)(y + h - 1u) * pitch + (uint64_t)(x + w) * bpp;
}

void nv2a_2d_copy(uint8_t *mem, const struct nv2a_pb_blit *b, uint32_t beta)
{
    uint8_t *src = mem + b->src + (size_t)b->in_y * b->src_pitch + (size_t)b->in_x * b->bpp;
    uint8_t *dst = mem + b->dst + (size_t)b->out_y * b->dst_pitch + (size_t)b->out_x * b->bpp;
    size_t row = (size_t)b->w * b->bpp;
    uint32_t y;

    if (b->operation == NV09F_OP_BLEND_AND) {
        /* xemu's blit.c: 32-bit pixels, beta's top 8 fraction bits. */
        uint32_t mx = 0x7F80u, bm = (beta & 0x7F800000u) >> 16, inv = mx - bm, x, ch;
        if (b->bpp != 4)
            return;
        for (y = 0; y < b->h; y++) {
            uint8_t *s = src + (size_t)y * b->src_pitch, *d = dst + (size_t)y * b->dst_pitch;
            for (x = 0; x < b->w; x++)
                for (ch = 0; ch < 3; ch++)
                    d[x * 4u + ch] = (uint8_t)((s[x * 4u + ch] * bm + d[x * 4u + ch] * inv) / mx);
        }
        return;
    }
    if (b->operation != NV09F_OP_SRCCOPY)
        return;
    /* A destination below an overlapping source is copied bottom up, so
     * no row is read after it was written; memmove covers a row's own
     * overlap. */
    if (dst > src && dst < src + (size_t)b->h * b->src_pitch) {
        for (y = b->h; y-- > 0;)
            memmove(dst + (size_t)y * b->dst_pitch, src + (size_t)y * b->src_pitch, row);
    } else {
        for (y = 0; y < b->h; y++)
            memmove(dst + (size_t)y * b->dst_pitch, src + (size_t)y * b->src_pitch, row);
    }
}

int nv2a_2d_host_box(const struct nv2a_pb_blit *b, uint32_t w, uint32_t h, uint32_t pad,
                     unsigned n, uint32_t hw, uint32_t hh, uint32_t box[6])
{
    uint64_t sx, dx, bw;

    if (!b->w || !b->h || n < 1
        || (uint64_t)b->in_x + b->w > w || (uint64_t)b->in_y + b->h > h
        || (uint64_t)b->out_x + b->w > w || (uint64_t)b->out_y + b->h > h)
        return 0;
    if (b->in_x == 0 && b->out_x == 0 && b->w == w) {
        /* The whole width: the pads too, which hold the Hor+ frame's
         * wings. */
        sx = dx = 0;
        bw = hw;
    } else {
        sx = (uint64_t)(b->in_x + pad) * n;
        dx = (uint64_t)(b->out_x + pad) * n;
        bw = (uint64_t)b->w * n;
    }
    box[0] = (uint32_t)sx;
    box[1] = b->in_y * n;
    box[2] = (uint32_t)dx;
    box[3] = b->out_y * n;
    box[4] = (uint32_t)bw;
    box[5] = b->h * n;
    if (sx + bw > hw || dx + bw > hw || (uint64_t)box[1] + box[5] > hh
        || (uint64_t)box[3] + box[5] > hh)
        return 0;
    return 1;
}
