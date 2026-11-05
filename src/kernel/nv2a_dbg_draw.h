/*
 * nv2a_dbg_draw.h - the mods' 3D debug draw (mod API version 8, draw3d):
 * world-space triangles and lines a mod submits with the view and
 * projection they are seen through, drawn by the GPU backends at the flip
 * into the presented frame, depth-tested against the game's own depth
 * buffer or not (x-ray), alpha-blended.
 *
 * The guest thread (a mod callback) fills one list a frame:
 * nv2a_dbg_begin, then nv2a_dbg_add any number of times, then
 * nv2a_dbg_publish. Each vertex is taken to clip space here, on the CPU,
 * so the backends' shaders are a pass-through and this module is testable
 * without a GPU (tests/dbg_draw). The flip (the render thread) takes the
 * latest published list with nv2a_dbg_take and keeps drawing it until a
 * newer one arrives; an empty published list clears the drawing. Lists go
 * through a lock-free triple buffer, so neither side ever waits.
 *
 * It lives in the kernel library, not in src/enhance, because the backends
 * use it and the layer links the kernel, never the other way round (as the
 * batch filter and the HUD capture glue). Only XBOXRECOMP_ENHANCE builds
 * carry it; before the first publish the flip's check is one atomic load.
 *
 * Matrices are row-vector D3D: clip = [x y z 1] . view . proj, clip z in
 * [0, w], NDC y up. The backends store depth as normalised z (z / zmax),
 * which is the projection's NDC z for a title whose viewport z scale is
 * zmax with no offset, so the clip z compares with the game's depth as is.
 */
#ifndef NV2A_DBG_DRAW_H
#define NV2A_DBG_DRAW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One vertex as the backends read it: clip space, then the colour as
 * bytes r, g, b, a (DXGI R8G8B8A8_UNORM, Metal uchar4). 20 bytes. */
struct nv2a_dbg_vtx {
    float x, y, z, w;
    uint8_t rgba[4];
};

/* The segments, in the order the backends draw them: what hides things
 * first, then what is blended over them, the x-ray parts last so they
 * show over everything. */
enum {
    NV2A_DBG_TRI_OPAQUE = 0,    /* depth test and write */
    NV2A_DBG_LINE,              /* depth test */
    NV2A_DBG_TRI_BLEND,         /* depth test, no write */
    NV2A_DBG_TRI_XRAY,          /* no test, no write */
    NV2A_DBG_LINE_XRAY,
    NV2A_DBG_SEGS
};

/* nv2a_dbg_add's flags (mod.h XBOX_DBG_* are the same bits). */
#define NV2A_DBG_XRAY   0x1u    /* no depth test */
#define NV2A_DBG_OPAQUE 0x2u    /* triangles: write depth (alpha is still blended) */
#define NV2A_DBG_PULL   0x4u    /* pulled toward the eye, so lines on faces win */
#define NV2A_DBG_FLAGS  0x7u

/* The view-space pull of NV2A_DBG_PULL: the point moves this fraction of
 * its distance toward the eye: enough to lift an edge off its own faces
 * at any depth, too little to move it in front of other geometry. */
#define NV2A_DBG_PULL_K 0.002f

/* The most vertices one list holds over all segments; past it nv2a_dbg_add
 * drops the primitives that do not fit and counts them. */
#define NV2A_DBG_MAX_VTX (1u << 20)

struct nv2a_dbg_list {
    struct nv2a_dbg_vtx *v[NV2A_DBG_SEGS];
    uint32_t n[NV2A_DBG_SEGS];          /* vertices, 3 a triangle, 2 a line */
    uint32_t cap[NV2A_DBG_SEGS];
    uint32_t total;                     /* sum of n */
    uint32_t dropped;                   /* primitives over NV2A_DBG_MAX_VTX */
    uint32_t serial;                    /* the publish count when published */
    float view[16], proj[16];
    int open;                           /* between begin and publish */
};

/* The guest side (one producer). begin starts the frame's list with these
 * matrices (16 floats each, row-major, row-vector convention); a second
 * begin before publish keeps the list and its matrices and returns 0. */
int nv2a_dbg_begin(const float view[16], const float proj[16]);
/* n primitives (3n vertices for triangles, 2n for lines) of xyz + colour
 * 0xAARRGGBB, stride bytes apart (at least 16: three floats and the
 * colour). Returns the primitives added; 0 outside begin/publish, for bad
 * flags or with nothing to add. */
int nv2a_dbg_add(int lines, const void *verts, uint32_t stride, uint32_t n, uint32_t flags);
/* Hands the list to the flip; the next begin starts an empty one. Returns
 * the list's vertex count, or -1 when no list is open. */
int nv2a_dbg_publish(void);
/* Drops the open list unpublished (its producer went away before
 * publish); the next begin starts an empty one. */
void nv2a_dbg_cancel(void);

/* The render side (one consumer, at each flip): the latest published
 * list, NULL before the first publish. A list with total 0 draws nothing.
 * The pointer stays valid until the next take. */
const struct nv2a_dbg_list *nv2a_dbg_take(void);

/* What a backend multiplies clip x by so the guest's 0..w lands in the
 * middle of a hor+ target (w + 2 pad wide): w / (w + 2 pad). */
static inline float nv2a_dbg_xscale(uint32_t w, uint32_t pad)
{
    return w ? (float)w / (float)(w + 2u * pad) : 1.0f;
}

/* Any thread: stop drawing until the next publish (a mod turned off
 * mid-frame; its later calls are refused, so nothing republishes it). */
void nv2a_dbg_hide(void);

/* Counts for the log and the tests. */
uint32_t nv2a_dbg_published(void);

/* Tests: every list empty and freed, nothing published. */
void nv2a_dbg_reset(void);

/* Pure helper (tests): p (xyz) through view then proj, with the pull. */
void nv2a_dbg_transform(const float view[16], const float proj[16], const float p[3], int pull,
                        float out[4]);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_DBG_DRAW_H */
