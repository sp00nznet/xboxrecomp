/* The symbols a real game's generated code and recomp_manual.c provide, so
 * the template's main.c links without a game. None of them runs: with no XBE
 * the template stops at its first step. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef void (*recomp_func_t)(void);

void xbe_entry_point(void)
{
    printf("template_main: stub xbe_entry_point\n");
}

recomp_func_t recomp_lookup(uint32_t xbox_va)
{
    (void)xbox_va;
    return NULL;
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return NULL;
}

int recomp_dispatch_init(void)
{
    return 0;
}

void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "template_main: stub recomp_icall_fail_log 0x%08X\n", va);
}
