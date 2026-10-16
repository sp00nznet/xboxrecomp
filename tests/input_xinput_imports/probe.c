/* Calls the three XInput entry points the input backend uses, so the linker
 * keeps the XInput import the toolkit's link line selects. Run on Windows it
 * only has to start (with no pad connected the calls just fail); the loader
 * stops it with 0xC0000135 first if the imported DLL is missing. */
#include <windows.h>
#include <xinput.h>

int main(void)
{
    XINPUT_STATE st;
    XINPUT_VIBRATION vib = {0};
    XINPUT_CAPABILITIES caps;
    DWORD rc = XInputGetState(0, &st);
    rc ^= XInputSetState(0, &vib);
    rc ^= XInputGetCapabilities(0, XINPUT_FLAG_GAMEPAD, &caps);
    return (int)rc;
}
