/* Voice word updates from two threads: none may be lost.
 *
 * The title's VOICE_OFF and VOICE_PAUSE run on its own thread, outside the
 * frame lock, and change bits of a voice's PAR_STATE in guest RAM. The frame
 * thread changes other bits of the same word as it processes the voice (the
 * per-frame NEW_VOICE clear). voice_set_mask used to be a plain
 * read-modify-write, so a method landing between the frame thread's read
 * and write was undone: a lost VOICE_OFF left the voice playing with no
 * idle-voice trap, and DirectSound's buffer teardown spun until its 500 ms
 * watchdog forced the voice off (BLiNX 2's 0.5 s and 1 s freezes on 5-1).
 *
 * A zeroed buffer stands in for RAM, every xa2_* is faked, and the APU's own
 * frame thread stays idle (pause_requested), so the "frame thread" here is a
 * test thread that runs mcpx_apu_vp_frame under d->lock, as se_frame does.
 * VPVADDR puts the voice array in the fake RAM; voice V is the only voice in
 * the 2D list and loops over a silent buffer, so it never ends by itself.
 * FECTL stays zero, never TRAPPED, and the idle-voice trap is not enabled. */
#include "apu_state.h"
#include "apu.h"
#include "apu_xaudio2.h"
#include "apu_regs.h"
#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

/* ---- fake device: every xa2_* symbol, so apu_xaudio2.o is not linked ---- */
int  xa2_init(void)            { return 1; }
void xa2_shutdown(void)        {}
int  xa2_is_active(void)       { return 0; }
int  xa2_get_buffer_size(void) { return 512; }
int  xa2_get_buffer_count(void) { return 4; }
unsigned xa2_underruns(void)   { return 0; }
uint64_t xa2_samples_played(void) { return 0; }
int  xa2_queued_samples(void)  { return 0; }
void xa2_expect_drain(void)    {}
int  xa2_submit_samples(const int16_t *s, int n) { (void)s; (void)n; return 1; }

#define FAKE_RAM   (256u << 10)
#define VOICES     0x00030000u      /* 256 voices x NV_PAVS_SIZE, 32 KB */
#define VP_BASE    0x20000u
#define SGE        0x00038000u      /* one-page scatter-gather table */
#define SAMPLES    0x00039000u      /* the silent page it maps */
#define NOTIFIERS  0x0003A000u      /* VOICE_OFF's completion notifiers */
#define V          5                /* the voice under test */
#define ANTE       7                /* VOICE_ON's antecedent, in no list */

static int failures;
static MCPXAPUState *d;
static uint8_t *ram;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void method(uint32_t m, uint32_t arg)
{
    mcpx_apu_mmio_write(d, VP_BASE + m, arg, 4);
}

static uint32_t *voice_word(unsigned v, uint32_t offset)
{
    return (uint32_t *)(ram + VOICES + v * NV_PAVS_SIZE + offset);
}

static uint32_t par_state(void)
{
    return qatomic_load32(voice_word(V, NV_PAVS_VOICE_PAR_STATE));
}

/* ---- the frame thread ---- */

static volatile LONG stop_frames;
static volatile long frames;

static DWORD WINAPI frame_thread(LPVOID arg)
{
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    (void)arg;
    while (!stop_frames) {
        qemu_mutex_lock(&d->lock);
        /* VOICE_ON marks a voice new and the frame thread clears the mark
         * on its first frame. Marking it new every frame keeps that clear a
         * real write of PAR_STATE, which is where a method gets lost. */
        qatomic_or(voice_word(V, NV_PAVS_VOICE_PAR_STATE),
                   NV_PAVS_VOICE_PAR_STATE_NEW_VOICE);
        mcpx_apu_vp_frame(d, mixbins);
        qemu_mutex_unlock(&d->lock);
        frames++;
        /* A host mutex is not fair: without a gap the title's VOICE_ON,
         * which takes this lock, would wait out thousands of frames. */
        SwitchToThread();
    }
    return 0;
}

static HANDLE start_frames(void)
{
    stop_frames = 0;
    frames = 0;
    return CreateThread(NULL, 0, frame_thread, NULL, 0, NULL);
}

static void stop(HANDLE h)
{
    stop_frames = 1;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
}

static void setup_voice(uint32_t state)
{
    memset(ram + VOICES, 0, 256 * NV_PAVS_SIZE);
    /* U8 mono, looping over 1000 silent samples: it plays until it is
     * turned off. */
    *voice_word(V, NV_PAVS_VOICE_CFG_FMT) = NV_PAVS_VOICE_CFG_FMT_LOOP;
    *voice_word(V, NV_PAVS_VOICE_PAR_NEXT) = 1000;
    *voice_word(V, NV_PAVS_VOICE_TAR_PITCH_LINK) = 0xFFFF;
    *voice_word(ANTE, NV_PAVS_VOICE_TAR_PITCH_LINK) = 0xFFFF;
    *voice_word(V, NV_PAVS_VOICE_PAR_STATE) = state;
    memset(ram + SGE, 0, 0x2000);
    *(uint32_t *)(ram + SGE) = SAMPLES;
    d->regs[NV_PAPU_VPSGEADDR] = SGE;
    d->regs[NV_PAPU_FENADDR] = NOTIFIERS;
    d->regs[NV_PAPU_TVL2D] = V;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
}

/* ---- case 1: VOICE_PAUSE against the frame thread ---- */

#define RUN_NS (1000ull * 1000 * 1000)   /* each case runs for 1 s */

static int running(uint64_t t0)
{
    return xbox_HostNowNs() - t0 < RUN_NS;
}

static void pause_case(void)
{
    setup_voice(NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    HANDLE h = start_frames();
    while (frames < 10)
        xbox_HostSleepNs(100 * 1000);

    /* Nothing but these writes changes PAUSED, so the bit must read back as
     * the last write left it, now and at the next round. */
    int lost = 0, expect = 0, i;
    uint64_t t0 = xbox_HostNowNs();
    for (i = 0; running(t0); i++) {
        int want = i & 1;
        if (((par_state() & NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0) != expect)
            lost++;
        method(NV1BA0_PIO_VOICE_PAUSE, V | (want ? NV1BA0_PIO_VOICE_PAUSE_ACTION : 0));
        expect = want;
        int got = (par_state() & NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;
        if (got != expect) {
            lost++;
            expect = got;
        }
    }
    stop(h);

    printf("  %d VOICE_PAUSE writes over %ld frames: %d lost\n", i, frames, lost);
    check(lost == 0, "no VOICE_PAUSE is undone by the frame thread");
    check(par_state() & NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE,
          "the voice is still active (PAUSE leaves ACTIVE alone)");
}

/* ---- case 2: VOICE_ON then VOICE_OFF against the frame thread ---- */

static void off_case(void)
{
    setup_voice(0);
    HANDLE h = start_frames();
    while (frames < 10)
        xbox_HostSleepNs(100 * 1000);

    /* INHERIT with ANTE: ON links V after ANTE, whose link to the list end
     * is put back each round, so V stays the 2D list's only voice. OFF,
     * as from the title, never takes the frame lock. */
    method(NV1BA0_PIO_SET_ANTECEDENT_VOICE, ANTE);
    int lost = 0, on_failed = 0, i;
    uint64_t t0 = xbox_HostNowNs();
    for (i = 0; running(t0); i++) {
        *voice_word(ANTE, NV_PAVS_VOICE_TAR_PITCH_LINK) = 0xFFFF;
        method(NV1BA0_PIO_VOICE_ON, V);
        if (!(par_state() & NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
            on_failed++;
        /* Vary where in the frame thread's work the OFF lands. */
        for (volatile int spin = 0; spin < (i % 64) * 8; spin++) {}
        method(NV1BA0_PIO_VOICE_OFF, V);
        if (par_state() & NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)
            lost++;
        else {
            /* A frame already past its read of PAR_STATE writes it back
             * later; look again once the frame in progress is over. */
            qemu_mutex_lock(&d->lock);
            qemu_mutex_unlock(&d->lock);
            if (par_state() & NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)
                lost++;
        }
    }
    stop(h);

    printf("  %d VOICE_ON/VOICE_OFF pairs over %ld frames: %d OFFs lost\n",
           i, frames, lost);
    check(on_failed == 0, "VOICE_ON sets ACTIVE");
    check(lost == 0, "ACTIVE stays 0 after VOICE_OFF");
}

/* ---- case 3: qatomic_cmpxchg32 on disjoint bits ---- */

#define CAS_ROUNDS 1000000

static uint32_t shared_word;
static volatile LONG cas_errors;

static void cas_set(uint32_t mask, uint32_t bits)
{
    uint32_t old = qatomic_load32(&shared_word);
    for (;;) {
        uint32_t seen = qatomic_cmpxchg32(&shared_word, old, (old & ~mask) | bits);
        if (seen == old)
            return;
        old = seen;
    }
}

static DWORD WINAPI cas_thread(LPVOID arg)
{
    /* Each thread owns one bit, sets and clears it, and checks it reads back:
     * the other thread never writes it, so a mismatch is a lost update. */
    uint32_t bit = 1u << (uint32_t)(uintptr_t)arg;
    for (int i = 0; i < CAS_ROUNDS; i++) {
        uint32_t want = (i & 1) ? 0 : bit;
        cas_set(bit, want);
        if ((qatomic_load32(&shared_word) & bit) != want)
            InterlockedIncrement(&cas_errors);
    }
    return 0;
}

static void cas_case(void)
{
    HANDLE a = CreateThread(NULL, 0, cas_thread, (LPVOID)(uintptr_t)3, 0, NULL);
    HANDLE b = CreateThread(NULL, 0, cas_thread, (LPVOID)(uintptr_t)17, 0, NULL);
    WaitForSingleObject(a, INFINITE);
    WaitForSingleObject(b, INFINITE);
    CloseHandle(a);
    CloseHandle(b);
    printf("  %d set/clear rounds each on bits 3 and 17: %ld lost\n",
           CAS_ROUNDS, (long)cas_errors);
    check(cas_errors == 0 && shared_word == 0,
          "qatomic_cmpxchg32 loses neither thread's bit");

    uint32_t w = 0x1234;
    check(qatomic_cmpxchg32(&w, 0x1234, 0xABCD) == 0x1234 && w == 0xABCD,
          "a swap returns the old value and stores the new one");
    check(qatomic_cmpxchg32(&w, 0x1234, 0x5555) == 0xABCD && w == 0xABCD,
          "a failed swap returns what it saw and stores nothing");
}

int main(void)
{
    ram = calloc(1, FAKE_RAM);
    if (!ram) return 1;

    printf("apu_voice_mask:\n");
    d = mcpx_apu_init_standalone(ram);
    if (!d) { printf("  init failed\n"); return 1; }
    d->regs[NV_PAPU_VPVADDR] = VOICES;

    pause_case();
    off_case();
    cas_case();

    mcpx_apu_shutdown(d);
    free(ram);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
