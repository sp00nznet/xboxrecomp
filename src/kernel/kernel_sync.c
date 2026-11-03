/*
 * kernel_sync.c - Xbox Synchronization Primitives
 *
 * Implements events, semaphores, wait functions, kernel timers, and DPCs
 * using Win32 synchronization objects.
 *
 * Xbox synchronization model:
 *   - Events: notification (manual-reset) or synchronization (auto-reset)
 *   - Semaphores: standard counting semaphores
 *   - Wait functions: single and multiple, with optional timeout
 *   - Timers: kernel timers with optional DPC callback on expiry
 *   - DPCs: deferred procedure calls, executed via thread pool on Windows
 *
 * Time values use NT 100-nanosecond units (negative = relative).
 */

#include "kernel.h"
#include "guest_time.h"

/* ============================================================================
 * Helper: Convert NT 100ns interval to Win32 milliseconds
 *
 * NT time intervals:
 *   - Negative = relative (most common), in 100ns units
 *   - Positive = absolute FILETIME
 *   - NULL = infinite wait
 * ============================================================================ */

static DWORD xbox_nt_timeout_to_ms(PLARGE_INTEGER Timeout)
{
    if (!Timeout)
        return INFINITE;

    if (Timeout->QuadPart == 0)
        return 0;

    if (Timeout->QuadPart < 0) {
        /* Relative: negative 100ns units */
        LONGLONG relative_100ns = -Timeout->QuadPart;
        DWORD ms = (DWORD)(relative_100ns / 10000);
        if (ms == 0 && relative_100ns > 0)
            ms = 1;
        return ms;
    }

    /* Absolute: compute delta from now */
    LARGE_INTEGER now;
    now.QuadPart = (LONGLONG)xbox_GuestSystemTimeCoarse();
    LONGLONG diff = Timeout->QuadPart - now.QuadPart;
    if (diff <= 0)
        return 0;
    return (DWORD)(diff / 10000);
}

/* ============================================================================
 * Events
 *
 * Xbox event types:
 *   XboxNotificationEvent (0) = manual-reset (Win32: TRUE)
 *   XboxSynchronizationEvent (1) = auto-reset (Win32: FALSE)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtCreateEvent(
    PHANDLE EventHandle,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    ULONG EventType,
    BOOLEAN InitialState)
{
    HANDLE hEvent;
    BOOL bManualReset;

    (void)ObjectAttributes;

    if (!EventHandle)
        return STATUS_INVALID_PARAMETER;

    /* XboxNotificationEvent = manual-reset, XboxSynchronizationEvent = auto-reset */
    bManualReset = (EventType == XboxNotificationEvent) ? TRUE : FALSE;

    hEvent = CreateEventW(NULL, bManualReset, InitialState, NULL);
    if (!hEvent) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtCreateEvent: CreateEventW failed (error %u)", GetLastError());
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *EventHandle = hEvent;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "NtCreateEvent: handle=%p, type=%s, initial=%d",
        hEvent, bManualReset ? "notification" : "synchronization", InitialState);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtSetEvent(HANDLE EventHandle, PLONG PreviousState)
{
    if (PreviousState)
        *PreviousState = 0; /* We don't track previous state */

    if (!SetEvent(EventHandle)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtSetEvent: SetEvent failed for handle %p (error %u)",
            EventHandle, GetLastError());
        return STATUS_INVALID_HANDLE;
    }

    return STATUS_SUCCESS;
}

/*
 * KeSetEvent - kernel-mode event signal.
 * On Xbox, this is the kernel-mode equivalent of NtSetEvent.
 * The Object parameter is treated as a Win32 event HANDLE.
 * Returns the previous signal state.
 */
LONG __stdcall xbox_KeSetEvent(PVOID Event, LONG Increment, BOOLEAN Wait)
{
    HANDLE hEvent = (HANDLE)Event;

    (void)Increment;
    (void)Wait;

    /* We can't easily query previous state, so just set and return 0 */
    SetEvent(hEvent);
    return 0;
}

/* ============================================================================
 * Semaphores
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtCreateSemaphore(
    PHANDLE SemaphoreHandle,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    LONG InitialCount,
    LONG MaximumCount)
{
    HANDLE hSemaphore;

    (void)ObjectAttributes;

    if (!SemaphoreHandle)
        return STATUS_INVALID_PARAMETER;

    hSemaphore = CreateSemaphoreW(NULL, InitialCount, MaximumCount, NULL);
    if (!hSemaphore) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtCreateSemaphore: CreateSemaphoreW failed (error %u)", GetLastError());
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *SemaphoreHandle = hSemaphore;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "NtCreateSemaphore: handle=%p, initial=%d, max=%d",
        hSemaphore, InitialCount, MaximumCount);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReleaseSemaphore(
    HANDLE SemaphoreHandle,
    LONG ReleaseCount,
    PLONG PreviousCount)
{
    if (!ReleaseSemaphore(SemaphoreHandle, ReleaseCount, PreviousCount)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtReleaseSemaphore: failed for handle %p (error %u)",
            SemaphoreHandle, GetLastError());
        return STATUS_INVALID_HANDLE;
    }

    return STATUS_SUCCESS;
}

/* ============================================================================
 * Wait Functions
 * ============================================================================ */

/*
 * Map Win32 WaitFor* return codes to NTSTATUS.
 */
static NTSTATUS xbox_wait_result_to_ntstatus(DWORD result, ULONG count)
{
    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count)
        return (NTSTATUS)(STATUS_SUCCESS + (result - WAIT_OBJECT_0));

    switch (result) {
        case WAIT_TIMEOUT:          return STATUS_TIMEOUT;
        case WAIT_IO_COMPLETION:    return STATUS_ALERTED;
        case WAIT_ABANDONED_0:      return STATUS_ABANDONED;
        case WAIT_FAILED:           return STATUS_UNSUCCESSFUL;
        default:                    return STATUS_UNSUCCESSFUL;
    }
}

/* With guest_sched a wait is a short call at its kernel call
 * (kernel_guest_cpu.c): one whose object is already signalled returns
 * without a context switch on the console, so a time-critical holder keeps
 * the guest CPU through it. Poll first, and say the call blocks only when
 * it is about to. */
/* Virtual time (guest_time.h): a timed wait ends when the guest clock
 * reaches its due time, not when the host's milliseconds run out, so a
 * timeout lands on the same vblank on every run. It waits in 1 ms host
 * slices and checks the clock between them; the object still ends it at
 * once. Returns 0 (no virtual wait: untimed, zero or real mode) or the due
 * time on the interrupt-time clock. */
static uint64_t sync_virtual_due(PLARGE_INTEGER Timeout)
{
    if (!Timeout || Timeout->QuadPart == 0 || !xbox_GuestTimeVirtual())
        return 0;
    return xbox_GuestTimeDue(Timeout->QuadPart) | 1;   /* never 0 */
}

/* The deterministic mode (xbox_GuestDetOn): a guest thread that would block
 * hands the guest ring on and polls its objects each time its turn comes
 * back; the clock moves only when every guest thread is idle like this, so
 * the wait ends at the same point of the run every time. A zero timeout is
 * a plain poll and never gives the turn up. */
typedef struct {
    DWORD n;
    const HANDLE *h;
    BOOL all, alertable;
    DWORD r;
} SyncDetWait;

static int sync_det_check(void *ctx)
{
    SyncDetWait *w = (SyncDetWait *)ctx;

    w->r = w->n == 1 ? WaitForSingleObjectEx(w->h[0], 0, w->alertable)
                     : WaitForMultipleObjectsEx(w->n, w->h, w->all, 0,
                                                w->alertable);
    return w->r != WAIT_TIMEOUT;
}

static DWORD sync_det_wait(DWORD n, const HANDLE *h, BOOL all,
                           BOOL alertable, uint64_t due, uint32_t site)
{
    SyncDetWait w = { n, h, all, alertable, WAIT_TIMEOUT };

    return xbox_GuestDetBlock(sync_det_check, &w, due, site) ? w.r
                                                             : WAIT_TIMEOUT;
}

/* A due time under 1 ms ahead rounds ms to 0 and still waits: the ring's
 * wait, not the host's 1 ms slices with the token held and the clock
 * frozen. */
static DWORD sync_wait_one(HANDLE h, DWORD ms, BOOL alertable, uint64_t due)
{
    if ((ms || due) && xbox_GuestDetThread())
        return sync_det_wait(1, &h, FALSE, alertable, due, DET_SITE_WAIT_ONE);
    if (due) {
        DWORD r = WaitForSingleObjectEx(h, 0, alertable);
        if (r != WAIT_TIMEOUT)
            return r;
        if (xbox_GuestSchedOn())
            xbox_GuestCpuBlocked();
        while (!xbox_GuestTimeReached(due)) {
            r = WaitForSingleObjectEx(h, 1, alertable);
            if (r != WAIT_TIMEOUT)
                return r;
        }
        return WAIT_TIMEOUT;
    }
    if (ms && xbox_GuestSchedOn()) {
        DWORD r = WaitForSingleObjectEx(h, 0, alertable);
        if (r != WAIT_TIMEOUT)
            return r;
        xbox_GuestCpuBlocked();
    }
    return WaitForSingleObjectEx(h, ms, alertable);
}

static DWORD sync_wait_many(DWORD n, const HANDLE *h, BOOL all, DWORD ms,
                            BOOL alertable, uint64_t due)
{
    if ((ms || due) && xbox_GuestDetThread())
        return sync_det_wait(n, h, all, alertable, due, DET_SITE_WAIT_MANY);
    if (due) {
        DWORD r = WaitForMultipleObjectsEx(n, h, all, 0, alertable);
        if (r != WAIT_TIMEOUT)
            return r;
        if (xbox_GuestSchedOn())
            xbox_GuestCpuBlocked();
        while (!xbox_GuestTimeReached(due)) {
            r = WaitForMultipleObjectsEx(n, h, all, 1, alertable);
            if (r != WAIT_TIMEOUT)
                return r;
        }
        return WAIT_TIMEOUT;
    }
    if (ms && xbox_GuestSchedOn()) {
        DWORD r = WaitForMultipleObjectsEx(n, h, all, 0, alertable);
        if (r != WAIT_TIMEOUT)
            return r;
        xbox_GuestCpuBlocked();
    }
    return WaitForMultipleObjectsEx(n, h, all, ms, alertable);
}

NTSTATUS __stdcall xbox_NtWaitForSingleObject(
    HANDLE Handle,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);
    DWORD result = sync_wait_one(Handle, ms, Alertable, sync_virtual_due(Timeout));
    return xbox_wait_result_to_ntstatus(result, 1);
}

NTSTATUS __stdcall xbox_NtWaitForSingleObjectEx(
    HANDLE Handle,
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);
    DWORD result;

    /*
     * Same as NtWaitForSingleObject but with an explicit wait mode. We run
     * everything in one host process with no kernel/user split, so WaitMode
     * has nothing to select and is ignored.
     */
    (void)WaitMode;

    result = sync_wait_one(Handle, ms, Alertable, sync_virtual_due(Timeout));
    return xbox_wait_result_to_ntstatus(result, 1);
}

NTSTATUS __stdcall xbox_NtWaitForMultipleObjectsEx(
    ULONG Count,
    HANDLE Handles[],
    ULONG WaitType,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);
    BOOL bWaitAll;
    DWORD result;

    /* WaitType: 0 = WaitAll, 1 = WaitAny (matches NT definitions) */
    bWaitAll = (WaitType == 0) ? TRUE : FALSE;

    result = sync_wait_many(Count, Handles, bWaitAll, ms, Alertable,
                            sync_virtual_due(Timeout));
    return xbox_wait_result_to_ntstatus(result, Count);
}

/*
 * KeWaitForSingleObject - kernel-mode wait on a dispatcher object.
 * On Xbox, Objects can be events, timers, threads, etc.
 * We treat the object pointer as a Win32 HANDLE.
 */
NTSTATUS __stdcall xbox_KeWaitForSingleObject(
    PVOID Object,
    ULONG WaitReason,
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    (void)WaitReason;
    (void)WaitMode;

    HANDLE hObject = (HANDLE)Object;
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);
    DWORD result = sync_wait_one(hObject, ms, Alertable, sync_virtual_due(Timeout));
    return xbox_wait_result_to_ntstatus(result, 1);
}

NTSTATUS __stdcall xbox_KeWaitForMultipleObjects(
    ULONG Count,
    PVOID Objects[],
    ULONG WaitType,
    ULONG WaitReason,
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout,
    PVOID WaitBlockArray)
{
    (void)WaitReason;
    (void)WaitMode;
    (void)WaitBlockArray;

    BOOL bWaitAll = (WaitType == 0) ? TRUE : FALSE;
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);

    /* Objects[] is an array of PVOID which we treat as HANDLE[] */
    DWORD result = sync_wait_many(Count, (HANDLE*)Objects, bWaitAll, ms,
                                  Alertable, sync_virtual_due(Timeout));
    return xbox_wait_result_to_ntstatus(result, Count);
}

/* ============================================================================
 * Kernel Timers
 *
 * Xbox kernel timers are dispatcher objects that can be waited on and
 * optionally queue a DPC when they expire.
 *
 * Implementation: Each XBOX_KTIMER contains a Win32 event (for waitable
 * behavior) and uses CreateTimerQueueTimer for the timing mechanism.
 * When the timer fires, it signals the event and optionally invokes the DPC.
 * ============================================================================ */

/* Global timer queue - created lazily on first timer use */
static HANDLE g_timer_queue = NULL;
static CRITICAL_SECTION g_timer_cs;
static BOOL g_timer_cs_init = FALSE;

static void xbox_ensure_timer_queue(void)
{
    if (!g_timer_cs_init) {
        InitializeCriticalSection(&g_timer_cs);
        g_timer_cs_init = TRUE;
    }

    if (!g_timer_queue) {
        EnterCriticalSection(&g_timer_cs);
        if (!g_timer_queue) {
            g_timer_queue = CreateTimerQueue();
            if (!g_timer_queue) {
                xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
                    "Failed to create timer queue (error %u)", GetLastError());
            }
        }
        LeaveCriticalSection(&g_timer_cs);
    }
}

/*
 * Timer callback - called by the Windows timer queue thread.
 * Signals the event (for KeWaitForSingleObject) and fires the DPC if set.
 */
static VOID CALLBACK xbox_timer_callback(PVOID lpParameter, BOOLEAN TimerOrWaitFired)
{
    PXBOX_KTIMER timer = (PXBOX_KTIMER)lpParameter;

    (void)TimerOrWaitFired;

    /* Signal the event so waiters wake up */
    if (timer->win32_event)
        SetEvent(timer->win32_event);

    /* Fire the DPC if one is associated */
    if (timer->Dpc && timer->Dpc->DeferredRoutine) {
        xbox_log(XBOX_LOG_TRACE, XBOX_LOG_SYNC, "Timer DPC firing: routine=%p",
            timer->Dpc->DeferredRoutine);
        timer->Dpc->DeferredRoutine(
            timer->Dpc,
            timer->Dpc->DeferredContext,
            timer->Dpc->SystemArgument1,
            timer->Dpc->SystemArgument2);
    }

    /* If not periodic, mark as no longer inserted */
    if (timer->Period == 0)
        timer->Inserted = FALSE;
}

VOID __stdcall xbox_KeInitializeTimerEx(PXBOX_KTIMER Timer, XBOX_TIMER_TYPE Type)
{
    (void)Type;

    if (!Timer)
        return;

    memset(Timer, 0, sizeof(XBOX_KTIMER));

    /* Create a manual-reset event for notification timers,
     * auto-reset for synchronization timers */
    BOOL manual_reset = (Type == XboxNotificationTimer) ? TRUE : FALSE;
    Timer->win32_event = CreateEventW(NULL, manual_reset, FALSE, NULL);
    Timer->Inserted = FALSE;
    Timer->Period = 0;
    Timer->Dpc = NULL;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "KeInitializeTimerEx: timer=%p, type=%d, event=%p",
        Timer, Type, Timer->win32_event);
}

BOOLEAN __stdcall xbox_KeSetTimer(PXBOX_KTIMER Timer, LARGE_INTEGER DueTime, PXBOX_KDPC Dpc)
{
    return xbox_KeSetTimerEx(Timer, DueTime, 0, Dpc);
}

BOOLEAN __stdcall xbox_KeSetTimerEx(
    PXBOX_KTIMER Timer,
    LARGE_INTEGER DueTime,
    LONG Period,
    PXBOX_KDPC Dpc)
{
    BOOLEAN was_inserted;
    DWORD due_ms;
    DWORD period_ms;

    if (!Timer)
        return FALSE;

    xbox_ensure_timer_queue();

    was_inserted = Timer->Inserted;

    /* Cancel existing timer if re-arming */
    if (was_inserted && Timer->win32_timer) {
        DeleteTimerQueueTimer(g_timer_queue, Timer->win32_timer, NULL);
        Timer->win32_timer = NULL;
    }

    /* Reset the event */
    if (Timer->win32_event)
        ResetEvent(Timer->win32_event);

    Timer->Dpc = Dpc;
    Timer->Period = Period;

    /* Convert DueTime (100ns units) to milliseconds */
    if (DueTime.QuadPart < 0) {
        LONGLONG relative_100ns = -DueTime.QuadPart;
        due_ms = (DWORD)(relative_100ns / 10000);
        if (due_ms == 0 && relative_100ns > 0)
            due_ms = 1;
    } else if (DueTime.QuadPart == 0) {
        due_ms = 0;
    } else {
        LARGE_INTEGER now;
        now.QuadPart = (LONGLONG)xbox_GuestSystemTimeCoarse();
        LONGLONG diff = DueTime.QuadPart - now.QuadPart;
        due_ms = (diff > 0) ? (DWORD)(diff / 10000) : 0;
    }

    period_ms = (Period > 0) ? (DWORD)Period : 0;

    DWORD flags = 0;
    if (period_ms == 0)
        flags |= WT_EXECUTEONLYONCE;

    if (!CreateTimerQueueTimer(&Timer->win32_timer, g_timer_queue,
                               xbox_timer_callback, Timer,
                               due_ms, period_ms, flags)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "KeSetTimerEx: CreateTimerQueueTimer failed (error %u)", GetLastError());
        return was_inserted;
    }

    Timer->Inserted = TRUE;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "KeSetTimerEx: timer=%p, due=%ums, period=%ums, dpc=%p",
        Timer, due_ms, period_ms, Dpc);

    return was_inserted;
}

BOOLEAN __stdcall xbox_KeCancelTimer(PXBOX_KTIMER Timer)
{
    BOOLEAN was_inserted;

    if (!Timer)
        return FALSE;

    was_inserted = Timer->Inserted;

    if (was_inserted && Timer->win32_timer) {
        DeleteTimerQueueTimer(g_timer_queue, Timer->win32_timer, INVALID_HANDLE_VALUE);
        Timer->win32_timer = NULL;
        Timer->Inserted = FALSE;
    }

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "KeCancelTimer: timer=%p, was_inserted=%d", Timer, was_inserted);

    return was_inserted;
}

/* ============================================================================
 * Deferred Procedure Calls (DPCs)
 *
 * Xbox DPCs are typically queued from ISRs or timer callbacks to run at
 * DISPATCH_LEVEL. On Windows, we execute them immediately or via thread pool
 * since we don't have real IRQL levels.
 * ============================================================================ */

VOID __stdcall xbox_KeInitializeDpc(
    PXBOX_KDPC Dpc,
    PKDEFERRED_ROUTINE DeferredRoutine,
    PVOID DeferredContext)
{
    if (!Dpc)
        return;

    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
    Dpc->SystemArgument1 = NULL;
    Dpc->SystemArgument2 = NULL;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "KeInitializeDpc: dpc=%p, routine=%p", Dpc, DeferredRoutine);
}

/*
 * Thread pool callback for executing DPCs.
 */
static VOID CALLBACK xbox_dpc_work_callback(PTP_CALLBACK_INSTANCE Instance,
                                            PVOID Context)
{
    PXBOX_KDPC dpc = (PXBOX_KDPC)Context;

    (void)Instance;

    if (dpc && dpc->DeferredRoutine) {
        dpc->DeferredRoutine(dpc, dpc->DeferredContext,
                            dpc->SystemArgument1, dpc->SystemArgument2);
    }
}

BOOLEAN __stdcall xbox_KeInsertQueueDpc(
    PXBOX_KDPC Dpc,
    PVOID SystemArgument1,
    PVOID SystemArgument2)
{
    if (!Dpc || !Dpc->DeferredRoutine)
        return FALSE;

    Dpc->SystemArgument1 = SystemArgument1;
    Dpc->SystemArgument2 = SystemArgument2;

    /* Submit to the Windows thread pool for async execution */
    if (!TrySubmitThreadpoolCallback(xbox_dpc_work_callback, Dpc, NULL)) {
        /* Fallback: execute synchronously */
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_SYNC,
            "KeInsertQueueDpc: threadpool submit failed, executing synchronously");
        Dpc->DeferredRoutine(Dpc, Dpc->DeferredContext,
                            SystemArgument1, SystemArgument2);
    }

    return TRUE;
}

BOOLEAN __stdcall xbox_KeRemoveQueueDpc(PXBOX_KDPC Dpc)
{
    /*
     * On Windows, once submitted to the thread pool we can't easily cancel.
     * DPCs are typically very short-lived, so this is rarely called.
     * Return FALSE to indicate the DPC was not in the queue (may have already run).
     */
    (void)Dpc;
    return FALSE;
}

/* ============================================================================
 * KeSynchronizeExecution
 *
 * On Xbox, this raises IRQL to the interrupt's level and executes a routine.
 * Since we don't have real IRQLs, we just call the routine directly.
 * ============================================================================ */

BOOLEAN __stdcall xbox_KeSynchronizeExecution(
    PXBOX_KINTERRUPT Interrupt,
    PVOID SynchronizeRoutine,
    PVOID SynchronizeContext)
{
    typedef BOOLEAN (__stdcall *PKSYNCHRONIZE_ROUTINE)(PVOID);
    PKSYNCHRONIZE_ROUTINE routine = (PKSYNCHRONIZE_ROUTINE)SynchronizeRoutine;

    (void)Interrupt;

    if (!routine)
        return FALSE;

    return routine(SynchronizeContext);
}

/* ============================================================================
 * Events (continued)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtClearEvent(HANDLE EventHandle)
{
    if (!ResetEvent(EventHandle)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtClearEvent: ResetEvent failed (error %u)", GetLastError());
        return STATUS_INVALID_HANDLE;
    }
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Mutants (mutexes)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtCreateMutant(
    PHANDLE MutantHandle,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    BOOLEAN InitialOwner)
{
    HANDLE hMutex;

    (void)ObjectAttributes;

    if (!MutantHandle)
        return STATUS_INVALID_PARAMETER;

    hMutex = CreateMutexW(NULL, InitialOwner, NULL);
    if (!hMutex) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtCreateMutant: CreateMutexW failed (error %u)", GetLastError());
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *MutantHandle = hMutex;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "NtCreateMutant: handle=%p initial_owner=%d", hMutex, InitialOwner);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount)
{
    /*
     * ReleaseMutex reports the previous count only through its own bookkeeping,
     * which Win32 does not expose. Callers overwhelmingly use PreviousCount as
     * an ignore-me out-param; report 0 (the count before this release took it
     * to unheld) rather than leaving the caller's storage uninitialised.
     */
    if (!ReleaseMutex(MutantHandle)) {
        DWORD err = GetLastError();
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtReleaseMutant: ReleaseMutex failed (error %u)", err);
        /* Releasing a mutex this thread does not own is the common failure. */
        return (err == ERROR_NOT_OWNER) ? STATUS_MUTANT_NOT_OWNED
                                        : STATUS_INVALID_HANDLE;
    }

    if (PreviousCount)
        *PreviousCount = 0;

    return STATUS_SUCCESS;
}
