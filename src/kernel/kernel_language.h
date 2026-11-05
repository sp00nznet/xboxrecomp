/*
 * kernel_language.h - the dashboard language the title is told
 *
 * XAPI's XGetLanguage reads EEPROM index 7 (XC_LANGUAGE), and some titles
 * read it from the XC_MAX_OS block at +0x30. Titles pick their text and
 * voice archives from it, so the stock answer (1, English) kept every
 * player in English whatever the disc carries.
 *
 * The language in force is system.language (RECOMP_SYSTEM_LANGUAGE): one
 * of the dashboard's nine codes, en ja de fr es it ko zh pt, which are
 * XC_LANGUAGE 1..9 in that order, lower case only (zh is the XDK's
 * traditional Chinese). Unset is English, the stock answer.
 *
 * A game declares the languages it ships in its game.toml ([system]
 * languages); the CLI passes them to the build as XBOXRECOMP_LANGUAGES
 * ("en,ja,de"), compiled into kernel_language.c alone. With a list, a code
 * outside it is refused (English, one log line): a title asked for a
 * language its disc lacks falls back in its own way, or not at all. With no
 * list any of the nine is taken, and the menu offers none.
 */
#ifndef KERNEL_LANGUAGE_H
#define KERNEL_LANGUAGE_H

#include "kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_LANGUAGE_ENGLISH 1u

/* The XC_LANGUAGE value of a code ("fr" -> 4, lower case only), 0 for none. */
ULONG xbox_LanguageFromCode(const char *code);
/* The code of an XC_LANGUAGE value, NULL outside 1..9. */
const char *xbox_LanguageCode(ULONG lang);
/* The English name of an XC_LANGUAGE value ("French"), NULL outside 1..9. */
const char *xbox_LanguageName(ULONG lang);

/* The game's declared languages, NULL-terminated, in its order; empty (the
 * first entry NULL) when it declares none. */
const char *const *xbox_LanguagesShipped(void);

/* The language for this run, from system.language: called before the guest
 * starts (the enhance layer does). NULL or "" is English. A code that is
 * unknown, or not among the declared languages, is logged and gives
 * English. Returns the value in force. Without a call, the first query
 * reads RECOMP_SYSTEM_LANGUAGE the same way. */
ULONG xbox_SetLanguage(const char *code);
/* The XC_LANGUAGE value the kernel answers with. */
ULONG xbox_Language(void);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_LANGUAGE_H */
