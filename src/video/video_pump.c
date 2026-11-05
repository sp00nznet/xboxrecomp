/**
 * The pump behind xbox_VideoPlayFile().
 *
 * Owns a window, a D3D8 device and a Media Foundation session, all on one
 * thread of its own. Nothing here reads or writes guest memory and nothing
 * runs on the guest's thread, so a title that is still booting keeps booting
 * while its video plays.
 *
 * The guest decides when. A title opens its movie file at the point its own
 * logic reaches the intro; the runtime notices that open and plays that file.
 */
#if defined(_WIN32)
#define COBJMACROS
#include <windows.h>
#include "recomp_env.h"
#include <stdio.h>
#include <string.h>

#include "video_player.h"
#include "d3d/d3d8_xbox.h"
#include "d3d/d3d8_internal.h"

#define VP_WINDOW_W 640
#define VP_WINDOW_H 480

static volatile LONG s_playing;
static char          s_path[MAX_PATH];

static LRESULT CALLBACK vp_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE || m == WM_DESTROY) {
        InterlockedExchange(&s_playing, 0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static HWND vp_create_window(void)
{
    WNDCLASSA wc;
    RECT r;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = vp_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "XboxRecompVideo";
    RegisterClassA(&wc);

    r.left = 0; r.top = 0; r.right = VP_WINDOW_W; r.bottom = VP_WINDOW_H;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowExA(0, "XboxRecompVideo", "Xbox Recomp - Video",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
}

extern void xbox_log_thread_role(const char *role, uint32_t routine);

static DWORD WINAPI vp_thread(LPVOID unused)
{
    xbox_log_thread_role("vp-pump", 0);
    IDirect3D8 *d3d;
    IDirect3DDevice8 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    HWND hwnd;
    HRESULT hr;
    DWORD last;
    unsigned frames = 0;

    (void)unused;

    hwnd = vp_create_window();
    if (!hwnd) {
        fprintf(stderr, "  [VIDEO] window creation failed\n");
        goto done;
    }

    d3d = xbox_Direct3DCreate8(0);
    if (!d3d) {
        fprintf(stderr, "  [VIDEO] Direct3DCreate8 failed\n");
        goto done;
    }

    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth        = VP_WINDOW_W;
    pp.BackBufferHeight       = VP_WINDOW_H;
    pp.BackBufferFormat       = D3DFMT_X8R8G8B8;
    pp.BackBufferCount        = 1;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow          = hwnd;
    pp.Windowed               = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;

    hr = d3d->lpVtbl->CreateDevice(d3d, 0, 0, hwnd, 0, &pp, &dev);
    if (FAILED(hr) || !dev) {
        fprintf(stderr, "  [VIDEO] CreateDevice failed: 0x%08lX\n", (unsigned long)hr);
        goto done;
    }

    if (video_init() != 0)
        goto done;
    if (video_open(s_path) != 0) {
        fprintf(stderr, "  [VIDEO] could not open %s\n", s_path);
        goto done;
    }

    fprintf(stderr, "  [VIDEO] playing %s\n", s_path);
    last = GetTickCount();

    while (InterlockedCompareExchange(&s_playing, 1, 1)) {
        MSG msg;
        DWORD now = GetTickCount();
        float dt = (float)(now - last) / 1000.0f;
        last = now;

        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        /* video_update returns 1 when it uploaded a new frame, 0 when the
         * next one is not due yet, and negative at end of stream or on error.
         * Only the negative case ends playback -- treating any non-zero as
         * failure stops on the first frame that actually decoded, which is
         * every frame worth showing. */
        if (video_update(dt) < 0 || video_is_finished())
            break;

        /* Clear, then the frame as a fullscreen quad, then present. */
        dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           0xFF000000u, 1.0f, 0);
        dev->lpVtbl->BeginScene(dev);
        video_render();
        dev->lpVtbl->EndScene(dev);
        dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);
        frames++;

        /* A couple of frames to disc as evidence that real pixels were
         * drawn -- two of them, far enough apart to show the picture moving
         * rather than one still held for the duration. */
        if ((frames == 60 || frames == 80) && recomp_env(RENV_FMV_DUMP)) {
            char out[MAX_PATH];
            _snprintf_s(out, sizeof(out), _TRUNCATE, "%s.%u.bmp",
                        recomp_env(RENV_FMV_DUMP), frames);
            video_dump_frame_bmp(out);
        }

        Sleep(1);
    }

    fprintf(stderr, "  [VIDEO] finished %s after %u presented frames\n",
            s_path, frames);
    video_close();
    video_shutdown();

done:
    InterlockedExchange(&s_playing, 0);
    return 0;
}

int xbox_VideoPlayFile(const char *host_path)
{
    HANDLE th;

    if (!host_path || !*host_path)
        return -1;
    /* One at a time: the pump owns a window and a device, and a second one
     * would fight the first for both. */
    if (InterlockedCompareExchange(&s_playing, 1, 0) != 0)
        return -1;

    strncpy(s_path, host_path, sizeof(s_path) - 1);
    s_path[sizeof(s_path) - 1] = '\0';

    th = CreateThread(NULL, 0, vp_thread, NULL, 0, NULL);
    if (!th) {
        InterlockedExchange(&s_playing, 0);
        return -1;
    }
    CloseHandle(th);
    return 0;
}

int xbox_VideoIsPlaying(void)
{
    return InterlockedCompareExchange(&s_playing, 0, 0) != 0;
}

#else  /* !_WIN32 */
int xbox_VideoPlayFile(const char *p) { (void)p; return -1; }
int xbox_VideoIsPlaying(void) { return 0; }
#endif
