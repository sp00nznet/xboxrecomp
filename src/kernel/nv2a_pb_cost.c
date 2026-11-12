/* Per-batch cost by stage (nv2a_pb_cost.h). */
#include "nv2a_pb_cost.h"
#include "nv2a_vsh_cpu.h"
#include "nv2a_backend_common.h"
#include "recomp_env.h"
#include <stdio.h>
#include <string.h>

extern uint64_t nv2a_pb_now_ns(void);

int nv2a_pb_cost_on;
uint64_t nv2a_pb_cost_ticks[NV2A_COST_N];

static const char *const k_stage[NV2A_COST_N] = {
    "pre", "setup", "ao", "streams", "ib", "aspect", "consts", "bind", "px_stages",
    "px_aspect", "px_bind", "px_shader", "pixel",
    "place", "draw", "d_exec", "d_ckpt", "d_walk", "d_clear", "d_poll", "d_report",
    "d_blit", "d_wake", "f_head", "f_present", "f_hooks", "f_stats", "f_dumps", "f_log",
    "f_tail"
};

/* Per Kelvin method (0x0000-0x1FFC, by word) and one row for the other
 * subchannels: inclusive ticks and calls since the last table. */
#define COST_M_ROWS 2049u
static uint64_t s_m_ticks[COST_M_ROWS];
static uint32_t s_m_calls[COST_M_ROWS];
static uint32_t s_m_flips;

void nv2a_pb_cost_method(uint32_t subch, uint32_t method, uint64_t t)
{
    uint32_t row;
    if (!t)
        return;
    row = subch || method >= 0x2000u ? COST_M_ROWS - 1u : method >> 2;
    s_m_ticks[row] += nv2a_pb_cost_tick() - t;
    s_m_calls[row]++;
}

/* Every 300 flips: the 12 methods with the most inclusive time, in ms and
 * calls a flip. */
static void cost_method_table(uint32_t flip, double ns_per_tick)
{
    char line[1024];
    uint32_t top[12], k, i;
    int n, nt = 0;

    if (++s_m_flips < 300u)
        return;
    for (k = 0; k < 12; k++) {
        uint32_t best = COST_M_ROWS;
        for (i = 0; i < COST_M_ROWS; i++) {
            uint32_t j;
            int used = 0;
            for (j = 0; j < k; j++)
                used |= top[j] == i;
            if (!used && s_m_ticks[i] && (best == COST_M_ROWS || s_m_ticks[i] > s_m_ticks[best]))
                best = i;
        }
        if (best == COST_M_ROWS)
            break;
        top[k] = best;
        nt++;
    }
    n = snprintf(line, sizeof line, "[PB-COST-M] flip %u over %u flips:", flip, s_m_flips);
    for (k = 0; k < (uint32_t)nt && n > 0 && (size_t)n < sizeof line; k++) {
        uint32_t r = top[k];
        if (r == COST_M_ROWS - 1u)
            n += snprintf(line + n, sizeof line - (size_t)n, " other");
        else
            n += snprintf(line + n, sizeof line - (size_t)n, " %04X", r << 2);
        if (n > 0 && (size_t)n < sizeof line)
            n += snprintf(line + n, sizeof line - (size_t)n, " %.3f/%u",
                          (double)s_m_ticks[r] * ns_per_tick / 1e6 / s_m_flips,
                          s_m_calls[r] / s_m_flips);
    }
    fprintf(stderr, "%s\n", line);
    memset(s_m_ticks, 0, sizeof s_m_ticks);
    memset(s_m_calls, 0, sizeof s_m_calls);
    s_m_flips = 0;
}

/* The counter's rate, from the last flip to this one against the walker's
 * clock: the TSC's is not known in advance (and under Proton not asked for),
 * and cntvct_el0's is, but one path serves both. */
static struct {
    uint64_t tick, ns;
    uint32_t vsh_runs;
    uint64_t vsh_steps, rt_hashes, rt_bytes, rt_ns, slice_hits, slice_misses;
} s_cost_last;

void nv2a_pb_cost_init(void)
{
    nv2a_pb_cost_on = recomp_env(RENV_PB_COST) != NULL;
    nv2a_vsh_count_on = nv2a_rt_own_cost_on = nv2a_pb_cost_on;
    /* The [PB-COST] line rides on the [PB-PERF] one, which only the flip
     * trace prints: say so once rather than collect and print nothing. */
    if (nv2a_pb_cost_on && !recomp_env(RENV_FLIP_LOG))
        fprintf(stderr, "[PB-COST] pb_cost prints with the flip trace only:"
                        " add flip to RECOMP_TRACE\n");
    if (nv2a_pb_cost_on) {
        s_cost_last.tick = nv2a_pb_cost_tick();
        s_cost_last.ns = nv2a_pb_now_ns();
    }
}

void nv2a_pb_cost_flip(uint32_t flip, uint32_t batches, uint32_t draws)
{
    char line[768];
    uint64_t tick, ns, sum = 0;
    double ns_per_tick;
    int i, n;

    if (!nv2a_pb_cost_on)
        return;
    tick = nv2a_pb_cost_tick();
    ns = nv2a_pb_now_ns();
    ns_per_tick = tick > s_cost_last.tick
                ? (double)(ns - s_cost_last.ns) / (double)(tick - s_cost_last.tick) : 0.0;
    n = snprintf(line, sizeof line, "[PB-COST] flip %u batches %u draws %u", flip,
                 batches, draws);
    for (i = 0; i < NV2A_COST_N && n > 0 && (size_t)n < sizeof line; i++) {
        if (i < NV2A_COST_DRAW_N)
            sum += nv2a_pb_cost_ticks[i];
        n += snprintf(line + n, sizeof line - (size_t)n, " %s %.3f", k_stage[i],
                      (double)nv2a_pb_cost_ticks[i] * ns_per_tick / 1e6);
    }
    if (n > 0 && (size_t)n < sizeof line)
        snprintf(line + n, sizeof line - (size_t)n,
                 " sum %.3f ms vsh %u steps %llu rt_hash %llu kb %llu us %llu"
                 " slice_hit %llu slice_miss %llu",
                 (double)sum * ns_per_tick / 1e6, nv2a_vsh_run_count - s_cost_last.vsh_runs,
                 (unsigned long long)(nv2a_vsh_step_count - s_cost_last.vsh_steps),
                 (unsigned long long)(nv2a_rt_own_hashes() - s_cost_last.rt_hashes),
                 (unsigned long long)((nv2a_rt_own_hash_bytes() - s_cost_last.rt_bytes) >> 10),
                 (unsigned long long)((nv2a_rt_own_hash_ns() - s_cost_last.rt_ns) / 1000u),
                 (unsigned long long)(nv2a_vsh_slice_hits - s_cost_last.slice_hits),
                 (unsigned long long)(nv2a_vsh_slice_misses - s_cost_last.slice_misses));
    fprintf(stderr, "%s\n", line);
    cost_method_table(flip, ns_per_tick);
    memset(nv2a_pb_cost_ticks, 0, sizeof nv2a_pb_cost_ticks);
    s_cost_last.tick = tick;
    s_cost_last.ns = ns;
    s_cost_last.vsh_runs = nv2a_vsh_run_count;
    s_cost_last.vsh_steps = nv2a_vsh_step_count;
    s_cost_last.rt_hashes = nv2a_rt_own_hashes();
    s_cost_last.rt_bytes = nv2a_rt_own_hash_bytes();
    s_cost_last.rt_ns = nv2a_rt_own_hash_ns();
    s_cost_last.slice_hits = nv2a_vsh_slice_hits;
    s_cost_last.slice_misses = nv2a_vsh_slice_misses;
}
