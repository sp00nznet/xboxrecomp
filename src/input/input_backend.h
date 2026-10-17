/**
 * xbox_input -- the seam between the public API (input_core.c) and a host
 * backend (xinput_device.c: XInput on Windows, SDL2 GameController
 * elsewhere). Internal to src/input and its tests.
 *
 * input_core.c owns the per-port connection cache and the probe backoff;
 * a backend only answers "what is on port N now". tests/input_map installs
 * a fake backend with xbox_InputSetBackend() and counts the calls.
 */
#ifndef XBOX_INPUT_BACKEND_H
#define XBOX_INPUT_BACKEND_H

#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xbox_input_backend {
    /* Called once from xbox_InputInit(); may be NULL. */
    void  (*init)(void);
    /* Read port's pad. ERROR_SUCCESS with *out filled (already mapped
     * through input_map), or ERROR_DEVICE_NOT_CONNECTED. */
    DWORD (*get_state)(DWORD port, XBOX_INPUT_STATE *out);
    /* Set port's motors. */
    DWORD (*set_state)(DWORD port, const XBOX_VIBRATION *vib);
    /* Port 0 keyboard, merged on top of the pad when RECOMP_KEYBOARD is on.
     * Returns 1 when the keyboard is enabled and *out was filled; NULL when
     * the backend has no keyboard. */
    int   (*keyboard)(XBOX_INPUT_STATE *out);
    /* Monotonic milliseconds. */
    uint64_t (*now_ms)(void);
} xbox_input_backend;

/* The host backend this build uses (xinput_device.c). */
extern const xbox_input_backend xbox_input_host_backend;

/* Swap the backend and reset the cache and the init flag (tests). NULL
 * restores the host backend. */
void xbox_InputSetBackend(const xbox_input_backend *b);

/* Disconnected ports are probed at most this often (XInput's guidance). */
#define XBOX_INPUT_PROBE_MS 1000

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_BACKEND_H */
