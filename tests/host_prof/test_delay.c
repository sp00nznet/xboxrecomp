/*
 * A dump while the sampler still naps out its start delay (prof_delay) must
 * not wait for that nap: the dump waits only on a round in flight, so it
 * returns at once and writes a sorted (here empty) table. It used to wait on
 * the sampler's whole life, give up after a second and dump unsorted.
 */
#include "host_prof.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

static double now_s(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void)
{
    char path[] = "host_prof_delay_report.txt";
    char line[600] = "";
    double t0, took;
    FILE *f;

    printf("host_prof: a dump during a 5 s start delay\n");
    check(host_prof_start(250, 5000, path) == 1, "the sampler starts");
#ifdef _WIN32
    Sleep(50);
#else
    usleep(50000);
#endif
    t0 = now_s();
    host_prof_dump();
    took = now_s() - t0;
    printf("  the dump took %.3f s\n", took);
    check(took < 0.5, "the dump does not wait out the delay");
    f = fopen(path, "r");
    check(f != NULL, "the report is written");
    if (f) {
        check(fgets(line, sizeof line, f) && strstr(line, " rounds 0 ") != NULL,
              "the report counts no rounds");
        fclose(f);
        remove(path);
    }
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
