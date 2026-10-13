/*
 * pad_map.h -- pure translations between host pad data and Xbox pad data.
 *
 * Nothing here touches XInput, USB or the clock, so the mappings can be
 * asserted on any platform by a test that links pad_map.c alone.
 */
#ifndef XBOX_PAD_MAP_H
#define XBOX_PAD_MAP_H

#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* XINPUT_GAMEPAD's fields, without <xinput.h>. Same widths and meanings. */
typedef struct XBOX_XINPUT_GAMEPAD {
    uint16_t wButtons;
    uint8_t  bLeftTrigger;
    uint8_t  bRightTrigger;
    int16_t  sThumbLX;
    int16_t  sThumbLY;
    int16_t  sThumbRX;
    int16_t  sThumbRY;
} XBOX_XINPUT_GAMEPAD;

/* XINPUT_GAMEPAD_* values for the buttons that need translating. The low
 * byte (d-pad, Start, Back, thumb clicks) has the same bits on both sides. */
#define XBOX_XI_LEFT_SHOULDER   0x0100
#define XBOX_XI_RIGHT_SHOULDER  0x0200
#define XBOX_XI_A               0x1000
#define XBOX_XI_B               0x2000
#define XBOX_XI_X               0x4000
#define XBOX_XI_Y               0x8000

/* Translate an XInput gamepad to an Xbox one. Zeroes `out` first and leaves
 * dwPacketNumber at 0 for the caller to fill in.
 *
 *   A B X Y         analog 255 / 0
 *   LB / RB         White / Black (the xemu and Cxbx-Reloaded layout)
 *   triggers        raw 0..255
 *   sticks          raw
 *   wButtons        in & 0x00FF
 */
void xbox_PadFromXInput(const XBOX_XINPUT_GAMEPAD *in, XBOX_INPUT_STATE *out);

/* Parse the gamepad's 6-byte interrupt-OUT rumble report:
 *
 *   [0x00, 0x06, left_lo, left_hi, right_lo, right_hi]
 *
 * Each motor is a 16-bit little-endian value, which XInput's 0..65535 takes
 * as is. Returns 0 on success; nonzero if `len` < 6 or byte 1 is not 6. */
int xbox_RumbleFromReport(const uint8_t *data, int len, XBOX_VIBRATION *out);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_PAD_MAP_H */
