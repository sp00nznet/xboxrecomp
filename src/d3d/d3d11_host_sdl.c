/**
 * The D3D11 backend's Linux host (d3d11_host.h), for the dxvk-native build:
 * dxvk-native's DXGI makes the swap chain on the SDL presenter's window
 * (fb_present_swap.h; DXVK_WSI_DRIVER=SDL3 takes the SDL_Window * where
 * Windows takes an HWND), and vkd3d-shader compiles the HLSL
 * (d3d11_hlsl.h).
 *
 * The window is the presenter's, not ours: its event loop, input, title and
 * present.fullscreen stay on the main thread, as on every other SDL path,
 * so the pump, title and the window-shape calls below are no-ops or a
 * resize request. The backend runs on the NV2A ack thread and only reads
 * the drawable size the main thread last saw.
 *
 * Includes no Windows or DXVK header: the blob is this file's own
 * ID3D10Blob, laid out as COM's C binding has it (a vtable pointer first,
 * the five methods in IUnknown then ID3D10Blob order), which is what the
 * backend's ID3D10Blob_GetBufferPointer and friends call through.
 *
 * On Windows this file compiles to nothing.
 */
#ifndef _WIN32

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "d3d11_host.h"
#include "d3d11_hlsl.h"
#include "video/fb_present_swap.h"

#define LOGP "[d3d11] "

/* ---- clocks ------------------------------------------------------------- */

static uint64_t mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

double d3d11_host_now_ms(void)
{
    return (double)mono_ns() / 1e6;
}

uint64_t d3d11_host_now_us(void)
{
    return mono_ns() / 1000u;
}

uint64_t d3d11_host_ticks_ms(void)
{
    return mono_ns() / 1000000u;
}

/* ---- thread ------------------------------------------------------------- */

void d3d11_host_yield(void)
{
    sched_yield();
}

void d3d11_host_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

/* ---- blobs -------------------------------------------------------------- */

/* COM's types as dxvk-native defines them off Windows: HRESULT a 32-bit
 * int, ULONG a 32-bit unsigned, no stdcall (System V calls). Checked again
 * when phase 2 links the real headers (d3d11_host_sdl ctest pins the
 * layout here). */
typedef int32_t  blob_hresult;
typedef uint32_t blob_ulong;

struct ID3D10Blob;
struct blob_vtbl {
    blob_hresult (*QueryInterface)(struct ID3D10Blob *, const void *riid, void **out);
    blob_ulong   (*AddRef)(struct ID3D10Blob *);
    blob_ulong   (*Release)(struct ID3D10Blob *);
    void        *(*GetBufferPointer)(struct ID3D10Blob *);
    size_t       (*GetBufferSize)(struct ID3D10Blob *);
};

struct ID3D10Blob {
    const struct blob_vtbl *lpVtbl;
    blob_ulong refs;          /* the ack thread's alone: no atomics */
    size_t size;
    unsigned char data[];
};

#define BLOB_E_NOINTERFACE ((blob_hresult)0x80004002u)
#define BLOB_E_OUTOFMEMORY ((blob_hresult)0x8007000Eu)
#define BLOB_E_FAIL        ((blob_hresult)0x80004005u)

/* The backend never asks a blob for another interface. */
static blob_hresult blob_query(struct ID3D10Blob *b, const void *riid, void **out)
{
    (void)b; (void)riid;
    if (out)
        *out = NULL;
    return BLOB_E_NOINTERFACE;
}

static blob_ulong blob_addref(struct ID3D10Blob *b)
{
    return ++b->refs;
}

static blob_ulong blob_release(struct ID3D10Blob *b)
{
    blob_ulong r = --b->refs;
    if (!r)
        free(b);
    return r;
}

static void *blob_ptr(struct ID3D10Blob *b)
{
    return b->data;
}

static size_t blob_size(struct ID3D10Blob *b)
{
    return b->size;
}

static const struct blob_vtbl s_blob_vtbl = {
    blob_query, blob_addref, blob_release, blob_ptr, blob_size,
};

static struct ID3D10Blob *blob_new(const void *data, size_t n)
{
    struct ID3D10Blob *b = malloc(sizeof *b + (n ? n : 1));
    if (!b)
        return NULL;
    b->lpVtbl = &s_blob_vtbl;
    b->refs = 1;
    b->size = n;
    if (data && n)
        memcpy(b->data, data, n);
    else
        memset(b->data, 0, n ? n : 1);
    return b;
}

/* ---- HLSL compiler ---------------------------------------------------- */

static const char *s_hlsl;   /* the compiler's version, once found */

int d3d11_host_compiler_load(void)
{
    s_hlsl = d3d11_hlsl_version();
    if (!s_hlsl) {
        fprintf(stderr, LOGP "no HLSL compiler (vkd3d-shader)\n");
        return 0;
    }
    fprintf(stderr, LOGP "HLSL compiler: %s\n", s_hlsl);
    return 1;
}

/* The version names the compiler: the library is the system's (or the
 * package's) libvkd3d-shader, and a new one is a new version. Never equal
 * to the Windows host's id (a d3dcompiler_47 path), so a cache dir shared
 * with a Proton run keeps the two compilers' DXBC apart. */
int d3d11_host_compiler_id(char *out, size_t n)
{
    if (!s_hlsl)
        return 0;
    snprintf(out, n, "vkd3d-shader|%s", s_hlsl);
    return 1;
}

int32_t d3d11_host_compile(const void *src, size_t len, const char *name,
                           const char *entry, const char *target,
                           unsigned flags1, unsigned flags2,
                           struct ID3D10Blob **code, struct ID3D10Blob **err)
{
    void *bytes = NULL;
    size_t nbytes = 0;
    char *msg = NULL;
    int rc;

    *code = NULL;
    if (err)
        *err = NULL;
    rc = d3d11_hlsl_compile(src, len, name, entry, target, flags1, flags2,
                            &bytes, &nbytes, err ? &msg : NULL);
    /* The messages, warnings included, as D3DCompile hands them back: a
     * NUL-terminated string in a blob, the backend prints it. */
    if (err && msg)
        *err = blob_new(msg, strlen(msg) + 1);
    d3d11_hlsl_free(msg);
    if (rc < 0)
        return BLOB_E_FAIL;
    *code = blob_new(bytes, nbytes);
    d3d11_hlsl_free(bytes);
    return *code ? 0 : BLOB_E_OUTOFMEMORY;
}

int d3d11_host_blob(size_t n, struct ID3D10Blob **out)
{
    *out = blob_new(NULL, n);
    return *out != NULL;
}

/* ---- window ----------------------------------------------------------- */

/* The presenter opened the window before the guest started, at its own
 * size (present.window_scale), which a windowed start keeps: the backend's
 * w x h there is the 1x client size, not the window's. Only
 * present.surface=internal asks for the frame's size, as the Win32 host
 * makes its client area. A popup (present.shader's output surface) is the
 * presenter's fullscreen on SDL, so it keeps its size. */
int d3d11_host_window_open(int popup, unsigned w, unsigned h)
{
    unsigned cw = 0, ch = 0;

    if (!xbox_FramebufferWindowSwap()) {
        fprintf(stderr, LOGP "no window to present to: the SDL presenter did "
                "not open one for the D3D11 backend\n");
        return 0;
    }
    xbox_FramebufferWindowSwapSize(&cw, &ch);
    if (!popup && xbox_FramebufferWindowSwapInternal() && w && h && (cw != w || ch != h))
        xbox_FramebufferWindowSwapResize(w, h);
    return 1;
}

void *d3d11_host_window(void)
{
    return xbox_FramebufferWindowSwap();
}

/* The presenter's loop runs the events, input, title and timed quit on the
 * main thread; the swap chain needs nothing from the window here. */
void d3d11_host_swap_chain_made(void *swap) { (void)swap; }
void d3d11_host_ready(void) {}
void d3d11_host_pump(void) {}

void d3d11_host_present_begin(void)
{
    xbox_FramebufferWindowSwapPresentBegin();
}

void d3d11_host_present_end(void)
{
    xbox_FramebufferWindowSwapPresentEnd();
}

void d3d11_host_work_begin(void)
{
    xbox_FramebufferWindowSwapWorkBegin();
}

void d3d11_host_work_end(void)
{
    xbox_FramebufferWindowSwapWorkEnd();
}
void d3d11_host_title(void) {}

void d3d11_host_client_size(unsigned *w, unsigned *h)
{
    xbox_FramebufferWindowSwapSize(w, h);
}

void d3d11_host_client_rect(unsigned *w, unsigned *h)
{
    xbox_FramebufferWindowSwapSize(w, h);
}

void d3d11_host_screen_size(unsigned *w, unsigned *h)
{
    xbox_FramebufferWindowSwapDisplay(w, h);
}

void d3d11_host_monitor_rect(int *x, int *y, unsigned *w, unsigned *h)
{
    *x = 0;
    *y = 0;
    xbox_FramebufferWindowSwapDisplay(w, h);
}

void d3d11_host_set_client_size(unsigned w, unsigned h, int framed)
{
    (void)framed;
    xbox_FramebufferWindowSwapResize(w, h);
}

/* Borderless and framed are the presenter's present.fullscreen on SDL; the
 * backend's own switches are not followed (phase 2 decides whether
 * present.shader's popup needs one). */
void d3d11_host_popup(int x, int y, unsigned w, unsigned h)
{
    (void)x; (void)y; (void)w; (void)h;
}

void d3d11_host_framed(void) {}

void d3d11_host_fullscreen(int full, int internal, unsigned fw, unsigned fh,
                           unsigned dw, unsigned dh)
{
    (void)full; (void)internal; (void)fw; (void)fh; (void)dw; (void)dh;
}

/* ---- files ------------------------------------------------------------ */

void d3d11_host_mkdir(const char *path)
{
    if (mkdir(path, 0777) != 0 && errno != EEXIST)
        fprintf(stderr, LOGP "mkdir %s: %s\n", path, strerror(errno));
}

#endif /* !_WIN32 */
