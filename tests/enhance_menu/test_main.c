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
 *             item on its sub-list; a switch's help on the Mods list and
 *             its Enabled row (test_mods_toggle_help)
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
 *   files     the Files page (D6): Export settings with its unsaved values
 *             marked, a refused name, the replace confirm; Import's preview,
 *             Apply as unsaved changes undone by Exit without saving, a
 *             key with no setting kept through Save, menu.enabled = false
 *             written but not applied, the refusals, a shader profile under
 *             a free name; an unreadable folder, Open exports folder and
 *             Game Mode, RECOMP_CONFIG=none
 *
 * Runs in its working directory, where it writes config/config.toml. A
 * checksum of 0 below means "not pinned yet": the run prints the value.
 */
#include "enhance.h"
#include "enhance_actions.h"
#include "enhance_cfg.h"
#include "enhance_files.h"
#include "enhance_input.h"
#include "enhance_menu.h"
#include "enhance_menu_input.h"
#include "enhance_menu_layout.h"
#include "enhance_osk.h"
#include "enhance_text.h"
#include "file_browser.h"
#include "host_open.h"
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
#include "pointer.h"
#include "recomp_env.h"
#include "shader_param_kinds.h"
#include "shader_profile_edit.h"
#include "text_input.h"

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
/* MENU_REF_OUT=<dir>: test_shots writes the menu, rasterised by the CPU
 * reference over a stand-in frame, as PPM to look at before pinning. */
static const char *s_ref_out;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- stubs for what the runtime would link ---------------------------- */
/* The display probe needs a window system: the Deck shots set
 * the display it would have found. */
static const struct nv2a_ovl_display *s_display;
void enhance_display_start(void)
{
    if (s_display)
        nv2a_overlay_set_display(s_display);
}

static int s_pacing = 1;
int xbox_PacingMode(void);
int xbox_PacingMode(void) { return s_pacing; }
void xbox_PacingSetMode(int mode);
void xbox_PacingSetMode(int mode) { s_pacing = mode; }
/* kernel_ramfs.c's switch, which this test does not link. */
void xbox_RamCacheEnable(int on);
void xbox_RamCacheEnable(int on) { (void)on; }
/* kernel_language.c, which this test does not link: the menu lists the
 * game's languages (s_langs), empty unless a test sets one. */
static const char *const k_no_langs[] = { NULL };
static const char *const *s_langs = k_no_langs;
static char s_lang_set[8];
uint32_t xbox_SetLanguage(const char *code);
uint32_t xbox_SetLanguage(const char *code)
{
    snprintf(s_lang_set, sizeof s_lang_set, "%s", code ? code : "");
    return 1;
}
const char *const *xbox_LanguagesShipped(void);
const char *const *xbox_LanguagesShipped(void) { return s_langs; }
uint32_t xbox_LanguageFromCode(const char *code);
uint32_t xbox_LanguageFromCode(const char *code) { return !strcmp(code, "fr") ? 4 : 1; }
const char *xbox_LanguageName(uint32_t lang);
const char *xbox_LanguageName(uint32_t lang) { return lang == 4 ? "French" : "English"; }
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

/* kernel_path.c and kernel_xbox.c: the running title and its save roots,
 * for the Saves page. s_tid 0: no title yet, as before the XBE loads. */
static uint32_t s_tid;
static const char *s_save_root[2];
static int s_saves_busy;
uint32_t xbox_path_title_id(void);
uint32_t xbox_path_title_id(void) { return s_tid; }
const char *xbox_path_title_name(void);
const char *xbox_path_title_name(void) { return s_tid ? "Test Title" : ""; }
int xbox_path_save_root(int tdata, char *out, size_t n);
int xbox_path_save_root(int tdata, char *out, size_t n)
{
    if (!s_tid || !s_save_root[tdata ? 1 : 0])
        return 0;
    snprintf(out, n, "%s", s_save_root[tdata ? 1 : 0]);
    return 1;
}
int xbox_saves_busy(void);
int xbox_saves_busy(void) { return s_saves_busy; }
/* Each read moves it by s_saves_gen_step: 1 plays a game that saves
 * while the page copies. */
static unsigned s_saves_gen, s_saves_gen_step;
unsigned xbox_saves_gen(void);
unsigned xbox_saves_gen(void)
{
    unsigned g = s_saves_gen;
    s_saves_gen += s_saves_gen_step;
    return g;
}
const char *xbox_saves_keys_id(void);
const char *xbox_saves_keys_id(void) { return "zero"; }
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

/* Esc as the pad's B (D9a): from a section's top level the first goes to
 * the toolbar, and the one there leaves, asking first when unsaved. */
static void esc_leave(void)
{
    if (enhance_menu_focus() != MENU_FOCUS_TOOLBAR)
        tap(0x1B);
    tap(0x1B);
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
    xbox_InputPointerReset();
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
    CHECK(enhance_menu_page_count() == 10);

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
    /* Q11: no wrap. Up from the first row stays; Down past the last goes
     * to the footer, on Cancel with nothing to save, and Up comes back. */
    CHECK(enhance_menu_select_row("Render scale"));
    tap(0x26);
    CHECK(!strcmp(selected_label(), "Render scale"));
    CHECK(enhance_menu_focus() == 0);
    CHECK(enhance_menu_select_row("Overlay fit"));
    tap(0x28);
    CHECK(enhance_menu_focus() == 2 && enhance_menu_footer_selected() == 0);
    CHECK(!strcmp(selected_label(), "Overlay fit"));
    tap(0x26);
    CHECK(enhance_menu_focus() == 0 && enhance_menu_footer_selected() == -1);
    CHECK(!strcmp(selected_label(), "Overlay fit"));

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

    /* Esc is B (D9a): the first goes to the toolbar, the one there asks
     * first. The A held here stepped the filter, so the dialog comes up
     * and N leaves without saving. The title gets nothing while the keys
     * are down, and the pad button held through the close stays the
     * host's to its release. */
    hold_a(1);
    advance(16);
    poll0();
    tap(0x1B);
    CHECK(enhance_menu_is_open() && enhance_menu_focus() == MENU_FOCUS_TOOLBAR);
    CHECK(enhance_menu_dialog() < 0);
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

/* A mod's switch shows its enable setting's help as the tooltip, on the
 * Mods list and on the mod page's Enabled row; a switch with no help (the
 * descriptor mod above) keeps an empty one. */
#define HM_HELP "Diagonals run as fast as straight ahead."
static const struct xbox_mod_setting k_hm_set[] = {
    { "enable", "hmod.enable", NULL, NULL, "Help mod", HM_HELP, NULL,
      XBOX_SET_ENUM, ENHANCE_LIVE, XBOX_SET_ENABLE, 0, 0, 0, 0, k_dm_offon },
};
static const struct xbox_mod_desc k_hm_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc),
    "Help mod", "About the help mod.", NULL, NULL, k_hm_set, 1, NULL, 0
};
static const struct xbox_mod k_hm = { "hmod", dm_init, NULL, &k_hm_desc };
static const struct xbox_mod *const k_hmods[] = { &k_hm, &k_dm, NULL };

static int row_index(const char *label);

static void test_mods_toggle_help(void)
{
    start_with(ARMED, k_hmods);
    open_menu();
    CHECK(enhance_menu_set_page("Mods"));
    CHECK(!strcmp(enhance_menu_row_help(row_index("Help mod")), HM_HELP));
    CHECK(!strcmp(enhance_menu_row_help(row_index("Descriptor mod")), ""));
    CHECK(enhance_menu_select_row("Help mod"));
    tap(0x0D);
    CHECK(row_value("Enabled") != NULL);
    CHECK(!strcmp(enhance_menu_row_help(row_index("Enabled")), HM_HELP));
    tap(0x08);
    CHECK(enhance_menu_select_row("Descriptor mod"));
    tap(0x0D);
    CHECK(row_value("Enabled") != NULL);
    CHECK(!strcmp(enhance_menu_row_help(row_index("Enabled")), ""));
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

/* ---- 6b. the Sticks group: input.pad_square_left / _right --------------- */

static void test_sticks(void)
{
    xbox_pad_setup ps;
    const char *f;
    char l[96], v[128];
    int n, at = -1;

    start_with(ARMED, NULL);
    xbox_InputPadSetup(&ps);
    CHECK(!ps.square_left && !ps.square_right);
    open_menu();
    CHECK(enhance_menu_set_page("Controls"));
    n = enhance_menu_rows();
    for (int i = 0; i < n; i++) {
        enhance_menu_row(i, l, sizeof l, v, sizeof v, NULL);
        if (!strcmp(l, "Sticks"))
            at = i;
    }
    CHECK(at > 0);
    enhance_menu_row(at + 1, l, sizeof l, v, sizeof v, NULL);
    CHECK(!strcmp(l, "Square left stick") && !strcmp(v, "false"));
    enhance_menu_row(at + 2, l, sizeof l, v, sizeof v, NULL);
    CHECK(!strcmp(l, "Square right stick") && !strcmp(v, "false"));

    /* The toggle installs the setup at once, the other stick untouched. */
    CHECK(enhance_menu_select_row("Square left stick"));
    tap(0x27);
    CHECK(!strcmp(row_value("Square left stick"), "true"));
    xbox_InputPadSetup(&ps);
    CHECK(ps.square_left && !ps.square_right);
    {
        const char *keys[4], *vals[4];
        CHECK(enhance_menu_pending(keys, vals, 4) == 1);
        CHECK(!strcmp(keys[0], "input.pad_square_left") && !strcmp(vals[0], "true"));
    }
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "pad_square_left = true") != NULL);
    CHECK(strstr(f, "pad_square_right") == NULL);
    open_menu();

    /* The input layer reads its knobs from the files as loaded, which Save
     * leaves alone: a reload over the re-read file keeps the stick, and so
     * does the next start. */
    capture_begin();
    enhance_cfg_init(".", NULL);
    enhance_input_reload();
    capture_end();
    xbox_InputPadSetup(&ps);
    CHECK(ps.square_left && !ps.square_right);
    start_with(read_file("config/config.toml"), NULL);
    xbox_InputPadSetup(&ps);
    CHECK(ps.square_left && !ps.square_right);
    open_menu();
    CHECK(enhance_menu_set_page("Controls"));
    CHECK(!strcmp(row_value("Square left stick"), "true"));

    /* Back off: the key leaves the file and the setup is the identity. */
    CHECK(enhance_menu_select_row("Square left stick"));
    tap(0x27);
    tap(VK_F5);
    CHECK(strstr(read_file("config/config.toml"), "pad_square_left") == NULL);
    xbox_InputPadSetup(&ps);
    CHECK(xbox_PadSetupIsIdentity(&ps));
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
    CHECK(i >= 0 && strstr(enhance_menu_row_help(i), "A: add a key, a click or a game button") != NULL);
    /* A captured control's row is rebound by capture (test_ctl_capture);
     * Esc leaves it as it was. */
    CHECK(enhance_menu_select_row("Rise"));
    tap(0x0D);
    CHECK(enhance_menu_capturing());
    tap(0x1B);
    CHECK(!enhance_menu_capturing());
    CHECK(row_value("Rise") && !strcmp(s_val, "rt, x, R (keys 3, C)"));

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

/* A mod with two STICK rows, for the swap (D3). */
static const char *const k_sm_move_def[] = { "left_stick", NULL };
static const char *const k_sm_look_def[] = { "right_stick", NULL };
static const char *const k_sm_sink_def[] = { "y", NULL };
static const char *const k_sm_fly_def[] = { "guide+y", NULL };
static const struct xbox_mod_control k_sm_ctl[] = {
    { "fly", "Fly", NULL, NULL, XBOX_CTL_ACTION, k_sm_fly_def, NULL, NULL },
    { "move", "Move", NULL, NULL, XBOX_CTL_STICK, k_sm_move_def, NULL, NULL },
    { "look", "Look", NULL, NULL, XBOX_CTL_STICK, k_sm_look_def, NULL, NULL },
    { "sink", "Sink", NULL, NULL, XBOX_CTL_BUTTON, k_sm_sink_def, NULL, NULL },
};
static const struct xbox_mod_desc k_sm_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc),
    "S mod", "Two sticks.", NULL, NULL, NULL, 0, k_sm_ctl, 4
};
static int sm_init(const struct xbox_mod_api *api) { (void)api; return 0; }
static const struct xbox_mod k_sm = { "smod", sm_init, NULL, &k_sm_desc };
static const struct xbox_mod *const k_smods[] = { &k_sm, NULL };

static void ctl_capture_start(const char *label)
{
    CHECK(enhance_menu_select_row(label));
    tap(0x0D);
    CHECK(enhance_menu_capturing());
}

/* A pad input held for a few polls, then let go. */
static void pad_hold_release(void (*set)(int on))
{
    set(1);
    for (int i = 0; i < 3; i++) {
        advance(16);
        poll0();
    }
    set(0);
    advance(16);
    poll0();
    flip();
}
static void set_y(int on) { f_pad[0].Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = on ? 255 : 0; }
static void set_paddle(int on) { f_extra[0] = on ? XBOX_HOST_PADDLE1 : 0; }
static void set_rx_hard(int on) { f_pad[0].Gamepad.sThumbRX = on ? 30000 : 0; }
static void set_rx_soft(int on) { f_pad[0].Gamepad.sThumbRX = on ? 12000 : 0; }

/* A captured control rebound from the Mods page (mod-settings D3): a key
 * or a game button joins its list, X drops the last, the conflicts are
 * refused with their owner, and Save writes [input.controls]. */
static void test_ctl_capture(void)
{
    char l[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];
    const char *f;
    start_with(ARMED, k_bmods);
    open_menu();
    open_detail("B mod");

    /* A host key joins the list and reads at once. */
    ctl_capture_start("Rise");
    tap('E');
    CHECK(!enhance_menu_capturing());
    CHECK(xbox_mod_control_bindings(1, 1, l, ENHANCE_ACTION_BINDS, NULL) == 4
          && !strcmp(l[3], "E"));
    CHECK(xbox_mod_control_dirty(1, 1) && enhance_menu_unsaved() == 1);
    CHECK(pending_has("input.controls.bmod.rise", "[\"rt\", \"x\", \"R\", \"E\"]"));

    /* Already bound; an action's single key (rule 2); a paddle the game
     * never sees; a stick on a button row. */
    ctl_capture_start("Rise");
    tap('R');
    CHECK(strstr(enhance_menu_status(), "R is bound already") != NULL);
    ctl_capture_start("Rise");
    tap(VK_F7);
    CHECK(strstr(enhance_menu_status(), "F7 refused: F7 is B mod: Toggle flight") != NULL);
    ctl_capture_start("Rise");
    pad_hold_release(set_paddle);
    CHECK(strstr(enhance_menu_status(), "fly controls take buttons the game sees") != NULL);
    ctl_capture_start("Rise");
    pad_hold_release(set_rx_hard);
    CHECK(strstr(enhance_menu_status(), "a stick goes on a stick row") != NULL);
    CHECK(xbox_mod_control_bindings(1, 1, l, ENHANCE_ACTION_BINDS, NULL) == 4);

    /* A game button, by its guest name. */
    ctl_capture_start("Rise");
    pad_hold_release(set_y);
    CHECK(xbox_mod_control_bindings(1, 1, l, ENHANCE_ACTION_BINDS, NULL) == 5
          && !strcmp(l[4], "y"));

    /* X (Delete) drops the last; back at the stock nothing is staged. */
    CHECK(enhance_menu_select_row("Rise"));
    tap(0x2E);
    tap(0x2E);
    CHECK(xbox_mod_control_bindings(1, 1, l, ENHANCE_ACTION_BINDS, NULL) == 3);
    CHECK(!xbox_mod_control_dirty(1, 1) && enhance_menu_unsaved() == 0);
    CHECK(!pending_any("input.controls.bmod.rise"));

    /* A stick row: a push under 0.6 is no capture, a full one is. */
    ctl_capture_start("Move");
    pad_hold_release(set_rx_soft);
    CHECK(enhance_menu_capturing());
    pad_hold_release(set_rx_hard);
    CHECK(!enhance_menu_capturing());
    CHECK(row_value("Move") && !strncmp(s_val, "right_stick", 11));

    /* Save writes the table key; the row stays as saved. */
    ctl_capture_start("Rise");
    tap('E');
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "[input.controls]") != NULL);
    CHECK(strstr(f, "\"bmod.rise\" = [\"rt\", \"x\", \"R\", \"E\"]") != NULL);
    CHECK(strstr(f, "\"bmod.move\" = [\"right_stick\"]") != NULL);
    CHECK(enhance_menu_unsaved() == 0 && !xbox_mod_control_dirty(1, 1));

    /* Reset this mod stages the stock for each. */
    CHECK(enhance_menu_select_row("Reset this mod"));
    tap(0x0D);
    CHECK(xbox_mod_control_is_stock(1, 1) && xbox_mod_control_is_stock(1, 2));
    CHECK(pending_has("input.controls.bmod.rise", NULL));
    open_menu();

    /* The file's binding is the one in effect at the next start. */
    start_with(ARMED "[input.controls]\n\"bmod.rise\" = [\"E\"]\n", k_bmods);
    CHECK(xbox_mod_control_bindings(1, 1, l, ENHANCE_ACTION_BINDS, NULL) == 1
          && !strcmp(l[0], "E"));
    open_menu();
}

/* Two STICK rows trade sticks; a BUTTON row keeps one binding; Revert puts
 * every control back. */
static void test_ctl_swap(void)
{
    char l[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];
    start_with(ARMED, k_smods);
    open_menu();
    open_detail("S mod");
    ctl_capture_start("Move");
    pad_hold_release(set_rx_hard);
    CHECK(xbox_mod_control_bindings(0, 1, l, ENHANCE_ACTION_BINDS, NULL) == 1
          && !strcmp(l[0], "right_stick"));
    CHECK(xbox_mod_control_bindings(0, 2, l, ENHANCE_ACTION_BINDS, NULL) == 1
          && !strcmp(l[0], "left_stick"));
    CHECK(enhance_menu_unsaved() == 2);
    CHECK(enhance_menu_select_row("Sink"));
    tap(0x2E);
    CHECK(strstr(enhance_menu_status(), "Sink keeps one binding") != NULL);
    /* y sits in fly's chord: a button may (rule 3). */
    CHECK(xbox_mod_control_bindings(0, 3, l, ENHANCE_ACTION_BINDS, NULL) == 1);
    enhance_menu_revert();
    CHECK(xbox_mod_control_bindings(0, 1, l, ENHANCE_ACTION_BINDS, NULL) == 1
          && !strcmp(l[0], "left_stick"));
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

/* ---- 7b'. parameter kinds (overlay-revamp D5) ------------------------- */

/* A parameter the sidecar beside the exe declares a toggle: a pill, the
 * help first in the footer, and the store and Save keep the preset's
 * numbers (maskDark is 0..2, so off is 0 and on is 2). The sidecar keys it
 * on the pass file that declares it, which the chain reads from the
 * preset beside the exe. */
static void test_param_kinds(void)
{
    const char *f;
    float v = 0;
    int i;

    MKDIR("shaders");
    write_file("shaders/crt-lottes.slangp", "shaders = 1\nshader0 = \"kinds.slang\"\n");
    write_file("shaders/kinds.slang",
               "#pragma parameter maskDark \"Lottes maskDark\" 0.5 0.0 2.0 0.05\n");
    write_file("shaders/param-kinds.toml",
               "[[param]]\nfile = \"kinds.slang\"\nname = \"maskDark\"\nkind = \"toggle\"\n"
               "choices = [\"Soft\", \"Hard\"]\nhelp = \"How dark the mask gets\"\n");
    shader_param_kinds_builtin_reset(NULL);
    start_with(ARMED "[present]\nshader = \"crt-lottes\"\nshader_lib = \"" FAKE_LRS_PARAMS "\"\n",
               NULL);
    open_menu();
    CHECK(enhance_menu_set_page("Shaders"));
    chain_built();
    flip();
    CHECK(row_value("warpX") && !strcmp(row_value("warpX"), "0.01"));
    CHECK(row_value("Lottes maskDark") && !strcmp(row_value("Lottes maskDark"), "false"));
    CHECK(enhance_menu_select_row("Lottes maskDark"));
    i = enhance_menu_selected();
    CHECK(strstr(enhance_menu_row_help(i), "How dark the mask gets \xC2\xB7 maskDark \xC2\xB7 0..2")
          == enhance_menu_row_help(i));
    CHECK(strstr(enhance_menu_row_help(i), "on: Hard") != NULL);
    tap(0x27);
    CHECK(!strcmp(row_value("Lottes maskDark"), "true"));
    CHECK(nv2a_present_chain_param_get("maskDark", &v) && v == 2.0f);
    tap(VK_F5);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "maskDark = 2.0") != NULL);
    tap(0x0D);
    CHECK(!strcmp(row_value("Lottes maskDark"), "false"));
    CHECK(nv2a_present_chain_param_get("maskDark", &v) && v == 0.0f);
    open_menu();
    nv2a_present_chain_params_clear();
    remove("shaders/crt-lottes.slangp");
    remove("shaders/kinds.slang");
    remove("shaders/param-kinds.toml");
    rmdir("shaders");
    shader_param_kinds_builtin_reset(NULL);
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

static void shift_tap_vk(int vk)
{
    xbox_InputKeyEvent(0x10, 1);
    tap(vk);
    xbox_InputKeyEvent(0x10, 0);
}

/* Text as the host's window types it (SDL_EVENT_TEXT_INPUT, WM_CHAR): into
 * the ring while a field wants text. */
static void type_text(const char *t)
{
    xbox_InputTextPushUtf8(t);
    advance(16);
    poll0();
    flip();
}

static void press_analog(int b)
{
    f_pad[0].Gamepad.bAnalogButtons[b] = 255;
    advance(16);
    poll0();
    flip();
    f_pad[0].Gamepad.bAnalogButtons[b] = 0;
    advance(16);
    poll0();
    flip();
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

    /* Rename on the keyboard (D4): the host's text, in the player's
     * layout, types; Q and E and F5 are swallowed as keys (their text is
     * the ring's); Enter keeps it. No on-screen keys for a keyboard. */
    CHECK(enhance_menu_select_row("Rename"));
    tap(0x0D);
    CHECK(xbox_InputTextActive());
    CHECK(!enhance_menu_osk());
    for (int i = 0; i < 4; i++)
        tap(0x08);
    type_text("My ");
    tap('Q');
    tap('E');
    type_text("qe");
    tap(VK_F5);
    CHECK(!strcmp(enhance_menu_page_name(enhance_menu_page()), "Shaders"));
    CHECK(row_value("Rename") && !strcmp(s_val, "My qe|"));
    tap(0x25);
    tap(0x2E);
    CHECK(row_value("Rename") && !strcmp(s_val, "My q|"));
    tap(0x0D);
    CHECK(!xbox_InputTextActive());
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

    /* Rename on the pad: the on-screen keyboard (D4) opens on q; A
     * types the focused key, the d-pad moves, X deletes; Shift gives one
     * capital; B cancels. */
    CHECK(enhance_menu_select_row("Rename"));
    press_a();
    CHECK(enhance_menu_osk());
    press_a();
    CHECK(row_value("Rename") && !strcmp(s_val, "My qq|"));
    /* The menu's global buttons are the editor's or nothing while it is
     * up: Y does not save, View does not ask to leave, L3 keeps the hints,
     * R3 is nothing; LB and RB move the text cursor, not the section. */
    {
        int pg = enhance_menu_page(), hints = enhance_menu_hints_inline();
        press_analog(XBOX_BUTTON_Y);
        press(XBOX_GAMEPAD_BACK);
        press(XBOX_GAMEPAD_LEFT_THUMB);
        press(XBOX_GAMEPAD_RIGHT_THUMB);
        CHECK(enhance_menu_is_open() && enhance_menu_dialog() < 0 && enhance_menu_osk());
        CHECK(enhance_menu_hints_inline() == hints);
        CHECK(row_value("Rename") && !strcmp(s_val, "My qq|"));
        press_analog(XBOX_BUTTON_WHITE);
        CHECK(row_value("Rename") && !strcmp(s_val, "My q|q"));
        press_analog(XBOX_BUTTON_BLACK);
        CHECK(row_value("Rename") && !strcmp(s_val, "My qq|"));
        CHECK(enhance_menu_page() == pg && enhance_menu_osk());
    }
    press(XBOX_GAMEPAD_DPAD_RIGHT);
    press_a();
    CHECK(row_value("Rename") && !strcmp(s_val, "My qqw|"));
    press_analog(XBOX_BUTTON_X);
    CHECK(row_value("Rename") && !strcmp(s_val, "My qq|"));
    /* Down three rows: from w to the bottom row's Shift, then a capital
     * z. */
    press(XBOX_GAMEPAD_DPAD_DOWN);
    press(XBOX_GAMEPAD_DPAD_DOWN);
    press(XBOX_GAMEPAD_DPAD_DOWN);
    press_a();
    press(XBOX_GAMEPAD_DPAD_UP);
    press_a();
    CHECK(row_value("Rename") && !strcmp(s_val, "My qqZ|"));
    /* Typed text from a keyboard puts the on-screen keys away. */
    type_text("y");
    CHECK(!enhance_menu_osk());
    CHECK(row_value("Rename") && !strcmp(s_val, "My qqZy|"));
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
    /* Render, Shaders, Pack, Game, Mods, Stats, Controls, Debug, Saves,
     * Files. Saves with no title yet: its rows hold no dates. */
    static const uint32_t k_sum[10] = {
        0xD2D6070Cu, 0xF5E5A2AAu, 0xD56F9851u, 0xF9F4DC86u, 0x39FA9CFAu,
        0x0813383Bu, 0x05CA29ACu, 0xCC7407C7u, 0x7B5D7063u, 0x7778B546u };
    /* Files shows Open exports folder only off Game Mode. */
    set_env("RECOMP_HOST_GAMESCOPE", NULL);
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_is_open());
    for (int p = 0; p < enhance_menu_page_count(); p++) {
        const struct nv2a_ovl_list *l;
        uint32_t want = p < 10 ? k_sum[p] : 0;  /* an eleventh page lands unpinned */
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
    type_text("Z");
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
    esc_leave();
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
    esc_leave();
    CHECK(!enhance_menu_is_open());
    open_menu();
    shift_esc();
    CHECK(!enhance_menu_is_open());
    open_menu();
    pad_b();                                    /* to the toolbar (C.2b) */
    CHECK(enhance_menu_is_open() && enhance_menu_focus() == MENU_FOCUS_TOOLBAR);
    pad_b();                                    /* there: the leave */
    CHECK(!enhance_menu_is_open());
    open_menu();
    pad_tap(XBOX_GAMEPAD_BACK);                 /* View, on its release */
    CHECK(!enhance_menu_is_open());

    /* With a change: each asks, and B, Esc and View are Go Back. */
    open_menu();
    change_scale();
    sel = enhance_menu_selected();
    esc_leave();
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
    /* Go Back leaves the focus where the leave came from: the toolbar. */
    CHECK(enhance_menu_focus() == MENU_FOCUS_TOOLBAR);
    tap(0x27);
    CHECK(enhance_menu_focus() == MENU_FOCUS_CONTENT);

    pad_b();
    CHECK(enhance_menu_dialog() < 0 && enhance_menu_focus() == MENU_FOCUS_TOOLBAR);
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
    esc_leave();
    advance(160);
    tap('N');
    CHECK(!enhance_menu_is_open());
    CHECK(enhance_menu_unsaved() == 0);
    CHECK(!strcmp(row_value("Render scale"), "1"));

    /* S: exit and save. */
    open_menu();
    change_scale();
    esc_leave();
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
    esc_leave();
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
    esc_leave();
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
    esc_leave();
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

/* D2: R3 on its release flips the size, written at once and alone; the
 * start reads it back, and RECOMP_CONFIG=none keeps it to the session. */
static void test_size(void)
{
    const struct nv2a_ovl_list *l;
    uint32_t compact;
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_size() == 0);
    enhance_menu_draw(100000);
    l = nv2a_overlay_acquire(NV2A_OVL_MENU);
    compact = l ? l->sum : 0;
    change_scale();
    pad_tap(XBOX_GAMEPAD_RIGHT_THUMB);
    CHECK(enhance_menu_size() == 1);
    CHECK(enhance_menu_unsaved() == 1);   /* the pending change stays pending */
    read_file("config/config.toml");
    CHECK(strstr(s_file, "menu_size = \"expanded\"") != NULL);
    CHECK(strstr(s_file, "enabled = true") != NULL);
    CHECK(strstr(s_file, "render") == NULL);   /* the pending scale is not written */
    enhance_menu_draw(100000);
    l = nv2a_overlay_acquire(NV2A_OVL_MENU);
    CHECK(l && l->sum != compact);
    tap(VK_F5);
    open_menu();
    CHECK(!enhance_menu_is_open());

    start_with(read_file("config/config.toml"), k_mods);
    CHECK(enhance_menu_size() == 1);
    open_menu();
    pad_tap(XBOX_GAMEPAD_RIGHT_THUMB);
    CHECK(enhance_menu_size() == 0);
    read_file("config/config.toml");
    CHECK(strstr(s_file, "menu_size = \"compact\"") != NULL);
    open_menu();

    set_env("RECOMP_CONFIG", "none");
    set_env("RECOMP_MENU", "1");
    start_with(ARMED "[overlay]\nmenu_size = \"expanded\"\n", k_mods);
    open_menu();
    CHECK(enhance_menu_is_open());
    pad_tap(XBOX_GAMEPAD_RIGHT_THUMB);
    CHECK(enhance_menu_size() == 1);
    read_file("config/config.toml");
    CHECK(strstr(s_file, "menu_size = \"expanded\"") != NULL);
    pad_tap(XBOX_GAMEPAD_RIGHT_THUMB);
    CHECK(enhance_menu_size() == 0);
    read_file("config/config.toml");
    CHECK(strstr(s_file, "menu_size = \"expanded\"") != NULL);   /* untouched */
    open_menu();
    set_env("RECOMP_MENU", NULL);
    set_env("RECOMP_CONFIG", NULL);
}

/* R3 is no size toggle when it is part of the menu.open chord, under the
 * leave dialog, or while a capture takes the next input. */
static void test_size_guards(void)
{
    start_with(ARMED "[input.actions]\n\"menu.open\" = [\"F10\", \"back+right_stick_click@400\"]\n",
               k_mods);
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_RIGHT_THUMB, 1);
    for (int i = 0; i < 30 && !enhance_menu_is_open(); i++) {
        advance(20);
        poll0();
    }
    CHECK(enhance_menu_is_open());
    hold_btn(XBOX_GAMEPAD_RIGHT_THUMB, 0);
    advance(16);
    poll0();
    hold_btn(XBOX_GAMEPAD_BACK, 0);
    advance(16);
    poll0();
    CHECK(enhance_menu_is_open() && enhance_menu_size() == 0);
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_RIGHT_THUMB, 1);
    for (int i = 0; i < 30 && enhance_menu_is_open(); i++) {
        advance(20);
        poll0();
    }
    CHECK(!enhance_menu_is_open());
    hold_btn(XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_RIGHT_THUMB, 0);
    for (int i = 0; i < 4; i++) {
        advance(16);
        poll0();
    }
    CHECK(!enhance_menu_is_open() && enhance_menu_size() == 0);

    /* The leave dialog keeps the size. */
    open_menu();
    change_scale();
    open_menu();
    CHECK(enhance_menu_dialog() == 2);
    advance(200);
    poll0();
    pad_tap(XBOX_GAMEPAD_RIGHT_THUMB);
    CHECK(enhance_menu_dialog() == 2 && enhance_menu_size() == 0);
    tap('G');
    tap(VK_F5);
    open_menu();
    CHECK(!enhance_menu_is_open());

    /* A capture takes R3 as its input, never as the toggle. */
    start_with(ARMED, k_bmods);
    open_menu();
    open_detail("B mod");
    ctl_capture_start("Rise");
    pad_tap(XBOX_GAMEPAD_RIGHT_THUMB);
    CHECK(enhance_menu_size() == 0);
    for (int i = 0; i < 4; i++) {
        advance(16);
        poll0();
    }
    CHECK(enhance_menu_size() == 0);
}

/* Q10: the toolbar unfolds while it has the focus, and for PEEK_MS after
 * LB/RB; the hook draws once more when the peek runs out. */
static void test_toolbar_fold(void)
{
    const struct nv2a_ovl_list *l;
    uint32_t folded, open, peek;
    uint64_t gen;
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(!enhance_menu_toolbar_open(f_clock()));
    enhance_menu_draw(f_clock());
    l = nv2a_overlay_acquire(NV2A_OVL_MENU);
    folded = l ? l->sum : 0;
    enhance_menu_set_toolbar_focus(1);
    CHECK(enhance_menu_toolbar_open(f_clock()));
    enhance_menu_draw(f_clock());
    l = nv2a_overlay_acquire(NV2A_OVL_MENU);
    open = l ? l->sum : 0;
    CHECK(open != folded);
    enhance_menu_set_toolbar_focus(0);
    CHECK(!enhance_menu_toolbar_open(f_clock()));

    tap(0x22);                                  /* PageDown: the next section */
    tap(0x21);                                  /* and back */
    CHECK(enhance_menu_toolbar_open(f_clock()));
    l = nv2a_overlay_acquire(NV2A_OVL_MENU);
    peek = l ? l->sum : 0;
    advance(1400);
    poll0();
    CHECK(enhance_menu_toolbar_open(f_clock()));
    advance(200);
    CHECK(!enhance_menu_toolbar_open(f_clock()));
    poll0();
    l = nv2a_overlay_acquire(NV2A_OVL_MENU);
    CHECK(l && l->sum != peek);                 /* the hook folded it */
    gen = l ? l->gen : 0;
    advance(100);
    poll0();
    l = nv2a_overlay_acquire(NV2A_OVL_MENU);
    CHECK(l && l->gen == gen);                  /* once, not every poll */
    open_menu();
    CHECK(!enhance_menu_is_open());
    open_menu();
    CHECK(!enhance_menu_toolbar_open(f_clock()));   /* a fresh open has no peek */
    open_menu();
}

/* The header names the game the window title carries. */
static void test_header_game(void)
{
    const struct nv2a_ovl_list *l;
    uint32_t bare;
    char longname[300];
    start_with(ARMED, k_mods);
    CHECK(!strcmp(nv2a_overlay_game(), ""));
    open_menu();
    enhance_menu_draw(100000);
    l = nv2a_overlay_acquire(NV2A_OVL_MENU);
    bare = l ? l->sum : 0;
    nv2a_overlay_set_game("BLiNX 2: Masters of Time & Space");
    CHECK(!strcmp(nv2a_overlay_game(), "BLiNX 2: Masters of Time & Space"));
    enhance_menu_draw(100000);
    l = nv2a_overlay_acquire(NV2A_OVL_MENU);
    CHECK(l && l->sum != bare);
    /* A long name is cut on a character's edge, never in a sequence. */
    memset(longname, 'a', sizeof longname);
    for (int i = 120; i < 140; i += 2) {
        longname[i] = (char)0xC3;
        longname[i + 1] = (char)0xA9;
    }
    longname[sizeof longname - 1] = 0;
    nv2a_overlay_set_game(longname);
    CHECK(strlen(nv2a_overlay_game()) <= 127);
    CHECK(((unsigned char)nv2a_overlay_game()[strlen(nv2a_overlay_game()) - 1] & 0xC0) != 0xC0);
    nv2a_overlay_set_game(NULL);
    CHECK(!strcmp(nv2a_overlay_game(), ""));
    open_menu();
}

/* The overlay scale and fit rows: live, quarter steps, a step
 * sets the scale at once and Cancel puts it back. */
static void test_ui_scale_row(void)
{
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Render"));
    CHECK(row_value("Overlay scale") != NULL);
    for (int i = 0; i < enhance_menu_rows(); i++) {
        char lb[96];
        enhance_menu_row(i, lb, sizeof lb, NULL, 0, NULL);
        if (!strcmp(lb, "Overlay scale") || !strcmp(lb, "Overlay fit"))
            CHECK(enhance_menu_row_live(i) == ENHANCE_LIVE);
    }
    nv2a_overlay_set_ui_fit(0);
    CHECK(nv2a_ovl_ui_scale(1080) == 1.0f);
    CHECK(enhance_menu_select_row("Overlay scale"));
    press(XBOX_GAMEPAD_DPAD_RIGHT);
    CHECK(!strcmp(row_value("Overlay scale"), "1.25"));
    CHECK(nv2a_ovl_ui_scale(1080) == 1.25f);
    enhance_menu_revert();
    CHECK(nv2a_ovl_ui_scale(1080) == 1.0f);
    nv2a_overlay_set_ui_fit(1);
    open_menu();
}

/* system.language: no Language row for a game that names no
 * languages; with a list, a restart row on the Game page offering those
 * only, its value from the config, and enhance.c hands the kernel the
 * config's code at start. */
static void test_language_row(void)
{
    static const char *const langs[] = { "en", "fr", NULL };
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Game"));
    CHECK(row_value("Language") == NULL);
    open_menu();

    s_langs = langs;
    start_with(ARMED "[system]\nlanguage = \"fr\"\n", k_mods);
    CHECK(!strcmp(s_lang_set, "fr"));
    open_menu();
    CHECK(enhance_menu_set_page("Game"));
    CHECK(row_value("Language") && !strcmp(row_value("Language"), "fr"));
    for (int i = 0; i < enhance_menu_rows(); i++) {
        char lb[96];
        enhance_menu_row(i, lb, sizeof lb, NULL, 0, NULL);
        if (!strcmp(lb, "Language"))
            CHECK(enhance_menu_row_live(i) == ENHANCE_LIVE_RESTART);
    }
    open_menu();
    s_langs = k_no_langs;
    start_with(ARMED, k_mods);
    CHECK(!strcmp(s_lang_set, "en"));
}

/* The menu's sizes and toolbar states as pictures (MENU_REF_OUT only):
 * the review of a layout change, since a sum says only that it moved. */
static void shot(const char *name, int w, int h)
{
    const struct nv2a_ovl_list *l;
    uint32_t *px;
    char path[1024];
    FILE *f;
    nv2a_overlay_set_placement(0, 0, w, h);
    enhance_menu_draw(f_clock());
    l = nv2a_overlay_acquire(NV2A_OVL_MENU);
    CHECK(l != NULL);
    if (!l)
        return;
    px = calloc((size_t)w * h, 4);
    if (!px)
        return;
    nv2a_ovl_ref_draw(px, w, h, l, nv2a_overlay_atlas());
    snprintf(path, sizeof path, "%s/%s.ppm", s_ref_out, name);
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const unsigned char *p = (const unsigned char *)&px[y * w + x];
                /* A stand-in frame: a diagonal gradient, so the backdrop's
                 * dimming and the panel's edges show. */
                int bg[3] = { 40 + 120 * x / w, 70 + 90 * y / h, 150 - 80 * x / w };
                unsigned char o[3];
                for (int k = 0; k < 3; k++)
                    o[k] = (unsigned char)(p[k] + bg[k] * (255 - p[3]) / 255);
                fwrite(o, 1, 3, f);
            }
        fclose(f);
    }
    free(px);
}

static void test_shots(void)
{
    static const struct { const char *tag; int w, h; } k_out[] = {
        { "1080p", 1920, 1080 }, { "720p", 1280, 720 }, { "narrow", 600, 800 } };
    if (!s_ref_out)
        return;
    start_with(ARMED, k_mods);
    nv2a_overlay_set_game("BLiNX 2: Masters of Time & Space");
    open_menu();
    for (unsigned o = 0; o < sizeof k_out / sizeof k_out[0]; o++)
        for (int size = 0; size < 2; size++)
            for (int bar = 0; bar < 2; bar++) {
                char name[64];
                if (enhance_menu_size() != size)
                    pad_tap(XBOX_GAMEPAD_RIGHT_THUMB);
                enhance_menu_set_toolbar_focus(bar);
                snprintf(name, sizeof name, "%s-%s-%s", k_out[o].tag,
                         size ? "expanded" : "compact", bar ? "open" : "folded");
                shot(name, k_out[o].w, k_out[o].h);
            }
    enhance_menu_set_toolbar_focus(0);
    if (enhance_menu_size() != 0)
        pad_tap(XBOX_GAMEPAD_RIGHT_THUMB);
    CHECK(enhance_menu_set_page("Controls"));
    shot("1080p-compact-controls", 1920, 1080);
    CHECK(enhance_menu_set_page("Stats"));
    shot("1080p-compact-stats", 1920, 1080);
    open_menu();
    /* The Deck's panel: baked and drawn at its 800 lines. */
    {
        struct nv2a_ovl_display deck = { 1280, 800, 1.0f, 0, 0 };
        nv2a_overlay_display_mark_deck(&deck, "1");
        s_display = &deck;
        start_with(ARMED, k_mods);
        s_display = NULL;
        nv2a_overlay_set_game("BLiNX 2: Masters of Time & Space");
        open_menu();
        shot("deck-compact-folded", 1280, 800);
        CHECK(enhance_menu_set_page("Controls"));
        shot("deck-compact-controls", 1280, 800);
        open_menu();
        nv2a_overlay_reset_for_test();
    }
    nv2a_overlay_set_game(NULL);
    remove("config/config.toml");
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

/* ---- overlay-revamp C: the pad's regions, the mouse, hints, the OSK ----- */

/* D3's owner table: the global buttons and the section buttons are two
 * sets, and every event sits on the set its kind belongs to, so no section
 * binds a global button. */
static void test_owner_table(void)
{
    int n;
    const struct menu_pad_event *ev = menu_pad_events(&n);
    uint32_t g = menu_owner_mask(MENU_OWN_GLOBAL), sec = menu_owner_mask(MENU_OWN_SECTION);
    CHECK(g && sec && !(g & sec));
    for (int i = 0; i < n; i++) {
        int global = ev[i].ev == ENHANCE_EV_SAVE || ev[i].ev == ENHANCE_EV_CANCEL
                     || ev[i].ev == ENHANCE_EV_HINTS || ev[i].ev == ENHANCE_EV_SIZE
                     || ev[i].ev == ENHANCE_EV_PAGE_PREV || ev[i].ev == ENHANCE_EV_PAGE_NEXT;
        int own = menu_button_owner(ev[i].src);
        if (own != (global ? MENU_OWN_GLOBAL : MENU_OWN_SECTION)) {
            fprintf(stderr, "pad event %d on src %d: owner %d\n", ev[i].ev, ev[i].src, own);
            CHECK(0);
        }
        /* Only a global button acts on its release (a game's chord). */
        CHECK(!ev[i].on_release || own == MENU_OWN_GLOBAL);
        for (int j = 0; j < i; j++)
            CHECK(ev[j].src != ev[i].src);
    }
    CHECK(menu_focus_cycle(MENU_FOCUS_CONTENT, 1) == MENU_FOCUS_FOOTER);
    CHECK(menu_focus_cycle(MENU_FOCUS_FOOTER, 1) == MENU_FOCUS_TOOLBAR);
    CHECK(menu_focus_cycle(MENU_FOCUS_TOOLBAR, -1) == MENU_FOCUS_FOOTER);
    CHECK(menu_focus_cycle(MENU_FOCUS_HEADER, 1) == MENU_FOCUS_FOOTER);
}

/* Q11 and Q12 on the pad: Down past the last row is the footer, B at the
 * top is the toolbar, and a B there leaves. */
static void test_pad_regions(void)
{
    int last;
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Render"));
    for (int i = 0; i < 60 && enhance_menu_focus() == MENU_FOCUS_CONTENT; i++)
        press(XBOX_GAMEPAD_DPAD_DOWN);
    CHECK(enhance_menu_focus() == MENU_FOCUS_FOOTER);
    CHECK(enhance_menu_footer_selected() == MENU_FB_CANCEL);   /* nothing to save */
    last = enhance_menu_selected();
    press(XBOX_GAMEPAD_DPAD_DOWN);                             /* no wrap */
    CHECK(enhance_menu_focus() == MENU_FOCUS_FOOTER);
    press(XBOX_GAMEPAD_DPAD_RIGHT);
    CHECK(enhance_menu_footer_selected() == MENU_FB_SAVE);
    press(XBOX_GAMEPAD_DPAD_LEFT);
    CHECK(enhance_menu_footer_selected() == MENU_FB_CANCEL);
    press(XBOX_GAMEPAD_DPAD_UP);
    CHECK(enhance_menu_focus() == MENU_FOCUS_CONTENT && enhance_menu_selected() == last);
    press(XBOX_GAMEPAD_DPAD_DOWN);
    CHECK(enhance_menu_focus() == MENU_FOCUS_FOOTER);
    pad_b();                                                   /* B: back to the row */
    CHECK(enhance_menu_focus() == MENU_FOCUS_CONTENT && enhance_menu_selected() == last);

    /* With a change the footer opens on Save, and A there saves. */
    change_scale();
    for (int i = 0; i < 60 && enhance_menu_focus() == MENU_FOCUS_CONTENT; i++)
        press(XBOX_GAMEPAD_DPAD_DOWN);
    CHECK(enhance_menu_footer_selected() == MENU_FB_SAVE);
    press_a();
    CHECK(enhance_menu_unsaved() == 0);
    CHECK(enhance_menu_is_open());

    /* The toolbar: B at the top, Up and Down walk the sections, A or Right
     * enters. */
    press(XBOX_GAMEPAD_DPAD_UP);
    CHECK(enhance_menu_set_page("Render"));
    pad_b();
    CHECK(enhance_menu_focus() == MENU_FOCUS_TOOLBAR);
    CHECK(enhance_menu_toolbar_open(f_clock()));
    press(XBOX_GAMEPAD_DPAD_DOWN);
    CHECK(enhance_menu_page() == 1 && enhance_menu_focus() == MENU_FOCUS_TOOLBAR);
    press(XBOX_GAMEPAD_DPAD_UP);
    CHECK(enhance_menu_page() == 0);
    press(XBOX_GAMEPAD_DPAD_RIGHT);
    CHECK(enhance_menu_focus() == MENU_FOCUS_CONTENT);
    pad_b();
    press_a();
    CHECK(enhance_menu_focus() == MENU_FOCUS_CONTENT && enhance_menu_page() == 0);
    /* Tab from the content goes to the footer, Shift+Tab back. */
    tap(0x09);
    CHECK(enhance_menu_focus() == MENU_FOCUS_FOOTER);
    shift_tap_vk(0x09);
    CHECK(enhance_menu_focus() == MENU_FOCUS_CONTENT);
    pad_b();
    pad_b();
    CHECK(!enhance_menu_is_open());

    /* B, B with a change: the dialog, and its B is Go Back to the toolbar. */
    open_menu();
    change_scale();
    pad_b();
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() < 0);
    pad_b();
    CHECK(enhance_menu_dialog() == 2);
    advance(160);
    pad_b();
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() < 0);
    CHECK(enhance_menu_focus() == MENU_FOCUS_TOOLBAR);
    CHECK(enhance_menu_unsaved() == 1);
    enhance_menu_revert();
    pad_b();
    CHECK(!enhance_menu_is_open());
}

/* ---- the pointer ---- */

static const struct menu_layout *lay_now(void)
{
    enhance_menu_draw(f_clock());
    return enhance_menu_last_layout();
}

#define CX(r) ((r).x + (r).w / 2)
#define CY(r) ((r).y + (r).h / 2)

static void ptr_step(void)
{
    advance(16);
    poll0();
    flip();
}

static void ptr_move(int x, int y)
{
    xbox_InputPointerMove(x, y);
    ptr_step();
}

static void ptr_click(unsigned b, int x, int y)
{
    xbox_InputPointerButton(b, 1, x, y);
    ptr_step();
    xbox_InputPointerButton(b, 0, x, y);
    ptr_step();
}

static const struct menu_layout_row *vrow(const struct menu_layout *l, int index)
{
    for (int i = 0; l && i < l->nvrow; i++)
        if (l->row[i].index == index && !l->row[i].clipped)
            return &l->row[i];
    return NULL;
}

/* The first row in view with this control: 0 a pill, 1 a track, 2 arrows. */
static const struct menu_layout_row *vrow_with(const struct menu_layout *l, int what)
{
    for (int i = 0; l && i < l->nvrow; i++) {
        const struct menu_layout_row *r = &l->row[i];
        if (r->clipped)
            continue;
        if ((what == 0 && r->pill.w > 0) || (what == 1 && r->track.w > 0)
            || (what == 2 && r->next.w > 0 && r->prev.w > 0))
            return r;
    }
    return NULL;
}

static const char *row_val(int index)
{
    enhance_menu_row(index, NULL, 0, s_val, sizeof s_val, NULL);
    return s_val;
}

/* A resting mouse never takes the selection: only motion past 2 px at the
 * ui scale does. The cursor hides on a pad or key press. The menu holds
 * the mouse: a mod's divert is off while it is open. */
static void test_pointer_motion(void)
{
    const struct menu_layout *l;
    const struct menu_layout_row *r3;
    int thr, x, y, dx, dy;
    start_with(ARMED, k_mods);
    enhance_input_capture(1);
    CHECK(xbox_InputMouseTakeCounts(&dx, &dy) == 1);
    open_menu();
    CHECK(xbox_InputMouseTakeCounts(&dx, &dy) == 0);   /* held: no divert */
    CHECK(xbox_InputMouseCursorHidden());
    CHECK(enhance_menu_set_page("Render"));
    l = lay_now();
    CHECK(l != NULL);
    r3 = vrow(l, 3);
    CHECK(r3 != NULL);
    if (!l || !r3)
        return;
    thr = menu_pointer_threshold(l->s);
    x = CX(r3->r);
    y = CY(r3->r);
    ptr_move(x, y);
    CHECK(enhance_menu_selected() == 3);
    CHECK(enhance_menu_device() == ENHANCE_DEV_MOUSE);
    CHECK(!xbox_InputMouseCursorHidden());
    tap(0x26);
    CHECK(enhance_menu_selected() == 2);
    CHECK(enhance_menu_device() == ENHANCE_DEV_KEYBOARD);
    CHECK(xbox_InputMouseCursorHidden());
    ptr_step();                                 /* a still mouse */
    CHECK(enhance_menu_selected() == 2);
    ptr_move(x + thr - 1, y);                   /* under the threshold */
    CHECK(enhance_menu_selected() == 2);
    CHECK(enhance_menu_device() == ENHANCE_DEV_KEYBOARD);
    ptr_move(x + thr, y);
    CHECK(enhance_menu_selected() == 3);
    CHECK(enhance_menu_device() == ENHANCE_DEV_MOUSE);
    press(XBOX_GAMEPAD_DPAD_DOWN);
    CHECK(enhance_menu_selected() == 4);
    CHECK(enhance_menu_device() == ENHANCE_DEV_PAD);
    CHECK(xbox_InputMouseCursorHidden());
    open_menu();
    CHECK(!enhance_menu_is_open());
    CHECK(!xbox_InputMouseCursorHidden());
    CHECK(xbox_InputMouseTakeCounts(&dx, &dy) == 1);   /* the divert is back */
    /* A capture given back while the menu holds the mouse stays given back. */
    open_menu();
    enhance_input_capture(0);
    open_menu();
    CHECK(xbox_InputMouseTakeCounts(&dx, &dy) == 0);
    /* And one taken meanwhile waits for the close. */
    open_menu();
    enhance_input_capture(1);
    CHECK(xbox_InputMouseTakeCounts(&dx, &dy) == 0);
    open_menu();
    CHECK(xbox_InputMouseTakeCounts(&dx, &dy) == 1);
    enhance_input_capture(0);
}

/* Binding rows carry a glyph form next to the plain text (F.2). */
static void test_ctl_glyphs(void)
{
    start_with(ARMED, k_bmods);
    open_menu();
    open_detail("B mod");
    CHECK(enhance_menu_row_glyph(row_index("Rise")) != NULL
          && !strcmp(enhance_menu_row_glyph(row_index("Rise")),
                     "{bind:rt}, {bind:x}, {bind:R} (keys {key:3}, {key:C})"));
    CHECK(enhance_menu_row_glyph(row_index("Toggle flight")) != NULL
          && !strcmp(enhance_menu_row_glyph(row_index("Toggle flight")), "{bind:F7}"));
    CHECK(enhance_menu_row_glyph(row_index("Speed")) != NULL
          && !enhance_menu_row_glyph(row_index("Speed"))[0]);
    CHECK(enhance_menu_row_glyph(-1) == NULL);
}

/* Reset <mod> controls: the bindings go back, the settings stay. */
static void test_ctl_reset(void)
{
    start_with(ARMED "[bmod]\nspeed = 2.0\n"
                     "[input.actions]\n\"bmod.toggle\" = [\"F8\"]\n"
                     "[input.controls]\n\"bmod.rise\" = [\"y\"]\n",
               k_bmods);
    open_menu();
    open_detail("B mod");
    CHECK(row_value("Rise") && !strcmp(s_val, "y (key V)"));
    CHECK(enhance_menu_select_row("Reset b mod controls"));
    tap(0x0D);
    CHECK(strstr(enhance_menu_status(), "controls reset to defaults") != NULL);
    CHECK(row_value("Toggle flight") && !strcmp(s_val, "F7"));
    CHECK(row_value("Rise") && !strcmp(s_val, "rt, x, R (keys 3, C)"));
    CHECK(row_value("Speed") && !strcmp(s_val, "2.00"));
    CHECK(pending_has("input.actions.bmod.toggle", NULL));
    CHECK(pending_has("input.controls.bmod.rise", NULL));
    CHECK(!pending_any("bmod.speed"));
    open_menu();
}

/* A mouse button binds a fly button; a stick row and an action refuse
 * it; the click that might have started the capture never binds. */
static void test_ctl_mouse(void)
{
    char l[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];
    start_with(ARMED, k_bmods);
    open_menu();
    open_detail("B mod");
    ptr_move(200, 200);
    xbox_InputMouseEnable(1);

    ctl_capture_start("Rise");
    ptr_click(XBOX_POINTER_RIGHT, 200, 200);
    CHECK(!enhance_menu_capturing());
    CHECK(xbox_mod_control_bindings(1, 1, l, ENHANCE_ACTION_BINDS, NULL) == 4
          && !strcmp(l[3], "mouse2"));
    CHECK(pending_has("input.controls.bmod.rise", "[\"rt\", \"x\", \"R\", \"mouse2\"]"));

    /* Held from before the capture: it waits for the release, then binds
     * nothing until a fresh click. */
    CHECK(enhance_menu_select_row("Rise"));
    xbox_InputPointerButton(XBOX_POINTER_MIDDLE, 1, 200, 200);
    ptr_step();
    tap(0x0D);
    xbox_InputPointerButton(XBOX_POINTER_MIDDLE, 0, 200, 200);
    ptr_step();
    ptr_step();
    CHECK(enhance_menu_capturing());
    tap(0x1B);
    CHECK(!enhance_menu_capturing());
    CHECK(xbox_mod_control_bindings(1, 1, l, ENHANCE_ACTION_BINDS, NULL) == 4);

    ctl_capture_start("Move");
    ptr_click(XBOX_POINTER_LEFT, 200, 200);
    CHECK(!enhance_menu_capturing());
    CHECK(strstr(enhance_menu_status(), "a stick row takes a stick") != NULL);

    ctl_capture_start("Toggle flight");
    ptr_click(XBOX_POINTER_LEFT, 200, 200);
    CHECK(!enhance_menu_capturing());
    CHECK(strstr(enhance_menu_status(), "mouse input") != NULL);
    CHECK(row_value("Toggle flight") && !strcmp(s_val, "F7"));

    /* input.mouse off: the pointer still reaches the menu, but the mouse
     * records no press, so the click is refused rather than bound dead. */
    xbox_InputMouseEnable(0);
    ctl_capture_start("Rise");
    ptr_click(XBOX_POINTER_MIDDLE, 200, 200);
    CHECK(!enhance_menu_capturing());
    CHECK(strstr(enhance_menu_status(), "mouse3 refused: the mouse is off; turn on input.mouse first")
          != NULL);
    CHECK(xbox_mod_control_bindings(1, 1, l, ENHANCE_ACTION_BINDS, NULL) == 4);
    xbox_InputMouseEnable(-1);
    open_menu();
}

static void test_pointer_clicks(void)
{
    const struct menu_layout *l;
    const struct menu_layout_row *r;
    char v0[128];
    int i, page, sel;
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Render"));
    l = lay_now();
    if (!l) {
        CHECK(0);
        return;
    }

    /* The pill, and the label: the row's A. */
    r = vrow_with(l, 0);
    CHECK(r != NULL);
    if (r) {
        i = r->index;
        snprintf(v0, sizeof v0, "%s", row_val(i));
        ptr_click(XBOX_POINTER_LEFT, CX(r->pill), CY(r->pill));
        CHECK(enhance_menu_selected() == i);
        CHECK(strcmp(row_val(i), v0) != 0);
        CHECK(enhance_menu_unsaved() == 1);
        ptr_click(XBOX_POINTER_LEFT, r->r.x + 4, CY(r->r));
        CHECK(!strcmp(row_val(i), v0));
        CHECK(enhance_menu_unsaved() == 0);
    }

    /* The arrows. */
    l = lay_now();
    r = vrow_with(l, 2);
    CHECK(r != NULL);
    if (r) {
        i = r->index;
        snprintf(v0, sizeof v0, "%s", row_val(i));
        ptr_click(XBOX_POINTER_LEFT, CX(r->next), CY(r->next));
        CHECK(strcmp(row_val(i), v0) != 0);
        ptr_click(XBOX_POINTER_LEFT, CX(r->prev), CY(r->prev));
        CHECK(!strcmp(row_val(i), v0));
    }

    /* The slider: a press at a point snaps there, a drag follows, a move
     * with no button held does not. */
    l = lay_now();
    r = vrow_with(l, 1);
    CHECK(r != NULL);
    if (r) {
        int lx = r->track.x, rx = r->track.x + r->track.w - 1, ty = CY(r->track);
        char hi[128], lo[128];
        i = r->index;
        xbox_InputPointerButton(XBOX_POINTER_LEFT, 1, lx, ty);
        ptr_step();
        snprintf(lo, sizeof lo, "%s", row_val(i));
        xbox_InputPointerMove(rx, ty);
        ptr_step();
        snprintf(hi, sizeof hi, "%s", row_val(i));
        CHECK(strcmp(hi, lo) != 0);
        xbox_InputPointerMove(lx, ty);
        ptr_step();
        CHECK(!strcmp(row_val(i), lo));
        xbox_InputPointerMove(rx, ty);
        ptr_step();
        CHECK(!strcmp(row_val(i), hi));
        xbox_InputPointerButton(XBOX_POINTER_LEFT, 0, rx, ty);
        ptr_step();
        ptr_move(lx, ty);
        CHECK(!strcmp(row_val(i), hi));
        enhance_menu_revert();
    }

    /* The header's hints and size buttons. */
    l = lay_now();
    ptr_click(XBOX_POINTER_LEFT, CX(l->hbtn[MENU_HB_HINTS]), CY(l->hbtn[MENU_HB_HINTS]));
    CHECK(enhance_menu_hints_inline());
    l = lay_now();
    ptr_click(XBOX_POINTER_LEFT, CX(l->hbtn[MENU_HB_HINTS]), CY(l->hbtn[MENU_HB_HINTS]));
    CHECK(!enhance_menu_hints_inline());
    l = lay_now();
    ptr_click(XBOX_POINTER_LEFT, CX(l->hbtn[MENU_HB_SIZE]), CY(l->hbtn[MENU_HB_SIZE]));
    CHECK(enhance_menu_size() == 1);
    l = lay_now();
    ptr_click(XBOX_POINTER_LEFT, CX(l->hbtn[MENU_HB_SIZE]), CY(l->hbtn[MENU_HB_SIZE]));
    CHECK(enhance_menu_size() == 0);

    /* A toolbar entry: that section, the content focused. */
    l = lay_now();
    CHECK(l->nsec > 2);
    ptr_click(XBOX_POINTER_LEFT, CX(l->sec[2]), CY(l->sec[2]));
    CHECK(enhance_menu_page() != 0);
    CHECK(enhance_menu_focus() == MENU_FOCUS_CONTENT);
    l = lay_now();
    ptr_click(XBOX_POINTER_LEFT, CX(l->sec[0]), CY(l->sec[0]));
    CHECK(enhance_menu_page() == 0);

    /* Outside the panel: nothing. */
    l = lay_now();
    CHECK(l->panel.x + l->panel.w < l->place.w - 20);
    page = enhance_menu_page();
    sel = enhance_menu_selected();
    ptr_click(XBOX_POINTER_LEFT, l->place.w - 10, l->place.h / 2);
    CHECK(enhance_menu_is_open() && enhance_menu_page() == page && enhance_menu_selected() == sel);
    CHECK(enhance_menu_unsaved() == 0);
    xbox_InputPointerWheel(-3);
    ptr_step();
    CHECK(enhance_menu_is_open() && enhance_menu_selected() == sel);

    /* The footer's Save, then its Cancel leaves. */
    change_scale();
    l = lay_now();
    ptr_click(XBOX_POINTER_LEFT, CX(l->fbtn[MENU_FB_SAVE]), CY(l->fbtn[MENU_FB_SAVE]));
    CHECK(enhance_menu_unsaved() == 0);
    l = lay_now();
    ptr_click(XBOX_POINTER_LEFT, CX(l->fbtn[MENU_FB_CANCEL]), CY(l->fbtn[MENU_FB_CANCEL]));
    CHECK(!enhance_menu_is_open());

    /* The close button asks; the dialog's buttons and the right button. */
    open_menu();
    change_scale();
    l = lay_now();
    ptr_click(XBOX_POINTER_LEFT, CX(l->hbtn[MENU_HB_CLOSE]), CY(l->hbtn[MENU_HB_CLOSE]));
    CHECK(enhance_menu_dialog() >= 0);
    advance(160);
    l = lay_now();
    CHECK(l->dlg_on);
    ptr_click(XBOX_POINTER_LEFT, CX(l->dlg_btn[2]), CY(l->dlg_btn[2]));   /* Go Back */
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() < 0 && enhance_menu_unsaved() == 1);
    l = lay_now();
    ptr_click(XBOX_POINTER_LEFT, CX(l->hbtn[MENU_HB_CLOSE]), CY(l->hbtn[MENU_HB_CLOSE]));
    advance(160);
    ptr_click(XBOX_POINTER_RIGHT, 10, 10);                               /* Go Back */
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() < 0);
    l = lay_now();
    ptr_click(XBOX_POINTER_LEFT, CX(l->hbtn[MENU_HB_CLOSE]), CY(l->hbtn[MENU_HB_CLOSE]));
    advance(160);
    l = lay_now();
    ptr_click(XBOX_POINTER_LEFT, CX(l->dlg_btn[0]), CY(l->dlg_btn[0]));   /* Exit without saving */
    CHECK(!enhance_menu_is_open());

    /* The right button is the pad's B: the toolbar, then out. */
    open_menu();
    l = lay_now();
    r = vrow(l, 1);
    CHECK(r != NULL);
    if (r) {
        ptr_click(XBOX_POINTER_RIGHT, CX(r->r), CY(r->r));
        CHECK(enhance_menu_focus() == MENU_FOCUS_TOOLBAR);
        ptr_click(XBOX_POINTER_RIGHT, CX(r->r), CY(r->r));
    }
    CHECK(!enhance_menu_is_open());
}

/* The wheel scrolls the rows and leaves the selection; the next key brings
 * it back into view. */
static void test_pointer_wheel(void)
{
    const struct menu_layout *l = NULL;
    int found = 0, sel, max;
    start_with(ARMED, k_mods);
    open_menu();
    for (int p = 0; p < enhance_menu_page_count() && !found; p++) {
        if (!enhance_menu_set_page(enhance_menu_page_name(p)))
            continue;
        l = lay_now();
        found = l && l->scrollbar;
    }
    CHECK(found);
    if (!found)
        return;
    l = lay_now();
    ptr_move(l->row[0].r.x + 4, CY(l->row[0].r));     /* over the label */
    sel = enhance_menu_selected();
    max = enhance_menu_rows() - l->vis;
    xbox_InputPointerWheel(-2);                 /* toward the player: down */
    ptr_step();
    l = lay_now();
    CHECK(l->scroll == (max < 2 ? max : 2));
    CHECK(enhance_menu_selected() == sel);
    xbox_InputPointerWheel(5);
    ptr_step();
    CHECK(lay_now()->scroll == 0);
    xbox_InputPointerWheel(-100);
    ptr_step();
    l = lay_now();
    CHECK(l->scroll == max);
    CHECK(enhance_menu_selected() == sel);
    if (sel < l->scroll)
        CHECK(!enhance_menu_tooltip(f_clock() + 10000));   /* no card for a row out of view */
    tap(0x28);                                  /* re-anchors */
    l = lay_now();
    CHECK(enhance_menu_selected() >= l->scroll && enhance_menu_selected() < l->scroll + l->vis);
    open_menu();
}

/* A tap: the press and its release both between two menu steps (a
 * trackpad's tap, a touch, a window pumped at flips) still clicks, where it
 * was pressed. */
static void ptr_tap(unsigned b, int x, int y)
{
    xbox_InputPointerButton(b, 1, x, y);
    xbox_InputPointerButton(b, 0, x, y);
    ptr_step();
}

/* The review's cases: taps between steps, the dialog's swallow for the
 * pointer, a press after a wheel scroll spent on the re-anchor, a drag set
 * once on the item's grid. */
#define AN_ROWS 40
static struct enhance_item s_an[AN_ROWS];
static char s_an_key[AN_ROWS][16], s_an_label[AN_ROWS][16];

static void test_pointer_taps(void)
{
    const struct menu_layout *l;
    const struct menu_layout_row *r;
    char v0[128];
    int i, found = 0, sel;
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Render"));
    l = lay_now();
    r = vrow_with(l, 0);
    CHECK(r != NULL);
    if (r) {
        i = r->index;
        snprintf(v0, sizeof v0, "%s", row_val(i));
        ptr_tap(XBOX_POINTER_LEFT, CX(r->pill), CY(r->pill));
        CHECK(enhance_menu_selected() == i);
        CHECK(strcmp(row_val(i), v0) != 0);
        /* Tapped on the pill, then moved away before the step: the press's
         * place is the one that counts. */
        xbox_InputPointerButton(XBOX_POINTER_LEFT, 1, CX(r->pill), CY(r->pill));
        xbox_InputPointerButton(XBOX_POINTER_LEFT, 0, CX(r->pill), CY(r->pill));
        xbox_InputPointerMove(l->place.w - 10, l->place.h / 2);
        ptr_step();
        CHECK(!strcmp(row_val(i), v0));
        CHECK(enhance_menu_unsaved() == 0);
    }
    /* A right tap is the pad's B. */
    ptr_tap(XBOX_POINTER_RIGHT, 10, 10);
    CHECK(enhance_menu_focus() == MENU_FOCUS_TOOLBAR);
    press_a();
    CHECK(enhance_menu_focus() == MENU_FOCUS_CONTENT);

    /* The slider: a tap at its right end is its top, set once; Right adds
     * nothing past it. */
    l = lay_now();
    r = vrow_with(l, 1);
    CHECK(r != NULL);
    if (r) {
        char hi[128];
        i = r->index;
        ptr_tap(XBOX_POINTER_LEFT, r->track.x + r->track.w - 1, CY(r->track));
        snprintf(hi, sizeof hi, "%s", row_val(i));
        CHECK(enhance_menu_selected() == i);
        tap(0x27);
        CHECK(!strcmp(row_val(i), hi));
        ptr_tap(XBOX_POINTER_LEFT, r->track.x, CY(r->track));
        snprintf(v0, sizeof v0, "%s", row_val(i));
        CHECK(strcmp(v0, hi) != 0);
        tap(0x25);
        CHECK(!strcmp(row_val(i), v0));       /* the bottom */
        enhance_menu_revert();
    }

    /* The dialog's buttons and the right button wait out its swallow, as
     * the pad's do: a double click on the close button does not answer it. */
    change_scale();
    l = lay_now();
    ptr_tap(XBOX_POINTER_LEFT, CX(l->hbtn[MENU_HB_CLOSE]), CY(l->hbtn[MENU_HB_CLOSE]));
    CHECK(enhance_menu_dialog() >= 0);
    l = lay_now();
    CHECK(l->dlg_on);
    ptr_tap(XBOX_POINTER_LEFT, CX(l->dlg_btn[0]), CY(l->dlg_btn[0]));   /* Exit without saving */
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() >= 0);
    ptr_tap(XBOX_POINTER_RIGHT, 10, 10);
    CHECK(enhance_menu_dialog() >= 0);
    advance(160);
    ptr_tap(XBOX_POINTER_RIGHT, 10, 10);                               /* Go Back */
    CHECK(enhance_menu_is_open() && enhance_menu_dialog() < 0 && enhance_menu_unsaved() == 1);
    enhance_menu_revert();

    /* After a wheel scroll, the next press only brings the selection back
     * into view: Right on a row the player has not seen yet sets nothing.
     * A Game section long enough to scroll, every row a choice. */
    start_with(ARMED, NULL);
    for (i = 0; i < AN_ROWS; i++) {
        snprintf(s_an_key[i], sizeof s_an_key[i], "fx.an%d", i);
        snprintf(s_an_label[i], sizeof s_an_label[i], "Anchor %d", i);
        s_an[i] = k_gm_stage;
        s_an[i].key = s_an_key[i];
        s_an[i].label = s_an_label[i];
        CHECK(enhance_menu_add(&s_an[i]));
    }
    open_menu();
    CHECK(enhance_menu_set_page("Game"));
    l = lay_now();
    found = l && l->scrollbar;
    CHECK(found);
    if (found) {
        ptr_move(CX(l->row[0].r), CY(l->row[0].r));          /* the wheel goes where it points */
        CHECK(enhance_menu_select_row("Anchor 0"));
        sel = enhance_menu_selected();
        xbox_InputPointerWheel(-100);
        ptr_step();
        l = lay_now();
        CHECK(sel < l->scroll);
        tap(0x27);
        l = lay_now();
        CHECK(enhance_menu_selected() >= l->scroll && enhance_menu_selected() < l->scroll + l->vis);
        CHECK(enhance_menu_unsaved() == 0);
        tap(0x27);                                                   /* now it sets */
        CHECK(enhance_menu_unsaved() == 1);
        enhance_menu_revert();
    }
    open_menu();
}

/* D11's card: 350 ms after a key or pad selection settles, 500 ms after the
 * pointer rests; none while hints are inline or off, or the toolbar holds
 * the focus. */
static void test_tooltip(void)
{
    const struct menu_layout *l;
    const struct menu_layout_row *r;
    uint64_t t;
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Render"));
    tap(0x28);
    t = f_clock() - 16;                         /* the press's poll */
    CHECK(!enhance_menu_tooltip(t + MENU_TIP_SETTLE_MS - 1));
    CHECK(enhance_menu_tooltip(t + MENU_TIP_SETTLE_MS));
    l = lay_now();
    r = vrow(l, 3);
    CHECK(r != NULL);
    if (r) {
        ptr_move(CX(r->r), CY(r->r));
        t = f_clock();
        CHECK(enhance_menu_selected() == 3);
        CHECK(!enhance_menu_tooltip(t + MENU_TIP_SETTLE_MS));
        CHECK(!enhance_menu_tooltip(t + MENU_TIP_HOVER_MS - 1));
        CHECK(enhance_menu_tooltip(t + MENU_TIP_HOVER_MS));
    }
    pad_b();
    CHECK(!enhance_menu_tooltip(f_clock() + 10000));
    press_a();
    tap(0x70);                                  /* F1: inline */
    CHECK(enhance_menu_hints_inline());
    CHECK(!enhance_menu_tooltip(f_clock() + 10000));
    tap(0x70);
    open_menu();

    start_with(ARMED "[overlay]\nmenu_hints = \"off\"\n", k_mods);
    CHECK(enhance_menu_hints() == MENU_HINTS_OFF);
    open_menu();
    tap(0x28);
    CHECK(!enhance_menu_tooltip(f_clock() + 10000));
    open_menu();
}

/* overlay.menu_hints: F1, L3 on its release and the header's ? flip it, it
 * is written alone at once, and RECOMP_CONFIG=none writes nothing. */
static void test_hints_key(void)
{
    start_with(ARMED, k_mods);
    CHECK(enhance_menu_hints() == MENU_HINTS_TOOLTIP);
    open_menu();
    change_scale();
    tap(0x70);
    CHECK(enhance_menu_hints() == MENU_HINTS_INLINE);
    read_file("config/config.toml");
    CHECK(strstr(s_file, "menu_hints = \"inline\"") != NULL);
    CHECK(strstr(s_file, "render") == NULL);    /* the pending scale stays pending */
    CHECK(enhance_menu_unsaved() == 1);
    hold_btn(XBOX_GAMEPAD_LEFT_THUMB, 1);
    advance(16);
    poll0();
    CHECK(enhance_menu_hints() == MENU_HINTS_INLINE);   /* not on the press */
    hold_btn(XBOX_GAMEPAD_LEFT_THUMB, 0);
    advance(16);
    poll0();
    CHECK(enhance_menu_hints() == MENU_HINTS_TOOLTIP);
    read_file("config/config.toml");
    CHECK(strstr(s_file, "menu_hints = \"tooltip\"") != NULL);
    tap(0x70);
    enhance_menu_revert();
    open_menu();
    start_with(read_file("config/config.toml"), k_mods);
    CHECK(enhance_menu_hints() == MENU_HINTS_INLINE);

    set_env("RECOMP_CONFIG", "none");
    set_env("RECOMP_MENU", "1");
    start_with(ARMED "[overlay]\nmenu_hints = \"off\"\n", k_mods);
    open_menu();
    CHECK(enhance_menu_hints() == MENU_HINTS_TOOLTIP);   /* the file is not read */
    tap(0x70);
    CHECK(enhance_menu_hints() == MENU_HINTS_INLINE);
    read_file("config/config.toml");
    CHECK(strstr(s_file, "menu_hints = \"off\"") != NULL);   /* untouched */
    open_menu();
    set_env("RECOMP_MENU", NULL);
    set_env("RECOMP_CONFIG", NULL);
}

/* The on-screen keyboard's model, geometry and hit test (D4). */
/* Esc and Backspace are B (D9a): from a section's rows they go up to the
 * toolbar; on the toolbar they leave, through the dialog when a change is
 * unsaved. */
static void test_back_keys(void)
{
    static const unsigned keys[] = { 0x1B, 0x08 };
    for (int k = 0; k < 2; k++) {
        start_with(ARMED, k_mods);
        open_menu();
        CHECK(enhance_menu_set_page("Render"));
        CHECK(enhance_menu_select_row("Scaling filter"));
        if (enhance_menu_focus() != MENU_FOCUS_CONTENT)
            tap(0x28);
        CHECK(enhance_menu_focus() == MENU_FOCUS_CONTENT);
        tap(keys[k]);
        CHECK(enhance_menu_is_open() && enhance_menu_focus() == MENU_FOCUS_TOOLBAR);
        tap(keys[k]);
        CHECK(!enhance_menu_is_open());

        open_menu();
        CHECK(enhance_menu_set_page("Render"));
        CHECK(enhance_menu_select_row("Scaling filter"));
        if (enhance_menu_focus() != MENU_FOCUS_CONTENT)
            tap(0x28);
        tap(0x27);
        CHECK(enhance_menu_unsaved() == 1);
        tap(keys[k]);
        CHECK(enhance_menu_focus() == MENU_FOCUS_TOOLBAR && enhance_menu_dialog() < 0);
        tap(keys[k]);
        CHECK(enhance_menu_is_open() && enhance_menu_dialog() == 2);
        advance(160);
        tap('N');
        CHECK(!enhance_menu_is_open());
    }
}

/* The page shown with a row whose control is this (vrow_with's what), or
 * NULL; the layout is the page's. */
static const struct menu_layout_row *page_with(int what, const struct menu_layout **lp)
{
    for (int p = 0; p < enhance_menu_page_count(); p++) {
        const struct menu_layout *l;
        const struct menu_layout_row *r;
        if (!enhance_menu_set_page(enhance_menu_page_name(p)))
            continue;
        l = lay_now();
        r = vrow_with(l, what);
        if (r) {
            *lp = l;
            return r;
        }
    }
    return NULL;
}

/* One notch over a control: the value one step along, as the pad's Right
 * (or Left) gives it, live and unsaved; the rows stay where they are. */
static void wheel_check(int x, int y, int index)
{
    char v0[128], v1[128];
    int scroll = lay_now()->scroll, unsaved = enhance_menu_unsaved();
    snprintf(v0, sizeof v0, "%s", row_val(index));
    ptr_move(x, y);
    xbox_InputPointerWheel(1);
    ptr_step();
    snprintf(v1, sizeof v1, "%s", row_val(index));
    CHECK(strcmp(v1, v0) != 0);
    CHECK(enhance_menu_unsaved() == unsaved + 1);
    CHECK(enhance_menu_selected() == index && enhance_menu_focus() == MENU_FOCUS_CONTENT);
    CHECK(lay_now()->scroll == scroll);
    xbox_InputPointerWheel(-1);
    ptr_step();
    CHECK(!strcmp(row_val(index), v0));
    CHECK(enhance_menu_unsaved() == unsaved);
    /* The pad's step from the same value lands where the notch did. */
    tap(0x27);
    CHECK(!strcmp(row_val(index), v1));
    tap(0x25);
    CHECK(!strcmp(row_val(index), v0));
}

/* The wheel over a slider or a choice steps it, one step a notch; over a
 * label it scrolls (test_pointer_wheel). */
static void test_pointer_wheel_steps(void)
{
    const struct menu_layout *l = NULL;
    const struct menu_layout_row *r;
    int index;
    start_with(ARMED, k_mods);
    open_menu();
    r = page_with(1, &l);
    CHECK(r != NULL);
    if (r) {
        index = r->index;
        wheel_check(CX(r->track), CY(r->track), index);
    }
    r = page_with(2, &l);
    CHECK(r != NULL);
    if (r) {
        index = r->index;
        wheel_check(CX(r->next), CY(r->next), index);
        r = vrow(lay_now(), index);
        CHECK(r != NULL);
        if (r)
            wheel_check((r->prev.x + r->prev.w + r->next.x) / 2, CY(r->next), index);
    }
    open_menu();
}

/* The on-screen keys come up for a rename started by the mouse or a touch
 * while no key has been used; once one has, the host's text is enough. */
static void test_osk_pointer(void)
{
    const struct menu_layout *l;
    const struct menu_layout_row *r;
    int i;
    MKDIR(PROFILES);
    write_file(PROFILES "/mine.toml", "version = 1\nname = \"Mine\"\n"
                                      "stages = [\"user:stage_a\"]\n");
    start_with(CFG_PROF "profile = \"mine\"\n", NULL);
    enhance_menu_open(f_clock());       /* no key: the pad chord or a host call */
    CHECK(enhance_menu_is_open());
    CHECK(enhance_menu_set_page("Shaders"));
    CHECK(row_value("Profile") && !strcmp(s_val, "Mine"));
    i = row_index("Rename");
    CHECK(i >= 0);
    l = lay_now();
    r = vrow(l, i);
    CHECK(r != NULL);
    if (r) {
        ptr_move(CX(r->r) + 3, CY(r->r));
        ptr_click(XBOX_POINTER_LEFT, CX(r->r), CY(r->r));
        CHECK(xbox_InputTextActive() && enhance_menu_osk());
        tap(0x1B);                      /* a key: from here the keyboard is known */
        CHECK(!xbox_InputTextActive());
        ptr_click(XBOX_POINTER_LEFT, CX(r->r), CY(r->r));
        CHECK(xbox_InputTextActive() && !enhance_menu_osk());
        tap(0x1B);
    }
    CHECK(row_value("Rename") && !strcmp(s_val, "Mine"));
    esc_leave();
    CHECK(!enhance_menu_is_open());
    remove(PROFILES "/mine.toml");
    rmdir(PROFILES);
}

/* Under present.surface = internal the pointer is in the window's pixels
 * and the placement in the output's: the menu maps through the view the
 * presenter publishes. Here the output is stretched 2x into the window. */
static void test_pointer_view(void)
{
    const struct menu_layout *l;
    const struct menu_layout_row *r = NULL;
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Render"));
    l = lay_now();
    for (int i = l ? l->nvrow - 1 : -1; i >= 0 && !r; i--) {
        int sel;
        enhance_menu_row(l->row[i].index, NULL, 0, NULL, 0, &sel);
        if (sel && !l->row[i].clipped && l->row[i].index != enhance_menu_selected())
            r = &l->row[i];
    }
    CHECK(r != NULL);
    if (r) {
        int index = r->index, x = r->r.x + 4, y = CY(r->r);
        nv2a_overlay_set_view(0, 0, 3840, 2160, 1920, 1080);
        ptr_move(2 * x + 9, 2 * y);
        ptr_move(2 * x, 2 * y);
        CHECK(enhance_menu_selected() == index);
        /* A click lands on the arrow drawn under it, at its window pixel. */
        r = vrow_with(lay_now(), 2);
        CHECK(r != NULL);
        if (r) {
            char v0[128];
            int ci = r->index, cx = CX(r->next), cy = CY(r->next);
            snprintf(v0, sizeof v0, "%s", row_val(ci));
            ptr_move(2 * cx, 2 * cy);
            ptr_click(XBOX_POINTER_LEFT, 2 * cx, 2 * cy);
            CHECK(enhance_menu_selected() == ci && strcmp(row_val(ci), v0) != 0);
            CHECK(enhance_menu_unsaved() == 1);
        }
        nv2a_overlay_set_view(0, 0, 0, 0, 0, 0);
    }
    esc_leave();
    if (enhance_menu_dialog() >= 0) {
        advance(160);
        tap('N');
    }
    CHECK(!enhance_menu_is_open());
}

/* The Deck's report: render.scale 3 under present.surface =
 * internal is a 1920x1440 back buffer shrunk into a window held to the
 * 1067x800 screen. A touch on the size button, then on the expanded
 * panel's Save at the far bottom-right, lands on what is drawn there. */
#define HELD_X(x) (((x) * 1067 + 960) / 1920)
#define HELD_Y(y) (((y) * 800 + 720) / 1440)
static void test_pointer_held(void)
{
    const struct menu_layout *l;
    start_with(ARMED, k_mods);
    nv2a_overlay_set_placement(0, 0, 1920, 1440);
    nv2a_overlay_set_view(0, 0, 1067, 800, 1920, 1440);
    open_menu();
    CHECK(enhance_menu_set_page("Render"));
    change_scale();
    l = lay_now();
    CHECK(enhance_menu_size() == 0);
    ptr_tap(XBOX_POINTER_LEFT, HELD_X(CX(l->hbtn[MENU_HB_SIZE])), HELD_Y(CY(l->hbtn[MENU_HB_SIZE])));
    CHECK(enhance_menu_size() == 1);
    l = lay_now();
    CHECK(l->fbtn[MENU_FB_SAVE].x + l->fbtn[MENU_FB_SAVE].w > 1920 * 3 / 4 &&
          l->fbtn[MENU_FB_SAVE].y > 1440 * 3 / 4);
    CHECK(enhance_menu_unsaved() == 1);
    ptr_tap(XBOX_POINTER_LEFT, HELD_X(CX(l->fbtn[MENU_FB_SAVE])), HELD_Y(CY(l->fbtn[MENU_FB_SAVE])));
    CHECK(enhance_menu_unsaved() == 0);
    l = lay_now();
    ptr_tap(XBOX_POINTER_LEFT, HELD_X(CX(l->hbtn[MENU_HB_SIZE])), HELD_Y(CY(l->hbtn[MENU_HB_SIZE])));
    CHECK(enhance_menu_size() == 0);
    l = lay_now();
    ptr_tap(XBOX_POINTER_LEFT, HELD_X(CX(l->hbtn[MENU_HB_CLOSE])), HELD_Y(CY(l->hbtn[MENU_HB_CLOSE])));
    CHECK(!enhance_menu_is_open());
    nv2a_overlay_set_view(0, 0, 0, 0, 0, 0);
    nv2a_overlay_set_placement(0, 0, 1920, 1080);
}

/* A slider drag answers a pending ask with no, as the pad's step does. */
static void test_drag_cancels_ask(void)
{
    const struct menu_layout_row *r;
    MKDIR(PROFILES);
    write_file(PROFILES "/mine.toml", "version = 1\nname = \"Mine\"\n"
                                      "stages = [\"user:stage_a\", \"user:stage_b\"]\n");
    start_with(CFG_PROF "profile = \"mine\"\n", NULL);
    open_menu();
    CHECK(enhance_menu_set_page("Shaders"));
    chain_up();
    CHECK(enhance_menu_select_row("1  user:stage_a"));
    tap(0x2E);
    CHECK(enhance_menu_select_row("Remove user:stage_a? A / Enter: yes"));
    r = vrow_with(lay_now(), 1);
    CHECK(r != NULL);
    if (r) {
        int x = r->track.x + 1, y = CY(r->track);
        char v0[128];
        snprintf(v0, sizeof v0, "%s", row_val(r->index));
        ptr_move(x + 5, y);
        ptr_click(XBOX_POINTER_LEFT, x, y);
        CHECK(strcmp(row_val(r->index), v0) != 0);
        CHECK(!enhance_menu_select_row("Remove user:stage_a? A / Enter: yes"));
        CHECK(row_value("1  user:stage_a") && row_value("2  user:stage_b"));
    }
    esc_leave();
    if (enhance_menu_dialog() >= 0) {
        advance(160);
        tap('N');
    }
    CHECK(!enhance_menu_is_open());
    remove(PROFILES "/mine.toml");
    rmdir(PROFILES);
}

static void test_osk(void)
{
    struct osk k;
    struct osk_layout ly;
    char out[8];
    int first, row, col;
    const char *label;
    enhance_osk_open(&k);
    CHECK(k.on && k.row == 1 && k.col == 0);
    CHECK(enhance_osk_press(&k, out, sizeof out) == OSK_ACT_TEXT && !strcmp(out, "q"));
    enhance_osk_move(&k, 0, -1);
    CHECK(k.row == 0 && k.col == 0);
    enhance_osk_move(&k, 0, -1);                /* rows wrap: the bottom row */
    CHECK(k.row == OSK_ROWS - 1 && k.col == 0);
    CHECK(enhance_osk_key(k.row, k.col, 0, &first, &label) == OSK_KEY_SHIFT);
    enhance_osk_move(&k, 1, 0);                 /* a wide key is one stop */
    CHECK(k.col == 2);
    CHECK(enhance_osk_press(&k, out, sizeof out) == OSK_ACT_TEXT && !strcmp(out, " "));
    enhance_osk_move(&k, 1, 0);
    CHECK(k.col == 8);
    CHECK(enhance_osk_press(&k, out, sizeof out) == OSK_ACT_DONE);
    enhance_osk_move(&k, 1, 0);
    CHECK(k.col == 0);
    enhance_osk_move(&k, -1, 0);
    CHECK(k.col == 8);
    enhance_osk_move(&k, -1, 0);
    CHECK(k.col == 2);
    enhance_osk_move(&k, 0, -1);                /* up from Space: its column */
    CHECK(k.row == 3 && k.col == 2);
    enhance_osk_move(&k, 0, 1);
    enhance_osk_move(&k, -1, 0);
    CHECK(enhance_osk_press(&k, out, sizeof out) == OSK_ACT_NONE && k.shift);
    k.row = 2;
    k.col = 0;
    CHECK(enhance_osk_press(&k, out, sizeof out) == OSK_ACT_TEXT && !strcmp(out, "A"));
    CHECK(!k.shift);                            /* one capital */
    CHECK(enhance_osk_press(&k, out, sizeof out) == OSK_ACT_TEXT && !strcmp(out, "a"));
    k.row = 3;
    k.col = 9;
    CHECK(enhance_osk_press(&k, out, sizeof out) == OSK_ACT_BACKSPACE);
    CHECK(enhance_osk_key(3, 9, 0, &first, &label) == OSK_KEY_BACKSPACE && !strcmp(label, "Del"));
    CHECK(enhance_osk_key(OSK_ROWS, 0, 0, &first, &label) < 0 && !label);
    enhance_osk_close(&k);
    CHECK(!k.on);

    /* Under the field when it fits, else above; inside the area. */
    enhance_osk_layout(100, 100, 300, 40, 1920, 1080, 40, 4, &ly);
    CHECK(ly.panel.y == 100 + 40 + 4 && ly.panel.x == 100);
    CHECK(ly.cell[4][2].w == 6 * 40 + 5 * 4 && ly.span[4][2] == 6 && ly.span[4][3] == 0);
    CHECK(enhance_osk_hit(&ly, CX(ly.cell[1][3]), CY(ly.cell[1][3]), &row, &col) && row == 1 && col == 3);
    CHECK(enhance_osk_hit(&ly, ly.cell[4][2].x + ly.cell[4][2].w - 2, CY(ly.cell[4][2]), &row, &col)
          && row == 4 && col == 2);
    CHECK(!enhance_osk_hit(&ly, ly.panel.x - 5, ly.panel.y, &row, &col));
    enhance_osk_layout(1900, 1000, 300, 40, 1920, 1080, 40, 4, &ly);
    CHECK(ly.panel.y + ly.panel.h <= 1000);
    CHECK(ly.panel.x + ly.panel.w <= 1920);

    CHECK(strstr(enhance_osk_hint(1), "Steam + X") != NULL);
    CHECK(strstr(enhance_osk_hint(0), "Steam") == NULL);
}

static void test_input_rules(void)
{
    static const int w[] = { 10, 20, 30 };
    CHECK(menu_hints_fit(w, 3, 5, 70) == 3);
    CHECK(menu_hints_fit(w, 3, 5, 69) == 2);
    CHECK(menu_hints_fit(w, 3, 5, 35) == 2);
    CHECK(menu_hints_fit(w, 3, 5, 34) == 1);
    CHECK(menu_hints_fit(w, 3, 5, 9) == 0);
    CHECK(menu_pointer_threshold(1.0f) == 2);
    CHECK(menu_pointer_threshold(2.0f) == 4);
    CHECK(menu_pointer_threshold(0.2f) == 1);
    CHECK(!menu_pointer_moved(10, 10, 11, 9, 1.0f));
    CHECK(menu_pointer_moved(10, 10, 12, 10, 1.0f));
    CHECK(menu_pointer_moved(10, 10, 10, 8, 1.0f));
    CHECK(!menu_tip_due(ENHANCE_DEV_PAD, 0, 5000));
    CHECK(menu_tip_due(ENHANCE_DEV_PAD, 1000, 1350));
    CHECK(!menu_tip_due(ENHANCE_DEV_MOUSE, 1000, 1350));
    CHECK(menu_tip_due(ENHANCE_DEV_MOUSE, 1000, 1500));
}

/* ---- Files: import and export (design D6) ------------------------------ */

#define EXPORTS "config/exports"

static int status_has(const char *s) { return strstr(enhance_menu_status(), s) != NULL; }

/* The menu open: Files, then a main row's browser, which opens on the
 * Exports folder. */
static void files_open(const char *row)
{
    CHECK(enhance_menu_set_page("Files"));
    CHECK(enhance_menu_select_row(row));
    tap(0x0D);
}

/* Drop what is unsaved and close. */
static void files_quit(void)
{
    enhance_menu_revert();
    if (enhance_menu_is_open())
        open_menu();
    CHECK(!enhance_menu_is_open());
}

static void name_type(const char *t)
{
    for (int i = 0; i < 48; i++)
        tap(0x08);
    type_text(t);
    tap(0x0D);
}

static void files_unit(void)
{
    char kind[EF_KIND_MAX], out[128], err[128];
    int format = -1;
    recomp_cfg *c;
    const char *k[1] = { "render.scale" }, *v[1] = { "2" };
    char *x;
    CHECK(ef_marker("\xEF\xBB\xBF\n# a note\n# xbr-export: config 3\nk = 1\n", kind, sizeof kind,
                    &format) == 1);
    CHECK(!strcmp(kind, "config") && format == 3);
    CHECK(ef_marker("k = 1\n# xbr-export: config 1\n", kind, sizeof kind, &format) == 0);
    CHECK(ef_marker("", kind, sizeof kind, &format) == 0);
    c = recomp_cfg_load_string("a = 1.0\nb = \"q\\\"\"\nc = [1, \"x\", true]\nd = 0.1\ne = 3\n",
                               "t", err, sizeof err);
    CHECK(c != NULL);
    if (c) {
        CHECK(ef_literal(c, "a", out, sizeof out) && !strcmp(out, "1.0"));
        CHECK(ef_literal(c, "b", out, sizeof out) && !strcmp(out, "\"q\\\"\""));
        CHECK(ef_literal(c, "c", out, sizeof out) && !strcmp(out, "[1, \"x\", true]"));
        CHECK(ef_literal(c, "d", out, sizeof out) && !strcmp(out, "0.1"));
        CHECK(ef_literal(c, "e", out, sizeof out) && !strcmp(out, "3"));
        CHECK(!ef_literal(c, "none", out, sizeof out));
        CHECK(!ef_literal(c, "c", out, 6));
        recomp_cfg_free(c);
    }
    /* An export of an export: one marker, CRLF kept. */
    x = ef_export_config("# xbr-export: config 1\r\n[render]\r\nscale = 1\r\n", k, v, 1, k, 1, err,
                         sizeof err);
    CHECK(x != NULL);
    if (x) {
        static const char head[] = "# xbr-export: config 1\r\n"
                                   "# xbr-export: 1 unsaved value included: render.scale\r\n";
        CHECK(!strncmp(x, head, sizeof head - 1));
        CHECK(strstr(x + 1, "# xbr-export: config") == NULL);
        CHECK(strstr(x, "scale = 2\r\n") != NULL);
        free(x);
    }
}

static void test_files_export(void)
{
    const char *f;
    char want[64];
    files_unit();
    start_with(ARMED, k_mods);
    open_menu();
    change_scale();
    files_open("Export settings");
    CHECK(row_value("Exports") != NULL);
    CHECK(row_value("Export here as") && !strncmp(s_val, "settings-", 9)
          && strstr(s_val, ".toml"));
    CHECK(enhance_menu_select_row("Export here as"));
    tap(0x0D);
    CHECK(xbox_InputTextActive());
    /* A refused name keeps the field, with the reason. */
    name_type("a/b");
    CHECK(xbox_InputTextActive());
    CHECK(enhance_menu_status()[0] != 0);
    CHECK(!fb_exists(EXPORTS "/a"));
    tap(0x1B);
    CHECK(!xbox_InputTextActive());
    CHECK(status_has("Export cancelled"));
    CHECK(row_value("Exports") != NULL);   /* Esc took the field only */

    /* Written with the unsaved value, marked, and named in the status. */
    CHECK(enhance_menu_select_row("Export here as"));
    tap(0x0D);
    name_type("mine");
    CHECK(!xbox_InputTextActive());
    CHECK(status_has("Exported to ") && status_has("mine.toml")
          && status_has("1 unsaved value included"));
    f = read_file(EXPORTS "/mine.toml");
    CHECK(!strncmp(f, "# xbr-export: config 1\n", 23));
    CHECK(strstr(f, "# xbr-export: 1 unsaved value included: render.scale\n") != NULL);
    CHECK(strstr(f, "scale = 2") != NULL);
    CHECK(strstr(f, "enabled = true") != NULL);
    CHECK(!fb_exists(EXPORTS "/mine.toml.tmp"));
    CHECK(enhance_menu_unsaved() == 1);           /* an export saves nothing */
    CHECK(strstr(read_file("config/config.toml"), "scale") == NULL);
    CHECK(row_value("mine.toml") != NULL);        /* the listing refreshed */

    /* The same name asks, on No; No keeps the file. */
    change_scale();
    files_open("Export settings");
    CHECK(enhance_menu_select_row("Export here as"));
    tap(0x0D);
    name_type("mine");
    snprintf(want, sizeof want, "%s", selected_label());
    CHECK(!strcmp(want, "No"));
    CHECK(row_value("Yes, replace it") != NULL);
    tap(0x0D);
    CHECK(status_has("Not replaced"));
    CHECK(strstr(read_file(EXPORTS "/mine.toml"), "scale = 2") != NULL);
    /* B is No too; Yes replaces. */
    CHECK(enhance_menu_select_row("Export here as"));
    tap(0x0D);
    name_type("MINE.toml");
    tap(0x1B);
    CHECK(status_has("Not replaced") && row_value("Export here as") != NULL);
    CHECK(enhance_menu_select_row("mine.toml"));   /* A on a file: its name */
    tap(0x0D);
    CHECK(xbox_InputTextActive());
    tap(0x0D);
    CHECK(enhance_menu_select_row("Yes, replace it"));
    tap(0x0D);
    CHECK(status_has("Exported to "));
    CHECK(strstr(read_file(EXPORTS "/mine.toml"), "scale = 3") != NULL);

    /* B goes up to the places, then back to the page. */
    tap(0x1B);
    CHECK(row_value("Exports") && row_value("Home"));
    tap(0x1B);
    CHECK(row_value("Import") && row_value("Export settings"));
    files_quit();

    /* A saved profile exports its file; none is dimmed with the why. */
    start_with(ARMED, k_mods);
    open_menu();
    files_open("Export shader profile");
    CHECK(status_has("Only a saved profile exports"));
    CHECK(row_value("Import") != NULL);
    files_quit();
    shader_files();
    MKDIR(PROFILES);
    write_file(PROFILES "/mine.toml", "version = 1\nname = \"Mine\"\nstages = [\"fsr\"]\n");
    start_with(CFG_PROF "profile = \"mine\"\n", k_mods);
    open_menu();
    files_open("Export shader profile");
    CHECK(row_value("Export here as") && !strncmp(s_val, "mine-", 5));
    CHECK(enhance_menu_select_row("Export here as"));
    tap(0x0D);
    name_type("prof");
    CHECK(!strcmp(read_file(EXPORTS "/prof.toml"),
                  "version = 1\nname = \"Mine\"\nstages = [\"fsr\"]\n"));
    files_quit();
    shader_files_remove();
    remove(PROFILES "/mine.toml");
    remove(EXPORTS "/prof.toml");
    remove(EXPORTS "/mine.toml");
}

static void test_files_import(void)
{
    const char *f;
    MKDIR("config/exports");
    write_file(EXPORTS "/imp.toml", "[render]\nscale = 3\n[future]\nthing = 7\n");

    /* The preview, then Apply: unsaved changes, and Exit without saving
     * drops every one, the kept key with them. */
    start_with(ARMED, k_mods);
    open_menu();
    files_open("Import");
    CHECK(enhance_menu_select_row("imp.toml"));
    tap(0x0D);
    CHECK(row_value("Changes") && !strcmp(s_val, "1"));
    CHECK(row_value("Kept as written") && !strcmp(s_val, "1"));
    CHECK(row_value("future.thing") != NULL);
    CHECK(enhance_menu_unsaved() == 0);           /* nothing before Apply */
    CHECK(enhance_menu_select_row("Apply"));
    tap(0x0D);
    CHECK(status_has("1 change, 1 kept as written"));
    CHECK(enhance_menu_unsaved() == 2);
    CHECK(row_value("Import") != NULL);           /* back on the page */
    CHECK(enhance_menu_set_page("Render"));
    CHECK(!strcmp(row_value("Render scale"), "3"));
    esc_leave();
    advance(160);
    CHECK(enhance_menu_dialog() == 2);
    tap('N');                                     /* Exit without saving */
    CHECK(!enhance_menu_is_open());
    CHECK(enhance_menu_unsaved() == 0);
    CHECK(!strcmp(row_value("Render scale"), "1"));
    CHECK(strstr(read_file("config/config.toml"), "thing") == NULL);

    /* A key with no slot survives Save, as it was read. */
    start_with(ARMED, k_mods);
    open_menu();
    files_open("Import");
    CHECK(enhance_menu_select_row("imp.toml"));
    tap(0x0D);
    CHECK(enhance_menu_select_row("Apply"));
    tap(0x0D);
    tap(VK_F5);
    CHECK(enhance_menu_unsaved() == 0);
    f = read_file("config/config.toml");
    CHECK(strstr(f, "[future]\nthing = 7") != NULL);
    CHECK(strstr(f, "scale = 3") != NULL);
    files_quit();

    /* menu.enabled = false is written on Save, never applied to the menu
     * that is open; the preview says when it applies. Cancel changes
     * nothing first. */
    write_file(EXPORTS "/off.toml", "[menu]\nenabled = false\n[render]\nscale = 2\n");
    start_with(ARMED, k_mods);
    open_menu();
    files_open("Import");
    CHECK(enhance_menu_select_row("off.toml"));
    tap(0x0D);
    CHECK(row_value("menu.enabled = false") && strstr(s_val, "next start"));
    CHECK(enhance_menu_select_row("Cancel"));
    tap(0x0D);
    CHECK(status_has("Import cancelled") && enhance_menu_unsaved() == 0);
    CHECK(enhance_menu_select_row("off.toml"));
    tap(0x0D);
    CHECK(enhance_menu_select_row("Apply"));
    tap(0x0D);
    CHECK(status_has("next start"));
    CHECK(enhance_menu_is_open());
    tap(VK_F5);
    CHECK(enhance_menu_is_open());
    CHECK(strstr(read_file("config/config.toml"), "enabled = false") != NULL);
    tap(VK_F10);
    CHECK(!enhance_menu_is_open());
    open_menu();                                  /* still armed this run */
    CHECK(enhance_menu_is_open());
    open_menu();
    {
        const char *out = start_with(read_file("config/config.toml"), k_mods);
        CHECK(strstr(out, "menu.enabled=off") != NULL);
    }

    /* Refusals leave everything as it was: a file that does not parse, a
     * newer format, an export of something else, a text that is not, a
     * file over the cap. */
    write_file(EXPORTS "/bad.toml", "[render\nscale = 3\n");
    write_file(EXPORTS "/newer.toml", "# xbr-export: config 9\n[render]\nscale = 3\n");
    write_file(EXPORTS "/save.toml", "# xbr-export: savestate 1\n[render]\nscale = 3\n");
    {
        FILE *b = fopen(EXPORTS "/bin.toml", "wb");
        if (b) {
            fwrite("scale = 3\n\0\0", 1, 12, b);
            fclose(b);
        }
        b = fopen(EXPORTS "/big.toml", "wb");
        if (b) {
            for (int i = 0; i < 300 * 1024 / 16; i++)
                fputs("# padding......\n", b);
            fclose(b);
        }
    }
    start_with(ARMED, k_mods);
    open_menu();
    files_open("Import");
    {
        static const struct { const char *file, *says; } k_ref[] = {
            { "bad.toml", "Not imported" },
            { "newer.toml", "newer build" },
            { "save.toml", "not of settings" },
            { "bin.toml", "not a text file" },
            { "big.toml", "over 256 KB" },
        };
        for (size_t i = 0; i < sizeof k_ref / sizeof k_ref[0]; i++) {
            CHECK(enhance_menu_select_row(k_ref[i].file));
            tap(0x0D);
            CHECK(status_has(k_ref[i].says));
            CHECK(row_value("Apply") == NULL && row_value("..") != NULL);
            CHECK(enhance_menu_unsaved() == 0);
        }
    }
    files_quit();

    /* Env-forced and mistyped keys are skipped and named. */
    write_file(EXPORTS "/mixed.toml",
               "[present]\nfilter = \"nearest\"\n[render]\nscale = \"big\"\n");
    set_env("RECOMP_PRESENT_FILTER", "linear");
    start_with(ARMED, k_mods);
    open_menu();
    files_open("Import");
    CHECK(enhance_menu_select_row("mixed.toml"));
    tap(0x0D);
    CHECK(row_value("Changes") && !strcmp(s_val, "0"));
    CHECK(row_value("Set by the environment") && strstr(s_val, "present.filter"));
    CHECK(row_value("Skipped") && strstr(s_val, "render.scale"));
    files_quit();
    set_env("RECOMP_PRESENT_FILTER", NULL);

    /* A shader profile: copied under a name not taken, never replacing. */
    MKDIR(PROFILES);
    write_file(PROFILES "/later.toml", "version = 1\nname = \"Later\"\nstages = [\"fsr\"]\n");
    write_file(EXPORTS "/p.toml", "version = 1\nname = \"Later\"\nstages = [\"fsr\"]\n");
    start_with(ARMED, k_mods);
    open_menu();
    files_open("Import");
    CHECK(enhance_menu_select_row("p.toml"));
    tap(0x0D);
    CHECK(row_value("Name") && !strcmp(s_val, "Later"));
    CHECK(row_value("Stages") && !strcmp(s_val, "1"));
    CHECK(enhance_menu_select_row("Apply"));
    tap(0x0D);
    CHECK(status_has("Imported as later-2.toml"));
    CHECK(!strcmp(read_file(PROFILES "/later-2.toml"), read_file(EXPORTS "/p.toml")));
    CHECK(enhance_menu_unsaved() == 0);
    files_quit();

    /* A name with no ASCII letter or digit has no slug: the file's stem
     * names the copy. */
    write_file(EXPORTS "/neon.toml",
               "version = 1\nname = \"\xE2\x98\x85 \xE2\x98\x85\"\nstages = [\"fsr\"]\n");
    start_with(ARMED, k_mods);
    open_menu();
    files_open("Import");
    CHECK(enhance_menu_select_row("neon.toml"));
    tap(0x0D);
    CHECK(row_value("Saved as") && !strncmp(s_val, "neon.toml", 9));
    CHECK(enhance_menu_select_row("Apply"));
    tap(0x0D);
    CHECK(status_has("Imported as neon.toml"));
    CHECK(!strcmp(read_file(PROFILES "/neon.toml"), read_file(EXPORTS "/neon.toml")));
    files_quit();
    remove(PROFILES "/neon.toml");

    /* The kept keys' cap (64 a session): the same file again takes its
     * keys' places, not 40 more; a file over the cap is refused whole. */
    {
        char body[4096];
        size_t at;
        at = (size_t)snprintf(body, sizeof body, "[render]\nscale = 2\n[keep]\n");
        for (int i = 0; i < 40; i++)
            at += (size_t)snprintf(body + at, sizeof body - at, "a%d = %d\n", i, i);
        write_file(EXPORTS "/k40.toml", body);
        at = (size_t)snprintf(body, sizeof body, "[render]\nscale = 3\n[more]\n");
        for (int i = 0; i < 30; i++)
            at += (size_t)snprintf(body + at, sizeof body - at, "b%d = %d\n", i, i);
        write_file(EXPORTS "/k30.toml", body);
        at = (size_t)snprintf(body, sizeof body, "[many]\n");
        for (int i = 0; i < 65; i++)
            at += (size_t)snprintf(body + at, sizeof body - at, "c%d = %d\n", i, i);
        write_file(EXPORTS "/k65.toml", body);
    }
    start_with(ARMED, k_mods);
    open_menu();
    files_open("Import");
    for (int round = 0; round < 2; round++) {
        CHECK(enhance_menu_select_row("k40.toml"));
        tap(0x0D);
        CHECK(row_value("Kept as written") && !strcmp(s_val, "40"));
        CHECK(enhance_menu_select_row("Apply"));
        tap(0x0D);
        CHECK(status_has("40 kept as written"));
        CHECK(enhance_menu_unsaved() == 41);
        files_open("Import");
    }
    CHECK(enhance_menu_select_row("k30.toml"));
    tap(0x0D);
    CHECK(status_has("70 keys to keep as written, at most 64"));
    CHECK(row_value("Apply") == NULL && enhance_menu_unsaved() == 41);
    CHECK(enhance_menu_set_page("Render"));
    CHECK(!strcmp(row_value("Render scale"), "2"));
    files_quit();
    start_with(ARMED, k_mods);
    open_menu();
    files_open("Import");
    CHECK(enhance_menu_select_row("k65.toml"));
    tap(0x0D);
    CHECK(status_has("65 keys to keep as written, at most 64"));
    CHECK(enhance_menu_unsaved() == 0);
    files_quit();

    remove(PROFILES "/later.toml");
    remove(PROFILES "/later-2.toml");
    {
        static const char *const k_rm[] = { "imp", "off", "bad", "newer", "save", "bin", "big",
                                            "mixed", "p", "neon", "k40", "k30", "k65", NULL };
        char p[96];
        for (int i = 0; k_rm[i]; i++) {
            snprintf(p, sizeof p, EXPORTS "/%s.toml", k_rm[i]);
            remove(p);
        }
    }
    remove("config/config.toml");
}

static void test_files_folders(void)
{
    char path[XBOX_HOST_OPEN_PATH_MAX];
    MKDIR("config/exports");
#ifndef _WIN32
    /* An unreadable folder says so (no system picker before phase I). */
    MKDIR(EXPORTS "/locked");
    chmod(EXPORTS "/locked", 0);
    start_with(ARMED, k_mods);
    open_menu();
    files_open("Import");
    CHECK(enhance_menu_select_row("locked/"));
    tap(0x0D);
    if (geteuid() != 0)
        CHECK(row_value("This folder cannot be read") != NULL);
    tap(0x1B);
    CHECK(row_value("locked/") != NULL);
    files_quit();
    chmod(EXPORTS "/locked", 0755);
    rmdir(EXPORTS "/locked");
#endif

    /* Open exports folder: the host's file manager, asked through the
     * present thread's slot; none in Game Mode. */
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Files"));
    CHECK(enhance_menu_select_row("Open exports folder"));
#ifndef _WIN32
    tap(0x0D);
    CHECK(status_has("Opening ") && status_has("exports"));
    CHECK(xbox_HostOpenTake(path, sizeof path) == 1);
    CHECK(strstr(path, "config/exports") != NULL);
    CHECK(xbox_HostOpenTake(path, sizeof path) == 0);
    {
        /* The default user root is ".": a file URL needs the path made
         * absolute, existing or not, and encoded. */
        char url[3 * XBOX_HOST_OPEN_PATH_MAX + 8], cwd[1024];
        CHECK(getcwd(cwd, sizeof cwd) != NULL);
        CHECK(xbox_HostOpenFileUrl("./" EXPORTS, url, sizeof url));
        CHECK(!strncmp(url, "file:///", 8) && strstr(url, "file://./") == NULL);
        CHECK(strstr(url, "/" EXPORTS) != NULL && strstr(url, "/./") == NULL);
        CHECK(xbox_HostOpenFileUrl("no such/dir", url, sizeof url));
        CHECK(!strncmp(url, "file:///", 8) && strstr(url, "/no%20such/dir") != NULL);
        CHECK(xbox_HostOpenFileUrl("/tmp/a#b", url, sizeof url));
        CHECK(!strcmp(url, "file:///tmp/a%23b"));
        CHECK(!xbox_HostOpenFileUrl("x", url, 4));
    }
#else
    /* Windows opens the folder at once (ShellExecuteW): not in a ctest. */
    (void)path;
#endif
    open_menu();
    set_env("RECOMP_HOST_GAMESCOPE", "1");
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Files"));
    CHECK(row_value("Import") != NULL && row_value("Open exports folder") == NULL);
    open_menu();
    set_env("RECOMP_HOST_GAMESCOPE", NULL);

    /* RECOMP_CONFIG=none: no user folder, nothing to import or export. */
    set_env("RECOMP_CONFIG", "none");
    set_env("RECOMP_MENU", "1");
    start_with(ARMED, k_mods);
    open_menu();
    files_open("Import");
    CHECK(status_has("RECOMP_CONFIG=none"));
    CHECK(row_value("Open exports folder") == NULL);
    files_quit();
    set_env("RECOMP_MENU", NULL);
    set_env("RECOMP_CONFIG", NULL);
    rmdir(EXPORTS);
}

/* ---- Saves: export and import (openspec overlay-saves) ----------------- */

#define SV_U "saves_hdd/UDATA"
#define SV_T "saves_hdd/TDATA"
#define SV_SAVE SV_U "/4d530065/0123456789AB"

/* A SaveMeta.xbx as the dashboard writes it: UTF-16LE, a BOM, Name=. */
static void sv_meta(const char *path, const char *name)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        exit(2);
    fputc(0xFF, f);
    fputc(0xFE, f);
    for (const char *c = "Name="; *c; c++) {
        fputc(*c, f);
        fputc(0, f);
    }
    for (const char *c = name; *c; c++) {
        fputc(*c, f);
        fputc(0, f);
    }
    fclose(f);
}

static void sv_tree(const char *data)
{
    MKDIR("saves_hdd");
    MKDIR(SV_U);
    MKDIR(SV_T);
    MKDIR(SV_U "/4d530065");
    MKDIR(SV_SAVE);
    sv_meta(SV_SAVE "/SaveMeta.xbx", "Slot A");
    write_file(SV_SAVE "/progress.bin", data);
}

static void sv_tree_remove(void)
{
    remove(SV_SAVE "/SaveMeta.xbx");
    remove(SV_SAVE "/progress.bin");
    rmdir(SV_SAVE);
    rmdir(SV_U "/4d530065");
    rmdir(SV_U);
    rmdir(SV_T);
    rmdir("saves_hdd");
}

/* The backup the import wrote, by its name's shape. */
static int sv_backup_emit(void *ctx, const struct fb_entry *e)
{
    char *out = ctx;
    if (!e->is_dir && strstr(e->name, "test-title-saves-backup-") && strstr(e->name, ".zip"))
        snprintf(out, 160, EXPORTS "/save-backups/%s", e->name);
    return 0;
}

static void test_saves(void)
{
    char backup[160] = "";
    s_save_root[0] = SV_U;
    s_save_root[1] = SV_T;
    MKDIR("config/exports");

    /* Before the XBE loads: no title, nothing to move. */
    s_tid = 0;
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Saves"));
    CHECK(row_value("Save folder") && strstr(s_val, "not known"));
    CHECK(enhance_menu_select_row("Export saves"));
    tap(0x0D);
    CHECK(status_has("not known until the game has started"));
    files_quit();

    /* A title with no saves yet. */
    s_tid = 0x4d530065;
    MKDIR("saves_hdd");
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Saves"));
    CHECK(row_value("No saves yet") != NULL);
    CHECK(row_value("Folder") && strstr(s_val, "4d530065"));
    CHECK(enhance_menu_select_row("Export saves"));
    tap(0x0D);
    CHECK(status_has("No saves yet"));
    files_quit();

    /* One save, listed by its SaveMeta name, then exported. */
    sv_tree("first");
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Saves"));
    CHECK(row_value("Slot A") != NULL);
    if (s_ref_out)
        shot("1080p-compact-saves", 1920, 1080);
    CHECK(enhance_menu_select_row("Export saves"));
    tap(0x0D);
    CHECK(row_value("Export here as") && !strncmp(s_val, "test-title-saves-", 17)
          && strstr(s_val, ".zip"));
    /* B: back to Saves, not Files. */
    tap(0x1B);
    CHECK(row_value("Exports") != NULL);
    tap(0x1B);
    CHECK(!strcmp(enhance_menu_page_name(enhance_menu_page()), "Saves"));
    CHECK(row_value("Export saves") != NULL);
    /* A save in progress refuses the export at the write. */
    CHECK(enhance_menu_select_row("Export saves"));
    tap(0x0D);
    s_saves_busy = 1;
    CHECK(enhance_menu_select_row("Export here as"));
    tap(0x0D);
    name_type("sv");
    CHECK(status_has("The game is saving"));
    CHECK(!fb_exists(EXPORTS "/sv.zip"));
    s_saves_busy = 0;
    /* A save that starts while the zip is read: the copy is dropped. */
    s_saves_gen_step = 1;
    CHECK(enhance_menu_select_row("Export here as"));
    tap(0x0D);
    name_type("sv");
    CHECK(status_has("The game is saving"));
    CHECK(!fb_exists(EXPORTS "/sv.zip"));
    s_saves_gen_step = 0;
    CHECK(enhance_menu_select_row("Export here as"));
    tap(0x0D);
    name_type("sv");
    CHECK(status_has("Exported to ") && status_has("sv.zip"));
    CHECK(fb_exists(EXPORTS "/sv.zip"));
    files_quit();

    /* A file that is no saves zip: refused at the pick, nothing changes. */
    write_file(EXPORTS "/bad.zip", "not a zip");
    write_file(SV_SAVE "/progress.bin", "second");
    start_with(ARMED, k_mods);
    open_menu();
    CHECK(enhance_menu_set_page("Saves"));
    CHECK(enhance_menu_select_row("Import saves"));
    tap(0x0D);
    CHECK(row_value("sv.zip") != NULL && row_value("bad.zip") != NULL);
    CHECK(enhance_menu_select_row("bad.zip"));
    tap(0x0D);
    CHECK(status_has("Not imported: "));
    CHECK(row_value("sv.zip") != NULL);

    /* The good one: a preview first, Cancel changes nothing. */
    CHECK(enhance_menu_select_row("sv.zip"));
    tap(0x0D);
    CHECK(row_value("Import and quit") != NULL);
    CHECK(row_value("Game") && !strcmp(s_val, "Test Title"));
    CHECK(row_value("Slot A") != NULL);
    CHECK(row_value("Replaces") && strstr(s_val, "backed up first"));
    if (s_ref_out)
        shot("1080p-compact-saves-import", 1920, 1080);
    CHECK(enhance_menu_select_row("Cancel"));
    tap(0x0D);
    CHECK(status_has("Import cancelled"));
    CHECK(!strcmp(read_file(SV_SAVE "/progress.bin"), "second"));

    /* Busy at "Import and quit": refused, the preview kept. */
    CHECK(enhance_menu_select_row("sv.zip"));
    tap(0x0D);
    s_saves_busy = 1;
    CHECK(enhance_menu_select_row("Import and quit"));
    tap(0x0D);
    CHECK(status_has("The game is saving"));
    CHECK(row_value("Import and quit") != NULL);
    CHECK(!strcmp(read_file(SV_SAVE "/progress.bin"), "second"));
    s_saves_busy = 0;
    /* A save during the backup: refused, nothing written. */
    s_saves_gen_step = 1;
    CHECK(enhance_menu_select_row("Import and quit"));
    tap(0x0D);
    CHECK(status_has("The game is saving"));
    CHECK(row_value("Import and quit") != NULL);
    CHECK(!strcmp(read_file(SV_SAVE "/progress.bin"), "second"));
    CHECK(fb_list_host(EXPORTS "/save-backups", sv_backup_emit, backup, NULL) != 0
          || !backup[0]);
    s_saves_gen_step = 0;
    CHECK(enhance_menu_select_row("Import and quit"));

    /* Imported: the old saves backed up, the zip's in, and with no window
     * to take the quit the page says to quit by hand. */
    tap(0x0D);
    CHECK(status_has("Saves imported") && status_has("Quit the game now"));
    CHECK(!strcmp(read_file(SV_SAVE "/progress.bin"), "first"));
    CHECK(row_value("Saves imported") != NULL);
    CHECK(row_value("Slot A") != NULL);
    CHECK(fb_list_host(EXPORTS "/save-backups", sv_backup_emit, backup, NULL) == 0);
    CHECK(backup[0] != 0);
    files_quit();

    remove(backup);
    rmdir(EXPORTS "/save-backups");
    remove(EXPORTS "/sv.zip");
    remove(EXPORTS "/bad.zip");
    sv_tree_remove();
    rmdir(EXPORTS);
    s_tid = 0;
    s_save_root[0] = s_save_root[1] = NULL;
}

int main(int argc, char **argv)
{
    s_pin = argc > 1 && !strcmp(argv[1], "--pin");
    s_ref_out = getenv("MENU_REF_OUT");
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
    test_mods_toggle_help();
    test_capture_and_save();
    test_capture_duplicate();
    test_capture_owned();
    test_detail_rows();
    test_detail_wrap();
    test_detail_reset();
    test_detail_save();
    test_detail_conflicts();
    test_ctl_capture();
    test_ctl_swap();
    test_controls_groups();
    test_mouse();
    test_sticks();
    test_snapshot();
    test_shaders();
    test_param_kinds();
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
    test_size();
    test_size_guards();
    test_toolbar_fold();
    test_header_game();
    test_ui_scale_row();
    test_language_row();
    test_help();
    test_owner_table();
    test_pad_regions();
    test_pointer_motion();
    test_pointer_clicks();
    test_ctl_glyphs();
    test_ctl_reset();
    test_ctl_mouse();
    test_pointer_wheel();
    test_pointer_taps();
    test_pointer_wheel_steps();
    test_back_keys();
    test_osk_pointer();
    test_pointer_view();
    test_pointer_held();
    test_drag_cancels_ask();
    test_tooltip();
    test_hints_key();
    test_osk();
    test_input_rules();
    test_files_export();
    test_files_import();
    test_files_folders();
    test_saves();
    test_shots();
    xbox_mod_shutdown();
    enhance_menu_reset_for_test();
    enhance_actions_reset();
    remove("config/config.toml");
    remove("menu_stderr.txt");
    shader_files_remove();
    printf("enhance_menu: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
