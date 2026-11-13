/*
 * An exit as a window (prof_dur) ends: the sampler and the exit path both
 * call host_prof_dump. Whichever loses waits for the other to finish, so
 * when the exit's call returns the report is whole: header, every line
 * ending in a newline, no .tmp. Before, the loser returned at once and
 * exit() tore stdio down under the sampler's fprintf, leaving a truncated
 * report with a valid header.
 */
#include "host_prof.h"

#include <stdio.h>
#include <string.h>

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
    const char *path = "host_prof_dur_exit_report.txt";
    char line[600] = "";
    int lines = 0, whole = 1, c, last = '\n';
    FILE *f;

    remove(path);
    printf("host_prof: an exit as a 100 ms window ends waits for its report\n");
    check(host_prof_start_for(1000, 0, 100, path) == 1, "the sampler starts");
    /* About when the window ends: either the sampler is writing, and this
     * call waits for it, or this call wins and writes. Both leave the whole
     * report on return, which is all an exit needs. */
    nap(100);
    host_prof_dump();
    f = fopen(path, "r");
    check(f != NULL, "the report is in place when the exit's dump returns");
    if (f) {
        check(fgets(line, sizeof line, f) && strstr(line, "# recomp-prof 1 ") == line,
              "the report has its header");
        rewind(f);
        while ((c = fgetc(f)) != EOF) {
            if (c == '\n')
                lines++;
            last = c;
        }
        whole = last == '\n';
        fclose(f);
        printf("  %d lines\n", lines);
        check(whole && lines >= 2, "the report ends on a whole line");
    }
    f = fopen("host_prof_dur_exit_report.txt.tmp", "r");
    check(f == NULL, "no .tmp is left behind");
    if (f)
        fclose(f);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
