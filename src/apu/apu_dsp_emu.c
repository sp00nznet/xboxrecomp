/*
 * MCPX APU GP/EP device model on the dsp56300 JIT (XBOXRECOMP_APU_DSP).
 *
 * Ported from xemu's hw/xbox/mcpx/apu/dsp/gp_ep.c (the GP/EP windows, the
 * GPRST/EPRST bootstrap, the frame loop) and hw/xbox/mcpx/apu/dsp/dsp_dma.c
 * (the DSP's DMA engine), both LGPL-2.1+ like the rest of src/apu. The core
 * is the MIT dsp56300 library.
 *
 * xemu's hw/xbox/mcpx/apu/dsp/dsp.c and dsp_jit.c (its glue to the same
 * library) and its DSP interpreter are GPL-2.0+, and no code here comes from
 * them.
 * Only the hardware map was taken from them: the peripheral addresses and
 * bits, the X:0x1400 mixbuf alias and the boot ROM's copy. The glue to the
 * core (memory map, peripheral decode, bootstrap) is this project's own,
 * written against the library's C header.
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
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "apu_dsp_emu.h"

#include <dsp56300.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Memory the window holds for each space, in words. X runs to 0x1000 on both
 * cores: on the GP its last quarter (X:0xC00-0xFFF) is the mixbuf, which the
 * core also sees at X:0x1400 (below). The EP's MMIO only decodes 0xC00 X
 * words and 0x100 Y words, but its program sees the same map as the GP's,
 * as on xemu. */
#define X_WORDS 0x1000u
#define Y_WORDS 0x800u
#define P_WORDS 0x1000u
#define MIXBUF_X_ALIAS 0xC00u

#define BOOT_WORDS APU_DSP_BOOT_WORDS

#define WORD_MASK 0xFFFFFFu

/* The peripheral page, X:$FFFF80-$FFFFFF, by word offset from its base.
 * Only these words decode; the rest read 0 and drop writes. */
#define PERIPH_BASE  0xFFFF80u
#define PERIPH_END   0x1000000u
#define REG_HALT     0x44u  /* $FFFFC4: bit 0 written ends the frame */
#define REG_INT      0x45u  /* $FFFFC5: interrupt status, write 1 to clear */
#define REG_DMA      0x54u  /* $FFFFD4-$FFFFD7: the DMA engine, by DMA_REG_* */
#define DMA_REG_NEXT    0u
#define DMA_REG_START   1u
#define DMA_REG_CONTROL 2u
#define DMA_REG_CONFIG  3u
#define DMA_REGS        4u

#define INT_START_FRAME (1u << 1)
#define INT_DMA_EOL     (1u << 7)

/* dsp_dma_regs.h */
#define DMA_CONTROL_ACTION           0x7u
#define DMA_CONTROL_ACTION_NOP       0u
#define DMA_CONTROL_ACTION_START     1u
#define DMA_CONTROL_ACTION_STOP      2u
#define DMA_CONTROL_ACTION_FREEZE    3u
#define DMA_CONTROL_ACTION_UNFREEZE  4u
#define DMA_CONTROL_ACTION_ABORT     5u
#define DMA_CONTROL_FROZEN  (1u << 3)
#define DMA_CONTROL_RUNNING (1u << 4)
#define DMA_CONTROL_STOPPED (1u << 5)
#define NODE_POINTER_VAL 0x3fffu
#define NODE_POINTER_EOL (1u << 14)
#define NODE_CONTROL_DIRECTION (1u << 1)

/* The DMA's view of DSP memory: X below 0x1800, Y at 0x1800, P at 0x2800. */
#define DMA_Y_BASE 0x1800u
#define DMA_Y_END  0x2000u
#define DMA_P_BASE 0x2800u
#define DMA_P_END  0x3800u
/* A transfer never spans more than the largest space it can land in. */
#define DMA_MAX_WORDS 0x1800u
/* Descriptors one control write may walk. DMA memory holds fewer than 0x800
 * descriptors, so a longer chain has looped back on itself and would never
 * reach EOL. */
#define DMA_MAX_CHAIN 0x800u

/* Program-side DMA register writes and descriptors, kept for the log when a
 * descriptor is refused: the program that set next_block is the question.
 * An entry that repeats the one before it only counts, so a chain that loops
 * on one descriptor does not push the register writes out of the ring. */
#define TRACE_LEN 32

typedef struct DmaTrace {
    uint64_t frame;
    uint32_t what;   /* peripheral address, or 0 for a descriptor */
    uint32_t a, b, c;
    uint32_t repeats;
} DmaTrace;

typedef struct DspDma {
    uint32_t configuration;
    uint32_t control;
    uint32_t start_block;
    uint32_t next_block;
    bool eol;
    /* xemu's completion model: three control reads while running. */
    uint32_t read_count;
} DspDma;

struct ApuDspEmu {
    char name[8];
    bool is_gp;
    Dsp56300Jit *jit;
    uint8_t *window;
    bool own_window;
    uint32_t *x, *y, *p;
    /* P as the JIT last saw it. A title writes P through the untrapped
     * window, which the JIT cannot see; the frame compares the window to
     * this and replays each change through the library, which drops only
     * the translations over the words that changed. */
    uint32_t p_shadow[P_WORDS];

    void *opaque;
    ApuDspScratchRw scratch_rw;
    ApuDspFifoRw fifo_rw;

    uint32_t interrupts;
    DspDma dma;
    uint32_t rst;
    bool running;

    uint8_t xfer[DMA_MAX_WORDS * 4];
    uint32_t words[DMA_MAX_WORDS];

    DmaTrace trace[TRACE_LEN];
    unsigned trace_n;
    int reject_logs;
    int cap_logs;
    ApuDspEmuStats st;
};

static void trace(ApuDspEmu *e, uint32_t what, uint32_t a, uint32_t b,
                  uint32_t c)
{
    if (e->trace_n) {
        DmaTrace *last = &e->trace[(e->trace_n - 1) % TRACE_LEN];
        if (last->frame == e->st.frames && last->what == what &&
            last->a == a && last->b == b && last->c == c) {
            last->repeats++;
            e->st.trace_repeats++;
            return;
        }
    }
    DmaTrace *t = &e->trace[e->trace_n++ % TRACE_LEN];
    t->frame = e->st.frames;
    t->what = what;
    t->a = a;
    t->b = b;
    t->c = c;
    t->repeats = 0;
}

static void trace_dump(ApuDspEmu *e)
{
    unsigned n = e->trace_n < TRACE_LEN ? e->trace_n : TRACE_LEN;
    for (unsigned i = e->trace_n - n; i != e->trace_n; i++) {
        const DmaTrace *t = &e->trace[i % TRACE_LEN];
        if (t->what)
            fprintf(stderr, "[DSP]   %s f%llu write x:$%06x = %06x", e->name,
                    (unsigned long long)t->frame, t->what, t->a);
        else
            fprintf(stderr, "[DSP]   %s f%llu block %04x control %06x"
                    " count %06x", e->name, (unsigned long long)t->frame,
                    t->a, t->b, t->c);
        if (t->repeats)
            fprintf(stderr, " (x%u more)", t->repeats);
        fputc('\n', stderr);
    }
}

/* ---- DSP memory by DMA address ---- */

static bool dma_space(uint32_t a, uint32_t n, Dsp56300MemSpace *space,
                      uint32_t *addr)
{
    if (a < DMA_Y_BASE) {
        *space = DSP56300_MEM_SPACE_X;
        *addr = a;
        return a + n <= DMA_Y_BASE;
    }
    if (a < DMA_Y_END) {
        *space = DSP56300_MEM_SPACE_Y;
        *addr = a - DMA_Y_BASE;
        return a + n <= DMA_Y_END;
    }
    if (a >= DMA_P_BASE && a < DMA_P_END) {
        *space = DSP56300_MEM_SPACE_P;
        *addr = a - DMA_P_BASE;
        return a + n <= DMA_P_END;
    }
    return false;
}

static void p_written(ApuDspEmu *e, uint32_t addr, uint32_t n)
{
    for (uint32_t i = 0; i < n && addr + i < P_WORDS; i++)
        e->p_shadow[addr + i] = e->p[addr + i];
}

static void mem_write_run(ApuDspEmu *e, Dsp56300MemSpace space, uint32_t addr,
                          const uint32_t *vals, uint32_t n)
{
    /* Through the library, never into the buffers behind its back: a P word
     * changed here must drop the blocks compiled over it. */
    dsp56300_write_memory_run(e->jit, space, addr, vals, n);
    if (space == DSP56300_MEM_SPACE_P)
        p_written(e, addr, n);
}

/* ---- the DMA engine (dsp_dma.c) ---- */

static void scratch_circular_copy(ApuDspEmu *e, uint32_t scratch_base,
                                  uint32_t *scratch_offset,
                                  uint32_t scratch_size, size_t transfer_size,
                                  uint8_t *scratch_buf, bool direction)
{
    if (*scratch_offset >= scratch_size)
        *scratch_offset = 0;
    size_t buf_offset = 0;
    while (transfer_size > 0) {
        size_t bytes_until_wrap = scratch_size - *scratch_offset;
        size_t chunk_size = transfer_size < bytes_until_wrap
                                ? transfer_size : bytes_until_wrap;
        e->scratch_rw(e->opaque, &scratch_buf[buf_offset],
                      scratch_base + *scratch_offset, chunk_size, direction);
        *scratch_offset += (uint32_t)chunk_size;
        if (*scratch_offset >= scratch_size)
            *scratch_offset = 0;
        transfer_size -= chunk_size;
        buf_offset += chunk_size;
    }
}

/* xemu asserts on these; a device in a game must not abort, so it refuses
 * the descriptor, ends the chain there (EOL, so a program waiting on it
 * goes on) and logs the descriptor with the DMA writes before it. */
static void dma_reject(ApuDspEmu *e, uint32_t block, const uint32_t *d,
                       const char *why)
{
    DspDma *s = &e->dma;
    s->eol = true;
    s->next_block |= NODE_POINTER_EOL;
    e->st.dma_rejected++;
    if (e->reject_logs++ >= 4)
        return;
    fprintf(stderr, "[DSP] %s dma refused (%s): block %04x next %06x"
            " control %06x count %06x dsp %06x scratch off %06x base %06x"
            " size %06x\n", e->name, why, block, d[0], d[1], d[2], d[3], d[4],
            d[5], d[6]);
    trace_dump(e);
}

/* One sample in the transfer buffer: 16-bit items carry the word's top 16
 * bits, 32-bit items the whole word. */
static void put_item(uint8_t *buf, size_t i, unsigned size, uint32_t v)
{
    if (size == 2) {
        uint16_t h = (uint16_t)(v >> 8);
        memcpy(buf + i * 2, &h, 2);
    } else {
        memcpy(buf + i * 4, &v, 4);
    }
}

static uint32_t get_item(const uint8_t *buf, size_t i, unsigned size)
{
    if (size == 2) {
        uint16_t h;
        memcpy(&h, buf + i * 2, 2);
        return (uint32_t)h << 8;
    }
    uint32_t v;
    memcpy(&v, buf + i * 4, 4);
    return v & WORD_MASK;
}

static void dsp_dma_run(ApuDspEmu *e)
{
    DspDma *s = &e->dma;
    if (!(s->control & DMA_CONTROL_RUNNING) || (s->control & DMA_CONTROL_FROZEN))
        return;

    uint32_t walked = 0;
    while (!(s->next_block & NODE_POINTER_EOL)) {
        uint32_t addr = s->next_block & NODE_POINTER_VAL;
        Dsp56300MemSpace block_space;
        uint32_t block_addr, d[7] = { 0 };
        if (!dma_space(addr, 7, &block_space, &block_addr)) {
            dma_reject(e, addr, d, "block address");
            return;
        }
        dsp56300_read_memory_run(e->jit, block_space, block_addr, d, 7);
        uint32_t next_block = d[0], control = d[1], count = d[2];
        uint32_t dsp_offset = d[3], scratch_offset = d[4];
        uint32_t scratch_base = d[5], scratch_size = d[6] + 1;
        trace(e, 0, addr, control, count);
        if (walked++ == DMA_MAX_CHAIN) {
            dma_reject(e, addr, d, "chain loop");
            return;
        }

        s->next_block = next_block;
        if (s->next_block & NODE_POINTER_EOL)
            s->eol = true;

        bool dsp_interleave = control & 1;
        bool direction = control & NODE_CONTROL_DIRECTION;
        uint32_t unk2 = (control >> 2) & 0x3;
        bool buffer_offset_writeback = (control >> 4) & 1;
        uint32_t buf_id = (control >> 5) & 0xf;
        uint32_t format = (control >> 10) & 0x7;
        bool unk13 = (control >> 13) & 1;

        uint32_t channel_count = (count & 0xF) + 1;
        uint32_t block_count = count >> 4;

        /* Format 0 (8-bit) is in xemu's table but no title is known to use
         * it, and its packing is unverified; refuse it with the unknowns. */
        unsigned item_size;
        switch (format) {
        case 1: item_size = 2; break;
        case 2:
        case 6: item_size = 4; break;
        default: dma_reject(e, addr, d, "format"); return;
        }
        if (unk2 || unk13) {
            dma_reject(e, addr, d, "control bits");
            return;
        }
        bool fifo = buf_id <= 3, scratch = buf_id >= 0xE;
        if (!(scratch || (fifo && direction))) {
            dma_reject(e, addr, d, "buffer");
            return;
        }
        if (dsp_interleave && !direction) {
            dma_reject(e, addr, d, "interleave");
            return;
        }
        Dsp56300MemSpace mem_space;
        uint32_t mem_address;
        if (count > DMA_MAX_WORDS ||
            !dma_space(dsp_offset, count, &mem_space, &mem_address)) {
            dma_reject(e, addr, d, "dsp range");
            return;
        }

        uint32_t scratch_addr = scratch_base + scratch_offset;
        size_t transfer_size = (size_t)count * item_size;
        uint8_t *buf = e->xfer;

        if (direction) {
            dsp56300_read_memory_run(e->jit, mem_space, mem_address, e->words,
                                     count);
            if (dsp_interleave) {
                transfer_size = (size_t)block_count * item_size * channel_count;
                for (uint32_t i = 0; i < block_count; i++)
                    for (uint32_t ch = 0; ch < channel_count; ch++) {
                        uint32_t j = ch * block_count + i;
                        put_item(buf, (size_t)i * channel_count + ch,
                                 item_size, j < count ? e->words[j] : 0);
                    }
            } else {
                for (uint32_t i = 0; i < count; i++)
                    put_item(buf, i, item_size, e->words[i]);
            }
            if (fifo)
                e->fifo_rw(e->opaque, buf, buf_id, transfer_size, true);
            else if (buf_id == 0xE)
                scratch_circular_copy(e, scratch_base, &scratch_offset,
                                      scratch_size, transfer_size, buf, true);
            else
                e->scratch_rw(e->opaque, buf, scratch_addr, transfer_size,
                              true);
        } else {
            if (buf_id == 0xE)
                scratch_circular_copy(e, scratch_base, &scratch_offset,
                                      scratch_size, transfer_size, buf, false);
            else
                e->scratch_rw(e->opaque, buf, scratch_addr, transfer_size,
                              false);
            for (uint32_t i = 0; i < count; i++)
                e->words[i] = get_item(buf, i, item_size);
            mem_write_run(e, mem_space, mem_address, e->words, count);
            if (mem_space == DSP56300_MEM_SPACE_P)
                e->st.p_dma_words += count;
        }

        if (buffer_offset_writeback)
            mem_write_run(e, block_space, block_addr + 4, &scratch_offset, 1);
        e->st.dma_blocks++;
    }
}

static uint32_t dsp_dma_read(ApuDspEmu *e, uint32_t reg)
{
    DspDma *s = &e->dma;
    switch (reg) {
    case DMA_REG_CONFIG:
        return s->configuration;
    case DMA_REG_CONTROL:
        if (s->control & DMA_CONTROL_RUNNING) {
            s->read_count++;
            if (s->read_count > 2) {
                s->control &= ~DMA_CONTROL_RUNNING;
                s->control |= DMA_CONTROL_STOPPED;
                s->read_count = 0;
            }
        }
        return s->control;
    case DMA_REG_START:
        return s->start_block;
    default:
        return s->next_block;
    }
}

static void dsp_dma_write(ApuDspEmu *e, uint32_t reg, uint32_t v)
{
    DspDma *s = &e->dma;
    trace(e, PERIPH_BASE + REG_DMA + reg, v, 0, 0);
    switch (reg) {
    case DMA_REG_CONFIG:
        s->configuration = v;
        break;
    case DMA_REG_CONTROL:
        switch (v & DMA_CONTROL_ACTION) {
        case DMA_CONTROL_ACTION_START:
            s->control |= DMA_CONTROL_RUNNING;
            s->control &= ~DMA_CONTROL_STOPPED;
            s->read_count = 0;
            break;
        case DMA_CONTROL_ACTION_STOP:
            s->control |= DMA_CONTROL_STOPPED;
            s->control &= ~DMA_CONTROL_RUNNING;
            break;
        case DMA_CONTROL_ACTION_FREEZE:
            s->control |= DMA_CONTROL_FROZEN;
            break;
        case DMA_CONTROL_ACTION_UNFREEZE:
            s->control &= ~DMA_CONTROL_FROZEN;
            break;
        default:
            /* xemu asserts; NOP and ABORT change nothing here. */
            break;
        }
        /* The whole chain moves inside this write, so a program that polls
         * the status afterwards sees its data in place. */
        dsp_dma_run(e);
        break;
    case DMA_REG_START:
        s->start_block = v;
        break;
    default:
        s->next_block = v;
        break;
    }
}

/* ---- peripherals ---- */

/* The core calls these for any X access in the peripheral page. The DSP
 * runs on one thread per core, so the decode needs no lock. */
static uint32_t periph_read(void *opaque, uint32_t address)
{
    ApuDspEmu *e = opaque;
    uint32_t off = address - PERIPH_BASE;
    if (off - REG_DMA < DMA_REGS)
        return dsp_dma_read(e, off - REG_DMA);
    if (off == REG_INT)
        return e->interrupts | (e->dma.eol ? INT_DMA_EOL : 0);
    return 0;
}

static void periph_write(void *opaque, uint32_t address, uint32_t value)
{
    ApuDspEmu *e = opaque;
    uint32_t off = address - PERIPH_BASE;
    if (off - REG_DMA < DMA_REGS) {
        dsp_dma_write(e, off - REG_DMA, value);
    } else if (off == REG_INT) {
        e->interrupts &= ~value;
        if (value & INT_DMA_EOL)
            e->dma.eol = false;
    } else if (off == REG_HALT && (value & 1)) {
        dsp56300_set_halt_requested(e->jit, true);
    }
}

/* ---- the core ---- */

/* Core addresses [start, start + words) backed by base[offset...]. */
static Dsp56300MemoryRegion buffer_region(uint32_t start, uint32_t words,
                                          uint32_t *base, uint32_t offset)
{
    return (Dsp56300MemoryRegion){
        .start = start, .end = start + words,
        .kind = DSP56300_REGION_BUFFER,
        .data.buffer = { .base = base, .offset = offset } };
}

ApuDspEmu *apu_dsp_emu_create(const ApuDspEmuConfig *cfg)
{
    ApuDspEmu *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    snprintf(e->name, sizeof(e->name), "%s", cfg->name ? cfg->name : "DSP");
    e->is_gp = cfg->is_gp;
    e->opaque = cfg->opaque;
    e->scratch_rw = cfg->scratch_rw;
    e->fifo_rw = cfg->fifo_rw;
    e->window = cfg->window;
    if (!e->window) {
        e->window = calloc(1, APU_DSP_WINDOW_BYTES);
        e->own_window = true;
        if (!e->window) {
            free(e);
            return NULL;
        }
    }
    e->x = (uint32_t *)(e->window + APU_DSP_WIN_X);
    e->y = (uint32_t *)(e->window + APU_DSP_WIN_Y);
    e->p = (uint32_t *)(e->window + APU_DSP_WIN_P);
    for (uint32_t i = 0; i < P_WORDS; i++)
        e->p_shadow[i] = e->p[i] &= WORD_MASK;
    /* The core starts in reset whatever the window holds: a title that
     * deasserted GPRST before the device existed (JSRF) still gets its
     * bootstrap on the first poll. */
    e->rst = 0;

    /* X: the window's words, the mixbuf a second time at X:0x1400, then the
     * peripheral page. The mixbuf is one memory, X:0xC00-0xFFF, which the
     * core also reads and writes at X:0x1400-0x17FF: the VP's mixbins land
     * there, and the GP program reads them, and leaves its output, at
     * either address. The window's GPMIXBUF page (+0x5000) is not this
     * memory: the APU writes the mixbuf by DSP address, and each GP frame
     * copies it to that page for whoever reads it through the window. */
    Dsp56300MemoryRegion xr[3], yr[1], pr[1];
    xr[0] = buffer_region(0, X_WORDS, e->x, 0);
    xr[1] = buffer_region(APU_DSP_MIXBUF_BASE, APU_DSP_MIXBUF_WORDS, e->x,
                          MIXBUF_X_ALIAS);
    xr[2] = (Dsp56300MemoryRegion){
        .start = PERIPH_BASE, .end = PERIPH_END,
        .kind = DSP56300_REGION_CALLBACK,
        .data.callback = { .opaque = e, .read = periph_read,
                           .write = periph_write } };
    yr[0] = buffer_region(0, Y_WORDS, e->y, 0);
    pr[0] = buffer_region(0, P_WORDS, e->p, 0);
    Dsp56300CreateInfo ci = { .memory_map = {
        .x_regions = xr, .x_count = 3,
        .y_regions = yr, .y_count = 1,
        .p_regions = pr, .p_count = 1 } };
    e->jit = dsp56300_create(&ci);
    if (!e->jit) {
        apu_dsp_emu_destroy(e);
        return NULL;
    }
    return e;
}

void apu_dsp_emu_destroy(ApuDspEmu *e)
{
    if (!e)
        return;
    if (e->jit)
        dsp56300_destroy(e->jit);
    if (e->own_window)
        free(e->window);
    free(e);
}

uint8_t *apu_dsp_emu_window(ApuDspEmu *e)
{
    return e->window;
}

static void core_reset(ApuDspEmu *e)
{
    /* A core held in reset loses its DMA engine and interrupt state with
     * it. xemu resets only the core; a title restarting a GP whose last
     * chain was still running would otherwise see that chain's status. */
    dsp56300_reset(e->jit);
    memset(&e->dma, 0, sizeof(e->dma));
    e->interrupts = 0;
    e->running = false;
}

/* GPRST/EPRST going to 3: the boot ROM loads the program from the start of
 * scratch into P and starts it at P:0. */
static void bootstrap(ApuDspEmu *e)
{
    core_reset(e);
    e->scratch_rw(e->opaque, e->xfer, 0, BOOT_WORDS * 4, false);
    for (uint32_t i = 0; i < BOOT_WORDS; i++) {
        uint32_t w = get_item(e->xfer, i, 4);
        e->p[i] = w;
        e->p_shadow[i] = w;
    }
    /* The whole program changed and nothing runs, the one place a full drop
     * is right (never from a peripheral callback, which runs on a block the
     * drop would free). */
    dsp56300_invalidate_cache(e->jit);
    e->running = true;
    e->st.bootstraps++;
}

void apu_dsp_emu_rst_write(ApuDspEmu *e, uint32_t val)
{
    bool was = (e->rst & 3) == 3, now = (val & 3) == 3;
    e->rst = val & 3;
    if (!now)
        core_reset(e);
    else if (!was)
        bootstrap(e);
}

bool apu_dsp_emu_poll_rst(ApuDspEmu *e)
{
    uint32_t v = *(volatile uint32_t *)(e->window + APU_DSP_WIN_RST) & 3;
    if (v == e->rst)
        return false;
    apu_dsp_emu_rst_write(e, v);
    return true;
}

bool apu_dsp_emu_running(const ApuDspEmu *e)
{
    return e->running;
}

/* The title writes DSP memory through the window 32 bits a word; the
 * hardware keeps 24 (xemu masks on the MMIO write). Then hand the JIT the P
 * words the title changed. */
static void sync_window(ApuDspEmu *e)
{
    uint64_t masked = 0;
    for (uint32_t i = 0; i < X_WORDS; i++)
        if (e->x[i] & ~WORD_MASK) {
            e->x[i] &= WORD_MASK;
            masked++;
        }
    for (uint32_t i = 0; i < Y_WORDS; i++)
        if (e->y[i] & ~WORD_MASK) {
            e->y[i] &= WORD_MASK;
            masked++;
        }
    for (uint32_t i = 0; i < P_WORDS; i++) {
        uint32_t v = e->p[i];
        if (v == e->p_shadow[i])
            continue;
        if (v & ~WORD_MASK)
            masked++;
        /* Put back what the JIT compiled from, so the library sees the
         * change and marks the word dirty. */
        e->p[i] = e->p_shadow[i];
        dsp56300_write_memory(e->jit, DSP56300_MEM_SPACE_P, i, v);
        e->p_shadow[i] = e->p[i];
        e->st.p_host_words++;
    }
    e->st.masked += masked;
}

uint32_t apu_dsp_emu_run_frame(ApuDspEmu *e, uint32_t cycle_cap)
{
    if (!e->running)
        return 0;
    sync_window(e);
    e->interrupts |= INT_START_FRAME;
    dsp56300_set_halt_requested(e->jit, false);
    dsp56300_set_cycle_count(e->jit, 0);
    uint32_t n = 0;
    do {
        dsp56300_run(e->jit, 1000);
        n += 1000;
    } while (!dsp56300_halt_requested(e->jit) && n < cycle_cap);
    uint32_t cycles = dsp56300_cycle_count(e->jit);
    Dsp56300State ss;
    dsp56300_get_state(e->jit, &ss);
    e->st.pc = ss.pc;
    if (!dsp56300_halt_requested(e->jit)) {
        /* A program that never halts (waiting on something this model
         * lacks, or run off its code) must not stall the frame thread. */
        e->st.capped++;
        if (e->cap_logs++ < 4)
            fprintf(stderr, "[DSP] %s frame %llu ran %u cycles without"
                    " halting, pc=%06x\n", e->name,
                    (unsigned long long)e->st.frames, cycles, ss.pc);
    }
    if (e->is_gp)
        memcpy(e->window + APU_DSP_WIN_MIXBUF, e->x + MIXBUF_X_ALIAS,
               APU_DSP_MIXBUF_WORDS * 4);
    e->st.frames++;
    e->st.cycles += cycles;
    if (cycles > e->st.cycles_max)
        e->st.cycles_max = cycles;
    return cycles;
}

uint32_t apu_dsp_emu_read(ApuDspEmu *e, ApuDspSpace space, uint32_t addr)
{
    uint32_t v;
    apu_dsp_emu_read_run(e, space, addr, &v, 1);
    return v;
}

void apu_dsp_emu_write(ApuDspEmu *e, ApuDspSpace space, uint32_t addr,
                       uint32_t val)
{
    apu_dsp_emu_write_run(e, space, addr, &val, 1);
}

static Dsp56300MemSpace core_space(ApuDspSpace s)
{
    return s == APU_DSP_SPACE_P ? DSP56300_MEM_SPACE_P
         : s == APU_DSP_SPACE_Y ? DSP56300_MEM_SPACE_Y
                                : DSP56300_MEM_SPACE_X;
}

void apu_dsp_emu_read_run(ApuDspEmu *e, ApuDspSpace space, uint32_t addr,
                          uint32_t *out, uint32_t count)
{
    dsp56300_read_memory_run(e->jit, core_space(space), addr, out, count);
}

void apu_dsp_emu_write_run(ApuDspEmu *e, ApuDspSpace space, uint32_t addr,
                           const uint32_t *vals, uint32_t count)
{
    mem_write_run(e, core_space(space), addr, vals, count);
}

void apu_dsp_emu_get_stats(const ApuDspEmu *e, ApuDspEmuStats *out)
{
    *out = e->st;
    out->dma_control = e->dma.control;
    out->dma_next_block = e->dma.next_block;
}
