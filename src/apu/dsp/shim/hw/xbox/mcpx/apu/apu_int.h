/*
 * hw/xbox/mcpx/apu/apu_int.h shim for the pinned xemu DSP56300 sources
 * (A4b1 step 2).
 *
 * The pinned gp_ep.c includes this as its only header. Upstream it is the APU's
 * private header; here it resolves onto the toolkit's own APU state, so the
 * pinned GP/EP code compiles against the ONE layout rather than a copy of it
 * (step 2: "One layout").
 *
 * What the pinned gp_ep.c needs and where it comes from:
 *   MCPXAPUState, MCPXAPUGPState, MCPXAPUEPState   -- apu_state.h
 *   DSPState and the pinned DSP API                -- dsp/dsp.h (via apu_state.h)
 *   NV_PAPU_*, NUM_MIXBINS, GP_DSP_MIXBUF_BASE ... -- apu_regs.h (via apu_state.h)
 *   MCPX_APU_DEBUG_MON_*                           -- apu_debug.h (via apu_state.h)
 *   float_to_24b                                   -- fpconv.h
 *   DPRINTF / DEBUG_DSP                            -- dsp/debug.h
 *   MemoryRegion, address_space_memory, ldl_le_phys,
 *   memory_region_size, GET_MASK, assert,
 *   qemu_mutex_lock/unlock, g_new0, g_free         -- apu_shim.h (via apu_state.h)
 *
 * Nothing is added here that is not one of those includes.
 */

#ifndef XBOXRECOMP_SHIM_HW_XBOX_MCPX_APU_APU_INT_H
#define XBOXRECOMP_SHIM_HW_XBOX_MCPX_APU_APU_INT_H

#include "apu_state.h"
#include "fpconv.h"
#include "dsp/debug.h"

#endif /* XBOXRECOMP_SHIM_HW_XBOX_MCPX_APU_APU_INT_H */
