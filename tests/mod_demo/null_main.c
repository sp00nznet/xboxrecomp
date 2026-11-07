/*
 * mod_demo's other build: demo_mod.c with no enhancements layer, as main.c
 * sees it in a game built without one. The accessor must be there and return
 * NULL, so the game's table stays empty and it still links.
 */
#include <stdio.h>
#include "demo_mod.h"

int main(void)
{
    if (demo_mod() != NULL) {
        fprintf(stderr, "FAIL: demo_mod() is not NULL with no API\n");
        return 1;
    }
    printf("mod_demo_null: accessor is NULL\n");
    return 0;
}
