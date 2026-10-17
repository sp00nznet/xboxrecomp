/* The display mode's frame size: the AV mode word's timing byte through
 * xbox_AvModeSize, and the record AvSetDisplayMode leaves for the present
 * pick (xbox_SetDisplayMode / xbox_GetDisplayMode). The present extent is
 * the mode's frame, so a wrong row here is a wrong-sized picture. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* This kernel-only test must never dispatch guest code. */
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

static int s_fail;

static void check_size(ULONG mode, ULONG want_w, ULONG want_h)
{
    ULONG w = 1, h = 1;
    int known = xbox_AvModeSize(mode, &w, &h);
    int ok = w == want_w && h == want_h && known == (want_w != 0);
    printf("%s mode 0x%08lX -> %lux%lu (want %lux%lu)\n", ok ? "PASS" : "FAIL",
           (unsigned long)mode, (unsigned long)w, (unsigned long)h,
           (unsigned long)want_w, (unsigned long)want_h);
    s_fail |= !ok;
}

/* The [AV] lines a mode set writes to stderr. */
static int av_lines(uint32_t fb, uint32_t pitch, uint32_t mode, uint32_t format)
{
    char buf[4096];
    FILE *tmp = tmpfile();
    int saved = dup(2), n = 0;
    char *p;
    size_t len;

    fflush(stderr);
    dup2(fileno(tmp), 2);
    xbox_SetDisplayMode(fb, pitch, mode, format);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    rewind(tmp);
    len = fread(buf, 1, sizeof buf - 1, tmp);
    buf[len] = 0;
    fclose(tmp);
    for (p = buf; (p = strstr(p, "[AV]")) != NULL; p++)
        n++;
    return n;
}

static void check_record(const char *what, uint32_t fb, uint32_t pitch,
                         uint32_t want_w, uint32_t want_h)
{
    uint32_t p = 0, w = 1, h = 1, va = xbox_GetDisplayMode(&p, &w, &h);
    int ok = va == fb && p == pitch && w == want_w && h == want_h;
    printf("%s %s: fb 0x%08X pitch %u size %ux%u (want 0x%08X %u %ux%u)\n",
           ok ? "PASS" : "FAIL", what, va, p, w, h, fb, pitch, want_w, want_h);
    s_fail |= !ok;
}

int main(void)
{
    uint32_t gen;
    int n;

    check_size(0x0801010D, 640, 480);     /* HDTV pack, NTSC-M 480i 4:3 */
    check_size(0x080F0D12, 640, 480);     /* HDTV pack, 480p 4:3 */
    check_size(0x0802020E, 720, 480);
    check_size(0x880B0A02, 1280, 720);
    check_size(0x880E0C03, 1920, 1080);
    check_size(0x48050516, 640, 576);     /* PAL */
    check_size(0x08017F0D, 0, 0);         /* a timing byte the table lacks */

    /* Nothing is recorded before a mode set. */
    check_record("before any mode", 0, 0, 0, 0);

    gen = xbox_DisplayModeGeneration();
    n = av_lines(0x805E8000, 2560, 0x080F0D12, 0x12);
    xbox_DisplayModeBump();
    check_record("480p mode", 0x805E8000, 2560, 640, 480);
    printf("%s 480p mode: %d [AV] lines (want 0), generation %u -> %u\n",
           n == 0 && xbox_DisplayModeGeneration() == gen + 1 ? "PASS" : "FAIL",
           n, gen, xbox_DisplayModeGeneration());
    s_fail |= n != 0 || xbox_DisplayModeGeneration() != gen + 1;

    /* A field title passes half its buffers' pitch: one frame line. */
    n = av_lines(0x80104000, 1280, 0x0801010D, 0x12);
    check_record("field mode", 0x80104000, 1280, 640, 480);
    printf("%s field mode: %d [AV] lines (want 0)\n", n == 0 ? "PASS" : "FAIL", n);
    s_fail |= n != 0;

    /* A pitch the mode's width disagrees with: one line, no size (the
     * present keeps the clip), the address and pitch still recorded. */
    n = av_lines(0x80104000, 3200, 0x0801010D, 0x12);
    check_record("pitch disagrees", 0x80104000, 3200, 0, 0);
    printf("%s pitch disagrees: %d [AV] lines (want 1)\n", n == 1 ? "PASS" : "FAIL", n);
    s_fail |= n != 1;

    /* An unknown word: one line, no size, the address still recorded. */
    n = av_lines(0x80104000, 2560, 0x08017F0D, 0x12);
    check_record("unknown word", 0x80104000, 2560, 0, 0);
    printf("%s unknown word: %d [AV] lines (want 1)\n", n == 1 ? "PASS" : "FAIL", n);
    s_fail |= n != 1;

    /* 720p, then the address-only setter, which carries no size. */
    n = av_lines(0x80200000, 5120, 0x880B0A02, 0x12);
    check_record("720p mode", 0x80200000, 5120, 1280, 720);
    xbox_SetDisplayFramebuffer(0x80300000, 2560);
    check_record("address only", 0x80300000, 2560, 0, 0);

    puts(s_fail ? "FAIL: kernel_display_mode" : "PASS: kernel_display_mode");
    return s_fail;
}
