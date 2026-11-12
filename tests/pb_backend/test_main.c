/* The executor backend for a run (pb_backend.h): the variable outranks the
 * build's default, an empty variable is unset, and only a d3d11 run is
 * demoted to cpu, once. */
#include <stdio.h>
#include <string.h>
#include "pb_backend.h"
#include "recomp_env.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
#define SAME(a, b) (!strcmp((a), (b)))

int main(void)
{
#ifdef XBOXRECOMP_DXVK_NATIVE
    const char *dflt = "d3d11";
#else
    const char *dflt = "cpu";
#endif
    CHECK(SAME(pb_backend_default(), dflt));

    /* Pure: the variable, else the default; demoted turns d3d11 into cpu. */
    CHECK(SAME(pb_backend_resolve(NULL, "d3d11", 0), "d3d11"));
    CHECK(SAME(pb_backend_resolve("", "d3d11", 0), "d3d11"));
    CHECK(SAME(pb_backend_resolve("cpu", "d3d11", 0), "cpu"));
    CHECK(SAME(pb_backend_resolve("metal", "cpu", 0), "metal"));
    CHECK(SAME(pb_backend_resolve("D3D11", "cpu", 0), "D3D11"));
    CHECK(SAME(pb_backend_resolve("D3D11", "cpu", 1), "cpu"));
    CHECK(SAME(pb_backend_resolve(NULL, "d3d11", 1), "cpu"));
    CHECK(SAME(pb_backend_resolve("null", "d3d11", 1), "null"));
    CHECK(SAME(pb_backend_resolve("metal", "cpu", 1), "metal"));

    /* Through the variable. */
    recomp_env_set(RENV_PB_BACKEND, NULL);
    CHECK(SAME(pb_backend(), dflt));
    recomp_env_set(RENV_PB_BACKEND, "Null");
    CHECK(pb_backend_is("null"));
    CHECK(!pb_backend_is("d3d11"));
    /* A cpu run is never demoted, and the call leaves no mark. */
    recomp_env_set(RENV_PB_BACKEND, "cpu");
    pb_backend_demote("test");
    recomp_env_set(RENV_PB_BACKEND, "d3d11");
    CHECK(pb_backend_is("d3d11"));
    /* A d3d11 run is, and stays so. */
    pb_backend_demote("test");
    CHECK(pb_backend_is("cpu"));
    CHECK(!pb_backend_is("d3d11"));
    pb_backend_demote("again");   /* said once */
    recomp_env_set(RENV_PB_BACKEND, "metal");
    CHECK(pb_backend_is("metal"));

    printf("pb_backend: ok (default %s)\n", dflt);
    return 0;
}
