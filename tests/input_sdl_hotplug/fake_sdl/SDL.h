#ifndef TEST_FAKE_SDL_H
#define TEST_FAKE_SDL_H

#include <stdatomic.h>
#include <stdint.h>

typedef uint8_t Uint8;
typedef int16_t Sint16;
typedef uint32_t Uint32;
typedef int32_t SDL_JoystickID;

typedef struct SDL_GameController SDL_GameController;
typedef struct SDL_Joystick SDL_Joystick;
typedef struct SDL_atomic_t {
    _Atomic int value;
} SDL_atomic_t;

#define SDL_INIT_GAMECONTROLLER 0x00002000u

enum {
    SDL_CONTROLLER_BUTTON_DPAD_UP,
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_LEFTSTICK,
    SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_A,
    SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_X,
    SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER
};

enum {
    SDL_CONTROLLER_AXIS_TRIGGERLEFT,
    SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
    SDL_CONTROLLER_AXIS_LEFTX,
    SDL_CONTROLLER_AXIS_LEFTY,
    SDL_CONTROLLER_AXIS_RIGHTX,
    SDL_CONTROLLER_AXIS_RIGHTY
};

#define SDL_HINT_NO_SIGNAL_HANDLERS "SDL_NO_SIGNAL_HANDLERS"

int SDL_SetHint(const char *name, const char *value);
Uint32 SDL_WasInit(Uint32 flags);
int SDL_InitSubSystem(Uint32 flags);
int SDL_NumJoysticks(void);
int SDL_IsGameController(int joystick_index);
SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int joystick_index);
SDL_GameController *SDL_GameControllerOpen(int joystick_index);
void SDL_GameControllerClose(SDL_GameController *controller);
int SDL_GameControllerGetAttached(SDL_GameController *controller);
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *controller);
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *joystick);
void SDL_GameControllerUpdate(void);
Uint8 SDL_GameControllerGetButton(SDL_GameController *controller, int button);
Sint16 SDL_GameControllerGetAxis(SDL_GameController *controller, int axis);
int SDL_GameControllerRumble(SDL_GameController *controller,
                             uint16_t low_frequency_rumble,
                             uint16_t high_frequency_rumble,
                             Uint32 duration_ms);
void SDL_LockJoysticks(void);
void SDL_UnlockJoysticks(void);
int SDL_AtomicGet(SDL_atomic_t *value);
int SDL_AtomicSet(SDL_atomic_t *value, int desired);

#endif
