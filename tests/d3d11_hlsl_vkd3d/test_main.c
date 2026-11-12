/* d3d11_hlsl_vkd3d - the Linux D3D11 host's HLSL compiler
 * (src/d3d/d3d11_hlsl_vkd3d.c, over vkd3d-shader) on what the backend
 * compiles:
 *
 *   1. the version string and the flag mapping (D3DCOMPILE_OPTIMIZATION_LEVEL3,
 *      the backend's one flag, maps to nothing; the matrix order and
 *      backwards compatibility bits are kept);
 *   2. a broken source fails with messages;
 *   3. the basic shaders (nv2a_basic_hlsl.h: VS_PASS, PS_BASIC and its
 *      render.snap_2d twin), the texture shader's dot-product and cube-map
 *      states (tests/d3d8_hlsl_split/dot_cases.h, with and without
 *      rt_offset), the W-buffer variant, and NPROG vertex programs and NCOMB
 *      combiner states from d3d8_hlsl_split's pseudo-random stream,
 *      through the emitters (d3d8_hlsl.h), each to DXBC ("DXBC" magic),
 *      except a vertex program whose text has -1.#INF (compile_text);
 *   4. with a directory (argv[1], or D3D11_HLSL_CACHE_DIR): every entry of a
 *      D3D11 shader cache (shader_cache.c's format), such as the one a
 *      Proton run writes, recompiled from its own key (source, target,
 *      entry, flags). That is the attract frames' real shader set; how many
 *      come out byte-equal to the cached DXBC is reported, not checked
 *      (Proton's vkd3d-shader may be another version).
 *
 * Exit 0 only when every compile in 3 and 4 succeeded. vkd3d-shader 1.14
 * (Fedora 42/43) fails four of the BLiNX 2 attract set's pixel shaders
 * (E5032, an [unroll] loop it cannot unroll); 1.18 compiles them all, so
 * the Linux build needs 1.18 or later. */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d11_hlsl.h"
#include "d3d8_hlsl.h"
#include "nv2a_basic_hlsl.h"
#include "../d3d8_hlsl_split/dot_cases.h"

#define OPT_LEVEL3 (1u << 15)   /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */
#define NPROG 64
#define NCOMB 64

static int s_fail, s_ok, s_inf;

static int compile_one(const char *what, const char *src, size_t len,
                       const char *entry, const char *target,
                       unsigned f1, unsigned f2, void **keep, size_t *keep_len)
{
    void *code = NULL;
    size_t n = 0;
    char *msg = NULL;
    int rc = d3d11_hlsl_compile(src, len, what, entry, target, f1, f2,
                                &code, &n, &msg);
    if (rc < 0 || n < 4 || memcmp(code, "DXBC", 4)) {
        printf("FAIL compile %-28s %s rc=%d\n%s\n", what, target, rc,
               msg ? msg : "(no messages)");
        s_fail++;
        d3d11_hlsl_free(code);
        d3d11_hlsl_free(msg);
        return 0;
    }
    s_ok++;
    d3d11_hlsl_free(msg);
    if (keep) {
        *keep = code;
        *keep_len = n;
    } else {
        d3d11_hlsl_free(code);
    }
    return 1;
}

static void compile_text(const char *what, const char *src, const char *target)
{
    /* The vertex emitter's LOG of 0 writes fxc's -1.#INF, which
     * vkd3d-shader's HLSL parser rejects (a syntax error). None of the
     * game's programs in a Proton cache has one, so the random programs
     * that do are counted, not compiled; phase 2 of linux-native-vulkan
     * owns the emitter fix (it changes the hashed text). */
    if (strstr(src, "1.#INF")) {
        s_inf++;
        return;
    }
    compile_one(what, src, strlen(src), "main", target, OPT_LEVEL3, 0, NULL, NULL);
}

static int check_api(void)
{
    const char *v = d3d11_hlsl_version();
    void *code = (void *)1;
    size_t n = 1;
    char *msg = NULL;
    int bad = 0, rc;

    printf("compiler: %s\n", v ? v : "(none)");
    if (!v || !strstr(v, "vkd3d")) {
        printf("FAIL: version string\n");
        bad = 1;
    }
    if (d3d11_hlsl_flags(OPT_LEVEL3) != 0
            || d3d11_hlsl_flags(1u << 3) != 1u << 3
            || d3d11_hlsl_flags(1u << 4) != 1u << 4
            || d3d11_hlsl_flags(1u << 12) != 1u << 12) {
        printf("FAIL: flag mapping\n");
        bad = 1;
    }
    rc = d3d11_hlsl_compile("float4 main() : SV_Target { return nope; }", 42,
                            "broken", "main", "ps_5_0", OPT_LEVEL3, 0,
                            &code, &n, &msg);
    if (rc >= 0 || code || n || !msg || !strstr(msg, "broken")) {
        printf("FAIL: a broken source: rc=%d code=%p len=%zu msg=%s\n", rc, code,
               n, msg ? msg : "(none)");
        bad = 1;
    }
    d3d11_hlsl_free(msg);
    return bad;
}

/* d3d8_hlsl_split's stream: the first NPROG programs are its first ones
 * (the combiner states then differ, as it draws 400 programs first). */
static uint32_t s_seed = 0x2f6b1d3u;
static uint32_t rnd(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed ^ (s_seed >> 15);
}

static void check_emitters(void)
{
    static uint32_t prog[NV2A_VS_MAX_INSTRUCTIONS * 4];
    static NV2AVshProgram parsed;
    static char buf[256 * 1024];
    static const uint32_t zero[8];
    char what[64];
    NV2ACombinerState st;
    int i, k, off;

    compile_text("vs_pass", NV2A_HLSL_VS_PASS, "vs_5_0");
    compile_text("ps_basic", NV2A_HLSL_PS_BASIC, "ps_5_0");
    compile_text("ps_basic_snap", NV2A_HLSL_PS_BASIC_SNAP, "ps_5_0");

    for (i = 0; i < DOT_NCASE; i++)
        for (off = 0; off < 2; off++) {
            dot_case_state(i, off, &st);
            if (d3d8_combiners_generate_hlsl(&st, buf, (int)sizeof buf) <= 0) {
                printf("FAIL: %s: no HLSL\n", k_dot_cases[i].name);
                s_fail++;
                continue;
            }
            snprintf(what, sizeof what, "%s%s", k_dot_cases[i].name, off ? "+off" : "");
            compile_text(what, buf, "ps_5_0");
        }

    memset(&st, 0, sizeof st);
    d3d8_combiners_from_regs(zero, zero, zero, zero, 1u, 0u, 0u, 1u, &st);
    st.w_depth = 1;
    if (d3d8_combiners_generate_hlsl(&st, buf, (int)sizeof buf) > 0)
        compile_text("rc-wdepth", buf, "ps_5_0");

    for (i = 0; i < NPROG; i++) {
        int len = 1 + (int)(rnd() % 40), fin = (int)(rnd() % (uint32_t)len);
        for (k = 0; k < len * 4; k++)
            prog[k] = rnd();
        for (k = 0; k < len; k++)
            prog[k * 4 + 3] &= ~1u;
        prog[fin * 4 + 3] |= 1u;
        d3d8_vsh_parse(prog, len, &parsed);
        if (d3d8_vsh_generate_hlsl_ex(&parsed, NV2A_VSH_HLSL_SCREEN_SPACE,
                                      buf, (int)sizeof buf) > 0) {
            snprintf(what, sizeof what, "vsh-ss-%d", i);
            compile_text(what, buf, "vs_5_0");
        }
    }
    for (i = 0; i < NCOMB; i++) {
        uint32_t cicw[8], aicw[8], cocw[8], aocw[8];
        uint32_t control, fcw0, fcw1, modes;
        for (k = 0; k < 8; k++) {
            cicw[k] = rnd(); aicw[k] = rnd(); cocw[k] = rnd(); aocw[k] = rnd();
        }
        control = (rnd() & ~0xFFu) | (1 + rnd() % 8);
        fcw0 = rnd(); fcw1 = rnd(); modes = rnd();
        d3d8_combiners_from_regs(cicw, aicw, cocw, aocw, control, fcw0, fcw1,
                                 modes, &st);
        st.fog_input = (int)(rnd() & 1);
        if (d3d8_combiners_generate_hlsl(&st, buf, (int)sizeof buf) > 0) {
            snprintf(what, sizeof what, "rc-%d", i);
            compile_text(what, buf, "ps_5_0");
        }
    }
}

/* ---- a shader cache's entries ------------------------------------------ */

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A u32-length-prefixed string of the key at *at, copied to out. */
static int key_str(const uint8_t *k, size_t klen, size_t *at, char *out, size_t n)
{
    uint32_t len;
    if (*at + 4 > klen)
        return 0;
    len = get32(k + *at);
    *at += 4;
    if (len >= n || *at + len > klen)
        return 0;
    memcpy(out, k + *at, len);
    out[len] = 0;
    *at += len;
    return 1;
}

static int s_entries, s_equal, s_vkd3d_entries;

static void check_entry(const char *path, const char *name)
{
    uint8_t *b = NULL, *k, *code;
    char compiler[512], target[32], entry[64];
    size_t size, at = 0, klen, clen;
    uint32_t f1, f2, slen;
    void *out = NULL;
    size_t out_len = 0;
    FILE *f = fopen(path, "rb");
    long l;

    if (!f)
        return;
    if (fseek(f, 0, SEEK_END) || (l = ftell(f)) < 32 || fseek(f, 0, SEEK_SET)
            || !(b = malloc((size_t)l)) || fread(b, 1, (size_t)l, f) != (size_t)l) {
        fclose(f);
        free(b);
        return;
    }
    fclose(f);
    size = (size_t)l;
    if (memcmp(b, "XRSC", 4) || get32(b + 4) != 1u) {
        free(b);
        return;
    }
    klen = get32(b + 8);
    clen = get32(b + 12);
    if (32 + klen + clen != size) {
        printf("FAIL entry %s: sizes\n", name);
        s_fail++;
        free(b);
        return;
    }
    k = b + 32;
    code = k + klen;
    if (!key_str(k, klen, &at, compiler, sizeof compiler)
            || !key_str(k, klen, &at, target, sizeof target)
            || !key_str(k, klen, &at, entry, sizeof entry)
            || at + 12 > klen) {
        printf("FAIL entry %s: key\n", name);
        s_fail++;
        free(b);
        return;
    }
    f1 = get32(k + at);
    f2 = get32(k + at + 4);
    slen = get32(k + at + 8);
    at += 12;
    if (at + slen != klen) {
        printf("FAIL entry %s: source length\n", name);
        s_fail++;
        free(b);
        return;
    }
    s_entries++;
    if (strstr(compiler, "vkd3d-shader|"))
        s_vkd3d_entries++;
    if (compile_one(name, (const char *)k + at, slen, entry, target, f1, f2,
                    &out, &out_len)) {
        if (out_len == clen && !memcmp(out, code, clen))
            s_equal++;
        d3d11_hlsl_free(out);
    }
    free(b);
}

static void check_cache(const char *dir)
{
    char path[4096];
    struct dirent *e;
    DIR *d = opendir(dir);

    if (!d) {
        printf("FAIL: cannot open %s\n", dir);
        s_fail++;
        return;
    }
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        check_entry(path, e->d_name);
    }
    closedir(d);
    printf("cache %s: %d entries (%d from vkd3d-shader natively), "
           "%d DXBC byte-equal to the cached one\n",
           dir, s_entries, s_vkd3d_entries, s_equal);
    if (!s_entries) {
        printf("FAIL: no cache entries in %s\n", dir);
        s_fail++;
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : getenv("D3D11_HLSL_CACHE_DIR");

    s_fail += check_api();
    check_emitters();
    if (dir && *dir)
        check_cache(dir);
    printf("d3d11_hlsl_vkd3d: %d compiled, %d failed, %d emitted texts with "
           "-1.#INF not compiled\n", s_ok, s_fail, s_inf);
    return s_fail ? 1 : 0;
}
