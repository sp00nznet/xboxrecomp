/**
 * RECOMP_DEBUG=vsh_const_dump: the vertex-program constants of every program
 * batch in a window of flips, one record per draw.
 *
 * vsh_dump writes a program's constants once, the first time the program is
 * seen. A title that animates through the constants (skinning bone matrices,
 * a camera, a time uniform) needs them at draw time, flip after flip, to see
 * which ones go wrong and when. The pushbuffer executor tracks the transform
 * context for every backend, so the records are the same on CPU, Metal and
 * D3D11.
 *
 * The file is a sequence of records in host byte order, nothing else:
 *
 *     char     magic[4] = "VCD1"
 *     uint32_t flip     s_gpu.flips at the draw
 *     uint32_t draw     the draw's number, over the whole run
 *     uint32_t hash     nv2a_vsh_program_hash: the [VSH] program hashes
 *     uint32_t start    SET_TRANSFORM_PROGRAM_START
 *     uint32_t len      the program's length in slots
 *     float    c[192][4]
 *
 * This is free of the executor's state so tests/nv2a_vsh_cdump can run it.
 */
#ifndef XBOXRECOMP_NV2A_VSH_CDUMP_H
#define XBOXRECOMP_NV2A_VSH_CDUMP_H

#include <stdint.h>
#include <stdio.h>

#include "nv2a_vsh_cpu.h"

#define NV2A_VSH_CDUMP_RECORD (24u + NV2A_VSH_CONSTANTS * 16u)
#define NV2A_VSH_CDUMP_PROGS  16

struct nv2a_vsh_cdump_cfg {
    uint32_t first, last, every;            /* flips first..last, every nth */
    uint32_t prog[NV2A_VSH_CDUMP_PROGS];    /* only these hashes, if nprog */
    int nprog;
};

/* vsh_const_flips=a-b[/n] (or a, a-, -b; NULL is every flip) and
 * vsh_const_prog=hash[,hash] (hex, an optional 0x; NULL is every program).
 * Returns 0 and fills cfg, or -1 for a malformed value. */
int nv2a_vsh_cdump_parse(const char *flips, const char *progs,
                         struct nv2a_vsh_cdump_cfg *cfg);

/* Whether a draw at this flip running this program is dumped. */
int nv2a_vsh_cdump_wanted(const struct nv2a_vsh_cdump_cfg *cfg,
                          uint32_t flip, uint32_t hash);

/* Append one record. Returns 0, or -1 if the write failed. */
int nv2a_vsh_cdump_write(FILE *f, uint32_t flip, uint32_t draw, uint32_t hash,
                         uint32_t start, uint32_t len,
                         const float c[NV2A_VSH_CONSTANTS][4]);

#endif
