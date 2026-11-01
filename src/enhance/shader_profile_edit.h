/*
 * shader_profile_edit.h - the Shaders page's model of the shader profiles
 * (design D5): the picker's entries (Off, the bundled presets, a config
 * path, the player's profiles), the chosen one's chain and its edits, and
 * what Save writes. No rows and no drawing: enhance_menu.c lays the page
 * out from this. The menu's thread only (under its step lock).
 *
 * Every edit is live: the chosen entry is requested again at once, and its
 * parameter values stay in the present chain's store (keyed by the slug)
 * until Save writes them to the profile's file. A bundled preset or a
 * config path is a one-stage profile kept in present.shader; the first
 * chain edit on one forks it into an unsaved "<name> custom".
 */
#ifndef SHADER_PROFILE_EDIT_H
#define SHADER_PROFILE_EDIT_H

#include <stddef.h>

#include "shader_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SHADER_EDIT_OFF,        /* stock present */
    SHADER_EDIT_BUILTIN,    /* a bundled preset (present.shader) */
    SHADER_EDIT_PATH,       /* a .slangp path from the config (present.shader) */
    SHADER_EDIT_USER        /* a profile file (present.profile) */
};
/* An entry's state: SHADER_PROFILE_OK, _BROKEN, _NEWER, or MISSING (the
 * start chose a profile that has no file). Only OK is chosen by the picker. */
#define SHADER_EDIT_MISSING 2

struct shader_edit_entry {
    int kind, state;
    char name[SHADER_PROFILE_NAME_MAX + 1];
    /* The store's key: a profile's slug (its next file name), a bundled
     * preset's name, a path's preset key. */
    char slug[SHADER_PROFILE_NAME_MAX + 1];
    char file[SHADER_PROFILE_NAME_MAX + 1];    /* the slug on disk; "" unsaved */
    char err[SHADER_PROFILE_ERR_MAX];          /* broken, newer, missing: why */
    int nstages;
    char stage[SHADER_PROFILE_STAGES_MAX][SHADER_PROFILE_REF_MAX];
    /* The file's name and stages, as last read or written. */
    char fname[SHADER_PROFILE_NAME_MAX + 1];
    int fnstages;
    char fstage[SHADER_PROFILE_STAGES_MAX][SHADER_PROFILE_REF_MAX];
    int loaded;             /* its values are in the store */
};

/* Read the profiles folder again (the Shaders page calls it on each
 * entry, so nothing is read before the page opens): new files appear,
 * removed ones go, and an entry with edits is kept as it is. The first
 * call also takes the choice the start made. */
void shader_edit_refresh(void);

int shader_edit_count(void);
const struct shader_edit_entry *shader_edit_entry(int i);
int shader_edit_active(void);   /* the chosen entry; 0 (Off) before refresh */

/* The environment variable that holds the choice (RECOMP_PRESENT_SHADER or
 * RECOMP_PRESENT_PROFILE), which makes the picker read only; NULL else. */
const char *shader_edit_forced(void);

/* Choose entry i (live). step: the next choosable entry by dir (+1, -1),
 * wrapping, skipping a broken, newer or missing profile. 0, or -1 with the
 * reason in msg. */
int shader_edit_choose(int i, char *msg, size_t n);
int shader_edit_step(int dir, char *msg, size_t n);

/* The chosen chain, as the last change found it: info's passes and each
 * stage's flags, 1 when it composes; 0 with why in err (a stage missing,
 * a clash, a limit). missing: stage s's preset was not found. names: the
 * parameters stage s declares (n set to the count). */
int shader_edit_info(struct shader_profile_compose *info, char *err, size_t errlen);
int shader_edit_stage_missing(int s);
const char (*shader_edit_stage_names(int s, int *n))[64];

/* Chain edits on the chosen entry, each live; a bundled preset or a path
 * forks first. 0, or -1 with the reason in msg. move: stage s one place by
 * dir (at an end nothing, 0). remove: the last stage stays. add: ref (a
 * bundled name or "user:<stem>") as the last stage, after it is found,
 * parses and composes with the chain. */
int shader_edit_move(int s, int dir, char *msg, size_t n);
int shader_edit_remove(int s, char *msg, size_t n);
int shader_edit_add(const char *ref, char *msg, size_t n);
/* 1 when Add is refused before any preset is tried (8 stages, 16 passes),
 * with the limit in msg. */
int shader_edit_add_full(char *msg, size_t n);

/* The presets Add offers: the bundled names, then "user:<stem>" for each
 * .slangp in <root>/shaders. Read on each call. The count. */
int shader_edit_presets(char (*ref)[SHADER_PROFILE_REF_MAX], int cap);

/* Profile actions. new: a new unsaved profile of the chosen chain and its
 * values, named "Profile N" (copy 0) or "<name> copy" (copy 1), chosen.
 * rename: the chosen profile (a user one) takes name; its file is renamed
 * at Save. del: the chosen profile's file goes now, and present.profile
 * with it when the user config names it; the picker moves to Off. */
int shader_edit_new(int copy, char *msg, size_t n);
int shader_edit_rename(const char *name, char *msg, size_t n);
int shader_edit_delete(char *msg, size_t n);

/* Reset the chosen entry's values: every stage's, or those stage s owns
 * (the names no earlier stage declares). */
void shader_edit_reset_values(void);
void shader_edit_reset_stage(int s);

/* Entry i differs from its file (a profile's chain, name or values; a
 * bundled preset's overrides), and the menu's unsaved count for all of it
 * (the choice included). */
int shader_edit_dirty(int i);
int shader_edit_unsaved(void);

/* Save. config: the keys and values (NULL: remove) the choice needs in the
 * user config, design D5 ("none" where a lower layer sets a key the choice
 * needs gone); config_saved after they were written. save: every profile
 * with changes to its file; the count written, or -1 with msg. */
int shader_edit_config(const char **keys, const char **vals, int max);
void shader_edit_config_saved(void);
int shader_edit_save(char *msg, size_t n);

/* The Save page's Reset to defaults: the picker to Off. Files stay. */
void shader_edit_reset_defaults(void);

/* The menu's "Exit without saving" (overlay-revamp D9a): the entries, their
 * chains, names and values, and the choice back to what the files and the
 * config hold. An unsaved profile goes; a deleted one stays deleted. Then
 * the list is read again and the choice requested, as a reload does.
 * 1 when the session had deleted a profile (Revert cannot bring it back). */
int shader_edit_revert(void);
int shader_edit_deleted(void);
/* 1 when the picker's choice differs from the config's (the Profile row's
 * unsaved mark), 0 under an environment override. */
int shader_edit_choice_dirty(void);

void shader_edit_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif
