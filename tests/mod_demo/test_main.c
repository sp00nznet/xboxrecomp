/*
 * mod_demo test: the template mod (templates/mod/demo_mod.c) as a title runs
 * it through xbox_enhance_init, so the how-to's code stays code that builds
 * and works against this toolkit.
 *
 * Stock: mods=none, no [MOD demo] line, no panel. On from the user file:
 * listed, the panel on the first frame with the control's token, the count
 * refreshed at its interval. Dormant with the menu on, then the Mods page's
 * toggle: nothing until the next frame, then the panel, and off drops it.
 * The setting: a value above its range clamped, on_setting at the next frame.
 * The control: its action and default binding, both in the descriptor dump,
 * and its key resetting the count through action_pressed. The hook, through a
 * slot declared as the generated code declares one: counted with its first
 * stack argument while on, not called while dormant or switched off.
 * null_main.c holds the build with no API, where the accessor returns NULL.
 *
 * Runs in its working directory, where it writes config/config.toml, and
 * writes config.default.toml beside the executable.
 */
#include "demo_mod.h"
#include "enhance.h"
#include "enhance_actions.h"
#include "enhance_cfg.h"
#include "enhance_input.h"
#include "enhance_menu.h"
#include "input_backend.h"
#include "input_names.h"
#include "keyboard.h"
#include "mod.h"
#include "nv2a_backend_common.h"
#include "nv2a_overlay.h"
#include "nv2a_pb_state.h"
#include "recomp_env.h"
#include "recomp_exe_dir.h"
#include "recomp_hook.h"
#include "xbox_memory_layout.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
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
#define HAS(out, line) CHECK(strstr((out), (line)) != NULL)
#define LACKS(out, line) CHECK(strstr((out), (line)) == NULL)

/* ---- stubs for what the runtime would link (as tests/mod_hook) -------- */

void xbox_PacingSetMode(int mode);
void xbox_PacingSetMode(int mode) { (void)mode; }
void xbox_RamCacheEnable(int on);
void xbox_RamCacheEnable(int on) { (void)on; }
uint32_t xbox_SetLanguage(const char *code);
uint32_t xbox_SetLanguage(const char *code) { (void)code; return 1; }
int xbox_path_shader_cache(char *out, size_t n);
int xbox_path_shader_cache(char *out, size_t n) { (void)out; (void)n; return 0; }
int nv2a_overlay_arm(uint32_t layers, uint32_t out_h)
{
    (void)layers;
    (void)out_h;
    return -1;
}
void nv2a_overlay_set_stats(int on) { (void)on; }
float nv2a_overlay_set_ui_scale(float mul) { return mul; }
void enhance_display_start(void) {}
int nv2a_overlay_stats(void) { return 0; }
void nv2a_overlay_toast(const char *fmt, ...) { (void)fmt; }
void enhance_menu_start(void);
void enhance_menu_start(void) {}
struct enhance_item;
const struct enhance_item *enhance_menu_toolkit_item(const char *key);
const struct enhance_item *enhance_menu_toolkit_item(const char *key) { (void)key; return 0; }

/* Guest 0x1000..0x10FF: the hooked call's stack. */
static uint8_t s_guest[256];
ptrdiff_t xbox_GetMemoryOffset(void);
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)((uintptr_t)s_guest - 0x1000u); }
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

static uint64_t f_clock(void) { return 1000; }
static DWORD f_get_raw(DWORD p, InputRaw *raw, DWORD *packet, int extras)
{
    (void)p; (void)raw; (void)packet; (void)extras;
    return ERROR_DEVICE_NOT_CONNECTED;
}
static DWORD f_set(DWORD p, const XBOX_VIBRATION *v) { (void)p; (void)v; return ERROR_SUCCESS; }
static const xbox_input_backend k_fake = { NULL, f_get_raw, f_set, xbox_InputKbmPoll, f_clock, NULL, NULL, NULL };
const xbox_input_backend xbox_input_host_backend = { NULL, f_get_raw, f_set, NULL, f_clock, NULL, NULL, NULL };


/* ---- files and stderr -------------------------------------------------- */

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

static const char *defaults_file(void)
{
    static char p[4096];
    snprintf(p, sizeof p, "%s/config.default.toml", recomp_exe_dir());
    return p;
}

static int s_saved_fd = -1;
static char s_cap[65536];

static void capture_begin(void)
{
    int fd;
    fflush(stderr);
    s_saved_fd = DUP(2);
    fd = OPEN_TRUNC("mod_demo_stderr.txt");
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
    f = fopen("mod_demo_stderr.txt", "rb");
    if (f) {
        n = fread(s_cap, 1, sizeof s_cap - 1, f);
        fclose(f);
    }
    s_cap[n] = 0;
    return s_cap;
}

static const struct xbox_mod *s_table[2];

/* A fresh title start with this user file, as a title's main.c does it. */
static const char *start(const char *user)
{
    xbox_mod_shutdown();
    xbox_InputSetBackend(&k_fake);
    if (user)
        write_file("config/config.toml", user);
    else
        remove("config/config.toml");
    s_table[0] = demo_mod();
    s_table[1] = NULL;
    xbox_mod_register_table(s_table);
    capture_begin();
    xbox_enhance_init(".", NULL);
    return capture_end();
}

static const char *panel(void)
{
    static char t[XBOX_MOD_STATUS_LINE * XBOX_MOD_STATUS_LINES];
    xbox_mod_status_text(0, t, sizeof t);
    return t;
}

static void frames(int n)
{
    capture_begin();
    while (n-- > 0)
        xbox_mod_frame();
    capture_end();
}

/* ---- the hook slot, as the generated code declares one ----------------- */

#define VA_HOOKED 0x00010000u       /* DEMO_MOD_HOOK_VA in CMakeLists.txt */

/* The guest registers, as xbox_memory_layout.c defines them. */
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top;

static struct recomp_hook_slot s_slots[] = {
    { VA_HOOKED, 0, 0, 0, NULL },
};
RECOMP_HOOK_SLOTS(s_slots, 1)

static int s_body_runs;
static void hooked_body(void)
{
    s_body_runs++;
    g_eax = 1;
    g_esp += 4;     /* its ret */
}
static void sub_hooked(void) { RECOMP_HOOK_CALL(&s_slots[0], hooked_body); }

/* One guest call of sub_hooked with one stack argument. */
static void call_hooked(uint32_t arg)
{
    g_esp = 0x1080u;
    *(uint32_t *)(s_guest + 0x84) = arg;
    *(uint32_t *)(s_guest + 0x80) = 0x2000u;    /* the return address */
    sub_hooked();
}

/* ---- the cases --------------------------------------------------------- */

static void test_stock(void)
{
    const char *out = start(NULL);

    HAS(out, "[ENHANCE] mods=none");
    LACKS(out, "[MOD demo]");
    CHECK(!enhance_actions_armed());
    frames(3);
    CHECK(panel()[0] == 0);
    call_hooked(1);
    CHECK(s_body_runs == 1);        /* the body ran, and no hook counted it */
}

static void test_on(void)
{
    const char *out = start("[mods]\n\"demo.enable\" = true\n\"demo.interval\" = 2\n");

    HAS(out, "[MOD demo] on");
    HAS(out, "[ENHANCE] mods=demo");
    CHECK(xbox_mod_state(0) == XBOX_MOD_ON);
    CHECK(enhance_actions_armed());     /* with no menu: its key and panel work */
    frames(1);
    HAS(panel(), "Frames\n1\n");
    HAS(panel(), "{ctl:reset} resets");
    frames(1);                      /* frame 2: inside the interval */
    HAS(panel(), "Frames\n1\n");
    frames(1);                      /* frame 3: the interval is up */
    HAS(panel(), "Frames\n3\n");

    /* The hook, counted and reading its argument. */
    call_hooked(0x1234ABCDu);
    frames(2);
    HAS(panel(), "calls 1 arg 1234ABCD");
}

static void test_menu_toggle(void)
{
    const char *out = start("[menu]\nenabled = true\n[mods]\n\"demo.interval\" = 1\n");
    int calls = s_body_runs;

    /* Dormant: inited so the menu can turn it on, and doing nothing. */
    HAS(out, "[ENHANCE] mods=none");
    LACKS(out, "gave no on_enable");
    CHECK(xbox_mod_state(0) == XBOX_MOD_OFF);
    frames(2);
    CHECK(panel()[0] == 0);
    call_hooked(9);
    CHECK(s_body_runs == calls + 1);

    /* The toggle reaches the mod at the next frame. */
    xbox_mod_set_enabled(0, 1);
    CHECK(panel()[0] == 0);
    frames(2);
    HAS(panel(), "Frames\n");
    HAS(panel(), "calls 0 arg");    /* the dormant call above was not counted */
    call_hooked(7);
    frames(1);
    HAS(panel(), "calls 1 arg 00000007");

    /* Off: the toolkit drops the panel, and the mod draws none after. */
    xbox_mod_set_enabled(0, 0);
    frames(2);
    CHECK(panel()[0] == 0);
    call_hooked(9);
    frames(2);
    CHECK(panel()[0] == 0);

    /* And back: a new panel from a zero count, and the call made while off
     * was not counted. */
    xbox_mod_set_enabled(0, 1);
    frames(1);
    HAS(panel(), "Frames\n");
    HAS(panel(), "calls 1 arg 00000007");
}

static void test_setting(void)
{
    const char *out = start("[mods]\n\"demo.enable\" = true\n\"demo.interval\" = 5000\n");
    int i = xbox_mod_enable_setting(0);

    CHECK(i == 0);
    HAS(out, "[MOD] demo.interval: 5000 -> 600");
    CHECK(xbox_mod_setting_value(0, 1) == 600.0);
    frames(1);
    HAS(panel(), "Frames\n1\n");
    frames(5);
    HAS(panel(), "Frames\n1\n");    /* 600 frames apart now */

    /* A change from the menu: on_setting at the next frame. */
    CHECK(xbox_mod_setting_set(0, 1, 1.0) == 1.0);
    frames(2);
    HAS(panel(), "Frames\n8\n");
}

/* One title poll, which is where the actions layer reads the keys. */
static void poll0(void)
{
    XBOX_INPUT_STATE st;

    memset(&st, 0, sizeof st);
    xbox_InputGetState(0, &st);
}

/* The reset control's key, through action_pressed: the interval is too long
 * to redraw by itself, so a panel at 0 can only come from the reset. */
static void test_reset_key(void)
{
    start("[mods]\n\"demo.enable\" = true\n\"demo.interval\" = 600\n");
    poll0();                        /* a binding held at its build never fires */
    frames(3);
    HAS(panel(), "Frames\n1\n");
    xbox_InputKeyEvent(XBOX_VK_F1 + 7, 1);  /* F8 */
    poll0();
    xbox_InputKeyEvent(XBOX_VK_F1 + 7, 0);
    poll0();
    frames(1);
    HAS(panel(), "Frames\n0\n");
    CHECK(enhance_action_pressed(xbox_mod_control_action(0, 0)) == 0);  /* it fired once */
}

static void test_control_and_dump(void)
{
    char binds[4][ENHANCE_ACTION_TEXT];
    const char *src = NULL;
    FILE *f;
    char json[32768];
    size_t n;

    start("[mods]\n\"demo.enable\" = true\n");
    CHECK(xbox_mod_control_action(0, 0) >= 0);
    CHECK(xbox_mod_control_bindings(0, 0, binds, 4, &src) == 1);
    CHECK(!strcmp(binds[0], "F8"));
    CHECK(src && !strcmp(src, "default"));

    CHECK(xbox_mod_describe_write("mods.json") == 0);
    f = fopen("mods.json", "rb");
    CHECK(f != NULL);
    n = f ? fread(json, 1, sizeof json - 1, f) : 0;
    if (f)
        fclose(f);
    json[n] = 0;
    HAS(json, "\"demo\"");
    HAS(json, "demo.interval");
    HAS(json, "Reset the count");
    HAS(json, "F8");
}

int main(void)
{
    MKDIR("config");
    remove(defaults_file());
    test_stock();
    test_on();
    test_menu_toggle();
    test_setting();
    test_reset_key();
    test_control_and_dump();
    xbox_mod_shutdown();
    printf("mod_demo: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
