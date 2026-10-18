/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"
#include "recomp_env.h"

/* The XAudio2 backend is Windows-only. On POSIX hosts apu_sdl2.c provides
 * the same xa2_* functions over SDL2. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
/* Device buffer size and count. Defaults 512 x 4 (10.7 ms each, 43 ms
 * worst case queued); RECOMP_AUDIO_BUF_SAMPLES and RECOMP_AUDIO_BUF_COUNT
 * override them at xa2_init, clamped to the static storage below. */
#define XA2_BUF_SAMPLES_DEFAULT 512
#define XA2_NUM_BUFS_DEFAULT    4
#define XA2_BUF_SAMPLES_MIN     128
#define XA2_BUF_SAMPLES_MAX     4096
#define XA2_NUM_BUFS_MIN        2
#define XA2_NUM_BUFS_MAX        8

static int g_xa2_buf_samples = XA2_BUF_SAMPLES_DEFAULT;
static int g_xa2_num_bufs    = XA2_NUM_BUFS_DEFAULT;
#define XA2_BUF_SAMPLES g_xa2_buf_samples
#define XA2_NUM_BUFS    g_xa2_num_bufs

static int xa2_env_int(recomp_env_id id, int def, int lo, int hi)
{
    const char *e = recomp_env(id);
    int v;
    if (!e || !*e) return def;
    v = atoi(e);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
/* One slot more than can be queued: with every buffer on the voice, the
 * next one is staged in a slot the voice does not hold. With only
 * XA2_NUM_BUFS slots the staging buffer was the oldest queued one, and
 * filling it overwrote audio the device had not played yet. */
#define XA2_RING (XA2_NUM_BUFS + 1)
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS_MAX + 1][XA2_BUF_SAMPLES_MAX][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_fill = 0;  /* samples staged in bufs[next_buf] */
static unsigned  g_xa2_underruns;
static int       g_xa2_dry;
static ULONGLONG g_xa2_underrun_logged;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    g_xa2_buf_samples = xa2_env_int(RENV_AUDIO_BUF_SAMPLES,
                                    XA2_BUF_SAMPLES_DEFAULT,
                                    XA2_BUF_SAMPLES_MIN, XA2_BUF_SAMPLES_MAX);
    g_xa2_num_bufs = xa2_env_int(RENV_AUDIO_BUF_COUNT, XA2_NUM_BUFS_DEFAULT,
                                 XA2_NUM_BUFS_MIN, XA2_NUM_BUFS_MAX);

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_fill = 0;
    g_xa2_dry = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers, %.1f ms queued max)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES,
            1000.0 * XA2_NUM_BUFS * XA2_BUF_SAMPLES / XA2_SAMPLE_RATE);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Hand mixed samples to XAudio2. Called from the APU frame thread with one
 * EP period (256 samples) at a time. Samples accumulate in the current device
 * buffer, which is submitted when full, so a call shorter than a buffer is
 * kept, not padded or dropped. Returns 1 if every sample was accepted; 0 if
 * the voice already had every buffer queued when one filled, in which case
 * that buffer is discarded (pacing by queue depth is what prevents it). */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx, n, ok = 1;
    HRESULT hr;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    while (num_samples > 0) {
        idx = g_xa2_next_buf;
        n = XA2_BUF_SAMPLES - g_xa2_fill;
        if (n > num_samples) n = num_samples;
        memcpy(g_xa2_bufs[idx][g_xa2_fill], samples,
               n * XA2_CHANNELS * sizeof(int16_t));
        g_xa2_fill += n;
        samples += n * XA2_CHANNELS;
        num_samples -= n;
        if (g_xa2_fill < XA2_BUF_SAMPLES)
            break;

        g_xa2_fill = 0;
        IXAudio2SourceVoice_GetState(g_xa2_source, &state,
                                     XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if ((int)state.BuffersQueued >= XA2_NUM_BUFS) { ok = 0; continue; }

        memset(&xbuf, 0, sizeof(xbuf));
        xbuf.AudioBytes = XA2_BUF_SAMPLES * XA2_CHANNELS * sizeof(int16_t);
        xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];
        hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
        if (FAILED(hr)) { ok = 0; continue; }

        g_xa2_next_buf = (idx + 1) % XA2_RING;
        g_xa2_frames_written++;
    }
    return ok;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

int xa2_get_buffer_count(void)
{
    return XA2_NUM_BUFS;
}

/* Samples handed over and not yet played: the buffers the voice still
 * holds plus what is staged in the next one. Also the underrun detector:
 * the voice running dry after audio has started is one underrun, counted
 * once per dry spell and reported at most once a second. A drain announced
 * by xa2_expect_drain (the frame thread pausing) is not one. */

int xa2_queued_samples(void)
{
    XAUDIO2_VOICE_STATE state;

    if (!g_xa2_initialized || !g_xa2_source) return 0;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state,
                                 XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued == 0 && g_xa2_frames_written > 0) {
        if (!g_xa2_dry) {
            ULONGLONG now = GetTickCount64();
            g_xa2_dry = 1;
            g_xa2_underruns++;
            if (now - g_xa2_underrun_logged >= 1000) {
                g_xa2_underrun_logged = now;
                fprintf(stderr, "[XA2] underrun x%u\n", g_xa2_underruns);
            }
        }
    } else {
        g_xa2_dry = 0;
    }
    return (int)state.BuffersQueued * XA2_BUF_SAMPLES + g_xa2_fill;
}

/* Mark the coming dry spell as already seen; the first call that finds
 * buffers queued again clears it, so later dry spells count as before. */
void xa2_expect_drain(void)
{
    g_xa2_dry = 1;
}

unsigned xa2_underruns(void)
{
    return g_xa2_underruns;
}

uint64_t xa2_samples_played(void)
{
    XAUDIO2_VOICE_STATE state;

    if (!g_xa2_initialized || !g_xa2_source) return 0;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state, 0);
    return state.SamplesPlayed;
}

#else /* !_WIN32: apu_sdl2.c implements the same contract over SDL2 */

/* ISO C wants at least one declaration in a translation unit. */
typedef int apu_xaudio2_posix_unused;

#endif /* _WIN32 */
