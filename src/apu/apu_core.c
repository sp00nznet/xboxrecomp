/*
 * MCPX APU Core - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_state.h"
#include "apu.h"
#include "apu_xaudio2.h"
#include "apu_wav.h"
#include "fpconv.h"
#include "../kernel/xbox_memory_layout.h"   /* XBOX_WORKER_STACK_TOP */

/* ============================================================
 * Globals
 * ============================================================ */

uint8_t *g_apu_ram_ptr = NULL;

MCPXAPUState *g_state = NULL;

/* Forward declarations for software mixer */
static void mixer_init(void);
static void mixer_render(int16_t frame_buf[][2], int num_samples);
static APUMixerVoice g_mixer_voices[APU_MIXER_MAX_VOICES];
static volatile int g_mixer_active_count = 0;
static CRITICAL_SECTION g_mixer_cs;
static bool g_mixer_initialized = false;
struct McpxApuDebug g_dbg;
struct McpxApuDebug g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4] = { 0 };

/* Global audio mute — disables all AWD/mixer sound playback */
volatile int g_audio_muted = 0;  /* 0 = audio enabled */

/* ============================================================
 * Debug frame markers (minimal stubs)
 * ============================================================ */

void mcpx_debug_begin_frame(void) {}
void mcpx_debug_end_frame(void) {}

/* ============================================================
 * IRQ handling (stubbed - no PCI bus in standalone)
 * ============================================================ */

/* Physical addresses, resolved much as the other bus masters here do it
 * (dma_resolve in nv2a_pb_exec.c, bus_resolve in usb/ohci.c), but in an
 * order of the APU's own.
 *
 * DirectSound builds its voice, SGE and notifier structures in
 * MmAllocateContiguousMemory and hands the APU their physical addresses.
 * Physical P and the contiguous window's 0x80000000 + P are the same bytes on
 * hardware; here the window is separate storage. Reading every address as low
 * RAM meant the voice processor walked zeroes and wrote each "voice done"
 * notification into ordinary RAM, where DirectSound never looked -- so a
 * buffer never reported that it had stopped, and a frontend that waits on
 * IDirectSoundBuffer::GetStatus polled forever.
 *
 * Because the window is separate storage, one physical number below 64 MB can
 * name two different pages: a window page the contiguous allocator handed out
 * and a RAM page of the image or the heap. The cases below decide which one
 * the title meant. */
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;
extern uint32_t xbox_ContiguousAllocatedBytes(void);
extern size_t g_xbox_total_ram;
extern size_t g_xbox_map_size;     /* host mapping; 0 means same as RAM */

/* How far up the host actually mapped guest RAM: the map size when it was
 * set apart from the RAM size (xbox_SetMapSize), else the RAM size. */
static inline uint32_t apu_mapped_ram(void)
{
    return (uint32_t)(g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram);
}

uint8_t *mcpx_apu_phys(uint64_t addr)
{
    uint32_t a = (uint32_t)addr & 0x0FFFFFFFu;
    /* RAM above the window's 64 MB. On a 128 MB layout (a devkit) most of
     * the heap sits above 0x04000000, and so do the sound buffers a title
     * malloc()s and hands to DirectSound as plain pointers, which
     * MmGetPhysicalAddress turns into physical addresses (the identity for
     * RAM). Folding those into the low 64 MB, as the fallback below does,
     * read code as PCM: full-scale white noise on every effect, and music
     * that lost its upper half each time its ring crossed the boundary.
     * There is no window up here, so it is RAM as far as the host mapped it;
     * only above that does the address wrap. */
    if (a >= (64u << 20)) {
        if (a < apu_mapped_ram())
            return g_apu_ram_ptr + a;
        return g_apu_ram_ptr + (a & 0x03FFFFFFu);
    }
    /* A heap page the title named through MmGetPhysicalAddress is RAM, even
     * where the contiguous allocator has handed out the same number as a
     * window page. A title that loads its effect banks and music stream into
     * the heap puts those physical numbers in the voices' SGE lists; read
     * from the window, effects played whatever the window held there (white
     * noise) and the stream played zeros. Only heap pages are recorded, so
     * the voice-array case below still holds for the image. */
    if (xbox_PhysRamNameMap()[a >> 12])
        return g_apu_ram_ptr + a;
    /* A page the contiguous allocator handed out is the window, even where
     * it overlaps the image's physical range: an image whose BSS runs past
     * 48 MB can have DirectSound's voice array at a window address inside
     * that range. Resolved to RAM, the VP never saw the driver take an idle
     * voice off its list and re-trapped it every frame. (The OHCI resolves
     * the image first: a USB driver can read its descriptors into .data on a
     * page the boot has also allocated as contiguous memory.) */
    if (xbox_ContiguousPageMap()[a >> 12])
        return g_apu_ram_ptr + 0x80000000u + a;
    if (a >= g_xbox_image_lo && a < g_xbox_image_hi)
        return g_apu_ram_ptr + a;
    if (a < xbox_ContiguousAllocatedBytes())
        return g_apu_ram_ptr + 0x80000000u + a;
    return g_apu_ram_ptr + (a & 0x03FFFFFFu);
}

/* Which of the cases above resolved an address, for the RECOMP_APU_TRACE
 * per-voice line and the doorbell line: "named" (a heap page
 * MmGetPhysicalAddress named), "win" (a page the contiguous allocator handed
 * out), "img" (inside the XBE image), "hiwin" (below the arena's high-water
 * mark but not a live page), "ram" (plain RAM: below 64 MB, or above it
 * where the host mapped RAM) or "wrap" (above the mapping, folded into the
 * low 64 MB). A voice reading noise from "wrap" or "hiwin" is the address
 * resolution, not the decoder. */
const char *mcpx_apu_phys_class(uint64_t addr)
{
    uint32_t a = (uint32_t)addr & 0x0FFFFFFFu;
    if (a >= (64u << 20))
        return a < apu_mapped_ram() ? "ram" : "wrap";
    if (xbox_PhysRamNameMap()[a >> 12])
        return "named";
    if (xbox_ContiguousPageMap()[a >> 12])
        return "win";
    if (a >= g_xbox_image_lo && a < g_xbox_image_hi)
        return "img";
    if (a < xbox_ContiguousAllocatedBytes())
        return "hiwin";
    return "ram";
}

/* A diagnostic switch is on when set to anything but empty or a leading
 * '0', as RECOMP_HEADLESS is read: =0 turns it off rather than on. */
int apu_env_on(const char *name)
{
    const char *e = getenv(name);
    return e && *e && *e != '0';
}

/* RECOMP_APU_TRACE register lines, read once: every MMIO access tests it. */
static int apu_mmio_trace(void)
{
    static int on = -1;
    if (on < 0)
        on = apu_env_on("RECOMP_APU_TRACE");
    return on;
}

/* The interrupt line, as the frame thread sees it. update_irq used to call
 * pci_irq_assert, which is an empty stub here, so DirectSound's service
 * routine never ran and no voice completion ever reached it. */
static volatile LONG s_irq_line;

/* FETINTSTS is not derived from FECTL here. It is latched once, where the
 * front end traps (fe_method), and an ISTS write clears it like any other
 * status bit.
 *
 * This used to OR FETINTSTS back in whenever FECTL's method mode was not
 * free running (the mask also matches HALTED). The guest's service routine
 * acks ISTS first and resumes the front end afterwards, so its own ack
 * re-latched the bit, and FETRAPREASON is left at REQUESTED after the
 * resume. The next delivery -- a frame later, or the next notification --
 * found FETINTSTS with a stale trap reason and handled FEDECMETH/FEDECPARAM a
 * second time. By then the title's threads, which run alongside the
 * interrupt here, had often released that voice: the routine looked the
 * stale voice up, got a null object and dereferenced it (a read at
 * 0xFFFFFFBE). A guest that halts the front end itself and later acks
 * anything got the same stale replay, plus an unwanted resume. It also made
 * every trap cost two or three ISR calls instead of one. */
static void update_irq(MCPXAPUState *d)
{
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
        ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) &
         d->regs[NV_PAPU_IEN])) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_GINTSTS);
        InterlockedExchange(&s_irq_line, 1);
    } else {
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~NV_PAPU_ISTS_GINTSTS);
        InterlockedExchange(&s_irq_line, 0);
    }
}

/* ---- delivering it ---------------------------------------------------- */

/* HalGetInterruptVector(5) -- the APU's IRQ -- is what DirectSound connects
 * its service routine to (0x002F8E81 in Burnout 3). */
#define APU_VECTOR 5

typedef void (*apu_guest_fn)(void);
extern apu_guest_fn recomp_lookup(uint32_t xbox_va);
extern int  xbox_worker_stack_alloc(void);
extern void xbox_worker_stack_free(int slot);
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);
extern uint32_t xbox_AllocThreadTib(void);
extern int xbox_IrqlBlocksInterrupts(void);
extern int xbox_IrqlEnterInterrupt(int level);
extern void xbox_IrqlLeaveInterrupt(int saved);
extern int  xbox_DispatchGateOn(void);
extern int  xbox_DispatchGateTryEnter(void);
extern void xbox_DispatchGateLeave(void);
/* kernel/kernel.h's xbox_Irq* (the device-interrupt post), declared here as
 * the rest are: the APU does not include the kernel header. */
enum { XBOX_IRQ_APU = 1 };
enum { XBOX_IRQ_POSTED = 0, XBOX_IRQ_GATED = 1, XBOX_IRQ_UNGATED = 2 };
extern int  xbox_IrqSafePointsOn(void);
extern void xbox_IrqSetHandler(int line, void (*run)(void));
extern int  xbox_IrqPost(int line);
extern void xbox_IrqNoteUngated(int line);
#if defined(_MSC_VER)
#  define APU_TLS __declspec(thread)
#else
#  define APU_TLS __thread
#endif
extern APU_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
extern APU_TLS uint32_t g_fs_base;

/* Call the connected service routine while the line is up, from the frame
 * thread, the way the OHCI model delivers USB interrupts (ohci_call_isr):
 * a worker stack for the call and a TIB of this thread's own. When a guest
 * thread or DPC holds the one-CPU gate, which is when the single-CPU console
 * could not have taken the interrupt beside it, the interrupt is posted and
 * the holder runs it at its next safe point (xbox_IrqPost). The routine
 * acknowledges by writing ISTS, which drops the line through update_irq. */

/* Push the routine's arguments on g_esp and call it at the device IRQL.
 * Returns what it returned, or -1 with nothing connected. */
static int apu_call_isr(void)
{
    uint32_t kint, routine, context;
    apu_guest_fn fn;

    kint = xbox_GetConnectedInterrupt(APU_VECTOR);
    if (!kint)
        return -1;
    routine = *(uint32_t *)(g_apu_ram_ptr + kint + 0);
    context = *(uint32_t *)(g_apu_ram_ptr + kint + 4);
    fn = routine ? recomp_lookup(routine) : NULL;
    if (!fn)
        return -1;
    g_esp -= 4; *(uint32_t *)(g_apu_ram_ptr + g_esp) = context;
    g_esp -= 4; *(uint32_t *)(g_apu_ram_ptr + g_esp) = kint;
    g_esp -= 4; *(uint32_t *)(g_apu_ram_ptr + g_esp) = 0xDEADBEEFu;
    { int _irql = xbox_IrqlEnterInterrupt(16); fn(); xbox_IrqlLeaveInterrupt(_irql); }
    {
        static unsigned n;
        if (n++ < 3) {
            fprintf(stderr, "[APU] interrupt delivered to 0x%08X -> %s\n",
                    routine, (g_eax & 1) ? "claimed" : "declined");
            fflush(stderr);
        }
    }
    return (int)(g_eax & 1u);
}

/* The gate holder's run of a posted APU interrupt (kernel_hal.c has put it
 * on a worker stack). The routine reads the APU registers through the MMIO
 * trap, which takes d->lock as any guest access does; the frame thread is
 * not holding it here (apu_deliver_irq drops it before posting). If the line
 * dropped since the post, the routine reads ISTS and declines, as on
 * hardware. */
static void apu_isr_on_holder(void)
{
    apu_call_isr();
}

static void apu_deliver_irq(MCPXAPUState *d)
{
    /* Per thread, like the g_fs_base it guards: a new instance's frame
     * thread is a new OS thread and needs its own TIB. g_fs_base itself
     * cannot tell, as every thread starts at the main thread's TIB. */
    static APU_TLS int tib_ready;
    static unsigned held_off;
    uint32_t kint, routine;
    int slot, how;

    if (!InterlockedCompareExchange(&s_irq_line, 0, 0))
        return;
    kint = xbox_GetConnectedInterrupt(APU_VECTOR);
    if (!kint)
        return;
    routine = *(uint32_t *)(g_apu_ram_ptr + kint + 0);
    if (!routine || !recomp_lookup(routine))
        return;
    if (!tib_ready) {
        uint32_t tib = xbox_AllocThreadTib();
        if (!tib)
            return;
        g_fs_base = tib;
        tib_ready = 1;
    }

    qemu_mutex_unlock(&d->lock);
    xbox_IrqSetHandler(XBOX_IRQ_APU, apu_isr_on_holder);
    /* The one-CPU gate (xbox_DispatchGateTryEnter): with it, no thread at
     * DISPATCH_LEVEL, no DPC and no KeSynchronizeExecution routine runs
     * alongside the routine. Held: posted, and this thread goes on with its
     * frame. It must never wait for the gate: a DirectSound DPC holding it
     * can be waiting on this thread to clear a trapped FE method. The post
     * happens without d->lock, since the release of the gate it may take
     * runs other lines' routines here. */
    if (xbox_IrqSafePointsOn()) {
        how = xbox_IrqPost(XBOX_IRQ_APU);
        if (how == XBOX_IRQ_POSTED) {
            qemu_mutex_lock(&d->lock);
            return;
        }
    } else if (!xbox_DispatchGateOn()) {
        /* No gate (the default): a hold-off while a guest thread sits at
         * raised IRQL, which is when the single-CPU console could not have
         * taken the interrupt, then delivered anyway. */
        if (xbox_IrqlBlocksInterrupts() && ++held_off <= 50) {
            qemu_mutex_lock(&d->lock);
            return;
        }
        held_off = 0;
        how = XBOX_IRQ_GATED;   /* an absent gate's leave takes nothing */
    } else {
        /* RECOMP_IRQ_SAFE_POINTS=0: the bounded hold-off, then beside the
         * holder. */
        how = xbox_DispatchGateTryEnter() ? XBOX_IRQ_GATED : XBOX_IRQ_UNGATED;
        if (how == XBOX_IRQ_UNGATED && ++held_off <= 50) {
            qemu_mutex_lock(&d->lock);
            return;
        }
        held_off = 0;
    }
    if (how == XBOX_IRQ_UNGATED)
        xbox_IrqNoteUngated(XBOX_IRQ_APU);
    slot = xbox_worker_stack_alloc();
    if (slot >= 0) {
        g_esp = XBOX_WORKER_STACK_TOP(slot);
        g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;
        apu_call_isr();
        xbox_worker_stack_free(slot);
    }
    if (how == XBOX_IRQ_GATED)
        xbox_DispatchGateLeave();
    qemu_mutex_lock(&d->lock);
}

/* ============================================================
 * MMIO Read / Write
 * ============================================================ */

uint64_t mcpx_apu_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    uint64_t r = 0;

    switch (addr) {
    case NV_PAPU_XGSCNT:
        /* The sequencer's own sample counter, reset by a title write and
         * advanced by the frame thread while the sequencer runs. It used to
         * return the host clock divided by 100, which advances far too fast
         * and in the wrong units for a title comparing it against SSL. */
        r = d->sample_count;
        break;
    default:
        if (addr < 0x20000) {
            r = qatomic_read(&d->regs[addr]);
        }
        break;
    }

    if (apu_mmio_trace())
        fprintf(stderr, "[APU] read  [0x%05llX] size=%u -> 0x%08llX\n",
                (unsigned long long)addr, size, (unsigned long long)r);
    (void)size;
    return r;
}

void mcpx_apu_write(void *opaque, hwaddr addr, uint64_t val,
                     unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;

    if (apu_mmio_trace())
        fprintf(stderr, "[APU] write [0x%05llX] size=%u <- 0x%08llX\n",
                (unsigned long long)addr, size, (unsigned long long)val);
    (void)size;

    switch (addr) {
    case NV_PAPU_XGSCNT:
        /* Writing XGSCNT resets the sequencer's sample counter; the value
         * the title writes (0, so far, always) becomes the new start. */
        qatomic_set(&d->regs[addr], (uint32_t)val);
        d->sample_count = val;
        break;
    case NV_PAPU_ISTS:
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~(uint32_t)val);
        update_irq(d);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FECTL:
    case NV_PAPU_SECTL:
        qatomic_set(&d->regs[addr], (uint32_t)val);
        /* Starting the APU has to start the frame thread.
         *
         * The thread idles on pause_requested, which init sets and only the
         * test tone ever cleared -- so a title that enabled the APU through
         * these registers got an APU that stayed asleep. Nothing then advanced
         * the front end, and a title waiting on a notify completion (the
         * FEMEMDATA magic write, which is how completion reaches guest memory)
         * waited forever. Wreckless hangs exactly there during DirectSound
         * init, and because it initialises its whole engine behind a
         * successful DirectSound create, that hang is not confined to audio.
         *
         * Resume whenever the write is not switching the block off; the thread
         * re-checks FECTL itself and idles again if it is halted or trapped. */
        {
            uint32_t sectl = qatomic_read(&d->regs[NV_PAPU_SECTL]);
            uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
            bool running =
                ((sectl & NV_PAPU_SECTL_XCNTMODE) != NV_PAPU_SECTL_XCNTMODE_OFF)
                && ((fectl & NV_PAPU_FECTL_FEMETHMODE)
                    != NV_PAPU_FECTL_FEMETHMODE_HALTED);
            if (running && d->pause_requested) {
                d->pause_requested = false;
                fprintf(stderr, "[APU] started by the title"
                                " (SECTL=%08X FECTL=%08X)\n", sectl, fectl);
            }
        }
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write' - value written to FEMEMADDR on notify completion */
        stl_le_phys(address_space_memory, d->regs[NV_PAPU_FEMEMADDR], (uint32_t)val);
        qatomic_set(&d->regs[addr], (uint32_t)val);
        break;
    default:
        if (addr < 0x20000) {
            qatomic_set(&d->regs[addr], (uint32_t)val);
        }
        break;
    }
}

/* ============================================================
 * Test tone state (used by monitor and test tone functions)
 * ============================================================ */

static struct {
    bool active;
    double phase;
    double phase_inc;
    int16_t amplitude;
} g_test_tone = { false, 0.0, 0.0, 0 };

/* ============================================================
 * Monitor - Audio output (XAudio2 primary, waveOut fallback)
 * ============================================================ */

#if defined(_WIN32)
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif
/* On Linux, waveOut* are inert stubs from win32_compat.h: the APU's
 * waveOut fallback path stays inactive and never produces audio. */

/* Ring of waveOut buffers for double-buffering */
#define WAVEOUT_NUM_BUFS 4
#define WAVEOUT_BUF_SAMPLES 2048  /* ~42.7ms at 48kHz, matches 8-frame delivery rate */
#define MIXER_FRAME_SAMPLES 256  /* Internal mixing frame size (matches frame_buf) */
/* monitor.queued_bytes_low/high are the pacing marks in bytes of stereo
 * s16 output; pacing compares them in samples (SAMPLE_BYTES each), so a
 * device buffer that is not a multiple of the 256-sample EP period keeps
 * exact marks. */
#define SAMPLE_BYTES (2 * (int)sizeof(int16_t))

typedef struct {
    HWAVEOUT hwo;
    WAVEHDR  hdrs[WAVEOUT_NUM_BUFS];
    int16_t  bufs[WAVEOUT_NUM_BUFS][WAVEOUT_BUF_SAMPLES][2];
    int      next_buf;
    bool     initialized;
    int      frames_written;
} WaveOutState;

static WaveOutState g_waveout = { 0 };

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    (void)errp;
    d->monitor.stream = NULL;
    d->monitor.queued_bytes_low = 1024;
    d->monitor.queued_bytes_high = 3072;

    /* RECOMP_AUDIO_WAV=<path>: dump exactly what the device is given,
     * capped at RECOMP_AUDIO_WAV_SECS seconds when that is set. */
    {
        const char *wav = getenv("RECOMP_AUDIO_WAV");
        const char *secs = getenv("RECOMP_AUDIO_WAV_SECS");
        if (wav && *wav)
            apu_wav_open(wav, secs ? atof(secs) : 0.0);
    }

    /* Try XAudio2 first (lower latency) */
    if (xa2_init()) {
        /* Pacing marks in samples (xa2_pacing_marks): high is all the
         * device buffers, capped so a period submitted below it never
         * fills a buffer the voice has no slot for; low is half of it. */
        int low, high;
        xa2_pacing_marks(xa2_get_buffer_size(), xa2_get_buffer_count(),
                         MIXER_FRAME_SAMPLES, &low, &high);
        d->monitor.queued_bytes_low  = low * SAMPLE_BYTES;
        d->monitor.queued_bytes_high = high * SAMPLE_BYTES;
        fprintf(stderr, "[APU] Using %s audio backend (pacing: %d..%d samples queued)\n",
                XA2_BACKEND_NAME, low, high);
        if (high < 2 * MIXER_FRAME_SAMPLES)
            fprintf(stderr, "[APU] device buffers hold under two EP periods;"
                            " expect underruns\n");
        return;
    }
    fprintf(stderr, "[APU] %s unavailable, falling back to waveOut\n", XA2_BACKEND_NAME);

    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT mr = waveOutOpen(&g_waveout.hwo, WAVE_MAPPER, &wfx,
                               0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        fprintf(stderr, "[APU] waveOutOpen failed (error %u)\n", mr);
        g_waveout.initialized = false;
        return;
    }

    /* Prepare all headers */
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        memset(&g_waveout.hdrs[i], 0, sizeof(WAVEHDR));
        g_waveout.hdrs[i].lpData = (LPSTR)g_waveout.bufs[i];
        g_waveout.hdrs[i].dwBufferLength = WAVEOUT_BUF_SAMPLES * 2 * sizeof(int16_t);
        waveOutPrepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }

    g_waveout.next_buf = 0;
    g_waveout.initialized = true;
    g_waveout.frames_written = 0;

    fprintf(stderr, "[APU] waveOut audio output initialized (48kHz stereo 16-bit, %d buffers)\n",
            WAVEOUT_NUM_BUFS);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    (void)d;
    apu_wav_close();
    if (xa2_is_active()) {
        xa2_shutdown();
        return;
    }
    if (!g_waveout.initialized) return;

    waveOutReset(g_waveout.hwo);
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        waveOutUnprepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_waveout.hwo);
    g_waveout.initialized = false;
    fprintf(stderr, "[APU] waveOut audio output shut down (%d frames written)\n",
            g_waveout.frames_written);
}

/* ============================================================
 * Host playback accounting (RECOMP_AUDIO_HOST, or RECOMP_APU_TRACE)
 *
 * The WAV tap records what the APU hands the device backend, back to back,
 * so it cannot show a period that reached the device late: the device
 * plays silence meanwhile and the WAV has no hole. A clean WAV therefore
 * says nothing about whether the output was continuous; that rests on what
 * the device consumed, which this measures. At each submit the device's
 * consumed-sample count (xa2_samples_played) is set against the wall clock
 * since a baseline. The difference, the deficit, is silence the device
 * inserted, plus about one device buffer of jitter (the device takes a
 * buffer at a time, and Wine mixes in its own periods). Each time the
 * deficit passes the part already accounted for by more than that jitter,
 * the excess is starvation, logged as
 *   [AUDIO-HOST] starve wav=<sample index> ms=<length> ...
 * with the sample index into the WAV, so a check can splice that much
 * silence in and analyse what the speaker actually played. starved_ms on
 * the once-a-second line is their total, and pending_ms what is above the
 * accounted part but not yet reported (negative: inside the jitter band);
 * the inserted silence is their sum, to within one buffer. Also tracked:
 * submits the backend refused (in the WAV, never played), the longest
 * wall-clock gap between submits, and each spell with FECTL trapped
 * (frames produced: none) with its wall-clock length.
 * ============================================================ */

/* For this long after a baseline the accounted part follows the deficit
 * up, so it starts at the top of the jitter band: the device's first pull,
 * and XAudio2 staging a whole buffer before it queues one, both read as
 * deficit. A loss in it is still reported when it comes as one rise of
 * more than the jitter. */
#define STARVE_SETTLE_US 250000
/* The device's clock and the wall clock drift apart. While the device has
 * not run dry the accounted part follows the deficit up whatever the rate
 * (a device clock several percent slow, as SDL's dummy driver runs, is
 * not loss). Once it has, a loss is pending and the accounted part only
 * creeps up at this rate, so drift on a real device (tens of ppm) is not
 * added to it, and silence inserted at under 0.2 ms a second is missed.
 * The limit: on a clock more than this far off (the dummy driver) each dry
 * spell can add up to about a buffer of drift to its report, and a single
 * loss under one buffer is absorbed by the creep and never reported. */
#define STARVE_DRIFT_PPM 200

void apu_starve_rebase(ApuStarve *s, int64_t now_us, uint64_t played)
{
    s->t0_us = now_us;
    s->played0 = played;
    s->deficit = 0;
    s->ref = 0;
    s->ref_us = now_us;
    s->dry = 0;
}

int64_t apu_starve_step(ApuStarve *s, int64_t now_us, uint64_t played,
                        int jitter, int dry)
{
    int64_t deficit = (now_us - s->t0_us) * 48 / 1000
                    - (int64_t)(played - s->played0);
    double creep = (double)(now_us - s->ref_us) * 48e-3 * STARVE_DRIFT_PPM * 1e-6;

    s->deficit = deficit;
    s->ref_us = now_us;
    s->dry |= dry;
    /* Once the device has run dry, only a reported loss moves the accounted
     * part to the deficit, so the reports add up to the inserted silence
     * however it came. Moving it to every new high (as this did) left a run
     * of losses each under the jitter -- twenty 5 ms gaps -- reporting
     * nothing, while one 100 ms gap reported 100 ms. */
    if (s->dry && deficit > s->ref + jitter) {
        int64_t lost = deficit - (int64_t)s->ref;
        s->ref = (double)deficit;
        s->dry = 0;
        return lost;
    }
    if (deficit > s->ref) {
        if (!s->dry || now_us - s->t0_us < STARVE_SETTLE_US)
            s->ref = (double)deficit;
        else
            s->ref = fmin((double)deficit, s->ref + creep);
    } else if (deficit < s->ref - jitter) {
        /* A device clock running fast pulls the band down: follow it to
         * the top of the band. */
        s->ref = (double)(deficit + jitter);
    }
    return 0;
}

static struct {
    int      on;                /* -1 until the environment is read */
    ApuStarve acct;             /* t0_us 0: a new baseline at the next submit */
    uint64_t submitted;         /* samples handed to the backend (= WAV sample index) */
    unsigned refused;           /* submits the backend refused (in the WAV, not played) */
    unsigned underruns_seen;    /* xa2_underruns() at the last submit */
    double   starved_ms;        /* total, all baselines */
    unsigned starves;
    int64_t  last_submit_us, gap_max_us;    /* gap_max: this report interval */
    int      q_min;                         /* this report interval */
    int64_t  trap_start_us, trap_max_us;    /* trap_max: this report interval */
    unsigned trap_spells, trap_long;        /* long: > 20 ms */
    int64_t  next_report_us;
    int64_t  first_submit_us;               /* for the rate lines */
} g_hs = { -1 };

static int hs_on(void)
{
    if (g_hs.on < 0)
        g_hs.on = apu_env_on("RECOMP_APU_TRACE") ||
                  apu_env_on("RECOMP_AUDIO_HOST");
    return g_hs.on;
}

/* Called with the frame thread's view of FECTL each loop iteration. */
static void hs_trap(bool trapped)
{
    int64_t now;
    if (!hs_on())
        return;
    now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (trapped) {
        if (!g_hs.trap_start_us)
            g_hs.trap_start_us = now;
        return;
    }
    if (g_hs.trap_start_us) {
        int64_t dur = now - g_hs.trap_start_us;
        g_hs.trap_spells++;
        if (dur > 20000)
            g_hs.trap_long++;
        if (dur > g_hs.trap_max_us)
            g_hs.trap_max_us = dur;
        if (dur > 20000)
            fprintf(stderr, "[AUDIO-HOST] trap spell %.1f ms ending at wav=%llu\n",
                    dur / 1000.0, (unsigned long long)g_hs.submitted);
        g_hs.trap_start_us = 0;
    }
}

/* The frame thread is pausing on purpose: the device drains, which is not
 * starvation. Start a new baseline after it. */
static void hs_rebase(void)
{
    g_hs.acct.t0_us = 0;
}

/* The queue is read before the submit. Both backends count an underrun
 * only when a poll sees the queue empty, and a dry spell that begins
 * partway through a period (a front-end trap, a stall) gets no other poll
 * before this one: read after the submit, the queue holds the new period
 * and the spell is never counted, so its loss would be folded into the
 * reference. It also keeps queued= and q_min meaning what was left. */
static int hs_poll(void)
{
    return hs_on() && xa2_is_active() ? xa2_queued_samples() : 0;
}

/* After handing n samples (stereo frames) to the device; ok is what the
 * backend returned and q the queue hs_poll read just before. A refused
 * submit is in the WAV and counts in the sample index all the same, so the
 * index stays the WAV's; what was lost ends at the index on the refused
 * line (on XAudio2 the whole device buffer the period completed, on SDL2
 * the period). */
static void hs_submit(int n, int ok, int q)
{
    int64_t now, gap, lost;
    uint64_t played;
    unsigned u;

    if (!hs_on() || !xa2_is_active())
        return;
    now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    played = xa2_samples_played();

    /* samples= on the last-submit line is what was submitted since the
     * first, so samples / (t_last - t_first) is the rate the device was fed
     * at, pauses included. */
    if (!g_hs.first_submit_us) {
        g_hs.first_submit_us = now;
        fprintf(stderr, "[APU] first submit t=%.6f\n", now / 1e6);
    }
    if (g_hs.acct.t0_us == 0) {
        apu_starve_rebase(&g_hs.acct, now, played);
        g_hs.underruns_seen = xa2_underruns();
        g_hs.last_submit_us = now;
        g_hs.q_min = q;
        g_hs.next_report_us = now + 1000000;
    }

    gap = now - g_hs.last_submit_us;
    if (gap > g_hs.gap_max_us)
        g_hs.gap_max_us = gap;
    g_hs.last_submit_us = now;
    if (q < g_hs.q_min)
        g_hs.q_min = q;

    u = xa2_underruns();
    lost = apu_starve_step(&g_hs.acct, now, played, xa2_get_buffer_size(),
                           u != g_hs.underruns_seen);
    g_hs.underruns_seen = u;
    if (lost) {
        g_hs.starves++;
        g_hs.starved_ms += lost / 48.0;
        fprintf(stderr, "[AUDIO-HOST] starve wav=%llu ms=%.1f queued=%d gap_ms=%.1f trapped=%d\n",
                (unsigned long long)g_hs.submitted, lost / 48.0, q, gap / 1000.0,
                g_hs.trap_start_us != 0);
    }
    g_hs.submitted += (uint64_t)n;
    if (!ok && g_hs.refused++ < 100)
        fprintf(stderr, "[AUDIO-HOST] refused wav=%llu queued=%d\n",
                (unsigned long long)g_hs.submitted, q);

    if (now >= g_hs.next_report_us) {
        fprintf(stderr, "[AUDIO-HOST] wav=%llu played=%llu starves=%u starved_ms=%.1f pending_ms=%.1f"
                " deficit=%lld refused=%u gap_max_ms=%.1f q_min=%d trap_spells=%u trap_long=%u"
                " trap_max_ms=%.1f underruns=%u\n",
                (unsigned long long)g_hs.submitted, (unsigned long long)played,
                g_hs.starves, g_hs.starved_ms,
                (g_hs.acct.deficit - g_hs.acct.ref) / 48.0, (long long)g_hs.acct.deficit,
                g_hs.refused, g_hs.gap_max_us / 1000.0, g_hs.q_min,
                g_hs.trap_spells, g_hs.trap_long, g_hs.trap_max_us / 1000.0,
                xa2_underruns());
        fprintf(stderr, "[APU] last submit t=%.6f samples=%llu\n", now / 1e6,
                (unsigned long long)(g_hs.submitted - (uint64_t)n));
        g_hs.gap_max_us = 0;
        g_hs.trap_max_us = 0;
        g_hs.q_min = q;
        g_hs.next_report_us = now + 1000000;
    }
}

/* Saturating add of a 16-bit stereo block into another. */
static void mix_into(int16_t dst[][2], const int16_t src[][2], int n)
{
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < 2; c++) {
            int32_t v = (int32_t)dst[i][c] + src[i][c];
            if (v > 32767)  v = 32767;
            if (v < -32768) v = -32768;
            dst[i][c] = (int16_t)v;
        }
    }
}

/* Hand one EP period (MIXER_FRAME_SAMPLES samples) to the waveOut arm. The
 * device buffers are larger than a period, so periods accumulate in the
 * current buffer and it is written when full: every sample rendered is
 * written once, none is rendered twice. */
static void waveout_submit(MCPXAPUState *d, const int16_t period[][2])
{
    static int fill;                     /* samples already in next_buf */
    int idx = g_waveout.next_buf;
    WAVEHDR *hdr = &g_waveout.hdrs[idx];

    if (fill == 0) {
        /* Wait if this buffer is still playing (with timeout) */
        int wait_loops = 0;
        while (!(hdr->dwFlags & WHDR_DONE) && (hdr->dwFlags & WHDR_INQUEUE)) {
            qemu_mutex_unlock(&d->lock);
            Sleep(1);
            qemu_mutex_lock(&d->lock);
            if (++wait_loops > 50) break;
        }
    }

    memcpy(g_waveout.bufs[idx][fill], period,
           MIXER_FRAME_SAMPLES * 2 * sizeof(int16_t));
    fill += MIXER_FRAME_SAMPLES;
    if (fill < WAVEOUT_BUF_SAMPLES)
        return;

    hdr->dwFlags &= ~WHDR_DONE;
    waveOutWrite(g_waveout.hwo, hdr, sizeof(WAVEHDR));
    g_waveout.next_buf = (idx + 1) % WAVEOUT_NUM_BUFS;
    g_waveout.frames_written++;
    fill = 0;
}

/* Called once per 32-sample frame; acts on the last frame of each EP period
 * (8 frames, 256 samples, 5.33 ms).
 *
 * frame_buf holds the period the DSP stage just wrote (mcpx_apu_dsp_frame,
 * or the VP monitor point, which accumulates). The test tone and the
 * software mixer are rendered into a scratch period and added to it, and
 * exactly that one period is handed to the device -- once.
 *
 * This used to clear frame_buf before rendering the tone and the mixer into
 * it, so whatever the VP and DSP produced never reached the device, and it
 * rendered a whole device buffer (1024 samples) per 256-sample period. */
void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    int16_t period[MIXER_FRAME_SAMPLES][2];
    int16_t extra[MIXER_FRAME_SAMPLES][2];

    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    memcpy(period, d->monitor.frame_buf, sizeof(period));
    /* The VP monitor point accumulates into frame_buf, so the next period
     * has to start from silence. */
    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));

    if (g_audio_muted) {
        memset(period, 0, sizeof(period));
    } else {
        memset(extra, 0, sizeof(extra));
        if (g_test_tone.active) {
            for (int i = 0; i < MIXER_FRAME_SAMPLES; i++) {
                int16_t s = (int16_t)(sin(g_test_tone.phase) * g_test_tone.amplitude);
                extra[i][0] = s;
                extra[i][1] = s;
                g_test_tone.phase += g_test_tone.phase_inc;
                if (g_test_tone.phase >= 2.0 * M_PI)
                    g_test_tone.phase -= 2.0 * M_PI;
            }
        }
        mixer_render(extra, MIXER_FRAME_SAMPLES);
        mix_into(period, (const int16_t (*)[2])extra, MIXER_FRAME_SAMPLES);
    }

    /* A period dropped while pacing is stalled never reaches the device,
     * so it stays out of the dump too. */
    if (!(xa2_is_active() && d->pace_stalled))
        apu_wav_write((const int16_t *)period, MIXER_FRAME_SAMPLES);

    if (xa2_is_active()) {
        if (!d->pace_stalled) {
            int q = hs_poll();
            hs_submit(MIXER_FRAME_SAMPLES,
                      xa2_submit_samples((const int16_t *)period,
                                         MIXER_FRAME_SAMPLES), q);
        }
        return;
    }
    if (g_waveout.initialized)
        waveout_submit(d, (const int16_t (*)[2])period);
}

/* ============================================================
 * Throttle (timing control for frame pacing)
 * ============================================================ */

/* How long the device queue may sit at high without falling before the
 * device counts as stopped. A consuming device takes a buffer (10.7 ms at
 * the default 512 samples) well inside this; the largest buffers the hosts
 * accept (4096 samples, 85 ms) still fit. */
#define PACE_STALL_US 200000

/* One pacing decision, taken at an EP-period boundary. Returns 0 to render
 * the next period now, or how many microseconds to wait before asking again.
 *
 * Without a queue to read (waveOut, or no device), the host clock paces one
 * period per EP_FRAME_US, as before. With XAudio2 the device queue paces:
 *
 *   queued >= high   wait one period (the device has enough; the clock is
 *                    not advanced, so a device that pauses briefly stops
 *                    rendering at high and nothing is dropped)
 *   queued <= low    render ahead of the clock, but no further ahead of it
 *                    than the queue can hold (high samples), so a device
 *                    that stops consuming cannot run the APU ahead of guest
 *                    time. (Bounding it by one device buffer instead holds
 *                    the queue under any low mark larger than a buffer: the
 *                    clock then admits one period per period and the queue
 *                    never climbs.)
 *   otherwise        one period per EP_FRAME_US of host clock, counted
 *                    from now rather than from a schedule a render-ahead
 *                    pushed forward
 *
 * A device that stops consuming for good (an XAudio2 critical error or a
 * removed device, a suspended PipeWire sink, a paused SDL device) would hold
 * the queue at high forever, and with it the APU clock: no frames, no voice
 * notifications, and every guest wait on them hangs. When the queue has not
 * fallen for PACE_STALL_US the host clock paces instead and the periods are
 * dropped rather than queued (pace_stalled, read by monitor_frame); the
 * first fall of the queue hands pacing back to it.
 *
 * The period's credit is charged by mcpx_apu_pace_rendered, once the period
 * has been rendered, not here: a pass that renders nothing (the front end
 * trapped) asks again a millisecond later, and charging each of those asks
 * let a trap that began on a period boundary eat the queue's lead.
 *
 * A clock that has fallen more than a period behind (after a stall, or the
 * first call) restarts from now instead of rendering the backlog in a burst. */
int64_t mcpx_apu_pace_step(MCPXAPUState *d, int64_t now_us)
{
    int64_t ahead_us = 0;
    int mid_band = 0;

    if (d->ep_frame_div % 8)
        return 0;

    if (xa2_is_active()) {
        int q    = xa2_queued_samples();
        int low  = d->monitor.queued_bytes_low / SAMPLE_BYTES;
        int high = d->monitor.queued_bytes_high / SAMPLE_BYTES;

        if (q < high || q < d->pace_q_seen || d->pace_drain_us == 0) {
            d->pace_drain_us = now_us;
            if (d->pace_stalled) {
                d->pace_stalled = false;
                fprintf(stderr, "[APU] audio device consuming again: the"
                                " queue paces the APU\n");
            }
        }
        d->pace_q_seen = q;
        if (!d->pace_stalled && q >= high &&
            now_us - d->pace_drain_us >= PACE_STALL_US) {
            static int shown;
            d->pace_stalled = true;
            if (!shown++)
                fprintf(stderr, "[APU] audio device stopped consuming (%d"
                                " samples queued for %d ms): the host clock"
                                " paces the APU and periods are dropped\n",
                        q, PACE_STALL_US / 1000);
        }

        if (d->pace_stalled)
            ;                              /* host clock, as without a queue */
        else if (q >= high)
            return EP_FRAME_US;
        else if (q <= low)
            ahead_us = (int64_t)high * EP_FRAME_US / MIXER_FRAME_SAMPLES;
        else
            mid_band = 1;
    } else {
        d->pace_stalled = false;
    }

    if (d->next_frame_time_us == 0 ||
        now_us - d->next_frame_time_us > EP_FRAME_US)
        d->next_frame_time_us = now_us;

    /* In the band the lead a render-ahead built is already in the queue;
     * keeping it in the schedule as well would idle until the queue had
     * drained it. One period per period from here holds the level. */
    if (mid_band && d->next_frame_time_us - now_us > EP_FRAME_US)
        d->next_frame_time_us = now_us + EP_FRAME_US;

    if (d->next_frame_time_us - now_us > ahead_us)
        return d->next_frame_time_us - now_us - ahead_us;
    return 0;
}

/* A period that pace_step admitted has been rendered: advance the clock. */
void mcpx_apu_pace_rendered(MCPXAPUState *d)
{
    d->next_frame_time_us += EP_FRAME_US;
}

static void throttle(MCPXAPUState *d)
{
    int64_t start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    while (!d->pause_requested) {
        int64_t wait_us = mcpx_apu_pace_step(
            d, qemu_clock_get_us(QEMU_CLOCK_REALTIME));
        if (wait_us <= 0)
            break;
        qemu_cond_timedwait(&d->cond, &d->lock,
                            (int)((wait_us + 999) / 1000));
    }

    d->sleep_acc_us += (int)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) - start_us);
}

/* ============================================================
 * se_frame - Process one audio frame (VP -> GP -> EP pipeline)
 * ============================================================ */

static void se_frame(MCPXAPUState *d)
{
    mcpx_apu_update_dsp_preference(d);
    mcpx_debug_begin_frame();
    g_dbg.gp_realtime = d->gp.realtime;
    g_dbg.ep_realtime = d->ep.realtime;

    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t elapsed_ms = now_ms - d->frame_count_time_ms;
    if (elapsed_ms >= 1000) {
        g_dbg.utilization = 1.0f - d->sleep_acc_us / (elapsed_ms * 1000.0f);
        g_dbg.frames_processed = (int)(d->frame_count * 1000.0 / elapsed_ms + 0.5);
        d->frame_count_time_ms = now_ms;
        d->frame_count = 0;
        d->sleep_acc_us = 0;
    }
    d->frame_count++;
    apu_irq_stats.se_frames++;

    /* Buffer for all mixbins for this frame */
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    memset(mixbins, 0, sizeof(mixbins));

    mcpx_apu_vp_frame(d, mixbins);
    mcpx_apu_dsp_frame(d, mixbins);
    mcpx_apu_monitor_frame(d);

    d->ep_frame_div++;

    mcpx_debug_end_frame();
}

/* ============================================================
 * APU frame thread (background processing)
 * ============================================================ */

ApuIrqStats apu_irq_stats;

/* Every 5 s under RECOMP_APU_TRACE: front-end traps, the running count of
 * VP/DSP frames (se_frame), the frames that found FECTL trapped, and the
 * device underruns. Trapped frames climbing while se_frames stands still is
 * the service routine not clearing the trap; underruns with se_frames
 * advancing is the host, not the title. */
static int s_irq_trace = -1;
static uint64_t s_irq_next_ns;

static void apu_irq_stats_line(void)
{
    uint64_t now;

    if (s_irq_trace < 0)
        s_irq_trace = apu_env_on("RECOMP_APU_TRACE");
    if (!s_irq_trace)
        return;
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now < s_irq_next_ns)
        return;
    if (s_irq_next_ns)
        fprintf(stderr, "[APU-IRQ] traps %u se_frames %u fe-trapped-frames %u underruns %u\n",
                apu_irq_stats.traps, apu_irq_stats.se_frames,
                apu_irq_stats.fe_trapped_frames, xa2_underruns());
    s_irq_next_ns = now + 5000000000ull;
}

static void *mcpx_apu_frame_thread(void *arg)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(arg);
    qemu_mutex_lock(&d->lock);

    while (!qatomic_read(&d->exiting)) {
        if (d->pause_requested && !g_test_tone.active && !g_mixer_active_count) {
            d->is_idle = true;
            if (xa2_is_active())
                xa2_expect_drain();     /* a pause, not an underrun */
            hs_rebase();
            qemu_cond_signal(&d->idle_cond);
            qemu_cond_wait(&d->cond, &d->lock);
            d->is_idle = false;
            continue;
        }

        /* Always run the audio output loop — the software mixer and test tone
         * need continuous frame delivery regardless of APU register state.
         * The VP/DSP pipeline (se_frame) only runs when registers allow it. */
        throttle(d);
        int div0 = d->ep_frame_div;

        /* The doorbell ack stands in for the GP DSP, which on hardware runs
         * whatever the front end is doing. Tying it to se_frame stopped it
         * whenever FECTL was trapped or halted, and DirectSound then waits
         * forever to post its next command: Burnout 3 stalls in
         * sub_002F805E polling the same doorbell it was acked on at init. */
        mcpx_apu_dsp_ack_poll(d);

        int xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                                NV_PAPU_SECTL_XCNTMODE);
        uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
        bool apu_active = (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED);

        /* A bit test, so HALTED (0x80, a subset of TRAPPED's 0xE0) counts
         * too: xemu's frame thread idles in both modes. */
        bool fe_trapped = (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF) &&
                          (fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) &&
                          !g_test_tone.active;

        hs_trap(fe_trapped);

        if (apu_active && !g_test_tone.active) {
            /* Full pipeline: VP voices → DSP → monitor → waveOut */
            se_frame(d);
        } else if (fe_trapped) {
            /* The title has the APU on, but the front end is trapped (a voice
             * went idle under FETFORCE1) and stays so until DirectSound's
             * service routine clears FECTL, or the title halted it. No frame
             * is produced meanwhile, as in xemu, which waits and re-checks:
             * the device queue holds what is already paced into it. The
             * software mixer pauses with it; the test tone keeps the branch
             * below. This used to fall through to
             * the lightweight branch, which advances the frame counter and
             * hands the device a period with this frame's 32 samples silent:
             * one hole per trapped frame, four or five in a row when the
             * routine cleared the trap from a DPC (32-160 samples of digital
             * zero in streamed music every few seconds). The interrupt is
             * delivered below; the wait is after it. */
            apu_irq_stats.fe_trapped_frames++;
        } else {
            /* Lightweight: just monitor frame (test tone + software mixer) */
            mcpx_apu_monitor_frame(d);
            d->ep_frame_div++;
        }

        if (div0 % 8 == 0 && d->ep_frame_div != div0)
            mcpx_apu_pace_rendered(d);

        /* The SSL tick: the sequencer's periodic sample-limit interrupt.
         *
         * DirectSound (Black, XDK 5849) resets XGSCNT to 0, programs SSL
         * (0x800 = 2048 samples) and enables ISTS bit 3 in IEN. Its service
         * routine -- dispatched on that bit, kicking the GP doorbell and
         * reposting front-end commands -- is what lets the rest of
         * DirectSound initialisation finish. Without the tick the title
         * re-runs the sequencer setup forever and never gets past audio. */
        {
            uint32_t sectl = qatomic_read(&d->regs[NV_PAPU_SECTL]);
            uint32_t ssl = qatomic_read(&d->regs[NV_PAPU_SSL]);
            d->sample_count += NUM_SAMPLES_PER_FRAME;
            if (ssl
                    && d->sample_count >= ssl
                    && (sectl & NV_PAPU_SECTL_XCNTMODE)
                           != NV_PAPU_SECTL_XCNTMODE_OFF) {
                d->sample_count -= ssl;
                qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_SSLINTSTS);
                d->set_irq = true;
            }
        }

        /* What xemu's frame thread does after each frame: turn a pending
         * notification (set by the voice processor or a trapped method) into
         * the interrupt line. Nothing here ever did, so the line never rose
         * even before there was anything to deliver it to. */
        if (d->set_irq) {
            update_irq(d);
            d->set_irq = false;
        }
        apu_deliver_irq(d);
        apu_irq_stats_line();

        /* Let the guest in once per frame.
         *
         * The thread holds d->lock for its whole loop and only drops it inside
         * throttle()'s wait. Once voices really play, processing can run
         * behind real time, throttle never waits, and the lock is never
         * released -- while every VOICE_ON/OFF/RELEASE the title writes needs
         * it (voice_lock). A critical section is not fair, so the title's
         * thread starved there indefinitely: Burnout 3 froze on its vehicle
         * select, blocked in voice_lock at raised IRQL, which in turn held
         * off every USB interrupt. */
        qemu_mutex_unlock(&d->lock);
        SwitchToThread();
        qemu_mutex_lock(&d->lock);
        if (fe_trapped &&
            (qatomic_read(&d->regs[NV_PAPU_FECTL]) &
             NV_PAPU_FECTL_FEMETHMODE_TRAPPED)) {
            /* Still trapped after the delivery (held off at raised IRQL, or
             * cleared from a DPC): give the title a millisecond with the lock
             * dropped rather than spinning on it. */
            qemu_cond_timedwait(&d->cond, &d->lock, 1);
        }
    }

    qemu_mutex_unlock(&d->lock);
    return NULL;
}

/* ============================================================
 * Wait for idle / resume helpers
 * ============================================================ */

static void mcpx_apu_wait_for_idle(MCPXAPUState *d)
{
    d->pause_requested = true;
    qemu_cond_signal(&d->cond);
    while (!d->is_idle) {
        qemu_cond_wait(&d->idle_cond, &d->lock);
    }
}

static void mcpx_apu_resume(MCPXAPUState *d)
{
    d->pause_requested = false;
    qemu_cond_signal(&d->cond);
}

/* ============================================================
 * Reset
 * ============================================================ */

static void mcpx_apu_reset_locked(MCPXAPUState *d)
{
    memset(d->regs, 0, sizeof(d->regs));
    mcpx_apu_vp_reset(d);

    if (d->gp.dsp) {
        memset((void *)d->gp.dsp->core.pram_opcache, 0,
               sizeof(d->gp.dsp->core.pram_opcache));
    }
    if (d->ep.dsp) {
        memset((void *)d->ep.dsp->core.pram_opcache, 0,
               sizeof(d->ep.dsp->core.pram_opcache));
    }
    d->set_irq = false;
}

/* ============================================================
 * Public API: Init / Shutdown
 * ============================================================ */

/* The diagnostics' state is per instance: a new instance after a shutdown
 * reads the switches again and counts from zero, with no baseline or trap
 * spell left over from the last one. */
static void apu_diag_reset(void)
{
    memset(&g_hs, 0, sizeof(g_hs));
    g_hs.on = -1;
    memset(&apu_irq_stats, 0, sizeof(apu_irq_stats));
    s_irq_trace = -1;
    s_irq_next_ns = 0;
}

MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(MCPXAPUState));
    if (!d) {
        fprintf(stderr, "[APU] Failed to allocate MCPXAPUState\n");
        return NULL;
    }

    g_apu_ram_ptr = ram_ptr;
    g_state = d;
    d->ram_ptr = ram_ptr;
    /* A line left up by a previous instance would call the guest ISR
     * against this one's empty ISTS. */
    InterlockedExchange(&s_irq_line, 0);
    apu_diag_reset();

    /* RECOMP_APU_SOLO=<voice>: mute every other voice in the mix, for
     * telling which voice a sound (or a noise) comes from. */
    {
        const char *solo = getenv("RECOMP_APU_SOLO");
        if (solo && *solo) {
            g_dbg_voice_monitor = atoi(solo);
            fprintf(stderr, "[APU] solo: only voice %d is mixed\n",
                    g_dbg_voice_monitor);
        }
    }

    d->set_irq = false;
    d->exiting = false;
    d->is_idle = false;
    d->pause_requested = true;

    qemu_mutex_init(&d->lock);
    qemu_mutex_lock(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);

    /* Init VP (voice processor) */
    mcpx_apu_vp_init(d);

    /* Init DSP (GP/EP - stubbed) */
    mcpx_apu_dsp_init(d);

    /* Init software mixer for DirectSound bridge */
    mixer_init();

    /* Init monitor (waveOut output) */
    Error *local_err = NULL;
    mcpx_apu_monitor_init(d, &local_err);
    if (local_err) {
        warn_reportf_err(local_err, "mcpx_apu_monitor_init failed: ");
    }

    /* Start background frame thread */
    qemu_thread_create(&d->apu_thread, "mcpx.apu_thread",
                       mcpx_apu_frame_thread, d, QEMU_THREAD_JOINABLE);
    mcpx_apu_wait_for_idle(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU] MCPX APU initialized (standalone)\n");
    fprintf(stderr, "[APU]   RAM pointer: %p\n", (void *)ram_ptr);
    fprintf(stderr, "[APU]   MMIO base: 0xFE800000 (512KB)\n");
    fprintf(stderr, "[APU]   VP: %d max voices, %d samples/frame\n",
            MCPX_HW_MAX_VOICES, NUM_SAMPLES_PER_FRAME);
    return d;
}

void mcpx_apu_shutdown(MCPXAPUState *d)
{
    if (!d) return;

    fprintf(stderr, "[APU] Shutting down MCPX APU...\n");

    qemu_mutex_lock(&d->lock);
    mcpx_apu_wait_for_idle(d);
    qatomic_set(&d->exiting, true);
    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);

    /* Everything below is freed only once the frame thread has returned.
     * g_state is cleared before the teardown, so a mixer start that begins
     * after this point finds no instance; a play already in flight is not
     * serialised against shutdown. */
    qemu_thread_join(&d->apu_thread);
    g_state = NULL;
    mcpx_apu_vp_finalize(d);
    mcpx_apu_dsp_finalize(d);
    mcpx_apu_monitor_finalize(d);

    qemu_cond_destroy(&d->idle_cond);
    qemu_cond_destroy(&d->cond);
    qemu_mutex_destroy(&d->lock);
    free(d);
    fprintf(stderr, "[APU] Shutdown complete\n");
}

/* ============================================================
 * VP MMIO handlers (sub-region at +0x20000)
 *
 * These are called when the game writes to the VP PIO registers
 * to configure voices, SSL, etc.
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size);
void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

/* Dispatch a VP-region access (offset 0x20000-0x2FFFF from APU base) */
void mcpx_apu_dispatch_mmio(MCPXAPUState *d, hwaddr addr, uint64_t val,
                             unsigned int size, bool is_write)
{
    if (addr >= 0x20000 && addr < 0x30000) {
        /* VP region */
        hwaddr vp_addr = addr - 0x20000;
        if (is_write) {
            mcpx_apu_vp_write(d, vp_addr, val, size);
        }
        /* VP reads handled by caller if needed */
    } else if (addr < 0x20000) {
        /* Main APU registers */
        if (is_write) {
            mcpx_apu_write(d, addr, val, size);
        }
    } else if (addr == 0x30000 + NV_PAPU_GPRST) {
        if (is_write)
            mcpx_apu_dsp_gprst_write((uint32_t)val);
    }
    /* The rest of the GP (0x30000) and EP (0x50000) regions is ignored */
}

/* ============================================================
 * Public MMIO API (called from VEH or MMIO hook)
 * addr is offset from APU base (0xFE800000)
 * ============================================================ */

uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size)
{
    if (!d) return 0;
    if (addr >= 0x20000 && addr < 0x30000) {
        return mcpx_apu_vp_read(d, addr - 0x20000, size);
    } else if (addr < 0x20000) {
        return mcpx_apu_read(d, (hwaddr)addr, size);
    }
    return 0;
}

void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size)
{
    if (!d) return;
    mcpx_apu_dispatch_mmio(d, (hwaddr)addr, val, size, true);
}

/* ============================================================
 * APU Test Tone - Direct waveOut sine generator
 *
 * Bypasses the VP pipeline entirely and writes a 440Hz sine wave
 * directly to the monitor frame_buf. This verifies that waveOut
 * output works correctly.
 * ============================================================ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mcpx_apu_play_test_tone(MCPXAPUState *d)
{
    if (!d) {
        fprintf(stderr, "[APU-TEST] No APU state\n");
        return;
    }

    if (g_test_tone.active) {
        /* Toggle off */
        g_test_tone.active = false;
        fprintf(stderr, "[APU-TEST] Test tone OFF\n");
        return;
    }

    /* 440Hz at 48kHz sample rate */
    g_test_tone.phase = 0.0;
    g_test_tone.phase_inc = 2.0 * M_PI * 440.0 / 48000.0;
    g_test_tone.amplitude = 6000;  /* ~18% of full scale */
    g_test_tone.active = true;

    /* Make sure waveOut is running - enable SECTL and resume APU thread */
    qemu_mutex_lock(&d->lock);
    d->regs[NV_PAPU_SECTL] = NV_PAPU_SECTL_XCNTMODE & ~NV_PAPU_SECTL_XCNTMODE_OFF;
    d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING;
    /* Initialize empty voice lists so VP frame doesn't crash */
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    mcpx_apu_resume(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU-TEST] Test tone ON - 440Hz sine, amplitude=%d\n",
            g_test_tone.amplitude);
}

/* ============================================================
 * Software mixer - mixes DirectSound buffers to waveOut
 *
 * This bypasses the VP hardware voice pipeline entirely.
 * DirectSound buffers register PCM data here, and the APU
 * frame thread mixes them into the monitor frame_buf.
 * ============================================================ */

static void mixer_init(void)
{
    if (g_mixer_initialized) return;
    InitializeCriticalSection(&g_mixer_cs);
    memset(g_mixer_voices, 0, sizeof(g_mixer_voices));
    g_mixer_initialized = true;
}

int apu_mixer_alloc_voice(void)
{
    if (!g_mixer_initialized) mixer_init();
    EnterCriticalSection(&g_mixer_cs);
    for (int i = 0; i < APU_MIXER_MAX_VOICES; i++) {
        if (!g_mixer_voices[i].active && !g_mixer_voices[i].pcm_data) {
            g_mixer_voices[i].volume = 1.0f;
            g_mixer_voices[i].sample_rate = 44100;
            g_mixer_voices[i].num_channels = 2;
            LeaveCriticalSection(&g_mixer_cs);
            return i;
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
    return -1;
}

void apu_mixer_free_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    apu_mixer_stop(slot);
    g_mixer_voices[slot].pcm_data = NULL;
    g_mixer_voices[slot].pcm_bytes = 0;
    g_mixer_voices[slot].play_offset = 0;
    LeaveCriticalSection(&g_mixer_cs);
}

APUMixerVoice *apu_mixer_get_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return NULL;
    return &g_mixer_voices[slot];
}

int apu_mixer_set_position(int slot, uint32_t byte_offset)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return 0;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    uint32_t frame_bytes = v->num_channels * sizeof(int16_t);
    int valid = (v->num_channels == 1 || v->num_channels == 2) &&
                byte_offset < v->pcm_bytes &&
                byte_offset / frame_bytes < v->pcm_bytes / frame_bytes;
    if (valid) v->play_offset = ((uint64_t)(byte_offset / frame_bytes)) << 16;
    LeaveCriticalSection(&g_mixer_cs);
    return valid;
}

void apu_mixer_get_state(int slot, uint32_t *byte_offset, int *active, int *looping)
{
    if (byte_offset) *byte_offset = 0;
    if (active) *active = 0;
    if (looping) *looping = 0;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (byte_offset) *byte_offset = (uint32_t)(v->play_offset >> 16) *
                                    v->num_channels * sizeof(int16_t);
    if (active) *active = v->active;
    if (looping) *looping = v->active && v->looping;
    LeaveCriticalSection(&g_mixer_cs);
}

void apu_mixer_play(int slot, int looping)
{
    if (g_audio_muted) return;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (!v->pcm_data || (v->num_channels != 1 && v->num_channels != 2) ||
        v->pcm_bytes < v->num_channels * sizeof(int16_t)) {
        LeaveCriticalSection(&g_mixer_cs);
        return;
    }
    v->looping = looping;
    if (!v->active) InterlockedIncrement((volatile LONG *)&g_mixer_active_count);
    v->active = 1;

    static int play_log_count = 0;
    if (play_log_count < 20) {
        fprintf(stderr, "[APU-MIX] Play voice %d: %u bytes, %u ch, %u Hz, vol=%.2f, loop=%d\n",
                slot, v->pcm_bytes, v->num_channels, v->sample_rate, v->volume, looping);
        play_log_count++;
    }
    LeaveCriticalSection(&g_mixer_cs);

    /* The frame thread takes the APU lock before the mixer lock. */
    extern MCPXAPUState *g_state;
    if (g_state) {
        qemu_mutex_lock(&g_state->lock);
        g_state->pause_requested = false;
        qemu_cond_signal(&g_state->cond);
        qemu_mutex_unlock(&g_state->lock);
    }
}

void apu_mixer_stop(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    if (g_mixer_voices[slot].active) {
        g_mixer_voices[slot].active = 0;
        InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
    }
    LeaveCriticalSection(&g_mixer_cs);
}

/* Mix all active voices into frame_buf. Called from mcpx_apu_monitor_frame.
 * Keep 16 fractional bits in a wide offset so buffers can exceed 65536 frames. */
static void mixer_render(int16_t frame_buf[][2], int num_samples)
{
    if (!g_mixer_initialized) return;
    EnterCriticalSection(&g_mixer_cs);

    for (int v = 0; v < APU_MIXER_MAX_VOICES; v++) {
        APUMixerVoice *voice = &g_mixer_voices[v];
        if (!voice->active || !voice->pcm_data || voice->pcm_bytes == 0)
            continue;

        uint32_t total_frames = voice->pcm_bytes / sizeof(int16_t);
        if (voice->num_channels == 2) total_frames /= 2;
        if (total_frames == 0) continue;

        /* Fixed-point 16.16 increment per output sample */
        uint64_t inc = ((uint64_t)voice->sample_rate << 16) / 48000;
        uint64_t pos = voice->play_offset;
        uint64_t end = (uint64_t)total_frames << 16;
        float vol = voice->volume;

        for (int i = 0; i < num_samples; i++) {
            uint32_t src_frame = pos >> 16;

            if (src_frame >= total_frames) {
                if (voice->looping) {
                    pos %= end;
                    src_frame = (uint32_t)(pos >> 16);
                } else {
                    pos = 0;
                    voice->active = 0;
                    InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
                    break;
                }
            }

            int32_t left, right;
            if (voice->num_channels >= 2) {
                left  = (int32_t)(voice->pcm_data[src_frame * 2] * vol);
                right = (int32_t)(voice->pcm_data[src_frame * 2 + 1] * vol);
            } else {
                left = right = (int32_t)(voice->pcm_data[src_frame] * vol);
            }

            /* Accumulate (mix) into frame_buf with clamping */
            int32_t mixed_l = frame_buf[i][0] + left;
            int32_t mixed_r = frame_buf[i][1] + right;
            if (mixed_l > 32767) mixed_l = 32767;
            if (mixed_l < -32768) mixed_l = -32768;
            if (mixed_r > 32767) mixed_r = 32767;
            if (mixed_r < -32768) mixed_r = -32768;
            frame_buf[i][0] = (int16_t)mixed_l;
            frame_buf[i][1] = (int16_t)mixed_r;

            pos += inc;
        }

        voice->play_offset = pos;
        uint32_t end_frame = pos >> 16;
        if (end_frame >= total_frames) {
            if (voice->looping) {
                voice->play_offset = pos % end;
            } else if (voice->active) {
                voice->play_offset = 0;
                voice->active = 0;
                InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
            }
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
}
