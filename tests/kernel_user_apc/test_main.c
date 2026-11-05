/*
 * kernel_user_apc: the per-thread queue the bridge delivers file I/O APCs
 * through (on by default; RECOMP_FILE_APC_DEFER=0 bypasses it).
 *
 * Jet Set Radio Future's CRI file server marks a read busy, issues ReadFileEx,
 * and only accounts the bytes when its own SleepEx(0, TRUE) sees the
 * completion clear the busy flag. Delivered inline, the APC cleared it before
 * the server looked, the accounting never ran, and title.adx stopped after
 * its first block. These checks pin what the queue promises the bridge.
 */
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

#include "kernel_user_apc.h"

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t g_log[256];
static int g_nlog;

static void record(uint32_t routine, uint32_t context, uint32_t iosb)
{
    (void)iosb;
    if (g_nlog < 256)
        g_log[g_nlog++] = routine ^ context;
}

/* A completion that issues the next read, as a streaming title's does. */
static int g_chain_left;
static void chain(uint32_t routine, uint32_t context, uint32_t iosb)
{
    record(routine, context, iosb);
    if (g_chain_left-- > 0)
        kuapc_queue(routine, context + 1, iosb);
}

static void test_fifo(void)
{
    int i;

    g_nlog = 0;
    CHECK(kuapc_pending() == 0, "starts empty");
    for (i = 0; i < 5; i++)
        CHECK(kuapc_queue(0x1000, (uint32_t)i, 0x2000), "queue %d", i);
    CHECK(kuapc_pending() == 5, "pending %d", kuapc_pending());
    CHECK(kuapc_drain(record) == 5, "drain runs all five");
    for (i = 0; i < 5; i++)
        CHECK(g_log[i] == (0x1000u ^ (uint32_t)i), "order at %d", i);
    CHECK(kuapc_pending() == 0, "empty after drain");
    CHECK(kuapc_drain(record) == 0, "empty drain runs nothing");
}

static void test_chain_waits_for_next_drain(void)
{
    g_nlog = 0;
    g_chain_left = 3;
    kuapc_queue(0x3000, 0, 0);
    /* Each drain runs only the APC that was waiting; the one it queues is
     * left for the next wait, so the title gets control back in between. */
    CHECK(kuapc_drain(chain) == 1, "first drain");
    CHECK(kuapc_pending() == 1, "next read's APC waits");
    CHECK(kuapc_drain(chain) == 1, "second drain");
    CHECK(kuapc_drain(chain) == 1, "third drain");
    CHECK(kuapc_drain(chain) == 1, "fourth drain");
    CHECK(kuapc_pending() == 0, "chain ends");
    CHECK(g_nlog == 4, "four completions, got %d", g_nlog);
}

/* A completion that makes an alertable wait (a nested drain), then issues
 * the next read. */
static int g_nested_done;
static void nested(uint32_t routine, uint32_t context, uint32_t iosb)
{
    record(routine, context, iosb);
    if (g_nested_done++)
        return;
    kuapc_drain(record);
    kuapc_queue(routine, 99, iosb);
}

static void test_nested_drain_bound(void)
{
    g_nlog = 0;
    g_nested_done = 0;
    kuapc_queue(0x8000, 1, 0);
    kuapc_queue(0x8000, 2, 0);
    /* The first APC's own wait runs the second; the read it issues after
     * that was queued after the outer drain began, so it waits for the next
     * wait even though the outer drain has run only one of its two. */
    kuapc_drain(nested);
    CHECK(g_nlog == 2, "outer drain stops at its bound, ran %d", g_nlog);
    CHECK(kuapc_pending() == 1, "chained APC waits, pending %d",
          kuapc_pending());
    g_nlog = 0;
    kuapc_drain(record);
    CHECK(g_nlog == 1 && g_log[0] == (0x8000u ^ 99u), "next drain runs it");
}

static void test_full(void)
{
    int i, ok = 1;

    for (i = 0; i < KUAPC_MAX; i++)
        ok &= kuapc_queue(0x4000, (uint32_t)i, 0);
    CHECK(ok, "fills to KUAPC_MAX");
    CHECK(!kuapc_queue(0x4000, 999, 0), "refuses past KUAPC_MAX");
    g_nlog = 0;
    CHECK(kuapc_drain(record) == KUAPC_MAX, "drains a full ring");
    /* The ring wrapped: queue and drain again across the seam. */
    for (i = 0; i < 3; i++)
        kuapc_queue(0x5000, (uint32_t)i, 0);
    g_nlog = 0;
    CHECK(kuapc_drain(record) == 3, "drain after wrap");
    CHECK(g_log[2] == (0x5000u ^ 2u), "order after wrap");
}

static void *other_thread(void *arg)
{
    int *seen = arg;
    *seen = kuapc_pending();
    kuapc_queue(0x6000, 1, 0);
    return NULL;
}

static void test_per_thread(void)
{
    pthread_t t;
    int seen = -1;

    kuapc_queue(0x7000, 1, 0);
    pthread_create(&t, NULL, other_thread, &seen);
    pthread_join(t, NULL);
    /* The APC belongs to the thread that issued the request: another thread
     * neither sees this one nor hands its own to us. */
    CHECK(seen == 0, "other thread saw %d", seen);
    CHECK(kuapc_pending() == 1, "ours still 1, got %d", kuapc_pending());
    g_nlog = 0;
    kuapc_drain(record);
    CHECK(g_nlog == 1 && g_log[0] == (0x7000u ^ 1u), "ran only ours");
}

int main(void)
{
    test_fifo();
    test_chain_waits_for_next_drain();
    test_nested_drain_bound();
    test_full();
    test_per_thread();
    printf("%s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
