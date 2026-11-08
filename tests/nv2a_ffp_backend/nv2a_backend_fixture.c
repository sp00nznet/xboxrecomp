#include "nv2a_backend_fixture.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef DWORD FixtureThread;
static FixtureThread current_thread(void) { return GetCurrentThreadId(); }
static int same_thread(FixtureThread a, FixtureThread b) { return a == b; }
#else
#include <pthread.h>
typedef pthread_t FixtureThread;
static FixtureThread current_thread(void) { return pthread_self(); }
static int same_thread(FixtureThread a, FixtureThread b)
{
    return pthread_equal(a, b);
}
#endif

static Nv2aBackendFixture *s_active;
static FixtureThread s_expected_thread;
static int s_expected_thread_valid;

static void check_thread(Nv2aBackendFixture *fixture)
{
    if (!s_expected_thread_valid ||
        !same_thread(s_expected_thread, current_thread()))
        fixture->thread_mismatches++;
}

static Nv2aBackendFixtureEvent *new_event(
    Nv2aBackendFixture *fixture,
    Nv2aBackendFixtureEventType type)
{
    Nv2aBackendFixtureEvent *event;

    if (fixture->event_count >= NV2A_BACKEND_FIXTURE_MAX_EVENTS) {
        fixture->dropped_events++;
        return NULL;
    }
    event = &fixture->events[fixture->event_count++];
    memset(event, 0, sizeof(*event));
    event->type = type;
    return event;
}

static void notify(Nv2aBackendFixture *fixture,
                   Nv2aBackendFixtureEvent *event)
{
    if (event && fixture->observer)
        fixture->observer(fixture, event, fixture->observer_opaque);
}

static void fixture_clear(const Nv2aSurface *surface,
                          const Nv2aRenderState *state,
                          uint32_t flags,
                          uint32_t argb,
                          uint32_t zstencil)
{
    Nv2aBackendFixture *fixture = s_active;
    Nv2aBackendFixtureEvent *event;

    if (!fixture)
        return;
    check_thread(fixture);
    fixture->clears++;
    fixture->surf = *surface;
    fixture->rs = *state;
    fixture->clear_flags = flags;
    fixture->clear_argb = argb;
    fixture->clear_zstencil = zstencil;

    event = new_event(fixture, NV2A_BACKEND_FIXTURE_CLEAR);
    if (event) {
        event->surface = *surface;
        event->state = *state;
        event->clear_flags = flags;
        event->clear_argb = argb;
        event->clear_zstencil = zstencil;
    }
    notify(fixture, event);
}

static void fixture_draw(const Nv2aSurface *surface,
                         const Nv2aBatch *batch)
{
    Nv2aBackendFixture *fixture = s_active;
    Nv2aBackendFixtureEvent *event;
    uint32_t copied;

    if (!fixture)
        return;
    check_thread(fixture);
    fixture->draws++;
    fixture->surf = *surface;
    fixture->rs = *batch->state;
    fixture->count = batch->count;
    fixture->textured = batch->texture != NULL;
    if (batch->texture)
        fixture->texture = *batch->texture;
    copied = batch->count < NV2A_BACKEND_FIXTURE_MAX_VERTICES
           ? batch->count : NV2A_BACKEND_FIXTURE_MAX_VERTICES;
    memcpy(fixture->v, batch->vertices, copied * sizeof(*fixture->v));

    event = new_event(fixture, NV2A_BACKEND_FIXTURE_DRAW);
    if (event) {
        event->surface = *surface;
        event->state = *batch->state;
        event->count = batch->count;
        event->textured = batch->texture != NULL;
        if (batch->texture)
            event->texture = *batch->texture;
        memcpy(event->vertices, batch->vertices,
               copied * sizeof(*event->vertices));
    }
    notify(fixture, event);
}

static void fixture_flip(void)
{
    Nv2aBackendFixture *fixture = s_active;
    Nv2aBackendFixtureEvent *event;

    if (!fixture)
        return;
    check_thread(fixture);
    fixture->flips++;
    event = new_event(fixture, NV2A_BACKEND_FIXTURE_FLIP);
    notify(fixture, event);
}

void nv2a_backend_fixture_init(Nv2aBackendFixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->backend.clear = fixture_clear;
    fixture->backend.draw = fixture_draw;
    fixture->backend.flip = fixture_flip;
}

void nv2a_backend_fixture_reset(Nv2aBackendFixture *fixture)
{
    Nv2aBackend backend = fixture->backend;
    Nv2aBackendFixtureObserver observer = fixture->observer;
    void *observer_opaque = fixture->observer_opaque;

    memset(fixture, 0, sizeof(*fixture));
    fixture->backend = backend;
    fixture->observer = observer;
    fixture->observer_opaque = observer_opaque;
}

void nv2a_backend_fixture_set_observer(
    Nv2aBackendFixture *fixture,
    Nv2aBackendFixtureObserver observer,
    void *opaque)
{
    fixture->observer = observer;
    fixture->observer_opaque = opaque;
}

const Nv2aBackend *nv2a_backend_fixture_bind(Nv2aBackendFixture *fixture)
{
    s_active = fixture;
    s_expected_thread = current_thread();
    s_expected_thread_valid = 1;
    return &fixture->backend;
}

void nv2a_backend_fixture_present_sync(void)
{
    Nv2aBackendFixture *fixture = s_active;
    Nv2aBackendFixtureEvent *event;

    if (!fixture)
        return;
    check_thread(fixture);
    fixture->syncs++;
    event = new_event(fixture, NV2A_BACKEND_FIXTURE_PRESENT_SYNC);
    notify(fixture, event);
}
