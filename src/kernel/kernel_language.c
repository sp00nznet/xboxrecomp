/*
 * kernel_language.c - the dashboard language; see kernel_language.h.
 */
#include "kernel_language.h"
#include "recomp_env.h"
#include <stdio.h>
#include <string.h>

/* XC_LANGUAGE 1..9, the dashboard's order (XDK xbox.h XC_LANGUAGE_*). */
static const char *const k_code[] = {
    "en", "ja", "de", "fr", "es", "it", "ko", "zh", "pt"
};
static const char *const k_name[] = {
    "English", "Japanese", "German", "French", "Spanish", "Italian", "Korean",
    "Chinese (Traditional)", "Portuguese"
};
#define N_LANG ((ULONG)(sizeof k_code / sizeof k_code[0]))

/* XBOXRECOMP_LANGUAGES, from the game's game.toml through the CLI;
 * the toolkit's CMake checks every code at configure. */
#ifndef XBOXRECOMP_LANGUAGES
#define XBOXRECOMP_LANGUAGES ""
#endif

static const char *s_shipped[N_LANG + 1];
static int s_shipped_built;
/* 0 until resolved (a call, or the first query reading the env). Set
 * before the guest's first thread runs, so no lock: the guest only reads. */
static ULONG s_lang;

/* Lower case only, as the menu compares the config value with its choices
 * exactly: "FR" accepted here would run French behind a row showing "en". */
static int code_is(const char *v, const char *code)
{
    return v[0] == code[0] && v[1] == code[1] && !v[2];
}

ULONG xbox_LanguageFromCode(const char *code)
{
    ULONG i;
    if (!code || !code[0] || !code[1])
        return 0;
    for (i = 0; i < N_LANG; i++)
        if (code_is(code, k_code[i]))
            return i + 1;
    return 0;
}

const char *xbox_LanguageCode(ULONG lang)
{
    return lang >= 1 && lang <= N_LANG ? k_code[lang - 1] : NULL;
}

const char *xbox_LanguageName(ULONG lang)
{
    return lang >= 1 && lang <= N_LANG ? k_name[lang - 1] : NULL;
}

const char *const *xbox_LanguagesShipped(void)
{
    if (!s_shipped_built) {
        const char *p = XBOXRECOMP_LANGUAGES;
        int n = 0;
        while (*p && n < (int)N_LANG) {
            char c[3] = { 0, 0, 0 };
            size_t len = strcspn(p, ",");
            ULONG v;
            if (len == 2) {
                c[0] = p[0];
                c[1] = p[1];
            }
            v = xbox_LanguageFromCode(c);
            if (v) {
                int dup = 0;
                for (int k = 0; k < n; k++)
                    dup |= s_shipped[k] == k_code[v - 1];
                if (!dup)
                    s_shipped[n++] = k_code[v - 1];
            }
            p += len;
            if (*p == ',')
                p++;
        }
        s_shipped[n] = NULL;
        s_shipped_built = 1;
    }
    return s_shipped;
}

static int shipped(ULONG lang)
{
    const char *const *l = xbox_LanguagesShipped();
    if (!l[0])
        return 1;
    for (; *l; l++)
        if (*l == k_code[lang - 1])
            return 1;
    return 0;
}

ULONG xbox_SetLanguage(const char *code)
{
    ULONG v = XBOX_LANGUAGE_ENGLISH;
    if (code && *code) {
        ULONG want = xbox_LanguageFromCode(code);
        if (!want)
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_XBOX,
                "system.language=%s is not a language code (en ja de fr es it ko zh pt); "
                "English", code);
        /* English is the fallback anyway, so a game without it does not
         * warn on every boot for the default "en". */
        else if (want != XBOX_LANGUAGE_ENGLISH && !shipped(want))
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_XBOX,
                "system.language=%s: this game ships %s; English", code, XBOXRECOMP_LANGUAGES);
        else
            v = want;
    }
    if (v != XBOX_LANGUAGE_ENGLISH)
        xbox_log(XBOX_LOG_INFO, XBOX_LOG_XBOX, "system.language=%s (%s, XC_LANGUAGE %lu)",
            k_code[v - 1], k_name[v - 1], (unsigned long)v);
    s_lang = v;
    return v;
}

ULONG xbox_Language(void)
{
    if (!s_lang)
        xbox_SetLanguage(recomp_env(RENV_SYSTEM_LANGUAGE));
    return s_lang;
}
