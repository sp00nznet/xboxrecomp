/*
 * recomp_cfg.c - a small TOML-subset config reader; see recomp_cfg.h.
 *
 * One pass over the whole text with a cursor and a line counter. Entries are
 * kept flat, by full dotted key, in file order; lookups are linear (a config
 * file has tens of keys and is read at startup).
 */
#include "recomp_cfg.h"

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct {
    recomp_cfg_type type;
    union { char *s; long long i; double f; int b; } v;
} Val;

typedef struct {
    char *key;
    int line;
    Val val;          /* type RCFG_ARRAY: items below */
    Val *items;
    size_t n_items;
    /* Where it sits in the text, for recomp_cfg_patch: the line's start,
     * the value's bytes, and the start of the line after it (past any
     * comment and the newline); the [header] it sits under, or -1. */
    size_t lbeg, vbeg, vend, lnext;
    long tab;
} Entry;

struct recomp_cfg {
    Entry *e;
    size_t n, cap;
    char **tables;    /* [headers] seen, to refuse a second [render] */
    size_t n_tables, cap_tables;
    size_t *tab_at;   /* the start of each header's line, by index */
    size_t cap_tab_at;
    char **dotted;    /* tables made by dotted keys (a.b = 1 makes a) */
    size_t n_dotted, cap_dotted;
    /* [[name]] arrays of tables (recomp_cfg_load_string_aot only): each
     * name and how many of its tables were opened. */
    char **aot;
    size_t *aot_n;
    size_t n_aot, cap_aot;
    char *name;
};

typedef struct {
    const char *base, *p, *end;
    int line;
    recomp_cfg *cfg;
    char *err;
    size_t errlen;
    int failed;
    int aot;          /* [[name]] allowed (recomp_cfg_load_string_aot) */
} P;

/* ── small helpers ─────────────────────────────────────────────────── */

static char *dup_n(const char *s, size_t n)
{
    char *d = (char *)malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

static int fail(P *ps, const char *fmt, ...)
{
    if (!ps->failed && ps->err && ps->errlen) {
        char msg[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        snprintf(ps->err, ps->errlen, "%s:%d: %s", ps->cfg->name, ps->line, msg);
    }
    ps->failed = 1;
    return 0;
}

static void val_free(Val *v)
{
    if (v->type == RCFG_STRING)
        free(v->v.s);
}

/* Growable byte buffer for keys and strings. */
typedef struct { char *b; size_t n, cap; } Buf;

static int buf_put(Buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        char *nb;
        while (cap < b->n + n + 1)
            cap *= 2;
        nb = (char *)realloc(b->b, cap);
        if (!nb)
            return 0;
        b->b = nb;
        b->cap = cap;
    }
    memcpy(b->b + b->n, s, n);
    b->n += n;
    b->b[b->n] = 0;
    return 1;
}

static int is_bare(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static void skip_ws(P *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t'))
        ps->p++;
}

/* A comment runs to the end of the line; like a string it may not hold
 * control characters other than tab (a lone CR or a DEL is an error). */
static void skip_comment(P *ps)
{
    if (ps->p >= ps->end || *ps->p != '#')
        return;
    while (ps->p < ps->end && *ps->p != '\n') {
        unsigned char c = (unsigned char)*ps->p;
        if (c == '\r' && ps->p + 1 < ps->end && ps->p[1] == '\n')
            return;
        if ((c < 0x20 && c != '\t') || c == 0x7F) {
            fail(ps, "control character in comment");
            return;
        }
        ps->p++;
    }
}

/* Consume a newline (\n or \r\n). 1 if one was there. */
static int eat_newline(P *ps)
{
    if (ps->p < ps->end && *ps->p == '\n') {
        ps->p++;
        ps->line++;
        return 1;
    }
    if (ps->end - ps->p >= 2 && ps->p[0] == '\r' && ps->p[1] == '\n') {
        ps->p += 2;
        ps->line++;
        return 1;
    }
    return 0;
}

/* Whitespace, an optional comment, then end of line or of input. */
static int expect_eol(P *ps)
{
    skip_ws(ps);
    skip_comment(ps);
    if (ps->p >= ps->end || eat_newline(ps))
        return 1;
    return fail(ps, "expected end of line, found '%c'", *ps->p);
}

/* Whitespace, newlines and comments, inside an array. */
static void skip_blank(P *ps)
{
    for (;;) {
        skip_ws(ps);
        skip_comment(ps);
        if (!eat_newline(ps))
            return;
    }
}

/* ── strings ───────────────────────────────────────────────────────── */

static int put_utf8(Buf *b, unsigned long cp)
{
    char u[4];
    size_t n;
    if (cp < 0x80) { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                             u[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { u[0] = (char)(0xF0 | (cp >> 18)); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
           u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    return buf_put(b, u, n);
}

/* At a '"' or '\''. Appends the decoded string to `out`. */
static int parse_string(P *ps, Buf *out)
{
    char q = *ps->p;
    if (ps->end - ps->p >= 3 && ps->p[1] == q && ps->p[2] == q)
        return fail(ps, "multi-line strings are not supported");
    ps->p++;
    for (;;) {
        char c;
        if (ps->p >= ps->end || *ps->p == '\n' || *ps->p == '\r')
            return fail(ps, "unterminated string");
        c = *ps->p++;
        if (c == q)
            return 1;
        if (c == '\\' && q == '"') {
            char e;
            if (ps->p >= ps->end)
                return fail(ps, "unterminated string");
            e = *ps->p++;
            switch (e) {
            case 'b': c = '\b'; break;
            case 't': c = '\t'; break;
            case 'n': c = '\n'; break;
            case 'f': c = '\f'; break;
            case 'r': c = '\r'; break;
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case 'u': case 'U': {
                int nd = e == 'u' ? 4 : 8, k;
                unsigned long cp = 0;
                for (k = 0; k < nd; k++) {
                    char h = ps->p < ps->end ? *ps->p : 0;
                    int d = (h >= '0' && h <= '9') ? h - '0' :
                            (h >= 'a' && h <= 'f') ? h - 'a' + 10 :
                            (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                    if (d < 0)
                        return fail(ps, "bad \\%c escape", e);
                    cp = cp * 16 + (unsigned long)d;
                    ps->p++;
                }
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                    return fail(ps, "bad unicode code point");
                if (!put_utf8(out, cp))
                    return fail(ps, "out of memory");
                continue;
            }
            default:
                return fail(ps, "bad escape '\\%c'", e);
            }
        } else if (((unsigned char)c < 0x20 && c != '\t') || c == 0x7F) {
            return fail(ps, "control character in string");
        }
        if (!buf_put(out, &c, 1))
            return fail(ps, "out of memory");
    }
}

/* ── keys ──────────────────────────────────────────────────────────── */

/* Tables whose keys are names with dots in them, stored flat: under
 * [input.actions], "menu.open" = [...] (what a TOML writer emits for that
 * key) and menu.open = [...] are the same entry; so are [mods]
 * "camera.free" and camera.free (a mod's name is its key), and so are
 * [input.controls] "camera.free.look" and camera.free.look. Only a last key
 * part may quote a dot, and only directly under one of these. */
static const char *const flat_tables[] = { "input.actions.", "input.controls.", "mods." };

/* The length of the flat table prefix `key` starts with ("mods."), or 0. */
static size_t flat_prefix(const char *key)
{
    size_t i;
    for (i = 0; i < sizeof flat_tables / sizeof flat_tables[0]; i++) {
        size_t n = strlen(flat_tables[i]);
        if (!strncmp(key, flat_tables[i], n) && key[n])
            return n;
    }
    return 0;
}

static int flat_quoted(const Buf *out, size_t before)
{
    for (size_t i = 0; i < sizeof flat_tables / sizeof flat_tables[0]; i++) {
        size_t n = strlen(flat_tables[i]);
        if (before == n && !memcmp(out->b, flat_tables[i], n))
            return 1;
    }
    return 0;
}

/* A bare, quoted or dotted key, appended to `out` with '.' between parts. */
static int parse_key(P *ps, Buf *out)
{
    for (;;) {
        skip_ws(ps);
        if (ps->p < ps->end && (*ps->p == '"' || *ps->p == '\'')) {
            size_t before = out->n;
            if (!parse_string(ps, out))
                return 0;
            if (out->n == before)
                return fail(ps, "empty key");
            if (memchr(out->b + before, '.', out->n - before)) {
                if (!flat_quoted(out, before))
                    return fail(ps, "'.' inside a quoted key is not supported");
                skip_ws(ps);
                if (ps->p < ps->end && *ps->p == '.')
                    return fail(ps, "a quoted key with '.' must be the last part");
                return 1;
            }
        } else {
            const char *s = ps->p;
            while (ps->p < ps->end && is_bare(*ps->p))
                ps->p++;
            if (ps->p == s)
                return ps->p < ps->end && *ps->p != '\n'
                     ? fail(ps, "expected a key, found '%c'", *ps->p)
                     : fail(ps, "expected a key");
            if (!buf_put(out, s, (size_t)(ps->p - s)))
                return fail(ps, "out of memory");
        }
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == '.') {
            ps->p++;
            if (!buf_put(out, ".", 1))
                return fail(ps, "out of memory");
            continue;
        }
        return 1;
    }
}

/* ── values ────────────────────────────────────────────────────────── */

static int is_dec(char c) { return c >= '0' && c <= '9'; }
static int is_hex(char c) { return is_dec(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int is_oct(char c) { return c >= '0' && c <= '7'; }
static int is_bin(char c) { return c == '0' || c == '1'; }

/* s[0..n) must be digits of `isd`, with each '_' between two digits.
 * Appends the digits (no '_') to out at *on. 1 ok, 0 empty or a bad
 * character, -1 a misplaced '_'. */
static int take_digits(const char *s, size_t n, int (*isd)(char), char *out, size_t *on)
{
    size_t i;
    if (!n)
        return 0;
    for (i = 0; i < n; i++) {
        if (s[i] == '_') {
            if (i == 0 || i + 1 == n || !isd(s[i - 1]) || !isd(s[i + 1]))
                return -1;
            continue;
        }
        if (!isd(s[i]))
            return 0;
        out[(*on)++] = s[i];
    }
    out[*on] = 0;
    return 1;
}

static int bad_number(P *ps, int r, const char *tok)
{
    return r < 0 ? fail(ps, "misplaced '_' in '%s'", tok)
                 : fail(ps, "invalid number '%s'", tok);
}

static int parse_number(P *ps, Val *v)
{
    char tok[128], clean[128];
    size_t n = 0, cn = 0;
    const char *t, *q, *e;
    int sign = 0, is_float = 0, r;
    char *endp;

    while (ps->p < ps->end && (is_bare(*ps->p) || *ps->p == '+' || *ps->p == '.')) {
        if (n + 1 >= sizeof tok)
            return fail(ps, "number too long");
        tok[n++] = *ps->p++;
    }
    tok[n] = 0;
    if ((ps->p < ps->end && *ps->p == ':') ||
        (n >= 10 && is_dec(tok[0]) && is_dec(tok[3]) && tok[4] == '-' && tok[7] == '-'))
        return fail(ps, "dates and times are not supported");
    if (n == 0)
        return ps->p < ps->end ? fail(ps, "expected a value, found '%c'", *ps->p)
                               : fail(ps, "expected a value");

    t = tok;
    if (*t == '+' || *t == '-') {
        sign = *t == '-' ? -1 : 1;
        clean[cn++] = *t++;
    }
    if (!strcmp(t, "inf") || !strcmp(t, "nan")) {
        v->type = RCFG_FLOAT;
        v->v.f = t[0] == 'i' ? (sign < 0 ? -HUGE_VAL : HUGE_VAL) : NAN;
        return 1;
    }
    if (t[0] == '0' && (t[1] == 'x' || t[1] == 'o' || t[1] == 'b')) {
        int base = t[1] == 'x' ? 16 : t[1] == 'o' ? 8 : 2;
        unsigned long long u;
        if (sign)
            return fail(ps, "sign on a 0%c integer", t[1]);
        r = take_digits(t + 2, strlen(t + 2), base == 16 ? is_hex : base == 8 ? is_oct : is_bin,
                        clean, &cn);
        if (r <= 0)
            return bad_number(ps, r, tok);
        errno = 0;
        u = strtoull(clean, &endp, base);   /* clean is digits only */
        if (*endp || errno == ERANGE || u > (unsigned long long)9223372036854775807LL)
            return fail(ps, "integer out of range '%s'", tok);
        v->type = RCFG_INT;
        v->v.i = (long long)u;
        return 1;
    }
    if (!is_dec(t[0]))
        return fail(ps, "invalid value '%s' (strings need quotes)", tok);

    /* dec-int [ '.' digits ] [ (e|E) [+-] digits ] */
    for (q = t; *q && *q != '.' && *q != 'e' && *q != 'E'; q++)
        ;
    {
        size_t int_at = cn;
        r = take_digits(t, (size_t)(q - t), is_dec, clean, &cn);
        if (r <= 0)
            return bad_number(ps, r, tok);
        if (clean[int_at] == '0' && cn - int_at > 1)
            return fail(ps, "leading zero in '%s'", tok);
    }
    if (*q == '.') {
        is_float = 1;
        clean[cn++] = '.';
        for (e = ++q; *e && *e != 'e' && *e != 'E'; e++)
            ;
        r = take_digits(q, (size_t)(e - q), is_dec, clean, &cn);
        if (r <= 0)
            return bad_number(ps, r, tok);
        q = e;
    }
    if (*q == 'e' || *q == 'E') {
        is_float = 1;
        clean[cn++] = 'e';
        q++;
        if (*q == '+' || *q == '-')
            clean[cn++] = *q++;
        r = take_digits(q, strlen(q), is_dec, clean, &cn);
        if (r <= 0)
            return bad_number(ps, r, tok);
    } else if (*q) {
        return fail(ps, "invalid number '%s'", tok);
    }
    clean[cn] = 0;

    if (is_float) {
        v->type = RCFG_FLOAT;
        if (!recomp_cfg_parse_double(clean, &v->v.f))
            return fail(ps, "invalid number '%s'", tok);
        return 1;
    }
    errno = 0;
    v->type = RCFG_INT;
    v->v.i = strtoll(clean, &endp, 10);
    if (*endp)
        return fail(ps, "invalid number '%s'", tok);
    if (errno == ERANGE)
        return fail(ps, "integer out of range '%s'", tok);
    return 1;
}

static int parse_scalar(P *ps, Val *v)
{
    char c = ps->p < ps->end ? *ps->p : 0;
    if (c == '"' || c == '\'') {
        Buf b = {0};
        if (!buf_put(&b, "", 0) || !parse_string(ps, &b)) {
            free(b.b);
            return ps->failed ? 0 : fail(ps, "out of memory");
        }
        v->type = RCFG_STRING;
        v->v.s = b.b;
        return 1;
    }
    if (c == '{')
        return fail(ps, "inline tables are not supported");
    if (c == '[')
        return fail(ps, "nested arrays are not supported");
    if (ps->end - ps->p >= 4 && !memcmp(ps->p, "true", 4) &&
        (ps->end - ps->p == 4 || !is_bare(ps->p[4]))) {
        ps->p += 4;
        v->type = RCFG_BOOL;
        v->v.b = 1;
        return 1;
    }
    if (ps->end - ps->p >= 5 && !memcmp(ps->p, "false", 5) &&
        (ps->end - ps->p == 5 || !is_bare(ps->p[5]))) {
        ps->p += 5;
        v->type = RCFG_BOOL;
        v->v.b = 0;
        return 1;
    }
    return parse_number(ps, v);
}

static int parse_array(P *ps, Entry *e)
{
    size_t cap = 0;
    ps->p++;   /* '[' */
    for (;;) {
        skip_blank(ps);
        if (ps->p >= ps->end)
            return fail(ps, "unterminated array");
        if (*ps->p == ']') {
            ps->p++;
            return 1;
        }
        if (e->n_items == cap) {
            size_t nc = cap ? cap * 2 : 4;
            Val *ni = (Val *)realloc(e->items, nc * sizeof *ni);
            if (!ni)
                return fail(ps, "out of memory");
            e->items = ni;
            cap = nc;
        }
        if (!parse_scalar(ps, &e->items[e->n_items]))
            return 0;
        e->n_items++;
        skip_blank(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            return 1;
        }
        return ps->p < ps->end ? fail(ps, "expected ',' or ']' in array, found '%c'", *ps->p)
                               : fail(ps, "unterminated array");
    }
}

/* ── the table of entries ──────────────────────────────────────────── */

/* 1 if `a` is `b` or a dotted prefix of it ("render" of "render.scale"). */
static int is_prefix(const char *a, const char *b)
{
    size_t n = strlen(a);
    return !strncmp(a, b, n) && (b[n] == 0 || b[n] == '.');
}

static Entry *entry_get(const recomp_cfg *cfg, const char *key)
{
    long i = recomp_cfg_find(cfg, key);
    return i < 0 ? NULL : &cfg->e[i];
}

static int list_has(char *const *l, size_t n, const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (strlen(l[i]) == len && !strncmp(l[i], s, len))
            return 1;
    return 0;
}

static int list_add(char ***l, size_t *n, size_t *cap, const char *s, size_t len)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        char **nl = (char **)realloc(*l, nc * sizeof *nl);
        if (!nl)
            return 0;
        *l = nl;
        *cap = nc;
    }
    if (!((*l)[*n] = dup_n(s, len)))
        return 0;
    (*n)++;
    return 1;
}

/* TOML 1.0: a dotted key defines each table before its last part; such a
 * table may not be reopened with a [header], and a dotted key may not
 * reach into a table a [header] defined. `from` is where the key's own
 * parts start (past the current [table] prefix). */
static int note_dotted(P *ps, const char *key, size_t from)
{
    recomp_cfg *c = ps->cfg;
    const char *d;
    for (d = strchr(key + from, '.'); d; d = strchr(d + 1, '.')) {
        size_t len = (size_t)(d - key);
        if (list_has(c->tables, c->n_tables, key, len))
            return fail(ps, "table [%.*s] was defined by a header; a dotted key cannot add to it",
                        (int)len, key);
        if (!list_has(c->dotted, c->n_dotted, key, len) &&
            !list_add(&c->dotted, &c->n_dotted, &c->cap_dotted, key, len))
            return fail(ps, "out of memory");
    }
    return 1;
}

/* A key may not repeat, may not sit under a key that holds a value, and may
 * not be a table that already has keys under it. */
static int check_key(P *ps, const char *key)
{
    size_t i, flat = flat_prefix(key);
    for (i = 0; i < ps->cfg->n; i++) {
        const char *k = ps->cfg->e[i].key;
        if (!strcmp(k, key))
            return fail(ps, "duplicate key '%s' (first set on line %d)", key, ps->cfg->e[i].line);
        /* Under a flat table every name is one key, so a mod's switch
         * "camera.free" and its setting "camera.free.speed" sit side by
         * side; only the same name twice clashes. */
        if (flat && flat_prefix(k) == flat && !strncmp(k, key, flat))
            continue;
        if (is_prefix(k, key))
            return fail(ps, "'%s' is a value (line %d), not a table", k, ps->cfg->e[i].line);
        if (is_prefix(key, k))
            return fail(ps, "'%s' is a table, not a value", key);
    }
    for (i = 0; i < ps->cfg->n_tables; i++)
        if (is_prefix(key, ps->cfg->tables[i]))
            return fail(ps, "'%s' is a table, not a value", key);
    return 1;
}

/* The start of the line `p` is on. */
static size_t line_start(const P *ps, const char *p)
{
    while (p > ps->base && p[-1] != '\n')
        p--;
    return (size_t)(p - ps->base);
}

/* The index of `name` among the [[name]] arrays, or -1. */
static long aot_find(const recomp_cfg *c, const char *name, size_t len)
{
    size_t i;
    for (i = 0; i < c->n_aot; i++)
        if (strlen(c->aot[i]) == len && !strncmp(c->aot[i], name, len))
            return (long)i;
    return -1;
}

/* [[name]]: the next table of the array, held as [name.N] (N from 0), so
 * its keys read as "name.N.key". Only one level: no [[a.b]] under [[a]]. */
static int parse_aot_header(P *ps, Buf *table, size_t at)
{
    recomp_cfg *c = ps->cfg;
    char num[24];
    long k;
    size_t i;

    ps->p++;   /* the second '[' */
    table->n = 0;
    if (!buf_put(table, "", 0) || !parse_key(ps, table))
        return 0;
    if (ps->end - ps->p < 2 || ps->p[0] != ']' || ps->p[1] != ']')
        return fail(ps, "expected ']]' after array of tables name");
    ps->p += 2;
    if (aot_find(c, table->b, table->n) < 0)
        for (i = 0; i < c->n_tables; i++)
            if (is_prefix(table->b, c->tables[i]) || is_prefix(c->tables[i], table->b))
                return fail(ps, "[[%s]]: [%s] is a table", table->b, c->tables[i]);
    if (list_has(c->dotted, c->n_dotted, table->b, table->n))
        return fail(ps, "[[%s]]: already defined by a dotted key", table->b);
    for (i = 0; i < c->n; i++)
        if (is_prefix(c->e[i].key, table->b) || !strcmp(c->e[i].key, table->b))
            return fail(ps, "'%s' is a value (line %d), not an array of tables",
                        c->e[i].key, c->e[i].line);
    k = aot_find(c, table->b, table->n);
    if (k < 0) {
        if (c->n_aot == c->cap_aot) {
            size_t nc = c->cap_aot ? c->cap_aot * 2 : 4;
            size_t *nn = (size_t *)realloc(c->aot_n, nc * sizeof *nn);
            char **na;
            if (!nn)
                return fail(ps, "out of memory");
            c->aot_n = nn;
            na = (char **)realloc(c->aot, nc * sizeof *na);
            if (!na)
                return fail(ps, "out of memory");
            c->aot = na;
            c->cap_aot = nc;
        }
        if (!(c->aot[c->n_aot] = dup_n(table->b, table->n)))
            return fail(ps, "out of memory");
        c->aot_n[c->n_aot] = 0;
        k = (long)c->n_aot++;
    }
    snprintf(num, sizeof num, ".%zu", c->aot_n[k]++);
    if (!buf_put(table, num, strlen(num)))
        return fail(ps, "out of memory");
    if (c->n_tables == c->cap_tab_at) {
        size_t nc = c->cap_tab_at ? c->cap_tab_at * 2 : 8;
        size_t *na = (size_t *)realloc(c->tab_at, nc * sizeof *na);
        if (!na)
            return fail(ps, "out of memory");
        c->tab_at = na;
        c->cap_tab_at = nc;
    }
    c->tab_at[c->n_tables] = at;
    if (!list_add(&c->tables, &c->n_tables, &c->cap_tables, table->b, table->n))
        return fail(ps, "out of memory");
    return expect_eol(ps);
}

static int parse_table_header(P *ps, Buf *table)
{
    size_t i, at = line_start(ps, ps->p);
    ps->p++;   /* '[' */
    if (ps->p < ps->end && *ps->p == '[') {
        if (!ps->aot)
            return fail(ps, "arrays of tables are not supported");
        return parse_aot_header(ps, table, at);
    }
    table->n = 0;
    if (!buf_put(table, "", 0) || !parse_key(ps, table))
        return 0;
    if (ps->p >= ps->end || *ps->p != ']')
        return fail(ps, "expected ']' after table name");
    ps->p++;
    for (i = 0; i < ps->cfg->n_aot; i++)
        if (is_prefix(ps->cfg->aot[i], table->b))
            return fail(ps, "table [%s] is inside the array of tables [[%s]]", table->b,
                        ps->cfg->aot[i]);
    for (i = 0; i < ps->cfg->n_tables; i++)
        if (!strcmp(ps->cfg->tables[i], table->b))
            return fail(ps, "table [%s] defined twice", table->b);
    if (list_has(ps->cfg->dotted, ps->cfg->n_dotted, table->b, table->n))
        return fail(ps, "table [%s] was already defined by a dotted key", table->b);
    for (i = 0; i < ps->cfg->n; i++)
        if (is_prefix(ps->cfg->e[i].key, table->b))
            return fail(ps, "'%s' is a value (line %d), not a table",
                        ps->cfg->e[i].key, ps->cfg->e[i].line);
    if (ps->cfg->n_tables == ps->cfg->cap_tab_at) {
        size_t nc = ps->cfg->cap_tab_at ? ps->cfg->cap_tab_at * 2 : 8;
        size_t *na = (size_t *)realloc(ps->cfg->tab_at, nc * sizeof *na);
        if (!na)
            return fail(ps, "out of memory");
        ps->cfg->tab_at = na;
        ps->cfg->cap_tab_at = nc;
    }
    ps->cfg->tab_at[ps->cfg->n_tables] = at;
    if (!list_add(&ps->cfg->tables, &ps->cfg->n_tables, &ps->cfg->cap_tables,
                  table->b, table->n))
        return fail(ps, "out of memory");
    return expect_eol(ps);
}

static int parse_keyval(P *ps, const Buf *table)
{
    Buf key = {0};
    Entry e;
    int line = ps->line;

    memset(&e, 0, sizeof e);
    e.lbeg = line_start(ps, ps->p);
    e.tab = table->n ? (long)ps->cfg->n_tables - 1 : -1;
    if (table->n && (!buf_put(&key, table->b, table->n) || !buf_put(&key, ".", 1)))
        return fail(ps, "out of memory");
    if (!buf_put(&key, "", 0) || !parse_key(ps, &key))
        goto bad;
    if (ps->p >= ps->end || *ps->p != '=') {
        fail(ps, "expected '=' after key '%s'", key.b);
        goto bad;
    }
    ps->p++;
    skip_ws(ps);
    if (!check_key(ps, key.b) || !note_dotted(ps, key.b, table->n ? table->n + 1 : 0))
        goto bad;
    e.vbeg = (size_t)(ps->p - ps->base);
    if (ps->p < ps->end && *ps->p == '[') {
        e.val.type = RCFG_ARRAY;
        if (!parse_array(ps, &e))
            goto bad;
    } else if (!parse_scalar(ps, &e.val)) {
        goto bad;
    }
    e.vend = (size_t)(ps->p - ps->base);
    if (!expect_eol(ps))
        goto bad;
    e.lnext = (size_t)(ps->p - ps->base);
    if (ps->cfg->n == ps->cfg->cap) {
        size_t nc = ps->cfg->cap ? ps->cfg->cap * 2 : 16;
        Entry *ne = (Entry *)realloc(ps->cfg->e, nc * sizeof *ne);
        if (!ne) {
            fail(ps, "out of memory");
            goto bad;
        }
        ps->cfg->e = ne;
        ps->cfg->cap = nc;
    }
    e.key = key.b;
    e.line = line;
    ps->cfg->e[ps->cfg->n++] = e;
    return 1;
bad:
    {
        size_t i;
        for (i = 0; i < e.n_items; i++)
            val_free(&e.items[i]);
        free(e.items);
        val_free(&e.val);
        free(key.b);
    }
    return 0;
}

/* The first byte of a malformed UTF-8 sequence (overlong, surrogate,
 * above U+10FFFF, truncated), or NULL when the text is valid. */
static const char *utf8_invalid(const char *s, const char *end)
{
    const unsigned char *p = (const unsigned char *)s, *e = (const unsigned char *)end;
    while (p < e) {
        unsigned c = *p;
        unsigned long cp;
        int n, k;
        if (c < 0x80) { p++; continue; }
        if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; }
        else return (const char *)p;
        if (e - p <= n)
            return (const char *)p;
        for (k = 1; k <= n; k++) {
            if ((p[k] & 0xC0) != 0x80)
                return (const char *)p;
            cp = (cp << 6) | (p[k] & 0x3F);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return (const char *)p;
        p += n + 1;
    }
    return NULL;
}

/* ── loading ───────────────────────────────────────────────────────── */

static recomp_cfg *load_string(const char *text, const char *name, char *err, size_t errlen,
                               int aot)
{
    recomp_cfg *cfg = (recomp_cfg *)calloc(1, sizeof *cfg);
    Buf table = {0};
    P ps;

    if (err && errlen)
        err[0] = 0;
    if (!cfg || !(cfg->name = dup_n(name ? name : "<string>", strlen(name ? name : "<string>")))) {
        if (err && errlen)
            snprintf(err, errlen, "out of memory");
        free(cfg);
        return NULL;
    }
    memset(&ps, 0, sizeof ps);
    ps.base = ps.p = text ? text : "";
    ps.end = ps.p + strlen(ps.p);
    ps.line = 1;
    ps.cfg = cfg;
    ps.err = err;
    ps.errlen = errlen;
    ps.aot = aot;
    if (ps.end - ps.p >= 3 && !memcmp(ps.p, "\xEF\xBB\xBF", 3))
        ps.p += 3;   /* UTF-8 byte-order mark */
    {
        const char *bad = utf8_invalid(ps.p, ps.end);
        if (bad) {
            const char *q;
            for (q = ps.p; q < bad; q++)
                ps.line += *q == '\n';
            fail(&ps, "invalid UTF-8");
        }
    }
    buf_put(&table, "", 0);

    while (ps.p < ps.end && !ps.failed) {
        skip_ws(&ps);
        skip_comment(&ps);
        if (ps.p >= ps.end || eat_newline(&ps))
            continue;
        if (*ps.p == '[')
            parse_table_header(&ps, &table);
        else
            parse_keyval(&ps, &table);
    }
    free(table.b);
    if (ps.failed) {
        recomp_cfg_free(cfg);
        return NULL;
    }
    return cfg;
}

recomp_cfg *recomp_cfg_load_string(const char *text, const char *name,
                                   char *err, size_t errlen)
{
    return load_string(text, name, err, errlen, 0);
}

recomp_cfg *recomp_cfg_load_string_aot(const char *text, const char *name,
                                       char *err, size_t errlen)
{
    return load_string(text, name, err, errlen, 1);
}

size_t recomp_cfg_table_count(const recomp_cfg *cfg, const char *name)
{
    long k = cfg && name ? aot_find(cfg, name, strlen(name)) : -1;
    return k < 0 ? 0 : cfg->aot_n[k];
}

recomp_cfg *recomp_cfg_load_file(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    char *text;
    long size;
    size_t got;
    recomp_cfg *cfg;

    if (!f) {
        if (err && errlen)
            snprintf(err, errlen, "%s: cannot open: %s", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
        !(text = (char *)malloc((size_t)size + 1))) {
        if (err && errlen)
            snprintf(err, errlen, "%s: cannot read", path);
        fclose(f);
        return NULL;
    }
    got = fread(text, 1, (size_t)size, f);
    fclose(f);
    text[got] = 0;
    if (strlen(text) != got) {
        if (err && errlen)
            snprintf(err, errlen, "%s: NUL byte in file", path);
        free(text);
        return NULL;
    }
    cfg = recomp_cfg_load_string(text, path, err, errlen);
    free(text);
    return cfg;
}

void recomp_cfg_free(recomp_cfg *cfg)
{
    size_t i, k;
    if (!cfg)
        return;
    for (i = 0; i < cfg->n; i++) {
        for (k = 0; k < cfg->e[i].n_items; k++)
            val_free(&cfg->e[i].items[k]);
        free(cfg->e[i].items);
        val_free(&cfg->e[i].val);
        free(cfg->e[i].key);
    }
    for (i = 0; i < cfg->n_tables; i++)
        free(cfg->tables[i]);
    free(cfg->tables);
    free(cfg->tab_at);
    for (i = 0; i < cfg->n_dotted; i++)
        free(cfg->dotted[i]);
    free(cfg->dotted);
    for (i = 0; i < cfg->n_aot; i++)
        free(cfg->aot[i]);
    free(cfg->aot);
    free(cfg->aot_n);
    free(cfg->e);
    free(cfg->name);
    free(cfg);
}

int recomp_cfg_parse_double(const char *s, double *out)
{
    char buf[128];
    const char *dp, *q;
    size_t dl, n = 0;
    char *end;
    double d;

    if (!s || !out)
        return 0;
    q = s + (*s == '+' || *s == '-');
    if (!*q)
        return 0;
    for (; *q; q++)   /* digits . e E + - only: no hex floats, spaces, ',' */
        if (!(is_dec(*q) || *q == '.' || *q == 'e' || *q == 'E' || *q == '+' || *q == '-'))
            break;
    if (*q) {
        q = s + (*s == '+' || *s == '-');
        if (strcmp(q, "inf") && strcmp(q, "nan") && strcmp(q, "infinity"))
            return 0;
        d = q[0] == 'n' ? NAN : (*s == '-' ? -HUGE_VAL : HUGE_VAL);
        *out = d;
        return 1;
    }
    /* strtod reads the locale's decimal point; hand it that instead of '.'. */
    dp = localeconv()->decimal_point;
    dl = dp && *dp ? strlen(dp) : 1;
    for (q = s; *q; q++) {
        if (*q == '.') {
            if (n + dl >= sizeof buf)
                return 0;
            memcpy(buf + n, dp && *dp ? dp : ".", dl);
            n += dl;
        } else {
            if (n + 1 >= sizeof buf)
                return 0;
            buf[n++] = *q;
        }
    }
    buf[n] = 0;
    d = strtod(buf, &end);
    if (end == buf || *end)
        return 0;
    *out = d;
    return 1;
}

/* ── getters ───────────────────────────────────────────────────────── */

long recomp_cfg_find(const recomp_cfg *cfg, const char *key)
{
    size_t i;
    if (!cfg || !key)
        return -1;
    for (i = 0; i < cfg->n; i++)
        if (!strcmp(cfg->e[i].key, key))
            return (long)i;
    return -1;
}

size_t recomp_cfg_count(const recomp_cfg *cfg) { return cfg ? cfg->n : 0; }

const char *recomp_cfg_key_at(const recomp_cfg *cfg, size_t i)
{
    return cfg && i < cfg->n ? cfg->e[i].key : NULL;
}

int recomp_cfg_line(const recomp_cfg *cfg, const char *key)
{
    const Entry *e = entry_get(cfg, key);
    return e ? e->line : 0;
}

const char *recomp_cfg_name(const recomp_cfg *cfg) { return cfg ? cfg->name : NULL; }

recomp_cfg_type recomp_cfg_type_of(const recomp_cfg *cfg, const char *key)
{
    const Entry *e = entry_get(cfg, key);
    return e ? e->val.type : RCFG_NONE;
}

static const char *val_string(const Val *v, const char *def) { return v && v->type == RCFG_STRING ? v->v.s : def; }
static long long val_int(const Val *v, long long def) { return v && v->type == RCFG_INT ? v->v.i : def; }
static int val_bool(const Val *v, int def) { return v && v->type == RCFG_BOOL ? v->v.b : def; }
static double val_float(const Val *v, double def)
{
    if (v && v->type == RCFG_FLOAT)
        return v->v.f;
    if (v && v->type == RCFG_INT)
        return (double)v->v.i;
    return def;
}

static const Val *scalar(const recomp_cfg *cfg, const char *key)
{
    const Entry *e = entry_get(cfg, key);
    return e ? &e->val : NULL;
}

const char *recomp_cfg_string(const recomp_cfg *cfg, const char *key, const char *def) { return val_string(scalar(cfg, key), def); }
long long recomp_cfg_int(const recomp_cfg *cfg, const char *key, long long def) { return val_int(scalar(cfg, key), def); }
double recomp_cfg_float(const recomp_cfg *cfg, const char *key, double def) { return val_float(scalar(cfg, key), def); }
int recomp_cfg_bool(const recomp_cfg *cfg, const char *key, int def) { return val_bool(scalar(cfg, key), def); }

int recomp_cfg_choice(const recomp_cfg *cfg, const char *key,
                      const char *const *choices, int def)
{
    const char *s = recomp_cfg_string(cfg, key, NULL);
    int i;
    if (!s || !choices)
        return def;
    for (i = 0; choices[i]; i++)
        if (!strcmp(choices[i], s))
            return i;
    return def;
}

static const Val *item(const recomp_cfg *cfg, const char *key, size_t i)
{
    const Entry *e = entry_get(cfg, key);
    return e && e->val.type == RCFG_ARRAY && i < e->n_items ? &e->items[i] : NULL;
}

size_t recomp_cfg_array_len(const recomp_cfg *cfg, const char *key)
{
    const Entry *e = entry_get(cfg, key);
    return e && e->val.type == RCFG_ARRAY ? e->n_items : 0;
}

recomp_cfg_type recomp_cfg_array_type(const recomp_cfg *cfg, const char *key, size_t i)
{
    const Val *v = item(cfg, key, i);
    return v ? v->type : RCFG_NONE;
}

const char *recomp_cfg_array_string(const recomp_cfg *cfg, const char *key, size_t i, const char *def) { return val_string(item(cfg, key, i), def); }
long long recomp_cfg_array_int(const recomp_cfg *cfg, const char *key, size_t i, long long def) { return val_int(item(cfg, key, i), def); }
double recomp_cfg_array_float(const recomp_cfg *cfg, const char *key, size_t i, double def) { return val_float(item(cfg, key, i), def); }
int recomp_cfg_array_bool(const recomp_cfg *cfg, const char *key, size_t i, int def) { return val_bool(item(cfg, key, i), def); }

/* ── patching a file ───────────────────────────────────────────────── */

/* The menu's Save rewrites only what changed: every other byte (comments,
 * blank lines, key order, the user's spelling) stays as the user wrote it,
 * and the result is parsed again before anything reaches the disk. */

typedef struct {
    size_t at, end;   /* replace text[at, end) (at == end inserts) */
    char *text;
    size_t seq;       /* call order, so inserts at one place keep it */
} Edit;

static int vals_equal(const Val *a, const Val *b)
{
    if (a->type != b->type)
        return 0;
    switch (a->type) {
    case RCFG_STRING: return !strcmp(a->v.s, b->v.s);
    case RCFG_INT:    return a->v.i == b->v.i;
    case RCFG_FLOAT:  return a->v.f == b->v.f || (isnan(a->v.f) && isnan(b->v.f));
    case RCFG_BOOL:   return !a->v.b == !b->v.b;
    default:          return 1;
    }
}

static int entries_equal(const Entry *a, const Entry *b)
{
    size_t i;
    if (!vals_equal(&a->val, &b->val) || a->n_items != b->n_items)
        return 0;
    for (i = 0; i < a->n_items; i++)
        if (!vals_equal(&a->items[i], &b->items[i]))
            return 0;
    return 1;
}

int recomp_cfg_quote(const char *s, char *out, size_t outlen)
{
    Buf b = {0};
    int ok = buf_put(&b, "\"", 1);
    for (; ok && s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        char esc[8];
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            ok = buf_put(&b, esc, 2);
        } else if (c < 0x20 || c == 0x7F) {
            snprintf(esc, sizeof esc, "\\u%04X", c);
            ok = buf_put(&b, esc, 6);
        } else {
            ok = buf_put(&b, (const char *)&c, 1);
        }
    }
    ok = ok && buf_put(&b, "\"", 1);
    if (!ok || b.n + 1 > outlen) {
        free(b.b);
        if (out && outlen)
            out[0] = 0;
        return 0;
    }
    memcpy(out, b.b, b.n + 1);
    free(b.b);
    return 1;
}

static int put_part(Buf *b, const char *s, size_t n)
{
    size_t i;
    char *tmp, *q;
    int ok;
    for (i = 0; i < n && is_bare(s[i]); i++)
        ;
    if (n && i == n)
        return buf_put(b, s, n);
    if (!(tmp = dup_n(s, n)) || !(q = (char *)malloc(n * 6 + 3))) {
        free(tmp);
        return 0;
    }
    ok = recomp_cfg_quote(tmp, q, n * 6 + 3) && buf_put(b, q, strlen(q));
    free(tmp);
    free(q);
    return ok;
}

/* `key` spelled for a line under a table whose prefix is key[0, from): the
 * parts split at dots, except a flat table's name, quoted whole. */
static int put_key(Buf *b, const char *key, size_t from)
{
    size_t flat = flat_prefix(key);
    const char *p = key + from, *end = key + strlen(key);
    const char *last = flat && flat >= from ? key + flat : NULL;
    for (;;) {
        const char *d;
        if (last && p == last)
            return put_part(b, p, (size_t)(end - p));
        d = memchr(p, '.', (size_t)(end - p));
        if (!d)
            d = end;
        if (!put_part(b, p, (size_t)(d - p)))
            return 0;
        if (d == end)
            return 1;
        if (!buf_put(b, ".", 1))
            return 0;
        p = d + 1;
    }
}

/* The table a new `key` belongs in: its prefix length (0 for the root). */
static size_t natural_table(const char *key)
{
    size_t flat = flat_prefix(key);
    const char *d;
    if (flat)
        return flat - 1;
    d = strrchr(key, '.');
    return d ? (size_t)(d - key) : 0;
}

static long header_of(const recomp_cfg *c, const char *key, size_t len)
{
    size_t i;
    for (i = 0; i < c->n_tables; i++)
        if (strlen(c->tables[i]) == len && !strncmp(c->tables[i], key, len))
            return (long)i;
    return -1;
}

/* The source spans of an array's items, from its '[' to past its ']'.
 * Blanks, newlines and comments between items are skipped; strings are
 * scanned to their closing quote, so a '#' or ',' in one is not a break.
 * Returns the item count, or -1 for anything this does not handle (a nested
 * array, a multi-line string): the caller then replaces the whole value. */
#define ARRAY_EDIT_MAX 32

static long array_items(const char *s, size_t at, size_t end, size_t *beg, size_t *fin, size_t max)
{
    size_t p = at + 1;
    long n = 0;
    if (at >= end || s[at] != '[')
        return -1;
    for (;;) {
        while (p < end && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n' || s[p] == '#')) {
            if (s[p] == '#')
                while (p < end && s[p] != '\n')
                    p++;
            else
                p++;
        }
        if (p >= end)
            return -1;
        if (s[p] == ']')
            return n;
        if ((size_t)n == max || s[p] == '[' || s[p] == ',')
            return -1;
        beg[n] = p;
        if (s[p] == '"' || s[p] == '\'') {
            char q = s[p++];
            if (p + 1 < end && s[p] == q && s[p + 1] == q)
                return -1;
            while (p < end && s[p] != q && s[p] != '\n')
                p += (q == '"' && s[p] == '\\' && p + 1 < end) ? 2 : 1;
            if (p >= end || s[p] != q)
                return -1;
            p++;
        } else {
            while (p < end && !strchr(" \t\r\n,]#", s[p]))
                p++;
        }
        fin[n++] = p;
        while (p < end && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n' || s[p] == '#')) {
            if (s[p] == '#')
                while (p < end && s[p] != '\n')
                    p++;
            else
                p++;
        }
        if (p < end && s[p] == ',')
            p++;
        else if (p >= end || s[p] != ']')
            return -1;
    }
}

static int add_edit(Edit **ed, size_t *n, size_t at, size_t end, const char *text, size_t tn)
{
    Edit *ne = (Edit *)realloc(*ed, (*n + 1) * sizeof *ne);
    if (!ne)
        return 0;
    *ed = ne;
    ne[*n].at = at;
    ne[*n].end = end;
    ne[*n].seq = *n;
    if (!(ne[*n].text = dup_n(text, tn)))
        return 0;
    (*n)++;
    return 1;
}

static int edit_cmp(const void *a, const void *b)
{
    const Edit *x = (const Edit *)a, *y = (const Edit *)b;
    if (x->at != y->at)
        return x->at < y->at ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static void perr(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    if (!err || !errlen)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* recomp_cfg_patch_text, with the count of keys left out for a [table]
 * in *nleft. The count is kept apart from `err`: a short or absent buffer
 * holds no message, and a caller must still learn the write was partial. */
static char *patch_text(const char *text, const char *name, const char *const *keys,
                        const char *const *values, size_t n, char *err, size_t errlen,
                        size_t *nleft)
{
    recomp_cfg *orig = NULL, *after = NULL, **want = NULL;
    Edit *ed = NULL;
    size_t ned = 0, i, k, len, expect;
    char *work = NULL, *out = NULL;
    const char *eol;
    unsigned char *del_tab = NULL;   /* per header: 1 when it is removed */
    unsigned char *ins_tab = NULL;   /* per header: 1 when a key goes in */
    unsigned char *skip = NULL;      /* per key: 1 when refused for a table */
    size_t nskip = 0;
    Buf tail = {0}, line = {0}, res = {0};
    char e2[256];
    int ok = 0;

    if (err && errlen)
        err[0] = 0;
    if (nleft)
        *nleft = 0;
    name = name ? name : "<string>";
    text = text ? text : "";
    len = strlen(text);
    /* A last line without its newline gets one, so a key can follow it. */
    if (!(work = (char *)malloc(len + 2)))
        goto oom;
    memcpy(work, text, len + 1);
    if (len && work[len - 1] != '\n') {
        work[len++] = '\n';
        work[len] = 0;
    }
    eol = strstr(work, "\r\n") ? "\r\n" : "\n";
    if (!(orig = recomp_cfg_load_string(work, name, e2, sizeof e2))) {
        perr(err, errlen, "%s (left as it is)", e2);
        goto done;
    }
    want = (recomp_cfg **)calloc(n ? n : 1, sizeof *want);
    del_tab = (unsigned char *)calloc(orig->n_tables + 1, 1);
    ins_tab = (unsigned char *)calloc(orig->n_tables + 1, 1);
    if (!want || !del_tab || !ins_tab)
        goto oom;

    /* Each new value must be one TOML value on its own. */
    for (i = 0; i < n; i++) {
        Buf t = {0};
        if (!keys[i] || !*keys[i] || keys[i][0] == '.' || strstr(keys[i], "..") ||
            keys[i][strlen(keys[i]) - 1] == '.') {
            perr(err, errlen, "%s: bad key '%s'", name, keys[i] ? keys[i] : "");
            goto done;
        }
        for (k = 0; k < i; k++)
            if (!strcmp(keys[k], keys[i])) {
                perr(err, errlen, "%s: key '%s' given twice", name, keys[i]);
                goto done;
            }
        if (!values[i])
            continue;
        if (!buf_put(&t, "v = ", 4) || !buf_put(&t, values[i], strlen(values[i])) ||
            !buf_put(&t, "\n", 1)) {
            free(t.b);
            goto oom;
        }
        want[i] = recomp_cfg_load_string(t.b, keys[i], e2, sizeof e2);
        free(t.b);
        if (!want[i] || want[i]->n != 1) {
            perr(err, errlen, "%s: bad value for %s: %s", name, keys[i],
                 want[i] ? "more than one value" : e2);
            goto done;
        }
    }

    /* A flat name that is also a [header] in the file ([mods.alpha] under
     * "alpha" = false), or the start of a deeper one ([mods.alpha.x]),
     * cannot be written: the header makes it a table, and TOML has no key
     * that is both. Mod settings are flat keys under [mods] ("alpha.speed"
     * = 3), so the user moves the table's keys there. Only that key is
     * left out: a menu Save batches every change, and one legacy table
     * must not lose the rest. */
    for (i = 0; i < n; i++) {
        size_t el;
        const char *sep, *who, *colon;
        if (!values[i] || !flat_prefix(keys[i]))
            continue;
        for (k = 0; k < orig->n_tables && !is_prefix(keys[i], orig->tables[k]); k++)
            ;
        if (k == orig->n_tables)
            continue;
        if (!skip && !(skip = (unsigned char *)calloc(n, 1)))
            goto oom;
        skip[i] = 1;
        nskip++;
        /* The file is named once, so a long config path leaves room
         * for every key in a caller's buffer. */
        el = err && errlen ? strlen(err) : 0;
        sep = nskip > 1 ? "; " : "";
        who = nskip > 1 ? "" : name;
        colon = nskip > 1 ? "" : ": ";
        if (!strcmp(keys[i], orig->tables[k]))
            perr(err ? err + el : NULL, err && errlen ? errlen - el : 0,
                 "%s%s%s%s is the table [%s]; write its keys flat under [%.*s] "
                 "(left as it is)", sep, who, colon, keys[i], orig->tables[k],
                 (int)(flat_prefix(keys[i]) - 1), keys[i]);
        else
            perr(err ? err + el : NULL, err && errlen ? errlen - el : 0,
                 "%s%s%s%s cannot sit beside the table [%s]; write that table's keys "
                 "flat under [%.*s] (left as it is)", sep, who, colon, keys[i],
                 orig->tables[k], (int)(flat_prefix(keys[i]) - 1), keys[i]);
    }
    if (nskip) {
        /* The rest, as a patch of their own: it is checked the same way. */
        const char **fk = (const char **)malloc((n - nskip + 1) * sizeof *fk);
        const char **fv = (const char **)malloc((n - nskip + 1) * sizeof *fv);
        size_t nf = 0;
        if (!fk || !fv) {
            free(fk);
            free(fv);
            goto oom;
        }
        for (i = 0; i < n; i++)
            if (!skip[i]) {
                fk[nf] = keys[i];
                fv[nf++] = values[i];
            }
        out = patch_text(text, name, fk, fv, nf, e2, sizeof e2, NULL);
        free(fk);
        free(fv);
        if (!out) {
            perr(err, errlen, "%s", e2);
            goto done;
        }
        if (nleft)
            *nleft = nskip;
        ok = 1;
        goto done;
    }

    /* Changed and removed keys, where they are. */
    for (i = 0; i < n; i++) {
        long at = recomp_cfg_find(orig, keys[i]);
        const Entry *e;
        if (at < 0)
            continue;
        e = &orig->e[at];
        if (values[i]) {
            /* An array that keeps its item count is edited item by item, so
             * a binding list written one per line keeps its layout and the
             * comments between the items. */
            size_t ob[ARRAY_EDIT_MAX], oe[ARRAY_EDIT_MAX], nb[ARRAY_EDIT_MAX], ne[ARRAY_EDIT_MAX];
            size_t vl = strlen(values[i]);
            long on = array_items(work, e->vbeg, e->vend, ob, oe, ARRAY_EDIT_MAX);
            long nn = array_items(values[i], 0, vl, nb, ne, ARRAY_EDIT_MAX);
            if (on > 0 && on == nn) {
                for (k = 0; k < (size_t)on; k++)
                    if (!add_edit(&ed, &ned, ob[k], oe[k], values[i] + nb[k], ne[k] - nb[k]))
                        goto oom;
            } else if (!add_edit(&ed, &ned, e->vbeg, e->vend, values[i], vl)) {
                goto oom;
            }
        } else if (!add_edit(&ed, &ned, e->lbeg, e->lnext, "", 0)) {
            goto oom;
        }
    }

    /* New keys: under their own [header] when the file has one, at the root
     * when a dotted key there already made their table, else in a new
     * [header] at the end. */
    for (i = 0; i < n; i++) {
        size_t tl, from, at;
        long h;
        if (!values[i] || recomp_cfg_find(orig, keys[i]) >= 0)
            continue;
        tl = natural_table(keys[i]);
        h = tl ? header_of(orig, keys[i], tl) : -1;
        if (tl && h < 0 && list_has(orig->dotted, orig->n_dotted, keys[i], tl)) {
            /* the longest header above it, else the root */
            size_t best = 0;
            for (k = 0; k < orig->n_tables; k++) {
                size_t hl = strlen(orig->tables[k]);
                if (hl < tl && hl > best && is_prefix(orig->tables[k], keys[i])) {
                    best = hl;
                    h = (long)k;
                }
            }
            tl = best;
        } else if (tl && h < 0) {
            continue;   /* a new header, below */
        }
        from = tl ? tl + 1 : 0;
        line.n = 0;
        if (!buf_put(&line, "", 0) || !put_key(&line, keys[i], from) ||
            !buf_put(&line, " = ", 3) || !buf_put(&line, values[i], strlen(values[i])) ||
            !buf_put(&line, eol, strlen(eol)))
            goto oom;
        at = (size_t)-1;
        for (k = 0; k < orig->n; k++)
            if (orig->e[k].tab == h && (at == (size_t)-1 || orig->e[k].lnext > at))
                at = orig->e[k].lnext;
        if (at == (size_t)-1) {
            if (h >= 0) {
                const char *nl = strchr(work + orig->tab_at[h], '\n');
                at = nl ? (size_t)(nl + 1 - work) : len;
            } else if (orig->n_tables) {
                at = orig->tab_at[0];
                if (!buf_put(&line, eol, strlen(eol)))
                    goto oom;
            } else {
                at = len;
            }
        }
        if (h >= 0)
            ins_tab[h] = 1;
        if (!add_edit(&ed, &ned, at, at, line.b, line.n))
            goto oom;
    }
    for (i = 0; i < n; i++) {
        size_t tl;
        if (!values[i] || recomp_cfg_find(orig, keys[i]) >= 0)
            continue;
        tl = natural_table(keys[i]);
        if (!tl || header_of(orig, keys[i], tl) >= 0 ||
            list_has(orig->dotted, orig->n_dotted, keys[i], tl))
            continue;
        /* the first key of this new table writes the header and all of its
         * keys, in the order given */
        for (k = 0; k < i; k++)
            if (values[k] && recomp_cfg_find(orig, keys[k]) < 0 &&
                natural_table(keys[k]) == tl && !strncmp(keys[k], keys[i], tl))
                break;
        if (k < i)
            continue;
        if ((len || tail.n) && !buf_put(&tail, eol, strlen(eol)))
            goto oom;
        {
            char *tname = dup_n(keys[i], tl);
            int hok = tname && buf_put(&tail, "[", 1) && put_key(&tail, tname, 0);
            free(tname);
            if (!hok)
                goto oom;
        }
        if (!buf_put(&tail, "]", 1) || !buf_put(&tail, eol, strlen(eol)))
            goto oom;
        for (k = i; k < n; k++) {
            if (!values[k] || recomp_cfg_find(orig, keys[k]) >= 0 ||
                natural_table(keys[k]) != tl || strncmp(keys[k], keys[i], tl))
                continue;
            if (!put_key(&tail, keys[k], tl + 1) || !buf_put(&tail, " = ", 3) ||
                !buf_put(&tail, values[k], strlen(values[k])) ||
                !buf_put(&tail, eol, strlen(eol)))
                goto oom;
        }
    }

    /* A [header] left with nothing under it goes too, unless it carries a
     * comment: a comment is the user's and is never dropped. */
    for (k = 0; k < orig->n_tables; k++) {
        size_t s = orig->tab_at[k];
        size_t e = k + 1 < orig->n_tables ? orig->tab_at[k + 1] : len;
        size_t p, cnt = 0, gone = 0;
        const char *nl;
        int clean = 1;
        if (ins_tab[k])
            continue;
        for (i = 0; i < orig->n; i++) {
            if (orig->e[i].tab != (long)k)
                continue;
            cnt++;
            for (size_t j = 0; j < n; j++)
                if (!values[j] && !strcmp(keys[j], orig->e[i].key))
                    gone++;
        }
        if (!cnt || gone != cnt)
            continue;
        nl = memchr(work + s, '\n', e - s);
        p = nl ? (size_t)(nl + 1 - work) : e;
        if (memchr(work + s, '#', p - s))
            continue;
        while (p < e && clean) {
            for (i = 0; i < orig->n; i++)
                if (orig->e[i].tab == (long)k && orig->e[i].lbeg == p)
                    break;
            if (i < orig->n) {
                p = orig->e[i].lnext;
                continue;
            }
            if (work[p] != ' ' && work[p] != '\t' && work[p] != '\r' && work[p] != '\n')
                clean = 0;
            p++;
        }
        if (clean) {
            del_tab[k] = 1;
            if (!add_edit(&ed, &ned, s, e, "", 0))
                goto oom;
        }
    }
    /* drop the line removals a removed section already covers */
    for (i = 0; i < ned; i++) {
        for (k = 0; k < orig->n_tables; k++) {
            size_t s = orig->tab_at[k];
            size_t e = k + 1 < orig->n_tables ? orig->tab_at[k + 1] : len;
            if (del_tab[k] && ed[i].at >= s && ed[i].end <= e && !(ed[i].at == s && ed[i].end == e))
                ed[i].at = ed[i].end = (size_t)-1;
        }
    }

    qsort(ed, ned, sizeof *ed, edit_cmp);
    {
        size_t cur = 0;
        if (!buf_put(&res, "", 0))
            goto oom;
        for (i = 0; i < ned; i++) {
            if (ed[i].at == (size_t)-1)
                continue;
            if (ed[i].at < cur) {
                perr(err, errlen, "%s: overlapping edits (left as it is)", name);
                goto done;
            }
            if (!buf_put(&res, work + cur, ed[i].at - cur) ||
                !buf_put(&res, ed[i].text, strlen(ed[i].text)))
                goto oom;
            cur = ed[i].end;
        }
        if (!buf_put(&res, work + cur, len - cur) || (tail.n && !buf_put(&res, tail.b, tail.n)))
            goto oom;
    }

    /* The check: the new text must say exactly the old file plus the patch. */
    if (!(after = recomp_cfg_load_string(res.b, name, e2, sizeof e2))) {
        perr(err, errlen, "the patched file does not parse: %s (left as it is)", e2);
        goto done;
    }
    expect = orig->n;
    for (i = 0; i < n; i++) {
        int had = recomp_cfg_find(orig, keys[i]) >= 0;
        const Entry *got = entry_get(after, keys[i]);
        if (values[i] && !had)
            expect++;
        if (!values[i] && had)
            expect--;
        if (values[i] ? !got || !entries_equal(got, &want[i]->e[0]) : got != NULL) {
            perr(err, errlen, "%s: %s did not come out as asked (left as it is)", name, keys[i]);
            goto done;
        }
    }
    for (i = 0; i < orig->n; i++) {
        const Entry *got;
        for (k = 0; k < n && strcmp(keys[k], orig->e[i].key); k++)
            ;
        if (k < n)
            continue;
        got = entry_get(after, orig->e[i].key);
        if (!got || !entries_equal(got, &orig->e[i])) {
            perr(err, errlen, "%s: %s would change (left as it is)", name, orig->e[i].key);
            goto done;
        }
    }
    if (after->n != expect) {
        perr(err, errlen, "%s: the patch adds keys it was not asked to (left as it is)", name);
        goto done;
    }
    /* Nothing changed: hand back the caller's own text, newline and all. */
    if (!strcmp(res.b, work) && strlen(text) != len)
        out = dup_n(text, strlen(text));
    else
        out = dup_n(res.b, res.n);
    if (!out)
        goto oom;
    ok = 1;
    goto done;
oom:
    perr(err, errlen, "%s: out of memory", name);
done:
    for (i = 0; i < ned; i++)
        free(ed[i].text);
    free(ed);
    if (want)
        for (i = 0; i < n; i++)
            recomp_cfg_free(want[i]);
    free(want);
    free(del_tab);
    free(ins_tab);
    free(skip);
    free(tail.b);
    free(line.b);
    free(res.b);
    free(work);
    recomp_cfg_free(orig);
    recomp_cfg_free(after);
    if (!ok) {
        free(out);
        out = NULL;
    }
    return out;
}

char *recomp_cfg_patch_text(const char *text, const char *name,
                            const char *const *keys, const char *const *values,
                            size_t n, char *err, size_t errlen)
{
    return patch_text(text, name, keys, values, n, err, errlen, NULL);
}

int recomp_cfg_patch(const char *path, const char *const *keys,
                     const char *const *values, size_t n, char *err, size_t errlen)
{
    char *text = NULL, *res, *tmp, *real = NULL;
    size_t plen = strlen(path), left = 0;
    FILE *f;
    int rc = -1, part;

    if (err && errlen)
        err[0] = 0;
    f = fopen(path, "rb");
    if (f) {
        long size;
        size_t got;
        if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
            !(text = (char *)malloc((size_t)size + 1))) {
            fclose(f);
            perr(err, errlen, "%s: cannot read", path);
            return -1;
        }
        got = fread(text, 1, (size_t)size, f);
        fclose(f);
        text[got] = 0;
        if (strlen(text) != got) {
            perr(err, errlen, "%s: NUL byte in file (left as it is)", path);
            free(text);
            return -1;
        }
    } else if (errno != ENOENT) {
        perr(err, errlen, "%s: cannot open: %s", path, strerror(errno));
        return -1;
    }
    res = patch_text(text, path, keys, values, n, err, errlen, &left);
    if (!res) {
        free(text);
        return -1;
    }
    /* Text with err set: keys left out for a [table], the rest written. */
    part = left > INT_MAX ? INT_MAX : (int)left;
    if (text ? !strcmp(res, text) : !*res) {
        free(text);
        free(res);
        return part;   /* nothing to write */
    }
    free(text);
#ifndef _WIN32
    /* A config.toml that is a symlink (a dotfiles repo, a shared config on
     * another drive) is written at its target: renaming over the link
     * would turn it into a plain file and cut it off from the target. */
    {
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
            if (!(real = realpath(path, NULL))) {
                perr(err, errlen, "%s: cannot follow the link: %s", path, strerror(errno));
                free(res);
                return -1;
            }
            path = real;
            plen = strlen(path);
        }
    }
#endif
    /* A whole new file beside the old one, then one rename: a crash leaves
     * the old file or the new one, never half of each. */
    if (!(tmp = (char *)malloc(plen + 5))) {
        perr(err, errlen, "%s: out of memory", path);
        free(res);
        free(real);
        return -1;
    }
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5);
    f = fopen(tmp, "wb");
    if (!f) {
        perr(err, errlen, "%s: cannot write: %s", tmp, strerror(errno));
    } else {
        size_t rn = strlen(res);
        int wok = fwrite(res, 1, rn, f) == rn;
        wok = (fflush(f) == 0) && wok;
        /* The data must be on the disk before the rename is: a delayed
         * allocation filesystem (exFAT on an SD card, ext4 without
         * auto_da_alloc) can otherwise land the rename first and leave an
         * empty config.toml after a power loss right after Save. */
#ifdef _WIN32
        wok = wok && FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(f)));
#else
        wok = wok && fsync(fileno(f)) == 0;
#endif
        wok = (fclose(f) == 0) && wok;
        if (!wok) {
            perr(err, errlen, "%s: write failed", tmp);
            remove(tmp);
        }
#ifdef _WIN32
        else if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            perr(err, errlen, "%s: cannot replace (error %lu)", path, (unsigned long)GetLastError());
            remove(tmp);
        }
#else
        else if (rename(tmp, path) != 0) {
            perr(err, errlen, "%s: cannot replace: %s", path, strerror(errno));
            remove(tmp);
        }
#endif
        else
            rc = 0;
    }
#ifndef _WIN32
    /* ...and the rename itself is only durable once its directory is. A
     * failure here is not reported: the file is already in place. */
    if (rc == 0) {
        const char *slash = strrchr(path, '/');
        char *dir = slash ? dup_n(path, slash == path ? 1 : (size_t)(slash - path)) : NULL;
        int fd = open(dir ? dir : ".", O_RDONLY);
        if (fd >= 0) {
            fsync(fd);
            close(fd);
        }
        free(dir);
    }
#endif
    free(tmp);
    free(real);
    free(res);
    return rc == 0 ? part : rc;
}
