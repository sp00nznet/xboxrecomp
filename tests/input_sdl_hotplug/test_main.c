#include "xinput_xbox.h"
#include <SDL.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct SDL_Joystick {
    SDL_JoystickID instance_id;
};

struct SDL_GameController {
    struct SDL_Joystick joystick;
    int attached;
};

static struct SDL_GameController s_controller;
static int s_device_count;
static SDL_JoystickID s_device_instance;
static int s_open_count;
static int s_close_count;
static int s_update_count;
static pthread_mutex_t s_joystick_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int s_lock_balance;
static int s_fail;

#define CHECK(c) do { \
    if (!(c)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
        s_fail = 1; \
    } \
} while (0)

int xbox_InputKeyboardPoll(XBOX_INPUT_STATE *state)
{
    (void)state;
    return 0;
}

int SDL_SetHint(const char *name, const char *value)
{
    (void)name;
    (void)value;
    return 1;
}

Uint32 SDL_WasInit(Uint32 flags)
{
    (void)flags;
    return SDL_INIT_GAMECONTROLLER;
}

int SDL_InitSubSystem(Uint32 flags)
{
    (void)flags;
    return 0;
}

int SDL_NumJoysticks(void)
{
    return s_device_count;
}

int SDL_IsGameController(int joystick_index)
{
    return joystick_index >= 0 && joystick_index < s_device_count;
}

SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int joystick_index)
{
    return SDL_IsGameController(joystick_index) ? s_device_instance : -1;
}

SDL_GameController *SDL_GameControllerOpen(int joystick_index)
{
    if (!SDL_IsGameController(joystick_index))
        return NULL;
    s_controller.joystick.instance_id = s_device_instance;
    s_controller.attached = 1;
    s_open_count++;
    return &s_controller;
}

void SDL_GameControllerClose(SDL_GameController *controller)
{
    if (controller != &s_controller)
        abort();
    if (controller)
        controller->attached = 0;
    s_close_count++;
}

int SDL_GameControllerGetAttached(SDL_GameController *controller)
{
    return controller && controller->attached;
}

SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *controller)
{
    return controller ? &controller->joystick : NULL;
}

SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *joystick)
{
    return joystick ? joystick->instance_id : -1;
}

void SDL_GameControllerUpdate(void)
{
    s_update_count++;
}

Uint8 SDL_GameControllerGetButton(SDL_GameController *controller, int button)
{
    (void)controller;
    return button == SDL_CONTROLLER_BUTTON_A;
}

Sint16 SDL_GameControllerGetAxis(SDL_GameController *controller, int axis)
{
    (void)controller;
    (void)axis;
    return 0;
}

int SDL_GameControllerRumble(SDL_GameController *controller,
                             uint16_t low_frequency_rumble,
                             uint16_t high_frequency_rumble,
                             Uint32 duration_ms)
{
    (void)controller;
    (void)low_frequency_rumble;
    (void)high_frequency_rumble;
    (void)duration_ms;
    return 0;
}

void SDL_LockJoysticks(void)
{
    if (pthread_mutex_lock(&s_joystick_lock) != 0)
        abort();
    atomic_fetch_add_explicit(&s_lock_balance, 1, memory_order_relaxed);
}

void SDL_UnlockJoysticks(void)
{
    if (atomic_fetch_sub_explicit(&s_lock_balance, 1, memory_order_relaxed) != 1)
        abort();
    if (pthread_mutex_unlock(&s_joystick_lock) != 0)
        abort();
}

int SDL_AtomicGet(SDL_atomic_t *value)
{
    return atomic_load_explicit(&value->value, memory_order_seq_cst);
}

int SDL_AtomicSet(SDL_atomic_t *value, int desired)
{
    return atomic_exchange_explicit(&value->value, desired, memory_order_seq_cst);
}

static void *poll_controller(void *unused)
{
    XBOX_INPUT_STATE state;
    (void)unused;
    for (int i = 0; i < 2000; i++)
        (void)xbox_InputGetState(0, &state);
    return NULL;
}

static void *hotplug_controller(void *unused)
{
    (void)unused;
    for (int i = 0; i < 2000; i++) {
        SDL_LockJoysticks();
        if (i & 1) {
            s_device_count = 1;
            s_device_instance++;
        } else {
            s_controller.attached = 0;
            s_device_count = 0;
        }
        SDL_UnlockJoysticks();
        xbox_InputSetMainPump(i & 1);
    }
    return NULL;
}

int main(void)
{
    XBOX_INPUT_STATE state;
    pthread_t poll_thread, hotplug_thread;

    xbox_InputInit();
    memset(&state, 0, sizeof state);
    CHECK(xbox_InputGetState(0, &state) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(s_open_count == 0);

    s_device_count = 1;
    s_device_instance = 10;
    memset(&state, 0, sizeof state);
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
    CHECK(s_open_count == 1);
    CHECK(s_update_count == 2);
    CHECK(state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    CHECK(xbox_InputIsConnected(0));

    s_controller.attached = 0;
    s_device_count = 0;
    CHECK(xbox_InputGetState(0, &state) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(s_close_count == 1);
    CHECK(!xbox_InputIsConnected(0));

    s_device_count = 1;
    s_device_instance = 11;
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
    CHECK(s_open_count == 2);

    xbox_InputSetMainPump(1);
    int updates_before = s_update_count;
    s_controller.attached = 0;
    s_device_count = 0;
    CHECK(xbox_InputGetState(0, &state) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(s_update_count == updates_before);
    CHECK(s_close_count == 2);

    s_device_count = 1;
    s_device_instance = 12;
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
    CHECK(s_open_count == 3);
    CHECK(s_update_count == updates_before);
    CHECK(xbox_InputAnyOpen());

    CHECK(pthread_create(&poll_thread, NULL, poll_controller, NULL) == 0);
    CHECK(pthread_create(&hotplug_thread, NULL, hotplug_controller, NULL) == 0);
    CHECK(pthread_join(poll_thread, NULL) == 0);
    CHECK(pthread_join(hotplug_thread, NULL) == 0);

    SDL_LockJoysticks();
    s_device_count = 1;
    s_device_instance++;
    SDL_UnlockJoysticks();
    xbox_InputSetMainPump(1);
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
    CHECK(atomic_load_explicit(&s_lock_balance, memory_order_relaxed) == 0);

    if (!s_fail)
        puts("SDL controller hotplug: ok");
    return s_fail;
}
