/*
 * kernel_file.c - Xbox File I/O
 *
 * Implements the Nt*File kernel functions. All Xbox device paths are
 * translated through kernel_path.c before use.
 *
 * The Xbox kernel uses NT-style file I/O with ANSI strings in
 * OBJECT_ATTRIBUTES (unlike Windows NT, which uses Unicode).
 *
 * Two backends:
 *   _WIN32  -> Win32 CreateFileW / ReadFile / FindFirstFileW ...
 *   POSIX   -> open / read / write / stat / opendir ...
 * The Xbox semantics (disposition mapping, IO_STATUS_BLOCK, info classes)
 * are identical on both; only the host syscalls differ.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* FNM_CASEFOLD */
#endif
#include "kernel.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <fnmatch.h>
#endif

/* Get the ANSI path from OBJECT_ATTRIBUTES (platform-independent). */
static const char* get_xbox_path(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    if (!ObjectAttributes || !ObjectAttributes->ObjectName ||
        !ObjectAttributes->ObjectName->Buffer)
        return NULL;
    return ObjectAttributes->ObjectName->Buffer;
}

/* Xbox volume geometry.
 *
 * FATX uses 16 KB clusters: 512-byte sectors, 32 sectors per cluster. That is
 * not cosmetic. A title's CRT startup asks for FileFsSizeInformation and
 * multiplies SectorsPerAllocationUnit by BytesPerSector, then *requires* the
 * product to equal the cluster size it was built for. Half-Life 2 checks for
 * 0x4000 and returns STATUS_DEVICE_NOT_READY (0xC000014F) otherwise, which
 * aborts CRT init before main ever runs -- the process then exits cleanly,
 * which reads as a title that did nothing rather than one that failed.
 *
 * Reporting the host's PC-typical 4 KB cluster (512 x 8) fails that check. */
#define XBOX_BYTES_PER_SECTOR       512u
#define XBOX_SECTORS_PER_CLUSTER    32u      /* 512 * 32 = 16384 */

/* ======================================================================== */
/* Shared by both backends. kernel_bridge.c calls xbox_LastFileError()
 * unconditionally, so defining it only under _WIN32 leaves every POSIX link
 * with an undefined symbol -- invisible until something links an executable,
 * because a static archive never resolves its own references.
 *
 * Both backends set it on a failed open: the Win32 error, or on POSIX the
 * Win32 code its errno corresponds to (NtCreateFile only). */
uint32_t g_xbox_last_file_error;

uint32_t xbox_LastFileError(void)
{
    return g_xbox_last_file_error;
}

#if defined(_WIN32)
/* ====================  Win32 backend  =================================== */
/* ======================================================================== */

/* Convert Xbox create disposition to Win32 */
static DWORD xbox_disposition_to_win32(ULONG Disposition)
{
    switch (Disposition) {
        case XBOX_FILE_SUPERSEDE:    return CREATE_ALWAYS;
        case XBOX_FILE_OPEN:         return OPEN_EXISTING;
        case XBOX_FILE_CREATE:       return CREATE_NEW;
        case XBOX_FILE_OPEN_IF:      return OPEN_ALWAYS;
        case XBOX_FILE_OVERWRITE:    return TRUNCATE_EXISTING;
        case XBOX_FILE_OVERWRITE_IF: return CREATE_ALWAYS;
        default:                     return OPEN_EXISTING;
    }
}

/* Convert Xbox access mask to Win32 */
static DWORD xbox_access_to_win32(ACCESS_MASK Access)
{
    DWORD result = 0;
    if (Access & XBOX_GENERIC_READ)           result |= GENERIC_READ;
    if (Access & XBOX_GENERIC_WRITE)          result |= GENERIC_WRITE;
    if (Access & XBOX_GENERIC_ALL)            result |= GENERIC_ALL;
    if (Access & XBOX_FILE_READ_DATA)         result |= FILE_READ_DATA;
    if (Access & XBOX_FILE_WRITE_DATA)        result |= FILE_WRITE_DATA;
    if (Access & XBOX_FILE_APPEND_DATA)       result |= FILE_APPEND_DATA;
    if (Access & XBOX_FILE_READ_ATTRIBUTES)   result |= FILE_READ_ATTRIBUTES;
    if (Access & XBOX_FILE_WRITE_ATTRIBUTES)  result |= FILE_WRITE_ATTRIBUTES;
    if (Access & XBOX_SYNCHRONIZE)            result |= SYNCHRONIZE;
    if (Access & XBOX_DELETE)                  result |= DELETE;
    if (result == 0 || result == SYNCHRONIZE)
        result |= GENERIC_READ;
    return result;
}

/* Convert Xbox share access to Win32 */
static DWORD xbox_share_to_win32(ULONG Share)
{
    DWORD result = 0;
    if (Share & 0x01) result |= FILE_SHARE_READ;
    if (Share & 0x02) result |= FILE_SHARE_WRITE;
    if (Share & 0x04) result |= FILE_SHARE_DELETE;
    return result;
}

/* FATX overwrite semantics (Win32 side; the POSIX backend below has the
 * same rule and the long story, at s_overwrite_pending).
 *
 * An overwrite open (FILE_SUPERSEDE, FILE_OVERWRITE, FILE_OVERWRITE_IF) of an
 * existing file sets its size to 0 on FATX, but FATX keeps no valid-data
 * length, so a later SetEndOfFile that grows the file brings back the bytes
 * already on disk. A title's z: texture cache can depend on it: it writes
 * x.tmp, reopens it with CREATE_ALWAYS, only sets the end of file to the true
 * size, and renames it to x.dat. CREATE_ALWAYS / TRUNCATE_EXISTING on NTFS (and
 * Wine's ftruncate) zero-fill on the regrow, so every cache file came out as
 * zeros of the right length.
 *
 * So an overwrite of an existing non-empty file opens it with OPEN_EXISTING
 * and marks the handle pending. If the first thing done with it is setting
 * the end of file, that is a plain SetEndOfFile to the new size, which keeps
 * the bytes below it. Anything else (read, write, size query, allocation,
 * close) first truncates to 0, the normal overwrite. A position query does
 * not settle: XAPI SetEndOfFile makes one before it sets the end of file. */
#define FATX_PENDING_MAX 64
static HANDLE s_overwrite_pending[FATX_PENDING_MAX];
static SRWLOCK s_overwrite_lock = SRWLOCK_INIT;

static int disposition_overwrites(ULONG disposition)
{
    return disposition == XBOX_FILE_SUPERSEDE
        || disposition == XBOX_FILE_OVERWRITE
        || disposition == XBOX_FILE_OVERWRITE_IF;
}

/* Mark h pending. Returns 0 if the table is full. */
static int overwrite_mark(HANDLE h)
{
    int i, ok = 0;
    AcquireSRWLockExclusive(&s_overwrite_lock);
    for (i = 0; i < FATX_PENDING_MAX; i++)
        if (!s_overwrite_pending[i]) { s_overwrite_pending[i] = h; ok = 1; break; }
    ReleaseSRWLockExclusive(&s_overwrite_lock);
    return ok;
}

/* Take the pending mark without truncating. Returns 1 if h was pending. */
static int overwrite_take(HANDLE h)
{
    int i, was = 0;
    if (!h || h == INVALID_HANDLE_VALUE)
        return 0;
    AcquireSRWLockExclusive(&s_overwrite_lock);
    for (i = 0; i < FATX_PENDING_MAX; i++)
        if (s_overwrite_pending[i] == h) { s_overwrite_pending[i] = NULL; was = 1; break; }
    ReleaseSRWLockExclusive(&s_overwrite_lock);
    return was;
}

/* Truncate to 0, keeping the file pointer where it was. */
static void truncate_to_zero(HANDLE h)
{
    LARGE_INTEGER cur, zero;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(h, zero, &cur, FILE_CURRENT))
        cur.QuadPart = 0;
    SetFilePointerEx(h, zero, NULL, FILE_BEGIN);
    if (!SetEndOfFile(h))
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                 "overwrite: truncate failed err=%u", GetLastError());
    SetFilePointerEx(h, cur, NULL, FILE_BEGIN);
}

/* Finish a pending overwrite: truncate to 0. */
static void overwrite_settle(HANDLE h)
{
    if (overwrite_take(h))
        truncate_to_zero(h);
}

/* The missing-file report's high/low split: a miss under a directory that
 * exists is a file the dump lacks; one under a directory that does not is
 * usually a title probing a loose-file layout it never shipped. Same answer
 * as the POSIX version below for the same tree. */
int xbox_host_parent_exists(const xbox_host_char *host_path)
{
    WCHAR dir[MAX_PATH];
    const WCHAR *cut;
    DWORD attrs;

    if (!host_path)
        return 0;
    cut = wcsrchr(host_path, L'\\');
    {
        const WCHAR *fwd = wcsrchr(host_path, L'/');
        if (fwd && (!cut || fwd > cut)) cut = fwd;
    }
    if (!cut || cut == host_path || (size_t)(cut - host_path) >= MAX_PATH)
        return 0;
    memcpy(dir, host_path, (size_t)(cut - host_path) * sizeof(WCHAR));
    dir[cut - host_path] = 0;
    attrs = GetFileAttributesW(dir);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/* Translate an Xbox OBJECT_ATTRIBUTES path to a Win32 wide path */
static BOOL translate_obj_path(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
                               WCHAR* win_path, DWORD buf_size)
{
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path)
        return FALSE;
    if (ObjectAttributes->RootDirectory && xbox_path[0] != '\\' &&
        !(xbox_path[0] && xbox_path[1] == ':')) {
        /* XDeleteSaveGame opens each child relative to its save directory. */
        DWORD used = GetFinalPathNameByHandleW(ObjectAttributes->RootDirectory,
            win_path, buf_size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!used || used >= buf_size || used + 1 >= buf_size) return FALSE;
        if (win_path[used - 1] != L'\\') win_path[used++] = L'\\';
        int count = MultiByteToWideChar(CP_ACP, 0, xbox_path, -1,
            win_path + used, (int)(buf_size - used));
        return count != 0;
    }
    return xbox_translate_path(xbox_path, win_path, buf_size);
}

NTSTATUS __stdcall xbox_NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions)
{
    WCHAR win_path[MAX_PATH];
    HANDLE h;
    DWORD flags_and_attrs = FILE_ATTRIBUTE_NORMAL;
    (void)AllocationSize;

    if (!FileHandle || !ObjectAttributes)
        return STATUS_INVALID_PARAMETER;

    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_FILE, "NtCreateFile: path translation failed");
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    /* A partition device opened as a directory.
     *
     * The path layer maps \Device\Harddisk0\PartitionN to a PartitionN.img
     * backing file, but a title asking for free space opens the partition with
     * FILE_DIRECTORY_FILE | FILE_OPEN_FOR_FREE_SPACE_QUERY -- it wants the
     * volume, not the bytes. Opening a regular file as a directory fails, and
     * the title reads STATUS_OBJECT_PATH_NOT_FOUND as "no such volume".
     *
     * Half-Life 2's CRT probes partition0 this way during startup and treats
     * the failure as fatal. Redirecting to the directory that holds the image
     * gives a handle that is valid for exactly what the caller is going to do
     * with it, which is NtQueryVolumeInformationFile. */
    if ((CreateOptions & XBOX_FILE_DIRECTORY_FILE) &&
        GetFileAttributesW(win_path) != INVALID_FILE_ATTRIBUTES &&
        !(GetFileAttributesW(win_path) & FILE_ATTRIBUTE_DIRECTORY)) {
        WCHAR *slash = wcsrchr(win_path, L'\\');
        if (slash && slash != win_path) {
            *slash = 0;
            xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE,
                     "NtCreateFile: directory open of a device image, "
                     "using its containing directory instead");
        }
    }

    if (CreateOptions & XBOX_FILE_DIRECTORY_FILE) {
        if (CreateDisposition == XBOX_FILE_CREATE || CreateDisposition == XBOX_FILE_OPEN_IF)
            CreateDirectoryW(win_path, NULL);
        h = CreateFileW(win_path, xbox_access_to_win32(DesiredAccess),
            xbox_share_to_win32(ShareAccess), NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, NULL);
    } else {
        if (CreateOptions & XBOX_FILE_NO_INTERMEDIATE_BUFFERING)
            flags_and_attrs |= FILE_FLAG_NO_BUFFERING;
        if (FileAttributes & XBOX_FILE_ATTRIBUTE_READONLY)
            flags_and_attrs |= FILE_ATTRIBUTE_READONLY;
        DWORD access = xbox_access_to_win32(DesiredAccess);
        WIN32_FILE_ATTRIBUTE_DATA fad;
        /* An overwrite of an existing non-empty file is deferred (see
         * s_overwrite_pending). It needs write access to settle later. */
        int defer = disposition_overwrites(CreateDisposition)
                 && (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA))
                 && GetFileAttributesExW(win_path, GetFileExInfoStandard, &fad)
                 && !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                 && (fad.nFileSizeHigh || fad.nFileSizeLow);
        h = INVALID_HANDLE_VALUE;
        if (defer) {
            h = CreateFileW(win_path, access, xbox_share_to_win32(ShareAccess),
                NULL, OPEN_EXISTING, flags_and_attrs, NULL);
            if (h != INVALID_HANDLE_VALUE)
                overwrite_take(h);          /* a stale mark; see below */
            if (h != INVALID_HANDLE_VALUE && !overwrite_mark(h))
                truncate_to_zero(h);        /* no slot to defer in */
            else if (h != INVALID_HANDLE_VALUE)
                XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile: overwrite of %S deferred", win_path);
        }
        if (h == INVALID_HANDLE_VALUE) {    /* not deferred, or it vanished */
            h = CreateFileW(win_path, access,
                xbox_share_to_win32(ShareAccess), NULL,
                xbox_disposition_to_win32(CreateDisposition), flags_and_attrs, NULL);
            /* A HANDLE value is reused once closed, and a pending handle
             * closed by DuplicateHandle(DUPLICATE_CLOSE_SOURCE) never went
             * through xbox_NtClose, so its mark may still be in the table.
             * Clear it, or this unrelated file is truncated on first use. */
            if (h != INVALID_HANDLE_VALUE)
                overwrite_take(h);
        }
    }

    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        /* Kept for the caller's trace. An NTSTATUS says "it did not open";
         * only the Win32 error distinguishes a title probing for a file that
         * is genuinely absent from one it cannot open because this runtime
         * already holds it open with a share mode the second open forbids --
         * and those need opposite responses. */
        g_xbox_last_file_error = (uint32_t)err;
        /* A warning, not a compiled-out trace: a failed open is how a title
         * silently decides a volume or asset is missing, and in a Release
         * build that decision was invisible. */
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                 "NtCreateFile FAILED: %S (err=%u)", win_path, err);
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_OBJECT_NAME_NOT_FOUND;
            IoStatusBlock->Information = 0;
        }
        switch (err) {
            case ERROR_FILE_NOT_FOUND: return STATUS_OBJECT_NAME_NOT_FOUND;
            case ERROR_PATH_NOT_FOUND: return STATUS_OBJECT_PATH_NOT_FOUND;
            case ERROR_ACCESS_DENIED:  return STATUS_ACCESS_DENIED;
            case ERROR_ALREADY_EXISTS: return STATUS_OBJECT_NAME_COLLISION;
            case ERROR_TOO_MANY_OPEN_FILES: return STATUS_TOO_MANY_OPENED_FILES;
            default:                   return STATUS_UNSUCCESSFUL;
        }
    }

    *FileHandle = h;
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = (CreateDisposition == XBOX_FILE_CREATE) ? 2 : 1;
    }
    XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile: %S -> handle=%p", win_path, h);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReadFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    DWORD bytes_read = 0;
    BOOL result;
    OVERLAPPED ov;
    (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    overwrite_settle(FileHandle);
    if (ByteOffset && ByteOffset->QuadPart >= 0) {
        memset(&ov, 0, sizeof(ov));
        ov.Offset = ByteOffset->LowPart;
        ov.OffsetHigh = ByteOffset->HighPart;
        result = ReadFile(FileHandle, Buffer, Length, &bytes_read, &ov);
    } else {
        result = ReadFile(FileHandle, Buffer, Length, &bytes_read, NULL);
    }

    if (result || GetLastError() == ERROR_HANDLE_EOF) {
        IoStatusBlock->Information = bytes_read;
        if (bytes_read == 0 && Length > 0) {
            IoStatusBlock->Status = STATUS_END_OF_FILE;
            return STATUS_END_OF_FILE;
        }
        IoStatusBlock->Status = STATUS_SUCCESS;
        if (Event) SetEvent(Event);
        return STATUS_SUCCESS;
    }

    XBOX_TRACE(XBOX_LOG_FILE, "NtReadFile(handle=%p, len=%u) failed err=%u",
               FileHandle, Length, GetLastError());
    IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
    IoStatusBlock->Information = 0;
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall xbox_NtWriteFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    DWORD bytes_written = 0;
    BOOL result;
    OVERLAPPED ov;
    (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    overwrite_settle(FileHandle);
    if (ByteOffset && ByteOffset->QuadPart >= 0) {
        memset(&ov, 0, sizeof(ov));
        ov.Offset = ByteOffset->LowPart;
        ov.OffsetHigh = ByteOffset->HighPart;
        result = WriteFile(FileHandle, Buffer, Length, &bytes_written, &ov);
    } else {
        result = WriteFile(FileHandle, Buffer, Length, &bytes_written, NULL);
    }

    if (result) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = bytes_written;
        if (Event) SetEvent(Event);
        return STATUS_SUCCESS;
    }

    XBOX_TRACE(XBOX_LOG_FILE, "NtWriteFile(handle=%p, len=%u) failed err=%u",
               FileHandle, Length, GetLastError());
    IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
    IoStatusBlock->Information = 0;
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall xbox_NtClose(HANDLE Handle)
{
    XBOX_TRACE(XBOX_LOG_FILE, "NtClose(handle=%p)", Handle);
    if (Handle && Handle != INVALID_HANDLE_VALUE) {
        overwrite_settle(Handle);
        xbox_dir_context_drop(Handle);
        CloseHandle(Handle);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_HANDLE;
}

NTSTATUS __stdcall xbox_NtDeleteFile(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    WCHAR win_path[MAX_PATH];
    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    XBOX_TRACE(XBOX_LOG_FILE, "NtDeleteFile: %S", win_path);
    if (DeleteFileW(win_path))    return STATUS_SUCCESS;
    if (RemoveDirectoryW(win_path)) return STATUS_SUCCESS;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS __stdcall xbox_NtQueryInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    /* Not for a position query: XAPI SetEndOfFile makes one before it
     * sets the end of file, and a pending overwrite must survive it. */
    if (FileInformationClass != XboxFilePositionInformation)
        overwrite_settle(FileHandle);

    switch (FileInformationClass) {
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            BY_HANDLE_FILE_INFORMATION fi;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return STATUS_UNSUCCESSFUL;
            info->CreationTime.LowPart    = fi.ftCreationTime.dwLowDateTime;
            info->CreationTime.HighPart   = fi.ftCreationTime.dwHighDateTime;
            info->LastAccessTime.LowPart  = fi.ftLastAccessTime.dwLowDateTime;
            info->LastAccessTime.HighPart = fi.ftLastAccessTime.dwHighDateTime;
            info->LastWriteTime.LowPart   = fi.ftLastWriteTime.dwLowDateTime;
            info->LastWriteTime.HighPart  = fi.ftLastWriteTime.dwHighDateTime;
            info->ChangeTime = info->LastWriteTime;
            info->FileAttributes = fi.dwFileAttributes;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_BASIC_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileStandardInformation: {
            PXBOX_FILE_STANDARD_INFORMATION info = (PXBOX_FILE_STANDARD_INFORMATION)FileInformation;
            BY_HANDLE_FILE_INFORMATION fi;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return STATUS_UNSUCCESSFUL;
            info->AllocationSize.QuadPart = ((LONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
            info->AllocationSize.QuadPart = (info->AllocationSize.QuadPart + 4095) & ~4095LL;
            info->EndOfFile.QuadPart = ((LONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
            info->NumberOfLinks = fi.nNumberOfLinks;
            info->DeletePending = FALSE;
            info->Directory = (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? TRUE : FALSE;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_STANDARD_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            LARGE_INTEGER pos, zero;
            zero.QuadPart = 0;
            if (!SetFilePointerEx(FileHandle, zero, &pos, FILE_CURRENT))
                return STATUS_UNSUCCESSFUL;
            info->CurrentByteOffset = pos;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_POSITION_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileNetworkOpenInformation: {
            PXBOX_FILE_NETWORK_OPEN_INFORMATION info = (PXBOX_FILE_NETWORK_OPEN_INFORMATION)FileInformation;
            BY_HANDLE_FILE_INFORMATION fi;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return STATUS_UNSUCCESSFUL;
            info->CreationTime.LowPart    = fi.ftCreationTime.dwLowDateTime;
            info->CreationTime.HighPart   = fi.ftCreationTime.dwHighDateTime;
            info->LastAccessTime.LowPart  = fi.ftLastAccessTime.dwLowDateTime;
            info->LastAccessTime.HighPart = fi.ftLastAccessTime.dwHighDateTime;
            info->LastWriteTime.LowPart   = fi.ftLastWriteTime.dwLowDateTime;
            info->LastWriteTime.HighPart  = fi.ftLastWriteTime.dwHighDateTime;
            info->ChangeTime = info->LastWriteTime;
            info->EndOfFile.QuadPart = ((LONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
            info->AllocationSize.QuadPart = (info->EndOfFile.QuadPart + 4095) & ~4095LL;
            info->FileAttributes = fi.dwFileAttributes;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_NETWORK_OPEN_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryInformationFile: unhandled class %d", FileInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtSetInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FileInformationClass) {
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            if (!SetFilePointerEx(FileHandle, info->CurrentByteOffset, NULL, FILE_BEGIN)) {
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                    "SetFilePosition failed: offset=%lld err=%u",
                    (long long)info->CurrentByteOffset.QuadPart, GetLastError());
                return STATUS_UNSUCCESSFUL;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileEndOfFileInformation: {
            PXBOX_FILE_END_OF_FILE_INFORMATION info = (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            LARGE_INTEGER cur, zero = {0};
            /* A pending overwrite ends here: SetEndOfFile to the new size
             * keeps the old bytes below it (see s_overwrite_pending). */
            overwrite_take(FileHandle);
            SetFilePointerEx(FileHandle, zero, &cur, FILE_CURRENT);
            SetFilePointerEx(FileHandle, info->EndOfFile, NULL, FILE_BEGIN);
            if (!SetEndOfFile(FileHandle)) {
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                    "SetEndOfFile failed: size=%lld err=%u",
                    (long long)info->EndOfFile.QuadPart, GetLastError());
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
                return STATUS_UNSUCCESSFUL;
            }
            if (cur.QuadPart <= info->EndOfFile.QuadPart)
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileAllocationInformation: {
            /* Reserve space for a file. Halo's save path calls this before
             * writing, and an unimplemented class here returned
             * STATUS_NOT_IMPLEMENTED, which the title turned into DOS error
             * 317 and reported as "couldn't open or create saved game file".
             *
             * Same payload shape as EndOfFile: one LARGE_INTEGER. Windows
             * FileAllocationInfo is the direct equivalent; if the filesystem
             * declines it, fall back to setting the size, since the caller
             * only needs the space to exist. */
            PXBOX_FILE_END_OF_FILE_INFORMATION info =
                (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            FILE_ALLOCATION_INFO fai;
            overwrite_settle(FileHandle);
            fai.AllocationSize = info->EndOfFile;
            if (!SetFileInformationByHandle(FileHandle, FileAllocationInfo,
                                            &fai, sizeof(fai))) {
                LARGE_INTEGER cur, zero = {0};
                SetFilePointerEx(FileHandle, zero, &cur, FILE_CURRENT);
                SetFilePointerEx(FileHandle, info->EndOfFile, NULL, FILE_BEGIN);
                SetEndOfFile(FileHandle);
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileDispositionInformation: {
            PXBOX_FILE_DISPOSITION_INFORMATION info = (PXBOX_FILE_DISPOSITION_INFORMATION)FileInformation;
            FILE_DISPOSITION_INFO fdi;
            fdi.DeleteFile = info->DeleteFile;
            if (!SetFileInformationByHandle(FileHandle, FileDispositionInfo, &fdi, sizeof(fdi)))
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                         "SetFileDispositionInfo failed: err=%u", GetLastError());
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            FILETIME ct, at, wt;
            ct.dwLowDateTime = info->CreationTime.LowPart;
            ct.dwHighDateTime = info->CreationTime.HighPart;
            at.dwLowDateTime = info->LastAccessTime.LowPart;
            at.dwHighDateTime = info->LastAccessTime.HighPart;
            wt.dwLowDateTime = info->LastWriteTime.LowPart;
            wt.dwHighDateTime = info->LastWriteTime.HighPart;
            SetFileTime(FileHandle, &ct, &at, &wt);
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        default:
            /* stderr, not xbox_log: WARN is filtered out by default, and an
             * unimplemented info class is exactly the kind of silent gap that
             * surfaces far away. Halo's save path hits one, gets
             * STATUS_NOT_IMPLEMENTED, converts it to DOS error 317 and asserts
             * "couldn't open or create saved game file". */
            fprintf(stderr, "  [FILE] NtSetInformationFile: unhandled class %d\n",
                    (int)FileInformationClass);
            fflush(stderr);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtQueryVolumeInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FsInformation, ULONG Length, XBOX_FS_INFORMATION_CLASS FsInformationClass)
{
    (void)FileHandle; (void)Length;
    if (!IoStatusBlock || !FsInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FsInformationClass) {
        case XboxFileFsSizeInformation: {
            PXBOX_FILE_FS_SIZE_INFORMATION info = (PXBOX_FILE_FS_SIZE_INFORMATION)FsInformation;
            ULARGE_INTEGER free_bytes, total_bytes, total_free;
            if (GetDiskFreeSpaceExW(NULL, &free_bytes, &total_bytes, &total_free)) {
                info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
                info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
                ULONGLONG cs = (ULONGLONG)info->BytesPerSector * info->SectorsPerAllocationUnit;
                info->TotalAllocationUnits.QuadPart = total_bytes.QuadPart / cs;
                info->AvailableAllocationUnits.QuadPart = free_bytes.QuadPart / cs;
            } else {
                info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
                info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
                info->TotalAllocationUnits.QuadPart = 1048576;
                info->AvailableAllocationUnits.QuadPart = 524288;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_FS_SIZE_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryVolumeInformationFile: unhandled class %d", FsInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtFlushBuffersFile(HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    FlushFileBuffers(FileHandle);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryFullAttributesFile(
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    PXBOX_FILE_NETWORK_OPEN_INFORMATION FileInformation)
{
    WCHAR win_path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fad;

    if (!FileInformation)
        return STATUS_INVALID_PARAMETER;
    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;

    if (!GetFileAttributesExW(win_path, GetFileExInfoStandard, &fad)) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        return STATUS_UNSUCCESSFUL;
    }

    FileInformation->CreationTime.LowPart    = fad.ftCreationTime.dwLowDateTime;
    FileInformation->CreationTime.HighPart   = fad.ftCreationTime.dwHighDateTime;
    FileInformation->LastAccessTime.LowPart  = fad.ftLastAccessTime.dwLowDateTime;
    FileInformation->LastAccessTime.HighPart = fad.ftLastAccessTime.dwHighDateTime;
    FileInformation->LastWriteTime.LowPart   = fad.ftLastWriteTime.dwLowDateTime;
    FileInformation->LastWriteTime.HighPart  = fad.ftLastWriteTime.dwHighDateTime;
    FileInformation->ChangeTime = FileInformation->LastWriteTime;
    FileInformation->EndOfFile.QuadPart = ((LONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    FileInformation->AllocationSize.QuadPart = (FileInformation->EndOfFile.QuadPart + 4095) & ~4095LL;
    FileInformation->FileAttributes = fad.dwFileAttributes;
    return STATUS_SUCCESS;
}

#define MAX_DIR_CONTEXTS 64
typedef struct {
    HANDLE file_handle;
    HANDLE find_handle;
    BOOL   first_done;
    WIN32_FIND_DATAW find_data;
} DIR_CONTEXT;

static DIR_CONTEXT s_dir_contexts[MAX_DIR_CONTEXTS];
static CRITICAL_SECTION s_dir_cs;
static BOOL s_dir_cs_init = FALSE;

static DIR_CONTEXT* find_or_create_dir_context(HANDLE FileHandle, BOOL create)
{
    if (!s_dir_cs_init) { InitializeCriticalSection(&s_dir_cs); s_dir_cs_init = TRUE; }
    EnterCriticalSection(&s_dir_cs);
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].file_handle == FileHandle && s_dir_contexts[i].find_handle != NULL) {
            LeaveCriticalSection(&s_dir_cs);
            return &s_dir_contexts[i];
        }
    }
    if (!create) { LeaveCriticalSection(&s_dir_cs); return NULL; }
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].find_handle == NULL) {
            s_dir_contexts[i].file_handle = FileHandle;
            s_dir_contexts[i].first_done = FALSE;
            LeaveCriticalSection(&s_dir_cs);
            return &s_dir_contexts[i];
        }
    }
    LeaveCriticalSection(&s_dir_cs);
    return NULL;
}

/* A closed directory handle takes its enumeration with it. Host handle values
 * are reused, and a search abandoned part-way (a title that stops at the first
 * match) otherwise carried on under the next directory opened at the same
 * value: its first answer was the old search's next entry. */
void xbox_dir_context_drop(HANDLE FileHandle)
{
    if (!s_dir_cs_init)
        return;
    EnterCriticalSection(&s_dir_cs);
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].file_handle == FileHandle) {
            if (s_dir_contexts[i].find_handle &&
                s_dir_contexts[i].find_handle != INVALID_HANDLE_VALUE)
                FindClose(s_dir_contexts[i].find_handle);
            s_dir_contexts[i].find_handle = NULL;
            s_dir_contexts[i].file_handle = NULL;
            s_dir_contexts[i].first_done = FALSE;
        }
    }
    LeaveCriticalSection(&s_dir_cs);
}

NTSTATUS __stdcall xbox_NtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    XBOX_FILE_INFORMATION_CLASS FileInformationClass,
    PXBOX_ANSI_STRING FileName, BOOLEAN RestartScan)
{
    DIR_CONTEXT* ctx;
    PXBOX_FILE_DIRECTORY_INFORMATION entry;
    (void)Event; (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;
    IoStatusBlock->Information = 0;
    if (FileInformationClass != XboxFileDirectoryInformation) {
        IoStatusBlock->Status = STATUS_INVALID_INFO_CLASS;
        return STATUS_INVALID_INFO_CLASS;
    }

    ctx = find_or_create_dir_context(FileHandle, TRUE);
    if (!ctx)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (RestartScan || !ctx->first_done) {
        if (ctx->find_handle && ctx->find_handle != INVALID_HANDLE_VALUE) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
        }
        WCHAR search_path[MAX_PATH];
        WCHAR dir_path[MAX_PATH];
        DWORD path_len = GetFinalPathNameByHandleW(FileHandle, dir_path, MAX_PATH,
                                                   FILE_NAME_NORMALIZED);
        if (path_len == 0 || path_len >= MAX_PATH) {
            IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
            return STATUS_UNSUCCESSFUL;
        }
        WCHAR* clean_path = dir_path;
        if (wcsncmp(clean_path, L"\\\\?\\", 4) == 0)
            clean_path += 4;
        if (FileName && FileName->Buffer) {
            WCHAR pattern_wide[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, FileName->Buffer, FileName->Length,
                                pattern_wide, MAX_PATH);
            pattern_wide[FileName->Length] = L'\0';
            swprintf_s(search_path, MAX_PATH, L"%s\\%s", clean_path, pattern_wide);
        } else {
            swprintf_s(search_path, MAX_PATH, L"%s\\*", clean_path);
        }
        ctx->find_handle = FindFirstFileW(search_path, &ctx->find_data);
        if (ctx->find_handle == INVALID_HANDLE_VALUE) {
            ctx->find_handle = NULL;
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
        ctx->first_done = TRUE;
    } else {
        if (!FindNextFileW(ctx->find_handle, &ctx->find_data)) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
            ctx->file_handle = NULL;
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
    }

    /* FATX enumeration never exposes the host's dot directories. */
    while (!wcscmp(ctx->find_data.cFileName, L".") ||
           !wcscmp(ctx->find_data.cFileName, L"..")) {
        if (!FindNextFileW(ctx->find_handle, &ctx->find_data)) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
            ctx->file_handle = NULL;
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
    }

    entry = (PXBOX_FILE_DIRECTORY_INFORMATION)FileInformation;
    memset(entry, 0, Length);
    char filename_ansi[MAX_PATH];
    int name_len = WideCharToMultiByte(CP_ACP, 0, ctx->find_data.cFileName, -1,
                                       filename_ansi, MAX_PATH, NULL, NULL);
    if (name_len > 0) name_len--;

    entry->NextEntryOffset = 0;
    entry->FileIndex = 0;
    entry->CreationTime.LowPart   = ctx->find_data.ftCreationTime.dwLowDateTime;
    entry->CreationTime.HighPart  = ctx->find_data.ftCreationTime.dwHighDateTime;
    entry->LastAccessTime.LowPart = ctx->find_data.ftLastAccessTime.dwLowDateTime;
    entry->LastAccessTime.HighPart = ctx->find_data.ftLastAccessTime.dwHighDateTime;
    entry->LastWriteTime.LowPart  = ctx->find_data.ftLastWriteTime.dwLowDateTime;
    entry->LastWriteTime.HighPart = ctx->find_data.ftLastWriteTime.dwHighDateTime;
    entry->ChangeTime = entry->LastWriteTime;
    entry->EndOfFile.QuadPart = ((LONGLONG)ctx->find_data.nFileSizeHigh << 32) | ctx->find_data.nFileSizeLow;
    entry->AllocationSize.QuadPart = (entry->EndOfFile.QuadPart + 4095) & ~4095LL;
    entry->FileAttributes = ctx->find_data.dwFileAttributes;
    entry->FileNameLength = name_len;
    {
        ULONG header_size = (ULONG)((ULONG_PTR)&((PXBOX_FILE_DIRECTORY_INFORMATION)0)->FileName);
        if (name_len > 0 && (header_size + name_len) <= Length)
            memcpy(entry->FileName, filename_ansi, name_len);
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = header_size + name_len;
    }
    return STATUS_SUCCESS;
}


/* Rename an open file. See the POSIX version for the cases; this one asks
 * the handle to do it (FileRenameInfo), which needs DELETE access, and falls
 * back to MoveFileExW on the path the handle names. Tested under Proton
 * only. */
NTSTATUS __stdcall xbox_RenameFileByHandle(
    HANDLE FileHandle, PXBOX_OBJECT_ATTRIBUTES NewName, BOOLEAN ReplaceIfExists)
{
    WCHAR src[MAX_PATH], dst[MAX_PATH];
    const char *name = get_xbox_path(NewName);
    DWORD n;

    if (!name || !*name)
        return STATUS_INVALID_PARAMETER;
    n = GetFinalPathNameByHandleW(FileHandle, src, MAX_PATH,
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= MAX_PATH)
        return STATUS_INVALID_HANDLE;

    if (!NewName->RootDirectory && !strchr(name, '\\') && !strchr(name, ':')) {
        WCHAR *slash = wcsrchr(src, L'\\');
        size_t dir = slash ? (size_t)(slash - src) + 1 : 0;
        memcpy(dst, src, dir * sizeof(WCHAR));
        if (!MultiByteToWideChar(CP_ACP, 0, name, -1, dst + dir,
                                 (int)(MAX_PATH - dir)))
            return STATUS_OBJECT_NAME_INVALID;
    } else if (!translate_obj_path(NewName, dst, MAX_PATH)) {
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    {
        size_t len = wcslen(dst);
        size_t bytes = sizeof(FILE_RENAME_INFO) + len * sizeof(WCHAR);
        FILE_RENAME_INFO *fri = (FILE_RENAME_INFO *)calloc(1, bytes);
        BOOL ok = FALSE;
        if (fri) {
            fri->ReplaceIfExists = ReplaceIfExists ? TRUE : FALSE;
            fri->RootDirectory = NULL;
            fri->FileNameLength = (DWORD)(len * sizeof(WCHAR));
            memcpy(fri->FileName, dst, (len + 1) * sizeof(WCHAR));
            ok = SetFileInformationByHandle(FileHandle, FileRenameInfo, fri,
                                            (DWORD)bytes);
            free(fri);
        }
        if (!ok)
            ok = MoveFileExW(src, dst,
                             ReplaceIfExists ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!ok) {
            DWORD e = GetLastError();
            return e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS
                 ? STATUS_OBJECT_NAME_COLLISION
                 : e == ERROR_ACCESS_DENIED ? STATUS_ACCESS_DENIED
                 : STATUS_UNSUCCESSFUL;
        }
    }
    fprintf(stderr, "  [FILE] rename %S -> %S\n", src, dst);
    fflush(stderr);
    return STATUS_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  POSIX backend  =================================== */
/* ======================================================================== */

/* Convert Xbox access mask + disposition to POSIX open() flags. */
static int posix_open_flags(ACCESS_MASK access, ULONG disposition)
{
    int wantWrite = (access & (XBOX_GENERIC_WRITE | XBOX_GENERIC_ALL |
                               XBOX_FILE_WRITE_DATA | XBOX_FILE_APPEND_DATA)) != 0;
    int rw = wantWrite ? O_RDWR : O_RDONLY;
    int extra;

    switch (disposition) {
        case XBOX_FILE_SUPERSEDE:    extra = O_CREAT | O_TRUNC; break;
        case XBOX_FILE_OPEN:         extra = 0;                 break;
        case XBOX_FILE_CREATE:       extra = O_CREAT | O_EXCL;  break;
        case XBOX_FILE_OPEN_IF:      extra = O_CREAT;           break;
        case XBOX_FILE_OVERWRITE:    extra = O_TRUNC;           break;
        case XBOX_FILE_OVERWRITE_IF: extra = O_CREAT | O_TRUNC; break;
        default:                     extra = 0;                 break;
    }
    /* O_TRUNC / O_CREAT imply write intent */
    if ((extra & (O_TRUNC | O_CREAT)) && rw == O_RDONLY)
        rw = O_RDWR;
    if (access & XBOX_FILE_APPEND_DATA)
        extra |= O_APPEND;
    return rw | extra;
}

/* FATX overwrite semantics.
 *
 * An overwrite open (FILE_SUPERSEDE, FILE_OVERWRITE, FILE_OVERWRITE_IF) of an
 * existing file sets its size to 0 on FATX, but FATX keeps no valid-data
 * length: growing the file again with SetEndOfFile, without writing, brings
 * back the bytes already on disk. A title can rely on this when it builds a
 * utility-drive texture cache:
 *
 *   CreateFile(x.tmp, CREATE_ALWAYS, NO_BUFFERING|OVERLAPPED)
 *   SetEndOfFile(size rounded up to 512); WriteFile(all of it); CloseHandle
 *   CreateFile(x.tmp, CREATE_ALWAYS)      <- the second overwrite
 *   SetEndOfFile(true size); CloseHandle; rename x.tmp -> x.dat
 *
 * The second open only trims the sector padding. With O_TRUNC the host
 * filesystem zero-fills on the regrow, so every cache file came out as zeros
 * of the right length, and the game later loaded zero textures from them.
 *
 * So an overwrite of an existing non-empty file is opened without O_TRUNC
 * and marked pending. If the first thing done with the handle is setting
 * the end of file, that is a plain ftruncate to the new size, which keeps
 * the old bytes below it as FATX does. Anything else (read, write, size
 * query, allocation, close) first truncates to 0, which is the normal
 * overwrite. The table is indexed by host fd. */
#define FATX_PENDING_MAX 4096
static volatile unsigned char s_overwrite_pending[FATX_PENDING_MAX];

static int disposition_overwrites(ULONG disposition)
{
    return disposition == XBOX_FILE_SUPERSEDE
        || disposition == XBOX_FILE_OVERWRITE
        || disposition == XBOX_FILE_OVERWRITE_IF;
}

/* Finish a pending overwrite: truncate to 0. */
static void overwrite_settle(int fd)
{
    if (fd >= 0 && fd < FATX_PENDING_MAX && s_overwrite_pending[fd]) {
        s_overwrite_pending[fd] = 0;
        if (ftruncate(fd, 0) != 0) { /* nothing more to do */ }
    }
}

/* Take the pending mark without truncating (end-of-file set). Returns 1 if
 * the handle was pending. */
static int overwrite_take(int fd)
{
    if (fd >= 0 && fd < FATX_PENDING_MAX && s_overwrite_pending[fd]) {
        s_overwrite_pending[fd] = 0;
        return 1;
    }
    return 0;
}

static void unix_to_filetime(time_t sec, long nsec, LARGE_INTEGER* out)
{
    /* 100-ns ticks since 1601-01-01 */
    ULONGLONG t = 116444736000000000ULL
                + (ULONGLONG)sec * 10000000ULL
                + (ULONGLONG)nsec / 100ULL;
    out->LowPart  = (DWORD)(t & 0xFFFFFFFFULL);
    out->HighPart = (LONG)(t >> 32);
}

static ULONG mode_to_xbox_attrs(mode_t m)
{
    ULONG a = 0;
    if (S_ISDIR(m))      a |= XBOX_FILE_ATTRIBUTE_DIRECTORY;
    if (!(m & S_IWUSR))  a |= XBOX_FILE_ATTRIBUTE_READONLY;
    if (a == 0)          a = XBOX_FILE_ATTRIBUTE_NORMAL;
    return a;
}

/* See the Win32 version: the same test, the same answer. */
int xbox_host_parent_exists(const xbox_host_char *host_path)
{
    char dir[MAX_PATH];
    const char *cut;
    struct stat st;

    if (!host_path)
        return 0;
    cut = strrchr(host_path, '/');
    if (!cut || cut == host_path || (size_t)(cut - host_path) >= sizeof dir)
        return 0;
    memcpy(dir, host_path, (size_t)(cut - host_path));
    dir[cut - host_path] = '\0';
    return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

static NTSTATUS errno_to_status(int e)
{
    switch (e) {
        case ENOENT:  return STATUS_OBJECT_NAME_NOT_FOUND;
        case ENOTDIR: return STATUS_OBJECT_PATH_NOT_FOUND;
        case EACCES:
        case EPERM:   return STATUS_ACCESS_DENIED;
        case EEXIST:  return STATUS_OBJECT_NAME_COLLISION;
        case ENOMEM:  return STATUS_NO_MEMORY;
        case EMFILE:
        case ENFILE:  return STATUS_TOO_MANY_OPENED_FILES;
        default:      return STATUS_UNSUCCESSFUL;
    }
}

NTSTATUS __stdcall xbox_NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions)
{
    char host_path[MAX_PATH];
    (void)AllocationSize; (void)FileAttributes; (void)ShareAccess;

    if (!FileHandle || !ObjectAttributes)
        return STATUS_INVALID_PARAMETER;

    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path || !xbox_translate_path(xbox_path, host_path, MAX_PATH)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_FILE, "NtCreateFile: path translation failed");
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    int fd;
    /* A partition device opened as a directory: the volume, for a free-space
     * query, rather than the image's bytes. As in the Win32 backend, the
     * directory holding the image answers that. */
    if (CreateOptions & XBOX_FILE_DIRECTORY_FILE) {
        struct stat st;
        if (stat(host_path, &st) == 0 && S_ISREG(st.st_mode)) {
            char *slash = strrchr(host_path, '/');
            if (slash && slash != host_path) {
                *slash = '\0';
                xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE,
                         "NtCreateFile: directory open of a device image, "
                         "using its containing directory instead");
            }
        }
    }
    if (CreateOptions & XBOX_FILE_DIRECTORY_FILE) {
        if (CreateDisposition == XBOX_FILE_CREATE || CreateDisposition == XBOX_FILE_OPEN_IF)
            mkdir(host_path, 0755);   /* EEXIST is fine */
        fd = open(host_path, O_RDONLY | O_DIRECTORY);
    } else {
        int flags = posix_open_flags(DesiredAccess, CreateDisposition);
        struct stat st;
        int defer = disposition_overwrites(CreateDisposition)
                 && stat(host_path, &st) == 0 && S_ISREG(st.st_mode)
                 && st.st_size > 0;
        if (defer)
            flags &= ~O_TRUNC;
        fd = open(host_path, flags, 0644);
        if (fd >= 0 && fd < FATX_PENDING_MAX)
            s_overwrite_pending[fd] = (unsigned char)defer;
        else if (fd >= 0 && defer && ftruncate(fd, 0) != 0) {
            /* no slot to defer in: truncate now */
        }
    }

    if (fd < 0) {
        int e = errno;
        NTSTATUS st = errno_to_status(e);
        /* ENOENT covers a missing file and a missing directory alike. The
         * console and the Win32 backend (ERROR_PATH_NOT_FOUND) tell them
         * apart, and a title may branch on which, so POSIX does too. */
        int no_dir = e == ENOTDIR || (e == ENOENT && !xbox_host_parent_exists(host_path));
        if (no_dir)
            st = STATUS_OBJECT_PATH_NOT_FOUND;
        /* The Win32 code, as the Win32 backend keeps it, for the bridge's
         * FAILED line: it read win32 err=0 on every POSIX failure. */
        g_xbox_last_file_error = no_dir ? 3u
                               : e == ENOENT ? 2u
                               : (e == EACCES || e == EPERM) ? 5u
                               : e == EEXIST ? 80u
                               : (e == EMFILE || e == ENFILE) ? 4u : 0u;
        /* Not the title's fault, and easy to mistake for a missing file:
         * on stderr, where a tester's log has it. */
        if (e == EMFILE || e == ENFILE) {
            static volatile LONG warned;
            if (InterlockedExchange(&warned, 1) == 0) {
                fprintf(stderr, "  [FILE] out of host file descriptors with %d "
                        "files open (%s): raise the open-file limit\n",
                        w32_open_file_handles(), host_path);
                fflush(stderr);
            }
        }
        XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile FAILED: %s (errno=%d)", host_path, e);
        if (IoStatusBlock) {
            IoStatusBlock->Status = st;
            IoStatusBlock->Information = 0;
        }
        return st;
    }

    *FileHandle = w32_open_handle(fd, host_path);
    if (!*FileHandle) {                 /* the handle closed fd */
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_NO_MEMORY;
            IoStatusBlock->Information = 0;
        }
        return STATUS_NO_MEMORY;
    }
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = (CreateDisposition == XBOX_FILE_CREATE) ? 2 : 1;
    }
    XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile: %s -> handle=%p", host_path, *FileHandle);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReadFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0) {
        IoStatusBlock->Status = STATUS_INVALID_HANDLE;
        return STATUS_INVALID_HANDLE;
    }

    overwrite_settle(fd);
    if (ByteOffset && ByteOffset->QuadPart >= 0)
        lseek(fd, (off_t)ByteOffset->QuadPart, SEEK_SET);

    ssize_t n = read(fd, Buffer, Length);
    if (n < 0) {
        XBOX_TRACE(XBOX_LOG_FILE, "NtReadFile(handle=%p) errno=%d", FileHandle, errno);
        IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
        IoStatusBlock->Information = 0;
        return STATUS_UNSUCCESSFUL;
    }

    IoStatusBlock->Information = (ULONG_PTR)n;
    if (n == 0 && Length > 0) {
        IoStatusBlock->Status = STATUS_END_OF_FILE;
        return STATUS_END_OF_FILE;
    }
    IoStatusBlock->Status = STATUS_SUCCESS;
    if (Event) SetEvent(Event);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtWriteFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0) {
        IoStatusBlock->Status = STATUS_INVALID_HANDLE;
        return STATUS_INVALID_HANDLE;
    }

    overwrite_settle(fd);
    if (ByteOffset && ByteOffset->QuadPart >= 0)
        lseek(fd, (off_t)ByteOffset->QuadPart, SEEK_SET);

    ssize_t n = write(fd, Buffer, Length);
    if (n < 0) {
        XBOX_TRACE(XBOX_LOG_FILE, "NtWriteFile(handle=%p) errno=%d", FileHandle, errno);
        IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
        IoStatusBlock->Information = 0;
        return STATUS_UNSUCCESSFUL;
    }

    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = (ULONG_PTR)n;
    if (Event) SetEvent(Event);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtClose(HANDLE Handle)
{
    XBOX_TRACE(XBOX_LOG_FILE, "NtClose(handle=%p)", Handle);
    if (Handle && Handle != INVALID_HANDLE_VALUE) {
        overwrite_settle(w32_handle_fd(Handle));
        /* Before CloseHandle: once the object is freed its address can come
         * back as the next handle, which must not inherit this search. */
        xbox_dir_context_drop(Handle);
        CloseHandle(Handle);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_HANDLE;
}

NTSTATUS __stdcall xbox_NtDeleteFile(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    char host_path[MAX_PATH];
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path || !xbox_translate_path(xbox_path, host_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    XBOX_TRACE(XBOX_LOG_FILE, "NtDeleteFile: %s", host_path);
    if (unlink(host_path) == 0) return STATUS_SUCCESS;
    if (rmdir(host_path)  == 0) return STATUS_SUCCESS;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS __stdcall xbox_NtQueryInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0)
        return STATUS_INVALID_HANDLE;

    struct stat st;
    if (FileInformationClass != XboxFilePositionInformation) {
        /* Not for a position query: XAPI SetEndOfFile makes one before it
         * sets the end of file, and a pending overwrite must survive it. */
        overwrite_settle(fd);
        if (fstat(fd, &st) != 0)
            return STATUS_UNSUCCESSFUL;
    }

    switch (FileInformationClass) {
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            unix_to_filetime(st.st_ctime, 0, &info->CreationTime);
            unix_to_filetime(st.st_atime, 0, &info->LastAccessTime);
            unix_to_filetime(st.st_mtime, 0, &info->LastWriteTime);
            info->ChangeTime = info->LastWriteTime;
            info->FileAttributes = mode_to_xbox_attrs(st.st_mode);
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_BASIC_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileStandardInformation: {
            PXBOX_FILE_STANDARD_INFORMATION info = (PXBOX_FILE_STANDARD_INFORMATION)FileInformation;
            info->EndOfFile.QuadPart = st.st_size;
            info->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
            info->NumberOfLinks = (ULONG)st.st_nlink;
            info->DeletePending = FALSE;
            info->Directory = S_ISDIR(st.st_mode) ? TRUE : FALSE;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_STANDARD_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            off_t pos = lseek(fd, 0, SEEK_CUR);
            if (pos < 0) return STATUS_UNSUCCESSFUL;
            info->CurrentByteOffset.QuadPart = pos;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_POSITION_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileNetworkOpenInformation: {
            PXBOX_FILE_NETWORK_OPEN_INFORMATION info = (PXBOX_FILE_NETWORK_OPEN_INFORMATION)FileInformation;
            unix_to_filetime(st.st_ctime, 0, &info->CreationTime);
            unix_to_filetime(st.st_atime, 0, &info->LastAccessTime);
            unix_to_filetime(st.st_mtime, 0, &info->LastWriteTime);
            info->ChangeTime = info->LastWriteTime;
            info->EndOfFile.QuadPart = st.st_size;
            info->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
            info->FileAttributes = mode_to_xbox_attrs(st.st_mode);
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_NETWORK_OPEN_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryInformationFile: unhandled class %d", FileInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtSetInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0)
        return STATUS_INVALID_HANDLE;

    switch (FileInformationClass) {
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            if (lseek(fd, (off_t)info->CurrentByteOffset.QuadPart, SEEK_SET) < 0)
                return STATUS_UNSUCCESSFUL;
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileEndOfFileInformation: {
            PXBOX_FILE_END_OF_FILE_INFORMATION info = (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            /* A pending overwrite ends here: ftruncate to the new size keeps
             * the old bytes below it (see s_overwrite_pending). */
            overwrite_take(fd);
            if (ftruncate(fd, (off_t)info->EndOfFile.QuadPart) != 0)
                return STATUS_UNSUCCESSFUL;
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileAllocationInformation: {
            /* Reserve space before writing. The Win32 backend already handles
             * this; POSIX fell through to STATUS_NOT_IMPLEMENTED. A title calls it as it copies textures
             * into the utility-drive cache.
             *
             * NT semantics: an allocation below the end of file truncates to
             * it; one above only reserves space and leaves the size alone.
             * POSIX has no portable reservation, and the caller only needs the
             * write that follows to succeed, so growing is a no-op. */
            PXBOX_FILE_END_OF_FILE_INFORMATION info =
                (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            struct stat st;
            overwrite_settle(fd);
            if (fstat(fd, &st) == 0 && (off_t)info->EndOfFile.QuadPart < st.st_size
                    && ftruncate(fd, (off_t)info->EndOfFile.QuadPart) != 0)
                return STATUS_UNSUCCESSFUL;
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileDispositionInformation: {
            PXBOX_FILE_DISPOSITION_INFORMATION info = (PXBOX_FILE_DISPOSITION_INFORMATION)FileInformation;
            /* POSIX: unlinking an open file removes it on last close -- this
             * matches NT "delete on close" semantics exactly. */
            if (info->DeleteFile) {
                const char* p = w32_handle_path(FileHandle);
                if (p) unlink(p);
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileBasicInformation:
            /* Setting file times is non-essential for the game; accept it. */
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        default:
            /* stderr, not xbox_log: WARN is filtered out by default, and an
             * unimplemented info class is exactly the kind of silent gap that
             * surfaces far away. Halo's save path hits one, gets
             * STATUS_NOT_IMPLEMENTED, converts it to DOS error 317 and asserts
             * "couldn't open or create saved game file". */
            fprintf(stderr, "  [FILE] NtSetInformationFile: unhandled class %d\n",
                    (int)FileInformationClass);
            fflush(stderr);
            return STATUS_NOT_IMPLEMENTED;
    }
}

/* Rename an open file (NtSetInformationFile, FileRenameInformation).
 *
 * A title can build each utility-drive cache file as z:\...\name.tmp, then
 * rename it to name.dat and later open the .dat. With this class
 * unimplemented the rename failed, so every .dat open failed with
 * STATUS_OBJECT_NAME_NOT_FOUND and the title rebuilt the cache instead.
 *
 * The new name is resolved the way NT does:
 *   RootDirectory set    relative to that directory handle
 *   bare name, no root   within the file's own directory
 *   anything else        a full Xbox path, through the path table
 * POSIX rename() replaces an existing target, so ReplaceIfExists = FALSE is
 * checked first. The handle keeps working (the fd follows the inode), and its
 * recorded path is updated so a later delete-on-close removes the new name. */
NTSTATUS __stdcall xbox_RenameFileByHandle(
    HANDLE FileHandle, PXBOX_OBJECT_ATTRIBUTES NewName, BOOLEAN ReplaceIfExists)
{
    char dst[MAX_PATH];
    const char *src = w32_handle_path(FileHandle);
    const char *name = get_xbox_path(NewName);
    struct stat st;

    if (!src)
        return STATUS_INVALID_HANDLE;
    if (!name || !*name)
        return STATUS_INVALID_PARAMETER;

    if (NewName->RootDirectory
            || (!strchr(name, '\\') && !strchr(name, ':'))) {
        const char *base = NewName->RootDirectory
                         ? w32_handle_path(NewName->RootDirectory) : src;
        const char *slash;
        size_t dir;
        char *p;

        if (!base)
            return STATUS_INVALID_HANDLE;
        if (NewName->RootDirectory) {
            dir = strlen(base);
        } else {
            slash = strrchr(base, '/');
            dir = slash ? (size_t)(slash - base) : 0;
        }
        if (dir + 1 + strlen(name) >= sizeof dst)
            return STATUS_OBJECT_NAME_INVALID;
        memcpy(dst, base, dir);
        dst[dir] = '/';
        strcpy(dst + dir + 1, name[0] == '\\' ? name + 1 : name);
        for (p = dst + dir + 1; *p; p++)
            if (*p == '\\') *p = '/';
    } else if (!xbox_translate_path(name, dst, MAX_PATH)) {
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    if (strcmp(src, dst) != 0) {
        if (!ReplaceIfExists && stat(dst, &st) == 0)
            return STATUS_OBJECT_NAME_COLLISION;
        if (rename(src, dst) != 0) {
            int e = errno;
            fprintf(stderr, "  [FILE] rename %s -> %s FAILED (%s)\n",
                    src, dst, strerror(e));
            fflush(stderr);
            return errno_to_status(e);
        }
    }
    fprintf(stderr, "  [FILE] rename %s -> %s\n", src, dst);
    fflush(stderr);
    w32_handle_set_path(FileHandle, dst);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryVolumeInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FsInformation, ULONG Length, XBOX_FS_INFORMATION_CLASS FsInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FsInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FsInformationClass) {
        case XboxFileFsSizeInformation: {
            PXBOX_FILE_FS_SIZE_INFORMATION info = (PXBOX_FILE_FS_SIZE_INFORMATION)FsInformation;
            struct statvfs vfs;
            int fd = w32_handle_fd(FileHandle);
            /* Xbox geometry, not the host's -- see the note on
             * XBOX_SECTORS_PER_CLUSTER above. */
            info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
            info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
            if (fd >= 0 && fstatvfs(fd, &vfs) == 0) {
                ULONGLONG cs = (ULONGLONG)info->BytesPerSector * info->SectorsPerAllocationUnit;
                ULONGLONG total = (ULONGLONG)vfs.f_blocks * vfs.f_frsize;
                ULONGLONG avail = (ULONGLONG)vfs.f_bavail * vfs.f_frsize;
                info->TotalAllocationUnits.QuadPart = total / cs;
                info->AvailableAllocationUnits.QuadPart = avail / cs;
            } else {
                info->TotalAllocationUnits.QuadPart = 1048576;
                info->AvailableAllocationUnits.QuadPart = 524288;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_FS_SIZE_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryVolumeInformationFile: unhandled class %d", FsInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtFlushBuffersFile(HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    int fd = w32_handle_fd(FileHandle);
    if (fd >= 0) fsync(fd);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryFullAttributesFile(
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    PXBOX_FILE_NETWORK_OPEN_INFORMATION FileInformation)
{
    char host_path[MAX_PATH];
    struct stat st;

    if (!FileInformation)
        return STATUS_INVALID_PARAMETER;
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path || !xbox_translate_path(xbox_path, host_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    if (stat(host_path, &st) != 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    unix_to_filetime(st.st_ctime, 0, &FileInformation->CreationTime);
    unix_to_filetime(st.st_atime, 0, &FileInformation->LastAccessTime);
    unix_to_filetime(st.st_mtime, 0, &FileInformation->LastWriteTime);
    FileInformation->ChangeTime = FileInformation->LastWriteTime;
    FileInformation->EndOfFile.QuadPart = st.st_size;
    FileInformation->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
    FileInformation->FileAttributes = mode_to_xbox_attrs(st.st_mode);
    return STATUS_SUCCESS;
}

/* Directory enumeration state, keyed by the directory's Nt handle. */
#define MAX_DIR_CONTEXTS 64
typedef struct {
    HANDLE handle;
    DIR*   dir;
    char   pattern[64];
} DIR_CONTEXT;

static DIR_CONTEXT s_dir_contexts[MAX_DIR_CONTEXTS];
static CRITICAL_SECTION s_dir_cs;
static BOOL s_dir_cs_init = FALSE;

/* As on Win32: a closed directory handle takes its enumeration with it. The
 * handle is a heap object, and the allocator hands a freed one straight back,
 * so a search abandoned after one result otherwise resumed under the next
 * directory opened: its first answer came from the old directory. */
void xbox_dir_context_drop(HANDLE FileHandle)
{
    if (!s_dir_cs_init)
        return;
    EnterCriticalSection(&s_dir_cs);
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].handle == FileHandle) {
            if (s_dir_contexts[i].dir)
                closedir(s_dir_contexts[i].dir);
            s_dir_contexts[i].dir = NULL;
            s_dir_contexts[i].handle = NULL;
        }
    }
    LeaveCriticalSection(&s_dir_cs);
}

NTSTATUS __stdcall xbox_NtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    XBOX_FILE_INFORMATION_CLASS FileInformationClass,
    PXBOX_ANSI_STRING FileName, BOOLEAN RestartScan)
{
    (void)Event; (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;
    IoStatusBlock->Information = 0;
    if (FileInformationClass != XboxFileDirectoryInformation) {
        IoStatusBlock->Status = STATUS_INVALID_INFO_CLASS;
        return STATUS_INVALID_INFO_CLASS;
    }

    if (!s_dir_cs_init) { InitializeCriticalSection(&s_dir_cs); s_dir_cs_init = TRUE; }
    EnterCriticalSection(&s_dir_cs);

    /* Locate or create the per-handle enumeration context. */
    DIR_CONTEXT* ctx = NULL;
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++)
        if (s_dir_contexts[i].handle == FileHandle) { ctx = &s_dir_contexts[i]; break; }
    if (!ctx) {
        for (int i = 0; i < MAX_DIR_CONTEXTS; i++)
            if (s_dir_contexts[i].handle == NULL) { ctx = &s_dir_contexts[i]; break; }
        if (!ctx) { LeaveCriticalSection(&s_dir_cs); return STATUS_INSUFFICIENT_RESOURCES; }
        ctx->handle = FileHandle;
        ctx->dir = NULL;
    }

    if (RestartScan || ctx->dir == NULL) {
        if (ctx->dir) { closedir(ctx->dir); ctx->dir = NULL; }
        const char* dpath = w32_handle_path(FileHandle);
        if (!dpath) { LeaveCriticalSection(&s_dir_cs); return STATUS_UNSUCCESSFUL; }
        ctx->dir = opendir(dpath);
        if (!ctx->dir) {
            LeaveCriticalSection(&s_dir_cs);
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
        if (FileName && FileName->Buffer && FileName->Length > 0) {
            USHORT n = FileName->Length;
            if (n >= sizeof(ctx->pattern)) n = sizeof(ctx->pattern) - 1;
            memcpy(ctx->pattern, FileName->Buffer, n);
            ctx->pattern[n] = '\0';
        } else {
            strcpy(ctx->pattern, "*");
        }
    }

    /* Advance to the next entry matching the search pattern. */
    struct dirent* de;
    const char* dpath = w32_handle_path(FileHandle);
    struct stat st;
    for (;;) {
        de = readdir(ctx->dir);
        if (!de) {
            closedir(ctx->dir);
            ctx->dir = NULL;
            ctx->handle = NULL;
            LeaveCriticalSection(&s_dir_cs);
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (fnmatch(ctx->pattern, de->d_name, FNM_CASEFOLD) == 0)
            break;
    }

    char full[MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", dpath ? dpath : ".", de->d_name);
    if (stat(full, &st) != 0)
        memset(&st, 0, sizeof(st));
    LeaveCriticalSection(&s_dir_cs);

    PXBOX_FILE_DIRECTORY_INFORMATION entry = (PXBOX_FILE_DIRECTORY_INFORMATION)FileInformation;
    memset(entry, 0, Length);

    int name_len = (int)strlen(de->d_name);
    entry->NextEntryOffset = 0;
    entry->FileIndex = 0;
    unix_to_filetime(st.st_ctime, 0, &entry->CreationTime);
    unix_to_filetime(st.st_atime, 0, &entry->LastAccessTime);
    unix_to_filetime(st.st_mtime, 0, &entry->LastWriteTime);
    entry->ChangeTime = entry->LastWriteTime;
    entry->EndOfFile.QuadPart = st.st_size;
    entry->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
    entry->FileAttributes = mode_to_xbox_attrs(st.st_mode);
    entry->FileNameLength = name_len;

    ULONG header_size = (ULONG)((ULONG_PTR)&((PXBOX_FILE_DIRECTORY_INFORMATION)0)->FileName);
    if (name_len > 0 && (header_size + (ULONG)name_len) <= Length)
        memcpy(entry->FileName, de->d_name, name_len);
    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = header_size + name_len;
    return STATUS_SUCCESS;
}

#endif /* _WIN32 */

/* ======================================================================== */
/* ====================  Platform-independent  ============================ */
/* ======================================================================== */

NTSTATUS __stdcall xbox_NtOpenFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    ULONG ShareAccess, ULONG OpenOptions)
{
    /* NtOpenFile is NtCreateFile with FILE_OPEN disposition */
    return xbox_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes,
        IoStatusBlock, NULL, 0, ShareAccess, XBOX_FILE_OPEN, OpenOptions);
}

NTSTATUS __stdcall xbox_IoCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG Disposition, ULONG CreateOptions, ULONG Options)
{
    (void)Options;
    return xbox_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
        AllocationSize, FileAttributes, ShareAccess, Disposition, CreateOptions);
}

NTSTATUS __stdcall xbox_NtFsControlFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, ULONG FsControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength)
{
    (void)FileHandle; (void)Event; (void)ApcRoutine; (void)ApcContext;
    (void)InputBuffer; (void)InputBufferLength; (void)OutputBuffer; (void)OutputBufferLength;
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE, "NtFsControlFile(0x%X) - stub", FsControlCode);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_NOT_IMPLEMENTED;
        IoStatusBlock->Information = 0;
    }
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS __stdcall xbox_NtDeviceIoControlFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, ULONG IoControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength)
{
    (void)FileHandle; (void)Event; (void)ApcRoutine; (void)ApcContext;
    (void)InputBuffer; (void)InputBufferLength; (void)OutputBuffer; (void)OutputBufferLength;
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE, "NtDeviceIoControlFile(0x%X) - stub", IoControlCode);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_NOT_IMPLEMENTED;
        IoStatusBlock->Information = 0;
    }
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS __stdcall xbox_NtOpenSymbolicLinkObject(
    PHANDLE LinkHandle, PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    /*
     * Xbox uses symbolic links for drive-letter mapping (D: -> \Device\CdRom0).
     * Path translation handles this transparently, so return a dummy handle.
     */
    if (LinkHandle)
        *LinkHandle = (HANDLE)(ULONG_PTR)0xDEAD0001;
    XBOX_TRACE(XBOX_LOG_FILE, "NtOpenSymbolicLinkObject(%s) - stub",
        get_xbox_path(ObjectAttributes) ? get_xbox_path(ObjectAttributes) : "?");
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQuerySymbolicLinkObject(
    HANDLE LinkHandle, PXBOX_ANSI_STRING LinkTarget, PULONG ReturnedLength)
{
    (void)LinkHandle;
    const char* target = "\\Device\\CdRom0";
    if (LinkTarget && LinkTarget->Buffer) {
        USHORT len = (USHORT)strlen(target);
        if (len < LinkTarget->MaximumLength) {
            memcpy(LinkTarget->Buffer, target, len + 1);
            LinkTarget->Length = len;
        }
    }
    if (ReturnedLength)
        *ReturnedLength = (ULONG)strlen(target);
    return STATUS_SUCCESS;
}
