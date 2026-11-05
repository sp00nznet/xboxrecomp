/*
 * fake_lrs.c - a stand-in for librashader with only the version exports,
 * built once per (ABI, API) pair the loader must accept or refuse.
 * FAKE_PARAMS adds the preset calls the parameter store and the descriptor
 * dump use, with fixed tables (a profile's stages and their composed chain
 * among them), so they run without the real library or a GPU.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

#ifdef FAKE_NO_EXPORTS
EXPORT int fake_lrs_unrelated(void) { return 0; }
#else
EXPORT size_t libra_instance_abi_version(void) { return FAKE_ABI; }
EXPORT size_t libra_instance_api_version(void) { return FAKE_API; }
#endif

#ifdef FAKE_PARAMS
/* Mirrors libra_preset_param_t and libra_preset_param_list_t. */
struct fake_param {
    const char *name, *description;
    float initial, minimum, maximum, step;
};
struct fake_list {
    const struct fake_param *parameters;
    uint64_t length;
};

/* The preset a handle stands for, by the file it was created from: a
 * profile's two stages, their composed chain, one past the store's cap, or
 * the fixed table for any other file (and for a NULL handle). */
enum { K_PLAIN, K_STAGE_A, K_STAGE_B, K_COMPOSED, K_MANY, K_N };
static int s_kind[K_N] = { K_PLAIN, K_STAGE_A, K_STAGE_B, K_COMPOSED, K_MANY };
static int s_broken = 1;
static const struct fake_param s_table[] = {
    {"warpX", "warpX", 0.01f, 0.0f, 0.125f, 0.01f},
    {"maskDark", "Lottes maskDark", 0.5f, 0.0f, 2.0f, 0.05f},
};
/* Stage a declares A1 and SHARED, stage b SHARED and B1. librashader lists
 * a shared name once, so the composed chain has A1 SHARED B1, plus a name
 * neither stage lists (a stage that changed on disk since the compose). */
static const struct fake_param s_a[] = {
    {"A1", "a one", 1.0f, 0.0f, 2.0f, 0.1f},
    {"SHARED", "shared", 0.5f, 0.0f, 1.0f, 0.05f},
};
static const struct fake_param s_b[] = {
    {"SHARED", "shared", 0.5f, 0.0f, 1.0f, 0.05f},
    {"B1", "b one", 3.0f, 0.0f, 4.0f, 0.5f},
};
static const struct fake_param s_composed[] = {
    {"A1", "a one", 1.0f, 0.0f, 2.0f, 0.1f},
    {"SHARED", "shared", 0.5f, 0.0f, 1.0f, 0.05f},
    {"B1", "b one", 3.0f, 0.0f, 4.0f, 0.5f},
    {"ORPHAN", "orphan", 0.0f, 0.0f, 1.0f, 0.1f},
};
#define MANY 150
static struct fake_param s_many[MANY];
static char s_many_name[MANY][8];

/* A file whose name has "broken" in it does not parse: an error object
 * (any non-NULL pointer; there is no libra_error_write, so the loader says
 * "unknown error"). */
EXPORT void *libra_preset_create(const char *filename, void **out)
{
    int k = K_PLAIN;
    if (strstr(filename, "broken"))
        return &s_broken;
    if (strstr(filename, "stage_a"))
        k = K_STAGE_A;
    else if (strstr(filename, "stage_b"))
        k = K_STAGE_B;
    else if (strstr(filename, "composed"))
        k = K_COMPOSED;
    else if (strstr(filename, "many"))
        k = K_MANY;
    *out = &s_kind[k];
    return NULL;
}
EXPORT void *libra_preset_free(void **preset)
{
    *preset = NULL;
    return NULL;
}
EXPORT void *libra_preset_get_runtime_params(const void *preset, struct fake_list *out)
{
    const int *h = preset ? *(const int *const *)preset : NULL;
    int k = h ? *h : K_PLAIN, i;
#define SET(t) (out->parameters = (t), out->length = sizeof(t) / sizeof((t)[0]))
    switch (k) {
    case K_STAGE_A: SET(s_a); break;
    case K_STAGE_B: SET(s_b); break;
    case K_COMPOSED: SET(s_composed); break;
    case K_MANY:
        for (i = 0; i < MANY; i++) {
            snprintf(s_many_name[i], sizeof s_many_name[i], "m%d", i);
            s_many[i].name = s_many[i].description = s_many_name[i];
            s_many[i].initial = 0.0f;
            s_many[i].maximum = 1.0f;
            s_many[i].step = 0.1f;
        }
        out->parameters = s_many;
        out->length = MANY;
        break;
    default: SET(s_table); break;
    }
#undef SET
    return NULL;
}
EXPORT void *libra_preset_free_runtime_params(struct fake_list list)
{
    (void)list;
    return NULL;
}
#endif
