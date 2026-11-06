#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *recomp_lookup(ULONG a) { (void)a; abort(); }
void *recomp_lookup_manual(ULONG a) { (void)a; abort(); }

/* Opens `xbox_path` through xbox_NtCreateFile and returns the NTSTATUS. */
static NTSTATUS open_path(const char *xbox_path, ULONG options)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES attrs;
    XBOX_IO_STATUS_BLOCK ios;
    HANDLE h = NULL;
    NTSTATUS st;
    name.Length = name.MaximumLength = (USHORT)strlen(xbox_path);
    name.Buffer = (PCHAR)xbox_path;
    attrs.RootDirectory = NULL;
    attrs.ObjectName = &name;
    attrs.Attributes = 0;
    st = xbox_NtCreateFile(&h, GENERIC_READ, &attrs, &ios, NULL, 0,
                           1, XBOX_FILE_OPEN, options);
    if (NT_SUCCESS(st)) xbox_NtClose(h);
    return st;
}

int main(void)
{
    char root[MAX_PATH], dir[MAX_PATH], sub[MAX_PATH];
    int ok = 1;
    GetTempPathA(MAX_PATH, root);
    sprintf(dir, "%sxrecomp-bare-cdrom0-%lu", root, GetCurrentProcessId());
    sprintf(sub, "%s\\SaveSlot", dir);
    if (!CreateDirectoryA(dir, NULL) || !CreateDirectoryA(sub, NULL)) return 10;
    xbox_path_init(dir, NULL);

    /* The bare device is the game directory, as the bare partitions are. */
    if (open_path("\\Device\\CdRom0", XBOX_FILE_DIRECTORY_FILE) != 0) { puts("FAIL bare CdRom0"); ok = 0; }

    RemoveDirectoryA(sub); RemoveDirectoryA(dir);
    if (!ok) return 1;
    puts("PASS: bare CdRom0 device opens as the game directory");
    return 0;
}
