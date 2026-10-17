/*
 * The mouse as part of the port-0 pad. See mouse.h.
 *
 * The title has no pointer. On the console its camera is the right stick,
 * and on a PC the camera is the mouse, so motion becomes a stick deflection:
 * the counts accumulated since the last poll, scaled by the sensitivity,
 * with a floor. The floor is there because a title that zeroes a stick
 * under 0.4 of full deflection (BLiNX 2 does) would ignore slow motion and
 * make medium motion jump in; with it, any motion turns the camera at least
 * at the slowest rate the title knows. A mouse at rest reads zero on the
 * next poll, so there is no drift to decay.
 *
 * The cursor is captured (hidden, relative, confined) while the window has
 * focus, so the motion is never lost at a screen edge and a click never
 * lands on the desktop. Losing the focus releases it, as it releases the
 * keys; so do the release key (F9 by default) and the enhancements menu.
 */
#include "mouse.h"
#include "keyboard.h"
#include "input_names.h"
#include "recomp_env.h"
#include "recomp_exit_hook.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define LOAD(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define STORE(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define XCHG(p, v)   __atomic_exchange_n((p), (v), __ATOMIC_ACQ_REL)
#define ADD(p, v)    __atomic_add_fetch((p), (v), __ATOMIC_ACQ_REL)

static xbox_mouse_opts s_opts = {
    XBOX_MOUSE_LOOK_RIGHT, 1.0, 0, 0, 0.45, XBOX_MOUSE_CAPTURE_FOCUS, 0x78, 100
};
static int s_enable = -1;

/* Window thread in, poll thread out. */
static int s_dx, s_dy;
static int s_wheel_q[2];                 /* queued notches: up, down */
static unsigned char s_btn[XBOX_MOUSE_5 + 1];

/* The capture state machine's inputs, and its answer. */
static int s_focus, s_suspend, s_key_free, s_clicked, s_forced, s_closed;
static int s_windowed;                   /* a window has reported its focus */
static int s_activate_click;             /* the next press focused the window */
static int s_focus_armed;                /* focus gained, no press seen yet */
static uint32_t s_focus_ms;              /* when, on the feeder's clock */
static int s_captured;
/* Taken by every change of s_captured and by the writers that test it, so
 * a release cannot land between a writer's test and its write (a button
 * left held after release_all). Never held across a print. */
static int s_lock;

static void lock(void)
{
    while (__atomic_test_and_set(&s_lock, __ATOMIC_ACQUIRE))
        ;
}

static void unlock(void)
{
    __atomic_clear(&s_lock, __ATOMIC_RELEASE);
}

/* Poll thread only: the wheel pulses. */
static int      s_wheel_held[2];
static uint64_t s_wheel_until[2];

void xbox_InputMouseDefaultOpts(xbox_mouse_opts *o)
{
    o->look = XBOX_MOUSE_LOOK_RIGHT;
    o->sensitivity = 1.0;
    o->invert_x = o->invert_y = 0;
    o->min_deflection = 0.45;
    o->capture = XBOX_MOUSE_CAPTURE_FOCUS;
    o->release_key = 0x78;              /* F9 */
    o->wheel_pulse_ms = 100;
}

static void update(const char *reason);

void xbox_InputMouseOpts(const xbox_mouse_opts *o)
{
    if (o)
        s_opts = *o;
    else
        xbox_InputMouseDefaultOpts(&s_opts);
    update("off");
}

void xbox_InputMouseGetOpts(xbox_mouse_opts *o)
{
    *o = s_opts;
}

void xbox_InputMouseEnable(int on)
{
    STORE(&s_enable, on < 0 ? -1 : on != 0);
    update("off");
}

int xbox_InputMouseOn(void)
{
    int e = LOAD(&s_enable);
    return e >= 0 ? e : recomp_env_on(RENV_MOUSE);
}

/* ---- capture ------------------------------------------------------------ */

static int want_capture(void)
{
    if (!xbox_InputMouseOn() || LOAD(&s_closed))
        return 0;
    /* The injector's capture stands in for a focus a headless run never
     * gets; with a window, losing the focus still releases it, or a
     * windowed scripted run would keep the grab across Alt-Tab. */
    if (LOAD(&s_forced))
        return !LOAD(&s_windowed) || LOAD(&s_focus);
    if (!LOAD(&s_focus) || LOAD(&s_suspend) || LOAD(&s_key_free))
        return 0;
    if (s_opts.capture == XBOX_MOUSE_CAPTURE_NEVER)
        return 0;
    return s_opts.capture != XBOX_MOUSE_CAPTURE_CLICK || LOAD(&s_clicked);
}

static void release_all(void)
{
    for (int i = 0; i <= XBOX_MOUSE_5; i++)
        STORE(&s_btn[i], 0);
    XCHG(&s_dx, 0);
    XCHG(&s_dy, 0);
    XCHG(&s_wheel_q[0], 0);
    XCHG(&s_wheel_q[1], 0);
}

/* Every input of the state machine ends here. The decision and the
 * release are serialised; the line is printed after the lock is dropped, so
 * two threads changing it at once may log in either order. */
static void update(const char *reason)
{
    int want, was;

    lock();
    want = want_capture();
    was = XCHG(&s_captured, want);
    if (want != was && !want)
        release_all();
    unlock();
    if (want == was)
        return;
    if (want)
        fprintf(stderr, "[INPUT] mouse captured\n");
    else
        fprintf(stderr, "[INPUT] mouse released (%s)\n", reason);
    xbox_InputTraceEdge(want ? "mouse captured" : "mouse released");
    fflush(stderr);
}

int xbox_InputMouseCaptured(void)
{
    return LOAD(&s_captured);
}

void xbox_InputMouseFocus(int lost)
{
    STORE(&s_windowed, 1);
    if (lost) {
        STORE(&s_focus, 0);
        STORE(&s_clicked, 0);
        STORE(&s_activate_click, 0);
        STORE(&s_focus_armed, 0);
    } else {
        STORE(&s_focus, 1);
        STORE(&s_key_free, 0);           /* refocusing grabs again */
    }
    update("focus");
}

void xbox_InputMouseActivateClick(void)
{
    STORE(&s_activate_click, 1);
}

void xbox_InputMouseFocusGainedAt(uint32_t now_ms)
{
    STORE(&s_focus_ms, now_ms);
    STORE(&s_focus_armed, 1);
    xbox_InputMouseFocus(0);
}

void xbox_InputMouseButtonAt(int n, int down, uint32_t now_ms)
{
    /* A window manager that activates on click hands the window its focus
     * and then the click itself; nothing else tells the two apart, so the
     * first press that close behind the focus is the player picking the
     * window, as WM_MOUSEACTIVATE says on Win32. */
    if (down && XCHG(&s_focus_armed, 0)
        && (uint32_t)(now_ms - LOAD(&s_focus_ms)) <= XBOX_MOUSE_FOCUS_CLICK_MS)
        xbox_InputMouseActivateClick();
    xbox_InputMouseButton(n, down);
}

void xbox_InputMouseClose(void)
{
    STORE(&s_closed, 1);
    update("close");
}

int xbox_InputMouseKey(int vk, int down)
{
    if (!s_opts.release_key || vk != s_opts.release_key)
        return 0;
    if (down && xbox_InputMouseOn()) {
        if (LOAD(&s_captured)) {
            STORE(&s_key_free, 1);
        } else {
            STORE(&s_key_free, 0);
            STORE(&s_clicked, 1);
        }
        update("key");
    }
    return 1;
}

void xbox_InputMouseSuspend(int on)
{
    STORE(&s_suspend, on != 0);
    update("menu");
}

void xbox_InputMouseForceCapture(int on)
{
    STORE(&s_forced, on != 0);
    update("script");
}

/* ---- the window's side ------------------------------------------------- */

void xbox_InputMouseMotion(int dx, int dy)
{
    lock();
    if (LOAD(&s_captured)) {
        if (dx) ADD(&s_dx, dx);
        if (dy) ADD(&s_dy, dy);
    }
    unlock();
}

void xbox_InputMouseButton(int n, int down)
{
    if (n < XBOX_MOUSE_LEFT || n > XBOX_MOUSE_5)
        return;
    if (!down) {
        if (XCHG(&s_btn[n], 0))
            xbox_InputTraceEdge("mouse up %s", xbox_InputCodeName(n, 1));
        return;
    }
    if (!xbox_InputMouseOn())
        return;
    /* "focus": the click that brought the window forward is the player
     * picking the window, so it drives nothing (as "click" mode's grab). */
    if (XCHG(&s_activate_click, 0) && s_opts.capture == XBOX_MOUSE_CAPTURE_FOCUS)
        return;
    int edge = 0, grab = 0;
    lock();
    if (!LOAD(&s_captured)) {
        /* "click": the press that grabs is the player asking for the
         * mouse, not a press of B. "never": the window only reports
         * presses over it, so they drive. */
        if (s_opts.capture == XBOX_MOUSE_CAPTURE_CLICK && LOAD(&s_focus)
            && !LOAD(&s_suspend))
            grab = 1;
        else if (s_opts.capture == XBOX_MOUSE_CAPTURE_NEVER)
            edge = !XCHG(&s_btn[n], 1);
    } else {
        edge = !XCHG(&s_btn[n], 1);
    }
    unlock();
    if (grab) {
        STORE(&s_clicked, 1);
        STORE(&s_key_free, 0);
        update("click");
    }
    if (edge)
        xbox_InputTraceEdge("mouse down %s", xbox_InputCodeName(n, 1));
}

void xbox_InputMouseWheel(int notches)
{
    int took = 0;

    if (!notches || !xbox_InputMouseOn())
        return;
    lock();
    if (LOAD(&s_captured) || s_opts.capture == XBOX_MOUSE_CAPTURE_NEVER) {
        ADD(&s_wheel_q[notches > 0 ? 0 : 1], notches > 0 ? notches : -notches);
        took = 1;
    }
    unlock();
    if (took)
        xbox_InputTraceEdge("wheel %+d", notches);
}

/* ---- the poll's side --------------------------------------------------- */

void xbox_InputMouseTick(uint64_t now_ms, int have_clock)
{
    for (int i = 0; i < 2; i++) {
        if (s_wheel_held[i] && have_clock && now_ms < s_wheel_until[i])
            continue;
        if (s_wheel_held[i]) {
            /* One poll released between notches, or two notches read as
             * one long press to an edge detector. */
            s_wheel_held[i] = 0;
            continue;
        }
        int q = LOAD(&s_wheel_q[i]);
        while (q > 0 && !__atomic_compare_exchange_n(&s_wheel_q[i], &q, q - 1, 0,
                                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            ;
        if (q > 0) {
            s_wheel_held[i] = 1;
            s_wheel_until[i] = now_ms + (uint64_t)(s_opts.wheel_pulse_ms > 0
                                                   ? s_opts.wheel_pulse_ms : 0);
        }
    }
}

int xbox_InputMouseHeld(int code)
{
    if (code >= XBOX_MOUSE_LEFT && code <= XBOX_MOUSE_5)
        return LOAD(&s_btn[code]) != 0;
    if (code == XBOX_MOUSE_WHEEL_UP)
        return s_wheel_held[0];
    if (code == XBOX_MOUSE_WHEEL_DOWN)
        return s_wheel_held[1];
    return 0;
}

int xbox_InputMouseModel(int counts, double sensitivity, double min_deflection)
{
    double c = counts * sensitivity / 40.0, n, out;

    if (counts == 0 || !(c != 0.0))
        return 0;
    n = fabs(c) < 1.0 ? fabs(c) : 1.0;
    out = (min_deflection + (1.0 - min_deflection) * n) * 32767.0;
    if (out > 32767.0)
        out = 32767.0;
    return c < 0 ? -(int)lround(out) : (int)lround(out);
}

int xbox_InputMouseLook(int *x, int *y)
{
    static unsigned traced;
    int dx = XCHG(&s_dx, 0), dy = XCHG(&s_dy, 0);

    *x = *y = 0;
    if (s_opts.look == XBOX_MOUSE_LOOK_NONE || (!dx && !dy))
        return s_opts.look;
    *x = xbox_InputMouseModel(dx, s_opts.sensitivity, s_opts.min_deflection);
    /* Mouse forward is negative dy on Win32 and SDL alike; stick up is
     * positive. */
    *y = xbox_InputMouseModel(-dy, s_opts.sensitivity, s_opts.min_deflection);
    if (s_opts.invert_x) *x = -*x;
    if (s_opts.invert_y) *y = -*y;
    if (recomp_env(RENV_MOUSE_TRACE) && traced < 200) {
        traced++;
        fprintf(stderr, "[MOUSE] dx=%d dy=%d -> %s(%d,%d)\n", dx, dy,
                s_opts.look == XBOX_MOUSE_LOOK_LEFT ? "LS" : "RS", *x, *y);
        fflush(stderr);
    }
    return s_opts.look;
}

void xbox_InputMouseReset(void)
{
    STORE(&s_focus, 0);
    STORE(&s_suspend, 0);
    STORE(&s_key_free, 0);
    STORE(&s_clicked, 0);
    STORE(&s_forced, 0);
    STORE(&s_closed, 0);
    STORE(&s_windowed, 0);
    STORE(&s_captured, 0);
    STORE(&s_activate_click, 0);
    STORE(&s_focus_armed, 0);
    release_all();
    s_wheel_held[0] = s_wheel_held[1] = 0;
}

/* ---- Win32 -------------------------------------------------------------- */
#ifdef _WIN32

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

static int  s_applied;          /* window thread: the cursor is captured */
static int  s_raw_seen;         /* WM_INPUT arrived since the capture */
static int  s_moves;            /* WM_MOUSEMOVE away from the centre */
static int  s_path_logged;
static LONG s_abs_x, s_abs_y;
static int  s_have_abs;
static int  s_wheel_rem;

static void log_path(const char *path)
{
    if (s_path_logged)
        return;
    s_path_logged = 1;
    fprintf(stderr, "[INPUT] mouse: %s\n", path);
    fflush(stderr);
}

static int client_centre(HWND h, POINT *c)
{
    RECT r;
    if (!GetClientRect(h, &r) || r.right <= 0 || r.bottom <= 0)
        return 0;
    c->x = r.right / 2;
    c->y = r.bottom / 2;
    return 1;
}

static void raw_input(HWND h, HRAWINPUT hr)
{
    RAWINPUT ri;
    UINT size = sizeof ri;
    uint32_t now;
    static const USHORT down[5] = {
        RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_3_DOWN,
        RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN };
    static const USHORT up[5] = {
        RI_MOUSE_BUTTON_1_UP, RI_MOUSE_BUTTON_2_UP, RI_MOUSE_BUTTON_3_UP,
        RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP };

    (void)h;
    if (GetRawInputData(hr, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1
        || ri.header.dwType != RIM_TYPEMOUSE)
        return;
    s_raw_seen = 1;
    log_path("raw input");
    if (ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
        /* A tablet or a remote session reports positions; the difference
         * is the motion. */
        if (s_have_abs)
            xbox_InputMouseMotion((int)(ri.data.mouse.lLastX - s_abs_x),
                                  (int)(ri.data.mouse.lLastY - s_abs_y));
        s_abs_x = ri.data.mouse.lLastX;
        s_abs_y = ri.data.mouse.lLastY;
        s_have_abs = 1;
    } else {
        xbox_InputMouseMotion((int)ri.data.mouse.lLastX, (int)ri.data.mouse.lLastY);
    }
    now = (uint32_t)GetTickCount();
    for (int i = 0; i < 5; i++) {
        if (ri.data.mouse.usButtonFlags & down[i]) xbox_InputMouseButtonAt(i + 1, 1, now);
        if (ri.data.mouse.usButtonFlags & up[i])   xbox_InputMouseButton(i + 1, 0);
    }
}

void xbox_InputMouseWin32Msg(void *hwnd, unsigned msg, uintptr_t w, intptr_t l)
{
    HWND h = (HWND)hwnd;
    POINT c;
    uint32_t now;

    switch (msg) {
    case WM_MOUSEACTIVATE:
        /* Sent before the button message of a click on an inactive
         * window; only a click in the client area reaches the pad. */
        if (LOWORD(l) == HTCLIENT)
            xbox_InputMouseActivateClick();
        return;
    case WM_SETFOCUS:  xbox_InputMouseFocusGainedAt((uint32_t)GetTickCount()); return;
    case WM_KILLFOCUS: xbox_InputMouseFocus(1); return;
    case WM_SIZE:
        if (w == SIZE_MINIMIZED) xbox_InputMouseFocus(1);
        return;
    case WM_CLOSE:
    case WM_DESTROY:   xbox_InputMouseClose(); return;
    case WM_INPUT:
        if (s_applied)
            raw_input(h, (HRAWINPUT)l);
        return;
    case WM_MOUSEMOVE:
        /* The fallback for a Wine build whose raw input stays silent: the
         * cursor is put back at the centre after every move, so its offset
         * is the motion. A couple of moves first, so raw input has its
         * chance to arrive. */
        if (!s_applied || s_raw_seen || !client_centre(h, &c))
            return;
        if (GET_X_LPARAM(l) == c.x && GET_Y_LPARAM(l) == c.y)
            return;
        if (++s_moves > 2) {
            log_path("re-centre fallback");
            xbox_InputMouseMotion(GET_X_LPARAM(l) - c.x, GET_Y_LPARAM(l) - c.y);
        }
        ClientToScreen(h, &c);
        SetCursorPos(c.x, c.y);
        return;
    /* The wheel always comes this way: raw_input() leaves it out. */
    case WM_MOUSEWHEEL:
        s_wheel_rem += GET_WHEEL_DELTA_WPARAM(w);
        if (s_wheel_rem >= WHEEL_DELTA || s_wheel_rem <= -WHEEL_DELTA) {
            xbox_InputMouseWheel(s_wheel_rem / WHEEL_DELTA);
            s_wheel_rem %= WHEEL_DELTA;
        }
        return;
    }
    /* Raw input already carries the buttons while it is live; the window
     * messages are the buttons the rest of the time (the click that grabs,
     * the release after the capture ends, the re-centre fallback). */
    if (s_applied && s_raw_seen)
        return;
    now = (uint32_t)GetTickCount();
    switch (msg) {
    case WM_LBUTTONDOWN: xbox_InputMouseButtonAt(XBOX_MOUSE_LEFT, 1, now);   return;
    case WM_LBUTTONUP:   xbox_InputMouseButton(XBOX_MOUSE_LEFT, 0);          return;
    case WM_RBUTTONDOWN: xbox_InputMouseButtonAt(XBOX_MOUSE_RIGHT, 1, now);  return;
    case WM_RBUTTONUP:   xbox_InputMouseButton(XBOX_MOUSE_RIGHT, 0);         return;
    case WM_MBUTTONDOWN: xbox_InputMouseButtonAt(XBOX_MOUSE_MIDDLE, 1, now); return;
    case WM_MBUTTONUP:   xbox_InputMouseButton(XBOX_MOUSE_MIDDLE, 0);        return;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        xbox_InputMouseButtonAt(HIWORD(w) == XBUTTON2 ? XBOX_MOUSE_5 : XBOX_MOUSE_4,
                                msg == WM_XBUTTONDOWN, now);
        return;
    }
}

/* Undo the capture on the host: cursor shown, clip dropped, raw input
 * unregistered. Exchanged, so the window's pump and an exit hook on another
 * thread undo it once. ShowCursor counts per thread, so only the window's
 * thread gets the cursor back that way; the clip is the one that outlives
 * a Wine process. */
static int unapply(void)
{
    RAWINPUTDEVICE rid;

    if (!__atomic_exchange_n(&s_applied, 0, __ATOMIC_ACQ_REL))
        return 0;
    for (int i = 0; i < 8 && ShowCursor(TRUE) < 0; i++)
        ;
    ClipCursor(NULL);
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_REMOVE;
    rid.hwndTarget = NULL;
    RegisterRawInputDevices(&rid, 1, sizeof rid);
    return 1;
}

void xbox_InputMouseWin32Release(void *hwnd)
{
    (void)hwnd;
    xbox_InputMouseClose();
    /* One line, so a close-while-captured run can show the clip went. */
    if (unapply()) {
        fprintf(stderr, "[INPUT] mouse: cursor clip released at exit\n");
        fflush(stderr);
    }
}

static void release_at_exit_hook(const char *why)
{
    (void)why;
    xbox_InputMouseWin32Release(NULL);
}

static void release_at_exit(void)
{
    xbox_InputMouseWin32Release(NULL);
}

void xbox_InputMouseWin32Sync(void *hwnd)
{
    HWND h = (HWND)hwnd;
    int want = xbox_InputMouseCaptured();
    RAWINPUTDEVICE rid;
    POINT c;
    RECT r;

    if (!h)
        return;
    rid.usUsagePage = 0x01;              /* generic desktop */
    rid.usUsage = 0x02;                  /* mouse */
    if (want && !s_applied) {
        /* The display count is per thread and starts at 0; bounded, since
         * with no mouse installed it never reaches the target. */
        for (int i = 0; i < 8 && ShowCursor(FALSE) >= 0; i++)
            ;
        rid.dwFlags = 0;
        rid.hwndTarget = h;
        RegisterRawInputDevices(&rid, 1, sizeof rid);
        s_raw_seen = 0;
        s_moves = 0;
        s_have_abs = 0;
        STORE(&s_applied, 1);
        /* A window close or a guest exit ends the process without another
         * pump: the hooks release the clip on the way out (Wine keeps it
         * after the process is gone). */
        static int hooked;
        if (!hooked) {
            hooked = 1;
            recomp_exit_hook_add(release_at_exit_hook);
            atexit(release_at_exit);
        }
    } else if (!want && s_applied) {
        unapply();
        return;
    }
    if (!s_applied || !GetClientRect(h, &r))
        return;
    /* Every pump: the window may have moved, and a cursor at the centre
     * cannot leave through an edge before the next one. */
    MapWindowPoints(h, NULL, (POINT *)&r, 2);
    ClipCursor(&r);
    if (client_centre(h, &c)) {
        ClientToScreen(h, &c);
        SetCursorPos(c.x, c.y);
    }
    /* The exit hook may release on another thread between the checks above
     * and this clip, and then nothing would undo it: Close is set before
     * that release, so look again once the clip is in place. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_closed, __ATOMIC_SEQ_CST)) {
        unapply();
        ClipCursor(NULL);
    }
}

#endif /* _WIN32 */
