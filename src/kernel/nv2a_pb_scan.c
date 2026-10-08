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
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;
static uint32_t s_tot_calls;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
static int s_exec_enabled = -1;
static int s_scan_enabled = -1;

/* Clears the NV2A request bits (xbox_memory_layout.c); called every 256
 * words while executing, so kickoffs are answered mid-walk. */
extern void xbox_Nv2aAckBusyBits(void);
static uint32_t s_ack_tick;

/* Either switch, read once. The answer "not set" has to be cached as well as
 * "set": this runs on every pass of the NV2A poll loop, and on the default
 * path -- neither switch set -- a cache that only remembered "set" called
 * getenv on every pass, which walks the whole environment. */
static int pb_walk_enabled(void)
{
    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    if (s_scan_enabled < 0)
        s_scan_enabled = getenv("RECOMP_PB_SCAN") != NULL;
    return s_exec_enabled || s_scan_enabled;
}

/* Slot + 1 of each (subchannel, method) in s_seen. note() runs for every
 * pushbuffer word, so a linear search here was the executor's largest cost
 * (about 40% of its thread in X-Men Legends' levels). */
static uint16_t s_seen_slot[8][0x2000 / 4];

static void note(uint32_t subch, uint32_t method)
{
    uint16_t *slot = &s_seen_slot[subch & 7][(method & 0x1FFC) / 4];

    if (*slot) {
        s_seen[*slot - 1].count++;
        return;
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
    { 0x0130, "SET_FLIP_READ" },
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
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "INLINE_ARRAY" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
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

    if (s_exec_enabled > 0) {
        nv2a_pb_exec_report();
        fprintf(stderr, "[PB] %u pushbuffer calls followed\n", s_tot_calls);
    }
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u calls,"
                    " %u unrecognised -- %d distinct (subchannel, method)"
                    " pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_calls,
            s_tot_unknown, s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* Walk the pushbuffer the way the GPU's DMA engine does, from our own read
 * position up to the title's DMA_PUT.
 *
 * This used to scan only the bytes between the previous PUT and this one,
 * stop at the first jump, skip subroutine calls, and skip the whole segment
 * whenever PUT wrapped back to the start of the ring. Each of those drops
 * commands: XDK D3D sends state and vertex-program uploads through CALLs into
 * pre-built pushbuffers, and every wrap lost a segment. What survived drew
 * with stale state -- untransformed meshes run through the pre-transformed
 * pass-through program, garbled text.
 *
 * So: keep a GET of our own (physical, like the register), follow JUMP and
 * CALL/RETURN (NV2A has one level of subroutine), and stop when GET reaches
 * PUT. Addresses are physical; the contiguous window at XBOX_CONTIG_BASE is
 * the physical view.
 *
 * ponytail: if the walk desynchronises (a run of words that decode as
 * nothing), GET snaps to PUT rather than wandering through guest RAM. */
#define PB_PHYS_MASK 0x0FFFFFFCu
#define PB_RAM_BYTES (64u * 1024u * 1024u)   /* XBOX_CONTIG_SIZE */

/* A jump or call outside RAM means the walk is reading data as commands. */
/* The last command headers decoded, so a desync can be traced back to the
 * command that caused it. */
#define PB_HIST 8
static struct { uint32_t at, w; } s_hist[PB_HIST];
static unsigned s_hist_n;
/* The last jumps/calls/returns taken, and resyncs (w = 0xFFFFFFFF). */
static struct { uint32_t at, w, to; } s_flow[PB_HIST];
static unsigned s_flow_n;
static void pb_flow(uint32_t at, uint32_t w, uint32_t to)
{
    s_flow[s_flow_n % PB_HIST].at = at;
    s_flow[s_flow_n % PB_HIST].w = w;
    s_flow[s_flow_n % PB_HIST].to = to;
    s_flow_n++;
}

static int pb_bad_target(uint32_t w, uint32_t at, uint32_t target, uint32_t put)
{
    static unsigned total;
    unsigned k;
    if (target < PB_RAM_BYTES)
        return 0;
    if (++total <= 16 || total % 100 == 0) {
        fprintf(stderr, "  [PB] bad target #%u 0x%08X from word 0x%08X at 0x%08X"
                        " (PUT 0x%08X); resync\n", total, target, w, at, put);
        if (total <= 4)
            for (k = 0; k < PB_HIST && k < s_hist_n; k++) {
                unsigned i = (s_hist_n - 1 - k) % PB_HIST;
                fprintf(stderr, "  [PB]   earlier header 0x%08X at 0x%08X\n",
                        s_hist[i].w, s_hist[i].at);
            }
        if (total <= 4)
            for (k = 0; k < PB_HIST && k < s_flow_n; k++) {
                unsigned i = (s_flow_n - 1 - k) % PB_HIST;
                fprintf(stderr, "  [PB]   earlier flow 0x%08X at 0x%08X -> 0x%08X\n",
                        s_flow[i].w, s_flow[i].at, s_flow[i].to);
            }
    }
    return 1;
}
static uint32_t s_get = 0xFFFFFFFFu, s_ret;
static int      s_in_call;

/* A method whose parameters the walk has not all reached: the header said
 * `count` words and PUT came first. The DMA engine stalls at PUT and carries
 * on with the same method when PUT moves; reading the next word as a new
 * header instead would decode vertex data as commands. (D3D streams large
 * inline arrays across several kicks, so this is routine, not a desync.) */
static uint32_t s_pend_left, s_pend_i, s_pend_method, s_pend_subch;
static int      s_pend_noninc;

/* DMA_GET is the GPU's fetch pointer: nothing at or past it has been read, and
 * XDK D3D relies on exactly that. Its fence wait reads GET and, when the fence
 * is far enough on, patches a "wake me" software method into the pushbuffer at
 * the fence and sleeps on an event. If GET lagged the executor -- it used to
 * move only when the walk reached PUT -- D3D patched commands the executor had
 * already passed and slept for good. So under RECOMP_PB_EXEC the walk publishes
 * its position before every command (s_pub is the last value it wrote); a GET
 * that is not what it last wrote means the title moved it, and the walk
 * follows. */
extern volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);
#define NV2A_USER_DMA_GET 0x800044u
static uint32_t s_pub = 0xFFFFFFFFu;

/* The title moved GET itself: XDK D3D resets the ring by writing DMA_PUT and
 * DMA_GET together (CDevice::KickOff's restart path). Walking on from the old
 * position would read whatever lies between as commands. */
void nv2a_pb_resync(uint32_t get_phys)
{
    if (!pb_walk_enabled())
        return;
    pb_flow(s_get, 0xFFFFFFFFu, get_phys & PB_PHYS_MASK);
    s_get = get_phys & PB_PHYS_MASK;
    s_in_call = 0;
    s_pend_left = 0;
    s_pub = s_get;
}

/* Feed the parameters of the method in progress to the survey and the
 * executor, up to PUT. */
static void pb_consume(const uint8_t *mem, uint32_t put, uint32_t *words)
{
    while (s_pend_left && s_get < PB_RAM_BYTES && s_get != put) {
        uint32_t m = s_pend_noninc ? s_pend_method : s_pend_method + s_pend_i * 4;
        uint32_t param = *(const uint32_t *)(mem + (0x80000000u | s_get));

        note(s_pend_subch, m);
        /* Same walk, two consumers: the survey counts, the executor
         * acts. Keeping them on one decode means they can never
         * disagree about what the stream said. */
        if (s_exec_enabled)
            nv2a_pb_exec_method(s_pend_subch, m, param);
        s_get = (s_get + 4) & PB_PHYS_MASK;
        s_pend_i++;
        s_pend_left--;
        (*words)++;
    }
}

void nv2a_pb_scan(uint32_t put_phys)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t put = put_phys & PB_PHYS_MASK;
    volatile uint32_t *get_reg = NULL;
    uint32_t words = 0, jumps = 0, unknown = 0;

    if (!pb_walk_enabled())
        return;
    if (s_get == 0xFFFFFFFFu) {               /* never resynced: start at PUT */
        s_get = put, s_pend_left = 0;
        return;
    }
    if (s_exec_enabled)
        get_reg = xbox_Nv2aRegPtr(NV2A_USER_DMA_GET);

    pb_consume(mem, put, &words);             /* the method PUT stalled inside */

    while (s_get != put) {
        uint32_t at, target;
        uint32_t w;

        if (get_reg) {
            uint32_t g = *get_reg;

            if (g != s_pub) {                 /* the title moved GET mid-walk */
                pb_flow(s_get, 0xFFFFFFFFu, g & PB_PHYS_MASK);
                s_get = g & PB_PHYS_MASK;
                s_in_call = 0;
                s_pend_left = 0;
                s_pub = s_get;
                continue;
            }
            *get_reg = s_get;
            s_pub = s_get;
        }
        at = s_get;

        /* Keep answering kickoffs while executing; see xbox_Nv2aAckBusyBits. */
        if (s_exec_enabled && (++s_ack_tick & 0xFFu) == 0)
            xbox_Nv2aAckBusyBits();

        if (s_get >= PB_RAM_BYTES) {
            s_get = put, s_pend_left = 0;
            break;
        }
        w = *(const uint32_t *)(mem + (0x80000000u | s_get));
        s_hist[s_hist_n % PB_HIST].at = at;
        s_hist[s_hist_n % PB_HIST].w = w;
        s_hist_n++;

        if (++words > 0x400000u || unknown > 64) {
            s_get = put, s_pend_left = 0;                      /* lost sync, or runaway */
            break;
        }
        s_get = (s_get + 4) & PB_PHYS_MASK;

        if ((w & 0xE0000003u) == 0x20000000u) {         /* old-style jump */
            target = w & 0x1FFFFFFCu;
            if (pb_bad_target(w, at, target, put)) { s_get = put, s_pend_left = 0; break; }
            pb_flow(at, w, target);
            s_get = target;
            jumps++;
            continue;
        }
        if ((w & 3u) == 1u) {                            /* jump */
            target = w & 0xFFFFFFFCu;
            if (pb_bad_target(w, at, target, put)) { s_get = put, s_pend_left = 0; break; }
            pb_flow(at, w, target);
            s_get = target;
            jumps++;
            continue;
        }
        if ((w & 3u) == 2u) {                            /* call */
            target = w & 0xFFFFFFFCu;
            if (pb_bad_target(w, at, target, put)) { s_get = put, s_pend_left = 0; break; }
            if (s_in_call) {
                /* One level only: the hardware raises a CALL error and stops
                 * the pusher. Taking it would overwrite the return address
                 * and send the first RETURN to the wrong place. */
                s_in_call = 0;
                s_get = put, s_pend_left = 0;
                break;
            }
            s_ret = s_get;
            s_in_call = 1;
            s_tot_calls++;
            pb_flow(at, w, target);
            s_get = target;
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {          /* return */
            if (s_in_call) {
                pb_flow(at, w, s_ret);
                s_get = s_ret;
                s_in_call = 0;
            }
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            /* PUT bounds the parameters too. A count that runs past it must
             * not carry s_get beyond PUT (the outer loop would then replay a
             * lap of stale ring contents), nor be forgotten (the leftover
             * parameters would be read as headers): the method stays pending
             * and the next walk finishes it. */
            s_pend_left = count;
            s_pend_i = 0;
            s_pend_method = method;
            s_pend_subch = subch;
            s_pend_noninc = noninc;
            pb_consume(mem, put, &words);
            unknown = 0;
            continue;
        }
        unknown++;
    }

    if (get_reg) {                            /* what the poll thread writes too */
        *get_reg = s_get;
        s_pub = s_get;
    }
    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}

/* RECOMP_PB_WALK=dma selects the DMA-engine walk above. The default is the
 * segment scan below, which is what the executor did before the walk existed.
 *
 * The walk is the more faithful of the two: it follows JUMPs and ring wraps,
 * keeps its own GET, publishes it before every command, and answers kickoffs
 * mid-walk. That fidelity changes what XDK D3D does. Told exactly how far the
 * GPU has got, D3D's fence code takes its "GPU far behind" path: it patches a
 * software method (NO_OPERATION with a value) into the pushbuffer and sleeps
 * on an event that the GPU's interrupt for that method signals. A runtime that
 * does not deliver that interrupt leaves the title asleep for good -- a title
 * that ran under the segment scan froze on the first screen whose pushbuffer
 * made the walk long enough. So the walk is opt-in, for projects that deliver
 * the GPU's software-method interrupt. */
int nv2a_pb_dma_walk(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_PB_WALK");
        on = e && !strcmp(e, "dma");
    }
    return on;
}

/* The segment scan: walk commands from va to end_va. Returns the words
 * consumed.
 *
 * A CALL runs a subroutine -- a pushbuffer somewhere else in memory -- up to
 * its RETURN, then carries on after the CALL. RenderWare on the Xbox compiles
 * static geometry into pushbuffers and draws it with a CALL, so skipping CALLs
 * loses whole meshes. The NV2A has one level of subroutine, so a CALL inside a
 * call is not followed. A JUMP outside a call ends the segment: the ring's
 * wrap is handled by the caller, which scans the two pieces separately.
 * Addresses are physical, like PUT's, and reach guest memory through the
 * contiguous window. */
static uint32_t pb_walk_segment(uint32_t va, uint32_t end_va, int in_call,
                                uint32_t *jumps, uint32_t *unknown)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t words = 0;

    while (va < end_va && words < 0x100000u) {
        uint32_t w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            uint32_t target = (w & 3u) == 1u ? (w & 0xFFFFFFFCu)
                                             : (w & 0x1FFFFFFCu);
            (*jumps)++;
            if (!in_call)
                break;                        /* the ring: a jump ends it */
            va = 0x80000000u | (target & 0x0FFFFFFFu);
            end_va = va + 0x400000u;
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {  /* RETURN */
            if (in_call)
                break;
            continue;
        }
        if ((w & 3u) == 2u) {                    /* CALL */
            if (!in_call) {
                uint32_t sub = 0x80000000u | ((w & 0xFFFFFFFCu) & 0x0FFFFFFFu);
                s_tot_calls++;
                words += pb_walk_segment(sub, sub + 0x400000u, 1, jumps, unknown);
            }
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            for (uint32_t i = 0; i < count && va < end_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                note(subch, m);
                if (s_exec_enabled)
                    nv2a_pb_exec_method(subch, m,
                                        *(const uint32_t *)(mem + va));
                va += 4;
                words++;
            }
            continue;
        }
        (*unknown)++;
    }
    return words;
}

void nv2a_pb_scan_segment(uint32_t start_va, uint32_t end_va)
{
    uint32_t words, jumps = 0, unknown = 0;

    if (!pb_walk_enabled() || end_va <= start_va)
        return;
    if (end_va - start_va > 0x400000u)        /* a sane single-frame bound */
        end_va = start_va + 0x400000u;

    words = pb_walk_segment(start_va, end_va, 0, &jumps, &unknown);

    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}
