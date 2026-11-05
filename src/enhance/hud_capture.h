/*
 * hud_capture.h - the HUD mapping capture (openspec change hud-anchors,
 * design A15): what the game draws as HUD, recorded per sprite with the
 * call chain that drew it, for `xbr hud-map` to turn into a table.
 *
 * RECOMP_DEBUG keys:
 *   hud_capture=<dir>          on; writes <dir>/capture.jsonl and one
 *                              <dir>/frame_NNNNN.bmp per captured frame
 *   hud_capture_at=<from>:<count>  the guest frames (default 1:1)
 *   hud_capture_table=<path>   a HUD table the capture loads and hooks
 *                              itself, so a capture needs no mod
 *
 * It records and changes nothing. Level 1 records each hud_apply call
 * (hud_anchor.h's recorder), whether or not the HUD mod is switched on;
 * level 0 hooks the draw entries the table lists under [capture] draws and
 * finds their callers by a scan of the stack. Both go into a buffer on the
 * guest's thread, written to capture.jsonl at xbox_mod_frame; a frame's
 * lines are written once its picture is (or cannot be) taken, so a reader
 * that sees frame F's record has F's sprites and its BMP. The format is in
 * docs/runtime/config.md ("HUD table (v5)", the capture).
 *
 * A guest frame is the span between two xbox_mod_frame calls: frame F runs
 * from the Fth call to the next. Its picture is the flip that presents it:
 * at the Fth call the capture waits (up to 200 ms) for the walker to finish
 * what the guest kicked, so the next flip is F's, and asks for it
 * (nv2a_backend_common.h's capture requests).
 *
 * With the keys unset, hud_capture_init reads one key and every other
 * entry returns at its first test.
 */
#ifndef HUD_CAPTURE_H
#define HUD_CAPTURE_H

#include <stdint.h>

#include "hud_anchor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The keys, at mod init. `mem` is where guest memory is mapped (mod.c's
 * api.mem, as hud_layer_set_mem). 1 when a capture runs: the layer's
 * capture flag and recorder are then set. */
int hud_capture_init(const uintptr_t *mem);
int hud_capture_on(void);
/* On and not yet done (its window written, or the end): while this holds,
 * the HUD table owner's hooks run with the mod off (mod.c, hook_runs). */
int hud_capture_live(void);

/* After the mods' inits, before the hooks are published: loads
 * hud_capture_table when no mod registered a table (`have_table` 0), and
 * hooks the table's [capture] draws, both through `hook`. */
void hud_capture_tables(hud_hook_fn hook, int have_table);

/* Once per guest frame (xbox_mod_frame), on the guest's thread, after the
 * layer's frame. */
void hud_capture_frame(void);

/* Shutdown: waits a little for the pictures asked for, writes what is
 * left and closes the file. */
void hud_capture_end(void);

/* Tests: everything back to off. */
void hud_capture_reset(void);

/* Tests: the call-opcode test the stack scan applies to a candidate return
 * address (a CALL rel32, CALL [abs], CALL reg or CALL [reg+d8] just before
 * it). */
int hud_capture_is_call_ret(const uint8_t *mem, uint32_t ret);

#ifdef __cplusplus
}
#endif

#endif /* HUD_CAPTURE_H */
