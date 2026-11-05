/*
 * recomp_exe_dir.c - see recomp_exe_dir.h.
 *
 * macOS: _NSGetExecutablePath, then realpath (it may name a symlink).
 * Windows: GetModuleFileNameA (under Wine a Z:\ path, which fopen takes).
 * Linux and other POSIX: readlink("/proc/self/exe").
 * Anything failing: "." (the working directory, the old behaviour).
 */
#include "recomp_exe_dir.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static char s_dir[PATH_MAX];

/* Cut `p` at its last separator; 0 if it has none. */
static int cut_dir(char *p)
{
    char *s = strrchr(p, '/');
#if defined(_WIN32)
    char *b = strrchr(p, '\\');
    if (!s || (b && b > s))
        s = b;
#endif
    if (!s)
        return 0;
    if (s == p)
        s[1] = 0;           /* "/exe" -> "/" */
    else
        *s = 0;
    return 1;
}

static int resolve(char *out, size_t cap)
{
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap)
        return 0;
    return cut_dir(out);
#elif defined(__APPLE__)
    char raw[PATH_MAX];
    uint32_t sz = sizeof raw;
    if (_NSGetExecutablePath(raw, &sz) != 0)
        return 0;
    if (!realpath(raw, out)) {
        if (strlen(raw) >= cap)
            return 0;
        strcpy(out, raw);
    }
    return cut_dir(out);
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0)
        return 0;
    out[n] = 0;
    return cut_dir(out);
#endif
}

const char *recomp_exe_dir(void)
{
    if (!s_dir[0] && !resolve(s_dir, sizeof s_dir))
        strcpy(s_dir, ".");
    return s_dir;
}
