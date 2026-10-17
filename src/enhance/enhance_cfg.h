/*
 * enhance_cfg.h - the configuration files, layered under the environment.
 *
 * Opt-in (XBOXRECOMP_ENHANCE=ON). Reads TOML (recomp_cfg.h) and answers
 * typed lookups by dotted key; for a key, the first of these with a value
 * wins:
 *
 *   1. the environment variable bound to the key, when set
 *      (RECOMP_RENDER_SCALE for render.scale; recomp_env.h config tier)
 *   2. <root>/<title>/config.toml, the per-title user file
 *   3. <root>/config/config.toml, the user file
 *   4. <exe dir>/config.default.toml, the defaults shipped with the build
 *      (on Apple platforms <exe dir>/../Resources/ when absent beside the
 *      executable); read, never written
 *   5. the caller's default
 *
 * `root` is the user data root (a bundle's data folder, the working
 * directory of a developer run), not the executable's directory: the
 * defaults file is found from recomp_exe_dir() here, so no caller needs two
 * roots. RECOMP_CONFIG=<path> replaces 2 and 3 with that one file and keeps
 * 4; RECOMP_CONFIG=none reads no file at all, the defaults included (the
 * golden pin). RECOMP_ENHANCE_CONFIG is its legacy name.
 *
 * The user file holds differences only: the keys whose value differs from
 * the defaults file's. Before a user file named config.toml is read, an
 * enhance.toml beside it is renamed to it once (an earlier version's file);
 * an absent user file whose directory exists is created as a comment
 * header. A file that does not parse is reported (with its line) and
 * skipped as a whole, so a typo never half applies. Docs:
 * docs/runtime/config.md.
 *
 * Call enhance_cfg_init once at startup, then read the keys during module
 * init (one thread) and keep the values: a lookup records that the key was
 * read, for enhance_cfg_report_unused.
 */
#ifndef ENHANCE_CFG_H
#define ENHANCE_CFG_H

#include "recomp_cfg.h"
#include "recomp_env.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ENHANCE_SRC_DEFAULT = 0,
    ENHANCE_SRC_ENV,
    ENHANCE_SRC_FILE,       /* a user file: config.toml or the title's */
    ENHANCE_SRC_DEFAULTS    /* config.default.toml */
} enhance_cfg_source;

/* Load the files. `root` is the user data root, `title` the per-title
 * directory name under it (either may be NULL; root NULL is "."). Logs one
 * line naming each file read, lowest layer first, with its own key count:
 *   [ENHANCE] config: <exe dir>/config.default.toml (9 keys), ./config/config.toml (1 key)
 *   [ENHANCE] config: none (RECOMP_CONFIG)
 * plus a line for a migration rename. Returns the number of files loaded,
 * or -1 if one failed to parse (or the RECOMP_CONFIG file cannot be read).
 * A second call reloads (for tests). */
int enhance_cfg_init(const char *root, const char *title);
void enhance_cfg_shutdown(void);

/* Bind a key to an environment variable of recomp_env's table. The toolkit
 * binds its own (render.scale, display.aspect); a game binds its keys
 * (e.g. game.mode -> RENV_GAME_MODE) before reading them. A later bind of the same
 * key replaces the earlier one. Returns 0 when the table is full. */
int enhance_cfg_bind_env(const char *key, recomp_env_id id);

/* Typed lookups, env over the files over default. An env value that does not
 * convert to the type, or a file value of another type, is reported once
 * and skipped (the next tier answers). */
const char *enhance_cfg_string(const char *key, const char *def);
long long enhance_cfg_int(const char *key, long long def);
double enhance_cfg_float(const char *key, double def);
int enhance_cfg_bool(const char *key, int def);   /* env: 1/0 true/false yes/no on/off */
int enhance_cfg_choice(const char *key, const char *const *choices, int def);

/* Where `key`'s value comes from, for the init log line of a module. */
enhance_cfg_source enhance_cfg_source_of(const char *key);

/* The file that holds `key` (title file first), or NULL, and the key counts
 * as read. For arrays, which have no env tier:
 *   const recomp_cfg *c = enhance_cfg_lookup("render.modes");
 *   n = recomp_cfg_array_len(c, "render.modes");                       */
const recomp_cfg *enhance_cfg_lookup(const char *key);

/* The parsed files, 0 title, 1 user (or the RECOMP_CONFIG file) and 2
 * defaults (NULL when absent), for prefix scans such as
 * aspect.program.<hash>; pass each key used to enhance_cfg_lookup. */
const recomp_cfg *enhance_cfg_file(int i);

/* What a differences-only writer (the in-game menu's Save) needs.
 *
 * enhance_cfg_user_path: the file it writes. The title file when one was
 * read (or created), else <root>/config/config.toml or the RECOMP_CONFIG
 * file; the path is returned even when the file does not exist (no
 * directory to create it in). NULL under RECOMP_CONFIG=none, where Save is
 * off.
 *
 * enhance_cfg_defaults: the parsed config.default.toml, or NULL. A writer
 * compares a new value with the defaults file's (recomp_cfg_* on it) or,
 * when the key is absent there, with the item's code default: equal means
 * delete the key from the user file, different means write it. Reading
 * this does not mark keys read. */
const char *enhance_cfg_user_path(void);
const recomp_cfg *enhance_cfg_defaults(void);

/* Log the file keys no lookup has read so far ("unused key render.scael,
 * config/config.toml:3"), the defaults file's included; a key a higher file
 * overrides counts as read. So a typo is visible. Call after the modules'
 * init. Returns the count. */
int enhance_cfg_report_unused(void);

#ifdef __cplusplus
}
#endif

#endif /* ENHANCE_CFG_H */
