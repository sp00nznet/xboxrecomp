/*
 * A window (prof_dur): the sampler stops and writes the report itself once
 * the window ends, with no dump call and no exit. A bench run killed at its
 * time limit (Proton: the SIGINT goes to umu-run, not the game) never runs
 * the exit hook, and a report left for the exit was lost.
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

static void nap(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000u);
#endif
}

int main(void)
{
    char path[] = "host_prof_dur_report.txt";
    char line[600] = "";
    unsigned long long rounds = 0;
    FILE *f = NULL;

    remove(path);
    printf("host_prof: a 300 ms window at 100 Hz writes its own report\n");
    check(host_prof_start_for(100, 0, 300, path) == 1, "the sampler starts");
    /* Up to 5 s for the report: the window is 0.3 s. It is written to a
     * .tmp and renamed into place, so once it opens it is whole. */
    for (int i = 0; i < 100 && !(f = fopen(path, "r")); i++)
        nap(50);
    check(f != NULL, "the report is written without a dump or an exit");
    if (f) {
        const char *r;
        check(f && fgets(line, sizeof line, f) && strstr(line, "# recomp-prof 1 ") == line,
              "the report has its header");
        r = strstr(line, " rounds ");
        if (r)
            sscanf(r, " rounds %llu", &rounds);
        printf("  rounds %llu\n", rounds);
        check(rounds >= 10 && rounds <= 60, "the window held about 30 rounds");
        if (f)
            fclose(f);
        f = fopen("host_prof_dur_report.txt.tmp", "r");
        check(f == NULL, "no .tmp is left behind");
        if (f)
            fclose(f);
        f = NULL;
    }
    /* The window is over: sampling has stopped, and a later dump (the exit
     * hook's) does not overwrite the report. */
    remove(path);
    nap(300);
    host_prof_dump();
    f = fopen(path, "r");
    check(f == NULL, "a dump after the window writes nothing");
    if (f)
        fclose(f);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
