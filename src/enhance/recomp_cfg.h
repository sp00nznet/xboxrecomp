/*
 * recomp_cfg.h - a small TOML-subset config reader. C11, no dependencies.
 *
 * The subset (docs/runtime/config.md has the full grammar):
 *
 *   # comment
 *   [render]                 table; dotted names allowed: [aspect.program]
 *   scale = 2                integer (decimal, 0x, 0o, 0b, '_' separators)
 *   sharpness = 0.5          float (fraction and/or exponent, inf, nan)
 *   filter = "linear"        basic string ("..." with \ escapes) or '...'
 *   vsync = true             bool
 *   modes = [1, 2, 3]        array of scalars, may span lines, trailing comma
 *   a.b = 1                  dotted key: the same as [a] b = 1
 *
 * Not supported, and reported as errors with a line number: multi-line
 * strings, inline tables, arrays of tables (but recomp_cfg_load_string_aot),
 * nested arrays, dates and times.
 * Every file this reader accepts is valid TOML 1.0 (and must be UTF-8).
 *
 * Keys are looked up by their full dotted name ("render.scale"). A getter
 * returns its default when the key is missing or has another type; a float
 * getter also takes an integer. Every getter accepts a NULL config, so "no
 * file" needs no special case.
 */
#ifndef RECOMP_CFG_H
#define RECOMP_CFG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct recomp_cfg recomp_cfg;

typedef enum {
    RCFG_NONE = 0,   /* missing key */
    RCFG_STRING,
    RCFG_INT,
    RCFG_FLOAT,
    RCFG_BOOL,
    RCFG_ARRAY
} recomp_cfg_type;

/* Parse a file or a string. On failure NULL, and `err` (if given) holds
 * "<name>:<line>: <message>". A missing file is an error like any other;
 * callers that treat it as optional check for the file first. */
recomp_cfg *recomp_cfg_load_file(const char *path, char *err, size_t errlen);
recomp_cfg *recomp_cfg_load_string(const char *text, const char *name,
                                   char *err, size_t errlen);
void recomp_cfg_free(recomp_cfg *cfg);

/* As recomp_cfg_load_string, and [[name]] arrays of tables are taken: the
 * Nth [[name]] (from 0) is the table "name.N", so its keys read as
 * "name.N.key", and recomp_cfg_table_count gives how many there are. One
 * level only, and only for embedded data (a HUD table, hud_anchor.h): the
 * user's config files and the menu's Save keep refusing them. */
recomp_cfg *recomp_cfg_load_string_aot(const char *text, const char *name,
                                       char *err, size_t errlen);
/* The number of [[name]] tables, 0 when there are none. */
size_t recomp_cfg_table_count(const recomp_cfg *cfg, const char *name);

/* Parse a C-locale number ("0.5", "-1e3", "inf", "nan"; no hex floats or
 * spaces) whatever the process locale is. 1 and *out on success. */
int recomp_cfg_parse_double(const char *s, double *out);

/* Typed getters with defaults. */
recomp_cfg_type recomp_cfg_type_of(const recomp_cfg *cfg, const char *key);
const char *recomp_cfg_string(const recomp_cfg *cfg, const char *key, const char *def);
long long recomp_cfg_int(const recomp_cfg *cfg, const char *key, long long def);
double recomp_cfg_float(const recomp_cfg *cfg, const char *key, double def);
int recomp_cfg_bool(const recomp_cfg *cfg, const char *key, int def);

/* A string key that must be one of `choices` (NULL-terminated): the index
 * of the match, or `def` when missing, not a string, or not in the list. */
int recomp_cfg_choice(const recomp_cfg *cfg, const char *key,
                      const char *const *choices, int def);

/* Arrays: the length (0 when missing or not an array) and typed elements. */
size_t recomp_cfg_array_len(const recomp_cfg *cfg, const char *key);
recomp_cfg_type recomp_cfg_array_type(const recomp_cfg *cfg, const char *key, size_t i);
const char *recomp_cfg_array_string(const recomp_cfg *cfg, const char *key, size_t i, const char *def);
long long recomp_cfg_array_int(const recomp_cfg *cfg, const char *key, size_t i, long long def);
double recomp_cfg_array_float(const recomp_cfg *cfg, const char *key, size_t i, double def);
int recomp_cfg_array_bool(const recomp_cfg *cfg, const char *key, size_t i, int def);

/* Iteration, in file order (for prefix scans such as "aspect.program."). */
size_t recomp_cfg_count(const recomp_cfg *cfg);
const char *recomp_cfg_key_at(const recomp_cfg *cfg, size_t i);
/* The entry's index, or -1. */
long recomp_cfg_find(const recomp_cfg *cfg, const char *key);
/* The line the key was set on, or 0. */
int recomp_cfg_line(const recomp_cfg *cfg, const char *key);
/* The name given at load (the path, for a file). */
const char *recomp_cfg_name(const recomp_cfg *cfg);

/* ---- writing: the menu's Save ------------------------------------------ */

/* Set or remove keys in a config text and touch nothing else: a key that
 * is there keeps its line, spelling and comment and gets only its new value;
 * a new key goes under its table's [header] (a new [header] at the end when
 * the file has none); NULL removes the key's line, and a [header] left with
 * no keys and no comment. Each value is TOML as it would follow "key = "
 * ("2", "\"linear\"", "[\"F10\", \"guide+a\"]"). An array that keeps its
 * item count is edited item by item, so its layout and the comments between
 * its items stay; one that grows or shrinks is written on one line and
 * loses them. A comment line above a removed key stays (it cannot be told
 * from a note on the table). CRLF files stay CRLF.
 * The result is parsed again and must hold exactly the old keys plus the
 * patch, else nothing comes back. Returns the new text (free() it), or NULL
 * with `err` set; a text that does not parse is refused, never repaired.
 * A flat key the file holds as a [table] (or the start of a deeper one)
 * is left out and named in `err` (the file once, then each key), and the
 * other keys are still written: the text comes back with `err` set, as
 * far as `errlen` holds it. `err` may be NULL or of any length. */
char *recomp_cfg_patch_text(const char *text, const char *name,
                            const char *const *keys, const char *const *values,
                            size_t n, char *err, size_t errlen);

/* The same on a file (missing is empty): the new text goes to "<path>.tmp",
 * is flushed to the disk and renamed over the file, so a crash or a power
 * loss leaves one whole file. A path that is a symlink is written at its
 * target and stays a link (POSIX; on Windows the link is replaced). Nothing
 * is written when nothing changes. 0; the number of keys left out for a
 * [table], the rest written, with `err` naming them as far as it holds
 * (the count does not depend on it); or -1 with `err` set and nothing
 * written. */
int recomp_cfg_patch(const char *path, const char *const *keys,
                     const char *const *values, size_t n, char *err, size_t errlen);

/* `s` as a TOML basic string, quotes included. 0 when it does not fit. */
int recomp_cfg_quote(const char *s, char *out, size_t outlen);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_CFG_H */
