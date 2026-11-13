/*
 * recomp_stdio_test - recomp_stdio.c: the buffering policy, and the flush
 * once a flip on a buffered and an unbuffered stream (temporary files).
 */
#include "recomp_stdio.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int s_fail;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    s_fail = 1; } } while (0)

/* What the file holds on disk, not in the stream's buffer. */
static long on_disk(FILE *f)
{
    struct stat st;

    if (fstat(fileno(f), &st) != 0)
        return -1;
    return (long)st.st_size;
}

int main(void)
{
    static const char line[] = "[PB-PERF] flip 1 wall 33.33 ms\n";
    const long n = (long)strlen(line);
    FILE *held, *direct;

    /* Only Windows buffers, and never a console. */
    CHECK(recomp_stdio_policy(1, 0) == 1);
    CHECK(recomp_stdio_policy(1, 1) == 0);
    CHECK(recomp_stdio_policy(0, 0) == 0);
    CHECK(recomp_stdio_policy(0, 1) == 0);

    /* Unbuffered: on disk as soon as it is printed; nothing is marked yet,
     * so the flush has nothing to do. */
    direct = tmpfile();
    CHECK(direct != NULL);
    if (!direct)
        return 1;
    recomp_stdio_apply(direct, 0);
    fputs(line, direct);
    CHECK(on_disk(direct) == n);
    recomp_stdio_flush();
    CHECK(on_disk(direct) == n);

    /* Buffered: held until the flip's flush, then all of it. */
    held = tmpfile();
    CHECK(held != NULL);
    if (!held)
        return 1;
    /* Unbuffered first, so it is apply that buffers it, not the CRT's
     * default for a file. */
    setvbuf(held, NULL, _IONBF, 0);
    recomp_stdio_apply(held, 1);
    fputs(line, held);
    fputs(line, held);
    CHECK(on_disk(held) == 0);
    recomp_stdio_flush();
    CHECK(on_disk(held) == 2 * n);
    fputs(line, held);
    CHECK(on_disk(held) == 2 * n);
    recomp_stdio_flush();
    CHECK(on_disk(held) == 3 * n);

    /* Only a thread inside the flush says so. */
    CHECK(recomp_stdio_flushing_here() == 0);

    /* The unbuffered one is untouched by the flushes. */
    fputs(line, direct);
    CHECK(on_disk(direct) == 2 * n);

    fclose(held);
    fclose(direct);
    if (!s_fail)
        printf("recomp_stdio_test: ok\n");
    return s_fail;
}
