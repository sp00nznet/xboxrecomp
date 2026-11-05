/**
 * Xbox Input Compatibility Layer
 *
 * The host backends behind input_core.c (input_backend.h): each one reads
 * a host pad into an InputRaw through pad_map.c's adapter, so both hosts
 * name controls the same way; input_core.c applies the pad map.
 *
 *   _WIN32 -> Windows XInput (also Proton/Wine)
 *   POSIX  -> SDL3 gamepad
 */

#include "input_backend.h"
#include "recomp_env.h"
#include "pad_map.h"
#include "keyboard.h"
#include "host_pad.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

/* The keyboard on port 0 is shared with the SDL backend (keyboard.c); the
 * window procedures in fb_present.c and nv2a_pb_d3d11.c record the keys. */

_Static_assert(sizeof(XINPUT_GAMEPAD) == sizeof(XBOX_HOST_XINPUT_GAMEPAD)
               && offsetof(XINPUT_GAMEPAD, bLeftTrigger) == offsetof(XBOX_HOST_XINPUT_GAMEPAD, bLeftTrigger)
               && offsetof(XINPUT_GAMEPAD, sThumbRY) == offsetof(XBOX_HOST_XINPUT_GAMEPAD, sThumbRY),
               "XBOX_HOST_XINPUT_GAMEPAD must match XINPUT_GAMEPAD");

/* XInputGetState never reports the Guide button; the undocumented
 * XInputGetStateEx (xinput1_4 ordinal 100, also in Wine's) does, as bit
 * 0x0400. Resolved once by ordinal, since it has no name; absent, Guide
 * reads as never pressed and the F10 and chord fallbacks remain. It fills
 * XINPUT_STATE_EX, 4 bytes longer than XINPUT_STATE (Wine's and SDL's
 * definition), so it gets that struct, never an XINPUT_STATE. */
typedef struct {
    DWORD          dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
    DWORD          dwPaddingReserved;
} xi_state_ex;
_Static_assert(sizeof(xi_state_ex) == 20, "XINPUT_STATE_EX is 20 bytes");
_Static_assert(offsetof(xi_state_ex, Gamepad) == offsetof(XINPUT_STATE, Gamepad),
               "XINPUT_STATE_EX starts as XINPUT_STATE");

typedef DWORD (WINAPI *xi_get_state_ex_fn)(DWORD, xi_state_ex *);
#define XI_GUIDE 0x0400u
static xi_get_state_ex_fn g_xi_ex;     /* atomic */
static int                g_xi_ex_done;

static xi_get_state_ex_fn xi_reader(void)
{
    if (!__atomic_load_n(&g_xi_ex_done, __ATOMIC_ACQUIRE)) {
        /* Two threads may both resolve it; they find the same address. */
        HMODULE h = LoadLibraryA("xinput1_4.dll");
        xi_get_state_ex_fn fn = NULL;
        if (h)
            fn = (xi_get_state_ex_fn)(void (*)(void))GetProcAddress(h, (LPCSTR)(uintptr_t)100);
        __atomic_store_n(&g_xi_ex, fn, __ATOMIC_RELEASE);
        __atomic_store_n(&g_xi_ex_done, 1, __ATOMIC_RELEASE);
    }
    return __atomic_load_n(&g_xi_ex, __ATOMIC_ACQUIRE);
}

/* Any thread: XInput is thread safe. */
static DWORD xi_get_raw(DWORD port, InputRaw *raw, DWORD *packet_out, int extras)
{
    XBOX_HOST_XINPUT_GAMEPAD pad;
    XINPUT_GAMEPAD g;
    DWORD packet, rc;
    /* Only the armed host actions or a map that names Guide want it; a stock
     * run keeps the documented call. */
    xi_get_state_ex_fn ex = extras ? xi_reader() : NULL;

    if (ex) {
        xi_state_ex xe;
        rc = ex(port, &xe);
        g = xe.Gamepad;
        packet = xe.dwPacketNumber;
    } else {
        XINPUT_STATE xi;
        rc = XInputGetState(port, &xi);
        g = xi.Gamepad;
        packet = xi.dwPacketNumber;
    }
    if (rc != ERROR_SUCCESS)
        return rc;
    memcpy(&pad, &g, sizeof pad);
    pad.wButtons &= (uint16_t)~XI_GUIDE;
    *packet_out = packet;
    xbox_PadRawXInput(&pad, (g.wButtons & XI_GUIDE) ? XBOX_HOST_GUIDE : 0u, raw);
    return ERROR_SUCCESS;
}

/* Always XInputGetStateEx (extras = 1): the guide button costs an
 * undocumented call, which is only fine because the caller, the flip tick,
 * reads only while the host actions are armed. A caller that ticks
 * unarmed would bring it to stock runs. */
static DWORD xi_read_raw(DWORD port, InputRaw *raw)
{
    DWORD packet;
    return xi_get_raw(port, raw, &packet, 1);
}

/* XInput cannot tell Steam's virtual pad from a real one, nor the family. */
static int xi_family(DWORD port)
{
    (void)port;
    return XBOX_PAD_FAMILY_XBOX;
}

static int xi_steam_input(DWORD port)
{
    const char *id = recomp_env(RENV_STEAM_GAME_ID), *deck = recomp_env(RENV_STEAM_DECK);
    (void)port;
    /* Only SteamDeck=1 means a Deck: "0" or anything else does not. */
    return (id && *id) || (deck && strcmp(deck, "1") == 0) ? XBOX_STEAM_INPUT_MAYBE
                                                             : XBOX_STEAM_INPUT_OFF;
}

static DWORD xi_set_state(DWORD port, const XBOX_VIBRATION *v)
{
    XINPUT_VIBRATION xv;
    xv.wLeftMotorSpeed  = v->wLeftMotorSpeed;
    xv.wRightMotorSpeed = v->wRightMotorSpeed;
    return XInputSetState(port, &xv);
}

static uint64_t xi_now_ms(void) { return GetTickCount64(); }

/* Under Wine XInputGetState has been seen to return ERROR_SUCCESS and an
 * idle pad for an empty user index. If a Wine/Proton build does that, the
 * connection probe has to move to XInputGetCapabilities. */
const xbox_input_backend xbox_input_host_backend = {
    NULL, xi_get_raw, xi_set_state, xbox_InputKbmPoll, xi_now_ms,
    xi_read_raw, xi_family, xi_steam_input
};

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL3 gamepad backend  ============================ */
/* ======================================================================== */

#include <SDL3/SDL.h>
#include <errno.h>
#include "platform/sdl_env.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>

/* Hot-plug. A port holds one open controller for as long as SDL reports it
 * attached; ports never shift, so player 1 keeps port 0 while pads come and
 * go on other ports. A removed pad frees its port. A new pad takes, in
 * order: the port the same model (gamepad GUID) last held, if it is free
 * and another pad is still connected; else the lowest free port. So a pad
 * pulled and replugged comes back as the same player, and a pad that
 * connects while no other is connected is player 1. A pad left alone on a
 * higher port stays there (ports never shift, as on the Xbox). Pads beyond
 * XBOX_MAX_CONTROLLERS wait for a free port.
 *
 * Threads: the game's poll threads read the table (get_state, set_state)
 * while the window thread pumps SDL (fb_present_sdl.c). Enumerating and
 * opening (SDL_GetGamepads, SDL_OpenGamepad) run on the thread that pumps
 * SDL events. SDL3 locks its device list, so this is no longer needed for
 * safety (sdl2-compat's unlocked index list was why it began), but it keeps
 * one owner for every open and close, and the boot wait below depends on
 * that owner. So with a window the window thread opens the subsystem and
 * rescans (xbox_InputSdlPadSync, once per loop), and the poll threads only
 * use open pads by pointer; headless, the poll thread is the pump and
 * rescans itself, with SDL_UpdateGamepads posting the events. Both run under g_slot_lock, which is taken before
 * SDL's joystick lock (inside every SDL call), never after: SDL runs event
 * watches with its lock held, so the watch only raises g_rescan. A rescan
 * runs on that flag, or at most once a second without it. */
struct sdl_slot {
    SDL_Gamepad    *pad;
    SDL_JoystickID  id;     /* instance id while open */
    SDL_GUID        guid;   /* the last pad held; kept after removal */
    int             had;    /* guid is set */
    int             family; /* atomic: the last sdl_family/steam_input read, */
    int             steam;  /* returned while another thread holds the lock */
    xbox_rumble_gate rumble; /* what the guest asks vs what SDL was given */
};
static struct sdl_slot  g_slot[XBOX_MAX_CONTROLLERS];
static pthread_mutex_t  g_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static int              g_rescan;       /* atomic: a device came or went */
static uint64_t         g_last_scan;
static int              g_wanted;       /* atomic: sdl_init ran (host pads on) */
static int              g_ready;        /* subsystem up and watched; lock held */
static pthread_cond_t   g_ready_cv = PTHREAD_COND_INITIALIZER;

/* How long the first poll waits for the window thread to open the pads, so
 * a pad plugged in before launch is connected at the title's first read
 * (XGetDevices) and not inserted a second later. Bounded: the window loop
 * wakes at least every 100 ms, and a caller that is the window thread
 * itself, or one the window is waiting on, only loses this much. */
#define XBOX_SDL_BOOT_WAIT_MS 250

/* Set by the window (src/video/fb_present_sdl.c) when the process main thread
 * runs the SDL event loop. Its SDL_PumpEvents already updates the
 * controllers, so this side only reads them: one event loop, not two threads
 * racing to pump. */
static volatile int g_main_pump;
void xbox_InputSetMainPump(int on) { g_main_pump = on; }

static DWORD g_packet[XBOX_MAX_CONTROLLERS];

_Static_assert(SDL_GAMEPAD_BUTTON_SOUTH == XBOX_SDL_BUTTON_A
               && SDL_GAMEPAD_BUTTON_START == XBOX_SDL_BUTTON_START
               && SDL_GAMEPAD_BUTTON_LEFT_SHOULDER == XBOX_SDL_BUTTON_LEFTSHOULDER
               && SDL_GAMEPAD_BUTTON_DPAD_RIGHT == XBOX_SDL_BUTTON_DPAD_RIGHT,
               "XBOX_SDL_BUTTON_* must match SDL_GamepadButton");
_Static_assert(SDL_GAMEPAD_AXIS_LEFTX == XBOX_SDL_AXIS_LEFTX
               && SDL_GAMEPAD_AXIS_RIGHTY == XBOX_SDL_AXIS_RIGHTY
               && SDL_GAMEPAD_AXIS_RIGHT_TRIGGER == XBOX_SDL_AXIS_TRIGGERRIGHT,
               "XBOX_SDL_AXIS_* must match SDL_GamepadAxis");

static uint64_t sdl_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Runs on whichever thread posts the event, with SDL's locks held: raise the
 * flag and nothing else (see above). */
static bool SDLCALL device_watch(void *ud, SDL_Event *ev)
{
    (void)ud;
    switch (ev->type) {
    case SDL_EVENT_JOYSTICK_ADDED:
    case SDL_EVENT_JOYSTICK_REMOVED:
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        __atomic_store_n(&g_rescan, 1, __ATOMIC_RELEASE);
        break;
    default:
        break;
    }
    return true;
}

/* What sdl_family and sdl_steam_input answer for a pad (below). */
static int pad_family(SDL_Gamepad *c);
static int pad_steam(SDL_Gamepad *c);

/* g_slot_lock held; any thread, as closing goes by pointer. */
static void close_slot(int port)
{
    SDL_CloseGamepad(g_slot[port].pad);
    g_slot[port].pad = NULL;
    xbox_RumbleGateReset(&g_slot[port].rumble);
    __atomic_store_n(&g_slot[port].family, XBOX_PAD_FAMILY_XBOX, __ATOMIC_RELEASE);
    __atomic_store_n(&g_slot[port].steam, 0, __ATOMIC_RELEASE);
    fprintf(stderr, "[INPUT] host pad removed port=%d\n", port);
    fflush(stderr);
    /* A pad waiting for a free port can take this one at the next poll. */
    __atomic_store_n(&g_rescan, 1, __ATOMIC_RELEASE);
}

/* g_slot_lock held, on the pumping thread. Open gamepad instance id into
 * the port it should have. */
static void open_device(SDL_JoystickID id)
{
    SDL_GUID g = SDL_GetGamepadGUIDForID(id);
    SDL_Gamepad *c;
    const char *name;
    int port = -1, others = 0;

    for (int p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        others |= g_slot[p].pad != NULL;
    if (others)
        for (int p = 0; p < XBOX_MAX_CONTROLLERS && port < 0; p++)
            if (!g_slot[p].pad && g_slot[p].had
                && !memcmp(&g_slot[p].guid, &g, sizeof g))
                port = p;
    for (int p = 0; p < XBOX_MAX_CONTROLLERS && port < 0; p++)
        if (!g_slot[p].pad)
            port = p;
    if (port < 0 || !(c = SDL_OpenGamepad(id)))
        return;
    g_slot[port].pad  = c;
    g_slot[port].id   = id;
    g_slot[port].guid = g;
    g_slot[port].had  = 1;
    xbox_RumbleGateReset(&g_slot[port].rumble);
    __atomic_store_n(&g_slot[port].family, pad_family(c), __ATOMIC_RELEASE);
    __atomic_store_n(&g_slot[port].steam, pad_steam(c), __ATOMIC_RELEASE);
    name = SDL_GetGamepadName(c);
    fprintf(stderr, "[INPUT] host pad added port=%d \"%s\"\n", port, name ? name : "?");
    fflush(stderr);
}

/* g_slot_lock held. Hand SDL the rumble level the gate lets through
 * (pad_map.h: a title's stream of changes, passed straight on, can starve
 * an HIDAPI pad's input reads). SDL rumble needs a duration: ~1 s, renewed
 * by the gate while the title keeps asking; 0 stops it. */
static void flush_rumble(int port, int asked)
{
    SDL_Gamepad *c = g_slot[port].pad;
    uint16_t l, r;

    /* The result is ignored and the gate counts the level sent. SDL keeps
     * its old level when the driver refuses one, so the gate's next renewal
     * is a change to SDL and tries again; a refused stop leaves the old
     * level, which SDL's duration ends within 1 s. */
    if (c && xbox_RumbleGatePoll(&g_slot[port].rumble, sdl_now_ms(), asked, &l, &r))
        (void)SDL_RumbleGamepad(c, l, r, (l || r) ? 1000 : 0);
}

/* g_slot_lock held, on the pumping thread. Close what SDL lost, open what
 * it gained. */
static void rescan(void)
{
    SDL_JoystickID *ids;
    int n = 0;

    /* With nothing pumping, device detection runs here. */
    if (!g_main_pump)
        SDL_UpdateGamepads();
    for (int p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        if (g_slot[p].pad && !SDL_GamepadConnected(g_slot[p].pad))
            close_slot(p);
    if (!(ids = SDL_GetGamepads(&n)))
        return;
    for (int i = 0; i < n; i++) {
        int open = 0;
        for (int p = 0; p < XBOX_MAX_CONTROLLERS; p++)
            open |= g_slot[p].pad && g_slot[p].id == ids[i];
        if (!open)
            open_device(ids[i]);
    }
    SDL_free(ids);
}

/* g_slot_lock held, on the pumping thread. On a device event, else at most
 * once a second. */
static void maybe_rescan(void)
{
    uint64_t now = sdl_now_ms();

    /* Cleared before the scan, so an event posted during it is not lost. */
    if (__atomic_exchange_n(&g_rescan, 0, __ATOMIC_ACQ_REL)
        || now - g_last_scan >= XBOX_INPUT_PROBE_MS) {
        g_last_scan = now;
        rescan();
    }
}

/* g_slot_lock held, on the pumping thread. */
static void start_sdl(void)
{
    /* Game controllers pull in SDL events; keep SIGINT/SIGTERM default. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_GAMEPAD))
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    SDL_AddEventWatch(device_watch, NULL);
    g_ready = 1;
    g_last_scan = sdl_now_ms();
    __atomic_store_n(&g_rescan, 0, __ATOMIC_RELEASE);
    rescan();
    pthread_cond_broadcast(&g_ready_cv);
}

/* Window thread, once per event loop (fb_present_sdl.c): the controller
 * subsystem's first open and every rescan happen here while it pumps. */
void xbox_InputSdlPadSync(void)
{
    if (!__atomic_load_n(&g_wanted, __ATOMIC_ACQUIRE))
        return;         /* host pads are off, or no poll yet */
    pthread_mutex_lock(&g_slot_lock);
    if (!g_ready)
        start_sdl();
    else
        maybe_rescan();
    /* A change the gate held back lands here, within a loop of its time. */
    for (int p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        flush_rumble(p, 0);
    pthread_mutex_unlock(&g_slot_lock);
}

static void sdl_init(void)
{
    /* Before the first SDL call: the old driver names still work. */
    xbox_sdl_env_compat();
    __atomic_store_n(&g_wanted, 1, __ATOMIC_RELEASE);
    pthread_mutex_lock(&g_slot_lock);
    if (!g_main_pump) {
        if (!g_ready)
            start_sdl();
    } else if (!g_ready) {
        /* The window's next loop opens the pads; wait for it, a bounded
         * time (the wait drops g_slot_lock, which that loop takes). Past
         * the bound the ports read as empty and the core asks again
         * within a second. */
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += XBOX_SDL_BOOT_WAIT_MS * 1000000L;
        dl.tv_sec  += dl.tv_nsec / 1000000000L;
        dl.tv_nsec %= 1000000000L;
        while (!g_ready
               && pthread_cond_timedwait(&g_ready_cv, &g_slot_lock, &dl) != ETIMEDOUT)
            ;
    }
    pthread_mutex_unlock(&g_slot_lock);
}

/* SDL buttons past the guest map (XBOX_SDL_BUTTON_COUNT) and Guide, which
 * the map leaves out: the host-only buttons (host_pad.h). g_slot_lock held. */
static uint32_t sdl_extra(SDL_Gamepad *c)
{
    static const struct { SDL_GamepadButton b; uint32_t bit; } k_extra[] = {
        { SDL_GAMEPAD_BUTTON_GUIDE,         XBOX_HOST_GUIDE },
        { SDL_GAMEPAD_BUTTON_MISC1,         XBOX_HOST_MISC1 },
        { SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, XBOX_HOST_PADDLE1 },
        { SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,  XBOX_HOST_PADDLE2 },
        { SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, XBOX_HOST_PADDLE3 },
        { SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,  XBOX_HOST_PADDLE4 },
        { SDL_GAMEPAD_BUTTON_TOUCHPAD,      XBOX_HOST_TOUCHPAD },
    };
    uint32_t extra = 0;

    for (size_t i = 0; i < sizeof k_extra / sizeof k_extra[0]; i++)
        if (SDL_GetGamepadButton(c, k_extra[i].b))
            extra |= k_extra[i].bit;
    return extra;
}

static void sdl_read_pad(SDL_Gamepad *c, XBOX_HOST_SDL_GAMEPAD *pad)
{
    for (int b = 0; b < XBOX_SDL_BUTTON_COUNT; b++)
        pad->button[b] = SDL_GetGamepadButton(c, (SDL_GamepadButton)b) ? 1 : 0;
    for (int a = 0; a < XBOX_SDL_AXIS_COUNT; a++)
        pad->axis[a] = SDL_GetGamepadAxis(c, (SDL_GamepadAxis)a);
}

static DWORD sdl_get_raw(DWORD port, InputRaw *raw, DWORD *packet, int extras)
{
    SDL_Gamepad *c;
    XBOX_HOST_SDL_GAMEPAD pad;
    uint32_t extra;

    pthread_mutex_lock(&g_slot_lock);
    if (!g_main_pump && g_ready)
        maybe_rescan();
    c = g_slot[port].pad;
    if (c && !g_main_pump)
        SDL_UpdateGamepads();
    if (c && !SDL_GamepadConnected(c)) {
        close_slot((int)port);
        c = NULL;
    }
    if (!c) {
        pthread_mutex_unlock(&g_slot_lock);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    if (!g_main_pump)
        flush_rumble((int)port, 0);     /* headless, the reads are the loop */
    sdl_read_pad(c, &pad);
    /* The buttons past the Duke only when the host actions are armed or the
     * map names one, as the XInput backend does with Guide. */
    extra = extras ? sdl_extra(c) : 0u;
    pthread_mutex_unlock(&g_slot_lock);
    *packet = ++g_packet[port];
    xbox_PadRawSdl(&pad, extra, raw);
    return ERROR_SUCCESS;
}

static DWORD sdl_set_state(DWORD port, const XBOX_VIBRATION *v)
{
    SDL_Gamepad *c;

    pthread_mutex_lock(&g_slot_lock);
    c = g_slot[port].pad;
    if (c) {
        xbox_RumbleGateSet(&g_slot[port].rumble, v->wLeftMotorSpeed, v->wRightMotorSpeed);
        flush_rumble((int)port, 1);
    }
    pthread_mutex_unlock(&g_slot_lock);
    return c ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

/* The flip tick's read, off the poll thread. Only a try: the window thread
 * may hold g_slot_lock across a rescan, and the flip must not wait on it.
 * No SDL_UpdateGamepads here: the pumping thread updates the pads, and a
 * second updater would race it. */
static DWORD sdl_read_raw(DWORD port, InputRaw *raw)
{
    XBOX_HOST_SDL_GAMEPAD pad;
    SDL_Gamepad *c;
    uint32_t extra;

    if (pthread_mutex_trylock(&g_slot_lock) != 0)
        return XBOX_INPUT_BUSY;
    c = g_ready ? g_slot[port].pad : NULL;
    if (!c || !SDL_GamepadConnected(c)) {
        pthread_mutex_unlock(&g_slot_lock);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    sdl_read_pad(c, &pad);
    extra = sdl_extra(c);
    pthread_mutex_unlock(&g_slot_lock);
    xbox_PadRawSdl(&pad, extra, raw);
    return ERROR_SUCCESS;
}

/* The Deck's built-in pad and Steam's virtual one (Valve, 28de). */
#define XBOX_VID_VALVE       0x28de
#define XBOX_PID_DECK        0x1205
#define XBOX_PID_STEAM_VPAD  0x11ff

/* gamepad-input-ux D5: a table, not detection, so the Deck's own pad is a
 * steam family whether or not Steam Input is on. */
static int xbox_sdl_family_of(int type, uint16_t vid, uint16_t pid)
{
    if (vid == XBOX_VID_VALVE && pid == XBOX_PID_DECK)
        return XBOX_PAD_FAMILY_STEAM;
    switch (type) {
    case SDL_GAMEPAD_TYPE_STANDARD:
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
        return XBOX_PAD_FAMILY_XBOX;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return XBOX_PAD_FAMILY_PLAYSTATION;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return XBOX_PAD_FAMILY_NINTENDO;
#if SDL_VERSION_ATLEAST(3, 4, 0)
    /* Its own family: SDL places its A X B Y south, east, west, north,
     * so the Switch's letters would name the wrong buttons. */
    case SDL_GAMEPAD_TYPE_GAMECUBE:
        return XBOX_PAD_FAMILY_GAMECUBE;
#endif
#if SDL_VERSION_ATLEAST(3, 4, 0)
    case SDL_GAMEPAD_TYPE_STEAM:
        return XBOX_PAD_FAMILY_STEAM;
#endif
    default:
        return XBOX_PAD_FAMILY_GENERIC;
    }
}

static int pad_family(SDL_Gamepad *c)
{
    return xbox_sdl_family_of(SDL_GetGamepadType(c), SDL_GetGamepadVendor(c),
                              SDL_GetGamepadProduct(c));
}

/* Steam's virtual pad: it carries a Steam handle, or is 28de:11ff. The
 * vendor alone never counts: the Deck's own pad with Steam Input off is
 * 28de:1205, and its paddles then reach the game. */
static int pad_steam(SDL_Gamepad *c)
{
    return SDL_GetGamepadSteamHandle(c) != 0
        || (SDL_GetGamepadVendor(c) == XBOX_VID_VALVE
            && SDL_GetGamepadProduct(c) == XBOX_PID_STEAM_VPAD);
}

/* Both may be asked from any thread, the flip thread among them (the
 * glyph strip, gamepad-input-ux I4), and a rescan may hold g_slot_lock
 * across SDL calls: they only try it, and answer the slot's last value
 * when it is busy. */
static int sdl_family(DWORD port)
{
    SDL_Gamepad *c;
    int f = XBOX_PAD_FAMILY_XBOX;

    if (pthread_mutex_trylock(&g_slot_lock) != 0)
        return __atomic_load_n(&g_slot[port].family, __ATOMIC_ACQUIRE);
    if ((c = g_slot[port].pad))
        f = pad_family(c);
    __atomic_store_n(&g_slot[port].family, f, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_slot_lock);
    return f;
}

static int sdl_steam_input(DWORD port)
{
    SDL_Gamepad *c;
    int on = 0;

    if (pthread_mutex_trylock(&g_slot_lock) != 0)
        on = __atomic_load_n(&g_slot[port].steam, __ATOMIC_ACQUIRE);
    else {
        if ((c = g_slot[port].pad))
            on = pad_steam(c);
        __atomic_store_n(&g_slot[port].steam, on, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&g_slot_lock);
    }
    return on ? XBOX_STEAM_INPUT_ON : XBOX_STEAM_INPUT_OFF;
}

const xbox_input_backend xbox_input_host_backend = {
    sdl_init, sdl_get_raw, sdl_set_state, xbox_InputKbmPoll, sdl_now_ms,
    sdl_read_raw, sdl_family, sdl_steam_input
};

#endif /* _WIN32 */
