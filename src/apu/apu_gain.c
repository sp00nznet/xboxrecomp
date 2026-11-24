/**
 * The master volume: see apu_gain.h.
 */
#include "apu_gain.h"

#include <stdatomic.h>

/* Written by the menu or config thread, read by the APU frame thread once a
 * period. Nothing else is ordered against it, so relaxed is enough; the
 * atomic only keeps the read whole. */
static _Atomic int g_master_volume = APU_MASTER_VOLUME_MAX;

int apu_master_volume_set(int pct)
{
    if (pct < 0)
        pct = 0;
    if (pct > APU_MASTER_VOLUME_MAX)
        pct = APU_MASTER_VOLUME_MAX;
    atomic_store_explicit(&g_master_volume, pct, memory_order_relaxed);
    return pct;
}

int apu_master_volume(void)
{
    return atomic_load_explicit(&g_master_volume, memory_order_relaxed);
}

int32_t apu_master_gain_q15(int pct)
{
    if (pct <= 0)
        return 0;
    if (pct >= APU_MASTER_VOLUME_MAX)
        return 32768;
    /* (pct / 100)^2 in Q15, rounded. */
    return (int32_t)(((int64_t)pct * pct * 32768 + 5000) / 10000);
}

void apu_master_gain_apply(int16_t *s, int n)
{
    int pct = atomic_load_explicit(&g_master_volume, memory_order_relaxed);
    int32_t g;
    if (pct >= APU_MASTER_VOLUME_MAX)
        return;
    g = apu_master_gain_q15(pct);
    for (int i = 0; i < n; i++) {
        /* |s * g| < 2^30: no overflow before the shift. Under unity the
         * result cannot leave int16, but the clamp keeps that true if the
         * range ever grows past 100%. */
        int32_t v = ((int32_t)s[i] * g + 16384) >> 15;
        if (v > 32767)
            v = 32767;
        if (v < -32768)
            v = -32768;
        s[i] = (int16_t)v;
    }
}
