/**
 * xbox_input -- the seam between the public API (input_core.c) and a host
 * backend (xinput_device.c: XInput on Windows, the SDL3 gamepad API
 * elsewhere). Internal to src/input and its tests.
 *
 * input_core.c owns the per-port connection cache and the probe backoff;
 * a backend only answers "what is on port N now". tests/pad_map installs
 * a fake backend with xbox_InputSetBackend() and counts the calls.
 */
#ifndef XBOX_INPUT_BACKEND_H
#define XBOX_INPUT_BACKEND_H

#include <stdint.h>
#include "xinput_xbox.h"
#include "host_pad.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xbox_input_backend {
    /* Called once from xbox_InputInit(); may be NULL. */
    void  (*init)(void);
    /* Read port's pad as the host sees it (pad_map.h's adapter fills *raw;
     * input_core.c applies the pad map). ERROR_SUCCESS with *raw and
     * *packet filled, or ERROR_DEVICE_NOT_CONNECTED. The host-only buttons
     * (Guide, the paddles; XInputGetStateEx on Windows) are read only when
     * extras is nonzero, so a stock run polls as it always did. */
    DWORD (*get_raw)(DWORD port, InputRaw *raw, DWORD *packet, int extras);
    /* Set port's motors. */
    DWORD (*set_state)(DWORD port, const XBOX_VIBRATION *vib);
    /* Port 0 keyboard, merged on top of the pad when RECOMP_KEYBOARD is on.
     * Returns 1 when the keyboard is enabled and *out was filled; NULL when
     * the backend has no keyboard. */
    int   (*keyboard)(XBOX_INPUT_STATE *out);
    /* Monotonic milliseconds. */
    uint64_t (*now_ms)(void);
    /* port's raw pad with its host-only buttons, read off the poll thread
     * (the flip tick, xbox_input_backend_read_host): ERROR_SUCCESS,
     * ERROR_DEVICE_NOT_CONNECTED or XBOX_INPUT_BUSY. NULL: not connected. */
    DWORD (*read_raw)(DWORD port, InputRaw *raw);
    /* The controller family (XBOX_PAD_FAMILY_*) and Steam Input state
     * (XBOX_STEAM_INPUT_*) of the pad on port; NULL: xbox and off. */
    int   (*family)(DWORD port);
    int   (*steam_input)(DWORD port);
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
