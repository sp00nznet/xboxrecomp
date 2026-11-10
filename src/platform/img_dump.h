/*
 * img_dump.h - the one writer behind every debug image the runtime dumps:
 * frame dumps (fb_dump, fb_dump_at, d3d11_dump, metal_host_dump), A/B
 * captures, the HUD capture's pictures, window shots and texture BMPs.
 *
 * The format follows the path's extension: ".png" writes a lossless PNG
 * (stb_image_write, third_party/stb), anything else the uncompressed BMP
 * the dumpers always wrote. Callers name their files with img_dump_ext(),
 * which is ".png" unless RECOMP_DEBUG=dump_format=bmp. A 640x480 frame is
 * 921,654 bytes as BMP; as PNG, measured on BLiNX 2's goldens, 1-91% of
 * that a frame (flat menus and loading screens 1-10%, noisy textured
 * gameplay up to 91%), 41-76% a scenario and 52-53% over all three. The CLI's readers take either spelling, so
 * references and old runs stay valid.
 *
 * The budget: every byte written through here, plus what the texture dump
 * charges (img_dump_budget_take), counts against one per-process cap,
 * RECOMP_DEBUG=dump_budget_mb, whole MiB (default 2048 on macOS, where the
 * work disk is the workbench's, else none; 0 = none). A write that would
 * pass it is refused with one line, and the run goes on without further
 * dumps.
 */
#ifndef IMG_DUMP_H
#define IMG_DUMP_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "recomp_env.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ".png", or ".bmp" with dump_format=bmp: the extension a dumper names its
 * files with. Inline, so a file that only names dumps needs no link. */
static inline const char *img_dump_ext(void)
{
    const char *f = recomp_env(RENV_DUMP_FORMAT);
    return f && strcmp(f, "bmp") == 0 ? ".bmp" : ".png";
}

/* bpp 4 only: keep the fourth byte. The PNG is then RGBA unless that byte is
 * 0 in every pixel (as `frames compact` decides, so a dump and a compacted
 * BMP of it have the same pixels), and a BMP is 32-bit top-down. Without it
 * the fourth byte is dropped: RGB, 24-bit. */
#define IMG_DUMP_ALPHA 1u

/* w x h pixels, rows top-down `pitch` bytes apart, bytes B, G, R (bpp 3) or
 * B, G, R, X (bpp 4), to `path` in the format its extension names. The file
 * is written under path.tmp and renamed, so a reader polling for it never
 * sees half a frame. 1 when it is written; 0 on an error or over the
 * budget, and then no file.
 *
 * A PNG is only repacked here; the deflate runs on writer threads (a game
 * frame costs 7-35 ms to compress, more than a 60 Hz flip). Its budget is
 * charged here, at an upper bound of its size, and settled once it is
 * deflated, so a 1 is a promise: the file appears shortly after (only an
 * I/O error or a hard kill breaks it), and img_dump_flush waits for it.
 * The queue is bounded: a caller ahead of it waits. A BMP is written
 * before the call returns, as is a PNG when no writer thread started. */
int img_dump_write(const char *path, const uint8_t *px, size_t pitch, uint32_t w,
                   uint32_t h, unsigned bpp, unsigned flags);

/* Wait up to `ms` for every queued PNG to be on disk: 1 when they are.
 * The process's exits (atexit, the window's exit hooks, SIGINT/SIGTERM, the
 * Windows console handler) already do it; tests and a dumper that reads
 * its own file back call it. Signal-safe. */
int img_dump_flush(unsigned ms);

/* Charge `bytes` to the budget: 1 when they fit (and are now counted), 0
 * when they do not (counted as refused; the first refusal prints the line). */
int img_dump_budget_take(uint64_t bytes);

/* Give back bytes taken with img_dump_budget_take for a file that was not
 * written after all. */
void img_dump_budget_refund(uint64_t bytes);

/* Bytes charged so far, and the cap (0: none). */
uint64_t img_dump_budget_used(void);
uint64_t img_dump_budget_cap(void);

/* Tests: forget the cap read from the environment and the bytes charged. */
void img_dump_budget_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* IMG_DUMP_H */
