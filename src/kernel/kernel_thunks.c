/*
 * kernel_thunks.c - Xbox Kernel Thunk Table & Initialization
 *
 * Wires the kernel thunk table (an array whose address/size come from the
 * loaded XBE, not a hardcoded title) to our xbox_* function/data
 * implementations.
 *
 * The Xbox kernel thunk table is an array of function/data pointers that
 * game code calls through via indirect calls: call [thunk_addr].
 * Each entry corresponds to a kernel export ordinal.
 *
 * This file provides:
 *   - xbox_kernel_thunk_table[] - the pointer slots
 *   - xbox_resolve_ordinal() - maps ordinal → function/data pointer
 *   - xbox_kernel_init() - fills the thunk table and initializes subsystems
 *   - xbox_kernel_shutdown() - cleanup
 *   - xbox_log() - logging implementation
 */

#include "kernel.h"
#include "recomp_env.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>

extern ptrdiff_t g_xbox_mem_offset;

/* ============================================================================
 * Thunk Table Storage
 * ============================================================================ */

ULONG_PTR xbox_kernel_thunk_table[XBOX_KERNEL_THUNK_TABLE_SIZE] = {0};

/* ============================================================================
 * Logging Implementation
 * ============================================================================ */

static FILE* g_log_file = NULL;
static int   g_log_level = XBOX_LOG_INFO;
static CRITICAL_SECTION g_log_cs;
static BOOL  g_log_cs_init = FALSE;

static const char* xbox_log_level_str(int level)
{
    switch (level) {
        case XBOX_LOG_ERROR: return "ERROR";
        case XBOX_LOG_WARN:  return "WARN ";
        case XBOX_LOG_INFO:  return "INFO ";
        case XBOX_LOG_DEBUG: return "DEBUG";
        case XBOX_LOG_TRACE: return "TRACE";
        default:             return "?????";
    }
}

void xbox_log(int level, const char* subsystem, const char* fmt, ...)
{
    va_list args;
    char timestamp[32];
    SYSTEMTIME st;

    if (level > g_log_level)
        return;

    GetLocalTime(&st);
    snprintf(timestamp, sizeof(timestamp), "%02d:%02d:%02d.%03d",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    if (g_log_cs_init)
        EnterCriticalSection(&g_log_cs);

    FILE* out = g_log_file ? g_log_file : stderr;
    fprintf(out, "[%s] %s [%-6s] ", timestamp, xbox_log_level_str(level), subsystem);

    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fprintf(out, "\n");
    fflush(out);

    if (g_log_cs_init)
        LeaveCriticalSection(&g_log_cs);
}

/* ============================================================================
 * Ordinal Resolution
 *
 * Maps each Xbox kernel ordinal to our implementation address.
 * Returns 0 for unimplemented ordinals (logged as warnings).
 *
 * Ordinals follow the canonical Xbox kernel export table (see
 * tools/xbe_parser/xbe_parser.py, verified against xboxdevwiki.net/Kernel).
 * That table includes DATA exports -- ExEventObjectType (16),
 * HalDiskModelNumber (41), IoFileObjectType (71), KdDebuggerEnabled (88) and
 * others -- which occupy ordinals but are variables, not callable functions.
 * Numbering the callable exports sequentially and skipping those slots shifts
 * every later ordinal and silently dispatches the wrong function; that bug is
 * what this table was corrected from. When adding an ordinal, take it from the
 * export table, never by counting.
 *
 * Where no implementation exists the case is omitted entirely, so the default
 * arm logs it, rather than pointing at a neighbouring function.
 * ============================================================================ */

ULONG_PTR xbox_resolve_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* ---- Av ---- */
    case   1: return (ULONG_PTR)xbox_AvGetSavedDataAddress;
    case   2: return (ULONG_PTR)xbox_AvSendTVEncoderOption;
    case   3: return (ULONG_PTR)xbox_AvSetDisplayMode;
    case   4: return (ULONG_PTR)xbox_AvSetSavedDataAddress;

    /* ---- Debug ---- */
    case   5: return (ULONG_PTR)xbox_DbgBreakPoint;
    case   8: return (ULONG_PTR)xbox_Unknown_8;  /* DbgPrint */

    /* ---- Executive / pool ---- */
    case  14: return (ULONG_PTR)xbox_ExAllocatePool;
    case  15: return (ULONG_PTR)xbox_ExAllocatePoolWithTag;
    case  16: return (ULONG_PTR)&xbox_ExEventObjectType;
    case  17: return (ULONG_PTR)xbox_ExFreePool;
    case  23: return (ULONG_PTR)xbox_ExQueryPoolBlockSize;
    case  24: return (ULONG_PTR)xbox_ExQueryNonVolatileSetting;
    case  29: return (ULONG_PTR)xbox_ExSaveNonVolatileSetting;

    /* ---- HAL ---- */
    case   9: return (ULONG_PTR)xbox_HalReadSMCTrayState;
    case  38: return (ULONG_PTR)xbox_HalClearSoftwareInterrupt;
    case  39: return (ULONG_PTR)xbox_HalDisableSystemInterrupt;
    case  40: return (ULONG_PTR)&xbox_HalDiskCachePartitionCount;
    case  41: return (ULONG_PTR)&xbox_HalDiskModelNumber;
    case  42: return (ULONG_PTR)&xbox_HalDiskSerialNumber;
    case  44: return (ULONG_PTR)xbox_HalGetInterruptVector;
    case  45: return (ULONG_PTR)xbox_HalReadSMBusValue;
    case  46: return (ULONG_PTR)xbox_HalReadWritePCISpace;
    case  47: return (ULONG_PTR)xbox_HalRegisterShutdownNotification;
    case  48: return (ULONG_PTR)xbox_HalRequestSoftwareInterrupt;
    case  49: return (ULONG_PTR)xbox_HalReturnToFirmware;
    case  50: return (ULONG_PTR)xbox_HalWriteSMBusValue;
    case 356: return (ULONG_PTR)&xbox_HalBootSMCVideoMode;
    case 358: return (ULONG_PTR)xbox_HalIsResetOrShutdownPending;
    case 360: return (ULONG_PTR)xbox_HalInitiateShutdown;

    /* ---- I/O manager ---- */
    case  61: return (ULONG_PTR)xbox_IoBuildDeviceIoControlRequest;
    case  62: return (ULONG_PTR)xbox_IoBuildSynchronousFsdRequest;
    case  64: return (ULONG_PTR)&xbox_IoCompletionObjectType;
    case  65: return (ULONG_PTR)xbox_IoCreateDevice;
    case  66: return (ULONG_PTR)xbox_IoCreateFile;
    case  67: return (ULONG_PTR)xbox_IoCreateSymbolicLink;
    case  68: return (ULONG_PTR)xbox_IoDeleteDevice;
    case  69: return (ULONG_PTR)xbox_IoDeleteSymbolicLink;
    case  70: return (ULONG_PTR)&xbox_IoDeviceObjectType;
    case  71: return (ULONG_PTR)xbox_IoFileObjectType;
    case  73: return (ULONG_PTR)xbox_IoInitializeIrp;
    case  74: return (ULONG_PTR)xbox_IoInvalidDeviceRequest;
    case  79: return (ULONG_PTR)xbox_IoSetIoCompletion;
    case  81: return (ULONG_PTR)xbox_IoStartNextPacket;
    case  82: return (ULONG_PTR)xbox_IoStartNextPacketByKey;
    case  83: return (ULONG_PTR)xbox_IoStartPacket;
    case  84: return (ULONG_PTR)xbox_IoSynchronousDeviceIoControlRequest;
    case  85: return (ULONG_PTR)xbox_IoSynchronousFsdRequest;
    case  86: return (ULONG_PTR)xbox_IofCallDriver;
    case  87: return (ULONG_PTR)xbox_IofCompleteRequest;
    case 359: return (ULONG_PTR)xbox_IoMarkIrpMustComplete;

    /* ---- Kernel core ---- */
    case  93: return (ULONG_PTR)xbox_KeAlertThread;
    case  95: return (ULONG_PTR)xbox_KeBugCheck;
    case  96: return (ULONG_PTR)xbox_KeBugCheckEx;
    case  97: return (ULONG_PTR)xbox_KeCancelTimer;
    case  98: return (ULONG_PTR)xbox_KeConnectInterrupt;
    case  99: return (ULONG_PTR)xbox_KeDelayExecutionThread;
    case 100: return (ULONG_PTR)xbox_KeDisconnectInterrupt;
    case 107: return (ULONG_PTR)xbox_KeInitializeDpc;
    case 109: return (ULONG_PTR)xbox_KeInitializeInterrupt;
    case 113: return (ULONG_PTR)xbox_KeInitializeTimerEx;
    case 119: return (ULONG_PTR)xbox_KeInsertQueueDpc;
    case 124: return (ULONG_PTR)xbox_KeQueryBasePriorityThread;
    case 125: return (ULONG_PTR)xbox_KeQueryInterruptTime;
    case 126: return (ULONG_PTR)xbox_KeQueryPerformanceCounter;
    case 127: return (ULONG_PTR)xbox_KeQueryPerformanceFrequency;
    case 128: return (ULONG_PTR)xbox_KeQuerySystemTime;
    case 129: return (ULONG_PTR)xbox_KeRaiseIrqlToDpcLevel;
    case 137: return (ULONG_PTR)xbox_KeRemoveQueueDpc;
    case 139: return (ULONG_PTR)xbox_KeRestoreFloatingPointState;
    case 142: return (ULONG_PTR)xbox_KeSaveFloatingPointState;
    case 143: return (ULONG_PTR)xbox_KeSetBasePriorityThread;
    case 145: return (ULONG_PTR)xbox_KeSetEvent;
    case 149: return (ULONG_PTR)xbox_KeSetTimer;
    case 150: return (ULONG_PTR)xbox_KeSetTimerEx;
    case 151: return (ULONG_PTR)xbox_KeStallExecutionProcessor;
    case 153: return (ULONG_PTR)xbox_KeSynchronizeExecution;
    case 156: return (ULONG_PTR)&xbox_KeTickCount;
    case 158: return (ULONG_PTR)xbox_KeWaitForMultipleObjects;
    case 159: return (ULONG_PTR)xbox_KeWaitForSingleObject;
    case 160: return (ULONG_PTR)xbox_KfRaiseIrql;
    case 161: return (ULONG_PTR)xbox_KfLowerIrql;

    /* ---- Memory manager ---- */
    case 165: return (ULONG_PTR)xbox_MmAllocateContiguousMemory;
    case 166: return (ULONG_PTR)xbox_MmAllocateContiguousMemoryEx;
    case 167: return (ULONG_PTR)xbox_MmAllocateSystemMemory;
    case 168: return (ULONG_PTR)xbox_MmClaimGpuInstanceMemory;
    case 169: return (ULONG_PTR)xbox_MmCreateKernelStack;
    case 170: return (ULONG_PTR)xbox_MmDeleteKernelStack;
    case 171: return (ULONG_PTR)xbox_MmFreeContiguousMemory;
    case 172: return (ULONG_PTR)xbox_MmFreeSystemMemory;
    case 173: return (ULONG_PTR)xbox_MmGetPhysicalAddress;
    case 175: return (ULONG_PTR)xbox_MmLockUnlockBufferPages;
    case 176: return (ULONG_PTR)xbox_MmLockUnlockPhysicalPage;
    case 177: return (ULONG_PTR)xbox_MmMapIoSpace;
    case 178: return (ULONG_PTR)xbox_MmPersistContiguousMemory;
    case 179: return (ULONG_PTR)xbox_MmQueryAddressProtect;
    case 180: return (ULONG_PTR)xbox_MmQueryAllocationSize;
    case 181: return (ULONG_PTR)xbox_MmQueryStatistics;
    case 182: return (ULONG_PTR)xbox_MmSetAddressProtect;
    case 183: return (ULONG_PTR)xbox_MmUnmapIoSpace;

    /* ---- Native API ---- */
    case 184: return (ULONG_PTR)xbox_NtAllocateVirtualMemory;
    case 186: return (ULONG_PTR)xbox_NtClearEvent;
    case 187: return (ULONG_PTR)xbox_NtClose;
    case 189: return (ULONG_PTR)xbox_NtCreateEvent;
    case 190: return (ULONG_PTR)xbox_NtCreateFile;
    case 192: return (ULONG_PTR)xbox_NtCreateMutant;
    case 193: return (ULONG_PTR)xbox_NtCreateSemaphore;
    case 195: return (ULONG_PTR)xbox_NtDeleteFile;
    case 196: return (ULONG_PTR)xbox_NtDeviceIoControlFile;
    case 197: return (ULONG_PTR)xbox_NtDuplicateObject;
    case 198: return (ULONG_PTR)xbox_NtFlushBuffersFile;
    case 199: return (ULONG_PTR)xbox_NtFreeVirtualMemory;
    case 200: return (ULONG_PTR)xbox_NtFsControlFile;
    case 202: return (ULONG_PTR)xbox_NtOpenFile;
    case 203: return (ULONG_PTR)xbox_NtOpenSymbolicLinkObject;
    case 207: return (ULONG_PTR)xbox_NtQueryDirectoryFile;
    case 210: return (ULONG_PTR)xbox_NtQueryFullAttributesFile;
    case 211: return (ULONG_PTR)xbox_NtQueryInformationFile;
    case 215: return (ULONG_PTR)xbox_NtQuerySymbolicLinkObject;
    case 217: return (ULONG_PTR)xbox_NtQueryVirtualMemory;
    case 218: return (ULONG_PTR)xbox_NtQueryVolumeInformationFile;
    case 219: return (ULONG_PTR)xbox_NtReadFile;
    case 221: return (ULONG_PTR)xbox_NtReleaseMutant;
    case 222: return (ULONG_PTR)xbox_NtReleaseSemaphore;
    case 224: return (ULONG_PTR)xbox_NtResumeThread;
    case 225: return (ULONG_PTR)xbox_NtSetEvent;
    case 226: return (ULONG_PTR)xbox_NtSetInformationFile;
    case 228: return (ULONG_PTR)xbox_NtSetSystemTime;
    case 231: return (ULONG_PTR)xbox_NtSuspendThread;
    case 233: return (ULONG_PTR)xbox_NtWaitForSingleObject;
    case 234: return (ULONG_PTR)xbox_NtWaitForSingleObjectEx;
    case 235: return (ULONG_PTR)xbox_NtWaitForMultipleObjectsEx;
    case 236: return (ULONG_PTR)xbox_NtWriteFile;
    case 238: return (ULONG_PTR)xbox_NtYieldExecution;

    /* ---- Object manager ---- */
    case 246: return (ULONG_PTR)xbox_ObReferenceObjectByHandle;
    case 247: return (ULONG_PTR)xbox_ObReferenceObjectByName;
    case 250: return (ULONG_PTR)xbox_ObfDereferenceObject;
    case 251: return (ULONG_PTR)xbox_ObfReferenceObject;

    /* ---- Process / thread ---- */
    case 255: return (ULONG_PTR)xbox_PsCreateSystemThreadEx;
    case 258: return (ULONG_PTR)xbox_PsTerminateSystemThread;
    case 259: return (ULONG_PTR)&xbox_PsThreadObjectType;

    /* ---- Runtime library ---- */
    case 260: return (ULONG_PTR)xbox_RtlAnsiStringToUnicodeString;
    case 269: return (ULONG_PTR)xbox_RtlCompareMemoryUlong;
    case 277: return (ULONG_PTR)xbox_RtlEnterCriticalSection;
    case 279: return (ULONG_PTR)xbox_RtlEqualString;
    case 289: return (ULONG_PTR)xbox_RtlInitAnsiString;
    case 290: return (ULONG_PTR)xbox_RtlInitUnicodeString;
    case 291: return (ULONG_PTR)xbox_RtlInitializeCriticalSection;
    case 294: return (ULONG_PTR)xbox_RtlLeaveCriticalSection;
    case 301: return (ULONG_PTR)xbox_RtlNtStatusToDosError;
    case 302: return (ULONG_PTR)xbox_RtlRaiseException;
    case 304: return (ULONG_PTR)xbox_RtlTimeFieldsToTime;
    case 305: return (ULONG_PTR)xbox_RtlTimeToTimeFields;
    case 308: return (ULONG_PTR)xbox_RtlUnicodeStringToAnsiString;
    case 312: return (ULONG_PTR)xbox_RtlUnwind;
    case 352: return (ULONG_PTR)xbox_RtlRip;
    case 361: return (ULONG_PTR)xbox_RtlSnprintf;
    case 362: return (ULONG_PTR)xbox_RtlSprintf;
    case 363: return (ULONG_PTR)xbox_RtlVsnprintf;
    case 364: return (ULONG_PTR)xbox_RtlVsprintf;

    /* ---- Ports / misc HW ---- */
    case 252: return (ULONG_PTR)xbox_PhyGetLinkState;
    case 253: return (ULONG_PTR)xbox_PhyInitialize;
    case 333: return (ULONG_PTR)xbox_WRITE_PORT_BUFFER_USHORT;
    case 334: return (ULONG_PTR)xbox_WRITE_PORT_BUFFER_ULONG;
    case 357: return (ULONG_PTR)xbox_IdexChannelObject;

    /* ---- Crypto ---- */
    case 335: return (ULONG_PTR)xbox_XcSHAInit;
    case 336: return (ULONG_PTR)xbox_XcSHAUpdate;
    case 337: return (ULONG_PTR)xbox_XcSHAFinal;
    case 338: return (ULONG_PTR)xbox_XcRC4Key;
    case 339: return (ULONG_PTR)xbox_XcRC4Crypt;
    case 340: return (ULONG_PTR)xbox_XcHMAC;
    case 341: return (ULONG_PTR)xbox_XcPKEncPublic;
    case 342: return (ULONG_PTR)xbox_XcPKDecPrivate;
    case 343: return (ULONG_PTR)xbox_XcPKGetKeyLen;
    case 344: return (ULONG_PTR)xbox_XcVerifyPKCS1Signature;
    case 345: return (ULONG_PTR)xbox_XcModExp;
    case 346: return (ULONG_PTR)xbox_XcDESKeyParity;
    case 347: return (ULONG_PTR)xbox_XcKeyTable;
    case 348: return (ULONG_PTR)xbox_XcBlockCrypt;
    case 349: return (ULONG_PTR)xbox_XcBlockCryptCBC;
    case 350: return (ULONG_PTR)xbox_XcCryptService;
    case 351: return (ULONG_PTR)xbox_XcUpdateCrypto;

    /* ---- Loader / image ---- */
    case 326: return (ULONG_PTR)&xbox_XeImageFileName;
    case 327: return (ULONG_PTR)xbox_XeLoadSection;
    case 328: return (ULONG_PTR)xbox_XeUnloadSection;
    case 355: return (ULONG_PTR)xbox_XePublicKeyData;

    /* ---- Xbox globals ---- */
    case 321: return (ULONG_PTR)xbox_EEPROMKey;
    case 322: return (ULONG_PTR)&xbox_HardwareInfo;
    case 323: return (ULONG_PTR)xbox_HDKey;
    case 324: return (ULONG_PTR)&xbox_KrnlVersion;
    case 325: return (ULONG_PTR)xbox_SignatureKey;
    case 353: return (ULONG_PTR)xbox_LANKey;
    case 354: return (ULONG_PTR)xbox_AlternateSignatureKeys;

    /* ---- Unclassified ---- */
    case 164: return (ULONG_PTR)&xbox_LaunchDataPage;

    default:
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THUNK,
            "Unresolved kernel ordinal %u", ordinal);
        return 0;
    }
}

/* ============================================================================
 * Fallback Ordinal List
 *
 * Reference import set kept in thunk-table order, used only when the XBE has
 * not been loaded into Xbox memory (no memory layout, no parsed thunk table).
 * When it is loaded, xbox_kernel_init() reads the actual ordinals out of the
 * mapped thunk table, so this fallback's specific titles never matter at
 * runtime -- it exists to keep the table populated when there is no XBE.
 * ============================================================================ */

static const ULONG g_thunk_ordinals[XBOX_KERNEL_THUNK_TABLE_SIZE] = {
      1,   2,   3,   4,   8,  15,  16,  17,  23,  24,   /* 0-9   */
     40,  41,  42,  44,  46,  47,  49,  62,  65,  67,   /* 10-19 */
     69,  71,  74,  81,  83,  84,  85,  86,  87,  95,   /* 20-29 */
     97,  98,  99, 100, 107, 109, 113, 119, 124, 126,   /* 30-39 */
    127, 128, 129, 137, 139, 142, 143, 145, 149, 150,   /* 40-49 */
    151, 153, 156, 158, 159, 160, 161, 164, 165, 166,   /* 50-59 */
    168, 169, 170, 171, 173, 175, 176, 178, 179, 180,   /* 60-69 */
    181, 182, 184, 187, 189, 190, 193, 195, 196, 197,   /* 70-79 */
    198, 199, 200, 202, 203, 207, 210, 211, 215, 217,   /* 80-89 */
    218, 219, 222, 225, 226, 228, 233, 234, 236, 238,   /* 90-99 */
    246, 247, 250, 252, 253, 255, 256, 258, 259, 260,   /* 100-109 */
    269, 277, 279, 289, 291, 294, 301, 302, 304, 305,   /* 110-119 */
    308, 312, 322, 323, 324, 325, 326, 327, 328, 335,   /* 120-129 */
    336, 337, 338, 339, 340, 344, 345, 346, 347, 349,   /* 130-139 */
    353, 354, 355, 356, 357, 358, 359,                   /* 140-146 */
};

/* ============================================================================
 * Unresolved Thunk Handler
 *
 * Called if game code tries to use a thunk slot that wasn't resolved.
 * Logs the error and triggers a debug break.
 * ============================================================================ */

static void __stdcall xbox_unresolved_thunk(void)
{
    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THUNK,
        "Call to unresolved kernel thunk! Return address is on the stack.");
#ifdef _DEBUG
    DebugBreak();
#endif
}

/* ============================================================================
 * xbox_kernel_init - Initialize the kernel replacement layer
 *
 * Must be called before any game code runs. Sets up:
 *   1. Logging system
 *   2. Path translation
 *   3. Thunk table (all 147 entries)
 * ============================================================================ */

void xbox_kernel_init(void)
{
    ULONG resolved = 0;
    ULONG unresolved = 0;
    uint32_t thunk_base = 0;
    uint32_t thunk_count = 0;

    /* Initialize logging */
    InitializeCriticalSection(&g_log_cs);
    g_log_cs_init = TRUE;

    /* Try to open log file, fall back to stderr */
    g_log_file = fopen("xbox_kernel.log", "w");

    /* Set log level from environment variable if present */
    const char* log_env = recomp_env(RENV_LOG_LEVEL);
    if (log_env) {
        g_log_level = atoi(log_env);
        if (g_log_level < XBOX_LOG_ERROR) g_log_level = XBOX_LOG_ERROR;
        if (g_log_level > XBOX_LOG_TRACE) g_log_level = XBOX_LOG_TRACE;
    }

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_THUNK,
        "=== Xbox Kernel Replacement Layer initializing ===");
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_THUNK,
        "Kernel version: %u.%u.%u.%u (emulated)",
        xbox_KrnlVersion.Major, xbox_KrnlVersion.Minor,
        xbox_KrnlVersion.Build, xbox_KrnlVersion.Qfe);

#if !defined(_WIN32)
    /* Before the title opens anything: it may hold hundreds of files open
     * at once (see w32_raise_fd_limit). */
    {
        unsigned long long was = 0, now = 0;
        int rc = w32_raise_fd_limit(&was, &now);
        if (rc > 0)
            xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE,
                "open-file limit raised from %llu to %llu", was, now);
        else if (rc < 0)
            xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE,
                "open-file limit stays at %llu (could not raise it)", was);
    }
#endif

    /* Fill thunk table.
     *
     * The slot->ordinal mapping normally comes from the in-memory thunk table
     * of the loaded XBE (0x80000000|ordinal per entry, same source the kernel
     * bridge reads). That makes the table match any title's import order.
     * Without a mapped XBE, fall back to the static reference list below. */
    xbox_kernel_get_thunk_address(&thunk_base, &thunk_count);
    for (ULONG i = 0; i < XBOX_KERNEL_THUNK_TABLE_SIZE; i++) {
        ULONG ordinal;

        if (thunk_base && i < thunk_count && xbox_GetMemoryBase()) {
            uint32_t entry = *(volatile uint32_t *)
                ((uintptr_t)(thunk_base + i * 4) + g_xbox_mem_offset);
            if (entry == 0) {
                break;  /* rest of the table is empty */
            }
            ordinal = entry & 0x7FFFFFFF;
        } else {
            ordinal = g_thunk_ordinals[i];
        }

        ULONG_PTR ptr = xbox_resolve_ordinal(ordinal);

        if (ptr) {
            xbox_kernel_thunk_table[i] = ptr;
            resolved++;
        } else {
            /* Point unresolved thunks to our error handler */
            xbox_kernel_thunk_table[i] = (ULONG_PTR)xbox_unresolved_thunk;
            unresolved++;
        }
    }

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_THUNK,
        "Thunk table: %u/%u resolved, %u unresolved",
        resolved, XBOX_KERNEL_THUNK_TABLE_SIZE, unresolved);

    if (unresolved > 0) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_THUNK,
            "WARNING: %u kernel imports are unresolved - game may crash!", unresolved);
    }

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_THUNK,
        "=== Xbox Kernel Replacement Layer ready ===");
}

/* ============================================================================
 * xbox_kernel_shutdown - Clean up the kernel replacement layer
 * ============================================================================ */

void xbox_kernel_shutdown(void)
{
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_THUNK,
        "=== Xbox Kernel Replacement Layer shutting down ===");

    /* Close log file */
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }

    if (g_log_cs_init) {
        DeleteCriticalSection(&g_log_cs);
        g_log_cs_init = FALSE;
    }

    /* Zero thunk table */
    memset(xbox_kernel_thunk_table, 0, sizeof(xbox_kernel_thunk_table));
}
