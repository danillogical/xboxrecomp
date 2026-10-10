/*
 * keyboard_pad.h -- the host keyboard, as an Xbox controller.
 *
 * A title that waits on PRESS START is unreachable on a machine with no
 * controller plugged in, which is most machines someone brings this up on.
 * The whole point of a recompilation is to be able to look at the thing
 * running, and a build nobody can press a button in cannot be looked at.
 *
 * This is the mapping, in one place, with no policy about where the keys come
 * from. The caller supplies `key_down`; the framebuffer window owns the only
 * real key state (it is the window that receives WM_KEYDOWN, and only while it
 * has the focus), and a test owns a synthetic one. Both therefore exercise
 * this same table and this same report builder rather than a copy of either.
 *
 * The keys are the ones a Dreamcast or Saturn emulator would pick, which is
 * the closest thing to a convention here:
 *
 *   arrows        d-pad             Enter      START
 *   Z X A S       A B X Y           Backspace  BACK
 *   Q E           white black       1 3        triggers
 *   numpad 8/2/4/6 left thumb       I/K/J/L    right thumb
 */
#ifndef XBOX_KEYBOARD_PAD_H
#define XBOX_KEYBOARD_PAD_H

#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One virtual-key -> Xbox control mapping.
 *
 * `button` is a digital flag in the XBOX_GAMEPAD_* bitmask, or 0 when the
 * entry is an analog button. `analog` is an XBOX_BUTTON_* index, or -1 when
 * the entry is digital. A key is one or the other, never both. */
typedef struct {
    int      vk;      /* Win32 virtual key                       */
    uint16_t button;  /* XBOX_GAMEPAD_* flag, or 0               */
    int      analog;  /* XBOX_BUTTON_* index, or -1              */
} XBOX_KEY_MAP;

/* The digital/analog table, terminated by vk == 0.
 *
 * Exposed so a test asserts against the data the runtime uses instead of a
 * second copy of it that can drift. */
extern const XBOX_KEY_MAP xbox_KeyboardMap[];

/* One thumb axis from two keys, at the full deflection a digital key
 * implies. Either key may be 0, meaning "no key on that side". */
SHORT xbox_KeyboardAxis(int negative, int positive);

/* Build the port-0 pad state the keyboard is asserting right now.
 *
 * `key_down(vk)` answers "is this virtual key held" and must be safe to call
 * for any virtual key the table names. The report is zeroed first, so a key
 * that is not held contributes nothing.
 *
 * The packet number advances whenever the produced state differs from the
 * previous call, because the title's input layer records edges and an
 * unchanged packet number is read as the same state. */
void xbox_KeyboardPadState(XBOX_INPUT_STATE *pState,
                           int (*key_down)(int vk));

#ifdef __cplusplus
}
#endif

#endif /* XBOX_KEYBOARD_PAD_H */
