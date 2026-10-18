/*
 * Vulkan render back end for the NV2A pushbuffer executor.
 *
 * The same contract as the Direct3D 11 back end (nv2a_d3d11_backend.h): the
 * executor's decoded batches (kernel/nv2a_backend.h) are rasterised on the GPU,
 * and every render target drawn since the last flip is written back into guest
 * memory at the flip, so everything that reads guest memory keeps working.
 *
 * Publication is explicit. A flush reports whether it left a target dirty; the
 * coherence boundary that asked for it (the flip, a target release, a texture that
 * reads guest memory a target overwrote) retries a failed publication EXACTLY ONCE,
 * and the dirty targets still unpublished after that are counted as
 * Nv2aVkReadbackStats.incomplete. A boundary that cannot publish at all stops the
 * back end: it publishes nothing more, presents nothing as a new frame, and leaves
 * guest memory holding the last bytes published for each target rectangle -- the
 * unpublished newer targets are never overwritten by older guest bytes, and no later
 * load, texture decode or CPU rasterisation may supersede them. A flush publishes
 * batch by batch, so an earlier batch may have published before a later one failed:
 * the stop preserves the targets that were never published, and it is not whole-frame
 * atomicity. A stopped back end reports that through the executor's optional
 * `stopped` entry (kernel/nv2a_backend.h), which is what makes the executor's CPU
 * paths stop writing guest framebuffer memory. The D3D11 back
 * end never stops.
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
 * RECOMP_VK_PIPELINE_CACHE=<file> persists the driver's VkPipelineCache in that file (unset:
 * %LOCALAPPDATA%\xboxrecomp\vk-pipeline-cache-<vendorID>-<deviceID>.bin on Windows, off
 * elsewhere; `off` or `0` disables it). A missing, damaged or foreign file starts the run cold.
 * It stores only the driver's own builds of the ahead-of-time shaders.
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
 * back end.
 *
 * The one counter that is NOT published work is `incomplete`: the dirty targets a coherence
 * boundary (the flip, a target release, a texture that reads guest memory a target
 * overwrote) had to leave unpublished after the flush and its one bounded retry both failed.
 * Counting one stops the back end: it publishes nothing more, and guest memory keeps the last
 * bytes published for each target rectangle. 0 in any run whose frames all published. */
typedef struct {
    uint64_t readbacks;      /* targets whose dirty rectangle was published to guest memory */
    uint64_t batches;        /* published batches: one submission and one fence wait each */
    uint64_t packed;         /* published 16-bit targets, packed by cs_pack on the GPU */
    uint64_t packed_pixels;  /* pixels those packed targets cover (dirty rect w*h, not padded) */
    uint64_t copied;         /* published 4-byte targets, read back straight from their image */
    uint64_t bytes;          /* bytes published into guest memory */
    uint64_t wait_ns;        /* the publication's own submit + fence wait (not a reuse wait) */
    uint64_t publish_ns;     /* host memcpy time inside publication */
    uint64_t incomplete;     /* dirty targets a boundary left unpublished; nonzero stops it */
} Nv2aVkReadbackStats;

void nv2a_vk_backend_readback_stats(Nv2aVkReadbackStats *out);

/* What device memory the back end holds and how it got it (plan N4.4). One vkAllocateMemory
 * makes a BLOCK and every pooled resource takes a RANGE out of it (VbAlloc: one memory handle,
 * a binding offset, a size), so `alloc_calls` is what the pooling actually saved: it is the
 * number of vkAllocateMemory calls made for a run that used to make one per resource. Pools are
 * keyed by (memoryTypeIndex, kind) -- an image is optimal tiling, a buffer is linear -- so an
 * image and a buffer never share a block, which is why bufferImageGranularity can never apply
 * between two neighbouring ranges.
 *
 * A DEDICATED allocation (pool -1, taken at offset 0) is the exception, and every reason it was
 * taken is counted separately: `dedicated_required` and `dedicated_prefer` (the object's
 * VkMemoryDedicatedRequirements, from the optional *_MemoryRequirements2 queries),
 * `dedicated_oversize` (a range over VB_POOL_MAX_BLOCK), `dedicated_noroom` (a pool at its
 * block or reserve budget, or every pool slot in use) and `dedicated_nohost` (a request whose
 * SELECTED memory type is host-visible without HOST_COHERENT). The last one is conservative on
 * purpose: a pooled host-visible block is mapped as a whole, so one range of it could not be
 * flushed or invalidated without touching the live ranges beside it, which rules pooling out
 * for such a type -- including for a request that never asked for host visibility, a
 * device-local image say, when that is the type its requirements select. `fallbacks` counts
 * the pool blocks that could not be made at all, each of which fell back once to a dedicated
 * allocation. `dedicated` counts all of them together.
 *
 * Lifetime: `released` counts the ranges given back -- an ADMITTED release only, the idle one or
 * the park copy; a refused one is counted by `parked`/`parked_lost` alone -- `parked` those a
 * recording or a pending submission was still able to read (every release that had to be parked
 * or refused, so a refused one counts here too), `parked_peak` the most of them actually held at
 * once and `parked_lost` the releases a FULL park list refused to admit (capacity failure,
 * terminal): a refused release is not abandoned and never freed -- the range stays counted live
 * in its block (`used`/`live`, or `dedicated_bytes` for a dedicated allocation) and no free range
 * ever receives it, so nothing is freed and nothing is handed out, and the back end stops
 * write-once. No VbAlloc keeps the range: every production caller reaches the refusal with a
 * by-value vb_garbage_free entry, and that entry is dropped rather than kept -- vb_stream's
 * dead-recording branch returns without its local entry, and the deferred-destroy route's drain
 * drops the entry it holds -- while the range stays pinned and accounted live. So `parked_lost`
 * is never a silently dropped range, and it is 0 in every sound run. `reuse_hits` are the ranges
 * taken from a block's free
 * list rather than its bump cursor; `blocks_retired` are the empty blocks given back to the
 * driver. `first_shared_block`/`first_shared_block_kind` and `first_reuse_off`/
 * `first_reuse_block` are write-once latches of the first block with two live ranges in it (the
 * pool's index, and 0 for a linear pool/1 for an optimal-tiling one) and the first reused range
 * (its offset in its block, and the block's index), so the deciding event survives even when
 * the counters that describe it are only rates; unset is ~0 for the block fields.
 *
 * The byte fields are live values, derived from the pools when this is called: what they
 * reserve (`reserved_bytes`), what is handed out (`used_bytes`, with `dedicated_bytes` of that
 * being whole dedicated allocations), what is free (`free_bytes`, `largest_free`), what
 * alignment wasted (`padding_bytes`) and the allocator's own bookkeeping (`metadata_bytes`,
 * honestly the arrays it really keeps, not an estimate). `peak_used` is the high-water mark of
 * `used_bytes`. Same threading contract as the rest of the back end: read it on the executor
 * thread, or from a single-threaded fixture with no concurrent submission. */
typedef struct {
    uint64_t pools;               /* pools in use */
    uint64_t blocks;              /* pool blocks created (one vkAllocateMemory each) */
    uint64_t blocks_retired;
    uint64_t ranges;              /* ranges handed out of blocks */
    uint64_t reuse_hits;          /* of those, taken from a free range, not the bump */
    uint64_t dedicated;           /* dedicated allocations, all reasons */
    uint64_t dedicated_required, dedicated_prefer, dedicated_oversize;
    uint64_t dedicated_noroom, dedicated_nohost;
    uint64_t alloc_calls;         /* vkAllocateMemory calls the allocator made */
    uint64_t released;            /* ranges given back */
    uint64_t parked, parked_peak, parked_lost;
    uint64_t reserved_bytes, used_bytes, free_bytes, largest_free, padding_bytes;
    uint64_t metadata_bytes;      /* the allocator's own bookkeeping, honestly counted */
    uint64_t dedicated_bytes, peak_used;
    uint64_t alloc_failures, bind_failures, map_failures, rollbacks, fallbacks;
    uint32_t first_shared_block, first_shared_block_kind;  /* write-once: >= 2 live ranges in one block */
    uint64_t first_reuse_off, first_reuse_block;           /* write-once: a range reused; ~0 = none */
} Nv2aVkMemoryStats;

void nv2a_vk_backend_memory_stats(Nv2aVkMemoryStats *out);

/* The pipeline table and the persisted pipeline cache (plan B2). `created` is the pipelines the
 * driver built, `evictions` the table's least-recently-used replacements. The cache_ fields
 * are the cache file's: the blob bytes that seeded the cache, the saves written and failed, the
 * files (or driver refusals) rejected, and whether persistence is on at all. Same threading
 * contract as the other stats. */
typedef struct {
    uint64_t created, evictions;
    uint64_t cache_loaded_bytes, cache_saves, cache_save_failures, cache_rejects;
    uint32_t cache_enabled;
} Nv2aVkPipelineStats;

void nv2a_vk_backend_pipeline_stats(Nv2aVkPipelineStats *out);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_VK_BACKEND_H */
