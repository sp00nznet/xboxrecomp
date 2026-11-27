/* RECOMP_APU_TRACE lines go through a buffer that a logger thread writes to
 * stderr, so an audio thread never waits on a slow stderr. Lines arrive, and
 * a burst larger than the buffer is dropped and counted rather than waited
 * out.
 *
 * stderr goes to a file; the APU is brought up as in apu_lifecycle, with a
 * fake device and a zeroed buffer for RAM. */
#include "apu_state.h"
#include "apu.h"
#include "apu_xaudio2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

int  xa2_init(void)            { return 1; }
void xa2_shutdown(void)        {}
int  xa2_is_active(void)       { return 1; }
int  xa2_get_buffer_size(void) { return 512; }
int  xa2_get_buffer_count(void) { return 4; }
unsigned xa2_underruns(void)   { return 0; }
int  xa2_queued_samples(void)  { return 0; }
uint64_t xa2_samples_played(void) { return 0; }
void xa2_expect_drain(void)    {}
int  xa2_submit_samples(const int16_t *s, int n) { (void)s; (void)n; return 1; }

#define FAKE_RAM (256u << 10)
#define BURST    20000                 /* lines of ~100 bytes: 2 MB, past the buffer */
#define WAIT_MS  5000                  /* for the 50 ms logger on a busy machine */

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000u);
#endif
}

/* Lines in the log starting with prefix, and whether any contains needle. */
static int count_lines(const char *path, const char *prefix, const char *needle, int *found)
{
    char line[512];
    int n = 0;
    FILE *f = fopen(path, "r");

    *found = 0;
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, prefix, strlen(prefix)))
            n++;
        if (strstr(line, needle))
            *found = 1;
    }
    fclose(f);
    return n;
}

int main(void)
{
    static const char pad[] = "................................................................................";
    char log[600], longline[1500];
    uint8_t *ram = (uint8_t *)calloc(1, FAKE_RAM);
    int i, n, t, sent, dropped;

    if (!ram) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        snprintf(log, sizeof log, "%sapu-trace-%lu.log", tmp, (unsigned long)GetCurrentProcessId());
    }
    _putenv_s("RECOMP_APU_TRACE", "1");
#else
    snprintf(log, sizeof log, "/tmp/apu-trace-%ld.log", (long)getpid());
    setenv("RECOMP_APU_TRACE", "1", 1);
#endif
    if (!freopen(log, "w", stderr))
        return 2;
    if (!mcpx_apu_init_standalone(ram))
        return 2;

    printf("apu trace\n");
    apu_trace("[TEST] one %d\n", 1);
    for (t = 0, n = 0; t < WAIT_MS && n != 1; t += 50) {
        sleep_ms(50);
        n = count_lines(log, "[TEST] one", "lines dropped", &dropped);
    }
    check(n == 1, "a line reaches stderr through the logger");

    /* A line longer than apu_trace's own is cut and keeps its newline. */
    memset(longline, 'x', sizeof longline - 1);
    longline[sizeof longline - 1] = 0;
    apu_trace("[TEST] long %s\n", longline);
    apu_trace("[TEST] after\n");
    for (t = 0, n = 0; t < WAIT_MS && n != 1; t += 50) {
        sleep_ms(50);
        n = count_lines(log, "[TEST] after", "lines dropped", &dropped);
    }
    check(n == 1, "a line cut short ends with its newline");

    /* Bursts until one outruns the logger: the first, unless this machine
     * formats lines slower than the logger writes them. */
    for (sent = 0, t = 0; t < WAIT_MS; t += 50) {
        for (i = 0; i < BURST; i++, sent++)
            apu_trace("[TEST] burst %05d %s\n", i, pad);
        sleep_ms(50);
        n = count_lines(log, "[TEST] burst", "lines dropped", &dropped);
        if (dropped)
            break;
    }
    for (; t < WAIT_MS && n == 0; t += 50) {
        sleep_ms(50);
        n = count_lines(log, "[TEST] burst", "lines dropped", &dropped);
    }
    check(n > 0 && n < sent, "a burst past the buffer is cut short");
    check(dropped, "... and the drop is reported");

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    remove(log);
    return failures ? 1 : 0;
}
