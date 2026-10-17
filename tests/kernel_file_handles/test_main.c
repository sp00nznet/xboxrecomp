/*
 * POSIX file handles: validation, release, and the open-file limit.
 *
 * BLiNX 2 on a first boot creates its Z: cache and keeps one handle per
 * file open, as the console allows. Under macOS's Finder soft limit of 256
 * its 247th open failed with EMFILE, the title passed the INVALID_HANDLE_VALUE
 * it got back to NtQueryInformationFile, and w32_handle_fd dereferenced it.
 * This covers the three parts of the fix:
 *   - the file HLE answers STATUS_INVALID_HANDLE for anything that is not a
 *     live file handle, instead of faulting;
 *   - NtClose gives the host fd back, for the cache's open/close/reopen
 *     pattern as well;
 *   - w32_raise_fd_limit lifts a 256 soft limit far enough for a full cache
 *     (363 files), and an exhausted limit reads TOO_MANY_OPENED_FILES.
 * Then a few threads open, look up and close handles at once, since the
 * title's file calls come from several threads (build with
 * -DXBOX_FILE_HANDLES_ASAN=ON to catch a lookup that touches freed memory).
 */
#include "kernel.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

#define CACHE_FILES 400   /* more than a full cache, well past 256 */

static int failures;

static void check(int ok, const char *what, unsigned got)
{
    printf("  %-62s %s", what, ok ? "PASS" : "FAIL");
    if (!ok) printf(" -- got 0x%08X", got);
    putchar('\n');
    if (!ok) failures++;
}

/* Host fds open in this process. */
static int count_fds(void)
{
    struct rlimit rl;
    int n = 0, top;
    getrlimit(RLIMIT_NOFILE, &rl);
    top = rl.rlim_cur > 8192 ? 8192 : (int)rl.rlim_cur;
    for (int fd = 0; fd < top; fd++)
        if (fcntl(fd, F_GETFD) != -1) n++;
    return n;
}

static NTSTATUS open_cache(int i, ULONG options, HANDLE *h)
{
    char path[64];
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    snprintf(path, sizeof path, "D:\\cache\\blinx2_%03d.rec", i);
    name.Length = (USHORT)strlen(path);
    name.MaximumLength = (USHORT)(name.Length + 1);
    name.Buffer = path;
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = 0x40;
    *h = NULL;
    /* GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE, FILE_OPEN_IF, as the title */
    return xbox_NtCreateFile(h, 0xC0100000u, &oa, &ios, NULL, 0x80, 0,
                             XBOX_FILE_OPEN_IF, options);
}

static void bad_handle(HANDLE h, const char *what)
{
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_FILE_STANDARD_INFORMATION info;
    char buf[16], line[96];
    NTSTATUS st;

    st = xbox_NtQueryInformationFile(h, &ios, &info, sizeof info,
                                     XboxFileStandardInformation);
    snprintf(line, sizeof line, "%s: NtQueryInformationFile INVALID_HANDLE", what);
    check(st == STATUS_INVALID_HANDLE, line, (unsigned)st);
    st = xbox_NtReadFile(h, NULL, NULL, NULL, &ios, buf, sizeof buf, NULL);
    snprintf(line, sizeof line, "%s: NtReadFile INVALID_HANDLE", what);
    check(st == STATUS_INVALID_HANDLE, line, (unsigned)st);
    st = xbox_NtWriteFile(h, NULL, NULL, NULL, &ios, buf, sizeof buf, NULL);
    snprintf(line, sizeof line, "%s: NtWriteFile INVALID_HANDLE", what);
    check(st == STATUS_INVALID_HANDLE, line, (unsigned)st);
    snprintf(line, sizeof line, "%s: no fd, no path", what);
    check(w32_handle_fd(h) == -1 && w32_handle_path(h) == NULL, line, 0);
}

#define STRESS_THREADS 4
#define STRESS_ROUNDS 2000

static volatile LONG stress_errors;

/* One file per thread, opened and closed over and over. After each close
 * the stale handle is looked up again while the other threads allocate and
 * free objects: it must answer "not a file handle" or, if its address has
 * been reused, some other thread's live handle, and never fault. */
static void *stress_thread(void *arg)
{
    int k = (int)(intptr_t)arg;
    for (int r = 0; r < STRESS_ROUNDS; r++) {
        XBOX_IO_STATUS_BLOCK ios;
        XBOX_FILE_STANDARD_INFORMATION info;
        HANDLE h;
        if (open_cache(900 + k, 0x48, &h) != STATUS_SUCCESS || w32_handle_fd(h) < 0
                || xbox_NtQueryInformationFile(h, &ios, &info, sizeof info,
                                               XboxFileStandardInformation) != STATUS_SUCCESS) {
            InterlockedIncrement(&stress_errors);
            if (h) xbox_NtClose(h);
            continue;
        }
        xbox_NtClose(h);
        (void)w32_handle_fd(h);
        (void)w32_handle_path(h);
        (void)xbox_NtQueryInformationFile(h, &ios, &info, sizeof info,
                                          XboxFileStandardInformation);
    }
    return NULL;
}

int main(void)
{
    static HANDLE kept[CACHE_FILES];
    char root[64], cache[96];
    struct rlimit rl, saved;
    unsigned long long was = 0, now = 0;
    NTSTATUS st;
    int base_fds, base_handles, opened = 0, rc, enough;

    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(root, sizeof root, "/tmp/filehandlesXXXXXX");
    if (!mkdtemp(root)) return 2;
    snprintf(cache, sizeof cache, "%s/cache", root);
    mkdir(cache, 0755);
    xbox_path_init(root, root);

    printf("handles that are not live file handles\n");
    bad_handle(NULL, "NULL");
    bad_handle(INVALID_HANDLE_VALUE, "INVALID_HANDLE_VALUE");
    bad_handle((HANDLE)(uintptr_t)0xDEAD0001u, "synthetic 0xDEAD0001");
    bad_handle((HANDLE)(uintptr_t)1, "the value 1");
    {
        HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        bad_handle(ev, "an event");
        CloseHandle(ev);
    }
    {
        HANDLE h;
        st = open_cache(999, 0x60, &h);
        check(st == STATUS_SUCCESS && w32_handle_fd(h) >= 0, "a fresh file handle has an fd", (unsigned)st);
        xbox_NtClose(h);
        bad_handle(h, "a closed file handle");
    }

    printf("the 256 soft limit is raised\n");
    getrlimit(RLIMIT_NOFILE, &saved);
    rl = saved;
    rl.rlim_cur = 256;
    enough = rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= CACHE_FILES + 64;
    if (!enough) {
        printf("  hard limit %llu is below the test's need: skipped\n",
               (unsigned long long)rl.rlim_max);
    } else {
        check(setrlimit(RLIMIT_NOFILE, &rl) == 0, "soft limit set to 256", 0);
        rc = w32_raise_fd_limit(&was, &now);
        printf("  raised %llu -> %llu (hard %llu)\n", was, now,
               (unsigned long long)rl.rlim_max);
        check(rc == 1 && was == 256 && now >= CACHE_FILES + 64,
              "w32_raise_fd_limit lifts it past a full cache", (unsigned)now);
        check(w32_raise_fd_limit(&was, &now) == 0 && was == now,
              "a second call has nothing left to do", (unsigned)was);
    }

    printf("the cache pattern: open, size, close, reopen and keep\n");
    if (!enough)
        printf("  hard limit %llu is below the test's need: skipped\n",
               (unsigned long long)rl.rlim_max);
    base_fds = count_fds();
    base_handles = w32_open_file_handles();
    for (int i = 0; enough && i < CACHE_FILES; i++) {
        HANDLE probe;
        if (open_cache(i, 0x60, &probe) != STATUS_SUCCESS) break;
        xbox_NtClose(probe);
        if (open_cache(i, 0x48, &kept[i]) != STATUS_SUCCESS) break;
        opened++;
    }
    if (enough) {
        check(opened == CACHE_FILES, "every cache file opened", (unsigned)opened);
        check(count_fds() - base_fds == opened, "one host fd per kept handle, none per closed one",
              (unsigned)(count_fds() - base_fds));
        check(w32_open_file_handles() - base_handles == opened, "the live-handle count agrees",
              (unsigned)(w32_open_file_handles() - base_handles));
        for (int i = 0; i < opened; i++)
            xbox_NtClose(kept[i]);
        check(count_fds() == base_fds, "NtClose gives every fd back", (unsigned)count_fds());
        check(w32_open_file_handles() == base_handles, "and every handle", (unsigned)w32_open_file_handles());
    }

    printf("%d threads open, look up and close at once\n", STRESS_THREADS);
    {
        pthread_t t[STRESS_THREADS];
        int started = 0;
        base_fds = count_fds();
        base_handles = w32_open_file_handles();
        for (int k = 0; k < STRESS_THREADS; k++)
            if (pthread_create(&t[k], NULL, stress_thread, (void *)(intptr_t)k) == 0)
                started++;
        for (int k = 0; k < started; k++)
            pthread_join(t[k], NULL);
        check(started == STRESS_THREADS, "every thread started", (unsigned)started);
        check(stress_errors == 0, "every open, lookup and query of a live handle worked",
              (unsigned)stress_errors);
        check(count_fds() == base_fds && w32_open_file_handles() == base_handles,
              "and nothing is left open", (unsigned)w32_open_file_handles());
    }

    printf("an exhausted limit\n");
    rl = saved;
    rl.rlim_cur = (rlim_t)(count_fds() + 8);
    check(setrlimit(RLIMIT_NOFILE, &rl) == 0, "soft limit set just above the open fds", 0);
    opened = 0;
    st = STATUS_SUCCESS;
    for (int i = 0; i < 64 && st == STATUS_SUCCESS; i++) {
        st = open_cache(i, 0x48, &kept[i]);
        if (st == STATUS_SUCCESS) opened++;
    }
    check(st == STATUS_TOO_MANY_OPENED_FILES, "the failed open says TOO_MANY_OPENED_FILES", (unsigned)st);
    for (int i = 0; i < opened; i++)
        xbox_NtClose(kept[i]);
    setrlimit(RLIMIT_NOFILE, &saved);

    for (int i = 0; i < CACHE_FILES; i++) {
        char p[160];
        snprintf(p, sizeof p, "%s/blinx2_%03d.rec", cache, i);
        unlink(p);
    }
    {
        char p[160];
        snprintf(p, sizeof p, "%s/blinx2_999.rec", cache);
        unlink(p);
        for (int k = 0; k < STRESS_THREADS; k++) {
            snprintf(p, sizeof p, "%s/blinx2_%03d.rec", cache, 900 + k);
            unlink(p);
        }
    }
    rmdir(cache);
    rmdir(root);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
