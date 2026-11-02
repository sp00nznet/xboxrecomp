/*
 * usb_gamepad.c -- an Xbox controller, as the console's USB stack expects it.
 *
 * Descriptors and the input report, from the USB 2.0 specification for the
 * standard requests and from the device's own published interface class for
 * the rest. The gamepad is not a HID device: it reports interface class 0x58
 * subclass 0x42, which is Microsoft's own, and its report has a fixed layout
 * rather than one described by a HID report descriptor. That is why there is
 * no report descriptor here and why nothing asks for one.
 *
 * Input comes from the host through the existing xbox_input layer, so a real
 * pad plugged into the PC drives this one.
 */
#include "usb_gamepad.h"
#include "pad_script.h"
#include "recomp_env.h"
#include "recomp_exit_hook.h"   /* the recorder's held steps at exit */

#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <ctype.h>
#include <signal.h>
#ifdef _WIN32
#include <io.h>
#define pad_write _write
#else
#include <unistd.h>
#define pad_write write
#endif

/* ---- descriptors ------------------------------------------------------- */

static const uint8_t s_device_desc[18] = {
    18,             /* bLength                                    */
    0x01,           /* bDescriptorType: DEVICE                    */
    0x10, 0x01,     /* bcdUSB 1.10                                */
    0x00,           /* bDeviceClass: per interface                */
    0x00,           /* bDeviceSubClass                            */
    0x00,           /* bDeviceProtocol                            */
    0x08,           /* bMaxPacketSize0: 8                         */
    0x5E, 0x04,     /* idVendor  0x045E Microsoft                 */
    0x89, 0x02,     /* idProduct 0x0289 Controller S              */
    0x21, 0x01,     /* bcdDevice                                  */
    0x00,           /* iManufacturer: none                        */
    0x00,           /* iProduct: none                             */
    0x00,           /* iSerialNumber: none                        */
    0x01            /* bNumConfigurations                         */
};

/* Configuration, interface and both endpoints, in the one block a
 * GET_DESCRIPTOR(CONFIGURATION) returns. wTotalLength covers all of it. */
static const uint8_t s_config_desc[32] = {
    /* configuration */
    9, 0x02, 32, 0x00, 0x01, 0x01, 0x00, 0x80, 50,
    /* interface: class 0x58 subclass 0x42, the Xbox gamepad's own */
    9, 0x04, 0x00, 0x00, 0x02, 0x58, 0x42, 0x00, 0x00,
    /* endpoint 0x81 IN, interrupt, 32 bytes, 4 ms */
    7, 0x05, 0x81, 0x03, 0x20, 0x00, 0x04,
    /* endpoint 0x02 OUT, interrupt, 32 bytes, 4 ms -- rumble */
    7, 0x05, 0x02, 0x03, 0x20, 0x00, 0x04
};

static uint8_t s_address[USB_GAMEPAD_MAX];
static uint8_t s_configuration[USB_GAMEPAD_MAX];

uint8_t usb_gamepad_address(int pad) { return s_address[pad & 3]; }
int usb_gamepad_configured(int pad) { return s_configuration[pad & 3] != 0; }

void usb_gamepad_detach(int pad)
{
    s_address[pad & 3] = 0;
    s_configuration[pad & 3] = 0;
}

/* The game's input layer (usb_gamepad.h). Set once before the controller
 * thread starts and read on it, so a plain pointer is enough. */
static const UsbPadSource *s_source;

void usb_gamepad_set_source(const UsbPadSource *src) { s_source = src; }
const UsbPadSource *usb_gamepad_source(void) { return s_source; }

/* ---- control transfers ------------------------------------------------- */

#define REQ_GET_STATUS         0x00
#define REQ_CLEAR_FEATURE      0x01
#define REQ_SET_FEATURE        0x03
#define REQ_SET_ADDRESS        0x05
#define REQ_GET_DESCRIPTOR     0x06
#define REQ_GET_CONFIGURATION  0x08
#define REQ_SET_CONFIGURATION  0x09
#define REQ_SET_INTERFACE      0x0B

#define DESC_DEVICE            0x01
#define DESC_CONFIGURATION     0x02
#define DESC_STRING            0x03

static int copy_out(uint8_t *out, int max, const uint8_t *src, int len,
                    uint16_t wLength)
{
    /* A device sends the smaller of what was asked for and what it has. */
    if (len > (int)wLength) len = (int)wLength;
    if (len > max)          len = max;
    if (len > 0)            memcpy(out, src, (size_t)len);
    return len;
}

int usb_gamepad_control(int pad, const UsbSetup *setup, uint8_t *out, int max)
{
    int is_in = (setup->bmRequestType & 0x80) != 0;
    int type  = (setup->bmRequestType >> 5) & 3;   /* 0 standard, 1 class */

    if (type == 0) {
        switch (setup->bRequest) {
        case REQ_GET_DESCRIPTOR:
            switch (setup->wValue >> 8) {
            case DESC_DEVICE:
                return copy_out(out, max, s_device_desc,
                                (int)sizeof s_device_desc, setup->wLength);
            case DESC_CONFIGURATION:
                return copy_out(out, max, s_config_desc,
                                (int)sizeof s_config_desc, setup->wLength);
            case DESC_STRING:
                /* No string descriptors. Stalling is the correct answer and
                 * the one a host expects; returning an empty descriptor gets
                 * read as a malformed one. */
                return -1;
            default:
                return -1;
            }

        case REQ_SET_ADDRESS:
            s_address[pad & 3] = (uint8_t)(setup->wValue & 0x7F);
            return 0;                    /* zero-length status stage */

        case REQ_SET_CONFIGURATION:
            s_configuration[pad & 3] = (uint8_t)(setup->wValue & 0xFF);
            return 0;

        case REQ_GET_CONFIGURATION:
            if (!is_in || max < 1) return -1;
            out[0] = s_configuration[pad & 3];
            return 1;

        case REQ_GET_STATUS:
            /* Bus-powered, no remote wakeup. */
            if (!is_in || max < 2) return -1;
            out[0] = 0; out[1] = 0;
            return 2;

        case REQ_CLEAR_FEATURE:
        case REQ_SET_FEATURE:
        case REQ_SET_INTERFACE:
            return 0;

        default:
            return -1;
        }
    }

    /* Class requests on the interface -- GET_REPORT.
     *
     * XAPI reads the pad through the interrupt endpoint and also asks for
     * the same report over the control pipe. Stalling that is not a small
     * omission: a stalled control transfer reads to the driver as a broken
     * device, and because the stall also halts the endpoint, the pad stops
     * being polled for good.
     *
     * Measured on Shin Megami Tensei: Nine, the periodic list runs at about
     * a hundred descriptors a second and then collapses to nothing the
     * moment one `A1 01 value 0100 len 20` arrives.
     *
     * The answer is the report the interrupt endpoint would have sent.
     */
    if ((setup->bmRequestType & 0x60u) == 0x20u        /* class */
        && setup->bRequest == 0x01u                    /* GET_REPORT */
        && is_in) {
        uint8_t report[20];
        int n = usb_gamepad_report(pad, report, (int)sizeof report);
        if (n <= 0)
            return -1;
        return copy_out(out, max, report, n, setup->wLength);
    }

    /* Vendor requests on the interface -- the XID protocol.
     *
     * This is how XAPI tells a controller from any other USB device. The
     * standard descriptors say "interface class 0x58", which gets the device
     * enumerated and no further: XAPI then asks for the XID descriptor to
     * learn what kind of controller it is and how big its reports are, and a
     * stall there means "not a controller", so the device is enumerated,
     * configured, and then ignored. Which is exactly what it looked like --
     * a clean enumeration and a title that still saw no gamepad.
     */
    if ((setup->bmRequestType & 0x60u) == 0x40u) {   /* vendor */
        static const uint8_t xid_desc[16] = {
            0x10,           /* bLength                                  */
            0x42,           /* bDescriptorType: XID                     */
            0x00, 0x01,     /* bcdXid 1.00                              */
            0x01,           /* bType: gamepad                           */
            0x02,           /* bSubType: gamepad S                      */
            20,             /* bMaxInputReportSize                      */
            6,              /* bMaxOutputReportSize                     */
            0xFF, 0xFF, 0xFF, 0xFF,   /* wAlternateProductIds[0..1]     */
            0xFF, 0xFF, 0xFF, 0xFF    /* wAlternateProductIds[2..3]     */
        };
        /* Capabilities are the report with every supported field set to all
         * ones: the same layout, read as a mask. A gamepad S supports every
         * field of both, so both are filled in apart from the id and length
         * bytes, which are values rather than flags. */
        static const uint8_t caps_in[20] = {
            0x00, 20,
            0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
        };
        static const uint8_t caps_out[6] = {
            0x00, 6, 0xFF, 0xFF, 0xFF, 0xFF
        };

        if (!is_in)
            return 0;                     /* accept, nothing to send back */
        if (setup->bRequest == REQ_GET_DESCRIPTOR
                && (setup->wValue >> 8) == 0x42)
            return copy_out(out, max, xid_desc, (int)sizeof xid_desc,
                            setup->wLength);
        if (setup->bRequest == 0x01) {    /* GET_CAPABILITIES */
            if ((setup->wValue >> 8) == 0x01)
                return copy_out(out, max, caps_in, (int)sizeof caps_in,
                                setup->wLength);
            if ((setup->wValue >> 8) == 0x02)
                return copy_out(out, max, caps_out, (int)sizeof caps_out,
                                setup->wLength);
        }
        return -1;
    }

    /* Class requests: not ours to guess at. */
    return -1;
}

/* ---- the input report -------------------------------------------------- */

/* The host's own pad, through the layer that already maps one to XInput.
 * A real controller plugged into the PC drives this emulated one. */
#include "../input/xinput_xbox.h"
#include "../input/keyboard.h"   /* xbox_FramebufferKeyDown */

/*
 * The Xbox report is 20 bytes and fixed:
 *
 *   0      report id, always 0
 *   1      length, always 20
 *   2      digital buttons: dpad, start, back, thumb clicks
 *   3      reserved
 *   4..11  analog buttons A B X Y Black White, then the two triggers
 *   12..19 four signed 16-bit stick axes, little endian
 */
/* A synthetic press, for bringing a title up without a pad on the desk.
 *
 * RECOMP_PAD_PRESS=0x10 holds Start for a quarter of a second every two
 * seconds. A title sitting on a "press Start" screen needs an edge, not a
 * level, so this pulses rather than latching -- and it repeats because the
 * moment the title starts reading input is not knowable from here.
 *
 * Bit values are the Xbox digital button mask: 0x01/02/04/08 dpad
 * up/down/left/right, 0x10 Start, 0x20 Back, 0x40/0x80 thumb clicks.
 *
 * This is a bring-up probe and nothing else. It is off unless the variable
 * is set, and a real pad on the host is always the better input.
 */
static uint8_t synthetic_buttons(void)
{
    static int      configured = -1;
    static unsigned mask, period_ms, hold_ms;
    static unsigned long t0;
    unsigned long now, phase;

    if (configured < 0) {
        const char *spec = recomp_env(RENV_PAD_PRESS);
        configured = 0;
        if (spec && *spec) {
            char *end;
            mask = (unsigned)strtoul(spec, &end, 0) & 0xFFu;
            period_ms = (*end == ',') ? (unsigned)strtoul(end + 1, &end, 0)
                                      : 2000u;
            hold_ms   = (*end == ',') ? (unsigned)strtoul(end + 1, &end, 0)
                                      : 250u;
            if (!period_ms) period_ms = 2000u;
            if (!hold_ms || hold_ms >= period_ms) hold_ms = period_ms / 4u;
            if (mask) {
                configured = 1;
                t0 = (unsigned long)GetTickCount();
                fprintf(stderr, "  PAD: synthesising button mask 0x%02X for "
                        "%u ms every %u ms\n", mask, hold_ms, period_ms);
                fflush(stderr);
            }
        }
    }
    if (!configured)
        return 0;
    now = (unsigned long)GetTickCount();
    phase = (now - t0) % period_ms;
    return (uint8_t)((phase < hold_ms) ? mask : 0u);
}

/* A scripted pad, for driving a title through its menus with nobody at it.
 *
 * RECOMP_PAD_PRESS pulses one digital mask forever, which gets past a
 * "press Start" screen and no further: a menu wants A, which is an analog
 * button on this pad, and it wants presses in order. This takes a timeline:
 *
 *   RECOMP_PAD_SCRIPT=40000:start,44000:a,46000:down,47000:a:400
 *
 * Each entry is <ms>:<button>[+<button>...][:<hold ms>], with times measured
 * from the first report the title asks for (so from enumeration, not from
 * process start) and a default hold of 150 ms. RECOMP_PAD_SCRIPT=@file reads
 * the same syntax from a file, one entry per line or comma separated, "#"
 * comments allowed.
 *
 * f<flip>:<button>...[:<hold flips>] times a step by the executor's flip
 * count instead (pad_script.h), default hold 9 flips. A millisecond script
 * replays on wall time, so it lands on a different screen on a slower or
 * faster host; a flip-timed one lands on the same flip count, within about
 * 1-2 flips on a steady screen and more across loading screens.
 *
 * Buttons: up down left right start back lthumb rthumb (the digital byte),
 * a b x y black white lt rt (the analog bytes, reported fully pressed), and
 * lleft lright lup ldown / rleft rright rup rdown (a stick at full throw),
 * and lx=<v> ly rx ry (a stick axis at a value).
 * Every entry is printed as it fires, so a run log says what was pressed when.
 *
 * RECOMP_PAD_LIVE=<file> is the same thing driven while the title runs: each
 * line appended to the file -- <button>[+<button>...][:<hold ms>], no time --
 * is pressed as soon as it is read. With RECOMP_FB_DUMP and a short
 * RECOMP_PB_REPORT_MS that makes a loop: look at the frame, append a press,
 * look again. A fixed timeline cannot do that on a rasterised run, where
 * how long a screen takes to appear depends on how much the software
 * rasteriser has to draw. A live line may carry its own f<flip> time; it
 * is pressed at that flip, or at once if the flip has passed.
 *
 * RECOMP_DEBUG=pad_record=<file> writes what each pad reports to the title,
 * the script's presses and a person's alike, as flip-timed steps
 * (pad_recorder_feed): a route played once by hand becomes a script that
 * replays it. The sticks keep their values (lx=<v>); analog buttons are
 * recorded as pressed or not, as a script presses them. The path cannot
 * hold a comma: RECOMP_DEBUG is a comma list.
 *
 * A script or record may carry anchors, f<flip>:@open:<text> (pad_script.h):
 * the steps after one count their flips from the flip at which that file
 * opens in this run (usb_gamepad_file_open, from NtCreateFile), so a press
 * that waited on a movie or a loading screen lands on it. With
 * RECOMP_DEBUG=pad_record=<file>,pad_anchor=<text>|<text>, the record writes
 * one for each open whose path holds one of the texts.
 *
 * Like RECOMP_PAD_PRESS these are bring-up tools, off unless set. */
static PadStep s_script[PAD_SCRIPT_MAX];
static int     s_script_len = -1;
static PadAnchors s_anchors;
/* The pads the anchors' retries press: fixed once the script is read. */
static uint32_t s_anchor_pads;
/* Opens come on the game's threads, any of them at once (a loader thread
 * and the main one), and the pad thread ticks the give-up and records: an
 * anchor matched twice, or lost, would hang a replay. Taken only with a
 * pad script or record set (usb_gamepad_file_open's `on`). */
static SRWLOCK s_anchor_lock = SRWLOCK_INIT;
/* 0 not read, 1 being read, 2 read. The script is read on the first pad
 * poll or the first file open, whichever comes first, and those are
 * different threads. */
static int s_script_state;
/* Bumped by every compaction: the steps below s_script_len have moved, so
 * script_pads must look at all of them again (a live poll may compact and
 * append in one go, leaving the length past where it last looked). */
static unsigned s_script_gen;

/* The executor's flip count, for flip-timed steps and the recorder. Written
 * by the walker's thread, read on the pad's: a stale read is a flip late. */
static volatile uint32_t s_flip;

void usb_gamepad_flip(uint32_t flip)
{
    s_flip = flip;
}

uint32_t usb_gamepad_flips(void) { return s_flip; }

/* The title's port-0 polls so far: the poll clock (g<poll> steps,
 * pad_script.h). Only the pad's thread moves it, after a poll's record, so
 * a poll's press and its record read the same count; another thread (a file
 * open) reads the next poll's. */
static volatile uint32_t s_polls;
/* The script's clock is the poll's (its steps are g<poll>), and the
 * record's (RECOMP_DEBUG=pad_clock=poll). Each is read once. */
static int s_script_poll;
static int rec_poll(void)
{
    static int v = -1;
    if (v < 0) {
        const char *c = recomp_env(RENV_PAD_CLOCK);
        v = c && !strcmp(c, "poll");
    }
    return v;
}
static uint32_t script_clock(void) { return s_script_poll ? s_polls : s_flip; }
static uint32_t rec_clock(void) { return rec_poll() ? s_polls : s_flip; }

static void pad_script_read(void)
{
    const char *spec = recomp_env(RENV_PAD_SCRIPT);

    s_script_len = 0;
    if (!spec || !*spec)
        return;
    if (spec[0] == '@') {
        FILE *f = fopen(spec + 1, "rb");
        char *buf;
        long n;
        if (!f) {
            fprintf(stderr, "  PAD: cannot open script %s\n", spec + 1);
            return;
        }
        /* A recorded route runs to tens of kilobytes: read all of it. */
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = n >= 0 ? (char *)malloc((size_t)n + 1) : NULL;
        if (!buf) {
            fclose(f);
            fprintf(stderr, "  PAD: cannot read script %s\n", spec + 1);
            return;
        }
        n = (long)fread(buf, 1, (size_t)n, f);
        fclose(f);
        buf[n] = 0;
        s_script_len = pad_script_parse_anchored(buf, s_script, 0, PAD_SCRIPT_MAX,
                                                 &s_anchors);
        free(buf);
    } else {
        s_script_len = pad_script_parse_anchored(spec, s_script, 0, PAD_SCRIPT_MAX,
                                                 &s_anchors);
    }
    s_script_poll = s_anchors.clock == 2;
    {
        int i;
        for (i = 0; i < s_anchors.n; i++)
            if (s_anchors.a[i].period)
                s_anchor_pads |= 1u << (s_anchors.a[i].retry.pad & 3);
    }
    if (s_anchors.n)
        fprintf(stderr, "  PAD: script of %d step(s), %d anchor(s)\n",
                s_script_len, s_anchors.n);
    else
        fprintf(stderr, "  PAD: script of %d step(s)\n", s_script_len);
    fflush(stderr);
}

static void pad_script_load(void)
{
    int idle = 0;

    if (__atomic_load_n(&s_script_state, __ATOMIC_ACQUIRE) == 2)
        return;
    if (__atomic_compare_exchange_n(&s_script_state, &idle, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        pad_script_read();
        __atomic_store_n(&s_script_state, 2, __ATOMIC_RELEASE);
        return;
    }
    /* The other thread is reading the file: a few milliseconds, once. */
    while (__atomic_load_n(&s_script_state, __ATOMIC_ACQUIRE) != 2)
        SwitchToThread();
}

/* Drop steps that have finished, so a long live session never fills up. */
static void pad_script_compact(unsigned long t)
{
    int i, n = 0;
    for (i = 0; i < s_script_len; i++)
        if (!pad_step_done_at(&s_script[i], &s_anchors, t, script_clock()))
            s_script[n++] = s_script[i];
    s_script_len = n;
    s_script_gen++;
}

/* Read whatever has been appended to RECOMP_PAD_LIVE since last time, and
 * schedule each complete line at t. Polled at most every 50 ms. */
static void pad_live_poll(unsigned long t)
{
    static const char *path;
    static int checked;
    static long consumed;
    static unsigned long last_poll;
    static char pending[512];
    static size_t npending;
    static unsigned long next_free[USB_GAMEPAD_MAX];
    FILE *f;
    int c;

    if (!checked) {
        checked = 1;
        path = recomp_env(RENV_PAD_LIVE);
        if (path && *path) {
            fprintf(stderr, "  PAD: live input from %s\n", path);
            fflush(stderr);
        }
    }
    if (!path || !*path || (t - last_poll < 50 && last_poll))
        return;
    last_poll = t;
    f = fopen(path, "rb");
    if (!f)
        return;
    if (fseek(f, consumed, SEEK_SET) != 0) {
        fclose(f);
        return;
    }
    while ((c = fgetc(f)) != EOF) {
        consumed++;
        if (c == '\n' || c == '\r') {
            if (npending) {
                char line[560];
                /* Several lines in one read go one after another, with a
                 * 200 ms release between: a title polling its pad a few times
                 * a second would otherwise see "down" and "a" in the same
                 * report, which is a different input from "down, then a". */
                /* Serialised per pad: pad 2's throttle must not wait out
                 * pad 1's -- split-screen players press at the same time. */
                int pd = (pending[0] == 'p' || pending[0] == 'P')
                         && pending[1] >= '1' && pending[1] <= '4'
                         && pending[2] == '-' ? pending[1] - '1' : 0;
                unsigned long at = t > next_free[pd] ? t : next_free[pd];
                /* "f<flip>:..." or "g<poll>:..." carries its own time: the
                 * line is a step of a timed script, not a press for now. */
                int timed = (pending[0] == 'f' || pending[0] == 'F') ? 1
                            : (pending[0] == 'g' || pending[0] == 'G') ? 2 : 0;
                if (timed && !isdigit((unsigned char)pending[1]))
                    timed = 0;
                pending[npending] = 0;
                /* One clock a script, as in the file: the first timed entry
                 * sets it when the script had none. */
                if (timed && !s_anchors.clock) {
                    s_anchors.clock = (uint8_t)timed;
                    s_script_poll = timed == 2;
                } else if (timed && s_anchors.clock != timed) {
                    fprintf(stderr, "  PAD: live \"%s\": the script is on the %s"
                            " clock; skipped\n", pending,
                            s_anchors.clock == 2 ? "poll" : "flip");
                    fflush(stderr);
                    npending = 0;
                    continue;
                }
                if (timed)
                    snprintf(line, sizeof line, "%s", pending);
                else
                    snprintf(line, sizeof line, "%lu:%s", at, pending);
                if (s_script_len >= PAD_SCRIPT_MAX - 1)
                    pad_script_compact(t);
                {
                    int had = s_script_len;
                    /* No anchors here: one added under the open's thread
                     * would race it, and a live line is pressed now, not on
                     * a recorded event. The parser says it skipped one. */
                    s_script_len = pad_script_parse(line, s_script, s_script_len,
                                                    PAD_SCRIPT_MAX);
                    if (s_script_len > had)
                        pad_step_not_before(&s_script[s_script_len - 1],
                                            script_clock());
                }
                if (s_script_len > 0 && !timed)
                    next_free[pd] = at + s_script[s_script_len - 1].hold + 200;
                if (timed)
                    fprintf(stderr, "  PAD: live \"%s\" (%s %u)\n", pending,
                            timed == 2 ? "poll" : "flip", (unsigned)script_clock());
                else
                    fprintf(stderr, "  PAD: live \"%s\" at t=%lu ms\n", pending, at);
                fflush(stderr);
                npending = 0;
            }
        } else if (npending < sizeof pending - 1) {
            pending[npending++] = (char)c;
        }
    }
    fclose(f);
}

/* RECOMP_DEBUG=pad_quit: a replay ends where its record ended. A record
 * ends with an @end step at the count it stopped on, so once every step of
 * the script has ended, that one included, the run is over (a record from
 * before @end ends with its last release). Only for a script from the
 * start (RECOMP_PAD_SCRIPT); live lines added meanwhile must end too.
 *
 * This runs on the title's pad thread, inside its XInputGetState and under
 * the game's pad lock (BLiNX 2: pad_input.c's g_lock). Exiting here would
 * run the exit hooks and atexit work (SDL input, the mods) on that thread
 * with the lock held. The quit is posted to the window instead, which
 * closes as its close button does, on its own thread. Without a window (a
 * headless run) a thread of its own ends the run, off the pad lock. */
static DWORD WINAPI pad_quit_thread(LPVOID why)
{
    recomp_exit_hook_run((const char *)why);
    fflush(stdout);
    fflush(stderr);
    /* exit() on POSIX, so a mod's trace buffered for atexit is written. */
    ExitProcess(0);
    return 0;
}

static void pad_script_quit(unsigned long t, uint32_t clock)
{
    static int on = -1, from;
    int i, k;

    if (on < 0)
        on = recomp_env_on(RENV_PAD_QUIT) && s_script_len > 0;
    if (!on)
        return;
    /* A step once ended stays ended, so the scan goes on from the last step
     * still running: one check a poll while it runs, not the whole script. */
    for (k = 0; k < s_script_len; k++) {
        i = (from + k) % s_script_len;
        if (!pad_step_done_at(&s_script[i], &s_anchors, t, clock)) {
            from = i;
            return;
        }
    }
    on = 0;
    fprintf(stderr, "  PAD: the script's last step has ended (%s %u): quitting"
            " (pad_quit)\n", s_script_poll ? "poll" : "flip", (unsigned)clock);
    fflush(stderr);
    /* A post the window refused (a full SDL queue, a window already gone)
     * would lose the quit, which is asked only once: the quit thread ends
     * the run then, as without a window. */
    if (recomp_quit_post("pad_quit"))
        return;
    if (!CreateThread(NULL, 0, pad_quit_thread, (LPVOID)"pad_quit", 0, NULL))
        pad_quit_thread((LPVOID)"pad_quit");   /* no thread: here, as before */
}

static void pad_script_apply(int pad, uint8_t *out, uint32_t flip)
{
    static unsigned long t0;
    static int warned;
    unsigned long now, t;
    int i;

    pad_script_load();
    now = (unsigned long)GetTickCount();
    if (!t0)
        t0 = now;
    t = now - t0;
    pad_live_poll(t);
    /* Only the LLE pushbuffer walker counts flips (usb_gamepad_flip); a
     * title on the HLE D3D8 present path would get every f-step at boot.
     * g<poll> steps count the title's polls and need no flips. */
    if (!warned && !s_flip && t > 20000) {
        for (i = 0; i < s_script_len; i++)
            if (s_script[i].by_flip == 1)
                break;
        if (i < s_script_len) {
            fprintf(stderr, "  PAD: no flips counted after 20 s: f<flip> steps"
                    " need the pushbuffer walker's flip count\n");
            fflush(stderr);
        }
        warned = 1;
    }
    for (i = 0; i < s_script_len; i++) {
        PadStep *st = &s_script[i];
        if (st->pad != pad || !pad_step_active_at(st, &s_anchors, t, flip))
            continue;
        if (!st->announced) {
            st->announced = 1;
            fprintf(stderr, "  PAD: t=%lu ms %s %u step %d (digital 0x%02X analog 0x%02X)\n",
                    t, s_script_poll ? "poll" : "flip", (unsigned)flip, i, st->digital, st->analog);
            fflush(stderr);
        }
        pad_step_press(st, out);
    }
    if (s_anchors.n) {
        PadStep retry;
        int on;
        char log[512], rlog[160];
        AcquireSRWLockExclusive(&s_anchor_lock);
        pad_anchors_tick(&s_anchors, s_script, s_script_len, flip, log, sizeof log);
        on = pad_anchor_retry(&s_anchors, pad, flip, &retry, rlog, sizeof rlog);
        ReleaseSRWLockExclusive(&s_anchor_lock);
        if (log[0] || rlog[0]) {
            fputs(log, stderr);
            fputs(rlog, stderr);
            fflush(stderr);
        }
        if (on)
            pad_step_press(&retry, out);
    }
    if (pad == 0)
        pad_script_quit(t, flip);
}

/* The pads the toolkit's own inputs drive: the script's (every pad a step
 * has named, kept after the step ends, so a pad does not leave the bus
 * between two presses), the live file's pad 1, the synthetic press's pad 1.
 * With a source installed these stay plugged in whatever the source says,
 * so a run driven by RECOMP_PAD_SCRIPT alone gets the pad it gets today.
 *
 * Unlocked, like s_script itself. The USB model's calls come from the OHCI
 * model's thread; a game's own XInputGetState (usb_gamepad_game_*) calls it
 * under that game's pad lock (BLiNX 2: pad_input.c's g_lock). The two never
 * meet in BLiNX 2 because its XAPI never starts the OHCI (its logs have no
 * "[OHCI0] operational"). A title that answers XInputGetState itself and
 * also starts the OHCI would need a lock here. `named` only gains bits, so
 * a torn read costs a rescan at most. */
static uint32_t script_pads(void)
{
    static uint32_t named;
    static int scanned;
    static unsigned gen;
    int i;

    pad_script_load();
    /* A game's own poll asks once per port per frame, and a recorded route
     * holds thousands of steps: look only at the ones added since (from the
     * start again after a live session's compaction). */
    if (gen != s_script_gen || s_script_len < scanned) {
        gen = s_script_gen;
        scanned = 0;
    }
    for (i = scanned; i < s_script_len; i++)
        named |= 1u << (s_script[i].pad & 3);
    scanned = s_script_len;
    named |= s_anchor_pads;
    {
        const char *live = recomp_env(RENV_PAD_LIVE);
        if (live && *live)
            named |= 1u;
    }
    {
        const char *press = recomp_env(RENV_PAD_PRESS);
        if (press && *press && strtoul(press, NULL, 0) & 0xFFu)
            named |= 1u;
    }
    return named;
}

uint32_t usb_gamepad_present(void)
{
    uint32_t m = script_pads();
    if (s_source && s_source->present)
        m |= s_source->present();
    return m & ((1u << USB_GAMEPAD_MAX) - 1u);
}

void usb_gamepad_rumble(int pad, uint16_t left, uint16_t right)
{
    if (s_source && s_source->rumble) {
        s_source->rumble(pad, left, right);
    } else {
        XBOX_VIBRATION v;
        v.wLeftMotorSpeed  = (WORD)left;
        v.wRightMotorSpeed = (WORD)right;
        xbox_InputSetState((DWORD)pad, &v);
    }
}

/* The 20-byte report's bytes 2..19 from a pad state, and back: the script
 * and the recorder work on reports, a game that answers XInputGetState
 * itself has states. */
static void report_from_gamepad(const XBOX_GAMEPAD *g, uint8_t *out)
{
    int i;

    memset(out, 0, 20);
    out[1] = 20;
    out[2] = (uint8_t)(g->wButtons & 0xFF);
    out[3] = (uint8_t)((g->wButtons >> 8) & 0xFF);
    for (i = 0; i < 8; i++)
        out[4 + i] = g->bAnalogButtons[i];
    out[12] = (uint8_t)(g->sThumbLX & 0xFF);
    out[13] = (uint8_t)((g->sThumbLX >> 8) & 0xFF);
    out[14] = (uint8_t)(g->sThumbLY & 0xFF);
    out[15] = (uint8_t)((g->sThumbLY >> 8) & 0xFF);
    out[16] = (uint8_t)(g->sThumbRX & 0xFF);
    out[17] = (uint8_t)((g->sThumbRX >> 8) & 0xFF);
    out[18] = (uint8_t)(g->sThumbRY & 0xFF);
    out[19] = (uint8_t)((g->sThumbRY >> 8) & 0xFF);
}

static void gamepad_from_report(const uint8_t *r, XBOX_GAMEPAD *g)
{
    int i;

    g->wButtons = (WORD)(r[2] | (r[3] << 8));
    for (i = 0; i < 8; i++)
        g->bAnalogButtons[i] = r[4 + i];
    g->sThumbLX = (SHORT)(uint16_t)(r[12] | (r[13] << 8));
    g->sThumbLY = (SHORT)(uint16_t)(r[14] | (r[15] << 8));
    g->sThumbRX = (SHORT)(uint16_t)(r[16] | (r[17] << 8));
    g->sThumbRY = (SHORT)(uint16_t)(r[18] | (r[19] << 8));
}

/* RECOMP_DEBUG=pad_record=<file>: the report as the title gets it, after the
 * script, as steps timed by flip or by poll (pad_clock). Each step is written
 * when it ends and flushed; the steps still held and the @end line are
 * written at exit (atexit, or the window's exit hook, since ExitProcess
 * skips atexit). */
static FILE *s_rec_file;
static PadRecorder s_rec[USB_GAMEPAD_MAX];

static void pad_record_end(void)
{
    char lines[512], end[32];
    FILE *f;
    int i, w;

    /* Under the lock the pad thread feeds the recorders under, and only
     * once: atexit and the exit hook both come here, and the hook may run
     * on another thread (pad_quit's, the window's) while the title still
     * polls. A pad poll after the end would start its recorder again, and
     * the second pass would write a step and a second, later @end. With
     * s_rec_file gone pad_record writes nothing more.
     *
     * The unlocked check first: once the record has ended, a second pass
     * must not take the lock. A signal handler chained after ours may call
     * exit() on a thread that holds it (inside pad_record), and the rwlock
     * is not recursive, so atexit would deadlock there. */
    if (!__atomic_load_n(&s_rec_file, __ATOMIC_ACQUIRE))
        return;
    AcquireSRWLockExclusive(&s_anchor_lock);
    f = __atomic_load_n(&s_rec_file, __ATOMIC_ACQUIRE);
    if (f) {
        /* Before pad_recorder_end, which forgets the pads' last reports. */
        w = pad_recorder_end_line(s_rec, USB_GAMEPAD_MAX, rec_clock(), end,
                                  sizeof end);
        for (i = 0; i < USB_GAMEPAD_MAX; i++)
            if (pad_recorder_end(&s_rec[i], rec_clock(), lines, sizeof lines))
                fputs(lines, f);
        if (w > 0)
            fputs(end, f);
        fflush(f);
        __atomic_store_n(&s_rec_file, NULL, __ATOMIC_RELEASE);
    }
    ReleaseSRWLockExclusive(&s_anchor_lock);
}

static void pad_record_exit_hook(const char *why)
{
    (void)why;
    pad_record_end();
}

/* Benches and golden runs stop the game with SIGINT (or SIGTERM), which
 * never reaches atexit, and a recording that ends on a held stick would lose
 * that last step. The handler writes the held steps with write(2), not
 * stdio: every fputs to the record is flushed at once, so its buffer is
 * empty unless the signal lands mid-fputs, and write cannot deadlock on the
 * FILE's lock. The lines are formatted without stdio too (pad_script.h).
 * It takes no lock, which a signal cannot; a signal landing inside
 * pad_record_end may write the end twice, and nothing worse. Then the
 * handler that was there before runs (the [MEM] exit line's, or the
 * default), so the stop still means what it did and the bench keeps its
 * memory line. Only installed while recording. */
typedef void (*pad_sig_fn)(int);
static pad_sig_fn s_rec_old_int = SIG_DFL, s_rec_old_term = SIG_DFL;

static void pad_record_signal(int sig)
{
    pad_sig_fn old = sig == SIGINT ? s_rec_old_int : s_rec_old_term;

    char lines[512], end[32];
    FILE *f = __atomic_exchange_n(&s_rec_file, NULL, __ATOMIC_ACQ_REL);
    int i, fd, w;

    if (f) {
        fd = fileno(f);
        w = pad_recorder_end_line(s_rec, USB_GAMEPAD_MAX, rec_clock(), end, sizeof end);
        for (i = 0; i < USB_GAMEPAD_MAX; i++)
            if (pad_recorder_end(&s_rec[i], rec_clock(), lines, sizeof lines))
                (void)!pad_write(fd, lines, (unsigned)strlen(lines));
        if (w > 0)
            (void)!pad_write(fd, end, (unsigned)w);
    }
    signal(sig, old == SIG_ERR ? SIG_DFL : old);
    raise(sig);
}

/* The record's last anchor: the next one's retry looks only at presses
 * after it, and a file that opens again straight after is not a second
 * anchor (a movie's or a save's few opens of one file). */
static uint32_t s_rec_anchor_at;
static char s_rec_anchor_name[PAD_ANCHOR_TEXT];

/* RECOMP_DEBUG=pad_anchor=<text>|<text>: an open whose path holds one of
 * them, in any case. */
static int anchor_wanted(const char *path)
{
    const char *list = recomp_env(RENV_PAD_ANCHOR), *p;

    if (!list || !*list)
        return 0;
    for (p = list; *p; ) {
        size_t n = strcspn(p, "|"), i, j;
        for (i = 0; n && path[i]; i++) {
            for (j = 0; j < n && path[i + j]; j++)
                if (tolower((unsigned char)path[i + j]) != tolower((unsigned char)p[j]))
                    break;
            if (j == n)
                return 1;
        }
        p += n;
        if (*p == '|')
            p++;
    }
    return 0;
}

/* An anchor line for an open, written on the thread that opened the file,
 * under s_anchor_lock: the pad thread feeds the recorders under it too, so
 * the presses the retry is read from are whole and the anchor's line goes
 * in order with the steps around it. */
/* Under s_anchor_lock: a refusal goes to log[0..logn), printed after it. */
static void pad_record_open(const char *path, char *log, size_t logn)
{
    FILE *f = __atomic_load_n(&s_rec_file, __ATOMIC_ACQUIRE);
    char line[512];
    uint32_t at;
    const char *name;
    int w;

    if (!f || !anchor_wanted(path))
        return;
    name = pad_path_name(path);
    if (s_rec_anchor_name[0] && !strcmp(name, s_rec_anchor_name))
        return;
    w = pad_recorder_anchor(s_rec, USB_GAMEPAD_MAX, path, rec_clock(),
                            s_rec_anchor_at, line, sizeof line, &at);
    if (w <= 0) {
        size_t used = strlen(log);
        if (used + 1 < logn)
            snprintf(log + used, logn - used,
                     w ? "  PAD: %s: its anchor line is too long; not recorded\n"
                       : "  PAD: %s cannot be an anchor (its name)\n", path);
        return;
    }
    s_rec_anchor_at = at;
    snprintf(s_rec_anchor_name, sizeof s_rec_anchor_name, "%s", name);
    fputs(line, f);
    fflush(f);
}

static void pad_record(int pad, const uint8_t *out, uint32_t flip)
{
    static int state = -1;
    FILE *f;
    char lines[512];

    if (state < 0) {
        const char *path = recomp_env(RENV_PAD_RECORD);
        int i;
        state = 0;
        if (path && *path) {
            f = fopen(path, "w");
            if (!f) {
                fprintf(stderr, "  PAD: cannot write the pad record %s\n", path);
            } else {
                state = 1;
                __atomic_store_n(&s_rec_file, f, __ATOMIC_RELEASE);
                atexit(pad_record_end);
                recomp_exit_hook_add(pad_record_exit_hook);
                s_rec_old_int = signal(SIGINT, pad_record_signal);
                s_rec_old_term = signal(SIGTERM, pad_record_signal);
                if (rec_poll())
                    fprintf(f, "# pad_record: pad_script steps timed by the title's"
                            " pad polls (g<poll>:<buttons>:<hold polls>, sticks"
                            " lx=<v>, analog depth lt=<v>)\n");
                else
                    fprintf(f, "# pad_record: pad_script steps timed by flip"
                            " (f<flip>:<buttons>:<hold flips>, sticks lx=<v>)\n");
                fflush(f);
                fprintf(stderr, "  PAD: recording the pads to %s\n", path);
            }
            fflush(stderr);
        }
        for (i = 0; i < USB_GAMEPAD_MAX; i++) {
            s_rec[i].pad = i;
            s_rec[i].poll = rec_poll();
        }
    }
    if (!state || pad < 0 || pad >= USB_GAMEPAD_MAX)
        return;
    if (!__atomic_load_n(&s_rec_file, __ATOMIC_ACQUIRE))   /* the end is written */
        return;
    /* One uncontended lock per pad poll, only while recording. The file is
     * read again under it: pad_record_end may have ended the record since. */
    AcquireSRWLockExclusive(&s_anchor_lock);
    f = __atomic_load_n(&s_rec_file, __ATOMIC_ACQUIRE);
    if (f && pad_recorder_feed(&s_rec[pad], pad_bits_from_report(out), flip,
                               lines, sizeof lines)) {
        fputs(lines, f);
        fflush(f);
    }
    ReleaseSRWLockExclusive(&s_anchor_lock);
}

/* The clock a poll's press reads: the poll's count or the flip read
 * with it. */
static uint32_t script_at(uint32_t flip, uint32_t polls)
{
    pad_script_load();   /* s_script_poll comes with the script */
    return s_script_poll ? polls : flip;
}

/* A port-0 poll is over: the poll clock moves on. */
static void poll_done(int pad)
{
    if (pad == 0)
        s_polls = s_polls + 1;
}

int usb_gamepad_report(int pad, uint8_t *out, int max)
{
    XBOX_INPUT_STATE state;
    const XBOX_GAMEPAD *g;
    uint8_t synth = synthetic_buttons();
    /* One flip for the script and the recorder: the walker may count one
     * between two reads, and the record would then lag the press by a flip. */
    uint32_t flip = s_flip, polls = s_polls;

    if (max < 20)
        return 0;
    memset(out, 0, 20);
    out[0] = 0;
    out[1] = 20;

    /* A disconnected host pad is not an error here: the device is present on
     * the bus either way, it just reports nothing pressed. */
    /* RECOMP_INPUT_DIAG: the whole chain on one line, once a second.
     *
     * "Nothing happens when I press a key" has several candidate causes and
     * guessing between them costs a rebuild each: the variable not read,
     * XInput claiming a pad so the keyboard fallback never runs, the window
     * not receiving the key, or the report going out without it. Printing
     * all four together answers it in one run.
     *
     * It samples the held state, so pair it with the window's own
     * RECOMP_KEY_TRACE: that answers "did the key arrive", this answers
     * "is the chain wired". */
    {
        static int diag = -1;
        if (diag < 0)
            diag = recomp_env(RENV_INPUT_DIAG) != NULL;
        if (diag) {
            static unsigned long last;
            unsigned long now = (unsigned long)GetTickCount();
            if (now - last > 1000) {
                XBOX_INPUT_STATE probe;
                DWORD rc = xbox_InputGetState(0, &probe);
                last = now;
                fprintf(stderr, "  [INPUT] keyboard=%d window_has_RETURN=%d "
                        "InputGetState=%lu buttons=0x%04X\n",
                        xbox_InputKeyboardOn(),
                        xbox_FramebufferKeyDown(0x0D),
                        (unsigned long)rc,
                        rc == 0 ? probe.Gamepad.wButtons : 0);
                fflush(stderr);
            }
        }
    }

    /* The synthetic press and the host pad drive pad n from host pad n;
     * with a game's source installed the source drives it instead. */
    if (pad != 0)
        synth = 0;
    if (s_source) {
        memset(&state, 0, sizeof state);
        if (!s_source->get_state || !s_source->get_state(pad, &state.Gamepad))
            memset(&state, 0, sizeof state);
    } else if (xbox_InputGetState((DWORD)pad, &state) != 0) {
        out[2] = synth;
        pad_script_apply(pad, out, script_at(flip, polls));
        pad_record(pad, out, rec_poll() ? polls : flip);
        poll_done(pad);
        return 20;
    }

    g = &state.Gamepad;
    report_from_gamepad(g, out);
    out[2] |= synth;
    pad_script_apply(pad, out, script_at(flip, polls));
    pad_record(pad, out, rec_poll() ? polls : flip);
    poll_done(pad);
    return 20;
}

/* ---- a game's own XInputGetState --------------------------------------- */

/* The flip each pad's press was applied under, for the record that follows
 * it on the same poll: the walker may count a flip between the two calls,
 * and a release recorded a flip late lengthens the hold (f10:a:5 would
 * read back f10:a:6). */
static uint32_t s_game_flip[USB_GAMEPAD_MAX];
static uint8_t  s_game_pressed[USB_GAMEPAD_MAX];

void usb_gamepad_game_press(int pad, struct XBOX_GAMEPAD *g)
{
    uint8_t r[20];
    uint32_t flip = s_flip;

    if (pad < 0 || pad >= USB_GAMEPAD_MAX)
        return;
    s_game_flip[pad] = flip;
    s_game_pressed[pad] = 1;
    report_from_gamepad(g, r);
    if (pad == 0)
        r[2] |= synthetic_buttons();
    pad_script_apply(pad, r, script_at(flip, s_polls));
    gamepad_from_report(r, g);
}

void usb_gamepad_game_record(int pad, const struct XBOX_GAMEPAD *g)
{
    uint8_t r[20];
    uint32_t flip;

    if (pad < 0 || pad >= USB_GAMEPAD_MAX)
        return;
    /* The press's flip when this poll had one, else this one's own. The
     * poll clock moves only below, so the press read this poll's count. */
    flip = s_game_pressed[pad] ? s_game_flip[pad] : s_flip;
    s_game_pressed[pad] = 0;
    report_from_gamepad(g, r);
    pad_record(pad, r, rec_poll() ? s_polls : flip);
    poll_done(pad);
}

uint32_t usb_gamepad_game_pads(void)
{
    return script_pads() & ((1u << USB_GAMEPAD_MAX) - 1u);
}

/* ---- file opens, for anchors -------------------------------------------- */

void usb_gamepad_file_open(const char *path, uint32_t status)
{
    /* Every thread works out the same answer: a race on it is harmless.
     * Without a script or a record this is the one load per open. */
    static int on = -1;
    int o = __atomic_load_n(&on, __ATOMIC_RELAXED);
    char log[512];

    if (o < 0) {
        o = (recomp_env(RENV_PAD_SCRIPT) && *recomp_env(RENV_PAD_SCRIPT))
            || (recomp_env(RENV_PAD_RECORD) && *recomp_env(RENV_PAD_RECORD));
        __atomic_store_n(&on, o, __ATOMIC_RELAXED);
    }
    if (!o || status || !path)
        return;
    pad_script_load();
    AcquireSRWLockExclusive(&s_anchor_lock);
    pad_anchors_open(&s_anchors, path, script_clock(), log, sizeof log);
    pad_record_open(path, log, sizeof log);
    ReleaseSRWLockExclusive(&s_anchor_lock);
    /* Printed unlocked: a slow stderr must not hold the pad threads. */
    if (log[0]) {
        fputs(log, stderr);
        fflush(stderr);
    }
}
