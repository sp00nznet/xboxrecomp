#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wma_decoder.h"

#if defined(_WIN32)
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

static int inject_read_error;
static int read_count;
static DWORD first_sample_bytes;

static HRESULT read_sample(IMFSourceReader *reader, DWORD stream, DWORD control,
                           DWORD *actual, DWORD *flags, LONGLONG *timestamp,
                           IMFSample **sample)
{
    HRESULT hr;
    if (inject_read_error && ++read_count > 2)
        return E_UNEXPECTED;
    hr = IMFSourceReader_ReadSample(reader, stream, control, actual, flags,
                                   timestamp, sample);
    if (inject_read_error && SUCCEEDED(hr)) {
        if (read_count == 1 && *sample)
            IMFSample_GetTotalLength(*sample, &first_sample_bytes);
        if (read_count == 2)
            *flags |= MF_SOURCE_READERF_ERROR;
    }
    return hr;
}

/* Keep the real decoder and Media Foundation; inject only a read status flag. */
#undef IMFSourceReader_ReadSample
#define IMFSourceReader_ReadSample read_sample
#endif
#include "../../src/audio/wma_decoder.c"

static int is_cleared(const XboxWmaPcm *pcm)
{
    return pcm->data == NULL && pcm->size == 0 && pcm->sample_rate == 0 &&
           pcm->channels == 0 && pcm->bits_per_sample == 0;
}

int main(int argc, char **argv)
{
    XboxWmaPcm pcm;

    /* Even argument-validation failures must honor the public failure contract
     * and clear a caller-provided result structure. */
    memset(&pcm, 0xA5, sizeof(pcm));
    if (xbox_wma_decode_file(NULL, &pcm) != -1) {
        fprintf(stderr, "NULL input unexpectedly decoded\n");
        return 1;
    }
    if (!is_cleared(&pcm)) {
        fprintf(stderr, "NULL-path failure did not leave a cleared result\n");
        return 1;
    }

    memset(&pcm, 0xA5, sizeof(pcm));
    if (xbox_wma_decode_file("__xboxrecomp_missing_wma_file__.wma", &pcm) != -1) {
        fprintf(stderr, "missing input unexpectedly decoded\n");
        return 1;
    }

    if (!is_cleared(&pcm)) {
        fprintf(stderr, "failed decode did not leave a cleared result\n");
        return 1;
    }

    /* Cleanup must remain idempotent after a failed decode. */
    xbox_wma_pcm_free(&pcm);
    xbox_wma_pcm_free(&pcm);
    if (pcm.data != NULL || pcm.size != 0) {
        fprintf(stderr, "cleanup did not remain zeroed\n");
        return 1;
    }

#if defined(_WIN32)
    if (argc != 2 || xbox_wma_decode_file(argv[1], &pcm) != 0) {
        fprintf(stderr, "fixture did not decode\n");
        return 1;
    }
    if (!pcm.data || pcm.size == 0 || pcm.size % 4 != 0 ||
        pcm.sample_rate != 44100 || pcm.channels != 2 || pcm.bits_per_sample != 16) {
        fprintf(stderr, "decoded fixture has invalid PCM or format metadata\n");
        xbox_wma_pcm_free(&pcm);
        return 1;
    }
    {
        size_t i;
        for (i = 0; i < pcm.size && pcm.data[i] == 0; ++i) {}
        if (i == pcm.size) {
            fprintf(stderr, "decoded sine fixture is silent\n");
            xbox_wma_pcm_free(&pcm);
            return 1;
        }
    }
    xbox_wma_pcm_free(&pcm);
    xbox_wma_pcm_free(&pcm);

    inject_read_error = 1;
    if (xbox_wma_decode_file(argv[1], &pcm) != -1 || !is_cleared(&pcm) ||
        read_count != 2 || first_sample_bytes == 0) {
        fprintf(stderr, "read error did not stop decoding and clear partial PCM\n");
        xbox_wma_pcm_free(&pcm);
        return 1;
    }
#else
    (void)argc;
    (void)argv;
#endif
    return 0;
}
