#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef void (*guest_fn)(void);
extern guest_fn recomp_lookup_kernel(uint32_t);
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_esp;
extern ptrdiff_t g_xbox_mem_offset;
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }
static uint8_t *memory;
static int call_irql(unsigned ordinal, uint32_t ecx, unsigned expected_old, unsigned expected_level) {
    uint32_t *stack=(uint32_t *)(memory+0x20000);
    stack[0]=0x12345678; stack[1]=0xA5A5A507;
    g_ecx=ecx; g_esp=0x20000;
    uint32_t token=*(uint32_t *)(memory+0x10000+(ordinal==160?0:4));
    guest_fn fn=recomp_lookup_kernel(token);
    if(!fn) return 1;
    fn();
    /* The host raise returns the prior level; raising to the expected level is a no-op after a correct bridge call. */
    unsigned level=xbox_KfRaiseIrql((KIRQL)expected_level);
    if(g_esp!=0x20004 || stack[1]!=0xA5A5A507 || level!=expected_level ||
       (ordinal==160 && g_eax!=expected_old)) {
        fprintf(stderr,"FAIL ordinal=%u ECX=%08X IRQL=%u expected=%u old=%u expected_old=%u ESP=%08X\n",
                ordinal,ecx,level,expected_level,g_eax,expected_old,g_esp);
        return 1;
    }
    return 0;
}
/* The level is per thread: another thread reads PASSIVE_LEVEL while this one
 * is raised. Under the MinGW build XBOX_THREAD_LOCAL used to be ignored, and
 * the level was one process-wide variable. */
static DWORD WINAPI other_thread(LPVOID out) {
    *(unsigned *)out = xbox_KfRaiseIrql(0);
    return 0;
}
static int check(const char *name, int cond) {
    if (!cond) fprintf(stderr, "FAIL %s (depth %d)\n", name, xbox_IrqlRaisedCount());
    return !cond;
}
/* A lower that raises (KfLowerIrql to a level above the current one) sets
 * the level but never counts as a raise: counted, it had no lower to undo it
 * and the depth every device model reads stayed up for the rest of the run. */
static int lower_raises_case(void) {
    int failed = 0;
    unsigned seen = 99;
    HANDLE t;
    failed |= check("starts passive and uncounted", xbox_KfRaiseIrql(0) == 0 && xbox_IrqlRaisedCount() == 0);
    xbox_KfLowerIrql(16);
    /* Read the level where the guest does, KPCR.Irql at fs:[0x24]: asking
     * through KfRaiseIrql would itself count as a raise. */
    failed |= check("lower-to-16 sets the level", memory[XBOX_TIB_MAIN + 0x24] == 16);
    failed |= check("lower-to-16 is not counted", xbox_IrqlRaisedCount() == 0);
    xbox_KfLowerIrql(0);
    failed |= check("back to passive, still uncounted", xbox_IrqlRaisedCount() == 0);
    (void)xbox_KfRaiseIrql(2);
    failed |= check("a real raise counts once", xbox_IrqlRaisedCount() == 1);
    xbox_KfLowerIrql(16);
    failed |= check("lower-raises while counted stays at one", xbox_IrqlRaisedCount() == 1);
    t = CreateThread(NULL, 0, other_thread, &seen, 0, NULL);
    if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
    failed |= check("another thread is at passive", seen == 0);
    xbox_KfLowerIrql(0);
    failed |= check("the real lower uncounts", xbox_IrqlRaisedCount() == 0 && !xbox_IrqlBlocksInterrupts());
    return failed;
}
int main(void) {
    memory=VirtualAlloc(NULL,16*1024*1024,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!memory) return 10;
    g_xbox_mem_offset=(ptrdiff_t)memory;
    uint32_t *imports=(uint32_t *)(memory+0x10000);
    imports[0]=0x800000A0; imports[1]=0x800000A1;
    xbox_kernel_set_thunk_address(0x10000,2); xbox_kernel_bridge_init();
    int failed=0;
    failed|=call_irql(160,0xBEEF0002,0,2);
    failed|=call_irql(160,0x12340003,2,3);
    failed|=call_irql(161,0xFACE0002,0,2);
    failed|=call_irql(161,0xABCD0000,0,0);
    failed|=lower_raises_case();
    VirtualFree(memory,0,MEM_RELEASE);
    if(!failed) puts("PASS fastcall IRQL: CL arguments, old levels, nesting, stack canary, lower-raises not counted, per-thread level");
    return failed;
}
