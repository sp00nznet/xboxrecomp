#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*guest_fn)(void);
extern guest_fn recomp_lookup_kernel(uint32_t);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;
void *recomp_lookup(ULONG a) { (void)a; abort(); }
void *recomp_lookup_manual(ULONG a) { (void)a; abort(); }

/* NtDeviceIoControlFile (ordinal 196) on the synthetic DVD handle, with a
 * 44-byte SCSI_PASS_THROUGH_DIRECT whose CDB opcode is `opcode` and whose
 * DataBuffer starts out filled with 0xCC. */
int main(void)
{
    uint8_t *mem = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint32_t *sp;
    uint32_t i;
    int ok = 1;
    if (!mem) return 10;
    g_xbox_mem_offset = (ptrdiff_t)mem;
    *(uint32_t *)(mem + 0x10000) = 0x800000C4;          /* ordinal 196 */
    xbox_kernel_set_thunk_address(0x10000, 1);
    xbox_kernel_bridge_init();

    memset(mem + 0x30000, 0, 44);
    mem[0x30000 + 2] = 0x02;                            /* ScsiStatus: CHECK CONDITION */
    *(uint32_t *)(mem + 0x30000 + 12) = 64;             /* DataTransferLength */
    *(uint32_t *)(mem + 0x30000 + 20) = 0x31000;        /* DataBuffer */
    mem[0x30000 + 28] = 0xAD;                           /* READ DVD STRUCTURE */
    memset(mem + 0x31000, 0xCC, 64);

    sp = (uint32_t *)(mem + 0x20000);
    memset(sp, 0, 64);
    sp[0] = 0xBEEF0001;
    sp[1] = 0xDECD0001;                                 /* synthetic DVD handle */
    sp[5] = 0x32000;                                    /* IoStatusBlock */
    sp[6] = 0x4D014;                                    /* IOCTL_SCSI_PASS_THROUGH_DIRECT */
    sp[7] = 0x30000;                                    /* input */
    sp[8] = 44;
    g_esp = 0x20000;
    recomp_lookup_kernel(*(uint32_t *)(mem + 0x10000))();

    if (g_eax != 0 || g_esp != 0x2002C) { fprintf(stderr, "FAIL status=%08X esp=%08X\n", g_eax, g_esp); ok = 0; }
    if (mem[0x30000 + 2] != 0) { puts("FAIL ScsiStatus not GOOD"); ok = 0; }
    for (i = 0; i < 64; i++)
        if (mem[0x31000 + i]) { puts("FAIL DataBuffer not zeroed"); ok = 0; break; }

    VirtualFree(mem, 0, MEM_RELEASE);
    if (!ok) return 1;
    puts("PASS: DVD SCSI pass-through returns GOOD with a zeroed DataBuffer");
    return 0;
}
