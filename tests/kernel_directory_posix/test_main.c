/* A game-free test of the actual POSIX file/path/handle implementations.
 * Explicit checks execute in Release, including when NDEBUG is defined. */
#define _GNU_SOURCE
#include "kernel.h"
#include "guest_det_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

/* Logging is orthogonal to file/search behavior; no title or runtime init. */
void xbox_log(int level, const char *subsystem, const char *fmt, ...)
{ (void)level; (void)subsystem; (void)fmt; }

/* This fixture creates no symbolic links; normal device-path translation is real. */
const char *xbox_LookupSymbolicLink(const char *link)
{ (void)link; return NULL; }

/* The deterministic scheduler is not linked; the size test flips the mode. */
static int det_on;
int xbox_GuestDetOn(void) { return det_on; }

static int failures;
static void check(int ok, const char *name)
{
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}
static NTSTATUS open_guest(const char *path, ULONG options, HANDLE *h)
{
    XBOX_ANSI_STRING name = {(USHORT)strlen(path), (USHORT)(strlen(path)+1), (PCHAR)path};
    XBOX_OBJECT_ATTRIBUTES oa = {0};
    XBOX_IO_STATUS_BLOCK ios = {0};
    oa.ObjectName = &name; oa.Attributes = 0x40; *h = NULL;
    return xbox_NtCreateFile(h, 0x80000000u, &oa, &ios, NULL, 0x80, 0,
                             XBOX_FILE_OPEN, options);
}
static int query(HANDLE h, int directory, int restart)
{
    union { uint64_t alignment; unsigned char bytes[512]; } buf = {0};
    XBOX_IO_STATUS_BLOCK ios = {0};
    XBOX_ANSI_STRING mask = {5, 6, "*.sav"};
    NTSTATUS st = xbox_NtQueryDirectoryFile(h, NULL, NULL, NULL, &ios,
        buf.bytes, sizeof buf.bytes, XboxFileDirectoryInformation, &mask, restart);
    PXBOX_FILE_DIRECTORY_INFORMATION entry = (void *)buf.bytes;
    char expected[16]; snprintf(expected, sizeof expected, "dir%d-", directory);
    return st == STATUS_SUCCESS && entry->FileNameLength == strlen(expected)+5 &&
        ios.Information >= entry->FileNameLength &&
        memcmp(entry->FileName, expected, strlen(expected)) == 0 &&
        memcmp(entry->FileName+entry->FileNameLength-4, ".sav", 4) == 0;
}
static int descriptor_count(void)
{
    int n = 0;
    for (int fd=0;fd<4096;fd++) if (fcntl(fd, F_GETFD) != -1) n++;
    return n;
}
static void lifetime(void)
{
    int fresh=0, reuse=0, completed=0; uintptr_t previous=0;
    int before = descriptor_count();
    for (int i=0;i<300;i++) {
        char path[32]; HANDLE h=NULL;
        snprintf(path,sizeof path,"z:\\dir%d",i%8);
        NTSTATUS st=open_guest(path,XBOX_FILE_DIRECTORY_FILE,&h);
        if (st != STATUS_SUCCESS || !h) { failures++; break; }
        reuse += previous == (uintptr_t)h; previous=(uintptr_t)h;
        fresh += query(h,i%8,0); /* do not use RestartScan to mask stale state */
        if (xbox_NtClose(h) != STATUS_SUCCESS) failures++;
        completed++;
    }
    printf("abandoned searches: fresh=%d/300 completed=%d reused=%d\n",fresh,completed,reuse);
    check(fresh==300 && completed==300,"300 partial searches start in the requested directory");
    int after=descriptor_count();
    printf("open descriptors: before=%d after=%d\n",before,after);
    check(before==after,"partial-search close releases directory descriptors");
}
static void independent_searches(void)
{
    HANDLE a=NULL,b=NULL;int ok=1;
    ok &= open_guest("z:\\dir0",XBOX_FILE_DIRECTORY_FILE,&a)==STATUS_SUCCESS;
    ok &= open_guest("z:\\dir1",XBOX_FILE_DIRECTORY_FILE,&b)==STATUS_SUCCESS;
    if (a && b) {
        ok &= query(a,0,0) && query(b,1,0);
        ok &= xbox_NtClose(a)==STATUS_SUCCESS; a=NULL;
        ok &= query(b,1,0); /* closing a must not drop b's enumeration */
        ok &= query(b,1,1); /* restart still works */
    } else ok=0;
    if(a)xbox_NtClose(a);if(b)xbox_NtClose(b);
    check(ok,"closing one search preserves another live search and RestartScan");
    HANDLE h=NULL;
    ok=open_guest("z:\\dir2",XBOX_FILE_DIRECTORY_FILE,&h)==STATUS_SUCCESS;
    if(h)ok &= xbox_NtClose(h)==STATUS_SUCCESS;else ok=0;
    check(ok,"directory close without enumeration");
    h=NULL;ok=open_guest("z:\\dir0\\dir0-a.sav",0,&h)==STATUS_SUCCESS;
    if(h)ok &= xbox_NtClose(h)==STATUS_SUCCESS;else ok=0;
    check(ok,"ordinary file close");
    check(xbox_NtClose(NULL)==STATUS_INVALID_HANDLE,"null close retains invalid-handle status");
}
/* FileFsSizeInformation on a host-backed drive (the game's D:, not the RAM
 * cache's Z:): the host disk's free space normally, and in the deterministic
 * mode a fixed volume, since the host's moves between runs. */
static void volume_size(void)
{
    HANDLE h=NULL; XBOX_IO_STATUS_BLOCK ios={0}; XBOX_FILE_FS_SIZE_INFORMATION fs;
    int ok=open_guest("d:\\",XBOX_FILE_DIRECTORY_FILE,&h)==STATUS_SUCCESS && h;
    memset(&fs,0,sizeof fs);
    ok = ok && xbox_NtQueryVolumeInformationFile(h,&ios,&fs,sizeof fs,XboxFileFsSizeInformation)==STATUS_SUCCESS;
    check(ok && fs.BytesPerSector*fs.SectorsPerAllocationUnit==16384 &&
          fs.TotalAllocationUnits.QuadPart>0,"volume size from the host disk");
    det_on=1; memset(&fs,0,sizeof fs);
    ok = ok && xbox_NtQueryVolumeInformationFile(h,&ios,&fs,sizeof fs,XboxFileFsSizeInformation)==STATUS_SUCCESS;
    det_on=0;
    check(ok && fs.TotalAllocationUnits.QuadPart==1048576 &&
          fs.AvailableAllocationUnits.QuadPart==524288 &&
          fs.BytesPerSector*fs.SectorsPerAllocationUnit==16384,
          "deterministic mode reports a fixed volume");
    if (h) xbox_NtClose(h);
}
/* File times on the host-backed drive: the host's normally, and in the
 * deterministic mode the fixed 2004-01-01, since a file the run creates
 * carries the wall clock. */
static void file_times(void)
{
    HANDLE h=NULL; XBOX_IO_STATUS_BLOCK ios={0}; XBOX_FILE_NETWORK_OPEN_INFORMATION ni;
    int ok=open_guest("d:\\",XBOX_FILE_DIRECTORY_FILE,&h)==STATUS_SUCCESS && h;
    memset(&ni,0,sizeof ni);
    ok = ok && xbox_NtQueryInformationFile(h,&ios,&ni,sizeof ni,XboxFileNetworkOpenInformation)==STATUS_SUCCESS;
    check(ok && ni.LastWriteTime.QuadPart>XBOX_DET_FILETIME,"file times from the host");
    det_on=1; memset(&ni,0,sizeof ni);
    ok = ok && xbox_NtQueryInformationFile(h,&ios,&ni,sizeof ni,XboxFileNetworkOpenInformation)==STATUS_SUCCESS;
    det_on=0;
    check(ok && ni.CreationTime.QuadPart==XBOX_DET_FILETIME &&
          ni.LastAccessTime.QuadPart==XBOX_DET_FILETIME &&
          ni.LastWriteTime.QuadPart==XBOX_DET_FILETIME &&
          ni.ChangeTime.QuadPart==XBOX_DET_FILETIME,
          "deterministic mode reports fixed file times");
    if (h) xbox_NtClose(h);
}
int main(void)
{
    char root[]="/tmp/xbox-dir-search-XXXXXX",path[512];
    if(!mkdtemp(root))return 2;
    snprintf(path,sizeof path,"%s/Cache",root);if(mkdir(path,0700))return 2;
    for(int i=0;i<8;i++) {
        snprintf(path,sizeof path,"%s/Cache/dir%d",root,i);if(mkdir(path,0700))return 2;
        for(int j=0;j<4;j++) {
            snprintf(path,sizeof path,"%s/Cache/dir%d/dir%d-%c.%s",root,i,i,'a'+j,j<3?"sav":"txt");
            FILE *f=fopen(path,"wb");if(!f)return 2;fclose(f);
        }
    }
    xbox_path_init(root,root);
    lifetime();independent_searches();volume_size();file_times();
    for(int i=0;i<8;i++) {
        for(int j=0;j<4;j++) {
            snprintf(path,sizeof path,"%s/Cache/dir%d/dir%d-%c.%s",root,i,i,'a'+j,j<3?"sav":"txt");unlink(path);
        }
        snprintf(path,sizeof path,"%s/Cache/dir%d",root,i);rmdir(path);
    }
    snprintf(path,sizeof path,"%s/Cache",root);rmdir(path);rmdir(root);
    return failures?1:0;
}
