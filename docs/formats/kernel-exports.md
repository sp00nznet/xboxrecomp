# Xbox Kernel Exports

The Xbox kernel (`xboxkrnl.exe`) exports up to 366 functions and data objects. Games import a subset of these via the kernel thunk table in the XBE header. This document covers the most commonly imported exports (~147 that a typical game uses), organized by category.

Each entry shows: **ordinal**, function prototype, description, and suggested Win32 replacement.

## Memory Management

### Contiguous Memory (GPU-accessible)

| Ord | Function | Description |
|-----|----------|-------------|
| 165 | `PVOID MmAllocateContiguousMemory(ULONG NumberOfBytes)` | Allocate physically contiguous memory. Used for GPU-accessible buffers (vertex buffers, textures, push buffers). |
| 166 | `PVOID MmAllocateContiguousMemoryEx(ULONG NumberOfBytes, ULONG_PTR LowestAddr, ULONG_PTR HighestAddr, ULONG Alignment, ULONG Protect)` | Extended contiguous allocation with address range and alignment constraints. |
| 171 | `VOID MmFreeContiguousMemory(PVOID BaseAddress)` | Free contiguous memory allocated by ordinals 165/166. |
| 178 | `VOID MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)` | Mark memory as persistent across quick reboot. |

**Win32 replacement**: `VirtualAlloc(NULL, size, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE)` or `_aligned_malloc()` for alignment. Physical contiguity is irrelevant on PC since GPU resources are managed by D3D11/D3D12.

### Virtual Memory

| Ord | Function | Description |
|-----|----------|-------------|
| 184 | `NTSTATUS NtAllocateVirtualMemory(PVOID* BaseAddress, ULONG_PTR ZeroBits, PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect)` | Allocate or reserve virtual memory pages. |
| 199 | `NTSTATUS NtFreeVirtualMemory(PVOID* BaseAddress, PSIZE_T RegionSize, ULONG FreeType)` | Free virtual memory. |
| 217 | `NTSTATUS NtQueryVirtualMemory(PVOID BaseAddress, PVOID MemoryInformation, ULONG Length, PULONG ReturnLength)` | Query virtual memory region info. |

**Win32 replacement**: Direct mapping to `VirtualAlloc`, `VirtualFree`, `VirtualQuery`. The API is nearly identical.

### System/Pool Memory

| Ord | Function | Description |
|-----|----------|-------------|
| 15 | `PVOID ExAllocatePool(ULONG NumberOfBytes)` | Allocate from kernel pool (NonPagedPool). |
| 16 | `PVOID ExAllocatePoolWithTag(ULONG NumberOfBytes, ULONG Tag)` | Pool allocation with debug tag. |
| 24 | `ULONG ExQueryPoolBlockSize(PVOID PoolBlock)` | Query size of a pool allocation. |
| 167 | `PVOID MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect)` | Map physical address to virtual. Used for NV2A GPU register access. |
| 173 | `ULONG_PTR MmGetPhysicalAddress(PVOID BaseAddress)` | Get physical address of virtual address. |
| 181 | `NTSTATUS MmQueryStatistics(PMM_STATISTICS MemoryStatistics)` | Get memory usage statistics. |
| 180 | `ULONG MmQueryAllocationSize(PVOID BaseAddress)` | Query allocation size. |
| 179 | `ULONG MmQueryAddressProtect(PVOID VirtualAddress)` | Query page protection. |
| 182 | `VOID MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect)` | Change page protection. |
| 168 | `PVOID MmClaimGpuInstanceMemory(ULONG NumberOfBytes, PULONG NumberOfPaddingBytes)` | Claim GPU instance memory from the top of RAM. |
| 169 | `PVOID MmCreateKernelStack(ULONG NumberOfBytes, BOOLEAN DebuggerThread)` | Create a kernel thread stack. |
| 170 | `VOID MmDeleteKernelStack(PVOID StackBase, PVOID StackLimit)` | Delete a kernel thread stack. |
| 175 | `VOID MmLockUnlockBufferPages(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN UnlockPages)` | Lock/unlock physical pages. |
| 176 | `VOID MmLockUnlockPhysicalPage(ULONG_PTR PhysicalAddress, BOOLEAN UnlockPage)` | Lock/unlock a single physical page. |

**Win32 replacement**: `HeapAlloc`/`HeapFree` for pool, `VirtualAlloc` for system memory, `VirtualProtect` for protection changes. `MmMapIoSpace` for NV2A registers should return a stub buffer that absorbs GPU register writes.

## File I/O

| Ord | Function | Description |
|-----|----------|-------------|
| 190 | `NTSTATUS NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions)` | Open or create a file. Primary file open API. |
| 202 | `NTSTATUS NtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, ULONG ShareAccess, ULONG OpenOptions)` | Open an existing file (simplified NtCreateFile). |
| 219 | `NTSTATUS NtReadFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset)` | Read data from a file. |
| 236 | `NTSTATUS NtWriteFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset)` | Write data to a file. |
| 187 | `NTSTATUS NtClose(HANDLE Handle)` | Close any kernel handle (files, events, semaphores, threads). |
| 195 | `NTSTATUS NtDeleteFile(POBJECT_ATTRIBUTES ObjectAttributes)` | Delete a file by name. |
| 211 | `NTSTATUS NtQueryInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length, FILE_INFORMATION_CLASS FileInformationClass)` | Query file metadata (size, timestamps, attributes). |
| 226 | `NTSTATUS NtSetInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length, FILE_INFORMATION_CLASS FileInformationClass)` | Set file metadata (position, size, disposition). |
| 210 | `NTSTATUS NtQueryFullAttributesFile(POBJECT_ATTRIBUTES ObjectAttributes, PFILE_NETWORK_OPEN_INFORMATION FileInformation)` | Query file attributes without opening. |
| 207 | `NTSTATUS NtQueryDirectoryFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length, PANSI_STRING FileName, BOOLEAN RestartScan)` | Enumerate directory contents. |
| 218 | `NTSTATUS NtQueryVolumeInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FsInformation, ULONG Length, FS_INFORMATION_CLASS FsInformationClass)` | Query volume/filesystem info (free space, etc.). |
| 198 | `NTSTATUS NtFlushBuffersFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock)` | Flush file buffers to disk. |
| 200 | `NTSTATUS NtFsControlFile(...)` | File system control operations. |
| 196 | `NTSTATUS NtDeviceIoControlFile(...)` | Device I/O control. |

**Win32 replacement**: Xbox uses NT-style paths (`\Device\CdRom0\`, `\Device\Harddisk0\Partition1\`). Implement a path translation layer that maps:
- `D:\` (DVD drive) to your game data directory
- `T:\` (title persistent storage) to a save directory
- `U:\` (user data) to a user save directory
- `Z:\` (utility partition) to a cache directory

Then use `CreateFileW`, `ReadFile`, `WriteFile`, `CloseHandle`, etc. Key difference: Xbox uses ANSI_STRING for paths (not UNICODE_STRING like Windows NT).

### I/O Manager

| Ord | Function | Description |
|-----|----------|-------------|
| 67 | `NTSTATUS IoCreateFile(...)` | Extended file creation (internal). |
| 62 | `NTSTATUS IoBuildDeviceIoControlRequest(...)` | Build an IRP for device I/O. |
| 86 | `NTSTATUS IoSynchronousDeviceIoControlRequest(...)` | Synchronous device I/O. |
| 87 | `NTSTATUS IoSynchronousFsdRequest(...)` | Synchronous FSD request. |
| 203 | `NTSTATUS NtOpenSymbolicLinkObject(PHANDLE LinkHandle, POBJECT_ATTRIBUTES ObjectAttributes)` | Open a symbolic link. |
| 215 | `NTSTATUS NtQuerySymbolicLinkObject(HANDLE LinkHandle, PANSI_STRING LinkTarget, PULONG ReturnedLength)` | Read symbolic link target. Games use this to resolve drive letters. |

**Win32 replacement**: Most I/O manager functions can be stubbed. Symbolic link queries should return the mapped paths for Xbox drive letters.

## Threading

| Ord | Function | Description |
|-----|----------|-------------|
| 255 | `NTSTATUS PsCreateSystemThreadEx(PHANDLE ThreadHandle, ULONG ThreadExtraSize, ULONG KernelStackSize, ULONG TlsDataSize, PULONG ThreadId, PVOID StartContext1, PVOID StartContext2, BOOLEAN CreateSuspended, BOOLEAN DebugStack, PSYSTEM_ROUTINE StartRoutine)` | Create a kernel thread. Primary thread creation API. |
| 258 | `NTSTATUS PsTerminateSystemThread(NTSTATUS ExitStatus)` | Terminate the current thread. |
| 256 | `NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval)` | Sleep the current thread (negative = relative time in 100ns units). |
| 143 | `LONG KeSetBasePriorityThread(PVOID Thread, LONG Increment)` | Set thread base priority. |
| 124 | `LONG KeQueryBasePriorityThread(PVOID Thread)` | Get thread base priority. |
| 238 | `NTSTATUS NtYieldExecution(void)` | Yield CPU to another thread. |
| 197 | `NTSTATUS NtDuplicateObject(HANDLE SourceHandle, PHANDLE TargetHandle, ULONG Options)` | Duplicate a kernel handle. |

**Win32 replacement**: `CreateThread` (note Xbox uses `__stdcall` for thread procs), `ExitThread`, `Sleep`, `SetThreadPriority`, `SwitchToThread`.

## Synchronization

### Events and Semaphores

| Ord | Function | Description |
|-----|----------|-------------|
| 189 | `NTSTATUS NtCreateEvent(PHANDLE EventHandle, POBJECT_ATTRIBUTES ObjectAttributes, ULONG EventType, BOOLEAN InitialState)` | Create a named or unnamed event. |
| 225 | `NTSTATUS NtSetEvent(HANDLE EventHandle, PLONG PreviousState)` | Signal an event. |
| 193 | `NTSTATUS NtCreateSemaphore(PHANDLE SemaphoreHandle, POBJECT_ATTRIBUTES ObjectAttributes, LONG InitialCount, LONG MaximumCount)` | Create a semaphore. |
| 222 | `NTSTATUS NtReleaseSemaphore(HANDLE SemaphoreHandle, LONG ReleaseCount, PLONG PreviousCount)` | Release a semaphore. |
| 234 | `NTSTATUS NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout)` | Wait on a single sync object. |
| 233 | `NTSTATUS NtWaitForMultipleObjectsEx(ULONG Count, HANDLE Handles[], ULONG WaitType, BOOLEAN Alertable, PLARGE_INTEGER Timeout)` | Wait on multiple sync objects. |

**Win32 replacement**: `CreateEvent`, `SetEvent`, `CreateSemaphore`, `ReleaseSemaphore`, `WaitForSingleObject`, `WaitForMultipleObjects`. Nearly 1:1 mapping.

### Kernel-mode Sync (Dispatcher Objects)

| Ord | Function | Description |
|-----|----------|-------------|
| 145 | `LONG KeSetEvent(PVOID Event, LONG Increment, BOOLEAN Wait)` | Kernel-mode set event. |
| 159 | `NTSTATUS KeWaitForSingleObject(PVOID Object, ULONG WaitReason, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout)` | Kernel-mode wait. |
| 158 | `NTSTATUS KeWaitForMultipleObjects(ULONG Count, PVOID Objects[], ULONG WaitType, ULONG WaitReason, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout, PVOID WaitBlockArray)` | Kernel-mode multi-wait. |

**Win32 replacement**: Same as Nt* variants above. The kernel-mode API takes object pointers instead of handles; for recompilation purposes, wrap Win32 events and forward.

### Critical Sections

| Ord | Function | Description |
|-----|----------|-------------|
| 291 | `VOID RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)` | Initialize a critical section. |
| 277 | `VOID RtlEnterCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)` | Enter (lock) a critical section. |
| 294 | `VOID RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)` | Leave (unlock) a critical section. |

**Win32 replacement**: Direct 1:1 mapping to `InitializeCriticalSection`, `EnterCriticalSection`, `LeaveCriticalSection`. The structure layout is compatible.

### Timers and DPCs

| Ord | Function | Description |
|-----|----------|-------------|
| 113 | `VOID KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type)` | Initialize a kernel timer. |
| 149 | `BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)` | Set a one-shot timer. |
| 150 | `BOOLEAN KeSetTimerEx(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period, PKDPC Dpc)` | Set a periodic timer. |
| 99 | `BOOLEAN KeCancelTimer(PKTIMER Timer)` | Cancel a timer. |
| 107 | `VOID KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine, PVOID DeferredContext)` | Initialize a Deferred Procedure Call. |
| 119 | `BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID Arg1, PVOID Arg2)` | Queue a DPC for execution. |
| 137 | `BOOLEAN KeRemoveQueueDpc(PKDPC Dpc)` | Remove a queued DPC. |

**Win32 replacement**: Use `CreateTimerQueueTimer` / `DeleteTimerQueueTimer` for timers. DPCs can be implemented as thread pool work items or direct callbacks.

## HAL & System

| Ord | Function | Description |
|-----|----------|-------------|
| 49 | `VOID HalRequestSoftwareInterrupt(KIRQL RequestIrql)` | Request a software interrupt. |
| 40 | `VOID HalClearSoftwareInterrupt(KIRQL RequestIrql)` | Clear a software interrupt. |
| 41 | `VOID HalDisableSystemInterrupt(ULONG BusInterruptLevel, KIRQL Irql)` | Disable a hardware interrupt. |
| 44 | `ULONG HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql)` | Get interrupt vector for a bus IRQ. |
| 46 | `ULONG HalReadSMCTrayState(PULONG TrayState, PULONG ChangeCount)` | Read DVD tray state from SMC. |
| 47 | `VOID HalReadWritePCISpace(ULONG BusNumber, ULONG SlotNumber, ULONG RegisterNumber, PVOID Buffer, ULONG Length, BOOLEAN Write)` | Read/write PCI configuration space. |
| 126 | `LARGE_INTEGER KeQueryPerformanceCounter(void)` | High-resolution timer (same as QPC). |
| 127 | `LARGE_INTEGER KeQueryPerformanceFrequency(void)` | Performance counter frequency. |
| 128 | `VOID KeQuerySystemTime(PLARGE_INTEGER CurrentTime)` | Get current system time (100ns since 1601). |
| 151 | `VOID KeStallExecutionProcessor(ULONG MicroSeconds)` | Busy-wait for N microseconds. |
| 160 | `KIRQL KfRaiseIrql(KIRQL NewIrql)` | Raise IRQL (interrupt priority). |
| 161 | `VOID KfLowerIrql(KIRQL NewIrql)` | Lower IRQL. |
| 129 | `KIRQL KeRaiseIrqlToDpcLevel(void)` | Raise to DISPATCH_LEVEL. |
| 142 | `NTSTATUS KeSaveFloatingPointState(PVOID State)` | Save FPU/SSE state. |
| 139 | `NTSTATUS KeRestoreFloatingPointState(PVOID State)` | Restore FPU/SSE state. |
| 97 | `VOID KeBugCheck(ULONG BugCheckCode)` | Trigger kernel bugcheck (BSOD). |
| 98 | `VOID KeBugCheckEx(ULONG BugCheckCode, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4)` | Extended bugcheck with parameters. |
| 358 | `BOOLEAN HalIsResetOrShutdownPending(void)` | Check if reset/shutdown requested. |
| 360 | `VOID HalInitiateShutdown(void)` | Initiate system shutdown. |

**Win32 replacement**: `QueryPerformanceCounter`, `QueryPerformanceFrequency`, `GetSystemTimeAsFileTime`. IRQL functions should be no-ops (PC has no IRQL concept). `KeBugCheck` should log and `exit(1)`. FPU state save/restore is unnecessary on PC (each thread has its own FPU context).

### Data Exports

| Ord | Symbol | Description |
|-----|--------|-------------|
| 156 | `KeTickCount` | System tick count (increments every ~1ms). |
| 322 | `XboxHardwareInfo` | Hardware flags and revision info. |
| 324 | `XboxKrnlVersion` | Kernel version (major.minor.build.qfe). |
| 164 | `LaunchDataPage` | Pointer to launch data page (title ID, launch path). |
| 259 | `PsThreadObjectType` | Thread object type pointer. |
| 17 | `ExEventObjectType` | Event object type pointer. |
| 328 | `XeImageFileName` | ANSI_STRING with XBE filename. |
| 323 | `XboxHDKey` | 16-byte hard drive key. |
| 325 | `XboxSignatureKey` | 16-byte title signature key. |
| 326 | `XboxLANKey` | 16-byte LAN encryption key. |
| 327 | `XboxAlternateSignatureKeys` | 16x16-byte alternate signature keys. |
| 357 | `XePublicKeyData` | 284-byte RSA public key data. |
| 65 | `IoCompletionObjectType` | I/O completion object type. |
| 71 | `IoDeviceObjectType` | I/O device object type. |

**Win32 replacement**: Allocate static structures with plausible values. `KeTickCount` should be updated from `GetTickCount()`. Hardware info flags = 0x20000000 (no HDD, has DVD). Kernel version = 1.0.5849.1 (or match your target XDK).

## Runtime Library (Rtl*)

| Ord | Function | Description |
|-----|----------|-------------|
| 289 | `VOID RtlInitAnsiString(PANSI_STRING Dest, const char* Src)` | Initialize ANSI_STRING from C string. |
| 260 | `NTSTATUS RtlAnsiStringToUnicodeString(PUNICODE_STRING Dest, PANSI_STRING Src, BOOLEAN Alloc)` | Convert ANSI to Unicode string. |
| 308 | `NTSTATUS RtlUnicodeStringToAnsiString(PANSI_STRING Dest, PUNICODE_STRING Src, BOOLEAN Alloc)` | Convert Unicode to ANSI string. |
| 279 | `BOOLEAN RtlEqualString(PANSI_STRING S1, PANSI_STRING S2, BOOLEAN CaseInsensitive)` | Compare two ANSI strings. |
| 269 | `ULONG RtlCompareMemoryUlong(PVOID Source, ULONG Length, ULONG Pattern)` | Scan memory for ULONG pattern. |
| 301 | `ULONG RtlNtStatusToDosError(NTSTATUS Status)` | Convert NTSTATUS to Win32 error code. |
| 304 | `BOOLEAN RtlTimeFieldsToTime(PTIME_FIELDS Fields, PLARGE_INTEGER Time)` | Convert time fields to NT time. |
| 305 | `VOID RtlTimeToTimeFields(PLARGE_INTEGER Time, PTIME_FIELDS Fields)` | Convert NT time to time fields. |
| 312 | `VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PVOID ExceptionRecord, PVOID ReturnValue)` | SEH stack unwinding. |
| 302 | `VOID RtlRaiseException(PVOID ExceptionRecord)` | Raise an exception. |
| 354 | `VOID RtlRip(PCHAR ApiName, PCHAR Expression, PCHAR Message)` | Debug assertion failure (debug builds). |

**Win32 replacement**: Most Rtl functions have direct Win32 equivalents or can be reimplemented trivially. String functions use ANSI_STRING (Length + MaximumLength + Buffer). SEH unwinding (`RtlUnwind`) is complex but rarely exercised in normal gameplay.

## Object Manager

| Ord | Function | Description |
|-----|----------|-------------|
| 246 | `NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, PVOID ObjectType, PVOID* Object)` | Get object pointer from handle. |
| 247 | `NTSTATUS ObReferenceObjectByName(PANSI_STRING Name, ULONG Attributes, PVOID ObjectType, PVOID ParseContext, PVOID* Object)` | Get object by name lookup. |
| 250 | `VOID ObfDereferenceObject(PVOID Object)` | Decrement object reference count. |

**Win32 replacement**: Reference counting can be no-ops for most games. Handle-to-object resolution needs a simple mapping table.

## XBE Section Loading

| Ord | Function | Description |
|-----|----------|-------------|
| (varies) | `NTSTATUS XeLoadSection(PXBE_SECTION_HEADER Section)` | Demand-load an XBE section. |
| (varies) | `NTSTATUS XeUnloadSection(PXBE_SECTION_HEADER Section)` | Unload a demand-loaded section. |

**Win32 replacement**: Read the section data from the XBE file and copy to the section's VA. Increment/decrement the reference count.

## Crypto

| Ord | Function | Description |
|-----|----------|-------------|
| 337 | `VOID XcSHAInit(PSHA_CONTEXT Context)` | Initialize SHA-1 context. |
| 338 | `VOID XcSHAUpdate(PSHA_CONTEXT Context, const UCHAR* Input, ULONG Length)` | Update SHA-1 hash. |
| 339 | `VOID XcSHAFinal(PSHA_CONTEXT Context, UCHAR* Digest)` | Finalize SHA-1 hash. |
| 340 | `VOID XcRC4Key(PRC4_CONTEXT Context, ULONG KeyLength, const UCHAR* Key)` | Initialize RC4 key schedule. |

**Win32 replacement**: Use Windows CNG (`BCryptCreateHash` etc.) or a lightweight SHA-1/RC4 implementation. Most games only use crypto for save game signing and Xbox Live authentication, both of which can be stubbed.

## Debug

| Ord | Function | Description |
|-----|----------|-------------|
| 7 | `VOID DbgPrint(const char* Format, ...)` | Debug printf (debug builds only). |

**Win32 replacement**: `OutputDebugStringA` or `fprintf(stderr, ...)`.

## Network (Xbox Live)

| Ord | Function | Description |
|-----|----------|-------------|
| 252 | `ULONG PhyGetLinkState(BOOLEAN Verify)` | Get Ethernet link state. |
| 253 | `NTSTATUS PhyInitialize(BOOLEAN ForceReset, PVOID Param2)` | Initialize network PHY. |

**Win32 replacement**: Return "link down" (0) for offline play. Full Xbox Live replacement requires significant networking infrastructure.

## Display / AV

| Ord | Function | Description |
|-----|----------|-------------|
| 1 | `ULONG AvGetSavedDataAddress(void)` | Get saved display configuration. |
| 2 | `VOID AvSendTVEncoderOption(PVOID RegisterBase, ULONG Option, ULONG Param, PULONG Result)` | Configure TV encoder (resolution, format). |
| 3 | `VOID AvSetDisplayMode(PVOID RegisterBase, ULONG Step, ULONG Mode, ULONG Format, ULONG Pitch, ULONG FrameBuffer)` | Set display mode. |
| 4 | `VOID AvSetSavedDataAddress(ULONG Address)` | Save display configuration. |

**Win32 replacement**: All display/AV functions should be no-ops. Display mode is handled by your D3D backend (D3D11 or D3D12 window creation).

## Port I/O

| Ord | Function | Description |
|-----|----------|-------------|
| 335 | `VOID WRITE_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count)` | Write to I/O port (16-bit). |
| 336 | `VOID WRITE_PORT_BUFFER_ULONG(PULONG Port, PULONG Buffer, ULONG Count)` | Write to I/O port (32-bit). |

**Win32 replacement**: No-ops. Port I/O is for direct hardware access (SMBus, GPU, etc.) which is irrelevant on PC.
