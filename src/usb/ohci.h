/*
 * ohci.h -- the Xbox USB host controllers, enough of them to be found.
 *
 * A title reaches its gamepad through XAPI, which is statically linked into
 * the image and drives the OHCI controller registers directly rather than
 * going through anything this runtime can shim. Half-Life 2 registers a 4 KB
 * block at 0xFED00000 and takes an interrupt vector for it, then reads
 * HcRevision out of an aperture backed by zeroed RAM, concludes there is a
 * host controller with no root hub ports, and enumerates nothing.
 *
 * This is the register half of the answer: a controller that reports a real
 * revision, a root hub with ports, and the handful of registers that have
 * behaviour rather than storage. It does not yet walk the endpoint and
 * transfer descriptor lists, which is what actually moves a report, so a
 * driver that gets this far will find a port and then have nothing to talk to.
 * That is deliberate -- the register trace is what says whether the driver
 * polls HcInterruptStatus or waits on the IRQ, and that answers how much of
 * the rest is needed before any of it is written.
 *
 * Written from the OHCI 1.0a specification and this title's own code.
 */
#ifndef XBOX_OHCI_H
#define XBOX_OHCI_H

#include <stdint.h>

/* The two MCPX host controllers, as the XDK addresses them. */
#define XBOX_OHCI0_BASE   0xFED00000u
#define XBOX_OHCI1_BASE   0xFED08000u
#define XBOX_OHCI_SIZE    0x00001000u   /* 4 KB, the length XAPI registers */

/* Bring the models up. Safe to call more than once; does nothing unless
 * RECOMP_USB is set, so a title that was working without a controller keeps
 * behaving exactly as it did. Call it after the guest memory is mapped.
 *
 * The registers are trapped on Windows x86-64 and on POSIX arm64 hosts. On
 * any other host the model stays off, with a one-line notice, and the title
 * sees the aperture as plain memory, as it does with RECOMP_USB unset. */
void xbox_OhciInit(void);

/* 1 if the address is inside a controller this model owns. */
int  xbox_OhciOwnsAddress(uint32_t xbox_va);

/* Service a trapped access. Returns 1 if the faulting instruction was decoded
 * and stepped over. `ctx` is the fault's thread state: the PCONTEXT from a
 * Win32 VEH, or the ucontext_t * a POSIX SA_SIGINFO handler receives (its
 * third argument). void * keeps windows.h out of here. A title's fault
 * handler calls this (or xbox_VehMmioFault / xbox_PosixMmioFault, which
 * route to it) before it treats the fault as a crash. */
int  xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);

/* How the model resolves a physical address the title handed it: a
 * descriptor word (data = 0) or a transfer buffer of the TD at `td`
 * (data = 1). Returns the guest address it reads. For the address
 * resolution ctest; the model's own path is the same code. */
uint32_t xbox_OhciResolveForTest(int data, uint32_t td, uint32_t addr);

/* One root-hub pass of the pad-source hot-plug (usb_gamepad.h): plug the
 * pads usb_gamepad_present() names, unplug the ones it lost. `reset` first
 * empties the bus. Writes the four port status registers to `ports` and
 * returns the plugged mask. For the pad-source ctest; needs no title. */
uint32_t xbox_OhciHotplugForTest(int reset, uint32_t ports[4]);

/* A dword register write as the guest makes it, on the controller that
 * carries the pads; returns the register afterwards. For the pad-source
 * ctest, to enable a port or acknowledge a change the way a driver does. */
uint32_t xbox_OhciWriteForTest(uint32_t off, uint32_t v);

/* The condition code a transfer to function address `addr` completes with
 * when no plugged pad holds that address, or 0xFFFFFFFF when one does. */
uint32_t xbox_OhciNoDeviceForTest(uint32_t addr);

/* Report counts at exit, so a run says whether the driver ever looked. */
void xbox_OhciReport(void);

#endif /* XBOX_OHCI_H */
