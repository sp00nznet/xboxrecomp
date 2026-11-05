/*
 * kernel_user_apc.c - per-thread queue of file I/O completion APCs.
 * See kernel_user_apc.h for when the bridge uses it.
 */
#include "kernel_user_apc.h"

/* Same keying as xbox_winnt.h's XBOX_THREAD_LOCAL: on the compiler, because
 * MinGW is GCC and ignores __declspec(thread). Not included from there so the
 * test builds this file on its own. */
#if defined(_MSC_VER)
#define KUAPC_TLS __declspec(thread)
#else
#define KUAPC_TLS _Thread_local
#endif

typedef struct {
    uint32_t routine, context, iosb;
    unsigned seq;
} kuapc_entry;

/* A ring: head is the next to run, count how many wait, seq the number the
 * next queued entry gets. An APC is queued and run by the thread that issued
 * the request, so nothing here is shared. */
static KUAPC_TLS kuapc_entry t_ring[KUAPC_MAX];
static KUAPC_TLS int t_head, t_count;
static KUAPC_TLS unsigned t_seq;

int kuapc_queue(uint32_t routine, uint32_t context, uint32_t iosb)
{
    kuapc_entry *e;

    if (t_count == KUAPC_MAX)
        return 0;
    e = &t_ring[(t_head + t_count) % KUAPC_MAX];
    e->routine = routine;
    e->context = context;
    e->iosb    = iosb;
    e->seq     = t_seq++;
    t_count++;
    return 1;
}

int kuapc_pending(void)
{
    return t_count;
}

int kuapc_drain(kuapc_deliver_fn deliver)
{
    /* Only what was queued when the wait began. A completion routine that
     * issues the next read queues that read's APC behind these, and it waits
     * for the next alertable wait: here the read has already finished, so
     * running it in this drain would turn a streaming chain into a loop that
     * never returns to the title.
     *
     * The bound is a sequence number, not a count: a routine that makes an
     * alertable wait drains again from inside this one, and that inner drain
     * takes entries this one counted, so a count would let this one run on
     * into the chained APCs queued after it began. */
    unsigned end = t_seq;
    int ran = 0;

    while (t_count > 0 && (int)(t_ring[t_head].seq - end) < 0) {
        kuapc_entry e = t_ring[t_head];
        t_head = (t_head + 1) % KUAPC_MAX;
        t_count--;
        deliver(e.routine, e.context, e.iosb);
        ran++;
    }
    return ran;
}
