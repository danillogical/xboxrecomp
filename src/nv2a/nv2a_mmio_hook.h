/*
 * NV2A MMIO Hook - VEH instruction decoder for GPU register access
 *
 * When recompiled code accesses NV2A MMIO registers (0xFD000000+),
 * the access faults because no physical page is mapped. This module
 * decodes the faulting x86-64 instruction, extracts the read/write
 * operation, routes it through the NV2A register handlers, and
 * advances RIP past the instruction.
 *
 * This is the key bridge between recompiled Xbox D3D8 code and
 * the xemu NV2A GPU emulation.
 */

#ifndef BURNOUT3_NV2A_MMIO_HOOK_H
#define BURNOUT3_NV2A_MMIO_HOOK_H

#include "platform/xbox_winnt.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Initialize the NV2A GPU subsystem.
 * Call this during startup before the game code runs.
 * Allocates VRAM, RAMIN, and initializes register state.
 */
void nv2a_hook_init(ptrdiff_t xbox_mem_offset);

/* Install the model behind the already mapped guest aperture. Every guest
 * access faults on PAGE_NOACCESS and is serialized through the model owner. */
bool nv2a_hook_install_aperture(void *aperture, size_t size);

/* Disable interception and snapshot publication for an intentionally
 * unreadable aperture. This is one-way for the process lifetime. */
void nv2a_hook_disable_aperture(void);

/* Stop interception, wait for an in-flight handler, clear publication, and
 * restore the backing mapping before xbox_MemoryLayoutShutdown releases it. */
void nv2a_hook_shutdown(void);

/* Focused decoder/flags/bounds regression used by the integration probe. */
bool nv2a_hook_run_decoder_tests(void);

/* Deterministic clock injection and wakeup for runtime integration tests. */
bool nv2a_hook_set_ptimer_clock(uint64_t (*clock_ns)(void *), void *opaque);
void nv2a_hook_notify_ptimer_clock_changed(void);

typedef struct NV2AHookSubmissionSnapshot {
    uint32_t get;
    uint32_t put;
    uint32_t diagnostic_get;
    uint32_t diagnostic_subchannel;
    uint32_t diagnostic_method;
    uint32_t diagnostic_param;
    uint32_t sink_count;
    uint32_t successes;
    uint32_t last_method;
    uint32_t last_param;
    char diagnostic[32];
} NV2AHookSubmissionSnapshot;

/* Test/diagnostic seam. This reports parser state only; `successes` is the
 * number of atomically accepted streams and is not a GPU completion signal. */
bool nv2a_hook_get_submission_snapshot(NV2AHookSubmissionSnapshot *snapshot);

/* Serialized NV2A PCI owner used by both VEH and the kernel HAL bridge. */
bool nv2a_hook_pci_config_read(uint32_t offset, void *buffer, uint32_t length);
bool nv2a_hook_pci_config_write(uint32_t offset, const void *buffer,
                                uint32_t length);

/*
 * Handle an NV2A MMIO access fault.
 *
 * Called from the VEH handler when fault_xbox_va is in GPU register
 * space (0xFD000000-0xFDFFFFFF).
 *
 * Decodes the faulting instruction, performs the MMIO read/write
 * through the NV2A register handlers, updates CPU context, and
 * returns true if handled successfully.
 *
 * Returns false if the instruction or access is outside the supported
 * contract; the caller must leave the exception unhandled and diagnostic.
 */
bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write);

/*
 * Handle a GPU framebuffer/push buffer access.
 *
 * For 0xF0000000-0xFCFFFFFF range. Currently just allocates
 * pages backed by NV2A VRAM where appropriate.
 *
 * Returns true if handled.
 */
bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va);

#endif /* BURNOUT3_NV2A_MMIO_HOOK_H */
