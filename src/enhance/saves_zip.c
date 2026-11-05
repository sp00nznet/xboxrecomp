/*
 * saves_zip.c - the saves zip: writer, validating reader, manifest
 * (saves_zip.h; openspec change overlay-saves, D2 and D3).
 *
 * Stored entries only. Our own export never compresses (saves are a few
 * kilobytes and the toolkit has no zlib), so the reader takes method 0 and
 * names anything else, which is a zip a desktop tool re-packed.
 */
#include "saves_zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- CRC-32 (IEEE, as zip uses) ---- */

uint32_t sz_crc32(uint32_t crc, const void *data, size_t len)
{
    /* A nibble at a time from a constant table, so there is no lazy setup
     * for two threads to race on. */
    static const uint32_t table[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    const unsigned char *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = table[crc & 0xF] ^ (crc >> 4);
        crc = table[crc & 0xF] ^ (crc >> 4);
    }
    return ~crc;
}

/* ---- little-endian fields ---- */

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void wr16(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char *p, uint32_t v)
{
    wr16(p, v & 0xFFFF);
    wr16(p + 2, v >> 16);
}

/* ---- time: Unix seconds <-> DOS fields, UTC, no libc (gmtime_r is not
 * on every host and the reader must not depend on the time zone) ---- */

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

static void dos_time(int64_t t, uint16_t *tm, uint16_t *dt)
{
    int y, mo, d;
    int64_t days = t >= 0 ? t / 86400 : 0;
    int sec = t >= 0 ? (int)(t % 86400) : 0;
    civil_from_days(days, &y, &mo, &d);
    if (y < 1980) { y = 1980; mo = 1; d = 1; sec = 0; }
    if (y > 2107) y = 2107;
    *tm = (uint16_t)((sec / 3600) << 11 | ((sec / 60) % 60) << 5 | (sec % 60) / 2);
    *dt = (uint16_t)((y - 1980) << 9 | mo << 5 | d);
}

static int64_t from_dos(uint16_t tm, uint16_t dt)
{
    int y = 1980 + (dt >> 9), mo = (dt >> 5) & 15, d = dt & 31;
    if (mo < 1 || mo > 12 || d < 1)
        return 0;
    return days_from_civil(y, mo, d) * 86400 + (tm >> 11) * 3600 + ((tm >> 5) & 63) * 60
         + (tm & 31) * 2;
}

/* ---- names ---- */

/* The length of one UTF-8 sequence at s, or 0 when it is not valid
 * (overlong, a surrogate, past U+10FFFF, cut short). */
static int utf8_seq(const unsigned char *s)
{
    unsigned c = s[0];
    int n;
    uint32_t cp;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; }
    else return 0;
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = cp << 6 | (s[i] & 0x3F);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF))
        || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    return n;
}

static int ascii_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* A Windows device name, with or without an extension: such a file cannot
 * be made, or opens the device, on a Windows host. */
static int reserved_name(const char *s, size_t n)
{
    static const char *const names[] = { "con", "prn", "aux", "nul" };
    size_t stem = 0;
    while (stem < n && s[stem] != '.')
        stem++;
    if (stem == 3) {
        for (int i = 0; i < 4; i++) {
            int same = 1;
            for (int k = 0; k < 3; k++)
                same &= ascii_lower((unsigned char)s[k]) == names[i][k];
            if (same) return 1;
        }
    }
    if (stem == 4 && s[3] >= '1' && s[3] <= '9') {
        char a = (char)ascii_lower((unsigned char)s[0]), b = (char)ascii_lower((unsigned char)s[1]),
             c = (char)ascii_lower((unsigned char)s[2]);
        if ((a == 'c' && b == 'o' && c == 'm') || (a == 'l' && b == 'p' && c == 't'))
            return 1;
    }
    return 0;
}

int sz_name_ok(const char *path, const char **why)
{
    const unsigned char *p = (const unsigned char *)path;
    int depth = 0;
    *why = NULL;
    if (!p[0]) { *why = "an empty name"; return -1; }
    if (p[0] == '/') { *why = "an absolute path"; return -1; }
    while (*p) {
        const unsigned char *c = p;
        size_t n = 0;
        while (c[n] && c[n] != '/') {
            unsigned ch = c[n];
            int k;
            if (ch < 0x20 || ch == 0x7F) { *why = "a control character in a name"; return -1; }
            if (ch == '\\') { *why = "a backslash in a name"; return -1; }
            if (ch == ':') { *why = "a drive letter or ':' in a name"; return -1; }
            if (strchr("*?\"<>|", (int)ch)) { *why = "a character Windows refuses in a name"; return -1; }
            if (!(k = utf8_seq(c + n))) { *why = "a name that is not UTF-8"; return -1; }
            n += (size_t)k;
        }
        if (n == 0) { *why = "an empty folder name"; return -1; }
        if ((n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.')) {
            *why = "a '.' or '..' folder"; return -1;
        }
        if (n > SZ_NAME_MAX) { *why = "a name over 42 bytes"; return -1; }
        if (c[n - 1] == '.' || c[n - 1] == ' ') { *why = "a name ending in a dot or space"; return -1; }
        if (reserved_name((const char *)c, n)) { *why = "a Windows device name"; return -1; }
        if (++depth > SZ_MAX_DEPTH) { *why = "folders nested too deep"; return -1; }
        p = c + n;
        if (*p == '/')
            p++;
    }
    return 0;
}

/* ---- writer ---- */

void sz_writer_init(struct sz_writer *w) { memset(w, 0, sizeof *w); }

void sz_writer_free(struct sz_writer *w)
{
    free(w->buf);
    free(w->cd);
    memset(w, 0, sizeof *w);
}

static int grow(unsigned char **b, size_t *cap, size_t need)
{
    size_t c = *cap ? *cap : 4096;
    unsigned char *nb;
    if (need <= *cap) return 0;
    while (c < need) c *= 2;
    if (!(nb = realloc(*b, c))) return -1;
    *b = nb;
    *cap = c;
    return 0;
}

static int add_entry(struct sz_writer *w, const char *name, const void *data, size_t len,
                     int64_t mtime, int dir)
{
    const char *why;
    size_t nl = strlen(name);
    uint16_t tm, dt;
    uint32_t crc = len ? sz_crc32(0, data, len) : 0;
    uint32_t t32 = mtime > 0 && mtime < 0xFFFFFFFFLL ? (uint32_t)mtime : 0;
    size_t off = w->len;
    unsigned char *h;
    if (w->err) return -1;
    if (sz_name_ok(name, &why) || nl >= SZ_PATH_MAX || len > SZ_MAX_FILE
        || w->count >= SZ_MAX_ENTRIES || off + 30 + nl + 9 + len > SZ_MAX_ZIP
        || grow(&w->buf, &w->cap, off + 30 + nl + 9 + len)
        || grow(&w->cd, &w->cd_cap, w->cd_len + 46 + nl + 9)) {
        w->err = 1;
        return -1;
    }
    dos_time(mtime, &tm, &dt);
    h = w->buf + off;
    wr32(h, 0x04034b50);
    wr16(h + 4, 20);
    wr16(h + 6, 0x0800);           /* the name is UTF-8 */
    wr16(h + 8, 0);                /* stored */
    wr16(h + 10, tm);
    wr16(h + 12, dt);
    wr32(h + 14, crc);
    wr32(h + 18, (uint32_t)len);
    wr32(h + 22, (uint32_t)len);
    wr16(h + 26, (uint32_t)nl);
    wr16(h + 28, 9);
    memcpy(h + 30, name, nl);
    /* extended timestamp: the mtime in Unix seconds, UTC */
    wr16(h + 30 + nl, 0x5455);
    wr16(h + 32 + nl, 5);
    h[34 + nl] = 1;
    wr32(h + 35 + nl, t32);
    if (len)
        memcpy(h + 39 + nl, data, len);
    w->len = off + 30 + nl + 9 + len;

    h = w->cd + w->cd_len;
    wr32(h, 0x02014b50);
    wr16(h + 4, 3 << 8 | 20);      /* made by Unix: the mode is in the attributes */
    wr16(h + 6, 20);
    wr16(h + 8, 0x0800);
    wr16(h + 10, 0);
    wr16(h + 12, tm);
    wr16(h + 14, dt);
    wr32(h + 16, crc);
    wr32(h + 20, (uint32_t)len);
    wr32(h + 24, (uint32_t)len);
    wr16(h + 28, (uint32_t)nl);
    wr16(h + 30, 9);
    wr16(h + 32, 0);
    wr16(h + 34, 0);
    wr16(h + 36, 0);
    wr32(h + 38, (dir ? 0040755u : 0100644u) << 16 | (dir ? 0x10u : 0u));
    wr32(h + 42, (uint32_t)off);
    memcpy(h + 46, name, nl);
    wr16(h + 46 + nl, 0x5455);
    wr16(h + 48 + nl, 5);
    h[50 + nl] = 1;
    wr32(h + 51 + nl, t32);
    w->cd_len += 46 + nl + 9;
    w->count++;
    return 0;
}

int sz_add_dir(struct sz_writer *w, const char *name, int64_t mtime)
{
    size_t n = strlen(name);
    if (!n || name[n - 1] != '/') { w->err = 1; return -1; }
    return add_entry(w, name, NULL, 0, mtime, 1);
}

int sz_add_file(struct sz_writer *w, const char *name, const void *data, size_t len,
                int64_t mtime)
{
    size_t n = strlen(name);
    if (!n || name[n - 1] == '/') { w->err = 1; return -1; }
    return add_entry(w, name, data, len, mtime, 0);
}

int sz_finish(struct sz_writer *w, unsigned char **out, size_t *len)
{
    size_t cd_off = w->len;
    unsigned char *e;
    if (w->err || cd_off + w->cd_len + 22 > SZ_MAX_ZIP
        || grow(&w->buf, &w->cap, cd_off + w->cd_len + 22)) {
        sz_writer_free(w);
        return -1;
    }
    if (w->cd_len)
        memcpy(w->buf + cd_off, w->cd, w->cd_len);
    e = w->buf + cd_off + w->cd_len;
    wr32(e, 0x06054b50);
    wr16(e + 4, 0);
    wr16(e + 6, 0);
    wr16(e + 8, (uint32_t)w->count);
    wr16(e + 10, (uint32_t)w->count);
    wr32(e + 12, (uint32_t)w->cd_len);
    wr32(e + 16, (uint32_t)cd_off);
    wr16(e + 20, 0);
    *out = w->buf;
    *len = cd_off + w->cd_len + 22;
    w->buf = NULL;
    sz_writer_free(w);
    return 0;
}

/* ---- reader ---- */

static int fail(char *err, size_t n, const char *fmt, const char *arg)
{
    if (err && n)
        snprintf(err, n, fmt, arg ? arg : "");
    return -1;
}

/* The extended timestamp's mtime in an extra field, or -1. */
static int64_t extra_mtime(const unsigned char *x, size_t n)
{
    while (n >= 4) {
        uint16_t id = rd16(x), sz = rd16(x + 2);
        if ((size_t)sz + 4 > n)
            break;
        if (id == 0x5455 && sz >= 5 && (x[4] & 1))
            return (int64_t)rd32(x + 5);
        x += 4 + sz;
        n -= 4 + (size_t)sz;
    }
    return -1;
}

/* Compare two names for the duplicate check: ASCII case folded (Windows
 * and macOS hosts fold case, so "A" and "a" are one file there), a
 * folder's trailing '/' left out (a file "x" and a folder "x/" clash). */
static int fold_cmp(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la && a[la - 1] == '/') la--;
    if (lb && b[lb - 1] == '/') lb--;
    for (size_t i = 0; i < la && i < lb; i++) {
        int x = ascii_lower((unsigned char)a[i]), y = ascii_lower((unsigned char)b[i]);
        if (x != y) return x - y;
    }
    return la < lb ? -1 : la > lb;
}

static const struct sz_entry *s_sort_base;
static int by_name(const void *x, const void *y)
{
    return fold_cmp(s_sort_base[*(const int *)x].name, s_sort_base[*(const int *)y].name);
}

struct span { size_t lo, hi; };
static int by_lo(const void *x, const void *y)
{
    const struct span *a = x, *b = y;
    return a->lo < b->lo ? -1 : a->lo > b->lo;
}

void sz_archive_free(struct sz_archive *a)
{
    free(a->e);
    a->e = NULL;
    a->n = 0;
}

int sz_read(const void *zip, size_t len, struct sz_archive *a, char *err, size_t errn)
{
    const unsigned char *z = zip;
    size_t eocd = 0, cd_off, cd_size, p, total = 0;
    int count, found = 0, i;
    struct span *spans = NULL;
    int *order = NULL;
    const char *why;

    memset(a, 0, sizeof *a);
    if (len > SZ_MAX_ZIP)
        return fail(err, errn, "This zip is over 64 MiB, too big for saves%s", NULL);
    if (len < 22)
        return fail(err, errn, "This is not a zip file%s", NULL);
    /* the end record: within the last 22 + 65535 bytes (its comment) */
    for (size_t k = len - 22 + 1; k-- > 0 && len - k <= 22 + 0xFFFFu;) {
        if (rd32(z + k) == 0x06054b50 && k + 22 + rd16(z + k + 20) == len) {
            eocd = k;
            found = 1;
            break;
        }
    }
    if (!found)
        return fail(err, errn, "This is not a zip file%s", NULL);
    count = rd16(z + eocd + 10);
    cd_size = rd32(z + eocd + 12);
    cd_off = rd32(z + eocd + 16);
    if (rd16(z + eocd + 4) || rd16(z + eocd + 6) || rd16(z + eocd + 8) != count)
        return fail(err, errn, "This zip is split into parts%s", NULL);
    if (count == 0xFFFF || cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu)
        return fail(err, errn, "This zip uses zip64, which saves never need%s", NULL);
    if (count > SZ_MAX_ENTRIES)
        return fail(err, errn, "This zip has too many files for saves%s", NULL);
    if (cd_off > eocd || cd_size > eocd - cd_off)
        return fail(err, errn, "This zip is damaged (its index is out of place)%s", NULL);
    if (count && !(a->e = calloc((size_t)count, sizeof *a->e)))
        return fail(err, errn, "Out of memory reading the zip%s", NULL);
    if (count && !(spans = calloc((size_t)count, sizeof *spans)))
        goto oom;

    p = cd_off;
    for (i = 0; i < count; i++) {
        struct sz_entry *e = &a->e[i];
        const unsigned char *c = z + p;
        size_t nl, xl, cl, lo, ln, lx, data;
        uint16_t flags, method;
        uint32_t csize, usize, attr;
        int64_t mt;
        if (p + 46 > cd_off + cd_size || rd32(c) != 0x02014b50)
            goto damaged_index;
        flags = rd16(c + 8);
        method = rd16(c + 10);
        csize = rd32(c + 20);
        usize = rd32(c + 24);
        nl = rd16(c + 28);
        xl = rd16(c + 30);
        cl = rd16(c + 32);
        attr = rd32(c + 38);
        lo = rd32(c + 42);
        if (p + 46 + nl + xl + cl > cd_off + cd_size)
            goto damaged_index;
        if (!nl || nl >= SZ_PATH_MAX || memchr(c + 46, 0, nl)) {
            fail(err, errn, "This zip has a file name saves cannot have%s", NULL);
            goto out;
        }
        memcpy(e->name, c + 46, nl);
        e->name[nl] = 0;
        if (flags & 0x41) {
            fail(err, errn, "This zip is encrypted (%s)", e->name);
            goto out;
        }
        if (method != 0) {
            fail(err, errn, "This zip is compressed (%s); export it again from the menu",
                 e->name);
            goto out;
        }
        if (csize != usize || usize == 0xFFFFFFFFu || lo == 0xFFFFFFFFu) {
            fail(err, errn, "This zip is damaged (%s)", e->name);
            goto out;
        }
        if (sz_name_ok(e->name, &why)) {
            if (err && errn)
                snprintf(err, errn, "This zip has %s (%s)", why, e->name);
            goto out;
        }
        e->is_dir = e->name[nl - 1] == '/';
        /* Unix mode (made by 3): a link or a device file is never a save */
        if ((rd16(c + 4) >> 8) == 3) {
            uint32_t type = (attr >> 16) & 0170000u;
            if (type && type != 0100000u && type != 0040000u) {
                fail(err, errn, "This zip holds a link or a special file (%s)", e->name);
                goto out;
            }
            if (type == 0040000u && !e->is_dir) {
                fail(err, errn, "This zip is damaged (%s)", e->name);
                goto out;
            }
        }
        if (e->is_dir && usize) {
            fail(err, errn, "This zip is damaged (%s)", e->name);
            goto out;
        }
        if (usize > SZ_MAX_FILE) {
            fail(err, errn, "A file in this zip is over 16 MiB (%s)", e->name);
            goto out;
        }
        total += usize;
        if (total > SZ_MAX_TOTAL) {
            fail(err, errn, "This zip holds over 64 MiB, too much for saves%s", NULL);
            goto out;
        }
        /* the local header must agree with the index */
        if (lo + 30 > cd_off || rd32(z + lo) != 0x04034b50)
            goto damaged_entry;
        ln = rd16(z + lo + 26);
        lx = rd16(z + lo + 28);
        data = lo + 30 + ln + lx;
        if (ln != nl || memcmp(z + lo + 30, e->name, nl) || rd16(z + lo + 8) != method
            || (rd16(z + lo + 6) & 0x41) || data > cd_off || usize > cd_off - data)
            goto damaged_entry;
        /* a data-descriptor entry may leave the local sizes and CRC zero */
        if (!(rd16(z + lo + 6) & 0x08) || rd32(z + lo + 14) || rd32(z + lo + 18)) {
            if (rd32(z + lo + 14) != rd32(c + 16) || rd32(z + lo + 18) != csize
                || rd32(z + lo + 22) != usize)
                goto damaged_entry;
        }
        e->data = z + data;
        e->size = usize;
        e->crc = rd32(c + 16);
        if (sz_crc32(0, e->data, usize) != e->crc) {
            fail(err, errn, "This zip is damaged (bad checksum in %s)", e->name);
            goto out;
        }
        mt = extra_mtime(c + 46 + nl, xl);
        e->mtime = mt >= 0 ? mt : from_dos(rd16(c + 12), rd16(c + 14));
        spans[i].lo = lo;
        spans[i].hi = data + usize;
        p += 46 + nl + xl + cl;
        continue;
damaged_entry:
        fail(err, errn, "This zip is damaged (%s)", e->name);
        goto out;
    }
    a->n = count;

    /* no two entries share bytes (a crafted zip can point many names at
     * one blob) */
    qsort(spans, (size_t)count, sizeof *spans, by_lo);
    for (i = 1; i < count; i++)
        if (spans[i].lo < spans[i - 1].hi) {
            fail(err, errn, "This zip is damaged (entries overlap)%s", NULL);
            goto out;
        }
    /* no two names that one case-insensitive host folds together */
    if (count > 1) {
        if (!(order = malloc((size_t)count * sizeof *order)))
            goto oom;
        for (i = 0; i < count; i++)
            order[i] = i;
        s_sort_base = a->e;
        qsort(order, (size_t)count, sizeof *order, by_name);
        for (i = 1; i < count; i++)
            if (!fold_cmp(a->e[order[i]].name, a->e[order[i - 1]].name)) {
                fail(err, errn, "This zip has the same name twice (%s)", a->e[order[i]].name);
                goto out;
            }
    }
    free(spans);
    free(order);
    return 0;

damaged_index:
    fail(err, errn, "This zip is damaged (its index is cut short)%s", NULL);
    goto out;
oom:
    fail(err, errn, "Out of memory reading the zip%s", NULL);
out:
    free(spans);
    free(order);
    sz_archive_free(a);
    return -1;
}

const struct sz_entry *sz_find(const struct sz_archive *a, const char *name)
{
    for (int i = 0; i < a->n; i++)
        if (!strcmp(a->e[i].name, name))
            return &a->e[i];
    return NULL;
}

/* ---- the manifest ---- */

static int put_str(char *out, size_t n, size_t *at, const char *key, const char *v)
{
    int k = snprintf(out + *at, n - *at, "%s = \"", key);
    if (k < 0 || (size_t)k >= n - *at) return -1;
    *at += (size_t)k;
    for (const unsigned char *s = (const unsigned char *)v; *s; s++) {
        if (*s < 0x20 || *s == 0x7F)
            continue;               /* a title name never needs one */
        if (*at + 3 >= n) return -1;
        if (*s == '"' || *s == '\\')
            out[(*at)++] = '\\';
        out[(*at)++] = (char)*s;
    }
    if (*at + 2 >= n) return -1;
    out[(*at)++] = '"';
    out[(*at)++] = '\n';
    out[*at] = 0;
    return 0;
}

int saves_manifest_write(const struct saves_manifest *m, char *out, size_t n)
{
    size_t at = 0;
    char tid[16];
    int k = snprintf(out, n, "# Saves exported by the in-game menu (xboxrecomp)\nformat = %d\n",
                     m->format);
    if (k < 0 || (size_t)k >= n) return -1;
    at = (size_t)k;
    snprintf(tid, sizeof tid, "%08x", (unsigned)m->title_id);
    if (put_str(out, n, &at, "kind", m->kind) || put_str(out, n, &at, "title_id", tid)
        || put_str(out, n, &at, "title", m->title) || put_str(out, n, &at, "created", m->created)
        || put_str(out, n, &at, "keys", m->keys))
        return -1;
    k = snprintf(out + at, n - at, "files = %d\nbytes = %llu\n", m->files,
                 (unsigned long long)m->bytes);
    if (k < 0 || (size_t)k >= n - at) return -1;
    at += (size_t)k;
    if (put_str(out, n, &at, "toolkit", m->toolkit))
        return -1;
    return (int)at;
}

/* One value: a "string" (\" and \\ escapes) into s, or an unsigned
 * integer into *u. 0, or -1 when it is neither or does not fit. */
static int parse_value(const char *v, const char *end, char *s, size_t sn, uint64_t *u,
                       int *is_str)
{
    if (v < end && *v == '"') {
        size_t at = 0;
        v++;
        while (v < end && *v != '"') {
            if (*v == '\\' && v + 1 < end)
                v++;
            if (at + 1 >= sn) return -1;
            s[at++] = *v++;
        }
        if (v >= end) return -1;
        s[at] = 0;
        v++;
        while (v < end && (*v == ' ' || *v == '\t')) v++;
        if (v < end && *v != '#') return -1;
        *is_str = 1;
        return 0;
    }
    *u = 0;
    if (v >= end || *v < '0' || *v > '9') return -1;
    while (v < end && *v >= '0' && *v <= '9') {
        if (*u > (UINT64_MAX - 9) / 10) return -1;
        *u = *u * 10 + (uint64_t)(*v++ - '0');
    }
    while (v < end && (*v == ' ' || *v == '\t')) v++;
    if (v < end && *v != '#') return -1;
    *is_str = 0;
    return 0;
}

int saves_manifest_parse(const char *text, size_t len, struct saves_manifest *m,
                         const char **why)
{
    const char *p = text, *end = text + len;
    unsigned seen = 0;
    memset(m, 0, sizeof *m);
    *why = NULL;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *le = eol ? eol : end, *k, *ke, *v;
        char sval[128];
        uint64_t u = 0;
        int is_str = 0;
        if (le > p && le[-1] == '\r') le--;
        while (p < le && (*p == ' ' || *p == '\t')) p++;
        if (p == le || *p == '#') { p = eol ? eol + 1 : end; continue; }
        k = p;
        while (p < le && *p != '=' && *p != ' ' && *p != '\t') p++;
        ke = p;
        while (p < le && (*p == ' ' || *p == '\t')) p++;
        if (p >= le || *p != '=') { *why = "a line that is not key = value"; return -1; }
        v = p + 1;
        while (v < le && (*v == ' ' || *v == '\t')) v++;
        if (parse_value(v, le, sval, sizeof sval, &u, &is_str)) {
            *why = "a value it cannot read"; return -1;
        }
#define KEY(s) ((size_t)(ke - k) == sizeof(s) - 1 && !memcmp(k, s, sizeof(s) - 1))
        if (KEY("format") && !is_str && u < 1000) { m->format = (int)u; seen |= 1; }
        else if (KEY("kind") && is_str) { snprintf(m->kind, sizeof m->kind, "%s", sval); seen |= 2; }
        else if (KEY("title_id") && is_str) {
            if (strlen(sval) != 8 || strspn(sval, "0123456789abcdefABCDEF") != 8) {
                *why = "a bad title_id"; return -1;
            }
            m->title_id = (uint32_t)strtoul(sval, NULL, 16);
            seen |= 4;
        }
        else if (KEY("title") && is_str) snprintf(m->title, sizeof m->title, "%s", sval);
        else if (KEY("created") && is_str) snprintf(m->created, sizeof m->created, "%s", sval);
        else if (KEY("keys") && is_str) { snprintf(m->keys, sizeof m->keys, "%s", sval); seen |= 8; }
        else if (KEY("files") && !is_str && u <= SZ_MAX_ENTRIES) { m->files = (int)u; seen |= 16; }
        else if (KEY("bytes") && !is_str && u <= SZ_MAX_TOTAL) { m->bytes = u; seen |= 32; }
        else if (KEY("toolkit") && is_str) snprintf(m->toolkit, sizeof m->toolkit, "%s", sval);
        else if (KEY("format") || KEY("kind") || KEY("title_id") || KEY("keys") || KEY("files")
                 || KEY("bytes")) {
            *why = "a value of the wrong type"; return -1;
        }
#undef KEY
        p = eol ? eol + 1 : end;
    }
    if ((seen & 63) != 63) { *why = "a required key missing"; return -1; }
    return 0;
}

/* ---- a saves zip ---- */

const char *saves_zip_rest(const char *name, uint32_t tid, int *tree)
{
    char t[16];
    size_t n;
    if (strncmp(name, "hdd/", 4))
        return NULL;
    name += 4;
    if (!strncmp(name, "UDATA/", 6)) *tree = SAVES_TREE_UDATA;
    else if (!strncmp(name, "TDATA/", 6)) *tree = SAVES_TREE_TDATA;
    else return NULL;
    name += 6;
    n = (size_t)snprintf(t, sizeof t, "%08x", (unsigned)tid);
    /* the folder name compared ignoring case: a host that folds case may
     * have written it either way */
    for (size_t i = 0; i < n; i++)
        if (ascii_lower((unsigned char)name[i]) != t[i])
            return NULL;
    name += n;
    if (*name == 0)
        return NULL;       /* "hdd/UDATA/<tid>" without the slash is a file */
    return *name == '/' ? name + 1 : NULL;
}

int saves_zip_check(const struct sz_archive *a, uint32_t tid, const char *keys,
                    struct saves_manifest *m, int *trees, char *err, size_t errn)
{
    const struct sz_entry *me = sz_find(a, SZ_MANIFEST);
    const char *why;
    int files = 0;
    uint64_t bytes = 0;
    char buf[32];
    *trees = 0;
    if (!me || me->is_dir) {
        /* a zip of a hdd folder made by hand, or one from another tool */
        return fail(err, errn, "This zip was not exported from the Saves menu "
                    "(no xbr-saves.toml)%s", NULL);
    }
    if (me->size > SZ_MANIFEST_MAX)
        return fail(err, errn, "This zip's xbr-saves.toml is too big%s", NULL);
    if (saves_manifest_parse((const char *)me->data, me->size, m, &why)) {
        if (err && errn) snprintf(err, errn, "This zip's xbr-saves.toml has %s", why);
        return -1;
    }
    if (strcmp(m->kind, "saves"))
        return fail(err, errn, "This zip is not a saves export (kind \"%s\")", m->kind);
    if (m->format > SAVES_FORMAT)
        return fail(err, errn, "This zip comes from a newer build; update the game first%s",
                    NULL);
    if (m->format < 1)
        return fail(err, errn, "This zip's xbr-saves.toml has a bad format%s", NULL);
    if (m->title_id != tid) {
        snprintf(buf, sizeof buf, "%08x", (unsigned)m->title_id);
        return fail(err, errn, "This zip is for another game (title %s)", buf);
    }
    if (strcmp(m->keys, keys))
        return fail(err, errn, "These saves were made with different console keys "
                    "and would not load here%s", NULL);
    for (int i = 0; i < a->n; i++) {
        const struct sz_entry *e = &a->e[i];
        const char *rest;
        int tree = 0;
        if (e == me)
            continue;
        if (e->is_dir && (!strcmp(e->name, "hdd/") || !strcmp(e->name, "hdd/UDATA/")
                          || !strcmp(e->name, "hdd/TDATA/")))
            continue;
        if (!(rest = saves_zip_rest(e->name, tid, &tree)))
            return fail(err, errn, "This zip holds a file outside the game's saves (%s)",
                        e->name);
        *trees |= tree;
        if (!e->is_dir) {
            if (!*rest)
                return fail(err, errn, "This zip is damaged (%s)", e->name);
            files++;
            bytes += e->size;
        }
    }
    if (files != m->files || bytes != m->bytes)
        return fail(err, errn, "This zip is incomplete (its file count does not match "
                    "xbr-saves.toml)%s", NULL);
    if (!*trees)
        return fail(err, errn, "This zip holds no saves%s", NULL);
    return 0;
}

/* ---- SaveMeta.xbx ---- */

int saves_meta_name(const unsigned char *data, size_t len, char *out, size_t n)
{
    size_t i = 0, at = 0;
    if (!n) return -1;
    out[0] = 0;
    if (len >= 2 && data[0] == 0xFF && data[1] == 0xFE)
        i = 2;
    /* line by line, in UTF-16 code units */
    while (i + 1 < len) {
        size_t ls = i;
        while (i + 1 < len && !(data[i] == '\n' && data[i + 1] == 0))
            i += 2;
        if (i - ls >= 10 && !memcmp(data + ls, "N\0a\0m\0e\0=\0", 10)) {
            size_t k = ls + 10;
            while (k + 1 < i) {
                uint32_t cp = rd16(data + k);
                k += 2;
                if (cp == '\r') break;
                if (cp >= 0xD800 && cp <= 0xDBFF && k + 1 < i) {
                    uint32_t lo = rd16(data + k);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        k += 2;
                    } else cp = 0xFFFD;
                } else if (cp >= 0xD800 && cp <= 0xDFFF)
                    cp = 0xFFFD;
                if (cp < 0x20) continue;
                {
                    unsigned char b[4];
                    int bn;
                    if (cp < 0x80) { b[0] = (unsigned char)cp; bn = 1; }
                    else if (cp < 0x800) { b[0] = (unsigned char)(0xC0 | cp >> 6); b[1] = (unsigned char)(0x80 | (cp & 63)); bn = 2; }
                    else if (cp < 0x10000) { b[0] = (unsigned char)(0xE0 | cp >> 12); b[1] = (unsigned char)(0x80 | ((cp >> 6) & 63)); b[2] = (unsigned char)(0x80 | (cp & 63)); bn = 3; }
                    else { b[0] = (unsigned char)(0xF0 | cp >> 18); b[1] = (unsigned char)(0x80 | ((cp >> 12) & 63)); b[2] = (unsigned char)(0x80 | ((cp >> 6) & 63)); b[3] = (unsigned char)(0x80 | (cp & 63)); bn = 4; }
                    if (at + (size_t)bn >= n) break;
                    memcpy(out + at, b, (size_t)bn);
                    at += (size_t)bn;
                }
            }
            out[at] = 0;
            return at ? 0 : -1;
        }
        i += 2;
    }
    return -1;
}
