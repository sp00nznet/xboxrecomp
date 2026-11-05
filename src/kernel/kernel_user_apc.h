/*
 * kernel_user_apc.h - per-thread queue of file I/O completion APCs.
 *
 * On hardware the APC a ReadFileEx/WriteFileEx request names is queued to the
 * issuing thread and runs at that thread's next alertable wait, never inside
 * the request. The bridge runs file I/O synchronously, queues the APC here,
 * and drains the queue at the thread's next alertable user-mode wait, which
 * then returns STATUS_USER_APC. RECOMP_FILE_APC_DEFER=0 skips the queue and
 * delivers the APC inline, before NtReadFile returns (the old behaviour).
 *
 * The queue holds guest values only (routine, context, IO_STATUS_BLOCK
 * address); running a routine is the caller's job, through the deliver
 * callback, so this file needs nothing from the bridge and tests stand alone.
 */
#ifndef KERNEL_USER_APC_H
#define KERNEL_USER_APC_H

#include <stdint.h>

/* Room per thread. A title keeps a handful of requests in flight; a full
 * queue makes kuapc_queue return 0 and the caller deliver inline. */
#define KUAPC_MAX 64

typedef void (*kuapc_deliver_fn)(uint32_t routine, uint32_t context,
                                 uint32_t iosb);

/* Queue an APC on the calling thread. 1 when queued, 0 when the queue is full. */
int kuapc_queue(uint32_t routine, uint32_t context, uint32_t iosb);

/* APCs waiting on the calling thread. */
int kuapc_pending(void);

/* Run the APCs queued on the calling thread when the call began, oldest
 * first. One an APC queues while they run waits for the next drain. Returns
 * how many ran. */
int kuapc_drain(kuapc_deliver_fn deliver);

#endif /* KERNEL_USER_APC_H */
