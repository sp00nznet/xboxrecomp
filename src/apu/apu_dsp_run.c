/*
 * MCPX APU DSP: the GP and EP on the APU (RECOMP_APU_DSP=1).
 *
 * Built only with the dsp56300 JIT (XBOXRECOMP_APU_DSP), which also defines
 * XBOXRECOMP_APU_DSP_DEVICE so the key table lets RECOMP_APU_DSP=1 through.
 * apu_dsp_emu.c is one core; this file is what the APU does with the two of
 * them each frame, as xemu's mcpx_apu_dsp_frame and gp_ep.c do it: hand the
 * GP the VP's mixbins, run it, take the stereo output from its mixbuf, and
 * run the EP once an EP period. The system memory side (scatter-gather
 * scratch, the circular FIFOs) follows gp_ep.c.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 espes
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
 */
#include "apu_state.h"
#include "apu_regs.h"
#include "apu_dsp_emu.h"
#include "apu_dsp_warm.h"
#include "apu_lockstat.h"
#include "fpconv.h"
#include "recomp_env.h"
#include "../kernel/xbox_memory_layout.h"
#include "../kernel/guest_det_device.h"

#include <stdio.h>
#include <string.h>

/* One APU frame is MCPX_APU_GP_FRAME_CYCLES of a DSP's clock (apu_state.h).
 * A program gets twice that before the frame gives up on it: one that never
 * halts (waiting on something this model lacks) must not stall the frame
 * thread. The EP's frame is an EP period, eight APU frames. */
#define GP_FRAME_CYCLES MCPX_APU_GP_FRAME_CYCLES
#define GP_CAP          (2u * GP_FRAME_CYCLES)
#define EP_CAP          (2u * 8u * GP_FRAME_CYCLES)

/* The GP's stereo output: its mixbuf bins 0 and 1 (xemu's monitor point
 * GP). The EP's AC-3 and surround output is not used yet: 6-channel output
 * waits for the SDL3 move (apu-dsp-emulation task 3.3). */
#define GP_OUT_LEFT  APU_DSP_MIXBUF_BASE
#define GP_OUT_RIGHT (APU_DSP_MIXBUF_BASE + NUM_SAMPLES_PER_FRAME)

/* After a bootstrap a core's first frames compile its whole program
 * (apu_dsp_warm.h): on the Deck the GP's first frame took 522 ms and the EP's
 * first burst 16.6 ms, each enough to starve the audio device run on the APU
 * thread. For about a second after each bootstrap (1500 GP frames, 188 EP
 * periods) a core runs on its worker, and the APU thread waits for each frame
 * up to WARM_WAIT_NS. A GP frame that misses it is silent, and so are the
 * frames that find the worker still busy, which are skipped and counted. The
 * EP's output is not heard (see GP_OUT_LEFT). */
#define GP_WARM_FRAMES  1500u
#define EP_WARM_PERIODS 188u
/* Above the GP's slowest steady frames (1.8 ms on the Deck), well below the
 * device's buffer. */
#define WARM_WAIT_NS    3000000u

#define STATS_FRAMES    1500u      /* one second of APU frames */
#define CAP_LOG_FRAMES  15000u     /* ten seconds */

enum { GP = 0, EP = 1 };
static const char *const k_name[2] = { "GP", "EP" };
static const int k_ids[2] = { GP, EP };

typedef struct RunWindow {
    uint64_t frames, cycles, ns, ns_max;
    uint32_t cycles_max;
} RunWindow;

/* A core's warm-up worker. busy is set by the APU thread when it hands over
 * a frame and cleared by the worker; the core is the worker's alone while it
 * is set. */
typedef struct Worker {
    QemuThread thread;
    QemuMutex mu;
    QemuCond cv;
    bool up, busy, job, quit, done;
    uint32_t cycles;
    uint64_t ns;
    ApuDspWarm warm;
    uint64_t skipped_shown, late_shown;
} Worker;

static struct {
    MCPXAPUState *d;
    ApuDspEmu *emu[2];
    bool created;
    int on;                         /* -1: RECOMP_APU_DSP not read yet */
    int stats;
    uint64_t frames;
    RunWindow win[2];
    uint64_t capped_shown[2];
    uint64_t next_cap_log;
    Worker wk[2];
    ApuDspEmuStats st[2];           /* each core's, as of its last idle frame */
} s = { .on = -1 };

bool mcpx_apu_dsp_device_on(void)
{
    if (s.on < 0) {
        s.on = recomp_env(RENV_APU_DSP) != NULL;
        s.stats = recomp_env_on(RENV_APU_DSP_STATS) > 0;
        /* The title writes GPRST through the MCPX window. Without it the GP
         * never starts, nothing answers the doorbell and audio init waits
         * for ever: keep the stub for the session (apu_dsp.c's ack then
         * takes its default). */
        if (s.on && !xbox_McpxHost()) {
            fprintf(stderr, "[APU DSP] RECOMP_APU_DSP=1 but no MCPX window:"
                    " the stub\n");
            s.on = 0;
        }
    }
    return s.on;
}

/* ---- system memory: scatter-gather pages and circular FIFOs ---- */

/* An SGE table: 8 bytes an entry, the physical page in the first dword. */
static void sg_rw(uint32_t sge_base, uint32_t max_sge, uint8_t *ptr,
                  uint32_t addr, size_t len, bool dir)
{
    uint32_t pe = addr / 4096, off = addr % 4096;
    size_t n = 4096 - off;

    while (len > 0) {
        if (pe > max_sge) {
            /* Both cores' DMA can land here, the GP's on its worker. */
            static long logs;
            if (qatomic_inc(&logs) <= 4)
                fprintf(stderr, "[APU DSP] SGE %u past the table's %u;"
                        " %zu bytes dropped\n", pe, max_sge, len);
            return;
        }
        uint32_t page = *(const uint32_t *)mcpx_apu_phys(sge_base + pe * 8);
        uint8_t *host = mcpx_apu_phys(page + off);
        if (n > len)
            n = len;
        if (dir)
            memcpy(host, ptr, n);
        else
            memcpy(ptr, host, n);
        ptr += n;
        len -= n;
        pe++;
        n = 4096;
        off = 0;
    }
}

static uint32_t reg(uint32_t a)
{
    return qatomic_read(&s.d->regs[a]);
}

static void scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                       bool dir)
{
    int k = *(const int *)opaque;
    if (k == GP)
        sg_rw(reg(NV_PAPU_GPSADDR), reg(NV_PAPU_GPSMAXSGE), ptr, addr, len,
              dir);
    else
        sg_rw(reg(NV_PAPU_EPSADDR), reg(NV_PAPU_EPSMAXSGE), ptr, addr, len,
              dir);
}

static void fifo_rw(void *opaque, uint8_t *ptr, unsigned index, size_t len,
                    bool dir)
{
    int k = *(const int *)opaque;
    uint32_t grp = k == GP ? NV_PAPU_GPOFBASE0 : NV_PAPU_EPOFBASE0;
    uint32_t base, end, cur_reg;

    /* EP FIFO 0 is the AC97 stream: one period, which xemu hands the
     * monitor instead of the title's buffer. Nothing plays it here yet. */
    if (k == EP && dir && index == 0 &&
        len == sizeof(s.d->monitor.frame_buf))
        return;
    if (dir) {
        base = reg(grp + 0x10 * index) & 0xFFFFFF;
        end = reg(grp + 0x04 + 0x10 * index) & 0xFFFFFF;
        cur_reg = grp + 0x08 + 0x10 * index;
    } else {
        grp += NV_PAPU_GPIFBASE0 - NV_PAPU_GPOFBASE0;
        base = reg(grp + 0x10 * index) & 0xFFFFFF;
        end = reg(grp + 0x04 + 0x10 * index) & 0xFFFFFF;
        cur_reg = grp + 0x08 + 0x10 * index;
    }
    if (end <= base)
        return;
    uint32_t cur = reg(cur_reg) & 0xFFFFFF;
    if (cur < base || cur >= end)
        cur = base;
    uint32_t sge = k == GP ? reg(NV_PAPU_GPFADDR) : reg(NV_PAPU_EPFADDR);
    uint32_t max = k == GP ? reg(NV_PAPU_GPFMAXSGE) : reg(NV_PAPU_EPFMAXSGE);
    while (len > 0) {
        size_t n = end - cur;
        if (n > len)
            n = len;
        sg_rw(sge, max, ptr, cur, n, dir);
        ptr += n;
        len -= n;
        cur += (uint32_t)n;
        if (cur >= end)
            cur = base;
    }
    qatomic_set(&s.d->regs[cur_reg], (reg(cur_reg) & ~0xFFFFFFu) | cur);
}

/* ---- the warm-up workers ---- */

static void *worker_main(void *arg)
{
    int k = *(const int *)arg;
    Worker *w = &s.wk[k];
    uint32_t cap = k == GP ? GP_CAP : EP_CAP;

    qemu_mutex_lock(&w->mu);
    for (;;) {
        while (!w->job && !w->quit)
            qemu_cond_wait(&w->cv, &w->mu);
        if (w->quit)
            break;
        w->job = false;
        qemu_mutex_unlock(&w->mu);
        uint64_t t0 = apu_lockstat_now_ns();
        uint32_t cycles = apu_dsp_emu_run_frame(s.emu[k], cap);
        uint64_t ns = apu_lockstat_now_ns() - t0;
        qemu_mutex_lock(&w->mu);
        w->cycles = cycles;
        w->ns = ns;
        w->done = true;
        w->busy = false;
        qemu_cond_signal(&w->cv);
    }
    qemu_mutex_unlock(&w->mu);
    return NULL;
}

static void count(int k, uint32_t cycles, uint64_t ns)
{
    /* Every GP frame that ran, inline or on its worker, is lag accounted on
     * the frame thread. A skipped warm-up frame is the host's delay, not the
     * GP's: it adds none. Nor does a frame stopped at the cap: a GP that
     * never halts waits on something this model lacks, so its cycles say
     * nothing of the program's pace, and counting them would raise the bit
     * every 64 frames and run the title's GP restart for a gap of ours. The
     * cap hit is already counted (capped=) and logged. */
    if (k == GP && cycles < GP_CAP)
        mcpx_apu_gp_frame_ran(s.d, cycles);
    RunWindow *w = &s.win[k];
    w->frames++;
    w->cycles += cycles;
    if (cycles > w->cycles_max)
        w->cycles_max = cycles;
    w->ns += ns;
    if (ns > w->ns_max)
        w->ns_max = ns;
}

/* True when core k is the APU thread's: no frame of it is on the worker,
 * after waiting up to wait_ns for the worker to finish one. Folds a finished
 * worker frame into the stats. */
static bool idle_within(int k, uint64_t wait_ns)
{
    Worker *w = &s.wk[k];
    if (!w->up)
        return true;
    qemu_mutex_lock(&w->mu);
    if (w->busy && wait_ns) {
        uint64_t end = apu_lockstat_now_ns() + wait_ns;
        while (w->busy && apu_lockstat_now_ns() < end)
            qemu_cond_timedwait(&w->cv, &w->mu, 1);
    }
    bool idle_now = !w->busy;
    if (w->done) {
        count(k, w->cycles, w->ns);
        w->done = false;
    }
    qemu_mutex_unlock(&w->mu);
    return idle_now;
}

static bool idle(int k)
{
    return idle_within(k, 0);
}

static void dispatch(int k)
{
    static const char *const thread_name[2] = { "mcpx.apu_gp", "mcpx.apu_ep" };
    Worker *w = &s.wk[k];
    if (!w->up) {
        qemu_mutex_init(&w->mu);
        qemu_cond_init(&w->cv);
        w->up = true;
        qemu_thread_create(&w->thread, thread_name[k], worker_main,
                           (void *)&k_ids[k], QEMU_THREAD_JOINABLE);
    }
    qemu_mutex_lock(&w->mu);
    w->busy = true;
    w->job = true;
    qemu_cond_signal(&w->cv);
    qemu_mutex_unlock(&w->mu);
}

/* Hands core k's frame to its worker and waits for it up to the deadline:
 * true when it finished in time. */
static bool run_on_worker(int k)
{
    dispatch(k);
    bool in_time = idle_within(k, WARM_WAIT_NS);
    apu_dsp_warm_ran(&s.wk[k].warm, in_time);
    return in_time;
}

/* ---- setup ---- */

static void create(MCPXAPUState *d)
{
    uint8_t *mcpx = xbox_McpxHost();

    s.created = true;
    s.d = d;
    for (int k = 0; k < 2; k++) {
        /* The title writes the cores' memory and GPRST/EPRST through these
         * pages untrapped, so the cores run on them (the window is there:
         * mcpx_apu_dsp_device_on keeps the stub without it). */
        ApuDspEmuConfig c = {
            .name = k_name[k], .is_gp = k == GP,
            .window = mcpx + (k == GP ? 0x30000 : 0x50000),
            .opaque = (void *)&k_ids[k],
            .scratch_rw = scratch_rw, .fifo_rw = fifo_rw };
        s.emu[k] = apu_dsp_emu_create(&c);
    }
    if (!s.emu[GP] || !s.emu[EP])
        fprintf(stderr, "[APU DSP] creating the %s failed; its frames are"
                " silent\n", !s.emu[GP] ? "GP" : "EP");
    else
        fprintf(stderr, "[APU DSP] GP and EP on the dsp56300 JIT (cap %u/%u"
                " cycles a frame)\n", GP_CAP, EP_CAP);
}

void mcpx_apu_dsp_run_finalize(void)
{
    for (int k = 0; k < 2; k++) {
        Worker *w = &s.wk[k];
        if (!w->up)
            continue;
        qemu_mutex_lock(&w->mu);
        w->quit = true;
        qemu_cond_signal(&w->cv);
        qemu_mutex_unlock(&w->mu);
        qemu_thread_join(&w->thread);
        qemu_cond_destroy(&w->cv);
        qemu_mutex_destroy(&w->mu);
    }
    for (int k = 0; k < 2; k++)
        if (s.emu[k])
            apu_dsp_emu_destroy(s.emu[k]);
    int on = s.on, stats = s.stats;
    memset(&s, 0, sizeof(s));
    s.on = on;
    s.stats = stats;
}

/* ---- reports ---- */

static void report(void)
{
    /* A core on its worker is writing its stats: keep the last snapshot. */
    for (int k = 0; k < 2; k++)
        if (idle(k))
            apu_dsp_emu_get_stats(s.emu[k], &s.st[k]);
    const ApuDspEmuStats *st = s.st;

    if (s.stats && s.frames % STATS_FRAMES == 0) {
        fprintf(stderr, "[APU DSP] t=%llus",
                (unsigned long long)(s.frames / STATS_FRAMES));
        for (int k = 0; k < 2; k++) {
            const RunWindow *w = &s.win[k];
            fprintf(stderr, " %s frames=%llu cyc avg=%llu max=%u us avg=%.1f"
                    " max=%.1f capped=%llu dma=%llu rej=%llu skipped=%llu"
                    " late=%llu",
                    k_name[k],
                    (unsigned long long)w->frames,
                    (unsigned long long)(w->frames ? w->cycles / w->frames : 0),
                    w->cycles_max,
                    w->frames ? (double)w->ns / (double)w->frames / 1e3 : 0.0,
                    (double)w->ns_max / 1e3,
                    (unsigned long long)st[k].capped,
                    (unsigned long long)st[k].dma_blocks,
                    (unsigned long long)st[k].dma_rejected,
                    (unsigned long long)s.wk[k].warm.skipped,
                    (unsigned long long)s.wk[k].warm.late);
        }
        fprintf(stderr, " ssl=%u lag=%llu", s.d->ssl_raised,
                (unsigned long long)(s.d->gp_lag_cycles
                                     * NUM_SAMPLES_PER_FRAME
                                     / MCPX_APU_GP_FRAME_CYCLES));
        fputc('\n', stderr);
        memset(s.win, 0, sizeof(s.win));
    }

    /* Cap hits and skipped warm-up frames are never silent: whatever the
     * key, a line every ten seconds while there are new ones. */
    if (s.frames < s.next_cap_log)
        return;
    if (st[GP].capped != s.capped_shown[GP] ||
        st[EP].capped != s.capped_shown[EP] ||
        s.wk[GP].warm.skipped != s.wk[GP].skipped_shown ||
        s.wk[EP].warm.skipped != s.wk[EP].skipped_shown ||
        s.wk[GP].warm.late != s.wk[GP].late_shown ||
        s.wk[EP].warm.late != s.wk[EP].late_shown) {
        fprintf(stderr, "[APU DSP] cycle cap hit: GP %llu, EP %llu frames;"
                " warm-up skipped/late: GP %llu/%llu frames,"
                " EP %llu/%llu periods\n",
                (unsigned long long)st[GP].capped,
                (unsigned long long)st[EP].capped,
                (unsigned long long)s.wk[GP].warm.skipped,
                (unsigned long long)s.wk[GP].warm.late,
                (unsigned long long)s.wk[EP].warm.skipped,
                (unsigned long long)s.wk[EP].warm.late);
        for (int k = 0; k < 2; k++) {
            s.capped_shown[k] = st[k].capped;
            s.wk[k].skipped_shown = s.wk[k].warm.skipped;
            s.wk[k].late_shown = s.wk[k].warm.late;
        }
        s.next_cap_log = s.frames + CAP_LOG_FRAMES;
    }
}

/* ---- one APU frame ---- */

static void silence(MCPXAPUState *d, int off)
{
    memset(d->monitor.frame_buf[off], 0,
           sizeof(d->monitor.frame_buf[0]) * NUM_SAMPLES_PER_FRAME);
}

/* The GP's stereo output, 24-bit words whose top 16 bits are the sample,
 * into the monitor's frame slot. */
static void gp_output(MCPXAPUState *d, ApuDspEmu *gp, int off)
{
    uint32_t l[NUM_SAMPLES_PER_FRAME], r[NUM_SAMPLES_PER_FRAME];
    apu_dsp_emu_read_run(gp, APU_DSP_SPACE_X, GP_OUT_LEFT, l,
                         NUM_SAMPLES_PER_FRAME);
    apu_dsp_emu_read_run(gp, APU_DSP_SPACE_X, GP_OUT_RIGHT, r,
                         NUM_SAMPLES_PER_FRAME);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        d->monitor.frame_buf[off + i][0] = (int16_t)(l[i] >> 8);
        d->monitor.frame_buf[off + i][1] = (int16_t)(r[i] >> 8);
    }
}

static void gp_frame(MCPXAPUState *d, ApuDspEmu *gp,
                     float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                     int off, bool out)
{
    Worker *w = &s.wk[GP];
    ApuDspWarmStep step = apu_dsp_warm_step(&w->warm, idle(GP));

    g_dbg.gp.cycles = 0;
    if (step == APU_DSP_WARM_SKIP) {
        if (out)
            silence(d, off);
        return;
    }

    uint32_t words[NUM_MIXBINS * NUM_SAMPLES_PER_FRAME];
    for (int b = 0; b < NUM_MIXBINS; b++)
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            words[b * NUM_SAMPLES_PER_FRAME + i] = float_to_24b(mixbins[b][i]);
    apu_dsp_emu_write_run(gp, APU_DSP_SPACE_X, APU_DSP_MIXBUF_BASE, words,
                          NUM_MIXBINS * NUM_SAMPLES_PER_FRAME);
    if (step == APU_DSP_WARM_WORKER) {
        bool in_time = run_on_worker(GP);
        if (out) {
            if (in_time)
                gp_output(d, gp, off);
            else
                silence(d, off);
        }
        return;
    }
    uint64_t t0 = apu_lockstat_now_ns();
    uint32_t cycles = apu_dsp_emu_run_frame(gp, GP_CAP);
    count(GP, cycles, apu_lockstat_now_ns() - t0);
    g_dbg.gp.cycles = (int)cycles;
    if (out)
        gp_output(d, gp, off);
}

static void ep_frame(ApuDspEmu *ep)
{
    switch (apu_dsp_warm_step(&s.wk[EP].warm, idle(EP))) {
    case APU_DSP_WARM_SKIP:
        break;
    case APU_DSP_WARM_WORKER:
        run_on_worker(EP);
        break;
    case APU_DSP_WARM_INLINE: {
        uint64_t t0 = apu_lockstat_now_ns();
        uint32_t cycles = apu_dsp_emu_run_frame(ep, EP_CAP);
        count(EP, cycles, apu_lockstat_now_ns() - t0);
        g_dbg.ep.cycles = (int)cycles;
        break;
    }
    }
}

void mcpx_apu_dsp_run_frame(MCPXAPUState *d,
                            float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    if (!s.created)
        create(d);
    ApuDspEmu *gp = s.emu[GP], *ep = s.emu[EP];
    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
    bool out = d->monitor.point != MCPX_APU_DEBUG_MON_VP;

    if (!gp || !ep) {
        if (out)
            silence(d, off);
        return;
    }

    /* GPRST and EPRST come through the untrapped window: act on a change,
     * when the core is not on its worker. A bootstrap starts the warm-up.
     * An EPRST write restarts the EP period, as xemu's ep_frame_div = 0. */
    if (idle(GP) && apu_dsp_emu_poll_rst(gp)) {
        d->gp_lag_cycles = 0;
        /* Deterministic mode: no warm-up worker. A frame that missed the
         * worker's deadline would be silent or skipped by host time; the
         * frame thread is not on the device's pace there anyway. */
        if (apu_dsp_emu_running(gp) && !xbox_GuestDetOn())
            apu_dsp_warm_start(&s.wk[GP].warm, GP_WARM_FRAMES);
    }
    if (idle(EP) && apu_dsp_emu_poll_rst(ep)) {
        d->ep_frame_div = 0;
        off = 0;
        if (apu_dsp_emu_running(ep) && !xbox_GuestDetOn())
            apu_dsp_warm_start(&s.wk[EP].warm, EP_WARM_PERIODS);
    }

    /* A running GP program sets the sample limit's pace: its lag, not the
     * stub's once-per-start tick (mcpx_apu_ssl_tick). */
    d->gp_emulated = apu_dsp_emu_running(gp);

    if (d->gp_emulated) {
        gp_frame(d, gp, mixbins, off, out);
    } else {
        /* No GP program, no output: the hardware's speakers hear the GP. */
        g_dbg.gp.cycles = 0;
        if (out)
            silence(d, off);
    }

    g_dbg.ep.cycles = 0;
    if (d->ep_frame_div % 8 == 0 && apu_dsp_emu_running(ep))
        ep_frame(ep);

    s.frames++;
    report();
}
