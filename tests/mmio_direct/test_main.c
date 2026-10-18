/* A constant-address mov into the APU window reaches the model without a
 * fault, and only when the same access would have faulted into it.
 *
 * The lifter turns `mov eax, [0xFE820010]` and its store and narrow forms
 * into xbox_MmioRead/xbox_MmioWrite (tools/recomp/test_lifter_mmio_direct.py).
 * Under Proton each trapped access was a Wine exception dispatch of about
 * 51 us, and DirectSound makes thousands a second. This pins the runtime half:
 *
 *   untrapped (RECOMP_AC97_READY=0): every access is plain RAM, model or not;
 *   trapped, model up: widths 1, 2 and 4 hand the model what the fault path's
 *     apu_mmio_rd/_wr would (the same call on a reference state), up to the
 *     last dword of the trap (0x2FFFC), and RECOMP_APU_TRACE lists them;
 *   trapped, past the trap (0x30000, the GP DSP memory): plain RAM, not the
 *     model.
 *
 * A trapped access with the model down still faults, as it did; that is the
 * fault handler's business and not exercised here. */
#include "xbox_memory_layout.h"
#include "recomp_env.h"
#include "apu_state.h"
#include "apu.h"
#include "apu_regs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

extern MCPXAPUState *g_apu_state;

#define APU 0xFE800000u

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } \
         else printf("  ok: %s\n", name); } while (0)

static unsigned char *load_xbe(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long len;
    unsigned char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    rewind(f);
    buf = len > 0 ? malloc((size_t)len) : NULL;
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f); free(buf); return NULL;
    }
    fclose(f);
    *out_len = (size_t)len;
    return buf;
}

static MCPXAPUState *new_state(void)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    if (!d) exit(2);
    qemu_mutex_init(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);
    return d;
}

static volatile uint32_t *host32(uint32_t va)
{
    return (volatile uint32_t *)((char *)xbox_GetMemoryOffset() + va);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tools/conformance/test.xbe";
    size_t n = 0;
    unsigned char *xbe = load_xbe(path, &n);
    MCPXAPUState *d = new_state(), *ref = new_state();
    char log[] = "/tmp/mmio_direct_XXXXXX";
    int fd, saved;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!xbe) {
        fprintf(stderr, "cannot read %s\n", path);
        return 2;
    }

    /* 1. Untrapped: plain memory even with a model present. */
    recomp_env_set(RENV_AC97_READY, NULL);
    if (!xbox_MemoryLayoutInit(xbe, n)) { printf("FAIL: init\n"); return 1; }
    g_apu_state = d;
    xbox_MmioWrite(APU + NV_PAPU_IEN, 0x12345678u, 4);
    CHECK("untrapped store lands in RAM",
          *host32(APU + NV_PAPU_IEN) == 0x12345678u);
    CHECK("untrapped store skips the model", d->regs[NV_PAPU_IEN] == 0);
    CHECK("untrapped dword load reads RAM",
          xbox_MmioRead(APU + NV_PAPU_IEN, 4) == 0x12345678u);
    CHECK("untrapped word load reads RAM",
          (uint16_t)xbox_MmioRead(APU + NV_PAPU_IEN + 2, 2) == 0x1234u);
    xbox_MmioWrite(APU + NV_PAPU_IEN + 1, 0xABu, 1);
    CHECK("untrapped byte store is one byte",
          *host32(APU + NV_PAPU_IEN) == 0x1234AB78u);
    g_apu_state = NULL;
    xbox_MemoryLayoutShutdown();

    /* 2. Trapped, model up. stderr goes to a file so the trace listing can be
     *    read back. */
    recomp_env_set(RENV_AC97_READY, "1");
    recomp_env_set(RENV_APU_TRACE, "1");
    fd = mkstemp(log);
    if (fd < 0) return 2;
    fflush(stderr);
    saved = dup(2);
    dup2(fd, 2);
    if (!xbox_MemoryLayoutInit(xbe, n)) {
        dup2(saved, 2);
        printf("FAIL: trapped init\n");
        return 1;
    }
    g_apu_state = d;

    static const struct { uint32_t off, v; int size; } w[] = {
        { NV_PAPU_IEN, 0xC0FFEE11u, 4 },
        { NV_PAPU_IEN, 0x0000BEEFu, 2 },
        { NV_PAPU_IEN, 0x000000D9u, 1 },
    };
    for (unsigned i = 0; i < sizeof w / sizeof w[0]; i++) {
        char name[64];
        uint32_t got, want;
        uint32_t v = w[i].size == 1 ? (uint8_t)w[i].v
                   : w[i].size == 2 ? (uint16_t)w[i].v : w[i].v;
        xbox_MmioWrite(APU + w[i].off, v, w[i].size);
        mcpx_apu_mmio_write(ref, w[i].off, v, (unsigned)w[i].size);
        snprintf(name, sizeof name, "trapped store width %d reaches the model",
                 w[i].size);
        CHECK(name, d->regs[w[i].off] == ref->regs[w[i].off]);
        got = xbox_MmioRead(APU + w[i].off, w[i].size);
        want = (uint32_t)mcpx_apu_mmio_read(ref, w[i].off, (unsigned)w[i].size);
        snprintf(name, sizeof name, "trapped load width %d reads the model",
                 w[i].size);
        CHECK(name, got == want);
    }
    /* PIO_FREE: the voice processor answers, as it does through the fault. */
    CHECK("PIO_FREE reads the model's free count",
          xbox_MmioRead(APU + 0x20010u, 4)
              == (uint32_t)mcpx_apu_mmio_read(ref, 0x20010u, 4));
    (void)xbox_MmioRead(APU + 0x2FFFCu, 4);   /* last dword of the trap */

    /* Past the trap: the GP DSP memory is RAM. */
    xbox_MmioWrite(APU + 0x30000u, 0xDEADBEEFu, 4);
    CHECK("0x30000 store lands in RAM", *host32(APU + 0x30000u) == 0xDEADBEEFu);
    CHECK("0x30000 load reads RAM", xbox_MmioRead(APU + 0x30000u, 4) == 0xDEADBEEFu);

    g_apu_state = NULL;
    xbox_MemoryLayoutShutdown();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);

    {
        FILE *f = fopen(log, "r");
        char line[256];
        int ien_w = 0, ien_r = 0, pio = 0, last = 0, past = 0;
        while (f && fgets(line, sizeof line, f)) {
            if (strstr(line, "[APUMMIO] write 0x01004")) ien_w++;
            if (strstr(line, "[APUMMIO] read  0x01004")) ien_r++;
            if (strstr(line, "[APUMMIO] read  0x20010 = ")) pio++;
            if (strstr(line, "[APUMMIO] read  0x2FFFC")) last++;
            if (strstr(line, "0x30000")) past += strstr(line, "[APUMMIO]") != NULL;
        }
        if (f) fclose(f);
        unlink(log);
        CHECK("trace lists the three direct stores", ien_w == 3);
        CHECK("trace lists the three direct loads", ien_r == 3);
        CHECK("trace lists PIO_FREE and 0x2FFFC", pio == 1 && last == 1);
        CHECK("trace does not list 0x30000", past == 0);
    }

    printf("mmio_direct: %s (%d failure(s))\n", failures ? "FAIL" : "PASS",
           failures);
    return failures ? 1 : 0;
}
