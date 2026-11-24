/*
 * enhance_host.c - which machine the game runs on, for a game's or a
 * host's own defaults (enhance_cfg_set_code_default before
 * xbox_enhance_init): xbox_host_is_deck. No SDL and no video, so it
 * answers before any window or the overlay's display probe
 * (enhance_display.c, which reads the board from here too).
 */
#include "enhance.h"

#include "nv2a_overlay.h"
#include "recomp_env.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

/* Whether the machine is a Steam Deck by its SMBIOS names: Steam sets
 * SteamDeck only in Game Mode, and a Desktop Mode run (or one outside
 * Steam) still holds the Deck's 7" panel (a Deck check). */
int xbox_host_deck_board(void)
{
    char vendor[64] = "", product[64] = "";
#if defined(_WIN32)
    /* Windows keeps the SMBIOS names here, and Wine's wineboot fills the
     * same key from Linux's DMI, so one read serves Proton too. */
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", 0,
                      KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return 0;
    {
        DWORD n = sizeof vendor - 1, m = sizeof product - 1, t;
        if (RegQueryValueExA(k, "SystemManufacturer", NULL, &t, (BYTE *)vendor, &n) != ERROR_SUCCESS ||
            t != REG_SZ)
            vendor[0] = 0;
        if (RegQueryValueExA(k, "SystemProductName", NULL, &t, (BYTE *)product, &m) !=
                ERROR_SUCCESS ||
            t != REG_SZ)
            product[0] = 0;
    }
    RegCloseKey(k);
#elif defined(__linux__)
    static const char *const k_path[] = {"/sys/class/dmi/id/sys_vendor",
                                         "/sys/class/dmi/id/product_name"};
    char *dst[] = {vendor, product};
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(k_path[i], "r");
        if (!f)
            return 0;
        if (!fgets(dst[i], 64, f))
            dst[i][0] = 0;
        fclose(f);
    }
#endif
    return nv2a_overlay_is_deck_board(vendor, product);
}

int xbox_host_is_deck(void)
{
    const char *deck = recomp_env(RENV_STEAM_DECK);
    return (deck && strcmp(deck, "1") == 0) || xbox_host_deck_board();
}
