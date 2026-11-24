/* apu_gain: the master volume on the final mix.
 *
 *   apu_gain_test unity   100% leaves every int16 value as it was
 *   apu_gain_test clamp   the level clamps to 0..100; 0 is silence
 *   apu_gain_test scale   the square law, rounding, the int16 extremes
 */
#include "apu_gain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

/* Every int16 value once, the way a period holds them. */
static int16_t s_all[65536], s_ref[65536];

static void fill_all(void)
{
    for (int i = 0; i < 65536; i++)
        s_all[i] = (int16_t)(i - 32768);
    memcpy(s_ref, s_all, sizeof s_ref);
}

static void test_unity(void)
{
    CHECK("unity: the default is 100", apu_master_volume() == 100);
    CHECK("unity: gain is exactly one", apu_master_gain_q15(100) == 32768);
    fill_all();
    apu_master_gain_apply(s_all, 65536);
    CHECK("unity: default leaves samples bit-identical", !memcmp(s_all, s_ref, sizeof s_ref));
    apu_master_volume_set(40);
    apu_master_volume_set(100);
    fill_all();
    apu_master_gain_apply(s_all, 65536);
    CHECK("unity: back at 100 leaves samples bit-identical",
          !memcmp(s_all, s_ref, sizeof s_ref));
    apu_master_gain_apply(NULL, 0);   /* nothing to do, nothing touched */
}

static void test_clamp(void)
{
    CHECK("clamp: below 0", apu_master_volume_set(-5) == 0 && apu_master_volume() == 0);
    CHECK("clamp: above 100", apu_master_volume_set(250) == 100 && apu_master_volume() == 100);
    CHECK("clamp: in range kept", apu_master_volume_set(55) == 55 && apu_master_volume() == 55);
    CHECK("clamp: q15 below 0", apu_master_gain_q15(-1) == 0);
    CHECK("clamp: q15 above 100", apu_master_gain_q15(101) == 32768);
    apu_master_volume_set(0);
    fill_all();
    apu_master_gain_apply(s_all, 65536);
    {
        int silent = 1;
        for (int i = 0; i < 65536; i++)
            silent &= s_all[i] == 0;
        CHECK("clamp: 0 is silence", silent);
    }
    apu_master_volume_set(100);
}

static void test_scale(void)
{
    int32_t prev = -1;
    for (int p = 0; p <= 100; p++) {
        int32_t g = apu_master_gain_q15(p);
        double want = 32768.0 * (p / 100.0) * (p / 100.0);
        CHECK("scale: square law", fabs(g - want) <= 0.5);
        CHECK("scale: monotonic", g >= prev);
        prev = g;
    }
    /* 50% is -12 dB, near enough: a quarter of the amplitude. */
    CHECK("scale: 50 is a quarter", apu_master_gain_q15(50) == 8192);

    apu_master_volume_set(50);
    fill_all();
    apu_master_gain_apply(s_all, 65536);
    CHECK("scale: full scale positive", s_all[65535] == 8192);   /* 32767/4, rounded */
    CHECK("scale: full scale negative", s_all[0] == -8192);
    CHECK("scale: zero stays zero", s_all[32768] == 0);
    {
        int ok = 1;
        for (int i = 1; i < 65536; i++)
            ok &= s_all[i] >= s_all[i - 1];
        CHECK("scale: order kept", ok);
    }
    apu_master_volume_set(99);
    fill_all();
    apu_master_gain_apply(s_all, 65536);
    {
        int ok = 1;
        for (int i = 0; i < 65536; i++)
            ok &= abs(s_all[i]) <= abs((int)s_ref[i]);
        CHECK("scale: under unity never louder", ok);
    }
    apu_master_volume_set(100);
}

int main(int argc, char **argv)
{
    const char *t = argc > 1 ? argv[1] : "";
    if (!strcmp(t, "unity"))
        test_unity();
    else if (!strcmp(t, "clamp"))
        test_clamp();
    else if (!strcmp(t, "scale"))
        test_scale();
    else {
        fprintf(stderr, "usage: apu_gain_test unity|clamp|scale\n");
        return 2;
    }
    if (!failures)
        printf("%s: ok\n", t);
    return failures ? 1 : 0;
}
