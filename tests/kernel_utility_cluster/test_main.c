/*
 * The utility drive reports the cluster size the title formatted it with.
 *
 * XAPI formats a title's cache partition (Harddisk0\Partition3-5, the Z:
 * drive) with the cluster size the XBE's init flags ask for: the top two
 * bits of the header word at 0x10124 scale 16 KB. XMountUtilityDrive then
 * opens the partition for a free-space query and requires
 * SectorsPerAllocationUnit * BytesPerSector to equal that size, failing with
 * STATUS_UNRECOGNIZED_VOLUME otherwise -- and XAPI boots to the dashboard
 * with an error before the title's main runs. Turok: Evolution (init flags
 * 0x80000007, 64 KB clusters) hit that against a fixed 16 KB.
 *
 * Covers: a cache partition follows the flags (16, 32, 64 KB), the data
 * partition keeps 16 KB whatever they say, and a closed handle does not leave
 * its cluster size to the next one.
 *
 * Cross-platform: Win32 backend on Windows and Proton, POSIX elsewhere.
 */
#include "kernel.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;

static void check(int ok, const char *what, unsigned got)
{
    printf("  %-62s %s", what, ok ? "PASS" : "FAIL");
    if (!ok) printf(" -- got 0x%08X", got);
    putchar('\n');
    if (!ok) failures++;
}

/* What xbox_MemoryLayoutInit hands the file layer from the XBE header. */
static void set_init_flags(uint32_t flags)
{
    xbox_file_set_init_flags(flags);
}

/* Open a partition the way XMountUtilityDrive does (FILE_DIRECTORY_FILE |
 * FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_FREE_SPACE_QUERY) and return
 * its cluster size in bytes, or 0 if the open or the query failed. */
static unsigned cluster_bytes(const char *device, HANDLE *keep)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_FILE_FS_SIZE_INFORMATION info;
    HANDLE h = NULL;
    NTSTATUS st;

    name.Length = (USHORT)strlen(device);
    name.MaximumLength = (USHORT)(name.Length + 1);
    name.Buffer = (PCHAR)device;
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = 0x40;
    st = xbox_NtCreateFile(&h, 0x80100000u /* GENERIC_READ|SYNCHRONIZE */, &oa, &ios,
                           NULL, 0, 3, XBOX_FILE_OPEN, 0x00800021u);
    if (st != 0 || !h)
        return 0;
    memset(&info, 0, sizeof info);
    st = xbox_NtQueryVolumeInformationFile(h, &ios, &info, sizeof info,
                                           XboxFileFsSizeInformation);
    if (keep)
        *keep = h;
    else
        xbox_NtClose(h);
    return st == 0 ? (unsigned)(info.BytesPerSector * info.SectorsPerAllocationUnit) : 0;
}

int main(int argc, char **argv)
{
    char root[512];
    HANDLE held = NULL;
    unsigned got;

    /* argv[1]: write the report there (Proton drops a console exe's stdout) */
    if (argc > 1 && !freopen(argv[1], "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        unsigned n = 0;
        GetTempPathA(MAX_PATH, tmp);
        do snprintf(root, sizeof root, "%sutilcluster-%lu-%u", tmp,
                    (unsigned long)GetCurrentProcessId(), n);
        while (!CreateDirectoryA(root, NULL) && ++n < 1000);
        if (n == 1000) return 2;
    }
#else
    {
        char tmpl[] = "/tmp/utilclusterXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(root, sizeof root, "%s", tmpl);
    }
#endif
    /* Creates the partition images in the save directory. */
    xbox_path_init(root, root);

    printf("before any XBE is loaded\n");
    got = cluster_bytes("\\Device\\Harddisk0\\Partition3\\", NULL);
    check(got == 0x4000, "no init flags yet: 16 KB", got);

    printf("cache partition follows the XBE's init flags\n");
    set_init_flags(0x00000007u);
    got = cluster_bytes("\\Device\\Harddisk0\\Partition5\\", NULL);
    check(got == 0x4000, "flags 0x00000007: 16 KB (unchanged)", got);
    set_init_flags(0x40000007u);
    got = cluster_bytes("\\Device\\Harddisk0\\Partition4\\", NULL);
    check(got == 0x8000, "flags 0x40000007: 32 KB", got);
    set_init_flags(0x80000007u);
    got = cluster_bytes("\\Device\\Harddisk0\\Partition3\\", NULL);
    check(got == 0x10000, "flags 0x80000007: 64 KB (Turok: Evolution)", got);
    got = cluster_bytes("\\Device\\Harddisk0\\Partition5\\", &held);
    check(got == 0x10000, "flags 0x80000007, Partition5: 64 KB", got);

    printf("the data partition keeps FATX's 16 KB\n");
    got = cluster_bytes("\\Device\\Harddisk0\\Partition1\\", NULL);
    check(got == 0x4000, "Partition1 with flags 0x80000007: 16 KB", got);

    printf("a closed handle forgets its partition\n");
    if (held) xbox_NtClose(held);
    got = cluster_bytes("\\Device\\Harddisk0\\Partition1\\", NULL);
    check(got == 0x4000, "Partition1 after a cache handle closed: 16 KB", got);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
