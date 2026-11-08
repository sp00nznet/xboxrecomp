#ifndef NV2A_BACKEND_FIXTURE_H
#define NV2A_BACKEND_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

#include "nv2a_backend.h"

#define NV2A_BACKEND_FIXTURE_MAX_EVENTS 64
#define NV2A_BACKEND_FIXTURE_MAX_VERTICES 16

typedef enum {
    NV2A_BACKEND_FIXTURE_CLEAR = 1,
    NV2A_BACKEND_FIXTURE_DRAW,
    NV2A_BACKEND_FIXTURE_PRESENT_SYNC,
    NV2A_BACKEND_FIXTURE_FLIP,
} Nv2aBackendFixtureEventType;

typedef struct {
    Nv2aBackendFixtureEventType type;
    Nv2aSurface surface;
    Nv2aRenderState state;
    uint32_t clear_flags;
    uint32_t clear_argb;
    uint32_t clear_zstencil;
    uint32_t count;
    Nv2aVertex vertices[NV2A_BACKEND_FIXTURE_MAX_VERTICES];
    int textured;
    Nv2aTexture texture;
} Nv2aBackendFixtureEvent;

struct Nv2aBackendFixture;
typedef void (*Nv2aBackendFixtureObserver)(
    struct Nv2aBackendFixture *fixture,
    const Nv2aBackendFixtureEvent *event,
    void *opaque);

typedef struct Nv2aBackendFixture {
    Nv2aBackend backend;
    unsigned draws;
    unsigned clears;
    unsigned syncs;
    unsigned flips;
    Nv2aSurface surf;
    Nv2aRenderState rs;
    uint32_t count;
    Nv2aVertex v[NV2A_BACKEND_FIXTURE_MAX_VERTICES];
    int textured;
    Nv2aTexture texture;
    uint32_t clear_flags;
    uint32_t clear_argb;
    uint32_t clear_zstencil;
    Nv2aBackendFixtureEvent events[NV2A_BACKEND_FIXTURE_MAX_EVENTS];
    size_t event_count;
    unsigned dropped_events;
    unsigned thread_mismatches;
    Nv2aBackendFixtureObserver observer;
    void *observer_opaque;
} Nv2aBackendFixture;

void nv2a_backend_fixture_init(Nv2aBackendFixture *fixture);
void nv2a_backend_fixture_reset(Nv2aBackendFixture *fixture);
void nv2a_backend_fixture_set_observer(
    Nv2aBackendFixture *fixture,
    Nv2aBackendFixtureObserver observer,
    void *opaque);
const Nv2aBackend *nv2a_backend_fixture_bind(Nv2aBackendFixture *fixture);
void nv2a_backend_fixture_present_sync(void);

#endif
