#include "input_map.h"
int main()
{
    InputPadMap map;
    InputRaw raw = {};
    XBOX_GAMEPAD out;
    InputBindings b = {};
    uint8_t down[INPUT_KEY_COUNT] = {};
    xbox_InputMap_PadMapDefaults(&map);
    raw.src[INPUT_SRC_SOUTH] = 32767;
    xbox_InputMap_PadMap(&map, &raw, &out);
    if (out.bAnalogButtons[XBOX_BUTTON_A] != 255)
        return 1;
    xbox_InputMap_BindingsParse(&b, INPUT_START, "space");
    down[XBOX_VK_SPACE] = 1;
    xbox_InputMap_KeysResolve(&b, down, &out);
    return out.wButtons == XBOX_GAMEPAD_START ? 0 : 1;
}
