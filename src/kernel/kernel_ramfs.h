/*
 * kernel_ramfs.h - the title's cache partition held in host memory
 *
 * On by default (RECOMP_STORAGE_RAM_CACHE, config storage.ram_cache; 0 or
 * false turns it off).
 * With it on, the paths kernel_path.c sends to <hdd>/Cache -- Z:, and
 * Partition3-5 below the device -- are served from memory instead, capped at
 * the stock 750 MB partition and seeded read-only from <hdd>/Cache when it
 * starts. Nothing is written back. When the seed did not fit under the cap
 * (a shared or stale Cache dir) and a write needs room, seeds the title has
 * neither opened nor listed this run are dropped from memory, oldest first
 * (the host copies stay), so the leftovers cannot starve the
 * title's own files. A Cache dir that fit is never touched.
 *
 * The console's utility drives are scratch: the dashboard and other titles
 * clear them, so a title cannot count on what it left there. BLiNX 1 rewrites
 * a 570 MB ring of cache files at about one file a second while it plays,
 * about 7 GB an hour of host SSD writes on disk.
 *
 * kernel_file.c's xbox_Nt*File functions ask the store first. Each xbox_ramfs_*
 * call returns 1 when the path or handle is the store's (the NTSTATUS in *st)
 * and 0 to leave it to the disk backend.
 */
#ifndef KERNEL_RAMFS_H
#define KERNEL_RAMFS_H

#include "kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The stock Z: partition: 0x177000 sectors of 512 bytes (kernel_path.c). */
#define XBOX_RAMFS_STOCK_CAP (0x00177000ull * 512ull)

/* On or off; called before the guest starts (the enhance layer does, from
 * storage.ram_cache). Without a call, the first file call reads
 * RECOMP_STORAGE_RAM_CACHE: unset or empty is on, 0/false/no/off is off.
 * Turning it off after use is ignored. */
void xbox_RamCacheEnable(int on);
/* The cap in bytes; 0 is the stock 750 MB. Before first use (tests). */
void xbox_RamCacheSetCap(uint64_t bytes);
/* Bytes in use against the cap, in 16 KB clusters (tests, logs). */
uint64_t xbox_RamCacheUsed(void);
/* Tests: 1 when the store path (`s0.rec`, `sub\\t.rec`) is a seed not yet
 * opened, 0 for any other file or directory, -1 when there is no such name;
 * *order (if given) is its creation order, the seed's host listing order.
 * Unlike a query or a listing, it does not mark the seed seen. */
int xbox_RamCacheSeedState(const char *path, uint32_t *order);

int xbox_ramfs_active(void);

int xbox_ramfs_create(PHANDLE out, ACCESS_MASK access, PXBOX_OBJECT_ATTRIBUTES oa,
                      PXBOX_IO_STATUS_BLOCK ios, ULONG disposition, ULONG options,
                      NTSTATUS *st);
int xbox_ramfs_read(HANDLE h, HANDLE event, PXBOX_IO_STATUS_BLOCK ios,
                    PVOID buf, ULONG len, PLARGE_INTEGER off, NTSTATUS *st);
int xbox_ramfs_write(HANDLE h, HANDLE event, PXBOX_IO_STATUS_BLOCK ios,
                     PVOID buf, ULONG len, PLARGE_INTEGER off, NTSTATUS *st);
int xbox_ramfs_close(HANDLE h);
int xbox_ramfs_delete(PXBOX_OBJECT_ATTRIBUTES oa, NTSTATUS *st);
int xbox_ramfs_query_info(HANDLE h, PXBOX_IO_STATUS_BLOCK ios, PVOID info,
                          ULONG len, XBOX_FILE_INFORMATION_CLASS cls, NTSTATUS *st);
int xbox_ramfs_set_info(HANDLE h, PXBOX_IO_STATUS_BLOCK ios, PVOID info,
                        ULONG len, XBOX_FILE_INFORMATION_CLASS cls, NTSTATUS *st);
int xbox_ramfs_query_volume(HANDLE h, PXBOX_IO_STATUS_BLOCK ios, PVOID info,
                            ULONG len, XBOX_FS_INFORMATION_CLASS cls, NTSTATUS *st);
int xbox_ramfs_flush(HANDLE h, PXBOX_IO_STATUS_BLOCK ios, NTSTATUS *st);
int xbox_ramfs_query_full(PXBOX_OBJECT_ATTRIBUTES oa,
                          PXBOX_FILE_NETWORK_OPEN_INFORMATION info, NTSTATUS *st);
int xbox_ramfs_query_dir(HANDLE h, PXBOX_IO_STATUS_BLOCK ios, PVOID info, ULONG len,
                         XBOX_FILE_INFORMATION_CLASS cls, PXBOX_ANSI_STRING name,
                         BOOLEAN restart, NTSTATUS *st);
/* Either side may be the store's; a rename between it and disk is refused. */
int xbox_ramfs_rename(HANDLE h, PXBOX_OBJECT_ATTRIBUTES new_name,
                      BOOLEAN replace, NTSTATUS *st);
/* NtDuplicateObject: a new handle on the same file object (shared position).
 * Returns 1 for a store handle, with *dup NULL when it failed. */
int xbox_ramfs_duplicate(HANDLE src, HANDLE *dup, int close_source);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_RAMFS_H */
