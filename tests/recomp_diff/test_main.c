#include "kernel.h"
#include "recomp_diff.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *recomp_lookup(ULONG a) { (void)a; abort(); }
void *recomp_lookup_manual(ULONG a) { (void)a; abort(); }

/* Reads the dump back as the documented layout: ten little-endian u32 each. */
static uint32_t *load(const char *path, int expect)
{
    FILE *f = fopen(path, "rb");
    uint8_t *raw;
    uint32_t *out;
    int i;
    if (!f) return NULL;
    raw = malloc((size_t)expect * 40 + 1);
    if (fread(raw, 1, (size_t)expect * 40 + 1, f) != (size_t)expect * 40) { fclose(f); free(raw); return NULL; }
    fclose(f);
    out = malloc((size_t)expect * 40);
    for (i = 0; i < expect * 10; i++)
        out[i] = raw[i * 4] | raw[i * 4 + 1] << 8 | raw[i * 4 + 2] << 16 | (uint32_t)raw[i * 4 + 3] << 24;
    free(raw);
    return out;
}

int main(void)
{
    char path[MAX_PATH];
    uint32_t *r;
    uint32_t i;
    int ok = 1;

    GetTempPathA(MAX_PATH, path);
    sprintf(path + strlen(path), "recomp-diff-%lu.bin", GetCurrentProcessId());

    /* A few records come back in order with every field in place. */
    recomp_diff_checkpoint_reset();
    recomp_diff_checkpoint(0x1000, 1, 2, 3, 4, 5, 6, 7, 0x202);
    recomp_diff_checkpoint(0x1004, 0x80000000u, 0, 0, 0, 0, 0, 0, 0);
    if (recomp_diff_checkpoint_dump(path) != 2) { puts("FAIL count"); ok = 0; }
    r = load(path, 2);
    if (!r || r[0] != 1 || r[1] != 0x1000 || r[2] != 1 || r[8] != 7 || r[9] != 0x202 ||
        r[10] != 2 || r[11] != 0x1004 || r[12] != 0x80000000u) { puts("FAIL fields"); ok = 0; }
    free(r);

    /* Past the ring size only the newest records survive, oldest first. */
    recomp_diff_checkpoint_reset();
    for (i = 1; i <= RECOMP_DIFF_RING + 5; i++)
        recomp_diff_checkpoint(i, 0, 0, 0, 0, 0, 0, 0, 0);
    if (recomp_diff_checkpoint_dump(path) != (int)RECOMP_DIFF_RING) { puts("FAIL wrapped count"); ok = 0; }
    r = load(path, (int)RECOMP_DIFF_RING);
    if (!r || r[0] != 6 || r[1] != 6 ||
        r[(RECOMP_DIFF_RING - 1) * 10] != RECOMP_DIFF_RING + 5) { puts("FAIL wrapped order"); ok = 0; }
    free(r);

    /* Trace events: seven-u32 records, with the memory filter applied to
     * READ/WRITE only. */
    recomp_diff_trace_reset();
    recomp_diff_trace_set_memory_filter(0x1000, 0x2000);
    recomp_diff_trace_event(RECOMP_DIFF_TRACE_WRITE, 0x400, 0x1800, 7, 8);
    recomp_diff_trace_event(RECOMP_DIFF_TRACE_WRITE, 0x404, 0x3000, 0, 0);  /* filtered */
    recomp_diff_trace_event(3, 0x408, 0x3000, 1, 2);                        /* not memory */
    if (recomp_diff_trace_dump(path) != 2) { puts("FAIL trace count"); ok = 0; }
    r = NULL;
    {
        FILE *f = fopen(path, "rb");
        uint8_t raw[56];
        if (!f || fread(raw, 1, 56, f) != 56) { puts("FAIL trace read"); ok = 0; }
        else if (raw[0] != 1 || raw[8] != 0x00 || raw[9] != 0x04 || raw[12] != 6 ||
                 raw[16] != 0x00 || raw[17] != 0x18 || raw[28] != 2 || raw[40] != 3) {
            puts("FAIL trace fields"); ok = 0;
        }
        if (f) fclose(f);
    }

    DeleteFileA(path);
    if (!ok) return 1;
    puts("PASS: checkpoint and trace rings record, dump little-endian, and wrap");
    return 0;
}
