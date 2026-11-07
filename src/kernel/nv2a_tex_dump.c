/*
 * nv2a_tex_dump.c - see nv2a_tex_dump.h.
 */
#include "nv2a_tex_dump.h"
#include "nv2a_backend_common.h"
#include "nv2a_tex_pack.h"   /* NV2A_TEX_PACK_MAX_DIM */
#include "recomp_env.h"
#include "recomp_exit_hook.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define dump_mkdir(p) _mkdir(p)
#else
#include <time.h>
#define dump_mkdir(p) mkdir((p), 0755)
#endif

/* -1 not read yet, 0 off, 1 on. */
static int  s_on = -1;
static char s_dir[1024];
static uint64_t s_flip;
static struct nv2a_tex_dump_stats s_st;
/* Something counts.json or volatile.json would show differently. */
static int s_dirty;
/* Held by every table change and by the summary, which also runs at exit
 * on another thread than the walker's. Uncontended on the upload path. */
static char s_lock;
/* The .xtd bytes allowed in one run, and why the dump stopped (NULL while
 * it runs): past the cap, or out of memory for its tables. */
static uint64_t    s_max_bytes = NV2A_TEX_DUMP_MAX_BYTES;
static const char *s_stop;

/* Hashes seen: one slot each, open addressing, grown at half full. */
typedef struct {
    uint64_t hash;
    uint32_t uploads;
    uint8_t  used;
    uint8_t  done;   /* its files are written; else the next upload retries */
    uint8_t  tries;  /* failed writes, given up on at DUMP_TRIES */
    /* The facts only a draw or a target bind can tell, after the first upload
     * wrote the sidecar; `dirty` is a sidecar to rewrite. */
    uint8_t  drawn;  /* DRAWN_* */
    uint8_t  bump, rt, dirty;
} HashSlot;
enum { DRAWN_NONE, DRAWN_2D, DRAWN_3D };   /* 3D wins: one 3D draw and it is not 2D */
#define DUMP_TRIES 3u
static HashSlot *s_hash;
static size_t    s_hash_cap, s_hash_n;

/* Addresses: the distinct hashes each took, up to the volatile limit. */
typedef struct {
    uint32_t offset, width, height;
    uint32_t n;              /* distinct hashes, capped at the limit + 1 */
    uint64_t hashes[NV2A_TEX_DUMP_VOLATILE];
    uint8_t  used, vol;
} AddrSlot;
static AddrSlot *s_addr;
static size_t    s_addr_cap, s_addr_n;

/* Render targets bound, distinct by (offset, size, format): a few hundred
 * in a title at most, so a fixed table; past it only the binds count, as
 * s_st.rt_dropped too, and such a target marks no `rt`. */
#define RT_SEEN 1024u
/* The bits the backends' rt_texture compares: the same memory is seen through
 * more than one window of the GPU's address space. */
#define RT_ADDR_MASK 0x07FFFFFFu
static struct { uint32_t offset, width, height, color; uint64_t binds; } s_rt[RT_SEEN];
static unsigned s_rt_n;

static void lock(void)
{
    while (__atomic_test_and_set(&s_lock, __ATOMIC_ACQUIRE))
        ;
}

static void unlock(void)
{
    __atomic_clear(&s_lock, __ATOMIC_RELEASE);
}

/* The exit path's lock: ExitProcess may have ended the walker thread
 * inside a write, holding the lock forever, so the exit summary waits
 * about 200 ms for it and is skipped when it does not come. */
static int lock_bounded(void)
{
    int i;
    for (i = 0; i < 200; i++) {
        if (!__atomic_test_and_set(&s_lock, __ATOMIC_ACQUIRE))
            return 1;
#ifdef _WIN32
        Sleep(1);
#else
        {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
#endif
    }
    return 0;
}

static size_t mix(uint64_t h)
{
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    return (size_t)h;
}

static HashSlot *hash_slot(uint64_t hash, int add)
{
    size_t i;
    if (add && (s_hash_n + 1) * 2 > s_hash_cap) {
        size_t cap = s_hash_cap ? s_hash_cap * 2 : 1024, k;
        HashSlot *t = (HashSlot *)calloc(cap, sizeof *t);
        if (!t)
            return NULL;
        for (k = 0; k < s_hash_cap; k++) {
            if (!s_hash[k].used)
                continue;
            i = mix(s_hash[k].hash) & (cap - 1);
            while (t[i].used)
                i = (i + 1) & (cap - 1);
            t[i] = s_hash[k];
        }
        free(s_hash);
        s_hash = t;
        s_hash_cap = cap;
    }
    if (!s_hash_cap)
        return NULL;
    i = mix(hash) & (s_hash_cap - 1);
    while (s_hash[i].used) {
        if (s_hash[i].hash == hash)
            return &s_hash[i];
        i = (i + 1) & (s_hash_cap - 1);
    }
    if (!add)
        return NULL;
    s_hash[i].used = 1;
    s_hash[i].hash = hash;
    s_hash[i].uploads = 0;
    s_hash_n++;
    return &s_hash[i];
}

static size_t addr_key(uint32_t offset, uint32_t w, uint32_t h)
{
    return mix(((uint64_t)offset << 32) ^ ((uint64_t)w << 16) ^ h);
}

static AddrSlot *addr_slot(uint32_t offset, uint32_t w, uint32_t h, int add)
{
    size_t i;
    if (add && (s_addr_n + 1) * 2 > s_addr_cap) {
        size_t cap = s_addr_cap ? s_addr_cap * 2 : 1024, k;
        AddrSlot *t = (AddrSlot *)calloc(cap, sizeof *t);
        if (!t)
            return NULL;
        for (k = 0; k < s_addr_cap; k++) {
            if (!s_addr[k].used)
                continue;
            i = addr_key(s_addr[k].offset, s_addr[k].width, s_addr[k].height) & (cap - 1);
            while (t[i].used)
                i = (i + 1) & (cap - 1);
            t[i] = s_addr[k];
        }
        free(s_addr);
        s_addr = t;
        s_addr_cap = cap;
    }
    if (!s_addr_cap)
        return NULL;
    i = addr_key(offset, w, h) & (s_addr_cap - 1);
    while (s_addr[i].used) {
        AddrSlot *a = &s_addr[i];
        if (a->offset == offset && a->width == w && a->height == h)
            return a;
        i = (i + 1) & (s_addr_cap - 1);
    }
    if (!add)
        return NULL;
    memset(&s_addr[i], 0, sizeof s_addr[i]);
    s_addr[i].used = 1;
    s_addr[i].offset = offset;
    s_addr[i].width = w;
    s_addr[i].height = h;
    s_addr_n++;
    return &s_addr[i];
}

static int addr_has(const AddrSlot *a, uint64_t hash)
{
    uint32_t k, n = a->n < NV2A_TEX_DUMP_VOLATILE ? a->n : NV2A_TEX_DUMP_VOLATILE;
    for (k = 0; k < n; k++)
        if (a->hashes[k] == hash)
            return 1;
    return 0;
}

/* mkdir -p: the directory and its parents. */
static int make_dirs(const char *dir)
{
    char tmp[sizeof s_dir];
    size_t i, n = strlen(dir);
    struct stat st;
    if (!n || n >= sizeof tmp)
        return -1;
    memcpy(tmp, dir, n + 1);
    for (i = 1; i <= n; i++) {
        if (tmp[i] != '/' && tmp[i] != '\\' && tmp[i] != '\0')
            continue;
        /* "C:" alone is a drive, not a directory to make. */
        if (i == 2 && tmp[1] == ':')
            continue;
        {
            char c = tmp[i];
            tmp[i] = '\0';
            if (dump_mkdir(tmp) != 0 && errno != EEXIST) {
                tmp[i] = c;
                return -1;
            }
            tmp[i] = c;
        }
    }
    return stat(dir, &st) == 0 && (st.st_mode & S_IFDIR) ? 0 : -1;
}

static void summary(int exiting);

/* The magnification trace: -1 not read yet, 0 off, 1 on. One slot a hash,
 * with or without the dump; a slot's dirty mark is a sidecar to rewrite. */
static int s_mag = -1;
typedef struct {
    uint64_t hash, draws;
    double   mag_max;
    unsigned render_scale;
    uint8_t  used, dirty;
} MagSlot;
static MagSlot *s_mag_tab;
static size_t   s_mag_cap, s_mag_n;
static uint64_t s_mag_dropped;

static void at_exit(void)
{
    if (s_on > 0 || s_mag > 0)
        summary(1);
}

static void at_exit_hook(const char *why)
{
    (void)why;
    at_exit();
}

static void reset(void)
{
    free(s_hash);
    free(s_addr);
    s_hash = NULL;
    s_addr = NULL;
    s_hash_cap = s_hash_n = s_addr_cap = s_addr_n = 0;
    s_rt_n = 0;
    s_flip = 0;
    s_dirty = 0;
    s_stop = NULL;
    memset(&s_st, 0, sizeof s_st);
    free(s_mag_tab);
    s_mag_tab = NULL;
    s_mag_cap = s_mag_n = 0;
    s_mag_dropped = 0;
}

static void hook_exit_once(void)
{
    static int hooked;
    if (!hooked) {
        hooked = 1;
        atexit(at_exit);
        recomp_exit_hook_add(at_exit_hook);   /* the windows' ExitProcess */
    }
}

int nv2a_tex_dump_set_dir(const char *dir)
{
    size_t n;
    lock();
    reset();
    s_on = 0;
    s_dir[0] = '\0';
    if (dir && *dir) {
        n = strlen(dir);
        while (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
            n--;
        if (n < sizeof s_dir - 64) {
            memcpy(s_dir, dir, n);
            s_dir[n] = '\0';
            if (make_dirs(s_dir) == 0)
                s_on = 1;
        }
        if (!s_on)
            fprintf(stderr, "[TEXDUMP] tex_dump=%s: not a directory that can be made;"
                    " texture dump off\n", dir);
    }
    unlock();
    if (s_on > 0)
        hook_exit_once();
    if (s_on > 0) {
        fprintf(stderr, "[TEXDUMP] dumping each uploaded texture once per hash to %s\n",
                s_dir);
        fflush(stderr);
    }
    return s_on > 0 ? 0 : -1;
}

int nv2a_tex_dump_on(void)
{
    if (s_on < 0)
        nv2a_tex_dump_set_dir(recomp_env(RENV_TEX_DUMP));
    return s_on > 0;
}

static int is_linear(uint32_t color)
{
    return !d3d8_format_is_swizzled(color) && !d3d8_format_dxt_block_bytes(color);
}

/* What an upload is, before any side effect. */
enum { UP_WRITE, UP_REPEAT, UP_LINEAR, UP_VOLATILE, UP_OVERSIZE };

static int classify(const struct nv2a_stage *t, uint64_t hash)
{
    const AddrSlot *a;
    const HashSlot *h;
    if (t->width > NV2A_TEX_PACK_MAX_DIM || t->height > NV2A_TEX_PACK_MAX_DIM)
        return UP_OVERSIZE;
    a = addr_slot(t->offset, t->width, t->height, 0);
    if (a && (a->vol || (a->n >= NV2A_TEX_DUMP_VOLATILE && !addr_has(a, hash))))
        return UP_VOLATILE;
    h = hash_slot(hash, 0);
    if (h && (h->done || h->tries >= DUMP_TRIES))
        return UP_REPEAT;
    return is_linear(t->color) ? UP_LINEAR : UP_WRITE;
}

int nv2a_tex_dump_wants(const struct nv2a_stage *t, uint64_t hash)
{
    int c;
    if (s_on <= 0 || !t)
        return 0;
    lock();
    c = s_stop ? UP_REPEAT : classify(t, hash);
    unlock();
    return c == UP_WRITE;
}

static const char *fmt_name(uint32_t c)
{
    /* NV097_SET_TEXTURE_FORMAT_COLOR, by the D3D names the CLI and the
     * pack docs use; LIN_ for the linear (rect) ones. */
    switch (c) {
    case 0x00: return "L8";
    case 0x01: return "AL8";
    case 0x02: return "A1R5G5B5";
    case 0x03: return "X1R5G5B5";
    case 0x04: return "A4R4G4B4";
    case 0x05: return "R5G6B5";
    case 0x06: return "A8R8G8B8";
    case 0x07: return "X8R8G8B8";
    case 0x0B: return "P8";
    case 0x0C: return "DXT1";
    case 0x0E: return "DXT3";
    case 0x0F: return "DXT5";
    case 0x10: return "LIN_A1R5G5B5";
    case 0x11: return "LIN_R5G6B5";
    case 0x12: return "LIN_A8R8G8B8";
    case 0x13: return "LIN_L8";
    case 0x16: return "LIN_R8B8";
    case 0x17: return "LIN_G8B8";
    case 0x19: return "A8";
    case 0x1A: return "A8L8";
    case 0x1B: return "LIN_AL8";
    case 0x1C: return "LIN_X1R5G5B5";
    case 0x1D: return "LIN_A4R4G4B4";
    case 0x1E: return "LIN_X8R8G8B8";
    case 0x1F: return "LIN_A8";
    case 0x20: return "LIN_A8L8";
    case 0x24: return "LIN_YUY2";
    case 0x25: return "LIN_UYVY";
    case 0x27: return "R6G5B5";
    case 0x28: return "G8B8";
    case 0x29: return "R8B8";
    case 0x32: return "L16";
    case 0x35: return "LIN_L16";
    case 0x37: return "LIN_R6G5B5";
    case 0x38: return "R5G5B5A1";
    case 0x39: return "R4G4B4A4";
    case 0x3A: return "A8B8G8R8";
    case 0x3B: return "B8G8R8A8";
    case 0x3C: return "R8G8B8A8";
    case 0x3D: return "LIN_R5G5B5A1";
    case 0x3E: return "LIN_R4G4B4A4";
    case 0x3F: return "LIN_A8B8G8R8";
    case 0x40: return "LIN_B8G8R8A8";
    case 0x41: return "LIN_R8G8B8A8";
    default:   return NULL;
    }
}

/* SET_TEXTURE_ADDRESS modes, as nv2a_stage keeps them. */
static const char *wrap_name(uint32_t m)
{
    switch (m) {
    case 1:  return "wrap";
    case 2:  return "mirror";
    case 3:  return "clamp_edge";
    case 4:  return "border";
    case 5:  return "clamp";
    default: return "unknown";
    }
}

/* SET_TEXTURE_FILTER MIN (bits 16-21) and MAG (24-27). */
static const char *min_name(uint32_t f)
{
    switch ((f >> 16) & 0x3Fu) {
    case 1:  return "point";
    case 2:  return "linear";
    case 3:  return "point_mip_point";
    case 4:  return "linear_mip_point";
    case 5:  return "point_mip_linear";
    case 6:  return "linear_mip_linear";
    case 7:  return "convolution";
    default: return "unknown";
    }
}

static const char *mag_name(uint32_t f)
{
    switch ((f >> 24) & 0xFu) {
    case 1:  return "point";
    case 2:  return "linear";
    case 4:  return "convolution";
    default: return "unknown";
    }
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t layout_flags(const struct nv2a_stage *t)
{
    uint32_t f = 0;
    if (d3d8_format_dxt_block_bytes(t->color))
        f |= XTD_FLAG_DXT;
    else if (d3d8_format_is_swizzled(t->color))
        f |= XTD_FLAG_SWIZZLED;
    else
        f |= XTD_FLAG_LINEAR;
    if (t->color == NV2A_TEX_P8)
        f |= XTD_FLAG_PALETTE;
    return f;
}

/* Written to <name>.tmp and renamed, so a reader (or a killed run) never
 * sees half a file under the real name. */
static FILE *open_tmp(char *tmp, size_t len, const char *name)
{
    snprintf(tmp, len, "%s/%s.tmp", s_dir, name);
    return fopen(tmp, "wb");
}

static int commit_tmp(const char *tmp, const char *name, FILE *f)
{
    char path[sizeof s_dir + 64];
    int ok = !ferror(f);
    if (fclose(f) != 0)
        ok = 0;
    snprintf(path, sizeof path, "%s/%s", s_dir, name);
    if (ok) {
#ifdef _WIN32
        /* Windows' rename does not replace; POSIX's does, atomically. */
        remove(path);
#endif
        ok = rename(tmp, path) == 0;
    }
    if (!ok)
        remove(tmp);
    return ok;
}

static int write_xtd(const struct nv2a_stage *t, uint64_t hash, const uint32_t *levels,
                     size_t texels)
{
    uint8_t hdr[NV2A_TEX_DUMP_HDR];
    char name[48], tmp[sizeof s_dir + 64];
    FILE *f;
    size_t l;

    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, NV2A_TEX_DUMP_MAGIC, 4);
    wr32(hdr + 4, NV2A_TEX_DUMP_VERSION);
    wr64(hdr + 8, hash);
    wr32(hdr + 16, t->width);
    wr32(hdr + 20, t->height);
    wr32(hdr + 24, t->levels);
    wr32(hdr + 28, t->color);
    wr32(hdr + 32, t->pitch);
    wr32(hdr + 36, layout_flags(t));
    wr32(hdr + 40, (uint32_t)s_flip);
    wr32(hdr + 44, NV2A_TEX_DUMP_HDR);
    wr64(hdr + 48, (uint64_t)texels);
    snprintf(name, sizeof name, "%016llX.xtd", (unsigned long long)hash);
    if (!(f = open_tmp(tmp, sizeof tmp, name)))
        return 0;
    fwrite(hdr, 1, sizeof hdr, f);
    /* A8R8G8B8 as little-endian words, the host order of every target. */
    for (l = 0; l < texels; l += 65536) {
        size_t k = texels - l < 65536 ? texels - l : 65536;
        fwrite(levels + l, 4, k, f);
    }
    return commit_tmp(tmp, name, f);
}

static void print_facts(FILE *f, uint64_t hash);
static MagSlot *mag_slot(uint64_t hash, int add);

static int write_json(const struct nv2a_stage *t, uint64_t hash, int texels)
{
    char name[48], tmp[sizeof s_dir + 64], fbuf[8];
    const char *fn = fmt_name(t->color);
    uint32_t fl = layout_flags(t);
    FILE *f;

    if (!fn) {
        snprintf(fbuf, sizeof fbuf, "fmt%02X", t->color & 0xFFu);
        fn = fbuf;
    }
    snprintf(name, sizeof name, "%016llX.json", (unsigned long long)hash);
    if (!(f = open_tmp(tmp, sizeof tmp, name)))
        return 0;
    fprintf(f,
            "{\n"
            "  \"hash\": \"%016llX\",\n"
            "  \"width\": %u,\n"
            "  \"height\": %u,\n"
            "  \"levels\": %u,\n"
            "  \"format\": \"%s\",\n"
            "  \"format_id\": %u,\n"
            "  \"pitch\": %u,\n"
            "  \"layout\": \"%s\",\n"
            "  \"palette\": %s,\n"
            "  \"palette_len\": %u,\n"
            "  \"wrap_u\": \"%s\",\n"
            "  \"wrap_v\": \"%s\",\n"
            "  \"min_filter\": \"%s\",\n"
            "  \"mag_filter\": \"%s\",\n"
            "  \"lod_min\": %u,\n"
            "  \"lod_max\": %u,\n"
            "  \"cube\": %s,\n"
            "  \"offset\": \"0x%08X\",\n"
            "  \"first_flip\": %llu,\n"
            "  \"texels\": %s",
            (unsigned long long)hash, t->width, t->height, t->levels, fn, t->color,
            t->pitch,
            fl & XTD_FLAG_DXT ? "dxt" : fl & XTD_FLAG_SWIZZLED ? "swizzled" : "linear",
            fl & XTD_FLAG_PALETTE ? "true" : "false",
            fl & XTD_FLAG_PALETTE ? t->pal_len : 0u,
            wrap_name(t->addr_u), wrap_name(t->addr_v), min_name(t->filter),
            mag_name(t->filter), t->lod_min, t->lod_max, t->cube ? "true" : "false",
            t->offset, (unsigned long long)s_flip, texels ? "true" : "false");
    print_facts(f, hash);
    fputs("\n}\n", f);
    return commit_tmp(tmp, name, f);
}

/* Stops the dump for the rest of the run, with one line saying why; the
 * uploads after it are only counted. Under the lock. */
static void stop_locked(const char *why)
{
    s_stop = why;
    fprintf(stderr, "[TEXDUMP] dump stopped: %s; %llu written (%llu MiB)\n", why,
            (unsigned long long)s_st.written, (unsigned long long)(s_st.bytes >> 20));
    fflush(stderr);
}

static void remove_file(uint64_t hash, const char *ext)
{
    char path[sizeof s_dir + 64];
    snprintf(path, sizeof path, "%s/%016llX.%s", s_dir, (unsigned long long)hash, ext);
    remove(path);
}

/* Whether a stage has been bound to a render target at this address and size. */
static int rt_bound(uint32_t offset, uint32_t width, uint32_t height)
{
    unsigned i;
    for (i = 0; i < s_rt_n; i++)
        if (((s_rt[i].offset ^ offset) & RT_ADDR_MASK) == 0 && s_rt[i].width == width
                && s_rt[i].height == height)
            return 1;
    return 0;
}

void nv2a_tex_dump_upload(const struct nv2a_stage *t, uint64_t hash,
                          const uint32_t *levels)
{
    AddrSlot *a;
    HashSlot *h;
    int c;

    if (s_on <= 0 || !t)
        return;
    lock();
    s_st.uploads++;
    if (s_stop) {
        s_st.stopped++;
        unlock();
        return;
    }
    c = classify(t, hash);
    if (c == UP_OVERSIZE) {
        s_st.oversize++;
        unlock();
        return;
    }
    if (c == UP_VOLATILE) {
        s_st.volatile_skips++;
        a = addr_slot(t->offset, t->width, t->height, 0);
        if (a && !a->vol) {
            /* The hash past the limit: the address is given up on now. */
            a->vol = 1;
            a->n = NV2A_TEX_DUMP_VOLATILE + 1;
            s_st.volatile_addrs++;
            s_dirty = 1;
        }
        unlock();
        return;
    }
    a = addr_slot(t->offset, t->width, t->height, 1);
    h = hash_slot(hash, 1);
    if (!a || !h) {
        /* Without the tables every upload would look new and be rewritten. */
        stop_locked("out of memory for its tables");
        s_st.stopped++;
        unlock();
        return;
    }
    if (!addr_has(a, hash) && a->n < NV2A_TEX_DUMP_VOLATILE)
        a->hashes[a->n++] = hash;
    h->uploads++;
    if (!h->rt && rt_bound(t->offset, t->width, t->height)) {
        h->rt = 1;
        h->dirty = 1;
    }
    s_dirty = 1;
    if (c == UP_REPEAT) {
        s_st.repeats++;
        unlock();
        return;
    }
    if (c == UP_LINEAR) {
        if (write_json(t, hash, 0)) {
            h->done = 1;
            s_st.linear++;
        } else {
            h->tries++;
            s_st.errors++;
        }
        unlock();
        return;
    }
    if (!levels) {
        /* The caller did not decode though wants said it would be written. */
        h->tries++;
        s_st.errors++;
    } else {
        size_t n = nv2a_tex_texels(t->width, t->height, t->levels);
        uint64_t bytes = NV2A_TEX_DUMP_HDR + 4ull * n;
        if (s_st.bytes + bytes > s_max_bytes) {
            static char why[64];
            snprintf(why, sizeof why, "it reached its %llu MiB cap",
                     (unsigned long long)(s_max_bytes >> 20));
            stop_locked(why);
            s_st.stopped++;
        } else if (!write_xtd(t, hash, levels, n)) {
            h->tries++;
            s_st.errors++;
        } else if (!write_json(t, hash, 1)) {
            /* An .xtd without its sidecar is not a dump entry; the next
             * upload of the hash writes both again. */
            remove_file(hash, "xtd");
            h->tries++;
            s_st.errors++;
        } else {
            h->done = 1;
            s_st.written++;
            s_st.texels += n;
            s_st.bytes += bytes;
        }
    }
    unlock();
}

void nv2a_tex_dump_rt(const struct nv2a_stage *t)
{
    unsigned i;
    if (s_on <= 0 || !t)
        return;
    lock();
    s_st.rt_binds++;
    s_dirty = 1;
    for (i = 0; i < s_rt_n; i++)
        if (((s_rt[i].offset ^ t->offset) & RT_ADDR_MASK) == 0 && s_rt[i].width == t->width
                && s_rt[i].height == t->height && s_rt[i].color == t->color)
            break;
    if (i == s_rt_n && s_rt_n < RT_SEEN) {
        s_rt[s_rt_n].offset = t->offset;
        s_rt[s_rt_n].width = t->width;
        s_rt[s_rt_n].height = t->height;
        s_rt[s_rt_n].color = t->color;
        s_rt[s_rt_n].binds = 0;
        s_rt_n++;
        s_st.rt_targets++;
        /* What was uploaded from this memory before the game rendered there.
         * By address, not by a hash's latest: one hash can sit at two. */
        for (size_t k = 0; k < s_addr_cap; k++) {
            const AddrSlot *a = &s_addr[k];
            uint32_t j, n;
            if (!a->used || ((a->offset ^ t->offset) & RT_ADDR_MASK) || a->width != t->width
                    || a->height != t->height)
                continue;
            n = a->n < NV2A_TEX_DUMP_VOLATILE ? a->n : NV2A_TEX_DUMP_VOLATILE;
            for (j = 0; j < n; j++) {
                HashSlot *h = hash_slot(a->hashes[j], 0);
                if (h && !h->rt) {
                    h->rt = 1;
                    h->dirty = 1;
                }
            }
        }
    }
    if (i < s_rt_n)
        s_rt[i].binds++;
    else
        s_st.rt_dropped++;
    unlock();
}

int nv2a_tex_dump_draw_open(const uint64_t hash[NV2A_TEX_DUMP_STAGES])
{
    int k, open = 0;
    if (s_on <= 0)
        return 0;
    lock();
    for (k = 0; k < NV2A_TEX_DUMP_STAGES && !open; k++) {
        const HashSlot *h = hash[k] ? hash_slot(hash[k], 0) : NULL;
        open = h && h->drawn != DRAWN_3D;
    }
    unlock();
    return open;
}

void nv2a_tex_dump_draw(const uint64_t hash[NV2A_TEX_DUMP_STAGES], unsigned bump_stages, int two_d)
{
    int k;
    if (s_on <= 0)
        return;
    lock();
    for (k = 0; k < NV2A_TEX_DUMP_STAGES; k++) {
        HashSlot *h = hash[k] ? hash_slot(hash[k], 0) : NULL;
        uint8_t drawn;
        if (!h)
            continue;
        drawn = two_d < 0 ? h->drawn : !two_d || h->drawn == DRAWN_3D ? DRAWN_3D : DRAWN_2D;
        if (drawn != h->drawn) {
            h->drawn = drawn;
            h->dirty = 1;
        }
        if (bump_stages >> k & 1u && !h->bump) {
            h->bump = 1;
            h->dirty = 1;
        }
    }
    unlock();
}

void nv2a_tex_dump_flip(uint64_t flip)
{
    if (s_on <= 0 && s_mag <= 0)
        return;
    s_flip = flip;
    if (flip && flip % 600 == 0)
        nv2a_tex_dump_summary();
}

static void write_counts(void)
{
    char tmp[sizeof s_dir + 64];
    FILE *f = open_tmp(tmp, sizeof tmp, "counts.json");
    size_t i;
    int first = 1;
    if (!f) {
        s_st.errors++;
        return;
    }
    fputs("{\n", f);
    for (i = 0; i < s_hash_cap; i++) {
        if (!s_hash[i].used)
            continue;
        fprintf(f, "%s  \"%016llX\": %u", first ? "" : ",\n",
                (unsigned long long)s_hash[i].hash, s_hash[i].uploads);
        first = 0;
    }
    fputs(first ? "}\n" : "\n}\n", f);
    if (!commit_tmp(tmp, "counts.json", f))
        s_st.errors++;
}

static void write_volatile(void)
{
    char tmp[sizeof s_dir + 64];
    FILE *f = open_tmp(tmp, sizeof tmp, "volatile.json");
    size_t i;
    uint32_t k;
    int first = 1;
    if (!f) {
        s_st.errors++;
        return;
    }
    fputs("[\n", f);
    for (i = 0; i < s_addr_cap; i++) {
        const AddrSlot *a = &s_addr[i];
        if (!a->used || !a->vol)
            continue;
        fprintf(f, "%s  {\"offset\": \"0x%08X\", \"width\": %u, \"height\": %u, \"hashes\": [",
                first ? "" : ",\n", a->offset, a->width, a->height);
        for (k = 0; k < NV2A_TEX_DUMP_VOLATILE; k++)
            fprintf(f, "%s\"%016llX\"", k ? ", " : "", (unsigned long long)a->hashes[k]);
        fputs("]}", f);
        first = 0;
    }
    fputs(first ? "]\n" : "\n]\n", f);
    if (!commit_tmp(tmp, "volatile.json", f))
        s_st.errors++;
}

/* The targets themselves: no hash and no texels, so no sidecar to hold them. */
static void write_render_targets(void)
{
    char tmp[sizeof s_dir + 64];
    FILE *f = open_tmp(tmp, sizeof tmp, "render_targets.json");
    unsigned i;
    if (!f) {
        s_st.errors++;
        return;
    }
    fputs("{\n  \"targets\": [\n", f);
    for (i = 0; i < s_rt_n; i++)
        fprintf(f, "%s    {\"offset\": \"0x%08X\", \"width\": %u, \"height\": %u,"
                " \"format_id\": %u, \"binds\": %llu}",
                i ? ",\n" : "", s_rt[i].offset, s_rt[i].width, s_rt[i].height, s_rt[i].color,
                (unsigned long long)s_rt[i].binds);
    fprintf(f, "%s  ],\n  \"dropped_binds\": %llu\n}\n", s_rt_n ? "\n" : "",
            (unsigned long long)s_st.rt_dropped);
    if (!commit_tmp(tmp, "render_targets.json", f))
        s_st.errors++;
}

int nv2a_tex_mag_on(void)
{
    if (s_mag < 0) {
        s_mag = recomp_env_on(RENV_TEX_MAG_TRACE) ? 1 : 0;
        if (s_mag > 0)
            hook_exit_once();
    }
    return s_mag > 0;
}

void nv2a_tex_mag_set_on(int on)
{
    lock();
    s_mag = on ? 1 : 0;
    free(s_mag_tab);
    s_mag_tab = NULL;
    s_mag_cap = s_mag_n = 0;
    s_mag_dropped = 0;
    unlock();
}

static MagSlot *mag_slot(uint64_t hash, int add)
{
    size_t i;
    if (add && (s_mag_n + 1) * 2 > s_mag_cap) {
        size_t cap = s_mag_cap ? s_mag_cap * 2 : 1024, k;
        MagSlot *t = (MagSlot *)calloc(cap, sizeof *t);
        if (!t)
            return NULL;
        for (k = 0; k < s_mag_cap; k++) {
            if (!s_mag_tab[k].used)
                continue;
            i = mix(s_mag_tab[k].hash) & (cap - 1);
            while (t[i].used)
                i = (i + 1) & (cap - 1);
            t[i] = s_mag_tab[k];
        }
        free(s_mag_tab);
        s_mag_tab = t;
        s_mag_cap = cap;
    }
    if (!s_mag_cap)
        return NULL;
    i = mix(hash) & (s_mag_cap - 1);
    while (s_mag_tab[i].used) {
        if (s_mag_tab[i].hash == hash)
            return &s_mag_tab[i];
        i = (i + 1) & (s_mag_cap - 1);
    }
    if (!add)
        return NULL;
    s_mag_tab[i].used = 1;
    s_mag_tab[i].hash = hash;
    s_mag_n++;
    return &s_mag_tab[i];
}

/* Under the lock. */
static void mag_add(uint64_t hash, double mag, unsigned render_scale)
{
    MagSlot *m = mag_slot(hash, 1);
    if (!m)
        return;
    if (mag > m->mag_max)
        m->mag_max = mag;
    m->draws++;
    m->render_scale = render_scale;
    m->dirty = 1;
}

void nv2a_tex_mag_note(uint64_t hash, double mag, unsigned render_scale)
{
    if (!hash || !(mag > 0.0))
        return;
    lock();
    mag_add(hash, mag, render_scale);
    unlock();
}

unsigned nv2a_tex_mag_draw_id(struct nv2a_tex_mag_draw *draws, unsigned *n,
                              const uint64_t hash[NV2A_TEX_MAG_STAGES],
                              const uint32_t scale[NV2A_TEX_MAG_STAGES])
{
    struct nv2a_tex_mag_draw *d;
    if (*n >= NV2A_TEX_MAG_DRAWS - 1u) {
        __atomic_add_fetch(&s_mag_dropped, 1, __ATOMIC_RELAXED);
        return NV2A_TEX_MAG_DRAWS - 1u;
    }
    d = &draws[*n];
    memcpy(d->hash, hash, sizeof d->hash);
    memcpy(d->scale, scale, sizeof d->scale);
    return (*n)++;
}

void nv2a_tex_mag_fold(const struct nv2a_tex_mag_draw *draws, unsigned n,
                       const uint32_t *slots, unsigned render_scale)
{
    unsigned i, k;
    lock();
    for (i = 0; i < n; i++)
        for (k = 0; k < NV2A_TEX_MAG_STAGES; k++) {
            uint32_t v = slots[i * NV2A_TEX_MAG_STAGES + k];
            uint32_t scale = draws[i].scale[k] ? draws[i].scale[k] : 1u;
            /* The shader's 2^-lod is host pixels per texel of what is bound;
             * a source texel is `scale` of an entry's. */
            if (draws[i].hash[k] && v && v < 64u * 1024u)
                mag_add(draws[i].hash[k], pow(2.0, v / 1024.0 - 32.0) * scale, render_scale);
        }
    unlock();
}

double nv2a_tex_mag_triangle(const float xy[3][2], const float uv[3][2])
{
    double dx1 = xy[1][0] - xy[0][0], dy1 = xy[1][1] - xy[0][1];
    double dx2 = xy[2][0] - xy[0][0], dy2 = xy[2][1] - xy[0][1];
    double du1 = uv[1][0] - uv[0][0], dv1 = uv[1][1] - uv[0][1];
    double du2 = uv[2][0] - uv[0][0], dv2 = uv[2][1] - uv[0][1];
    double det = dx1 * dy2 - dx2 * dy1, rx, ry, rho;
    if (det == 0.0)
        return 0.0;
    /* The gradients of (u, v) over (x, y), and the larger of their lengths:
     * what the sampler's isotropic LOD is the log of. */
    rx = hypot((du1 * dy2 - du2 * dy1) / det, (dv1 * dy2 - dv2 * dy1) / det);
    ry = hypot((du2 * dx1 - du1 * dx2) / det, (dv2 * dx1 - dv1 * dx2) / det);
    rho = rx > ry ? rx : ry;
    return rho > 0.0 ? 1.0 / rho : 0.0;
}

int nv2a_tex_mag_get(uint64_t hash, double *mag_max, uint64_t *draws, unsigned *render_scale)
{
    const MagSlot *m;
    lock();
    m = mag_slot(hash, 0);
    if (m) {
        if (mag_max)
            *mag_max = m->mag_max;
        if (draws)
            *draws = m->draws;
        if (render_scale)
            *render_scale = m->render_scale;
    }
    unlock();
    return m != NULL;
}

/* The fields after "texels": what is known of the hash so far. No `drawn_2d`
 * until a draw shows it, so a scene that only uploaded a texture has no say. */
static void print_facts(FILE *f, uint64_t hash)
{
    const HashSlot *h = hash_slot(hash, 0);
    const MagSlot *m = mag_slot(hash, 0);
    fprintf(f, ",\n  \"bump\": %s,\n  \"rt\": %s", h && h->bump ? "true" : "false",
            h && h->rt ? "true" : "false");
    if (h && h->drawn != DRAWN_NONE)
        fprintf(f, ",\n  \"drawn_2d\": %s", h->drawn == DRAWN_2D ? "true" : "false");
    if (m)
        fprintf(f,
                ",\n"
                "  \"mag_max\": %.3f,\n"
                "  \"mag_scale1\": %.3f,\n"
                "  \"mag_draws\": %llu,\n"
                "  \"render_scale\": %u",
                m->mag_max, m->mag_max / (m->render_scale ? m->render_scale : 1u),
                (unsigned long long)m->draws, m->render_scale);
}

/* The facts into a hash's sidecar, in place of the last ones. A hash the dump
 * did not write has no sidecar; it stays dirty, for one written later. */
static void rewrite_sidecar(uint64_t hash)
{
    HashSlot *h = hash_slot(hash, 0);
    MagSlot *m = mag_slot(hash, 0);
    char name[48], tmp[sizeof s_dir + 64], path[sizeof s_dir + 64];
    char buf[4096];
    size_t n;
    char *cut, *mag;
    FILE *f;

    snprintf(name, sizeof name, "%016llX.json", (unsigned long long)hash);
    snprintf(path, sizeof path, "%s/%s", s_dir, name);
    if (!(f = fopen(path, "rb")))
        return;
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    /* The dump's own sidecars are under 500 bytes. A longer file is not one of
     * them, and rewriting a cut copy would leave the inventory corrupt JSON. */
    if (n == sizeof buf) {
        fprintf(stderr, "[TEXDUMP] %s: over %zu bytes, facts not written\n",
                name, sizeof buf - 1);
        s_st.errors++;
        if (h)
            h->dirty = 0;
        if (m)
            m->dirty = 0;
        return;
    }
    buf[n] = '\0';
    /* A sidecar of an older dump in the same directory has only the figures. */
    cut = strstr(buf, ",\n  \"bump\"");
    mag = strstr(buf, ",\n  \"mag_max\"");
    if (!cut || (mag && mag < cut))
        cut = mag;
    if (!cut)
        cut = strrchr(buf, '}');
    if (!cut)
        return;
    /* Back over the newline before the closing brace, if that is the cut. */
    if (*cut == '}' && cut > buf && cut[-1] == '\n')
        cut--;
    *cut = '\0';
    if (!(f = open_tmp(tmp, sizeof tmp, name))) {
        s_st.errors++;
        return;
    }
    fputs(buf, f);
    print_facts(f, hash);
    fputs("\n}\n", f);
    if (commit_tmp(tmp, name, f)) {
        if (h)
            h->dirty = 0;
        if (m)
            m->dirty = 0;
    } else {
        s_st.errors++;
    }
}

/* Under the lock: a summary's worth of the trace, the sidecars when there is
 * a dump, each hash's line at exit when there is none. */
static void mag_summary(int exiting)
{
    size_t i;
    const MagSlot *top = NULL;
    for (i = 0; i < s_mag_cap; i++) {
        MagSlot *m = &s_mag_tab[i];
        if (!m->used)
            continue;
        if (!top || m->mag_max > top->mag_max)
            top = m;
        if (s_on > 0 && m->dirty)
            rewrite_sidecar(m->hash);
        if (s_on <= 0 && exiting)
            fprintf(stderr, "[TEXDUMP] mag %016llX mag_max=%.3f mag_scale1=%.3f draws=%llu"
                    " render_scale=%u\n", (unsigned long long)m->hash, m->mag_max,
                    m->mag_max / (m->render_scale ? m->render_scale : 1u),
                    (unsigned long long)m->draws, m->render_scale);
    }
    fprintf(stderr, "[TEXDUMP] tex_mag: %llu textures, highest %.3f (%016llX), %llu draws past"
            " the frame's slots; an upper bound: occluded fragments count\n",
            (unsigned long long)s_mag_n, top ? top->mag_max : 0.0,
            (unsigned long long)(top ? top->hash : 0u),
            (unsigned long long)__atomic_load_n(&s_mag_dropped, __ATOMIC_RELAXED));
}

static void summary(int exiting)
{
    struct nv2a_tex_dump_stats st;
    const char *stop;
    char dropped[48] = "";
    if (s_on <= 0 && s_mag <= 0)
        return;
    if (!exiting) {
        lock();
    } else if (!lock_bounded()) {
        fprintf(stderr, "[TEXDUMP] exit summary skipped: the walker thread ended"
                " holding the dump's lock; counts.json is from the last summary\n");
        fflush(stderr);
        return;
    }
    if (s_on > 0 && s_dirty) {
        write_counts();
        write_volatile();
        write_render_targets();
        s_dirty = 0;
    }
    if (s_on > 0) {
        size_t i;
        for (i = 0; i < s_hash_cap; i++)
            if (s_hash[i].used && s_hash[i].dirty)
                rewrite_sidecar(s_hash[i].hash);
    }
    if (s_mag > 0)
        mag_summary(exiting);
    st = s_st;
    stop = s_stop;
    unlock();
    if (s_on <= 0) {
        fflush(stderr);
        return;
    }
    if (st.rt_dropped)
        snprintf(dropped, sizeof dropped, ", %llu binds past the table",
                 (unsigned long long)st.rt_dropped);
    fprintf(stderr, "[TEXDUMP] flip %llu: %llu written (%llu texels), %llu repeat uploads,"
            " %llu linear (sidecar only), %llu volatile addresses (%llu uploads skipped),"
            " %llu render-target binds skipped (%llu targets%s), %llu over size, %llu errors;"
            " %s%s%s\n",
            (unsigned long long)s_flip, (unsigned long long)st.written,
            (unsigned long long)st.texels, (unsigned long long)st.repeats,
            (unsigned long long)st.linear, (unsigned long long)st.volatile_addrs,
            (unsigned long long)st.volatile_skips, (unsigned long long)st.rt_binds,
            (unsigned long long)st.rt_targets, dropped, (unsigned long long)st.oversize,
            (unsigned long long)st.errors, s_dir, stop ? "; stopped: " : "",
            stop ? stop : "");
    fflush(stderr);
}

void nv2a_tex_dump_summary(void)
{
    summary(0);
}

void nv2a_tex_dump_set_max_bytes(uint64_t bytes)
{
    lock();
    s_max_bytes = bytes ? bytes : NV2A_TEX_DUMP_MAX_BYTES;
    unlock();
}

void nv2a_tex_dump_get_stats(struct nv2a_tex_dump_stats *out)
{
    if (!out)
        return;
    lock();
    *out = s_st;
    unlock();
}
