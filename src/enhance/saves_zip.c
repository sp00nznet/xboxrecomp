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

/* The saves manifest's lines after a first comment line, at *at. */
static int manifest_body(const struct saves_manifest *m, const char *comment, char *out,
                         size_t n, size_t *atp)
{
    size_t at = 0;
    char tid[16];
    int k = snprintf(out, n, "# %s (xboxrecomp)\nformat = %d\n", comment, m->format);
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
    *atp = at;
    return 0;
}

int saves_manifest_write(const struct saves_manifest *m, char *out, size_t n)
{
    size_t at = 0;
    if (manifest_body(m, "Saves exported by the in-game menu", out, n, &at))
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

int saves_zip_keys_differ(const struct saves_manifest *m, const char *keys)
{
    return strcmp(m->keys, keys ? keys : "") != 0;
}

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

/* The saves rules over every entry but the manifest me (and pad/ when
 * pads): each under the title's two trees, the skeleton folders "hdd/",
 * "hdd/UDATA/" and "hdd/TDATA/" passed over. The trees present, the files
 * and their bytes counted. */
static int tree_entries(const struct sz_archive *a, uint32_t tid, const struct sz_entry *me,
                        int pads, int *trees, int *files, uint64_t *bytes, char *err,
                        size_t errn)
{
    for (int i = 0; i < a->n; i++) {
        const struct sz_entry *e = &a->e[i];
        const char *rest;
        int tree = 0;
        if (e == me)
            continue;
        if (pads && !strncmp(e->name, REPORT_PAD_DIR, sizeof REPORT_PAD_DIR - 1))
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
            (*files)++;
            *bytes += e->size;
        }
    }
    return 0;
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
    /* other console keys are no refusal: the caller re-signs the saves
     * (saves_sign.h; pad-recording-ui R6) */
    (void)keys;
    if (tree_entries(a, tid, me, 0, trees, &files, &bytes, err, errn))
        return -1;
    if (files != m->files || bytes != m->bytes)
        return fail(err, errn, "This zip is incomplete (its file count does not match "
                    "xbr-saves.toml)%s", NULL);
    if (!*trees)
        return fail(err, errn, "This zip holds no saves%s", NULL);
    return 0;
}

/* ---- a bug-report bundle (pad-recording-ui R6) ---- */

int report_manifest_write(const struct report_manifest *m, char *out, size_t n)
{
    size_t at = 0;
    if (manifest_body(&m->s, "A bug report from the in-game menu", out, n, &at)
        || put_str(out, n, &at, "game", m->game) || put_str(out, n, &at, "host_os", m->host_os)
        || put_str(out, n, &at, "note", m->note))
        return -1;
    if (at + 10 >= n) return -1;
    memcpy(out + at, "chain = [", 9);
    at += 9;
    for (int i = 0; i < m->nchain; i++) {
        if (i) {
            if (at + 2 >= n) return -1;
            out[at++] = ',';
            out[at++] = ' ';
        }
        if (at + 1 >= n) return -1;
        out[at++] = '"';
        /* names that passed sz_name_ok: no '"', '\\' or control character */
        for (const char *c = m->chain[i]; *c; c++) {
            if (at + 1 >= n) return -1;
            out[at++] = *c;
        }
        if (at + 1 >= n) return -1;
        out[at++] = '"';
    }
    if (at + 2 >= n) return -1;
    out[at++] = ']';
    out[at++] = '\n';
    out[at] = 0;
    return (int)at;
}

/* A "string" at v (\" and \\ escapes) into s: the byte after its closing
 * quote, or NULL when it is not one or does not fit. */
static const char *read_qstr(const char *v, const char *end, char *s, size_t sn)
{
    size_t at = 0;
    if (v >= end || *v != '"')
        return NULL;
    v++;
    while (v < end && *v != '"') {
        if (*v == '\\' && v + 1 < end)
            v++;
        if (at + 1 >= sn) return NULL;
        s[at++] = *v++;
    }
    if (v >= end) return NULL;
    s[at] = 0;
    return v + 1;
}

/* Nothing but blanks or a comment from v to end. */
static int line_done(const char *v, const char *end)
{
    while (v < end && (*v == ' ' || *v == '\t')) v++;
    return v == end || *v == '#';
}

int report_manifest_parse(const char *text, size_t len, struct report_manifest *m,
                          const char **why)
{
    /* the report's own keys here; every other line goes to the saves
     * manifest's parser, which checks the shared keys as it does a saves
     * zip's */
    char rest[SZ_MANIFEST_MAX + 1];
    size_t rn = 0;
    const char *p = text, *end = text + len;
    int chain = 0;
    memset(m, 0, sizeof *m);
    *why = NULL;
    if (len > SZ_MANIFEST_MAX) { *why = "too many lines"; return -1; }
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *next = eol ? eol + 1 : end, *le = eol ? eol : end, *k = p, *ke, *v;
        while (k < le && (*k == ' ' || *k == '\t')) k++;
        ke = k;
        while (ke < le && *ke != '=' && *ke != ' ' && *ke != '\t') ke++;
        v = ke;
        while (v < le && (*v == ' ' || *v == '\t')) v++;
        if (v < le && *v == '=') v++;
        while (v < le && (*v == ' ' || *v == '\t')) v++;
        if (le > v && le[-1] == '\r') le--;
#define KEY(s) ((size_t)(ke - k) == sizeof(s) - 1 && !memcmp(k, s, sizeof(s) - 1))
        if (KEY("game") || KEY("host_os") || KEY("note")) {
            char *dst = KEY("game") ? m->game : KEY("host_os") ? m->host_os : m->note;
            size_t dn = KEY("game") ? sizeof m->game
                        : KEY("host_os") ? sizeof m->host_os : sizeof m->note;
            const char *after = read_qstr(v, le, dst, dn);
            size_t o = 0;
            if (!after || !line_done(after, le)) { *why = "a value it cannot read"; return -1; }
            /* The zip is someone else's: control bytes (ESC, newlines) would
             * reach the preview rows and stderr. Dropped, as the writer
             * drops them (review 1, F3). */
            for (size_t i = 0; dst[i]; i++)
                if ((unsigned char)dst[i] >= 0x20 && dst[i] != 0x7f)
                    dst[o++] = dst[i];
            dst[o] = 0;
        } else if (KEY("chain")) {
            if (v >= le || *v != '[') { *why = "a chain that is not a list"; return -1; }
            v++;
            m->nchain = 0;
            for (;;) {
                while (v < le && (*v == ' ' || *v == '\t')) v++;
                if (v < le && *v == ']' && !m->nchain) break;
                if (m->nchain == REPORT_CHAIN_MAX) {
                    *why = "a chain of more than 8 recordings"; return -1;
                }
                v = read_qstr(v, le, m->chain[m->nchain], sizeof m->chain[0]);
                if (!v) { *why = "a chain it cannot read"; return -1; }
                m->nchain++;
                while (v < le && (*v == ' ' || *v == '\t')) v++;
                if (v < le && *v == ',') { v++; continue; }
                if (v < le && *v == ']') break;
                *why = "a chain it cannot read";
                return -1;
            }
            if (!line_done(v + 1, le)) { *why = "a chain it cannot read"; return -1; }
            chain = 1;
        } else {
            memcpy(rest + rn, p, (size_t)(next - p));
            rn += (size_t)(next - p);
        }
#undef KEY
        p = next;
    }
    rest[rn] = 0;
    if (saves_manifest_parse(rest, rn, &m->s, why))
        return -1;
    if (!chain || !m->nchain) { *why = "no chain"; return -1; }
    return 0;
}

int report_pad_name(int pos, const char *name, char *out, size_t n)
{
    char pre[16];
    size_t pl, sl = strlen(name), room;
    const char *why;
    if (sl < 5 || ascii_lower((unsigned char)name[sl - 4]) != '.'
        || ascii_lower((unsigned char)name[sl - 3]) != 'p'
        || ascii_lower((unsigned char)name[sl - 2]) != 'a'
        || ascii_lower((unsigned char)name[sl - 1]) != 'd')
        return -1;
    sl -= 4;
    pl = (size_t)snprintf(pre, sizeof pre, "%d-", pos);
    if (pl + 4 >= SZ_NAME_MAX)
        return -1;
    room = SZ_NAME_MAX - pl - 4;
    if (sl > room) {
        /* cut at a character: the name is UTF-8 */
        sl = room;
        while (sl && ((unsigned char)name[sl] & 0xC0) == 0x80)
            sl--;
    }
    if (snprintf(out, n, REPORT_PAD_DIR "%s%.*s.pad", pre, (int)sl, name) >= (int)n)
        return -1;
    return sz_name_ok(out, &why) ? -1 : 0;
}

const char *report_pad_file(const char *entry)
{
    const char *s = entry, *d;
    if (!strncmp(s, REPORT_PAD_DIR, sizeof REPORT_PAD_DIR - 1))
        s += sizeof REPORT_PAD_DIR - 1;
    for (d = s; *d >= '0' && *d <= '9'; d++)
        ;
    return d > s && *d == '-' && d[1] ? d + 1 : s;
}

int report_zip_check(const struct sz_archive *a, uint32_t tid, struct report_manifest *m,
                     int *trees, char *err, size_t errn)
{
    const struct sz_entry *me = sz_find(a, SZ_REPORT);
    const size_t pn = sizeof REPORT_PAD_DIR - 1;
    const char *why;
    int files = 0;
    uint64_t bytes = 0;
    char buf[32];
    *trees = 0;
    if (!me || me->is_dir)
        return fail(err, errn, "This zip is not a bug report (no " SZ_REPORT ")%s", NULL);
    if (me->size > SZ_MANIFEST_MAX)
        return fail(err, errn, "This zip's " SZ_REPORT " is too big%s", NULL);
    if (report_manifest_parse((const char *)me->data, me->size, m, &why)) {
        if (err && errn) snprintf(err, errn, "This zip's " SZ_REPORT " has %s", why);
        return -1;
    }
    if (strcmp(m->s.kind, "report"))
        return fail(err, errn, "This zip is not a bug report (kind \"%s\")", m->s.kind);
    if (m->s.format > REPORT_FORMAT)
        return fail(err, errn, "This report comes from a newer build; update the game first%s",
                    NULL);
    if (m->s.format < 1)
        return fail(err, errn, "This zip's " SZ_REPORT " has a bad format%s", NULL);
    if (m->s.title_id != tid) {
        snprintf(buf, sizeof buf, "%08x", (unsigned)m->s.title_id);
        return fail(err, errn, "This report is for another game (title %s)", buf);
    }
    /* each chain entry names one file under pad/, once */
    for (int i = 0; i < m->nchain; i++) {
        const struct sz_entry *e = sz_find(a, m->chain[i]);
        if (strncmp(m->chain[i], REPORT_PAD_DIR, pn) || !e || e->is_dir)
            return fail(err, errn, "This report's chain names a recording it does not hold "
                        "(%s)", m->chain[i]);
        for (int j = 0; j < i; j++)
            if (!strcmp(m->chain[i], m->chain[j]))
                return fail(err, errn, "This report's chain names %s twice", m->chain[i]);
    }
    /* and every file under pad/ is in the chain, one level deep, capped */
    for (int i = 0; i < a->n; i++) {
        const struct sz_entry *e = &a->e[i];
        const char *rest = e->name + pn;
        size_t rl;
        int named = 0;
        if (strncmp(e->name, REPORT_PAD_DIR, pn))
            continue;
        if (e->is_dir) {
            if (*rest)
                return fail(err, errn, "This report holds a folder inside pad/ (%s)", e->name);
            continue;
        }
        rl = strlen(rest);
        if (strchr(rest, '/') || rl < 5 || strcmp(rest + rl - 4, ".pad"))
            return fail(err, errn, "This report holds a file in pad/ that is not a recording "
                        "(%s)", e->name);
        /* "pad/1-.pad" would import as ".pad": a file the page lists with
         * no name (review 1, F4). The writer never makes one. */
        if (strlen(report_pad_file(e->name)) < 5)
            return fail(err, errn, "This report holds a recording with no name (%s)", e->name);
        if (e->size > REPORT_PAD_MAX)
            return fail(err, errn, "This report's %s is over 4 MB", e->name);
        for (int j = 0; j < m->nchain && !named; j++)
            named = !strcmp(m->chain[j], e->name);
        if (!named)
            return fail(err, errn, "This report holds %s, which its chain does not name",
                        e->name);
    }
    if (tree_entries(a, tid, me, 1, trees, &files, &bytes, err, errn))
        return -1;
    if (files != m->s.files || bytes != m->s.bytes)
        return fail(err, errn, "This report is incomplete (its file count does not match "
                    SZ_REPORT ")%s", NULL);
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
