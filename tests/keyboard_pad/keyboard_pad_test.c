/*
 * keyboard_pad_test.c -- the Enter -> Xbox Start boundary, tested directly.
 *
 * The plan's T1-T5. These exercise the same table and the same report builder
 * the real window path uses (src/input/keyboard_pad.c, called from
 * xinput_device.c's keyboard_state, which is what usb_gamepad_report consumes),
 * not a second implementation of the mapping that could agree with itself
 * while the runtime disagrees with both.
 *
 * The key state here is a bit array the test owns, exactly as the framebuffer
 * window owns the real one. Nothing about the mapping is re-declared: the
 * assertions read XBOX_GAMEPAD_START and xbox_KeyboardMap, the same symbols
 * the runtime links.
 */
#include "keyboard_pad.h"

#include <stdio.h>
#include <string.h>

static unsigned char g_keys[256];

static int test_key_down(int vk)
{
    return (unsigned)vk < 256 && g_keys[vk] != 0;
}

static void press(int vk)   { g_keys[vk] = 1; }
static void release(int vk) { g_keys[vk] = 0; }
static void release_all(void) { memset(g_keys, 0, sizeof g_keys); }

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* The mapping table is the authority for what Enter means; assert against it
 * rather than restating the bit. */
static const XBOX_KEY_MAP *entry_for(int vk)
{
    const XBOX_KEY_MAP *m;

    for (m = xbox_KeyboardMap; m->vk; m++)
        if (m->vk == vk)
            return m;
    return NULL;
}

int main(void)
{
    XBOX_INPUT_STATE s;
    const XBOX_KEY_MAP *enter;

    /* --- the table itself ------------------------------------------------- */

    enter = entry_for(VK_RETURN);
    check(enter != NULL, "Enter is in the mapping table");
    check(enter && enter->button == XBOX_GAMEPAD_START,
          "Enter maps to XBOX_GAMEPAD_START (the toolkit's own constant)");
    check(entry_for(VK_UP) && entry_for(VK_UP)->button == XBOX_GAMEPAD_DPAD_UP,
          "Up maps to d-pad up (K17's first direction)");
    check(entry_for(VK_BACK) && entry_for(VK_BACK)->button == XBOX_GAMEPAD_BACK,
          "Backspace maps to BACK (K17's cancel)");

    /* --- T1: Start down --------------------------------------------------- */

    release_all();
    xbox_KeyboardPadState(&s, test_key_down);
    check((s.Gamepad.wButtons & XBOX_GAMEPAD_START) == 0,
          "T1 precondition: Start is clear with no key held");

    press(VK_RETURN);
    xbox_KeyboardPadState(&s, test_key_down);
    check((s.Gamepad.wButtons & XBOX_GAMEPAD_START) != 0,
          "T1: Enter down asserts Start");

    /* --- T2: Start up ----------------------------------------------------- */

    release(VK_RETURN);
    xbox_KeyboardPadState(&s, test_key_down);
    check((s.Gamepad.wButtons & XBOX_GAMEPAD_START) == 0,
          "T2: Enter up clears Start");

    /* --- T3: no sticky state on focus loss -------------------------------- */

    press(VK_RETURN);
    xbox_KeyboardPadState(&s, test_key_down);
    check((s.Gamepad.wButtons & XBOX_GAMEPAD_START) != 0,
          "T3 precondition: Start asserted while Enter is held");

    /* What WM_KILLFOCUS does to the window's array, done to this one. */
    release_all();
    xbox_KeyboardPadState(&s, test_key_down);
    check((s.Gamepad.wButtons & XBOX_GAMEPAD_START) == 0,
          "T3: a focus-loss reset clears a held Start");

    /* --- T4: an unrelated key does not assert Start ----------------------- */

    release_all();
    press('Z');                     /* A button, not Start */
    press(VK_LEFT);                 /* d-pad, not Start    */
    xbox_KeyboardPadState(&s, test_key_down);
    check((s.Gamepad.wButtons & XBOX_GAMEPAD_START) == 0,
          "T4: A and Left do not assert Start");
    check((s.Gamepad.wButtons & XBOX_GAMEPAD_DPAD_LEFT) != 0,
          "T4 control: Left did assert the d-pad, so the state was built");
    check(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255,
          "T4 control: Z did assert A at full pressure");

    /* --- packet number moves on a change, and only on a change ------------ */

    release_all();
    xbox_KeyboardPadState(&s, test_key_down);
    {
        DWORD a = s.dwPacketNumber;
        xbox_KeyboardPadState(&s, test_key_down);
        check(s.dwPacketNumber == a,
              "packet number does not move when nothing changed");
        press(VK_RETURN);
        xbox_KeyboardPadState(&s, test_key_down);
        check(s.dwPacketNumber != a,
              "packet number moves when Start appears (the title reads edges)");
    }

    if (!failures)
        printf("PASS keyboard pad: Enter->Start down/up, focus reset, unrelated "
               "key, packet edges\n");
    return failures ? 1 : 0;
}
