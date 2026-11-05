/*
 * shader_param_kinds.h - how the Shaders page shows a preset parameter: a
 * toggle, a choice or a slider (overlay-revamp D5).
 *
 * A slang preset declares each parameter as a number with a range and a
 * step (#pragma parameter); nothing says that 0..3 is four mask types. The
 * kind comes from the first of these rules that matches:
 *   1. declared: a shader profile's [param_kinds] table, then the toolkit's
 *      sidecar (assets/shaders/param-kinds.toml, keyed by the declaring
 *      shader file and the name);
 *   2. inferred toggle: 0..1, step 1;
 *   3. inferred choice: an integer range of 3 to 8 values, step 1, and a
 *      label ending in ": name | name ..." with one name per value;
 *   4. a slider.
 * Values stay numbers whatever the kind: a toggle is min (off) or max (on),
 * a choice is min + its index, so profile files and the config keep what
 * they hold today.
 *
 * No GPU, no library and no menu here, so the ctest runs the rules over
 * every shipped shader anywhere.
 */
#ifndef SHADER_PARAM_KINDS_H
#define SHADER_PARAM_KINDS_H

#include <stddef.h>

#include "nv2a_present_chain.h"

#ifdef __cplusplus
extern "C" {
#endif

enum shader_param_kind {
    SHADER_PARAM_SLIDER = 0,
    SHADER_PARAM_TOGGLE,
    SHADER_PARAM_CHOICE,
};

#define SHADER_PARAM_CHOICES_MAX 8
#define SHADER_PARAM_CHOICE_LEN 32
#define SHADER_PARAM_HELP_LEN 100   /* leaves the row room for its bounds */
#define SHADER_PARAM_DECLS_MAX 64   /* in the sidecar, or in one profile */

/* A declared kind. kind < 0: no kind declared (a help string alone). */
struct shader_param_decl {
    char file[64];      /* the declaring shader's basename; "" in a profile */
    char name[64];
    int kind;
    int nchoices;
    char choice[SHADER_PARAM_CHOICES_MAX][SHADER_PARAM_CHOICE_LEN];
    char help[SHADER_PARAM_HELP_LEN];
};

struct shader_param_kinds {
    int n;
    struct shader_param_decl d[SHADER_PARAM_DECLS_MAX];
};

/* How a row shows. names: a choice's one per value; a toggle's off and on
 * words when its label named them ("Gray | RGB"), else none. */
struct shader_param_kind_info {
    int kind;
    char label[96];     /* desc (else name), a trailing ": a | b" cut off */
    int nnames;
    char name[SHADER_PARAM_CHOICES_MAX][SHADER_PARAM_CHOICE_LEN];
    char help[SHADER_PARAM_HELP_LEN];   /* the declaration's, "" none */
};

/* The sidecar's format: one [[param]] per parameter, with file, name,
 * kind ("toggle", "choice" or "slider"), choices (a choice's names, one
 * per value from min) and help, each optional but file and name. A
 * broken entry is skipped with a line on stderr. Returns 0, or -1 with err
 * when the text does not parse. */
int shader_param_kinds_parse(const char *text, const char *src, struct shader_param_kinds *t,
                             char *err, size_t errlen);

/* A shader profile's [param_kinds.<name>] tables (the same keys but file):
 * text is the profile file. A profile's names are one row each across its
 * stages, so the name alone keys them. Returns 0, or -1 with err. */
int shader_param_kinds_parse_profile(const char *text, const char *src,
                                     struct shader_param_kinds *t, char *err, size_t errlen);

/* The [param_kinds] of the profile file at path, read again only when the
 * path, its size or its time changed. NULL when there is no such file; the
 * table is the module's, valid until the next call. Menu thread only:
 * the cache is unlocked. */
const struct shader_param_kinds *shader_param_kinds_profile_file(const char *path);

/* 1 when key belongs to a profile's [param_kinds] (for its parser's
 * unknown-key check). */
int shader_param_kinds_profile_key(const char *key);

/* The declaration of (file, name); file "" or NULL matches any file. */
const struct shader_param_decl *shader_param_kinds_find(const struct shader_param_kinds *t,
                                                        const char *file, const char *name);

/* The toolkit's sidecar, shaders/param-kinds.toml beside the executable
 * (Resources/shaders in a macOS bundle), read on the first call. An empty
 * table when there is none. Menu thread only, as is builtin_reset: the
 * table is read and replaced unlocked. */
const struct shader_param_kinds *shader_param_kinds_builtin(void);

/* Read the sidecar from path instead (the ctest; NULL: beside the
 * executable again, at the next builtin call). */
void shader_param_kinds_builtin_reset(const char *path);

/* The rules above for p. profile: the profile's table, or NULL. A
 * declaration that cannot fit the range (a choice whose names are not one
 * per value) does not match, and the next rule runs. */
void shader_param_kind_resolve(const struct nv2a_present_param *p,
                               const struct shader_param_kinds *profile,
                               const struct shader_param_kinds *sidecar,
                               struct shader_param_kind_info *out);

/* A choice's index for a value (rounded, clamped to the range), and the
 * value for an index (clamped): min + index. */
int shader_param_kind_index(const struct nv2a_present_param *p, float v);
float shader_param_kind_value(const struct nv2a_present_param *p, int index);

/* A toggle's state: on from the middle of the range up, and on and off
 * store max and min. A declared toggle may span any range (0..2 by 1), so
 * its index is never a choice's. */
int shader_param_toggle_on(const struct nv2a_present_param *p, float v);
float shader_param_toggle_value(const struct nv2a_present_param *p, int on);

/* A choice of n names one step from v (dir < 0 back), wrapping at both
 * ends, as the menu's Left and Right do. v off the grid steps from its
 * nearest index. */
float shader_param_choice_next(const struct nv2a_present_param *p, int n, float v, int dir);

#ifdef __cplusplus
}
#endif

#endif /* SHADER_PARAM_KINDS_H */
