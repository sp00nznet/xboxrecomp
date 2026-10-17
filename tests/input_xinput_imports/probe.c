/* Calls the two XInput entry points the input backend uses, so the linker
 * keeps the XInput import the toolkit's link line selects. Never run. */
#include <windows.h>
#include <xinput.h>

int main(void)
{
    XINPUT_STATE st;
    XINPUT_VIBRATION vib = {0};
    DWORD rc = XInputGetState(0, &st);
    return (int)(rc ^ XInputSetState(0, &vib));
}
