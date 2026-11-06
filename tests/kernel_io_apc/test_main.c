/*
 * File I/O APCs: where they run, and what they leave on the guest stack.
 *
 * ReadFileEx hands NtReadFile an APC routine. On hardware it runs in the
 * thread's next alertable wait, which then returns STATUS_USER_APC. The
 * runtime runs it inline by default; xbox_SetDeferredIoApc(1) gives the
 * hardware behaviour, which Fallout: Brotherhood of Steel's streamer needs.
 *
 * Both paths are driven the way a title drives them: through the thunk
 * table, with a buffer standing in for guest memory, as tests/kernel_bridge
 * does. The APC routine is a C function this test hands back from
 * recomp_lookup for a made-up guest VA.
 *
 * Checked:
 *   - inline: the routine runs during NtReadFile, and esp comes back exactly
 *     past NtReadFile's arguments. deliver_one_apc used to add 12 after a
 *     stdcall routine had popped its own 16 bytes.
 *   - deferred: the routine has not run when NtReadFile returns; an alertable
 *     user-mode NtWaitForSingleObjectEx runs it and returns STATUS_USER_APC;
 *     esp is exact after both calls.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

#define APC_VA       0x00005000u   /* "guest" address of the APC routine */
#define APC_CONTEXT  0x0000C0DEu

static uint8_t *mem;
static int apc_runs;
static uint32_t apc_seen_context;

/* VOID NTAPI ApcRoutine(PVOID ApcContext, PIO_STATUS_BLOCK, ULONG):
 * stdcall, so it pops its return address and three arguments. */
static void test_apc(void)
{
    apc_seen_context = *(uint32_t *)(mem + g_esp + 4);
    apc_runs++;
    g_esp += 16;
}

recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va)
{
    return xbox_va == APC_VA ? test_apc : NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-62s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

/* Guest layout. */
enum {
    THUNK_VA = 0x10000,   /* NtOpenFile, NtReadFile, NtWaitForSingleObjectEx */
    STACK_VA = 0x20000,
    NAME_VA  = 0x30000,   /* path characters */
    ANSI_VA  = 0x30100,   /* ANSI_STRING */
    OA_VA    = 0x30200,   /* OBJECT_ATTRIBUTES */
    IOSB_VA  = 0x30300,
    HANDLE_VA = 0x30400,
    BUF_VA   = 0x31000,
    TIMEOUT_VA = 0x32000,
};

static void call(int slot, const uint32_t *args, int n)
{
    uint32_t *sp = (uint32_t *)(mem + STACK_VA);
    int i;

    sp[0] = 0xBEEF0000u | (uint32_t)slot;   /* guest return address */
    for (i = 0; i < n; i++)
        sp[1 + i] = args[i];
    g_esp = STACK_VA;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 4 * slot))();
}

static uint32_t open_file(void)
{
    const char *path = "\\Device\\CdRom0\\apc.bin";
    uint32_t args[6] = { HANDLE_VA, 0x80100000u /* GENERIC_READ|SYNCHRONIZE */,
                         OA_VA, IOSB_VA, 1 /* FILE_SHARE_READ */,
                         0x20 /* FILE_SYNCHRONOUS_IO_NONALERT */ };

    strcpy((char *)mem + NAME_VA, path);
    *(uint16_t *)(mem + ANSI_VA)     = (uint16_t)strlen(path);
    *(uint16_t *)(mem + ANSI_VA + 2) = (uint16_t)strlen(path) + 1;
    *(uint32_t *)(mem + ANSI_VA + 4) = NAME_VA;
    *(uint32_t *)(mem + OA_VA)     = 0;         /* RootDirectory */
    *(uint32_t *)(mem + OA_VA + 4) = ANSI_VA;   /* ObjectName */
    *(uint32_t *)(mem + OA_VA + 8) = 0x40;      /* OBJ_CASE_INSENSITIVE */
    call(0, args, 6);
    return g_eax == 0 ? *(uint32_t *)(mem + HANDLE_VA) : 0;
}

static void read_with_apc(uint32_t handle)
{
    uint32_t args[8] = { handle, 0, APC_VA, APC_CONTEXT, IOSB_VA,
                         BUF_VA, 4, 0 };
    call(1, args, 8);
}

int main(void)
{
    char dir[MAX_PATH], file[MAX_PATH], detail[160];
    FILE *f;
    uint32_t h;

    setvbuf(stdout, NULL, _IONBF, 0);

    mem = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT,
                       PAGE_READWRITE);
    if (!mem) {
        printf("guest memory stand-in not allocated\n");
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)mem;

    /* A host directory standing in for the disc, holding one file. */
    GetTempPathA(sizeof dir, dir);
    strcat(dir, "xr_io_apc");
    CreateDirectoryA(dir, NULL);
    /* A forward slash, which Win32 takes as readily as a backslash. fopen is
     * the C library, not the shim, so nothing normalises a separator for it:
     * with a backslash the file landed beside the directory as
     * "xr_io_apc\\apc.bin" rather than in it, and the open under test then
     * correctly found nothing. One spelling for every caller here. */
    snprintf(file, sizeof file, "%s/apc.bin", dir);
    f = fopen(file, "wb");
    if (!f) {
        printf("cannot create %s\n", file);
        return 1;
    }
    fwrite("APC!", 1, 4, f);
    fclose(f);

    *(uint32_t *)(mem + THUNK_VA)     = 0x80000000u | 202u;  /* NtOpenFile */
    *(uint32_t *)(mem + THUNK_VA + 4) = 0x80000000u | 219u;  /* NtReadFile */
    *(uint32_t *)(mem + THUNK_VA + 8) = 0x80000000u | 234u;  /* NtWaitForSingleObjectEx */
    xbox_kernel_set_thunk_address(THUNK_VA, 3);
    xbox_kernel_init();
    xbox_path_init(dir, dir);
    xbox_kernel_bridge_init();

    h = open_file();
    snprintf(detail, sizeof detail, "status 0x%08X", g_eax);
    check(h != 0, "NtOpenFile opens the stand-in disc file", detail);
    if (!h)
        goto done;

    /* Inline (the default). */
    read_with_apc(h);
    check(g_eax == 0 && memcmp(mem + BUF_VA, "APC!", 4) == 0,
          "inline: NtReadFile reads the file", NULL);
    check(apc_runs == 1 && apc_seen_context == APC_CONTEXT,
          "inline: the APC runs during NtReadFile, with its context", NULL);
    snprintf(detail, sizeof detail, "esp 0x%08X, expected 0x%08X",
             g_esp, STACK_VA + 4 + 32);
    check(g_esp == STACK_VA + 4 + 32,
          "inline: esp is exactly past NtReadFile's arguments", detail);

    /* Deferred. */
    xbox_SetDeferredIoApc(1);
    apc_runs = 0;
    read_with_apc(h);
    check(apc_runs == 0, "deferred: the APC has not run when NtReadFile returns",
          NULL);
    snprintf(detail, sizeof detail, "esp 0x%08X, expected 0x%08X",
             g_esp, STACK_VA + 4 + 32);
    check(g_esp == STACK_VA + 4 + 32,
          "deferred: esp is exactly past NtReadFile's arguments", detail);
    {
        /* Zero timeout, so a regression returns instead of hanging. */
        uint32_t args[4] = { h, 1 /* UserMode */, 1 /* Alertable */,
                             TIMEOUT_VA };
        memset(mem + TIMEOUT_VA, 0, 8);
        call(2, args, 4);
    }
    snprintf(detail, sizeof detail, "status 0x%08X, runs %d", g_eax, apc_runs);
    check(g_eax == 0xC0u && apc_runs == 1,
          "deferred: the alertable wait runs it and returns STATUS_USER_APC",
          detail);
    snprintf(detail, sizeof detail, "esp 0x%08X, expected 0x%08X",
             g_esp, STACK_VA + 4 + 16);
    check(g_esp == STACK_VA + 4 + 16,
          "deferred: esp is exactly past the wait's arguments", detail);

done:
    DeleteFileA(file);
    RemoveDirectoryA(dir);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
