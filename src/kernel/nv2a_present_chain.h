/*
 * nv2a_present_chain.h - present.shader: a RetroArch slang preset run by
 * librashader in place of the final present draw (Metal and D3D11).
 *
 * The library is loaded at run time, never linked: librashader's D3D11
 * runtime does not build with MinGW, so the Windows exe calls the upstream
 * MSVC dll through plain C and COM pointers, and the macOS dylib goes the
 * same way so there is one code path. A missing or mismatched library turns
 * the feature off with one log line; the present stays stock.
 *
 * This module owns the request (the key, or the menu's live choice), the
 * library and the preset path. Each backend owns its filter chain: it
 * compares nv2a_present_chain_gen() with the generation its chain was built
 * for on every present, and rebuilds when it moved. Nothing here loads while
 * the request is "none", so the stock path costs one load of an int.
 */
#ifndef NV2A_PRESENT_CHAIN_H
#define NV2A_PRESENT_CHAIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The presets the toolkit bundles (assets/shaders), in menu order after
 * "none". A request may also be a path to any .slangp. One list: the menu's
 * choices are built from the macro, so the two cannot drift. */
#define NV2A_PRESENT_CHAIN_NAMES "crt-lottes", "crt-lottes-fast", "crt-hyllian", "fsr"
extern const char *const nv2a_present_chain_names[];

/* The request: "none" (or NULL, or empty) for the stock present, a bundled
 * name, or a .slangp path (absolute, or relative to the executable's
 * directory). Moves the generation; any thread. */
void nv2a_present_chain_request(const char *shader);

/* The library path override (present.shader_lib); NULL or empty: the
 * default beside the executable. Before the first present. */
void nv2a_present_chain_set_lib(const char *path);

/* The current request, copied into out ("none" when off; a profile's
 * preset path for a profile request). */
void nv2a_present_chain_requested(char *out, size_t cap);

/* ---- Profile requests (present.profile, shader_profile.h) ----
 *
 * A profile is requested as one preset file (the composed .slangp, or a
 * one-stage profile's own preset) plus what the store cannot learn from the
 * library: librashader lists a chain's parameters with no pass index, so
 * the caller hands in each stage's preset key and the names its preset
 * declares (nv2a_present_chain_describe on the stage alone, at compose
 * time). Publish tags each row by name from those lists. Its values live in
 * the profile scope, keyed by the slug, never in the per-preset overrides. */
#define NV2A_PRESENT_STAGES_MAX 8
#define NV2A_PRESENT_PROFILE_ERR_MAX 160
/* Rows in the active parameter table (below), and names per stage. */
#define NV2A_PRESENT_PARAMS_MAX 128

struct nv2a_present_stage {
    char key[64];                     /* the stage's preset key */
    int nnames;
    char name[NV2A_PRESENT_PARAMS_MAX][64];   /* the parameters it declares */
};

/* Request profile slug, run as the preset at path (absolute), with
 * stages[0..nstages) (past NV2A_PRESENT_STAGES_MAX the first 8). Moves the
 * generation and clears the profile error, as nv2a_present_chain_request
 * does; the store copies stages. */
void nv2a_present_chain_request_profile(const char *slug, const char *path,
                                        const struct nv2a_present_stage *stages, int nstages);

/* Request profile slug before its chain can be composed: at start the
 * composed file goes under the shader cache, which the path module sets up
 * after the enhancements layer has read the config. The request is a
 * profile (requested_profile says so, wanted is 1) with no preset yet; the
 * first nv2a_present_chain_gen() after it runs prepare once (again when it
 * rearms), on that caller's thread (the backend's present), and prepare
 * makes the real request (request_profile, or "none" with the profile error). Any other
 * request first drops prepare unrun. */
void nv2a_present_chain_request_pending(const char *slug, void (*prepare)(void));

/* From a pending start's prepare, which cannot make its request yet (the
 * boot can present before the path module is up, so the shader cache has
 * no folder): run prepare again at the next nv2a_present_chain_gen(), but
 * only while the generation is still gen. Keeps the generation, so the
 * start stays pending. 1 rearmed, 0 when another request came first. */
int nv2a_present_chain_request_rearm(unsigned gen, void (*prepare)(void));

/* The generation as it stands, without running a pending prepare. */
unsigned nv2a_present_chain_request_gen(void);

/* A pending start's real request, from its prepare: as request_profile
 * (path "none" or NULL: stock with err as the profile error), but only
 * while the generation is still gen, read right after request_pending. A
 * menu request made meanwhile wins: 1 stored, 0 dropped. */
int nv2a_present_chain_request_profile_at(unsigned gen, const char *slug, const char *path,
                                          const struct nv2a_present_stage *stages, int nstages,
                                          const char *err);

/* 1 with the slug in out when the request is a profile; 0 with "" else. */
int nv2a_present_chain_requested_profile(char *out, size_t cap);

/* The profile error: why the chosen profile presents stock (a parse error,
 * a missing stage, a clash, a limit, or a chain that failed to build). The
 * profile module sets it after requesting stock; a build failure of a
 * profile request sets it in nv2a_present_chain_failed. Cleared by the next
 * request. get: 1 with the text, 0 with "" when there is none. */
void nv2a_present_chain_profile_error_set(const char *msg);
int nv2a_present_chain_profile_error_get(char *out, size_t cap);

/* Moves on every request and on nv2a_present_chain_failed(); 0 before the
 * first request. Runs a pending request's prepare first. */
unsigned nv2a_present_chain_gen(void);

/* 1 when a shader is requested and has not failed for this generation:
 * the backend should run (or build) its chain. One atomic load. */
int nv2a_present_chain_wanted(void);

/* The library, loaded on first use: NULL when it is missing or its ABI
 * does not match (said once per path). */
struct nv2a_lrs {
    void *(*sym)(const char *name);   /* any librashader export, or NULL */
    unsigned abi, api;
};
const struct nv2a_lrs *nv2a_present_chain_lib(void);

/* The presenter that runs chains (Metal's, D3D11's) says so once it is up.
 * One that has none never does: the SDL renderer, or Windows' GDI window
 * under the CPU rasteriser. name: for the log ("Metal", "D3D11"). */
void nv2a_present_chain_host(const char *name);

/* 1 when a shader can run here: a chain presenter is up and the library
 * loads (loaded now if it was not tried yet). 0 with the reason in why. It
 * does not look at the request: "none" is still supported. Any thread. */
int nv2a_present_chain_supported(char *why, size_t cap);

/* The preset file for the current request into out. Returns 1 found,
 * 0 for "none", -1 not found (said, with what was tried). gen_out gets the
 * generation the path belongs to. */
int nv2a_present_chain_preset(char *out, size_t cap, unsigned *gen_out);

/* The backend could not build the chain for generation gen (said by the
 * backend): wanted() is 0 until the next request, so a broken preset is
 * not retried every frame. For a profile request it also sets the profile
 * error, naming the profile and the composed file. A start still waiting
 * to compose (pending again after a rearm) has no preset yet and does not
 * count. */
void nv2a_present_chain_failed(unsigned gen);

/* The rate the chain's frames arrive at, for the presets' frame-rate
 * uniform (librashader's frames_per_second): a moving average of the
 * intervals between calls, so a game that presents at 30 says 30, not the
 * display's 60. now_ns: a monotonic time of this present
 * (xbox_HostNowNs). 60 until two presents; a pause over a second starts
 * over. The backend's present thread only. */
float nv2a_present_chain_rate(uint64_t now_ns);

/* librashader's message for an error object into out, and frees the error.
 * err is a libra_error_t. */
void nv2a_present_chain_error(void *err, char *out, size_t cap);

/* ---- Runtime parameters (present.shader_params) ----
 *
 * A preset declares parameters (#pragma parameter: name, label, range,
 * step). The backend that built a chain publishes the preset's table here;
 * the menu and the config set per-preset overrides; the backend follows
 * nv2a_present_chain_params_gen() with the chain's set_param. Overrides are
 * keyed by preset (the bundled name, or a path's file stem) so each preset
 * keeps its own. A value equal to the preset's own removes the override,
 * so the config only ever holds differences. */
#define NV2A_PRESENT_OVERRIDES_MAX 256   /* over every preset and profile */

struct nv2a_present_param {
    char name[64];
    char desc[96];
    float initial, min, max, step;   /* initial: the preset's value */
    /* A profile's rows: the first stage that declares the name (-1 for a
     * name no stage lists, 0 for a present.shader request), and shared
     * when a later stage declares it too: one value drives both. */
    int stage;
    int shared;
};

/* The preset key of a request ("crt-lottes", or a path's sanitised stem)
 * into out; "" for "none". */
void nv2a_present_chain_preset_key(const char *request, char *out, size_t cap);

/* Read a parsed preset's parameters (preset: a libra_shader_preset_t *)
 * into out. Returns the count (0 when the library lacks the call). */
int nv2a_present_chain_params_read(void *preset, struct nv2a_present_param *out, int cap);

/* The backend built its chain for generation gen from preset (a
 * libra_shader_preset_t *): read and publish its parameters as the active
 * table. Ignored when a newer request has arrived. */
void nv2a_present_chain_params_publish(void *preset, unsigned gen);

/* The chain was released or failed: no active table. */
void nv2a_present_chain_params_clear(void);

/* Moves on every publish, clear, set and reset. */
unsigned nv2a_present_chain_params_gen(void);

/* A copy of the active table into out (cap rows) and its preset key (a
 * profile's slug for a profile request). Returns the row count; 0 with key
 * "" when no chain is up. */
int nv2a_present_chain_params(struct nv2a_present_param *out, int cap, char *key, size_t keycap);

/* 1 when the active table belongs to a profile request: its values are in
 * the profile scope. */
int nv2a_present_chain_params_profile(void);

/* The active preset's current value of name (override or initial); the
 * value is snapped to the row's range. Returns 0 when name is not active. */
int nv2a_present_chain_param_get(const char *name, float *out);

/* Set name on the active preset (clamped to its range); equal to the
 * preset's value removes the override. A profile request's table writes the
 * profile scope. Returns 0 when not active. */
int nv2a_present_chain_param_set(const char *name, float v);

/* Remove the overrides of preset key (NULL: of every preset). The preset
 * scope only: profiles reset through nv2a_present_chain_profile_reset. */
void nv2a_present_chain_params_reset(const char *key);

/* The backend applies the parameters: when the params generation moved
 * past *applied, set(ctx, name, value) for every active row (override or
 * initial), then *applied takes the generation. The backend's present
 * thread, with its chain built. */
typedef void (*nv2a_present_param_set_fn)(void *ctx, const char *name, float v);
void nv2a_present_chain_params_apply(unsigned *applied, nv2a_present_param_set_fn set, void *ctx);

/* Config: an override read at start (any layer; a later call wins). It
 * counts as saved. */
void nv2a_present_chain_override_load(const char *key, const char *name, float v);

/* Overrides for Save: row i (from 0) while it returns 1. has: the
 * override is set (else it was removed since the last save); dirty: it
 * differs from what was loaded or saved last. The preset scope only, as are
 * saved, dirty and get below: profile values go to the profile's file. */
int nv2a_present_chain_override_at(int i, char *key, size_t keycap, char *name,
                                   size_t namecap, float *v, int *has, int *dirty);

/* Save wrote the overrides: they are clean now. */
void nv2a_present_chain_overrides_saved(void);

/* The overrides that differ from what was loaded or saved. */
int nv2a_present_chain_overrides_dirty(void);

/* The override of name for preset key: 1 with *v set, 0 when none. */
int nv2a_present_chain_override_get(const char *key, const char *name, float *v);

/* ---- The profile scope: values keyed by (slug, name) ---- */

/* A value read from profile slug's file at start or when it is chosen. It
 * counts as saved. */
void nv2a_present_chain_profile_value_load(const char *slug, const char *name, float v);

/* The value of name in profile slug: 1 with *v, 0 when it has none. */
int nv2a_present_chain_profile_value_get(const char *slug, const char *name, float *v);

/* The values profile slug holds now, for its file at Save: names and
 * values into name[0..cap) and v. The count. */
int nv2a_present_chain_profile_values(const char *slug, char (*name)[64], float *v, int cap);

/* Remove profile slug's value of name (NULL: every value). Moves the
 * params generation. */
void nv2a_present_chain_profile_reset(const char *slug, const char *name);

/* The values of profile slug (NULL: of every profile) that differ from what
 * was loaded or saved. */
int nv2a_present_chain_profile_dirty(const char *slug);

/* Save wrote profile slug's file: its values are clean now. */
void nv2a_present_chain_profile_saved(const char *slug);

/* A rename: the values of from now belong to to (and the active table's
 * key, when it is from's). A delete: forget slug's values, saved or not. */
void nv2a_present_chain_profile_rekey(const char *from, const char *to);
void nv2a_present_chain_profile_forget(const char *slug);

/* Tests: drop the active table, every override and profile value, the
 * profile request and its error, as a fresh start. */
void nv2a_present_chain_params_reset_for_test(void);

/* Forget the presenter and the library (closing it), so a test can load
 * another one or none. */
void nv2a_present_chain_lib_reset_for_test(void);

/* The descriptor dump: the parameters of request (a bundled name or a
 * path) parsed from its preset, with no GPU and no chain. The count, or -1
 * when the library or the preset is missing or does not parse. */
int nv2a_present_chain_describe(const char *request, struct nv2a_present_param *out, int cap);

/* The menu's "Exit without saving" (overlay-revamp D9a): every preset-scope
 * override back to what was loaded or saved last; one added since goes.
 * Moves the params generation, so the backend applies the values again. */
void nv2a_present_chain_override_revert(void);

/* The same for profile slug's values (NULL: every profile's). */
void nv2a_present_chain_profile_revert(const char *slug);

/* 1 when the active table's parameter name differs from its saved value:
 * the menu's unsaved mark on that row (D10). */
int nv2a_present_chain_param_dirty(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_PRESENT_CHAIN_H */
