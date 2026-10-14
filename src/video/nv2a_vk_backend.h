/*
 * Vulkan render back end for the NV2A pushbuffer executor.
 *
 * The same contract as the Direct3D 11 back end (nv2a_d3d11_backend.h): the
 * executor's decoded batches (kernel/nv2a_backend.h) are rasterised on the GPU,
 * and every render target drawn since the last flip is written back into guest
 * memory at the flip, so everything that reads guest memory keeps working.
 *
 * Built only when CMake finds Vulkan and its shader tools (XBR_VULKAN_FOUND);
 * the shaders are SPIR-V compiled at build time, and the Vulkan loader is
 * opened at run time, so the executable does not depend on it. Elsewhere the
 * installer is a stub that reports failure.
 *
 * RECOMP_VK_DEVICE=<substring> picks the device whose name contains it (case
 * insensitive, CPU devices included); otherwise the first discrete GPU, then
 * integrated, virtual and other, never a CPU device.
 *
 * Vertex programs (V5) come from an ahead-of-time corpus (nv2a_vsh_aot.h): the
 * back end looks a batch's program up by hash, re-checks its identity against
 * the words the guest uploaded, and runs the pre-compiled shader. Nothing is
 * translated or compiled at run time; a program the corpus does not carry is
 * declined, and the executor's CPU interpreter draws the batch.
 */
#ifndef NV2A_VK_BACKEND_H
#define NV2A_VK_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "nv2a_vsh_aot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Open the loader, create the device and the fixed pipeline objects, and
 * register the back end with the executor. Call before the guest starts
 * submitting work; the executor itself must be enabled (RECOMP_PB_EXEC).
 * Returns 0 on success; on failure nothing is registered and the executor
 * keeps rasterising on the CPU. Idempotent. */
int nv2a_vk_backend_install(void);

/* Hand the back end the ahead-of-time vertex-program corpus. `entries` is
 * static storage owned by the caller and must stay valid for the life of the
 * process; `count` may be 0, which is the honest "no corpus built" state (every
 * program is then declined and the CPU interpreter draws it). Call before the
 * guest submits work, with or after nv2a_vk_backend_install(). Idempotent per
 * call; the last call wins. A no-op when the build has no Vulkan support.
 *
 * The corpus is never consulted to *compile* anything: an entry whose shader
 * the device cannot use is declined like a miss. */
void nv2a_vk_backend_set_programs(const Nv2aVshAotEntry *entries, size_t count);

/* What the vertex-program path did, for tests and reports. Cumulative since
 * start, plus write-once latches of the first hit and the first miss so the
 * deciding event is retained independently of any rate-limited report line.
 * Same threading contract as the rest of the back end: read it on the executor
 * thread, or from a single-threaded fixture with no concurrent submission. */
typedef struct {
    uint64_t hits;              /* batches drawn by a corpus shader */
    uint64_t misses;            /* identities the corpus does not carry */
    uint64_t identity_fail;     /* a hash matched but len/digest did not: declined, never drawn */
    uint64_t declines;          /* found, but this draw could not be set up on this device/state */
    uint64_t tris;              /* triangles the program path actually drew */
    uint64_t const_uploads;     /* 192-float4 constant blocks uploaded for those draws */
    uint32_t first_hit_hash, first_hit_len, first_hit_mask;
    uint64_t first_hit_digest;
    uint32_t first_miss_hash, first_miss_len, first_miss_mask;
    uint32_t first_hit_tris;
} Nv2aVkProgramStats;

void nv2a_vk_backend_program_stats(Nv2aVkProgramStats *out);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_VK_BACKEND_H */
