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
#include "recomp_env.h"

#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <ctype.h>

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
 * the same syntax from a file, one entry per line or comma separated.
 *
 * Buttons: up down left right start back lthumb rthumb (the digital byte),
 * a b x y black white lt rt (the analog bytes, reported fully pressed), and
 * lleft lright lup ldown / rleft rright rup rdown (a stick at full throw).
 * Every entry is printed as it fires, so a run log says what was pressed when.
 *
 * RECOMP_PAD_LIVE=<file> is the same thing driven while the title runs: each
 * line appended to the file -- <button>[+<button>...][:<hold ms>], no time --
 * is pressed as soon as it is read. With RECOMP_FB_DUMP and a short
 * RECOMP_PB_REPORT_MS that makes a loop: look at the frame, append a press,
 * look again. A fixed timeline cannot do that on a rasterised run, where
 * how long a screen takes to appear depends on how much the software
 * rasteriser has to draw.
 *
 * Like RECOMP_PAD_PRESS this is a bring-up tool, off unless set. */
#define PAD_SCRIPT_MAX 256

typedef struct {
    unsigned long at_ms, hold_ms;
    uint8_t digital;         /* report byte 2 */
    uint8_t analog;          /* bit n = report byte 4 + n */
    uint8_t pad;             /* which controller: "p2-a" is pad 1 */
    uint8_t stick;           /* full deflection: 1 L-left 2 L-right 4 L-up
                                8 L-down, then the same four for the right */
    int announced;
} PadStep;

static PadStep s_script[PAD_SCRIPT_MAX];
static int     s_script_len = -1;

static int same_word(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return 1;
}

static int pad_button_bits(const char *name, size_t n, uint8_t *dig,
                           uint8_t *ana, uint8_t *stick)
{
    /* Stick directions, for screens and games that read the stick and
     * ignore the d-pad: Burnout 3 steers and picks map locations with it. */
    static const struct { const char *name; uint8_t bit; } sk[] = {
        {"lleft", 0x01}, {"lright", 0x02}, {"lup", 0x04}, {"ldown", 0x08},
        {"rleft", 0x10}, {"rright", 0x20}, {"rup", 0x40}, {"rdown", 0x80},
    };
    static const struct { const char *name; uint8_t dig, ana; } k[] = {
        {"up", 0x01, 0}, {"down", 0x02, 0}, {"left", 0x04, 0},
        {"right", 0x08, 0}, {"start", 0x10, 0}, {"back", 0x20, 0},
        {"lthumb", 0x40, 0}, {"rthumb", 0x80, 0},
        {"a", 0, 0x01}, {"b", 0, 0x02}, {"x", 0, 0x04}, {"y", 0, 0x08},
        {"black", 0, 0x10}, {"white", 0, 0x20}, {"lt", 0, 0x40}, {"rt", 0, 0x80},
    };
    size_t i;
    for (i = 0; i < sizeof sk / sizeof sk[0]; i++) {
        if (strlen(sk[i].name) == n && same_word(sk[i].name, name, n)) {
            *stick |= sk[i].bit;
            return 1;
        }
    }
    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        if (strlen(k[i].name) == n && same_word(k[i].name, name, n)) {
            *dig |= k[i].dig;
            *ana |= k[i].ana;
            return 1;
        }
    }
    return 0;
}

static void pad_script_parse(const char *text)
{
    const char *p = text;

    while (*p && s_script_len < PAD_SCRIPT_MAX) {
        PadStep st;
        char *end;
        const char *b;

        while (*p == ',' || *p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')
            p++;
        if (!*p)
            break;
        memset(&st, 0, sizeof st);
        st.hold_ms = 150;
        st.at_ms = strtoul(p, &end, 0);
        if (end == p || *end != ':') {
            fprintf(stderr, "  PAD: script entry without <ms>: at \"%.20s\"\n", p);
            break;
        }
        b = end + 1;
        /* "p2-" .. "p4-": the step is for that controller, not the first. */
        if ((b[0] == 'p' || b[0] == 'P') && b[1] >= '1' && b[1] <= '4'
            && b[2] == '-') {
            st.pad = (uint8_t)(b[1] - '1');
            b += 3;
        }
        for (;;) {
            size_t n = strcspn(b, "+:,\r\n ");
            if (!pad_button_bits(b, n, &st.digital, &st.analog, &st.stick))
                fprintf(stderr, "  PAD: unknown button \"%.*s\"\n", (int)n, b);
            b += n;
            if (*b != '+')
                break;
            b++;
        }
        if (*b == ':')
            st.hold_ms = strtoul(b + 1, (char **)&b, 0);
        s_script[s_script_len++] = st;
        p = b;
    }
}

static void pad_script_load(void)
{
    const char *spec = recomp_env(RENV_PAD_SCRIPT);

    s_script_len = 0;
    if (!spec || !*spec)
        return;
    if (spec[0] == '@') {
        FILE *f = fopen(spec + 1, "rb");
        static char buf[16384];
        size_t n;
        if (!f) {
            fprintf(stderr, "  PAD: cannot open script %s\n", spec + 1);
            return;
        }
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        pad_script_parse(buf);
    } else {
        pad_script_parse(spec);
    }
    fprintf(stderr, "  PAD: script of %d step(s)\n", s_script_len);
    fflush(stderr);
}

/* Drop steps that have finished, so a long live session never fills up. */
static void pad_script_compact(unsigned long t)
{
    int i, n = 0;
    for (i = 0; i < s_script_len; i++)
        if (t < s_script[i].at_ms + s_script[i].hold_ms)
            s_script[n++] = s_script[i];
    s_script_len = n;
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
                pending[npending] = 0;
                snprintf(line, sizeof line, "%lu:%s", at, pending);
                if (s_script_len >= PAD_SCRIPT_MAX - 1)
                    pad_script_compact(t);
                pad_script_parse(line);
                if (s_script_len > 0)
                    next_free[pd] = at + s_script[s_script_len - 1].hold_ms + 200;
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

static void pad_script_apply(int pad, uint8_t *out)
{
    static unsigned long t0;
    unsigned long now, t;
    int i, j;

    if (s_script_len < 0)
        pad_script_load();
    now = (unsigned long)GetTickCount();
    if (!t0)
        t0 = now;
    t = now - t0;
    pad_live_poll(t);
    if (s_script_len == 0)
        return;
    for (i = 0; i < s_script_len; i++) {
        PadStep *st = &s_script[i];
        if (st->pad != pad)
            continue;
        if (t < st->at_ms || t >= st->at_ms + st->hold_ms)
            continue;
        if (!st->announced) {
            st->announced = 1;
            fprintf(stderr, "  PAD: t=%lu ms step %d (digital 0x%02X analog 0x%02X)\n",
                    t, i, st->digital, st->analog);
            fflush(stderr);
        }
        out[2] |= st->digital;
        for (j = 0; j < 8; j++)
            if (st->analog & (1u << j))
                out[4 + j] = 0xFF;
        if (st->stick) {
            /* Axes at 12..19: LX LY RX RY, signed 16-bit, up positive. */
            static const int16_t full = 32767;
            int16_t ax[4];
            memcpy(ax, &out[12], sizeof ax);
            if (st->stick & 0x01) ax[0] = (int16_t)-full;
            if (st->stick & 0x02) ax[0] = full;
            if (st->stick & 0x04) ax[1] = full;
            if (st->stick & 0x08) ax[1] = (int16_t)-full;
            if (st->stick & 0x10) ax[2] = (int16_t)-full;
            if (st->stick & 0x20) ax[2] = full;
            if (st->stick & 0x40) ax[3] = full;
            if (st->stick & 0x80) ax[3] = (int16_t)-full;
            memcpy(&out[12], ax, sizeof ax);
        }
    }
}

int usb_gamepad_report(int pad, uint8_t *out, int max)
{
    XBOX_INPUT_STATE state;
    const XBOX_GAMEPAD *g;
    uint8_t synth = synthetic_buttons();
    int i;

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

    /* The synthetic press and the host pad drive pad n from host pad n. */
    if (pad != 0)
        synth = 0;
    if (xbox_InputGetState((DWORD)pad, &state) != 0) {
        out[2] = synth;
        pad_script_apply(pad, out);
        return 20;
    }

    g = &state.Gamepad;
    out[2] = (uint8_t)((g->wButtons & 0xFF) | synth);
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
    pad_script_apply(pad, out);
    return 20;
}
