/*
 * MCPX APU GP/EP device model on the dsp56300 JIT (XBOXRECOMP_APU_DSP).
 *
 * One ApuDspEmu is one of the APU's two DSP56300 cores: the GP (global
 * processor, effects) or the EP (encode processor). It owns the core, its
 * DMA engine and its peripherals, and runs one APU frame at a time. The
 * caller supplies the system memory side (scatter-gather scratch and the
 * FIFOs) as callbacks, which keeps this file free of MCPXAPUState and lets a
 * test drive a core on its own.
 *
 * Nothing here exposes the JIT's types: the APU includes this header in
 * every build, the JIT only exists in one with XBOXRECOMP_APU_DSP.
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
#ifndef APU_DSP_EMU_H
#define APU_DSP_EMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A core's window in the MCPX MMIO space (GP at +0x30000, EP at +0x50000):
 * one uint32 per 24-bit DSP word. The core runs on these pages directly, so
 * a title's writes through the untrapped window are what the program sees. */
#define APU_DSP_WINDOW_BYTES 0x10000u
#define APU_DSP_WIN_X        0x0000u   /* X:0x000-0xFFF */
#define APU_DSP_WIN_MIXBUF   0x5000u   /* GPMIXBUF: a GP frame's mixbuf copy */
#define APU_DSP_WIN_Y        0x6000u   /* Y:0x000-0x7FF */
#define APU_DSP_WIN_P        0xA000u   /* P:0x000-0xFFF */
#define APU_DSP_WIN_RST      0xFFFCu   /* GPRST / EPRST */

/* What the boot ROM copies from the start of scratch into P. */
#define APU_DSP_BOOT_WORDS   0x800u

/* X:0x1400, where the VP writes the 32 mixbins and the GP leaves its
 * output (GP_DSP_MIXBUF_BASE in apu_regs.h). */
#define APU_DSP_MIXBUF_BASE  0x1400u
#define APU_DSP_MIXBUF_WORDS 0x400u

/* System memory, as xemu's dsp_dma.h has them. addr is a byte address in the
 * core's scratch space; dir true is DSP to system memory. */
typedef void (*ApuDspScratchRw)(void *opaque, uint8_t *ptr, uint32_t addr,
                                size_t len, bool dir);
typedef void (*ApuDspFifoRw)(void *opaque, uint8_t *ptr, unsigned index,
                             size_t len, bool dir);

typedef enum ApuDspSpace {
    APU_DSP_SPACE_X,
    APU_DSP_SPACE_Y,
    APU_DSP_SPACE_P,
} ApuDspSpace;

typedef struct ApuDspEmu ApuDspEmu;

typedef struct ApuDspEmuConfig {
    const char *name;           /* "GP" or "EP", for the log */
    bool is_gp;
    /* APU_DSP_WINDOW_BYTES of 4-byte aligned memory, or NULL for a private
     * zeroed one (tests). */
    uint8_t *window;
    void *opaque;
    ApuDspScratchRw scratch_rw;
    ApuDspFifoRw fifo_rw;
} ApuDspEmuConfig;

typedef struct ApuDspEmuStats {
    uint64_t frames;            /* frames run */
    uint64_t cycles;            /* summed over those frames */
    uint32_t cycles_max;
    uint64_t capped;            /* frames that hit the cycle cap */
    uint64_t dma_blocks;        /* descriptors transferred */
    uint64_t dma_rejected;      /* descriptors refused (see apu_dsp_emu.c) */
    uint64_t p_dma_words;       /* P words written by the DMA */
    uint64_t p_host_words;      /* P words the host changed in the window */
    uint64_t masked;            /* window words cut back to 24 bits */
    uint64_t bootstraps;
    uint64_t trace_repeats;     /* DMA trace entries folded into the last */
    uint32_t pc;                /* PC after the last frame */
    uint32_t dma_control;       /* DMA control/status (bits 3-5) */
    uint32_t dma_next_block;
} ApuDspEmuStats;

ApuDspEmu *apu_dsp_emu_create(const ApuDspEmuConfig *cfg);
void apu_dsp_emu_destroy(ApuDspEmu *e);
uint8_t *apu_dsp_emu_window(ApuDspEmu *e);

/* GPRST/EPRST. A title writes it through the untrapped window, so the APU
 * frame thread polls: poll_rst acts on a change of the two run bits and
 * returns true when it did (an EPRST write restarts the EP's 8-frame
 * period). rst_write is the same for a trapped write. Both bits set runs
 * the core; the edge that sets them bootstraps it (the boot ROM copies the
 * first 0x800 words of scratch into P); any other value holds it in reset. */
bool apu_dsp_emu_poll_rst(ApuDspEmu *e);
void apu_dsp_emu_rst_write(ApuDspEmu *e, uint32_t val);
bool apu_dsp_emu_running(const ApuDspEmu *e);

/* One APU frame: raise START_FRAME and run until the program halts (writes
 * bit 0 of X:$FFFFC4) or cycle_cap cycles pass. Returns the cycles run. */
uint32_t apu_dsp_emu_run_frame(ApuDspEmu *e, uint32_t cycle_cap);

/* DSP memory by DSP address (X:0x1400 is the mixbuf). A P write tells the
 * JIT, so it drops only the translations that word was under. */
uint32_t apu_dsp_emu_read(ApuDspEmu *e, ApuDspSpace space, uint32_t addr);
void apu_dsp_emu_write(ApuDspEmu *e, ApuDspSpace space, uint32_t addr,
                       uint32_t val);
void apu_dsp_emu_read_run(ApuDspEmu *e, ApuDspSpace space, uint32_t addr,
                          uint32_t *out, uint32_t count);
void apu_dsp_emu_write_run(ApuDspEmu *e, ApuDspSpace space, uint32_t addr,
                           const uint32_t *vals, uint32_t count);

void apu_dsp_emu_get_stats(const ApuDspEmu *e, ApuDspEmuStats *out);

#ifdef __cplusplus
}
#endif

#endif /* APU_DSP_EMU_H */
