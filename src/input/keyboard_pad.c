/*
 * keyboard_pad.c -- the host keyboard, as an Xbox controller.
 *
 * See keyboard_pad.h for the mapping and why it is separated from where the
 * keys come from. This file holds the table and the report builder, and no
 * state of its own beyond the packet counter.
 */
#include "keyboard_pad.h"

#include <string.h>

/* The one mapping. Adding a key for a later menu is one line here and nothing
 * else; nothing downstream knows which key produced a control. */
const XBOX_KEY_MAP xbox_KeyboardMap[] = {
    /* d-pad */
    { VK_UP,        XBOX_GAMEPAD_DPAD_UP,      -1 },
    { VK_DOWN,      XBOX_GAMEPAD_DPAD_DOWN,    -1 },
    { VK_LEFT,      XBOX_GAMEPAD_DPAD_LEFT,    -1 },
    { VK_RIGHT,     XBOX_GAMEPAD_DPAD_RIGHT,   -1 },
    /* start / back */
    { VK_RETURN,    XBOX_GAMEPAD_START,        -1 },
    { VK_BACK,      XBOX_GAMEPAD_BACK,         -1 },
    /* thumb clicks */
    { VK_SHIFT,     XBOX_GAMEPAD_LEFT_THUMB,   -1 },
    { VK_CONTROL,   XBOX_GAMEPAD_RIGHT_THUMB,  -1 },
    /* face buttons, analog on the console so a key is 255 rather than a flag:
     * a title that reads these as a pressure never sees a press if they are 1 */
    { 'Z',          0,                          XBOX_BUTTON_A },
    { 'X',          0,                          XBOX_BUTTON_B },
    { 'A',          0,                          XBOX_BUTTON_X },
    { 'S',          0,                          XBOX_BUTTON_Y },
    { 'Q',          0,                          XBOX_BUTTON_WHITE },
    { 'E',          0,                          XBOX_BUTTON_BLACK },
    { '1',          0,                          XBOX_BUTTON_LTRIGGER },
    { '3',          0,                          XBOX_BUTTON_RTRIGGER },
    { 0,            0,                          -1 },   /* terminator */
};

/* Left thumb shares the numeric pad because W/A/S/D collide with the face
 * buttons above; the right thumb uses the I/J/K/L cluster. */
#define KB_LX_NEG VK_NUMPAD4
#define KB_LX_POS VK_NUMPAD6
#define KB_LY_NEG VK_NUMPAD2
#define KB_LY_POS VK_NUMPAD8
#define KB_RX_NEG 'J'
#define KB_RX_POS 'L'
#define KB_RY_NEG 'K'
#define KB_RY_POS 'I'

SHORT xbox_KeyboardAxis(int negative, int positive)
{
    int v = 0;

    if (negative && negative != positive)
        v -= 32767;
    if (positive && positive != negative)
        v += 32767;
    return (SHORT)v;
}

void xbox_KeyboardPadState(XBOX_INPUT_STATE *pState,
                           int (*key_down)(int vk))
{
    /* The previous state, so the packet number moves only when it changes.
     * Static and single-threaded by contract: the only caller is the input
     * path on port 0. */
    static DWORD packet;
    static XBOX_GAMEPAD prev;
    static int have_prev;
    XBOX_GAMEPAD cur;
    const XBOX_KEY_MAP *m;

    if (!pState || !key_down)
        return;

    memset(&cur, 0, sizeof cur);
    for (m = xbox_KeyboardMap; m->vk; m++) {
        if (!key_down(m->vk))
            continue;
        if (m->button)
            cur.wButtons |= m->button;
        else if (m->analog >= 0 && m->analog < 8)
            cur.bAnalogButtons[m->analog] = 255;
    }

    cur.sThumbLX = xbox_KeyboardAxis(key_down(KB_LX_NEG),
                                     key_down(KB_LX_POS));
    cur.sThumbLY = xbox_KeyboardAxis(key_down(KB_LY_NEG),
                                     key_down(KB_LY_POS));
    cur.sThumbRX = xbox_KeyboardAxis(key_down(KB_RX_NEG),
                                     key_down(KB_RX_POS));
    cur.sThumbRY = xbox_KeyboardAxis(key_down(KB_RY_NEG),
                                     key_down(KB_RY_POS));

    /* The title's input layer records edges, so an unchanged packet number is
     * read as the same state and a press is never noticed. */
    if (!have_prev || memcmp(&cur, &prev, sizeof cur) != 0) {
        prev = cur;
        have_prev = 1;
        packet++;
    }

    memset(pState, 0, sizeof *pState);
    pState->dwPacketNumber = packet;
    pState->Gamepad = cur;
}
