/*
 * input_hotplug_sdl test: see CMakeLists.txt.
 *
 * The real SDL backend behind the public API, with a fake clock in the core
 * only (the core's once-a-second probe of an empty port is input_map's
 * test; here each step moves the clock a second on so every port is asked).
 * Pads are SDL virtual joysticks of the game controller type; which port a
 * pad landed on is found by pressing its START and seeing which port reads
 * it, so the check covers the whole path from the device to the port.
 *
 * The boot case runs first, as under a window: the first poll waits a
 * bounded time for the window's loop to open a pad plugged in before launch.
 * The other cases run twice: headless, where the poll rescans, then as under
 * a window, where this thread plays the window's and calls the pad sync each
 * step. The thread case runs only the second way, as the game does: with
 * sdl2-compat every device-index call has to stay on one thread, and here
 * that is the one attaching the virtual pads.
 */
#include "input_backend.h"

#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !SDL_VERSION_ATLEAST(2, 24, 0)
/* Virtual joysticks of a given type and product (SDL_JoystickAttachVirtualEx)
 * came in SDL 2.24. */
int main(void)
{
    printf("SKIP SDL %d.%d.%d has no SDL_JoystickAttachVirtualEx (2.24+)\n",
           SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
    return 77;
}
#else

static int g_fail, g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t f_now = 100000;
static uint64_t f_clock(void) { return f_now; }
static xbox_input_backend g_be;
static int g_window;     /* this thread stands in for the window's loop */

extern void xbox_InputSetMainPump(int on);
extern void xbox_InputSdlPadSync(void);

static void sync_pads(void)
{
    if (g_window)
        xbox_InputSdlPadSync();
}

/* ── pads ──────────────────────────────────────────────────────────────── */

static int g_attached;

/* A virtual game controller; the product id sets its GUID ("model"). */
static SDL_JoystickID attach(Uint16 product)
{
    SDL_VirtualJoystickDesc d;
    int idx;

    memset(&d, 0, sizeof d);
    d.version  = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type     = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes    = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.vendor_id  = 0x1234;
    d.product_id = product;
    d.name     = "hotplug test pad";
    idx = SDL_JoystickAttachVirtualEx(&d);
    if (idx < 0) {
        /* The first one failing means no virtual joystick driver here. */
        printf("%s attach: %s\n", g_attached ? "FAIL" : "SKIP", SDL_GetError());
        exit(g_attached ? 1 : 77);
    }
    g_attached = 1;
    if (!SDL_IsGameController(idx)) {
        /* SDL builds without an automatic mapping for virtual pads. */
        char guid[64], map[512];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(idx), guid, sizeof guid);
        snprintf(map, sizeof map, "%s,hotplug test pad,a:b0,b:b1,x:b2,y:b3,back:b4,"
                 "guide:b5,start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,"
                 "rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
                 "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5", guid);
        SDL_GameControllerAddMapping(map);
    }
    return SDL_JoystickGetDeviceInstanceID(idx);
}

static int index_of(SDL_JoystickID id)
{
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_JoystickGetDeviceInstanceID(i) == id)
            return i;
    return -1;
}

static void detach(SDL_JoystickID id)
{
    int i = index_of(id);
    if (i < 0 || SDL_JoystickDetachVirtual(i) < 0)
        printf("FAIL detach %d: %s\n", (int)id, SDL_GetError()), exit(1);
}

/* The port pad id drives, or -1: press its START, see which port reads it. */
static int port_of(SDL_JoystickID id)
{
    SDL_Joystick *j = SDL_JoystickFromInstanceID(id);
    XBOX_INPUT_STATE st;
    int port = -1, hits = 0;

    if (!j)
        return -1;      /* the backend has not opened it */
    SDL_JoystickSetVirtualButton(j, SDL_CONTROLLER_BUTTON_START, 1);
    if (g_window)
        SDL_GameControllerUpdate();     /* the window's pump would */
    for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        if (xbox_InputGetState(p, &st) == ERROR_SUCCESS
            && (st.Gamepad.wButtons & XBOX_GAMEPAD_START))
            port = (int)p, hits++;
    SDL_JoystickSetVirtualButton(j, SDL_CONTROLLER_BUTTON_START, 0);
    if (g_window)
        SDL_GameControllerUpdate();
    for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        xbox_InputGetState(p, &st);
    return hits == 1 ? port : -1;
}

/* ── masks, as XGetDeviceChanges reports them ──────────────────────────── */

static uint32_t g_prev;

/* One poll a second later: the connected mask, and what changed. */
static uint32_t step(uint32_t *ins, uint32_t *rem)
{
    XBOX_INPUT_STATE st;
    uint32_t m = 0;

    f_now += XBOX_INPUT_PROBE_MS;
    sync_pads();
    for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++) {
        (void)xbox_InputGetState(p, &st);
        if (xbox_InputIsConnected(p))
            m |= 1u << p;
    }
    *ins = m & ~g_prev;
    *rem = g_prev & ~m;
    g_prev = m;
    return m;
}

#define EXPECT(m_, ins_, rem_, what) do { \
    uint32_t i_, r_, m = step(&i_, &r_); \
    CHECK(m == (m_) && i_ == (ins_) && r_ == (rem_), \
          "%s: ports=0x%X ins=0x%X rem=0x%X, want 0x%X 0x%X 0x%X", \
          what, m, i_, r_, (unsigned)(m_), (unsigned)(ins_), (unsigned)(rem_)); \
} while (0)

/* ── 0. boot: a pad plugged in before launch is there at the first read ─ */

static volatile int g_boot_done;
static uint32_t     g_boot_mask;

static uint32_t connected_now(void)
{
    uint32_t m = 0;
    for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        if (xbox_InputIsConnected(p))
            m |= 1u << p;
    return m;
}

/* The guest thread's first XInput call. */
static void *boot_poll(void *arg)
{
    (void)arg;
    xbox_InputInit();
    g_boot_mask = connected_now();      /* same fake second: the init's probe */
    __atomic_store_n(&g_boot_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_boot(void)
{
    SDL_JoystickID a;
    pthread_t t;
    Uint32 t0;

    /* No window loop running: the first poll waits its bound, then reads
     * empty ports (here the caller is the would-be window thread itself,
     * the case the bound exists for). */
    a = attach(0x90);
    t0 = SDL_GetTicks();
    xbox_InputInit();
    {
        Uint32 ms = SDL_GetTicks() - t0;
        CHECK(ms >= 200 && ms < 1000, "unsynced boot wait took %u ms, want ~250", (unsigned)ms);
    }
    CHECK(connected_now() == 0, "ports=0x%X with no window loop, want 0", connected_now());

    /* The window loop running beside the guest: port 0 is connected at the
     * first read, with no insert a second later. */
    xbox_InputSetBackend(&g_be);
    t0 = SDL_GetTicks();
    pthread_create(&t, NULL, boot_poll, NULL);
    while (!__atomic_load_n(&g_boot_done, __ATOMIC_ACQUIRE)) {
        SDL_Delay(5);
        xbox_InputSdlPadSync();
    }
    pthread_join(t, NULL);
    CHECK(g_boot_mask == 0x1, "boot ports=0x%X, want 0x1", g_boot_mask);
    CHECK(SDL_GetTicks() - t0 < 200, "boot wait took %u ms with the loop running",
          (unsigned)(SDL_GetTicks() - t0));
    g_prev = g_boot_mask;
    EXPECT(0x1, 0, 0, "a second after boot");
    CHECK(port_of(a) == 0, "boot pad on port %d, want 0", port_of(a));
    detach(a);
    EXPECT(0x0, 0, 0x1, "boot pad pulled");
}

/* ── 1. attach, detach, reattach on one model ─────────────────────────── */

static void test_basic(void)
{
    XBOX_INPUT_STATE st;
    XBOX_VIBRATION vib = { 1000, 1000 };
    SDL_JoystickID a, b, c;

    EXPECT(0x0, 0, 0, "start, no pad");
    CHECK(xbox_InputGetState(0, &st) == ERROR_DEVICE_NOT_CONNECTED, "port 0 with no pad");

    a = attach(1);
    EXPECT(0x1, 0x1, 0, "A plugged");
    CHECK(port_of(a) == 0, "A on port %d, want 0", port_of(a));

    b = attach(1);
    EXPECT(0x3, 0x2, 0, "B plugged");
    CHECK(port_of(b) == 1, "B on port %d, want 1", port_of(b));

    /* Removal is seen at the next read of the port, not a second later. */
    detach(a);
    CHECK(xbox_InputGetState(0, &st) == ERROR_DEVICE_NOT_CONNECTED,
          "port 0 still connected after A was pulled");
    CHECK(g_be.get_state(0, &st) == ERROR_DEVICE_NOT_CONNECTED, "backend port 0 after A pulled");
    CHECK(xbox_InputSetState(0, &vib) == ERROR_DEVICE_NOT_CONNECTED, "rumble on a pulled pad");
    EXPECT(0x2, 0, 0x1, "A pulled");
    CHECK(port_of(b) == 1, "B moved to port %d when A left", port_of(b));

    /* A new pad takes the free port 0; B stays player 2. */
    c = attach(1);
    EXPECT(0x3, 0x1, 0, "C plugged");
    CHECK(port_of(c) == 0, "C on port %d, want 0", port_of(c));
    CHECK(port_of(b) == 1, "B on port %d, want 1", port_of(b));

    /* Player 1 stays put while player 2 leaves. */
    detach(b);
    EXPECT(0x1, 0, 0x2, "B pulled");
    CHECK(port_of(c) == 0, "C moved to port %d when B left", port_of(c));

    /* Everything out, then one back: a pad connecting alone is player 1. */
    detach(c);
    EXPECT(0x0, 0, 0x1, "C pulled");
    for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        CHECK(xbox_InputGetState(p, &st) == ERROR_DEVICE_NOT_CONNECTED, "port %u with no pad", p);
    a = attach(1);
    EXPECT(0x1, 0x1, 0, "A replugged alone");
    CHECK(port_of(a) == 0, "A back on port %d, want 0", port_of(a));
    detach(a);
    EXPECT(0x0, 0, 0x1, "A pulled again");
}

/* ── 2. a replugged model gets its old port back ──────────────────────── */

static void test_memory(void)
{
    SDL_JoystickID p, q, x;

    p = attach(0x10);
    q = attach(0x20);
    x = attach(0x30);
    EXPECT(0x7, 0x7, 0, "P Q X plugged");
    CHECK(port_of(p) == 0 && port_of(q) == 1 && port_of(x) == 2, "P Q X on %d %d %d",
          port_of(p), port_of(q), port_of(x));

    detach(q);
    detach(p);
    EXPECT(0x4, 0, 0x3, "P and Q pulled");

    /* Q first: port 1 is its old one, though port 0 is lower and free. */
    q = attach(0x20);
    EXPECT(0x6, 0x2, 0, "Q replugged");
    CHECK(port_of(q) == 1, "Q back on port %d, want 1", port_of(q));
    p = attach(0x10);
    EXPECT(0x7, 0x1, 0, "P replugged");
    CHECK(port_of(p) == 0, "P back on port %d, want 0", port_of(p));

    /* A fifth pad waits, and takes the first port that frees up. */
    {
        SDL_JoystickID y = attach(0x40), z = attach(0x50);
        EXPECT(0xF, 0x8, 0, "Y plugged, Z waits");
        CHECK(port_of(y) == 3, "Y on port %d, want 3", port_of(y));
        CHECK(port_of(z) == -1, "Z got port %d with all four taken", port_of(z));
        detach(x);
        EXPECT(0xF, 0, 0, "X pulled, Z takes port 2 at once");
        CHECK(port_of(z) == 2, "Z on port %d, want 2", port_of(z));
        detach(y);
        detach(z);
    }
    detach(p);
    detach(q);
    EXPECT(0x0, 0, 0xF, "all pulled");
}

/* ── 3. polls on one thread, hot-plug on another ──────────────────────── */

static volatile int g_stop;

static void *poller(void *arg)
{
    XBOX_INPUT_STATE st;
    long n = 0;
    (void)arg;
    while (!g_stop)
        for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++, n++) {
            g_be.get_state(p, &st);
            if (!(n & 63)) {
                XBOX_VIBRATION v = { 0, 0 };
                g_be.set_state(p, &v);
            }
        }
    return NULL;
}

static void test_threads(void)
{
    pthread_t t;
    SDL_JoystickID keep = attach(0x60);
    XBOX_INPUT_STATE st;

    sync_pads();
    pthread_create(&t, NULL, poller, NULL);
    for (int i = 0; i < 200; i++) {
        SDL_JoystickID id = attach((Uint16)(0x70 + (i & 3)));
        sync_pads();
        if (i & 1)
            SDL_Delay(1);
        detach(id);
        if (i & 2)
            sync_pads();
    }
    g_stop = 1;
    pthread_join(t, NULL);
    /* Settle: the last removals are seen, the kept pad is still player 1. */
    sync_pads();
    for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        g_be.get_state(p, &st);
    EXPECT(0x1, 0x1, 0, "after the churn");
    CHECK(port_of(keep) == 0, "kept pad on port %d after the churn", port_of(keep));
    detach(keep);
    EXPECT(0x0, 0, 0x1, "kept pad pulled");
}

int main(void)
{
    SDL_version v;

    SDL_GetVersion(&v);
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        printf("FAIL SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (SDL_NumJoysticks() > 0) {
        printf("SKIP a host joystick is attached (%s); unplug it to run this test\n",
               SDL_JoystickNameForIndex(0));
        return 77;
    }
    g_be = xbox_input_host_backend;
    g_be.now_ms = f_clock;
    xbox_InputSetBackend(&g_be);
    /* First, as under a window: the backend's first open is the boot one. */
    g_window = 1;
    xbox_InputSetMainPump(1);
    test_boot();
    /* Headless: the poll pumps SDL itself. */
    g_window = 0;
    xbox_InputSetMainPump(0);
    test_basic();
    test_memory();
    /* Under a window again, this thread standing in for its loop. */
    g_window = 1;
    xbox_InputSetMainPump(1);
    test_basic();
    test_memory();
    test_threads();
    printf("input_hotplug_sdl (SDL %d.%d.%d): %d checks, %d failed\n",
           v.major, v.minor, v.patch, g_checks, g_fail);
    return g_fail ? 1 : 0;
}

#endif /* SDL 2.24 */
