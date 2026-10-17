/**
 * xbox_input -- the public API over a host backend, with the per-port
 * connection cache.
 *
 * Connected ports are read at every call. A port last seen disconnected is
 * not asked again until XBOX_INPUT_PROBE_MS has passed since the last probe:
 * XInput's own guidance, because XInputGetState on an empty user index is
 * slow on Windows, and harmless elsewhere. xbox_InputIsConnected() answers
 * from the cache and refreshes it under the same rule, so a pad plugged in
 * later shows up within a second while the title polls the mask every frame.
 *
 * Nothing here calls the platform: the backend (input_backend.h) does, and
 * tests/input_map swaps in a fake one to count the calls.
 */
#include "input_backend.h"
#include "keyboard.h"

#include <stdlib.h>
#include <string.h>

static const xbox_input_backend *g_be = &xbox_input_host_backend;
static int      g_inited;
static BOOL     g_connected[XBOX_MAX_CONTROLLERS];
static uint64_t g_last_probe[XBOX_MAX_CONTROLLERS];
static int      g_probed[XBOX_MAX_CONTROLLERS];   /* ever probed */
static int      g_rumbling[XBOX_MAX_CONTROLLERS];

static void shutdown_atexit(void) { xbox_InputShutdown(); }

static DWORD probe(DWORD port, XBOX_INPUT_STATE *st)
{
    DWORD rc = g_be->get_state(port, st);
    g_last_probe[port] = g_be->now_ms();
    g_probed[port] = 1;
    g_connected[port] = (rc == ERROR_SUCCESS);
    return rc;
}

/* Whether a probe of a port is due. */
static int due(DWORD port)
{
    return !g_probed[port] || g_connected[port]
        || g_be->now_ms() - g_last_probe[port] >= XBOX_INPUT_PROBE_MS;
}

void xbox_InputInit(void)
{
    static int atexit_done;
    XBOX_INPUT_STATE st;

    if (g_inited)
        return;
    g_inited = 1;
    if (g_be->init)
        g_be->init();
    /* The keyboard's stick ramp and the wheel pulses run on the poll's
     * clock, the backend's, so a test's fake clock drives them too. */
    xbox_InputKbmSetClock(g_be->now_ms);
    for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        probe(p, &st);
    /* A crash or an exit while the title has the motors on must not leave a
     * pad buzzing. */
    if (!atexit_done && g_be == &xbox_input_host_backend) {
        atexit_done = 1;
        atexit(shutdown_atexit);
    }
}

void xbox_InputSetBackend(const xbox_input_backend *b)
{
    g_be = b ? b : &xbox_input_host_backend;
    g_inited = 0;
    memset(g_connected, 0, sizeof g_connected);
    memset(g_last_probe, 0, sizeof g_last_probe);
    memset(g_probed, 0, sizeof g_probed);
    memset(g_rumbling, 0, sizeof g_rumbling);
}

DWORD xbox_InputGetPadState(DWORD slot, XBOX_INPUT_STATE *pState)
{
    if (slot >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    xbox_InputInit();
    return due(slot) ? probe(slot, pState) : ERROR_DEVICE_NOT_CONNECTED;
}

void xbox_InputMergeKbm(XBOX_INPUT_STATE *pad, const XBOX_INPUT_STATE *kb)
{
    XBOX_GAMEPAD *g = &pad->Gamepad;

    g->wButtons |= kb->Gamepad.wButtons;
    for (int i = 0; i < 8; i++)
        if (kb->Gamepad.bAnalogButtons[i] > g->bAnalogButtons[i])
            g->bAnalogButtons[i] = kb->Gamepad.bAnalogButtons[i];
    if (kb->Gamepad.sThumbLX) g->sThumbLX = kb->Gamepad.sThumbLX;
    if (kb->Gamepad.sThumbLY) g->sThumbLY = kb->Gamepad.sThumbLY;
    if (kb->Gamepad.sThumbRX) g->sThumbRX = kb->Gamepad.sThumbRX;
    if (kb->Gamepad.sThumbRY) g->sThumbRY = kb->Gamepad.sThumbRY;
    /* The title's input layer records edges, so an unchanged packet
     * number reads as the same state and a key press never happens. */
    pad->dwPacketNumber = kb->dwPacketNumber;
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb;
    DWORD rc;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    rc = xbox_InputGetPadState(dwPort, pState);

    /* The keyboard answers for port 0 with or without a pad there, and is
     * merged on top of one: a real pad keeps working, the keys only add. */
    if (dwPort == 0 && g_be->keyboard && g_be->keyboard(&kb)) {
        if (rc != ERROR_SUCCESS) {
            *pState = kb;
            return ERROR_SUCCESS;
        }
        xbox_InputMergeKbm(pState, &kb);
    }
    return rc;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    DWORD rc;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;
    xbox_InputInit();
    if (!g_connected[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;
    rc = g_be->set_state(dwPort, pVibration);
    if (rc == ERROR_SUCCESS)
        g_rumbling[dwPort] = pVibration->wLeftMotorSpeed
                          || pVibration->wRightMotorSpeed;
    return rc;
}

void xbox_InputShutdown(void)
{
    static const XBOX_VIBRATION off = { 0, 0 };

    if (!g_inited)
        return;
    for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        if (g_rumbling[p] || g_connected[p]) {
            g_be->set_state(p, &off);
            g_rumbling[p] = 0;
        }
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    XBOX_INPUT_STATE st;

    if (dwPort >= XBOX_MAX_CONTROLLERS)
        return FALSE;
    xbox_InputInit();
    /* Connected ports are refreshed by the title's own reads; only a port
     * not read for a probe interval is asked here. */
    if (g_be->now_ms() - g_last_probe[dwPort] >= XBOX_INPUT_PROBE_MS)
        probe(dwPort, &st);
    return g_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!xbox_InputIsConnected(dwPort))
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(pCaps, 0, sizeof *pCaps);
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    return ERROR_SUCCESS;
}
