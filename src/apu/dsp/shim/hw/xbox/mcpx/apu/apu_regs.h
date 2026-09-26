/*
 * hw/xbox/mcpx/apu/apu_regs.h shim for the pinned xemu DSP56300 sources
 * (A4b1 step 2).
 *
 * The pinned gp_ep.h includes this by its upstream path. This toolkit already
 * has the register file -- src/apu/apu_regs.h -- and the execution evidence
 * records that all 131 distinct DSP and NV_PAPU constants the pinned code uses
 * are already defined there. So this is a pass-through onto that file and
 * defines nothing: adding a second copy of the register file here is exactly
 * how the two would drift apart.
 */

#ifndef XBOXRECOMP_SHIM_HW_XBOX_MCPX_APU_APU_REGS_H
#define XBOXRECOMP_SHIM_HW_XBOX_MCPX_APU_APU_REGS_H

#include "apu_regs.h"

#endif /* XBOXRECOMP_SHIM_HW_XBOX_MCPX_APU_APU_REGS_H */
