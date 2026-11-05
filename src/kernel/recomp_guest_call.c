/*
 * recomp_guest_call.c - a guest function called from runtime code, and
 * guest memory for it (recomp_guest_call.h).
 */
#include "recomp_guest_call.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "recomp_regs.h"
#include "xbox_memory_layout.h"

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
int xbox_DpcIsGuestThread(void);    /* kernel.h */

int recomp_guest_call(const char *who, uint32_t va, uint32_t eax, uint32_t ecx, uint32_t edx,
                      const uint32_t *stack, uint32_t n_stack, uint32_t *eax_out)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    recomp_func_t fn;
    RecompRegs saved;
    uint32_t top, args;

    if (!who)
        who = "[GUEST]";
    /* The mod layer's callback flag says a hook or frame is running; this
     * says the thread has guest registers at all. A host thread (menu,
     * render, input) that reached a callback some other way has none, and
     * its g_esp is whatever its TLS started as. */
    if (!xbox_DpcIsGuestThread()) {
        fprintf(stderr, "%s guest_call 0x%08X: refused, not a guest thread\n", who,
                (unsigned)va);
        return 0;
    }
    if (n_stack > RECOMP_GUEST_CALL_MAX_STACK || (n_stack && !stack)) {
        fprintf(stderr, "%s guest_call 0x%08X: %u stack arguments (at most %u)\n", who,
                (unsigned)va, (unsigned)n_stack, RECOMP_GUEST_CALL_MAX_STACK);
        return 0;
    }
    fn = recomp_lookup(va);
    if (!fn) {
        fprintf(stderr, "%s guest_call 0x%08X: no lifted function there\n", who, (unsigned)va);
        return 0;
    }
    /* Room for the gap, the arguments, the return address and the
     * alignment, all above the guest's first page. */
    if (g_esp < 0x1000u + RECOMP_GUEST_CALL_GAP + 4u * (RECOMP_GUEST_CALL_MAX_STACK + 1u) + 16u) {
        fprintf(stderr, "%s guest_call 0x%08X: esp 0x%08X is no guest stack\n", who,
                (unsigned)va, (unsigned)g_esp);
        return 0;
    }

    recomp_regs_save(&saved);
    /* The first argument on a 16-byte boundary, as a call from aligned
     * code puts it: SSE spills the callee makes relative to esp stay
     * aligned. Pops in the callee (stdcall's `ret n`) do not matter: esp
     * comes back from the save. */
    top = (g_esp - RECOMP_GUEST_CALL_GAP) & ~15u;
    args = (top - 4u * n_stack) & ~15u;
    if (n_stack)
        memcpy(mem + args, stack, 4u * n_stack);
    g_esp = args - 4u;
    {
        const uint32_t ra = RECOMP_GUEST_CALL_RA;
        memcpy(mem + g_esp, &ra, 4);
    }
    g_eax = eax;
    g_ecx = ecx;
    g_edx = edx;
    fn();
    if (eax_out)
        *eax_out = g_eax;
    recomp_regs_restore(&saved);
    return 1;
}

uint32_t recomp_guest_alloc(uint32_t size, uint32_t align)
{
    uint32_t va;

    if (!size)
        return 0;
    if (!xbox_DpcIsGuestThread()) {
        fprintf(stderr, "[GUEST] guest_alloc: refused, not a guest thread\n");
        return 0;
    }
    va = xbox_ContiguousAlloc(size, align);
    if (va)
        memset((uint8_t *)xbox_GetMemoryOffset() + va, 0, size);
    return va;
}
