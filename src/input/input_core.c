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
 * A backend reads the host pad raw (InputRaw, pad_map.h's adapter); the
 * pad map ([input.pad], xbox_InputSetPadSetup, the identity by default) is
 * applied here, so it reaches every title that reads its pads through this
 * API.
 *
 * Nothing here calls the platform: the backend (input_backend.h) does, and
 * tests/pad_map swaps in a fake one to count the calls.
 */
#include "input_backend.h"
#include "host_pad.h"
#include "keyboard.h"
#include "pad_map.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define spin_yield() SwitchToThread()
#else
#include <sched.h>
#define spin_yield() sched_yield()
#endif

static const xbox_input_backend *g_be = &xbox_input_host_backend;
static int      g_inited;
static BOOL     g_connected[XBOX_MAX_CONTROLLERS];
static uint64_t g_last_probe[XBOX_MAX_CONTROLLERS];
static int      g_probed[XBOX_MAX_CONTROLLERS];   /* ever probed */
static int      g_rumbling[XBOX_MAX_CONTROLLERS];

/* The host-only buttons each slot's last read saw (host_pad.h): written by
 * the poll and by the flip tick's read, read by either and by a title's
 * glue, so atomic. */
static uint32_t g_extra[XBOX_MAX_CONTROLLERS];
/* The host actions' filter (host_pad.h); atomic, NULL when disarmed. */
static xbox_input_pad_filter_fn g_filter;
/* The mod layer's record (host_pad.h); atomic, NULL without one. */
static xbox_input_seen_fn g_seen;
/* input.glyphs (XBOX_PAD_FAMILY_*); atomic. */
static int g_glyphs = XBOX_PAD_FAMILY_AUTO;

/* Spin locks held for a struct copy. The holder may be preempted, and on a
 * busy host (Proton's threads plus the guest's) a waiter that never yields
 * can keep it off the core. */
static void spin_lock(unsigned char *l)
{
    unsigned spins = 0;

    while (__atomic_test_and_set(l, __ATOMIC_ACQUIRE))
        if (++spins >= 64) {
            spins = 0;
            spin_yield();
        }
}

static void spin_unlock(unsigned char *l)
{
    __atomic_clear(l, __ATOMIC_RELEASE);
}

/* The raw host pad each slot's last read saw (xbox_InputHostRaw): the poll
 * and the flip tick write it, a title's glue reads it. */
static InputRaw g_raw[XBOX_MAX_CONTROLLERS];
static unsigned char g_raw_lock;   /* the type __atomic_test_and_set is defined on */

static void publish_raw(DWORD slot, const InputRaw *raw)
{
    spin_lock(&g_raw_lock);
    g_raw[slot] = *raw;
    spin_unlock(&g_raw_lock);
    __atomic_store_n(&g_extra[slot], xbox_PadRawExtra(raw), __ATOMIC_RELEASE);
}

/* The pad map in force: NULL is the identity. A setup is never freed once
 * installed, because a poll on another thread may still be reading it; a
 * player changes the map by hand, so the few bytes per change stay small. */
struct pad_cfg {
    xbox_pad_setup setup;
    int extras;              /* xbox_PadSetupNeedsExtras */
};
static const struct pad_cfg *g_cfg;   /* atomic */

void xbox_InputSetPadSetup(const xbox_pad_setup *s)
{
    struct pad_cfg *c = NULL;

    if (s && !xbox_PadSetupIsIdentity(s)) {
        if (!(c = malloc(sizeof *c)))
            return;
        c->setup = *s;
        c->extras = xbox_PadSetupNeedsExtras(s);
    }
    __atomic_store_n(&g_cfg, c, __ATOMIC_RELEASE);
    /* The keys' diagonal follows the squared stick, or a ramped W + D
     * would stay short of the corner the pad now reaches. A second atomic,
     * not part of g_cfg: the setters (config load, the menu) never run at
     * once, so the two cannot disagree past one poll. */
    xbox_InputKeyboardSquareLeft(s && s->square_left);
}

void xbox_InputPadSetup(xbox_pad_setup *out)
{
    const struct pad_cfg *c = __atomic_load_n(&g_cfg, __ATOMIC_ACQUIRE);

    if (c)
        *out = c->setup;
    else
        xbox_PadSetupIdentity(out);
}

/* raw through the setup in force: inverted in place, then mapped. */
static void map_raw(const struct pad_cfg *c, InputRaw *raw, XBOX_GAMEPAD *out)
{
    xbox_pad_setup id;
    const xbox_pad_setup *s = c ? &c->setup : &id;

    if (!c)
        xbox_PadSetupIdentity(&id);
    xbox_PadRawInvert(raw, s->invert_ly, s->invert_ry);
    xbox_PadSetupApply(s, raw, out);
}

static void shutdown_atexit(void) { xbox_InputShutdown(); }

/* Read port. On success *st is the mapped pad; *raw is always written (zero
 * when no pad answered). */
static DWORD probe(DWORD port, XBOX_INPUT_STATE *st, InputRaw *raw)
{
    const struct pad_cfg *c = __atomic_load_n(&g_cfg, __ATOMIC_ACQUIRE);
    /* The host-only buttons only for the armed host actions or a map that
     * names one: a stock run keeps the documented XInputGetState. */
    int extras = xbox_InputPadFilterSet() || (c && c->extras);
    DWORD packet = 0;
    DWORD rc;

    memset(raw, 0, sizeof *raw);
    rc = g_be->get_raw(port, raw, &packet, extras);
    g_last_probe[port] = g_be->now_ms();
    g_probed[port] = 1;
    g_connected[port] = (rc == ERROR_SUCCESS);
    if (rc == ERROR_SUCCESS) {
        map_raw(c, raw, &st->Gamepad);
        st->dwPacketNumber = packet;
    } else {
        memset(raw, 0, sizeof *raw);
    }
    publish_raw(port, raw);
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
    InputRaw raw;

    if (g_inited)
        return;
    __atomic_store_n(&g_inited, 1, __ATOMIC_RELEASE);
    if (g_be->init)
        g_be->init();
    /* The keyboard's stick ramp and the wheel pulses run on the poll's
     * clock, the backend's, so a test's fake clock drives them too. */
    xbox_InputKbmSetClock(g_be->now_ms);
    for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        probe(p, &st, &raw);
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
    for (int p = 0; p < XBOX_MAX_CONTROLLERS; p++) {
        static const InputRaw none;
        publish_raw((DWORD)p, &none);
    }
}

void xbox_InputSetPadFilter(xbox_input_pad_filter_fn fn)
{
    __atomic_store_n(&g_filter, fn, __ATOMIC_RELEASE);
}

void xbox_InputSetPadSeen(xbox_input_seen_fn fn)
{
    __atomic_store_n(&g_seen, fn, __ATOMIC_RELEASE);
}

int xbox_InputPadFilterSet(void)
{
    return __atomic_load_n(&g_filter, __ATOMIC_ACQUIRE) != NULL;
}

uint32_t xbox_InputHostExtra(DWORD slot)
{
    if (slot >= XBOX_MAX_CONTROLLERS)
        return 0;
    return __atomic_load_n(&g_extra[slot], __ATOMIC_ACQUIRE);
}

void xbox_InputHostRaw(DWORD slot, InputRaw *raw)
{
    if (slot >= XBOX_MAX_CONTROLLERS) {
        memset(raw, 0, sizeof *raw);
        return;
    }
    spin_lock(&g_raw_lock);
    *raw = g_raw[slot];
    spin_unlock(&g_raw_lock);
}

void xbox_InputSetGlyphFamily(int family)
{
    if (family < XBOX_PAD_FAMILY_AUTO || family >= XBOX_PAD_FAMILY_COUNT)
        family = XBOX_PAD_FAMILY_AUTO;
    __atomic_store_n(&g_glyphs, family, __ATOMIC_RELEASE);
}

int xbox_InputPadFamily(DWORD slot)
{
    int f = __atomic_load_n(&g_glyphs, __ATOMIC_ACQUIRE);

    if (f != XBOX_PAD_FAMILY_AUTO)
        return f;
    if (slot >= XBOX_MAX_CONTROLLERS || !g_be->family)
        return XBOX_PAD_FAMILY_XBOX;
    return g_be->family(slot);
}

int xbox_InputSteamInput(DWORD slot)
{
    if (slot >= XBOX_MAX_CONTROLLERS || !g_be->steam_input)
        return XBOX_STEAM_INPUT_OFF;
    return g_be->steam_input(slot);
}

static const char *const k_family[XBOX_PAD_FAMILY_COUNT] = {
    "xbox", "playstation", "nintendo", "steam", "generic", "gamecube",
};

const char *xbox_InputPadFamilyName(int family)
{
    return family >= 0 && family < XBOX_PAD_FAMILY_COUNT ? k_family[family] : "auto";
}

int xbox_InputPadFamilyFromName(const char *name)
{
    if (!name)
        return -2;
    if (!strcmp(name, "auto"))
        return XBOX_PAD_FAMILY_AUTO;
    for (int f = 0; f < XBOX_PAD_FAMILY_COUNT; f++)
        if (!strcmp(name, k_family[f]))
            return f;
    return -2;
}

uint64_t xbox_InputNowMs(void)
{
    return g_be && g_be->now_ms ? g_be->now_ms() : 0;
}

DWORD xbox_input_backend_read_host(DWORD slot, struct xbox_host_pad *host)
{
    InputRaw raw;
    DWORD rc = ERROR_DEVICE_NOT_CONNECTED;

    if (slot >= XBOX_MAX_CONTROLLERS || !host)
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(&raw, 0, sizeof raw);
    /* Before the title's first poll the backend is not up, and bringing it
     * up is the poll thread's job (the SDL boot wait). */
    if (__atomic_load_n(&g_inited, __ATOMIC_ACQUIRE) && g_be->read_raw)
        rc = g_be->read_raw(slot, &raw);
    if (rc == XBOX_INPUT_BUSY)
        return rc;
    memset(host, 0, sizeof *host);
    host->slot = (int)slot;
    if (rc == ERROR_SUCCESS) {
        map_raw(__atomic_load_n(&g_cfg, __ATOMIC_ACQUIRE), &raw, &host->pad.Gamepad);
        host->raw = raw;
        host->extra = xbox_PadRawExtra(&raw);
        host->connected = 1;
    }
    publish_raw(slot, &host->raw);
    return rc;
}

static DWORD pad_state(DWORD slot, XBOX_INPUT_STATE *pState, InputRaw *raw)
{
    xbox_InputInit();
    if (due(slot))
        return probe(slot, pState, raw);
    memset(raw, 0, sizeof *raw);
    return ERROR_DEVICE_NOT_CONNECTED;
}

DWORD xbox_InputGetPadState(DWORD slot, XBOX_INPUT_STATE *pState)
{
    InputRaw raw;

    if (slot >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    return pad_state(slot, pState, &raw);
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

#ifdef XBOXRECOMP_ENHANCE
/* Port 0 as the title's last read saw it, for a mod (xbox_InputLastState).
 * The title's poll writes it (the USB thread) and a mod reads it from the
 * guest's frame, so it sits behind a spin lock held for a copy. */
static XBOX_INPUT_STATE g_last;
static DWORD g_last_rc = ERROR_DEVICE_NOT_CONNECTED;
static unsigned char g_last_lock;

static void last_lock(void)   { spin_lock(&g_last_lock); }
static void last_unlock(void) { spin_unlock(&g_last_lock); }

static DWORD keep_last(DWORD rc, const XBOX_INPUT_STATE *st)
{
    last_lock();
    g_last_rc = rc;
    if (rc == ERROR_SUCCESS)
        g_last = *st;
    else
        memset(&g_last, 0, sizeof g_last);
    last_unlock();
    return rc;
}

DWORD xbox_InputLastState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    DWORD rc;

    if (dwPort != 0 || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    last_lock();
    rc = g_last_rc;
    *pState = g_last;
    last_unlock();
    return rc;
}

/* Port 0's neutral pad (host_pad.h, xbox_InputSetPadSeen): on while the
 * record says a mod holds it. The title reads edges by packet number (see
 * xbox_InputMergeKbm), so a state that turns to zero under the same number
 * would read as still held: the number moves by one more each time the
 * neutral pad starts or ends, and stays moved. The title's poll thread
 * only. */
static int g_neutral;
static DWORD g_neutral_bump;

static DWORD finish0(DWORD rc, XBOX_INPUT_STATE *st)
{
    static const XBOX_GAMEPAD none;
    xbox_input_seen_fn seen = __atomic_load_n(&g_seen, __ATOMIC_ACQUIRE);
    int held;

    /* What the title would have read, for xbox_InputLastState and the
     * record: the fly input stays readable while the title gets zeros. */
    keep_last(rc, st);
    held = seen && seen(0, rc == ERROR_SUCCESS ? &st->Gamepad : &none, rc == ERROR_SUCCESS);
    /* A failed poll has no pad to neutralise and is left as it is. */
    if (rc != ERROR_SUCCESS)
        return rc;
    if (held != g_neutral) {
        g_neutral = held;
        g_neutral_bump++;
    }
    if (held)
        memset(&st->Gamepad, 0, sizeof st->Gamepad);
    st->dwPacketNumber += g_neutral_bump;
    return rc;
}
#define KEEP(port, rc, st) ((port) == 0 ? finish0((rc), (st)) : (rc))

void xbox_InputKeepLastState(DWORD dwPort, DWORD rc, const XBOX_INPUT_STATE *pState)
{
    if (dwPort == 0 && pState)
        keep_last(rc, pState);
}
#else
#define KEEP(port, rc, st) (rc)

void xbox_InputKeepLastState(DWORD dwPort, DWORD rc, const XBOX_INPUT_STATE *pState)
{
    (void)dwPort; (void)rc; (void)pState;
}
#endif

#ifdef XBOXRECOMP_ENHANCE
/* The host actions see every poll, a failed one too (with a released
 * state), so a key or a chord released while the pad was away is seen
 * released. */
static DWORD filtered(DWORD port, DWORD rc, XBOX_INPUT_STATE *st,
                      xbox_input_pad_filter_fn fn, const struct xbox_host_pad *host)
{
    XBOX_INPUT_STATE idle;

    if (!fn)
        return rc;
    if (rc == ERROR_SUCCESS) {
        fn(port, host, st);
    } else {
        memset(&idle, 0, sizeof idle);
        fn(port, host, &idle);
    }
    return rc;
}
#define FILTER(port, rc, st) filtered((port), (rc), (st), fn, &host)
#else
#define FILTER(port, rc, st) (rc)
#endif

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb;
    InputRaw raw;
    DWORD rc;
#ifdef XBOXRECOMP_ENHANCE
    xbox_input_pad_filter_fn fn;
    struct xbox_host_pad host;
#endif

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    rc = pad_state(dwPort, pState, &raw);
#ifdef XBOXRECOMP_ENHANCE
    /* The host pad before the keyboard and the script are merged: a chord is
     * the pad's, a key the keyboard's (the filter reads keys itself). */
    fn = __atomic_load_n(&g_filter, __ATOMIC_ACQUIRE);
    if (fn) {
        memset(&host, 0, sizeof host);
        host.slot = (int)dwPort;
        if (rc == ERROR_SUCCESS) {
            host.pad = *pState;
            host.raw = raw;
            host.extra = xbox_PadRawExtra(&raw);
            host.connected = 1;
        }
    }
#endif

    /* The keyboard answers for port 0 with or without a pad there, and is
     * merged on top of one: a real pad keeps working, the keys only add. */
    if (dwPort == 0 && g_be->keyboard && g_be->keyboard(&kb)) {
        if (rc != ERROR_SUCCESS) {
            *pState = kb;
            return KEEP(dwPort, FILTER(dwPort, ERROR_SUCCESS, pState), pState);
        }
        xbox_InputMergeKbm(pState, &kb);
    }
    return KEEP(dwPort, FILTER(dwPort, rc, pState), pState);
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
    InputRaw raw;

    if (dwPort >= XBOX_MAX_CONTROLLERS)
        return FALSE;
    xbox_InputInit();
    /* Connected ports are refreshed by the title's own reads; only a port
     * not read for a probe interval is asked here. */
    if (g_be->now_ms() - g_last_probe[dwPort] >= XBOX_INPUT_PROBE_MS)
        probe(dwPort, &st, &raw);
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
