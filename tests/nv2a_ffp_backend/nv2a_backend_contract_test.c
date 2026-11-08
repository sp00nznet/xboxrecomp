#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend_fixture.h"

#define MEM_BYTES 0x10000u
#define COLOR_AT 0x8000u

static uint8_t s_mem[MEM_BYTES];
static int s_fail;
static unsigned s_order[16];
static size_t s_order_count;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; \
} } while (0)

enum {
    ORDER_CLEAR = 1,
    ORDER_DRAW,
    ORDER_SYNC,
    ORDER_PRESENT,
    ORDER_FLIP,
};

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch)
{
    (void)va;
    (void)pitch;
}
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
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset)
{
    (void)offset;
    return NULL;
}

static void append_order(unsigned value)
{
    CHECK(s_order_count < sizeof(s_order) / sizeof(s_order[0]));
    if (s_order_count < sizeof(s_order) / sizeof(s_order[0]))
        s_order[s_order_count++] = value;
}

void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch)
{
    CHECK(va == COLOR_AT);
    CHECK(pitch == 64);
    append_order(ORDER_PRESENT);
}

static void observe_backend(Nv2aBackendFixture *fixture,
                            const Nv2aBackendFixtureEvent *event,
                            void *opaque)
{
    (void)fixture;
    (void)opaque;
    switch (event->type) {
    case NV2A_BACKEND_FIXTURE_CLEAR:
        append_order(ORDER_CLEAR);
        break;
    case NV2A_BACKEND_FIXTURE_DRAW:
        append_order(ORDER_DRAW);
        break;
    case NV2A_BACKEND_FIXTURE_PRESENT_SYNC:
        append_order(ORDER_SYNC);
        break;
    case NV2A_BACKEND_FIXTURE_FLIP:
        append_order(ORDER_FLIP);
        break;
    }
}

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method,
                                uint32_t param);

static void method(uint32_t number, uint32_t value)
{
    nv2a_pb_exec_method(0, number, value);
}

static uint32_t f2u(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void method_float(uint32_t number, float value)
{
    method(number, f2u(value));
}

static void put_float(uint32_t address, float value)
{
    memcpy(s_mem + address, &value, sizeof(value));
}

static void configure_executor(void)
{
    static const float matrix[16] = {
        8, 0, 0, 8,
        0, -4, 0, 4,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
    unsigned i;

    method(0x0208, 0x0008);
    method(0x0200, 16u << 16);
    method(0x0204, 8u << 16);
    method(0x020C, 64);
    method(0x0210, COLOR_AT);
    for (i = 0; i < 16; i++)
        method_float(0x0680 + 4 * i, matrix[i]);
    method(0x1E94, 0);

    put_float(0x2000, -1);
    put_float(0x2004, -1);
    put_float(0x2008, 0);
    put_float(0x200C, 1);
    put_float(0x2010, -1);
    put_float(0x2014, 0);
    put_float(0x2018, 0);
    put_float(0x201C, 1);
    put_float(0x2020, 0);
    method(0x1720, 0x2000);
    method(0x1760, 2 | (3 << 4) | (12 << 8));
}

static void draw_triangle(void)
{
    method(0x17FC, 5);
    method(0x1810, 2u << 24);
    method(0x17FC, 0);
}

static int near(float a, float b)
{
    return fabsf(a - b) < 1e-3f;
}

int main(void)
{
    Nv2aBackendFixture first, replacement;
    Nv2aBackendFixtureEvent first_draw;
    size_t first_events;

    configure_executor();
    nv2a_backend_fixture_init(&first);
    nv2a_backend_fixture_set_observer(&first, observe_backend, NULL);
    nv2a_backend_register_with_present_sync(
        nv2a_backend_fixture_bind(&first),
        nv2a_backend_fixture_present_sync);

    method(0x1D90, 0xFF102030u);
    method(0x1D94, 0xF0);
    draw_triangle();
    method(0x0130, 0);

    CHECK(s_order_count == 5);
    CHECK(s_order[0] == ORDER_CLEAR && s_order[1] == ORDER_DRAW);
    CHECK(s_order[2] == ORDER_SYNC && s_order[3] == ORDER_PRESENT);
    CHECK(s_order[4] == ORDER_FLIP);
    CHECK(first.clears == 1 && first.draws == 1);
    CHECK(first.syncs == 1 && first.flips == 1);
    CHECK(first.event_count == 4 && first.dropped_events == 0);
    CHECK(first.thread_mismatches == 0);

    CHECK(first.events[0].type == NV2A_BACKEND_FIXTURE_CLEAR);
    CHECK(first.events[0].surface.color_va == COLOR_AT);
    CHECK(first.events[0].surface.width == 16);
    CHECK(first.events[0].surface.height == 8);
    CHECK(first.events[0].surface.pitch == 64);
    CHECK(first.events[0].surface.bytes_per_pixel == 4);
    CHECK(first.events[0].surface.aa_sx == 1);
    CHECK(first.events[0].surface.aa_sy == 1);
    CHECK(first.events[0].clear_flags == 0xF0);
    CHECK(first.events[0].clear_argb == 0xFF102030u);

    CHECK(first.events[1].type == NV2A_BACKEND_FIXTURE_DRAW);
    CHECK(first.events[1].count == 3);
    CHECK(!first.events[1].textured);
    CHECK(near(first.events[1].vertices[0].x, 0));
    CHECK(near(first.events[1].vertices[0].y, 8));
    CHECK(near(first.events[1].vertices[1].x, 16));
    CHECK(near(first.events[1].vertices[1].y, 8));
    CHECK(near(first.events[1].vertices[2].x, 8));
    CHECK(near(first.events[1].vertices[2].y, 0));

    first_draw = first.events[1];
    put_float(0x2000, 0);
    draw_triangle();
    CHECK(first.draws == 2);
    CHECK(first.events[1].count == first_draw.count);
    CHECK(memcmp(first.events[1].vertices, first_draw.vertices,
                 sizeof(first_draw.vertices)) == 0);
    CHECK(!near(first.events[4].vertices[0].x,
                first_draw.vertices[0].x));

    first_events = first.event_count;
    nv2a_backend_fixture_init(&replacement);
    nv2a_backend_fixture_set_observer(&replacement, observe_backend, NULL);
    nv2a_backend_register(nv2a_backend_fixture_bind(&replacement));
    draw_triangle();
    CHECK(first.event_count == first_events);
    CHECK(replacement.draws == 1 && replacement.event_count == 1);
    CHECK(replacement.events[0].type == NV2A_BACKEND_FIXTURE_DRAW);
    CHECK(replacement.thread_mismatches == 0);

    nv2a_backend_register(NULL);
    first_events = replacement.event_count;
    draw_triangle();
    method(0x0130, 0);
    CHECK(replacement.event_count == first_events);

    if (!s_fail)
        puts("NV2A back-end callback contract: ok");
    return s_fail;
}
