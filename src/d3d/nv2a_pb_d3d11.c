/**
 * A D3D11 backend for the pushbuffer executor (struct nv2a_pb_backend).
 *
 * The executor (src/kernel/nv2a_pb_exec.c) decodes the title's pushbuffer into
 * the state in nv2a_pb_state.h and calls the hooks of struct nv2a_pb_backend;
 * this file answers them with D3D11 instead of the CPU rasteriser:
 *
 *   on_clear  -> ClearRenderTargetView on the surface at color_offset
 *   on_draw   -> the batch's vertex program, translated to HLSL by d3d8_vsh.c
 *                and compiled with D3DCompile (Wine's d3dcompiler_47 under
 *                Proton), or a pass-through for pre-transformed batches
 *   on_flip   -> copy the surface the walker picked for this flip
 *                (nv2a_pb_present_state) into the swap chain, Present
 *
 * Opt-in: RECOMP_PB_BACKEND=d3d11 (see nv2a_pb_d3d11_register_from_env).
 *
 * Depth and stencil: a D32_FLOAT_S8X24_UINT buffer per zeta offset, cleared
 * by on_zclear.
 *
 * Not yet: the clear rectangle (whole surface), guest readback of the rendered
 * surface (the title never sees D3D11's pixels). A 16-bit surface is drawn
 * into a BGRA8 target and bound from there (rt_texture), at 8 bits a channel.
 *
 * Everything runs on the NV2A ack thread: the device, the window and its
 * message pump are created and used there only.
 *
 * Off Windows this file compiles to nothing.
 */
#ifdef _WIN32

#define COBJMACROS
#include <windows.h>
#include "recomp_env.h"
#include "recomp_exit_hook.h"
#include "mem_stats.h"
#include <d3d11.h>
#include <d3d11_1.h>   /* ID3D11DeviceContext1::ClearView, for a partial clear */
#include <dxgi.h>
#include <dxgi1_4.h>  /* IDXGIAdapter3::QueryVideoMemoryInfo, the [MEM] line */
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d8_vsh.h"
#include "d3d8_combiners.h"
#include "d3d8_swizzle.h"
#include "nv2a_basic_hlsl.h"
#include "nv2a_pb_state.h"
#include "nv2a_backend_common.h"
#include "nv2a_stats.h"
#include "nv2a_tex_pack.h"
#ifdef XBOXRECOMP_ENHANCE
#include "nv2a_overlay.h"
#include "nv2a_present_chain.h"
#define LIBRA_RUNTIME_D3D11
#include "librashader.h"
#endif
#include "nv2a_vsh_cpu.h"      /* the batch trace runs the CPU interpreter */
#include "shader_cache.h"
#include "xbox_memory_layout.h"   /* xbox_GetMemoryOffset */
#include "video/video_player.h"   /* xbox_FramebufferWindowTitleText */
#include "video/window_title.h"

#define LOGP "[d3d11] "
#define D3D11_WND_CLASS "RecompNv2aD3D11"   /* RegisterClassA, then CreateWindowW as L"" D3D11_WND_CLASS */

typedef HRESULT (WINAPI *compile_fn)(LPCVOID, SIZE_T, LPCSTR,
                                     const D3D_SHADER_MACRO *, ID3DInclude *,
                                     LPCSTR, LPCSTR, UINT, UINT,
                                     ID3DBlob **, ID3DBlob **);
typedef HRESULT (WINAPI *create_blob_fn)(SIZE_T, ID3DBlob **);

/* kernel_path.c: <HDD root>/ShaderCache, which the dxbc cache sits under. */
int xbox_path_shader_cache(char *out, size_t n);

/* ---- device ------------------------------------------------------------ */

static int                     s_failed;        /* init failed: draw nothing */
static HWND                    s_hwnd;
/* This window is not fb_present.c's, which keeps the title bar's name (and,
 * with RECOMP_TRACE=title, FPS and draws); ours comes from the same place.
 * One clock for the creation title and the per-flip refresh, so the window
 * opens with the game's name and the first flip has nothing to rewrite. */
static struct xbox_title_clock s_title_clock;
/* Present's sync interval. Stock D3D11 presents without waiting for the
 * display (0): the guest's vblank clock paces the frames. RECOMP_PRESENT_VSYNC
 * is the SDL window's knob (default on there); here only a value that turns
 * it on changes anything, so an unset key keeps the stock behaviour. */
static UINT                    s_sync_interval;
static ID3D11Device           *s_dev;
static ID3D11DeviceContext    *s_ctx;
static ID3D11DeviceContext1   *s_ctx1;   /* NULL below D3D 11.1: partial clears clear it all */
static IDXGISwapChain         *s_swap;
static ID3D11Texture2D        *s_back;
static UINT                    s_back_w, s_back_h;
/* The swap chain size init() created, before any WM_SIZE resize. */
static UINT                    s_made_w, s_made_h;
/* Present (E1, nv2a_host_opts): the client size WM_SIZE last reported (0 =
 * none since init); the back buffer's view, made for the scaling blit only;
 * whether the frame being dumped is the present RT rather than the back
 * buffer (the blit path: the dump stays the frame, not the window). */
static volatile UINT           s_client_w, s_client_h;
static ID3D11RenderTargetView *s_back_rtv;
static int                     s_dump_rt;
static compile_fn              s_compile;
static create_blob_fn          s_create_blob;  /* a cache hit's blob */

/* The on-disk shader cache (shader_cache.h): D3DCompile under Proton is
 * Wine's, at tens of ms a shader, and a title's first 3D scene asks for a
 * few dozen at once (JSRF's title: 24 vertex programs in its load). A hit
 * is the bytes the same d3dcompiler_47 made from the same source and flags,
 * so what is drawn does not change. The compiler's identity, its path, size
 * and write time, is in every key: a new d3dcompiler (a Proton or Windows
 * update) starts over. RECOMP_DXBC_CACHE=0 turns it off, a path moves it. */
static shader_cache            s_shc;
static char                    s_shc_compiler[1100];
/* Compiles and cache loads since init, with their time, for the log. */
static unsigned                s_sh_compiled, s_sh_cached;
static double                  s_sh_compile_ms, s_sh_cached_ms;

static ID3D11Buffer           *s_cb_consts;     /* b1: c[192]            */
static ID3D11Buffer           *s_cb_vp;         /* b2: viewport, fog, specular */
static ID3D11Buffer           *s_cb_ps;         /* PS b0: PsConsts       */

/* Per-batch CPU memos (RECOMP_D3D11_MEMO=0 turns them off for A/B): a
 * constant buffer is only rewritten when its bytes change, and the shader
 * and texture lookups reuse the last batch's answer when their inputs are
 * the same. Each one returns what the full path would, so the output does
 * not change. */
static int s_memo = -1;
static int memo_on(void)
{
    if (s_memo < 0) {
        const char *e = recomp_env(RENV_D3D11_MEMO);
        s_memo = !(e && *e == '0');
    }
    return s_memo;
}

/* The pipeline state last bound, so a batch binding the same again skips
 * the call. Only state this file alone sets and nothing unbinds behind it;
 * SRVs (which the runtime nulls on a render-target hazard) and the vertex
 * and index buffers (new offsets every batch) are always set. Releasing a
 * shader or input layout bumps s_state_gen, since a new object can come
 * back at a released one's address. The output merger's targets need no
 * such bump: the context holds a reference to the views it has bound, so a
 * bound view cannot be freed and its address reused. Rebinding the same
 * targets would only unbind SRVs of them, and the runtime already nulls
 * those when they are bound after the targets. All 0xFF is "nothing
 * known". */
static uint32_t s_state_gen;
static struct {
    uint32_t                  gen;
    ID3D11BlendState         *bs;
    float                     bf[4];    /* OMSetBlendState's blend factor */
    ID3D11RasterizerState    *rs;
    ID3D11InputLayout        *il;
    D3D11_PRIMITIVE_TOPOLOGY  topo;
    ID3D11VertexShader       *vs;
    ID3D11PixelShader        *ps;
    ID3D11Buffer             *vs_cb[3], *ps_cb;
    ID3D11SamplerState       *smp[4];
    D3D11_VIEWPORT            vp;
    D3D11_RECT                sr;
    ID3D11RenderTargetView   *rtv;
    ID3D11DepthStencilView   *dsv;
    ID3D11DepthStencilState  *dss;
    UINT                      sref;     /* the stencil reference with dss */
} s_sc;
static int s_sc_init;

static int sc_on(void)
{
    if (!memo_on())
        return 0;
    if (!s_sc_init || s_sc.gen != s_state_gen) {
        memset(&s_sc, 0xFF, sizeof s_sc);
        s_sc.gen = s_state_gen;
        s_sc_init = 1;
    }
    return 1;
}
static ID3D11Buffer           *s_vb;            /* ring of float4 streams */
static UINT                    s_vb_size, s_vb_pos;
static ID3D11Buffer           *s_ib;
static UINT                    s_ib_size, s_ib_pos;
/* Rasteriser states: [cull][front CCW], cull 0 none, 1 back, 2 front.
 * s_rs_sc are the same with the scissor on: every draw's, whose scissor is
 * the surface clip (nv2a_draw_box). */
static ID3D11RasterizerState  *s_rs[3][2], *s_rs_sc[3][2];
static ID3D11DepthStencilState *s_ds_off;
static ID3D11PixelShader      *s_ps;

/* The pixel shaders' constants (PS b0): struct nv2a_ps_consts, whose
 * layout PS_CB_DECL must match. */
typedef struct nv2a_ps_consts PsConsts;

/* PsConsts and the legacy path's NV2APSConstants are one shader ABI. */
#define PS_ABI_SAME(a, b) \
    _Static_assert(offsetof(PsConsts, a) == offsetof(NV2APSConstants, b) \
                   && sizeof(((PsConsts *)0)->a) \
                      == sizeof(((NV2APSConstants *)0)->b), #a " != " #b)
PS_ABI_SAME(c0, c0);
PS_ABI_SAME(c1, c1);
PS_ABI_SAME(fc0, final_c0);
PS_ABI_SAME(fc1, final_c1);
PS_ABI_SAME(fog_color, fog_color);
PS_ABI_SAME(alpha_ref, alpha_ref);
PS_ABI_SAME(alpha_func, alpha_func);
PS_ABI_SAME(alpha_test_enable, alpha_test_enable);
PS_ABI_SAME(fog_enable, fog_enable);
PS_ABI_SAME(alpha_only, alpha_only);
PS_ABI_SAME(tex_scale, tex_scale);
PS_ABI_SAME(tex_mode, tex_mode);
PS_ABI_SAME(bump, bump);
PS_ABI_SAME(bumpls, bumpls);
PS_ABI_SAME(clip_mode, clip_mode);
_Static_assert(sizeof(PsConsts) == sizeof(NV2APSConstants),
               "PsConsts and NV2APSConstants differ in size");
#undef PS_ABI_SAME

/* PS_CB_DECL (nv2a_basic_hlsl.h) is that layout in HLSL. */
static uint64_t s_presents, s_clears, s_draws, s_draws_skipped;
/* Draws dropped because CULL_FACE was FRONT_AND_BACK (D3D11 has no such
 * mode); counted apart from s_draws_skipped, which means "could not draw". */
static uint64_t s_draws_culled;
static uint64_t s_draws_bconst;   /* drawn with a CONSTANT_* blend factor */
static uint64_t s_draws_stretch;  /* hor+: batches placed off-centre (nv2a_aspect_place) */
static uint64_t s_tex_uploads, s_tex_rehash, s_tex_changed;
static uint64_t s_tex_mip_uploads;   /* uploads with more than one level */
static uint64_t s_vb_native, s_vb_expanded;  /* vertex bytes copied / made */
static uint64_t s_draws_prog, s_draws_tex;   /* with a vertex program / a texture */

/* ---- render targets: one per guest surface ------------------------------ */

/* A post-process chain can draw more than 8 targets a frame; with 8 the
 * LRU cycled them and an off-screen one evicted between its draw and the
 * pass that samples it lost its pixels (this backend writes nothing back).
 * The Metal backend's size. */
#define RT_MAX 16
/* render.scale keeps targets inside D3D11's 2D limit at feature level 11
 * (nv2a_host_size lowers the factor for a larger surface). */
#define D3D11_MAX_DIM 16384u
typedef struct {
    uint32_t                offset;     /* color_offset, as the title set it */
    uint32_t                addr;       /* ... resolved to a guest address */
    UINT                    w, h;       /* guest size: the key, the mapping */
    UINT                    hw, hh;     /* host size: the texture, (w + 2 pad) * n x h * n */
    UINT                    pad;        /* hor+ padding, guest pixels a side (nv2a_host_pad) */
    unsigned                n;          /* the render.scale factor used */
    ID3D11Texture2D        *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;      /* for render-to-texture, made lazily */
    uint64_t                last_use;
    uint64_t                last_draw, last_clear;  /* s_seq at the last one */
    uint32_t                pitch;      /* the guest pitch it was made with */
    uint32_t                fmt;        /* the SET_SURFACE_FORMAT word it was drawn with */
    struct nv2a_rt_own      own;        /* is the guest memory under it still its own */
    uint32_t                bar_flip[2]; /* hor+ band bars per third (nv2a_aspect_bar_mark) */
} RenderTarget;

typedef struct nv2a_stage Stage;
static RenderTarget s_rt[RT_MAX];
static RenderTarget *s_drawn;   /* what the last flip presented */
static RenderTarget *s_cur;     /* the target of the batch being drawn */
static uint64_t      s_tick;
static uint64_t      s_seq;              /* draw/clear order */
static uint64_t      s_rtt_binds;        /* textures served from a render target */
static uint64_t      s_loop_hits;        /* setup_pixel stage loops served by its memo */

/* ---- shaders --------------------------------------------------------------- */

#define VS_MAX 128
typedef struct {
    uint32_t            hash;     /* 0 = pass-through (no program) */
    int                 used;
    int                 bad;      /* failed to compile: skip its batches */
    uint16_t            inputs;   /* ATTR slots the shader reads */
    struct nv2a_aspect_gate hp;   /* hor+: is this program 2D */
    ID3D11VertexShader *vs;
    ID3DBlob           *code;     /* for input layouts made later */
    struct {
        uint8_t            fmt[16];  /* DXGI_FORMAT per ATTR slot, 0 unread */
        ID3D11InputLayout *il;
    } lay[8];                     /* one per combination of stream formats */
    int                 lay_next;
} VsEntry;

static VsEntry s_vs[VS_MAX];

#define BLEND_MAX 32
typedef struct {
    uint32_t          key[5];
    ID3D11BlendState *bs;
} BlendEntry;
static BlendEntry s_blend[BLEND_MAX];
static int        s_blend_n;

/* ------------------------------------------------------------------------- */

/* The port-0 keyboard and mouse (src/input/keyboard.c, mouse.c); declared
 * here rather than included so this file keeps no dependency on src/input's
 * headers. */
extern void xbox_InputKeyEventWin32(int vk, unsigned long lparam, int down);
extern void xbox_InputKeysClear(void);
extern void xbox_InputMouseWin32Msg(void *hwnd, unsigned msg, uintptr_t w, intptr_t l);
extern void xbox_InputMouseWin32Sync(void *hwnd);
extern void xbox_InputMouseWin32Release(void *hwnd);
extern int xbox_InputKeyboardOn(void);
extern int xbox_InputTextWin32Char(void *hwnd, uintptr_t w, intptr_t l);
extern void xbox_InputTextFocusLost(void);

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* The mouse only observes (motion, buttons, focus, the capture), so
     * every message goes on to the cases below. */
    xbox_InputMouseWin32Msg(h, m, (uintptr_t)w, (intptr_t)l);

    switch (m) {
    /* Keys for the port-0 keyboard, as the GDI window (fb_present.c)
     * records them. This window is the only one a D3D11 run has, so
     * without these the keyboard is a port-0 pad that never presses
     * anything. RECOMP_TRACE=key traces the edges in xbox_InputKeyEvent. */
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        /* Auto-repeat (bit 30, the previous state) is not a new edge. */
        if (!(l & (1L << 30)))
            xbox_InputKeyEventWin32((int)w, (unsigned long)l, 1);
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        xbox_InputKeyEventWin32((int)w, (unsigned long)l, 0);
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;
    /* Typed text for a menu field (text_input.c), only while one has
     * focus; otherwise WM_CHAR goes on to DefWindowProc. Same handling as
     * the GDI window's (fb_present.c). */
    case WM_CHAR:
        if (xbox_InputTextWin32Char(h, (uintptr_t)w, (intptr_t)l))
            return 0;
        break;
    /* Alt (the walk key) or F10 alone would put the window in menu mode,
     * which takes the keyboard until the next key. There is no menu;
     * Alt+F4 is SC_CLOSE and still reaches Windows. With the keyboard off
     * the keys drive nothing, so Alt+Space keeps the system menu. */
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_KEYMENU && xbox_InputKeyboardOn())
            return 0;
        break;
    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        xbox_InputKeysClear();
        xbox_InputTextFocusLost();
        return 0;
    }
    if (m == WM_CLOSE) {
        /* Closing the window ends the run, like the CPU path's window.
         * ExitProcess skips atexit, where the missing-file summary prints. */
        xbox_InputMouseWin32Release(h);   /* no pump follows to drop the clip */
        recomp_exit_hook_run("window");
        ExitProcess(0);
    }
    if (m == WM_SIZE && w != SIZE_MINIMIZED && LOWORD(l) && HIWORD(l)) {
        /* Recorded only: the flip (this thread, which pumps the window)
         * resizes the swap chain before its next present. */
        s_client_w = LOWORD(l);
        s_client_h = HIWORD(l);
    }
    return DefWindowProcA(h, m, w, l);
}

/* A quit asked for off this thread (RECOMP_DEBUG=pad_quit): closed as the
 * close button closes it, by WM_CLOSE on the thread that pumps the window. */
static int post_close(const char *why)
{
    (void)why;
    return PostMessageA(s_hwnd, WM_CLOSE, 0, 0) ? 1 : 0;
}

static void pump(void)
{
    /* RECOMP_WINDOW_QUIT_AFTER=<seconds>: close the window then, as the SDL
     * window does, so a timed bench run leaves through WM_CLOSE (the exit
     * hooks: profile and summaries) rather than a kill. Timed from the
     * first pump, the window's first frame. */
    static ULONGLONG quit_at;
    static int quit_posted;
    MSG msg;
    if (!quit_at && recomp_env(RENV_WINDOW_QUIT_AFTER)) {
        double s = atof(recomp_env(RENV_WINDOW_QUIT_AFTER));
        quit_at = s > 0 ? GetTickCount64() + (ULONGLONG)(s * 1000.0) : (ULONGLONG)-1;
    }
    if (quit_at && quit_at != (ULONGLONG)-1 && !quit_posted && GetTickCount64() >= quit_at) {
        quit_posted = 1;
        PostMessageA(s_hwnd, WM_CLOSE, 0, 0);
    }
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    xbox_InputMouseWin32Sync(s_hwnd);
}

static double sh_now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* The DLL's VS_FIXEDFILEINFO file version, read from the loaded module's
 * own resource (no version.dll): a replacement that keeps the old file's
 * size and time still changes this. 0 when there is none. */
static unsigned long long dll_file_version(HMODULE dll)
{
    HRSRC r = FindResourceW(dll, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16) /* RT_VERSION */);
    const BYTE *b;
    DWORD n, i;

    if (!r || !(n = SizeofResource(dll, r)))
        return 0;
    b = (const BYTE *)LockResource(LoadResource(dll, r));
    if (!b)
        return 0;
    /* The fixed block sits after a wide-string header; find its signature. */
    for (i = 0; i + sizeof(VS_FIXEDFILEINFO) <= n; i += 4) {
        const VS_FIXEDFILEINFO *v = (const VS_FIXEDFILEINFO *)(b + i);
        if (v->dwSignature == 0xFEEF04BDu)
            return (unsigned long long)v->dwFileVersionMS << 32 | v->dwFileVersionLS;
    }
    return 0;
}

/* Opens the cache, after D3DCompile is found. The key's compiler identity is
 * "<path>|<size>|<write time>|<file version>" of the d3dcompiler_47.dll in
 * use; when that cannot be asked the cache stays off, since a key without
 * the compiler could hand one compiler's output to another. */
static void shader_cache_setup(HMODULE dll)
{
    const char *e = recomp_env(RENV_DXBC_CACHE);
    shader_cache_env_kind kind = shader_cache_env(e);
    char dir[1024], path[MAX_PATH * 3];
    wchar_t wpath[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    DWORD n;

    if (kind == SHADER_CACHE_ENV_OFF) {
        fprintf(stderr, LOGP "shader cache: off (RECOMP_DXBC_CACHE=%s)\n", e);
        return;
    }
    if (kind == SHADER_CACHE_ENV_RELATIVE) {
        fprintf(stderr, LOGP "shader cache: off (RECOMP_DXBC_CACHE=%s is not an "
                "absolute directory)\n", e);
        return;
    }
    n = GetModuleFileNameW(dll, wpath, MAX_PATH);
    if (!s_create_blob || !n || n >= MAX_PATH
     || !GetFileAttributesExW(wpath, GetFileExInfoStandard, &fa)
     || !WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, (int)sizeof path, NULL, NULL)) {
        fprintf(stderr, LOGP "shader cache: off (no compiler identity)\n");
        return;
    }
    snprintf(s_shc_compiler, sizeof s_shc_compiler, "%s|%08lx%08lx|%08lx%08lx|%016llx",
             path, (unsigned long)fa.nFileSizeHigh, (unsigned long)fa.nFileSizeLow,
             (unsigned long)fa.ftLastWriteTime.dwHighDateTime,
             (unsigned long)fa.ftLastWriteTime.dwLowDateTime, dll_file_version(dll));
    if (kind == SHADER_CACHE_ENV_DIR) {
        snprintf(dir, sizeof dir, "%s", e);
    } else if (xbox_path_shader_cache(dir, sizeof dir - 8)) {
        /* Beside the saves, under no name a guest path maps to. */
        strcat(dir, "/dxbc");
    } else {
        fprintf(stderr, LOGP "shader cache: off (no HDD root yet)\n");
        return;
    }
    if (shader_cache_init(&s_shc, dir)) {
        unsigned pruned = shader_cache_prune(&s_shc, SHADER_CACHE_DEFAULT_CAP);
        fprintf(stderr, LOGP "shader cache: %s (%u old files removed)\n", s_shc.dir, pruned);
    }
    else
        fprintf(stderr, LOGP "shader cache: off (cannot make or write %s)\n", dir);
}

static int compile(const char *what, const char *src, const char *target,
                   ID3DBlob **code)
{
    ID3DBlob *err = NULL;
    shader_cache_key key;
    double t0 = sh_now_ms();
    HRESULT hr;

    key.compiler = s_shc_compiler;
    key.target = target;
    key.entry = "main";
    key.flags1 = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    key.flags2 = 0;
    key.src = src;
    key.src_len = strlen(src);
    if (s_shc.dir[0]) {
        size_t n = 0;
        void *hit = shader_cache_load(&s_shc, &key, &n);
        if (hit && SUCCEEDED(s_create_blob(n, code))) {
            memcpy(ID3D10Blob_GetBufferPointer(*code), hit, n);
            free(hit);
            s_sh_cached++;
            s_sh_cached_ms += sh_now_ms() - t0;
            return 1;
        }
        free(hit);
    }
    hr = s_compile(src, key.src_len, what, NULL, NULL, key.entry, target,
                   key.flags1, key.flags2, code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, LOGP "compile %s failed hr=0x%08lX\n%s\n", what,
                (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (recomp_env(RENV_D3D11_VERBOSE))
            fprintf(stderr, "--- source ---\n%s\n--- end ---\n", src);
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    if (err) ID3D10Blob_Release(err);
    s_sh_compiled++;
    s_sh_compile_ms += sh_now_ms() - t0;
    if (s_shc.dir[0])
        shader_cache_store(&s_shc, &key, ID3D10Blob_GetBufferPointer(*code),
                           ID3D10Blob_GetBufferSize(*code));
    return 1;
}

static ID3D11Buffer *make_buffer(UINT size, UINT bind)
{
    D3D11_BUFFER_DESC bd;
    ID3D11Buffer *b = NULL;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = size;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = bind;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &b)))
        return NULL;
    return b;
}

/* The basic pixel shader (nv2a_basic_hlsl.h): the stock text, and
 * render.snap_2d's, built when it first draws (snap_ps). */
static const char s_ps_src[] = NV2A_HLSL_PS_BASIC;
static const char s_ps_snap_src[] = NV2A_HLSL_PS_BASIC_SNAP;

/* Batches without a vertex program (nv2a_basic_hlsl.h). */
static const char s_vs_passthrough_src[] = NV2A_HLSL_VS_PASS;

static void create_black_texture(void);

static int init(void)
{
    static int tried;
    HMODULE dll;
    DXGI_SWAP_CHAIN_DESC sd;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr;
    WNDCLASSA wc;
    wchar_t title[192];
    /* display.aspect: a window of the padded frame's shape (854x480 at
     * 16:9), so the pads are on screen at 1:1. */
    RECT r = {0, 0, (LONG)nv2a_host_window_w(640, 480, 1), 480};
    int full = nv2a_host_opts()->fullscreen;
    int internal = nv2a_host_opts()->present_surface == NV2A_PRESENT_SURFACE_INTERNAL;
    UINT bw = (UINT)r.right, bh = 480;

    if (tried)
        return !s_failed;
    tried = 1;
    s_failed = 1;

    dll = LoadLibraryA("d3dcompiler_47.dll");
    if (dll)
        s_compile = (compile_fn)(void (*)(void))GetProcAddress(dll, "D3DCompile");
    if (!s_compile) {
        fprintf(stderr, LOGP "no D3DCompile in d3dcompiler_47.dll\n");
        return 0;
    }
    s_create_blob = (create_blob_fn)(void (*)(void))GetProcAddress(dll, "D3DCreateBlob");
    shader_cache_setup(dll);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = D3D11_WND_CLASS;
    RegisterClassA(&wc);
    if (!xbox_FramebufferWindowTitleText(title, 192, &s_title_clock))
        title[0] = 0;
    if (internal) {
        /* present.surface=internal: the surface is the frame, and a scaling
         * compositor (gamescope) fits it to its output with its own filter.
         * The screen size is not asked: under gamescope it is the output,
         * and a surface that large leaves FSR nothing to upscale. */
        bw = nv2a_host_window_w(640, 480, nv2a_host_render_scale());
        bh = 480u * nv2a_host_render_scale();
        r.right = (LONG)bw;
        r.bottom = (LONG)bh;
    } else if (full) {
        /* present.fullscreen: borderless over the primary monitor (no HWND
         * yet to ask MonitorFromWindow), no exclusive mode. */
        bw = (UINT)GetSystemMetrics(SM_CXSCREEN);
        bh = (UINT)GetSystemMetrics(SM_CYSCREEN);
        if (!bw || !bh) {
            bw = nv2a_host_window_w(640, 480, 1);
            bh = 480;
            full = 0;
        }
    }
    if (full) {
        s_hwnd = CreateWindowW(L"" D3D11_WND_CLASS, title,
                               WS_POPUP | WS_VISIBLE, 0, 0, (int)bw, (int)bh,
                               NULL, NULL, wc.hInstance, NULL);
    } else {
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        s_hwnd = CreateWindowW(L"" D3D11_WND_CLASS, title,
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               r.right - r.left, r.bottom - r.top,
                               NULL, NULL, wc.hInstance, NULL);
    }
    if (!s_hwnd) {
        fprintf(stderr, LOGP "CreateWindow failed (%lu)\n", GetLastError());
        return 0;
    }
    s_sync_interval = recomp_env_on(RENV_PRESENT_VSYNC) ? 1 : 0;
    if (recomp_env(RENV_PRESENT_VSYNC))
        fprintf(stderr, LOGP "present vsync %s (RECOMP_PRESENT_VSYNC=%s)\n",
                s_sync_interval ? "on" : "off", recomp_env(RENV_PRESENT_VSYNC));

    memset(&sd, 0, sizeof sd);
    sd.BufferDesc.Width = bw;
    sd.BufferDesc.Height = bh;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = s_hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                       NULL, 0, D3D11_SDK_VERSION, &sd,
                                       &s_swap, &s_dev, &fl, &s_ctx);
    if (FAILED(hr)) {
        fprintf(stderr, LOGP "D3D11CreateDeviceAndSwapChain hr=0x%08lX\n",
                (unsigned long)hr);
        return 0;
    }
    /* DXGI's default window association takes Alt+Enter as a fullscreen
     * toggle, and nothing here follows that switch. With the keyboard on,
     * Alt is a key players hold in play (a walk key) and Enter is START,
     * so a pause while walking would switch to exclusive fullscreen behind
     * the presenter's back. Stock runs (keyboard off) keep DXGI's
     * default. */
    if (xbox_InputKeyboardOn()) {
        IDXGIFactory *f = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetParent(s_swap, &IID_IDXGIFactory, (void **)&f)) && f) {
            IDXGIFactory_MakeWindowAssociation(f, s_hwnd, DXGI_MWA_NO_ALT_ENTER);
            IDXGIFactory_Release(f);
        }
    }
    IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&s_back);
    if (FAILED(ID3D11DeviceContext_QueryInterface(s_ctx, &IID_ID3D11DeviceContext1,
                                                  (void **)&s_ctx1)))
        s_ctx1 = NULL;
    s_back_w = s_made_w = bw;
    s_back_h = s_made_h = bh;

    s_cb_consts = make_buffer(NV2A_VSH_CONSTANTS * 16, D3D11_BIND_CONSTANT_BUFFER);
    s_cb_vp     = make_buffer(80, D3D11_BIND_CONSTANT_BUFFER);
    s_cb_ps     = make_buffer(sizeof(PsConsts), D3D11_BIND_CONSTANT_BUFFER);
    s_vb_size   = 8u << 20;
    s_vb        = make_buffer(s_vb_size, D3D11_BIND_VERTEX_BUFFER);
    s_ib_size   = NV_MAX_INDICES * 3u * 4u;  /* any accepted batch fits */
    s_ib        = make_buffer(s_ib_size, D3D11_BIND_INDEX_BUFFER);
    if (!s_cb_consts || !s_cb_vp || !s_cb_ps || !s_vb || !s_ib) {
        fprintf(stderr, LOGP "buffer creation failed\n");
        return 0;
    }
    {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        int c, f;
        /* The vertex programs hand D3D11 real clip-space positions (see the
         * oPos epilogue in d3d8_vsh.c: the title's divide by w is undone, w
         * kept), so the rasteriser clips triangles with vertices behind the
         * eye against the near plane instead of dropping them, as the CPU
         * path has to. Depth clipping stays on for that; NV2A depth runs
         * 0..zmax, which vp_scale.z maps onto D3D's 0..1. */
        rd.DepthClipEnable = TRUE;
        /* Culling as the CPU path does it (nv2a_pb_exec.c), from
         * SET_CULL_FACE_ENABLE / CULL_FACE / FRONT_FACE: rasterizer_state().
         * The vertex program's pixels map onto the target unflipped (y down
         * on both), so a triangle clockwise as displayed is clockwise to
         * D3D11 too. */
        for (c = 0; c < 3; c++)
            for (f = 0; f < 2; f++) {
                rd.CullMode = c == 0 ? D3D11_CULL_NONE
                            : c == 1 ? D3D11_CULL_BACK : D3D11_CULL_FRONT;
                rd.FrontCounterClockwise = f;
                rd.ScissorEnable = FALSE;
                ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs[c][f]);
                rd.ScissorEnable = TRUE;
                ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_sc[c][f]);
            }
    }
    {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthEnable = FALSE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s_ds_off);
    }
    {
        ID3DBlob *code = NULL;
        if (!compile("nv2a_ps_diffuse", s_ps_src, "ps_5_0", &code))
            return 0;
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                       ID3D10Blob_GetBufferSize(code), NULL,
                                       &s_ps);
        ID3D10Blob_Release(code);
    }

    create_black_texture();
    fprintf(stderr, LOGP "device up: feature level 0x%X, swap chain %ux%u\n",
            (unsigned)fl, s_back_w, s_back_h);
#ifdef XBOXRECOMP_ENHANCE
    nv2a_present_chain_host("D3D11");
#endif
    /* Only once the device is up: a failed init returns before any frame
     * is pumped, and a poster left behind would take pad_quit's close for
     * a window that never closes, and the quit would be lost. */
    recomp_quit_set(post_close);
    s_failed = 0;
    return 1;
}

/* ---- surfaces ------------------------------------------------------------- */

/* Moves whenever the render-target set could answer a texture lookup
 * (rt_texture) differently: a target made, grown, released or given another
 * format. setup_pixel's stage memo keys on it. */
static uint32_t s_rt_gen;

static void rt_release(RenderTarget *rt)
{
    s_rt_gen++;
    if (s_drawn == rt)
        s_drawn = NULL;
    if (rt->srv)
        ID3D11ShaderResourceView_Release(rt->srv);
    ID3D11RenderTargetView_Release(rt->rtv);
    ID3D11Texture2D_Release(rt->tex);
    memset(rt, 0, sizeof *rt);
}

/* ---- render-target ownership (nv2a_backend_common.h) ----------------------
 *
 * A target is the title's surface only while the guest memory under it holds
 * what it held when the target was made. A title that frees a target's
 * memory and loads something else there (a menu leaves 512x512 targets
 * behind whose megabytes the next level refills with textures) would otherwise get
 * the old image bound in place of the new texels. This backend never writes
 * guest memory, so a stale target is only dropped. The Metal backend's rule;
 * A/B: RECOMP_DEBUG=rt_alias_check=0. */
static uint64_t s_stale_drops, s_retired, s_evict_lost;
static uint64_t s_grows, s_partial_clears;   /* rt_grow, clear_region (as Metal's) */

static int alias_check_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_RT_ALIAS_CHECK);
        on = !(e && e[0] == '0');
    }
    return on;
}

/* Drop rt when the title has rewritten its memory; 1 if it was dropped. */
static int rt_drop_if_stale(RenderTarget *rt, const char *why)
{
    static int said;
    if (!alias_check_on() || !rt->tex || !rt->pitch
            || !nv2a_rt_own_stale(&rt->own, (const uint8_t *)xbox_GetMemoryOffset(),
                                  rt->addr, rt->pitch, rt->h, (uint32_t)s_presents))
        return 0;
    s_stale_drops++;
    if (said++ < 20)
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target dropped (%s): the title"
                " rewrote its %u bytes; last drawn at flip %u, now %llu\n",
                rt->offset, rt->w, rt->h, why, rt->pitch * rt->h, rt->own.draw_flip,
                (unsigned long long)s_presents);
    rt_release(rt);
    return 1;
}

/* rt made w x h (guest pixels), what it holds kept at its top left. The new
 * area starts black: the guest bytes are there, but this backend never seeds
 * a target from guest memory (the title clears or draws it first). */
static int rt_grow(RenderTarget *rt, UINT w, UINT h)
{
    static const float black[4] = {0, 0, 0, 0};
    static int said;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *tex = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    D3D11_BOX b;
    UINT hw, hh, pad, dx = 0;
    unsigned n;

    pad = nv2a_host_pad(w, h);
    n = nv2a_host_size(w + 2u * pad, h, nv2a_host_render_scale(), D3D11_MAX_DIM, &hw, &hh);
    memset(&td, 0, sizeof td);
    td.Width = hw;
    td.Height = hh;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &tex)))
        return 0;
    if (FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)tex,
                                                   NULL, &rtv))) {
        ID3D11Texture2D_Release(tex);
        return 0;
    }
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, rtv, black);
    /* The guest columns move with a change of hor+ padding (a 640x464
     * clip growing to 4:3 gains pads): column 0 stays column 0. */
    b.left = rt->pad > pad ? (rt->pad - pad) * n : 0;
    if (pad > rt->pad)
        dx = (pad - rt->pad) * n;
    b.top = 0;
    b.front = 0;
    b.right = rt->hw < hw - dx ? rt->hw : hw - dx;
    b.bottom = rt->hh < hh ? rt->hh : hh;
    b.back = 1;
    if (b.right > b.left)
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)tex, 0, dx, 0, 0,
                                                  (ID3D11Resource *)rt->tex, 0, &b);
    s_grows++;
    if (said++ < 20)
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target grown to %ux%u\n",
                rt->offset, rt->w, rt->h, w, h);
    if (rt->srv) {
        ID3D11ShaderResourceView_Release(rt->srv);
        rt->srv = NULL;
    }
    ID3D11RenderTargetView_Release(rt->rtv);
    ID3D11Texture2D_Release(rt->tex);
    rt->tex = tex;
    rt->rtv = rtv;
    s_rt_gen++;
    rt->w = w;
    rt->h = h;
    rt->hw = hw;
    rt->hh = hh;
    rt->pad = pad;
    rt->n = n;
    if (alias_check_on() && rt->pitch)
        nv2a_rt_own_reset(&rt->own, (const uint8_t *)xbox_GetMemoryOffset(),
                          rt->addr, rt->pitch, h, (uint32_t)s_presents);
    /* The draw path's state shadow may name the released view. */
    s_sc_init = 0;
    return 1;
}

static RenderTarget *surface(void)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    UINT w = g->clip_x + g->clip_w, h = g->clip_y + g->clip_h;
    RenderTarget *free_rt = NULL, *lru = &s_rt[0];
    int i;

    if (!g->color_offset || !w || !h)
        return NULL;
    s_tick++;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->tex && rt->offset == g->color_offset && rt->w == w && rt->h == h) {
            /* First use this flip of a target the title may have rewritten:
             * a stale one is made again. */
            if (rt_drop_if_stale(rt, "reuse"))
                break;
            rt->last_use = s_tick;
            if (rt->fmt != g->format)
                s_rt_gen++;
            rt->fmt = g->format;
            nv2a_rt_own_drawn(&rt->own, (uint32_t)s_presents);
            return rt;
        }
    }
    /* The same surface under another clip extent: one target per surface,
     * as large as any clip it was drawn with. A title can draw its 640x480
     * back buffer with clips of 640x464, 623x401, 159x344 and more in one
     * frame; a target per extent split the frame across textures, the
     * overlap rule below dropped each in turn (this backend writes nothing
     * back), and the flip found no target of the frame's size: a black
     * title screen and a frozen race. Draws map guest pixels to NDC through
     * rt->w/h, so a larger target keeps every position. What that asks of
     * the rest: a clear covers only its clear rect or clip (clear_region),
     * and a flip shows only the frame's extent (present_extent). Another
     * pitch at the same offset is a new allocation, not another clip: it
     * gets a target of its own and the overlap rule retires this one. */
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->tex || rt->offset != g->color_offset || rt->pitch != g->pitch)
            continue;
        if (rt_drop_if_stale(rt, "reuse"))
            break;
        if ((rt->w >= w && rt->h >= h)
                || rt_grow(rt, rt->w > w ? rt->w : w, rt->h > h ? rt->h : h)) {
            rt->last_use = s_tick;
            if (rt->fmt != g->format)
                s_rt_gen++;
            rt->fmt = g->format;
            nv2a_rt_own_drawn(&rt->own, (uint32_t)s_presents);
            return rt;
        }
        break;
    }
    /* No two live targets share memory: one the new target overlaps is
     * dropped (this backend has nothing to write back). */
    if (alias_check_on()) {
        uint32_t addr = nv2a_pb_dma_resolve(g->color_offset);
        for (i = 0; i < RT_MAX; i++) {
            RenderTarget *rt = &s_rt[i];
            if (rt->tex && nv2a_rt_overlap(rt->addr, rt->pitch * rt->h, addr, g->pitch * h)) {
                if (s_retired++ < 20)
                    fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target retired:"
                            " 0x%08X %ux%u overlaps it\n", rt->offset, rt->w, rt->h,
                            g->color_offset, w, h);
                rt_release(rt);
            }
        }
    }
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->tex && !free_rt)
            free_rt = rt;
        if (rt->last_use < lru->last_use)
            lru = rt;
    }
    if (!free_rt) {
        free_rt = lru;
        /* Nothing is written back: a target drawn into loses its pixels. */
        if (free_rt->last_draw && s_evict_lost++ < 20)
            fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target evicted for"
                    " 0x%08X, its pixels lost (RT_MAX %d full)\n", free_rt->offset,
                    free_rt->w, free_rt->h, g->color_offset, RT_MAX);
        rt_release(free_rt);
    }
    free_rt->pad = nv2a_host_pad(w, h);
    free_rt->n = nv2a_host_size(w + 2u * free_rt->pad, h, nv2a_host_render_scale(),
                                D3D11_MAX_DIM, &free_rt->hw, &free_rt->hh);
    {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = free_rt->hw;
        td.Height = free_rt->hh;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &free_rt->tex)))
            return NULL;
        if (FAILED(ID3D11Device_CreateRenderTargetView(
                    s_dev, (ID3D11Resource *)free_rt->tex, NULL, &free_rt->rtv))) {
            ID3D11Texture2D_Release(free_rt->tex);
            free_rt->tex = NULL;
            return NULL;
        }
    }
    s_rt_gen++;
    free_rt->offset = g->color_offset;
    free_rt->addr = nv2a_pb_dma_resolve(g->color_offset);
    free_rt->w = w;
    free_rt->h = h;
    free_rt->pitch = g->pitch;
    free_rt->fmt = g->format;
    free_rt->last_use = s_tick;
    if (alias_check_on() && free_rt->pitch)
        nv2a_rt_own_reset(&free_rt->own, (const uint8_t *)xbox_GetMemoryOffset(),
                          free_rt->addr, free_rt->pitch, h, (uint32_t)s_presents);
    nv2a_rt_own_drawn(&free_rt->own, (uint32_t)s_presents);
    if (free_rt->hw != w)
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target (%ux%u host)\n",
                g->color_offset, w, h, free_rt->hw, free_rt->hh);
    else
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target\n",
                g->color_offset, w, h);
    return free_rt;
}

/* ---- depth ------------------------------------------------------------------ */

/* One depth-stencil buffer per zeta offset and surface size. The depth half
 * is float, so the test compares the same normalised z (depth units / zmax)
 * the CPU path stores in its float buffer, rather than D24S8's 24-bit fixed
 * point; the 8 stencil bits are the guest's Z24S8 stencil. */
#define DT_MAX 4
typedef struct {
    uint32_t                zeta;
    UINT                    w, h;       /* guest size, as its colour target's */
    UINT                    hw, hh;     /* host size (render.scale, hor+ pads) */
    ID3D11Texture2D        *tex;
    ID3D11DepthStencilView *dsv;
    uint64_t                last_use;
} DepthTarget;

static DepthTarget s_dt[DT_MAX];
#define DSS_MAX 64
static struct { uint64_t key; ID3D11DepthStencilState *dss; } s_dss[DSS_MAX];
static int s_dss_count;
static uint64_t s_zclears, s_draws_z;

static DepthTarget *depth_target(UINT w, UINT h)
{
    uint32_t zeta = nv2a_pb_vsh_state()->zeta_offset;
    DepthTarget *e = NULL, *lru = &s_dt[0];
    D3D11_TEXTURE2D_DESC td;
    int i;

    for (i = 0; i < DT_MAX; i++) {
        DepthTarget *d = &s_dt[i];
        if (d->tex && d->zeta == zeta && d->w == w && d->h == h) {
            d->last_use = s_tick;
            return d;
        }
        if (!d->tex && !e)
            e = d;
        if (d->last_use < lru->last_use)
            lru = d;
    }
    if (!e) {
        e = lru;
        ID3D11DepthStencilView_Release(e->dsv);
        ID3D11Texture2D_Release(e->tex);
        memset(e, 0, sizeof *e);
    }
    nv2a_host_size(w + 2u * nv2a_host_pad(w, h), h, nv2a_host_render_scale(), D3D11_MAX_DIM,
                   &e->hw, &e->hh);
    memset(&td, 0, sizeof td);
    td.Width = e->hw;
    td.Height = e->hh;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &e->tex)))
        return NULL;
    if (FAILED(ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)e->tex,
                                                   NULL, &e->dsv))) {
        ID3D11Texture2D_Release(e->tex);
        e->tex = NULL;
        return NULL;
    }
    /* The CPU path starts its buffer at the far plane, stencil 0. */
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, e->dsv,
                                              D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                              1.0f, 0);
    e->zeta = zeta;
    e->w = w;
    e->h = h;
    e->last_use = s_tick;
    fprintf(stderr, LOGP "zeta 0x%08X: %ux%u depth buffer\n", zeta, w, h);
    return e;
}

/* GL's stencil op codes as D3D11_STENCIL_OP; anything else keeps, as on the
 * CPU. */
static uint32_t stencil_op(uint32_t op)
{
    return nv2a_stencil_op_from_gl(op) + D3D11_STENCIL_OP_KEEP;
}

/* SET_DEPTH_FUNC takes GL's codes, 0x200 NEVER .. 0x207 ALWAYS, in the order
 * D3D11_COMPARISON_* numbers them from 1, and SET_STENCIL_FUNC the same (the
 * walker keeps its low nibble). Anything else tests as ALWAYS, as on the CPU.
 * Depth and stencil each only when enabled; one state per distinct key. */
static ID3D11DepthStencilState *depth_state(int depth, int stencil)
{
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t f = nv2a_cmp_from_gl(v->depth_func);
    uint64_t key = 0;
    D3D11_DEPTH_STENCIL_DESC dd;
    int i;

    if (depth)
        key |= 1u | f << 1 | (v->depth_mask ? 1u : 0u) << 4;
    if (stencil)
        key |= (uint64_t)(1u | (v->stencil_func & 7u) << 1
                          | stencil_op(v->stencil_op[0]) << 4
                          | stencil_op(v->stencil_op[1]) << 8
                          | stencil_op(v->stencil_op[2]) << 12
                          | (v->stencil_rmask & 0xFFu) << 16
                          | (v->stencil_wmask & 0xFFu) << 24) << 8;
    for (i = 0; i < s_dss_count; i++)
        if (s_dss[i].key == key)
            return s_dss[i].dss;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = depth ? TRUE : FALSE;
    dd.DepthWriteMask = depth && v->depth_mask ? D3D11_DEPTH_WRITE_MASK_ALL
                                               : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = depth ? (D3D11_COMPARISON_FUNC)(f + 1) : D3D11_COMPARISON_ALWAYS;
    if (stencil) {
        dd.StencilEnable = TRUE;
        dd.StencilReadMask = (UINT8)v->stencil_rmask;
        dd.StencilWriteMask = (UINT8)v->stencil_wmask;
        dd.FrontFace.StencilFailOp = (D3D11_STENCIL_OP)stencil_op(v->stencil_op[0]);
        dd.FrontFace.StencilDepthFailOp = (D3D11_STENCIL_OP)stencil_op(v->stencil_op[1]);
        dd.FrontFace.StencilPassOp = (D3D11_STENCIL_OP)stencil_op(v->stencil_op[2]);
        dd.FrontFace.StencilFunc = (D3D11_COMPARISON_FUNC)((v->stencil_func & 7u) + 1);
        dd.BackFace = dd.FrontFace;             /* GL one-sided stencil */
    }
    if (s_dss_count >= DSS_MAX)
        return s_ds_off;
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s_dss[s_dss_count].dss)))
        return s_ds_off;
    s_dss[s_dss_count].key = key;
    return s_dss[s_dss_count++].dss;
}

/* CLEAR_SURFACE with the Z bit: the clear value scaled as the CPU path
 * scales it, by the zeta format's range. The stencil bit: its low byte. */
static void d3d11_on_zclear(uint32_t param)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    RenderTarget *rt;
    DepthTarget *dt;
    float z;

    if (!(param & 3u) || !init())
        return;
    rt = surface();
    if (!rt)
        return;
    dt = depth_target(rt->w, rt->h);
    if (!dt)
        return;
    z = nv2a_zclear_depth(g->format, v->zclear);
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, dt->dsv,
        ((param & 1u) ? D3D11_CLEAR_DEPTH : 0u) | ((param & 2u) ? D3D11_CLEAR_STENCIL : 0u),
        z, (UINT8)(v->zclear & 0xFFu));
    if (param & 1u)
        s_zclears++;
}

static void clear_region(RenderTarget *rt, const struct nv2a_pb_gpu *g, const float c[4])
{
    uint32_t b[4];
    UINT x0, y0, x1, y1;
    D3D11_RECT r;

    /* The clear rect, else the clip (nv2a_clear_box): one target serves every
     * clip of its surface (rt_grow), so a whole-target clear would wipe the
     * parts of the frame another clip drew. */
    if (!nv2a_clear_box(g, rt->w, rt->h, b))
        return;
    x0 = b[0]; y0 = b[1]; x1 = b[2]; y1 = b[3];
    if ((x0 == 0 && y0 == 0 && x1 == rt->w && y1 == rt->h) || !s_ctx1) {
        static int said;
        if (!s_ctx1 && !said++ && !(x0 == 0 && y0 == 0 && x1 == rt->w && y1 == rt->h))
            fprintf(stderr, LOGP "no D3D 11.1 context: a partial clear clears the"
                    " whole target\n");
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, rt->rtv, c);
        return;
    }
    /* Host pixels, as a 3D draw's clip: the guest columns pad*n in, an
     * edge at 0 or w reaching the hor+ pads (nv2a_aspect_box). */
    {
        uint32_t hb[4];
        nv2a_aspect_box(b, rt->w, rt->h, rt->pad, rt->n, NV2A_PLACE_CENTRE, 0, 0, hb);
        r.left = (LONG)hb[0];
        r.top = (LONG)hb[1];
        r.right = (LONG)hb[2];
        r.bottom = (LONG)hb[3];
    }
    ID3D11DeviceContext1_ClearView(s_ctx1, (ID3D11View *)rt->rtv, c, &r, 1);
    s_partial_clears++;
}

static void d3d11_on_clear(uint32_t param)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    RenderTarget *rt;
    float c[4];
    uint32_t argb;

    if (!(param & 0xF0) || !init())
        return;
    rt = surface();
    if (!rt)
        return;
    argb = nv2a_clear_argb(g);   /* in the surface's format */
    c[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
    c[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
    c[2] = (float)(argb & 0xFF) / 255.0f;
    c[3] = (float)(argb >> 24) / 255.0f;
    clear_region(rt, g, c);
    rt->last_clear = ++s_seq;
    s_clears++;
}

/* ---- vertices -------------------------------------------------------------- */

/* One attribute of one vertex as float4, decoded the way the CPU path's
 * fetch_attr does. 0 if the stream has no data for it. */
static int fetch(const struct nv2a_pb_draw *d, const VertexAttr *a,
                 uint32_t index, float out[4])
{
    const uint8_t *p;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (d->inline_data) {
        size_t at = (size_t)a->offset + (size_t)index * a->stride;
        if (at + 4 > d->inline_bytes)
            return 0;
        p = (const uint8_t *)d->inline_buf + at;
    } else {
        if (!a->offset)
            return 0;
        p = (const uint8_t *)xbox_GetMemoryOffset() + a->offset
          + (size_t)index * a->stride;
    }
    return nv2a_vtx_decode(a->type, a->size, p, out);
}

/* Input i of one vertex as the shader sees it: the stream's element, its
 * one element at stride 0, or the SET_VERTEX_DATA value when it has none
 * (the stream upload in d3d11_on_draw, for nv2a_aspect_scan). */
static void vtx_input(const struct nv2a_pb_draw *d, const struct nv2a_pb_vsh *v,
                      unsigned i, uint32_t index, float out[4])
{
    const VertexAttr *a = &d->attr[i];

    if (a->size && a->stride ? fetch(d, a, index, out) : a->size && fetch(d, a, 0, out))
        return;
    memcpy(out, v->inl[i], 16);
}

/* The DXGI format a stream can be read in as it is, so its bytes are copied
 * rather than expanded to float4 (the shader's float4 input gets the same
 * values fetch() would make, missing components (0,0,0,1)). 0 for the types
 * and sizes that have none: S32K (integers read as floats), CMP, and
 * three-component bytes and shorts. *bytes: the size of one element. */
static DXGI_FORMAT native_format(const VertexAttr *a, UINT *bytes)
{
    static const DXGI_FORMAT f32[5] = {0, DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT,
        DXGI_FORMAT_R32G32B32A32_FLOAT};
    static const DXGI_FORMAT u8[5] = {0, DXGI_FORMAT_R8_UNORM,
        DXGI_FORMAT_R8G8_UNORM, 0, DXGI_FORMAT_R8G8B8A8_UNORM};
    static const DXGI_FORMAT s16[5] = {0, DXGI_FORMAT_R16_SNORM,
        DXGI_FORMAT_R16G16_SNORM, 0, DXGI_FORMAT_R16G16B16A16_SNORM};

    if (a->size < 1 || a->size > 4)
        return 0;
    switch (a->type) {
    case NV2A_VTX_D3DCOLOR: *bytes = 4;           return DXGI_FORMAT_B8G8R8A8_UNORM;
    case NV2A_VTX_FLOAT:    *bytes = 4 * a->size; return f32[a->size];
    case NV2A_VTX_UBYTE:    *bytes = a->size;     return u8[a->size];
    case NV2A_VTX_S1:       *bytes = 2 * a->size; return s16[a->size];
    default:                return 0;
    }
}

static void *ring_map(ID3D11Buffer *b, UINT size, UINT *pos, UINT need,
                      UINT align)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    UINT at = (*pos + align - 1) / align * align;

    if (need > size)
        return NULL;
    if (at + need > size) {
        at = 0;
        how = D3D11_MAP_WRITE_DISCARD;
    }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)b, 0, how, 0, &m)))
        return NULL;
    *pos = at + need;
    return (uint8_t *)m.pData + at;
}

/* A batch's vertex streams, written through one Map of s_vb: vb_begin
 * reserves an upper bound for all of them, vb_alloc hands out 16-aligned
 * pieces of it, vb_end unmaps and gives back what was not used. Without a
 * reservation (memos off, or the bound too large) each piece is its own
 * ring_map, as before. One mapping also means a wrap's DISCARD cannot land
 * between two streams of the same batch. */
static uint8_t *s_vbm_base;
static UINT     s_vbm_at, s_vbm_used, s_vbm_cap;

static void vb_begin(UINT bound)
{
    s_vbm_base = NULL;
    if (!memo_on() || !bound || bound > s_vb_size / 4)
        return;
    s_vbm_base = (uint8_t *)ring_map(s_vb, s_vb_size, &s_vb_pos, bound, 16);
    if (s_vbm_base) {
        s_vbm_at = s_vb_pos - bound;
        s_vbm_used = 0;
        s_vbm_cap = bound;
    }
}

static void *vb_alloc(UINT bytes, UINT *off)
{
    void *p;
    if (s_vbm_base) {
        UINT o = (s_vbm_used + 15u) & ~15u;
        if (o + bytes > s_vbm_cap)
            return NULL;
        s_vbm_used = o + bytes;
        *off = s_vbm_at + o;
        return s_vbm_base + o;
    }
    p = ring_map(s_vb, s_vb_size, &s_vb_pos, bytes, 16);
    if (p)
        *off = s_vb_pos - bytes;
    return p;
}

/* After each piece is written: unmapped now unless it is part of a batch. */
static void vb_written(void)
{
    if (!s_vbm_base)
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
}

static void vb_end(void)
{
    if (s_vbm_base) {
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
        s_vb_pos = s_vbm_at + s_vbm_used;
        s_vbm_base = NULL;
    }
}

/* Triangle, line and point lists from any NV097 primitive. Returns the
 * index count (into out, rebased by -base) and the topology. */
static uint32_t convert_indices(const struct nv2a_pb_draw *d, uint32_t base,
                                uint32_t *out, D3D11_PRIMITIVE_TOPOLOGY *topo)
{
    enum nv2a_topology t;
    uint32_t k = nv2a_prim_to_list(d->prim, d->idx, d->idx_count, base, out, &t);
    *topo = t == NV2A_TOPO_POINTS ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST
          : t == NV2A_TOPO_LINES  ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
          :                         D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    return k;
}

/* ---- shaders --------------------------------------------------------------- */

static uint32_t program_hash(const struct nv2a_pb_vsh *v)
{
    uint32_t h = nv2a_vsh_program_hash(v, NULL);
    return h ? h : 1;
}

static VsEntry *vertex_shader(int program)
{
    static NV2AVshProgram parsed;
    static char hlsl[65536];
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t hash = 0;
    VsEntry *e = NULL;
    ID3DBlob *code = NULL;
    const char *src;
    char name[32];
    int i;
    static struct { int valid, program; uint32_t dwords, start; VsEntry *e; } memo;

    if (memo_on() && memo.valid && memo.program == program
            && memo.dwords == v->prog_dwords && memo.start == v->start)
        return memo.e;
    if (program)
        hash = program_hash(v);
    for (i = 0; i < VS_MAX; i++) {
        if (s_vs[i].used && s_vs[i].hash == hash) {
            memo.valid = 1;
            memo.program = program;
            memo.dwords = v->prog_dwords;
            memo.start = v->start;
            memo.e = s_vs[i].bad ? NULL : &s_vs[i];
            return memo.e;
        }
        if (!s_vs[i].used && !e)
            e = &s_vs[i];
    }
    if (!e)
        return NULL;    /* full; a title with this many programs wants LRU */
    e->used = 1;
    e->hash = hash;
    e->bad = 1;
    e->hp.two_d = -1;
    e->hp.n = 0;

    if (program) {
        d3d8_vsh_parse(&v->prog[v->start][0],
                       NV2A_VSH_SLOTS - (int)v->start, &parsed);
        if (d3d8_vsh_generate_hlsl_ex(&parsed, NV2A_VSH_HLSL_SCREEN_SPACE,
                                      hlsl, sizeof hlsl) <= 0) {
            fprintf(stderr, LOGP "program %08X: no HLSL\n", hash);
            return NULL;
        }
        src = hlsl;
        e->inputs = parsed.inputs_read;
        snprintf(name, sizeof name, "nv2a_vsh_%08X", hash);
    } else {
        src = s_vs_passthrough_src;
        e->inputs = (1u << 0) | (1u << 3) | (1u << 4) | (0xFu << 9);
        snprintf(name, sizeof name, "nv2a_vsh_passthrough");
    }
    if (!compile(name, src, "vs_5_0", &code))
        return NULL;
    if (FAILED(ID3D11Device_CreateVertexShader(
                s_dev, ID3D10Blob_GetBufferPointer(code),
                ID3D10Blob_GetBufferSize(code), NULL, &e->vs))) {
        ID3D10Blob_Release(code);
        return NULL;
    }
    e->code = code;     /* input layouts come per draw: input_layout() */
    fprintf(stderr, LOGP "compiled %s (inputs 0x%04X)\n", name, e->inputs);
    e->bad = 0;
    return e;
}

/* The input layout for a shader and the formats its streams arrive in this
 * batch (one input slot per ATTR), made the first time that pair is seen.
 * NULL if the shader reads nothing. */
static ID3D11InputLayout *input_layout(VsEntry *e, const uint8_t fmt[16])
{
    D3D11_INPUT_ELEMENT_DESC el[NV2A_VS_MAX_INPUTS];
    int i, n, k;

    for (k = 0; k < 8; k++)
        if (e->lay[k].il && !memcmp(e->lay[k].fmt, fmt, 16))
            return e->lay[k].il;
    for (i = 0, n = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (!(e->inputs & (1u << i)))
            continue;
        memset(&el[n], 0, sizeof el[n]);
        el[n].SemanticName = "ATTR";
        el[n].SemanticIndex = (UINT)i;
        el[n].Format = (DXGI_FORMAT)fmt[i];
        el[n].InputSlot = (UINT)i;
        el[n].AlignedByteOffset = 0;
        el[n].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        n++;
    }
    if (!n)
        return NULL;
    k = e->lay_next;
    e->lay_next = (k + 1) % 8;
    if (e->lay[k].il) {
        ID3D11InputLayout_Release(e->lay[k].il);
        s_state_gen++;
    }
    e->lay[k].il = NULL;
    memcpy(e->lay[k].fmt, fmt, 16);
    if (FAILED(ID3D11Device_CreateInputLayout(
                s_dev, el, (UINT)n, ID3D10Blob_GetBufferPointer(e->code),
                ID3D10Blob_GetBufferSize(e->code), &e->lay[k].il))) {
        fprintf(stderr, LOGP "vs %08X: CreateInputLayout failed\n", e->hash);
        e->lay[k].il = NULL;
    }
    return e->lay[k].il;
}

/* ---- rasteriser state ----------------------------------------------------- */

/* The state for the pgraph cull setting: CULL_FACE 0x404 FRONT, 0x405 BACK,
 * 0x408 FRONT_AND_BACK; FRONT_FACE 0x900 CW, 0x901 CCW. *all is set when the
 * draw culls every triangle (FRONT_AND_BACK), which D3D11 cannot express.
 * Character shadow volumes need it: each body part's shadow volume is
 * drawn twice into destination alpha, back faces (CULL_FRONT) added and
 * front faces (CULL_BACK) reverse-subtracted, which leaves alpha only where
 * the floor lies inside a volume. With no culling both passes cover the
 * whole volume and its screen footprint comes out as one dark wedge. */
static ID3D11RasterizerState *rasterizer_state(int *all)
{
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    static int off = -1;
    int c = 0, f = v->front_face != 0x900;

    static int flip = -1;
    if (off < 0)
        off = recomp_env(RENV_D3D11_NO_CULL) != NULL;  /* A/B: cull nothing */
    if (flip < 0)
        flip = recomp_env(RENV_D3D11_CULL_FLIP) != NULL;  /* A/B: other winding */
    f ^= flip;
    *all = 0;
    if (v->cull_enable && !off) {
        if (v->cull_face == 0x405)      c = 1;
        else if (v->cull_face == 0x404) c = 2;
        else if (v->cull_face == 0x408) *all = 1;
    }
    return s_rs_sc[c][f];
}

/* ---- blending -------------------------------------------------------------- */

/* SET_BLEND_EQUATION as a D3D11 blend op. The signed variants (0xF005
 * REVERSE_SUBTRACT_SIGNED, 0xF006 ADD_SIGNED) have no D3D11 form and fall
 * back to their unsigned ones. */
static D3D11_BLEND_OP blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B:
    case 0xF005: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_BLEND blend_factor(uint32_t gl, int alpha)
{
    static const D3D11_BLEND map[NV2A_BF_UNKNOWN + 1] = {
        D3D11_BLEND_ZERO, D3D11_BLEND_ONE,
        D3D11_BLEND_SRC_COLOR, D3D11_BLEND_INV_SRC_COLOR,
        D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA,
        D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_DEST_ALPHA,
        D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_DEST_COLOR,
        D3D11_BLEND_SRC_ALPHA_SAT,
        /* No CONSTANT_ALPHA: blend_color() puts the right value in. */
        D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR,
        D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR,
        D3D11_BLEND_ONE,                        /* unknown */
    };
    uint32_t bf = nv2a_blend_from_gl(gl);
    return map[alpha ? nv2a_blend_for_alpha(bf) : bf];
}

static ID3D11BlendState *blend_state(void)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    uint32_t key[5];
    D3D11_BLEND_DESC bd;
    ID3D11BlendState *bs = NULL;
    int i;

    key[0] = g->blend_enable ? 1 : 0;
    key[1] = key[0] ? g->blend_sfactor : 0;
    key[2] = key[0] ? g->blend_dfactor : 0;
    key[3] = g->color_keep;             /* SET_COLOR_MASK, inverted */
    key[4] = key[0] ? (uint32_t)blend_op(g->blend_equation) : 0;
    for (i = 0; i < s_blend_n; i++)
        if (!memcmp(s_blend[i].key, key, sizeof key))
            return s_blend[i].bs;
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = key[0];
    bd.RenderTarget[0].SrcBlend = blend_factor(key[1], 0);
    bd.RenderTarget[0].DestBlend = blend_factor(key[2], 0);
    bd.RenderTarget[0].BlendOp = key[0] ? (D3D11_BLEND_OP)key[4] : D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = blend_factor(key[1], 1);
    bd.RenderTarget[0].DestBlendAlpha = blend_factor(key[2], 1);
    bd.RenderTarget[0].BlendOpAlpha = bd.RenderTarget[0].BlendOp;
    bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)(
        ((key[3] & 0x00FF0000u) ? 0 : D3D11_COLOR_WRITE_ENABLE_RED)
      | ((key[3] & 0x0000FF00u) ? 0 : D3D11_COLOR_WRITE_ENABLE_GREEN)
      | ((key[3] & 0x000000FFu) ? 0 : D3D11_COLOR_WRITE_ENABLE_BLUE)
      | ((key[3] & 0xFF000000u) ? 0 : D3D11_COLOR_WRITE_ENABLE_ALPHA));
    if (!key[0]) {
        bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    }
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &bd, &bs)))
        return NULL;
    if (s_blend_n < BLEND_MAX) {
        i = s_blend_n++;
    } else {
        /* Full: replace one round robin, as the sampler cache does. A
         * released state's address can come back, so the bound-state
         * filter starts over. */
        static unsigned next;
        i = (int)(next++ % BLEND_MAX);
        ID3D11BlendState_Release(s_blend[i].bs);
        s_state_gen++;
    }
    memcpy(s_blend[i].key, key, sizeof key);
    s_blend[i].bs = bs;
    return bs;
}

/* The factor D3D11_BLEND_BLEND_FACTOR reads: SET_BLEND_COLOR, unpacked
 * from ARGB to RGBA. D3D11 has no CONSTANT_ALPHA, so when only the
 * _ALPHA factors are in use the alpha is replicated into all four; a
 * draw mixing _COLOR and _ALPHA keeps the colour (logged once). With
 * no constant factor the default (1,1,1,1) is used, so a colour the title
 * sets without using it does not rebind the blend state. Returns whether a
 * constant factor is in use. */
static int blend_color(float bf[4])
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    int mixed, on = nv2a_blend_constant(g->blend_enable, g->blend_sfactor,
                                        g->blend_dfactor, g->blend_color,
                                        bf, &mixed);
    if (mixed) {
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, LOGP "blend: CONSTANT_COLOR and CONSTANT_ALPHA mixed"
                    " (sfactor 0x%04X dfactor 0x%04X); the _ALPHA side uses the"
                    " colour\n", g->blend_sfactor, g->blend_dfactor);
        }
    }
    return on;
}

/* ---- textures -------------------------------------------------------------
 *
 * Four stages, decoded from the texture registers the title last wrote, the
 * way the CPU path's vsh_build_stages does: offset, format, size (from the
 * format word for swizzled and DXT, from IMAGE_RECT for linear), pitch,
 * wrap modes, enable, and a texture-shader mode from SET_SHADER_STAGE_PROGRAM.
 *
 * Each texture is decoded to A8R8G8B8 on the CPU -- the same per-format
 * arithmetic as the CPU sampler, so both paths see the same texels -- and
 * uploaded as B8G8R8A8_UNORM (the same bytes). Cache key and invalidation
 * follow nv2a_pb_state.h: offset + format + width + height + pitch; on a new
 * flip generation the whole texture is hashed again and re-uploaded if it
 * changed (the letterboxed movie changes only in the middle).
 *
 * Mipmaps: a swizzled or DXT texture brings the levels SET_TEXTURE_FORMAT
 * counts (clamped as xemu does: to CONTROL0's MAX_LOD_CLAMP + 1 and to the
 * level that reaches 1x1), packed one after another, each swizzled or
 * block-compressed at its own size (DXT levels round up to whole 4x4
 * blocks). The level count is part of the cache key and every level is in
 * the hash. Linear textures have one level. RECOMP_D3D11_NO_MIPS=1 uploads
 * level 0 only, for A/B.
 *
 * ponytail: DXT is decoded rather than uploaded as BC1-3; a texture
 * rewritten between two batches of one flip is not seen; cube maps and 3D
 * textures are face/slice 0 only. */

static Stage s_stage[4];
/* What each stage's SRV came from, for the batch trace. */
static const char   *s_stage_src[4];
static RenderTarget *s_stage_rt[4];

#define TEX_MAX_TEXELS (2048u * 2048u)
#define TEX_CACHE 512   /* array bound; entries in use: tex_cache_n() */
typedef struct {
    uint32_t offset, color, width, height, pitch, palette, levels;
    uint32_t gen, used;
    uint32_t pack_gen;      /* nv2a_tex_pack_gen() it was uploaded under */
    uint64_t hash;
    int      live;
    /* What tex holds: the guest size, or a pack entry's (textures.pack);
     * a re-upload of another size or format makes a new texture. */
    uint32_t tex_w, tex_h, tex_levels;
    int      tex_bc3;
    ID3D11Texture2D          *tex;
    ID3D11ShaderResourceView *srv;
} TexEntry;

static TexEntry  s_tc[TEX_CACHE];

/* Entries in use: RECOMP_DEBUG=tex_cache=n, 1..TEX_CACHE. Default 512: a
 * busy frame binds ~100 textures and 64 thrashed. */
static uint32_t tex_cache_n(void)
{
    static long n = -1;
    if (n < 0) {
        n = recomp_env_int(RENV_TEX_CACHE, 512);
        if (n < 1) n = 1;
        if (n > TEX_CACHE) n = TEX_CACHE;
    }
    return (uint32_t)n;
}
static uint32_t  s_tc_clock;
/* Key hash -> entry + 1, checked before the scan. A live entry with a key
 * is the only one (one is made only when the scan finds none), so a hint
 * that matches is the entry the scan would find; a stale or colliding one
 * just falls through to it. */
#define TC_HINTS 2048
static uint16_t  s_tc_hint[TC_HINTS];

static uint32_t tc_slot(const Stage *t)
{
    uint32_t h = t->offset * 0x9E3779B1u;
    h ^= (t->color + (t->width << 8) + (t->height << 20)) * 0x85EBCA77u;
    h ^= (t->pitch ^ t->palette ^ (t->levels << 27)) * 0xC2B2AE3Du;
    return (h >> 16) & (TC_HINTS - 1);
}
static uint32_t *s_texels;
static size_t    s_texels_cap;
static ID3D11ShaderResourceView *s_black_srv;   /* too large: (0,0,0,1) */
static ID3D11ShaderResourceView *s_magenta_srv; /* format not decoded */

static int no_mips(void)
{
    static int off = -1;
    if (off < 0)
        off = recomp_env(RENV_D3D11_NO_MIPS) != NULL;
    return off;
}

/* Moves each time build_stages decodes s_stage again. */
static uint32_t s_stage_gen;

static void build_stages(void)
{
    extern uint32_t xbox_ContiguousAllocatedBytes(void);
    const uint32_t *regs = nv2a_pb_tex_regs();
    const uint8_t  *sets = nv2a_pb_tex_regs_set();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    /* The stages are a function of the texture registers, the shader
     * program and the DMA resolve, whose answer moves only with the
     * contiguous arena's high-water mark. Unchanged, the last decode
     * stands: ~1.3 ms a flip on the Deck at 10k batches. */
    static struct { uint32_t gen, prog, contig; int set, ok; } memo;
    uint32_t gen = nv2a_pb_tex_regs_gen(), contig = xbox_ContiguousAllocatedBytes();
    int n;

    if (memo_on() && memo.ok && memo.gen == gen && memo.prog == v->shader_prog
        && memo.set == v->shader_set && memo.contig == contig)
        return;
    memo.gen = gen;
    memo.prog = v->shader_prog;
    memo.set = v->shader_set;
    memo.contig = contig;
    memo.ok = 1;
    s_stage_gen++;
    for (n = 0; n < 4; n++) {
        Stage *t = &s_stage[n];
        int mode;

        nv2a_stage_decode(n, &regs[(0x40u * n) / 4], &sets[(0x40u * n) / 4],
                          v->shader_set, v->shader_prog, nv2a_pb_dma_resolve,
                          NV2A_STAGE_P8_PALETTE | NV2A_STAGE_PITCH_LINEAR
                          | (no_mips() ? 0u : NV2A_STAGE_MIPS), t);
        mode = t->raw_mode;
        if (mode && mode != 4 && (!t->on || !t->valid)) {
            /* The program asks for a texture this stage cannot give: it
             * reads (0, 0, 0, 1) now, as a NONE stage. Said once per stage, mode and format, since a
             * world drawn in flat vertex colour starts exactly here. */
            static uint8_t said[4][32][32];
            uint8_t bit = (uint8_t)(1u << (t->color & 7));
            if (!(said[n][mode & 31][(t->color >> 3) & 31] & bit)) {
                said[n][mode & 31][(t->color >> 3) & 31] |= bit;
                fprintf(stderr, LOGP "\x1b[1;35mstage %d: mode %d with no texture"
                        " (enabled %d, offset 0x%08X, format 0x%02X, %ux%u, pitch %u)"
                        "\x1b[0m\n", n, mode, t->on, t->offset, t->color,
                        t->width, t->height, t->pitch);
            }
        }
        if (t->mode == 5)
            t->mode = 0;    /* CLIP_PLANE: no texture, the register reads 0;
                             * the kill is clip_mode's (nv2a_ps_consts_fill) */
    }
}

/* Bytes of guest memory the texture occupies; 0 if it cannot be decoded. */
static uint32_t tex_extent(const Stage *t)
{
    if (!t->valid || (uint64_t)t->width * t->height > TEX_MAX_TEXELS)
        return 0;
    return nv2a_tex_extent(t->color, t->width, t->height, t->pitch, t->levels);
}

static uint64_t tex_hash(const uint8_t *p, uint32_t n)
{
    return nv2a_tex_hash(p, n, NV2A_TEX_HASH_SEED);
}

/* The whole texture into s_texels (A8R8G8B8): level 0 (width x height),
 * then each further level packed after it. */
static int decode_texture(const Stage *t)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset() + t->offset;
    size_t n = nv2a_tex_texels(t->width, t->height, t->levels);
    const uint32_t *pal = NULL;

    if (n > s_texels_cap) {
        uint32_t *p = (uint32_t *)realloc(s_texels, n * 4);
        if (!p)
            return 0;
        s_texels = p;
        s_texels_cap = n;
    }
    if (t->color == NV2A_TEX_P8)
        pal = (const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset()
                                 + t->palette);
    nv2a_tex_decode(mem, t->color, t->width, t->height, t->pitch, t->levels,
                    pal, pal ? t->pal_len : 0, s_texels);
    return 1;
}

/* e's texture at this size: a new one is made before the old one is
 * released, so a refusal leaves e as it was. `bc3`: BC3 blocks (a pack
 * entry's payload, XRTP_FLAG_BC3) rather than A8R8G8B8 texels. */
static int tex_alloc(TexEntry *e, uint32_t width, uint32_t height, uint32_t levels,
                     int bc3)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *tex = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    if (e->tex && e->tex_w == width && e->tex_h == height && e->tex_levels == levels
            && e->tex_bc3 == bc3)
        return 1;
    memset(&td, 0, sizeof td);
    td.Width = width;
    td.Height = height;
    td.MipLevels = levels;
    td.ArraySize = 1;
    td.Format = bc3 ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &tex)))
        return 0;
    if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)tex, NULL, &srv))) {
        ID3D11Texture2D_Release(tex);
        return 0;
    }
    if (e->srv)
        ID3D11ShaderResourceView_Release(e->srv);
    if (e->tex)
        ID3D11Texture2D_Release(e->tex);
    e->tex = tex;
    e->srv = srv;
    e->tex_w = width;
    e->tex_h = height;
    e->tex_levels = levels;
    e->tex_bc3 = bc3;
    return 1;
}

/* `hash` is the entry's texel hash: with a texture pack loaded
 * (enhancements, textures.pack) an entry it holds is uploaded from the
 * pack's texels at the pack's size instead of the guest's. If the device
 * refuses a texture of the entry's size, the guest texels go up instead:
 * the pack only ever adds detail, never a black texture. */
/* Moves whenever a cache entry's view may change (an upload or a free), so
 * a view remembered from tex_bind is never one since released. */
static uint32_t s_tc_gen;
/* The entry tex_bind's last view came from (NULL for magenta or black). */
static TexEntry *s_tex_last;

static int tex_upload(TexEntry *e, const Stage *t, uint64_t hash)
{
    s_tc_gen++;
    const uint32_t *src = NULL;
    const uint8_t *p;
    uint32_t width = t->width, height = t->height, levels = t->levels;
    uint32_t l;
    int bc3 = 0;
    if (nv2a_tex_pack_count()) {
        struct nv2a_tex_pack_entry *pe = nv2a_tex_pack_find(hash, t->width, t->height);
        if (pe && (src = nv2a_tex_pack_texels(pe)) != NULL) {
            width = pe->width;
            height = pe->height;
            levels = pe->levels;
            bc3 = pe->bc3;
        } else {
            nv2a_tex_pack_miss(hash, t->width, t->height, t->color, t->levels);
        }
    }
    if (src && !tex_alloc(e, width, height, levels, bc3)) {
        /* A refused entry is a miss too, tagged so in the trace. */
        nv2a_tex_pack_miss(hash, t->width, t->height, t->color, t->levels);
        src = NULL;
        width = t->width;
        height = t->height;
        levels = t->levels;
        bc3 = 0;
    }
    if (!src) {
        if (!decode_texture(t) || !tex_alloc(e, width, height, levels, 0))
            return 0;
        src = s_texels;
    }
    /* BC3 levels go up as they are, one row of 4x4 blocks per pitch. */
    p = (const uint8_t *)src;
    for (l = 0; l < levels; l++) {
        uint32_t w = nv2a_tex_level_dim(width, l), h = nv2a_tex_level_dim(height, l);
        uint32_t row = bc3 ? (w + 3u) / 4u * 16u : w * 4u;
        uint32_t rows = bc3 ? (h + 3u) / 4u : h;
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)e->tex,
                                              l, NULL, p, row, 0);
        p += (size_t)row * rows;
    }
    s_tex_uploads++;
    nv2a_stats_add(NV2A_STAT_TEX_UPLOADS, 1);
    if (levels > 1)
        s_tex_mip_uploads++;
    return 1;
}

static void tex_free(TexEntry *e)
{
    s_tc_gen++;
    if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
    if (e->tex) ID3D11Texture2D_Release(e->tex);
    memset(e, 0, sizeof *e);
}

/* The texels' hash, and for P8 the palette's folded in: a palette swap is a
 * new texture. */
static uint64_t tex_hash_pal(const uint8_t *mem, const Stage *t, uint32_t bytes)
{
    uint64_t h = tex_hash(mem + t->offset, bytes);
    if (t->color == 0x0B)
        h ^= tex_hash(mem + t->palette, t->pal_len * 4u) * 31u;
    return h;
}

/* The view for a stage's texture, decoded and uploaded if it is new or its
 * bytes changed since it was last checked (once per flip). */
static ID3D11ShaderResourceView *tex_bind(const Stage *t)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bytes = tex_extent(t), i;
    TexEntry *e = NULL, *victim = &s_tc[0];
    uint32_t slot;

    s_tex_last = NULL;

    if (!bytes) {
        /* A format with no decoder is magenta, and said once, loudly: white
         * or black passes for a texture that decoded to that. */
        static uint8_t said[256];
        if ((uint64_t)t->width * t->height > TEX_MAX_TEXELS)
            return s_black_srv;
        if (!said[t->color & 0xFF]) {
            said[t->color & 0xFF] = 1;
            fprintf(stderr, LOGP "\x1b[1;35mtexture format 0x%02X (%ux%u at"
                    " 0x%08X) not decoded: drawn magenta\x1b[0m\n",
                    t->color, t->width, t->height, t->offset);
        }
        return s_magenta_srv ? s_magenta_srv : s_black_srv;
    }
    slot = tc_slot(t);
    if (memo_on() && s_tc_hint[slot]) {
        TexEntry *c = &s_tc[s_tc_hint[slot] - 1];
        if (c->live && c->offset == t->offset && c->color == t->color
                && c->width == t->width && c->height == t->height
                && c->pitch == t->pitch && c->palette == t->palette
                && c->levels == t->levels)
            e = c;
    }
    uint32_t n = tex_cache_n();
    for (i = 0; !e && i < n; i++) {
        TexEntry *c = &s_tc[i];
        if (c->live && c->offset == t->offset && c->color == t->color
                && c->width == t->width && c->height == t->height
                && c->pitch == t->pitch && c->palette == t->palette
                && c->levels == t->levels) {
            e = c;
            break;
        }
        if (!c->live || (victim->live && c->used < victim->used))
            victim = c;
    }
    if (e) {
        /* A pack toggle (nv2a_tex_pack_gen) changes what the same texels
         * upload as: re-upload as if the hash had moved. */
        uint32_t pg = nv2a_tex_pack_gen();
        if (e->gen != g->flips || e->pack_gen != pg) {
            uint64_t h = tex_hash_pal(mem, t, bytes);
            s_tex_rehash++;
            e->gen = g->flips;
            if (h != e->hash || e->pack_gen != pg) {
                s_tex_changed++;
                /* The hash moves only with the texels: a failed upload keeps
                 * the old texture and is tried again next flip. */
                if (!tex_upload(e, t, h))
                    return s_black_srv;
                e->hash = h;
                e->pack_gen = pg;
            }
        }
    } else {
        e = victim;
        tex_free(e);
        e->offset = t->offset;
        e->color = t->color;
        e->width = t->width;
        e->height = t->height;
        e->pitch = t->pitch;
        e->palette = t->palette;
        e->levels = t->levels;
        e->gen = g->flips;
        e->hash = tex_hash_pal(mem, t, bytes);
        e->pack_gen = nv2a_tex_pack_gen();
        if (!tex_upload(e, t, e->hash)) {
            tex_free(e);
            return s_black_srv;
        }
        e->live = 1;
    }
    e->used = ++s_tc_clock;
    s_tc_hint[slot] = (uint16_t)(e - s_tc + 1);
    s_tex_last = e;
    return e->srv;
}

/* NV2A wrap modes: 1 wrap, 2 mirror, 3 clamp to edge, 4 border, 5 clamp.
 * Border takes the stage's SET_TEXTURE_BORDER_COLOR (nv2a_stage_decode). */
static D3D11_TEXTURE_ADDRESS_MODE address_mode(uint32_t m)
{
    switch (m) {
    case 1:  return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case NV2A_ADDR_BORDER: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

/* SET_TEXTURE_FILTER: LOD bias in bits 0-12 (signed, 8 fraction bits), MIN
 * in bits 16-21, MAG in bits 24-27. MIN: 1 nearest and 2 linear on level 0,
 * 3/4 nearest/linear within the nearest level, 5/6 nearest/linear blended
 * between two levels, 7 convolution on level 0 (taken as linear). MAG: 1
 * nearest, 2 linear, 4 convolution (linear). The LOD range is CONTROL0's
 * clamps, as xemu's sampler: 0..0 when MIN does not mipmap. RECOMP_DEBUG=d3d11_point
 * forces nearest texels (the CPU path with RECOMP_DEBUG=pb_bilinear=0) for every
 * stage. */
#define filter_linear nv2a_tex_filter_linear   /* nv2a_backend_common.h */
#define filter_mip    nv2a_tex_filter_mip
#define lod_bias      nv2a_tex_lod_bias

#define SMP_MAX 256
static struct { uint64_t key; uint32_t border; ID3D11SamplerState *s; } s_smp[SMP_MAX];
static int s_smp_n;
static int s_smp_last[4];   /* per stage: the entry it got last */

static ID3D11SamplerState *sampler(const Stage *t, int bump_source)
{
    static int point = -1;
    static ID3D11SamplerState *fallback;
    D3D11_SAMPLER_DESC sd;
    uint32_t u = t->addr_u & 0xF, v = t->addr_v & 0xF;
    int mn, mg, mip, i;
    unsigned an;
    uint64_t key;
    uint32_t border;
    ID3D11SamplerState *st = NULL;

    if (point < 0)
        point = recomp_env(RENV_D3D11_POINT) != NULL;
    mn = !point && filter_linear(t->filter, 0);
    mg = !point && filter_linear(t->filter, 1);
    mip = t->levels > 1 ? filter_mip(t->filter) : 0;
    /* render.aniso: anisotropic filtering on the world's surfaces, seen at
     * an angle (nv2a_stage_aniso has the rule). In the key, so a stage that
     * is a bump source in one draw and plain in the next gets its own. */
    an = nv2a_stage_aniso(t, mip, mn, bump_source);
    key = u | v << 4 | (uint64_t)mn << 8 | (uint64_t)mg << 9
        | (uint64_t)mip << 10 | (uint64_t)an << 35;
    if (mip)
        key |= (uint64_t)(t->filter & 0x1FFFu) << 12
             | (uint64_t)t->lod_min << 25 | (uint64_t)t->lod_max << 29;
    /* The colour is part of the state only where an axis uses it. */
    border = (u == NV2A_ADDR_BORDER || v == NV2A_ADDR_BORDER) ? t->border : 0;
    if (s_smp_last[t - s_stage] < s_smp_n
            && s_smp[s_smp_last[t - s_stage]].key == key
            && s_smp[s_smp_last[t - s_stage]].border == border)
        return s_smp[s_smp_last[t - s_stage]].s;
    for (i = 0; i < s_smp_n; i++)
        if (s_smp[i].key == key && s_smp[i].border == border) {
            s_smp_last[t - s_stage] = i;
            return s_smp[i].s;
        }
    memset(&sd, 0, sizeof sd);
    sd.Filter = (D3D11_FILTER)((mn ? 0x10 : 0) | (mg ? 0x4 : 0)
                               | (mip == 2 ? 0x1 : 0));
    if (an) {
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = an;
    }
    sd.AddressU = address_mode(u);
    sd.AddressV = address_mode(v);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    nv2a_argb_to_float4(border, sd.BorderColor);
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MipLODBias = mip ? lod_bias(t->filter) : 0.0f;
    if (sd.MipLODBias > 15.99f) sd.MipLODBias = 15.99f;
    if (sd.MipLODBias < -16.0f) sd.MipLODBias = -16.0f;
    sd.MinLOD = mip ? (float)t->lod_min : 0.0f;
    sd.MaxLOD = mip ? (float)t->lod_max : 0.0f;
    ID3D11Device_CreateSamplerState(s_dev, &sd, &st);
    if (!st)
        return fallback;
    if (s_smp_n < SMP_MAX) {
        i = s_smp_n++;
    } else {
        /* Full: replace one round robin. A released state's address can
         * come back, so the bound-state filter starts over. */
        static int next;
        i = next++ % SMP_MAX;
        if (s_smp[i].s == fallback)
            fallback = NULL;
        ID3D11SamplerState_Release(s_smp[i].s);
        s_state_gen++;
    }
    s_smp[i].key = key;
    s_smp[i].border = border;
    s_smp[i].s = st;
    s_smp_last[t - s_stage] = i;
    if (!fallback)
        fallback = st;
    return st;
}

static ID3D11ShaderResourceView *solid_texture(uint32_t argb)
{
    ID3D11ShaderResourceView *v = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Texture2D *t = NULL;
    memset(&td, 0, sizeof td);
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = &argb;
    sd.SysMemPitch = 4;
    sd.SysMemSlicePitch = 4;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &t))) {
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t, NULL, &v);
        ID3D11Texture2D_Release(t);
    }
    return v;
}

static void create_black_texture(void)
{
    static const uint32_t black = 0xFF000000u;
    s_magenta_srv = solid_texture(0xFFFF00FFu);
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Texture2D *t = NULL;
    memset(&td, 0, sizeof td);
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = &black;
    sd.SysMemPitch = 4;
    sd.SysMemSlicePitch = 4;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &t))) {
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t, NULL,
                                              &s_black_srv);
        ID3D11Texture2D_Release(t);
    }
}

/* ---- pixel stage --------------------------------------------------------- */

/* Register-combiner pixel shaders, generated by d3d8_combiners.c from the
 * combiner registers and keyed by a hash of the parsed state (constants are
 * not part of it: they go in the constant buffer). */
#define RC_MAX 64
typedef struct {
    uint32_t           hash;
    int                used, bad;
    uint64_t           last_use;
    NV2ACombinerState  st;
    ID3D11PixelShader *ps;
} RcEntry;
static RcEntry  s_rc[RC_MAX];
static uint64_t s_rc_compiles;
static RcEntry *s_rc_last;      /* the entry combiner_ps last returned from */

static uint32_t rc_hash(const NV2ACombinerState *st)
{
    const uint8_t *p = (const uint8_t *)st;
    uint32_t h = 0x811C9DC5u;
    size_t i;
    for (i = 0; i < sizeof *st; i++)
        h = (h ^ p[i]) * 0x01000193u;
    return h ? h : 1;
}

/* s_ps with render.snap_2d's sampling (s_ps_snap_src), built the first
 * time the setting draws so a stock run never compiles it. NULL when it
 * cannot be built: the caller falls back to s_ps. */
static ID3D11PixelShader *snap_ps(void)
{
    static int tried;
    static ID3D11PixelShader *ps;
    ID3DBlob *code = NULL;

    if (tried)
        return ps;
    tried = 1;
    if (compile("nv2a_ps_diffuse_snap", s_ps_snap_src, "ps_5_0", &code)) {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                       ID3D10Blob_GetBufferSize(code), NULL, &ps);
        ID3D10Blob_Release(code);
    }
    if (!ps)
        fprintf(stderr, LOGP "nv2a_ps_diffuse_snap: FAILED to build, render.snap_2d"
                " draws the stock shader\n");
    return ps;
}

/* NULL when the shader cannot be built: the caller falls back to s_ps. */
static ID3D11PixelShader *combiner_ps(const NV2ACombinerState *st)
{
    static char src[32768];
    uint32_t h = rc_hash(st);
    RcEntry *e = NULL, *lru = &s_rc[0];
    ID3DBlob *code = NULL;
    int i;

    for (i = 0; i < RC_MAX; i++) {
        RcEntry *c = &s_rc[i];
        if (c->used && c->hash == h && !memcmp(&c->st, st, sizeof *st)) {
            c->last_use = s_tick;
            s_rc_last = c;
            return c->bad ? NULL : c->ps;
        }
        if (!c->used) {
            if (!e) e = c;
        } else if (c->last_use < lru->last_use) {
            lru = c;
        }
    }
    if (!e) {
        e = lru;
        if (e->ps) {
            ID3D11PixelShader_Release(e->ps);
            s_state_gen++;
        }
    }
    memset(e, 0, sizeof *e);
    s_rc_last = e;
    e->used = 1;
    e->hash = h;
    e->st = *st;
    e->last_use = s_tick;
    s_rc_compiles++;
    if (d3d8_combiners_generate_hlsl(st, src, (int)sizeof src) < 0
     || !compile("nv2a_ps_combiner", src, "ps_5_0", &code)) {
        fprintf(stderr, LOGP "combiner shader %08X failed (%d stages)\n",
                (unsigned)h, st->num_stages);
        e->bad = 1;
        return NULL;
    }
    if (FAILED(ID3D11Device_CreatePixelShader(s_dev,
                   ID3D10Blob_GetBufferPointer(code),
                   ID3D10Blob_GetBufferSize(code), NULL, &e->ps)))
        e->bad = 1;
    ID3D10Blob_Release(code);
    if (recomp_env(RENV_D3D11_VERBOSE))
        fprintf(stderr, LOGP "combiner shader %08X: %d stages\n%s\n",
                (unsigned)h, st->num_stages, src);
    return e->bad ? NULL : e->ps;
}

/* Render to texture: a stage whose texture lies where a surface this backend
 * renders into starts samples that surface. The guest's copy of those bytes
 * is stale (nothing here writes guest memory), so decoding it would show
 * whatever was there before -- or nothing at all. A 32-bit texture at the
 * surface's address only, and never the surface being drawn into. */
/* The surface being drawn into, sampled by the same batch (a full-screen
 * post pass reading the frame so far). D3D11 cannot bind one texture as both
 * target and resource, and the guest memory under it is empty -- the frame
 * lives on the GPU -- so the batch gets a copy taken just before it draws.
 * The CPU path reads the same thing: the surface memory as drawn so far. */
static RenderTarget     s_self;         /* tex/srv/w/h of the copy */
static uint64_t         s_self_copies;

static RenderTarget *rt_self_copy(RenderTarget *rt)
{
    if (s_self.tex && (s_self.w != rt->w || s_self.h != rt->h
                       || s_self.hw != rt->hw || s_self.hh != rt->hh)) {
        if (s_self.srv) ID3D11ShaderResourceView_Release(s_self.srv);
        ID3D11Texture2D_Release(s_self.tex);
        s_self.tex = NULL;
        s_self.srv = NULL;
    }
    if (!s_self.tex) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(rt->tex, &td);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_self.tex)))
            return NULL;
        if (FAILED(ID3D11Device_CreateShaderResourceView(
                    s_dev, (ID3D11Resource *)s_self.tex, NULL, &s_self.srv))) {
            ID3D11Texture2D_Release(s_self.tex);
            s_self.tex = NULL;
            return NULL;
        }
        s_self.w = rt->w;
        s_self.h = rt->h;
        s_self.hw = rt->hw;     /* GetDesc gave the copy the source's size */
        s_self.hh = rt->hh;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_self.tex,
                                     (ID3D11Resource *)rt->tex);
    s_self.offset = rt->offset;
    s_self.pad = rt->pad;   /* the copy has the frame's pads (Metal's s_self too) */
    s_self.addr = rt->addr;
    s_self.last_draw = rt->last_draw;
    s_self.last_clear = rt->last_clear;
    s_self_copies++;
    {
        static int said;
        if (!said++)
            fprintf(stderr, LOGP "surface 0x%08X sampled while drawn into:"
                    " bound as a copy\n", rt->offset);
    }
    return &s_self;
}

static int aspect_trace(void);

static RenderTarget *rt_texture(const Stage *t)
{
    static int off = -1;
    RenderTarget *self = NULL;
    int i;

    if (off < 0)
        off = recomp_env(RENV_D3D11_NO_RTT) != NULL;   /* A/B: decode memory */
    if (off || !t->offset)
        return NULL;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        /* Low 27 bits: the same memory through either window. A 16-bit
         * texture binds too: this backend writes nothing back, so one
         * decoded from the guest bytes under a 16-bit surface reads black. */
        if (!rt->tex || ((rt->addr ^ t->offset) & 0x07FFFFFFu))
            continue;
        if (!nv2a_rt_tex_reads(t->color, t->pitch, rt->fmt, rt->pitch)) {
            /* Decoded from guest memory instead, which this backend never
             * writes a target back to: that reads black. The format
             * word is the last draw's: reuse matches offset and size only. */
            if (aspect_trace() && nv2a_rt_reject_new(rt->fmt, rt->pitch, t->color, t->pitch))
                fprintf(stderr, "[aspect.rt-reject] flip %llu: target 0x%08X surface fmt %X"
                        " pitch %u, texture fmt %X pitch %u\n",
                        (unsigned long long)s_presents, rt->addr, rt->fmt, rt->pitch, t->color,
                        t->pitch);
            continue;
        }
        if (rt_drop_if_stale(rt, "bind"))
            continue;   /* the texture decodes from the title's bytes */
        if (rt == s_cur) {
            self = rt;
            continue;
        }
        if (!rt->srv && FAILED(ID3D11Device_CreateShaderResourceView(
                    s_dev, (ID3D11Resource *)rt->tex, NULL, &rt->srv)))
            return NULL;
        return rt;
    }
    return self ? rt_self_copy(self) : NULL;
}

/* The combiner state of the batch being drawn (for the trace), and whether
 * it ran through combiners at all. */
static NV2ACombinerState s_last_rc;
static int               s_last_rc_on;

/* RECOMP_D3D11_DEBUG_PS=<what>: every batch draws one input of the pixel
 * stage instead of its real shader, to see which one goes wrong:
 *   d0 / d1     the interpolated diffuse / specular colour
 *   t0 .. t3    stage n's texture at its coordinates (alpha forced to 1)
 *   tc0 .. tc3  stage n's coordinates as colour (frac of s, t)
 * Unknown values draw white. */
/* RECOMP_D3D11_DEBUG_PS_SKIP_POST=1: batches that look like post passes --
 * depth test off, at most 8 vertices (a full-screen quad or two) -- keep
 * their real shader, so a glow or blur drawn over the scene does not
 * cover what the debug shader shows. Set per batch by on_draw. */
static int s_dbg_post;

static ID3D11PixelShader *debug_ps(void)
{
    static int tried;
    static ID3D11PixelShader *ps;
    const char *w;
    char src[4096], expr[256];
    ID3DBlob *code = NULL;

    if (tried)
        return ps;
    tried = 1;
    w = recomp_env(RENV_D3D11_DEBUG_PS);
    if (!w || !*w)
        return NULL;
    if (!strcmp(w, "d0"))
        snprintf(expr, sizeof expr, "float4(i.oD0.rgb, 1)");
    else if (!strcmp(w, "d1"))
        snprintf(expr, sizeof expr, "float4(i.oD1.rgb, 1)");
    else if (w[0] == 't' && w[1] >= '0' && w[1] <= '3' && !w[2])
        snprintf(expr, sizeof expr,
                 "float4(tex%c.Sample(samp%c, nv2a_snap_uv(i.oT%c.xy, i.oPos.xy, tex_scale[%c])"
                 " * tex_scale[%c].xy).rgb, 1)",
                 w[1], w[1], w[1], w[1], w[1]);
    else if (w[0] == 't' && w[1] == 'c' && w[2] >= '0' && w[2] <= '3' && !w[3])
        snprintf(expr, sizeof expr, "float4(frac(i.oT%c.xy), 0, 1)", w[2]);
    else
        snprintf(expr, sizeof expr, "float4(1, 1, 1, 1)");
    snprintf(src, sizeof src, "%s%s%s"
             "Texture2D tex0 : register(t0); Texture2D tex1 : register(t1);\n"
             "Texture2D tex2 : register(t2); Texture2D tex3 : register(t3);\n"
             "SamplerState samp0 : register(s0); SamplerState samp1 : register(s1);\n"
             "SamplerState samp2 : register(s2); SamplerState samp3 : register(s3);\n"
             "float4 main(VS_OUT i) : SV_Target { return %s; }\n",
             VS_OUT_DECL, PS_CB_DECL, D3D8_HLSL_SNAP_UV, expr);
    if (compile("nv2a_ps_debug", src, "ps_5_0", &code)) {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                       ID3D10Blob_GetBufferSize(code), NULL, &ps);
        ID3D10Blob_Release(code);
    }
    fprintf(stderr, LOGP "d3d11_debug_ps=%s: every batch draws %s%s\n",
            w, expr, ps ? "" : " (FAILED to build)");
    return ps;
}

/* Stages, textures, samplers and PS constants for the batch about to be
 * drawn. Returns the pixel shader to use. */
static PsConsts s_cb_ps_shadow;
static int      s_cb_ps_ok;

/* Hor+ (display.aspect): a batch's placement needs to know whether a stage
 * samples a guest texture, which only the stage binding below finds out;
 * d3d11_on_draw fills two_d and full_w, setup_pixel the placement (as
 * Metal's metal_on_draw does in one place). */
struct aspect_batch {
    int two_d, full_w;
    int place;          /* nv2a_aspect_place */
    int guest_tex;      /* a stage samples a guest picture (nv2a_aspect_picture) */
    /* A small 3D program triangle batch on a padded target, which
     * setup_pixel checks for a screen quad (nv2a_aspect_screen_quad) once
     * it knows the stages sample padded targets; NULL d for any other. */
    const struct nv2a_pb_draw *d;
    const VsEntry *vs;
    const uint32_t *idx;
    uint32_t lo, count, nidx;
    float lo_x, hi_x;
    int vary_z;         /* a program's 2D batch off one oPos.z (nv2a_aspect_gate) */
    int post_tex;       /* a full-width 2D pass whose picture nv2a_aspect_place_tex dropped */
};

/* Whether hp's batch is a full-width screen quad (nv2a_aspect_screen_quad;
 * quad: the stages allow one), which marks its third on rt as holding a
 * bar, or a piece of a letterbox band (nv2a_aspect_band; band: the stages
 * and depth allow one, in the thirds bars), its x range then in hp->lo_x,
 * hi_x. */
static int aspect_quad(struct aspect_batch *hp, const struct nv2a_pb_vsh *v, RenderTarget *rt,
                       int quad, unsigned bars)
{
    float vf[NV2A_ASPECT_QUAD_MAX * NV2A_VS_MAX_INPUTS * 4], xy[NV2A_ASPECT_QUAD_MAX][2];
    int slot[NV2A_VS_MAX_INPUTS];
    unsigned nf4 = 0, i;
    uint32_t k;

    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++)
        if (hp->vs->inputs & (1u << i))
            slot[nf4++] = (int)i;
    for (k = 0; k < hp->count; k++)
        for (i = 0; i < nf4; i++)
            vtx_input(hp->d, v, (unsigned)slot[i], hp->lo + k, vf + ((size_t)k * nf4 + i) * 4u);
    if (!nv2a_aspect_screen_quad((const uint32_t (*)[4])v->prog, v->start,
                                 (float (*)[4])(uintptr_t)v->c, vf, nf4, slot, hp->count, xy,
                                 &hp->lo_x, &hp->hi_x))
        return 0;
    if (quad && nv2a_aspect_quad_span(hp->lo_x, hp->hi_x, rt->w)
        && nv2a_aspect_full_tris((const float (*)[2])xy, hp->idx, hp->nidx / 3u, rt->w)) {
        nv2a_aspect_bar_mark(rt->bar_flip,
                             nv2a_aspect_third((const float (*)[2])xy, hp->count, rt->h),
                             nv2a_pb_gpu_state()->flips);
        return 1;
    }
    return bars && nv2a_aspect_band((const float (*)[2])xy, hp->count, rt->w, rt->h, bars);
}

static ID3D11PixelShader *setup_pixel(struct aspect_batch *hp)
{
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    ID3D11ShaderResourceView *srv[4];
    ID3D11SamplerState *smp[4];
    D3D11_MAPPED_SUBRESOURCE m;
    PsConsts pc;
    int n, textured = 0;
    unsigned bump_src;

    build_stages();
    bump_src = nv2a_bump_sources(s_stage, v->rc_set && (v->rc_ctl & 0xFF),
                                 v->other_stage_input);
    {
        static int bump_trace = -1;
        if (bump_trace < 0)
            bump_trace = recomp_env(RENV_BUMP_TRACE) != NULL;
        /* Only a combiner draw samples a bump stage (the basic shader
         * reads every stage as plain 2D), so only those are logged. */
        if (bump_trace && v->rc_set && (v->rc_ctl & 0xFF))
            nv2a_bump_trace(v, s_stage, nv2a_pb_gpu_state()->flips);
    }
    nv2a_ps_consts_fill(v, s_stage, &pc);
    /* The stage loop's views, while nothing they come from has moved: the
     * same decoded stages (s_stage_gen) in the same flip (tex_bind checks
     * the texels once a flip), no texture uploaded or freed and no render
     * target made, grown, dropped or reformatted since. Only a batch whose
     * stages all came from the texture cache is remembered: a render
     * target's view (and the self copy, made per draw) goes through
     * rt_texture every time. Most batches of a busy frame repeat the last
     * one's textures; the loop was ~2.4 ms a flip at 10k batches on the
     * Deck. */
    static struct {
        uint32_t stage_gen, rt_gen, tc_gen, pack_gen;
        uint64_t flip;
        int ok, textured;
        ID3D11ShaderResourceView *srv[4];
        TexEntry *e[4];
        const char *src[4];
    } lm;
    int lhit = memo_on() && lm.ok && lm.stage_gen == s_stage_gen && lm.rt_gen == s_rt_gen
              && lm.tc_gen == s_tc_gen && lm.pack_gen == nv2a_tex_pack_gen()
              && lm.flip == nv2a_pb_gpu_state()->flips;
    if (lhit) {
        for (n = 0; n < 4; n++) {
            srv[n] = lm.srv[n];
            s_stage_rt[n] = NULL;
            s_stage_src[n] = lm.src[n];
            if (lm.e[n])
                lm.e[n]->used = ++s_tc_clock;   /* the LRU sees the use */
            smp[n] = sampler(&s_stage[n], bump_src >> n & 1);
        }
        textured = lm.textured;
        s_loop_hits++;
    }
    for (n = 0; !lhit && n < 4; n++) {
        const Stage *t = &s_stage[n];
        RenderTarget *src = (t->mode && t->mode != 4) ? rt_texture(t) : NULL;
        lm.e[n] = NULL;
        s_stage_rt[n] = src;
        s_stage_src[n] = !t->mode ? "none" : t->mode == 4 ? "passthru" : "cache";
        if (src) {
            s_stage_src[n] = src == &s_self ? "render-target(self copy)"
                                            : "render-target";
            nv2a_ps_consts_rt(&pc, n, t, src->w, src->h);
            if (nv2a_rt_no_alpha(nv2a_tex_linear_twin(t->color)))
                pc.alpha_only[n] = 2;
            srv[n] = src->srv;
            s_rtt_binds++;
            textured = 1;
        } else if (t->mode && t->mode != 4) {
            srv[n] = tex_bind(t);
            lm.e[n] = s_tex_last;
            if (srv[n] == s_magenta_srv) s_stage_src[n] = "MAGENTA(undecoded)";
            else if (srv[n] == s_black_srv) s_stage_src[n] = "BLACK(too large/failed)";
            textured = 1;
        } else {
            srv[n] = s_black_srv;
        }
        smp[n] = sampler(t, bump_src >> n & 1);
    }
    if (!lhit) {
        /* Taken after the loop: tex_bind's own uploads moved the counters. */
        lm.ok = !s_stage_rt[0] && !s_stage_rt[1] && !s_stage_rt[2] && !s_stage_rt[3];
        /* Nor one that got a fallback view: tex_bind gives black for a
         * failed re-upload only once, then the entry's old view for the
         * rest of the flip, which a remembered black would hide. */
        for (n = 0; n < 4; n++)
            if (s_stage[n].mode && s_stage[n].mode != 4
                && (srv[n] == s_black_srv || srv[n] == s_magenta_srv))
                lm.ok = 0;
        lm.stage_gen = s_stage_gen;
        lm.rt_gen = s_rt_gen;
        lm.tc_gen = s_tc_gen;
        lm.pack_gen = nv2a_tex_pack_gen();
        lm.flip = nv2a_pb_gpu_state()->flips;
        lm.textured = textured;
        memcpy(lm.srv, srv, sizeof srv);
        memcpy(lm.src, s_stage_src, sizeof lm.src);
    }
    hp->place = NV2A_PLACE_CENTRE;
    hp->post_tex = 0;
    if (s_cur && s_cur->pad) {
        int guest_tex = 0, flat_rt = 0, padded_rt = 0;
        for (n = 0; n < 4; n++) {
            if (s_stage[n].mode && s_stage[n].mode != 4 && !s_stage_rt[n]
                && nv2a_aspect_picture(s_stage[n].width, s_cur->w, bump_src >> n & 1))
                guest_tex = 1;
            if (s_stage_rt[n] && !s_stage_rt[n]->pad)
                flat_rt = 1;
            if (s_stage_rt[n] && s_stage_rt[n]->pad)
                padded_rt = 1;
        }
        hp->guest_tex = guest_tex;
        /* A perspective screen quad over padded targets is a full-width
         * pass too (nv2a_aspect_screen_quad) and marks its third as
         * holding a bar; one drawn without depth in such a third is
         * stretched as a piece of a letterbox band (nv2a_aspect_band), as
         * in metal_on_draw. */
        if (hp->d && !hp->two_d) {
            int quad = nv2a_aspect_quad_ok(padded_rt, flat_rt, guest_tex);
            unsigned bars = v->depth_enable || flat_rt || guest_tex
                                ? 0u
                                : nv2a_aspect_bars(s_cur->bar_flip, nv2a_pb_gpu_state()->flips);
            if ((quad || bars) && aspect_quad(hp, v, s_cur, quad, bars))
                hp->two_d = hp->full_w = 1;
        }
        /* A square target is a reflection, glow or shadow map, never a
         * movie: display.movie applies to the frame only. */
        hp->place = nv2a_aspect_place(hp->two_d, hp->full_w,
                                      nv2a_aspect_place_tex(guest_tex, padded_rt), flat_rt,
                                      s_cur->w == s_cur->h ? NV2A_MOVIE_BLACK
                                                           : nv2a_host_opts()->movie_mode);
        hp->post_tex = hp->two_d && hp->full_w && guest_tex && padded_rt;
    }
    if (s_cur && nv2a_snap_2d_on(s_cur->n)) {
        /* render.snap_2d: a centred 2D batch samples guest textures at its
         * guest pixels' 1x points (nv2a_uv_snap), as metal_on_draw. */
        unsigned rt_mask = 0;
        for (n = 0; n < 4; n++)
            if (s_stage_rt[n])
                rt_mask |= 1u << n;
        nv2a_ps_consts_snap(&pc, s_stage, rt_mask, s_cur->n,
                            nv2a_snap_flat2d(hp->two_d && !hp->vary_z, hp->place));
    }
    /* A batch sampling a padded target the stock way reads it whole, pads
     * and all, which is what a stretched pass wants, short of a square's
     * overscan (nv2a_rt_stretch_read). Any other reads its
     * guest columns (nv2a_rt_guest_read; tex_scale.w, rt_offset), on an
     * unpadded target too: Burnout 3's 640x464 frame has no pad, but the
     * squares it samples do. */
    for (n = 0; n < 4; n++) {
        const RenderTarget *src = s_stage_rt[n];
        float sc, off;
        if (!src || !src->pad)
            continue;
        if (hp->place != NV2A_PLACE_STRETCH && hp->place != NV2A_PLACE_COVER)
            nv2a_rt_guest_read(src->w, src->pad, &sc, &off);
        /* Not a square's overscan: a stretch spans the ratio's pad. */
        else if (!nv2a_rt_stretch_read(src->w, src->h, src->pad, &sc, &off))
            continue;
        pc.tex_scale[n][0] *= sc;
        pc.tex_scale[n][3] = off;
    }
    if (memo_on() && s_cb_ps_ok && !memcmp(&s_cb_ps_shadow, &pc, sizeof pc)) {
        /* the buffer already holds these bytes */
    } else if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb_ps, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &pc, sizeof pc);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_ps, 0);
        s_cb_ps_shadow = pc;
        s_cb_ps_ok = 1;
    }
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, srv);
    if (!sc_on() || memcmp(s_sc.smp, smp, sizeof smp)) {
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smp);
        memcpy(s_sc.smp, smp, sizeof smp);
    }
    if (!sc_on() || s_sc.ps_cb != s_cb_ps) {
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb_ps);
        s_sc.ps_cb = s_cb_ps;
    }
    if (textured)
        s_draws_tex++;

    if (v->rc_set && (v->rc_ctl & 0xFF)) {
        NV2ACombinerState st;
        ID3D11PixelShader *ps;
        uint32_t modes = 0;
        uint32_t inputs = 0;
        for (n = 0; n < 4; n++) {
            /* Only 2D textures are bound: other sampling modes read 2D at
             * (s, t), as the CPU path does -- but BUMPENVMAP(_LUMINANCE),
             * which reads 2D at (s, t) moved by an earlier stage's texel,
             * from stage 1 on (OTHER_STAGE_INPUT names the stage). */
            uint32_t m = nv2a_stage_ps_mode(&s_stage[n], n);
            if (m == 6 || m == 7)
                inputs |= (uint32_t)nv2a_stage_input(n, v->other_stage_input) << (4 * n);
            modes |= m << (5 * n);
        }
        /* The registers and modes that make the state: the same as the last
         * batch's give the same state and so the same shader. */
        static struct {
            uint32_t r[38];
            int      key_ok, ps_ok;   /* s_last_rc is r's state; ps is its shader */
            ID3D11PixelShader *ps;
            RcEntry *e;
            uint64_t compiles;
        } memo;
        uint32_t key[38];
        int hit;
        memcpy(key, v->rc_cicw, 32);
        memcpy(key + 8, v->rc_aicw, 32);
        memcpy(key + 16, v->rc_cocw, 32);
        memcpy(key + 24, v->rc_aocw, 32);
        key[32] = v->rc_ctl;
        key[33] = v->rc_fcw0;
        key[34] = v->rc_fcw1;
        key[35] = modes;
        /* render.scale changes at run time: each scale class has its own text */
        key[36] = (nv2a_host_render_scale() > 1) | (nv2a_host_opts()->aspect_w ? 2u : 0u)
                | (nv2a_snap_2d_on(nv2a_host_render_scale()) ? 4u : 0u);
        key[37] = inputs;
        hit = memo_on() && memo.key_ok && !memcmp(memo.r, key, sizeof key);
        if (!hit) {
            d3d8_combiners_from_regs(v->rc_cicw, v->rc_aicw, v->rc_cocw,
                                     v->rc_aocw, v->rc_ctl, v->rc_fcw0,
                                     v->rc_fcw1, modes, &st);
            st.fog_input = 1;   /* FOG.a is the interpolated vertex fog factor */
            st.rect_snap = (int)(key[36] & 1u);   /* nv2a_snap_uv; in rc_hash via st */
            st.rt_offset = (int)(key[36] >> 1 & 1u);   /* hor+: tex_scale.w, nv2a_aspect_place */
            st.snap_2d = (int)(key[36] >> 2 & 1u);     /* render.snap_2d: nv2a_snap_uv2d */
            for (n = 0; n < 4; n++)
                st.tex_input[n] = (int)((inputs >> (4 * n)) & 0xF);
            st.bump_on = 1;
            s_last_rc = st;
            memcpy(memo.r, key, sizeof key);
            memo.key_ok = 1;
            memo.ps_ok = 0;
        }
        s_last_rc_on = 1;
        if (!s_dbg_post && (ps = debug_ps()) != NULL)
            return ps;
        /* A compile since may have evicted the entry remembered. */
        if (hit && memo.ps_ok && memo.compiles == s_rc_compiles) {
            memo.e->last_use = s_tick;   /* as combiner_ps's hit does */
            ps = memo.ps;
        } else {
            ps = combiner_ps(&s_last_rc);
            memo.ps = ps;
            memo.e = s_rc_last;
            memo.compiles = s_rc_compiles;
            memo.ps_ok = 1;
        }
        if (ps)
            return ps;
    }
    if (!(v->rc_set && (v->rc_ctl & 0xFF)))
        s_last_rc_on = 0;
    {
        ID3D11PixelShader *ps = s_dbg_post ? NULL : debug_ps();
        if (ps)
            return ps;
    }
    if (nv2a_snap_2d_on(nv2a_host_render_scale())) {
        ID3D11PixelShader *ps = snap_ps();
        if (ps)
            return ps;
    }
    return s_ps;
}


/* ---- batch trace ------------------------------------------------------------
 *
 * The full state of one batch, for the pixel trace below (RECOMP_TRACE=px):
 * the state, every stage and where its SRV came from, the input layout, the
 * vertex program's outputs for the first three vertices twice -- from the CPU
 * interpreter on the same inputs, and read back from the GPU through stream
 * output -- the generated HLSL (once per program/combiner), and the colour
 * of the first triangle's centroid before the draw. Lines start
 * "[D3D11-WHITE]", after the investigation it was written for. */

static uint32_t read_tex_pixel(ID3D11Texture2D *tex, UINT x, UINT y)
{
    static ID3D11Texture2D *st;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BOX b;
    uint32_t px = 0xDEADBEEFu;

    if (!st) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = 1;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st)))
            return px;
    }
    b.left = x; b.right = x + 1;
    b.top = y; b.bottom = y + 1;
    b.front = 0; b.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)st, 0, 0, 0, 0,
                                              (ID3D11Resource *)tex, 0, &b);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0,
                                          D3D11_MAP_READ, 0, &m))) {
        px = *(const uint32_t *)m.pData;   /* B,G,R,A bytes: 0xAARRGGBB */
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    return px;
}

static uint32_t read_pixel(RenderTarget *rt, int x, int y)
{
    if (x < 0 || y < 0 || (UINT)x >= rt->w || (UINT)y >= rt->h)
        return 0xDEADBEEFu;
    /* Guest coordinates; a scaled target holds the pixel's first sample at
     * N times them. */
    return read_tex_pixel(rt->tex, ((UINT)x + rt->pad) * rt->n, (UINT)y * rt->n);
}

/* The first three vertices of the bound batch through the VS, read back by
 * stream output: 7 float4 each (SV_POSITION, COLOR0/1, TEXCOORD0..3). */
static int trace_stream_out(VsEntry *vs, D3D11_PRIMITIVE_TOPOLOGY topo,
                            float out[3][7][4])
{
    static const D3D11_SO_DECLARATION_ENTRY so[7] = {
        {0, "SV_POSITION", 0, 0, 4, 0}, {0, "COLOR", 0, 0, 4, 0},
        {0, "COLOR", 1, 0, 4, 0},       {0, "TEXCOORD", 0, 0, 4, 0},
        {0, "TEXCOORD", 1, 0, 4, 0},    {0, "TEXCOORD", 2, 0, 4, 0},
        {0, "TEXCOORD", 3, 0, 4, 0},
    };
    static ID3D11Buffer *buf, *stage;
    UINT stride = 7 * 16, zero = 0;
    ID3D11GeometryShader *gs = NULL;
    ID3D11Buffer *none = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr;
    int ok = 0;

    if (!buf) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 3 * 7 * 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &buf)))
            return 0;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &stage)))
            return 0;
    }
    hr = ID3D11Device_CreateGeometryShaderWithStreamOutput(
        s_dev, ID3D10Blob_GetBufferPointer(vs->code), ID3D10Blob_GetBufferSize(vs->code),
        so, 7, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, NULL, &gs);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11-WHITE]  stream output: hr 0x%08lX\n", (unsigned long)hr);
        return 0;
    }
    ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &buf, &zero);
    ID3D11DeviceContext_GSSetShader(s_ctx, gs, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_DrawIndexed(s_ctx, 3, 0, 0);
    ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &none, &zero);
    ID3D11DeviceContext_GSSetShader(s_ctx, NULL, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, topo);
    ID3D11GeometryShader_Release(gs);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stage, (ID3D11Resource *)buf);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stage, 0,
                                          D3D11_MAP_READ, 0, &m))) {
        memcpy(out, m.pData, 3 * 7 * 16);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stage, 0);
        ok = 1;
    }
    return ok;
}

static void trace_print_hlsl_once(uint32_t key, const char *what, const char *src)
{
    static uint32_t seen[64];
    static int n;
    int i;
    for (i = 0; i < n; i++)
        if (seen[i] == key)
            return;
    if (n < 64)
        seen[n++] = key;
    fprintf(stderr, "[D3D11-WHITE] ---- %s %08X ----\n%s\n[D3D11-WHITE] ---- end ----\n",
            what, key, src);
}

/* Before the draw: everything but the pixel. Returns the pixel to read back
 * after it in *px, *py (-1 if none). */
static void trace_batch(const struct nv2a_pb_draw *d, VsEntry *vs, RenderTarget *rt,
                        const uint8_t fmts[16], const UINT strides[16],
                        const UINT offs[16], D3D11_PRIMITIVE_TOPOLOGY topo,
                        int *px, int *py)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    float so[3][7][4];
    int k, i, have_so;

    *px = *py = -1;
    fprintf(stderr, "[D3D11-WHITE] batch prim %u n %u prog %08X start %u | rc %d ctl %08X"
            " shader %08X | depth %d func 0x%X mask %d | blend %d 0x%X 0x%X eq 0x%X"
            " | cull %d 0x%X front 0x%X | ctl0 %08X poly %d %g %g | atest %d"
            " 0x%X %u | surface 0x%08X %ux%u | fog en %d colour %08X\n",
            d->prim, d->idx_count, vs->hash, v->start, s_last_rc_on, v->rc_ctl,
            v->shader_prog, v->depth_enable, v->depth_func, v->depth_mask,
            g->blend_enable, g->blend_sfactor, g->blend_dfactor, g->blend_equation,
            v->cull_enable, v->cull_face, v->front_face, v->control0,
            v->poly_offset, v->poly_factor, v->poly_bias, v->atest_enable,
            v->atest_func, v->atest_ref, rt->offset, rt->w, rt->h, v->fog_enable,
            v->fog_color);
    for (k = 0; k < 4; k++) {
        const Stage *t = &s_stage[k];
        fprintf(stderr, "[D3D11-WHITE]  tex%d mode %d on %d fmt %02X %ux%u off %08X pitch %u"
                " addr %u/%u filter %08X pal %08X src %s", k, t->mode, t->on, t->color,
                t->width, t->height, t->offset, t->pitch, t->addr_u, t->addr_v,
                t->filter, t->palette,
                s_stage_src[k] ? s_stage_src[k] : "?");
        fprintf(stderr, " levels %u lod %u-%u", t->levels, t->lod_min, t->lod_max);
        if (s_stage_rt[k])
            fprintf(stderr, " (surface 0x%08X %ux%u, drawn %llu, cleared %llu)",
                    s_stage_rt[k]->offset, s_stage_rt[k]->w, s_stage_rt[k]->h,
                    (unsigned long long)s_stage_rt[k]->last_draw,
                    (unsigned long long)s_stage_rt[k]->last_clear);
        fprintf(stderr, "\n");
    }
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        const VertexAttr *a = &d->attr[i];
        if (!(vs->inputs & (1u << i)) && !a->size)
            continue;
        fprintf(stderr, "[D3D11-WHITE]  attr%d %s type %u size %u stride %u offset 0x%X"
                " inline %d | IA dxgi %u stride %u at %u | inl %g %g %g %g\n", i,
                (vs->inputs & (1u << i)) ? "read" : "unread", a->type, a->size,
                a->stride, a->offset, d->inline_data, fmts[i], strides[i], offs[i],
                v->inl[i][0], v->inl[i][1], v->inl[i][2], v->inl[i][3]);
    }
    fprintf(stderr, "[D3D11-WHITE]  c58 %g %g %g %g c59 %g %g %g %g\n",
            v->c[58][0], v->c[58][1], v->c[58][2], v->c[58][3],
            v->c[59][0], v->c[59][1], v->c[59][2], v->c[59][3]);
    {   /* RECOMP_D3D11_PX_CONSTS=a-b,c,...: those vertex constants too */
        const char *e = recomp_env(RENV_D3D11_PX_CONSTS);
        while (e && *e) {
            char *end;
            long lo = strtol(e, &end, 10), hi = lo, c;
            if (end == e)
                break;
            if (*end == '-')
                hi = strtol(end + 1, &end, 10);
            for (c = lo; c <= hi && c >= 0 && c < NV2A_VSH_CONSTANTS; c++)
                fprintf(stderr, "[D3D11-WHITE]  c%ld %g %g %g %g\n", c,
                        v->c[c][0], v->c[c][1], v->c[c][2], v->c[c][3]);
            e = *end == ',' ? end + 1 : NULL;
        }
    }

    have_so = trace_stream_out(vs, topo, so);
    {   /* RECOMP_D3D11_PX_VERTS=1: the program's first 16 slots and every
         * vertex's screen position, for working a draw out by hand */
        static int all = -1;
        if (all < 0)
            all = recomp_env(RENV_D3D11_PX_VERTS) != NULL;
        for (k = 0; all && k < 16; k++)
            fprintf(stderr, "[D3D11-P] %d %08X %08X %08X %08X\n", k,
                    v->prog[v->start + k][0], v->prog[v->start + k][1],
                    v->prog[v->start + k][2], v->prog[v->start + k][3]);
        for (k = 0; all && (uint32_t)k < d->idx_count; k++) {
            float in[NV2A_VSH_INPUTS][4];
            Nv2aVshOut o;
            memset(in, 0, sizeof in);
            for (i = 0; i < NV2A_VSH_INPUTS && i < NV2A_VS_MAX_INPUTS; i++) {
                const VertexAttr *a = &d->attr[i];
                if (!a->size || !fetch(d, a, d->idx[k], in[i]))
                    memcpy(in[i], v->inl[i], 16);
            }
            nv2a_vsh_run((const uint32_t (*)[4])v->prog, v->start,
                         (float (*)[4])v->c, 0, (const float (*)[4])in, &o);
            fprintf(stderr, "[D3D11-V] %u %g %g %g %g\n", d->idx[k],
                    o.o[0][0], o.o[0][1], o.o[0][2], o.o[0][3]);
        }
    }
    for (k = 0; k < 3 && (uint32_t)k < d->idx_count; k++) {
        float in[NV2A_VSH_INPUTS][4];
        Nv2aVshOut o;
        uint32_t index = d->idx[k];
        memset(in, 0, sizeof in);
        for (i = 0; i < NV2A_VSH_INPUTS && i < NV2A_VS_MAX_INPUTS; i++) {
            const VertexAttr *a = &d->attr[i];
            if (!a->size || !fetch(d, a, index, in[i]))
                memcpy(in[i], v->inl[i], 16);
        }
        nv2a_vsh_run((const uint32_t (*)[4])v->prog, v->start,
                     (float (*)[4])v->c, 0, (const float (*)[4])in, &o);
        fprintf(stderr, "[D3D11-WHITE]  vert %u (index %u):", k, index);
        for (i = 0; i < 16; i++)
            if (d->attr[i].size)
                fprintf(stderr, " v%d(%g %g %g %g)", i, in[i][0], in[i][1], in[i][2], in[i][3]);
        fprintf(stderr, "\n[D3D11-WHITE]   cpu pos %g %g %g %g D0 %g %g %g %g D1 %g %g %g %g"
                " T0 %g %g %g %g T1 %g %g %g %g\n",
                o.o[0][0], o.o[0][1], o.o[0][2], o.o[0][3],
                o.o[3][0], o.o[3][1], o.o[3][2], o.o[3][3],
                o.o[4][0], o.o[4][1], o.o[4][2], o.o[4][3],
                o.o[9][0], o.o[9][1], o.o[9][2], o.o[9][3],
                o.o[10][0], o.o[10][1], o.o[10][2], o.o[10][3]);
        if (have_so)
            fprintf(stderr, "[D3D11-WHITE]   gpu clip %g %g %g %g D0 %g %g %g %g D1 %g %g %g %g"
                    " T0 %g %g %g %g T1 %g %g %g %g\n",
                    so[k][0][0], so[k][0][1], so[k][0][2], so[k][0][3],
                    so[k][1][0], so[k][1][1], so[k][1][2], so[k][1][3],
                    so[k][2][0], so[k][2][1], so[k][2][2], so[k][2][3],
                    so[k][3][0], so[k][3][1], so[k][3][2], so[k][3][3],
                    so[k][4][0], so[k][4][1], so[k][4][2], so[k][4][3]);
    }
    if (have_so && d->idx_count >= 3) {
        float sx = 0, sy = 0;
        int ok = 1;
        for (k = 0; k < 3; k++) {
            float w = so[k][0][3];
            if (w <= 0.0f) { ok = 0; break; }
            sx += (so[k][0][0] / w * 0.5f + 0.5f) * (float)rt->w;
            sy += (0.5f - so[k][0][1] / w * 0.5f) * (float)rt->h;
        }
        if (ok) {
            *px = (int)(sx / 3.0f);
            *py = (int)(sy / 3.0f);
        }
    }
    {
        static NV2AVshProgram parsed;
        static char hlsl[65536];
        d3d8_vsh_parse(&v->prog[v->start][0],
                       NV2A_VSH_SLOTS - (int)v->start, &parsed);
        if (d3d8_vsh_generate_hlsl_ex(&parsed, NV2A_VSH_HLSL_SCREEN_SPACE,
                                      hlsl, sizeof hlsl) > 0)
            trace_print_hlsl_once(vs->hash, "vertex program", hlsl);
        if (s_last_rc_on && d3d8_combiners_generate_hlsl(&s_last_rc, hlsl, (int)sizeof hlsl) >= 0) {
            uint32_t h = 2166136261u;
            const char *c;
            for (c = hlsl; *c; c++) { h ^= (uint8_t)*c; h *= 16777619u; }
            fprintf(stderr, "[D3D11-WHITE]  combiner %08X\n", h);
            trace_print_hlsl_once(h, "combiner", hlsl);
        }
    }
    {
        for (k = 0; k < 8; k++)
            fprintf(stderr, "%s%08X/%08X%s", k ? " " : "[D3D11-WHITE]  factors c0/c1: ",
                    v->rc_f0[k], v->rc_f1[k], k == 7 ? "\n" : "");
        fprintf(stderr, "[D3D11-WHITE]  final cw %08X %08X, sf %08X %08X\n",
                v->rc_fcw0, v->rc_fcw1, v->rc_sf[0], v->rc_sf[1]);
        for (k = 0; k < (int)(v->rc_ctl & 0xFF) && k < 8; k++)
            fprintf(stderr, "[D3D11-WHITE]  stage %d icw %08X ocw %08X aicw %08X aocw %08X\n",
                    k, v->rc_cicw[k], v->rc_cocw[k], v->rc_aicw[k], v->rc_aocw[k]);
    }
    if (*px >= 0)
        fprintf(stderr, "[D3D11-WHITE]  px (%d,%d) before %08X\n", *px, *py,
                read_pixel(rt, *px, *py));
}

/* ---- draw ------------------------------------------------------------------ */

/* ---- pixel probe ------------------------------------------------------------
 *
 * RECOMP_D3D11_PX=x,y[;x,y...] (up to 4) with RECOMP_D3D11_PX_FLIPS=a-b
 * (this backend's present count, the "[D3D11] flip N" number; default 0-0
 * means the first frame only): every batch drawn in that window that covers
 * one of the pixels is redrawn twice into eight float targets, with the same
 * vertex shader, inputs, textures and samplers, depth tested LESS_EQUAL
 * without writes against the depth the real draw left, and the pixel's
 * interpolated inputs read back:
 *   pass A  oD0, oD1, oT0..oT3 as interpolated, SV_Position, fog
 *   pass B  stage 0..3 texels as the combiner samples them (q divide for
 *           true 2D stages, tex_scale) and the coordinates it samples at
 * Lines start "[D3D11-PX]": the colour before and after the real draw, then
 * the inputs; then the batch's full [D3D11-WHITE] block (stages with mode,
 * enable, address and filter, attributes, vertex outputs, combiner factors;
 * the HLSL once per program/combiner).
 * RECOMP_D3D11_PX_MAX caps the batches printed (default 400). Alpha test and
 * stencil are not applied to the probe, so "covered" can include a fragment
 * the real draw discarded: compare before/after. */
static int s_px_n = -1, s_px_x[4], s_px_y[4], s_px_max;
static uint64_t s_px_f0, s_px_f1;

static int px_on(void)
{
    if (s_px_n < 0) {
        const char *e = recomp_env(RENV_D3D11_PX), *f = recomp_env(RENV_D3D11_PX_FLIPS);
        const char *m = recomp_env(RENV_D3D11_PX_MAX);
        s_px_n = 0;
        while (e && *e && s_px_n < 4) {
            int x, y;
            if (sscanf(e, "%d,%d", &x, &y) != 2)
                break;
            s_px_x[s_px_n] = x;
            s_px_y[s_px_n] = y;
            s_px_n++;
            e = strchr(e, ';');
            if (e) e++;
        }
        if (f) {
            unsigned long long a = 0, b = 0;
            int k = sscanf(f, "%llu-%llu", &a, &b);
            s_px_f0 = a;
            s_px_f1 = k == 2 ? b : a;
        }
        s_px_max = m ? atoi(m) : 400;
        if (s_px_n)
            fprintf(stderr, LOGP "px: %d pixel(s), flips %llu-%llu\n",
                    s_px_n, (unsigned long long)s_px_f0,
                    (unsigned long long)s_px_f1);
    }
    return s_px_n > 0 && s_px_max > 0 && s_presents >= s_px_f0 && s_presents <= s_px_f1;
}

static ID3D11PixelShader *px_shader(int pass)
{
    static ID3D11PixelShader *ps[2];
    static int tried[2];
    char src[6144];
    ID3DBlob *code = NULL;

    if (tried[pass])
        return ps[pass];
    tried[pass] = 1;
    snprintf(src, sizeof src, "%s%s%s"
        "Texture2D tex0 : register(t0); Texture2D tex1 : register(t1);\n"
        "Texture2D tex2 : register(t2); Texture2D tex3 : register(t3);\n"
        "SamplerState samp0 : register(s0); SamplerState samp1 : register(s1);\n"
        "SamplerState samp2 : register(s2); SamplerState samp3 : register(s3);\n"
        "struct PO { float4 o0 : SV_Target0; float4 o1 : SV_Target1;\n"
        "    float4 o2 : SV_Target2; float4 o3 : SV_Target3; float4 o4 : SV_Target4;\n"
        "    float4 o5 : SV_Target5; float4 o6 : SV_Target6; float4 o7 : SV_Target7; };\n"
        "float2 uvof(float4 t, uint n, float2 pos) {\n"
        "    float2 uv = t.xy;\n"
        "    if (tex_mode[n] == 1u && t.w != 0.0 && t.w != 1.0) uv /= t.w;\n"
        "    return nv2a_snap_uv(uv, pos, tex_scale[n]) * tex_scale[n].xy\n"
    "         + float2(tex_scale[n].w, 0.0);\n"
        "}\n"
        "PO main(VS_OUT i) {\n"
        "    PO o;\n"
        "%s"
        "    return o;\n"
        "}\n",
        VS_OUT_DECL, PS_CB_DECL, D3D8_HLSL_SNAP_UV,
        pass == 0
        ? "    o.o0 = i.oD0; o.o1 = i.oD1; o.o2 = i.oT0; o.o3 = i.oT1;\n"
          "    o.o4 = i.oT2; o.o5 = i.oT3; o.o6 = i.oPos;\n"
          "    o.o7 = float4(i.oFog, 1, 0, 0);\n"
        : "    float2 u0 = uvof(i.oT0, 0, i.oPos.xy), u1 = uvof(i.oT1, 1, i.oPos.xy);\n"
          "    float2 u2 = uvof(i.oT2, 2, i.oPos.xy), u3 = uvof(i.oT3, 3, i.oPos.xy);\n"
          "    o.o0 = tex0.Sample(samp0, u0); o.o1 = tex1.Sample(samp1, u1);\n"
          "    o.o2 = tex2.Sample(samp2, u2); o.o3 = tex3.Sample(samp3, u3);\n"
          "    o.o4 = float4(u0, 0, 0); o.o5 = float4(u1, 0, 0);\n"
          "    o.o6 = float4(u2, 0, 0); o.o7 = float4(u3, 0, 0);\n");
    if (compile("nv2a_ps_probe", src, "ps_5_0", &code)) {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                       ID3D10Blob_GetBufferSize(code), NULL, &ps[pass]);
        ID3D10Blob_Release(code);
    }
    if (!ps[pass])
        fprintf(stderr, LOGP "px: probe shader %d FAILED to build\n", pass);
    return ps[pass];
}

static void px_probe(const struct nv2a_pb_draw *d, VsEntry *vs, RenderTarget *rt,
                     const uint8_t fmts[16], const UINT strides[16], const UINT offs[16],
                     D3D11_PRIMITIVE_TOPOLOGY topo, UINT nidx,
                     ID3D11DepthStencilView *dsv, const uint32_t before[4])
{
    static ID3D11Texture2D *tex[8], *st;
    static ID3D11RenderTargetView *rtv[8];
    static UINT tw, th;
    static ID3D11DepthStencilState *dss_le;
    static const char *names[2][8] = {
        {"D0", "D1", "T0", "T1", "T2", "T3", "pos", "fog/cover"},
        {"tex0", "tex1", "tex2", "tex3", "uv0", "uv1", "uv2", "uv3"}};
    float val[2][4][8][4];
    const float zero[4] = {0, 0, 0, 0};
    D3D11_MAPPED_SUBRESOURCE m;
    int pass, k, n, any = 0;

    if (!px_shader(0) || !px_shader(1))
        return;
    /* At the target's host size: they are bound with its depth view, and
     * D3D11 wants every attachment the same size. */
    if (!tex[0] || tw != rt->hw || th != rt->hh) {
        D3D11_TEXTURE2D_DESC td;
        for (k = 0; k < 8; k++) {
            if (rtv[k]) ID3D11RenderTargetView_Release(rtv[k]);
            if (tex[k]) ID3D11Texture2D_Release(tex[k]);
            rtv[k] = NULL;
            tex[k] = NULL;
        }
        memset(&td, 0, sizeof td);
        td.Width = rt->hw;
        td.Height = rt->hh;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        for (k = 0; k < 8; k++)
            if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &tex[k]))
                    || FAILED(ID3D11Device_CreateRenderTargetView(
                        s_dev, (ID3D11Resource *)tex[k], NULL, &rtv[k])))
                return;
        tw = rt->hw;
        th = rt->hh;
    }
    if (!st) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = 8;
        td.Height = 4;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st)))
            return;
    }
    if (!dss_le) {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        ID3D11Device_CreateDepthStencilState(s_dev, &dd, &dss_le);
    }

    for (pass = 0; pass < 2; pass++) {
        for (k = 0; k < 8; k++)
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, rtv[k], zero);
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 8, rtv, dsv);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, dsv ? dss_le : s_ds_off, 0);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFF);
        ID3D11DeviceContext_PSSetShader(s_ctx, px_shader(pass), NULL, 0);
        ID3D11DeviceContext_DrawIndexed(s_ctx, nidx, 0, 0);
        for (n = 0; n < s_px_n; n++) {
            D3D11_BOX b;
            if (s_px_x[n] < 0 || s_px_y[n] < 0
                    || (UINT)s_px_x[n] >= rt->w || (UINT)s_px_y[n] >= rt->h)
                continue;
            b.left = ((UINT)s_px_x[n] + rt->pad) * rt->n; b.right = b.left + 1;
            b.top = (UINT)s_px_y[n] * rt->n; b.bottom = b.top + 1;
            b.front = 0; b.back = 1;
            for (k = 0; k < 8; k++)
                ID3D11DeviceContext_CopySubresourceRegion(
                    s_ctx, (ID3D11Resource *)st, 0, (UINT)k, (UINT)n, 0,
                    (ID3D11Resource *)tex[k], 0, &b);
        }
        memset(val[pass], 0, sizeof val[pass]);
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0,
                                              D3D11_MAP_READ, 0, &m))) {
            for (n = 0; n < s_px_n; n++)
                memcpy(val[pass][n], (const uint8_t *)m.pData + (size_t)n * m.RowPitch,
                       sizeof val[pass][n]);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
        }
    }
    /* back to the real draw's state; the state-call filter's view of it
     * stays true for everything restored here */
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rt->rtv, dsv);
    {
        /* Recomputed, not s_sc.bs/.bf: setup_pixel can evict a shader
         * after the blend bind, and sc_on() then resets s_sc to 0xFF. The
         * pgraph state is unchanged since the draw, so these are the same. */
        float bf[4];
        blend_color(bf);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, blend_state(), bf, 0xFFFFFFFF);
    }
    ID3D11DeviceContext_PSSetShader(s_ctx, s_sc.ps, NULL, 0);
    s_state_gen++;              /* and the next draw rebinds everything */

    for (n = 0; n < s_px_n; n++) {
        uint32_t after;
        if (val[0][n][7][1] != 1.0f)
            continue;           /* not covered */
        after = read_pixel(rt, s_px_x[n], s_px_y[n]);
        if (!any) {
            s_px_max--;
            fprintf(stderr, "[D3D11-PX] flip %llu draw %llu surface 0x%08X prog %08X"
                    " prim %u n %u\n", (unsigned long long)s_presents,
                    (unsigned long long)s_draws, rt->offset,
                    d->program ? vs->hash : 0u, d->prim, d->idx_count);
        }
        any = 1;
        fprintf(stderr, "[D3D11-PX]  px (%d,%d) before %08X after %08X\n",
                s_px_x[n], s_px_y[n], before[n], after);
        for (pass = 0; pass < 2; pass++)
            for (k = 0; k < 8; k++)
                fprintf(stderr, "[D3D11-PX]   %-9s %g %g %g %g\n", names[pass][k],
                        val[pass][n][k][0], val[pass][n][k][1],
                        val[pass][n][k][2], val[pass][n][k][3]);
    }
    if (any) {
        int px, py;
        trace_batch(d, vs, rt, fmts, strides, offs, topo, &px, &py);
    }
}

static int aspect_trace(void)
{
    static int on = -1;
    if (on < 0)
        on = recomp_env(RENV_ASPECT_TRACE) != NULL;
    return on;
}

/* RECOMP_ASPECT_TRACE: each 2D batch's placement, once per program,
 * stage-0 texture and placement (Metal's aspect_log, same line). */
static void aspect_log(uint32_t hash, uint32_t count, uint32_t ntri, float lo_x,
                       float hi_x, const RenderTarget *rt, int place, int lift, int wide)
{
    static const char *const names[] = { "centre", "stretch", "cover" };
    enum { SET = 4096 };
    static uint64_t set[SET];
    static unsigned used;
    const Stage *t0 = &s_stage[0];
    uint64_t key = ((uint64_t)hash << 32 | (t0->mode ? t0->offset : 0)) * 0x9E3779B97F4A7C15ull
                   + (uint64_t)(place | lift << 4 | wide << 5) + 1u;
    unsigned h = (unsigned)(key >> 52) & (SET - 1u);
    while (set[h] && set[h] != key)
        h = (h + 1u) & (SET - 1u);
    if (set[h] || used >= SET / 2u)
        return;
    set[h] = key;
    used++;
    fprintf(stderr, "[aspect.program] %08X flip %llu: %u vertices %u tris x %g..%g on %ux%u"
            " pad %u at 0x%08X pitch %u, stage 0 %s 0x%08X fmt %X %ux%u pitch %u -> %s%s%s\n", hash,
            (unsigned long long)s_presents, count, ntri, lo_x, hi_x, rt->w, rt->h, rt->pad,
            rt->addr, rt->pitch,
            !t0->mode || t0->mode == 4 ? "none" : s_stage_rt[0] ? "rt" : "tex", t0->offset,
            t0->color, t0->width, t0->height, t0->pitch, names[place], lift ? " lift" : "",
            wide ? " wide" : "");
}

/* RECOMP_ASPECT_TRACE: each program and target size the first time
 * nv2a_aspect_place_tex drops a guest picture: a full-width 2D pass that
 * samples a padded target and a picture, placed as a post pass, not a
 * movie (Metal's aspect_log_post, same line). */
static void aspect_log_post(uint32_t hash, const RenderTarget *rt, int place)
{
    static const char *const names[] = { "centre", "stretch", "cover" };
    enum { SET = 256 };
    static uint64_t set[SET];
    static unsigned used;
    uint64_t key = ((uint64_t)hash << 32 | rt->w << 16 | rt->h) * 0x9E3779B97F4A7C15ull + 1u;
    unsigned h = (unsigned)(key >> 56) & (SET - 1u);
    while (set[h] && set[h] != key)
        h = (h + 1u) & (SET - 1u);
    if (set[h] || used >= SET / 2u)
        return;
    set[h] = key;
    used++;
    fprintf(stderr, "[aspect.post] %08X flip %llu: picture beside a padded target on %ux%u"
            " pad %u at 0x%08X -> %s\n", hash, (unsigned long long)s_presents, rt->w, rt->h,
            rt->pad, rt->addr, names[place]);
}

/* RECOMP_ASPECT_TRACE: each program and w the first time a flat near
 * layer of it is clipped to the guest columns (Metal's aspect_log_flat,
 * same line). */
static void aspect_log_flat(uint32_t hash, uint32_t count, float w, const RenderTarget *rt)
{
    enum { SET = 256 };
    static uint64_t set[SET];
    static unsigned used;
    union { float f; uint32_t u; } wb = { w };
    uint64_t key = ((uint64_t)hash << 32 | wb.u) * 0x9E3779B97F4A7C15ull + 1u;
    unsigned h = (unsigned)(key >> 56) & (SET - 1u);
    while (set[h] && set[h] != key)
        h = (h + 1u) & (SET - 1u);
    if (set[h] || used >= SET / 2u)
        return;
    set[h] = key;
    used++;
    fprintf(stderr, "[aspect.flat] %08X flip %llu: %u vertices at w %g on %ux%u pad %u at"
            " 0x%08X\n", hash, (unsigned long long)s_presents, count, w, rt->w, rt->h, rt->pad,
            rt->addr);
}

/* Whether a program batch (count vertices from lo) is a flat near layer
 * (nv2a_aspect_flat). Its inputs are gathered for the vertices the helper
 * samples only, and the first alone settles a world batch. */
static int aspect_flat(const struct nv2a_pb_draw *d, const struct nv2a_pb_vsh *v,
                       const VsEntry *vs, const RenderTarget *rt, uint32_t lo, uint32_t count)
{
    /* nv2a_aspect_flat's sample (nv2a_aspect_flat_step), gathered here;
     * at most NV2A_ASPECT_FLAT_MAX vertices, which the helper then takes
     * at step 1, so both backends run the same vertices. */
    static float vf[NV2A_ASPECT_FLAT_MAX * NV2A_VS_MAX_INPUTS * 4];
    int slot[NV2A_VS_MAX_INPUTS];
    uint32_t step = nv2a_aspect_flat_step(count), j, n = 0;
    unsigned nf4 = 0, i;
    float w;

    if (!count)
        return 0;
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++)
        if (vs->inputs & (1u << i))
            slot[nf4++] = (int)i;
    for (i = 0; i < nf4; i++)
        vtx_input(d, v, (unsigned)slot[i], lo, vf + (size_t)i * 4u);
    /* The first vertex alone rules out the world's batches. */
    if (!nv2a_aspect_flat((const uint32_t (*)[4])v->prog, v->start,
                          (float (*)[4])(uintptr_t)v->c, vf, nf4, slot, 1, NULL))
        return 0;
    for (j = 0; j < count && n < NV2A_ASPECT_FLAT_MAX; j += step, n++)
        for (i = 0; i < nf4; i++)
            vtx_input(d, v, (unsigned)slot[i], lo + j, vf + ((size_t)n * nf4 + i) * 4u);
    if (!nv2a_aspect_flat((const uint32_t (*)[4])v->prog, v->start,
                          (float (*)[4])(uintptr_t)v->c, vf, nf4, slot, n, &w))
        return 0;
    if (aspect_trace())
        aspect_log_flat(vs->hash, count, w, rt);
    return 1;
}

/* The pixels->NDC mapping at b2 and the scissor for one placement
 * (nv2a_aspect_map, nv2a_aspect_box; with pad 0 both are stock). cb is
 * the surface clip (nv2a_draw_box). */
static void set_vp(const struct nv2a_pb_vsh *v, const struct nv2a_pb_gpu *g,
                   const RenderTarget *rt, int place, int two_d, int lift,
                   const uint32_t cb[4])
{
    static uint8_t vp_shadow[80];
    static int vp_ok;
    D3D11_MAPPED_SUBRESOURCE m;
    struct nv2a_vp_consts vc;
    D3D11_RECT sr;
    uint32_t hb[4];
    uint8_t vp[80];

    nv2a_vp_consts_fill(v, g->format, rt->w, rt->h, rt->pad, &vc);
    nv2a_aspect_map(&vc, rt->w, rt->h, rt->pad, place);
    nv2a_vp_tie_fill(&vc, rt->hw, rt->hh);
    memcpy(vp, vc.c, sizeof vc.c);
    memcpy(vp + sizeof vc.c, vc.u, sizeof vc.u);
    if (memo_on() && vp_ok && !memcmp(vp_shadow, vp, sizeof vp)) {
        /* unchanged */
    } else if (SUCCEEDED(ID3D11DeviceContext_Map(
            s_ctx, (ID3D11Resource *)s_cb_vp, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, vp, sizeof vp);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_vp, 0);
        memcpy(vp_shadow, vp, sizeof vp);
        vp_ok = 1;
    }
    nv2a_aspect_box(cb, rt->w, rt->h, rt->pad, rt->n, place, two_d, lift, hb);
    sr.left = (LONG)hb[0];
    sr.top = (LONG)hb[1];
    sr.right = (LONG)hb[2];
    sr.bottom = (LONG)hb[3];
    if (!sc_on() || memcmp(&s_sc.sr, &sr, sizeof sr)) {
        ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sr);
        s_sc.sr = sr;
    }
}

static void d3d11_on_draw(const struct nv2a_pb_draw *d)
{
    /* As Metal's: at most three list entries per index, grown to the
     * largest batch so far rather than sized to NV_MAX_INDICES. */
    static uint32_t *idx;
    static uint32_t idx_cap;
    static float (*hxy)[2];        /* hor+: a 2D batch's vertex x, y (full width) */
    static uint32_t hxy_cap;
    static float *hvf;             /* hor+: a program batch's inputs, for the scan */
    static size_t hvf_cap;
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    struct aspect_batch hp = {0, 0, NV2A_PLACE_CENTRE, 0, NULL};
    int have_x = 0, tris = 0, flat = 0, lift = 0, wide = 0, scan;
    float lo_x = 0, hi_x = 0;      /* hor+: a 2D batch's x range, surface pixels */
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11Buffer *vbs[NV2A_VS_MAX_INPUTS];
    UINT strides[NV2A_VS_MAX_INPUTS], offs[NV2A_VS_MAX_INPUTS];
    RenderTarget *rt;
    VsEntry *vs;
    ID3D11RasterizerState *rs;
    int cull_all;
    uint8_t fmts[16];
    uint32_t lo = 0xFFFF, hi = 0, count, nidx, i, k;
    void *p;

    if (!init())
        return;
    /* A mod's batch filter (nv2a_backend_common.h); Metal asks it at the
     * same point. Nothing without the enhancements layer. */
    if (nv2a_batch_skip(d, g, v))
        return;
    rt = surface();
    vs = vertex_shader(d->program);
    if (!rt || !vs || !d->idx_count) {
        s_draws_skipped++;
        return;
    }
    for (i = 0; i < d->idx_count; i++) {
        if (d->idx[i] < lo) lo = d->idx[i];
        if (d->idx[i] > hi) hi = d->idx[i];
    }
    count = hi - lo + 1;
    if (d->idx_count * 3u > idx_cap) {
        uint32_t *grown = (uint32_t *)realloc(idx, (size_t)d->idx_count * 12u);
        if (!grown) {
            s_draws_skipped++;
            return;
        }
        idx = grown;
        idx_cap = d->idx_count * 3u;
    }
    nidx = convert_indices(d, lo, idx, &topo);
    rs = rasterizer_state(&cull_all);
    if (!nidx) {
        s_draws_skipped++;
        return;
    }
    if (cull_all && (topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                     || topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)) {
        s_draws_culled++;
        return;
    }

    /* Streams: each one the shader reads. One in a format the input
     * assembler reads natively is copied as it is: vertices lo..hi of its
     * buffer in one block, shared by every stream interleaved in it (same
     * stride, within one stride of each other), bound at their own offsets.
     * The rest go as float4 per vertex, or one float4 at stride 0: a
     * stride-0 array's one element (every vertex reads it, as fetch_attr
     * does), or the current SET_VERTEX_DATA value if disabled. */
    memset(vbs, 0, sizeof vbs);
    memset(strides, 0, sizeof strides);
    memset(offs, 0, sizeof offs);
    memset(fmts, 0, sizeof fmts);
    {   /* the most the streams below can take, each 16-aligned */
        uint64_t bound = 0;
        for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
            const VertexAttr *a = &d->attr[i];
            if (!(vs->inputs & (1u << i)))
                continue;
            bound += (a->size && a->stride)
                   ? (uint64_t)count * (a->stride > 16 ? a->stride : 16) + 48 : 32;
        }
        vb_begin(bound <= 0xFFFFFFFFu ? (UINT)bound : 0);
    }
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        const VertexAttr *a = &d->attr[i];
        int on = a->size && a->stride;
        UINT bytes = on ? count * 16u : 16u, esz, at;
        float *f;

        if (!(vs->inputs & (1u << i)) || fmts[i])
            continue;
        if (on && (d->inline_data || a->offset) && !(a->stride & 3)
                && native_format(a, &esz)) {
            uint32_t lo_off = a->offset, hi_end = a->offset + esz, group = 0, j;
            const uint8_t *base = d->inline_data
                ? (const uint8_t *)d->inline_buf
                : (const uint8_t *)xbox_GetMemoryOffset();
            for (j = i; j < NV2A_VS_MAX_INPUTS; j++) {
                const VertexAttr *b = &d->attr[j];
                UINT bsz;
                if (!(vs->inputs & (1u << j)) || fmts[j] || !b->size
                        || b->stride != a->stride || !native_format(b, &bsz)
                        || (!d->inline_data && !b->offset)
                        || (b->offset > a->offset ? b->offset - a->offset
                                                  : a->offset - b->offset) >= a->stride)
                    continue;
                group |= 1u << j;
                if (b->offset < lo_off) lo_off = b->offset;
                if (b->offset + bsz > hi_end) hi_end = b->offset + bsz;
            }
            bytes = (count - 1u) * a->stride + (hi_end - lo_off);
            if (!d->inline_data
                    || (size_t)lo_off + (size_t)lo * a->stride + bytes <= d->inline_bytes) {
                uint8_t *dst = (uint8_t *)vb_alloc(bytes, &at);
                if (!dst) {
                    vb_end();
                    s_draws_skipped++;
                    return;
                }
                memcpy(dst, base + lo_off + (size_t)lo * a->stride, bytes);
                vb_written();
                for (j = i; j < NV2A_VS_MAX_INPUTS; j++) {
                    if (!(group & (1u << j)))
                        continue;
                    fmts[j] = (uint8_t)native_format(&d->attr[j], &esz);
                    offs[j] = at + (d->attr[j].offset - lo_off);
                    strides[j] = a->stride;
                    vbs[j] = s_vb;
                }
                s_vb_native += bytes;
                continue;
            }
            bytes = count * 16u;
        }
        fmts[i] = (uint8_t)DXGI_FORMAT_R32G32B32A32_FLOAT;
        f = (float *)vb_alloc(bytes, &at);
        if (!f) {
            vb_end();
            s_draws_skipped++;
            return;
        }
        offs[i] = at;
        if (on) {
            for (k = 0; k < count; k++) {
                if (!fetch(d, a, lo + k, f + 4 * k))
                    memcpy(f + 4 * k, v->inl[i], 16);
            }
            strides[i] = 16;
        } else {
            if (!a->size || !fetch(d, a, 0, f))
                memcpy(f, v->inl[i], 16);
            strides[i] = 0;
        }
        s_vb_expanded += bytes;
        vb_written();
        vbs[i] = s_vb;
    }
    vb_end();
    p = ring_map(s_ib, s_ib_size, &s_ib_pos, nidx * 4u, 4);
    if (!p) {
        s_draws_skipped++;
        return;
    }
    memcpy(p, idx, nidx * 4u);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ib, 0);

    /* Hor+: which batches are 2D, and their x range (nv2a_aspect_place
     * decides from it), as metal_on_draw does. A pass-through batch is 2D,
     * its positions in surface pixels; a program batch is 2D when its own
     * output is orthographic (nv2a_aspect_scan). Its inputs are gathered
     * as float4 only when the gate is due to run it. With render.snap_2d
     * above render scale 1 the 2D test runs without hor+ too: a 2D batch's
     * stages sample at their 1x points (nv2a_snap_flat2d). */
    scan = rt->pad || nv2a_snap_2d_on(rt->n);
    tris = rt->pad && topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    if (tris && count > hxy_cap) {
        float (*grown)[2] = (float (*)[2])realloc(hxy, (size_t)count * sizeof *hxy);
        if (grown) {
            hxy = grown;
            hxy_cap = count;
        } else {
            tris = 0;
        }
    }
    if (rt->pad && !d->program && (vs->inputs & 1u)) {
        for (k = 0; k < count; k++) {
            float o[4];
            vtx_input(d, v, 0, lo + k, o);
            if (!k || o[0] < lo_x) lo_x = o[0];
            if (!k || o[0] > hi_x) hi_x = o[0];
            if (tris) {
                hxy[k][0] = o[0];
                hxy[k][1] = o[1];
            }
        }
        hp.two_d = have_x = 1;
    } else if (!rt->pad && scan && !d->program) {
        hp.two_d = 1;
    } else if (scan && d->program && nv2a_aspect_due(&vs->hp)) {
        int slot[NV2A_VS_MAX_INPUTS];
        unsigned nf4 = 0;
        for (i = 0; i < NV2A_VS_MAX_INPUTS; i++)
            if (vs->inputs & (1u << i))
                slot[nf4++] = (int)i;
        if ((size_t)count * nf4 * 4u > hvf_cap) {
            float *grown = (float *)realloc(hvf, (size_t)count * nf4 * 16u);
            if (grown) {
                hvf = grown;
                hvf_cap = (size_t)count * nf4 * 4u;
            }
        }
        if ((size_t)count * nf4 * 4u <= hvf_cap) {
            for (k = 0; k < count; k++)
                for (i = 0; i < nf4; i++)
                    vtx_input(d, v, (unsigned)slot[i], lo + k, hvf + ((size_t)k * nf4 + i) * 4u);
            hp.two_d = have_x = nv2a_aspect_scan(&vs->hp, (const uint32_t (*)[4])v->prog,
                                                 v->start, (float (*)[4])(uintptr_t)v->c,
                                                 hvf, nf4, slot, count, NULL,
                                                 &lo_x, &hi_x);
            hp.vary_z = have_x && vs->hp.vary_z;
            /* Full-width bounds: every vertex, for the per-triangle test
             * (a 2D program is scanned every batch, so the gate passes). */
            if (have_x && tris && nv2a_aspect_full_width(lo_x, hi_x, rt->w))
                nv2a_aspect_scan(&vs->hp, (const uint32_t (*)[4])v->prog, v->start,
                                 (float (*)[4])(uintptr_t)v->c, hvf, nf4, slot, count, hxy,
                                 &lo_x, &hi_x);
        }
    } else if (scan && d->program) {
        /* not due: count the batch so the gate's cadence holds */
        nv2a_aspect_scan(&vs->hp, NULL, 0, NULL, NULL, 0, NULL, 0, NULL, &lo_x, &hi_x);
    }
    if (have_x)
        hp.full_w = nv2a_aspect_full(lo_x, hi_x, tris ? (const float (*)[2])hxy : NULL,
                                     tris ? idx : NULL, nidx / 3u, rt->w, rt->h, &wide);
    else if (rt->pad && d->program && topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
             && count <= NV2A_ASPECT_QUAD_MAX) {
        hp.d = d;
        hp.vs = vs;
        hp.idx = idx;
        hp.lo = lo;
        hp.count = count;
        hp.nidx = nidx;
    }

    /* Constants: the program's c[192] at b1. The pixels->NDC mapping at b2
     * waits for the placement (setup_pixel, below). */
    {
        static uint8_t consts_shadow[sizeof v->c];
        static int consts_ok;
        static uint32_t consts_gen;
        uint32_t gen = nv2a_pb_vsh_consts_gen();
        D3D11_MAPPED_SUBRESOURCE m;
        /* Most batches find c[] untouched since the last upload (BLiNX 2's
         * 1-2 writes it before one batch in fifteen of its ~10k a frame),
         * and the 3 KB compare that finds that out was ~2 ms a frame under
         * Proton, whose memcmp goes 8 bytes at a time. No write since the
         * upload: nothing to compare. */
        if (!d->program || (memo_on() && consts_ok && consts_gen == gen)) {
            /* the buffer already holds these bytes */
        } else if (memo_on() && consts_ok
                   && !memcmp(consts_shadow, v->c, sizeof v->c)) {
            consts_gen = gen;
        } else if (SUCCEEDED(ID3D11DeviceContext_Map(
                s_ctx, (ID3D11Resource *)s_cb_consts, 0,
                D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, v->c, sizeof v->c);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_consts, 0);
            memcpy(consts_shadow, v->c, sizeof v->c);
            consts_ok = 1;
            consts_gen = gen;
        }
    }

    {
        D3D11_VIEWPORT vp;
        ID3D11Buffer *cbs[3];
        uint32_t cb[4];
        vp.TopLeftX = 0;
        vp.TopLeftY = 0;
        vp.Width = (float)rt->hw;
        vp.Height = (float)rt->hh;
        vp.MinDepth = 0;
        vp.MaxDepth = 1;
        /* The surface clip, as the hardware scissors (nv2a_draw_box). */
        if (!nv2a_draw_box(g, rt->w, rt->h, cb)) {
            s_draws_skipped++;
            return;
        }
        cbs[0] = NULL;
        cbs[1] = s_cb_consts;
        cbs[2] = s_cb_vp;
        ID3D11DepthStencilView *dsv = NULL;
        ID3D11DepthStencilState *dss = s_ds_off;
        /* Stencil only on a Z24S8 zeta surface, as on the CPU. */
        int use_s = v->stencil_enable && ((g->format >> 4) & 0xF) == 2;
        if (v->depth_enable || use_s) {
            DepthTarget *dt = depth_target(rt->w, rt->h);
            if (dt) {
                dsv = dt->dsv;
                dss = depth_state(v->depth_enable, use_s);
                s_draws_z++;
            }
        }
        {
            /* BLiNX 2's 1-2 rebinds the same target and depth state for
             * nearly all of its ~10k batches a frame: ~2 ms a frame of
             * DXVK under Proton on the Deck. */
            UINT sref = use_s ? v->stencil_ref & 0xFFu : 0;
            int sc = sc_on();
            if (!sc || s_sc.rtv != rt->rtv || s_sc.dsv != dsv) {
                ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rt->rtv, dsv);
                s_sc.rtv = rt->rtv;
                s_sc.dsv = dsv;
            }
            if (!sc || s_sc.dss != dss || s_sc.sref != sref) {
                ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, dss, sref);
                s_sc.dss = dss;
                s_sc.sref = sref;
            }
        }
        {
            ID3D11BlendState *bs = blend_state();
            ID3D11InputLayout *il = input_layout(vs, fmts);
            int sc = sc_on();
            float bf[4];
            s_draws_bconst += (uint64_t)blend_color(bf);
            if (!sc || s_sc.bs != bs || memcmp(s_sc.bf, bf, sizeof bf)) {
                ID3D11DeviceContext_OMSetBlendState(s_ctx, bs, bf, 0xFFFFFFFF);
                s_sc.bs = bs;
                memcpy(s_sc.bf, bf, sizeof bf);
            }
            if (!sc || memcmp(&s_sc.vp, &vp, sizeof vp)) {
                ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
                s_sc.vp = vp;
            }
            if (!sc || s_sc.rs != rs) {
                ID3D11DeviceContext_RSSetState(s_ctx, rs);
                s_sc.rs = rs;
            }
            if (!sc || s_sc.il != il) {
                ID3D11DeviceContext_IASetInputLayout(s_ctx, il);
                s_sc.il = il;
            }
            ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, NV2A_VS_MAX_INPUTS,
                                                   vbs, strides, offs);
            ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT,
                                                 s_ib_pos - nidx * 4u);
            if (!sc || s_sc.topo != topo) {
                ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, topo);
                s_sc.topo = topo;
            }
            if (!sc || s_sc.vs != vs->vs) {
                ID3D11DeviceContext_VSSetShader(s_ctx, vs->vs, NULL, 0);
                s_sc.vs = vs->vs;
            }
            if (!sc || memcmp(s_sc.vs_cb, cbs, sizeof cbs)) {
                ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 3, cbs);
                memcpy(s_sc.vs_cb, cbs, sizeof cbs);
            }
        }
        s_cur = rt;
        {
            static int skip_post = -1;
            ID3D11PixelShader *ps;
            if (skip_post < 0)
                skip_post = recomp_env(RENV_D3D11_DEBUG_PS_SKIP_POST) != NULL;
            s_dbg_post = skip_post && !v->depth_enable && d->idx_count <= 8;
            ps = setup_pixel(&hp);
            /* setup_pixel may have compiled (and evicted) a shader */
            if (!sc_on() || s_sc.ps != ps) {
                ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
                s_sc.ps = ps;
            }
        }
        if (hp.place != NV2A_PLACE_CENTRE)
            s_draws_stretch++;
        if (hp.two_d && !have_x) {
            lo_x = hp.lo_x;     /* a screen quad (aspect_quad) */
            hi_x = hp.hi_x;
        }
        /* A flat near layer drawn without depth keeps its place but is
         * clipped to the guest columns like the 2D (nv2a_aspect_flat), as
         * in metal_on_draw. */
        flat = rt->pad && d->program && !hp.two_d && !v->depth_enable
               && aspect_flat(d, v, vs, rt, lo, count);
        /* A HUD mod clipped its elements in guest memory: lift the 4:3
         * clip of the 2D it moved (nv2a_aspect_hud_lift). */
        lift = rt->pad && nv2a_aspect_hud_lift(rt->w, rt->h, hp.two_d, flat, hp.full_w,
                                               have_x ? lo_x : 0.0f,
                                               have_x ? hi_x : (float)rt->w);
        if (hp.two_d && rt->pad && aspect_trace())
            aspect_log(vs->hash, count, nidx / 3u, lo_x, hi_x, rt, hp.place, lift, wide);
        if (hp.post_tex && aspect_trace())
            aspect_log_post(vs->hash, rt, hp.place);
        set_vp(v, g, rt, hp.place, hp.two_d || flat, lift, cb);
        if (px_on()) {
            uint32_t before[4];
            int n;
            for (n = 0; n < s_px_n; n++)
                before[n] = read_pixel(rt, s_px_x[n], s_px_y[n]);
            ID3D11DeviceContext_DrawIndexed(s_ctx, nidx, 0, 0);
            px_probe(d, vs, rt, fmts, strides, offs, topo, nidx, dsv, before);
        } else {
            ID3D11DeviceContext_DrawIndexed(s_ctx, nidx, 0, 0);
        }
    }
    rt->last_draw = ++s_seq;
    s_cur = NULL;
    s_draws += d->batches;
    if (d->program)
        s_draws_prog += d->batches;
}

/* ---- flip ------------------------------------------------------------------ */

/* RECOMP_D3D11_DUMP=<prefix>: every 60th presented frame as <prefix>NNNN.bmp,
 * read back from the swap chain's buffer. Proof of what reached the screen.
 * The prefix is prepended to the file name as is; one ending in / or \ is a
 * directory, and the files in it are named frame_NNNN.bmp. The flips listed
 * in RECOMP_FB_DUMP_AT are written too, as <prefix>flip_NNNNN.bmp named by
 * present number (= flip number), so a golden run can pick its frames by an
 * event's flip rather than every 60th present. */
/* w x h 32-bit pixels (B8G8R8A8, `pitch` bytes a row) as a top-down BMP,
 * with the dump's one line either way. */
static int write_bmp32(const char *path, const uint8_t *px, size_t pitch, UINT w, UINT h)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    FILE *f = fopen(path, "wb");
    UINT y;
    int ok = 1;

    if (!f) {
        fprintf(stderr, LOGP "dump %s: cannot open it\n", path);
        return 0;
    }
    memset(&fh, 0, sizeof fh);
    memset(&ih, 0, sizeof ih);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + w * h * 4;
    ih.biSize = sizeof ih;
    ih.biWidth = (LONG)w;
    ih.biHeight = -(LONG)h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    if (fwrite(&fh, sizeof fh, 1, f) != 1 || fwrite(&ih, sizeof ih, 1, f) != 1)
        ok = 0;
    for (y = 0; ok && y < h; y++)
        if (fwrite(px + (size_t)y * pitch, (size_t)w * 4, 1, f) != 1)
            ok = 0;
    if (fclose(f))
        ok = 0;
    if (ok)
        fprintf(stderr, LOGP "dumped %s\n", path);
    else
        fprintf(stderr, LOGP "dump %s: write failed\n", path);
    return ok;
}

/* The frame of this flip as a BMP: the back buffer after the direct copy
 * (the stock path, as always), or the present RT itself when the flip went
 * through the scaling blit, so a window size or a filter never reaches the
 * dump (under render.scale the RT is host-sized: dumps are hw x hh). */
static int dump_texture(ID3D11Texture2D *src, const char *path)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    int ok = 0;

    if (!src) {
        /* No back buffer after a failed resize (d3d11_on_flip said so). */
        return 0;
    }
    ID3D11Texture2D_GetDesc(src, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st)))
        return 0;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st,
                                     (ID3D11Resource *)src);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0,
                                          D3D11_MAP_READ, 0, &m))) {
        ok = write_bmp32(path, (const uint8_t *)m.pData, m.RowPitch, td.Width, td.Height);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
    return ok;
}

static UINT s_shown_w, s_shown_h;   /* the last flip's present extent */

#ifdef XBOXRECOMP_ENHANCE
/* The HUD capture's picture (nv2a_backend_common.h): rt's frame, cw x ch
 * guest pixels (its present extent, the hor+ pads left out), at guest size.
 * Under render.scale each n x n block of host pixels is averaged, as
 * Metal's write-back does, so the BMP is the guest surface that Metal and
 * the CPU path write from guest memory and a guest-pixel bbox lands on the
 * same pixels on every backend. A grown target or a crop never changes its
 * size. The take maps a staging copy here, synchronously. */
static int dump_guest_frame(RenderTarget *rt, UINT cw, UINT ch, const char *path)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_BOX b;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D *st = NULL;
    unsigned n = rt->n ? rt->n : 1;
    int ok = 0;

    if (!rt->tex || !cw || !ch || (rt->pad + cw) * n > rt->hw || ch * n > rt->hh)
        return 0;
    ID3D11Texture2D_GetDesc(rt->tex, &td);
    td.Width = cw * n;
    td.Height = ch * n;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st)))
        return 0;
    b.left = rt->pad * n;
    b.top = 0;
    b.front = 0;
    b.right = b.left + cw * n;
    b.bottom = ch * n;
    b.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)st, 0, 0, 0, 0,
                                              (ID3D11Resource *)rt->tex, 0, &b);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0,
                                          &m))) {
        if (n == 1) {
            ok = write_bmp32(path, (const uint8_t *)m.pData, m.RowPitch, cw, ch);
        } else {
            uint8_t *px = (uint8_t *)malloc((size_t)cw * ch * 4u);
            if (px) {
                nv2a_downscale_box32((const uint8_t *)m.pData, m.RowPitch, cw, ch, n, px,
                                     cw * 4u);
                ok = write_bmp32(path, px, (size_t)cw * 4u, cw, ch);
                free(px);
            }
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
    return ok;
}
#endif

static void dump_back_buffer(const char *path)
{
    dump_texture(s_dump_rt && s_drawn && s_drawn->tex ? s_drawn->tex : s_back, path);
}

#ifdef XBOXRECOMP_ENHANCE
static int s_chained;                   /* this flip went through present.shader */
#endif

static void dump_frame(void)
{
    static const char *prefix;
    static int checked;
    char path[MAX_PATH];
    size_t n;
    int dir;

    if (!checked) {
        checked = 1;
        prefix = recomp_env(RENV_D3D11_DUMP);
    }
#ifdef XBOXRECOMP_ENHANCE
    /* The HUD capture's picture of the guest frame this present shows
     * (nv2a_backend_common.h): the guest render target, before any scaling
     * blit or shader, at guest size. Taken under the walker's flip number,
     * which the request names: s_presents misses the flips before init().
     * No frame (no target picked): the picture fails, never the window's
     * back buffer at another size. One load while nothing is posted. */
    if (nv2a_cap_pending()) {
        char cpath[NV2A_CAP_PATH];
        int slot = nv2a_cap_take(nv2a_pb_flips(), cpath, sizeof cpath);
        if (slot >= 0)
            nv2a_cap_done(slot, s_drawn && dump_guest_frame(s_drawn, s_shown_w, s_shown_h,
                                                            cpath));
    }
#endif
    if (!prefix) {
        /* fb_dump_at and fb_dump_prog leave their flips to this backend,
         * which writes them only next to its d3d11_dump frames. */
        static int warned;
        if (!warned && nv2a_pb_dump_at_listed((uint32_t)s_presents)) {
            warned = 1;
            fprintf(stderr, LOGP "WARNING: %s is set without d3d11_dump;"
                    " no flips will be dumped\n",
                    recomp_env(RENV_FB_DUMP_AT) ? "fb_dump_at" : "fb_dump_prog");
        }
        return;
    }
    n = strlen(prefix);
    dir = n && (prefix[n - 1] == '/' || prefix[n - 1] == '\\');
    if ((s_presents % 60) == 1) {
        snprintf(path, sizeof path, "%s%s%04u.bmp", prefix, dir ? "frame_" : "",
                 (unsigned)(s_presents / 60));
        dump_back_buffer(path);
    }
    if (nv2a_pb_dump_at_listed((uint32_t)s_presents)) {
        snprintf(path, sizeof path, "%sflip_%05u.bmp", prefix,
                 (unsigned)s_presents);
        dump_back_buffer(path);
#ifdef XBOXRECOMP_ENHANCE
        /* What present.shader put on screen, the overlay not yet drawn. */
        if (s_chained) {
            snprintf(path, sizeof path, "%spresent_%05u.bmp", prefix, (unsigned)s_presents);
            dump_texture(s_back, path);
        }
#endif
    }
}

static struct nv2a_occ s_occ;           /* see "Visibility tests" below */

/* The surface a flip puts on screen: the walker's pick (present_pick in
 * nv2a_pb_exec.c, the same for every backend), as the render target at that
 * offset and pitch (a field buffer and a frame can share an offset),
 * whatever its size: a smaller one (a buffer drawn only below the
 * display mode's frame) is grown by present_extent. The walker hands its
 * offset over as `surface_offset`. With no render target for it here yet,
 * the frame on screen stays. */
static RenderTarget *present_target(uint32_t surface_offset)
{
    const struct nv2a_pb_present *p = nv2a_pb_present_state();
    int i;

    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->tex && surface_offset && rt->offset == surface_offset
                && (!p->offset || rt->pitch == p->pitch))
            return rt;
    }
    /* The frame on screen stays, even when it is the pick's offset at
     * another pitch (a field buffer for a frame): a NULL here would present
     * an undefined back buffer. present_extent matches the pitch too, so it
     * neither grows nor crops that target as if it were the pick. */
    return s_drawn;
}

/* RECOMP_FLIP_LOG: a line per flip, just before the walker's "[GPU] flip"
 * one, with what this backend did since the last. The walker's own "batches"
 * counts the CPU rasteriser's batches, so under this backend it reads 0;
 * "batches" here is the same quantity for D3D11 (3D frames have them, movie
 * frames do not); "vb" is vertex bytes copied as they are / expanded to
 * float4; the flip number is this backend's present count. */
static void flip_log(uint32_t surface_offset)
{
    static int on = -1;
    static uint64_t p_draws, p_prog, p_tex, p_rtt, p_clears, p_up, p_chg,
                    p_re, p_vn, p_ve, p_skip, p_cull, p_self, p_mip,
                    p_occ, p_occ_vis, p_occ_us, p_occ_late, p_occ_super,
                    p_occ_set, p_occ_stale, p_occ_first, p_loop;
    static int zt = -1;
    /* With a pack, its hit count every 600 flips, flip trace or not. */
    if (nv2a_tex_pack_count() && s_presents % 600 == 0) {
        fprintf(stderr, LOGP "textures.pack: %llu hits so far\n",
                (unsigned long long)nv2a_tex_pack_hits());
        if (recomp_env(RENV_TEX_PACK_TRACE))
            nv2a_tex_pack_print_misses();
    }
    if (on < 0) {
        on = recomp_env(RENV_FLIP_LOG) != NULL;
        zt = recomp_env(RENV_ZPASS_TRACE) != NULL;
    }
    /* The visibility totals so far: a run killed by its timeout has no
     * exit to print them at, so the last of these is the run's. */
    if ((on || zt) && s_occ.reports && s_presents % 300 == 0)
        nv2a_occ_summary(&s_occ, "[D3D11]");
    if (!on)
        return;
    fprintf(stderr, "[D3D11] flip %llu batches %llu prog %llu tex %llu rtt %llu (self %llu)"
            " skipped %llu culled %llu clears %llu uploads %llu (mip %llu) changed %llu rehash %llu"
            " memo %llu vb %llu/%llu bytes occ %llu (visible %llu, pending %d, late %llu, superseded %llu,"
            " settled %llu (stale %llu, first %llu), sync %llu us)"
            " present 0x%08X walker 0x%08X\n",
            (unsigned long long)s_presents,
            (unsigned long long)(s_draws - p_draws),
            (unsigned long long)(s_draws_prog - p_prog),
            (unsigned long long)(s_draws_tex - p_tex),
            (unsigned long long)(s_rtt_binds - p_rtt),
            (unsigned long long)(s_self_copies - p_self),
            (unsigned long long)(s_draws_skipped - p_skip),
            (unsigned long long)(s_draws_culled - p_cull),
            (unsigned long long)(s_clears - p_clears),
            (unsigned long long)(s_tex_uploads - p_up),
            (unsigned long long)(s_tex_mip_uploads - p_mip),
            (unsigned long long)(s_tex_changed - p_chg),
            (unsigned long long)(s_tex_rehash - p_re),
            (unsigned long long)(s_loop_hits - p_loop),
            (unsigned long long)(s_vb_native - p_vn),
            (unsigned long long)(s_vb_expanded - p_ve),
            (unsigned long long)(s_occ.reports - p_occ),
            (unsigned long long)(s_occ.vis - p_occ_vis), s_occ.npend,
            (unsigned long long)(s_occ.late - p_occ_late),
            (unsigned long long)(s_occ.super - p_occ_super),
            (unsigned long long)(s_occ.settled - p_occ_set),
            (unsigned long long)(s_occ.stale - p_occ_stale),
            (unsigned long long)(s_occ.first - p_occ_first),
            (unsigned long long)(s_occ.sync_us - p_occ_us),
            s_drawn ? s_drawn->offset : 0, surface_offset);
    if (s_drawn && s_drawn->pad && aspect_trace()) {
        static uint64_t p_stretch;
        fprintf(stderr, LOGP "flip %llu hor+: target %ux%u pad %u (%ux%u host),"
                " %llu batches off-centre\n", (unsigned long long)s_presents,
                s_drawn->w, s_drawn->h, s_drawn->pad, s_drawn->hw, s_drawn->hh,
                (unsigned long long)(s_draws_stretch - p_stretch));
        p_stretch = s_draws_stretch;
    }
    p_draws = s_draws; p_prog = s_draws_prog; p_tex = s_draws_tex;
    p_rtt = s_rtt_binds; p_skip = s_draws_skipped; p_cull = s_draws_culled; p_clears = s_clears;
    p_up = s_tex_uploads; p_chg = s_tex_changed; p_re = s_tex_rehash; p_loop = s_loop_hits;
    p_vn = s_vb_native; p_ve = s_vb_expanded; p_self = s_self_copies;
    p_mip = s_tex_mip_uploads;
    p_occ = s_occ.reports; p_occ_vis = s_occ.vis; p_occ_us = s_occ.sync_us;
    p_occ_late = s_occ.late; p_occ_super = s_occ.super;
    p_occ_set = s_occ.settled; p_occ_stale = s_occ.stale; p_occ_first = s_occ.first;
}

/* ---- present: the scaling blit and the resize (E1) ------------------------
 *
 * Stock (render.scale 1, nearest, the window as created) presents by copy,
 * as it always did. Anything else -- a scaled frame, a resized or
 * fullscreen window, linear or integer filtering -- draws the present RT
 * into nv2a_present_rect's rectangle of a cleared back buffer with one
 * full-screen triangle. */
static const char s_blit_vs_src[] =
    "struct VO { float4 p : SV_Position; float2 t : TEXCOORD0; };\n"
    /* xy: the share of the target shown; zw: where it starts, the
     * display.overscan crop's offset (0 without one) */
    "cbuffer C : register(b0) { float4 uv_scale; };\n"
    "VO main(uint i : SV_VertexID) {\n"
    "    VO o;\n"
    "    o.t = float2((i << 1) & 2, i & 2);\n"
    "    o.p = float4(o.t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    o.t = o.t * uv_scale.xy + uv_scale.zw;\n"
    "    return o;\n"
    "}\n";
static const char s_blit_ps_src[] =
    "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
    "float4 main(float4 p : SV_Position, float2 t : TEXCOORD0) : SV_Target {\n"
    "    return float4(t0.Sample(s0, t).rgb, 1);\n"
    "}\n";
static ID3D11VertexShader *s_blit_vs;
static ID3D11PixelShader  *s_blit_ps;
static ID3D11SamplerState *s_blit_smp[2];   /* point, linear */
static ID3D11Buffer       *s_blit_cb;       /* b0: the share of the target shown */
static int                 s_blit_failed;

static int blit_init(void)
{
    ID3DBlob *code = NULL;
    D3D11_SAMPLER_DESC sd;
    int k;

    if (s_blit_ps || s_blit_failed)
        return s_blit_ps != NULL;
    s_blit_failed = 1;
    if (!compile("nv2a_present_vs", s_blit_vs_src, "vs_5_0", &code))
        return 0;
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                    ID3D10Blob_GetBufferSize(code), NULL, &s_blit_vs);
    ID3D10Blob_Release(code);
    if (!s_blit_vs || !compile("nv2a_present_ps", s_blit_ps_src, "ps_5_0", &code))
        return 0;
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                   ID3D10Blob_GetBufferSize(code), NULL, &s_blit_ps);
    ID3D10Blob_Release(code);
    for (k = 0; k < 2; k++) {
        memset(&sd, 0, sizeof sd);
        sd.Filter = k ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        ID3D11Device_CreateSamplerState(s_dev, &sd, &s_blit_smp[k]);
        if (!s_blit_smp[k])
            return 0;
    }
    s_blit_cb = make_buffer(16, D3D11_BIND_CONSTANT_BUFFER);
    if (!s_blit_ps || !s_blit_cb)
        return 0;
    s_blit_failed = 0;
    return 1;
}

/* Resize the swap chain to cw x ch (FLIP_DISCARD keeps no contents worth
 * keeping) and re-read the back buffer's size. */
static void swap_resize(UINT cw, UINT ch)
{
    HRESULT hr;

    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    s_sc_init = 0;                  /* behind the draw path's state shadow */
    if (s_back_rtv) {
        ID3D11RenderTargetView_Release(s_back_rtv);
        s_back_rtv = NULL;
    }
    if (s_back) {
        ID3D11Texture2D_Release(s_back);
        s_back = NULL;
    }
    hr = IDXGISwapChain_ResizeBuffers(s_swap, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr))
        fprintf(stderr, LOGP "ResizeBuffers %ux%u hr=0x%08lX\n", cw, ch, (unsigned long)hr);
    IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&s_back);
    if (s_back) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(s_back, &td);
        s_back_w = td.Width;
        s_back_h = td.Height;
    }
}

/* WM_SIZE recorded a client size the swap chain does not have: resize it. */
static void present_resize(void)
{
    UINT cw = s_client_w, ch = s_client_h;

    /* present.surface=internal: the swap chain stays the frame's size; the
     * compositor scales it to whatever the window became. */
    if (nv2a_host_opts()->present_surface == NV2A_PRESENT_SURFACE_INTERNAL)
        return;
    if (!cw || !ch || (cw == s_back_w && ch == s_back_h))
        return;
    swap_resize(cw, ch);
    fprintf(stderr, LOGP "window resized: swap chain %ux%u\n", s_back_w, s_back_h);
}

/* present.surface=internal after a render.scale change (the in-game menu):
 * the swap chain and the client area follow the frame's host size, so the
 * compositor keeps getting the whole frame rather than a scaling blit into
 * the starting size (Metal's drawable follows the frame the same way). A
 * frame cropped from a grown target (rt_grow) is not the frame's size, so it
 * leaves both alone. */
static void internal_resize(const RenderTarget *rt, UINT cw, UINT ch)
{
    RECT r;

    if (!s_back || (rt->hw == s_back_w && rt->hh == s_back_h))
        return;
    if (cw != rt->w || ch != rt->h || !rt->hw || !rt->hh)
        return;
    r.left = 0;
    r.top = 0;
    r.right = (LONG)rt->hw;
    r.bottom = (LONG)rt->hh;
    if (!nv2a_host_opts()->fullscreen)
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(s_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    swap_resize(rt->hw, rt->hh);
    fprintf(stderr, LOGP "render scale change: back buffer %ux%u (surface internal)\n",
            s_back_w, s_back_h);
}

/* The part of rt a flip shows, in guest pixels: the walker's present extent
 * when it names this surface. A target grown for a larger clip (rt_grow) can
 * be bigger than the frame; the rest of it, and its black grow margin, is not
 * the frame. */
static void present_extent(RenderTarget *rt, UINT *w, UINT *h)
{
    const struct nv2a_pb_present *p = nv2a_pb_present_state();
    /* The walker publishes the display mode's frame, which a title that only
     * ever drew this buffer at a smaller clip has no target that big for:
     * grow it (as a larger clip would) rather than show less than the CPU
     * path, which reads the whole frame from guest memory. A failed grow
     * shows what there is. */
    if (p->offset && p->offset == rt->offset && p->pitch == rt->pitch && p->w && p->h
            && (p->w > rt->w || p->h > rt->h))
        rt_grow(rt, rt->w > p->w ? rt->w : p->w, rt->h > p->h ? rt->h : p->h);
    *w = rt->w;
    *h = rt->h;
    if (p->offset && p->offset == rt->offset && p->pitch == rt->pitch && p->w && p->h) {
        if (p->w < *w) *w = p->w;
        if (p->h < *h) *h = p->h;
    }
}

/* rt's present extent into the back buffer at nv2a_present_rect's place,
 * filtered, less display.overscan's edges (nv2a_present_crop). Under
 * present.surface=internal the back buffer is the frame: the crop fills it,
 * point-sampled for integer as Metal's internal present is. */
#ifdef XBOXRECOMP_ENHANCE
static struct nv2a_rect s_ovl_place;    /* the overlay's: the frame in the back buffer */
#endif
static void present_blit(RenderTarget *rt, int filter, unsigned overscan, int internal)
{
    static const float black[4] = {0, 0, 0, 1};
    ID3D11ShaderResourceView *none = NULL;
    struct nv2a_rect r;
    D3D11_VIEWPORT vp;
    UINT cw, ch, hcw, hch;
    struct nv2a_crop c;
    int whole;

    if (!s_back || !blit_init())
        return;
    if (!s_back_rtv && FAILED(ID3D11Device_CreateRenderTargetView(
                s_dev, (ID3D11Resource *)s_back, NULL, &s_back_rtv)))
        return;
    if (!rt->srv && FAILED(ID3D11Device_CreateShaderResourceView(
                s_dev, (ID3D11Resource *)rt->tex, NULL, &rt->srv)))
        return;
    present_extent(rt, &cw, &ch);
    /* hor+: the pads go to the screen with the frame (Metal's present) */
    hcw = (cw + 2u * rt->pad) * rt->n;
    hch = ch * rt->n;
    nv2a_present_crop(hcw, hch, rt->hw, rt->hh, rt->n, rt->pad, overscan, filter, &c);
    whole = nv2a_present_rect(c.fit_w, c.fit_h, s_back_w, s_back_h, filter, &r);
    if (overscan && internal) {
        r.x = r.y = 0;
        r.w = (int32_t)s_back_w;
        r.h = (int32_t)s_back_h;
        whole = 1;
    }
    {
        D3D11_MAPPED_SUBRESOURCE m;
        float uv[4] = {c.u_scale, c.v_scale, c.u_off, c.v_off};
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_blit_cb, 0,
                                           D3D11_MAP_WRITE_DISCARD, 0, &m)))
            return;
        memcpy(m.pData, uv, sizeof uv);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_blit_cb, 0);
    }
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_back_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_back_rtv, black);
    vp.TopLeftX = (float)r.x;
    vp.TopLeftY = (float)r.y;
    vp.Width = (float)r.w;
    vp.Height = (float)r.h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs[0][0]);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_ds_off, 0);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_blit_cb);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_blit_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &rt->srv);
    /* Point for nearest and whole multiples; linear for linear and for an
     * integer frame that had to shrink to fit. */
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1,
        &s_blit_smp[filter == NV2A_PRESENT_LINEAR
                    || (filter == NV2A_PRESENT_INTEGER && !whole)]);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    /* Every binding above went behind the draw path's state shadow. */
    s_sc_init = 0;
#ifdef XBOXRECOMP_ENHANCE
    s_ovl_place = r;
#endif
}

#ifdef XBOXRECOMP_ENHANCE
/* ── present.shader (nv2a_present_chain.h) ──────────────────────────────
 *
 * librashader's D3D11 filter chain draws the frame into a texture the size
 * of the scaling blit's rectangle, which is then copied into that rectangle
 * of the cleared back buffer, in place of the blit's triangle. Its input is
 * the frame at render.scale's host size, pads included: the whole target
 * when the frame fills it, else a copy of the frame's part (a grown target
 * holds more); for a CRT preset at render.scale > 1, the frame resolved to
 * the guest's lines (nv2a_present_chain_source). Built on this thread when
 * the request's generation moves; "none" never loads the library. */
static libra_d3d11_filter_chain_t s_lrs_chain;
static unsigned s_lrs_gen;
static size_t s_lrs_frames;
static int s_lrs_lines;             /* the chain's preset draws per source line */
static int s_lrs_said;              /* the chain's first frame said how it was fed */
static ID3D11Texture2D *s_lrs_src;
static ID3D11ShaderResourceView *s_lrs_srv;
static ID3D11RenderTargetView *s_lrs_src_rtv;   /* when s_lrs_src is a resolve */
/* The chain's output, the rectangle's size: librashader gives the last pass
 * the whole render target's size as OutputSize and FinalViewportSize, not
 * the viewport's, so drawn straight into a letterboxed rectangle of the back
 * buffer the mask (crt-lottes: vTexCoord / OutputSize.zw) was laid out for
 * the whole back buffer and squeezed into the rectangle, and a
 * viewport-scaled pass came out at the back buffer's size. */
static ID3D11Texture2D *s_lrs_out;
static ID3D11RenderTargetView *s_lrs_out_rtv;
static ID3D11PixelShader *s_lrs_res_ps;
static ID3D11Buffer *s_lrs_res_cb;
static int s_lrs_res_failed;
static PFN_libra_d3d11_filter_chain_frame s_lrs_ff;   /* with the chain */
static PFN_libra_d3d11_filter_chain_set_param s_lrs_sp;
static unsigned s_lrs_pgen;         /* the parameter generation applied */
static int s_lrs_ov_said;

/* The chain, its input copy and its output: after a switch to none or a
 * failure the copy (host size, tens of MB at scale 4) is not kept for the
 * session. */
static void lrs_free(void)
{
    const struct nv2a_lrs *l;
    PFN_libra_d3d11_filter_chain_free fr;
    if (s_lrs_srv)
        ID3D11ShaderResourceView_Release(s_lrs_srv);
    if (s_lrs_src_rtv)
        ID3D11RenderTargetView_Release(s_lrs_src_rtv);
    if (s_lrs_src)
        ID3D11Texture2D_Release(s_lrs_src);
    if (s_lrs_out_rtv)
        ID3D11RenderTargetView_Release(s_lrs_out_rtv);
    if (s_lrs_out)
        ID3D11Texture2D_Release(s_lrs_out);
    s_lrs_srv = NULL;
    s_lrs_src_rtv = NULL;
    s_lrs_src = NULL;
    s_lrs_out_rtv = NULL;
    s_lrs_out = NULL;
    s_lrs_ff = NULL;
    s_lrs_sp = NULL;
    if (!s_lrs_chain)
        return;
    nv2a_present_chain_params_clear();
    l = nv2a_present_chain_lib();
    fr = l ? (PFN_libra_d3d11_filter_chain_free)l->sym("libra_d3d11_filter_chain_free") : NULL;
    if (fr)
        fr(&s_lrs_chain);
    s_lrs_chain = NULL;
}

/* The chain for the current request, or NULL (none, a missing library, a
 * preset that failed: said once, then not retried until the next request). */
static libra_d3d11_filter_chain_t lrs_chain(void)
{
    unsigned gen = nv2a_present_chain_gen();
    const struct nv2a_lrs *l;
    PFN_libra_preset_create pc;
    PFN_libra_preset_free pf;
    PFN_libra_d3d11_filter_chain_create cc;
    libra_shader_preset_t preset = NULL;
    libra_error_t e;
    filter_chain_d3d11_opt_t co;
    char path[1024], msg[512];
    int found;

    if (gen == s_lrs_gen)
        return s_lrs_chain;
    lrs_free();
    s_lrs_gen = gen;
    s_lrs_frames = 0;
    if (!nv2a_present_chain_wanted())
        return NULL;
    found = nv2a_present_chain_preset_lines(path, sizeof path, NULL, &s_lrs_lines);
    s_lrs_said = 0;
    l = found > 0 ? nv2a_present_chain_lib() : NULL;
    if (!l) {
        nv2a_present_chain_failed(gen);
        return NULL;
    }
    pc = (PFN_libra_preset_create)l->sym("libra_preset_create");
    pf = (PFN_libra_preset_free)l->sym("libra_preset_free");
    cc = (PFN_libra_d3d11_filter_chain_create)l->sym("libra_d3d11_filter_chain_create");
    s_lrs_ff = (PFN_libra_d3d11_filter_chain_frame)l->sym("libra_d3d11_filter_chain_frame");
    s_lrs_sp = (PFN_libra_d3d11_filter_chain_set_param)l->sym("libra_d3d11_filter_chain_set_param");
    if (!pc || !pf || !cc || !s_lrs_ff) {
        fprintf(stderr, "[SHADER] present.shader off: the library has no D3D11 runtime\n");
        nv2a_present_chain_failed(gen);
        return NULL;
    }
    memset(&co, 0, sizeof co);
    co.version = LIBRASHADER_CURRENT_VERSION;
    e = pc(path, &preset);
    /* The parameters before the chain: creating it consumes the preset. */
    if (!e)
        nv2a_present_chain_params_publish(&preset, gen);
    if (!e)
        e = cc(&preset, s_dev, &co, &s_lrs_chain);
    if (preset)
        pf(&preset);
    s_sc_init = 0;
    if (e) {
        nv2a_present_chain_error(e, msg, sizeof msg);
        fprintf(stderr, "[SHADER] present.shader off: %s: %s\n", path, msg);
        s_lrs_chain = NULL;
        nv2a_present_chain_params_clear();
        nv2a_present_chain_failed(gen);
        return NULL;
    }
    /* A new chain starts at the preset's values: apply every override. */
    s_lrs_pgen = nv2a_present_chain_params_gen() - 1;
    fprintf(stderr, "[SHADER] present.shader: %s (D3D11)\n", path);
    return s_lrs_chain;
}

/* nv2a_present_chain_params_apply's setter. librashader refuses a name the
 * chain lacks (UnknownShaderParameter), but the names come from the table
 * read off this chain's own preset, so none is unknown; an error here would
 * repeat on every change, so it is dropped unlogged. */
static void lrs_set_param(void *ctx, const char *name, float v)
{
    libra_error_t e;
    char msg[256];
    if (!s_lrs_sp)
        return;
    e = s_lrs_sp((libra_d3d11_filter_chain_t *)ctx, name, v);
    if (e)
        nv2a_present_chain_error(e, msg, sizeof msg);
}

/* While a chain is wanted under present.surface=internal the surface is
 * the output, not the frame. The chain draws its mask and scanlines per
 * output pixel; drawn into a frame-sized surface and scaled after by the
 * compositor (gamescope on the Deck: 640x480 to 1280x800, bilinear), they
 * are resampled into colour fringes and moire. Under gamescope or
 * present.fullscreen a borderless window covers the monitor and the swap
 * chain takes its size; otherwise the swap chain follows the client area as
 * under window. With the chain off the window style comes back and
 * internal_resize returns the frame's size. present.surface=window already
 * draws at the client size (present_resize). Returns 1 while the surface is
 * the output. */
static int chain_surface(int on, int internal)
{
    static int s_popup;
    const struct nv2a_host_opts *eo = nv2a_host_opts();
    int over = recomp_env_on(RENV_HOST_GAMESCOPE) || eo->fullscreen;
    UINT w = s_client_w, h = s_client_h;

    if (!on || !internal) {
        if (s_popup && !eo->fullscreen) {
            SetWindowLongPtrW(s_hwnd, GWL_STYLE, (LONG_PTR)(WS_OVERLAPPEDWINDOW | WS_VISIBLE));
            SetWindowPos(s_hwnd, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        s_popup = 0;
        return 0;
    }
    if (over) {
        MONITORINFO mi;
        RECT r;
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            r = mi.rcMonitor;
        } else {
            r.left = r.top = 0;
            r.right = GetSystemMetrics(SM_CXSCREEN);
            r.bottom = GetSystemMetrics(SM_CYSCREEN);
        }
        w = (UINT)(r.right - r.left);
        h = (UINT)(r.bottom - r.top);
        if (!s_popup && w && h) {
            SetWindowLongPtrW(s_hwnd, GWL_STYLE, (LONG_PTR)(WS_POPUP | WS_VISIBLE));
            SetWindowPos(s_hwnd, HWND_TOP, r.left, r.top, (int)w, (int)h,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            s_popup = 1;
        }
    }
    if (w && h && (w != s_back_w || h != s_back_h)) {
        RECT cr = {0, 0, 0, 0};
        swap_resize(w, h);
        /* The client area beside it: a window the compositor sees at
         * another size than the swap chain is scaled by it. */
        GetClientRect(s_hwnd, &cr);
        fprintf(stderr, "[SHADER] present.shader: back buffer %ux%u (the output's size),"
                " client %ldx%ld\n", s_back_w, s_back_h, (long)(cr.right - cr.left),
                (long)(cr.bottom - cr.top));
    }
    return 1;
}

/* The resolve to the guest's lines: each output row the mean of its n host
 * rows, at the whole host width (the extra sharpness stays across). */
static const char s_lrs_res_src[] =
    "Texture2D t0 : register(t0);\n"
    "cbuffer C : register(b0) { int4 k; };\n"   /* k.x: host rows per guest line */
    "float4 main(float4 p : SV_Position, float2 t : TEXCOORD0) : SV_Target {\n"
    "    int2 q = int2(p.xy);\n"
    "    float3 c = 0;\n"
    "    for (int i = 0; i < k.x; i++)\n"
    "        c += t0.Load(int3(q.x, q.y * k.x + i, 0)).rgb;\n"
    "    return float4(c / k.x, 1);\n"
    "}\n";

static int lrs_res_init(void)
{
    ID3DBlob *code = NULL;

    if (s_lrs_res_ps || s_lrs_res_failed)
        return s_lrs_res_ps != NULL;
    s_lrs_res_failed = 1;
    if (!blit_init() || !compile("nv2a_present_lines_ps", s_lrs_res_src, "ps_5_0", &code))
        return 0;
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                   ID3D10Blob_GetBufferSize(code), NULL, &s_lrs_res_ps);
    ID3D10Blob_Release(code);
    s_lrs_res_cb = make_buffer(16, D3D11_BIND_CONSTANT_BUFFER);
    if (!s_lrs_res_ps || !s_lrs_res_cb) {
        fprintf(stderr, "[SHADER] the guest-line resolve failed; the chain reads the host rows\n");
        return 0;
    }
    s_lrs_res_failed = 0;
    return 1;
}

/* s_lrs_src at w x h of format f, made again when any of them changed.
 * rtv: bindable as a render target (the resolve draws into it). */
static int lrs_src_make(UINT w, UINT h, DXGI_FORMAT f, int rtv)
{
    D3D11_TEXTURE2D_DESC td;

    if (s_lrs_src) {
        ID3D11Texture2D_GetDesc(s_lrs_src, &td);
        if (td.Width == w && td.Height == h && td.Format == f && !rtv == !s_lrs_src_rtv)
            return 1;
        ID3D11ShaderResourceView_Release(s_lrs_srv);
        if (s_lrs_src_rtv)
            ID3D11RenderTargetView_Release(s_lrs_src_rtv);
        ID3D11Texture2D_Release(s_lrs_src);
        s_lrs_srv = NULL;
        s_lrs_src_rtv = NULL;
        s_lrs_src = NULL;
    }
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = f;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rtv ? D3D11_BIND_RENDER_TARGET : 0);
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_lrs_src)))
        return 0;
    if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_lrs_src,
                                                     NULL, &s_lrs_srv))
        || (rtv && FAILED(ID3D11Device_CreateRenderTargetView(
                s_dev, (ID3D11Resource *)s_lrs_src, NULL, &s_lrs_src_rtv)))) {
        if (s_lrs_srv)
            ID3D11ShaderResourceView_Release(s_lrs_srv);
        ID3D11Texture2D_Release(s_lrs_src);
        s_lrs_srv = NULL;
        s_lrs_src_rtv = NULL;
        s_lrs_src = NULL;
        return 0;
    }
    return 1;
}

/* The chain's input: rt's w x h corner at host size, or with sh < h that
 * corner resolved to sh rows (sh = h / n, nv2a_present_chain_source).
 * *resolved: 1 when it was, 0 when it is the host rows (sh == h, or the
 * resolve shader failed to build). */
static ID3D11ShaderResourceView *lrs_source(RenderTarget *rt, UINT w, UINT h, UINT sh,
                                            int *resolved)
{
    static ID3D11ShaderResourceView *const none = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_BOX b;

    ID3D11Texture2D_GetDesc(rt->tex, &td);
    *resolved = 0;
    if (sh < h && lrs_res_init()) {
        D3D11_MAPPED_SUBRESOURCE m;
        D3D11_VIEWPORT vp;
        int k[4] = { (int)(h / sh), 0, 0, 0 };
        float uv[4] = { 1, 1, 0, 0 };

        if (!lrs_src_make(w, sh, td.Format, 1))
            return NULL;
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_lrs_res_cb, 0,
                                           D3D11_MAP_WRITE_DISCARD, 0, &m)))
            return NULL;
        memcpy(m.pData, k, sizeof k);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_lrs_res_cb, 0);
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_blit_cb, 0,
                                           D3D11_MAP_WRITE_DISCARD, 0, &m)))
            return NULL;
        memcpy(m.pData, uv, sizeof uv);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_blit_cb, 0);
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_lrs_src_rtv, NULL);
        vp.TopLeftX = vp.TopLeftY = 0.0f;
        vp.Width = (float)w;
        vp.Height = (float)sh;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        ID3D11DeviceContext_RSSetState(s_ctx, s_rs[0][0]);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFF);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_ds_off, 0);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_blit_cb);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_lrs_res_ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_lrs_res_cb);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &rt->srv);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        /* Every binding above went behind the draw path's state shadow. */
        s_sc_init = 0;
        *resolved = 1;
        return s_lrs_srv;
    }
    if (td.Width == w && td.Height == h)
        return rt->srv;
    if (!lrs_src_make(w, h, td.Format, 0))
        return NULL;
    b.left = b.top = b.front = 0;
    b.right = w;
    b.bottom = h;
    b.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_lrs_src, 0, 0, 0, 0,
                                              (ID3D11Resource *)rt->tex, 0, &b);
    return s_lrs_srv;
}

/* s_lrs_out at w x h, the back buffer's format. */
static int lrs_out_make(UINT w, UINT h)
{
    D3D11_TEXTURE2D_DESC td;

    if (s_lrs_out) {
        ID3D11Texture2D_GetDesc(s_lrs_out, &td);
        if (td.Width == w && td.Height == h)
            return 1;
        ID3D11RenderTargetView_Release(s_lrs_out_rtv);
        ID3D11Texture2D_Release(s_lrs_out);
        s_lrs_out_rtv = NULL;
        s_lrs_out = NULL;
    }
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_lrs_out)))
        return 0;
    if (FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_lrs_out, NULL,
                                                   &s_lrs_out_rtv))) {
        ID3D11Texture2D_Release(s_lrs_out);
        s_lrs_out = NULL;
        return 0;
    }
    return 1;
}

/* rt through the chain into the back buffer at the stock present's place
 * (nv2a_present_chain_rect; the filter only sizes the rectangle: the chain
 * samples). internal: the back buffer is the frame's size (no chain
 * surface), so the chain fills it; surface_internal: the config asked for
 * present.surface=internal. 0 when it did not draw, and the caller presents
 * as without a shader. */
static int present_chain(RenderTarget *rt, int filter, int internal, int surface_internal)
{
    static const float black[4] = {0, 0, 0, 1};
    libra_d3d11_filter_chain_t chain;
    ID3D11ShaderResourceView *src;
    frame_d3d11_opt_t fo;
    libra_viewport_t lv;
    libra_error_t e;
    struct nv2a_rect r;
    UINT cw, ch, hcw, hch, sw, sh;
    int resolved;
    char msg[512];

    chain = lrs_chain();
    if (!chain || !s_back)
        return 0;
    nv2a_present_chain_params_apply(&s_lrs_pgen, lrs_set_param, &s_lrs_chain);
    if (!s_back_rtv && FAILED(ID3D11Device_CreateRenderTargetView(
                s_dev, (ID3D11Resource *)s_back, NULL, &s_back_rtv)))
        return 0;
    if (!rt->srv && FAILED(ID3D11Device_CreateShaderResourceView(
                s_dev, (ID3D11Resource *)rt->tex, NULL, &rt->srv)))
        return 0;
    present_extent(rt, &cw, &ch);
    hcw = (cw + 2u * rt->pad) * rt->n;
    hch = ch * rt->n;
    if (hcw > rt->hw)
        hcw = rt->hw;
    if (hch > rt->hh)
        hch = rt->hh;
    nv2a_present_chain_source(hcw, hch, rt->n, s_lrs_lines, &sw, &sh);
    src = lrs_source(rt, hcw, hch, sh, &resolved);
    if (!src)
        return 0;
    /* Said at the first frame, once the source is known: a height that is
     * no multiple of the scale, or a resolve that did not build, feeds the
     * host rows even to a CRT preset. */
    if (s_lrs_lines && !s_lrs_said) {
        s_lrs_said = 1;
        if (resolved || rt->n <= 1)
            fprintf(stderr, "[SHADER] present.shader: fed the guest's lines (%u of %u rows)\n",
                    resolved ? sh : hch, hch);
        else
            fprintf(stderr, "[SHADER] present.shader: fed the host's %u rows (%s)\n", hch,
                    sh < hch ? "the guest-line resolve failed" : "not a multiple of render.scale");
    }
    /* The rectangle keeps the host frame's shape: a resolved source is
     * hcw wide and only ch high. */
    nv2a_present_chain_rect(hcw, hch, s_back_w, s_back_h, filter, surface_internal, &r);
    if (internal) {
        r.x = r.y = 0;
        r.w = (int32_t)s_back_w;
        r.h = (int32_t)s_back_h;
    }
    if (r.w <= 0 || r.h <= 0 || !lrs_out_make((UINT)r.w, (UINT)r.h))
        return 0;
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_back_rtv, black);
    lv.x = 0.0f;
    lv.y = 0.0f;
    lv.width = (uint32_t)r.w;
    lv.height = (uint32_t)r.h;
    memset(&fo, 0, sizeof fo);
    fo.version = LIBRASHADER_CURRENT_VERSION;
    fo.total_subframes = 1;
    fo.current_subframe = 1;
    fo.frames_per_second = nv2a_present_chain_rate((uint64_t)(sh_now_ms() * 1e6));
    e = s_lrs_ff(&s_lrs_chain, s_ctx, s_lrs_frames++, src, s_lrs_out_rtv, &lv, NULL, &fo);
    /* Every binding the chain made went behind the draw path's state shadow. */
    s_sc_init = 0;
    if (e) {
        nv2a_present_chain_error(e, msg, sizeof msg);
        fprintf(stderr, "[SHADER] present.shader off: frame: %s\n", msg);
        nv2a_present_chain_failed(s_lrs_gen);
        lrs_free();
        return 0;
    }
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_back, 0, (UINT)r.x,
                                              (UINT)r.y, 0, (ID3D11Resource *)s_lrs_out, 0,
                                              NULL);
    s_ovl_place = r;
    return 1;
}
#endif /* XBOXRECOMP_ENHANCE */

#ifdef XBOXRECOMP_ENHANCE
/* ── The overlay (nv2a_overlay.h) ─────────────────────────────────────────
 *
 * This flip is the overlay's presenting thread here: it polls (the strip
 * and toasts compose in the poll), then draws the visible lists over the
 * frame's rectangle after dump_frame, so d3d11_dump never has them, and
 * before Present. A menu edit therefore shows at the next guest flip, not
 * within 100 ms as on SDL and Metal (F-9). */
static ID3D11VertexShader *s_ovl_vs;
static ID3D11PixelShader  *s_ovl_ps;
static ID3D11InputLayout  *s_ovl_il;
static ID3D11BlendState   *s_ovl_bs;
static ID3D11Buffer       *s_ovl_cb, *s_ovl_vb, *s_ovl_ib;
static UINT                s_ovl_vb_cap, s_ovl_ib_cap;
static ID3D11ShaderResourceView *s_ovl_srv[1 + NV2A_OVL_IMAGES];
static uint64_t            s_ovl_srv_gen[1 + NV2A_OVL_IMAGES];
static int                 s_ovl_failed;

static const char s_ovl_vs_src[] =
    "cbuffer C : register(b0) { float4 xf; };\n"
    "struct VI { float2 p : POSITION; float2 t : TEXCOORD0; float4 c : COLOR0; };\n"
    "struct VO { float4 p : SV_Position; float2 t : TEXCOORD0; float4 c : COLOR0; };\n"
    "VO main(VI i) {\n"
    "    VO o;\n"
    "    o.p = float4(i.p * xf.xy + xf.zw, 0, 1);\n"
    "    o.t = i.t;\n"
    "    o.c = i.c;\n"
    "    return o;\n"
    "}\n";
static const char s_ovl_ps_src[] =
    "Texture2D t : register(t0);\n"
    "SamplerState s : register(s0);\n"
    "struct VO { float4 p : SV_Position; float2 t : TEXCOORD0; float4 c : COLOR0; };\n"
    "float4 main(VO i) : SV_Target { return t.Sample(s, i.t) * i.c; }\n";

static int ovl_init(void)
{
    static const D3D11_INPUT_ELEMENT_DESC il[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3DBlob *code = NULL;
    D3D11_BLEND_DESC bd;

    if (s_ovl_ps || s_ovl_failed)
        return s_ovl_ps != NULL;
    s_ovl_failed = 1;
    if (!blit_init() || !compile("nv2a_overlay_vs", s_ovl_vs_src, "vs_5_0", &code))
        return 0;
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                    ID3D10Blob_GetBufferSize(code), NULL, &s_ovl_vs);
    ID3D11Device_CreateInputLayout(s_dev, il, 3, ID3D10Blob_GetBufferPointer(code),
                                   ID3D10Blob_GetBufferSize(code), &s_ovl_il);
    ID3D10Blob_Release(code);
    if (!s_ovl_vs || !s_ovl_il || !compile("nv2a_overlay_ps", s_ovl_ps_src, "ps_5_0", &code))
        return 0;
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                   ID3D10Blob_GetBufferSize(code), NULL, &s_ovl_ps);
    ID3D10Blob_Release(code);
    /* Premultiplied alpha. */
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(s_dev, &bd, &s_ovl_bs);
    s_ovl_cb = make_buffer(16, D3D11_BIND_CONSTANT_BUFFER);
    if (!s_ovl_ps || !s_ovl_bs || !s_ovl_cb) {
        fprintf(stderr, LOGP "overlay pipeline failed; no overlay drawn\n");
        return 0;
    }
    s_ovl_failed = 0;
    return 1;
}

/* Texture k of the overlay (0 the atlas, 1.. the image slots), re-made when
 * its gen moves. */
static ID3D11ShaderResourceView *ovl_srv(uint16_t k)
{
    const uint32_t *px;
    uint32_t w, h;
    uint64_t gen;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Texture2D *t = NULL;

    if (k == 0) {
        const struct nv2a_ovl_atlas *a = nv2a_overlay_atlas();
        if (!a)
            return NULL;
        px = a->px, w = a->w, h = a->h, gen = a->gen;
    } else {
        const struct nv2a_ovl_image *im = k <= NV2A_OVL_IMAGES ? nv2a_overlay_image(k) : NULL;
        if (!im || !im->px)
            return NULL;
        px = im->px, w = im->w, h = im->h, gen = im->gen;
    }
    if (s_ovl_srv[k] && s_ovl_srv_gen[k] == gen)
        return s_ovl_srv[k];
    if (s_ovl_srv[k])
        ID3D11ShaderResourceView_Release(s_ovl_srv[k]);
    s_ovl_srv[k] = NULL;
    s_ovl_srv_gen[k] = gen;
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = px;
    sd.SysMemPitch = w * 4;
    sd.SysMemSlicePitch = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &t))) {
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t, NULL, &s_ovl_srv[k]);
        ID3D11Texture2D_Release(t);
    }
    return s_ovl_srv[k];
}

static int ovl_fill(ID3D11Buffer **b, UINT *cap, UINT bind, const void *src, UINT n)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (n > *cap) {
        UINT c = *cap ? *cap : 4096;
        while (c < n)
            c *= 2;
        if (*b)
            ID3D11Buffer_Release(*b);
        *b = make_buffer(c, bind);
        *cap = *b ? c : 0;
        if (!*b)
            return 0;
    }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)*b, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return 0;
    memcpy(m.pData, src, n);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)*b, 0);
    return 1;
}

/* After dump_frame, before Present. On the stock copy path this is the
 * only draw into the back buffer: it binds its own target and viewport
 * and leaves the draw path's state shadow stale, as present_blit does. */
static void ovl_present(void)
{
    const struct nv2a_ovl_list *ls[NV2A_OVL_COUNT];
    struct nv2a_rect r = s_ovl_place;
    ID3D11ShaderResourceView *none = NULL;
    D3D11_VIEWPORT vp;
    UINT stride = sizeof(struct nv2a_ovl_vtx), off = 0, cb_cap = 16;
    int n, i;
    uint32_t c;

    if (!s_back || !nv2a_overlay_armed())
        return;
    int copied = r.w > 0 && r.h > 0;

    nv2a_overlay_set_presenter("D3D11");
    if (!copied) {
        r.x = r.y = 0;
        r.w = (int32_t)s_back_w;
        r.h = (int32_t)s_back_h;
    }
    nv2a_overlay_set_placement(r.x, r.y, r.w, r.h);
    /* A back buffer of another size than the client area is stretched into
     * it by DXGI (the legacy description's scaling): under
     * present.surface=internal whenever the window is resized, and for the
     * one flip between a WM_SIZE and present_resize otherwise. The pointer's
     * client pixels map through that stretch (Metal's fit is the same
     * call); the same sizes publish none. */
    {
        UINT vw = s_client_w, vh = s_client_h;
        if (vw && vh && (vw != s_back_w || vh != s_back_h))
            nv2a_overlay_set_view(0, 0, (int)vw, (int)vh, (int)s_back_w, (int)s_back_h);
        else
            nv2a_overlay_set_view(0, 0, 0, 0, 0, 0);
    }
    nv2a_overlay_poll(GetTickCount64());
    if (!nv2a_overlay_visible() || !(n = nv2a_overlay_lists(ls)) || !ovl_init())
        return;
    if (!s_back_rtv && FAILED(ID3D11Device_CreateRenderTargetView(
                s_dev, (ID3D11Resource *)s_back, NULL, &s_back_rtv)))
        return;
    /* Nothing copied this present: a flip-model back buffer holds whatever
     * an older frame left, so the strip would sit on garbage (review F9). */
    if (!copied) {
        static const float black[4] = {0, 0, 0, 1};
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_back_rtv, black);
    }
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_back_rtv, NULL);
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float)s_back_w;
    vp.Height = (float)s_back_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs_sc[0][0]);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s_ovl_bs, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_ds_off, 0);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_ovl_il);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_ovl_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_ovl_cb);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ovl_ps, NULL, 0);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_blit_smp[1]);
    for (i = 0; i < n; i++) {
        const struct nv2a_ovl_list *l = ls[i];
        float sx = l->out_w ? (float)r.w / (float)l->out_w : 1.0f;
        float sy = l->out_h ? (float)r.h / (float)l->out_h : 1.0f;
        float xf[4] = { sx * 2.0f / (float)s_back_w, -sy * 2.0f / (float)s_back_h,
                        (float)r.x * 2.0f / (float)s_back_w - 1.0f,
                        1.0f - (float)r.y * 2.0f / (float)s_back_h };
        if (!ovl_fill(&s_ovl_vb, &s_ovl_vb_cap, D3D11_BIND_VERTEX_BUFFER, l->vtx,
                      l->nvtx * (UINT)sizeof *l->vtx)
            || !ovl_fill(&s_ovl_ib, &s_ovl_ib_cap, D3D11_BIND_INDEX_BUFFER, l->idx,
                         l->nidx * (UINT)sizeof *l->idx)
            || !ovl_fill(&s_ovl_cb, &cb_cap, D3D11_BIND_CONSTANT_BUFFER, xf, sizeof xf))
            continue;
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_ovl_vb, &stride, &off);
        ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ovl_ib, DXGI_FORMAT_R16_UINT, 0);
        for (c = 0; c < l->ncmd; c++) {
            const struct nv2a_ovl_cmd *k = &l->cmd[c];
            ID3D11ShaderResourceView *t = ovl_srv(k->tex);
            D3D11_RECT sc;
            if (!t || !k->count)
                continue;
            sc.left = r.x + (LONG)(k->clip[0] * sx);
            sc.top = r.y + (LONG)(k->clip[1] * sy);
            sc.right = r.x + (LONG)(k->clip[0] * sx + k->clip[2] * sx + 0.999f);
            sc.bottom = r.y + (LONG)(k->clip[1] * sy + k->clip[3] * sy + 0.999f);
            ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
            ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &t);
            ID3D11DeviceContext_DrawIndexed(s_ctx, k->count, k->first, 0);
        }
    }
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    s_sc_init = 0;
}
#endif /* XBOXRECOMP_ENHANCE */
/* nv2a_pb_d3d11_back_pixel: a back-buffer pixel read at each flip before
 * Present, since FLIP_DISCARD leaves nothing to read after it. */
static int s_probe_x = -1, s_probe_y;
static uint32_t s_probe_px = 0xDEADBEEFu;

/* The [MEM] exit line's GPU figures: this process's video memory in the
 * adapter's local and non-local segments (DXGI 1.4; Windows 10 and DXVK).
 * An adapter without IDXGIAdapter3 leaves them n/a. Called every 60
 * presents on the render thread. The adapter is held per device: a new
 * s_dev drops it and asks again, so it never outlives the device. */
static void gpu_mem_note(void)
{
    static IDXGIAdapter3 *a3;
    static ID3D11Device *a3_dev;
    static int said;
    DXGI_QUERY_VIDEO_MEMORY_INFO loc, nl;

    if (a3_dev != s_dev) {
        IDXGIDevice *dd = NULL;
        IDXGIAdapter *ad = NULL;
        if (a3)
            IDXGIAdapter3_Release(a3);
        a3 = NULL;
        a3_dev = s_dev;
        if (s_dev && SUCCEEDED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice,
                                                           (void **)&dd)) && dd) {
            if (SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad)) && ad) {
                if (FAILED(IDXGIAdapter_QueryInterface(ad, &IID_IDXGIAdapter3,
                                                       (void **)&a3)))
                    a3 = NULL;
                IDXGIAdapter_Release(ad);
            }
            IDXGIDevice_Release(dd);
        }
        if (!a3 && s_dev && !said) {
            said = 1;
            fprintf(stderr, LOGP "no IDXGIAdapter3: the [MEM] line has no GPU figures\n");
        }
    }
    if (!a3)
        return;
    if (FAILED(IDXGIAdapter3_QueryVideoMemoryInfo(a3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &loc)))
        loc.CurrentUsage = MEM_STATS_NA;
    if (FAILED(IDXGIAdapter3_QueryVideoMemoryInfo(a3, 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,
                                                  &nl)))
        nl.CurrentUsage = MEM_STATS_NA;
    mem_stats_gpu_note("d3d11", loc.CurrentUsage, nl.CurrentUsage);
}

static void d3d11_flip(uint32_t surface_offset, uint32_t pitch)
{
    HRESULT hr;

    if (!init())
        return;
    pump();
    {
        wchar_t tb[192];
        if (xbox_FramebufferWindowTitleText(tb, 192, &s_title_clock))
            SetWindowTextW(s_hwnd, tb);
    }
    if (s_occ.ops)
        nv2a_occ_poll(&s_occ, 0);
    present_resize();
    s_drawn = present_target(surface_offset);
    /* The frame's target size, for a HUD canvas on "frame". */
    if (s_drawn)
        nv2a_aspect_frame_publish(s_drawn->w, s_drawn->h);
    s_dump_rt = 0;
#ifdef XBOXRECOMP_ENHANCE
    s_ovl_place.w = s_ovl_place.h = 0;     /* no frame: the whole back buffer */
#endif
    if (s_drawn) {
        static int said;
        const struct nv2a_host_opts *eo = nv2a_host_opts();
        /* Stock: the frame as the guest drew it, a copy into the unresized
         * windowed back buffer (whole or, for another size, its overlap).
         * Not for a hor+ target: its overlap is the left pad and 4:3 of the
         * rest, the right pad cut off; it goes whole or through the blit. */
        int stock = nv2a_host_render_scale() == 1
                 && eo->present_filter == NV2A_PRESENT_NEAREST
                 && !eo->fullscreen && !s_drawn->pad && !eo->overscan
                 && s_back_w == s_made_w && s_back_h == s_made_h;
        UINT cw, ch;
        int same;
        int internal = eo->present_surface == NV2A_PRESENT_SURFACE_INTERNAL;
        present_extent(s_drawn, &cw, &ch);
        s_shown_w = cw;
        s_shown_h = ch;
#ifdef XBOXRECOMP_ENHANCE
        /* A shader draws at the output: everything below is as under
         * present.surface=window. Gated on a built chain, as Metal: a
         * missing library or preset must not flash a monitor-sized popup
         * for the one flip its build takes to fail. */
        if (chain_surface(nv2a_present_chain_wanted() && lrs_chain(), internal))
            internal = 0;
#endif
        if (internal)
            internal_resize(s_drawn, cw, ch);
        same = cw == s_drawn->w && ch == s_drawn->h
            && s_drawn->hw == s_back_w && s_drawn->hh == s_back_h;
        /* Under present.surface=internal nothing is scaled here, so no
         * filter has anything to do: the frame is copied as it is, at
         * scale > 1 too (the compositor gets the scaled frame whole). */
        /* A display.overscan crop always blits: a copy shows the edges. */
        int blit = !stock && (eo->overscan
                || !(same && (eo->present_filter != NV2A_PRESENT_LINEAR || internal)));
#ifdef XBOXRECOMP_ENHANCE
        /* present.shader: the chain reads the whole frame, so a display.overscan
         * crop has no place in it. */
        s_chained = 0;
        if (s_back && (nv2a_present_chain_wanted() || s_lrs_chain)) {
            s_chained = present_chain(s_drawn, eo->present_filter, internal,
                                      eo->present_surface == NV2A_PRESENT_SURFACE_INTERNAL);
            if (s_chained && eo->overscan && !s_lrs_ov_said++)
                fprintf(stderr, "[SHADER] display.overscan=%u ignored while"
                        " present.shader runs\n", eo->overscan);
        }
        if (s_chained) {
            /* d3d11_dump keeps reading the frame (the goldens'); the back
             * buffer goes to present_NNNNN.bmp beside it. */
            s_dump_rt = 1;
        } else
#endif
        if (!s_back) {
            /* A failed ResizeBuffers/GetBuffer left no back buffer. */
            static int said_nb;
            if (!said_nb++)
                fprintf(stderr, LOGP "no back buffer (resize failed): frame not shown\n");
        } else if (blit) {
            present_blit(s_drawn, eo->present_filter, eo->overscan, internal);
            s_dump_rt = 1;
        } else if (same) {
            ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_back,
                                             (ID3D11Resource *)s_drawn->tex);
#ifdef XBOXRECOMP_ENHANCE
            s_ovl_place.x = s_ovl_place.y = 0;
            s_ovl_place.w = (int32_t)s_back_w;
            s_ovl_place.h = (int32_t)s_back_h;
#endif
        } else {
            static const float black[4] = {0, 0, 0, 1};
            D3D11_BOX b;
            b.left = 0;
            b.top = 0;
            b.front = 0;
            b.right = cw < s_back_w ? cw : s_back_w;
            b.bottom = ch < s_back_h ? ch : s_back_h;
            b.back = 1;
            /* FLIP_DISCARD hands back a buffer whose contents are undefined
             * (DXVK keeps an old frame), so the band the copy leaves -- a
             * frame cropped smaller than its grown target, or than the
             * window -- is black, as present_blit leaves it. */
            if ((b.right < s_back_w || b.bottom < s_back_h)
                    && (s_back_rtv || SUCCEEDED(ID3D11Device_CreateRenderTargetView(
                            s_dev, (ID3D11Resource *)s_back, NULL, &s_back_rtv))))
                ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_back_rtv, black);
            ID3D11DeviceContext_CopySubresourceRegion(
                s_ctx, (ID3D11Resource *)s_back, 0, 0, 0, 0,
                (ID3D11Resource *)s_drawn->tex, 0, &b);
#ifdef XBOXRECOMP_ENHANCE
            s_ovl_place.x = s_ovl_place.y = 0;
            s_ovl_place.w = (int32_t)b.right;
            s_ovl_place.h = (int32_t)b.bottom;
#endif
        }
        if (s_back && !said++)
            fprintf(stderr, LOGP "first present: back buffer %ux%u, frame %ux%u"
                    " (%ux%u host), %s%s\n", s_back_w, s_back_h, s_drawn->w, s_drawn->h,
                    s_drawn->hw, s_drawn->hh,
#ifdef XBOXRECOMP_ENHANCE
                    s_chained ? "present.shader" :
#endif
                    blit ? "scaling blit" : same ? "copy" : "overlap copy",
                    internal ? " (surface internal)" : "");
    }
    if (s_back && s_probe_x >= 0)
        s_probe_px = (UINT)s_probe_x < s_back_w && (UINT)s_probe_y < s_back_h
                   ? read_tex_pixel(s_back, (UINT)s_probe_x, (UINT)s_probe_y)
                   : 0xDEADBEEFu;
    s_presents++;
    dump_frame();
#ifdef XBOXRECOMP_ENHANCE
    ovl_present();
    nv2a_stats_add(NV2A_STAT_PRESENTS, 1);
#endif
    hr = IDXGISwapChain_Present(s_swap, s_sync_interval, 0);
    if (FAILED(hr) && s_presents < 4)
        fprintf(stderr, LOGP "Present hr=0x%08lX\n", (unsigned long)hr);
    /* FLIP_DISCARD unbinds the back buffer; the next frame rebinds its own
     * render target anyway. */
    /* The title may read this frame's visibility tests from here on: settle
     * what is still pending. After Present, which submitted the frame's
     * queries; a flushing GetData before it would be the first submission. */
    if (s_occ.ops)
        nv2a_occ_flip(&s_occ);
    flip_log(surface_offset);
    if (s_presents == 1 || s_presents % 60 == 0)
        gpu_mem_note();
    if (s_presents == 1 || (s_presents % 300) == 0)
        fprintf(stderr, LOGP "present #%llu: surface 0x%08X (walker said 0x%08X),"
                " %llu clears, %llu draws, %llu skipped, %llu culled, %llu texture uploads,"
                " %llu rehashes, %llu combiner shaders, %llu zclears,"
                " %llu depth-tested draws, %llu render-target textures,"
                " %llu stale drops, %llu evictions lost, %llu target grows,"
                " %llu partial clears\n",
                (unsigned long long)s_presents,
                s_drawn ? s_drawn->offset : 0, surface_offset,
                (unsigned long long)s_clears, (unsigned long long)s_draws,
                (unsigned long long)s_draws_skipped,
                (unsigned long long)s_draws_culled,
                (unsigned long long)s_tex_uploads,
                (unsigned long long)s_tex_rehash,
                (unsigned long long)s_rc_compiles,
                (unsigned long long)s_zclears, (unsigned long long)s_draws_z,
                (unsigned long long)s_rtt_binds, (unsigned long long)s_stale_drops,
                (unsigned long long)s_evict_lost, (unsigned long long)s_grows,
                (unsigned long long)s_partial_clears);
    if (s_presents == 1 || (s_presents % 300) == 0)
        fprintf(stderr, LOGP "shaders: %u compiled (%.1f ms), %u from the cache (%.1f ms),"
                " %u cache misses, %u damaged entries, %u failed stores\n",
                s_sh_compiled, s_sh_compile_ms, s_sh_cached, s_sh_cached_ms,
                s_shc.misses, s_shc.corrupt, s_shc.store_fails);
    if (s_presents == 1 || (s_presents % 300) == 0)
        fprintf(stderr, LOGP "blend: %llu draws with a constant factor,"
                " %u SET_BLEND_COLOR writes (last 0x%08X)\n",
                (unsigned long long)s_draws_bconst,
                nv2a_pb_gpu_state()->blend_color_writes,
                nv2a_pb_gpu_state()->blend_color);
}

/* render.scale changed at this flip: every target and depth buffer was made
 * at the old factor. Targets are released, their pixels lost as on any
 * eviction here (the title redraws its post chain every frame). After the
 * present, so the frame being shown keeps the targets it was drawn in.
 *
 * The texture cache stays, as on Metal. Its entries are guest texels at
 * guest size (or a pack's), which no scale changes; a render-to-texture
 * input binds the target itself (rt_texture, s_self resized by its own
 * check), never a cache entry; and tex_bind rehashes each entry once a flip,
 * so one under a released target's memory reads what a fresh decode would.
 * Dropping it made the next flip decode and upload every texture again:
 * 151-712 ms on Jet Set Radio Future under Proton, the freeze seen at each
 * change. */
static void scale_drop(void)
{
    int i;
    for (i = 0; i < RT_MAX; i++)
        if (s_rt[i].tex)
            rt_release(&s_rt[i]);
    for (i = 0; i < DT_MAX; i++) {
        DepthTarget *d = &s_dt[i];
        if (!d->tex)
            continue;
        ID3D11DepthStencilView_Release(d->dsv);
        ID3D11Texture2D_Release(d->tex);
        memset(d, 0, sizeof *d);
    }
    /* Both point into s_rt, now zeroed; the stage list is only rebuilt by
     * the next textured batch, and the white-draw log reads it. */
    s_cur = NULL;
    memset(s_stage_rt, 0, sizeof s_stage_rt);
    /* Queries issued at the old scale are read at it: a count taken at 2x
     * divided by 1 would report four times the samples. Retired results go
     * unread (nv2a_occ_set_scale). */
    if (s_occ.ops) {
        nv2a_occ_settle(&s_occ);
        nv2a_occ_set_scale(&s_occ, nv2a_host_render_scale());
    }
    fprintf(stderr, LOGP "render.scale=%u from this flip: targets dropped\n",
            nv2a_host_render_scale());
}

/* present.fullscreen changed live: swap the window's style and size on this
 * thread, which owns the window. SetWindowPos sends WM_SIZE here at once,
 * so the next flip's present_resize gives the swap chain the new client
 * size before it presents; no other thread touches the window, so there is
 * no resize to race. Borderless over the window's monitor, as at start; the
 * windowed placement is the one left, or the starting size when the run
 * began fullscreen. Under present.surface=internal the client size stays the
 * frame's (the compositor scales it), only the border changes. */
static void fullscreen_switch(int full)
{
    /* The placement, not GetWindowRect: a maximised window comes back
     * maximised over its restored rectangle, and a minimised one's rect
     * (off screen) is not where it goes back to. */
    static WINDOWPLACEMENT s_windowed;
    static int s_have_windowed;
    RECT r;
    int internal = nv2a_host_opts()->present_surface == NV2A_PRESENT_SURFACE_INTERNAL;

    if (!s_hwnd)
        return;
#ifdef XBOXRECOMP_ENHANCE
    /* With a chain the back buffer is the output's size (chain_surface),
     * not the frame's: the switch goes as under present.surface=window, or
     * leaving fullscreen would keep a monitor-sized window. */
    if (s_lrs_chain)
        internal = 0;
#endif
    if (full) {
        MONITORINFO mi;

        s_windowed.length = sizeof s_windowed;
        if (GetWindowPlacement(s_hwnd, &s_windowed)) {
            s_have_windowed = 1;
            if (s_windowed.showCmd == SW_SHOWMINIMIZED)
                s_windowed.showCmd = (s_windowed.flags & WPF_RESTORETOMAXIMIZED)
                                   ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        }
        mi.cbSize = sizeof mi;
        if (internal) {
            r.left = r.top = 0;
            r.right = (LONG)s_back_w;
            r.bottom = (LONG)s_back_h;
        } else if (GetMonitorInfoW(MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            r = mi.rcMonitor;
        } else {
            r.left = r.top = 0;
            r.right = GetSystemMetrics(SM_CXSCREEN);
            r.bottom = GetSystemMetrics(SM_CYSCREEN);
        }
        SetWindowLongPtrW(s_hwnd, GWL_STYLE, (LONG_PTR)(WS_POPUP | WS_VISIBLE));
    } else {
        if (internal || !s_have_windowed) {
            r.left = r.top = 0;
            r.right = internal ? (LONG)s_back_w : (LONG)nv2a_host_window_w(640, 480, 1);
            r.bottom = internal ? (LONG)s_back_h : 480;
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            OffsetRect(&r, 64 - r.left, 64 - r.top);
        } else {
            /* The style first, so the placement sizes a framed window. */
            SetWindowLongPtrW(s_hwnd, GWL_STYLE, (LONG_PTR)(WS_OVERLAPPEDWINDOW | WS_VISIBLE));
            SetWindowPlacement(s_hwnd, &s_windowed);
            SetWindowPos(s_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                         | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            r = s_windowed.rcNormalPosition;
            fprintf(stderr, LOGP "present.fullscreen=off (live): window %ldx%ld%s\n",
                    (long)(r.right - r.left), (long)(r.bottom - r.top),
                    s_windowed.showCmd == SW_SHOWMAXIMIZED ? ", maximised" : "");
            return;
        }
        SetWindowLongPtrW(s_hwnd, GWL_STYLE, (LONG_PTR)(WS_OVERLAPPEDWINDOW | WS_VISIBLE));
    }
    SetWindowPos(s_hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    fprintf(stderr, LOGP "present.fullscreen=%s (live): window %ldx%ld\n",
            full ? "on" : "off", (long)(r.right - r.left), (long)(r.bottom - r.top));
}

/* Live options (nv2a_host_opts_request) are applied after the present:
 * the one point where no target is bound and no upload is in flight, and
 * the frame shown was drawn at the old render.scale, which the present
 * sizes its source by (applied first, a 2x frame lowered to 1x would show
 * its top-left quarter for one flip). Filter, aniso, movie and overscan
 * are read where they are used (present_blit, the sampler key, the movie
 * draw), so storing them is the change. */
static void d3d11_on_flip(uint32_t surface_offset, uint32_t pitch)
{
    unsigned changed;
    d3d11_flip(surface_offset, pitch);
    changed = nv2a_host_opts_apply();
    if ((changed & NV2A_OPT_SCALE) && s_dev)
        scale_drop();
    if (changed & NV2A_OPT_FULLSCREEN)
        fullscreen_switch(nv2a_host_opts()->fullscreen);
}

/* ---- registration ---------------------------------------------------------- */

/* ---- Visibility tests: D3D11 occlusion queries ----------------------
 *
 * BeginVisibilityTest is CLEAR_REPORT_VALUE + SET_ZPASS_PIXEL_COUNT_ENABLE(1),
 * EndVisibilityTest is ENABLE(0) + GET_REPORT. Each stretch with counting on
 * is one D3D11_QUERY_OCCLUSION (begun at enable, or at a clear while
 * enabled; ended at disable or the report), and a report sums the stretches
 * since the last clear. The count is samples passing depth and stencil with
 * the pixel shader's alpha-test discard applied, the GPU's version of the
 * CPU path's put_pixel count.
 *
 * Latency: a report is not completed at GET_REPORT. Its queries go on a
 * pending list, polled (GetData without a flush) at every batch. Until then
 * the status keeps D3D's 0xFFFFFFFF, which GetVisibilityTestResult reads as
 * D3DERR_TESTINCOMPLETE. When done, count then status 0 are written. At the
 * end of each walk, before a semaphore release that follows a report (both
 * on_poll) and at the flip after Present, nv2a_occ_settle waits for the
 * results while the frame's RECOMP_DEBUG=occ_settle_us budget lasts (8000
 * by default), then settles whatever is still pending with the report
 * address's last count (visible the first time), so the title never reads a
 * test as incomplete (nv2a_backend_common.h). occ_delay_us holds
 * every result back (a test hook for a slow GPU). RECOMP_D3D11_OCC=sync
 * waits for the result at GET_REPORT instead (the stall the flip log's
 * "occ" shows), =fixed leaves reports to the walker (RECOMP_ZPASS_FIXED,
 * every test visible). A report still pending after 250 ms, or one with too
 * many stretches, completes as visible (0x10000); the flip log's "visible"
 * (s_occ_vis) counts those fallbacks too.
 *
 * Reused reports: the XDK recycles its report slots, so a GET_REPORT can name
 * a va that still has a report pending. The new one supersedes it, before
 * anything is polled: the old queries go back to the pool unread and nothing
 * is written for the old test, so its late completion cannot land over the
 * title's reset status or the new count. */
/* Pending and retired reports, a query each at least, and the open ones. */
#define OCC_POOL    (NV2A_OCC_PENDING * 3 + NV2A_OCC_SEG)
static ID3D11Query *s_occ_pool[OCC_POOL];
static int          s_occ_pool_n, s_occ_made;
static int          s_occ_mode = -1;         /* 0 defer, 1 sync, 2 fixed */

static int occ_mode(void)
{
    if (s_occ_mode < 0) {
        const char *e = recomp_env(RENV_D3D11_OCC);
        s_occ_mode = !e ? 0 : !_stricmp(e, "sync") ? 1 : !_stricmp(e, "fixed") ? 2 : 0;
    }
    return s_occ_mode;
}

static uint64_t occ_now_us(void *ctx)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    (void)ctx;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000.0 / (double)f.QuadPart);
}

static void *occ_get(void *ctx)
{
    ID3D11Query *q = NULL;
    D3D11_QUERY_DESC qd;
    (void)ctx;
    if (s_occ_pool_n)
        return s_occ_pool[--s_occ_pool_n];
    if (s_occ_made >= OCC_POOL)
        return NULL;
    qd.Query = D3D11_QUERY_OCCLUSION;
    qd.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateQuery(s_dev, &qd, &q)))
        return NULL;
    s_occ_made++;
    return q;
}

static void occ_put(void *ctx, void *q)
{
    (void)ctx;
    if (q && s_occ_pool_n < OCC_POOL)
        s_occ_pool[s_occ_pool_n++] = (ID3D11Query *)q;
    else if (q)
        ID3D11Query_Release((ID3D11Query *)q);
}

static void occ_begin(void *ctx, void *q)
{
    (void)ctx;
    ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)q);
}

static void occ_end(void *ctx, void *q)
{
    (void)ctx;
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)q);
}

/* GetData without a flush polls; with one, it pushes the GPU along. */
static int occ_result(void *ctx, void *q, int flush, uint64_t *count)
{
    UINT64 v = 0;
    (void)ctx;
    if (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)q, &v, sizeof v,
                                    flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        return 0;
    *count = v;
    return 1;
}

static const struct nv2a_occ_ops s_occ_ops = {
    occ_get, occ_put, occ_begin, occ_end, occ_result, occ_now_us, NULL
};

static uint64_t occ_env_us(recomp_env_id id, uint64_t def)
{
    long v = recomp_env_int(id, (long)def);
    return v > 0 ? (uint64_t)v : 0;
}

/* The tracker, set up on first use (init() has made the device). */
static int occ_ready_on(void)
{
    if (occ_mode() == 2 || !init())
        return 0;
    if (!s_occ.ops) {
        nv2a_occ_init(&s_occ, &s_occ_ops, (uint8_t *)xbox_GetMemoryOffset());
        nv2a_occ_set_delay(&s_occ, occ_env_us(RENV_OCC_DELAY_US, 0));
        nv2a_occ_set_settle(&s_occ, occ_env_us(RENV_OCC_SETTLE_US,
                                               NV2A_OCC_SETTLE_US));
        nv2a_occ_set_trace(&s_occ, recomp_env(RENV_ZPASS_TRACE) != NULL);
    }
    return 1;
}

static void d3d11_on_zpass(int enable)
{
    if (occ_ready_on())
        nv2a_occ_zpass(&s_occ, enable);
}

static void d3d11_on_zpass_clear(void)
{
    if (occ_ready_on())
        nv2a_occ_zpass_clear(&s_occ);
}

static int d3d11_on_report(uint32_t va)
{
    if (!occ_ready_on())
        return 0;
    nv2a_occ_report(&s_occ, va, occ_mode() == 1);
    return 1;
}

/* At the end of a walk, before a semaphore release after a report, and
 * every ack pass (retired queries only, unflushed, when nothing is
 * pending). */
static void d3d11_on_poll(void)
{
    if ((s_occ.npend || s_occ.nret) && s_dev)
        nv2a_occ_settle(&s_occ);
}

static const struct nv2a_pb_backend s_d3d11_backend = {
    d3d11_on_clear,
    d3d11_on_zclear,
    d3d11_on_draw,
    d3d11_on_flip,
    d3d11_on_zpass,
    d3d11_on_zpass_clear,
    d3d11_on_report,
    d3d11_on_poll,
    0,                  /* writes_back: presents and dumps from its swap chain */
    NULL,               /* sync_guest */
    1,                  /* merge_draws */
};

/* For tests/d3d11_backend_smoke: the size of the target at a colour offset
 * and the part of it the last flip showed, 0 when there is none. */
int nv2a_pb_d3d11_shown(uint32_t offset, uint32_t *tw, uint32_t *th,
                        uint32_t *sw, uint32_t *sh)
{
    int i;
    for (i = 0; i < RT_MAX; i++)
        if (s_rt[i].tex && s_rt[i].offset == offset) {
            *tw = s_rt[i].w;
            *th = s_rt[i].h;
            *sw = s_shown_w;
            *sh = s_shown_h;
            return 1;
        }
    return 0;
}

/* For tests/d3d11_backend_smoke: the host size of the target at a colour
 * offset (render.scale times its guest size), 0 when there is none. */
int nv2a_pb_d3d11_host_size(uint32_t offset, uint32_t *hw, uint32_t *hh)
{
    int i;
    for (i = 0; i < RT_MAX; i++)
        if (s_rt[i].tex && s_rt[i].offset == offset) {
            *hw = s_rt[i].hw;
            *hh = s_rt[i].hh;
            return 1;
        }
    return 0;
}

/* For tests/d3d11_backend_smoke: the swap chain's back-buffer size. */
void nv2a_pb_d3d11_back_size(uint32_t *w, uint32_t *h)
{
    *w = s_back_w;
    *h = s_back_h;
}

/* For tests/d3d11_backend_smoke: textures uploaded so far (a scale change
 * keeps the cache, so it uploads none). */
uint64_t nv2a_pb_d3d11_tex_uploads(void)
{
    return s_tex_uploads;
}

/* For tests/d3d11_backend_smoke: the back-buffer pixel the last flip read,
 * 0xAARRGGBB, at the point the previous call set (0xDEADBEEF before a flip
 * has read one there); then (x, y) is the point the next flips read, and
 * (-1, -1) stops the reads (each is a copy and a Map stall per flip). */
uint32_t nv2a_pb_d3d11_back_pixel(int x, int y)
{
    uint32_t px = s_probe_px;
    if (x != s_probe_x || y != s_probe_y)
        s_probe_px = 0xDEADBEEFu;
    s_probe_x = x;
    s_probe_y = y;
    return px;
}

/* For tests/d3d11_backend_smoke: the pixel at guest (x, y) of the target
 * at a colour offset, 0xAARRGGBB, or 0xDEADBEEF when there is none. */
uint32_t nv2a_pb_d3d11_pixel(uint32_t offset, int x, int y)
{
    int i;
    for (i = 0; i < RT_MAX; i++)
        if (s_rt[i].tex && s_rt[i].offset == offset)
            return read_pixel(&s_rt[i], x, y);
    return 0xDEADBEEFu;
}

const struct nv2a_pb_backend *nv2a_pb_d3d11_backend(void)
{
    return &s_d3d11_backend;
}

/* RECOMP_PB_BACKEND=d3d11 routes the executor here. Called once from the
 * executor's first method, on the ack thread. */
void nv2a_pb_d3d11_register_from_env(void)
{
    const char *b = recomp_env(RENV_PB_BACKEND);
    if (b && !_stricmp(b, "d3d11")) {
        nv2a_pb_set_backend(&s_d3d11_backend);
        fprintf(stderr, LOGP "RECOMP_PB_BACKEND=d3d11: executor draws through D3D11\n");
    }
}

#endif /* _WIN32 */
