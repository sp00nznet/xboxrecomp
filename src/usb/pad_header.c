/*
 * pad_header.c -- the header of a pad recording (pad_header.h).
 */
#include "pad_header.h"
#include "pad_script.h"

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

int pad_header_list_parse(const char *v, char *out, size_t width, int max)
{
    int n = 0;

    while (*v == ' ' || *v == '\t')
        v++;
    if (*v++ != '[')
        return -1;
    for (;;) {
        const char *s;
        char q;
        size_t len;
        while (*v == ' ' || *v == '\t')
            v++;
        if (*v == ']')
            break;
        if (*v != '"' && *v != '\'')
            return -1;
        q = *v++;
        for (s = v; *v && *v != q; v++)
            /* No escapes: a config key needs none, and a list read here
             * must mean what tomllib reads. */
            if (*v == '\\' && q == '"')
                return -1;
        if (*v != q)
            return -1;
        len = (size_t)(v - s);
        v++;
        if (out && n < max) {
            char *o = out + (size_t)n * width;
            if (len >= width)
                len = width - 1;
            memcpy(o, s, len);
            o[len] = 0;
        }
        n++;
        while (*v == ' ' || *v == '\t')
            v++;
        if (*v == ',') {
            v++;
            continue;
        }
        while (*v == ' ' || *v == '\t')
            v++;
        if (*v != ']')
            return -1;
        break;
    }
    v++;
    while (*v == ' ' || *v == '\t')
        v++;
    return *v ? -1 : n;
}

int pad_header_list(const PadHeader *h, const char *key, char *out, size_t width, int max)
{
    for (int i = 0; i < h->n; i++)
        if (!strcmp(h->k[i].key, key))
            return h->k[i].text ? -1 : pad_header_list_parse(h->k[i].value, out, width, max);
    return 0;
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
    } else if (p < e && *p == '[') {
        /* An array of strings (requires = ["mods.debug_menu.enable"]),
         * kept as written and checked by the list reader, so the writer
         * puts back what it read. */
        char q = 0;
        for (v = p; v < e; v++) {
            if (q) {
                if (*v == q)
                    q = 0;
            } else if (*v == '"' || *v == '\'') {
                q = *v;
            } else if (*v == ']' || *v == '#') {
                break;
            }
            if ((unsigned char)*v >= 0x20 && *v != 0x7F)
                val[o++] = *v;
        }
        if (v >= e || *v != ']')
            return PAD_HDR_SYNTAX;
        val[o++] = *v++;
        val[o] = 0;
        if (pad_header_list_parse(val, NULL, 0, 0) < 0)
            return PAD_HDR_SYNTAX;
        while (v < e && (*v == ' ' || *v == '\t'))
            v++;
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

/* ---- replacing ---------------------------------------------------------- */

char *pad_header_replace(const char *text, const PadHeader *h)
{
    size_t tl = strlen(text), cap = (size_t)PAD_HDR_KEYS_MAX * (PAD_HDR_LINE_MAX + 2) + 64;
    char *out = malloc(cap + tl + 1);
    const char *p = text;
    int w;
    size_t o;

    if (!out)
        return NULL;
    if ((w = pad_header_write(h, out, cap)) < 0) {
        free(out);
        return NULL;
    }
    o = (size_t)w;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB
        && (unsigned char)p[2] == 0xBF)
        p += 3;
    /* The leading comment block loses its marker and key lines; its other
     * comments and blank lines stay, and from the first step on the text
     * is copied as it is. */
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *next = nl ? nl + 1 : p + len;
        size_t bare = len && p[len - 1] == '\r' ? len - 1 : len;

        if (bare && p[0] != '#')
            break;
        if (!starts(p, bare, MARKER) && !starts(p, bare, KEYLINE)) {
            memcpy(out + o, p, (size_t)(next - p));
            o += (size_t)(next - p);
        }
        p = next;
    }
    memcpy(out + o, p, strlen(p) + 1);
    return out;
}

/* ---- a whole file ------------------------------------------------------- */

int pad_header_title_check(const PadHeader *h, uint32_t title_id, const char *title,
                           char *why, size_t whyn)
{
    const char *tid = h->present ? pad_header_get(h, "title_id") : NULL;
    const char *of;

    if (!title_id || !tid || !*tid || strtoul(tid, NULL, 16) == title_id)
        return 0;
    of = pad_header_get(h, "title");
    if (!of || !*of)
        of = pad_header_get(h, "game");
    if (title && *title)
        snprintf(why, whyn, "a recording of %s (%s), not of %s (%08x)",
                 of && *of ? of : "another game", tid, title, (unsigned)title_id);
    else
        snprintf(why, whyn, "a recording of %s (%s), not of this game (%08x)",
                 of && *of ? of : "another game", tid, (unsigned)title_id);
    return -1;
}

int pad_file_check(const char *text, size_t len, uint32_t title_id, const char *title,
                   PadFileInfo *out, char *why, size_t whyn)
{
    PadFileInfo info;
    PadStep *steps;
    PadAnchors *anchors;
    int rc;

    if (why && whyn)
        why[0] = 0;
    memset(&info, 0, sizeof info);
    if (len > (size_t)PAD_FILE_MAX) {
        snprintf(why, whyn, "over %ld MB", PAD_FILE_MAX >> 20);
        return -1;
    }
    /* A NUL inside is a binary, not a recording the parser can read whole. */
    if (strlen(text) != len) {
        snprintf(why, whyn, "not a text file");
        return -1;
    }
    if ((rc = pad_header_read(text, &info.h)) < 0) {
        snprintf(why, whyn, "%s", pad_header_error(rc));
        return -1;
    }
    if (pad_header_title_check(&info.h, title_id, title, why, whyn) < 0)
        return -1;
    info.anchors = pad_script_anchor_lines(text);
    if (info.anchors > PAD_ANCHOR_MAX) {
        snprintf(why, whyn, "%d anchors, over %d", info.anchors, PAD_ANCHOR_MAX);
        return -1;
    }
    /* One step past the cap tells a full file from one the cap cut. */
    steps = malloc(sizeof *steps * (PAD_STEPS_MAX + 1));
    anchors = calloc(1, sizeof *anchors);
    if (!steps || !anchors) {
        free(steps);
        free(anchors);
        snprintf(why, whyn, "out of memory");
        return -1;
    }
    /* Parsed as the session parses it, anchors and all, so the count is
     * the one playback would see. */
    info.steps = pad_script_parse_anchored(text, steps, 0, PAD_STEPS_MAX + 1, anchors);
    free(steps);
    free(anchors);
    if (info.steps > PAD_STEPS_MAX) {
        snprintf(why, whyn, "more than %d steps", PAD_STEPS_MAX);
        return -1;
    }
    if (info.steps < 1) {
        snprintf(why, whyn, "no steps");
        return -1;
    }
    if (out)
        *out = info;
    return 0;
}
