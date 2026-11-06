/*
 * POSIX open-file limit.
 *
 * A title may create its Z: cache on first boot and keep one handle per
 * file open, as the console allows. Under macOS's Finder soft limit of 256
 * one title's 247th open failed with EMFILE and answered "not found". This
 * covers the two parts of the fix:
 *   - w32_raise_fd_limit lifts a 256 soft limit far enough for a full cache
 *     (363 files in that title), and the cache's open, close, reopen and
 *     keep pattern then holds one host fd per kept handle;
 *   - an exhausted limit reads STATUS_TOO_MANY_OPENED_FILES, in the return
 *     value and the IO_STATUS_BLOCK.
 */
#include "kernel.h"

#include <fcntl.h>
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

static NTSTATUS open_cache(int i, ULONG options, HANDLE *h, NTSTATUS *ios_status)
{
    char path[64];
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    NTSTATUS st;
    snprintf(path, sizeof path, "D:\\cache\\cache_%03d.rec", i);
    name.Length = (USHORT)strlen(path);
    name.MaximumLength = (USHORT)(name.Length + 1);
    name.Buffer = path;
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = 0x40;
    *h = NULL;
    memset(&ios, 0, sizeof ios);
    /* GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE, FILE_OPEN_IF, as the title */
    st = xbox_NtCreateFile(h, 0xC0100000u, &oa, &ios, NULL, 0x80, 0,
                           XBOX_FILE_OPEN_IF, options);
    if (ios_status) *ios_status = ios.Status;
    return st;
}

int main(void)
{
    static HANDLE kept[CACHE_FILES];
    char root[64], cache[96];
    struct rlimit rl, saved;
    unsigned long long was = 0, now = 0;
    NTSTATUS st, ios_st = 0;
    int base_fds, opened = 0, rc, enough;

    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(root, sizeof root, "/tmp/fdlimitXXXXXX");
    if (!mkdtemp(root)) return 2;
    snprintf(cache, sizeof cache, "%s/cache", root);
    mkdir(cache, 0755);
    xbox_path_init(root, root);

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
    base_fds = count_fds();
    for (int i = 0; enough && i < CACHE_FILES; i++) {
        HANDLE probe;
        if (open_cache(i, 0x60, &probe, NULL) != STATUS_SUCCESS) break;
        xbox_NtClose(probe);
        if (open_cache(i, 0x48, &kept[i], NULL) != STATUS_SUCCESS) break;
        opened++;
    }
    if (enough) {
        check(opened == CACHE_FILES, "every cache file opened", (unsigned)opened);
        check(count_fds() - base_fds == opened, "one host fd per kept handle, none per closed one",
              (unsigned)(count_fds() - base_fds));
        for (int i = 0; i < opened; i++)
            xbox_NtClose(kept[i]);
        check(count_fds() == base_fds, "NtClose gives every fd back", (unsigned)count_fds());
    } else {
        printf("  skipped\n");
    }

    printf("an exhausted limit\n");
    rl = saved;
    rl.rlim_cur = (rlim_t)(count_fds() + 8);
    check(setrlimit(RLIMIT_NOFILE, &rl) == 0, "soft limit set just above the open fds", 0);
    opened = 0;
    st = STATUS_SUCCESS;
    for (int i = 0; i < 64 && st == STATUS_SUCCESS; i++) {
        st = open_cache(i, 0x48, &kept[i], &ios_st);
        if (st == STATUS_SUCCESS) opened++;
    }
    check(st == STATUS_TOO_MANY_OPENED_FILES, "the failed open says TOO_MANY_OPENED_FILES", (unsigned)st);
    check(ios_st == STATUS_TOO_MANY_OPENED_FILES, "and so does its IO_STATUS_BLOCK", (unsigned)ios_st);
    for (int i = 0; i < opened; i++)
        xbox_NtClose(kept[i]);
    setrlimit(RLIMIT_NOFILE, &saved);

    for (int i = 0; i < CACHE_FILES; i++) {
        char p[160];
        snprintf(p, sizeof p, "%s/cache_%03d.rec", cache, i);
        unlink(p);
    }
    rmdir(cache);
    rmdir(root);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
