/**
 * Xbox DirectSound → XAudio2 Compatibility Layer
 *
 * Implements the Xbox DirectSound interfaces. Currently provides
 * stub sound buffer objects that accept all method calls silently.
 * This allows the game to initialize audio and create buffers without
 * crashing, while actual audio playback is deferred to a later phase.
 *
 * Architecture:
 * - XAudio2 mastering voice for output (future)
 * - Each DirectSound buffer maps to an XAudio2 source voice (future)
 * - 3D audio emulated using X3DAudio (future)
 */

#include "dsound_xbox.h"
#include "../apu/apu.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ================================================================
 * Sound buffer stub implementation
 *
 * Returns a valid buffer object for every CreateSoundBuffer call.
 * All audio methods are no-ops that return S_OK.
 * ================================================================ */

typedef struct DSoundBuffer {
    IDirectSoundBuffer8 iface;
    LONG    ref_count;
    void   *buffer_data;
    DWORD   buffer_size;
    DWORD   play_cursor;
    DWORD   status;  /* DSBSTATUS_* flags */
    LONG    volume;  /* hundredths of dB */
    DWORD   frequency;
    WORD    channels;     /* 1=mono, 2=stereo */
    WORD    bits_per_sample;
    int     mixer_slot;   /* APU mixer voice slot, -1 = none */
} DSoundBuffer;

static DSoundBuffer *buf_from_iface(IDirectSoundBuffer8 *iface)
{
    return (DSoundBuffer *)iface;
}

static HRESULT __stdcall buf_QueryInterface(IDirectSoundBuffer8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall buf_AddRef(IDirectSoundBuffer8 *self)
{
    return (ULONG)InterlockedIncrement(&buf_from_iface(self)->ref_count);
}

static ULONG __stdcall buf_Release(IDirectSoundBuffer8 *self)
{
    DSoundBuffer *buf = buf_from_iface(self);
    LONG ref = InterlockedDecrement(&buf->ref_count);
    if (ref <= 0) {
        if (buf->mixer_slot >= 0) {
            apu_mixer_stop(buf->mixer_slot);
            apu_mixer_free_voice(buf->mixer_slot);
        }
        free(buf->buffer_data);
        free(buf);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall buf_SetBufferData(IDirectSoundBuffer8 *self, const void *pvBufferData, DWORD dwBufferBytes)
{
    DSoundBuffer *buf = buf_from_iface(self);
    free(buf->buffer_data);
    buf->buffer_data = NULL;
    buf->buffer_size = dwBufferBytes;
    if (pvBufferData && dwBufferBytes > 0) {
        buf->buffer_data = malloc(dwBufferBytes);
        if (buf->buffer_data)
            memcpy(buf->buffer_data, pvBufferData, dwBufferBytes);
    }

    /* Update mixer voice with new buffer data */
    if (buf->mixer_slot >= 0 && buf->buffer_data && buf->bits_per_sample == 16) {
        APUMixerVoice *v = apu_mixer_get_voice(buf->mixer_slot);
        if (v) {
            v->pcm_data = (const int16_t *)buf->buffer_data;
            v->pcm_bytes = dwBufferBytes;
            v->num_channels = buf->channels;
            v->sample_rate = buf->frequency;
        }
    }
    return S_OK;
}

static HRESULT __stdcall buf_SetPlayRegion(IDirectSoundBuffer8 *self, DWORD dwPlayStart, DWORD dwPlayLength)
{
    (void)self; (void)dwPlayStart; (void)dwPlayLength;
    return S_OK;
}

static HRESULT __stdcall buf_Lock(IDirectSoundBuffer8 *self, DWORD dwOffset, DWORD dwBytes,
    void **ppvAudioPtr1, DWORD *pdwAudioBytes1, void **ppvAudioPtr2, DWORD *pdwAudioBytes2, DWORD dwFlags)
{
    DSoundBuffer *buf = buf_from_iface(self);
    (void)dwFlags;

    if (!buf->buffer_data || buf->buffer_size == 0) {
        if (ppvAudioPtr1) *ppvAudioPtr1 = NULL;
        if (pdwAudioBytes1) *pdwAudioBytes1 = 0;
        if (ppvAudioPtr2) *ppvAudioPtr2 = NULL;
        if (pdwAudioBytes2) *pdwAudioBytes2 = 0;
        return E_FAIL;
    }

    /* Simple linear lock (no wraparound) */
    if (dwOffset >= buf->buffer_size) dwOffset = 0;
    if (dwBytes == 0 || dwOffset + dwBytes > buf->buffer_size)
        dwBytes = buf->buffer_size - dwOffset;

    if (ppvAudioPtr1) *ppvAudioPtr1 = (BYTE *)buf->buffer_data + dwOffset;
    if (pdwAudioBytes1) *pdwAudioBytes1 = dwBytes;
    if (ppvAudioPtr2) *ppvAudioPtr2 = NULL;
    if (pdwAudioBytes2) *pdwAudioBytes2 = 0;
    return S_OK;
}

static HRESULT __stdcall buf_Unlock(IDirectSoundBuffer8 *self, void *p1, DWORD n1, void *p2, DWORD n2)
{
    (void)self; (void)p1; (void)n1; (void)p2; (void)n2;
    return S_OK;
}

static HRESULT __stdcall buf_SetCurrentPosition(IDirectSoundBuffer8 *self, DWORD dwNewPosition)
{
    buf_from_iface(self)->play_cursor = dwNewPosition;
    return S_OK;
}

static HRESULT __stdcall buf_GetCurrentPosition(IDirectSoundBuffer8 *self, DWORD *pdwPlay, DWORD *pdwWrite)
{
    DSoundBuffer *buf = buf_from_iface(self);
    if (pdwPlay)  *pdwPlay = buf->play_cursor;
    if (pdwWrite) *pdwWrite = buf->play_cursor;
    return S_OK;
}

static HRESULT __stdcall buf_GetStatus(IDirectSoundBuffer8 *self, DWORD *pdwStatus)
{
    if (pdwStatus) *pdwStatus = buf_from_iface(self)->status;
    return S_OK;
}

static HRESULT __stdcall buf_Play(IDirectSoundBuffer8 *self, DWORD r1, DWORD r2, DWORD dwFlags)
{
    (void)r1; (void)r2;
    DSoundBuffer *buf = buf_from_iface(self);
    buf->status = DSBSTATUS_PLAYING;
    if (dwFlags & DSBPLAY_LOOPING) buf->status |= DSBSTATUS_LOOPING;

    /* Start playback through APU mixer */
    if (buf->mixer_slot >= 0) {
        apu_mixer_play(buf->mixer_slot, (dwFlags & DSBPLAY_LOOPING) ? 1 : 0);
    }
    return S_OK;
}

static HRESULT __stdcall buf_Stop(IDirectSoundBuffer8 *self)
{
    DSoundBuffer *buf = buf_from_iface(self);
    buf->status = 0;
    if (buf->mixer_slot >= 0) {
        apu_mixer_stop(buf->mixer_slot);
    }
    return S_OK;
}

static HRESULT __stdcall buf_SetVolume(IDirectSoundBuffer8 *self, LONG lVolume)
{
    DSoundBuffer *buf = buf_from_iface(self);
    buf->volume = lVolume;

    /* Convert DirectSound volume (hundredths of dB, 0=max, -9600=min) to linear */
    if (buf->mixer_slot >= 0) {
        APUMixerVoice *v = apu_mixer_get_voice(buf->mixer_slot);
        if (v) {
            if (lVolume <= -9600) {
                v->volume = 0.0f;
            } else if (lVolume >= 0) {
                v->volume = 1.0f;
            } else {
                /* dB to linear: 10^(dB/2000) since lVolume is in hundredths of dB */
                v->volume = powf(10.0f, lVolume / 2000.0f);
            }
        }
    }
    return S_OK;
}

static HRESULT __stdcall buf_SetFrequency(IDirectSoundBuffer8 *self, DWORD dwFrequency)
{
    DSoundBuffer *buf = buf_from_iface(self);
    buf->frequency = dwFrequency;

    if (buf->mixer_slot >= 0) {
        APUMixerVoice *v = apu_mixer_get_voice(buf->mixer_slot);
        if (v) v->sample_rate = dwFrequency;
    }
    return S_OK;
}

static HRESULT __stdcall buf_SetMaxDistance(IDirectSoundBuffer8 *self, float f, DWORD dw) { (void)self; (void)f; (void)dw; return S_OK; }
static HRESULT __stdcall buf_SetMinDistance(IDirectSoundBuffer8 *self, float f, DWORD dw) { (void)self; (void)f; (void)dw; return S_OK; }
static HRESULT __stdcall buf_SetPosition(IDirectSoundBuffer8 *self, float x, float y, float z, DWORD dw) { (void)self; (void)x; (void)y; (void)z; (void)dw; return S_OK; }
static HRESULT __stdcall buf_SetVelocity(IDirectSoundBuffer8 *self, float x, float y, float z, DWORD dw) { (void)self; (void)x; (void)y; (void)z; (void)dw; return S_OK; }
static HRESULT __stdcall buf_SetConeAngles(IDirectSoundBuffer8 *self, DWORD in, DWORD out, DWORD dw) { (void)self; (void)in; (void)out; (void)dw; return S_OK; }
static HRESULT __stdcall buf_SetConeOutsideVolume(IDirectSoundBuffer8 *self, LONG vol, DWORD dw) { (void)self; (void)vol; (void)dw; return S_OK; }
static HRESULT __stdcall buf_SetMixBins(IDirectSoundBuffer8 *self, const DSMIXBINS *pMixBins) { (void)self; (void)pMixBins; return S_OK; }

static const IDirectSoundBuffer8Vtbl g_buf_vtbl = {
    buf_QueryInterface, buf_AddRef, buf_Release,
    buf_SetBufferData, buf_SetPlayRegion,
    buf_Lock, buf_Unlock,
    buf_SetCurrentPosition, buf_GetCurrentPosition,
    buf_GetStatus,
    buf_Play, buf_Stop,
    buf_SetVolume, buf_SetFrequency,
    buf_SetMaxDistance, buf_SetMinDistance,
    buf_SetPosition, buf_SetVelocity,
    buf_SetConeAngles, buf_SetConeOutsideVolume,
    buf_SetMixBins,
};

/* ================================================================
 * IDirectSound8 implementation
 * ================================================================ */

static HRESULT __stdcall ds_QueryInterface(IDirectSound8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall ds_AddRef(IDirectSound8 *self)  { (void)self; return 1; }
static ULONG __stdcall ds_Release(IDirectSound8 *self) { (void)self; return 0; }

static HRESULT __stdcall ds_CreateSoundBuffer(IDirectSound8 *self, const DSBUFFERDESC *desc, IDirectSoundBuffer8 **ppBuffer, void *pUnkOuter)
{
    DSoundBuffer *buf;
    (void)self; (void)pUnkOuter;

    if (!ppBuffer) return E_INVALIDARG;

    buf = (DSoundBuffer *)calloc(1, sizeof(*buf));
    if (!buf) return E_OUTOFMEMORY;

    buf->iface.lpVtbl = &g_buf_vtbl;
    buf->ref_count = 1;
    buf->volume = 0;         /* Full volume (0 dB) */
    buf->frequency = 44100;  /* Default sample rate */
    buf->channels = 2;
    buf->bits_per_sample = 16;

    /* Extract format from WAVEFORMATEX if provided */
    if (desc && desc->lpwfxFormat) {
        XBOX_WAVEFORMATEX *wfx = (XBOX_WAVEFORMATEX *)desc->lpwfxFormat;
        buf->channels = wfx->nChannels;
        buf->frequency = wfx->nSamplesPerSec;
        buf->bits_per_sample = wfx->wBitsPerSample;
    }

    /* Allocate a mixer voice slot */
    buf->mixer_slot = apu_mixer_alloc_voice();
    if (buf->mixer_slot >= 0) {
        APUMixerVoice *v = apu_mixer_get_voice(buf->mixer_slot);
        if (v) {
            v->num_channels = buf->channels;
            v->sample_rate = buf->frequency;
        }
    }

    /* Pre-allocate buffer if size specified */
    if (desc && desc->dwBufferBytes > 0) {
        buf->buffer_data = calloc(1, desc->dwBufferBytes);
        buf->buffer_size = desc->dwBufferBytes;
    }

    static int create_log_count = 0;
    if (create_log_count < 30) {
        fprintf(stderr, "[DSOUND] CreateSoundBuffer: %u bytes, %d ch, %u Hz, %d-bit → mixer slot %d\n",
                desc ? desc->dwBufferBytes : 0, buf->channels, buf->frequency,
                buf->bits_per_sample, buf->mixer_slot);
        create_log_count++;
    }

    *ppBuffer = &buf->iface;
    return S_OK;
}

static HRESULT __stdcall ds_CreateSoundStream(IDirectSound8 *self, const DSSTREAMDESC *desc, IDirectSoundStream **ppStream, void *pUnkOuter)
{
    (void)self; (void)desc; (void)pUnkOuter;
    /* Streams are more complex - stub for now */
    if (ppStream) *ppStream = NULL;
    return S_OK;
}

static HRESULT __stdcall ds_SetMixBinHeadroom(IDirectSound8 *self, DWORD dwMixBin, DWORD dwHeadroom)
{
    (void)self; (void)dwMixBin; (void)dwHeadroom;
    return S_OK;
}

static HRESULT __stdcall ds_SetPosition(IDirectSound8 *self, float x, float y, float z, DWORD dwApply)
{
    (void)self; (void)x; (void)y; (void)z; (void)dwApply;
    return S_OK;
}

static HRESULT __stdcall ds_SetVelocity(IDirectSound8 *self, float x, float y, float z, DWORD dwApply)
{
    (void)self; (void)x; (void)y; (void)z; (void)dwApply;
    return S_OK;
}

static HRESULT __stdcall ds_SetDistanceFactor(IDirectSound8 *self, float f, DWORD dwApply)
{
    (void)self; (void)f; (void)dwApply;
    return S_OK;
}

static HRESULT __stdcall ds_SetRolloffFactor(IDirectSound8 *self, float f, DWORD dwApply)
{
    (void)self; (void)f; (void)dwApply;
    return S_OK;
}

static HRESULT __stdcall ds_SetDopplerFactor(IDirectSound8 *self, float f, DWORD dwApply)
{
    (void)self; (void)f; (void)dwApply;
    return S_OK;
}

static HRESULT __stdcall ds_SetOrientation(IDirectSound8 *self, float xf, float yf, float zf, float xt, float yt, float zt, DWORD dwApply)
{
    (void)self; (void)xf; (void)yf; (void)zf; (void)xt; (void)yt; (void)zt; (void)dwApply;
    return S_OK;
}

static HRESULT __stdcall ds_CommitDeferredSettings(IDirectSound8 *self)
{
    (void)self;
    return S_OK;
}

static const IDirectSound8Vtbl g_ds_vtbl = {
    ds_QueryInterface,
    ds_AddRef,
    ds_Release,
    ds_CreateSoundBuffer,
    ds_CreateSoundStream,
    ds_SetMixBinHeadroom,
    ds_SetPosition,
    ds_SetVelocity,
    ds_SetDistanceFactor,
    ds_SetRolloffFactor,
    ds_SetDopplerFactor,
    ds_SetOrientation,
    ds_CommitDeferredSettings,
};

static IDirectSound8 g_dsound = { &g_ds_vtbl };

HRESULT xbox_DirectSoundCreate(void *pGuid, IDirectSound8 **ppDS, void *pUnkOuter)
{
    (void)pGuid; (void)pUnkOuter;
    if (!ppDS) return E_INVALIDARG;
    *ppDS = &g_dsound;
    fprintf(stderr, "Audio: DirectSound created (stub - no audio output)\n");
    return S_OK;
}
