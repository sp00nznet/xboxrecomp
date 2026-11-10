/**
 * Shared FMV player: Media Foundation decode onto a D3D8 texture.
 *
 * Titles whose video is an ordinary container Windows can already decode do not
 * need their in-game decoder emulated to be watchable. The recompiled title
 * still drives everything -- it opens the file when its own logic says to, and
 * the runtime plays that file -- so the video appears where the game puts it
 * rather than being replayed on a timer.
 *
 * Lifted from burnout3's player, with its boot-sequence state machine and menu
 * dependencies removed: those are title logic, and this is the decoder.
 *
 * The pump in xbox_VideoPlayRun() owns its window, its D3D8 device and its
 * Media Foundation session on one thread, so nothing here touches the guest's
 * execution or its memory.
 */
#if defined(_WIN32)
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>
#include <math.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "ole32.lib")

#include "video_player.h"
#include "img_dump.h"
#include "d3d/d3d8_xbox.h"
#include "d3d/d3d8_internal.h"

/* ================================================================
 * Media Foundation video decoder state
 * ================================================================ */

enum vp_format { VP_FMT_RGB32, VP_FMT_NV12, VP_FMT_YUY2 };

static struct {
    int                  initialized;   /* MF started? */
    IMFSourceReader     *reader;        /* source reader */
    int                  width;
    int                  height;
    float                duration;      /* seconds */
    float                elapsed;       /* playback position */
    float                frame_time;    /* time per frame (1/fps) */
    float                next_frame_at; /* when to decode next frame */
    int                  finished;
    int                  has_frame;     /* current frame valid? */
    enum vp_format       format;        /* decoded frame format */

    /* Frame buffer (BGRA, uploaded to D3D texture) */
    uint8_t             *frame_buf;
    uint32_t             frame_buf_size;
    uint32_t             stride;
    int                  bottom_up;     /* source rows run bottom to top */
    int                  orientation_known;

    /* D3D8 texture for rendering */
    IDirect3DTexture8   *texture;
    uint32_t             tex_width;
    uint32_t             tex_height;
} g_vp;


/* ================================================================
 * Helper: convert char* to wchar_t*
 * ================================================================ */
static wchar_t *to_wide(const char *s)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(len * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, len);
    return w;
}

/* ================================================================
 * Helper: next power of 2
 * ================================================================ */
static uint32_t next_pot(uint32_t v)
{
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    return v + 1;
}


/* ================================================================
 * Public API
 * ================================================================ */

int video_init(void)
{
    if (g_vp.initialized) return 0;

    /* COM must be initialized for Media Foundation */
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "  [VIDEO] CoInitializeEx failed: 0x%08lX\n", hr);
        /* Try apartment-threaded as fallback */
        hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (FAILED(hr) && hr != S_FALSE) {
            fprintf(stderr, "  [VIDEO] CoInitializeEx (STA) also failed: 0x%08lX\n", hr);
            return -1;
        }
    }

    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        fprintf(stderr, "  [VIDEO] MFStartup failed: 0x%08lX\n", hr);
        return -1;
    }
    g_vp.initialized = 1;
    fprintf(stderr, "  [VIDEO] Media Foundation initialized\n");
    return 0;
}

void video_shutdown(void)
{
    video_close();
    if (g_vp.initialized) {
        MFShutdown();
        CoUninitialize();
        g_vp.initialized = 0;
    }
}

int video_open(const char *path)
{
    HRESULT hr;
    IMFAttributes *attrs = NULL;
    IMFMediaType *native_type = NULL;
    IMFMediaType *output_type = NULL;

    if (!g_vp.initialized) {
        if (video_init() != 0) return -1;
    }

    /* Close any existing video */
    video_close();

    /* Check file exists */
    {
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "  [VIDEO] File not found: %s\n", path);
            return -1;
        }
        fclose(f);
    }

    /* Create source reader attributes */
    hr = MFCreateAttributes(&attrs, 1);
    if (FAILED(hr)) goto fail;

    /* Create source reader from file */
    wchar_t *wpath = to_wide(path);
    hr = MFCreateSourceReaderFromURL(wpath, attrs, &g_vp.reader);
    free(wpath);
    if (FAILED(hr)) {
        fprintf(stderr, "  [VIDEO] Failed to open: %s (hr=0x%08lX)\n", path, hr);
        goto fail;
    }

    /* Get native video format to extract dimensions */
    hr = IMFSourceReader_GetNativeMediaType(g_vp.reader,
            (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native_type);
    if (FAILED(hr)) {
        fprintf(stderr, "  [VIDEO] No video stream found (hr=0x%08lX)\n", hr);
        goto fail;
    }

    /* Extract dimensions from native type */
    {
        UINT64 frame_size = 0;
        IMFMediaType_GetUINT64(native_type, &MF_MT_FRAME_SIZE, &frame_size);
        g_vp.width  = (int)(frame_size >> 32);
        g_vp.height = (int)(frame_size & 0xFFFFFFFF);
    }

    /* Extract frame rate */
    {
        UINT64 frame_rate = 0;
        IMFMediaType_GetUINT64(native_type, &MF_MT_FRAME_RATE, &frame_rate);
        uint32_t num = (uint32_t)(frame_rate >> 32);
        uint32_t den = (uint32_t)(frame_rate & 0xFFFFFFFF);
        if (num > 0 && den > 0)
            g_vp.frame_time = (float)den / (float)num;
        else
            g_vp.frame_time = 1.0f / 30.0f;
    }

    /* Extract duration */
    {
        PROPVARIANT var;
        PropVariantInit(&var);
        hr = IMFSourceReader_GetPresentationAttribute(g_vp.reader,
                (DWORD)MF_SOURCE_READER_MEDIASOURCE,
                &MF_PD_DURATION, &var);
        if (SUCCEEDED(hr) && var.vt == VT_UI8) {
            g_vp.duration = (float)(var.uhVal.QuadPart / 10000000.0);
        } else {
            g_vp.duration = 60.0f; /* fallback */
        }
        PropVariantClear(&var);
    }

    IMFMediaType_Release(native_type);
    native_type = NULL;

    /* Configure output format: RGB32 (BGRA).
     * MF requires major type + subtype at minimum. Some decoders also
     * need frame size set on the output type. */
    hr = MFCreateMediaType(&output_type);
    if (FAILED(hr)) goto fail;

    IMFMediaType_SetGUID(output_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(output_type, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);

    hr = IMFSourceReader_SetCurrentMediaType(g_vp.reader,
            (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, output_type);

    if (FAILED(hr)) {
        /* Try NV12 as fallback — widely supported, we'll convert manually */
        fprintf(stderr, "  [VIDEO] RGB32 failed (0x%08lX), trying NV12...\n", hr);
        IMFMediaType_SetGUID(output_type, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
        hr = IMFSourceReader_SetCurrentMediaType(g_vp.reader,
                (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, output_type);
    }

    if (FAILED(hr)) {
        /* Last resort: try YUY2 */
        fprintf(stderr, "  [VIDEO] NV12 failed (0x%08lX), trying YUY2...\n", hr);
        IMFMediaType_SetGUID(output_type, &MF_MT_SUBTYPE, &MFVideoFormat_YUY2);
        hr = IMFSourceReader_SetCurrentMediaType(g_vp.reader,
                (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, output_type);
    }

    if (FAILED(hr)) {
        /* Accept whatever the decoder gives us */
        fprintf(stderr, "  [VIDEO] All formats failed, using decoder default (hr=0x%08lX)\n", hr);
        IMFMediaType_Release(output_type);
        output_type = NULL;
        /* Re-query the actual output type */
        hr = IMFSourceReader_GetCurrentMediaType(g_vp.reader,
                (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &output_type);
        if (FAILED(hr)) goto fail;
    }

    /* Check what format we actually got */
    {
        GUID subtype;
        IMFMediaType_GetGUID(output_type, &MF_MT_SUBTYPE, &subtype);
        if (IsEqualGUID(&subtype, &MFVideoFormat_RGB32)) {
            g_vp.format = VP_FMT_RGB32;
            fprintf(stderr, "  [VIDEO] Output format: RGB32\n");
        } else if (IsEqualGUID(&subtype, &MFVideoFormat_NV12)) {
            g_vp.format = VP_FMT_NV12;
            fprintf(stderr, "  [VIDEO] Output format: NV12\n");
        } else if (IsEqualGUID(&subtype, &MFVideoFormat_YUY2)) {
            g_vp.format = VP_FMT_YUY2;
            fprintf(stderr, "  [VIDEO] Output format: YUY2\n");
        } else {
            g_vp.format = VP_FMT_NV12; /* assume NV12 */
            fprintf(stderr, "  [VIDEO] Output format: unknown GUID, assuming NV12\n");
        }
    }

    /* Which way up the decoder hands us rows.
     *
     * RGB32 out of Media Foundation is conventionally bottom-up, and the code
     * used to assume it always is -- but the sign of MF_MT_DEFAULT_STRIDE is
     * what actually says, and a WMV decoded here comes back top-down. Assuming
     * bottom-up flipped a frame that needed no flipping, so the video played
     * upside down. Absent attribute means the convention: bottom-up. */
    {
        INT32 mf_stride = 0;
        if (SUCCEEDED(IMFMediaType_GetUINT32(output_type, &MF_MT_DEFAULT_STRIDE,
                                             (UINT32 *)&mf_stride))) {
            g_vp.bottom_up = (mf_stride < 0);
        } else {
            g_vp.bottom_up = 1;
        }
        (void)mf_stride;   /* the buffer settles it; this is only a default */
    }

    IMFMediaType_Release(output_type);
    output_type = NULL;

    /* Allocate frame buffer */
    g_vp.stride = g_vp.width * 4;
    g_vp.frame_buf_size = g_vp.stride * g_vp.height;
    g_vp.frame_buf = (uint8_t *)calloc(1, g_vp.frame_buf_size);
    if (!g_vp.frame_buf) goto fail;

    /* Create D3D8 texture (power-of-2 for compatibility) */
    g_vp.tex_width  = next_pot(g_vp.width);
    g_vp.tex_height = next_pot(g_vp.height);

    /* LIN_A8R8G8B8, not A8R8G8B8.
     *
     * On Xbox the plain format is SWIZZLED -- only the LIN_ variants are
     * linear (see d3d8_format_is_swizzled). We write plain linear BGRA rows
     * here, so asking for A8R8G8B8 made the d3d8 layer faithfully unswizzle
     * data that was never swizzled, which scrambled every frame into a grid of
     * blocks. */
    hr = d3d8_CreateTextureImpl(g_vp.tex_width, g_vp.tex_height, 1, 0,
                                D3DFMT_LIN_A8R8G8B8, &g_vp.texture);
    if (FAILED(hr)) {
        fprintf(stderr, "  [VIDEO] Failed to create texture %ux%u (hr=0x%08lX)\n",
                g_vp.tex_width, g_vp.tex_height, hr);
        goto fail;
    }

    g_vp.elapsed = 0.0f;
    g_vp.next_frame_at = 0.0f;
    g_vp.finished = 0;
    g_vp.has_frame = 0;

    if (attrs) IMFAttributes_Release(attrs);

    fprintf(stderr, "  [VIDEO] Opened: %s (%dx%d, %.1fs, %.1f fps)\n",
            path, g_vp.width, g_vp.height, g_vp.duration,
            1.0f / g_vp.frame_time);
    return 0;

fail:
    if (native_type) IMFMediaType_Release(native_type);
    if (output_type) IMFMediaType_Release(output_type);
    if (attrs) IMFAttributes_Release(attrs);
    video_close();
    return -1;
}

/* Clamp helper for YUV conversion */
static inline uint8_t clamp_u8(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* Convert NV12 frame to BGRA in frame_buf */
static void convert_nv12_to_bgra(const BYTE *src, DWORD src_len)
{
    int w = g_vp.width;
    int h = g_vp.height;
    /* NV12: Y plane = w*h bytes, UV plane = w*(h/2) bytes, interleaved U,V */
    const BYTE *y_plane = src;
    const BYTE *uv_plane = src + w * h;
    uint32_t *dst = (uint32_t *)g_vp.frame_buf;

    (void)src_len;

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int Y = y_plane[row * w + col];
            int uv_idx = (row / 2) * w + (col & ~1);
            int U = uv_plane[uv_idx]     - 128;
            int V = uv_plane[uv_idx + 1] - 128;

            /* BT.601 YUV→RGB */
            int R = Y + ((351 * V) >> 8);
            int G = Y - ((179 * V + 86 * U) >> 8);
            int B = Y + ((443 * U) >> 8);

            dst[row * w + col] = 0xFF000000u
                | ((uint32_t)clamp_u8(R) << 16)
                | ((uint32_t)clamp_u8(G) << 8)
                | ((uint32_t)clamp_u8(B));
        }
    }
}

/* Convert YUY2 frame to BGRA in frame_buf */
static void convert_yuy2_to_bgra(const BYTE *src, DWORD src_len)
{
    int w = g_vp.width;
    int h = g_vp.height;
    uint32_t *dst = (uint32_t *)g_vp.frame_buf;

    (void)src_len;

    for (int row = 0; row < h; row++) {
        const BYTE *line = src + row * w * 2;
        for (int col = 0; col < w; col += 2) {
            int Y0 = line[col * 2];
            int U  = line[col * 2 + 1] - 128;
            int Y1 = line[col * 2 + 2];
            int V  = line[col * 2 + 3] - 128;

            int R0 = Y0 + ((351 * V) >> 8);
            int G0 = Y0 - ((179 * V + 86 * U) >> 8);
            int B0 = Y0 + ((443 * U) >> 8);
            dst[row * w + col] = 0xFF000000u
                | ((uint32_t)clamp_u8(R0) << 16)
                | ((uint32_t)clamp_u8(G0) << 8)
                | ((uint32_t)clamp_u8(B0));

            int R1 = Y1 + ((351 * V) >> 8);
            int G1 = Y1 - ((179 * V + 86 * U) >> 8);
            int B1 = Y1 + ((443 * U) >> 8);
            dst[row * w + col + 1] = 0xFF000000u
                | ((uint32_t)clamp_u8(R1) << 16)
                | ((uint32_t)clamp_u8(G1) << 8)
                | ((uint32_t)clamp_u8(B1));
        }
    }
}

/* Decode one frame from the source reader into frame_buf (BGRA) */
static int decode_one_frame(void)
{
    if (!g_vp.reader) return -1;

    DWORD flags = 0;
    LONGLONG timestamp = 0;
    IMFSample *sample = NULL;
    IMFMediaBuffer *buffer = NULL;
    BYTE *src = NULL;
    DWORD src_len = 0;

    HRESULT hr = IMFSourceReader_ReadSample(g_vp.reader,
            (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            0, NULL, &flags, &timestamp, &sample);

    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
        g_vp.finished = 1;
        if (sample) IMFSample_Release(sample);
        return -1;
    }

    if (!sample) return 0; /* no sample yet, try again */

    /* Which way up this decoder really hands rows over.
     *
     * RGB32 out of Media Foundation is bottom-up by convention, and assuming
     * that unconditionally played this WMV upside down -- the convention is
     * about the format, not about what a particular decoder produced.
     * MF_MT_DEFAULT_STRIDE is absent on the type here, so ask the buffer: a
     * 2D buffer reports the real pitch, and its sign is the answer. Checked
     * once, since it cannot change mid-stream. */
    if (!g_vp.orientation_known) {
        IMF2DBuffer *b2d = NULL;
        IMFMediaBuffer *mb = NULL;

        g_vp.orientation_known = 1;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &mb)) && mb) {
            if (SUCCEEDED(IMFMediaBuffer_QueryInterface(mb, &IID_IMF2DBuffer,
                                                        (void **)&b2d)) && b2d) {
                BYTE *scan0 = NULL;
                LONG pitch = 0;
                if (SUCCEEDED(IMF2DBuffer_Lock2D(b2d, &scan0, &pitch))) {
                    g_vp.bottom_up = (pitch < 0);
                    IMF2DBuffer_Unlock2D(b2d);
                }
                IMF2DBuffer_Release(b2d);
            }
            IMFMediaBuffer_Release(mb);
        }
        fprintf(stderr, "  [VIDEO] source rows are %s\n",
                g_vp.bottom_up ? "bottom-up" : "top-down");
    }

    hr = IMFSample_ConvertToContiguousBuffer(sample, &buffer);
    if (FAILED(hr)) {
        IMFSample_Release(sample);
        return 0;
    }

    hr = IMFMediaBuffer_Lock(buffer, &src, NULL, &src_len);
    if (SUCCEEDED(hr) && src && src_len > 0) {
        switch (g_vp.format) {
        case VP_FMT_NV12:
            convert_nv12_to_bgra(src, src_len);
            g_vp.has_frame = 1;
            break;

        case VP_FMT_YUY2:
            convert_yuy2_to_bgra(src, src_len);
            g_vp.has_frame = 1;
            break;

        case VP_FMT_RGB32:
        default: {
            /* frame_buf is always top-down; flip only if the source is not. */
            uint32_t copy_h = (src_len / g_vp.stride < (uint32_t)g_vp.height)
                            ? src_len / g_vp.stride
                            : (uint32_t)g_vp.height;
            for (uint32_t y = 0; y < copy_h; y++) {
                uint32_t src_row = g_vp.bottom_up ? (copy_h - 1 - y) : y;
                memcpy(g_vp.frame_buf + y * g_vp.stride,
                       src + src_row * g_vp.stride,
                       g_vp.stride);
            }
            g_vp.has_frame = 1;
            break;
        }
        }
        IMFMediaBuffer_Unlock(buffer);
    }

    IMFMediaBuffer_Release(buffer);
    IMFSample_Release(sample);
    return 1;
}

/* Upload frame_buf to the D3D texture */
static void upload_frame(void)
{
    if (!g_vp.texture || !g_vp.frame_buf || !g_vp.has_frame) return;

    D3DLOCKED_RECT lr;
    HRESULT hr = g_vp.texture->lpVtbl->LockRect(g_vp.texture, 0, &lr, NULL, 0);
    if (SUCCEEDED(hr)) {
        /* Copy frame data, accounting for texture being POT-padded */
        uint8_t *dst = (uint8_t *)lr.pBits;
        for (int y = 0; y < g_vp.height; y++) {
            memcpy(dst + y * lr.Pitch,
                   g_vp.frame_buf + y * g_vp.stride,
                   g_vp.stride);
        }
        g_vp.texture->lpVtbl->UnlockRect(g_vp.texture, 0);
    }
}

int video_update(float dt)
{
    if (!g_vp.reader || g_vp.finished) return -1;

    g_vp.elapsed += dt;

    /* Decode frames until we catch up to elapsed time */
    while (g_vp.elapsed >= g_vp.next_frame_at && !g_vp.finished) {
        int r = decode_one_frame();
        if (r < 0) {
            g_vp.finished = 1;
            return -1;
        }
        g_vp.next_frame_at += g_vp.frame_time;
    }

    if (g_vp.has_frame) {
        upload_frame();
        return 1;
    }
    return 0;
}

void video_render(void)
{
    if (!g_vp.texture || !g_vp.has_frame) return;

    IDirect3DDevice8 *dev = d3d8_GetDevice();
    if (!dev) return;

    /* UV coords account for POT texture padding */
    float u_max = (float)g_vp.width  / (float)g_vp.tex_width;
    float v_max = (float)g_vp.height / (float)g_vp.tex_height;
    float sw = (float)d3d8_GetBackbufferWidth();
    float sh = (float)d3d8_GetBackbufferHeight();

    /* Transformed + textured vertex (XYZRHW + UV) */
    struct { float x, y, z, rhw; float u, v; } quad[4] = {
        {  -0.5f,      -0.5f, 0.0f, 1.0f, 0.0f,  0.0f  },
        { sw - 0.5f,   -0.5f, 0.0f, 1.0f, u_max, 0.0f  },
        {  -0.5f,  sh - 0.5f, 0.0f, 1.0f, 0.0f,  v_max },
        { sw - 0.5f, sh-0.5f, 0.0f, 1.0f, u_max, v_max },
    };

    /* Set render state for fullscreen quad */
    dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture8 *)g_vp.texture);
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    /* Use the enum names, not hand-written numbers. COLOROP was passed a
     * literal 3 labelled SELECTARG1, but D3DTOP_SELECTARG1 is 2 and 3 is
     * D3DTOP_SELECTARG2. The stage therefore selected ARG2, which defaults to
     * D3DTA_CURRENT, and on stage 0 with no diffuse in the vertex format that
     * is opaque white. The video decoded, converted and uploaded perfectly,
     * and every frame then drew a white rectangle over it. */
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->lpVtbl->SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    dev->lpVtbl->SetTexture(dev, 0, NULL);
}

int video_is_finished(void)
{
    return g_vp.finished;
}

void video_close(void)
{
    if (g_vp.reader) {
        IMFSourceReader_Release(g_vp.reader);
        g_vp.reader = NULL;
    }
    if (g_vp.texture) {
        g_vp.texture->lpVtbl->Release(g_vp.texture);
        g_vp.texture = NULL;
    }
    if (g_vp.frame_buf) {
        free(g_vp.frame_buf);
        g_vp.frame_buf = NULL;
    }
    g_vp.has_frame = 0;
    g_vp.finished = 0;
    g_vp.elapsed = 0.0f;
    g_vp.width = 0;
    g_vp.height = 0;
}

/* Write the frame currently decoded to an image (img_dump.h).
 *
 * The point is evidence rather than debugging: "the video played" is otherwise
 * only a log line, and a log line cannot distinguish a decoded frame that got
 * drawn from a black screen that ran for the right number of seconds. This
 * writes what was uploaded to the texture, so a non-black file means real
 * decoded pixels reached the renderer. */
int video_dump_frame_bmp(const char *path)
{
    uint32_t w = g_vp.width, h = g_vp.height;

    if (!g_vp.has_frame || !g_vp.frame_buf || !w || !h)
        return -1;
    /* BGRA rows, the alpha dropped. */
    if (!img_dump_write(path, g_vp.frame_buf, g_vp.stride, w, h, 4, 0))
        return -1;
    fprintf(stderr, "  [VIDEO] wrote %s (%ux%u)\n", path, w, h);
    return 0;
}

#else  /* !_WIN32 -- Media Foundation is the decoder, so there is none here. */

int  video_init(void)            { return -1; }
void video_shutdown(void)        {}
int  video_open(const char *p)   { (void)p; return -1; }
int  video_update(float dt)      { (void)dt; return -1; }
void video_render(void)          {}
int  video_is_finished(void)     { return 1; }
void video_close(void)           {}

#endif /* _WIN32 */
