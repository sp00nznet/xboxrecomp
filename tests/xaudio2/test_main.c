/* Windows SDK only; no audio device required.
 * cmake -S tests/xaudio2 -B build/xaudio2-test
 * cmake --build build/xaudio2-test --config Debug
 * ctest --test-dir build/xaudio2-test -C Debug --output-on-failure
 */
#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>
#include <stdio.h>
#include <string.h>

static HRESULT com_result, create_result, master_result, source_result;
static HRESULT start_result, submit_result;
static int com_refs, releases, master_destroys, source_destroys, failures;
static IXAudio2 engine;
static IXAudio2MasteringVoice master;
static IXAudio2SourceVoice source;
static IXAudio2MasteringVoiceVtbl master_vtable;
static IXAudio2SourceVoiceVtbl source_vtable;
/* The fake voice holds exactly as many buffers as the backend's ring, so a
 * submit into a full queue is a ring overwrite. Checked against XA2_NUM_BUFS
 * after the backend is included below. */
#define TEST_QUEUE 12
static const BYTE *queued[TEST_QUEUE];
static BYTE snapshots[TEST_QUEUE][4096];
static UINT32 queued_bytes[TEST_QUEUE], queue_count;

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); failures++; \
} } while (0)

static HRESULT fake_com_init(void)
{
    if (SUCCEEDED(com_result)) com_refs++;
    return com_result;
}

static HRESULT fake_create(IXAudio2 **out)
{
    if (SUCCEEDED(create_result)) *out = &engine;
    return create_result;
}

static HRESULT fake_master(IXAudio2MasteringVoice **out)
{
    if (SUCCEEDED(master_result)) *out = &master;
    return master_result;
}

static HRESULT fake_source(IXAudio2SourceVoice **out)
{
    if (SUCCEEDED(source_result)) *out = &source;
    return source_result;
}

static void STDMETHODCALLTYPE destroy_master(IXAudio2MasteringVoice *voice)
{
    CHECK(voice == &master);
    master_destroys++;
}

static void STDMETHODCALLTYPE destroy_source(IXAudio2SourceVoice *voice)
{
    CHECK(voice == &source);
    source_destroys++;
    queue_count = 0;
}

static void check_queued(void)
{
    for (UINT32 i = 0; i < queue_count; i++)
        CHECK(memcmp(queued[i], snapshots[i], queued_bytes[i]) == 0);
}

static void fake_state(XAUDIO2_VOICE_STATE *state)
{
    memset(state, 0, sizeof(*state));
    state->BuffersQueued = queue_count;
}

static HRESULT fake_submit(const XAUDIO2_BUFFER *buffer)
{
    check_queued();
    if (FAILED(submit_result)) return submit_result;
    CHECK(queue_count < TEST_QUEUE);
    if (queue_count == TEST_QUEUE) return E_FAIL;
    queued[queue_count] = buffer->pAudioData;
    queued_bytes[queue_count] = buffer->AudioBytes;
    memcpy(snapshots[queue_count], buffer->pAudioData, buffer->AudioBytes);
    queue_count++;
    return S_OK;
}

/* Replace only external APIs; compile the real backend, including its ring. */
#define CoInitializeEx(...) fake_com_init()
#define CoUninitialize() ((void)--com_refs)
#define XAudio2Create(out, ...) fake_create(out)
#undef IXAudio2_CreateMasteringVoice
#define IXAudio2_CreateMasteringVoice(engine, out, ...) fake_master(out)
#undef IXAudio2_CreateSourceVoice
#define IXAudio2_CreateSourceVoice(engine, out, ...) fake_source(out)
#undef IXAudio2_Release
#define IXAudio2_Release(...) ((void)++releases)
#undef IXAudio2SourceVoice_Start
#define IXAudio2SourceVoice_Start(...) start_result
#undef IXAudio2SourceVoice_Stop
#define IXAudio2SourceVoice_Stop(...) ((void)0)
#undef IXAudio2SourceVoice_FlushSourceBuffers
#define IXAudio2SourceVoice_FlushSourceBuffers(...) ((void)0)
#undef IXAudio2SourceVoice_GetState
#define IXAudio2SourceVoice_GetState(source, state, ...) fake_state(state)
#undef IXAudio2SourceVoice_SubmitSourceBuffer
#define IXAudio2SourceVoice_SubmitSourceBuffer(source, buffer, ...) fake_submit(buffer)
#include "../../src/apu/apu_xaudio2.c"

_Static_assert(TEST_QUEUE == XA2_NUM_BUFS, "fake queue must match the ring");

static void reset(void)
{
    /* Fake resources are static, so even a broken cleanup can be reset. */
    g_xa2 = NULL;
    g_xa2_master = NULL;
    g_xa2_source = NULL;
    g_xa2_initialized = 0;
    com_refs = releases = master_destroys = source_destroys = 0;
    queue_count = 0;
    com_result = create_result = master_result = source_result = S_OK;
    start_result = submit_result = S_OK;
}

static void check_init_failures(HRESULT com_hr)
{
    HRESULT *stages[] = { &create_result, &master_result, &source_result, &start_result };
    for (int stage = 0; stage < 4; stage++) {
        reset();
        com_result = com_hr;
        *stages[stage] = E_FAIL;
        CHECK(xa2_init() == 0);
        CHECK(!xa2_is_active());
        CHECK(com_refs == 0);
        CHECK(releases == (stage >= 1));
        CHECK(master_destroys == (stage >= 2));
        CHECK(source_destroys == (stage >= 3));
        CHECK(!g_xa2 && !g_xa2_master && !g_xa2_source);
        xa2_shutdown();
        CHECK(releases == (stage >= 1));
    }
}

int main(void)
{
    master_vtable.DestroyVoice = destroy_master;
    source_vtable.DestroyVoice = destroy_source;
    master.lpVtbl = &master_vtable;
    source.lpVtbl = &source_vtable;
    check_init_failures(S_OK);
    check_init_failures(S_FALSE);
    check_init_failures(RPC_E_CHANGED_MODE);
    reset();
    com_result = E_FAIL;
    CHECK(xa2_init() == 0 && !xa2_is_active() && com_refs == 0);

    reset();
    CHECK(xa2_init() == 1 && xa2_is_active());
    CHECK(xa2_init() == 1 && com_refs == 1);
    int16_t samples[1024][2];
    memset(samples, 1, sizeof(samples));
    CHECK(xa2_submit_samples(&samples[0][0], 1024) == 1);
    submit_result = E_FAIL;
    for (int i = 0; i < 3; i++) {
        memset(samples, 2 + i, sizeof(samples));
        CHECK(xa2_submit_samples(&samples[0][0], 1024) == 0);
        CHECK(g_xa2_next_buf == 1 && g_xa2_frames_written == 1);
        check_queued();
    }
    submit_result = S_OK;
    for (int i = 1; i < XA2_NUM_BUFS; i++)
        CHECK(xa2_submit_samples(&samples[0][0], 1024) == 1);
    CHECK(xa2_submit_samples(&samples[0][0], 1024) == 0);
    CHECK(g_xa2_frames_written == XA2_NUM_BUFS);
    check_queued();
    /* Complete the oldest buffer, then wrap the ring into that free slot. */
    for (UINT32 i = 0; i < XA2_NUM_BUFS - 1; i++) {
        queued[i] = queued[i + 1];
        queued_bytes[i] = queued_bytes[i + 1];
        memcpy(snapshots[i], snapshots[i + 1], queued_bytes[i]);
    }
    queue_count--;
    memset(samples, 9, sizeof(samples));
    CHECK(xa2_submit_samples(&samples[0][0], 1024) == 1);
    check_queued();
    xa2_shutdown();
    CHECK(!xa2_is_active() && releases == 1);
    CHECK(master_destroys == 1 && source_destroys == 1);
    xa2_shutdown();
    CHECK(releases == 1 && master_destroys == 1 && source_destroys == 1);
    printf("XAudio2 regression: %d failures\n", failures);
    return failures ? 1 : 0;
}
