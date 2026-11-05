/**
 * basic_shaders: the basic shaders, for batches without register
 * combiners, as the backends compile them (nv2a_basic_hlsl.h,
 * nv2a_basic_msl.h).
 *
 *   stock      s_ps_src (D3D11) and s_msl (Metal) hash to the texts of
 *              toolkit 6385aaf8, before render.snap_2d: a stock run
 *              draws with the base shaders, which the JSRF dogenzaka
 *              D3D11 golden showed to matter. A deliberate
 *              change to them updates the checksums here: vs_pass took
 *              the upscale tie shift, which is 0 at 1x, so a
 *              stock run still places every vertex where 6385aaf8 did;
 *              the D3D11 pass-through (s_vs_pass) took the same shift
 *   snap       the render.snap_2d variants sample with the 1x gradients
 *              (SampleGrad, gradient2d) and the stock ones do not
 *   compile    on macOS, s_msl's fs_basic, vs_pass and fs_clear and the
 *              snap library's fs_basic_snap build with Metal; a typo
 *              there would otherwise only log FAILED at run time and
 *              fall back to the stock shader
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nv2a_basic_hlsl.h"
#include "nv2a_basic_msl.h"

#ifdef MSL_HAVE_METAL
int msl_compile(const char *src, const char *fn, char *err, int errsize);
#endif

static int s_fail;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "[basic_shaders] FAIL: %s\n", what);
        s_fail++;
    }
}

static uint64_t fnv64(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 0x100000001b3ull;
    return h;
}

static const char s_ps_src[] = NV2A_HLSL_PS_BASIC;
static const char s_ps_snap_src[] = NV2A_HLSL_PS_BASIC_SNAP;
static const char s_msl[] = NV2A_MSL_BASIC;
static const char s_msl_snap[] = NV2A_MSL_BASIC_SNAP;
static const char s_vs_pass[] = NV2A_HLSL_VS_PASS;

int main(void)
{
    check("stock HLSL is 6385aaf8's", strlen(s_ps_src) == 2325
          && fnv64(s_ps_src) == 0x7ca2bcd0dede08fdull);
    check("stock MSL is 6385aaf8's plus the tie shift", strlen(s_msl) == 3371
          && fnv64(s_msl) == 0x1730838980e6d759ull);
    check("stock HLSL pass-through is 6385aaf8's plus the tie shift",
          strlen(s_vs_pass) == 981 && fnv64(s_vs_pass) == 0x73a3e344b098a945ull);
    check("both pass-throughs move only flat ties",
          strstr(s_vs_pass, "(float)(i.v0.w == 1.0)") && strstr(s_msl, "float(v[0].w == 1.0)"));
    if (s_fail)
        fprintf(stderr, "[basic_shaders] HLSL %zu bytes %016llx, MSL %zu bytes %016llx, "
                "HLSL VS %zu bytes %016llx\n",
                strlen(s_ps_src), (unsigned long long)fnv64(s_ps_src),
                strlen(s_msl), (unsigned long long)fnv64(s_msl),
                strlen(s_vs_pass), (unsigned long long)fnv64(s_vs_pass));

    check("stock HLSL has no snap gradients", !strstr(s_ps_src, "SampleGrad")
          && !strstr(s_ps_src, "nv2a_snap_grad") && !strstr(s_ps_src, "nv2a_snap_uv2d"));
    check("stock MSL has no snap gradients", !strstr(s_msl, "gradient2d")
          && !strstr(s_msl, "nv2a_snap_grad") && !strstr(s_msl, "fs_basic_snap"));
    check("snap HLSL samples with the 1x gradients", strstr(s_ps_snap_src, "SampleGrad")
          && strstr(s_ps_snap_src, "nv2a_snap_grad(") && strstr(s_ps_snap_src, "nv2a_snap_uv2d("));
    check("snap MSL samples with the 1x gradients", strstr(s_msl_snap, "gradient2d")
          && strstr(s_msl_snap, "nv2a_snap_grad(") && strstr(s_msl_snap, "fs_basic_snap"));

#ifdef MSL_HAVE_METAL
    {
        static const struct { const char *src, *fn; } c[] = {
            { s_msl, "fs_basic" }, { s_msl, "vs_pass" }, { s_msl, "fs_clear" },
            { s_msl_snap, "fs_basic_snap" },
        };
        char err[2048];
        unsigned i;
        for (i = 0; i < sizeof c / sizeof c[0]; i++)
            if (msl_compile(c[i].src, c[i].fn, err, (int)sizeof err) != 0) {
                fprintf(stderr, "[basic_shaders] %s does not compile:\n%s\n", c[i].fn, err);
                s_fail++;
            }
    }
#endif

    if (s_fail) {
        fprintf(stderr, "[basic_shaders] FAIL: %d check(s)\n", s_fail);
        return 1;
    }
    fprintf(stderr, "[basic_shaders] done: all checks pass\n");
    return 0;
}
