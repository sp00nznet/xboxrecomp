/*
 * input_bindings test: see CMakeLists.txt.
 *
 * The expected stick values are worked out here from the design's formulas
 * (kbm-input D6 and D7), not read back from the code: the ramp is
 * 0.41 + 0.59 * t / ramp of full deflection, a diagonal is 1/sqrt 2 of it,
 * and a mouse axis is sign * (min + (1 - min) * min(1, counts * sens / 40))
 * of full deflection.
 */
#include "input_backend.h"
#include "input_names.h"
#include "keyboard.h"
#include "mouse.h"
#include "recomp_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>        /* the window messages of the focus-click case */
#endif

static int g_fail, g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b) (abs((int)(a) - (int)(b)) <= 1)

static uint64_t f_now;
static uint64_t f_clock(void) { return f_now; }

static int code_of(const char *name)
{
    int code = 0, mouse = 0;
    xbox_InputNameToCode(name, &code, &mouse);
    return code;
}

/* Back to a known start: no keys, the compiled-in table, no ramp, the mouse
 * released with the code defaults, no script, the fake clock at 1000. */
static void fresh(void)
{
    xbox_InputKeysClear();
    xbox_InputBindingsReset();
    xbox_InputKeyboardOpts(0, 0, 1.0);
    xbox_InputMouseOpts(NULL);
    xbox_InputMouseReset();
    xbox_InputKeyboardEnable(1);
    xbox_InputMouseEnable(1);
    xbox_InputKbmScriptReset();
    xbox_InputKbmSetClock(f_clock);
    f_now = 1000;
}

static void bind_only(int control, const char *name)
{
    int code = 0, mouse = 0;
    xbox_InputNameToCode(name, &code, &mouse);
    xbox_InputBindingsClear(control);
    CHECK(xbox_InputBind(control, code, mouse) == 0, "bind %s", name);
}

static XBOX_INPUT_STATE state(int keys, int mouse)
{
    XBOX_INPUT_STATE s;
    xbox_InputKbmState(&s, keys, mouse);
    return s;
}

/* ── 1. names ────────────────────────────────────────────────────────── */

static void test_names(void)
{
    const char *name, *other;
    int code, mouse, alias, c2, m2, a2, n = 0;

    for (int i = 0; xbox_InputNameAt(i, &name, &code, &mouse, &alias); i++, n++) {
        int got = -1, got_mouse = -1;
        CHECK(xbox_InputNameToCode(name, &got, &got_mouse) && got == code
              && got_mouse == mouse, "%s does not look up to its own code", name);
        if (alias)
            continue;
        CHECK(xbox_InputCodeName(code, mouse) && !strcmp(xbox_InputCodeName(code, mouse), name),
              "%s: code 0x%02X names %s", name, code,
              xbox_InputCodeName(code, mouse) ? xbox_InputCodeName(code, mouse) : "(null)");
        for (int j = i + 1; xbox_InputNameAt(j, &other, &c2, &m2, &a2); j++)
            CHECK(a2 || c2 != code || m2 != mouse, "%s and %s share code 0x%02X",
                  name, other, code);
    }
    CHECK(n > 100, "only %d names", n);

    /* Aliases and case. */
    CHECK(code_of("Enter") == 0x0D && code_of("Esc") == 0x1B, "Enter, Esc");
    CHECK(!strcmp(xbox_InputCodeName(0x0D, 0), "Return")
          && !strcmp(xbox_InputCodeName(0x1B, 0), "Escape"), "canonical names win");
    CHECK(code_of("return") == 0x0D && code_of("SPACE") == 0x20 && code_of("lalt") == 0xA4,
          "lookup is case-insensitive");
    CHECK(code_of("Shift") == 0x10 && code_of("Ctrl") == 0x11 && code_of("Alt") == 0x12,
          "either-side names");
    CHECK(code_of("LShift") == XBOX_VK_LSHIFT && code_of("RAlt") == XBOX_VK_RMENU
          && code_of("KPEnter") == XBOX_VK_KP_ENTER, "sided codes");
    CHECK(xbox_InputNameToCode("MouseLeft", &code, &mouse) && mouse && code == XBOX_MOUSE_LEFT
          && xbox_InputNameToCode("WheelDown", &code, &mouse) && mouse
          && code == XBOX_MOUSE_WHEEL_DOWN, "mouse names");
    /* The system's keys are not the game's. */
    CHECK(!xbox_InputNameToCode("LGui", NULL, NULL) && !xbox_InputNameToCode("RGui", NULL, NULL),
          "LGui/RGui accepted");
    CHECK(!xbox_InputNameToCode("Nope", NULL, NULL) && !xbox_InputNameToCode("", NULL, NULL)
          && !xbox_InputNameToCode(NULL, NULL, NULL), "unknown name accepted");

    /* Controls. */
    static const char *const ctl[XBOX_CTL_COUNT] = {
        "a", "b", "x", "y", "white", "black", "ltrigger", "rtrigger",
        "start", "back", "lthumb", "rthumb",
        "dpad_up", "dpad_down", "dpad_left", "dpad_right",
        "lstick_up", "lstick_down", "lstick_left", "lstick_right",
        "rstick_up", "rstick_down", "rstick_left", "rstick_right",
    };
    for (int c = 0; c < XBOX_CTL_COUNT; c++) {
        CHECK(xbox_InputControlFromName(ctl[c]) == c, "control %s", ctl[c]);
        CHECK(xbox_InputControlName(c) && !strcmp(xbox_InputControlName(c), ctl[c]),
              "control %d name", c);
    }
    CHECK(xbox_InputControlFromName("START") == XBOX_CTL_START, "control case");
    CHECK(xbox_InputControlFromName("select") == -1 && !xbox_InputControlName(XBOX_CTL_COUNT),
          "unknown control");
}

/* ── 2. bindings ─────────────────────────────────────────────────────── */

static void test_bindings(void)
{
    int codes[XBOX_BIND_MAX], mouse[XBOX_BIND_MAX];

    fresh();
    /* The runtime table starts as the compiled-in map: Return is Start. */
    int n = xbox_InputBindingsOf(XBOX_CTL_START, codes, mouse, XBOX_BIND_MAX);
    int has_return = 0;
    for (int i = 0; i < n; i++)
        has_return |= codes[i] == 0x0D && !mouse[i];
    CHECK(has_return, "compiled-in Start has no Return");

    xbox_InputBindingsClear(XBOX_CTL_A);
    CHECK(xbox_InputBindingsOf(XBOX_CTL_A, NULL, NULL, 0) == 0, "clear");
    CHECK(xbox_InputBind(XBOX_CTL_A, 0x20, 0) == 0
          && xbox_InputBind(XBOX_CTL_A, XBOX_MOUSE_LEFT, 1) == 0, "bind");
    n = xbox_InputBindingsOf(XBOX_CTL_A, codes, mouse, XBOX_BIND_MAX);
    CHECK(n == 2 && codes[0] == 0x20 && !mouse[0] && codes[1] == XBOX_MOUSE_LEFT && mouse[1],
          "bindings of a: %d", n);
    CHECK(xbox_InputBind(XBOX_CTL_COUNT, 0x20, 0) == -1 && xbox_InputBind(XBOX_CTL_A, 0, 0) == -1
          && xbox_InputBind(XBOX_CTL_A, 8, 1) == -1 && xbox_InputBind(XBOX_CTL_A, 256, 0) == -1,
          "out-of-range binds accepted");
    for (int i = 2; i < XBOX_BIND_MAX; i++)
        xbox_InputBind(XBOX_CTL_A, 0x41 + i, 0);
    CHECK(xbox_InputBind(XBOX_CTL_A, 0x5A, 0) == -1, "a ninth input accepted");
    xbox_InputBindingsReset();
    n = xbox_InputBindingsOf(XBOX_CTL_A, codes, mouse, XBOX_BIND_MAX);
    CHECK(n > 0 && codes[0] != 0x20, "reset did not restore the compiled-in a");
}

/* ── 3. the composer ─────────────────────────────────────────────────── */

static void test_compiled_rows(void)
{
    /* Each compiled-in row through the runtime table with the ramp off is
     * exactly what the legacy call makes. */
    fresh();
    for (size_t r = 0; r < xbox_key_map_count; r++)
        for (int k = 0; k < 2; k++) {
            XBOX_INPUT_STATE a, b;
            int vk = xbox_key_map[r].vk[k];
            if (!vk)
                continue;
            xbox_InputKeysClear();
            xbox_InputKeySet(vk, 1);
            a = state(1, 0);
            xbox_InputKeyboardState(&b);
            a.dwPacketNumber = b.dwPacketNumber = 0;
            CHECK(!memcmp(&a, &b, sizeof a), "row %s (vk 0x%02X) differs",
                  xbox_key_map[r].keys, vk);
            CHECK(memcmp(&a.Gamepad, &(XBOX_GAMEPAD){0}, sizeof a.Gamepad),
                  "row %s (vk 0x%02X) drives nothing", xbox_key_map[r].keys, vk);
        }
    xbox_InputKeysClear();
}

static void test_ramp(void)
{
    XBOX_INPUT_STATE s;

    fresh();
    bind_only(XBOX_CTL_LSTICK_UP, "W");
    bind_only(XBOX_CTL_LSTICK_RIGHT, "D");
    bind_only(XBOX_CTL_LSTICK_DOWN, "S");
    bind_only(XBOX_CTL_A, "Space");
    xbox_InputKeyboardOpts(60, code_of("LAlt"), 0.5);

    s = state(1, 0);
    CHECK(s.Gamepad.sThumbLY == 0 && s.Gamepad.sThumbLX == 0, "at rest");

    xbox_InputKeyEvent(code_of("W"), 1);
    s = state(1, 0);
    CHECK(NEAR(s.Gamepad.sThumbLY, 13434), "W at 0 ms: %d", s.Gamepad.sThumbLY);
    f_now += 30;
    s = state(1, 0);
    CHECK(NEAR(s.Gamepad.sThumbLY, 23101), "W at 30 ms: %d", s.Gamepad.sThumbLY);
    f_now += 30;
    s = state(1, 0);
    CHECK(s.Gamepad.sThumbLY == 32767, "W at 60 ms: %d", s.Gamepad.sThumbLY);
    f_now += 500;
    s = state(1, 0);
    CHECK(s.Gamepad.sThumbLY == 32767, "W held: %d", s.Gamepad.sThumbLY);

    /* The opposite key cancels; releasing it starts the ramp again. */
    xbox_InputKeyEvent(code_of("S"), 1);
    s = state(1, 0);
    CHECK(s.Gamepad.sThumbLY == 0, "W + S: %d", s.Gamepad.sThumbLY);
    xbox_InputKeyEvent(code_of("S"), 0);
    s = state(1, 0);
    CHECK(NEAR(s.Gamepad.sThumbLY, 13434), "W after S up: %d", s.Gamepad.sThumbLY);

    /* The diagonal, past the ramp. */
    xbox_InputKeyEvent(code_of("D"), 1);
    s = state(1, 0);                     /* the ramp runs from this poll */
    f_now += 100;
    s = state(1, 0);
    CHECK(NEAR(s.Gamepad.sThumbLX, 23170) && NEAR(s.Gamepad.sThumbLY, 23170),
          "W + D: (%d, %d)", s.Gamepad.sThumbLX, s.Gamepad.sThumbLY);
    xbox_InputKeyEvent(code_of("D"), 0);

    /* The walk key. */
    xbox_InputKeyEvent(code_of("LAlt"), 1);
    f_now += 100;
    s = state(1, 0);
    CHECK(NEAR(s.Gamepad.sThumbLY, 16383), "W + LAlt: %d", s.Gamepad.sThumbLY);
    xbox_InputKeyEvent(code_of("LAlt"), 0);
    xbox_InputKeyEvent(code_of("W"), 0);

    /* A button key. */
    xbox_InputKeyEvent(code_of("Space"), 1);
    s = state(1, 0);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255, "Space: A=%d",
          s.Gamepad.bAnalogButtons[XBOX_BUTTON_A]);
    xbox_InputKeyEvent(code_of("Space"), 0);

    /* The ramp off: W + D is full on both axes, as the compiled-in map. */
    xbox_InputKeyboardOpts(0, 0, 1.0);
    xbox_InputKeyEvent(code_of("W"), 1);
    xbox_InputKeyEvent(code_of("D"), 1);
    s = state(1, 0);
    CHECK(s.Gamepad.sThumbLX == 32767 && s.Gamepad.sThumbLY == 32767,
          "W + D, no ramp: (%d, %d)", s.Gamepad.sThumbLX, s.Gamepad.sThumbLY);

    /* The packet moves on every call. */
    {
        DWORD p = s.dwPacketNumber;
        s = state(1, 0);
        CHECK(s.dwPacketNumber == p + 1, "packet %u after %u", (unsigned)s.dwPacketNumber,
              (unsigned)p);
    }
    xbox_InputKeysClear();
}

static void test_sides(void)
{
    XBOX_INPUT_STATE s;

    fresh();
    /* A binding of the either-side name hears both keys; a sided one only
     * its own. */
    bind_only(XBOX_CTL_X, "Shift");
    bind_only(XBOX_CTL_Y, "RCtrl");
    xbox_InputKeyEvent(XBOX_VK_RSHIFT, 1);
    xbox_InputKeyEvent(XBOX_VK_LCONTROL, 1);
    s = state(1, 0);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_X] == 255 && !s.Gamepad.bAnalogButtons[XBOX_BUTTON_Y],
          "Shift by RShift, RCtrl not by LCtrl");
    xbox_InputKeyEvent(XBOX_VK_RCONTROL, 1);
    xbox_InputKeyEvent(XBOX_VK_LCONTROL, 0);
    s = state(1, 0);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_Y] == 255, "RCtrl");
    xbox_InputKeysClear();

    /* Win32 messages: the scancode and the extended bit pick the side. */
    xbox_InputKeyEventWin32(0x10, 0x36ul << 16, 1);
    CHECK(xbox_FramebufferKeyDown(XBOX_VK_RSHIFT) && xbox_FramebufferKeyDown(0x10),
          "Win32 RShift");
    /* Both Shifts held: Windows sends one key-up, for the last released. */
    xbox_InputKeyEventWin32(0x10, 0x2Aul << 16, 1);
    CHECK(xbox_FramebufferKeyDown(XBOX_VK_LSHIFT), "Win32 LShift");
    xbox_InputKeyEventWin32(0x10, 0x36ul << 16, 0);
    CHECK(!xbox_FramebufferKeyDown(XBOX_VK_LSHIFT) && !xbox_FramebufferKeyDown(XBOX_VK_RSHIFT)
              && !xbox_FramebufferKeyDown(0x10),
          "Win32 one Shift up releases both sides");
    xbox_InputKeyEventWin32(0x12, 1ul << 24, 1);
    CHECK(xbox_FramebufferKeyDown(XBOX_VK_RMENU), "Win32 RAlt");
    xbox_InputKeyEventWin32(0x0D, 1ul << 24, 1);
    CHECK(xbox_FramebufferKeyDown(XBOX_VK_KP_ENTER) && xbox_FramebufferKeyDown(0x0D),
          "Win32 keypad Enter holds Return too");
    xbox_InputKeyEventWin32(0x0D, 1ul << 24, 0);
    CHECK(!xbox_FramebufferKeyDown(0x0D), "keypad Enter up releases Return");
    xbox_InputKeysClear();
    CHECK(!xbox_FramebufferKeyDown(0x10) && !xbox_FramebufferKeyDown(XBOX_VK_RSHIFT), "clear");
}

static void test_wheel(void)
{
    XBOX_INPUT_STATE s;

    fresh();
    bind_only(XBOX_CTL_WHITE, "WheelUp");
    bind_only(XBOX_CTL_BLACK, "WheelDown");
    xbox_InputMouseFocus(0);
    CHECK(xbox_InputMouseCaptured(), "focus did not capture");

    xbox_InputMouseWheel(1);
    s = state(1, 1);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] == 255, "notch not held");
    f_now += 99;
    s = state(1, 1);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] == 255, "notch released early");
    f_now += 1;
    s = state(1, 1);
    CHECK(!s.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE], "notch held past wheel_pulse_ms");
    f_now += 10;
    s = state(1, 1);
    CHECK(!s.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE], "a second press from one notch");

    /* Two notches are two presses with a release between them. */
    int presses = 0, prev = 0;
    xbox_InputMouseWheel(2);
    for (int i = 0; i < 40; i++, f_now += 10) {
        s = state(1, 1);
        int on = s.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] != 0;
        presses += on && !prev;
        prev = on;
    }
    CHECK(presses == 2, "two notches gave %d presses", presses);

    xbox_InputMouseWheel(-1);
    s = state(1, 1);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] == 255
          && !s.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE], "wheel down");
}

/* ── 4. the mouse ────────────────────────────────────────────────────── */

static void test_mouse_model(void)
{
    XBOX_INPUT_STATE s;
    xbox_mouse_opts o;

    CHECK(xbox_InputMouseModel(0, 1.0, 0.45) == 0, "0 counts");
    CHECK(NEAR(xbox_InputMouseModel(1, 1.0, 0.45), 15196), "1 count: %d",
          xbox_InputMouseModel(1, 1.0, 0.45));
    CHECK(NEAR(xbox_InputMouseModel(20, 1.0, 0.45), 23756), "20 counts: %d",
          xbox_InputMouseModel(20, 1.0, 0.45));
    CHECK(xbox_InputMouseModel(40, 1.0, 0.45) == 32767, "40 counts");
    CHECK(xbox_InputMouseModel(80, 1.0, 0.45) == 32767, "80 counts");
    CHECK(NEAR(xbox_InputMouseModel(-20, 1.0, 0.45), -23756), "-20 counts");
    CHECK(xbox_InputMouseModel(20, 2.0, 0.45) == 32767
          && xbox_InputMouseModel(10, 2.0, 0.45) == xbox_InputMouseModel(20, 1.0, 0.45),
          "sensitivity 2.0 halves the counts");

    /* Through the composer: right stick, mouse forward is stick up. */
    fresh();
    xbox_InputMouseFocus(0);
    xbox_InputMouseMotion(20, -20);
    s = state(1, 1);
    CHECK(NEAR(s.Gamepad.sThumbRX, 23756) && NEAR(s.Gamepad.sThumbRY, 23756)
          && !s.Gamepad.sThumbLX && !s.Gamepad.sThumbLY,
          "RS (%d, %d)", s.Gamepad.sThumbRX, s.Gamepad.sThumbRY);
    s = state(1, 1);
    CHECK(!s.Gamepad.sThumbRX && !s.Gamepad.sThumbRY, "at rest the next poll");

    xbox_InputMouseDefaultOpts(&o);
    o.invert_y = 1;
    xbox_InputMouseOpts(&o);
    xbox_InputMouseFocus(0);
    xbox_InputMouseMotion(20, -20);
    s = state(1, 1);
    CHECK(NEAR(s.Gamepad.sThumbRX, 23756) && NEAR(s.Gamepad.sThumbRY, -23756), "invert_y");
    o.invert_y = 0;
    o.invert_x = 1;
    xbox_InputMouseOpts(&o);
    xbox_InputMouseMotion(20, -20);
    s = state(1, 1);
    CHECK(NEAR(s.Gamepad.sThumbRX, -23756) && NEAR(s.Gamepad.sThumbRY, 23756), "invert_x");

    o.invert_x = 0;
    o.look = XBOX_MOUSE_LOOK_LEFT;
    xbox_InputMouseOpts(&o);
    xbox_InputMouseMotion(40, 0);
    s = state(1, 1);
    CHECK(s.Gamepad.sThumbLX == 32767 && !s.Gamepad.sThumbRX, "left_stick: LX %d RX %d",
          s.Gamepad.sThumbLX, s.Gamepad.sThumbRX);
    o.look = XBOX_MOUSE_LOOK_NONE;
    xbox_InputMouseOpts(&o);
    xbox_InputMouseMotion(40, 40);
    s = state(1, 1);
    CHECK(!s.Gamepad.sThumbLX && !s.Gamepad.sThumbLY && !s.Gamepad.sThumbRX
          && !s.Gamepad.sThumbRY, "look none moved a stick");

    /* The mouse axis replaces the keys' for the poll it moved in. */
    xbox_InputMouseOpts(NULL);
    bind_only(XBOX_CTL_RSTICK_RIGHT, "L");
    xbox_InputKeyEvent(code_of("L"), 1);
    xbox_InputMouseMotion(-20, 0);
    s = state(1, 1);
    CHECK(NEAR(s.Gamepad.sThumbRX, -23756), "mouse over key: %d", s.Gamepad.sThumbRX);
    s = state(1, 1);
    CHECK(s.Gamepad.sThumbRX == 32767, "key after the mouse: %d", s.Gamepad.sThumbRX);
    xbox_InputKeysClear();
}

static void test_capture(void)
{
    xbox_mouse_opts o;
    XBOX_INPUT_STATE s;

    /* focus, the default. */
    fresh();
    bind_only(XBOX_CTL_B, "MouseLeft");
    CHECK(!xbox_InputMouseCaptured(), "captured before focus");
    xbox_InputMouseMotion(40, 0);
    xbox_InputMouseFocus(0);
    s = state(1, 1);
    CHECK(xbox_InputMouseCaptured() && !s.Gamepad.sThumbRX,
          "focus gained: motion from before the capture leaked");
    xbox_InputMouseFocus(1);
    CHECK(!xbox_InputMouseCaptured(), "focus lost kept the capture");
    xbox_InputMouseFocus(0);
    CHECK(xbox_InputMouseCaptured(), "focus regained");
    xbox_InputMouseSuspend(1);
    CHECK(!xbox_InputMouseCaptured(), "Suspend(1) kept the capture");
    xbox_InputMouseSuspend(0);
    CHECK(xbox_InputMouseCaptured(), "Suspend(0)");

    /* The release key toggles; buttons while free drive nothing. */
    xbox_InputKeyEvent(0x78, 1);
    xbox_InputKeyEvent(0x78, 0);
    CHECK(!xbox_InputMouseCaptured(), "F9 did not free");
    xbox_InputMouseButton(XBOX_MOUSE_LEFT, 1);
    s = state(1, 1);
    CHECK(!s.Gamepad.bAnalogButtons[XBOX_BUTTON_B], "a press while free drove B");
    xbox_InputMouseButton(XBOX_MOUSE_LEFT, 0);
    xbox_InputKeyEvent(0x78, 1);
    xbox_InputKeyEvent(0x78, 0);
    CHECK(xbox_InputMouseCaptured(), "F9 again did not grab");
    xbox_InputMouseButton(XBOX_MOUSE_LEFT, 1);
    s = state(1, 1);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 255, "a press while captured");
    xbox_InputMouseButton(XBOX_MOUSE_LEFT, 0);
    /* The click that activates the window is not a press of B. */
    xbox_InputMouseFocus(1);
    xbox_InputMouseActivateClick();
    xbox_InputMouseFocus(0);
    xbox_InputMouseButton(XBOX_MOUSE_LEFT, 1);
    s = state(1, 1);
    CHECK(xbox_InputMouseCaptured() && !s.Gamepad.bAnalogButtons[XBOX_BUTTON_B],
          "the activating click drove B");
    xbox_InputMouseButton(XBOX_MOUSE_LEFT, 0);
    xbox_InputMouseButton(XBOX_MOUSE_LEFT, 1);
    s = state(1, 1);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 255, "the press after it");
    /* Release clears what was held. */
    xbox_InputMouseFocus(1);
    s = state(1, 1);
    CHECK(!s.Gamepad.bAnalogButtons[XBOX_BUTTON_B], "focus lost left B held");
    /* The release key alone never grabs without focus. */
    xbox_InputKeyEvent(0x78, 1);
    xbox_InputKeyEvent(0x78, 0);
    CHECK(!xbox_InputMouseCaptured(), "F9 grabbed an unfocused window");

    /* A host that only says "focus gained" (SDL on X11 and Wayland, Wine
     * under a click-to-focus window manager): the first press within
     * XBOX_MOUSE_FOCUS_CLICK_MS is the focusing click and drives nothing;
     * one later, or the second, drives. The clock wraps. */
    fresh();
    bind_only(XBOX_CTL_RTRIGGER, "MouseLeft");
    xbox_InputMouseFocusGainedAt(5000);
    xbox_InputMouseButtonAt(XBOX_MOUSE_LEFT, 1, 5010);
    s = state(1, 1);
    CHECK(xbox_InputMouseCaptured() && !s.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER],
          "the focusing click drove RT");
    xbox_InputMouseButtonAt(XBOX_MOUSE_LEFT, 0, 5060);
    xbox_InputMouseButtonAt(XBOX_MOUSE_LEFT, 1, 5100);
    s = state(1, 1);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255, "the press after the focusing click");
    xbox_InputMouseButtonAt(XBOX_MOUSE_LEFT, 0, 5150);
    xbox_InputMouseFocus(1);
    xbox_InputMouseFocusGainedAt(9000);
    xbox_InputMouseButtonAt(XBOX_MOUSE_LEFT, 1, 9000 + XBOX_MOUSE_FOCUS_CLICK_MS + 1);
    s = state(1, 1);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255, "a press well after the focus");
    xbox_InputMouseButtonAt(XBOX_MOUSE_LEFT, 0, 9400);
    xbox_InputMouseFocus(1);
    xbox_InputMouseFocusGainedAt(0xFFFFFFF0u);
    xbox_InputMouseButtonAt(XBOX_MOUSE_LEFT, 1, 0x50);
    s = state(1, 1);
    CHECK(!s.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER], "the focusing click across the wrap");
    xbox_InputMouseButtonAt(XBOX_MOUSE_LEFT, 0, 0x90);
    /* Focus lost before any press disarms. */
    xbox_InputMouseFocus(1);
    xbox_InputMouseFocusGainedAt(20000);
    xbox_InputMouseFocus(1);
    xbox_InputMouseFocus(0);
    xbox_InputMouseButtonAt(XBOX_MOUSE_LEFT, 1, 20010);
    s = state(1, 1);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255, "a stale focus ate a press");
    xbox_InputMouseButtonAt(XBOX_MOUSE_LEFT, 0, 20050);
#ifdef _WIN32
    /* The same through the window procedure: WM_SETFOCUS then the button
     * message, with no WM_MOUSEACTIVATE (the window manager activated the
     * window first). */
    xbox_InputMouseWin32Msg(NULL, WM_KILLFOCUS, 0, 0);
    xbox_InputMouseWin32Msg(NULL, WM_SETFOCUS, 0, 0);
    xbox_InputMouseWin32Msg(NULL, WM_LBUTTONDOWN, 0, 0);
    s = state(1, 1);
    CHECK(xbox_InputMouseCaptured() && !s.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER],
          "Win32: the focusing click drove RT");
    xbox_InputMouseWin32Msg(NULL, WM_LBUTTONUP, 0, 0);
    xbox_InputMouseWin32Msg(NULL, WM_LBUTTONDOWN, 0, 0);
    s = state(1, 1);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255, "Win32: the press after it");
    xbox_InputMouseWin32Msg(NULL, WM_LBUTTONUP, 0, 0);
#endif

    /* click: the first press grabs and is consumed. */
    fresh();
    bind_only(XBOX_CTL_B, "MouseLeft");
    xbox_InputMouseDefaultOpts(&o);
    o.capture = XBOX_MOUSE_CAPTURE_CLICK;
    xbox_InputMouseOpts(&o);
    xbox_InputMouseFocus(0);
    CHECK(!xbox_InputMouseCaptured(), "click mode grabbed on focus");
    xbox_InputMouseButton(XBOX_MOUSE_LEFT, 1);
    s = state(1, 1);
    CHECK(xbox_InputMouseCaptured() && !s.Gamepad.bAnalogButtons[XBOX_BUTTON_B],
          "click: the grabbing press was not consumed");
    xbox_InputMouseButton(XBOX_MOUSE_LEFT, 0);
    xbox_InputMouseButton(XBOX_MOUSE_LEFT, 1);
    s = state(1, 1);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 255, "click: the second press");
    xbox_InputMouseFocus(1);
    xbox_InputMouseFocus(0);
    CHECK(!xbox_InputMouseCaptured(), "click: refocus grabbed without a click");

    /* never: no capture, and the buttons drive. */
    fresh();
    bind_only(XBOX_CTL_B, "MouseLeft");
    o.capture = XBOX_MOUSE_CAPTURE_NEVER;
    xbox_InputMouseOpts(&o);
    xbox_InputMouseFocus(0);
    xbox_InputKeyEvent(0x78, 1);
    xbox_InputKeyEvent(0x78, 0);
    CHECK(!xbox_InputMouseCaptured(), "never grabbed");
    xbox_InputMouseButton(XBOX_MOUSE_LEFT, 1);
    s = state(1, 1);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 255, "never: buttons drive");
    xbox_InputMouseButton(XBOX_MOUSE_LEFT, 0);

    /* The mouse off: nothing grabs. */
    fresh();
    xbox_InputMouseEnable(0);
    xbox_InputMouseFocus(0);
    CHECK(!xbox_InputMouseCaptured(), "a disabled mouse grabbed");
    xbox_InputMouseEnable(1);
    CHECK(xbox_InputMouseCaptured(), "enabling with focus grabs");
    xbox_InputMouseClose();
    CHECK(!xbox_InputMouseCaptured(), "close kept the capture");

    /* A script's forced capture: held with no window (headless), but once
     * a window reports focus it follows the focus, so Alt-Tab frees it. */
    fresh();
    xbox_InputMouseForceCapture(1);
    CHECK(xbox_InputMouseCaptured(), "forced: no window, no grab");
    xbox_InputMouseFocus(0);
    CHECK(xbox_InputMouseCaptured(), "forced: focused window");
    xbox_InputMouseFocus(1);
    CHECK(!xbox_InputMouseCaptured(), "forced: kept the grab across Alt-Tab");
    xbox_InputMouseFocus(0);
    CHECK(xbox_InputMouseCaptured(), "forced: focus back");
    xbox_InputMouseForceCapture(0);
    xbox_InputMouseReset();
}

static void test_enable(void)
{
    XBOX_INPUT_STATE s;

    recomp_env_set(RENV_KEYBOARD, "1");
    recomp_env_set(RENV_MOUSE, "0");
    xbox_InputKeyboardEnable(0);
    xbox_InputMouseEnable(-1);
    CHECK(!xbox_InputKeyboardOn() && !xbox_InputMouseOn(), "Enable(0) over RECOMP_KEYBOARD=1");
    CHECK(!xbox_InputKbmPoll(&s), "poll with both off");
    xbox_InputKeyboardEnable(-1);
    CHECK(xbox_InputKeyboardOn(), "-1 back to RECOMP_KEYBOARD=1");
    xbox_InputMouseEnable(1);
    CHECK(xbox_InputMouseOn(), "Enable(1) over RECOMP_MOUSE=0");
    recomp_env_set(RENV_KEYBOARD, NULL);
    recomp_env_set(RENV_MOUSE, NULL);
    xbox_InputKeyboardEnable(-1);
    xbox_InputMouseEnable(-1);
    CHECK(!xbox_InputKeyboardOn() && !xbox_InputMouseOn(), "unset env is off");
}

/* ── 5. kbm_script ───────────────────────────────────────────────────── */

static void test_script(void)
{
    XBOX_INPUT_STATE s;
    static const char text[] =
        "# a comment line\n"
        "0p    key down Space\n"
        "40p   key up Space      # polls\n"
        "1p    key down Nope\n"
        "2p    jump\n"
        "0.5   mouse move 20 -5\n"
        "0.6   mouse down MouseRight\n"
        "0.7   wheel 1\n"
        "x     key down W\n";

    fresh();
    bind_only(XBOX_CTL_A, "Space");
    bind_only(XBOX_CTL_X, "MouseRight");
    bind_only(XBOX_CTL_WHITE, "WheelUp");
    xbox_InputMouseFocus(0);
    CHECK(xbox_InputKbmScriptParse(text, "test.kbm") == 5, "events parsed");

    int a_from = -1, a_to = -1;
    for (int poll = 0; poll < 50; poll++, f_now += 5) {
        CHECK(xbox_InputKbmPoll(&s), "poll %d", poll);
        int a = s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] != 0;
        if (a && a_from < 0) a_from = poll;
        if (!a && a_from >= 0 && a_to < 0) a_to = poll;
    }
    CHECK(a_from == 0 && a_to == 40, "Space held from poll %d to %d", a_from, a_to);

    /* Seconds: 0.5 s after the first poll (f_now started at 1000). */
    f_now = 1000 + 499;
    xbox_InputKbmPoll(&s);
    CHECK(!s.Gamepad.sThumbRX, "move before its time");
    f_now = 1000 + 500;
    xbox_InputKbmPoll(&s);
    CHECK(NEAR(s.Gamepad.sThumbRX, 23756) && NEAR(s.Gamepad.sThumbRY, 16998),
          "mouse move 20 -5: RS(%d, %d)", s.Gamepad.sThumbRX, s.Gamepad.sThumbRY);
    xbox_InputKbmPoll(&s);
    CHECK(!s.Gamepad.sThumbRX && !s.Gamepad.sThumbRY, "the move lasts one poll");
    f_now = 1000 + 600;
    xbox_InputKbmPoll(&s);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_X] == 255, "mouse down MouseRight");
    f_now = 1000 + 700;
    xbox_InputKbmPoll(&s);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] == 255, "wheel 1");
    xbox_InputKbmScriptReset();
    xbox_InputMouseReset();
    xbox_InputKeysClear();
}

/* ── 6. the port-0 merge ─────────────────────────────────────────────── */

static int f_plugged;
static DWORD f_get(DWORD p, XBOX_INPUT_STATE *s)
{
    if (p != 0 || !f_plugged)
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(s, 0, sizeof *s);
    s->dwPacketNumber = 5;
    s->Gamepad.wButtons = XBOX_GAMEPAD_START;
    s->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 50;
    s->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 100;
    s->Gamepad.sThumbLX = 1000;
    s->Gamepad.sThumbRY = -5000;
    return ERROR_SUCCESS;
}
static DWORD f_set(DWORD p, const XBOX_VIBRATION *v) { (void)p; (void)v; return ERROR_SUCCESS; }

static const xbox_input_backend k_fake = { NULL, f_get, f_set, xbox_InputKbmPoll, f_clock };
/* input_core.c's default; never called here. */
const xbox_input_backend xbox_input_host_backend = { NULL, f_get, f_set, NULL, f_clock };

static void test_merge(void)
{
    XBOX_INPUT_STATE s;
    DWORD packet;

    fresh();
    f_plugged = 1;
    xbox_InputSetBackend(&k_fake);
    bind_only(XBOX_CTL_A, "Space");
    bind_only(XBOX_CTL_DPAD_UP, "Up");
    bind_only(XBOX_CTL_LSTICK_RIGHT, "D");
    xbox_InputKeyEvent(code_of("Space"), 1);
    xbox_InputKeyEvent(code_of("Up"), 1);
    xbox_InputKeyEvent(code_of("D"), 1);
    CHECK(xbox_InputGetState(0, &s) == ERROR_SUCCESS, "port 0");
    CHECK(s.Gamepad.wButtons == (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_UP), "buttons OR: 0x%04X",
          s.Gamepad.wButtons);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255
          && s.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 100, "analog max");
    CHECK(s.Gamepad.sThumbLX == 32767 && s.Gamepad.sThumbRY == -5000,
          "axes: LX %d RY %d", s.Gamepad.sThumbLX, s.Gamepad.sThumbRY);
    packet = s.dwPacketNumber;
    xbox_InputGetState(0, &s);
    CHECK(s.dwPacketNumber != packet && s.dwPacketNumber != 5, "packet did not move");

    /* The mouse alone, no pad: port 0 answers. */
    xbox_InputKeysClear();
    f_plugged = 0;
    xbox_InputSetBackend(&k_fake);
    xbox_InputMouseFocus(0);
    xbox_InputMouseMotion(40, 0);
    CHECK(xbox_InputGetState(0, &s) == ERROR_SUCCESS && s.Gamepad.sThumbRX == 32767,
          "mouse without a pad: RX %d", s.Gamepad.sThumbRX);
    CHECK(xbox_InputGetState(1, &s) == ERROR_DEVICE_NOT_CONNECTED, "port 1");

    /* Both off: port 0 is the pad alone, or nothing. */
    xbox_InputKeyboardEnable(0);
    xbox_InputMouseEnable(0);
    CHECK(xbox_InputGetState(0, &s) == ERROR_DEVICE_NOT_CONNECTED, "both off, no pad");
    f_plugged = 1;
    xbox_InputSetBackend(&k_fake);
    xbox_InputKeyEvent(code_of("Space"), 1);
    CHECK(xbox_InputGetState(0, &s) == ERROR_SUCCESS
          && s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 50 && s.dwPacketNumber == 5,
          "both off: the pad untouched");
    xbox_InputKeysClear();
    xbox_InputSetBackend(NULL);
}

int main(void)
{
    test_names();
    test_bindings();
    test_compiled_rows();
    test_ramp();
    test_sides();
    test_wheel();
    test_mouse_model();
    test_capture();
    test_enable();
    test_script();
    test_merge();
    printf("input_bindings: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
