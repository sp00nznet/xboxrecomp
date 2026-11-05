/*
 * recomp_guest_call.h - run a lifted guest function from runtime code in
 * the middle of the guest's own, and give runtime code guest memory the
 * title's heap never hands out. The mod API's guest_call and guest_alloc
 * (mod.h, v6) are these behind a thread check.
 *
 * Kept apart from mod.c, as recomp_hook_run.c is from recomp_hook.c: this
 * is the part that touches the guest registers, so the mod layer's tests
 * link mod.c with two stubs instead of the register set.
 */
#ifndef RECOMP_GUEST_CALL_H
#define RECOMP_GUEST_CALL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stack arguments a call may pass. */
#define RECOMP_GUEST_CALL_MAX_STACK 16u
/* How far below the caller's esp the call's frame starts: the hooked
 * function's own red zone and the bytes its prologue may have reserved
 * without moving esp yet are left alone. */
#define RECOMP_GUEST_CALL_GAP 0x100u
/* The return address the callee sees: no lifted code lives there, so a
 * return-address lookup (HUD wrappers, traces) matches nothing. */
#define RECOMP_GUEST_CALL_RA 0xDEADBEEFu

/* Run the function at va (recomp_lookup) on this thread's guest registers:
 * eax, ecx and edx set, stack[0..n_stack) pushed right to left (stack[0]
 * is the first argument, at [esp + 4] on entry), a return address pushed,
 * the frame 0x100 bytes below the current esp with the first argument on a
 * 16-byte boundary. Every register in recomp_regs.h is saved first and put
 * back after, so the caller's code continues as if no call happened; the
 * callee's eax goes to *eax_out (may be NULL). Returns 1, or 0 with one
 * stderr line (prefix `who`) when this is no guest thread
 * (xbox_DpcMarkGuestThread), va has no lifted function, n_stack is above
 * the maximum or esp is not a usable guest stack. The caller still checks
 * that guest code is at a point where a call may run (mod.c: inside a hook
 * or frame callback). */
int recomp_guest_call(const char *who, uint32_t va, uint32_t eax, uint32_t ecx, uint32_t edx,
                      const uint32_t *stack, uint32_t n_stack, uint32_t *eax_out);

/* A block of size bytes from the kernel's contiguous arena (the one
 * MmAllocateContiguousMemoryEx draws from), zeroed, aligned to align
 * (rounded up to a page). The guest heap never gives it out and nothing
 * frees it. Its guest VA, or 0 (also off a guest thread). */
uint32_t recomp_guest_alloc(uint32_t size, uint32_t align);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_GUEST_CALL_H */
