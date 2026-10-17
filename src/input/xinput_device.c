/**
 * Xbox Input Compatibility Layer
 *
 * The host backends behind input_core.c (input_backend.h): each one reads
 * a host pad and maps it to the Duke layout through the table in
 * input_map.c, so both hosts map controls the same way.
 *
 *   _WIN32 -> Windows XInput (also Proton/Wine)
 *   POSIX  -> SDL2 GameController
 */

#include "input_backend.h"
#include "recomp_env.h"
#include "input_map.h"
#include "keyboard.h"
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

static DWORD xi_get_state(DWORD port, XBOX_INPUT_STATE *out)
{
    XINPUT_STATE xi;
    XBOX_HOST_XINPUT_GAMEPAD pad;
    DWORD rc = XInputGetState(port, &xi);

    if (rc != ERROR_SUCCESS)
        return rc;
    memcpy(&pad, &xi.Gamepad, sizeof pad);
    out->dwPacketNumber = xi.dwPacketNumber;
    xbox_InputMapXInput(&pad, &out->Gamepad);
    return ERROR_SUCCESS;
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
    NULL, xi_get_state, xi_set_state, xbox_InputKbmPoll, xi_now_ms
};

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

/* Hot-plug. A port holds one open controller for as long as SDL reports it
 * attached; ports never shift, so player 1 keeps port 0 while pads come and
 * go on other ports. A removed pad frees its port. A new pad takes, in
 * order: the port the same model (joystick GUID) last held, if it is free
 * and another pad is still connected; else the lowest free port. So a pad
 * pulled and replugged comes back as the same player, and a pad that
 * connects while no other is connected is player 1. A pad left alone on a
 * higher port stays there (ports never shift, as on the Xbox). Pads beyond
 * XBOX_MAX_CONTROLLERS wait for a free port.
 *
 * Threads: the game's poll threads read the table (get_state, set_state)
 * while the window thread pumps SDL (fb_present_sdl.c). Every call that
 * takes a device index (SDL_NumJoysticks, SDL_GameControllerOpen, ...) runs
 * on the thread that pumps SDL events, because sdl2-compat keeps the index
 * list in an unlocked global that its event conversion rebuilds when it
 * pops or posts a device event: a poll thread enumerating beside the
 * window's pump double-frees that list. So with a window the window thread
 * opens the subsystem and rescans (xbox_InputSdlPadSync, once per loop),
 * and the poll threads only use open pads by pointer; headless, the poll
 * thread is the pump and rescans itself, with SDL_GameControllerUpdate
 * posting the events. Both run under g_slot_lock, which is taken before
 * SDL's joystick lock (inside every SDL call), never after: SDL runs event
 * watches with its lock held, so the watch only raises g_rescan. A rescan
 * runs on that flag, or at most once a second without it. */
struct sdl_slot {
    SDL_GameController *pad;
    SDL_JoystickID      id;     /* instance id while open */
    SDL_JoystickGUID    guid;   /* the last pad held; kept after removal */
    int                 had;    /* guid is set */
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

/* Whether a controller is open, i.e. whether SDL may still be in use here.
 * Pads open only on the window thread, which is the one that asks. */
int xbox_InputAnyOpen(void)
{
    int any = 0;
    pthread_mutex_lock(&g_slot_lock);
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        any |= g_slot[i].pad != NULL;
    pthread_mutex_unlock(&g_slot_lock);
    return any;
}
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

_Static_assert(SDL_CONTROLLER_BUTTON_A == XBOX_SDL_BUTTON_A
               && SDL_CONTROLLER_BUTTON_START == XBOX_SDL_BUTTON_START
               && SDL_CONTROLLER_BUTTON_LEFTSHOULDER == XBOX_SDL_BUTTON_LEFTSHOULDER
               && SDL_CONTROLLER_BUTTON_DPAD_RIGHT == XBOX_SDL_BUTTON_DPAD_RIGHT,
               "XBOX_SDL_BUTTON_* must match SDL_GameControllerButton");
_Static_assert(SDL_CONTROLLER_AXIS_LEFTX == XBOX_SDL_AXIS_LEFTX
               && SDL_CONTROLLER_AXIS_RIGHTY == XBOX_SDL_AXIS_RIGHTY
               && SDL_CONTROLLER_AXIS_TRIGGERRIGHT == XBOX_SDL_AXIS_TRIGGERRIGHT,
               "XBOX_SDL_AXIS_* must match SDL_GameControllerAxis");

static uint64_t sdl_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Runs on whichever thread posts the event, with SDL's locks held: raise the
 * flag and nothing else (see above). */
static int SDLCALL device_watch(void *ud, SDL_Event *ev)
{
    (void)ud;
    switch (ev->type) {
    case SDL_JOYDEVICEADDED:
    case SDL_JOYDEVICEREMOVED:
    case SDL_CONTROLLERDEVICEADDED:
    case SDL_CONTROLLERDEVICEREMOVED:
        __atomic_store_n(&g_rescan, 1, __ATOMIC_RELEASE);
        break;
    default:
        break;
    }
    return 1;
}

/* g_slot_lock held; any thread, as closing goes by pointer. */
static void close_slot(int port)
{
    SDL_GameControllerClose(g_slot[port].pad);
    g_slot[port].pad = NULL;
    fprintf(stderr, "[INPUT] host pad removed port=%d\n", port);
    fflush(stderr);
    /* A pad waiting for a free port can take this one at the next poll. */
    __atomic_store_n(&g_rescan, 1, __ATOMIC_RELEASE);
}

/* g_slot_lock held, on the pumping thread. Open device index i into the
 * port it should have. */
static void open_device(int i)
{
    SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(i);
    SDL_GameController *c;
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
    if (port < 0 || !(c = SDL_GameControllerOpen(i)))
        return;
    g_slot[port].pad  = c;
    g_slot[port].id   = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
    g_slot[port].guid = g;
    g_slot[port].had  = 1;
    name = SDL_GameControllerName(c);
    fprintf(stderr, "[INPUT] host pad added port=%d \"%s\"\n", port, name ? name : "?");
    fflush(stderr);
}

/* g_slot_lock held, on the pumping thread. Close what SDL lost, open what
 * it gained. */
static void rescan(void)
{
    int n;

    /* With nothing pumping, device detection runs here. */
    if (!g_main_pump)
        SDL_GameControllerUpdate();
    for (int p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        if (g_slot[p].pad && !SDL_GameControllerGetAttached(g_slot[p].pad))
            close_slot(p);
    n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        int open = 0;
        for (int p = 0; p < XBOX_MAX_CONTROLLERS; p++)
            open |= g_slot[p].pad && g_slot[p].id == id;
        if (!open && SDL_IsGameController(i))
            open_device(i);
    }
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
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
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
    pthread_mutex_unlock(&g_slot_lock);
}

static void sdl_init(void)
{
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

static DWORD sdl_get_state(DWORD port, XBOX_INPUT_STATE *out)
{
    SDL_GameController *c;
    XBOX_HOST_SDL_GAMEPAD pad;

    pthread_mutex_lock(&g_slot_lock);
    if (!g_main_pump && g_ready)
        maybe_rescan();
    c = g_slot[port].pad;
    if (c && !g_main_pump)
        SDL_GameControllerUpdate();
    if (c && !SDL_GameControllerGetAttached(c)) {
        close_slot((int)port);
        c = NULL;
    }
    if (!c) {
        pthread_mutex_unlock(&g_slot_lock);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    for (int b = 0; b < XBOX_SDL_BUTTON_COUNT; b++)
        pad.button[b] = SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b) ? 1 : 0;
    for (int a = 0; a < XBOX_SDL_AXIS_COUNT; a++)
        pad.axis[a] = SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)a);
    pthread_mutex_unlock(&g_slot_lock);
    out->dwPacketNumber = ++g_packet[port];
    xbox_InputMapSdl(&pad, &out->Gamepad);
    return ERROR_SUCCESS;
}

static DWORD sdl_set_state(DWORD port, const XBOX_VIBRATION *v)
{
    SDL_GameController *c;

    pthread_mutex_lock(&g_slot_lock);
    c = g_slot[port].pad;
    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). 0 stops it. */
    if (c)
        SDL_GameControllerRumble(c, v->wLeftMotorSpeed, v->wRightMotorSpeed,
                                 (v->wLeftMotorSpeed || v->wRightMotorSpeed) ? 1000 : 0);
    pthread_mutex_unlock(&g_slot_lock);
    return c ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

const xbox_input_backend xbox_input_host_backend = {
    sdl_init, sdl_get_state, sdl_set_state, xbox_InputKbmPoll, sdl_now_ms
};

#endif /* _WIN32 */
