/*
 * kernel_path.c - Xbox device-path translation
 *
 * Translates Xbox device-style paths to host filesystem paths:
 *   \Device\CdRom0\  -> <game_dir>/
 *   D:\               -> <game_dir>/
 *   T:\               -> <save_dir>/TitleData/
 *   U:\               -> <save_dir>/UserData/
 *   Z:\               -> <save_dir>/Cache/
 *
 * The Win32 build emits UTF-16 paths (for CreateFileW); the Linux build
 * emits UTF-8 paths with '/' separators (for open()).
 */

#include "kernel.h"
#include "recomp_env.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* A project may set this to watch every guest path the kernel translates (for
 * example to print who opened a file). NULL by default. */
void (*g_xbox_path_hook)(const char *xbox_path) = NULL;

/*
 * Helper: check if an ANSI string starts with a prefix (case-insensitive).
 * Returns the number of chars consumed from the prefix, or 0 if no match.
 * Platform-independent.
 */
static int match_prefix(const char* path, const char* prefix)
{
    int i = 0;
    while (prefix[i]) {
        if (tolower((unsigned char)path[i]) != tolower((unsigned char)prefix[i]))
            return 0;
        i++;
    }
    return i;
}

/* A device-path translation rule, shared by both backends. */
typedef struct {
    const char* prefix;     /* Xbox path prefix (backslash form)         */
    int         to_save;    /* 1 = under save_dir, 0 = under game_dir,
                               2 = under the user-data root (save_dir, or
                               RECOMP_SAVE_DIR when set)                  */
    const char* sub_win;    /* sub-directory, Win32 backslash form       */
    const char* sub_posix;  /* sub-directory, POSIX slash form           */
} path_rule;

static const path_rule s_rules[] = {
    { "\\Device\\CdRom0\\",                   0, NULL,         NULL          },
    /* Saves. Titles map U: and T: themselves (XapiMapLetterToDirectory) to
     * E:\UDATA\<title id> and E:\TDATA\<title id>, which are under
     * Partition1. Without these two rules the Partition1 rule below put every
     * save in the game dir, shared by all runs and all save dirs: a fresh
     * XDG_DATA_HOME still saw the last run's save game. Listed first so they
     * win over Partition1; no trailing separator, the title opens the bare
     * directories too. RECOMP_SAVE_DIR moves just these (to_save 2), so a
     * scripted run can start from an empty save without touching the rest of
     * the save dir (partition images, caches). */
    { "\\Device\\Harddisk0\\Partition1\\UDATA", 2, "\\UDATA",  "/UDATA"      },
    { "\\Device\\Harddisk0\\Partition1\\TDATA", 2, "\\TDATA",  "/TDATA"      },
    /* E:\CACHE is the title's own scratch on the console's hard disk, and it
     * writes there: Burnout 3 opens CACHE\LocalCache00.bin and
     * LocalCache08.bin with FILE_OPEN_IF. Under the Partition1 rule below
     * those landed in the game dir, so every run wrote into the extracted
     * game files and all save dirs shared one cache. It goes with the saves,
     * for the same reasons they do.
     *
     * Its folder is TitleCache, not CACHE: Z: and Partition3-5 (the cache
     * partitions behind X:, Y: and Z:) already go to <save>/Cache, and on a
     * case-insensitive host (Windows, Proton, macOS) CACHE and Cache are one
     * folder, so the title's files would mix with the utility-drive caches
     * and could collide by name.
     *
     * Partition1 as a whole cannot follow it: a title may read its own assets
     * through Partition1 (Wreckless links \??\Z: to Partition1\ and loads
     * everything through z:\, see resolve_symlink), and those live in the
     * game dir. So only the directories a title writes move, one rule each. */
    { "\\Device\\Harddisk0\\Partition1\\CACHE", 2, "\\TitleCache","/TitleCache" },
    { "\\Device\\Harddisk0\\Partition1\\",    0, NULL,         NULL          },
    /* The rest of the disk. Partition 0 is the whole raw device, 2 holds
     * system data, and 3-5 are the per-title caches behind X:, Y: and Z:.
     * Without these the path layer reported "Unrecognized Xbox path" and
     * NtOpenFile returned STATUS_OBJECT_PATH_NOT_FOUND; DSTEAL_JP probes
     * partition0 at startup and treats the failure as fatal.
     *
     * No trailing separator: the probe opens the device itself, with nothing
     * after it. Listed after Partition1 so that keeps its own mapping. */
    /* Partition2 is C:, the system partition. On a console that is where the
     * dashboard and its own assets live, and the dashboard opens them by
     * device path as well as through Y:, so a path *under* partition2 is an
     * ordinary asset read and belongs in the game dir -- the same place Y:
     * already goes. Only the bare device keeps the system-data image, which
     * is why this rule carries the trailing separator and is listed first. */
    { "\\Device\\Harddisk0\\Partition2\\",    0, NULL,         NULL          },
    { "C:\\",                                 0, NULL,         NULL          },
    { "\\??\\C:\\",                           0, NULL,         NULL          },
    { "\\Device\\Harddisk0\\Partition2",     1, "\\SystemData", "/SystemData" },
    { "\\Device\\Harddisk0\\Partition3",     1, "\\Cache",   "/Cache"      },
    { "\\Device\\Harddisk0\\Partition4",     1, "\\Cache",   "/Cache"      },
    { "\\Device\\Harddisk0\\Partition5",     1, "\\Cache",   "/Cache"      },
    { "D:\\",                                 0, NULL,         NULL          },
    { "d:\\",                                 0, NULL,         NULL          },
    /* Y: is the Xbox dashboard partition; the dashboard opens its assets
     * (e.g. "Y:\default.xip") from there. Map it to the game dir. */
    { "Y:\\",                                 0, NULL,         NULL          },
    { "y:\\",                                 0, NULL,         NULL          },
    { "T:\\",                                 1, "\\TitleData","/TitleData"  },
    { "U:\\",                                 1, "\\UserData", "/UserData"   },
    { "Z:\\",                                 1, "\\Cache",    "/Cache"      },
    { "\\??\\D:\\",                           0, NULL,         NULL          },
    { "\\??\\Y:\\",                           0, NULL,         NULL          },
    { "\\??\\y:\\",                           0, NULL,         NULL          },
    { "\\??\\T:\\",                           1, "\\TitleData","/TitleData"  },
};
#define PATH_RULE_COUNT ((int)(sizeof(s_rules) / sizeof(s_rules[0])))

/* Saves left in the game dir. Builds before the UDATA/TDATA rules sent all
 * of Partition1 there, so a player's saves may be in <game_dir>/UDATA. With
 * RECOMP_SAVE_DIR unset, no UDATA in the save dir and one in the game dir,
 * xbox_path_init keeps using that one (and says so); the same for TDATA.
 * Indexed 0 for UDATA, 1 for TDATA. TitleCache never stays in the game dir:
 * the title's E:\CACHE is scratch it rebuilds, and keeping it out of the
 * dump is why its rule exists. */
static int s_user_in_game[2];

static int user_rule_in_game(const path_rule* r)
{
    if (strcmp(r->sub_posix, "/UDATA") == 0)
        return s_user_in_game[0];
    if (strcmp(r->sub_posix, "/TDATA") == 0)
        return s_user_in_game[1];
    return 0;
}

/*
 * Match a rule against a path: the prefix, and then a boundary unless the
 * prefix already ends in a separator. Without the boundary the bare
 * directory rules matched any name that starts the same way, so an asset
 * read through E: such as Partition1\cached.dat became <save>/CACHE/d.dat
 * (before CACHE moved to TitleCache), and Partition1\UDATAX\a went to the
 * saves. Wreckless loads every asset through z:, which it links to
 * Partition1\, so such a name is reachable.
 *
 * Returns the number of chars consumed, or 0 if the rule does not apply.
 */
static int match_rule(const char* path, const path_rule* rule)
{
    int  n = match_prefix(path, rule->prefix);
    char last, next;

    if (!n)
        return 0;
    last = rule->prefix[n - 1];
    next = path[n];
    if (last == '\\' || last == '/' || next == '\0' || next == '\\' || next == '/')
        return n;
    return 0;
}

/*
 * Rewrite a path through a drive letter the title mapped for itself.
 *
 * Titles do their own mounting. Wreckless calls IoCreateSymbolicLink at guest
 * 0x000E9B1D to link "\??\Z:" to "\Device\Harddisk0\Partition1\", then loads
 * every asset through z:\. kernel_io.c has recorded that link since it was
 * written, but nothing ever read the table back, so the static rule below --
 * Z: is the cache partition, which is true of the console in general and wrong
 * for this title -- sent every asset open to the save directory and it failed
 * with ERROR_FILE_NOT_FOUND.
 *
 * Accepts "Z:\rest" and "\??\Z:\rest"; the table is keyed on the "\??\Z:" form
 * the kernel actually registers.
 *
 * Deliberately conservative: the rewritten path is returned only when it
 * matches a static rule. A link whose target this layer has no rule for would
 * otherwise turn a path that translated adequately into one that does not
 * translate at all.
 *
 * Returns 1 and fills `out` on success, 0 to leave the path alone.
 */
static int resolve_symlink(const char* xbox_path, char* out, size_t out_size)
{
    char        link[8];
    const char* rest;
    const char* target;
    size_t      tlen, rlen;
    int         i;

    if (!xbox_path || !out || out_size == 0)
        return 0;

    if (match_prefix(xbox_path, "\\??\\") && xbox_path[4] && xbox_path[5] == ':')
        xbox_path += 4;
    if (!xbox_path[0] || xbox_path[1] != ':')
        return 0;
    rest = xbox_path + 2;

    link[0] = '\\';
    link[1] = '?';
    link[2] = '?';
    link[3] = '\\';
    link[4] = (char)toupper((unsigned char)xbox_path[0]);
    link[5] = ':';
    link[6] = '\0';

    target = xbox_LookupSymbolicLink(link);
    if (!target || !target[0])
        return 0;

    while (*rest == '\\' || *rest == '/')
        rest++;

    tlen = strlen(target);
    rlen = strlen(rest);
    if (tlen + rlen + 2 > out_size)
        return 0;

    memcpy(out, target, tlen);
    if (tlen && target[tlen - 1] != '\\' && target[tlen - 1] != '/')
        out[tlen++] = '\\';
    memcpy(out + tlen, rest, rlen);
    out[tlen + rlen] = '\0';

    for (i = 0; i < PATH_RULE_COUNT; i++) {
        if (match_rule(out, &s_rules[i]))
            return 1;           /* the target is somewhere we can place */
    }
    return 0;
}

/* ======================================================================== */
/* Shared by both backends, for the same reason as xbox_LastFileError: the
 * bridge calls it unconditionally, and a _WIN32-only definition breaks every
 * POSIX link. Both backends fill it at the end of a rule translation. */
static XBOX_THREAD_LOCAL wchar_t s_last_host_path_shared[MAX_PATH];

const wchar_t *xbox_LastHostPath(void)
{
    return s_last_host_path_shared;
}

/*
 * The raw disk device, \Device\Harddisk0\Partition0, and the partition
 * devices after it. Both backends back them with image files in the save dir.
 *
 * A title opens it to read the Xbox partition table at sector 4 (offset 0x800)
 * and find the cache partitions behind X:, Y: and Z: -- XAPI's utility-drive
 * mount does exactly that, and DSTEAL_JP calls HalReturnToFirmware when the
 * read fails. A directory cannot answer a 512-byte read at a file offset, so
 * the device is backed by an image file instead.
 *
 * The table is the standard retail geometry. This is emulating a device that
 * has to be there, not fabricating anything the title owns.
 */
#define XBOX_PART_TABLE_OFFSET 0x800
#define XBOX_PART_IN_USE       0x80000000u

/* Each partition device's size in 512-byte sectors, matching the table, so a
 * title that asks the device how big it is gets an answer consistent with
 * the table it just read. Partition0 is the whole disk and is not sized. */
static const uint64_t s_part_sectors[6] = {
    0,             /* 0: whole disk                   */
    0x00465400ull, /* 1: E: game and saves            */
    0x000FA000ull, /* 2: C: system                    */
    0x00177000ull, /* 3: X: cache                     */
    0x00177000ull, /* 4: Y: cache                     */
    0x00177000ull, /* 5: Z: cache                     */
};

static void put_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* The 512-byte sector Partition0 carries at XBOX_PART_TABLE_OFFSET. */
static void xbox_build_partition_table(unsigned char sector[512])
{
    /* name[16], flags, lba_start, lba_size, reserved -- 32 bytes each */
    static const struct { const char *name; uint32_t start, size; } parts[] = {
        { "XBOX_PART_X",  0x00000400, 0x00177000 },  /* X: cache      */
        { "XBOX_PART_Y",  0x00177400, 0x00177000 },  /* Y: cache      */
        { "XBOX_PART_Z",  0x002EE400, 0x00177000 },  /* Z: cache      */
        { "XBOX_PART_C",  0x00465400, 0x000FA000 },  /* C: system     */
        { "XBOX_PART_E",  0x0055F400, 0x00465400 },  /* E: game/save  */
    };
    size_t i;

    memset(sector, 0, 512);
    memcpy(sector, "****PARTINFO****", 16);
    for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        unsigned char *e = sector + 48 + i * 32;   /* 16 magic + 32 reserved */
        size_t n = strlen(parts[i].name);
        memset(e, ' ', 16);
        memcpy(e, parts[i].name, n < 16 ? n : 16);
        put_le32(e + 16, XBOX_PART_IN_USE);
        put_le32(e + 20, parts[i].start);
        put_le32(e + 24, parts[i].size);
        put_le32(e + 28, 0);
    }
}

/*
 * A partition device, as opposed to a path on one.
 *
 * Matches "\Device\Harddisk0\PartitionN" with nothing below it, which is how
 * a device is opened, and returns the digit N ('0'-'9'); 0 otherwise. Anything
 * with a path under it -- Partition1\TDATA and the like -- is an ordinary
 * filesystem access and falls through to the rules table, which puts it in a
 * directory.
 *
 * Partition0 is the whole disk and carries the partition table. The rest
 * start empty, which is what an unformatted partition looks like, so a title
 * that wants a filesystem there formats one.
 */
static int xbox_partition_device_digit(const char *xbox_path)
{
    static const char *prefix = "\\Device\\Harddisk0\\Partition";
    int len = match_prefix(xbox_path, prefix);
    int digit;
    const char *rest;

    if (!len)
        return 0;
    digit = xbox_path[len];
    if (digit < '0' || digit > '9')
        return 0;

    rest = xbox_path + len + 1;
    /* Nothing below it, allowing for a single trailing separator. */
    if (*rest == '\\' || *rest == '/')
        rest++;
    return *rest == '\0' ? digit : 0;
}

/*
 * The tree a guest path lands in, for the missing-file report.
 *
 * It walks the same links and the same rules, in the same order, as
 * xbox_translate_path, so the two cannot disagree about where a path goes --
 * but it prints no [PATH] line and creates no directory, which translating a
 * second time would (both backends make the save and cache directories on
 * every call). Shared by both backends: the table, resolve_symlink and the
 * partition-device test are.
 */
int xbox_path_tree(const char *xbox_path)
{
    char linked[2][512];
    const char *p = xbox_path;
    int i, hop;

    if (!p || !p[0])
        return XBOX_TREE_UNKNOWN;
    /* Relative to a RootDirectory handle (XDeleteSaveGame opens each child
     * of a save directory that way): no rule ever sees it. */
    if (p[0] != '\\' && !(isalpha((unsigned char)p[0]) && p[1] == ':'))
        return XBOX_TREE_UNKNOWN;

    /* xbox_translate_path recurses once per link; a title can chain them. */
    for (hop = 0; hop < 8; hop++) {
        if (!resolve_symlink(p, linked[hop & 1], sizeof linked[0]))
            break;
        p = linked[hop & 1];
    }

    if (xbox_partition_device_digit(p))
        return XBOX_TREE_DEVICE;
    {
        const char *d = p;
        int n;
        if (match_prefix(d, "\\??\\"))
            d += 4;
        n = match_prefix(d, "\\Device\\CdRom0");
        if (n && d[n] == '\\' && d[n + 1] == '\0')
            n++;
        if (n && d[n] == '\0')
            return XBOX_TREE_DEVICE;
    }

    for (i = 0; i < PATH_RULE_COUNT; i++) {
        if (match_rule(p, &s_rules[i]))
            return s_rules[i].to_save == 2 ? XBOX_TREE_USER
                 : s_rules[i].to_save     ? XBOX_TREE_HDD : XBOX_TREE_GAME;
    }
    return XBOX_TREE_UNKNOWN;
}

/*
 * Whether a guest path lands in the cache partition, for the RAM cache
 * (kernel_ramfs.c). The same links and rules as xbox_translate_path, so the
 * store takes exactly the paths that would go to <hdd>/Cache.
 *
 * Returns 1 with the part below the partition root in `out` (backslashes, no
 * leading separator; "" for the root), 2 for a bare cache device
 * (Partition3-5, which xbox_translate_path sends to the partition image), and
 * 0 for anything else.
 */
int xbox_path_cache_rest(const char *xbox_path, char *out, size_t n)
{
    char linked[2][512];
    const char *p = xbox_path;
    int i, hop, skip, digit;

    if (!p || !p[0] || !out || !n)
        return 0;
    for (hop = 0; hop < 8; hop++) {
        if (!resolve_symlink(p, linked[hop & 1], sizeof linked[0]))
            break;
        p = linked[hop & 1];
    }

    digit = xbox_partition_device_digit(p);
    if (digit)
        return digit >= '3' && digit <= '5' ? 2 : 0;

    for (i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_rule(p, &s_rules[i]);
        if (!skip)
            continue;
        if (!s_rules[i].sub_posix || strcmp(s_rules[i].sub_posix, "/Cache") != 0)
            return 0;
        p += skip;
        while (*p == '\\' || *p == '/')
            p++;
        if (strlen(p) >= n)
            return 0;
        for (i = 0; p[i]; i++)
            out[i] = p[i] == '/' ? '\\' : p[i];
        out[i] = '\0';
        return 1;
    }
    return 0;
}

/* The running title's id, from its XBE certificate (xbox_MemoryLayoutInit),
 * or 0 before that. With no save dir from the game, xbox_path_init roots the
 * default hard disk at <base>/xboxrecomp/<title id>. A single shared
 * <base>/xboxrecomp gave every game one Cache, one TitleCache, one set of
 * partition images and one shader cache. BLiNX 2's RAM cache then seeded
 * BLiNX 1's pelon*.rec pages and ran out of room for its own. */
static uint32_t s_title_id;

void xbox_path_set_title_id(uint32_t title_id)
{
    s_title_id = title_id;
}

uint32_t xbox_path_title_id(void)
{
    return s_title_id;
}

/* The certificate's title name as UTF-8, for the saves zip's manifest
 * (enhance_menu.c's Saves page). Written once at load, before any menu. */
static char s_title_name[128];

void xbox_path_set_title_name(const uint16_t *name, int max_chars)
{
    size_t o = 0;
    int i;
    for (i = 0; name && i < max_chars && name[i]; i++) {
        unsigned c = name[i];
        if (c >= 0xD800 && c <= 0xDFFF)
            c = '?';                    /* a FATX title name is UCS-2 */
        if (c < 0x80 && o + 1 < sizeof s_title_name) {
            s_title_name[o++] = c < 0x20 ? ' ' : (char)c;
        } else if (c < 0x800 && o + 2 < sizeof s_title_name) {
            s_title_name[o++] = (char)(0xC0 | (c >> 6));
            s_title_name[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c >= 0x800 && o + 3 < sizeof s_title_name) {
            s_title_name[o++] = (char)(0xE0 | (c >> 12));
            s_title_name[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            s_title_name[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            break;
        }
    }
    while (o && s_title_name[o - 1] == ' ')
        o--;
    s_title_name[o] = '\0';
}

const char *xbox_path_title_name(void)
{
    return s_title_name;
}

/*
 * Whether a guest path lands in the title's saves, for the Saves page's
 * busy guard (kernel_file.c): UDATA and TDATA on Partition1, and the T:\ and
 * U:\ fallbacks a title that never links its drives writes through. Not
 * E:\CACHE, though it shares their root: a title rewrites its scratch all
 * the time, and an import never touches it. The same links and rules as
 * xbox_path_tree; relative names are 0 (the caller tracks their handles).
 */
int xbox_path_is_save(const char *xbox_path)
{
    char linked[2][512];
    const char *p = xbox_path;
    int i, hop;

    if (!p || !p[0])
        return 0;
    for (hop = 0; hop < 8; hop++) {
        if (!resolve_symlink(p, linked[hop & 1], sizeof linked[0]))
            break;
        p = linked[hop & 1];
    }
    if (xbox_partition_device_digit(p))
        return 0;
    for (i = 0; i < PATH_RULE_COUNT; i++) {
        const char *sub;
        if (!match_rule(p, &s_rules[i]))
            continue;
        sub = s_rules[i].sub_posix;
        return sub && (strcmp(sub, "/UDATA") == 0 || strcmp(sub, "/TDATA") == 0
                       || strcmp(sub, "/TitleData") == 0 || strcmp(sub, "/UserData") == 0);
    }
    return 0;
}

#if defined(_WIN32)
#define XBOX_HOST_SEP '\\'
#else
#define XBOX_HOST_SEP '/'
#endif

/* Host primitives for the legacy-save copy, one set per backend, all on
 * UTF-8 paths. The ones returning int return 0 on success, except the two
 * tests (host_is_dir, host_exists), which return nonzero when true.
 *
 * host_copy_tree copies regular files (a link to one is copied as the file it
 * names) and directories, each with its times, and flushes what it wrote;
 * anything else (a link to a directory, a junction, a device) fails the copy,
 * so a save is never adopted with a part silently missing. The times matter:
 * XFindFirstSaveGame reports a save folder's last write as the save's date.
 * Its top folder must not exist yet: a leftover of the same name (another
 * pid namespace reusing our pid) fails the copy rather than mix into it.
 * host_rename moves a folder onto a name that is absent or an empty folder,
 * and fails when that folder has entries or (Win32) another process holds it
 * open. host_holds_copy is nonzero when dst has every entry at the top of
 * src, with the same name and last write (a copy, perhaps with more saves).
 * host_clear_stale removes the <prefix> and <prefix>.<pid> entries in dir
 * whose process is gone (or is this one: Wine reuses process ids), the
 * partial copies a crash left behind. */
static int host_is_dir(const char *p);
static int host_exists(const char *p);
static int host_dir_empty(const char *p);
static void host_unlink(const char *p);
static int host_mkdir(const char *p);
static void host_rmdir(const char *p);
static int host_copy_tree(const char *src, const char *dst);
static int host_remove_tree(const char *p);
static int host_rename(const char *from, const char *to);
static int host_holds_copy(const char *dst, const char *src);
static void host_sync_dir(const char *p);
static int host_write_new(const char *p, const char *text);
static unsigned long host_pid(void);
static void host_clear_stale(const char *dir, const char *prefix);

#if defined(_WIN32)
/* ======================================================================== */

#include <shlobj.h>
#include <winioctl.h>

static WCHAR s_game_dir[MAX_PATH];
static WCHAR s_save_dir[MAX_PATH];
static WCHAR s_user_dir[MAX_PATH];   /* UDATA/TDATA/TitleCache root: s_save_dir or RECOMP_SAVE_DIR */
/* Set last by xbox_path_init, after the roots above. Other threads read the
 * roots once it is set (the present thread polls xbox_path_hdd_root while a
 * shader profile start waits on guest-main's init), so the store releases
 * and every read acquires. */
static atomic_int s_initialized;

static int path_ready(void)
{
    return atomic_load_explicit(&s_initialized, memory_order_acquire);
}

#define XBOX_DISK_IMAGE_NAME   L"Partition0.img"

static void xbox_write_partition_table(const WCHAR *path)
{
    unsigned char sector[512];
    HANDLE h;
    DWORD written;
    LARGE_INTEGER off;

    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    xbox_build_partition_table(sector);
    off.QuadPart = XBOX_PART_TABLE_OFFSET;
    if (SetFilePointerEx(h, off, NULL, FILE_BEGIN))
        WriteFile(h, sector, sizeof(sector), &written, NULL);
    CloseHandle(h);
}

static BOOL xbox_partition_device_path(const char *xbox_path, WCHAR *out, DWORD n)
{
    int digit = xbox_partition_device_digit(xbox_path);

    if (!digit)
        return FALSE;
    swprintf_s(out, n, L"%s\\Partition%c.img", s_save_dir, (WCHAR)digit);
    return TRUE;
}

static int widen(const char *p, WCHAR *out)
{
    return p && MultiByteToWideChar(CP_UTF8, 0, p, -1, out, MAX_PATH) > 0;
}

/* a\b into out (MAX_PATH): 0 when it does not fit. swprintf_s would call
 * the invalid-parameter handler, which aborts, rather than return. */
static int wjoin(WCHAR *out, const WCHAR *a, const WCHAR *b)
{
    return _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%s\\%s", a, b) >= 0;
}

static int host_is_dir(const char *p)
{
    WCHAR w[MAX_PATH];
    DWORD a;
    if (!widen(p, w)) return 0;
    a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int host_exists(const char *p)
{
    WCHAR w[MAX_PATH];
    return widen(p, w) && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
}

/* 1 when p is a folder with nothing in it. */
static int host_dir_empty(const char *p)
{
    WCHAR w[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int empty = 1;

    if (!host_is_dir(p) || !widen(p, w) || !wjoin(pat, w, L"*"))
        return 0;
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
            empty = 0;
    } while (empty && FindNextFileW(f, &fd));
    FindClose(f);
    return empty;
}

static void host_unlink(const char *p)
{
    WCHAR w[MAX_PATH];
    if (widen(p, w))
        DeleteFileW(w);
}

static int host_mkdir(const char *p)
{
    WCHAR w[MAX_PATH];
    return widen(p, w) && CreateDirectoryW(w, NULL) ? 0 : -1;
}

static void host_rmdir(const char *p)
{
    WCHAR w[MAX_PATH];
    if (widen(p, w))
        RemoveDirectoryW(w);   /* only when empty */
}

/* dst's creation, access and write times set to src's. A failure is logged
 * and the copy goes on: the times date the save, and a folder without them
 * reads as not a copy to an instance that loses the rename to it. */
static void copy_times_w(const WCHAR *src, const WCHAR *dst)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    HANDLE h;
    BOOL ok;

    if (!GetFileAttributesExW(src, GetFileExInfoStandard, &a)) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%S: times not kept (error %lu)",
                 dst, GetLastError());
        return;
    }
    h = CreateFileW(dst, FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    ok = h != INVALID_HANDLE_VALUE
      && SetFileTime(h, &a.ftCreationTime, &a.ftLastAccessTime, &a.ftLastWriteTime);
    if (!ok)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%S: times not kept (error %lu)",
                 dst, GetLastError());
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

/* What CopyFileW wrote, on the disk before the rename names it. CopyFileW
 * keeps a read-only attribute, which refuses the write handle the flush
 * needs, so it is lifted for the flush and put back after. */
static int flush_file_w(const WCHAR *p)
{
    DWORD attr = GetFileAttributesW(p);
    HANDLE h;
    BOOL ok;

    if (attr == INVALID_FILE_ATTRIBUTES)
        return -1;
    if (attr & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(p, attr & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    h = CreateFileW(p, GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ok = h != INVALID_HANDLE_VALUE && FlushFileBuffers(h);
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    if (attr & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(p, attr);
    return ok ? 0 : -1;
}

static int copy_tree_w(const WCHAR *src, const WCHAR *dst)
{
    WCHAR pat[MAX_PATH], s[MAX_PATH], t[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int rc = 0;

    if (!CreateDirectoryW(dst, NULL)) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%S: cannot create it (error %lu)",
                 dst, GetLastError());
        return -1;
    }
    if (!wjoin(pat, src, L"*"))
        return -1;
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (!wjoin(s, src, fd.cFileName) || !wjoin(t, dst, fd.cFileName)) {
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%S\\%S: path too long to copy",
                     src, fd.cFileName);
            rc = -1;
            break;
        }
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                     "%S: a junction or a link to a folder; not copied", s);
            rc = -1;
            break;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (copy_tree_w(s, t) != 0) { rc = -1; break; }
        } else if (!CopyFileW(s, t, TRUE) || flush_file_w(t) != 0) {
            /* CopyFileW follows a file link and keeps the last-write time. */
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%S: cannot copy (error %lu)",
                     s, GetLastError());
            rc = -1;
            break;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    /* After the entries: creating them moved the folder's write time. */
    if (rc == 0)
        copy_times_w(src, dst);
    return rc;
}

static int host_copy_tree(const char *src, const char *dst)
{
    WCHAR s[MAX_PATH], d[MAX_PATH];
    return widen(src, s) && widen(dst, d) ? copy_tree_w(s, d) : -1;
}

static int remove_tree_w(const WCHAR *p)
{
    WCHAR pat[MAX_PATH], s[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;

    if (!wjoin(pat, p, L"*"))
        return -1;
    f = FindFirstFileW(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            if (!wjoin(s, p, fd.cFileName))
                continue;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                remove_tree_w(s);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryW(s);
            else {
                /* DeleteFileW refuses a read-only file. */
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                    SetFileAttributesW(s, fd.dwFileAttributes
                                          & ~(DWORD)FILE_ATTRIBUTE_READONLY);
                DeleteFileW(s);
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return RemoveDirectoryW(p) ? 0 : -1;
}

static int host_remove_tree(const char *p)
{
    WCHAR w[MAX_PATH];
    return widen(p, w) ? remove_tree_w(w) : -1;
}

/* MoveFileExW does not replace a folder, as POSIX rename does an empty one,
 * so the empty one goes first, through a handle that shares nothing. A
 * running instance whose game made that folder at boot and holds it open
 * makes the open fail, and the copy fails and is tried next run.
 * RemoveDirectoryW would instead mark the folder for deletion, and it would
 * vanish from under that instance once it closed its handle. Only a plain
 * empty folder is deleted: one with entries refuses the delete, and a file
 * or a junction is never touched. A process that makes the folder again
 * between the delete and the move fails the move, which is retried next
 * run as well. */
static int host_rename(const char *from, const char *to)
{
    WCHAR f[MAX_PATH], t[MAX_PATH];
    FILE_DISPOSITION_INFO del;
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE h;
    BOOL ok;

    if (!widen(from, f) || !widen(to, t))
        return -1;
    h = CreateFileW(t, DELETE | FILE_READ_ATTRIBUTES, 0, NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION)
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                     "%S: open elsewhere (another instance, Explorer, a "
                     "scanner); not replaced this run", t);
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
            return -1;
    } else {
        ok = GetFileInformationByHandle(h, &info)
          && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
          && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        if (ok) {
            del.DeleteFile = TRUE;
            ok = SetFileInformationByHandle(h, FileDispositionInfo, &del, sizeof del);
        }
        CloseHandle(h);
        if (!ok)
            return -1;
    }
    return MoveFileExW(f, t, 0) ? 0 : -1;
}

static int host_holds_copy(const char *dst, const char *src)
{
    WCHAR d[MAX_PATH], s[MAX_PATH], pat[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA x, y;
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int held = 1;

    if (!widen(dst, d) || !widen(src, s) || !wjoin(pat, s, L"*"))
        return 0;
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        held = wjoin(a, s, fd.cFileName) && wjoin(b, d, fd.cFileName)
            && GetFileAttributesExW(a, GetFileExInfoStandard, &x)
            && GetFileAttributesExW(b, GetFileExInfoStandard, &y)
            && x.ftLastWriteTime.dwLowDateTime == y.ftLastWriteTime.dwLowDateTime
            && x.ftLastWriteTime.dwHighDateTime == y.ftLastWriteTime.dwHighDateTime;
    } while (held && FindNextFileW(f, &fd));
    if (held && GetLastError() != ERROR_NO_MORE_FILES)
        held = 0;
    FindClose(f);
    return held;
}

/* NTFS journals its metadata; a folder handle cannot be flushed without
 * administrator rights. */
static void host_sync_dir(const char *p)
{
    (void)p;
}

static int host_write_new(const char *p, const char *text)
{
    WCHAR w[MAX_PATH];
    HANDLE h;
    DWORD n = 0;
    BOOL ok;

    if (!widen(p, w))
        return -1;
    h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    ok = WriteFile(h, text, (DWORD)strlen(text), &n, NULL)
      && WriteFile(h, "\r\n", 2, &n, NULL)
      && FlushFileBuffers(h);
    CloseHandle(h);
    return ok ? 0 : -1;
}

static unsigned long host_pid(void)
{
    return (unsigned long)GetCurrentProcessId();
}

static int pid_alive(unsigned long pid)
{
    DWORD code = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h)
        return GetLastError() == ERROR_ACCESS_DENIED;
    if (!GetExitCodeProcess(h, &code))
        code = STILL_ACTIVE;
    CloseHandle(h);
    return code == STILL_ACTIVE;
}

static void host_clear_stale(const char *dir, const char *prefix)
{
    WCHAR d[MAX_PATH], wp[MAX_PATH], pat[MAX_PATH], s[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    size_t n;

    if (!widen(dir, d) || !widen(prefix, wp) || !wjoin(pat, d, L"*"))
        return;
    n = wcslen(wp);
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        const WCHAR *rest = fd.cFileName + n;
        WCHAR *end = NULL;
        unsigned long pid = 0;

        if (_wcsnicmp(fd.cFileName, wp, n) != 0)
            continue;
        if (*rest == L'.') {
            pid = wcstoul(rest + 1, &end, 10);
            if (end == rest + 1 || *end
                || (pid != host_pid() && pid_alive(pid)))
                continue;
        } else if (*rest) {
            continue;
        }
        if (wjoin(s, d, fd.cFileName))
            remove_tree_w(s);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    WCHAR save_base[MAX_PATH];
    WCHAR legacy_root[MAX_PATH];

    legacy_root[0] = L'\0';

    /* The fallbacks used to name Burnout 3 specifically, so any other title
     * that passed NULL silently pointed its game dir and its saves at another
     * game's folders. Generic now -- a caller that wants a title-specific
     * location should pass one. */
    if (game_dir) {
        MultiByteToWideChar(CP_UTF8, 0, game_dir, -1, s_game_dir, MAX_PATH);
    } else {
        /* Neutral default: the CWD's "game" subdirectory, which is the
         * layout the game projects use (see templates/new-game). */
        GetCurrentDirectoryW(MAX_PATH, s_game_dir);
    }

    if (save_dir) {
        MultiByteToWideChar(CP_UTF8, 0, save_dir, -1, s_save_dir, MAX_PATH);
    } else {
        /* %LOCALAPPDATA% as the environment has it, as XDG_DATA_HOME on
         * POSIX: the same folder for a player (Windows and Wine set it from
         * the known folder), and a test or a scripted run can point it at
         * scratch. The known folder when it is unset or not absolute. */
        const WCHAR *env = _wgetenv(L"LOCALAPPDATA");
        BOOL have_base = env && env[0] && env[1] == L':'
                      && (env[2] == L'\\' || env[2] == L'/')
                      && wcslen(env) < MAX_PATH;
        if (have_base)
            wcscpy_s(save_base, MAX_PATH, env);
        else
            have_base = SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, save_base));
        /* One root per title (s_title_id); the shared one before the title
         * id is known. _TRUNCATE: swprintf_s would abort on a base too long
         * for the root, which falls back to SaveData instead. */
        int fits = have_base
                && _snwprintf_s(s_save_dir, MAX_PATH, _TRUNCATE, L"%s\\xboxrecomp",
                                save_base) >= 0;
        if (fits && s_title_id) {
            wcscpy_s(legacy_root, MAX_PATH, s_save_dir);
            fits = _snwprintf_s(s_save_dir, MAX_PATH, _TRUNCATE, L"%s\\xboxrecomp\\%08x",
                                save_base, (unsigned)s_title_id) >= 0;
        }
        if (!fits) {
            if (have_base)
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%ls: path too long "
                         "for the save root; using SaveData in the current "
                         "folder", save_base);
            legacy_root[0] = L'\0';
            GetCurrentDirectoryW(MAX_PATH, s_save_dir);
            wcscat_s(s_save_dir, MAX_PATH, L"\\SaveData");
        }
    }

    size_t len = wcslen(s_game_dir);
    if (len > 0 && s_game_dir[len - 1] == L'\\')
        s_game_dir[len - 1] = L'\0';

    len = wcslen(s_save_dir);
    if (len > 0 && s_save_dir[len - 1] == L'\\')
        s_save_dir[len - 1] = L'\0';

    /* Absolute, because SHCreateDirectoryExW below only accepts a fully
     * qualified path: handed the relative "saves" a host passes, it fails with
     * ERROR_BAD_PATHNAME and creates nothing. The partition images then cannot
     * be created either, and a title that opens \Device\Harddisk0\Partition1\
     * at boot gets STATUS_OBJECT_PATH_NOT_FOUND and quits to the dashboard.
     * It only ever worked where someone had made the directory by hand. */
    {
        WCHAR full[MAX_PATH];
        if (GetFullPathNameW(s_save_dir, MAX_PATH, full, NULL))
            wcscpy_s(s_save_dir, MAX_PATH, full);
    }

    /* Create the save-side directories. T:/U:/Z: map into subdirectories of
     * save_dir, and a title that opens a file there with a create disposition
     * fails if the parent does not exist -- which reads as "cannot create save
     * file" and sends the title down its init-failure path. Halo asserts
     * exactly that at saved games/game_state_xbox.c:97 and then unwinds,
     * clearing global_d3d_device on the way out, so a missing directory
     * surfaces as a graphics failure.
     *
     * Cheap and idempotent: SHCreateDirectoryExW builds intermediates and is
     * happy if they already exist. */
    {
        static const WCHAR *subs[] = { L"TitleData", L"UserData", L"Cache",
                                       L"SystemData" };
        WCHAR image[MAX_PATH];
        WCHAR dir[MAX_PATH];
        SHCreateDirectoryExW(NULL, s_save_dir, NULL);
        for (int i = 0; i < (int)(sizeof(subs) / sizeof(subs[0])); i++) {
            swprintf_s(dir, MAX_PATH, L"%s\\%s", s_save_dir, subs[i]);
            SHCreateDirectoryExW(NULL, dir, NULL);
        }
        swprintf_s(image, MAX_PATH, L"%s\\%s", s_save_dir, XBOX_DISK_IMAGE_NAME);
        xbox_write_partition_table(image);
        /* The other partition devices, sized to the same geometry the table
         * above describes, so a title that asks the device how big it is gets
         * an answer consistent with the table it just read. Marked sparse
         * first: the cache partitions are 750 MB each and none of that is
         * touched until something writes to it. */
        {
            for (int p = 1; p <= 5; p++) {
                HANDLE h;
                DWORD ret;
                LARGE_INTEGER end;

                swprintf_s(image, MAX_PATH, L"%s\\Partition%d.img",
                           s_save_dir, p);
                h = CreateFileW(image, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h == INVALID_HANDLE_VALUE)
                    continue;
                DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0,
                                &ret, NULL);
                end.QuadPart = (LONGLONG)(s_part_sectors[p] * 512ull);
                if (SetFilePointerEx(h, end, NULL, FILE_BEGIN))
                    SetEndOfFile(h);
                CloseHandle(h);
            }
        }
    }

    /* RECOMP_SAVE_DIR: the root for the title's UDATA, TDATA and CACHE,
     * created if missing. Absolute; under Wine a DOS path (Z:\home\...) is safest. */
    {
        const char *ov = recomp_env(RENV_SAVE_DIR);
        /* Room for the longest suffix the rules append (UDATA\<title id>\
         * <save>\<file>); a longer root would only truncate later. */
        int n = (ov && ov[0])
              ? MultiByteToWideChar(CP_UTF8, 0, ov, -1, s_user_dir, MAX_PATH) : 0;
        if (ov && ov[0] && (n == 0 || n > MAX_PATH - 96)) {
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                     "RECOMP_SAVE_DIR %s: %s; using the default save dir",
                     ov, n == 0 ? "conversion failed (longer than MAX_PATH?)" : "too long for the paths under it");
            ov = NULL;
        }
        if (ov && ov[0]) {
            for (WCHAR *p = s_user_dir; *p; p++)
                if (*p == L'/') *p = L'\\';
            len = wcslen(s_user_dir);
            if (len > 3 && s_user_dir[len - 1] == L'\\')
                s_user_dir[len - 1] = L'\0';
            SHCreateDirectoryExW(NULL, s_user_dir, NULL);
        } else {
            wcscpy_s(s_user_dir, MAX_PATH, s_save_dir);
        }

        /* The title's saves from the shared root, once
         * (xbox_path_adopt_legacy_saves). Not with a RECOMP_SAVE_DIR in
         * force: the run named its saves itself. One dropped above as too
         * long is not in force, so the default root gets its saves. */
        if (legacy_root[0] && !(ov && ov[0])) {
            char from[MAX_PATH * 3], to[MAX_PATH * 3];
            if (WideCharToMultiByte(CP_UTF8, 0, legacy_root, -1, from, (int)sizeof from, NULL, NULL) > 0
                && WideCharToMultiByte(CP_UTF8, 0, s_save_dir, -1, to, (int)sizeof to, NULL, NULL) > 0)
                xbox_path_adopt_legacy_saves(from, to, s_title_id);
        }

        /* Saves still in the game dir stay there (s_user_in_game). */
        for (int k = 0; k < 2; k++) {
            static const WCHAR *names[2] = { L"UDATA", L"TDATA" };
            WCHAR in_save[MAX_PATH], in_game[MAX_PATH];
            DWORD fa, fg;
            swprintf_s(in_save, MAX_PATH, L"%s\\%s", s_save_dir, names[k]);
            swprintf_s(in_game, MAX_PATH, L"%s\\%s", s_game_dir, names[k]);
            fa = GetFileAttributesW(in_save);
            fg = GetFileAttributesW(in_game);
            s_user_in_game[k] = !(ov && ov[0]) && fa == INVALID_FILE_ATTRIBUTES
                             && fg != INVALID_FILE_ATTRIBUTES
                             && (fg & FILE_ATTRIBUTE_DIRECTORY);
            if (s_user_in_game[k])
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                         "%S: saves found here and not in the save dir; kept "
                         "here (move it to %S to use the save dir)",
                         in_game, in_save);
        }
    }

    atomic_store_explicit(&s_initialized, 1, memory_order_release);
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%S, save=%S, user data=%S",
             s_game_dir, s_save_dir, s_user_dir);
}

int xbox_path_hdd_root(char *out, size_t n)
{
    if (!path_ready() || !n)
        return 0;
    return WideCharToMultiByte(CP_UTF8, 0, s_save_dir, -1, out, (int)n,
                               NULL, NULL) > 0;
}

int xbox_path_save_root(int tdata, char *out, size_t n)
{
    WCHAR w[MAX_PATH];
    const WCHAR *base;
    if (!path_ready() || !n)
        return 0;
    base = s_user_in_game[tdata ? 1 : 0] ? s_game_dir : s_user_dir;
    if (_snwprintf_s(w, MAX_PATH, _TRUNCATE, L"%s\\%s", base,
                     tdata ? L"TDATA" : L"UDATA") < 0)
        return 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL) > 0;
}

/* The host path the last translation produced.
 *
 * A caller that wants to act on the file a title just opened -- playing an FMV
 * the host can decode itself, say -- has the guest path but not the host one,
 * and re-deriving it would duplicate every rule above. */
/* Thread-local: several threads open files at once, and a plain static let one
 * thread's translation overwrite another's between the translate and the read.
 * That showed up as the FMV trigger firing on roughly two runs in three. */

static void xbox_remember_host_path(const wchar_t *p)
{
    if (p) wcsncpy_s(s_last_host_path_shared, MAX_PATH, p, _TRUNCATE);
}



BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char*  remainder = NULL;
    const WCHAR* base_dir  = NULL;
    const char*  sub_dir   = NULL;
    int          skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!path_ready())
        xbox_path_init(NULL, NULL);

    {
        char linked[512];
        if (resolve_symlink(xbox_path, linked, sizeof(linked)))
            return xbox_translate_path(linked, host_path_buf, buf_size);
    }

    if (xbox_partition_device_path(xbox_path, host_path_buf, buf_size)) {
        fprintf(stderr, "  [PATH] %s -> partition image\n", xbox_path);
        fflush(stderr);
        return TRUE;
    }

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_rule(xbox_path, &s_rules[i]);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save == 2
                          ? (user_rule_in_game(&s_rules[i]) ? s_game_dir : s_user_dir)
                      : s_rules[i].to_save     ? s_save_dir : s_game_dir;
            sub_dir   = s_rules[i].sub_win;
            goto translate;
        }
    }

    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    MultiByteToWideChar(CP_ACP, 0, xbox_path, -1, host_path_buf, buf_size);
    return TRUE;

translate:
    if (g_xbox_path_hook)
        g_xbox_path_hook(xbox_path);
    fprintf(stderr, "  [PATH] %s\n", xbox_path);
    fflush(stderr);
    {
        WCHAR remainder_wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, remainder, -1, remainder_wide, MAX_PATH);

        for (WCHAR* p = remainder_wide; *p; p++) {
            if (*p == L'/') *p = L'\\';
        }

        if (sub_dir) {
            WCHAR sub_wide[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, sub_dir, -1, sub_wide, MAX_PATH);
            swprintf_s(host_path_buf, buf_size, L"%s%s\\%s", base_dir, sub_wide, remainder_wide);

            WCHAR dir_path[MAX_PATH];
            swprintf_s(dir_path, MAX_PATH, L"%s%s", base_dir, sub_wide);
            CreateDirectoryW(base_dir, NULL);
            CreateDirectoryW(dir_path, NULL);
        } else {
            swprintf_s(host_path_buf, buf_size, L"%s\\%s", base_dir, remainder_wide);
        }

        /* An empty remainder means the title opened the device itself, so
         * the join above leaves a trailing separator -- which CreateFileW
         * rejects even with FILE_FLAG_BACKUP_SEMANTICS, turning an existing
         * directory into STATUS_OBJECT_PATH_NOT_FOUND. */
        {
            size_t n = wcslen(host_path_buf);
            while (n > 1 && (host_path_buf[n - 1] == L'\\'
                             || host_path_buf[n - 1] == L'/'))
                host_path_buf[--n] = L'\0';
        }

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %S", xbox_path, host_path_buf);
        xbox_remember_host_path(host_path_buf);
        return TRUE;
    }
}

/* ======================================================================== */
#else /* !_WIN32  -- POSIX / Linux */
/* ======================================================================== */

#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <strings.h>
#include <signal.h>

static char s_game_dir[MAX_PATH];
static char s_save_dir[MAX_PATH];
static char s_user_dir[MAX_PATH];   /* UDATA/TDATA/TitleCache root: s_save_dir or RECOMP_SAVE_DIR */
/* Set last by xbox_path_init, after the roots above. Other threads read the
 * roots once it is set (the present thread polls xbox_path_hdd_root while a
 * shader profile start waits on guest-main's init), so the store releases
 * and every read acquires. */
static atomic_int s_initialized;

static int path_ready(void)
{
    return atomic_load_explicit(&s_initialized, memory_order_acquire);
}

/* Strip a single trailing '/' (but never the root '/'). */
static void strip_trailing_slash(char* s)
{
    size_t len = strlen(s);
    if (len > 1 && s[len - 1] == '/')
        s[len - 1] = '\0';
}

/* Recursively create a directory and all missing parents. */
static void mkdir_p(const char* path)
{
    char tmp[MAX_PATH];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return;
    memcpy(tmp, path, len + 1);

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
}

/* The Xbox file system is case-insensitive and titles rely on it: a title
 * opens MEDIA\Sound.xwb while the disc holds media/sound.xwb, and on
 * Windows or an APFS default volume that resolves by itself. On a
 * case-sensitive file system (ext4, btrfs: Linux, the Deck) it fails, so
 * when the exact host path is missing each guest-supplied component past
 * `fixed` (the host's own base and sub dir, never rewritten) is matched
 * case-insensitively against its directory. A component with no match
 * ends the walk and the rest stays as the title wrote it, so a create of
 * a new file still lands in the right, existing directory. One warning per
 * path: a title that polls a missing file must not flood the log. */
#define CASE_WARNED_SLOTS 256
static uint32_t s_case_warned[CASE_WARNED_SLOTS];

static int case_warn_first(const char* path)
{
    uint32_t h = 2166136261u;
    for (const char* p = path; *p; p++)
        h = (h ^ (unsigned char)*p) * 16777619u;
    if (!h) h = 1;
    /* Open addressing; a full table only repeats warnings. Two threads
     * racing on a slot can at worst warn twice. */
    for (int i = 0; i < CASE_WARNED_SLOTS; i++) {
        uint32_t* slot = &s_case_warned[(h + (uint32_t)i) % CASE_WARNED_SLOTS];
        if (*slot == h) return 0;
        if (*slot == 0) { *slot = h; return 1; }
    }
    return 1;
}

static void fix_case(char* path, size_t fixed)
{
    struct stat st;
    char guest[MAX_PATH];
    size_t pos = fixed;
    BOOL changed = FALSE;

    if (lstat(path, &st) == 0 || errno != ENOENT)
        return;
    snprintf(guest, sizeof(guest), "%s", path);

    while (path[pos] == '/') pos++;
    while (path[pos]) {
        size_t end = pos;
        char save;
        DIR* d;
        struct dirent* e;
        BOOL found = FALSE;

        while (path[end] && path[end] != '/') end++;
        save = path[end];
        path[end] = '\0';
        if (lstat(path, &st) == 0) {
            path[end] = save;
        } else {
            /* Open the parent: path[pos - 1] is the '/' before this one. */
            path[pos - 1] = '\0';
            d = opendir(pos > 1 ? path : "/");
            path[pos - 1] = '/';
            if (d) {
                while ((e = readdir(d)) != NULL) {
                    if (strlen(e->d_name) == end - pos
                            && strcasecmp(e->d_name, path + pos) == 0) {
                        memcpy(path + pos, e->d_name, end - pos);
                        found = changed = TRUE;
                        break;
                    }
                }
                closedir(d);
            }
            path[end] = save;
            if (!found)
                break;
        }
        pos = end;
        while (path[pos] == '/') pos++;
    }

    /* Warn only when the corrected path is there: a miss further down still
     * fails the open, and the missing-file report names it instead. */
    if (changed && lstat(path, &st) == 0 && case_warn_first(guest))
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                 "%s: not found; opened %s (case differs)", guest, path);
}

static int host_is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int host_exists(const char *p)
{
    struct stat st;
    return lstat(p, &st) == 0;
}

static int host_mkdir(const char *p)
{
    return mkdir(p, 0755);
}

static void host_rmdir(const char *p)
{
    rmdir(p);   /* only when empty */
}

#if defined(__APPLE__)
#define ST_ATIM(st) ((st).st_atimespec)
#define ST_MTIM(st) ((st).st_mtimespec)
#else
#define ST_ATIM(st) ((st).st_atim)
#define ST_MTIM(st) ((st).st_mtim)
#endif

/* fsync on macOS leaves the data in the drive's cache; F_FULLFSYNC flushes
 * it. Cheap here: a handful of save files, once per title. */
static int host_fsync(int fd)
{
#ifdef F_FULLFSYNC
    if (fcntl(fd, F_FULLFSYNC) == 0)
        return 0;
#endif
    return fsync(fd);
}

/* Copies src (st, from stat) to the new file dst with its mode and times,
 * and flushes it, so the rename after the copy never names an empty file
 * after a power loss. */
static int copy_file(const char *src, const char *dst, const struct stat *st)
{
    char buf[65536];
    ssize_t n = 0;
    struct timespec ts[2];
    int in, out, rc = 0;

    in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    out = open(dst, O_WRONLY | O_CREAT | O_EXCL, st->st_mode & 0777);
    if (out < 0) {
        close(in);
        return -1;
    }
    while ((n = read(in, buf, sizeof buf)) > 0) {
        char *p = buf;
        while (n > 0) {
            ssize_t w = write(out, p, (size_t)n);
            if (w < 0) {
                if (errno == EINTR) continue;
                rc = -1;
                break;
            }
            p += w;
            n -= w;
        }
        if (rc)
            break;
    }
    if (n < 0)
        rc = -1;
    ts[0] = ST_ATIM(*st);
    ts[1] = ST_MTIM(*st);
    /* The times only date the save; a file system that refuses them still
     * holds the data. */
    if (rc == 0 && futimens(out, ts) != 0)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%s: times not kept: %s",
                 dst, strerror(errno));
    if (rc == 0 && host_fsync(out) != 0)
        rc = -1;
    close(in);
    if (close(out) != 0)
        rc = -1;
    return rc;
}

static void host_sync_dir(const char *p)
{
    int fd = open(p, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        host_fsync(fd);
        close(fd);
    }
}

static int host_copy_tree(const char *src, const char *dst)
{
    DIR *d;
    struct dirent *e;
    struct stat sd;
    int rc = 0;

    if (stat(src, &sd) != 0)
        return -1;
    if (mkdir(dst, 0755) != 0) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%s: cannot create it: %s",
                 dst, strerror(errno));
        return -1;
    }
    d = opendir(src);
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        char s[MAX_PATH * 2], t[MAX_PATH * 2];
        struct stat st;

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if ((size_t)snprintf(s, sizeof s, "%s/%s", src, e->d_name) >= sizeof s
            || (size_t)snprintf(t, sizeof t, "%s/%s", dst, e->d_name) >= sizeof t) {
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%s/%s: path too long to copy",
                     src, e->d_name);
            rc = -1;
            break;
        }
        if (lstat(s, &st) != 0) {
            rc = -1;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (host_copy_tree(s, t) != 0) { rc = -1; break; }
            continue;
        }
        /* A link to a file is copied as the file; a link to a folder (or
         * a dangling one, a fifo, a device) fails the copy. */
        if (S_ISLNK(st.st_mode) && (stat(s, &st) != 0 || !S_ISREG(st.st_mode))) {
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                     "%s: a link to a folder or to nothing; not copied", s);
            rc = -1;
            break;
        }
        if (!S_ISREG(st.st_mode)) {
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                     "%s: neither a file nor a folder; not copied", s);
            rc = -1;
            break;
        }
        if (copy_file(s, t, &st) != 0) {
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%s: cannot copy: %s",
                     s, strerror(errno));
            rc = -1;
            break;
        }
    }
    closedir(d);
    /* After the entries: creating them moved the folder's times. */
    if (rc == 0) {
        struct timespec ts[2];
        ts[0] = ST_ATIM(sd);
        ts[1] = ST_MTIM(sd);
        if (utimensat(AT_FDCWD, dst, ts, 0) != 0)
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%s: times not kept: %s",
                     dst, strerror(errno));
        host_sync_dir(dst);
    }
    return rc;
}

static int host_remove_tree(const char *p)
{
    struct stat st;
    DIR *d;
    struct dirent *e;

    if (lstat(p, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(p);
    d = opendir(p);
    if (d) {
        while ((e = readdir(d)) != NULL) {
            char s[MAX_PATH * 2];
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            if ((size_t)snprintf(s, sizeof s, "%s/%s", p, e->d_name) < sizeof s)
                host_remove_tree(s);
        }
        closedir(d);
    }
    return rmdir(p);
}

/* rename replaces an empty folder and refuses one with entries. Another
 * instance holding the empty one open keeps a handle on a removed folder;
 * its paths reach the copy. */
static int host_rename(const char *from, const char *to)
{
    return rename(from, to);
}

static int host_holds_copy(const char *dst, const char *src)
{
    DIR *d = opendir(src);
    struct dirent *e;
    int held = 1;

    if (!d)
        return 0;
    errno = 0;
    while (held && (e = readdir(d)) != NULL) {
        char a[MAX_PATH * 2], b[MAX_PATH * 2];
        struct stat x, y;

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        held = (size_t)snprintf(a, sizeof a, "%s/%s", src, e->d_name) < sizeof a
            && (size_t)snprintf(b, sizeof b, "%s/%s", dst, e->d_name) < sizeof b
            && stat(a, &x) == 0 && stat(b, &y) == 0
            && ST_MTIM(x).tv_sec == ST_MTIM(y).tv_sec
            && ST_MTIM(x).tv_nsec == ST_MTIM(y).tv_nsec;
        errno = 0;
    }
    if (held && errno)
        held = 0;
    closedir(d);
    return held;
}

static void host_unlink(const char *p)
{
    unlink(p);
}

/* 1 when p is a folder with nothing in it. */
static int host_dir_empty(const char *p)
{
    DIR *d = opendir(p);
    struct dirent *e;
    int empty = 1;

    if (!d)
        return 0;
    while (empty && (e = readdir(d)) != NULL)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            empty = 0;
    closedir(d);
    return empty;
}

static int host_write_new(const char *p, const char *text)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
    size_t n = strlen(text);
    int rc;

    if (fd < 0)
        return -1;
    rc = write(fd, text, n) == (ssize_t)n && write(fd, "\n", 1) == 1
      && host_fsync(fd) == 0 ? 0 : -1;
    if (close(fd) != 0)
        rc = -1;
    return rc;
}

static unsigned long host_pid(void)
{
    return (unsigned long)getpid();
}

static void host_clear_stale(const char *dir, const char *prefix)
{
    size_t n = strlen(prefix);
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        const char *rest = e->d_name + n;
        char s[MAX_PATH * 2], *end = NULL;
        unsigned long pid;

        if (strncmp(e->d_name, prefix, n) != 0)
            continue;
        if (*rest == '.') {
            pid = strtoul(rest + 1, &end, 10);
            /* EPERM: alive, under another user. */
            if (end == rest + 1 || *end
                || (pid != host_pid()
                    && (kill((pid_t)pid, 0) == 0 || errno == EPERM)))
                continue;
        } else if (*rest) {
            continue;
        }
        if ((size_t)snprintf(s, sizeof s, "%s/%s", dir, e->d_name) < sizeof s)
            host_remove_tree(s);
    }
    closedir(d);
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    char legacy_root[MAX_PATH];

    legacy_root[0] = '\0';
    if (game_dir) {
        snprintf(s_game_dir, sizeof(s_game_dir), "%s", game_dir);
    } else {
        /* Neutral default: CWD/"game", the layout game projects use. */
        char cwd[MAX_PATH];
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), ".");
        snprintf(s_game_dir, sizeof(s_game_dir), "%s/game", cwd);
    }

    if (save_dir) {
        snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);
    } else {
        /* XDG base-directory spec: $XDG_DATA_HOME or ~/.local/share */
        const char* xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0]) {
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/xboxrecomp", xdg);
        } else {
            const char* home = getenv("HOME");
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/.local/share/xboxrecomp",
                     (home && home[0]) ? home : ".");
        }
        /* One root per title (s_title_id); the shared one before the title
         * id is known. */
        if (s_title_id) {
            strip_trailing_slash(s_save_dir);
            snprintf(legacy_root, sizeof(legacy_root), "%s", s_save_dir);
            if ((size_t)snprintf(s_save_dir, sizeof(s_save_dir), "%s/%08x",
                                 legacy_root, (unsigned)s_title_id)
                    >= sizeof(s_save_dir)) {
                snprintf(s_save_dir, sizeof(s_save_dir), "%s", legacy_root);
                legacy_root[0] = '\0';
            }
        }
    }

    strip_trailing_slash(s_game_dir);
    strip_trailing_slash(s_save_dir);

    /* The save-side directories and the partition device images, as the
     * Win32 backend makes them. Without Partition0.img a title's
     * utility-drive mount fails to read the partition table, and a title
     * that treats that as fatal quick-reboots. */
    {
        static const char *subs[] = { "TitleData", "UserData", "Cache",
                                      "SystemData" };
        char path[MAX_PATH];
        unsigned char sector[512];
        int fd;

        mkdir_p(s_save_dir);
        for (int i = 0; i < (int)(sizeof(subs) / sizeof(subs[0])); i++) {
            snprintf(path, sizeof(path), "%s/%s", s_save_dir, subs[i]);
            mkdir_p(path);
        }

        snprintf(path, sizeof(path), "%s/Partition0.img", s_save_dir);
        fd = open(path, O_WRONLY | O_CREAT, 0644);
        if (fd >= 0) {
            xbox_build_partition_table(sector);
            if (pwrite(fd, sector, sizeof(sector), XBOX_PART_TABLE_OFFSET)
                    != (ssize_t)sizeof(sector))
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                         "%s: partition table write failed: %s",
                         path, strerror(errno));
            close(fd);
        } else {
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%s: %s",
                     path, strerror(errno));
        }

        /* The other partition devices, sized to the table. ftruncate leaves
         * a hole on the filesystems this runs on, so the 750 MB cache
         * partitions cost nothing until something writes to them. An image
         * that is already there keeps its contents; only the size is set. */
        for (int part = 1; part <= 5; part++) {
            snprintf(path, sizeof(path), "%s/Partition%d.img", s_save_dir, part);
            fd = open(path, O_WRONLY | O_CREAT, 0644);
            if (fd < 0)
                continue;
            if (ftruncate(fd, (off_t)(s_part_sectors[part] * 512ull)) != 0)
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%s: ftruncate: %s",
                         path, strerror(errno));
            close(fd);
        }
    }

    /* The title's saves from the shared root, once (xbox_path_adopt_legacy_saves).
     * Not with RECOMP_SAVE_DIR: the run named its saves itself. */
    if (legacy_root[0]) {
        const char* ov = recomp_env(RENV_SAVE_DIR);
        if (!(ov && ov[0]))
            xbox_path_adopt_legacy_saves(legacy_root, s_save_dir, s_title_id);
    }

    /* RECOMP_SAVE_DIR: the root for the title's UDATA, TDATA and CACHE,
     * created if missing, so a scripted run can start from an empty save. */
    {
        const char* ov = recomp_env(RENV_SAVE_DIR);
        if (ov && ov[0]) {
            snprintf(s_user_dir, sizeof(s_user_dir), "%s", ov);
            strip_trailing_slash(s_user_dir);
            mkdir_p(s_user_dir);
        } else {
            snprintf(s_user_dir, sizeof(s_user_dir), "%s", s_save_dir);
        }

        /* Saves still in the game dir stay there (s_user_in_game). */
        for (int k = 0; k < 2; k++) {
            static const char *names[2] = { "UDATA", "TDATA" };
            char in_save[MAX_PATH], in_game[MAX_PATH];
            struct stat sa, sg;
            snprintf(in_save, sizeof(in_save), "%s/%s", s_save_dir, names[k]);
            snprintf(in_game, sizeof(in_game), "%s/%s", s_game_dir, names[k]);
            s_user_in_game[k] = !(ov && ov[0]) && stat(in_save, &sa) != 0
                             && stat(in_game, &sg) == 0 && S_ISDIR(sg.st_mode);
            if (s_user_in_game[k])
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                         "%s: saves found here and not in the save dir; kept "
                         "here (move it to %s to use the save dir)",
                         in_game, in_save);
        }
    }

    atomic_store_explicit(&s_initialized, 1, memory_order_release);
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%s, save=%s, user data=%s",
             s_game_dir, s_save_dir, s_user_dir);
}

int xbox_path_hdd_root(char *out, size_t n)
{
    if (!path_ready() || !n || strlen(s_save_dir) >= n)
        return 0;
    memcpy(out, s_save_dir, strlen(s_save_dir) + 1);
    return 1;
}

int xbox_path_save_root(int tdata, char *out, size_t n)
{
    const char *base;
    int w;
    if (!path_ready() || !n)
        return 0;
    base = s_user_in_game[tdata ? 1 : 0] ? s_game_dir : s_user_dir;
    w = snprintf(out, n, "%s/%s", base, tdata ? "TDATA" : "UDATA");
    return w > 0 && (size_t)w < n;
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char* remainder = NULL;
    const char* base_dir  = NULL;
    const char* sub_dir   = NULL;
    int         skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!path_ready())
        xbox_path_init(NULL, NULL);

    {
        char linked[512];
        if (resolve_symlink(xbox_path, linked, sizeof(linked)))
            return xbox_translate_path(linked, host_path_buf, buf_size);
    }

    {
        int digit = xbox_partition_device_digit(xbox_path);
        if (digit) {
            snprintf(host_path_buf, buf_size, "%s/Partition%c.img",
                     s_save_dir, digit);
            fprintf(stderr, "  [PATH] %s -> partition image\n", xbox_path);
            fflush(stderr);
            return TRUE;
        }
    }

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_rule(xbox_path, &s_rules[i]);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save == 2
                          ? (user_rule_in_game(&s_rules[i]) ? s_game_dir : s_user_dir)
                      : s_rules[i].to_save     ? s_save_dir : s_game_dir;
            sub_dir   = s_rules[i].sub_posix;
            goto translate;
        }
    }

    /* Unrecognized path: pass through, just normalize separators. */
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    snprintf(host_path_buf, buf_size, "%s", xbox_path);
    for (char* p = host_path_buf; *p; p++)
        if (*p == '\\') *p = '/';
    return TRUE;

translate:
    {
        char remainder_posix[MAX_PATH];
        size_t fixed;
        snprintf(remainder_posix, sizeof(remainder_posix), "%s", remainder);

        /* Xbox paths use backslashes -> POSIX slashes. */
        for (char* p = remainder_posix; *p; p++)
            if (*p == '\\') *p = '/';

        if (sub_dir) {
            fixed = (size_t)snprintf(host_path_buf, buf_size, "%s%s",
                                     base_dir, sub_dir);
            snprintf(host_path_buf, buf_size, "%s%s/%s",
                     base_dir, sub_dir, remainder_posix);

            /* Ensure the save directory tree exists. */
            char dir_path[MAX_PATH];
            snprintf(dir_path, sizeof(dir_path), "%s%s", base_dir, sub_dir);
            mkdir_p(dir_path);
        } else {
            fixed = strlen(base_dir);
            snprintf(host_path_buf, buf_size, "%s/%s", base_dir, remainder_posix);
        }

        {
            size_t n = strlen(host_path_buf);
            while (n > 1 && host_path_buf[n - 1] == '/')
                host_path_buf[--n] = '\0';
        }
        if (fixed < strlen(host_path_buf))
            fix_case(host_path_buf, fixed);

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %s", xbox_path, host_path_buf);
        /* As the Win32 backend does: the bridge reads back the host path
         * the call it just made translated (the missing-file report, the
         * FMV hook). Bytes widen one to one; the readers narrow them the
         * same way, so ASCII host paths survive and others are out of
         * scope on both backends. */
        {
            size_t k;
            for (k = 0; k + 1 < MAX_PATH && host_path_buf[k]; k++)
                s_last_host_path_shared[k] = (wchar_t)(unsigned char)host_path_buf[k];
            s_last_host_path_shared[k] = 0;
        }
        return TRUE;
    }
}

#endif /* _WIN32 */

/* Both hosts: it only builds on the hdd root each half resolves. */
int xbox_path_shader_cache(char *out, size_t n)
{
    static const char sub[] = "/ShaderCache";
    size_t l;
    if (n <= sizeof sub || !xbox_path_hdd_root(out, n - (sizeof sub - 1)))
        return 0;
    for (l = 0; out[l]; l++)
        if (out[l] == '\\')
            out[l] = '/';
    while (l > 1 && out[l - 1] == '/')
        l--;
    memcpy(out + l, sub, sizeof sub);
    return 1;
}

/* ======================================================================== */
/* Saves from the shared default root
 *
 * Builds before the per-title default kept every game's saves in one
 * <base>/xboxrecomp, as UDATA/<title id> and TDATA/<title id>. The first time
 * a title runs with its own root, its two folders are copied in from there.
 * The shared root is only read: never written, moved or deleted, so a failed
 * or unwanted copy loses nothing.
 *
 * Each folder is copied to <tid>.copying.<pid>, flushed, and then renamed,
 * so a crash midway never leaves a half save under the real name, and two
 * instances started together never write into one copy: the first rename
 * wins and the other finds the folder in place. A copy left by a process
 * that is gone is removed by the next attempt. The parent (UDATA, TDATA) is
 * made only for the copy and removed again when the copy fails and leaves it
 * empty: an empty UDATA in the root would hide saves still kept in the game
 * folder (s_user_in_game) until the next run.
 *
 * An empty folder under the real name counts as absent: the game makes an
 * empty TDATA/<tid> at boot, so a run after a failed copy would otherwise
 * find it and give up. Every failed folder leaves .legacy-saves-failed-<folder>
 * in the root, which says in words what happened. While that folder in the
 * root stays empty, the next run copies again; once the player has saved
 * into it, it is never overwritten, and each run logs that the saves in the
 * shared root wait for a copy by hand (then delete the note). Without the
 * note such a folder would count as present and the marker would leave the
 * old saves behind unreported, so a note that cannot be written is logged.
 * A note whose folder is gone from a shared root still in place is removed:
 * nothing is left to copy. One whose whole <NAME> folder is missing there
 * (a root that is unmounted or unreadable for now) is kept and fails this
 * run, so no marker is written and a run with the root back copies again.
 *
 * A rename that loses to a folder with entries, or a note next to one, asks
 * whether that folder holds the copy: every entry at the top of the shared
 * root's folder, with the same name and last write, since a copy keeps the
 * times. Another instance's copy does, even after its game added a save; a
 * folder another instance's game made during the copy does not, and fails
 * this folder with the note, since the old saves were not copied. Names
 * alone would not tell them apart: a game with fixed slot names makes the
 * same ones. A folder that holds the copy counts as present, and an
 * earlier note for it is removed. An empty source folder is held by any
 * folder: nothing in it is left to copy. Only the times are compared, so
 * the test fails safe: a copy on a file system with coarser times than
 * the shared root's, a linked entry on Win32 (its own time is read, not its
 * target's), or a .DS_Store, desktop.ini or Thumbs.db that a file browser
 * rewrote in the shared root all read as not a copy and leave the note.
 *
 * The marker is written once both folders are in place (copied now, or
 * already there) and stops later runs from copying again, so a save the
 * player deleted in the new root does not come back. Any failure writes no
 * marker. Caches, partition images and the
 * shader cache are not copied: they are scratch, and the shared Cache holds
 * other games' pages, which is what the per-title root is for. */

#define LEGACY_SAVES_MARKER ".legacy-saves"
#define LEGACY_SAVES_FAILED ".legacy-saves-failed-"

/* Leaves the note that copying <legacy_root>/<name>/<tid> failed (see above);
 * one from an earlier run is kept as it is. */
static void write_failed_note(const char *failnote, const char *legacy_root,
                              const char *root, const char *name, uint32_t tid)
{
    char text[MAX_PATH * 6];

    snprintf(text, sizeof text,
             "The saves in %s%c%s%c%08x were not copied to %s%c%s%c%08x (the "
             "log says why). They are still in the shared root. While the new "
             "folder is missing or empty, the next run copies them again. Once "
             "a save is made there, check whether it already holds them; if "
             "not, copy the old ones by hand without replacing newer saves, "
             "then delete this file.", legacy_root, XBOX_HOST_SEP, name,
             XBOX_HOST_SEP, (unsigned)tid, root, XBOX_HOST_SEP, name,
             XBOX_HOST_SEP, (unsigned)tid);
    if (host_write_new(failnote, text) != 0 && !host_exists(failnote))
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                 "%s: cannot write the note. If a save is made in %s%c%s%c%08x "
                 "before a later run copies the old one, the saves in "
                 "%s%c%s%c%08x are no longer reported: copy them by hand",
                 failnote, root, XBOX_HOST_SEP, name, XBOX_HOST_SEP, (unsigned)tid,
                 legacy_root, XBOX_HOST_SEP, name, XBOX_HOST_SEP, (unsigned)tid);
}

int xbox_path_adopt_legacy_saves(const char *legacy_root, const char *root,
                                 uint32_t title_id)
{
    static const char *names[2] = { "UDATA", "TDATA" };
    char marker[MAX_PATH * 2], note[MAX_PATH * 4], present[64];
    size_t used, pused = 0;
    int copied = 0, failed = 0;

    if (!legacy_root || !root || !title_id)
        return 0;
    if ((size_t)snprintf(marker, sizeof marker, "%s%c" LEGACY_SAVES_MARKER,
                         root, XBOX_HOST_SEP) >= sizeof marker) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%s: path too long for the "
                 "saves marker; saves are not copied from %s", root, legacy_root);
        return -1;
    }
    if (host_exists(marker))
        return 0;
    /* "saves from <shared root>: copied UDATA/<tid>; already present
     * TDATA/<tid>", or "...: nothing for this title". */
    used = (size_t)snprintf(note, sizeof note, "saves from %s:", legacy_root);
    present[0] = '\0';

    for (int k = 0; k < 2; k++) {
        char src[MAX_PATH * 2], dst[MAX_PATH * 2], tmp[MAX_PATH * 2], parent[MAX_PATH * 2];
        char failnote[MAX_PATH * 2], stale[32];
        int made_parent = 0;

        snprintf(stale, sizeof stale, "%08x.copying", (unsigned)title_id);
        /* The note first: a failure further down can then leave it. Its
         * length depends on the root alone, so a root too long for it fails
         * every run the same way, before any copy or marker. */
        if ((size_t)snprintf(failnote, sizeof failnote, "%s%c" LEGACY_SAVES_FAILED
                             "%s", root, XBOX_HOST_SEP, names[k]) >= sizeof failnote
            || strlen(failnote) >= MAX_PATH) {
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                     "%s: path too long; %s%c%s%c%08x is not copied",
                     root, legacy_root, XBOX_HOST_SEP, names[k], XBOX_HOST_SEP,
                     (unsigned)title_id);
            failed = 1;
            continue;
        }
        /* tmp holds the pid, so a later run may fit where this one did not. */
        if ((size_t)snprintf(src, sizeof src, "%s%c%s%c%08x", legacy_root,
                             XBOX_HOST_SEP, names[k], XBOX_HOST_SEP,
                             (unsigned)title_id) >= sizeof src
            || (size_t)snprintf(parent, sizeof parent, "%s%c%s", root,
                                XBOX_HOST_SEP, names[k]) >= sizeof parent
            || (size_t)snprintf(dst, sizeof dst, "%s%c%08x", parent,
                                XBOX_HOST_SEP, (unsigned)title_id) >= sizeof dst
            || (size_t)snprintf(tmp, sizeof tmp, "%s%c%s.%lu", parent,
                                XBOX_HOST_SEP, stale, host_pid()) >= sizeof tmp
            || strlen(tmp) >= MAX_PATH) {
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                     "%s: path too long; %s%c%s%c%08x is not copied this run",
                     root, legacy_root, XBOX_HOST_SEP, names[k], XBOX_HOST_SEP,
                     (unsigned)title_id);
            write_failed_note(failnote, legacy_root, root, names[k], title_id);
            failed = 1;
            continue;
        }
        if (!host_is_dir(src)) {
            char lname[MAX_PATH * 2];
            /* Nothing left to copy, unless the whole folder is missing for
             * now (an unmounted root): then the note stays, and so does the
             * retry, which a marker would end. */
            if ((size_t)snprintf(lname, sizeof lname, "%s%c%s", legacy_root,
                                 XBOX_HOST_SEP, names[k]) < sizeof lname
                && host_is_dir(lname))
                host_unlink(failnote);
            else if (host_exists(failnote)) {
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                         "%s: missing, so %s stays and the next run looks "
                         "again", lname, failnote);
                failed = 1;
            }
            continue;
        }
        if (host_exists(dst) && !host_dir_empty(dst)) {
            host_clear_stale(parent, stale);
            if (host_exists(failnote) && host_holds_copy(dst, src)) {
                /* An earlier run lost the rename to another instance's
                 * copy, or the player copied by hand: the note is stale. */
                host_unlink(failnote);
            } else if (host_exists(failnote)) {
                /* Saved into after an earlier copy failed: the player's
                 * now, never overwritten. */
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                         "%s holds saves made after copying %s failed in an "
                         "earlier run, so it is not overwritten. The saves in %s "
                         "stay there: copy them by hand, then delete %s",
                         dst, src, src, failnote);
                failed = 1;
                continue;
            }
            /* The player's own, or an earlier copy: kept as it is. */
            pused += (size_t)snprintf(present + pused, sizeof present - pused,
                                      " %s%c%08x", names[k], XBOX_HOST_SEP,
                                      (unsigned)title_id);
            continue;
        }
        if (!host_is_dir(parent)) {
            if (host_mkdir(parent) != 0 && !host_is_dir(parent)) {
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%s: cannot create it; "
                         "%s is not copied this run", parent, src);
                write_failed_note(failnote, legacy_root, root, names[k], title_id);
                failed = 1;
                continue;
            }
            made_parent = 1;
        }
        host_clear_stale(parent, stale);
        /* rename replaces dst when it is an empty folder. */
        if (host_copy_tree(src, tmp) == 0 && host_rename(tmp, dst) == 0) {
            host_sync_dir(parent);
            host_unlink(failnote);
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                     "saves copied from the shared root: %s -> %s (the original "
                     "stays where it was)", src, dst);
            if (used < sizeof note)
                used += (size_t)snprintf(note + used, sizeof note - used, "%s %s%c%08x",
                                         copied ? "" : " copied", names[k],
                                         XBOX_HOST_SEP, (unsigned)title_id);
            copied++;
            continue;
        }
        host_remove_tree(tmp);
        if (host_exists(dst) && !host_dir_empty(dst)) {
            if (host_holds_copy(dst, src)) {
                /* Another instance's copy got there first. */
                host_unlink(failnote);
                pused += (size_t)snprintf(present + pused, sizeof present - pused,
                                          " %s%c%08x", names[k], XBOX_HOST_SEP,
                                          (unsigned)title_id);
                continue;
            }
            /* Not a copy: another instance's game saved there while this
             * one copied, or its copy lost the times or had an entry
             * replaced since (see above). */
            write_failed_note(failnote, legacy_root, root, names[k], title_id);
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                     "%s does not hold a copy of %s (another instance's game "
                     "saved there, or its copy lost the times), so it is not "
                     "overwritten. Check whether it already holds the saves in "
                     "%s; if not, copy them by hand, then delete %s",
                     dst, src, src, failnote);
            failed = 1;
            continue;
        }
        if (made_parent)
            host_rmdir(parent);
        write_failed_note(failnote, legacy_root, root, names[k], title_id);
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                 "copying the saves %s to %s failed; they are still in %s. "
                 "The next run tries again while %s is missing or empty; once "
                 "a save is made there, copy them by hand", src, dst, src, dst);
        failed = 1;
    }

    if (failed)
        return -1;
    if (pused && used < sizeof note)
        used += (size_t)snprintf(note + used, sizeof note - used,
                                 "%s already present%s", copied ? ";" : "", present);
    else if (!copied && used < sizeof note)
        snprintf(note + used, sizeof note - used, " nothing for this title");
    /* The folders' names in the root reach the disk before the marker that
     * says they are there. */
    host_sync_dir(root);
    if (host_write_new(marker, note) != 0 && !host_exists(marker))
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%s: cannot write the marker; "
                 "the next run checks the shared root again", marker);
    return copied;
}
