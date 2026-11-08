#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"
#include "nv2a_backend_fixture.h"

#define MEM_BYTES 0x10000u
#define FB_ADDR 0x8000u
#define OLD_PIXEL 0xFF123456u
#define NEW_PIXEL 0xFFABCDEFu

static uint32_t s_memory[MEM_BYTES / sizeof(uint32_t)];
static uint8_t *const s_mem = (uint8_t *)s_memory;
static int s_fail;
static unsigned s_events[8], s_event_count, s_present_count, s_draw_count;
static uint32_t s_presented[4], s_presented_va[4];
static int s_flip_writes, s_flip_presents;
static Nv2aBackendFixture s_backend;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; \
} } while (0)

enum { EVENT_SYNC = 1, EVENT_PRESENT, EVENT_FLIP };

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

static void event(unsigned value)
{
    CHECK(s_event_count < sizeof s_events / sizeof s_events[0]);
    if (s_event_count < sizeof s_events / sizeof s_events[0])
        s_events[s_event_count++] = value;
}

void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch)
{
    (void)pitch;
    event(EVENT_PRESENT);
    CHECK(s_present_count < sizeof s_presented / sizeof s_presented[0]);
    CHECK(va <= MEM_BYTES - sizeof(uint32_t));
    if (s_present_count < sizeof s_presented / sizeof s_presented[0] &&
        va <= MEM_BYTES - sizeof(uint32_t)) {
        s_presented_va[s_present_count] = va;
        memcpy(&s_presented[s_present_count], s_mem + va, sizeof(uint32_t));
        s_present_count++;
    }
}

static void write_pixel(uint32_t pixel) { memcpy(s_mem + FB_ADDR, &pixel, sizeof pixel); }
static void observe_backend(Nv2aBackendFixture *fixture,
                            const Nv2aBackendFixtureEvent *backend_event,
                            void *opaque)
{
    (void)fixture;
    (void)opaque;
    switch (backend_event->type) {
    case NV2A_BACKEND_FIXTURE_DRAW:
        CHECK(backend_event->surface.color_va == FB_ADDR);
        s_draw_count++;
        break;
    case NV2A_BACKEND_FIXTURE_PRESENT_SYNC:
        event(EVENT_SYNC);
        write_pixel(NEW_PIXEL);
        break;
    case NV2A_BACKEND_FIXTURE_FLIP:
        event(EVENT_FLIP);
        if (s_flip_writes)
            write_pixel(NEW_PIXEL);
        if (s_flip_presents)
            xbox_FramebufferWindowPresent(FB_ADDR, 4);
        break;
    case NV2A_BACKEND_FIXTURE_CLEAR:
        break;
    }
}

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
static void method(uint32_t number, uint32_t value) { nv2a_pb_exec_method(0, number, value); }
static void write_float(uint32_t address, float value) { memcpy(s_mem + address, &value, 4); }

static void reset_events(void)
{
    memset(s_events, 0, sizeof s_events);
    memset(s_presented, 0, sizeof s_presented);
    memset(s_presented_va, 0, sizeof s_presented_va);
    s_event_count = s_present_count = 0;
    s_draw_count = 0;
    s_flip_writes = s_flip_presents = 0;
    nv2a_backend_fixture_reset(&s_backend);
}

static void dump_path(char *path, size_t size, const char *prefix, unsigned sequence)
{
    int n = snprintf(path, size, "%s%05u.bmp", prefix, sequence);
    if (n < 0 || (size_t)n >= size) {
        fprintf(stderr, "framebuffer dump path is too long\n");
        exit(1);
    }
}

static void check_dump(const char *prefix, unsigned sequence, uint32_t expected)
{
    char path[512];
    uint8_t bgr[3];
    dump_path(path, sizeof path, prefix, sequence);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(fseek(f, 54, SEEK_SET) == 0);
    if (fread(bgr, 1, sizeof bgr, f) != sizeof bgr) {
        fprintf(stderr, "cannot read pixel from %s\n", path);
        s_fail = 1;
    } else {
        CHECK(bgr[0] == (expected & 0xFFu));
        CHECK(bgr[1] == ((expected >> 8) & 0xFFu));
        CHECK(bgr[2] == ((expected >> 16) & 0xFFu));
    }
    CHECK(fclose(f) == 0);
    CHECK(remove(path) == 0);
}

static void flip_and_check_dump(const char *prefix, unsigned sequence, uint32_t expected)
{
    char path[512];
    dump_path(path, sizeof path, prefix, sequence);
    FILE *old = fopen(path, "rb");
    if (old) {
        fclose(old);
        fprintf(stderr, "refusing to overwrite an existing dump: %s\n", path);
        exit(1);
    }
    method(0x0130, 0);
    check_dump(prefix, sequence, expected);
}

static void draw_to_first_surface(void)
{
    static const float pos[3][3] = {{0, 0, 0}, {16, 0, 0}, {0, 16, 0}};
    for (unsigned i = 0; i < 3; i++)
        for (unsigned j = 0; j < 3; j++)
            write_float(0x2000 + i * 12u + j * 4u, pos[i][j]);
    method(0x1720, 0x2000);
    method(0x1760, 2 | (3 << 4) | (12 << 8));
    method(0x17FC, 5);
    method(0x1810, 2u << 24);
    method(0x17FC, 0);
    CHECK(s_draw_count == 1);
    method(0x0210, 0x9000);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "expected the framebuffer dump prefix\n");
        return 2;
    }
    nv2a_backend_fixture_init(&s_backend);
    nv2a_backend_fixture_set_observer(&s_backend, observe_backend, NULL);
    method(0x0208, 0x0008);
    method(0x0200, 1u << 16);
    method(0x0204, 1u << 16);
    method(0x020C, 4);
    method(0x0210, FB_ADDR);

    reset_events();
    write_pixel(OLD_PIXEL);
    flip_and_check_dump(argv[1], 0, OLD_PIXEL);
    CHECK(s_event_count == 1 && s_events[0] == EVENT_PRESENT);
    CHECK(s_present_count == 1 && s_presented[0] == OLD_PIXEL);

    nv2a_backend_register(nv2a_backend_fixture_bind(&s_backend));
    reset_events();
    write_pixel(OLD_PIXEL);
    s_flip_writes = s_flip_presents = 1;
    flip_and_check_dump(argv[1], 1, OLD_PIXEL);
    CHECK(s_event_count == 3 && s_events[0] == EVENT_PRESENT &&
          s_events[1] == EVENT_FLIP && s_events[2] == EVENT_PRESENT);
    CHECK(s_present_count == 2 && s_presented[0] == OLD_PIXEL && s_presented[1] == NEW_PIXEL);

    nv2a_backend_register_with_present_sync(
        nv2a_backend_fixture_bind(&s_backend),
        nv2a_backend_fixture_present_sync);
    reset_events();
    write_pixel(OLD_PIXEL);
    draw_to_first_surface();
    flip_and_check_dump(argv[1], 2, NEW_PIXEL);
    CHECK(s_event_count == 3 && s_events[0] == EVENT_SYNC &&
          s_events[1] == EVENT_PRESENT && s_events[2] == EVENT_FLIP);
    CHECK(s_present_count == 1 && s_presented_va[0] == FB_ADDR && s_presented[0] == NEW_PIXEL);

    nv2a_backend_register_with_present_sync(
        nv2a_backend_fixture_bind(&s_backend), NULL);
    reset_events();
    write_pixel(OLD_PIXEL);
    flip_and_check_dump(argv[1], 3, OLD_PIXEL);
    CHECK(s_event_count == 2 && s_events[0] == EVENT_PRESENT && s_events[1] == EVENT_FLIP);
    CHECK(s_present_count == 1 && s_presented[0] == OLD_PIXEL);

    nv2a_backend_register_with_present_sync(
        nv2a_backend_fixture_bind(&s_backend),
        nv2a_backend_fixture_present_sync);
    nv2a_backend_register(nv2a_backend_fixture_bind(&s_backend));
    reset_events();
    write_pixel(OLD_PIXEL);
    flip_and_check_dump(argv[1], 4, OLD_PIXEL);
    CHECK(s_event_count == 2 && s_events[0] == EVENT_PRESENT && s_events[1] == EVENT_FLIP);
    CHECK(s_present_count == 1 && s_presented[0] == OLD_PIXEL);

    nv2a_backend_register_with_present_sync(
        NULL, nv2a_backend_fixture_present_sync);
    reset_events();
    write_pixel(OLD_PIXEL);
    flip_and_check_dump(argv[1], 5, OLD_PIXEL);
    CHECK(s_event_count == 1 && s_events[0] == EVENT_PRESENT);
    CHECK(s_present_count == 1 && s_presented[0] == OLD_PIXEL);

    nv2a_backend_register_with_present_sync(
        nv2a_backend_fixture_bind(&s_backend),
        nv2a_backend_fixture_present_sync);
    nv2a_backend_register(NULL);
    reset_events();
    write_pixel(OLD_PIXEL);
    flip_and_check_dump(argv[1], 6, OLD_PIXEL);
    CHECK(s_event_count == 1 && s_events[0] == EVENT_PRESENT);
    CHECK(s_present_count == 1 && s_presented[0] == OLD_PIXEL);
    CHECK(s_backend.thread_mismatches == 0 && s_backend.dropped_events == 0);

    if (!s_fail)
        puts("NV2A present synchronization: ok");
    return s_fail;
}
