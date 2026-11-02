/*
 * input_pointer test: see CMakeLists.txt.
 *
 * The message words are built here by hand (MAKELPARAM's layout), not by
 * the code under test.
 */
#include "pointer.h"

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

static intptr_t lp(int x, int y)
{
    return (intptr_t)(((uint32_t)(uint16_t)(int16_t)y << 16) | (uint16_t)(int16_t)x);
}

static uintptr_t wheel_wp(int delta)
{
    return (uintptr_t)((uint32_t)(uint16_t)(int16_t)delta << 16);
}

static void test_win32(void)
{
    struct xbox_pointer p;
    xbox_InputPointerReset();
    xbox_InputPointerTake(&p);
    CHECK(p.x == -1 && p.y == -1 && p.seq == 0 && p.buttons == 0 && p.wheel == 0,
          "start %d,%d seq %u", p.x, p.y, (unsigned)p.seq);

    CHECK(xbox_InputPointerWin32Msg(0x0200, 0, lp(320, 200)), "WM_MOUSEMOVE taken");
    xbox_InputPointerTake(&p);
    CHECK(p.x == 320 && p.y == 200 && p.seq == 1, "move %d,%d seq %u", p.x, p.y,
          (unsigned)p.seq);

    /* Dragged out of the client area while a button holds the capture: the
     * words are signed. */
    xbox_InputPointerWin32Msg(0x0201, 0, lp(10, 20));
    xbox_InputPointerWin32Msg(0x0200, 0x0001, lp(-5, -7));   /* MK_LBUTTON */
    xbox_InputPointerTake(&p);
    CHECK(p.x == -5 && p.y == -7, "negative %d,%d", p.x, p.y);
    CHECK(p.buttons == XBOX_POINTER_LEFT, "left held %u", p.buttons);
    xbox_InputPointerWin32Msg(0x0204, 0, lp(1, 1));
    xbox_InputPointerWin32Msg(0x0207, 0, lp(1, 1));
    xbox_InputPointerTake(&p);
    CHECK(p.buttons == (XBOX_POINTER_LEFT | XBOX_POINTER_RIGHT | XBOX_POINTER_MIDDLE),
          "three held %u", p.buttons);
    xbox_InputPointerWin32Msg(0x0202, 0, lp(1, 1));
    xbox_InputPointerWin32Msg(0x0208, 0, lp(1, 1));
    xbox_InputPointerTake(&p);
    CHECK(p.buttons == XBOX_POINTER_RIGHT, "right left %u", p.buttons);
    xbox_InputPointerWin32Msg(0x0205, 0, lp(1, 1));
    xbox_InputPointerTake(&p);
    CHECK(p.buttons == 0, "none %u", p.buttons);

    /* Notches, then a high-resolution wheel's 40s, then down. The point in
     * lParam is in screen coordinates and is not the pointer. */
    xbox_InputPointerWin32Msg(0x020A, wheel_wp(240), lp(3000, 3000));
    xbox_InputPointerTake(&p);
    CHECK(p.wheel == 2 && p.x == 1 && p.y == 1, "two notches %d at %d,%d", p.wheel, p.x, p.y);
    xbox_InputPointerTake(&p);
    CHECK(p.wheel == 0, "taken once %d", p.wheel);
    xbox_InputPointerWin32Msg(0x020A, wheel_wp(40), 0);
    xbox_InputPointerWin32Msg(0x020A, wheel_wp(40), 0);
    xbox_InputPointerTake(&p);
    CHECK(p.wheel == 0, "two thirds %d", p.wheel);
    xbox_InputPointerWin32Msg(0x020A, wheel_wp(40), 0);
    xbox_InputPointerTake(&p);
    CHECK(p.wheel == 1, "the third third %d", p.wheel);
    xbox_InputPointerWin32Msg(0x020A, wheel_wp(-120), 0);
    xbox_InputPointerTake(&p);
    CHECK(p.wheel == -1, "down %d", p.wheel);

    /* Focus lost with a button down: nothing stays held. */
    xbox_InputPointerWin32Msg(0x0201, 0, lp(4, 4));
    CHECK(xbox_InputPointerWin32Msg(0x0008, 0, 0), "WM_KILLFOCUS taken");
    xbox_InputPointerTake(&p);
    CHECK(p.buttons == 0, "focus lost %u", p.buttons);
    CHECK(!xbox_InputPointerWin32Msg(0x0100, 0, 0), "WM_KEYDOWN ignored");
}

/* A press and its release between two takes: a trackpad's tap, a touch, a
 * window pumped only at flips. The take still says pressed, at the place
 * of the press, and only once. */
static void test_presses(void)
{
    struct xbox_pointer p;
    xbox_InputPointerReset();
    xbox_InputPointerTake(&p);
    CHECK(p.pressed == 0, "none pressed at the start %u", p.pressed);

    xbox_InputPointerWin32Msg(0x0201, 0, lp(40, 50));
    xbox_InputPointerWin32Msg(0x0202, 0, lp(41, 50));
    xbox_InputPointerWin32Msg(0x0200, 0, lp(90, 90));
    xbox_InputPointerTake(&p);
    CHECK(p.buttons == 0, "tap: nothing held %u", p.buttons);
    CHECK(p.pressed == XBOX_POINTER_LEFT, "tap: left pressed %u", p.pressed);
    CHECK(p.press[0].x == 40 && p.press[0].y == 50, "tap at %d,%d", p.press[0].x,
          p.press[0].y);
    CHECK(p.x == 90 && p.y == 90, "the pointer moved on %d,%d", p.x, p.y);
    xbox_InputPointerTake(&p);
    CHECK(p.pressed == 0, "tap taken once %u", p.pressed);

    /* A right tap and a held left in one gap; SDL's path too. */
    xbox_InputPointerSdlButton(XBOX_POINTER_RIGHT, 1, 5, 6, 2.0f);
    xbox_InputPointerSdlButton(XBOX_POINTER_RIGHT, 0, 5, 6, 2.0f);
    xbox_InputPointerSdlButton(XBOX_POINTER_LEFT, 1, 7, 8, 2.0f);
    xbox_InputPointerTake(&p);
    CHECK(p.pressed == (XBOX_POINTER_LEFT | XBOX_POINTER_RIGHT), "two pressed %u", p.pressed);
    CHECK(p.buttons == XBOX_POINTER_LEFT, "left still held %u", p.buttons);
    CHECK(p.press[1].x == 10 && p.press[1].y == 12 && p.press[0].x == 14,
          "places right %d,%d left %d", p.press[1].x, p.press[1].y, p.press[0].x);

    /* A double tap between takes is one press for the reader, at the
     * second place. */
    xbox_InputPointerReset();
    xbox_InputPointerButton(XBOX_POINTER_LEFT, 1, 1, 1);
    xbox_InputPointerButton(XBOX_POINTER_LEFT, 0, 1, 1);
    xbox_InputPointerButton(XBOX_POINTER_LEFT, 1, 3, 3);
    xbox_InputPointerButton(XBOX_POINTER_LEFT, 0, 3, 3);
    xbox_InputPointerTake(&p);
    CHECK(p.pressed == XBOX_POINTER_LEFT && p.press[0].x == 3, "double tap %u at %d",
          p.pressed, p.press[0].x);

    /* A drag let go outside the client area: the up went elsewhere, and the
     * next move inside (MK_* clear) lets it go. */
    xbox_InputPointerReset();
    xbox_InputPointerWin32Msg(0x0201, 0, lp(10, 10));
    xbox_InputPointerWin32Msg(0x0200, 0x0001, lp(12, 10));
    xbox_InputPointerTake(&p);
    CHECK(p.buttons == XBOX_POINTER_LEFT, "dragging %u", p.buttons);
    xbox_InputPointerWin32Msg(0x0200, 0, lp(30, 30));
    xbox_InputPointerTake(&p);
    CHECK(p.buttons == 0 && p.pressed == 0, "let go outside: held %u pressed %u", p.buttons,
          p.pressed);
    xbox_InputPointerWin32Msg(0x0200, 0x0002 | 0x0010, lp(30, 30));
    xbox_InputPointerTake(&p);
    CHECK(p.buttons == (XBOX_POINTER_RIGHT | XBOX_POINTER_MIDDLE) && p.pressed == 0,
          "MK flags held %u pressed %u", p.buttons, p.pressed);

    /* A down while marked held (its up lost) is still a press. */
    xbox_InputPointerReset();
    xbox_InputPointerButton(XBOX_POINTER_LEFT, 1, 1, 1);
    xbox_InputPointerTake(&p);
    xbox_InputPointerButton(XBOX_POINTER_LEFT, 1, 2, 2);
    xbox_InputPointerTake(&p);
    CHECK(p.pressed == XBOX_POINTER_LEFT, "down after a lost up %u", p.pressed);
}

static void test_sdl(void)
{
    struct xbox_pointer p;
    uint32_t seq;
    xbox_InputPointerReset();
    /* Points times the density: 2 on a Retina display, 1.5 at 150 %. */
    xbox_InputPointerSdlMove(100.25f, 50.75f, 2.0f);
    xbox_InputPointerTake(&p);
    CHECK(p.x == 200 && p.y == 101, "retina %d,%d", p.x, p.y);
    xbox_InputPointerSdlMove(100.0f, 50.0f, 1.5f);
    xbox_InputPointerTake(&p);
    CHECK(p.x == 150 && p.y == 75, "150%% %d,%d", p.x, p.y);
    /* No density yet (0): one pixel a point. */
    xbox_InputPointerSdlButton(XBOX_POINTER_RIGHT, 1, 7.9f, 8.0f, 0.0f);
    xbox_InputPointerTake(&p);
    CHECK(p.x == 7 && p.y == 8 && p.buttons == XBOX_POINTER_RIGHT, "button %d,%d %u", p.x, p.y,
          p.buttons);
    xbox_InputPointerSdlButton(XBOX_POINTER_RIGHT, 0, 7.9f, 8.0f, 1.0f);
    seq = p.seq;
    xbox_InputPointerTake(&p);
    CHECK(p.buttons == 0 && p.seq == seq + 1, "release seq %u -> %u", (unsigned)seq,
          (unsigned)p.seq);
    /* A trackpad's fractions add up; the rest keeps its sign. */
    xbox_InputPointerSdlWheel(0.4f);
    xbox_InputPointerSdlWheel(0.4f);
    xbox_InputPointerTake(&p);
    CHECK(p.wheel == 0, "0.8 %d", p.wheel);
    xbox_InputPointerSdlWheel(0.4f);
    xbox_InputPointerTake(&p);
    CHECK(p.wheel == 1, "1.2 %d", p.wheel);
    xbox_InputPointerSdlWheel(-1.5f);
    xbox_InputPointerTake(&p);
    CHECK(p.wheel == -1, "-1.3 %d", p.wheel);
    /* A still pointer and one back in the same place differ by seq. */
    seq = p.seq;
    xbox_InputPointerSdlMove(7.9f, 8.0f, 1.0f);
    xbox_InputPointerTake(&p);
    CHECK(p.x == 7 && p.seq != seq, "same place, new seq");
}

/* The writer moves along x == y; a torn read would see them differ. */
static volatile int s_stop;
#ifdef _WIN32
static DWORD WINAPI writer(LPVOID u)
#else
static void *writer(void *u)
#endif
{
    (void)u;
    for (int i = 0; !s_stop; i++) {
        xbox_InputPointerMove(i & 0xFFFF, i & 0xFFFF);
        if (!(i & 63))
            xbox_InputPointerWheel(1);
    }
    return 0;
}

static void test_threads(void)
{
    struct xbox_pointer p;
    long wheel = 0, torn = 0;
    uint32_t last = 0;
    int back = 0;
#ifdef _WIN32
    HANDLE t;
#else
    pthread_t t;
#endif
    xbox_InputPointerReset();
    s_stop = 0;
#ifdef _WIN32
    t = CreateThread(NULL, 0, writer, NULL, 0, NULL);
#else
    pthread_create(&t, NULL, writer, NULL);
#endif
    for (int i = 0; i < 200000; i++) {
        xbox_InputPointerTake(&p);
        torn += p.x != p.y;
        back += p.seq < last;
        last = p.seq;
        wheel += p.wheel;
    }
    s_stop = 1;
#ifdef _WIN32
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
#else
    pthread_join(t, NULL);
#endif
    xbox_InputPointerTake(&p);
    wheel += p.wheel;
    CHECK(torn == 0, "%ld torn snapshots", torn);
    CHECK(back == 0, "seq went back %d times", back);
    CHECK(wheel > 0, "wheel steps arrived");
}

int main(void)
{
    test_win32();
    test_presses();
    test_sdl();
    test_threads();
    printf("input_pointer: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
