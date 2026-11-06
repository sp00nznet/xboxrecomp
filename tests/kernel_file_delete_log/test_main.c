/*
 * Every file delete a title asks for is in the log.
 *
 * A title may delete a file it judges invalid, and a delete is the one file
 * operation that loses data, so both ways to ask for one write a
 * "[FILE] delete <path>" line at the default log level: NtDeleteFile, and
 * NtSetInformationFile(FileDispositionInformation) with DeleteFile set.
 * A disposition that clears the flag deletes nothing and logs nothing.
 *
 * Cross-platform: it calls the xbox_Nt* file functions directly, which are the
 * Win32 backend on Windows and the POSIX one elsewhere.
 */
#include "kernel.h"

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

#if defined(_WIN32)
#define SEP "\\"
#else
#define SEP "/"
#endif

static int failures;
static char g_cache[600];

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int exists(const char *name)
{
    char p[700];
    FILE *f;
    snprintf(p, sizeof p, "%s" SEP "%s", g_cache, name);
    f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void make(const char *name)
{
    char p[700];
    FILE *f;
    snprintf(p, sizeof p, "%s" SEP "%s", g_cache, name);
    f = fopen(p, "wb");
    if (f) { fputs("save", f); fclose(f); }
}

static void attrs(XBOX_ANSI_STRING *name, XBOX_OBJECT_ATTRIBUTES *oa, const char *path)
{
    name->Length = (USHORT)strlen(path);
    name->MaximumLength = (USHORT)(name->Length + 1);
    name->Buffer = (PCHAR)path;
    oa->RootDirectory = NULL;
    oa->ObjectName = name;
    oa->Attributes = 0x40;
}

static NTSTATUS delete_path(const char *path)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    attrs(&name, &oa, path);
    return xbox_NtDeleteFile(&oa);
}

static NTSTATUS open_for_delete(const char *path, HANDLE *out)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    attrs(&name, &oa, path);
    *out = NULL;
    return xbox_NtCreateFile(out, 0xC0110000u /* R|W|DELETE|SYNC */, &oa, &ios,
                             NULL, 0x80, 0x7 /* share all */, XBOX_FILE_OPEN, 0x20);
}

static NTSTATUS set_disposition(HANDLE h, BOOLEAN del)
{
    XBOX_FILE_DISPOSITION_INFORMATION info;
    XBOX_IO_STATUS_BLOCK ios;
    info.DeleteFile = del;
    return xbox_NtSetInformationFile(h, &ios, &info, sizeof info,
                                     XboxFileDispositionInformation);
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    for (; (hay = strstr(hay, needle)) != NULL; hay += strlen(needle))
        n++;
    return n;
}

/* Is there a delete line (not a path-translation trace) that names `name`? */
static int has_delete_line(const char *log, const char *name)
{
    const char *l = log;
    while (*l) {
        const char *e = strchr(l, 10);
        size_t len = e ? (size_t)(e - l) : strlen(l);
        char line[1024];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, l, len);
        line[len] = 0;
        if (strstr(line, "[FILE  ] delete ") && strstr(line, name))
            return 1;
        l += len + (e ? 1 : 0);
    }
    return 0;
}

int main(void)
{
    const char *logpath = "delete_log_test.stderr";
    char root[512], buf[8192];
    size_t got;
    FILE *f;
    HANDLE h;

#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        snprintf(root, sizeof root, "%sdellog-%lu", tmp, (unsigned long)GetCurrentProcessId());
        if (!CreateDirectoryA(root, NULL)) return 2;
        snprintf(g_cache, sizeof g_cache, "%s\\Cache", root);
        CreateDirectoryA(g_cache, NULL);
    }
#else
    {
        char tmpl[] = "/tmp/dellogXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(root, sizeof root, "%s", tmpl);
        snprintf(g_cache, sizeof g_cache, "%s/Cache", root);
        mkdir(g_cache, 0755);
    }
#endif
    xbox_path_init(root, root);

    /* The log goes to stderr; read it back from a file. */
    if (!freopen(logpath, "w", stderr)) return 2;

    make("by_path.dat");
    make("by_handle.dat");
    make("kept.dat");

    check(exists("by_path.dat"), "the files exist");
    check(delete_path("z:\\by_path.dat") == STATUS_SUCCESS, "NtDeleteFile succeeds");
    check(!exists("by_path.dat"), "NtDeleteFile removed the file");

    check(open_for_delete("z:\\by_handle.dat", &h) == STATUS_SUCCESS && h, "open with DELETE access");
    check(set_disposition(h, FALSE) == STATUS_SUCCESS, "disposition with DeleteFile clear");
    check(set_disposition(h, TRUE) == STATUS_SUCCESS, "disposition with DeleteFile set");
    xbox_NtClose(h);
    check(!exists("by_handle.dat"), "the disposition removed the file on close");

    check(exists("kept.dat"), "a file nobody deleted is still there");

    fflush(stderr);
    f = fopen(logpath, "r");
    got = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[got] = 0;
    if (f) fclose(f);

    check(count_of(buf, "[FILE  ] delete ") == 2, "exactly two delete lines");
    check(has_delete_line(buf, "by_path.dat"), "NtDeleteFile's line names its file");
    check(has_delete_line(buf, "by_handle.dat"), "the disposition's line names its file");
    check(!has_delete_line(buf, "kept.dat"), "the file left alone has no delete line");

    {
        char cmd[700];
#if defined(_WIN32)
        snprintf(cmd, sizeof cmd, "cmd /c rmdir /s /q \"%s\"", root);
#else
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
#endif
        (void)system(cmd);
    }
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
