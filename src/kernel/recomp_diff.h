/**
 * Register checkpoint ring for differential debugging.
 *
 * Generated or hand-written code calls recomp_diff_checkpoint() at points it
 * wants compared against another implementation (an emulator, a native
 * build); recomp_diff_checkpoint_dump() writes the most recent records as the
 * raw little-endian layout tools/diff/checkpoint_decode.py reads: ten u32 per
 * record, sequence eip eax ebx ecx edx esi edi esp eflags, oldest first.
 */
#ifndef RECOMP_DIFF_H
#define RECOMP_DIFF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECOMP_DIFF_RING 4096u  /* power of two; older records are overwritten */

void recomp_diff_checkpoint(uint32_t eip, uint32_t eax, uint32_t ebx,
                            uint32_t ecx, uint32_t edx, uint32_t esi,
                            uint32_t edi, uint32_t esp, uint32_t eflags);

/* Writes the retained records to `path`. Returns the record count, or -1. */
int recomp_diff_checkpoint_dump(const char *path);

/* Forgets all records and restarts the sequence at 1. */
void recomp_diff_checkpoint_reset(void);

/* Trace events: same ring idea, 28-byte records (sequence thread eip type
 * arg0 arg1 arg2, seven u32) as tools/diff/trace_decode.py reads. `type` is
 * the caller's; READ and WRITE events (arg0 = guest address) can be limited
 * to one address window with recomp_diff_trace_set_memory_filter. */
#define RECOMP_DIFF_TRACE_READ  5u
#define RECOMP_DIFF_TRACE_WRITE 6u

void recomp_diff_trace_event(uint32_t type, uint32_t eip,
                             uint32_t arg0, uint32_t arg1, uint32_t arg2);
/* begin >= end disables the filter. */
void recomp_diff_trace_set_memory_filter(uint32_t begin, uint32_t end);
int recomp_diff_trace_dump(const char *path);
void recomp_diff_trace_reset(void);

#ifdef __cplusplus
}
#endif

#endif
