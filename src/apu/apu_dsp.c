/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * For initial audio, we bypass the DSP entirely:
 * - VP mixbins are passed directly to the EP output
 * - GP effects processing is skipped
 * - The EP just copies mixbin 0/1 (front L/R) to the monitor buffer
 *
 * This gives us basic voice playback without effects. The DSP can be
 * connected later for reverb, EQ, and other processing.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "recomp_env.h"
#include "fpconv.h"
#include "apu_dsp_emu.h"
#include "../kernel/xbox_memory_layout.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * RECOMP_APU_DSP_ACK=auto, the default when the variable is unset, finds the
 * doorbell from what the title programs; "0" or "off" turns the ack off.
 * The XDK DSOUND GP program takes its commands through the first page of
 * the GP's scratch space: a 2 KB command FIFO, then a header whose dword at
 * +0x810 is the doorbell the host writes and the DSP program clears (the
 * DSOUND submit of every XDK title seen spins on <block>+0x810). The
 * scratch pages are what NV_PAPU_GPSADDR's scatter-gather table maps -- 8
 * bytes an entry, the physical page in the first dword, as xemu's apu.c
 * scratch_rw walks it -- so the doorbell is SGE[0] + 0x810 wherever the
 * contiguous allocator put the block. A fixed address moves with allocation
 * order (reserving physical page 0 moved one title's by a page); this does
 * not. Wreckless fits too: its doorbell 0x014F8810 is the first page of the
 * scratch its GPSADDR table (0x01504000) maps, not the table itself, which
 * is where the earlier note here looked.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 */
#define APU_DSP_ACK_MAX 8
#define GP_DOORBELL_OFFSET 0x810u
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;
static int s_dsp_ack_auto;                /* =auto: derive from GPSADDR */
static uint32_t s_dsp_ack_auto_page;      /* the page last reported */
static volatile int s_gp_in_reset;        /* last GPRST write held the GP */

static void dsp_ack_init(void)
{
    const char *spec = recomp_env(RENV_APU_DSP_ACK);
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
#ifdef XBOXRECOMP_APU_DSP_DEVICE
    /* RECOMP_APU_DSP=1 forced the ack to 0 for the GP, but the device kept
     * the stub (no MCPX window): the default ack, or audio init hangs. */
    if (recomp_env(RENV_APU_DSP))
        spec = NULL;
#endif
    if (!spec || !*spec)
        spec = "auto";                    /* default: derive it */
    if (!strcmp(spec, "0") || !strcmp(spec, "off"))
        return;
    if (!strcmp(spec, "auto")) {
        s_dsp_ack_auto = 1;
        fprintf(stderr, "[APU] DSP doorbell ack: auto (GP scratch page 0"
                        " + 0x%X, from GPSADDR)\n", GP_DOORBELL_OFFSET);
        return;
    }
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

/* HOW THE MIXBINS REACH THE STEREO OUTPUT.
 *
 *   =1 (default) the GP's gain staging: speaker bins at unity, crosstalk
 *      bins at half, the I3DL2 send dropped (see mixbin_gain below);
 *   =0 bins 0 and 1 only, the read from before any of this;
 *   =2 every bin at unity, the mixdown JSRF's effects first came back with.
 *
 * Each changes audible output for every title, so each stays one switch
 * away. */
enum { MIXDOWN_TWO_BINS = 0, MIXDOWN_STAGED = 1, MIXDOWN_UNITY = 2 };

static int mcpx_apu_mixdown_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = recomp_env(RENV_APU_MIXDOWN_ALL);
        mode = (e && *e) ? atoi(e) : MIXDOWN_STAGED;
        if (mode < MIXDOWN_TWO_BINS || mode > MIXDOWN_UNITY)
            mode = MIXDOWN_STAGED;
    }
    return mode;
}

/* What the title's GP program does to each bin on the way to the speakers,
 * measured, since the GP itself is a stub here.
 *
 * DirectSound's mixbins are 0-5 the speakers, 6-9 the crosstalk pairs its 3D
 * voices are routed to, 10 the I3DL2 reverb send and 11-31 further effect
 * sends. Summing all 32 at unity made Jet Set Radio Future's character
 * sounds 14-15 dB louder than its music, peaking at full scale and clipping
 * 0.37% of samples. The title's own effects image (the standard dsstdfx), run
 * on a DSP56300 for one gameplay capture, gave per one-second window:
 *
 *   music, bins 0/1 only        out = in, 0.0 dB in every window
 *   character SFX, bins 6/7     out = in - 6.0 dB (within 0.3 dB)
 *   reverb send, bin 10         adds nothing measurable
 *
 * The send is an effect input, not a speaker feed: the image runs the reverb
 * on it and mixes only the wet result back, and JSRF leaves the room at the
 * default, which has no wet level. The halving is the crosstalk effect's own
 * gain; 3D voices reach those bins without the submix headroom (hrtf_headroom
 * 0 against 1 for every submix in JSRF), so the image's 6 dB puts them back
 * level with the 2D voices.
 *
 * Bins 11-31 stay at unity because nothing measured them. They feed effects
 * a title loads itself, whose wet output does reach the speakers: Burnout 3
 * sends to 11/12 and 17/18, the last pair only about 5 dB below its main
 * bins, and dropping them took 0.9 dB off its race. The dry sum stands in for
 * a wet path the stub cannot run until a capture says otherwise.
 *
 * Only 6/7 carried signal in the capture; 8/9 at half is assumed by symmetry,
 * as the other crosstalk pair of the same effect.
 *
 * Even bins left and odd bins right, as before, so each pair keeps its side. */
static float mixbin_gain(int bin)
{
    if (bin < 6)
        return 1.0f;
    if (bin < 10)
        return 0.5f;
    if (bin == 10)
        return 0.0f;
    return 1.0f;
}

/* GPRST (GP region + 0xFFFC): the GP runs only with both reset bits set, as
 * in xemu's gp_write. Nothing else models the GP, so this is kept for the
 * =auto ack alone. Before the first write the GP counts as running: a title
 * that never touches GPRST keeps its ack. */
void mcpx_apu_dsp_gprst_write(uint32_t val)
{
    const uint32_t run = NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    int held = (val & run) != run;

    if (held != s_gp_in_reset && s_dsp_ack_auto)
        fprintf(stderr, "[APU] GPRST 0x%X: GP %s; DSP doorbell ack %s\n",
                val, held ? "held in reset" : "running",
                held ? "paused" : "resumed");
    s_gp_in_reset = held;
}

/* The doorbell for =auto: SGE[0] of the GP scratch table + 0x810, re-read on
 * every poll, so the slot always lies in the page the table names now. NULL
 * while GPSADDR is unset, the table's first entry names no page, or the GP
 * is held in reset: a title that tears DirectSound down frees the scratch,
 * and a dword cleared there after that would land in whatever reuses the
 * page. */
static uint32_t *dsp_ack_auto_slot(MCPXAPUState *d)
{
    uint32_t sge = qatomic_read(&d->regs[NV_PAPU_GPSADDR]), page;

    if (!sge || s_gp_in_reset)
        return NULL;
    page = *(const uint32_t *)mcpx_apu_phys(sge);
    if (!page || (page & 0xFFF))
        return NULL;                       /* table not filled in yet */
    if (page != s_dsp_ack_auto_page) {
        s_dsp_ack_auto_page = page;
        fprintf(stderr, "[APU] DSP doorbell: GP scratch SGE table 0x%08X maps"
                        " page 0x%08X (%s); doorbell at physical 0x%08X\n",
                sge, page, mcpx_apu_phys_class(page),
                page + GP_DOORBELL_OFFSET);
    }
    return (uint32_t *)(mcpx_apu_phys(page) + GP_DOORBELL_OFFSET);
}

/* THE BOOT ROM, WITHOUT A DSP. When GPRST/EPRST go to 3 the core's boot
 * ROM copies the first 0x800 words of its scratch into P:0 and starts there
 * (xemu's dsp_bootstrap; apu_dsp_emu.c bootstrap() with the DSP on). The
 * stub runs no program, but a title may read the loaded program back: KUF:
 * Heroes' DirectSound checks EP P:6 against 0xCCCCCC, word 6 of the DOLBY
 * program it copied to EP scratch, on every DoWork (sub_002F5CCD), and on a
 * mismatch resets both cores, rewinds XGSCNT and stalls 10 ms
 * (sub_002F5A8D). With nothing loaded that ran about 30 times a second,
 * XACT's sample clock lost the time on each, and voices played in 10.7 ms
 * chunks with gaps between. GPRST/EPRST and P live in the
 * untrapped MCPX window, so the frame poll looks at them there. Only the
 * copy is modelled: P holds what the title loaded, and nothing runs it.
 * The copy fires when the APU frame thread's poll sees RST at 3, not on the
 * register write: a 0->3 toggle between two polls, or made while the frame
 * thread is paused (pause_requested), gets no copy. apu_dsp_emu_poll_rst
 * has the same limit. KUF's 10 ms stall after the reset is what keeps RST
 * at 3 long enough for the poll to see the edge. */
static uint32_t s_boot_rst[2];            /* GP, EP: last RST bits seen */
static unsigned long s_boots[2];

static void dsp_boot_copy(uint8_t *p, uint32_t sge_base, uint32_t max_sge)
{
    const uint8_t *src = NULL;
    uint32_t i;

    if (!sge_base)
        return;
    for (i = 0; i < APU_DSP_BOOT_WORDS; i++) {
        uint32_t off = i * 4, w;
        if ((off & 0xFFF) == 0) {
            uint32_t pe = off >> 12, page;
            if (pe > max_sge)
                return;                    /* past the table: P keeps the rest */
            page = *(const uint32_t *)mcpx_apu_phys(sge_base + pe * 8);
            if (!page || (page & 0xFFF))
                return;                    /* entry not filled in */
            src = mcpx_apu_phys(page);
        }
        memcpy(&w, src + (off & 0xFFF), 4);
        w &= 0xFFFFFFu;
        memcpy(p + off, &w, 4);
    }
}

void mcpx_apu_dsp_boot_poll(MCPXAPUState *d, uint8_t *mcpx)
{
    static const uint32_t win[2] = { 0x30000u, 0x50000u };
    static const char *const name[2] = { "GP", "EP" };
    int k;

    if (!mcpx || !d->ram_ptr)
        return;
    for (k = 0; k < 2; k++) {
        uint8_t *w = mcpx + win[k];
        uint32_t rst = *(volatile uint32_t *)(w + APU_DSP_WIN_RST) & 3;
        if (rst == s_boot_rst[k])
            continue;
        s_boot_rst[k] = rst;
        if (rst != 3)
            continue;
        dsp_boot_copy(w + APU_DSP_WIN_P,
                      qatomic_read(&d->regs[k ? NV_PAPU_EPSADDR
                                              : NV_PAPU_GPSADDR]),
                      qatomic_read(&d->regs[k ? NV_PAPU_EPSMAXSGE
                                              : NV_PAPU_GPSMAXSGE]));
        if (s_boots[k]++ < 3)
            fprintf(stderr, "[APU] %s out of reset: boot ROM copy, scratch"
                            " to P:0 (stub; P:6 = 0x%06X)\n", name[k],
                    *(const uint32_t *)(w + APU_DSP_WIN_P + 6 * 4));
    }
}

void mcpx_apu_dsp_ack_poll(MCPXAPUState *d)
{
    int i;

#ifdef XBOXRECOMP_APU_DSP_DEVICE
    /* The GP's own program clears the doorbell (apu_dsp_run.c). */
    if (mcpx_apu_dsp_device_on())
        return;
#endif
    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    mcpx_apu_dsp_boot_poll(d, (uint8_t *)xbox_McpxHost());
    if (s_dsp_ack_auto) {
        uint32_t *slot = dsp_ack_auto_slot(d);
        if (slot && *slot) {
            static int shown;
            if (shown++ < 3)
                fprintf(stderr, "[APU] DSP doorbell (auto): command 0x%08X"
                                " acknowledged\n", *slot);
            *slot = 0;
        }
    }
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            *slot = 0;
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* The doorbell ack's state is file-static, so it outlives an instance:
     * a GP the last instance held in reset would leave this one's ack
     * paused, and the RECOMP_APU_DSP_ACK setting would never be read again.
     * Start from what a fresh process has. */
    s_dsp_ack_count = -1;                  /* re-read on the first poll */
    s_dsp_ack_auto = 0;
    s_dsp_ack_auto_page = 0;
    s_gp_in_reset = 0;
    memset(s_boot_rst, 0, sizeof s_boot_rst);
    memset(s_boots, 0, sizeof s_boots);

    /* Allocate minimal DSP state for GP and EP.
     * We need these to exist so reset doesn't crash,
     * but they won't actually run DSP programs. */
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;

    d->gp.realtime = false;
    d->ep.realtime = false;

#ifdef XBOXRECOMP_APU_DSP_DEVICE
    if (mcpx_apu_dsp_device_on())
        return;                  /* apu_dsp_run.c says so at the first frame */
#endif
    fprintf(stderr, "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)\n");
}

/* Called after the frame thread is joined. Only mcpx_apu_reset_locked
 * (currently uncalled) reads this state, and it would run on that thread.
 * Safe to call twice. */
void mcpx_apu_dsp_finalize(MCPXAPUState *d)
{
#ifdef XBOXRECOMP_APU_DSP_DEVICE
    mcpx_apu_dsp_run_finalize();
#endif
    free(d->gp.dsp);
    free(d->ep.dsp);
    d->gp.dsp = NULL;
    d->ep.dsp = NULL;
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}

/* RECOMP_TRACE=apu: [APU-MIXBIN] once a second, the RMS and peak of every
 * mixbin that carried signal, with the bin's submix headroom. That is the
 * divisor for 2D voices only: the VP divides a 3D voice's first four bins by
 * hrtf_headroom instead (apu_vp.c), printed once as hrtf_hr. */
static void dsp_bin_stats(MCPXAPUState *d,
                          float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    static int on = -1;
    static double sq[NUM_MIXBINS];
    static float peak[NUM_MIXBINS];
    static unsigned frames, secs;

    if (on < 0)
        on = recomp_env_on(RENV_APU_TRACE) > 0;
    if (!on)
        return;
    for (int b = 0; b < NUM_MIXBINS; b++)
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float x = mixbins[b][i];
            sq[b] += (double)x * x;
            if (fabsf(x) > peak[b]) peak[b] = fabsf(x);
        }
    if (++frames % 1500)
        return;
    secs++;
    fprintf(stderr, "[APU-MIXBIN] t=%us hrtf_submix=%u/%u/%u/%u hrtf_hr=%u",
            secs, d->vp.hrtf_submix[0], d->vp.hrtf_submix[1],
            d->vp.hrtf_submix[2], d->vp.hrtf_submix[3], d->vp.hrtf_headroom);
    for (int b = 0; b < NUM_MIXBINS; b++) {
        if (peak[b] > 0.0f)
            fprintf(stderr, " b%d:submix_hr%u,rms%.1f,pk%.1f", b,
                    d->vp.submix_headroom[b],
                    20.0 * log10(sqrt(sq[b] / (1500.0 * NUM_SAMPLES_PER_FRAME)) + 1e-9),
                    20.0 * log10(peak[b] + 1e-9));
        sq[b] = 0.0;
        peak[b] = 0.0f;
    }
    fprintf(stderr, "\n");
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Bypass DSP: take mixbin 0 (front-left) and mixbin 1 (front-right)
     * and write them directly to the monitor frame buffer as the final
     * EP output.
     *
     * The Xbox DirectSound typically routes:
     *   Mixbin 0 = Front Left
     *   Mixbin 1 = Front Right
     *   Mixbin 2 = Center (often unused in stereo)
     *   Mixbin 3 = LFE
     *   Mixbin 4-5 = Rear L/R
     *
     * For stereo output, bins 0 and 1 are what we want.
     */

    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    dsp_bin_stats(d, mixbins);
#ifdef XBOXRECOMP_APU_DSP_DEVICE
    if (mcpx_apu_dsp_device_on()) {
        mcpx_apu_dsp_run_frame(d, mixbins);
        return;
    }
#endif

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        const int mode = mcpx_apu_mixdown_mode();
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* Bins 2..31 used to be computed and then dropped on the floor.
             * On hardware the GP and EP mix the submixes down; here they are
             * stubs, so thirty of thirty-two bins were discarded every frame
             * with no counter anywhere to say so.
             *
             * Measured on Jet Set Radio Future, one 200 s gameplay run, with
             * a positive control moving beside it:
             *
             *     [APU-BIN] 2D heard=557466 lost=0
             *               3D heard=0      lost=377768
             *               lost by bin: 6,7,8,9,10
             *
             * 557,466 music voice-frames heard and none lost; 377,768 effect
             * voice-frames produced correctly and thrown away. The title's 3D
             * positional voices -- its sound effects -- are routed to bins 6
             * to 10 by the guest's own V0BIN..V3BIN, and music on 2D voices
             * lands in bins 0 and 1, which is why the music was always
             * audible and no effect ever was. Every instrument upstream of
             * this line read healthy.
             *
             * Gating the HRTF submix override was tried first and did not fix
             * it, so the defect is the width of this mixdown and nothing else.
             *
             * Even bins left, odd bins right, which preserves the stereo
             * pairing the guest set up -- bins 6/7 and 8/9 arrive with matched
             * counts. This is not what a real EP does; it is the cheapest
             * mixdown that stops discarding audio. */
            float left, right;
            if (mode != MIXDOWN_TWO_BINS) {
                left = 0.0f;
                right = 0.0f;
                for (int b = 0; b < NUM_MIXBINS; ++b) {
                    float s = mixbins[b][i];
                    if (mode == MIXDOWN_STAGED)
                        s *= mixbin_gain(b);
                    if (b & 1) right += s;
                    else       left  += s;
                }
            } else {
                left = mixbins[0][i];
                right = mixbins[1][i];
            }
            /* Clamp to [-1, 1] range */
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;

            /* Convert to 16-bit and write (not accumulate) into frame buffer.
             * Each of the 8 sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}
