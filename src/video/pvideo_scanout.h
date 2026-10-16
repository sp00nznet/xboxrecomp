#ifndef XBOX_PVIDEO_SCANOUT_H
#define XBOX_PVIDEO_SCANOUT_H

#include <stddef.h>
#include <stdint.h>
#include "../nv2a/nv2a_regs.h"

/* PVIDEO is the NV2A's video overlay: a YUY2 buffer in guest memory, scaled
 * and placed over the framebuffer at scanout, optionally behind a colour key.
 * Titles play movies through it (Bink's Xbox player does), so without it the
 * screen stays on whatever the framebuffer last held. */

static inline unsigned pvideo_clamp(int value)
{
    return value < 0 ? 0u : value > 255 ? 255u : (unsigned)value;
}

/* Draw bank 0 or 1 of the overlay into dst (width x height, 0xAARRGGBB).
 * regs is the PVIDEO register block indexed by byte offset / 4; source holds
 * `available` bytes of the bank's YUY2 image. Returns 0 without drawing when
 * the bank is not pending, the overlay is stopped, or the registers describe
 * anything this does not handle (only YUY2 is). */
static inline int pvideo_scanout(const uint32_t *regs, const uint8_t *source,
                                 size_t available, uint32_t *dst,
                                 unsigned width, unsigned height, unsigned bank)
{
#define PV(reg) regs[((reg) + bank * 4u) / 4u]
    unsigned format = PV(NV_PVIDEO_FORMAT);
    unsigned pitch = format & NV_PVIDEO_FORMAT_PITCH;
    unsigned iw = PV(NV_PVIDEO_SIZE_IN) & 0x7ff;
    unsigned ih = (PV(NV_PVIDEO_SIZE_IN) >> 16) & 0x7ff;
    unsigned ox = PV(NV_PVIDEO_POINT_OUT) & 0xfff;
    unsigned oy = (PV(NV_PVIDEO_POINT_OUT) >> 16) & 0xfff;
    unsigned ow = PV(NV_PVIDEO_SIZE_OUT) & 0xfff;
    unsigned oh = (PV(NV_PVIDEO_SIZE_OUT) >> 16) & 0xfff;
    uint64_t start_s = (uint64_t)(PV(NV_PVIDEO_POINT_IN) & 0x7fff) << 16;
    uint64_t start_t = (uint64_t)(PV(NV_PVIDEO_POINT_IN) >> 17) << 17;
    uint64_t bytes = (uint64_t)pitch * ih;
    unsigned x, y;

    if (!(regs[NV_PVIDEO_BUFFER / 4] & (1u << (bank * 4u))) ||
        (regs[NV_PVIDEO_STOP / 4] & 1u) ||
        !iw || !ih || !ow || !oh || (iw & 1u) || pitch < iw * 2u ||
        ((format & NV_PVIDEO_FORMAT_COLOR) >> 16) != 1u || bytes > available ||
        (uint64_t)PV(NV_PVIDEO_OFFSET) + bytes > PV(NV_PVIDEO_LIMIT))
        return 0;

    for (y = 0; y < oh && (uint64_t)oy + y < height; ++y) {
        uint64_t sy = (start_t + (uint64_t)y * PV(NV_PVIDEO_DT_DY)) >> 20;
        if (sy >= ih) continue;
        for (x = 0; x < ow && (uint64_t)ox + x < width; ++x) {
            uint64_t sx = (start_s + (uint64_t)x * PV(NV_PVIDEO_DS_DX)) >> 20;
            uint32_t *pixel = dst + (size_t)(oy + y) * width + ox + x;
            const uint8_t *pair;
            int c, u, v;
            if (sx >= iw) continue;
            if ((format & NV_PVIDEO_FORMAT_DISPLAY) &&
                (*pixel & 0xffffffu) != (regs[NV_PVIDEO_COLOR_KEY / 4] & 0xffffffu))
                continue;
            pair = source + (size_t)sy * pitch + (size_t)(sx & ~1ull) * 2;
            c = pair[(sx & 1u) * 2] - 16;
            u = pair[1] - 128;
            v = pair[3] - 128;
            *pixel = 0xff000000u |
                (pvideo_clamp((298*c + 409*v + 128) >> 8) << 16) |
                (pvideo_clamp((298*c - 100*u - 208*v + 128) >> 8) << 8) |
                pvideo_clamp((298*c + 516*u + 128) >> 8);
        }
    }
#undef PV
    return 1;
}

#endif
