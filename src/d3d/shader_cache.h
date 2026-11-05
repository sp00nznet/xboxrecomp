/**
 * shader_cache.h - an on-disk cache of compiled shader bytecode.
 *
 * The D3D11 backend compiles its HLSL at runtime with D3DCompile, which
 * under Proton is Wine's d3dcompiler_47 and costs tens of ms a shader. A
 * title's first scene can ask for a few dozen at once, so the wait into its
 * first frame is mostly compiling. This keeps each compiled blob in a file
 * and hands it back the next time the same source is compiled.
 *
 * An entry's key is everything the compiler's output depends on: the
 * compiler's identity (the caller's string, e.g. the DLL's path, size and
 * time), the target, the entry point, both flag words and the source text.
 * The file name is a 64-bit hash of the key, and the file holds the whole
 * key too: a load compares it byte for byte, so a hash collision is a miss,
 * never another shader's bytecode. A hit is therefore the exact bytes the
 * same compiler produced for the same input.
 *
 * Invalidation is by key: a new compiler, source or flag set names a new
 * entry, and an entry from an older file format is removed on load. The
 * entries no key names any more are left to shader_cache_prune's cap. An
 * entry that fails its checks (truncated, size or code hash wrong) is
 * removed and reported as a miss, so the caller compiles. A store writes a
 * temporary file and renames it over the entry, so a reader never sees a
 * half-written one and two processes storing the same key both succeed.
 *
 * Portable C11: no D3D here, so it builds (and is tested) on every host.
 * Paths are UTF-8; on Windows they go through the wide-char file APIs.
 */
#ifndef SHADER_CACHE_H
#define SHADER_CACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped when the entry layout changes; older entries are then removed. */
#define SHADER_CACHE_FORMAT 1u

/* Neither a key nor an entry's code may be larger (a sanity bound on what a
 * damaged header can make a load allocate). */
#define SHADER_CACHE_MAX_PART (16u << 20)

/* What a cache directory may hold before shader_cache_prune trims it. A
 * game's whole set is a few hundred entries of a few KB; the cap only
 * matters for entries no key names any more (an old compiler's). */
#define SHADER_CACHE_DEFAULT_CAP (64ull << 20)

typedef struct {
    const char *compiler;   /* the compiler's identity; "" when unknown */
    const char *target;     /* "vs_5_0" */
    const char *entry;      /* "main" */
    uint32_t    flags1, flags2;
    const char *src;
    size_t      src_len;
} shader_cache_key;

typedef struct {
    char     dir[1024];     /* "" = off */
    unsigned seq;           /* temporary-file names */
    /* Counts, for the caller's log line. */
    unsigned hits, misses, stores, corrupt, store_fails, pruned;
} shader_cache;

/* What a RECOMP_DXBC_CACHE value asks for. The switch words follow
 * recomp_env_on: set but empty, or starting with '0', is off; so are
 * "off", "false" and "no" in any case. Unset, "1", "on", "true" and "yes"
 * are the default directory. Anything else is a directory, which must be
 * absolute (a drive or UNC path on Windows, '/' elsewhere): a relative one
 * would land wherever the game was started from, so it turns the cache off
 * rather than guessing. */
typedef enum {
    SHADER_CACHE_ENV_DEFAULT,
    SHADER_CACHE_ENV_OFF,
    SHADER_CACHE_ENV_DIR,
    SHADER_CACHE_ENV_RELATIVE
} shader_cache_env_kind;
shader_cache_env_kind shader_cache_env(const char *value);

/* Opens (and creates) dir and checks a file can be written there. 0, with
 * the cache off, when dir is NULL, empty, too long, cannot be created or
 * cannot be written. */
int shader_cache_init(shader_cache *c, const char *dir);

/* 64-bit FNV-1a of the key: the entry's file name. */
uint64_t shader_cache_key_hash(const shader_cache_key *k);

/* The entry's path ("<dir>/<16 hex digits>.bin"). 0 if it does not fit. */
int shader_cache_path(const shader_cache *c, const shader_cache_key *k,
                      char *out, size_t n);

/* The cached code for k (malloc'd, free() it) and its size, or NULL on a
 * miss. A damaged or stale entry is removed. */
void *shader_cache_load(shader_cache *c, const shader_cache_key *k,
                        size_t *len);

/* Trims the directory: removes temporary files older than an hour (a
 * store a crash cut short) and, when the entries add up to more than cap
 * bytes, the least recently used ones (a hit refreshes an entry's time)
 * until three quarters of cap are left. Returns how many files it removed
 * (a remove that fails, e.g. on a file another process holds open, is not
 * counted, and that entry's bytes still count against the cap). */
unsigned shader_cache_prune(shader_cache *c, uint64_t cap);

/* Writes k's entry. 1 on success; 0 leaves no entry and no temporary file. */
int shader_cache_store(shader_cache *c, const shader_cache_key *k,
                       const void *code, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SHADER_CACHE_H */
