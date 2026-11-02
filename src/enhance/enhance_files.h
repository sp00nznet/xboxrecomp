/*
 * enhance_files.h - the menu's import and export texts (openspec change
 * overlay-revamp, D6), pure so the ctest covers them: an exported config's
 * header, the export marker an import checks, and a config value written
 * back as the TOML it would follow "key = " with.
 *
 * A config has no version key of its own, so an export says what it is on
 * its first lines, as comments any TOML reader skips:
 *
 *   # xbr-export: config 1
 *   # xbr-export: 2 unsaved values included: render.scale, fx.glow
 *
 * An import reads the marker back: a file another build exported in a
 * later format is refused rather than half applied, and a file with no
 * marker is a hand-written config, read as format 1.
 */
#ifndef ENHANCE_FILES_H
#define ENHANCE_FILES_H

#include <stddef.h>

#include "recomp_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EF_CONFIG_FORMAT 1
#define EF_KIND_MAX 16

/* The marker on the leading comment lines of text: 1 with kind ("config")
 * and *format set, 0 when there is none (blank and comment lines are read
 * up to the first other line). A marker that names no format reads as 0. */
int ef_marker(const char *text, char *kind, size_t kind_n, int *format);

/* The exported config: user_text (the user file, "" when it has none)
 * with the n pending keys patched in (recomp_cfg_patch_text: NULL values
 * remove a key), under the marker and a line naming the unsaved keys
 * listed (nunsaved of them; none: no such line). A marker the user text
 * already starts with is dropped first. CRLF text keeps CRLF. Returns the
 * text (free() it), or NULL with err set. */
char *ef_export_config(const char *user_text, const char *const *keys,
                       const char *const *values, size_t n, const char *const *unsaved,
                       size_t nunsaved, char *err, size_t errlen);

/* The value of key in cfg as TOML ("2", "1.5", "\"linear\"", "[\"F10\"]"),
 * so a key the menu has no slot for is written back as it was read. A
 * float keeps a point or an exponent ("1.0"), so it stays a float. 0 when
 * the key is missing or the text does not fit in out. */
int ef_literal(const recomp_cfg *cfg, const char *key, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif
