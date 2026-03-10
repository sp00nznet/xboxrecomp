/*
 * MCPX APU State Structures (adapted from xemu)
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

#pragma once

#include "apu_shim.h"
#include "apu_regs.h"
#include "apu_debug.h"

/* ============================================================
 * DSP CPU registers
 * ============================================================ */

#define DSP_REG_MAX     0x40
#define DSP_XRAM_SIZE   4096
#define DSP_YRAM_SIZE   2048
#define DSP_PRAM_SIZE   4096
#define DSP_MIXBUFFER_SIZE 1024
#define DSP_PERIPH_SIZE 128

/* ============================================================
 * DSP Core State (from dsp_cpu.h)
 * ============================================================ */

typedef struct dsp_core_s {
    bool is_gp;
    bool is_idle;
    uint32_t cycle_count;

    uint16_t instr_cycle;

    uint32_t pc;
    uint32_t registers[DSP_REG_MAX];

    /* stack[0=ssh], stack[1=ssl] */
    uint32_t stack[2][16];

    uint32_t xram[DSP_XRAM_SIZE];
    uint32_t yram[DSP_YRAM_SIZE];
    uint32_t pram[DSP_PRAM_SIZE];
    const void *pram_opcache[DSP_PRAM_SIZE];

    uint32_t mixbuffer[DSP_MIXBUFFER_SIZE];

    /* peripheral space */
    uint32_t periph[DSP_PERIPH_SIZE];

    uint32_t loop_rep;
    uint32_t pc_on_rep;

    /* Interruptions */
    uint16_t interrupt_state;
    uint16_t interrupt_instr_fetch;
    uint16_t interrupt_save_pc;
    uint16_t interrupt_counter;
    uint16_t interrupt_ipl_to_raise;
    uint16_t interrupt_pipeline_count;
    int16_t interrupt_ipl[12];
    uint16_t interrupt_is_pending[12];

    /* callbacks */
    uint32_t (*read_peripheral)(struct dsp_core_s *core, uint32_t address);
    void (*write_peripheral)(struct dsp_core_s *core, uint32_t address, uint32_t value);

    uint32_t num_inst;
    uint32_t cur_inst_len;
    uint32_t cur_inst;

    char str_disasm_memory[2][50];
    uint32_t disasm_memory_ptr;
    bool exception_debugging;

    uint32_t disasm_prev_inst_pc;
    bool disasm_is_looping;

    uint32_t disasm_cur_inst;
    uint16_t disasm_cur_inst_len;

    char disasm_str_instr[256];
    char disasm_str_instr2[523];
    char disasm_parallelmove_name[64];

    uint32_t disasm_registers_save[64];
} dsp_core_t;

/* ============================================================
 * DSP DMA State (from dsp_dma.h)
 * ============================================================ */

typedef void (*dsp_scratch_rw_func)(
    void *opaque, uint8_t *ptr, uint32_t addr, size_t len, bool dir);
typedef void (*dsp_fifo_rw_func)(
    void *opaque, uint8_t *ptr, unsigned int index, size_t len, bool dir);

typedef struct DSPDMAState {
    dsp_core_t *core;

    void *rw_opaque;
    dsp_scratch_rw_func scratch_rw;
    dsp_fifo_rw_func fifo_rw;

    uint32_t configuration;
    uint32_t control;
    uint32_t start_block;
    uint32_t next_block;

    bool error;
    bool eol;
} DSPDMAState;

/* ============================================================
 * DSP State (from dsp_state.h)
 * ============================================================ */

typedef struct DSPState {
    dsp_core_t core;
    DSPDMAState dma;
    int save_cycles;
    uint32_t interrupts;
    bool is_gp;
} DSPState;

/* ============================================================
 * SVF (State Variable Filter) - from vp/svf.h
 * ============================================================ */

#define flush_to_zero(x) (x)
#define F_LP 1
#define F_HP 2
#define F_BP 3
#define F_BR 4
#define F_AP 5
#define F_R  1

typedef struct {
    float f;
    float q;
    float qnrm;
    float h;
    float b;
    float l;
    float p;
    float n;
    float *op;
} sv_filter;

static inline void setup_svf(sv_filter *sv, float fc, float q, int t) {
    sv->f = fc;
    sv->q = q;
    sv->qnrm = (float)sqrt(sv->q / 2.0 + 0.01);
    switch (t) {
    case F_LP: sv->op = &(sv->l); break;
    case F_HP: sv->op = &(sv->h); break;
    case F_BP: sv->op = &(sv->b); break;
    case F_BR: sv->op = &(sv->n); break;
    default:   sv->op = &(sv->p); break;
    }
}

static inline float run_svf(sv_filter *sv, float in) {
    float out;
    in = sv->qnrm * in;
    for (int i = 0; i < F_R; i++) {
        sv->b = flush_to_zero(sv->b - sv->b * sv->b * sv->b * 0.001f);
        sv->h = flush_to_zero(in - sv->l - sv->q * sv->b);
        sv->b = sv->b + sv->f * sv->h;
        sv->l = flush_to_zero(sv->l + sv->f * sv->b);
        sv->n = sv->l + sv->h;
        sv->p = sv->l - sv->h;
        out = *(sv->op);
        in = out;
    }
    return out;
}

/* ============================================================
 * HRTF Filter - from vp/hrtf.h
 * ============================================================ */

#define HRTF_SAMPLES_PER_FRAME  NUM_SAMPLES_PER_FRAME
#define HRTF_NUM_TAPS           31
#define HRTF_MAX_DELAY_SAMPLES  42
#define HRTF_BUFLEN             (HRTF_NUM_TAPS + HRTF_MAX_DELAY_SAMPLES)
#define HRTF_PARAM_SMOOTH_ALPHA 0.01f

typedef struct {
    int buf_pos;
    struct {
        float buf[HRTF_BUFLEN];
        float hrir_coeff_cur[HRTF_NUM_TAPS];
        float hrir_coeff_tar[HRTF_NUM_TAPS];
    } ch[2];
    float itd_cur;
    float itd_tar;
} HrtfFilter;

static inline void hrtf_filter_init(HrtfFilter *f) {
    memset(f, 0, sizeof(*f));
}

static inline void hrtf_filter_clear_history(HrtfFilter *f) {
    f->buf_pos = 0;
    memset(f->ch[0].buf, 0, sizeof(f->ch[0].buf));
    memset(f->ch[1].buf, 0, sizeof(f->ch[1].buf));
}

static inline void
hrtf_filter_set_target_params(HrtfFilter *f, float hrir_coeff[2][HRTF_NUM_TAPS],
                              float itd) {
    f->itd_tar = fmaxf(-(float)HRTF_MAX_DELAY_SAMPLES,
                        fminf(itd, (float)HRTF_MAX_DELAY_SAMPLES));
    for (int ch = 0; ch < 2; ch++) {
        float *coeff = f->ch[ch].hrir_coeff_tar;
        memcpy(coeff, hrir_coeff[ch], sizeof(f->ch[ch].hrir_coeff_tar));
        float s = 0.0f;
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            s += fabsf(coeff[k]);
        }
        if (s == 0.0f || s == 1.0f) break;
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            coeff[k] /= s;
        }
    }
}

static inline float hrtf_filter_smooth_param(float cur, float tar) {
    return cur + HRTF_PARAM_SMOOTH_ALPHA * (tar - cur);
}

static inline void hrtf_filter_step_parameters(HrtfFilter *f) {
    for (int ch = 0; ch < 2; ch++) {
        float *cc = f->ch[ch].hrir_coeff_cur;
        float *ct = f->ch[ch].hrir_coeff_tar;
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            cc[k] = hrtf_filter_smooth_param(cc[k], ct[k]);
        }
    }
    f->itd_cur = hrtf_filter_smooth_param(f->itd_cur, f->itd_tar);
}

static inline void hrtf_filter_process(HrtfFilter *f,
                                       float in[][2],
                                       float out[][2]) {
    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        hrtf_filter_step_parameters(f);
        for (int ch = 0; ch < 2; ch++) {
            float *buf = f->ch[ch].buf;
            float *coeff = f->ch[ch].hrir_coeff_cur;
            buf[f->buf_pos] = in[n][ch];
            float d = f->itd_cur * (ch == 0 ? +1.0f : -1.0f);
            if (d < 0.0f) d = 0.0f;
            int di = (int)d;
            float dfrac = d - di;
            float acc = 0.0f;
            for (int k = 0; k < HRTF_NUM_TAPS; k++) {
                int idx1 = (f->buf_pos - di - k + HRTF_BUFLEN) % HRTF_BUFLEN;
                float s = buf[idx1];
                if (dfrac > 0.0f) {
                    int idx2 = (idx1 - 1 + HRTF_BUFLEN) % HRTF_BUFLEN;
                    s = s * (1 - dfrac) + buf[idx2] * dfrac;
                }
                acc += coeff[k] * s;
            }
            out[n][ch] = acc;
        }
        f->buf_pos = (f->buf_pos + 1) % HRTF_BUFLEN;
    }
}

/* ============================================================
 * ADPCM Decoder - from vp/adpcm.h
 * ============================================================ */

static inline int adpcm_decode_block(int16_t *outbuf, const uint8_t *inbuf,
                                      size_t inbufsize, int channels) {
    #define ADPCM_CLIP(data, mn, mx) \
        if ((data) > (mx)) data = mx; \
        else if ((data) < (mn)) data = mn;

    static const uint16_t step_table[89] = {
        7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,
        34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
        157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,
        724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,
        3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,
        15289,16818,18500,20350,22385,24623,27086,29794,32767
    };
    static const int index_table[] = { -1, -1, -1, -1, 2, 4, 6, 8 };

    int samples = 1, chunks;
    int32_t pcmdata[2];
    int8_t index[2];

    if (inbufsize < (uint32_t)channels * 4) return 0;

    for (int ch = 0; ch < channels; ch++) {
        *outbuf++ = pcmdata[ch] = (int16_t)(inbuf[0] | (inbuf[1] << 8));
        index[ch] = inbuf[2];
        if (index[ch] < 0 || index[ch] > 88 || inbuf[3]) return 0;
        inbufsize -= 4;
        inbuf += 4;
    }

    chunks = (int)(inbufsize / (channels * 4));
    samples += chunks * 8;

    while (chunks--) {
        for (int ch = 0; ch < channels; ++ch) {
            for (int i = 0; i < 4; ++i) {
                int step = step_table[index[ch]], delta = step >> 3;
                if (*inbuf & 1) delta += (step >> 2);
                if (*inbuf & 2) delta += (step >> 1);
                if (*inbuf & 4) delta += step;
                if (*inbuf & 8) delta = -delta;
                pcmdata[ch] += delta;
                index[ch] += index_table[*inbuf & 0x7];
                ADPCM_CLIP(index[ch], 0, 88);
                ADPCM_CLIP(pcmdata[ch], -32768, 32767);
                outbuf[i * 2 * channels] = (int16_t)pcmdata[ch];

                step = step_table[index[ch]]; delta = step >> 3;
                if (*inbuf & 0x10) delta += (step >> 2);
                if (*inbuf & 0x20) delta += (step >> 1);
                if (*inbuf & 0x40) delta += step;
                if (*inbuf & 0x80) delta = -delta;
                pcmdata[ch] += delta;
                index[ch] += index_table[(*inbuf >> 4) & 0x7];
                ADPCM_CLIP(index[ch], 0, 88);
                ADPCM_CLIP(pcmdata[ch], -32768, 32767);
                outbuf[(i * 2 + 1) * channels] = (int16_t)pcmdata[ch];
                inbuf++;
            }
            outbuf++;
        }
        outbuf += channels * 7;
    }
    #undef ADPCM_CLIP
    return samples;
}

/* ============================================================
 * VP State (from vp/vp.h)
 * ============================================================ */

typedef struct MCPXAPUState MCPXAPUState;

typedef struct MCPXAPUVPSSLData {
    uint32_t base[MCPX_HW_SSLS_PER_VOICE];
    uint8_t count[MCPX_HW_SSLS_PER_VOICE];
    int ssl_index;
    int ssl_seg;
} MCPXAPUVPSSLData;

typedef struct MCPXAPUVoiceFilter {
    uint16_t voice;
    float resample_buf[NUM_SAMPLES_PER_FRAME * 2];
    SRC_STATE *resampler;
    sv_filter svf[2];
    HrtfFilter hrtf;
} MCPXAPUVoiceFilter;

typedef struct VoiceWorkItem {
    int voice;
    int list;
} VoiceWorkItem;

typedef struct VoiceWorker {
    QemuThread thread;
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    float sample_buf[NUM_SAMPLES_PER_FRAME][2];
    VoiceWorkItem queue[MCPX_HW_MAX_VOICES];
    int queue_len;
} VoiceWorker;

typedef struct VoiceWorkDispatch {
    QemuMutex lock;
    int num_workers;
    VoiceWorker *workers;
    bool workers_should_exit;
    QemuCond work_pending;
    uint64_t workers_pending;
    QemuCond work_finished;
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    VoiceWorkItem queue[MCPX_HW_MAX_VOICES];
    int queue_len;
} VoiceWorkDispatch;

typedef struct MCPXAPUVPState {
    MemoryRegion mmio;
    VoiceWorkDispatch voice_work_dispatch;
    MCPXAPUVoiceFilter filters[MCPX_HW_MAX_VOICES];

    int ssl_base_page;
    MCPXAPUVPSSLData ssl[MCPX_HW_MAX_VOICES];
    uint8_t hrtf_headroom;
    uint8_t hrtf_submix[4];
    uint8_t submix_headroom[NUM_MIXBINS];
    float sample_buf[NUM_SAMPLES_PER_FRAME][2];
    uint64_t voice_locked[4];

    struct {
        int current_entry;
        struct {
            float hrir[2][HRTF_NUM_TAPS];
            float itd;
        } entries[HRTF_ENTRY_COUNT];
    } hrtf;

    uint32_t inbuf_sge_handle;
    uint32_t outbuf_sge_handle;
} MCPXAPUVPState;

/* ============================================================
 * GP/EP State (from dsp/gp_ep.h)
 * ============================================================ */

typedef struct MCPXAPUGPState {
    bool realtime;
    MemoryRegion mmio;
    DSPState *dsp;
    uint32_t regs[0x10000];
} MCPXAPUGPState;

typedef struct MCPXAPUEPState {
    bool realtime;
    MemoryRegion mmio;
    DSPState *dsp;
    uint32_t regs[0x10000];
} MCPXAPUEPState;

/* ============================================================
 * Main APU State (from apu_int.h)
 * ============================================================ */

struct MCPXAPUState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    bool exiting;
    bool set_irq;

    QemuThread apu_thread;
    QemuMutex lock;
    QemuCond cond;
    QemuCond idle_cond;
    bool pause_requested;
    bool is_idle;

    MemoryRegion *ram;
    uint8_t *ram_ptr;
    MemoryRegion mmio;

    MCPXAPUVPState vp;
    MCPXAPUGPState gp;
    MCPXAPUEPState ep;

    uint32_t regs[0x20000];

    int ep_frame_div;
    int sleep_acc_us;
    int frame_count;
    int64_t frame_count_time_ms;
    int64_t next_frame_time_us;

    struct {
        struct {
            int backoff, ok, speedup;
        } pacing;
        struct {
            int64_t last_us;
            int64_t min_us, max_us, sum_us;
            int count;
        } deviation;
        int queued_bytes_min, queued_bytes_max;
        int64_t queued_bytes_sum;
        int queued_bytes_count;
    } throttle;

    struct {
        McpxApuDebugMonitorPoint point;
        int16_t frame_buf[256][2];
        void *stream; /* SDL_AudioStream* - stubbed */
        int queued_bytes_low, queued_bytes_high;
    } monitor;
};

/* ============================================================
 * Forward declarations for APU sub-module functions
 * ============================================================ */

/* VP functions */
void mcpx_apu_vp_init(MCPXAPUState *d);
void mcpx_apu_vp_finalize(MCPXAPUState *d);
void mcpx_apu_vp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]);
void mcpx_apu_vp_reset(MCPXAPUState *d);

/* DSP functions (stubbed) */
void mcpx_apu_dsp_init(MCPXAPUState *d);
void mcpx_apu_update_dsp_preference(MCPXAPUState *d);
void mcpx_apu_dsp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]);

/* Debug globals */
extern MCPXAPUState *g_state;
extern struct McpxApuDebug g_dbg, g_dbg_cache;
extern int g_dbg_voice_monitor;
extern uint64_t g_dbg_muted_voices[4];
