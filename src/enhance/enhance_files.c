/*
 * enhance_files.c - the menu's import and export texts (enhance_files.h).
 */
#include "enhance_files.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARKER "# xbr-export:"

/* One line of text from p: its length without the end of line, and *next
 * past it. */
static size_t line_at(const char *p, const char **next)
{
    size_t n = strcspn(p, "\n");
    *next = p[n] ? p + n + 1 : p + n;
    if (n && p[n - 1] == '\r')
        n--;
    return n;
}

int ef_marker(const char *text, char *kind, size_t kind_n, int *format)
{
    const char *p = text ? text : "", *next;
    /* A UTF-8 byte order mark is no line of its own. */
    if (!strncmp(p, "\xEF\xBB\xBF", 3))
        p += 3;
    for (; *p; p = next) {
        size_t n = line_at(p, &next), i = 0, k = 0;
        while (i < n && (p[i] == ' ' || p[i] == '\t'))
            i++;
        if (i == n)
            continue;
        if (p[i] != '#')
            return 0;
        if (n - i < sizeof MARKER - 1 || strncmp(p + i, MARKER, sizeof MARKER - 1))
            continue;
        i += sizeof MARKER - 1;
        while (i < n && p[i] == ' ')
            i++;
        while (i < n && p[i] != ' ' && k + 1 < kind_n)
            kind[k++] = p[i++];
        if (kind_n)
            kind[k] = 0;
        while (i < n && p[i] == ' ')
            i++;
        *format = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9' && *format < 100000)
            *format = *format * 10 + (p[i++] - '0');
        return 1;
    }
    return 0;
}

static void set_err(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
}

char *ef_export_config(const char *user_text, const char *const *keys,
                       const char *const *values, size_t n, const char *const *unsaved,
                       size_t nunsaved, char *err, size_t errlen)
{
    const char *body = user_text ? user_text : "", *next;
    const char *eol = strstr(body, "\r\n") ? "\r\n" : "\n";
    char *patched, *out;
    size_t need, len = 0;

    /* The marker and its lines from an earlier export: this one writes its
     * own. */
    while (!strncmp(body, MARKER, sizeof MARKER - 1)) {
        line_at(body, &next);
        body = next;
    }
    if (err && errlen)
        err[0] = 0;
    patched = recomp_cfg_patch_text(body, "export", keys, values, n, err, errlen);
    if (!patched)
        return NULL;
    need = strlen(patched) + 128;
    for (size_t i = 0; i < nunsaved; i++)
        need += strlen(unsaved[i]) + 2;
    out = malloc(need);
    if (!out) {
        free(patched);
        set_err(err, errlen, "out of memory");
        return NULL;
    }
    len += (size_t)snprintf(out + len, need - len, MARKER " config %d%s", EF_CONFIG_FORMAT, eol);
    if (nunsaved) {
        len += (size_t)snprintf(out + len, need - len, MARKER " %zu unsaved value%s included: ",
                                nunsaved, nunsaved == 1 ? "" : "s");
        for (size_t i = 0; i < nunsaved; i++)
            len += (size_t)snprintf(out + len, need - len, "%s%s", i ? ", " : "", unsaved[i]);
        len += (size_t)snprintf(out + len, need - len, "%s", eol);
    }
    snprintf(out + len, need - len, "%s", patched);
    free(patched);
    return out;
}

/* The shortest text that reads back as v, with a point or an exponent. */
static int float_text(double v, char *out, size_t n)
{
    char t[40];
    if (isnan(v))
        return snprintf(out, n, "nan") < (int)n;
    if (isinf(v))
        return snprintf(out, n, "%s", v < 0 ? "-inf" : "inf") < (int)n;
    for (int p = 1; p <= 17; p++) {
        snprintf(t, sizeof t, "%.*g", p, v);
        if (strtod(t, NULL) == v)
            break;
    }
    if (!strpbrk(t, ".eEn"))
        strcat(t, ".0");
    return snprintf(out, n, "%s", t) < (int)n;
}

static int scalar_text(int type, const char *s, long long i, double f, int b, char *out,
                       size_t n)
{
    switch (type) {
    case RCFG_STRING:
        return recomp_cfg_quote(s, out, n);
    case RCFG_INT:
        return snprintf(out, n, "%lld", i) < (int)n;
    case RCFG_FLOAT:
        return float_text(f, out, n);
    case RCFG_BOOL:
        return snprintf(out, n, "%s", b ? "true" : "false") < (int)n;
    default:
        return 0;
    }
}

int ef_literal(const recomp_cfg *cfg, const char *key, char *out, size_t n)
{
    int type = recomp_cfg_type_of(cfg, key);
    size_t len, cnt;
    if (!n)
        return 0;
    out[0] = 0;
    if (type != RCFG_ARRAY)
        return scalar_text(type, recomp_cfg_string(cfg, key, ""), recomp_cfg_int(cfg, key, 0),
                           recomp_cfg_float(cfg, key, 0), recomp_cfg_bool(cfg, key, 0), out, n);
    cnt = recomp_cfg_array_len(cfg, key);
    if (n < 3)
        return 0;
    out[0] = '[';
    len = 1;
    for (size_t i = 0; i < cnt; i++) {
        char item[512];
        if (!scalar_text(recomp_cfg_array_type(cfg, key, i),
                         recomp_cfg_array_string(cfg, key, i, ""),
                         recomp_cfg_array_int(cfg, key, i, 0),
                         recomp_cfg_array_float(cfg, key, i, 0),
                         recomp_cfg_array_bool(cfg, key, i, 0), item, sizeof item))
            return 0;
        if (len + strlen(item) + 3 > n)
            return 0;
        len += (size_t)snprintf(out + len, n - len, "%s%s", i ? ", " : "", item);
    }
    if (len + 2 > n)
        return 0;
    out[len++] = ']';
    out[len] = 0;
    return 1;
}
