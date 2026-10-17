/*
 * input_keyboard test: see CMakeLists.txt.
 *
 * The expected map below is written out independently of the table in
 * src/input/keyboard.c (it is the spec's, input-real-devices D9: arrows
 * d-pad, Enter START, Backspace BACK, Shift/Ctrl stick clicks, Z X C V =
 * A B X Y, Q E = White Black, 1 3 = triggers, W/S/A/D and keypad 8/2/4/6
 * the left stick, I/K/J/L the right), so a wrong row fails here rather than
 * being compared with itself.
 */
#include "keyboard.h"
#include "recomp_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    char buf[128];
    snprintf(buf, sizeof buf, "%s=%s", k, v ? v : "");
    _putenv(buf);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
    recomp_env_reload();   /* the runtime reads the environment once */
}

enum { F_BTN, F_ANALOG, F_STICK };
struct expect {
    int vk;
    int field;      /* F_* */
    int index;      /* wButtons bit / bAnalogButtons index / axis 0..3 LX LY RX RY */
    int value;      /* the stick value; 0 for the others */
};

static const struct expect k_expect[] = {
    { 0x26, F_BTN, 0x0001, 0 },          /* Up arrow   -> DPAD_UP */
    { 0x28, F_BTN, 0x0002, 0 },          /* Down arrow -> DPAD_DOWN */
    { 0x25, F_BTN, 0x0004, 0 },          /* Left       -> DPAD_LEFT */
    { 0x27, F_BTN, 0x0008, 0 },          /* Right      -> DPAD_RIGHT */
    { 0x0D, F_BTN, 0x0010, 0 },          /* Enter      -> START */
    { 0x08, F_BTN, 0x0020, 0 },          /* Backspace  -> BACK */
    { 0x10, F_BTN, 0x0040, 0 },          /* Shift      -> LEFT_THUMB */
    { 0x11, F_BTN, 0x0080, 0 },          /* Ctrl       -> RIGHT_THUMB */
    { 'Z',  F_ANALOG, 0, 0 },            /* A */
    { 'X',  F_ANALOG, 1, 0 },            /* B */
    { 'C',  F_ANALOG, 2, 0 },            /* X */
    { 'V',  F_ANALOG, 3, 0 },            /* Y */
    { 'E',  F_ANALOG, 4, 0 },            /* BLACK */
    { 'Q',  F_ANALOG, 5, 0 },            /* WHITE */
    { '1',  F_ANALOG, 6, 0 },            /* LTRIGGER */
    { '3',  F_ANALOG, 7, 0 },            /* RTRIGGER */
    { 'W',  F_STICK, 1,  32767 },
    { 0x68, F_STICK, 1,  32767 },        /* keypad 8 */
    { 'S',  F_STICK, 1, -32767 },
    { 0x62, F_STICK, 1, -32767 },        /* keypad 2 */
    { 'A',  F_STICK, 0, -32767 },
    { 0x64, F_STICK, 0, -32767 },        /* keypad 4 */
    { 'D',  F_STICK, 0,  32767 },
    { 0x66, F_STICK, 0,  32767 },        /* keypad 6 */
    { 'I',  F_STICK, 3,  32767 },
    { 'K',  F_STICK, 3, -32767 },
    { 'J',  F_STICK, 2, -32767 },
    { 'L',  F_STICK, 2,  32767 },
};
#define N_EXPECT (sizeof k_expect / sizeof k_expect[0])

static int stick(const XBOX_GAMEPAD *g, int a)
{
    return a == 0 ? g->sThumbLX : a == 1 ? g->sThumbLY
         : a == 2 ? g->sThumbRX : g->sThumbRY;
}

/* Everything in g is at rest except the one field e names. */
static void check_only(const XBOX_GAMEPAD *g, const struct expect *e)
{
    WORD want_btn = e->field == F_BTN ? (WORD)e->index : 0;
    CHECK(g->wButtons == want_btn, "vk 0x%02X: wButtons 0x%04X, want 0x%04X",
          e->vk, g->wButtons, want_btn);
    for (int i = 0; i < 8; i++) {
        int want = (e->field == F_ANALOG && e->index == i) ? 255 : 0;
        CHECK(g->bAnalogButtons[i] == want, "vk 0x%02X: analog[%d] %d, want %d",
              e->vk, i, g->bAnalogButtons[i], want);
    }
    for (int a = 0; a < 4; a++) {
        int want = (e->field == F_STICK && e->index == a) ? e->value : 0;
        CHECK(stick(g, a) == want, "vk 0x%02X: axis %d %d, want %d",
              e->vk, a, stick(g, a), want);
    }
}

/* ── 1. every key alone, and nothing else moves ─────────────────────────── */

static void test_rows(void)
{
    XBOX_INPUT_STATE st;
    DWORD last;

    xbox_InputKeysClear();
    xbox_InputKeyboardState(&st);
    last = st.dwPacketNumber;
    {
        struct expect rest = { 0, F_BTN, 0, 0 };
        check_only(&st.Gamepad, &rest);
    }

    for (size_t i = 0; i < N_EXPECT; i++) {
        xbox_InputKeysClear();
        xbox_InputKeySet(k_expect[i].vk, 1);
        CHECK(xbox_FramebufferKeyDown(k_expect[i].vk), "vk 0x%02X not held", k_expect[i].vk);
        xbox_InputKeyboardState(&st);
        check_only(&st.Gamepad, &k_expect[i]);
        CHECK(st.dwPacketNumber != last, "packet number did not move");
        last = st.dwPacketNumber;
    }

    /* Every row of the code is one of the expected keys, and the code has
     * a row for every expected key. */
    {
        size_t keys = 0;
        for (size_t r = 0; r < xbox_key_map_count; r++)
            keys += xbox_key_map[r].vk[1] ? 2 : 1;
        CHECK(keys == N_EXPECT, "map has %u keys, expected %u",
              (unsigned)keys, (unsigned)N_EXPECT);
    }
}

/* ── 2. no key on two rows; keys outside the map do nothing ──────────────── */

static void test_unique(void)
{
    int seen[256] = { 0 };
    XBOX_INPUT_STATE st;
    struct expect rest = { 0, F_BTN, 0, 0 };

    for (size_t r = 0; r < xbox_key_map_count; r++)
        for (int k = 0; k < 2; k++) {
            int vk = xbox_key_map[r].vk[k];
            if (!vk) continue;
            CHECK(!seen[vk], "vk 0x%02X is on two rows", vk);
            seen[vk] = 1;
        }

    for (int vk = 1; vk < 256; vk++) {
        if (seen[vk]) continue;
        xbox_InputKeysClear();
        xbox_InputKeySet(vk, 1);
        xbox_InputKeyboardState(&st);
        rest.vk = vk;
        check_only(&st.Gamepad, &rest);
    }
}

/* ── 3. combinations, release, clear, range ────────────────────────────── */

static void test_combos(void)
{
    XBOX_INPUT_STATE st;

    /* Opposite directions cancel; both rows of one direction are one. */
    xbox_InputKeysClear();
    xbox_InputKeySet('A', 1);
    xbox_InputKeySet('D', 1);
    xbox_InputKeySet('W', 1);
    xbox_InputKeySet(0x68, 1);
    xbox_InputKeyboardState(&st);
    CHECK(st.Gamepad.sThumbLX == 0, "A+D: LX %d, want 0", st.Gamepad.sThumbLX);
    CHECK(st.Gamepad.sThumbLY == 32767, "W+kp8: LY %d, want 32767", st.Gamepad.sThumbLY);

    /* Release one of W and keypad 8: still up. Release both: rest. */
    xbox_InputKeySet('W', 0);
    xbox_InputKeyboardState(&st);
    CHECK(st.Gamepad.sThumbLY == 32767, "kp8 alone: LY %d", st.Gamepad.sThumbLY);
    xbox_InputKeySet(0x68, 0);
    xbox_InputKeyboardState(&st);
    CHECK(st.Gamepad.sThumbLY == 0, "released: LY %d", st.Gamepad.sThumbLY);

    /* Buttons together. */
    xbox_InputKeysClear();
    xbox_InputKeySet(0x0D, 1);
    xbox_InputKeySet(0x26, 1);
    xbox_InputKeySet('Z', 1);
    xbox_InputKeyboardState(&st);
    CHECK(st.Gamepad.wButtons == (0x0010 | 0x0001), "Enter+Up: 0x%04X", st.Gamepad.wButtons);
    CHECK(st.Gamepad.bAnalogButtons[0] == 255, "Z: A %d", st.Gamepad.bAnalogButtons[0]);

    /* Clear releases everything. */
    xbox_InputKeysClear();
    for (int vk = 0; vk < 256; vk++)
        CHECK(!xbox_FramebufferKeyDown(vk), "vk 0x%02X held after Clear", vk);

    /* Out of range is ignored, both ways. */
    xbox_InputKeySet(-1, 1);
    xbox_InputKeySet(256, 1);
    CHECK(!xbox_FramebufferKeyDown(-1) && !xbox_FramebufferKeyDown(256), "out of range");
}

/* ── 4. RECOMP_KEYBOARD gates the backend hook ─────────────────────────── */

static void test_enable(void)
{
    XBOX_INPUT_STATE st;

    xbox_InputKeysClear();
    xbox_InputKeySet(0x0D, 1);
    set_env("RECOMP_KEYBOARD", NULL);
    CHECK(!xbox_InputKeyboardPoll(&st), "unset: poll answered");
    set_env("RECOMP_KEYBOARD", "0");
    CHECK(!xbox_InputKeyboardPoll(&st), "=0: poll answered");
    set_env("RECOMP_KEYBOARD", "1");
    memset(&st, 0, sizeof st);
    CHECK(xbox_InputKeyboardPoll(&st), "=1: poll did not answer");
    CHECK(st.Gamepad.wButtons == 0x0010, "=1: wButtons 0x%04X", st.Gamepad.wButtons);
    set_env("RECOMP_KEYBOARD", NULL);
    xbox_InputKeysClear();
}

/* ── 5. README ─────────────────────────────────────────────────────────── */

static void test_readme(const char *path)
{
    char line[512], want[512];
    FILE *f;
    size_t row = 0;
    int in = 0, found = 0;
    const char *out = getenv("INPUT_KEYBOARD_README_OUT");

    if (out) {
        FILE *o = fopen(out, "w");
        if (o) {
            fprintf(o, "%s\n", XBOX_KEY_MAP_README_BEGIN);
            for (size_t i = 0; xbox_InputKeyMapReadmeRow(i, want, sizeof want); i++)
                fprintf(o, "%s\n", want);
            fprintf(o, "%s\n", XBOX_KEY_MAP_README_END);
            fclose(o);
        }
    }

    f = fopen(path, "r");
    CHECK(f != NULL, "cannot open %s", path);
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!in) {
            if (!strcmp(line, XBOX_KEY_MAP_README_BEGIN)) in = found = 1;
            continue;
        }
        if (!strcmp(line, XBOX_KEY_MAP_README_END)) { in = 0; break; }
        if (!xbox_InputKeyMapReadmeRow(row, want, sizeof want)) {
            CHECK(0, "README has an extra key line: %s", line);
            break;
        }
        CHECK(!strcmp(line, want), "README key line %u:\n  have: %s\n  want: %s",
              (unsigned)row, line, want);
        row++;
    }
    fclose(f);
    CHECK(found, "README has no '%s' marker", XBOX_KEY_MAP_README_BEGIN);
    CHECK(!in, "README key table is not closed by '%s'", XBOX_KEY_MAP_README_END);
    CHECK(!xbox_InputKeyMapReadmeRow(row, want, sizeof want),
          "README key table is missing line %u: %s", (unsigned)row, want);
}

int main(int argc, char **argv)
{
    test_rows();
    test_unique();
    test_combos();
    test_enable();
    test_readme(argc > 1 ? argv[1] : "input_README.md");
    printf("input_keyboard: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
