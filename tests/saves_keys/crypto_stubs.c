/*
 * crypto_stubs.c - the two host calls kernel_crypto.c makes, for tests
 * that link it without the runtime (tests/saves_keys, tests/saves_zip).
 * Windows has SecureZeroMemory itself.
 */
#include "kernel.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
VOID SecureZeroMemory(PVOID ptr, SIZE_T cnt)
{
    volatile unsigned char *p = ptr;
    while (cnt--)
        *p++ = 0;
}
#endif

void xbox_log(int level, const char *subsystem, const char *fmt, ...)
{
    va_list ap;
    (void)level;
    fprintf(stderr, "[%s] ", subsystem);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
