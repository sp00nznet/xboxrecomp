/*
 * input_actions test: the host actions (src/enhance/enhance_actions.c) as a
 * title runs them, through xbox_enhance_init and xbox_InputGetState against
 * a fake input backend with its own clock: the rules that keep a binding
 * off what the title uses (docs/runtime/config.md, "[input.actions]"), hold
 * chords and their consumption, guide+a (rule 7), chord_delay_ms, the flip
 * tick that reads a pad the title has stopped polling, arming by feature
 * keys only, the mod API v2 calls, the pad filter seam in input_core.c, and
 * bindings in host-source space under [input.pad] (gamepad-input-ux D4),
 * and the captured controls' port-0 record with D3's conflict rules
 * (mod-settings D1, D3). Runs in its working directory, where it writes config/config.toml.
 */
#include "enhance.h"
#include "enhance_actions.h"
#include "enhance_cfg.h"
#include "enhance_input.h"
#include "enhance_menu.h"
#include "host_pad.h"
#include "input_backend.h"
#include "input_names.h"
#include "pad_map.h"
#include "keyboard.h"
#include "mod.h"
#include "mouse.h"
#include "nv2a_backend_common.h"
#include "nv2a_overlay.h"
#include "nv2a_pb_state.h"
#include "recomp_env.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MKDIR(p) _mkdir(p)
#define DUP _dup
#define DUP2 _dup2
#define CLOSE _close
#define OPEN_TRUNC(p) _open(p, _O_WRONLY | _O_CREAT | _O_TRUNC, 0644)
#else
#include <pthread.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#define DUP dup
#define DUP2 dup2
#define CLOSE close
#define OPEN_TRUNC(p) open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644)
#endif

static int s_fail, s_checks;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- stubs for what the runtime would link ---------------------------- */

void xbox_PacingSetMode(int mode);
void xbox_PacingSetMode(int mode) { (void)mode; }
/* kernel_ramfs.c's switch, which this test does not link. */
void xbox_RamCacheEnable(int on);
void xbox_RamCacheEnable(int on) { (void)on; }
/* kernel_language.c's setter (enhance.c), which this test does not link. */
uint32_t xbox_SetLanguage(const char *code);
uint32_t xbox_SetLanguage(const char *code) { (void)code; return 1; }
/* kernel_path.c: no save root in this test, so a chain profile cannot
 * compose (shader_profile_req.c). */
int xbox_path_shader_cache(char *out, size_t n);
int xbox_path_shader_cache(char *out, size_t n) { (void)out; (void)n; return 0; }

ptrdiff_t xbox_GetMemoryOffset(void);
ptrdiff_t xbox_GetMemoryOffset(void) { return 0; }
/* mod.c's v6 guest calls run guest code (recomp_guest_call.c, tested in
 * mod_guest_call); this test has none to run. */
int recomp_guest_call(const char *who, uint32_t va, uint32_t eax, uint32_t ecx, uint32_t edx,
                      const uint32_t *stack, uint32_t n_stack, uint32_t *eax_out);
int recomp_guest_call(const char *who, uint32_t va, uint32_t eax, uint32_t ecx, uint32_t edx,
                      const uint32_t *stack, uint32_t n_stack, uint32_t *eax_out)
{
    (void)who; (void)va; (void)eax; (void)ecx; (void)edx; (void)stack; (void)n_stack;
    (void)eax_out;
    return 0;
}
uint32_t recomp_guest_alloc(uint32_t size, uint32_t align);
uint32_t recomp_guest_alloc(uint32_t size, uint32_t align)
{
    (void)size; (void)align;
    return 0;
}

static struct nv2a_pb_present s_present = { 0x00C00000u, 640, 480, 2560, 0 };
const struct nv2a_pb_present *nv2a_pb_present_state(void) { return &s_present; }

/* The overlay (tests/overlay has it): what enhance.c arms and what the
 * overlay.stats action asks of it. */
static uint32_t s_ovl_layers;
static int s_ovl_stats;
int nv2a_overlay_arm(uint32_t layers, uint32_t out_h)
{
    (void)out_h;
    s_ovl_layers = layers;
    return 0;
}
void nv2a_overlay_set_stats(int on) { s_ovl_stats = on; }
float nv2a_overlay_set_ui_scale(float mul) { return mul; }
/* The display probe needs a window system. */
void enhance_display_start(void) {}
int nv2a_overlay_stats(void) { return s_ovl_stats; }
static int s_toasts;
static char s_toast[128];
/* The menu (tests/enhance_menu has it): enhance.c starts it. */
void enhance_menu_start(void);
void enhance_menu_start(void) {}
/* enhance_menu.c's item lookup for a mod's link setting: none here. */
struct enhance_item;
const struct enhance_item *enhance_menu_toolkit_item(const char *key);
const struct enhance_item *enhance_menu_toolkit_item(const char *key) { (void)key; return 0; }
void nv2a_overlay_toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_toast, sizeof s_toast, fmt, ap);
    va_end(ap);
    s_toasts++;
}

/* ---- the fake pad backend --------------------------------------------- */

/* Win32 virtual-key codes, as the key table holds them. */
#define VK_F6  0x75
#define VK_F8  0x77
#define VK_F10 0x79

#define A_BIT  (1u << (8 + XBOX_BUTTON_A))
#define B_BIT  (1u << (8 + XBOX_BUTTON_B))

static uint64_t s_now = 1000;               /* atomic: the poll and flip threads read it */
static XBOX_INPUT_STATE f_pad[4];
static int f_conn[4] = { 1, 0, 0, 0 };
static uint32_t f_extra[4];
static int f_reads;                         /* read_raw calls */
static int f_busy;
static void (*f_during_read)(void);         /* runs once inside the next read_raw */

static uint64_t f_clock(void) { return __atomic_load_n(&s_now, __ATOMIC_ACQUIRE); }
static void advance(uint64_t ms) { __atomic_add_fetch(&s_now, ms, __ATOMIC_ACQ_REL); }

static DWORD f_get(DWORD p, XBOX_INPUT_STATE *s)
{
    if (p >= 4 || !f_conn[p])
        return ERROR_DEVICE_NOT_CONNECTED;
    *s = f_pad[p];
    return ERROR_SUCCESS;
}
static DWORD f_set(DWORD p, const XBOX_VIBRATION *v) { (void)p; (void)v; return ERROR_SUCCESS; }
/* Like the real backends, the host-only buttons come back only when asked. */
static DWORD f_get_raw(DWORD p, InputRaw *raw, DWORD *packet, int extras)
{
    XBOX_INPUT_STATE s;
    DWORD rc = f_get(p, &s);

    if (rc == ERROR_SUCCESS) {
        xbox_PadRawFromGamepad(&s.Gamepad, extras ? f_extra[p] : 0, raw);
        *packet = s.dwPacketNumber;
    }
    return rc;
}
/* A pad without Guide or paddles. */
static DWORD f_get_plain(DWORD p, InputRaw *raw, DWORD *packet, int extras)
{
    (void)extras;
    return f_get_raw(p, raw, packet, 0);
}
static DWORD f_read_raw(DWORD p, InputRaw *raw)
{
    f_reads++;
    if (f_busy)
        return XBOX_INPUT_BUSY;
    if (p >= 4 || !f_conn[p])
        return ERROR_DEVICE_NOT_CONNECTED;
    xbox_PadRawFromGamepad(&f_pad[p].Gamepad, f_extra[p], raw);
    if (f_during_read) {
        void (*fn)(void) = f_during_read;
        f_during_read = NULL;
        fn();
    }
    return ERROR_SUCCESS;
}

static const xbox_input_backend k_fake = {
    NULL, f_get_raw, f_set, xbox_InputKbmPoll, f_clock, f_read_raw, NULL, NULL };
/* A backend with no host-only buttons at all (a pad without Guide). */
static const xbox_input_backend k_plain = {
    NULL, f_get_plain, f_set, xbox_InputKbmPoll, f_clock, NULL, NULL, NULL };
/* input_core.c's default; never called here. */
const xbox_input_backend xbox_input_host_backend = {
    NULL, f_get_raw, f_set, NULL, f_clock, NULL, NULL, NULL };

static void pad_clear(void)
{
    memset(f_pad, 0, sizeof f_pad);
    memset(f_extra, 0, sizeof f_extra);
    f_conn[0] = 1;
    f_busy = 0;
}
static void hold_a(int on) { f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_A] = on ? 255 : 0; }
static void hold_b(int on) { f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_B] = on ? 255 : 0; }
static void hold_btn(WORD w, int on)
{
    if (on)
        f_pad[0].Gamepad.wButtons |= w;
    else
        f_pad[0].Gamepad.wButtons &= (WORD)~w;
}
static void hold_extra(uint32_t x, int on)
{
    if (on)
        f_extra[0] |= x;
    else
        f_extra[0] &= ~x;
}

/* What the title gets from port 0. */
static XBOX_INPUT_STATE poll0(void)
{
    XBOX_INPUT_STATE st;
    memset(&st, 0, sizeof st);
    xbox_InputGetState(0, &st);
    return st;
}

/* ---- stderr capture ---------------------------------------------------- */

static int s_saved_fd = -1;
static char s_cap[32768];

static void capture_begin(void)
{
    int fd;
    fflush(stderr);
    s_saved_fd = DUP(2);
    fd = OPEN_TRUNC("actions_stderr.txt");
    if (s_saved_fd < 0 || fd < 0) { fprintf(stderr, "cannot capture stderr\n"); exit(2); }
    DUP2(fd, 2);
    CLOSE(fd);
}

static const char *capture_end(void)
{
    FILE *f;
    size_t n = 0;
    fflush(stderr);
    DUP2(s_saved_fd, 2);
    CLOSE(s_saved_fd);
    s_cap[0] = 0;
    f = fopen("actions_stderr.txt", "rb");
    if (f) {
        n = fread(s_cap, 1, sizeof s_cap - 1, f);
        fclose(f);
    }
    s_cap[n] = 0;
    {
        char *r = s_cap, *w = s_cap;
        for (; *r; r++)
            if (*r != '\r')
                *w++ = *r;
        *w = 0;
    }
    return s_cap;
}

static int count(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

/* ---- setup -------------------------------------------------------------- */

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    char buf[512];
    snprintf(buf, sizeof buf, "%s=%s", k, v ? v : "");
    _putenv(buf);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
    recomp_env_reload();
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

/* A fresh title start with this user config (NULL: none) and backend; the
 * startup lines are returned. */
static const char *start_with(const char *toml, const xbox_input_backend *be,
                              const struct xbox_mod *const *mods)
{
    xbox_mod_shutdown();
    xbox_InputSetBackend(be);
    xbox_InputKeysClear();
    pad_clear();
    if (toml)
        write_file("config/config.toml", toml);
    else
        remove("config/config.toml");
    xbox_mod_register_table(mods);
    capture_begin();
    xbox_enhance_init(".", NULL);
    return capture_end();
}

/* The same with the backend's first poll, nothing held: a binding built
 * while its input is down fires only after a release, so a test that
 * presses first needs the title to have polled once. */
static const char *start(const char *toml)
{
    const char *out = start_with(toml, &k_fake, NULL);
    poll0();
    return out;
}

static int menu_fires(void)
{
    return enhance_action_pressed(enhance_action_find("menu.open"));
}

#define ARMED "[menu]\nenabled = true\n"

/* ---- 1. a hold chord: fires once after the hold, consumed until release -- */

static void test_hold_chord(void)
{
    XBOX_INPUT_STATE st;
    const char *out = start(ARMED "[input.actions]\nmenu.open = [\"back+left_thumb@400\"]\n");

    CHECK(strstr(out, "[ENHANCE] actions hook=on (menu.enabled)\n") != NULL);
    CHECK(enhance_actions_armed());

    /* Back alone passes; the chord is not complete. */
    hold_btn(XBOX_GAMEPAD_BACK, 1);
    st = poll0();
    CHECK(st.Gamepad.wButtons & XBOX_GAMEPAD_BACK);
    /* Both down: the title sees neither from now on. */
    advance(10);
    hold_btn(XBOX_GAMEPAD_LEFT_THUMB, 1);
    st = poll0();
    CHECK(!(st.Gamepad.wButtons & (XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB)));
    advance(300);
    st = poll0();
    CHECK(!(st.Gamepad.wButtons & (XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB)));
    /* Let go before the hold: nothing fires, and Back, still held, stays
     * the host's until its own release. */
    hold_btn(XBOX_GAMEPAD_LEFT_THUMB, 0);
    advance(10);
    st = poll0();
    CHECK(!(st.Gamepad.wButtons & XBOX_GAMEPAD_BACK));
    advance(500);
    st = poll0();
    CHECK(!(st.Gamepad.wButtons & XBOX_GAMEPAD_BACK));
    CHECK(menu_fires() == 0);
    hold_btn(XBOX_GAMEPAD_BACK, 0);
    poll0();
    /* A new press of Back is the title's again. */
    advance(10);
    hold_btn(XBOX_GAMEPAD_BACK, 1);
    st = poll0();
    CHECK(st.Gamepad.wButtons & XBOX_GAMEPAD_BACK);
    hold_btn(XBOX_GAMEPAD_BACK, 0);
    poll0();

    /* Held through the hold: one fire, down while held, gone at release. */
    capture_begin();
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB, 1);
    poll0();
    advance(399);
    poll0();
    CHECK(menu_fires() == 0);
    CHECK(!enhance_action_down(enhance_action_find("menu.open")));
    advance(1);
    poll0();
    advance(200);
    st = poll0();
    CHECK(!(st.Gamepad.wButtons & (XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB)));
    CHECK(enhance_action_down(enhance_action_find("menu.open")));
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB, 0);
    poll0();
    out = capture_end();
    CHECK(menu_fires() == 1);
    CHECK(menu_fires() == 0);
    CHECK(!enhance_action_down(enhance_action_find("menu.open")));
    CHECK(count(out, "[ACTION] menu.open via back+left_thumb@400\n") == 1);
}

/* ---- 2. guide+a (rule 7) ---------------------------------------------- */

static void test_guide_a(void)
{
    XBOX_INPUT_STATE st;
    const char *out = start(ARMED);     /* menu.open's defaults: F10, guide+a, paddle1 */

    CHECK(strstr(out, "[ENHANCE] actions hook=on (menu.enabled)\n") != NULL);
    /* Guide, then A: fires on A's press; the title never sees A, and the
     * rest of the pad passes. */
    hold_extra(XBOX_HOST_GUIDE, 1);
    poll0();
    advance(16);
    capture_begin();
    hold_a(1);
    hold_btn(XBOX_GAMEPAD_START, 1);
    st = poll0();
    out = capture_end();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    CHECK(st.Gamepad.wButtons & XBOX_GAMEPAD_START);
    CHECK(menu_fires() == 1);
    CHECK(strstr(out, "[ACTION] menu.open via guide+a\n") != NULL);
    /* Guide let go first: A, still held, is still not the title's. */
    hold_extra(XBOX_HOST_GUIDE, 0);
    advance(16);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    advance(500);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    CHECK(menu_fires() == 0);
    hold_a(0);
    poll0();
    advance(16);
    hold_a(1);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    CHECK(menu_fires() == 0);

    /* A before Guide: A passes, Guide joining does not complete it. */
    hold_a(0);
    hold_btn(XBOX_GAMEPAD_START, 0);
    poll0();
    advance(16);
    hold_a(1);
    poll0();
    advance(16);
    hold_extra(XBOX_HOST_GUIDE, 1);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    advance(500);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    CHECK(menu_fires() == 0);
    hold_a(0);
    hold_extra(XBOX_HOST_GUIDE, 0);
    poll0();
}

/* ---- 3. a backend with no Guide: A passes unchanged -------------------- */

static void test_no_guide(void)
{
    XBOX_INPUT_STATE st;
    start_with(ARMED, &k_plain, NULL);
    f_extra[0] = XBOX_HOST_GUIDE;     /* the backend cannot report it */
    poll0();
    advance(16);
    hold_a(1);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    CHECK(menu_fires() == 0);
    CHECK(xbox_InputHostExtra(0) == 0);
}

/* ---- 4. the button that closed the menu, masked until release --------- */

static void test_mask_until_release(void)
{
    XBOX_INPUT_STATE st;
    start(ARMED);
    hold_b(1);
    poll0();
    /* B is held, A is not: only B is masked. */
    enhance_actions_mask_until_release(0, B_BIT | A_BIT);
    advance(16);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 0);
    advance(16);
    hold_a(1);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 0);
    hold_b(0);
    advance(16);
    poll0();
    hold_b(1);
    advance(16);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 255);
}

/* ---- 5. refusals (rules 1, 2 and 7) ------------------------------------ */

static void test_refusals(void)
{
    const char *out = start(
        ARMED
        "[input]\nwalk_key = \"F7\"\n"
        "[input.bindings]\nwhite = \"F6\"\n"
        "[input.actions]\nmenu.open = [\"a\", \"back+left_thumb\", \"back+left_thumb@100\","
        " \"left_thumb+guide\", \"F6\", \"F7\", \"Ctrl+F8\", \"F8\"]\n"
        "overlay.stats = [\"Z\", \"F9\", \"mouse1\", \"F2+F8\", \"pad2:F8\","
        " \"guide+a+b\", \"paddle1@5000\", \"F3\", \"F4\"]\n");

    CHECK(strstr(out, "'a' inactive: south (the game's a) goes to the game; a single"
                 " source the game uses") != NULL);
    CHECK(strstr(out, "'back+left_thumb' inactive: every member goes to the game (back"
                 " (the game's back), left_stick_click (left_thumb)); such a chord needs a"
                 " hold of at least 200 ms") != NULL);
    CHECK(strstr(out, "'back+left_thumb@100' inactive: every member goes to the game")
          != NULL);
    /* The modifier need not come first: the kind follows the map. */
    CHECK(strstr(out, "'left_thumb+guide'") == NULL);
    CHECK(strstr(out, "'F6' refused: 'F6' is bound to the guest's") != NULL);
    CHECK(strstr(out, "'F7' refused: 'F7' is the walk key (input.walk_key)") != NULL);
    CHECK(strstr(out, "'Ctrl+F8' refused: 'Ctrl' is bound to the guest's") != NULL);
    CHECK(strstr(out, "'Z' refused: 'Z' is bound to the guest's") != NULL);
    CHECK(strstr(out, "'F9' refused: 'F9' is the mouse release key") != NULL);
    CHECK(strstr(out, "'mouse1' refused: 'mouse1' is a mouse input") != NULL);
    CHECK(strstr(out, "'F2+F8' refused: 'F2' is not a modifier") != NULL);
    CHECK(strstr(out, "'pad2:F8' refused: '@' and 'padN:' are for pad bindings") != NULL);
    CHECK(strstr(out, "'paddle1@5000' refused: the hold after '@' must be") != NULL);
    /* guide+a+b is a modifier chord of two guest members: allowed. */
    CHECK(strstr(out, "'guide+a+b' refused") == NULL);
    CHECK(strstr(out, "'F8' refused") == NULL);
    /* Eight bindings an action at most. */
    CHECK(strstr(out, "more than 8 bindings; the rest skipped") != NULL);
    CHECK(strstr(out, "'F4'") == NULL);
    /* Each refusal names where it came from. */
    CHECK(strstr(out, "[INPUT] action input.actions.menu.open (./config/config.toml:")
          != NULL);

    /* The one that stood fires; Ctrl held with it is another binding. */
    xbox_InputKeyEvent(VK_F8, 1);
    poll0();
    xbox_InputKeyEvent(VK_F8, 0);
    poll0();
    CHECK(menu_fires() == 1);
    xbox_InputKeyEvent(XBOX_VK_LCONTROL, 1);
    xbox_InputKeyEvent(VK_F8, 1);
    poll0();
    CHECK(menu_fires() == 0);
    xbox_InputKeysClear();
    poll0();
    /* F10 is not bound any more: the list replaced the defaults. */
    xbox_InputKeyEvent(VK_F10, 1);
    poll0();
    CHECK(menu_fires() == 0);

    /* An old pad name in a hold chord (the fork's names before upstream's)
     * is named, not taken for a key misusing '@'. */
    out = start(ARMED "[input.actions]\nmenu.open = [\"back+lthumb@400\", \"pad2:KP8\"]\n");
    CHECK(strstr(out, "'back+lthumb@400' refused: unknown name 'lthumb'") != NULL);
    CHECK(strstr(out, "'pad2:KP8' refused: unknown name 'KP8'") != NULL);
    CHECK(strstr(out, "are for pad bindings") == NULL);

    /* Unknown actions under [input.actions] are named. */
    out = start(ARMED "[input.actions]\nmenu.opne = \"F8\"\n");
    CHECK(strstr(out, "no action named 'menu.opne'") != NULL);

    /* The quoted spelling a TOML writer emits is the same binding. */
    out = start(ARMED "[input.actions]\n\"menu.open\" = [\"F8\"]\n");
    CHECK(strstr(out, "config error") == NULL);
    CHECK(strstr(out, "no action named") == NULL);
    xbox_InputKeyEvent(VK_F8, 1);
    poll0();
    xbox_InputKeyEvent(VK_F8, 0);
    poll0();
    CHECK(menu_fires() == 1);

    /* x.y beside x.y.z cannot both be keys: the second is refused. */
    capture_begin();
    CHECK(enhance_action_register("menu.open.all", "All", NULL) < 0);
    CHECK(enhance_action_register("menu", "Bare", NULL) < 0);
    CHECK(enhance_action_register("overlay", "Bare", NULL) < 0);
    out = capture_end();
    CHECK(strstr(out, "'menu.open.all' refused: it and 'menu.open' cannot both be"
                 " TOML keys") != NULL);
}

/* ---- 6. chord_delay_ms -------------------------------------------------- */

static void test_chord_delay(void)
{
    XBOX_INPUT_STATE st;
    const char *out = start(ARMED "[input]\nchord_delay_ms = 50\n"
                            "[input.actions]\nmenu.open = [\"back+left_thumb@400\"]\n");
    CHECK(strstr(out, "chord_delay_ms") == NULL);   /* no warning */

    /* A lone chord member reaches the title after the delay. */
    hold_btn(XBOX_GAMEPAD_BACK, 1);
    st = poll0();
    CHECK(!(st.Gamepad.wButtons & XBOX_GAMEPAD_BACK));
    advance(30);
    st = poll0();
    CHECK(!(st.Gamepad.wButtons & XBOX_GAMEPAD_BACK));
    advance(30);
    st = poll0();
    CHECK(st.Gamepad.wButtons & XBOX_GAMEPAD_BACK);
    /* A button that is in no chord is never held back. */
    hold_btn(XBOX_GAMEPAD_START, 1);
    st = poll0();
    CHECK(st.Gamepad.wButtons & XBOX_GAMEPAD_START);
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START, 0);
    poll0();

    /* Completed inside the window: the title never sees either member. */
    advance(100);
    hold_btn(XBOX_GAMEPAD_BACK, 1);
    st = poll0();
    CHECK(!(st.Gamepad.wButtons & XBOX_GAMEPAD_BACK));
    advance(20);
    hold_btn(XBOX_GAMEPAD_LEFT_THUMB, 1);
    st = poll0();
    CHECK(!(st.Gamepad.wButtons & (XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB)));
    advance(100);
    st = poll0();
    CHECK(!(st.Gamepad.wButtons & (XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB)));
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB, 0);
    poll0();
    CHECK(menu_fires() == 0);

    /* Out of range: clamped with a line. */
    out = start(ARMED "[input]\nchord_delay_ms = 500\n");
    CHECK(strstr(out, "input.chord_delay_ms=500 out of 0..100; using 100") != NULL);
}

/* ---- 7. a script's pad never fires; a kbm-script key does -------------- */

static void test_scripts(void)
{
    XBOX_INPUT_STATE st;
    struct xbox_host_pad none;
    const char *out;

    start(ARMED "[input.actions]\nmenu.open = [\"a+b@200\", \"F10\"]\n");
    /* RECOMP_INPUT_SCRIPT's pad (cat's glue): no host pad behind it. The
     * state passes untouched and nothing fires. */
    memset(&none, 0, sizeof none);
    none.slot = -1;
    for (int i = 0; i < 4; i++) {
        memset(&st, 0, sizeof st);
        st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
        st.Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 255;
        xbox_enhance_pad_filter(0, &none, &st);
        CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
        CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 255);
        advance(100);
    }
    CHECK(menu_fires() == 0);
    /* The same buttons on the host pad do fire. */
    hold_a(1);
    hold_b(1);
    poll0();
    advance(200);
    poll0();
    CHECK(menu_fires() == 1);
    hold_a(0);
    hold_b(0);
    poll0();

    /* A kbm_script key is a key: it fires. */
    xbox_InputKbmScriptReset();
    CHECK(xbox_InputKbmScriptParse("0p key down F10\n2p key up F10\n", "test") == 2);
    capture_begin();
    for (int i = 0; i < 4; i++) {
        advance(16);
        poll0();
    }
    out = capture_end();
    CHECK(menu_fires() == 1);
    CHECK(strstr(out, "[ACTION] menu.open via F10\n") != NULL);
    xbox_InputKbmScriptReset();
}

/* ---- 8. disarmed: hook=off, nothing filtered or fired ------------------ */

static int s_probe_id = -2;
static int probe_init(const struct xbox_mod_api *api)
{
    static const char *const defs[] = { "F6", "paddle1", NULL };
    s_probe_id = api->action_register("probe.go", "Probe", defs);
    return 0;
}
static const struct xbox_mod k_probe = { "probe", probe_init, NULL, NULL };

static void test_disarmed(void)
{
    static const struct xbox_mod *const mods[] = { &k_probe, NULL };
    XBOX_INPUT_STATE st;
    const char *out;
    int toasts;

    set_env("RECOMP_CONFIG", "none");
    out = start_with(ARMED, &k_fake, mods);
    set_env("RECOMP_CONFIG", NULL);
    CHECK(strstr(out, "[ENHANCE] actions hook=off\n") != NULL);
    CHECK(strstr(out, "hook=on") == NULL);
    CHECK(!enhance_actions_armed());
    CHECK(s_probe_id >= 0);
    CHECK(nv2a_flip_hook_installed == NULL);
    /* The mod's bindings, guide+a and F10: nothing fires, A passes. */
    toasts = s_toasts;
    xbox_InputKeyEvent(VK_F6, 1);
    xbox_InputKeyEvent(VK_F10, 1);
    hold_extra(XBOX_HOST_GUIDE | XBOX_HOST_PADDLE1, 1);
    poll0();
    advance(16);
    hold_a(1);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    enhance_actions_tick();
    nv2a_flip_hook_run();
    CHECK(enhance_action_pressed(s_probe_id) == 0);
    CHECK(menu_fires() == 0);
    CHECK(s_toasts == toasts);
    xbox_InputKeysClear();

    /* No config file at all: off too. */
    out = start(NULL);
    CHECK(strstr(out, "[ENHANCE] actions hook=off\n") != NULL);
    /* A non-feature key does not arm it. */
    out = start("[input.actions]\nmenu.open = [\"paddle1\"]\n");
    CHECK(strstr(out, "[ENHANCE] actions hook=off\n") != NULL);
    hold_extra(XBOX_HOST_PADDLE1, 1);
    poll0();
    CHECK(menu_fires() == 0);
}

/* ---- 9. a host-only single fires, and the fire log's limit ------------- */

static void test_paddle_and_log(void)
{
    const char *out = start(ARMED "[input.actions]\nmenu.open = [\"paddle1\", \"pad2:touchpad\"]\n");
    int fired = 0;

    capture_begin();
    hold_extra(XBOX_HOST_PADDLE1, 1);
    poll0();
    advance(16);
    poll0();
    CHECK(enhance_action_down(enhance_action_find("menu.open")));
    hold_extra(XBOX_HOST_PADDLE1, 0);
    poll0();
    out = capture_end();
    CHECK(menu_fires() == 1);
    CHECK(count(out, "[ACTION] menu.open via paddle1\n") == 1);
    /* Port 1's binding does not answer port 0. */
    hold_extra(XBOX_HOST_TOUCHPAD, 1);
    poll0();
    CHECK(menu_fires() == 0);
    hold_extra(XBOX_HOST_TOUCHPAD, 0);
    poll0();

    /* Twelve fires inside one second: ten lines, then a count. */
    __atomic_store_n(&s_now, 50000, __ATOMIC_RELEASE);
    capture_begin();
    for (int i = 0; i < 12; i++) {
        hold_extra(XBOX_HOST_PADDLE1, 1);
        poll0();
        advance(10);
        hold_extra(XBOX_HOST_PADDLE1, 0);
        poll0();
        advance(10);
    }
    advance(1000);
    hold_extra(XBOX_HOST_PADDLE1, 1);
    poll0();
    out = capture_end();
    hold_extra(XBOX_HOST_PADDLE1, 0);
    poll0();
    fired = menu_fires();
    CHECK(fired == 13);
    CHECK(count(out, "[ACTION] menu.open via paddle1\n") == 11);
    CHECK(strstr(out, "[ACTION] 2 more fires not logged\n") != NULL);
}

/* ---- menu.open's paddle1 default (the Deck's R4) ----------------------- */

static void test_paddle_default(void)
{
    XBOX_INPUT_STATE st;
    const char *out = start(ARMED);     /* no [input.actions]: the defaults */
    int toasts;

    CHECK(strstr(out, "refused") == NULL);
    toasts = s_toasts;
    capture_begin();
    hold_extra(XBOX_HOST_PADDLE1, 1);
    hold_a(1);
    st = poll0();
    out = capture_end();
    CHECK(menu_fires() == 1);
    CHECK(strstr(out, "[ACTION] menu.open via paddle1\n") != NULL);
    /* Until the menu lands, each fire is a toast. */
    CHECK(s_toasts == toasts + 1);
    CHECK(strcmp(s_toast, "Menu is off (menu.enabled)") == 0);
    /* Nothing of the game's is taken: A, held with it, still passes. */
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    advance(16);
    poll0();
    CHECK(menu_fires() == 0);
    hold_extra(XBOX_HOST_PADDLE1, 0);
    hold_a(0);
    poll0();
    advance(16);
    /* A second press is a second fire: the menu closes as it opened. */
    hold_extra(XBOX_HOST_PADDLE1, 1);
    poll0();
    CHECK(menu_fires() == 1);
    hold_extra(XBOX_HOST_PADDLE1, 0);
    poll0();

    /* A loading screen (no guest poll): the flip tick reads the paddle. */
    start_with(ARMED, &k_fake, NULL);
    xbox_InputInit();
    nv2a_flip_hook_run();
    advance(16);
    hold_extra(XBOX_HOST_PADDLE1, 1);
    nv2a_flip_hook_run();
    CHECK(menu_fires() == 1);
    hold_extra(XBOX_HOST_PADDLE1, 0);
    advance(16);
    nv2a_flip_hook_run();

    /* A player's list replaces the defaults, paddle1 with them. */
    start(ARMED "[input.actions]\nmenu.open = [\"F10\"]\n");
    hold_extra(XBOX_HOST_PADDLE1, 1);
    poll0();
    CHECK(menu_fires() == 0);
    hold_extra(XBOX_HOST_PADDLE1, 0);
    poll0();
}

/* A consumed stick direction: guide+left_stick_up takes the left stick's
 * upper half from the game until the stick comes back; the other halves
 * and the other stick pass. With the sticks swapped the half moves to the
 * guest stick the physical one drives. */
static void test_stick_mask(void)
{
    XBOX_INPUT_STATE st;

    start(ARMED "[input.actions]\nmenu.open = [\"guide+left_stick_up\"]\n");
    hold_extra(XBOX_HOST_GUIDE, 1);
    poll0();
    advance(16);
    f_pad[0].Gamepad.sThumbLY = 32767;
    f_pad[0].Gamepad.sThumbLX = 20000;
    f_pad[0].Gamepad.sThumbRY = 12000;
    st = poll0();
    CHECK(menu_fires() == 1);
    CHECK(st.Gamepad.sThumbLY == 0);
    CHECK(st.Gamepad.sThumbLX == 20000);
    CHECK(st.Gamepad.sThumbRY == 12000);
    /* Guide let go: the stick, still up, is still the host's. */
    hold_extra(XBOX_HOST_GUIDE, 0);
    advance(16);
    st = poll0();
    CHECK(st.Gamepad.sThumbLY == 0);
    /* Back to centre and down: the lower half was never taken. */
    f_pad[0].Gamepad.sThumbLY = 0;
    poll0();
    advance(16);
    f_pad[0].Gamepad.sThumbLY = -30000;
    st = poll0();
    CHECK(st.Gamepad.sThumbLY == -30000);
    pad_clear();
    poll0();

    start(ARMED "[input.pad]\nleft_stick = \"right\"\nright_stick = \"left\"\n"
          "[input.actions]\nmenu.open = [\"guide+left_stick_up\"]\n");
    hold_extra(XBOX_HOST_GUIDE, 1);
    poll0();
    advance(16);
    f_pad[0].Gamepad.sThumbLY = 32767;
    f_pad[0].Gamepad.sThumbRY = 12000;
    st = poll0();
    CHECK(menu_fires() == 1);
    CHECK(st.Gamepad.sThumbRY == 0);
    CHECK(st.Gamepad.sThumbLY == 12000);
    pad_clear();
    poll0();
}

/* ---- the overlay.stats action toggles the strip ------------------------ */

static void press_paddle(void)
{
    hold_extra(XBOX_HOST_PADDLE2, 1);
    poll0();
    advance(16);
    hold_extra(XBOX_HOST_PADDLE2, 0);
    poll0();
    advance(16);
}

static void test_stats_toggle(void)
{
    const char *out;

    /* Stock: neither the hook nor the overlay is armed. */
    s_ovl_layers = 0;
    s_ovl_stats = 0;
    out = start(NULL);
    CHECK(s_ovl_layers == 0);
    CHECK(strstr(out, "overlay.stats=off\n") != NULL);

    /* A feature key arms both layers, the strip hidden; each fire flips it. */
    out = start(ARMED "[input.actions]\n\"overlay.stats\" = \"paddle2\"\n");
    CHECK(s_ovl_layers == ((1u << NV2A_OVL_STATS) | (1u << NV2A_OVL_TOAST)));
    CHECK(s_ovl_stats == 0);
    press_paddle();
    CHECK(s_ovl_stats == 1);
    press_paddle();
    CHECK(s_ovl_stats == 0);

    /* overlay.stats on: armed by its own key, shown at start, then off. */
    s_ovl_layers = 0;
    out = start("[overlay]\nstats = true\n[input.actions]\n\"overlay.stats\" = \"paddle2\"\n");
    CHECK(s_ovl_layers == ((1u << NV2A_OVL_STATS) | (1u << NV2A_OVL_TOAST)));
    CHECK(strstr(out, "overlay.stats=on\n") != NULL);
    CHECK(strstr(out, "actions hook=on (overlay.stats)\n") != NULL);
    CHECK(s_ovl_stats == 1);
    press_paddle();
    CHECK(s_ovl_stats == 0);
    s_ovl_stats = 0;
}

/* ---- 10. the flip tick: guide+a with no guest poll --------------------- */

static void test_tick(void)
{
    XBOX_INPUT_STATE st;
    int reads;

    start_with(ARMED, &k_fake, NULL);
    CHECK(nv2a_flip_hook_installed == enhance_actions_tick);
    /* The title brought input up (its XInitDevices) but never polls. */
    xbox_InputInit();
    f_reads = 0;
    hold_extra(XBOX_HOST_GUIDE, 1);
    nv2a_flip_hook_run();
    CHECK(f_reads == 1);
    CHECK(xbox_InputHostExtra(0) == XBOX_HOST_GUIDE);
    CHECK(menu_fires() == 0);
    advance(16);
    hold_a(1);
    nv2a_flip_hook_run();
    CHECK(menu_fires() == 1);
    advance(16);
    nv2a_flip_hook_run();
    CHECK(menu_fires() == 0);
    /* The title polls again: A, pressed with Guide held, stays the host's. */
    hold_extra(XBOX_HOST_GUIDE, 0);
    advance(16);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    /* While polled, the tick leaves the port to the poll. */
    reads = f_reads;
    advance(50);
    nv2a_flip_hook_run();
    CHECK(f_reads == reads);
    hold_a(0);
    poll0();
    advance(16);
    hold_a(1);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    CHECK(menu_fires() == 0);
    hold_a(0);
    poll0();

    /* Stale for 100 ms: the tick reads again. A busy read keeps what the
     * port had. */
    advance(100);
    f_busy = 1;
    reads = f_reads;
    nv2a_flip_hook_run();
    CHECK(f_reads == reads + 1);
    f_busy = 0;

    /* The pad goes away with Guide held: the tick lets Guide go and clears
     * the snapshot, so a glue that reads it (cat's) cannot complete the
     * chord when the pad comes back with A. */
    hold_extra(XBOX_HOST_GUIDE, 1);
    advance(16);
    nv2a_flip_hook_run();
    CHECK(xbox_InputHostExtra(0) == XBOX_HOST_GUIDE);
    f_conn[0] = 0;
    advance(16);
    nv2a_flip_hook_run();
    CHECK(xbox_InputHostExtra(0) == 0);
    f_conn[0] = 1;
    f_extra[0] = 0;
    hold_a(1);
    {
        struct xbox_host_pad h;
        memset(&h, 0, sizeof h);
        h.pad = f_pad[0];
        h.extra = xbox_InputHostExtra(0);
        xbox_PadRawFromGamepad(&h.pad.Gamepad, h.extra, &h.raw);
        h.connected = 1;
        h.slot = 0;
        st = f_pad[0];
        advance(16);
        xbox_enhance_pad_filter(0, &h, &st);
        CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    }
    CHECK(menu_fires() == 0);
}

/* A guest poll that lands while the tick reads the pad: it sees guide+a,
 * then guide and A go up and it polls again. Straight into the filter, as
 * the title's poll on its own thread would reach it. */
static void filter_now(void)
{
    struct xbox_host_pad h;
    XBOX_INPUT_STATE st;

    memset(&h, 0, sizeof h);
    h.pad = f_pad[0];
    h.extra = f_extra[0];
    xbox_PadRawFromGamepad(&h.pad.Gamepad, h.extra, &h.raw);
    h.connected = 1;
    h.slot = 0;
    st = f_pad[0];
    xbox_enhance_pad_filter(0, &h, &st);
}

static void poll_press_release(void)
{
    advance(5);
    filter_now();
    hold_extra(XBOX_HOST_GUIDE, 0);
    hold_a(0);
    advance(5);
    filter_now();
}

static void test_tick_race(void)
{
    start_with(ARMED, &k_fake, NULL);
    xbox_InputInit();
    f_conn[0] = 1;
    memset(&f_pad[0], 0, sizeof f_pad[0]);
    f_extra[0] = 0;
    /* The port is stale, so the tick reads guide+a. The polls during that
     * read step the press and the release; the tick's older read must not
     * step after them, or it would press guide+a a second time. */
    hold_extra(XBOX_HOST_GUIDE, 1);
    filter_now();           /* the modifier first, as a chord needs */
    advance(200);
    hold_a(1);
    f_during_read = poll_press_release;
    nv2a_flip_hook_run();
    CHECK(f_during_read == NULL);
    CHECK(menu_fires() == 1);
    CHECK(enhance_action_down(enhance_action_find("menu.open")) == 0);
}

/* ---- 11. the mod API: a v1 mod, a v2 mod, a failed init ----------------- */

static int s_v1_frames;
static void v1_frame(void *u) { (void)u; s_v1_frames++; }
static int v1_init(const struct xbox_mod_api *api)
{
    /* A mod built against version 1 touches nothing past batch_filter. */
    return api->on_frame(v1_frame, NULL) ? 0 : -1;
}
static const struct xbox_mod k_v1 = { "oldmod", v1_init, NULL, NULL };

static const struct xbox_mod_api *s_v2_api;
static int s_cam_id = -2, s_cam_armed = -1;
static int v2_init(const struct xbox_mod_api *api)
{
    static const char *const defs[] = { "F6", "guide+y", NULL };
    if (api->size < offsetof(struct xbox_mod_api, actions_arm) + sizeof api->actions_arm)
        return -1;
    s_v2_api = api;
    s_cam_id = api->action_register("camera.toggle", "Free camera", defs);
    s_cam_armed = api->actions_arm("camera.free");
    return 0;
}
static const struct xbox_mod k_v2 = { "camera", v2_init, NULL, NULL };

static int bad_init(const struct xbox_mod_api *api)
{
    api->action_register("bad.thing", "Bad", NULL);
    api->actions_arm("bad.on");
    return -1;
}
static const struct xbox_mod k_bad = { "bad", bad_init, NULL, NULL };

static void test_mod_api(void)
{
    static const struct xbox_mod *const mods[] = { &k_v1, &k_v2, &k_bad, NULL };
    const char *out;
    int id;

    CHECK(XBOX_MOD_API_VERSION == 8u);
    out = start_with("[camera]\nfree = true\n", &k_fake, mods);
    CHECK(strstr(out, "[ENHANCE] actions hook=on (camera.free)\n") != NULL);
    CHECK(s_cam_armed == 1);
    CHECK(s_v2_api && s_v2_api->version == XBOX_MOD_API_VERSION);
    s_v1_frames = 0;
    xbox_mod_frame();
    CHECK(s_v1_frames == 1);
    id = enhance_action_find("camera.toggle");
    CHECK(id >= 0 && id == s_cam_id);
    CHECK(enhance_action_find("bad.thing") < 0);
    CHECK(enhance_action_find("menu.open") >= 0);
    /* Registration is init only. */
    CHECK(s_v2_api->action_register("camera.late", "Late", NULL) < 0);
    /* The mod's default binding fires through its own calls. */
    poll0();
    xbox_InputKeyEvent(VK_F6, 1);
    poll0();
    CHECK(s_v2_api->action_down(id));
    xbox_InputKeyEvent(VK_F6, 0);
    poll0();
    CHECK(s_v2_api->action_pressed(id) == 1);
    CHECK(s_v2_api->action_pressed(id) == 0);
    CHECK(!s_v2_api->action_down(id));
    /* guide+y */
    hold_extra(XBOX_HOST_GUIDE, 1);
    poll0();
    f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
    poll0();
    CHECK(s_v2_api->action_pressed(id) == 1);
    pad_clear();
    poll0();
    capture_begin();
    s_v2_api->toast(&k_v2, "free camera %s", "on");
    out = capture_end();
    CHECK(strstr(out, "[MOD camera] toast: free camera on\n") != NULL);

    /* A mod whose init fails takes the key it armed with: the hook stays
     * off. */
    {
        static const struct xbox_mod *const bad_only[] = { &k_bad, NULL };
        out = start_with("[bad]\non = true\n", &k_fake, bad_only);
        CHECK(strstr(out, "[ENHANCE] actions hook=off\n") != NULL);
        CHECK(strstr(out, "bad.on") == NULL);
        CHECK(!enhance_actions_armed());
        CHECK(nv2a_flip_hook_installed == NULL);
    }

    /* An armed key that is false arms nothing. */
    out = start_with("[camera]\nfree = false\n", &k_fake, mods);
    CHECK(strstr(out, "[ENHANCE] actions hook=off\n") != NULL);
    CHECK(s_cam_armed == 0);
    xbox_mod_shutdown();
    xbox_mod_register_table(NULL);
}

/* ---- 12. the seam in xbox_InputGetState (3A.1) ------------------------- */

static struct xbox_host_pad s_seen;
static int s_calls;
static void rewrite(DWORD port, const struct xbox_host_pad *host, XBOX_INPUT_STATE *st)
{
    (void)port;
    s_calls++;
    s_seen = *host;
    st->Gamepad.wButtons &= (WORD)~XBOX_GAMEPAD_START;
}

static void test_seam(void)
{
    XBOX_INPUT_STATE st;

    start(NULL);
    xbox_InputKeyboardEnable(1);
    /* The filter sees the pad before the keyboard merge, with the extra
     * bits, and its change is what the title gets. */
    xbox_InputSetPadFilter(rewrite);
    hold_btn(XBOX_GAMEPAD_START | XBOX_GAMEPAD_BACK, 1);
    hold_extra(XBOX_HOST_PADDLE2, 1);
    xbox_InputKeyEvent('Z', 1);
    s_calls = 0;
    st = poll0();
    CHECK(s_calls == 1);
    CHECK(s_seen.connected && s_seen.slot == 0);
    CHECK(s_seen.extra == XBOX_HOST_PADDLE2);
    CHECK(s_seen.pad.Gamepad.wButtons & XBOX_GAMEPAD_START);
    CHECK(s_seen.pad.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    CHECK(!(st.Gamepad.wButtons & XBOX_GAMEPAD_START));
    CHECK(st.Gamepad.wButtons & XBOX_GAMEPAD_BACK);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] != 0);   /* Z, the keyboard's */
    /* No pad: the filter still runs, with connected 0, on the keyboard's state. */
    f_conn[0] = 0;
    s_calls = 0;
    advance(2000);
    st = poll0();
    CHECK(s_calls == 1 && !s_seen.connected);
    f_conn[0] = 1;
    /* No filter: untouched. */
    xbox_InputSetPadFilter(NULL);
    advance(2000);
    st = poll0();
    CHECK(st.Gamepad.wButtons & XBOX_GAMEPAD_START);
    /* Disarmed, the toolkit's filter is a no-op even when set. */
    xbox_InputSetPadFilter(xbox_enhance_pad_filter);
    st = poll0();
    CHECK(st.Gamepad.wButtons & XBOX_GAMEPAD_START);
    xbox_InputSetPadFilter(NULL);
    xbox_InputKeyboardEnable(0);
    xbox_InputKeysClear();
}

/* ---- 13. a live rebuild while a thread polls and another flips ---------- */

#ifndef _WIN32
static int s_stop;

static void *poll_thread(void *u)
{
    (void)u;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) {
        XBOX_INPUT_STATE st;
        xbox_InputGetState(0, &st);
        advance(1);
    }
    return NULL;
}

static void *flip_thread(void *u)
{
    (void)u;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE))
        nv2a_flip_hook_run();
    return NULL;
}

static void test_live_rebuild(void)
{
    pthread_t p, f;
    int fires = 0;

    /* The keyboard on, so the reload's [input.bindings] swap races the
     * poll's composition too. */
    start_with(ARMED "[input]\nkeyboard = true\n"
               "[input.actions]\nmenu.open = [\"paddle1\", \"back+left_thumb@200\"]\n",
               &k_fake, NULL);
    hold_extra(XBOX_HOST_PADDLE1, 1);
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB, 1);
    __atomic_store_n(&s_stop, 0, __ATOMIC_RELEASE);
    pthread_create(&p, NULL, poll_thread, NULL);
    pthread_create(&f, NULL, flip_thread, NULL);
    for (int i = 0; i < 200; i++) {
        if (i % 20 == 0)
            enhance_input_reload();     /* [input] and [input.actions] */
        else
            enhance_actions_rebuild();
        fires += menu_fires();
    }
    __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
    pthread_join(p, NULL);
    pthread_join(f, NULL);
    fires += menu_fires();
    /* Held since before every rebuild: a new table never fires on it. */
    CHECK(fires == 0);
    CHECK(enhance_action_find("menu.open") >= 0);
    pad_clear();
    poll0();

    /* A rebuild that moves a held binding's bit: the old bit does not stay
     * down, and the held input reads as up until let go and pressed. */
    {
        char txt[4][ENHANCE_ACTION_TEXT];
        int id;
        start(ARMED "[input.actions]\nmenu.open = [\"F8\", \"paddle1\"]\n");
        id = enhance_action_find("menu.open");
        hold_extra(XBOX_HOST_PADDLE1, 1);
        poll0();
        CHECK(enhance_action_down(id));
        CHECK(menu_fires() == 1);
        CHECK(enhance_action_bindings(id, txt, 4) == 2);
        /* Through the writer, as the menu's Save goes, so the reload reads
         * the new list from the loaded copy. */
        {
            const char *k = "input.actions.menu.open", *v = "[\"paddle1\"]";
            char err[256];
            CHECK(enhance_cfg_patch(enhance_cfg_user_path(), &k, &v, 1, err, sizeof err) == 0);
        }
        enhance_input_reload();
        CHECK(enhance_action_bindings(id, txt, 4) == 1 && !strcmp(txt[0], "paddle1"));
        CHECK(!enhance_action_down(id));
        poll0();
        CHECK(!enhance_action_down(id));
        CHECK(menu_fires() == 0);
        hold_extra(XBOX_HOST_PADDLE1, 0);
        poll0();
        CHECK(!enhance_action_down(id));
        hold_extra(XBOX_HOST_PADDLE1, 1);
        poll0();
        CHECK(enhance_action_down(id));
        CHECK(menu_fires() == 1);
        hold_extra(XBOX_HOST_PADDLE1, 0);
        poll0();
        CHECK(!enhance_action_down(id));
    }
}
#endif

/* ---- 14. host-space bindings under [input.pad] (gamepad-input-ux D4) ---- */

static void hold_y(int on) { f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = on ? 255 : 0; }

static void test_host_space(void)
{
    XBOX_INPUT_STATE st;
    xbox_pad_setup ps;
    const char *out;
    int id;

    /* Two unmapped members: both are modifiers and both must be held. */
    start(ARMED "[input.actions]\nmenu.open = [\"guide+paddle1+a\"]\n");
    hold_extra(XBOX_HOST_GUIDE, 1);
    poll0();
    advance(16);
    hold_a(1);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    CHECK(menu_fires() == 0);
    /* paddle1 joining late does not complete it: A was the game's first. */
    hold_extra(XBOX_HOST_PADDLE1, 1);
    advance(16);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    CHECK(menu_fires() == 0);
    hold_a(0);
    poll0();
    advance(16);
    hold_a(1);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    CHECK(menu_fires() == 1);
    pad_clear();
    poll0();

    /* guide mapped to Start: guide+a has every member in the game and no
     * hold, so it goes inactive with a line; the game gets both. paddle1,
     * still unmapped, keeps working. */
    out = start(ARMED "[input.pad]\nstart = \"guide\"\n");
    CHECK(strstr(out, "'guide+a' inactive: every member goes to the game (south (the"
                 " game's a), guide (start))") != NULL);
    CHECK(strstr(out, "'paddle1'") == NULL);
    hold_extra(XBOX_HOST_GUIDE, 1);
    poll0();
    advance(16);
    hold_a(1);
    st = poll0();
    CHECK(st.Gamepad.wButtons & XBOX_GAMEPAD_START);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    advance(500);
    poll0();
    CHECK(menu_fires() == 0);
    pad_clear();
    poll0();
    hold_extra(XBOX_HOST_PADDLE1, 1);
    poll0();
    CHECK(menu_fires() == 1);
    pad_clear();
    poll0();

    /* A source the map leaves out is free for a single binding: Back once
     * the player unbinds it. */
    out = start(ARMED "[input.pad]\nback = []\n[input.actions]\nmenu.open = [\"back\"]\n");
    CHECK(strstr(out, "'back'") == NULL);
    hold_btn(XBOX_GAMEPAD_BACK, 1);
    st = poll0();
    CHECK(menu_fires() == 1);
    CHECK(!(st.Gamepad.wButtons & XBOX_GAMEPAD_BACK));
    pad_clear();
    poll0();

    /* back+left_thumb@400 with Back unbound: a modifier chord that keeps its
     * hold. L3 pressed under Back is the host's at once; the fire waits for
     * the hold. */
    start(ARMED "[input.pad]\nback = []\n[input.actions]\n"
          "menu.open = [\"back+left_thumb@400\"]\n");
    hold_btn(XBOX_GAMEPAD_BACK, 1);
    poll0();
    advance(16);
    hold_btn(XBOX_GAMEPAD_LEFT_THUMB, 1);
    st = poll0();
    CHECK(!(st.Gamepad.wButtons & XBOX_GAMEPAD_LEFT_THUMB));
    advance(399);
    poll0();
    CHECK(menu_fires() == 0);
    advance(1);
    st = poll0();
    CHECK(menu_fires() == 1);
    CHECK(!(st.Gamepad.wButtons & XBOX_GAMEPAD_LEFT_THUMB));
    pad_clear();
    poll0();
    /* L3 first: it reaches the game and Back joining does not fire. */
    advance(16);
    hold_btn(XBOX_GAMEPAD_LEFT_THUMB, 1);
    poll0();
    advance(16);
    hold_btn(XBOX_GAMEPAD_BACK, 1);
    advance(500);
    st = poll0();
    CHECK(st.Gamepad.wButtons & XBOX_GAMEPAD_LEFT_THUMB);
    CHECK(menu_fires() == 0);
    pad_clear();
    poll0();

    /* Masking goes through both maps: paddle2 is A's second source, so a
     * chord that consumes paddle2 takes A from the game, and the button the
     * menu closed on is masked whichever source holds it. */
    start(ARMED "[input.pad]\na = [\"south\", \"paddle2\"]\n[input.actions]\n"
          "menu.open = [\"guide+paddle2\"]\n");
    hold_extra(XBOX_HOST_GUIDE, 1);
    poll0();
    advance(16);
    hold_extra(XBOX_HOST_PADDLE2, 1);
    st = poll0();
    CHECK(menu_fires() == 1);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    pad_clear();
    poll0();
    advance(16);
    hold_extra(XBOX_HOST_PADDLE2, 1);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    enhance_actions_mask_until_release(0, A_BIT);
    advance(16);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    pad_clear();
    poll0();

    /* paddle1 mapped, remapped and unmapped by a live map change (what the
     * Controls page does: a new setup, then a rebuild): the kind is derived
     * at every rebuild, one line per change. */
    start(ARMED "[input.actions]\nmenu.open = [\"paddle1\", \"paddle1+y\", \"paddle1+b@300\"]\n");
    id = enhance_action_find("menu.open");
    CHECK(id >= 0);
    xbox_PadSetupIdentity(&ps);
    ps.map.source[INPUT_X] = INPUT_SRC_PADDLE1;
    xbox_InputSetPadSetup(&ps);
    capture_begin();
    enhance_actions_rebuild();
    out = capture_end();
    CHECK(strstr(out, "'paddle1' inactive: paddle1 (the game's x) goes to the game")
          != NULL);
    CHECK(strstr(out, "(was host-only)\n") != NULL);
    CHECK(strstr(out, "'paddle1+y' inactive: every member goes to the game") != NULL);
    CHECK(strstr(out, "(was a modifier chord)\n") != NULL);
    CHECK(strstr(out, "'paddle1+b@300' is now a guest chord (was a modifier chord)\n")
          != NULL);
    hold_extra(XBOX_HOST_PADDLE1, 1);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_X] == 255);
    advance(16);
    hold_y(1);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_Y] == 255);
    CHECK(menu_fires() == 0);
    pad_clear();
    poll0();

    /* Remapped to Y: still inactive, now naming y; nothing changes kind. */
    xbox_PadSetupIdentity(&ps);
    ps.map.source[INPUT_Y] = INPUT_SRC_PADDLE1;
    xbox_InputSetPadSetup(&ps);
    capture_begin();
    enhance_actions_rebuild();
    out = capture_end();
    CHECK(strstr(out, "'paddle1' inactive: paddle1 (the game's y) goes to the game")
          != NULL);
    CHECK(strstr(out, "'paddle1' inactive: paddle1 (the game's y) goes to the game"
                 " (was") == NULL);
    /* paddle1+y: Y's own source (north) is free now, so this is a modifier
     * chord with north the modifier. */
    CHECK(strstr(out, "'paddle1+y' is now a modifier chord (was inactive)\n") != NULL);
    CHECK(strstr(out, "'paddle1+b@300' is now") == NULL);

    /* Unmapped again: back to host-only and a modifier chord. */
    xbox_InputSetPadSetup(NULL);
    capture_begin();
    enhance_actions_rebuild();
    out = capture_end();
    CHECK(strstr(out, "'paddle1' is now host-only (was inactive)\n") != NULL);
    CHECK(strstr(out, "'paddle1+b@300' is now a modifier chord (was a guest chord)\n")
          != NULL);
    poll0();    /* a new table fires only on a press it saw start */
    advance(16);
    hold_extra(XBOX_HOST_PADDLE1, 1);
    st = poll0();
    /* paddle1 is inside paddle1+b@300 and paddle1+y: the longest match
     * wins, so paddle1 alone fires at its release. */
    CHECK(menu_fires() == 0);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_X] == 0);
    pad_clear();
    poll0();
    CHECK(menu_fires() == 1);
    remove("config/config.toml");
}

/* ---- one input, one action ---------------------------------------------- */

static int stats_fires(void)
{
    return enhance_action_pressed(enhance_action_find("overlay.stats"));
}

static void tap_key(int vk)
{
    xbox_InputKeyEvent(vk, 1);
    poll0();
    advance(16);
    xbox_InputKeyEvent(vk, 0);
    poll0();
    advance(16);
}

/* The Deck case: a capture saved overlay.stats = "F10" beside menu.open's
 * default F10, and every F10 fired both. Now one binding keeps the input:
 * menu.open's, else the written one over a default, else the first
 * registered. */
static void test_one_input(void)
{
    static const struct xbox_mod *const mods[] = { &k_probe, &k_v2, NULL };
    char bound[ENHANCE_ACTION_TEXT];
    const char *out;
    int menu, stats;

    /* menu.open never gives way, even to a written binding: under Proton
     * the Deck's grip (F10) is the one way into the menu. */
    out = start(ARMED "[input.actions]\n\"overlay.stats\" = \"F10\"\n");
    CHECK(strstr(out, "[INPUT] action overlay.stats: 'F10' refused: menu.open has it"
                 " ('F10'); one input fires one action\n") != NULL);
    CHECK(strstr(out, "gives way") == NULL);
    tap_key(VK_F10);
    CHECK(menu_fires() == 1);
    CHECK(stats_fires() == 0);

    /* Any other pair: the written one beats a default. probe.go and
     * camera.toggle both default to F6, and the first registered keeps it;
     * menu.open keeps paddle1 from probe.go. */
    out = start_with(ARMED, &k_fake, mods);
    poll0();
    CHECK(strstr(out, "[INPUT] action camera.toggle: 'F6' refused: probe.go has it ('F6')")
          != NULL);
    CHECK(strstr(out, "[INPUT] action probe.go: 'paddle1' refused: menu.open has it") != NULL);
    tap_key(VK_F6);
    CHECK(enhance_action_pressed(s_probe_id) == 1);
    CHECK(enhance_action_pressed(s_cam_id) == 0);
    hold_extra(XBOX_HOST_PADDLE1, 1);
    poll0();
    hold_extra(XBOX_HOST_PADDLE1, 0);
    poll0();
    CHECK(menu_fires() == 1);
    CHECK(enhance_action_pressed(s_probe_id) == 0);
    /* The toolkit's written binding takes a mod's default. */
    out = start_with(ARMED "[input.actions]\n\"overlay.stats\" = \"F6\"\n", &k_fake, mods);
    poll0();
    CHECK(strstr(out, "[INPUT] action probe.go: 'F6' refused: overlay.stats has it") != NULL);
    CHECK(strstr(out, "[INPUT] action camera.toggle: 'F6' refused: overlay.stats has it")
          != NULL);
    tap_key(VK_F6);
    CHECK(stats_fires() == 1);
    CHECK(enhance_action_pressed(s_probe_id) == 0);
    CHECK(enhance_action_pressed(s_cam_id) == 0);
    /* A mod's written binding takes another mod's default, though that one
     * was registered first. */
    out = start_with(ARMED "[input.actions]\n\"camera.toggle\" = \"F6\"\n", &k_fake, mods);
    poll0();
    CHECK(strstr(out, "[INPUT] action probe.go (default): 'F6' gives way to"
                 " camera.toggle ('F6')\n") != NULL);
    tap_key(VK_F6);
    CHECK(enhance_action_pressed(s_cam_id) == 1);
    CHECK(enhance_action_pressed(s_probe_id) == 0);
    /* Both written beside a default: the first written keeps it, and no
     * line says the second one won anything. */
    out = start_with(ARMED "[input.actions]\n\"overlay.stats\" = \"F6\"\n"
                     "\"camera.toggle\" = \"F6\"\n", &k_fake, mods);
    poll0();
    CHECK(strstr(out, "[INPUT] action camera.toggle: 'F6' refused: overlay.stats has it") != NULL);
    CHECK(strstr(out, "gives way to camera.toggle") == NULL);
    tap_key(VK_F6);
    CHECK(stats_fires() == 1);
    CHECK(enhance_action_pressed(s_cam_id) == 0);
    xbox_mod_shutdown();
    xbox_mod_register_table(NULL);

    /* Modifiers by side, as key_cond reads them: LAlt+F7 and RAlt+F7 are
     * two inputs, Alt+F7 is either. */
    out = start(ARMED "[input.actions]\n\"overlay.stats\" = [\"LAlt+F7\"]\n"
                "\"shot.window\" = [\"RAlt+F7\"]\n");
    CHECK(strstr(out, "refused") == NULL);
    out = start(ARMED "[input.actions]\n\"overlay.stats\" = [\"LAlt+F7\"]\n"
                "\"shot.window\" = [\"Alt+F7\"]\n");
    CHECK(strstr(out, "'Alt+F7' refused: overlay.stats has it") != NULL);

    /* Both written: menu.open was registered first and keeps it. The same
     * key with a modifier is another input and stays. */
    out = start(ARMED "[input.actions]\nmenu.open = [\"F10\"]\n"
                "\"overlay.stats\" = [\"F10\", \"Alt+F10\"]\n");
    CHECK(strstr(out, "[INPUT] action overlay.stats: 'F10' refused: menu.open has it"
                 " ('F10'); one input fires one action\n") != NULL);
    CHECK(strstr(out, "'Alt+F10' refused") == NULL);
    tap_key(VK_F10);
    CHECK(menu_fires() == 1);
    CHECK(stats_fires() == 0);
    xbox_InputKeyEvent(XBOX_VK_LMENU, 1);
    tap_key(VK_F10);
    xbox_InputKeysClear();
    poll0();
    CHECK(menu_fires() == 0);
    CHECK(stats_fires() == 1);

    /* A pad chord with another hold is the same input; its order and the
     * hold do not matter. */
    out = start(ARMED "[input.actions]\nmenu.open = [\"left_trigger+right_trigger@400\"]\n"
                "\"overlay.stats\" = [\"right_trigger+left_trigger@200\"]\n");
    CHECK(strstr(out, "'right_trigger+left_trigger@200' refused: menu.open has it") != NULL);

    /* What the menu's capture asks. */
    start(ARMED);
    menu = enhance_action_find("menu.open");
    stats = enhance_action_find("overlay.stats");
    CHECK(enhance_action_owner("F10", stats, bound, sizeof bound) == menu);
    CHECK(!strcmp(bound, "F10"));
    CHECK(enhance_action_owner("a+guide", stats, bound, sizeof bound) == menu);
    CHECK(!strcmp(bound, "guide+a"));
    CHECK(enhance_action_owner("paddle1", stats, NULL, 0) == menu);
    CHECK(enhance_action_owner("F10", menu, bound, sizeof bound) == -1);
    CHECK(bound[0] == 0);
    CHECK(enhance_action_owner("Alt+F10", stats, NULL, 0) == -1);
    CHECK(enhance_action_owner("guide", stats, NULL, 0) == -1);
    CHECK(enhance_action_owner("pad2:paddle1", stats, NULL, 0) == -1);
    CHECK(enhance_action_owner("nonsense+", stats, NULL, 0) == -1);
}

/* The longest match: guide inside guide+a fires at its release, and not at
 * all when guide+a fired while it was held. */
static void test_longest_match(void)
{
    XBOX_INPUT_STATE st;
    int stats;

    start(ARMED "[input.actions]\nmenu.open = [\"guide+a\"]\n"
          "\"overlay.stats\" = [\"guide\"]\n");
    stats = enhance_action_find("overlay.stats");
    /* Guide, then A: only menu.open. */
    hold_extra(XBOX_HOST_GUIDE, 1);
    poll0();
    CHECK(stats_fires() == 0);
    advance(16);
    hold_a(1);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    CHECK(menu_fires() == 1);
    CHECK(!enhance_action_down(stats));
    advance(16);
    pad_clear();
    poll0();
    CHECK(stats_fires() == 0);
    CHECK(menu_fires() == 0);

    /* Guide alone: overlay.stats at the release, never down. */
    advance(16);
    hold_extra(XBOX_HOST_GUIDE, 1);
    poll0();
    advance(500);
    poll0();
    CHECK(stats_fires() == 0);
    CHECK(!enhance_action_down(stats));
    hold_extra(XBOX_HOST_GUIDE, 0);
    poll0();
    CHECK(stats_fires() == 1);
    CHECK(menu_fires() == 0);

    /* Both in one poll, and released in one: still only the longer. */
    advance(16);
    hold_extra(XBOX_HOST_GUIDE, 1);
    hold_a(1);
    poll0();
    advance(16);
    pad_clear();
    poll0();
    CHECK(menu_fires() == 1);
    CHECK(stats_fires() == 0);

    /* Guest chords with holds: back+start@400 inside a longer chord fires
     * at its release, only after its hold, and not when the longer fired. */
    start(ARMED "[input.actions]\nmenu.open = [\"back+start+left_thumb@400\"]\n"
          "\"overlay.stats\" = [\"back+start@400\"]\n");
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START, 1);
    poll0();
    advance(100);
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START, 0);
    poll0();
    CHECK(stats_fires() == 0);      /* let go before its hold */
    advance(16);
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START, 1);
    poll0();
    advance(400);
    poll0();
    CHECK(stats_fires() == 0);
    advance(16);
    hold_btn(XBOX_GAMEPAD_LEFT_THUMB, 1);
    poll0();
    advance(400);
    poll0();
    CHECK(menu_fires() == 1);
    hold_btn(0xFFFF, 0);
    poll0();
    CHECK(stats_fires() == 0);
    advance(16);
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START, 1);
    poll0();
    advance(400);
    poll0();
    hold_btn(0xFFFF, 0);
    poll0();
    CHECK(stats_fires() == 1);
    CHECK(menu_fires() == 0);
}

/* A longer chord held whole takes the press, fired or not: let go before
 * its hold, neither it nor the shorter one inside fires. */
static void test_longest_spent(void)
{
    static const struct xbox_mod *const mods[] = { &k_probe, NULL };
    XBOX_INPUT_STATE st;
    int probe, stats;

    start(ARMED "[input.actions]\nmenu.open = [\"back+start+left_thumb@400\"]\n"
          "\"overlay.stats\" = [\"back+start@400\"]\n");
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START, 1);
    poll0();
    advance(400);
    poll0();
    advance(16);
    hold_btn(XBOX_GAMEPAD_LEFT_THUMB, 1);
    poll0();
    advance(200);
    poll0();
    hold_btn(0xFFFF, 0);
    poll0();
    CHECK(menu_fires() == 0);
    CHECK(stats_fires() == 0);

    /* Three deep: guide inside guide+a inside guide+a+b. */
    start_with(ARMED "[input.actions]\nmenu.open = [\"guide+a+b\"]\n"
               "\"overlay.stats\" = [\"guide+a\"]\n\"probe.go\" = [\"guide\"]\n",
               &k_fake, mods);
    poll0();
    probe = s_probe_id;
    stats = enhance_action_find("overlay.stats");
    /* All three in one poll and let go in one: only the longest. */
    hold_extra(XBOX_HOST_GUIDE, 1);
    hold_a(1);
    hold_b(1);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 0);
    advance(16);
    pad_clear();
    poll0();
    CHECK(menu_fires() == 1);
    CHECK(stats_fires() == 0);
    CHECK(enhance_action_pressed(probe) == 0);
    /* guide+a let go in one poll: the longer of the two inside guide+a+b. */
    advance(16);
    hold_extra(XBOX_HOST_GUIDE, 1);
    hold_a(1);
    poll0();
    advance(500);
    poll0();
    CHECK(!enhance_action_down(stats));
    pad_clear();
    poll0();
    CHECK(stats_fires() == 1);
    CHECK(enhance_action_pressed(probe) == 0);
    CHECK(menu_fires() == 0);
    xbox_mod_shutdown();
    xbox_mod_register_table(NULL);

    /* A rebuild while a shadowed binding is held past its hold: its
     * release fires nothing. */
    start(ARMED "[input.actions]\nmenu.open = [\"guide+a\"]\n"
          "\"overlay.stats\" = [\"guide\"]\n");
    hold_extra(XBOX_HOST_GUIDE, 1);
    poll0();
    advance(500);
    poll0();
    enhance_actions_rebuild();
    advance(16);
    poll0();
    hold_extra(XBOX_HOST_GUIDE, 0);
    poll0();
    CHECK(stats_fires() == 0);
}

/* Ports apart: a binding on pad 2 is neither shadowed by nor the same
 * input as one on pad 1. */
static void test_longest_ports(void)
{
    XBOX_INPUT_STATE st;
    const char *out;
    int menu, stats;

    out = start(ARMED "[input.actions]\nmenu.open = [\"guide+a\"]\n"
                "\"overlay.stats\" = [\"pad2:guide+a\", \"pad2:guide\"]\n");
    CHECK(strstr(out, "refused") == NULL);
    menu = enhance_action_find("menu.open");
    stats = enhance_action_find("overlay.stats");
    CHECK(enhance_action_owner("pad2:guide+a", menu, NULL, 0) == stats);
    CHECK(enhance_action_owner("guide+a", stats, NULL, 0) == menu);
    /* pad2:guide is inside pad2:guide+a, and only that one. */
    f_conn[1] = 1;
    advance(1000);              /* past the probe interval of an empty port */
    memset(&st, 0, sizeof st);
    CHECK(xbox_InputGetState(1, &st) == ERROR_SUCCESS);
    advance(16);
    f_extra[1] = XBOX_HOST_GUIDE;
    xbox_InputGetState(1, &st);
    CHECK(stats_fires() == 0);
    advance(500);
    xbox_InputGetState(1, &st);
    f_extra[1] = 0;
    xbox_InputGetState(1, &st);
    CHECK(stats_fires() == 1);
    /* Guide on pad 1 fires nothing of pad 2's. */
    advance(16);
    hold_extra(XBOX_HOST_GUIDE, 1);
    poll0();
    advance(16);
    hold_a(1);
    poll0();
    CHECK(menu_fires() == 1);
    CHECK(stats_fires() == 0);
    pad_clear();
    poll0();
    f_conn[1] = 0;
}

/* The flip tick, with a title that never polls, follows rules 9 and 10. */
static void test_tick_rules(void)
{
    start_with(ARMED "[input.actions]\nmenu.open = [\"guide+a\", \"paddle1\"]\n"
               "\"overlay.stats\" = [\"guide\"]\n", &k_fake, NULL);
    xbox_InputInit();
    pad_clear();
    nv2a_flip_hook_run();
    advance(16);
    /* Guide alone: at the release. */
    hold_extra(XBOX_HOST_GUIDE, 1);
    nv2a_flip_hook_run();
    advance(500);
    nv2a_flip_hook_run();
    CHECK(stats_fires() == 0);
    advance(16);
    hold_extra(XBOX_HOST_GUIDE, 0);
    nv2a_flip_hook_run();
    CHECK(stats_fires() == 1);
    /* Guide, then A: only menu.open. */
    advance(16);
    hold_extra(XBOX_HOST_GUIDE, 1);
    nv2a_flip_hook_run();
    advance(16);
    hold_a(1);
    nv2a_flip_hook_run();
    CHECK(menu_fires() == 1);
    advance(16);
    pad_clear();
    nv2a_flip_hook_run();
    CHECK(stats_fires() == 0);
    /* Quiet: paddle1 held through a capture fires only once let go and
     * pressed again. */
    advance(16);
    enhance_actions_quiet(1);
    hold_extra(XBOX_HOST_PADDLE1, 1);
    nv2a_flip_hook_run();
    advance(16);
    nv2a_flip_hook_run();
    enhance_actions_quiet(0);
    advance(16);
    nv2a_flip_hook_run();
    CHECK(menu_fires() == 0);
    advance(16);
    hold_extra(XBOX_HOST_PADDLE1, 0);
    nv2a_flip_hook_run();
    CHECK(menu_fires() == 0);
    advance(16);
    hold_extra(XBOX_HOST_PADDLE1, 1);
    nv2a_flip_hook_run();
    CHECK(menu_fires() == 1);
    pad_clear();
    advance(16);
    nv2a_flip_hook_run();
}

/* The menu's capture: nothing fires while it runs, and an input held
 * through its end fires only after a release. */
static void test_quiet(void)
{
    start(ARMED);
    enhance_actions_quiet(1);
    xbox_InputKeyEvent(VK_F10, 1);
    poll0();
    advance(16);
    poll0();
    CHECK(menu_fires() == 0);
    enhance_actions_quiet(0);
    advance(16);
    poll0();
    CHECK(menu_fires() == 0);
    xbox_InputKeyEvent(VK_F10, 0);
    poll0();
    CHECK(menu_fires() == 0);
    tap_key(VK_F10);
    CHECK(menu_fires() == 1);

    /* A tap of a shorter binding released during a capture is spent too. */
    start(ARMED "[input.actions]\nmenu.open = [\"guide+a\"]\n"
          "\"overlay.stats\" = [\"guide\"]\n");
    hold_extra(XBOX_HOST_GUIDE, 1);
    poll0();
    enhance_actions_quiet(1);
    advance(16);
    hold_extra(XBOX_HOST_GUIDE, 0);
    poll0();
    enhance_actions_quiet(0);
    CHECK(stats_fires() == 0);
    xbox_InputKeysClear();

    /* A key that went down after the last step and ended the capture
     * (the menu reads keys after the step): the step after quiet still
     * drops it. */
    start(ARMED);
    enhance_actions_quiet(1);
    poll0();
    xbox_InputKeyEvent(VK_F10, 1);
    enhance_actions_quiet(0);
    advance(16);
    poll0();
    CHECK(menu_fires() == 0);
    xbox_InputKeyEvent(VK_F10, 0);
    poll0();
    CHECK(menu_fires() == 0);
    tap_key(VK_F10);
    CHECK(menu_fires() == 1);
    /* Off when it was not on takes effect at once. */
    enhance_actions_quiet(0);
    tap_key(VK_F10);
    CHECK(menu_fires() == 1);

    /* The drain is port 0's: with port 0 never stepped, pad 2 fires at
     * once after the capture. */
    start(ARMED "[input.actions]\n\"overlay.stats\" = [\"pad2:paddle1\"]\n");
    {
        XBOX_INPUT_STATE st;
        f_conn[1] = 1;
        advance(1000);          /* past the probe interval of an empty port */
        CHECK(xbox_InputGetState(1, &st) == ERROR_SUCCESS);
        enhance_actions_quiet(1);
        enhance_actions_quiet(0);
        advance(16);
        f_extra[1] = XBOX_HOST_PADDLE1;
        xbox_InputGetState(1, &st);
        CHECK(stats_fires() == 1);
        f_extra[1] = 0;
        xbox_InputGetState(1, &st);
        f_conn[1] = 0;
    }
}

/* ---- descriptor mods: dormant actions (mod-settings D3) --------------- */

/* A descriptor mod whose switch is live: its ACTION control is registered
 * before init and is dormant while the switch is off. */
static const char *const k_fly_defs[] = { "F6", NULL };
static const struct xbox_mod_setting k_fly_set[] = {
    { "free", "fly.free", NULL, NULL, NULL, NULL, NULL,
      XBOX_SET_BOOL, ENHANCE_LIVE, XBOX_SET_ENABLE, 0, 0, 0, 0, NULL },
};
static const struct xbox_mod_control k_fly_ctl[] = {
    { "toggle", "Free camera", NULL, NULL, XBOX_CTL_ACTION, k_fly_defs, NULL, "RECOMP_EXT_VMA" },
};
static const struct xbox_mod_desc k_fly_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc), NULL, NULL, NULL, NULL,
    k_fly_set, 1, k_fly_ctl, 1
};
static int s_fly_inits;
static void fly_enable(int on, void *u) { (void)on; (void)u; }
static int fly_init(const struct xbox_mod_api *api)
{
    s_fly_inits++;
    api->on_enable(fly_enable, NULL);
    api->actions_arm("fly.free");
    return 0;
}
static const struct xbox_mod k_fly = { "fly", fly_init, NULL, &k_fly_desc };

static void test_dormant(void)
{
    static const struct xbox_mod *const mods[] = { &k_fly, &k_probe, NULL };
    char b[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];
    char bound[ENHANCE_ACTION_TEXT];
    const char *out;
    int fly, id;

    /* Off, the menu off: not inited, nothing arms the hook, and F6 fires
     * nothing; the dormant default still keeps F6 from probe.go's. */
    s_fly_inits = 0;
    out = start_with(NULL, &k_fake, mods);
    poll0();
    fly = 0;
    id = enhance_action_find("fly.toggle");
    CHECK(id >= 0 && xbox_mod_control_action(fly, 0) == id);
    CHECK(s_fly_inits == 0);
    CHECK(strstr(out, "[ENHANCE] fly.free=off\n") != NULL);
    CHECK(strstr(out, "[ENHANCE] actions hook=off\n") != NULL);
    CHECK(strstr(out, "[INPUT] action probe.go: 'F6' refused: fly.toggle has it ('F6')")
          != NULL);
    CHECK(strstr(out, "[MOD] F6: fly.toggle keeps it over probe.go (dormant)\n") != NULL);
    CHECK(enhance_action_dormant(id));
    CHECK(enhance_action_owner("F6", -1, bound, sizeof bound) == id);
    tap_key(VK_F6);
    CHECK(enhance_action_pressed(id) == 0);
    CHECK(enhance_action_pressed(s_probe_id) == 0);

    /* The menu on: inited dormant, the hook armed by the menu only; F6
     * still fires nothing. */
    s_fly_inits = 0;
    out = start_with(ARMED, &k_fake, mods);
    poll0();
    id = enhance_action_find("fly.toggle");
    CHECK(s_fly_inits == 1);
    CHECK(!strcmp(xbox_mod_init_outcome(fly), "dormant"));
    CHECK(strstr(out, "[ENHANCE] actions hook=on (menu.enabled)\n") != NULL);
    CHECK(enhance_action_dormant(id));
    tap_key(VK_F6);
    CHECK(enhance_action_pressed(id) == 0);
    CHECK(enhance_action_pressed(s_probe_id) == 0);
    /* The live switch: the table is rebuilt, F6 stays where it was and
     * now fires; the mod's key arms the hook beside the menu's. */
    xbox_mod_setting_set(fly, 0, 1);
    CHECK(!enhance_action_dormant(id));
    CHECK(enhance_action_bindings(id, b, ENHANCE_ACTION_BINDS) == 1 && !strcmp(b[0], "F6"));
    CHECK(enhance_action_owner("F6", -1, bound, sizeof bound) == id);
    CHECK(enhance_actions_armed());
    poll0();
    tap_key(VK_F6);
    CHECK(enhance_action_pressed(id) == 1);
    CHECK(enhance_action_pressed(s_probe_id) == 0);
    /* And off again: dormant, F6 held, nothing fires. */
    xbox_mod_setting_set(fly, 0, 0);
    CHECK(enhance_action_dormant(id));
    poll0();
    tap_key(VK_F6);
    CHECK(enhance_action_pressed(id) == 0);
    CHECK(enhance_action_owner("F6", -1, bound, sizeof bound) == id);

    /* On at start: inited, its key arms the hook. */
    out = start_with("[fly]\nfree = true\n", &k_fake, mods);
    CHECK(strstr(out, "[ENHANCE] fly.free=on\n") != NULL);
    CHECK(strstr(out, "[ENHANCE] actions hook=on (fly.free)\n") != NULL);
    CHECK(!enhance_action_dormant(enhance_action_find("fly.toggle")));

    /* The env row replaces the bindings; F6 goes back to probe.go. */
    set_env("RECOMP_EXT_VMA", "F8");
    out = start_with("[fly]\nfree = true\n", &k_fake, mods);
    poll0();
    id = enhance_action_find("fly.toggle");
    CHECK(enhance_action_bindings(id, b, ENHANCE_ACTION_BINDS) == 1 && !strcmp(b[0], "F8"));
    CHECK(enhance_action_owner("F6", -1, bound, sizeof bound) == s_probe_id);
    tap_key(VK_F8);
    CHECK(enhance_action_pressed(id) == 1);
    set_env("RECOMP_EXT_VMA", NULL);
}

/* ---- the arm keys switched live while another thread reads them ------- */

#ifndef _WIN32
static int s_arm_miss;

static void *keyed_thread(void *u)
{
    (void)u;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) {
        /* The menu's key never leaves the list: a torn compaction in
         * disarm_match would drop it for a read. */
        if (!enhance_actions_keyed("menu.enabled"))
            __atomic_store_n(&s_arm_miss, 1, __ATOMIC_RELEASE);
        enhance_actions_keyed("fly.free");
    }
    return NULL;
}

static void test_live_arm(void)
{
    static const struct xbox_mod *const mods[] = { &k_fly, &k_probe, NULL };
    pthread_t k, p;

    start_with(ARMED, &k_fake, mods);
    __atomic_store_n(&s_stop, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_arm_miss, 0, __ATOMIC_RELEASE);
    pthread_create(&k, NULL, keyed_thread, NULL);
    pthread_create(&p, NULL, poll_thread, NULL);
    for (int i = 0; i < 200; i++)
        xbox_mod_setting_set(0, 0, (double)((i + 1) & 1));   /* ends off */
    __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
    pthread_join(k, NULL);
    pthread_join(p, NULL);
    CHECK(!__atomic_load_n(&s_arm_miss, __ATOMIC_ACQUIRE));
    CHECK(enhance_actions_keyed("menu.enabled") && !enhance_actions_keyed("fly.free"));
    CHECK(enhance_actions_armed());
}
#endif

/* ---- captured controls: the port-0 record (mod-settings D1, D3) -------- */

static const char *const k_rec_fly[] = { "guide+y", NULL };
static const char *const k_rec_sink[] = { "y", NULL };
static const char *const k_rec_rise[] = { "rt", "R", NULL };
static const char *const k_rec_move[] = { "left_stick", NULL };
static const struct xbox_mod_control k_rec_ctl[] = {
    { "fly", "Fly", NULL, NULL, XBOX_CTL_ACTION, k_rec_fly, NULL, NULL },
    { "sink", "Sink", NULL, NULL, XBOX_CTL_BUTTON, k_rec_sink, NULL, NULL },
    { "rise", "Rise", NULL, NULL, XBOX_CTL_BUTTON, k_rec_rise, NULL, NULL },
    { "move", "Move", NULL, NULL, XBOX_CTL_STICK, k_rec_move, NULL, NULL },
};
static const struct xbox_mod_desc k_rec_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc), "Rec", NULL, NULL, NULL,
    NULL, 0, k_rec_ctl, 4
};
static const struct xbox_mod_api *s_rec;
static int rec_init(const struct xbox_mod_api *api) { s_rec = api; return 0; }
static const struct xbox_mod k_rec = { "rec", rec_init, NULL, &k_rec_desc };
static const struct xbox_mod *const k_recs[] = { &k_rec, NULL };

static int rec_value(uint32_t idx, struct xbox_ctl_value *v)
{
    memset(v, 0x55, sizeof *v);
    return s_rec->control_value(&k_rec_desc, idx, v);
}

static void hold_rt(int on) { f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = on ? 200 : 0; }

/* With the hook disarmed: no record until a capture, then a neutral pad
 * whose packet moves as capture starts and ends, the pad's own values to
 * the mod, a pulled pad read as gone, a host key read with no pad. */
static void test_record(void)
{
    struct xbox_ctl_value v;
    XBOX_INPUT_STATE st;
    DWORD pk;
    const char *out = start_with("[input.controls]\n\"rec.rise\" = [\"rt\", \"R\"]\n", &k_fake,
                                 k_recs);

    CHECK(strstr(out, "hook=off") != NULL);
    /* [input.controls] keys are no action names. */
    CHECK(strstr(out, "no action named") == NULL);
    poll0();
    hold_y(1);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_Y] == 255);
    CHECK(rec_value(1, &v) == 0 && v.raw[0] == 0 && !v.connected);

    pk = st.dwPacketNumber;
    s_rec->input_capture(1);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_Y] == 0 && st.Gamepad.wButtons == 0);
    CHECK(st.dwPacketNumber != pk);
    CHECK(rec_value(1, &v) == 1 && v.raw[0] == 255 && v.connected);
    /* Another port is the game's: no neutral pad, the record untouched. */
    CHECK(xbox_enhance_pad_seen(1, &st.Gamepad, 1) == 0);
    CHECK(rec_value(1, &v) == 1 && v.raw[0] == 255);
    /* The largest of rt (200) and R (down). */
    hold_rt(1);
    poll0();
    CHECK(rec_value(2, &v) == 1 && v.raw[0] == 200);
    xbox_InputKeyEvent('R', 1);
    CHECK(rec_value(2, &v) == 1 && v.raw[0] == 255);
    xbox_InputKeyEvent('R', 0);
    hold_rt(0);
    f_pad[0].Gamepad.sThumbLX = 20000;
    f_pad[0].Gamepad.sThumbLY = -9000;
    st = poll0();
    CHECK(st.Gamepad.sThumbLX == 0 && st.Gamepad.sThumbLY == 0);
    CHECK(rec_value(3, &v) == 1 && v.raw[0] == 20000 && v.raw[1] == -9000
          && v.src == XBOX_CTL_SRC_LEFT);

    /* Pulled: the title's poll fails as it would, the mod reads it gone;
     * a host key still reads. */
    f_conn[0] = 0;
    CHECK(xbox_InputGetState(0, &st) != ERROR_SUCCESS);
    CHECK(rec_value(1, &v) == 0 && v.raw[0] == 0 && !v.connected);
    xbox_InputKeyEvent('R', 1);
    CHECK(rec_value(2, &v) == 1 && v.raw[0] == 255 && !v.connected);
    xbox_InputKeyEvent('R', 0);
    f_conn[0] = 1;
    advance(XBOX_INPUT_PROBE_MS + 1);

    /* Capture ends: the pad is the title's again, on a new packet. */
    st = poll0();
    pk = st.dwPacketNumber;
    s_rec->input_capture(0);
    st = poll0();
    CHECK(st.dwPacketNumber != pk);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_Y] == 255 && st.Gamepad.sThumbLX == 20000);
    /* An old record never reads after the capture that made it. */
    CHECK(rec_value(1, &v) == 0 && v.raw[0] == 0);
    s_rec->input_capture(1);
    CHECK(rec_value(1, &v) == 0 && v.raw[0] == 0);
    st = poll0();
    CHECK(rec_value(1, &v) == 1 && v.raw[0] == 255);
    s_rec->input_capture(0);
    pad_clear();
    poll0();
}

/* Guide+Y toggles flight without sinking: the chord's Y is masked before
 * the record, until Y is let go; Y alone sinks. */
static void test_record_chord(void)
{
    struct xbox_ctl_value v;
    int fly;

    start_with(ARMED, &k_fake, k_recs);
    poll0();
    fly = enhance_action_find("rec.fly");
    CHECK(fly >= 0);
    s_rec->input_capture(1);
    hold_extra(XBOX_HOST_GUIDE, 1);
    poll0();
    advance(16);
    hold_y(1);
    poll0();
    CHECK(enhance_action_pressed(fly) == 1);
    CHECK(rec_value(1, &v) == 1 && v.raw[0] == 0);
    hold_extra(XBOX_HOST_GUIDE, 0);
    advance(16);
    poll0();
    CHECK(rec_value(1, &v) == 1 && v.raw[0] == 0);
    hold_y(0);
    advance(16);
    poll0();
    advance(16);
    hold_y(1);
    poll0();
    CHECK(rec_value(1, &v) == 1 && v.raw[0] == 255);
    CHECK(enhance_action_pressed(fly) == 0);
    s_rec->input_capture(0);
    pad_clear();
    poll0();
}

/* A mouse button holds a fly button like a key (overlay-revamp D12);
 * the wheel, which only steps, is left out with a line. */
static void test_record_mouse(void)
{
    struct xbox_ctl_value v;
    struct enhance_ctl_src a, b;
    xbox_mouse_opts o;
    const char *out = start_with("[input.controls]\n\"rec.sink\" = [\"mouse2\", \"wheelup\"]\n",
                                 &k_fake, k_recs);

    CHECK(strstr(out, "\"wheelup\" is not a game button, a host key or a mouse button; left out")
          != NULL);
    CHECK(enhance_control_parse("mouse2", &a) && a.type == ENHANCE_CTL_MOUSE
          && a.idx == XBOX_MOUSE_RIGHT);
    CHECK(!enhance_control_parse("wheeldown", &b) && b.type == ENHANCE_CTL_NONE);
    CHECK(enhance_control_parse("mouse2", &b) && enhance_control_same(&a, &b));
    CHECK(enhance_control_parse("mouse3", &b) && !enhance_control_same(&a, &b));

    xbox_InputMouseDefaultOpts(&o);
    o.capture = XBOX_MOUSE_CAPTURE_NEVER;
    xbox_InputMouseOpts(&o);
    xbox_InputMouseEnable(1);
    s_rec->input_capture(1);
    poll0();
    CHECK(rec_value(1, &v) == 1 && v.raw[0] == 0);
    xbox_InputMouseButton(XBOX_MOUSE_RIGHT, 1);
    CHECK(rec_value(1, &v) == 1 && v.raw[0] == 255);
    xbox_InputMouseButton(XBOX_MOUSE_RIGHT, 0);
    CHECK(rec_value(1, &v) == 1 && v.raw[0] == 0);
    s_rec->input_capture(0);
    xbox_InputMouseEnable(0);
    xbox_InputMouseDefaultOpts(&o);
    xbox_InputMouseOpts(&o);
    poll0();
}

/* The menu's capture, host source to guest name, under a remap. */
static void test_record_from_host(void)
{
    xbox_pad_setup ps;
    char name[ENHANCE_ACTION_TEXT], why[96];

    start_with(ARMED, &k_fake, k_recs);
    CHECK(enhance_control_from_host(INPUT_SRC_WEST, 0, name, sizeof name, why, sizeof why)
          && !strcmp(name, "x"));
    CHECK(enhance_control_from_host(INPUT_SRC_RIGHT_TRIGGER, 0, name, sizeof name, why,
                                    sizeof why) && !strcmp(name, "rt"));
    CHECK(!enhance_control_from_host(INPUT_SRC_PADDLE1, 0, name, sizeof name, why, sizeof why)
          && strstr(why, "buttons the game sees"));
    CHECK(enhance_control_from_host(INPUT_SRC_RSTICK_UP, 1, name, sizeof name, why, sizeof why)
          && !strcmp(name, "right_stick"));
    CHECK(!enhance_control_from_host(INPUT_SRC_LSTICK_UP, 0, name, sizeof name, why, sizeof why)
          && strstr(why, "stick row"));
    CHECK(!enhance_control_from_host(INPUT_SRC_WEST, 1, name, sizeof name, why, sizeof why)
          && strstr(why, "takes a stick"));
    xbox_PadSetupIdentity(&ps);
    ps.map.source[INPUT_X] = INPUT_SRC_PADDLE1;
    xbox_InputSetPadSetup(&ps);
    CHECK(enhance_control_from_host(INPUT_SRC_PADDLE1, 0, name, sizeof name, why, sizeof why)
          && !strcmp(name, "x"));
    xbox_InputSetPadSetup(NULL);
}

/* D3's conflict rules, each way. */
static void test_record_conflicts(void)
{
    char why[128];
    int other;

    start_with(ARMED "[input.actions]\nmenu.open = [\"F10\", \"F8\"]\n", &k_fake, k_recs);
    /* 1: another control of the mod has it. */
    CHECK(enhance_control_conflict(0, 1, "rt", &other, why, sizeof why) && other == 2);
    CHECK(enhance_control_conflict(0, 1, "R", &other, why, sizeof why) && other == 2
          && strstr(why, "Rec: Rise"));
    /* 2: an action's single key, a captured key, either way. */
    CHECK(enhance_control_conflict(0, 1, "F10", &other, why, sizeof why) && other < 0
          && strstr(why, "F10 is"));
    CHECK(enhance_action_control_clash("R", why, sizeof why) && strstr(why, "Rec: Rise"));
    CHECK(!enhance_action_control_clash("Ctrl+R", why, sizeof why));
    /* 3: a button in an action's chord (menu.open's guide+a) is fine. */
    CHECK(!enhance_control_conflict(0, 2, "a", &other, why, sizeof why));
    /* Every binding of an action counts, not only its first. */
    CHECK(enhance_control_conflict(0, 2, "F8", &other, why, sizeof why) && strstr(why, "F8 is"));
}

#ifndef _WIN32
static int s_rec_stop;
static void *rec_poll_thread(void *u)
{
    (void)u;
    for (int16_t i = 1; !__atomic_load_n(&s_rec_stop, __ATOMIC_ACQUIRE); i = (int16_t)(i % 30000 + 1)) {
        XBOX_INPUT_STATE st;
        f_pad[0].Gamepad.sThumbLX = i;
        f_pad[0].Gamepad.sThumbLY = i;
        xbox_InputGetState(0, &st);
    }
    return NULL;
}

/* No torn read: both axes come from one record. */
static void test_record_torn(void)
{
    pthread_t t;
    struct xbox_ctl_value v;
    int torn = 0, reads = 0;

    start_with(NULL, &k_fake, k_recs);
    s_rec->input_capture(1);
    __atomic_store_n(&s_rec_stop, 0, __ATOMIC_RELEASE);
    pthread_create(&t, NULL, rec_poll_thread, NULL);
    for (int i = 0; i < 200000; i++)
        if (rec_value(3, &v)) {
            reads++;
            torn += v.raw[0] != v.raw[1];
        }
    __atomic_store_n(&s_rec_stop, 1, __ATOMIC_RELEASE);
    pthread_join(t, NULL);
    CHECK(reads > 0 && torn == 0);
    s_rec->input_capture(0);
    pad_clear();
    poll0();
}
#endif

int main(void)
{
    MKDIR("config");
    set_env("RECOMP_CONFIG", NULL);
    set_env("RECOMP_INPUT_CHORD_DELAY_MS", NULL);
    test_hold_chord();
    test_guide_a();
    test_no_guide();
    test_mask_until_release();
    test_refusals();
    test_chord_delay();
    test_scripts();
    test_disarmed();
    test_paddle_and_log();
    test_paddle_default();
    test_stats_toggle();
    test_tick();
    test_tick_race();
    test_mod_api();
    test_seam();
    test_host_space();
    test_stick_mask();
#ifndef _WIN32
    test_live_rebuild();
#endif
    test_one_input();
    test_longest_match();
    test_longest_spent();
    test_longest_ports();
    test_tick_rules();
    test_quiet();
    test_dormant();
#ifndef _WIN32
    test_live_arm();
#endif
    test_record();
    test_record_chord();
    test_record_mouse();
    test_record_from_host();
    test_record_conflicts();
#ifndef _WIN32
    test_record_torn();
#endif
    xbox_mod_shutdown();
    enhance_actions_reset();
    remove("config/config.toml");
    remove("actions_stderr.txt");
    printf("input_actions: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
