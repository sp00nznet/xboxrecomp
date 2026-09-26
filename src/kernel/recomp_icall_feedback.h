/*
 * Indirect-branch target feedback - shared constants.
 *
 * Included by both src/kernel/icall_feedback.c (which defines the array) and
 * templates/runtime/recomp_types.h (which emits the store into the ICALL
 * macros). Kept separate from recomp_types.h so the generated-code header stays
 * a template and this stays a runtime detail.
 *
 * See src/kernel/icall_feedback.c for the rationale and
 * docs/technical/ms-fusion-codegen-teardown.md for where the idea comes from.
 */

#ifndef RECOMP_ICALL_FEEDBACK_H
#define RECOMP_ICALL_FEEDBACK_H

#include <stdint.h>

/* Window covered by the observation array.
 *
 * CUSTOMIZE: must span every executable section of your XBE. The default covers
 * the whole image for a 7.7 MiB XBE (tools/disasm/config.py XBE_BASE_ADDRESS /
 * XBE_IMAGE_SIZE). Targets outside the window are ignored rather than clamped --
 * a wrong bucket is worse than a missing one, because it would seed function
 * detection at an address the title never actually branched to. */
#define RECOMP_ICALL_FB_BASE 0x00010000u
#define RECOMP_ICALL_FB_SIZE 0x00800000u  /* 8 MiB */

/* Flags OR'd into g_icall_seen[va - base]. */
#define RECOMP_ICALL_SEEN_RESOLVED   1u  /* dispatch found a translation */
#define RECOMP_ICALL_SEEN_UNRESOLVED 2u  /* dispatch failed: a real gap */

#ifdef RECOMP_ICALL_FEEDBACK

extern volatile unsigned char g_icall_seen[RECOMP_ICALL_FB_SIZE];

/**
 * Record an observed indirect-branch target.
 *
 * Inlined into the ICALL macros rather than being a function call, because it
 * sits on the hottest path in the generated code: one subtract, one compare,
 * one OR.
 */
#define RECOMP_ICALL_OBSERVE(va, flags) do { \
    uint32_t _obs_off = (uint32_t)(va) - RECOMP_ICALL_FB_BASE; \
    if (_obs_off < RECOMP_ICALL_FB_SIZE) \
        g_icall_seen[_obs_off] |= (unsigned char)(flags); \
} while (0)

/* Per-site record: which targets each `call` site has reached. The inline
 * part is a 64K-entry (site, last target) cache keyed by a multiplicative
 * hash of the site, so a site that keeps calling the same target costs two
 * loads and a compare; the out-of-line insert runs only when the pair
 * changes. Collisions between sites only cost extra inserts, never wrong
 * records -- the table behind the call keys on the exact site. Races between
 * threads can lose or duplicate an insert; this is an instrument, and the
 * merge tool unions the results. */
extern uint32_t g_icall_site_hs[65536];
extern uint32_t g_icall_site_hl[65536];
void recomp_icall_observe_site(uint32_t site, uint32_t va);
#define RECOMP_ICALL_OBSERVE_SITE(site, va) do { \
    uint32_t _os = (uint32_t)(site), _ot = (uint32_t)(va); \
    uint32_t _oh = (_os * 2654435761u) >> 16; \
    if (g_icall_site_hs[_oh] != _os || g_icall_site_hl[_oh] != _ot) { \
        g_icall_site_hs[_oh] = _os; g_icall_site_hl[_oh] = _ot; \
        recomp_icall_observe_site(_os, _ot); \
    } \
} while (0)

/**
 * Write the observed target set to a text file.
 * Merge it into the persisted database with tools/recomp/icall_feedback.py.
 * Safe to call more than once; call it from atexit() and from your crash
 * handler, since a title that dies mid-boot is exactly the one whose targets
 * you want.
 */
void recomp_icall_feedback_dump(const char *path);

/** Register an atexit() dump to RECOMP_ICALL_FEEDBACK_PATH. */
void recomp_icall_feedback_init(void);

/* Default dump location, relative to the working directory. CUSTOMIZE if your
 * launcher runs from somewhere you would rather not write to. */
#ifndef RECOMP_ICALL_FEEDBACK_PATH
#define RECOMP_ICALL_FEEDBACK_PATH "icall_targets.dump"
#endif

/* Call these from the host program. They are macros so a host can call them
 * unconditionally without #ifdef -- both compile away when the feature is off,
 * which is the point: instrumentation the host has to bracket in #ifdef is
 * instrumentation that rots. */
#define RECOMP_ICALL_FEEDBACK_INIT() recomp_icall_feedback_init()
#define RECOMP_ICALL_FEEDBACK_DUMP() \
    recomp_icall_feedback_dump(RECOMP_ICALL_FEEDBACK_PATH)

#else  /* !RECOMP_ICALL_FEEDBACK */

#define RECOMP_ICALL_OBSERVE(va, flags) ((void)0)
#define RECOMP_ICALL_OBSERVE_SITE(site, va) ((void)0)
#define RECOMP_ICALL_FEEDBACK_INIT()   ((void)0)
#define RECOMP_ICALL_FEEDBACK_DUMP()   ((void)0)

#endif

#endif /* RECOMP_ICALL_FEEDBACK_H */
