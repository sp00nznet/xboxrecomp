/* RECOMP_DEBUG=vsh_const_dump: the parser and record writer (see the header). */
#include "nv2a_vsh_cdump.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* strtoull, not strtoul: unsigned long is 32 bits on Windows, where the range
 * check never fires and "-3" wraps to 4294967293. The leading-digit test stops
 * strtoull taking a sign or leading blanks. */
static int parse_u32(const char **p, uint32_t *out)
{
    char *end;
    unsigned long long v;
    if (!isdigit((unsigned char)**p))
        return -1;
    v = strtoull(*p, &end, 10);
    if (end == *p || v > 0xFFFFFFFFull)
        return -1;
    *out = (uint32_t)v;
    *p = end;
    return 0;
}

int nv2a_vsh_cdump_parse(const char *flips, const char *progs,
                         struct nv2a_vsh_cdump_cfg *cfg)
{
    const char *p;

    cfg->first = 0;
    cfg->last = 0xFFFFFFFFu;
    cfg->every = 1;
    cfg->nprog = 0;

    if (flips && *flips) {
        p = flips;
        if (*p != '-' && parse_u32(&p, &cfg->first))
            return -1;
        if (*p == '-') {
            p++;
            if (*p && *p != '/' && parse_u32(&p, &cfg->last))
                return -1;
        } else if (*p == '\0' || *p == '/') {
            cfg->last = cfg->first;     /* a single flip */
        }
        if (*p == '/') {
            p++;
            if (parse_u32(&p, &cfg->every) || cfg->every == 0)
                return -1;
        }
        if (*p || cfg->last < cfg->first)
            return -1;
    }

    if (progs && *progs) {
        p = progs;
        for (;;) {
            char *end;
            unsigned long long v;
            if (cfg->nprog >= NV2A_VSH_CDUMP_PROGS
                    || !isxdigit((unsigned char)*p))
                return -1;
            v = strtoull(p, &end, 16);  /* takes an optional 0x itself */
            if (end == p || v > 0xFFFFFFFFull)
                return -1;
            cfg->prog[cfg->nprog++] = (uint32_t)v;
            p = end;
            if (*p == '\0')
                break;
            if (*p != ',')
                return -1;
            p++;
        }
    }
    return 0;
}

int nv2a_vsh_cdump_wanted(const struct nv2a_vsh_cdump_cfg *cfg,
                          uint32_t flip, uint32_t hash)
{
    int i;

    if (flip < cfg->first || flip > cfg->last
            || (flip - cfg->first) % cfg->every)
        return 0;
    if (!cfg->nprog)
        return 1;
    for (i = 0; i < cfg->nprog; i++)
        if (cfg->prog[i] == hash)
            return 1;
    return 0;
}

int nv2a_vsh_cdump_write(FILE *f, uint32_t flip, uint32_t draw, uint32_t hash,
                         uint32_t start, uint32_t len,
                         const float c[NV2A_VSH_CONSTANTS][4])
{
    uint32_t head[5];

    head[0] = flip;
    head[1] = draw;
    head[2] = hash;
    head[3] = start;
    head[4] = len;
    if (fwrite("VCD1", 4, 1, f) != 1
            || fwrite(head, sizeof head, 1, f) != 1
            || fwrite(c, sizeof(float) * 4, NV2A_VSH_CONSTANTS, f)
                   != NV2A_VSH_CONSTANTS)
        return -1;
    return 0;
}
