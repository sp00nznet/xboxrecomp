/*
 * Typed text for the menu's fields: the ring and the request (text_input.h).
 *
 * The ring is single-producer, single-consumer: s_head is written only by
 * the window's thread and s_tail only by the guest's, each published with
 * release and read with acquire, so a slot is filled before the head that
 * shows it and emptied before the tail that frees it.
 *
 * The gate and the generation are one word, s_state = gen << 1 | on, so a
 * producer reads both in one load. Each slot carries the state it was
 * pushed under, and Pop drops a slot whose state is not the current one: a
 * producer that read the gate open, stalled across an End and a Begin and
 * then pushed would otherwise put a codepoint of the old field into the new
 * one. Begin and End also empty the ring from the consumer side.
 */
#include "text_input.h"
#include "recomp_env.h"

#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif

#define LOAD(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define STORE(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define RELAXED(p)   __atomic_load_n((p), __ATOMIC_RELAXED)

static uint32_t s_ring[XBOX_TEXT_RING];
static unsigned s_tag[XBOX_TEXT_RING];  /* s_state at the push */
static unsigned s_head;         /* producer only; free-running */
static unsigned s_tail;         /* consumer only; free-running */
static unsigned s_dropped;      /* producer adds, anyone reads */

/* The request. One writer, the guest's thread. s_seq is a sequence lock
 * over s_state and s_rect (odd while they change), so a reader never pairs
 * one request's generation with another's rect. The producers read only
 * s_state. */
static unsigned s_seq;
static unsigned s_state;
static uint64_t s_rect;         /* x, y, w, h: four 16-bit fields */
static unsigned s_dropped_at_begin;    /* consumer only: for the trace */

/* Producer only: a high surrogate waiting for its low one (UTF-16), and a
 * DBCS lead byte waiting for its trail byte (an ANSI window's WM_CHAR),
 * each with the s_state it arrived under: a half from before an End, a
 * Begin or a focus loss never pairs with what comes after. */
static uint16_t s_high;
static unsigned s_high_state;
#ifdef _WIN32
static unsigned char s_lead;
static unsigned s_lead_state;
#endif

static uint64_t pack(int x, int y, int w, int h)
{
    int v[4] = { x, y, w, h };
    uint64_t r = 0;
    int i;

    for (i = 0; i < 4; i++) {
        int c = v[i] < 0 ? 0 : v[i] > 0xFFFF ? 0xFFFF : v[i];
        r |= (uint64_t)c << (16 * i);
    }
    return r;
}

static void flush(void)
{
    STORE(&s_tail, LOAD(&s_head));
}

/* The consumer's write of a new request: odd s_seq, the fields, even. */
static void publish(unsigned state, const uint64_t *rect)
{
    unsigned q = RELAXED(&s_seq);

    __atomic_store_n(&s_seq, q + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (rect)
        __atomic_store_n(&s_rect, *rect, __ATOMIC_RELAXED);
    __atomic_store_n(&s_state, state, __ATOMIC_RELEASE);
    STORE(&s_seq, q + 2);
}

void xbox_InputTextBegin(int x, int y, int w, int h)
{
    unsigned st = RELAXED(&s_state);
    uint64_t r = pack(x, y, w, h);

    if (!(st & 1))
        s_dropped_at_begin = LOAD(&s_dropped);
    publish((((st >> 1) + 1) << 1) | 1, &r);
    flush();
    if (recomp_env(RENV_KEY_TRACE)) {
        fprintf(stderr, "[TEXT] on: field %d,%d %dx%d\n", x, y, w, h);
        fflush(stderr);
    }
}

void xbox_InputTextEnd(void)
{
    unsigned st = RELAXED(&s_state);

    if (!(st & 1))
        return;
    publish(((st >> 1) + 1) << 1, NULL);
    flush();
    if (recomp_env(RENV_KEY_TRACE)) {
        fprintf(stderr, "[TEXT] off: %u dropped (ring full)\n",
                LOAD(&s_dropped) - s_dropped_at_begin);
        fflush(stderr);
    }
}

static int utf8_len(uint32_t cp)
{
    return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

int xbox_InputTextPop(char *utf8, size_t n)
{
    unsigned st = LOAD(&s_state);
    unsigned t = s_tail;
    uint32_t cp;
    int len;
    unsigned char *o = (unsigned char *)utf8;

    if (!(st & 1))
        return 0;
    for (;; t++) {
        if (t == LOAD(&s_head)) {
            STORE(&s_tail, t);
            return 0;
        }
        if (s_tag[t % XBOX_TEXT_RING] == st)
            break;
        /* Pushed under another request: an old field's, or with the gate
         * closed. */
    }
    STORE(&s_tail, t);
    cp = s_ring[t % XBOX_TEXT_RING];
    len = utf8_len(cp);
    if (!utf8 || n < (size_t)len + 1)
        return -1;
    switch (len) {
    case 1:
        o[0] = (unsigned char)cp;
        break;
    case 2:
        o[0] = (unsigned char)(0xC0 | (cp >> 6));
        o[1] = (unsigned char)(0x80 | (cp & 0x3F));
        break;
    case 3:
        o[0] = (unsigned char)(0xE0 | (cp >> 12));
        o[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (unsigned char)(0x80 | (cp & 0x3F));
        break;
    default:
        o[0] = (unsigned char)(0xF0 | (cp >> 18));
        o[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        o[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        o[3] = (unsigned char)(0x80 | (cp & 0x3F));
        break;
    }
    o[len] = 0;
    STORE(&s_tail, t + 1);
    return len;
}

int xbox_InputTextActive(void)
{
    return LOAD(&s_state) & 1;
}

/* One codepoint under state `st`, read once by the caller. */
static int push(uint32_t cp, unsigned st)
{
    unsigned h = s_head;

    if (!(st & 1))
        return 0;
    /* Control characters arrive as key edges; the field never takes them
     * (Ctrl+S's 0x13 would otherwise land in a name). */
    if (cp < 0x20 || cp == 0x7F)
        return 0;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    if (h - LOAD(&s_tail) >= XBOX_TEXT_RING) {
        __atomic_add_fetch(&s_dropped, 1, __ATOMIC_ACQ_REL);
        return 0;
    }
    s_ring[h % XBOX_TEXT_RING] = cp;
    s_tag[h % XBOX_TEXT_RING] = st;
    STORE(&s_head, h + 1);
    return 1;
}

int xbox_InputTextPushCodepoint(uint32_t cp)
{
    return push(cp, LOAD(&s_state));
}

int xbox_InputTextPushUtf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned st = LOAD(&s_state);
    int queued = 0;

    if (!p)
        return 0;
    while (*p) {
        uint32_t cp, min;
        int more, i;

        if (*p < 0x80)       { cp = *p;        more = 0; min = 0; }
        else if (*p >= 0xC2 && *p < 0xE0) { cp = *p & 0x1F; more = 1; min = 0x80; }
        else if (*p >= 0xE0 && *p < 0xF0) { cp = *p & 0x0F; more = 2; min = 0x800; }
        else if (*p >= 0xF0 && *p < 0xF5) { cp = *p & 0x07; more = 3; min = 0x10000; }
        else { p++; continue; }            /* a stray continuation, or never valid */
        for (i = 1; i <= more; i++) {
            if ((p[i] & 0xC0) != 0x80)
                break;
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (i <= more) {                   /* truncated: resume at the bad byte */
            p += i;
            continue;
        }
        p += more + 1;
        /* Overlong forms and surrogates are not text; push drops the
         * surrogates and anything past U+10FFFF. */
        if (cp < min)
            continue;
        queued += push(cp, st);
    }
    return queued;
}

int xbox_InputTextPushUtf16(uint16_t unit)
{
    unsigned st = LOAD(&s_state);
    uint16_t high = s_high_state == st ? s_high : 0;

    s_high = 0;
    if (!(st & 1))
        return 0;
    if (unit >= 0xD800 && unit <= 0xDBFF) {
        s_high = unit;                     /* a high one before is dropped */
        s_high_state = st;
        return 0;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (!high)
            return 0;                      /* a lone low surrogate */
        return push(0x10000u + (((uint32_t)high - 0xD800u) << 10)
                    + ((uint32_t)unit - 0xDC00u), st);
    }
    return push(unit, st);
}

void xbox_InputTextFocusLost(void)
{
    s_high = 0;
#ifdef _WIN32
    s_lead = 0;
#endif
}

unsigned xbox_InputTextRequest(int *on, int rect[4])
{
    unsigned q, st;
    uint64_t r;
    int i;

    /* The writer's three stores take no time unless the guest's thread is
     * preempted between them; yield rather than spin through that. */
    for (;;) {
        q = LOAD(&s_seq);
        if (!(q & 1)) {
            st = RELAXED(&s_state);
            r = RELAXED(&s_rect);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (RELAXED(&s_seq) == q)
                break;
        }
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    if (on)
        *on = (int)(st & 1);
    if (rect)
        for (i = 0; i < 4; i++)
            rect[i] = (int)((r >> (16 * i)) & 0xFFFF);
    return st >> 1;
}

unsigned xbox_InputTextDropped(void)
{
    return LOAD(&s_dropped);
}

void xbox_InputTextReset(void)
{
    int i;

    STORE(&s_seq, 0);
    STORE(&s_state, 0);
    STORE(&s_rect, 0);
    STORE(&s_head, 0);
    STORE(&s_tail, 0);
    STORE(&s_dropped, 0);
    for (i = 0; i < XBOX_TEXT_RING; i++)
        s_tag[i] = 0;
    s_dropped_at_begin = 0;
    s_high = 0;
    s_high_state = 0;
#ifdef _WIN32
    s_lead = 0;
    s_lead_state = 0;
#endif
}

#ifdef _WIN32
/* The code page an ANSI window's WM_CHAR is taken to be in: the input
 * language's, which the layout switch (Alt+Shift) changes. Whether Windows
 * converts by this or by the system ACP (and so under the "Beta: UTF-8"
 * system setting, by UTF-8) is not settled; a Japanese IME on the PC is the
 * hand check. */
static UINT input_code_page(void)
{
    LANGID lang = LOWORD((UINT_PTR)GetKeyboardLayout(0));
    UINT cp = 0;

    if (GetLocaleInfoA(MAKELCID(lang, SORT_DEFAULT),
                       LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                       (LPSTR)&cp, sizeof cp) == 0 || cp == 0)
        cp = CP_ACP;
    return cp;
}

int xbox_InputTextWin32Ansi(unsigned cp, unsigned char b, int repeat)
{
    unsigned st = LOAD(&s_state);
    unsigned char lead = s_lead_state == st ? s_lead : 0;
    char mb[2];
    WCHAR wc[2];
    int n = 0, nw, i, k, queued = 0;

    s_lead = 0;
    if (!(st & 1))
        return 0;
    /* A double-byte code page (Japanese, Chinese, Korean) sends a
     * character as two WM_CHARs, lead byte first. */
    if (!lead && IsDBCSLeadByteEx(cp, b)) {
        s_lead = b;
        s_lead_state = st;
        return 0;
    }
    if (lead)
        mb[n++] = (char)lead;
    mb[n++] = (char)b;
    nw = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, mb, n, wc, 2);
    if (nw == 1) {
        for (k = 0; k < repeat; k++)
            queued += xbox_InputTextPushUtf16((uint16_t)wc[0]);
    } else {
        for (i = 0; i < nw; i++)
            queued += xbox_InputTextPushUtf16((uint16_t)wc[i]);
    }
    return queued;
}

int xbox_InputTextWin32Char(void *hwnd, uintptr_t w, intptr_t l)
{
    /* Bits 0-15: the repeat count of a held key, which Windows may fold
     * into one message. Capped so one message never fills the ring. */
    int repeat = (int)(l & 0xFFFF), k;

    if (!xbox_InputTextActive()) {
        xbox_InputTextFocusLost();
        return 0;
    }
    if (repeat < 1)
        repeat = 1;
    if (repeat > 16)
        repeat = 16;
    if (hwnd && IsWindowUnicode((HWND)hwnd)) {
        uint16_t unit = (uint16_t)w;
        /* A surrogate half takes no repeat: the pair is one keystroke. */
        if (unit >= 0xD800 && unit <= 0xDFFF)
            repeat = 1;
        for (k = 0; k < repeat; k++)
            xbox_InputTextPushUtf16(unit);
        return 1;
    }
    xbox_InputTextWin32Ansi(input_code_page(), (unsigned char)w, repeat);
    return 1;
}
#endif
