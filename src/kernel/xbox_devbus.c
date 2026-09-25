/*
 * xbox_devbus.c -- what a device model needs from the kernel, and no more
 *
 * The APU and the OHCI model raise interrupt lines and resolve bus addresses
 * through the contiguous arena. Both answers belong to the kernel, but these
 * few functions are their own library on purpose: taking them from the
 * kernel's own objects pulls in the bridge, and with it recomp_lookup, which
 * only a title provides. A device model is linked alone -- tests/apu_mixdown
 * does exactly that -- so it depends on xbox_devbus, and xbox_kernel feeds it.
 */

#include "kernel.h"

/* ── Contiguous arena, as the bus sees it ─────────────────────
 *
 * xbox_ContiguousAlloc (xbox_memory_layout.c) publishes the physical range it
 * has handed out: [lo, hi), where the arena's VA is XBOX_CONTIG_BASE + phys.
 * Empty until the first allocation, so nothing is physical before then. */
static volatile uint32_t g_contig_phys_lo, g_contig_phys_hi;

void xbox_ContiguousSetPhysicalRange(uint32_t lo, uint32_t hi)
{
    g_contig_phys_lo = lo;
    g_contig_phys_hi = hi;
}

/* Does this physical (bus) address name memory the contiguous arena handed
 * out? If so it is reached at XBOX_CONTIG_BASE + phys; otherwise it is a
 * pass-through VA (see g_contig_start in xbox_memory_layout.c). */
int xbox_ContiguousIsPhysical(uint32_t phys)
{
    return phys >= g_contig_phys_lo && phys < g_contig_phys_hi;
}

/* Device interrupt lines, level-triggered, one bit per vector.
 *
 * A device model (the APU on vector 5) asserts its line; the timer thread
 * wakes at once and calls the connected ISR, and keeps calling it each tick
 * while the line stays up. The ISR acknowledges in the device's own status
 * register, which is what lowers the line.
 *
 * ponytail: ISRs run on the timer thread with no IRQL and no interrupt
 * spinlock, like the vblank in kernel_bridge.c; KeSynchronizeExecution takes no lock
 * either. Add the interrupt object's lock if a driver is seen racing its ISR.
 */
static volatile LONG g_irq_lines;
static HANDLE        g_irq_event;

HANDLE xbox_irq_line_event(void)
{
    if (!g_irq_event) {
        HANDLE e = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (InterlockedCompareExchangePointer(&g_irq_event, e, NULL))
            CloseHandle(e);
    }
    return g_irq_event;
}

void xbox_set_irq_line(uint32_t vector, int level)
{
    if (vector >= 32)
        return;
    if (level) {
        InterlockedOr(&g_irq_lines, (LONG)(1u << vector));
        SetEvent(xbox_irq_line_event());
    } else {
        InterlockedAnd(&g_irq_lines, ~(LONG)(1u << vector));
    }
}

uint32_t xbox_irq_lines(void)
{
    return (uint32_t)g_irq_lines;
}
