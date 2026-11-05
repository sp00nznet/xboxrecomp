/* Render-target ownership (nv2a_backend_common.h): when a target the
 * backend keeps for a surface goes stale because the title rewrote its
 * memory, and the once-per-flip bound on the hashing that finds out. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend_common.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

/* A 512x512 32-bit target, the shape a title leaves behind when it frees a
 * render target's memory and loads textures over it. */
#define ADDR  0x01200000u
#define PITCH 2048u
#define H     512u

int main(void)
{
    uint8_t *mem = (uint8_t *)calloc(1, NV2A_RT_WINDOW);
    struct nv2a_rt_own o;
    uint32_t flip = 10;
    uint64_t n0;
    size_t i;

    CHECK(mem != NULL);
    for (i = 0; i < (size_t)PITCH * H; i++)
        mem[ADDR + i] = (uint8_t)(i * 7u);
    memset(&o, 0, sizeof o);

    /* Made and drawn in flip 10: never stale that flip, whatever memory holds. */
    nv2a_rt_own_reset(&o, mem, ADDR, PITCH, H, flip);
    nv2a_rt_own_drawn(&o, flip);
    mem[ADDR] ^= 1;
    CHECK(!nv2a_rt_own_stale(&o, mem, ADDR, PITCH, H, flip));
    mem[ADDR] ^= 1;

    /* Not drawn since, bytes unchanged: kept (render-to-texture). */
    flip++;
    CHECK(!nv2a_rt_own_stale(&o, mem, ADDR, PITCH, H, flip));

    /* At most one hash per flip: a later check in the same flip is free,
     * even after a write (it is found next flip). */
    n0 = nv2a_rt_own_hashes();
    mem[ADDR + 5] ^= 0x80;
    CHECK(!nv2a_rt_own_stale(&o, mem, ADDR, PITCH, H, flip));
    CHECK(nv2a_rt_own_hashes() == n0);
    flip++;
    CHECK(nv2a_rt_own_stale(&o, mem, ADDR, PITCH, H, flip));
    CHECK(nv2a_rt_own_hashes() == n0 + 1);
    mem[ADDR + 5] ^= 0x80;

    /* A write anywhere in pitch x h: the first byte, a middle row, the last
     * byte. Each case starts from a fresh reference. */
    {
        static const uint32_t at[] = { 0, PITCH * (H / 2) + 1000, PITCH * H - 1 };
        for (i = 0; i < sizeof at / sizeof at[0]; i++) {
            flip++;
            nv2a_rt_own_reset(&o, mem, ADDR, PITCH, H, flip);
            flip++;
            mem[ADDR + at[i]] ^= 0x5A;
            CHECK(nv2a_rt_own_stale(&o, mem, ADDR, PITCH, H, flip));
            mem[ADDR + at[i]] ^= 0x5A;
        }
    }

    /* A byte just outside the range is not the target's. */
    flip++;
    nv2a_rt_own_reset(&o, mem, ADDR, PITCH, H, flip);
    flip++;
    mem[ADDR + PITCH * H] ^= 1;
    mem[ADDR - 1] ^= 1;
    CHECK(!nv2a_rt_own_stale(&o, mem, ADDR, PITCH, H, flip));

    /* The same bytes written again: kept. */
    flip++;
    memcpy(mem + ADDR, mem + ADDR, PITCH * H);
    CHECK(!nv2a_rt_own_stale(&o, mem, ADDR, PITCH, H, flip));

    /* A drawn target is never checked: drawing this flip, then a write. */
    flip++;
    nv2a_rt_own_drawn(&o, flip);
    mem[ADDR + 100] ^= 1;
    n0 = nv2a_rt_own_hashes();
    CHECK(!nv2a_rt_own_stale(&o, mem, ADDR, PITCH, H, flip));
    CHECK(nv2a_rt_own_hashes() == n0);

    /* A write-back makes the new bytes the reference: reset after it. */
    nv2a_rt_own_reset(&o, mem, ADDR, PITCH, H, flip);
    flip++;
    CHECK(!nv2a_rt_own_stale(&o, mem, ADDR, PITCH, H, flip));

    /* A reset is not a check: checked, written back (reset), then the title
     * writes in the same flip: the next check sees it. */
    flip++;
    CHECK(!nv2a_rt_own_stale(&o, mem, ADDR, PITCH, H, flip));
    nv2a_rt_own_reset(&o, mem, ADDR, PITCH, H, flip);
    mem[ADDR + 77] ^= 1;
    CHECK(nv2a_rt_own_stale(&o, mem, ADDR, PITCH, H, flip));
    mem[ADDR + 77] ^= 1;
    nv2a_rt_own_reset(&o, mem, ADDR, PITCH, H, flip);

    /* A range running past the top of the low window is clamped: no read
     * past the buffer (ASan or a guard page would catch one), and a write in
     * the part that exists is still seen. */
    {
        uint32_t top = NV2A_RT_WINDOW - PITCH * 4u;
        flip++;
        nv2a_rt_own_reset(&o, mem, top, PITCH, H, flip);
        flip++;
        CHECK(!nv2a_rt_own_stale(&o, mem, top, PITCH, H, flip));
        mem[NV2A_RT_WINDOW - 1] ^= 1;
        flip++;
        CHECK(nv2a_rt_own_stale(&o, mem, top, PITCH, H, flip));
    }

    /* Overlap of guest ranges, through the low 27 bits. */
    CHECK(nv2a_rt_overlap(0x0119B000u, 0x100000u, 0x011A5000u, 0x40000u));   /* inside */
    CHECK(nv2a_rt_overlap(0x0119B000u, 0x100000u, 0x01290000u, 0x40000u));   /* the tail */
    CHECK(nv2a_rt_overlap(0x8119B000u, 0x100000u, 0x0119B000u, 1u));         /* windows */
    CHECK(!nv2a_rt_overlap(0x0119B000u, 0x100000u, 0x0129B000u, 0x40000u));  /* adjacent */
    CHECK(!nv2a_rt_overlap(0x0119B000u, 0u, 0x0119B000u, 0x40000u));         /* empty */

    free(mem);
    printf("rt_alias: ok\n");
    return 0;
}
