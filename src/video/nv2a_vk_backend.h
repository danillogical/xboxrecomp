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
 * The device it picks must have ONE queue family with both VK_QUEUE_GRAPHICS_BIT
 * and VK_QUEUE_COMPUTE_BIT: the frame's draws and the readback's pack kernel
 * (shaders/nv2a_vk_pack.hlsl, the compute pipeline that truncates a 16-bit
 * target to 5:6:5 on the GPU) are submitted to that one family, and a device
 * whose graphics family has no compute is not usable at all. There is exactly
 * one 16-bit publication path, so such a device is passed over and the
 * executor's CPU rasteriser remains the fallback.
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

/* What the bounded submission ring did. The ring is the back end's execution
 * model: each of `slots` slots owns one command recording, one fence, one
 * descriptor pool and one upload stream; a submission is issued WITHOUT waiting
 * for the ones before it; and the host reaches a fence only to reuse an occupied
 * slot or to publish guest memory (a flip's readback, a target flush/release).
 * These counters are the deciding instrumentation for the lifetime/overlap gate.
 * They are the host's own accounting: `in_flight`/`max_in_flight` count slots
 * submitted and not yet waited for (nothing polls a fence's state, so this is
 * "issued without waiting", not proof that the device was still executing);
 * `submissions` splits into `splits` (submitted at a draw entry because the
 * recording slot was worth sending) and `overflows` (submitted because the
 * stream or the descriptor pool would have overflowed); `reuse_waits` and
 * `drain_waits` count the fence waits that were performed, not the ones that
 * actually stalled the host. Same threading contract as the rest of the back
 * end. */
typedef struct {
    uint32_t slots;             /* the ring size */
    uint32_t in_flight;         /* slots submitted and not yet waited for */
    uint32_t max_in_flight;     /* the most of them at any one time */
    uint64_t submissions;       /* command buffers submitted */
    uint64_t reuse_waits;       /* fence waits performed before reusing a slot */
    uint64_t drain_waits;       /* fence waits performed while publishing guest memory */
    uint64_t splits;            /* submissions begun at a draw entry inside a frame */
    uint64_t overflows;         /* submissions begun because a slot's stream/sets would overflow */
    uint64_t frames;            /* flips served */
} Nv2aVkRingStats;

void nv2a_vk_backend_ring_stats(Nv2aVkRingStats *out);

/* What the readback publication (the flip's and a target flush/release's write of
 * guest memory) did. One dirty target at a time is either copied straight out of
 * its image (a 4-byte target) or packed to 5:6:5 by the compute kernel and then
 * copied out (a 16-bit target); a bounded group of them shares one submission and
 * one fence wait, and the host publishes the rows with a memcpy.
 *
 * Every counter below except the two time counters describes work that was PUBLISHED:
 * `readbacks` (targets whose dirty rectangle was written into guest memory), `batches`
 * (batches that reached that publish, one submission and one fence wait each -- a
 * `vb_reserve` slot advance in the middle of a batch is not a batch of its own), `packed`
 * and `packed_pixels` (16-bit targets the compute kernel produced, and the pixels they
 * cover: each packed dirty rectangle's width times its height, not the dispatch's padded
 * threads), `copied` (4-byte targets read back straight from their image) and `bytes` (what
 * went into guest memory). A batch that never reaches its publish leaves all of them
 * unchanged and leaves its targets dirty, so a partial batch can never be counted as
 * published work. `wait_ns` is the publication's own vkQueueSubmit plus fence wait -- a
 * reuse wait inside a `vb_reserve` slot advance is not included -- and `publish_ns` is the
 * host's memcpy time. Cumulative since start. Same threading contract as the rest of the
 * back end. */
typedef struct {
    uint64_t readbacks;      /* targets whose dirty rectangle was published to guest memory */
    uint64_t batches;        /* published batches: one submission and one fence wait each */
    uint64_t packed;         /* published 16-bit targets, packed by cs_pack on the GPU */
    uint64_t packed_pixels;  /* pixels those packed targets cover (dirty rect w*h, not padded) */
    uint64_t copied;         /* published 4-byte targets, read back straight from their image */
    uint64_t bytes;          /* bytes published into guest memory */
    uint64_t wait_ns;        /* the publication's own submit + fence wait (not a reuse wait) */
    uint64_t publish_ns;     /* host memcpy time inside publication */
} Nv2aVkReadbackStats;

void nv2a_vk_backend_readback_stats(Nv2aVkReadbackStats *out);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_VK_BACKEND_H */
