/*
 * qemu/osdep.h shim for the pinned xemu DSP56300 sources (A4b1 step 2).
 *
 * The pinned files under src/apu/dsp/ are vendored byte-for-byte from xemu at
 * 67cc79e663038d1f55448c0f566b37dde016adf6 and include QEMU headers that do not
 * exist in this toolkit. This directory supplies them.
 *
 * Every header here is a NO-OP or a PASS-THROUGH: it resolves a QEMU include
 * path onto the toolkit's existing QEMU shim (src/nv2a/qemu_shim.h, reached
 * through src/apu/apu_shim.h) and defines nothing of its own that could invent
 * device behaviour. The surface was inventoried rather than assumed -- see
 * src/apu/dsp/PROVENANCE.md, "QEMU shim surface".
 */

#ifndef XBOXRECOMP_SHIM_QEMU_OSDEP_H
#define XBOXRECOMP_SHIM_QEMU_OSDEP_H

#include "apu_shim.h"

#endif /* XBOXRECOMP_SHIM_QEMU_OSDEP_H */
