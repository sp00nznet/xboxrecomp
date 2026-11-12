/**
 * The D3D11 backend's Windows host (d3d11_host.h): moved out of
 * nv2a_pb_d3d11.c unchanged, so the backend itself names no Win32 call and
 * a second host (d3d11_host_sdl.c, Linux over dxvk-native) can take this
 * one's place.
 *
 * Off Windows this file compiles to nothing.
 */
#ifdef _WIN32

#define COBJMACROS
#include <windows.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d11_host.h"
#include "recomp_env.h"
#include "img_dump.h"
#include "recomp_exit_hook.h"
#include "video/video_player.h"   /* xbox_FramebufferWindowTitleText */
#include "video/window_title.h"

#define LOGP "[d3d11] "
#define D3D11_WND_CLASS "RecompNv2aD3D11"   /* RegisterClassA, then CreateWindowW as L"" D3D11_WND_CLASS */

/* ---- clocks ------------------------------------------------------------- */

double d3d11_host_now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

uint64_t d3d11_host_now_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000.0 / (double)f.QuadPart);
}

uint64_t d3d11_host_ticks_ms(void)
{
    return (uint64_t)GetTickCount64();
}

/* ---- thread ----------------------------------------------------------- */

void d3d11_host_yield(void)
{
    SwitchToThread();
}

void d3d11_host_cpu_relax(void)
{
    YieldProcessor();
}

/* ---- HLSL compiler ---------------------------------------------------- */

typedef HRESULT (WINAPI *compile_fn)(LPCVOID, SIZE_T, LPCSTR,
                                     const D3D_SHADER_MACRO *, ID3DInclude *,
                                     LPCSTR, LPCSTR, UINT, UINT,
                                     ID3DBlob **, ID3DBlob **);
typedef HRESULT (WINAPI *create_blob_fn)(SIZE_T, ID3DBlob **);

static HMODULE        s_dll;
static compile_fn     s_compile;
static create_blob_fn s_create_blob;  /* a cache hit's blob */

int d3d11_host_compiler_load(void)
{
    s_dll = LoadLibraryA("d3dcompiler_47.dll");
    if (s_dll)
        s_compile = (compile_fn)(void (*)(void))GetProcAddress(s_dll, "D3DCompile");
    if (!s_compile) {
        fprintf(stderr, LOGP "no D3DCompile in d3dcompiler_47.dll\n");
        return 0;
    }
    s_create_blob = (create_blob_fn)(void (*)(void))GetProcAddress(s_dll, "D3DCreateBlob");
    return 1;
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

/* "<path>|<size>|<write time>|<file version>" of the d3dcompiler_47.dll in
 * use. Without D3DCreateBlob a hit has nowhere to go, so no identity. */
int d3d11_host_compiler_id(char *out, size_t n)
{
    char path[MAX_PATH * 3];
    wchar_t wpath[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    DWORD len;

    len = GetModuleFileNameW(s_dll, wpath, MAX_PATH);
    if (!s_create_blob || !len || len >= MAX_PATH
     || !GetFileAttributesExW(wpath, GetFileExInfoStandard, &fa)
     || !WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, (int)sizeof path, NULL, NULL))
        return 0;
    snprintf(out, n, "%s|%08lx%08lx|%08lx%08lx|%016llx",
             path, (unsigned long)fa.nFileSizeHigh, (unsigned long)fa.nFileSizeLow,
             (unsigned long)fa.ftLastWriteTime.dwHighDateTime,
             (unsigned long)fa.ftLastWriteTime.dwLowDateTime, dll_file_version(s_dll));
    return 1;
}

int32_t d3d11_host_compile(const void *src, size_t len, const char *name,
                           const char *entry, const char *target,
                           unsigned flags1, unsigned flags2,
                           struct ID3D10Blob **code, struct ID3D10Blob **err)
{
    return (int32_t)s_compile(src, len, name, NULL, NULL, entry, target,
                              flags1, flags2, code, err);
}

int d3d11_host_blob(size_t n, struct ID3D10Blob **out)
{
    return SUCCEEDED(s_create_blob(n, out));
}

/* ---- window ----------------------------------------------------------- */

static HWND s_hwnd;
/* This window is not fb_present.c's, which keeps the title bar's name (and,
 * with RECOMP_TRACE=title, FPS and draws); ours comes from the same place.
 * One clock for the creation title and the per-flip refresh, so the window
 * opens with the game's name and the first flip has nothing to rewrite. */
static struct xbox_title_clock s_title_clock;
/* The client size WM_SIZE last reported (0 = none since the window opened). */
static volatile UINT s_client_w, s_client_h;

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
        /* The queued PNGs too, even if the hook table had no slot for the
         * writer's drain: this is how a bench's RECOMP_WINDOW_QUIT_AFTER
         * ends, seconds after the last dump. */
        img_dump_flush(5000);
        ExitProcess(0);
    }
    if (m == WM_SIZE && w != SIZE_MINIMIZED && LOWORD(l) && HIWORD(l)) {
        /* Recorded only: the flip (this thread, which pumps the window)
         * resizes the swap chain before its next present
         * (d3d11_host_client_size). */
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

void d3d11_host_pump(void)
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

int d3d11_host_window_open(int popup, unsigned w, unsigned h)
{
    WNDCLASSA wc;
    wchar_t title[192];
    RECT r = {0, 0, (LONG)w, (LONG)h};

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = D3D11_WND_CLASS;
    RegisterClassA(&wc);
    if (!xbox_FramebufferWindowTitleText(title, 192, &s_title_clock))
        title[0] = 0;
    if (popup) {
        s_hwnd = CreateWindowW(L"" D3D11_WND_CLASS, title,
                               WS_POPUP | WS_VISIBLE, 0, 0, (int)w, (int)h,
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
    return 1;
}

void *d3d11_host_window(void)
{
    return s_hwnd;
}

void d3d11_host_swap_chain_made(void *swap)
{
    IDXGISwapChain *sc = (IDXGISwapChain *)swap;

    /* DXGI's default window association takes Alt+Enter as a fullscreen
     * toggle, and nothing here follows that switch. With the keyboard on,
     * Alt is a key players hold in play (a walk key) and Enter is START,
     * so a pause while walking would switch to exclusive fullscreen behind
     * the presenter's back. Stock runs (keyboard off) keep DXGI's
     * default. */
    if (xbox_InputKeyboardOn()) {
        IDXGIFactory *f = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetParent(sc, &IID_IDXGIFactory, (void **)&f)) && f) {
            IDXGIFactory_MakeWindowAssociation(f, s_hwnd, DXGI_MWA_NO_ALT_ENTER);
            IDXGIFactory_Release(f);
        }
    }
}

/* WM_CLOSE ends the process on this thread's own pump, between frames. */
void d3d11_host_present_begin(void) {}
void d3d11_host_present_end(void) {}
void d3d11_host_work_begin(void) {}
void d3d11_host_work_end(void) {}

void d3d11_host_ready(void)
{
    /* Only once the device is up: a failed init returns before any frame
     * is pumped, and a poster left behind would take pad_quit's close for
     * a window that never closes, and the quit would be lost. */
    recomp_quit_set(post_close);
}

void d3d11_host_title(void)
{
    wchar_t tb[192];
    if (xbox_FramebufferWindowTitleText(tb, 192, &s_title_clock))
        SetWindowTextW(s_hwnd, tb);
}

void d3d11_host_client_size(unsigned *w, unsigned *h)
{
    *w = s_client_w;
    *h = s_client_h;
}

void d3d11_host_client_rect(unsigned *w, unsigned *h)
{
    RECT cr = {0, 0, 0, 0};
    GetClientRect(s_hwnd, &cr);
    *w = (unsigned)(cr.right - cr.left);
    *h = (unsigned)(cr.bottom - cr.top);
}

void d3d11_host_screen_size(unsigned *w, unsigned *h)
{
    *w = (unsigned)GetSystemMetrics(SM_CXSCREEN);
    *h = (unsigned)GetSystemMetrics(SM_CYSCREEN);
}

void d3d11_host_monitor_rect(int *x, int *y, unsigned *w, unsigned *h)
{
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
    *x = (int)r.left;
    *y = (int)r.top;
    *w = (unsigned)(r.right - r.left);
    *h = (unsigned)(r.bottom - r.top);
}

void d3d11_host_set_client_size(unsigned w, unsigned h, int framed)
{
    RECT r = {0, 0, (LONG)w, (LONG)h};
    if (framed)
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(s_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void d3d11_host_popup(int x, int y, unsigned w, unsigned h)
{
    SetWindowLongPtrW(s_hwnd, GWL_STYLE, (LONG_PTR)(WS_POPUP | WS_VISIBLE));
    SetWindowPos(s_hwnd, HWND_TOP, x, y, (int)w, (int)h,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

void d3d11_host_framed(void)
{
    SetWindowLongPtrW(s_hwnd, GWL_STYLE, (LONG_PTR)(WS_OVERLAPPEDWINDOW | WS_VISIBLE));
    SetWindowPos(s_hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

/* present.fullscreen changed live (fullscreen_switch in the backend): swap
 * the window's style and size on this thread, which owns the window.
 * SetWindowPos sends WM_SIZE here at once, so the next flip's present_resize
 * gives the swap chain the new client size before it presents; no other
 * thread touches the window, so there is no resize to race. Borderless over
 * the window's monitor, as at start; the windowed placement is the one
 * left, or the starting size (dw x dh) when the run began fullscreen. Under
 * present.surface=internal the client size stays the frame's, fw x fh (the
 * compositor scales it), only the border changes. */
void d3d11_host_fullscreen(int full, int internal, unsigned fw, unsigned fh,
                           unsigned dw, unsigned dh)
{
    /* The placement, not GetWindowRect: a maximised window comes back
     * maximised over its restored rectangle, and a minimised one's rect
     * (off screen) is not where it goes back to. */
    static WINDOWPLACEMENT s_windowed;
    static int s_have_windowed;
    RECT r;

    if (!s_hwnd)
        return;
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
            r.right = (LONG)fw;
            r.bottom = (LONG)fh;
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
            UINT ww = dw, wh = dh;
            if (internal) {
                ww = fw;
                wh = fh;
            }
            r.left = r.top = 0;
            r.right = (LONG)ww;
            r.bottom = (LONG)wh;
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

/* ---- files ------------------------------------------------------------ */

void d3d11_host_mkdir(const char *path)
{
    CreateDirectoryA(path, NULL);
}

#endif /* _WIN32 */
