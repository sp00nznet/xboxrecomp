/**
 * xbox_input -- the host pad as the host sees it, before any merge, and the
 * filter seam the enhancements layer's host actions use (opt-in,
 * XBOXRECOMP_ENHANCE=ON; docs/runtime/config.md, "[input.actions]").
 *
 * A host pad has buttons the Duke never had: Guide, the paddles, the
 * touchpad and the share button (MISC1). The guest map leaves them out, so
 * they reach the host side only, as the `extra` bits below. A pad filter
 * sees the host pad and the state the title is about to get, and may clear
 * buttons in the latter (a chord the host consumed); input_core.c calls it
 * at the end of xbox_InputGetState, the title's glue calls it from its own
 * pad read (cat's pad_input.c). With no filter set nothing changes.
 */
#ifndef XBOX_INPUT_HOST_PAD_H
#define XBOX_INPUT_HOST_PAD_H

#include <stdint.h>
#include "xinput_xbox.h"
#include "input_map.h"   /* InputRaw */

#ifdef __cplusplus
extern "C" {
#endif

/* The host-only buttons (struct xbox_host_pad.extra). */
#define XBOX_HOST_GUIDE     0x01u
#define XBOX_HOST_MISC1     0x02u   /* share, capture, mic */
#define XBOX_HOST_PADDLE1   0x04u   /* upper right (Elite P1, Deck R4) */
#define XBOX_HOST_PADDLE2   0x08u   /* upper left (P3, L4) */
#define XBOX_HOST_PADDLE3   0x10u   /* lower right (P2, R5) */
#define XBOX_HOST_PADDLE4   0x20u   /* lower left (P4, L5) */
#define XBOX_HOST_TOUCHPAD  0x40u
#define XBOX_HOST_EXTRA_MASK 0x7Fu

/* A read the backend could not make now (its lock was busy): the caller
 * keeps what it had. */
#define XBOX_INPUT_BUSY 170u        /* ERROR_BUSY */

/* struct xbox_host_pad has `raw`; a title's glue fills it under this. */
#define XBOX_HOST_PAD_RAW 1

struct xbox_host_pad {
    XBOX_INPUT_STATE pad;   /* the host pad mapped to the Duke ([input.pad]),
                               no keyboard */
    uint32_t extra;         /* XBOX_HOST_* */
    int connected;          /* 0: pad and extra are zero */
    /* The backend slot behind the title's port (xbox_InputGetPadState's), or
     * -1 for none: the flip tick reads this slot when the title has not
     * polled for a while. input_core.c's ports are their own slots; a title
     * that maps pads to ports itself (cat's pad_slots) says which. */
    int slot;
    /* The host pad before the map (pad_map.h's adapter, inversion applied):
     * the host actions name these sources, so a remap never moves them. */
    InputRaw raw;
};

/* Called with the title's port, the host pad behind it (connected = 0 when
 * there is none: the keyboard alone, a script) and the state the title gets,
 * which it may change. */
typedef void (*xbox_input_pad_filter_fn)(DWORD port, const struct xbox_host_pad *host,
                                         XBOX_INPUT_STATE *st);

/* Install or remove (NULL) the filter xbox_InputGetState calls. Any thread;
 * a poll in flight finishes with the filter it loaded. */
void xbox_InputSetPadFilter(xbox_input_pad_filter_fn fn);

/* Nonzero while a filter is installed (the host actions are armed). A
 * backend reads the host-only buttons only then, so a stock run polls the
 * way it did before them. */
int xbox_InputPadFilterSet(void);

/* The mod layer's record of port 0 (mod.h xbox_enhance_pad_seen). Called by
 * xbox_InputGetState under XBOXRECOMP_ENHANCE after the filter, with the
 * state the title is about to get (a failed poll: zeros, connected 0),
 * whether or not a filter is set: a mod may hold capture with the hook
 * disarmed. Nonzero while a mod holds port 0: the title then reads it
 * connected and released (the neutral pad). The enhance library installs
 * it, so xbox_input never links against it. */
#define XBOX_HOST_PAD_SEEN 1
typedef int (*xbox_input_seen_fn)(DWORD port, const XBOX_GAMEPAD *g, int connected);
void xbox_InputSetPadSeen(xbox_input_seen_fn fn);

/* The host-only buttons the last poll of slot saw (0 when none). */
uint32_t xbox_InputHostExtra(DWORD slot);

/* The host pad the last poll of slot saw, before the map (all zero when
 * none). */
void xbox_InputHostRaw(DWORD slot, InputRaw *raw);

/* Read slot's host pad off the poll thread (the flip tick): the raw pad, the
 * guest map of it and extra. ERROR_SUCCESS, ERROR_DEVICE_NOT_CONNECTED (host zeroed), or
 * XBOX_INPUT_BUSY (host untouched). Publishes the extra bits, and clears
 * them when no pad is open, so a modifier held as the pad went away does
 * not stay held. */
DWORD xbox_input_backend_read_host(DWORD slot, struct xbox_host_pad *host);

/* The controller family of the pad on slot, for glyphs (gamepad-input-ux
 * D5): XInput says xbox, since it cannot tell a Steam virtual pad from a
 * real one; SDL3 maps SDL_GetGamepadType, and the Deck's own pad (28de:1205)
 * is steam. input.glyphs overrides it for every slot
 * (xbox_InputSetGlyphFamily; XBOX_PAD_FAMILY_AUTO restores detection). */
enum {
    XBOX_PAD_FAMILY_XBOX, XBOX_PAD_FAMILY_PLAYSTATION, XBOX_PAD_FAMILY_NINTENDO,
    XBOX_PAD_FAMILY_STEAM, XBOX_PAD_FAMILY_GENERIC, XBOX_PAD_FAMILY_GAMECUBE,
    XBOX_PAD_FAMILY_COUNT,
    XBOX_PAD_FAMILY_AUTO = -1
};
int  xbox_InputPadFamily(DWORD slot);
void xbox_InputSetGlyphFamily(int family);
const char *xbox_InputPadFamilyName(int family);
int  xbox_InputPadFamilyFromName(const char *name);   /* "auto" -> AUTO; -2 unknown */

/* Whether Steam Input stands between the pad and the game (D6). on: an SDL
 * pad Steam made (SDL_GetGamepadSteamHandle, or its virtual pad 28de:11ff;
 * the vendor id alone never counts, so a Deck with Steam Input off is off).
 * maybe: XInput, which cannot tell, under Steam (SteamGameId or SteamDeck
 * set). */
enum { XBOX_STEAM_INPUT_OFF, XBOX_STEAM_INPUT_ON, XBOX_STEAM_INPUT_MAYBE };
int  xbox_InputSteamInput(DWORD slot);

/* The backend's clock (a test's fake one), in milliseconds. */
uint64_t xbox_InputNowMs(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_HOST_PAD_H */
