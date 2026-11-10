/* nv2a_bind_shadow.h - what a GPU backend last bound to one command encoder,
 * so a batch binding the same object again skips the call.
 *
 * A heavy frame draws thousands of batches on one vertex ring and a handful
 * of textures and samplers, and each bind is an API call plus the driver's
 * own bookkeeping. An encoder (Metal) keeps its bindings from draw to draw,
 * so a pointer equal to the one last set at that slot of the same encoder is
 * already bound. The pointers are compared, never dereferenced. The backend
 * must guarantee that a compared object outlives the encoder (Metal's command
 * buffer retains what it was given), or a freed object's address coming back
 * as a new object would read as bound.
 *
 * A new encoder starts with nothing bound (NULL at every slot), which is what
 * a fresh shadow says. A slot can also be unknown (NV2A_BIND_UNKNOWN, an
 * address no object has): the next set binds whatever it is given. That is
 * every slot with the shadow off, and every slot after something else bound
 * behind the shadow on the same encoder (nv2a_bind_shadow_drop: a debug
 * draw). */
#ifndef NV2A_BIND_SHADOW_H
#define NV2A_BIND_SHADOW_H

#include <stdint.h>
#include <string.h>

#define NV2A_BIND_SHADOW_STAGES 4
#define NV2A_BIND_UNKNOWN ((const void *)(uintptr_t)1)

struct nv2a_bind_shadow {
    uint64_t enc;              /* encoder serial the slots describe; 0 none */
    const void *vb;            /* vertex buffer 0 */
    const void *tex[NV2A_BIND_SHADOW_STAGES];
    const void *smp[NV2A_BIND_SHADOW_STAGES];
};

static inline void nv2a_bind_shadow_drop(struct nv2a_bind_shadow *s)
{
    int k;
    s->vb = NV2A_BIND_UNKNOWN;
    for (k = 0; k < NV2A_BIND_SHADOW_STAGES; k++)
        s->tex[k] = s->smp[k] = NV2A_BIND_UNKNOWN;
}

/* Before a draw's binds on encoder `enc` (monotonic, never 0). A different
 * encoder starts empty. With the shadow off every slot is unknown, so the
 * draw binds everything, as before the shadow; the encoder is still noted,
 * so turning it on again on that encoder does not take it for a fresh one. */
static inline void nv2a_bind_shadow_begin(struct nv2a_bind_shadow *s, uint64_t enc, int on)
{
    if (!on) {
        s->enc = enc;
        nv2a_bind_shadow_drop(s);
    } else if (s->enc != enc) {
        memset(s, 0, sizeof *s);
        s->enc = enc;
    }
}

/* 1 when *slot does not hold p yet: the caller binds p. The slot then
 * records p either way. */
static inline int nv2a_bind_shadow_set(const void **slot, const void *p)
{
    if (*slot == p)
        return 0;
    *slot = p;
    return 1;
}

#endif
