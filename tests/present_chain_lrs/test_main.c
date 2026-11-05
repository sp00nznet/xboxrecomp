/*
 * present_chain_lrs - see CMakeLists.txt.
 *
 *   present_chain_lrs_test LIBRASHADER SHADERS_DIR
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <stdlib.h>
#else
#include <limits.h>
#endif

#include "nv2a_present_chain.h"
#include "shader_profile.h"

static int s_fail;

#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);             \
            s_fail = 1;                                                              \
        }                                                                            \
    } while (0)

static const struct nv2a_present_param *find(const struct nv2a_present_param *p, int n,
                                             const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(p[i].name, name))
            return &p[i];
    return NULL;
}

static int describe(const char *path, struct nv2a_present_param *out, int cap)
{
    return nv2a_present_chain_describe(path, out, cap);
}

int main(int argc, char **argv)
{
    static struct nv2a_present_param p[128];
    char shaders[1024], fsr[1100], lottes[1100], out[1100], err[256];
    const char *paths[2];
    const struct nv2a_present_param *r;
    int n, nf, nl;

    if (argc < 3 || !argv[1][0]) {
        printf("skip: no librashader for this host\n");
        return 77;
    }
#ifdef _WIN32
    if (!_fullpath(shaders, argv[2], sizeof shaders))
        return 2;
#else
    if (!realpath(argv[2], shaders))
        return 2;
#endif
    nv2a_present_chain_set_lib(argv[1]);
    snprintf(fsr, sizeof fsr, "%s/fsr.slangp", shaders);
    snprintf(lottes, sizeof lottes, "%s/crt-lottes.slangp", shaders);

    nf = describe(fsr, p, 128);
    CHECK(nf == 4);
    nl = describe(lottes, p, 128);
    CHECK(nl == 13);

    paths[0] = fsr;
    paths[1] = lottes;
    snprintf(out, sizeof out, "composed dir/slangp/fsr-crt.slangp");
    CHECK(shader_profile_compose(paths, NULL, 2, "fsr-crt", out, NULL, err, sizeof err) == 0);
    {
        char abs[1200];
#ifdef _WIN32
        _fullpath(abs, out, sizeof abs);
#else
        if (!realpath(out, abs))
            snprintf(abs, sizeof abs, "%s", out);
#endif
        n = describe(abs, p, 128);
    }
    CHECK(n == nf + nl);
    /* the wrappers' values survive the composition */
    r = find(p, n, "warpX");
    CHECK(r && r->initial > 0.0099f && r->initial < 0.0101f);
    r = find(p, n, "warpY");
    CHECK(r && r->initial > 0.0199f && r->initial < 0.0201f);
    r = find(p, n, "FSR_SHARPENING");
    CHECK(r && r->initial > 0.299f && r->initial < 0.301f);

    if (!s_fail)
        printf("ok present_chain_lrs: %d + %d = %d parameters\n", nf, nl, n);
    return s_fail;
}
