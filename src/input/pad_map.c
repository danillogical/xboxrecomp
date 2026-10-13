/*
 * pad_map.c -- pure pad translations; see pad_map.h.
 */
#include "pad_map.h"

#include <string.h>

void xbox_PadFromXInput(const XBOX_XINPUT_GAMEPAD *in, XBOX_INPUT_STATE *out)
{
    memset(out, 0, sizeof *out);

    out->Gamepad.wButtons = (WORD)(in->wButtons & 0x00FF);

    out->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = (in->wButtons & XBOX_XI_A) ? 255 : 0;
    out->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = (in->wButtons & XBOX_XI_B) ? 255 : 0;
    out->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = (in->wButtons & XBOX_XI_X) ? 255 : 0;
    out->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = (in->wButtons & XBOX_XI_Y) ? 255 : 0;
    out->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (in->wButtons & XBOX_XI_LEFT_SHOULDER) ? 255 : 0;
    out->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (in->wButtons & XBOX_XI_RIGHT_SHOULDER) ? 255 : 0;
    out->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = in->bLeftTrigger;
    out->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = in->bRightTrigger;

    out->Gamepad.sThumbLX = in->sThumbLX;
    out->Gamepad.sThumbLY = in->sThumbLY;
    out->Gamepad.sThumbRX = in->sThumbRX;
    out->Gamepad.sThumbRY = in->sThumbRY;
}

int xbox_RumbleFromReport(const uint8_t *data, int len, XBOX_VIBRATION *out)
{
    if (!data || !out || len < 6 || data[1] != 6)
        return 1;
    out->wLeftMotorSpeed  = (WORD)(data[2] | (data[3] << 8));
    out->wRightMotorSpeed = (WORD)(data[4] | (data[5] << 8));
    return 0;
}
