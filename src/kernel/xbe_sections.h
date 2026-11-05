/*
 * XBE section paging: XeLoadSection / XeUnloadSection with the console's
 * semantics, for titles whose non-preload sections do not fit in RAM.
 *
 * On the console only the sections flagged PRELOAD are resident at boot. The
 * others (level geometry, per-stage models, overlays) own their virtual range
 * but no pages until the title calls XeLoadSection, which commits the pages,
 * reads the section from disc and bumps its reference count; XeUnloadSection
 * at refcount 1 gives the pages back. A title can therefore have more section
 * bytes than RAM. Copying every section at boot, as the loader does by
 * default, puts them all in the flat guest map and leaves the heap what is
 * left of 64 MB -- 12 MB for BLiNX 1, whose image is 50 MB.
 *
 * With paging on, the loader copies only the preload sections, registers
 * every section's initialised bytes here and leaves the rest zeroed;
 * xbe_section_load copies a section in fresh on 0 -> 1 (fresh because titles
 * fix up pointers in place after a load), xbe_section_unload drops the
 * charge on 1 -> 0 and leaves the bytes. Off (the default), load and unload
 * only count, as before.
 *
 * The module touches guest memory only through the base pointer it is given,
 * so it runs without the runtime (tests/xbe_sections).
 */
#ifndef XBE_SECTIONS_H
#define XBE_SECTIONS_H

#include <stddef.h>
#include <stdint.h>

/* XBE_SECTION_HEADER fields the kernel updates (kernel.h has the struct). */
#define XBE_SECTHDR_FLAGS          0x00
#define XBE_SECTHDR_VA             0x04
#define XBE_SECTHDR_VSIZE          0x08
#define XBE_SECTHDR_REFCOUNT       0x18
#define XBE_SECTHDR_HEAD_SHARED    0x1C /* -> WORD shared-page count */
#define XBE_SECTHDR_TAIL_SHARED    0x20
#define XBE_SECTION_FLAG_PRELOAD   0x00000002u

#define XBE_STATUS_SUCCESS            0x00000000u
#define XBE_STATUS_INVALID_PARAMETER  0xC000000Du
#define XBE_STATUS_NO_MEMORY          0xC0000017u

/* Paging is a title's choice (xbox_SetSectionPaging), overridden either way
 * by the RECOMP_XBE_PAGING config key; it must be set before
 * xbox_MemoryLayoutInit. */
void xbe_sections_set_paging(int on);
int  xbe_sections_paging(void);

/* Forget every section and free the copies. */
void xbe_sections_reset(void);

/* Register one section: its header VA, flags, range and initialised bytes
 * (copied; may be NULL when raw_size is 0). Returns 0, or -1 when the table
 * is full or the copy cannot be allocated. */
int xbe_sections_add(uint32_t hdr_va, uint32_t flags, uint32_t va,
                     uint32_t vsize, const void *raw, uint32_t raw_size);

/* XeLoadSection / XeUnloadSection on the header at hdr_va. mem is the guest
 * base (VA 0) and mem_size the mapped bytes. *count_out gets the new
 * reference count. A header that was never registered, or any call while
 * paging is off, only counts (the pre-paging behaviour). */
uint32_t xbe_section_load(uint8_t *mem, size_t mem_size, uint32_t hdr_va,
                          uint32_t *count_out);
uint32_t xbe_section_unload(uint8_t *mem, size_t mem_size, uint32_t hdr_va,
                            uint32_t *count_out);

/* Load every registered PRELOAD section once, as the console's loader does
 * at boot: refcount 1 and its shared pages counted. */
void xbe_sections_load_preload(uint8_t *mem, size_t mem_size);

/* Page-rounded bytes of the sections now resident, and how many sections
 * are registered / resident. A report of the 64 MB budget, not a limit. */
uint64_t xbe_sections_resident_bytes(void);
unsigned xbe_sections_count(void);
unsigned xbe_sections_resident(void);

#endif
