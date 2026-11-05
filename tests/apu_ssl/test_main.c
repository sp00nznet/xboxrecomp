/* The sequencer's sample-limit interrupt fires once per sequencer start.
 *
 * DirectSound programs XGSCNT = 0 and SSL = 0x800 and needs one SSLINTSTS to
 * finish its initialisation (Black). Its service routine then restarts the GP
 * and EP, stalls 10 ms at DISPATCH and programs XGSCNT and SSL again. On
 * hardware the DSP code keeps the limit from being reached; our DSPs are
 * stubs, so a limit re-armed by that restart fired every 2048 samples and the
 * restart ran 23 times a second (JSRF). This checks the stub's tick fires
 * once at the limit, not again after the title's restart (JSRF's, with no
 * SECTL write, and B3's, which switches only the counter off and on) or a
 * running mode written while running, again after the sequencer is stopped
 * whole (SECTL 0) and started, and never with no limit or the counter
 * off. With an emulated GP it fires only when the GP's lag reaches SSL
 * samples: never while the GP keeps pace, once at the limit for a GP that
 * runs long, later when early frames pay the lag back, and never from lag
 * the GP built while the counter was off or before a restart.
 *
 * mcpx_apu_ssl_tick is the frame thread's per-frame step; an ISTS write of 0
 * (acks nothing) stands in for the frame thread turning set_irq into the
 * line. */
#include "apu_state.h"
#include "apu.h"
#include "apu_regs.h"

#include <stdio.h>
#include <stdlib.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

/* What DirectSound writes: SSL 2048 samples, the counter running. */
#define SSL_SAMPLES 0x800u
#define SECTL_RUN   (1u << 3)
/* The values B3's DirectSound writes: 0xF runs, 0x7 is the counter off with
 * the rest of SECTL kept. */
#define SECTL_ON      0xFu
#define SECTL_CNT_OFF 0x7u

static uint32_t reg(MCPXAPUState *d, uint32_t r) { return d->regs[r]; }
static int ssl_status(MCPXAPUState *d) { return !!(reg(d, NV_PAPU_ISTS) & NV_PAPU_ISTS_SSLINTSTS); }
static int line(MCPXAPUState *d) { return !!(reg(d, NV_PAPU_ISTS) & NV_PAPU_ISTS_GINTSTS); }
static void settle(MCPXAPUState *d) { mcpx_apu_mmio_write(d, NV_PAPU_ISTS, 0, 4); }
static void ack(MCPXAPUState *d) { mcpx_apu_mmio_write(d, NV_PAPU_ISTS, 0xFFFFFFFFu, 4); }

/* The title's sequencer setup, as DirectSound runs it on every restart. */
static void program(MCPXAPUState *d)
{
    mcpx_apu_mmio_write(d, NV_PAPU_XGSCNT, 0, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_SSL, SSL_SAMPLES, 4);
}

static void ticks(MCPXAPUState *d, int n)
{
    for (int i = 0; i < n; i++)
        mcpx_apu_ssl_tick(d);
    settle(d);
}

int main(void)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    if (!d) return 2;
    qemu_mutex_init(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);
    printf("apu_ssl: running\n");

    const int frames = SSL_SAMPLES / NUM_SAMPLES_PER_FRAME;
    mcpx_apu_mmio_write(d, NV_PAPU_IEN, 0xD9, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, SECTL_RUN, 4);
    program(d);

    /* 1. One frame short of the limit: nothing. At the limit: the status
     *    latches and the line rises. */
    ticks(d, frames - 1);
    CHECK("short of the limit: no SSLINTSTS", !ssl_status(d));
    ticks(d, 1);
    CHECK("at the limit: SSLINTSTS", ssl_status(d));
    CHECK("at the limit: line asserted", line(d));
    CHECK("XGSCNT reads the samples counted",
          mcpx_apu_mmio_read(d, NV_PAPU_XGSCNT, 4) == SSL_SAMPLES);

    /* 2. The service routine acks and restarts the sequencer the way it
     *    does after every SSL interrupt (JSRF's and Black's shape: no SECTL
     *    write). The stub DSPs never hold the limit off, so a re-armed tick
     *    would fire again here, 23 times a second. */
    ack(d);
    for (int restart = 0; restart < 4; restart++) {
        program(d);
        ticks(d, frames * 2);
    }
    CHECK("after the title's restarts: no SSLINTSTS", !ssl_status(d));
    CHECK("after the title's restarts: line low", !line(d));

    /* 2b. A running mode written while running is not a new start: only an
     *     off write re-arms. */
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, SECTL_RUN, 4);
    program(d);
    ticks(d, frames * 2);
    CHECK("running mode rewritten: no SSLINTSTS", !ssl_status(d));

    /* 2c. B3's, SSX 3's and BLiNX 2's restart switches only the counter
     *     off and on again (SECTL 0xF, 0x7, program, 0xF). That is not a
     *     sequencer reset: no second interrupt. */
    for (int restart = 0; restart < 4; restart++) {
        mcpx_apu_mmio_write(d, NV_PAPU_SECTL, SECTL_ON, 4);
        mcpx_apu_mmio_write(d, NV_PAPU_SECTL, SECTL_CNT_OFF, 4);
        program(d);
        mcpx_apu_mmio_write(d, NV_PAPU_SECTL, SECTL_ON, 4);
        ticks(d, frames * 2);
    }
    CHECK("counter-only restarts: no SSLINTSTS", !ssl_status(d));
    CHECK("counter-only restarts: line low", !line(d));

    /* 3. DirectSound's init stops the sequencer whole before it programs
     *    it (SECTL 0, program, 0x7, 0xF): a new start, and the limit fires
     *    again. */
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, 0, 4);
    program(d);
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, SECTL_CNT_OFF, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, SECTL_ON, 4);
    ticks(d, frames);
    CHECK("new start: SSLINTSTS again", ssl_status(d));
    ack(d);
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, SECTL_CNT_OFF, 4);
    program(d);
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, SECTL_ON, 4);
    ticks(d, frames * 2);
    CHECK("its recovery after: no SSLINTSTS", !ssl_status(d));

    /* 4. Counter off: no tick however long it runs. */
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, 0, 4);
    program(d);
    ticks(d, frames * 4);
    CHECK("counter off: no SSLINTSTS", !ssl_status(d));

    /* 5. No limit programmed: no tick either. */
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, SECTL_RUN, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_XGSCNT, 0, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_SSL, 0, 4);
    ticks(d, frames * 4);
    CHECK("SSL 0: no SSLINTSTS", !ssl_status(d));

    /* 6. An emulated GP that keeps pace (JSRF's: 42k cycles a frame) never
     *    raises it, while XGSCNT still counts. The interrupt would make
     *    DirectSound restart the GP from a scratch image its effect commits
     *    have changed. Clearing the flag brings the stub's tick
     *    back. */
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, 0, 4);
    d->gp_emulated = true;
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, SECTL_RUN, 4);
    program(d);
    for (int i = 0; i < frames * 2; i++) {
        mcpx_apu_gp_frame_ran(d, 42000);
        mcpx_apu_ssl_tick(d);
    }
    settle(d);
    CHECK("GP keeping pace: no SSLINTSTS", !ssl_status(d));
    CHECK("GP keeping pace: XGSCNT counts",
          mcpx_apu_mmio_read(d, NV_PAPU_XGSCNT, 4) == 2 * SSL_SAMPLES);
    d->gp_emulated = false;
    ticks(d, 1);
    CHECK("stub GP again: SSLINTSTS", ssl_status(d));
    ack(d);

    /* 7. A GP whose frames take twice a frame loses one frame of pace
     *    each: the bit is raised the frame that reaches SSL samples,
     *    and the lag starts again. */
    const uint32_t frame = MCPX_APU_GP_FRAME_CYCLES;
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, 0, 4);
    d->gp_emulated = true;
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, SECTL_RUN, 4);
    program(d);
    for (int i = 0; i < frames - 1; i++) {
        mcpx_apu_gp_frame_ran(d, 2 * frame);
        mcpx_apu_ssl_tick(d);
    }
    settle(d);
    CHECK("GP lag short of SSL: no SSLINTSTS", !ssl_status(d));
    mcpx_apu_gp_frame_ran(d, 2 * frame);
    ticks(d, 1);
    CHECK("GP lag at SSL: SSLINTSTS", ssl_status(d));
    CHECK("GP lag at SSL: line asserted", line(d));
    CHECK("GP lag at SSL: lag cleared", d->gp_lag_cycles == 0);
    ack(d);

    /* 7b. Early frames pay a lag back: half the limit late, then as much
     *     slack, then the other half late again is still short of it. */
    for (int i = 0; i < frames / 2; i++)
        mcpx_apu_gp_frame_ran(d, 2 * frame);
    for (int i = 0; i < frames / 2; i++)
        mcpx_apu_gp_frame_ran(d, 0);
    CHECK("GP slack pays the lag back", d->gp_lag_cycles == 0);
    for (int i = 0; i < frames - 1; i++)
        mcpx_apu_gp_frame_ran(d, 2 * frame);
    ticks(d, 1);
    CHECK("paid-back lag: no SSLINTSTS", !ssl_status(d));

    /* 7c. With the counter off the lag does not build: however late the
     *     GP gets meanwhile, the bit stays down, and when the counter runs
     *     again one late frame is still one frame of lag. */
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, 0, 4);
    for (int i = 0; i < frames * 2; i++) {
        mcpx_apu_gp_frame_ran(d, 2 * frame);
        mcpx_apu_ssl_tick(d);
    }
    settle(d);
    CHECK("GP lag, counter off: no SSLINTSTS", !ssl_status(d));
    CHECK("GP lag, counter off: lag cleared", d->gp_lag_cycles == 0);
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, SECTL_RUN, 4);
    program(d);
    mcpx_apu_gp_frame_ran(d, 2 * frame);
    ticks(d, 1);
    CHECK("counter on after off-time lag: no SSLINTSTS", !ssl_status(d));

    /* 7d. A restart (B3's and SSX 3's: SECTL 0x7 then 0xF, with no tick
     *     between) starts the lag again: the late frames of the run
     *     before it do not fire the new one at once. */
    for (int i = 0; i < frames - 2; i++) {
        mcpx_apu_gp_frame_ran(d, 2 * frame);
        mcpx_apu_ssl_tick(d);
    }
    settle(d);
    CHECK("lag one frame short of SSL: no SSLINTSTS", !ssl_status(d));
    mcpx_apu_gp_frame_ran(d, 2 * frame);
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, SECTL_CNT_OFF, 4);
    program(d);
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, SECTL_ON, 4);
    ticks(d, 1);
    CHECK("restart after a lag at SSL: no SSLINTSTS", !ssl_status(d));
    mcpx_apu_gp_frame_ran(d, 2 * frame);
    ticks(d, 1);
    CHECK("restart: lag counts the new run alone",
          !ssl_status(d) && d->gp_lag_cycles == frame);
    d->gp_emulated = false;

    if (failures) { printf("apu_ssl: %d failure(s)\n", failures); return 1; }
    printf("apu_ssl: all passed\n");
    return 0;
}
