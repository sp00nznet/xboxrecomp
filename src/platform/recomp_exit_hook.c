/*
 * recomp_exit_hook.c - the hook a window runs before ExitProcess
 * (recomp_exit_hook.h).
 */
#include "recomp_exit_hook.h"

#include <stddef.h>

/* Set by whichever thread notes the first miss, read by the window thread:
 * an aligned pointer store, so the reader sees the old or the new hook. */
static recomp_exit_hook_fn volatile s_hook;

void recomp_exit_hook_set(recomp_exit_hook_fn fn)
{
    s_hook = fn;
}

static recomp_exit_hook_fn s_more[4];

int recomp_exit_hook_add(recomp_exit_hook_fn fn)
{
    for (int i = 0; i < 4; i++) {
        recomp_exit_hook_fn cur = NULL;
        if (__atomic_compare_exchange_n(&s_more[i], &cur, fn, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)
            || cur == fn)
            return 1;
    }
    return 0;
}

void recomp_exit_hook_run(const char *why)
{
    recomp_exit_hook_fn fn = s_hook;
    if (fn != NULL)
        fn(why);
    for (int i = 0; i < 4; i++) {
        fn = __atomic_load_n(&s_more[i], __ATOMIC_ACQUIRE);
        if (fn != NULL)
            fn(why);
    }
}
