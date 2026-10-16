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

/* Match a rule. The UDATA, TDATA and CACHE rules carry no trailing
 * separator, since a title opens the bare directories too, so they stop at
 * one: otherwise Partition1\UDATAx would be taken for Partition1\UDATA\x,
 * and an asset such as Partition1\cached.dat would move to the cache. */
static int rule_match(const char* path, const path_rule* r)
{
    int skip = match_prefix(path, r->prefix);
    if (skip && r->to_save == 2 && path[skip] && path[skip] != '\\')
        return 0;
    return skip;
}

/* Saves left in the game dir. Builds before the UDATA/TDATA rules sent all
 * of Partition1 there, so a player's saves may be in <game_dir>/UDATA. With
 * RECOMP_SAVE_DIR unset, no UDATA in the save dir and one in the game dir,
 * xbox_path_init keeps using that one (and says so); the same for TDATA.
 * Indexed 0 for UDATA, 1 for TDATA. CACHE never stays: a cache left in the
 * game dir is scratch the title rebuilds, and keeping it there is the write
 * into the game files that its rule exists to stop. */
static int s_user_in_game[2];

/* The root a to_save 2 rule maps under. By the whole name: TitleCache
 * starts with the same letter as TDATA and must not follow its saves. */
#define USER_RULE_DIR(r)                                                    \
    ((strcmp((r)->sub_posix, "/UDATA") == 0 && s_user_in_game[0]) ||        \
     (strcmp((r)->sub_posix, "/TDATA") == 0 && s_user_in_game[1])           \
         ? s_game_dir : s_user_dir)

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
        if (rule_match(out, &s_rules[i]))
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
        if (rule_match(p, &s_rules[i]))
            return s_rules[i].to_save == 2 ? XBOX_TREE_USER
                 : s_rules[i].to_save     ? XBOX_TREE_HDD : XBOX_TREE_GAME;
    }
    return XBOX_TREE_UNKNOWN;
}

#if defined(_WIN32)
/* ======================================================================== */

#include <shlobj.h>
#include <winioctl.h>

static WCHAR s_game_dir[MAX_PATH];
static WCHAR s_save_dir[MAX_PATH];
static WCHAR s_user_dir[MAX_PATH];   /* UDATA/TDATA/TitleCache root: s_save_dir or RECOMP_SAVE_DIR */
static BOOL  s_initialized = FALSE;

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

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    WCHAR save_base[MAX_PATH];

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
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, save_base))) {
            swprintf_s(s_save_dir, MAX_PATH, L"%s\\xboxrecomp", save_base);
        } else {
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
        const char *ov = getenv("RECOMP_SAVE_DIR");
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

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%S, save=%S, user data=%S",
             s_game_dir, s_save_dir, s_user_dir);
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

    if (!s_initialized)
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
        skip = rule_match(xbox_path, &s_rules[i]);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save == 2 ? USER_RULE_DIR(&s_rules[i])
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

static char s_game_dir[MAX_PATH];
static char s_save_dir[MAX_PATH];
static char s_user_dir[MAX_PATH];   /* UDATA/TDATA/TitleCache root: s_save_dir or RECOMP_SAVE_DIR */
static BOOL s_initialized = FALSE;

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

void xbox_path_init(const char* game_dir, const char* save_dir)
{
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

    /* RECOMP_SAVE_DIR: the root for the title's UDATA, TDATA and CACHE,
     * created if missing, so a scripted run can start from an empty save. */
    {
        const char* ov = getenv("RECOMP_SAVE_DIR");
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

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%s, save=%s, user data=%s",
             s_game_dir, s_save_dir, s_user_dir);
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char* remainder = NULL;
    const char* base_dir  = NULL;
    const char* sub_dir   = NULL;
    int         skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
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
        skip = rule_match(xbox_path, &s_rules[i]);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save == 2 ? USER_RULE_DIR(&s_rules[i])
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
        snprintf(remainder_posix, sizeof(remainder_posix), "%s", remainder);

        /* Xbox paths use backslashes -> POSIX slashes. */
        for (char* p = remainder_posix; *p; p++)
            if (*p == '\\') *p = '/';

        if (sub_dir) {
            snprintf(host_path_buf, buf_size, "%s%s/%s",
                     base_dir, sub_dir, remainder_posix);

            /* Ensure the save directory tree exists. */
            char dir_path[MAX_PATH];
            snprintf(dir_path, sizeof(dir_path), "%s%s", base_dir, sub_dir);
            mkdir_p(dir_path);
        } else {
            snprintf(host_path_buf, buf_size, "%s/%s", base_dir, remainder_posix);
        }

        {
            size_t n = strlen(host_path_buf);
            while (n > 1 && host_path_buf[n - 1] == '/')
                host_path_buf[--n] = '\0';
        }

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
