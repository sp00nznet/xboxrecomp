/**
 * xbox_input host_open (host_open.h): the request, and the Win32 call.
 */
#include "host_open.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <stdlib.h>

/* ShellExecuteW can block for seconds (Explorer starting, a shell
 * extension) and wants COM on its thread, so it runs on a thread of its
 * own, never the menu's (the guest's). */
static DWORD WINAPI open_thread(LPVOID arg)
{
    wchar_t *w = arg;
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    /* ShellExecuteW returns a value above 32 on success. */
    INT_PTR rc = (INT_PTR)ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    if (rc <= 32)
        fprintf(stderr, "[INPUT] opening the folder failed (ShellExecuteW %d)\n", (int)rc);
    if (SUCCEEDED(co))
        CoUninitialize();
    free(w);
    return 0;
}

int xbox_HostOpenFolder(const char *path)
{
    wchar_t *w;
    HANDLE t;
    if (!path || !(w = malloc(XBOX_HOST_OPEN_PATH_MAX * sizeof *w)))
        return 0;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w,
                             XBOX_HOST_OPEN_PATH_MAX)) {
        free(w);
        return 0;
    }
    t = CreateThread(NULL, 0, open_thread, w, 0, NULL);
    if (!t) {
        free(w);
        return 0;
    }
    CloseHandle(t);
    return 1;
}

int xbox_HostOpenTake(char *out, size_t n)
{
    (void)out;
    (void)n;
    return 0;
}

#else

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

/* One slot: EMPTY, WRITING (the menu's thread), READY, READING (the
 * window's). Each side claims it with a compare-and-swap, so neither waits
 * on the other. */
enum { SLOT_EMPTY, SLOT_WRITING, SLOT_READY, SLOT_READING };
static int s_slot;
static char s_path[XBOX_HOST_OPEN_PATH_MAX];

static int claim(int from, int to)
{
    return __atomic_compare_exchange_n(&s_slot, &from, to, 0, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}

int xbox_HostOpenFolder(const char *path)
{
    if (!path || strlen(path) >= sizeof s_path)
        return 0;
    if (!claim(SLOT_EMPTY, SLOT_WRITING) && !claim(SLOT_READY, SLOT_WRITING))
        return 0;
    snprintf(s_path, sizeof s_path, "%s", path);
    __atomic_store_n(&s_slot, SLOT_READY, __ATOMIC_RELEASE);
    return 1;
}

int xbox_HostOpenTake(char *out, size_t n)
{
    if (!claim(SLOT_READY, SLOT_READING))
        return 0;
    snprintf(out, n, "%s", s_path);
    __atomic_store_n(&s_slot, SLOT_EMPTY, __ATOMIC_RELEASE);
    return 1;
}

/* A file URL's path: the bytes a URL may hold as they are, the rest
 * percent-encoded (a space, '#', '%', UTF-8), so a folder with any name
 * opens. A file URL holds an absolute path: the default user root is "."
 * (file://./config would name a host "."), so a relative one is resolved
 * first, against the working directory when it does not exist yet. */
int xbox_HostOpenFileUrl(const char *path, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    char abs[PATH_MAX + XBOX_HOST_OPEN_PATH_MAX + 2];
    size_t k;
    if (!path || !path[0] || n < 8)
        return 0;
    if (!realpath(path, abs)) {
        char cwd[PATH_MAX];
        if (path[0] == '/')
            snprintf(abs, sizeof abs, "%s", path);
        else if (getcwd(cwd, sizeof cwd))
            snprintf(abs, sizeof abs, "%s/%s", cwd, path);
        else
            return 0;
    }
    k = (size_t)snprintf(out, n, "file://");
    for (const unsigned char *p = (const unsigned char *)abs; *p; p++) {
        int plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
                 || (*p >= '0' && *p <= '9') || *p == '/' || *p == '-' || *p == '_'
                 || *p == '.' || *p == '~';
        if (k + 4 > n)
            return 0;
        if (plain) {
            out[k++] = (char)*p;
        } else {
            out[k++] = '%';
            out[k++] = hex[*p >> 4];
            out[k++] = hex[*p & 15];
        }
    }
    out[k] = 0;
    return 1;
}

#endif
