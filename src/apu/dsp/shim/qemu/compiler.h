/*
 * qemu/compiler.h shim for the pinned xemu DSP56300 sources (A4b1 step 2).
 *
 * The pinned dsp_dma.c includes this for MIN(); the toolkit's QEMU shim already
 * defines MIN/MAX/ARRAY_SIZE, so this is a pure pass-through.
 */

#ifndef XBOXRECOMP_SHIM_QEMU_COMPILER_H
#define XBOXRECOMP_SHIM_QEMU_COMPILER_H

#include "qemu/osdep.h"

#endif /* XBOXRECOMP_SHIM_QEMU_COMPILER_H */
