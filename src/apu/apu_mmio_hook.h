/*
 * MCPX APU MMIO hook - the VEH entry point for APU register access.
 *
 * apu_mmio_hook.c has defined apu_hook_handle_mmio since the APU emulation was
 * extracted, and no caller has ever existed in either repository, so the
 * function has never run once. Its own comment says what that costs: "The DSPs
 * are stubbed here, so a title that waits on one waits forever, and the only
 * way to work out what it is waiting for is to see the register traffic that
 * precedes the wait." That traffic is only visible when a caller exists, which
 * is what this declaration is for.
 *
 * Windows only. The hook is a Win32-VEH x86-64 instruction decoder and its
 * signature needs PCONTEXT, so the declaration sits behind the same guard as
 * the definition.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* For MCPXAPUState. */
#include "apu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The live APU state the hook routes into. Defined by apu_mmio_hook.c outside
 * its Win32 guard so a caller can assign it on either platform; the hook itself
 * declines every access while this is NULL. */
extern MCPXAPUState *g_apu_state;

#if defined(_WIN32)
#include <windows.h>

/* Handle a fault that landed in the APU's 512K window (0xFE800000..0xFE87FFFF).
 *
 * Only reached when the caller has unmapped that window so the accesses fault
 * (RECOMP_APU_TRAP); with the aperture left as plain memory no fault occurs and
 * there is nothing to route.
 *
 * ctx           - the faulting CONTEXT; advanced past the decoded instruction
 * fault_addr    - the host address that faulted
 * fault_xbox_va - the same address as an Xbox VA
 * is_write      - nonzero for ExceptionInformation[0] == 1
 *
 * Returns true when the instruction was decoded and handled, which is also the
 * condition for EXCEPTION_CONTINUE_EXECUTION. */
bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write);
#endif

#ifdef __cplusplus
}
#endif
