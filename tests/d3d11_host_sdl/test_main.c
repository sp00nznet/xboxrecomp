/* d3d11_host_sdl - the D3D11 backend's Linux host (src/d3d/d3d11_host_sdl.c)
 * against a fake SDL presenter (fb_present_swap.h) and a fake compiler
 * (d3d11_hlsl.h), so it runs on every POSIX host without dxvk-native or
 * vkd3d:
 *
 *   - the clocks are monotonic and agree with each other;
 *   - no presenter window: window_open fails, window is NULL;
 *   - with one: window is the presenter's; a windowed open keeps the
 *     presenter's size (its w x h is the 1x client size), a framed open of
 *     another size under present.surface=internal asks for a resize and a
 *     popup does not; the sizes are the presenter's;
 *   - the overlay's output hold: the popup asks for its size,
 *     the restore for the drawable the save found, once, and a restore
 *     with no save asks for nothing; a menu reopened before the restore
 *     is made still saves the size before the popup;
 *   - the compiler: load and id from the version, a compile's bytes and
 *     messages in blobs, a failure's HRESULT, a cache hit's empty blob;
 *   - the blob, called the way COM's C binding calls it (lpVtbl first:
 *     QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize);
 *   - mkdir, twice.
 * Run under ASan in a debug build to catch a blob leak or a double free. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "d3d11_host.h"
#include "d3d11_hlsl.h"
#include "video/fb_present_swap.h"

static int s_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); s_fail++; } } while (0)

/* ---- the fake presenter ---------------------------------------------- */

static int s_win_obj;
static void *s_win;
static unsigned s_w = 640, s_h = 480, s_dw = 1920, s_dh = 1080;
static unsigned s_req_w, s_req_h;
static int s_reqs;
static int s_internal;

void *xbox_FramebufferWindowSwap(void) { return s_win; }
void xbox_FramebufferWindowSwapSize(unsigned *w, unsigned *h) { *w = s_w; *h = s_h; }
void xbox_FramebufferWindowSwapDisplay(unsigned *w, unsigned *h) { *w = s_dw; *h = s_dh; }
int xbox_FramebufferWindowSwapInternal(void) { return s_internal; }
static int s_pbegin, s_pend;
void xbox_FramebufferWindowSwapPresentBegin(void) { s_pbegin++; }
void xbox_FramebufferWindowSwapPresentEnd(void) { s_pend++; }
static int s_wbegin, s_wend;
void xbox_FramebufferWindowSwapWorkBegin(void) { s_wbegin++; }
void xbox_FramebufferWindowSwapWorkEnd(void) { s_wend++; }
void xbox_FramebufferWindowSwapResize(unsigned w, unsigned h)
{
    s_req_w = w;
    s_req_h = h;
    s_reqs++;
}

/* ---- the fake compiler ------------------------------------------------ */

static const char *s_version = "vkd3d-shader 1.14 (1.14)";

const char *d3d11_hlsl_version(void) { return s_version; }
unsigned d3d11_hlsl_flags(unsigned f) { return f & (1u << 3 | 1u << 4 | 1u << 12); }
void d3d11_hlsl_free(void *p) { free(p); }

/* "fail" in the source fails; "warn" adds messages. The code is the source
 * reversed, so the test can tell bytes were copied. */
int d3d11_hlsl_compile(const void *src, size_t len, const char *name, const char *entry,
                       const char *target, unsigned flags1, unsigned flags2,
                       void **code, size_t *code_len, char **messages)
{
    const char *s = src;
    size_t i;
    *code = NULL;
    *code_len = 0;
    if (messages)
        *messages = NULL;
    if (messages && (strstr(s, "warn") || strstr(s, "fail"))) {
        *messages = malloc(64);
        snprintf(*messages, 64, "%s: %s", name, strstr(s, "fail") ? "error" : "warning");
    }
    if (strstr(s, "fail"))
        return -4;  /* VKD3D_ERROR_INVALID_SHADER */
    *code = malloc(len);
    for (i = 0; i < len; i++)
        ((char *)*code)[i] = s[len - 1 - i];
    *code_len = len;
    return 0;
}

/* ---- the blob as the backend calls it --------------------------------- */

struct vtbl {
    int32_t  (*QueryInterface)(struct ID3D10Blob *, const void *, void **);
    uint32_t (*AddRef)(struct ID3D10Blob *);
    uint32_t (*Release)(struct ID3D10Blob *);
    void    *(*GetBufferPointer)(struct ID3D10Blob *);
    size_t   (*GetBufferSize)(struct ID3D10Blob *);
};
#define VT(b) (*(const struct vtbl *const *)(b))

static void check_clocks(void)
{
    double a = d3d11_host_now_ms();
    uint64_t u = d3d11_host_now_us(), t = d3d11_host_ticks_ms();
    usleep(20000);
    double b = d3d11_host_now_ms();
    uint64_t u2 = d3d11_host_now_us(), t2 = d3d11_host_ticks_ms();
    CHECK(b - a >= 15.0 && b - a < 1000.0, "now_ms moved %.3f ms over 20 ms", b - a);
    CHECK(u2 - u >= 15000 && u2 - u < 1000000, "now_us moved %llu", (unsigned long long)(u2 - u));
    CHECK(t2 - t >= 15 && t2 - t < 1000, "ticks_ms moved %llu", (unsigned long long)(t2 - t));
    CHECK((double)u / 1000.0 - a < 5.0 && a - (double)u / 1000.0 < 5.0, "now_ms and now_us disagree");
}

static void check_window(void)
{
    unsigned w = 0, h = 0;
    int x = -1, y = -1;

    s_win = NULL;
    CHECK(!d3d11_host_window_open(0, 640, 480), "a window with no presenter window");
    CHECK(!d3d11_host_window(), "window without one");

    s_win = &s_win_obj;
    CHECK(d3d11_host_window_open(0, 640, 480), "window_open");
    CHECK(d3d11_host_window() == &s_win_obj, "not the presenter's window");
    CHECK(s_reqs == 0, "a resize for the size it has");
    /* A windowed start: init passes the 1x client size (854x480), the
     * presenter's window is window_scale times that and stays so. */
    CHECK(d3d11_host_window_open(0, 854, 480) && s_reqs == 0, "a windowed open shrank the window");
    s_internal = 1;
    CHECK(d3d11_host_window_open(0, 1280, 960) && s_reqs == 1
          && s_req_w == 1280 && s_req_h == 960, "framed open of another size");
    CHECK(d3d11_host_window_open(1, 1920, 1080) && s_reqs == 1, "a popup asked for a resize");
    d3d11_host_set_client_size(800, 600, 1);
    CHECK(s_reqs == 2 && s_req_w == 800 && s_req_h == 600, "set_client_size");

    d3d11_host_client_size(&w, &h);
    CHECK(w == 640 && h == 480, "client_size %ux%u", w, h);
    d3d11_host_client_rect(&w, &h);
    CHECK(w == 640 && h == 480, "client_rect %ux%u", w, h);
    d3d11_host_screen_size(&w, &h);
    CHECK(w == 1920 && h == 1080, "screen_size %ux%u", w, h);
    d3d11_host_monitor_rect(&x, &y, &w, &h);
    CHECK(x == 0 && y == 0 && w == 1920 && h == 1080, "monitor_rect");

    /* The presenter's: these change nothing here. */
    d3d11_host_swap_chain_made(NULL);
    d3d11_host_ready();
    d3d11_host_pump();
    d3d11_host_title();
    d3d11_host_framed();
    d3d11_host_fullscreen(1, 0, 640, 480, 1920, 1080);
    CHECK(s_reqs == 2, "a no-op asked for a resize");

    /* The Present bracket is the presenter's close handshake. */
    d3d11_host_present_begin();
    d3d11_host_present_end();
    CHECK(s_pbegin == 1 && s_pend == 1, "present bracket %d/%d", s_pbegin, s_pend);
    d3d11_host_work_begin();
    d3d11_host_work_end();
    CHECK(s_wbegin == 1 && s_wend == 1, "work bracket %d/%d", s_wbegin, s_wend);
}

/* The hold under gamescope as output_surface drives it: save the window,
 * pop it up at the output's shape, put the saved size back at the end. The
 * presenter makes the requests later, so the restore never compares. */
static void check_hold(void)
{
    s_win = &s_win_obj;
    s_w = 1536;
    s_h = 960;
    s_reqs = 0;
    d3d11_host_hold_restore(0);
    CHECK(s_reqs == 0, "a restore with no save asked for a resize");

    d3d11_host_hold_save();
    CHECK(s_reqs == 0, "the save asked for a resize");
    d3d11_host_popup(0, 0, 1280, 800);
    CHECK(s_reqs == 1 && s_req_w == 1280 && s_req_h == 800, "popup %ux%u", s_req_w, s_req_h);
    /* Not made yet: the drawable is still the hold's start. */
    d3d11_host_hold_restore(1);
    CHECK(s_reqs == 2 && s_req_w == 1536 && s_req_h == 960, "restore %ux%u", s_req_w, s_req_h);
    d3d11_host_hold_restore(1);
    CHECK(s_reqs == 2, "a second restore asked again");

    /* Made: the drawable is the popup's, the restore still goes back. */
    d3d11_host_hold_save();
    d3d11_host_popup(0, 0, 1280, 800);
    s_w = 1280;
    s_h = 800;
    d3d11_host_hold_restore(0);
    CHECK(s_reqs == 4 && s_req_w == 1536 && s_req_h == 960, "restore after the popup %ux%u",
          s_req_w, s_req_h);

    /* No drawable yet (the presenter has not noted one): nothing to put back. */
    s_w = s_h = 0;
    d3d11_host_hold_save();
    d3d11_host_hold_restore(0);
    CHECK(s_reqs == 4, "a restore to 0x0");
    s_w = 640;
    s_h = 480;
}

/* SDL_SetWindowSize is asynchronous on X11: the menu opened again before
 * the restore is made must still save the size before the popup, or its
 * close leaves the window at the popup's size. The restore stays out until
 * the drawable shows it, a third size comes, a popup replaces it or the
 * wait runs out. */
static void check_hold_reopen(void)
{
    unsigned w = 0, h = 0;
    s_win = &s_win_obj;
    s_w = 1536;
    s_h = 960;
    s_reqs = 0;
    d3d11_host_hold_save();
    d3d11_host_popup(0, 0, 1280, 800);
    s_w = 1280;
    s_h = 800;
    d3d11_host_hold_restore(0);
    CHECK(s_reqs == 2 && s_req_w == 1536 && s_req_h == 960, "restore %ux%u", s_req_w, s_req_h);
    /* Not made yet: the client size is the restore's target, the rect the
     * drawable. */
    d3d11_host_client_size(&w, &h);
    CHECK(w == 1536 && h == 960, "client_size while the restore is out %ux%u", w, h);
    d3d11_host_client_rect(&w, &h);
    CHECK(w == 1280 && h == 800, "client_rect while the restore is out %ux%u", w, h);
    /* Reopened before it was made: the save keeps the target, the popup
     * replaces the restore, and the close goes back to the target. */
    d3d11_host_hold_save();
    d3d11_host_popup(0, 0, 1280, 800);
    d3d11_host_client_size(&w, &h);
    CHECK(w == 1280 && h == 800, "client_size after the popup %ux%u", w, h);
    d3d11_host_hold_restore(0);
    CHECK(s_reqs == 4 && s_req_w == 1536 && s_req_h == 960, "restore after a reopen %ux%u",
          s_req_w, s_req_h);
    /* Made: the drawable again. */
    s_w = 1536;
    s_h = 960;
    d3d11_host_client_size(&w, &h);
    CHECK(w == 1536 && h == 960, "client_size once made %ux%u", w, h);
    s_w = 1280;
    s_h = 800;
    d3d11_host_client_size(&w, &h);
    CHECK(w == 1280 && h == 800, "the restore outlived its ack %ux%u", w, h);

    /* A third size (a fullscreen switch) ends it. */
    s_w = 1536;
    s_h = 960;
    d3d11_host_hold_save();
    d3d11_host_popup(0, 0, 1280, 800);
    s_w = 1280;
    s_h = 800;
    d3d11_host_hold_restore(0);
    s_w = 1920;
    s_h = 1080;
    d3d11_host_client_size(&w, &h);
    CHECK(w == 1920 && h == 1080, "client_size after a third size %ux%u", w, h);

    /* A size the window manager refused: the wait runs out. */
    s_w = 1536;
    s_h = 960;
    d3d11_host_hold_save();
    d3d11_host_popup(0, 0, 1280, 800);
    s_w = 1280;
    s_h = 800;
    d3d11_host_hold_restore(0);
    usleep(700000);
    d3d11_host_client_size(&w, &h);
    CHECK(w == 1280 && h == 800, "the restore never ran out %ux%u", w, h);
    s_w = 640;
    s_h = 480;
}

static void check_compiler(void)
{
    struct ID3D10Blob *code = NULL, *err = NULL;
    char id[128];
    void *iface = (void *)1;
    int32_t hr;

    CHECK(d3d11_host_compiler_load(), "compiler_load");
    CHECK(d3d11_host_compiler_id(id, sizeof id) && !strcmp(id, "vkd3d-shader|vkd3d-shader 1.14 (1.14)"),
          "compiler_id %s", id);

    hr = d3d11_host_compile("abc", 3, "t", "main", "ps_5_0", 1u << 15, 0, &code, &err);
    CHECK(hr == 0 && code && !err, "compile: hr %d", hr);
    if (code) {
        CHECK(VT(code)->GetBufferSize(code) == 3 && !memcmp(VT(code)->GetBufferPointer(code), "cba", 3),
              "the code's bytes");
        CHECK(VT(code)->AddRef(code) == 2, "AddRef");
        CHECK(VT(code)->QueryInterface(code, NULL, &iface) < 0 && !iface, "QueryInterface");
        CHECK(VT(code)->Release(code) == 1, "Release to 1");
        CHECK(VT(code)->Release(code) == 0, "Release to 0");
    }

    code = NULL;
    hr = d3d11_host_compile("warn", 4, "w", "main", "vs_5_0", 0, 0, &code, &err);
    CHECK(hr == 0 && code && err, "warnings: hr %d", hr);
    if (err) {
        const char *m = VT(err)->GetBufferPointer(err);
        CHECK(!strcmp(m, "w: warning") && VT(err)->GetBufferSize(err) == strlen(m) + 1,
              "the messages' blob: %s", m);
        VT(err)->Release(err);
    }
    if (code)
        VT(code)->Release(code);

    code = (struct ID3D10Blob *)1;
    err = NULL;
    hr = d3d11_host_compile("fail", 4, "f", "main", "ps_5_0", 0, 0, &code, &err);
    CHECK(hr < 0 && !code && err, "a failure: hr %d", hr);
    if (err)
        VT(err)->Release(err);
    code = (struct ID3D10Blob *)1;
    hr = d3d11_host_compile("fail", 4, "f", "main", "ps_5_0", 0, 0, &code, NULL);
    CHECK(hr < 0 && !code, "a failure with no messages wanted");

    code = NULL;
    CHECK(d3d11_host_blob(100, &code) && code && VT(code)->GetBufferSize(code) == 100, "an empty blob");
    if (code) {
        memset(VT(code)->GetBufferPointer(code), 0xAB, 100);   /* writable */
        VT(code)->Release(code);
    }
    CHECK(d3d11_host_blob(0, &code) && code && VT(code)->GetBufferSize(code) == 0, "a 0-byte blob");
    if (code)
        VT(code)->Release(code);

    s_version = NULL;
    CHECK(!d3d11_host_compiler_load(), "compiler_load with no compiler");
    CHECK(!d3d11_host_compiler_id(id, sizeof id), "compiler_id with no compiler");
}

static void check_mkdir(void)
{
    char dir[256];
    struct stat st;
    snprintf(dir, sizeof dir, "d3d11_host_sdl_mkdir.%ld", (long)getpid());
    d3d11_host_mkdir(dir);
    d3d11_host_mkdir(dir);   /* one that exists is fine */
    CHECK(stat(dir, &st) == 0 && S_ISDIR(st.st_mode), "mkdir %s", dir);
    rmdir(dir);
}

int main(void)
{
    check_clocks();
    check_window();
    check_hold();
    check_hold_reopen();
    check_compiler();
    check_mkdir();
    printf("d3d11_host_sdl: %s\n", s_fail ? "FAIL" : "ok");
    return s_fail ? 1 : 0;
}
