/*
 * saves_store.h - the Saves page's file work (openspec change
 * overlay-saves, D5 and D6): list the title's saves, export the two save
 * trees as a saves zip, and import one: stage the zip's trees beside the
 * live ones, then swap them in with renames, rolling back on a failure.
 *
 * No kernel calls: the menu passes the host roots it got from the path
 * layer (xbox_path_save_root) and the busy check, so the ctest
 * (tests/saves_zip) runs it on a temp folder. Paths are UTF-8; Windows
 * converts at each call, as file_browser.c does.
 */
#ifndef SAVES_STORE_H
#define SAVES_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "file_browser.h"
#include "saves_zip.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The two save roots: root[0] the host folder behind UDATA, root[1] the
 * one behind TDATA (each holding <tid>/), and the running title. */
struct saves_paths {
    char root[2][FB_PATH_MAX];
    uint32_t tid;
};

#define SAVES_LIST_MAX 12

struct saves_item {
    char id[SZ_NAME_MAX + 1];   /* the save's folder, UDATA/<tid>/<id> */
    char name[96];              /* SaveMeta.xbx Name=, or the id */
    uint64_t bytes;
    int64_t mtime;
};

struct saves_info {
    int saves;                  /* save folders in all */
    int n;                      /* listed in item, at most SAVES_LIST_MAX */
    struct saves_item item[SAVES_LIST_MAX];
    int files;                  /* files in both trees */
    uint64_t bytes;
    int64_t mtime;              /* the newest file */
};

/* The live saves. Missing folders are no saves. 0, or -1 when a folder
 * could not be read. */
int saves_scan(const struct saves_paths *p, struct saves_info *out);
/* What an archive holds, in the same terms, for the import preview. */
void saves_scan_archive(const struct sz_archive *a, uint32_t tid, struct saves_info *out);

/* The live trees as a saves zip, its manifest from base (title, created,
 * keys, toolkit; files, bytes and title_id are filled here). The file
 * count (0: there are no saves, *zip is NULL), or -1 with err set. Links
 * and special files are left out. */
int saves_export(const struct saves_paths *p, const struct saves_manifest *base,
                 unsigned char **zip, size_t *len, char *err, size_t errn);

/* Replace the trees a checked archive carries (trees, from
 * saves_zip_check). Each is written to a new folder
 * .saves-import-<stamp>-<pid>-<n>-<TREE> beside its root (made exclusively,
 * so a retry never reuses one), then busy(ctx) is asked again (non-zero
 * stops the import), then each live <root>/<tid> is renamed aside and the
 * staged one renamed in; a failed rename puts back every tree already
 * swapped. The old trees are removed last.
 *
 * 0, or -1 with err set. The live saves are then as they were, unless err
 * says the undo failed too: then it names the tree that is not, and the
 * stage folder keeping that tree's old saves is left in place.
 *
 * The stage sits in the folder that holds the roots (the hdd root, which
 * the title can list), not above it: a rename stays on one volume only
 * there, and the import quits the title before it could look. */
int saves_import(const struct saves_paths *p, const struct sz_archive *a, int trees,
                 int (*busy)(void *ctx), void *ctx, char *err, size_t errn);

/* For the ctest: fail(from, to) is asked before each rename of a save
 * tree and fails it when non-zero. NULL, the default, never fails. */
void saves_store_rename_hook(int (*fail)(const char *from, const char *to));

#ifdef __cplusplus
}
#endif

#endif /* SAVES_STORE_H */
