/*
 * ui/xemu-settings.h shim for the pinned xemu DSP56300 sources (A4b1 step 2).
 *
 * The pinned dsp.c includes this for `g_config.audio.use_dsp_jit`. The toolkit
 * already carries a `g_config` shim in src/apu/apu_shim.h; this header is a
 * pass-through onto it rather than a second definition, so the pinned code and
 * the toolkit see one settings object.
 *
 * The two fields the pinned code reads are set by that shim to the values
 * Device semantics 2 requires, with no environment variable:
 *   use_dsp     = true   -- the GP is enabled unconditionally
 *   use_dsp_jit = false  -- the C interpreter, never the JIT (a packet non-goal)
 */

#ifndef XBOXRECOMP_SHIM_UI_XEMU_SETTINGS_H
#define XBOXRECOMP_SHIM_UI_XEMU_SETTINGS_H

#include "apu_shim.h"

#endif /* XBOXRECOMP_SHIM_UI_XEMU_SETTINGS_H */
