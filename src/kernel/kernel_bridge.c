/**
 * kernel_bridge.c - Bridge between translated game code and kernel functions
 *
 * Problem:
 *   Translated game code calls kernel functions via indirect calls through
 *   the kernel thunk table at VA 0x0036B7C0. In the XBE file, these entries
 *   contain unresolved ordinals (0x80000000 | ordinal). On real Xbox hardware,
 *   the kernel loader replaces these with actual function pointers before the
 *   game runs.
 *
 * Solution:
 *   1. After xbox_MemoryLayoutInit copies .rdata, call xbox_kernel_bridge_init()
 *   2. Replace each ordinal entry in Xbox memory with a synthetic VA
 *   3. When RECOMP_ICALL encounters a synthetic VA, route it to a per-ordinal
 *      bridge function that reads args from the simulated Xbox stack, translates
 *      pointer arguments from Xbox VA→native, and calls the kernel function.
 *
 * Synthetic VA scheme:
 *   Each thunk slot i gets VA 0xFE000000 + i*4
 *   The lookup function checks this range and dispatches appropriately.
 *
 * Why per-ordinal bridges instead of a generic trampoline:
 *   Kernel functions receive Xbox pointers (32-bit VAs) that must be translated
 *   to native pointers by adding g_xbox_mem_offset. Different functions have
 *   different parameter layouts (pointer vs value), so each needs its own bridge.
 */

#include "kernel.h"
#include "xbox_memory_layout.h"
#include "recomp_icall_feedback.h"
#include <stdio.h>
/* stdlib.h is load-bearing, not tidiness. Without it C89 implicit declaration
 * makes malloc return `int`, so bridge_spawn_thread truncated its heap pointer
 * to 32 bits and sign-extended it into a `struct bridge_thread_start *`. Every
 * subsequent s->field wrote to an address that had nothing to do with the
 * allocation. MSVC says so (C4013 + C4047 "differs in levels of indirection")
 * but only as warnings, and this file is compiled with /W4 /WX-. */
#include <stdlib.h>
#include <float.h>
/* Section B string helpers: wcslen helper, case folding.
 * ctype.h/wctype.h are not pulled in by the platform headers on either host. */
#include <string.h>
#include <ctype.h>
#include <wctype.h>

/* Access to recompiled code registers. Per-thread: RECOMP_TLS comes from
 * xbox_memory_layout.h and must match the definitions there -- a plain extern
 * here binds to the TLS template rather than the calling thread's copy, which
 * reads as every register being zero. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;
extern RECOMP_TLS uint32_t g_seh_ebp;
extern ptrdiff_t g_xbox_mem_offset;

/* Dispatch table lookup (for function pointer args) */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
/* Timer DPCs run on a host thread, which needs a guest stack under it. */
int  xbox_worker_stack_alloc(void);

recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

/* Memory access - same as recomp_types.h MEM32 but without the #define guard */
#define BRIDGE_MEM32(addr) (*(volatile uint32_t *)((uintptr_t)(addr) + g_xbox_mem_offset))

/* Translate Xbox VA to native pointer (NULL-safe: 0 → NULL) */
#define XBOX_TO_NATIVE(va) ((va) ? (void*)((uintptr_t)(va) + g_xbox_mem_offset) : NULL)

/* ── Synthetic VA range (for function exports) ─────────── */

#define KERNEL_VA_BASE  0xFE000000u
#define KERNEL_VA_END   (KERNEL_VA_BASE + XBOX_KERNEL_THUNK_TABLE_SIZE * 4)

/* ── Kernel data exports ──────────────────────────────────
 *
 * Some kernel ordinals are DATA exports (structs/variables), not functions.
 * The game reads their thunk entries and dereferences the result to access
 * the data. These cannot use synthetic VAs — they must point to real,
 * dereferenceable addresses in the Xbox VA space.
 *
 * We allocate a "kernel data area" at XBOX_KERNEL_DATA_BASE and populate
 * it with the expected structures.
 */

#define BRIDGE_MEM16(addr) (*(volatile uint16_t *)((uintptr_t)(addr) + g_xbox_mem_offset))
#define BRIDGE_MEM8(addr)  (*(volatile uint8_t  *)((uintptr_t)(addr) + g_xbox_mem_offset))

/**
 * Get the Xbox VA of data for a kernel DATA export ordinal.
 * Returns 0 if the ordinal is not a data export (i.e., it's a function).
 */
static uint32_t kernel_data_va_for_ordinal(ULONG ordinal)
{
    /* Ordinals here are checked BEFORE function routing (see the thunk build
     * loop), so an ordinal listed by mistake turns a real kernel function into
     * a data address -- the title then calls it and jumps into kernel data.
     *
     * This table had a whole block shifted. 17 (ExFreePool), 65
     * (IoCreateDevice), 327 (XeLoadSection) and 328 (XeUnloadSection) are all
     * functions and were all being handed data addresses; Crimson Skies imports
     * every one of them. In the other direction, the genuine exports at 16, 353,
     * 354, 355, 356 and 357 got no thunk at all, so a title reading
     * XboxLANKey or KeTimeIncrement read whatever the function fallback left.
     *
     * test_bridge_ordinals.py now checks every entry below against the export
     * table, which is why the block cannot drift again unnoticed. */
    switch (ordinal) {
    case  16: return XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    case  22: return XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE;
    case  30: return XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE;
    case  31: return XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE;
    case  40: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_CACHE_PARTS;
    case  41: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_MODEL_STR;
    case  42: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_SERIAL_STR;
    case  64: return XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE;
    case  70: return XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE;
    case  71: return XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE;
    case 156: return XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT;
    case 157: return XBOX_KERNEL_DATA_BASE + KDATA_TIME_INCREMENT;
    case 164: return XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE;
    case 259: return XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    case 322: return XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO;
    case 323: return XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY;
    case 324: return XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION;
    case 325: return XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY;
    case 326: return XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_FILENAME;
    case 353: return XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY;
    case 354: return XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS;
    case 355: return XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY;
    case 356: return XBOX_KERNEL_DATA_BASE + KDATA_BOOT_SMC_VIDEO;
    case 357: return XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL;
    default:  return 0;  /* Not a data export */
    }
}

/**
 * Initialize kernel data export values at the kernel data area.
 * Called during bridge init, after Xbox memory is mapped.
 */
static void kernel_data_init(void)
{
    /* XboxHardwareInfo (ordinal 322) - XBOX_HARDWARE_INFO
     *   +0: ULONG Flags (0 = retail, 0x20 = devkit)
     *   +4: UCHAR GpuRevision
     *   +5: UCHAR McpRevision
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 0) = 0;   /* Retail */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 4) = 0xA1; /* NV2A A1 */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 5) = 0xB1; /* MCPX B1 */

    /* XboxKrnlVersion (ordinal 324) - XBOX_KRNL_VERSION
     *   +0: USHORT Major (1)
     *   +2: USHORT Minor (0)
     *   +4: USHORT Build (5849 = XDK version)
     *   +6: USHORT Qfe (0)
     */
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 0) = 1;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 2) = 0;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 4) = 5849;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 6) = 0;

    /* KeTickCount (ordinal 156) - initialized to current tick count.
     * A background thread in main.c updates this every ~1ms. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) = GetTickCount();

    /* LaunchDataPage (ordinal 164).
     *
     * This is how a title receives a command line. XGetLaunchInfo reads the
     * page, takes LaunchDataType from its first dword, and copies 3072 bytes
     * from +0x400; a type of 3 means those bytes *are* the command line.
     * Half-Life 2 does exactly that in sub_00596710, and falls back to the
     * empty string at 0x00772EA7 when the call fails -- which is what a NULL
     * page produces, so the engine started with no arguments and no map.
     *
     * On hardware the launcher fills this in before rebooting into the title.
     * RECOMP_CMDLINE does the same thing here, so a title can be told to load
     * a level the way the console would tell it: "+map intro".
     */
    {
        const char *cmdline = getenv("RECOMP_CMDLINE");

        if (cmdline && *cmdline) {
            uint32_t page = xbox_HeapAlloc(0x1000 + 0x0C00, 4096);
            if (page) {
                size_t n = strlen(cmdline);
                size_t i;

                if (n > 0x0BFF)
                    n = 0x0BFF;
                BRIDGE_MEM32(page + 0) = 3;          /* LaunchDataType */
                BRIDGE_MEM32(page + 4) = 0x45410091; /* title id */
                for (i = 0; i < n; i++)
                    BRIDGE_MEM8(page + 0x400 + (uint32_t)i) =
                        (uint8_t)cmdline[i];
                BRIDGE_MEM8(page + 0x400 + (uint32_t)n) = 0;

                BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) =
                    page;
                fprintf(stderr, "  Launch data: type 3 at 0x%08X, "
                                "command line %s\n", page, cmdline);
            } else {
                BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;
            }
        } else {
            BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;
        }
    }

    /* Object-type exports. Each gets a DISTINCT non-zero value rather than 0.
     *
     * They were all zero, which is wrong twice over: a title that null-checks
     * one sees "no such type", and a title that distinguishes two of them --
     * ObReferenceObjectByHandle takes an expected type and compares it -- sees
     * every type as equal, so a mutant handle passes a check meant for events.
     * The values are opaque to the game; only identity and non-nullness matter,
     * so they are the export ordinal offset into the kernel data area, which
     * also makes a stray one recognisable in a crash dump.
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE)    = XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE)     = XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE)    = XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE) = XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE)     = XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE)      = XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE;

    /* KeTimeIncrement (ordinal 157) - 100ns units per clock tick. 0x2710 is
     * 1 ms, which is what KeTickCount above is updated at. A title dividing by
     * this to convert ticks to time gets a division by zero if it is left 0. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TIME_INCREMENT) = 0x2710;

    /* HalBootSMCVideoMode (ordinal 356) - SMC video mode word from boot. 0 is
     * "no video mode reported", which titles treat as auto-detect. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_BOOT_SMC_VIDEO) = 0;

    /* IdexChannelObject (ordinal 357) - IDE channel object. Opaque; only ever
     * passed back to Io* routines we stub, so a recognisable non-null is enough. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL) = XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL;

    /* HalDiskCachePartitionCount (ordinal 40) - number of cache partitions.
     * Retail consoles report 3 (X, Y, Z). Titles size a partition array from
     * this, so 0 gives a zero-length array and 1 hides two drives. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_DISK_CACHE_PARTS) = 3;

    /* IoCompletionObjectType (ordinal 64) - type object */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE) = 0;

    /* IoDeviceObjectType (ordinal 71) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE) = 0;

    /* XboxHDKey (ordinal 323) - 16 bytes of zeros (no key) */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxSignatureKey (ordinal 325) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxLANKey (ordinals 326, 355) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxAlternateSignatureKeys (ordinals 327, 356) - 256 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS) + g_xbox_mem_offset), 0, 256);

    /* XePublicKeyData (ordinal 357) - 284 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY) + g_xbox_mem_offset), 0, 284);

    /* HAL disk identity strings (ordinals 41/42). The exported symbol is an
     * XBOX_ANSI_STRING whose Buffer must be an Xbox VA the title can deref --
     * HalRandGather reads the bytes for entropy. Build the struct and its text
     * inside the kernel data area so both are addressable. */
    {
        struct { uint32_t str_off, buf_off; const char *text; } d[] = {
            { KDATA_DISK_MODEL_STR,  KDATA_DISK_MODEL_BUF,  "XBOXRECOMP VIRTUAL HDD" },
            { KDATA_DISK_SERIAL_STR, KDATA_DISK_SERIAL_BUF, "XR0000000000" },
            /* XeImageFileName (ordinal 326). Declared but never filled in, so
             * its Buffer held whatever was in the page -- Half-Life 2's CRT
             * reads it while working out the running image's path, took the
             * uninitialised bytes as a char*, and dereferenced 0x68737572
             * (the ASCII "rush"). A disc-booted title's value looks like this. */
            { KDATA_XE_IMAGE_FILENAME, KDATA_XE_IMAGE_BUF,
              "\\Device\\CdRom0\\default.xbe" },
        };
        for (int k = 0; k < (int)(sizeof(d) / sizeof(d[0])); k++) {
            uint32_t str_va = XBOX_KERNEL_DATA_BASE + d[k].str_off;
            uint32_t buf_va = XBOX_KERNEL_DATA_BASE + d[k].buf_off;
            size_t len = strlen(d[k].text);
            memcpy(XBOX_TO_NATIVE(buf_va), d[k].text, len + 1);
            BRIDGE_MEM16(str_va + 0) = (uint16_t)len;        /* Length */
            BRIDGE_MEM16(str_va + 2) = (uint16_t)(len + 1);  /* MaximumLength */
            BRIDGE_MEM32(str_va + 4) = buf_va;               /* Buffer (Xbox VA) */
        }
    }

    fprintf(stderr, "  Kernel data exports: initialized at Xbox VA 0x%08X\n",
            XBOX_KERNEL_DATA_BASE);
}

/* ── Per-slot ordinal and bridge function ────────────────── */

/* Ordinal for each slot (read from Xbox memory during init) */
static ULONG g_slot_ordinals[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Log counter - limit output to avoid flooding */
/* Calls per ordinal, for the ranking in the periodic summary. 378 counters
 * is smaller than one of the strings this file prints. */
static unsigned long long g_ordinal_calls[XBOX_KERNEL_THUNK_TABLE_SIZE];
static int g_kernel_call_count = 0;

/* How many kernel calls get logged before the log goes quiet.
 *
 * The cap keeps a title that makes thousands of calls from burying the
 * console, but a bring-up that gets past early init then has no visibility at
 * exactly the point it stops being obvious. Override with
 * RECOMP_KERNEL_LOG_BUDGET, the same way RECOMP_TRACE_BUDGET works for the
 * function tracer. 0 silences the log entirely.
 */
static long kernel_log_budget(void)
{
    static long budget = -1;

    if (budget < 0) {
        const char *env = getenv("RECOMP_KERNEL_LOG_BUDGET");
        budget = env ? strtol(env, NULL, 0) : 200;
        if (budget < 0)
            budget = 0;
    }
    return budget;
}

#define KERNEL_LOG_ON()      (g_kernel_call_count <= kernel_log_budget())
/* Some sites logged at a tighter cap than the rest; keep them proportional. */
#define KERNEL_LOG_ON_HALF() (g_kernel_call_count <= kernel_log_budget() / 2)

/* Read Xbox stack arg as uint32_t.
 * After kernel_thunk_dispatch pops the dummy return address (g_esp += 4),
 * arg0 is at g_esp+0, arg1 at g_esp+4, etc. */
#define STACK_ARG(n) ((uint32_t)BRIDGE_MEM32(g_esp + (n) * 4))

/* Guest return address of the call currently in a bridge. */
RECOMP_TLS uint32_t g_xbox_kernel_caller;

/* ── Per-ordinal bridge functions ─────────────────────────
 *
 * Each bridge reads args from the Xbox stack, translates pointer
 * args from Xbox VA→native, calls the kernel function, and stores
 * the result in g_eax.
 *
 * Xbox cdecl: args pushed right-to-left, caller cleans stack.
 * Xbox stdcall: args pushed right-to-left, callee cleans stack.
 * In our case the caller (translated code) does "PUSH32" for each arg
 * before calling, and the kernel function's ret-N is handled by the
 * translated code's own stack adjustment.
 */

/* ── PsCreateSystemThreadEx (ordinal 255) ────────────────
 * NTSTATUS PsCreateSystemThreadEx(
 *   PHANDLE ThreadHandle,      // arg0: Xbox VA → pointer
 *   ULONG ThreadExtraSize,     // arg1: value
 *   ULONG KernelStackSize,     // arg2: value
 *   ULONG TlsDataSize,         // arg3: value
 *   PULONG ThreadId,           // arg4: Xbox VA → pointer (can be NULL)
 *   PVOID StartContext1,       // arg5: Xbox VA → opaque
 *   PVOID StartContext2,       // arg6: Xbox VA → opaque
 *   BOOLEAN CreateSuspended,   // arg7: value
 *   BOOLEAN DebugStack,        // arg8: value
 *   PXBOX_SYSTEM_ROUTINE StartRoutine  // arg9: Xbox function pointer
 * )
 *
 * For static recompilation, we don't create a real thread.
 * Instead we call the StartRoutine synchronously via RECOMP_ICALL.
 * This is correct because on Xbox, the entry point creates a system
 * thread and returns, and the thread runs the actual game.
 */
static int g_thread_call_count = 0;

/* Thread entry shim. Sets up the new thread's own simulated stack, pushes the
 * two Xbox start-context arguments plus the dummy return address the callee's
 * `ret` consumes, and runs. */
/* Set on threads this bridge spawned; see PsTerminateSystemThread. */
static RECOMP_TLS int g_is_spawned_thread = 0;

/* This thread's simulated stack, so both exits can give it back. Thread-local
 * for the obvious reason, and needed at all because the common exit is
 * ExitThread from PsTerminateSystemThread -- bridge_thread_main's own return
 * path is the rare one. */
static RECOMP_TLS uint32_t g_thread_stack_top = 0;

struct bridge_thread_start {
    recomp_func_t fn;
    uint32_t ctx1, ctx2, stack_top;
};

static void bridge_write_handle(uint32_t handle_va, HANDLE h);

static void bridge_run_thread_inline(recomp_func_t fn, uint32_t ctx1,
                                     uint32_t ctx2)
{
    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx2;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx1;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    g_seh_ebp = g_esp;
    fn();
    g_esp += 12;
}

static DWORD WINAPI bridge_thread_main(LPVOID param)
{
    struct bridge_thread_start *s = (struct bridge_thread_start *)param;
    recomp_func_t fn = s->fn;
    uint32_t ctx1 = s->ctx1, ctx2 = s->ctx2;

    /* Own register set (RECOMP_TLS), own simulated stack -- and own TIB.
     *
     * The TIB carries the SEH chain head and, through fs:[4], the CRT's
     * per-thread data. Sharing one made "which thread am I" a single answer
     * for every thread, which is how two of them ended up inside _lock() each
     * holding the lock the other wanted. */
    g_is_spawned_thread = 1;
    g_esp = s->stack_top;
    g_thread_stack_top = s->stack_top;
    {
        uint32_t tib = xbox_AllocThreadTib();
        if (tib)
            g_fs_base = tib;
        else
            fprintf(stderr, "  [KERNEL] worker thread has no TIB of its own;"
                            " it shares the main thread's\n");
    }
    free(s);

    bridge_run_thread_inline(fn, ctx1, ctx2);

    fprintf(stderr, "  [KERNEL] worker thread returned (eax=0x%08X)\n", g_eax);
    fflush(stderr);
    /* The routine returned instead of calling PsTerminateSystemThread; the
     * stack is still ours to give back. */
    xbox_FreeThreadStack(g_thread_stack_top);
    g_thread_stack_top = 0;
    return 0;
}

static HANDLE bridge_spawn_thread(recomp_func_t fn, uint32_t ctx1,
                                  uint32_t ctx2, uint32_t stack_top)
{
    struct bridge_thread_start *s = malloc(sizeof(*s));
    HANDLE th;

    if (!s) return NULL;
    s->fn = fn; s->ctx1 = ctx1; s->ctx2 = ctx2; s->stack_top = stack_top;

    th = CreateThread(NULL, 0, bridge_thread_main, s, 0, NULL);
    if (!th) free(s);
    /* Record the game thread so a host-tick-driven title's watchdog can sample
     * it via xbox_thread_debug_handle. Harmless for default-model titles: they
     * spawn workers too, but never read it back. See kernel_thread.c. */
    else xbox_set_game_thread(th);
    return th;
}

/* Two ways a title expects its first PsCreateSystemThreadEx to behave.
 *
 * INLINE (default): the first call IS the game starting -- run the routine
 * inline, inheriting register state, and it drives its own main loop forever.
 * This is what Halo and Crimson Skies need and the historical behavior.
 *
 * SPAWN: the title's entry spawns an init thread and RETURNS, expecting the host
 * to drive the per-frame tick afterwards (Burnout 3 is tick-driven). Here the
 * first call must spawn a real thread and return, so control comes back to the
 * host. Opt in with xbox_SetThreadMode before the game starts. See
 * docs/technical/burnout3-reunification.md. */
/* XBOX_THREAD_MODE_* and xbox_SetThreadMode are declared in xbox_memory_layout.h. */
static int g_thread_mode = XBOX_THREAD_MODE_INLINE;
void xbox_SetThreadMode(int mode) { g_thread_mode = mode; }

static void bridge_PsCreateSystemThreadEx(void)
{
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t start_context1  = STACK_ARG(5);
    uint32_t start_context2  = STACK_ARG(6);
    uint32_t start_routine   = STACK_ARG(9);
    /* In SPAWN mode there is no privileged "first call": every thread is real,
     * so the entry can return. In INLINE mode the first call runs the game. */
    int is_first_call = (g_thread_mode == XBOX_THREAD_MODE_INLINE)
                        && (g_thread_call_count == 0);
    g_thread_call_count++;

    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx #%d: routine=0x%08X ctx1=0x%08X ctx2=0x%08X\n",
            g_thread_call_count, start_routine, start_context1, start_context2);
    fflush(stderr);

    /* Write a fake handle to the output pointer */
    if (xbox_handle_ptr) {
        BRIDGE_MEM32(xbox_handle_ptr) = 0xBEEF0001;  /* fake handle */
    }

    /* Call the start routine synchronously through the recomp dispatch.
     * Xbox thread start routines receive two parameters:
     *   void ThreadRoutine(PVOID StartContext1, PVOID StartContext2)
     * We push both onto the simulated stack (right-to-left).
     *
     * First call: the game's main thread entry point. Must run synchronously
     * and inherit the current register state (this IS the game starting).
     *
     * Subsequent calls: worker threads. Must save/restore ALL global registers
     * because on real Xbox each thread has its own register set. Without this,
     * the worker clobbers the caller's g_esi, g_ebx, etc. */
    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn) fn = recomp_lookup_manual(start_routine);
        if (fn) {
            if (is_first_call) {
                /* Main game thread: run directly, inheriting register state */
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                fn();
                g_esp += 12;
                fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: main thread returned (g_eax=0x%08X)\n", g_eax);
                fflush(stderr);
            } else {
                /* Worker thread: a real one.
                 *
                 * This used to run the routine synchronously and restore the
                 * caller's registers afterwards, which is fine only for a
                 * worker that finishes. Halo's cache/file worker does not -- it
                 * blocks on an event waiting for requests, so CreateThread
                 * never returned and startup deadlocked before the main loop.
                 *
                 * Now that the register set is thread-local (RECOMP_TLS), a
                 * spawned thread gets its own, and the caller's is untouched by
                 * construction rather than by save/restore. */
                /* RECOMP_WORKERS=inline runs a title's worker routines on
                 * the calling thread instead of spawning one.
                 *
                 * Not a mode to ship a title in -- a worker that blocks
                 * waiting for requests never returns, and the caller never
                 * gets control back. It is a bisecting tool: when something
                 * only goes wrong with two guest threads running, this says so
                 * in one run, and the answer separates a concurrency bug from
                 * everything else it might have been. */
                const char *inline_workers = getenv("RECOMP_WORKERS");
                uint32_t stack_top;

                if (inline_workers && !strcmp(inline_workers, "inline")) {
                    fprintf(stderr, "  [KERNEL] RECOMP_WORKERS=inline: running "
                            "worker 0x%08X on this thread\n", start_routine);
                    fflush(stderr);
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                    return;
                }

                stack_top = xbox_AllocThreadStack();

                if (!stack_top) {
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: out of "
                            "thread stacks, running worker 0x%08X inline\n",
                            start_routine);
                    fflush(stderr);
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                } else {
                    HANDLE th = bridge_spawn_thread(fn, start_context1,
                                                    start_context2, stack_top);
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: spawned "
                            "worker 0x%08X (ctx=0x%08X, stack top 0x%08X)\n",
                            start_routine, start_context1, stack_top);
                    fflush(stderr);
                    if (xbox_handle_ptr && th) {
                        bridge_write_handle(xbox_handle_ptr, th);
                    }
                }
            }
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
        }
    }

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtClose (ordinal 187) ───────────────────────────────
 * NTSTATUS NtClose(HANDLE Handle)
 * Handle is a value (not a pointer), so safe for generic call.
 */
/* Handle-table helpers; defined further below. Xbox memory slots are 32-bit
 * but native HANDLEs are 64-bit pointers, so handles are kept in a table and
 * referenced by tagged 32-bit tokens. */
static void   bridge_write_handle(uint32_t handle_va, HANDLE h);
static HANDLE bridge_take_handle(uint32_t token);

static void bridge_NtClose(void)
{
    uint32_t raw_handle = STACK_ARG(0);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtClose: handle=0x%08X\n", raw_handle);
        fflush(stderr);
    }

    /* Close real handles but skip fake/synthetic ones */
    if (raw_handle && raw_handle != 0xDEAD0001u && raw_handle != 0xBEEF0010u) {
        HANDLE h = bridge_take_handle(raw_handle);
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── MmAllocateContiguousMemory (ordinal 165) ─────────────
 * PVOID MmAllocateContiguousMemory(ULONG NumberOfBytes)
 */
static void bridge_MmAllocateContiguousMemory(void)
{
    uint32_t size = STACK_ARG(0);

    /* From the contiguous window, not the general heap: the caller is
     * entitled to assume (VA & 0x0FFFFFFF) | 0x80000000 == VA, because that
     * is how a driver converts between the address it holds and the one it
     * hands the hardware. See xbox_ContiguousAlloc. */
    uint32_t xbox_va = xbox_ContiguousAlloc(size, 4096);

    if (KERNEL_LOG_ON_HALF()) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemory: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmAllocateSystemMemory (ordinal 167) ─────────────────
 * PVOID MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect)
 *
 * The kernel's own page allocator: whole pages, not the title's heap, and not
 * required to be physically contiguous. Titles use it for large buffers they
 * intend to keep -- Half-Life 2 takes one while loading a level, right after
 * it opens the .bsp.
 *
 * Unbridged this returned 0, and a NULL from an allocator is not a failure the
 * caller checks for here; it carried the pointer into graphics setup and hung
 * touching the NV2A aperture with it. Page-aligned out of the ordinary heap is
 * the right answer: the distinction the console draws between system memory
 * and the title heap is about which pool the pages come from, and there is one
 * pool here.
 *
 * Zeroed, because the console hands out zeroed pages and callers assume it.
 */
static void bridge_MmAllocateSystemMemory(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t prot = STACK_ARG(1);
    uint32_t xbox_va;

    (void)prot;
    if (!size) {
        g_eax = 0;
        return;
    }
    xbox_va = xbox_HeapAlloc(size, 4096);
    if (xbox_va)
        memset((void *)((uintptr_t)xbox_va + g_xbox_mem_offset), 0, size);
    else
        fprintf(stderr, "  [KERNEL] MmAllocateSystemMemory: %u bytes REFUSED\n",
                size);
    g_eax = xbox_va;
}

/* ── MmAllocateContiguousMemoryEx (ordinal 166) ───────────
 * PVOID MmAllocateContiguousMemoryEx(SIZE_T size, ULONG_PTR low, ULONG_PTR high,
 *                                     ULONG alignment, ULONG protect)
 */
/* Contiguous memory is addressed through the physical-memory mirror: physical
 * page P is visible at 0x80000000 + P. Titles that pin buffers at fixed
 * physical addresses check the returned pointer against that, so the address
 * has to be honoured rather than satisfied from the general heap. */
#define XBOX_PHYSICAL_MIRROR_BASE 0x80000000u

static void bridge_MmAllocateContiguousMemoryEx(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t low = STACK_ARG(1);
    uint32_t high = STACK_ARG(2);
    uint32_t align = STACK_ARG(3);
    uint32_t prot = STACK_ARG(4);
    uint32_t xbox_va;

    (void)prot;

    /*
     * A caller that constrains the range to exactly one allocation's worth is
     * demanding a specific physical address, not expressing a preference.
     * Halo does this for its two big pools and asserts on the result
     * (physical_memory_map.c:46) - XPhysicalAlloc passes lowest = the address
     * it wants and highest = lowest + size - 1, then requires
     * 0x80000000 | lowest back. Satisfying that from the heap fails the assert
     * and leaves its whole memory map wrong.
     */
    if (low && high >= low && (high - low + 1) <= size + 0x1000) {
        xbox_va = XBOX_PHYSICAL_MIRROR_BASE + low;

        /* The console hands out zeroed pages here, and titles rely on it:
         * pool headers and free-list roots are assumed clear, so whatever the
         * backing view happened to contain shows up later as structures that
         * are "allocated" but full of garbage. */
        memset((void *)((uintptr_t)xbox_va + g_xbox_mem_offset), 0, size);

        if (KERNEL_LOG_ON_HALF()) {
            fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u "
                    "pinned phys 0x%08X -> Xbox VA 0x%08X (zeroed)\n",
                    size, low, xbox_va);
            fflush(stderr);
        }
        g_eax = xbox_va;
        return;
    }

    if (align < 4096) align = 4096;
    xbox_va = xbox_ContiguousAlloc(size, align);

    if (KERNEL_LOG_ON_HALF()) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u align=%u → Xbox VA 0x%08X\n",
                size, align, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmFreeContiguousMemory (ordinal 171) ─────────────────
 * VOID MmFreeContiguousMemory(PVOID BaseAddress)
 */
static void bridge_MmFreeContiguousMemory(void)
{
    uint32_t addr = STACK_ARG(0);
    xbox_HeapFree(addr);
    g_eax = 0;
}

/* ── NtAllocateVirtualMemory (ordinal 184) ────────────────
 * NTSTATUS NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits,
 *     PULONG AllocationSize, ULONG AllocationType, ULONG Protect)
 */
static void bridge_NtAllocateVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);  /* PVOID* in Xbox VA */
    uint32_t zero_bits = STACK_ARG(1);
    uint32_t size_ptr = STACK_ARG(2);  /* PULONG in Xbox VA */
    uint32_t alloc_type = STACK_ARG(3);
    uint32_t protect = STACK_ARG(4);

    /* Read the requested size from Xbox memory */
    uint32_t size = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;
    /* Read the base address hint (0 = let kernel choose) */
    uint32_t base_hint = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: base=0x%08X size=%u type=0x%X prot=0x%X\n",
                base_hint, size, alloc_type, protect);
        fflush(stderr);
    }

    if (size == 0) {
        g_eax = 0xC0000045u; /* STATUS_INVALID_PAGE_PROTECTION */
        return;
    }

    /*
     * Xbox NtAllocateVirtualMemory supports two modes:
     * - MEM_RESERVE (0x2000): Reserve virtual address space
     * - MEM_COMMIT  (0x1000): Commit pages within a reserved region
     * - MEM_RESERVE|MEM_COMMIT (0x3000): Both in one call
     *
     * Our Xbox heap (bump allocator) always commits memory immediately,
     * so MEM_COMMIT on an already-reserved region is a no-op.
     * Only allocate new memory when MEM_RESERVE is requested.
     */
    /* An address above physical RAM is not free memory -- it aliases.
     *
     * The runtime maps 64 MB and then mirrors it at 64 MB intervals,
     * because real Xbox RAM wraps on a 26-bit address bus. So a guest that
     * sub-allocates past the top of RAM does not get fresh pages, it gets
     * low memory that something else already owns, and the two quietly
     * share storage. Half-Life 2 put a CUtlRBTree element array at
     * 0x0CB80000, which aliases 0x00B80000; other regions it took land
     * inside the live heap (0x05B80000 -> 0x01B80000).
     *
     * Real hardware wraps *physical* addresses while translating virtual
     * ones, so this never happens there. Modelling every guest address as
     * physical is the gap, and that is a bigger change than a bridge fix.
     * Until then, say so: silent aliasing surfaces as corrupted data
     * structures far from here, which is the worst way to find it.
     */
    if (base_hint >= (g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram)) {
        static unsigned warned;
        if (warned++ < 8)
            fprintf(stderr,
                    "  [KERNEL] WARNING: allocation at 0x%08X is above "
                    "%u MB mapped; it aliases 0x%08X\n",
                    base_hint,
                    (unsigned)((g_xbox_map_size ? g_xbox_map_size
                                                : g_xbox_total_ram)
                               / (1024 * 1024)),
                    (uint32_t)(base_hint % (g_xbox_map_size
                                            ? g_xbox_map_size
                                            : g_xbox_total_ram)));
        fflush(stderr);
    }

    if (base_hint != 0 && (alloc_type & 0x2000) == 0) {
        /* MEM_COMMIT only, on an already-reserved region.
         * The memory is already committed by our bump allocator.
         * Don't change the base address - just return success. */
        if (KERNEL_LOG_ON()) {
            fprintf(stderr, "  [KERNEL] → MEM_COMMIT on existing region 0x%08X, no-op\n", base_hint);
            fflush(stderr);
        }
        g_eax = 0; /* STATUS_SUCCESS */
        return;
    }

    /* Allocate from Xbox heap (MEM_RESERVE or MEM_RESERVE|MEM_COMMIT).
     *
     * A pure MEM_RESERVE costs no RAM on real hardware -- it takes address
     * space out of a 4 GB range, not pages out of the 64 MB of memory -- so
     * titles reserve far more than the console physically has and commit a
     * fraction of it. Our heap is a bump allocator that commits everything it
     * hands out, so a large reserve asks for RAM that does not exist.
     *
     * Half-Life 2's XBE header sets PeHeapReserve to 128 MB, and its CRT
     * reserves exactly that during RtlCreateHeap. Failing it returned
     * STATUS_NO_MEMORY, RtlCreateHeap returned 0, and CRT init aborted before
     * main -- on a console with 64 MB, asking for 128 MB is normal, not an
     * error.
     *
     * So a reserve that does not fit is clamped to what the heap can actually
     * back, and the caller is told the real size through the IN/OUT RegionSize
     * parameter, which is where the API already reports the rounded figure.
     *
     * ponytail: the honest fix is a reserve that costs nothing and a commit
     * that backs pages on demand, which needs the allocator to separate the
     * two. This clamp is enough for a title that reserves generously and
     * commits little, and it fails loudly and later rather than silently and
     * at startup if one does not. */
    uint32_t xbox_va = xbox_HeapAlloc(size, 4096);
    if (!xbox_va && (alloc_type & 0x2000) && !(alloc_type & 0x1000)) {
        /* A pure reservation too big for the heap. Take it from the mapped
         * space above RAM, where it costs no heap and the pages are distinct.
         *
         * Clamping instead -- handing back a fraction of what was asked for --
         * is what broke Half-Life 2. It reserves 128 MB and then 200 MB, got
         * 32 MB and 12.5 MB, and then sub-allocated across the range it
         * believed it owned. That walks past the top of RAM, where the mirrors
         * alias low memory, so its containers quietly shared storage with the
         * live heap. Granting the full range is both more honest and less
         * damaging. Returns 0 unless the title asked for a mapping larger than
         * RAM, so nothing changes for titles that did not. */
        xbox_va = xbox_ReserveAlloc(size, 4096);
        if (xbox_va) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u"
                            " granted at 0x%08X above RAM\n", size, xbox_va);
            fflush(stderr);
        }
    }
    if (!xbox_va && (alloc_type & 0x2000) && !(alloc_type & 0x1000)) {
        uint32_t want = size;
        while (want > 0x10000 && !xbox_va) {
            want /= 2;
            xbox_va = xbox_HeapAlloc(want, 4096);
        }
        if (xbox_va) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u "
                            "clamped to %u (heap cannot back the full range)\n",
                    size, want);
            fflush(stderr);
            size = want;
        }
    }
    if (!xbox_va) {
        g_eax = 0xC0000017u; /* STATUS_NO_MEMORY */
        return;
    }

    /* Write back the allocated address and actual size */
    if (base_ptr) BRIDGE_MEM32(base_ptr) = xbox_va;
    if (size_ptr) BRIDGE_MEM32(size_ptr) = size;

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtFreeVirtualMemory (ordinal 199) ────────────────────
 * NTSTATUS NtFreeVirtualMemory(PVOID *BaseAddress, PULONG FreeSize,
 *     ULONG FreeType)
 */
/* -- NtQueryVirtualMemory (ordinal 217, 2 args = 8 bytes) --------------
 *
 * Xbox takes two arguments, not NT's four:
 *
 *     NTSTATUS NtQueryVirtualMemory(PVOID BaseAddress,
 *                                   PMEMORY_BASIC_INFORMATION Info);
 *
 * This has to be a guest-side answer. The existing xbox_NtQueryVirtualMemory
 * in kernel_memory.c calls the host VirtualQuery and memcpy's a host
 * MEMORY_BASIC_INFORMATION into guest memory, whose pointer fields are 64-bit
 * on an x64 build -- so every field after BaseAddress lands in the wrong place.
 *
 * It also has to exist at all. Without a bridge entry the thunk is left
 * unbridged, and a title's CRT heap creation calls this to probe its heap
 * region: RtlCreateHeap does
 *
 *     call NtQueryVirtualMemory ; test eax,eax ; jl fail
 *     cmp  mbi.BaseAddress, requested ; jne fail
 *     cmp  mbi.State, MEM_FREE        ; je  fail
 *
 * and returns 0 on any of those. On Half-Life 2 that null heap propagated
 * silently through the rest of CRT init.
 *
 * Guest MEMORY_BASIC_INFORMATION, 32-bit, 28 bytes:
 *     +0x00 BaseAddress   +0x04 AllocationBase  +0x08 AllocationProtect
 *     +0x0C RegionSize    +0x10 State           +0x14 Protect
 *     +0x18 Type
 *
 * The guest is one flat committed mapping, so that is what we report: any
 * address inside it is MEM_COMMIT / PAGE_READWRITE / MEM_PRIVATE, and anything
 * outside is MEM_FREE rather than an error, which is the honest answer and the
 * one that lets a caller distinguish the two.
 */
static void bridge_NtQueryVirtualMemory(void)
{
    uint32_t base_va = STACK_ARG(0);
    uint32_t info_va = STACK_ARG(1);
    uint32_t page_base = base_va & ~0xFFFu;

    if (!info_va) {
        g_eax = 0xC000000Du;               /* STATUS_INVALID_PARAMETER */
        return;
    }

    BRIDGE_MEM32(info_va + 0x00) = page_base;          /* BaseAddress */
    BRIDGE_MEM32(info_va + 0x04) = page_base;          /* AllocationBase */
    BRIDGE_MEM32(info_va + 0x08) = 0x04;               /* PAGE_READWRITE */
    BRIDGE_MEM32(info_va + 0x14) = 0x04;               /* Protect */
    BRIDGE_MEM32(info_va + 0x18) = 0x20000;            /* MEM_PRIVATE */

    if (page_base >= g_xbox_code_lo && page_base < XBOX_TOTAL_RAM) {
        BRIDGE_MEM32(info_va + 0x0C) = XBOX_TOTAL_RAM - page_base; /* RegionSize */
        BRIDGE_MEM32(info_va + 0x10) = 0x1000;         /* MEM_COMMIT */
    } else {
        BRIDGE_MEM32(info_va + 0x0C) = 0x1000;
        BRIDGE_MEM32(info_va + 0x10) = 0x10000;        /* MEM_FREE */
        BRIDGE_MEM32(info_va + 0x08) = 0;
        BRIDGE_MEM32(info_va + 0x18) = 0;
    }

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtQueryVirtualMemory: base=0x%08X -> "
                        "state=0x%X size=%u\n", base_va,
                        BRIDGE_MEM32(info_va + 0x10),
                        BRIDGE_MEM32(info_va + 0x0C));
        fflush(stderr);
    }
    g_eax = 0;                                          /* STATUS_SUCCESS */
}

static void bridge_NtFreeVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t size_ptr = STACK_ARG(1);
    uint32_t free_type = STACK_ARG(2);

    g_eax = (uint32_t)xbox_NtFreeVirtualMemory(
        XBOX_TO_NATIVE(base_ptr), XBOX_TO_NATIVE(size_ptr), free_type);
}

/* ── ExAllocatePool / ExAllocatePoolWithTag (ordinals 15, 16) ─
 * Must allocate from Xbox heap so the returned pointer is an Xbox VA
 * that can be accessed via MEM32(). Native HeapAlloc returns 64-bit
 * pointers that get truncated and produce garbage Xbox VAs.
 */
/* ExQueryNonVolatileSetting(ValueIndex, Type, Value, ValueLength, ResultLength)
 *
 * Titles read region, language and AV settings from EEPROM through this very
 * early in boot. Ordinal 24 was previously routed to bridge_ExQueryPoolBlockSize,
 * so the call returned a pool size where the game expected a settings blob. */
/* ── FscGetCacheSize (35) / FscSetCacheSize (37) ───────────
 *
 * The Xbox filesystem cache, sized in 4 KB pages. A title that streams from a
 * pack file resizes it around the work: Half-Life 2's pack scanner
 * (sub_0041E650) reads the current size, sets 1 MB for the scan, and puts the
 * old value back when it is done.
 *
 * There is no cache here -- reads go to the host filesystem, which has its
 * own -- so the size is only ever a number the title stores and restores.
 * Keeping it is still worth doing: unbridged, the getter returned 0, and a
 * title that saves that and restores it later is restoring a cache size of
 * zero pages. 64 KB is the console's own default.
 */
#define XBOX_FSCACHE_DEFAULT_PAGES 16u      /* 64 KB in 4 KB pages */

static uint32_t g_fscache_pages = XBOX_FSCACHE_DEFAULT_PAGES;

static void bridge_FscGetCacheSize(void)
{
    g_eax = g_fscache_pages;
}

static void bridge_FscSetCacheSize(void)
{
    uint32_t pages = STACK_ARG(0);

    /* The kernel rejects a request it cannot satisfy and leaves the current
     * size alone; the caller checks for a negative status. Nothing here can
     * fail, so accept it and remember what was asked for. */
    g_fscache_pages = pages;
    g_eax = STATUS_SUCCESS;
}

/* ── RtlCompareMemory (268) / RtlCompareMemoryUlong (269) ──
 *
 * Both answer "how far do these match", counted in bytes from the start, and
 * both are used to decide whether a buffer needs work rather than to do it.
 * Unbridged they returned 0, which reads as "differs at the first byte" -- the
 * safe-looking answer that is wrong whenever the caller is checking for a
 * region it can skip.
 *
 * RtlCompareMemoryUlong compares against a repeating ULONG and only ever
 * examines whole ULONGs, so a length that is not a multiple of four leaves the
 * remainder uncompared; the count it returns is still in bytes.
 */
static void bridge_RtlCompareMemory(void)
{
    uint32_t a_va   = STACK_ARG(0);
    uint32_t b_va   = STACK_ARG(1);
    uint32_t length = STACK_ARG(2);
    const uint8_t *a, *b;
    uint32_t i;

    if (!a_va || !b_va || !length) {
        g_eax = 0;
        return;
    }
    a = (const uint8_t *)XBOX_TO_NATIVE(a_va);
    b = (const uint8_t *)XBOX_TO_NATIVE(b_va);
    for (i = 0; i < length; i++) {
        if (a[i] != b[i])
            break;
    }
    g_eax = i;
}

static void bridge_RtlCompareMemoryUlong(void)
{
    uint32_t base_va = STACK_ARG(0);
    uint32_t length  = STACK_ARG(1);
    uint32_t pattern = STACK_ARG(2);
    uint32_t i;

    if (!base_va) {
        g_eax = 0;
        return;
    }
    length &= ~3u;                      /* whole ULONGs only */
    for (i = 0; i < length; i += 4) {
        if (BRIDGE_MEM32(base_va + i) != pattern)
            break;
    }
    g_eax = i;
}

static void bridge_ExQueryNonVolatileSetting(void)
{
    uint32_t value_index  = STACK_ARG(0);
    uint32_t type_va      = STACK_ARG(1);
    uint32_t value_va     = STACK_ARG(2);
    uint32_t value_length = STACK_ARG(3);
    uint32_t result_va    = STACK_ARG(4);

    NTSTATUS st = xbox_ExQueryNonVolatileSetting(
        value_index,
        type_va   ? (PULONG)&BRIDGE_MEM32(type_va)   : NULL,
        value_va  ? (PVOID)((uintptr_t)value_va + g_xbox_mem_offset) : NULL,
        value_length,
        result_va ? (PULONG)&BRIDGE_MEM32(result_va) : NULL);

    g_eax = (uint32_t)st;
}

/* HalReturnToFirmware(Routine) - the title asking to reboot or quit.
 *
 * It never returns on hardware. Returning here would let the game run on past
 * a decision to quit, which reads as a hang rather than an exit. */
static void bridge_HalReturnToFirmware(void)
{
    uint32_t routine = STACK_ARG(0);

    /* Routine 2 is a quick reboot, which on Xbox is how a title hands off to
     * another image: XLaunchNewImage fills the launch data page and reboots.
     * So "the title is exiting" and "the title is launching something" look
     * identical here, and the launch page is what tells them apart. */
    {
        uint32_t page = BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE);

        if (page) {
            char path[64];
            uint32_t i;

            for (i = 0; i < sizeof(path) - 1; i++) {
                uint8_t c = BRIDGE_MEM8(page + 8 + i);
                if (!c) break;
                path[i] = (char)c;
            }
            path[i] = 0;
            fprintf(stderr, "  [KERNEL] launch data page 0x%08X:"
                            " type=%u titleid=0x%08X path='%s'\n",
                    page, BRIDGE_MEM32(page), BRIDGE_MEM32(page + 4), path);
            /* XapiBootToDash packs its reason and two parameters into the
             * front of the launch data, so this says why the title asked to
             * leave rather than merely that it did. */
            fprintf(stderr, "  [KERNEL]   launch data:");
            for (i = 0; i < 8; i++)
                fprintf(stderr, " %08X", BRIDGE_MEM32(page + 1024 + i * 4));
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "  [KERNEL] no launch data page set\n");
        }
    }

    /* Who asked to quit.
     *
     * A title exiting looks identical whether it finished cleanly, hit an
     * error path, or was told to reboot -- and the routine number does not say
     * which. The guest call chain does. Same GS format tools/stackwalk.py
     * reads. */
    {
        const uint8_t *mem = (const uint8_t *)g_xbox_mem_offset;
        uint32_t i;

        fprintf(stderr, "  [KERNEL] exit requested, guest esp=0x%08X:\n", g_esp);
        for (i = 0; i < 200; i++) {
            uint32_t a = g_esp + i * 4;
            if (a < 0x00010000u || a >= 0x04000000u) break;
            fprintf(stderr, "    GS %08X %08X\n", a,
                    *(const uint32_t *)(mem + a));
        }
        fflush(stderr);
    }

    fprintf(stderr, "  [KERNEL] HalReturnToFirmware: routine=%u - title is exiting\n",
            routine);
    fflush(stderr);

    /* Write the indirect-branch targets before the process goes away. This
     * path ends in ExitProcess, which does not run atexit handlers, so the
     * host's registered dump never fires -- and a title that gives up during
     * boot is exactly the one whose targets are worth having. No-op unless
     * RECOMP_ICALL_FEEDBACK is on. */
    RECOMP_ICALL_FEEDBACK_DUMP();

    /* Let a host-played FMV finish before the process goes away.
     *
     * The title is not the one presenting it, so it has no reason to wait --
     * it opens the file, carries on, and quits, which would kill the video
     * thread part-way through a five-second clip. Waiting here is what makes
     * the clip actually watchable, and it costs nothing when no video is
     * playing. Bounded, so a stuck player cannot stop the process exiting. */
    {
        extern int xbox_VideoIsPlaying(void);
        int waited = 0;

        while (xbox_VideoIsPlaying() && waited < 60000) {
            Sleep(50);
            waited += 50;
        }
        if (waited)
            fprintf(stderr, "  [KERNEL] waited %dms for the video to finish\n",
                    waited);
    }

    xbox_HalReturnToFirmware(routine);
}

static void bridge_ExAllocatePool(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] ExAllocatePool: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

static void bridge_ExAllocatePoolWithTag(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] ExAllocatePoolWithTag: size=%u tag='%c%c%c%c' → Xbox VA 0x%08X\n",
                size,
                (char)(tag & 0xFF), (char)((tag >> 8) & 0xFF),
                (char)((tag >> 16) & 0xFF), (char)((tag >> 24) & 0xFF),
                xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── KfRaiseIrql / KfLowerIrql (ordinals 160, 161) ────── */
static void bridge_KfRaiseIrql(void)
{
    uint32_t new_irql = STACK_ARG(0);
    g_eax = (uint32_t)xbox_KfRaiseIrql((UCHAR)new_irql);
}

static void bridge_KfLowerIrql(void)
{
    uint32_t new_irql = STACK_ARG(0);
    xbox_KfLowerIrql((UCHAR)new_irql);
    g_eax = 0;
}

/* ── KeRaiseIrqlToDpcLevel (ordinal 129) ─────────────────── */
static void bridge_KeRaiseIrqlToDpcLevel(void)
{
    g_eax = (uint32_t)xbox_KeRaiseIrqlToDpcLevel();
}

/* ── RtlInitializeCriticalSection / Enter / Leave (ordinals 291, 277, 294) ─ */
static void bridge_RtlInitializeCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlInitializeCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlEnterCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlEnterCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlLeaveCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlLeaveCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

/* ── KeQueryPerformanceCounter / Frequency (ordinals 126, 127) ─ */
static void bridge_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceCounter();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

static void bridge_KeQueryPerformanceFrequency(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceFrequency();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

/* ── KeQuerySystemTime (ordinal 128) ─────────────────────── */
static void bridge_KeQuerySystemTime(void)
{
    uint32_t time_ptr = STACK_ARG(0);
    xbox_KeQuerySystemTime(XBOX_TO_NATIVE(time_ptr));
    g_eax = 0;
}

/* ── MmQueryStatistics (ordinal 181) ─────────────────────── */
static void bridge_MmQueryStatistics(void)
{
    uint32_t stats_ptr = STACK_ARG(0);
    g_eax = (uint32_t)xbox_MmQueryStatistics(XBOX_TO_NATIVE(stats_ptr));
}

/* ── NtCreateEvent (ordinal 189) ─────────────────────────── */
static void bridge_NtCreateEvent(void)
{
    uint32_t handle_ptr = STACK_ARG(0);
    uint32_t obj_attr_ptr = STACK_ARG(1);
    uint32_t event_type = STACK_ARG(2);
    uint32_t initial_state = STACK_ARG(3);

    /* Use local HANDLE to avoid 8-byte write to 4-byte Xbox memory slot.
     * On x64, HANDLE is 8 bytes but Xbox expects 4-byte handles. */
    HANDLE local_handle = NULL;
    NTSTATUS status = xbox_NtCreateEvent(
        &local_handle,
        XBOX_TO_NATIVE(obj_attr_ptr),
        event_type, initial_state);

    if (handle_ptr) {
        bridge_write_handle(handle_ptr, local_handle);
    }

    fprintf(stderr, "  [BRIDGE] NtCreateEvent: handle_ptr=0x%08X type=%u init=%u → status=0x%08X handle=0x%08X\n",
            handle_ptr, event_type, initial_state, (uint32_t)status,
            (uint32_t)(uintptr_t)local_handle);

    g_eax = (uint32_t)status;
}

static HANDLE ke_shadow_lookup(uint32_t guest_va);
static void ke_shadow_insert(uint32_t guest_va, HANDLE host);
static HANDLE bridge_resolve_handle(uint32_t token);

/* ── KeSetEvent (ordinal 145) ────────────────────────────── */
static void bridge_KeSetEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);
    HANDLE h;

    (void)increment;
    (void)wait;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = bridge_resolve_handle(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        g_eax = (uint32_t)SetEvent(h);
    else
        g_eax = 0;
}

/* ── KeWaitForSingleObject (ordinal 159) ─────────────────── */
static void bridge_KeWaitForSingleObject(void)
{
    uint32_t object = STACK_ARG(0);
    uint32_t wait_reason = STACK_ARG(1);
    uint32_t wait_mode = STACK_ARG(2);
    uint32_t alertable = STACK_ARG(3);
    uint32_t timeout_ptr = STACK_ARG(4);
    HANDLE h;

    h = ke_shadow_lookup(object);
    if (!h)
        h = bridge_resolve_handle(object);
    if (!h)
        h = XBOX_TO_NATIVE(object);

    g_eax = (uint32_t)xbox_KeWaitForSingleObject(
        h, wait_reason, wait_mode,
        (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
}

/* ── NtWaitForSingleObject (ordinal 233) ─────────────────── */
/*
 * The synchronous sibling of ...Ex. Halo's synchronous ReadFile issues the read
 * and then waits on its completion event through this; unbridged it fell to the
 * "return 0" default (STATUS_SUCCESS = "already signalled"), so the read handshake
 * completed before the data arrived and the UI-map precache never made progress.
 */
static void bridge_NtWaitForSingleObject(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t alertable   = STACK_ARG(1);
    uint32_t timeout_ptr = STACK_ARG(2);

    g_eax = (uint32_t)xbox_NtWaitForSingleObject(
        handle, (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
}

/* ── NtClearEvent (ordinal 186) ──────────────────────────── */
/* Resets an event to non-signalled. Halo clears the read-completion event
 * before each async map read; a no-op here left the event stuck signalled. */
static void bridge_NtClearEvent(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    g_eax = (uint32_t)xbox_NtClearEvent(handle);
}

/* ── NtSetEvent (ordinal 225) ────────────────────────────── */
/* Signals an event and optionally returns its previous state. Unbridged it
 * no-op'd, so a producer's "work ready" signal never landed -- Halo's map-copy
 * worker thread then slept forever in WaitForSingleObject on the decompress
 * context's go-event and only the first 14 KB of the map ever loaded. */
static void bridge_NtSetEvent(void)
{
    HANDLE   handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t prev   = STACK_ARG(1);
    g_eax = (uint32_t)xbox_NtSetEvent(handle, XBOX_TO_NATIVE(prev));
}

/* ── NtPulseEvent (ordinal 205) ──────────────────────────── */
/* Signal-then-reset: releases threads currently waiting, then leaves the event
 * non-signalled. Same unbridged-no-op hazard as NtSetEvent in the map-load
 * handoff chain. PulseEvent carries the (deprecated, lossy) Xbox semantics
 * faithfully -- a waiter not yet blocked misses it, exactly as on hardware. */
static void bridge_NtPulseEvent(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    if (handle) PulseEvent(handle);
    g_eax = 0;
}

/* ── NtWaitForSingleObjectEx (ordinal 234) ───────────────── */
/*
 * Unbridged, this fell through to the "no bridge, returning 0" default -- and 0
 * is STATUS_SUCCESS, so every wait returned instantly as though the object were
 * already signalled. Halo's main loop then spun: 91 million calls in 100
 * seconds, no blocking, no progress. A wait that always succeeds is worse than
 * one that always fails, because it looks like the game is running.
 */
static HANDLE bridge_resolve_handle(uint32_t token);

static void bridge_NtWaitForSingleObjectEx(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t wait_mode   = STACK_ARG(1);
    uint32_t alertable   = STACK_ARG(2);
    uint32_t timeout_ptr = STACK_ARG(3);

    static int logged = 0;
    if (logged++ < 20) {
        fprintf(stderr, "  [KERNEL] NtWaitForSingleObjectEx: token=0x%08X "
                "handle=%p timeout=%s\n",
                STACK_ARG(0), handle, timeout_ptr ? "finite" : "INFINITE");
        fflush(stderr);
    }

    g_eax = (uint32_t)xbox_NtWaitForSingleObjectEx(
        handle, (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(timeout_ptr));
}

/* ── MmQueryAddressProtect (ordinal 179) ─────────────────── */
/* NtWaitForMultipleObjectsEx (ordinal 235, 6 args = 24 bytes)
 *
 * NTSTATUS NtWaitForMultipleObjectsEx(ULONG Count, HANDLE *Handles,
 *                                     WAIT_TYPE WaitType,
 *                                     KPROCESSOR_MODE WaitMode,
 *                                     BOOLEAN Alertable,
 *                                     PLARGE_INTEGER Timeout);
 *
 * Six, not five: WaitMode sits between WaitType and Alertable. Half-Life 2's
 * own call site settles it -- sub_0059BE0F pushes six dwords before the
 * thunk (esi, eax, ebx, 1, [ebp+0x18], edi) and the callee is expected to pop
 * them all.
 *
 * Getting it wrong cost 4 bytes of guest stack per call and shifted every
 * argument after WaitType, so the wait read Alertable as its timeout pointer
 * and reported INFINITE for every wait. The leak was invisible to the esp
 * invariant because sub_0059BE0F restores esp with `leave`: its own frame
 * came back correct while `pop edi; pop esi; pop ebx` took their values one
 * slot out, so the caller's `this` -- kept in ebx by sub_005ACDC0 -- came
 * back holding what esi had, and the next [ebx+0x94] read a string as a
 * pointer.
 *
 * xbox_NtWaitForMultipleObjectsEx has been in kernel_sync.c all along; only
 * the bridge wrapper was missing, so the thunk fell through to the fallback
 * and returned 0 -- STATUS_SUCCESS, meaning 'object 0 is signalled'. A wait
 * that always reports signalled turns a blocking wait into a busy loop, which
 * is exactly what Half-Life 2 does after spawning its first worker: the main
 * thread spins in a CUtlLinkedList walk making no indirect calls at all.
 *
 * Handles is a guest array of tokens, so each has to be resolved
 * individually -- the array cannot just be pointed at. Bounded because a
 * bogus Count would otherwise read arbitrary guest memory onto the stack;
 * MAXIMUM_WAIT_OBJECTS is the real kernel's own limit.
 */
static void bridge_NtWaitForMultipleObjectsEx(void)
{
    uint32_t count       = STACK_ARG(0);
    uint32_t handles_va  = STACK_ARG(1);
    uint32_t wait_type   = STACK_ARG(2);
    uint32_t wait_mode   = STACK_ARG(3);   /* KernelMode / UserMode */
    uint32_t alertable   = STACK_ARG(4);
    uint32_t timeout_ptr = STACK_ARG(5);

    (void)wait_mode;
    HANDLE   handles[MAXIMUM_WAIT_OBJECTS];
    uint32_t i;

    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS || !handles_va) {
        g_eax = 0xC000000Du;             /* STATUS_INVALID_PARAMETER */
        return;
    }
    for (i = 0; i < count; i++)
        handles[i] = bridge_resolve_handle(BRIDGE_MEM32(handles_va + i * 4));

    {
        static int logged;
        if (logged++ < 20) {
            fprintf(stderr, "  [KERNEL] NtWaitForMultipleObjectsEx: count=%u type=%u timeout=%s\n",
                    count, wait_type, timeout_ptr ? "finite" : "INFINITE");
            for (i = 0; i < count; i++)
                fprintf(stderr, "      [%u] token=0x%08X host=%p\n", i,
                        BRIDGE_MEM32(handles_va + i * 4), handles[i]);
            fflush(stderr);
        }
    }

    g_eax = (uint32_t)xbox_NtWaitForMultipleObjectsEx(
        count, handles, wait_type, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(timeout_ptr));
}

/*
 * Takes an Xbox VA, so the native pointer has to be formed before the query --
 * an unbridged 0 return reads as PAGE_NOACCESS. Halo walks all 22 MB of its
 * physical memory map asserting every page is PAGE_READWRITE
 * (physical_memory_map.c:77), so a zero here stops startup on the first page.
 */
static void bridge_MmQueryAddressProtect(void)
{
    uint32_t address = STACK_ARG(0);

    g_eax = address ? (uint32_t)xbox_MmQueryAddressProtect(XBOX_TO_NATIVE(address))
                    : 0;
}

/* ── NtUserIoApcDispatcher (ordinal 232) ─────────────────── */
/*
 * The kernel side of XAPI's ReadFileEx/WriteFileEx. XAPI passes *this* as the
 * ApcRoutine to NtReadFile and puts the title's completion routine in
 * ApcContext, so the dispatcher's only job is to call it with Win32 argument
 * shape:
 *
 *   VOID CALLBACK Completion(DWORD dwErrorCode,
 *                            DWORD dwNumberOfBytesTransfered,
 *                            LPOVERLAPPED lpOverlapped)   // __stdcall, ret 12
 *
 * lpOverlapped is the IO_STATUS_BLOCK pointer: an NT OVERLAPPED begins with
 * Internal/InternalHigh, which is exactly a IO_STATUS_BLOCK, so the title's
 * OVERLAPPED and the block it handed to NtReadFile are the same address.
 * Halo's cache_files_windows completion relies on that -- it reads its own
 * field at lpOverlapped+0x10 and sets the flag the setup loop polls.
 */
static void bridge_NtUserIoApcDispatcher(void)
{
    uint32_t apc_context = STACK_ARG(0);
    uint32_t iostatus    = STACK_ARG(1);
    uint32_t status      = iostatus ? BRIDGE_MEM32(iostatus) : 0;
    uint32_t information = iostatus ? BRIDGE_MEM32(iostatus + 4) : 0;
    recomp_func_t fn;

    fn = recomp_lookup(apc_context);
    if (!fn) fn = recomp_lookup_manual(apc_context);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] NtUserIoApcDispatcher: completion routine "
                "0x%08X not in dispatch\n", apc_context);
        fflush(stderr);
        g_eax = 0;
        return;
    }

    /* __stdcall, right-to-left. The callee's `ret 12` consumes the dummy
     * return address and all three arguments, so g_esp needs no fixup here. */
    g_esp -= 4; BRIDGE_MEM32(g_esp) = iostatus;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = information;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = (status == 0) ? 0 : status;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    fn();

    g_eax = 0;
}

/* ── KeDelayExecutionThread (ordinal 99) ─────────────────── */
/* Unbridged this returned instantly, turning every "sleep and retry" in the
 * title into a hot spin. Halo's cache-partition setup retries this way. */
static void bridge_KeDelayExecutionThread(void)
{
    uint32_t wait_mode    = STACK_ARG(0);
    uint32_t alertable    = STACK_ARG(1);
    uint32_t interval_ptr = STACK_ARG(2);


    g_eax = (uint32_t)xbox_KeDelayExecutionThread(
        (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(interval_ptr));
}

/* ── KeBugCheck (ordinal 95) / KeBugCheckEx (96) ─────────── */
/*
 * The title asking the kernel to die. Unbridged this returned 0 and execution
 * carried on into whatever the bug check was there to prevent, so the real
 * failure surfaced later somewhere unrelated. Report the code and stop
 * pretending the call succeeded.
 */
static void bridge_KeBugCheck(void)
{
    fprintf(stderr, "  [KERNEL] *** KeBugCheck: code=0x%08X ***\n",
            STACK_ARG(0));
    fflush(stderr);
    g_eax = 0;
}

static void bridge_KeBugCheckEx(void)
{
    fprintf(stderr, "  [KERNEL] *** KeBugCheckEx: code=0x%08X "
            "(0x%08X, 0x%08X, 0x%08X, 0x%08X) ***\n",
            STACK_ARG(0), STACK_ARG(1), STACK_ARG(2),
            STACK_ARG(3), STACK_ARG(4));
    fflush(stderr);
    g_eax = 0;
}

/* ── NtYieldExecution (ordinal 238) ──────────────────────── */
static void bridge_NtYieldExecution(void)
{
    g_eax = (uint32_t)xbox_NtYieldExecution();
}

/* ── MmGetPhysicalAddress (ordinal 173) ──────────────────── */
static void bridge_MmGetPhysicalAddress(void)
{
    uint32_t addr = STACK_ARG(0);
    /* Xbox uses identity mapping (physical == virtual) for the lower 64MB.
     * Just return the Xbox VA as-is. Don't call xbox_MmGetPhysicalAddress
     * which would return a native pointer. */
    g_eax = addr;
}

/* ── MmSetAddressProtect (ordinal 182) ───────────────────── */
static void bridge_MmSetAddressProtect(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t size = STACK_ARG(1);
    uint32_t prot = STACK_ARG(2);

    xbox_MmSetAddressProtect(XBOX_TO_NATIVE(addr), size, prot);
    g_eax = 0;
}

/* ── AvSetDisplayMode (ordinal 3) ────────────────────────── */
static void bridge_AvSetDisplayMode(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t step = STACK_ARG(1);
    uint32_t mode = STACK_ARG(2);
    uint32_t format = STACK_ARG(3);
    uint32_t pitch = STACK_ARG(4);
    uint32_t fb = STACK_ARG(5);

    /* The framebuffer the display is meant to scan out, and the format it is
     * in. This is the only place the address is stated: the title never writes
     * PCRTC_START itself, so without this there is nothing that says where the
     * guest believes its picture is. */
    fprintf(stderr, "  [AV] SetDisplayMode mode=0x%08X format=0x%08X"
                    " pitch=%u fb=0x%08X\n", mode, format, pitch, fb);
    fflush(stderr);

    {
        /* Point the framebuffer window at whatever the title just set, and
         * start it on the first display mode -- before that there is nothing
         * to show and no pitch to interpret it with. */
        extern void xbox_FramebufferWindowSet(uint32_t, uint32_t);
        extern void xbox_FramebufferWindowStart(void);
        uint32_t fb_va = fb;

        /* AvSetDisplayMode reports the scanout address the way the CRTC wants
         * it -- a physical address. The window has to read it the way the CPU
         * sees it. Where the title allocated says which: a framebuffer from
         * MmAllocateContiguousMemory lives in the window at XBOX_CONTIG_BASE,
         * so physical P is visible at XBOX_CONTIG_BASE + P. Reading P
         * directly lands in the loaded image instead, which is why the window
         * showed black while the executor was clearing and rasterising
         * correctly a few megabytes away. */
        if (fb_va && fb_va < XBOX_CONTIG_SIZE)
            fb_va = XBOX_CONTIG_BASE + fb_va;

        xbox_FramebufferWindowSet(fb_va, pitch);
        xbox_FramebufferWindowStart();

        /* Record the resolved address, not the physical one the title passed.
         * Every reader of this wants to read guest memory with it, and the
         * physical form lands in the loaded image -- so the checksum probe
         * reported an unchanging zero while the title was drawing correctly a
         * few megabytes away, and so did every dump that asked for "the"
         * framebuffer. Resolving once here is the fix; resolving in each
         * caller is how there came to be two of them disagreeing. */
        xbox_SetDisplayFramebuffer(fb_va, pitch);
    }
    xbox_AvSetDisplayMode(XBOX_TO_NATIVE(addr), step, mode, format, pitch, fb);
    g_eax = 0;
}

/* ── PsTerminateSystemThread (ordinal 258) ───────────────
 * VOID PsTerminateSystemThread(NTSTATUS ExitStatus)
 *
 * On real Xbox, this terminates the calling thread (never returns).
 * In our recompiled version, threads run synchronously, so we just
 * return. The caller (sub_001D1818) handles this gracefully.
 */
static void bridge_PsTerminateSystemThread(void)
{
    uint32_t exit_status = STACK_ARG(0);

    fprintf(stderr, "  [KERNEL] PsTerminateSystemThread: status=0x%08X%s\n",
            exit_status, g_is_spawned_thread ? " (worker)" : " (main)");
    fflush(stderr);

    g_eax = exit_status;

    /*
     * This does not return on hardware. Returning was survivable while every
     * thread ran on the host's main thread, but a spawned worker that returns
     * here falls off the end of its start routine and into whatever bytes
     * follow -- Halo's input worker landed on an int 3, and the resulting
     * breakpoint took down the whole process while the main thread was still
     * inside input_initialize.
     *
     * The main thread still returns: it is the host's thread and unwinding
     * back to main() is how the process shuts down cleanly.
     */
    if (g_is_spawned_thread) {
        /* The normal exit for a worker, and therefore the one that has to
         * return the stack -- ExitThread never comes back to bridge_thread_main
         * to do it. */
        xbox_FreeThreadStack(g_thread_stack_top);
        g_thread_stack_top = 0;
        ExitThread(exit_status);
    }
}

/* ── HalReadSMCTrayState (ordinal 47) ─────────────────────
 * VOID HalReadSMCTrayState(PDWORD TrayState, PDWORD TrayStateChangeCount)
 *
 * Returns DVD tray state. 0x10 = no disc, 0x14 = tray closed with disc.
 */
static void bridge_HalReadSMCTrayState(void)
{
    uint32_t state_ptr = STACK_ARG(0);
    uint32_t count_ptr = STACK_ARG(1);

    if (state_ptr) BRIDGE_MEM32(state_ptr) = 0x10;  /* No disc */
    if (count_ptr) BRIDGE_MEM32(count_ptr) = 0;
    g_eax = 0;
}

/* -- KeInsertQueueDpc (ordinal 119) ------------------------
 * BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1,
 *                          PVOID SystemArgument2)
 *
 * Unbridged this returned 0, which reads to a driver as "already queued" and
 * means the deferred routine never runs. That is not a small loss: an
 * interrupt service routine is expected to do almost nothing except mask the
 * source and queue a DPC, so with this missing every driver that follows the
 * normal pattern acknowledges its interrupt and then does none of the work.
 * Half-Life 2's USB stack does exactly that -- its ISR masks the master
 * interrupt enable and queues here, and the enumeration it should have started
 * lives entirely in the deferred routine.
 *
 * ponytail: runs the routine inline rather than queueing it. A real DPC runs
 * at DISPATCH_LEVEL shortly after the ISR returns, and this runs it before the
 * ISR returns, on whichever thread queued it. That ordering difference has not
 * mattered for anything here yet; when it does, the upgrade is a real queue
 * drained by the thread that lowered IRQL, not a second call site.
 */
/* Run a DPC's deferred routine on the calling thread.
 *
 * VOID DeferredRoutine(PKDPC, PVOID Context, PVOID Arg1, PVOID Arg2),
 * __stdcall: its `ret 16` consumes the dummy return address and all four
 * arguments, so g_esp needs no fixup afterwards. The caller must already have
 * a guest stack -- true on a guest thread, and true on a host thread that has
 * taken a worker slice.
 *
 * Returns 1 if the routine was found and called.
 */
static int kernel_run_dpc(uint32_t dpc_va, uint32_t arg1, uint32_t arg2)
{
    uint32_t routine, context;
    recomp_func_t fn;

    if (!dpc_va)
        return 0;
    routine = BRIDGE_MEM32(dpc_va + 12);
    context = BRIDGE_MEM32(dpc_va + 16);
    if (!routine)
        return 0;

    fn = recomp_lookup(routine);
    if (!fn) fn = recomp_lookup_manual(routine);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] DPC routine 0x%08X not in dispatch\n",
                routine);
        fflush(stderr);
        return 0;
    }

    BRIDGE_MEM32(dpc_va + 20) = arg1;
    BRIDGE_MEM32(dpc_va + 24) = arg2;

    g_esp -= 4; BRIDGE_MEM32(g_esp) = arg2;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = arg1;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = dpc_va;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    fn();
    return 1;
}

/* -- KeSynchronizeExecution (ordinal 153) ------------------
 * BOOLEAN KeSynchronizeExecution(PKINTERRUPT Interrupt,
 *                                PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
 *                                PVOID SynchronizeContext)
 *
 * Runs a routine while holding the interrupt's spinlock at the ISR's IRQL,
 * which is how a driver touches its hardware from anywhere that is not the
 * ISR without racing the ISR. Unbridged it returned 0 and the routine never
 * ran -- the same failure as an unqueued DPC, and just as quiet: the driver
 * asks for exclusive access, is told it did not get it, and skips the work.
 *
 * ponytail: no lock is taken. Nothing else here runs at ISR IRQL, and the one
 * caller that matters is a device model on its own thread; if two of those
 * ever contend, this wants the interrupt object's own lock rather than a
 * global one.
 */
static void bridge_KeSynchronizeExecution(void)
{
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);
    recomp_func_t fn;

    fn = routine ? recomp_lookup(routine) : NULL;
    if (!fn && routine) fn = recomp_lookup_manual(routine);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] KeSynchronizeExecution: routine 0x%08X "
                        "not in dispatch\n", routine);
        fflush(stderr);
        g_eax = 0;
        return;
    }

    /* BOOLEAN SynchronizeRoutine(PVOID Context), __stdcall. Its `ret 4` takes
     * the dummy return address and the argument, so g_esp needs no fixup. */
    g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    fn();
    /* g_eax is whatever the routine returned, which is this call's result. */
}

/* -- KeRemoveQueueDpc (ordinal 137) ------------------------
 * BOOLEAN KeRemoveQueueDpc(PKDPC Dpc)
 *
 * Cancels a queued DPC, returning whether it was still in the queue. DPCs run
 * inline here (see KeInsertQueueDpc), so by the time anyone can call this the
 * routine has already run and there is nothing to cancel. FALSE is both the
 * honest answer and the one that keeps a caller's bookkeeping right.
 */
static void bridge_KeRemoveQueueDpc(void)
{
    g_eax = 0;
}

/* The pending DPC queue.
 *
 * These used to run inline, on whichever thread queued them, before the caller
 * returned. That is not what a DPC is: an interrupt service routine masks its
 * source and queues, and the deferred routine runs afterwards at DISPATCH_LEVEL
 * -- after the ISR has returned. Running it inline inverts that, so a routine
 * that re-enables interrupts does it while the ISR that masked them is still on
 * the stack, and a driver's state machine re-enters itself from inside its own
 * interrupt.
 *
 * Queued properly now, and drained by the timer thread, which is the one thread
 * here that already has a guest stack and a TIB and runs nothing else urgent.
 *
 * ponytail: one queue, no IRQL, no per-processor list, and a DPC queued from a
 * DPC runs on the next drain rather than immediately. Nothing here depends on
 * DPC ordering beyond "after the ISR".
 */
#define XBOX_MAX_PENDING_DPC 64
typedef struct { uint32_t dpc, arg1, arg2; } PendingDpc;
static PendingDpc g_dpc_queue[XBOX_MAX_PENDING_DPC];
static volatile LONG g_dpc_head, g_dpc_tail;

static void bridge_KeInsertQueueDpc(void)
{
    uint32_t dpc  = STACK_ARG(0);
    uint32_t arg1 = STACK_ARG(1);
    uint32_t arg2 = STACK_ARG(2);
    LONG tail, next;

    if (!dpc) { g_eax = 0; return; }

    tail = g_dpc_tail;
    next = (tail + 1) % XBOX_MAX_PENDING_DPC;
    if (next == g_dpc_head) {
        fprintf(stderr, "  [KERNEL] DPC queue full, dropping 0x%08X\n", dpc);
        fflush(stderr);
        g_eax = 0;
        return;
    }
    g_dpc_queue[tail].dpc  = dpc;
    g_dpc_queue[tail].arg1 = arg1;
    g_dpc_queue[tail].arg2 = arg2;
    g_dpc_tail = next;
    g_eax = 1;
}

/* Call a connected interrupt service routine.
 *
 * BOOLEAN ServiceRoutine(PKINTERRUPT, PVOID ServiceContext), __stdcall: its
 * `ret 8` takes the dummy return address and both arguments, so g_esp needs no
 * fixup. The caller must already have a guest stack and a TIB, which the timer
 * thread has.
 *
 * Returns what the routine returned -- an ISR that does not recognise the
 * interrupt returns FALSE, and that is worth seeing rather than assuming.
 */
uint32_t xbox_GetConnectedInterrupt(uint32_t vector);   /* defined below */

static int kernel_raise_interrupt(uint32_t vector)
{
    uint32_t kint = xbox_GetConnectedInterrupt(vector);
    uint32_t routine, context;
    recomp_func_t fn;

    if (!kint)
        return -1;
    routine = BRIDGE_MEM32(kint + 0);
    context = BRIDGE_MEM32(kint + 4);
    if (!routine)
        return -1;
    fn = recomp_lookup(routine);
    if (!fn) fn = recomp_lookup_manual(routine);
    if (!fn)
        return -1;

    g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = kint;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    fn();
    return (int)(g_eax & 1u);
}

/* The GPU's vertical blank, delivered rather than merely enabled.
 *
 * The D3D8 library linked into a title installs an ISR for this and then waits
 * on it. Nothing ever raised it, so a title whose frame loop waits for vblank
 * rather than polling stops after its first clear -- which is exactly where
 * Half-Life 2's loader stops, with its videos open, its 23 MB of UI textures
 * loaded, and no second frame.
 *
 * The ISR reads the NV2A's own interrupt status to decide whether the
 * interrupt is its business, so the registers have to say vblank before the
 * routine is called: PCRTC_INTR_0 bit 0 for the vblank itself, and PMC_INTR_0
 * bit 24 to say the PCRTC block is the source. Without those the handler looks,
 * finds nothing, and correctly declines.
 *
 * ponytail: a fixed 60 Hz off the timer tick rather than anything tied to the
 * display mode, and no field or interlace handling. A title that measures
 * refresh rate from this will read 60; a title that needs the real one wants
 * the mode AvSetDisplayMode was given, which is recorded a few files away.
 */
#define XBOX_NV2A_REG_BASE     0xFD000000u
#define NV2A_PMC_INTR_0        0x00000100u
#define NV2A_PMC_INTR_PCRTC    (1u << 24)
#define NV2A_PCRTC_INTR_0      0x00600100u
#define NV2A_PCRTC_INTR_VBLANK (1u << 0)
#define NV2A_VECTOR            3u

static void kernel_vblank_tick(void)
{
    static int enabled = -1;
    static long long next_ms;
    long long now;

    if (enabled < 0)
        enabled = getenv("RECOMP_VBLANK") != NULL;
    if (!enabled)
        return;

    now = (long long)GetTickCount64();
    if (now < next_ms)
        return;
    next_ms = now + 16;                       /* ~60 Hz */

    if (!xbox_GetConnectedInterrupt(NV2A_VECTOR))
        return;

    BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PCRTC_INTR_0) |= NV2A_PCRTC_INTR_VBLANK;
    BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PMC_INTR_0)   |= NV2A_PMC_INTR_PCRTC;

    {
        static unsigned n;
        int claimed = kernel_raise_interrupt(NV2A_VECTOR);
        if (n++ < 3)
            fprintf(stderr, "  [NV2A] vblank -> ISR %s\n",
                    claimed < 0 ? "not callable" :
                    claimed ? "claimed it" : "declined it");
        fflush(stderr);
    }
}

/* Run whatever is queued. Called from the timer thread, which has the guest
 * stack and TIB that a deferred routine needs. */
static void kernel_drain_dpcs(void)
{
    while (g_dpc_head != g_dpc_tail) {
        LONG head = g_dpc_head;
        PendingDpc d = g_dpc_queue[head];
        g_dpc_head = (head + 1) % XBOX_MAX_PENDING_DPC;
        kernel_run_dpc(d.dpc, d.arg1, d.arg2);
    }
}

/* ── KeInitializeDpc (ordinal 107) ────────────────────────
 * VOID KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine,
 *                       PVOID DeferredContext)
 *
 * Initializes a DPC object. The Xbox KDPC structure is 32 bytes.
 * We zero it and set the routine and context pointers.
 */
static void bridge_KeInitializeDpc(void)
{
    uint32_t dpc_va = STACK_ARG(0);
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);

    /* Zero the structure (32 bytes) */
    memset(XBOX_TO_NATIVE(dpc_va), 0, 32);

    /* Set Type (0x13 = DpcObject) and fields */
    BRIDGE_MEM16(dpc_va + 0) = 0x13;   /* Type */
    BRIDGE_MEM32(dpc_va + 12) = routine; /* DeferredRoutine */
    BRIDGE_MEM32(dpc_va + 16) = context; /* DeferredContext */
    g_eax = 0;
}

/* ── NV2A interrupt plumbing (ordinals 44, 98, 109) ───────
 *
 * The D3D8 library linked into a title installs an ISR for the GPU's vblank /
 * command-completion interrupt. There is no NV2A here and nothing ever raises
 * that interrupt, so these exist to let initialisation complete rather than to
 * deliver anything.
 *
 * KeConnectInterrupt reports success: reporting failure sends Halo's
 * rasterizer down an error path during preinitialize, and the goal is to get
 * past setup, not to pretend the hardware is broken.
 *
 * ponytail: no interrupt is ever delivered. Code that *waits* on the ISR
 * rather than polling will hang here, and the fix for that is to bridge the
 * D3D8 entry point that owns the wait, not to synthesise NV2A interrupts.
 */

/* ULONG HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql) */
static void bridge_HalGetInterruptVector(void)
{
    uint32_t level   = STACK_ARG(0);
    uint32_t irql_va = STACK_ARG(1);

    if (irql_va) {
        /* IRQL is conventionally the vector for device interrupts. */
        BRIDGE_MEM8(irql_va) = (uint8_t)level;
    }
    g_eax = level;
}

/* VOID KeInitializeInterrupt(PKINTERRUPT, ServiceRoutine, ServiceContext,
 *                            Vector, Irql, InterruptMode, ShareVector) */
static void bridge_KeInitializeInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    uint32_t routine      = STACK_ARG(1);
    uint32_t context      = STACK_ARG(2);
    uint32_t vector       = STACK_ARG(3);

    /* Xbox KINTERRUPT is 44 bytes. */
    memset(XBOX_TO_NATIVE(interrupt_va), 0, 44);
    BRIDGE_MEM32(interrupt_va + 0)  = routine;
    BRIDGE_MEM32(interrupt_va + 4)  = context;
    BRIDGE_MEM32(interrupt_va + 8)  = vector;
    g_eax = 0;
}

/* The interrupt objects a title has connected, by vector.
 *
 * KeInitializeInterrupt already writes the service routine, its context and
 * the vector into the guest KINTERRUPT; connecting is what says the title is
 * ready to be called on it. Recording that is what lets a device model raise
 * an interrupt at all -- without it the routine is written down in guest
 * memory and nothing on this side knows it is there.
 *
 * A small fixed table rather than a list: an Xbox has 26 interrupt vectors and
 * a title connects a handful, so the whole thing is smaller than the comment.
 */
#define XBOX_MAX_VECTORS 32
static uint32_t g_connected_isr[XBOX_MAX_VECTORS];   /* KINTERRUPT guest VA */

/* BOOLEAN KeConnectInterrupt(PKINTERRUPT Interrupt) */
static void bridge_KeConnectInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);

    if (interrupt_va) {
        uint32_t vector = BRIDGE_MEM32(interrupt_va + 8);
        if (vector < XBOX_MAX_VECTORS) {
            g_connected_isr[vector] = interrupt_va;
            fprintf(stderr, "  [KERNEL] KeConnectInterrupt: vector %u -> "
                            "routine 0x%08X context 0x%08X\n",
                    vector, BRIDGE_MEM32(interrupt_va + 0),
                    BRIDGE_MEM32(interrupt_va + 4));
            fflush(stderr);
        }
    }
    g_eax = 1;  /* connected -- see the note above */
}

/* The KINTERRUPT a title connected on this vector, or 0. Device models use it
 * to find the routine to call; the routine and its context are at +0 and +4. */
uint32_t xbox_GetConnectedInterrupt(uint32_t vector)
{
    return (vector < XBOX_MAX_VECTORS) ? g_connected_isr[vector] : 0;
}

/* ── MmClaimGpuInstanceMemory (ordinal 168) ───────────────
 * PVOID MmClaimGpuInstanceMemory(SIZE_T NumberOfBytes, SIZE_T *Padding)
 *
 * Reserves the GPU instance memory the NV2A keeps its object context in. On
 * hardware it sits at the very top of physical RAM, so the returned address is
 * the end of the contiguous window minus the request. D3D8 stores this and
 * indexes off it, so returning 0 (the unbridged default) had it building
 * pointers from a null base.
 *
 * MAXULONG_PTR means "claim everything left"; the console answers with the
 * default instance size rather than the whole of RAM.
 */
static void bridge_MmClaimGpuInstanceMemory(void)
{
    uint32_t bytes      = STACK_ARG(0);
    uint32_t padding_va = STACK_ARG(1);

    if (bytes == 0xFFFFFFFFu) {
        bytes = XBOX_GPU_INSTANCE_DEFAULT;
    }
    if (padding_va) {
        BRIDGE_MEM32(padding_va) = 0;
    }
    g_eax = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE - bytes;
}

/* VOID HalRegisterShutdownNotification(PHAL_SHUTDOWN_REGISTRATION, BOOLEAN)
 * Records a callback for console shutdown. Nothing here ever shuts down that
 * way, so registration is accepted and dropped. */
static void bridge_HalRegisterShutdownNotification(void)
{
    g_eax = 0;
}

/* ── Kernel timers (ordinals 113, 149, 150, 97) ───────────
 *
 * Timers are waitable: KeInitializeTimerEx backs every guest KTIMER with a
 * Win32 event in the shadow table (manual reset for notification timers, auto
 * reset for synchronization timers), so a KeWaitForSingleObject on a timer
 * sleeps on a real object. KeSetTimer/KeSetTimerEx arm the polling table below
 * whose thread fires DPCs and signals that event; KeCancelTimer clears both. */

/* ── KeInitializeTimerEx (ordinal 113) ────────────────────
 * VOID KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type)
 */
static void bridge_KeInitializeTimerEx(void)
{
    uint32_t timer_va = STACK_ARG(0);
    uint32_t type     = STACK_ARG(1);
    HANDLE   ev;

    if (!timer_va) {
        g_eax = 0;
        return;
    }

    memset(XBOX_TO_NATIVE(timer_va), 0, 40);

    /* Dispatcher header. 0x08 = notification timer, 0x09 = synchronization. */
    BRIDGE_MEM8(timer_va + 0)  = (uint8_t)(0x08 + (type & 1));
    BRIDGE_MEM8(timer_va + 2)  = 40;      /* Size */
    BRIDGE_MEM8(timer_va + 3)  = 0;       /* Inserted */
    BRIDGE_MEM32(timer_va + 4) = 0;       /* SignalState */
    BRIDGE_MEM32(timer_va + 8) = 0;       /* WaitListHead */
    BRIDGE_MEM32(timer_va + 12) = 0;

    ev = CreateEventW(NULL, (type == 0) ? TRUE : FALSE, FALSE, NULL);
    if (!ev) {
        fprintf(stderr, "  [KERNEL] KeInitializeTimerEx: CreateEventW failed "
                        "(error %u)\n", GetLastError());
        g_eax = 0;
        return;
    }
    ke_shadow_insert(timer_va, ev);
    g_eax = 0;
}

/* -- KeSetTimer / KeSetTimerEx (ordinal 149/150) ----------
 * BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)
 * BOOLEAN KeSetTimerEx(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period,
 *                      PKDPC Dpc)
 *
 * These used to return FALSE and do nothing, on the grounds that timers were
 * not needed for basic execution. They are needed for more than that: a driver
 * that polls its hardware does it from a timer DPC, and one that never fires
 * is a state machine that never advances. Half-Life 2's USB stack sets one up
 * during XInitDevices and enumerates from it.
 *
 * A due time is in 100 ns units, negative for relative and positive for an
 * absolute time since 1601. Only the relative form is honoured here; an
 * absolute due time is treated as immediate, which is wrong in principle and
 * has not come up in practice.
 *
 * ponytail: one thread, a fixed table, and a 10 ms tick, so a due time is late
 * by up to a tick and a periodic timer drifts. Nothing here is scheduling
 * audio off a timer. A title that needs better wants the host's timer queue,
 * not a smaller sleep.
 */
#define XBOX_MAX_TIMERS 32
typedef struct {
    uint32_t timer_va;      /* the guest KTIMER, 0 for a free slot */
    uint32_t dpc_va;
    long long due_ms;       /* host tick when it fires */
    long      period_ms;    /* 0 for one-shot */
} XboxTimer;
static XboxTimer g_timers[XBOX_MAX_TIMERS];
static CRITICAL_SECTION g_timer_lock;
static int g_timer_started;

static DWORD WINAPI kernel_timer_thread(LPVOID unused)
{
    int slot = xbox_worker_stack_alloc();

    (void)unused;
    if (slot < 0) {
        fprintf(stderr, "  [KERNEL] timer thread has no worker stack; "
                        "timer DPCs will not run\n");
        fflush(stderr);
        return 0;
    }
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    {
        /* Its own TIB, for the same reason bridge_thread_main gives one to
         * every worker: a DPC routine with an SEH prologue reads fs:[0],
         * and a thread whose g_fs_base is zero faults inside the runtime
         * before the routine runs. */
        uint32_t tib = xbox_AllocThreadTib();
        if (!tib) {
            fprintf(stderr, "  [KERNEL] timer thread has no TIB; "
                            "timer DPCs will not run\n");
            fflush(stderr);
            return 0;
        }
        g_fs_base = tib;
    }

    for (;;) {
        long long now;
        int i;

        Sleep(10);
        kernel_vblank_tick();  /* the GPU's frame clock */
        kernel_drain_dpcs();   /* deferred work, before due timers */
        now = (long long)GetTickCount64();

        for (i = 0; i < XBOX_MAX_TIMERS; i++) {
            uint32_t dpc, fired_va;

            EnterCriticalSection(&g_timer_lock);
            if (!g_timers[i].timer_va || now < g_timers[i].due_ms) {
                LeaveCriticalSection(&g_timer_lock);
                continue;
            }
            fired_va = g_timers[i].timer_va;
            dpc = g_timers[i].dpc_va;
            if (g_timers[i].period_ms > 0)
                g_timers[i].due_ms = now + g_timers[i].period_ms;
            else
                g_timers[i].timer_va = 0;      /* one-shot, done */
            LeaveCriticalSection(&g_timer_lock);

            /* Outside the lock: the routine can set or cancel timers. */
            if (dpc)
                kernel_run_dpc(dpc, 0, 0);

            /* Wake anyone parked on the timer's shadow event; a timer with no
             * DPC is just a kernel sleep. */
            {
                HANDLE ev = ke_shadow_lookup(fired_va);
                if (ev) {
                    SetEvent(ev);
                    BRIDGE_MEM32(fired_va + 4) = 1;   /* SignalState */
                }
            }
        }
    }
}

/* Shared by KeSetTimer and KeSetTimerEx; period is 0 for the former. */
static void kernel_set_timer(uint32_t timer_va, long long due_100ns,
                             long period_ms, uint32_t dpc_va)
{
    long long delay_ms = (due_100ns < 0) ? (-due_100ns) / 10000 : 0;
    int i, free_slot = -1;
    uint32_t was_set = 0;

    if (!g_timer_started) {
        InitializeCriticalSection(&g_timer_lock);
        g_timer_started = 1;
        CloseHandle(CreateThread(NULL, 0, kernel_timer_thread, NULL, 0, NULL));
    }

    EnterCriticalSection(&g_timer_lock);
    for (i = 0; i < XBOX_MAX_TIMERS; i++) {
        if (g_timers[i].timer_va == timer_va) { free_slot = i; was_set = 1; break; }
        if (!g_timers[i].timer_va && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        g_timers[free_slot].timer_va  = timer_va;
        g_timers[free_slot].dpc_va    = dpc_va;
        g_timers[free_slot].due_ms    = (long long)GetTickCount64() + delay_ms;
        g_timers[free_slot].period_ms = period_ms;
    }
    LeaveCriticalSection(&g_timer_lock);

    /* A freshly set timer starts unsignaled, like the real KeSetTimer. */
    {
        HANDLE ev = ke_shadow_lookup(timer_va);
        if (ev)
            ResetEvent(ev);
    }
    g_eax = was_set;
}

static void bridge_KeSetTimer(void)
{
/* LARGE_INTEGER is two stack slots. */
    long long due = (long long)((uint64_t)STACK_ARG(1)
                              | ((uint64_t)STACK_ARG(2) << 32));
    kernel_set_timer(STACK_ARG(0), due, 0, STACK_ARG(3));
}

static void bridge_KeSetTimerEx(void)
{
    long long due = (long long)((uint64_t)STACK_ARG(1)
                              | ((uint64_t)STACK_ARG(2) << 32));
    kernel_set_timer(STACK_ARG(0), due, (long)STACK_ARG(3), STACK_ARG(4));
}

/* Drop a timer so it stops firing. Returns whether it was armed. */
int xbox_kernel_cancel_timer(uint32_t timer_va)
{
    int i, was_set = 0;

    if (!g_timer_started)
        return 0;
    EnterCriticalSection(&g_timer_lock);
    for (i = 0; i < XBOX_MAX_TIMERS; i++)
        if (g_timers[i].timer_va == timer_va) {
            g_timers[i].timer_va = 0;
            was_set = 1;
        }
    LeaveCriticalSection(&g_timer_lock);
    return was_set;
}

/* ── ExQueryPoolBlockSize (ordinal 24) ────────────────────
 * ULONG ExQueryPoolBlockSize(PVOID PoolBlock)
 *
 * Returns the size of a pool memory block.
 * Since we use HeapAlloc, we can query the Windows heap.
 */
static void bridge_ExQueryPoolBlockSize(void)
{
    /* Pool blocks come from xbox_HeapAlloc, so the block table has the real
     * answer. It used to return a literal 0 on the theory that this is only
     * ever used for stats -- which is a guess about the caller, and a title
     * that sizes a copy from it copies nothing. */
    g_eax = xbox_HeapBlockSize(STACK_ARG(0));
}

/* ── RtlNtStatusToDosError (ordinal 301) ─────────────────
 * ULONG RtlNtStatusToDosError(NTSTATUS Status)
 *
 * Converts an NTSTATUS to a Win32 error code.
 */
static void bridge_RtlNtStatusToDosError(void)
{
    uint32_t status = STACK_ARG(0);

    /* Simple mapping of common status codes */
    switch (status) {
    case 0x00000000: g_eax = 0; break;          /* STATUS_SUCCESS → ERROR_SUCCESS */
    case 0xC0000034: g_eax = 2; break;          /* STATUS_OBJECT_NAME_NOT_FOUND → ERROR_FILE_NOT_FOUND */
    case 0xC000003A: g_eax = 3; break;          /* STATUS_OBJECT_PATH_NOT_FOUND → ERROR_PATH_NOT_FOUND */
    case 0xC0000022: g_eax = 5; break;          /* STATUS_ACCESS_DENIED → ERROR_ACCESS_DENIED */
    case 0xC0000008: g_eax = 6; break;          /* STATUS_INVALID_HANDLE → ERROR_INVALID_HANDLE */
    case 0xC0000017: g_eax = 8; break;          /* STATUS_NO_MEMORY → ERROR_NOT_ENOUGH_MEMORY */
    case 0xC000000D: g_eax = 87; break;         /* STATUS_INVALID_PARAMETER → ERROR_INVALID_PARAMETER */
    default:         g_eax = 317; break;         /* ERROR_MR_MID_NOT_FOUND (generic) */
    }
}

/* ── File I/O bridge helpers ─────────────────────────────── */

/*
 * Xbox structures use 32-bit pointers. On Win64, the C structs
 * (XBOX_OBJECT_ATTRIBUTES, etc.) have 64-bit pointers, so we can't
 * cast Xbox memory to them directly. Instead, parse the 32-bit
 * Xbox layout manually:
 *
 * XBOX_OBJECT_ATTRIBUTES (12 bytes):
 *   offset 0: RootDirectory  (uint32_t)
 *   offset 4: ObjectName     (uint32_t, Xbox VA to ANSI_STRING)
 *   offset 8: Attributes     (uint32_t)
 *
 * XBOX_ANSI_STRING (8 bytes):
 *   offset 0: Length          (uint16_t)
 *   offset 2: MaximumLength   (uint16_t)
 *   offset 4: Buffer          (uint32_t, Xbox VA to char[])
 *
 * XBOX_IO_STATUS_BLOCK (8 bytes):
 *   offset 0: Status          (uint32_t)
 *   offset 4: Information     (uint32_t)
 */

/* Extract the ANSI path string from an Xbox OBJECT_ATTRIBUTES */
static const char* bridge_get_xbox_path(uint32_t obj_attrs_va)
{
    uint32_t ansi_str_va, buf_va;
    if (!obj_attrs_va) return NULL;
    ansi_str_va = BRIDGE_MEM32(obj_attrs_va + 4);
    if (!ansi_str_va) return NULL;
    buf_va = BRIDGE_MEM32(ansi_str_va + 4);
    if (!buf_va) return NULL;
    return (const char*)XBOX_TO_NATIVE(buf_va);
}

/* Write NTSTATUS + Information into Xbox IO_STATUS_BLOCK */
static void bridge_write_iostatus(uint32_t ios_va, NTSTATUS status, uint32_t info)
{
    if (ios_va) {
        BRIDGE_MEM32(ios_va + 0) = (uint32_t)status;
        BRIDGE_MEM32(ios_va + 4) = info;
    }
}

/*
 * Handle table.
 *
 * Xbox memory only has 32-bit handle slots, but native HANDLEs are 64-bit
 * pointers (win32_compat objects, or real Win32 handles on Windows). Map
 * 32-bit tokens <-> native HANDLEs so a handle survives a round-trip through
 * Xbox memory. Tokens carry a tag in the high byte so they never collide
 * with the synthetic handles (0xDEAD0001 / 0xBEEF0010) used elsewhere.
 */
#define BRIDGE_HANDLE_TAG  0x48000000u
#define BRIDGE_HANDLE_MASK 0x00FFFFFFu
#define BRIDGE_HANDLE_MAX  16384
static HANDLE s_handle_table[BRIDGE_HANDLE_MAX];

static uint32_t bridge_handle_token(HANDLE h)
{
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == h) return BRIDGE_HANDLE_TAG | (uint32_t)i;
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == NULL) {
            s_handle_table[i] = h;
            return BRIDGE_HANDLE_TAG | (uint32_t)i;
        }
    fprintf(stderr, "  [BRIDGE] handle table full\n");
    return 0;
}

/* Store a native HANDLE into a 32-bit Xbox memory slot (as a token). */
static void bridge_write_handle(uint32_t handle_va, HANDLE h)
{
    if (handle_va)
        BRIDGE_MEM32(handle_va) = bridge_handle_token(h);
}

/* Resolve a 32-bit Xbox handle slot back to a native HANDLE. */
static HANDLE bridge_read_handle(uint32_t va)
{
    uint32_t token = BRIDGE_MEM32(va);
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }
    /* Untagged value: synthetic/dummy handle -- pass through unchanged. */
    return (HANDLE)(uintptr_t)token;
}

/* Resolve a token to a HANDLE and release its table slot (for NtClose). */
/* Resolve a handle token passed BY VALUE, without consuming it.
 *
 * Three accessors, easily confused, and confusing two of them broke all file
 * I/O: bridge_read_handle(va) reads a token *from memory* and suits a PHANDLE
 * out-parameter; bridge_take_handle(token) resolves and CLEARS the table slot,
 * which is NtClose semantics; this one resolves and leaves the slot alone,
 * which is what every by-value HANDLE argument needs.
 *
 * NtSetInformationFile and friends take the handle by value, but were calling
 * bridge_read_handle on it -- dereferencing the token as if it were an address.
 * Halo created its save file successfully and then failed the very next call,
 * which surfaced as "couldn't open or create saved game file". */
static HANDLE bridge_resolve_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }
    /* Untagged: synthetic/dummy handle -- pass through unchanged. */
    return (HANDLE)(uintptr_t)token;
}

static HANDLE bridge_take_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        if (i > 0 && i < BRIDGE_HANDLE_MAX) {
            HANDLE h = s_handle_table[i];
            s_handle_table[i] = NULL;
            return h;
        }
    }
    return NULL;   /* untagged -> not a table handle, do not close */
}

/* Build a native OBJECT_ATTRIBUTES wrapping the translated Xbox path. */
static void bridge_build_oa(uint32_t obj_attrs_va,
                            XBOX_OBJECT_ATTRIBUTES* oa, XBOX_ANSI_STRING* name)
{
    const char* path = bridge_get_xbox_path(obj_attrs_va);
    name->Buffer        = (PCHAR)path;
    name->Length        = path ? (USHORT)strlen(path) : 0;
    name->MaximumLength = (USHORT)(name->Length + 1);
    oa->RootDirectory = NULL;
    oa->ObjectName    = name;
    oa->Attributes    = 0;
}

/* Open a file by delegating to the ported xbox_NtCreateFile kernel HLE. */
static NTSTATUS bridge_create_file_impl(
    uint32_t handle_va, ACCESS_MASK access, uint32_t obj_attrs_va,
    uint32_t iostatus_va, ULONG file_attrs, ULONG share,
    ULONG disposition, ULONG options)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;
    XBOX_IO_STATUS_BLOCK   ios;
    HANDLE   h  = NULL;
    NTSTATUS st;

    bridge_build_oa(obj_attrs_va, &oa, &name);
    if (!name.Buffer) {
        bridge_write_iostatus(iostatus_va, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    memset(&ios, 0, sizeof(ios));

    st = xbox_NtCreateFile(&h, access, &oa, &ios, NULL,
                           file_attrs, share, disposition, options);

    if (NT_SUCCESS(st)) {
        bridge_write_handle(handle_va, h);
        bridge_write_iostatus(iostatus_va, ios.Status, (uint32_t)ios.Information);
    } else {
        bridge_write_iostatus(iostatus_va, st, 0);
    }
    return st;
}

/* ── RtlInitAnsiString (ordinal 289) ──────────────────────
 * VOID RtlInitAnsiString(PANSI_STRING Destination, PCSZ Source)
 *
 * Fills an ANSI_STRING { USHORT Length; USHORT MaximumLength; PCHAR Buffer; }.
 * Unbridged this returned 0 and wrote nothing, so every path a title built
 * this way arrived at NtCreateFile as a null Buffer and failed with
 * STATUS_OBJECT_PATH_NOT_FOUND -- which looks like a missing file rather than
 * a missing bridge. Halo builds its map paths exactly this way.
 */
/* -- RtlEqualString (ordinal 279, 3 args) ----------------
 * BOOLEAN RtlEqualString(PSTRING String1, PSTRING String2, BOOLEAN CaseInSens)
 *
 * The fields are read out by hand rather than casting the guest struct. A
 * guest ANSI_STRING is {USHORT Length, USHORT MaximumLength, 32-bit Buffer},
 * eight bytes; the native one has a 64-bit PCHAR, so a cast would read
 * MaximumLength and Buffer from the wrong offsets and then dereference a guest
 * VA as a host address. RtlInitAnsiString stores a guest VA in that field --
 * see the bridge below -- so it has to be translated, not passed through.
 *
 * Stubbed, this returned 0: "never equal". Wreckless initialises a string and
 * compares it in a critical-section-protected lookup, so every comparison
 * missing turned that lookup into unbounded recursion and the process died of
 * a host stack overflow 200 kernel calls in.
 */
static void bridge_RtlEqualString(void)
{
    uint32_t s1_va  = STACK_ARG(0);
    uint32_t s2_va  = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    XBOX_ANSI_STRING a, b;

    if (!s1_va || !s2_va) {
        g_eax = 0;
        return;
    }
    a.Length        = BRIDGE_MEM16(s1_va + 0);
    a.MaximumLength = BRIDGE_MEM16(s1_va + 2);
    a.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    b.Length        = BRIDGE_MEM16(s2_va + 0);
    b.MaximumLength = BRIDGE_MEM16(s2_va + 2);
    b.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));

    if (!a.Buffer || !b.Buffer) {
        g_eax = 0;
        return;
    }
    g_eax = xbox_RtlEqualString(&a, &b, (BOOLEAN)nocase) ? 1 : 0;
}

static void bridge_RtlInitAnsiString(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);

    if (!dest_va) {
        g_eax = 0;
        return;
    }
    if (src_va) {
        const char *src = (const char *)XBOX_TO_NATIVE(src_va);
        size_t len = strlen(src);
        if (len > 0xFFFE) {
            len = 0xFFFE;
        }
        BRIDGE_MEM16(dest_va + 0) = (uint16_t)len;
        BRIDGE_MEM16(dest_va + 2) = (uint16_t)(len + 1);
        BRIDGE_MEM32(dest_va + 4) = src_va;
    } else {
        BRIDGE_MEM16(dest_va + 0) = 0;
        BRIDGE_MEM16(dest_va + 2) = 0;
        BRIDGE_MEM32(dest_va + 4) = 0;
    }
    g_eax = 0;
}

/* ── NtCreateFile (ordinal 190, 9 args = 36 bytes) ─────── */
static void bridge_NtCreateFile(void)
{
    uint32_t handle_va   = STACK_ARG(0);  /* PHANDLE */
    uint32_t access      = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs   = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus    = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    /* arg4: AllocationSize - ignored */
    uint32_t file_attrs  = STACK_ARG(5);  /* FileAttributes */
    uint32_t share       = STACK_ARG(6);  /* ShareAccess */
    uint32_t disposition = STACK_ARG(7);  /* CreateDisposition */
    uint32_t options     = STACK_ARG(8);  /* CreateOptions */

    /* The out-parameter addresses matter as much as the result: this bridge
     * hands them to a real Win32 call, so a bogus one has Windows itself write
     * into Xbox memory. That is how a wild write ends up with a stack inside
     * ntdll and no recompiled frame to blame. */
    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options);

    /* An FMV the host can decode itself.
     *
     * The title's own decoder is emulated like everything else, but it only
     * produces pixels once there is something to execute its GPU work -- so on
     * a bring-up where that does not exist yet, the video the game just asked
     * for can still be shown. The trigger is the title opening the file, so
     * this plays when the game decides to play it, not on a timer, and it
     * plays the file the game chose.
     *
     * Off unless RECOMP_FMV_HOST is set: it is a substitute for the title's
     * own output, and that should be a decision rather than a default. */
    if (g_eax == 0 && getenv("RECOMP_FMV_HOST")) {
        /* Declared here rather than included: the player lives in xbox_video,
         * which links xbox_d3d8, and having the kernel include its header
         * would make the dependency circular for no gain. Both land in the
         * same executable. */
        extern int xbox_VideoPlayFile(const char *host_path);
        extern int xbox_VideoIsPlaying(void);

        char host[MAX_PATH * 2];
        size_t n = 0;

        const wchar_t *w = xbox_LastHostPath();

        while (n < sizeof(host) - 1 && w[n]) {
            host[n] = (char)w[n];
            n++;
        }
        host[n] = 0;
        if (n > 4 && _stricmp(host + n - 4, ".wmv") == 0
                && !xbox_VideoIsPlaying())
            xbox_VideoPlayFile(host);
    }

    /* Paired with the [PATH] line the translation just printed: that says what
     * was asked for, this says whether it opened. A failed open is not itself
     * a bug -- a title probing the cache partition before the disc expects one
     * -- so the status is what separates a probe from a real miss. */
    {
        extern uint32_t xbox_LastFileError(void);
        uint32_t _e = g_eax ? xbox_LastFileError() : 0u;
        if (g_eax)
            fprintf(stderr, "  [FILE] -> 0x%08X FAILED (win32 err=%u%s)\n",
                    g_eax, _e,
                    _e == 32u ? " ERROR_SHARING_VIOLATION"
                  : _e ==  2u ? " ERROR_FILE_NOT_FOUND"
                  : _e ==  3u ? " ERROR_PATH_NOT_FOUND" : "");
        else
            fprintf(stderr, "  [FILE] -> 0x%08X\n", g_eax);
    }
    fflush(stderr);
}

/* ── NtOpenFile (ordinal 202, 6 args = 24 bytes) ──────── */
static void bridge_NtOpenFile(void)
{
    uint32_t handle_va = STACK_ARG(0);  /* PHANDLE */
    uint32_t access    = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus  = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    uint32_t share     = STACK_ARG(4);  /* ShareAccess */
    uint32_t options   = STACK_ARG(5);  /* OpenOptions */

    /* NtOpenFile = NtCreateFile with FILE_OPEN disposition */
    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        0, share, 1 /* FILE_OPEN */, options);
}

/*
 * Completion for a file request that carried an Event or an APC routine.
 *
 * Both bridges below do the I/O synchronously, and used to drop args 1-3
 * (Event, ApcRoutine, ApcContext) on the floor. A title that issues an async
 * request and waits alertably for the completion then waits forever: Halo's
 * cache-partition setup does exactly that, gives up after its 5-second SleepEx,
 * and asserts "setup for new cache file failed (#0)".
 *
 * ponytail: the APC runs inline here rather than at the next alertable wait.
 * The data really is ready by then, so the observable result matches; a title
 * that depends on the APC *not* having run yet would notice. A per-thread
 * deferred queue drained at alertable waits was tried for Halo's map streamer
 * and made no difference (it still issues one 14 KB batch and stops), so it was
 * dropped rather than risk changing this shared path for the other titles.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

static void deliver_one_apc(uint32_t apc_routine, uint32_t apc_context,
                            uint32_t iostatus)
{
    /* The APC can be game code or a kernel export. Halo's XAPI passes the
     * latter -- 0xFE0000FC, one of our own synthetic thunk VAs -- so the recomp
     * dispatch correctly fails to find it and the kernel fallback is the one
     * that matters. Checking only recomp_lookup left it undelivered. */
    recomp_func_t fn = recomp_lookup(apc_routine);
    if (!fn) fn = recomp_lookup_manual(apc_routine);
    if (!fn) fn = recomp_lookup_kernel(apc_routine);
    if (fn) {
        /* VOID ApcRoutine(PVOID ApcContext, PIO_STATUS_BLOCK, ULONG) */
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = iostatus;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = apc_context;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;   /* dummy return address */
        fn();
        g_esp += 12;
    } else {
        uint32_t ord = 0;
        if (apc_routine >= KERNEL_VA_BASE && apc_routine < KERNEL_VA_END) {
            ord = g_slot_ordinals[(apc_routine - KERNEL_VA_BASE) / 4];
        }
        fprintf(stderr, "  [KERNEL] file I/O APC 0x%08X unresolved"
                " (kernel ordinal %u)\n", apc_routine, ord);
        fflush(stderr);
    }
}

/* Per-thread pending-APC ring. An APC is delivered on the thread that issued
 * the request, which is also the thread that waits, so thread-local is right. */
static void bridge_complete_file_io(uint32_t event_token, uint32_t apc_routine,
                                    uint32_t apc_context, uint32_t iostatus)
{
    if (event_token) {
        HANDLE ev = bridge_resolve_handle(event_token);
        if (ev) SetEvent(ev);
    }
    if (apc_routine) {
        deliver_one_apc(apc_routine, apc_context, iostatus);
    }
}

/* -- XeLoadSection / XeUnloadSection (ordinals 327/328, 1 arg = 4 bytes) --
 *
 * NTSTATUS XeLoadSection(PXBE_SECTION_HEADER Section);
 *
 * On hardware a section marked non-preload is paged in from disc on demand,
 * and a title that keeps its video decoder in one -- Wreckless keeps WMVDEC
 * there -- calls this before touching it. Every section is already resident
 * here, so the work is the bookkeeping: hand back success and keep the
 * reference count the title can read.
 *
 * Done against guest memory rather than the PXBE_SECTION_HEADER struct: the
 * on-disc header is nine 32-bit fields and a digest, and the native struct
 * declares some of them as pointers, so on x64 its layout is not the 56 bytes
 * actually there.
 *
 *   +0x14  section name address      +0x18  section reference count
 */
#define XBE_SECTION_REFCOUNT_OFFSET 0x18

static void bridge_XeSection(int load)
{
    uint32_t section = STACK_ARG(0);
    uint32_t count;

    if (!section) {
        g_eax = 0xC000000Du;              /* STATUS_INVALID_PARAMETER */
        return;
    }
    count = BRIDGE_MEM32(section + XBE_SECTION_REFCOUNT_OFFSET);
    if (load)
        count++;
    else if (count)
        count--;
    BRIDGE_MEM32(section + XBE_SECTION_REFCOUNT_OFFSET) = count;

    if (KERNEL_LOG_ON())
        fprintf(stderr, "  [XBE] Xe%sSection(0x%08X) refcount=%u\n",
                load ? "Load" : "Unload", section, count);
    g_eax = 0;
}

static void bridge_XeLoadSection(void)   { bridge_XeSection(1); }
static void bridge_XeUnloadSection(void) { bridge_XeSection(0); }

/* -- RtlUnwind (ordinal 312, 4 args = 16 bytes) -------------------------
 *
 * VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
 *                PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue);
 *
 * Discards the SEH registration frames between the current one and
 * TargetFrame, letting each handler run its __finally blocks on the way past,
 * and leaves fs:[0] pointing at TargetFrame. fs:[0] is guest address 0 here,
 * because the runtime models the TIB at the bottom of guest memory.
 *
 * Left unbridged this returned 0 without touching anything, which is not a
 * harmless stub: MSVC's _global_unwind2 calls it and then carries on as if the
 * frames were gone, so the chain kept pointing into stack that had already
 * been reused and the next dispatch walked records built out of live locals.
 *
 * The walk is bounded and checked rather than trusting the chain, since it
 * lives in guest stack memory that a title can corrupt: records must climb
 * toward the stack top, stay inside the stack, and stay 4-byte aligned. A
 * chain that breaks any of those is truncated instead of followed.
 */
#define XBOX_SEH_END_OF_CHAIN 0xFFFFFFFFu
#define XBOX_EXCEPTION_UNWINDING  0x02u
#define XBOX_EXCEPTION_EXIT_UNWIND 0x04u
#define XBOX_SEH_MAX_FRAMES 64

static void bridge_RtlUnwind(void)
{
    uint32_t target_frame = STACK_ARG(0);
    uint32_t exc_record   = STACK_ARG(2);
    uint32_t reg          = BRIDGE_MEM32(XBOX_FS_BASE);
    uint32_t prev_reg     = 0;
    uint32_t scratch      = 0;
    int      guard;

    /* An unwind with no record of its own still has to tell the handlers it
     * is an unwind, so synthesise one below the stack pointer. */
    if (!exc_record) {
        g_esp -= 0x50;
        scratch = g_esp;
        memset((uint8_t *)XBOX_TO_NATIVE(scratch), 0, 0x50);
        BRIDGE_MEM32(scratch) = 0xC0000027u;   /* STATUS_UNWIND */
        exc_record = scratch;
    }
    BRIDGE_MEM32(exc_record + 4) |= XBOX_EXCEPTION_UNWINDING
        | (target_frame ? 0u : XBOX_EXCEPTION_EXIT_UNWIND);

    for (guard = 0; guard < XBOX_SEH_MAX_FRAMES; guard++) {
        uint32_t next, handler;

        if (reg == XBOX_SEH_END_OF_CHAIN || reg == 0 || reg == target_frame)
            break;
        if ((reg & 3u) || reg < XBOX_STACK_BASE || reg >= XBOX_STACK_TOP)
            break;                       /* not a stack frame: chain is broken */
        if (prev_reg && reg <= prev_reg)
            break;                       /* not climbing: cycle or corruption */

        next    = BRIDGE_MEM32(reg);
        handler = BRIDGE_MEM32(reg + 4);

        /* Pop before dispatching. The handler may raise, and it must not see
         * its own frame still on the chain. */
        BRIDGE_MEM32(XBOX_FS_BASE) = next;

        if (handler) {
            recomp_func_t fn = recomp_lookup(handler);
            if (!fn) fn = recomp_lookup_manual(handler);
            if (!fn) fn = recomp_lookup_kernel(handler);
            if (fn) {
                /* EXCEPTION_DISPOSITION handler(record, frame, context,
                 * dispatcher) -- cdecl, so the caller pops. */
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = reg;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = exc_record;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;  /* return address */
                fn();
                /* 16, not 20: the handler's own `ret` has already taken the
                 * return address off, leaving just the four arguments for the
                 * caller to drop. Cleaning 20 leaves esp four bytes high, and
                 * every argument the unwound-into frame reads after that comes
                 * from one slot over. */
                g_esp += 16;
            }
        }

        prev_reg = reg;
        reg      = next;
    }

    /* Land on the target even if the walk stopped early: leaving fs:[0] on a
     * discarded frame is worse than losing a __finally. */
    if (target_frame && target_frame != XBOX_SEH_END_OF_CHAIN)
        BRIDGE_MEM32(XBOX_FS_BASE) = target_frame;

    if (scratch)
        g_esp += 0x50;
}

/* ── NtReadFile (ordinal 219, 8 args = 32 bytes) ──────── */
static void bridge_NtReadFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off = {0};
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    g_eax = (uint32_t)xbox_NtReadFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);

    /* What a read actually delivered. A decoder that rejects its input cannot
     * say whether the bytes were wrong or the read was, and the two look
     * identical from inside the title -- the first bytes settle it. */
    {
        const uint8_t *p = (const uint8_t *)XBOX_TO_NATIVE(buffer_va);
        uint32_t got = (uint32_t)ios.Information;
        /* The offset matters as much as the length. A title streaming a pack
         * file reads sector-aligned chunks, so the first bytes belong to
         * whatever precedes the file it actually wants, and a read that stops
         * early looks identical to one that never started -- until you can
         * see where each one landed. */
        if (poff)
            fprintf(stderr, "  [READ] @%lld want=%u got=%u st=0x%08X  %02X %02X %02X %02X\n",
                    (long long)off.QuadPart, length, got,
                    (uint32_t)ios.Status,
                    got > 0 ? p[0] : 0, got > 1 ? p[1] : 0,
                    got > 2 ? p[2] : 0, got > 3 ? p[3] : 0);
        else
            fprintf(stderr, "  [READ] @seq want=%u got=%u st=0x%08X  %02X %02X %02X %02X\n",
                    length, got, (uint32_t)ios.Status,
                    got > 0 ? p[0] : 0, got > 1 ? p[1] : 0,
                    got > 2 ? p[2] : 0, got > 3 ? p[3] : 0);
        fflush(stderr);
    }
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            iostatus);
}

/* ── NtWriteFile (ordinal 236, 8 args = 32 bytes) ─────── */
static void bridge_NtWriteFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off = {0};
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    g_eax = (uint32_t)xbox_NtWriteFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            iostatus);
}

/* ── NtQueryInformationFile (ordinal 211, 5 args = 20 bytes) */
static void bridge_NtQueryInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtQueryInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtSetInformationFile (ordinal 226, 5 args = 20 bytes) ─ */
static void bridge_NtSetInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtSetInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryVolumeInformationFile (ordinal 218, 5 args = 20 bytes) */
static void bridge_NtQueryVolumeInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtQueryVolumeInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FS_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryFullAttributesFile (ordinal 210, 2 args = 8 bytes) */
static void bridge_NtQueryFullAttributesFile(void)
{
    uint32_t obj_attrs = STACK_ARG(0);
    uint32_t info_va   = STACK_ARG(1);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(obj_attrs, &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtQueryFullAttributesFile(&oa,
                (PXBOX_FILE_NETWORK_OPEN_INFORMATION)XBOX_TO_NATIVE(info_va));
}

/* ── NtFlushBuffersFile (ordinal 198, 2 args = 8 bytes) ─── */
static void bridge_NtFlushBuffersFile(void)
{
    HANDLE   handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va = STACK_ARG(1);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtFlushBuffersFile(handle, &ios);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtDeleteFile (ordinal 195, 1 arg = 4 bytes) ─────── */
static void bridge_NtDeleteFile(void)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(STACK_ARG(0), &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtDeleteFile(&oa);
}

/* ── NtQueryDirectoryFile (ordinal 207, 9 args = 36 bytes) ─ */
static void bridge_NtQueryDirectoryFile(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va      = STACK_ARG(4);
    uint32_t info_va     = STACK_ARG(5);
    uint32_t length      = STACK_ARG(6);
    uint32_t filename_va = STACK_ARG(7);  /* PXBOX_ANSI_STRING */
    uint32_t restart     = STACK_ARG(8);  /* BOOLEAN */
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_ANSI_STRING     fn;
    PXBOX_ANSI_STRING    pfn = NULL;

    memset(&ios, 0, sizeof(ios));
    if (filename_va) {
        /* Xbox ANSI_STRING: 0=Length(u16), 2=MaximumLength(u16), 4=Buffer(u32) */
        uint32_t fn_buf  = BRIDGE_MEM32(filename_va + 4);
        fn.Length        = BRIDGE_MEM16(filename_va);
        fn.MaximumLength = BRIDGE_MEM16(filename_va + 2);
        fn.Buffer        = fn_buf ? (PCHAR)XBOX_TO_NATIVE(fn_buf) : NULL;
        if (fn.Buffer) pfn = &fn;
    }
    g_eax = (uint32_t)xbox_NtQueryDirectoryFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(info_va), length, pfn, (BOOLEAN)restart);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtOpenSymbolicLinkObject (ordinal 203, 2 args = 8 bytes) */
static void bridge_NtOpenSymbolicLinkObject(void)
{
    uint32_t handle_va = STACK_ARG(0);
    /* arg1: POBJECT_ATTRIBUTES - ignored, we return a synthetic handle.
     * Written raw (untagged) so NtClose recognises it and skips it. */
    if (handle_va) BRIDGE_MEM32(handle_va) = 0xDEAD0001u;
    g_eax = STATUS_SUCCESS;
}

/* ── NtQuerySymbolicLinkObject (ordinal 215, 3 args = 12 bytes) */
static void bridge_NtQuerySymbolicLinkObject(void)
{
    /* uint32_t handle = STACK_ARG(0); */
    uint32_t target_va = STACK_ARG(1);
    uint32_t retlen_va = STACK_ARG(2);
    const char* target = "\\Device\\CdRom0";
    USHORT len = (USHORT)strlen(target);

    if (retlen_va) BRIDGE_MEM32(retlen_va) = (uint32_t)len;

    /* Say so when the buffer could not be filled.
     *
     * Reporting STATUS_SUCCESS with an untouched output buffer is the same
     * defect that left ordinal 215 unrouted: the caller believes it has a
     * device path and parses whatever was already in that memory. Half-Life 2
     * does exactly that -- it walked uninitialised bytes and dereferenced
     * 0x68737572, the ASCII "rush", as a pointer. That only looked survivable
     * because the RAM mirrors happened to back the address; with a mapping
     * that does not alias, it faults immediately.
     *
     * STATUS_BUFFER_TOO_SMALL is the honest answer, and it is one the caller
     * already has to handle -- it is what a real kernel returns when the
     * ANSI_STRING it was handed has no room. */
    if (!target_va) {
        g_eax = 0xC0000023u;             /* STATUS_BUFFER_TOO_SMALL */
        return;
    }
    {
        uint16_t max_len = BRIDGE_MEM16(target_va + 2);
        uint32_t buf_va  = BRIDGE_MEM32(target_va + 4);

        if (!buf_va || len >= max_len) {
            static unsigned warned;
            if (warned++ < 4) {
                fprintf(stderr,
                        "  [KERNEL] NtQuerySymbolicLinkObject: buffer 0x%08X "
                        "max=%u cannot hold %u bytes; returning "
                        "STATUS_BUFFER_TOO_SMALL\n",
                        buf_va, (unsigned)max_len, (unsigned)len + 1);
                fflush(stderr);
            }
            g_eax = 0xC0000023u;         /* STATUS_BUFFER_TOO_SMALL */
            return;
        }
        memcpy(XBOX_TO_NATIVE(buf_va), target, len + 1);
        BRIDGE_MEM16(target_va) = len;
    }
    g_eax = STATUS_SUCCESS;
}

/* ── IoCreateFile (ordinal 67, 10 args = 40 bytes) ────── */
static void bridge_IoCreateFile(void)
{
    /* Same as NtCreateFile with an extra Options arg at the end */
    uint32_t handle_va   = STACK_ARG(0);
    uint32_t access      = STACK_ARG(1);
    uint32_t obj_attrs   = STACK_ARG(2);
    uint32_t iostatus    = STACK_ARG(3);
    uint32_t file_attrs  = STACK_ARG(5);
    uint32_t share       = STACK_ARG(6);
    uint32_t disposition = STACK_ARG(7);
    uint32_t options     = STACK_ARG(8);

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options);
}

/* -- NtDeviceIoControlFile (ordinal 196, 10 args = 40 bytes) ----
 *
 * NTSTATUS NtDeviceIoControlFile(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID,
 *                                PIO_STATUS_BLOCK, ULONG IoControlCode,
 *                                PVOID In, ULONG InLen,
 *                                PVOID Out, ULONG OutLen);
 */
#define IOCTL_DISK_GET_DRIVE_GEOMETRY 0x00070000u
#define IOCTL_DISK_GET_PARTITION_INFO 0x00074004u

/* The retail hard disk, in the units DISK_GEOMETRY reports. Deliberately the
 * same geometry kernel_path.c writes into the partition table it synthesises,
 * so a title that reads both sees one disk rather than two. */
#define XBOX_DISK_BYTES_PER_SECTOR   512u
#define XBOX_DISK_SECTORS_PER_TRACK  63u
#define XBOX_DISK_TRACKS_PER_CYL     255u
#define XBOX_DISK_CYLINDERS          1216u    /* ~10 GB, a retail 8 GB drive */

static void bridge_NtDeviceIoControlFile(void)
{
    uint32_t ios_va  = STACK_ARG(4);
    uint32_t ioctl   = STACK_ARG(5);
    uint32_t out_va  = STACK_ARG(8);
    uint32_t out_len = STACK_ARG(9);

    if (ioctl == IOCTL_DISK_GET_DRIVE_GEOMETRY) {
        /* DISK_GEOMETRY: Cylinders (LARGE_INTEGER), MediaType,
         * TracksPerCylinder, SectorsPerTrack, BytesPerSector -- 24 bytes. */
        if (!out_va || out_len < 24) {
            bridge_write_iostatus(ios_va, 0xC0000023u, 0); /* BUFFER_TOO_SMALL */
            g_eax = 0xC0000023u;
            return;
        }
        BRIDGE_MEM32(out_va +  0) = XBOX_DISK_CYLINDERS;
        BRIDGE_MEM32(out_va +  4) = 0;
        BRIDGE_MEM32(out_va +  8) = 0x0B;      /* FixedMedia */
        BRIDGE_MEM32(out_va + 12) = XBOX_DISK_TRACKS_PER_CYL;
        BRIDGE_MEM32(out_va + 16) = XBOX_DISK_SECTORS_PER_TRACK;
        BRIDGE_MEM32(out_va + 20) = XBOX_DISK_BYTES_PER_SECTOR;
        bridge_write_iostatus(ios_va, 0, 24);
        g_eax = 0;
        return;
    }

    if (ioctl == IOCTL_DISK_GET_PARTITION_INFO) {
        /* PARTITION_INFORMATION: StartingOffset and PartitionLength as
         * LARGE_INTEGERs, then HiddenSectors, PartitionNumber, and three
         * bytes of type/flags -- 32 bytes. The length is the image's own
         * size, which kernel_path.c set from the partition table. */
        HANDLE h = bridge_resolve_handle(STACK_ARG(0));
        LARGE_INTEGER size;

        if (!out_va || out_len < 32) {
            bridge_write_iostatus(ios_va, 0xC0000023u, 0);
            g_eax = 0xC0000023u;
            return;
        }
        size.QuadPart = 0;
        if (h && h != INVALID_HANDLE_VALUE)
            GetFileSizeEx(h, &size);
        BRIDGE_MEM32(out_va +  0) = 0;                       /* StartingOffset */
        BRIDGE_MEM32(out_va +  4) = 0;
        BRIDGE_MEM32(out_va +  8) = (uint32_t)size.LowPart;  /* PartitionLength */
        BRIDGE_MEM32(out_va + 12) = (uint32_t)size.HighPart;
        BRIDGE_MEM32(out_va + 16) = 0;                       /* HiddenSectors  */
        BRIDGE_MEM32(out_va + 20) = 1;                       /* PartitionNumber*/
        BRIDGE_MEM32(out_va + 24) = 0x00010106u;             /* type/boot/recog */
        BRIDGE_MEM32(out_va + 28) = 0;
        bridge_write_iostatus(ios_va, 0, 32);
        g_eax = 0;
        return;
    }

    fprintf(stderr, "  [FILE] NtDeviceIoControlFile(0x%X) - unhandled\n", ioctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu; /* STATUS_NOT_SUPPORTED */
}

/* ── NtFsControlFile (ordinal 200, 10 args = 40 bytes) ──── */
static void bridge_NtFsControlFile(void)
{
    uint32_t fsctl = STACK_ARG(5);
    uint32_t ios_va = STACK_ARG(4);
    fprintf(stderr, "  [FILE] NtFsControlFile(0x%X) - stub\n", fsctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu;
}

/* ── NtCreateDirectoryObject (ordinal 188) ──────────────── */
static void bridge_NtCreateDirectoryObject(void)
{
    /* Return STATUS_SUCCESS with a fake handle */
    uint32_t handle_ptr = STACK_ARG(0);
    if (handle_ptr) BRIDGE_MEM32(handle_ptr) = 0xBEEF0010;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* IoCreateSymbolicLink (ordinal 67, 2 args)
 *
 * Was a bare "return STATUS_SUCCESS": the title was told its link existed and
 * nothing recorded it. Titles mount their own drive letters this way --
 * Wreckless links \??\Z: to \Device\Harddisk0\Partition1\ and then loads
 * every asset through z:\ -- so dropping the link left xbox_translate_path
 * applying the generic "Z: is the cache partition" rule, and every asset open
 * failed with ERROR_FILE_NOT_FOUND a whole boot later.
 *
 * Both arguments are guest ANSI_STRINGs whose Buffer field holds a guest VA,
 * so passing the structs straight through would have xbox_copy_ansi read a
 * 32-bit guest address as a 64-bit host pointer. Rebuild them by hand, the way
 * bridge_RtlEqualString does.
 */
static void bridge_IoCreateSymbolicLink(void)
{
    uint32_t link_va   = STACK_ARG(0);
    uint32_t target_va = STACK_ARG(1);
    XBOX_ANSI_STRING link, target;

    if (!link_va) {
        g_eax = 0xC000000Du;  /* STATUS_INVALID_PARAMETER */
        return;
    }
    link.Length        = BRIDGE_MEM16(link_va + 0);
    link.MaximumLength = BRIDGE_MEM16(link_va + 2);
    link.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(link_va + 4));

    if (target_va) {
        target.Length        = BRIDGE_MEM16(target_va + 0);
        target.MaximumLength = BRIDGE_MEM16(target_va + 2);
        target.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(target_va + 4));
    } else {
        target.Length = target.MaximumLength = 0;
        target.Buffer = NULL;
    }

    g_eax = (uint32_t)xbox_IoCreateSymbolicLink(&link,
                                                target_va ? &target : NULL);
}

/* ── ObReferenceObjectByHandle (ordinal 246) ─────────────── */
static void bridge_ObReferenceObjectByHandle(void)
{
    /* Xbox: NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, PVOID ObjectType, PVOID* Object)
     * 3 args (not 6 like Windows NT) */
    uint32_t handle = STACK_ARG(0);
    uint32_t obj_type = STACK_ARG(1);
    uint32_t object_ptr = STACK_ARG(2);
    if (object_ptr) BRIDGE_MEM32(object_ptr) = 0;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── RtlRaiseException (ordinal 302) ─────────────────────
 * VOID RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord)
 *
 * Called by CRT / SEH code to raise structured exceptions.
 * On Xbox this triggers the kernel exception dispatcher.
 * For recompilation, we log and continue (no real SEH dispatch yet).
 */
static void bridge_RtlRaiseException(void)
{
    uint32_t record_ptr = STACK_ARG(0);
    uint32_t code = record_ptr ? BRIDGE_MEM32(record_ptr) : 0;

    static int raise_count = 0;
    raise_count++;
    if (raise_count <= 10) {
        fprintf(stderr, "  [KERNEL] RtlRaiseException: record=0x%08X code=0x%08X (#%d)\n",
                record_ptr, code, raise_count);
        fflush(stderr);
    }

    /* Handle float exceptions by clearing the FPU status.
     *
     * On the real Xbox, RtlRaiseException dispatches through the SEH chain.
     * For float exceptions (0xC0000090-0xC0000096), the CRT exception handler
     * clears the x87/SSE status word and continues execution. Without clearing,
     * the caller re-checks the FPU status, sees the exception still pending,
     * and re-raises in an infinite loop.
     *
     * _clearfp() clears both x87 and SSE exception flags on Windows x64.
     */
    if (code >= 0xC0000090u && code <= 0xC0000096u) {
        _clearfp();
    }

    g_eax = 0;
}

/* ── MmMapIoSpace (ordinal 177) ──────────────────────────
 * PVOID MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect)
 *
 * Maps physical I/O memory (GPU registers, etc.) into virtual address space.
 * Allocate from Xbox heap so the returned pointer is a valid Xbox VA.
 */
static void bridge_MmMapIoSpace(void)
{
    uint32_t phys_addr = STACK_ARG(0);
    uint32_t num_bytes = STACK_ARG(1);
    uint32_t protect = STACK_ARG(2);
    uint32_t xbox_va = xbox_HeapAlloc(num_bytes, 4096);

    fprintf(stderr, "  [KERNEL] MmMapIoSpace: phys=0x%08X size=%u → Xbox VA 0x%08X\n",
            phys_addr, num_bytes, xbox_va);
    fflush(stderr);

    g_eax = xbox_va;
}

/* ── MmPersistContiguousMemory (ordinal 178) ─────────────
 * VOID MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
 *
 * Marks contiguous memory as persistent across reboots (for save data).
 * No-op for recompilation.
 */
static void bridge_MmPersistContiguousMemory(void)
{
    /* No-op stub */
    g_eax = 0;
}

/* ── Generic fallback for simple value-only functions ────── */
static void bridge_generic_stub(void)
{
    /* Success-returning stub for functions whose callers only check for 0.
     * Deliberately silent: the caller (kernel_thunk_dispatch) warns for
     * ordinals with no bridge at all, which is the case worth hearing about. */
    g_eax = 0;
}


/* ══════════════════════════════════════════════════════════════════════════
 * Wrappers for the ordinals Halo 2276's thunk table binds but the bridge did
 * not route. Every one of these already had a working xbox_* implementation in
 * src/kernel/*.c; only the wrapper that moves arguments off the simulated stack
 * was missing, so each call was silently a no-op returning 0.
 *
 * Guest pointers go through XBOX_TO_NATIVE, which maps NULL to NULL. Scalars
 * pass straight through. Handles are tokens, not host HANDLEs, so they go
 * through bridge_resolve_handle / bridge_write_handle.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── AvGetSavedDataAddress (ordinal 1, void) */
static void bridge_AvGetSavedDataAddress(void)
{
    g_eax = (uint32_t)xbox_AvGetSavedDataAddress();
}

/* ── AvSendTVEncoderOption (ordinal 2, 4 args) */
static void bridge_AvSendTVEncoderOption(void)
{
    xbox_AvSendTVEncoderOption(XBOX_TO_NATIVE(STACK_ARG(0)),
                               STACK_ARG(1), STACK_ARG(2),
                               (PULONG)XBOX_TO_NATIVE(STACK_ARG(3)));
    g_eax = 0;
}

/* ── ExFreePool (ordinal 17, 1 arg)
 * Was resolving to a DATA address before the kernel_data_va_for_ordinal fix,
 * so the title was calling into kernel data. Even after that it was an
 * unbridged no-op, which leaks every pool block the title ever frees.
 *
 * Deliberately does NOT call xbox_ExFreePool: that one HeapFrees its argument
 * on the host heap, but every pool block reaches the title as a guest VA from
 * xbox_HeapAlloc, so only the guest heap can release it. xbox_HeapFree answers
 * through the block table and takes the raw 32-bit guest VA. */
static void bridge_ExFreePool(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* ── IoCreateDevice (ordinal 65, 6 args) ────────────────
 *
 * NTSTATUS IoCreateDevice(PDRIVER_OBJECT DriverObject,
 *                         ULONG DeviceExtensionSize,
 *                         PANSI_STRING DeviceName,
 *                         DEVICE_TYPE DeviceType,
 *                         BOOLEAN Exclusive,
 *                         PDEVICE_OBJECT *DeviceObject);
 *
 * Deliberately does NOT call xbox_IoCreateDevice. That one allocates from the
 * host heap and writes a 64-bit native pointer through this 4-byte guest
 * out-parameter -- the memory-model mismatch the NOT ROUTED note below
 * describes. The object has to live in guest memory because the caller
 * immediately walks it, so allocate it there.
 *
 * Leaving the ordinal unbridged was not the safe option it looked like: the
 * unresolved thunk returns 0, which is STATUS_SUCCESS, so the title proceeds
 * with a NULL device. Wreckless reads DeviceExtension from it and `rep stosd`s
 * DeviceExtensionSize bytes through the NULL, erasing the fake TIB at guest
 * VA 0. The crash surfaced in SetLastError, thousands of calls later.
 *
 * Layout is the Xbox DEVICE_OBJECT: Flags at 0x14, DeviceExtension at 0x18,
 * DeviceType at 0x1C, StackSize at 0x1D, header 0x38 bytes. Only the fields a
 * title actually reads are filled; the rest is zero, which is what a freshly
 * created device object holds anyway.
 */
#define XBOX_DEVICE_OBJECT_SIZE 0x38u

static void bridge_IoCreateDevice(void)
{
    uint32_t extension_size = STACK_ARG(1);
    uint32_t device_type    = STACK_ARG(3);
    uint32_t out_va         = STACK_ARG(5);
    uint32_t object_va;

    if (!out_va) {
        g_eax = 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
        return;
    }

    object_va = xbox_HeapAlloc(XBOX_DEVICE_OBJECT_SIZE + extension_size, 16);
    if (!object_va) {
        g_eax = 0xC000009Au;   /* STATUS_INSUFFICIENT_RESOURCES */
        return;
    }

    memset(XBOX_TO_NATIVE(object_va), 0,
           XBOX_DEVICE_OBJECT_SIZE + extension_size);
    BRIDGE_MEM16(object_va + 0x02) = (uint16_t)XBOX_DEVICE_OBJECT_SIZE;
    BRIDGE_MEM32(object_va + 0x04) = 1;                 /* ReferenceCount   */
    BRIDGE_MEM32(object_va + 0x08) = STACK_ARG(0);      /* DriverObject     */
    BRIDGE_MEM32(object_va + 0x18) =                    /* DeviceExtension  */
        extension_size ? object_va + XBOX_DEVICE_OBJECT_SIZE : 0;
    BRIDGE_MEM8(object_va + 0x1C)  = (uint8_t)device_type;
    BRIDGE_MEM8(object_va + 0x1D)  = 1;                 /* StackSize        */

    BRIDGE_MEM32(out_va) = object_va;
    g_eax = 0;                                          /* STATUS_SUCCESS   */
}

/* ── KeCancelTimer (ordinal 97, 1 arg)
 * BOOLEAN KeCancelTimer(PKTIMER Timer) -- returns whether it was set. */
static void bridge_KeCancelTimer(void)
{
/* Both halves: the shadow object this runtime keeps, and the firing
     * table above, or a cancelled timer keeps calling its DPC. */
    uint32_t timer_va = STACK_ARG(0);
    int armed = xbox_kernel_cancel_timer(timer_va);
    g_eax = (uint32_t)xbox_KeCancelTimer(XBOX_TO_NATIVE(timer_va)) || armed;
}

/* ── KeDisconnectInterrupt (ordinal 100, 1 arg) */
static void bridge_KeDisconnectInterrupt(void)
{
    g_eax = (uint32_t)xbox_KeDisconnectInterrupt(
        (PXBOX_KINTERRUPT)XBOX_TO_NATIVE(STACK_ARG(0)));
}

/* ── KeSetBasePriorityThread (ordinal 143, 2 args) */
/* KeQueryBasePriorityThread (ordinal 124, 1 arg). The implementation has
 * existed in kernel_thread.c all along; only the bridge wrapper was
 * missing, so the thunk fell through to the fallback and returned 0. */
static void bridge_KeQueryBasePriorityThread(void)
{
    g_eax = (uint32_t)xbox_KeQueryBasePriorityThread(
        XBOX_TO_NATIVE(STACK_ARG(0)));
}

static void bridge_KeSetBasePriorityThread(void)
{
    g_eax = (uint32_t)xbox_KeSetBasePriorityThread(
        XBOX_TO_NATIVE(STACK_ARG(0)), (LONG)STACK_ARG(1));
}

/* ── KeStallExecutionProcessor (ordinal 151, 1 arg) */
static void bridge_KeStallExecutionProcessor(void)
{
    xbox_KeStallExecutionProcessor(STACK_ARG(0));
    g_eax = 0;
}

/* ── MmLockUnlockBufferPages (ordinal 175, 3 args) */
static void bridge_MmLockUnlockBufferPages(void)
{
    xbox_MmLockUnlockBufferPages(XBOX_TO_NATIVE(STACK_ARG(0)),
                                 STACK_ARG(1), (BOOLEAN)STACK_ARG(2));
    g_eax = 0;
}

/* ── MmQueryAllocationSize (ordinal 180, 1 arg)
 *
 * Answered from the guest heap's block table, NOT from xbox_MmQueryAllocationSize.
 * That one calls VirtualQuery, which on a translated guest address reports the
 * size of the whole 64 MB guest mapping -- a confidently wrong answer where the
 * title expects the size of the block it allocated. This is the memory-model
 * check the parked-bridge list below asks for, done: the question is about
 * guest memory, so only the guest allocator can answer it.
 */
static void bridge_MmQueryAllocationSize(void)
{
    g_eax = xbox_HeapBlockSize(STACK_ARG(0));
}

/* ── NtCreateMutant (ordinal 192, 3 args) */
static void bridge_NtCreateMutant(void)
{
    uint32_t handle_va = STACK_ARG(0);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING name;
    HANDLE h = NULL;
    NTSTATUS st;

    bridge_build_oa(STACK_ARG(1), &oa, &name);
    st = xbox_NtCreateMutant(&h, STACK_ARG(1) ? &oa : NULL,
                             (BOOLEAN)STACK_ARG(2));
    if (st >= 0 && handle_va) bridge_write_handle(handle_va, h);
    g_eax = (uint32_t)st;
}

/* ── NtReleaseMutant (ordinal 221, 2 args)
 *
 * NTSTATUS NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount);
 *
 * The partner of NtCreateMutant above, and routing one without the other is a
 * deadlock generator: the create succeeds, the release silently does nothing,
 * and the mutex stays held forever by a thread that has already exited.
 *
 * That is what the Xbox Dashboard's audio streaming did. Each ambient WAV gets
 * five events, a worker thread and a mutant; the worker finished, failed to
 * release, and terminated. The next attempt could not take the mutex, so the
 * dashboard reopened the same file and spawned another worker with another
 * 512 KB stack, forever -- visible only as a heap that climbed and a tick that
 * never returned.
 *
 * Memory model: a handle token in, an optional 4-byte LONG out through a guest
 * address. Nothing allocates, frees, or hands back a host pointer.
 */
static void bridge_NtReleaseMutant(void)
{
    uint32_t count_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtReleaseMutant(
        bridge_resolve_handle(STACK_ARG(0)),
        count_va ? (PLONG)XBOX_TO_NATIVE(count_va) : NULL);
}

/* -- NtSuspendThread (ordinal 231, 2 args) --------------------------------
 *
 * NTSTATUS NtSuspendThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount);
 *
 * The implementation was already here and only the dispatch entry was missing,
 * which is worse than an outright stub: the call returned 0, so a thread that
 * parked itself believed it had stopped and carried straight on. Wreckless
 * does that on a worker, and the "suspended" thread spun through 289 million
 * kernel calls while the title thought it was idle.
 */
static void bridge_NtSuspendThread(void)
{
    uint32_t count_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtSuspendThread(
        bridge_resolve_handle(STACK_ARG(0)),
        count_va ? (PULONG)XBOX_TO_NATIVE(count_va) : NULL);
}

/* ── NtResumeThread (ordinal 224, 2 args) */
static void bridge_NtResumeThread(void)
{
    g_eax = (uint32_t)xbox_NtResumeThread(
        bridge_resolve_handle(STACK_ARG(0)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── ObfDereferenceObject (ordinal 250, fastcall: object in ecx)
 * Not STACK_ARG(0). Xbox uses __fastcall here, so the argument never reaches
 * the stack and the arg-size entry is 0. Reading it off the stack would
 * dereference whatever the caller happened to leave there. */
static void bridge_ObfDereferenceObject(void)
{
    xbox_ObfDereferenceObject(XBOX_TO_NATIVE(g_ecx));
    g_eax = 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * More wrappers for xbox_* implementations that had no route. Same rule as
 * the block above: each one was checked against the memory-model bar --
 * reads/writes only happen at caller-supplied guest addresses, XBOX_TO_NATIVE
 * maps guest NULL to host NULL, and none of them allocates, frees, or hands
 * back a host pointer. Any exception is noted in place.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── ObfReferenceObject (ordinal 251, fastcall: object in ecx)
 * Mirror of 250. Xbox uses __fastcall here too, so the argument never reaches
 * the stack and the arg-size entry is 0; reading it off the stack would
 * dereference whatever the caller happened to leave there. */
static void bridge_ObfReferenceObject(void)
{
    xbox_ObfReferenceObject(XBOX_TO_NATIVE(g_ecx));
    g_eax = 0;
}

/* ── HalReadSMBusValue / HalWriteSMBusValue (ordinals 45 / 50, 4 args)
 *
 * The SMC side of the AV-pack / temperature queries titles make at init.
 * Unrouted they read 0 -- "no AV pack connected" -- which made D3D pick the
 * lowest common denominator display mode and 40C sensors read as 0C.
 *
 * HalReadSMBusValue writes one ULONG through a caller-supplied DataValue and
 * nothing else; HalWriteSMBusValue is all scalars. Both are the safe kind the
 * note below names.
 */
static void bridge_HalReadSMBusValue(void)
{
    uint32_t data_va = STACK_ARG(3);

    g_eax = (uint32_t)xbox_HalReadSMBusValue(
        (UCHAR)STACK_ARG(0), (UCHAR)STACK_ARG(1),
        (BOOLEAN)STACK_ARG(2), (PULONG)XBOX_TO_NATIVE(data_va));
}

static void bridge_HalWriteSMBusValue(void)
{
    g_eax = (uint32_t)xbox_HalWriteSMBusValue(
        (UCHAR)STACK_ARG(0), (UCHAR)STACK_ARG(1),
        (BOOLEAN)STACK_ARG(2), STACK_ARG(3));
}

/* ── HalReadWritePCISpace (ordinal 46, 6 args)
 *
 * PCI config space for the NV2A/southbridge; on reads the kernel serves a
 * zeroed buffer, so only a memset at a caller-supplied address happens. */
static void bridge_HalReadWritePCISpace(void)
{
    xbox_HalReadWritePCISpace(
        STACK_ARG(0), STACK_ARG(1), STACK_ARG(2),
        XBOX_TO_NATIVE(STACK_ARG(3)), STACK_ARG(4), (BOOLEAN)STACK_ARG(5));
    g_eax = 0;
}

/* ── HalRequestSoftwareInterrupt / HalClearSoftwareInterrupt (48 / 38, 1 arg)
 * DPC/APC delivery hooks with no interrupt-driven machinery behind them here;
 * both xbox_* versions are documented no-ops. */
static void bridge_HalRequestSoftwareInterrupt(void)
{
    xbox_HalRequestSoftwareInterrupt((KIRQL)STACK_ARG(0));
    g_eax = 0;
}

static void bridge_HalClearSoftwareInterrupt(void)
{
    xbox_HalClearSoftwareInterrupt((KIRQL)STACK_ARG(0));
    g_eax = 0;
}

/* ── ExSaveNonVolatileSetting (ordinal 29, 4 args)
 *
 * The write half of the EEPROM pair whose read side is already routed.
 * xbox_ExSaveNonVolatileSetting logs and returns success without ever reading
 * the Value buffer, so marshalling it across is cosmetic but keeps the write
 * path out of the unknowable-stub category. */
static void bridge_ExSaveNonVolatileSetting(void)
{
    g_eax = (uint32_t)xbox_ExSaveNonVolatileSetting(
        STACK_ARG(0), STACK_ARG(1), XBOX_TO_NATIVE(STACK_ARG(2)),
        STACK_ARG(3));
}

/* ── MmUnmapIoSpace (ordinal 183, 2 args)
 *
 * Deliberately does NOT call xbox_MmUnmapIoSpace: that one VirtualFrees its
 * argument, but the mapping coming back out of bridge_MmMapIoSpace was carved
 * from the guest heap, so only the guest heap can release it. xbox_HeapFree
 * matches against the block table and takes the raw 32-bit guest VA. */
static void bridge_MmUnmapIoSpace(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* ── IoDeleteDevice (ordinal 68, 1 arg)
 *
 * Same memory model as MmUnmapIoSpace: bridge_IoCreateDevice allocates the
 * device object on the guest heap, so deletion has to answer there too --
 * HeapFree (what xbox_IoDeleteDevice calls) would free a pointer that heap
 * never saw. */
static void bridge_IoDeleteDevice(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* ── KeQueryInterruptTime (ordinal 125, void)
 * Returns a 64-bit tick count; the caller reads it as edx:eax, so the high
 * half goes to g_edx exactly like the performance-counter bridges. */
static void bridge_KeQueryInterruptTime(void)
{
    uint64_t t = xbox_KeQueryInterruptTime();

    g_eax = (uint32_t)t;
    g_edx = (uint32_t)(t >> 32);
}

/* ── KeSaveFloatingPointState / KeRestoreFloatingPointState (142 / 139, 1 arg)
 *
 * D3D brackets its fixed-function transform code with these on hardware. Both
 * xbox_* implementations are successful no-ops -- Windows user mode preserves
 * FP state across context switches -- so marshalling is a pass-through of one
 * address. */
static void bridge_KeSaveFloatingPointState(void)
{
    g_eax = (uint32_t)xbox_KeSaveFloatingPointState(
        XBOX_TO_NATIVE(STACK_ARG(0)));
}

static void bridge_KeRestoreFloatingPointState(void)
{
    g_eax = (uint32_t)xbox_KeRestoreFloatingPointState(
        XBOX_TO_NATIVE(STACK_ARG(0)));
}

/* ── NtCreateSemaphore (ordinal 193, 4 args)
 *
 * Handle-shaped twin of NtCreateEvent/NtCreateMutant. The native HANDLE is
 * 8 bytes and the guest slot is 4, so the create goes through a local and the
 * result lands via bridge_write_handle. */
static void bridge_NtCreateSemaphore(void)
{
    uint32_t handle_va = STACK_ARG(0);
    HANDLE h = NULL;
    NTSTATUS st;

    st = xbox_NtCreateSemaphore(
        &h, XBOX_TO_NATIVE(STACK_ARG(1)),
        (LONG)STACK_ARG(2), (LONG)STACK_ARG(3));
    if (st >= 0 && handle_va)
        bridge_write_handle(handle_va, h);
    g_eax = (uint32_t)st;
}

/* ── NtReleaseSemaphore (ordinal 222, 3 args)
 * Handle token in, LONG by value, optional 4-byte PLONG out. */
static void bridge_NtReleaseSemaphore(void)
{
    uint32_t count_va = STACK_ARG(2);

    g_eax = (uint32_t)xbox_NtReleaseSemaphore(
        bridge_resolve_handle(STACK_ARG(0)), (LONG)STACK_ARG(1),
        count_va ? (PLONG)XBOX_TO_NATIVE(count_va) : NULL);
}

/* ── KeAlertThread (ordinal 93, 2 args)
 * Thread passed as an object; resolves through the handle table the way the
 * Nt* thread calls do, degrading to a pass-through for synthetic handles. */
static void bridge_KeAlertThread(void)
{
    g_eax = (uint32_t)xbox_KeAlertThread(
        bridge_resolve_handle(STACK_ARG(0)), (KPROCESSOR_MODE)STACK_ARG(1));
}

/* ── KeWaitForMultipleObjects (ordinal 158, 8 args)
 *
 * KeWaitForSingleObject is routed; the multiple-object sibling was not. Like
 * NtWaitForMultipleObjectsEx, the Objects[] array in guest memory holds 32-bit
 * handle tokens, so each is resolved before the native wait sees it. */
#define BRIDGE_MAXIMUM_WAIT_OBJECTS 64

static void bridge_KeWaitForMultipleObjects(void)
{
    uint32_t count      = STACK_ARG(0);
    uint32_t objects_va = STACK_ARG(1);
    uint32_t wait_type  = STACK_ARG(2);
    uint32_t alertable  = STACK_ARG(5);   /* 3=WaitReason, 4=WaitMode */
    uint32_t timeout_va = STACK_ARG(6);
    HANDLE handles[BRIDGE_MAXIMUM_WAIT_OBJECTS];
    uint32_t i;

    if (count == 0) {
        g_eax = (uint32_t)STATUS_INVALID_PARAMETER;
        return;
    }
    if (count > BRIDGE_MAXIMUM_WAIT_OBJECTS)
        count = BRIDGE_MAXIMUM_WAIT_OBJECTS;
    for (i = 0; i < count; i++)
        handles[i] = bridge_resolve_handle(
            objects_va ? BRIDGE_MEM32(objects_va + i * 4) : 0);

    g_eax = (uint32_t)xbox_KeWaitForMultipleObjects(
        count, (PVOID *)handles, wait_type,
        STACK_ARG(3), (KPROCESSOR_MODE)STACK_ARG(4),
        (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_va),
        XBOX_TO_NATIVE(STACK_ARG(7)));
}

#undef BRIDGE_MAXIMUM_WAIT_OBJECTS

/* ── NtSetSystemTime (ordinal 228, 2 args)
 * Accepts the new time, ignores it, reports the old one through the optional
 * PreviousTime out-parameter (8-byte FILETIME at a guest address). */
static void bridge_NtSetSystemTime(void)
{
    uint32_t prev_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtSetSystemTime(
        XBOX_TO_NATIVE(STACK_ARG(0)),
        prev_va ? (PLARGE_INTEGER)XBOX_TO_NATIVE(prev_va) : NULL);
}

/* ── ObReferenceObjectByName (ordinal 247, 5 args)
 *
 * The ANSI_STRING is rebuilt by hand (native struct has a 64-bit Buffer at
 * offset 8, the guest one a 32-bit Buffer at offset 4) and the Object
 * out-parameter is filled through a local so the 8-byte NULL store lands in a
 * local instead of spilling past a 4-byte guest slot. */
static void bridge_ObReferenceObjectByName(void)
{
    uint32_t name_va   = STACK_ARG(0);
    uint32_t object_va = STACK_ARG(4);
    XBOX_ANSI_STRING name;
    PVOID object_local = NULL;

    if (!name_va) {
        g_eax = (uint32_t)STATUS_INVALID_PARAMETER;
        return;
    }
    name.Length        = BRIDGE_MEM16(name_va);
    name.MaximumLength = BRIDGE_MEM16(name_va + 2);
    name.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(name_va + 4));

    g_eax = (uint32_t)xbox_ObReferenceObjectByName(
        &name, STACK_ARG(1), XBOX_TO_NATIVE(STACK_ARG(2)),
        XBOX_TO_NATIVE(STACK_ARG(3)), &object_local);
    if (object_va)
        BRIDGE_MEM32(object_va) = (uint32_t)(uintptr_t)object_local;
}

/* ── RtlInitUnicodeString (ordinal 290, 2 args)
 *
 * Mirror of the RtlInitAnsiString bridge: the guest UNICODE_STRING is
 * { USHORT Length; USHORT MaximumLength; 32-bit Buffer; } and the Buffer field
 * must carry the guest VA of the source, so the fields are written by hand
 * rather than letting the native xbox_* write a 64-bit host pointer into a
 * 4-byte guest slot. Lengths are in bytes (wide chars x2). */
static void bridge_RtlInitUnicodeString(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);

    if (!dest_va) {
        g_eax = 0;
        return;
    }
    if (src_va) {
        const uint16_t *wp = (const uint16_t *)XBOX_TO_NATIVE(src_va);
        size_t bytes = 0;

        while (wp[bytes / 2])
            bytes += sizeof(uint16_t);
        if (bytes > 0xFFFE)
            bytes = 0xFFFE;
        BRIDGE_MEM16(dest_va + 0) = (uint16_t)bytes;
        BRIDGE_MEM16(dest_va + 2) = (uint16_t)(bytes + sizeof(uint16_t));
        BRIDGE_MEM32(dest_va + 4) = src_va;
    } else {
        BRIDGE_MEM16(dest_va + 0) = 0;
        BRIDGE_MEM16(dest_va + 2) = 0;
        BRIDGE_MEM32(dest_va + 4) = 0;
    }
    g_eax = 0;
}

/* ── RtlTimeFieldsToTime (ordinal 304, 2 args)
 * Counterpart of the routed RtlTimeToTimeFields: reads a XBOX_TIME_FIELDS and
 * writes a LARGE_INTEGER, both at caller-supplied guest addresses. */
static void bridge_RtlTimeFieldsToTime(void)
{
    g_eax = (uint32_t)xbox_RtlTimeFieldsToTime(
        (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PLARGE_INTEGER)XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── PhyGetLinkState (ordinal 252, 1 arg) */
static void bridge_PhyGetLinkState(void)
{
    g_eax = (uint32_t)xbox_PhyGetLinkState((BOOLEAN)STACK_ARG(0));
}

/* ── PhyInitialize (ordinal 253, 2 args) */
static void bridge_PhyInitialize(void)
{
    g_eax = (uint32_t)xbox_PhyInitialize((BOOLEAN)STACK_ARG(0),
                                         XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── RtlTimeToTimeFields (ordinal 305, 2 args) */
static void bridge_RtlTimeToTimeFields(void)
{
    xbox_RtlTimeToTimeFields(
        (PLARGE_INTEGER)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* ── XcSHAInit / XcSHAUpdate / XcSHAFinal (ordinals 335-337) */
static void bridge_XcSHAInit(void)
{
    xbox_XcSHAInit((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_XcSHAUpdate(void)
{
    xbox_XcSHAUpdate((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                     (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(1)),
                     STACK_ARG(2));
    g_eax = 0;
}

static void bridge_XcSHAFinal(void)
{
    xbox_XcSHAFinal((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* ── XcRC4Key / XcRC4Crypt (ordinals 338-339) */
static void bridge_XcRC4Key(void)
{
    xbox_XcRC4Key((PXBOX_RC4_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                  STACK_ARG(1),
                  (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

static void bridge_XcRC4Crypt(void)
{
    xbox_XcRC4Crypt((PXBOX_RC4_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    STACK_ARG(1),
                    (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

/* ── XcHMAC (ordinal 340, 7 args) */
static void bridge_XcHMAC(void)
{
    xbox_XcHMAC((const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1),
                (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
                (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
                (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(6)));
    g_eax = 0;
}

/* ── XcDESKeyParity (ordinal 346, 2 args) */
static void bridge_XcDESKeyParity(void)
{
    xbox_XcDESKeyParity((PUCHAR)XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1));
    g_eax = 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * The rest of the xbox_* implementations that had no route. Each was checked
 * against the memory-model bar: reads or writes only happen at
 * caller-supplied guest addresses, XBOX_TO_NATIVE maps guest NULL to host
 * NULL, and nothing allocates, frees, or hands back a host pointer unless the
 * exception is stated in place. I/O Manager and HAL entries are mostly
 * documented stubs whose whole contract is a return value; the Xc* crypto
 * entries are stubs and guest-buffer operations.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── DbgBreakPoint (ordinal 5, void) */
static void bridge_DbgBreakPoint(void)
{
    xbox_DbgBreakPoint();
    g_eax = 0;
}

/* ── AvSetSavedDataAddress (ordinal 4, 1 arg) */
static void bridge_AvSetSavedDataAddress(void)
{
    xbox_AvSetSavedDataAddress(STACK_ARG(0));
    g_eax = 0;
}

/* ── HalDisableSystemInterrupt (ordinal 39, 2 args) */
static void bridge_HalDisableSystemInterrupt(void)
{
    xbox_HalDisableSystemInterrupt(STACK_ARG(0), (KIRQL)STACK_ARG(1));
    g_eax = 0;
}

/* ── HalInitiateShutdown (ordinal 360, void)
 * The implementation calls ExitProcess, so this never actually returns. */
static void bridge_HalInitiateShutdown(void)
{
    xbox_HalInitiateShutdown();
    g_eax = 0;
}

/* ── HalIsResetOrShutdownPending (ordinal 358, void) */
static void bridge_HalIsResetOrShutdownPending(void)
{
    g_eax = (uint32_t)xbox_HalIsResetOrShutdownPending();
}

/* ── WRITE_PORT_BUFFER_ULONG / WRITE_PORT_BUFFER_USHORT (334 / 333, 3 args)
 * Port I/O stubs that ignore every argument, so pointers pass through without
 * translation -- nothing dereferences them. */
static void bridge_WRITE_PORT_BUFFER_ULONG(void)
{
    xbox_WRITE_PORT_BUFFER_ULONG((PULONG)(uintptr_t)STACK_ARG(0),
        (PULONG)(uintptr_t)STACK_ARG(1), STACK_ARG(2));
    g_eax = 0;
}

static void bridge_WRITE_PORT_BUFFER_USHORT(void)
{
    xbox_WRITE_PORT_BUFFER_USHORT((PUSHORT)(uintptr_t)STACK_ARG(0),
        (PUSHORT)(uintptr_t)STACK_ARG(1), STACK_ARG(2));
    g_eax = 0;
}

/* ── I/O Manager stubs (ordinals 61, 62, 69, 73, 74, 79, 81-87, 359)
 *
 * The Xbox I/O manager is used internally by the XDK libraries we replace, so
 * these are all stubs whose contract is a return value plus occasional writes
 * to caller-supplied guest structures (IRP, IO_STATUS_BLOCK, symlink table).
 * None of them hands back a pointer the title dereferences.
 *
 * IofCallDriver / IofCompleteRequest are __fastcall on Xbox, so their
 * arguments live in ecx/edx, never on the stack -- the arg-size entries are 0
 * and reading STACK_ARG would dereference whatever the caller left there.
 */
static void bridge_IoBuildDeviceIoControlRequest(void)
{
    g_eax = (uint32_t)xbox_IoBuildDeviceIoControlRequest(
        STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
        (BOOLEAN)STACK_ARG(6), bridge_resolve_handle(STACK_ARG(7)),
        (PXBOX_IO_STATUS_BLOCK)XBOX_TO_NATIVE(STACK_ARG(8)));
}

static void bridge_IoBuildSynchronousFsdRequest(void)
{
    g_eax = (uint32_t)(uintptr_t)xbox_IoBuildSynchronousFsdRequest(
        STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)), bridge_resolve_handle(STACK_ARG(5)),
        (PXBOX_IO_STATUS_BLOCK)XBOX_TO_NATIVE(STACK_ARG(6)));
}

static void bridge_IoDeleteSymbolicLink(void)
{
    uint32_t name_va = STACK_ARG(0);
    XBOX_ANSI_STRING name;

    if (!name_va) {
        g_eax = 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
        return;
    }
    name.Length        = BRIDGE_MEM16(name_va + 0);
    name.MaximumLength = BRIDGE_MEM16(name_va + 2);
    name.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(name_va + 4));

    g_eax = (uint32_t)xbox_IoDeleteSymbolicLink(&name);
}

static void bridge_IoInitializeIrp(void)
{
    xbox_IoInitializeIrp(XBOX_TO_NATIVE(STACK_ARG(0)),
        (USHORT)STACK_ARG(1), (CCHAR)STACK_ARG(2));
    g_eax = 0;
}

static void bridge_IoInvalidDeviceRequest(void)
{
    g_eax = (uint32_t)xbox_IoInvalidDeviceRequest(
        XBOX_TO_NATIVE(STACK_ARG(0)), XBOX_TO_NATIVE(STACK_ARG(1)));
}

static void bridge_IoMarkIrpMustComplete(void)
{
    xbox_IoMarkIrpMustComplete(XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_IoSetIoCompletion(void)
{
    g_eax = (uint32_t)xbox_IoSetIoCompletion(
        XBOX_TO_NATIVE(STACK_ARG(0)), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), (NTSTATUS)STACK_ARG(3),
        (ULONG_PTR)STACK_ARG(4));
}

static void bridge_IoStartNextPacket(void)
{
    xbox_IoStartNextPacket(XBOX_TO_NATIVE(STACK_ARG(0)),
        (BOOLEAN)STACK_ARG(1));
    g_eax = 0;
}

static void bridge_IoStartNextPacketByKey(void)
{
    xbox_IoStartNextPacketByKey(XBOX_TO_NATIVE(STACK_ARG(0)),
        (BOOLEAN)STACK_ARG(1), STACK_ARG(2));
    g_eax = 0;
}

static void bridge_IoStartPacket(void)
{
    xbox_IoStartPacket(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)), (PULONG)XBOX_TO_NATIVE(STACK_ARG(2)),
        XBOX_TO_NATIVE(STACK_ARG(3)));
    g_eax = 0;
}

static void bridge_IoSynchronousDeviceIoControlRequest(void)
{
    g_eax = (uint32_t)xbox_IoSynchronousDeviceIoControlRequest(
        STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(6)), (BOOLEAN)STACK_ARG(7));
}

static void bridge_IoSynchronousFsdRequest(void)
{
    g_eax = (uint32_t)xbox_IoSynchronousFsdRequest(
        STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)));
}

static void bridge_IofCallDriver(void)
{
    g_eax = (uint32_t)xbox_IofCallDriver(XBOX_TO_NATIVE(g_ecx),
                                        XBOX_TO_NATIVE(g_edx));
}

static void bridge_IofCompleteRequest(void)
{
    xbox_IofCompleteRequest(XBOX_TO_NATIVE(g_ecx), (CCHAR)g_edx);
    g_eax = 0;
}

/* ── MmLockUnlockPhysicalPage (ordinal 176, 2 args)
 * Page locking is a no-op in user mode; the physical address is a value,
 * never dereferenced. */
static void bridge_MmLockUnlockPhysicalPage(void)
{
    xbox_MmLockUnlockPhysicalPage(STACK_ARG(0), (BOOLEAN)STACK_ARG(1));
    g_eax = 0;
}

/* ── MmFreeSystemMemory (ordinal 172, 2 args)
 * Guest-heap twin of the allocator above; nothing on the host heap has ever
 * seen this address. */
static void bridge_MmFreeSystemMemory(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* ── MmCreateKernelStack / MmDeleteKernelStack (169 / 170, 2 args)
 *
 * Same memory model as the pair above. The Xbox convention is that
 * MmCreateKernelStack returns the TOP of the stack and the delete call
 * receives (StackBase=top, StackLimit=base). Both halves of that come from the
 * guest heap here: create returns base+size, delete frees the base (arg 1). */
static void bridge_MmCreateKernelStack(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t base_va = xbox_HeapAlloc(size, 16);

    g_eax = base_va ? base_va + size : 0;
}

static void bridge_MmDeleteKernelStack(void)
{
    xbox_HeapFree(STACK_ARG(1));
    g_eax = 0;
}

/* ── Xc* remaining crypto (ordinals 341-345, 347-351)
 *
 * Split into two kinds, both of which clear the memory-model bar:
 *  - the public-key / DES / ModExp entries are documented stubs that ignore
 *    their arguments (no Xbox Live, no on-console key derivation), so the
 *    pointers pass through without being dereferenced;
 *  - XcBlockCrypt/XcKeyTable/XcCryptService/XcUpdateCrypto are the same shape.
 * XcVerifyPKCS1Signature is worth singling out: it returns TRUE so that
 * signature checks succeed instead of rebooting the dashboard. */
static void bridge_XcPKGetKeyLen(void)
{
    g_eax = (uint32_t)xbox_XcPKGetKeyLen(XBOX_TO_NATIVE(STACK_ARG(0)));
}

static void bridge_XcPKDecPrivate(void)
{
    g_eax = (uint32_t)xbox_XcPKDecPrivate(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)), XBOX_TO_NATIVE(STACK_ARG(2)));
}

static void bridge_XcPKEncPublic(void)
{
    g_eax = (uint32_t)xbox_XcPKEncPublic(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)), XBOX_TO_NATIVE(STACK_ARG(2)));
}

static void bridge_XcVerifyPKCS1Signature(void)
{
    g_eax = (uint32_t)xbox_XcVerifyPKCS1Signature(
        XBOX_TO_NATIVE(STACK_ARG(0)), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)));
}

static void bridge_XcModExp(void)
{
    g_eax = (uint32_t)xbox_XcModExp((PULONG)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(1)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(2)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(3)), STACK_ARG(4));
}

static void bridge_XcKeyTable(void)
{
    xbox_XcKeyTable(STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

static void bridge_XcBlockCrypt(void)
{
    xbox_XcBlockCrypt(STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), XBOX_TO_NATIVE(STACK_ARG(3)),
        STACK_ARG(4));
    g_eax = 0;
}

static void bridge_XcBlockCryptCBC(void)
{
    xbox_XcBlockCryptCBC(STACK_ARG(0), STACK_ARG(1),
        XBOX_TO_NATIVE(STACK_ARG(2)), XBOX_TO_NATIVE(STACK_ARG(3)),
        XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
        XBOX_TO_NATIVE(STACK_ARG(6)));
    g_eax = 0;
}

static void bridge_XcCryptService(void)
{
    xbox_XcCryptService(STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

static void bridge_XcUpdateCrypto(void)
{
    xbox_XcUpdateCrypto(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* ── RtlRip (ordinal 352, 3 args)
 * The CRT rip/assert path. All three pointers are guest C strings, so each is
 * translated; nothing is written. */
static void bridge_RtlRip(void)
{
    xbox_RtlRip((PCHAR)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PCHAR)XBOX_TO_NATIVE(STACK_ARG(1)),
        (PCHAR)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

/* ── RtlAnsiStringToUnicodeString (ordinal 260, 3 args) ──────────────────
 *
 * NOT a call into xbox_RtlAnsiStringToUnicodeString. That one HeapAllocs on
 * the host heap and stores the resulting 64-bit host pointer into the guest
 * string's 4-byte Buffer field -- the memory-model mismatch. This bridge does
 * the whole conversion by hand:
 *
 *  - Source fields are read from the guest struct (Buffer at offset 4);
 *  - the destination stays wherever the title put it (non-allocating), or is
 *    carved from the guest heap (allocating) and its guest VA stored at
 *    offset 4;
 *  - conversion writes only at guest addresses.
 */
static void bridge_RtlAnsiStringToUnicodeString(void)
{
    uint32_t dst_va  = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint32_t s_len, s_buf_va, d_max, d_buf_va = 0;
    uint32_t unicode_bytes;
    int result;

    if (!dst_va || !src_va) {
        g_eax = 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
        return;
    }
    s_len   = BRIDGE_MEM16(src_va + 0);
    s_buf_va = BRIDGE_MEM32(src_va + 4);
    if (!s_buf_va) {
        g_eax = 0xC000000Du;
        return;
    }
    unicode_bytes = (uint32_t)(s_len + 1) * sizeof(WCHAR);

    d_max = BRIDGE_MEM16(dst_va + 2);
    if (do_alloc) {
        d_buf_va = xbox_HeapAlloc(unicode_bytes, 16);
        if (!d_buf_va) {
            g_eax = 0xC0000017u;   /* STATUS_NO_MEMORY */
            return;
        }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)unicode_bytes;
        BRIDGE_MEM32(dst_va + 4) = d_buf_va;
    } else {
        if (d_max < unicode_bytes) {
            g_eax = 0xC0000205u;   /* STATUS_BUFFER_OVERFLOW */
            return;
        }
        d_buf_va = BRIDGE_MEM32(dst_va + 4);
        if (!d_buf_va) {
            g_eax = 0xC000000Du;
            return;
        }
    }

    result = MultiByteToWideChar(CP_ACP, 0,
        (const char*)XBOX_TO_NATIVE(s_buf_va), (int)s_len,
        (WCHAR*)XBOX_TO_NATIVE(d_buf_va), (int)(unicode_bytes / sizeof(WCHAR)));
    if (result > 0) {
        BRIDGE_MEM16(dst_va + 0) = (uint16_t)(result * sizeof(WCHAR));
        ((WCHAR*)XBOX_TO_NATIVE(d_buf_va))[result] = 0;
        g_eax = 0;
    } else {
        g_eax = 0xC0000001u;   /* STATUS_UNSUCCESSFUL */
    }
}

/* ── RtlUnicodeStringToAnsiString (ordinal 308, 3 args)
 * Mirror of the ANSI→Unicode bridge above. */
static void bridge_RtlUnicodeStringToAnsiString(void)
{
    uint32_t dst_va  = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint32_t s_len, s_buf_va, d_max, d_buf_va = 0;
    uint32_t ansi_bytes;
    int result;

    if (!dst_va || !src_va) {
        g_eax = 0xC000000Du;
        return;
    }
    s_len   = BRIDGE_MEM16(src_va + 0);
    s_buf_va = BRIDGE_MEM32(src_va + 4);
    if (!s_buf_va) {
        g_eax = 0xC000000Du;
        return;
    }
    ansi_bytes = s_len / sizeof(WCHAR) + 1;

    d_max = BRIDGE_MEM16(dst_va + 2);
    if (do_alloc) {
        d_buf_va = xbox_HeapAlloc(ansi_bytes, 16);
        if (!d_buf_va) {
            g_eax = 0xC0000017u;
            return;
        }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)ansi_bytes;
        BRIDGE_MEM32(dst_va + 4) = d_buf_va;
    } else {
        if (d_max < ansi_bytes) {
            g_eax = 0xC0000205u;
            return;
        }
        d_buf_va = BRIDGE_MEM32(dst_va + 4);
        if (!d_buf_va) {
            g_eax = 0xC000000Du;
            return;
        }
    }

    result = WideCharToMultiByte(CP_ACP, 0,
        (const WCHAR*)XBOX_TO_NATIVE(s_buf_va), (int)(s_len / sizeof(WCHAR)),
        (char*)XBOX_TO_NATIVE(d_buf_va), (int)ansi_bytes, NULL, NULL);
    if (result > 0) {
        BRIDGE_MEM16(dst_va + 0) = (uint16_t)result;
        if ((uint16_t)result < d_max)
            ((char*)XBOX_TO_NATIVE(d_buf_va))[result] = 0;
        g_eax = 0;
    } else {
        g_eax = 0xC0000001u;
    }
}

/* ── NtDuplicateObject (ordinal 197, 3 args)
 *
 * Xbox NtDuplicateObject has one process, and the handle table is ours: a
 * by-value source token in, a 4-byte guest slot for the duplicated target.
 * The source resolves through the table, DuplicateHandle produces a real
 * native handle, and the target lands via bridge_write_handle as a fresh
 * token -- same shape as NtCreateEvent. DUPLICATE_CLOSE_SOURCE also closes
 * the native handle behind the source token, which is why the source token is
 * NOT consumed here (the OS handle it wraps is gone after this call). */
static void bridge_NtDuplicateObject(void)
{
    uint32_t target_va = STACK_ARG(1);
    HANDLE src = bridge_resolve_handle(STACK_ARG(0));
    HANDLE dup = NULL;
    DWORD opts = 0;

    if (!target_va) {
        g_eax = 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
        return;
    }
    if (STACK_ARG(2) & 0x1) opts |= DUPLICATE_CLOSE_SOURCE;
    if (STACK_ARG(2) & 0x2) opts |= DUPLICATE_SAME_ACCESS;

    if (!DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(),
                         &dup, 0, FALSE, opts)) {
        g_eax = 0xC0000001u;   /* STATUS_UNSUCCESSFUL */
        return;
    }
    bridge_write_handle(target_va, dup);
    g_eax = 0;
}

/* ── Dispatch table: ordinal → bridge function + stack arg bytes ── */

typedef void (*bridge_func_t)(void);

/**
 * stdcall arg byte count for each kernel ordinal.
 * On x86 stdcall, the callee cleans (ret N). Our bridges must do the same
 * via g_esp += N after execution so the simulated stack stays balanced.
 *
 * Special cases:
 *   - KfRaiseIrql/KfLowerIrql: fastcall (arg in ecx), 0 stack bytes
 *   - KeSetTimer: DueTime is LARGE_INTEGER (8 bytes on stack) + Timer + Dpc
 */
/* ---- Section B: Rtl* bridge functions (ordinals 261-321) ----
 * Drawn from bridges_Rtl.c and spliced here at byte level. Macro and status
 * definitions inside remain #ifndef-guarded so the block still compiles
 * standalone. Routing lives in the two dispatch tables below. */
/* NTSTATUS values kernel.h does not define. */
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL    ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_INTEGER_OVERFLOW
#define STATUS_INTEGER_OVERFLOW    ((NTSTATUS)0xC0000095L)
#endif

/* ── Guest string helpers ─────────────────────────────────── */

/* Byte length (in UTF-16 bytes) of the guest C string at src_va, capped so the
 * caller can always add a terminator without overflowing USHORT Length. */
static uint32_t bridge_guest_wcslen_bytes(uint32_t src_va)
{
    const uint16_t *wp = (const uint16_t *)XBOX_TO_NATIVE(src_va);
    uint32_t bytes = 0;

    while (wp[bytes / 2]) {
        bytes += sizeof(uint16_t);
        if (bytes > 0xFFFE)
            return 0xFFFE;
    }
    return bytes;
}

/* ── Rtl Append (ordinals 261, 262, 263) ────────────────────
 * Each appends a source to a destination XBOX_*_STRING, honouring the
 * destination MaximumLength; the source bump and the destination length both
 * live in guest memory. Returns STATUS_SUCCESS or STATUS_BUFFER_TOO_SMALL. */

/* 261: NTSTATUS RtlAppendStringToString(PXBOX_ANSI_STRING, PXBOX_ANSI_STRING) */
static void bridge_RtlAppendStringToString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_len = 0, d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_len = BRIDGE_MEM16(dst_va + 0);
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (!d_buf || !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if ((uint32_t)d_len + s_len > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
    memcpy((char *)XBOX_TO_NATIVE(d_buf) + d_len, XBOX_TO_NATIVE(s_buf), s_len);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)(d_len + s_len);
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* 262: NTSTATUS RtlAppendUnicodeStringToString(PXBOX_UNICODE_STRING, PXBOX_UNICODE_STRING) */
static void bridge_RtlAppendUnicodeStringToString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_len = 0, d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_len = BRIDGE_MEM16(dst_va + 0);
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (!d_buf || !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if ((uint32_t)d_len + s_len > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
    memcpy((char *)XBOX_TO_NATIVE(d_buf) + d_len, XBOX_TO_NATIVE(s_buf), s_len);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)(d_len + s_len);
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* 263: NTSTATUS RtlAppendUnicodeToString(PXBOX_UNICODE_STRING, PCWSTR) */
static void bridge_RtlAppendUnicodeToString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_len = 0, d_max = 0;
    uint32_t d_buf = 0, add_bytes;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_len = BRIDGE_MEM16(dst_va + 0);
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    if (!d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    add_bytes = bridge_guest_wcslen_bytes(src_va);
    if ((uint32_t)d_len + add_bytes > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
    memcpy((char *)XBOX_TO_NATIVE(d_buf) + d_len, XBOX_TO_NATIVE(src_va), add_bytes);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)(d_len + add_bytes);
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* ── Rtl Compare (ordinals 270, 271, 280) ───────────────────
 * <0 / 0 / >0 against the string bodies (not just the prefix), matching the
 * NT RtlCompare* contract: compare up to the common length, then lengths. */

/* 270: LONG RtlCompareString(PXBOX_ANSI_STRING, PXBOX_ANSI_STRING, BOOLEAN) */
static void bridge_RtlCompareString(void)
{
    uint32_t s1_va = STACK_ARG(0);
    uint32_t s2_va = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    uint32_t l1, l2, common, i;
    const uint8_t *p1, *p2;
    int diff;

    if (!s1_va || !s2_va) { g_eax = 0; return; }
    l1 = BRIDGE_MEM16(s1_va + 0);
    l2 = BRIDGE_MEM16(s2_va + 0);
    p1 = (const uint8_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    p2 = (const uint8_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));
    if (!p1 || !p2) { g_eax = 0; return; }

    common = l1 < l2 ? l1 : l2;
    diff = 0;
    for (i = 0; i < common; i++) {
        int a = p1[i], b = p2[i];
        if (nocase) { a = tolower(a); b = tolower(b); }
        if (a != b) { diff = a - b; break; }
    }
    if (!diff)
        diff = (int)l1 - (int)l2;
    g_eax = (uint32_t)(int32_t)diff;
}

/* 271: LONG RtlCompareUnicodeString(PXBOX_UNICODE_STRING, PXBOX_UNICODE_STRING, BOOLEAN) */
static void bridge_RtlCompareUnicodeString(void)
{
    uint32_t s1_va = STACK_ARG(0);
    uint32_t s2_va = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    uint32_t b1, b2, common, i;
    const uint16_t *p1, *p2;
    int diff;

    if (!s1_va || !s2_va) { g_eax = 0; return; }
    b1 = BRIDGE_MEM16(s1_va + 0);
    b2 = BRIDGE_MEM16(s2_va + 0);
    p1 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    p2 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));
    if (!p1 || !p2) { g_eax = 0; return; }

    common = b1 < b2 ? b1 : b2;
    diff = 0;
    for (i = 0; i < common / 2; i++) {
        int a = p1[i], b = p2[i];
        if (nocase) { a = towlower((wint_t)a); b = towlower((wint_t)b); }
        if (a != b) { diff = a - b; break; }
    }
    if (!diff)
        diff = (int)b1 - (int)b2;
    g_eax = (uint32_t)(int32_t)diff;
}

/* 280: BOOLEAN RtlEqualUnicodeString(PXBOX_UNICODE_STRING, PXBOX_UNICODE_STRING, BOOLEAN) */
static void bridge_RtlEqualUnicodeString(void)
{
    uint32_t s1_va = STACK_ARG(0);
    uint32_t s2_va = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    uint32_t b1, b2, i;
    const uint16_t *p1, *p2;

    if (!s1_va || !s2_va) { g_eax = 0; return; }
    b1 = BRIDGE_MEM16(s1_va + 0);
    b2 = BRIDGE_MEM16(s2_va + 0);
    if (b1 != b2) { g_eax = 0; return; }
    p1 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    p2 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));
    if ((!p1 || !p2) && b1) { g_eax = 0; return; }

    for (i = 0; i < b1 / 2; i++) {
        int a = p1[i], b = p2[i];
        if (nocase) {
            a = towlower((wint_t)a);
            b = towlower((wint_t)b);
        }
        if (a != b) { g_eax = 0; return; }
    }
    g_eax = 1;
}

/* ── Rtl Copy (ordinals 272, 273, 317) ────────────────────── */

/* 272: VOID RtlCopyString(PXBOX_ANSI_STRING Dest, PXBOX_ANSI_STRING Src) */
static void bridge_RtlCopyString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0, copy;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    copy = s_len < d_max ? s_len : d_max;
    if (copy && d_buf && s_buf)
        memcpy(XBOX_TO_NATIVE(d_buf), XBOX_TO_NATIVE(s_buf), copy);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)copy;
    g_eax = 0;
}

/* 273: VOID RtlCopyUnicodeString(PXBOX_UNICODE_STRING Dest, PXBOX_UNICODE_STRING Src) */
static void bridge_RtlCopyUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0, copy;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    copy = s_len < d_max ? s_len : d_max;
    if (copy && d_buf && s_buf)
        memcpy(XBOX_TO_NATIVE(d_buf), XBOX_TO_NATIVE(s_buf), copy);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)copy;
    g_eax = 0;
}

/* 317: VOID RtlUpperString(PXBOX_ANSI_STRING Dest, PXBOX_ANSI_STRING Src) */
static void bridge_RtlUpperString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0, copy, i;
    const uint8_t *sp;
    uint8_t *dp;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    copy = s_len < d_max ? s_len : d_max;
    if (copy && d_buf && s_buf) {
        sp = (const uint8_t *)XBOX_TO_NATIVE(s_buf);
        dp = (uint8_t *)XBOX_TO_NATIVE(d_buf);
        for (i = 0; i < copy; i++)
            dp[i] = (uint8_t)toupper(sp[i]);
    }
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)copy;
    g_eax = 0;
}

/* ── Rtl Create / Free string (ordinals 274, 287) ─────────── */

/* 274: BOOLEAN RtlCreateUnicodeString(PXBOX_UNICODE_STRING Dest, PCWSTR Src)
 * The buffer is carved from the guest heap so the Buffer field stays a guest
 * VA the title can dereference.  Returns TRUE on success. */
static void bridge_RtlCreateUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint32_t bytes, buf_va;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    bytes = bridge_guest_wcslen_bytes(src_va);
    buf_va = xbox_HeapAlloc(bytes + sizeof(uint16_t), 16);
    if (!buf_va) { g_eax = 0; return; }
    memcpy(XBOX_TO_NATIVE(buf_va), XBOX_TO_NATIVE(src_va), bytes);
    ((uint16_t *)XBOX_TO_NATIVE(buf_va))[bytes / 2] = 0;
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)bytes;
    BRIDGE_MEM16(dst_va + 2) = (uint16_t)(bytes + sizeof(uint16_t));
    BRIDGE_MEM32(dst_va + 4) = buf_va;
    g_eax = 1;
}

/* 287: VOID RtlFreeUnicodeString(PXBOX_UNICODE_STRING Str) */
static void bridge_RtlFreeUnicodeString(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t buf_va;

    if (!str_va) { g_eax = 0; return; }
    buf_va = BRIDGE_MEM32(str_va + 4);
    if (buf_va)
        xbox_HeapFree(buf_va);
    BRIDGE_MEM16(str_va + 0) = 0;
    BRIDGE_MEM16(str_va + 2) = 0;
    BRIDGE_MEM32(str_va + 4) = 0;
    g_eax = 0;
}

/* ── Rtl Control characters (ordinals 275, 296, 313, 316) ── */

/* 275: WCHAR RtlDowncaseUnicodeChar(WCHAR Ch) */
static void bridge_RtlDowncaseUnicodeChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint16_t)towlower((wint_t)(uint16_t)ch);
}

/* 296: CHAR RtlLowerChar(CHAR Ch) */
static void bridge_RtlLowerChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint8_t)tolower((int)(uint8_t)ch);
}

/* 313: WCHAR RtlUpcaseUnicodeChar(WCHAR Ch) */
static void bridge_RtlUpcaseUnicodeChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint16_t)towupper((wint_t)(uint16_t)ch);
}

/* 316: CHAR RtlUpperChar(CHAR Ch) */
static void bridge_RtlUpperChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint8_t)toupper((int)(uint8_t)ch);
}

/* ── Rtl Upcase/Downcase string (ordinals 276, 314) ─────────
 * NT name suggests (2 args) but the real signature takes a third
 * BOOLEAN AllocateDestString (12 stack bytes).  If AllocateDest is set the
 * destination buffer is carved from the guest heap and its guest VA stored in
 * Dest->Buffer; otherwise the caller's existing buffer must fit. */

/* 276: NTSTATUS RtlDowncaseUnicodeString(Dest, Src, AllocDest) */
static void bridge_RtlDowncaseUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint16_t s_len, d_max = 0;
    uint32_t s_buf, d_buf = 0, i;
    const uint16_t *sp;
    uint16_t *dp;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (s_len && !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    if (do_alloc) {
        d_buf = xbox_HeapAlloc((uint32_t)s_len + sizeof(uint16_t), 16);
        if (!d_buf) { g_eax = (uint32_t)STATUS_NO_MEMORY; return; }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)(s_len + sizeof(uint16_t));
        BRIDGE_MEM32(dst_va + 4) = d_buf;
    } else {
        d_max = BRIDGE_MEM16(dst_va + 2);
        d_buf = BRIDGE_MEM32(dst_va + 4);
        if (d_max < s_len) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
        if (s_len && !d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    }

    sp = (const uint16_t *)XBOX_TO_NATIVE(s_buf);
    dp = (uint16_t *)XBOX_TO_NATIVE(d_buf);
    for (i = 0; i < (uint32_t)(s_len / 2); i++)
        dp[i] = (uint16_t)towlower((wint_t)sp[i]);
    dp[(uint32_t)(s_len / 2)] = 0;
    BRIDGE_MEM16(dst_va + 0) = s_len;
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* 314: NTSTATUS RtlUpcaseUnicodeString(Dest, Src, AllocDest) */
static void bridge_RtlUpcaseUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint16_t s_len, d_max = 0;
    uint32_t s_buf, d_buf = 0, i;
    const uint16_t *sp;
    uint16_t *dp;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (s_len && !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    if (do_alloc) {
        d_buf = xbox_HeapAlloc((uint32_t)s_len + sizeof(uint16_t), 16);
        if (!d_buf) { g_eax = (uint32_t)STATUS_NO_MEMORY; return; }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)(s_len + sizeof(uint16_t));
        BRIDGE_MEM32(dst_va + 4) = d_buf;
    } else {
        d_max = BRIDGE_MEM16(dst_va + 2);
        d_buf = BRIDGE_MEM32(dst_va + 4);
        if (d_max < s_len) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
        if (s_len && !d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    }

    sp = (const uint16_t *)XBOX_TO_NATIVE(s_buf);
    dp = (uint16_t *)XBOX_TO_NATIVE(d_buf);
    for (i = 0; i < (uint32_t)(s_len / 2); i++)
        dp[i] = (uint16_t)towupper((wint_t)sp[i]);
    dp[(uint32_t)(s_len / 2)] = 0;
    BRIDGE_MEM16(dst_va + 0) = s_len;
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* ── Rtl Integer conversion (ordinals 292, 293) ─────────────
 * NT pads RtlIntegerToChar's output to Length with leading zeros and expects
 * Length+1 bytes of buffer (the NUL goes at [Length]). */

/* 292: NTSTATUS RtlIntegerToChar(ULONG Value, ULONG Base, ULONG Length, PCHAR String) */
static void bridge_RtlIntegerToChar(void)
{
    static const char digs[] = "0123456789ABCDEF";
    uint32_t value = STACK_ARG(0);
    uint32_t base  = STACK_ARG(1);
    uint32_t len   = STACK_ARG(2);
    uint32_t str_va = STACK_ARG(3);
    char tmp[40];
    uint32_t i, n;

    if (!str_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (base < 2 || base > 16) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    n = 0;
    {
        uint32_t v = value;
        if (v == 0)
            tmp[n++] = '0';
        while (v) {
            tmp[n++] = digs[v % base];
            v /= base;
        }
        for (i = 0; i < n / 2; i++) {
            char t = tmp[i];
            tmp[i] = tmp[n - 1 - i];
            tmp[n - 1 - i] = t;
        }
    }
    if (n > len) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }

    for (i = 0; i < len - n; i++)
        BRIDGE_MEM8(str_va + i) = (uint8_t)'0';
    for (i = 0; i < n; i++)
        BRIDGE_MEM8(str_va + (len - n + i)) = (uint8_t)tmp[i];
    BRIDGE_MEM8(str_va + len) = 0;
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* 293: NTSTATUS RtlIntegerToUnicodeString(ULONG Value, ULONG Base, PXBOX_UNICODE_STRING String) */
static void bridge_RtlIntegerToUnicodeString(void)
{
    static const uint16_t digs[] = { '0','1','2','3','4','5','6','7','8','9',
                                     'A','B','C','D','E','F' };
    uint32_t value = STACK_ARG(0);
    uint32_t base  = STACK_ARG(1);
    uint32_t str_va = STACK_ARG(2);
    uint16_t body[32], d_max;
    uint32_t d_buf, i, n_body = 0, total;
    uint32_t v;
    int neg = 0;
    uint16_t *dst;

    if (!str_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (base < 2 || base > 16) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_max = BRIDGE_MEM16(str_va + 2);
    d_buf = BRIDGE_MEM32(str_va + 4);
    if (!d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    if (base == 10 && (LONG)value < 0) {
        neg = 1;
        v = (uint32_t)(0u - value);
    } else {
        v = value;
    }
    if (v == 0)
        body[n_body++] = digs[0];
    while (v) {
        body[n_body++] = digs[v % base];
        v /= base;
    }
    for (i = 0; i < n_body / 2; i++) {
        uint16_t t = body[i];
        body[i] = body[n_body - 1 - i];
        body[n_body - 1 - i] = t;
    }

    total = n_body + (uint32_t)neg;
    if (total * 2 > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }

    dst = (uint16_t *)XBOX_TO_NATIVE(d_buf);
    {
        uint32_t k = 0;
        if (neg)
            dst[k++] = (uint16_t)L'-';
        for (i = 0; i < n_body; i++)
            dst[k++] = body[i];
        dst[k] = 0;
    }
    BRIDGE_MEM16(str_va + 0) = (uint16_t)(total * 2);
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* ── Rtl Memory (ordinals 284, 298, 320) ──────────────────── */

/* 284: VOID RtlFillMemory(PVOID Dest, SIZE_T Len, UCHAR Fill) */
static void bridge_RtlFillMemory(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t len     = STACK_ARG(1);
    uint32_t fill    = STACK_ARG(2);

    if (dest_va && len)
        memset(XBOX_TO_NATIVE(dest_va), (int)(UCHAR)fill, (size_t)len);
    g_eax = 0;
}

/* 298: VOID RtlMoveMemory(PVOID Dest, PVOID Src, SIZE_T Len) */
static void bridge_RtlMoveMemory(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);
    uint32_t len     = STACK_ARG(2);

    if (dest_va && src_va && len)
        memmove(XBOX_TO_NATIVE(dest_va), XBOX_TO_NATIVE(src_va), (size_t)len);
    g_eax = 0;
}

/* 320: VOID RtlZeroMemory(PVOID Dest, SIZE_T Len) */
static void bridge_RtlZeroMemory(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t len     = STACK_ARG(1);

    if (dest_va && len)
        memset(XBOX_TO_NATIVE(dest_va), 0, (size_t)len);
    g_eax = 0;
}

/* ── Rtl MultiByte <-> Unicode (ordinals 299, 300, 310, 311, 315) ──
 * All buffers are guest VAs.  MultiByteToWideChar / WideCharToMultiByte are
 * available on both host platforms.  UNICODE_STRING lengths are byte counts. */

/* 299: NTSTATUS RtlMultiByteToUnicodeN(PWCH UnicodeString, ULONG MaxBytesInUnicodeString,
 *       PULONG BytesInUnicodeString, PCCH MultiByteString, ULONG BytesInMultiByteString) */
static void bridge_RtlMultiByteToUnicodeN(void)
{
    uint32_t uni_va    = STACK_ARG(0);
    uint32_t max_bytes = STACK_ARG(1);
    uint32_t bytes_va  = STACK_ARG(2);
    uint32_t mb_va     = STACK_ARG(3);
    uint32_t mb_bytes  = STACK_ARG(4);
    int chars, result;

    if (!uni_va && max_bytes) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    chars = (int)(max_bytes / sizeof(WCHAR));
    result = MultiByteToWideChar(CP_ACP, 0,
        (const char *)XBOX_TO_NATIVE(mb_va), (int)mb_bytes,
        uni_va ? (WCHAR *)XBOX_TO_NATIVE(uni_va) : NULL, chars);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)(result * sizeof(WCHAR)) : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* 300: NTSTATUS RtlMultiByteToUnicodeSize(PULONG BytesInUnicodeString,
 *       PCCH MultiByteString, ULONG BytesInMultiByteString) */
static void bridge_RtlMultiByteToUnicodeSize(void)
{
    uint32_t bytes_va = STACK_ARG(0);
    uint32_t mb_va    = STACK_ARG(1);
    uint32_t mb_bytes = STACK_ARG(2);
    int result;

    result = MultiByteToWideChar(CP_ACP, 0,
        (const char *)XBOX_TO_NATIVE(mb_va), (int)mb_bytes, NULL, 0);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)(result * sizeof(WCHAR)) : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* 310: NTSTATUS RtlUnicodeToMultiByteN(PCHAR MultiByteString, ULONG MaxBytesInMultiByteString,
 *       PULONG BytesInMultiByteString, PCWCH UnicodeString, ULONG BytesInUnicodeString) */
static void bridge_RtlUnicodeToMultiByteN(void)
{
    uint32_t mb_va    = STACK_ARG(0);
    uint32_t max_bytes = STACK_ARG(1);
    uint32_t bytes_va = STACK_ARG(2);
    uint32_t uni_va   = STACK_ARG(3);
    uint32_t uni_bytes = STACK_ARG(4);
    int result;

    if (!mb_va && max_bytes) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    result = WideCharToMultiByte(CP_ACP, 0,
        (const WCHAR *)XBOX_TO_NATIVE(uni_va), (int)(uni_bytes / sizeof(WCHAR)),
        mb_va ? (char *)XBOX_TO_NATIVE(mb_va) : NULL, (int)max_bytes,
        NULL, NULL);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)result : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* 311: NTSTATUS RtlUnicodeToMultiByteSize(PULONG BytesInMultiByteString,
 *       PCWCH UnicodeString, ULONG BytesInUnicodeString) */
static void bridge_RtlUnicodeToMultiByteSize(void)
{
    uint32_t bytes_va = STACK_ARG(0);
    uint32_t uni_va   = STACK_ARG(1);
    uint32_t uni_bytes = STACK_ARG(2);
    int result;

    result = WideCharToMultiByte(CP_ACP, 0,
        (const WCHAR *)XBOX_TO_NATIVE(uni_va), (int)(uni_bytes / sizeof(WCHAR)),
        NULL, 0, NULL, NULL);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)result : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* 315: NTSTATUS RtlUpcaseUnicodeToMultiByteN(PCHAR MultiByteString,
 *       ULONG MaxBytesInUnicodeString, PULONG BytesInMultiByteString,
 *       PCWCH UnicodeString, ULONG BytesInUnicodeString)
 * WideCharToMultiByte has no case flag, so the source is upcased into a
 * temporary host buffer first. */
static void bridge_RtlUpcaseUnicodeToMultiByteN(void)
{
    uint32_t mb_va     = STACK_ARG(0);
    uint32_t max_bytes = STACK_ARG(1);
    uint32_t bytes_va  = STACK_ARG(2);
    uint32_t uni_va    = STACK_ARG(3);
    uint32_t uni_bytes = STACK_ARG(4);
    uint32_t wc = uni_bytes / sizeof(WCHAR);
    uint16_t *tmp = NULL;
    int result;
    uint32_t i;

    if (!mb_va && max_bytes) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (wc) {
        tmp = (uint16_t *)malloc(((size_t)wc + 1) * sizeof(uint16_t));
        if (!tmp) { g_eax = (uint32_t)STATUS_NO_MEMORY; return; }
        {
            const uint16_t *sp = (const uint16_t *)XBOX_TO_NATIVE(uni_va);
            for (i = 0; i < wc; i++)
                tmp[i] = (uint16_t)towupper((wint_t)sp[i]);
            tmp[wc] = 0;
        }
    }
    result = WideCharToMultiByte(CP_ACP, 0,
        tmp, (int)wc,
        mb_va ? (char *)XBOX_TO_NATIVE(mb_va) : NULL, (int)max_bytes,
        NULL, NULL);
    free(tmp);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)result : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* ── Rtl String -> Integer (ordinals 267, 309) ────────────── */

/* 267: NTSTATUS RtlCharToInteger(PCSTR Str, ULONG Base, PULONG Value)
 * Base 0 autodetects (0x -> 16, 0 -> 8, else 10), mirroring strtoul. */
static void bridge_RtlCharToInteger(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t base   = STACK_ARG(1);
    uint32_t val_va = STACK_ARG(2);
    const char *p;
    unsigned long long acc = 0;
    uint32_t base_ = base;
    int neg = 0;
    uint32_t st = 0;   /* STATUS_SUCCESS */

    if (!val_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (base_ != 0 && (base_ < 2 || base_ > 16)) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (!str_va) { BRIDGE_MEM32(val_va) = 0; g_eax = 0; return; }
    p = (const char *)XBOX_TO_NATIVE(str_va);

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
           *p == '\f' || *p == '\v')
        p++;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') p++;

    if (base_ == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base_ = 16; p += 2; }
        else if (p[0] == '0') { base_ = 8; }
        else base_ = 10;
    }

    for (; *p; p++) {
        int d, c = (unsigned char)*p;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= (int)base_) break;
        if (acc > (0xFFFFFFFFULL - (unsigned)d) / base_) {
            st = (uint32_t)STATUS_INTEGER_OVERFLOW;
            acc = 0xFFFFFFFFULL;
            break;
        }
        acc = acc * base_ + (unsigned)d;
    }
    if (neg)
        acc = (unsigned long long)(uint32_t)(0u - (uint32_t)acc);
    BRIDGE_MEM32(val_va) = (uint32_t)acc;
    g_eax = st;
}

/* 309: NTSTATUS RtlUnicodeStringToInteger(PXBOX_UNICODE_STRING Str, ULONG Base, PULONG Value) */
static void bridge_RtlUnicodeStringToInteger(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t base   = STACK_ARG(1);
    uint32_t val_va = STACK_ARG(2);
    uint16_t s_len;
    uint32_t s_buf;
    const uint16_t *wp;
    unsigned long long acc = 0;
    uint32_t base_ = base;
    int neg = 0, i, nch;
    uint32_t st = 0;   /* STATUS_SUCCESS */

    if (!val_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (base_ != 0 && (base_ < 2 || base_ > 16)) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (!str_va) { BRIDGE_MEM32(val_va) = 0; g_eax = 0; return; }
    s_len = BRIDGE_MEM16(str_va + 0);
    s_buf = BRIDGE_MEM32(str_va + 4);
    if (!s_buf) { BRIDGE_MEM32(val_va) = 0; g_eax = 0; return; }
    wp = (const uint16_t *)XBOX_TO_NATIVE(s_buf);
    nch = s_len / 2;

    i = 0;
    while (i < nch && (wp[i] == (uint16_t)' ' || wp[i] == (uint16_t)'\t' ||
                       wp[i] == (uint16_t)'\n' || wp[i] == (uint16_t)'\r'))
        i++;
    if (i < nch && wp[i] == (uint16_t)'-') { neg = 1; i++; }
    else if (i < nch && wp[i] == (uint16_t)'+') i++;

    if (base_ == 0) {
        if (i + 1 < nch && wp[i] == (uint16_t)'0' &&
            (wp[i + 1] == (uint16_t)'x' || wp[i + 1] == (uint16_t)'X')) {
            base_ = 16;
            i += 2;
        } else if (i < nch && wp[i] == (uint16_t)'0') {
            base_ = 8;
        } else {
            base_ = 10;
        }
    }

    for (; i < nch; i++) {
        uint16_t c = wp[i];
        int d;
        if (c >= (uint16_t)'0' && c <= (uint16_t)'9') d = c - (uint16_t)'0';
        else if (c >= (uint16_t)'a' && c <= (uint16_t)'f') d = c - (uint16_t)'a' + 10;
        else if (c >= (uint16_t)'A' && c <= (uint16_t)'F') d = c - (uint16_t)'A' + 10;
        else break;
        if (d >= (int)base_) break;
        if (acc > (0xFFFFFFFFULL - (unsigned)d) / base_) {
            st = (uint32_t)STATUS_INTEGER_OVERFLOW;
            acc = 0xFFFFFFFFULL;
            break;
        }
        acc = acc * base_ + (unsigned)d;
    }
    if (neg)
        acc = (unsigned long long)(uint32_t)(0u - (uint32_t)acc);
    BRIDGE_MEM32(val_va) = (uint32_t)acc;
    g_eax = st;
}

/* ── Rtl Byte swaps (ordinals 307, 318) ─────────────────────
 * Hand-rolled rather than MSVC _byteswap_* so the file stays portable. */

/* 307: ULONG RtlUlongByteSwap(ULONG Value) */
static void bridge_RtlUlongByteSwap(void)
{
    uint32_t v = STACK_ARG(0);
    g_eax = ((v & 0x000000FFu) << 24) |
            ((v & 0x0000FF00u) << 8)  |
            ((v & 0x00FF0000u) >> 8)  |
            ((v & 0xFF000000u) >> 24);
}

/* 318: USHORT RtlUshortByteSwap(USHORT Value) */
static void bridge_RtlUshortByteSwap(void)
{
    uint32_t v = STACK_ARG(0);
    g_eax = (uint32_t)(uint16_t)(((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu));
}

/* ── Rtl Critical sections (ordinals 278, 295, 306) ─────────
 * 277/294 (plain Enter/Leave) are routed via the xbox_Rtl* wrappers already.
 *
 * TryEnter cannot reuse xbox_cs_shadow (it is private to kernel_rtl.c), so it
 * keeps its own keyed host-lock table: guest critical-section address -> host
 * CRITICAL_SECTION, created and probed under a guard.  Only the try path uses
 * it, so entering with Try and leaving with LeaveCriticalSection are both
 * routed at the shadow level and stay consistent on the host side. */

#define BRIDGE_TRY_CS_SLOTS 512

typedef struct {
    uint32_t key;             /* guest critical section; 0 means free */
    int      ready;
    CRITICAL_SECTION cs;
} BRIDGE_TRY_CS_SLOT;

static BRIDGE_TRY_CS_SLOT g_try_cs[BRIDGE_TRY_CS_SLOTS];
static CRITICAL_SECTION g_try_cs_guard;
static int g_try_cs_guard_ready;

static void bridge_try_cs_init_guard(void)
{
    /* First call in practice comes from the title's main thread during setup;
     * a second InitializeCriticalSection on the same object is benign. */
    if (!g_try_cs_guard_ready) {
        InitializeCriticalSection(&g_try_cs_guard);
        g_try_cs_guard_ready = 1;
    }
}

static CRITICAL_SECTION *bridge_try_cs_shadow(uint32_t guest)
{
    size_t home, i;

    bridge_try_cs_init_guard();
    EnterCriticalSection(&g_try_cs_guard);
    home = (size_t)((guest >> 4) % BRIDGE_TRY_CS_SLOTS);
    for (i = 0; i < BRIDGE_TRY_CS_SLOTS; i++) {
        BRIDGE_TRY_CS_SLOT *slot = &g_try_cs[(home + i) % BRIDGE_TRY_CS_SLOTS];
        if (slot->key == guest) {
            LeaveCriticalSection(&g_try_cs_guard);
            return &slot->cs;
        }
        if (slot->key == 0) {
            if (!slot->ready) {
                InitializeCriticalSection(&slot->cs);
                slot->ready = 1;
            }
            slot->key = guest;
            LeaveCriticalSection(&g_try_cs_guard);
            return &slot->cs;
        }
    }
    LeaveCriticalSection(&g_try_cs_guard);
    return NULL;
}

/* 278: VOID RtlEnterCriticalSectionAndRegion(PRTL_CRITICAL_SECTION) */
static void bridge_RtlEnterCriticalSectionAndRegion(void)
{
    xbox_RtlEnterCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

/* 295: VOID RtlLeaveCriticalSectionAndRegion(PRTL_CRITICAL_SECTION) */
static void bridge_RtlLeaveCriticalSectionAndRegion(void)
{
    xbox_RtlLeaveCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

/* 306: BOOLEAN RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION) */
static void bridge_RtlTryEnterCriticalSection(void)
{
    CRITICAL_SECTION *cs = bridge_try_cs_shadow(STACK_ARG(0));
    if (!cs) { g_eax = 0; return; }
    g_eax = TryEnterCriticalSection(cs) ? 1 : 0;
}

/* ── Rtl Misc stubs (ordinals 288, 297, 319, 321) ─────────── */

/* 288: VOID RtlGetCallersAddress(PVOID *CallersAddress, PVOID *CallersCaller) */
static void bridge_RtlGetCallersAddress(void)
{
    uint32_t a_va = STACK_ARG(0);
    uint32_t c_va = STACK_ARG(1);

    if (a_va) BRIDGE_MEM32(a_va) = 0;
    if (c_va) BRIDGE_MEM32(c_va) = 0;
    g_eax = 0;
}

/* 297: VOID RtlMapGenericMask(PACCESS_MASK AccessMask, PRTL_GENERIC_MAPPING) */
static void bridge_RtlMapGenericMask(void)
{
    g_eax = 0;
}

/* 319: ULONG RtlWalkFrameChain(PVOID *Callers, ULONG Count, ULONG Flags) */
static void bridge_RtlWalkFrameChain(void)
{
    g_eax = 0;
}

/* 321: XboxEEPROMKey (data export, 0 args)
 * Not in the kernel_data_va_for_ordinal table, so it needs a bridge that hands
 * the caller a guest-addressable 16-byte key.  Carved from the guest heap once. */
static void bridge_XboxEEPROMKey(void)
{
    static uint32_t eeprom_va = 0;

    if (!eeprom_va) {
        eeprom_va = xbox_HeapAlloc(16, 16);
        if (eeprom_va)
            memset(XBOX_TO_NATIVE(eeprom_va), 0, 16);
    }
    g_eax = eeprom_va;
}

/* ── Rtl Exception (ordinals 264, 265, 266, 303) ──────────── */

/* 264: VOID RtlAssert(PVOID FailedAssertion, PVOID FileName, ULONG LineNumber) */
static void bridge_RtlAssert(void)
{
    uint32_t assert_va = STACK_ARG(0);
    uint32_t file_va   = STACK_ARG(1);
    uint32_t line      = STACK_ARG(2);
    const char *a = assert_va ? (const char *)XBOX_TO_NATIVE(assert_va) : NULL;
    const char *f = file_va   ? (const char *)XBOX_TO_NATIVE(file_va) : NULL;

    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_RTL, "RtlAssert: %s:%u %s",
             f ? f : "?", line, a ? a : "?");
#ifdef _DEBUG
    DebugBreak();
#endif
    g_eax = 0;
}

/* 265: VOID RtlCaptureContext(PCONTEXT Context) - stub: zero the x86 CONTEXT. */
static void bridge_RtlCaptureContext(void)
{
    uint32_t ctx_va = STACK_ARG(0);

    if (ctx_va)
        memset(XBOX_TO_NATIVE(ctx_va), 0, 0x2CC);
    g_eax = 0;
}

/* 266: ULONG RtlCaptureStackBackTrace(FramesToSkip, FramesToCapture,
 *       BackTrace, BackTraceHash) - stub, returns 0 frames. Stack is cleaned
 *       up per stdcall_args_for_ordinal (20 bytes in kernel_bridge.c). */
static void bridge_RtlCaptureStackBackTrace(void)
{
    g_eax = 0;
}


/* --- Ke Object Shadow Table --- */
#define KE_SHADOW_SIZE 4096
typedef struct { uint32_t guest_va; HANDLE host_handle; uint32_t in_use; } KE_SHADOW_SLOT;
static KE_SHADOW_SLOT g_ke_shadow[KE_SHADOW_SIZE];
static CRITICAL_SECTION g_ke_shadow_cs;
static int g_ke_shadow_init;

static void ke_shadow_init(void)
{
    if (g_ke_shadow_init)
        return;
    InitializeCriticalSection(&g_ke_shadow_cs);
    /* Wipe any stale pointers from a prior lifecycle (defensive). */
    memset(g_ke_shadow, 0, sizeof(g_ke_shadow));
    g_ke_shadow_init = 1;
}

/* Insert a mapping guest_va -> host HANDLE. Replaces any prior mapping for
 * the same guest_va (closing the old handle and inserting the new one). */
static void ke_shadow_insert(uint32_t guest_va, HANDLE host)
{
    uint32_t i;

    if (!guest_va || !host)
        return;

    ke_shadow_init();

    EnterCriticalSection(&g_ke_shadow_cs);

    /* Reuse an existing slot for this VA, if present. */
    for (i = 0; i < KE_SHADOW_SIZE; i++) {
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va == guest_va) {
            if (g_ke_shadow[i].host_handle && g_ke_shadow[i].host_handle != host)
                CloseHandle(g_ke_shadow[i].host_handle);
            g_ke_shadow[i].host_handle = host;
            LeaveCriticalSection(&g_ke_shadow_cs);
            return;
        }
    }

    /* Otherwise find a free slot. */
    for (i = 0; i < KE_SHADOW_SIZE; i++) {
        if (!g_ke_shadow[i].in_use) {
            g_ke_shadow[i].in_use      = 1;
            g_ke_shadow[i].guest_va    = guest_va;
            g_ke_shadow[i].host_handle = host;
            LeaveCriticalSection(&g_ke_shadow_cs);
            return;
        }
    }

    /* Table full -- log and drop the new object. The caller's subsequent
     * waits will fall back to XBOX_TO_NATIVE and fail gracefully. */
    fprintf(stderr, "  [KERNEL] Ke shadow table full; dropping VA 0x%08X\n",
            guest_va);
    LeaveCriticalSection(&g_ke_shadow_cs);
    CloseHandle(host);
}

/* Look up the host HANDLE for a guest object VA. Returns NULL if not found. */
static HANDLE ke_shadow_lookup(uint32_t guest_va)
{
    uint32_t i;
    HANDLE h = NULL;

    if (!guest_va)
        return NULL;

    ke_shadow_init();

    EnterCriticalSection(&g_ke_shadow_cs);
    for (i = 0; i < KE_SHADOW_SIZE; i++) {
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va == guest_va) {
            h = g_ke_shadow[i].host_handle;
            break;
        }
    }
    LeaveCriticalSection(&g_ke_shadow_cs);
    return h;
}

/* Remove (and close) the mapping for a guest object VA. */
static void ke_shadow_remove(uint32_t guest_va)
{
    uint32_t i;

    if (!guest_va)
        return;

    ke_shadow_init();

    EnterCriticalSection(&g_ke_shadow_cs);
    for (i = 0; i < KE_SHADOW_SIZE; i++) {
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va == guest_va) {
            if (g_ke_shadow[i].host_handle)
                CloseHandle(g_ke_shadow[i].host_handle);
            g_ke_shadow[i].in_use      = 0;
            g_ke_shadow[i].guest_va    = 0;
            g_ke_shadow[i].host_handle = NULL;
            break;
        }
    }
    LeaveCriticalSection(&g_ke_shadow_cs);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Data Exports (102, 120, 154, 240, 245, 249)
 *
 * These ordinals are DATA exports, not functions. The bridge returns a guest
 * VA inside the kernel data page so the title can dereference it. The offsets
 * below (0x510-0x558) are free in xbox_memory_layout.h's KDATA block (existing
 * slots end at 0x4B0+64, KiBugCheckData uses 0x500, page is 4 KB). Prefer
 * moving them into xbox_memory_layout.h and dropping the guarded defines here.
 * ═══════════════════════════════════════════════════════════════════════════
 */
#ifndef KDATA_MMGLOBAL
#define KDATA_MMGLOBAL          0x510  /* MmGlobalData (4 bytes) */
#endif
#ifndef KDATA_INTERRUPT_TIME
#define KDATA_INTERRUPT_TIME    0x520  /* KeInterruptTime (LARGE_INTEGER, 8 bytes) */
#endif
#ifndef KDATA_SYSTEM_TIME
#define KDATA_SYSTEM_TIME       0x530  /* KeSystemTime (LARGE_INTEGER, 8 bytes) */
#endif
#ifndef KDATA_OBJ_DIR_TYPE
#define KDATA_OBJ_DIR_TYPE      0x540  /* ObDirectoryObjectType (4 bytes) */
#endif
#ifndef KDATA_OBJ_HANDLE_TABLE
#define KDATA_OBJ_HANDLE_TABLE  0x548  /* ObpObjectHandleTable (4 bytes) */
#endif
#ifndef KDATA_OBJ_SYM_LINK_TYPE
#define KDATA_OBJ_SYM_LINK_TYPE 0x550  /* ObSymbolicLinkObjectType (4 bytes) */
#endif

/* --- MmGlobalData (ordinal 102, 0 args) --- */
static void bridge_MmGlobalData(void)
{
    /* Titles rarely dereference this; give it a stable, mapped address that
     * reads back the address itself (self-referential placeholder). */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_MMGLOBAL) =
        XBOX_KERNEL_DATA_BASE + KDATA_MMGLOBAL;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_MMGLOBAL;
}

/* --- KeInterruptTime (ordinal 120, 0 args) --- */
static void bridge_KeInterruptTime(void)
{
    /* Refresh the 100ns-since-boot counter before handing out the pointer. */
    ULONGLONG t = (ULONGLONG)GetTickCount64() * 10000ull;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_INTERRUPT_TIME + 0) = (uint32_t)t;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_INTERRUPT_TIME + 4) = (uint32_t)(t >> 32);
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_INTERRUPT_TIME;
}

/* --- KeSystemTime (ordinal 154, 0 args) --- */
static void bridge_KeSystemTime(void)
{
    /* Refresh the 100ns-since-1601 clock before handing out the pointer. */
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SYSTEM_TIME + 0) = ft.dwLowDateTime;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SYSTEM_TIME + 4) = ft.dwHighDateTime;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_SYSTEM_TIME;
}

/* --- ObDirectoryObjectType (ordinal 240, 0 args) --- */
static void bridge_ObDirectoryObjectType(void)
{
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_OBJ_DIR_TYPE) =
        XBOX_KERNEL_DATA_BASE + KDATA_OBJ_DIR_TYPE;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_OBJ_DIR_TYPE;
}

/* --- ObpObjectHandleTable (ordinal 245, 0 args) --- */
static void bridge_ObpObjectHandleTable(void)
{
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_OBJ_HANDLE_TABLE) =
        XBOX_KERNEL_DATA_BASE + KDATA_OBJ_HANDLE_TABLE;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_OBJ_HANDLE_TABLE;
}

/* --- ObSymbolicLinkObjectType (ordinal 249, 0 args) --- */
static void bridge_ObSymbolicLinkObjectType(void)
{
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_OBJ_SYM_LINK_TYPE) =
        XBOX_KERNEL_DATA_BASE + KDATA_OBJ_SYM_LINK_TYPE;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_OBJ_SYM_LINK_TYPE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Dbg (6, 7, 8, 10, 11)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- DbgBreakPointWithStatus (ordinal 6, 1 arg = 4 bytes) --- */
static void bridge_DbgBreakPointWithStatus(void)
{
    (void)STACK_ARG(0);
    DebugBreak();
    g_eax = 0;
}

/* --- DbgLoadImageSymbols (ordinal 7, 3 args = 12 bytes) --- */
static void bridge_DbgLoadImageSymbols(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

/* --- DbgPrint (ordinal 8, __cdecl varargs, 0 fixed args) --- */
static void bridge_DbgPrint(void)
{
    /* First vararg is a guest format string. Log it verbatim (never treat it
     * as a format, it is guest-owned); caller cleans the stack (__cdecl). */
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(0));
    if (fmt && KERNEL_LOG_ON()) {
        size_t i, n = 0;
        for (i = 0; i < 512 && fmt[i]; i++)
            n++;
        fwrite(fmt, 1, n, stderr);
        fputc('\n', stderr);
        fflush(stderr);
    }
    g_eax = 0;
}

/* --- DbgPrompt (ordinal 10, 2 args = 8 bytes) --- */
static void bridge_DbgPrompt(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- DbgUnLoadImageSymbols (ordinal 11, 3 args = 12 bytes) --- */
static void bridge_DbgUnLoadImageSymbols(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Ex (12, 13, 18, 19, 20, 21, 25, 26, 27, 28)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- ExAcquireReadWriteLockExclusive (ordinal 12, 1 arg = 4 bytes) --- */
static void bridge_ExAcquireReadWriteLockExclusive(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- ExAcquireReadWriteLockShared (ordinal 13, 1 arg = 4 bytes) --- */
static void bridge_ExAcquireReadWriteLockShared(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- ExInitializeReadWriteLock (ordinal 18, 1 arg = 4 bytes) --- */
static void bridge_ExInitializeReadWriteLock(void)
{
    uint32_t lock_va = STACK_ARG(0);
    if (lock_va)
        memset(XBOX_TO_NATIVE(lock_va), 0, 24);
    g_eax = 0;
}

/* --- ExInterlockedAddLargeInteger (ordinal 19, 3 args = 12 bytes)
 * LARGE_INTEGER ExInterlockedAddLargeInteger(PLARGE_INTEGER Addend,
 *                                            LARGE_INTEGER Increment);
 * Addend (4) + Increment.Low (4) + Increment.High (4). Returns the PREVIOUS
 * value of Addend in edx:eax. */
static void bridge_ExInterlockedAddLargeInteger(void)
{
    uint32_t addend_va = STACK_ARG(0);
    LONGLONG increment = ((LONGLONG)(uint32_t)STACK_ARG(2) << 32)
                       | (LONGLONG)(uint32_t)STACK_ARG(1);
    volatile LONG *p = (volatile LONG *)XBOX_TO_NATIVE(addend_va);
    LONGLONG old = 0;

    if (p) {
        if (((uintptr_t)p & 7) == 0) {
            volatile LONGLONG *qp = (volatile LONGLONG *)p;
            LONGLONG cmp = *qp;
            for (;;) {
                LONGLONG desired = cmp + increment;
                LONGLONG actual = InterlockedCompareExchange64(qp, desired, cmp);
                if (actual == cmp)
                    break;
                cmp = actual;
            }
            old = cmp;
        } else {
            /* Unaligned: single-threaded guest, plain RMW is good enough. */
            old = ((LONGLONG)(uint32_t)p[1] << 32) | (uint32_t)p[0];
            {
                LONGLONG newv = old + increment;
                p[0] = (LONG)(uint32_t)newv;
                p[1] = (LONG)(uint32_t)((uint64_t)newv >> 32);
            }
        }
    }

    g_eax = (uint32_t)old;
    g_edx = (uint32_t)((uint64_t)old >> 32);
}

/* --- ExInterlockedAddLargeStatistic (ordinal 20, 2 args = 8 bytes)
 * LARGE_INTEGER ExInterlockedAddLargeStatistic(PLARGE_INTEGER Addend,
 *                                              ULONG Increment);
 * Addend (4) + Increment (4). Atomic add; per task spec, returns 0. */
static void bridge_ExInterlockedAddLargeStatistic(void)
{
    uint32_t addend_va = STACK_ARG(0);
    LONG increment = (LONG)STACK_ARG(1);
    volatile LONG *p = (volatile LONG *)XBOX_TO_NATIVE(addend_va);

    if (p) {
        if (((uintptr_t)p & 7) == 0) {
            volatile LONGLONG *qp = (volatile LONGLONG *)p;
            LONGLONG cmp = *qp;
            for (;;) {
                LONGLONG desired = cmp + (LONGLONG)increment;
                LONGLONG actual = InterlockedCompareExchange64(qp, desired, cmp);
                if (actual == cmp)
                    break;
                cmp = actual;
            }
        } else {
            LONGLONG val = ((LONGLONG)(uint32_t)p[1] << 32) | (uint32_t)p[0];
            LONGLONG newv = val + (LONGLONG)increment;
            p[0] = (LONG)(uint32_t)newv;
            p[1] = (LONG)(uint32_t)((uint64_t)newv >> 32);
        }
    }
    g_eax = 0;
}

/* --- ExInterlockedCompareExchange64 (ordinal 21, 4 args = 16 bytes)
 * LONGLONG ExInterlockedCompareExchange64(PLONGLONG Destination,
 *                                         LONGLONG Exchange,
 *                                         PLONGLONG Comparand);
 * Arg layout on a 32-bit stack is Destination (4) + Exchange (8) +
 * Comparand.Low (4) -- the Comparand high dword is not passed, so the
 * comparison is against a zero-extended 32-bit value. Returns the old value
 * in edx:eax. */
static void bridge_ExInterlockedCompareExchange64(void)
{
    LONGLONG old = 0;
    volatile LONGLONG *qp;
    LONGLONG exchange;
    LONGLONG comparand;

    exchange  = ((LONGLONG)(uint32_t)STACK_ARG(2) << 32)
              | (LONGLONG)(uint32_t)STACK_ARG(1);
    comparand = (LONGLONG)(uint32_t)STACK_ARG(3);
    qp = (volatile LONGLONG *)XBOX_TO_NATIVE(STACK_ARG(0));

    if (qp)
        old = InterlockedCompareExchange64(qp, exchange, comparand);

    g_eax = (uint32_t)old;
    g_edx = (uint32_t)((uint64_t)old >> 32);
}

/* --- ExReadWriteRefurbInfo (ordinal 25, 3 args = 12 bytes) --- */
static void bridge_ExReadWriteRefurbInfo(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

/* --- ExRaiseException (ordinal 26, 1 arg = 4 bytes) --- */
static void bridge_ExRaiseException(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- ExRaiseStatus (ordinal 27, 1 arg = 4 bytes) --- */
static void bridge_ExRaiseStatus(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- ExReleaseReadWriteLock (ordinal 28, 1 arg = 4 bytes) --- */
static void bridge_ExReleaseReadWriteLock(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ExfInterlocked linked-list operations (32, 33, 34)
 *
 * The Xbox ABI keeps a spin-lock around these; with a single-threaded guest
 * the two-pointer fix-up after the atomic head swap is sufficient.
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- ExfInterlockedInsertHeadList (ordinal 32, 2 args = 8 bytes)
 * PLIST_ENTRY ExfInterlockedInsertHeadList(ListHead, Entry);
 * Returns the entry's old Flink (the previous first entry, or ListHead
 * itself for an empty list). */
static void bridge_ExfInterlockedInsertHeadList(void)
{
    uint32_t entry_va = STACK_ARG(0);
    uint32_t head_va  = STACK_ARG(1);
    volatile LONG *h_flink = (volatile LONG *)XBOX_TO_NATIVE(head_va);
    uint32_t old_flink = 0;

    if (h_flink) {
        /* Atomically seat the new head; remember what used to be first. */
        old_flink = (uint32_t)InterlockedExchange(h_flink, (LONG)entry_va);
        BRIDGE_MEM32(entry_va + 0) = old_flink;  /* Entry->Flink = old first */
        BRIDGE_MEM32(entry_va + 4) = head_va;    /* Entry->Blink = head       */
        if (old_flink)
            BRIDGE_MEM32(old_flink + 4) = entry_va; /* old first->Blink = Entry */
    }
    g_eax = old_flink;
}

/* --- ExfInterlockedInsertTailList (ordinal 33, 2 args = 8 bytes)
 * PLIST_ENTRY ExfInterlockedInsertTailList(ListHead, Entry);
 * Returns the previous tail. NT semantics return the old Blink (the entry
 * that was last, or ListHead for an empty list); note the task brief said
 * "old Flink" but the canonical Xbox/NT behaviour is the old Blink. */
static void bridge_ExfInterlockedInsertTailList(void)
{
    uint32_t entry_va = STACK_ARG(0);
    uint32_t head_va  = STACK_ARG(1);
    volatile LONG *h_blink = (volatile LONG *)XBOX_TO_NATIVE(head_va + 4);
    uint32_t old_blink = 0;

    if (h_blink) {
        /* Atomically seat the new tail; remember what used to be last. */
        old_blink = (uint32_t)InterlockedExchange(h_blink, (LONG)entry_va);
        BRIDGE_MEM32(entry_va + 0) = head_va;     /* Entry->Flink = head      */
        BRIDGE_MEM32(entry_va + 4) = old_blink;   /* Entry->Blink = old last  */
        if (old_blink)
            BRIDGE_MEM32(old_blink + 0) = entry_va; /* old last->Flink = Entry */
    }
    g_eax = old_blink;
}

/* --- ExfInterlockedRemoveHeadList (ordinal 34, 1 arg = 4 bytes)
 * PLIST_ENTRY ExfInterlockedRemoveHeadList(ListHead);
 * Returns the removed first entry, or 0 when the list is empty. */
static void bridge_ExfInterlockedRemoveHeadList(void)
{
    uint32_t head_va = STACK_ARG(0);
    volatile LONG *h_flink = (volatile LONG *)XBOX_TO_NATIVE(head_va);
    uint32_t removed = 0;

    if (h_flink) {
        for (;;) {
            uint32_t first = (uint32_t)*h_flink;
            if (first == head_va) {   /* empty list */
                removed = 0;
                break;
            }
            /* Pull Entry = first; Entry's Flink becomes the new head, but only
             * if the head still points at first (single-threaded: it does). */
            {
                uint32_t entry_flink = BRIDGE_MEM32(first + 0);
                if (InterlockedCompareExchange(h_flink,
                                              (LONG)entry_flink,
                                              (LONG)first) == (LONG)first) {
                    if (entry_flink)
                        BRIDGE_MEM32(entry_flink + 4) = head_va;
                    removed = first;
                    break;
                }
            }
        }
    }
    g_eax = removed;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Fsc (36)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- FscInvalidateIdleBlocks (ordinal 36, 0 args = 0 bytes) --- */
static void bridge_FscInvalidateIdleBlocks(void)
{
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Hal (43, 365, 366)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- HalEnableSystemInterrupt (ordinal 43, 2 args = 8 bytes) --- */
static void bridge_HalEnableSystemInterrupt(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- HalEnableSecureTrayEject (ordinal 365, 0 args = 0 bytes) --- */
static void bridge_HalEnableSecureTrayEject(void)
{
    g_eax = 0;
}

/* --- HalWriteSMCScratchRegister (ordinal 366, 1 arg = 4 bytes) --- */
static void bridge_HalWriteSMCScratchRegister(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Interlocked (51-58) -- __fastcall: args travel in ecx/edx/eax, never on
 * the guest stack, so the arg-size entries are all 0 for these. The guest VA
 * in ecx/edx must go through XBOX_TO_NATIVE, exactly like the existing
 * bridge_ObfDereferenceObject.
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- InterlockedCompareExchange (ordinal 51)
 * LONG InterlockedCompareExchange(PLONG Destination, LONG Exchange,
 *                                 LONG Comparand);
 * fastcall: ecx=Destination, edx=Exchange, eax=Comparand-in/old-out. */
static void bridge_InterlockedCompareExchange(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedCompareExchange(dst, (LONG)g_edx, (LONG)g_eax);
}

/* --- InterlockedDecrement (ordinal 52)
 * fastcall: ecx=Destination. */
static void bridge_InterlockedDecrement(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedDecrement(dst);
}

/* --- InterlockedIncrement (ordinal 53)
 * fastcall: ecx=Destination. */
static void bridge_InterlockedIncrement(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedIncrement(dst);
}

/* --- InterlockedExchange (ordinal 54)
 * fastcall: ecx=Destination, edx=Value. */
static void bridge_InterlockedExchange(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedExchange(dst, (LONG)g_edx);
}

/* --- InterlockedExchangeAdd (ordinal 55)
 * fastcall: ecx=Destination, edx=Value. */
static void bridge_InterlockedExchangeAdd(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedExchangeAdd(dst, (LONG)g_edx);
}

/* --- InterlockedFlushSList (ordinal 56, fastcall) --- */
static void bridge_InterlockedFlushSList(void)
{
    (void)g_ecx;
    g_eax = 0;
}

/* --- InterlockedPopEntrySList (ordinal 57, fastcall) --- */
static void bridge_InterlockedPopEntrySList(void)
{
    (void)g_ecx;
    g_eax = 0;
}

/* --- InterlockedPushEntrySList (ordinal 58, fastcall) --- */
static void bridge_InterlockedPushEntrySList(void)
{
    (void)g_ecx;
    (void)g_edx;
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Io (59, 60, 63, 72, 75, 76, 77, 78, 80, 90, 91)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- IoAllocateIrp (ordinal 59, 2 args = 8 bytes)
 * PIRP IoAllocateIrp(CCHAR StackSize, BOOLEAN ChargeQuota);
 * Allocates an IRP header plus StackSize IO_STACK_LOCATIONs from the guest
 * heap. Pair with IoFreeIrp (ordinal 72), which frees through the same heap. */
static void bridge_IoAllocateIrp(void)
{
    uint32_t stack_size = STACK_ARG(0);
    uint32_t charge_quota = STACK_ARG(1);
    uint32_t total = 0x28 + stack_size * 0x20;  /* header + stack locations */
    uint32_t va;

    (void)charge_quota;
    va = xbox_HeapAlloc(total, 16);
    if (va)
        memset((void *)((uintptr_t)va + g_xbox_mem_offset), 0, total);
    else
        fprintf(stderr, "  [KERNEL] IoAllocateIrp: %u bytes REFUSED\n", total);
    g_eax = va;
}

/* --- IoBuildAsynchronousFsdRequest (ordinal 60, 7 args = 28 bytes) --- */
static void bridge_IoBuildAsynchronousFsdRequest(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    (void)STACK_ARG(6);
    g_eax = 0;
}

/* --- IoCheckShareAccess (ordinal 63, 5 args = 20 bytes) --- */
static void bridge_IoCheckShareAccess(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

/* --- IoFreeIrp (ordinal 72, 1 arg = 4 bytes) --- */
static void bridge_IoFreeIrp(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* --- IoQueryFileInformation (ordinal 75, 5 args = 20 bytes) --- */
static void bridge_IoQueryFileInformation(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0xC000000D;  /* STATUS_INVALID_PARAMETER */
}

/* --- IoQueryVolumeInformation (ordinal 76, 5 args = 20 bytes) --- */
static void bridge_IoQueryVolumeInformation(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0xC000000D;  /* STATUS_INVALID_PARAMETER */
}

/* --- IoQueueThreadIrp (ordinal 77, 1 arg = 4 bytes) --- */
static void bridge_IoQueueThreadIrp(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- IoRemoveShareAccess (ordinal 78, 2 args = 8 bytes) --- */
static void bridge_IoRemoveShareAccess(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
}

/* --- IoSetShareAccess (ordinal 80, 5 args = 20 bytes) --- */
static void bridge_IoSetShareAccess(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
}

/* --- IoDismountVolume (ordinal 90, 1 arg = 4 bytes) --- */
static void bridge_IoDismountVolume(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- IoDismountVolumeByName (ordinal 91, 1 arg = 4 bytes) --- */
static void bridge_IoDismountVolumeByName(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Kd (88, 89)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- KdDebuggerEnabled (ordinal 88, 0 args = 0 bytes) --- */
static void bridge_KdDebuggerEnabled(void)
{
    g_eax = 0;  /* not connected */
}

/* --- KdDebuggerNotPresent (ordinal 89, 0 args = 0 bytes) --- */
static void bridge_KdDebuggerNotPresent(void)
{
    g_eax = 1;  /* true: no debugger present */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Ke -- shadow-table object bridges (92-163)
 *
 * The KeInitialize* family creates a real Win32 object, records guest-VA ->
 * host-HANDLE in the shadow table, and zeroes/initialises the guest dispatcher
 * header. The KePulse/Release/Reset/Set family resolves back through the
 * shadow table (with XBOX_TO_NATIVE as a fallback for legacy callers).
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- KeInitializeEvent (ordinal 108, 3 args = 12 bytes) --- */
static void bridge_KeInitializeEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t event_type = STACK_ARG(1); /* 0=Notification(manual-reset), 1=Synchronization(auto-reset) */
    uint32_t initial_state = STACK_ARG(2);

    BOOL manual_reset = (event_type == 0) ? TRUE : FALSE;
    HANDLE h = CreateEventW(NULL, manual_reset, initial_state ? TRUE : FALSE, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] KeInitializeEvent: CreateEventW failed (error %u)\n",
                GetLastError());
        g_eax = 0;
        return;
    }

    ke_shadow_insert(guest_va, h);

    /* Also zero the guest KEVENT struct (16 bytes) and set SignalState.
     * KEVENT layout: +0x00 Type(UCHAR), +0x01 Absolute(UCHAR),
     *                +0x02 Size(UCHAR), +0x03 Inserted(UCHAR)
     *                +0x04 SignalState(LONG), +0x08 WaitListHead.Flink,
     *                +0x0C WaitListHead.Blink */
    BRIDGE_MEM8(guest_va + 0)  = (uint8_t)(event_type ? 1 : 0);
    BRIDGE_MEM8(guest_va + 1)  = 0;
    BRIDGE_MEM8(guest_va + 2)  = 16;
    BRIDGE_MEM8(guest_va + 3)  = 1;
    BRIDGE_MEM32(guest_va + 4) = initial_state ? 1 : 0; /* SignalState */
    BRIDGE_MEM32(guest_va + 8) = 0;  /* WaitListHead.Flink (self/empty) */
    BRIDGE_MEM32(guest_va + 12) = 0; /* WaitListHead.Blink (self/empty) */

    g_eax = 0;
}

/* --- KeInitializeMutant (ordinal 110, 2 args = 8 bytes) --- */
static void bridge_KeInitializeMutant(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t initial_owner = STACK_ARG(1);

    HANDLE h = CreateMutexW(NULL, initial_owner ? TRUE : FALSE, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] KeInitializeMutant: CreateMutexW failed (error %u)\n",
                GetLastError());
        g_eax = 0;
        return;
    }

    ke_shadow_insert(guest_va, h);

    /* Mutant dispatcher header. SignalState semantics differ from events:
     * a mutant is SIGNALLED (owned count 0) when its SignalState is 0.
     * Win32 CreateMutex(initialOwner=TRUE) is owned, so SignalState=1. */
    BRIDGE_MEM8(guest_va + 0)  = 19;   /* DispatcherObjectType */
    BRIDGE_MEM8(guest_va + 1)  = 0;
    BRIDGE_MEM8(guest_va + 2)  = 16;
    BRIDGE_MEM8(guest_va + 3)  = 1;
    BRIDGE_MEM32(guest_va + 4) = initial_owner ? 1 : 0; /* SignalState */
    BRIDGE_MEM32(guest_va + 8) = 0;   /* WaitListHead */
    BRIDGE_MEM32(guest_va + 12) = 0;

    g_eax = 0;
}

/* --- KeInitializeSemaphore (ordinal 112, 3 args = 12 bytes) --- */
static void bridge_KeInitializeSemaphore(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t count = STACK_ARG(1);
    uint32_t limit = STACK_ARG(2);

    HANDLE h = CreateSemaphoreW(NULL, (LONG)count, (LONG)limit, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] KeInitializeSemaphore: CreateSemaphoreW failed (error %u)\n",
                GetLastError());
        g_eax = 0;
        return;
    }

    ke_shadow_insert(guest_va, h);

    BRIDGE_MEM8(guest_va + 0)  = 18;   /* DispatcherObjectType */
    BRIDGE_MEM8(guest_va + 1)  = 0;
    BRIDGE_MEM8(guest_va + 2)  = 16;
    BRIDGE_MEM8(guest_va + 3)  = 1;
    BRIDGE_MEM32(guest_va + 4) = (LONG)count; /* SignalState = count */
    BRIDGE_MEM32(guest_va + 8) = 0;   /* WaitListHead */
    BRIDGE_MEM32(guest_va + 12) = 0;

    g_eax = 0;
}

/* --- KePulseEvent (ordinal 123, 3 args = 12 bytes) --- */
static void bridge_KePulseEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);
    HANDLE h;

    (void)increment;
    (void)wait;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        PulseEvent(h);

    g_eax = 0; /* previous state unknown */
}

/* --- KeReleaseMutant (ordinal 131, 4 args = 16 bytes) --- */
static void bridge_KeReleaseMutant(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);
    uint32_t abandoned = STACK_ARG(3);
    HANDLE h;

    (void)increment;
    (void)wait;
    (void)abandoned;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        ReleaseMutex(h);

    g_eax = 0;
}

/* --- KeReleaseSemaphore (ordinal 132, 4 args = 16 bytes) --- */
static void bridge_KeReleaseSemaphore(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t adjustment = STACK_ARG(2);  /* LONG */
    uint32_t wait = STACK_ARG(3);
    HANDLE h;

    (void)increment;
    (void)wait;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        ReleaseSemaphore(h, (LONG)adjustment, NULL);

    g_eax = 0;
}

/* --- KeResetEvent (ordinal 138, 1 arg = 4 bytes) --- */
static void bridge_KeResetEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    HANDLE h;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        ResetEvent(h);

    g_eax = 0;
}

/* --- KeResumeThread (ordinal 140, 1 arg = 4 bytes) --- */
static void bridge_KeResumeThread(void)
{
    HANDLE hThread = bridge_resolve_handle(STACK_ARG(0));
    if (!hThread)
        hThread = XBOX_TO_NATIVE(STACK_ARG(0));

    if (hThread)
        g_eax = (uint32_t)ResumeThread(hThread);
    else
        g_eax = 0;
}

/* --- KeSuspendThread (ordinal 152, 1 arg = 4 bytes) --- */
static void bridge_KeSuspendThread(void)
{
    HANDLE hThread = bridge_resolve_handle(STACK_ARG(0));
    if (!hThread)
        hThread = XBOX_TO_NATIVE(STACK_ARG(0));

    if (hThread)
        g_eax = (uint32_t)SuspendThread(hThread);
    else
        g_eax = 0;
}

/* --- KeAlertResumeThread (ordinal 92, 1 arg = 4 bytes) --- */
static void bridge_KeAlertResumeThread(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- KeBoostPriorityThread (ordinal 94, 2 args = 8 bytes) --- */
static void bridge_KeBoostPriorityThread(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- KeEnterCriticalRegion (ordinal 101, 0 args = 0 bytes) --- */
static void bridge_KeEnterCriticalRegion(void)
{
    g_eax = 0;
}

/* --- KeInitializeApc (ordinal 105, 6 args = 24 bytes) --- */
static void bridge_KeInitializeApc(void)
{
    uint32_t apc_va = STACK_ARG(0);

    if (apc_va) {
        BRIDGE_MEM8(apc_va + 0)  = 0;
        BRIDGE_MEM8(apc_va + 1)  = 0;
        BRIDGE_MEM8(apc_va + 2)  = 0;
        BRIDGE_MEM8(apc_va + 3)  = 0;
        BRIDGE_MEM32(apc_va + 4) = 0;
        BRIDGE_MEM32(apc_va + 8) = 0;
        BRIDGE_MEM32(apc_va + 12) = 0;
        BRIDGE_MEM32(apc_va + 16) = 0;
        BRIDGE_MEM32(apc_va + 20) = 0;
    }
    g_eax = 0;
}

/* --- KeInitializeDeviceQueue (ordinal 106, 1 arg = 4 bytes) --- */
static void bridge_KeInitializeDeviceQueue(void)
{
    uint32_t queue_va = STACK_ARG(0);

    if (queue_va) {
        BRIDGE_MEM8(queue_va + 0)  = 0;
        BRIDGE_MEM8(queue_va + 1)  = 0;
        BRIDGE_MEM8(queue_va + 2)  = 0;
        BRIDGE_MEM8(queue_va + 3)  = 0;
        BRIDGE_MEM32(queue_va + 4) = 0;  /* DeviceLock */
        BRIDGE_MEM32(queue_va + 8) = 0;  /* DeviceListHead.Flink */
        BRIDGE_MEM32(queue_va + 12) = 0; /* DeviceListHead.Blink */
    }
    g_eax = 0;
}

/* --- KeInitializeQueue (ordinal 111, 2 args = 8 bytes) --- */
static void bridge_KeInitializeQueue(void)
{
    uint32_t queue_va = STACK_ARG(0);
    uint32_t count = STACK_ARG(1);

    if (queue_va) {
        BRIDGE_MEM8(queue_va + 0)  = 0;
        BRIDGE_MEM8(queue_va + 1)  = 0;
        BRIDGE_MEM8(queue_va + 2)  = 0;
        BRIDGE_MEM8(queue_va + 3)  = 1;
        BRIDGE_MEM32(queue_va + 4) = 0;  /* Count */
        BRIDGE_MEM32(queue_va + 8) = 0;  /* EntryCount */
        BRIDGE_MEM32(queue_va + 12) = (LONG)count; /* MaximumCount */
        BRIDGE_MEM32(queue_va + 16) = 0; /* ThreadListHead */
        BRIDGE_MEM32(queue_va + 20) = 0; /* ThreadListHead */
        BRIDGE_MEM32(queue_va + 24) = 0; /* Lock */
        BRIDGE_MEM32(queue_va + 28) = 0; /* Lock */
    }
    g_eax = queue_va;
}

/* --- KeInsertByKeyDeviceQueue (ordinal 114, 3 args = 12 bytes) --- */
static void bridge_KeInsertByKeyDeviceQueue(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

/* --- KeInsertDeviceQueue (ordinal 115, 2 args = 8 bytes) --- */
static void bridge_KeInsertDeviceQueue(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- KeInsertHeadQueue (ordinal 116, 2 args = 8 bytes) --- */
static void bridge_KeInsertHeadQueue(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- KeInsertQueue (ordinal 117, 2 args = 8 bytes) --- */
static void bridge_KeInsertQueue(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- KeInsertQueueApc (ordinal 118, 4 args = 16 bytes) --- */
static void bridge_KeInsertQueueApc(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    g_eax = 1;
}

/* --- KeInsertQueueDpc (ordinal 119) ---
 * BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1,
 *                          PVOID SystemArgument2)
 *
 * Enqueues a DPC for the timer thread to drain. See the queue above. */

/* --- KeIsExecutingDpc (ordinal 121, 0 args = 0 bytes) --- */
static void bridge_KeIsExecutingDpc(void)
{
    g_eax = 0;
}

/* --- KeLeaveCriticalRegion (ordinal 122, 0 args = 0 bytes) --- */
static void bridge_KeLeaveCriticalRegion(void)
{
    g_eax = 0;
}

/* --- KeRaiseIrqlToSynchLevel (ordinal 130, 0 args = 0 bytes) --- */
static void bridge_KeRaiseIrqlToSynchLevel(void)
{
    g_eax = 0;  /* PASSIVE_LEVEL; IRQL is not modelled */
}

/* --- KeRemoveByKeyDeviceQueue (ordinal 133, 2 args = 8 bytes) --- */
static void bridge_KeRemoveByKeyDeviceQueue(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- KeRemoveDeviceQueue (ordinal 134, 1 arg = 4 bytes) --- */
static void bridge_KeRemoveDeviceQueue(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- KeRemoveEntryDeviceQueue (ordinal 135, 2 args = 8 bytes) --- */
static void bridge_KeRemoveEntryDeviceQueue(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- KeRemoveQueue (ordinal 136, 1 arg = 4 bytes) --- */
static void bridge_KeRemoveQueue(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- KeRemoveQueueDpc (ordinal 137, 1 arg = 4 bytes) ---
 * BOOLEAN KeRemoveQueueDpc(PKDPC Dpc)
 *
 * Removes a queued DPC. See the queue above. */

/* --- KeRundownQueue (ordinal 141, 1 arg = 4 bytes) --- */
static void bridge_KeRundownQueue(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- KeSetEventBoostPriority (ordinal 146, 2 args = 8 bytes) --- */
static void bridge_KeSetEventBoostPriority(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    HANDLE h;

    (void)increment;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        SetEvent(h);

    g_eax = 0;
}

/* --- KeSetPriorityProcess (ordinal 147, 2 args = 8 bytes) --- */
static void bridge_KeSetPriorityProcess(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- KeSetPriorityThread (ordinal 148, 2 args = 8 bytes) --- */
static void bridge_KeSetPriorityThread(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- KeTestAlertThread (ordinal 155, 1 arg = 4 bytes) --- */
static void bridge_KeTestAlertThread(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- KiBugCheckData (ordinal 162, 0 args = 0 bytes) --- */
static void bridge_KiBugCheckData(void)
{
    g_eax = XBOX_KERNEL_DATA_BASE + 0x500;
}

/* --- KiUnlockDispatcherDatabase (ordinal 163, 1 arg = 4 bytes) --- */
static void bridge_KiUnlockDispatcherDatabase(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- KeGetCurrentIrql (ordinal 103, 0 args = 0 bytes)
 * Stack-based with 0 args (not the Kf* fastcall form). IRQL is unmounted, so
 * report PASSIVE_LEVEL. */
static void bridge_KeGetCurrentIrql(void)
{
    g_eax = 0;  /* PASSIVE_LEVEL */
}

/* --- KeGetCurrentThread (ordinal 104, 0 args = 0 bytes) --- */
static void bridge_KeGetCurrentThread(void)
{
    g_eax = 0;
}

/* --- KeSetDisableBoostThread (ordinal 144, 2 args = 8 bytes) --- */
static void bridge_KeSetDisableBoostThread(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Mm (174, 374, 375, 376, 377, 378)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- MmIsAddressValid (ordinal 174, 1 arg = 4 bytes) --- */
static void bridge_MmIsAddressValid(void)
{
    (void)STACK_ARG(0);
    g_eax = 1;  /* assume every guest address is valid */
}

/* --- MmDbgAllocateMemory (ordinal 374, 2 args = 8 bytes)
 * PVOID MmDbgAllocateMemory(ULONG Size, ULONG AllocationTag);
 * Allocates zeroed guest memory. */
static void bridge_MmDbgAllocateMemory(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t va;

    (void)tag;
    if (!size) {
        g_eax = 0;
        return;
    }
    va = xbox_HeapAlloc(size, 16);
    if (va)
        memset((void *)((uintptr_t)va + g_xbox_mem_offset), 0, size);
    g_eax = va;
}

/* --- MmDbgFreeMemory (ordinal 375, 2 args = 8 bytes)
 * VOID MmDbgFreeMemory(PVOID BaseAddress, ULONG Size); */
static void bridge_MmDbgFreeMemory(void)
{
    (void)STACK_ARG(1);
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* --- MmDbgQueryAvailablePages (ordinal 376, 0 args = 0 bytes) --- */
static void bridge_MmDbgQueryAvailablePages(void)
{
    g_eax = 0x10000;  /* large made-up free-page count */
}

/* --- MmDbgReleaseAddress (ordinal 377, 2 args = 8 bytes) --- */
static void bridge_MmDbgReleaseAddress(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- MmDbgWriteCheck (ordinal 378, 2 args = 8 bytes)
 * Passthrough: returns the first argument (the guest VA). */
static void bridge_MmDbgWriteCheck(void)
{
    (void)STACK_ARG(1);
    g_eax = STACK_ARG(0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Nt (185, 191, 194, 201, 204, 206, 208, 209, 212, 213, 214, 216, 220, 223,
 *      227, 229, 230, 237)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- NtCancelTimer (ordinal 185, 2 args = 8 bytes)
 * NTSTATUS NtCancelTimer(HANDLE TimerHandle);
 * Resolves the handle token (the pair created by NtCreateTimer) and cancels
 * the waitable timer. (No xbox_NtCancelTimer() exists in kernel.h; done
 * inline with the Win32 API.) */
static void bridge_NtCancelTimer(void)
{
    HANDLE h = bridge_resolve_handle(STACK_ARG(0));
    (void)STACK_ARG(1);

    if (h)
        CancelWaitableTimer(h);
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* --- NtCreateIoCompletion (ordinal 191, 4 args = 16 bytes) --- */
static void bridge_NtCreateIoCompletion(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    g_eax = 0;
}

/* --- NtCreateTimer (ordinal 194, 3 args = 12 bytes)
 * NTSTATUS NtCreateTimer(PHANDLE TimerHandle, POBJECT_ATTRIBUTES
 *                        ObjectAttributes, TIMER_TYPE TimerType);
 * Creates a manual-reset waitable timer and hands its token out through the
 * handle table, matching the NtCreateEvent/NtCreateMutant pattern. */
static void bridge_NtCreateTimer(void)
{
    HANDLE h;

    (void)STACK_ARG(1);
    (void)STACK_ARG(2);

    h = CreateWaitableTimerW(NULL, TRUE, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] NtCreateTimer: CreateWaitableTimerW failed (error %u)\n",
                GetLastError());
        g_eax = 0xC000009Au;  /* STATUS_INSUFFICIENT_RESOURCES */
        return;
    }
    if (STACK_ARG(0))
        bridge_write_handle(STACK_ARG(0), h);
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* --- NtOpenDirectoryObject (ordinal 201, 3 args = 12 bytes) --- */
static void bridge_NtOpenDirectoryObject(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

/* --- NtProtectVirtualMemory (ordinal 204, 4 args = 16 bytes)
 * NTSTATUS NtProtectVirtualMemory(PVOID* BaseAddress, PSIZE_T RegionSize,
 *                                 ULONG NewProtect, PULONG OldProtect);
 * Both pointers live in guest memory; translate and let VirtualProtect do the
 * work. (No xbox_NtProtectVirtualMemory() exists in kernel.h; done inline.) */
static void bridge_NtProtectVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t size_ptr = STACK_ARG(1);
    uint32_t new_prot = STACK_ARG(2);
    uint32_t old_ptr  = STACK_ARG(3);
    uint32_t base_va = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;
    uint32_t size    = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;
    DWORD old = 0;

    if (VirtualProtect(XBOX_TO_NATIVE(base_va), size, (DWORD)new_prot, &old)) {
        if (old_ptr)
            BRIDGE_MEM32(old_ptr) = (uint32_t)old;
        g_eax = 0;  /* STATUS_SUCCESS */
    } else {
        g_eax = 0xC0000005u;  /* STATUS_ACCESS_VIOLATION */
    }
}

/* --- NtQueueApcThread (ordinal 206, 5 args = 20 bytes) --- */
static void bridge_NtQueueApcThread(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

/* --- NtQueryDirectoryObject (ordinal 208, 7 args = 28 bytes) --- */
static void bridge_NtQueryDirectoryObject(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    (void)STACK_ARG(6);
    g_eax = 0;
}

/* --- NtQueryEvent (ordinal 209, 4 args = 16 bytes) --- */
static void bridge_NtQueryEvent(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    g_eax = 0;
}

/* --- NtQueryIoCompletion (ordinal 212, 5 args = 20 bytes) --- */
static void bridge_NtQueryIoCompletion(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

/* --- NtQueryMutant (ordinal 213, 5 args = 20 bytes) --- */
static void bridge_NtQueryMutant(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

/* --- NtQuerySemaphore (ordinal 214, 5 args = 20 bytes) --- */
static void bridge_NtQuerySemaphore(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

/* --- NtQueryTimer (ordinal 216, 5 args = 20 bytes) --- */
static void bridge_NtQueryTimer(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

/* --- NtReadFileScatter (ordinal 220, 8 args = 32 bytes) --- */
static void bridge_NtReadFileScatter(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    (void)STACK_ARG(6);
    (void)STACK_ARG(7);
    g_eax = 0;
}

/* --- NtRemoveIoCompletion (ordinal 223, 5 args = 20 bytes) --- */
static void bridge_NtRemoveIoCompletion(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

/* --- NtSetIoCompletion (ordinal 227, 5 args = 20 bytes) --- */
static void bridge_NtSetIoCompletion(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

/* --- NtSetTimerEx (ordinal 229, 8 args = 32 bytes) --- */
static void bridge_NtSetTimerEx(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    (void)STACK_ARG(6);
    (void)STACK_ARG(7);
    g_eax = 0;
}

/* --- NtSignalAndWaitForSingleObjectEx (ordinal 230, 5 args = 20 bytes) --- */
static void bridge_NtSignalAndWaitForSingleObjectEx(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

/* --- NtWriteFileGather (ordinal 237, 8 args = 32 bytes) --- */
static void bridge_NtWriteFileGather(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    (void)STACK_ARG(6);
    (void)STACK_ARG(7);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Ob (239, 241, 242, 243, 244, 248)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- ObCreateObject (ordinal 239, 7 args = 28 bytes) --- */
static void bridge_ObCreateObject(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    (void)STACK_ARG(6);
    g_eax = 0;
}

/* --- ObInsertObject (ordinal 241, 6 args = 24 bytes) --- */
static void bridge_ObInsertObject(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    g_eax = 0;
}

/* --- ObMakeTemporaryObject (ordinal 242, 1 arg = 4 bytes) --- */
static void bridge_ObMakeTemporaryObject(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- ObOpenObjectByName (ordinal 243, 4 args = 16 bytes) --- */
static void bridge_ObOpenObjectByName(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    g_eax = 0;
}

/* --- ObOpenObjectByPointer (ordinal 244, 5 args = 20 bytes) --- */
static void bridge_ObOpenObjectByPointer(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

/* --- ObReferenceObjectByPointer (ordinal 248, 1 arg = 4 bytes) --- */
static void bridge_ObReferenceObjectByPointer(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Ps (254, 256, 257)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- PsCreateSystemThread (ordinal 254, 7 args = 28 bytes)
 * NTSTATUS PsCreateSystemThread(PHANDLE ThreadHandle, ULONG KernelStackSize,
 *                               ULONG TlsDataSize, PULONG ThreadId,
 *                               PVOID StartContext1, PVOID StartContext2,
 *                               PXBOX_SYSTEM_ROUTINE StartRoutine);
 * Mirror of PsCreateSystemThreadEx (ordinal 255) with the ThreadExtraSize
 * argument dropped: same first-call-inline / worker-spawn behaviour.
 * Requires bridge_spawn_thread/bridge_run_thread_inline/g_thread_mode from
 * Section A of kernel_bridge.c. */
static void bridge_PsCreateSystemThread(void)
{
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t start_context1  = STACK_ARG(4);
    uint32_t start_context2  = STACK_ARG(5);
    uint32_t start_routine   = STACK_ARG(6);
    int is_first_call = (g_thread_mode == XBOX_THREAD_MODE_INLINE)
                        && (g_thread_call_count == 0);

    g_thread_call_count++;

    /* Fake handle until the worker path can write a real one. */
    if (xbox_handle_ptr)
        BRIDGE_MEM32(xbox_handle_ptr) = 0xBEEF0002;

    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn)
            fn = recomp_lookup_manual(start_routine);
        if (fn) {
            if (is_first_call) {
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                fn();
                g_esp += 12;
            } else {
                const char *inline_workers = getenv("RECOMP_WORKERS");
                uint32_t stack_top;

                if (inline_workers && !strcmp(inline_workers, "inline")) {
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                    g_eax = 0;
                    return;
                }
                stack_top = xbox_AllocThreadStack();
                if (!stack_top) {
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                } else {
                    HANDLE th = bridge_spawn_thread(fn, start_context1,
                                                    start_context2, stack_top);
                    if (xbox_handle_ptr && th)
                        bridge_write_handle(xbox_handle_ptr, th);
                }
            }
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThread: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
        }
    }
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* --- PsQueryStatistics (ordinal 256, 1 arg = 4 bytes)
 * Zeroes the caller's statistics structure. */
static void bridge_PsQueryStatistics(void)
{
    uint32_t stats_va = STACK_ARG(0);
    if (stats_va)
        memset(XBOX_TO_NATIVE(stats_va), 0, 0x80);
    g_eax = 0;
}

/* --- PsSetCreateThreadNotifyRoutine (ordinal 257, 2 args = 8 bytes) --- */
static void bridge_PsSetCreateThreadNotifyRoutine(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Rtl (281, 282, 283, 285, 286, 303, 361, 362, 363, 364)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- RtlExtendedIntegerMultiply (ordinal 281, 3 args = 12 bytes)
 * LARGE_INTEGER RtlExtendedIntegerMultiply(LARGE_INTEGER Multiplicand,
 *                                          LONG Multiplier);
 * Multiplicand.Low (4) + Multiplicand.High (4) + Multiplier (4). C's int64
 * multiply wraps mod 2^64, which is exactly what the x86 imul pair produces
 * for the low 64 bits the API returns. */
static void bridge_RtlExtendedIntegerMultiply(void)
{
    int32_t mult = (int32_t)STACK_ARG(2);
    int64_t multi = ((int64_t)(int32_t)STACK_ARG(1) << 32)
                  | (uint32_t)STACK_ARG(0);
    int64_t result = multi * (int64_t)mult;

    g_eax = (uint32_t)result;
    g_edx = (uint32_t)((uint64_t)result >> 32);
}

/* --- RtlExtendedLargeIntegerDivide (ordinal 282, 4 args = 16 bytes)
 * LARGE_INTEGER RtlExtendedLargeIntegerDivide(LARGE_INTEGER Dividend,
 *                                             ULONG Divisor,
 *                                             PULONG Remainder);
 * Dividend.Low (4) + Dividend.High (4) + Divisor (4) + Remainder-ptr (4). */
static void bridge_RtlExtendedLargeIntegerDivide(void)
{
    uint64_t dividend = ((uint64_t)(uint32_t)STACK_ARG(1) << 32)
                      | (uint32_t)STACK_ARG(0);
    uint32_t divisor = STACK_ARG(2);
    uint32_t rem_ptr = STACK_ARG(3);
    uint32_t remainder;
    uint64_t quotient;

    if (divisor == 0) {
        /* Division by zero: report 0/*; Real hardware faults here. */
        if (rem_ptr)
            BRIDGE_MEM32(rem_ptr) = 0;
        g_eax = 0;
        g_edx = 0;
        return;
    }

    quotient  = dividend / divisor;
    remainder = (uint32_t)(dividend % divisor);

    if (rem_ptr)
        BRIDGE_MEM32(rem_ptr) = remainder;
    g_eax = (uint32_t)quotient;
    g_edx = (uint32_t)(quotient >> 32);
}

/* --- RtlExtendedMagicDivide (ordinal 283, 4 args = 16 bytes)
 * RtlExtendedMagicDivide(Dividend.Low, Dividend.High, Divisor, Remainder-ptr)
 * per the 16-byte stack layout. The precomputed magic multiplier is not
 * available, so a real division produces the identical quotient. */
static void bridge_RtlExtendedMagicDivide(void)
{
    uint64_t dividend = ((uint64_t)(uint32_t)STACK_ARG(1) << 32)
                      | (uint32_t)STACK_ARG(0);
    uint32_t divisor = STACK_ARG(2);
    uint32_t rem_ptr = STACK_ARG(3);
    uint32_t remainder;
    uint64_t quotient;

    if (divisor == 0) {
        if (rem_ptr)
            BRIDGE_MEM32(rem_ptr) = 0;
        g_eax = 0;
        g_edx = 0;
        return;
    }

    quotient  = dividend / divisor;
    remainder = (uint32_t)(dividend % divisor);

    if (rem_ptr)
        BRIDGE_MEM32(rem_ptr) = remainder;
    g_eax = (uint32_t)quotient;
    g_edx = (uint32_t)(quotient >> 32);
}

/* --- RtlFillMemoryUlong (ordinal 285, 3 args = 12 bytes)
 * VOID RtlFillMemoryUlong(PVOID Destination, ULONG Length, ULONG Pattern);
 * Length is multiples of 4 by contract. */
static void bridge_RtlFillMemoryUlong(void)
{
    volatile uint32_t *dst = (volatile uint32_t *)XBOX_TO_NATIVE(STACK_ARG(0));
    uint32_t length = STACK_ARG(1);
    uint32_t pattern = STACK_ARG(2);
    uint32_t i;

    if (!dst)
        return;
    for (i = 0; i < length / 4; i++)
        dst[i] = pattern;
    g_eax = 0;
}

/* --- RtlFreeAnsiString (ordinal 286, 1 arg = 4 bytes)
 * VOID RtlFreeAnsiString(PANSI_STRING AnsiString);
 * Frees AnsiString->Buffer (a guest-heap allocation) and clears the struct.
 * ANSI_STRING layout: +0 Length(USHORT), +2 MaximumLength(USHORT), +4 Buffer. */
static void bridge_RtlFreeAnsiString(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t buf;

    if (!str_va) {
        g_eax = 0;
        return;
    }
    buf = BRIDGE_MEM32(str_va + 4);
    if (buf)
        xbox_HeapFree(buf);
    BRIDGE_MEM16(str_va + 0) = 0;
    BRIDGE_MEM16(str_va + 2) = 0;
    BRIDGE_MEM32(str_va + 4) = 0;
    g_eax = 0;
}

/* --- RtlRaiseStatus (ordinal 303, 1 arg = 4 bytes) --- */
static void bridge_RtlRaiseStatus(void)
{
    uint32_t status = STACK_ARG(0);
    fprintf(stderr, "  [KERNEL] RtlRaiseStatus: 0x%08X (swallowed)\n", status);
    g_eax = 0;
}

/* --- Rtl* printf family (ordinals 361-364) ---
 *
 * int RtlSnprintf(char*, size_t count, const char* fmt, ...)
 * int RtlSprintf(char*, const char* fmt, ...)
 * int RtlVsnprintf(char*, size_t count, const char* fmt, va_list)
 * int RtlVsprintf(char*, const char* fmt, va_list)
 *
 * All four are __cdecl on the console, so the stdcall table pops nothing
 * ("caller cleans", like DbgPrint) and the varargs stay readable on the guest
 * stack while the bridge runs. The V* variants take an x86 va_list, which is
 * just a guest pointer to where the argument slots begin.
 *
 * The x64 CRT's vsnprintf cannot consume an x86 va_list, so these bridges used
 * to copy the format string verbatim ("%d" came out as the literal text) and
 * read the wrong fixed arguments for the two non-counted variants: RtlSprintf's
 * "count" was the format pointer, and the vararg list started one slot early.
 * This walks the format itself and formats each conversion with the host CRT,
 * reading 32-bit argument slots from guest memory (64-bit args 8-aligned, as
 * on x86).
 */

/* Guest cursor over the variadic argument slots. */
static uint32_t bridge_va_next32(uint32_t *pos)
{
    uint32_t v = BRIDGE_MEM32(*pos);
    *pos += 4;
    return v;
}

static uint64_t bridge_va_next64(uint32_t *pos)
{
    uint32_t p = (*pos + 7) & ~7u;
    uint32_t lo = BRIDGE_MEM32(p);
    uint32_t hi = BRIDGE_MEM32(p + 4);
    *pos = p + 8;
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

/* Format fmt, pulling varargs from guest memory at `pos`, into out (at most
 * cap-1 chars, NUL-terminated). Returns the number of characters written. */
static int bridge_format_printf(char *out, size_t cap, const char *fmt,
                                uint32_t pos)
{
    size_t used = 0;
    size_t i = 0;

    if (!out || cap == 0)
        return 0;

    while (fmt[i] && used + 1 < cap) {
        if (fmt[i] != '%') {
            out[used++] = fmt[i++];
            continue;
        }
        i++;
        if (fmt[i] == '%') {
            out[used++] = '%';
            i++;
            continue;
        }

        {
            char flags[8];
            int  nflags = 0;
            char conv[32];
            char *cp = conv;
            int  width = 0, have_width = 0, width_star = 0;
            int  prec  = 0, have_prec  = 0, prec_star  = 0;
            char len = 0;          /* 0 none, 1 h, 2 hh, 3 l, 4 ll */
            char spec;
            char tmp[512];
            size_t tlen;

            while (fmt[i] && strchr("-+ 0#", fmt[i]) && nflags < 7)
                flags[nflags++] = fmt[i++];
            flags[nflags] = 0;

            if (fmt[i] == '*') { width_star = 1; have_width = 1; i++; }
            else if (fmt[i] >= '0' && fmt[i] <= '9') {
                have_width = 1;
                while (fmt[i] >= '0' && fmt[i] <= '9') {
                    width = width * 10 + (fmt[i] - '0');
                    i++;
                }
            }

            if (fmt[i] == '.') {
                i++;
                have_prec = 1;
                if (fmt[i] == '*') { prec_star = 1; i++; }
                else while (fmt[i] >= '0' && fmt[i] <= '9') {
                    prec = prec * 10 + (fmt[i] - '0');
                    i++;
                }
            }

            if (fmt[i] == 'h') { len = 1; i++; if (fmt[i] == 'h') { len = 2; i++; } }
            else if (fmt[i] == 'l') { len = 3; i++; if (fmt[i] == 'l') { len = 4; i++; } }
            else if (fmt[i] == 'q') { len = 4; i++; }
            else if (fmt[i] == 'I' && fmt[i+1] == '6' && fmt[i+2] == '4') {
                len = 4; i += 3;
            }

            spec = fmt[i];
            if (!spec)
                break;
            i++;

            *cp++ = '%';
            if (nflags) { memcpy(cp, flags, (size_t)nflags); cp += nflags; }

            if (width_star) {
                int w = (int32_t)bridge_va_next32(&pos);
                if (w < 0) { *cp++ = '-'; w = -w; }
                cp += sprintf(cp, "%d", w);
            } else if (have_width) {
                cp += sprintf(cp, "%d", width);
            }
            if (have_prec) {
                *cp++ = '.';
                if (prec_star) {
                    int p = (int32_t)bridge_va_next32(&pos);
                    if (p < 0) p = 0;
                    cp += sprintf(cp, "%d", p);
                } else {
                    cp += sprintf(cp, "%d", prec);
                }
            }
            /* Single 'l' is a 32-bit long on the console; the host long is 32
             * bits too, so 'l' is passed through for the d/i/u/o/x/X paths. */
            if (len == 2) { *cp++ = 'h'; *cp++ = 'h'; }
            else if (len == 1) { *cp++ = 'h'; }
            else if (len == 4) { *cp++ = 'l'; *cp++ = 'l'; }

            switch (spec) {
            case 'd': case 'i': {
                if (len == 4) {
                    long long v = (long long)bridge_va_next64(&pos);
                    *cp++ = 'd'; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                } else {
                    int v = (int)(int32_t)bridge_va_next32(&pos);
                    *cp++ = 'd'; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                }
                break;
            }
            case 'u': {
                if (len == 4) {
                    unsigned long long v = bridge_va_next64(&pos);
                    *cp++ = 'u'; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                } else {
                    unsigned int v = bridge_va_next32(&pos);
                    *cp++ = 'u'; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                }
                break;
            }
            case 'o': case 'x': case 'X': {
                if (len == 4) {
                    unsigned long long v = bridge_va_next64(&pos);
                    *cp++ = spec; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                } else {
                    unsigned int v = bridge_va_next32(&pos);
                    *cp++ = spec; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                }
                break;
            }
            case 'c': {
                int v = (int)bridge_va_next32(&pos);
                *cp++ = 'c'; *cp = 0;
                snprintf(tmp, sizeof tmp, conv, v);
                break;
            }
            case 's': {
                uint32_t sva = bridge_va_next32(&pos);
                const char *gs = sva ? (const char *)XBOX_TO_NATIVE(sva)
                                     : "(null)";
                *cp++ = 's'; *cp = 0;
                snprintf(tmp, sizeof tmp, conv, gs);
                break;
            }
            case 'p': {
                /* 32-bit pointer as padded lower-case hex, like the x86 CRT */
                unsigned int v = bridge_va_next32(&pos);
                const char *pfx = strchr(flags, '#') ? "%#08x" : "%08x";
                snprintf(tmp, sizeof tmp, pfx, v);
                break;
            }
            case 'n': {
                uint32_t tva = bridge_va_next32(&pos);
                if (tva)
                    BRIDGE_MEM32(tva) = (uint32_t)used;   /* int written */
                tlen = 0;
                break;
            }
            case 'f': case 'e': case 'g': case 'E': case 'G': {
                double v = (double)bridge_va_next64(&pos);
                *cp++ = spec; *cp = 0;
                snprintf(tmp, sizeof tmp, conv, v);
                break;
            }
            default:
                /* Unsupported conversion: keep '%' and the specifier */
                out[used++] = '%';
                out[used++] = spec;
                continue;
            }

            tlen = (spec == 'n') ? 0 : strlen(tmp);
            {
                size_t k;
                for (k = 0; k < tlen && used + 1 < cap; k++)
                    out[used++] = tmp[k];
            }
        }
    }

    if (used + 1 >= cap)
        used = cap - 1;
    out[used] = 0;
    return (int)used;
}

/* --- RtlSnprintf (ordinal 361, __cdecl, caller cleans) --- */
static void bridge_RtlSnprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    uint32_t  count = STACK_ARG(1);
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(2));

    if (!dst || !fmt) {
        g_eax = 0;
        return;
    }
    /* Varargs start after buffer/count/format on the guest stack. */
    g_eax = bridge_format_printf(dst, count, fmt, g_esp + 12);
}

/* --- RtlSprintf (ordinal 362, __cdecl, caller cleans) --- */
static void bridge_RtlSprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(1));

    if (!dst || !fmt) {
        g_eax = 0;
        return;
    }
    /* Unbounded like the CRT's vsprintf; a wide cap prevents a wild write. */
    g_eax = bridge_format_printf(dst, 65536, fmt, g_esp + 8);
}

/* --- RtlVsnprintf (ordinal 363, __cdecl, caller cleans) --- */
static void bridge_RtlVsnprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    uint32_t  count = STACK_ARG(1);
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(2));
    uint32_t  list = STACK_ARG(3);   /* x86 va_list = guest pointer to args */

    if (!dst || !fmt || !list) {
        g_eax = 0;
        return;
    }
    g_eax = bridge_format_printf(dst, count, fmt, list);
}

/* --- RtlVsprintf (ordinal 364, __cdecl, caller cleans) --- */
static void bridge_RtlVsprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t  list = STACK_ARG(2);   /* x86 va_list = guest pointer to args */

    if (!dst || !fmt || !list) {
        g_eax = 0;
        return;
    }
    g_eax = bridge_format_printf(dst, 65536, fmt, list);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Port I/O (329, 330, 331, 332)
 *
 * The port argument is an I/O window the title mapped with MmMapIoSpace, so
 * it is an ordinary guest VA in the memory model. The READ variants copy the
 * requested element count from the mapped window into the caller's buffer;
 * the WRITE variants are no-ops (mirroring the existing 333/334 bridges).
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- READ_PORT_BUFFER_UCHAR (ordinal 329, 3 args = 12 bytes) --- */
static void bridge_READ_PORT_BUFFER_UCHAR(void)
{
    void *src = XBOX_TO_NATIVE(STACK_ARG(0));
    void *dst = XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t count = STACK_ARG(2);

    if (src && dst)
        memcpy(dst, src, count);
    g_eax = 0;
}

/* --- READ_PORT_BUFFER_USHORT (ordinal 330, 3 args = 12 bytes) --- */
static void bridge_READ_PORT_BUFFER_USHORT(void)
{
    void *src = XBOX_TO_NATIVE(STACK_ARG(0));
    void *dst = XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t count = STACK_ARG(2);

    if (src && dst)
        memcpy(dst, src, count * sizeof(uint16_t));
    g_eax = 0;
}

/* --- READ_PORT_BUFFER_ULONG (ordinal 331, 3 args = 12 bytes) --- */
static void bridge_READ_PORT_BUFFER_ULONG(void)
{
    void *src = XBOX_TO_NATIVE(STACK_ARG(0));
    void *dst = XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t count = STACK_ARG(2);

    if (src && dst)
        memcpy(dst, src, count * sizeof(uint32_t));
    g_eax = 0;
}

/* --- WRITE_PORT_BUFFER_UCHAR (ordinal 332, 3 args = 12 bytes) --- */
static void bridge_WRITE_PORT_BUFFER_UCHAR(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

static int stdcall_args_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* ── Display / AV ── */
    case   1: return  0;  /* AvGetSavedDataAddress (void) */
    case   2: return 16;  /* AvSendTVEncoderOption (4) */
    case   3: return 24;  /* AvSetDisplayMode (6) */
    case   4: return  4;  /* AvSetSavedDataAddress (1) */
    case   5: return  0;  /* DbgBreakPoint (void) */
    case   8: return  0;  /* DbgPrint - __cdecl varargs, caller cleans */
    case   9: return  8;  /* HalReadSMCTrayState (2) */
    case  14: return  4;  /* ExAllocatePool (1) */
    case  15: return  8;  /* ExAllocatePoolWithTag (2) */
    case  17: return  4;  /* ExFreePool (1) */
    case  23: return  4;  /* ExQueryPoolBlockSize (1) */
    case  24: return 20;  /* ExQueryNonVolatileSetting (5) */
    case  29: return 16;  /* ExSaveNonVolatileSetting (4) */
    case  35: return  0;  /* FscGetCacheSize (void) */
    case  37: return  4;  /* FscSetCacheSize (1) */
    case  38: return  4;  /* HalClearSoftwareInterrupt (1) */
    case  39: return  8;  /* HalDisableSystemInterrupt (2) */
    case  42: return  0;  /* HalDiskSerialNumber - data export */
    case  44: return  8;  /* HalGetInterruptVector (2) */
    case  47: return  8;  /* HalRegisterShutdownNotification (2) */
    case  46: return 24;  /* HalReadWritePCISpace (6) */
    case  45: return 16;  /* HalReadSMBusValue (4) */
    case  50: return 16;  /* HalWriteSMBusValue (4) */
    case  48: return  4;  /* HalRequestSoftwareInterrupt (1) */
    case  49: return  4;  /* HalReturnToFirmware (1) */
    case  61: return 36;  /* IoBuildDeviceIoControlRequest (9) */
    case  62: return 28;  /* IoBuildSynchronousFsdRequest (7) */
    case  65: return 24;  /* IoCreateDevice (6) */
    case  66: return 40;  /* IoCreateFile (10) */
    case  67: return  8;  /* IoCreateSymbolicLink (2) */
    case  68: return  4;  /* IoDeleteDevice (1) */
    case  69: return  4;  /* IoDeleteSymbolicLink (1) */
    case  73: return 12;  /* IoInitializeIrp (3) */
    case  74: return  8;  /* IoInvalidDeviceRequest (2) */
    case  79: return 20;  /* IoSetIoCompletion (5) */
    case  81: return  8;  /* IoStartNextPacket (2) */
    case  82: return 12;  /* IoStartNextPacketByKey (3) */
    case  83: return 16;  /* IoStartPacket (4) */
    case  84: return 32;  /* IoSynchronousDeviceIoControlRequest (8) */
    case  85: return 20;  /* IoSynchronousFsdRequest (5) */
    case  86: return  0;  /* IofCallDriver (fastcall: args in ecx/edx) */
    case  87: return  0;  /* IofCompleteRequest (fastcall: args in ecx/edx) */
    /* Missing this entry cost the Xbox Dashboard its whole boot. Ordinal 91 has
     * no bridge, so the generic stub ran -- and with no arg count it left the
     * one pushed argument on the guest stack. The caller (sub_00032859) then
     * ran `pop edi; pop esi; pop ebx` four bytes low and came back with its
     * registers rotated, which destroyed the XApp `this` pointer two frames up.
     * Its scene manager was never created, its scene never loaded, and it
     * returned to firmware -- reported as nothing more than "returning 0". */
    case  90: return  4;  /* IoDismountVolume (1) */
    case  91: return  4;  /* IoDismountVolumeByName (1) */
    case  93: return  8;  /* KeAlertThread (2) */
    case  95: return  4;  /* KeBugCheck (1) */
    case  96: return 20;  /* KeBugCheckEx (5) */
    case  97: return  4;  /* KeCancelTimer (1) */
    case  98: return  4;  /* KeConnectInterrupt (1) */
    case  99: return 12;  /* KeDelayExecutionThread (3) */
    case 100: return  4;  /* KeDisconnectInterrupt (1) */
    case 107: return 12;  /* KeInitializeDpc (3) */
    case 109: return 28;  /* KeInitializeInterrupt (7) */
    case 113: return  8;  /* KeInitializeTimerEx (2) */
    case 119: return 12;  /* KeInsertQueueDpc (3) */
    case 124: return  4;  /* KeQueryBasePriorityThread (1) */
    case 125: return  0;  /* KeQueryInterruptTime (void) */
    case 126: return  0;  /* KeQueryPerformanceCounter (void) */
    case 127: return  0;  /* KeQueryPerformanceFrequency (void) */
    case 128: return  4;  /* KeQuerySystemTime (1) */
    case 129: return  0;  /* KeRaiseIrqlToDpcLevel (void) */
    case 137: return  4;  /* KeRemoveQueueDpc (1) */
    case 139: return  4;  /* KeRestoreFloatingPointState (1) */
    case 142: return  4;  /* KeSaveFloatingPointState (1) */
    case 143: return  8;  /* KeSetBasePriorityThread (2) */
    case 144: return  8;  /* KeSetDisableBoostThread (2) */
    case 145: return 12;  /* KeSetEvent (3) */
    case 149: return 16;  /* KeSetTimer (Timer+DueTime[8]+Dpc) */
    case 150: return 20;  /* KeSetTimerEx (Timer+DueTime[8]+Period+Dpc) */
    case 151: return  4;  /* KeStallExecutionProcessor (1) */
    case 153: return 12;  /* KeSynchronizeExecution (3) */
    case 158: return 32;  /* KeWaitForMultipleObjects (8) */
    case 159: return 20;  /* KeWaitForSingleObject (5) */
    case 160: return  0;  /* KfRaiseIrql (fastcall: arg in ecx) */
    case 161: return  0;  /* KfLowerIrql (fastcall: arg in ecx) */
    case 165: return  4;  /* MmAllocateContiguousMemory (1) */
    case 166: return 20;  /* MmAllocateContiguousMemoryEx (5) */
    case 167: return  8;  /* MmAllocateSystemMemory (2) */
    case 168: return  8;  /* MmClaimGpuInstanceMemory (2) */
    case 169: return  8;  /* MmCreateKernelStack (2) */
    case 170: return  8;  /* MmDeleteKernelStack (2) */
    case 171: return  4;  /* MmFreeContiguousMemory (1) */
    case 172: return  8;  /* MmFreeSystemMemory (2) */
    case 173: return  4;  /* MmGetPhysicalAddress (1) */
    case 175: return 12;  /* MmLockUnlockBufferPages (3) */
    case 176: return  8;  /* MmLockUnlockPhysicalPage (2) */
    case 177: return 12;  /* MmMapIoSpace (3) */
    case 178: return 12;  /* MmPersistContiguousMemory (3) */
    case 179: return  4;  /* MmQueryAddressProtect (1) */
    case 180: return  4;  /* MmQueryAllocationSize (1) */
    case 181: return  4;  /* MmQueryStatistics (1) */
    case 182: return 12;  /* MmSetAddressProtect (3) */
    case 183: return  8;  /* MmUnmapIoSpace (2) */
    case 184: return 20;  /* NtAllocateVirtualMemory (5) */
    case 185: return  8;  /* NtCancelTimer (2) */
    case 186: return  4;  /* NtClearEvent (1) */
    case 187: return  4;  /* NtClose (1) */
    case 188: return  8;  /* NtCreateDirectoryObject (2) */
    case 189: return 16;  /* NtCreateEvent (4) */
    case 190: return 36;  /* NtCreateFile (9) */
    case 191: return 16;  /* NtCreateIoCompletion (4) */
    case 192: return 12;  /* NtCreateMutant (3) */
    case 193: return 16;  /* NtCreateSemaphore (4) */
    case 194: return 12;  /* NtCreateTimer (3) */
    case 195: return  4;  /* NtDeleteFile (1) */
    case 196: return 40;  /* NtDeviceIoControlFile (10) */
    case 197: return 12;  /* NtDuplicateObject (3) */
    case 198: return  8;  /* NtFlushBuffersFile (2) */
    case 199: return 12;  /* NtFreeVirtualMemory (3) */
    case 200: return 40;  /* NtFsControlFile (10) */
    case 202: return 24;  /* NtOpenFile (6) */
    case 203: return  8;  /* NtOpenSymbolicLinkObject (2) */
    case 204: return 16;  /* NtProtectVirtualMemory (4) */
    case 205: return  8;  /* NtPulseEvent (2) */
    case 206: return 20;  /* NtQueueApcThread (5) */
    case 207: return 36;  /* NtQueryDirectoryFile (9) */
    case 210: return  8;  /* NtQueryFullAttributesFile (2) */
    case 211: return 20;  /* NtQueryInformationFile (5) */
    case 215: return 12;  /* NtQuerySymbolicLinkObject (3) */
    case 217: return 8;   /* NtQueryVirtualMemory (2) -- Xbox takes
                             BaseAddress and Info only, not NT's four */
    case 218: return 20;  /* NtQueryVolumeInformationFile (5) */
    case 219: return 32;  /* NtReadFile (8) */
    case 220: return 32;  /* NtReadFileScatter (8) */
    case 221: return  8;  /* NtReleaseMutant (2) */
    case 222: return 12;  /* NtReleaseSemaphore (3) */
    case 223: return 20;  /* NtRemoveIoCompletion (5) */
    case 224: return  8;  /* NtResumeThread (2) */
    case 225: return  8;  /* NtSetEvent (2) */
    case 226: return 20;  /* NtSetInformationFile (5) */
    case 227: return 20;  /* NtSetIoCompletion (5) */
    case 228: return  8;  /* NtSetSystemTime (2) */
    case 229: return 32;  /* NtSetTimerEx (8) */
    case 230: return 20;  /* NtSignalAndWaitForSingleObjectEx (5) */
    case 231: return  8;  /* NtSuspendThread (2) */
    case 232: return 12;  /* NtUserIoApcDispatcher (3) */
    case 233: return 12;  /* NtWaitForSingleObject (3) */
    case 234: return 16;  /* NtWaitForSingleObjectEx (4) */
    case 235: return 24;  /* NtWaitForMultipleObjectsEx (6) */
    case 236: return 32;  /* NtWriteFile (8) */
    case 237: return 32;  /* NtWriteFileGather (8) */
    case 238: return  0;  /* NtYieldExecution (void) */
    case 243: return 16;  /* ObOpenObjectByName (4) */
    case 247: return 20;  /* ObReferenceObjectByName (5) */
    case 250: return  0;  /* ObfDereferenceObject (fastcall: arg in ecx) */
    case 251: return  0;  /* ObfReferenceObject (fastcall: arg in ecx) */
    case 252: return  4;  /* PhyGetLinkState (1) */
    case 253: return  8;  /* PhyInitialize (2) */
    case 255: return 40;  /* PsCreateSystemThreadEx (10) */
    case 258: return  4;  /* PsTerminateSystemThread (1) */
    case 260: return 12;  /* RtlAnsiStringToUnicodeString (3) */
    case 268: return 12;  /* RtlCompareMemory (3) */
    case 269: return 12;  /* RtlCompareMemoryUlong (3) */
    case 270: return 12;  /* RtlCompareString (3) */
    case 261: return  8;  /* RtlAppendStringToString (2) */
    case 262: return  8;  /* RtlAppendUnicodeStringToString (2) */
    case 263: return  8;  /* RtlAppendUnicodeToString (2) */
    case 264: return 12;  /* RtlAssert (3) */
    case 265: return  4;  /* RtlCaptureContext (1) */
    case 266: return 20;  /* RtlCaptureStackBackTrace (5) */
    case 267: return 12;  /* RtlCharToInteger (3) */
    case 271: return 12;  /* RtlCompareUnicodeString (3) */
    case 272: return  8;  /* RtlCopyString (2) */
    case 273: return  8;  /* RtlCopyUnicodeString (2) */
    case 274: return  8;  /* RtlCreateUnicodeString (2) */
    case 275: return  4;  /* RtlDowncaseUnicodeChar (1) */
    case 276: return 12;  /* RtlDowncaseUnicodeString (3) */
    case 278: return  4;  /* RtlEnterCriticalSectionAndRegion (1) */
    case 280: return 12;  /* RtlEqualUnicodeString (3) */
    case 284: return 12;  /* RtlFillMemory (3) */
    case 287: return  4;  /* RtlFreeUnicodeString (1) */
    case 288: return  8;  /* RtlGetCallersAddress (2) */
    case 292: return 16;  /* RtlIntegerToChar (4) */
    case 293: return 12;  /* RtlIntegerToUnicodeString (3) */
    case 295: return  4;  /* RtlLeaveCriticalSectionAndRegion (1) */
    case 296: return  4;  /* RtlLowerChar (1) */
    case 297: return  8;  /* RtlMapGenericMask (2) */
    case 298: return 12;  /* RtlMoveMemory (3) */
    case 299: return 20;  /* RtlMultiByteToUnicodeN (5) */
    case 300: return 12;  /* RtlMultiByteToUnicodeSize (3) */
    case 306: return  4;  /* RtlTryEnterCriticalSection (1) */
    case 307: return  4;  /* RtlUlongByteSwap (1) */
    case 309: return 12;  /* RtlUnicodeStringToInteger (3) */
    case 310: return 20;  /* RtlUnicodeToMultiByteN (5) */
    case 311: return 12;  /* RtlUnicodeToMultiByteSize (3) */
    case 313: return  4;  /* RtlUpcaseUnicodeChar (1) */
    case 314: return 12;  /* RtlUpcaseUnicodeString (3) */
    case 315: return 20;  /* RtlUpcaseUnicodeToMultiByteN (5) */
    case 316: return  4;  /* RtlUpperChar (1) */
    case 317: return  8;  /* RtlUpperString (2) */
    case 318: return  4;  /* RtlUshortByteSwap (1) */
    case 319: return 12;  /* RtlWalkFrameChain (3) */
    case 320: return  8;  /* RtlZeroMemory (2) */
    case 321: return  0;  /* XboxEEPROMKey (void) */
    case 277: return  4;  /* RtlEnterCriticalSection (1) */
    case 279: return 12;  /* RtlEqualString (3) */
    case 285: return 12;  /* RtlFillMemoryUlong (3) */
    case 286: return  4;  /* RtlFreeAnsiString (1) */
    case 289: return  8;  /* RtlInitAnsiString (2) */
    case 290: return  8;  /* RtlInitUnicodeString (2) */
    case 291: return  4;  /* RtlInitializeCriticalSection (1) */
    case 294: return  4;  /* RtlLeaveCriticalSection (1) */
    case 301: return  4;  /* RtlNtStatusToDosError (1) */
    case 302: return  4;  /* RtlRaiseException (1) */
    case 304: return  8;  /* RtlTimeFieldsToTime (2) */
    case 305: return  8;  /* RtlTimeToTimeFields (2) */
    case 308: return 12;  /* RtlUnicodeStringToAnsiString (3) */
    case 312: return 16;  /* RtlUnwind (4) */
    case 327: return  4;  /* XeLoadSection (1) */
    case 328: return  4;  /* XeUnloadSection (1) */
    case 333: return 12;  /* WRITE_PORT_BUFFER_USHORT (3) */
    case 334: return 12;  /* WRITE_PORT_BUFFER_ULONG (3) */
    case 335: return  4;  /* XcSHAInit (1) */
    case 336: return 12;  /* XcSHAUpdate (3) */
    case 337: return  8;  /* XcSHAFinal (2) */
    case 338: return 12;  /* XcRC4Key (3) */
    case 339: return 12;  /* XcRC4Crypt (3) */
    case 340: return 28;  /* XcHMAC (7) */
    case 341: return 12;  /* XcPKEncPublic (3) */
    case 342: return 12;  /* XcPKDecPrivate (3) */
    case 343: return  4;  /* XcPKGetKeyLen (1) */
    case 344: return 12;  /* XcVerifyPKCS1Signature (3) */
    case 345: return 20;  /* XcModExp (5) */
    case 346: return  8;  /* XcDESKeyParity (2) */
    case 347: return 12;  /* XcKeyTable (3) */
    case 348: return 20;  /* XcBlockCrypt (5) */
    case 349: return 28;  /* XcBlockCryptCBC (7) */
    case 350: return  8;  /* XcCryptService (2) */
    case 351: return  8;  /* XcUpdateCrypto (2) */
    case 352: return 12;  /* RtlRip (3) */
    case 358: return  0;  /* HalIsResetOrShutdownPending (void) */
    case 359: return  4;  /* IoMarkIrpMustComplete (1) */

    /* ── Unknown stubs ── */

    /* ── Pool Allocator ── */
    /* ordinal 16 is the ExEventObjectType data export; see
     * kernel_thunks.c, which points its thunk at kernel data.
     * 17 is ExFreePool and is a real function. */

    /* ── HAL ── */

    /* ── I/O Manager ── */
    /* ordinal 64 is the IoCompletionObjectType data export; see
     * kernel_thunks.c. 65 is IoCreateDevice, a real function. */
    /* case  71: DATA export - IoDeviceObjectType */

    /* ── Kernel Synchronization ── */
    /* case 156: DATA export - KeTickCount */

    /* ── Launch Data ── */
    /* case 164: DATA export - LaunchDataPage */

    /* ── Memory Management ── */

    /* ── NT Virtual Memory ── */

    /* ── NT File I/O & Handle ── */

    /* ── Object Manager ── */
    case 246: return 12;  /* ObReferenceObjectByHandle(3) - Xbox: Handle,Type,Object* */
    case 360: return  0;  /* HalInitiateShutdown (void) */

    /* ── Network / PHY ── */

    /* ── Threading ── */
    /* case 259: DATA export - PsThreadObjectType */

    /* ── Runtime Library ── */

    /* ── Xbox Identity (data exports) ── */
    /* cases 322-328, 355-357: DATA exports */

    /* ── Port I/O ── */

    /* ── Crypto ── */

    /* Data exports */
    case 102: return 0;  /* MmGlobalData (void/data) */
    case 120: return 0;  /* KeInterruptTime (void/data) */
    case 154: return 0;  /* KeSystemTime (void/data) */
    case 240: return 0;  /* ObDirectoryObjectType (void/data) */
    case 245: return 0;  /* ObpObjectHandleTable (void/data) */
    case 249: return 0;  /* ObSymbolicLinkObjectType (void/data) */

    /* Dbg */
    case   6: return  4;  /* DbgBreakPointWithStatus (1) */
    case   7: return 12;  /* DbgLoadImageSymbols (3) */
    case  10: return  8;  /* DbgPrompt (2) */
    case  11: return 12;  /* DbgUnLoadImageSymbols (3) */

    /* Ex */
    case  12: return  4;  /* ExAcquireReadWriteLockExclusive (1) */
    case  13: return  4;  /* ExAcquireReadWriteLockShared (1) */
    case  18: return  4;  /* ExInitializeReadWriteLock (1) */
    case  19: return 12;  /* ExInterlockedAddLargeInteger (3) */
    case  20: return  8;  /* ExInterlockedAddLargeStatistic (2) */
    case  21: return 16;  /* ExInterlockedCompareExchange64 (4) */
    case  25: return 12;  /* ExReadWriteRefurbInfo (3) */
    case  26: return  4;  /* ExRaiseException (1) */
    case  27: return  4;  /* ExRaiseStatus (1) */
    case  28: return  4;  /* ExReleaseReadWriteLock (1) */

    /* ExfInterlocked */
    case  32: return  8;  /* ExfInterlockedInsertHeadList (2) */
    case  33: return  8;  /* ExfInterlockedInsertTailList (2) */
    case  34: return  4;  /* ExfInterlockedRemoveHeadList (1) */

    /* Fsc */
    case  36: return  0;  /* FscInvalidateIdleBlocks (void) */

    /* Hal */
    case  43: return  8;  /* HalEnableSystemInterrupt (2) */
    case 365: return  0;  /* HalEnableSecureTrayEject (void) */
    case 366: return  4;  /* HalWriteSMCScratchRegister (1) */

    /* Interlocked (fastcall, args in ecx/edx) */
    case  51: return  0;  /* InterlockedCompareExchange (fastcall) */
    case  52: return  0;  /* InterlockedDecrement (fastcall) */
    case  53: return  0;  /* InterlockedIncrement (fastcall) */
    case  54: return  0;  /* InterlockedExchange (fastcall) */
    case  55: return  0;  /* InterlockedExchangeAdd (fastcall) */
    case  56: return  0;  /* InterlockedFlushSList (fastcall) */
    case  57: return  0;  /* InterlockedPopEntrySList (fastcall) */
    case  58: return  0;  /* InterlockedPushEntrySList (fastcall) */

    /* Io */
    case  59: return  8;  /* IoAllocateIrp (2) */
    case  60: return 28;  /* IoBuildAsynchronousFsdRequest (7) */
    case  63: return 20;  /* IoCheckShareAccess (5) */
    case  72: return  4;  /* IoFreeIrp (1) */
    case  75: return 20;  /* IoQueryFileInformation (5) */
    case  76: return 20;  /* IoQueryVolumeInformation (5) */
    case  77: return  4;  /* IoQueueThreadIrp (1) */
    case  78: return  8;  /* IoRemoveShareAccess (2) */
    case  80: return 20;  /* IoSetShareAccess (5) */

    /* Kd */
    case  88: return  0;  /* KdDebuggerEnabled (void) */
    case  89: return  0;  /* KdDebuggerNotPresent (void) */

    /* Ke (new) */
    case  92: return  4;  /* KeAlertResumeThread (1) */
    case  94: return  8;  /* KeBoostPriorityThread (2) */
    case 101: return  0;  /* KeEnterCriticalRegion (void) */
    case 103: return  0;  /* KeGetCurrentIrql (void) */
    case 104: return  0;  /* KeGetCurrentThread (void) */
    case 105: return 24;  /* KeInitializeApc (6) */
    case 106: return  4;  /* KeInitializeDeviceQueue (1) */
    case 108: return 12;  /* KeInitializeEvent (3) */
    case 110: return  8;  /* KeInitializeMutant (2) */
    case 111: return  8;  /* KeInitializeQueue (2) */
    case 112: return 12;  /* KeInitializeSemaphore (3) */
    case 114: return 12;  /* KeInsertByKeyDeviceQueue (3) */
    case 115: return  8;  /* KeInsertDeviceQueue (2) */
    case 116: return  8;  /* KeInsertHeadQueue (2) */
    case 117: return  8;  /* KeInsertQueue (2) */
    case 118: return 16;  /* KeInsertQueueApc (4) */
    case 121: return  0;  /* KeIsExecutingDpc (void) */
    case 122: return  0;  /* KeLeaveCriticalRegion (void) */
    case 123: return 12;  /* KePulseEvent (3) */
    case 130: return  0;  /* KeRaiseIrqlToSynchLevel (void) */
    case 131: return 16;  /* KeReleaseMutant (4) */
    case 132: return 16;  /* KeReleaseSemaphore (4) */
    case 133: return  8;  /* KeRemoveByKeyDeviceQueue (2) */
    case 134: return  4;  /* KeRemoveDeviceQueue (1) */
    case 135: return  8;  /* KeRemoveEntryDeviceQueue (2) */
    case 136: return  4;  /* KeRemoveQueue (1) */
    case 138: return  4;  /* KeResetEvent (1) */
    case 140: return  4;  /* KeResumeThread (1) */
    case 141: return  4;  /* KeRundownQueue (1) */
    case 146: return  8;  /* KeSetEventBoostPriority (2) */
    case 147: return  8;  /* KeSetPriorityProcess (2) */
    case 148: return  8;  /* KeSetPriorityThread (2) */
    case 152: return  4;  /* KeSuspendThread (1) */
    case 155: return  4;  /* KeTestAlertThread (1) */
    case 162: return  0;  /* KiBugCheckData (void/data) */
    case 163: return  4;  /* KiUnlockDispatcherDatabase (1) */

    /* Mm */
    case 174: return  4;  /* MmIsAddressValid (1) */
    case 374: return  8;  /* MmDbgAllocateMemory (2) */
    case 375: return  8;  /* MmDbgFreeMemory (2) */
    case 376: return  0;  /* MmDbgQueryAvailablePages (void) */
    case 377: return  8;  /* MmDbgReleaseAddress (2) */
    case 378: return  8;  /* MmDbgWriteCheck (2) */

    /* Nt */
    case 201: return 12;  /* NtOpenDirectoryObject (3) */
    case 208: return 28;  /* NtQueryDirectoryObject (7) */
    case 209: return 16;  /* NtQueryEvent (4) */
    case 212: return 20;  /* NtQueryIoCompletion (5) */
    case 213: return 20;  /* NtQueryMutant (5) */
    case 214: return 20;  /* NtQuerySemaphore (5) */
    case 216: return 20;  /* NtQueryTimer (5) */

    /* Ob */
    case 239: return 28;  /* ObCreateObject (7) */
    case 241: return 24;  /* ObInsertObject (6) */
    case 242: return  4;  /* ObMakeTemporaryObject (1) */
    case 244: return 20;  /* ObOpenObjectByPointer (5) */
    case 248: return  4;  /* ObReferenceObjectByPointer (1) */

    /* Ps */
    case 254: return 28;  /* PsCreateSystemThread (7) */
    case 256: return  4;  /* PsQueryStatistics (1) */
    case 257: return  8;  /* PsSetCreateThreadNotifyRoutine (2) */

    /* Rtl */
    case 281: return 12;  /* RtlExtendedIntegerMultiply (3) */
    case 282: return 16;  /* RtlExtendedLargeIntegerDivide (4) */
    case 283: return 16;  /* RtlExtendedMagicDivide (4) */
    case 303: return  4;  /* RtlRaiseStatus (1) */
    case 361: return  0;  /* RtlSnprintf - __cdecl, caller cleans */
    case 362: return  0;  /* RtlSprintf - __cdecl, caller cleans */
    case 363: return  0;  /* RtlVsnprintf - __cdecl, caller cleans */
    case 364: return  0;  /* RtlVsprintf - __cdecl, caller cleans */

    /* Port I/O */
    case 329: return 12;  /* READ_PORT_BUFFER_UCHAR (3) */
    case 330: return 12;  /* READ_PORT_BUFFER_USHORT (3) */
    case 331: return 12;  /* READ_PORT_BUFFER_ULONG (3) */
    case 332: return 12;  /* WRITE_PORT_BUFFER_UCHAR (3) */

    /* Not 0: a genuine zero-argument function and an ordinal nobody has
     * written down are both "pop nothing", but only one of them is a
     * problem, and the warning below could not tell them apart -- it
     * accused FscGetCacheSize, which really does take no arguments. */
    default:  return -1;  /* DATA exports or truly unknown */
    }
}

static bridge_func_t bridge_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* Threading */
    case 255: return bridge_PsCreateSystemThreadEx;
    case 258: return bridge_PsTerminateSystemThread;

    /* File/Handle */
    case 187: return bridge_NtClose;
    case 190: return bridge_NtCreateFile;
    case 279: return bridge_RtlEqualString;
    case 289: return bridge_RtlInitAnsiString;
    case 195: return bridge_NtDeleteFile;
    case 196: return bridge_NtDeviceIoControlFile;
    case 198: return bridge_NtFlushBuffersFile;
    case 200: return bridge_NtFsControlFile;
    case 202: return bridge_NtOpenFile;
    case 203: return bridge_NtOpenSymbolicLinkObject;
    case 207: return bridge_NtQueryDirectoryFile;
    case 210: return bridge_NtQueryFullAttributesFile;
    case 211: return bridge_NtQueryInformationFile;
    case 218: return bridge_NtQueryVolumeInformationFile;
    case 219: return bridge_NtReadFile;
    case 312: return bridge_RtlUnwind;
    case 327: return bridge_XeLoadSection;
    case 328: return bridge_XeUnloadSection;
    case 226: return bridge_NtSetInformationFile;
    case 236: return bridge_NtWriteFile;

    /* Memory - contiguous */
    case 165: return bridge_MmAllocateContiguousMemory;
    case 166: return bridge_MmAllocateContiguousMemoryEx;
    case 171: return bridge_MmFreeContiguousMemory;
    case 173: return bridge_MmGetPhysicalAddress;
    case 182: return bridge_MmSetAddressProtect;
    case 181: return bridge_MmQueryStatistics;

    /* Memory - virtual */
    case 184: return bridge_NtAllocateVirtualMemory;
    case 199: return bridge_NtFreeVirtualMemory;
    case 215: return bridge_NtQuerySymbolicLinkObject;
    case 217: return bridge_NtQueryVirtualMemory;

    /* Pool */
    case  14: return bridge_ExAllocatePool;
    case  15: return bridge_ExAllocatePoolWithTag;
    case  23: return bridge_ExQueryPoolBlockSize;
    case 268: return bridge_RtlCompareMemory;
    case 269: return bridge_RtlCompareMemoryUlong;
    case  35: return bridge_FscGetCacheSize;
    case  37: return bridge_FscSetCacheSize;
    case  24: return bridge_ExQueryNonVolatileSetting;

    /* IRQL */
    case 160: return bridge_KfRaiseIrql;
    case 161: return bridge_KfLowerIrql;
    case 129: return bridge_KeRaiseIrqlToDpcLevel;

    /* Critical sections */
    case 291: return bridge_RtlInitializeCriticalSection;
    /* Wreckless asks for the AV pack during D3D device creation; unbridged it
     * read 0, which is "no pack connected". Writes one ULONG through a guest
     * pointer it is given and nothing else, and XBOX_TO_NATIVE turns a guest
     * NULL into a host NULL so xbox_AvSendTVEncoderOption's own !Result guard
     * still fires. */
    case   2: return bridge_AvSendTVEncoderOption;
    /* Reads a LARGE_INTEGER and fills a TIME_FIELDS, both at caller-supplied
     * guest addresses -- the case the NOT ROUTED note below names as fine. */
    case 305: return bridge_RtlTimeToTimeFields;
    case 277: return bridge_RtlEnterCriticalSection;
    case 294: return bridge_RtlLeaveCriticalSection;

    /* Timing */
    case 126: return bridge_KeQueryPerformanceCounter;
    case 127: return bridge_KeQueryPerformanceFrequency;
    case 128: return bridge_KeQuerySystemTime;
    case 149: return bridge_KeSetTimer;
    case 150: return bridge_KeSetTimerEx;

    /* DPC / Timer init */
    case 107: return bridge_KeInitializeDpc;
    case 119: return bridge_KeInsertQueueDpc;
    case 137: return bridge_KeRemoveQueueDpc;
    case 153: return bridge_KeSynchronizeExecution;
    case 113: return bridge_KeInitializeTimerEx;

    /* NV2A interrupt plumbing */
    case  44: return bridge_HalGetInterruptVector;
    case  98: return bridge_KeConnectInterrupt;
    case 109: return bridge_KeInitializeInterrupt;
    case  47: return bridge_HalRegisterShutdownNotification;
    case 168: return bridge_MmClaimGpuInstanceMemory;

    /* Synchronization */
    case 189: return bridge_NtCreateEvent;
    case 145: return bridge_KeSetEvent;
    case 159: return bridge_KeWaitForSingleObject;
    case  99: return bridge_KeDelayExecutionThread;
    case 179: return bridge_MmQueryAddressProtect;
    case 232: return bridge_NtUserIoApcDispatcher;
    case  95: return bridge_KeBugCheck;
    case  96: return bridge_KeBugCheckEx;
    case 186: return bridge_NtClearEvent;
    case 205: return bridge_NtPulseEvent;
    case 225: return bridge_NtSetEvent;
    case 233: return bridge_NtWaitForSingleObject;
    case 234: return bridge_NtWaitForSingleObjectEx;
    case 235: return bridge_NtWaitForMultipleObjectsEx;
    case 238: return bridge_NtYieldExecution;

    /* Hardware */
    case   9: return bridge_HalReadSMCTrayState;
    case  49: return bridge_HalReturnToFirmware;

    /* Display */
    case   3: return bridge_AvSetDisplayMode;

    /* I/O */
    case  66: return bridge_IoCreateFile;
    case  65: return bridge_IoCreateDevice;
    case  67: return bridge_IoCreateSymbolicLink;
    case 188: return bridge_NtCreateDirectoryObject;
    case 246: return bridge_ObReferenceObjectByHandle;

    /* Memory - I/O mapping */
    case 177: return bridge_MmMapIoSpace;
    case 178: return bridge_MmPersistContiguousMemory;

    /* RTL */
    case 301: return bridge_RtlNtStatusToDosError;
    case 302: return bridge_RtlRaiseException;

    /* Section B: per-ordinal bridges (261-321), defined in the splice above. */
    case 261: return bridge_RtlAppendStringToString;
    case 262: return bridge_RtlAppendUnicodeStringToString;
    case 263: return bridge_RtlAppendUnicodeToString;
    case 264: return bridge_RtlAssert;
    case 265: return bridge_RtlCaptureContext;
    case 266: return bridge_RtlCaptureStackBackTrace;
    case 267: return bridge_RtlCharToInteger;
    case 270: return bridge_RtlCompareString;
    case 271: return bridge_RtlCompareUnicodeString;
    case 272: return bridge_RtlCopyString;
    case 273: return bridge_RtlCopyUnicodeString;
    case 274: return bridge_RtlCreateUnicodeString;
    case 275: return bridge_RtlDowncaseUnicodeChar;
    case 276: return bridge_RtlDowncaseUnicodeString;
    case 278: return bridge_RtlEnterCriticalSectionAndRegion;
    case 280: return bridge_RtlEqualUnicodeString;
    case 284: return bridge_RtlFillMemory;
    case 287: return bridge_RtlFreeUnicodeString;
    case 288: return bridge_RtlGetCallersAddress;
    case 292: return bridge_RtlIntegerToChar;
    case 293: return bridge_RtlIntegerToUnicodeString;
    case 295: return bridge_RtlLeaveCriticalSectionAndRegion;
    case 296: return bridge_RtlLowerChar;
    case 297: return bridge_RtlMapGenericMask;
    case 298: return bridge_RtlMoveMemory;
    case 299: return bridge_RtlMultiByteToUnicodeN;
    case 300: return bridge_RtlMultiByteToUnicodeSize;
    case 306: return bridge_RtlTryEnterCriticalSection;
    case 307: return bridge_RtlUlongByteSwap;
    case 309: return bridge_RtlUnicodeStringToInteger;
    case 310: return bridge_RtlUnicodeToMultiByteN;
    case 311: return bridge_RtlUnicodeToMultiByteSize;
    case 313: return bridge_RtlUpcaseUnicodeChar;
    case 314: return bridge_RtlUpcaseUnicodeString;
    case 315: return bridge_RtlUpcaseUnicodeToMultiByteN;
    case 316: return bridge_RtlUpperChar;
    case 317: return bridge_RtlUpperString;
    case 318: return bridge_RtlUshortByteSwap;
    case 319: return bridge_RtlWalkFrameChain;
    case 320: return bridge_RtlZeroMemory;
    case 321: return bridge_XboxEEPROMKey;


    /* Routed. The XcRC4 pair was turned OFF mid-bisect and never turned back
     * on. Both pass the memory-model bar: a guest RC4_CONTEXT and a guest data
     * buffer, every access translated through XBOX_TO_NATIVE, no allocation
     * and no host pointer returned. Unrouted, savegame/EEPROM code that used
     * RC4 silently did nothing. */
    case 338: return bridge_XcRC4Key;
    case 339: return bridge_XcRC4Crypt;

    /* Routed. The lesson of the block that used to sit here was that an
     * xbox_* existence does not make a wrapper mechanical: each candidate has
     * to be checked for which side owns the allocation and whether an
     * out-pointer must carry a guest VA. IoCreateDevice was fixed by
     * allocating the device object in guest memory; ExFreePool below answers
     * through the guest heap; MmUnmapIoSpace/IoDeleteDevice do the same.
     *
     * The three beside them were parked, never broken: AvGetSavedDataAddress
     * returns a ULONG value, KeCancelTimer takes a guest KTIMER through
     * XBOX_TO_NATIVE, KeDisconnectInterrupt takes a guest KINTERRUPT the same
     * way. None of them allocates, frees, or hands back a host pointer. */
    case   1: return bridge_AvGetSavedDataAddress;
    case  17: return bridge_ExFreePool;
    case  97: return bridge_KeCancelTimer;
    case 100: return bridge_KeDisconnectInterrupt;
    /* Routed: thread base priority, queried and set. Neither allocates,
     * frees, nor returns a pointer -- each takes a thread handle and a
     * LONG. Half-Life 2 calls both while starting its worker threads,
     * and unbridged they returned 0, so every thread read its own base
     * priority as 0 and any priority the title set was discarded. */
    case 124: return bridge_KeQueryBasePriorityThread;
    case 143: return bridge_KeSetBasePriorityThread;
    /* Routed. Both clear the memory-model bar above: neither allocates,
     * frees, nor hands back a host pointer. KeStallExecutionProcessor takes a
     * microsecond count and busy-waits -- no pointers at all.
     * MmLockUnlockBufferPages takes (BaseAddress, Length, UnlockPages) and
     * pins physical pages, which is a no-op on the host; XBOX_TO_NATIVE is the
     * correct marshalling for its one address argument, and it writes nothing
     * through it.
     *
     * Half-Life 2 calls both during C++ static initialisation. Unbridged they
     * returned 0 without stalling or locking anything -- harmless in isolation,
     * but they are exactly the kind of silent no-op that makes a later failure
     * unattributable. */
    case 151: return bridge_KeStallExecutionProcessor;
    case 175: return bridge_MmLockUnlockBufferPages;
    /* Routed, both checked against the memory-model bar above.
     *
     * MmQueryAllocationSize now answers from the guest heap's block table
     * instead of the host's VirtualQuery, so nothing crosses the two worlds.
     *
     * NtCreateMutant creates a host mutex and hands it back through
     * bridge_write_handle, which is a guest token -- the same shape as
     * NtCreateEvent, which has been routed all along. It allocates no guest
     * memory and returns no host pointer.
     *
     * The Xbox Dashboard needs the mutant: its audio thread creates one during
     * the first tick, and unbridged the call returned STATUS_SUCCESS without
     * writing a handle, so the main thread waited on five events that nothing
     * would ever signal. */
    case 180: return bridge_MmQueryAllocationSize;
    case 192: return bridge_NtCreateMutant;
    case 221: return bridge_NtReleaseMutant;
    /* Routed. Checked against the memory-model warning above rather than
     * assumed mechanical: NtResumeThread takes a handle token and writes a
     * 4-byte suspend count through an optional out-parameter. Guest ULONG and
     * host ULONG are both 4 bytes, XBOX_TO_NATIVE already maps NULL to NULL,
     * and xbox_NtResumeThread checks the pointer before writing. Nothing
     * allocates, frees, or hands back a host pointer -- which is what
     * disqualified IoCreateDevice and ExFreePool.
     *
     * Halo 2276 calls this immediately before its first camera frustum build;
     * unbridged it returned 0 (STATUS_SUCCESS) without resuming anything, so a
     * thread the title had created suspended never started. */
    case 224: return bridge_NtResumeThread;
    case 231: return bridge_NtSuspendThread;
    /* Routed. ObfDereferenceObject is the fastcall partner of 251 (see its
     * wrapper) and only touches the object's reference count; PhyGetLinkState
     * reports link-up and PhyInitialize is a successful no-op. None of the
     * three allocates, frees, or hands back a host pointer; PhyGetLinkState is
     * what titles poll for Ethernet presence before selecting their network
     * path. */
    case 250: return bridge_ObfDereferenceObject;
    case 252: return bridge_PhyGetLinkState;
    case 253: return bridge_PhyInitialize;
    /* Routed. The memory-model note above already names this group as the
     * safe kind: each one reads or writes bytes at an address the caller
     * supplied, and none allocates, frees, or hands back a host pointer.
     *
     * Unbridged they returned 0 without hashing anything, which is invisible
     * until something checks a digest. The Xbox Dashboard verifies each XIP
     * archive it loads against a 20-byte digest in its own table
     * (sub_00034924) and calls HalReturnToFirmware(4) when the compare fails
     * -- so a no-op SHA does not corrupt anything, it reboots the console. */
    case 335: return bridge_XcSHAInit;
    case 336: return bridge_XcSHAUpdate;
    case 337: return bridge_XcSHAFinal;
    case 340: return bridge_XcHMAC;
    case 346: return bridge_XcDESKeyParity;

    /* Routed. Each of these was checked against the memory-model bar
     * described above before being added: reads or writes only happen at
     * caller-supplied guest addresses, and nothing allocates, frees, or hands
     * back a host pointer. Two of them (MmUnmapIoSpace, IoDeleteDevice) free
     * guest memory, so they deliberately answer through the guest heap rather
     * than calling the xbox_* host-heap version -- see the wrappers. */
    case  29: return bridge_ExSaveNonVolatileSetting;
    case  38: return bridge_HalClearSoftwareInterrupt;
    case  45: return bridge_HalReadSMBusValue;
    case  46: return bridge_HalReadWritePCISpace;
    case  48: return bridge_HalRequestSoftwareInterrupt;
    case  50: return bridge_HalWriteSMBusValue;
    case  68: return bridge_IoDeleteDevice;
    case  93: return bridge_KeAlertThread;
    case 125: return bridge_KeQueryInterruptTime;
    case 139: return bridge_KeRestoreFloatingPointState;
    case 142: return bridge_KeSaveFloatingPointState;
    case 158: return bridge_KeWaitForMultipleObjects;
    case 183: return bridge_MmUnmapIoSpace;
    case 193: return bridge_NtCreateSemaphore;
    case 222: return bridge_NtReleaseSemaphore;
    case 228: return bridge_NtSetSystemTime;
    case 247: return bridge_ObReferenceObjectByName;
    case 251: return bridge_ObfReferenceObject;
    case 290: return bridge_RtlInitUnicodeString;
    case 304: return bridge_RtlTimeFieldsToTime;

    /* Routed. The next batch is every remaining xbox_* implementation that
     * clears the memory-model bar, split by category:
     *
     *  - HAL/AV/port-I/O and the I/O Manager stubs all take or produce value
     *    arguments and caller-supplied guest structures; none of them returns
     *    a pointer the title dereferences. IofCallDriver/IofCompleteRequest
     *    are __fastcall (ns in ecx/edx).
     *  - The Xc* entries are documented stubs (no Xbox Live, no on-console key
     *    derivation); pointers are translated but never dereferenced.
     *  - MmAllocateSystemMemory/MmCreateKernelStack answer through the guest
     *    heap rather than VirtualAlloc so the title sees a 32-bit guest VA;
     *    their frees pair with them.
     *  - NtDuplicateObject dups a real native handle and hands it back as a
     *    fresh guest token; RtlRip logs guest strings; the ANSI<->Unicode
     *    conversion bridges do the whole conversion in guest memory (their
     *    xbox_* versions write host pointers into 4-byte guest slots).
     */
    case   4: return bridge_AvSetSavedDataAddress;
    case   5: return bridge_DbgBreakPoint;
    case  39: return bridge_HalDisableSystemInterrupt;
    case 360: return bridge_HalInitiateShutdown;
    case 358: return bridge_HalIsResetOrShutdownPending;
    case 333: return bridge_WRITE_PORT_BUFFER_USHORT;
    case 334: return bridge_WRITE_PORT_BUFFER_ULONG;

    case  61: return bridge_IoBuildDeviceIoControlRequest;
    case  62: return bridge_IoBuildSynchronousFsdRequest;
    case  69: return bridge_IoDeleteSymbolicLink;
    case  73: return bridge_IoInitializeIrp;
    case  74: return bridge_IoInvalidDeviceRequest;
    case  79: return bridge_IoSetIoCompletion;
    case  81: return bridge_IoStartNextPacket;
    case  82: return bridge_IoStartNextPacketByKey;
    case  83: return bridge_IoStartPacket;
    case  84: return bridge_IoSynchronousDeviceIoControlRequest;
    case  85: return bridge_IoSynchronousFsdRequest;
    case  86: return bridge_IofCallDriver;
    case  87: return bridge_IofCompleteRequest;
    case 359: return bridge_IoMarkIrpMustComplete;

    case 176: return bridge_MmLockUnlockPhysicalPage;
    /* Allocator and free together: both answer from the guest heap, and the
     * allocator page-aligns and zeroes because that is what the console's
     * page allocator returns and callers assume it. */
    case 167: return bridge_MmAllocateSystemMemory;
    case 172: return bridge_MmFreeSystemMemory;
    case 169: return bridge_MmCreateKernelStack;
    case 170: return bridge_MmDeleteKernelStack;

    case 341: return bridge_XcPKEncPublic;
    case 342: return bridge_XcPKDecPrivate;
    case 343: return bridge_XcPKGetKeyLen;
    case 344: return bridge_XcVerifyPKCS1Signature;
    case 345: return bridge_XcModExp;
    case 347: return bridge_XcKeyTable;
    case 348: return bridge_XcBlockCrypt;
    case 349: return bridge_XcBlockCryptCBC;
    case 350: return bridge_XcCryptService;
    case 351: return bridge_XcUpdateCrypto;

    case 352: return bridge_RtlRip;
    case 260: return bridge_RtlAnsiStringToUnicodeString;
    case 308: return bridge_RtlUnicodeStringToAnsiString;
    case 197: return bridge_NtDuplicateObject;

    /* Data exports */
    case 102: return bridge_MmGlobalData;
    case 120: return bridge_KeInterruptTime;
    case 154: return bridge_KeSystemTime;
    case 240: return bridge_ObDirectoryObjectType;
    case 245: return bridge_ObpObjectHandleTable;
    case 249: return bridge_ObSymbolicLinkObjectType;

    /* Dbg */
    case   6: return bridge_DbgBreakPointWithStatus;
    case   7: return bridge_DbgLoadImageSymbols;
    case   8: return bridge_DbgPrint;
    case  10: return bridge_DbgPrompt;
    case  11: return bridge_DbgUnLoadImageSymbols;

    /* Ex */
    case  12: return bridge_ExAcquireReadWriteLockExclusive;
    case  13: return bridge_ExAcquireReadWriteLockShared;
    case  18: return bridge_ExInitializeReadWriteLock;
    case  19: return bridge_ExInterlockedAddLargeInteger;
    case  20: return bridge_ExInterlockedAddLargeStatistic;
    case  21: return bridge_ExInterlockedCompareExchange64;
    case  25: return bridge_ExReadWriteRefurbInfo;
    case  26: return bridge_ExRaiseException;
    case  27: return bridge_ExRaiseStatus;
    case  28: return bridge_ExReleaseReadWriteLock;

    /* ExfInterlocked */
    case  32: return bridge_ExfInterlockedInsertHeadList;
    case  33: return bridge_ExfInterlockedInsertTailList;
    case  34: return bridge_ExfInterlockedRemoveHeadList;

    /* Fsc */
    case  36: return bridge_FscInvalidateIdleBlocks;

    /* Hal */
    case  43: return bridge_HalEnableSystemInterrupt;
    case 365: return bridge_HalEnableSecureTrayEject;
    case 366: return bridge_HalWriteSMCScratchRegister;

    /* Interlocked */
    case  51: return bridge_InterlockedCompareExchange;
    case  52: return bridge_InterlockedDecrement;
    case  53: return bridge_InterlockedIncrement;
    case  54: return bridge_InterlockedExchange;
    case  55: return bridge_InterlockedExchangeAdd;
    case  56: return bridge_InterlockedFlushSList;
    case  57: return bridge_InterlockedPopEntrySList;
    case  58: return bridge_InterlockedPushEntrySList;

    /* Io */
    case  59: return bridge_IoAllocateIrp;
    case  60: return bridge_IoBuildAsynchronousFsdRequest;
    case  63: return bridge_IoCheckShareAccess;
    case  72: return bridge_IoFreeIrp;
    case  75: return bridge_IoQueryFileInformation;
    case  76: return bridge_IoQueryVolumeInformation;
    case  77: return bridge_IoQueueThreadIrp;
    case  78: return bridge_IoRemoveShareAccess;
    case  80: return bridge_IoSetShareAccess;
    case  90: return bridge_IoDismountVolume;
    case  91: return bridge_IoDismountVolumeByName;

    /* Kd */
    case  88: return bridge_KdDebuggerEnabled;
    case  89: return bridge_KdDebuggerNotPresent;

    /* Ke (new) */
    case  92: return bridge_KeAlertResumeThread;
    case  94: return bridge_KeBoostPriorityThread;
    case 101: return bridge_KeEnterCriticalRegion;
    case 103: return bridge_KeGetCurrentIrql;
    case 104: return bridge_KeGetCurrentThread;
    case 105: return bridge_KeInitializeApc;
    case 106: return bridge_KeInitializeDeviceQueue;
    case 108: return bridge_KeInitializeEvent;
    case 110: return bridge_KeInitializeMutant;
    case 111: return bridge_KeInitializeQueue;
    case 112: return bridge_KeInitializeSemaphore;
    case 114: return bridge_KeInsertByKeyDeviceQueue;
    case 115: return bridge_KeInsertDeviceQueue;
    case 116: return bridge_KeInsertHeadQueue;
    case 117: return bridge_KeInsertQueue;
    case 118: return bridge_KeInsertQueueApc;
    case 121: return bridge_KeIsExecutingDpc;
    case 122: return bridge_KeLeaveCriticalRegion;
    case 123: return bridge_KePulseEvent;
    case 130: return bridge_KeRaiseIrqlToSynchLevel;
    case 131: return bridge_KeReleaseMutant;
    case 132: return bridge_KeReleaseSemaphore;
    case 133: return bridge_KeRemoveByKeyDeviceQueue;
    case 134: return bridge_KeRemoveDeviceQueue;
    case 135: return bridge_KeRemoveEntryDeviceQueue;
    case 136: return bridge_KeRemoveQueue;
    case 138: return bridge_KeResetEvent;
    case 140: return bridge_KeResumeThread;
    case 141: return bridge_KeRundownQueue;
    case 144: return bridge_KeSetDisableBoostThread;
    case 146: return bridge_KeSetEventBoostPriority;
    case 147: return bridge_KeSetPriorityProcess;
    case 148: return bridge_KeSetPriorityThread;
    case 152: return bridge_KeSuspendThread;
    case 155: return bridge_KeTestAlertThread;
    case 162: return bridge_KiBugCheckData;
    case 163: return bridge_KiUnlockDispatcherDatabase;

    /* Mm */
    case 174: return bridge_MmIsAddressValid;
    case 374: return bridge_MmDbgAllocateMemory;
    case 375: return bridge_MmDbgFreeMemory;
    case 376: return bridge_MmDbgQueryAvailablePages;
    case 377: return bridge_MmDbgReleaseAddress;
    case 378: return bridge_MmDbgWriteCheck;

    /* Nt */
    case 185: return bridge_NtCancelTimer;
    case 191: return bridge_NtCreateIoCompletion;
    case 194: return bridge_NtCreateTimer;
    case 201: return bridge_NtOpenDirectoryObject;
    case 204: return bridge_NtProtectVirtualMemory;
    case 206: return bridge_NtQueueApcThread;
    case 208: return bridge_NtQueryDirectoryObject;
    case 209: return bridge_NtQueryEvent;
    case 212: return bridge_NtQueryIoCompletion;
    case 213: return bridge_NtQueryMutant;
    case 214: return bridge_NtQuerySemaphore;
    case 216: return bridge_NtQueryTimer;
    case 220: return bridge_NtReadFileScatter;
    case 223: return bridge_NtRemoveIoCompletion;
    case 227: return bridge_NtSetIoCompletion;
    case 229: return bridge_NtSetTimerEx;
    case 230: return bridge_NtSignalAndWaitForSingleObjectEx;
    case 237: return bridge_NtWriteFileGather;

    /* Ob */
    case 239: return bridge_ObCreateObject;
    case 241: return bridge_ObInsertObject;
    case 242: return bridge_ObMakeTemporaryObject;
    case 243: return bridge_ObOpenObjectByName;
    case 244: return bridge_ObOpenObjectByPointer;
    case 248: return bridge_ObReferenceObjectByPointer;

    /* Ps */
    case 254: return bridge_PsCreateSystemThread;
    case 256: return bridge_PsQueryStatistics;
    case 257: return bridge_PsSetCreateThreadNotifyRoutine;

    /* Rtl */
    case 281: return bridge_RtlExtendedIntegerMultiply;
    case 282: return bridge_RtlExtendedLargeIntegerDivide;
    case 283: return bridge_RtlExtendedMagicDivide;
    case 285: return bridge_RtlFillMemoryUlong;
    case 286: return bridge_RtlFreeAnsiString;
    case 303: return bridge_RtlRaiseStatus;
    case 361: return bridge_RtlSnprintf;
    case 362: return bridge_RtlSprintf;
    case 363: return bridge_RtlVsnprintf;
    case 364: return bridge_RtlVsprintf;

    /* Port I/O */
    case 329: return bridge_READ_PORT_BUFFER_UCHAR;
    case 330: return bridge_READ_PORT_BUFFER_USHORT;
    case 331: return bridge_READ_PORT_BUFFER_ULONG;
    case 332: return bridge_WRITE_PORT_BUFFER_UCHAR;

    default:  return NULL;
    }
}

/* ── Per-slot bridge functions (resolved at init) ────────── */

static bridge_func_t g_slot_bridges[XBOX_KERNEL_THUNK_TABLE_SIZE];
static int g_slot_arg_bytes[XBOX_KERNEL_THUNK_TABLE_SIZE];
static uint8_t g_slot_arg_unknown[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Xbox VA to sample around each bridge call; 0 = off. See dispatch. */
uint32_t g_kernel_watch_va = 0;

/* Arm the watch from the environment.
 *
 * The facility existed but nothing set it, so it was unreachable.
 * RECOMP_KERNEL_WATCH=<guest addr> samples that dword either side of every
 * bridge call and names the ordinal that changed it -- which is the one fact
 * a hardware watchpoint cannot give, because a bridge corrupting Xbox memory
 * faults inside ntdll with no recompiled frame to blame.
 *
 * A change seen between the previous call's "after" and this call's "before"
 * is guest code, not a bridge, which is just as useful to know. */
static void kernel_watch_arm_once(void)
{
    static int done;
    const char *env;
    if (done)
        return;
    done = 1;
    env = getenv("RECOMP_KERNEL_WATCH");
    if (env)
        g_kernel_watch_va = (uint32_t)strtoul(env, NULL, 0);
}

/* Current dispatching slot */
static int g_kernel_dispatch_slot = -1;

static void kernel_thunk_dispatch(void)
{
    int slot = g_kernel_dispatch_slot;
    bridge_func_t bridge;
    ULONG ordinal;

    if (slot < 0 || slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr, "  [KERNEL] bad slot %d\n", slot);
        g_eax = 0;
        g_esp += 4;  /* pop dummy return address */
        return;
    }

    ordinal = g_slot_ordinals[slot];
    bridge = g_slot_bridges[slot];

    g_kernel_call_count++;
    if (ordinal < XBOX_KERNEL_THUNK_TABLE_SIZE)
        g_ordinal_calls[ordinal]++;

    if (KERNEL_LOG_ON()) {
        /* The guest return address sits at the top of the guest stack: the
         * caller pushed it before dispatching here. Logging it turns "some
         * function is calling this" into "this call site is", which is the
         * difference between guessing and knowing when a title recurses. */
        fprintf(stderr,
                "  [KERNEL] #%d: ordinal %u (slot %d) esp=0x%08X ret=0x%08X\n",
                g_kernel_call_count, ordinal, slot, g_esp,
                g_esp ? BRIDGE_MEM32(g_esp) : 0);
        fflush(stderr);
    }

    {
        static DWORD last_summary_tick = 0;
        DWORD now = GetTickCount();
        if (last_summary_tick == 0) last_summary_tick = now;
        if (now - last_summary_tick >= 2000 && g_kernel_call_count > 200) {
            fprintf(stderr, "  [KERNEL] summary: %d total calls, latest ordinal %u (slot %d) esp=0x%08X\n",
                    g_kernel_call_count, ordinal, slot, g_esp);
            /* And which ones, ranked. "Latest" names whatever the sample
             * happened to land on; the question behind this line is what a
             * title sitting still is actually asking the kernel for, and
             * that wants counting rather than sampling. */
            {
                static unsigned char shown_ord[XBOX_KERNEL_THUNK_TABLE_SIZE];
                int r, shown;

                memset(shown_ord, 0, sizeof shown_ord);
                for (shown = 0; shown < 6; shown++) {
                    int best = -1;
                    for (r = 0; r < XBOX_KERNEL_THUNK_TABLE_SIZE; r++)
                        if (g_ordinal_calls[r] && !shown_ord[r]
                            && (best < 0 || g_ordinal_calls[r] > g_ordinal_calls[best]))
                            best = r;
                    if (best < 0)
                        break;
                    shown_ord[best] = 1;
                    fprintf(stderr, "  [KERNEL]   ordinal %3d x%llu\n", best,
                            (unsigned long long)g_ordinal_calls[best]);
                }
            }
            fflush(stderr);
            last_summary_tick = now;
        }
    }

    /* Pop the dummy return address that PUSH32(esp, 0) pushed before RECOMP_ICALL.
     * On real x86, "call [thunk]" pushes a real return address and "ret" pops it.
     * In our model, the bridge is called directly (not via the simulated stack),
     * so we must manually consume the dummy return address. */
    /* Keep the caller before popping it. A bridge that blocks -- a critical
     * section above all -- can then say which guest call site is holding the
     * lock, which is the one fact a deadlock report otherwise lacks. */
    g_xbox_kernel_caller = g_esp ? BRIDGE_MEM32(g_esp) : 0;
    g_esp += 4;

    /* Name the bridge that corrupts a watched dword.
     *
     * A bridge hands Xbox pointers to real Win32 calls, so a bad one has
     * Windows write into Xbox memory -- the resulting wild write has a stack
     * inside ntdll with no recompiled frame to blame, and a watchpoint just
     * says "something changed". Sampling either side of the call names the
     * ordinal directly, which is the one fact those tools cannot give.
     *
     * Set g_kernel_watch_va to arm; zero (the default) costs one compare. */
    uint32_t _watch_before = 0;
    kernel_watch_arm_once();
    if (g_kernel_watch_va) {
        _watch_before = BRIDGE_MEM32(g_kernel_watch_va);
        /* Reporting only on change misses the case that matters most: a value
         * that was already wrong before the first call sampled it. Printing
         * every sample under RECOMP_KERNEL_WATCH_ALL shows when it changed
         * even if no single bridge did it. */
        if (getenv("RECOMP_KERNEL_WATCH_ALL")) {
            static uint32_t seen = 0xDEADBEEFu;
            if (_watch_before != seen) {
                seen = _watch_before;
                fprintf(stderr, "  [KWATCH] 0x%08X = %08X before ordinal %u"
                                " (call #%d)\n",
                        g_kernel_watch_va, _watch_before, ordinal,
                        g_kernel_call_count);
                fflush(stderr);
            }
        }
    }

    if (bridge) {
        bridge();
    } else {
        /* No specific bridge - return 0. Warn once per ordinal rather than
         * gating on g_kernel_call_count: a missing bridge is rare and is
         * usually the reason a game misbehaves, so it must not be swallowed
         * by the general call-trace throttle. Bounded to one line per slot. */
        static uint8_t warned[XBOX_KERNEL_THUNK_TABLE_SIZE];
        if (!warned[slot]) {
            warned[slot] = 1;
            fprintf(stderr, "  [KERNEL] WARNING: no bridge for ordinal %u (slot %d), returning 0\n",
                    ordinal, slot);
            /* "Returning 0" is the harmless half. The damaging half is the
             * stack: the Xbox kernel is stdcall, so the callee owes the caller
             * its arguments back, and an ordinal missing from
             * stdcall_args_for_ordinal() returns 0 bytes and leaves them
             * there. The caller's own `pop`s then run low by that much and it
             * returns with its callee-saved registers rotated -- silently,
             * frames away from here. Say so, because a title that dies of this
             * looks nothing like a title that is missing a kernel function. */
            if (g_slot_arg_unknown[slot])
                fprintf(stderr, "  [KERNEL]   ordinal %u has no entry in "
                        "stdcall_args_for_ordinal(). If it takes arguments, "
                        "this call is corrupting the caller's stack -- add its "
                        "argument size there before anything else.\n", ordinal);
            fflush(stderr);
        }
        g_eax = 0;
    }

    /* Clean stdcall args from the simulated stack.
     * On real x86, stdcall callee does "ret N" to pop the return address
     * and N bytes of arguments. We already popped the dummy return address
     * above; now pop the args. */
    g_esp += g_slot_arg_bytes[slot];

    if (g_kernel_watch_va) {
        uint32_t _after = BRIDGE_MEM32(g_kernel_watch_va);
        if (_after != _watch_before) {
            fprintf(stderr,
                    "  [KWATCH] ordinal %u changed Xbox VA 0x%08X: "
                    "%08X -> %08X\n",
                    ordinal, g_kernel_watch_va, _watch_before, _after);
            fflush(stderr);
        }
    }

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] → returned 0x%08X\n", g_eax);
        fflush(stderr);
    }
}

/* ── Dispatch lookup ────────────────────────────────────── */

/**
 * Look up a kernel thunk by synthetic VA.
 * Called as a fallback when recomp_lookup() returns NULL.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va)
{
    if (xbox_va >= KERNEL_VA_BASE && xbox_va < KERNEL_VA_END) {
        int slot = (xbox_va - KERNEL_VA_BASE) / 4;
        if (slot >= 0 && slot < XBOX_KERNEL_THUNK_TABLE_SIZE) {
            g_kernel_dispatch_slot = slot;
            return kernel_thunk_dispatch;
        }
    }
    return NULL;
}

/* ── Initialization ─────────────────────────────────────── */

/*
 * Where this title's kernel thunk table lives. Defaults to the compile-time
 * constant, but every XBE puts it somewhere different (it comes from the
 * header's KernelImageThunkAddress), so xbox_MemoryLayoutInit() parses the
 * real address out of the binary and overrides it here.
 *
 * Halo build 2276 puts it at 0x00253090 against the default's 0x0036B7C0 --
 * without the override the bridge patches ordinals into whatever happens to
 * live at the wrong address and every kernel call goes somewhere arbitrary.
 */
static uint32_t g_thunk_table_base  = XBOX_KERNEL_THUNK_TABLE_BASE;
static uint32_t g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;

/**
 * Return the kernel thunk table address and entry count currently in effect.
 * The address is parsed from the XBE header during memory layout init, so the
 * values are per-title, not hardcoded. *base/*count are set to 0 if unavailable.
 */
void xbox_kernel_get_thunk_address(uint32_t *xbox_va, uint32_t *count)
{
    if (xbox_va) *xbox_va = g_thunk_table_base;
    if (count)  *count  = g_thunk_table_count;
}

void xbox_kernel_set_thunk_address(uint32_t xbox_va, uint32_t count)
{
    if (!xbox_va) {
        return;
    }

    g_thunk_table_base = xbox_va;

    /* count indexes g_slot_* arrays, which are sized by the macro. A title
     * importing more slots than the real kernel exports would run off them. */
    if (count && count <= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        g_thunk_table_count = count;
    } else if (count > XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr,
                "  Kernel thunk bridge: XBE declares %u thunk slots, clamping to %d\n",
                count, XBOX_KERNEL_THUNK_TABLE_SIZE);
        g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;
    }
}

/**
 * Resolve the kernel thunk table in Xbox memory.
 *
 * Must be called AFTER xbox_MemoryLayoutInit() so Xbox memory is mapped.
 *
 * Reads the actual ordinals from the XBE memory thunk table (0x80000000|ordinal),
 * resolves each to a per-ordinal bridge function, and replaces the entry
 * with a synthetic VA for dispatch.
 */
/* Per-title ordinal remap; NULL = identity (the kernel's own XDK). Set by
 * xbox_kernel_set_ordinal_remap before init. See kernel.h. */
static const unsigned short *g_ordinal_remap = NULL;
static int g_ordinal_remap_count = 0;

void xbox_kernel_set_ordinal_remap(const unsigned short *map, int count)
{
    g_ordinal_remap = map;
    g_ordinal_remap_count = count;
}

void xbox_kernel_bridge_init(void)
{
    int i;
    int resolved = 0;
    int bridged = 0;
    int unbridged = 0;
    DWORD old_protect;

    fprintf(stderr, "  Kernel thunk bridge: resolving %d entries at 0x%08X\n",
            g_thunk_table_count, g_thunk_table_base);

    /* The thunk table lives in .rdata which is marked PAGE_READONLY.
     * Temporarily make it writable so we can patch the ordinals. */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        PAGE_READWRITE,
        &old_protect
    );

    /* Initialize kernel data export values first */
    kernel_data_init();

    for (i = 0; i < g_thunk_table_count; i++) {
        uint32_t va = g_thunk_table_base + i * 4;
        uint32_t current = BRIDGE_MEM32(va);

        if (current & 0x80000000) {
            /* Read the actual ordinal from Xbox memory, then translate it into
             * the kernel's canonical ordinal space. Identity unless the title
             * set a remap (a different XDK). Every routing decision below --
             * data export, bridge, arg size -- keys off the canonical ordinal,
             * so one translation here covers all three. */
            ULONG ordinal = current & 0x7FFFFFFF;
            if (g_ordinal_remap && ordinal < (ULONG)g_ordinal_remap_count
                && g_ordinal_remap[ordinal]) {
                ordinal = g_ordinal_remap[ordinal];
            }
            g_slot_ordinals[i] = ordinal;

            /* Check if this is a data export */
            uint32_t data_va = kernel_data_va_for_ordinal(ordinal);
            if (data_va) {
                /* DATA export: point thunk to actual data in mapped memory.
                 * This allows the game to dereference the thunk entry. */
                BRIDGE_MEM32(va) = data_va;
                resolved++;
                bridged++;
                continue;
            }

            /* FUNCTION export: use synthetic VA for dispatch */
            g_slot_bridges[i] = bridge_for_ordinal(ordinal);
            { int _n = stdcall_args_for_ordinal(ordinal);
              g_slot_arg_unknown[i] = (uint8_t)(_n < 0);
              g_slot_arg_bytes[i] = (_n < 0) ? 0 : _n; }
            if (g_slot_bridges[i]) {
                bridged++;
            } else {
                unbridged++;
                fprintf(stderr, "  [KERNEL] unbridged function thunk: ordinal %lu"
                        " (slot %u, VA 0x%08X)\n",
                        (unsigned long)ordinal, i, KERNEL_VA_BASE + i * 4);
            }

            /* Replace Xbox memory entry with synthetic VA */
            uint32_t synthetic = KERNEL_VA_BASE + i * 4;
            BRIDGE_MEM32(va) = synthetic;
            resolved++;
        }
    }

    /*
     * Thunk entries below the header-declared base.
     *
     * KernelImageThunkAddress points at the main import run, but the linker can
     * emit further runs just before it, separated by a NULL. Halo has three at
     * base-0x10 (ordinals 52, 51 and 5). Those stay unpatched, so game code
     * doing "mov ebx,[thunk]; call ebx" jumps to the raw 0x8000xxxx marker
     * instead of a kernel function. The indirect call cannot resolve it, yields
     * 0, and a caller looping until it sees an error code never sees one - in
     * Halo that hung main() in a file-enumeration loop before it reached any
     * initialisation.
     *
     * Only entries still carrying the ordinal marker are touched, so scanning
     * back over unrelated .rdata is harmless.
     */
    {
        const int LOOKBEHIND = 16;   /* entries, i.e. 64 bytes */
        DWORD scan_protect;
        uint32_t low = g_thunk_table_base - LOOKBEHIND * 4;

        VirtualProtect((LPVOID)((uintptr_t)low + g_xbox_mem_offset),
                       LOOKBEHIND * 4, PAGE_READWRITE, &scan_protect);

        for (i = 1; i <= LOOKBEHIND; i++) {
            uint32_t va = g_thunk_table_base - i * 4;
            uint32_t current = BRIDGE_MEM32(va);
            int slot;

            if (!(current & 0x80000000)) {
                continue;            /* NULL separator or ordinary data */
            }
            slot = g_thunk_table_count + i;   /* park these above the main run */
            if (slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
                break;
            }

            g_slot_ordinals[slot] = current & 0x7FFFFFFF;
            g_slot_bridges[slot] = bridge_for_ordinal(g_slot_ordinals[slot]);
            { int _n = stdcall_args_for_ordinal(g_slot_ordinals[slot]);
              g_slot_arg_unknown[slot] = (uint8_t)(_n < 0);
              g_slot_arg_bytes[slot] = (_n < 0) ? 0 : _n; }
            BRIDGE_MEM32(va) = KERNEL_VA_BASE + slot * 4;
            resolved++;
            if (g_slot_bridges[slot]) bridged++; else unbridged++;

            fprintf(stderr, "  [KERNEL] extra thunk at 0x%08X: ordinal %u (slot %d)\n",
                    va, g_slot_ordinals[slot], slot);
        }

        VirtualProtect((LPVOID)((uintptr_t)low + g_xbox_mem_offset),
                       LOOKBEHIND * 4, scan_protect, &scan_protect);
    }

    /* Restore original protection */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        old_protect,
        &old_protect
    );

    fprintf(stderr, "  Kernel thunk bridge: %d/%d resolved (%d bridged, %d stub)\n",
            resolved, g_thunk_table_count, bridged, unbridged);
    fprintf(stderr, "  Synthetic VA range: 0x%08X-0x%08X\n",
            KERNEL_VA_BASE, KERNEL_VA_BASE + (resolved - 1) * 4);

}
