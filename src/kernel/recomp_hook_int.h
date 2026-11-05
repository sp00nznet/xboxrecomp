/*
 * recomp_hook_int.h - the hook chains as recomp_hook.c builds them and
 * recomp_hook_run.c walks them. Not for the generated code or the mods.
 */
#ifndef RECOMP_HOOK_INT_H
#define RECOMP_HOOK_INT_H

#include "recomp_hook_mod.h"

/* Hooks per VA: a handful of mods at most ever share one function. */
#define HOOK_PER_VA 8

struct hook_ent {
    const void *owner;
    const char *name;
    uint32_t va;
    xbox_mod_hook_fn pre, post;
    uint32_t flags;
    void *user;
    int warned; /* a skip without MAY_SKIP was logged */
};

struct recomp_hook_chain {
    int n;
    struct hook_ent *e[HOOK_PER_VA];
};

#endif /* RECOMP_HOOK_INT_H */
