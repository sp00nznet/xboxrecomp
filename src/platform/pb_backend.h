/**
 * The pushbuffer executor's backend for this run, chosen in one place.
 *
 * RECOMP_PB_BACKEND names it when set and not empty; otherwise the build's
 * default: d3d11 in the dxvk-native Linux build (XBOXRECOMP_DXVK_NATIVE,
 * where it is the reason for the build), cpu everywhere else. The
 * executor's registration, the presenter's window and the menu all ask
 * here, so an unset variable cannot mean d3d11 to one and cpu to another.
 *
 * pb_backend_demote turns a d3d11 run into a cpu run before the executor's
 * first method, when the window the D3D11 backend needs cannot exist (the
 * Vulkan window failed, SDL video is unavailable, RECOMP_HEADLESS): the
 * frame is then drawn, not left black.
 */
#ifndef PB_BACKEND_H
#define PB_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

/* "d3d11" in the dxvk-native build, else "cpu". */
const char *pb_backend_default(void);

/* The backend for env (RECOMP_PB_BACKEND's value, may be NULL) with
 * dflt as the default and demoted set by pb_backend_demote: env when set
 * and not empty, else dflt; "cpu" instead of a d3d11 either way when
 * demoted. The returned text is env's own (any case) or a literal. Pure,
 * for the test. */
const char *pb_backend_resolve(const char *env, const char *dflt, int demoted);

/* The backend for this run (pb_backend_resolve over RECOMP_PB_BACKEND). */
const char *pb_backend(void);

/* Whether this run's backend is name, in any case ("d3d11", "metal", ...). */
int pb_backend_is(const char *name);

/* A d3d11 run becomes a cpu run, with one line saying why. A no-op for any
 * other backend. Called on the main thread before the guest starts. */
void pb_backend_demote(const char *why);

#ifdef __cplusplus
}
#endif

#endif /* PB_BACKEND_H */
