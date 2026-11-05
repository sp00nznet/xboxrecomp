/* The visibility-test tracker (nv2a_occ, nv2a_backend_common.h) against fake
 * query ops: a query's result is in once the fake clock reaches the time set
 * when it began, and the pool counts gets and puts. The report words are read
 * back from a guest-memory array. One "[occ_reports] name: PASS|FAIL" line a
 * case. The settle is the fix: a report still pending at the end
 * of a walk, a semaphore release or the flip reads its address's last count
 * (or visible the first time), never TESTINCOMPLETE. The cases run with no
 * settle wait unless they set one; "the bound" covers the default. */
#include <stdio.h>
#include <string.h>
#include "nv2a_backend_common.h"

#define CHECK(x) \
    do { if (!(x)) { printf("  line %d: %s\n", __LINE__, #x); return 1; } } while (0)

#define NQ 2048
struct fq {
    uint64_t ready_at, value;
    int out;
};
static struct fq s_q[NQ];
static int s_qn;
static uint64_t s_clock, s_tick;    /* s_tick: added on every now_us call */
static uint64_t s_lat, s_val;       /* the next query begun: latency, samples */
static unsigned s_gets, s_puts, s_bad;
static uint32_t s_mem[1024];        /* 4 KiB of "guest memory" */

static void *q_get(void *c)
{
    int i;
    (void)c;
    for (i = 0; i < NQ; i++) {
        int k = (s_qn + i) % NQ;
        if (!s_q[k].out) {
            s_q[k].out = 1;
            s_qn = (k + 1) % NQ;
            s_gets++;
            return &s_q[k];
        }
    }
    return NULL;
}

static void q_put(void *c, void *q)
{
    (void)c;
    if (!((struct fq *)q)->out)
        s_bad++;                    /* put twice */
    ((struct fq *)q)->out = 0;
    s_puts++;
}

static void q_begin(void *c, void *q)
{
    (void)c;
    ((struct fq *)q)->ready_at = s_clock + s_lat;
    ((struct fq *)q)->value = s_val;
}

static void q_end(void *c, void *q) { (void)c; (void)q; }

static int q_result(void *c, void *q, int flush, uint64_t *n)
{
    (void)c;
    (void)flush;
    if (s_clock < ((struct fq *)q)->ready_at)
        return 0;
    *n = ((struct fq *)q)->value;
    return 1;
}

static uint64_t q_now(void *c)
{
    (void)c;
    s_clock += s_tick;
    return s_clock;
}

static const struct nv2a_occ_ops s_ops = { q_get, q_put, q_begin, q_end, q_result, q_now, NULL };
static struct nv2a_occ s_o;

static void fresh(void)
{
    memset(s_q, 0, sizeof s_q);
    memset(s_mem, 0, sizeof s_mem);
    s_qn = 0;
    s_clock = 1000000;
    s_tick = 0;
    s_lat = 0;
    s_val = 0;
    s_gets = s_puts = s_bad = 0;
    nv2a_occ_init(&s_o, &s_ops, (uint8_t *)s_mem);
    nv2a_occ_set_scale(&s_o, 1);
    nv2a_occ_set_settle(&s_o, 0);       /* the fake clock stands still */
}

/* BeginVisibilityTest / EndVisibilityTest at va: one stretch counting
 * `val` samples, its result in `lat` us after it began. */
static void vtest(uint32_t va, uint64_t lat, uint64_t val)
{
    s_lat = lat;
    s_val = val;
    s_mem[va / 4 + 3] = 0xFFFFFFFFu;   /* what EndVisibilityTest writes */
    nv2a_occ_zpass_clear(&s_o);
    nv2a_occ_zpass(&s_o, 1);
    nv2a_occ_zpass(&s_o, 0);
    nv2a_occ_report(&s_o, va, 0);
}

static uint32_t count_at(uint32_t va) { return s_mem[va / 4 + 2]; }
static uint32_t status_at(uint32_t va) { return s_mem[va / 4 + 3]; }

static int last_of(uint32_t va, uint32_t *c)
{
    int i;
    for (i = 0; i < s_o.nlast; i++)
        if (s_o.last[i].va == va) {
            *c = s_o.last[i].count;
            return 1;
        }
    return 0;
}

/* Case 9, for every case: once every result is in or too old, every query
 * the tracker took has gone back, once. */
static int drained(void)
{
    s_clock += 10 * NV2A_OCC_LATE_US;
    nv2a_occ_poll(&s_o, 1);
    nv2a_occ_flip(&s_o);
    return s_o.npend == 0 && s_o.nret == 0 && s_gets == s_puts && !s_bad;
}

enum { A = 0x100, B = 0x200, C = 0x300, D = 0x400 };

static int case_ready(void)
{
    uint32_t c;
    fresh();
    vtest(A, 0, 500);
    nv2a_occ_flip(&s_o);
    CHECK(count_at(A) == 500 && status_at(A) == 0);
    CHECK(s_o.settled == 0);
    CHECK(last_of(A, &c) && c == 500);
    CHECK(drained());
    return 0;
}

static int case_stale(void)
{
    uint32_t c;
    fresh();
    vtest(A, 0, 500);
    nv2a_occ_flip(&s_o);
    vtest(A, 5000, 700);
    CHECK(status_at(A) == 0xFFFFFFFFu);
    nv2a_occ_flip(&s_o);
    CHECK(count_at(A) == 500 && status_at(A) == 0);
    CHECK(s_o.settled == 1 && s_o.stale == 1 && s_o.first == 0);
    /* The late result amends the settled report... */
    s_clock += 6000;
    nv2a_occ_poll(&s_o, 0);
    CHECK(count_at(A) == 700 && status_at(A) == 0);
    CHECK(s_o.amended == 1);
    CHECK(last_of(A, &c) && c == 700);
    CHECK(s_o.nret == 0);
    /* The next pending test at A reads the late count: one frame stale. */
    vtest(A, 5000, 900);
    nv2a_occ_flip(&s_o);
    CHECK(count_at(A) == 700 && status_at(A) == 0);
    CHECK(s_o.stale == 2);
    CHECK(drained());
    return 0;
}

/* A slot the title has reset before the late result stays reset, and a
 * slot a later GET_REPORT named keeps that test's count even when the
 * words match what the settle wrote. */
static int case_amend_guard(void)
{
    uint32_t c;
    fresh();
    vtest(A, 0, 500);
    nv2a_occ_flip(&s_o);
    vtest(A, 5000, 700);
    nv2a_occ_flip(&s_o);
    CHECK(count_at(A) == 500 && status_at(A) == 0);
    s_mem[A / 4 + 3] = 0xFFFFFFFFu;
    s_clock += 6000;
    nv2a_occ_poll(&s_o, 0);
    CHECK(status_at(A) == 0xFFFFFFFFu && count_at(A) == 500);
    CHECK(last_of(A, &c) && c == 700 && s_o.amended == 0);
    CHECK(drained());

    fresh();
    vtest(B, 5000, 300);
    nv2a_occ_flip(&s_o);
    CHECK(count_at(B) == NV2A_OCC_VISIBLE && s_o.first == 1);
    /* The next test at B is in before the old result, with the visible
     * count again: the old 300 must not land over it. */
    vtest(B, 0, NV2A_OCC_VISIBLE);
    nv2a_occ_flip(&s_o);
    CHECK(count_at(B) == NV2A_OCC_VISIBLE && status_at(B) == 0);
    s_clock += 6000;
    nv2a_occ_poll(&s_o, 0);
    CHECK(count_at(B) == NV2A_OCC_VISIBLE && s_o.amended == 0);
    CHECK(drained());
    return 0;
}

/* Burnout 3's sun test: ten report slots in rotation, one a
 * frame, each read nine frames after its GET_REPORT, results 12 ms late.
 * Every report settles, the first ten as visible, but each read sees the
 * true count. */
static int case_rotation(void)
{
    enum { SLOTS = 10, SLOT0 = 0x800 };
    int f;
    fresh();
    nv2a_occ_set_delay(&s_o, 12000);
    for (f = 0; f < 40; f++) {
        uint32_t va = SLOT0 + 16 * (uint32_t)(f % SLOTS);
        if (f >= SLOTS) {
            CHECK(status_at(va) == 0);
            CHECK(count_at(va) == 200 + (uint32_t)(f - SLOTS));
        }
        vtest(va, 0, 200 + (uint64_t)f);
        s_clock += 100;
        nv2a_occ_flip(&s_o);
        s_clock += 16600;
        nv2a_occ_poll(&s_o, 0);
    }
    /* Only slot 0 has no count beside it; the rest settle beside. */
    CHECK(s_o.settled == 40 && s_o.first == 1 && s_o.beside == SLOTS - 1);
    CHECK(s_o.amended == 40);
    CHECK(drained());
    return 0;
}

/* A slot with no count of its own reads the youngest count beside it
 * before its result is in, never visible; a slot far away still does. */
static int case_near(void)
{
    enum { S0 = 0x800 };
    fresh();
    vtest(S0, 0, 300);
    nv2a_occ_flip(&s_o);
    s_clock += 16600;
    vtest(S0 + 16, 0, 310);
    nv2a_occ_flip(&s_o);
    s_clock += 16600;
    vtest(S0 + 32, 5000, 320);
    vtest(S0 + 0x400, 5000, 330);
    nv2a_occ_flip(&s_o);
    CHECK(count_at(S0 + 32) == 310 && status_at(S0 + 32) == 0);
    CHECK(count_at(S0 + 0x400) == NV2A_OCC_VISIBLE);
    CHECK(s_o.beside == 1 && s_o.first == 1 && s_o.stale == 0);
    s_clock += 5000;
    nv2a_occ_poll(&s_o, 0);
    CHECK(count_at(S0 + 32) == 320 && count_at(S0 + 0x400) == 330);
    CHECK(drained());
    return 0;
}

/* A borrowed 0 is a count like any other: the sun hidden last flip stays
 * hidden, and the late result still amends it. */
static int case_near_zero(void)
{
    enum { S0 = 0x800 };
    fresh();
    vtest(S0, 0, 0);
    nv2a_occ_flip(&s_o);
    s_clock += 16600;
    vtest(S0 + 16, 5000, 40);
    nv2a_occ_flip(&s_o);
    CHECK(count_at(S0 + 16) == 0 && status_at(S0 + 16) == 0);
    CHECK(s_o.beside == 1 && s_o.first == 0);
    s_clock += 5000;
    nv2a_occ_poll(&s_o, 0);
    CHECK(count_at(S0 + 16) == 40 && s_o.amended == 1);
    CHECK(drained());
    return 0;
}

/* Several objects tested beside each other every flip (JSRF's lights,
 * SSX 3's flares): a new slot among them borrows from none, whether their
 * counts are in or still pending, and settles to visible. */
static int case_crowd(void)
{
    enum { S0 = 0x800 };
    int f;
    fresh();
    for (f = 0; f < 4; f++) {
        vtest(S0, 0, 400);
        vtest(S0 + 16, 0, 0);           /* hidden */
        nv2a_occ_flip(&s_o);
        s_clock += 16600;
    }
    vtest(S0, 0, 400);
    vtest(S0 + 16, 0, 0);
    vtest(S0 + 32, 5000, 50);
    nv2a_occ_flip(&s_o);
    CHECK(count_at(S0 + 32) == NV2A_OCC_VISIBLE && s_o.beside == 0);
    s_clock += 16600;
    /* The same with the neighbours' results still out. */
    vtest(S0, 5000, 400);
    vtest(S0 + 16, 5000, 0);
    vtest(S0 + 48, 5000, 60);
    nv2a_occ_flip(&s_o);
    CHECK(count_at(S0 + 48) == NV2A_OCC_VISIBLE && s_o.beside == 0);
    CHECK(count_at(S0 + 16) == 0 && s_o.stale == 2);
    CHECK(drained());
    return 0;
}

/* retired[] full: the oldest settled report goes, and its late result
 * never amends the slot. */
static int case_retired_full(void)
{
    enum { S0 = 0x100 };
    int k;
    fresh();
    for (k = 0; k <= NV2A_OCC_PENDING; k++) {
        vtest(S0 + 16 * (uint32_t)k, 5000, 70);
        nv2a_occ_flip(&s_o);
    }
    CHECK(s_o.ret_drop == 1 && s_o.nret == NV2A_OCC_PENDING);
    CHECK(count_at(S0) == NV2A_OCC_VISIBLE);
    s_clock += 5000;
    nv2a_occ_poll(&s_o, 0);
    CHECK(count_at(S0) == NV2A_OCC_VISIBLE);
    CHECK(count_at(S0 + 16) == 70 && count_at(S0 + 16 * NV2A_OCC_PENDING) == 70);
    CHECK(s_o.amended == NV2A_OCC_PENDING);
    CHECK(drained());
    return 0;
}

static int case_first(void)
{
    fresh();
    vtest(B, 5000, 300);
    nv2a_occ_flip(&s_o);
    CHECK(count_at(B) == NV2A_OCC_VISIBLE && status_at(B) == 0);
    CHECK(s_o.settled == 1 && s_o.first == 1 && s_o.stale == 0);
    CHECK(drained());
    return 0;
}

/* The settle after a walk (on_poll) is the flip's without the flip count:
 * the title reads within the frame, before the flip. */
static int case_walk(void)
{
    fresh();
    vtest(A, 0, 500);
    nv2a_occ_settle(&s_o);
    vtest(A, 5000, 700);
    nv2a_occ_settle(&s_o);
    CHECK(count_at(A) == 500 && status_at(A) == 0);
    CHECK(s_o.settled == 1 && s_o.stale == 1 && s_o.flips == 0);
    /* A last count from another screen is no history: visible. */
    s_clock += 10 * NV2A_OCC_LATE_US;
    nv2a_occ_poll(&s_o, 1);
    vtest(A, 5000, 0);
    nv2a_occ_settle(&s_o);
    CHECK(count_at(A) == NV2A_OCC_VISIBLE && s_o.first == 1);
    CHECK(drained());
    return 0;
}

static int case_supersede(void)
{
    fresh();
    vtest(C, 5000, 10);
    CHECK(s_puts == 0);
    vtest(C, 5000, 20);
    CHECK(s_o.super == 1 && s_puts == 1 && s_o.npend == 1);
    /* The first was never written: the status is the second's pending. */
    CHECK(status_at(C) == 0xFFFFFFFFu);
    nv2a_occ_flip(&s_o);
    CHECK(s_o.settled == 1 && count_at(C) == NV2A_OCC_VISIBLE);
    CHECK(drained());
    return 0;
}

static int case_overflow_late(void)
{
    int i;
    fresh();
    s_lat = 1000000000ull;
    nv2a_occ_zpass_clear(&s_o);
    for (i = 0; i < NV2A_OCC_SEG + 1; i++) {
        nv2a_occ_zpass(&s_o, 1);
        nv2a_occ_zpass(&s_o, 0);
    }
    nv2a_occ_report(&s_o, A, 0);
    nv2a_occ_poll(&s_o, 0);
    CHECK(count_at(A) == NV2A_OCC_VISIBLE && status_at(A) == 0);
    CHECK(s_o.npend == 0 && s_o.settled == 0);
    /* No flip at all: visible after NV2A_OCC_LATE_US, as before. */
    vtest(D, 1000000000ull, 5);
    s_clock += NV2A_OCC_LATE_US / 2;
    nv2a_occ_poll(&s_o, 0);
    CHECK(status_at(D) == 0xFFFFFFFFu);
    s_clock += NV2A_OCC_LATE_US;
    nv2a_occ_poll(&s_o, 0);
    CHECK(count_at(D) == NV2A_OCC_VISIBLE && status_at(D) == 0);
    CHECK(s_o.late == 1 && s_o.settled == 0);
    CHECK(drained());
    return 0;
}

/* The harness case in miniature: results 16 ms late, a 16.7 ms frame, the
 * test pushed just before the flip as BLiNX 2 does. */
static int case_delay(void)
{
    int f;
    fresh();
    nv2a_occ_set_delay(&s_o, 16000);
    for (f = 0; f < 20; f++) {
        vtest(A, 0, 100 + (uint64_t)f);
        s_clock += 100;
        nv2a_occ_flip(&s_o);
        CHECK(status_at(A) == 0);
        if (f == 0)
            CHECK(count_at(A) == NV2A_OCC_VISIBLE);
        else
            CHECK(count_at(A) == 100 + (uint32_t)f - 1);
        s_clock += 16600;
    }
    CHECK(s_o.settled == 20 && s_o.first == 1 && s_o.stale == 19);
    CHECK(s_o.settled == s_o.reports);
    CHECK(drained());
    return 0;
}

static int case_bound(void)
{
    fresh();
    nv2a_occ_init(&s_o, &s_ops, (uint8_t *)s_mem);
    CHECK(s_o.settle_us == NV2A_OCC_SETTLE_US);     /* the default */
    nv2a_occ_set_scale(&s_o, 1);
    s_tick = 10;                        /* the clock runs while the flip spins */
    vtest(A, 1000, 11);
    nv2a_occ_flip(&s_o);
    CHECK(count_at(A) == 11 && s_o.settled == 0);
    vtest(B, NV2A_OCC_SETTLE_US + 1000, 22);    /* later than the budget */
    nv2a_occ_flip(&s_o);
    CHECK(count_at(B) == NV2A_OCC_VISIBLE && s_o.settled == 1);
    CHECK(s_o.settle_wait_us >= NV2A_OCC_SETTLE_US);
    CHECK(drained());

    fresh();
    s_tick = 10;
    vtest(A, 1000, 11);
    nv2a_occ_flip(&s_o);
    vtest(B, 3000, 22);
    nv2a_occ_flip(&s_o);
    CHECK(s_o.settled == 2 && s_o.settle_wait_us == 0);
    CHECK(count_at(A) == NV2A_OCC_VISIBLE && count_at(B) == NV2A_OCC_VISIBLE);
    CHECK(drained());
    return 0;
}

/* The wait is a budget a frame: once a walk has spent it, the frame's later
 * settles take what is in; the flip starts a new one. */
static int case_budget(void)
{
    uint64_t w;
    fresh();
    s_tick = 10;
    nv2a_occ_set_settle(&s_o, 2000);
    vtest(A, 3000, 1);
    nv2a_occ_settle(&s_o);
    CHECK(count_at(A) == NV2A_OCC_VISIBLE && s_o.settled == 1);
    w = s_o.settle_wait_us;
    CHECK(w >= 2000 && w < 2100);
    vtest(B, 1000, 2);
    nv2a_occ_settle(&s_o);
    CHECK(count_at(B) == NV2A_OCC_VISIBLE && s_o.settled == 2);
    CHECK(s_o.settle_wait_us == w);
    nv2a_occ_flip(&s_o);
    CHECK(s_o.frame_wait_us == 0);
    vtest(C, 1000, 3);
    nv2a_occ_settle(&s_o);
    CHECK(count_at(C) == 3 && s_o.settled == 2);
    CHECK(s_o.settle_wait_us > w && s_o.settle_wait_us < w + 1100);
    CHECK(drained());
    return 0;
}

/* A settled report's late result never replaces the count of a newer report
 * at the same address that came in first (the t0 guard in occ_result_in). */
static int case_order(void)
{
    uint32_t c;
    fresh();
    vtest(A, 0, 100);
    nv2a_occ_flip(&s_o);
    vtest(A, 5000, 500);                /* r1: settled stale, then late */
    nv2a_occ_flip(&s_o);
    CHECK(count_at(A) == 100 && s_o.stale == 1 && s_o.nret == 1);
    s_clock += 10;
    vtest(A, 0, 700);                   /* r2: exact */
    nv2a_occ_flip(&s_o);
    CHECK(count_at(A) == 700 && last_of(A, &c) && c == 700);
    s_clock += 6000;
    nv2a_occ_poll(&s_o, 0);             /* r1's 500 arrives */
    CHECK(s_o.nret == 0);
    CHECK(last_of(A, &c) && c == 700 && count_at(A) == 700);
    CHECK(drained());
    return 0;
}

static int case_scale(void)
{
    uint32_t c;
    fresh();
    vtest(A, 0, 500);
    nv2a_occ_flip(&s_o);
    vtest(A, 5000, 900);
    nv2a_occ_flip(&s_o);
    CHECK(s_o.nret == 1);
    /* The scale-change path: a settle, then the new scale. */
    nv2a_occ_settle(&s_o);
    nv2a_occ_set_scale(&s_o, 2);
    CHECK(s_o.nret == 0 && s_o.ret_drop == 1);
    s_clock += 10000;
    nv2a_occ_poll(&s_o, 1);
    CHECK(last_of(A, &c) && c == 500);
    /* The armed report went with its queries: no amend over the settle. */
    CHECK(count_at(A) == 500 && s_o.amended == 0);
    CHECK(drained());
    return 0;
}

static int case_accounting(void)
{
    uint32_t i;
    fresh();
    /* A result that never comes, one new address a frame: retired[] fills,
     * then drops its oldest unread with a put each. */
    for (i = 0; i < NV2A_OCC_PENDING + 2; i++) {
        vtest(16 * (i + 1), 1000000000ull, 1);
        s_clock += 10;
        nv2a_occ_flip(&s_o);
    }
    CHECK(s_o.nret == NV2A_OCC_PENDING && s_o.ret_drop == 2);
    CHECK(s_puts == 2 && s_gets == NV2A_OCC_PENDING + 2);
    CHECK(s_o.first == NV2A_OCC_PENDING + 2);
    CHECK(drained());
    CHECK(s_o.ret_drop == NV2A_OCC_PENDING + 2);
    return 0;
}

int main(void)
{
    static const struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        { "ready at the flip", case_ready },
        { "pending at the flip, with history", case_stale },
        { "pending, no history", case_first },
        { "the amend keeps a reset or reused slot", case_amend_guard },
        { "rotating slots read late", case_rotation },
        { "a count from the slot beside", case_near },
        { "a borrowed count of 0", case_near_zero },
        { "no borrow among tests beside each other", case_crowd },
        { "retired full drops an armed report", case_retired_full },
        { "settled after a walk", case_walk },
        { "superseded within a frame", case_supersede },
        { "overflow and late", case_overflow_late },
        { "injected delay", case_delay },
        { "the bound", case_bound },
        { "a budget a frame", case_budget },
        { "a late result never replaces a newer count", case_order },
        { "scale change", case_scale },
        { "accounting", case_accounting },
    };
    unsigned i, fail = 0;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int r = cases[i].fn();
        printf("[occ_reports] %s: %s\n", cases[i].name, r ? "FAIL" : "PASS");
        fail += r != 0;
    }
    return fail ? 1 : 0;
}
