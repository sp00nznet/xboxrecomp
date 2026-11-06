/*
 * RECOMP_WAIT_LOG: one stderr line for each single-object wait that timed out
 * or lasted at least 500 ms; nothing at all when the variable is unset; and the
 * wait itself returns what it always did.
 *
 * Cross-platform: it calls the xbox_Nt* / xbox_Ke* wait functions directly. The
 * CTest entries run it twice, once with the variable set and once without; the
 * test reads the variable to know which output to expect.
 */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    for (; (hay = strstr(hay, needle)) != NULL; hay += strlen(needle))
        n++;
    return n;
}

int main(void)
{
    const int want = getenv("RECOMP_WAIT_LOG") != NULL;
    const char *path = "wait_log_test.stderr";
    char buf[4096];
    size_t got;
    FILE *f;
    HANDLE ev = NULL;
    LARGE_INTEGER t;
    char timeout_field[64];

    /* Everything the wait functions print on stderr goes to a file we can read. */
    if (!freopen(path, "w", stderr)) {
        printf("cannot redirect stderr\n");
        return 2;
    }

    check(xbox_NtCreateEvent(&ev, NULL, XboxNotificationEvent, FALSE) == STATUS_SUCCESS,
          "create a manual-reset event, not signalled");

    t.QuadPart = -20 * 10000LL;                 /* 20 ms, relative */
    check(xbox_NtWaitForSingleObject(ev, FALSE, &t) == STATUS_TIMEOUT,
          "NtWaitForSingleObject times out");
    check(xbox_NtWaitForSingleObjectEx(ev, 0, FALSE, &t) == STATUS_TIMEOUT,
          "NtWaitForSingleObjectEx times out");
    check(xbox_KeWaitForSingleObject(ev, 0, 0, FALSE, &t) == STATUS_TIMEOUT,
          "KeWaitForSingleObject times out");

    xbox_NtSetEvent(ev, NULL);
    check(xbox_NtWaitForSingleObject(ev, FALSE, &t) == STATUS_SUCCESS,
          "a signalled wait returns at once");

    fflush(stderr);
    f = fopen(path, "r");
    got = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[got] = 0;
    if (f) fclose(f);

    if (want) {
        snprintf(timeout_field, sizeof timeout_field, "timeout=20 ms result=%lu",
                 (unsigned long)WAIT_TIMEOUT);
        check(count_of(buf, "[WAIT] NtWaitForSingleObject tid=") == 1,
              "set: NtWaitForSingleObject logged once");
        check(count_of(buf, "[WAIT] NtWaitForSingleObjectEx tid=") == 1,
              "set: NtWaitForSingleObjectEx logged once");
        check(count_of(buf, "[WAIT] KeWaitForSingleObject tid=") == 1,
              "set: KeWaitForSingleObject logged once");
        check(count_of(buf, timeout_field) == 3,
              "set: each line carries the 20 ms timeout and the timeout result");
        check(count_of(buf, "[WAIT]") == 3,
              "set: the quick signalled wait wrote nothing");
    } else {
        check(count_of(buf, "[WAIT]") == 0, "unset: no [WAIT] line at all");
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
