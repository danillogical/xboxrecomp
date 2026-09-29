/*
 * qemu/bswap.h shim for the pinned xemu DSP56300 sources (A4b1 step 2).
 *
 * The pinned interp/dsp_cpu.c includes this header but does not call anything
 * from it: the only byte-order helpers it actually uses are ldl_le_p/stl_le_p,
 * which the toolkit's QEMU shim provides (little-endian host). Measured, not
 * assumed: see src/apu/dsp/PROVENANCE.md, "QEMU shim surface",
 * which lists `bswap` as part of the surface; a grep of the vendored tree shows
 * the sole occurrence is the #include itself.
 *
 * So this header is a pass-through with nothing added. If a future re-pin makes
 * the pinned code call bswap32/bswap64, they must be added here rather than
 * silently left undefined.
 */

#ifndef XBOXRECOMP_SHIM_QEMU_BSWAP_H
#define XBOXRECOMP_SHIM_QEMU_BSWAP_H

#include "qemu/osdep.h"

#endif /* XBOXRECOMP_SHIM_QEMU_BSWAP_H */
