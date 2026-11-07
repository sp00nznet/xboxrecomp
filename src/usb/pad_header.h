/*
 * pad_header.h -- the header of a pad recording (pad-recording-ui R5).
 *
 * A recording made in the game starts with comment lines the step parser
 * already skips (pad_script.h): the overlay-revamp export marker, kind pad,
 * format 1, then one "# xbr-pad: key = value" line per key:
 *
 *   # xbr-export: pad 1
 *   # xbr-pad: game = "blinx2"
 *   # xbr-pad: clock = "poll"
 *   # xbr-pad: origin = "session"
 *   # xbr-pad: length = 5412
 *
 * Values are TOML scalars, so the CLI's tomllib and this read them the same
 * way: a basic or literal string, or a bare scalar (an integer, a boolean)
 * kept as written. Unknown keys are kept, so a file from a newer toolkit
 * loses nothing it can still play. A file with no marker is an env record
 * (origin boot). Pure: text in, text out, no files, so it is tested alone
 * (tests/pad_script).
 */
#ifndef XBOX_USB_PAD_HEADER_H
#define XBOX_USB_PAD_HEADER_H

#include <stddef.h>

#define PAD_HDR_KIND   "pad"
#define PAD_HDR_FORMAT 1
/* The reader's bounds: past them the file is refused, not cut, so a crafted
 * header can neither run the parser long nor hide steps in a long line. */
#define PAD_HDR_LINE_MAX  1024
#define PAD_HDR_KEYS_MAX  64
#define PAD_HDR_VALUE_MAX 512
#define PAD_HDR_KEY_MAX   32

typedef struct {
    char key[PAD_HDR_KEY_MAX];
    /* A string's text, decoded (UTF-8, no control characters), or a bare
     * scalar as written. */
    char value[PAD_HDR_VALUE_MAX + 1];
    int text;                   /* 1: a string */
} PadHeaderKey;

typedef struct {
    int present;                /* the marker was read, or a key was set */
    int n;
    PadHeaderKey k[PAD_HDR_KEYS_MAX];
} PadHeader;

enum {
    PAD_HDR_OK = 0,
    PAD_HDR_NEWER = -1,         /* a format above PAD_HDR_FORMAT */
    PAD_HDR_KIND_OTHER = -2,    /* the marker of another export (a config) */
    PAD_HDR_LONG_LINE = -3,
    PAD_HDR_TOO_MANY = -4,
    PAD_HDR_LONG_VALUE = -5,
    PAD_HDR_SYNTAX = -6
};

void pad_header_init(PadHeader *h);

/* Set key to a string. Control characters (CR and LF included) are
 * dropped, so a note can never split its comment line into a step; the
 * text is cut on a character boundary to what the reader takes. Returns 0,
 * or -1 for a bad key or a full header. */
int pad_header_set_text(PadHeader *h, const char *key, const char *utf8);
int pad_header_set_int(PadHeader *h, const char *key, long long v);

/* The value of key (a string's text, a bare scalar as written), or NULL. */
const char *pad_header_get(const PadHeader *h, const char *key);
long long pad_header_get_int(const PadHeader *h, const char *key, long long dflt);

/* "session" for a recording made in the game, "boot" for one with no
 * header or no origin key (an env record, counted from boot). */
const char *pad_header_origin(const PadHeader *h);

/* The marker and one line per key, LF ended, into out. Returns the length,
 * or -1 when n is too small. */
int pad_header_write(const PadHeader *h, char *out, size_t n);

/* Read the leading comment lines of text (a UTF-8 BOM skipped, CRLF read
 * as LF). PAD_HDR_OK with h->present 0 when there is no marker. A
 * negative PAD_HDR_* refuses the file. */
int pad_header_read(const char *text, PadHeader *h);

/* A rc of pad_header_read in words, for a log line or a toast. */
const char *pad_header_error(int rc);

#endif /* XBOX_USB_PAD_HEADER_H */
