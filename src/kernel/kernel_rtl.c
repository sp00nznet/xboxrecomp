/*
 * kernel_rtl.c - Xbox Runtime Library Functions
 *
 * Implements Rtl* functions: critical sections, string init/conversion,
 * NTSTATUS→Win32 error mapping, time conversion, sprintf variants.
 *
 * Most of these map 1:1 to Win32 CRT functions.
 */

#include "kernel.h"
/* RECOMP_TLS, and the guest stack pointer the contention report walks. */
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

/* ============================================================================
 * String Initialization
 * ============================================================================ */

VOID __stdcall xbox_RtlInitAnsiString(PXBOX_ANSI_STRING DestinationString, const char* SourceString)
{
    if (SourceString) {
        USHORT len = (USHORT)strlen(SourceString);
        DestinationString->Length = len;
        DestinationString->MaximumLength = len + 1;
        DestinationString->Buffer = (PCHAR)SourceString;
    } else {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        DestinationString->Buffer = NULL;
    }
}

VOID __stdcall xbox_RtlInitUnicodeString(PXBOX_UNICODE_STRING DestinationString, const WCHAR* SourceString)
{
    if (SourceString) {
        USHORT len = (USHORT)(wcslen(SourceString) * sizeof(WCHAR));
        DestinationString->Length = len;
        DestinationString->MaximumLength = len + sizeof(WCHAR);
        DestinationString->Buffer = (PWCHAR)SourceString;
    } else {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        DestinationString->Buffer = NULL;
    }
}

/* ============================================================================
 * String Conversion (ANSI ↔ Unicode)
 * ============================================================================ */

NTSTATUS __stdcall xbox_RtlAnsiStringToUnicodeString(
    PXBOX_UNICODE_STRING DestinationString,
    PXBOX_ANSI_STRING SourceString,
    BOOLEAN AllocateDestinationString)
{
    ULONG unicode_len;

    if (!DestinationString || !SourceString)
        return STATUS_INVALID_PARAMETER;

    unicode_len = (SourceString->Length + 1) * sizeof(WCHAR);

    if (AllocateDestinationString) {
        DestinationString->Buffer = (PWCHAR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, unicode_len);
        if (!DestinationString->Buffer)
            return STATUS_NO_MEMORY;
        DestinationString->MaximumLength = (USHORT)unicode_len;
    } else if (DestinationString->MaximumLength < unicode_len) {
        return STATUS_BUFFER_OVERFLOW;
    }

    int result = MultiByteToWideChar(CP_ACP, 0,
        SourceString->Buffer, SourceString->Length,
        DestinationString->Buffer, DestinationString->MaximumLength / sizeof(WCHAR));

    if (result > 0) {
        DestinationString->Length = (USHORT)(result * sizeof(WCHAR));
        DestinationString->Buffer[result] = L'\0';
        return STATUS_SUCCESS;
    }

    return STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall xbox_RtlUnicodeStringToAnsiString(
    PXBOX_ANSI_STRING DestinationString,
    PXBOX_UNICODE_STRING SourceString,
    BOOLEAN AllocateDestinationString)
{
    ULONG ansi_len;

    if (!DestinationString || !SourceString)
        return STATUS_INVALID_PARAMETER;

    ansi_len = SourceString->Length / sizeof(WCHAR) + 1;

    if (AllocateDestinationString) {
        DestinationString->Buffer = (PCHAR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ansi_len);
        if (!DestinationString->Buffer)
            return STATUS_NO_MEMORY;
        DestinationString->MaximumLength = (USHORT)ansi_len;
    } else if (DestinationString->MaximumLength < ansi_len) {
        return STATUS_BUFFER_OVERFLOW;
    }

    int result = WideCharToMultiByte(CP_ACP, 0,
        SourceString->Buffer, SourceString->Length / sizeof(WCHAR),
        DestinationString->Buffer, DestinationString->MaximumLength,
        NULL, NULL);

    if (result >= 0) {
        DestinationString->Length = (USHORT)result;
        if ((USHORT)result < DestinationString->MaximumLength)
            DestinationString->Buffer[result] = '\0';
        return STATUS_SUCCESS;
    }

    return STATUS_UNSUCCESSFUL;
}

/* ============================================================================
 * String Comparison
 * ============================================================================ */

BOOLEAN __stdcall xbox_RtlEqualString(
    PXBOX_ANSI_STRING String1,
    PXBOX_ANSI_STRING String2,
    BOOLEAN CaseInSensitive)
{
    if (String1->Length != String2->Length)
        return FALSE;

    if (CaseInSensitive)
        return _strnicmp(String1->Buffer, String2->Buffer, String1->Length) == 0;
    else
        return strncmp(String1->Buffer, String2->Buffer, String1->Length) == 0;
}

/*
 * RtlCompareMemoryUlong - Scans memory for a ULONG pattern.
 * Returns the number of bytes that matched.
 */
ULONG __stdcall xbox_RtlCompareMemoryUlong(PVOID Source, ULONG Length, ULONG Pattern)
{
    PULONG src = (PULONG)Source;
    ULONG count = Length / sizeof(ULONG);

    for (ULONG i = 0; i < count; i++) {
        if (src[i] != Pattern)
            return i * sizeof(ULONG);
    }
    return count * sizeof(ULONG);
}

/* ============================================================================
 * Critical Sections (direct 1:1 mapping)
 * ============================================================================ */

/*
 * Critical sections, shadowed onto real host locks.
 *
 * These used to be no-ops, on the stated grounds that every Xbox thread ran
 * synchronously so there was no contention. That stopped being true when
 * PsCreateSystemThreadEx began spawning real threads: a title's worker and its
 * main thread now run at the same time on different host threads, and the
 * locks the title takes to keep them apart did nothing.
 *
 * The failure that produced is worth describing, because it looks like
 * anything but a lock. Half-Life 2's level load faulted intermittently -- four
 * runs, three different functions, one clean -- and any attempt to observe it
 * made it disappear, including two plain stores to globals. Registers held
 * fragments of strings where pointers belonged, which is what a half-updated
 * structure looks like from the other thread.
 *
 * The Xbox RTL_CRITICAL_SECTION is 20 bytes of 32-bit layout and a Win32
 * CRITICAL_SECTION is 40 bytes of 64-bit layout, so the guest's own structure
 * cannot back a host lock. Shadow it instead: the guest pointer is the key,
 * the host lock lives here, and the guest's bytes are left alone. Win32
 * critical sections are recursive, which matches RTL semantics.
 *
 * Lazily created on first use rather than in RtlInitializeCriticalSection,
 * because a title can perfectly well use a statically zeroed one it never
 * announced.
 */
#define XBOX_CS_TABLE_SIZE 4096

typedef struct {
    uintptr_t        key;        /* guest critical section; 0 means free */
    CRITICAL_SECTION cs;
    /* Who holds it, and the guest call site that took it.
     *
     * A deadlock report can already say which lock and which thread. What it
     * cannot say is where the holder took it, and that is the only part that
     * points at code. Written after the lock is held and cleared as the last
     * recursion is released, so a waiter reads a consistent pair or a stale
     * one -- never a torn address. Nothing depends on them being current;
     * they exist to be printed. */
    volatile uint32_t      owner_site;
    volatile unsigned long owner_tid;
    volatile long          depth;
} XBOX_CS_SLOT;

static XBOX_CS_SLOT g_cs_table[XBOX_CS_TABLE_SIZE];

/* The slot a shadow lock lives in, or NULL if it is not one of ours --
 * RECOMP_CS_MODE=single hands out a lock that belongs to no slot. */
static XBOX_CS_SLOT* xbox_cs_slot_of(CRITICAL_SECTION* cs)
{
    char* p = (char*)cs;
    if (p < (char*)&g_cs_table[0]
            || p >= (char*)&g_cs_table[XBOX_CS_TABLE_SIZE])
        return NULL;
    return (XBOX_CS_SLOT*)(p - offsetof(XBOX_CS_SLOT, cs));
}
static SRWLOCK      g_cs_table_lock = SRWLOCK_INIT;
static int          g_cs_table_full;

/* RECOMP_CS_MODE=single puts every guest critical section behind one recursive
 * lock.
 *
 * Deadlock-free by construction -- there is only one lock, so there is no order
 * to invert -- and deliberately NOT the default. A title that deadlocks under
 * real locks is telling you its threads are reaching a place the console's
 * would not, and collapsing the hierarchy hides that rather than answering it.
 * What it is good for is deciding whether a lock cycle is the last thing in the
 * way: if the title still does not get further with this on, the cycle was not
 * what was stopping it.
 *
 * Safe only because guest code here never holds a lock while waiting on another
 * guest thread to make progress -- the runtime's own I/O completes
 * synchronously. That would not hold in general.
 */
static CRITICAL_SECTION g_cs_single;
static INIT_ONCE g_cs_single_once = INIT_ONCE_STATIC_INIT;
static int g_cs_single_mode = -1;

static BOOL CALLBACK xbox_cs_single_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_cs_single);
    return TRUE;
}

static CRITICAL_SECTION* xbox_cs_shadow(PRTL_CRITICAL_SECTION guest)
{
    if (g_cs_single_mode < 0) {
        const char* mode = getenv("RECOMP_CS_MODE");
        g_cs_single_mode = (mode && !strcmp(mode, "single")) ? 1 : 0;
        if (g_cs_single_mode)
            fprintf(stderr, "  [CS] RECOMP_CS_MODE=single: every guest lock "
                            "shares one recursive lock\n");
    }
    if (g_cs_single_mode) {
        if (!guest)
            return NULL;
        InitOnceExecuteOnce(&g_cs_single_once, xbox_cs_single_init, NULL, NULL);
        return &g_cs_single;
    }
    uintptr_t key = (uintptr_t)guest;
    CRITICAL_SECTION* found = NULL;
    size_t home, i;

    if (!key)
        return NULL;
    home = (size_t)((key >> 4) % XBOX_CS_TABLE_SIZE);

    /* Shared pass first: after start-up almost every call finds an existing
     * entry, and taking the table exclusively for each of those would put a
     * global serialisation point in front of the title's own locks. */
    AcquireSRWLockShared(&g_cs_table_lock);
    for (i = 0; i < XBOX_CS_TABLE_SIZE; i++) {
        XBOX_CS_SLOT* slot = &g_cs_table[(home + i) % XBOX_CS_TABLE_SIZE];
        if (slot->key == key) { found = &slot->cs; break; }
        if (slot->key == 0) break;
    }
    ReleaseSRWLockShared(&g_cs_table_lock);
    if (found)
        return found;

    AcquireSRWLockExclusive(&g_cs_table_lock);
    for (i = 0; i < XBOX_CS_TABLE_SIZE; i++) {
        XBOX_CS_SLOT* slot = &g_cs_table[(home + i) % XBOX_CS_TABLE_SIZE];
        if (slot->key == key) { found = &slot->cs; break; }
        if (slot->key == 0) {
            InitializeCriticalSection(&slot->cs);
            slot->key = key;             /* published last: a shared reader
                                          * that sees the key sees a live
                                          * lock behind it */
            found = &slot->cs;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_cs_table_lock);

    if (!found && !g_cs_table_full) {
        g_cs_table_full = 1;
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_RTL,
                 "critical-section table full at %d entries; further locks "
                 "are unprotected", XBOX_CS_TABLE_SIZE);
    }
    return found;
}

/* Contention is worth saying out loud.
 *
 * Now that these locks are real, a title that leaks one -- or a thread of ours
 * that holds one somewhere the console's would not -- deadlocks instead of
 * sailing through. That looks like a hang with no output at all, so the first
 * time a lock is actually contended, say which one and who is waiting.
 * TryEnter first, so the common uncontended path costs nothing extra.
 */
/* The guest VA is what cross-references against a disassembly; the native
 * pointer does not. */
extern ptrdiff_t g_xbox_mem_offset;

extern RECOMP_TLS uint32_t g_esp;
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;

/* A guest backtrace, printed where the guest is standing.
 *
 * This runs on the guest thread -- g_esp is thread-local and therefore real
 * here, which it is not in the watchdog or any sampling thread. Lifted calls
 * push their guest return address before jumping, so scanning the stack for
 * words that land in the code range recovers the chain. Approximate by
 * construction: a stale value from an earlier call can look like a frame. It
 * is still the difference between knowing two threads are deadlocked and
 * knowing which two paths did it. */
static void xbox_guest_backtrace(int depth)
{
    uint32_t sp = g_esp;
    int shown = 0;
    int i;

    if (!sp || !g_xbox_code_hi)
        return;
    for (i = 0; i < 256 && shown < depth; i++) {
        uint32_t word = *(const uint32_t *)((uintptr_t)(sp + i * 4)
                                            + g_xbox_mem_offset);
        if (word >= g_xbox_code_lo && word < g_xbox_code_hi) {
            fprintf(stderr, "  [CS]     guest 0x%08X\n", word);
            shown++;
        }
    }
}

static LONG g_cs_contention_reports;
static LONG g_cs_enters, g_cs_leaves;

/* Which CRT lock is this?
 *
 * MSVC keeps its internal locks in a table of {CRITICAL_SECTION*, refcount}
 * pairs, so a contended address can be named by its index rather than left as
 * a bare pointer. The index is what the CRT's own headers document: 1 is the
 * stdio scan lock, 4 the heap, and 16 upwards are the per-stream locks. The
 * table's address is per-title, so it is supplied rather than assumed.
 */
static uint32_t g_crt_lock_table, g_crt_lock_count;

void xbox_SetCrtLockTable(uint32_t table_va, uint32_t count)
{
    g_crt_lock_table = table_va;
    g_crt_lock_count = count;
}

static int xbox_crt_lock_index(uint32_t guest_va)
{
    uint32_t i;

    if (!g_crt_lock_table)
        return -1;
    for (i = 0; i < g_crt_lock_count; i++) {
        uint32_t slot = *(const uint32_t *)((uintptr_t)(g_crt_lock_table + i * 8)
                                            + g_xbox_mem_offset);
        if (slot == guest_va)
            return (int)i;
    }
    return -1;
}

/* Every acquisition and release of a lock the CRT names, in order.
 *
 * A deadlock report says which two locks are crossed but not how they got that
 * way, and the two candidate explanations need opposite fixes: the title really
 * does take them in two orders (its problem, and it shipped, so unlikely), or
 * this runtime dropped a release somewhere and a lock that should be free is
 * still held (our problem). The order in this log tells them apart.
 *
 * Gated on RECOMP_CS_TRACE_CRT because it is one table scan per lock operation
 * and the CRT locks are hot. */
static void crt_lock_trace(const char *what, PRTL_CRITICAL_SECTION guest)
{
    static int enabled = -1;
    int idx;

    static int all;
    uint32_t va;

    if (enabled < 0) {
        const char *v = getenv("RECOMP_CS_TRACE_CRT");
        enabled = v != NULL;
        all = v && !strcmp(v, "all");
    }
    if (!enabled)
        return;
    va = (uint32_t)((uintptr_t)guest - (uintptr_t)g_xbox_mem_offset);
    idx = xbox_crt_lock_index(va);
    /* "all" logs by address as well as index. Whether a lock's release is
     * missing or merely unrecognised by the index lookup are different bugs,
     * and only the raw address separates them. */
    /* One lock, both sides, with the guest stack that got there.
     *
     * A take at one address and a release 0x78 lower is not a lock protocol
     * this runtime can reason about from addresses alone -- it needs the two
     * call sites, because the question is whether the guest computed different
     * addresses or we did. */
    {
        static int watch = -1;
        static uint32_t watch_va;
        if (watch < 0) {
            const char *w = getenv("RECOMP_CS_WATCH");
            watch = w != NULL;
            watch_va = w ? (uint32_t)strtoul(w, NULL, 0) : 0;
        }
        if (watch && va == watch_va) {
            fprintf(stderr, "  [CSWATCH] t%-6lu %s 0x%08X\n",
                    GetCurrentThreadId(), what, va);
            xbox_guest_backtrace(8);
            fflush(stderr);
        }
    }

    if (all) {
        fprintf(stderr, "  [CSTRACE] t%-6lu %-4s 0x%08X idx %d\n",
                GetCurrentThreadId(), what, va, idx);
        fflush(stderr);
        return;
    }
    if (idx < 0)
        return;
    fprintf(stderr, "  [CSTRACE] t%-6lu %-4s lock %d\n",
            GetCurrentThreadId(), what, idx);
    fflush(stderr);
}

extern RECOMP_TLS uint32_t g_xbox_kernel_caller;

static void xbox_cs_note_owner(CRITICAL_SECTION* cs)
{
    XBOX_CS_SLOT* slot = xbox_cs_slot_of(cs);
    if (!slot)
        return;
    slot->owner_site = g_xbox_kernel_caller;
    slot->owner_tid  = GetCurrentThreadId();
    slot->depth++;
}

static void xbox_cs_clear_owner(CRITICAL_SECTION* cs)
{
    XBOX_CS_SLOT* slot = xbox_cs_slot_of(cs);
    if (!slot)
        return;
    if (--slot->depth <= 0) {
        slot->depth = 0;
        slot->owner_tid = 0;
    }
}

VOID __stdcall xbox_RtlEnterCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    CRITICAL_SECTION* cs = xbox_cs_shadow(CriticalSection);
    if (!cs)
        return;
    InterlockedIncrement(&g_cs_enters);
    crt_lock_trace("take", CriticalSection);
    if (TryEnterCriticalSection(cs)) {
        xbox_cs_note_owner(cs);
        return;
    }
    if (InterlockedIncrement(&g_cs_contention_reports) <= 16) {
        fprintf(stderr, "  [CS] thread %lu waiting on guest lock 0x%08X"
                        " (held by thread %lu)\n",
                GetCurrentThreadId(),
                (uint32_t)((uintptr_t)CriticalSection
                           - (uintptr_t)g_xbox_mem_offset),
                (unsigned long)(uintptr_t)cs->OwningThread);
        {
            int idx = xbox_crt_lock_index(
                (uint32_t)((uintptr_t)CriticalSection
                           - (uintptr_t)g_xbox_mem_offset));
            if (idx >= 0)
                fprintf(stderr, "  [CS]   that is CRT lock %d\n", idx);
        }
        {
            /* Where the holder took it. The waiter's own backtrace below
             * says who is blocked; this is the half that says who to look
             * at. */
            XBOX_CS_SLOT* slot = xbox_cs_slot_of(cs);
            if (slot && slot->owner_tid)
                fprintf(stderr, "  [CS]   holder thread %lu took it at guest"
                                " 0x%08X (depth %ld)\n",
                        slot->owner_tid, slot->owner_site, slot->depth);
        }
        fprintf(stderr, "  [CS] enters=%ld leaves=%ld (outstanding %ld)\n",
                g_cs_enters, g_cs_leaves, g_cs_enters - g_cs_leaves);
        xbox_guest_backtrace(10);
        fflush(stderr);
    }
    EnterCriticalSection(cs);
    xbox_cs_note_owner(cs);
    if (g_cs_contention_reports <= 16) {
        fprintf(stderr, "  [CS] thread %lu acquired 0x%08X\n",
                GetCurrentThreadId(),
                (uint32_t)((uintptr_t)CriticalSection
                           - (uintptr_t)g_xbox_mem_offset));
        fflush(stderr);
    }
}

VOID __stdcall xbox_RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    CRITICAL_SECTION* cs = xbox_cs_shadow(CriticalSection);
    if (cs) {
        InterlockedIncrement(&g_cs_leaves);
        crt_lock_trace("drop", CriticalSection);
        xbox_cs_clear_owner(cs);
        LeaveCriticalSection(cs);
    }
}

VOID __stdcall xbox_RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    /* Creating the shadow here is not required -- Enter does it -- but doing
     * it now keeps the first Enter off the exclusive path. */
    (void)xbox_cs_shadow(CriticalSection);
}

/* ============================================================================
 * NTSTATUS → Win32 Error Code Mapping
 * ============================================================================ */

ULONG __stdcall xbox_RtlNtStatusToDosError(NTSTATUS Status)
{
    switch (Status) {
        case STATUS_SUCCESS:                    return ERROR_SUCCESS;
        case STATUS_INVALID_PARAMETER:          return ERROR_INVALID_PARAMETER;
        case STATUS_NO_MEMORY:                  return ERROR_NOT_ENOUGH_MEMORY;
        case STATUS_INSUFFICIENT_RESOURCES:     return ERROR_NO_SYSTEM_RESOURCES;
        case STATUS_ACCESS_DENIED:              return ERROR_ACCESS_DENIED;
        case STATUS_OBJECT_NAME_NOT_FOUND:      return ERROR_FILE_NOT_FOUND;
        case STATUS_OBJECT_PATH_NOT_FOUND:      return ERROR_PATH_NOT_FOUND;
        case STATUS_OBJECT_NAME_COLLISION:      return ERROR_ALREADY_EXISTS;
        case STATUS_NO_SUCH_FILE:               return ERROR_FILE_NOT_FOUND;
        case STATUS_END_OF_FILE:                return ERROR_HANDLE_EOF;
        case STATUS_INVALID_HANDLE:             return ERROR_INVALID_HANDLE;
        case STATUS_NOT_IMPLEMENTED:            return ERROR_CALL_NOT_IMPLEMENTED;
        case STATUS_UNSUCCESSFUL:               return ERROR_GEN_FAILURE;
        case STATUS_PENDING:                    return ERROR_IO_PENDING;
        case STATUS_BUFFER_OVERFLOW:            return ERROR_MORE_DATA;
        case STATUS_NO_MORE_FILES:              return ERROR_NO_MORE_FILES;
        case STATUS_NOT_SUPPORTED:              return ERROR_NOT_SUPPORTED;
        case STATUS_CANCELLED:                  return ERROR_CANCELLED;
        case STATUS_ALREADY_COMMITTED:          return ERROR_COMMITMENT_LIMIT;
        default:
            /* Fall back to RtlNtStatusToDosError from ntdll if available */
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_RTL,
                "RtlNtStatusToDosError: unmapped status 0x%08X", Status);
            return ERROR_MR_MID_NOT_FOUND;
    }
}

/* ============================================================================
 * Time Conversion
 * ============================================================================ */

BOOLEAN __stdcall xbox_RtlTimeFieldsToTime(PXBOX_TIME_FIELDS TimeFields, PLARGE_INTEGER Time)
{
    SYSTEMTIME st;
    FILETIME ft;

    st.wYear         = (WORD)TimeFields->Year;
    st.wMonth        = (WORD)TimeFields->Month;
    st.wDayOfWeek    = (WORD)TimeFields->Weekday;
    st.wDay          = (WORD)TimeFields->Day;
    st.wHour         = (WORD)TimeFields->Hour;
    st.wMinute       = (WORD)TimeFields->Minute;
    st.wSecond       = (WORD)TimeFields->Second;
    st.wMilliseconds = (WORD)TimeFields->Milliseconds;

    if (!SystemTimeToFileTime(&st, &ft))
        return FALSE;

    Time->LowPart  = ft.dwLowDateTime;
    Time->HighPart = ft.dwHighDateTime;
    return TRUE;
}

VOID __stdcall xbox_RtlTimeToTimeFields(PLARGE_INTEGER Time, PXBOX_TIME_FIELDS TimeFields)
{
    FILETIME ft;
    SYSTEMTIME st;

    ft.dwLowDateTime  = Time->LowPart;
    ft.dwHighDateTime = Time->HighPart;

    if (FileTimeToSystemTime(&ft, &st)) {
        TimeFields->Year         = (SHORT)st.wYear;
        TimeFields->Month        = (SHORT)st.wMonth;
        TimeFields->Day          = (SHORT)st.wDay;
        TimeFields->Hour         = (SHORT)st.wHour;
        TimeFields->Minute       = (SHORT)st.wMinute;
        TimeFields->Second       = (SHORT)st.wSecond;
        TimeFields->Milliseconds = (SHORT)st.wMilliseconds;
        TimeFields->Weekday      = (SHORT)st.wDayOfWeek;
    } else {
        memset(TimeFields, 0, sizeof(XBOX_TIME_FIELDS));
    }
}

/* ============================================================================
 * Exception Handling
 * ============================================================================ */

VOID __stdcall xbox_RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PVOID ExceptionRecord, PVOID ReturnValue)
{
    /* Delegate to Win32 RtlUnwind */
    RtlUnwind(TargetFrame, TargetIp, (PEXCEPTION_RECORD)ExceptionRecord, ReturnValue);
}

VOID __stdcall xbox_RtlRaiseException(PVOID ExceptionRecord)
{
    RaiseException(
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionCode,
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionFlags,
        ((PEXCEPTION_RECORD)ExceptionRecord)->NumberParameters,
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionInformation);
}

VOID __stdcall xbox_RtlRip(PCHAR ApiName, PCHAR Expression, PCHAR Message)
{
    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_RTL, "RtlRip: %s - %s: %s",
        ApiName ? ApiName : "?",
        Expression ? Expression : "?",
        Message ? Message : "?");

#ifdef _DEBUG
    DebugBreak();
#endif
}

/* ============================================================================
 * String Formatting (Rtl sprintf variants → CRT)
 * ============================================================================ */

int __cdecl xbox_RtlSnprintf(char* buffer, size_t count, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, count, format, args);
    va_end(args);
    return result;
}

int __cdecl xbox_RtlSprintf(char* buffer, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsprintf(buffer, format, args);
    va_end(args);
    return result;
}

int __cdecl xbox_RtlVsnprintf(char* buffer, size_t count, const char* format, va_list argptr)
{
    return vsnprintf(buffer, count, format, argptr);
}

int __cdecl xbox_RtlVsprintf(char* buffer, const char* format, va_list argptr)
{
    return vsprintf(buffer, format, argptr);
}
