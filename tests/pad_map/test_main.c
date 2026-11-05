/*
 * pad_map test: see CMakeLists.txt.
 *
 * The expected mapping below is written out independently of the table in
 * src/input/pad_map.c (it is the spec's: Black = RB, White = LB, faces 255,
 * triggers 0..255, sticks up/right positive), so a wrong table row fails here
 * rather than being compared with itself.
 */
#include "pad_map.h"
#include "input_map.h"
#include "input_backend.h"
#include "recomp_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    char buf[128];
    snprintf(buf, sizeof buf, "%s=%s", k, v ? v : "");
    _putenv(buf);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
    recomp_env_reload();   /* the runtime reads the environment once */
}

/* ── 1. every row, both helpers ────────────────────────────────────────── */

enum { F_BTN, F_ANALOG, F_STICK };
struct expect {
    const char *host;
    uint16_t xi_bit;            /* XInput button, or 0 for an axis */
    int      xi_axis;           /* XBOX_XI_SRC_* when xi_bit == 0 */
    int      sdl;               /* SDL button or axis */
    int      field;             /* F_* */
    int      index;             /* wButtons bit / bAnalogButtons idx / axis */
};

static const struct expect k_expect[] = {
    { "south",             XBOX_XI_A, 0, XBOX_SDL_BUTTON_A, F_ANALOG, 0 },
    { "east",              XBOX_XI_B, 0, XBOX_SDL_BUTTON_B, F_ANALOG, 1 },
    { "west",              XBOX_XI_X, 0, XBOX_SDL_BUTTON_X, F_ANALOG, 2 },
    { "north",             XBOX_XI_Y, 0, XBOX_SDL_BUTTON_Y, F_ANALOG, 3 },
    { "left_shoulder",     0x0100, 0, 9,  F_ANALOG, 5 },   /* WHITE */
    { "right_shoulder",    0x0200, 0, 10, F_ANALOG, 4 },   /* BLACK */
    { "start",             0x0010, 0, 6,  F_BTN, 0x0010 },
    { "back",              0x0020, 0, 4,  F_BTN, 0x0020 },
    { "left_stick_click",  0x0040, 0, 7,  F_BTN, 0x0040 },
    { "right_stick_click", 0x0080, 0, 8,  F_BTN, 0x0080 },
    { "dpad_up",           0x0001, 0, 11, F_BTN, 0x0001 },
    { "dpad_down",         0x0002, 0, 12, F_BTN, 0x0002 },
    { "dpad_left",         0x0004, 0, 13, F_BTN, 0x0004 },
    { "dpad_right",        0x0008, 0, 14, F_BTN, 0x0008 },
    { "left_trigger",      0, XBOX_XI_SRC_LT, 4, F_ANALOG, 6 },
    { "right_trigger",     0, XBOX_XI_SRC_RT, 5, F_ANALOG, 7 },
    { "left_stick_left / left_stick_right",   0, XBOX_XI_SRC_LX, 0, F_STICK, 0 },
    { "left_stick_down / left_stick_up",      0, XBOX_XI_SRC_LY, 1, F_STICK, 1 },
    { "right_stick_left / right_stick_right", 0, XBOX_XI_SRC_RX, 2, F_STICK, 2 },
    { "right_stick_down / right_stick_up",    0, XBOX_XI_SRC_RY, 3, F_STICK, 3 },
};
#define N_EXPECT (sizeof k_expect / sizeof k_expect[0])

static SHORT stick(const XBOX_GAMEPAD *g, int a)
{
    return a == 0 ? g->sThumbLX : a == 1 ? g->sThumbLY
         : a == 2 ? g->sThumbRX : g->sThumbRY;
}

/* Everything in g is at rest except the one field e names, which is want.
 * At rest is 0, except the SDL helper's Y axes: SDL's 0 inverts to -1
 * (design D4 keeps (-1 - v) so -32768 and 32767 swap exactly). */
static void check_only(const char *who, const struct expect *e,
                       const XBOX_GAMEPAD *g, int want)
{
    XBOX_GAMEPAD z;
    memset(&z, 0, sizeof z);
    if (!strcmp(who, "sdl"))
        z.sThumbLY = z.sThumbRY = -1;
    switch (e->field) {
    case F_BTN:
        CHECK(g->wButtons == (WORD)e->index, "%s %s: wButtons 0x%04x, want 0x%04x",
              who, e->host, g->wButtons, e->index);
        z.wButtons = g->wButtons;
        break;
    case F_ANALOG:
        CHECK(g->bAnalogButtons[e->index] == want, "%s %s: bAnalogButtons[%d] = %d, want %d",
              who, e->host, e->index, g->bAnalogButtons[e->index], want);
        z.bAnalogButtons[e->index] = g->bAnalogButtons[e->index];
        break;
    default:
        CHECK(stick(g, e->index) == want, "%s %s: axis %d = %d, want %d",
              who, e->host, e->index, stick(g, e->index), want);
        if (e->index == 0) z.sThumbLX = g->sThumbLX;
        if (e->index == 1) z.sThumbLY = g->sThumbLY;
        if (e->index == 2) z.sThumbRX = g->sThumbRX;
        if (e->index == 3) z.sThumbRY = g->sThumbRY;
        break;
    }
    CHECK(!memcmp(&z, g, sizeof z), "%s %s: other guest fields touched", who, e->host);
}

static void test_rows(void)
{
    CHECK(xbox_pad_map_count == N_EXPECT, "table has %u rows, want %u",
          (unsigned)xbox_pad_map_count, (unsigned)N_EXPECT);
    for (size_t i = 0; i < N_EXPECT && i < xbox_pad_map_count; i++) {
        const struct expect *e = &k_expect[i];
        XBOX_HOST_XINPUT_GAMEPAD xi;
        XBOX_HOST_SDL_GAMEPAD sd;
        XBOX_GAMEPAD g;

        CHECK(!strcmp(xbox_pad_map[i].host, e->host), "row %u is '%s', want '%s'",
              (unsigned)i, xbox_pad_map[i].host, e->host);
        /* One vocabulary: the host names are upstream's pad sources. */
        for (const char *p = e->host; *p;) {
            char tok[32];
            size_t n = strcspn(p, " /");
            snprintf(tok, sizeof tok, "%.*s", (int)n, p);
            if (n)
                CHECK(xbox_InputMap_SourceFromName(tok) > 0,
                      "row %u: '%s' is no upstream source", (unsigned)i, tok);
            p += n;
            p += strspn(p, " /");
        }

        memset(&xi, 0, sizeof xi);
        memset(&sd, 0, sizeof sd);
        if (e->field != F_STICK && e->xi_bit) {
            xi.wButtons = e->xi_bit;
            sd.button[e->sdl] = 1;
            xbox_PadMapXInput(&xi, &g);
            check_only("xinput", e, &g, 255);
            xbox_PadMapSdl(&sd, &g);
            check_only("sdl", e, &g, 255);
        } else if (e->field == F_ANALOG) {           /* triggers */
            static const struct { int xi, sdl, want; } t[] = {
                { 0, 0, 0 }, { 128, 16384, 128 }, { 255, 32767, 255 } };
            for (int k = 0; k < 3; k++) {
                memset(&xi, 0, sizeof xi);
                memset(&sd, 0, sizeof sd);
                if (e->xi_axis == XBOX_XI_SRC_LT) xi.bLeftTrigger = (uint8_t)t[k].xi;
                else xi.bRightTrigger = (uint8_t)t[k].xi;
                sd.axis[e->sdl] = (int16_t)t[k].sdl;
                xbox_PadMapXInput(&xi, &g);
                check_only("xinput", e, &g, t[k].want);
                xbox_PadMapSdl(&sd, &g);
                check_only("sdl", e, &g, t[k].want);
            }
        } else {                                      /* sticks */
            static const int vals[] = { 32767, -32768, 300, -200, 0 };
            int is_y = e->index == 1 || e->index == 3;
            for (int k = 0; k < 5; k++) {
                int v = vals[k];
                int16_t *xa = e->xi_axis == XBOX_XI_SRC_LX ? &xi.sThumbLX
                            : e->xi_axis == XBOX_XI_SRC_LY ? &xi.sThumbLY
                            : e->xi_axis == XBOX_XI_SRC_RX ? &xi.sThumbRX : &xi.sThumbRY;
                memset(&xi, 0, sizeof xi);
                memset(&sd, 0, sizeof sd);
                *xa = (int16_t)v;
                xbox_PadMapXInput(&xi, &g);
                check_only("xinput", e, &g, v);        /* XInput: Y already up */
                /* SDL Y points down: the host value for "guest v" is -1 - v. */
                sd.axis[e->sdl] = (int16_t)(is_y ? -1 - v : v);
                xbox_PadMapSdl(&sd, &g);
                check_only("sdl", e, &g, v);
            }
        }
    }

    /* The two that were swapped before: RB held -> BLACK 255, WHITE 0. */
    {
        XBOX_HOST_XINPUT_GAMEPAD xi = { XBOX_XI_RIGHT_SHOULDER, 0, 0, 0, 0, 0, 0 };
        XBOX_GAMEPAD g;
        xbox_PadMapXInput(&xi, &g);
        CHECK(g.bAnalogButtons[XBOX_BUTTON_BLACK] == 255
              && g.bAnalogButtons[XBOX_BUTTON_WHITE] == 0, "RB is not BLACK");
    }
    /* Full down on the right stick reaches the guest as -32768 (SDL +32767). */
    {
        XBOX_HOST_SDL_GAMEPAD sd;
        XBOX_GAMEPAD g;
        memset(&sd, 0, sizeof sd);
        sd.axis[XBOX_SDL_AXIS_RIGHTY] = 32767;
        xbox_PadMapSdl(&sd, &g);
        CHECK(g.sThumbRY == -32768, "SDL RY full down = %d, want -32768", g.sThumbRY);
        sd.axis[XBOX_SDL_AXIS_RIGHTY] = -32768;
        xbox_PadMapSdl(&sd, &g);
        CHECK(g.sThumbRY == 32767, "SDL RY full up = %d, want 32767", g.sThumbRY);
    }
}

/* ── 2. deadzone ───────────────────────────────────────────────────────── */

static void dz(int zone, int x, int y, int wx, int wy)
{
    SHORT sx = (SHORT)x, sy = (SHORT)y;
    xbox_InputApplyDeadzone(&sx, &sy, zone);
    CHECK(sx == wx && sy == wy, "deadzone %d (%d,%d) -> (%d,%d), want (%d,%d)",
          zone, x, y, sx, sy, wx, wy);
}

static void test_deadzone(void)
{
    dz(0, 300, -200, 300, -200);
    dz(0, -32768, 32767, -32768, 32767);
    dz(4000, 300, -200, 0, 0);
    dz(4000, 32767, 0, 32767, 0);
    dz(4000, 0, -32768, 0, -32768);
    dz(4000, -32768, 0, -32768, 0);
    dz(4000, 32767, 32767, 32767, 32767);      /* corners unchanged */
    dz(4000, 4000, 0, 0, 0);                   /* the edge reads 0 */
    {   /* rescaled: halfway between the edge and full reads half */
        SHORT x = (SHORT)(4000 + (32767 - 4000) / 2), y = 0;
        xbox_InputApplyDeadzone(&x, &y, 4000);
        CHECK(x >= 16382 && x <= 16385 && y == 0, "deadzone midpoint -> %d", x);
    }

    /* Through the helpers: unset -> identity; 4000 -> zeroed rest. */
    {
        XBOX_HOST_XINPUT_GAMEPAD xi = { 0, 0, 0, 300, -200, 300, -200 };
        XBOX_GAMEPAD g;
        xbox_InputSetDeadzone(0);
        xbox_PadMapXInput(&xi, &g);
        CHECK(g.sThumbLX == 300 && g.sThumbLY == -200, "no deadzone: (%d,%d)", g.sThumbLX, g.sThumbLY);
        xbox_InputSetDeadzone(4000);
        xbox_PadMapXInput(&xi, &g);
        CHECK(!g.sThumbLX && !g.sThumbLY && !g.sThumbRX && !g.sThumbRY,
              "deadzone 4000 through the helper: (%d,%d)", g.sThumbLX, g.sThumbLY);
    }

    /* RECOMP_PAD_DEADZONE is read once. */
    xbox_InputSetDeadzone(-1);
    set_env("RECOMP_PAD_DEADZONE", "4000");
    CHECK(xbox_InputDeadzone() == 4000, "env deadzone %d", xbox_InputDeadzone());
    set_env("RECOMP_PAD_DEADZONE", "100");
    CHECK(xbox_InputDeadzone() == 4000, "deadzone re-read from env");
    xbox_InputSetDeadzone(-1);
    set_env("RECOMP_PAD_DEADZONE", NULL);
    CHECK(xbox_InputDeadzone() == 0, "default deadzone %d", xbox_InputDeadzone());
    xbox_InputSetDeadzone(0);
}

/* ── 3. README ─────────────────────────────────────────────────────────── */

static void test_readme(const char *path)
{
    char line[512], want[512];
    FILE *f;
    size_t row = 0;
    int in = 0, found = 0;
    const char *out = getenv("PAD_MAP_README_OUT");

    if (out) {
        FILE *o = fopen(out, "w");
        if (o) {
            fprintf(o, "%s\n", XBOX_PAD_MAP_README_BEGIN);
            for (size_t i = 0; xbox_PadMapReadmeRow(i, want, sizeof want); i++)
                fprintf(o, "%s\n", want);
            fprintf(o, "%s\n", XBOX_PAD_MAP_README_END);
            fclose(o);
        }
    }

    f = fopen(path, "r");
    CHECK(f != NULL, "cannot open %s", path);
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!in) {
            if (!strcmp(line, XBOX_PAD_MAP_README_BEGIN)) in = found = 1;
            continue;
        }
        if (!strcmp(line, XBOX_PAD_MAP_README_END)) { in = 0; break; }
        if (!xbox_PadMapReadmeRow(row, want, sizeof want)) {
            CHECK(0, "README has an extra table line: %s", line);
            break;
        }
        CHECK(!strcmp(line, want), "README line %u:\n  have: %s\n  want: %s",
              (unsigned)row, line, want);
        row++;
    }
    fclose(f);
    CHECK(found, "README has no '%s' marker", XBOX_PAD_MAP_README_BEGIN);
    CHECK(!in, "README table is not closed by '%s'", XBOX_PAD_MAP_README_END);
    CHECK(!xbox_PadMapReadmeRow(row, want, sizeof want),
          "README table is missing line %u: %s", (unsigned)row, want);
}

/* ── 4. connection cache ───────────────────────────────────────────────── */

static uint64_t f_now;
static int f_plugged[4], f_get_calls[4], f_set_calls[4], f_init_calls, f_kb_on, f_kb_calls;
static XBOX_VIBRATION f_last_vib[4];

static void f_init(void) { f_init_calls++; }
static DWORD f_get(DWORD p, XBOX_INPUT_STATE *s)
{
    f_get_calls[p]++;
    if (!f_plugged[p]) return ERROR_DEVICE_NOT_CONNECTED;
    memset(s, 0, sizeof *s);
    s->Gamepad.wButtons = XBOX_GAMEPAD_START;
    return ERROR_SUCCESS;
}
static DWORD f_set(DWORD p, const XBOX_VIBRATION *v)
{
    f_set_calls[p]++;
    f_last_vib[p] = *v;
    return ERROR_SUCCESS;
}
static int f_kb(XBOX_INPUT_STATE *s)
{
    f_kb_calls++;
    if (!f_kb_on) return 0;
    memset(s, 0, sizeof *s);
    s->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
    s->dwPacketNumber = 77;
    return 1;
}
static uint64_t f_clock(void) { return f_now; }
static int f_last_extras = -1;
static DWORD f_get_raw(DWORD p, InputRaw *raw, DWORD *packet, int extras)
{
    XBOX_INPUT_STATE s;
    DWORD rc = f_get(p, &s);

    f_last_extras = extras;
    if (rc == ERROR_SUCCESS) {
        xbox_PadRawFromGamepad(&s.Gamepad, 0, raw);
        *packet = s.dwPacketNumber;
    }
    return rc;
}

static const xbox_input_backend k_fake = { f_init, f_get_raw, f_set, f_kb, f_clock, NULL, NULL, NULL };

/* input_core.c's default; never called here. */
const xbox_input_backend xbox_input_host_backend = { NULL, f_get_raw, f_set, NULL, f_clock, NULL, NULL, NULL };

static void test_cache(void)
{
    XBOX_INPUT_STATE st;
    XBOX_VIBRATION vib = { 100, 200 };

    f_now = 5000;
    f_plugged[0] = 1;
    xbox_InputSetBackend(&k_fake);
    xbox_InputInit();
    xbox_InputInit();                                 /* idempotent */
    CHECK(f_init_calls == 1, "backend init called %d times", f_init_calls);
    for (int p = 0; p < 4; p++)
        CHECK(f_get_calls[p] == 1, "init probed port %d %d times", p, f_get_calls[p]);
    CHECK(xbox_InputIsConnected(0) && !xbox_InputIsConnected(1), "init mask wrong");

    /* Connected: every call reaches the backend. */
    for (int i = 0; i < 5; i++)
        CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS, "port 0 read failed");
    CHECK(f_get_calls[0] == 6, "port 0 backend calls %d, want 6", f_get_calls[0]);
    CHECK(st.Gamepad.wButtons == XBOX_GAMEPAD_START, "port 0 state not passed on");

    /* Disconnected: no backend call until a second after the last probe. */
    for (int i = 0; i < 10; i++) {
        f_now += 99;
        CHECK(xbox_InputGetState(1, &st) == ERROR_DEVICE_NOT_CONNECTED, "port 1 connected?");
        CHECK(!xbox_InputIsConnected(1), "port 1 cache says connected");
    }
    CHECK(f_get_calls[1] == 1, "port 1 probed %d times inside the backoff", f_get_calls[1]);
    f_now += 10;                                      /* 1000 ms since init */
    xbox_InputGetState(1, &st);
    CHECK(f_get_calls[1] == 2, "port 1 not probed after 1 s (%d)", f_get_calls[1]);

    /* Plugged in: the mask follows within a second, through IsConnected. */
    f_plugged[1] = 1;
    f_now += 500;
    CHECK(!xbox_InputIsConnected(1), "port 1 probed before the backoff");
    CHECK(f_get_calls[1] == 2, "IsConnected probed inside the backoff");
    f_now += 500;
    CHECK(xbox_InputIsConnected(1), "port 1 not seen 1 s after plug-in");
    CHECK(f_get_calls[1] == 3, "port 1 probes %d, want 3", f_get_calls[1]);

    /* Unplugged: seen at the next read, then backed off again. */
    f_plugged[0] = 0;
    CHECK(xbox_InputGetState(0, &st) == ERROR_DEVICE_NOT_CONNECTED, "unplug not seen");
    CHECK(!xbox_InputIsConnected(0), "cache still connected after unplug");
    {
        int before = f_get_calls[0];
        f_now += 999;
        xbox_InputGetState(0, &st);
        CHECK(f_get_calls[0] == before, "probed an unplugged port inside the backoff");
    }

    /* Rumble: only to connected ports; shutdown stops it, twice is safe. */
    CHECK(xbox_InputSetState(2, &vib) == ERROR_DEVICE_NOT_CONNECTED && !f_set_calls[2],
          "rumble reached a disconnected port");
    CHECK(xbox_InputSetState(1, &vib) == ERROR_SUCCESS && f_set_calls[1] == 1, "rumble lost");
    xbox_InputShutdown();
    CHECK(f_set_calls[1] == 2 && !f_last_vib[1].wLeftMotorSpeed
          && !f_last_vib[1].wRightMotorSpeed, "shutdown did not stop port 1's motors");
    xbox_InputShutdown();

    /* Keyboard on port 0 answers with no pad there. */
    f_kb_on = 1;
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS
          && st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255 && st.dwPacketNumber == 77,
          "keyboard not merged on an empty port 0");
    f_kb_on = 0;

    xbox_InputSetBackend(NULL);
}

/* xbox_InputGetPadState: the pad alone, never the keyboard (a game merging
 * the keyboard itself must read it once a poll, since the read consumes
 * mouse motion), under the same probe and cache rule as GetState. */
static void test_pad_only(void)
{
    XBOX_INPUT_STATE st, kb;

    memset(f_plugged, 0, sizeof f_plugged);
    memset(f_get_calls, 0, sizeof f_get_calls);
    f_now = 50000;
    f_plugged[0] = 1;
    f_kb_on = 1;
    xbox_InputSetBackend(&k_fake);
    xbox_InputInit();
    f_kb_calls = 0;

    CHECK(xbox_InputGetPadState(0, &st) == ERROR_SUCCESS
          && st.Gamepad.wButtons == XBOX_GAMEPAD_START
          && !st.Gamepad.bAnalogButtons[XBOX_BUTTON_A], "slot 0 pad-only read carries the keyboard");
    CHECK(f_kb_calls == 0, "GetPadState(0) read the keyboard %d times", f_kb_calls);
    CHECK(f_get_calls[0] == 2, "slot 0 backend calls %d, want 2", f_get_calls[0]);

    /* An empty slot backs off like GetState, and never answers with keys. */
    CHECK(xbox_InputGetPadState(1, &st) == ERROR_DEVICE_NOT_CONNECTED, "slot 1 connected?");
    CHECK(f_get_calls[1] == 1, "slot 1 probed inside the backoff (%d)", f_get_calls[1]);
    f_plugged[0] = 0;
    CHECK(xbox_InputGetPadState(0, &st) == ERROR_DEVICE_NOT_CONNECTED,
          "empty slot 0 answered (keyboard leak)");
    CHECK(f_kb_calls == 0, "GetPadState read the keyboard %d times", f_kb_calls);

    /* GetState(0) still reads it exactly once a call. */
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS
          && st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255, "GetState(0) lost the keyboard");
    CHECK(f_kb_calls == 1, "GetState(0) read the keyboard %d times, want 1", f_kb_calls);
    xbox_InputGetState(1, &st);
    CHECK(f_kb_calls == 1, "GetState(1) read the keyboard");

    /* The exported merge is GetState's. */
    memset(&st, 0, sizeof st);
    st.Gamepad.wButtons = XBOX_GAMEPAD_START;
    st.Gamepad.sThumbRX = 1000;
    st.Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 9;
    memset(&kb, 0, sizeof kb);
    kb.Gamepad.wButtons = XBOX_GAMEPAD_BACK;
    kb.Gamepad.sThumbRY = -5;
    kb.Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 4;
    kb.dwPacketNumber = 9;
    xbox_InputMergeKbm(&st, &kb);
    CHECK(st.Gamepad.wButtons == (XBOX_GAMEPAD_START | XBOX_GAMEPAD_BACK)
          && st.Gamepad.sThumbRX == 1000 && st.Gamepad.sThumbRY == -5
          && st.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 9 && st.dwPacketNumber == 9,
          "xbox_InputMergeKbm");

    f_kb_on = 0;
    xbox_InputSetBackend(NULL);
}

/* ── 6. the identity setup is the old fixed table, bit for bit ─────────── */

static uint32_t s_rng = 0x2545F491u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static int same_pad(const XBOX_GAMEPAD *a, const XBOX_GAMEPAD *b)
{
    return a->wButtons == b->wButtons
        && !memcmp(a->bAnalogButtons, b->bAnalogButtons, sizeof a->bAnalogButtons)
        && a->sThumbLX == b->sThumbLX && a->sThumbLY == b->sThumbLY
        && a->sThumbRX == b->sThumbRX && a->sThumbRY == b->sThumbRY;
}

static int16_t pick_axis(void)
{
    static const int16_t edge[] = { 0, 1, -1, 32767, -32768, -32767, 7849, -7849, 8689 };
    return (rnd() & 3) ? (int16_t)rnd() : edge[rnd() % (sizeof edge / sizeof edge[0])];
}

/* Both adapters, then the identity, against xbox_PadMapXInput/Sdl. */
static int identity_xi(const XBOX_HOST_XINPUT_GAMEPAD *in)
{
    xbox_pad_setup id;
    XBOX_GAMEPAD want, got;
    InputRaw raw;

    xbox_PadSetupIdentity(&id);
    xbox_PadMapXInput(in, &want);
    xbox_PadRawXInput(in, 0, &raw);
    xbox_PadSetupApply(&id, &raw, &got);
    return same_pad(&want, &got);
}

static int identity_sdl(const XBOX_HOST_SDL_GAMEPAD *in)
{
    xbox_pad_setup id;
    XBOX_GAMEPAD want, got;
    InputRaw raw;

    xbox_PadSetupIdentity(&id);
    xbox_PadMapSdl(in, &want);
    xbox_PadRawSdl(in, 0, &raw);
    xbox_PadSetupApply(&id, &raw, &got);
    return same_pad(&want, &got);
}

static void test_identity(void)
{
    static const int16_t corner[] = { 32767, -32768, 0, 1, -1 };
    XBOX_HOST_XINPUT_GAMEPAD xi;
    XBOX_HOST_SDL_GAMEPAD sdl;
    int bad;

    /* Every trigger value through both adapters. */
    bad = 0;
    for (int t = 0; t < 256; t++) {
        memset(&xi, 0, sizeof xi);
        xi.bLeftTrigger = (uint8_t)t;
        xi.bRightTrigger = (uint8_t)(255 - t);
        bad += !identity_xi(&xi);
    }
    CHECK(!bad, "%d XInput trigger values differ from the old table", bad);
    bad = 0;
    for (int v = -1; v <= 32767; v += (v < 300 ? 1 : 97)) {
        memset(&sdl, 0, sizeof sdl);
        sdl.axis[XBOX_SDL_AXIS_TRIGGERLEFT] = (int16_t)v;
        sdl.axis[XBOX_SDL_AXIS_TRIGGERRIGHT] = (int16_t)(v < 0 ? -32768 : 32767 - v);
        bad += !identity_sdl(&sdl);
    }
    CHECK(!bad, "%d SDL trigger values differ from the old table", bad);

    /* Stick corners and -32768 on every axis, at deadzone 0 and with the
     * RECOMP_PAD_DEADZONE rescale on. */
    for (int dzi = 0; dzi < 3; dzi++) {
        static const int dzs[] = { 0, 7849, 30000 };
        xbox_InputSetDeadzone(dzs[dzi]);
        bad = 0;
        for (size_t a = 0; a < 5; a++)
            for (size_t b = 0; b < 5; b++)
                for (int stick = 0; stick < 2; stick++) {
                    memset(&xi, 0, sizeof xi);
                    memset(&sdl, 0, sizeof sdl);
                    (stick ? &xi.sThumbRX : &xi.sThumbLX)[0] = corner[a];
                    (stick ? &xi.sThumbRY : &xi.sThumbLY)[0] = corner[b];
                    sdl.axis[stick ? XBOX_SDL_AXIS_RIGHTX : XBOX_SDL_AXIS_LEFTX] = corner[a];
                    sdl.axis[stick ? XBOX_SDL_AXIS_RIGHTY : XBOX_SDL_AXIS_LEFTY] = corner[b];
                    bad += !identity_xi(&xi) + !identity_sdl(&sdl);
                }
        CHECK(!bad, "%d stick corners differ at deadzone %d", bad, dzs[dzi]);
    }

    /* Random pads, every button, both sticks. */
    xbox_InputSetDeadzone(0);
    bad = 0;
    for (int i = 0; i < 20000; i++) {
        if (i == 10000)
            xbox_InputSetDeadzone(7849);
        xi.wButtons = (uint16_t)(rnd() & 0xF3FF);
        xi.bLeftTrigger = (uint8_t)rnd();
        xi.bRightTrigger = (uint8_t)rnd();
        xi.sThumbLX = pick_axis();
        xi.sThumbLY = pick_axis();
        xi.sThumbRX = pick_axis();
        xi.sThumbRY = pick_axis();
        bad += !identity_xi(&xi);
        for (int b = 0; b < XBOX_SDL_BUTTON_COUNT; b++)
            sdl.button[b] = (uint8_t)(rnd() & 1);
        for (int a = 0; a < XBOX_SDL_AXIS_COUNT; a++)
            sdl.axis[a] = pick_axis();
        bad += !identity_sdl(&sdl);
    }
    CHECK(!bad, "%d random pads differ from the old table", bad);
    xbox_InputSetDeadzone(0);

    {
        xbox_pad_setup s;
        xbox_PadSetupIdentity(&s);
        CHECK(xbox_PadSetupIsIdentity(&s), "the identity is not the identity");
        CHECK(!xbox_PadSetupNeedsExtras(&s), "the identity asks for the host-only buttons");
    }
}

/* ── 7. a setup that is not the identity ───────────────────────────────── */

static void test_setup(void)
{
    xbox_pad_setup s;
    XBOX_HOST_XINPUT_GAMEPAD xi;
    XBOX_GAMEPAD g;
    InputRaw raw;
    SHORT ex, ey;

    /* Swapped sticks: the raw sticks, each under the RECOMP_PAD_DEADZONE
     * of its new place. */
    memset(&xi, 0, sizeof xi);
    xi.sThumbLX = 1000; xi.sThumbLY = -2000; xi.sThumbRX = -32768; xi.sThumbRY = 32767;
    xbox_PadSetupIdentity(&s);
    s.map.left_stick = INPUT_STICK_RIGHT;
    s.map.right_stick = INPUT_STICK_LEFT;
    CHECK(!xbox_PadSetupIsIdentity(&s), "a swap reads as the identity");
    xbox_PadRawXInput(&xi, 0, &raw);
    xbox_PadSetupApply(&s, &raw, &g);
    CHECK(g.sThumbLX == -32768 && g.sThumbLY == 32767 && g.sThumbRX == 1000 && g.sThumbRY == -2000,
          "swap: L %d,%d R %d,%d", g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY);

    /* A file deadzone goes through upstream's rescale and clamps to the
     * unit circle; the env deadzone does not apply on top of it. */
    xbox_InputSetDeadzone(7849);
    xbox_PadSetupIdentity(&s);
    s.map.deadzone_left = 20;
    xi.sThumbLX = 20000; xi.sThumbLY = 20000;
    xbox_PadRawXInput(&xi, 0, &raw);
    xbox_PadSetupApply(&s, &raw, &g);
    xbox_InputMap_ApplyStick(20000, 20000, 20, &ex, &ey);
    CHECK(g.sThumbLX == ex && g.sThumbLY == ey, "file deadzone: %d,%d want %d,%d",
          g.sThumbLX, g.sThumbLY, ex, ey);
    ex = -32768; ey = 32767;
    xbox_InputApplyDeadzone(&ex, &ey, 7849);
    CHECK(g.sThumbRX == ex && g.sThumbRY == ey, "the other stick keeps the env deadzone");
    xbox_InputSetDeadzone(0);

    /* Inversion: -32768 clamps to 32767, the direction sources follow. */
    memset(&xi, 0, sizeof xi);
    xi.sThumbLY = -32768; xi.sThumbRY = 5;
    xbox_PadRawXInput(&xi, 0, &raw);
    CHECK(raw.src[INPUT_SRC_LSTICK_DOWN] == 32767 && raw.src[INPUT_SRC_LSTICK_UP] == 0,
          "down source of -32768: %d", raw.src[INPUT_SRC_LSTICK_DOWN]);
    xbox_PadRawInvert(&raw, 1, 1);
    CHECK(raw.ly == 32767 && raw.ry == -5, "invert: %d %d", raw.ly, raw.ry);
    CHECK(raw.src[INPUT_SRC_LSTICK_UP] == 32767 && raw.src[INPUT_SRC_LSTICK_DOWN] == 0
          && raw.src[INPUT_SRC_RSTICK_DOWN] == 5 && raw.src[INPUT_SRC_RSTICK_UP] == 0,
          "inverted direction sources");

    /* Direction sources on the four halves. */
    memset(&xi, 0, sizeof xi);
    xi.sThumbLX = -300; xi.sThumbRX = 12345; xi.sThumbRY = -32768;
    xbox_PadRawXInput(&xi, 0, &raw);
    CHECK(raw.src[INPUT_SRC_LSTICK_LEFT] == 300 && raw.src[INPUT_SRC_LSTICK_RIGHT] == 0
          && raw.src[INPUT_SRC_RSTICK_RIGHT] == 12345 && raw.src[INPUT_SRC_RSTICK_DOWN] == 32767,
          "direction sources");

    /* A remap plus a second cell: A on east, the alternate map adds A from
     * the left trigger past the button threshold. */
    xbox_PadSetupIdentity(&s);
    s.map.source[INPUT_A] = INPUT_SRC_EAST;
    s.map.source[INPUT_B] = INPUT_SRC_NONE;
    s.alt.source[INPUT_A] = INPUT_SRC_LEFT_TRIGGER;
    memset(&xi, 0, sizeof xi);
    xi.bLeftTrigger = 200;
    xbox_PadRawXInput(&xi, 0, &raw);
    xbox_PadSetupApply(&s, &raw, &g);
    CHECK(g.bAnalogButtons[XBOX_BUTTON_A] == 255, "second cell: A %d", g.bAnalogButtons[XBOX_BUTTON_A]);
    CHECK(g.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 200, "the trigger still drives LT");
    xi.wButtons = XBOX_XI_B; xi.bLeftTrigger = 10;
    xbox_PadRawXInput(&xi, 0, &raw);
    xbox_PadSetupApply(&s, &raw, &g);
    CHECK(g.bAnalogButtons[XBOX_BUTTON_A] == 255 && g.bAnalogButtons[XBOX_BUTTON_B] == 0,
          "remap: A %d B %d", g.bAnalogButtons[XBOX_BUTTON_A], g.bAnalogButtons[XBOX_BUTTON_B]);

    /* Host-only sources: the extras bits round-trip and a map naming one
     * asks for them. */
    memset(&xi, 0, sizeof xi);
    xbox_PadRawXInput(&xi, XBOX_HOST_GUIDE | XBOX_HOST_PADDLE3, &raw);
    CHECK(xbox_PadRawExtra(&raw) == (XBOX_HOST_GUIDE | XBOX_HOST_PADDLE3), "extras round trip");
    xbox_PadSetupIdentity(&s);
    s.alt.source[INPUT_X] = INPUT_SRC_PADDLE1;
    CHECK(xbox_PadSetupNeedsExtras(&s), "a paddle in the map does not ask for extras");
    xbox_PadSetupApply(&s, &raw, &g);
    CHECK(g.bAnalogButtons[XBOX_BUTTON_X] == 0, "paddle 1 not held, X %d", g.bAnalogButtons[XBOX_BUTTON_X]);
    xbox_PadRawXInput(&xi, XBOX_HOST_PADDLE1, &raw);
    xbox_PadSetupApply(&s, &raw, &g);
    CHECK(g.bAnalogButtons[XBOX_BUTTON_X] == 255, "paddle 1 held, X %d", g.bAnalogButtons[XBOX_BUTTON_X]);
}

/* ── 8. the poll asks the backend for extras only when needed ──────────── */

static void dummy_filter(DWORD port, const struct xbox_host_pad *host, XBOX_INPUT_STATE *st)
{
    (void)port; (void)host; (void)st;
}

static void test_extras_flag(void)
{
    XBOX_INPUT_STATE st;
    xbox_pad_setup s;

    memset(f_plugged, 0, sizeof f_plugged);
    f_plugged[0] = 1;
    xbox_InputSetBackend(&k_fake);
    xbox_InputSetPadSetup(NULL);
    xbox_InputGetState(0, &st);
    CHECK(f_last_extras == 0, "stock poll asked for extras");
    CHECK(st.Gamepad.wButtons == XBOX_GAMEPAD_START, "stock pad");

    xbox_InputSetPadFilter(dummy_filter);
    xbox_InputGetState(0, &st);
    CHECK(f_last_extras == 1, "a filter did not ask for extras");
    xbox_InputSetPadFilter(NULL);

    xbox_PadSetupIdentity(&s);
    s.map.source[INPUT_BACK] = INPUT_SRC_GUIDE;
    xbox_InputSetPadSetup(&s);
    xbox_InputGetState(0, &st);
    CHECK(f_last_extras == 1, "guide in the map did not ask for extras");
    CHECK(st.Gamepad.wButtons == XBOX_GAMEPAD_START, "START survives the map: %#x", st.Gamepad.wButtons);
    {
        xbox_pad_setup back;
        xbox_InputPadSetup(&back);
        CHECK(!memcmp(&back, &s, sizeof s), "the setup in force reads back");
    }

    /* The identity, handed in, is stored as NULL (the stock path). */
    xbox_PadSetupIdentity(&s);
    xbox_InputSetPadSetup(&s);
    xbox_InputGetState(0, &st);
    CHECK(f_last_extras == 0, "the identity asked for extras");
    xbox_InputSetBackend(NULL);
}

/* The host rumble gate (pad_map.h): changes at most once per
 * XBOX_RUMBLE_MIN_MS with the latest winning, a stop at once, levels
 * compared by their high byte, a held level renewed only on asks, nothing
 * for a pad that stays still. */
static void test_rumble_gate(void)
{
    xbox_rumble_gate g;
    uint16_t l = 0, r = 0;
    uint64_t t = 5000;
    int sent = 0;

    xbox_RumbleGateReset(&g);
    xbox_RumbleGateSet(&g, 0, 0);
    CHECK(!xbox_RumbleGatePoll(&g, t, 1, &l, &r), "still pad, still ask: sent");

    /* The first change goes at once. */
    xbox_RumbleGateSet(&g, 30000, 10000);
    CHECK(xbox_RumbleGatePoll(&g, t, 1, &l, &r) && l == 30000 && r == 10000,
          "first change held back or wrong: %u %u", l, r);

    /* BLiNX's rate, a new level every 48 ms for a second: at most one
     * change per XBOX_RUMBLE_MIN_MS reaches the host. */
    for (int i = 0; i < 21; i++) {
        t += 48;
        xbox_RumbleGateSet(&g, (uint16_t)(30000 - i * 1000), 10000);
        sent += xbox_RumbleGatePoll(&g, t, 1, &l, &r);
    }
    CHECK(sent >= 5 && sent <= 1000 / XBOX_RUMBLE_MIN_MS + 1,
          "%d changes sent in ~1 s, want <= %u", sent, 1000 / XBOX_RUMBLE_MIN_MS + 1);

    /* A change held back lands from the loop (asked 0), the latest one. */
    t += 1;
    xbox_RumbleGateSet(&g, 1234, 0);
    xbox_RumbleGateSet(&g, 5000, 0);
    CHECK(!xbox_RumbleGatePoll(&g, t, 1, &l, &r), "a change inside the window was sent");
    t += XBOX_RUMBLE_MIN_MS;
    CHECK(xbox_RumbleGatePoll(&g, t, 0, &l, &r) && l == 5000 && r == 0,
          "the held change did not land: %u %u", l, r);

    /* A stop goes at once, inside the window: xbox_InputShutdown sends one
     * with no loop after it to land a held one. */
    xbox_RumbleGateSet(&g, 0, 0);
    CHECK(xbox_RumbleGatePoll(&g, t + 1, 1, &l, &r) && l == 0 && r == 0,
          "a stop 1 ms after a send was held back: %u %u", l, r);
    CHECK(!xbox_RumbleGatePoll(&g, t + 10000, 0, &l, &r), "a stop was repeated");
    CHECK(!xbox_RumbleGatePoll(&g, t + 10000, 1, &l, &r), "a stop was repeated on an ask");

    /* Levels compare as the pad gets them, the high byte: a change in the
     * low byte alone is the same report, never sent; a level under 256 is
     * still not a stop. */
    t += 1000;
    xbox_RumbleGateSet(&g, 0x1200, 0x3400);
    CHECK(xbox_RumbleGatePoll(&g, t, 1, &l, &r), "a start after a still second held back");
    t += XBOX_RUMBLE_MIN_MS;
    xbox_RumbleGateSet(&g, 0x12ff, 0x3401);
    CHECK(!xbox_RumbleGatePoll(&g, t, 1, &l, &r), "a low-byte change was sent");
    CHECK(!xbox_RumbleGatePoll(&g, t + 1, 0, &l, &r), "a low-byte change landed from the loop");
    xbox_RumbleGateSet(&g, 0x0050, 0);
    CHECK(xbox_RumbleGatePoll(&g, t + 1, 1, &l, &r) && l == 0x0050 && r == 0,
          "a level under 256 was taken for no change: %u %u", l, r);
    xbox_RumbleGateSet(&g, 0, 0);
    CHECK(xbox_RumbleGatePoll(&g, t + 2, 1, &l, &r) && l == 0 && r == 0,
          "the stop from a level under 256 did not go: %u %u", l, r);

    /* A held level: renewed on asks every XBOX_RUMBLE_REFRESH_MS, with the
     * level SDL already has (so it writes nothing), never by the loop
     * alone, so a title that stops asking stops rumbling. */
    t += 1000;
    xbox_RumbleGateSet(&g, 500, 500);
    CHECK(xbox_RumbleGatePoll(&g, t, 1, &l, &r), "a start after a still second held back");
    sent = 0;
    for (int i = 0; i < 40; i++) {
        t += 50;
        xbox_RumbleGateSet(&g, (uint16_t)(500 + i % 7), 500);
        if (xbox_RumbleGatePoll(&g, t, 1, &l, &r)) {
            sent++;
            CHECK(l == 500 && r == 500, "a renewal changed the level: %u %u", l, r);
        }
    }
    CHECK(sent == 4, "%d renewals in 2 s of asks, want 4", sent);
    CHECK(!xbox_RumbleGatePoll(&g, t + 5000, 0, &l, &r), "the loop renewed a level");

    /* A renewal is no change: a real change right after one still goes. */
    xbox_RumbleGateSet(&g, 9000, 9000);
    CHECK(xbox_RumbleGatePoll(&g, t + 1, 1, &l, &r) && l == 9000 && r == 9000,
          "a change 1 ms after a renewal was held back: %u %u", l, r);

    /* A clock at 0 still counts as a send. */
    xbox_RumbleGateReset(&g);
    xbox_RumbleGateSet(&g, 1, 1);
    CHECK(xbox_RumbleGatePoll(&g, 0, 1, &l, &r), "a change at t=0 held back");
    xbox_RumbleGateSet(&g, 2, 2);
    CHECK(!xbox_RumbleGatePoll(&g, 1, 1, &l, &r), "a change 1 ms after t=0 sent");
}

int main(int argc, char **argv)
{
    xbox_InputSetDeadzone(0);
    test_rows();
    test_deadzone();
    test_readme(argc > 1 ? argv[1] : "input_README.md");
    test_cache();
    test_pad_only();
    test_identity();
    test_setup();
    test_extras_flag();
    test_rumble_gate();
    printf("pad_map: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
