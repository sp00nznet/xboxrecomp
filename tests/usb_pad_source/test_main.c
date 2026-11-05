/*
 * A game's pad source behind the emulated Xbox controller.
 *
 * A title that drives the pad through its own statically linked XAPI never
 * calls XInputGetState, so a game layer that maps host pads to ports, runs
 * scripts or merges the keyboard has nowhere to stand but here. Without the
 * hook a pad was host pad n and the bus held a fixed number of pads that
 * never left; these checks cover what the hook adds: the report is the
 * source's, the toolkit script still keeps its pads, rumble goes to the
 * source, and the root hub plugs and unplugs pads as the source's set
 * changes.
 */
#include "ohci.h"
#include "usb_gamepad.h"
#include "xinput_xbox.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Supplied by recompiled game code in a real build; nothing here calls one. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

/* The registers the driver writes, by offset. */
#define REG_INTR_STATUS  0x0Cu
#define REG_PORT(n)      (0x54u + (n) * 4u)
#define INTR_RHSC        0x00000040u
#define PORT_SET_ENABLE  0x00000002u
#define PORT_CSC         0x00010000u
#define PORT_PESC        0x00020000u
#define CC_NOERROR       0u
#define CC_NOT_RESPONDING 5u
/* Passes the root hub waits after a pad configures or leaves (ohci.c). */
#define PLUG_SETTLE      50

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-56s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static uint32_t s_present = 1u;
static int s_rumble_pad = -1;
static uint16_t s_rumble_l, s_rumble_r;

static int src_get_state(int pad, XBOX_GAMEPAD *out)
{
    if (pad != 0)
        return 0;
    out->wButtons = XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_LEFT;
    out->bAnalogButtons[0] = 0xFF;          /* A */
    out->bAnalogButtons[7] = 0x80;          /* right trigger */
    out->sThumbLX = 0x1234;
    out->sThumbRY = -2;
    return 1;
}

static uint32_t src_present(void) { return s_present; }

static void src_rumble(int pad, uint16_t l, uint16_t r)
{
    s_rumble_pad = pad;
    s_rumble_l = l;
    s_rumble_r = r;
}

static const UsbPadSource s_src = { src_get_state, src_present, src_rumble };

/* What the driver does once a pad arrives at the default address. */
static void enumerate(int pad, uint8_t addr)
{
    UsbSetup su;
    uint8_t buf[8];
    memset(&su, 0, sizeof su);
    su.bRequest = 0x05;                     /* SET_ADDRESS */
    su.wValue = addr;
    usb_gamepad_control(pad, &su, buf, sizeof buf);
    su.bRequest = 0x09;                     /* SET_CONFIGURATION */
    su.wValue = 1;
    usb_gamepad_control(pad, &su, buf, sizeof buf);
}

/* Passes until `mask` is plugged, or 0 after `max`. */
static int passes_until(uint32_t mask, int max, uint32_t ports[4])
{
    int n;
    for (n = 1; n <= max; n++)
        if (xbox_OhciHotplugForTest(0, ports) == mask)
            return n;
    return 0;
}

int main(void)
{
    uint8_t r[20];
    uint32_t ports[4], m;
    int n;

    /* Pad 3 is named by a toolkit script whose only step is far in the
     * future: it presses nothing yet, and still holds its port. Set before
     * the first environment read, which caches. */
    setenv("RECOMP_DEBUG", "pad_script=f1000000:p3-a:1", 1);

    printf("usb_pad_source\n");

    /* No source: the model is what it was. */
    check(usb_gamepad_source() == NULL, "no source until one is installed");
    check(xbox_OhciNoDeviceForTest(5) == CC_NOERROR,
          "no source: a transfer nobody holds completes as before");

    usb_gamepad_set_source(&s_src);
    check(usb_gamepad_source() == &s_src, "the installed source is reported");
    check(xbox_OhciNoDeviceForTest(5) == CC_NOT_RESPONDING,
          "with a source: a transfer nobody holds is not answered");

    /* The report is the source's state, packed as the pad sends it. */
    memset(r, 0xAA, sizeof r);
    check(usb_gamepad_report(0, r, sizeof r) == 20, "report is 20 bytes");
    check(r[0] == 0 && r[1] == 20, "report header");
    check(r[2] == (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_LEFT) && r[3] == 0,
          "digital buttons from the source");
    check(r[4] == 0xFF && r[11] == 0x80, "analog buttons from the source");
    check(r[12] == 0x34 && r[13] == 0x12, "left stick X, little-endian");
    check(r[18] == 0xFE && r[19] == 0xFF, "right stick Y, negative");

    /* A pad the source does not drive is plugged and idle, not a host pad. */
    memset(r, 0xAA, sizeof r);
    usb_gamepad_report(1, r, sizeof r);
    {
        int i, zero = 1;
        for (i = 2; i < 20; i++)
            zero &= r[i] == 0;
        check(zero, "a pad the source does not drive reports nothing");
    }

    /* Present: the source's pads and the script's. */
    m = usb_gamepad_present();
    check(m == ((1u << 0) | (1u << 2)), "present = source pad 1 + script pad 3");

    /* Rumble reaches the source, not host pad n. */
    usb_gamepad_rumble(2, 0x1111, 0x2222);
    check(s_rumble_pad == 2 && s_rumble_l == 0x1111 && s_rumble_r == 0x2222,
          "rumble goes to the source");

    /* Detach forgets the address and the configuration. */
    enumerate(1, 9);
    check(usb_gamepad_address(1) == 9 && usb_gamepad_configured(1),
          "SET_ADDRESS and SET_CONFIGURATION stick");
    usb_gamepad_detach(1);
    check(usb_gamepad_address(1) == 0 && !usb_gamepad_configured(1),
          "detach returns the pad to the default address");

    /* The root hub. The first pad arrives on the first pass, on root port 3
     * (index 2): controller port 1 to XAPI. */
    m = xbox_OhciHotplugForTest(1, ports);
    check(m == 1u, "pad 1 plugs on the first pass");
    check((ports[2] & 0x00010001u) == 0x00010001u,
          "pad 1 on root port 3: connected, change latched");
    /* Pad 3 waits for pad 1 to be configured. */
    for (n = 0; n < 200; n++)
        xbox_OhciHotplugForTest(0, ports);
    check(xbox_OhciHotplugForTest(0, ports) == 1u,
          "the next pad waits while one is unconfigured");
    enumerate(0, 1);
    /* The driver enables pad 1's port and acknowledges the connect. */
    xbox_OhciWriteForTest(REG_PORT(2), PORT_SET_ENABLE | PORT_CSC);
    xbox_OhciWriteForTest(REG_INTR_STATUS, INTR_RHSC);
    check(xbox_OhciNoDeviceForTest(1) == 0xFFFFFFFFu,
          "pad 1 answers at its address");
    n = passes_until(1u | 4u, 200, ports);
    check(n > 1, "pad 3 arrives after the bus settles");
    check((ports[0] & 0x00000001u) != 0, "pad 3 on root port 1");
    check(usb_gamepad_address(2) == 0, "pad 3 starts at the default address");
    enumerate(2, 2);

    /* The source loses pad 1: it stays for a short gap, then leaves. */
    s_present = 0;
    for (n = 0; n < 10; n++)
        xbox_OhciHotplugForTest(0, ports);
    check(xbox_OhciHotplugForTest(0, ports) == (1u | 4u),
          "a brief gap does not unplug");
    n = passes_until(4u, 200, ports);
    check(n > 0, "a pad gone for longer unplugs");
    check((ports[2] & 0x00030003u) == 0x00030000u,
          "its port: disconnected, disabled, both changes latched");
    check(xbox_OhciWriteForTest(REG_INTR_STATUS, 0) & INTR_RHSC,
          "the unplug raises the root hub change");
    check(usb_gamepad_address(0) == 0 && !usb_gamepad_configured(0),
          "the removed pad forgets its address");
    check(xbox_OhciNoDeviceForTest(1) == CC_NOT_RESPONDING,
          "a transfer to its old address is not answered");
    check((ports[0] & 1u) != 0, "the script's pad stays");

    /* The driver acknowledges the changes. W1C must not take the RHSC a
     * concurrent latch would set, which a single thread cannot show; what
     * it can show is that the write clears exactly what it names. */
    m = xbox_OhciWriteForTest(REG_PORT(2), PORT_CSC | PORT_PESC);
    check((m & 0x00030003u) == 0, "the port's changes acknowledge");
    m = xbox_OhciWriteForTest(REG_INTR_STATUS, INTR_RHSC);
    check(!(m & INTR_RHSC), "the root hub change acknowledges");

    /* And it comes back, enumerating from scratch, but only after the bus
     * settles: plugging on the next pass would reconnect the port before
     * the driver had seen it disconnect. */
    s_present = 1;
    n = passes_until(1u | 4u, 200, ports);
    check(n >= PLUG_SETTLE, "the replug waits for the bus to settle");
    check((ports[2] & 0x00010001u) == 0x00010001u, "replugged on the same port");

    /* Pulled again before the driver enabled the port: a connect change,
     * and no enable change, since the enable never changed. */
    s_present = 0;
    n = passes_until(4u, 200, ports);
    check(n > 0 && (ports[2] & 0x00030003u) == 0x00010000u,
          "an unplug from a disabled port latches only CSC");

    usb_gamepad_set_source(NULL);
    printf("%s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
