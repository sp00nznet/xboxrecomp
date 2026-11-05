/* NV1BA0_PIO_VOICE_LOCK, and VOICE_ON/RELEASE, as posted writes.
 *
 * voice_lock used to take d->lock, which the APU frame thread holds for the
 * whole of each se_frame, and signal d->cond, which the frame thread's
 * pacing wait sleeps on. A title's VOICE_LOCK therefore waited out the
 * current frame (most of the APU's MMIO handler cost measured under Proton)
 * and woke the pacing wait for nothing. On the MCPX the method is posted
 * into the front end's FIFO and the CPU never waits.
 *
 * The model comes up as tests/apu_lifecycle brings it up: a small zeroed
 * buffer stands in for RAM, every xa2_* is faked, and the frame thread
 * idles on d->cond after init (pause_requested), so it is a second waiter
 * on the cond case 3 watches. VPVADDR puts the voice array in that RAM.
 *
 * Cases 4-6 cover the other half: the mix (mcpx_apu_vp_frame, run here on a
 * test thread with d->lock held, as the frame thread runs it) holds a frame
 * while a queued voice is locked, lets go on the unlock, gives up after its
 * bound, and the title's methods never wait on it; case 7 shows the
 * unlock's wake, not the poll, is what ends a hold. */
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

static int failures;
static MCPXAPUState *d;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static double now_ms(void)
{
    return (double)xbox_HostNowNs() / 1e6;
}

static void method(uint32_t m, uint32_t arg)
{
    mcpx_apu_mmio_write(d, VP_BASE + m, arg, 4);
}

static int locked(unsigned v)
{
    return (int)((d->vp.voice_locked[v / 64] >> (v % 64)) & 1);
}

/* ---- case 1: no method waits while the frame lock is held ---- */

static volatile LONG holder_has_lock;

static DWORD WINAPI hold_lock(LPVOID arg)
{
    (void)arg;
    qemu_mutex_lock(&d->lock);
    holder_has_lock = 1;
    xbox_HostSleepNs(200ull * 1000 * 1000);
    qemu_mutex_unlock(&d->lock);
    return 0;
}

/* Returns the method's duration in ms. */
static double timed(uint32_t m, uint32_t arg)
{
    double t0 = now_ms();
    method(m, arg);
    return now_ms() - t0;
}

static void no_wait_case(void)
{
    HANDLE h = CreateThread(NULL, 0, hold_lock, NULL, 0, NULL);
    double worst = 0, t;

    while (!holder_has_lock)
        xbox_HostSleepNs(100 * 1000);

    /* Voice 9 is the title's: it locks it and keeps it locked. VOICE_ON on
     * voice 7 then takes and drops 7's bit only. INHERIT with antecedent
     * voice 0 keeps the list registers out of it. */
    method(NV1BA0_PIO_SET_ANTECEDENT_VOICE, 0);
    t = timed(NV1BA0_PIO_SET_CURRENT_VOICE, 9);      if (t > worst) worst = t;
    t = timed(NV1BA0_PIO_VOICE_LOCK, 1);             if (t > worst) worst = t;
    check(locked(9), "VOICE_LOCK(1) sets the current voice's bit");
    t = timed(NV1BA0_PIO_SET_CURRENT_VOICE, 7);      if (t > worst) worst = t;
    t = timed(NV1BA0_PIO_VOICE_LOCK, 1);             if (t > worst) worst = t;
    check(locked(7), "VOICE_LOCK(1) on a second voice sets its bit");
    t = timed(NV1BA0_PIO_VOICE_LOCK, 0);             if (t > worst) worst = t;
    check(!locked(7) && locked(9), "VOICE_LOCK(0) clears only the current voice's bit");
    t = timed(NV1BA0_PIO_VOICE_ON, 7);               if (t > worst) worst = t;
    check(locked(9) && !locked(7), "VOICE_ON leaves the title's lock and clears its own");
    t = timed(NV1BA0_PIO_VOICE_RELEASE, 7);          if (t > worst) worst = t;
    check(locked(9) && !locked(7), "VOICE_RELEASE leaves the title's lock and clears its own");
    t = timed(NV1BA0_PIO_SET_CURRENT_VOICE, 9);      if (t > worst) worst = t;
    t = timed(NV1BA0_PIO_VOICE_LOCK, 0);             if (t > worst) worst = t;
    check(!locked(9), "VOICE_LOCK(0) clears the title's bit");

    printf("  slowest method with d->lock held elsewhere: %.3f ms\n", worst);
    check(worst < 20.0, "no method waits for the frame lock (each < 20 ms)");

    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
}

/* ---- case 2: two voices share a word; no update is lost ----
 *
 * Run on voices 3 and 5 and again on 35 and 37: the high half of the 64-bit
 * word is what a 32-bit interlocked op (qatomic_or on a long under MSVC)
 * would drop. */

#define TOGGLES 1000000

static DWORD WINAPI toggle_voice(LPVOID arg)
{
    /* SET_CURRENT_VOICE is a shared register, so each thread uses VOICE_ON's
     * own take-and-drop on its handle: the same read-modify-write as
     * VOICE_LOCK, on a voice the thread names itself. */
    uint32_t v = (uint32_t)(uintptr_t)arg;
    for (int i = 0; i < TOGGLES; i++)
        method(NV1BA0_PIO_VOICE_RELEASE, v);
    return 0;
}

static void lost_update_case(unsigned va, unsigned vb)
{
    char what[96];

    /* Each RELEASE sets its voice's bit and clears it again. A racing RMW on
     * the shared word would leave the other voice's bit stuck set (or
     * cleared under a real VOICE_LOCK, which the final check covers). */
    HANDLE a = CreateThread(NULL, 0, toggle_voice, (LPVOID)(uintptr_t)va, 0, NULL);
    HANDLE b = CreateThread(NULL, 0, toggle_voice, (LPVOID)(uintptr_t)vb, 0, NULL);
    WaitForSingleObject(a, INFINITE);
    WaitForSingleObject(b, INFINITE);
    CloseHandle(a);
    CloseHandle(b);
    snprintf(what, sizeof(what), "1M take/drop each on voices %u and %u: both bits clear",
             va, vb);
    check(d->vp.voice_locked[0] == 0, what);

    method(NV1BA0_PIO_SET_CURRENT_VOICE, va);
    method(NV1BA0_PIO_VOICE_LOCK, 1);
    method(NV1BA0_PIO_SET_CURRENT_VOICE, vb);
    method(NV1BA0_PIO_VOICE_LOCK, 1);
    snprintf(what, sizeof(what), "a final lock of each leaves exactly bits %u and %u set",
             va, vb);
    check(d->vp.voice_locked[0] == ((1ull << va) | (1ull << vb)), what);
    method(NV1BA0_PIO_VOICE_LOCK, 0);
    method(NV1BA0_PIO_SET_CURRENT_VOICE, va);
    method(NV1BA0_PIO_VOICE_LOCK, 0);
    check(d->vp.voice_locked[0] == 0, "and unlocking both clears the word");
}

/* VOICE_LOCK on voice 40 sets and clears bit 40 of word 0, and nothing
 * else: a 32-bit op would miss the bit or touch bit 8. */
static void high_bit_case(void)
{
    method(NV1BA0_PIO_SET_CURRENT_VOICE, 40);
    method(NV1BA0_PIO_VOICE_LOCK, 1);
    check(d->vp.voice_locked[0] == (1ull << 40) && locked(40),
          "VOICE_LOCK(1) on voice 40 sets bit 40 of word 0 only");
    method(NV1BA0_PIO_VOICE_LOCK, 0);
    check(d->vp.voice_locked[0] == 0 && !locked(40),
          "VOICE_LOCK(0) on voice 40 clears it");
}

/* ---- case 3: no method wakes a waiter on d->cond ---- */

static volatile LONG waiter_ready, waiter_done;
static int early_returns;

static DWORD WINAPI cond_waiter(LPVOID arg)
{
    (void)arg;
    qemu_mutex_lock(&d->lock);
    waiter_ready = 1;
    double start = now_ms();
    for (;;) {
        double left = 300.0 - (now_ms() - start);
        if (left <= 0)
            break;
        qemu_cond_timedwait(&d->cond, &d->lock, (int)left + 1);
        /* Woken before the time the wait was given: a signal, or a
         * spurious wakeup. */
        if (now_ms() - start < 300.0 - 1.0)
            early_returns++;
    }
    qemu_mutex_unlock(&d->lock);
    waiter_done = 1;
    return 0;
}

static void no_wake_case(void)
{
    HANDLE h = CreateThread(NULL, 0, cond_waiter, NULL, 0, NULL);
    while (!waiter_ready)
        xbox_HostSleepNs(100 * 1000);

    method(NV1BA0_PIO_SET_CURRENT_VOICE, 11);
    for (int i = 0; i < 1000 && !waiter_done; i++) {
        method(NV1BA0_PIO_VOICE_LOCK, i & 1 ? 0 : 1);
        xbox_HostSleepNs(100 * 1000);
    }
    method(NV1BA0_PIO_VOICE_LOCK, 0);

    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    printf("  early returns from a 300 ms wait on d->cond: %d\n", early_returns);
    check(early_returns < 10, "VOICE_LOCK does not signal d->cond (< 10 early returns)");
}

/* ---- cases 4-6: the mix holds while a queued voice is locked ---- */

#define HELD_VOICE  20
#define HOLD_MS     2.0     /* VP_HOLD_MAX_US in apu_vp.c */

static uint8_t *ram;

static void voice_reg(unsigned v, uint32_t reg, uint32_t value)
{
    memcpy(ram + VOICES + v * NV_PAVS_SIZE + reg, &value, 4);
}

/* List 0 holds voice HELD_VOICE alone, active and paused, so voice_process
 * only marks it in g_dbg; lists 1 and 2 are empty. */
static void one_voice_list(void)
{
    d->regs[NV_PAPU_TVL2D] = HELD_VOICE;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    voice_reg(HELD_VOICE, NV_PAVS_VOICE_TAR_PITCH_LINK, 0xFFFF);
    voice_reg(HELD_VOICE, NV_PAVS_VOICE_PAR_STATE,
              NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE | NV_PAVS_VOICE_PAR_STATE_PAUSED);
}

static volatile LONG frame_done;
static double frame_start_ms, frame_end_ms;

static DWORD WINAPI run_frame(LPVOID arg)
{
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    (void)arg;
    qemu_mutex_lock(&d->lock);
    frame_start_ms = now_ms();
    mcpx_apu_vp_frame(d, mixbins);
    frame_end_ms = now_ms();
    qemu_mutex_unlock(&d->lock);
    frame_done = 1;
    return 0;
}

/* One frame on its own thread; returns its length in ms. */
static double frame_on_thread(void)
{
    frame_done = 0;
    g_dbg.vp.v[HELD_VOICE].active = false;
    HANDLE h = CreateThread(NULL, 0, run_frame, NULL, 0, NULL);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return frame_end_ms - frame_start_ms;
}

static void lock_voice(unsigned v, int on)
{
    method(NV1BA0_PIO_SET_CURRENT_VOICE, v);
    method(NV1BA0_PIO_VOICE_LOCK, (uint32_t)on);
}

/* Case 4 and 6: the frame waits while the voice is locked, without
 * processing it; the title's methods meanwhile return at once; the unlock
 * lets the frame go well before the bound. */
static void hold_and_release_case(void)
{
    uint64_t holds = d->vp.hold_count, timeouts = d->vp.hold_timeouts;
    double worst = 0, t, t_unlock;
    int waited = 0;

    lock_voice(HELD_VOICE, 1);
    frame_done = 0;
    g_dbg.vp.v[HELD_VOICE].active = false;
    HANDLE h = CreateThread(NULL, 0, run_frame, NULL, 0, NULL);
    double t0 = now_ms();
    while (!qatomic_load64(&d->vp.hold_waiting) && !frame_done && now_ms() - t0 < 1000)
        ;
    waited = qatomic_load64(&d->vp.hold_waiting) != 0;
    check(waited && !frame_done, "the frame waits while a queued voice is locked");
    check(!g_dbg.vp.v[HELD_VOICE].active, "and has not processed the locked voice");

    /* Case 6: VOICE_LOCK on another voice and VOICE_ON/RELEASE, while the
     * hold waits, each return at once. */
    method(NV1BA0_PIO_SET_ANTECEDENT_VOICE, 0);
    for (int i = 0; i < 50; i++) {
        t = timed(NV1BA0_PIO_SET_CURRENT_VOICE, 30); if (t > worst) worst = t;
        t = timed(NV1BA0_PIO_VOICE_LOCK, 1);         if (t > worst) worst = t;
        t = timed(NV1BA0_PIO_VOICE_LOCK, 0);         if (t > worst) worst = t;
    }
    method(NV1BA0_PIO_SET_CURRENT_VOICE, HELD_VOICE);
    t_unlock = now_ms();
    t = timed(NV1BA0_PIO_VOICE_LOCK, 0);             if (t > worst) worst = t;
    printf("  slowest title method while the mix holds: %.3f ms\n", worst);
    check(worst < 5.0, "no title method waits on the hold (each < 5 ms)");

    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    printf("  frame ended %.3f ms after the unlock (bound %.0f ms)\n",
           frame_end_ms - t_unlock, HOLD_MS);
    check(frame_end_ms >= t_unlock, "the frame ends only after the unlock");
    check(g_dbg.vp.v[HELD_VOICE].active, "and then processes the voice");
    check(d->vp.hold_count == holds + 1 && d->vp.hold_timeouts == timeouts,
          "one hold, ended by the unlock, not the bound");
    check(!d->vp.hold_waiting, "hold_waiting is clear after the hold");
}

/* Case 7: the unlock's wake, not the 1 ms poll, ends the hold. The test
 * unlocks once the hold has had time to go to sleep, and counts holds that
 * hold_wakes says a wake cut short inside the first poll. The idle frame
 * thread also waits on d->cond (with a plain signal it took every wake on
 * macOS: 0 of 20), and a busy host can delay the hold past its poll, so
 * this asks for most of WAKE_REPS, not all. */
#define WAKE_REPS 20

static void wake_case(void)
{
    uint64_t wakes = d->vp.hold_wakes, holds = d->vp.hold_count;

    for (int i = 0; i < WAKE_REPS; i++) {
        lock_voice(HELD_VOICE, 1);
        frame_done = 0;
        HANDLE h = CreateThread(NULL, 0, run_frame, NULL, 0, NULL);
        double t0 = now_ms();
        while (!qatomic_load64(&d->vp.hold_waiting) && !frame_done && now_ms() - t0 < 1000)
            ;
        /* Past the re-check after hold_waiting is set, into the wait. */
        t0 = now_ms();
        while (now_ms() - t0 < 0.3)
            ;
        lock_voice(HELD_VOICE, 0);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    printf("  holds ended by the unlock's wake: %llu of %d\n",
           (unsigned long long)(d->vp.hold_wakes - wakes), WAKE_REPS);
    check(d->vp.hold_count == holds + WAKE_REPS, "every repetition held");
    check(d->vp.hold_wakes - wakes >= WAKE_REPS / 2,
          "the unlock wakes the hold before its poll runs out (most reps)");
}

/* Case 5: a voice the title never unlocks holds one frame for the bound and
 * no frame after it, until it is unlocked and locked again. */
static void bound_case(void)
{
    uint64_t holds = d->vp.hold_count, timeouts = d->vp.hold_timeouts;
    double t;

    lock_voice(HELD_VOICE, 1);
    t = frame_on_thread();
    printf("  frame with the voice left locked: %.3f ms\n", t);
    check(t >= HOLD_MS - 0.1 && t < HOLD_MS + 50.0,
          "a voice left locked holds the frame for the bound");
    check(d->vp.hold_timeouts == timeouts + 1 && g_dbg.vp.v[HELD_VOICE].active,
          "the bound runs out and the voice is processed as it stands");

    t = frame_on_thread();
    check(d->vp.hold_count == holds + 1 && g_dbg.vp.v[HELD_VOICE].active,
          "the next frame does not hold for the same lock");
    printf("  next frame: %.3f ms\n", t);

    lock_voice(HELD_VOICE, 0);
    frame_on_thread();
    check(d->vp.hold_count == holds + 1, "an unlocked voice is not held");
    lock_voice(HELD_VOICE, 1);
    frame_on_thread();
    check(d->vp.hold_count == holds + 2 && d->vp.hold_timeouts == timeouts + 2,
          "a fresh lock after the unlock is held again");
    lock_voice(HELD_VOICE, 0);

    /* A locked voice that is not queued (on no list) never holds. */
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;
    lock_voice(HELD_VOICE, 1);
    t = frame_on_thread();
    check(d->vp.hold_count == holds + 2, "a locked voice on no list does not hold");
    lock_voice(HELD_VOICE, 0);
    d->regs[NV_PAPU_TVL2D] = HELD_VOICE;
}

int main(void)
{
    ram = calloc(1, FAKE_RAM);
    if (!ram) return 1;

    printf("apu_voice_lock:\n");
    d = mcpx_apu_init_standalone(ram);
    if (!d) { printf("  init failed\n"); return 1; }
    d->regs[NV_PAPU_VPVADDR] = VOICES;

    no_wait_case();
    lost_update_case(3, 5);
    lost_update_case(35, 37);
    high_bit_case();
    no_wake_case();
    one_voice_list();
    hold_and_release_case();
    wake_case();
    bound_case();

    mcpx_apu_shutdown(d);
    free(ram);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
