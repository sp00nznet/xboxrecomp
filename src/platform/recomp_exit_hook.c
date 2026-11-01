/*
 * recomp_exit_hook.c - the hook a window runs before ExitProcess
 * (recomp_exit_hook.h).
 */
#include "recomp_exit_hook.h"

#include <stddef.h>
#include <stdio.h>

/* Set by whichever thread notes the first miss, read by the window thread:
 * an aligned pointer store, so the reader sees the old or the new hook. */
static recomp_exit_hook_fn volatile s_hook;

void recomp_exit_hook_set(recomp_exit_hook_fn fn)
{
    s_hook = fn;
}

/* The kernel's summary, the present trace, the mouse's clip, the mods and
 * the [MEM] exit line take one each; 8 leaves room. A full table says so
 * on stderr: the hook left out is often the mouse's, added last, whose
 * clip then outlives a Wine exit. */
#define MORE 8
static recomp_exit_hook_fn s_more[MORE];

int recomp_exit_hook_add(recomp_exit_hook_fn fn)
{
    for (int i = 0; i < MORE; i++) {
        recomp_exit_hook_fn cur = NULL;
        if (__atomic_compare_exchange_n(&s_more[i], &cur, fn, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)
            || cur == fn)
            return 1;
    }
    fprintf(stderr, "[EXIT] hook table full (%d); one exit hook left out\n", MORE);
    return 0;
}

void recomp_exit_hook_run(const char *why)
{
    recomp_exit_hook_fn fn = s_hook;
    if (fn != NULL)
        fn(why);
    for (int i = 0; i < MORE; i++) {
        fn = __atomic_load_n(&s_more[i], __ATOMIC_ACQUIRE);
        if (fn != NULL)
            fn(why);
    }
}

static recomp_quit_fn volatile s_quit;

void recomp_quit_set(recomp_quit_fn fn)
{
    __atomic_store_n(&s_quit, fn, __ATOMIC_RELEASE);
}

int recomp_quit_post(const char *why)
{
    recomp_quit_fn fn = __atomic_load_n(&s_quit, __ATOMIC_ACQUIRE);
    if (fn == NULL)
        return 0;
    fn(why);
    return 1;
}
