/*
 * usb_gamepad.h -- the device on the other end of the wire.
 *
 * A host controller moves bytes; it does not know what they mean. This is the
 * thing that answers them: an Xbox controller, as the console's own USB stack
 * expects to find it -- standard descriptors over endpoint 0, and a 20-byte
 * report over interrupt endpoint 1.
 *
 * Kept apart from ohci.c because it is a different concern. The controller
 * walks descriptor lists and raises interrupts and would do the same for a
 * memory unit or a headset; this knows one device and nothing about lists.
 */
#ifndef XBOX_USB_GAMEPAD_H
#define XBOX_USB_GAMEPAD_H

#include <stdint.h>

/* USB setup packet, as it arrives in a SETUP transfer's buffer. */
typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} UsbSetup;

/* Answer a control request.
 *
 * Returns the number of bytes written to `out` (at most `max`), or -1 if the
 * request is not one this device answers -- which the caller reports as a
 * stall rather than as a short transfer, because those mean different things
 * to a driver.
 */
int usb_gamepad_control(int pad, const UsbSetup *setup, uint8_t *out, int max);

/* Fill in pad `pad`'s 20-byte input report. Returns the byte count written. */
int usb_gamepad_report(int pad, uint8_t *out, int max);

/* The executor's flip count, for flip-timed pad scripts and the pad
 * recorder (pad_script.h). Called on each NV097_FLIP_INCREMENT_WRITE. */
void usb_gamepad_flip(uint32_t flip);

/* The address the host assigned pad `pad` with SET_ADDRESS, 0 until it does,
 * and whether it has been configured. */
uint8_t usb_gamepad_address(int pad);
int usb_gamepad_configured(int pad);

/* Up to four pads, one per Xbox controller port. Pad n is driven by host
 * XInput pad n and by pad-script steps prefixed "p<n+1>-". */
#define USB_GAMEPAD_MAX 4

/* A game's own input layer, in place of host pad n driving pad n.
 *
 * A title that reaches its pad through this model (XAPI linked into the XBE)
 * has no XInputGetState for a game to override, so this is where a layer
 * that maps host pads to ports, runs scripts or merges the keyboard plugs
 * in. Installed once, before xbox_OhciInit; with none installed every
 * report, plug and rumble is what it was before the hook existed.
 *
 *   get_state  pad `pad`'s state now; 0 when no source drives it (the
 *              report is then all released). Called on every report, on
 *              the controller thread.
 *   present    the pads that have a source now, bit n for pad n. The root
 *              hub plugs and unplugs pads to match (ohci.c), so the title
 *              sees controllers inserted and removed. Called once per
 *              controller pass.
 *   rumble     the title's motor words for pad `pad`; NULL forwards them
 *              to host pad `pad` as before.
 *
 * The toolkit's RECOMP_PAD_PRESS, RECOMP_PAD_SCRIPT and RECOMP_PAD_LIVE
 * still press on top of get_state, and keep the pads they name plugged in.
 * The callbacks must not call back into this model. */
struct XBOX_GAMEPAD;
typedef struct {
    int      (*get_state)(int pad, struct XBOX_GAMEPAD *out);
    uint32_t (*present)(void);
    void     (*rumble)(int pad, uint16_t left, uint16_t right);
} UsbPadSource;

void usb_gamepad_set_source(const UsbPadSource *src);

/* Called at the end of every port-0 poll with that poll's number (the g<poll>
 * clock), on the polling thread, before the clock moves on. One hook; NULL
 * removes it. The guest state hash (kernel guest_state_hash.c) uses it. */
void usb_gamepad_set_poll_hook(void (*fn)(uint32_t poll));
const UsbPadSource *usb_gamepad_source(void);

/* With a source: the pads that should be plugged in now -- the source's,
 * the toolkit script's, the synthetic press's. */
uint32_t usb_gamepad_present(void);

/* Pad `pad` left the bus: it forgets its address and configuration, so it
 * enumerates from the default address when it comes back. */
void usb_gamepad_detach(int pad);

/* The title's motor words for pad `pad`, to the source's rumble or to host
 * pad `pad`. */
void usb_gamepad_rumble(int pad, uint16_t left, uint16_t right);

/* The executor's flip count as usb_gamepad_flip last set it (0 on a title
 * whose present path does not count flips). */
uint32_t usb_gamepad_flips(void);

/* A game that answers the title's XInputGetState itself (BLiNX 2's
 * recomp_manual.c) never asks this model for a report, so the toolkit's
 * RECOMP_PAD_SCRIPT, RECOMP_PAD_LIVE, RECOMP_PAD_PRESS and
 * RECOMP_DEBUG=pad_record would never run for it. These give it the same
 * ones, on the same flip count and in the same step format, so a record
 * made through either path replays through either.
 *
 *   game_press   the script's, the live file's and the synthetic press's
 *                buttons for pad `pad`, pressed on top of *g, as
 *                usb_gamepad_report presses them on a source's state.
 *   game_record  *g, the state the title gets, to the pad record, under
 *                the flip the same poll's game_press used. Call it on
 *                every poll, a disconnected port's zeros too, so a pad
 *                that drops mid-hold is seen released.
 *   game_pads    the pads those inputs name (bit n for pad n), which the
 *                game should report connected.
 *
 * Call them on the thread that polls, one poll at a time; the toolkit
 * keeps their state unlocked, as it does for the report. */
#define USB_GAMEPAD_HAVE_GAME_TAP 1
void usb_gamepad_game_press(int pad, struct XBOX_GAMEPAD *g);
void usb_gamepad_game_record(int pad, const struct XBOX_GAMEPAD *g);
uint32_t usb_gamepad_game_pads(void);

/* A file the title opened (status 0) or failed to: the pad script's armed
 * anchor may be waiting on it, and the pad record may write an anchor for
 * it (pad_script.h). Called by NtCreateFile on the thread that opened it.
 * Without a pad script, live file or pad record it does nothing. */
#define USB_GAMEPAD_HAVE_FILE_OPEN 1
void usb_gamepad_file_open(const char *path, uint32_t status);

#endif /* XBOX_USB_GAMEPAD_H */
