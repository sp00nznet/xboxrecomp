/*
 * input_hotplug_sdl test: see CMakeLists.txt.
 *
 * The real SDL backend behind the public API, with a fake clock in the core
 * only (the core's once-a-second probe of an empty port is pad_map's
 * test; here each step moves the clock a second on so every port is asked).
 * Pads are SDL virtual joysticks of the gamepad type; which port a
 * pad landed on is found by pressing its START and seeing which port reads
 * it, so the check covers the whole path from the device to the port.
 *
 * The boot case runs first, as under a window: the first poll waits a
 * bounded time for the window's loop to open a pad plugged in before launch.
 * The other cases run twice: headless, where the poll rescans, then as under
 * a window, where this thread plays the window's and calls the pad sync each
 * step. The thread case runs only the second way, as the game does: every
 * open and close stays on one thread, and here that is the one attaching
 * the virtual pads.
 */
#include "host_pad.h"
#include "input_backend.h"
#include "pad_map.h"

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    SDL_JoystickID id;

    SDL_INIT_INTERFACE(&d);
    d.type     = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes    = 6;
    d.nbuttons = 15;
    d.vendor_id  = 0x1234;
    d.product_id = product;
    d.name     = "hotplug test pad";
    id = SDL_AttachVirtualJoystick(&d);
    if (!id) {
        /* The first one failing means no virtual joystick driver here. */
        printf("%s attach: %s\n", g_attached ? "FAIL" : "SKIP", SDL_GetError());
        exit(g_attached ? 1 : 77);
    }
    g_attached = 1;
    if (!SDL_IsGamepad(id)) {
        /* SDL builds without an automatic mapping for virtual pads. */
        char guid[64], map[512];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof guid);
        snprintf(map, sizeof map, "%s,hotplug test pad,a:b0,b:b1,x:b2,y:b3,back:b4,"
                 "guide:b5,start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,"
                 "rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
                 "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5", guid);
        SDL_AddGamepadMapping(map);
    }
    return id;
}

static void detach(SDL_JoystickID id)
{
    if (!SDL_DetachVirtualJoystick(id))
        printf("FAIL detach %d: %s\n", (int)id, SDL_GetError()), exit(1);
}

/* The port pad id drives, or -1: press its START, see which port reads it. */
static int port_of(SDL_JoystickID id)
{
    SDL_Joystick *j = SDL_GetJoystickFromID(id);
    XBOX_INPUT_STATE st;
    int port = -1, hits = 0;

    if (!j)
        return -1;      /* the backend has not opened it */
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_START, true);
    if (g_window)
        SDL_UpdateGamepads();           /* the window's pump would */
    for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        if (xbox_InputGetState(p, &st) == ERROR_SUCCESS
            && (st.Gamepad.wButtons & XBOX_GAMEPAD_START))
            port = (int)p, hits++;
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_START, false);
    if (g_window)
        SDL_UpdateGamepads();
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
    Uint64 t0;

    /* No window loop running: the first poll waits its bound, then reads
     * empty ports (here the caller is the would-be window thread itself,
     * the case the bound exists for). */
    a = attach(0x90);
    t0 = SDL_GetTicks();
    xbox_InputInit();
    {
        Uint64 ms = SDL_GetTicks() - t0;
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
    InputRaw raw;
    DWORD packet;
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
    CHECK(g_be.get_raw(0, &raw, &packet, 0) == ERROR_DEVICE_NOT_CONNECTED, "backend port 0 after A pulled");
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

/* ── 3. family, Steam Input and the paddles (gamepad-input-ux D5, D6) ── */

/* A virtual pad with a vendor and product, and SDL3's whole button set
 * (MISC1, the four paddles, the touchpad), so SDL's automatic virtual
 * mapping gives each its own button. */
static SDL_JoystickID attach_vidpid(Uint16 vendor, Uint16 product)
{
    SDL_VirtualJoystickDesc d;
    SDL_JoystickID id;

    SDL_INIT_INTERFACE(&d);
    d.type       = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes      = 6;
    d.nbuttons   = SDL_GAMEPAD_BUTTON_TOUCHPAD + 1;
    d.vendor_id  = vendor;
    d.product_id = product;
    d.name       = "family test pad";
    id = SDL_AttachVirtualJoystick(&d);
    if (!id)
        printf("FAIL attach %04x:%04x: %s\n", vendor, product, SDL_GetError()), exit(1);
    return id;
}

static void test_families(void)
{
    static const struct { Uint16 vid, pid; int family, steam; const char *what; } k[] = {
        { 0x045e, 0x0b12, XBOX_PAD_FAMILY_XBOX, XBOX_STEAM_INPUT_OFF, "Xbox Series" },
        { 0x054c, 0x09cc, XBOX_PAD_FAMILY_PLAYSTATION, XBOX_STEAM_INPUT_OFF, "DualShock 4" },
        { 0x054c, 0x0ce6, XBOX_PAD_FAMILY_PLAYSTATION, XBOX_STEAM_INPUT_OFF, "DualSense" },
        { 0x057e, 0x2009, XBOX_PAD_FAMILY_NINTENDO, XBOX_STEAM_INPUT_OFF, "Switch Pro" },
        /* The Deck's own pad is steam by the table, Steam Input on or off;
         * only Steam's virtual pad (11ff) says Steam Input is in between. */
        { 0x28de, 0x1205, XBOX_PAD_FAMILY_STEAM, XBOX_STEAM_INPUT_OFF, "Deck, Steam Input off" },
        { 0x28de, 0x11ff, -1, XBOX_STEAM_INPUT_ON, "Steam virtual pad" },
    };
    XBOX_INPUT_STATE st;

    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        SDL_JoystickID id = attach_vidpid(k[i].vid, k[i].pid);
        uint32_t ins, rem;
        int f, s;

        step(&ins, &rem);
        CHECK(port_of(id) == 0, "%s on port %d, want 0", k[i].what, port_of(id));
        f = xbox_InputPadFamily(0);
        s = xbox_InputSteamInput(0);
        if (k[i].family >= 0)
            CHECK(f == k[i].family, "%s: family %s, want %s", k[i].what,
                  xbox_InputPadFamilyName(f), xbox_InputPadFamilyName(k[i].family));
        CHECK(s == k[i].steam, "%s: steam input %d, want %d", k[i].what, s, k[i].steam);
        /* input.glyphs wins over the detection, and auto gives it back. */
        xbox_InputSetGlyphFamily(XBOX_PAD_FAMILY_NINTENDO);
        CHECK(xbox_InputPadFamily(0) == XBOX_PAD_FAMILY_NINTENDO, "%s: glyph override",
              k[i].what);
        xbox_InputSetGlyphFamily(XBOX_PAD_FAMILY_AUTO);
        CHECK(xbox_InputPadFamily(0) == f, "%s: auto again", k[i].what);
        detach(id);
        step(&ins, &rem);
    }

    /* The paddles reach the host side only, and the game only through a
     * map that names one: the stock map leaves R4 out. */
    {
        SDL_JoystickID id = attach_vidpid(0x28de, 0x1205);
        SDL_Joystick *j;
        xbox_pad_setup ps;
        InputRaw raw;
        uint32_t ins, rem;

        step(&ins, &rem);
        j = SDL_GetJoystickFromID(id);
        CHECK(j != NULL, "Deck pad not opened");
        SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, true);
        if (g_window)
            SDL_UpdateGamepads();
        CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS, "Deck pad read");
        CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0 && st.Gamepad.wButtons == 0,
              "R4 reached the game under the stock map");
        /* Mapped to A: the backend reads the extras for it, with no filter. */
        xbox_PadSetupIdentity(&ps);
        ps.map.source[INPUT_A] = INPUT_SRC_PADDLE1;
        xbox_InputSetPadSetup(&ps);
        CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS, "Deck pad read, mapped");
        CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255, "R4 mapped to A reads %u",
              st.Gamepad.bAnalogButtons[XBOX_BUTTON_A]);
        xbox_InputHostRaw(0, &raw);
        CHECK(raw.src[INPUT_SRC_PADDLE1] > 0, "the raw snapshot has no paddle1");
        CHECK(raw.src[INPUT_SRC_PADDLE2] == 0, "paddle2 down in the raw snapshot");
        xbox_InputSetPadSetup(NULL);
        SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, false);
        if (g_window)
            SDL_UpdateGamepads();
        xbox_InputGetState(0, &st);
        detach(id);
        step(&ins, &rem);
    }
}

/* ── 4. polls on one thread, hot-plug on another ──────────────────────── */

static volatile int g_stop;

static void *poller(void *arg)
{
    InputRaw raw;
    DWORD packet;
    long n = 0;
    (void)arg;
    while (!g_stop)
        for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++, n++) {
            g_be.get_raw(p, &raw, &packet, (int)(n & 1));
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
    InputRaw raw;
    DWORD packet;

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
        g_be.get_raw(p, &raw, &packet, 0);
    EXPECT(0x1, 0x1, 0, "after the churn");
    CHECK(port_of(keep) == 0, "kept pad on port %d after the churn", port_of(keep));
    detach(keep);
    EXPECT(0x0, 0, 0x1, "kept pad pulled");
}

/* ── 5. rumble at the title's rate ──────────────────────────────────── */

/* What the virtual pad's motors were told, and how often. */
static int      g_rumbles;
static uint16_t g_rumble_l, g_rumble_r;

static bool SDLCALL on_rumble(void *ud, Uint16 low, Uint16 high)
{
    (void)ud;
    __atomic_add_fetch(&g_rumbles, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_rumble_l, low, __ATOMIC_RELEASE);
    __atomic_store_n(&g_rumble_r, high, __ATOMIC_RELEASE);
    return true;
}

static SDL_JoystickID g_rumble_pad;
static int            g_saw_move;
static volatile int   g_guest_done;

/* The guest: BLiNX's XID output report every ~48 ms with a new level each
 * time, its input report read between them, and the player pushing the
 * stick halfway through. */
static void *rumble_guest(void *arg)
{
    XBOX_INPUT_STATE st;
    (void)arg;
    for (int i = 0; i < 21; i++) {
        XBOX_VIBRATION v = { (WORD)(30000 - i * 1000), (WORD)(i * 500) };
        CHECK(xbox_InputSetState(0, &v) == ERROR_SUCCESS, "rumble %d refused", i);
        if (i == 10)
            SDL_SetJoystickVirtualAxis(SDL_GetJoystickFromID(g_rumble_pad),
                                       SDL_GAMEPAD_AXIS_LEFTX, 32767);
        if (xbox_InputGetState(0, &st) == ERROR_SUCCESS && i > 10
            && st.Gamepad.sThumbLX > 16000)
            g_saw_move = 1;
        SDL_Delay(48);
    }
    {
        XBOX_VIBRATION off = { 0, 0 };
        xbox_InputSetState(0, &off);
    }
    __atomic_store_n(&g_guest_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* The gate through the real backend: changes reach the motors at most
 * once per XBOX_RUMBLE_MIN_MS, the stop lands, and xbox_InputShutdown stops
 * the motors right after a send. A virtual pad calls its Rumble callback
 * directly, with no HIDAPI rumble thread or device lock, so this cannot
 * reproduce the input starvation the gate is for; the stick check is only
 * a smoke check that reads still work while the motors are driven. */
static void test_rumble(void)
{
    SDL_VirtualJoystickDesc d;
    uint32_t ins, rem;
    uint64_t t0, elapsed;
    pthread_t t;
    int n;

    SDL_INIT_INTERFACE(&d);
    d.type       = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes      = 6;
    d.nbuttons   = 15;
    d.vendor_id  = 0x1234;
    d.product_id = 0x80;
    d.name       = "rumble test pad";
    d.Rumble     = on_rumble;
    g_rumble_pad = SDL_AttachVirtualJoystick(&d);
    CHECK(g_rumble_pad != 0, "rumble pad attach: %s", SDL_GetError());
    if (!g_rumble_pad)
        return;
    step(&ins, &rem);
    CHECK(xbox_InputIsConnected(0), "rumble pad not on port 0");
    g_rumbles = 0;
    g_saw_move = 0;
    g_guest_done = 0;
    t0 = SDL_GetTicks();
    if (g_window) {
        pthread_create(&t, NULL, rumble_guest, NULL);
        while (!__atomic_load_n(&g_guest_done, __ATOMIC_ACQUIRE)) {
            xbox_InputSdlPadSync();
            SDL_UpdateGamepads();       /* the window's pump would */
            SDL_Delay(5);
        }
        pthread_join(t, NULL);
    } else {
        rumble_guest(NULL);
    }
    elapsed = SDL_GetTicks() - t0;
    /* The stop goes at once; the loop (or the reads) running on after it
     * must not send anything more. */
    for (int i = 0; i < 30; i++) {
        XBOX_INPUT_STATE st;
        SDL_Delay(10);
        if (g_window)
            xbox_InputSdlPadSync();
        else
            xbox_InputGetState(0, &st);
    }
    n = __atomic_load_n(&g_rumbles, __ATOMIC_ACQUIRE);
    /* 22 asks over the guest's run (about 1 s, longer on a loaded host):
     * one start, a change per window at most, then the stop. */
    CHECK(n >= 3 && n <= (int)(elapsed / XBOX_RUMBLE_MIN_MS) + 2,
          "%s: the motors were told %d times for 22 asks in %llu ms",
          g_window ? "window" : "headless", n, (unsigned long long)elapsed);
    CHECK(g_rumble_l == 0 && g_rumble_r == 0, "%s: the stop did not land (%u %u)",
          g_window ? "window" : "headless", g_rumble_l, g_rumble_r);
    CHECK(g_saw_move, "%s: the stick did not move through the rumble",
          g_window ? "window" : "headless");

    /* Shutdown right after a send, with a change held back by the gate and
     * no loop after it: the motors still stop. */
    {
        XBOX_VIBRATION a = { 20000, 20000 }, b = { 40000, 40000 };
        CHECK(xbox_InputSetState(0, &a) == ERROR_SUCCESS, "rumble before shutdown refused");
        CHECK(xbox_InputSetState(0, &b) == ERROR_SUCCESS, "rumble before shutdown refused");
        CHECK(__atomic_load_n(&g_rumble_l, __ATOMIC_ACQUIRE) == 20000,
              "%s: the second change was not held back (%u)",
              g_window ? "window" : "headless", g_rumble_l);
        xbox_InputShutdown();
        CHECK(g_rumble_l == 0 && g_rumble_r == 0, "%s: shutdown left the motors on (%u %u)",
              g_window ? "window" : "headless", g_rumble_l, g_rumble_r);
    }
    SDL_SetJoystickVirtualAxis(SDL_GetJoystickFromID(g_rumble_pad), SDL_GAMEPAD_AXIS_LEFTX, 0);
    detach(g_rumble_pad);
    step(&ins, &rem);
}

int main(void)
{
    int v = SDL_GetVersion(), n = 0;
    SDL_JoystickID *host;

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        printf("FAIL SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    host = SDL_GetJoysticks(&n);
    if (n > 0) {
        printf("SKIP a host joystick is attached (%s); unplug it to run this test\n",
               SDL_GetJoystickNameForID(host[0]));
        SDL_free(host);
        return 77;
    }
    SDL_free(host);
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
    test_rumble();
    /* Under a window again, this thread standing in for its loop. */
    g_window = 1;
    xbox_InputSetMainPump(1);
    test_basic();
    test_memory();
    test_families();
    test_threads();
    test_rumble();
    printf("input_hotplug_sdl (SDL %d.%d.%d): %d checks, %d failed\n",
           SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v),
           g_checks, g_fail);
    return g_fail ? 1 : 0;
}
