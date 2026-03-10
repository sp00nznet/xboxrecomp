# xbox_dsound — DirectSound Compatibility Layer

Implements the Xbox's IDirectSound8 and IDirectSoundBuffer8 COM interfaces backed by the APU software mixer. Games that use DirectSound for audio will call these functions.

## Files

| File | LOC | Purpose |
|------|-----|---------|
| `dsound_xbox.h` | 189 | Public header — all types, structs, COM interfaces |
| `dsound_device.c` | 384 | IDirectSound8 + IDirectSoundBuffer8 implementation |

## Quick Start

```c
#include "dsound_xbox.h"

// Create DirectSound interface
IDirectSound8 *ds;
xbox_DirectSoundCreate(NULL, &ds, NULL);

// Create a sound buffer
DSBUFFERDESC desc = {
    .dwSize = sizeof(DSBUFFERDESC),
    .dwFlags = DSBCAPS_CTRLVOLUME,
    .dwBufferBytes = 44100 * 2 * 2,  // 1 second, 16-bit stereo
};
IDirectSoundBuffer8 *buf;
ds->lpVtbl->CreateSoundBuffer(ds, &desc, &buf, NULL);

// Fill buffer with PCM data
void *ptr1;
DWORD bytes1;
buf->lpVtbl->Lock(buf, 0, desc.dwBufferBytes, &ptr1, &bytes1, NULL, NULL, 0);
memcpy(ptr1, my_pcm_data, bytes1);
buf->lpVtbl->Unlock(buf, ptr1, bytes1, NULL, 0);

// Play (looping)
buf->lpVtbl->Play(buf, 0, 0, DSBPLAY_LOOPING);

// Adjust volume (in 100ths of dB, 0 = full, -10000 = silence)
buf->lpVtbl->SetVolume(buf, -2000);  // -20 dB

// Stop
buf->lpVtbl->Stop(buf);
```

## How It Works

Each `IDirectSoundBuffer8` is backed by an APU mixer voice slot:

```
Game calls CreateSoundBuffer()
  → Allocates APU mixer voice (apu_mixer_alloc_voice)
  → Stores voice handle in buffer struct

Game calls SetBufferData() or Lock()/Unlock()
  → Copies PCM data to mixer voice buffer

Game calls Play()
  → apu_mixer_play(voice, looping)
  → Voice is mixed into waveOut output at 48kHz
```

## IDirectSound8 Methods

```c
// Buffer creation
CreateSoundBuffer(desc, &buffer, unused)      // Standard sound buffer
CreateSoundStream(desc, &buffer, unused)       // Streaming buffer (same impl)

// 3D listener control
SetPosition(x, y, z, apply)                   // Listener position
SetVelocity(x, y, z, apply)                   // Listener velocity
SetOrientation(xF, yF, zF, xT, yT, zT, apply) // Listener orientation
SetDistanceFactor(factor, apply)               // Distance attenuation scale
SetRolloffFactor(factor, apply)                // Rolloff curve scale
SetDopplerFactor(factor, apply)                // Doppler effect scale

// Mixbin control
SetMixBinHeadroom(dwMixBinMask, dwHeadroom)   // Per-channel headroom

// Commit deferred changes
CommitDeferredSettings()                        // Apply 3D audio changes
```

## IDirectSoundBuffer8 Methods

```c
// Data
SetBufferData(pvBufferData, dwBufferBytes)     // Point to PCM data
SetPlayRegion(dwPlayStart, dwPlayLength)        // Set play region

// Lock/Unlock (circular buffer access)
Lock(dwOffset, dwBytes, &ptr1, &bytes1, &ptr2, &bytes2, dwFlags)
Unlock(ptr1, bytes1, ptr2, bytes2)

// Playback
Play(reserved1, reserved2, dwFlags)            // Start (DSBPLAY_LOOPING for loop)
Stop()                                          // Stop playback
SetCurrentPosition(dwNewPosition)               // Seek
GetCurrentPosition(&dwPlayCursor, &dwWriteCursor)
GetStatus(&dwStatus)                            // DSBSTATUS_PLAYING, etc.

// Volume & frequency
SetVolume(lVolume)                              // In 100ths of dB (0 to -10000)
SetFrequency(dwFrequency)                       // Playback rate in Hz

// 3D sound (per-buffer)
SetMinDistance(flMinDistance, dwApply)
SetMaxDistance(flMaxDistance, dwApply)
SetPosition(x, y, z, dwApply)
SetVelocity(x, y, z, dwApply)
SetConeAngles(dwInside, dwOutside, dwApply)
SetConeOutsideVolume(lVolume, dwApply)

// Mixbins
SetMixBins(pMixBins)                            // Route to output channels
```

## Xbox DirectSound vs PC DirectSound

Key differences:

1. **No cooperative level** — Xbox doesn't need `SetCooperativeLevel()`.
2. **SetBufferData()** — Xbox extension to point a buffer at existing PCM data without copying.
3. **SetPlayRegion()** — Xbox extension to play a sub-region of the buffer.
4. **Mixbins** — Xbox routes audio to specific output channels (front L/R, rear L/R, center, sub, etc.) instead of using speaker configurations.
5. **No DSBCAPS_PRIMARYBUFFER** — Xbox has no primary buffer concept.
6. **Buffer format** — Xbox uses `XBOX_WAVEFORMATEX` which is identical to Windows `WAVEFORMATEX` but defined separately.

## Types

```c
// Buffer descriptor
typedef struct {
    DWORD dwSize;              // sizeof(DSBUFFERDESC)
    DWORD dwFlags;             // DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME | ...
    DWORD dwBufferBytes;       // Buffer size in bytes
    DWORD dwMixBinMask;        // Xbox mixbin routing mask
    DWORD dwInputMixBin;       // Input mixbin
} DSBUFFERDESC;

// Mixbin routing
typedef struct {
    DWORD dwMixBinCount;       // Number of active mixbins
    DWORD *lpMixBinVolumePairs; // Volume per mixbin
} DSMIXBINS;

// 3D buffer parameters
typedef struct {
    DWORD  dwSize;
    D3DVECTOR vPosition;       // 3D position
    D3DVECTOR vVelocity;       // 3D velocity
    DWORD  dwInsideConeAngle;
    DWORD  dwOutsideConeAngle;
    D3DVECTOR vConeOrientation;
    LONG   lConeOutsideVolume;
    FLOAT  flMinDistance;      // Distance where attenuation starts
    FLOAT  flMaxDistance;      // Distance where attenuation stops
    DWORD  dwMode;             // DS3DMODE_NORMAL, _HEADRELATIVE, _DISABLE
} DS3DBUFFER;

// Audio format
typedef struct {
    WORD  wFormatTag;          // 1 = PCM
    WORD  nChannels;           // 1 = mono, 2 = stereo
    DWORD nSamplesPerSec;      // 22050, 44100, 48000, etc.
    DWORD nAvgBytesPerSec;     // nSamplesPerSec * nBlockAlign
    WORD  nBlockAlign;         // nChannels * wBitsPerSample / 8
    WORD  wBitsPerSample;      // 8 or 16
    WORD  cbSize;              // Extra data size (0 for PCM)
} XBOX_WAVEFORMATEX;
```

## Dependency

Links to `xbox_apu` for the software mixer backend.
