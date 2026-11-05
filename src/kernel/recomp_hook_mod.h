/**
 * recomp_hook_mod.h - the hook slots (recomp_hook.h) as the mods and the
 * runtime see them: what a hook callback is handed, and the calls mod.c
 * makes to queue, publish and clear the hooks.
 *
 * Kept apart from recomp_hook.h because the generated code defines its
 * register names as macros (eax, ecx, edx and esp become g_eax ... under
 * RECOMP_GENERATED_CODE, recomp_types.h), which would rename this struct's
 * fields in that file alone.
 */
#ifndef RECOMP_HOOK_MOD_H
#define RECOMP_HOOK_MOD_H

#if defined(RECOMP_GENERATED_CODE) || defined(eax) || defined(ecx) || defined(edx) || \
    defined(esp)
#  error "recomp_hook_mod.h is not for the generated code: its register macros rename struct xbox_mod_call's fields"
#endif

#include "recomp_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- What a mod's hook sees (the mod API's hook(), mod.h) ---------------- */

/* One call of a hooked function. Filled at entry from the guest registers:
 * esp points at the return address, so stack argument n (0-based) is at
 * esp + 4 + 4 * n (XBOX_MOD_ARG32 / XBOX_MOD_ARGF in mod.h). ecx and edx
 * are the entry values, for thiscall and fastcall. The post hooks see the
 * same esp, so they read the arguments the pre hooks saw.
 *
 * The return fields are what the function returns: a post hook reads them
 * after the body ran and may change them. A pre hook that sets skip = 1
 * (only with XBOX_MOD_HOOK_MAY_SKIP) returns them instead of running the
 * body. Once one has, the later pre hooks and every post hook see skip = 1
 * and the return it set; they run in chain order, so a later one that
 * writes eax, edx_ret or st0 changes what the call returns, and none can
 * clear the skip.
 *
 * st0_valid says the function returns a float on the x87 stack (the slot's
 * RECOMP_HOOK_SLOT_ST0). It is read-only for hooks: the runtime decides
 * every x87 push and write-back from the slot, never from this field, and
 * resets it before the post hooks. On such a function a skip pushes st0,
 * and otherwise st0 is written back only when a post changed it. */
struct xbox_mod_call {
    uint32_t esp;
    uint32_t ecx, edx;
    uint32_t eax, edx_ret;
    float st0;
    int st0_valid;
    int skip;
};

typedef void (*xbox_mod_hook_fn)(struct xbox_mod_call *c, void *user);

/* hook() flag: this hook's pre may set skip. One per VA: a second one is
 * refused, since two hooks that each replace the function cannot both win.
 * Any other bit is refused. */
#define XBOX_MOD_HOOK_MAY_SKIP 1u

/* ---- The runtime's side (mod.c) ------------------------------------------ */

/* The registered slot for `va`, or NULL when the game declared none. */
struct recomp_hook_slot *recomp_hook_slot_find(uint32_t va);

/* Queue a hook for `va` on behalf of `owner` (an opaque token, the mod).
 * Returns 1, or 0 with a log line when the VA has no slot, a flag is
 * unknown, a second MAY_SKIP hook is asked for, or the hooks are full.
 * Nothing runs until recomp_hook_publish. */
int recomp_hook_add(const void *owner, const char *owner_name, uint32_t va,
                    xbox_mod_hook_fn pre, xbox_mod_hook_fn post, uint32_t flags,
                    void *user);

/* Drop every queued hook of `owner` (its init failed). */
void recomp_hook_drop_owner(const void *owner);

/* Build the chains from the queued hooks and set each slot's chain. Called
 * once, after every mod's init. Returns the number of hooked slots. */
int recomp_hook_publish(void);

/* Shutdown: every slot's chain back to NULL, so no hook runs once the mods'
 * shutdowns have. The chains' storage stays, so a guest thread already in
 * a hooked call finishes it. */
void recomp_hook_unpublish(void);

/* Clear every chain and queued hook (a new start, tests). The slot tables
 * stay registered. */
void recomp_hook_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_HOOK_MOD_H */
