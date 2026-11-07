/*
 * pad_header.c -- the header of a pad recording (pad_header.h).
 */
#include "pad_header.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARKER "# xbr-export:"
#define KEYLINE "# xbr-pad:"

void pad_header_init(PadHeader *h)
{
    memset(h, 0, sizeof *h);
}

static int key_ok(const char *k)
{
    size_t n = 0;
    if (!k || !*k)
        return 0;
    for (; k[n]; n++) {
        char c = k[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
              || c == '_' || c == '-'))
            return 0;
    }
    return n < PAD_HDR_KEY_MAX;
}

static PadHeaderKey *slot(PadHeader *h, const char *key)
{
    for (int i = 0; i < h->n; i++)
        if (!strcmp(h->k[i].key, key))
            return &h->k[i];
    if (h->n >= PAD_HDR_KEYS_MAX)
        return NULL;
    memset(&h->k[h->n], 0, sizeof h->k[0]);
    snprintf(h->k[h->n].key, sizeof h->k[0].key, "%s", key);
    return &h->k[h->n++];
}

/* The length of the UTF-8 sequence led by c, 1 for a stray byte. */
static size_t seq_len(unsigned char c)
{
    return c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
}

/* What a byte costs written in a basic string: \" and \\ take two. */
static size_t esc_cost(unsigned char c)
{
    return c == '"' || c == '\\' ? 2 : 1;
}

/* src without control characters into dst, cut on a character boundary
 * so that its written form, quotes included, fits the reader's bound. */
static void clean_text(char *dst, size_t cap, const char *src)
{
    size_t o = 0, cost = 2;
    const unsigned char *s = (const unsigned char *)src;

    while (*s) {
        size_t n = seq_len(*s), c = 0, i;
        if (*s < 0x20 || *s == 0x7F) {
            s++;
            continue;
        }
        for (i = 0; i < n; i++) {
            if (!s[i])
                break;
            c += esc_cost(s[i]);
        }
        if (i < n || o + n >= cap || cost + c > PAD_HDR_VALUE_MAX)
            break;
        memcpy(dst + o, s, n);
        o += n;
        cost += c;
        s += n;
    }
    dst[o] = 0;
}

int pad_header_set_text(PadHeader *h, const char *key, const char *utf8)
{
    PadHeaderKey *k;
    if (!key_ok(key) || !(k = slot(h, key)))
        return -1;
    clean_text(k->value, sizeof k->value, utf8 ? utf8 : "");
    k->text = 1;
    h->present = 1;
    return 0;
}

int pad_header_set_int(PadHeader *h, const char *key, long long v)
{
    PadHeaderKey *k;
    if (!key_ok(key) || !(k = slot(h, key)))
        return -1;
    snprintf(k->value, sizeof k->value, "%lld", v);
    k->text = 0;
    h->present = 1;
    return 0;
}

const char *pad_header_get(const PadHeader *h, const char *key)
{
    for (int i = 0; i < h->n; i++)
        if (!strcmp(h->k[i].key, key))
            return h->k[i].value;
    return NULL;
}

long long pad_header_get_int(const PadHeader *h, const char *key, long long dflt)
{
    const char *v = pad_header_get(h, key);
    char *end;
    long long r;
    if (!v || !*v)
        return dflt;
    r = strtoll(v, &end, 10);
    return *end ? dflt : r;
}

const char *pad_header_origin(const PadHeader *h)
{
    const char *o = h->present ? pad_header_get(h, "origin") : NULL;
    return o && !strcmp(o, "session") ? "session" : "boot";
}

int pad_header_write(const PadHeader *h, char *out, size_t n)
{
    size_t o = 0;
    int w = snprintf(out, n, "# xbr-export: %s %d\n", PAD_HDR_KIND, PAD_HDR_FORMAT);

    if (w < 0 || (size_t)w >= n)
        return -1;
    o = (size_t)w;
    for (int i = 0; i < h->n; i++) {
        const PadHeaderKey *k = &h->k[i];
        w = snprintf(out + o, n - o, "%s %s = ", KEYLINE, k->key);
        if (w < 0 || (size_t)w >= n - o)
            return -1;
        o += (size_t)w;
        if (k->text) {
            if (o + 1 >= n)
                return -1;
            out[o++] = '"';
            for (const unsigned char *s = (const unsigned char *)k->value; *s; s++) {
                if (*s < 0x20 || *s == 0x7F)
                    continue;
                if (o + 3 >= n)
                    return -1;
                if (*s == '"' || *s == '\\')
                    out[o++] = '\\';
                out[o++] = (char)*s;
            }
            if (o + 2 >= n)
                return -1;
            out[o++] = '"';
            out[o++] = '\n';
            out[o] = 0;
        } else {
            w = snprintf(out + o, n - o, "%s\n", k->value);
            if (w < 0 || (size_t)w >= n - o)
                return -1;
            o += (size_t)w;
        }
    }
    return (int)o;
}

/* ---- reading ------------------------------------------------------------ */

static void put_utf8(char *out, size_t *o, size_t cap, uint32_t cp)
{
    unsigned char b[4];
    size_t n;
    if (cp < 0x80) { b[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (unsigned char)(0xC0 | (cp >> 6)); b[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { b[0] = (unsigned char)(0xE0 | (cp >> 12)); b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3; }
    else { b[0] = (unsigned char)(0xF0 | (cp >> 18)); b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4; }
    /* A decoded control character is dropped as a written one is. */
    if (n == 1 && (cp < 0x20 || cp == 0x7F))
        return;
    if (*o + n < cap) {
        memcpy(out + *o, b, n);
        *o += n;
    }
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* One "key = value" (the text after "# xbr-pad:"), len bytes. */
static int read_key(PadHeader *h, const char *p, size_t len)
{
    const char *e = p + len, *v;
    char key[PAD_HDR_KEY_MAX];
    char val[PAD_HDR_VALUE_MAX + 1];
    size_t kn = 0, o = 0;
    int text = 0;
    PadHeaderKey *k;

    while (p < e && (*p == ' ' || *p == '\t'))
        p++;
    while (p < e && *p != ' ' && *p != '\t' && *p != '=') {
        if (kn + 1 >= sizeof key)
            return PAD_HDR_SYNTAX;
        key[kn++] = *p++;
    }
    key[kn] = 0;
    while (p < e && (*p == ' ' || *p == '\t'))
        p++;
    if (!key_ok(key) || p >= e || *p != '=')
        return PAD_HDR_SYNTAX;
    p++;
    while (p < e && (*p == ' ' || *p == '\t'))
        p++;
    /* Trailing blanks and a comment after the value are TOML's too. */
    while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    if ((size_t)(e - p) > PAD_HDR_VALUE_MAX)
        return PAD_HDR_LONG_VALUE;
    if (p < e && (*p == '"' || *p == '\'')) {
        char q = *p++;
        text = 1;
        for (v = p; v < e && *v != q; v++) {
            if (q == '\'' || *v != '\\') {
                if ((unsigned char)*v >= 0x20 && *v != 0x7F && o + 1 < sizeof val)
                    val[o++] = *v;
                continue;
            }
            if (++v >= e)
                return PAD_HDR_SYNTAX;
            switch (*v) {
            case '"': case '\\': val[o++] = *v; break;
            case 'b': case 't': case 'n': case 'f': case 'r': break;   /* control: dropped */
            case 'u': case 'U': {
                int n = *v == 'u' ? 4 : 8, d;
                uint32_t cp = 0;
                if (e - v <= n)
                    return PAD_HDR_SYNTAX;
                for (int i = 1; i <= n; i++) {
                    if ((d = hexval(v[i])) < 0)
                        return PAD_HDR_SYNTAX;
                    cp = cp << 4 | (uint32_t)d;
                }
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                    return PAD_HDR_SYNTAX;
                put_utf8(val, &o, sizeof val, cp);
                v += n;
                break;
            }
            default:
                return PAD_HDR_SYNTAX;
            }
        }
        if (v >= e)
            return PAD_HDR_SYNTAX;
        for (v++; v < e && (*v == ' ' || *v == '\t'); v++)
            ;
        if (v < e && *v != '#')
            return PAD_HDR_SYNTAX;
    } else {
        /* Control bytes go as in quoted values: the strip draws the
         * header's values, and a bare one is no less a hand edit. */
        for (v = p; v < e && *v != ' ' && *v != '\t' && *v != '#'; v++)
            if ((unsigned char)*v >= 0x20 && *v != 0x7F)
                val[o++] = *v;
        if (!o)
            return PAD_HDR_SYNTAX;
        while (v < e && (*v == ' ' || *v == '\t'))
            v++;
        if (v < e && *v != '#')
            return PAD_HDR_SYNTAX;
    }
    val[o] = 0;
    if (!(k = slot(h, key)))
        return PAD_HDR_TOO_MANY;
    memcpy(k->value, val, o + 1);
    k->text = text;
    return PAD_HDR_OK;
}

static int starts(const char *p, size_t len, const char *s)
{
    size_t n = strlen(s);
    return len >= n && !memcmp(p, s, n);
}

int pad_header_read(const char *text, PadHeader *h)
{
    const char *p = text;
    int marker = 0;

    pad_header_init(h);
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB
        && (unsigned char)p[2] == 0xBF)
        p += 3;
    /* Only the leading comment block: the steps start the body. */
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *next = nl ? nl + 1 : p + len;

        if (len && p[len - 1] == '\r')
            len--;
        if (!len) {
            p = next;
            continue;
        }
        if (p[0] != '#')
            break;
        if (starts(p, len, MARKER) || starts(p, len, KEYLINE)) {
            if (len > PAD_HDR_LINE_MAX)
                return PAD_HDR_LONG_LINE;
        }
        if (!marker && starts(p, len, MARKER)) {
            char kind[16];
            int fmt = 0, i = 0;
            const char *q = p + strlen(MARKER), *e = p + len;
            while (q < e && *q == ' ')
                q++;
            while (q < e && *q != ' ' && i < (int)sizeof kind - 1)
                kind[i++] = *q++;
            kind[i] = 0;
            while (q < e && *q == ' ')
                q++;
            while (q < e && *q >= '0' && *q <= '9' && fmt < 100000)
                fmt = fmt * 10 + (*q++ - '0');
            if (strcmp(kind, PAD_HDR_KIND))
                return PAD_HDR_KIND_OTHER;
            if (fmt > PAD_HDR_FORMAT)
                return PAD_HDR_NEWER;
            if (fmt < 1)
                return PAD_HDR_SYNTAX;
            marker = 1;
            h->present = 1;
        } else if (marker && starts(p, len, KEYLINE)) {
            int rc;
            if (h->n >= PAD_HDR_KEYS_MAX)
                return PAD_HDR_TOO_MANY;
            rc = read_key(h, p + strlen(KEYLINE), len - strlen(KEYLINE));
            if (rc)
                return rc;
        }
        p = next;
    }
    return PAD_HDR_OK;
}

const char *pad_header_error(int rc)
{
    switch (rc) {
    case PAD_HDR_OK: return "ok";
    case PAD_HDR_NEWER: return "made by a newer build (pad format above 1)";
    case PAD_HDR_KIND_OTHER: return "not a pad recording (another export kind)";
    case PAD_HDR_LONG_LINE: return "a header line is too long";
    case PAD_HDR_TOO_MANY: return "the header has too many keys";
    case PAD_HDR_LONG_VALUE: return "a header value is too long";
    default: return "the header cannot be read";
    }
}
