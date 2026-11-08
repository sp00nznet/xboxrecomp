/*
 * saves_sign.c - re-signing imported saves under this build's keys
 * (saves_sign.h; openspec change pad-recording-ui, R6 save safeguard 2).
 *
 * The digest is HMAC-SHA1 under a 16-byte zero key because this build's
 * XboxSignatureKey and XboxHDKey are zero (kernel_keys.c, pinned by the
 * saves_keys ctest): a roamable save the game writes here carries exactly
 * that digest, so a re-signed save loads as one it wrote itself. A console's
 * save carries a digest under that console's keys instead, which is why a
 * keys mismatch needs this. The old digest is never checked: re-signing
 * needs no key from the source console.
 */
#include "saves_sign.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"

/* ---- saves-signing.toml ---- */

static int perr(char *err, size_t errn, int line, const char *what)
{
    if (err && errn && line)
        snprintf(err, errn, "saves-signing.toml line %d: %s", line, what);
    else if (err && errn)
        snprintf(err, errn, "saves-signing.toml: %s", what);
    return -1;
}

static const char *skip_ws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t'))
        p++;
    return p;
}

/* A decimal or 0x integer in [0, 2^32). The bytes after it in *end. */
static int parse_u32(const char *p, const char *e, uint32_t *out, const char **end)
{
    uint64_t v = 0;
    int base = 10, digits = 0;
    if (e - p > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    for (; p < e; p++) {
        int d;
        if (*p == '_' && digits)
            continue;
        if (*p >= '0' && *p <= '9')
            d = *p - '0';
        else if (base == 16 && *p >= 'a' && *p <= 'f')
            d = *p - 'a' + 10;
        else if (base == 16 && *p >= 'A' && *p <= 'F')
            d = *p - 'A' + 10;
        else
            break;
        v = v * (uint64_t)base + (uint64_t)d;
        if (v > 0xFFFFFFFFu)
            return -1;
        digits++;
    }
    if (!digits)
        return -1;
    *out = (uint32_t)v;
    *end = p;
    return 0;
}

/* A basic "..." string with no escapes (the CLI writes none: a glob below
 * UDATA/<tid>/ has no backslash). */
static int parse_str(const char *p, const char *e, char *out, size_t n, const char **end)
{
    size_t k = 0;
    if (p >= e || *p != '"')
        return -1;
    for (p++; p < e && *p != '"'; p++) {
        if (*p == '\\' || (unsigned char)*p < 0x20 || k + 1 >= n)
            return -1;
        out[k++] = *p;
    }
    if (p >= e)
        return -1;
    out[k] = 0;
    *end = p + 1;
    return 0;
}

/* Past blanks, then c; 1 when it was there. */
static int take(const char **p, const char *e, char c)
{
    const char *q = skip_ws(*p, e);
    if (q == e || *q != c)
        return 0;
    *p = q + 1;
    return 1;
}

/* Nothing but a comment may follow a value. */
static int line_done(const char *p, const char *e)
{
    p = skip_ws(p, e);
    return p == e || *p == '#';
}

static void ascii_lower(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s - 'A' + 'a');
}

static int has_dotdot(const char *g)
{
    for (const char *c = g; (c = strstr(c, "..")); c++)
        if ((c == g || c[-1] == '/') && (c[2] == 0 || c[2] == '/'))
            return 1;
    return 0;
}

/* ---- can two globs match one name ---- */

/* One glob token: '*', or the set of bytes one position matches. */
struct gtok {
    unsigned char star;
    uint32_t set[8];
};

/* g's tokens into t (a glob is at most SAVES_SIGN_GLOB_MAX bytes, so as many
 * tokens). Each set is what saves_glob_match itself takes for that token,
 * byte by byte, so the two never disagree on the syntax. */
static int glob_tokens(const char *g, struct gtok *t)
{
    int n = 0;
    while (*g) {
        char tok[SAVES_SIGN_GLOB_MAX + 1], one[2] = { 0, 0 };
        size_t len = 1;
        memset(&t[n], 0, sizeof t[n]);
        if (*g == '*') {
            t[n++].star = 1;
            g++;
            continue;
        }
        if (*g == '[') {
            const char *c = g + 1, *close;
            if (*c == '!')
                c++;
            close = c;
            if (*close == ']')
                close++;
            while (*close && *close != ']')
                close++;
            if (*close == ']')
                len = (size_t)(close - g) + 1;
        }
        memcpy(tok, g, len);
        tok[len] = 0;
        for (int b = 1; b < 256; b++) {
            one[0] = (char)b;
            if (saves_glob_match(tok, one))
                t[n].set[b >> 5] |= 1u << (b & 31);
        }
        n++;
        g += len;
    }
    return n;
}

/* 1 when some name matches both globs: r[i][j] is 1 when one string
 * matches a's tokens from i and b's from j. A '*' takes no byte or one of
 * the other's (a '*' there included); two sets need a byte in common. */
static int globs_meet(const char *a, const char *b)
{
    struct meet {
        struct gtok ta[SAVES_SIGN_GLOB_MAX], tb[SAVES_SIGN_GLOB_MAX];
        unsigned char r[SAVES_SIGN_GLOB_MAX + 1][SAVES_SIGN_GLOB_MAX + 1];
    } *m = malloc(sizeof *m);
    int na, nb, met;
    if (!m)
        return 1;   /* refuse the pair's overlap rather than miss it */
    na = glob_tokens(a, m->ta);
    nb = glob_tokens(b, m->tb);
    for (int i = na; i >= 0; i--)
        for (int j = nb; j >= 0; j--) {
            int v = 0;
            if (i == na && j == nb)
                v = 1;
            else if (i < na && m->ta[i].star)
                v = m->r[i + 1][j] || (j < nb && m->r[i][j + 1]);
            else if (j < nb && m->tb[j].star)
                v = m->r[i][j + 1] || (i < na && m->r[i + 1][j]);
            else if (i < na && j < nb && m->r[i + 1][j + 1])
                for (int w = 0; w < 8 && !v; w++)
                    v = (m->ta[i].set[w] & m->tb[j].set[w]) != 0;
            m->r[i][j] = (unsigned char)v;
        }
    met = m->r[0][0];
    free(m);
    return met;
}

/* The checks manifest.py makes at configure, made again on the file the
 * exe trusts, so a hand-edited file can never write outside a save. */
static int check_recipes(const struct saves_signing *s, char *err, size_t errn)
{
    for (int i = 0; i < s->n; i++) {
        const struct saves_recipe *r = &s->r[i];
        char what[160];
        if (!r->glob[0] || r->glob[0] == '/' || strchr(r->glob, '\\') || has_dotdot(r->glob))
            return perr(err, errn, 0, "a glob must be a pattern below UDATA/<tid>/");
        if (!(r->start < r->end && r->end <= r->size)) {
            snprintf(what, sizeof what, "recipe %d's data range is not inside the file", i + 1);
            return perr(err, errn, 0, what);
        }
        if ((uint64_t)r->digest + SAVES_SIGN_DIGEST > r->size) {
            snprintf(what, sizeof what, "recipe %d's digest is not inside the file", i + 1);
            return perr(err, errn, 0, what);
        }
        for (int j = 0; j < s->n; j++) {
            const struct saves_recipe *o = &s->r[j];
            int inside, over;
            /* Globs are lowered at parse, as the CLI's glob.lower(). */
            if (!strcmp(r->glob, o->glob) && o->size != r->size) {
                snprintf(what, sizeof what, "recipes %d and %d share a glob but not a size",
                         i + 1, j + 1);
                return perr(err, errn, 0, what);
            }
            /* Two different globs can match one file, and every recipe a
             * file matches is applied to it when they share its size (a
             * size apart skips the save). So every pair of one size whose
             * globs can match one name is checked, not only the pairs of
             * one glob (review 1, F5). */
            inside = r->digest < o->end && o->start < r->digest + SAVES_SIGN_DIGEST;
            over = j != i && (r->digest > o->digest ? r->digest - o->digest
                                                    : o->digest - r->digest) < SAVES_SIGN_DIGEST;
            if (o->size != r->size || !(inside || over) ||
                (j != i && strcmp(r->glob, o->glob) && !globs_meet(r->glob, o->glob)))
                continue;
            if (inside) {
                snprintf(what, sizeof what, "recipe %d's digest is inside recipe %d's data",
                         i + 1, j + 1);
                return perr(err, errn, 0, what);
            }
            snprintf(what, sizeof what, "recipes %d and %d's digests overlap", i + 1, j + 1);
            return perr(err, errn, 0, what);
        }
    }
    return 0;
}

enum { HAS_GLOB = 1, HAS_SIZE = 2, HAS_DATA = 4, HAS_DIGEST = 8, HAS_KEY = 16, HAS_ALL = 31 };

int saves_signing_parse(const char *text, size_t len, struct saves_signing *s, char *err,
                        size_t errn)
{
    const char *p = text, *end = text + len;
    int line = 0, in_signed = 0, format = 0;
    int has[SAVES_SIGN_MAX];
    memset(s, 0, sizeof *s);
    if (len > SAVES_SIGN_FILE_MAX)
        return perr(err, errn, 0, "the file is too big");
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p)), *q, *k0, *k1;
        const char *le = eol ? eol : end;
        char key[16];
        size_t kn;
        line++;
        if (le > p && le[-1] == '\r')
            le--;
        q = skip_ws(p, le);
        p = eol ? eol + 1 : end;
        if (q == le || *q == '#')
            continue;
        if (*q == '[') {
            if ((size_t)(le - q) >= 10 && !memcmp(q, "[[signed]]", 10) && line_done(q + 10, le)) {
                if (s->n == SAVES_SIGN_MAX)
                    goto bad_cap;
                if (s->n && has[s->n - 1] != HAS_ALL)
                    goto bad_missing;
                has[s->n] = 0;
                s->n++;
                in_signed = 1;
                continue;
            }
            perr(err, errn, line, "an unknown table");
            goto bad;
        }
        k0 = q;
        while (q < le && ((*q >= 'a' && *q <= 'z') || *q == '_'))
            q++;
        k1 = q;
        kn = (size_t)(k1 - k0);
        q = skip_ws(q, le);
        if (!kn || kn >= sizeof key || q == le || *q != '=') {
            perr(err, errn, line, "not a key = value line");
            goto bad;
        }
        memcpy(key, k0, kn);
        key[kn] = 0;
        q = skip_ws(q + 1, le);
        if (!in_signed) {
            if (!strcmp(key, "format")) {
                uint32_t v;
                if (parse_u32(q, le, &v, &q) || !line_done(q, le)) {
                    perr(err, errn, line, "format is not a number");
                    goto bad;
                }
                format = (int)(v > 1000 ? 1000 : v);
            } else if (!strcmp(key, "signing")) {
                char v[16];
                if (parse_str(q, le, v, sizeof v, &q) || !line_done(q, le)) {
                    perr(err, errn, line, "signing is not a string");
                    goto bad;
                }
                if (!strcmp(v, "resign"))
                    s->mode = SAVES_SIGNING_RESIGN;
                else if (!strcmp(v, "none"))
                    s->mode = SAVES_SIGNING_NONE;
                else {
                    perr(err, errn, line, "signing is not \"resign\" or \"none\"");
                    goto bad;
                }
            }
            /* any other top-level key: a later format's, ignored */
            continue;
        } else {
            struct saves_recipe *r = &s->r[s->n - 1];
            int *h = &has[s->n - 1], bit;
            if (!strcmp(key, "glob")) {
                bit = HAS_GLOB;
                if (parse_str(q, le, r->glob, sizeof r->glob, &q)) {
                    perr(err, errn, line, "glob is not a string of at most 128 bytes");
                    goto bad;
                }
                /* ASCII only: the match folds case in ASCII, and the
                 * CLI's .lower() folds Unicode, so a non-ASCII glob could
                 * match in one and not the other (review 1, F7). */
                for (const char *c = r->glob; *c; c++)
                    if ((unsigned char)*c >= 0x80) {
                        perr(err, errn, line, "glob is not ASCII");
                        goto bad;
                    }
                ascii_lower(r->glob);
            } else if (!strcmp(key, "size")) {
                bit = HAS_SIZE;
                if (parse_u32(q, le, &r->size, &q) || !r->size) {
                    perr(err, errn, line, "size is not a positive number");
                    goto bad;
                }
            } else if (!strcmp(key, "digest")) {
                bit = HAS_DIGEST;
                if (parse_u32(q, le, &r->digest, &q)) {
                    perr(err, errn, line, "digest is not a number");
                    goto bad;
                }
            } else if (!strcmp(key, "data")) {
                bit = HAS_DATA;
                if (!take(&q, le, '[') || parse_u32(skip_ws(q, le), le, &r->start, &q)
                    || !take(&q, le, ',') || parse_u32(skip_ws(q, le), le, &r->end, &q)
                    || !take(&q, le, ']')) {
                    perr(err, errn, line, "data is not [start, end]");
                    goto bad;
                }
            } else if (!strcmp(key, "key")) {
                char v[24];
                bit = HAS_KEY;
                if (parse_str(q, le, v, sizeof v, &q)) {
                    perr(err, errn, line, "key is not a string");
                    goto bad;
                }
                /* only "roamable" exists in v1; any other key is a
                 * console-locked save this build cannot sign */
                r->roamable = !strcmp(v, "roamable");
            } else {
                perr(err, errn, line, "an unknown key in [[signed]]");
                goto bad;
            }
            if (!line_done(q, le)) {
                perr(err, errn, line, "text after the value");
                goto bad;
            }
            if (*h & bit) {
                perr(err, errn, line, "a key given twice");
                goto bad;
            }
            *h |= bit;
        }
    }
    if (s->n && has[s->n - 1] != HAS_ALL)
        goto bad_missing;
    if (format != SAVES_SIGN_FORMAT) {
        perr(err, errn, 0, format > SAVES_SIGN_FORMAT ? "from a newer build" : "no format = 1");
        goto bad;
    }
    if (s->mode == SAVES_SIGNING_RESIGN && !s->n) {
        perr(err, errn, 0, "signing = \"resign\" with no [[signed]]");
        goto bad;
    }
    if (s->mode != SAVES_SIGNING_RESIGN && s->n) {
        perr(err, errn, 0, "[[signed]] without signing = \"resign\"");
        goto bad;
    }
    if (check_recipes(s, err, errn))
        goto bad;
    return 0;
bad_cap:
    perr(err, errn, line, "more than 64 [[signed]] entries");
    goto bad;
bad_missing:
    perr(err, errn, line, "a [[signed]] entry lacks glob, size, data, digest or key");
bad:
    memset(s, 0, sizeof *s);
    return -1;
}

/* ---- globs ---- */

int saves_glob_match(const char *g, const char *n)
{
    const char *star_g = NULL, *star_n = NULL;
    while (*n) {
        if (*g == '*') {
            while (*g == '*')
                g++;
            if (!*g)
                return 1;
            star_g = g;
            star_n = n;
            continue;
        }
        if (*g == '?') {
            g++;
            n++;
            continue;
        }
        if (*g == '[') {
            /* fnmatch's set: [!...] negates, a ']' right after the
             * opening (or the '!') is a member, an unclosed '[' is a
             * literal '[' */
            const char *c = g + 1, *close;
            int neg = 0, hit = 0;
            if (*c == '!') {
                neg = 1;
                c++;
            }
            close = c;
            if (*close == ']')
                close++;
            while (*close && *close != ']')
                close++;
            if (*close == ']') {
                for (const char *m = c; m < close; m++) {
                    if (m + 2 < close && m[1] == '-') {
                        if ((unsigned char)*n >= (unsigned char)m[0]
                            && (unsigned char)*n <= (unsigned char)m[2])
                            hit = 1;
                        m += 2;
                    } else if (*m == *n) {
                        hit = 1;
                    }
                }
                if (hit != neg) {
                    g = close + 1;
                    n++;
                    continue;
                }
                goto backtrack;
            }
        }
        if (*g && *g == *n) {
            g++;
            n++;
            continue;
        }
backtrack:
        if (!star_g)
            return 0;
        g = star_g;
        n = ++star_n;
    }
    while (*g == '*')
        g++;
    return !*g;
}

int saves_recipes_for(const struct saves_signing *s, const char *rel, int *idx, int max)
{
    char low[SZ_PATH_MAX];
    int k = 0;
    snprintf(low, sizeof low, "%s", rel);
    ascii_lower(low);
    for (int i = 0; i < s->n && k < max; i++)
        if (saves_glob_match(s->r[i].glob, low))
            idx[k++] = i;
    return k;
}

int saves_resign(unsigned char *buf, size_t len, const struct saves_signing *s, const int *idx,
                 int n)
{
    static const unsigned char zero_key[16];
    for (int i = 0; i < n; i++)
        if (s->r[idx[i]].size != len)
            return -1;
    for (int i = 0; i < n; i++) {
        const struct saves_recipe *r = &s->r[idx[i]];
        unsigned char d[SAVES_SIGN_DIGEST];
        xbox_XcHMAC(zero_key, sizeof zero_key, buf + r->start, r->end - r->start, NULL, 0, d);
        memcpy(buf + r->digest, d, sizeof d);
    }
    return 0;
}

/* ---- a saves zip under a keys mismatch ---- */

/* The save folder an entry belongs to ("" for none: the title folder's own
 * files), lower-cased into sid, and the rest after the title folder. */
static const char *save_of(const struct sz_entry *e, uint32_t tid, char *sid, size_t n,
                           int *tree)
{
    const char *rest = saves_zip_rest(e->name, tid, tree), *slash;
    size_t k;
    sid[0] = 0;
    if (!rest || *tree != SAVES_TREE_UDATA)
        return rest;
    slash = strchr(rest, '/');
    if (!slash)
        return rest; /* a file of the title folder itself, or the folder */
    k = (size_t)(slash - rest);
    if (k >= n)
        k = n - 1;
    memcpy(sid, rest, k);
    sid[k] = 0;
    return rest;
}

static int sid_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

struct save_slot {
    char sid[SZ_NAME_MAX + 1];
    int verdict;
    char why[96];
    int matched;
};

static int slot_of(struct save_slot *sl, int n, const char *sid)
{
    for (int i = 0; i < n; i++)
        if (sid_eq(sl[i].sid, sid))
            return i;
    return -1;
}

static void skip(struct save_slot *s, const char *why)
{
    if (s->verdict == SAVES_SKIPPED)
        return;
    s->verdict = SAVES_SKIPPED;
    snprintf(s->why, sizeof s->why, "%s", why);
}

int saves_resign_archive(const struct sz_archive *in, uint32_t tid, int trees,
                         const struct saves_signing *s, struct sz_archive *out,
                         unsigned char **blob, int *out_trees, struct saves_resign_report *rep,
                         char *err, size_t errn)
{
    struct save_slot *sl = NULL;
    int ns = 0, left = 0;
    size_t blob_len = 0, at = 0;
    char sid[SZ_NAME_MAX + 1];
    int idx[SAVES_SIGN_MAX];

    memset(out, 0, sizeof *out);
    memset(rep, 0, sizeof *rep);
    *blob = NULL;
    *out_trees = trees;
    if (in->n > 0 && !(sl = calloc((size_t)in->n, sizeof *sl)))
        goto oom;

    /* the saves, and each one's verdict */
    for (int i = 0; i < in->n; i++) {
        int tree = 0, k;
        const char *rest = save_of(&in->e[i], tid, sid, sizeof sid, &tree);
        if (!rest || !sid[0])
            continue;
        if ((k = slot_of(sl, ns, sid)) < 0) {
            k = ns++;
            snprintf(sl[k].sid, sizeof sl[k].sid, "%s", sid);
            sl[k].verdict = s->mode == SAVES_SIGNING_NONE     ? SAVES_KEPT
                            : s->mode == SAVES_SIGNING_RESIGN ? SAVES_RESIGNED
                                                              : SAVES_SKIPPED;
            if (s->mode == SAVES_SIGNING_UNKNOWN)
                snprintf(sl[k].why, sizeof sl[k].why,
                         "this build has no saves-signing.toml that says how");
        }
        if (s->mode != SAVES_SIGNING_RESIGN || in->e[i].is_dir)
            continue;
        int m = saves_recipes_for(s, rest, idx, SAVES_SIGN_MAX);
        if (!m)
            continue;
        sl[k].matched = 1;
        for (int j = 0; j < m; j++) {
            const struct saves_recipe *r = &s->r[idx[j]];
            char why[96];
            if (!r->roamable) {
                skip(&sl[k], "its recipe is console-locked");
            } else if (r->size != in->e[i].size) {
                snprintf(why, sizeof why, "%.40s is %u bytes, not %u", rest + strlen(sid) + 1,
                         (unsigned)in->e[i].size, (unsigned)r->size);
                skip(&sl[k], why);
            }
        }
        if (sl[k].verdict == SAVES_RESIGNED)
            blob_len += in->e[i].size;
    }
    for (int k = 0; k < ns; k++) {
        if (sl[k].verdict == SAVES_RESIGNED && !sl[k].matched)
            skip(&sl[k], "no recipe matches its files");
        if (sl[k].verdict != SAVES_SKIPPED)
            left++;
    }
    if (!left)
        *out_trees &= ~SAVES_TREE_UDATA;
    if (!*out_trees) {
        if (err && errn)
            snprintf(err, errn, "None of these saves can be re-signed for this build");
        goto fill_report;
    }

    /* the new archive */
    if (!(out->e = calloc((size_t)in->n, sizeof *out->e)) || (blob_len && !(*blob = malloc(blob_len))))
        goto oom;
    for (int i = 0; i < in->n; i++) {
        const struct sz_entry *e = &in->e[i];
        int tree = 0, k = -1;
        const char *rest = save_of(e, tid, sid, sizeof sid, &tree);
        if (rest && tree == SAVES_TREE_UDATA && !(*out_trees & SAVES_TREE_UDATA))
            continue;
        if (rest && sid[0]) {
            k = slot_of(sl, ns, sid);
            if (sl[k].verdict == SAVES_SKIPPED)
                continue;
        }
        out->e[out->n] = *e;
        if (k >= 0 && sl[k].verdict == SAVES_RESIGNED && !e->is_dir) {
            int m = saves_recipes_for(s, rest, idx, SAVES_SIGN_MAX);
            if (m) {
                unsigned char *b = *blob + at;
                memcpy(b, e->data, e->size);
                if (saves_resign(b, e->size, s, idx, m))
                    goto oom; /* sizes were checked above; never */
                out->e[out->n].data = b;
                out->e[out->n].crc = sz_crc32(0, b, e->size);
                at += e->size;
            }
        }
        out->n++;
    }

fill_report:
    rep->n = ns;
    for (int k = 0; k < ns; k++) {
        if (sl[k].verdict == SAVES_RESIGNED)
            rep->resigned++;
        else if (sl[k].verdict == SAVES_SKIPPED)
            rep->skipped++;
        else
            rep->kept++;
        if (k < SAVES_RESIGN_MAX_SAVES) {
            struct saves_resign_save *o = &rep->s[k];
            snprintf(o->sid, sizeof o->sid, "%s", sl[k].sid);
            o->verdict = sl[k].verdict;
            snprintf(o->why, sizeof o->why, "%s", sl[k].why);
        }
    }
    free(sl);
    if (!*out_trees)
        return -1;
    return 0;
oom:
    free(sl);
    free(out->e);
    free(*blob);
    memset(out, 0, sizeof *out);
    *blob = NULL;
    if (err && errn)
        snprintf(err, errn, "Out of memory while re-signing the saves");
    return -1;
}

size_t saves_resign_lines(const struct saves_resign_report *rep, int with_why, char *out,
                          size_t n)
{
    size_t k = 0;
    int shown = rep->n < SAVES_RESIGN_MAX_SAVES ? rep->n : SAVES_RESIGN_MAX_SAVES;
    /* k stops at the buffer's end (out + k never points past it, review 1,
     * F6); a cut line is dropped whole. */
#define PUT(...)                                                                         \
    do {                                                                                 \
        if (k < n) {                                                                     \
            int w_ = snprintf(out + k, n - k, __VA_ARGS__);                             \
            if (w_ > 0 && (size_t)w_ < n - k)                                            \
                k += (size_t)w_;                                                         \
            else {                                                                       \
                out[k] = 0;                                                              \
                k = n;                                                                   \
            }                                                                            \
        }                                                                                \
    } while (0)
    if (n)
        out[0] = 0;
    for (int i = 0; i < shown; i++) {
        const struct saves_resign_save *s = &rep->s[i];
        if (s->verdict == SAVES_RESIGNED)
            PUT("%s: %s [%s]\n", s->sid, SAVES_SAID_RESIGNED, SAVES_SAID_MARK);
        else if (s->verdict == SAVES_SKIPPED && with_why && s->why[0])
            PUT("%s: %s (%s)\n", s->sid, SAVES_SAID_SKIPPED, s->why);
        else if (s->verdict == SAVES_SKIPPED)
            PUT("%s: %s\n", s->sid, SAVES_SAID_SKIPPED);
    }
    if (rep->n > shown)
        PUT("and %d more\n", rep->n - shown);
    if (rep->skipped)
        PUT("%s\n", SAVES_SAID_WITHOUT);
#undef PUT
    return n ? strlen(out) : 0;
}
