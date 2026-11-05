/**
 * recomp_hook_run.c - a hooked call (recomp_hook.h), apart from the hook
 * queue in recomp_hook.c: this is the one part that touches the guest
 * registers, so a test of the mod layer links recomp_hook.c without them.
 */
#include "recomp_hook_int.h"

#include <stdio.h>
#include <string.h>

#include "xbox_memory_layout.h"

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;

void recomp_hook_run(struct recomp_hook_slot *slot, void (*body)(void))
{
    const struct recomp_hook_chain *ch = __atomic_load_n(&slot->chain, __ATOMIC_ACQUIRE);
    /* Every x87 decision comes from the slot: st0_valid is the hooks' to
     * read, and a push the function never makes would unbalance the
     * x87 stack for the rest of the frame. */
    const int has_st0 = (slot->flags & RECOMP_HOOK_SLOT_ST0) != 0;
    struct xbox_mod_call c;

    if (!ch) { /* unpublished since the dispatch read it */
        body();
        return;
    }
    memset(&c, 0, sizeof c);
    c.esp = g_esp;
    c.ecx = g_ecx;
    c.edx = g_edx;
    c.st0_valid = has_st0;

    /* A later pre sees the skip and the return the skipper set, and cannot
     * clear it. */
    int skipped = 0;
    for (int i = 0; i < ch->n; i++) {
        struct hook_ent *e = ch->e[i];
        if (!e->pre)
            continue;
        c.skip = skipped;
        e->pre(&c, e->user);
        if (skipped || !c.skip)
            continue;
        if (e->flags & XBOX_MOD_HOOK_MAY_SKIP) {
            skipped = 1;
        } else if (!e->warned) {
            e->warned = 1;
            fprintf(stderr,
                    "[MOD %s] hook 0x%08X: skip ignored (the hook was not registered "
                    "with XBOX_MOD_HOOK_MAY_SKIP)\n",
                    e->name, (unsigned)slot->va);
        }
    }
    c.skip = skipped;
    c.st0_valid = has_st0;

    /* The x87 return as the body left it, to tell whether a post changed
     * it: writing the float back unchanged would cut the double to float. */
    double st0_body = 0.0;
    if (skipped) {
        /* What the body's `ret n` would have done to the stack. */
        g_esp = c.esp + 4u + slot->ret_pop;
    } else {
        body();
        c.eax = g_eax;
        c.edx_ret = g_edx;
        if (has_st0) {
            st0_body = g_fp_stack[g_fp_top];
            c.st0 = (float)st0_body;
        }
    }

    for (int i = ch->n - 1; i >= 0; i--) {
        struct hook_ent *e = ch->e[i];
        if (e->post) {
            e->post(&c, e->user);
            c.skip = skipped;
            c.st0_valid = has_st0;
        }
    }

    g_eax = c.eax;
    g_edx = c.edx_ret;
    if (!has_st0)
        return;
    if (skipped) {
        g_fp_top = (g_fp_top + 7) & 7;
        g_fp_stack[g_fp_top] = (double)c.st0;
    } else if (c.st0 != (float)st0_body) {
        g_fp_stack[g_fp_top] = (double)c.st0;
    }
}
