/**
 * NV2A state logic shared by the pushbuffer backends: the out-of-line half
 * of nv2a_backend_common.h. No OS or graphics headers.
 */
#include "nv2a_backend_common.h"
#include "nv2a_tex_pack.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

uint32_t nv2a_tex_extent(uint32_t fmt, uint32_t w, uint32_t h,
                         uint32_t pitch, uint32_t levels)
{
    uint32_t bb = d3d8_format_dxt_block_bytes(fmt), tb, l, n = 0;
    if (bb || d3d8_format_is_swizzled(fmt)) {
        tb = bb ? 0 : nv2a_tex_texel_bytes(nv2a_tex_linear_twin(fmt));
        if (!bb && !tb)
            return 0;
        for (l = 0; l < levels; l++) {
            n += bb ? ((w + 3u) / 4u) * ((h + 3u) / 4u) * bb : w * h * tb;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        return n;
    }
    tb = nv2a_tex_texel_bytes(fmt);
    if (!tb || !pitch)
        return 0;
    return pitch * (h - 1u) + w * tb;
}

uint32_t nv2a_tex_face_stride(uint32_t fmt, uint32_t w, uint32_t h,
                              uint32_t pitch, uint32_t levels)
{
    uint32_t n = nv2a_tex_size_from_format(fmt)
               ? nv2a_tex_extent(fmt, w, h, pitch, levels ? levels : 1u)
               : pitch * h;
    return (n + 127u) & ~127u;
}

uint64_t nv2a_tex_hash(const uint8_t *p, uint32_t n, uint64_t h)
{
    uint64_t l[4] = { h, h ^ 1u, h ^ 2u, h ^ 3u };
    uint32_t i;
    for (i = 0; i + 32 <= n; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        l[0] = (l[0] ^ w[0]) * 0x100000001B3ull;
        l[1] = (l[1] ^ w[1]) * 0x100000001B3ull;
        l[2] = (l[2] ^ w[2]) * 0x100000001B3ull;
        l[3] = (l[3] ^ w[3]) * 0x100000001B3ull;
    }
    for (; i < n; i++)
        l[0] = (l[0] ^ p[i]) * 0x100000001B3ull;
    return (l[0] ^ (l[1] << 1) ^ (l[2] << 2) ^ (l[3] << 3)) * 0x100000001B3ull;
}

static _Atomic uint64_t s_rt_hashes;

static uint64_t rt_own_hash(const uint8_t *mem, uint32_t addr, uint32_t pitch, uint32_t h)
{
    /* The end of the window addr lies in; an address in neither (a test's
     * own surface) is read as the seed and a write-back would read it. */
    uint64_t top = addr >= NV2A_RT_CONTIG_BASE
                       ? (uint64_t)NV2A_RT_CONTIG_BASE + NV2A_RT_CONTIG_SIZE
                 : addr < NV2A_RT_WINDOW ? NV2A_RT_WINDOW : 0x100000000ull;
    uint64_t n = (uint64_t)pitch * h;
    if (n > top - addr)
        n = top - addr;
    atomic_fetch_add_explicit(&s_rt_hashes, 1, memory_order_relaxed);
    return nv2a_tex_hash(mem + addr, (uint32_t)n, NV2A_TEX_HASH_SEED);
}

void nv2a_rt_own_reset(struct nv2a_rt_own *o, const uint8_t *mem, uint32_t addr,
                       uint32_t pitch, uint32_t h, uint32_t flip)
{
    o->mem_hash = rt_own_hash(mem, addr, pitch, h);
    /* Knowledge of this instant, not a check for the flip: a title write
     * later in the same flip (a CPU fill before its next draw) must still
     * be seen by the next check. */
    o->checked_flip = flip - 1u;
}

int nv2a_rt_own_stale(struct nv2a_rt_own *o, const uint8_t *mem, uint32_t addr,
                      uint32_t pitch, uint32_t h, uint32_t flip)
{
    if (o->draw_flip == flip || o->checked_flip == flip)
        return 0;
    o->checked_flip = flip;
    return rt_own_hash(mem, addr, pitch, h) != o->mem_hash;
}

uint64_t nv2a_rt_own_hashes(void)
{
    return atomic_load_explicit(&s_rt_hashes, memory_order_relaxed);
}

size_t nv2a_tex_texels(uint32_t w, uint32_t h, uint32_t levels)
{
    size_t n = 0;
    uint32_t l;
    for (l = 0; l < levels; l++)
        n += (size_t)nv2a_tex_level_dim(w, l) * nv2a_tex_level_dim(h, l);
    return n;
}

uint32_t nv2a_tex_decode_level(const uint8_t *mem, uint32_t fmt, uint32_t w,
                               uint32_t h, const uint32_t *pal,
                               uint32_t pal_len, uint32_t *out)
{
    uint32_t x, y, bb = d3d8_format_dxt_block_bytes(fmt);
    if (bb) {
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                uint32_t argb = 0;
                d3d8_dxt_decode_texel(mem, fmt, x, y, w, &argb);
                out[(size_t)y * w + x] = argb;
            }
        return ((w + 3u) / 4u) * ((h + 3u) / 4u) * bb;
    } else {
        uint32_t mx, my, lin = nv2a_tex_linear_twin(fmt);
        xbox_swizzle_masks(w, h, &mx, &my);
        for (y = 0; y < h; y++) {
            uint32_t oy = swizzle_deposit(y, my);
            for (x = 0; x < w; x++) {
                uint32_t argb = 0;
                nv2a_tex_decode_texel(lin, mem, swizzle_deposit(x, mx) | oy,
                                      pal, pal_len, &argb);
                out[(size_t)y * w + x] = argb;
            }
        }
        return w * h * nv2a_tex_texel_bytes(lin);
    }
}

void nv2a_tex_decode(const uint8_t *mem, uint32_t fmt, uint32_t w, uint32_t h,
                     uint32_t pitch, uint32_t levels, const uint32_t *pal,
                     uint32_t pal_len, uint32_t *out)
{
    uint32_t x, y, l;
    if (nv2a_tex_size_from_format(fmt)) {
        for (l = 0; l < levels; l++) {
            uint32_t lw = nv2a_tex_level_dim(w, l), lh = nv2a_tex_level_dim(h, l);
            mem += nv2a_tex_decode_level(mem, fmt, lw, lh, pal, pal_len, out);
            out += (size_t)lw * lh;
        }
        return;
    }
    for (y = 0; y < h; y++) {
        const uint8_t *row = mem + (size_t)y * pitch;
        for (x = 0; x < w; x++) {
            uint32_t argb = 0;
            nv2a_tex_decode_texel(fmt, row, x, pal, pal_len, &argb);
            out[(size_t)y * w + x] = argb;
        }
    }
}

void nv2a_stage_decode(int n, const uint32_t *r, const uint8_t *set,
                       int shader_set, uint32_t shader_prog,
                       nv2a_dma_resolve_fn resolve, unsigned flags,
                       struct nv2a_stage *t)
{
    uint32_t fmt = r[0x04 / 4], ctl0 = r[0x0C / 4];
    int on = (ctl0 >> 30) & 1, mode, from_fmt;

    memset(t, 0, sizeof *t);
    t->offset = r[0] ? resolve(r[0]) : 0;
    t->color = (fmt >> 8) & 0xFF;
    from_fmt = nv2a_tex_size_from_format(t->color);
    if (from_fmt) {
        t->width  = 1u << ((fmt >> 20) & 0xF);
        t->height = 1u << ((fmt >> 24) & 0xF);
    } else {
        t->width  = r[0x1C / 4] >> 16;
        t->height = r[0x1C / 4] & 0xFFFF;
    }
    /* CONTROL1's pitch means nothing to a swizzled or DXT texture, and
     * left in a cache key a stale one splits it in two. */
    t->pitch  = (from_fmt && (flags & NV2A_STAGE_PITCH_LINEAR))
              ? 0 : r[0x10 / 4] >> 16;
    t->addr_u =  r[0x08 / 4]       & 0xF;
    t->addr_v = (r[0x08 / 4] >> 8) & 0xF;
    /* BORDER reads SET_TEXTURE_BORDER_COLOR (0x1B24) when FORMAT's
     * BORDER_SOURCE (bit 3) says so, as xemu does: Burnout 3 samples its
     * full-screen passes that way on stages 0-3, border 0x00000000, so the
     * taps past the edge are transparent black, not the edge texel. With the
     * texture as the source the border texels would come from the image;
     * that stays a clamp to edge, an approximation no title of ours needs. */
    if (!((fmt >> 3) & 1u)) {
        if (t->addr_u == NV2A_ADDR_BORDER) t->addr_u = 3;
        if (t->addr_v == NV2A_ADDR_BORDER) t->addr_v = 3;
    }
    t->border = r[0x24 / 4];
    t->filter = r[0x14 / 4];
    t->cube = (int)((fmt >> 2) & 1u);
    t->dims = (fmt >> 4) & 0xFu;
    t->fmt_levels = (fmt >> 16) & 0xFu;
    if (!t->fmt_levels)
        t->fmt_levels = 1;
    /* P8: SET_TEXTURE_PALETTE (0x1B20). */
    if (t->color == NV2A_TEX_P8)
        nv2a_tex_palette_decode(r[0x20 / 4], resolve, &t->palette, &t->pal_len);
    t->levels = 1;
    if (from_fmt && (flags & NV2A_STAGE_MIPS)) {
        /* As xemu's pgraph_get_texture_shape: the format's count, no more
         * than MAX_LOD_CLAMP + 1, none smaller than 1x1. The clamps are
         * whole levels (xemu reads the 12-bit fields as is). */
        uint32_t lu = (fmt >> 20) & 0xF, lv = (fmt >> 24) & 0xF;
        uint32_t lmax = (ctl0 >> 6) & 0xFFF, lmin = (ctl0 >> 18) & 0xFFF;
        uint32_t lv_n = (fmt >> 16) & 0xF;
        if (lv_n > lmax + 1u) lv_n = lmax + 1u;
        if (lv_n > (lu > lv ? lu : lv) + 1u) lv_n = (lu > lv ? lu : lv) + 1u;
        t->levels = lv_n ? lv_n : 1u;
        t->lod_min = lmin < t->levels - 1u ? lmin : t->levels - 1u;
        t->lod_max = lmax < t->levels - 1u ? lmax : t->levels - 1u;
    }
    t->valid = t->offset && t->width && t->height
            && (from_fmt || t->pitch)
            && (!(flags & NV2A_STAGE_P8_PALETTE)
                || t->color != NV2A_TEX_P8 || t->palette);
    if (n == 0 && !set[0x0C / 4])
        on = 1;                         /* never configured: counts as on */
    if (shader_set)
        mode = (int)((shader_prog >> (5 * n)) & 0x1F);
    else
        mode = n == 0 ? 1 : 0;
    t->raw_mode = mode;
    t->on = on;
    memcpy(t->bump, &r[0x28 / 4], sizeof t->bump);
    memcpy(&t->bump_scale, &r[0x38 / 4], 4);
    memcpy(&t->bump_offset, &r[0x3C / 4], 4);
    /* PASSTHRU and CLIPPLANE have no texture to be valid: they stay. The
     * GPU backends bind CLIPPLANE as 0 (no texture) and kill its pixels from
     * nv2a_ps_consts.clip_mode instead. */
    if (mode != 4 && mode != 5 && (!on || !t->valid))
        mode = 0;
    t->mode = mode;
}

uint32_t nv2a_prim_to_list(uint32_t prim, const uint16_t *v, uint32_t n,
                           uint32_t base, uint32_t *out,
                           enum nv2a_topology *topo)
{
    uint32_t k = 0, i;

#define PUT(x) (out[k++] = (uint32_t)(x) - base)
    switch (prim) {
    case NV_PRIM_POINTS:
        *topo = NV2A_TOPO_POINTS;
        for (i = 0; i < n; i++) PUT(v[i]);
        break;
    case NV_PRIM_LINES:
        *topo = NV2A_TOPO_LINES;
        for (i = 0; i + 1 < n; i += 2) { PUT(v[i]); PUT(v[i + 1]); }
        break;
    case NV_PRIM_LINE_LOOP:
    case NV_PRIM_LINE_STRIP:
        *topo = NV2A_TOPO_LINES;
        for (i = 0; i + 1 < n; i++) { PUT(v[i]); PUT(v[i + 1]); }
        if (prim == NV_PRIM_LINE_LOOP && n > 2) { PUT(v[n - 1]); PUT(v[0]); }
        break;
    case NV_PRIM_TRIANGLES:
        *topo = NV2A_TOPO_TRIANGLES;
        for (i = 0; i + 2 < n; i += 3) { PUT(v[i]); PUT(v[i + 1]); PUT(v[i + 2]); }
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        *topo = NV2A_TOPO_TRIANGLES;
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) { PUT(v[i + 1]); PUT(v[i]); PUT(v[i + 2]); }
            else       { PUT(v[i]); PUT(v[i + 1]); PUT(v[i + 2]); }
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        *topo = NV2A_TOPO_TRIANGLES;
        for (i = 1; i + 1 < n; i++) { PUT(v[0]); PUT(v[i]); PUT(v[i + 1]); }
        break;
    case NV_PRIM_QUADS:
        *topo = NV2A_TOPO_TRIANGLES;
        for (i = 0; i + 3 < n; i += 4) {
            PUT(v[i]); PUT(v[i + 1]); PUT(v[i + 2]);
            PUT(v[i]); PUT(v[i + 2]); PUT(v[i + 3]);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        *topo = NV2A_TOPO_TRIANGLES;
        for (i = 0; i + 3 < n; i += 2) {
            PUT(v[i]); PUT(v[i + 1]); PUT(v[i + 3]);
            PUT(v[i]); PUT(v[i + 3]); PUT(v[i + 2]);
        }
        break;
    default:
        *topo = NV2A_TOPO_NONE;
        return 0;
    }
#undef PUT
    return k;
}

int nv2a_merge_joinable(uint32_t prim, uint32_t n)
{
    return prim >= NV_PRIM_TRIANGLES && prim <= NV_PRIM_POLYGON
        && n >= 3 && n <= NV2A_MERGE_BATCH_MAX;
}

/* Append batch v's triangles to out at *at, room permitting. */
static int merge_put(uint16_t *out, uint32_t *at, uint32_t prim, const uint16_t *v, uint32_t n)
{
    uint32_t tmp[NV2A_MERGE_BATCH_MAX * 3u], k, i;
    enum nv2a_topology topo;

    k = nv2a_prim_to_list(prim, v, n, 0, tmp, &topo);
    if (*at + k > NV2A_MERGE_LIST_MAX)
        return 0;
    for (i = 0; i < k; i++)
        out[*at + i] = (uint16_t)tmp[i];
    *at += k;
    return 1;
}

int nv2a_merge_add(struct nv2a_merge *m, uint32_t prim, const uint16_t *v, uint32_t n)
{
    uint16_t lo = v[0], hi = v[0];
    uint32_t i, at;

    for (i = 1; i < n; i++) {
        if (v[i] < lo) lo = v[i];
        if (v[i] > hi) hi = v[i];
    }
    if (!m->batches) {
        m->prim = prim;
        m->batches = 1;
        m->verts = n;
        m->raw_n = n;
        memcpy(m->raw, v, n * sizeof *v);
        m->n = 0;
        m->lo = lo;
        m->hi = hi;
        return 1;
    }
    if (prim != m->prim)
        return 0;
    if (lo > m->lo) lo = m->lo;
    if (hi < m->hi) hi = m->hi;
    if ((uint32_t)(hi - lo) >= NV2A_MERGE_SPAN_MAX)
        return 0;
    at = 0;
    if (m->batches == 1 && !merge_put(m->list, &at, prim, m->raw, m->raw_n))
        return 0;
    if (m->batches > 1)
        at = m->n;
    if (!merge_put(m->list, &at, prim, v, n))
        return 0;
    m->n = at;
    m->batches++;
    m->verts += n;
    m->lo = lo;
    m->hi = hi;
    return 1;
}

uint32_t nv2a_vsh_program_hash(const struct nv2a_pb_vsh *v, uint32_t *len)
{
    uint32_t h = 2166136261u, n = 0, pc, k;
    for (pc = v->start; pc < NV2A_VSH_SLOTS; pc++) {
        for (k = 0; k < 4; k++)
            h = (h ^ v->prog[pc][k]) * 16777619u;
        n++;
        if (v->prog[pc][3] & 1u)        /* FINAL */
            break;
    }
    if (len)
        *len = n;
    return h;
}

int nv2a_blend_constant(int enable, uint32_t sfactor, uint32_t dfactor,
                        uint32_t color, float bf[4], int *mixed)
{
    uint32_t s = nv2a_blend_from_gl(sfactor), d = nv2a_blend_from_gl(dfactor);
    int col = s == NV2A_BF_CONST_COLOR || s == NV2A_BF_INV_CONST_COLOR
           || d == NV2A_BF_CONST_COLOR || d == NV2A_BF_INV_CONST_COLOR;
    int alp = s == NV2A_BF_CONST_ALPHA || s == NV2A_BF_INV_CONST_ALPHA
           || d == NV2A_BF_CONST_ALPHA || d == NV2A_BF_INV_CONST_ALPHA;

    *mixed = 0;
    if (!enable || (!col && !alp)) {
        bf[0] = bf[1] = bf[2] = bf[3] = 1.0f;
        return 0;
    }
    bf[3] = (float)(color >> 24) / 255.0f;
    *mixed = col && alp;
    if (col) {
        bf[0] = (float)((color >> 16) & 0xFF) / 255.0f;
        bf[1] = (float)((color >>  8) & 0xFF) / 255.0f;
        bf[2] = (float)( color        & 0xFF) / 255.0f;
    } else {
        bf[0] = bf[1] = bf[2] = bf[3];
    }
    return 1;
}

float nv2a_uv_snap(const struct nv2a_stage *t, unsigned n, int flat2d)
{
    if (n <= 1 || !t->mode || t->mode == 4
            || !(nv2a_tex_filter_linear(t->filter, 0)
                 || nv2a_tex_filter_linear(t->filter, 1)))
        return 0.0f;
    if (flat2d)
        return -(float)n;
    return nv2a_tex_size_from_format(t->color) ? 0.0f : (float)n;
}

void nv2a_ps_consts_snap(struct nv2a_ps_consts *pc, const struct nv2a_stage st[4],
                         unsigned rt_mask, unsigned n, int flat2d)
{
    int k;

    for (k = 0; k < 4; k++)
        if (!(rt_mask >> k & 1u))
            pc->tex_scale[k][2] = nv2a_uv_snap(&st[k], n, flat2d);
}

void nv2a_ps_consts_fill(const struct nv2a_pb_vsh *v,
                         const struct nv2a_stage st[4],
                         struct nv2a_ps_consts *pc)
{
    int n;

    memset(pc, 0, sizeof *pc);
    for (n = 0; n < 4; n++) {
        const struct nv2a_stage *t = &st[n];
        int scaled = nv2a_tex_size_from_format(t->color);
        pc->tex_mode[n] = (uint32_t)t->mode;
        pc->tex_scale[n][0] = scaled || !t->width ? 1.0f : 1.0f / (float)t->width;
        pc->tex_scale[n][1] = scaled || !t->height ? 1.0f : 1.0f / (float)t->height;
        pc->tex_scale[n][2] = nv2a_uv_snap(t, nv2a_host_render_scale(), 0);
        memcpy(pc->bump[n], t->bump, sizeof pc->bump[n]);
        pc->bumpls[n][0] = t->bump_scale;
        pc->bumpls[n][1] = t->bump_offset;
        /* raw_mode, not mode: the backends bind a CLIPPLANE stage as 0. */
        if (t->raw_mode == 5)
            pc->clip_mode[n] = 0x10u | ((v->clip_plane_mode >> (4 * n)) & 0xFu);
    }
    for (n = 0; n < 8; n++) {
        nv2a_argb_to_float4(v->rc_f0[n], pc->c0[n]);
        nv2a_argb_to_float4(v->rc_f1[n], pc->c1[n]);
    }
    nv2a_argb_to_float4(v->rc_sf[0], pc->fc0);
    nv2a_argb_to_float4(v->rc_sf[1], pc->fc1);
    nv2a_argb_to_float4(v->fog_color, pc->fog_color);
    {   /* the CPU path reads the fog colour register as RGBA, alpha 1 */
        float t = pc->fog_color[0];
        pc->fog_color[0] = pc->fog_color[2];
        pc->fog_color[2] = t;
        pc->fog_color[3] = 1.0f;
    }
    pc->alpha_ref = (float)v->atest_ref / 255.0f;
    pc->alpha_test_enable = v->atest_enable ? 1u : 0u;
    pc->alpha_func = nv2a_cmp_from_gl(v->atest_func) + 1;
}

void nv2a_ps_consts_rt(struct nv2a_ps_consts *pc, int n,
                       const struct nv2a_stage *t, uint32_t w, uint32_t h)
{
    if (!nv2a_tex_size_from_format(t->color)) {
        pc->tex_scale[n][0] = 1.0f / (float)w;
        pc->tex_scale[n][1] = 1.0f / (float)h;
    }
    pc->tex_scale[n][2] = 0.0f;
}

void nv2a_vp_consts_fill(const struct nv2a_pb_vsh *v, uint32_t surface_format,
                         uint32_t w, uint32_t h, uint32_t pad,
                         struct nv2a_vp_consts *o)
{
    uint32_t func, abs_, edge_one;
    float pw = (float)(w + 2u * pad);

    o->c[0] = 2.0f / pw;
    o->c[1] = -2.0f / (float)h;
    o->c[2] = 1.0f / nv2a_zmax(surface_format);
    o->c[3] = 0.0f;
    o->c[4] = -(float)w / pw;
    o->c[5] = 1.0f;
    o->c[6] = 0.0f;
    o->c[7] = 0.0f;
    /* Fog and specular, as the CPU path's vsh_fog_factor and vsh_transform
     * apply them (xemu vsh.c). */
    o->c[8] = v->fog_param[0];
    o->c[9] = v->fog_param[1];
    o->c[10] = v->fog_param[2];
    o->c[11] = 0.0f;
    memset(o->u, 0, sizeof o->u);
    if (nv2a_fog_mode(v->fog_mode, &func, &abs_, &edge_one)) {
        o->u[1] = func;
        o->u[2] = abs_;
        o->u[3] = edge_one;
        o->u[0] = v->fog_enable ? 1u : 0u;
    }
    o->u[4] = v->spec_enable ? 1u : 0u;
    o->u[5] = (v->light_ctl >> 17) & 1u;
    /* Depth test off: no far-plane clip (see the vertex program epilogue).
     * Test Drive: Eve of Destruction's 2D passes write z = 2^24 - 1 on a
     * Z16 surface (zmax 65535) with the depth test off; the NV2A draws
     * them, a far clip at zmax dropped every one and its Metal frames were
     * black. */
    o->u[6] = v->depth_enable ? 0u : 1u;
}

void nv2a_report_write(uint8_t *mem, uint32_t va, uint32_t count)
{
    volatile uint32_t *r = (volatile uint32_t *)(mem + va);
    r[0] = 0;                              /* timestamp */
    r[1] = 0;
    r[2] = count;
    /* status: done, as a release store, so the count (and the rest of the
     * report) is visible before it on any host, arm64 included. */
    atomic_store_explicit((_Atomic uint32_t *)(mem + va + 12), 0u,
                          memory_order_release);
}

void nv2a_report_set_pending(uint8_t *mem, uint32_t va)
{
    ((volatile uint32_t *)(mem + va))[3] = 0xFFFFFFFFu;
}

void nv2a_occ_init(struct nv2a_occ *o, const struct nv2a_occ_ops *ops,
                   uint8_t *mem)
{
    memset(o, 0, sizeof *o);
    o->ops = ops;
    o->mem = mem;
    o->scale = nv2a_host_render_scale();
    o->settle_us = NV2A_OCC_SETTLE_US;
}

static void occ_put_all(struct nv2a_occ *o, struct nv2a_occ_report *p)
{
    int i;
    for (i = 0; i < p->n; i++)
        o->ops->put(o->ops->ctx, p->q[i]);
    p->n = 0;
}

void nv2a_occ_set_scale(struct nv2a_occ *o, unsigned scale)
{
    int i;
    for (i = 0; i < o->nret; i++)
        occ_put_all(o, &o->retired[i]);
    o->ret_drop += (uint64_t)o->nret;
    o->nret = 0;
    o->scale = scale;
}

void nv2a_occ_set_delay(struct nv2a_occ *o, uint64_t us)
{
    o->delay_us = us;
}

void nv2a_occ_set_settle(struct nv2a_occ *o, uint64_t us)
{
    o->settle_us = us;
}

void nv2a_occ_set_trace(struct nv2a_occ *o, int on)
{
    o->trace = on;
}

uint32_t nv2a_occ_scale_count(uint64_t samples, unsigned scale)
{
    if (scale > 1 && samples) {
        uint64_t d = (uint64_t)scale * scale;
        /* Rounded to nearest; a few samples of a sliver still mean
         * "visible", so a non-zero count never rounds to 0. */
        samples = (samples + d / 2) / d;
        if (!samples)
            samples = 1;
    }
    return samples > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)samples;
}

static void occ_begin(struct nv2a_occ *o)
{
    void *q;
    if (o->open)
        return;
    if (o->nseg >= NV2A_OCC_SEG || !(q = o->ops->get(o->ops->ctx))) {
        o->ovf = 1;
        return;
    }
    o->ops->begin(o->ops->ctx, q);
    o->seg[o->nseg++] = q;
    o->open = 1;
}

static void occ_end(struct nv2a_occ *o)
{
    if (!o->open)
        return;
    o->ops->end(o->ops->ctx, o->seg[o->nseg - 1]);
    o->open = 0;
}

/* 1 when every query of the report has its result (summed into *count).
 * delay_us holds a result back as a slow host GPU would. */
static int occ_ready(struct nv2a_occ *o, const struct nv2a_occ_report *p,
                     int flush, uint64_t now, uint32_t *count)
{
    uint64_t sum = 0;
    int i;
    if (o->delay_us && now - p->t0 < o->delay_us)
        return 0;
    for (i = 0; i < p->n; i++) {
        uint64_t v = 0;
        if (!o->ops->result(o->ops->ctx, p->q[i], flush, &v))
            return 0;
        sum += v;
    }
    *count = nv2a_occ_scale_count(sum, o->scale);
    return 1;
}

/* A result taken at `now`: its age in the histogram, its count as the
 * address's last unless a newer report's is there already. */
static void occ_result_in(struct nv2a_occ *o, const struct nv2a_occ_report *p,
                          uint64_t now, uint32_t count)
{
    static const uint64_t edge[NV2A_OCC_AGES - 1] = {
        1000, 2000, 4000, 8000, 16000, 33000, 66000
    };
    uint64_t age = now - p->t0;
    struct nv2a_occ_last *l;
    int b, i;

    for (b = 0; b < NV2A_OCC_AGES - 1 && age >= edge[b]; b++)
        ;
    o->age[b]++;
    for (i = 0; i < o->nlast; i++)
        if (o->last[i].va == p->va) {
            if (o->last[i].t0 <= p->t0) {
                o->last[i].count = count;
                o->last[i].t0 = p->t0;
                o->last[i].flip = p->flip;
            }
            return;
        }
    /* A fixed set of report slots per device (the XDK recycles them), so
     * this is a bound, not a model: the oldest entry goes when full. */
    if (o->nlast < NV2A_OCC_LAST) {
        l = &o->last[o->nlast++];
    } else {
        l = &o->last[o->last_next];
        o->last_next = (o->last_next + 1) % NV2A_OCC_LAST;
    }
    l->va = p->va;
    l->count = count;
    l->t0 = p->t0;
    l->flip = p->flip;
}

static void occ_finish(struct nv2a_occ *o, struct nv2a_occ_report *p,
                       uint32_t count)
{
    nv2a_report_write(o->mem, p->va, count);
    o->vis += count != 0;
    occ_put_all(o, p);
}

/* The late result of a settled report, written over the settled count only
 * while the slot still holds it with status 0: a title that reset the slot
 * (EndVisibilityTest writes status 0xFFFFFFFF) or read and reused it keeps
 * what it wrote. The compare-and-swap leaves the title's own stores alone. */
static int occ_amend(struct nv2a_occ *o, const struct nv2a_occ_report *p,
                     uint32_t count)
{
    _Atomic uint32_t *c = (_Atomic uint32_t *)(o->mem + p->va + 8);
    _Atomic uint32_t *st = (_Atomic uint32_t *)(o->mem + p->va + 12);
    uint32_t want = p->settled;
    if (atomic_load_explicit(st, memory_order_acquire) != 0)
        return 0;
    return atomic_compare_exchange_strong_explicit(c, &want, count,
                                                   memory_order_release,
                                                   memory_order_relaxed);
}

/* Settled reports: a result refreshes the last count, amends the report
 * (occ_amend) and frees the queries; one never answered within
 * NV2A_OCC_LATE_US is dropped. Never with a flush: nothing waits on these,
 * and every ack pass polls them (a flushing GetData, or a Metal commit, at
 * tens of kHz). */
static void occ_poll_retired(struct nv2a_occ *o, uint64_t now)
{
    int i, j;
    for (i = j = 0; i < o->nret; i++) {
        struct nv2a_occ_report *p = &o->retired[i];
        uint32_t count;
        if (occ_ready(o, p, 0, now, &count)) {
            occ_result_in(o, p, now, count);
            if (p->amend && count != p->settled && occ_amend(o, p, count))
                o->amended++;
            occ_put_all(o, p);
        } else if (now - p->t0 > NV2A_OCC_LATE_US) {
            o->ret_drop++;
            occ_put_all(o, p);
        } else {
            o->retired[j++] = *p;
        }
    }
    o->nret = j;
}

void nv2a_occ_poll(struct nv2a_occ *o, int flush)
{
    uint64_t now;
    int i, j;
    if (!o->npend && !o->nret)
        return;
    now = o->ops->now_us(o->ops->ctx);
    for (i = j = 0; i < o->npend; i++) {
        struct nv2a_occ_report *p = &o->pend[i];
        uint32_t count;
        if (p->overflow) {
            occ_finish(o, p, NV2A_OCC_VISIBLE);
        } else if (occ_ready(o, p, flush, now, &count)) {
            occ_result_in(o, p, now, count);
            occ_finish(o, p, count);
        } else if (now - p->t0 > NV2A_OCC_LATE_US) {
            o->late++;
            occ_finish(o, p, NV2A_OCC_VISIBLE);
        } else {
            o->pend[j++] = *p;
        }
    }
    o->npend = j;
    occ_poll_retired(o, now);
}

void nv2a_occ_flip(struct nv2a_occ *o)
{
    o->flips++;
    nv2a_occ_settle(o);
    o->frame_wait_us = 0;
}

static int occ_near(uint32_t a, uint32_t b)
{
    return a != b && (a > b ? a - b : b - a) < NV2A_OCC_NEAR;
}

/* Another report beside pend[i] from the same flip, pending, settled or
 * done: several objects tested at once, not one test rotating. */
static int occ_crowded(const struct nv2a_occ *o, int i)
{
    const struct nv2a_occ_report *p = &o->pend[i];
    int j;

    for (j = 0; j < o->npend; j++)
        if (j != i && occ_near(o->pend[j].va, p->va))
            return 1;
    for (j = 0; j < o->nret; j++)
        if (o->retired[j].flip == p->flip && occ_near(o->retired[j].va, p->va))
            return 1;
    for (j = 0; j < o->nlast; j++)
        if (o->last[j].flip == p->flip && occ_near(o->last[j].va, p->va))
            return 1;
    return 0;
}

void nv2a_occ_settle(struct nv2a_occ *o)
{
    uint64_t now;
    int i, k;

    if (!o->npend) {
        nv2a_occ_poll(o, 0);
        return;
    }
    nv2a_occ_poll(o, 1);
    /* One budget a frame, not a walk: a title testing a dozen times a frame
     * on a host far behind would otherwise wait a dozen times over. */
    if (o->npend && o->frame_wait_us < o->settle_us) {
        uint64_t left = o->settle_us - o->frame_wait_us, w;
        uint64_t t = o->ops->now_us(o->ops->ctx);
        while (o->npend && o->ops->now_us(o->ops->ctx) - t < left)
            nv2a_occ_poll(o, 1);
        w = o->ops->now_us(o->ops->ctx) - t;
        o->frame_wait_us += w;
        o->settle_wait_us += w;
    }
    if (!o->npend)
        return;
    now = o->ops->now_us(o->ops->ctx);
    for (i = 0; i < o->npend; i++) {
        struct nv2a_occ_report *p = &o->pend[i];
        uint32_t count = NV2A_OCC_VISIBLE;
        int have = 0, kn = -1, lone = !occ_crowded(o, i);
        /* A last count older than NV2A_OCC_LATE_US belongs to another
         * screen (BLiNX 2's LOAD GAME reuses a slot its boot tested, with
         * count 0 then): no history, visible. */
        for (k = 0; k < o->nlast; k++) {
            if (now - o->last[k].t0 > NV2A_OCC_LATE_US)
                continue;
            if (o->last[k].va == p->va) {
                count = o->last[k].count;
                have = 1;
                break;
            }
            /* No count at va yet: the youngest from a slot beside it.
             * Burnout 3 rotates its one sun test a flip over ten slots, so
             * a GPU a frame behind leaves each slot without a count, and
             * a visible fallback drew the flare rings at full strength.
             * Only for a lone test: beside other tests of its flip it is
             * one object among several. */
            if (lone && occ_near(o->last[k].va, p->va)
                    && (kn < 0 || o->last[k].t0 > o->last[kn].t0))
                kn = k;
        }
        if (!have && kn >= 0) {
            count = o->last[kn].count;
            have = 2;
        }
        nv2a_report_write(o->mem, p->va, count);
        o->vis += count != 0;
        o->settled++;
        if (have == 1)
            o->stale++;
        else if (have)
            o->beside++;
        else
            o->first++;
        if (o->trace && o->trace_n < 64) {
            o->trace_n++;
            fprintf(stderr, "[OCC] settle (flip %llu) va 0x%08X count %u (%s) age %llu us\n",
                    (unsigned long long)o->flips, p->va, count,
                    have == 1 ? "stale" : have ? "beside" : "first",
                    (unsigned long long)(now - p->t0));
        }
        if (o->nret >= NV2A_OCC_PENDING) {
            o->ret_drop++;
            occ_put_all(o, &o->retired[0]);
            memmove(&o->retired[0], &o->retired[1],
                    (size_t)(--o->nret) * sizeof o->retired[0]);
        }
        p->settled = count;
        p->amend = 1;
        o->retired[o->nret++] = *p;     /* the queries move with it */
        p->n = 0;
    }
    o->npend = 0;
}

void nv2a_occ_summary(const struct nv2a_occ *o, const char *tag)
{
    fprintf(stderr, "%s occ totals: %llu reports, %llu visible, %llu late,"
            " %llu superseded,"
            " settled %llu (stale %llu, beside %llu, first %llu),"
            " %llu amended,"
            " %llu retired dropped,"
            " settle wait %llu us\n", tag,
            (unsigned long long)o->reports,
            (unsigned long long)o->vis,
            (unsigned long long)o->late,
            (unsigned long long)o->super,
            (unsigned long long)o->settled,
            (unsigned long long)o->stale,
            (unsigned long long)o->beside,
            (unsigned long long)o->first,
            (unsigned long long)o->amended,
            (unsigned long long)o->ret_drop,
            (unsigned long long)o->settle_wait_us);
    if (o->trace)
        fprintf(stderr, "%s occ result age: <1ms %llu <2 %llu <4 %llu <8 %llu"
                " <16 %llu <33 %llu <66 %llu >=66 %llu\n", tag,
                (unsigned long long)o->age[0], (unsigned long long)o->age[1],
                (unsigned long long)o->age[2], (unsigned long long)o->age[3],
                (unsigned long long)o->age[4], (unsigned long long)o->age[5],
                (unsigned long long)o->age[6], (unsigned long long)o->age[7]);
}

void nv2a_occ_zpass(struct nv2a_occ *o, int enable)
{
    o->en = enable;
    if (enable)
        occ_begin(o);
    else
        occ_end(o);
}

void nv2a_occ_zpass_clear(struct nv2a_occ *o)
{
    int i;
    occ_end(o);
    for (i = 0; i < o->nseg; i++)
        o->ops->put(o->ops->ctx, o->seg[i]);   /* ended; Begin on reuse drops it */
    o->nseg = 0;
    o->ovf = 0;
    if (o->en)
        occ_begin(o);
}

void nv2a_occ_report(struct nv2a_occ *o, uint32_t va, int sync)
{
    struct nv2a_occ_report *p;
    int i;

    occ_end(o);
    /* Supersede a report still pending at this va before any poll: a poll
     * here could finish the old one over the title's reset. Since every walk
     * ends in a settle, this happens within a walk only. */
    for (i = 0; i < o->npend; i++)
        if (o->pend[i].va == va) {
            occ_put_all(o, &o->pend[i]);
            memmove(&o->pend[i], &o->pend[i + 1],
                    (size_t)(o->npend - i - 1) * sizeof o->pend[0]);
            o->npend--;
            o->super++;
            break;              /* at most one: each push supersedes */
        }
    /* The address is the new test's now: an older settled report's late
     * result must not land over it. */
    for (i = 0; i < o->nret; i++)
        if (o->retired[i].va == va)
            o->retired[i].amend = 0;
    nv2a_occ_poll(o, 0);
    if (o->npend >= NV2A_OCC_PENDING)
        nv2a_occ_poll(o, 1);
    if (o->npend >= NV2A_OCC_PENDING) {
        /* Still full: the oldest is answered as visible. */
        o->late++;
        occ_finish(o, &o->pend[0], NV2A_OCC_VISIBLE);
        memmove(&o->pend[0], &o->pend[1],
                (size_t)(--o->npend) * sizeof o->pend[0]);
    }
    /* Pending until done, whatever the title left in the status. */
    nv2a_report_set_pending(o->mem, va);
    p = &o->pend[o->npend++];
    p->va = va;
    p->n = o->nseg;
    p->overflow = o->ovf;
    p->t0 = o->ops->now_us(o->ops->ctx);
    p->flip = o->flips;
    for (i = 0; i < o->nseg; i++)
        p->q[i] = o->seg[i];
    /* The report owns them now; a report with no clear before the next one
     * counts from here (the XDK always clears first). */
    o->nseg = 0;
    o->ovf = 0;
    o->reports++;
    if (sync) {
        uint64_t t = o->ops->now_us(o->ops->ctx), now = t;
        uint32_t count;
        while (!occ_ready(o, p, 1, now, &count)
               && (now = o->ops->now_us(o->ops->ctx)) - t < NV2A_OCC_LATE_US)
            ;
        o->sync_us += o->ops->now_us(o->ops->ctx) - t;
        nv2a_occ_poll(o, 1);
    }
    if (o->en)
        occ_begin(o);
}

/* ---- Host options (enhancements) ------------------------------------------ */

static struct nv2a_host_opts s_host_opts;   /* zero: stock */

/* The request queue: one pending struct and mask, guarded by a spinlock (this
 * file has no OS headers, and the critical sections are a struct copy). The
 * flag is what the flip reads, so the stock cost is one atomic load. */
static struct nv2a_host_opts s_req_opts;
static unsigned s_req_mask;
static atomic_flag s_req_lock = ATOMIC_FLAG_INIT;
static atomic_uint s_req_pending;
static atomic_uint s_opts_gen;

/* The pack's enable bits from pack_off. A change of what the pack matches
 * bumps nv2a_tex_pack_gen() (in nv2a_tex_pack), which every backend's
 * tex_bind compares: one place, so the backends cannot disagree. */
static void pack_apply_off(unsigned off)
{
    nv2a_tex_pack_set_enabled(!(off & NV2A_PACK_OFF_ALL));
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_ART, !(off & NV2A_PACK_OFF_ART));
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_FONT, !(off & NV2A_PACK_OFF_FONT));
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_TEXT, !(off & NV2A_PACK_OFF_TEXT));
}

void nv2a_host_opts_set(const struct nv2a_host_opts *o)
{
    s_host_opts = *o;
    pack_apply_off(o->pack_off);
}

static void req_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_req_lock, memory_order_acquire))
        ;
}

static void req_unlock(void)
{
    atomic_flag_clear_explicit(&s_req_lock, memory_order_release);
}

void nv2a_host_opts_request(const struct nv2a_host_opts *o, unsigned mask)
{
    if (!o || !mask)
        return;
    req_lock();
    if (mask & NV2A_OPT_SCALE)
        s_req_opts.render_scale = o->render_scale;
    if (mask & NV2A_OPT_FILTER)
        s_req_opts.present_filter = o->present_filter;
    if (mask & NV2A_OPT_FULLSCREEN)
        s_req_opts.fullscreen = o->fullscreen;
    if (mask & NV2A_OPT_PACK)
        s_req_opts.pack_off = o->pack_off;
    if (mask & NV2A_OPT_ANISO)
        s_req_opts.aniso = o->aniso;
    if (mask & NV2A_OPT_MOVIE)
        s_req_opts.movie_mode = o->movie_mode;
    if (mask & NV2A_OPT_OVERSCAN)
        s_req_opts.overscan = o->overscan;
    if (mask & NV2A_OPT_SNAP2D)
        s_req_opts.snap_2d = o->snap_2d;
    s_req_mask |= mask;
    req_unlock();
    atomic_store_explicit(&s_req_pending, 1u, memory_order_release);
}

unsigned nv2a_host_opts_apply(void)
{
    struct nv2a_host_opts r;
    unsigned mask, changed = 0;

    if (!atomic_load_explicit(&s_req_pending, memory_order_acquire))
        return 0;
    req_lock();
    atomic_store_explicit(&s_req_pending, 0u, memory_order_relaxed);
    r = s_req_opts;
    mask = s_req_mask;
    s_req_mask = 0;
    req_unlock();

    /* Scale 0 and 1 both mean guest size: not a change. */
    if ((mask & NV2A_OPT_SCALE)
        && (r.render_scale > 1 ? r.render_scale : 1) != nv2a_host_render_scale()) {
        s_host_opts.render_scale = r.render_scale;
        changed |= NV2A_OPT_SCALE;
    }
    if ((mask & NV2A_OPT_FILTER) && r.present_filter != s_host_opts.present_filter) {
        s_host_opts.present_filter = r.present_filter;
        changed |= NV2A_OPT_FILTER;
    }
    if ((mask & NV2A_OPT_FULLSCREEN) && r.fullscreen != s_host_opts.fullscreen) {
        s_host_opts.fullscreen = r.fullscreen;
        changed |= NV2A_OPT_FULLSCREEN;
    }
    if ((mask & NV2A_OPT_PACK) && r.pack_off != s_host_opts.pack_off) {
        s_host_opts.pack_off = r.pack_off;
        pack_apply_off(r.pack_off);
        changed |= NV2A_OPT_PACK;
    }
    if (mask & NV2A_OPT_ANISO) {
        unsigned a = r.aniso < 2 ? 0 : r.aniso > 16 ? 16 : r.aniso;
        if (a != s_host_opts.aniso) {
            s_host_opts.aniso = a;
            changed |= NV2A_OPT_ANISO;
        }
    }
    if ((mask & NV2A_OPT_MOVIE) && (unsigned)r.movie_mode > NV2A_MOVIE_FILL) {
        fprintf(stderr, "[ENHANCE] display.movie=%d out of range; refused\n",
                (int)r.movie_mode);
    } else if ((mask & NV2A_OPT_MOVIE) && r.movie_mode != s_host_opts.movie_mode) {
        s_host_opts.movie_mode = r.movie_mode;
        changed |= NV2A_OPT_MOVIE;
    }
    if (mask & NV2A_OPT_OVERSCAN) {
        unsigned ov = r.overscan > NV2A_OVERSCAN_MAX ? NV2A_OVERSCAN_MAX : r.overscan;
        /* As at start (xbox_enhance_init): under hor+ the 4:3 edges sit
         * inside the padded frame, where no source rectangle can cut them. */
        if (ov && s_host_opts.aspect_w) {
            fprintf(stderr, "[ENHANCE] display.overscan=%u ignored with display.aspect=%u:%u:"
                    " the 4:3 edges are inside the padded frame\n", ov,
                    s_host_opts.aspect_w, s_host_opts.aspect_h);
            ov = s_host_opts.overscan;
        }
        if (ov != s_host_opts.overscan) {
            s_host_opts.overscan = ov;
            changed |= NV2A_OPT_OVERSCAN;
        }
    }
    if ((mask & NV2A_OPT_SNAP2D) && !r.snap_2d != !s_host_opts.snap_2d) {
        s_host_opts.snap_2d = r.snap_2d != 0;
        changed |= NV2A_OPT_SNAP2D;
    }
    if (changed)
        atomic_fetch_add_explicit(&s_opts_gen, 1u, memory_order_release);
    return changed;
}

uint32_t nv2a_host_opts_gen(void)
{
    return atomic_load_explicit(&s_opts_gen, memory_order_acquire);
}

const struct nv2a_host_opts *nv2a_host_opts(void)
{
    return &s_host_opts;
}

unsigned nv2a_host_render_scale(void)
{
    return s_host_opts.render_scale > 1 ? s_host_opts.render_scale : 1;
}

unsigned nv2a_bump_sources(const struct nv2a_stage st[4], int combiner,
                           uint32_t other_stage_input)
{
    unsigned m = 0;
    int n;

    if (!combiner)
        return 0;
    for (n = 1; n < 4; n++) {
        uint32_t mode = nv2a_stage_ps_mode(&st[n], n);
        if (mode == 6 || mode == 7)
            m |= 1u << nv2a_stage_input(n, other_stage_input);
    }
    return m;
}

unsigned nv2a_stage_aniso(const struct nv2a_stage *t, int mip, int min_linear,
                          int bump_source)
{
    if (!mip || !min_linear || t->cube || bump_source)
        return 0;
    return s_host_opts.aniso;
}

/* The width a w x h target widens to under display.aspect, or 0 when it
 * keeps its own. */
static double host_wide(uint32_t w, uint32_t h)
{
    unsigned aw = s_host_opts.aspect_w, ah = s_host_opts.aspect_h;
    double wide;

    if (!aw || !ah || !w || !h)
        return 0.0;
    /* Square: the title's screen-space reflections (BLiNX 2's hub pit and
     * stage 1-1 water) draw the mirrored 4:3 view into a 512 or 256 square
     * and sample it projected from the frame, u = screen x / 640. A wide
     * frame reads u past [0, 1] in its pads, which clamps to the square's
     * edge columns: a seam at the 4:3 boundary. Padded by the frame's
     * ratio, the square holds the wider view; batches sampling it through
     * rt_offset read the same texels in [0, 1] as stock. */
    if (w == h)
        wide = (double)w * 3.0 * aw / (4.0 * ah);
    /* 4:3 within 1%: 3w is 4h give or take 1% of 4h. */
    else if (3.0 * w < 4.0 * h * 0.99 || 3.0 * w > 4.0 * h * 1.01)
        return 0.0;
    else
        wide = (double)h * aw / ah;
    return wide > (double)w ? wide : 0.0;
}

uint32_t nv2a_host_pad_ratio(uint32_t w, uint32_t h)
{
    double wide = host_wide(w, h);

    return wide > 0.0 ? (uint32_t)((wide - (double)w) / 2.0 + 0.5) : 0u;
}

uint32_t nv2a_host_pad(uint32_t w, uint32_t h)
{
    double wide = host_wide(w, h);

    if (wide <= 0.0)
        return 0;
    /* Overscan, squares only: the water bumps its reflection read (1-1's
     * BUMPENVMAP matrices move u by up to 0.106 of the square's width at
     * full-scale du, dv), so at the frame's edge it reads past the ratio's
     * pad and the clamped edge column shows as the effect ending at the
     * viewport's edge. An eighth of the width a side covers that offset
     * with five texels of a 256 square to spare. The mirrored view is
     * drawn through the square's own pad, so the extra columns hold more
     * of it, and every read goes through nv2a_rt_guest_read, so the stock
     * columns stay where they were. A stretched pass keeps to the ratio's
     * pad (nv2a_host_pad_ratio), so a full-width copy between the frame and
     * a square lines up as before. */
    if (w == h)
        return (uint32_t)((wide - (double)w) / 2.0 + (double)w / 8.0 + 0.5);
    return (uint32_t)((wide - (double)w) / 2.0 + 0.5);
}

uint32_t nv2a_host_window_w(uint32_t w, uint32_t h, unsigned scale)
{
    return (w + 2u * nv2a_host_pad(w, h)) * scale;
}

/* The pad a stretch spans on a target padded by pad: the ratio's
 * (nv2a_host_pad_ratio), or pad itself when the ratio gives none, as for a
 * pad passed in under other options. */
static uint32_t stretch_pad(uint32_t w, uint32_t h, uint32_t pad)
{
    uint32_t r = nv2a_host_pad_ratio(w, h);

    return r && r < pad ? r : pad;
}

void nv2a_aspect_map(struct nv2a_vp_consts *o, uint32_t w, uint32_t h, uint32_t pad,
                     int place)
{
    float pw = (float)(w + 2u * pad), k;

    if (!pad)
        return;
    switch (place) {
    case NV2A_PLACE_STRETCH: {
        /* 0..w over the ratio's pad, r a side: the whole target unless a
         * square's overscan lies beyond it, which a stretch leaves alone
         * so the frame's picture is not zoomed into it. */
        uint32_t r = stretch_pad(w, h, pad);
        o->c[0] = 2.0f * (float)(w + 2u * r) / ((float)w * pw);
        o->c[4] = 2.0f * (float)(pad - r) / pw - 1.0f;
        break;
    }
    case NV2A_PLACE_COVER:
        k = pw / (float)w;
        o->c[0] = 2.0f / (float)w;
        o->c[4] = -1.0f;
        o->c[1] *= k;
        o->c[5] *= k;
        break;
    default:
        break;
    }
}

/* The tie shift (nv2a_backend_common.h) moves content, not only coverage:
 * a flat vertex carries its texcoords with it, so above 1.01 host pixels a
 * guest pixel every flat full-screen pass lands 0.5 - 0.25 / s guest pixels
 * up and left of its source (0.375 at 2x, about 0.29 under the 1x hor+
 * stretch). Chained bilinear passes (bloom, blur ping-pong) and frame
 * feedback can add that per pass. No vertex shift avoids it: covering host
 * pixel 0 takes at least 0.5 - 0.5 / s. The proper fix is a matching
 * texcoord correction, still to do. */
void nv2a_vp_tie_fill(struct nv2a_vp_consts *o, uint32_t hw, uint32_t hh)
{
    /* Host pixels a guest pixel: the NDC scale times half the viewport. */
    float sx = o->c[0] * (float)hw * 0.5f, sy = -o->c[1] * (float)hh * 0.5f;

    /* Scales within 1% of 1 are stock (rounding leaves 1x a hair off). */
    o->c[6] = sx > 1.01f ? 0.5f - 0.25f / sx : 0.0f;
    o->c[7] = sy > 1.01f ? 0.5f - 0.25f / sy : 0.0f;
}

/* The HUD layer's state (hud_anchor.h), for the backends: 0 while the
 * layer's active flag is clear, else bit 32 and the canvas's target size,
 * w << 16 | h. One word, so a backend never sees the flag with another
 * target's size. */
static uint64_t s_hud_state;
/* The last frame's guest size, w << 16 | h, as a backend presented it. */
static uint32_t s_frame_wh;

void nv2a_aspect_hud_set(int on, uint32_t tw, uint32_t th)
{
    uint64_t v = on && tw && th && tw < 65536u && th < 65536u
                 ? (uint64_t)1 << 32 | (uint64_t)tw << 16 | th : 0;
    __atomic_store_n(&s_hud_state, v, __ATOMIC_RELAXED);
}

int nv2a_aspect_hud_on(uint32_t w, uint32_t h)
{
    uint64_t v = __atomic_load_n(&s_hud_state, __ATOMIC_RELAXED);
    return v && (uint32_t)(v >> 16 & 0xFFFFu) == w && (uint32_t)(v & 0xFFFFu) == h;
}

int nv2a_aspect_hud_lift(uint32_t rt_w, uint32_t rt_h, int two_d, int flat, int full_w,
                         float x_lo, float x_hi)
{
    if (!two_d || flat || full_w || !nv2a_aspect_hud_on(rt_w, rt_h))
        return 0;
    /* Wholly inside the columns: the 4:3 box clips nothing, so the batch
     * keeps it and its trace line reads as without the layer. */
    return !(x_lo >= 0.0f && x_hi <= (float)rt_w);
}

void nv2a_aspect_frame_publish(uint32_t w, uint32_t h)
{
    if (w && h && w < 65536u && h < 65536u)
        __atomic_store_n(&s_frame_wh, w << 16 | h, __ATOMIC_RELAXED);
}

int nv2a_aspect_frame(uint32_t *w, uint32_t *h)
{
    uint32_t v = __atomic_load_n(&s_frame_wh, __ATOMIC_RELAXED);
    if (!v)
        return 0;
    *w = v >> 16;
    *h = v & 0xFFFFu;
    return 1;
}

void nv2a_aspect_box(const uint32_t box[4], uint32_t w, uint32_t h, uint32_t pad,
                     unsigned n, int place, int two_d, int lift, uint32_t out[4])
{
    uint32_t pw = w + 2u * pad;

    if (n < 1)
        n = 1;
    out[1] = box[1] * n;
    out[3] = box[3] * n;
    switch (pad ? place : NV2A_PLACE_STRETCH) {
    case NV2A_PLACE_STRETCH: {
        /* With the mapping: 0..w over the ratio's pad (nv2a_aspect_map). */
        uint32_t r = stretch_pad(w, h, pad);
        out[0] = (uint32_t)(((uint64_t)(pad - r) * w + (uint64_t)box[0] * (w + 2u * r)) * n / w);
        out[2] = (uint32_t)(((uint64_t)(pad - r) * w + (uint64_t)box[2] * (w + 2u * r)) * n / w);
        break;
    }
    case NV2A_PLACE_COVER:
        /* Scaled by pw / w with the mapping; COVER's y overflow is clipped by
         * the viewport, so its box is the stretched one. */
        out[0] = (uint32_t)((uint64_t)box[0] * pw * n / w);
        out[2] = (uint32_t)((uint64_t)box[2] * pw * n / w);
        break;
    default:
        out[0] = (box[0] + pad) * n;
        out[2] = (box[2] + pad) * n;
        /* Lifted 2D (nv2a_aspect_hud_lift) is boxed like the world: the
         * HUD layer clipped it in guest memory, and its moved elements
         * must reach the pads. */
        if (!two_d || lift) {
            if (box[0] == 0)
                out[0] = 0;
            if (box[2] >= w)
                out[2] = pw * n;
        }
        break;
    }
}

unsigned nv2a_host_size(uint32_t w, uint32_t h, unsigned scale, uint32_t max_dim,
                        uint32_t *hw, uint32_t *hh)
{
    unsigned n = scale > 1 ? scale : 1;
    while (n > 1 && ((uint64_t)w * n > max_dim || (uint64_t)h * n > max_dim))
        n--;
    *hw = w * n;
    *hh = h * n;
    return n;
}

int nv2a_present_rect(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                      int filter, struct nv2a_rect *r)
{
    uint64_t fw, fh;

    r->x = r->y = r->w = r->h = 0;
    if (!src_w || !src_h || !dst_w || !dst_h)
        return 0;
    if (filter == NV2A_PRESENT_INTEGER) {
        uint32_t k = dst_w / src_w, kh = dst_h / src_h;
        if (kh < k)
            k = kh;
        if (k >= 1) {
            r->w = (int32_t)(src_w * k);
            r->h = (int32_t)(src_h * k);
            r->x = (int32_t)(dst_w - (uint32_t)r->w) / 2;
            r->y = (int32_t)(dst_h - (uint32_t)r->h) / 2;
            return 1;
        }
    }
    /* Fit: the axis that limits gets the whole output, the other the
     * frame's aspect, rounded to the nearest pixel. */
    if ((uint64_t)dst_w * src_h <= (uint64_t)dst_h * src_w) {
        fw = dst_w;
        fh = ((uint64_t)dst_w * src_h + src_w / 2) / src_w;
    } else {
        fh = dst_h;
        fw = ((uint64_t)dst_h * src_w + src_h / 2) / src_h;
    }
    /* A frame a hair off the output's shape fills it: hor+ pads by whole
     * guest columns, so a 16:9 frame is 854 x 480 (2562 x 1440 at scale 3),
     * and its fit in a 16:9 window came out a row short, the cleared row
     * left at the bottom. Under 1/256 of the axis the stretch is
     * invisible; a real letterbox or pillarbox is far wider. An axis
     * already at 1:1 keeps its size: a NEAREST 640 x 480 frame in a 642 x
     * 480 window stays crisp behind its two one-column bars. */
    if ((dst_h - fh) * 256u < dst_h && fh != src_h)
        fh = dst_h;
    if ((dst_w - fw) * 256u < dst_w && fw != src_w)
        fw = dst_w;
    r->w = (int32_t)fw;
    r->h = (int32_t)fh;
    r->x = (int32_t)((dst_w - fw) / 2);
    r->y = (int32_t)((dst_h - fh) / 2);
    return 0;
}

int nv2a_present_chain_rect(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                            int filter, int surface_internal, struct nv2a_rect *r)
{
    if (surface_internal && filter == NV2A_PRESENT_INTEGER)
        filter = NV2A_PRESENT_LINEAR;
    return nv2a_present_rect(src_w, src_h, dst_w, dst_h, filter, r);
}

void nv2a_present_chain_source(uint32_t hw, uint32_t hh, unsigned n, int lines,
                               uint32_t *sw, uint32_t *sh)
{
    *sw = hw;
    *sh = hh;
    if (lines && n > 1 && hh >= n && hh % n == 0)
        *sh = hh / n;
}

void nv2a_present_crop(uint32_t fw, uint32_t fh, uint32_t tw, uint32_t th, unsigned n,
                       unsigned pad, unsigned ov, int filter, struct nv2a_crop *c)
{
    float cx, cy;

    if (n < 1)
        n = 1;
    c->x = c->y = 0.0f;
    c->w = (float)fw;
    c->h = (float)fh;
    c->u_off = c->v_off = 0.0f;
    c->u_scale = tw ? (float)fw / (float)tw : 1.0f;
    c->v_scale = th ? (float)fh / (float)th : 1.0f;
    c->fit_w = fw;
    c->fit_h = fh;
    if (!ov || pad || !fw || !fh || !tw || !th || fw / n <= 2u * ov)
        return;
    cx = (float)(ov * n);
    cy = cx * (float)fh / (float)fw;
    if (filter == NV2A_PRESENT_INTEGER)
        cy = (float)(uint32_t)(cy + 0.5f);     /* positive: rounds */
    c->x = cx;
    c->y = cy;
    c->w = (float)fw - 2.0f * cx;
    c->h = (float)fh - 2.0f * cy;
    c->u_off = cx / (float)tw;
    c->v_off = cy / (float)th;
    c->u_scale = c->w / (float)tw;
    c->v_scale = c->h / (float)th;
    if (filter == NV2A_PRESENT_INTEGER) {
        c->fit_w = fw - 2u * ov * n;
        c->fit_h = fh - 2u * (uint32_t)cy;
    }
}

void nv2a_upscale_nearest32(const uint8_t *src, uint32_t src_pitch, uint32_t w, uint32_t h,
                            unsigned n, uint8_t *dst, uint32_t dst_pitch)
{
    uint32_t x, y;
    unsigned i, j;
    for (y = 0; y < h; y++) {
        const uint32_t *s = (const uint32_t *)(const void *)(src + (size_t)y * src_pitch);
        uint32_t *d = (uint32_t *)(void *)(dst + (size_t)y * n * dst_pitch);
        for (x = 0; x < w; x++)
            for (i = 0; i < n; i++)
                d[(size_t)x * n + i] = s[x];
        for (j = 1; j < n; j++)
            memcpy(dst + ((size_t)y * n + j) * dst_pitch, d, (size_t)w * n * 4u);
    }
}

void nv2a_downscale_box32(const uint8_t *src, uint32_t src_pitch, uint32_t w, uint32_t h,
                          unsigned n, uint8_t *dst, uint32_t dst_pitch)
{
    uint32_t x, y, nn = n * n, half = nn / 2;
    unsigned i, j, c;
    for (y = 0; y < h; y++) {
        uint8_t *d = dst + (size_t)y * dst_pitch;
        for (x = 0; x < w; x++) {
            uint32_t sum[4] = {0, 0, 0, 0};
            for (j = 0; j < n; j++) {
                const uint8_t *s = src + ((size_t)y * n + j) * src_pitch + (size_t)x * n * 4u;
                for (i = 0; i < n; i++, s += 4)
                    for (c = 0; c < 4; c++)
                        sum[c] += s[c];
            }
            for (c = 0; c < 4; c++)
                d[(size_t)x * 4u + c] = (uint8_t)((sum[c] + half) / nn);
        }
    }
}

uint32_t nv2a_surf_pack_row(uint32_t format, const uint8_t *in, uint32_t w, uint8_t *out)
{
    uint32_t f = format & 0xFu, x, c;

    switch (f) {
    case 0x1: case 0x2: case 0x3:
        for (x = 0; x < w; x++) {
            uint16_t p;
            memcpy(&c, in + (size_t)x * 4u, 4);
            p = f == 0x3
                ? (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x1F))
                : (uint16_t)((f == 0x2 ? 0x8000u : 0u) | ((c >> 9) & 0x7C00)
                             | ((c >> 6) & 0x03E0) | ((c >> 3) & 0x1F));
            memcpy(out + (size_t)x * 2u, &p, 2);
        }
        return 2;
    case 0x4: case 0x5: case 0x6: case 0x7:
        for (x = 0; x < w; x++) {
            uint32_t a;
            memcpy(&c, in + (size_t)x * 4u, 4);
            a = f == 0x4 ? 0u : f == 0x5 ? 0xFFu : (f == 0x7 ? 0x80u : 0u) | (c >> 25);
            c = (c & 0x00FFFFFFu) | (a << 24);
            memcpy(out + (size_t)x * 4u, &c, 4);
        }
        return 4;
    case 0x8: case 0xC:
        if (out != in)
            memcpy(out, in, (size_t)w * 4u);
        return 4;
    default:
        return 0;
    }
}

void nv2a_bump_trace(const struct nv2a_pb_vsh *v, const struct nv2a_stage st[4],
                     uint32_t flip)
{
    /* Distinct configurations, by FNV-1a of their fields; past the table's
     * end nothing more is printed (a title has a handful). */
    static uint32_t seen[512];
    static unsigned seen_n;
    int n;

    for (n = 1; n < 4; n++) {
        const struct nv2a_stage *t = &st[n], *in;
        uint32_t f[12], h = 0x811c9dc5u;
        unsigned i, k;
        int is;

        if (t->mode != 6 && t->mode != 7)
            continue;
        is = nv2a_stage_input(n, v->other_stage_input);
        in = &st[is];
        f[0] = (uint32_t)n | (uint32_t)t->mode << 4 | (uint32_t)is << 8;
        memcpy(&f[1], t->bump, 16);
        memcpy(&f[5], &t->bump_scale, 4);
        memcpy(&f[6], &t->bump_offset, 4);
        f[7] = t->color;
        f[8] = t->width | t->height << 16;
        f[9] = in->color;
        f[10] = in->width | in->height << 16;
        f[11] = (uint32_t)in->mode;
        for (i = 0; i < sizeof f; i++)
            h = (h ^ ((const uint8_t *)f)[i]) * 0x01000193u;
        for (k = 0; k < seen_n && seen[k] != h; k++)
            ;
        if (k < seen_n || seen_n == sizeof seen / sizeof seen[0])
            continue;
        seen[seen_n++] = h;
        /* As stored, slots 00 01 11 10; m01 != m10 with both non-zero is
         * what tells the transpose apart (a diagonal matrix only shows
         * that slot 2 is m11). */
        fprintf(stderr, "[BUMP] flip %u stage %d mode %d input %d (mode %d fmt 0x%02X %ux%u)"
                " slots %g %g %g %g (m00 %g m01 %g m10 %g m11 %g)%s scale %g offset %g"
                " fmt 0x%02X %ux%u\n", flip, n, t->mode, is, in->mode, in->color,
                in->width, in->height, t->bump[0], t->bump[1], t->bump[2], t->bump[3],
                t->bump[0], t->bump[1], t->bump[3], t->bump[2],
                t->bump[1] == 0.0f && t->bump[3] == 0.0f ? " diagonal"
                : t->bump[1] != t->bump[3] && t->bump[1] != 0.0f && t->bump[3] != 0.0f
                    ? " transpose-telling" : "",
                t->bump_scale, t->bump_offset, t->color, t->width, t->height);
    }
}

#ifdef XBOXRECOMP_ENHANCE
/* The HUD capture's requests (nv2a_backend_common.h). One guest thread
 * posts and polls, the walker takes; a slot's state is the handover. */
static struct {
    int state;                  /* atomic */
    uint32_t flip;
    char path[NV2A_CAP_PATH];
} s_cap[NV2A_CAP_SLOTS];
int nv2a_cap_live;
static uint32_t s_cap_flips;
static const struct nv2a_cap_kernel *s_cap_k;

void nv2a_cap_set_kernel(const struct nv2a_cap_kernel *k)
{
    __atomic_store_n(&s_cap_k, k, __ATOMIC_RELEASE);
}

const struct nv2a_cap_kernel *nv2a_cap_kernel(void)
{
    return __atomic_load_n(&s_cap_k, __ATOMIC_ACQUIRE);
}

void nv2a_cap_note_flip(uint32_t flip)
{
    __atomic_store_n(&s_cap_flips, flip, __ATOMIC_RELEASE);
}

uint32_t nv2a_cap_flips(void)
{
    return __atomic_load_n(&s_cap_flips, __ATOMIC_ACQUIRE);
}

int nv2a_cap_post(uint32_t flip, const char *path)
{
    size_t n = path ? strlen(path) : NV2A_CAP_PATH;

    if (n >= NV2A_CAP_PATH)
        return -1;
    for (unsigned i = 0; i < NV2A_CAP_SLOTS; i++) {
        if (__atomic_load_n(&s_cap[i].state, __ATOMIC_ACQUIRE) != NV2A_CAP_FREE)
            continue;
        s_cap[i].flip = flip;
        memcpy(s_cap[i].path, path, n + 1);
        __atomic_add_fetch(&nv2a_cap_live, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&s_cap[i].state, NV2A_CAP_LIVE, __ATOMIC_RELEASE);
        return (int)i;
    }
    return -1;
}

int nv2a_cap_poll(int slot, int retract)
{
    int st;

    if (slot < 0 || (unsigned)slot >= NV2A_CAP_SLOTS)
        return -1;
    st = __atomic_load_n(&s_cap[slot].state, __ATOMIC_ACQUIRE);
    if (st == NV2A_CAP_LIVE && retract) {
        int live = NV2A_CAP_LIVE;
        if (__atomic_compare_exchange_n(&s_cap[slot].state, &live, NV2A_CAP_FREE, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            __atomic_sub_fetch(&nv2a_cap_live, 1, __ATOMIC_RELAXED);
            return NV2A_CAP_DEAD;
        }
        st = live;
    }
    if (st != NV2A_CAP_DONE && st != NV2A_CAP_FAILED && st != NV2A_CAP_DEAD)
        return -1;
    __atomic_store_n(&s_cap[slot].state, NV2A_CAP_FREE, __ATOMIC_RELEASE);
    return st;
}

int nv2a_cap_take(uint32_t flip, char *path, size_t len)
{
    int got = -1;

    for (unsigned i = 0; i < NV2A_CAP_SLOTS; i++) {
        int live = NV2A_CAP_LIVE;
        int32_t d;
        if (__atomic_load_n(&s_cap[i].state, __ATOMIC_ACQUIRE) != NV2A_CAP_LIVE)
            continue;
        d = (int32_t)(s_cap[i].flip - flip);
        if (d > 0 || (d == 0 && got >= 0))
            continue;
        if (!__atomic_compare_exchange_n(&s_cap[i].state, &live,
                                         d == 0 ? NV2A_CAP_TAKEN : NV2A_CAP_DEAD, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;           /* retracted meanwhile */
        __atomic_sub_fetch(&nv2a_cap_live, 1, __ATOMIC_RELAXED);
        if (d == 0) {
            snprintf(path, len, "%s", s_cap[i].path);
            got = (int)i;
        }
    }
    return got;
}

void nv2a_cap_done(int slot, int ok)
{
    if (slot >= 0 && (unsigned)slot < NV2A_CAP_SLOTS)
        __atomic_store_n(&s_cap[slot].state, ok ? NV2A_CAP_DONE : NV2A_CAP_FAILED,
                         __ATOMIC_RELEASE);
}

void nv2a_cap_reset(void)
{
    for (unsigned i = 0; i < NV2A_CAP_SLOTS; i++)
        __atomic_store_n(&s_cap[i].state, NV2A_CAP_FREE, __ATOMIC_RELEASE);
    __atomic_store_n(&nv2a_cap_live, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_cap_flips, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_cap_k, NULL, __ATOMIC_RELEASE);
}

nv2a_flip_hook_fn nv2a_flip_hook_installed;

void nv2a_flip_hook_set(nv2a_flip_hook_fn fn)
{
    __atomic_store_n(&nv2a_flip_hook_installed, fn, __ATOMIC_RELEASE);
}

nv2a_batch_filter_fn nv2a_batch_filter_installed;

void nv2a_batch_filter_set(nv2a_batch_filter_fn fn)
{
    __atomic_store_n(&nv2a_batch_filter_installed, fn, __ATOMIC_RELEASE);
}

int nv2a_batch_filter_ask(const struct nv2a_pb_draw *d, const struct nv2a_pb_gpu *g,
                          const struct nv2a_pb_vsh *v)
{
    /* The hash walks the whole program; programs only change by upload
     * (prog_dwords) or a new start slot, as the backends' shader caches
     * know. Ack thread only, like every caller of on_draw. */
    static struct { int valid; uint32_t dwords, start, hash; } memo;
    struct nv2a_batch_info b;
    nv2a_batch_filter_fn fn = __atomic_load_n(&nv2a_batch_filter_installed, __ATOMIC_ACQUIRE);

    if (!fn)
        return 0;
    memset(&b, 0, sizeof b);
    if (d->program) {
        if (!memo.valid || memo.dwords != v->prog_dwords || memo.start != v->start) {
            memo.hash = nv2a_vsh_program_hash(v, NULL);
            memo.dwords = v->prog_dwords;
            memo.start = v->start;
            memo.valid = 1;
        }
        b.program_hash = memo.hash;
        b.program = 1;
    }
    b.prim = d->prim;
    b.idx_count = d->idx_count;
    b.color_offset = g->color_offset;
    b.surface_w = g->clip_x + g->clip_w;
    b.surface_h = g->clip_y + g->clip_h;
    b.surface_format = g->format;
    b.depth_test = v->depth_enable;
    b.depth_write = v->depth_mask;
    b.stencil_test = v->stencil_enable;
    b.alpha_test = v->atest_enable;
    b.blend = g->blend_enable != 0;
    b.blend_sfactor = g->blend_sfactor;
    b.blend_dfactor = g->blend_dfactor;
    b.color_keep = g->color_keep;
    b.flip = g->flips;
    b.present_offset = nv2a_pb_present_state()->offset;
    b.draw = d;
    return fn(&b) != 0;
}
#endif

int nv2a_aspect_uv_tracks(const float (*xy)[2], const float (*uv)[2], uint32_t n, float su,
                          float sv, uint32_t w, uint32_t h)
{
    float lo[2], hi[2], dlo[2], dhi[2], sum[2] = {0, 0};
    uint32_t i, a;

    if (!n)
        return 0;
    for (i = 0; i < n; i++) {
        float d[2];
        d[0] = uv[i][0] * su - xy[i][0];
        d[1] = uv[i][1] * sv - xy[i][1];
        if (!isfinite(d[0]) || !isfinite(d[1]))
            return 0;
        for (a = 0; a < 2; a++) {
            if (!i || xy[i][a] < lo[a]) lo[a] = xy[i][a];
            if (!i || xy[i][a] > hi[a]) hi[a] = xy[i][a];
            if (!i || d[a] < dlo[a]) dlo[a] = d[a];
            if (!i || d[a] > dhi[a]) dhi[a] = d[a];
            sum[a] += d[a];
        }
    }
    for (a = 0; a < 2; a++) {
        float frame = (float)(a ? h : w);
        if (dhi[a] - dlo[a] > (hi[a] - lo[a]) / 8.0f + 2.0f)
            return 0;
        if (fabsf(sum[a] / (float)n) > frame / 16.0f)
            return 0;
    }
    return 1;
}
