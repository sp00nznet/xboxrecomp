/*
 * enhance_menu test: the in-game menu (src/enhance/enhance_menu.c) as a
 * title runs it, through xbox_enhance_init and xbox_InputGetState against a
 * fake input backend with its own clock, drawn by the real overlay:
 *
 *   start     menu.enabled off logs off and arms nothing; on logs the
 *             menu.open bindings
 *   navigate  F10 opens, the pages wrap, the selection skips info rows,
 *             the title reads a neutral pad with a frozen packet number,
 *             Esc closes and the held key stays the host's to its release
 *   mods      a toggle through on_enable runs at the next xbox_mod_frame;
 *             a mod without on_enable is "applies at restart"; a mod's
 *             item on its sub-list
 *   capture   a key, a held pad chord, a refusal (a key the guest uses),
 *             Esc cancels, menu.open cannot be cleared
 *   save      config.toml gets only the changed keys: the binding as an
 *             array, an item, a value back at its default removed
 *   mouse     the Controls page's Mouse group and the capture choice
 *   snapshot  the pack's miss ring copied at the flip only while shown
 *   shaders   the Shaders page: the picker, the active preset's parameters
 *             live, Save as differences, the next start, Reset to preset
 *   profiles  the profile picker (broken and newer skipped), the chain
 *             rows, a stage's sub-list, Add, Move, Remove with its confirm,
 *             the fork of a bundled preset, Rename on keyboard and pad,
 *             Save as new, Duplicate, Delete, Save writing the files and
 *             present.profile, the list read when the page opens, Reset to
 *             defaults leaving the files
 *   shaders_hidden  no chain presenter, or no library: the page is left out
 *             of the tab strip and the paging, and why is said once
 *   pages     each page's draw list at 1920x1080 against a pinned sum
 *
 * Runs in its working directory, where it writes config/config.toml. A
 * checksum of 0 below means "not pinned yet": the run prints the value.
 */
#include "enhance.h"
#include "enhance_actions.h"
#include "enhance_cfg.h"
#include "enhance_menu.h"
#include "host_pad.h"
#include "input_backend.h"
#include "input_names.h"
#include "keyboard.h"
#include "mod.h"
#include "mouse.h"
#include "nv2a_backend_common.h"
#include "nv2a_overlay.h"
#include "nv2a_overlay_int.h"
#include "nv2a_pb_state.h"
#include "nv2a_present_chain.h"
#include "nv2a_tex_pack.h"
#include "pad_map.h"
#include "recomp_env.h"
#include "shader_profile_edit.h"

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
#define rmdir _rmdir
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
static int s_pin;   /* --pin: print the page sums instead of checking them */

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- stubs for what the runtime would link ---------------------------- */

static int s_pacing = 1;
int xbox_PacingMode(void);
int xbox_PacingMode(void) { return s_pacing; }
void xbox_PacingSetMode(int mode);
void xbox_PacingSetMode(int mode) { s_pacing = mode; }
/* kernel_ramfs.c's switch, which this test does not link. */
void xbox_RamCacheEnable(int on);
void xbox_RamCacheEnable(int on) { (void)on; }
/* kernel_path.c: the shader cache root a chain profile composes under
 * (shader_profile_req.c); NULL: none yet, as before the save root is up. */
static const char *s_cache_root;
int xbox_path_shader_cache(char *out, size_t n);
int xbox_path_shader_cache(char *out, size_t n)
{
    if (!s_cache_root)
        return 0;
    snprintf(out, n, "%s", s_cache_root);
    return 1;
}

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

/* ---- the fake pad backend --------------------------------------------- */

#define VK_F5  0x74
#define VK_F7  0x76
#define VK_F10 0x79

static uint64_t s_now = 1000;
static XBOX_INPUT_STATE f_pad[4];
static int f_conn[4] = { 1, 0, 0, 0 };
static uint32_t f_extra[4];

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
static DWORD f_read_raw(DWORD p, InputRaw *raw)
{
    if (p >= 4 || !f_conn[p])
        return ERROR_DEVICE_NOT_CONNECTED;
    xbox_PadRawFromGamepad(&f_pad[p].Gamepad, f_extra[p], raw);
    return ERROR_SUCCESS;
}

static const xbox_input_backend k_fake = {
    NULL, f_get_raw, f_set, xbox_InputKbmPoll, f_clock, f_read_raw, NULL, NULL };
const xbox_input_backend xbox_input_host_backend = {
    NULL, f_get_raw, f_set, NULL, f_clock, NULL, NULL, NULL };

static void pad_clear(void)
{
    memset(f_pad, 0, sizeof f_pad);
    memset(f_extra, 0, sizeof f_extra);
    f_conn[0] = 1;
}
static void hold_btn(WORD w, int on)
{
    if (on)
        f_pad[0].Gamepad.wButtons |= w;
    else
        f_pad[0].Gamepad.wButtons &= (WORD)~w;
}
static void hold_a(int on) { f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_A] = on ? 255 : 0; }

static XBOX_INPUT_STATE poll0(void)
{
    XBOX_INPUT_STATE st;
    memset(&st, 0, sizeof st);
    xbox_InputGetState(0, &st);
    return st;
}

/* The flip's part: the live options requested apply there. */
static void flip(void)
{
    nv2a_host_opts_apply();
    nv2a_tex_pack_snapshot();
}

/* A key press and release, each seen by a poll a little apart, a flip
 * after each. */
static void tap(int vk)
{
    xbox_InputKeyEvent(vk, 1);
    advance(16);
    poll0();
    flip();
    xbox_InputKeyEvent(vk, 0);
    advance(16);
    poll0();
    flip();
}

/* ---- stderr capture ---------------------------------------------------- */

static int s_saved_fd = -1;
static char s_cap[32768];

static void capture_begin(void)
{
    int fd;
    fflush(stderr);
    s_saved_fd = DUP(2);
    fd = OPEN_TRUNC("menu_stderr.txt");
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
    f = fopen("menu_stderr.txt", "rb");
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

static char s_file[8192];

static const char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t n = 0;
    s_file[0] = 0;
    if (!f)
        return s_file;
    n = fread(s_file, 1, sizeof s_file - 1, f);
    fclose(f);
    s_file[n] = 0;
    return s_file;
}

static const char *start_with(const char *toml, const struct xbox_mod *const *mods)
{
    const char *out;
    xbox_mod_shutdown();
    enhance_menu_reset_for_test();
    nv2a_present_chain_params_reset_for_test();
    nv2a_overlay_reset_for_test();
    xbox_InputSetBackend(&k_fake);
    xbox_InputKeysClear();
    pad_clear();
    if (toml)
        write_file("config/config.toml", toml);
    else
        remove("config/config.toml");
    xbox_mod_register_table(mods);
    capture_begin();
    xbox_enhance_init(".", NULL);
    out = capture_end();
    nv2a_overlay_set_placement(0, 0, 1920, 1080);
    poll0();
    return out;
}

#define ARMED "[menu]\nenabled = true\n"

/* The row with this label on the page shown: its value, or NULL. */
static char s_val[128];
static const char *row_value(const char *label)
{
    char l[96];
    int n = enhance_menu_rows();
    for (int i = 0; i < n; i++) {
        enhance_menu_row(i, l, sizeof l, s_val, sizeof s_val, NULL);
        if (!strcmp(l, label))
            return s_val;
    }
    return NULL;
}

static const char *selected_label(void)
{
    static char l[96];
    enhance_menu_row(enhance_menu_selected(), l, sizeof l, NULL, 0, NULL);
    return l;
}

static void open_menu(void)
{
    tap(VK_F10);
}

/* ---- a mod with on_enable, an action and an item; one without --------- */

static int s_enable_calls = -1, s_enable_last = -1, s_enable_thread_frame;
static int s_in_frame;
static void tm_enable(int on, void *u)
{
    (void)u;
    s_enable_calls++;
    s_enable_last = on;
    s_enable_thread_frame = s_in_frame;
}
static void tm_frame(void *u) { (void)u; }
static const char *const k_levels[] = { "low", "high", NULL };
static const struct enhance_item k_tm_level = {
    .key = "testmod.level", .label = "Level", .kind = ENHANCE_ITEM_CHOICE,
    .choices = k_levels, .def = "low" };
static int tm_init(const struct xbox_mod_api *api)
{
    static const char *const defs[] = { "F6", NULL };
    s_enable_calls = 0;
    api->on_frame(tm_frame, NULL);
    api->action_register("testmod.poke", "Poke", defs);
    api->menu_add(&k_tm_level);
    return api->on_enable(tm_enable, NULL) ? 0 : -1;
}
static const struct xbox_mod k_tm = { "testmod", tm_init, NULL, NULL };

static int pm_init(const struct xbox_mod_api *api) { (void)api; return 0; }
static const struct xbox_mod k_pm = { "plainmod", pm_init, NULL, NULL };

static const struct xbox_mod *const k_mods[] = { &k_tm, &k_pm, NULL };

/* ---- 1. start ------------------------------------------------------------- */

static void test_start(void)
{
    const char *out = start_with(NULL, NULL);
    CHECK(strstr(out, "[ENHANCE] menu.enabled=off\n") != NULL);
    open_menu();
    CHECK(!enhance_menu_is_open());

    out = start_with(ARMED, NULL);
    CHECK(strstr(out, "[ENHANCE] menu.enabled=on (F10, guide+a, paddle1)\n") != NULL);
    set_env("RECOMP_MENU", "0");
    out = start_with(ARMED, NULL);
    CHECK(strstr(out, "[ENHANCE] menu.enabled=off\n") != NULL);
    set_env("RECOMP_MENU", NULL);
}

/* ---- 2. navigation and the neutral pad ---------------------------------- */

static void test_navigate(void)
{
    XBOX_INPUT_STATE st, st2;
    start_with(ARMED, k_mods);
    f_pad[0].dwPacketNumber = 41;
    st = poll0();
    open_menu();
    CHECK(enhance_menu_is_open());
    CHECK(enhance_menu_page() == 0);
    CHECK(!strcmp(enhance_menu_page_name(enhance_menu_page()), "Render"));
    CHECK(enhance_menu_page_count() == 9);

    /* The title: no buttons, one packet number while the pad changes. A
     * trigger: the menu reads none, so the press does nothing there. */
    f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = 255;
    f_pad[0].dwPacketNumber = 50;
    advance(16);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 0);
    f_pad[0].dwPacketNumber = 51;
    advance(16);
    st2 = poll0();
    CHECK(st2.dwPacketNumber == st.dwPacketNumber);
    CHECK(st.dwPacketNumber != 41 && st.dwPacketNumber != 42 && st.dwPacketNumber != 50);
    f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = 0;
    advance(16);
    poll0();

    /* Q from Render wraps to Save, E back. */
    tap('Q');
    CHECK(!strcmp(enhance_menu_page_name(enhance_menu_page()), "Files"));
    tap('E');
    CHECK(!strcmp(enhance_menu_page_name(enhance_menu_page()), "Render"));
    CHECK(!strcmp(selected_label(), "Render scale"));
    xbox_InputKeyEvent(0x28, 1);    /* Down */
    advance(16);
    poll0();
    CHECK(!strcmp(selected_label(), "2D on guest grid"));
    /* Held: the first repeat after 150 ms, then every 60. */
    advance(100);
    poll0();
    CHECK(!strcmp(selected_label(), "2D on guest grid"));
    advance(60);
    poll0();
    CHECK(!strcmp(selected_label(), "Anisotropic filtering"));
    advance(60);
    poll0();
    CHECK(!strcmp(selected_label(), "Scaling filter"));
    advance(60);
    poll0();
    CHECK(!strcmp(selected_label(), "Fullscreen"));
    xbox_InputKeyEvent(0x28, 0);
    advance(16);
    poll0();
    /* Up from the first row wraps to the last. */
    CHECK(enhance_menu_select_row("Render scale"));
    tap(0x26);
    CHECK(!strcmp(selected_label(), "Present surface"));

    /* Right on Render scale: 1 -> 2, at the flip; unsaved. */
    CHECK(enhance_menu_select_row("Render scale"));
    tap(0x27);
    CHECK(!strcmp(row_value("Render scale"), "2"));
    CHECK(enhance_menu_unsaved() == 1);
    tap(0x27);
    tap(0x27);
    tap(0x27);
    CHECK(!strcmp(row_value("Render scale"), "4"));     /* clamped */
    tap(0x0D);                                          /* OK wraps */
    CHECK(!strcmp(row_value("Render scale"), "1"));
    CHECK(enhance_menu_unsaved() == 0);

    /* A choice wraps both ways. */
    CHECK(enhance_menu_select_row("Scaling filter"));
    tap(0x25);
    CHECK(!strcmp(row_value("Scaling filter"), "integer"));
    tap(0x27);
    CHECK(!strcmp(row_value("Scaling filter"), "nearest"));

    /* Esc asks first (D9a): the A held here stepped the filter, so the
     * dialog comes up and N leaves without saving. The title gets nothing
     * while the keys are down, and the pad button held through the close
     * stays the host's to its release. */
    hold_a(1);
    advance(16);
    poll0();
    xbox_InputKeyEvent(0x1B, 1);
    advance(16);
    st = poll0();
    CHECK(enhance_menu_is_open());
    CHECK(enhance_menu_dialog() == 2);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    xbox_InputKeyEvent(0x1B, 0);
    advance(160);
    poll0();
    xbox_InputKeyEvent('N', 1);
    advance(16);
    st = poll0();
    CHECK(!enhance_menu_is_open());
    CHECK(!strcmp(row_value("Scaling filter"), "nearest"));
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    xbox_InputKeyEvent('N', 0);
    advance(16);
    st = poll0();
    advance(1100);
    st = poll0();
    CHECK(!enhance_menu_is_open());
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    advance(16);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    hold_a(0);
    advance(16);
    poll0();
    hold_a(1);
    advance(16);
    st = poll0();
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    hold_a(0);
    advance(16);
    poll0();

    /* F10 again opens, and F10 while open closes. */
    open_menu();
    CHECK(enhance_menu_is_open());
    open_menu();
    CHECK(!enhance_menu_is_open());
}

/* ---- 3. Mods ------------------------------------------------------------- */

static void test_mods(void)
{
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Mods"));
    CHECK(row_value("testmod") && !strcmp(row_value("testmod"), "true"));
    CHECK(row_value("plainmod") && !strcmp(row_value("plainmod"), "true"));
    CHECK(xbox_mod_has_on_enable(0) && !xbox_mod_has_on_enable(1));

    /* Left toggles off: recorded now, on_enable at the next frame. */
    CHECK(enhance_menu_select_row("testmod"));
    tap(0x25);
    CHECK(!xbox_mod_enabled(0));
    CHECK(s_enable_calls == 0);
    s_in_frame = 1;
    xbox_mod_frame();
    s_in_frame = 0;
    CHECK(s_enable_calls == 1 && s_enable_last == 0 && s_enable_thread_frame);
    CHECK(!strcmp(row_value("testmod"), "false"));
    CHECK(enhance_menu_unsaved() == 1);

    /* Enter lists the mod's own items. */
    tap(0x0D);
    CHECK(row_value("Enabled") && !strcmp(row_value("Enabled"), "false"));
    CHECK(row_value("Level") && !strcmp(row_value("Level"), "low"));
    CHECK(enhance_menu_select_row("Enabled"));
    tap(0x0D);
    xbox_mod_frame();
    CHECK(s_enable_calls == 2 && s_enable_last == 1);
    CHECK(enhance_menu_select_row("Level"));
    tap(0x27);
    CHECK(!strcmp(row_value("Level"), "high"));
    /* Back leaves the sub-list, not the menu. */
    tap(0x08);
    CHECK(enhance_menu_is_open());
    CHECK(row_value("plainmod") != NULL);

    /* A mod without on_enable: the toggle is for the next start. */
    CHECK(enhance_menu_select_row("plainmod"));
    tap(0x27);
    CHECK(xbox_mod_state(1) == XBOX_MOD_RESTART);
    CHECK(strstr(enhance_menu_status(), "applies at restart") != NULL);
    open_menu();
}

/* A descriptor mod's toggle is its enable setting (mod-settings D4): Save
 * writes that key with the choice text, never mods."<id>", and turning it
 * on drops a stale mods."<id>" = false that would hold it off again. */
static const char *const k_dm_offon[] = { "off", "on", NULL };
static const struct xbox_mod_setting k_dm_set[] = {
    { "enabled", "dmod.enabled", NULL, NULL, "Descriptor mod", NULL, NULL,
      XBOX_SET_ENUM, ENHANCE_LIVE, XBOX_SET_ENABLE, 0, 0, 0, 0, k_dm_offon },
};
static const struct xbox_mod_desc k_dm_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc),
    "Descriptor mod", NULL, NULL, NULL, k_dm_set, 1, NULL, 0
};
static void dm_enable(int on, void *u) { (void)u; (void)on; }
static int dm_init(const struct xbox_mod_api *api)
{
    return api->on_enable(dm_enable, NULL) ? 0 : -1;
}
static const struct xbox_mod k_dm = { "dmod", dm_init, NULL, &k_dm_desc };
static const struct xbox_mod *const k_dmods[] = { &k_dm, NULL };

static int pending_has(const char *key, const char *val)
{
    const char *keys[16], *vals[16];
    int n = enhance_menu_pending(keys, vals, 16);
    for (int i = 0; i < n; i++)
        if (!strcmp(keys[i], key))
            return val ? vals[i] && !strcmp(vals[i], val) : vals[i] == NULL;
    return 0;
}

static int pending_any(const char *key)
{
    const char *keys[16], *vals[16];
    int n = enhance_menu_pending(keys, vals, 16);
    for (int i = 0; i < n; i++)
        if (!strcmp(keys[i], key))
            return 1;
    return 0;
}

static void test_mods_desc_toggle(void)
{
    const char *f;
    start_with(ARMED, k_dmods);
    open_menu();
    CHECK(enhance_menu_set_page("Mods"));
    CHECK(row_value("Descriptor mod") && !strcmp(row_value("Descriptor mod"), "false"));
    CHECK(enhance_menu_select_row("Descriptor mod"));
    tap(0x27);
    xbox_mod_frame();
    CHECK(xbox_mod_enabled(0));
    CHECK(pending_has("dmod.enabled", "\"on\""));
    CHECK(!pending_any("mods.dmod"));
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "[dmod]\nenabled = \"on\"") != NULL);
    CHECK(strstr(f, "dmod = ") == NULL);
    open_menu();

    /* Back off is the stock: the key goes, not a mods.dmod = false. */
    start_with(ARMED "[dmod]\nenabled = \"on\"\n", k_dmods);
    open_menu();
    CHECK(enhance_menu_set_page("Mods"));
    CHECK(!strcmp(row_value("Descriptor mod"), "true"));
    CHECK(enhance_menu_select_row("Descriptor mod"));
    tap(0x25);
    xbox_mod_frame();
    CHECK(pending_has("dmod.enabled", NULL));
    CHECK(!pending_any("mods.dmod"));
    open_menu();

    /* A stale switch off in the file goes when the toggle turns on. */
    start_with(ARMED "[mods]\ndmod = false\n", k_dmods);
    open_menu();
    CHECK(enhance_menu_set_page("Mods"));
    CHECK(enhance_menu_select_row("Descriptor mod"));
    tap(0x27);
    xbox_mod_frame();
    CHECK(pending_has("dmod.enabled", "\"on\""));
    CHECK(pending_has("mods.dmod", NULL));
    open_menu();
}

/* A chord already bound in another order or case is the same chord (N5):
 * capturing it again adds nothing. */
static void test_capture_duplicate(void)
{
    start_with(ARMED "[input.actions]\n\"testmod.poke\" = [\"Left_Stick_Click+BACK@400\"]\n", k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Controls"));
    CHECK(enhance_menu_select_row("Poke"));
    tap(0x0D);
    CHECK(enhance_menu_capturing());
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB, 1);
    advance(16);
    poll0();
    advance(450);
    poll0();
    CHECK(!enhance_menu_capturing());
    CHECK(strstr(enhance_menu_status(), "is bound already") != NULL);
    /* The table parses pad names in lowercase only, so the row says this
     * one never fires (mod-settings B.4). */
    CHECK(!strcmp(row_value("Poke"), "Left_Stick_Click+BACK@400 (refused)"));
    CHECK(enhance_menu_unsaved() == 0);
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB, 0);
    advance(16);
    poll0();
    open_menu();
}

/* A key another action owns is refused, naming the owner: the
 * Deck grip Steam Input sends as F10 must not land on the stats strip
 * while F10 still opens the menu, or one press fires both. The capture
 * itself fires nothing, so the menu stays open through it. */
static void test_capture_owned(void)
{
    char before[128];
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Controls"));
    CHECK(enhance_menu_select_row("Stats strip"));
    CHECK(row_value("Stats strip") != NULL);
    snprintf(before, sizeof before, "%s", row_value("Stats strip") ? s_val : "");
    tap(0x0D);
    CHECK(enhance_menu_capturing());
    tap(VK_F10);
    CHECK(!enhance_menu_capturing());
    CHECK(enhance_menu_is_open());
    CHECK(strstr(enhance_menu_status(), "F10 refused: Open the menu has it (F10); clear it there first") != NULL);
    CHECK(row_value("Stats strip") && !strcmp(s_val, before));
    CHECK(enhance_menu_unsaved() == 0);
    open_menu();
    CHECK(!enhance_menu_is_open());
}

/* ---- 4. Controls: capture, refusal, clear; 5. Save ----------------------- */

static void test_capture_and_save(void)
{
    const char *f;
    start_with(ARMED "[input]\nwalk_key = \"F8\"\n", k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Controls"));
    CHECK(row_value("Poke") && !strcmp(row_value("Poke"), "F6"));

    /* A key: the Enter that started it goes first, then F7. */
    CHECK(enhance_menu_select_row("Poke"));
    xbox_InputKeyEvent(0x0D, 1);
    advance(16);
    poll0();
    CHECK(enhance_menu_capturing());
    advance(16);
    poll0();
    CHECK(enhance_menu_capturing());
    xbox_InputKeyEvent(0x0D, 0);
    advance(16);
    poll0();
    tap(VK_F7);
    CHECK(!enhance_menu_capturing());
    CHECK(!strcmp(row_value("Poke"), "F6, F7"));
    CHECK(enhance_menu_unsaved() == 1);
    /* Live at once: F7 pokes while the menu is shut. */

    /* A pad chord held 400 ms. */
    tap(0x0D);
    CHECK(enhance_menu_capturing());
    hold_btn(XBOX_GAMEPAD_BACK, 1);
    advance(16);
    poll0();
    hold_btn(XBOX_GAMEPAD_LEFT_THUMB, 1);
    advance(16);
    poll0();
    advance(200);
    poll0();
    CHECK(enhance_menu_capturing());
    advance(250);
    poll0();
    CHECK(!enhance_menu_capturing());
    CHECK(!strcmp(row_value("Poke"), "F6, F7, back+left_stick_click@400"));
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB, 0);
    advance(16);
    poll0();

    /* Refused: the walk key. */
    tap(0x0D);
    tap(0x77);  /* F8 */
    CHECK(!enhance_menu_capturing());
    CHECK(strstr(enhance_menu_status(), "F8 refused") != NULL);
    CHECK(!strcmp(row_value("Poke"), "F6, F7, back+left_stick_click@400"));
    /* Esc cancels. */
    tap(0x0D);
    tap(0x1B);
    CHECK(!enhance_menu_capturing());
    CHECK(enhance_menu_is_open());
    CHECK(strstr(enhance_menu_status(), "cancelled") != NULL);
    /* menu.open keeps a binding. */
    CHECK(enhance_menu_select_row("Open the menu"));
    tap(0x2E);
    CHECK(strstr(enhance_menu_status(), "keeps a binding") != NULL);
    CHECK(!strcmp(row_value("Open the menu"), "F10, guide+a, paddle1"));

    /* An item, then one moved and moved back: neither that nor the
     * untouched keys reach the file. */
    CHECK(enhance_menu_set_page("Render"));
    CHECK(enhance_menu_select_row("Scaling filter"));
    tap(0x27);
    CHECK(enhance_menu_select_row("Render scale"));
    tap(0x27);
    tap(0x25);
    {
        const char *keys[16], *vals[16];
        int n = enhance_menu_pending(keys, vals, 16);
        CHECK(n == 2);
        CHECK(n >= 1 && !strcmp(keys[0], "present.filter") && vals[0]
              && !strcmp(vals[0], "\"linear\""));
        CHECK(n >= 2 && !strcmp(keys[1], "input.actions.testmod.poke") && vals[1]
              && !strcmp(vals[1], "[\"F6\", \"F7\", \"back+left_stick_click@400\"]"));
    }
    tap(VK_F5);
    CHECK(strstr(enhance_menu_status(), "Saved 2 changes") != NULL);
    CHECK(enhance_menu_unsaved() == 0);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "filter = \"linear\"") != NULL);
    CHECK(strstr(f, "\"testmod.poke\" = [\"F6\", \"F7\", \"back+left_stick_click@400\"]") != NULL);
    CHECK(strstr(f, "scale") == NULL);
    CHECK(strstr(f, "walk_key = \"F8\"") != NULL);
    tap(VK_F5);
    CHECK(strstr(enhance_menu_status(), "Nothing to save") != NULL);
    open_menu();

    /* The next start reads them back. */
    start_with(read_file("config/config.toml"), k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Controls"));
    CHECK(row_value("Poke") && !strcmp(row_value("Poke"), "F6, F7, back+left_stick_click@400"));
    CHECK(enhance_menu_set_page("Render"));
    CHECK(row_value("Scaling filter") && !strcmp(row_value("Scaling filter"), "linear"));

    /* Back to the default: the key leaves the file. */
    CHECK(enhance_menu_select_row("Scaling filter"));
    tap(0x25);
    CHECK(!strcmp(row_value("Scaling filter"), "nearest"));
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "filter") == NULL);
    CHECK(strstr(f, "testmod.poke") != NULL);

    /* Reset asks first, then puts the binding back to its defaults. */
    CHECK(enhance_menu_set_page("Files"));
    CHECK(enhance_menu_select_row("Reset to defaults"));
    tap(0x0D);
    CHECK(enhance_menu_select_row("Reset everything? A / Enter: yes"));
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "Reset to defaults") != NULL);
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "testmod.poke") == NULL);
    CHECK(strstr(f, "walk_key = \"F8\"") != NULL);
    open_menu();

    /* RECOMP_CONFIG=none: Save is off. */
    set_env("RECOMP_CONFIG", "none");
    set_env("RECOMP_MENU", "1");
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_select_row("Scaling filter"));
    tap(0x27);
    tap(VK_F5);
    CHECK(strstr(enhance_menu_status(), "Save is off") != NULL);
    open_menu();
    set_env("RECOMP_MENU", NULL);
    set_env("RECOMP_CONFIG", NULL);
}

/* ---- 6. the Mouse group ---------------------------------------------------- */

static void test_mouse(void)
{
    char l[96], v[128];
    int n, mouse = -1;
    start_with(ARMED, NULL);
    open_menu();
    CHECK(enhance_menu_set_page("Controls"));
    n = enhance_menu_rows();
    for (int i = 0; i < n; i++) {
        int sel;
        enhance_menu_row(i, l, sizeof l, v, sizeof v, &sel);
        if (!strcmp(l, "Mouse")) {
            mouse = i;
            CHECK(!sel);
        }
    }
    CHECK(mouse > 0);
    /* The group is last: the capture choice, the release key, the free
     * keys and the state. */
    enhance_menu_row(mouse + 1, l, sizeof l, v, sizeof v, NULL);
    CHECK(!strcmp(l, "Capture") && !strcmp(v, "focus"));
    enhance_menu_row(mouse + 2, l, sizeof l, v, sizeof v, NULL);
    CHECK(!strcmp(l, "Release key") && !strcmp(v, "F9"));
    enhance_menu_row(mouse + 3, l, sizeof l, v, sizeof v, NULL);
    CHECK(!strcmp(l, "Free keys"));
    enhance_menu_row(mouse + 4, l, sizeof l, v, sizeof v, NULL);
    CHECK(!strcmp(l, "State") && !strcmp(v, "free: F9"));
    CHECK(mouse + 5 == n);

    /* The choice: focus -> never -> click, applied through the options. */
    CHECK(enhance_menu_select_row("Capture"));
    tap(0x27);
    CHECK(!strcmp(row_value("Capture"), "never"));
    tap(0x27);
    CHECK(!strcmp(row_value("Capture"), "click"));
    {
        xbox_mouse_opts o;
        xbox_InputMouseGetOpts(&o);
        CHECK(o.capture == XBOX_MOUSE_CAPTURE_CLICK);
    }
    CHECK(!strcmp(row_value("State"), "free: click to capture"));
    {
        const char *keys[4], *vals[4];
        CHECK(enhance_menu_pending(keys, vals, 4) == 1);
        CHECK(!strcmp(keys[0], "input.mouse_capture") && !strcmp(vals[0], "\"click\""));
    }
    open_menu();
}

/* ---- 7. the miss snapshot ------------------------------------------------- */

static void test_snapshot(void)
{
    struct nv2a_tex_pack_miss m[4];
    start_with(ARMED, NULL);
    nv2a_tex_pack_miss(0x1111, 64, 64, 0x0C, 1);
    /* Nobody shows it: the flip takes no copy. */
    nv2a_tex_pack_snapshot();
    CHECK(nv2a_tex_pack_misses_snapshot(m, 4) == 0);
    open_menu();
    CHECK(enhance_menu_set_page("Pack"));
    nv2a_tex_pack_miss(0x2222, 128, 32, 0x12, 1);
    nv2a_tex_pack_miss(0x2222, 128, 32, 0x12, 1);
    nv2a_tex_pack_snapshot();
    CHECK(nv2a_tex_pack_misses_snapshot(m, 4) == 2);
    CHECK(m[0].hash == 0x2222 && m[0].count == 2 && m[1].hash == 0x1111);
    CHECK(row_value("No pack loaded") != NULL);
    CHECK(row_value("Misses") && !strcmp(row_value("Misses"), "3"));
    CHECK(row_value("0000000000002222") != NULL
          && strstr(row_value("0000000000002222"), "128x32") != NULL);
    open_menu();
    /* Closed: later misses are not copied. */
    nv2a_tex_pack_miss(0x3333, 8, 8, 0x0C, 1);
    nv2a_tex_pack_snapshot();
    CHECK(nv2a_tex_pack_misses_snapshot(m, 4) == 2);
}

/* ---- 8. each page's draw list -------------------------------------------- */

/* ---- 3b. a descriptor mod's page (mod-settings D5) ----------------------- */

static const char *const k_bm_hud[] = { "keep", "hide", NULL };
static const char *const k_bm_speed_alias[] = { "bmod_old_speed", NULL };
static const struct xbox_mod_setting k_bm_set[] = {
    { "enabled", "bmod.enabled", NULL, NULL, "Enabled", NULL, NULL,
      XBOX_SET_BOOL, ENHANCE_LIVE, XBOX_SET_ENABLE, 1, 0, 0, 0, NULL },
    { "speed", "bmod.speed", NULL, k_bm_speed_alias, "Speed",
      "Units per second, times the base speed.", "x",
      XBOX_SET_FLOAT, ENHANCE_LIVE, 0, 1, 0.05, 20, 0.05, NULL },
    { "hud", "bmod.hud", NULL, NULL, "HUD while flying", "Keep or hide the HUD.", NULL,
      XBOX_SET_ENUM, ENHANCE_LIVE_ACTIVATE, 0, 0, 0, 0, 0, k_bm_hud },
    { "quality", "bmod.quality", NULL, NULL, "Quality", NULL, NULL,
      XBOX_SET_INT, ENHANCE_LIVE_RESTART, 0, 2, 0, 4, 1, NULL },
    { "pinned", "bmod.pinned", "RECOMP_HEAP_QUARANTINE", NULL, "Pinned", NULL, NULL,
      XBOX_SET_INT, ENHANCE_LIVE, 0, 0, 0, 100, 1, NULL },
};
static const char *const k_bm_toggle_def[] = { "F7", NULL };
static const char *const k_bm_rise_def[] = { "rt", "x", "R", NULL };
static const char *const k_bm_move_def[] = { "left_stick", NULL };
static const char *const k_bm_mouse_def[] = { "Look", NULL };
static const struct xbox_mod_control k_bm_ctl[] = {
    { "toggle", "Toggle flight", "Start or stop flying.", NULL, XBOX_CTL_ACTION, k_bm_toggle_def,
      NULL, NULL },
    { "rise", "Rise", NULL, "while flying", XBOX_CTL_BUTTON, k_bm_rise_def, NULL, NULL },
    { "move", "Move", NULL, "while flying", XBOX_CTL_STICK, k_bm_move_def, NULL, NULL },
    { "mouse", "Mouse", NULL, "while flying", XBOX_CTL_INFO, k_bm_mouse_def, NULL, NULL },
};
#define BM_SUMMARY "Fly a detached camera around the stage. The game pauses its camera " \
                   "while you fly, and the HUD can hide so the shot is clean."
static const struct xbox_mod_desc k_bm_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc),
    "B mod", BM_SUMMARY, "The test suite", "1.0",
    k_bm_set, 5, k_bm_ctl, 4
};
static void bm_enable(int on, void *u) { (void)u; (void)on; }
static int bm_init(const struct xbox_mod_api *api)
{
    return api->on_enable(bm_enable, NULL) ? 0 : -1;
}
static const struct xbox_mod k_bm = { "bmod", bm_init, NULL, &k_bm_desc };

/* Refused at validation (a second "speed"), its tables fine: the page
 * still lists what it offers, marked unavailable. */
static const struct xbox_mod_setting k_fm_set[] = {
    { "speed", "fmod.speed", NULL, NULL, "Speed", NULL, NULL,
      XBOX_SET_FLOAT, ENHANCE_LIVE, 0, 1, 0, 2, 0, NULL },
    { "speed", "fmod.speed2", NULL, NULL, "Speed again", NULL, NULL,
      XBOX_SET_FLOAT, ENHANCE_LIVE, 0, 1, 0, 2, 0, NULL },
};
static const char *const k_fm_def[] = { "F4", NULL };
static const struct xbox_mod_control k_fm_ctl[] = {
    { "go", "Go", NULL, NULL, XBOX_CTL_ACTION, k_fm_def, NULL, NULL },
    { "aim", "Aim", NULL, NULL, XBOX_CTL_INFO, k_bm_mouse_def, NULL, NULL },
};
static const struct xbox_mod_desc k_fm_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc),
    "Failing mod", "Never runs.", NULL, NULL, k_fm_set, 2, k_fm_ctl, 2
};
static const struct xbox_mod k_fm = { "fmod", bm_init, NULL, &k_fm_desc };
static const struct xbox_mod *const k_bmods[] = { &k_tm, &k_bm, &k_fm, NULL };

static int row_index(const char *label)
{
    char l[96];
    for (int i = 0; i < enhance_menu_rows(); i++) {
        enhance_menu_row(i, l, sizeof l, NULL, 0, NULL);
        if (!strcmp(l, label))
            return i;
    }
    return -1;
}

/* The row after the one labelled label (the next row of a group). */
static const char *row_after(const char *label, int k)
{
    int i = row_index(label);
    if (i < 0 || !enhance_menu_row(i + k, NULL, 0, s_val, sizeof s_val, NULL))
        return NULL;
    return s_val;
}

static void open_detail(const char *mod)
{
    CHECK(enhance_menu_set_page("Mods"));
    CHECK(enhance_menu_select_row(mod));
    tap(0x0D);
}

static void test_detail_rows(void)
{
    int i;
    set_env("RECOMP_HEAP_QUARANTINE", "7");
    start_with(ARMED "[bmod]\ntypo = 1\n", k_bmods);
    set_env("RECOMP_HEAP_QUARANTINE", NULL);
    open_menu();
    open_detail("B mod");
    /* The header, then Author, About (wrapped), State and Enabled. */
    CHECK(enhance_menu_row(0, NULL, 0, s_val, sizeof s_val, NULL) && !strcmp(s_val, "v1.0"));
    CHECK(row_value("B mod") != NULL);
    CHECK(row_value("Author") && !strcmp(s_val, "The test suite"));
    CHECK(row_value("About") != NULL);
    CHECK(row_value("State") && !strcmp(s_val, "On"));
    CHECK(row_value("Enabled") && !strcmp(s_val, "true"));
    /* No selectable text: the first stop is Enabled. */
    CHECK(!strcmp(selected_label(), "Enabled"));

    /* Controls: the action's bindings, the captured ones with the keys the
     * guest names come from, the info row; each with its note. */
    CHECK(row_value("Toggle flight") && !strcmp(s_val, "F7"));
    i = row_index("Toggle flight");
    CHECK(i >= 0 && !strcmp(enhance_menu_row_help(i), "Start or stop flying."));
    /* Two guest bindings and a host key: one separator each, none left
     * by the host key (the test's input map gives RT 3 and X C). */
    CHECK(row_value("Rise") && !strcmp(s_val, "rt, x, R (keys 3, C)"));
    CHECK(row_value("Move") && !strncmp(s_val, "left_stick", 10));
    CHECK(row_value("Mouse") && !strcmp(s_val, "Look"));
    i = row_index("Rise");
    CHECK(i >= 0 && strstr(enhance_menu_row_help(i), "[input.controls] \"bmod.rise\"") != NULL);
    CHECK(enhance_menu_select_row("Rise"));
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "read-only for now") != NULL);

    /* Settings: each class as the descriptor gives it, the forced one
     * read-only with its env row named. */
    CHECK(row_value("Speed") && !strcmp(s_val, "1.00"));
    CHECK(enhance_menu_row_live(row_index("Speed")) == ENHANCE_LIVE);
    CHECK(!strcmp(enhance_menu_row_help(row_index("Speed")),
                  "Units per second, times the base speed."));
    CHECK(row_value("HUD while flying") && !strcmp(s_val, "keep"));
    CHECK(enhance_menu_row_live(row_index("HUD while flying")) == ENHANCE_LIVE_ACTIVATE);
    CHECK(row_value("Quality") && !strcmp(s_val, "2"));
    CHECK(enhance_menu_row_live(row_index("Quality")) == ENHANCE_LIVE_RESTART);
    CHECK(row_value("Pinned") && !strcmp(s_val, "7"));
    CHECK(enhance_menu_select_row("Pinned"));
    tap(0x27);
    CHECK(strstr(enhance_menu_status(), "Set by RECOMP_HEAP_QUARANTINE") != NULL);
    CHECK(row_value("Pinned") && !strcmp(s_val, "7"));
    CHECK(!pending_any("bmod.pinned"));
    CHECK(row_index("Reset this mod") >= 0);
    /* The key under the mod's name nothing reads, kept. */
    CHECK(row_value("Unknown keys") != NULL);
    CHECK(row_value("bmod.typo") && !strcmp(s_val, "not read; kept"));

    /* A change moves the setting now and stages its key, written with the
     * kind's literal. */
    CHECK(enhance_menu_select_row("Speed"));
    tap(0x27);
    CHECK(row_value("Speed") && !strcmp(s_val, "1.05"));
    CHECK(xbox_mod_setting_value(1, 1) > 1.04 && xbox_mod_setting_value(1, 1) < 1.06);
    CHECK(pending_has("bmod.speed", "1.05"));
    CHECK(enhance_menu_unsaved() == 1);
    tap(0x25);
    CHECK(enhance_menu_unsaved() == 0);

    /* Back returns to the list, on the mod's row. */
    tap(0x08);
    CHECK(enhance_menu_is_open() && !strcmp(selected_label(), "B mod"));

    /* The refused mod: its reason, every row unavailable, no Reset. */
    open_detail("Failing mod");
    CHECK(row_value("State") && !strncmp(s_val, "Failed: ", 8));
    CHECK(row_value("Go") && !strcmp(s_val, "unavailable"));
    CHECK(row_value("Aim") && !strcmp(s_val, "unavailable"));
    CHECK(row_value("Speed") && !strcmp(s_val, "unavailable"));
    CHECK(row_index("Reset this mod") < 0);
    tap(0x08);

    /* A mod with no descriptor keeps its name, State, Enabled and items. */
    open_detail("testmod");
    CHECK(row_value("testmod") != NULL);
    CHECK(row_value("State") && !strcmp(s_val, "On"));
    CHECK(row_value("Level") && !strcmp(s_val, "low"));
    CHECK(row_index("Controls") < 0);
    open_menu();
}

/* About wraps at the word that no longer fits, at any panel width: the
 * lines put back together are the summary, and a narrower panel takes
 * more of them. */
static int wrap_lines(int w, char *joined, size_t n)
{
    int i, lines = 0;
    size_t at = 0;
    char l[96];
    nv2a_overlay_set_placement(0, 0, w, 1080);
    open_menu();
    open_detail("B mod");
    i = row_index("About");
    joined[0] = 0;
    for (; i >= 0 && enhance_menu_row(i, l, sizeof l, s_val, sizeof s_val, NULL); i++) {
        if (lines && l[0])
            break;
        at += (size_t)snprintf(joined + at, n - at, "%s%s", lines ? " " : "", s_val);
        lines++;
    }
    open_menu();
    return lines;
}

static void test_detail_wrap(void)
{
    char a[512], b[512], c[512];
    int la, lb, lc;
    start_with(ARMED, k_bmods);
    la = wrap_lines(1920, a, sizeof a);
    lb = wrap_lines(1400, b, sizeof b);
    lc = wrap_lines(1100, c, sizeof c);
    CHECK(la >= 2 && lb > la && lc > lb);
    CHECK(!strcmp(a, BM_SUMMARY) && !strcmp(b, BM_SUMMARY) && !strcmp(c, BM_SUMMARY));
    nv2a_overlay_set_placement(0, 0, 1920, 1080);
}

/* Reset this mod stages the mod's own settings and bindings back to the
 * stock; its switch, other mods and the unknown key are left alone. */
static void test_detail_reset(void)
{
    const char *keys[16], *vals[16], *f;
    int n;
    start_with(ARMED "[bmod]\nspeed = 2.0\nhud = \"hide\"\ntypo = 1\n[testmod]\nlevel = \"high\"\n"
                     "[input.actions]\n\"bmod.toggle\" = [\"F8\"]\n",
               k_bmods);
    open_menu();
    open_detail("B mod");
    CHECK(row_value("Speed") && !strcmp(s_val, "2.00"));
    CHECK(row_value("Toggle flight") && !strcmp(s_val, "F8"));
    CHECK(enhance_menu_select_row("Reset this mod"));
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "B mod: reset to defaults") != NULL);
    CHECK(row_value("Speed") && !strcmp(s_val, "1.00"));
    CHECK(row_value("Toggle flight") && !strcmp(s_val, "F7"));
    n = enhance_menu_pending(keys, vals, 16);
    /* The patch, by hand: the three keys removed, nothing else. */
    CHECK(n == 3);
    CHECK(pending_has("bmod.speed", NULL) && pending_has("bmod.hud", NULL));
    CHECK(pending_has("input.actions.bmod.toggle", NULL));
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "speed") == NULL && strstr(f, "hud") == NULL && strstr(f, "F8") == NULL);
    CHECK(strstr(f, "typo = 1") != NULL && strstr(f, "level = \"high\"") != NULL);
    open_menu();
}

/* A user-file alias goes under its key at the first Save, edits or none;
 * Enabled turned on drops a mods."<id>" = false. */
static void test_detail_save(void)
{
    const char *f;
    start_with("bmod_old_speed = 3.0\n" ARMED, k_bmods);
    open_menu();
    CHECK(enhance_menu_unsaved() == 0);
    CHECK(pending_has("bmod.speed", "3.0") && pending_has("bmod_old_speed", NULL));
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "bmod_old_speed") == NULL && strstr(f, "speed = 3.0") != NULL);
    CHECK(!pending_any("bmod.speed"));
    open_menu();

    start_with(ARMED "[mods]\nbmod = false\n", k_bmods);
    open_menu();
    open_detail("B mod");
    CHECK(row_value("State") && !strcmp(s_val, "Off (mods.\"bmod\" = false)"));
    CHECK(row_value("Toggle flight") && !strcmp(s_val, "F7 (mod off)"));
    CHECK(enhance_menu_select_row("Enabled"));
    tap(0x27);
    xbox_mod_frame();
    CHECK(pending_has("mods.bmod", NULL));
    open_menu();

    /* The player edits the new key while the old one is still in the
     * file: the edit wins, the alias's value is not written over it, and
     * the old key goes. */
    start_with("bmod_old_speed = 3.0\n" ARMED, k_bmods);
    open_menu();
    open_detail("B mod");
    CHECK(row_value("Speed") && !strcmp(s_val, "3.00"));
    CHECK(enhance_menu_select_row("Speed"));
    tap(0x27);
    CHECK(pending_has("bmod.speed", "3.05") && pending_has("bmod_old_speed", NULL));
    CHECK(!pending_has("bmod.speed", "3.0"));
    tap(VK_F5);
    CHECK(strstr(enhance_menu_status(), "Saved 1 change, 1 renamed key") != NULL);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "bmod_old_speed") == NULL && strstr(f, "speed = 3.05") != NULL);
    CHECK(strstr(f, "speed = 3.0\n") == NULL);
    open_menu();

    /* An alias holding the stock: the old key goes, nothing is written
     * under the new one (no key the player never set). */
    start_with("bmod_old_speed = 1.0\n" ARMED, k_bmods);
    open_menu();
    CHECK(!pending_any("bmod.speed") && pending_has("bmod_old_speed", NULL));
    tap(VK_F5);
    CHECK(strstr(enhance_menu_status(), "Saved 1 renamed key") != NULL);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "speed") == NULL);
    open_menu();
}

/* The capture refuses an input a dormant action holds, naming the mod and
 * the label, and a key a mod's captured control names (D3 rule 2). */
static void test_detail_conflicts(void)
{
    start_with(ARMED "[bmod]\nenabled = false\n", k_bmods);
    open_menu();
    CHECK(enhance_menu_set_page("Controls"));
    CHECK(enhance_menu_select_row("Stats strip"));
    tap(0x0D);
    CHECK(enhance_menu_capturing());
    tap(0x76);  /* F7 */
    CHECK(strstr(enhance_menu_status(),
                 "F7 refused: B mod: Toggle flight has it (F7) (mod off); clear it there first")
          != NULL);
    CHECK(enhance_menu_select_row("Stats strip"));
    tap(0x0D);
    CHECK(enhance_menu_capturing());
    tap('R');
    CHECK(strstr(enhance_menu_status(), "R refused: B mod: Rise uses it") != NULL);
    CHECK(enhance_menu_unsaved() == 0);
    open_menu();
}

/* The Controls page: the toolkit's actions, then each mod's under its
 * title, a dormant mod's group saying so; Mouse last. */
static void test_controls_groups(void)
{
    char l[96];
    int t, m, b, mo;
    start_with(ARMED "[bmod]\nenabled = false\n", k_bmods);
    open_menu();
    CHECK(enhance_menu_set_page("Controls"));
    t = row_index("Toolkit");
    m = row_index("testmod");
    b = row_index("B mod (mod off)");
    mo = row_index("Mouse");
    CHECK(t == 0 && m > t && b > m && mo > b);
    CHECK(row_index("Game") < 0);
    CHECK(enhance_menu_row(t + 1, l, sizeof l, NULL, 0, NULL) && !strcmp(l, "Open the menu"));
    CHECK(enhance_menu_row(m + 1, l, sizeof l, NULL, 0, NULL) && !strcmp(l, "Poke"));
    CHECK(enhance_menu_row(b + 1, l, sizeof l, NULL, 0, NULL) && !strcmp(l, "Toggle flight"));
    /* Captured controls show on the mod's own page only; the group
     * header says the mod is off, not each row. */
    CHECK(row_index("Rise") < 0);
    CHECK(row_after("B mod (mod off)", 1) && !strcmp(s_val, "F7"));
    open_menu();

    /* An action the title registers with no owner: its own group, between
     * the toolkit's and the mods'. */
    {
        static const char *const k_jump[] = { "F12", NULL };
        int g;
        start_with(ARMED, k_bmods);
        CHECK(enhance_action_register("game.jump", "Jump", k_jump) >= 0);
        open_menu();
        CHECK(enhance_menu_set_page("Controls"));
        t = row_index("Toolkit");
        g = row_index("Game");
        m = row_index("testmod");
        b = row_index("B mod");
        CHECK(t == 0 && g > t && m > g && b > m);
        CHECK(enhance_menu_row(g + 1, l, sizeof l, s_val, sizeof s_val, NULL) && !strcmp(l, "Jump")
              && !strcmp(s_val, "F12"));
        open_menu();
    }
}

/* ---- 7b. the Shaders page --------------------------------------------- */

/* The backend built a chain for the request in force: publish the stand-in
 * library's table (warpX 0.01 in 0..0.125 step 0.01; maskDark 0.5 in 0..2
 * step 0.05), as Metal and D3D11 do before their chain create. */
static void chain_built(void)
{
    void *preset = NULL;    /* the fake's fixed table */
    CHECK(nv2a_present_chain_lib() != NULL);
    nv2a_present_chain_params_publish(&preset, nv2a_present_chain_gen());
}

static void test_shaders(void)
{
    const char *f;
    float v = 0;
    unsigned applied;

    /* No shader: the picker and one info row. */
    start_with(ARMED, NULL);
    open_menu();
    CHECK(enhance_menu_set_page("Shaders"));
    CHECK(row_value("Profile") && !strcmp(row_value("Profile"), "Off"));
    CHECK(row_value("Parameters") && !strcmp(row_value("Parameters"), "no shader"));
    CHECK(!enhance_menu_select_row("Reset to preset"));
    open_menu();

    /* A shader whose chain is not up yet; the config holds an override for
     * it and one for another preset. */
    start_with(ARMED "[present]\nshader = \"crt-lottes\"\nshader_lib = \"" FAKE_LRS_PARAMS "\"\n"
               "[present.shader_params.crt-lottes]\nmaskDark = 0.8\n"
               "[present.shader_params.fsr]\nFSR_SHARPENING = 0.1\n", NULL);
    open_menu();
    CHECK(enhance_menu_set_page("Shaders"));
    CHECK(row_value("Profile") && !strcmp(row_value("Profile"), "crt-lottes"));
    CHECK(row_value("Chain") && !strcmp(row_value("Chain"), "building"));
    chain_built();
    flip();
    CHECK(row_value("warpX") && !strcmp(row_value("warpX"), "0.01"));
    CHECK(row_value("Lottes maskDark") && !strcmp(row_value("Lottes maskDark"), "0.80"));
    CHECK(enhance_menu_unsaved() == 0);

    /* Right: one step, live; the backend sees it at its next present. */
    applied = nv2a_present_chain_params_gen();
    CHECK(enhance_menu_select_row("warpX"));
    CHECK(enhance_menu_row_live(enhance_menu_selected()) == ENHANCE_LIVE);
    tap(0x27);
    CHECK(!strcmp(row_value("warpX"), "0.02"));
    CHECK(nv2a_present_chain_params_gen() != applied);
    CHECK(nv2a_present_chain_param_get("warpX", &v) && v > 0.0199f && v < 0.0201f);
    /* Left past the bottom stays there. */
    tap(0x25);
    tap(0x25);
    tap(0x25);
    CHECK(!strcmp(row_value("warpX"), "0.00"));
    CHECK(enhance_menu_unsaved() == 1);
    {
        const char *keys[16], *vals[16];
        int n = enhance_menu_pending(keys, vals, 16);
        CHECK(n == 1);
        CHECK(n == 1 && !strcmp(keys[0], "present.shader_params.crt-lottes.warpX") && vals[0]
              && !strcmp(vals[0], "0.0"));
    }
    tap(VK_F5);
    CHECK(strstr(enhance_menu_status(), "Saved 1 change") != NULL);
    CHECK(enhance_menu_unsaved() == 0);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "[present.shader_params.crt-lottes]") != NULL);
    CHECK(strstr(f, "warpX = 0.0") != NULL);
    CHECK(strstr(f, "maskDark = 0.8") != NULL);
    CHECK(strstr(f, "FSR_SHARPENING = 0.1") != NULL);

    /* Back to the preset's value: the key leaves the file. */
    CHECK(enhance_menu_select_row("Lottes maskDark"));
    for (int i = 0; i < 6; i++)
        tap(0x25);
    CHECK(!strcmp(row_value("Lottes maskDark"), "0.50"));
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "maskDark") == NULL);
    CHECK(strstr(f, "warpX = 0.0") != NULL);
    open_menu();

    /* The next start reads them back. */
    start_with(read_file("config/config.toml"), NULL);
    open_menu();
    CHECK(enhance_menu_set_page("Shaders"));
    chain_built();
    flip();
    CHECK(row_value("warpX") && !strcmp(row_value("warpX"), "0.00"));

    /* Reset to preset: this preset's overrides go, the other preset's
     * stay; Save writes the removal. */
    CHECK(enhance_menu_select_row("Reset to preset"));
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "crt-lottes") != NULL);
    CHECK(!strcmp(row_value("warpX"), "0.01"));
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "warpX") == NULL);
    CHECK(strstr(f, "FSR_SHARPENING = 0.1") != NULL);

    /* The Save page's Reset to defaults clears every preset's. */
    CHECK(enhance_menu_set_page("Files"));
    CHECK(enhance_menu_select_row("Reset to defaults"));
    tap(0x0D);
    CHECK(enhance_menu_select_row("Reset everything? A / Enter: yes"));
    tap(0x0D);
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "FSR_SHARPENING") == NULL);
    open_menu();

    /* A key without the <preset>.<param> shape, or not a number, warns and
     * is skipped. */
    {
        const char *out = start_with(ARMED "[present.shader_params]\nwarpX = 1.0\n"
                                     "[present.shader_params.x]\ny = \"a\"\n", NULL);
        CHECK(strstr(out, "present.shader_params.warpX") != NULL);
        CHECK(strstr(out, "present.shader_params.x.y") != NULL);
        CHECK(!nv2a_present_chain_override_get("x", "y", NULL));
    }
    nv2a_present_chain_params_clear();
}

/* ---- 7c. shader profiles (design D5, D10) ------------------------------ */

#define ONE_PASS "shaders = 1\nshader0 = \"x.slang\"\n"
#define PROFILES "config/shader-profiles"
#define CACHE "composed-cache"

/* The bundled presets beside the exe and the player's own under the root
 * (both the test's folder), one pass each: the edits find and compose them. */
static void shader_files(void)
{
    static const char *const k_names[] = { "crt-lottes", "crt-lottes-fast", "crt-hyllian", "fsr",
                                           "stage_a", "stage_b", "broken_one", NULL };
    char path[128];
    MKDIR("shaders");
    write_file("shaders/x.slang", "#version 450\n");
    for (int i = 0; k_names[i]; i++) {
        snprintf(path, sizeof path, "shaders/%s.slangp", k_names[i]);
        write_file(path, ONE_PASS);
    }
}

static void shader_files_remove(void)
{
    static const char *const k_names[] = { "crt-lottes", "crt-lottes-fast", "crt-hyllian", "fsr",
                                           "stage_a", "stage_b", "broken_one", NULL };
    char path[128];
    for (int i = 0; k_names[i]; i++) {
        snprintf(path, sizeof path, "shaders/%s.slangp", k_names[i]);
        remove(path);
    }
    remove("shaders/x.slang");
    rmdir("shaders");
}

/* The backend built the chain for the request in force: its own preset
 * file's table (a composed chain's has A1 SHARED B1 ORPHAN). */
static void chain_up(void)
{
    char path[1024];
    unsigned gen = nv2a_present_chain_gen(), g = 0;
    void *h = NULL;
    void *(*create)(const char *, void **) =
        (void *(*)(const char *, void **))nv2a_present_chain_lib()->sym("libra_preset_create");
    if (nv2a_present_chain_preset(path, sizeof path, &g) == 1)
        create(path, &h);
    nv2a_present_chain_params_publish(&h, gen);
    flip();
}

/* A pad button press and release, as tap() does for a key. */
static void press(WORD w)
{
    hold_btn(w, 1);
    advance(16);
    poll0();
    flip();
    hold_btn(w, 0);
    advance(16);
    poll0();
    flip();
}

static void press_a(void)
{
    hold_a(1);
    advance(16);
    poll0();
    flip();
    hold_a(0);
    advance(16);
    poll0();
    flip();
}

static void shift_tap(int vk)
{
    xbox_InputKeyEvent(0x10, 1);
    tap(vk);
    xbox_InputKeyEvent(0x10, 0);
}

#define CFG_PROF ARMED "[present]\nshader_lib = \"" FAKE_LRS_PARAMS "\"\n"

static void test_profiles(void)
{
    const char *f;
    int n0;

    MKDIR(PROFILES);
    MKDIR(CACHE);
    MKDIR(CACHE "/slangp");
    s_cache_root = CACHE;
    write_file(PROFILES "/mine.toml", "version = 1\nname = \"Mine\"\n"
                                      "stages = [\"user:stage_a\", \"user:stage_b\"]\n"
                                      "[params.stage_a]\nA1 = 1.5\n");
    write_file(PROFILES "/bad.toml", "version = 1\nname = \"Bad\"\nstages = \n");
    write_file(PROFILES "/newer.toml", "version = 2\nname = \"Newer\"\nstages = [\"fsr\"]\n");

    /* The picker: Off, the bundled, then the player's; a broken or newer
     * profile is listed but skipped. */
    start_with(CFG_PROF, NULL);
    open_menu();
    CHECK(enhance_menu_set_page("Shaders"));
    n0 = shader_edit_count();
    CHECK(n0 == 8);
    CHECK(!strcmp(row_value("Profile"), "Off"));
    CHECK(enhance_menu_select_row("Profile"));
    CHECK(enhance_menu_row_live(enhance_menu_selected()) == ENHANCE_LIVE);
    for (int i = 0; i < 4; i++)
        tap(0x27);
    CHECK(!strcmp(row_value("Profile"), "fsr"));
    tap(0x27);
    CHECK(!strcmp(row_value("Profile"), "Mine"));
    tap(0x27);
    CHECK(!strcmp(row_value("Profile"), "Off"));
    tap(0x25);
    CHECK(!strcmp(row_value("Profile"), "Mine"));

    /* Mine's chain: its stages, its pass count once built, its value. */
    chain_up();
    CHECK(row_value("Chain") && !strcmp(row_value("Chain"), "2 passes"));
    CHECK(row_value("1  user:stage_a") != NULL);
    CHECK(row_value("2  user:stage_b") != NULL);
    /* Two stages: the parameters live in each stage's own list, and a name
     * no stage lists on the main page. */
    CHECK(row_value("a one") == NULL);
    CHECK(row_value("orphan") != NULL);
    CHECK(row_value("Rename") && !strcmp(s_val, "Mine"));
    CHECK(enhance_menu_unsaved() == 1);   /* the choice */
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "profile = \"mine\"") != NULL);
    CHECK(enhance_menu_unsaved() == 0);

    /* A stage's list: its preset, its parameters with the shared note,
     * Move, Remove, Reset; B goes back to the stage row. */
    CHECK(enhance_menu_select_row("2  user:stage_b"));
    tap(0x0D);
    CHECK(row_value("Preset") && !strcmp(s_val, "user:stage_b"));
    CHECK(row_value("b one") != NULL);
    CHECK(row_value("a one") == NULL);
    CHECK(row_value("orphan") == NULL);
    CHECK(row_value("shared") != NULL);
    tap(0x1B);
    CHECK(enhance_menu_is_open());
    CHECK(!strcmp(selected_label(), "2  user:stage_b"));

    /* Move: Left on a stage row, live; the selection follows it. */
    tap(0x25);
    CHECK(row_value("1  user:stage_b") != NULL && row_value("2  user:stage_a") != NULL);
    CHECK(!strcmp(selected_label(), "1  user:stage_b"));
    CHECK(shader_edit_dirty(shader_edit_active()));
    tap(0x25);   /* at the top: nothing */
    CHECK(!strcmp(selected_label(), "1  user:stage_b"));
    /* Move down from the stage's list. */
    tap(0x0D);
    CHECK(enhance_menu_select_row("Move down"));
    tap(0x0D);
    CHECK(row_value("Preset") && !strcmp(s_val, "user:stage_b"));
    tap(0x1B);
    CHECK(row_value("1  user:stage_a") != NULL && row_value("2  user:stage_b") != NULL);
    CHECK(!shader_edit_dirty(shader_edit_active()));

    /* Remove asks first; Esc keeps the stage. */
    CHECK(enhance_menu_select_row("2  user:stage_b"));
    tap(0x2E);
    CHECK(enhance_menu_select_row("Remove user:stage_b? A / Enter: yes"));
    tap(0x1B);
    CHECK(enhance_menu_is_open());
    CHECK(row_value("2  user:stage_b") != NULL);
    tap(0x2E);
    CHECK(enhance_menu_select_row("Remove user:stage_b? A / Enter: yes"));
    tap(0x0D);
    CHECK(row_value("2  user:stage_b") == NULL);
    chain_up();
    /* One stage: its parameters on the main list again. */
    CHECK(row_value("a one") && !strcmp(s_val, "1.5"));
    CHECK(enhance_menu_select_row("1  user:stage_a"));
    tap(0x2E);
    CHECK(strstr(enhance_menu_status(), "The last stage stays") != NULL);
    CHECK(row_value("1  user:stage_a") != NULL);

    /* Add: a preset that does not parse is refused; one that does goes
     * last. */
    CHECK(enhance_menu_select_row("Add shader"));
    tap(0x0D);
    CHECK(enhance_menu_select_row("user:broken_one"));
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "does not parse") != NULL);
    CHECK(enhance_menu_select_row("user:stage_b"));
    tap(0x0D);
    CHECK(row_value("2  user:stage_b") != NULL);
    CHECK(!strcmp(selected_label(), "2  user:stage_b"));
    CHECK(strstr(enhance_menu_status(), "added") != NULL);

    /* Save writes the file. */
    tap(VK_F5);
    f = read_file(PROFILES "/mine.toml");
    CHECK(strstr(f, "user:stage_a") && strstr(f, "user:stage_b"));
    CHECK(strstr(f, "A1 = 1.5") != NULL);
    CHECK(enhance_menu_unsaved() == 0);
    CHECK(read_file(CACHE "/slangp/mine.slangp")[0] != 0);

    /* Rename on the keyboard: letters, Q and E, F5 type or are swallowed;
     * Enter keeps it. */
    CHECK(enhance_menu_select_row("Rename"));
    tap(0x0D);
    for (int i = 0; i < 4; i++)
        tap(0x08);
    shift_tap('M');
    tap('Y');
    tap(0x20);
    tap('Q');
    tap('E');
    tap(VK_F5);
    CHECK(!strcmp(enhance_menu_page_name(enhance_menu_page()), "Shaders"));
    CHECK(row_value("Rename") && !strcmp(s_val, "My qe|"));
    tap(0x25);
    tap(0x2E);
    CHECK(row_value("Rename") && !strcmp(s_val, "My q|"));
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "Renamed to My q") != NULL);
    CHECK(row_value("Profile") && !strcmp(s_val, "My q"));
    /* The chain may still read the old composed file until a request
     * names the new one; then the old one goes. */
    CHECK(read_file(CACHE "/slangp/mine.slangp")[0] != 0);
    tap(VK_F5);
    f = read_file(PROFILES "/my-q.toml");
    CHECK(strstr(f, "name = \"My q\"") != NULL);
    CHECK(read_file(PROFILES "/mine.toml")[0] == 0);
    CHECK(read_file(CACHE "/slangp/mine.slangp")[0] != 0);
    CHECK(enhance_menu_select_row("Profile"));
    tap(0x27);
    CHECK(!strcmp(row_value("Profile"), "Off"));
    CHECK(read_file(CACHE "/slangp/mine.slangp")[0] == 0);
    tap(0x25);
    chain_up();
    CHECK(read_file(CACHE "/slangp/my-q.slangp")[0] != 0);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "profile = \"my-q\"") != NULL);

    /* Rename on the pad: Up past the end adds a letter under the cursor
     * and cycles it; B cancels. */
    CHECK(enhance_menu_select_row("Rename"));
    press_a();
    press(XBOX_GAMEPAD_DPAD_UP);
    CHECK(row_value("Rename") && !strcmp(s_val, "My q|A"));
    press(XBOX_GAMEPAD_DPAD_UP);
    CHECK(row_value("Rename") && !strcmp(s_val, "My q|B"));
    f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 255;
    advance(16);
    poll0();
    f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 0;
    advance(16);
    poll0();
    CHECK(strstr(enhance_menu_status(), "Rename cancelled") != NULL);
    CHECK(enhance_menu_is_open());
    CHECK(row_value("Profile") && !strcmp(s_val, "My q"));

    /* Save as new and Duplicate: unsaved copies, chosen; Delete of an
     * unsaved one leaves no file. */
    CHECK(enhance_menu_select_row("Save as new"));
    tap(0x0D);
    CHECK(row_value("Profile") && !strcmp(s_val, "Profile 1"));
    CHECK(enhance_menu_unsaved() > 0);
    CHECK(enhance_menu_select_row("Duplicate"));
    tap(0x0D);
    CHECK(row_value("Profile") && !strcmp(s_val, "Profile 1 copy"));
    CHECK(enhance_menu_select_row("Delete"));
    tap(0x0D);
    CHECK(enhance_menu_select_row("Delete this profile? A / Enter: yes"));
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "Deleted Profile 1 copy") != NULL);
    CHECK(!strcmp(row_value("Profile"), "Off"));

    /* Delete a saved one: its file and present.profile go at once. */
    CHECK(enhance_menu_select_row("Profile"));
    tap(0x25);
    tap(0x25);
    CHECK(!strcmp(row_value("Profile"), "My q"));
    CHECK(enhance_menu_select_row("Delete"));
    tap(0x0D);
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "Deleted My q") != NULL);
    CHECK(read_file(PROFILES "/my-q.toml")[0] == 0);
    CHECK(read_file(CACHE "/slangp/my-q.slangp")[0] == 0);
    f = read_file("config/config.toml");
    /* The key, not the word: shader_lib's path may hold "profile". */
    CHECK(strstr(f, "\nprofile =") == NULL);

    /* The fork: an edit on a bundled preset makes "<name> custom". */
    CHECK(enhance_menu_select_row("Profile"));
    for (int i = 0; i < 12 && strcmp(row_value("Profile"), "fsr"); i++)
        tap(0x27);
    CHECK(enhance_menu_select_row("Add shader"));
    tap(0x0D);
    CHECK(enhance_menu_select_row("user:stage_a"));
    tap(0x0D);
    CHECK(row_value("Profile") && !strcmp(s_val, "fsr custom"));
    CHECK(strstr(enhance_menu_status(), "Now editing fsr custom") != NULL);
    CHECK(row_value("1  fsr") && row_value("2  user:stage_a"));
    /* F10 with the fork unsaved asks (D9a); Go Back keeps the menu. */
    open_menu();
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() == 2);
    advance(160);
    tap(0x1B);
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() < 0);

    /* The folder is read when the page opens: a file added meanwhile shows
     * at the next visit, and the unsaved profiles stay. */
    write_file(PROFILES "/later.toml", "version = 1\nname = \"Later\"\nstages = [\"fsr\"]\n");
    n0 = shader_edit_count();
    CHECK(enhance_menu_set_page("Render"));
    CHECK(enhance_menu_set_page("Shaders"));
    CHECK(shader_edit_count() == n0 + 1);
    CHECK(!strcmp(row_value("Profile"), "fsr custom"));

    /* Reset to defaults: the picker to Off, the files stay. */
    CHECK(enhance_menu_set_page("Files"));
    CHECK(enhance_menu_select_row("Reset to defaults"));
    tap(0x0D);
    CHECK(enhance_menu_select_row("Reset everything? A / Enter: yes"));
    tap(0x0D);
    CHECK(enhance_menu_set_page("Shaders"));
    CHECK(!strcmp(row_value("Profile"), "Off"));
    CHECK(read_file(PROFILES "/later.toml")[0] != 0);
    open_menu();

    remove(PROFILES "/later.toml");
    remove(PROFILES "/bad.toml");
    remove(PROFILES "/newer.toml");
    remove(PROFILES "/fsr-custom.toml");
    rmdir(PROFILES);
    remove(CACHE "/slangp/mine.slangp");
    remove(CACHE "/slangp/my-q.slangp");
    remove(CACHE "/slangp/~unsaved.slangp");   /* an unsaved profile's */
    rmdir(CACHE "/slangp");
    rmdir(CACHE);
    s_cache_root = NULL;
    nv2a_present_chain_params_clear();
}

/* An env var holds the chain: every chain edit is refused, its row dimmed
 * with the var's name, and the request and the files stay as they were. */
static void test_profiles_forced(void)
{
    char req[64];
    int i, n0;

    MKDIR(PROFILES);
    MKDIR(CACHE);
    MKDIR(CACHE "/slangp");
    s_cache_root = CACHE;
    write_file(PROFILES "/mine.toml", "version = 1\nname = \"Mine\"\n"
                                      "stages = [\"user:stage_a\", \"user:stage_b\"]\n");
    set_env("RECOMP_PRESENT_PROFILE", "mine");
    start_with(CFG_PROF, NULL);
    open_menu();
    CHECK(enhance_menu_set_page("Shaders"));
    CHECK(row_value("Profile") && !strcmp(s_val, "Mine"));
    n0 = shader_edit_count();
    chain_up();
    for (int k = 0; k < 6; k++) {
        static const char *const rows[] = { "Save as new", "Duplicate", "Delete",
                                            "Rename",      "Add shader", "Profile" };
        i = row_index(rows[k]);
        CHECK(i >= 0 && strstr(enhance_menu_row_help(i), "Set by RECOMP_PRESENT_PROFILE"));
    }
    /* Move, Remove, Add, Save as new, Duplicate, Delete, Rename. */
    CHECK(enhance_menu_select_row("1  user:stage_a"));
    tap(0x27);
    CHECK(strstr(enhance_menu_status(), "Set by RECOMP_PRESENT_PROFILE") != NULL);
    CHECK(row_value("1  user:stage_a") && row_value("2  user:stage_b"));
    tap(0x2E);
    CHECK(strstr(enhance_menu_status(), "Set by RECOMP_PRESENT_PROFILE") != NULL);
    CHECK(!enhance_menu_select_row("Remove user:stage_a? A / Enter: yes"));
    tap(0x0D);
    CHECK(enhance_menu_select_row("Move down"));
    i = enhance_menu_selected();
    CHECK(strstr(enhance_menu_row_help(i), "Set by RECOMP_PRESENT_PROFILE") != NULL);
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "Set by RECOMP_PRESENT_PROFILE") != NULL);
    CHECK(enhance_menu_select_row("Remove"));
    tap(0x0D);
    CHECK(!enhance_menu_select_row("Remove user:stage_a? A / Enter: yes"));
    tap(0x1B);
    CHECK(row_value("1  user:stage_a") && row_value("2  user:stage_b"));
    CHECK(enhance_menu_select_row("Add shader"));
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "Set by RECOMP_PRESENT_PROFILE") != NULL);
    CHECK(enhance_menu_select_row("Save as new"));
    tap(0x0D);
    CHECK(enhance_menu_select_row("Duplicate"));
    tap(0x0D);
    CHECK(enhance_menu_select_row("Delete"));
    tap(0x0D);
    tap(0x0D);
    CHECK(enhance_menu_select_row("Rename"));
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "Set by RECOMP_PRESENT_PROFILE") != NULL);
    CHECK(row_value("Rename") && !strcmp(s_val, "Mine"));
    CHECK(row_value("Profile") && !strcmp(s_val, "Mine"));
    CHECK(shader_edit_count() == n0);
    CHECK(!shader_edit_dirty(shader_edit_active()));
    CHECK(read_file(PROFILES "/mine.toml")[0] != 0);
    CHECK(nv2a_present_chain_requested_profile(req, sizeof req) && !strcmp(req, "mine"));
    open_menu();

    /* RECOMP_PRESENT_SHADER the same, on a bundled preset. */
    set_env("RECOMP_PRESENT_PROFILE", NULL);
    set_env("RECOMP_PRESENT_SHADER", "fsr");
    start_with(CFG_PROF, NULL);
    open_menu();
    CHECK(enhance_menu_set_page("Shaders"));
    CHECK(row_value("Profile") && !strcmp(s_val, "fsr"));
    chain_up();
    CHECK(enhance_menu_select_row("Save as new"));
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "Set by RECOMP_PRESENT_SHADER") != NULL);
    CHECK(enhance_menu_select_row("Add shader"));
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "Set by RECOMP_PRESENT_SHADER") != NULL);
    CHECK(row_value("Profile") && !strcmp(s_val, "fsr"));
    nv2a_present_chain_requested(req, sizeof req);
    CHECK(!strcmp(req, "fsr"));
    open_menu();
    set_env("RECOMP_PRESENT_SHADER", NULL);

    remove(PROFILES "/mine.toml");
    rmdir(PROFILES);
    remove(CACHE "/slangp/mine.slangp");
    rmdir(CACHE "/slangp");
    rmdir(CACHE);
    s_cache_root = NULL;
    nv2a_present_chain_params_clear();
}

/* start_with under a per-title file, T1/config.toml over config.toml. */
static void start_titled(const char *user, const char *title)
{
    MKDIR("T1");
    write_file("T1/config.toml", title);
    write_file("config/config.toml", user);
    xbox_mod_shutdown();
    enhance_menu_reset_for_test();
    nv2a_present_chain_params_reset_for_test();
    xbox_InputSetBackend(&k_fake);
    xbox_InputKeysClear();
    pad_clear();
    xbox_mod_register_table(NULL);
    capture_begin();
    xbox_enhance_init(".", "T1");
    capture_end();
    nv2a_overlay_set_placement(0, 0, 1920, 1080);
    poll0();
}

/* Delete takes present.profile out of every file that names the profile,
 * not only the one Save writes; one it cannot write is named in the
 * status. */
static void test_profiles_delete_layers(void)
{
    static const char mine[] = "version = 1\nname = \"Mine\"\nstages = [\"fsr\"]\n";
    const char *f;

    MKDIR(PROFILES);
    MKDIR(CACHE);
    MKDIR(CACHE "/slangp");
    s_cache_root = CACHE;
    write_file(PROFILES "/mine.toml", mine);
    start_titled(CFG_PROF "profile = \"mine\"\n", ARMED "[present]\nprofile = \"mine\"\n");
    open_menu();
    CHECK(enhance_menu_set_page("Shaders"));
    CHECK(row_value("Profile") && !strcmp(s_val, "Mine"));
    CHECK(enhance_menu_select_row("Delete"));
    tap(0x0D);
    tap(0x0D);
    CHECK(!strcmp(enhance_menu_status(), "Deleted Mine"));
    CHECK(read_file(PROFILES "/mine.toml")[0] == 0);
    f = read_file("T1/config.toml");
    CHECK(strstr(f, "profile =") == NULL);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "\nprofile =") == NULL);
    CHECK(strstr(f, "shader_lib") != NULL);
    open_menu();

#ifndef _WIN32
    /* The user file under the title file, in a folder it cannot write. */
    write_file(PROFILES "/mine.toml", mine);
    start_titled(CFG_PROF "profile = \"mine\"\n", ARMED);
    open_menu();
    CHECK(enhance_menu_set_page("Shaders"));
    CHECK(row_value("Profile") && !strcmp(s_val, "Mine"));
    chmod("config", 0555);
    CHECK(enhance_menu_select_row("Delete"));
    tap(0x0D);
    tap(0x0D);
    chmod("config", 0755);
    f = enhance_menu_status();
    CHECK(strstr(f, "Deleted Mine; present.profile still names it in ") != NULL);
    CHECK(strstr(f, "config.toml") != NULL);
    CHECK(read_file(PROFILES "/mine.toml")[0] == 0);
    CHECK(strstr(read_file("config/config.toml"), "\nprofile = \"mine\"") != NULL);
    open_menu();
#endif

    remove("T1/config.toml");
    rmdir("T1");
    rmdir(PROFILES);
    remove(CACHE "/slangp/mine.slangp");
    rmdir(CACHE "/slangp");
    rmdir(CACHE);
    s_cache_root = NULL;
    nv2a_present_chain_params_clear();
}

/* The start's profile request waits for the first present. The Shaders
 * page opened before it loads the file's values at its own first request;
 * opened after, the store's (edited) values stand. */
static void test_profiles_early(void)
{
    float v = 0;

    MKDIR(PROFILES);
    MKDIR(CACHE);
    MKDIR(CACHE "/slangp");
    s_cache_root = CACHE;
    write_file(PROFILES "/early.toml", "version = 1\nname = \"Early\"\n"
                                       "stages = [\"user:stage_a\"]\n[params.stage_a]\nA1 = 1.5\n");
    start_with(CFG_PROF "profile = \"early\"\n", NULL);
    open_menu();
    CHECK(enhance_menu_set_page("Shaders"));
    CHECK(row_value("Profile") && !strcmp(s_val, "Early"));
    CHECK(!nv2a_present_chain_profile_value_get("early", "A1", &v));
    CHECK(enhance_menu_select_row("Add shader"));
    tap(0x0D);
    CHECK(enhance_menu_select_row("user:stage_b"));
    tap(0x0D);
    CHECK(row_value("2  user:stage_b") != NULL);
    CHECK(nv2a_present_chain_profile_value_get("early", "A1", &v) && v == 1.5f);
    open_menu();

    /* After the first present: an edit on the page survives the next
     * request. */
    start_with(CFG_PROF "profile = \"early\"\n", NULL);
    chain_up();
    CHECK(nv2a_present_chain_profile_value_get("early", "A1", &v) && v == 1.5f);
    nv2a_present_chain_profile_value_load("early", "A1", 2.5f);
    open_menu();
    CHECK(enhance_menu_set_page("Shaders"));
    CHECK(enhance_menu_select_row("Add shader"));
    tap(0x0D);
    CHECK(enhance_menu_select_row("user:stage_b"));
    tap(0x0D);
    CHECK(row_value("2  user:stage_b") != NULL);
    CHECK(nv2a_present_chain_profile_value_get("early", "A1", &v) && v == 2.5f);
    open_menu();

    remove(PROFILES "/early.toml");
    rmdir(PROFILES);
    remove(CACHE "/slangp/early.slangp");
    remove(CACHE "/slangp/~unsaved.slangp");
    rmdir(CACHE "/slangp");
    rmdir(CACHE);
    s_cache_root = NULL;
    nv2a_present_chain_params_clear();
}

/* A long chain error on the Chain header is cut to the room the label
 * leaves: drawn whole from the right edge, it ran over "Chain" and out of
 * the panel (seen on Metal with a missing stage's path). */
static uint32_t premul_of(mu_Color c)
{
    unsigned char b[4] = { (unsigned char)((c.r * c.a + 127) / 255),
                           (unsigned char)((c.g * c.a + 127) / 255),
                           (unsigned char)((c.b * c.a + 127) / 255), c.a };
    uint32_t v;
    memcpy(&v, b, 4);
    return v;
}

static void test_profiles_long_error(void)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    const struct nv2a_ovl_list *l;
    uint32_t bad = premul_of(t->bad), accent = premul_of(t->accent);
    float bad_y = 1e9f, bad_x = 1e9f, label_x = -1e9f;

    MKDIR(PROFILES);
    MKDIR(CACHE);
    MKDIR(CACHE "/slangp");
    s_cache_root = CACHE;
    write_file(PROFILES "/far.toml",
               "version = 1\nname = \"Far\"\nstages = [\"user:stage_a\", \"shaders/"
               "MW-a-preset-named-WIDE-MMMMWWWWMMMMWWWWMMMMWWWWMMMMWWWWMMMMWWWW.slangp\"]\n");
    start_with(CFG_PROF "profile = \"far\"\n", NULL);
    chain_up();
    open_menu();
    CHECK(enhance_menu_set_page("Shaders"));
    CHECK(row_value("Chain") && strstr(s_val, "MW-a-preset-named-WIDE") != NULL);
    enhance_menu_draw(100000);
    l = nv2a_overlay_acquire(NV2A_OVL_MENU);
    CHECK(l != NULL && l->nvtx > 0);
    for (uint32_t i = 0; l && i < l->nvtx; i++)
        if (l->vtx[i].rgba == bad && l->vtx[i].y < bad_y)
            bad_y = l->vtx[i].y;
    /* The topmost red text is the header's; "Chain" sits on its line. */
    for (uint32_t i = 0; l && i < l->nvtx; i++) {
        const struct nv2a_ovl_vtx *v = &l->vtx[i];
        if (v->y < bad_y - 2 || v->y > bad_y + 10)
            continue;
        if (v->rgba == bad && v->x < bad_x)
            bad_x = v->x;
        if (v->rgba == accent && v->x > label_x)
            label_x = v->x;
    }
    CHECK(label_x > 0);
    CHECK(bad_x > label_x);
    open_menu();

    remove(PROFILES "/far.toml");
    rmdir(PROFILES);
    remove(CACHE "/slangp/far.slangp");
    remove(CACHE "/slangp/~unsaved.slangp");
    rmdir(CACHE "/slangp");
    rmdir(CACHE);
    s_cache_root = NULL;
    nv2a_present_chain_params_clear();
}

static uint32_t sum_of(const struct nv2a_ovl_list *l)
{
    return nv2a_overlay_list_sum(l->vtx, l->nvtx, l->idx, l->nidx, l->cmd, l->ncmd);
}

static void test_pages(void)
{
    /* Render, Shaders, Pack, Game, Mods, Stats, Controls, Debug, Files. */
    static const uint32_t k_sum[9] = {
        0x39322648u, 0x50367F1Eu, 0x3FC9A1FBu, 0x0F4B7064u, 0x64E35CB8u,
        0xDE84570Bu, 0x8633F632u, 0xE72426AAu, 0xB2C4E2BAu };
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_is_open());
    for (int p = 0; p < enhance_menu_page_count(); p++) {
        const struct nv2a_ovl_list *l;
        uint32_t want = p < 9 ? k_sum[p] : 0;   /* a tenth page lands unpinned */
        CHECK(enhance_menu_set_page(enhance_menu_page_name(p)));
        enhance_menu_draw(100000);
        l = nv2a_overlay_acquire(NV2A_OVL_MENU);
        CHECK(l != NULL);
        if (!l)
            continue;
        CHECK(l->ncmd > 0 && l->sum == sum_of(l));
        /* A page with no pin fails: --pin prints every sum to pin a new
         * or changed page. The sum is over positions quantised to 1/16 px
         * (nv2a_overlay_list_sum), so a compiler's last float bit or a
         * host's libm does not move it; only a layout change does. */
        if (s_pin)
            printf("page %s = 0x%08X\n", enhance_menu_page_name(p), l->sum);
        else if (l->sum != want) {
            s_fail++;
            fprintf(stderr, "FAIL page %s: 0x%08X, want 0x%08X%s\n", enhance_menu_page_name(p),
                    l->sum, want, want ? "" : " (unpinned: run with --pin)");
        }
    }
    open_menu();
    CHECK(!(nv2a_overlay_visible() & (1u << NV2A_OVL_MENU)));
}

/* The library in force for every other test: the stand-in, loaded once,
 * under a presenter that says it runs chains (Metal and D3D11 do at init). */
static void shaders_supported(void)
{
    nv2a_present_chain_lib_reset_for_test();
    nv2a_present_chain_set_lib(FAKE_LRS_PARAMS);
    CHECK(nv2a_present_chain_lib() != NULL);
    nv2a_present_chain_host("test");
}

static int page_index(const char *name)
{
    for (int p = 0; p < enhance_menu_page_count(); p++)
        if (!strcmp(enhance_menu_page_name(p), name))
            return p;
    return -1;
}

/* Shaders hidden where none can run (the user, 2026-10-29). */
static void test_shaders_hidden(void)
{
    const char *out;
    int sp = page_index("Shaders");

    /* Supported with no shader: the page stays, for its picker. */
    start_with(ARMED, NULL);
    open_menu();
    CHECK(enhance_menu_page_shown(sp));
    CHECK(enhance_menu_set_page("Shaders"));
    open_menu();

    /* The library does not load (as on a Windows without D3DX9). */
    nv2a_present_chain_lib_reset_for_test();
    nv2a_present_chain_host("test");
    start_with(ARMED "[present]\nshader = \"crt-lottes\"\n"
               "shader_lib = \"no-such-librashader.bin\"\n", NULL);
    capture_begin();
    open_menu();
    out = capture_end();
    CHECK(enhance_menu_is_open());
    CHECK(strstr(out, "[SHADER] the menu's Shaders page is hidden: librashader not found") != NULL);
    CHECK(!enhance_menu_page_shown(sp));
    CHECK(!enhance_menu_set_page("Shaders"));
    CHECK(!strcmp(enhance_menu_page_name(enhance_menu_page()), "Render"));
    /* The picker is not moved to Render. */
    CHECK(row_value("Profile") == NULL);
    tap('E');
    CHECK(!strcmp(enhance_menu_page_name(enhance_menu_page()), "Pack"));
    tap('Q');
    CHECK(!strcmp(enhance_menu_page_name(enhance_menu_page()), "Render"));
    tap('Q');
    CHECK(!strcmp(enhance_menu_page_name(enhance_menu_page()), "Files"));
    tap('E');
    CHECK(!strcmp(enhance_menu_page_name(enhance_menu_page()), "Render"));
    /* Said once: a second open is quiet. */
    open_menu();
    capture_begin();
    open_menu();
    out = capture_end();
    CHECK(enhance_menu_is_open() && !strstr(out, "Shaders page is hidden"));
    open_menu();

    /* The library loads, but no presenter runs chains (the SDL renderer,
     * the CPU rasteriser's window on Windows). */
    nv2a_present_chain_lib_reset_for_test();
    start_with(ARMED "[present]\nshader_lib = \"" FAKE_LRS_PARAMS "\"\n", NULL);
    capture_begin();
    open_menu();
    out = capture_end();
    CHECK(strstr(out, "Shaders page is hidden: this presenter runs no shader chain") != NULL);
    CHECK(!enhance_menu_set_page("Shaders"));
    open_menu();

    shaders_supported();
}

/* Save under a root whose folders do not exist yet makes all of them (N7). */
static void test_save_deep(void)
{
    const char *f;
    remove("deep/a/b/config/config.toml");
    rmdir("deep/a/b/config");
    rmdir("deep/a/b");
    rmdir("deep/a");
    rmdir("deep");
    set_env("RECOMP_MENU", "1");
    start_with(NULL, NULL);
    xbox_mod_shutdown();
    enhance_menu_reset_for_test();
    capture_begin();
    xbox_enhance_init("deep/a/b", NULL);
    capture_end();
    poll0();
    open_menu();
    CHECK(enhance_menu_select_row("Scaling filter"));
    tap(0x27);
    tap(VK_F5);
    CHECK(strstr(enhance_menu_status(), "Saved 1 change") != NULL);
    f = read_file("deep/a/b/config/config.toml");
    CHECK(strstr(f, "filter = \"linear\"") != NULL);
    open_menu();
    set_env("RECOMP_MENU", NULL);
    remove("deep/a/b/config/config.toml");
    rmdir("deep/a/b/config");
    rmdir("deep/a/b");
    rmdir("deep/a");
    rmdir("deep");
}

/* A Save that leaves keys out for [table]s in a config under a long path
 * (a macOS ~/Library/Application Support/... one) names every key in the
 * log, and the status line gives the count. */
#define LONGROOT "lr_Library_Application_Support_with_a_long_user_name_and_a_game_folder" \
                 "_name_that_runs_on_well_past_what_the_old_buffer_held_xx"
static void test_save_tables(void)
{
    const char *f, *log;
    MKDIR(LONGROOT);
    MKDIR(LONGROOT "/config");
    write_file(LONGROOT "/config/config.toml",
               ARMED "[mods.testmod]\nx = 1\n[mods.plainmod]\ny = 1\n");
    xbox_mod_shutdown();
    enhance_menu_reset_for_test();
    nv2a_overlay_reset_for_test();
    xbox_InputSetBackend(&k_fake);
    xbox_InputKeysClear();
    pad_clear();
    xbox_mod_register_table(k_mods);
    capture_begin();
    xbox_enhance_init(LONGROOT, NULL);
    capture_end();
    nv2a_overlay_set_placement(0, 0, 1920, 1080);
    poll0();
    open_menu();
    CHECK(enhance_menu_set_page("Mods"));
    CHECK(enhance_menu_select_row("testmod"));
    tap(0x25);
    CHECK(enhance_menu_select_row("plainmod"));
    tap(0x27);
    CHECK(enhance_menu_set_page("Render"));
    CHECK(enhance_menu_select_row("Scaling filter"));
    tap(0x27);
    capture_begin();
    tap(VK_F5);
    log = capture_end();
    CHECK(strstr(enhance_menu_status(), "Saved; 2 keys left out") != NULL);
    CHECK(strstr(log, "2 keys left out") != NULL);
    CHECK(strstr(log, "mods.testmod is the table [mods.testmod]") != NULL);
    CHECK(strstr(log, "; mods.plainmod is the table [mods.plainmod]; write its keys flat"
                      " under [mods] (left as it is)\n") != NULL);
    f = read_file(LONGROOT "/config/config.toml");
    CHECK(strstr(f, "filter = \"linear\"") != NULL);
    CHECK(strstr(f, "testmod = ") == NULL && strstr(f, "plainmod = ") == NULL);
    open_menu();
    remove(LONGROOT "/config/config.toml");
    rmdir(LONGROOT "/config");
    rmdir(LONGROOT);
}

/* A title's STAGE item keeps its row as before the descriptor classes:
 * its label, its value and its class. */
static const char *const k_gm_lv[] = { "near", "far", NULL };
static const struct enhance_item k_gm_stage = {
    "fx.lod", "Draw distance", "Game", ENHANCE_ITEM_CHOICE, k_gm_lv, 0, 0, 0, NULL,
    NULL, NULL, ENHANCE_LIVE_STAGE, 0, "near", NULL, NULL
};
static void test_stage_row(void)
{
    int i;
    start_with(ARMED, NULL);
    CHECK(enhance_menu_add(&k_gm_stage));
    open_menu();
    CHECK(enhance_menu_set_page("Game"));
    i = row_index("Draw distance");
    CHECK(i >= 0 && enhance_menu_row_live(i) == ENHANCE_LIVE_STAGE);
    CHECK(row_value("Draw distance") && !strcmp(s_val, "near"));
    CHECK(enhance_menu_select_row("Draw distance"));
    tap(0x27);
    CHECK(row_value("Draw distance") && !strcmp(s_val, "far"));
    CHECK(pending_has("fx.lod", "\"far\""));
    CHECK(enhance_menu_row_live(-1) == -1);
    open_menu();
}

/* ---- D10 badges, revert, the D9a dialog, the footer keys -------------- */

static int row_at(const char *label)
{
    char l[96];
    int n = enhance_menu_rows();
    for (int i = 0; i < n; i++) {
        enhance_menu_row(i, l, sizeof l, NULL, 0, NULL);
        if (!strcmp(l, label))
            return i;
    }
    return -1;
}

#define ST_U ENHANCE_ROW_UNSAVED
#define ST_N ENHANCE_ROW_NONDEFAULT
#define ST_F ENHANCE_ROW_FORCED
static int state_of(const char *label) { return enhance_menu_row_state(row_at(label)); }

/* D10 cases 1-7: "unsaved" is against the saved value, "non-default"
 * against stock, and an env-forced row is neither changeable nor unsaved. */
static void test_badges(void)
{
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_select_row("Render scale"));
    CHECK(state_of("Render scale") == 0);                       /* 1 fresh */
    tap(0x27);
    CHECK(state_of("Render scale") == (ST_U | ST_N));           /* 2 set */
    tap(VK_F5);
    CHECK(state_of("Render scale") == ST_N);                    /* 3 saved */
    CHECK(enhance_menu_unsaved() == 0);
    tap(0x27);
    CHECK(state_of("Render scale") == (ST_U | ST_N));
    tap(0x25);
    CHECK(!strcmp(row_value("Render scale"), "2"));
    CHECK(state_of("Render scale") == ST_N);                    /* 4 back */
    CHECK(enhance_menu_unsaved() == 0);
    tap(0x2E);                                                  /* Delete: clear */
    CHECK(!strcmp(row_value("Render scale"), "1"));
    CHECK(state_of("Render scale") == ST_U);                    /* 5 cleared */
    CHECK(strstr(enhance_menu_status(), "reset to default (1)") != NULL);
    enhance_menu_revert();
    CHECK(!strcmp(row_value("Render scale"), "2"));
    CHECK(state_of("Render scale") == ST_N);                    /* 6 revert */
    CHECK(enhance_menu_unsaved() == 0);
    open_menu();
    CHECK(!enhance_menu_is_open());

    /* 7: forced by the environment. */
    set_env("RECOMP_PRESENT_FILTER", "linear");
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_select_row("Scaling filter"));
    CHECK(state_of("Scaling filter") == (ST_F | ST_N));
    tap(0x27);
    tap(0x2E);
    CHECK(!(state_of("Scaling filter") & ST_U));
    CHECK(strstr(enhance_menu_status(), "Set by") != NULL);
    CHECK(enhance_menu_unsaved() == 0);
    open_menu();
    CHECK(!enhance_menu_is_open());
    set_env("RECOMP_PRESENT_FILTER", NULL);
}

/* Revert per kind: a value, a mod toggle and a binding go back to the
 * saved state through set(), and each leaves unsaved at 0. */
static void test_revert(void)
{
    const char *t[2] = { "F6", "F7" };
    const char *back[1] = { "F6" };
    int poke;
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_select_row("Scaling filter"));
    tap(0x27);
    CHECK(!strcmp(row_value("Scaling filter"), "linear"));
    flip();
    CHECK(nv2a_host_opts()->present_filter == 1);
    CHECK(enhance_menu_unsaved() == 1);
    enhance_menu_revert();
    flip();
    CHECK(enhance_menu_unsaved() == 0);
    CHECK(!strcmp(row_value("Scaling filter"), "nearest"));
    CHECK(nv2a_host_opts()->present_filter == 0);   /* set() saw the saved value */

    /* A mod toggle. */
    CHECK(enhance_menu_set_page("Mods"));
    CHECK(enhance_menu_select_row("testmod"));
    tap(0x25);
    CHECK(!xbox_mod_enabled(0));
    CHECK(enhance_menu_unsaved() == 1);
    CHECK(state_of("testmod") & ST_U);
    enhance_menu_revert();
    CHECK(xbox_mod_enabled(0));
    CHECK(enhance_menu_unsaved() == 0);
    CHECK(!(state_of("testmod") & ST_U));

    /* A binding: changed, reverted; changed and put back by hand is clean. */
    CHECK(enhance_menu_set_page("Controls"));
    poke = enhance_action_find("testmod.poke");
    CHECK(poke >= 0);
    enhance_action_set_bindings(poke, t, 2);
    CHECK(enhance_menu_unsaved() == 1);
    CHECK(state_of("Poke") == (ST_U | ST_N));
    enhance_menu_revert();
    CHECK(enhance_menu_unsaved() == 0);
    CHECK(!strcmp(row_value("Poke"), "F6"));
    CHECK(state_of("Poke") == 0);
    enhance_action_set_bindings(poke, t, 2);
    enhance_action_set_bindings(poke, back, 1);
    CHECK(enhance_menu_unsaved() == 0);
    CHECK(state_of("Poke") == 0);

    /* The same through the menu's capture (action_set): F7 added, all
     * cleared, F6 captured again leaves the row clean. */
    CHECK(enhance_menu_select_row("Poke"));
    tap(0x0D);
    CHECK(enhance_menu_capturing());
    tap(VK_F7);
    CHECK(!strcmp(row_value("Poke"), "F6, F7"));
    CHECK(enhance_menu_unsaved() == 1);
    tap(0x2E);
    CHECK(!strcmp(row_value("Poke"), "none"));
    CHECK(enhance_menu_unsaved() == 1);
    tap(0x0D);
    CHECK(enhance_menu_capturing());
    tap(0x75);  /* F6 */
    CHECK(!enhance_menu_capturing());
    CHECK(!strcmp(row_value("Poke"), "F6"));
    CHECK(enhance_menu_unsaved() == 0);
    CHECK(state_of("Poke") == 0);
    advance(16);
    poll0();
    open_menu();
    CHECK(!enhance_menu_is_open());

    /* An off-grid value as loaded: within half a step of what the item
     * shows, so it is clean, and a step there and back stays clean. */
    start_with(ARMED "[render]\nscale = 2.4\n", k_mods);
    open_menu();
    CHECK(enhance_menu_select_row("Render scale"));
    CHECK(!(state_of("Render scale") & ST_U));
    CHECK(enhance_menu_unsaved() == 0);
    tap(0x27);
    CHECK(enhance_menu_unsaved() == 1);
    tap(0x25);
    CHECK(enhance_menu_unsaved() == 0);
    open_menu();
    CHECK(!enhance_menu_is_open());
}

static void change_scale_at(int line)
{
    int ok = enhance_menu_set_page("Render") && enhance_menu_select_row("Render scale");
    tap(0x27);
    ok = ok && enhance_menu_unsaved() == 1;
    if (!ok)
        fprintf(stderr, "change_scale failed from line %d\n", line);
    CHECK(ok);
}
#define change_scale() change_scale_at(__LINE__)

static void shift_esc(void)
{
    xbox_InputKeyEvent(0x10, 1);
    tap(0x1B);
    xbox_InputKeyEvent(0x10, 0);
}

static void pad_tap(WORD w)
{
    hold_btn(w, 1);
    advance(16);
    poll0();
    hold_btn(w, 0);
    advance(16);
    poll0();
}

static void hold_b(int on) { f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_B] = on ? 255 : 0; }

static void pad_b(void)
{
    hold_b(1);
    advance(16);
    poll0();
    hold_b(0);
    advance(16);
    poll0();
}

/* Revert on the Shaders page: a preset override, and through
 * shader_edit_revert() a rename, an unsaved New, a fork and the choice.
 * Then the dialog's deleted-profile line, and a Cmd+S typed into a name. */
static void test_revert_shaders(void)
{
    int n0;
    float v = 0;

    /* A preset override goes back to the saved value. */
    start_with(ARMED "[present]\nshader = \"crt-lottes\"\nshader_lib = \"" FAKE_LRS_PARAMS "\"\n"
               "[present.shader_params.crt-lottes]\nwarpX = 0.03\n", NULL);
    open_menu();
    CHECK(enhance_menu_set_page("Shaders"));
    chain_built();
    flip();
    CHECK(row_value("warpX") && !strcmp(s_val, "0.03"));
    CHECK(enhance_menu_select_row("warpX"));
    tap(0x27);
    CHECK(!strcmp(row_value("warpX"), "0.04"));
    CHECK(enhance_menu_unsaved() == 1);
    enhance_menu_revert();
    flip();
    CHECK(enhance_menu_unsaved() == 0);
    CHECK(row_value("warpX") && !strcmp(s_val, "0.03"));
    CHECK(nv2a_present_chain_param_get("warpX", &v) && v > 0.0299f && v < 0.0301f);
    open_menu();
    CHECK(!enhance_menu_is_open());
    nv2a_present_chain_params_clear();

    MKDIR(PROFILES);
    MKDIR(CACHE);
    MKDIR(CACHE "/slangp");
    s_cache_root = CACHE;
    write_file(PROFILES "/mine.toml", "version = 1\nname = \"Mine\"\n"
                                      "stages = [\"user:stage_a\", \"user:stage_b\"]\n");
    start_with(CFG_PROF "profile = \"mine\"\n", NULL);
    open_menu();
    CHECK(enhance_menu_set_page("Shaders"));
    CHECK(row_value("Profile") && !strcmp(s_val, "Mine"));
    chain_up();
    n0 = shader_edit_count();
    CHECK(enhance_menu_unsaved() == 0);

    /* The choice. */
    CHECK(enhance_menu_select_row("Profile"));
    tap(0x27);
    CHECK(strcmp(row_value("Profile"), "Mine"));
    CHECK(enhance_menu_unsaved() > 0);
    enhance_menu_revert();
    CHECK(row_value("Profile") && !strcmp(s_val, "Mine"));
    CHECK(enhance_menu_unsaved() == 0);

    /* A rename, with a Cmd+S typed while the name is open: it is spent
     * there, not a Save once the name ends. */
    CHECK(enhance_menu_select_row("Rename"));
    tap(0x0D);
    for (int i = 0; i < 4; i++)
        tap(0x08);
    shift_tap('Z');
    xbox_InputShortcutPost('S');
    advance(16);
    poll0();
    tap(0x0D);
    advance(16);
    poll0();
    CHECK(row_value("Profile") && !strcmp(s_val, "Z"));
    CHECK(enhance_menu_unsaved() > 0);
    CHECK(read_file(PROFILES "/z.toml")[0] == 0);
    enhance_menu_revert();
    CHECK(row_value("Profile") && !strcmp(s_val, "Mine"));
    CHECK(row_value("Rename") && !strcmp(s_val, "Mine"));
    CHECK(enhance_menu_unsaved() == 0);

    /* An unsaved New goes. */
    CHECK(enhance_menu_select_row("Save as new"));
    tap(0x0D);
    CHECK(row_value("Profile") && !strcmp(s_val, "Profile 1"));
    CHECK(shader_edit_count() == n0 + 1);
    enhance_menu_revert();
    CHECK(shader_edit_count() == n0);
    CHECK(row_value("Profile") && !strcmp(s_val, "Mine"));
    CHECK(enhance_menu_unsaved() == 0);

    /* A fork of a bundled preset goes, and the choice comes back. */
    CHECK(enhance_menu_select_row("Profile"));
    for (int i = 0; i < 12 && strcmp(row_value("Profile"), "fsr"); i++)
        tap(0x27);
    CHECK(enhance_menu_select_row("Add shader"));
    tap(0x0D);
    CHECK(enhance_menu_select_row("user:stage_a"));
    tap(0x0D);
    CHECK(row_value("Profile") && !strcmp(s_val, "fsr custom"));
    enhance_menu_revert();
    CHECK(shader_edit_count() == n0);
    CHECK(row_value("Profile") && !strcmp(s_val, "Mine"));
    CHECK(enhance_menu_unsaved() == 0);

    /* The deleted-profile line: a Delete is written at once, so the
     * dialog says Exit without saving does not bring it back. */
    CHECK(enhance_menu_select_row("Delete"));
    tap(0x0D);
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "Deleted Mine") != NULL);
    CHECK(shader_edit_deleted());
    change_scale();
    CHECK(enhance_menu_set_page("Shaders"));
    tap(0x1B);
    CHECK(enhance_menu_dialog() == 2);
    CHECK(strstr(enhance_menu_dialog_text(), "Deleted profiles are gone already") != NULL);
    advance(160);
    tap('G');
    /* A save is the new base: the line goes with it. */
    tap(VK_F5);
    CHECK(enhance_menu_unsaved() == 0);
    CHECK(!shader_edit_deleted());
    open_menu();
    CHECK(!enhance_menu_is_open());

    remove(PROFILES "/mine.toml");
    rmdir(PROFILES);
    remove(CACHE "/slangp/mine.slangp");
    remove(CACHE "/slangp/~unsaved.slangp");
    rmdir(CACHE "/slangp");
    rmdir(CACHE);
    s_cache_root = NULL;
    nv2a_present_chain_params_clear();
}

/* D9a: every way out asks while something is unsaved; Go Back has the
 * focus, the opening key is swallowed for 150 ms, and the dialog's
 * buttons and access keys do what they say. */
static void test_dialog(void)
{
    const char *f;
    int sel;
    start_with(ARMED, k_mods);
    open_menu();

    /* Nothing unsaved: each trigger just closes. */
    tap(0x1B);
    CHECK(!enhance_menu_is_open());
    open_menu();
    shift_esc();
    CHECK(!enhance_menu_is_open());
    open_menu();
    pad_b();
    CHECK(!enhance_menu_is_open());
    open_menu();
    pad_tap(XBOX_GAMEPAD_BACK);                 /* View, on its release */
    CHECK(!enhance_menu_is_open());

    /* With a change: each asks, and B, Esc and View are Go Back. */
    open_menu();
    change_scale();
    sel = enhance_menu_selected();
    tap(0x1B);
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() == 2);
    CHECK(strstr(enhance_menu_dialog_text(), "1 setting changed since the last save.") != NULL);
    CHECK(strstr(enhance_menu_dialog_text(), "Render scale") != NULL);
    /* The swallow: a key within 150 ms of the opening does nothing. */
    tap(0x25);
    CHECK(enhance_menu_dialog() == 2);
    advance(160);
    tap(0x25);
    CHECK(enhance_menu_dialog() == 1);
    tap(0x1B);                                  /* Esc: Go Back */
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() < 0);
    CHECK(!strcmp(enhance_menu_page_name(enhance_menu_page()), "Render"));
    CHECK(enhance_menu_selected() == sel);
    CHECK(enhance_menu_unsaved() == 1);

    pad_b();
    CHECK(enhance_menu_dialog() == 2);
    advance(160);
    pad_b();
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() < 0);

    pad_tap(XBOX_GAMEPAD_BACK);
    CHECK(enhance_menu_dialog() == 2);
    advance(160);
    pad_tap(XBOX_GAMEPAD_BACK);
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() < 0);

    shift_esc();
    CHECK(enhance_menu_dialog() == 2);
    advance(160);
    tap('G');
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() < 0);

    /* F10 asks too, and a second F10 is Go Back. */
    open_menu();
    CHECK(enhance_menu_dialog() == 2);
    advance(160);
    open_menu();
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() < 0);

    /* Y (Save) is not a dialog shortcut; Enter on the focus is. */
    open_menu();
    advance(160);
    f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
    advance(16);
    poll0();
    f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 0;
    advance(16);
    poll0();
    CHECK(enhance_menu_dialog() == 2);
    tap(0x0D);
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() < 0);

    /* N: exit without saving, the change reverted. */
    tap(0x1B);
    advance(160);
    tap('N');
    CHECK(!enhance_menu_is_open());
    CHECK(enhance_menu_unsaved() == 0);
    CHECK(!strcmp(row_value("Render scale"), "1"));

    /* S: exit and save. */
    open_menu();
    change_scale();
    tap(0x1B);
    advance(160);
    tap('S');
    CHECK(!enhance_menu_is_open());
    f = read_file("config/config.toml");
    CHECK(strstr(f, "scale = 2") != NULL);

    /* A save that fails keeps the dialog up with the reason. */
    open_menu();
    change_scale();
    remove("config/config.toml");
    MKDIR("config/config.toml");
    tap(0x1B);
    advance(160);
    tap('S');
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() == 2);
    CHECK(strstr(enhance_menu_dialog_text(), "Save failed") != NULL);
    rmdir("config/config.toml");
    /* The failed Exit and Save is inert until the dialog opens again,
     * even with the folder fixed. */
    tap('S');
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() == 2);
    tap(0x25);
    tap(0x0D);
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() == 1);
    CHECK(enhance_menu_unsaved() == 1);
    tap('G');
    CHECK(enhance_menu_dialog() < 0);
    tap(VK_F5);
    CHECK(enhance_menu_unsaved() == 0);
    open_menu();
    CHECK(!enhance_menu_is_open());

    /* Ctrl+S and Cmd+S in the dialog are swallowed: no save, no exit. */
    open_menu();
    change_scale();
    tap(0x1B);
    advance(160);
    xbox_InputKeyEvent(0x11, 1);
    tap('S');
    xbox_InputKeyEvent(0x11, 0);
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() == 2);
    CHECK(enhance_menu_unsaved() == 1);
    xbox_InputShortcutPost('S');
    advance(16);
    poll0();
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() == 2);
    CHECK(enhance_menu_unsaved() == 1);
    tap('N');
    CHECK(!enhance_menu_is_open());

    /* RECOMP_CONFIG=none: Exit and Save is off, the dialog stays. */
    set_env("RECOMP_CONFIG", "none");
    set_env("RECOMP_MENU", "1");
    start_with(ARMED, k_mods);
    open_menu();
    change_scale();
    tap(0x1B);
    advance(160);
    tap('S');
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() == 2);
    tap(0x25);
    tap(0x0D);                                  /* Enter on the dimmed Save */
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() == 1);
    tap('N');
    CHECK(!enhance_menu_is_open());
    set_env("RECOMP_MENU", NULL);
    set_env("RECOMP_CONFIG", NULL);

    /* The deleted-profile line shows only after a delete; covered by the
     * profile tests' delete path through shader_edit_deleted(). */
}

/* The footer's keys: Ctrl+S and the host's Cmd+S save, L3 on its release
 * moves the hints, and the chord that opened the menu fires neither. */
static void test_footer_keys(void)
{
    start_with(ARMED "[input.actions]\n\"menu.open\" = [\"F10\", \"back+left_stick_click@400\"]\n",
               k_mods);
    open_menu();
    change_scale();
    xbox_InputKeyEvent(0x11, 1);
    tap('S');
    xbox_InputKeyEvent(0x11, 0);
    CHECK(enhance_menu_unsaved() == 0);
    CHECK(strstr(enhance_menu_status(), "Saved") != NULL);
    change_scale();
    xbox_InputShortcutPost('S');
    advance(16);
    poll0();
    CHECK(enhance_menu_unsaved() == 0);

    CHECK(enhance_menu_hints_inline() == 0);
    pad_tap(XBOX_GAMEPAD_LEFT_THUMB);
    CHECK(enhance_menu_hints_inline() == 1);
    /* No status until hints draw at the rows (B or C). */
    CHECK(strstr(enhance_menu_status(), "Hints") == NULL);
    pad_tap(XBOX_GAMEPAD_LEFT_THUMB);
    CHECK(enhance_menu_hints_inline() == 0);
    open_menu();
    CHECK(!enhance_menu_is_open());

    /* A Cmd+S while shut is the host's: it never saves at the next open. */
    xbox_InputShortcutPost('S');
    advance(16);
    poll0();

    /* Open on the chord: letting go of View and L3 is neither Cancel nor
     * the hints toggle. */
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB, 1);
    for (int i = 0; i < 30 && !enhance_menu_is_open(); i++) {
        advance(20);
        poll0();
    }
    CHECK(enhance_menu_is_open());
    change_scale();
    advance(16);
    poll0();
    hold_btn(XBOX_GAMEPAD_LEFT_THUMB, 0);
    advance(16);
    poll0();
    hold_btn(XBOX_GAMEPAD_BACK, 0);
    advance(16);
    poll0();
    CHECK(enhance_menu_is_open());
    CHECK(enhance_menu_dialog() < 0);
    CHECK(enhance_menu_hints_inline() == 0);
    CHECK(enhance_menu_unsaved() == 1);   /* no stray Cmd+S saved it */

    /* View and L3 pressed while open, the toggle held: with a change the
     * toggle asks, and letting go is neither Cancel nor the hints. */
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB, 1);
    for (int i = 0; i < 30 && enhance_menu_dialog() < 0; i++) {
        advance(20);
        poll0();
    }
    CHECK(enhance_menu_dialog() == 2);
    advance(200);
    poll0();
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB, 0);
    advance(16);
    poll0();
    advance(16);
    poll0();
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() == 2);
    CHECK(enhance_menu_hints_inline() == 0);
    tap('G');
    tap(VK_F5);
    CHECK(enhance_menu_unsaved() == 0);

    /* The same with nothing unsaved: the toggle closes, the release
     * neither reopens, asks nor toggles the hints. */
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB, 1);
    for (int i = 0; i < 30 && enhance_menu_is_open(); i++) {
        advance(20);
        poll0();
    }
    CHECK(!enhance_menu_is_open());
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_LEFT_THUMB, 0);
    for (int i = 0; i < 4; i++) {
        advance(16);
        poll0();
    }
    CHECK(!enhance_menu_is_open());
    CHECK(enhance_menu_hints_inline() == 0);
    open_menu();
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() < 0);
    CHECK(enhance_menu_hints_inline() == 0);
    open_menu();
    CHECK(!enhance_menu_is_open());
}

/* A.8: every built-in row has its help, and each section its intro. */
static void test_help(void)
{
    static const char *const pages[] = { "Render", "Pack", "Stats", "Controls" };
    start_with(ARMED, k_mods);
    open_menu();
    for (unsigned p = 0; p < sizeof pages / sizeof pages[0]; p++) {
        CHECK(enhance_menu_set_page(pages[p]));
        for (int i = 0; i < enhance_menu_rows(); i++) {
            char l[96];
            if (enhance_menu_row_live(i) < 0)
                continue;
            enhance_menu_row(i, l, sizeof l, NULL, 0, NULL);
            if (!enhance_menu_row_help(i) || !enhance_menu_row_help(i)[0]) {
                fprintf(stderr, "no help: %s/%s\n", pages[p], l);
                CHECK(0);
            }
        }
    }
    for (int p = 0; p < enhance_menu_page_count(); p++)
        CHECK(enhance_menu_page_intro_text(p) && enhance_menu_page_intro_text(p)[0]);
    CHECK(enhance_menu_page_intro("Game", "Options this title adds."));
    CHECK(!enhance_menu_page_intro("Nope", "x"));
    CHECK(!strcmp(enhance_menu_page_intro_text(3), "Options this title adds."));
    open_menu();
    CHECK(!enhance_menu_is_open());
}

int main(int argc, char **argv)
{
    s_pin = argc > 1 && !strcmp(argv[1], "--pin");
    MKDIR("config");
    set_env("RECOMP_CONFIG", NULL);
    set_env("RECOMP_MENU", NULL);
    set_env("RECOMP_TRACE", NULL);
    set_env("RECOMP_PB_BACKEND", NULL);
    shaders_supported();
    shader_files();
    test_start();
    test_navigate();
    test_mods();
    test_mods_desc_toggle();
    test_capture_and_save();
    test_capture_duplicate();
    test_capture_owned();
    test_detail_rows();
    test_detail_wrap();
    test_detail_reset();
    test_detail_save();
    test_detail_conflicts();
    test_controls_groups();
    test_mouse();
    test_snapshot();
    test_shaders();
    test_profiles();
    test_profiles_forced();
    test_profiles_delete_layers();
    test_profiles_early();
    test_profiles_long_error();
    test_pages();
    test_save_deep();   /* after the pins: its Save moves the Debug page's options gen */
    test_save_tables();
    test_stage_row();
    test_shaders_hidden();
    test_badges();
    test_revert();
    test_revert_shaders();
    test_dialog();
    test_footer_keys();
    test_help();
    xbox_mod_shutdown();
    enhance_menu_reset_for_test();
    enhance_actions_reset();
    remove("config/config.toml");
    remove("menu_stderr.txt");
    shader_files_remove();
    printf("enhance_menu: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
