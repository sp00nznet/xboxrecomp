/* system.language: the first query reads RECOMP_SYSTEM_LANGUAGE,
 * a code the game does not ship or no code at all gives English, and the
 * XC_MAX_OS block carries the same value at +0x30 as XC_LANGUAGE. */
#include "kernel.h"
#include "kernel_language.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* This settings-only test must never dispatch guest code. */
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

static ULONG query(void)
{
    ULONG v = 0xdeadbeef, type = 0, len = 0;
    xbox_ExQueryNonVolatileSetting(XC_LANGUAGE, &type, &v, sizeof v, &len);
    return v;
}

static ULONG block(void)
{
    uint8_t blk[0x60];
    ULONG v, type = 0, len = 0;
    memset(blk, 0xCC, sizeof blk);
    xbox_ExQueryNonVolatileSetting(XC_MAX_OS, &type, blk, sizeof blk, &len);
    memcpy(&v, blk + 0x30, 4);
    return v;
}

static int check(const char *what, ULONG got, ULONG want)
{
    if (got == want && block() == want)
        return 0;
    fprintf(stderr, "FAIL: %s: XC_LANGUAGE %lu, block %lu, want %lu\n", what,
            (unsigned long)got, (unsigned long)block(), (unsigned long)want);
    return 1;
}

int main(void)
{
    static const char *const want[] = { "en", "ja", "fr", "es", NULL };
    const char *const *l = xbox_LanguagesShipped();
    int fail = 0, i;

    for (i = 0; want[i] || l[i]; i++)
        if (!want[i] || !l[i] || strcmp(want[i], l[i])) {
            fprintf(stderr, "FAIL: shipped[%d] %s, want %s\n", i, l[i] ? l[i] : "(end)",
                    want[i] ? want[i] : "(end)");
            return 1;
        }
#ifdef _WIN32
    _putenv("RECOMP_SYSTEM_LANGUAGE=fr");
#else
    setenv("RECOMP_SYSTEM_LANGUAGE", "fr", 1);
#endif
    fail |= check("env fr, first query", query(), 4);
    /* Lower case only: the menu would show "en" for a French "FR". */
    fail |= check("FR, upper case", (xbox_SetLanguage("FR"), query()), XBOX_LANGUAGE_ENGLISH);
    fail |= check("ja", (xbox_SetLanguage("ja"), query()), 2);
    fail |= check("de, not shipped", (xbox_SetLanguage("de"), query()), XBOX_LANGUAGE_ENGLISH);
    fail |= check("xx, unknown", (xbox_SetLanguage("xx"), query()), XBOX_LANGUAGE_ENGLISH);
    fail |= check("es", (xbox_SetLanguage("es"), query()), 5);
    fail |= check("unset", (xbox_SetLanguage(""), query()), XBOX_LANGUAGE_ENGLISH);
    fail |= check("NULL", (xbox_SetLanguage(NULL), query()), XBOX_LANGUAGE_ENGLISH);
    if (xbox_LanguageFromCode("pt") != 9 || xbox_LanguageFromCode("e") != 0
        || strcmp(xbox_LanguageName(6), "Italian")
        || strcmp(xbox_LanguageName(8), "Chinese (Traditional)") || xbox_LanguageCode(10)) {
        fprintf(stderr, "FAIL: the code table\n");
        fail = 1;
    }
    if (!fail)
        printf("kernel_language: ok\n");
    return fail;
}
