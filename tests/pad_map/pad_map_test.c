/*
 * pad_map_test.c -- the XInput -> Xbox pad mapping and the rumble report
 * parser, tested against the written spec in pad_map.h.
 *
 * Expected values are literals (255, 0x1234, the XInput bit values), not the
 * mapping's own constants, so a wrong table cannot agree with itself.
 */
#include "pad_map.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void map(const XBOX_XINPUT_GAMEPAD *in, XBOX_INPUT_STATE *out)
{
    memset(out, 0xAA, sizeof *out);
    xbox_PadFromXInput(in, out);
}

/* Every analog button except the one at `except` (or -1) must be 0. */
static int others_zero(const XBOX_INPUT_STATE *s, int except)
{
    int i;
    for (i = 0; i < 8; i++)
        if (i != except && s->Gamepad.bAnalogButtons[i] != 0)
            return 0;
    return 1;
}

static void test_released(void)
{
    XBOX_XINPUT_GAMEPAD in;
    XBOX_INPUT_STATE s;
    int i;

    memset(&in, 0, sizeof in);
    map(&in, &s);
    check(s.Gamepad.wButtons == 0, "released: wButtons 0");
    for (i = 0; i < 8; i++)
        check(s.Gamepad.bAnalogButtons[i] == 0, "released: analog button 0");
    check(s.Gamepad.sThumbLX == 0 && s.Gamepad.sThumbLY == 0 &&
          s.Gamepad.sThumbRX == 0 && s.Gamepad.sThumbRY == 0,
          "released: sticks 0");
    check(s.dwPacketNumber == 0, "dwPacketNumber left at 0 (out was 0xAA-filled)");
}

static void test_low_byte(void)
{
    static const unsigned bits[] = { 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80 };
    XBOX_XINPUT_GAMEPAD in;
    XBOX_INPUT_STATE s;
    size_t i;

    for (i = 0; i < sizeof bits / sizeof bits[0]; i++) {
        memset(&in, 0, sizeof in);
        in.wButtons = (uint16_t)bits[i];
        map(&in, &s);
        check(s.Gamepad.wButtons == bits[i], "low-byte button keeps its bit");
        check(others_zero(&s, -1), "low-byte button sets no analog button");
    }

    memset(&in, 0, sizeof in);
    in.wButtons = 0x00FF;
    map(&in, &s);
    check(s.Gamepad.wButtons == 0x00FF, "all low-byte buttons together");

    /* Shoulder and face bits never reach wButtons. */
    memset(&in, 0, sizeof in);
    in.wButtons = 0xFF00;
    map(&in, &s);
    check(s.Gamepad.wButtons == 0, "shoulder/face bits absent from wButtons");

    memset(&in, 0, sizeof in);
    in.wButtons = 0xFFFF;
    map(&in, &s);
    check(s.Gamepad.wButtons == 0x00FF, "0xFFFF masks to 0x00FF");
}

static void test_face_and_shoulders(void)
{
    static const struct { unsigned xi; int idx; const char *name; } t[] = {
        { 0x1000, XBOX_BUTTON_A,     "A" },
        { 0x2000, XBOX_BUTTON_B,     "B" },
        { 0x4000, XBOX_BUTTON_X,     "X" },
        { 0x8000, XBOX_BUTTON_Y,     "Y" },
        { 0x0100, XBOX_BUTTON_WHITE, "LB -> WHITE" },
        { 0x0200, XBOX_BUTTON_BLACK, "RB -> BLACK" },
    };
    XBOX_XINPUT_GAMEPAD in;
    XBOX_INPUT_STATE s;
    char msg[64];
    size_t i;

    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        memset(&in, 0, sizeof in);
        in.wButtons = (uint16_t)t[i].xi;
        map(&in, &s);
        snprintf(msg, sizeof msg, "%s is 255", t[i].name);
        check(s.Gamepad.bAnalogButtons[t[i].idx] == 255, msg);
        snprintf(msg, sizeof msg, "%s sets nothing else analog", t[i].name);
        check(others_zero(&s, t[i].idx), msg);
        snprintf(msg, sizeof msg, "%s leaves wButtons 0", t[i].name);
        check(s.Gamepad.wButtons == 0, msg);
    }

    /* The index constants themselves, so a renumbered header is noticed. */
    check(XBOX_BUTTON_BLACK == 4 && XBOX_BUTTON_WHITE == 5,
          "BLACK is index 4, WHITE is index 5");

    memset(&in, 0, sizeof in);
    in.wButtons = 0x1000 | 0x2000 | 0x4000 | 0x8000 | 0x0100 | 0x0200;
    map(&in, &s);
    check(s.Gamepad.bAnalogButtons[0] == 255 && s.Gamepad.bAnalogButtons[1] == 255 &&
          s.Gamepad.bAnalogButtons[2] == 255 && s.Gamepad.bAnalogButtons[3] == 255 &&
          s.Gamepad.bAnalogButtons[4] == 255 && s.Gamepad.bAnalogButtons[5] == 255,
          "all face+shoulder together: six buttons 255");
    check(s.Gamepad.bAnalogButtons[6] == 0 && s.Gamepad.bAnalogButtons[7] == 0,
          "buttons do not drive the trigger slots");
}

static void test_triggers_and_sticks(void)
{
    static const uint8_t trig[] = { 0, 1, 128, 255 };
    XBOX_XINPUT_GAMEPAD in;
    XBOX_INPUT_STATE s;
    size_t i;

    for (i = 0; i < sizeof trig; i++) {
        memset(&in, 0, sizeof in);
        in.bLeftTrigger = trig[i];
        map(&in, &s);
        check(s.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == trig[i],
              "left trigger passes through");
        check(others_zero(&s, XBOX_BUTTON_LTRIGGER), "left trigger only");

        memset(&in, 0, sizeof in);
        in.bRightTrigger = trig[i];
        map(&in, &s);
        check(s.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == trig[i],
              "right trigger passes through");
        check(others_zero(&s, XBOX_BUTTON_RTRIGGER), "right trigger only");
    }

    memset(&in, 0, sizeof in);
    in.sThumbLX = -32768; in.sThumbLY = 32767;
    in.sThumbRX = 32767;  in.sThumbRY = -32768;
    map(&in, &s);
    check(s.Gamepad.sThumbLX == -32768, "LX -32768");
    check(s.Gamepad.sThumbLY == 32767,  "LY 32767");
    check(s.Gamepad.sThumbRX == 32767,  "RX 32767");
    check(s.Gamepad.sThumbRY == -32768, "RY -32768");

    memset(&in, 0, sizeof in);
    in.sThumbLX = 1; in.sThumbLY = -1; in.sThumbRX = 0; in.sThumbRY = 12345;
    map(&in, &s);
    check(s.Gamepad.sThumbLX == 1 && s.Gamepad.sThumbLY == -1 &&
          s.Gamepad.sThumbRX == 0 && s.Gamepad.sThumbRY == 12345,
          "sticks pass through raw (no axis swap)");
}

static void test_rumble(void)
{
    XBOX_VIBRATION v;
    static const uint8_t zero[6]  = { 0, 6, 0, 0, 0, 0 };
    static const uint8_t mixed[6] = { 0, 6, 0x34, 0x12, 0xFF, 0xFF };
    static const uint8_t half[6]  = { 0, 6, 0x00, 0x80, 0x00, 0x00 };
    static const uint8_t longer[8] = { 0, 6, 0x01, 0x02, 0x03, 0x04, 0xEE, 0xEE };
    static const uint8_t badhdr[6] = { 0, 5, 1, 2, 3, 4 };
    static const uint8_t okhdr[6]  = { 0, 6, 1, 2, 3, 4 };

    memset(&v, 0xAA, sizeof v);
    check(xbox_RumbleFromReport(zero, 6, &v) == 0, "zero report accepted");
    check(v.wLeftMotorSpeed == 0 && v.wRightMotorSpeed == 0, "zero report values");

    check(xbox_RumbleFromReport(mixed, 6, &v) == 0, "mixed report accepted");
    check(v.wLeftMotorSpeed == 0x1234, "left 0x1234 (little-endian 16-bit)");
    check(v.wRightMotorSpeed == 0xFFFF, "right 0xFFFF");

    check(xbox_RumbleFromReport(half, 6, &v) == 0, "half report accepted");
    check(v.wLeftMotorSpeed == 32768, "left 32768 (high byte used)");
    check(v.wRightMotorSpeed == 0, "right 0");

    check(xbox_RumbleFromReport(longer, 8, &v) == 0, "len 8 accepted");
    check(v.wLeftMotorSpeed == 0x0201 && v.wRightMotorSpeed == 0x0403,
          "len 8: only the first 6 bytes used");

    /* Failures return nonzero and leave `out` untouched. */
    memset(&v, 0xAA, sizeof v);
    check(xbox_RumbleFromReport(okhdr, 5, &v) != 0, "len 5 rejected");
    check(v.wLeftMotorSpeed == 0xAAAA && v.wRightMotorSpeed == 0xAAAA,
          "len 5: out not written");
    check(xbox_RumbleFromReport(okhdr, 0, &v) != 0, "len 0 rejected");
    check(xbox_RumbleFromReport(okhdr, -1, &v) != 0, "len -1 rejected");
    check(v.wLeftMotorSpeed == 0xAAAA && v.wRightMotorSpeed == 0xAAAA,
          "len 0/-1: out not written");
    check(xbox_RumbleFromReport(badhdr, 6, &v) != 0, "data[1] != 6 rejected");
    check(v.wLeftMotorSpeed == 0xAAAA && v.wRightMotorSpeed == 0xAAAA,
          "bad header: out not written");
    check(xbox_RumbleFromReport(NULL, 6, &v) != 0, "NULL data rejected");
    check(v.wLeftMotorSpeed == 0xAAAA && v.wRightMotorSpeed == 0xAAAA,
          "NULL data: out not written");
}

int main(void)
{
    test_released();
    test_low_byte();
    test_face_and_shoulders();
    test_triggers_and_sticks();
    test_rumble();

    if (!failures)
        printf("PASS pad map: XInput->Xbox buttons/triggers/sticks, rumble report\n");
    return failures ? 1 : 0;
}
