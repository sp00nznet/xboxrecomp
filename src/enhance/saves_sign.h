/*
 * saves_sign.h - re-signing imported saves under this build's keys
 * (openspec change pad-recording-ui, R6 save safeguard 2): the C port of
 * xboxrecomp-cli's save-import resign (saves/importer.py, recipes_for and
 * resign), for the menu's Saves page and the bug-report bundle.
 *
 * A saves zip whose manifest names other console keys than this build's is
 * no longer refused. Each save in it is re-signed by the game's recipes
 * (game.toml [[saves.signed]], which the CLI writes beside the exe as
 * saves-signing.toml) or skipped with its reason. The recipes are read
 * only from beside the exe, never from a zip.
 *
 * Pure but for the HMAC (xbox_XcHMAC, kernel_crypto.c): no file calls, so
 * the ctest (tests/saves_zip) drives every refusal on buffers.
 */
#ifndef SAVES_SIGN_H
#define SAVES_SIGN_H

#include <stddef.h>
#include <stdint.h>

#include "saves_zip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SAVES_SIGN_FILE      "saves-signing.toml"
#define SAVES_SIGN_FILE_MAX  (64u << 10)
#define SAVES_SIGN_MAX       64     /* recipes, as manifest.py caps them */
#define SAVES_SIGN_GLOB_MAX  128    /* bytes of one glob */
#define SAVES_SIGN_DIGEST    20     /* HMAC-SHA1 */
#define SAVES_SIGN_FORMAT    1

/* [saves] signing: what a save of this game needs under a keys mismatch. */
enum saves_signing_mode {
    SAVES_SIGNING_UNKNOWN = 0,  /* no file, or no signing key: skip them all */
    SAVES_SIGNING_RESIGN,       /* re-sign by the recipes */
    SAVES_SIGNING_NONE,         /* unsigned: every save imports as it is */
};

struct saves_recipe {
    char glob[SAVES_SIGN_GLOB_MAX + 1];   /* below UDATA/<tid>/, any case */
    uint32_t size;                        /* the file's exact size */
    uint32_t start, end;                  /* the signed range [start, end) */
    uint32_t digest;                      /* where the 20-byte HMAC goes */
    int roamable;                         /* key = "roamable" */
};

struct saves_signing {
    int mode;                             /* enum saves_signing_mode */
    int n;
    struct saves_recipe r[SAVES_SIGN_MAX];
};

/* Parse saves-signing.toml. Every bound is checked here, before any
 * write: at most SAVES_SIGN_MAX recipes, a glob of 1..SAVES_SIGN_GLOB_MAX
 * bytes, 0 <= start < end <= size, digest + 20 <= size, each digest
 * outside every data range of its glob and clear of its other digests, and
 * one size per glob. 0 with *s filled; -1 with err set and *s left as no
 * recipes (mode UNKNOWN), so a bad file skips every signed save. */
int saves_signing_parse(const char *text, size_t len, struct saves_signing *s, char *err,
                        size_t errn);

/* fnmatch over a lower-cased name, as Python's fnmatchcase on lower-cased
 * text: '*' matches any run ('/' too), '?' one byte, [seq] and [!seq] a
 * set; an unclosed '[' is a literal. 1 on a match. */
int saves_glob_match(const char *glob, const char *name);

/* The recipes whose glob matches rel ("save id/file" below UDATA/<tid>/),
 * as indices into s->r in order. Their count. */
int saves_recipes_for(const struct saves_signing *s, const char *rel, int *idx, int max);

/* Re-sign buf in place under the zero key: each recipe's HMAC-SHA1 over
 * its data range written at its digest, in order, as importer.py resign
 * does. -1 and nothing written when len is not every recipe's size. */
int saves_resign(unsigned char *buf, size_t len, const struct saves_signing *s, const int *idx,
                 int n);

/* ---- a saves zip under a keys mismatch ---- */

#define SAVES_RESIGN_MAX_SAVES 64      /* saves the report names */

/* The plain words a player reads (R6): the import result, the bundle and
 * the CLI's report say the same. */
#define SAVES_SAID_RESIGNED "re-signed for this build; it may not load on the console it came from"
#define SAVES_SAID_SKIPPED  "skipped: this game's non-roamable (console-locked) saves aren't " \
                            "supported yet"
#define SAVES_SAID_MARK     "re-signed (keys differed)"
#define SAVES_SAID_WITHOUT  "the replay runs without the skipped saves"

enum saves_verdict {
    SAVES_KEPT = 0,       /* unsigned game: imported as it is */
    SAVES_RESIGNED,
    SAVES_SKIPPED,
};

struct saves_resign_save {
    char sid[SZ_NAME_MAX + 1];  /* the save folder's name */
    int verdict;                /* enum saves_verdict */
    char why[96];               /* a skip's reason, for the log */
};

struct saves_resign_report {
    int n;                      /* saves seen (the first MAX are named) */
    int resigned, skipped, kept;
    struct saves_resign_save s[SAVES_RESIGN_MAX_SAVES];
};

/* Under a keys mismatch: a new archive with the title's saves re-signed
 * by s, the saves that cannot be dropped, and TDATA and the title folder's
 * own files as they are. The re-signed bytes live in *blob (free it after
 * *out; *out's other entries still point into in's zip buffer). *trees is
 * in's trees less UDATA when no save is left, so an import never wipes the
 * live saves for nothing. 0 with *out, *blob, *trees and *rep set; -1 with
 * err set on an allocation failure, or when nothing is left to import. */
int saves_resign_archive(const struct sz_archive *in, uint32_t tid, int trees,
                         const struct saves_signing *s, struct sz_archive *out,
                         unsigned char **blob, int *out_trees, struct saves_resign_report *rep,
                         char *err, size_t errn);

/* The most saves_resign_lines writes, every save named with its longest
 * line (a skip with its reason), "and N more" and the closing line. */
#define SAVES_RESIGN_LINE_MAX                                                            \
    (SZ_NAME_MAX + 2 + sizeof SAVES_SAID_SKIPPED + 3 + 96 + sizeof SAVES_SAID_RESIGNED)
#define SAVES_RESIGN_LINES_MAX                                                           \
    (SAVES_RESIGN_MAX_SAVES * SAVES_RESIGN_LINE_MAX + 32 + sizeof SAVES_SAID_WITHOUT + 1)

/* One line per save into out, as the Saves page and the log show them:
 * "<sid>: re-signed for this build; ... [re-signed (keys differed)]" or
 * "<sid>: skipped: ...", then SAVES_SAID_WITHOUT when any was skipped.
 * with_why adds a skip's reason in parentheses (the log). Its length; a
 * buffer short of SAVES_RESIGN_LINES_MAX may lose whole lines at the end. */
size_t saves_resign_lines(const struct saves_resign_report *rep, int with_why, char *out,
                          size_t n);

#ifdef __cplusplus
}
#endif

#endif /* SAVES_SIGN_H */
