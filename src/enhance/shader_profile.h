/*
 * shader_profile.h - shader profiles: a named chain of 1 to 8 slang
 * presets, kept as <root>/config/shader-profiles/<slug>.toml and run by the
 * present chain (nv2a_present_chain.h) as one composed preset.
 *
 * librashader 0.12 builds a chain only from a preset file, and a preset can
 * #reference only one other, so a chain of presets cannot be a wrapper: a
 * profile of two or more stages is composed here into one .slangp (passes
 * renumbered, paths made absolute, the presets' parameter values merged),
 * written under the shader cache and requested as a path. A one-stage
 * profile is requested as its preset, never composed, so a built-in profile
 * is exactly today's present.shader.
 *
 * No GPU and no library here: parsing, writing and composing are file and
 * text work, so the menu and the ctest run them anywhere. The file format
 * is in docs/runtime/config.md, "Shader profiles".
 */
#ifndef SHADER_PROFILE_H
#define SHADER_PROFILE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHADER_PROFILE_VERSION 1
#define SHADER_PROFILE_STAGES_MAX 8
#define SHADER_PROFILE_PASSES_MAX 16    /* in a composed preset */
#define SHADER_PROFILE_LIST_MAX 64      /* profiles the menu lists */
#define SHADER_PROFILE_NAME_MAX 48      /* bytes of UTF-8, without the NUL */
#define SHADER_PROFILE_VALUES_MAX 256   /* parameter values over every stage */
#define SHADER_PROFILE_REF_MAX 260      /* a stage reference or a path */
#define SHADER_PROFILE_ERR_MAX 160

/* parse and load results */
#define SHADER_PROFILE_OK 0
#define SHADER_PROFILE_BROKEN (-1)      /* does not parse: listed, never written */
#define SHADER_PROFILE_NEWER 1          /* a later format: listed, never written */

struct shader_profile_value {
    char key[64];    /* the stage's preset key (nv2a_present_chain_preset_key) */
    char name[64];   /* the parameter */
    float v;
};

struct shader_profile {
    char name[SHADER_PROFILE_NAME_MAX + 1];
    char slug[SHADER_PROFILE_NAME_MAX + 1];
    int version;
    int nstages;
    char stage[SHADER_PROFILE_STAGES_MAX][SHADER_PROFILE_REF_MAX];
    /* `lines = "auto" | "guest" | "host"`: how the chain is fed
     * (NV2A_PRESENT_LINES_*, present.shader_lines); auto (0) when absent.
     * Read, never written: Save keeps the line as any key it does not own. */
    int lines;
    /* Differences from the composed preset's values, one per (key, name). */
    int nvalues;
    struct shader_profile_value value[SHADER_PROFILE_VALUES_MAX];
};

/* ---- names and slugs ---- */

/* The slug of a display name: ASCII letters lowered, digits, '-' and '_'
 * kept, any other run of bytes (spaces, punctuation, UTF-8) one '-', no
 * '-' at either end. 0 when nothing is left (the name has no ASCII letter
 * or digit). */
int shader_profile_slug(const char *name, char *out, size_t cap);

/* 1 when s is a slug as shader_profile_slug makes them, 1 to
 * SHADER_PROFILE_NAME_MAX bytes, and not "~unsaved". */
int shader_profile_slug_ok(const char *s);

/* 1 when name is a display name a profile may take: 1 to
 * SHADER_PROFILE_NAME_MAX bytes of valid UTF-8, no control character, and a
 * slug can be made of it. */
int shader_profile_name_ok(const char *name);

/* The slug of name that none of taken[0..ntaken) holds: the plain slug, or
 * "<slug>-2", "-3" ... ("profile" when name has no slug). */
void shader_profile_slug_free(const char *name, const char *const *taken, int ntaken,
                              char *out, size_t cap);

/* ---- the file ---- */

/* Parse a profile's text. file names it in messages; slug is its file stem
 * (the name when the text has none). SHADER_PROFILE_OK, _NEWER (version
 * above SHADER_PROFILE_VERSION; name and stages read as far as they go) or
 * _BROKEN with err set. Unknown keys and tables are logged once each
 * ("[SHADER] <file>:<line>: unknown key") and left alone; a [params] value
 * that is not a number is logged and skipped; past 8 stages the first 8
 * are taken, with one line. */
int shader_profile_parse(const char *text, const char *file, const char *slug,
                         struct shader_profile *p, char *err, size_t errlen);

/* The same on a file; the slug is the file's stem. */
int shader_profile_load(const char *path, struct shader_profile *p, char *err, size_t errlen);

/* Write p to path: version, name, stages and its values, and the
 * [params] keys the file has and p lacks removed. Every other line, comment
 * and unknown key stays (recomp_cfg_patch); a new file starts with a
 * comment header. The folder is created when missing. A file that is there
 * and does not parse, or is of a newer version, is never written. 0, or -1
 * with err set. */
int shader_profile_save(const char *path, const struct shader_profile *p, char *err,
                        size_t errlen);

/* The profiles folder, <user dir>/shader-profiles (enhance_cfg_user_dir).
 * 0 under RECOMP_CONFIG=none: there are no profiles. */
int shader_profile_dir(char *out, size_t cap);

/* <dir>/<slug>.toml into out. */
void shader_profile_path(const char *dir, const char *slug, char *out, size_t cap);

/* Rename <dir>/<from>.toml to <dir>/<to>.toml; refused when <to> exists.
 * Delete removes <dir>/<slug>.toml. 0, or -1 with err set. */
int shader_profile_rename(const char *dir, const char *from, const char *to, char *err,
                          size_t errlen);
int shader_profile_delete(const char *dir, const char *slug, char *err, size_t errlen);

/* One row of the folder's list. state: SHADER_PROFILE_OK, _BROKEN or
 * _NEWER; err holds a broken file's message. */
struct shader_profile_entry {
    char name[SHADER_PROFILE_NAME_MAX + 1];
    char slug[SHADER_PROFILE_NAME_MAX + 1];
    int state;
    int nstages;
    char stage[SHADER_PROFILE_STAGES_MAX][SHADER_PROFILE_REF_MAX];
    char err[SHADER_PROFILE_ERR_MAX];
};

/* The folder's *.toml (one level), sorted by name, at most cap (one line
 * when there are more). Each file is parsed for its name, stages and
 * state. The count; 0 for a missing folder. */
int shader_profile_list(const char *dir, struct shader_profile_entry *out, int cap);

/* The .slangp stems in dir (one level, sorted), for the Add picker's user
 * presets; a stem that is a bundled name is left out with one line. The
 * count. */
int shader_profile_list_presets(const char *dir, char (*stem)[64], int cap);

/* ---- values ---- */

/* The value of (key, name): 1 with *v, 0 when p holds none. */
int shader_profile_value_get(const struct shader_profile *p, const char *key,
                             const char *name, float *v);
/* Set (key, name) to v. 0 when the table is full. */
int shader_profile_value_set(struct shader_profile *p, const char *key, const char *name,
                             float v);
/* Remove (key, name); name NULL: every value of key. */
void shader_profile_value_remove(struct shader_profile *p, const char *key, const char *name);

/* What each stage's preset declares (nv2a_present_chain_describe), for the
 * shared-value rule: one value per name across the chain, saved under the
 * preset key of its owner, the first stage that declares it. */
struct shader_profile_stage_params {
    int n;
    const char *const *name;
};

/* The first stage of p that declares name, or -1. */
int shader_profile_owner(const struct shader_profile *p,
                         const struct shader_profile_stage_params *sp, const char *name);

/* Re-home p's values under their owners: for each name a stage declares,
 * the owner's table's value wins, else the first other stage's that sets
 * it, in stage order; the other copies are removed (one line when two
 * disagree), so a reorder that changes the owner keeps the value and Save
 * never writes two values for one name. Values of names no stage declares
 * stay as they are. file names the profile in the line. */
void shader_profile_rehome(struct shader_profile *p, const struct shader_profile_stage_params *sp,
                           const char *file);

/* ---- stages ---- */

/* The bundled preset index of ref (NV2A_PRESENT_CHAIN_NAMES), or -1. */
int shader_profile_bundled(const char *ref);

/* The preset key of a stage reference: "user:<stem>" keys as <stem>, any
 * other as nv2a_present_chain_preset_key. */
void shader_profile_stage_key(const char *ref, char *out, size_t cap);

/* The preset file of a stage reference, in this order:
 *   1. a bundled name: <exe_dir>/shaders/<name>.slangp (on Apple also
 *      <exe_dir>/../Resources/shaders/);
 *   2. "user:<stem>": <root>/shaders/<stem>.slangp;
 *   3. an absolute path, as is;
 *   4. a relative path: <root>/shaders/<ref>, then <exe_dir>/<ref>.
 * A file under <root>/shaders/ whose stem is a bundled name is refused, so
 * "fsr" in a profile always means the bundled one. exe_dir NULL:
 * recomp_exe_dir(). 1 found, -1 with err set. */
int shader_profile_stage_path(const char *ref, const char *root, const char *exe_dir,
                              char *out, size_t cap, char *err, size_t errlen);

/* ---- composing ---- */

/* Stage flags, for the menu's notes. */
#define SHADER_PROFILE_SCALED 1u     /* sees the scaled picture: an earlier
                                        stage's output is not source-sized */
#define SHADER_PROFILE_ORIGINAL 2u   /* a later stage reads Original*, the
                                        game frame: may look wrong */
#define SHADER_PROFILE_NO_FEEDBACK 4u   /* its feedback_pass was dropped: the
                                           chain has no such key per stage */

struct shader_profile_compose {
    int passes;
    int nstages;
    struct {
        int first_pass, passes;
        unsigned flags;
    } stage[SHADER_PROFILE_STAGES_MAX];
};

/* Compose the presets paths[0..n) into one preset's text (free() it), or
 * NULL with err set and one "[SHADER] profile <slug>: ..." line. labels[i]
 * names stage i in messages (the reference; NULL: the path); info (may be
 * NULL) gets the passes and each stage's flags. Refused: a missing or
 * unreadable preset, a #reference cycle or one deeper than 16, an alias or
 * a texture name two stages declare, PassOutputN / PassFeedbackN (and their
 * Size) read, not only declared, in a stage after the first, more than
 * SHADER_PROFILE_PASSES_MAX
 * passes. n may be 1 (the Add picker's check). */
char *shader_profile_compose_text(const char *const *paths, const char *const *labels, int n,
                                  const char *slug, struct shader_profile_compose *info,
                                  char *err, size_t errlen);

/* The same, written to out_path through "<out_path>.tmp" and a rename, its
 * folder created. 0, or -1 with err set. */
int shader_profile_compose(const char *const *paths, const char *const *labels, int n,
                           const char *slug, const char *out_path,
                           struct shader_profile_compose *info, char *err, size_t errlen);

/* <cache_root>/slangp/<slug>.slangp ("~unsaved" for a NULL or empty slug),
 * the composed file of a profile. cache_root: xbox_path_shader_cache. */
void shader_profile_cache_path(const char *cache_root, const char *slug, char *out, size_t cap);

/* base joined with rel (rel as is when absolute), win: '\' read as a
 * separator and written '/' (the Windows host; a test passes 1 on any
 * host). The composer writes every path this way. */
void shader_profile_path_join(const char *base, const char *rel, int win, char *out,
                              size_t cap);

/* ---- requesting (shader_profile_req.c) ---- */

/* The user data root for user: stages (xbox_enhance_init's root; "." for
 * NULL), and the one in use. */
void shader_profile_set_root(const char *root);
const char *shader_profile_root(void);

/* Request p as the present chain: each stage's preset found
 * (shader_profile_stage_path) and described for its parameter names, a
 * chain of two or more composed to shader_profile_cache_path under
 * xbox_path_shader_cache, p's values re-homed (shader_profile_rehome) and
 * loaded into the profile scope, then nv2a_present_chain_request_profile.
 * A failure requests stock, sets the profile error (the menu's red Chain
 * header) and logs one line. 0, or -1 with err (may be NULL) set. */
int shader_profile_request(const struct shader_profile *p, char *err, size_t errlen);

/* The same with flags, for the menu's requests after an edit:
 *   KEEP     the store's values of p's slug are left as they are (no
 *            re-home, no forget, p's values unused): the player's edits
 *            since the last Save live there;
 *   UNSAVED  a chain composes to the cache's "~unsaved" file: the profile
 *            has no file yet, and its slug may still change. */
#define SHADER_PROFILE_REQ_KEEP 1u
#define SHADER_PROFILE_REQ_UNSAVED 2u
int shader_profile_request_with(const struct shader_profile *p, unsigned flags, char *err,
                                size_t errlen);

/* Before Save writes p: each stage described for the names it declares
 * and p's values re-homed under their owners (shader_profile_rehome). 0,
 * or -1 when a stage could not be found or described (its names are then
 * unknown and its values stay where they are). */
int shader_profile_place(struct shader_profile *p);

/* Composed-file cleanup. renamed: from's file stays while the chain may run
 * it and goes at the next request of another file (a profile's, or sweep's
 * caller asked for stock or a plain preset). forget: slug's file goes now
 * (Delete; the chain is asked for stock in the same step). */
void shader_profile_cache_renamed(const char *from, const char *to);
void shader_profile_cache_sweep(void);
void shader_profile_cache_forget(const char *slug);

/* The slug the start chose (present.profile or RECOMP_PRESENT_PROFILE),
 * kept when it failed so the menu can show it with its error: 1 with it in
 * out, 0 with "" when the start chose no profile. */
int shader_profile_start_slug(char *out, size_t cap);

/* 1 once, with the start's slug in out, when its deferred request ran and
 * put the profile's values in the store; 0 before that, after, and when a
 * menu request came first (that request then loaded them itself). */
int shader_profile_start_loaded(char *out, size_t cap);

/* The start's choice (design D7), made from xbox_enhance_init and
 * requested:
 *   1. RECOMP_PRESENT_SHADER: that preset ("present.profile ignored" when
 *      a profile is set too);
 *   2. RECOMP_PRESENT_PROFILE, not "none": that profile;
 *   3. the highest config layer (title, user, defaults) that sets either
 *      present.profile or present.shader, alone: its profile unless none,
 *      else its shader unless none, else stock;
 *   4. stock.
 * A chosen profile that is missing, broken or of a newer format gives
 * stock and the profile error, never the layer's present.shader. Only that
 * one profile file is read, and none at a stock start. The request itself
 * (a chain's compose, and the stage descriptions that load librashader)
 * waits for the first present (nv2a_present_chain_request_pending): the
 * shader cache is set up after this call. A menu request made before it
 * runs wins. Returns 0 stock, 1 a
 * present.shader request (its value in shader), 2 a profile. */
int shader_profile_start(const char *root, char *shader, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* SHADER_PROFILE_H */
