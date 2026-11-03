/*
 * saves_zip.h - the saves zip of the menu's Saves page (openspec change
 * overlay-saves, D2 and D3): a stored (uncompressed) zip writer, a reader
 * that validates every byte it trusts, the manifest, and the save names in
 * SaveMeta.xbx. Pure: no file calls, so the ctest (tests/saves_zip) drives
 * every refusal on buffers.
 *
 * A saves zip holds xbr-saves.toml and the title's two trees as the host
 * save dir lays them out:
 *     xbr-saves.toml
 *     hdd/UDATA/<tid>/...      the saves (UDATA/<tid> on the console)
 *     hdd/TDATA/<tid>/...      the title's own data (TDATA/<tid>)
 * <tid> is the title id in 8 lower-case hex digits.
 *
 * The reader refuses the whole zip on the first problem, with a message
 * for the player (err). Nothing it returns points outside the buffer.
 */
#ifndef SAVES_ZIP_H
#define SAVES_ZIP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caps (D3). A real title's saves are kilobytes; these leave room for a
 * title that keeps a large TDATA and still bound what a crafted zip can
 * make the menu allocate. */
#define SZ_MAX_ZIP     (64u << 20)
#define SZ_MAX_ENTRIES 4096
#define SZ_MAX_FILE    (16u << 20)
#define SZ_MAX_TOTAL   (64u << 20)
#define SZ_MAX_DEPTH   8          /* components, "hdd/UDATA/<tid>" included */
#define SZ_NAME_MAX    42         /* a FATX name's bytes */
#define SZ_PATH_MAX    384
#define SZ_ERR_MAX     512
#define SZ_MANIFEST    "xbr-saves.toml"
#define SZ_MANIFEST_MAX 4096

uint32_t sz_crc32(uint32_t crc, const void *data, size_t len);

/* ---- names ---- */

/* 0 when path is a name the zip may hold (D3, the rules save-import
 * shares): '/'-separated components, each 1..SZ_NAME_MAX bytes of UTF-8,
 * no control character or \ : * ? " < > |, not "." or "..", no trailing
 * dot or space, not a Windows device name; no leading '/', at most
 * SZ_MAX_DEPTH components. A trailing '/' (a directory) is allowed. -1
 * with *why set otherwise. */
int sz_name_ok(const char *path, const char **why);

/* ---- writer ---- */

struct sz_writer {
    unsigned char *buf, *cd;
    size_t len, cap, cd_len, cd_cap;
    int count;
    int err;            /* sticky: an allocation or a cap failed */
};

void sz_writer_init(struct sz_writer *w);
void sz_writer_free(struct sz_writer *w);
/* A directory entry; name ends with '/'. mtime is Unix seconds. */
int sz_add_dir(struct sz_writer *w, const char *name, int64_t mtime);
int sz_add_file(struct sz_writer *w, const char *name, const void *data, size_t len,
                int64_t mtime);
/* Append the central directory and hand the zip over (*out is malloc'd,
 * the writer is emptied). 0, or -1 when any add failed. */
int sz_finish(struct sz_writer *w, unsigned char **out, size_t *len);

/* ---- reader ---- */

struct sz_entry {
    char name[SZ_PATH_MAX];
    const unsigned char *data;  /* into the zip buffer */
    uint32_t size;
    uint32_t crc;
    int is_dir;
    int64_t mtime;              /* Unix seconds */
};

struct sz_archive {
    struct sz_entry *e;
    int n;
};

/* Parse and check a whole zip (D3 structure, names, types, caps, CRCs).
 * 0 with *a filled (free it with sz_archive_free), -1 with err set. */
int sz_read(const void *zip, size_t len, struct sz_archive *a, char *err, size_t errn);
void sz_archive_free(struct sz_archive *a);
const struct sz_entry *sz_find(const struct sz_archive *a, const char *name);

/* ---- the manifest ---- */

struct saves_manifest {
    int format;               /* 1 */
    char kind[16];            /* "saves" */
    uint32_t title_id;
    char title[96];
    char created[32];         /* ISO 8601 UTC */
    char keys[48];            /* "zero", or a key fingerprint (D1) */
    int files;
    uint64_t bytes;
    char toolkit[64];
};

#define SAVES_FORMAT 1

/* The manifest as TOML into out. Its length, or -1 when it does not fit. */
int saves_manifest_write(const struct saves_manifest *m, char *out, size_t n);
/* Parse one. 0, or -1 with *why. Unknown keys are ignored (a later format
 * may add some); a missing required key is refused. */
int saves_manifest_parse(const char *text, size_t len, struct saves_manifest *m,
                         const char **why);

/* ---- a saves zip for the running title ---- */

/* Which of the two trees an archive carries: bit 0 UDATA, bit 1 TDATA. */
#define SAVES_TREE_UDATA 1
#define SAVES_TREE_TDATA 2

/* Validate a read archive as a saves zip for title tid built with the
 * running keys fingerprint (D3 content rules): the manifest, its title,
 * kind, format, keys and counts; every other entry under the title's two
 * trees. 0 with *m and *trees set, -1 with err set. */
int saves_zip_check(const struct sz_archive *a, uint32_t tid, const char *keys,
                    struct saves_manifest *m, int *trees, char *err, size_t errn);

/* The part of an entry name below "hdd/<TREE>/<tid>/" ("" for the tid
 * folder itself), and its tree; NULL when the name is not under one. */
const char *saves_zip_rest(const char *name, uint32_t tid, int *tree);

/* The Name= line of a SaveMeta.xbx (UTF-16LE, optional BOM) as UTF-8. 0,
 * or -1 when it has none. */
int saves_meta_name(const unsigned char *data, size_t len, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* SAVES_ZIP_H */
