/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 * RECOMP_D3D_PRESENT=1 shows the same frames through a D3D11 swap chain of
 * the window's own instead (gpu_present.c): scaled on the GPU, vsync-paced.
 *
 * The window opens at RECOMP_WINDOW_SCALE times the frame (default 2, made
 * smaller until it fits the desktop) and can be resized; the frame is
 * letterboxed to its 4:3 shape. F11 or Alt+Enter toggles borderless
 * fullscreen, RECOMP_FULLSCREEN=1 starts in it. Closing the window ends the
 * process, as the SDL window does.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Which keys are down, for the pad stand-in in src/input: this window
 * records them in the shared key table (src/input/keyboard.c).
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them. */
#include "keyboard.h"   /* src/input */
#include "gpu_present.h"
#include "recomp_exit_hook.h"   /* src/platform */

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * Two buffers and an index, swapped after the copy completes, so the window
 * thread is never reading the one being filled. */
static uint32_t     *s_present[2];
static volatile LONG s_present_idx = -1;   /* -1 until the first flip */

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Vertical anti-aliasing factor of the surface being shown: 2 for 2x2, where
 * the surface has twice the rows as well as twice the columns, and 1 for
 * everything else (2x1 included). Only the pushbuffer executor knows it. */
static uint32_t s_fb_aa_sy = 1;

void xbox_FramebufferWindowSetAA(uint32_t sy)
{
    s_fb_aa_sy = sy ? sy : 1;
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height,
                                             4);
        if (!s_present[next])
            return;
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* 2x-wide anti-aliased 32-bit surface: every other pixel, as in
             * fb_convert. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    /* Published only once it is whole. */
    InterlockedExchange(&s_present_idx, next);
}

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i)
        s_title[i] = 0;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

/* Alt+Enter / F11: borderless fullscreen on the window's monitor, and back.
 * (DXGI's own Alt+Enter, exclusive mode at the frame's 640x480, is off --
 * gpu_present.c; both presenters letterbox to any size.) */
static void fb_toggle_fullscreen(HWND h)
{
    static int full;
    static RECT saved;
    static LONG_PTR saved_style;
    if (!full) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        GetWindowRect(h, &saved);
        saved_style = GetWindowLongPtrA(h, GWL_STYLE);
        SetWindowLongPtrA(h, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        full = 1;
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, saved_style);
        SetWindowPos(h, NULL, saved.left, saved.top, saved.right - saved.left,
                     saved.bottom - saved.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        full = 0;
    }
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* First press only (bit 30: the key was already down), and with Alt held
     * (bit 29) for Enter; neither reaches the key table. */
    if ((m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30)))
        || (m == WM_KEYDOWN && w == VK_F11 && !(l & (1 << 30)))) {
        fb_toggle_fullscreen(h);
        return 0;
    }
    if (m == WM_SYSKEYDOWN && w == VK_RETURN)
        return 0;                       /* auto-repeat of Alt+Enter */
    switch (m) {
    case WM_CLOSE:
    case WM_DESTROY:
        /* Closing the window ends the run. Only stopping this thread left
         * every other one running -- audio played on, and the process had
         * to be ended from Task Manager. The guest has no exit path of its
         * own to ask. TerminateProcess rather than ExitProcess: the guest
         * threads are still running, and ExitProcess's cleanup can wait on
         * a lock one of them held. The exit hook does the reporting that
         * would otherwise be skipped. RECOMP_CLOSE_KEEPS_RUNNING=1 only
         * closes the window, as before. */
        InterlockedExchange(&s_fb_running, 0);
        if (!getenv("RECOMP_CLOSE_KEEPS_RUNNING")) {
            fprintf(stderr, "  [FBWIN] window closed: exiting\n");
            recomp_exit_hook_run("window");
            fflush(stdout);
            fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        xbox_InputKeySet((int)w, 1);
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        xbox_InputKeySet((int)w, 0);
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        xbox_InputKeysClear();
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch
                           * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* A 2x-wide anti-aliased 32-bit surface (pitch = 2 * width * 4):
             * take every other pixel. X-Men Legends renders its 640-wide
             * frame into a 1280-wide surface, which read as "8 bytes per
             * pixel" and showed black. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

/* GDI: the frame letterboxed into the client area, like the D3D11
 * presenter; the borders are cleared when the size changes. */
static void gdi_blit(HWND hwnd, HDC hdc, const BITMAPINFO *bi)
{
    static LONG lw, lh;
    RECT c;
    LONG cw, ch, w, h;
    GetClientRect(hwnd, &c);
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0)
        return;
    if (cw != lw || ch != lh) {
        FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        lw = cw; lh = ch;
    }
    if ((long long)cw * s_fb_height <= (long long)ch * s_fb_width) {
        w = cw; h = (LONG)((long long)cw * s_fb_height / s_fb_width);
    } else {
        h = ch; w = (LONG)((long long)ch * s_fb_width / s_fb_height);
    }
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);     /* HALFTONE wants it after the mode */
    StretchDIBits(hdc, (cw - w) / 2, (ch - h) / 2, w, h,
                  0, 0, (int)s_fb_width, (int)s_fb_height,
                  s_rgb, bi, DIB_RGB_COLORS, SRCCOPY);
}

/* The converted frame in s_rgb, on screen: D3D11 when it is up (which waits
 * for vsync, so returns 1 when the loop is already paced), else GDI. */
static int fb_show(HWND hwnd, HDC hdc, const BITMAPINFO *bi)
{
    if (gpu_present_active())
        return gpu_present_frame(s_rgb);
    gdi_blit(hwnd, hdc, bi);
    return 0;
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;

    (void)unused;

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    {
        /* RECOMP_WINDOW_SCALE (1-8, default 2, as in the SDL window): the
         * client area is the frame times this, made smaller until the window
         * fits the desktop work area, and centred on it. */
        const char *e = getenv("RECOMP_WINDOW_SCALE");
        int scale = e ? atoi(e) : 2;
        RECT work;
        if (scale < 1 || scale > 8)
            scale = 2;
        if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
            work.left = work.top = 0; work.right = 1920; work.bottom = 1080;
        }
        for (;; scale--) {
            r.left = 0; r.top = 0;
            r.right = (LONG)s_fb_width * scale; r.bottom = (LONG)s_fb_height * scale;
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            if (scale <= 1 || (r.right - r.left <= work.right - work.left
                               && r.bottom - r.top <= work.bottom - work.top))
                break;
        }
        hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                               work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                               r.right - r.left, r.bottom - r.top,
                               NULL, NULL, GetModuleHandleA(NULL), NULL);
    }
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    hdc = GetDC(hwnd);
    {
        const char *fs = getenv("RECOMP_FULLSCREEN");
        if (fs && *fs && *fs != '0')
            fb_toggle_fullscreen(hwnd);
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);
    if (gpu_present_enabled())
        gpu_present_init(hwnd, s_fb_width, s_fb_height);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        int paced = 0;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (s_present_idx >= 0 && s_rgb) {
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            LONG idx = s_present_idx;
            if (s_present[idx])
                memcpy(s_rgb, s_present[idx],
                       (size_t)s_fb_width * s_fb_height * 4);
            paced = fb_show(hwnd, hdc, &bi);
        } else if (s_fb_va && s_fb_pitch && s_rgb) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            paced = fb_show(hwnd, hdc, &bi);
        }
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 1000) {
                LONG f = s_flips;
                wchar_t tb[128];
                _snwprintf(tb, 127, L"%ls | FPS: %.1f | draws: %ld", s_title,
                           t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0,
                           (long)s_frame_draws);
                tb[127] = 0;
                SetWindowTextW(hwnd, tb);
                t0 = now; f0 = f;
            }
        }
        if (!paced)
            Sleep(16);                  /* D3D11's Present waited for vsync */
    }

    gpu_present_shutdown();
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#endif /* _WIN32: POSIX hosts get the SDL2 window in fb_present_sdl.c */
