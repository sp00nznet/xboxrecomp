/*
 * ohci.c -- OHCI 1.0a host controller registers for the MCPX.
 *
 * See ohci.h for what this is and is not. The short version: enough for a
 * title's own USB driver to find a controller and a populated root hub port,
 * plus a trace of every register access, because what the driver does after
 * that decides how the rest gets built.
 */
#include "ohci.h"
#include "recomp_env.h"
#include "../platform/mmio_decode.h"
#include "../platform/mmio_decode_a64.h"
#include "../kernel/xbox_memory_layout.h"
#include "../kernel/kernel.h"
#include "../kernel/guest_time.h"   /* the virtual clock's ms (deterministic) */
#include "../kernel/kernel_guest_unwind.h"
#include "usb_gamepad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* Where the registers can be trapped. They are answered by faulting on the
 * aperture and decoding the instruction that touched it: the x86-64 decoder
 * under a Win32 VEH or an x86-64 Linux SIGSEGV handler, or the A64 decoder
 * under a POSIX SIGBUS/SIGSEGV handler. A host with neither leaves the model
 * off (see xbox_OhciInit). */
#if defined(MMIO_DECODE_X64)
#  define OHCI_TRAP_X64 1
#elif defined(__aarch64__)
#  define OHCI_TRAP_POSIX_A64 1
#endif

/* The runtime maps guest memory at a fixed host offset. */
extern ptrdiff_t xbox_GetMemoryOffset(void);

/* Calling the title's interrupt service routine.
 *
 * A recompiled function reads its arguments off the guest stack and keeps its
 * registers in thread-local storage, so it can only be called from a thread
 * that has both. xbox_worker_stack_alloc hands out a guest stack slice for
 * exactly this -- a host thread calling recompiled code -- and recomp_lookup
 * turns a guest address into something callable.
 */
typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup(uint32_t xbox_va);
extern int  xbox_worker_stack_alloc(void);
extern void xbox_worker_stack_free(int slot);
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);

/* Declared in the generated runtime; thread-local, so the values below are
 * this thread's and not the guest thread's. */
#if defined(_MSC_VER)
#  define OHCI_TLS __declspec(thread)
#else
#  define OHCI_TLS __thread
#endif
extern OHCI_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern OHCI_TLS uint32_t g_ebx, g_esi, g_edi;
extern OHCI_TLS uint32_t g_fs_base;
extern uint32_t xbox_AllocThreadTib(void);

/* The vector XPP takes for USB0. HalGetInterruptVector(1) returns 1 here, and
 * the bus interrupt level is what the XDK passes. */
#define OHCI_VECTOR  1

/* ---- OHCI 1.0a operational registers, by byte offset ------------------- */
#define HcRevision              0x00
#define HcControl               0x04
#define HcCommandStatus         0x08
#define HcInterruptStatus       0x0C
#define HcInterruptEnable       0x10
#define HcInterruptDisable      0x14
#define HcHCCA                  0x18
#define HcPeriodCurrentED       0x1C
#define HcControlHeadED         0x20
#define HcControlCurrentED      0x24
#define HcBulkHeadED            0x28
#define HcBulkCurrentED         0x2C
#define HcDoneHead              0x30
#define HcFmInterval            0x34
#define HcFmRemaining           0x38
#define HcFmNumber              0x3C
#define HcPeriodicStart         0x40
#define HcLSThreshold           0x44
#define HcRhDescriptorA         0x48
#define HcRhDescriptorB         0x4C
#define HcRhStatus              0x50
#define HcRhPortStatus1         0x54
/* Through port 4. The root hub here reports two downstream ports, and the
 * driver reads four status registers regardless -- so the file has to cover
 * them or the last one falls outside the array and is answered by the
 * not-a-register path instead of by an empty port. Same value either way,
 * reached honestly. */
#define OHCI_REG_MAX            0x64

/* HcCommandStatus */
#define CS_HCR                  0x00000001u   /* host controller reset       */

/* HcInterruptStatus / Enable */
#define INTR_SO                 0x00000001u   /* scheduling overrun          */
#define INTR_WDH                0x00000002u   /* writeback done head         */
#define INTR_SF                 0x00000004u   /* start of frame              */
#define INTR_RD                 0x00000008u   /* resume detected             */
#define INTR_UE                 0x00000010u   /* unrecoverable error         */
#define INTR_FNO                0x00000020u   /* frame number overflow       */
#define INTR_RHSC               0x00000040u   /* root hub status change      */

/* HccaFrameNumber is at +0x80 and the done head at +0x84. Declared up here
 * because the register write handler retires the done list on a WDH ack. */
#define HCCA_DONE_HEAD 0x84
static void wr32(uint32_t va, uint32_t v);
#define INTR_MIE                0x80000000u   /* master interrupt enable     */

/* HcRhPortStatus */
#define PORT_CCS                0x00000001u   /* current connect status      */
#define PORT_PES                0x00000002u   /* port enable status          */
#define PORT_PSS                0x00000004u   /* port suspend status         */
#define PORT_PPS                0x00000100u   /* port power status           */
#define PORT_LSDA               0x00000200u   /* low speed device attached   */
#define PORT_CSC                0x00010000u   /* connect status change       */
#define PORT_PESC               0x00020000u   /* enable status change        */
#define PORT_PRSC               0x00100000u   /* reset status change         */

/* Writes to HcRhPortStatus set/clear by bit position rather than by value. */
#define PORT_W_CCS_CLEAR_ENABLE 0x00000001u   /* ClearPortEnable             */
#define PORT_W_PES_SET_ENABLE   0x00000002u   /* SetPortEnable               */
#define PORT_W_PRS_SET_RESET    0x00000010u   /* SetPortReset                */
#define PORT_W_PPS_SET_POWER    0x00000100u   /* SetPortPower                */
#define PORT_W_CLEAR_POWER      0x00000200u   /* ClearPortPower              */

/* The root hub the pads are plugged into. With four ports XAPI numbers the
 * controller slots from them as root port 3 = controller port 1 (player 1),
 * 4 = 2, 1 = 3, 2 = 4 -- measured: SSX 3's debug quick-start menu reads
 * controller slot 0 only, and saw the pad only on root port 3. */
#define OHCI_PORTS              4

/* Passes this thread will keep an interrupt back while the guest is at
 * DISPATCH_LEVEL or above. One pass is the OHCI_TICK_MS (4 ms) loop tick, so
 * this is two seconds. Under RECOMP_DEBUG=deterministic a pass is one vblank
 * step instead, so the same count is about 8.3 s of virtual time: still a
 * bound, and a slow section is still not mistaken for a stuck one.
 *
 * It was 80 ms, on the reasoning that no real critical section lasts longer.
 * True of the hardware and false here: recompiled code under an emulated
 * kernel is slower than the console by a wide and variable margin, and the
 * section XAPI holds while it opens the gamepad's interrupt pipe overran it.
 * The interrupt then landed in the middle of that setup and the title faulted
 * on a half-built structure. Two seconds is still not forever -- a guest that
 * genuinely never lowers is still not allowed to switch the device off -- but
 * it is long enough that a slow critical section is not mistaken for one. */
#define OHCI_IRQ_HOLDOFF        500

/* Milliseconds per pass of the controller thread, which is also how many USB
 * frames a pass is worth.
 *
 * It was 20. A control transfer needs a pass per stage and enumeration needs
 * several transfers, so twenty milliseconds put the whole sequence in the
 * hundreds of milliseconds and left it racing the driver's own timeouts: the
 * same build would enumerate fully on one run and stop half way on the next,
 * with nothing to tell the two apart but how many register reads happened.
 * Four is close enough to a real frame to stop losing that race and still
 * cheap -- this thread does nothing but read a few guest dwords per pass. */
#define OHCI_TICK_MS            4u

typedef struct {
    uint32_t base;                      /* Xbox VA of the register block   */
    uint32_t reg[OHCI_REG_MAX / 4];
    unsigned reads, writes, decode_fail;
    int      index;
    int      periodic_seen;
    int      ple_seen;
    /* Bumped every time the driver writes HcInterruptStatus. The interrupt
     * thread watches it to tell a source nobody is servicing from one that
     * is simply busy -- see the delivery loop. */
    volatile unsigned ack_seq;
} OhciController;

static OhciController s_hc[2];
/* Which controller carries the device, and so which one this thread services.
 * XAPI numbers its four gamepad slots across both controllers, and which end
 * it starts from decides whether a pad shows up as player 1 or player 3 --
 * a mapping worth finding by measurement, not by assertion. */
static int s_device_hc;
/* Downstream ports the root hub reports in HcRhDescriptorA (RECOMP_USB_NDP,
 * 1-4). Which slot XAPI gives a pad is decided by its port: see OHCI_PORTS. */
static unsigned s_ndp = OHCI_PORTS;
static int s_npads = 1;          /* RECOMP_USB_PADS */
static int s_npads_set;          /* ... and whether it was given */
static int s_enabled;
static int s_trace;

static uint32_t *reg_of(OhciController *hc, uint32_t off)
{
    return (off < OHCI_REG_MAX) ? &hc->reg[off / 4] : NULL;
}

/* Status bits change on two threads: the controller thread latches them
 * (a done queue, a frame, a pad arriving or leaving) while the guest's own
 * MMIO writes clear them, write-1-to-clear. A plain |= or &= on either side
 * is a read, a gap and a write, and a bit the other thread set in that gap
 * is lost: a dropped RHSC is a pad that arrives or leaves and nobody hears
 * of it. Every update of HcInterruptStatus and the port-status words goes
 * through these. */
static void reg_set(uint32_t *r, uint32_t bits)
{
    __atomic_fetch_or(r, bits, __ATOMIC_SEQ_CST);
}

static void reg_clear(uint32_t *r, uint32_t bits)
{
    __atomic_fetch_and(r, ~bits, __ATOMIC_SEQ_CST);
}

/* ---- register semantics ------------------------------------------------ */

static uint64_t ohci_read(void *dev, uint32_t off, int size)
{
    OhciController *hc = (OhciController *)dev;
    uint32_t aligned = off & ~3u;
    uint32_t *r = reg_of(hc, aligned);
    uint32_t v = r ? *r : 0;

    hc->reads++;

    /* HcFmRemaining and HcFmNumber advance on their own. A driver that waits
     * for the frame counter to move is waiting for the controller to be
     * running, and a counter that never changes is a controller that is not.
     * Derived from the read count rather than a timer: it only has to be
     * monotonic, and a timer here would need a thread to be worth having. */
    if (aligned == HcFmNumber)
        v = (hc->reads >> 3) & 0xFFFFu;
    else if (aligned == HcFmRemaining)
        v = (hc->reads * 977u) & 0x3FFFu;

    /* Sub-dword reads take their slice of the containing register. */
    if (size < 4) {
        unsigned shift = (off & 3u) * 8u;
        v >>= shift;
        if (size == 1) v &= 0xFFu;
        else if (size == 2) v &= 0xFFFFu;
    }

    if (s_trace) {
        static unsigned n;
        if (n++ < 600)
            fprintf(stderr, "  [OHCI%d] read  +0x%02X = %08X\n",
                    hc->index, off, (uint32_t)v);
    }
    return v;
}

static void ohci_write(void *dev, uint32_t off, uint64_t val, int size)
{
    OhciController *hc = (OhciController *)dev;
    uint32_t aligned = off & ~3u;
    uint32_t *r = reg_of(hc, aligned);
    uint32_t v = (uint32_t)val;

    hc->writes++;
    if (!r)
        return;

    if (s_trace) {
        static unsigned n;
        if (n++ < 600)
            fprintf(stderr, "  [OHCI%d] write +0x%02X = %08X\n",
                    hc->index, off, v);
    }

    switch (aligned) {
    case HcRevision:                    /* read-only */
        return;

    case HcCommandStatus:
        /* HCR is self-clearing: the controller resets and drops the bit, and
         * a driver polls for exactly that. Leaving it set is a hang, and it
         * is the first thing a driver does, so it would be the only thing
         * anyone ever saw of this file. */
        *r |= v;
        if (*r & CS_HCR) {
            *r &= ~CS_HCR;
            hc->reg[HcControl / 4] &= ~0xC0u;    /* back to UsbReset state  */
            hc->reg[HcInterruptStatus / 4] = 0;
            hc->reg[HcInterruptEnable / 4] = 0;
        }
        return;

    case HcInterruptStatus:
        /* Acknowledging WritebackDoneHead retires the done list.
         *
         * The controller publishes the queue in HccaDoneHead and raises WDH;
         * the driver reads the list and clears WDH to say it is finished, and
         * the head must then read as zero. Leaving the old value there is a
         * stale pointer to descriptors that have already been consumed, and a
         * driver that checks the head before trusting the interrupt walks
         * them a second time. That is a plausible way to enumerate a device
         * perfectly and then stop, which is what happened here. */
        if ((v & INTR_WDH) && (*r & INTR_WDH)) {
            uint32_t hcca = hc->reg[HcHCCA / 4];
            if (hcca)
                wr32(hcca + HCCA_DONE_HEAD, 0);
            hc->reg[HcDoneHead / 4] = 0;
        }
        reg_clear(r, v);                /* write 1 to clear                 */
        hc->ack_seq++;
        return;

    case HcInterruptEnable:
        hc->reg[HcInterruptEnable / 4] |= v;
        return;

    case HcInterruptDisable:
        hc->reg[HcInterruptEnable / 4] &= ~v;
        return;

    case HcFmNumber:
    case HcFmRemaining:
        return;                         /* driven by the controller         */

    case HcRhDescriptorA:
        /* NumberDownstreamPorts is ours; the driver may set the power and
         * over-current policy bits above it. */
        *r = (*r & 0x000000FFu) | (v & ~0x000000FFu);
        return;

    case HcRhPortStatus1:
    case HcRhPortStatus1 + 4:
    case HcRhPortStatus1 + 8:
    case HcRhPortStatus1 + 12: {
        /* Writes here are set/clear requests by bit position, not a value to
         * store. Getting that wrong looks like a port that will not enable.
         *
         * All four, not the two this root hub usually advertises. The read
         * path already answers four, because a driver reads every port
         * register whatever the descriptor says; the write path did not, so a
         * device placed on port 3 was seen, reset, and never reset-completed
         * -- the request fell through to the plain store and PRSC was never
         * raised. The driver reset it forever. A hub that reports N ports has
         * to give all N the same semantics. */
        unsigned port = (aligned - HcRhPortStatus1) / 4;
        uint32_t *ps = &hc->reg[(HcRhPortStatus1 + port * 4) / 4];

        /* Write-1-to-clear first, because the change bits a write carries
         * refer to the state BEFORE it.
         *
         * This ordering is the whole bug it once had. A driver starting a
         * fresh reset writes SetPortReset and ClearPortResetStatusChange in
         * the same word -- "acknowledge the last reset, begin another" --
         * which is both legal and what the Xbox USB stack does: DDS9 writes
         * 0x01100010. Clearing afterwards wiped the PRSC this very write had
         * just raised, so the reset never reported complete, the driver
         * reset the port again, and enumeration looped forever one step from
         * finishing. The port looked healthy the whole time: connected,
         * enabled, powered. */
        reg_clear(ps, v & 0xFFFF0000u);

        if (v & PORT_W_CCS_CLEAR_ENABLE) reg_clear(ps, PORT_PES);
        if (v & PORT_W_PES_SET_ENABLE)   reg_set(ps, (*ps & PORT_CCS) ? PORT_PES : 0);
        if (v & PORT_W_PPS_SET_POWER)    reg_set(ps, PORT_PPS);
        if (v & PORT_W_CLEAR_POWER)      reg_clear(ps, PORT_PPS);
        if (v & PORT_W_PRS_SET_RESET) {
            /* Reset completes immediately -- there is no wire to settle -- so
             * PRS is never observed set. What matters is what the driver
             * checks afterwards: a present device comes back enabled, and the
             * reset-change bit says the reset finished.
             *
             * Only the status bit is set here. Delivering the interrupt is the
             * controller thread's job, because this runs on the guest's own
             * thread inside a fault handler, and pointing g_esp at a worker
             * stack from here would overwrite the stack pointer of the thread
             * being interrupted. */
            if (*ps & PORT_CCS)
                reg_set(ps, PORT_PES);
            reg_set(ps, PORT_PRSC);
            reg_set(&hc->reg[HcInterruptStatus / 4], INTR_RHSC);
        }
        return;
    }

    default:
        break;
    }

    if (size == 4) {
        *r = v;
    } else {
        unsigned shift = (off & 3u) * 8u;
        uint32_t mask = ((size == 1) ? 0xFFu : 0xFFFFu) << shift;
        *r = (*r & ~mask) | ((v << shift) & mask);
    }
}

/* ---- the control list -------------------------------------------------- */
/*
 * A host controller is a bus master: the driver builds endpoint and transfer
 * descriptors in RAM, points HcControlHeadED at them and sets ControlListFilled,
 * and the controller walks that list itself. Nothing arrives through MMIO, so
 * none of this is visible to the register trace -- which is why the driver
 * looked idle after the port came up.
 *
 * OHCI 1.0a, section 4. An endpoint descriptor is four dwords:
 *
 *   +0  FA | EN<<7 | D<<11 | S<<13 | K<<14 | F<<15 | MPS<<16
 *   +4  TailP        queue tail, 16-byte aligned
 *   +8  HeadP        queue head, with Halted in bit 0 and toggleCarry in bit 1
 *   +C  NextED
 *
 * and a general transfer descriptor is four more:
 *
 *   +0  ... DP<<19 | DI<<21 | T<<24 | EC<<26 | CC<<28
 *   +4  CBP          current buffer pointer, 0 when the transfer moved nothing
 *   +8  NextTD
 *   +C  BE           last byte of the buffer, inclusive
 *
 * A transfer is done when HeadP reaches TailP. Completed descriptors go on the
 * done queue, newest first, and the controller publishes it in the HCCA and
 * raises WritebackDoneHead.
 */
#define ED_SKIP        (1u << 14)
#define ED_HEAD_HALT   (1u << 0)
#define ED_HEAD_TOGGLE (1u << 1)
#define ED_PTR_MASK    0xFFFFFFF0u

#define TD_DP_SETUP    0u
#define TD_DP_OUT      1u
#define TD_DP_IN       2u
#define TD_CC_NOERROR  0u
#define TD_CC_STALL    4u
#define TD_CC_DEVICENOTRESPONDING 5u
#define TD_CC_DATAUNDERRUN 9u
#define TD_ROUNDING    (1u << 18)   /* bufferRounding: a short packet is fine */


/* Per pad: each device on the bus has its own control pipe state. */
static uint32_t g_setup_pending_[USB_GAMEPAD_MAX];
static UsbSetup g_setup_[USB_GAMEPAD_MAX];
#define g_setup_pending g_setup_pending_[pad]
#define g_setup         g_setup_[pad]

/* The control transfer's data stage, across however many descriptors it takes.
 *
 * bMaxPacketSize0 is 8, so a driver asking for a 32-byte configuration
 * descriptor queues four 8-byte IN descriptors and expects successive chunks.
 * Answering each one from the top of the buffer hands it the first 8 bytes
 * four times: the descriptor it assembles is nonsense, it gives up and starts
 * again, which is exactly the loop DDS9 sat in -- GET_DESCRIPTOR, SET_ADDRESS,
 * GET_DESCRIPTOR, forever. The answer is computed once per setup packet and
 * then consumed. */
static uint8_t g_ctrl_buf_[USB_GAMEPAD_MAX][64];
static int     g_ctrl_len_[USB_GAMEPAD_MAX] = {-1, -1, -1, -1};
static int     g_ctrl_sent_[USB_GAMEPAD_MAX];
#define g_ctrl_buf  g_ctrl_buf_[pad]
#define g_ctrl_len  g_ctrl_len_[pad]
#define g_ctrl_sent g_ctrl_sent_[pad]

/* The pads on the bus, bit n for pad n. Without a game's pad source they
 * are pads 0..N-1, plugged in that order and never removed; with one they
 * follow its present mask (ohci_hotplug). */
static volatile uint32_t s_plugged;

/* Which pad an endpoint descriptor talks to: by its function address, and
 * address 0 -- the default address a device answers before SET_ADDRESS --
 * is the plugged pad that has not been given one yet. Pads are plugged one
 * at a time, each after the previous is configured, so there is never more
 * than one at address 0. */
static int ohci_pad_for(uint32_t ed0)
{
    uint32_t fa = ed0 & 0x7Fu, plugged = s_plugged;
    int d;
    for (d = 0; d < USB_GAMEPAD_MAX; d++)
        if ((plugged & (1u << d))
            && (fa ? usb_gamepad_address(d) == fa : usb_gamepad_address(d) == 0))
            return d;
    return -1;
}

/* Every address below came out of guest memory, so none of them are trusted.
 *
 * This is not theoretical. The driver writes HcControlHeadED twice during
 * bring-up, and the second write is 0xCCCCCCCC -- MSVC's uninitialised-memory
 * fill, left there by a local that was never assigned. Following it lands far
 * outside the mapping and takes the runtime down with an access violation,
 * which is a crash the title itself would never have had. A device model
 * following a pointer a title left lying around has to check it first.
 */
/* The contiguous (physical-mirror) window. Mapped separately from main RAM
 * and far above it, so a size check against the mapped RAM size rejects it. */
#define OHCI_CONTIG_BASE 0x80000000u
#define OHCI_CONTIG_SIZE (64u * 1024u * 1024u)

static int guest_ok(uint32_t va, uint32_t bytes)
{
    size_t mapped = xbox_GetMappedSize();

    if (va == 0 || mapped == 0)
        return 0;
    /* Descriptors live where a driver puts DMA memory, and on Xbox that is
     * MmAllocateContiguousMemory -- the contiguous window, not low RAM. The
     * bound below reads as "is this in RAM", which is not the question: the
     * window is mapped too, at the same offset, and is exactly where the Xbox
     * USB stack builds its endpoint and transfer descriptors. Rejecting it
     * made every rd32 return 0 and every wr32 a no-op, so the walker looked at
     * an empty list and the driver waited forever for a done queue that could
     * not be written. DDS9's first control transfer sits at 0x80408940. */
    if (va >= OHCI_CONTIG_BASE
            && (uint64_t)va + bytes <= (uint64_t)OHCI_CONTIG_BASE
                                     + OHCI_CONTIG_SIZE)
        return 1;
    return (uint64_t)va + bytes <= (uint64_t)mapped;
}

/* A host controller is a bus master: every pointer the driver hands it --
 * HcHCCA, HcControlHeadED, ED and TD links, buffer pointers -- is a PHYSICAL
 * address. On hardware physical P and the contiguous window's 0x80000000 + P
 * are the same bytes. Here the window is separate storage, so a physical
 * address a driver took from MmGetPhysicalAddress names nothing it wrote.
 *
 * Burnout 3's XPP does exactly that: it builds its descriptors in
 * MmAllocateContiguousMemory at 0x825B50C0, converts, and writes 0x025B50C0.
 * Read as RAM, every ED came back all zeroes, the controller saw an empty
 * list, and enumeration waited forever for a transfer that never moved.
 * (DDS9's driver writes window addresses directly, which guest_ok already
 * accepts, so it never needed this.)
 *
 * Resolved the way the pushbuffer executor resolves surface offsets
 * (dma_resolve in nv2a_pb_exec.c): below the contiguous allocator's
 * high-water mark an address is memory some MmAllocateContiguousMemory call
 * returned, and its bytes live in the window.
 *
 * Except inside the loaded image. A driver also points transfers at its own
 * statics -- Burnout 3's first GET_DESCRIPTOR reads into 0x0041A904, in .data
 * -- and MmGetPhysicalAddress passes those through unchanged. A real kernel
 * never hands out contiguous memory that overlaps the image, so an address
 * inside it is the image. Sending that one to the window delivered the
 * descriptor where the driver never looked, and it reset the port and asked
 * again, forever.
 *
 * The image therefore comes first. The contiguous allocator here does hand
 * out physical numbers inside the image's range (its window is separate
 * storage, so nothing collides there), and asking its page map first, as
 * mcpx_apu_phys (apu_core.c) does, sent 0x0041A904 to the window again:
 * Burnout 3's boot allocates 0x803E7000..0x805E7000 before XPP enumerates,
 * so page 0x41A is "contiguous", and enumeration retried the descriptor for
 * ever with the pad never attached. Outside the image the page map is asked
 * next: a page the first-fit allocator handed out -- or the reserved page 0,
 * where XPP keeps its private arena -- is the window; then the high-water
 * mark, for a page that was contiguous memory once and has since been
 * freed.
 *
 * Except a heap page the title named through MmGetPhysicalAddress
 * (xbox_PhysRamNameMap), which is RAM even when the arena has handed out
 * the same number: Jet Set Radio Future's XAPI reads each input report into
 * a heap buffer at 0x010DE0CA, on a page the arena also owns, so the reports
 * went to the window, the title's buffer never changed, and the pad looked
 * plugged in with nothing pressed. */
/* Which rule resolved each address, counted apart for descriptor words and
 * data buffers so RECOMP_USB_STATS (and RECOMP_USB_TRACE at exit) show what a
 * title leans on. Plain counters: the controller thread does almost all the
 * resolving, and a WDH acknowledgement on the title's thread can lose one
 * increment, which a count read by eye does not miss. */
enum {
    RES_IMAGE,       /* inside the loaded image                         */
    RES_RAMNAME,     /* a heap page the title named through Mm...Address */
    RES_PAGEMAP,     /* a live contiguous page: the window              */
    RES_HIGHWATER,   /* freed contiguous memory below the high-water    */
    RES_PLAIN,       /* none of the above: RAM as given                 */
    RES_DESC_WINDOW, /* descriptor on a live contiguous page            */
    RES_TD_BLOCK,    /* buffer in its TD's own contiguous allocation    */
    RES_N
};
static const char *const s_res_name[RES_N] = {
    "image", "ramname", "pagemap", "highwater", "plain", "desc-window",
    "td-block"
};
static unsigned long s_res_desc[RES_N], s_res_data[RES_N];

static uint32_t bus_resolve(uint32_t addr, unsigned long *count)
{
    int cls;
    uint32_t out = addr;

    if (addr >= g_xbox_image_lo && addr < g_xbox_image_hi)
        cls = RES_IMAGE;
    else if (addr && addr < (64u << 20) && xbox_PhysRamNameMap()[addr >> 12])
        cls = RES_RAMNAME;
    else if (addr && addr < (64u << 20) && xbox_ContiguousPageMap()[addr >> 12])
        cls = RES_PAGEMAP, out = OHCI_CONTIG_BASE + addr;
    else if (addr && addr < xbox_ContiguousAllocatedBytes())
        cls = RES_HIGHWATER, out = OHCI_CONTIG_BASE + addr;
    else
        cls = RES_PLAIN;
    count[cls]++;
    return out;
}

/* The controller's own structures -- the HCCA, EDs and TDs -- resolve the
 * other way round: a live contiguous page first, then the image.
 *
 * The image-first rule above is for data buffers, which a class driver
 * points at its statics. The descriptors are the host controller driver's,
 * and XAPI's OHCD takes them from MmAllocateContiguousMemory, because the
 * controller is a bus master that reads them by physical address. When that
 * allocation's physical number falls inside the image (the window is
 * separate storage here, and the arena hands such numbers out once a large
 * image leaves it no other room), image first reads code. Jet Set Radio
 * Future's XAPI (XDK 4134) gets its HCCA at 0x80082000 and writes
 * HcHCCA = 0x00082000, inside an image that runs to
 * 0x00288620: the walker read the interrupt table out of .text, found
 * garbage EDs, never moved a transfer, and the pad never enumerated -- one
 * root hub change, one done queue, then only start-of-frame interrupts.
 * (The arena keeps its blocks above that image now, so the two orders
 * agree there.)
 *
 * The cost: a descriptor a driver kept statically in its image, on a page
 * the arena has also handed out, now reads the window copy. No title known
 * here does that. The real fix is one physical space, where the window and
 * RAM below 64 MB are the same bytes and none of these rules is needed. */
static uint32_t bus_resolve_desc(uint32_t addr)
{
    if (addr && addr < (64u << 20) && xbox_ContiguousPageMap()[addr >> 12]) {
        s_res_desc[RES_DESC_WINDOW]++;
        return OHCI_CONTIG_BASE + addr;
    }
    return bus_resolve(addr, s_res_desc);
}

/* Descriptor words (HCCA, ED, TD); td_resolve below is for data buffers. */
static uint32_t rd32(uint32_t va)
{
    va = bus_resolve_desc(va);
    if (!guest_ok(va, 4))
        return 0;
    return *(uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + va);
}
static void wr32(uint32_t va, uint32_t v)
{
    va = bus_resolve_desc(va);
    if (!guest_ok(va, 4))
        return;
    *(uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + va) = v;
}

/* 1 if both physical addresses are live pages of one contiguous allocation.
 * Two addresses in one block end at the same place. */
static int same_contig_block(uint32_t a, uint32_t b)
{
    const volatile uint8_t *map = xbox_ContiguousPageMap();
    uint32_t na, nb;

    if (!a || !b || a >= (64u << 20) || b >= (64u << 20)
            || !map[a >> 12] || !map[b >> 12])
        return 0;
    na = xbox_ContiguousBlockSize(OHCI_CONTIG_BASE + a);
    nb = xbox_ContiguousBlockSize(OHCI_CONTIG_BASE + b);
    return na && nb && a + na == b + nb;
}

/* A transfer's buffer, as a guest address. One inside the TD's own
 * contiguous allocation is the OHCD's storage: XAPI keeps each SETUP packet
 * in the pool its TDs come from, and JSRF's sits at 0x00083FB0 beside TDs at
 * 0x00083Fxx, inside the image range, where the image-first rule read eight
 * bytes of .text as the request. Only the TD's own block goes to the window
 * this way: a contiguous page elsewhere can still be the image's. Burnout 3's
 * first GET_DESCRIPTOR reads into its .data at 0x0041A904, a page inside a
 * block its boot allocated (0x3E7000..0x5E7000), while its TDs are at
 * 0x025B5xxx; sending every contiguous buffer to the window brought that bug
 * back. Any other buffer can be a class driver's static, or a heap page the
 * title named, and takes bus_resolve's order. */
static uint32_t td_resolve(uint32_t td, uint32_t cbp)
{
    if (same_contig_block(td, cbp)) {
        s_res_data[RES_TD_BLOCK]++;
        return OHCI_CONTIG_BASE + cbp;
    }
    return bus_resolve(cbp, s_res_data);
}
static uint8_t *td_buf(uint32_t td, uint32_t cbp, uint32_t bytes)
{
    uint32_t va = td_resolve(td, cbp);
    return guest_ok(va, bytes)
         ? (uint8_t *)xbox_GetMemoryOffset() + va : NULL;
}

uint32_t xbox_OhciResolveForTest(int data, uint32_t td, uint32_t addr)
{
    return data ? td_resolve(td, addr) : bus_resolve_desc(addr);
}

static void ohci_print_res(void)
{
    int i;
    fprintf(stderr, " resolve desc");
    for (i = 0; i < RES_N; i++)
        if (s_res_desc[i])
            fprintf(stderr, " %s=%lu", s_res_name[i], s_res_desc[i]);
    fprintf(stderr, " data");
    for (i = 0; i < RES_N; i++)
        if (s_res_data[i])
            fprintf(stderr, " %s=%lu", s_res_name[i], s_res_data[i]);
}

/* What a transfer to an address no pad holds completes with; see ohci_do_td. */
static uint32_t ohci_absent_cc(void)
{
    return usb_gamepad_source() ? TD_CC_DEVICENOTRESPONDING : TD_CC_NOERROR;
}

uint32_t xbox_OhciNoDeviceForTest(uint32_t addr)
{
    return ohci_pad_for(addr & 0x7Fu) < 0 ? ohci_absent_cc() : 0xFFFFFFFFu;
}

/* Transfer counts for RECOMP_USB_STATS: input reports served, control
 * transfer stages, and OUT packets (rumble) by endpoint. */
static unsigned long s_n_report, s_n_ctrl, s_n_out_ep0, s_n_out_other;

/* Move one transfer descriptor. Returns the condition code to report. */
static uint32_t ohci_do_td(OhciController *hc, uint32_t ed0, uint32_t td)
{
    uint32_t info = rd32(td);
    uint32_t cbp  = rd32(td + 4);
    uint32_t be   = rd32(td + 12);
    uint32_t dp   = (info >> 19) & 3u;
    int      len  = (cbp && be >= cbp) ? (int)(be - cbp + 1) : 0;
    uint32_t endpoint = (ed0 >> 7) & 0xFu;
    int      moved = 0;
    int      pad = ohci_pad_for(ed0);

    /* No device at that address. Without a pad source: ignored, as it
     * always was. With one, the address may be a pad just unplugged, and
     * a controller whose transfer nobody answers retires it with DEVICE
     * NOT RESPONDING and halts the endpoint; completing it as a success
     * would hand the driver the stale buffer as a report. */
    if (pad < 0)
        return ohci_absent_cc();

    if (s_trace) {
        static unsigned n;
        if (n++ < 80) {
            fprintf(stderr, "  [OHCI%d] TD %08X dp=%u ep=%u cbp=%08X be=%08X "
                            "len=%d\n", hc->index, td, dp, endpoint, cbp, be,
                    len);
            fflush(stderr);
        }
    }
    if (dp == TD_DP_SETUP) {
        /* Eight bytes of setup, kept for the data stage that follows. */
        s_n_ctrl++;
        if (len >= 8) {
            const uint8_t *p = td_buf(td, cbp, 8);
            if (!p) return TD_CC_NOERROR;
            g_setup.bmRequestType = p[0];
            g_setup.bRequest      = p[1];
            g_setup.wValue        = (uint16_t)(p[2] | (p[3] << 8));
            g_setup.wIndex        = (uint16_t)(p[4] | (p[5] << 8));
            g_setup.wLength       = (uint16_t)(p[6] | (p[7] << 8));
            g_setup_pending = 1;
            g_ctrl_len = -1;   /* answered lazily on the first IN */
            g_ctrl_sent = 0;
            if (s_trace) {
                fprintf(stderr, "  [OHCI%d] SETUP %02X %02X value %04X "
                                "index %04X len %u\n",
                        hc->index, g_setup.bmRequestType, g_setup.bRequest,
                        g_setup.wValue, g_setup.wIndex, g_setup.wLength);
                fflush(stderr);
            }
            moved = 8;
        }
    } else if (dp == TD_DP_IN) {
        if (endpoint == 0) {
            /* Data stage of a control transfer, or its status stage when the
             * driver asks for nothing. */
            int n;

            if (!g_setup_pending)
                return TD_CC_NOERROR;
            if (g_ctrl_len < 0) {
                g_ctrl_len = usb_gamepad_control(pad, &g_setup, g_ctrl_buf,
                                                 (int)sizeof g_ctrl_buf);
                if (g_ctrl_len < 0) {
                    /* Worth saying out loud. A stall here halts the
                     * endpoint until the driver clears it, and a driver
                     * that sees one usually stops using the device -- so
                     * an unhandled request is not a gap that degrades
                     * gracefully, it is one that ends input. */
                    fprintf(stderr, "  [OHCI%d] STALL: unhandled control "
                            "request %02X %02X value %04X index %04X len %u\n",
                            hc->index, g_setup.bmRequestType, g_setup.bRequest,
                            g_setup.wValue, g_setup.wIndex, g_setup.wLength);
                    fflush(stderr);
                    g_setup_pending = 0;
                    return TD_CC_STALL;
                }
                g_ctrl_sent = 0;
            }
            /* Whatever is left, capped by this descriptor's buffer. A short
             * packet is how the device says "that is all", so running out is
             * the normal end of the stage rather than an error. */
            n = g_ctrl_len - g_ctrl_sent;
            if (n > len) n = len;
            if (n < 0) n = 0;
            if (n > 0) {
                uint8_t *d = td_buf(td, cbp, (uint32_t)n);
                if (d)
                    memcpy(d, g_ctrl_buf + g_ctrl_sent, (size_t)n);
            }
            g_ctrl_sent += n;
            moved = n;
            if (s_trace && n > 0) {
                fprintf(stderr, "  [OHCI%d] IN ep0 %d bytes (%d/%d)\n",
                        hc->index, n, g_ctrl_sent, g_ctrl_len);
                fflush(stderr);
            }
        } else {
            /* The pad's report, on its interrupt endpoint. */
            uint8_t rep[32];
            s_n_report++;
            int n = usb_gamepad_report(pad, rep, (int)sizeof rep);
            if (n > len) n = len;
            if (n > 0) {
                uint8_t *d = td_buf(td, cbp, (uint32_t)n);
                if (d)
                    memcpy(d, rep, (size_t)n);
            }
            moved = n;
        }
    } else {
        /* OUT: the status stage of an IN control transfer, or rumble. Only
         * the control pipe's ends a control transfer. A rumble packet on
         * endpoint 2 used to clear the pending setup too, so a force-feedback
         * write landing between a control request's stages broke that
         * request; Burnout 3 starts its engine-rev rumble on the Crash
         * countdown and then paused with the pad unresponsive. */
        moved = len;
        /* Rumble: an XID output report -- 00 06, left motor, right motor
         * (16-bit each) -- on the interrupt OUT endpoint, or as the data of
         * a SET_REPORT (21 09) on the control pipe. Forwarded to the host
         * pad; RECOMP_RUMBLE=0 ignores it. */
        if (len >= 6 && td_buf(td, cbp, 6u)
            && (endpoint != 0 || (g_setup_pending && g_setup.bmRequestType == 0x21
                                  && g_setup.bRequest == 0x09))) {
            static int rumble = -1;
            const uint8_t *r = td_buf(td, cbp, 6u);
            if (rumble < 0) {
                const char *e = recomp_env(RENV_RUMBLE);
                rumble = !(e && *e == '0');
            }
            if (rumble && r[0] == 0x00 && r[1] >= 6) {
                uint16_t left  = (uint16_t)(r[2] | r[3] << 8);
                uint16_t right = (uint16_t)(r[4] | r[5] << 8);
                if (s_trace)
                    fprintf(stderr, "  [OHCI%d] rumble pad %d left %u right %u (ep %u)\n",
                            hc->index, pad, left, right, endpoint);
                usb_gamepad_rumble(pad, left, right);
            }
        }
        if (endpoint == 0) {
            g_setup_pending = 0;
            s_n_out_ep0++;
        } else {
            s_n_out_other++;
        }
    }

    /* CBP is zero when everything asked for moved, and otherwise points past
     * what did. A driver computes the transferred length from it. */
    wr32(td + 4, (moved >= len) ? 0u : cbp + (uint32_t)moved);

    /* A short IN packet on a TD without bufferRounding is DATA UNDERRUN
     * (OHCI 1.0a): the controller retires this TD with that code
     * and halts the endpoint -- the walker does so for any non-zero code --
     * leaving the rest of the transfer for the driver to retire. A short
     * packet still ends the stage; what the driver needs to see is that it
     * did. Reporting NOERROR, and completing the trailing TDs with zero
     * bytes, sent XAPI's done-queue handler down its success path, which
     * walks from the ED head into the dummy tail TD and follows its garbage
     * link: X-Men Legends asks for 80 bytes of configuration descriptor,
     * gets 32, and faulted there. */
    if (dp == TD_DP_IN && moved < len && !(info & TD_ROUNDING))
        return TD_CC_DATAUNDERRUN;
    return TD_CC_NOERROR;
}

/* Walk the control list once. Returns 1 if anything completed. */
/* Walk one list of endpoint descriptors, moving whatever is queued on each.
 *
 * Shared, because the control list is not the only one that matters: a gamepad
 * delivers its reports on an interrupt endpoint, and those EDs hang off the
 * HCCA interrupt table rather than HcControlHeadED. Walking only the control
 * list enumerates a device perfectly and then never hears from it again. */
static int ohci_walk_eds(OhciController *hc, uint32_t ed, uint32_t *done_head)
{
    int completed = 0, guard = 0;

    while (guest_ok(ed, 16) && ++guard < 64) {
        uint32_t ed0  = rd32(ed);
        uint32_t tail = rd32(ed + 4) & ED_PTR_MASK;
        uint32_t head = rd32(ed + 8);
        int tguard = 0;

        if (s_trace) {
            static unsigned n;
            if (n++ < 60) {
                fprintf(stderr, "  [OHCI%d] ED %08X info=%08X head=%08X "
                                "tail=%08X next=%08X\n", hc->index, ed, ed0,
                        head, tail, rd32(ed + 12));
                fflush(stderr);
            }
        }
        if (ed0 & ED_SKIP) { ed = rd32(ed + 12) & ED_PTR_MASK; continue; }
        if (head & ED_HEAD_HALT) { ed = rd32(ed + 12) & ED_PTR_MASK; continue; }

        while ((head & ED_PTR_MASK) != tail
            && guest_ok(head & ED_PTR_MASK, 16) && ++tguard < 64) {
            uint32_t td = head & ED_PTR_MASK;
            uint32_t next = rd32(td + 8) & ED_PTR_MASK;
            uint32_t cc = ohci_do_td(hc, ed0, td);

            /* Report the outcome where the driver reads it, then put the
             * descriptor on the done queue, newest first. */
            wr32(td, (rd32(td) & 0x0FFFFFFFu) | (cc << 28));
            wr32(td + 8, *done_head);
            *done_head = td;
            completed++;

            head = next | (head & ED_HEAD_TOGGLE);
            if (cc != TD_CC_NOERROR) {
                head |= ED_HEAD_HALT;       /* a stall halts the endpoint */
                break;
            }
        }
        wr32(ed + 8, head);
        ed = rd32(ed + 12) & ED_PTR_MASK;
    }

    return completed;
}

/* Publish the done queue where the driver reads it and say so. */
static void ohci_publish_done(OhciController *hc, uint32_t done_head)
{
    uint32_t hcca = hc->reg[HcHCCA / 4];

    if (hcca)
        wr32(hcca + HCCA_DONE_HEAD, done_head);
    hc->reg[HcDoneHead / 4] = done_head;
    reg_set(&hc->reg[HcInterruptStatus / 4], INTR_WDH);
}

/* Both runners add to the caller's done queue rather than publishing their
 * own; see the service loop for why there is exactly one queue per pass. */
static int ohci_run_control_list(OhciController *hc, uint32_t *done_head)
{
    return ohci_walk_eds(hc, hc->reg[HcControlHeadED / 4] & ED_PTR_MASK,
                         done_head);
}

/* The periodic list for the current frame.
 *
 * The HCCA opens with 32 ED pointers, one per frame slot, and the controller
 * services the one the frame number selects. An interrupt endpoint polled
 * every 4 ms appears in several slots, so cycling through them the way the
 * frame counter does is what makes its transfers happen at all. */
static int ohci_run_periodic_list(OhciController *hc, uint32_t *done_head)
{
    uint32_t hcca = hc->reg[HcHCCA / 4];
    uint32_t slot, ed;
    int completed;

    if (!hcca)
        return 0;
    /* Every slot, not the one the frame counter happens to land on.
     *
     * A tick covers twenty frames, and stepping the counter by twenty means
     * only slots that are multiples of four are ever selected -- gcd(20,32)
     * is 4, so twenty-four of the thirty-two are never looked at. An
     * interrupt endpoint that lands in one of those is polled by the driver
     * and serviced by nobody. Sweeping all of them costs a guest read per
     * slot and cannot miss. */
    completed = 0;
    for (slot = 0; slot < 32u; slot++) {
        ed = rd32(hcca + slot * 4u) & ED_PTR_MASK;
        if (!ed)
            continue;
        /* The first interrupt endpoint the driver publishes is the one the
         * input reports ride on, so say so once: an enumerated pad whose
         * table stays empty is a pad nobody opened, which is a different
         * problem from a pad nobody serviced. */
        if (!hc->periodic_seen) {
            hc->periodic_seen = 1;
            fprintf(stderr, "  [OHCI%d] periodic slot %u -> ED %08X "
                    "info=%08X\n", hc->index, slot, ed, rd32(ed));
            fflush(stderr);
        }
        completed += ohci_walk_eds(hc, ed, done_head);
    }

    /* RECOMP_USB_STATS: one line every five seconds -- transfers the
     * periodic list completed, and the state of each endpoint on it. "The pad
     * stopped" has several causes that look the same from the title (nothing
     * queued, a halted endpoint, an interrupt never delivered), and the full
     * register trace is far too much to leave on for the minutes it takes to
     * reach one. */
    {
        static int stats = -1;
        static unsigned long last;
        static unsigned total;
        unsigned long now;
        total += (unsigned)completed;
        if (stats < 0)
            stats = recomp_env(RENV_USB_STATS) != NULL;
        now = (unsigned long)GetTickCount();
        if (stats && now - last >= 5000) {
            uint32_t seen[8] = {0};
            int nseen = 0, i;
            last = now;
            fprintf(stderr, "  [OHCI%d] stats: %u periodic transfers; "
                    "reports %lu setups %lu out0 %lu outN %lu; "
                    "ctl=%08X ien=%08X ists=%08X irql-depth=%d",
                    hc->index, total, s_n_report, s_n_ctrl, s_n_out_ep0,
                    s_n_out_other, hc->reg[HcControl / 4],
                    hc->reg[HcInterruptEnable / 4],
                    hc->reg[HcInterruptStatus / 4], xbox_IrqlRaisedCount());
            ohci_print_res();
            {
                uint32_t c = hc->reg[HcControlHeadED / 4] & ED_PTR_MASK;
                int n;
                fprintf(stderr, " cmd=%08X ctlhead=%08X",
                        hc->reg[HcCommandStatus / 4], c);
                for (n = 0; c && n < 4; n++, c = rd32(c + 12) & ED_PTR_MASK)
                    fprintf(stderr, " [cED %08X info=%08X head=%08X tail=%08X]",
                            c, rd32(c), rd32(c + 8), rd32(c + 4));
            }
            for (slot = 0; slot < 32u; slot++) {
                ed = rd32(hcca + slot * 4u) & ED_PTR_MASK;
                while (ed && nseen < 8) {
                    for (i = 0; i < nseen && seen[i] != ed; i++) ;
                    if (i < nseen)
                        break;
                    seen[nseen++] = ed;
                    if (rd32(ed) & 0x7FFu)          /* skip placeholder EDs */
                        fprintf(stderr, " | ED %08X info=%08X head=%08X "
                                "tail=%08X", ed, rd32(ed), rd32(ed + 8),
                                rd32(ed + 4));
                    ed = rd32(ed + 12) & ED_PTR_MASK;
                }
            }
            fprintf(stderr, "\n");
            /* A raised IRQL that never drops holds every interrupt off until
             * the forced-delivery timeout, which looks like a slow pad. Name
             * who is holding it. */
            if (xbox_IrqlRaisedCount() > 0)
                xbox_IrqlDumpHolders();
            fflush(stderr);
        }
    }
    return completed;
}

/* ---- raising an interrupt --------------------------------------------- */

/* Call the title's ISR on this thread, with a guest stack under it.
 *
 * BOOLEAN ServiceRoutine(PKINTERRUPT Interrupt, PVOID ServiceContext), stdcall,
 * so the two arguments go on the stack right to left with a return address on
 * top. The sentinel is what the routine pops on the way out; nothing jumps to
 * it, and a recognisable value beats a real address if it ever shows up in a
 * report.
 *
 * Returns what the routine returned: an ISR that does not claim the interrupt
 * returns FALSE, and that is worth seeing rather than assuming.
 */
static int ohci_isr_call(int index)
{
    uint32_t kinterrupt = xbox_GetConnectedInterrupt(OHCI_VECTOR);
    uint32_t routine, context;
    recomp_func_t fn;
    uint8_t *mem;
    int host;

    if (!kinterrupt)
        return -1;                      /* nothing connected yet            */

    mem     = (uint8_t *)xbox_GetMemoryOffset();
    routine = *(uint32_t *)(mem + kinterrupt + 0);
    context = *(uint32_t *)(mem + kinterrupt + 4);
    if (!routine)
        return -1;

    fn = recomp_lookup(routine);
    if (!fn) {
        fprintf(stderr, "  [OHCI%d] ISR 0x%08X has no translation\n",
                index, routine);
        fflush(stderr);
        return -1;
    }

    host = xbox_GuestUnwindHostBegin();
    if (!host)
        return -1;                      /* a reset is taking the guest down */

    g_esp -= 4; *(uint32_t *)(mem + g_esp) = context;      /* arg 2 */
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = kinterrupt;   /* arg 1 */
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = 0xDEADBEEFu;  /* return address */

    if (xbox_ProfOn()) {
        void *prev = xbox_ProfBegin(XBOX_PROF_ISR_OHCI, routine);
        int _irql = xbox_IrqlEnterInterrupt(16);
        long long t0 = xbox_ProfNowUs();
        fn();
        xbox_ProfEnd(prev, xbox_ProfNowUs() - t0);
        xbox_IrqlLeaveInterrupt(_irql);
    } else {
        int _irql = xbox_IrqlEnterInterrupt(16); fn(); xbox_IrqlLeaveInterrupt(_irql);
    }
    xbox_GuestUnwindHostEnd(host);
    return (int)(g_eax & 1u);
}

static int ohci_call_isr(OhciController *hc)
{
    int slot, claimed;

    if (!xbox_GetConnectedInterrupt(OHCI_VECTOR))
        return -1;
    /* One slice for the life of the raise. Sixteen exist and this takes one
     * only while the routine runs, so a title using them for its own workers
     * is not starved by a controller that interrupts. */
    slot = xbox_worker_stack_alloc();
    if (slot < 0) {
        fprintf(stderr, "  [OHCI%d] no worker stack for the ISR\n", hc->index);
        fflush(stderr);
        return -1;
    }
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;
    claimed = ohci_isr_call(hc->index);
    xbox_worker_stack_free(slot);
    return claimed;
}

/* The gate holder's run of a posted USB interrupt (kernel_hal.c has put it
 * on a worker stack). The status bits were set by the controller thread;
 * if the driver has cleared them since, the ISR declines. */
static volatile int s_posting_hc;       /* the controller that posted, for logs */

static void ohci_isr_on_holder(void)
{
    ohci_isr_call(s_posting_hc);
}

/* Set the status bits and, if the driver has unmasked them, call the ISR.
 *
 * MIE is the master enable and HcInterruptEnable is the per-source mask; a
 * controller that interrupts through either of those while they are clear is
 * a controller the driver has every right to be confused by.
 */
static void ohci_raise(OhciController *hc, uint32_t source)
{
    uint32_t enable = hc->reg[HcInterruptEnable / 4];
    int claimed;

    reg_set(&hc->reg[HcInterruptStatus / 4], source);

    if (!(enable & INTR_MIE) || !(enable & source))
        return;

    claimed = ohci_call_isr(hc);
    if (s_trace || claimed >= 0) {
        static unsigned n;
        if (n++ < 20) {
            fprintf(stderr, "  [OHCI%d] raised %08X -> ISR %s\n",
                    hc->index, source,
                    claimed < 0 ? "not callable" :
                    claimed ? "claimed it" : "declined it");
            fflush(stderr);
        }
    }
}

/* ---- bring-up ---------------------------------------------------------- */

static void ohci_reset(OhciController *hc, uint32_t base, int index)
{
    memset(hc, 0, sizeof *hc);
    hc->base  = base;
    hc->index = index;

    hc->reg[HcRevision / 4]       = 0x00000010u;   /* OHCI 1.0             */
    hc->reg[HcFmInterval / 4]     = 0x27782EDFu;   /* 11999, FSMPS default */
    hc->reg[HcPeriodicStart / 4]  = 0x00003E67u;   /* 90% of the frame     */
    hc->reg[HcLSThreshold / 4]    = 0x00000628u;

    /* Root hub: OHCI_PORTS downstream, ports always powered, no over-current
     * reporting. NoPowerSwitching keeps a driver from waiting on a power-on
     * sequence that has nothing to switch. */
    hc->reg[HcRhDescriptorA / 4]  = (uint32_t)s_ndp | (1u << 9);
    hc->reg[HcRhDescriptorB / 4]  = 0x00000000u;
    hc->reg[HcRhStatus / 4]       = 0x00000000u;

    /* Ports powered and empty. The gamepad is not here yet, deliberately.
     *
     * Presenting it as already connected does not work, and the reason is
     * worth keeping: the driver scans the root hub itself during bring-up,
     * sees the connect-status-change bit, clears it, and by the time it
     * unmasks the root hub interrupt there is no change left to report. The
     * pending status bit does not survive either, because the driver resets
     * the controller first and a reset clears interrupt status -- both of
     * those are correct behaviour, and between them a device that was always
     * there is a device that never arrives.
     *
     * A console detects the port change after the controller is running, so
     * that is what the controller thread does: it waits for operational with
     * the interrupt unmasked, and only then plugs the device in. */
    /* Every port the descriptor claims, not the first two.
     *
     * A root hub that reports four downstream ports and powers two has two
     * ports a driver will skip: no PortPowerStatus is an unpowered socket,
     * and nothing is expected to arrive on one. That made a device placed on
     * port 3 invisible, which read as "the driver only scans two ports" and
     * is really "we only powered two". */
    {
        unsigned p;
        for (p = 0; p < 4u; p++)
            hc->reg[(HcRhPortStatus1 + p * 4) / 4] =
                (p < s_ndp) ? PORT_PPS : 0u;
    }
}

/* ---- pads coming and going (with a game's pad source) ------------------ */

/* Which root port pad 0 sits on; pad n is on the next n along. XAPI maps
 * root ports 3, 4, 1, 2 to controller ports 1-4, so the default (index 2)
 * puts pad 0 on controller port 1. RECOMP_USB_PORT (0..3) picks another. */
static unsigned ohci_first_port(void)
{
    const char *pspec = recomp_env(RENV_USB_PORT);
    unsigned port = pspec ? (unsigned)atoi(pspec) : (s_ndp >= 4 ? 2u : 0u);
    if (port >= s_ndp) port = s_ndp - 1u;
    return port;
}

/* A pad unplugs after it has been absent this many passes (~250 ms), so a
 * source that drops a pad for a poll or two -- a host probe, a slot being
 * reassigned -- does not make the title see a pull and a reinsert. */
#define OHCI_UNPLUG_PASSES 64u
/* And the next pad arrives this many passes after the last one configured,
 * as without a source, so the driver enumerates one device at a time. */
#define OHCI_PLUG_SETTLE   50u

static unsigned s_gone[USB_GAMEPAD_MAX];
static unsigned s_hp_settle = OHCI_PLUG_SETTLE;

static void ohci_plug(OhciController *hc, int d)
{
    unsigned port = (ohci_first_port() + (unsigned)d) % s_ndp;
    usb_gamepad_detach(d);              /* enumerates from address 0 */
    reg_set(&hc->reg[(HcRhPortStatus1 + port * 4) / 4], PORT_CCS | PORT_CSC);
    reg_set(&hc->reg[HcInterruptStatus / 4], INTR_RHSC);
    s_plugged |= 1u << d;
    fprintf(stderr, "  [OHCI%d] pad %d arriving on port %u\n",
            hc->index, d + 1, port + 1);
    fflush(stderr);
}

/* What pulling the cable does: the port loses its device and its enable,
 * both changes latch, and the root hub interrupt tells the driver. The pad
 * forgets its address, and a transfer still queued to the old address gets
 * no answer (ohci_do_td), which is how XAPI learns it is gone. */
static void ohci_unplug(OhciController *hc, int d)
{
    unsigned port = (ohci_first_port() + (unsigned)d) % s_ndp;
    uint32_t *ps = &hc->reg[(HcRhPortStatus1 + port * 4) / 4];
    uint32_t was = __atomic_fetch_and(ps, ~(PORT_CCS | PORT_PES),
                                      __ATOMIC_SEQ_CST);
    s_plugged &= ~(1u << d);
    /* PESC says the port's enable changed, so only a port that was enabled
     * reports it: a pad pulled mid-enumeration, before the driver enabled
     * its port, is a connect change and nothing else, as the spec has it
     * and QEMU does. */
    reg_set(ps, PORT_CSC | ((was & PORT_PES) ? PORT_PESC : 0));
    reg_set(&hc->reg[HcInterruptStatus / 4], INTR_RHSC);
    usb_gamepad_detach(d);
    g_setup_pending_[d] = 0;
    g_ctrl_len_[d]      = -1;
    g_ctrl_sent_[d]     = 0;
    s_gone[d] = 0;
    /* The next plug waits the full settle, so the driver services this
     * unplug's RHSC first. A source that reports the pad again on the next
     * pass would otherwise reconnect it 4 ms later, the driver would read a
     * port that is connected with CSC set, and the disconnect would never
     * have happened as far as the title can tell. */
    s_hp_settle = 0;
    fprintf(stderr, "  [OHCI%d] pad %d removed from port %u\n",
            hc->index, d + 1, port + 1);
    fflush(stderr);
}

/* One controller pass: plug what the source has, unplug what it lost. */
static void ohci_hotplug(OhciController *hc)
{
    uint32_t want = usb_gamepad_present();
    uint32_t have = s_plugged;
    int d;

    /* RECOMP_USB_PADS given with a source keeps that many plugged anyway,
     * for a run that wants the ports filled without host pads. */
    if (s_npads_set)
        want |= (1u << s_npads) - 1u;
    want &= (1u << s_ndp) - 1u;         /* a pad needs a port */

    for (d = 0; d < USB_GAMEPAD_MAX; d++) {
        if (!(have & (1u << d)))
            continue;
        if (want & (1u << d))
            s_gone[d] = 0;
        else if (++s_gone[d] >= OHCI_UNPLUG_PASSES)
            ohci_unplug(hc, d);
    }

    /* One device at the default address at a time: wait until every
     * plugged pad is configured, then let the bus settle. */
    have = s_plugged;
    for (d = 0; d < USB_GAMEPAD_MAX; d++)
        if ((have & (1u << d)) && !usb_gamepad_configured(d)) {
            s_hp_settle = 0;
            return;
        }
    if (s_hp_settle < OHCI_PLUG_SETTLE) {
        s_hp_settle++;
        return;
    }
    for (d = 0; d < USB_GAMEPAD_MAX; d++)
        if ((want & ~have) & (1u << d)) {
            ohci_plug(hc, d);
            s_hp_settle = 0;
            return;
        }
}

uint32_t xbox_OhciWriteForTest(uint32_t off, uint32_t v)
{
    OhciController *hc = &s_hc[s_device_hc];
    ohci_write(hc, off, v, 4);
    uint32_t *r = reg_of(hc, off & ~3u);
    return r ? *r : 0;
}

uint32_t xbox_OhciHotplugForTest(int reset, uint32_t ports[4])
{
    OhciController *hc = &s_hc[s_device_hc];
    unsigned p;
    if (reset) {
        int d;
        ohci_reset(hc, s_device_hc ? XBOX_OHCI1_BASE : XBOX_OHCI0_BASE,
                   s_device_hc);
        for (d = 0; d < USB_GAMEPAD_MAX; d++) {
            if (s_plugged & (1u << d))
                usb_gamepad_detach(d);
            s_gone[d] = 0;
        }
        s_plugged   = 0;
        s_hp_settle = OHCI_PLUG_SETTLE;
    }
    ohci_hotplug(hc);
    for (p = 0; p < 4u; p++)
        ports[p] = hc->reg[(HcRhPortStatus1 + p * 4) / 4];
    return s_plugged;
}

/* The controller, running on its own thread.
 *
 * It has to be its own thread for a reason that is easy to get wrong: the
 * guest register file is thread-local, so setting g_esp to a worker stack on
 * the guest's thread would overwrite the guest's own stack pointer mid-call.
 * A separate thread has its own copy, and it also happens to be what the
 * hardware does -- an interrupt arrives when the controller decides, not when
 * the driver next reads a register.
 *
 * ponytail: one delivery, of the root hub status change that is already
 * pending from bring-up. There is nothing behind it yet -- no descriptor list
 * walking, so an enumeration attempt has nothing to answer it -- and the point
 * of this delivery is to find out what the driver does when it finally gets
 * the interrupt it has been waiting for. Repeat delivery and the transfer
 * lists come after that answer, not before it.
 */
/* RECOMP_DEBUG=deterministic with the guest lock (kernel_guest_det.c): a
 * pass runs only when the guest scheduler grants one, once per vblank step
 * with every guest thread parked, so the pads' reports, the done queue and
 * the interrupt land at the same point of guest progress on every run, and
 * a title's XAPI polls its pads once per frame there. On the host's 4 ms
 * tick a pass fell anywhere in the guest's frame, and a record on the poll
 * clock (port-0 reports) counted host time. Until the first guest join
 * decides the mode the thread naps and does nothing (D8): a pass begun then
 * could still run when the mode turns on, ungranted. */
static int  s_det;                      /* this thread's passes are granted */
static int  s_det_granted;              /* a grant is open: done owed */
static int  s_det_started;              /* s_det_ms set by the first grant */
static uint64_t s_det_ms;               /* the virtual ms of the last pass */

/* Before a pass: the host tick, or the next grant. 0 when this round is a
 * nap (the mode undecided) and the pass must not run. */
static int ohci_pass_begin(void)
{
    if (s_det_granted) {
        xbox_GuestDetDeviceDone(XBOX_DET_DEV_OHCI, 1);
        s_det_granted = 0;
    }
    if (!s_det) {
        if (xbox_GuestDetUndecided()) {
            Sleep(1);
            return 0;
        }
        s_det = xbox_GuestDetOn() ? 1 : -1;     /* decided: final */
    }
    if (s_det > 0) {
        xbox_GuestDetDeviceWait(XBOX_DET_DEV_OHCI);
        s_det_granted = 1;
        /* The clock's origin is read inside the first grant, not when the
         * mode is decided: the nap before that can end on either side of
         * the first vblank step, and an origin one vblank early would shift
         * every later frame number by one between runs. */
        if (!s_det_started) {
            s_det_started = 1;
            s_det_ms = xbox_GuestTimeMs();
        }
    } else {
        Sleep(OHCI_TICK_MS);
    }
    return 1;
}

/* USB frames this pass is worth: the tick on the host clock, the virtual
 * milliseconds since the last pass (16 or 17 a vblank) in the mode. */
static uint32_t ohci_pass_frames(void)
{
    uint64_t now, d;

    if (s_det <= 0)
        return OHCI_TICK_MS;
    now = xbox_GuestTimeMs();
    d = now - s_det_ms;
    s_det_ms = now;
    return (uint32_t)d;
}

/* A pass with the controller not Operational: no frames go by. Real OHCI
 * counts frames only while Operational, so in the mode the origin follows
 * virtual time here; otherwise the first Operational pass would add every
 * millisecond since the mode was decided (more than a 16-bit frame number
 * holds on a slow boot). Passes with MIE clear still count: the controller
 * runs then, it just does not interrupt. */
static void ohci_pass_idle(void)
{
    if (s_det > 0)
        s_det_ms = xbox_GuestTimeMs();
}

/* The thread is leaving: the scheduler stops waiting for its passes. */
static void ohci_pass_end(void)
{
    s_det_granted = 0;
    xbox_GuestDetDeviceRemove(XBOX_DET_DEV_OHCI);
}

/* The controller thread's own TIB; 0, with the thread leaving, when the
 * heap has none. */
static int ohci_thread_tib(void)
{
    /* The thread calls recompiled code, so it needs what any thread running
     * recompiled code needs: its own TIB. The guest register set is already
     * thread-local and the ISR gets a worker stack, but fs:[0] is the SEH
     * chain head and fs:[4] reaches the CRT's per-thread data -- and a guest
     * function with an SEH prologue on a thread whose g_fs_base is zero
     * dereferences null before it executes a line of its own body. That is
     * what killed the process here, two interrupts in, with no fault report
     * because the fault was in the runtime rather than in the title. */
    uint32_t tib = xbox_AllocThreadTib();

    if (!tib) {
        fprintf(stderr, "  [OHCI0] no TIB for the controller thread; "
                        "not delivering interrupts\n");
        fflush(stderr);
        ohci_pass_end();
        return 0;
    }
    g_fs_base = tib;
    return 1;
}

static DWORD WINAPI ohci_thread(LPVOID unused)
{
    xbox_log_thread_role("ohci", 0);
    unsigned waited = 0;
    int plugged = 0;
    uint32_t last_status = 0;
    unsigned repeats = 0;
    unsigned held_off = 0;
    int      held_off_warned = 0, held_off_forced = 0, gated = 0;
    unsigned last_ack = 0;
    unsigned first_port = 0, settle = 0;
    int      npl = 1;              /* pads plugged without a source */
    /* The TIB: at thread start off the virtual clock, where the mode
     * cannot turn on, as it always was. On it, from the first pass, which
     * the mode grants with every guest thread idle: taken at thread start
     * it raced the main thread's first heap allocations, and its place in
     * the heap moved every later allocation between runs. */
    int      tib_ready = 0;

    (void)unused;
    if (!xbox_GuestTimeVirtual()) {
        if (!ohci_thread_tib())
            return 0;
        tib_ready = 1;
    }

    for (;;) {
        OhciController *hc = &s_hc[s_device_hc];
        uint32_t control, enable, status;
        uint32_t pass_done = 0;     /* this pass's done queue, newest first */

        if (!ohci_pass_begin())
            continue;
        if (!tib_ready) {
            if (!ohci_thread_tib())
                return 0;
            tib_ready = 1;
        }
        control = hc->reg[HcControl / 4];
        enable  = hc->reg[HcInterruptEnable / 4];

        /* Operational is HCFS == 10b in bits 7:6. Interrupting a controller
         * the driver has not started yet is not a test of anything. */
        if ((control & 0xC0u) != 0x80u) {
            /* 6 s on the 4 ms tick; about 25 s of virtual time in the mode,
             * where a pass is one vblank. */
            if (++waited > 1500)
                break;
            ohci_pass_idle();
            continue;
        }
        if (!(enable & INTR_MIE))
            continue;

        /* With a game's pad source, pads come and go with it. */
        if (usb_gamepad_source()) {
            if (enable & INTR_RHSC)
                ohci_hotplug(hc);
            goto plugged_done;
        }

        /* Plug the device in once, after the driver is running and listening.
         * Presenting it earlier does not work: the driver clears the connect
         * change during its own bring-up scan, and a reset clears interrupt
         * status, so a device that was always there is one that never
         * arrives. */
        if (!plugged && (enable & INTR_RHSC)) {
            /* Which port the device appears on decides which controller slot
             * XAPI assigns it, and a title that reads "player 1" without
             * asking which port pressed Start looks at slot 0. The default is
             * root port 3 (index 2), controller port 1, when there are four;
             * RECOMP_USB_PORT (0..3) picks another. */
            unsigned port = ohci_first_port();
            reg_set(&hc->reg[(HcRhPortStatus1 + port * 4) / 4], PORT_CCS | PORT_CSC);
            reg_set(&hc->reg[HcInterruptStatus / 4], INTR_RHSC);
            plugged = 1;
            s_plugged = 1u;
            first_port = port;
            fprintf(stderr, "  [OHCI0] operational after %u passes; device "
                            "arriving on port %u\n", waited, port + 1);
            fflush(stderr);
        }

        /* More pads, one at a time: each arrives after the previous one is
         * configured, on the next port, so the driver enumerates them in
         * turn and there is only ever one device at the default address.
         * RECOMP_USB_PADS sets how many (1-4). */
        if (plugged && npl < s_npads && usb_gamepad_configured(npl - 1)) {
            /* ~200 ms after the last on the 4 ms tick, ~830 ms of virtual
             * time in the mode (a pass a vblank). */
            if (++settle > 50) {
                unsigned port = (first_port + (unsigned)npl) % s_ndp;
                reg_set(&hc->reg[(HcRhPortStatus1 + port * 4) / 4], PORT_CCS | PORT_CSC);
                reg_set(&hc->reg[HcInterruptStatus / 4], INTR_RHSC);
                fprintf(stderr, "  [OHCI0] pad %d arriving on port %u%c",
                        npl + 1, port + 1, 10);
                fflush(stderr);
                s_plugged |= 1u << npl;
                npl++;
                settle = 0;
            }
        }

    plugged_done:

        /* The frame clock.
         *
         * INTR_SF was defined and never raised, and HcFmNumber only moved when
         * something read it. That is enough for a driver that polls, and not
         * enough for one that waits: USB enumeration is full of delays counted
         * in frames -- the recovery time after SET_ADDRESS is the first -- and
         * the Xbox stack enables StartOfFrame precisely to count them. Without
         * a frame ever starting, enumeration stops one transfer past
         * SET_ADDRESS with the driver waiting for time to pass.
         *
         * Twenty frames a tick because a frame is 1 ms and this loop sleeps
         * 20. The count is what drivers compare; the rate is what they feel,
         * and both are closer to right this way than a counter that never
         * moves. (The tick is 4 ms now; under RECOMP_DEBUG=deterministic a
         * pass is a vblank, so the count moves by 16 or 17 virtual ms and
         * StartOfFrame is raised once a vblank, not once a frame.) */
        {
            uint32_t hcca = hc->reg[HcHCCA / 4];

            hc->reg[HcFmNumber / 4] =
                (hc->reg[HcFmNumber / 4] + ohci_pass_frames()) & 0xFFFFu;
            /* HccaFrameNumber is 16 bits at +0x80 with a pad above it that the
             * controller zeroes, so a dword write is what hardware does. */
            if (hcca)
                wr32(hcca + 0x80, hc->reg[HcFmNumber / 4]);
            reg_set(&hc->reg[HcInterruptStatus / 4], INTR_SF);
        }

        /* Be the bus master. ControlListEnable in HcControl says the driver
         * wants the list walked; ControlListFilled says it has put something
         * on it. Walking on the tick rather than only when CLF is written
         * costs a read of a guest dword and means a descriptor queued without
         * rewriting CLF is still moved -- which is legal, and drivers do it. */
        /* Not while the driver still owes us an acknowledgement.
         *
         * HccaDoneHead belongs to the driver from the moment WDH is set
         * until it clears it. Writing a second done list over the first
         * loses every descriptor on it -- the driver walks a chain whose
         * links now point into the new list, and the structures it keeps
         * alongside go with it. At a twenty-millisecond tick that overlap
         * was rare enough to look like bad luck; at four it corrupted the
         * host controller's pending list within a second or two of the
         * gamepad's reports starting. Leaving the descriptors on their
         * endpoints for another pass costs a few milliseconds of latency
         * and is what the controller is specified to do. */
        if (hc->reg[HcInterruptStatus / 4] & INTR_WDH)
            goto deliver;

        /* PeriodicListEnable, bit 2 of HcControl. */
        if (control & 0x04u) {
            if (!hc->ple_seen) {
                hc->ple_seen = 1;
                fprintf(stderr, "  [OHCI%d] periodic list enabled "
                        "(HcControl=%08X HCCA=%08X)\n",
                        hc->index, control, hc->reg[HcHCCA / 4]);
                fflush(stderr);
            }
            ohci_run_periodic_list(hc, &pass_done);
        }

        /* BulkListEnable, bit 5. Nothing on this device uses bulk, but the
         * list is walked the same way and a driver that puts a transfer
         * there is owed the same service. */
        if ((control & 0x20u)
         && guest_ok(hc->reg[HcBulkHeadED / 4] & ED_PTR_MASK, 16)) {
            if (ohci_walk_eds(hc, hc->reg[HcBulkHeadED / 4] & ED_PTR_MASK,
                              &pass_done))
                hc->reg[HcCommandStatus / 4] &= ~0x04u;   /* BLF consumed */
        }

        if ((control & 0x10u)
         && guest_ok(hc->reg[HcControlHeadED / 4] & ED_PTR_MASK, 16)) {
            if (ohci_run_control_list(hc, &pass_done))
                hc->reg[HcCommandStatus / 4] &= ~0x02u;   /* CLF consumed */
        }

        /* One done queue per pass, covering all three lists.
         *
         * The lists used to publish one after another, so the control list
         * wrote HccaDoneHead straight over what the periodic list had just
         * put there. That stayed invisible until a title used both at once:
         * Burnout 3 sends its pad a rumble report (SET_REPORT, 21 09) on the
         * control pipe every frame once its menus are up, the input report
         * completing in the same pass was lost, and the driver -- which
         * re-arms the interrupt endpoint from that completion -- stopped
         * polling the pad. A real controller accumulates everything that
         * retires in a frame and writes the queue back once; so does this. */
        if (pass_done)
            ohci_publish_done(hc, pass_done);

        /* Level-triggered, which is what OHCI is: while an enabled source is
         * set, the line is asserted. The handler clears the status bit, so
         * this stops on its own -- and if it ever does not, the cap below says
         * so rather than spinning the ISR forever. */
    deliver:
        status = hc->reg[HcInterruptStatus / 4] & enable & 0x7Fu;
        if (!status)
            continue;

        /* (Without safe points only, below.) Bounded, because the mask
         * is a model and not the hardware.
         *
         * The depth is a count of raise/lower pairs across every thread, and
         * one unmatched raise anywhere leaves the gate shut for the rest of
         * the run -- which is how this first showed up: no interrupt was ever
         * delivered and enumeration stopped at two empty control EDs. Real
         * hardware is never masked for 80 ms, so past that the line is
         * asserted anyway. The common case still holds the ISR out of the
         * guest's critical section; the pathological case costs a delay
         * instead of the device. */
        /* The one-CPU gate, as the APU model takes it: when free, no thread
         * at DISPATCH_LEVEL, DPC or KeSynchronizeExecution routine runs
         * beside the ISR; when held, the interrupt is posted to the holder
         * (xbox_IrqPost) and this pass goes on. The line stays up, so the
         * next pass posts again (coalesced) until the ISR has run. */
        xbox_IrqSetHandler(XBOX_IRQ_OHCI, ohci_isr_on_holder);
        s_posting_hc = hc->index;
        if (xbox_IrqSafePointsOn()) {
            int how = xbox_IrqPost(XBOX_IRQ_OHCI);
            if (how == XBOX_IRQ_POSTED)
                continue;               /* a post is not a delivery */
            gated = (how == XBOX_IRQ_GATED);
            if (!gated)
                xbox_IrqNoteUngated(XBOX_IRQ_OHCI);
        } else {
            /* No gate (the default): held off while a guest thread is at
             * raised IRQL; RECOMP_IRQ_SAFE_POINTS=0 under the gate: held off
             * while the gate is. Then beside the holder either way. Without
             * the gate the try always succeeds and the leave takes nothing. */
            gated = xbox_DispatchGateOn() ? xbox_DispatchGateTryEnter()
                                          : !xbox_IrqlBlocksInterrupts();
            if (!gated && ++held_off <= OHCI_IRQ_HOLDOFF) {
                if (!held_off_warned) {
                    held_off_warned = 1;
                    fprintf(stderr, "  [OHCI%d] irq held off by guest IRQL "
                            "(depth %d, status=0x%02X)\n",
                            hc->index, xbox_IrqlRaisedCount(), status);
                    fflush(stderr);
                }
                continue;
            }
            if (held_off > OHCI_IRQ_HOLDOFF && !held_off_forced) {
                held_off_forced = 1;
                fprintf(stderr, "  [OHCI%d] guest IRQL never dropped (depth %d); "
                        "delivering anyway\n", hc->index, xbox_IrqlRaisedCount());
                fflush(stderr);
            }
            if (!gated)
                xbox_IrqNoteUngated(XBOX_IRQ_OHCI);
            held_off = 0;
        }

        /* A stuck source is one the handler never clears. Repeating the
         * same status value is not that.
         *
         * This counted deliveries of an unchanged status and gave up at
         * 200, and a working gamepad trips that in two seconds: the pad
         * reports at 100 Hz, every report completes a transfer descriptor,
         * every completion raises WritebackDoneHead, and every one of them
         * is the same status value. The driver was acknowledging each one
         * -- 141 writes to HcInterruptStatus in the run this was found on
         * -- and the count climbed anyway, because nothing reset it.
         *
         * What distinguishes the two is whether the driver wrote the
         * register at all between one delivery and the next. */
        if (status == last_status && hc->ack_seq == last_ack) {
            if (++repeats > 200) {
                fprintf(stderr, "  [OHCI%d] status %08X delivered 200 times "
                                "with no acknowledgement; stopping\n",
                        hc->index, status);
                fflush(stderr);
                if (gated)
                    xbox_DispatchGateLeave();
                break;
            }
        } else {
            last_status = status;
            last_ack    = hc->ack_seq;
            repeats     = 0;
        }
        ohci_raise(hc, status);
        if (gated)
            xbox_DispatchGateLeave();
    }
    ohci_pass_end();
    return 0;
}

void xbox_OhciInit(void)
{
    static int done;

    if (done)
        return;
    done = 1;

    /* Opt-in per title: one that reaches its pad some other way (an XInput
     * override, as BLiNX 2 does) must not also get a controller under its
     * XAPI. A title whose statically linked XAPI drives these registers
     * turns it on by default in its main.c. =0 (or empty) is off, so a
     * default can be overridden from the environment. */
    if (!recomp_env_on(RENV_USB))
        return;

    s_enabled = 1;
    s_trace   = recomp_env(RENV_USB_TRACE) != NULL;
    {
        const char *hcspec = recomp_env(RENV_USB_HC);
        const char *ndpspec = recomp_env(RENV_USB_NDP);
        s_device_hc = (hcspec && atoi(hcspec) == 1) ? 1 : 0;
        const char *padspec = recomp_env(RENV_USB_PADS);
        if (ndpspec) {
            int n = atoi(ndpspec);
            if (n >= 1 && n <= 4) s_ndp = (unsigned)n;
        }
        if (padspec) {
            int n = atoi(padspec);
            if (n >= 1 && n <= USB_GAMEPAD_MAX) s_npads = n, s_npads_set = 1;
        }
        if ((unsigned)s_npads > s_ndp)
            s_ndp = (unsigned)s_npads;      /* a port for every pad */
    }
#if !defined(OHCI_TRAP_X64) && !defined(OHCI_TRAP_POSIX_A64)
    /* No decoder for this host's instructions, so a trapped access could not
     * be answered. Leave the aperture as plain memory -- the title then sees
     * no controller, exactly as with RECOMP_USB unset -- and say so. */
    s_enabled = 0;
    fprintf(stderr, "  OHCI: no trapped-MMIO decoder for this host; "
                    "USB controllers disabled\n");
    fflush(stderr);
    return;
#endif

    ohci_reset(&s_hc[0], XBOX_OHCI0_BASE, 0);
    ohci_reset(&s_hc[1], XBOX_OHCI1_BASE, 1);

    /* The registers have to fault to be answered. The MCPX aperture is mapped
     * as plain committed memory, so both blocks are made inaccessible here and
     * the title's fault handler (a VEH on Windows, a SIGBUS/SIGSEGV handler on
     * POSIX) routes the faults back to xbox_OhciHandleMmio. On a host whose
     * page is larger than a block (16 KB on macOS arm64) the protection covers
     * the whole page; nothing else lives in it.
     *
     * A failed protect switches the model off rather than leaving it half on:
     * a controller whose registers read as zero out of RAM is exactly the
     * situation this exists to end, and it would look identical. */
    {
        ptrdiff_t off = xbox_GetMemoryOffset();
        int i;

        if (!off) {
            s_enabled = 0;
            fprintf(stderr, "  OHCI: guest memory not mapped yet; disabled\n");
            return;
        }
        for (i = 0; i < 2; i++) {
            DWORD old_protect;
            LPVOID at = (LPVOID)((uintptr_t)off + s_hc[i].base);
            if (!VirtualProtect(at, XBOX_OHCI_SIZE, PAGE_NOACCESS,
                                &old_protect)) {
                s_enabled = 0;
                fprintf(stderr, "  OHCI: cannot trap 0x%08X (error %lu); "
                                "disabled\n", s_hc[i].base,
                                (unsigned long)GetLastError());
                return;
            }
        }
    }

    fprintf(stderr, "  OHCI: two controllers at 0x%08X and 0x%08X, "
                    "%u ports each, pads on HC%d (%s)\n",
            XBOX_OHCI0_BASE, XBOX_OHCI1_BASE, s_ndp, s_device_hc,
            usb_gamepad_source() ? "the game's pad source" : "host pads");
    fflush(stderr);

    {
        HANDLE th;

        /* Before the thread: in the deterministic mode a step grants it a
         * pass from then on (a no-op otherwise). */
        xbox_GuestDetDeviceAdd(XBOX_DET_DEV_OHCI);
        th = CreateThread(NULL, 0, ohci_thread, NULL, 0, NULL);
        if (th)
            CloseHandle(th);
        else
            xbox_GuestDetDeviceRemove(XBOX_DET_DEV_OHCI);
    }
}

static OhciController *hc_for(uint32_t va)
{
    int i;

    if (!s_enabled)
        return NULL;
    for (i = 0; i < 2; i++)
        if (va >= s_hc[i].base && va < s_hc[i].base + XBOX_OHCI_SIZE)
            return &s_hc[i];
    return NULL;
}

int xbox_OhciOwnsAddress(uint32_t xbox_va)
{
    return hc_for(xbox_va) != NULL;
}

#if defined(OHCI_TRAP_POSIX_A64) || defined(OHCI_TRAP_X64)
/* Both decoders hand over 8-byte accesses whole (ldr/str x or a d register
 * on A64, a REX.W mov on x86-64); the registers are 32-bit, so an 8-byte
 * access is two of them. */
static uint64_t ohci_read_split(void *dev, uint32_t off, int size)
{
    if (size == 8)
        return ohci_read(dev, off, 4) | (ohci_read(dev, off + 4, 4) << 32);
    return ohci_read(dev, off, size);
}

static void ohci_write_split(void *dev, uint32_t off, uint64_t val, int size)
{
    if (size == 8) {
        ohci_write(dev, off, (uint32_t)val, 4);
        ohci_write(dev, off + 4, val >> 32, 4);
        return;
    }
    ohci_write(dev, off, val, size);
}
#endif

int xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va)
{
#if defined(OHCI_TRAP_POSIX_A64)
    ucontext_t *uc = (ucontext_t *)ctx;
    OhciController *hc = hc_for(xbox_va);
    int ok;

    if (!hc)
        return 0;
    ok = mmio_emulate_a64(uc, xbox_va - hc->base, hc,
                          ohci_read_split, ohci_write_split);
    /* stdio in a signal handler, deliberately: see apu_mmio_hook.c. */
    if (!ok && hc->decode_fail++ < 20) {
        fprintf(stderr, "  [OHCI%d] undecoded access at +0x%03X: insn %08X\n",
                hc->index, xbox_va - hc->base,
                *(const uint32_t *)(uintptr_t)A64_CTX_PC(uc));
        fflush(stderr);
    }
    return ok;
#elif defined(OHCI_TRAP_X64)
    OhciController *hc = hc_for(xbox_va);
    int ok;

    if (!hc)
        return 0;
    ok = mmio_emulate((mmio_ctx_t *)ctx, xbox_va - hc->base, hc,
                      ohci_read_split, ohci_write_split);
    if (!ok && hc->decode_fail++ < 20) {
        const uint8_t *ip =
            (const uint8_t *)(uintptr_t)MMIO_RIP((mmio_ctx_t *)ctx);
        fprintf(stderr, "  [OHCI%d] undecoded access at +0x%03X: "
                        "%02X %02X %02X %02X %02X %02X\n",
                hc->index, xbox_va - hc->base,
                ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return ok;
#else
    (void)ctx; (void)xbox_va;
    return 0;
#endif
}

void xbox_OhciReport(void)
{
    int i;

    if (!s_enabled)
        return;
    fprintf(stderr, "  [OHCI] address classes:");
    ohci_print_res();
    fputc('\n', stderr);
    for (i = 0; i < 2; i++)
        fprintf(stderr, "  [OHCI%d] %u reads, %u writes, %u undecoded; "
                        "HcControl=%08X HcIntStatus=%08X port1=%08X\n",
                i, s_hc[i].reads, s_hc[i].writes, s_hc[i].decode_fail,
                s_hc[i].reg[HcControl / 4],
                s_hc[i].reg[HcInterruptStatus / 4],
                s_hc[i].reg[HcRhPortStatus1 / 4]);
    fflush(stderr);
}
