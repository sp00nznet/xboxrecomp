/*
 * input_text test: see CMakeLists.txt.
 *
 * The expected codepoints and their UTF-8 bytes are written out here by
 * hand, not produced by the code under test.
 */
/* The ring itself, so the toggle test can read the tag a push wrote. */
#include "text_input.c"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static int g_fail, g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Everything queued, concatenated as UTF-8. */
static void drain(char *out, size_t n)
{
    char cp[8];
    size_t used = 0;
    int len;

    out[0] = 0;
    while ((len = xbox_InputTextPop(cp, sizeof cp)) > 0) {
        if (used + (size_t)len < n) {
            memcpy(out + used, cp, (size_t)len);
            used += (size_t)len;
            out[used] = 0;
        }
    }
}

static void fresh(void)
{
    xbox_InputTextReset();
    xbox_InputTextBegin(10, 20, 300, 40);
}

static void test_utf8(void)
{
    char out[64];

    fresh();
    /* a, e-acute (2 bytes), euro (3 bytes), U+1F600 (4 bytes) */
    CHECK(xbox_InputTextPushUtf8("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80") == 4,
          "four codepoints queued");
    drain(out, sizeof out);
    CHECK(strcmp(out, "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80") == 0,
          "round trip, got '%s'", out);

    /* Ill-formed: a stray continuation, an overlong '/', an encoded
     * surrogate (U+D800), a truncated 3-byte form, a byte never valid. */
    CHECK(xbox_InputTextPushUtf8("\x80" "x" "\xC0\xAF" "\xED\xA0\x80" "\xE2\x82" "y"
                                 "\xFF" "z") == 3, "only x, y and z queued");
    drain(out, sizeof out);
    CHECK(strcmp(out, "xyz") == 0, "ill-formed skipped, got '%s'", out);
}

static void test_utf16(void)
{
    char out[64];

    fresh();
    CHECK(xbox_InputTextPushUtf16(0x00E9) == 1, "a BMP unit");
    /* U+1F600 = D83D DE00 */
    CHECK(xbox_InputTextPushUtf16(0xD83D) == 0, "a high surrogate waits");
    CHECK(xbox_InputTextPushUtf16(0xDE00) == 1, "its low one completes it");
    drain(out, sizeof out);
    CHECK(strcmp(out, "\xC3\xA9\xF0\x9F\x98\x80") == 0, "pair joined, got '%s'", out);

    /* A lone low surrogate, and a high one followed by a plain unit. */
    CHECK(xbox_InputTextPushUtf16(0xDC00) == 0, "a lone low surrogate dropped");
    CHECK(xbox_InputTextPushUtf16(0xD800) == 0, "a high surrogate waits");
    CHECK(xbox_InputTextPushUtf16('q') == 1, "a plain unit after it");
    CHECK(xbox_InputTextPushUtf16(0xDC00) == 0, "the high one was dropped, not kept");
    /* Two highs in a row: the first is dropped, the second pairs. */
    xbox_InputTextPushUtf16(0xD83D);
    xbox_InputTextPushUtf16(0xD83D);
    CHECK(xbox_InputTextPushUtf16(0xDE00) == 1, "the second high pairs");
    drain(out, sizeof out);
    CHECK(strcmp(out, "q\xF0\x9F\x98\x80") == 0, "lone surrogates dropped, got '%s'", out);
    CHECK(xbox_InputTextDropped() == 0, "malformed input is not an overflow");
}

static void test_control(void)
{
    char out[64];
    uint32_t c;
    int queued = 0;

    fresh();
    for (c = 0; c < 0x20; c++)
        queued += xbox_InputTextPushCodepoint(c);
    queued += xbox_InputTextPushCodepoint(0x7F);
    CHECK(queued == 0, "every control character dropped (%d queued)", queued);
    /* Tab, Enter, Esc, Backspace and Ctrl+S (0x13) as they arrive. */
    CHECK(xbox_InputTextPushUtf16(0x09) == 0, "Tab");
    CHECK(xbox_InputTextPushUtf16(0x0D) == 0, "Enter");
    CHECK(xbox_InputTextPushUtf16(0x1B) == 0, "Esc");
    CHECK(xbox_InputTextPushUtf16(0x08) == 0, "Backspace");
    CHECK(xbox_InputTextPushUtf16(0x13) == 0, "Ctrl+S");
    CHECK(xbox_InputTextPushUtf8("a\tb\x13" "c\x7F") == 3, "a, b and c from a mixed string");
    CHECK(xbox_InputTextPushCodepoint(0x20) == 1, "space is text");
    CHECK(xbox_InputTextPushCodepoint(0x110000) == 0, "past U+10FFFF");
    CHECK(xbox_InputTextPushCodepoint(0xDFFF) == 0, "a surrogate is no codepoint");
    drain(out, sizeof out);
    CHECK(strcmp(out, "abc ") == 0, "got '%s'", out);
    CHECK(xbox_InputTextDropped() == 0, "control characters are not an overflow");
}

static void test_overflow_wrap(void)
{
    char cp[8], out[8];
    int i, ok = 1, len;

    fresh();
    for (i = 0; i < XBOX_TEXT_RING; i++)
        ok &= xbox_InputTextPushCodepoint('A' + i % 26);
    CHECK(ok, "a full ring takes %d", XBOX_TEXT_RING);
    CHECK(xbox_InputTextPushCodepoint('!') == 0, "one past it dropped");
    CHECK(xbox_InputTextPushCodepoint('!') == 0, "and the next");
    CHECK(xbox_InputTextDropped() == 2, "both counted (%u)", xbox_InputTextDropped());
    for (i = 0; i < XBOX_TEXT_RING; i++) {
        len = xbox_InputTextPop(cp, sizeof cp);
        if (len != 1 || cp[0] != 'A' + i % 26) {
            ok = 0;
            break;
        }
    }
    CHECK(ok, "the first %d come out in order (stopped at %d)", XBOX_TEXT_RING, i);
    CHECK(xbox_InputTextPop(cp, sizeof cp) == 0, "then empty");

    /* Round the ring many times, a few at a time, so head and tail wrap
     * the slots at every offset. */
    ok = 1;
    for (i = 0; i < 10 * XBOX_TEXT_RING; i++) {
        uint32_t c = 0x100 + (uint32_t)i;      /* two-byte forms */
        int a = xbox_InputTextPushCodepoint(c);
        int b = xbox_InputTextPushCodepoint(c + 1);
        int got = 0;
        while (xbox_InputTextPop(cp, sizeof cp) > 0)
            got++;
        if (!a || !b || got != 2)
            ok = 0;
    }
    CHECK(ok, "wraparound keeps every codepoint");

    /* A buffer too small leaves the codepoint queued. */
    xbox_InputTextPushUtf8("\xE2\x82\xAC");
    CHECK(xbox_InputTextPop(out, 3) == -1, "3 bytes cannot hold a 3-byte form and its NUL");
    CHECK(xbox_InputTextPop(out, 4) == 3 && memcmp(out, "\xE2\x82\xAC", 4) == 0,
          "it is still there");
}

static void test_gate_request(void)
{
    char cp[8];
    int on = -1, rect[4] = { -1, -1, -1, -1 };
    unsigned g0, g1, g2, g3, g4;

    xbox_InputTextReset();
    /* A fake sync: it applies a request when the generation moved. */
    g0 = xbox_InputTextRequest(&on, rect);
    CHECK(on == 0 && !xbox_InputTextActive(), "off at the start");
    CHECK(xbox_InputTextPushCodepoint('a') == 0, "the gate is closed");
    CHECK(xbox_InputTextPushUtf8("abc") == 0, "for strings too");
    CHECK(xbox_InputTextPop(cp, sizeof cp) == 0, "nothing queued");
    CHECK(xbox_InputTextDropped() == 0, "a closed gate is not an overflow");
    xbox_InputTextEnd();
    CHECK(xbox_InputTextRequest(NULL, NULL) == g0, "End while off asks nothing");

    xbox_InputTextBegin(12, 34, 560, 78);
    g1 = xbox_InputTextRequest(&on, rect);
    CHECK(g1 != g0, "Begin is a new request");
    CHECK(on == 1 && xbox_InputTextActive(), "on");
    CHECK(rect[0] == 12 && rect[1] == 34 && rect[2] == 560 && rect[3] == 78,
          "the rect (%d,%d %dx%d)", rect[0], rect[1], rect[2], rect[3]);
    CHECK(xbox_InputTextRequest(NULL, NULL) == g1, "unchanged until the next call");

    xbox_InputTextPushUtf8("xy");
    xbox_InputTextBegin(-5, 70000, 1, 2);   /* moves the field, empties the ring */
    g2 = xbox_InputTextRequest(&on, rect);
    CHECK(g2 != g1 && on == 1, "a second Begin is a new request");
    CHECK(rect[0] == 0 && rect[1] == 0xFFFF && rect[2] == 1 && rect[3] == 2,
          "clamped (%d,%d %dx%d)", rect[0], rect[1], rect[2], rect[3]);
    CHECK(xbox_InputTextPop(cp, sizeof cp) == 0, "Begin emptied the ring");

    xbox_InputTextPushUtf8("z");
    xbox_InputTextEnd();
    g3 = xbox_InputTextRequest(&on, rect);
    CHECK(g3 != g2 && on == 0 && !xbox_InputTextActive(), "End turns it off");
    CHECK(xbox_InputTextPop(cp, sizeof cp) == 0, "End emptied the ring");
    CHECK(xbox_InputTextPushUtf16(0xD83D) == 0 && xbox_InputTextPushUtf16(0xDE00) == 0,
          "closed for UTF-16");

    /* A high surrogate typed before the gate closed does not pair with a
     * low one after it opens again. */
    xbox_InputTextBegin(0, 0, 1, 1);
    xbox_InputTextPushUtf16(0xD83D);
    xbox_InputTextEnd();
    xbox_InputTextPushUtf16('n');           /* closed: resets the pairing */
    xbox_InputTextBegin(0, 0, 1, 1);
    g4 = xbox_InputTextRequest(NULL, NULL);
    CHECK(g4 != g3, "a new generation");
    CHECK(xbox_InputTextPushUtf16(0xDE00) == 0, "the old high one is gone");

    /* Nor across a Begin with the gate open throughout. */
    xbox_InputTextPushUtf16(0xD83D);
    xbox_InputTextBegin(0, 0, 2, 2);
    CHECK(xbox_InputTextPushUtf16(0xDE00) == 0, "a high one from the field before");

    /* Nor across a focus loss. */
    xbox_InputTextPushUtf16(0xD83D);
    xbox_InputTextFocusLost();
    CHECK(xbox_InputTextPushUtf16(0xDE00) == 0, "a high one from before the focus loss");
    CHECK(xbox_InputTextPop(cp, sizeof cp) == 0, "nothing queued");
    xbox_InputTextEnd();
}

#ifdef _WIN32
static void test_win32(void)
{
    char out[32];

    /* No window: IsWindowUnicode(NULL) is false, the ANSI path. ASCII is
     * the same in every code page. */
    xbox_InputTextReset();
    CHECK(xbox_InputTextWin32Char(NULL, 'k', 1) == 0, "closed: passed on");
    xbox_InputTextBegin(0, 0, 1, 1);
    CHECK(xbox_InputTextWin32Char(NULL, 'k', 1) == 1, "open: taken");
    CHECK(xbox_InputTextWin32Char(NULL, 0x13, 1) == 1, "Ctrl+S taken, not queued");
    drain(out, sizeof out);
    CHECK(strcmp(out, "k") == 0, "got '%s'", out);

    /* The repeat count in lParam's low word: 0 is one, and it is capped. */
    xbox_InputTextWin32Char(NULL, 'r', 3);
    xbox_InputTextWin32Char(NULL, 's', 0);
    xbox_InputTextWin32Char(NULL, 't', 0x4000FFFF);
    drain(out, sizeof out);
    CHECK(strcmp(out, "rrrstttttttttttttttt") == 0, "repeats, got '%s'", out);

    /* Double-byte code pages: lead byte, then trail byte. Shift-JIS
     * 82 A0 is U+3042, GBK C4 E3 U+4F60, UHC C7 D1 U+D55C. */
    CHECK(xbox_InputTextWin32Ansi(932, 0x82, 1) == 0, "932: the lead byte waits");
    CHECK(xbox_InputTextWin32Ansi(932, 0xA0, 2) == 2, "932: the trail completes it, twice");
    drain(out, sizeof out);
    CHECK(strcmp(out, "\xE3\x81\x82\xE3\x81\x82") == 0, "932: U+3042 x2, got '%s'", out);
    CHECK(xbox_InputTextWin32Ansi(936, 0xC4, 1) == 0, "936: the lead byte waits");
    CHECK(xbox_InputTextWin32Ansi(936, 0xE3, 1) == 1, "936: the trail completes it");
    drain(out, sizeof out);
    CHECK(strcmp(out, "\xE4\xBD\xA0") == 0, "936: U+4F60, got '%s'", out);
    CHECK(xbox_InputTextWin32Ansi(949, 0xC7, 1) == 0, "949: the lead byte waits");
    CHECK(xbox_InputTextWin32Ansi(949, 0xD1, 1) == 1, "949: the trail completes it");
    drain(out, sizeof out);
    CHECK(strcmp(out, "\xED\x95\x9C") == 0, "949: U+D55C, got '%s'", out);

    /* A lead byte does not pair across a focus loss or a new field. */
    xbox_InputTextWin32Ansi(932, 0x82, 1);
    xbox_InputTextFocusLost();
    xbox_InputTextWin32Ansi(932, 0xA0, 1);
    xbox_InputTextWin32Ansi(932, 0x82, 1);
    xbox_InputTextBegin(0, 0, 2, 2);
    xbox_InputTextWin32Ansi(932, 0xA0, 1);
    drain(out, sizeof out);
    CHECK(strstr(out, "\xE3\x81\x82") == NULL, "no U+3042 from stale lead bytes");
    xbox_InputTextEnd();
}
#endif

/* ---- a producer thread against a consumer thread ------------------------ */

#define TWO_N 200000
static volatile int s_go;
static unsigned s_retries;      /* the producer's failed pushes */

static uint32_t cp_of(unsigned i)
{
    /* Every UTF-8 length, never a control character or a surrogate. */
    static const uint32_t base[4] = { 0x21, 0x100, 0x4E00, 0x1F300 };
    return base[i % 4] + (i / 4) % 64;
}

#ifdef _WIN32
static DWORD WINAPI producer(LPVOID arg)
#else
static void *producer(void *arg)
#endif
{
    unsigned i = 0;
    (void)arg;
    while (!__atomic_load_n(&s_go, __ATOMIC_ACQUIRE))
        ;
    while (i < TWO_N) {
        /* A full ring drops; retry so the stream stays in order, and count
         * the retries the ring reported. */
        if (xbox_InputTextPushCodepoint(cp_of(i)))
            i++;
        else
            s_retries++;
    }
    return 0;
}

static uint32_t decode(const unsigned char *s, int len)
{
    switch (len) {
    case 1: return s[0];
    case 2: return ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    case 3: return ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6)
                   | (s[2] & 0x3F);
    default: return ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12)
                    | ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
}

static void test_two_threads(void)
{
    unsigned got = 0, bad = 0;
    char cp[8];
#ifdef _WIN32
    HANDLE t;
#else
    pthread_t t;
#endif

    xbox_InputTextReset();
    s_retries = 0;
    xbox_InputTextBegin(0, 0, 1, 1);
#ifdef _WIN32
    t = CreateThread(NULL, 0, producer, NULL, 0, NULL);
    CHECK(t != NULL, "thread");
#else
    CHECK(pthread_create(&t, NULL, producer, NULL) == 0, "thread");
#endif
    __atomic_store_n(&s_go, 1, __ATOMIC_RELEASE);
    while (got < TWO_N) {
        int len = xbox_InputTextPop(cp, sizeof cp);
        if (len <= 0)
            continue;
        if (decode((const unsigned char *)cp, len) != cp_of(got) && bad++ < 5)
            printf("  at %u: got U+%X, want U+%X\n", got,
                   decode((const unsigned char *)cp, len), cp_of(got));
        got++;
    }
#ifdef _WIN32
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
#else
    pthread_join(t, NULL);
#endif
    CHECK(bad == 0, "%u of %u codepoints out of order or torn", bad, got);
    CHECK(xbox_InputTextPop(cp, sizeof cp) == 0, "nothing extra");
    /* The gate stays open and every codepoint is valid, so each failed
     * push was a full ring, and each is counted once. */
    CHECK(xbox_InputTextDropped() == s_retries, "dropped %u, retried %u",
          xbox_InputTextDropped(), s_retries);
    printf("  two threads: %u codepoints, %u full-ring drops retried\n",
           got, s_retries);
    xbox_InputTextEnd();
}


/* ---- Begin and End while a producer pushes ------------------------------ */

/* The guest's thread opens and closes fields while the window's thread
 * types into them and a third thread reads the request as the sync does.
 * Each codepoint is a sequence number. After a push the typist reads the
 * tag its push wrote (it owns s_head and the slots' tags, so that is the
 * state the push ran under) into s_pushed_gen; the consumer records the
 * generation it popped each one under. Every pop must match its push: a
 * codepoint typed into one field never comes out of another, however long
 * the typist stalls inside a push. */

#define TOGGLES 20000
#define SEQ_MAX 0x100000u       /* cp = 0x10000 + seq stays a scalar */
static int s_stop;
static unsigned s_rect_bad, s_reads;
static unsigned s_pushed_gen[SEQ_MAX], s_popped_gen[SEQ_MAX];
static unsigned char s_was_popped[SEQ_MAX];

static void rect_of(unsigned g, int r[4])
{
    r[0] = (int)(g & 0xFFFF);
    r[1] = (int)((g * 7) & 0xFFFF);
    r[2] = (int)((g * 13) & 0xFFFF);
    r[3] = (int)((g * 31) & 0xFFFF);
}

#ifdef _WIN32
static DWORD WINAPI typist(LPVOID arg)
#else
static void *typist(void *arg)
#endif
{
    unsigned seq = 0, i;
    (void)arg;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE) && seq < SEQ_MAX) {
        /* A short stall now and then, so a Begin or End falls between
         * reading the state and publishing the slot. */
        for (i = 0; i < (seq & 15); i++)
            __atomic_signal_fence(__ATOMIC_SEQ_CST);
        if (xbox_InputTextPushCodepoint(0x10000u + seq)) {
            s_pushed_gen[seq] = s_tag[(s_head - 1) % XBOX_TEXT_RING] >> 1;
            seq++;
        }
    }
    return 0;
}

#ifdef _WIN32
static DWORD WINAPI syncer(LPVOID arg)
#else
static void *syncer(void *arg)
#endif
{
    (void)arg;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) {
        int on, r[4], want[4];
        unsigned g = xbox_InputTextRequest(&on, r);
        s_reads++;
        if (!on)
            continue;
        rect_of(g, want);
        if (memcmp(r, want, sizeof want) != 0 && s_rect_bad++ < 5)
            printf("  gen %u: rect %d,%d %dx%d, want %d,%d %dx%d\n", g,
                   r[0], r[1], r[2], r[3], want[0], want[1], want[2], want[3]);
    }
    return 0;
}

static void test_toggle(void)
{
    unsigned k, popped = 0, stale = 0, while_off = 0, gen_bad = 0, seq;
    char cp[8];
#ifdef _WIN32
    HANDLE t1, t2;
#else
    pthread_t t1, t2;
#endif

    xbox_InputTextReset();
    s_stop = 0;
    s_rect_bad = s_reads = 0;
#ifdef _WIN32
    t1 = CreateThread(NULL, 0, typist, NULL, 0, NULL);
    t2 = CreateThread(NULL, 0, syncer, NULL, 0, NULL);
    CHECK(t1 != NULL && t2 != NULL, "threads");
#else
    CHECK(pthread_create(&t1, NULL, typist, NULL) == 0, "thread");
    CHECK(pthread_create(&t2, NULL, syncer, NULL) == 0, "thread");
#endif
    for (k = 0; k < TOGGLES; k++) {
        unsigned g = xbox_InputTextRequest(NULL, NULL) + 1, cur;
        int r[4], n, len;

        rect_of(g, r);
        xbox_InputTextBegin(r[0], r[1], r[2], r[3]);
        cur = xbox_InputTextRequest(NULL, NULL);
        if (cur != g)
            gen_bad++;
        /* Long enough for the typist to get some codepoints in. */
        for (n = 0; n < 400; n++) {
            len = xbox_InputTextPop(cp, sizeof cp);
            if (len == 4) {
                seq = decode((const unsigned char *)cp, len) - 0x10000u;
                if (seq < SEQ_MAX) {
                    s_popped_gen[seq] = cur;
                    s_was_popped[seq] = 1;
                }
                popped++;
            }
        }
        xbox_InputTextEnd();
        if (xbox_InputTextPop(cp, sizeof cp) != 0)
            while_off++;
    }
    __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
#ifdef _WIN32
    WaitForSingleObject(t1, INFINITE);
    WaitForSingleObject(t2, INFINITE);
    CloseHandle(t1);
    CloseHandle(t2);
#else
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
#endif
    for (seq = 0; seq < SEQ_MAX; seq++)
        if (s_was_popped[seq] && s_popped_gen[seq] != s_pushed_gen[seq] && stale++ < 5)
            printf("  #%u: pushed under gen %u, popped under gen %u\n", seq,
                   s_pushed_gen[seq], s_popped_gen[seq]);
    CHECK(gen_bad == 0, "%u Begins not one generation on", gen_bad);
    CHECK(stale == 0, "%u of %u popped codepoints from an earlier field", stale, popped);
    CHECK(while_off == 0, "%u pops returned text with the gate closed", while_off);
    CHECK(s_rect_bad == 0, "%u of %u request reads paired a rect with another generation",
          s_rect_bad, s_reads);
    printf("  toggle: %u fields, %u codepoints popped, %u request reads\n",
           TOGGLES, popped, s_reads);
}

int main(void)
{
    test_utf8();
    test_utf16();
    test_control();
    test_overflow_wrap();
    test_gate_request();
#ifdef _WIN32
    test_win32();
#endif
    test_two_threads();
    test_toggle();
    printf("input_text: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
