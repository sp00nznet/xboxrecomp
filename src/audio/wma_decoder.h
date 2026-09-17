#ifndef XBOXRECOMP_WMA_DECODER_H
#define XBOXRECOMP_WMA_DECODER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XboxWmaPcm {
    uint8_t *data;
    size_t size;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
} XboxWmaPcm;

/* Decode an ordinary WMA/ASF file to interleaved PCM using the host decoder.
 * Returns 0 on success and -1 on failure. The caller owns out->data and must
 * release it with xbox_wma_pcm_free(). */
int xbox_wma_decode_file(const char *path, XboxWmaPcm *out);

void xbox_wma_pcm_free(XboxWmaPcm *pcm);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_WMA_DECODER_H */
