/**
 * recomp_hook.c - the runtime side of the hook slots (recomp_hook.h,
 * recomp_hook_mod.h).
 *
 * Mods queue their hooks during init (the mod API's hook(), mod.c); after
 * every init the queue becomes one chain per hooked slot, and the slot's
 * chain pointer is set. From then on the chains are read-only, so the guest
 * threads that call a hooked function read them without a lock.
 */
#include "recomp_hook_int.h"

#include <stdio.h>
#include <string.h>

/* Tables: one per generated file that has slots. Hooks: every mod's, in
 * total. */
#define HOOK_TABLES_MAX 32
#define HOOK_MAX 64

static struct {
    struct recomp_hook_slot *slots;
    size_t n;
} s_tabs[HOOK_TABLES_MAX];
static int s_ntabs;

static struct hook_ent s_q[HOOK_MAX];
static int s_nq;
static struct recomp_hook_chain s_chains[HOOK_MAX];
static int s_published;

void recomp_hook_slots_register(struct recomp_hook_slot *slots, size_t n)
{
    if (!slots || n == 0)
        return;
    if (s_ntabs >= HOOK_TABLES_MAX) {
        fprintf(stderr, "[MOD] hook slots: more than %d tables, %zu slots left out\n",
                HOOK_TABLES_MAX, n);
        return;
    }
    /* Two tables naming one VA: find() keeps the first, so the second's
     * dispatch never runs a hook. Say so rather than hook half the calls. */
    for (size_t i = 0; i < n; i++) {
        if (recomp_hook_slot_find(slots[i].va))
            fprintf(stderr, "[MOD] hook slot 0x%08X is in two tables; the second is never hooked\n",
                    (unsigned)slots[i].va);
    }
    s_tabs[s_ntabs].slots = slots;
    s_tabs[s_ntabs].n = n;
    s_ntabs++;
}

struct recomp_hook_slot *recomp_hook_slot_find(uint32_t va)
{
    for (int t = 0; t < s_ntabs; t++)
        for (size_t i = 0; i < s_tabs[t].n; i++)
            if (s_tabs[t].slots[i].va == va)
                return &s_tabs[t].slots[i];
    return NULL;
}

int recomp_hook_add(const void *owner, const char *owner_name, uint32_t va,
                    xbox_mod_hook_fn pre, xbox_mod_hook_fn post, uint32_t flags,
                    void *user)
{
    const char *nm = owner_name ? owner_name : "?";
    if (s_published) {
        fprintf(stderr, "[MOD %s] hook 0x%08X: hooks are taken only during init\n", nm,
                (unsigned)va);
        return 0;
    }
    if (flags & ~XBOX_MOD_HOOK_MAY_SKIP) {
        fprintf(stderr, "[MOD %s] hook 0x%08X: refused, unknown flags 0x%X\n", nm,
                (unsigned)va, (unsigned)(flags & ~XBOX_MOD_HOOK_MAY_SKIP));
        return 0;
    }
    if (!pre && !post) {
        fprintf(stderr, "[MOD %s] hook 0x%08X: no pre or post callback\n", nm, (unsigned)va);
        return 0;
    }
    if (!recomp_hook_slot_find(va)) {
        fprintf(stderr,
                "[MOD %s] hook 0x%08X: no hook slot (the game's [mods] hook_functions "
                "does not list it)\n",
                nm, (unsigned)va);
        return 0;
    }
    if ((flags & XBOX_MOD_HOOK_MAY_SKIP) && !pre) {
        fprintf(stderr, "[MOD %s] hook 0x%08X: MAY_SKIP needs a pre callback\n", nm,
                (unsigned)va);
        return 0;
    }
    int on_va = 0;
    for (int i = 0; i < s_nq; i++) {
        if (s_q[i].va != va)
            continue;
        on_va++;
        if ((flags & XBOX_MOD_HOOK_MAY_SKIP) && (s_q[i].flags & XBOX_MOD_HOOK_MAY_SKIP)) {
            fprintf(stderr,
                    "[MOD %s] hook 0x%08X: refused, %s already replaces this function "
                    "(one MAY_SKIP hook per VA)\n",
                    nm, (unsigned)va, s_q[i].name ? s_q[i].name : "?");
            return 0;
        }
    }
    if (on_va >= HOOK_PER_VA || s_nq >= HOOK_MAX) {
        fprintf(stderr, "[MOD %s] hook 0x%08X: refused, the hooks are full\n", nm,
                (unsigned)va);
        return 0;
    }
    struct hook_ent *e = &s_q[s_nq++];
    memset(e, 0, sizeof *e);
    e->owner = owner;
    e->name = nm;
    e->va = va;
    e->pre = pre;
    e->post = post;
    e->flags = flags;
    e->user = user;
    return 1;
}

void recomp_hook_drop_owner(const void *owner)
{
    int w = 0;
    for (int i = 0; i < s_nq; i++)
        if (s_q[i].owner != owner)
            s_q[w++] = s_q[i];
    s_nq = w;
}

int recomp_hook_publish(void)
{
    int nchains = 0;
    for (int t = 0; t < s_ntabs; t++) {
        for (size_t i = 0; i < s_tabs[t].n; i++) {
            struct recomp_hook_slot *slot = &s_tabs[t].slots[i];
            if (recomp_hook_slot_find(slot->va) != slot)
                continue; /* the duplicate register() logged */
            struct recomp_hook_chain *ch = &s_chains[nchains];
            ch->n = 0;
            /* Queue order is the mod table's order: that is the pre order. */
            for (int q = 0; q < s_nq; q++)
                if (s_q[q].va == slot->va)
                    ch->e[ch->n++] = &s_q[q];
            if (ch->n) {
                /* Release: a guest thread that sees the pointer sees the
                 * chain filled in. */
                __atomic_store_n(&slot->chain, ch, __ATOMIC_RELEASE);
                nchains++;
            }
        }
    }
    s_published = 1;
    return nchains;
}

void recomp_hook_unpublish(void)
{
    /* The chains and the queue stay as they are: a thread inside a hooked
     * call still walks them. Only new calls stop seeing them. */
    for (int t = 0; t < s_ntabs; t++)
        for (size_t i = 0; i < s_tabs[t].n; i++)
            __atomic_store_n(&s_tabs[t].slots[i].chain, NULL, __ATOMIC_RELEASE);
}

void recomp_hook_reset(void)
{
    recomp_hook_unpublish();
    s_nq = 0;
    s_published = 0;
    memset(s_chains, 0, sizeof s_chains);
}
