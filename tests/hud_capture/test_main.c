/* The HUD mapping capture (hud_capture.h) over a synthetic run, driven as
 * mod.c drives it: the keys, the capture's own table file and hooks, then
 * per guest frame the layer's frame and the capture's, with level-1 calls
 * (the wrapper's pre and hud_layer_apply, the mod off) and level-0 calls
 * (a draw entry's pre over a fake stack) in between. The walker's side of
 * the picture requests is played by hand: frame 2's flip is taken and
 * written, frame 3's passes without a take. Then capture.jsonl is read
 * back line by line (openspec change hud-anchors, 2.1, design A15). */
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#define DUP _dup
#define DUP2 _dup2
#define CLOSE _close
#define OPEN_TRUNC(p) _open(p, _O_WRONLY | _O_CREAT | _O_TRUNC, 0644)
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define DUP dup
#define DUP2 dup2
#define CLOSE close
#define OPEN_TRUNC(p) open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644)
#endif

#include "hud_anchor.h"
#include "hud_capture.h"
#include "nv2a_backend_common.h"
#include "recomp_env.h"
#include "nv2a_pb_state.h"

/* nv2a_pb_exec.c's present pick, which an enhance build's batch filter
 * reads; never called here. */
static struct nv2a_pb_present s_present = { 0x00C00000u, 640, 480, 2560, 0 };
const struct nv2a_pb_present *nv2a_pb_present_state(void) { return &s_present; }

static int s_checks, s_failed;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        s_checks++;                                                                                \
        if (!(x)) {                                                                                \
            s_failed++;                                                                            \
            printf("FAIL line %d: %s\n", __LINE__, #x);                                            \
        }                                                                                          \
    } while (0)

static uint8_t g_mem[1u << 16];
static uintptr_t g_base;

#define DIR "hud_capture_out"
#define TABLE "hud_capture_table.toml"

/* stderr into a file and back, for the capture's lines. */
static int s_saved_fd = -1;
static char s_err[16384];
static void err_begin(void)
{
    int fd;
    fflush(stderr);
    s_saved_fd = DUP(2);
    fd = OPEN_TRUNC("hud_capture_stderr.txt");
    if (s_saved_fd < 0 || fd < 0) {
        fprintf(stderr, "cannot capture stderr\n");
        exit(2);
    }
    DUP2(fd, 2);
    CLOSE(fd);
}
static const char *err_end(void)
{
    FILE *f;
    size_t n = 0;
    fflush(stderr);
    DUP2(s_saved_fd, 2);
    CLOSE(s_saved_fd);
    f = fopen("hud_capture_stderr.txt", "rb");
    if (f) {
        n = fread(s_err, 1, sizeof s_err - 1, f);
        fclose(f);
    }
    s_err[n] = 0;
    return s_err;
}

/* The hook service: what the capture hooked, with its user pointer. */
static struct {
    uint32_t va;
    xbox_mod_hook_fn pre, post;
    void *user;
} s_hooks[16];
static int s_nhooks;

static int fake_hook(uint32_t va, xbox_mod_hook_fn pre, xbox_mod_hook_fn post, uint32_t flags,
                     void *user)
{
    (void)flags;
    if (s_nhooks >= 16)
        return 0;
    s_hooks[s_nhooks].va = va;
    s_hooks[s_nhooks].pre = pre;
    s_hooks[s_nhooks].post = post;
    s_hooks[s_nhooks].user = user;
    s_nhooks++;
    return 1;
}

static int hook_at(uint32_t va)
{
    for (int i = 0; i < s_nhooks; i++)
        if (s_hooks[i].va == va)
            return i;
    return -1;
}

/* The kernel's side: the walker always idle, the title's code at
 * 0x1000..0x3000. */
static int s_idle_calls;
static int fake_idle(void)
{
    s_idle_calls++;
    return 1;
}
static void fake_range(uint32_t *lo, uint32_t *hi)
{
    *lo = 0x1000;
    *hi = 0x3000;
}
static const struct nv2a_cap_kernel k_kernel = { fake_idle, fake_range };

static void wr32(uint32_t va, uint32_t v) { memcpy(g_mem + va, &v, 4); }
static void wrf(uint32_t va, float v) { memcpy(g_mem + va, &v, 4); }
static float rdf(uint32_t va)
{
    float v;
    memcpy(&v, g_mem + va, 4);
    return v;
}

/* A quad of (x, y, z, rhw) vertices, 16 bytes each. */
static void quad(uint32_t va, float x0, float y0, float x1, float y1)
{
    const float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
    for (uint32_t k = 0; k < 4; k++) {
        wrf(va + k * 16u, xs[k]);
        wrf(va + k * 16u + 4u, ys[k]);
        wrf(va + k * 16u + 8u, 0.5f);
        wrf(va + k * 16u + 12u, 1.0f);
    }
}

static struct xbox_mod_call at(uint32_t esp, uint32_t ret)
{
    struct xbox_mod_call c;
    memset(&c, 0, sizeof c);
    c.esp = esp;
    wr32(esp, ret);
    return c;
}

#define VTX1 0x4000u
#define VTX2 0x5000u

static int apply(uint32_t ret, uint32_t queue, uint64_t tex)
{
    struct xbox_mod_call c = at(0x6000, ret);
    struct xbox_hud_call d = { VTX1, 4, 0, queue, tex };
    return hud_layer_apply(&c, &d);
}

static void set_aspect(unsigned w, unsigned h)
{
    struct nv2a_host_opts o;
    memset(&o, 0, sizeof o);
    o.render_scale = 1;
    o.aspect_w = w;
    o.aspect_h = h;
    nv2a_host_opts_set(&o);
}

/* One xbox_mod_frame of mod.c with no HUD mod: the layer's frame, then
 * the capture's. */
static void tick(void)
{
    CHECK(hud_layer_frame(0) == 0);
    CHECK(!hud_layer_active());
    hud_capture_frame();
}

static const char k_table[] =
    "version = 1\n[canvas]\nw = 640\nh = 480\norigin = \"top-left\"\n"
    "view = [0, 0, 640, 480]\n"
    "[layout]\nstride = 16\nx_off = 0\nu_off = 8\nprim = 4\n"
    "[[wrapper]]\nva = 0x1000\nname = \"text\"\n"
    "[[group]]\nname = \"score\"\nkind = \"left\"\n"
    "[[group]]\nname = \"timer\"\nkind = \"right\"\n"
    "[[site]]\nret_va = 0x2000\nelement = \"score\"\ngroup = \"score\"\n"
    "[[site]]\nret_va = 0x2010\nelement = \"timer\"\ngroup = \"timer\"\n"
    "[[site]]\nret_va = 0x2060\nelement = \"gauge\"\nkind = \"proportional\"\n"
    /* The name holds a quote and a backslash: the JSON escapes both. */
    "[[capture.draws]]\nfn = 0x1800\nname = \"q\\\"u\\\\ad\"\n"
    "vtx = \"stack:0\"\ncount = \"stack:4\"\n"
    "stride = 16\nprim = 4\n"
    "[[capture.draws]]\nsig = \"55 8B EC\"\nname = \"by sig\"\n"
    "[[capture.draws]]\nfn = \"0x1900\"\nvtx = \"esi\"\ncount = 4\n"
    "[[capture.draws]]\nfn = 0x1A00\nvtx = \"ecx\"\n";

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("cannot write %s\n", path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

static char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *t;
    long len;
    size_t n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    t = calloc(1, (size_t)(len > 0 ? len : 0) + 1u);
    n = t ? fread(t, 1, (size_t)(len > 0 ? len : 0), f) : 0;
    if (t)
        t[n] = 0;
    fclose(f);
    return t;
}

/* The nth line (0-based) of text, copied out; "" past the end. */
static const char *line_at(const char *t, int n)
{
    static char b[4096];
    const char *e;
    while (n-- > 0 && t && (t = strchr(t, '\n')))
        t++;
    if (!t || !*t)
        return "";
    e = strchr(t, '\n');
    if (!e)
        e = t + strlen(t);
    snprintf(b, sizeof b, "%.*s", (int)(e - t), t);
    return b;
}

/* A strict JSON value (RFC 8259) at *p, which moves past it: enough to
 * say a capture line parses (no inf, nan or bare words). */
static int json_value(const char **p);
static void json_ws(const char **p)
{
    while (**p == ' ' || **p == '\t')
        (*p)++;
}
static int json_string(const char **p)
{
    if (*(*p)++ != '"')
        return 0;
    while (**p && **p != '"') {
        if ((unsigned char)**p < 0x20)
            return 0;
        if (**p == '\\') {
            (*p)++;
            if (!strchr("\"\\/bfnrtu", **p) || !**p)
                return 0;
        }
        (*p)++;
    }
    return *(*p)++ == '"';
}
static int json_number(const char **p)
{
    const char *s = *p;
    if (**p == '-')
        (*p)++;
    if (**p == '0')
        (*p)++;
    else if (**p >= '1' && **p <= '9')
        while (isdigit((unsigned char)**p))
            (*p)++;
    else
        return 0;
    if (**p == '.') {
        (*p)++;
        if (!isdigit((unsigned char)**p))
            return 0;
        while (isdigit((unsigned char)**p))
            (*p)++;
    }
    if (**p == 'e' || **p == 'E') {
        (*p)++;
        if (**p == '+' || **p == '-')
            (*p)++;
        if (!isdigit((unsigned char)**p))
            return 0;
        while (isdigit((unsigned char)**p))
            (*p)++;
    }
    return *p > s;
}
static int json_list(const char **p, char close, int obj)
{
    (*p)++;
    json_ws(p);
    if (**p == close) {
        (*p)++;
        return 1;
    }
    for (;;) {
        json_ws(p);
        if (obj) {
            if (!json_string(p))
                return 0;
            json_ws(p);
            if (*(*p)++ != ':')
                return 0;
        }
        if (!json_value(p))
            return 0;
        json_ws(p);
        if (**p == close) {
            (*p)++;
            return 1;
        }
        if (*(*p)++ != ',')
            return 0;
    }
}
static int json_value(const char **p)
{
    json_ws(p);
    switch (**p) {
    case '{': return json_list(p, '}', 1);
    case '[': return json_list(p, ']', 0);
    case '"': return json_string(p);
    case 'n': return !strncmp(*p, "null", 4) && (*p += 4, 1);
    case 't': return !strncmp(*p, "true", 4) && (*p += 4, 1);
    case 'f': return !strncmp(*p, "false", 5) && (*p += 5, 1);
    default: return json_number(p);
    }
}
static int json_line_ok(const char *line)
{
    const char *p = line;
    if (!json_value(&p))
        return 0;
    json_ws(&p);
    return !*p;
}

static int count_lines(const char *t)
{
    int n = 0;
    for (; t && *t; t++)
        n += *t == '\n';
    return n;
}

static void test_call_ret(void)
{
    memset(g_mem + 0x2100, 0, 0x40);
    g_mem[0x2100 - 5] = 0xE8;                   /* call rel32 */
    CHECK(hud_capture_is_call_ret(g_mem, 0x2100));
    g_mem[0x2110 - 6] = 0xFF;                   /* call [abs32] */
    g_mem[0x2110 - 5] = 0x15;
    CHECK(hud_capture_is_call_ret(g_mem, 0x2110));
    g_mem[0x2120 - 2] = 0xFF;                   /* call eax */
    g_mem[0x2120 - 1] = 0xD0;
    CHECK(hud_capture_is_call_ret(g_mem, 0x2120));
    g_mem[0x2130 - 3] = 0xFF;                   /* call [ecx+8] */
    g_mem[0x2130 - 2] = 0x51;
    g_mem[0x2130 - 1] = 0x08;
    CHECK(hud_capture_is_call_ret(g_mem, 0x2130));
    CHECK(!hud_capture_is_call_ret(g_mem, 0x2138));
    CHECK(!hud_capture_is_call_ret(g_mem, 3));
    memset(g_mem + 0x20F0, 0, 0x50);
}

static void test_off(void)
{
    /* The keys unset: nothing starts, nothing is hooked. */
    recomp_env_set(RENV_HUD_CAPTURE, NULL);
    recomp_env_set(RENV_HUD_CAPTURE_AT, NULL);
    recomp_env_set(RENV_HUD_CAPTURE_TABLE, NULL);
    CHECK(!hud_capture_init(&g_base));
    CHECK(!hud_capture_on());
    s_nhooks = 0;
    hud_capture_tables(fake_hook, 0);
    hud_capture_frame();
    hud_capture_end();
    CHECK(s_nhooks == 0);
    CHECK(!hud_layer_has_table());

    /* A bad window: off, with a line. */
    recomp_env_set(RENV_HUD_CAPTURE, DIR);
    recomp_env_set(RENV_HUD_CAPTURE_AT, "5");
    err_begin();
    CHECK(!hud_capture_init(&g_base));
    CHECK(strstr(err_end(), "is not FROM:COUNT") != NULL);
    CHECK(!hud_capture_on());
    recomp_env_set(RENV_HUD_CAPTURE_AT, "0:3");
    err_begin();
    CHECK(!hud_capture_init(&g_base));
    err_end();
    recomp_env_set(RENV_HUD_CAPTURE, NULL);
    recomp_env_set(RENV_HUD_CAPTURE_AT, NULL);
}

static void test_run(void)
{
    const struct hud_draw *dr;
    struct xbox_mod_call w, c;
    char path[NV2A_CAP_PATH];
    const char *err;
    char *out;
    int nd, slot, wi, di;

    hud_layer_reset();
    nv2a_cap_reset();
    nv2a_cap_set_kernel(&k_kernel);
    set_aspect(4, 3);
    s_nhooks = 0;
    remove(DIR "/capture.jsonl");
    remove(DIR "/frame_00002.bmp");
    remove(DIR "/frame_00003.bmp");
    write_file(TABLE, k_table);
    recomp_env_set(RENV_HUD_CAPTURE, DIR "/");
    recomp_env_set(RENV_HUD_CAPTURE_AT, "2:2");
    recomp_env_set(RENV_HUD_CAPTURE_TABLE, TABLE);

    err_begin();
    CHECK(hud_capture_init(&g_base) == 1);
    hud_capture_tables(fake_hook, 0);
    err = err_end();
    CHECK(hud_capture_on());
    CHECK(strstr(err, "on: guest frames 2..3 into " DIR) != NULL);
    CHECK(strstr(err, "draw sig:55 8B EC: skipped, a sig: draw needs its VA") != NULL);
    CHECK(strstr(err, "draw 0x1900: skipped, vtx is not ecx, edx or stack:N") != NULL);
    CHECK(strstr(err, "draw 0x00001A00: skipped, vtx and count go together") != NULL);
    CHECK(strstr(err, "1 of 4 draws hooked (level 0)") != NULL);

    /* The table's sites, its wrapper and its one usable draw. Loaded by
     * the capture, it is no mod's: no HUD margin item, no hud_pad. */
    CHECK(hud_layer_has_table());
    CHECK(!hud_layer_has_mod_table());
    nd = hud_layer_draws(&dr);
    CHECK(nd == 4);
    CHECK(dr[0].va == 0x1800 && !strcmp(dr[0].name, "q\"u\\ad") && dr[0].prim == 4);
    CHECK(dr[0].vtx.kind == HUD_LOC_STACK && dr[0].vtx.v == 0);
    CHECK(dr[0].count.kind == HUD_LOC_STACK && dr[0].count.v == 4);
    CHECK(dr[0].stride.kind == HUD_LOC_CONST && dr[0].stride.v == 16);
    CHECK(dr[1].va == 0 && dr[2].va == 0 && dr[3].va == 0);
    wi = hook_at(0x1000);
    di = hook_at(0x1800);
    CHECK(s_nhooks == 2 && wi >= 0 && di >= 0);
    if (wi < 0 || di < 0)
        return;

    /* Frame 1, outside the window: the calls go unrecorded, and with the
     * mod off nothing moves. */
    tick();
    quad(VTX1, 300, 10, 340, 30);
    CHECK(apply(0x2000, 0, 0) == -1);
    CHECK(rdf(VTX1) == 300.0f && rdf(VTX1 + 16) == 340.0f);

    /* Frame 2: its picture asked for at flip 1, after the walker caught
     * up. A string through the wrapper from the score's site (level 1),
     * then a draw entry under two callers (level 0). */
    s_idle_calls = 0;
    tick();
    CHECK(s_idle_calls == 1);
    w = at(0x7000, 0x2000);
    s_hooks[wi].pre(&w, s_hooks[wi].user);
    quad(VTX1, 300, 10, 340, 30);
    CHECK(apply(0x3333, 7, 0xDEADBEEFull) == -1);
    CHECK(rdf(VTX1) == 300.0f && rdf(VTX1 + 16) == 340.0f);
    s_hooks[wi].post(&w, s_hooks[wi].user);

    quad(VTX2, 100, 50, 200, 80);
    memset(g_mem + 0x9000, 0, 0x200);
    c = at(0x9000, 0x2F00);
    wr32(0x9004, VTX2);             /* stack:0, the vertices */
    wr32(0x9008, 4);                /* stack:4, the count */
    wr32(0x9010, 0x2010);           /* the timer's site, after a call rel32 */
    g_mem[0x2010 - 5] = 0xE8;
    wr32(0x9014, 0x2500);           /* in the code, but no call before it */
    wr32(0x9018, 0x2900);           /* an outer caller, after a call [abs32] */
    g_mem[0x2900 - 6] = 0xFF;
    g_mem[0x2900 - 5] = 0x15;
    s_hooks[di].pre(&c, s_hooks[di].user);

    /* The walker presents flip 1 and takes frame 2's request. */
    slot = nv2a_cap_take(1, path, sizeof path);
    CHECK(slot >= 0);
    CHECK(strstr(path, DIR "/frame_00002.bmp") != NULL);
    if (slot >= 0) {
        write_file(path, "BM");
        nv2a_cap_done(slot, 1);
    }
    nv2a_cap_note_flip(1);

    /* Frame 3: an implicit group's site (group null). Its flip passes
     * without a take. */
    err_begin();
    tick();
    quad(VTX1, 0, 0, 64, 16);
    CHECK(apply(0x2060, 0, 0) == -1);
    nv2a_cap_note_flip(2);
    CHECK(nv2a_cap_take(3, path, sizeof path) < 0);

    /* Frame 4: past the window; frame 3 settles and the file closes. */
    tick();
    err = err_end();
    CHECK(strstr(err, "frame 3: flip 2 passed without a picture") != NULL);
    CHECK(strstr(err, "done: frames 2..3, 3 lines (0 dropped), 1 pictures of 2 frames, in") != NULL);

    out = read_all(DIR "/capture.jsonl");
    CHECK(out != NULL);
    if (!out)
        return;
    CHECK(count_lines(out) == 5);
    for (int i = 0; i < 5; i++)
        CHECK(json_line_ok(line_at(out, i)));
    CHECK(!strcmp(line_at(out, 0),
                  "{\"type\":\"sprite\",\"level\":1,\"frame\":2,\"queue\":7,"
                  "\"chain\":[\"0x00002000\"],\"ret\":\"0x00003333\",\"site\":\"0x00002000\","
                  "\"element\":\"score\",\"group\":\"score\",\"n\":4,\"bbox\":[300,10,340,30],"
                  "\"prims\":1,\"tex\":\"0x00000000DEADBEEF\"}"));
    CHECK(!strcmp(line_at(out, 1),
                  "{\"type\":\"sprite\",\"level\":0,\"frame\":2,\"queue\":null,"
                  "\"draw\":\"0x00001800\",\"name\":\"q\\\"u\\\\ad\","
                  "\"chain\":[\"0x00002900\",\"0x00002010\"],\"ret\":\"0x00002F00\","
                  "\"site\":\"0x00002010\",\"element\":\"timer\",\"group\":\"timer\",\"n\":4,"
                  "\"bbox\":[100,50,200,80],\"prims\":1,\"tex\":null}"));
    CHECK(strstr(line_at(out, 2), "{\"type\":\"frame\",\"frame\":2,\"flip\":1,") != NULL);
    CHECK(strstr(line_at(out, 2),
                 "\"canvas\":{\"w\":640,\"h\":480,\"origin\":\"top-left\",\"y\":\"down\","
                 "\"view\":[0,0,640,480],\"target\":[640,480],\"pad\":0}")
          != NULL);
    CHECK(strstr(line_at(out, 2), "\"bmp\":\"frame_00002.bmp\"}") != NULL);
    CHECK(strstr(line_at(out, 3), "\"level\":1,\"frame\":3,") != NULL);
    CHECK(strstr(line_at(out, 3),
                 "\"site\":\"0x00002060\",\"element\":\"gauge\",\"group\":null,\"n\":4,"
                 "\"bbox\":[0,0,64,16]")
          != NULL);
    CHECK(strstr(line_at(out, 4), "{\"type\":\"frame\",\"frame\":3,\"flip\":2,") != NULL);
    CHECK(strstr(line_at(out, 4), "\"bmp\":null}") != NULL);
    if (s_failed)
        printf("capture.jsonl:\n%s", out);
    free(out);

    /* Finished: further frames and the end add nothing. */
    hud_capture_frame();
    hud_capture_end();
    out = read_all(DIR "/capture.jsonl");
    CHECK(out && count_lines(out) == 5);
    free(out);
}

/* A table a mod registered wins over hud_capture_table; its draws are
 * still hooked. And the capture's flag never turns the mod's part on. */
static void test_mod_wins(void)
{
    const char *err;

    hud_capture_reset();
    hud_layer_reset();
    nv2a_cap_reset();
    nv2a_cap_set_kernel(&k_kernel);
    s_nhooks = 0;
    CHECK(hud_layer_table(k_table, strlen(k_table), fake_hook) == 3);
    CHECK(s_nhooks == 1);
    CHECK(hud_layer_has_mod_table());
    recomp_env_set(RENV_HUD_CAPTURE, DIR);
    recomp_env_set(RENV_HUD_CAPTURE_AT, "1:1");
    recomp_env_set(RENV_HUD_CAPTURE_TABLE, "no-such-table.toml");
    err_begin();
    CHECK(hud_capture_init(&g_base) == 1);
    hud_capture_tables(fake_hook, 1);
    err = err_end();
    CHECK(strstr(err, "a mod registered a HUD table: it wins, "
                      "hud_capture_table=no-such-table.toml is not read") != NULL);
    CHECK(strstr(err, "cannot read") == NULL);
    CHECK(s_nhooks == 2 && hook_at(0x1800) >= 0);
    hud_capture_frame();            /* frame 1, the window */

    /* At 4:3 the mod on has no pad: off. At 16:9 it shifts, recording or
     * not. The capture alone (owner off) never does. */
    set_aspect(4, 3);
    CHECK(hud_layer_frame(1) == 0 && !hud_layer_active());
    set_aspect(16, 9);
    CHECK(hud_layer_frame(0) == 0 && !hud_layer_active());
    quad(VTX1, 300, 10, 340, 30);
    CHECK(apply(0x2000, 0, 0) == -1);
    CHECK(rdf(VTX1) == 300.0f);
    CHECK(hud_layer_frame(1) == 1 && hud_layer_active());
    quad(VTX1, 300, 10, 340, 30);
    CHECK(apply(0x2000, 0, 0) == XBOX_HUD_LEFT);
    CHECK(rdf(VTX1) != 300.0f);

    /* The picture fails; the end writes what it has. */
    {
        char path[NV2A_CAP_PATH];
        int slot = nv2a_cap_take(1, path, sizeof path);
        CHECK(slot >= 0);
        if (slot >= 0)
            nv2a_cap_done(slot, 0);
    }
    err_begin();
    hud_capture_end();
    err = err_end();
    CHECK(strstr(err, "frame 1: the picture failed") != NULL);
    CHECK(strstr(err, "ended at frame 1: 2 lines (0 dropped), 0 pictures of 1 frames, in") != NULL);
    {
        char *out = read_all(DIR "/capture.jsonl");
        CHECK(out && count_lines(out) == 3);
        /* Both recorded as the game wrote them, before any shift. */
        CHECK(out && strstr(line_at(out, 0), "\"level\":1,\"frame\":1,") != NULL);
        CHECK(out && strstr(line_at(out, 0), "\"bbox\":[300,10,340,30]") != NULL);
        CHECK(out && strstr(line_at(out, 1), "\"bbox\":[300,10,340,30]") != NULL);
        CHECK(out && strstr(line_at(out, 2), "\"type\":\"frame\",\"frame\":1,") != NULL);
        CHECK(out && strstr(line_at(out, 2), "\"bmp\":null}") != NULL);
        free(out);
    }
    set_aspect(4, 3);
}

/* Start a capture of frames 1..count over k_table with no walker
 * registered (no kernel yet): frames and lines, no pictures. */
static void start_bare(const char *count)
{
    char at[32];

    hud_capture_reset();
    hud_layer_reset();
    nv2a_cap_reset();
    nv2a_cap_set_kernel(NULL);
    set_aspect(4, 3);
    s_nhooks = 0;
    remove(DIR "/capture.jsonl");
    write_file(TABLE, k_table);
    snprintf(at, sizeof at, "1:%s", count);
    recomp_env_set(RENV_HUD_CAPTURE, DIR);
    recomp_env_set(RENV_HUD_CAPTURE_AT, at);
    recomp_env_set(RENV_HUD_CAPTURE_TABLE, TABLE);
    CHECK(hud_capture_init(&g_base) == 1);
    hud_capture_tables(fake_hook, 0);
}

/* No walker: one line for the run, not one per frame. Vertices past
 * retail RAM: not read, bbox null, said once. */
static void test_no_walker_high_va(void)
{
    struct xbox_mod_call c;
    const char *err, *p;
    char *out;
    int di, n = 0;

    err_begin();
    start_bare("2");
    di = hook_at(0x1800);
    CHECK(di >= 0);
    tick();
    if (di >= 0)
        for (int i = 0; i < 2; i++) {
            memset(g_mem + 0x9000, 0, 0x200);
            c = at(0x9000, 0x2F00);
            wr32(0x9004, 0x04000000u);  /* 64 MB: past retail RAM */
            wr32(0x9008, 4);
            s_hooks[di].pre(&c, s_hooks[di].user);
        }
    tick();
    tick();
    err = err_end();
    for (p = err; (p = strstr(p, "no walker registered")); p++)
        n++;
    CHECK(n == 1);
    CHECK(strstr(err, "did not catch up") == NULL);
    n = 0;
    for (p = err; (p = strstr(p, "lie past retail RAM")); p++)
        n++;
    CHECK(n == 1);
    CHECK(strstr(err, "done: frames 1..2, 2 lines (0 dropped), 0 pictures of 2 frames, in") != NULL);
    out = read_all(DIR "/capture.jsonl");
    CHECK(out && count_lines(out) == 4);
    CHECK(out && strstr(line_at(out, 0), "\"bbox\":null") != NULL);
    CHECK(out && strstr(line_at(out, 1), "\"bbox\":null") != NULL);
    CHECK(out && strstr(line_at(out, 2), "{\"type\":\"frame\",\"frame\":1,\"flip\":null,")
                     != NULL);
    free(out);
}

/* A huge finite vertex under a small view scale overflows to inf in the
 * canvas map: bbox null, and the line still parses as JSON. */
static void test_huge_vertex(void)
{
    static char t[sizeof k_table + 16];
    struct xbox_mod_call c;
    const char *v = strstr(k_table, "view = [0, 0, 640, 480]");
    char *out;
    int di;

    CHECK(v != NULL);
    if (!v)
        return;
    snprintf(t, sizeof t, "%.*sview = [0, 0, 1, 1]%s", (int)(v - k_table), k_table,
             v + strlen("view = [0, 0, 640, 480]"));
    hud_capture_reset();
    hud_layer_reset();
    nv2a_cap_reset();
    nv2a_cap_set_kernel(NULL);
    s_nhooks = 0;
    remove(DIR "/capture.jsonl");
    write_file(TABLE, t);
    recomp_env_set(RENV_HUD_CAPTURE, DIR);
    recomp_env_set(RENV_HUD_CAPTURE_AT, "1:1");
    recomp_env_set(RENV_HUD_CAPTURE_TABLE, TABLE);
    err_begin();
    CHECK(hud_capture_init(&g_base) == 1);
    hud_capture_tables(fake_hook, 0);
    di = hook_at(0x1800);
    CHECK(di >= 0);
    tick();
    if (di >= 0) {
        quad(VTX2, 0, 0, FLT_MAX, FLT_MAX);
        memset(g_mem + 0x9000, 0, 0x200);
        c = at(0x9000, 0x2F00);
        wr32(0x9004, VTX2);
        wr32(0x9008, 4);
        s_hooks[di].pre(&c, s_hooks[di].user);
    }
    tick();
    err_end();
    out = read_all(DIR "/capture.jsonl");
    CHECK(out && count_lines(out) == 2);
    for (int i = 0; out && i < count_lines(out); i++)
        CHECK(json_line_ok(line_at(out, i)));
    CHECK(out && strstr(line_at(out, 0), "\"level\":0,") != NULL);
    CHECK(out && strstr(line_at(out, 0), "\"bbox\":null") != NULL);
    CHECK(out && !strstr(out, "inf") && !strstr(out, "nan"));
    if (s_failed && out)
        printf("capture.jsonl:\n%s", out);
    free(out);
}

/* Level 1 with the mod off reads the mod's vtx_va for the bbox: one past
 * retail RAM is not read (bbox null), and the layer's own bbox query
 * refuses it too. */
static void test_level1_high_va(void)
{
    struct xbox_mod_call c;
    struct xbox_hud_call d = { 0x03FFFFF0u, 4, 0, 0, 0 };
    float bb[4];
    char *out;

    CHECK(!hud_layer_bbox(0x03FFFFF0u, 4, 16, 0, bb));
    CHECK(!hud_layer_bbox(0xFFFFFFF0u, 4, 16, 0, bb));
    CHECK(hud_layer_bbox(VTX1, 4, 16, 0, bb));
    err_begin();
    start_bare("1");
    tick();
    c = at(0x6000, 0x2000);
    CHECK(hud_layer_apply(&c, &d) == -1);
    d.vtx_va = 0xFFFFFFF0u;         /* wraps in 32 bits */
    CHECK(hud_layer_apply(&c, &d) == -1);
    tick();
    tick();
    err_end();
    out = read_all(DIR "/capture.jsonl");
    CHECK(out && count_lines(out) == 3);
    CHECK(out && strstr(line_at(out, 0), "\"site\":\"0x00002000\"") != NULL);
    CHECK(out && strstr(line_at(out, 0), "\"bbox\":null") != NULL);
    CHECK(out && strstr(line_at(out, 1), "\"bbox\":null") != NULL);
    free(out);
}

/* Many lines in one frame: the buffer grows (outside its lock) past its
 * first 64 KB and keeps every line, in order. */
static void test_emit_growth(void)
{
    enum { N = 2000 };
    const char *err;
    char *out;
    int ok = 1;

    err_begin();
    start_bare("1");
    quad(VTX1, 300, 10, 340, 30);
    tick();
    for (int i = 0; i < N; i++)
        apply(0x2000, (uint32_t)i, 0);
    tick();
    tick();
    err = err_end();
    CHECK(strstr(err, "2000 lines (0 dropped)") != NULL);
    out = read_all(DIR "/capture.jsonl");
    CHECK(out && strlen(out) > 65536);
    CHECK(out && count_lines(out) == N + 1);
    {
        const char *p = out;
        for (int i = 0; p && ok && i < N; i++) {
            const char *e = strchr(p, '\n'), *q;
            char want[32];
            snprintf(want, sizeof want, "\"queue\":%d,", i);
            q = strstr(p, want);
            ok = e && q && q < e;
            p = e ? e + 1 : NULL;
        }
    }
    CHECK(ok);
    CHECK(out && strstr(line_at(out, N), "{\"type\":\"frame\",\"frame\":1,") != NULL);
    free(out);
}

#ifndef _WIN32
/* Four guest threads emitting into one frame while it grows: every line
 * lands whole, none dropped (TSan sees the growth). */
#define EMIT_THREADS 4
#define EMIT_EACH    600
static void *emitter(void *arg)
{
    uint32_t esp = 0x7000u + 0x100u * (uint32_t)(uintptr_t)arg;
    for (int i = 0; i < EMIT_EACH; i++) {
        struct xbox_mod_call c;
        struct xbox_hud_call d = { VTX1, 4, 0, (uint32_t)i, 0 };
        memset(&c, 0, sizeof c);
        c.esp = esp;
        hud_layer_apply(&c, &d);
    }
    return NULL;
}

static void test_emit_threads(void)
{
    pthread_t th[EMIT_THREADS];
    const char *err;
    char *out;
    int made = 0;

    err_begin();
    start_bare("1");
    quad(VTX1, 300, 10, 340, 30);
    for (int t = 0; t < EMIT_THREADS; t++)
        wr32(0x7000u + 0x100u * (uint32_t)t, 0x2000);
    tick();
    for (int t = 0; t < EMIT_THREADS; t++)
        made += !pthread_create(&th[t], NULL, emitter, (void *)(uintptr_t)t);
    for (int t = 0; t < made; t++)
        pthread_join(th[t], NULL);
    tick();
    tick();
    err = err_end();
    CHECK(made == EMIT_THREADS);
    CHECK(strstr(err, "2400 lines (0 dropped)") != NULL);
    out = read_all(DIR "/capture.jsonl");
    CHECK(out && count_lines(out) == EMIT_THREADS * EMIT_EACH + 1);
    for (char *p = out; p && *p; ) {
        char *e = strchr(p, '\n');
        if (!e) {
            CHECK(0);
            break;
        }
        *e = 0;
        if (!json_line_ok(p)) {
            CHECK(0);
            break;
        }
        p = e + 1;
    }
    free(out);
}
#endif

#ifndef _WIN32
/* The shutdown on another thread (the exit hook, atexit) while the guest
 * is mid-frame: the end and the frame never share the queue or the file.
 * Every line written is whole and the file ends at a frame line. */
static volatile int s_stop;
static unsigned s_guest_frames;

static void *guest(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) {
        hud_layer_frame(0);
        hud_capture_frame();
        for (int i = 0; i < 8; i++)
            apply(0x2000, (uint32_t)i, 0);
        s_guest_frames++;
    }
    return NULL;
}

static void test_end_race(void)
{
    struct timespec ts = { 0, 20 * 1000 * 1000 };
    pthread_t th;
    const char *err, *last = NULL;
    char *out, *p;
    int bad = 0;

    for (int round = 0; round < 20; round++) {
        err_begin();
        start_bare("10000");
        quad(VTX1, 300, 10, 340, 30);
        s_stop = 0;
        s_guest_frames = 0;
        if (pthread_create(&th, NULL, guest, NULL)) {
            err_end();
            CHECK(0);
            return;
        }
        nanosleep(&ts, NULL);
        hud_capture_end();
        /* Further frames from the guest add nothing once it ended. */
        nanosleep(&ts, NULL);
        __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
        pthread_join(th, NULL);
        err = err_end();
        CHECK(strstr(err, "ended at frame") != NULL);
        CHECK(strstr(err, "ended at frame 0:") == NULL);
        out = read_all(DIR "/capture.jsonl");
        CHECK(out != NULL);
        if (!out)
            return;
        for (p = out; *p; p = strchr(p, '\n') + 1) {
            const char *e = strchr(p, '\n');
            if (!e || p[0] != '{' || e[-1] != '}') {
                bad++;
                break;
            }
            last = p;
        }
        CHECK(!bad);
        if (bad || (last && strncmp(last, "{\"type\":\"frame\",", 16)))
            printf("round %d, %u guest frames, at: %.200s\nlast: %.200s\nstderr: %.600s\n", round,
                   s_guest_frames, p, last ? last : "", err);
        CHECK(!last || !strncmp(last, "{\"type\":\"frame\",", 16));
        free(out);
        if (bad)
            break;
    }
}
#endif

/* The short form of [capture] draws: a list of addresses. */
static void test_list_form(void)
{
    static const char t[] =
        "version = 1\n[canvas]\nw = 640\nh = 480\n"
        "[layout]\nstride = 16\nx_off = 0\n"
        "[[site]]\nret_va = 0x2000\nelement = \"a\"\nkind = \"left\"\n"
        "[capture]\ndraws = [0x1800, \"0x1804\", \"sig:55 8B\", \"x\"]\n";
    const struct hud_draw *dr;

    hud_layer_reset();
    s_nhooks = 0;
    CHECK(hud_layer_table(t, strlen(t), fake_hook) == 1);
    CHECK(hud_layer_draws(&dr) == 4);
    CHECK(dr[0].va == 0x1800 && dr[1].va == 0x1804);
    CHECK(dr[2].va == 0 && dr[2].why && strstr(dr[2].why, "sig:"));
    CHECK(dr[3].va == 0 && dr[3].why);
    CHECK(dr[0].vtx.kind == HUD_LOC_NONE);
    /* [capture] holds draws and nothing else. */
    {
        static const char bad[] =
            "version = 1\n[canvas]\nw = 640\nh = 480\n"
            "[layout]\nstride = 16\nx_off = 0\n"
            "[capture]\nframes = 3\n";
        hud_layer_reset();
        err_begin();
        CHECK(hud_layer_table(bad, strlen(bad), fake_hook) < 0);
        CHECK(strstr(err_end(), "unknown key capture.frames") != NULL);
    }
    hud_layer_reset();
}

int main(void)
{
    g_base = (uintptr_t)g_mem;
    hud_layer_set_mem(&g_base);
#ifdef _WIN32
    _mkdir(DIR);
#else
    mkdir(DIR, 0755);
#endif

    test_call_ret();
    test_off();
    test_run();
    test_mod_wins();
    test_no_walker_high_va();
    test_huge_vertex();
    test_level1_high_va();
    test_emit_growth();
#ifndef _WIN32
    test_emit_threads();
#endif
#ifndef _WIN32
    test_end_race();
#endif
    test_list_form();
    hud_capture_reset();
    hud_layer_reset();
    nv2a_cap_reset();

    printf("hud_capture: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed != 0;
}
