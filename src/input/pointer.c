/*
 * The absolute pointer for the enhancements menu (pointer.h).
 *
 * A seqlock: the writer makes s_lock odd, stores, makes it even again; a
 * reader that saw it odd, or changed across its copy, reads again. The
 * fields are atomics with relaxed order inside the fences, so the copy is
 * no data race. The wheel is a counter of its own: the writer adds, the
 * reader takes it with an exchange, so no step is lost or counted twice.
 * A press is a counter per button with the place of its latest press, both
 * under the lock; the one reader keeps the counts it saw last, so a press
 * and its release between two takes still reach it as pressed.
 */
#include "pointer.h"

#include <math.h>

#define LOAD(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)

static unsigned s_lock;
static int s_x = -1, s_y = -1;
static unsigned s_buttons;
static uint32_t s_seq;
static int s_wheel;
static uint32_t s_presses[XBOX_POINTER_BUTTONS];
static int s_press_x[XBOX_POINTER_BUTTONS], s_press_y[XBOX_POINTER_BUTTONS];
/* The reader's: the press counts at its last take. */
static uint32_t s_seen[XBOX_POINTER_BUTTONS];
/* The SDL wheel's fraction and Win32's part of a notch (a high-resolution
 * wheel sends deltas under 120), carried into the next event. */
static float s_sdl_rem;
static int s_win_rem;

static void write_begin(void)
{
    STORE(&s_lock, LOAD(&s_lock) + 1);
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static void write_end(void)
{
    STORE(&s_seq, LOAD(&s_seq) + 1);
    __atomic_store_n(&s_lock, LOAD(&s_lock) + 1, __ATOMIC_RELEASE);
}

void xbox_InputPointerMove(int x, int y)
{
    write_begin();
    STORE(&s_x, x);
    STORE(&s_y, y);
    write_end();
}

void xbox_InputPointerButton(unsigned button, int down, int x, int y)
{
    unsigned b = LOAD(&s_buttons);
    write_begin();
    STORE(&s_x, x);
    STORE(&s_y, y);
    STORE(&s_buttons, down ? b | button : b & ~button);
    /* Every down message is a press, held already or not: a lost up must
     * not eat the next click. */
    for (int i = 0; down && i < XBOX_POINTER_BUTTONS; i++)
        if (button & (1u << i)) {
            STORE(&s_presses[i], LOAD(&s_presses[i]) + 1);
            STORE(&s_press_x[i], x);
            STORE(&s_press_y[i], y);
        }
    write_end();
}

/* A move that also says which buttons are held (Win32's MK_* flags). */
static void move_held(int x, int y, unsigned held)
{
    write_begin();
    STORE(&s_x, x);
    STORE(&s_y, y);
    STORE(&s_buttons, held);
    write_end();
}

void xbox_InputPointerWheel(int steps)
{
    if (!steps)
        return;
    __atomic_add_fetch(&s_wheel, steps, __ATOMIC_RELAXED);
    write_begin();
    write_end();
}

void xbox_InputPointerFocusLost(void)
{
    if (!LOAD(&s_buttons))
        return;
    write_begin();
    STORE(&s_buttons, 0u);
    write_end();
}

/* The Win32 numbers, spelled out so this decodes on every host. */
enum {
    P_WM_KILLFOCUS = 0x0008,
    P_WM_MOUSEMOVE = 0x0200,
    P_WM_LBUTTONDOWN = 0x0201, P_WM_LBUTTONUP = 0x0202,
    P_WM_RBUTTONDOWN = 0x0204, P_WM_RBUTTONUP = 0x0205,
    P_WM_MBUTTONDOWN = 0x0207, P_WM_MBUTTONUP = 0x0208,
    P_WM_MOUSEWHEEL = 0x020A,
    P_WHEEL_DELTA = 120,
    P_MK_LBUTTON = 0x0001, P_MK_RBUTTON = 0x0002, P_MK_MBUTTON = 0x0010,
};

int xbox_InputPointerWin32Msg(unsigned msg, uintptr_t w, intptr_t l)
{
    /* GET_X_LPARAM and GET_Y_LPARAM: signed 16-bit client coordinates
     * (negative left of or above the client area while a button holds the
     * capture). */
    int x = (int)(int16_t)(uint16_t)((uintptr_t)l & 0xFFFFu);
    int y = (int)(int16_t)(uint16_t)(((uintptr_t)l >> 16) & 0xFFFFu);
    switch (msg) {
    case P_WM_MOUSEMOVE:
        /* Without the capture, a button let go outside the client area
         * sends its up there; the next move inside says it is up. */
        move_held(x, y, ((w & P_MK_LBUTTON) ? XBOX_POINTER_LEFT : 0u)
                            | ((w & P_MK_RBUTTON) ? XBOX_POINTER_RIGHT : 0u)
                            | ((w & P_MK_MBUTTON) ? XBOX_POINTER_MIDDLE : 0u));
        return 1;
    case P_WM_LBUTTONDOWN: xbox_InputPointerButton(XBOX_POINTER_LEFT, 1, x, y); return 1;
    case P_WM_LBUTTONUP: xbox_InputPointerButton(XBOX_POINTER_LEFT, 0, x, y); return 1;
    case P_WM_RBUTTONDOWN: xbox_InputPointerButton(XBOX_POINTER_RIGHT, 1, x, y); return 1;
    case P_WM_RBUTTONUP: xbox_InputPointerButton(XBOX_POINTER_RIGHT, 0, x, y); return 1;
    case P_WM_MBUTTONDOWN: xbox_InputPointerButton(XBOX_POINTER_MIDDLE, 1, x, y); return 1;
    case P_WM_MBUTTONUP: xbox_InputPointerButton(XBOX_POINTER_MIDDLE, 0, x, y); return 1;
    case P_WM_MOUSEWHEEL: {
        /* GET_WHEEL_DELTA_WPARAM: the high word, signed; its point is in
         * screen coordinates, so only the delta is used. */
        int n;
        s_win_rem += (int)(int16_t)(uint16_t)((w >> 16) & 0xFFFFu);
        n = s_win_rem / P_WHEEL_DELTA;
        s_win_rem -= n * P_WHEEL_DELTA;
        xbox_InputPointerWheel(n);
        return 1;
    }
    case P_WM_KILLFOCUS:
        s_win_rem = 0;
        xbox_InputPointerFocusLost();
        return 1;
    default:
        return 0;
    }
}

static int px_of(float v, float density)
{
    return (int)floorf(v * (density > 0.0f ? density : 1.0f));
}

void xbox_InputPointerSdlMove(float x, float y, float density)
{
    xbox_InputPointerMove(px_of(x, density), px_of(y, density));
}

void xbox_InputPointerSdlButton(unsigned button, int down, float x, float y, float density)
{
    xbox_InputPointerButton(button, down, px_of(x, density), px_of(y, density));
}

void xbox_InputPointerSdlWheel(float y)
{
    int n;
    s_sdl_rem += y;
    n = (int)s_sdl_rem;     /* toward zero, so the rest keeps its sign */
    s_sdl_rem -= (float)n;
    xbox_InputPointerWheel(n);
}

void xbox_InputPointerTake(struct xbox_pointer *out)
{
    unsigned a, b;
    uint32_t n[XBOX_POINTER_BUTTONS];
    do {
        a = __atomic_load_n(&s_lock, __ATOMIC_ACQUIRE);
        out->x = LOAD(&s_x);
        out->y = LOAD(&s_y);
        out->buttons = LOAD(&s_buttons);
        out->seq = LOAD(&s_seq);
        for (int i = 0; i < XBOX_POINTER_BUTTONS; i++) {
            n[i] = LOAD(&s_presses[i]);
            out->press[i].x = LOAD(&s_press_x[i]);
            out->press[i].y = LOAD(&s_press_y[i]);
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        b = LOAD(&s_lock);
    } while ((a & 1u) || a != b);
    out->wheel = __atomic_exchange_n(&s_wheel, 0, __ATOMIC_RELAXED);
    out->pressed = 0;
    for (int i = 0; i < XBOX_POINTER_BUTTONS; i++) {
        if (n[i] != s_seen[i])
            out->pressed |= 1u << i;
        s_seen[i] = n[i];
    }
}

void xbox_InputPointerReset(void)
{
    STORE(&s_lock, 0u);
    STORE(&s_x, -1);
    STORE(&s_y, -1);
    STORE(&s_buttons, 0u);
    STORE(&s_seq, 0u);
    STORE(&s_wheel, 0);
    for (int i = 0; i < XBOX_POINTER_BUTTONS; i++) {
        STORE(&s_presses[i], 0u);
        STORE(&s_press_x[i], -1);
        STORE(&s_press_y[i], -1);
        s_seen[i] = 0;
    }
    s_sdl_rem = 0.0f;
    s_win_rem = 0;
}
