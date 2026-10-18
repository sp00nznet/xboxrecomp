/*
 * kernel_missing.c - the missing-game-file report (kernel_missing.h)
 *
 * Which failed opens count. A title fails opens all the time on purpose: it
 * probes its cache partition before building it, scans numbered files until
 * one is absent, looks for save metadata, and falls back from an archive to
 * a loose-file layout it never shipped. On one title every one of those is a
 * [FILE] ... FAILED line on a complete dump. So a miss is only a read-only
 * FILE_OPEN of a file, by absolute name, in the game-files tree -- and it is
 * "high confidence" only when the directory it should be in exists. In runs
 * with files removed from a dump, that split named exactly the removed files
 * and put every probe in the low bucket.
 *
 * Repeats. A looping stream re-opens its missing file every loop (97 FAILED
 * lines in 100 s on one title). Once a path has its "[FILE] missing" line,
 * the bridge asks xbox_missing_is_reported before printing FAILED again and
 * leaves it out; the summary counts what was left out. Verbose prints every
 * attempt, and off prints exactly what it did before this report existed.
 *
 * Memory is fixed: one static table of SLOTS paths, open addressing, no
 * allocation. Past it, misses are counted but not named, and their repeats
 * cannot be recognised, so they keep printing.
 */
#include "kernel.h"
#include "kernel_missing.h"
#include "recomp_env.h"
#include "recomp_exit_hook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define SLOTS      4096
#define GUEST_MAX  160      /* the per-attempt line's truncation */

enum { MODE_UNREAD = -1, MODE_OFF = 0, MODE_DEFAULT = 1, MODE_ALL = 2 };
enum { CLASS_EMPTY = 0, CLASS_PENDING, CLASS_HIGH, CLASS_LOW, CLASS_OTHER, CLASS_PROBE };

typedef struct {
    volatile LONG cls;          /* CLASS_*, written under the lock */
    LONG          attempts;     /* failed opens of this path, under the lock */
    uint32_t      hash;
    char          guest[GUEST_MAX];    /* as the title spelled it */
    char          key[GUEST_MAX];      /* folded: lower case */
    char          host[MAX_PATH];
    int           tree;
} missing_slot;

static missing_slot s_slots[SLOTS];
static SRWLOCK      s_lock = SRWLOCK_INIT;

/* Lock-free counters: the watchdog and the crash handler read them while the
 * thread they report on may be inside the lock. */
static volatile LONG s_high, s_low, s_other, s_probe;
static volatile LONG s_attempts;    /* failed game-tree opens */
static volatile LONG s_repeats;     /* FAILED lines left out */
static volatile LONG s_unnamed;     /* misses past the table */
static volatile LONG s_cap_said, s_summary_done, s_list_done, s_atexit_set;
/* Read and set without a lock: every thread computes the same value from
 * the same environment, so a racing first read only repeats the work. */
static volatile int s_mode = MODE_UNREAD;

/* Access bits that make an open a write: GENERIC_WRITE, GENERIC_ALL,
 * FILE_WRITE_DATA, FILE_APPEND_DATA, FILE_WRITE_EA, FILE_WRITE_ATTRIBUTES,
 * DELETE. A failed write or create is the title making something, not
 * reading a file it expects to be there. */
#define WRITE_ACCESS (0x40000000u | 0x10000000u | 0x2u | 0x4u | 0x10u | 0x100u | 0x10000u)

static int mode(void)
{
    if (s_mode == MODE_UNREAD) {
        const char *v = recomp_env(RENV_MISSING);
        s_mode = !v ? MODE_DEFAULT
               : strcmp(v, "0") == 0 ? MODE_OFF
               : strcmp(v, "all") == 0 ? MODE_ALL : MODE_DEFAULT;
    }
    return s_mode;
}

static LONG load(volatile LONG *p)
{
    return InterlockedCompareExchange(p, 0, 0);
}

/* FATX names compare without case. Separators are left alone: the path
 * rules only match backslashes, so "d:/x" is not the same open as "d:\x". */
static uint32_t fold(const char *in, char *out)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; in[i] && i + 1 < GUEST_MAX; i++) {
        char c = (char)tolower((unsigned char)in[i]);
        out[i] = c;
        h = (h ^ (unsigned char)c) * 16777619u;
    }
    out[i] = '\0';
    return h;
}

/* Under the lock. The slot holding key, or the empty slot where it would go,
 * or NULL when the table is full and key is not in it. */
static missing_slot *find(const char *key, uint32_t h)
{
    uint32_t i, n;
    for (n = 0, i = h & (SLOTS - 1); n < SLOTS; n++, i = (i + 1) & (SLOTS - 1)) {
        missing_slot *s = &s_slots[i];
        if (s->cls == CLASS_EMPTY)
            return s;
        if (s->hash == h && strcmp(s->key, key) == 0)
            return s;
    }
    return NULL;
}

static void copy_guest(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n >= GUEST_MAX) n = GUEST_MAX - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *tree_name(int tree)
{
    switch (tree) {
    case XBOX_TREE_GAME:   return "game";
    case XBOX_TREE_HDD:    return "hdd";
    case XBOX_TREE_USER:   return "user";
    case XBOX_TREE_DEVICE: return "device";
    default:               return "unknown";
    }
}

static void at_exit(void)
{
    xbox_missing_summary("exit");
}

int xbox_missing_verbose(void)
{
    return mode() == MODE_ALL;
}

int xbox_missing_is_reported(const char *guest_path)
{
    char key[GUEST_MAX];
    uint32_t h;
    missing_slot *s;
    int hit;

    if (!guest_path || mode() != MODE_DEFAULT || load(&s_high) == 0)
        return 0;
    h = fold(guest_path, key);
    AcquireSRWLockExclusive(&s_lock);
    s = find(key, h);
    hit = s && s->cls == CLASS_HIGH;
    ReleaseSRWLockExclusive(&s_lock);
    if (hit)
        InterlockedIncrement(&s_repeats);
    return hit;
}

void xbox_missing_note(const char *guest_path, uint32_t status,
                       uint32_t access, uint32_t disposition,
                       uint32_t options, int is_probe)
{
    int m = mode();
    int tree, cls, game, absolute, first;
    char key[GUEST_MAX];
    uint32_t h;
    missing_slot *s;

    if (m == MODE_OFF || !guest_path || !guest_path[0])
        return;
    if (status != (uint32_t)STATUS_OBJECT_NAME_NOT_FOUND
            && status != (uint32_t)STATUS_OBJECT_PATH_NOT_FOUND)
        return;
    if (disposition != XBOX_FILE_OPEN || (access & WRITE_ACCESS)
            || (options & XBOX_FILE_DIRECTORY_FILE))
        return;
    absolute = guest_path[0] == '\\'
            || (isalpha((unsigned char)guest_path[0]) && guest_path[1] == ':');
    /* The default mode reports game-tree opens only: everything else returns
     * here, with no lock and no tree walk. */
    if (m == MODE_DEFAULT && (is_probe || !absolute))
        return;
    tree = absolute ? xbox_path_tree(guest_path) : XBOX_TREE_UNKNOWN;
    game = !is_probe && tree == XBOX_TREE_GAME;
    if (m == MODE_DEFAULT && !game)
        return;
    if (game)
        InterlockedIncrement(&s_attempts);

    h = fold(guest_path, key);
    AcquireSRWLockExclusive(&s_lock);
    s = find(key, h);
    first = s && s->cls == CLASS_EMPTY;
    if (first) {
        s->cls = CLASS_PENDING;
        s->hash = h;
        memcpy(s->key, key, sizeof s->key);
        copy_guest(s->guest, guest_path);
        s->tree = tree;
        s->host[0] = '\0';
    }
    if (s)
        s->attempts++;
    ReleaseSRWLockExclusive(&s_lock);

    if (!s) {
        InterlockedIncrement(&s_unnamed);
        if (InterlockedExchange(&s_cap_said, 1) == 0) {
            fprintf(stderr, "  [FILE] missing-file table full (%d paths): "
                    "further misses are counted, not named\n", SLOTS);
            fflush(stderr);
        }
        return;
    }
    if (!first)
        return;

    if (InterlockedExchange(&s_atexit_set, 1) == 0) {
        atexit(at_exit);
        recomp_exit_hook_set(xbox_missing_summary);   /* the windows' ExitProcess */
    }

    /* Outside the lock: the stat, the narrowing and the print. Only the
     * rule trees have a host path from this call; the last host path of a
     * device or relative open belongs to an earlier one. */
    {
        char host[MAX_PATH] = "?";
        if (tree == XBOX_TREE_GAME || tree == XBOX_TREE_HDD || tree == XBOX_TREE_USER) {
            const wchar_t *w = xbox_LastHostPath();
            size_t n = 0;
            while (n + 1 < sizeof host && w[n]) {
                host[n] = (char)w[n];
                n++;
            }
            host[n] = '\0';
        }
        if (is_probe)
            cls = CLASS_PROBE;
        else if (!game)
            cls = CLASS_OTHER;
        else {
#if defined(_WIN32)
            cls = xbox_host_parent_exists(xbox_LastHostPath()) ? CLASS_HIGH : CLASS_LOW;
#else
            cls = xbox_host_parent_exists(host) ? CLASS_HIGH : CLASS_LOW;
#endif
        }

        AcquireSRWLockExclusive(&s_lock);
        memcpy(s->host, host, sizeof s->host);
        s->cls = cls;
        ReleaseSRWLockExclusive(&s_lock);

        switch (cls) {
        case CLASS_HIGH:
            InterlockedIncrement(&s_high);
            fprintf(stderr, "  [FILE] missing %s -> %s\n", s->guest, host);
            break;
        case CLASS_LOW:
            InterlockedIncrement(&s_low);
            if (m == MODE_ALL)
                fprintf(stderr, "  [FILE] not found (no such directory) %s -> %s\n",
                        s->guest, host);
            break;
        case CLASS_PROBE:
            InterlockedIncrement(&s_probe);
            fprintf(stderr, "  [FILE] not found (%s, probe) %s -> %s\n",
                    tree_name(tree), s->guest, host);
            break;
        default:
            InterlockedIncrement(&s_other);
            fprintf(stderr, "  [FILE] not found (%s) %s -> %s\n",
                    tree_name(tree), s->guest, host);
            break;
        }
        fflush(stderr);
    }
}

static const char *class_name(LONG cls)
{
    switch (cls) {
    case CLASS_HIGH:  return "high";
    case CLASS_LOW:   return "low";
    case CLASS_PROBE: return "probe";
    case CLASS_OTHER: return "other";
    default:          return NULL;     /* empty, or still being classified */
    }
}

static void write_list(const char *path)
{
    FILE *f = fopen(path, "w");
    int i;
    if (!f) {
        fprintf(stderr, "  [FILE] missing_list: cannot write %s\n", path);
        return;
    }
    AcquireSRWLockExclusive(&s_lock);
    for (i = 0; i < SLOTS; i++) {
        const char *c = class_name(s_slots[i].cls);
        if (c)
            fprintf(f, "%s %ld %s %s\n", c, (long)s_slots[i].attempts,
                    s_slots[i].guest, s_slots[i].host);
    }
    ReleaseSRWLockExclusive(&s_lock);
    fclose(f);
}

void xbox_missing_summary(const char *why)
{
    int m = mode();
    LONG hi, lo, ot, pr, at, rp, un;
    const char *list;
    char extra[160] = "";

    if (m == MODE_OFF)
        return;
    /* Once, by whichever exit gets here first: on POSIX a firmware exit
     * also runs atexit, and a window close can race the main thread's
     * exit, and two "w" opens of one file would interleave. */
    list = recomp_env(RENV_MISSING_LIST);
    if (list && *list && InterlockedExchange(&s_list_done, 1) == 0)
        write_list(list);

    hi = load(&s_high); lo = load(&s_low); ot = load(&s_other);
    pr = load(&s_probe); at = load(&s_attempts); rp = load(&s_repeats);
    un = load(&s_unnamed);
    /* By default a complete dump, whose only misses are probes, prints
     * nothing; verbose sums up whatever it listed. */
    if (m == MODE_DEFAULT ? hi == 0 : hi + lo + ot + pr + un == 0)
        return;
    if (InterlockedExchange(&s_summary_done, 1) != 0)
        return;

    if (rp)
        snprintf(extra + strlen(extra), sizeof extra - strlen(extra),
                 ", %ld repeat%s not printed", (long)rp, rp == 1 ? "" : "s");
    if (m == MODE_ALL && ot)
        snprintf(extra + strlen(extra), sizeof extra - strlen(extra), ", %ld other", (long)ot);
    if (m == MODE_ALL && pr)
        snprintf(extra + strlen(extra), sizeof extra - strlen(extra),
                 ", %ld probe%s", (long)pr, pr == 1 ? "" : "s");
    if (un)
        snprintf(extra + strlen(extra), sizeof extra - strlen(extra),
                 ", %ld past the %d-path table", (long)un, SLOTS);
    /* Verbose also says which exit printed it: atexit, firmware, window or
     * bugcheck. */
    fprintf(stderr, "  [FILE] summary: %ld game file%s missing, %ld not found under "
            "absent directories, %ld failed open%s%s%s%s%s%s\n",
            (long)hi, hi == 1 ? "" : "s", (long)lo, (long)at, at == 1 ? "" : "s", extra,
            m == MODE_DEFAULT && lo ? " (RECOMP_TRACE=missing=all lists them)" : "",
            m == MODE_ALL ? " [at " : "", m == MODE_ALL && why ? why : "",
            m == MODE_ALL ? "]" : "");
    fflush(stderr);
}

void xbox_missing_counts(uint32_t *high, uint32_t *low, uint32_t *attempts)
{
    if (high)     *high = (uint32_t)load(&s_high);
    if (low)      *low = (uint32_t)load(&s_low);
    if (attempts) *attempts = (uint32_t)load(&s_attempts);
}

void xbox_missing_reset(void)
{
    AcquireSRWLockExclusive(&s_lock);
    memset(s_slots, 0, sizeof s_slots);
    ReleaseSRWLockExclusive(&s_lock);
    InterlockedExchange(&s_high, 0);
    InterlockedExchange(&s_low, 0);
    InterlockedExchange(&s_other, 0);
    InterlockedExchange(&s_probe, 0);
    InterlockedExchange(&s_attempts, 0);
    InterlockedExchange(&s_repeats, 0);
    InterlockedExchange(&s_unnamed, 0);
    InterlockedExchange(&s_cap_said, 0);
    InterlockedExchange(&s_summary_done, 0);
    InterlockedExchange(&s_list_done, 0);
    s_mode = MODE_UNREAD;
}
