/*
 * A read-only open that asks for no intermediate buffering goes through the
 * host cache; an open that can write keeps no buffering.
 *
 * The host's reads are synchronous, so an uncached read waits out every disk
 * stall on the title's thread. The handle's mode (NtQueryInformationFile,
 * FileModeInformation) says whether the host opened it unbuffered.
 *
 * Windows only: the POSIX backend has no unbuffered opens.
 */
#include "kernel.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

#define FILE_MODE_INFORMATION_CLASS 16
#define MODE_NO_INTERMEDIATE_BUFFERING 0x00000008u
typedef LONG (NTAPI *query_info_fn)(HANDLE, void *, void *, ULONG, int);

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-60s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* The handle's mode bits, or ~0 if they cannot be read. */
static ULONG host_mode(HANDLE h)
{
    static query_info_fn query;
    ULONG_PTR iosb[2];
    ULONG mode = ~0u;

    if (!query)
        query = (query_info_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                              "NtQueryInformationFile");
    if (!query || query(h, iosb, &mode, sizeof mode, FILE_MODE_INFORMATION_CLASS) < 0)
        return ~0u;
    return mode;
}

static HANDLE open_unbuffered(const char *path, uint32_t access)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    HANDLE h = NULL;
    NTSTATUS st;

    name.Length = (USHORT)strlen(path);
    name.MaximumLength = (USHORT)(name.Length + 1);
    name.Buffer = (PCHAR)path;
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = 0x40;
    st = xbox_NtCreateFile(&h, access, &oa, &ios, NULL, 0, 1 /* share read */,
                           XBOX_FILE_OPEN,
                           0x60 | XBOX_FILE_NO_INTERMEDIATE_BUFFERING);
    return st == 0 ? h : NULL;
}

int main(void)
{
    char tmp[MAX_PATH], root[MAX_PATH + 64], file[MAX_PATH + 96];
    unsigned n = 0;
    HANDLE f, h;
    ULONG mode;

    setvbuf(stdout, NULL, _IONBF, 0);
    GetTempPathA(MAX_PATH, tmp);
    do snprintf(root, sizeof root, "%sfilecache-%lu-%u", tmp,
                (unsigned long)GetCurrentProcessId(), n);
    while (!CreateDirectoryA(root, NULL) && ++n < 1000);
    if (n == 1000) return 2;
    snprintf(file, sizeof file, "%s\\stream.bin", root);
    f = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 2;
    {
        static char block[4096];
        DWORD wrote;
        WriteFile(f, block, sizeof block, &wrote, NULL);
        CloseHandle(f);
    }
    xbox_path_init(root, root);

    printf("no intermediate buffering\n");
    h = open_unbuffered("D:\\stream.bin", 0x80100080u /* GENERIC_READ|SYNC|READ_ATTR */);
    mode = h ? host_mode(h) : ~0u;
    check(mode != ~0u && !(mode & MODE_NO_INTERMEDIATE_BUFFERING),
          "read-only open: host cache");
    if (h) CloseHandle(h);

    h = open_unbuffered("D:\\stream.bin", 0xC0100080u /* + GENERIC_WRITE */);
    mode = h ? host_mode(h) : ~0u;
    check(mode != ~0u && (mode & MODE_NO_INTERMEDIATE_BUFFERING),
          "read-write open: no buffering, as asked");
    if (h) CloseHandle(h);

    DeleteFileA(file);
    RemoveDirectoryA(root);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
