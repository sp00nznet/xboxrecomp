/*
 * guest_state_hash.h - a checksum of guest RAM at every port-0 pad poll.
 *
 * To find where two replays of the same input part ways, each run writes
 * one hash of the whole of guest RAM per poll, at the same guest point:
 * the end of the title's pad poll, which a replay's input is timed by (the
 * g<poll> clock). The first poll whose hashes differ is the first frame the
 * runs disagree on; a page file and a full dump at a chosen poll then say
 * which memory, and from there which code owns it.
 *
 *   RECOMP_DEBUG=state_hash=<path>      <path>: one line per poll,
 *                                       "poll vblank vtime hash"; <path>.pages:
 *                                       the 64 KB pages whose hash changed
 *   RECOMP_DEBUG=state_hash_skip=a-b,.. guest ranges left out (hex, end
 *                                       exclusive), e.g. a ring the host writes
 *   RECOMP_DEBUG=state_dump=<poll>[,..] all of guest RAM at that poll, to
 *                                       <path>.<poll>.ram
 *
 * Off unless state_hash is set. Hashing all of guest RAM takes a few milliseconds on
 * the polling thread, which changes the run's timing: compare runs that
 * both had it on.
 */
#ifndef XBOX_GUEST_STATE_HASH_H
#define XBOX_GUEST_STATE_HASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read the keys and, when state_hash is set, hook the pad poll. */
void xbox_GuestStateHashInit(void);

/* The hash of [0, len) of a buffer as this file computes it; for tests. */
uint64_t xbox_GuestStateHashBytes(const void *p, size_t len);

/* The two list keys, parsed as Init does; each returns how many items it
 * kept and warns once ([HASH]) for each item it rejects: a skip that is not
 * hex lo-hi with lo < hi (hi is clamped to 0xFFFFFFFF), a dump that is not
 * a decimal poll, anything past 16. Exposed for tests. */
int      xbox_GuestStateHashSetSkips(const char *list);
int      xbox_GuestStateHashSetDumps(const char *list);

/* The record's hash of one 64 KB page read from p, as the guest page at va
 * (rounded down to the page), with the skipped ranges read as zero. */
uint64_t xbox_GuestStateHashPage(const void *p, uint32_t va);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_GUEST_STATE_HASH_H */
