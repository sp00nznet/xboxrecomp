/*
 * The [MEM] line (mem_stats.c): what the bench parses must read back as
 * written, n/a must stay n/a, a short buffer must truncate cleanly, this
 * process must report its RAM, and (POSIX) a SIGINT must print the exit
 * line once and still end the process by SIGINT.
 */
#include "mem_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

#define MIB (1024ull * 1024ull)

static void test_round_trip(void)
{
    struct mem_stats m = {0}, r;
    char line[512], kind[16];
    int n;

    m.ram = 812 * MIB + 400 * 1024;          /* 812.4 MiB (0.39 rounds to .4) */
    m.ram_peak = 905 * MIB + MIB / 10;
    m.commit = m.commit_peak = MEM_STATS_NA;
    m.gpu = 301 * MIB;
    m.gpu_peak = 350 * MIB + MIB / 5;
    m.gpu_nl = 0;
    m.gpu_nl_peak = MEM_STATS_NA;
    m.ramcache = m.ramcache_peak = MEM_STATS_NA;
    m.ram_src = "phys_footprint";
    m.gpu_src = "metal";
    mem_stats_format(line, sizeof line, "exit", -1, &m, "atexit");
    printf("  %s\n", line);
    check(!strcmp(line, "exit ram_mib=812.4 ram_peak_mib=905.1 commit_mib=n/a"
                        " commit_peak_mib=n/a gpu_mib=301.0 gpu_peak_mib=350.2"
                        " gpu_nl_mib=0.0 gpu_nl_peak_mib=n/a ram_src=phys_footprint"
                        " gpu_src=metal why=atexit"),
          "format: the exact exit line");
    n = mem_stats_parse(line, kind, sizeof kind, &r);
    check(n == 5, "parse: five fields read, three n/a");
    check(!strcmp(kind, "exit"), "parse: kind");
    check(r.ram / (MIB / 10) == 8124 && r.gpu == 301 * MIB, "parse: values to 0.1 MiB");
    check(r.commit == MEM_STATS_NA && r.gpu_nl_peak == MEM_STATS_NA, "parse: n/a stays n/a");
    check(r.gpu_nl == 0, "parse: zero is not n/a");
    check(r.ramcache == MEM_STATS_NA, "parse: no RAM cache fields, n/a");

    /* The RAM cache in use: its two fields after the GPU's. */
    m.ramcache = 312 * MIB;
    m.ramcache_peak = 640 * MIB;
    mem_stats_format(line, sizeof line, "exit", -1, &m, "atexit");
    check(strstr(line, " gpu_nl_peak_mib=n/a ramcache_mib=312.0 ramcache_peak_mib=640.0"
                       " ram_src=") != NULL,
          "format: the RAM cache fields");
    check(mem_stats_parse(line, kind, sizeof kind, &r) == 7 && r.ramcache == 312 * MIB
              && r.ramcache_peak == 640 * MIB,
          "parse: the RAM cache fields");
    m.ramcache = m.ramcache_peak = MEM_STATS_NA;

    mem_stats_format(line, sizeof line, "sample", 12345, &m, NULL);
    check(!strncmp(line, "sample t=12.3 ram_mib=", 22) && !strstr(line, "why="),
          "format: sample with t, without why");
    {
        char full[600];
        snprintf(full, sizeof full, "noise before [MEM] %s", line);
        check(mem_stats_parse(full, kind, sizeof kind, &r) == 5 && !strcmp(kind, "sample"),
              "parse: prefix found mid-line");
    }
    check(mem_stats_parse("[PROF] nothing", kind, sizeof kind, &r) == -1,
          "parse: not a [MEM] line");
    check(mem_stats_parse("[MEM] exit ram_mib=7.5 future_mib=3.0", kind, sizeof kind, &r) == 1
              && r.ram == (uint64_t)(7.5 * MIB),
          "parse: unknown keys ignored");
    {
        char small[20];
        size_t len = mem_stats_format(small, sizeof small, "exit", -1, &m, "x");
        check(len == sizeof small - 1 && strlen(small) == sizeof small - 1,
              "format: truncates to the buffer, NUL-terminated");
    }
}

static void test_live(void)
{
    struct mem_stats m;
    int ok = mem_stats_read(&m);
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    check(ok && m.ram != MEM_STATS_NA && m.ram > MIB, "read: this process has RAM");
    check(m.ram_peak == MEM_STATS_NA || m.ram_peak >= m.ram / 2, "read: a sane peak");
#else
    check(!ok, "read: no source on this host");
#endif
    check(m.gpu == MEM_STATS_NA && !strcmp(m.gpu_src, "none"), "read: no GPU before a note");
    mem_stats_gpu_note("test", 200 * MIB, MEM_STATS_NA);
    mem_stats_gpu_note("test", 100 * MIB, 5 * MIB);
    mem_stats_read(&m);
    check(m.gpu == 100 * MIB && m.gpu_peak == 200 * MIB, "note: last and largest");
    check(m.gpu_nl == 5 * MIB && m.gpu_nl_peak == 5 * MIB, "note: non-local");
    check(!strcmp(m.gpu_src, "test"), "note: source");
    check(m.ramcache == MEM_STATS_NA, "read: no RAM cache before a note");
    mem_stats_ramcache_note(64 * MIB);
    mem_stats_ramcache_note(16 * MIB);
    mem_stats_read(&m);
    check(m.ramcache == 16 * MIB && m.ramcache_peak == 64 * MIB, "ramcache note: last and largest");
}

#ifndef _WIN32
static void test_sigint(void)
{
    int fds[2], st = 0;
    pid_t pid;
    char buf[2048];
    ssize_t got = 0, n;
    struct mem_stats r;
    char kind[16];

    if (pipe(fds)) {
        check(0, "sigint: pipe");
        return;
    }
    pid = fork();
    if (pid == 0) {
        dup2(fds[1], 2);
        close(fds[0]);
        mem_stats_start_from_env();
        raise(SIGINT);
        _exit(3);   /* not reached: SIGINT's default ends it */
    }
    close(fds[1]);
    while ((n = read(fds[0], buf + got, sizeof buf - 1 - (size_t)got)) > 0)
        got += n;
    buf[got > 0 ? got : 0] = 0;
    close(fds[0]);
    waitpid(pid, &st, 0);
    printf("  child: %s", buf);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGINT, "sigint: the child still dies by SIGINT");
    check(strstr(buf, "[MEM] exit ") && strstr(buf, "why=sigint"), "sigint: the exit line");
    {
        const char *q = buf;
        int lines = 0;
        while ((q = strstr(q, "[MEM] exit ")) != NULL) {
            lines++;
            q++;
        }
        check(lines == 1, "sigint: printed once");
    }
    check(mem_stats_parse(buf, kind, sizeof kind, &r) >= 2 && r.ram != MEM_STATS_NA,
          "sigint: the line parses with RAM");
}
#endif

/* RECOMP_TRACE=mem=S by value: a period under a second starts with '0'
 * and must not read as off. */
static void test_period(void)
{
    check(mem_stats_period_ms(NULL) == 0, "period: unset is off");
    check(mem_stats_period_ms("") == 0, "period: empty is off");
    check(mem_stats_period_ms("0") == 0, "period: 0 is off");
    check(mem_stats_period_ms("0.0") == 0, "period: 0.0 is off");
    check(mem_stats_period_ms("-2") == 0, "period: negative is off");
    check(mem_stats_period_ms("x") == 0, "period: not a number is off");
    check(mem_stats_period_ms("1") == 1000, "period: bare mem (\"1\") is 1 s");
    check(mem_stats_period_ms("0.5") == 500, "period: 0.5 is 500 ms");
    check(mem_stats_period_ms("0.25") == 250, "period: 0.25 is 250 ms");
    check(mem_stats_period_ms("0.01") == 100, "period: clamped to 100 ms");
    check(mem_stats_period_ms("10") == 10000, "period: 10 is 10 s");
}

int main(void)
{
    printf("mem_stats:\n");
    test_round_trip();
    test_period();
    test_live();
#ifndef _WIN32
    test_sigint();
#endif
    printf("%s (%d failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
