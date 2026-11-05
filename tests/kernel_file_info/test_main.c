/*
 * The info classes XAPI's GetFileInformationByHandle asks for, on every host.
 *
 * It queries FileFsVolumeInformation (1) for the serial, then
 * FileInternalInformation (6) for the file index, then
 * FileNetworkOpenInformation (0x22) for the size and times, and fails as a
 * whole when any of them does. Both disk backends refused the first two with
 * STATUS_NOT_IMPLEMENTED; Kingdom Under Fire: Heroes ignored the failure, took
 * stack garbage for a file size and stopped loading install_image.dds.
 *
 * Covered: the file index is answered, differs between files and (POSIX) is
 * the inode; the volume reply has the NT layout with the label at offset 17;
 * a buffer short of either structure is refused with
 * STATUS_INFO_LENGTH_MISMATCH; and a label that does not fit is cut short
 * with STATUS_BUFFER_OVERFLOW, as NT does.
 *
 * Cross-platform: Win32 backend on Windows and Proton, POSIX elsewhere.
 */
#include "kernel.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#include <sys/stat.h>
#endif

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

#define ST_INFO_LENGTH  0xC0000004u
#define ST_BUF_OVERFLOW 0x80000005u

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-62s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static NTSTATUS open_read(const char *path, HANDLE *h)
{
    XBOX_ANSI_STRING s;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    s.Length = (USHORT)strlen(path);
    s.MaximumLength = (USHORT)(s.Length + 1);
    s.Buffer = (PCHAR)path;
    oa.RootDirectory = NULL;
    oa.ObjectName = &s;
    oa.Attributes = 0x40;
    *h = NULL;
    return xbox_NtCreateFile(h, 0x80100080u /* GENERIC_READ|SYNC|READ_ATTR */, &oa, &ios,
                             NULL, 0, 1 /* share read */, XBOX_FILE_OPEN, 0x60);
}

static void host_write(const char *root, const char *name)
{
    char p[700];
    FILE *f;
#if defined(_WIN32)
    snprintf(p, sizeof p, "%s\\%s", root, name);
#else
    snprintf(p, sizeof p, "%s/%s", root, name);
#endif
    f = fopen(p, "wb");
    if (!f) { printf("cannot write %s\n", p); exit(2); }
    fputs("info classes", f);
    fclose(f);
}

int main(int argc, char **argv)
{
    char root[512], d[200];
    XBOX_FILE_INTERNAL_INFORMATION ia, ib;
    XBOX_IO_STATUS_BLOCK ios;
    unsigned char buf[64];
    PXBOX_FILE_FS_VOLUME_INFORMATION vol = (PXBOX_FILE_FS_VOLUME_INFORMATION)buf;
    XBOX_FILE_NETWORK_OPEN_INFORMATION net;
    HANDLE a, b;
    NTSTATUS st, sa, sb;

    /* argv[1]: write the report there (Proton drops a console exe's stdout) */
    if (argc > 1 && !freopen(argv[1], "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        unsigned n = 0;
        GetTempPathA(MAX_PATH, tmp);
        /* Wine reuses process ids from one run to the next. */
        do snprintf(root, sizeof root, "%sfileinfo-%lu-%u", tmp, (unsigned long)GetCurrentProcessId(), n);
        while (!CreateDirectoryA(root, NULL) && ++n < 1000);
        if (n == 1000) return 2;
    }
#else
    {
        char tmpl[] = "/tmp/fileinfoXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(root, sizeof root, "%s", tmpl);
    }
#endif
    host_write(root, "a.bin");
    host_write(root, "b.bin");
    xbox_path_init(root, root);
    sa = open_read("D:\\a.bin", &a);
    sb = open_read("D:\\b.bin", &b);
    if (sa || sb) {
        printf("open failed: 0x%08X 0x%08X\n", (unsigned)sa, (unsigned)sb);
        return 2;
    }

    printf("layout\n");
    check(offsetof(XBOX_FILE_FS_VOLUME_INFORMATION, VolumeLabel) == 17 &&
          sizeof(XBOX_FILE_FS_VOLUME_INFORMATION) == 24 &&
          sizeof(XBOX_FILE_INTERNAL_INFORMATION) == 8,
          "volume label at 17, structure 24 bytes; file index 8", NULL);

    printf("FileInternalInformation\n");
    ios.Information = 0;
    sa = xbox_NtQueryInformationFile(a, &ios, &ia, sizeof ia, XboxFileInternalInformation);
    snprintf(d, sizeof d, "status 0x%08X, info %u", (unsigned)sa, (unsigned)ios.Information);
    check(sa == 0 && ios.Status == 0 && ios.Information == 8, "answered, 8 bytes", d);
    sb = xbox_NtQueryInformationFile(b, &ios, &ib, sizeof ib, XboxFileInternalInformation);
    snprintf(d, sizeof d, "index %lld and %lld", (long long)ia.IndexNumber.QuadPart,
             (long long)ib.IndexNumber.QuadPart);
    check(sb == 0 && ia.IndexNumber.QuadPart != ib.IndexNumber.QuadPart,
          "two files, two indices", d);
#if !defined(_WIN32)
    {
        char p[700];
        struct stat s;
        snprintf(p, sizeof p, "%s/a.bin", root);
        stat(p, &s);
        check(ia.IndexNumber.QuadPart == (LONGLONG)s.st_ino, "the index is the inode", d);
    }
#endif
    ios.Status = 0x7EADBEEF;
    ios.Information = 99;
    st = xbox_NtQueryInformationFile(a, &ios, &ia, 4, XboxFileInternalInformation);
    snprintf(d, sizeof d, "status 0x%08X, ios 0x%08X/%u", (unsigned)st,
             (unsigned)ios.Status, (unsigned)ios.Information);
    check((uint32_t)st == ST_INFO_LENGTH && (uint32_t)ios.Status == ST_INFO_LENGTH &&
          ios.Information == 0, "4-byte buffer: INFO_LENGTH_MISMATCH, in the status block too", d);

    printf("FileFsVolumeInformation\n");
    memset(buf, 0xCC, sizeof buf);
    ios.Information = 0;
    st = xbox_NtQueryVolumeInformationFile(a, &ios, buf, 24, XboxFileFsVolumeInformation);
    snprintf(d, sizeof d, "status 0x%08X, info %u, label %u, objects %u", (unsigned)st,
             (unsigned)ios.Information, (unsigned)vol->VolumeLabelLength,
             (unsigned)vol->SupportsObjects);
    check(st == 0 && ios.Status == 0 && ios.Information == 17 &&
          vol->VolumeLabelLength == 0 && vol->SupportsObjects == 0 &&
          vol->VolumeCreationTime.QuadPart == 0,
          "answered: no label, 17 bytes", d);
    check(buf[17] == 0xCC && buf[24] == 0xCC, "nothing written past the reply", d);
#if !defined(_WIN32)
    {
        struct stat s;
        char p[700];
        snprintf(p, sizeof p, "%s/a.bin", root);
        stat(p, &s);
        snprintf(d, sizeof d, "serial 0x%08X, st_dev 0x%08X",
                 (unsigned)vol->VolumeSerialNumber, (unsigned)s.st_dev);
        check(vol->VolumeSerialNumber == (ULONG)s.st_dev, "the serial is the device number", d);
    }
#endif
    ios.Status = 0x7EADBEEF;
    ios.Information = 99;
    st = xbox_NtQueryVolumeInformationFile(a, &ios, buf, 16, XboxFileFsVolumeInformation);
    snprintf(d, sizeof d, "status 0x%08X, ios 0x%08X/%u", (unsigned)st,
             (unsigned)ios.Status, (unsigned)ios.Information);
    check((uint32_t)st == ST_INFO_LENGTH && (uint32_t)ios.Status == ST_INFO_LENGTH &&
          ios.Information == 0, "16-byte buffer: INFO_LENGTH_MISMATCH, in the status block too", d);

    printf("a label that does not fit\n");
    memset(buf, 0xCC, sizeof buf);
    st = xbox_FsVolumeInformation(&ios, buf, 24, 0x1234u, "XBOXLABEL1");
    snprintf(d, sizeof d, "status 0x%08X, info %u, label %u", (unsigned)st,
             (unsigned)ios.Information, (unsigned)vol->VolumeLabelLength);
    check((uint32_t)st == ST_BUF_OVERFLOW && ios.Status == st && ios.Information == 24 &&
          vol->VolumeLabelLength == 10 && !memcmp(vol->VolumeLabel, "XBOXLAB", 7) &&
          buf[24] == 0xCC, "BUFFER_OVERFLOW, the full length, 7 bytes of it", d);
    st = xbox_FsVolumeInformation(&ios, buf, sizeof buf, 0x1234u, "XBOXLABEL1");
    snprintf(d, sizeof d, "status 0x%08X, info %u", (unsigned)st, (unsigned)ios.Information);
    check(st == 0 && ios.Information == 27 && !memcmp(vol->VolumeLabel, "XBOXLABEL1", 10) &&
          vol->VolumeSerialNumber == 0x1234u, "room for it: success, 27 bytes", d);

    printf("FileNetworkOpenInformation (unchanged)\n");
    st = xbox_NtQueryInformationFile(a, &ios, &net, sizeof net, XboxFileNetworkOpenInformation);
    snprintf(d, sizeof d, "status 0x%08X, size %lld", (unsigned)st, (long long)net.EndOfFile.QuadPart);
    check(st == 0 && net.EndOfFile.QuadPart == 12, "answered, the real size", d);

    xbox_NtClose(a);
    xbox_NtClose(b);
    {
        char cmd[700];
#if defined(_WIN32)
        snprintf(cmd, sizeof cmd, "cmd /c rmdir /s /q \"%s\"", root);
#else
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
#endif
        (void)system(cmd);
    }
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
