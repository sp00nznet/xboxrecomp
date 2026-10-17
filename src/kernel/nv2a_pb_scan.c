/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 *
 * The walk goes from GET to PUT the way the DMA engine does: a jump moves
 * GET to its target (new style w & ~3, old style w & 0x1FFFFFFC), a call
 * does the same and keeps one return address, and a return goes back to it.
 * So a ring wrap is the title's own jump back to the start, and a segment
 * with a jump in it goes on after it to PUT.
 *
 * Anything else is a word the walk is out of step on, and it stops there
 * rather than guess: an unknown word, a call inside a call, or a word or
 * target outside the contiguous window (a parameter misread as a jump can
 * point anywhere in 256 MB, and only 64 MB is mapped).
 */
#include <stdio.h>
#include "recomp_env.h"
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>

#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */
#include "nv2a_pb_state.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;

/* Walks that stopped short of PUT. Counted because after eight warning lines
 * a stop is otherwise silent. */
static uint32_t s_tot_stops, s_tot_salvaged, s_catchups;
uint32_t nv2a_pb_scan_stops(void) { return s_tot_stops; }

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
extern uint32_t nv2a_pb_exec_semaphore_last_value(void);
extern uint32_t nv2a_pb_exec_semaphore_releases(void);
extern void nv2a_pb_perf_scan_begin(void);
extern void nv2a_pb_perf_scan_end(void);
static int s_exec_enabled = -1;
static int s_scan_enabled = -1;   /* RECOMP_PB_SCAN, read once */

/* s_seen's slot for each (subchannel, method), plus one (0: not seen yet).
 * The walker calls note() for every method in the stream, ~100,000 a frame in
 * a busy scene, and a linear search of a few hundred pairs there was
 * half the walker's time. s_seen keeps first-seen order for the report. */
static uint16_t s_seen_slot[8][0x2000 / 4];

static void note(uint32_t subch, uint32_t method)
{
    static uint16_t none;
    uint16_t *slot = &none;

    /* An incrementing run can carry the method past 0x1FFC; those (rare)
     * keep the linear search. */
    if (method < 0x2000 && !(method & 3)) {
        slot = &s_seen_slot[subch & 7][method >> 2];
        if (*slot) {
            s_seen[*slot - 1].count++;
            return;
        }
    } else {
        for (int i = 0; i < s_seen_count; i++) {
            if (s_seen[i].method == method && s_seen[i].subch == subch) {
                s_seen[i].count++;
                return;
            }
        }
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_count++;
        if (slot != &none)
            *slot = (uint16_t)s_seen_count;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0110, "WAIT_FOR_IDLE" },
    { 0x0120, "SET_FLIP_READ" },
    { 0x0124, "SET_FLIP_WRITE" },
    { 0x0128, "SET_FLIP_MODULO" },
    { 0x012C, "FLIP_INCREMENT_WRITE" },
    { 0x0130, "FLIP_STALL" },
    { 0x01A4, "SET_CONTEXT_DMA_SEMAPHORE" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_ZSTENCIL_CLEAR_VALUE" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_SEMAPHORE_OFFSET" },
    { 0x1D70, "BACK_END_WRITE_SEMAPHORE_RELEASE" },
    { 0x1D98, "SET_CLEAR_RECT_HORIZONTAL" },
    { 0x1D9C, "SET_CLEAR_RECT_VERTICAL" },
    { 0x0B00, "SET_TRANSFORM_PROGRAM" },
    { 0x0B80, "SET_TRANSFORM_CONSTANT" },
    { 0x1EA4, "SET_TRANSFORM_CONSTANT_LOAD" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "ARRAY_ELEMENT32" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1818, "INLINE_ARRAY" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B10, "SET_TEXTURE_CONTROL1" },
    { 0x1B14, "SET_TEXTURE_FILTER" },
    { 0x1B1C, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0)
        nv2a_pb_exec_report();
    if (!s_seen_count || !recomp_env(RENV_PB_SCAN))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u unrecognised,"
                    " %u stopped (%u with a fence release salvaged, %u catch-ups)"
                    " -- %d distinct (subchannel, method) pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_unknown,
            s_tot_stops, s_tot_salvaged, s_catchups, s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* Once per pass of the ack thread's loop: the backend's on_poll, so the
 * queries of reports already settled can come back to the pool while no
 * pushbuffer comes. The walk itself settles its reports
 * (nv2a_pb_exec_walk_end). */
void nv2a_pb_poll(void)
{
    const struct nv2a_pb_backend *b;
    if (s_exec_enabled <= 0)
        return;
    b = nv2a_pb_get_backend();
    if (b && b->on_poll)
        b->on_poll();
}

static int pb_in_window(uint32_t va)
{
    return (va & 3u) == 0 && va - XBOX_CONTIG_BASE <= XBOX_CONTIG_SIZE - 4u;
}

/* The words from a stop to PUT are never executed, and they may hold the
 * semaphore release the title's next fence wait spins on. stop_salvage
 * runs that release itself. When it cannot search the span, or finds
 * releases there but none it trusts, it asks for a catch-up instead: the
 * fence mirror then writes the title's submitted count once
 * (fence_mirrors_tick). */
uint32_t nv2a_pb_scan_catchups(void) { return s_catchups; }

/* The XDK writes a fence as the pair (BACK_END_WRITE_SEMAPHORE_RELEASE,
 * count 1, subchannel 0) and its value. Run the last such release found in
 * [from, to): releasing that one marks all the earlier ones done too, and
 * none beyond PUT. Mirroring the submitted count would also pass fences
 * the title has not yet handed over, and a resource it is still using
 * could then be freed.
 *
 * The pair can also be data: ARRAY_ELEMENT16 indices 7536, 4 read as
 * 0x00041D70. A value taken from data could jump the fence far ahead,
 * which would turn fences off for the rest of the run. So a candidate
 * counts only if it looks like the next fence: odd (the XDK's fence values
 * are), after the last release, and at most 128 past it. Returns how the
 * fence was handled, for the log. */
static const char *stop_salvage(const uint8_t *mem, uint32_t from, uint32_t to,
                                uint32_t *value)
{
    uint32_t va, last, rel;
    int found = 0, seen = 0;

    if (!s_exec_enabled)
        return "not executing";
    if (from >= to || !pb_in_window(from) || !pb_in_window(to - 4)) {
        s_catchups++;
        return "span wraps or is unreadable; fence catch-up requested";
    }
    last = nv2a_pb_exec_semaphore_last_value();
    for (va = from; va + 4 < to; va += 4) {
        if (*(const uint32_t *)(mem + va) == 0x00041D70u) {
            uint32_t v = *(const uint32_t *)(mem + va + 4);
            seen++;
            if ((v & 1u) && (int32_t)(v - last) > 0 && v - last <= 128u) {
                last = v;
                found = 1;
            }
        }
    }
    if (!found && seen) {
        /* Data, or a real release the check turned down (an even value,
         * or one far past a stale last value): not running it could leave
         * the title waiting for ever, so fall back to the mirror. */
        s_catchups++;
        return "release(s) in the rest but none plausible; catch-up requested";
    }
    if (!found)
        return "no fence release in the rest";
    rel = nv2a_pb_exec_semaphore_releases();
    nv2a_pb_exec_method(0, 0x1D70u, last);
    *value = last;
    if (nv2a_pb_exec_semaphore_releases() == rel)
        return "fence release found but refused by the executor";
    s_tot_salvaged++;
    return "fence release salvaged";
}

/* A method header's parameters can go past PUT: the title may submit
 * the header and some of its data now and the rest with the next PUT. So
 * the method in progress outlives one walk. */
static uint32_t s_left, s_subch, s_method;
static int s_noninc;

/* Whether the walk is inside a pushbuffer CALL, for the executor: a flip
 * hold can only resume a walk at the top level, since the return address
 * lives in this walk alone. */
static int s_in_call;
int nv2a_pb_scan_in_call(void) { return s_in_call; }

void nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t va = start_va, ret_va = 0, at = start_va, w = 0;
    uint32_t words = 0, jumps = 0, unknown = 0;
    static int inject = -1;
    const char *why = NULL;
    int in_call = 0;

    if (s_exec_enabled < 0)
        s_exec_enabled = recomp_env(RENV_PB_EXEC) != NULL;
    if (s_scan_enabled < 0)
        s_scan_enabled = recomp_env(RENV_PB_SCAN) != NULL;
    if (!(s_scan_enabled || s_exec_enabled) || end_va == start_va)
        return;
    if (s_exec_enabled) {
        extern void nv2a_pb_exec_walk_begin(void);
        nv2a_pb_perf_scan_begin();
        nv2a_pb_exec_walk_begin();      /* the flip hold's vblank */
    }
    /* Test hook: RECOMP_PB_INJECT_STOP=N stops every Nth walk at its first
     * word, so the recovery from a stop can be seen without a real desync.
     * Never the first walk: it sets up the semaphore, and without that the
     * executor never takes the fence over and there is nothing to recover. */
    if (inject < 0)
        inject = recomp_env(RENV_PB_INJECT_STOP)
               ? atoi(recomp_env(RENV_PB_INJECT_STOP)) : 0;
    if (inject > 0 && s_tot_segments && s_tot_segments % (uint32_t)inject == 0)
        why = "injected (RECOMP_DEBUG=pb_inject_stop)";

    /* Bounded at 4 MB of words, a sane single-frame amount: GET that never
     * reaches PUT is a stream this walk is out of step with. */
    while (!why && va != end_va) {
        uint32_t tgt;

        at = va;
        w = 0;
        if (words >= 0x100000u) { why = "word limit"; break; }
        if (!pb_in_window(va)) { why = "outside the contiguous window"; break; }
        w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if (s_left) {                         /* a parameter */
            note(s_subch, s_method);
            /* Same walk, two consumers: the survey counts, the executor
             * acts. Keeping them on one decode means they can never
             * disagree about what the stream said. */
            if (s_exec_enabled)
                nv2a_pb_exec_method(s_subch, s_method, w);
            if (!s_noninc)
                s_method += 4;
            s_left--;
            continue;
        }
        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            jumps++;
            tgt = (w & 3u) == 1u ? w & ~3u : w & 0x1FFFFFFCu;
            if (tgt >= XBOX_CONTIG_SIZE) { why = "jump outside the window"; break; }
            va = XBOX_CONTIG_BASE | tgt;
            continue;
        }
        if ((w & 3u) == 2u) {                 /* call */
            jumps++;
            tgt = w & ~3u;
            if (in_call) { why = "call inside a call"; break; }
            if (tgt >= XBOX_CONTIG_SIZE) { why = "call outside the window"; break; }
            ret_va = va;
            in_call = s_in_call = 1;
            va = XBOX_CONTIG_BASE | tgt;
            continue;
        }
        if (w == 0x00020000u) {               /* return */
            if (!in_call) { why = "return outside a call"; break; }
            va = ret_va;
            in_call = s_in_call = 0;
            continue;
        }
        if ((w & 0xE0030003u) == 0u || (w & 0xE0030003u) == 0x40000000u) {
            s_left   = (w >> 18) & 0x7FFu;
            s_subch  = (w >> 13) & 7u;
            s_method =  w & 0x1FFCu;
            s_noninc = (w & 0xE0000000u) == 0x40000000u;
            continue;
        }
        unknown++;
        why = "unknown word";
        break;
    }
    s_in_call = 0;
    /* The walk's reports, before GET moves or a stop's salvage releases a
     * semaphore: the title may read them once either happens. */
    if (s_exec_enabled) {
        extern void nv2a_pb_exec_walk_end(void);
        nv2a_pb_exec_walk_end();
    }
    if (why) {
        static unsigned said;
        extern void nv2a_pb_exec_flip_cancel(void);
        s_left = 0;                 /* out of step: drop the method in progress */
        if (s_exec_enabled)
            nv2a_pb_exec_flip_cancel();
        uint32_t v = 0;
        const char *fence = stop_salvage(mem, at, end_va, &v);
        s_tot_stops++;
        if (said++ < 8)
            fprintf(stderr, "[PB] walk 0x%08X -> 0x%08X stopped at 0x%08X"
                    " (word 0x%08X) after %u words without reaching PUT (%s);"
                    " %s 0x%08X\n",
                    start_va, end_va, at, w, words, why, fence, v);
    }

    if (s_exec_enabled)
        nv2a_pb_perf_scan_end();
    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}
