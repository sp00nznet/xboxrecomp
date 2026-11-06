/*
 * mod_guest_call test: the mod API v6 (entity-mod-api D1) as a title runs
 * it through xbox_enhance_init.
 *
 * guest_call, from an on_frame callback and from a hook's pre, on a
 * lifted function that does what a lifted body may: reads its stack
 * arguments and ecx, pops them with `ret 8`, and clobbers every register
 * recomp_regs.h lists. After the call every one of them holds its value
 * from before, bit for bit (g_ebp, g_seh_ebp, g_fp_top, the x87 slots, an
 * xmm, an mm, mxcsr); the callee saw its arguments at [esp + 4], the
 * first one 16-byte aligned, the frame at least 0x100 below the caller's
 * esp and the return address RECOMP_GUEST_CALL_RA; the callee's eax comes
 * back. The hooked function then runs with its own entry registers. The
 * refusals: outside a guest callback, on a host thread inside one, no
 * lifted function, 17 arguments, no guest stack. Every register is checked
 * against the value set before the call, not against a second save, so
 * one the save list misses fails.
 *
 * guest_alloc: refused outside a callback, one block per mod (a second
 * call returns it, a larger one is refused), zeroed, from the contiguous
 * arena (stubbed) and only on first use.
 */
#include "enhance.h"
#include "input_backend.h"
#include "mod.h"
#include "recomp_env.h"
#include "recomp_exe_dir.h"
#include "recomp_guest_call.h"
#include "recomp_regs.h"
#include "xbox_memory_layout.h"

#include <fcntl.h>
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

/* ---- stubs for what the runtime would link ---------------------------- */

void xbox_PacingSetMode(int mode);
void xbox_PacingSetMode(int mode) { (void)mode; }
void xbox_RamCacheEnable(int on);
void xbox_RamCacheEnable(int on) { (void)on; }
/* kernel_language.c's setter (enhance.c), which this test does not link. */
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
/* The display probe needs a window system. */
void enhance_display_start(void) {}
int nv2a_overlay_stats(void) { return 0; }
void nv2a_overlay_toast(const char *fmt, ...) { (void)fmt; }
void enhance_menu_start(void);
void enhance_menu_start(void) {}
/* enhance_menu.c's item lookup for a mod's link setting: none here. */
struct enhance_item;
const struct enhance_item *enhance_menu_toolkit_item(const char *key);
const struct enhance_item *enhance_menu_toolkit_item(const char *key) { (void)key; return 0; }
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

/* Guest 0x10000..0x1FFFF: stacks at 0x18000 and below, the arena's blocks
 * from 0x1C000. */
#define GUEST_BASE 0x10000u
static uint8_t s_guest[0x10000];
ptrdiff_t xbox_GetMemoryOffset(void);
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)((uintptr_t)s_guest - GUEST_BASE); }

static uint32_t rd32(uint32_t va)
{
    uint32_t v;
    memcpy(&v, s_guest + (va - GUEST_BASE), 4);
    return v;
}

/* kernel_hal.c's guest-thread mark: this thread is guest-main unless a
 * case says otherwise. */
static int s_guest_thread = 1;
int xbox_DpcIsGuestThread(void);
int xbox_DpcIsGuestThread(void) { return s_guest_thread; }

/* The contiguous arena: page blocks from 0x1C000, dirty as a freed page
 * may be. */
static uint32_t s_arena_next = 0x1C000u;
static int s_arena_calls;
uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t va = s_arena_next, n = (size + 0xFFFu) & ~0xFFFu;
    (void)alignment;
    s_arena_calls++;
    if (va + n > GUEST_BASE + sizeof s_guest)
        return 0;
    memset(s_guest + (va - GUEST_BASE), 0xA5, n);
    s_arena_next += n;
    return va;
}

/* ---- the guest registers, as xbox_memory_layout.c defines them --------- */

RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp, g_esp;
RECOMP_TLS uint32_t g_seh_ebp, g_eflags, g_mxcsr;
RECOMP_TLS int g_df, g_fp_top, g_fp_cmp;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS uint16_t g_fp_control_word, g_fp_cc;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;

/* The registers are thread-local, so their addresses are no constants. */
static uint8_t *xmm_at(int k)
{
    RecompXmm *const r[8] = { &g_xmm0, &g_xmm1, &g_xmm2, &g_xmm3,
                              &g_xmm4, &g_xmm5, &g_xmm6, &g_xmm7 };
    return (uint8_t *)r[k];
}

static uint8_t *mm_at(int k)
{
    RecompMmx *const r[8] = { &g_mm0, &g_mm1, &g_mm2, &g_mm3, &g_mm4, &g_mm5, &g_mm6, &g_mm7 };
    return (uint8_t *)r[k];
}

/* The byte pattern regs_fill puts in xmm k / mm k. */
static uint8_t xmm_byte(uint32_t seed, int k, int b) { return (uint8_t)(seed + 16 * k + b + 1); }
static uint8_t mm_byte(uint32_t seed, int k, int b) { return (uint8_t)(seed + 8 * k + b + 0x81); }

/* A distinct pattern in every register, each a function of seed alone, so
 * regs_check can name what every one should hold without the save under
 * test. */
static void regs_fill(uint32_t seed)
{
    g_eax = seed + 1; g_ecx = seed + 2; g_edx = seed + 3; g_ebx = seed + 4;
    g_esi = seed + 5; g_edi = seed + 6; g_ebp = seed + 7; g_esp = 0x18000u - (seed & 0xF0u);
    g_seh_ebp = seed + 9; g_eflags = 0x246; g_mxcsr = 0x1F80u | (seed & 0x6000u);
    g_df = 1; g_fp_top = 5; g_fp_cmp = 2;
    for (int k = 0; k < 8; k++)
        g_fp_stack[k] = 1.25 * (k + 1) + seed;
    g_fp_control_word = 0x027F; g_fp_cc = 0x4100;
    for (int k = 0; k < 8; k++) {
        for (int b = 0; b < (int)sizeof(RecompXmm); b++)
            xmm_at(k)[b] = xmm_byte(seed, k, b);
        for (int b = 0; b < (int)sizeof(RecompMmx); b++)
            mm_at(k)[b] = mm_byte(seed, k, b);
    }
}

/* Every register in recomp_regs.h against what regs_fill(seed) put there,
 * ecx and esp as given (a hook's entry changes them). A register the save
 * leaves out comes back clobbered and fails here, where a compare of two
 * saves would miss it. */
static int regs_check(uint32_t seed, uint32_t ecx, uint32_t esp)
{
    int ok = g_eax == seed + 1 && g_ecx == ecx && g_edx == seed + 3 && g_ebx == seed + 4
        && g_esi == seed + 5 && g_edi == seed + 6 && g_ebp == seed + 7 && g_esp == esp
        && g_seh_ebp == seed + 9 && g_eflags == 0x246
        && g_mxcsr == (0x1F80u | (seed & 0x6000u)) && g_df == 1 && g_fp_top == 5
        && g_fp_cmp == 2 && g_fp_control_word == 0x027F && g_fp_cc == 0x4100;

    for (int k = 0; k < 8; k++) {
        ok = ok && g_fp_stack[k] == 1.25 * (k + 1) + seed;
        for (int b = 0; b < (int)sizeof(RecompXmm); b++)
            ok = ok && xmm_at(k)[b] == xmm_byte(seed, k, b);
        for (int b = 0; b < (int)sizeof(RecompMmx); b++)
            ok = ok && mm_at(k)[b] == mm_byte(seed, k, b);
    }
    return ok;
}

/* ---- the lifted functions ---------------------------------------------- */

#define VA_ADD   0x00200000u    /* stdcall 2 args, ecx: eax = a + b + ecx; ret 8 */
#define VA_NONE  0x00210000u    /* nothing lifted there */
#define VA_HOOK  0x00220000u    /* the hooked function: cdecl, eax = ecx */

static uint32_t s_seen_esp, s_seen_ra, s_seen_a, s_seen_b, s_seen_ecx, s_seen_edx;
static int s_add_calls;

/* What a lifted body may do: read [esp + 4], [esp + 8] and ecx, pop with
 * `ret 8`, and leave every other register changed. */
static void add_body(void)
{
    s_add_calls++;
    s_seen_esp = g_esp;
    s_seen_ra = rd32(g_esp);
    s_seen_a = rd32(g_esp + 4);
    s_seen_b = rd32(g_esp + 8);
    s_seen_ecx = g_ecx;
    s_seen_edx = g_edx;
    g_eax = s_seen_a + s_seen_b + g_ecx;
    g_ecx = g_edx = g_ebx = g_esi = g_edi = 0xDDDDDDDDu;
    g_ebp = 0xBADBADu; g_seh_ebp = 0x5E5E5E5Eu; g_eflags = 0x400; g_mxcsr = 0x7F80u;
    g_df = 0; g_fp_cmp = 0;
    g_fp_top = (g_fp_top + 7) & 7;
    g_fp_stack[g_fp_top] = 99.0;
    g_fp_top = (g_fp_top + 7) & 7;
    g_fp_stack[g_fp_top] = -1.0;
    for (int k = 0; k < 8; k++)
        g_fp_stack[k] = -2.0 - k;
    g_fp_control_word = 0x0C7F; g_fp_cc = 0;
    for (int k = 0; k < 8; k++) {
        memset(xmm_at(k), 0xEE, sizeof(RecompXmm));
        memset(mm_at(k), 0xFF, sizeof(RecompMmx));
    }
    g_esp += 4 + 8;
}

static void hook_body(void);
static struct recomp_hook_slot s_slots[] = {
    { VA_HOOK, 0, 0, 0, NULL },
};
RECOMP_HOOK_SLOTS(s_slots, 1)
static uint32_t s_body_ecx, s_body_ebp;
static void hook_body(void)
{
    s_body_ecx = g_ecx;
    s_body_ebp = g_ebp;
    g_eax = g_ecx;
    g_esp += 4;
}
static void sub_hook(void) { RECOMP_HOOK_CALL(&s_slots[0], hook_body); }

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va)
{
    return xbox_va == VA_ADD ? add_body : NULL;
}

/* ---- stderr ------------------------------------------------------------ */

static int s_saved_fd = -1;
static char s_cap[65536];

static void capture_begin(void)
{
    int fd;
    fflush(stderr);
    s_saved_fd = DUP(2);
    fd = OPEN_TRUNC("mod_guest_call_stderr.txt");
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
    f = fopen("mod_guest_call_stderr.txt", "rb");
    if (f) {
        n = fread(s_cap, 1, sizeof s_cap - 1, f);
        fclose(f);
    }
    s_cap[n] = 0;
    return s_cap;
}

/* ---- the mods ---------------------------------------------------------- */

static const struct xbox_mod_api *s_api;
static int s_frame_case;
static const struct xbox_mod k_caller;
static const struct xbox_mod k_other;

/* Results the frame callback records. */
static int s_rc, s_rc2, s_rc3, s_rc4, s_same, s_regs_ok;
static uint32_t s_eax_out, s_frame_esp;
static uint32_t s_blk, s_blk2, s_blk3, s_blk_other;
static int s_zeroed;

static void call_add_checked(void)
{
    RecompRegs before, after;
    const uint32_t args[2] = { 40, 2 };
    struct xbox_guest_args a = { 0xA0, 100, 0xD0, args, 2 };

    regs_fill(0x1000);
    s_frame_esp = g_esp;
    memset(&before, 0, sizeof before);
    memset(&after, 0, sizeof after);
    recomp_regs_save(&before);
    s_eax_out = 0;
    s_rc = s_api->guest_call(VA_ADD, &a, &s_eax_out);
    s_regs_ok = regs_check(0x1000, 0x1002, s_frame_esp);
    recomp_regs_save(&after);
    s_same = memcmp(&before, &after, sizeof before) == 0;
}

static void caller_frame(void *u)
{
    (void)u;
    switch (s_frame_case) {
    case 1:
        call_add_checked();
        break;
    case 2: {
        uint32_t args[XBOX_GUEST_MAX_STACK + 1] = { 0 };
        struct xbox_guest_args a = { 0, 0, 0, args, XBOX_GUEST_MAX_STACK + 1 };
        struct xbox_guest_args none = { 0, 0, 0, NULL, 0 };
        regs_fill(0x2000);
        s_rc = s_api->guest_call(VA_NONE, &none, NULL);
        s_rc2 = s_api->guest_call(VA_ADD, &a, NULL);
        g_esp = 0x200;
        s_rc3 = s_api->guest_call(VA_ADD, &none, NULL);
        regs_fill(0x2000);
        s_rc4 = s_api->guest_call(VA_ADD, NULL, NULL);
        break;
    }
    case 4: {
        /* A callback on a thread that never ran guest code. */
        struct xbox_guest_args none = { 0, 0, 0, NULL, 0 };
        regs_fill(0x5000);
        s_guest_thread = 0;
        s_rc = s_api->guest_call(VA_ADD, &none, NULL);
        s_blk = s_api->guest_alloc(&k_caller, 16, 16);
        s_guest_thread = 1;
        break;
    }
    case 3:
        s_blk = s_api->guest_alloc(&k_caller, 0x2E00, 16);
        s_zeroed = 1;
        for (uint32_t k = 0; s_blk && k < 0x2E00; k++)
            if (s_guest[s_blk - GUEST_BASE + k])
                s_zeroed = 0;
        s_blk2 = s_api->guest_alloc(&k_caller, 0x100, 16);
        s_blk3 = s_api->guest_alloc(&k_caller, 0x3000, 16);
        s_blk_other = s_api->guest_alloc(&k_other, 0x10, 16);
        break;
    default:
        break;
    }
}

/* The hook's pre calls the guest; the hooked body must then see its own
 * entry registers. */
static int s_pre_rc, s_pre_same, s_pre_regs_ok;
static uint32_t s_pre_esp;
static void hook_pre(struct xbox_mod_call *c, void *u)
{
    RecompRegs before, after;
    const uint32_t args[2] = { 1, 2 };
    struct xbox_guest_args a = { 0, 3, 0, args, 2 };
    uint32_t r = 0;
    (void)c;
    (void)u;
    memset(&before, 0, sizeof before);
    memset(&after, 0, sizeof after);
    recomp_regs_save(&before);
    s_pre_rc = s_api->guest_call(VA_ADD, &a, &r) && r == 6;
    s_pre_regs_ok = regs_check(0x3000, 0x77, s_pre_esp);
    recomp_regs_save(&after);
    s_pre_same = memcmp(&before, &after, sizeof before) == 0;
}

static int caller_init(const struct xbox_mod_api *api)
{
    s_api = api;
    if (api->size < offsetof(struct xbox_mod_api, guest_alloc) + sizeof api->guest_alloc)
        return 1;
    api->on_frame(caller_frame, NULL);
    api->hook(VA_HOOK, hook_pre, NULL, 0, NULL);
    return 0;
}

static const struct xbox_mod k_caller = { "caller", caller_init, NULL, NULL, 0, NULL };
static int other_init(const struct xbox_mod_api *api) { (void)api; return 0; }
static const struct xbox_mod k_other = { "other", other_init, NULL, NULL, 0, NULL };
static const struct xbox_mod k_stranger = { "stranger", other_init, NULL, NULL, 0, NULL };

static const char *frame(int which)
{
    s_frame_case = which;
    capture_begin();
    xbox_mod_frame();
    s_frame_case = 0;
    return capture_end();
}

int main(void)
{
    static const struct xbox_mod *const mods[] = { &k_caller, &k_other, NULL };
    const char *out;

    MKDIR("config");
    remove("config/config.toml");
    xbox_InputSetBackend(&k_fake);
    xbox_mod_register_table(mods);
    capture_begin();
    xbox_enhance_init(".", NULL);
    out = capture_end();
    CHECK(s_api && s_api->version == 8u && XBOX_MOD_API_VERSION == 8u);
    HAS(out, "[MOD] 2 running: caller other");

    /* 1. From a frame callback: registers back, arguments, frame, eax. */
    s_add_calls = 0;
    out = frame(1);
    CHECK(s_rc == 1);
    CHECK(s_add_calls == 1);
    CHECK(s_eax_out == 142);
    CHECK(s_same);
    CHECK(s_regs_ok);
    CHECK(s_seen_a == 40 && s_seen_b == 2);
    CHECK(s_seen_ecx == 100 && s_seen_edx == 0xD0);
    CHECK(s_seen_ra == RECOMP_GUEST_CALL_RA);
    CHECK(((s_seen_esp + 4) & 15u) == 0);
    CHECK(s_seen_esp + 4 + 8 <= s_frame_esp - RECOMP_GUEST_CALL_GAP);
    CHECK(s_seen_esp + 4 + 8 > s_frame_esp - RECOMP_GUEST_CALL_GAP - 32);
    /* And still so after the callback returned. */
    CHECK(regs_check(0x1000, 0x1002, s_frame_esp));

    /* 2. From a hook's pre: the hooked body gets its own registers. */
    regs_fill(0x3000);
    g_ecx = 0x77;
    g_esp -= 4;
    s_pre_esp = g_esp;
    sub_hook();
    CHECK(s_pre_rc == 1);
    CHECK(s_pre_same);
    CHECK(s_pre_regs_ok);
    CHECK(s_body_ecx == 0x77 && s_body_ebp == 0x3007);
    CHECK(g_eax == 0x77);

    /* 3. Refusals inside a callback. */
    s_add_calls = 0;
    out = frame(2);
    CHECK(s_rc == 0 && s_rc2 == 0 && s_rc3 == 0 && s_rc4 == 0);
    CHECK(s_add_calls == 0);
    HAS(out, "guest_call 0x00210000: no lifted function there");
    HAS(out, "guest_call 0x00200000: 17 stack arguments (at most 16)");
    HAS(out, "guest_call 0x00200000: esp 0x00000200 is no guest stack");
    HAS(out, "guest_call 0x00200000: no arguments given");

    /* 4. Off a guest callback: refused, nothing run, nothing allocated. */
    {
        struct xbox_guest_args none = { 0, 0, 0, NULL, 0 };
        regs_fill(0x4000);
        capture_begin();
        CHECK(s_api->guest_call(VA_ADD, &none, NULL) == 0);
        CHECK(s_api->guest_alloc(&k_caller, 16, 16) == 0);
        out = capture_end();
        CHECK(s_add_calls == 0);
        CHECK(s_arena_calls == 0);
        HAS(out, "[MOD] guest_call 0x00200000: refused, not on a guest thread");
        HAS(out, "[MOD caller] guest_alloc: refused, not on a guest thread");
    }

    /* 4b. Inside a callback but on a host thread (never marked guest):
     * refused by the register layer, nothing run, nothing allocated. */
    s_add_calls = 0;
    s_blk = 1;
    out = frame(4);
    CHECK(s_rc == 0 && s_blk == 0);
    CHECK(s_add_calls == 0 && s_arena_calls == 0);
    HAS(out, "guest_call 0x00200000: refused, not a guest thread");
    HAS(out, "[GUEST] guest_alloc: refused, not a guest thread");
    CHECK(regs_check(0x5000, 0x5002, 0x18000u));

    /* 5. guest_alloc: one zeroed block per mod, on first use. */
    out = frame(3);
    CHECK(s_blk == 0x1C000u);
    CHECK(s_zeroed);
    CHECK(s_blk2 == s_blk);
    CHECK(s_blk3 == 0);
    CHECK(s_blk_other == 0x1F000u);
    CHECK(s_arena_calls == 2);
    HAS(out, "[MOD caller] guest_alloc: 11776 bytes at 0x0001C000");
    HAS(out, "[MOD caller] guest_alloc: 12288 bytes refused, the block has 11776");
    {
        s_frame_case = 0;
        capture_begin();
        CHECK(s_api->guest_alloc(&k_stranger, 16, 16) == 0);
        out = capture_end();
        HAS(out, "[MOD] guest_alloc: refused, not a mod in the table");
    }

    xbox_mod_shutdown();
    remove("config/config.toml");
    printf("mod_guest_call: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
