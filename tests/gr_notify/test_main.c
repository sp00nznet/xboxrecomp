/*
 * The PGRAPH notify for D3D's software methods and the run-ahead's ack rule
 * (nv2a_gr_notify.h, RECOMP_DEBUG=run_ahead).
 *
 * The guest side is a model of the XDK's ServiceGrInterrupt as a title
 * links it, step by step: PGRAPH_FIFO = 0; read INTR, TRAPPED_ADDR and
 * NSOURCE; write INTR back (write-1-to-clear on hardware, a plain store
 * here); when NSOURCE is set, INTR has ERROR or NOTIFY, NSOURCE lacks 0x40
 * and the method is NO_OPERATION, read TRAPPED_DATA and run the software
 * method; PGRAPH_FIFO = 1; return INTR re-read. Its DPC calls it while it
 * returns non-zero. Checked: which parameters are delivered (method 5,
 * never the swap), what the raise latches, that the handler finds and runs
 * method 5 with its data, that the serviced test sees only a full pass,
 * that a clear ends the DPC's loop, that a late write-back is taken down by
 * a second clear, the enable gate, the pending/queue state (one raised at a
 * time, the rest queued in order and raised once the line is free, a full
 * queue counted), the case of a walk that gave up before its DPC ran (the
 * ack thread's poll alone ends the DPC's loop, at any rate and phase, where
 * sampling the level misses a loop in step with it), and the ack rule.
 */
#include "nv2a_gr_notify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static uint32_t *regs;
#define R(off) NV2A_GR_REG(regs, off)

static int methods_run, last_method;
static uint32_t last_data;

/* The handler as steps, so a test can put another thread's action between
 * any two. */
typedef struct {
    int step;
    uint32_t intr, addr, nsource;
} Pass;

enum { PASS_STEPS = 8 };

/* One step; 1 when the pass returned, its re-read INTR in *ret. */
static int service_step(Pass *p, uint32_t *ret)
{
    switch (p->step++) {
    case 0: R(NV2A_GR_PGRAPH_FIFO) = 0; return 0;
    case 1: p->intr = R(NV2A_GR_PGRAPH_INTR); return 0;
    case 2: p->addr = R(NV2A_GR_PGRAPH_TRAPPED_ADDR) & 0x1FFCu; return 0;
    case 3: p->nsource = R(NV2A_GR_PGRAPH_NSOURCE); return 0;
    case 4: R(NV2A_GR_PGRAPH_INTR) = p->intr; return 0;     /* the "clear" */
    case 5:
        if (p->nsource && (p->intr & 0x100001u) && !(p->nsource & 0x40u)
            && p->addr == 0x100u) {
            uint32_t data = R(NV2A_GR_PGRAPH_TRAPPED_DATA);
            methods_run++;
            last_method = (int)(data & 0x1Fu);
            last_data = data >> 5;
        }
        return 0;
    case 6: R(NV2A_GR_PGRAPH_FIFO) = 1; return 0;
    default:
        *ret = R(NV2A_GR_PGRAPH_INTR);
        p->step = 0;
        return 1;
    }
}

/* Steps 0-4 (through the write-back), then the rest. */
static void service_begin(Pass *p)
{
    uint32_t r;
    memset(p, 0, sizeof *p);
    while (p->step < 5)
        service_step(p, &r);
}

static uint32_t service_end(Pass *p)
{
    uint32_t r = 0;
    while (!service_step(p, &r))
        ;
    return r;
}

static uint32_t service(void)
{
    Pass p;
    service_begin(&p);
    return service_end(&p);
}

/* The ack thread's pass, as gr_ack_tick and the ack table do it: take a
 * serviced notify down, and with none pending clear what a late write-back
 * left (the table's stock clear). */
static int tick_watch = 1;

static int ack_tick(Nv2aGrState *s)
{
    int took = nv2a_gr_poll(s, regs, tick_watch);
    if (!nv2a_gr_pending(s))
        nv2a_gr_notify_clear(regs);
    return took;
}

/* The DPC's loop after a walk that gave up: passes run until one returns
 * 0. The ack thread's pass comes every `every` steps from step `phase` on
 * (every 0: no ack thread), and while a notify is pending or its bits are
 * up it watches each step for `span` steps, as gr_ack_tick's watch spans
 * many passes. Returns the passes, or -1 at the cap. */
static int late_dpc(Nv2aGrState *s, int every, int phase, int span, int cap)
{
    Pass p;
    uint32_t r;
    int passes = 0, steps = 0, watch = 0;

    memset(&p, 0, sizeof p);
    while (passes < cap) {
        int done = service_step(&p, &r);
        steps++;
        if (every && steps >= phase && (steps - phase) % every == 0
            && (nv2a_gr_pending(s)
                || (R(NV2A_GR_PGRAPH_INTR) & NV2A_GR_PGRAPH_INTR_ERROR)))
            watch = span;
        if (watch) {
            watch--;
            ack_tick(s);
        }
        if (done) {
            passes++;
            if (!r)
                return passes;
        }
    }
    return -1;
}

int main(void)
{
    Pass pass;
    uint32_t intr;
    int passes;
    Nv2aGrState st;

    regs = calloc(1, 0x401000u);
    if (!regs)
        return 1;

    /* Which NO_OPERATION parameters are delivered. */
    check(nv2a_gr_notify_wanted(5, NV2A_GR_NOTIFY_DEFAULT_MASK),
          "method 5 (BlockOnTime's event) is delivered");
    check(nv2a_gr_notify_wanted((3u << 5) | 5u, NV2A_GR_NOTIFY_DEFAULT_MASK),
          "method 5 with data is delivered");
    check(!nv2a_gr_notify_wanted((0x2F4000u << 5) | 1u, 0xFFFFFFFFu),
          "the swap (method 1) never, whatever the mask");
    check(!nv2a_gr_notify_wanted(0, 0xFFFFFFFFu), "parameter 0 is a plain NOP");
    check(!nv2a_gr_notify_wanted(6, NV2A_GR_NOTIFY_DEFAULT_MASK),
          "a callback (method 6) is not in the default mask");

    /* The raise latches what the handler reads. */
    R(NV2A_GR_PMC_INTR_0) = 1u << 24;            /* a vblank already up */
    R(NV2A_GR_PMC_INTR_EN_0) = 1;
    nv2a_gr_notify_raise(regs, 0, (2u << 5) | 5u);
    check(R(NV2A_GR_PGRAPH_TRAPPED_ADDR) == 0x100u, "TRAPPED_ADDR is NO_OPERATION");
    check(R(NV2A_GR_PGRAPH_TRAPPED_DATA) == ((2u << 5) | 5u),
          "TRAPPED_DATA_LOW is the parameter");
    check(R(NV2A_GR_PGRAPH_NSOURCE) == NV2A_GR_NSOURCE_NOTIFICATION,
          "NSOURCE says NOTIFICATION");
    check(R(NV2A_GR_PGRAPH_INTR) & NV2A_GR_PGRAPH_INTR_ERROR, "PGRAPH_INTR has ERROR");
    check(R(NV2A_GR_PMC_INTR_0) == ((1u << 24) | NV2A_GR_PMC_INTR_PGRAPH),
          "PMC_INTR_0 has PGRAPH, and keeps the vblank's bit");
    check(!nv2a_gr_notify_serviced(regs), "not serviced before the handler runs");
    check(nv2a_gr_notify_deliverable(regs), "deliverable with the enable on");
    R(NV2A_GR_PMC_INTR_EN_0) = 0;
    check(!nv2a_gr_notify_deliverable(regs), "held while the ISR has it masked");
    R(NV2A_GR_PMC_INTR_EN_0) = 1;

    /* Half a handler is not serviced. */
    service_begin(&pass);
    check(!nv2a_gr_notify_serviced(regs), "not serviced mid-handler (FIFO 0)");
    intr = service_end(&pass);
    check(methods_run == 1 && last_method == 5 && last_data == 2,
          "the handler runs method 5 with its data");
    check(nv2a_gr_notify_serviced(regs), "serviced once the handler wrote FIFO 1");
    check(intr != 0, "plain RAM: the handler's own clear leaves INTR set");

    /* The walker's clear ends the DPC's loop. */
    nv2a_gr_notify_clear(regs);
    check(R(NV2A_GR_PGRAPH_INTR) == 0 && R(NV2A_GR_PGRAPH_NSOURCE) == 0,
          "the clear takes ERROR and NSOURCE down");
    check(R(NV2A_GR_PMC_INTR_0) == (1u << 24), "and only PGRAPH's PMC bit");
    passes = 0;
    while ((R(NV2A_GR_PMC_INTR_0) & NV2A_GR_PMC_INTR_PGRAPH) && passes < 8)
        passes++, service();
    check(passes == 0 && methods_run == 1, "the DPC finds nothing more to run");

    /* A second pass that read the bits before the clear writes them back
     * after it: the second clear takes them down, and the method ran at
     * most twice (event methods are idempotent). */
    nv2a_gr_notify_raise(regs, 0, 5);
    service();                                   /* pass 1, serviced */
    {                                            /* pass 2 reads INTR */
        uint32_t r;
        memset(&pass, 0, sizeof pass);
        while (pass.step < 4)
            service_step(&pass, &r);
    }
    nv2a_gr_notify_clear(regs);                  /* the walker's clear */
    intr = service_end(&pass);                   /* writes it back */
    check(intr & NV2A_GR_PGRAPH_INTR_ERROR, "a late write-back re-latches ERROR");
    nv2a_gr_notify_clear(regs);
    check(service() == 0 && methods_run == 3,
          "the second clear ends it: the method ran twice for one raise");

    /* One raised at a time; the rest queue in order behind it. */
    memset(&st, 0, sizeof st);
    methods_run = 0;
    check(nv2a_gr_post(&st, regs, 0, (1u << 5) | 5u) == NV2A_GR_RAISED
          && nv2a_gr_pending(&st), "a notify on a free line is raised");
    check(!nv2a_gr_poll(&st, regs, 1), "and not taken down before its handler");
    check(nv2a_gr_post(&st, regs, 0, (2u << 5) | 5u) == NV2A_GR_QUEUED
          && nv2a_gr_post(&st, regs, 0, (3u << 5) | 5u) == NV2A_GR_QUEUED
          && st.queued == 2, "two more queue behind it");
    check(!nv2a_gr_due(&st) && R(NV2A_GR_PGRAPH_TRAPPED_DATA) == ((1u << 5) | 5u),
          "none raised over the pending one");
    service();
    check(nv2a_gr_poll(&st, regs, 1) && !nv2a_gr_pending(&st) && last_data == 1,
          "the first is serviced and taken down");
    check(nv2a_gr_due(&st), "the next is due once the line is free");
    check(nv2a_gr_post(&st, regs, 0, (4u << 5) | 5u) == NV2A_GR_QUEUED,
          "a new one goes behind those queued, not past them");
    for (int i = 2; i <= 4; i++) {
        nv2a_gr_raise_next(&st, regs);
        service();
        nv2a_gr_poll(&st, regs, 1);
        check(last_data == (uint32_t)i && !nv2a_gr_pending(&st),
              i == 2 ? "queued ones are raised and serviced in order (2)"
              : i == 3 ? "queued ones are raised and serviced in order (3)"
                       : "queued ones are raised and serviced in order (4)");
    }
    check(!nv2a_gr_due(&st) && methods_run == 4,
          "four posted, four run, none lost");
    nv2a_gr_notify_clear(regs);
    nv2a_gr_post(&st, regs, 0, 5);
    for (unsigned i = 0; i < NV2A_GR_QUEUE_LEN; i++)
        nv2a_gr_post(&st, regs, 0, 5);
    check(nv2a_gr_post(&st, regs, 0, 5) == NV2A_GR_DROPPED && st.dropped == 1,
          "past a full queue a notify is dropped and counted");
    memset(&st, 0, sizeof st);
    nv2a_gr_notify_clear(regs);

    /* The timer thread takes a serviced notify down while the ack thread
     * raises the next. The old order (pending to 0, then the clear) let
     * the raise land between the two and cleared the new notify's bits:
     * pending, with nothing for the ISR to find. Shown first by hand. */
    methods_run = 0;
    nv2a_gr_post(&st, regs, 0, (1u << 5) | 5u);
    service();
    __atomic_store_n(&st.pending, 0, __ATOMIC_SEQ_CST);  /* old: down first */
    nv2a_gr_post(&st, regs, 0, (2u << 5) | 5u);          /* the ack thread */
    nv2a_gr_notify_clear(regs);                          /* old: then clear */
    check(nv2a_gr_pending(&st)
          && !(R(NV2A_GR_PGRAPH_INTR) & NV2A_GR_PGRAPH_INTR_ERROR)
          && service() == 0 && methods_run == 1,
          "old order: a raise between down and clear is lost");
    memset(&st, 0, sizeof st);
    nv2a_gr_notify_clear(regs);
    methods_run = 0;
    nv2a_gr_post(&st, regs, 0, (1u << 5) | 5u);
    service();
    check(nv2a_gr_take(&st) && nv2a_gr_pending(&st),
          "taken: the line still reads busy until cleared");
    check(nv2a_gr_post(&st, regs, 0, (2u << 5) | 5u) == NV2A_GR_QUEUED
          && !nv2a_gr_poll(&st, regs, 1) && !nv2a_gr_due(&st),
          "a raise meanwhile queues; another poller leaves it alone");
    nv2a_gr_settle(&st, regs);
    check(!nv2a_gr_pending(&st) && nv2a_gr_due(&st),
          "free only after the clear, and the next is due");
    nv2a_gr_raise_next(&st, regs);
    check((R(NV2A_GR_PGRAPH_INTR) & NV2A_GR_PGRAPH_INTR_ERROR)
          && (R(NV2A_GR_PMC_INTR_0) & NV2A_GR_PMC_INTR_PGRAPH),
          "its bits stay up for the ISR");
    service();
    check(nv2a_gr_poll(&st, regs, 0) && methods_run == 2 && last_data == 2,
          "and its handler runs: none lost");
    memset(&st, 0, sizeof st);
    nv2a_gr_notify_clear(regs);

    /* The walk gave up on its notify; the DPC runs later on a thread that
     * cannot poll. Without the ack thread its loop never ends (plain RAM
     * keeps the bits); with it, at any phase against the handler's steps,
     * it ends within a few passes and the method ran. */
    nv2a_gr_post(&st, regs, 0, (7u << 5) | 5u);
    methods_run = 0;
    check(late_dpc(&st, 0, 0, 0, 1000) < 0 && methods_run == 1000,
          "no poller: the DPC loops on the latched bits");
    {
        int worst = 0, ok = 1;
        for (int every = 1; every <= 40; every++)
            for (int phase = 1; phase <= PASS_STEPS; phase++) {
                int n;
                memset(&st, 0, sizeof st);
                nv2a_gr_notify_clear(regs);
                nv2a_gr_post(&st, regs, 0, (7u << 5) | 5u);
                methods_run = 0;
                n = late_dpc(&st, every, phase, PASS_STEPS + 1, 1000);
                if (n < 0 || methods_run < 1 || nv2a_gr_pending(&st)
                    || last_data != 7)
                    ok = 0;
                if (n > worst)
                    worst = n;
            }
        check(ok, "the ack thread's poll ends it at every phase and rate");
        printf("    (worst case %d passes)\n", worst);
        check(worst <= 8, "within a few passes");
    }
    /* The level alone: a sampler in step with the loop never sees its 1. */
    tick_watch = 0;
    memset(&st, 0, sizeof st);
    nv2a_gr_notify_clear(regs);
    nv2a_gr_post(&st, regs, 0, 5);
    check(late_dpc(&st, PASS_STEPS, 3, 1, 1000) < 0,
          "sampling the level misses a loop in step with the sampler");
    tick_watch = 1;
    memset(&st, 0, sizeof st);
    nv2a_gr_notify_clear(regs);

    /* The run-ahead's ack rule. */
    check(nv2a_run_ahead_may_ack(1, 0, 0, 1),
          "depth 1: ack while nothing is submitted past this walk");
    check(!nv2a_run_ahead_may_ack(1, 0, 1, 1),
          "depth 1: no ack once a segment is submitted past it");
    check(nv2a_run_ahead_may_ack(1, 0, 3, 4) && !nv2a_run_ahead_may_ack(1, 0, 4, 4),
          "depth 4: four segments past the walk, not five");
    check(!nv2a_run_ahead_may_ack(1, 1, 0, 64), "no ack while a flip is held");
    check(!nv2a_run_ahead_may_ack(0, 0, 0, 64), "no ack without a kick");

    free(regs);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures != 0;
}
