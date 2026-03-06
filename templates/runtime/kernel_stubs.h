/**
 * Xbox Kernel Stub Template
 *
 * Template with stub implementations for the most commonly imported
 * Xbox kernel functions. Each stub returns a sensible default value
 * and logs the call for debugging.
 *
 * Usage:
 *   1. Copy this file into your project's kernel implementation directory
 *   2. Replace stub bodies with real implementations as needed
 *   3. Wire each function into your kernel thunk table
 *
 * The ordinal numbers in comments correspond to the Xbox kernel export
 * table. See docs/formats/kernel-exports.md for full documentation.
 *
 * Calling convention:
 *   Most Xbox kernel functions use __stdcall (callee cleans stack).
 *   A few use __fastcall (KfRaiseIrql, KfLowerIrql, ObfDereferenceObject).
 *   DbgPrint and Rtl*printf use __cdecl (caller cleans stack).
 */

#ifndef XBOX_KERNEL_STUBS_H
#define XBOX_KERNEL_STUBS_H

#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Status codes (Xbox uses NT status codes)
 * ================================================================ */

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS          ((long)0x00000000L)
#endif
#ifndef STATUS_PENDING
#define STATUS_PENDING          ((long)0x00000103L)
#endif
#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL     ((long)0xC0000001L)
#endif
#ifndef STATUS_NOT_IMPLEMENTED
#define STATUS_NOT_IMPLEMENTED  ((long)0xC0000002L)
#endif
#ifndef STATUS_NO_MEMORY
#define STATUS_NO_MEMORY        ((long)0xC0000017L)
#endif

typedef long NTSTATUS;
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)

/* ================================================================
 * Logging macro (replace with your logging system)
 * ================================================================ */

#ifndef KSTUB_LOG
#define KSTUB_LOG(fmt, ...) \
    fprintf(stderr, "[KSTUB] " fmt "\n", ##__VA_ARGS__)
#endif

/* ================================================================
 * Memory Management Stubs
 * ================================================================ */

/* Ordinal 165: MmAllocateContiguousMemory */
static inline void* __stdcall kstub_MmAllocateContiguousMemory(
    unsigned long NumberOfBytes)
{
    void* p = VirtualAlloc(NULL, NumberOfBytes,
                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    KSTUB_LOG("MmAllocateContiguousMemory(%u) = %p", NumberOfBytes, p);
    return p;
}

/* Ordinal 166: MmAllocateContiguousMemoryEx */
static inline void* __stdcall kstub_MmAllocateContiguousMemoryEx(
    unsigned long NumberOfBytes, uintptr_t LowestAddr, uintptr_t HighestAddr,
    unsigned long Alignment, unsigned long Protect)
{
    void* p = VirtualAlloc(NULL, NumberOfBytes,
                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    KSTUB_LOG("MmAllocateContiguousMemoryEx(%u, align=%u) = %p",
              NumberOfBytes, Alignment, p);
    return p;
}

/* Ordinal 171: MmFreeContiguousMemory */
static inline void __stdcall kstub_MmFreeContiguousMemory(void* BaseAddress)
{
    KSTUB_LOG("MmFreeContiguousMemory(%p)", BaseAddress);
    if (BaseAddress) VirtualFree(BaseAddress, 0, MEM_RELEASE);
}

/* Ordinal 184: NtAllocateVirtualMemory */
static inline NTSTATUS __stdcall kstub_NtAllocateVirtualMemory(
    void** BaseAddress, uintptr_t ZeroBits, size_t* RegionSize,
    unsigned long AllocationType, unsigned long Protect)
{
    KSTUB_LOG("NtAllocateVirtualMemory(size=%zu)", *RegionSize);
    void* p = VirtualAlloc(*BaseAddress, *RegionSize,
                           AllocationType, Protect);
    if (p) { *BaseAddress = p; return STATUS_SUCCESS; }
    return STATUS_NO_MEMORY;
}

/* Ordinal 199: NtFreeVirtualMemory */
static inline NTSTATUS __stdcall kstub_NtFreeVirtualMemory(
    void** BaseAddress, size_t* RegionSize, unsigned long FreeType)
{
    KSTUB_LOG("NtFreeVirtualMemory(%p)", *BaseAddress);
    VirtualFree(*BaseAddress, 0, MEM_RELEASE);
    return STATUS_SUCCESS;
}

/* Ordinal 167: MmMapIoSpace */
static inline void* __stdcall kstub_MmMapIoSpace(
    uintptr_t PhysicalAddress, unsigned long NumberOfBytes, unsigned long Protect)
{
    /* Return a stub buffer that absorbs GPU register writes */
    void* p = VirtualAlloc(NULL, NumberOfBytes,
                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    KSTUB_LOG("MmMapIoSpace(phys=0x%08X, size=%u) = %p [STUB]",
              (unsigned)PhysicalAddress, NumberOfBytes, p);
    return p;
}

/* Ordinal 181: MmQueryStatistics */
static inline NTSTATUS __stdcall kstub_MmQueryStatistics(void* Stats)
{
    /* Fill with plausible values: 64 MB total, ~50 MB available */
    memset(Stats, 0, 36);  /* sizeof(MM_STATISTICS) */
    *(unsigned long*)((char*)Stats + 0) = 36;       /* Length */
    *(unsigned long*)((char*)Stats + 4) = 16384;    /* TotalPhysicalPages (64MB/4KB) */
    *(unsigned long*)((char*)Stats + 8) = 12288;    /* AvailablePages (~48MB) */
    KSTUB_LOG("MmQueryStatistics -> 64MB total, ~48MB free");
    return STATUS_SUCCESS;
}

/* Ordinal 173: MmGetPhysicalAddress */
static inline uintptr_t __stdcall kstub_MmGetPhysicalAddress(void* BaseAddress)
{
    /* On Xbox, physical == virtual for most addresses */
    return (uintptr_t)BaseAddress;
}

/* Ordinal 15: ExAllocatePool */
static inline void* __stdcall kstub_ExAllocatePool(unsigned long NumberOfBytes)
{
    void* p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, NumberOfBytes);
    KSTUB_LOG("ExAllocatePool(%u) = %p", NumberOfBytes, p);
    return p;
}

/* Ordinal 16: ExAllocatePoolWithTag */
static inline void* __stdcall kstub_ExAllocatePoolWithTag(
    unsigned long NumberOfBytes, unsigned long Tag)
{
    return kstub_ExAllocatePool(NumberOfBytes);
}

/* ================================================================
 * File I/O Stubs
 *
 * NOTE: File I/O requires a path translation layer to map Xbox paths
 * (\Device\CdRom0\, D:\, T:\) to Windows paths. These stubs return
 * STATUS_NOT_IMPLEMENTED. Replace with real implementations.
 * ================================================================ */

/* Ordinal 190: NtCreateFile */
static inline NTSTATUS __stdcall kstub_NtCreateFile(
    HANDLE* FileHandle, unsigned long DesiredAccess,
    void* ObjectAttributes, void* IoStatusBlock,
    void* AllocationSize, unsigned long FileAttributes,
    unsigned long ShareAccess, unsigned long CreateDisposition,
    unsigned long CreateOptions)
{
    KSTUB_LOG("NtCreateFile [STUB - implement path translation]");
    *FileHandle = INVALID_HANDLE_VALUE;
    return STATUS_NOT_IMPLEMENTED;
}

/* Ordinal 219: NtReadFile */
static inline NTSTATUS __stdcall kstub_NtReadFile(
    HANDLE FileHandle, HANDLE Event, void* ApcRoutine, void* ApcContext,
    void* IoStatusBlock, void* Buffer, unsigned long Length,
    void* ByteOffset)
{
    KSTUB_LOG("NtReadFile(handle=%p, len=%u) [STUB]", FileHandle, Length);
    return STATUS_NOT_IMPLEMENTED;
}

/* Ordinal 236: NtWriteFile */
static inline NTSTATUS __stdcall kstub_NtWriteFile(
    HANDLE FileHandle, HANDLE Event, void* ApcRoutine, void* ApcContext,
    void* IoStatusBlock, void* Buffer, unsigned long Length,
    void* ByteOffset)
{
    KSTUB_LOG("NtWriteFile(handle=%p, len=%u) [STUB]", FileHandle, Length);
    return STATUS_NOT_IMPLEMENTED;
}

/* Ordinal 187: NtClose */
static inline NTSTATUS __stdcall kstub_NtClose(HANDLE Handle)
{
    KSTUB_LOG("NtClose(%p)", Handle);
    if (Handle && Handle != INVALID_HANDLE_VALUE)
        CloseHandle(Handle);
    return STATUS_SUCCESS;
}

/* Ordinal 211: NtQueryInformationFile */
static inline NTSTATUS __stdcall kstub_NtQueryInformationFile(
    HANDLE FileHandle, void* IoStatusBlock,
    void* FileInformation, unsigned long Length,
    int FileInformationClass)
{
    KSTUB_LOG("NtQueryInformationFile(class=%d) [STUB]", FileInformationClass);
    return STATUS_NOT_IMPLEMENTED;
}

/* ================================================================
 * Threading Stubs
 * ================================================================ */

/* Ordinal 255: PsCreateSystemThreadEx */
static inline NTSTATUS __stdcall kstub_PsCreateSystemThreadEx(
    HANDLE* ThreadHandle, unsigned long ThreadExtraSize,
    unsigned long KernelStackSize, unsigned long TlsDataSize,
    unsigned long* ThreadId, void* StartContext1, void* StartContext2,
    int CreateSuspended, int DebugStack, void* StartRoutine)
{
    DWORD tid = 0;
    *ThreadHandle = CreateThread(NULL, KernelStackSize,
                                 (LPTHREAD_START_ROUTINE)StartRoutine,
                                 StartContext1,
                                 CreateSuspended ? CREATE_SUSPENDED : 0,
                                 &tid);
    if (ThreadId) *ThreadId = tid;
    KSTUB_LOG("PsCreateSystemThreadEx -> handle=%p, tid=%u",
              *ThreadHandle, tid);
    return *ThreadHandle ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

/* Ordinal 258: PsTerminateSystemThread */
static inline NTSTATUS __stdcall kstub_PsTerminateSystemThread(NTSTATUS ExitStatus)
{
    KSTUB_LOG("PsTerminateSystemThread(%d)", ExitStatus);
    ExitThread((DWORD)ExitStatus);
    return STATUS_SUCCESS;  /* unreachable */
}

/* Ordinal 256: KeDelayExecutionThread */
static inline NTSTATUS __stdcall kstub_KeDelayExecutionThread(
    char WaitMode, int Alertable, LARGE_INTEGER* Interval)
{
    /* Negative interval = relative time in 100ns units */
    if (Interval && Interval->QuadPart < 0) {
        DWORD ms = (DWORD)((-Interval->QuadPart) / 10000);
        Sleep(ms);
    }
    return STATUS_SUCCESS;
}

/* Ordinal 238: NtYieldExecution */
static inline NTSTATUS __stdcall kstub_NtYieldExecution(void)
{
    SwitchToThread();
    return STATUS_SUCCESS;
}

/* ================================================================
 * Synchronization Stubs
 * ================================================================ */

/* Ordinal 189: NtCreateEvent */
static inline NTSTATUS __stdcall kstub_NtCreateEvent(
    HANDLE* EventHandle, void* ObjectAttributes,
    unsigned long EventType, int InitialState)
{
    /* EventType: 0 = Notification (manual reset), 1 = Synchronization (auto reset) */
    *EventHandle = CreateEventA(NULL, (EventType == 0), InitialState, NULL);
    KSTUB_LOG("NtCreateEvent(type=%u) -> %p", EventType, *EventHandle);
    return *EventHandle ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

/* Ordinal 225: NtSetEvent */
static inline NTSTATUS __stdcall kstub_NtSetEvent(
    HANDLE EventHandle, long* PreviousState)
{
    SetEvent(EventHandle);
    if (PreviousState) *PreviousState = 0;
    return STATUS_SUCCESS;
}

/* Ordinal 234: NtWaitForSingleObject */
static inline NTSTATUS __stdcall kstub_NtWaitForSingleObject(
    HANDLE Handle, int Alertable, LARGE_INTEGER* Timeout)
{
    DWORD ms = INFINITE;
    if (Timeout) {
        if (Timeout->QuadPart < 0)
            ms = (DWORD)((-Timeout->QuadPart) / 10000);
        else if (Timeout->QuadPart == 0)
            ms = 0;
    }
    DWORD result = WaitForSingleObject(Handle, ms);
    return (result == WAIT_OBJECT_0) ? STATUS_SUCCESS : STATUS_PENDING;
}

/* Ordinal 291: RtlInitializeCriticalSection */
static inline void __stdcall kstub_RtlInitializeCriticalSection(
    CRITICAL_SECTION* CriticalSection)
{
    InitializeCriticalSection(CriticalSection);
}

/* Ordinal 277: RtlEnterCriticalSection */
static inline void __stdcall kstub_RtlEnterCriticalSection(
    CRITICAL_SECTION* CriticalSection)
{
    EnterCriticalSection(CriticalSection);
}

/* Ordinal 294: RtlLeaveCriticalSection */
static inline void __stdcall kstub_RtlLeaveCriticalSection(
    CRITICAL_SECTION* CriticalSection)
{
    LeaveCriticalSection(CriticalSection);
}

/* ================================================================
 * HAL & System Stubs
 * ================================================================ */

/* Ordinal 126: KeQueryPerformanceCounter */
static inline LARGE_INTEGER __stdcall kstub_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter;
}

/* Ordinal 127: KeQueryPerformanceFrequency */
static inline LARGE_INTEGER __stdcall kstub_KeQueryPerformanceFrequency(void)
{
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return freq;
}

/* Ordinal 128: KeQuerySystemTime */
static inline void __stdcall kstub_KeQuerySystemTime(LARGE_INTEGER* CurrentTime)
{
    GetSystemTimeAsFileTime((FILETIME*)CurrentTime);
}

/* Ordinal 151: KeStallExecutionProcessor */
static inline void __stdcall kstub_KeStallExecutionProcessor(unsigned long MicroSeconds)
{
    /* Busy-wait is rarely needed; Sleep(0) yields the timeslice */
    if (MicroSeconds > 1000)
        Sleep(MicroSeconds / 1000);
}

/* Ordinal 160: KfRaiseIrql (__fastcall) */
static inline unsigned char __fastcall kstub_KfRaiseIrql(unsigned char NewIrql)
{
    /* IRQL is meaningless on PC - return PASSIVE_LEVEL */
    return 0;
}

/* Ordinal 161: KfLowerIrql (__fastcall) */
static inline void __fastcall kstub_KfLowerIrql(unsigned char NewIrql)
{
    /* No-op on PC */
}

/* Ordinal 129: KeRaiseIrqlToDpcLevel */
static inline unsigned char __stdcall kstub_KeRaiseIrqlToDpcLevel(void)
{
    return 0;  /* Was at PASSIVE_LEVEL */
}

/* Ordinal 49: HalReturnToFirmware (actually HalRequestSoftwareInterrupt) */
static inline void __stdcall kstub_HalReturnToFirmware(unsigned long Routine)
{
    KSTUB_LOG("HalReturnToFirmware(%u) - exiting", Routine);
    ExitProcess(0);
}

/* Ordinal 97: KeBugCheck */
static inline void __stdcall kstub_KeBugCheck(unsigned long BugCheckCode)
{
    KSTUB_LOG("*** KeBugCheck(0x%08X) ***", BugCheckCode);
    ExitProcess(BugCheckCode);
}

/* Ordinal 142: KeSaveFloatingPointState */
static inline NTSTATUS __stdcall kstub_KeSaveFloatingPointState(void* State)
{
    /* Unnecessary on PC - each thread has its own FPU context */
    return STATUS_SUCCESS;
}

/* Ordinal 139: KeRestoreFloatingPointState */
static inline NTSTATUS __stdcall kstub_KeRestoreFloatingPointState(void* State)
{
    return STATUS_SUCCESS;
}

/* ================================================================
 * Runtime Library Stubs
 * ================================================================ */

/* Xbox ANSI_STRING structure (used by Rtl string functions) */
typedef struct {
    unsigned short Length;
    unsigned short MaximumLength;
    char*          Buffer;
} XBOX_ANSI_STRING;

/* Ordinal 289: RtlInitAnsiString */
static inline void __stdcall kstub_RtlInitAnsiString(
    XBOX_ANSI_STRING* Dest, const char* Src)
{
    if (Src) {
        unsigned short len = (unsigned short)strlen(Src);
        Dest->Length = len;
        Dest->MaximumLength = len + 1;
        Dest->Buffer = (char*)Src;
    } else {
        Dest->Length = 0;
        Dest->MaximumLength = 0;
        Dest->Buffer = NULL;
    }
}

/* Ordinal 301: RtlNtStatusToDosError */
static inline unsigned long __stdcall kstub_RtlNtStatusToDosError(NTSTATUS Status)
{
    /* Simplified mapping */
    if (Status == STATUS_SUCCESS) return 0;
    return (unsigned long)Status;  /* Pass through */
}

/* ================================================================
 * Display / AV Stubs (all no-ops - display handled by D3D layer)
 * ================================================================ */

/* Ordinal 1: AvGetSavedDataAddress */
static inline unsigned long __stdcall kstub_AvGetSavedDataAddress(void) { return 0; }

/* Ordinal 2: AvSendTVEncoderOption */
static inline void __stdcall kstub_AvSendTVEncoderOption(
    void* RegisterBase, unsigned long Option, unsigned long Param,
    unsigned long* Result)
{
    if (Result) *Result = 0;
}

/* Ordinal 3: AvSetDisplayMode */
static inline void __stdcall kstub_AvSetDisplayMode(
    void* RegisterBase, unsigned long Step, unsigned long Mode,
    unsigned long Format, unsigned long Pitch, unsigned long FrameBuffer)
{
    KSTUB_LOG("AvSetDisplayMode(step=%u, mode=%u) [no-op]", Step, Mode);
}

/* Ordinal 4: AvSetSavedDataAddress */
static inline void __stdcall kstub_AvSetSavedDataAddress(unsigned long Address) { }

/* ================================================================
 * Object Manager Stubs
 * ================================================================ */

/* Ordinal 250: ObfDereferenceObject (__fastcall) */
static inline void __fastcall kstub_ObfDereferenceObject(void* Object)
{
    /* Reference counting is a no-op for most recompilation targets */
}

/* Ordinal 246: ObReferenceObjectByHandle */
static inline NTSTATUS __stdcall kstub_ObReferenceObjectByHandle(
    HANDLE Handle, void* ObjectType, void** Object)
{
    /* Simple pass-through: treat handle as object pointer */
    *Object = (void*)Handle;
    return STATUS_SUCCESS;
}

/* ================================================================
 * Debug Stubs
 * ================================================================ */

/* Ordinal 7: DbgPrint (__cdecl, variadic) */
/* Note: This needs special handling in the thunk table because it's
 * __cdecl (caller cleans stack) while most kernel functions are __stdcall.
 * For the recompiled code, the argument count is baked into the caller. */
static inline unsigned long __cdecl kstub_DbgPrint(const char* Format, ...)
{
    va_list args;
    char buf[512];
    va_start(args, Format);
    vsnprintf(buf, sizeof(buf), Format, args);
    va_end(args);
    KSTUB_LOG("DbgPrint: %s", buf);
    return 0; /* STATUS_SUCCESS */
}

/* Ordinal 5: Exported data - KdDebuggerNotPresent (BOOLEAN)
 * TRUE on retail, FALSE when kernel debugger is attached.
 * Game code checks this to skip debug output. */
/* static BOOLEAN kstub_KdDebuggerNotPresent = TRUE; */

/* Ordinal 128: KeQueryInterruptTime
 * Returns monotonic time in 100-nanosecond units.
 * On Xbox, reads from kernel shared data page updated at each timer interrupt. */
static inline unsigned long long __stdcall kstub_KeQueryInterruptTime(void)
{
    /* GetTickCount64() returns milliseconds; convert to 100ns units */
    return (unsigned long long)GetTickCount64() * 10000ULL;
}

/* Ordinal 302: NtResumeThread
 * Resumes a previously suspended thread. Returns previous suspend count. */
static inline NTSTATUS __stdcall kstub_NtResumeThread(
    HANDLE ThreadHandle, unsigned long* PreviousSuspendCount)
{
    DWORD prev = ResumeThread(ThreadHandle);
    if (prev == (DWORD)-1) {
        KSTUB_LOG("NtResumeThread: ResumeThread failed (error %u)", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }
    if (PreviousSuspendCount)
        *PreviousSuspendCount = prev;
    return STATUS_SUCCESS;
}

/* Ordinal 304: NtSuspendThread
 * Suspends a thread. Returns previous suspend count. */
static inline NTSTATUS __stdcall kstub_NtSuspendThread(
    HANDLE ThreadHandle, unsigned long* PreviousSuspendCount)
{
    DWORD prev = SuspendThread(ThreadHandle);
    if (prev == (DWORD)-1) {
        KSTUB_LOG("NtSuspendThread: SuspendThread failed (error %u)", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }
    if (PreviousSuspendCount)
        *PreviousSuspendCount = prev;
    return STATUS_SUCCESS;
}

/* ================================================================
 * Port I/O Stubs (no-ops - no direct hardware access on PC)
 * ================================================================ */

/* Ordinal 335: WRITE_PORT_BUFFER_USHORT */
static inline void __stdcall kstub_WRITE_PORT_BUFFER_USHORT(
    unsigned short* Port, unsigned short* Buffer, unsigned long Count) { }

/* Ordinal 336: WRITE_PORT_BUFFER_ULONG */
static inline void __stdcall kstub_WRITE_PORT_BUFFER_ULONG(
    unsigned long* Port, unsigned long* Buffer, unsigned long Count) { }

/* ================================================================
 * Network Stubs
 * ================================================================ */

/* Ordinal 252: PhyGetLinkState */
static inline unsigned long __stdcall kstub_PhyGetLinkState(int Verify)
{
    return 0;  /* Link down - no network */
}

/* Ordinal 253: PhyInitialize */
static inline NTSTATUS __stdcall kstub_PhyInitialize(int ForceReset, void* Param2)
{
    return STATUS_SUCCESS;
}

/* ================================================================
 * XBE Section Loading Stubs
 * ================================================================ */

/* XeLoadSection / XeUnloadSection */
static inline NTSTATUS __stdcall kstub_XeLoadSection(void* SectionHeader)
{
    KSTUB_LOG("XeLoadSection(%p) [STUB]", SectionHeader);
    return STATUS_SUCCESS;
}

static inline NTSTATUS __stdcall kstub_XeUnloadSection(void* SectionHeader)
{
    KSTUB_LOG("XeUnloadSection(%p) [STUB]", SectionHeader);
    return STATUS_SUCCESS;
}

#ifdef __cplusplus
}
#endif

#endif /* XBOX_KERNEL_STUBS_H */
