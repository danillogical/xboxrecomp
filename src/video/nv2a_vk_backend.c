/*
 * Vulkan render back end for the NV2A pushbuffer executor.
 *
 * A port of the Direct3D 11 back end (nv2a_d3d11_backend.c), which stays the
 * reference: the same decoded batches (kernel/nv2a_backend.h) are rasterised
 * with Vulkan, and at every flip each render target drawn since the last one
 * is written back into guest memory in the guest's own format, so the
 * framebuffer window, the present tracker and frame dumps keep reading guest
 * memory exactly as they do on the CPU path. Functions are named after their
 * D3D11 counterparts (gb_x there is vb_x here) and keep their semantics:
 *
 *   - Render targets are cached per guest colour surface (address, pitch, pixel
 *     size) as B8G8R8A8 images, whose bytes are the guest's A8R8G8B8 layout; a
 *     16-bit surface is expanded on load and truncated to 5:6:5 on flush by the
 *     cs_pack compute kernel (shaders/nv2a_vk_pack.hlsl), so the flush's host
 *     side is a memcpy per row and no per-pixel conversion runs on the CPU. A
 *     target is (re)loaded from guest memory on its first use after each flip.
 *   - Depth is a D24S8 image (D32S8 where D24S8 is unsupported) per (zeta
 *     address, target size), never loaded from or written to guest memory; Z16
 *     titles are handled by the z scale, as in D3D11.
 *   - Textures are decoded with the executor's own decoder (DXT goes up as BC
 *     where the device samples it), cached, and re-verified by a hash of their
 *     guest bytes on their first use after each flip. A texture that is a live
 *     render target is sampled from the target (through a snapshot when it is
 *     also the output); any other texture overlapping a target with unflushed
 *     GPU writes flushes that target first.
 *   - Two pipelines: stage 0 modulated by diffuse (shaders/nv2a_vk_stage0.hlsl), and the
 *     register combiners (shaders/nv2a_vk_rc.hlsl): one uber shader that interprets the
 *     batch's Nv2aCombiner, uploaded per draw to a stream-backed dynamic uniform buffer,
 *     as nv2a_rc_eval does. A batch whose texture stages it cannot interpret (gb_rc_key's
 *     rule: modes > 5, true cube maps) is drawn the stage-0 way and counted as
 *     rc_fallback.
 *   - Vertex programs (draw_program, plan V5): a program batch is looked up in an
 *     ahead-of-time corpus (nv2a_vsh_aot.h) by hash, its identity re-checked against
 *     the words the guest uploaded, and its pre-compiled shader run as a third
 *     pipeline kind paired with ps_rc. Nothing is translated or compiled at run
 *     time: a program the corpus does not carry, or whose identity does not match,
 *     is declined and the executor's CPU interpreter draws the batch.
 *
 * Vulkan specifics:
 *   - y: the vertex shader is D3D11's, built with -fvk-invert-y, so surface
 *     pixel (x, y) lands on framebuffer pixel (x, y) with a positive-height
 *     viewport; winding is judged in framebuffer space in both APIs, so NV2A
 *     CW (0x900) is VK_FRONT_FACE_CLOCKWISE as it is FrontCounterClockwise=FALSE.
 *   - Pipelines are cached by the state that is baked into them (blend, write
 *     mask, depth, stencil func/ops, cull, winding, depth attachment present);
 *     viewport, scissor, stencil reference/masks and blend constants are dynamic.
 *   - Targets, depth images and the self-sample snapshot stay in GENERAL layout for
 *     their whole life: a target is an attachment, a sampled image and a copy source
 *     within one submission, a depth image is an attachment and a transfer
 *     destination, and the snapshot is a copy destination and a sampled image, so a
 *     per-access layout would need a transition per draw. Textures go up through
 *     TRANSFER_DST_OPTIMAL into SHADER_READ_ONLY_OPTIMAL. Each normal-path barrier
 *     names the stage and access scopes of the producer and of the consumers that
 *     resource can have, deliberately a superset where a consumer class cannot arise
 *     on this path today. Only two sites keep every stage and access on both sides,
 *     both vb_barrier(): the per-submission barrier in vb_slot_submit, the only thing
 *     that makes cross-submission ordering defined, and the abandonment barrier in
 *     vb_targets_flush, which must order an abandoned group's writes and reads
 *     against a whole next group.
 *   - Synchronisation is a bounded ring of VB_SLOTS submission slots. A slot owns
 *     one command recording, one fence, one descriptor pool, one upload stream and
 *     the objects a pending submission may still read; only the slot being recorded
 *     is ever written to, so a submission's stream and descriptor sets cannot be
 *     overwritten while the GPU runs it. A submission is submitted WITHOUT waiting,
 *     so the device runs one slot while the host records the next, and the ring
 *     bounds how far the host can get ahead. The host reaches a fence in only three
 *     places: to reuse a slot that is still in flight, to publish guest memory (the
 *     flip's readback, and a target flush/release), and on the device's error path.
 *     A lost device stops the back end (g.ok) instead of freeing or reusing anything
 *     a pending submission may read. Every submission ends with the same global
 *     memory barrier, recorded OUTSIDE any render pass, whose second scope is every
 *     command later in submission order -- that is what makes cross-submission
 *     ordering defined (Vulkan defines no implicit execution or memory dependency
 *     between submissions), including blending into one target from two submissions.
 *   - Pipeline cache (plan B2): the VkPipelineCache is persisted across runs, so the driver's
 *     own builds of the ahead-of-time SPIR-V that already happen are not repeated on a warm
 *     start. Nothing new is translated or compiled: the cache only stores what the driver
 *     made from the same shaders. RECOMP_VK_PIPELINE_CACHE names the file (unset: one file per
 *     device, vk-pipeline-cache-<vendorID>-<deviceID>.bin in the xboxrecomp folder under
 *     %LOCALAPPDATA% on Windows, disabled elsewhere; `off` or `0` disables it). The file is a 20-byte header (magic, version, blob length, CRC-32) and the
 *     raw vkGetPipelineCacheData blob; a file that is missing, damaged or from another device
 *     or driver is rejected and the run starts cold. There is no shutdown hook, so vb_flip saves
 *     the cache during the run, at most every VB_PCACHE_SAVE_MS and only after new pipelines, through
 *     a per-process temporary file, and never an image its own loader would reject.
 *
 *   - Device memory is SUB-ALLOCATED (plan N4.4). One vkAllocateMemory makes a BLOCK and
 *     every pooled resource takes a RANGE out of it, bound at the range's own offset
 *     (VbAlloc carries mem/off/size; a dedicated allocation owns its whole allocation and
 *     has pool -1, block -1, off 0). Pools are keyed by (memoryTypeIndex, nonlinear), so
 *     an image and a buffer never share a block -- which is why
 *     VkPhysicalDeviceLimits::bufferImageGranularity can never apply between two
 *     neighbouring ranges here: every neighbour of an image range is another image range
 *     and every neighbour of a buffer range is another buffer range. Inside a block a
 *     bounded bump cursor hands out fresh ranges and a bounded, coalescing free list hands
 *     back returned ones; a range's offset is aligned to the requirements' alignment and
 *     at least VB_POOL_ALIGN, two live ranges never overlap, and a block is never asked
 *     for more than VB_POOL_MAX_BLOCK or a pool for more than VB_POOL_RESERVE.
 *
 *     DEDICATED allocations are the exception, each reason counted separately in
 *     Nv2aVkMemoryStats (nv2a_vk_backend_memory_stats; the periodic [GPUVK] line carries
 *     the same counters): an object that requires or prefers a dedicated allocation
 *     (VkMemoryDedicatedRequirements, through the OPTIONAL
 *     vkGetImageMemoryRequirements2/vkGetBufferMemoryRequirements2 lookups -- an absent
 *     entrypoint means "not available", and the request is pooled), a range over
 *     VB_POOL_MAX_BLOCK, a pool at its block or reserve budget, no free pool slot, and a
 *     request whose selected memory type is host-visible WITHOUT coherence (the rule is on
 *     the selected type, so it can also catch a request that never asked for host visibility,
 *     a device-local image say: conservative on purpose, because a pooled host-visible block
 *     is mapped as a whole and one range of it could not be flushed or invalidated without
 *     touching the live ranges beside it). A pool block that cannot be made falls back
 *     ONCE to a dedicated allocation, and fails if that fails too.
 *
 *     COHERENCE and lifetime: pooled host-visible memory is coherent, and a flush of it
 *     would be a no-op, so vb_mem_flush skips one. The coherence calls the back end really
 *     makes are: one flush of the recording slot's stream before the submission that reads
 *     it (a no-op while that range is coherent, and the boundary when it is not), and one
 *     invalidate of the readback staging buffer's range before each published batch reads it
 *     -- issued for coherent memory too, which is what the publication always did and is
 *     legal, the atom rule below binding only a type that is not host-coherent. Both helpers
 *     clip to the range they are given -- never a neighbouring live range of the same block --
 *     and widen to whole VkPhysicalDeviceLimits::nonCoherentAtomSize atoms when the type is
 *     not coherent. A range whose release happens while something could still read it is
 *     PARKED: while g.slots[g.cur].recording || g.npending, vb_mem_give puts the range on
 *     the bounded s_pool_pending[] list (counted `parked`) and vb_pool_gc folds it back
 *     only after the ring has drained -- which is also why a parked range keeps its block's
 *     `used` bytes and a block holding one can never be retired. Only two kinds of release skip
 *     that path, because nothing recorded or in flight can name the range: a failed bind or map
 *     in this call's own resource creation -- the failed bind never bound the object, and a
 *     failed map's object is destroyed first, so the range names nothing either way -- and the
 *     staging pair a readback reserve made in its own call and never published. Each gives the
 *     range back at once (vb_mem_give_now), so a burst of such rollbacks cannot fill the bounded
 *     list. A release a recorded or in-flight command may still read stays conservative on
 *     purpose even when it looks new: a failed vkCreateImageView on an image that IS bound
 *     (vb_image_create discards it through vb_image_discard) and vb_stream's dead-recording
 *     branch (which frees its slice through vb_garbage_free) both take the queued/parked path.
 *     A release the FULL list cannot admit is REFUSED, not dropped (`parked`, `parked_lost`):
 *     the range keeps being counted live in its block and reaches no free list -- nothing is
 *     freed and nothing is handed out, and the VbAlloc copy naming it is dropped, not kept by a
 *     caller -- and the back end stops write-once, because a release the allocator cannot take
 *     is a state it cannot leave on its own. A pool keeps at
 *     most one empty block, and only one no larger than VB_POOL_KEEP_EMPTY (the size class
 *     the sizing rule makes for small and mid ranges); every other empty block is retired
 *     (vkFreeMemory) by vb_pool_gc, which vb_garbage_drain calls after every fence wait, and
 *     by a pool that needs a block while it is at VB_POOL_BLOCKS. A failed allocate, bind or
 *     map releases exactly what it took (counted `rollbacks`) through vb_mem_give_now, since
 *     nothing recorded or in flight can name an object this call has just made, and leaves
 *     the caller's failure path to destroy the object -- the object is destroyed FIRST when
 *     it was bound, because a bound range may not be handed out while the object still names
 *     it: no dangling VbAlloc, no double release, no early reuse, and no resource is usable
 *     after a failure.
 *
 *   - The readback's publication is one bounded batch per submission: every dirty
 *     target's slice lands in a staging buffer at a 4-byte-aligned offset, the
 *     4-byte ones copied straight from their image and the 16-bit ones packed by
 *     cs_pack into a device-local buffer first, then one copy engine command moves
 *     each packed slice to the host-visible staging buffer. The queue family must
 *     therefore have compute as well as graphics, so the pack kernel can be
 *     dispatched on the same queue as the draws.
 *
 *   - Publication is explicit at every coherence boundary. vb_targets_flush reports
 *     whether it left a target dirty; the boundary (the flip, a target release, a
 *     texture that reads guest memory a target overwrote) retries it EXACTLY ONCE
 *     through vb_targets_flush_recover, which counts whatever is still dirty as
 *     `readback_incomplete`. A boundary that still cannot publish stops the back end
 *     (vb_stop): g.ok and g.failed latch, the stop is announced once, nothing is freed
 *     or submitted, no target is marked stale and the frame counter does not advance.
 *     From then on the back end publishes nothing and the executor's CPU path stops
 *     consuming guest framebuffer memory through the backend's `stopped` entry
 *     (nv2a_backend.h), so guest memory KEEPS THE LAST BYTES PUBLISHED FOR EACH TARGET
 *     RECTANGLE -- no older guest bytes are ever uploaded over a target whose newer pixels
 *     were not published, and no CPU rasterisation or present is allowed to supersede them.
 *     A flush publishes batch by batch, so an earlier batch may have published before a
 *     later one failed: the stop preserves the targets never published, and it is not
 *     whole-frame atomicity.
 *     This is the whole N4.2 coherence rule; the D3D11 back end never stops.
 *
 * Everything runs on the executor's thread, one call at a time.
 */
#include "nv2a_vk_backend.h"

#if !defined(XBR_VK_BACKEND)

#include <string.h>

/* Built without Vulkan: report failure so the caller keeps the CPU path. */
int nv2a_vk_backend_install(void)
{
    return -1;
}

/* No Vulkan, so no program path: the corpus is not consulted and every program
 * batch falls back to the executor's CPU interpreter. */
void nv2a_vk_backend_set_programs(const Nv2aVshAotEntry *entries, size_t count)
{
    (void)entries;
    (void)count;
}

void nv2a_vk_backend_program_stats(Nv2aVkProgramStats *out)
{
    if (out)
        memset(out, 0, sizeof *out);
}

void nv2a_vk_backend_pipeline_stats(Nv2aVkPipelineStats *out)
{
    if (out)
        memset(out, 0, sizeof *out);
}

void nv2a_vk_backend_ring_stats(Nv2aVkRingStats *out)
{
    if (out)
        memset(out, 0, sizeof *out);
}

/* No Vulkan, so nothing is ever published here: the executor's CPU path keeps
 * writing guest memory itself. */
void nv2a_vk_backend_readback_stats(Nv2aVkReadbackStats *out)
{
    if (out)
        memset(out, 0, sizeof *out);
}

/* No Vulkan, so no device memory was ever allocated: every counter is 0. */
void nv2a_vk_backend_memory_stats(Nv2aVkMemoryStats *out)
{
    if (out)
        memset(out, 0, sizeof *out);
}

#else

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#endif
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/nv2a_backend.h"
#include "kernel/nv2a_clear.h"
#include "d3d/d3d8_swizzle.h"
#include "nv2a_vk_rc_coverage.h"        /* the rc refusal rule and its counters */
#include "nv2a_vsh_hlsl.h"              /* Nv2aVshHlslXf, the corpus shaders' second cbuffer */

/* Built from shaders/nv2a_vk_stage0.hlsl, shaders/nv2a_vk_rc.hlsl and
 * shaders/nv2a_vk_pack.hlsl by xbr_add_spirv. */
#include "xbr_nv2a_vk_stage0_vs.spv.h"
#include "xbr_nv2a_vk_stage0_ps.spv.h"
#include "xbr_nv2a_vk_rc_vs.spv.h"
#include "xbr_nv2a_vk_rc_ps.spv.h"
#include "xbr_nv2a_vk_pack_cs.spv.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define VB_MAX_TARGETS   16
#define VB_MAX_DEPTHS    8
#define VB_MAX_TEXTURES  512
#define VB_MAX_PIPES     256
#define VB_MAX_FBS       64
#define VB_MAX_DIM       4096
#define VB_SLOTS         3                /* submission slots the ring can have in flight at once */
#define VB_SLOT_STREAM   (16u << 20)      /* vertices and uploads of one slot's submission */
#define VB_STREAM_ALIGN 256u              /* stream slices; >= every device's minUniformBufferOffsetAlignment */
#define VB_SLOT_SETS     2048             /* descriptor sets of one slot's submission */
/* The hard cap on the readback publication's staging buffers, which are grow-only
 * (see vb_targets_flush): a group of dirty targets whose slices would not fit is
 * published as several batches instead, and a single target bigger than this stays
 * dirty and is retried. Every target fits: the largest possible one is a 4096x4096
 * 4-byte target, 64 MiB. */
#define VB_READBACK_MAX  (96u << 20)
/* Submit at a draw entry once the recording slot holds this much, when another slot is
 * free: that is what lets the device start on the earlier part of a frame while the host
 * records its later part. A real Garage frame records about 60 draws and 5 MiB of stream,
 * so these thresholds give it two or three submissions -- enough for the device to be
 * working while the host records the rest, without turning a frame into many small
 * submissions. Below them a frame stays one submission, exactly as before. */
#define VB_SPLIT_BYTES   (VB_SLOT_STREAM / 8)
#define VB_SPLIT_SETS    (VB_SLOT_SETS / 64)
/* The most one program batch's stream slices may sum to before the batch is declined to the CPU
 * interpreter -- unchanged from when one 32 MiB stream held a whole submission. A batch this
 * size may span more than one slot: each slice goes to the recording slot's stream, or -- when
 * it is over half that stream or no longer fits what is left of it -- to a buffer of its own
 * that carries every usage a stream slice needs (vertices, indices, transfer source, uniform
 * block). Either way a slice is read only by commands recorded after it, in that same slot, so
 * nothing can land between a slice being written and the draw that reads it.
 */
#define VB_PROG_RESERVE_MAX (32u << 20)
/* The per-draw vertex cap, unchanged from when one 32 MiB stream held a whole submission:
 * a draw that would exceed it is truncated to whole triangles exactly as it was before. */
#define VB_DRAW_CAP_BYTES ((32u << 20) / 2)
#define VB_STATS_MS      10000
#define VB_PCACHE_SAVE_MS 10000           /* least time between two pipeline cache saves */
#define VB_PCACHE_MAX    (64u << 20)      /* the largest blob a cache file may carry */
#define VB_PCACHE_HDR    20u              /* magic 8, version 4, blob length 4, CRC-32 4 */
#define VB_PCACHE_VER    1u
#define VB_MAX_MISS_LOG  256              /* distinct missing program hashes logged */
#define VB_WAIT_NS       (10ull * 1000000000ull)
#define VB_VALIDATION_SAY 64              /* validation messages printed; all are counted */

/* The vertex input below reads Nv2aVertex in place. */
typedef char vb_vertex_is_28_bytes[sizeof(Nv2aVertex) == 28 ? 1 : -1];

/* ── Loader ──────────────────────────────────────────────────────────── */

#define VB_GLOBAL_FNS(X) \
    X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)

#define VB_INSTANCE_FNS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr)

#define VB_DEVICE_FNS(X) \
    X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkQueueSubmit) \
    X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) \
    X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineCache) \
    X(vkCreatePipelineLayout) \
    X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateComputePipelines) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdCopyBuffer) X(vkCmdCopyImage) X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) \
    X(vkCmdPushConstants) X(vkCmdDispatch) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetStencilReference) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetBlendConstants) \
    X(vkCmdDraw) X(vkCmdDrawIndexed)

#define VB_DECLARE(fn) static PFN_##fn fn;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
VB_GLOBAL_FNS(VB_DECLARE)
VB_INSTANCE_FNS(VB_DECLARE)
VB_DEVICE_FNS(VB_DECLARE)
static PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT;

/* OPTIONAL device entrypoints (plan N4.4). Their absence is never a reason to fail the
 * install; it only changes what the memory allocator may use.
 *   - vkGetImageMemoryRequirements2 / vkGetBufferMemoryRequirements2 report
 *     VkMemoryDedicatedRequirements. NULL means "not available": the resource's plain
 *     requirements are used and the request is pooled, counted once like any other pool
 *     range (there is no separate counter for it).
 *   - vkFlushMappedMemoryRanges / vkInvalidateMappedMemoryRanges are the coherence
 *     boundary of a range the device and the host both touch. NULL makes that boundary
 *     fail instead of lying about a non-coherent range.
 *   - vkUnmapMemory drops a retired host-visible block's mapping. NULL leaves the block
 *     mapped; nothing is unmapped while a range of it may still live. */
static PFN_vkGetImageMemoryRequirements2 vkGetImageMemoryRequirements2;
static PFN_vkGetBufferMemoryRequirements2 vkGetBufferMemoryRequirements2;
static PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges;
static PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges;
static PFN_vkUnmapMemory vkUnmapMemory;
/* OPTIONAL (plan B2): NULL means the pipeline cache cannot be saved, nothing else. */
static PFN_vkGetPipelineCacheData vkGetPipelineCacheData;

/* ── State ───────────────────────────────────────────────────────────── */

/* The sub-allocator's bounds (plan N4.4). The pools live in `g` (g.pools) and their
 * implementation is the memory section below; these are here because VbPool is sized by
 * them and the resource structs below hold a VbAlloc. */
#define VB_POOLS          6        /* distinct (memoryTypeIndex, kind) pools one device may use */
#define VB_POOL_BLOCKS    6        /* blocks one pool may hold */
#define VB_POOL_FREE_MAX  32       /* free ranges one block tracks */
#define VB_POOL_MIN_BLOCK (1u << 20)
#define VB_POOL_MAX_BLOCK (64u << 20)   /* a single pooled range may not exceed this */
#define VB_POOL_SHARE_MAX (8u << 20)    /* a range with a block up to this size gets four times it */
#define VB_POOL_RESERVE   (256u << 20)  /* bytes one pool may reserve in total */
#define VB_POOL_PENDING   64       /* range releases parked until the ring is idle */
#define VB_POOL_ALIGN     256u     /* every pooled range's offset is a multiple of this */
#define VB_POOL_KEEP_EMPTY (4u << 20)   /* at most one empty block per pool, and only one no
                                         * larger than this -- the size class the sizing rule
                                         * makes for small and mid ranges, so a pool that churns
                                         * keeps one block to reuse instead of freeing it */

typedef struct { VkDeviceSize off, size; } VbFreeRange;

typedef struct {
    VkDeviceMemory mem;
    VkDeviceSize size;                 /* the block's whole size */
    VkDeviceSize bump;                 /* high-water mark of the bump allocator */
    VkDeviceSize used;                 /* live bytes handed out (not returned) */
    VkDeviceSize padding;              /* bytes this block spent aligning ranges */
    uint8_t *map;                      /* host-visible blocks: the whole block, mapped once */
    VbFreeRange free_list[VB_POOL_FREE_MAX];
    unsigned nfree;
} VbBlock;

typedef struct {
    int used;
    uint32_t type_index;
    int nonlinear;                     /* 1: images (optimal tiling); 0: linear buffers */
    VbBlock blocks[VB_POOL_BLOCKS];
    unsigned nblocks;
    /* `used` above is the pool's in-use flag (a C struct cannot have both); these are the
     * pool's byte counters: bytes its blocks reserve, live bytes handed out of them and the
     * padding they spent aligning ranges. */
    uint64_t reserved, live, padding;
    uint64_t ranges, reuse_hits;
} VbPool;

/* The memory one resource owns. A pooled allocation names the block and the range inside it;
 * a dedicated allocation owns the whole allocation and has pool == -1, block == -1, off == 0.
 * `mem` (not `off`) is what tells an empty one: a zeroed VbAlloc has pool and block 0, and
 * a pooled range's offset 0 is perfectly ordinary. */
typedef struct {
    VkDeviceMemory mem;   /* the object to bind: the block's memory, or the dedicated one */
    VkDeviceSize off;     /* the binding offset */
    VkDeviceSize size;    /* the range's size, >= the VkMemoryRequirements size */
    int pool;             /* index into g.pools, or -1 */
    int block;            /* index into that pool's blocks, or -1 */
    int coherent;         /* the selected memory type has HOST_COHERENT */
    int host_visible;     /* the selected memory type has HOST_VISIBLE */
} VbAlloc;

/* Push constants, shared by both stages; the D3D11 back end's GbConstants. */
typedef struct { float xf[4], tx[4]; } VbConstants;

/* What the combiner pipeline reads per vertex: Nv2aVertex and Nv2aVertexExtra merged
 * (the D3D11 back end's GbRcVertex). */
typedef struct {
    float x, y, z, rhw;
    uint32_t d0, d1;                    /* diffuse, specular: 0xAARRGGBB */
    float fog;                          /* Nv2aVertexExtra.fog */
    float t[4][4];                      /* Nv2aVertexExtra.tex */
} VbRcVertex;
typedef char vb_rc_vertex_is_92_bytes[sizeof(VbRcVertex) == 92 ? 1 : -1];

/* Push constants of the pack kernel (shaders/nv2a_vk_pack.hlsl's PackPush): the dirty
 * rectangle in surface pixels, then the slice's byte offset in the staging buffer and its
 * row stride in bytes. Both members are uint4, so C and the shader cannot disagree on
 * offsets and the block is exactly the 32 bytes layout_pack declares. */
typedef struct { uint32_t rect[4], dst[4]; } VbPackPush;
typedef char vb_pack_push_is_32_bytes[sizeof(VbPackPush) == 32 ? 1 : -1];

/* The combiner registers of one draw, binding 0 of dsl_rc (RcConsts in nv2a_vk_rc.hlsl). Every
 * member is a whole number of 16-byte vectors, so C and the shader cannot disagree on offsets. */
typedef struct {
    uint32_t stage[8][4];               /*   0: color_icw, alpha_icw, color_ocw, alpha_ocw */
    float    c0[8][4];                  /* 128: FACTOR0[s] r,g,b,a (nv2a_rc_unpack) */
    float    c1[8][4];                  /* 256: FACTOR1[s] */
    float    fc0[4];                    /* 384: final_c0 */
    float    fc1[4];                    /* 400: final_c1 */
    float    fogc[4];                   /* 416: fog colour (Nv2aBatch.fog_color unpacked) */
    float    ts[4][4];                  /* 432: per texture stage: scale x,y; z 1 if bound; w 0 */
    uint32_t ctl[4];                    /* 496: stages, flags, stage_program, clip_plane_mode */
    uint32_t fin[4];                    /* 512: final0, final1, 0, 0 */
} VbRcConstants;                        /* 528 */
typedef char vb_rc_constants_528[sizeof(VbRcConstants) == 528 ? 1 : -1];
/* The corpus shaders' second cbuffer (binding 10); its HLSL is float4 g_xf, float4 g_fog. */
typedef char vb_vsh_xf_is_32_bytes[sizeof(Nv2aVshHlslXf) == 32 ? 1 : -1];
typedef char vb_rc_ts_at_432[offsetof(VbRcConstants, ts) == 432 ? 1 : -1];
typedef char vb_rc_ctl_at_496[offsetof(VbRcConstants, ctl) == 496 ? 1 : -1];
typedef char vb_rc_fin_at_512[offsetof(VbRcConstants, fin) == 512 ? 1 : -1];

typedef struct {
    VkImage img;
    VbAlloc mem;
    VkImageView view;
} VbImage;

typedef struct {
    int used;
    uint32_t va, pitch, bpp, w, h;
    VbImage im, copy;                   /* the target, and its snapshot for self-sampling */
    int stale;                          /* must be reloaded from guest memory */
    int dirty;                          /* GPU writes not yet in guest memory */
    uint32_t dx0, dy0, dx1, dy1;        /* the rectangle they cover */
    uint64_t last_use;
} VbTarget;

typedef struct {
    int used;
    uint32_t va, w, h;
    VbImage im;
    uint64_t last_use;
} VbDepth;

typedef struct {
    int used;
    uint32_t offset, color, w, h, pitch, span;
    uint64_t hash;
    uint32_t frame;                     /* flip count when the hash was last checked */
    uint64_t last_use;
    VbImage im;                         /* view 0: the format does not decode */
} VbTex;

/* Everything baked into a pipeline; all uint32_t, so memcmp compares it and a
 * zero-filled struct is the stage-0 key. */
typedef struct {
    uint32_t rc;                                         /* 1: the combiner pipeline (nv2a_vk_rc.hlsl, VbRcVertex, layout_rc) */
    uint32_t prog;                                       /* 1: the vertex-program pipeline (corpus shader, layout_prog) */
    uint32_t prog_hash;                                  /* prog only: which corpus shader is baked in */
    uint32_t input_mask;                                 /* prog only: decides the baked vertex input layout */
    uint32_t has_depth;
    uint32_t blend, src, dst, eq, wmask;                 /* blend: src/dst/eq 0 when off */
    uint32_t z_enable, z_func, z_write;
    uint32_t s_enable, s_func, s_fail, s_zfail, s_zpass;
    uint32_t cull, ccw;                                  /* cull 0 none 1 back 2 front */
} VbPipeKey;
/* memcmp compares keys, so there must be no padding and no non-integer member. */
typedef char vb_pipe_key_is_20_words[sizeof(VbPipeKey) == 20 * sizeof(uint32_t) ? 1 : -1];

typedef struct {
    VbPipeKey k;
    VkPipeline pipe;
    uint64_t last_use;
} VbPipe;

typedef struct {
    VkImageView c, d;                   /* d 0: colour only */
    VkFramebuffer fb;
} VbFb;

/* Objects that recorded commands may still use, destroyed after the next wait. */
typedef struct {
    VkImage img;
    VkImageView view;
    VbAlloc mem;
    VkBuffer buf;
    VkFramebuffer fb;
    VkPipeline pipe;
} VbGarbage;

/* One slot of the bounded submission ring.
 *
 * Ownership and reuse invariants:
 *   - only g.slots[g.cur] is recorded into, and only while `recording`;
 *   - `pending` means the slot has been submitted and its fence has not been observed
 *     yet: its command buffer, descriptor pool, stream and fence are then untouched --
 *     no vkResetCommandBuffer, no vkResetDescriptorPool, no stream write, no re-record;
 *   - the slot's stream and pool are reset only in vb_slot_wait, i.e. after the fence;
 *   - the fence is created unsignaled (vkQueueSubmit requires that) and is reset right after
 *     it signals, so a slot that is recorded into has an unsignaled fence: `pending == 0` is
 *     the whole definition of a free slot, and nothing ever queries a fence's state directly;
 *   - the runtime never destroys the ring: the device lives as long as the process. */
typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    int pending;
    int recording;
    VkDescriptorPool dpool;
    uint32_t sets_used;
    VkBuffer stream;
    VbAlloc stream_mem;
    uint8_t *stream_ptr;
    VkDeviceSize stream_off;
    VkPipeline bound_pipe;              /* the pipeline bound in this slot's recording */
} VbSlot;

typedef struct {
    uint64_t draws, tris, clears, dropped_tris, skipped, cull_draws, cull_all;
    uint64_t tex_uploads, tex_hits, tex_undecodable, rt_aliases, rt_copies;
    uint64_t rt_loads, readbacks, readback_ns, rc_draws, rc_fallback, pipelines, submits;
    uint64_t validation_errors, validation_warnings;
    /* How the readback publication did its work (vb_targets_flush). `readback_batches` is the
     * batches that reached their publish, and `readback_packs`/`readback_pixels` (16-bit
     * targets packed by the compute kernel, and the pixels they cover) and
     * `readback_copies` (4-byte targets read back straight from their image) are counted per
     * target as it is published, so a batch that never got that far leaves them all
     * unchanged and leaves its targets dirty. `readback_bytes` is what went into guest
     * memory, and where the publication's time went is the submit and fence wait
     * (`readback_wait_ns`) and the host memcpy (`readback_publish_ns`). */
    uint64_t readback_batches, readback_packs, readback_pixels, readback_copies;
    uint64_t readback_bytes, readback_wait_ns, readback_publish_ns;
    /* Dirty targets a coherence boundary (vb_targets_flush_recover) had to leave unpublished
     * after the flush and its one bounded retry both failed. Counted only there, never by
     * vb_targets_flush itself: the direct-call gate compares the publication counters across a
     * failed call. Nonzero means a boundary stopped the back end, so no later load or texture
     * decode can overwrite the unpublished targets with older guest bytes. */
    uint64_t readback_incomplete;
    /* The submission ring (see the file header): submissions issued without waiting for the
     * ones before them, and the fence waits the ring did perform. Deciding instrumentation for
     * the lifetime/overlap gate, so these are counters, not log lines. `overflows` is the part
     * of `submits` that came from `vb_reserve` running out of stream or descriptor sets;
     * `splits` is the part that came from a draw entry deciding the slot was worth submitting. */
    uint64_t reuse_waits, drain_waits, splits, overflows;
    /* The vertex-program path (nv2a_vsh_aot.h); the program stats' counters, so the
     * same numbers reach the periodic [GPUVK] line. */
    uint64_t vsh_hits, vsh_misses, vsh_identity_fail, vsh_declines, vsh_tris, vsh_const_uploads;
    /* The pipeline table's evictions and the persisted pipeline cache (plan B2). */
    uint64_t pipe_evictions, pcache_loaded_bytes, pcache_saves, pcache_save_failures,
             pcache_rejects;
} VbStats;

static struct {
    int ok;
    int failed;                         /* write-once latch: a fatal failure (a wait, an end, a
                                         * submit, a begin) has been observed, so no slot is
                                         * recordable any more. `g.ok` is 0 from that point on
                                         * as well, but `g.ok` is also armed deliberately while
                                         * installing, and the latch is what distinguishes a
                                         * lost device from that. */
    void *lib;
    VkInstance inst;
    VkDebugUtilsMessengerEXT messenger;
    VkPhysicalDevice pd;
    VkPhysicalDeviceMemoryProperties mp;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkFormat depth_format;
    int bc, depth_clamp;                /* features enabled on the device */
    int shader_clip;                    /* shaderClipDistance; the program path needs it */
    VkCommandPool pool;
    /* The buffer for a recording that is not possible: allocated from the same pool, begun once
     * at install and then never ended, submitted or reset. vb_cmd returns it while the current
     * slot must not be recorded into (still pending after a failed wait, or a latched failure),
     * so a stray vkCmd*, transfer or barrier is valid and goes nowhere instead of corrupting a
     * submission the GPU may still read. */
    VkCommandBuffer dead;
    VbSlot slots[VB_SLOTS];
    unsigned cur;                       /* the slot being recorded */
    unsigned npending;                  /* slots submitted whose fence is not observed yet */
    unsigned max_inflight;              /* most slots outstanding (submitted, unwaited) at once */
    VkRenderPass rp[2];                 /* [has depth] */
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkDescriptorSetLayout dsl_rc;       /* the combiner pipeline's set */
    VkPipelineLayout layout_rc;
    VkDescriptorSetLayout dsl_prog;     /* the vertex-program pipeline's set: rc's, plus
                                         * binding 9 (c[192]) and 10 (Nv2aVshHlslXf) */
    VkPipelineLayout layout_prog;
    VkPipelineCache pcache;
    int pc_on;                          /* the cache file is read and written */
    int pc_dirty;                       /* pipelines were made since the last good save */
    uint64_t pc_last_ns;                /* the last save attempt, good or not */
    char pc_path[1024];
    VkPhysicalDeviceProperties pc_props;  /* the chosen device, which a cache file must match */
    int pc_latched;                     /* saving is off for the rest of the run */
    unsigned pc_fails, pc_said;         /* consecutive save failures; log lines printed */
    VkShaderModule vs, ps;
    VkShaderModule vs_rc, ps_rc;
    VkDescriptorSetLayout dsl_pack;     /* the pack kernel's set: the target's own image
                                         * (binding 0) and the device-local pack buffer
                                         * (binding 1, bound whole at offset 0) */
    VkPipelineLayout layout_pack;       /* that set plus the 32-byte pack push block */
    VkShaderModule cs_pack;
    VkPipeline pipe_pack;               /* the compute pipeline (nv2a_vk_pack.hlsl, cs_pack) */
    /* The readback publication's two staging buffers, process-lifetime like the ring's
     * streams and grow-only up to VB_READBACK_MAX (vb_readback_reserve). The host never
     * maps the first; the second is what the host publishes from, once the batch's fence
     * wait is behind it. */
    VkBuffer pack_buf;                  /* device-local: cs_pack writes the packed slices here */
    VbAlloc pack_mem;
    VkBuffer rb_buf;                    /* host-visible: the destination of the 4-byte copies
                                         * and of one buffer copy per packed slice */
    VbAlloc rb_mem;
    uint8_t *rb_ptr;
    VkDeviceSize rb_cap;                /* the pair's shared capacity, 0 until first used */
    VbImage white;                      /* bound when a draw is untextured */
    VkSampler samp[4][9];               /* [filter][addr_u*3 + addr_v], made on first use */
    /* The sub-allocator's pools (VB_POOLS, keyed by memory type index and kind) and the
     * device's nonCoherentAtomSize (0 or 1: no atom alignment is applied). Both are read
     * and written only by the memory section below. */
    VbPool pools[VB_POOLS];
    VkDeviceSize atom;
    /* The open render pass of the RECORDING slot (closed before any submission), and the
     * pipeline bound in its recording command buffer. */
    int pass_open;
    VkCommandBuffer pass_cmd;           /* the buffer the open pass was begun in, remembered so
                                         * vb_end_pass ends the pass there instead of assuming
                                         * the current slot's command buffer */
    VbTarget *pass_t;
    VbDepth *pass_d;
    VbTarget targets[VB_MAX_TARGETS];
    VbDepth depths[VB_MAX_DEPTHS];
    VbTex texs[VB_MAX_TEXTURES];
    VbPipe pipes[VB_MAX_PIPES];
    unsigned npipes;
    VbFb fbs[VB_MAX_FBS];
    unsigned next_fb;
    VbGarbage *garbage;
    size_t ngarbage, garbage_cap;
    uint32_t *scratch;                  /* texture decode buffer */
    size_t scratch_px;
    uint64_t use_clock;
    uint32_t frame;
    uint64_t last_stats_ns;
    int stats_said;
    unsigned fails, validation_said;
    VbStats st;
    /* The vertex-program path's own counters and once-per-hash miss log. */
    Nv2aVkProgramStats pst;
    int hit_seen, miss_seen;            /* the latches are write-once, whatever the hash */
    uint32_t miss_said[VB_MAX_MISS_LOG];
    unsigned nmiss_said;
    int miss_log_full;                  /* the suppression note has been printed */
} g;

/* The ahead-of-time corpus, static storage owned by the caller (nv2a_vk_backend.h). */
static const Nv2aVshAotEntry *s_programs;
static size_t s_nprograms;

void nv2a_vk_backend_set_programs(const Nv2aVshAotEntry *entries, size_t count)
{
    /* A NULL table with a nonzero count is the same as no corpus: a caller that
     * has not built one must not make the lookup walk NULL. */
    s_programs = (entries && count) ? entries : NULL;
    s_nprograms = s_programs ? count : 0;
}

static void vb_fail(const char *what, VkResult r)
{
    if (g.fails++ < 16) {
        fprintf(stderr, "[GPUVK] %s failed (VkResult %d)\n", what, (int)r);
        fflush(stderr);
    }
}

static uint64_t vb_now_ns(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static uint8_t *vb_guest(uint32_t va)
{
    return (uint8_t *)xbox_GetMemoryOffset() + va;
}

static uint32_t *vb_scratch(size_t px)
{
    if (px > g.scratch_px) {
        uint32_t *p = (uint32_t *)realloc(g.scratch, px * 4);
        if (!p)
            return NULL;
        g.scratch = p;
        g.scratch_px = px;
    }
    return g.scratch;
}

/* Cheap 64-bit content hash; it only has to notice that guest bytes changed. */
static uint64_t vb_hash(const uint8_t *p, size_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t)n;
    size_t i;

    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* ── Memory, garbage, command buffer ─────────────────────────────────── */

static int vb_memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;

    for (i = 0; i < g.mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g.mp.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

/* The counters Nv2aVkMemoryStats reports. The live fields of it (pools in use, reserved,
 * used, free, largest free, padding and the allocator's own footprint) are derived from the
 * pools at read time by nv2a_vk_backend_memory_stats; everything else is accumulated here. */
static Nv2aVkMemoryStats s_mem;
/* One counter the frozen public struct has no field for: the bytes of the blocks retired so
 * far. The periodic [GPUVK] line reports it, which is where a run's evidence survives. */
static uint64_t s_retired_bytes;
/* Ranges released while something that could still read them was recording or in flight.
 * vb_pool_gc folds them back into their blocks once the ring has drained. A release the full
 * list cannot admit is refused instead of dropped: the range keeps being counted live in its
 * block and reaches no free list -- nothing is freed and nothing is handed out -- while
 * `parked`/`parked_lost` count the refusal and `released` counts only what was given back, and
 * the back end stops. The VbAlloc the refusal was handed (a by-value vb_garbage_free entry)
 * never survives it, so the range stays pinned and accounted live. */
static VbAlloc s_pool_pending[VB_POOL_PENDING];
static unsigned s_nparked;

/* Round `v` up to a multiple of `a`; 0 when the result is not representable (overflow). */
static int vb_up(VkDeviceSize v, VkDeviceSize a, VkDeviceSize *out)
{
    VkDeviceSize r;

    if (a <= 1) {
        *out = v;
        return 1;
    }
    r = v % a;
    if (!r) {
        *out = v;
        return 1;
    }
    if (v > (VkDeviceSize)-1 - (a - r))
        return 0;
    *out = v + a - r;
    return 1;
}

/* a + b, or 0 on overflow. Every offset and size below goes through vb_up/vb_add: a range's
 * end has to be representable before anything is handed out. */
static int vb_add(VkDeviceSize a, VkDeviceSize b, VkDeviceSize *out)
{
    if (b > (VkDeviceSize)-1 - a)
        return 0;
    *out = a + b;
    return 1;
}

/* The device's nonCoherentAtomSize; 0 or 1 means no atom alignment is required (a device that
 * reports none, and a fixture that never set it). */
static VkDeviceSize vb_atom(void)
{
    return g.atom > 1 ? g.atom : 1;
}

/* Nothing is recording and nothing is submitted-and-unwaited: the only state in which a
 * released range may rejoin its block's free list, or a block be freed. */
static int vb_mem_idle(void)
{
    return !g.slots[g.cur].recording && !g.npending;
}

/* A block nothing lives in. A range that was released but parked still counts in `used`, so a
 * block holding one never looks empty: this is only ever true when nothing could still read
 * the block. */
static int vb_block_empty(const VbBlock *b)
{
    return b->used == 0;
}

/* The pool's live padding, recomputed from its blocks rather than accumulated: a block pads
 * in three different places (the bump cursor's alignment skip, a free range's, and a freed
 * range the full free list cannot record), and a recomputed sum cannot drift from the blocks
 * it describes -- it is what makes `reserved == used + padding + free` hold per pool. */
static void vb_pool_padding(VbPool *p)
{
    uint64_t sum = 0;
    unsigned i;

    for (i = 0; i < VB_POOL_BLOCKS; i++)
        if (p->blocks[i].mem != VK_NULL_HANDLE)
            sum += p->blocks[i].padding;
    p->padding = sum;
}

/* Free a block nothing lives in. Its whole mapping goes first -- nothing may be unmapped
 * while a range of it may live, and `used == 0` is exactly that -- then its memory. */
static void vb_block_retire(VbPool *p, VbBlock *b)
{
    s_retired_bytes += (uint64_t)b->size;
    s_mem.blocks_retired++;
    if (b->map && vkUnmapMemory)
        vkUnmapMemory(g.dev, b->mem);
    vkFreeMemory(g.dev, b->mem, NULL);
    if (p->reserved >= b->size)
        p->reserved -= b->size;
    else
        p->reserved = 0;
    if (p->nblocks)
        p->nblocks--;
    memset(b, 0, sizeof *b);
    vb_pool_padding(p);                 /* the retired block's padding is gone with it */
}

/* How big a block this pool makes for a `need`-byte range: the request rounded up to
 * VB_POOL_MIN_BLOCK, and -- while four times that is still at or below VB_POOL_SHARE_MAX --
 * four times it. Never above VB_POOL_MAX_BLOCK, never below `need`, 0 when the request is not
 * representable or is over the per-block cap at all (the caller has already sent those to a
 * dedicated allocation of their own).
 *
 * The multiple is what makes the whole exercise worth anything for the resources that churn:
 * one range per block would spend one of VB_POOL_BLOCKS block slots per resource, which is
 * exactly the one-allocation-per-resource layout this replaces, and a small or mid range's
 * block has room for the several live ranges that let the free list do the reusing and keep a
 * block alive through churn instead of having it emptied and retired at once. A larger range
 * gets exactly its rounded size instead: over-reserving it would cost more memory than the
 * sharing it buys back (a range past VB_POOL_SHARE_MAX is rarely one of several live ones),
 * so there the block is the request. */
static VkDeviceSize vb_block_size(VkDeviceSize need)
{
    VkDeviceSize size;

    if (!vb_up(need, VB_POOL_MIN_BLOCK, &size) || size > VB_POOL_MAX_BLOCK)
        return 0;
    while (size <= VB_POOL_SHARE_MAX / 4)
        size *= 4;
    return size;
}

/* A new block of at least `need` bytes in `p`, the whole block mapped once when the pool's
 * memory type is host-visible. At VB_POOL_BLOCKS the pool retires fully empty blocks first;
 * when that leaves no slot the request is refused. *refused tells the two refusals apart: 1 is
 * the budget (the pool is at VB_POOL_BLOCKS or VB_POOL_RESERVE, the request goes dedicated as
 * dedicated_noroom), 0 is a failed vkAllocateMemory/vkMapMemory of the block (the request falls
 * back ONCE to a dedicated allocation). Over VB_POOL_MAX_BLOCK is refused too: the caller has
 * already sent those to a dedicated allocation of their own. */
static VbBlock *vb_block_make(VbPool *p, VkDeviceSize need, int *refused)
{
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryPropertyFlags fl = g.mp.memoryTypes[p->type_index].propertyFlags;
    VbBlock *b = NULL;
    VkDeviceSize size;
    unsigned i;
    void *map = NULL;
    VkResult r;

    *refused = 1;
    for (i = 0; i < VB_POOL_BLOCKS && !b; i++)
        if (p->blocks[i].mem == VK_NULL_HANDLE)
            b = &p->blocks[i];
    if (!b) {
        if (g.failed)
            return NULL;                /* a stopped back end frees nothing */
        for (i = 0; i < VB_POOL_BLOCKS; i++)
            if (vb_block_empty(&p->blocks[i]))
                vb_block_retire(p, &p->blocks[i]);
        for (i = 0; i < VB_POOL_BLOCKS && !b; i++)
            if (p->blocks[i].mem == VK_NULL_HANDLE)
                b = &p->blocks[i];
        if (!b)
            return NULL;                /* at VB_POOL_BLOCKS with nothing empty to retire */
    }
    size = vb_block_size(need);
    if (size < need)
        return NULL;                    /* over VB_POOL_MAX_BLOCK, or not representable */
    if (p->reserved >= VB_POOL_RESERVE || size > VB_POOL_RESERVE - p->reserved)
        return NULL;                    /* at VB_POOL_RESERVE */
    ai.allocationSize = size;
    ai.memoryTypeIndex = p->type_index;
    *refused = 0;
    s_mem.alloc_calls++;
    r = vkAllocateMemory(g.dev, &ai, NULL, &b->mem);
    if (r != VK_SUCCESS) {
        s_mem.alloc_failures++;
        vb_fail("vkAllocateMemory", r);
        b->mem = VK_NULL_HANDLE;
        return NULL;
    }
    if (fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        r = vkMapMemory(g.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &map);
        if (r != VK_SUCCESS) {
            /* The block is rolled back whole: nothing was handed out of it. */
            s_mem.map_failures++;
            s_mem.rollbacks++;
            vb_fail("vkMapMemory", r);
            vkFreeMemory(g.dev, b->mem, NULL);
            b->mem = VK_NULL_HANDLE;
            return NULL;
        }
    }
    b->size = size;
    b->bump = 0;
    b->used = 0;
    b->padding = 0;
    b->nfree = 0;
    b->map = (uint8_t *)map;
    p->reserved += size;
    p->nblocks++;
    s_mem.blocks++;
    return b;
}

/* Put [off, off+size) back into b's free ranges, coalescing with everything it touches. A
 * full list cannot record it: those bytes leave `used` and never come back, counted as
 * padding, which is what the block then spends them on (nothing else would be true of them). */
static void vb_block_push_free(VbBlock *b, VkDeviceSize off, VkDeviceSize size)
{
    VkDeviceSize end, fe = 0;
    unsigned i;
    int merged = 1;

    if (!vb_add(off, size, &end))
        return;                         /* cannot happen: it came out of this block */
    while (merged) {
        merged = 0;
        for (i = 0; i < b->nfree; i++) {
            VbFreeRange *f = &b->free_list[i];
            if (!vb_add(f->off, f->size, &fe))
                continue;
            if (f->off > end || off > fe)
                continue;               /* disjoint, and not even touching */
            if (f->off < off)
                off = f->off;
            if (fe > end)
                end = fe;
            b->free_list[i] = b->free_list[--b->nfree];
            merged = 1;
            break;
        }
    }
    if (b->nfree == VB_POOL_FREE_MAX) {
        b->padding += end - off;
        return;
    }
    b->free_list[b->nfree].off = off;
    b->free_list[b->nfree].size = end - off;
    b->nfree++;
}

/* First fit out of the block's free ranges. An aligned head that the range does not consume
 * ([f->off, o)) STAYS on the free list -- it is free, not padding -- so nothing here counts it:
 * padding is only what no free range holds. */
static int vb_block_take_free(VbBlock *b, VkDeviceSize size, VkDeviceSize align,
                              VkDeviceSize *off)
{
    unsigned i;

    for (i = 0; i < b->nfree; i++) {
        VbFreeRange *f = &b->free_list[i];
        VkDeviceSize fe, o, end;

        if (!vb_add(f->off, f->size, &fe) || !vb_up(f->off, align, &o))
            continue;
        if (!vb_add(o, size, &end) || end > fe)
            continue;                   /* this range cannot hold the request */
        if (o == f->off && end == fe) {
            b->free_list[i] = b->free_list[--b->nfree];
        } else if (o == f->off) {
            f->off = end;
            f->size = fe - end;
        } else if (end == fe) {
            f->size = o - f->off;
        } else {
            /* A split: the tail stays free (or is padding when the list is full). */
            VkDeviceSize tail = fe - end;

            f->size = o - f->off;
            if (b->nfree < VB_POOL_FREE_MAX) {
                b->free_list[b->nfree].off = end;
                b->free_list[b->nfree].size = tail;
                b->nfree++;
            } else {
                b->padding += tail;
            }
        }
        *off = o;
        return 1;
    }
    return 0;
}

/* The bump cursor: fresh bytes, high-water marked, aligned to the request. */
static int vb_block_take_bump(VbBlock *b, VkDeviceSize size, VkDeviceSize align,
                              VkDeviceSize *off)
{
    VkDeviceSize o, end;

    if (!vb_up(b->bump, align, &o) || o > b->size)
        return 0;
    if (!vb_add(o, size, &end) || end > b->size)
        return 0;
    b->padding += o - b->bump;
    b->bump = end;
    *off = o;
    return 1;
}

/* Hand `size` aligned bytes out of p: a block's free range first (a reuse), then a block's
 * bump cursor, then a new block. NULL when the pool cannot serve it -- *refused then says
 * whether the budget refused or the device did. */
static VbBlock *vb_pool_take(VbPool *p, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *off,
                             int *reused, int *refused)
{
    VbBlock *b = NULL;
    unsigned i;

    *reused = 0;
    *refused = 1;
    for (i = 0; i < VB_POOL_BLOCKS && !b; i++) {
        if (p->blocks[i].mem != VK_NULL_HANDLE
            && vb_block_take_free(&p->blocks[i], size, align, off)) {
            *reused = 1;
            b = &p->blocks[i];
        }
    }
    for (i = 0; i < VB_POOL_BLOCKS && !b; i++) {
        if (p->blocks[i].mem != VK_NULL_HANDLE
            && vb_block_take_bump(&p->blocks[i], size, align, off))
            b = &p->blocks[i];
    }
    if (!b) {
        b = vb_block_make(p, size, refused);
        if (b && !vb_block_take_bump(b, size, align, off)) {
            /* Cannot happen -- the block was made to fit -- but a block that cannot serve the
             * request is worth nothing: it goes back rather than being handed out wrong. */
            vb_block_retire(p, b);
            b = NULL;
        }
    }
    if (b)
        vb_pool_padding(p);             /* the block may have padded to align the range */
    return b;
}

/* The pool for (bits, type index, kind), or -1. A pool's identity is the memory type index and
 * the kind, and it is only reused when the request's own memoryTypeBits allow its type. */
static int vb_pool_for(uint32_t bits, uint32_t type_index, int nonlinear)
{
    unsigned i;

    if (!(bits & (1u << type_index)))
        return -1;
    for (i = 0; i < VB_POOLS; i++) {
        VbPool *p = &g.pools[i];
        if (p->used && p->type_index == type_index && p->nonlinear == nonlinear)
            return (int)i;
    }
    return -1;
}

static int vb_pool_new(uint32_t type_index, int nonlinear)
{
    unsigned i;

    for (i = 0; i < VB_POOLS; i++) {
        VbPool *p = &g.pools[i];
        if (!p->used) {
            memset(p, 0, sizeof *p);
            p->used = 1;
            p->type_index = type_index;
            p->nonlinear = nonlinear;
            return (int)i;
        }
    }
    return -1;                          /* VB_POOLS pools are all in use */
}

/* The allocator's live bytes: what its blocks hold and what its dedicated allocations are. */
static uint64_t vb_mem_used(void)
{
    uint64_t u = s_mem.dedicated_bytes;
    unsigned i;

    for (i = 0; i < VB_POOLS; i++)
        u += g.pools[i].live;
    return u;
}

static void vb_mem_peak(void)
{
    uint64_t u = vb_mem_used();

    if (u > s_mem.peak_used)
        s_mem.peak_used = u;
}

/* Really give a range back: a pooled range rejoins its block's free list, a dedicated
 * allocation is freed. Only vb_mem_give_now (nothing recorded or in flight can name the
 * range), vb_mem_give while nothing can read it, and vb_pool_gc (after the ring drained) call
 * this. */
static void vb_mem_release(VbAlloc *a)
{
    VbPool *p;
    VbBlock *b;

    if (a->pool < 0) {
        if (s_mem.dedicated_bytes >= (uint64_t)a->size)
            s_mem.dedicated_bytes -= (uint64_t)a->size;
        else
            s_mem.dedicated_bytes = 0;
        vkFreeMemory(g.dev, a->mem, NULL);
        return;
    }
    if (a->pool >= VB_POOLS || a->block < 0 || a->block >= VB_POOL_BLOCKS) {
        /* Neither a pooled index this allocator hands out nor a dedicated -1: a zeroed or
         * corrupted VbAlloc reached the release path. Never write through an index like that --
         * latch it like any other failure and drop the handle. */
        vb_fail("pool release", VK_ERROR_INITIALIZATION_FAILED);
        memset(a, 0, sizeof *a);
        a->pool = -1;
        a->block = -1;
        return;
    }
    p = &g.pools[a->pool];
    b = &p->blocks[a->block];
    b->used = a->size <= b->used ? b->used - a->size : 0;
    p->live = a->size <= p->live ? p->live - a->size : 0;
    vb_block_push_free(b, a->off, a->size);
    vb_pool_padding(p);                 /* a full free list can turn the range into padding */
}

/* Defined below with the rest of the coherence stop; a release the bounded park list cannot
 * admit reports through it (write-once: it latches the stop, announces once and dumps the
 * stats line, so the refusal's parked_lost reaches a report). */
static void vb_stop(const char *why);

/* Give a range back. A pooled range may only be handed out again while nothing that could
 * still read it is recording or in flight: otherwise the range is parked in
 * s_pool_pending[] -- keeping its block's `used` bytes, so the block never looks empty -- and
 * folded back by vb_pool_gc after the drain. Both admitted branches count the range given back
 * (`released`). A release this bounded list cannot admit is REFUSED, not dropped: the range
 * keeps being counted live in its block (`used`/`live`, or `dedicated_bytes`) and no free range
 * ever receives it, so nothing is freed and nothing is handed out, while `parked` and
 * `parked_lost` count the refusal and `released` does not. The back end stops write-once,
 * because a release the allocator cannot take is a state it cannot leave on its own. No VbAlloc
 * keeps the range: every production caller hands in a by-value vb_garbage_free entry, and that
 * copy is dropped rather than kept -- vb_stream's dead-recording branch returns without its
 * local entry, and the deferred-destroy route's drain drops the entry it holds -- so the range
 * is retained and accounted live rather than named by anyone who could hand it back. A range
 * nothing recorded or in flight can name belongs in vb_mem_give_now instead: a burst of
 * rollbacks must never be able to reach this refusal. */
static void vb_mem_give(VbAlloc *a)
{
    if (a->mem == VK_NULL_HANDLE)
        return;
    if (vb_mem_idle()) {
        s_mem.released++;               /* an admitted release: counted given back */
        vb_mem_release(a);
    } else if (s_nparked < VB_POOL_PENDING) {
        s_mem.released++;               /* admitted too: the park copy is a release */
        s_pool_pending[s_nparked++] = *a;
        s_mem.parked++;
        if (s_nparked > s_mem.parked_peak)
            s_mem.parked_peak = s_nparked;
    } else {
        /* Bounded admission failure, reached with `a` pointing at a by-value copy (every
         * production caller goes through a vb_garbage_free entry), so this call is the only
         * thing that could hand the range back. It does not: nothing below decrements the
         * block's `used` or the pool's `live` (or `dedicated_bytes`), no free range is ever
         * given the range and vkFreeMemory is never called, so the range stays pinned and
         * accounted live, nothing is freed and nothing is handed out. The copy naming it is
         * dropped by whoever made it -- vb_stream's dead-recording branch returns without its
         * local entry, the deferred-destroy route's drain clears the list it holds -- and
         * `released` is NOT incremented: a refusal is counted by `parked` and `parked_lost`
         * only. */
        s_mem.parked++;
        s_mem.parked_lost++;
        vb_fail("VB_POOL_PENDING park list", VK_ERROR_OUT_OF_HOST_MEMORY);
        vb_stop("a released range cannot be parked: the park list is full, so the range stays "
                "counted live and reaches no free list -- neither handed out nor freed");
        return;
    }
    memset(a, 0, sizeof *a);
    a->pool = -1;
    a->block = -1;
}

/* Give a range back for good, WITHOUT parking: for an allocation nothing recorded or in flight
 * can reference -- a failed create, bind or map of a brand-new object, or an object destroyed
 * before any recorded command named it. Such a range is not parked, so a burst of rollbacks can
 * never fill the bounded park list. If the object was BOUND to the range, the object must be
 * destroyed FIRST: a bound range may not be handed out again while the object still names it.
 * For a range a recorded or in-flight command may still read, use vb_mem_give (the parked,
 * bounded path) instead. */
static void vb_mem_give_now(VbAlloc *a)
{
    if (a->mem == VK_NULL_HANDLE)
        return;
    s_mem.released++;
    vb_mem_release(a);
    memset(a, 0, sizeof *a);
    a->pool = -1;
    a->block = -1;
}

/* Fold what the parked releases hold back, and retire empty blocks beyond the pool's
 * retention budget (at most VB_POOL_KEEP_EMPTY bytes of empty blocks per pool, the first small
 * enough one). Called after every fence wait (vb_garbage_drain) and by a pool that needs a
 * block: in both cases nothing may still read what was released. */
static void vb_pool_gc(void)
{
    unsigned i, k;
    uint64_t kept;

    if (g.failed)
        return;                         /* a stopped back end frees nothing */
    if (vb_mem_idle()) {
        for (i = 0; i < s_nparked; i++)
            vb_mem_release(&s_pool_pending[i]);
        s_nparked = 0;
    }
    for (i = 0; i < VB_POOLS; i++) {
        VbPool *p = &g.pools[i];

        kept = 0;
        for (k = 0; k < VB_POOL_BLOCKS; k++) {
            VbBlock *b = &p->blocks[k];

            if (b->mem == VK_NULL_HANDLE || !vb_block_empty(b))
                continue;
            if (b->size <= VB_POOL_KEEP_EMPTY && kept + b->size <= VB_POOL_KEEP_EMPTY) {
                kept += b->size;
                continue;
            }
            vb_block_retire(p, b);
        }
    }
}

/* The request one flush or invalidate covers: clipped to [a->off, a->off + a->size) -- never a
 * neighbouring live range of the same block -- and, when the memory type is not coherent,
 * widened OUT to whole nonCoherentAtomSize atoms, which is what the specification requires of
 * a flush or an invalidate on such memory. 0 when there is nothing to order. */
static int vb_mem_range(const VbAlloc *a, VkDeviceSize off, VkDeviceSize size,
                        VkMappedMemoryRange *mr)
{
    VkDeviceSize atom = vb_atom(), s, e;

    if (a->mem == VK_NULL_HANDLE || !a->host_visible || !size)
        return 0;
    if (off > a->size)
        off = a->size;
    if (size > a->size - off)
        size = a->size - off;
    if (!size)
        return 0;
    s = off;
    e = off + size;
    if (!a->coherent && atom > 1) {
        s -= s % atom;
        /* Widened OUT to whole atoms (the specification's rule for memory that is not
         * host-coherent). The rounding is checked: an end that cannot be represented orders
         * nothing, and both callers treat 0 as "cannot". */
        if (e % atom && !vb_add(e, atom - (e % atom), &e))
            return 0;
        if (e > a->size)
            e = a->size;                /* our own range's end, never the neighbour's */
    }
    mr->memory = a->mem;
    mr->offset = a->off + s;
    mr->size = e - s;
    return 1;
}

/* The host has written a range a submission is about to read. A no-op on coherent memory: the
 * write is already visible, and the driver call would only walk a range with nothing to do.
 * A range that needs it and cannot get it returns 0: the caller must not submit. */
static int vb_mem_flush(const VbAlloc *a, VkDeviceSize off, VkDeviceSize size)
{
    VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
    VkResult r;

    if (a->coherent || !vb_mem_range(a, off, size, &mr))
        return 1;
    if (!vkFlushMappedMemoryRanges)
        return 0;
    r = vkFlushMappedMemoryRanges(g.dev, 1, &mr);
    if (r != VK_SUCCESS) {
        vb_fail("vkFlushMappedMemoryRanges", r);
        return 0;
    }
    return 1;
}

/* The device has written a range the host is about to read. Issued for coherent memory too
 * (that is what the publication always did, and the atom rule below only binds memory that is
 * not host-coherent); a range that needs it and cannot get it returns 0. */
static int vb_mem_invalidate(const VbAlloc *a, VkDeviceSize off, VkDeviceSize size)
{
    VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
    VkResult r;

    if (!vb_mem_range(a, off, size, &mr))
        return 1;
    if (!vkInvalidateMappedMemoryRanges)
        return a->coherent ? 1 : 0;
    r = vkInvalidateMappedMemoryRanges(g.dev, 1, &mr);
    if (r != VK_SUCCESS) {
        vb_fail("vkInvalidateMappedMemoryRanges", r);
        return 0;
    }
    return 1;
}

/* The resource's memory requirements and its dedication hints. Both *_MemoryRequirements2
 * entrypoints are OPTIONAL (loaded through vkGetDeviceProcAddr): an absent one means "not
 * available", so the plain vkGet*MemoryRequirements answers and the request is pooled --
 * counted once, like any other pool range. */
static int vb_mem_requirements(VkImage img, VkBuffer buf, VkMemoryRequirements *mr,
                               int *requires_dedicated, int *prefers_dedicated)
{
    *requires_dedicated = 0;
    *prefers_dedicated = 0;
    if (img != VK_NULL_HANDLE && vkGetImageMemoryRequirements2) {
        VkImageMemoryRequirementsInfo2 ir = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 };
        VkMemoryRequirements2 m2 = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
        VkMemoryDedicatedRequirements dr = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };

        ir.image = img;
        m2.pNext = &dr;
        vkGetImageMemoryRequirements2(g.dev, &ir, &m2);
        *mr = m2.memoryRequirements;
        *requires_dedicated = dr.requiresDedicatedAllocation != 0;
        *prefers_dedicated = dr.prefersDedicatedAllocation != 0;
        return 1;
    }
    if (buf != VK_NULL_HANDLE && vkGetBufferMemoryRequirements2) {
        VkBufferMemoryRequirementsInfo2 br = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2 };
        VkMemoryRequirements2 m2 = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
        VkMemoryDedicatedRequirements dr = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };

        br.buffer = buf;
        m2.pNext = &dr;
        vkGetBufferMemoryRequirements2(g.dev, &br, &m2);
        *mr = m2.memoryRequirements;
        *requires_dedicated = dr.requiresDedicatedAllocation != 0;
        *prefers_dedicated = dr.prefersDedicatedAllocation != 0;
        return 1;
    }
    if (img != VK_NULL_HANDLE)
        vkGetImageMemoryRequirements(g.dev, img, mr);
    else if (buf != VK_NULL_HANDLE)
        vkGetBufferMemoryRequirements(g.dev, buf, mr);
    else
        return 0;
    return 1;
}

/* Take memory for one resource: a range out of a pool's block (out->pool >= 0, out->block is
 * the block it came from, out->off the offset to bind at), or a dedicated allocation
 * (out->pool == -1, out->off == 0). 0 means neither was possible and nothing was allocated,
 * handed out or changed.
 *
 * `want` then `fallback` select the memory type exactly as the previous one-allocation-per-
 * resource allocator did, so the same memoryTypeBits, alignment and size are honored. The
 * coherence rule is on the SELECTED type, and it is conservative and deliberate: a type that
 * is host-visible without HOST_COHERENT is never pooled -- a pooled host-visible block is
 * mapped as a whole and one range of it could not be flushed or invalidated without touching
 * the live ranges beside it -- so such a request gets a dedicated allocation instead, counted
 * `dedicated_nohost`. That can also catch a request that never asked for host visibility (a
 * device-local image, say) when the type it lands on is host-visible without coherence; a
 * device that offers the host-visible coherent type every implementation must have does not
 * reach this at all. */
static int vb_mem_take(const VkMemoryRequirements *mr, VkMemoryPropertyFlags want,
                       VkMemoryPropertyFlags fallback, int nonlinear,
                       int requires_dedicated, int prefers_dedicated,
                       const VkImage *dedicated_image, const VkBuffer *dedicated_buffer,
                       VbAlloc *out)
{
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryDedicatedAllocateInfo di = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    VkMemoryPropertyFlags fl;
    VkDeviceSize align, need = 0, off = 0;
    VbBlock *b;
    VbPool *p;
    int t, pi, reused = 0, refused = 0;
    VkResult r;

    memset(out, 0, sizeof *out);
    out->pool = -1;
    out->block = -1;
    if (!s_mem.ranges && !s_mem.dedicated) {
        /* Nothing has been taken yet, so neither write-once latch below has happened: they
         * start at a value no pool index, block index or offset can be, so "never" cannot be
         * read as "the first pool, the first block". */
        s_mem.first_reuse_block = (uint64_t)-1;
        s_mem.first_shared_block = (uint32_t)-1;
    }
    t = vb_memtype(mr->memoryTypeBits, want);
    if (t < 0)
        t = vb_memtype(mr->memoryTypeBits, fallback);
    if (t < 0 && ((want | fallback) & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
        t = vb_memtype(mr->memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (t < 0) {
        vb_fail("memory type", VK_ERROR_FEATURE_NOT_PRESENT);
        return 0;
    }
    fl = g.mp.memoryTypes[t].propertyFlags;
    out->coherent = (fl & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    out->host_visible = (fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    align = mr->alignment > (VkDeviceSize)VB_POOL_ALIGN ? mr->alignment
                                                       : (VkDeviceSize)VB_POOL_ALIGN;
    if (!vb_up(mr->size, align, &need) || !need) {
        vb_fail("memory size", VK_ERROR_OUT_OF_DEVICE_MEMORY);
        return 0;
    }
    /* The dedicated reasons, in the order they are counted: the object's own requirement, the
     * range's size, the pool's budget, and the coherence a pool may not hold. */
    if (requires_dedicated) {
        s_mem.dedicated_required++;
    } else if (prefers_dedicated) {
        s_mem.dedicated_prefer++;
    } else if (need > VB_POOL_MAX_BLOCK) {
        s_mem.dedicated_oversize++;
    } else if (out->host_visible && !out->coherent) {
        s_mem.dedicated_nohost++;
    } else {
        pi = vb_pool_for(mr->memoryTypeBits, (uint32_t)t, nonlinear);
        if (pi < 0)
            pi = vb_pool_new((uint32_t)t, nonlinear);
        if (pi < 0) {
            s_mem.dedicated_noroom++;   /* every pool slot is in use */
        } else {
            p = &g.pools[pi];
            b = vb_pool_take(p, need, align, &off, &reused, &refused);
            if (b) {
                uint64_t was = b->used;

                b->used += need;
                p->live += need;
                p->ranges++;
                s_mem.ranges++;
                out->mem = b->mem;
                out->off = off;
                out->size = need;
                out->pool = pi;
                out->block = (int)(b - p->blocks);
                if (reused) {
                    p->reuse_hits++;
                    s_mem.reuse_hits++;
                    if (s_mem.first_reuse_block == (uint64_t)-1) {
                        s_mem.first_reuse_off = (uint64_t)off;
                        s_mem.first_reuse_block = (uint64_t)out->block;
                    }
                }
                if (was && s_mem.first_shared_block == (uint32_t)-1) {
                    /* Two live ranges in one block: one vkAllocateMemory for both. */
                    s_mem.first_shared_block = (uint32_t)pi;
                    s_mem.first_shared_block_kind = (uint32_t)p->nonlinear;
                }
                vb_mem_peak();
                return 1;
            }
            if (refused)
                s_mem.dedicated_noroom++;
            else
                s_mem.fallbacks++;      /* a block that could not be made: dedicated once */
        }
    }
    /* The dedicated allocation, exactly the size the requirements asked for -- byte for byte
     * what the one-allocation-per-resource allocator made. */
    ai.allocationSize = mr->size;
    ai.memoryTypeIndex = (uint32_t)t;
    if ((requires_dedicated || prefers_dedicated) && (dedicated_image || dedicated_buffer)) {
        /* The object requires or prefers a dedicated allocation, so it gets the matching
         * VkMemoryDedicatedAllocateInfo. (The device is 1.1 or later, which is where the two
         * queries that report these come from; the entries above only count them.) */
        di.image = dedicated_image ? *dedicated_image : VK_NULL_HANDLE;
        di.buffer = dedicated_buffer ? *dedicated_buffer : VK_NULL_HANDLE;
        ai.pNext = &di;
    }
    s_mem.alloc_calls++;
    r = vkAllocateMemory(g.dev, &ai, NULL, &out->mem);
    if (r != VK_SUCCESS) {
        s_mem.alloc_failures++;
        vb_fail("vkAllocateMemory", r);
        out->mem = VK_NULL_HANDLE;
        return 0;
    }
    out->off = 0;
    out->size = ai.allocationSize;
    s_mem.dedicated++;
    s_mem.dedicated_bytes += (uint64_t)ai.allocationSize;
    vb_mem_peak();
    return 1;
}

/* A host-visible buffer, persistently mapped; coherent (and cached when `cached`). The
 * pointer is the range's place in its block's own mapping -- a host-visible block is mapped
 * whole, once, when it is made -- or, for a dedicated allocation, that allocation's mapping.
 * `cached` is a hint like want/fallback: the pool a request lands in is the memory type the
 * requirements' bits and those flags select. */
static int vb_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, int cached,
                          VkBuffer *buf, VbAlloc *mem, uint8_t **ptr)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                   | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    void *p = NULL;
    int reqd = 0, prefd = 0;
    VkResult r;

    *buf = VK_NULL_HANDLE;
    *ptr = NULL;
    memset(mem, 0, sizeof *mem);
    mem->pool = -1;
    mem->block = -1;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = vkCreateBuffer(g.dev, &bi, NULL, buf);
    if (r != VK_SUCCESS) {
        vb_fail("vkCreateBuffer", r);
        return 0;
    }
    vb_mem_requirements(VK_NULL_HANDLE, *buf, &mr, &reqd, &prefd);
    if (!vb_mem_take(&mr, cached ? coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT : coherent,
                     coherent, 0, reqd, prefd, VK_NULL_HANDLE, buf, mem))
        goto fail;
    r = vkBindBufferMemory(g.dev, *buf, mem->mem, mem->off);
    if (r != VK_SUCCESS) {
        s_mem.bind_failures++;
        s_mem.rollbacks++;
        vb_fail("vkBindBufferMemory", r);
        goto fail;
    }
    if (mem->pool >= 0) {
        VbBlock *b = &g.pools[mem->pool].blocks[mem->block];

        p = b->map ? (void *)(b->map + mem->off) : NULL;
    } else {
        r = vkMapMemory(g.dev, mem->mem, 0, VK_WHOLE_SIZE, 0, &p);
        if (r != VK_SUCCESS) {
            s_mem.map_failures++;
            s_mem.rollbacks++;
            vb_fail("vkMapMemory", r);
            goto fail;
        }
    }
    if (!p) {
        s_mem.map_failures++;
        s_mem.rollbacks++;
        vb_fail("vkMapMemory", VK_ERROR_MEMORY_MAP_FAILED);
        goto fail;
    }
    *ptr = (uint8_t *)p;
    return 1;
fail:
    /* The buffer is destroyed FIRST: if this call's bind had succeeded before the map failed,
     * the buffer names the range, and a bound range may not be handed out while the object
     * still names it. Nothing recorded or in flight can name a buffer this call has just made,
     * so the range goes back now (vb_mem_give_now: never parked, so a burst of failed maps or
     * binds can never fill the park list), and the caller's failure path has no handle, no
     * pointer into memory the back end no longer owns, and nothing to release. */
    vkDestroyBuffer(g.dev, *buf, NULL);
    *buf = VK_NULL_HANDLE;
    vb_mem_give_now(mem);
    memset(mem, 0, sizeof *mem);
    mem->pool = -1;
    mem->block = -1;
    return 0;
}

/* A device-local buffer, never mapped: what a compute shader writes and what the copy engine
 * reads from. No fallback to a host-visible type beyond what the requirements' memoryTypeBits
 * allow, since anywhere this memory is legal is correct, only slower. */
static int vb_device_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                            VkBuffer *buf, VbAlloc *mem)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements mr;
    int reqd = 0, prefd = 0;
    VkResult r;

    *buf = VK_NULL_HANDLE;
    memset(mem, 0, sizeof *mem);
    mem->pool = -1;
    mem->block = -1;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = vkCreateBuffer(g.dev, &bi, NULL, buf);
    if (r != VK_SUCCESS) {
        vb_fail("vkCreateBuffer", r);
        return 0;
    }
    vb_mem_requirements(VK_NULL_HANDLE, *buf, &mr, &reqd, &prefd);
    if (!vb_mem_take(&mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, reqd, prefd,
                     VK_NULL_HANDLE, buf, mem))
        goto fail;
    r = vkBindBufferMemory(g.dev, *buf, mem->mem, mem->off);
    if (r == VK_SUCCESS)
        return 1;
    s_mem.bind_failures++;
    s_mem.rollbacks++;
    vb_fail("vkBindBufferMemory", r);
fail:
    /* Same ordering as vb_host_buffer: the object that may be bound to the range is destroyed
     * before the range goes back, and the range is never parked because nothing recorded or in
     * flight can name a buffer this call has just made. */
    vkDestroyBuffer(g.dev, *buf, NULL);
    *buf = VK_NULL_HANDLE;
    vb_mem_give_now(mem);
    memset(mem, 0, sizeof *mem);
    mem->pool = -1;
    mem->block = -1;
    return 0;
}

/* Destroy the objects one retained entry holds, the objects FIRST: an object bound to the
 * entry's range is gone before anything may hand that range out again. */
static void vb_garbage_destroy(VbGarbage *e)
{
    if (e->fb) vkDestroyFramebuffer(g.dev, e->fb, NULL);
    if (e->pipe) vkDestroyPipeline(g.dev, e->pipe, NULL);
    if (e->view) vkDestroyImageView(g.dev, e->view, NULL);
    if (e->img) vkDestroyImage(g.dev, e->img, NULL);
    if (e->buf) vkDestroyBuffer(g.dev, e->buf, NULL);
}

/* Free one retained entry whose objects a recorded or in-flight command may still name: the
 * objects above, then the range through the conservative path (parked while the ring is busy).
 * This is what vb_discard and vb_garbage_drain use. */
static void vb_garbage_free(VbGarbage *e)
{
    vb_garbage_destroy(e);
    /* The range goes back to the pool it came from -- a block's free list, or the dedicated
     * allocation's own free -- instead of one vkFreeMemory per resource. */
    if (e->mem.mem) vb_mem_give(&e->mem);
}

/* Free one entry NOTHING recorded or in flight can name -- the pair vb_readback_reserve made
 * in its own call and never published. The objects go first (they were bound to the range), and
 * the range follows immediately instead of being parked: a failed reserve must not be able to
 * fill the bounded park list. */
static void vb_garbage_free_now(VbGarbage *e)
{
    vb_garbage_destroy(e);
    if (e->mem.mem) vb_mem_give_now(&e->mem);
}

/* Free the held objects once no recorded command can still read them: nothing is being
 * recorded and no submission is in flight. Called after every fence wait. */
static void vb_garbage_drain(void)
{
    size_t i;

    if (g.slots[g.cur].recording || g.npending)
        return;
    for (i = 0; i < g.ngarbage; i++)
        vb_garbage_free(&g.garbage[i]);    /* possibly referenced: the conservative path */
    g.ngarbage = 0;
    /* Every fence wait is also where what the releases above parked -- and the blocks they
     * left empty -- are folded and retired: nothing can still read either. */
    vb_pool_gc();
}

/* Destroy now if nothing recorded can still read it, else hold it until the ring has no
 * outstanding submission: an object released while commands may still use it (an evicted
 * pipeline, a rebuilt target's image, a forgotten framebuffer) outlives those commands. */
static void vb_discard(VbGarbage e)
{
    if (!g.slots[g.cur].recording && !g.npending) {
        /* Possibly referenced (an evicted pipeline's or a rebuilt target's objects) but the
         * ring is idle, so nothing can still read them: still the conservative path. */
        vb_garbage_free(&e);
        return;
    }
    if (g.ngarbage == g.garbage_cap) {
        size_t cap = g.garbage_cap ? g.garbage_cap * 2 : 64;
        VbGarbage *p = (VbGarbage *)realloc(g.garbage, cap * sizeof *p);
        if (!p) {
            /* Leaking beats destroying something the GPU may still read. */
            vb_fail("garbage list", VK_ERROR_OUT_OF_HOST_MEMORY);
            return;
        }
        g.garbage = p;
        g.garbage_cap = cap;
    }
    g.garbage[g.ngarbage++] = e;
}

static void vb_image_discard(VbImage *im)
{
    VbGarbage e;

    /* Possibly referenced: a live image a recorded command may have named (or one whose bind
     * just failed, whose range the caller already released). Either way the entry travels the
     * conservative vb_discard/vb_garbage_free path. */
    memset(&e, 0, sizeof e);
    e.img = im->img;
    e.view = im->view;
    e.mem = im->mem;
    if (e.img || e.view || e.mem.mem)
        vb_discard(e);
    memset(im, 0, sizeof *im);
    im->mem.pool = -1;                  /* an empty VbAlloc is "no memory", not "pool 0" */
    im->mem.block = -1;
}

/* ── The submission ring ─────────────────────────────────────────────── */

/* Defined below in recording order, and used by the ring's submit path. */
static void vb_barrier(void);
static void vb_end_pass(void);
/* Defined below with the rest of the statistics; the coherence stop reports through it. */
static void vb_stats(int force);

/* Wait for a slot's submission. A fence's first synchronization scope includes every command
 * submitted before it, so once this returns the slot's stream, its descriptor pool and the
 * objects its submission may read are all free again. Returns 1 if a fence wait was performed
 * (0 if the slot was already free); nothing polls a fence's state, so a wait that finds the
 * work already done is still counted as a wait. A lost device stops the back end and frees
 * nothing. */
static int vb_slot_wait(unsigned i)
{
    VbSlot *s = &g.slots[i];
    VkResult r;

    if (!s->pending || !g.ok)
        return 0;
    r = vkWaitForFences(g.dev, 1, &s->fence, VK_TRUE, VB_WAIT_NS);
    if (r != VK_SUCCESS) {
        vb_fail(r == VK_TIMEOUT ? "submission wait (10 s timeout)" : "submission wait", r);
        fprintf(stderr, "[GPUVK] the Vulkan back end stops drawing; guest memory keeps "
                        "the last bytes it published for each target rectangle\n");
        fflush(stderr);
        g.ok = 0;
        g.failed = 1;                   /* write-once: no slot is recordable from here on */
        return 1;                       /* nothing is freed: the GPU may still use it */
    }
    vkResetFences(g.dev, 1, &s->fence);
    s->pending = 0;
    g.npending--;
    vkResetDescriptorPool(g.dev, s->dpool, 0);
    s->sets_used = 0;
    s->stream_off = 0;
    vb_garbage_drain();
    return 1;
}

/* End the current slot's recording and submit it, WITHOUT waiting: the device runs this
 * submission while the host records the next slot. Every submission ends with the global
 * barrier outside any pass, so its writes are ordered before the commands of every later
 * submission as well.
 *
 * After this returns, the slot is PENDING and nothing may write into it again (its stream,
 * its descriptor pool, its command buffer): every caller either moves g.cur to another slot
 * (vb_slot_advance, vb_maybe_split) or waits it out with vb_drain first (the flip, a target
 * flush, install), both before anything else records. */
static void vb_slot_submit(void)
{
    VbSlot *s = &g.slots[g.cur];
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkResult r;

    if (!s->recording || !g.ok)
        return;
    vb_end_pass();
    vb_barrier();
    /* The host wrote this slot's stream (vertices, indices, the program path's uniform
     * blocks); a submission is about to read it. Flushing the written prefix is a no-op on
     * coherent memory -- pooled host-visible memory is coherent -- and is the coherence
     * boundary when it is not: without it the device would read stale bytes, so a flush that
     * cannot happen stops the submission instead of submitting work that reads them. */
    if (!vb_mem_flush(&s->stream_mem, 0, s->stream_off)) {
        vb_fail("stream flush", VK_ERROR_FEATURE_NOT_PRESENT);
        fprintf(stderr, "[GPUVK] the Vulkan back end stops drawing; guest memory keeps "
                        "the last bytes it published for each target rectangle\n");
        fflush(stderr);
        s->recording = 0;
        g.ok = 0;
        g.failed = 1;                   /* write-once: no slot is recordable from here on */
        return;                         /* nothing is freed: the GPU may still use it */
    }
    r = vkEndCommandBuffer(s->cmd);
    if (r == VK_SUCCESS) {
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s->cmd;
        r = vkQueueSubmit(g.queue, 1, &si, s->fence);
    }
    if (r != VK_SUCCESS) {
        vb_fail("submission", r);
        fprintf(stderr, "[GPUVK] the Vulkan back end stops drawing; guest memory keeps "
                        "the last bytes it published for each target rectangle\n");
        fflush(stderr);
        s->recording = 0;
        g.ok = 0;
        g.failed = 1;                   /* write-once: no slot is recordable from here on */
        return;                         /* nothing is freed: the GPU may still use it */
    }
    s->recording = 0;
    s->pending = 1;
    g.npending++;
    if (g.npending > g.max_inflight)
        g.max_inflight = g.npending;
    g.st.submits++;
}

/* Publish: every submitted slot is complete, so guest memory is up to date. Counted per slot
 * waited (one publish of a three-slot frame adds up to three). */
static void vb_drain(void)
{
    unsigned i;

    for (i = 0; i < VB_SLOTS; i++)
        if (vb_slot_wait(i))
            g.st.drain_waits++;
}

/* Move to the next slot: submit what the current one recorded (ending its pass and its
 * ordering), then wait only if the slot we move into is still in flight. That is the ring's
 * bound, and the only wait in the middle of a frame. */
static void vb_slot_advance(void)
{
    vb_slot_submit();
    if (!g.ok)
        return;
    g.cur = (g.cur + 1u) % VB_SLOTS;
    if (vb_slot_wait(g.cur))
        g.st.reuse_waits++;
}

/* A draw is about to be recorded: if the recording slot already holds a worthwhile amount
 * of work and another slot is free, submit it now and start the next one. That is what lets
 * the device run the start of a frame while the host records the rest of it.
 *
 * The callers are the three draw entry points, and the call is the FIRST thing they do --
 * before any of this draw's stream slices are written. That is the invariant that makes
 * cross-slot reuse sound: a submission reads only its OWN slot's stream and its own
 * descriptor pool, so waiting for that slot's fence is enough before rewriting either.
 * When no slot is free the recording continues in the current one -- never a wait here;
 * the ring stays bounded through vb_reserve, which advances the slot when the stream or the
 * descriptor pool would overflow. */
static void vb_maybe_split(void)
{
    VbSlot *s = &g.slots[g.cur];
    unsigned next = (g.cur + 1u) % VB_SLOTS;

    if (!s->recording)
        return;
    if (s->stream_off < VB_SPLIT_BYTES && s->sets_used < VB_SPLIT_SETS)
        return;
    if (g.slots[next].pending)
        return;
    vb_slot_submit();
    if (!g.ok)
        return;
    g.cur = next;
    g.st.splits++;
}

static VkCommandBuffer vb_cmd(void)
{
    VbSlot *s = &g.slots[g.cur];

    /* No recording is possible: the current slot is still pending (its fence wait failed) or a
     * fatal failure is latched (its command buffer was never begun, or ended/executed already).
     * Either way its command buffer must not be reset, begun or recorded into, so while nothing
     * is recordable every command that goes through here is recorded into the dead buffer
     * instead: it is begun once at install and never ended, submitted or reset, so any vkCmd*,
     * transfer or barrier into it is valid and discarded. Callers that name
     * g.slots[g.cur].cmd directly do so only behind their own g.ok check -- the case where that
     * slot really is theirs. */
    if (g.failed)
        return g.dead;
    if (!s->recording) {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkResult r;
        /* Never record into a slot still in flight: its command buffer, its descriptor
         * sets and its stream belong to the pending submission. Only a fence wait that
         * failed (the device is gone) can leave one pending here -- and that latches the
         * failure above -- and then nothing is reset or begun. */
        vb_slot_wait(g.cur);
        if (s->pending)
            return g.dead;
        vkResetCommandBuffer(s->cmd, 0);
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        r = vkBeginCommandBuffer(s->cmd, &bi);
        if (r != VK_SUCCESS) {
            /* The slot's command buffer is not usable, so it is left exactly as it is: not
             * begun, never recorded into. The latch makes every later vb_cmd return the
             * dead buffer, and its callers check g.ok, so the failure propagates instead
             * of recording into a buffer the device or the pool no longer owns. */
            vb_fail("vkBeginCommandBuffer", r);
            g.failed = 1;               /* the recording cannot be used: stop the back end */
            g.ok = 0;
            return g.dead;
        }
        s->recording = 1;
        s->bound_pipe = VK_NULL_HANDLE;
    }
    return s->cmd;
}

/* Every write before it is visible to every access after it. Deliberately broad, and used at
 * exactly two sites: the per-submission barrier in vb_slot_submit, whose second scope must be
 * every command later in submission order, and the failure-path barrier in vb_targets_flush,
 * which orders an abandoned batch's writes against the whole next batch. */
static void vb_barrier(void)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };

    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(vb_cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* Order what `src_stage` writes (with `src_access`) before the accesses `dst_stage` makes
 * (`dst_access`). vb_barrier() is the deliberately broad form, for the two sites that really do
 * need every stage and access on both sides; this is the form for a site whose producer and its
 * consumer are both known from the resource's access contract. Every pair here is an access bit
 * that its stage supports, so the scoped form is also what synchronization validation checks. */
static void vb_dep(VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                   VkPipelineStageFlags dst_stage, VkAccessFlags dst_access)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };

    mb.srcAccessMask = src_access;
    mb.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(vb_cmd(), src_stage, dst_stage, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* A layout transition with explicit scopes: the transition's own source and destination,
 * instead of every stage and access. */
static void vb_image_dep(VkImage img, VkImageAspectFlags aspect, VkImageLayout from,
                         VkImageLayout to, VkPipelineStageFlags src_stage,
                         VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                         VkAccessFlags dst_access)
{
    VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };

    ib.srcAccessMask = src_access;
    ib.dstAccessMask = dst_access;
    ib.oldLayout = from;
    ib.newLayout = to;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img;
    ib.subresourceRange.aspectMask = aspect;
    ib.subresourceRange.levelCount = 1;
    ib.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(vb_cmd(), src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &ib);
}

/* Every stage and access this back end touches a target, a depth image or a texture from: a
 * render pass writes its colour attachment and reads/writes its depth-stencil attachment, a draw
 * (or the readback's pack kernel) samples an image, and the transfer engine loads, clears,
 * snapshots and reads images back. COLOR_ATTACHMENT_READ is in the set because the render pass is
 * created with loadOp = VK_ATTACHMENT_LOAD_OP_LOAD and initialLayout GENERAL, so every pass reads
 * the colour attachment's existing contents at COLOR_ATTACHMENT_OUTPUT: the broad barrier's
 * MEMORY_READ covered that read, so the scoped set must carry the matching read bit beside the
 * depth-stencil read it already had. TRANSFER_WRITE is in the set because vb_target_for creates a
 * target (layout transition + clear) and leaves it stale, so the same submission loads it: the
 * clear's write must be ordered before that later write as well, not only before reads. The
 * second scope of a barrier that must order one image write before every later access to that
 * image. */
#define VB_STAGE_IMAGE_DST (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT \
                          | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT \
                          | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT \
                          | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT \
                          | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT \
                          | VK_PIPELINE_STAGE_TRANSFER_BIT)
#define VB_ACCESS_IMAGE_DST (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT \
                           | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT \
                           | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT \
                           | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT \
                           | VK_ACCESS_SHADER_READ_BIT \
                           | VK_ACCESS_TRANSFER_READ_BIT \
                           | VK_ACCESS_TRANSFER_WRITE_BIT)
/* What a render pass writes: its colour attachment, and its depth-stencil attachment's writes. */
#define VB_STAGE_ATTACH (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT \
                       | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT \
                       | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
#define VB_ACCESS_ATTACH_WRITE (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT \
                              | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
/* What a depth-stencil image is consumed by: the two fragment test stages' depth test and
 * stencil op. */
#define VB_STAGE_DEPTH (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT \
                      | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
#define VB_ACCESS_DEPTH (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT \
                       | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)

static void vb_end_pass(void)
{
    if (!g.pass_open)
        return;
    /* The pass is ended in the buffer that began it, which is remembered when it opens: which
     * slot is current does not decide where a pass ends, and must not be assumed to. */
    vkCmdEndRenderPass(g.pass_cmd);
    g.pass_cmd = VK_NULL_HANDLE;
    g.pass_open = 0;
    g.pass_t = NULL;
    g.pass_d = NULL;
    /* The pass's colour and depth-stencil attachment writes before every later access to that
     * target or depth image: a later pass's attachment read/write, a draw or the pack kernel
     * sampling it, the snapshot copy, and the readback's image copy. */
    vb_dep(VB_STAGE_ATTACH, VB_ACCESS_ATTACH_WRITE, VB_STAGE_IMAGE_DST, VB_ACCESS_IMAGE_DST);
}

/* The recording command buffer, outside any render pass, for a transfer. A split is not
 * allowed here: the transfer may read a stream slice the host has just written into this
 * slot, and a submission must only ever read its OWN slot's stream. */
static VkCommandBuffer vb_transfer_cmd(void)
{
    vb_end_pass();
    return vb_cmd();
}

/* Round a stream slice up to its alignment; vb_stream aligns every slice this way,
 * so a caller reserving several slices must sum their rounded sizes. */
static VkDeviceSize vb_round_up(VkDeviceSize bytes)
{
    return (bytes + VB_STREAM_ALIGN - 1) & ~(VkDeviceSize)(VB_STREAM_ALIGN - 1);
}

/* Make sure `bytes` of the current slot's stream and `sets` of its descriptor pool fit, so
 * the allocations that follow cannot submit between being written and used. An overflow
 * moves to the next slot (waiting only if it is still in flight); it never drains the GPU,
 * so a frame's submissions keep overlapping. This is the ONLY thing that moves the slot
 * while recording, and callers do it before the slices it budgets for, so one sequence of
 * slices and its draw stay in one slot.
 *
 * It can fail: the advance's wait on the next slot's fence may find the device lost, which
 * leaves that slot pending (still the GPU's) and g.ok 0. Every caller therefore checks g.ok
 * before its slices' vb_stream, and before writing a single byte into what it hands back. */
static void vb_reserve(VkDeviceSize bytes, uint32_t sets)
{
    VbSlot *s = &g.slots[g.cur];
    VkDeviceSize off = (s->stream_off + VB_STREAM_ALIGN - 1) & ~(VkDeviceSize)(VB_STREAM_ALIGN - 1);

    if (off + bytes > VB_SLOT_STREAM || s->sets_used + sets > VB_SLOT_SETS) {
        g.st.overflows++;
        vb_slot_advance();
    }
}

/* `bytes` of host memory the recording commands can read at *buf + *off. A slice over half
 * the slot's stream, or one that no longer fits what this slot has left, gets a buffer of
 * its own -- never a slot move, so a submission's commands can only read their OWN slot's
 * stream (or a buffer whose destruction waits for the whole ring to drain).
 *
 * Caller invariant: every caller checks g.ok immediately after the vb_reserve that made room
 * for its slice and before this call, and returns if it is 0. A failed fence wait inside that
 * reserve's advance leaves the slot it moved to still pending -- the GPU's -- and g.ok 0, and
 * writing guest pixels here would overwrite bytes that submission may still read. The check is
 * before the write because no later guard can undo it.
 *
 * The latch is refused here as well, so a failure that appears in the middle of a sequence of
 * slices (a failed begin, or a failed wait in an earlier caller) hands out no further slice:
 * a slot's stream is only ever the caller's while the recording into it is alive. NULL is the
 * answer then, which every caller already handles. */
static uint8_t *vb_stream(VkDeviceSize bytes, VkBuffer *buf, VkDeviceSize *off)
{
    VbSlot *s = &g.slots[g.cur];
    VkDeviceSize o = (s->stream_off + VB_STREAM_ALIGN - 1) & ~(VkDeviceSize)(VB_STREAM_ALIGN - 1);

    if (g.failed)                       /* nothing is recordable, so nothing may be written */
        return NULL;
    if (bytes > VB_SLOT_STREAM / 2 || o + bytes > VB_SLOT_STREAM) {
        VbGarbage e;
        uint8_t *p;
        memset(&e, 0, sizeof e);
        /* A slice of its own is used the same way a stream slice is: as vertices, indices, a
         * transfer source, or a uniform block (the program path's rc/c[192]/xf slices). Its
         * range is host-written like the slot stream's, but it is not the same range, so the
         * submit-time flush of s->stream_mem does not cover it: a device whose memory types
         * denied this buffer HOST_COHERENT would let the GPU read stale bytes here, exactly as
         * the one-allocation-per-resource path did before this change (nothing was flushed
         * anywhere then). It cannot happen on a device that offers the host-visible coherent
         * type every Vulkan implementation must have; the write-back hazard of flushing a
         * released range that a pending submission may still be writing makes a blanket flush
         * of the garbage list the wrong repair, so this stays as it is and is disclosed. */
        if (!vb_host_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                   | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                   | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                                   | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0,
                            &e.buf, &e.mem, &p))
            return NULL;
        vb_cmd();                       /* the command that reads this buffer has to be recorded
                                         * in a live pass of the ring -- see below */
        if (g.failed) {
            /* There is no submission to hold this buffer any more (the dead buffer is not one),
             * so vb_discard would free it below and hand back a dangling pointer. Free it here
             * and refuse the slice instead: every caller checks for NULL. Conservative path on
             * purpose: the slice is refused, the recording is dead, and refusing is the safe
             * answer, so this is not a rollback that may hand a range out early. */
            vb_garbage_free(&e);
            return NULL;
        }
        vb_discard(e);
        *buf = e.buf;
        *off = 0;
        return p;
    }
    s->stream_off = o + bytes;
    *buf = s->stream;
    *off = o;
    return s->stream_ptr + o;
}

/* ── Fixed pipeline state ────────────────────────────────────────────── */

/* NV2A (GL) blend factor, as gb_blend_factor maps it to D3D11; the alpha slot cannot
 * take *_COLOR factors there, and the constant-alpha factors use the constant colour
 * (vb_setup fills it with the alpha, as D3D11 does). */
static VkBlendFactor vb_blend_factor(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                              : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA
                              : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: case 0x8003: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    default:     return VK_BLEND_FACTOR_ONE;    /* as the CPU path's default */
    }
}

static VkBlendOp vb_blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default:     return VK_BLEND_OP_ADD;
    }
}

/* GL_NEVER..GL_ALWAYS (0x200..0x207) are in Vulkan's order, from 0. */
static VkCompareOp vb_compare(uint32_t func)
{
    return (func >= 0x200 && func <= 0x207) ? (VkCompareOp)(func - 0x200) : VK_COMPARE_OP_ALWAYS;
}

/* GL INCR/DECR saturate (Vulkan *_AND_CLAMP); the _WRAP forms are *_AND_WRAP. */
static VkStencilOp vb_stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;     /* 0x1E00 */
    }
}

/* Depth only, stencil off; gb_dss_key. */
static void vb_dss_key(VbPipeKey *k, uint32_t enable, uint32_t func, uint32_t write)
{
    k->z_enable = enable;
    k->z_func = func;
    k->z_write = write;
    k->s_enable = k->s_func = k->s_fail = k->s_zfail = k->s_zpass = 0;
}

/* The blend part of a key; gb_blend_state's normalisation (off: no factors). */
static void vb_blend_key(VbPipeKey *k, uint32_t enable, uint32_t src, uint32_t dst,
                         uint32_t eq, uint32_t mask)
{
    if (!enable)
        src = dst = eq = 0;
    k->blend = enable;
    k->src = src;
    k->dst = dst;
    k->eq = eq;
    k->wmask = mask;
}

/* The corpus shader of the pipeline being built, or NULL; prog pipelines make their
 * module per creation from the entry and destroy it as soon as the pipeline exists. */
static VkPipeline vb_pipeline_create(const VbPipeKey *k, const Nv2aVshAotEntry *e)
{
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    };
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb = { 0, sizeof(Nv2aVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[4] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Nv2aVertex, x) },
        { 1, 0, VK_FORMAT_R32_SFLOAT, offsetof(Nv2aVertex, rhw) },
        /* 0xAARRGGBB read as R8G8B8A8 arrives as B,G,R,A; the shader swizzles it. */
        { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Nv2aVertex, diffuse) },
        { 3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Nv2aVertex, u) },
    };
    /* The combiner vertex, in D3D11's GbRcVertex input layout. */
    VkVertexInputBindingDescription vb_rc = { 0, sizeof(VbRcVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va_rc[9] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(VbRcVertex, x) },
        { 1, 0, VK_FORMAT_R32_SFLOAT,          offsetof(VbRcVertex, rhw) },
        { 2, 0, VK_FORMAT_R8G8B8A8_UNORM,      offsetof(VbRcVertex, d0) },
        { 3, 0, VK_FORMAT_R8G8B8A8_UNORM,      offsetof(VbRcVertex, d1) },
        { 4, 0, VK_FORMAT_R32_SFLOAT,          offsetof(VbRcVertex, fog) },
        { 5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VbRcVertex, t) },
        { 6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VbRcVertex, t) + 16 },
        { 7, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VbRcVertex, t) + 32 },
        { 8, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VbRcVertex, t) + 48 },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkVertexInputBindingDescription vb_prog;
    VkVertexInputAttributeDescription va_prog[16];
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule prog_mod = VK_NULL_HANDLE;
    uint32_t nattr = 0, rank = 0, bit;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r;

    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    if (k->prog && (!e || e->hash != k->prog_hash)) {
        /* The key names a program the entry does not carry: refuse rather than bake
         * the wrong shader. */
        vb_fail("vertex-program pipeline key", VK_ERROR_INITIALIZATION_FAILED);
        return VK_NULL_HANDLE;
    }
    if (k->prog) {
        /* One module per corpus program, made and destroyed around this call. */
        smi.codeSize = e->spirv_size;
        smi.pCode = e->spirv;
        r = vkCreateShaderModule(g.dev, &smi, NULL, &prog_mod);
        if (r != VK_SUCCESS) {
            vb_fail("vkCreateShaderModule (vertex program)", r);
            return VK_NULL_HANDLE;
        }
        st[0].module = prog_mod;
        st[0].pName = "vs_prog";
    } else {
        st[0].module = k->rc ? g.vs_rc : g.vs;
        st[0].pName = k->rc ? "vs_rc" : "vs_main";
    }
    st[1].module = (k->rc || k->prog) ? g.ps_rc : g.ps;
    st[1].pName = (k->rc || k->prog) ? "ps_rc" : "ps_main";

    if (k->prog) {
        /* The corpus shader's inputs: one float4 per set bit of the mask, in
         * ascending n, exactly the [[vk::location(n)]] the tool annotated them
         * with, and the layout p->inputs is uploaded in. */
        memset(&vb_prog, 0, sizeof vb_prog);
        for (bit = 0; bit < 16; bit++) {
            if (!((k->input_mask >> bit) & 1u))
                continue;
            va_prog[nattr].location = bit;
            va_prog[nattr].binding = 0;
            va_prog[nattr].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            va_prog[nattr].offset = 16u * rank;
            nattr++;
            rank++;
        }
        vb_prog.binding = 0;
        vb_prog.stride = 16u * rank;
        vb_prog.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vb_prog;
        vi.vertexAttributeDescriptionCount = nattr;
        vi.pVertexAttributeDescriptions = va_prog;
    } else {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = k->rc ? &vb_rc : &vb;
        vi.vertexAttributeDescriptionCount = k->rc ? 9 : 4;
        vi.pVertexAttributeDescriptions = k->rc ? va_rc : va;
    }
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    /* D3D11's DepthClipEnable=FALSE: no near/far clipping, depth clamped to the viewport. */
    rs.depthClampEnable = g.depth_clamp ? VK_TRUE : VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = k->cull == 1 ? VK_CULL_MODE_BACK_BIT
                : k->cull == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = k->ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    ds.depthTestEnable = k->z_enable ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = k->z_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = vb_compare(k->z_func);
    ds.stencilTestEnable = k->s_enable ? VK_TRUE : VK_FALSE;
    ds.front.failOp = vb_stencil_op(k->s_fail);
    ds.front.depthFailOp = vb_stencil_op(k->s_zfail);
    ds.front.passOp = vb_stencil_op(k->s_zpass);
    ds.front.compareOp = vb_compare(k->s_func);
    ds.back = ds.front;                 /* NV097 has one stencil state for both faces */
    ds.maxDepthBounds = 1.0f;

    memset(&ba, 0, sizeof ba);
    ba.blendEnable = k->blend ? VK_TRUE : VK_FALSE;
    if (k->blend) {
        ba.srcColorBlendFactor = vb_blend_factor(k->src, 0);
        ba.dstColorBlendFactor = vb_blend_factor(k->dst, 0);
        ba.colorBlendOp = vb_blend_op(k->eq);
        ba.srcAlphaBlendFactor = vb_blend_factor(k->src, 1);
        ba.dstAlphaBlendFactor = vb_blend_factor(k->dst, 1);
        ba.alphaBlendOp = vb_blend_op(k->eq);
    } else {
        ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    }
    ba.colorWriteMask = k->wmask;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    dy.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;

    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = k->prog ? g.layout_prog : k->rc ? g.layout_rc : g.layout;
    pi.renderPass = g.rp[k->has_depth ? 1 : 0];
    r = vkCreateGraphicsPipelines(g.dev, g.pcache, 1, &pi, NULL, &pipe);
    if (prog_mod)
        vkDestroyShaderModule(g.dev, prog_mod, NULL);
    if (r != VK_SUCCESS) {
        vb_fail("vkCreateGraphicsPipelines", r);
        return VK_NULL_HANDLE;
    }
    g.st.pipelines++;
    g.pc_dirty = 1;
    return pipe;
}

/* ── The ahead-of-time vertex-program corpus ─────────────────────────── */

/* Log a program hash once, then keep quiet about it. `e` NULL: the corpus does not
 * carry it; otherwise: the corpus carries the hash but the identity did not match. */
static void vb_program_log_once(const Nv2aProgramBatch *p, const Nv2aVshAotEntry *e)
{
    unsigned i;

    for (i = 0; i < g.nmiss_said; i++)
        if (g.miss_said[i] == p->program_hash)
            return;
    if (g.nmiss_said >= VB_MAX_MISS_LOG) {
        if (!g.miss_log_full) {
            g.miss_log_full = 1;
            fprintf(stderr, "[GPUVK] %u distinct vertex programs have been reported;"
                            " further ones are not logged\n", VB_MAX_MISS_LOG);
            fflush(stderr);
        }
        return;
    }
    g.miss_said[g.nmiss_said++] = p->program_hash;
    if (!e) {
        fprintf(stderr, "[GPUVK] vertex program %08X (len %u, inputs %04X) is not in the"
                        " ahead-of-time corpus; the CPU interpreter draws it\n",
                p->program_hash, p->program_len, p->input_mask);
    } else {
        /* The entry failed the identity check, so its storage is exactly what must
         * not be walked here: `e->words` may be NULL and `e->len` may be wrong.
         * Only scalars of `e` and the executor-validated batch are read, and the
         * reason is chosen from those scalars alone. */
        const char *why;

        if (e->len != p->program_len)
            why = "the instruction count differs";
        else if (e->input_mask != p->input_mask)
            why = "the input mask differs";
        else if (!e->words)
            why = "the corpus entry has no words";
        else if (!e->spirv || !e->spirv_size)
            why = "the corpus entry has no shader";
        else
            why = "the words or digest differ";
        fprintf(stderr, "[GPUVK] vertex program %08X matched hash but not identity"
                        " (%s; len %u/%u, inputs %04X/%04X, digest %016llX);"
                        " the CPU interpreter draws it\n",
                p->program_hash, why, e->len, p->program_len,
                e->input_mask, p->input_mask,
                (unsigned long long)e->digest);
    }
    fflush(stderr);
}

/* The entry `hash` names, or NULL. The table is sorted ascending by hash (the
 * generator guarantees it), so a binary search finds it; a candidate is usable
 * only when len, input_mask, the digest recomputed from its own words and the
 * words themselves all match what the batch reports. Any mismatch is a corrupt
 * or colliding identity, never another program's shader. */
static const Nv2aVshAotEntry *vb_program_lookup(const Nv2aProgramBatch *p)
{
    size_t lo = 0, hi = s_nprograms;
    const Nv2aVshAotEntry *e;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s_programs[mid].hash < p->program_hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= s_nprograms || s_programs[lo].hash != p->program_hash) {
        g.pst.misses++;
        g.st.vsh_misses++;
        if (!g.miss_seen) {
            g.miss_seen = 1;
            g.pst.first_miss_hash = p->program_hash;
            g.pst.first_miss_len = p->program_len;
            g.pst.first_miss_mask = p->input_mask;
        }
        vb_program_log_once(p, NULL);
        return NULL;
    }
    e = &s_programs[lo];
    /* Every identity field the batch reports must agree, and the digest is
     * recomputed from both the entry's words and the batch's, so a table whose
     * identity fields were corrupted cannot select another program's shader.
     *
     * THE ORDER OF THESE TERMS IS LOAD-BEARING. Each dereference below is guarded
     * by an earlier term, so a reorder can walk storage that must not be walked:
     * `e->len != p->program_len` must stay first, and `!e->words` must stay before
     * both `nv2a_vsh_aot_digest(e->words, ...)` and the `memcmp`, or a NULL or
     * oversized entry is read instead of rejected. `p->program`/`p->program_len`
     * are guarded by the caller (vb_draw_program). */
    if (e->len != p->program_len
        || e->input_mask != p->input_mask
        || !e->words || !e->spirv || !e->spirv_size
        || nv2a_vsh_aot_digest(e->words, e->len) != e->digest
        || nv2a_vsh_aot_digest(p->program, p->program_len) != e->digest
        || memcmp(e->words, p->program, (size_t)e->len * 16u) != 0) {
        g.pst.identity_fail++;
        g.st.vsh_identity_fail++;
        vb_program_log_once(p, e);
        return NULL;
    }
    return e;
}

/* The pipeline for a key, from the cache or made now; the least recently used goes. */
static VkPipeline vb_pipeline(const VbPipeKey *k, const Nv2aVshAotEntry *e)
{
    VbPipe *slot = NULL;
    unsigned i;

    for (i = 0; i < g.npipes; i++) {
        if (memcmp(&g.pipes[i].k, k, sizeof *k) == 0) {
            g.pipes[i].last_use = ++g.use_clock;
            return g.pipes[i].pipe;
        }
        if (!slot || g.pipes[i].last_use < slot->last_use)
            slot = &g.pipes[i];
    }
    if (g.npipes < VB_MAX_PIPES) {
        slot = &g.pipes[g.npipes++];
    } else {
        VbGarbage gb;
        g.st.pipe_evictions++;
        memset(&gb, 0, sizeof gb);
        gb.pipe = slot->pipe;
        if (gb.pipe == g.slots[g.cur].bound_pipe)
            g.slots[g.cur].bound_pipe = VK_NULL_HANDLE;
        vb_discard(gb);
    }
    slot->k = *k;
    slot->pipe = vb_pipeline_create(k, e);
    slot->last_use = ++g.use_clock;
    if (!slot->pipe) {
        /* Do not cache the failure under a key that later succeeds elsewhere. */
        memset(&slot->k, 0xFF, sizeof slot->k);
    }
    return slot->pipe;
}

/* ── The persisted pipeline cache ────────────────────────────────────── */

enum {
    VB_PC_OK, VB_PC_SHORT, VB_PC_MAGIC, VB_PC_VERSION, VB_PC_TOOBIG, VB_PC_LENGTH, VB_PC_CRC,
    VB_PC_VKHEADER, VB_PC_DEVICE, VB_PC_REASONS
};

static const char *const vb_pc_reason[VB_PC_REASONS] = {
    "ok",
    "shorter than the file header",
    "bad magic",
    "unsupported file version",
    "blob length over the 64 MiB limit",
    "blob length disagrees with the file size",
    "CRC-32 mismatch",
    "blob has no usable Vulkan pipeline cache header",
    "cache was made by another device or driver",
};

static const char vb_pc_magic[8] = { 'X', 'B', 'R', 'V', 'K', 'P', 'C', '1' };

static uint32_t vb_crc32(const uint8_t *p, size_t n)
{
    static uint32_t tab[256];
    uint32_t c = 0xFFFFFFFFu;
    size_t i;

    if (!tab[1]) {
        uint32_t k, j, v;
        for (k = 0; k < 256; k++) {
            v = k;
            for (j = 0; j < 8; j++)
                v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            tab[k] = v;
        }
    }
    for (i = 0; i < n; i++)
        c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static uint32_t vb_pc_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void vb_pc_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* The file header for a blob; the file is these VB_PCACHE_HDR bytes then the blob. */
static void vb_pcache_header(uint8_t hdr[VB_PCACHE_HDR], const void *blob, size_t n)
{
    memcpy(hdr, vb_pc_magic, 8);
    vb_pc_wr32(hdr + 8, VB_PCACHE_VER);
    vb_pc_wr32(hdr + 12, (uint32_t)n);
    vb_pc_wr32(hdr + 16, vb_crc32((const uint8_t *)blob, n));
}

/* Judge a whole cache file against the chosen device. VB_PC_OK points the blob outputs into `buf`;
 * anything else says why the file must not seed the cache. The blob's own header is
 * VkPipelineCacheHeaderVersionOne: length, version, vendor, device, pipelineCacheUUID. */
static int vb_pcache_parse(const uint8_t *buf, size_t n, const VkPhysicalDeviceProperties *p,
                           const void **blob, size_t *blob_n)
{
    const uint8_t *b;
    uint32_t len;

    if (blob)
        *blob = NULL;
    if (blob_n)
        *blob_n = 0;
    if (!buf || n < VB_PCACHE_HDR)
        return VB_PC_SHORT;
    if (memcmp(buf, vb_pc_magic, 8) != 0)
        return VB_PC_MAGIC;
    if (vb_pc_rd32(buf + 8) != VB_PCACHE_VER)
        return VB_PC_VERSION;
    len = vb_pc_rd32(buf + 12);
    if (len > VB_PCACHE_MAX)
        return VB_PC_TOOBIG;
    if ((size_t)len != n - VB_PCACHE_HDR)
        return VB_PC_LENGTH;
    b = buf + VB_PCACHE_HDR;
    if (vb_crc32(b, len) != vb_pc_rd32(buf + 16))
        return VB_PC_CRC;
    if (len < 16u + VK_UUID_SIZE || vb_pc_rd32(b) < 16u + VK_UUID_SIZE
        || vb_pc_rd32(b) > len || vb_pc_rd32(b + 4) != (uint32_t)VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
        return VB_PC_VKHEADER;
    if (!p || vb_pc_rd32(b + 8) != p->vendorID || vb_pc_rd32(b + 12) != p->deviceID
        || memcmp(b + 16, p->pipelineCacheUUID, VK_UUID_SIZE) != 0)
        return VB_PC_DEVICE;
    if (blob)
        *blob = b;
    if (blob_n)
        *blob_n = len;
    return VB_PC_OK;
}

static int vb_pc_ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

/* Decide where the cache lives. `env` is RECOMP_VK_PIPELINE_CACHE, `appdata` is %LOCALAPPDATA%
 * (NULL off Windows) and `vendor`/`device` name the chosen physical device, so each device has
 * its own default file. Returns 1 and fills `out` with a path, 0 for off, -1 for a path that does
 * not fit `cap`. An explicit path is used as given. */
static int vb_pcache_resolve(const char *env, const char *appdata, uint32_t vendor,
                             uint32_t device, char *out, size_t cap)
{
    int n;

    if (env && *env) {
        if (!strcmp(env, "0") || vb_pc_ieq(env, "off"))
            return 0;
        n = snprintf(out, cap, "%s", env);
    } else if (appdata && *appdata) {
        n = snprintf(out, cap, "%s\\xboxrecomp\\vk-pipeline-cache-%08x-%08x.bin", appdata,
                     (unsigned)vendor, (unsigned)device);
    } else {
        return 0;
    }
    return n > 0 && (size_t)n < cap ? 1 : -1;
}

static void vb_pcache_mkparent(const char *path)
{
    char dir[1024];
    size_t n = strlen(path);

    while (n && path[n - 1] != '\\' && path[n - 1] != '/')
        n--;
    if (n < 2 || n >= sizeof dir)
        return;
    memcpy(dir, path, n - 1);
    dir[n - 1] = 0;
#if defined(_WIN32)
    CreateDirectoryA(dir, NULL);
#else
    mkdir(dir, 0777);
#endif
}

/* The pipeline cache's own log lines are capped like vb_fail's; the load line is not counted. */
#define VB_PCACHE_SAY 8
#define VB_PCACHE_FAILS 3               /* consecutive save failures that turn saving off */

static void vb_pcache_say(const char *fmt, const char *a)
{
    if (g.pc_said++ < VB_PCACHE_SAY) {
        fprintf(stderr, fmt, a);
        fflush(stderr);
    }
}

/* Read the file named by g.pc_path and, when it is valid for this device, return it in a
 * malloc'd buffer whose blob is returned through the two outputs (the caller frees the return
 * value). `outcome` receives "cold" or "rejected(<reason>)" when nothing is returned. */
static uint8_t *vb_pcache_load(const void **blob, size_t *blob_n, char *outcome, size_t cap)
{
    FILE *f = fopen(g.pc_path, "rb");
    uint8_t *buf = NULL;
    long sz;
    int why;

    if (!f) {
        snprintf(outcome, cap, "cold");
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        why = VB_PC_SHORT;
    } else if ((unsigned long)sz > (unsigned long)VB_PCACHE_MAX + VB_PCACHE_HDR) {
        why = VB_PC_TOOBIG;
    } else if (!(buf = (uint8_t *)malloc(sz ? (size_t)sz : 1u))
               || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        why = VB_PC_SHORT;
    } else {
        why = vb_pcache_parse(buf, (size_t)sz, &g.pc_props, blob, blob_n);
    }
    fclose(f);
    if (why != VB_PC_OK) {
        g.st.pcache_rejects++;
        snprintf(outcome, cap, "rejected(%s)", vb_pc_reason[why]);
        free(buf);
        return NULL;
    }
    return buf;
}

/* Decide the knob and read the file. Runs before the cache is created; the caller prints the
 * one decision line (vb_pcache_report) once it knows whether the driver took the data. */
static void vb_pcache_init(const void **blob, size_t *blob_n, uint8_t **file, char *outcome,
                           size_t cap)
{
    const char *env = getenv("RECOMP_VK_PIPELINE_CACHE");
#if defined(_WIN32)
    const char *appdata = getenv("LOCALAPPDATA");
#else
    const char *appdata = NULL;
#endif
    int r = 0;

    *file = NULL;
    g.pc_on = 0;
    g.pc_path[0] = 0;
    snprintf(outcome, cap, "off");
    if (vkGetPhysicalDeviceProperties) {
        vkGetPhysicalDeviceProperties(g.pd, &g.pc_props);
        r = vb_pcache_resolve(env, appdata, g.pc_props.vendorID, g.pc_props.deviceID, g.pc_path,
                              sizeof g.pc_path);
    }
    if (r < 0) {
        g.pc_path[0] = 0;
        snprintf(outcome, cap, "off (path too long)");
    }
    if (r > 0) {
        g.pc_on = 1;
        *file = vb_pcache_load(blob, blob_n, outcome, cap);
    }
}

static void vb_pcache_report(const char *outcome)
{
    if (g.pc_on)
        fprintf(stderr, "[GPUVK] pipeline cache: %s %s\n", g.pc_path, outcome);
    else
        fprintf(stderr, "[GPUVK] pipeline cache: %s\n", outcome);
    fflush(stderr);
}

static void vb_pcache_save_failed(const char *what)
{
    g.st.pcache_save_failures++;
    vb_pcache_say("[GPUVK] pipeline cache: save failed (%s)\n", what);
    if (++g.pc_fails >= VB_PCACHE_FAILS) {
        g.pc_latched = 1;
        vb_pcache_say("[GPUVK] pipeline cache: saving stopped after %s consecutive failures\n",
                      "3");
    }
}

/* Called at every flip: write the cache when pipelines were made since the last good save and
 * the last attempt is at least VB_PCACHE_SAVE_MS ago. A failure is counted and logged, never
 * fatal, and is retried no sooner than the next interval; three in a row stop saving for the
 * run. The file is written beside its target as <path>.<pid>.tmp and renamed over it, and an
 * image our own loader would reject is never written (saving stops, the old file stays). */
static void vb_pcache_maybe_save(void)
{
    char tmp[1100];
    uint64_t now;
    uint8_t *img = NULL;
    size_t sz = 0, got;
    FILE *f;
    VkResult r;
    int why, ok;

    if (!g.pc_on || !g.pc_dirty || g.pc_latched)
        return;
    now = vb_now_ns();
    if (now - g.pc_last_ns < (uint64_t)VB_PCACHE_SAVE_MS * 1000000ull)
        return;
    g.pc_last_ns = now;
    if (!vkGetPipelineCacheData || !g.pcache) {
        vb_pcache_save_failed("no vkGetPipelineCacheData");
        return;
    }
#if defined(_WIN32)
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", g.pc_path, (unsigned long)GetCurrentProcessId());
#else
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", g.pc_path, (unsigned long)getpid());
#endif
    vb_pcache_mkparent(g.pc_path);
    if (!(f = fopen(tmp, "wb"))) {
        vb_pcache_save_failed("cannot open the temporary file");
        return;
    }
    if (vkGetPipelineCacheData(g.dev, g.pcache, &sz, NULL) != VK_SUCCESS || !sz
        || sz > VB_PCACHE_MAX) {
        vb_pcache_save_failed("size query");
        goto fail;
    }
    if (!(img = (uint8_t *)malloc(VB_PCACHE_HDR + sz))) {
        vb_pcache_save_failed("out of memory");
        goto fail;
    }
    got = sz;
    r = vkGetPipelineCacheData(g.dev, g.pcache, &got, img + VB_PCACHE_HDR);
    if (r != VK_SUCCESS || !got || got > sz) {
        vb_pcache_save_failed(r == VK_INCOMPLETE ? "vkGetPipelineCacheData was incomplete"
                                                 : "vkGetPipelineCacheData");
        goto fail;
    }
    vb_pcache_header(img, img + VB_PCACHE_HDR, got);
    why = vb_pcache_parse(img, VB_PCACHE_HDR + got, &g.pc_props, NULL, NULL);
    if (why != VB_PC_OK) {
        g.st.pcache_save_failures++;
        g.pc_latched = 1;
        vb_pcache_say("[GPUVK] pipeline cache: not saved, the data would be rejected on load "
                      "(%s); saving stopped\n", vb_pc_reason[why]);
        goto fail;
    }
    ok = fwrite(img, 1, VB_PCACHE_HDR + got, f) == VB_PCACHE_HDR + got;
    ok = fflush(f) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    f = NULL;
#if defined(_WIN32)
    ok = ok && MoveFileExA(tmp, g.pc_path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    ok = ok && rename(tmp, g.pc_path) == 0;
#endif
    if (!ok) {
        vb_pcache_save_failed("file write");
        goto fail;
    }
    g.st.pcache_saves++;
    g.pc_dirty = 0;
    g.pc_fails = 0;
    free(img);
    return;
fail:
    if (f)
        fclose(f);
    remove(tmp);
    free(img);
}

/* ── Images, framebuffers, render passes ─────────────────────────────── */

static int vb_image_create(VbImage *im, uint32_t w, uint32_t h, VkFormat fmt,
                           VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements mr;
    int reqd = 0, prefd = 0;
    VkResult r;

    memset(im, 0, sizeof *im);
    im->mem.pool = -1;
    im->mem.block = -1;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = w;
    ii.extent.height = h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = vkCreateImage(g.dev, &ii, NULL, &im->img);
    if (r != VK_SUCCESS) {
        vb_fail("vkCreateImage", r);
        im->img = VK_NULL_HANDLE;
        return 0;
    }
    /* An image is an optimal-tiling resource, so its ranges live in a nonlinear pool: an
     * image and a buffer never share a block, and bufferImageGranularity can never apply
     * between two neighbouring ranges. */
    if (!vb_mem_requirements(im->img, VK_NULL_HANDLE, &mr, &reqd, &prefd)
        || !vb_mem_take(&mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 1, reqd, prefd, &im->img,
                        VK_NULL_HANDLE, &im->mem)) {
        vb_image_discard(im);
        return 0;
    }
    r = vkBindImageMemory(g.dev, im->img, im->mem.mem, im->mem.off);
    if (r != VK_SUCCESS) {
        /* Nothing was ever recorded against this range and the image is not bound to it (the
         * bind is what failed), so nothing recorded or in flight can name the range: it goes
         * straight back through vb_mem_give_now, never parked, so 65 failed binds cannot fill
         * the bounded park list. The image is then discarded exactly as before: the caller
         * sees an image with no handle, no view and no memory. */
        s_mem.bind_failures++;
        s_mem.rollbacks++;
        vb_fail("vkBindImageMemory", r);
        vb_mem_give_now(&im->mem);
        vb_image_discard(im);
        return 0;
    }
    vi.image = im->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    r = vkCreateImageView(g.dev, &vi, NULL, &im->view);
    if (r != VK_SUCCESS) {
        vb_fail("vkCreateImageView", r);
        im->view = VK_NULL_HANDLE;
        vb_image_discard(im);
        return 0;
    }
    return 1;
}

/* Forget (and destroy after the submission) every framebuffer using `view`. */
static void vb_fb_forget(VkImageView view)
{
    unsigned i;

    for (i = 0; i < VB_MAX_FBS; i++) {
        VbFb *f = &g.fbs[i];
        if (f->fb && (f->c == view || f->d == view)) {
            VbGarbage e;
            memset(&e, 0, sizeof e);
            e.fb = f->fb;
            vb_discard(e);
            memset(f, 0, sizeof *f);
        }
    }
}

static VkFramebuffer vb_framebuffer(const VbTarget *t, const VbDepth *d)
{
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkImageView att[2];
    VkImageView dv = d ? d->im.view : VK_NULL_HANDLE;
    VbFb *f;
    unsigned i;
    VkResult r;

    for (i = 0; i < VB_MAX_FBS; i++)
        if (g.fbs[i].fb && g.fbs[i].c == t->im.view && g.fbs[i].d == dv)
            return g.fbs[i].fb;
    f = &g.fbs[g.next_fb++ % VB_MAX_FBS];
    if (f->fb) {
        VbGarbage e;
        memset(&e, 0, sizeof e);
        e.fb = f->fb;
        vb_discard(e);
    }
    memset(f, 0, sizeof *f);
    att[0] = t->im.view;
    att[1] = dv;
    fi.renderPass = g.rp[d ? 1 : 0];
    fi.attachmentCount = d ? 2u : 1u;
    fi.pAttachments = att;
    fi.width = t->w;
    fi.height = t->h;
    fi.layers = 1;
    r = vkCreateFramebuffer(g.dev, &fi, NULL, &f->fb);
    if (r != VK_SUCCESS) {
        vb_fail("vkCreateFramebuffer", r);
        f->fb = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    f->c = t->im.view;
    f->d = dv;
    return f->fb;
}

/* Render into `t` (and `d`), keeping the open pass when it already is that. */
static int vb_begin_pass(VbTarget *t, VbDepth *d)
{
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkFramebuffer fb;
    VkCommandBuffer cmd = vb_cmd();

    if (!g.ok)                          /* a fence wait, a submit or a begin failed: record nothing */
        return 0;
    if (g.pass_open && g.pass_t == t && g.pass_d == d)
        return 1;                       /* this target's pass is still open, in pass_cmd */
    vb_end_pass();
    fb = vb_framebuffer(t, d);
    if (!fb)
        return 0;
    bi.renderPass = g.rp[d ? 1 : 0];
    bi.framebuffer = fb;
    bi.renderArea.extent.width = t->w;
    bi.renderArea.extent.height = t->h;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    g.pass_cmd = cmd;                   /* vb_end_pass ends exactly this buffer */
    g.pass_open = 1;
    g.pass_t = t;
    g.pass_d = d;
    return 1;
}

/* ── Render targets ──────────────────────────────────────────────────── */

static void vb_target_release(VbTarget *t)
{
    if (g.pass_t == t)
        vb_end_pass();
    vb_fb_forget(t->im.view);
    vb_image_discard(&t->im);
    vb_image_discard(&t->copy);
    memset(t, 0, sizeof *t);
}

/* The bytes one target's dirty rectangle occupies in the staging buffer, and the row stride
 * it is copied or packed with. Both are multiples of 4 -- the 16-bit stride is rounded up, so
 * an odd width carries at most two bytes of slack that are never published -- which keeps
 * every slice offset 4-byte aligned and every buffer copy's size a multiple of 4. */
static VkDeviceSize vb_readback_slice(const VbTarget *t, uint32_t *row_stride)
{
    uint32_t rw = t->dx1 - t->dx0;

    *row_stride = t->bpp == 2 ? ((rw * 2 + 3) & ~3u) : rw * 4;
    return (VkDeviceSize)*row_stride * (t->dy1 - t->dy0);
}

/* Room for `bytes` in both publication staging buffers. Grow-only, and never past
 * VB_READBACK_MAX; the pair is replaced together, so a failure leaves the old two in place.
 * The replaced buffers go through vb_discard, which holds them until nothing is being
 * recorded and no submission is in flight: a submission the previous batch made may still be
 * reading them. Returns 0 if the request is over the cap or the pair could not be made; the
 * caller then stages nothing and the target keeps its dirty flag. */
static int vb_readback_reserve(VkDeviceSize bytes)
{
    VkBuffer hb = VK_NULL_HANDLE, db = VK_NULL_HANDLE;
    VbAlloc hm, dm;
    uint8_t *hp = NULL;
    VkDeviceSize cap;
    VbGarbage e;

    if (bytes <= g.rb_cap)
        return 1;
    if (bytes > VB_READBACK_MAX)
        return 0;
    cap = g.rb_cap ? g.rb_cap : (1u << 20);
    while (cap < bytes)
        cap *= 2;
    if (cap > VB_READBACK_MAX)
        cap = VB_READBACK_MAX;
    memset(&hm, 0, sizeof hm);
    hm.pool = hm.block = -1;
    memset(&dm, 0, sizeof dm);
    dm.pool = dm.block = -1;
    if (!vb_host_buffer(cap, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &hb, &hm, &hp)
        || !vb_device_buffer(cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                  | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &db, &dm)) {
        /* NOTHING referenced: both buffers were made in THIS call and never published, so no
         * recorded command and no submission can name either of them. They are destroyed and
         * their ranges released at once (vb_garbage_free_now), so a burst of failed reserves
         * can never fill the bounded park list. */
        memset(&e, 0, sizeof e);
        e.buf = hb; e.mem = hm;
        if (e.buf || e.mem.mem)
            vb_garbage_free_now(&e);
        memset(&e, 0, sizeof e);
        e.buf = db; e.mem = dm;
        if (e.buf || e.mem.mem)
            vb_garbage_free_now(&e);
        return 0;
    }
    /* The replaced pair: possibly referenced, because a submission the PREVIOUS batch made may
     * still be reading these two, so they stay on the conservative path (held until the ring
     * is idle), unlike this call's own failed pair above. */
    memset(&e, 0, sizeof e);
    e.buf = g.rb_buf; e.mem = g.rb_mem;
    if (e.buf || e.mem.mem)
        vb_discard(e);
    memset(&e, 0, sizeof e);
    e.buf = g.pack_buf; e.mem = g.pack_mem;
    if (e.buf || e.mem.mem)
        vb_discard(e);
    g.rb_buf = hb;
    g.rb_mem = hm;
    g.rb_ptr = hp;
    g.pack_buf = db;
    g.pack_mem = dm;
    g.rb_cap = cap;
    return 1;
}

/* Write the unflushed rectangles of `n` targets into guest memory in the guest format.
 *
 * The dirty targets are published in bounded batches: one batch is one submission carrying
 * every slice that fits VB_READBACK_MAX (a 4-byte target copied straight out of its image, a
 * 16-bit target packed to 5:6:5 by cs_pack into the device-local staging buffer and moved to
 * the host-visible one by a buffer copy of its own), one fence wait, and then a memcpy per
 * row into guest memory. No per-pixel work happens on the host.
 *
 * A target that cannot be staged -- one whose slice alone is over the cap, a batch whose
 * staging buffers could not be made, or a batch whose pack descriptor set could not be
 * allocated -- keeps its dirty flag and is retried by the next flush rather than published
 * wrong or silently dropped.
 *
 * The outcome is the return value: 1 iff no target in `ts` is left dirty when it returns
 * (every dirty target was published, or none was dirty), 0 as soon as one dirty target could
 * not be published -- a skipped batch, a slice over the cap, or a fence wait that failed. The
 * failure behaviour is otherwise exactly as it was: an abandoned batch submits nothing,
 * publishes nothing, latches nothing and leaves every publication counter unchanged, which is
 * what the readback fault gate asserts. This function does NOT touch the "left unpublished"
 * counter: that belongs to vb_targets_flush_recover, the boundary that decided the target
 * could not be published at all. */
static int vb_targets_flush(VbTarget **ts, unsigned n)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    uint64_t t0 = vb_now_ns(), done0 = g.st.readbacks;
    unsigned i;

    if (!g.ok)                          /* a fence wait already failed: record nothing more, and
                                         * nothing can be published now */
        return 0;
    for (i = 0; i < n; ) {
        VkDeviceSize slice[VB_MAX_TARGETS], total = 0;
        uint32_t stride[VB_MAX_TARGETS];
        int stage[VB_MAX_TARGETS];
        int batch_ok = 1;               /* 0: this batch recorded nothing usable, so it must
                                         * not be submitted -- or published -- at all */
        unsigned j = i, k;

        /* One bounded group: the dirty targets whose slices still fit. A target whose slice
         * is over the cap on its own can never be staged; it is stepped over -- and left
         * dirty -- so the targets after it still publish. */
        while (j < n) {
            VbTarget *t = ts[j];
            VkDeviceSize sz;
            uint32_t st;

            stage[j] = 0;
            if (t->dirty && t->dx1 > t->dx0 && t->dy1 > t->dy0) {
                sz = vb_readback_slice(t, &st);
                if (sz > VB_READBACK_MAX) {
                    j++;
                    continue;
                }
                if (total + sz > VB_READBACK_MAX)
                    break;              /* the next batch starts at this target */
                stage[j] = 1;
                slice[j] = total;
                stride[j] = st;
                total += sz;
            }
            j++;
        }
        if (!total) {                   /* nothing stageable in this group */
            i = j;
            continue;
        }
        if (!vb_readback_reserve(total) || !g.ok) {
            i = j;                      /* retried by the next flush, still dirty */
            continue;
        }
        /* One recording for the whole batch: every copy and its destination offset, and every
         * pack dispatch with the buffer copy that reads it, are recorded before anything is
         * submitted. */
        vb_transfer_cmd();              /* end any open pass; latched/dead-safe */
        if (!g.ok)
            break;
        for (k = i; k < j; k++) {
            VbTarget *t = ts[k];
            uint32_t rw, rh;

            if (!stage[k])
                continue;
            /* vb_cmd(): no pass is open here, and a vb_reserve below may have moved the
             * recording into the next slot, which this follows. */
            rw = t->dx1 - t->dx0;
            rh = t->dy1 - t->dy0;
            if (t->bpp == 4) {
                VkBufferImageCopy bc;

                memset(&bc, 0, sizeof bc);
                bc.bufferOffset = slice[k];
                bc.bufferRowLength = rw;    /* tight rows: the slice is exactly the rectangle */
                bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                bc.imageSubresource.layerCount = 1;
                bc.imageOffset.x = (int32_t)t->dx0;
                bc.imageOffset.y = (int32_t)t->dy0;
                bc.imageExtent.width = rw;
                bc.imageExtent.height = rh;
                bc.imageExtent.depth = 1;
                vkCmdCopyImageToBuffer(vb_cmd(), t->im.img, VK_IMAGE_LAYOUT_GENERAL, g.rb_buf, 1,
                                       &bc);
                continue;
            }
            /* 16-bit: pack it on the GPU. vb_reserve(0, 1) first, so the set this dispatch
             * needs is inside the slot's pool budget and the dispatch and the buffer copy
             * that reads it stay in one slot; the reserve can move the slot, and can fail on
             * that move's fence wait, so g.ok is re-checked and the command buffer taken
             * again after it. */
            vb_reserve(0, 1);
            if (!g.ok)
                break;
            {
                VbPackPush pc;
                VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                VkDescriptorBufferInfo bi;
                VkDescriptorImageInfo ii;
                VkWriteDescriptorSet wr[2];
                VkDescriptorSet set;
                VkCommandBuffer cmd = vb_cmd();
                VkResult r;

                dai.descriptorPool = g.slots[g.cur].dpool;
                dai.descriptorSetCount = 1;
                dai.pSetLayouts = &g.dsl_pack;
                r = vkAllocateDescriptorSets(g.dev, &dai, &set);
                if (r != VK_SUCCESS) {
                    /* Only this target's dispatch is missing, but a batch is published as a
                     * whole: nothing in this batch may be submitted or published, or every
                     * staged target after this one would be marked clean from bytes that
                     * were never written. They all keep `dirty` and are retried. Not a
                     * device loss: a transient pool failure must not stop the back end. */
                    vb_fail("vkAllocateDescriptorSets", r);
                    batch_ok = 0;
                    break;
                }
                g.slots[g.cur].sets_used++;
                memset(&ii, 0, sizeof ii);
                ii.imageView = t->im.view;      /* the target's own view, in GENERAL */
                ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                memset(&bi, 0, sizeof bi);
                bi.buffer = g.pack_buf;
                bi.offset = 0;
                bi.range = VK_WHOLE_SIZE;
                memset(wr, 0, sizeof wr);
                wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                wr[0].dstSet = set;
                wr[0].dstBinding = 0;
                wr[0].descriptorCount = 1;
                wr[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                wr[0].pImageInfo = &ii;
                wr[1] = wr[0];                  /* the pack buffer is bound whole at 0, so no
                                                 * dynamic offset is involved */
                wr[1].dstBinding = 1;
                wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                wr[1].pImageInfo = NULL;
                wr[1].pBufferInfo = &bi;
                vkUpdateDescriptorSets(g.dev, 2, wr, 0, NULL);
                pc.rect[0] = t->dx0;
                pc.rect[1] = t->dy0;
                pc.rect[2] = rw;
                pc.rect[3] = rh;
                pc.dst[0] = (uint32_t)slice[k];
                pc.dst[1] = stride[k];
                pc.dst[2] = pc.dst[3] = 0;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipe_pack);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout_pack, 0, 1,
                                        &set, 0, NULL);
                vkCmdPushConstants(cmd, g.layout_pack, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc,
                                   &pc);
                vkCmdDispatch(cmd, (rw + 15u) / 16u, (rh + 7u) / 8u, 1);
                /* The pack kernel's writes of g.pack_buf at this target's slice before the
                 * vkCmdCopyBuffer that reads exactly that slice. Two dispatches inside one
                 * batch never overlap: a group's slice offsets accumulate strictly. A later
                 * batch reusing offset 0 is separated by that batch's own submit plus
                 * vb_drain, or, for an abandoned batch that continues into the next group, by
                 * the abandonment barrier in the !batch_ok branch, which is the only thing
                 * ordering the pack buffer's reuse across that group boundary. */
                vb_dep(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                {
                    VkBufferCopy region;

                    memset(&region, 0, sizeof region);
                    region.srcOffset = slice[k];
                    region.dstOffset = slice[k];
                    region.size = (VkDeviceSize)stride[k] * rh;
                    vkCmdCopyBuffer(cmd, g.pack_buf, g.rb_buf, 1, &region);
                }
            }
        }
        if (!g.ok)
            break;
        if (!batch_ok) {
            /* A pack set could not be made, so the batch's copies are incomplete: submit and
             * publish nothing. Every staged target of this group keeps its dirty flag and is
             * retried by the next flush. What was recorded so far stays in the recording slot
             * and is submitted with the next submission the back end makes; the host never
             * reads the staging buffer for it. Order it all the same: the next group's slices
             * start at offset 0 again, so its copies of the 4-byte targets -- and the pack
             * kernel's writes of its 16-bit ones -- write the same staging bytes (the
             * host-visible slice buffer and the device-local pack buffer) as this group's
             * recorded copies did, and the HOST_READ barrier that publishes that next group
             * comes after both writes and so cannot order them against each other. */
            vb_barrier();
            i = j;
            continue;
        }
        /* Order the batch's device writes before the host reads them, whatever wrote them:
         * the pack kernel's shader writes and the copy engine's writes alike. */
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        /* vb_cmd(), not the slot directly: a fence wait above may have found the device lost,
         * in which case this barrier belongs in the dead buffer, a no-op. */
        vkCmdPipelineBarrier(vb_cmd(),
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        /* The coherence boundary: the batch's commands are in the newest submission, and the
         * host must not read the staging buffer until every submission it has made has
         * completed. */
        {
            uint64_t w0 = vb_now_ns();
            vb_slot_submit();
            vb_drain();
            g.st.readback_wait_ns += vb_now_ns() - w0;
        }
        if (!g.ok)
            break;
        /* Publish. The coherence boundary is the range rule: the host-visible staging buffer's
         * own range -- its offset in its block, its size, never a neighbouring range of the
         * same block -- widened to whole nonCoherentAtomSize atoms when the type is not
         * coherent (vb_mem_invalidate). A range that needs it and cannot get it publishes
         * nothing: the batch's targets stay dirty and are retried, any earlier batch having
         * published already, exactly like a batch that could not be staged at all. */
        if (!vb_mem_invalidate(&g.rb_mem, 0, g.rb_mem.size))
            break;
        g.st.readback_batches++;        /* the batch will publish: one submission, one wait */
        {
            uint64_t p0 = vb_now_ns();

            for (k = i; k < j; k++) {
                VbTarget *t = ts[k];
                uint32_t rw = t->dx1 - t->dx0, rows = t->dy1 - t->dy0;
                uint8_t *dst;
                uint32_t row;

                if (!stage[k])
                    continue;
                dst = vb_guest(t->va);
                for (row = 0; row < rows; row++) {
                    memcpy(dst + (size_t)(t->dy0 + row) * t->pitch + (size_t)t->dx0 * t->bpp,
                           g.rb_ptr + slice[k] + (VkDeviceSize)row * stride[k],
                           (size_t)rw * t->bpp);
                }
                t->dirty = 0;
                g.st.readbacks++;
                g.st.readback_bytes += (uint64_t)rw * t->bpp * rows;
                /* Counted here, not while the batch recorded its commands: these say what was
                 * published, so a batch that never reached its submit leaves them unchanged
                 * and leaves its targets dirty. */
                if (t->bpp == 2) {
                    g.st.readback_packs++;
                    g.st.readback_pixels += (uint64_t)rw * rows;
                } else {
                    g.st.readback_copies++;
                }
            }
            g.st.readback_publish_ns += vb_now_ns() - p0;
        }
        i = j;
    }
    /* Counted only when this flush published something, as it always was: a flush that
     * staged nothing is not readback time. */
    if (g.st.readbacks != done0)
        g.st.readback_ns += vb_now_ns() - t0;
    /* The outcome: a target left dirty is a target this flush could not publish. */
    for (i = 0; i < n; i++)
        if (ts[i]->dirty)
            return 0;
    return 1;
}

/* ── The coherence stop and its bounded retry ────────────────────────────
 *
 * A frame's render targets are published into guest memory by the flush above, and a target
 * whose newer pixels could not be published must never be overwritten by older guest bytes
 * afterwards. The three functions below are what makes that a property of the back end rather
 * than of every caller: a failed publication is retried exactly once, and a publication that
 * fails twice stops the back end, which then publishes nothing more: guest memory keeps the
 * last bytes published for each target rectangle, and the targets never published keep the
 * newer pixels no CPU path may overwrite. A flush publishes batch by batch, so an earlier
 * batch may have published before a later one fails: the stop is per target, not a whole-frame
 * transaction. */

/* The coherence stop. The back end cannot publish a frame, so it stops publishing: a stopped
 * back end leaves guest memory holding the last bytes published for each target rectangle, and
 * the executor's CPU path must not write guest framebuffer memory on top of them
 * (nv2a_backend.h's `stopped`). An earlier batch of the same flush may already have published,
 * so the stop preserves the targets that were never published and is not whole-frame
 * atomicity. Idempotent:
 * once the latch is set the line is not printed again and `g.fails` is not touched, so a
 * second boundary reaching the same state says nothing new. Nothing is freed, reset, submitted
 * or destroyed here -- the ring and every live target stay exactly as they are. */
static void vb_stop(const char *why)
{
    if (g.failed)
        return;
    fprintf(stderr, "[GPUVK] %s; the Vulkan back end stops publishing; guest memory keeps"
                    " the last bytes published for each target rectangle\n", why);
    fflush(stderr);
    g.ok = 0;
    g.failed = 1;
    /* The stop's own state, once: the periodic [GPUVK] line is printed on the way out of a
     * SUCCESSFUL flip, and every flip after this one returns at `if (!g.ok)`, so without this
     * the `readback_incomplete` this very stop just counted would never reach a report line.
     * No counter's meaning changes -- this only reads them, in a state where nothing can be
     * submitted or published. */
    vb_stats(1);
}

/* Bounded recovery for a failed publication: refresh the ring, then report whether the back
 * end can still publish. Called only on a failure path. Submits the abandoned batch's harmless
 * partial recording (staging writes only, never guest memory) and resets every slot's
 * descriptor pool and stream after its fence, so the next attempt records into a slot the GPU
 * is done with. Returns 0 once the back end is stopped: a failed wait, submit or begin has
 * already been latched and nothing more may be recorded. */
static int vb_readback_recover(void)
{
    if (!g.ok)
        return 0;
    vb_slot_submit();
    vb_drain();
    return g.ok;
}

/* The coherence boundary's entry point. Publishes the dirty targets of `ts`; on a failure it
 * recovers the ring and retries EXACTLY ONCE. Nothing is ever published twice: an abandoned
 * batch publishes nothing, so the retry is the only publication for those targets. A target
 * still dirty after the retry is counted as left unpublished (`readback_incomplete`) and the
 * caller must stop the back end. */
static int vb_targets_flush_recover(VbTarget **ts, unsigned n)
{
    unsigned i;

    if (vb_targets_flush(ts, n))
        return 1;                       /* published */
    if (vb_readback_recover() && vb_targets_flush(ts, n))
        return 1;                       /* ONE bounded retry published it */
    for (i = 0; i < n; i++)
        if (ts[i]->dirty)
            g.st.readback_incomplete++;
    return 0;
}

/* Bring the GPU copy up to date with guest memory if a flip made it stale. */
static void vb_target_load(VbTarget *t)
{
    const uint8_t *src;
    VkBufferImageCopy bc;
    VkBuffer buf;
    VkDeviceSize off;
    uint32_t *out;
    uint32_t x, y;

    if (!t->stale)
        return;
    /* Move to a fresh slot when this load would not fit what the current one has left, so a
     * load lands in the stream instead of allocating a buffer of its own: nothing written
     * earlier in this slot is waiting for a command, and the load's copy is recorded after
     * this, in whichever slot the reserve moved to. */
    vb_reserve((VkDeviceSize)t->w * t->h * 4, 0);
    if (!g.ok)                          /* the reserve's advance found the wait failed: the
                                         * slot it left us in is still the GPU's, so no slice
                                         * of guest pixels may be written into its stream */
        return;
    out = (uint32_t *)vb_stream((VkDeviceSize)t->w * t->h * 4, &buf, &off);
    if (!out)
        return;
    src = vb_guest(t->va);
    for (y = 0; y < t->h; y++) {
        const uint8_t *row = src + (size_t)y * t->pitch;
        uint32_t *o = out + (size_t)y * t->w;
        if (t->bpp == 4) {
            /* A8R8G8B8 in memory is B8G8R8A8 byte for byte. */
            memcpy(o, row, (size_t)t->w * 4);
        } else {
            /* Expanded by shifting, as the CPU blender reads 5:6:5, so a round trip is exact. */
            const uint16_t *r16 = (const uint16_t *)row;
            for (x = 0; x < t->w; x++) {
                uint32_t v = r16[x];
                o[x] = 0xFF000000u | ((v & 0xF800u) << 8) | ((v & 0x07E0u) << 5)
                     | ((v & 0x001Fu) << 3);
            }
        }
    }
    memset(&bc, 0, sizeof bc);
    bc.bufferOffset = off;
    bc.bufferRowLength = t->w;
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = t->w;
    bc.imageExtent.height = t->h;
    bc.imageExtent.depth = 1;
    if (!g.ok)                          /* a fence wait failed above: record nothing more */
        return;
    vkCmdCopyBufferToImage(vb_transfer_cmd(), buf, t->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    /* The load's transfer write before every later access to that target image. The set
     * also covers the clear-vs-load write-after-write when vb_target_create's clear and this
     * load share one submission. */
    vb_dep(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VB_STAGE_IMAGE_DST,
           VB_ACCESS_IMAGE_DST);
    t->stale = 0;
    g.st.rt_loads++;
}

static int vb_target_create(VbTarget *t, uint32_t va, uint32_t pitch, uint32_t bpp,
                            uint32_t w, uint32_t h)
{
    VkClearColorValue zero;
    VkImageSubresourceRange sr;

    if (!vb_image_create(&t->im, w, h, VK_FORMAT_B8G8R8A8_UNORM,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                         | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT)) {
        memset(t, 0, sizeof *t);
        return 0;
    }
    /* Zero, as a fresh D3D11 texture reads, so a first clear that skips the load
     * leaves the same alpha behind. */
    vb_transfer_cmd();
    if (!g.ok)                          /* a fence wait failed above: record nothing more */
        return 0;
    /* UNDEFINED contents are discarded; the clear that follows is this image's first write. */
    vb_image_dep(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    memset(&zero, 0, sizeof zero);
    memset(&sr, 0, sizeof sr);
    sr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sr.levelCount = 1;
    sr.layerCount = 1;
    vkCmdClearColorImage(g.slots[g.cur].cmd, t->im.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &sr);
    /* The clear's transfer write before every later access to that target image, including the
     * load that follows it in this same submission when vb_target_for left the target stale: a
     * write-after-write that the destination scope must keep covering. */
    vb_dep(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VB_STAGE_IMAGE_DST,
           VB_ACCESS_IMAGE_DST);
    t->used = 1;
    t->va = va;
    t->pitch = pitch;
    t->bpp = bpp;
    t->w = w;
    t->h = h;
    t->stale = 1;
    return 1;
}

/* The cached target for a surface, created or grown as needed; NULL if unusable.
 * The backing spans whole rows (pitch / pixel size) from row 0 to the clip's
 * bottom, because vertices are in surface pixels from (0,0). */
static VbTarget *vb_target_for(const Nv2aSurface *s)
{
    uint32_t bpp = s->bytes_per_pixel, w, h, i;
    VbTarget *t = NULL, *lru = NULL;

    if (!g.ok)                          /* the back end is stopped: nothing new may be made, and
                                         * no target may be released or reshaped */
        return NULL;
    if ((bpp != 2 && bpp != 4) || !s->color_va || !s->pitch || !s->width || !s->height)
        return NULL;
    w = s->pitch / bpp;
    h = s->clip_y + s->height;
    if (!w || w > VB_MAX_DIM || h > VB_MAX_DIM)
        return NULL;
    for (i = 0; i < VB_MAX_TARGETS; i++) {
        VbTarget *c = &g.targets[i];
        if (c->used && c->va == s->color_va) {
            t = c;
            break;
        }
        if (!c->used) {
            if (!lru || lru->used)
                lru = c;
        } else if (!lru || (lru->used && c->last_use < lru->last_use)) {
            lru = c;
        }
    }
    if (t && t->pitch == s->pitch && t->bpp == bpp && t->h >= h) {
        t->last_use = ++g.use_clock;
        return t;
    }
    if (t) {
        /* Same address, new shape: keep the taller backing, rebuilt from guest memory. */
        if (t->pitch == s->pitch && t->bpp == bpp && t->h > h)
            h = t->h;
        /* The target's newer image must be published BEFORE it is released or reshaped:
         * a release here would drop the only copy of pixels guest memory never got. */
        if (!vb_targets_flush_recover(&t, 1)) {
            vb_stop("a target replacement could not publish its target");
            return NULL;
        }
        vb_target_release(t);
    } else {
        t = lru;
        if (t->used) {
            /* Same for an LRU eviction: the victim's unpublished image must land first. */
            if (!vb_targets_flush_recover(&t, 1)) {
                vb_stop("a target replacement could not publish its target");
                return NULL;
            }
            vb_target_release(t);
        }
    }
    /* A flush publishes guest memory and may have found the fence wait failed: nothing is
     * recorded from here on (vb_target_create's own barrier and clear included). */
    if (!g.ok)
        return NULL;
    if (!vb_target_create(t, s->color_va, s->pitch, bpp, w, h))
        return NULL;
    t->last_use = ++g.use_clock;
    return t;
}

static void vb_target_mark(VbTarget *t, const VkRect2D *r)
{
    uint32_t x0 = (uint32_t)r->offset.x, y0 = (uint32_t)r->offset.y;
    uint32_t x1 = x0 + r->extent.width, y1 = y0 + r->extent.height;

    if (!t->dirty) {
        t->dirty = 1;
        t->dx0 = x0;
        t->dy0 = y0;
        t->dx1 = x1;
        t->dy1 = y1;
        return;
    }
    if (x0 < t->dx0) t->dx0 = x0;
    if (y0 < t->dy0) t->dy0 = y0;
    if (x1 > t->dx1) t->dx1 = x1;
    if (y1 > t->dy1) t->dy1 = y1;
}

/* ── Depth buffers ───────────────────────────────────────────────────── */

static void vb_depth_release(VbDepth *d)
{
    if (g.pass_d == d)
        vb_end_pass();
    vb_fb_forget(d->im.view);
    vb_image_discard(&d->im);
    memset(d, 0, sizeof *d);
}

/* A framebuffer needs the depth image the size of the target, so the key includes it. */
static VbDepth *vb_depth_for(uint32_t va, uint32_t w, uint32_t h)
{
    VbDepth *d = NULL, *lru = NULL;
    VkClearDepthStencilValue far0;
    VkImageSubresourceRange sr;
    unsigned i;

    for (i = 0; i < VB_MAX_DEPTHS; i++) {
        VbDepth *c = &g.depths[i];
        if (c->used && c->va == va && c->w == w && c->h == h) {
            c->last_use = ++g.use_clock;
            return c;
        }
        if (c->used && c->va == va && !d)
            d = c;                          /* same zeta, other size: replaced */
        if (!c->used) {
            if (!lru || lru->used)
                lru = c;
        } else if (!lru || (lru->used && c->last_use < lru->last_use)) {
            lru = c;
        }
    }
    if (!d)
        d = lru;
    if (d->used)
        vb_depth_release(d);
    if (!vb_image_create(&d->im, w, h, g.depth_format,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                         | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        memset(d, 0, sizeof *d);
        return NULL;
    }
    /* Far and stencil 0, as the CPU path's fresh buffers are, until the title clears it. */
    vb_transfer_cmd();
    if (!g.ok)                          /* a fence wait failed above: record nothing more */
        return NULL;
    /* As in a target create: the undefined contents are discarded before the clear's write. */
    vb_image_dep(d->im.img, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
    far0.depth = 1.0f;
    far0.stencil = 0;
    memset(&sr, 0, sizeof sr);
    sr.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    sr.levelCount = 1;
    sr.layerCount = 1;
    vkCmdClearDepthStencilImage(g.slots[g.cur].cmd, d->im.img, VK_IMAGE_LAYOUT_GENERAL, &far0, 1, &sr);
    /* The clear's transfer write before every later depth test and stencil op on that image. */
    vb_dep(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VB_STAGE_DEPTH,
           VB_ACCESS_DEPTH);
    d->used = 1;
    d->va = va;
    d->w = w;
    d->h = h;
    d->last_use = ++g.use_clock;
    return d;
}

/* What the title's z is measured in. XDK D3D sets SET_CLIP_MAX to the depth
 * format's full scale (2^24-1 for Z24S8, 2^16-1 for Z16), which is also the
 * scale its viewport transform gives z; Z24 is assumed when it says nothing. */
static float vb_z_scale(const Nv2aRenderState *rs)
{
    if (rs->depth_max > 65535.5f)
        return 16777215.0f;
    if (rs->depth_max > 1.5f)
        return 65535.0f;
    return 16777215.0f;
}

/* ── Textures ────────────────────────────────────────────────────────── */

/* Upload a texture into the entry: DXT whose size is a multiple of 4 goes up as BC data
 * where the device samples BC, anything else is decoded; a format the decoder rejects
 * leaves the view 0. */
static void vb_tex_upload(VbTex *e, const Nv2aTexture *tx)
{
    uint32_t block = d3d8_format_dxt_block_bytes(tx->color);
    int native = g.bc && block && tx->width && tx->height && !(tx->width & 3)
              && !(tx->height & 3);
    VkDeviceSize bytes;
    VkBufferImageCopy bc;
    VkBuffer buf;
    VkDeviceSize off;
    VkFormat fmt;
    uint32_t *dec = NULL;
    uint8_t *dst;

    vb_image_discard(&e->im);
    if (native) {
        /* Xbox DXT is linear 4x4 blocks in row-major order, which is the BC layout. */
        fmt = tx->color == 0x0C ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK
            : tx->color == 0x0E ? VK_FORMAT_BC2_UNORM_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
        bytes = (VkDeviceSize)(tx->width / 4) * (tx->height / 4) * block;
    } else {
        dec = vb_scratch((size_t)tx->width * tx->height);
        if (!dec || !nv2a_backend_decode_texture(tx, dec)) {
            g.st.tex_undecodable++;
            return;
        }
        fmt = VK_FORMAT_B8G8R8A8_UNORM;     /* 0xAARRGGBB words, as decoded */
        bytes = (VkDeviceSize)tx->width * tx->height * 4;
    }
    /* Same reason as a target load: take a fresh slot rather than allocate a buffer of its
     * own. The copy below is still recorded in the slot this ends up in. */
    vb_reserve(bytes, 0);
    if (!g.ok)                          /* as in a target load: that reserve's advance may have
                                         * left us in a slot the GPU is still reading */
        return;
    dst = vb_stream(bytes, &buf, &off);
    if (!dst)
        return;
    memcpy(dst, native ? vb_guest(tx->offset) : (const uint8_t *)dec, (size_t)bytes);
    if (!vb_image_create(&e->im, tx->width, tx->height, fmt,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT))
        return;
    vb_transfer_cmd();
    if (!g.ok)                          /* a fence wait failed above: record nothing more */
        return;
    /* Discarded contents before the vkCmdCopyBufferToImage that writes it. */
    vb_image_dep(e->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    memset(&bc, 0, sizeof bc);
    bc.bufferOffset = off;
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = tx->width;
    bc.imageExtent.height = tx->height;
    bc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(g.slots[g.cur].cmd, buf, e->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    /* The upload's transfer write before the draws that sample this texture: the sampled-image
     * bindings are fragment-stage, and the compute bit is a deliberate superset for the sampler
     * class, as at the snapshot barrier in vb_texture. */
    vb_image_dep(e->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_SHADER_READ_BIT);
    g.st.tex_uploads++;
}

/* Is this texture the pixels of a live render target, laid out as we store it? */
static int vb_tex_aliases(const Nv2aTexture *tx, const VbTarget *t)
{
    if (!t->used || t->va != tx->offset || t->pitch != tx->pitch)
        return 0;
    if (t->bpp == 2)
        return tx->color == 0x11;                         /* LIN_R5G6B5 */
    return tx->color == 0x12 || tx->color == 0x1E;        /* LIN_A8R8G8B8 / X8R8G8B8 */
}

/* The view to sample for stage 0, its layout, and the texel-to-normalised scale for it.
 * `out` is the target being drawn into, which must not also be sampled. */
static VkImageView vb_texture(const Nv2aTexture *tx, VbTarget *out, VkImageLayout *layout,
                              float *inv_w, float *inv_h)
{
    uint8_t *mem;
    uint32_t span, i;
    VbTex *e = NULL, *lru = NULL;

    if (!tx->width || !tx->height || tx->width > VB_MAX_DIM || tx->height > VB_MAX_DIM)
        return VK_NULL_HANDLE;
    if (!g.ok)                          /* a fence wait already failed: record nothing more */
        return VK_NULL_HANDLE;

    for (i = 0; i < VB_MAX_TARGETS; i++) {
        VbTarget *t = &g.targets[i];
        VkImageCopy ic;
        if (!vb_tex_aliases(tx, t))
            continue;
        vb_target_load(t);
        if (!g.ok)                      /* the load froze: the target's pixels are not in guest
                                         * memory, so nothing may be sampled from it */
            return VK_NULL_HANDLE;
        g.st.rt_aliases++;
        *inv_w = 1.0f / (float)t->w;
        *inv_h = 1.0f / (float)t->h;
        *layout = VK_IMAGE_LAYOUT_GENERAL;
        if (t != out)
            return t->im.view;
        /* Sampling the target being drawn: read a snapshot of it instead. */
        if (!t->copy.view) {
            if (!vb_image_create(&t->copy, t->w, t->h, VK_FORMAT_B8G8R8A8_UNORM,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT))
                return VK_NULL_HANDLE;
            vb_transfer_cmd();
            /* Discarded contents before the vkCmdCopyImage that writes it. */
            vb_image_dep(t->copy.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        memset(&ic, 0, sizeof ic);
        ic.srcSubresource.aspectMask = ic.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ic.srcSubresource.layerCount = ic.dstSubresource.layerCount = 1;
        ic.extent.width = t->w;
        ic.extent.height = t->h;
        ic.extent.depth = 1;
        if (!g.ok)                      /* a fence wait failed above: record nothing more */
            return VK_NULL_HANDLE;
        vkCmdCopyImage(vb_transfer_cmd(), t->im.img, VK_IMAGE_LAYOUT_GENERAL, t->copy.img,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
        /* The snapshot's transfer write before the draws that sample it (the sampled-image
         * bindings are fragment-stage; the compute bit is named because the readback's pack
         * kernel is this back end's one compute consumer, so the scope covers it rather than
         * tracking which stage samples which image), before the next snapshot copy's read of the
         * target image, and before that copy's write of this same snapshot image
         * (write-after-write). */
        vb_dep(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
               | VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT
               | VK_ACCESS_TRANSFER_WRITE_BIT);
        g.st.rt_copies++;
        return t->copy.view;
    }

    span = nv2a_backend_texture_span(tx);
    if (!span)
        return VK_NULL_HANDLE;
    /* Guest bytes that a target has overwritten on the GPU must land first. */
    for (i = 0; i < VB_MAX_TARGETS; i++) {
        VbTarget *t = &g.targets[i];
        uint64_t lo = t->va, hi = (uint64_t)t->va + (uint64_t)t->pitch * t->h;
        if (t->used && t->dirty && lo < (uint64_t)tx->offset + span && tx->offset < hi) {
            /* Every publication boundary: the flush, its one bounded retry, and the stop if
             * even that fails. Decoding now would read stale guest bytes and cache them as
             * the texture, so a failure here returns without decoding anything. */
            if (!vb_targets_flush_recover(&t, 1)) {
                vb_stop("a guest-memory texture could not be published first");
                return VK_NULL_HANDLE;
            }
        }
    }
    if (!g.ok)                          /* one of those flushes found the wait failed */
        return VK_NULL_HANDLE;

    for (i = 0; i < VB_MAX_TEXTURES; i++) {
        VbTex *c = &g.texs[i];
        if (c->used && c->offset == tx->offset && c->color == tx->color
            && c->w == tx->width && c->h == tx->height && c->pitch == tx->pitch) {
            e = c;
            break;
        }
        if (!c->used) {
            if (!lru || lru->used)
                lru = c;
        } else if (!lru || (lru->used && c->last_use < lru->last_use)) {
            lru = c;
        }
    }
    mem = vb_guest(tx->offset);
    if (e) {
        if (e->frame != g.frame) {
            uint64_t h = vb_hash(mem, span);
            if (h != e->hash) {
                vb_tex_upload(e, tx);
                e->hash = h;
            } else {
                g.st.tex_hits++;
            }
            e->frame = g.frame;
        } else {
            g.st.tex_hits++;
        }
    } else {
        e = lru;
        vb_image_discard(&e->im);
        memset(e, 0, sizeof *e);
        e->used = 1;
        e->offset = tx->offset;
        e->color = tx->color;
        e->w = tx->width;
        e->h = tx->height;
        e->pitch = tx->pitch;
        e->span = span;
        e->hash = vb_hash(mem, span);
        e->frame = g.frame;
        vb_tex_upload(e, tx);
    }
    e->last_use = ++g.use_clock;
    *inv_w = 1.0f / (float)tx->width;
    *inv_h = 1.0f / (float)tx->height;
    *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return e->im.view;
}

static uint32_t vb_sampler_mode(uint32_t addr)
{
    return addr == 1 ? 0u : addr == 2 ? 1u : 2u;    /* wrap, mirror, else clamp */
}

/* Is one half of SET_TEXTURE_FILTER (nv2a_regs.h NV097_SET_TEXTURE_FILTER_MIN
 * 0x00FF0000, _MAG 0x0F000000) a linear (tent) filter? 1 is box, 2 tent; the
 * MIN mip modes 3-6 (BOX/TENT_NEARESTLOD, BOX/TENT_TENT_LOD) keep their in-level
 * choice in the low bit, since only level 0 is uploaded. Anything else is linear. */
static int vb_filter_linear(uint32_t mode, int is_min)
{
    if (mode == 1 || (is_min && (mode == 3 || mode == 5)))
        return 0;
    return 1;
}

/* The sampler for a texture's filter and address modes, created on first use. */
static VkSampler vb_sampler(const Nv2aTexture *tx)
{
    static const VkSamplerAddressMode m[3] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
    };
    int min_lin = vb_filter_linear((tx->filter >> 16) & 0xFF, 1);
    int mag_lin = vb_filter_linear((tx->filter >> 24) & 0xF, 0);
    uint32_t fi = (uint32_t)min_lin * 2 + (uint32_t)mag_lin;
    uint32_t ai = vb_sampler_mode(tx->addr_u) * 3 + vb_sampler_mode(tx->addr_v);

    if (!g.samp[fi][ai]) {
        VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        VkResult r;
        si.magFilter = mag_lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.minFilter = min_lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = m[ai / 3];
        si.addressModeV = m[ai % 3];
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        r = vkCreateSampler(g.dev, &si, NULL, &g.samp[fi][ai]);
        if (r != VK_SUCCESS) {
            vb_fail("vkCreateSampler", r);
            g.samp[fi][ai] = VK_NULL_HANDLE;
        }
    }
    return g.samp[fi][ai];
}

/* ── Drawing ─────────────────────────────────────────────────────────── */

typedef struct {
    VbTarget *t;
    VbDepth *d;
    VkImageView view;                   /* 0: untextured */
    VkImageLayout view_layout;
    VkSampler samp;
    VkImageView views[4];               /* combiner stages; 0: g.white is bound */
    VkImageLayout view_layouts[4];
    VkSampler samps[4];                 /* 0: g.samp[0][8] is bound */
    VbRcConstants rck;                  /* the combiner registers, uploaded per draw */
    VbPipeKey key;
    float blend_factor[4];
    uint32_t stencil_ref, stencil_read, stencil_write;
    VbConstants k;
    VkRect2D scissor;
    /* Diagnostic only (RECOMP_PB_COVERAGE): the refusal nv2a_vk_rc_refusal()
     * reported when rc was requested but unsupported. Plain fields, NOT part
     * of VbPipeKey: a pipeline is never selected from them, and with the gate
     * off they are simply never read. */
    uint32_t rc_reason, rc_stage, rc_mode;
} VbDraw;

static int vb_vertex_ok(const Nv2aVertex *v)
{
    return isfinite(v->x) && isfinite(v->y) && isfinite(v->z) && isfinite(v->rhw)
        && v->rhw != 0.0f;
}

/* Upload the usable triangles of `v` (with `x` on the combiner path) and draw
 * them; returns how many vertices were drawn. */
static uint32_t vb_submit(const VbDraw *dr, const Nv2aVertex *v, const Nv2aVertexExtra *x,
                          uint32_t n)
{
    const int rc = dr->key.rc != 0;
    const uint32_t stride = rc ? (uint32_t)sizeof(VbRcVertex) : (uint32_t)sizeof(Nv2aVertex);
    const uint32_t cap = VB_DRAW_CAP_BYTES / stride;
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorImageInfo ii[8];
    VkDescriptorBufferInfo ub;
    VkWriteDescriptorSet wr[9];
    VkDescriptorSet set;
    VkPipeline pipe;
    VkBuffer buf, ubuf = VK_NULL_HANDLE;
    VkDeviceSize off, uoff = 0, bytes;
    VkViewport vp;
    uint8_t *dst;
    uint32_t i, j, out = 0, nw, doff;
    VkResult r;

    if (!g.ok)                          /* a fence wait failed during this call's setup */
        return 0;
    if (n > cap)
        n = cap - cap % 3;
    pipe = vb_pipeline(&dr->key, NULL);
    if (!pipe)
        return 0;
    /* Vertices, the combiner registers and the descriptor set land in the same
     * submission as the draw: the vertex slice, the alignment pad the UBO slice
     * needs, the UBO and the one set are all reserved before either slice is
     * written, so no submit can fall between them. */
    bytes = (VkDeviceSize)n * stride;
    if (rc)
        bytes = ((bytes + VB_STREAM_ALIGN - 1) & ~(VkDeviceSize)(VB_STREAM_ALIGN - 1))
              + sizeof(VbRcConstants);
    vb_reserve(bytes, 1);
    if (!g.ok)
        return 0;
    dst = vb_stream((VkDeviceSize)n * stride, &buf, &off);
    if (!dst)
        return 0;
    for (i = 0; i + 3 <= n; i += 3) {
        if (!(vb_vertex_ok(&v[i]) && vb_vertex_ok(&v[i + 1]) && vb_vertex_ok(&v[i + 2]))) {
            g.st.dropped_tris++;
            continue;
        }
        if (!rc) {
            memcpy((Nv2aVertex *)dst + out, v + i, 3 * sizeof *v);
        } else {
            for (j = i; j < i + 3; j++) {
                VbRcVertex *o = (VbRcVertex *)dst + out + (j - i);
                o->x = v[j].x;
                o->y = v[j].y;
                o->z = v[j].z;
                o->rhw = v[j].rhw;
                o->d0 = v[j].diffuse;
                o->d1 = x[j].specular;
                o->fog = x[j].fog;
                memcpy(o->t, x[j].tex, sizeof o->t);
            }
        }
        out += 3;
    }
    if (!out)
        return 0;
    if (rc) {
        /* Reserved above, so this cannot submit: the UBO and the draw that reads
         * it are in the same submission. uoff is 256-aligned, which satisfies
         * minUniformBufferOffsetAlignment on every device (VB_STREAM_ALIGN). */
        uint8_t *u = vb_stream(sizeof(VbRcConstants), &ubuf, &uoff);
        if (!u)
            return 0;
        memcpy(u, &dr->rck, sizeof dr->rck);
    }

    dai.descriptorPool = g.slots[g.cur].dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = rc ? &g.dsl_rc : &g.dsl;
    r = vkAllocateDescriptorSets(g.dev, &dai, &set);
    if (r != VK_SUCCESS) {
        vb_fail("vkAllocateDescriptorSets", r);
        return 0;
    }
    g.slots[g.cur].sets_used++;
    memset(ii, 0, sizeof ii);
    memset(wr, 0, sizeof wr);
    if (!rc) {
        ii[0].imageView = dr->view ? dr->view : g.white.view;
        ii[0].imageLayout = dr->view ? dr->view_layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii[1].sampler = dr->samp ? dr->samp : g.samp[0][8];
        for (i = 0; i < 2; i++) {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = set;
            wr[i].dstBinding = i;
            wr[i].descriptorCount = 1;
            wr[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_SAMPLER
                                     : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            wr[i].pImageInfo = &ii[i];
        }
        nw = 2;
    } else {
        ub.buffer = ubuf;
        ub.offset = 0;
        ub.range = sizeof(VbRcConstants);
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[0].dstSet = set;
        wr[0].dstBinding = 0;
        wr[0].descriptorCount = 1;
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        wr[0].pBufferInfo = &ub;
        for (i = 0; i < 4; i++) {
            ii[i].imageView = dr->views[i] ? dr->views[i] : g.white.view;
            ii[i].imageLayout = dr->views[i] ? dr->view_layouts[i]
                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            wr[1 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[1 + i].dstSet = set;
            wr[1 + i].dstBinding = 1 + i;
            wr[1 + i].descriptorCount = 1;
            wr[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            wr[1 + i].pImageInfo = &ii[i];
            ii[4 + i].sampler = dr->samps[i] ? dr->samps[i] : g.samp[0][8];
            wr[5 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[5 + i].dstSet = set;
            wr[5 + i].dstBinding = 5 + i;
            wr[5 + i].descriptorCount = 1;
            wr[5 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            wr[5 + i].pImageInfo = &ii[4 + i];
        }
        nw = 9;
    }
    vkUpdateDescriptorSets(g.dev, nw, wr, 0, NULL);

    if (!vb_begin_pass(dr->t, dr->d))
        return 0;
    if (g.slots[g.cur].bound_pipe != pipe) {
        vkCmdBindPipeline(g.slots[g.cur].cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        g.slots[g.cur].bound_pipe = pipe;
    }
    doff = (uint32_t)uoff;
    vkCmdBindDescriptorSets(g.slots[g.cur].cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            rc ? g.layout_rc : g.layout, 0, 1, &set,
                            rc ? 1u : 0u, rc ? &doff : NULL);
    vkCmdBindVertexBuffers(g.slots[g.cur].cmd, 0, 1, &buf, &off);
    vkCmdPushConstants(g.slots[g.cur].cmd, rc ? g.layout_rc : g.layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof dr->k, &dr->k);
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = (float)dr->t->w;
    vp.height = (float)dr->t->h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(g.slots[g.cur].cmd, 0, 1, &vp);
    vkCmdSetScissor(g.slots[g.cur].cmd, 0, 1, &dr->scissor);
    vkCmdSetStencilReference(g.slots[g.cur].cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dr->stencil_ref);
    vkCmdSetStencilCompareMask(g.slots[g.cur].cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dr->stencil_read);
    vkCmdSetStencilWriteMask(g.slots[g.cur].cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dr->stencil_write);
    vkCmdSetBlendConstants(g.slots[g.cur].cmd, dr->blend_factor);
    vkCmdDraw(g.slots[g.cur].cmd, out, 1, 0, 0);
    return out;
}

/* The clip rectangle, in target pixels; 0 if it is empty. */
static int vb_scissor(const Nv2aSurface *s, const VbTarget *t, VkRect2D *r)
{
    uint32_t x1 = s->clip_x + s->width, y1 = s->clip_y + s->height;

    if (x1 > t->w) x1 = t->w;
    if (y1 > t->h) y1 = t->h;
    if (s->clip_x >= x1 || s->clip_y >= y1)
        return 0;
    r->offset.x = (int32_t)s->clip_x;
    r->offset.y = (int32_t)s->clip_y;
    r->extent.width = x1 - s->clip_x;
    r->extent.height = y1 - s->clip_y;
    return 1;
}

static int vb_cull_mode(const Nv2aRenderState *rs)
{
    if (!rs->cull_enable) return 0;
    if (rs->cull_face == 0x405) return 1; /* BACK */
    if (rs->cull_face == 0x404) return 2; /* FRONT */
    if (rs->cull_face == 0x408) return 3; /* FRONT_AND_BACK */
    return 0;
}

static uint32_t vb_write_mask(uint32_t color_mask, uint32_t bpp)
{
    uint32_t m = 0;

    if (color_mask & 0x00010000u) m |= VK_COLOR_COMPONENT_R_BIT;
    if (color_mask & 0x00000100u) m |= VK_COLOR_COMPONENT_G_BIT;
    if (color_mask & 0x00000001u) m |= VK_COLOR_COMPONENT_B_BIT;
    /* A 5:6:5 target has no alpha; keeping ours at 1 makes DST_ALPHA read 1, as on the CPU path. */
    if ((color_mask & 0x01000000u) && bpp == 4) m |= VK_COLOR_COMPONENT_A_BIT;
    return m;
}

/* One bit per stage that carries a true cube texture, for nv2a_vk_rc_refusal. */
static uint32_t vb_cube_mask(const Nv2aBatch *b)
{
    uint32_t mask = 0;
    int st;

    for (st = 0; st < 4; st++)
        if (b->textures[st] && b->textures[st]->cube)
            mask |= 1u << st;
    return mask;
}

/* Can the combiner pipeline draw this batch? gb_rc_key's rule: bump-environment and
 * dot-product stage modes (> 5) and true cube maps are not interpreted. The rule
 * itself lives in nv2a_vk_rc_coverage.h so a fixture and this back end share one
 * definition; behavior here is unchanged. */
static int vb_rc_supported(const Nv2aBatch *b)
{
    uint32_t stage, mode;
    return nv2a_vk_rc_refusal(b->combiner->stage_program, vb_cube_mask(b),
                              &stage, &mode) == 0;
}

/* The combiner half of a draw (gb_rc_prepare): the register block and the four stages' textures. */
static void vb_rc_prepare(VbDraw *dr, const Nv2aBatch *b)
{
    const Nv2aCombiner *rc = b->combiner;
    VbRcConstants *k = &dr->rck;
    uint32_t n = rc->control & 0xFF, prog = rc->stage_program & 0xFFFFF, i;
    int st;

    if (n > 8)
        n = 8;
    for (i = 0; i < 8; i++) {
        k->stage[i][0] = rc->color_icw[i];
        k->stage[i][1] = rc->alpha_icw[i];
        k->stage[i][2] = rc->color_ocw[i];
        k->stage[i][3] = rc->alpha_ocw[i];
        nv2a_rc_unpack(rc->factor0[i], k->c0[i]);
        nv2a_rc_unpack(rc->factor1[i], k->c1[i]);
    }
    nv2a_rc_unpack(rc->final_c0, k->fc0);
    nv2a_rc_unpack(rc->final_c1, k->fc1);
    nv2a_rc_unpack(b->fog_color, k->fogc);          /* already a D3DCOLOR (backend_fill_rc) */
    k->ctl[0] = n;
    k->ctl[1] = (rc->control >> 8) & 0x111;          /* mux MSB, per-stage c0, c1 */
    k->ctl[2] = prog;
    k->ctl[3] = b->clip_plane_mode & 0xFFFF;
    k->fin[0] = rc->final0;
    k->fin[1] = rc->final1;
    for (st = 0; st < 4; st++) {
        const Nv2aTexture *tx = b->textures[st];
        uint32_t m = (prog >> (st * 5)) & 0x1F;
        float iw = 1.0f, ih = 1.0f;
        k->ts[st][0] = k->ts[st][1] = 1.0f;
        if (!tx || m < 1 || m > 3)
            continue;
        dr->views[st] = vb_texture(tx, dr->t, &dr->view_layouts[st], &iw, &ih);
        dr->samps[st] = vb_sampler(tx);
        if (!dr->samps[st])
            dr->views[st] = VK_NULL_HANDLE;
        /* Linear formats are addressed in texels, the rest in [0, 1]. */
        if (tx->linear) {
            k->ts[st][0] = iw;
            k->ts[st][1] = ih;
        }
        k->ts[st][2] = dr->views[st] ? 1.0f : 0.0f;
    }
}

/* Set up everything of a draw but its vertices: target, depth, transform and
 * alpha constants, the combiners when `rc` and the batch is interpretable, else
 * stage 0, blend and depth state. Returns 1 ready, 0 no target or an empty clip. */
static int vb_setup(VbDraw *dr, const Nv2aSurface *s, const Nv2aBatch *b, int rc)
{
    const Nv2aRenderState *rs = b->state;
    uint32_t bc = rs->blend_color;
    int cm = vb_cull_mode(rs);
    int rc_ok = 0;
    float zs;

    memset(dr, 0, sizeof *dr);
    dr->key.cull = (uint32_t)(cm % 3);
    dr->key.ccw = dr->key.cull && rs->front_face == 0x901;
    dr->t = vb_target_for(s);
    if (!dr->t || !vb_scissor(s, dr->t, &dr->scissor))
        return 0;
    vb_target_load(dr->t);
    if (rs->zeta_va)
        dr->d = vb_depth_for(rs->zeta_va, dr->t->w, dr->t->h);
    dr->key.has_depth = dr->d != NULL;

    zs = vb_z_scale(rs);
    dr->k.xf[0] = 2.0f / (float)dr->t->w;
    dr->k.xf[1] = 2.0f / (float)dr->t->h;
    dr->k.xf[2] = 1.0f / zs;
    dr->k.tx[2] = (rs->alpha_test_enable && rs->alpha_func >= 0x200 && rs->alpha_func < 0x207)
                ? (float)(rs->alpha_func - 0x200) : -1.0f;
    dr->k.tx[3] = (float)(rs->alpha_ref & 0xFF);
    if (rc) {
        uint32_t rs_stage, rs_mode;
        uint32_t reason = nv2a_vk_rc_refusal(b->combiner->stage_program,
                                             vb_cube_mask(b), &rs_stage, &rs_mode);
        if (!reason) {
            dr->key.rc = 1;
            vb_rc_prepare(dr, b);
            rc_ok = 1;
        } else {
            /* Unsupported rc: remember why for vb_draw's diagnostic record. The
             * setup below is exactly the old `else if (b->texture)` branch. */
            dr->rc_reason = reason;
            dr->rc_stage = rs_stage;
            dr->rc_mode = rs_mode;
        }
    }
    if (!rc_ok && b->texture) {
        dr->view = vb_texture(b->texture, dr->t, &dr->view_layout, &dr->k.tx[0], &dr->k.tx[1]);
        dr->samp = vb_sampler(b->texture);
        if (!dr->samp)
            dr->view = VK_NULL_HANDLE;
    }
    dr->k.xf[3] = dr->view ? 1.0f : 0.0f;

    vb_blend_key(&dr->key, rs->blend_enable ? 1u : 0u, rs->blend_src, rs->blend_dst,
                 rs->blend_eq, vb_write_mask(rs->color_mask, dr->t->bpp));
    if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004
        || rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004) {
        /* As D3D11 (no constant-alpha factor there): every channel of the factor is the alpha. */
        dr->blend_factor[0] = dr->blend_factor[1] = dr->blend_factor[2] =
        dr->blend_factor[3] = (float)(bc >> 24) / 255.0f;
    } else {
        dr->blend_factor[0] = (float)((bc >> 16) & 0xFF) / 255.0f;
        dr->blend_factor[1] = (float)((bc >> 8) & 0xFF) / 255.0f;
        dr->blend_factor[2] = (float)(bc & 0xFF) / 255.0f;
        dr->blend_factor[3] = (float)(bc >> 24) / 255.0f;
    }
    if (!dr->d)
        vb_dss_key(&dr->key, 0, 0x207, 0);
    else if (rs->depth_test_enable)
        vb_dss_key(&dr->key, 1, rs->depth_func, rs->depth_write ? 1u : 0u);
    else if (rs->depth_write)
        vb_dss_key(&dr->key, 1, 0x207, 1);    /* writes need the test on, as in D3D11 */
    else
        vb_dss_key(&dr->key, 0, 0x207, 0);
    if (dr->d && rs->stencil_test_enable) {
        dr->key.s_enable = 1;
        dr->key.s_func = rs->stencil_func;
        dr->key.s_fail = rs->stencil_op_fail;
        dr->key.s_zfail = rs->stencil_op_zfail;
        dr->key.s_zpass = rs->stencil_op_zpass;
        dr->stencil_read = rs->stencil_read_mask & 0xFF;
        dr->stencil_write = rs->stencil_write_mask & 0xFF;
        dr->stencil_ref = rs->stencil_ref & 0xFF;
    }
    return 1;
}

static void vb_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    VbDraw dr;
    uint32_t drawn;
    int rc = b->combiner && b->extra;

    if (!g.ok || b->count < 3)
        return;
    if (vb_cull_mode(b->state) == 3) {
        g.st.cull_all++;
        return;
    }
    vb_maybe_split();
    if (!g.ok)
        return;
    if (!vb_setup(&dr, s, b, rc)) {
        g.st.skipped++;
        return;
    }
    if (rc) {
        if (dr.key.rc) {
            g.st.rc_draws++;
        } else {
            g.st.rc_fallback++;
            /* Diagnostic only: total here is exactly this counter for the
             * process. Off the gate, nothing is recorded. */
            if (nv2a_vk_rc_coverage_enabled())
                nv2a_vk_rc_coverage_record(dr.rc_reason, dr.rc_stage, dr.rc_mode,
                                           b->combiner->stage_program,
                                           vb_cube_mask(b));
        }
    }

    drawn = vb_submit(&dr, b->vertices, dr.key.rc ? b->extra : NULL, b->count);
    if (!drawn)
        return;
    vb_target_mark(dr.t, &dr.scissor);
    g.st.draws++;
    if (dr.key.cull) g.st.cull_draws++;
    g.st.tris += drawn / 3;
}

/* One program submission's stream slices, in binding order: VbRcConstants is
 * binding 0 (ps_rc reads it), then the corpus shader's c[192] (9) and xf (10). */
enum { VB_PROG_SLOT_RC = 0, VB_PROG_SLOT_CONST = 1, VB_PROG_SLOT_XF = 2, VB_PROG_SLOTS = 3 };
/* vkCmdBindDescriptorSets takes one dynamic offset per dynamic binding, in binding
 * order: binding 0, then 9, then 10. */
typedef char vb_prog_slots_is_3[VB_PROG_SLOTS == 3 ? 1 : -1];

/* Draw a vertex-program batch with its pre-compiled corpus shader; 0 hands it
 * back to the executor's CPU interpreter. There is no translation and no
 * compilation on this path: the identity check above decides everything. */
static int vb_draw_program(const Nv2aSurface *s, const Nv2aBatch *b, const Nv2aProgramBatch *p)
{
    const Nv2aVshAotEntry *e;
    VbDraw dr;
    Nv2aVshHlslXf xf;
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorBufferInfo ub[VB_PROG_SLOTS];
    VkDescriptorImageInfo ii[8];
    VkWriteDescriptorSet wr[VB_PROG_SLOTS + 8];
    VkDescriptorSet set;
    VkBuffer vbuf, ubuf[VB_PROG_SLOTS], ibuf;
    VkDeviceSize voff, uoff[VB_PROG_SLOTS], ioff, total;
    VkViewport vp;
    VkPipeline pipe;
    uint8_t *dst;
    uint32_t offsets[VB_PROG_SLOTS], nin = 0, stride, bit, i;
    VkResult r;

    if (!g.ok || !b->combiner || !p->program_len || !p->positions || !p->index_count
        || p->index_count % 3 || !p->program || !p->constants || !p->inputs || !p->indices) {
        g.st.vsh_declines++;
        g.pst.declines++;
        return 0;
    }
    /* The corpus question comes FIRST for every batch that gets this far, because
     * `misses` and `declines` are defined by the program, not by this draw
     * (nv2a_vk_backend.h): `misses` is "identities the corpus does not carry" and
     * `declines` is "found, but this draw could not be set up". A batch whose program
     * the corpus does not carry is a MISS and returns 0 (CPU fallback) whatever its
     * mask, and that decision is taken below before any per-draw check; `declines` is
     * only for a program the corpus carries that this draw cannot set up. Looking up
     * after the setup checks would report a program the corpus lacks as a decline
     * whenever the batch also happened to be undrawable, and the V5 gate's "0 misses"
     * would then say nothing about those batches' coverage. The lookup itself comes
     * before the cull check below for the same reason: coverage is a property of the
     * batch's identity, not of whether the batch was visible, so every batch is
     * looked up exactly once. */
    e = vb_program_lookup(p);
    /* Cull FRONT_AND_BACK: nothing is visible. As in D3D11 (gb_draw_program) this
     * counts as drawn, so the executor does not transform the batch on the CPU;
     * the key would otherwise bake cull NONE (cull_mode 3 % 3) and draw it. This
     * return does not depend on what the lookup found: nothing visible means nothing
     * drawn by either path, so the batch must not be transformed on the CPU either,
     * and the lookup above owns the coverage accounting (a miss stays a miss). */
    if (vb_cull_mode(b->state) == 3) {
        g.st.cull_all++;
        return 1;
    }
    /* The corpus does not carry this program: a miss, and the CPU path draws the
     * batch. Deliberately independent of the mask check below — an absent program is
     * a miss however this batch's inputs look, so the batch is never counted as both. */
    if (!e)
        return 0;
    vb_maybe_split();
    if (!g.ok) {
        g.st.vsh_declines++;
        g.pst.declines++;
        return 0;
    }
    /* The mask decides the baked input layout; 0 or more than 16 attributes
     * cannot be described at all. Reached only for a program the corpus carries,
     * so an undescribable mask here is a decline. */
    for (bit = 0; bit < 16; bit++)
        nin += (p->input_mask >> bit) & 1u;
    if (!nin || nin > 16) {
        g.st.vsh_declines++;
        g.pst.declines++;
        return 0;
    }
    stride = 16u * nin;
    /* Every corpus shader feeds ps_rc's interpolants, so a combiner the back end
     * cannot interpret is a decline, never the stage-0 pixel shader; and every one
     * of them writes SV_ClipDistance0, so without that feature the program path is
     * off and a decline is the silent, correct answer. */
    if (!vb_rc_supported(b) || !g.shader_clip) {
        g.st.vsh_declines++;
        g.pst.declines++;
        return 0;
    }
    /* Setup after the lookup: a batch this draw cannot set up is a decline, and only a
     * program the corpus carries can be a decline. */
    if (!vb_setup(&dr, s, b, 1)) {
        g.st.vsh_declines++;
        g.pst.declines++;
        return 0;
    }
    if (!g.ok) {                        /* a fence wait failed in setup: record nothing more */
        g.st.vsh_declines++;
        g.pst.declines++;
        return 0;
    }
    dr.key.rc = 1;                      /* ps_rc and its bindings stay in use */
    dr.key.prog = 1;
    /* The hash is what makes the pipeline key unique per program: two programs
     * with the same mask must not share a cached pipeline, or one would be
     * rasterised with the other's shader. */
    dr.key.prog_hash = p->program_hash;
    dr.key.input_mask = p->input_mask;
    pipe = vb_pipeline(&dr.key, e);
    if (!pipe) {
        g.st.vsh_declines++;
        g.pst.declines++;
        return 0;
    }

    /* One reserve for the whole submission, before any slice is written, so the slices and the
     * draw that reads them stay in one slot: nothing after this point moves g.cur but
     * vb_reserve's advance, which happens before the slice it makes room for, and a slice that
     * still does not fit goes to a buffer of its own instead of moving on. Each slice is
     * 256-aligned by vb_stream, so round each one up before summing. */
    total = vb_round_up((VkDeviceSize)p->positions * stride)
          + vb_round_up((VkDeviceSize)p->index_count * 4u)
          + vb_round_up(sizeof(VbRcConstants))
          + vb_round_up(192u * 16u)
          + vb_round_up(sizeof xf);
    if (total > VB_PROG_RESERVE_MAX) {
        g.st.vsh_declines++;
        g.pst.declines++;
        return 0;
    }
    vb_reserve(total, 1);
    if (!g.ok) {
        g.st.vsh_declines++;
        g.pst.declines++;
        return 0;
    }

    dst = vb_stream((VkDeviceSize)p->positions * stride, &vbuf, &voff);
    if (!dst) {
        g.st.vsh_declines++;
        g.pst.declines++;
        return 0;
    }
    memcpy(dst, p->inputs, (size_t)p->positions * stride);

    dst = vb_stream((VkDeviceSize)p->index_count * 4u, &ibuf, &ioff);
    if (!dst) {
        g.st.vsh_declines++;
        g.pst.declines++;
        return 0;
    }
    memcpy(dst, p->indices, (size_t)p->index_count * 4u);

    dst = vb_stream(sizeof dr.rck, &ubuf[VB_PROG_SLOT_RC], &uoff[VB_PROG_SLOT_RC]);
    if (!dst) {
        g.st.vsh_declines++;
        g.pst.declines++;
        return 0;
    }
    memcpy(dst, &dr.rck, sizeof dr.rck);

    dst = vb_stream(192u * 16u, &ubuf[VB_PROG_SLOT_CONST], &uoff[VB_PROG_SLOT_CONST]);
    if (!dst) {
        g.st.vsh_declines++;
        g.pst.declines++;
        return 0;
    }
    memcpy(dst, p->constants, 192u * 16u);

    /* The D3D11 back end's block (nv2a_d3d11_backend.c, gb_draw_program). */
    xf.xf[0] = dr.k.xf[0];
    xf.xf[1] = dr.k.xf[1];
    xf.xf[2] = dr.k.xf[2];
    xf.xf[3] = 0.0f;
    xf.fog[0] = p->fog_enable ? 1.0f : 0.0f;
    xf.fog[1] = (float)p->fog_mode;
    xf.fog[2] = p->fog_param[0];
    xf.fog[3] = p->fog_param[1];
    dst = vb_stream(sizeof xf, &ubuf[VB_PROG_SLOT_XF], &uoff[VB_PROG_SLOT_XF]);
    if (!dst) {
        g.st.vsh_declines++;
        g.pst.declines++;
        return 0;
    }
    memcpy(dst, &xf, sizeof xf);

    dai.descriptorPool = g.slots[g.cur].dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &g.dsl_prog;
    r = vkAllocateDescriptorSets(g.dev, &dai, &set);
    if (r != VK_SUCCESS) {
        vb_fail("vkAllocateDescriptorSets", r);
        g.st.vsh_declines++;
        g.pst.declines++;
        return 0;
    }
    g.slots[g.cur].sets_used++;

    /* Bindings 0 and 9/10: three dynamic uniform buffers, offsets in binding order. */
    memset(ii, 0, sizeof ii);
    memset(wr, 0, sizeof wr);
    for (i = 0; i < VB_PROG_SLOTS; i++) {
        ub[i].buffer = ubuf[i];
        ub[i].offset = 0;
        ub[i].range = i == VB_PROG_SLOT_RC ? sizeof(VbRcConstants)
                   : i == VB_PROG_SLOT_CONST ? 192u * 16u : sizeof xf;
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = set;
        wr[i].dstBinding = i == VB_PROG_SLOT_RC ? 0u : (i == VB_PROG_SLOT_CONST ? 9u : 10u);
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        wr[i].pBufferInfo = &ub[i];
    }
    for (i = 0; i < 4; i++) {
        ii[i].imageView = dr.views[i] ? dr.views[i] : g.white.view;
        ii[i].imageLayout = dr.views[i] ? dr.view_layouts[i]
                                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr[VB_PROG_SLOTS + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[VB_PROG_SLOTS + i].dstSet = set;
        wr[VB_PROG_SLOTS + i].dstBinding = 1u + i;
        wr[VB_PROG_SLOTS + i].descriptorCount = 1;
        wr[VB_PROG_SLOTS + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        wr[VB_PROG_SLOTS + i].pImageInfo = &ii[i];
        ii[4 + i].sampler = dr.samps[i] ? dr.samps[i] : g.samp[0][8];
        wr[VB_PROG_SLOTS + 4 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[VB_PROG_SLOTS + 4 + i].dstSet = set;
        wr[VB_PROG_SLOTS + 4 + i].dstBinding = 5u + i;
        wr[VB_PROG_SLOTS + 4 + i].descriptorCount = 1;
        wr[VB_PROG_SLOTS + 4 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        wr[VB_PROG_SLOTS + 4 + i].pImageInfo = &ii[4 + i];
    }
    vkUpdateDescriptorSets(g.dev, VB_PROG_SLOTS + 8, wr, 0, NULL);

    if (!vb_begin_pass(dr.t, dr.d)) {
        g.st.vsh_declines++;
        g.pst.declines++;
        return 0;
    }
    for (i = 0; i < VB_PROG_SLOTS; i++)
        offsets[i] = (uint32_t)uoff[i];
    if (g.slots[g.cur].bound_pipe != pipe) {
        vkCmdBindPipeline(g.slots[g.cur].cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        g.slots[g.cur].bound_pipe = pipe;
    }
    vkCmdBindDescriptorSets(g.slots[g.cur].cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout_prog, 0, 1,
                            &set, VB_PROG_SLOTS, offsets);
    vkCmdBindVertexBuffers(g.slots[g.cur].cmd, 0, 1, &vbuf, &voff);
    vkCmdPushConstants(g.slots[g.cur].cmd, g.layout_prog,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof dr.k, &dr.k);
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = (float)dr.t->w;
    vp.height = (float)dr.t->h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(g.slots[g.cur].cmd, 0, 1, &vp);
    vkCmdSetScissor(g.slots[g.cur].cmd, 0, 1, &dr.scissor);
    vkCmdSetStencilReference(g.slots[g.cur].cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dr.stencil_ref);
    vkCmdSetStencilCompareMask(g.slots[g.cur].cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dr.stencil_read);
    vkCmdSetStencilWriteMask(g.slots[g.cur].cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dr.stencil_write);
    vkCmdSetBlendConstants(g.slots[g.cur].cmd, dr.blend_factor);
    vkCmdBindIndexBuffer(g.slots[g.cur].cmd, ibuf, ioff, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(g.slots[g.cur].cmd, p->index_count, 1, 0, 0, 0);

    vb_target_mark(dr.t, &dr.scissor);
    g.st.draws++;
    g.st.tris += p->index_count / 3;
    if (dr.key.cull)
        g.st.cull_draws++;
    g.st.vsh_hits++;
    g.st.vsh_tris += p->index_count / 3;
    g.st.vsh_const_uploads++;
    g.pst.const_uploads++;
    if (!g.hit_seen) {
        g.hit_seen = 1;
        g.pst.first_hit_hash = p->program_hash;
        g.pst.first_hit_len = p->program_len;
        g.pst.first_hit_mask = p->input_mask;
        g.pst.first_hit_digest = e->digest;
        g.pst.first_hit_tris = p->index_count / 3;
    }
    g.pst.hits++;
    g.pst.tris += p->index_count / 3;
    return 1;
}

static void vb_clear(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags,
                     uint32_t argb, uint32_t zstencil)
{
    VbDraw dr;
    Nv2aVertex q[6];
    uint32_t mask = 0, all, diffuse;
    float x0, y0, x1, y1, z;
    int i;

    if (!g.ok)
        return;
    vb_maybe_split();
    if (!g.ok)
        return;
    memset(&dr, 0, sizeof dr);
    dr.t = vb_target_for(s);
    if (!dr.t || !vb_scissor(s, dr.t, &dr.scissor)) {
        g.st.skipped++;
        return;
    }
    /* CLEAR_SURFACE colour bits: R 0x10, G 0x20, B 0x40, A 0x80. */
    if (flags & 0x10) mask |= VK_COLOR_COMPONENT_R_BIT;
    if (flags & 0x20) mask |= VK_COLOR_COMPONENT_G_BIT;
    if (flags & 0x40) mask |= VK_COLOR_COMPONENT_B_BIT;
    if ((flags & 0x80) && dr.t->bpp == 4) mask |= VK_COLOR_COMPONENT_A_BIT;
    all = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
        | (dr.t->bpp == 4 ? VK_COLOR_COMPONENT_A_BIT : 0u);
    if (dr.t->stale) {
        /* A clear of every pixel and channel needs nothing from guest memory. */
        if (mask == all && dr.scissor.offset.x == 0 && dr.scissor.offset.y == 0
            && dr.scissor.extent.width == dr.t->w && dr.scissor.extent.height == dr.t->h)
            dr.t->stale = 0;
        else
            vb_target_load(dr.t);
    }
    if ((flags & 0x3) && rs->zeta_va)
        dr.d = vb_depth_for(rs->zeta_va, dr.t->w, dr.t->h);
    if (!mask && !dr.d)
        return;
    dr.key.has_depth = dr.d != NULL;

    /* A 16-bit target's clear value is a 5:6:5 word, not ARGB; expand it for the quad. */
    diffuse = dr.t->bpp == 2 ? nv2a_clear_to_argb(argb, s->color_format) : argb;
    /* The same depth units the draws use, from the top 24 (Z24S8) or low 16 (Z16) bits. */
    z = vb_z_scale(rs) == 65535.0f ? (float)(zstencil & 0xFFFF) / 65535.0f
                                   : (float)(zstencil >> 8) / 16777215.0f;
    x0 = (float)dr.scissor.offset.x;
    y0 = (float)dr.scissor.offset.y;
    x1 = (float)(dr.scissor.offset.x + (int32_t)dr.scissor.extent.width);
    y1 = (float)(dr.scissor.offset.y + (int32_t)dr.scissor.extent.height);
    for (i = 0; i < 6; i++) {
        static const int cx[6] = { 0, 1, 0, 0, 1, 1 }, cy[6] = { 0, 0, 1, 1, 0, 1 };
        q[i].x = cx[i] ? x1 : x0;
        q[i].y = cy[i] ? y1 : y0;
        q[i].z = z;
        q[i].rhw = 1.0f;
        q[i].diffuse = diffuse;
        q[i].u = q[i].v = 0.0f;
    }
    dr.k.xf[0] = 2.0f / (float)dr.t->w;
    dr.k.xf[1] = 2.0f / (float)dr.t->h;
    dr.k.xf[2] = 1.0f;
    dr.k.xf[3] = 0.0f;
    dr.k.tx[2] = -1.0f;
    vb_blend_key(&dr.key, 0, 0, 0, 0, mask);
    /* Z is written by an ALWAYS test, stencil by ALWAYS/REPLACE with the low 8 bits as ref. */
    vb_dss_key(&dr.key, dr.d && (flags & 0x1) ? 1u : 0u, 0x207,
               dr.d && (flags & 0x1) ? 1u : 0u);
    if (dr.d && (flags & 0x2)) {
        dr.key.s_enable = 1;
        dr.key.s_func = 0x207;
        dr.key.s_fail = dr.key.s_zfail = dr.key.s_zpass = 0x1E01;
        dr.stencil_read = dr.stencil_write = 0xFF;
        dr.stencil_ref = zstencil & 0xFF;
    }
    if (!vb_submit(&dr, q, NULL, 6))
        return;
    if (mask)
        vb_target_mark(dr.t, &dr.scissor);
    g.st.clears++;
}

static void vb_stats(int force)
{
    Nv2aVkMemoryStats ms;
    uint64_t now = vb_now_ns();

    if (!force && now - g.last_stats_ns < (uint64_t)VB_STATS_MS * 1000000ull)
        return;
    g.last_stats_ns = now;
    /* The memory counters decide the N4.4 optimization, so they are read through the same
     * accessor a test reads and they are printed here: the deciding numbers have to survive in
     * the run's archived stderr, not only in a droppable debug log. */
    nv2a_vk_backend_memory_stats(&ms);
    fprintf(stderr, "[GPUVK] flips=%u draws=%llu tris=%llu clears=%llu tex_uploads=%llu"
                    " tex_hits=%llu tex_undecodable=%llu rt_aliases=%llu rt_copies=%llu"
                    " rt_loads=%llu readbacks=%llu readback_ms=%.1f dropped_tris=%llu"
                    " skipped=%llu rc_draws=%llu rc_fallback=%llu pipelines=%llu submits=%llu"
                    " reuse_waits=%llu drain_waits=%llu splits=%llu overflows=%llu max_inflight=%u"
                    " cull_draws=%llu cull_all=%llu validation_errors=%llu"
                    " validation_warnings=%llu vsh_hits=%llu vsh_misses=%llu"
                    " vsh_identity_fail=%llu vsh_declines=%llu vsh_tris=%llu"
                    " vsh_const_uploads=%llu rb_batches=%llu rb_packs=%llu rb_pixels=%llu"
                    " rb_copies=%llu rb_bytes=%llu rb_publish_ms=%.1f rb_wait_ms=%.1f"
                    " rb_incomplete=%llu"
                    " mem_pools=%llu mem_blocks=%llu mem_blocks_retired=%llu"
                    " mem_retired_bytes=%llu mem_ranges=%llu mem_reuse=%llu"
                    " mem_dedicated=%llu mem_ded_required=%llu mem_ded_prefer=%llu"
                    " mem_ded_oversize=%llu mem_ded_noroom=%llu mem_ded_nohost=%llu"
                    " mem_alloc_calls=%llu mem_released=%llu mem_parked=%llu"
                    " mem_parked_peak=%llu mem_parked_lost=%llu mem_reserved_bytes=%llu"
                    " mem_used_bytes=%llu mem_free_bytes=%llu mem_largest_free=%llu"
                    " mem_padding_bytes=%llu mem_metadata_bytes=%llu mem_ded_bytes=%llu"
                    " mem_peak_used=%llu mem_shared_block=%u mem_shared_kind=%u"
                    " mem_first_reuse_off=%llu mem_first_reuse_block=%llu"
                    " mem_alloc_failures=%llu mem_bind_failures=%llu mem_map_failures=%llu"
                    " mem_rollbacks=%llu mem_fallbacks=%llu"
                    " pipe_evictions=%llu pcache_loaded=%llu pcache_saves=%llu"
                    " pcache_save_failures=%llu pcache_rejects=%llu\n",
            g.frame, (unsigned long long)g.st.draws, (unsigned long long)g.st.tris,
            (unsigned long long)g.st.clears, (unsigned long long)g.st.tex_uploads,
            (unsigned long long)g.st.tex_hits, (unsigned long long)g.st.tex_undecodable,
            (unsigned long long)g.st.rt_aliases, (unsigned long long)g.st.rt_copies,
            (unsigned long long)g.st.rt_loads, (unsigned long long)g.st.readbacks,
            (double)g.st.readback_ns / 1e6, (unsigned long long)g.st.dropped_tris,
            (unsigned long long)g.st.skipped, (unsigned long long)g.st.rc_draws,
            (unsigned long long)g.st.rc_fallback,
            (unsigned long long)g.st.pipelines, (unsigned long long)g.st.submits,
            (unsigned long long)g.st.reuse_waits, (unsigned long long)g.st.drain_waits,
            (unsigned long long)g.st.splits, (unsigned long long)g.st.overflows, g.max_inflight,
            (unsigned long long)g.st.cull_draws, (unsigned long long)g.st.cull_all,
            (unsigned long long)g.st.validation_errors,
            (unsigned long long)g.st.validation_warnings,
            (unsigned long long)g.st.vsh_hits, (unsigned long long)g.st.vsh_misses,
            (unsigned long long)g.st.vsh_identity_fail, (unsigned long long)g.st.vsh_declines,
            (unsigned long long)g.st.vsh_tris, (unsigned long long)g.st.vsh_const_uploads,
            (unsigned long long)g.st.readback_batches,
            (unsigned long long)g.st.readback_packs,
            (unsigned long long)g.st.readback_pixels,
            (unsigned long long)g.st.readback_copies,
            (unsigned long long)g.st.readback_bytes,
            (double)g.st.readback_publish_ns / 1e6, (double)g.st.readback_wait_ns / 1e6,
            (unsigned long long)g.st.readback_incomplete,
            (unsigned long long)ms.pools, (unsigned long long)ms.blocks,
            (unsigned long long)ms.blocks_retired,
            (unsigned long long)s_retired_bytes, (unsigned long long)ms.ranges,
            (unsigned long long)ms.reuse_hits, (unsigned long long)ms.dedicated,
            (unsigned long long)ms.dedicated_required, (unsigned long long)ms.dedicated_prefer,
            (unsigned long long)ms.dedicated_oversize, (unsigned long long)ms.dedicated_noroom,
            (unsigned long long)ms.dedicated_nohost, (unsigned long long)ms.alloc_calls,
            (unsigned long long)ms.released, (unsigned long long)ms.parked,
            (unsigned long long)ms.parked_peak, (unsigned long long)ms.parked_lost,
            (unsigned long long)ms.reserved_bytes, (unsigned long long)ms.used_bytes,
            (unsigned long long)ms.free_bytes, (unsigned long long)ms.largest_free,
            (unsigned long long)ms.padding_bytes, (unsigned long long)ms.metadata_bytes,
            (unsigned long long)ms.dedicated_bytes, (unsigned long long)ms.peak_used,
            (unsigned)ms.first_shared_block, (unsigned)ms.first_shared_block_kind,
            (unsigned long long)ms.first_reuse_off, (unsigned long long)ms.first_reuse_block,
            (unsigned long long)ms.alloc_failures, (unsigned long long)ms.bind_failures,
            (unsigned long long)ms.map_failures, (unsigned long long)ms.rollbacks,
            (unsigned long long)ms.fallbacks,
            (unsigned long long)g.st.pipe_evictions, (unsigned long long)g.st.pcache_loaded_bytes,
            (unsigned long long)g.st.pcache_saves,
            (unsigned long long)g.st.pcache_save_failures,
            (unsigned long long)g.st.pcache_rejects);
    fflush(stderr);
}

/* End of frame: every target drawn since the last flip goes back to guest
 * memory, and every target reloads from guest memory on its next use.
 *
 * This is the frame's coherence boundary. A flip that cannot publish every dirty target stops
 * the back end instead of moving on: no target is marked stale and g.frame is not advanced, so
 * no later vb_target_load can upload older guest bytes over a target whose newer pixels were
 * never published. Frames only advance over guest memory that really holds them. */
static void vb_flip(void)
{
    VbTarget *ts[VB_MAX_TARGETS];
    unsigned i, n = 0;

    if (!g.ok)
        return;
    for (i = 0; i < VB_MAX_TARGETS; i++)
        if (g.targets[i].used)
            ts[n++] = &g.targets[i];
    if (!vb_targets_flush_recover(ts, n)) {
        vb_stop("the flip could not publish every dirty target");
        return;                     /* NO stale marking, NO frame++, NO submit/drain */
    }
    /* Whatever else was recorded goes out too, so a frame never lingers unpublished. */
    vb_slot_submit();
    vb_drain();
    if (!g.ok)
        return;
    for (i = 0; i < n; i++)
        ts[i]->stale = 1;
    g.frame++;
    vb_pcache_maybe_save();
    vb_stats(!g.stats_said);
    g.stats_said = 1;
}

/* ── Device ──────────────────────────────────────────────────────────── */

static VKAPI_ATTR VkBool32 VKAPI_CALL vb_debug(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                               VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d,
                                               void *user)
{
    int err = (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    /* GENERAL messages come from the loader (an ICD that fails to enumerate, say),
     * not from a validation layer, so only VALIDATION/PERFORMANCE ones are counted. */
    int layer = (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) != 0;

    (void)user;
    if (layer && err)
        g.st.validation_errors++;
    else if (layer)
        g.st.validation_warnings++;
    if (g.validation_said++ < VB_VALIDATION_SAY) {
        fprintf(stderr, "[GPUVK] %s %s: %s\n", layer ? "validation" : "loader",
                err ? "error" : "warning", d && d->pMessage ? d->pMessage : "(no message)");
        fflush(stderr);
    }
    return VK_FALSE;
}

static int vb_load_loader(void)
{
#if defined(_WIN32)
    HMODULE m = LoadLibraryA("vulkan-1.dll");
    g.lib = (void *)m;
    if (m)
        vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void (*)(void))
                                GetProcAddress(m, "vkGetInstanceProcAddr");
#else
    static const char *names[] = { "libvulkan.so.1", "libvulkan.so", "libvulkan.1.dylib" };
    unsigned i;
    for (i = 0; i < sizeof names / sizeof names[0] && !g.lib; i++)
        g.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (g.lib)
        *(void **)&vkGetInstanceProcAddr = dlsym(g.lib, "vkGetInstanceProcAddr");
#endif
    if (!vkGetInstanceProcAddr) {
        fprintf(stderr, "[GPUVK] no Vulkan loader (vulkan-1 / libvulkan) found\n");
        return -1;
    }
#define VB_LOAD_GLOBAL(fn) fn = (PFN_##fn)vkGetInstanceProcAddr(NULL, #fn);
    VB_GLOBAL_FNS(VB_LOAD_GLOBAL)
    if (!vkCreateInstance || !vkEnumerateInstanceExtensionProperties) {
        fprintf(stderr, "[GPUVK] the Vulkan loader lacks vkCreateInstance\n");
        return -1;
    }
    return 0;
}

static int vb_create_instance(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkExtensionProperties *ext = NULL;
    const char *want = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    uint32_t n = 0, i;
    int debug = 0;
    VkResult r;

    /* Messages from layers the environment enables (VK_INSTANCE_LAYERS) are printed. */
    if (vkEnumerateInstanceExtensionProperties(NULL, &n, NULL) == VK_SUCCESS && n) {
        ext = (VkExtensionProperties *)calloc(n, sizeof *ext);
        if (ext && vkEnumerateInstanceExtensionProperties(NULL, &n, ext) == VK_SUCCESS)
            for (i = 0; i < n; i++)
                if (strcmp(ext[i].extensionName, want) == 0)
                    debug = 1;
        free(ext);
    }
    app.pApplicationName = "xboxrecomp";
    app.pEngineName = "nv2a_vk_backend";
    app.apiVersion = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;
    if (debug) {
        ici.enabledExtensionCount = 1;
        ici.ppEnabledExtensionNames = &want;
    }
    r = vkCreateInstance(&ici, NULL, &g.inst);
    if (r != VK_SUCCESS) {
        vb_fail("vkCreateInstance", r);
        return -1;
    }
#define VB_LOAD_INSTANCE(fn) fn = (PFN_##fn)vkGetInstanceProcAddr(g.inst, #fn);
    VB_INSTANCE_FNS(VB_LOAD_INSTANCE)
    if (!vkCreateDevice || !vkGetDeviceProcAddr) {
        fprintf(stderr, "[GPUVK] instance functions missing\n");
        return -1;
    }
    if (debug) {
        VkDebugUtilsMessengerCreateInfoEXT mi = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        vkCreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(g.inst, "vkCreateDebugUtilsMessengerEXT");
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                           | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = vb_debug;
        if (vkCreateDebugUtilsMessengerEXT)
            vkCreateDebugUtilsMessengerEXT(g.inst, &mi, NULL, &g.messenger);
    }
    return 0;
}

static int vb_device_score(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:          return 1;
    default:                                     return 0; /* CPU */
    }
}

static int vb_name_has(const char *name, const char *sub)
{
    size_t n = strlen(name), m = strlen(sub), i, j;

    for (i = 0; i + m <= n; i++) {
        for (j = 0; j < m; j++)
            if (tolower((unsigned char)name[i + j]) != tolower((unsigned char)sub[j]))
                break;
        if (j == m)
            return 1;
    }
    return 0;
}

/* The queue family the back end uses: graphics, and compute so the pack kernel can be
 * dispatched on the same queue. A family with both is preferred; failing that the first
 * graphics family is returned, which vb_pick_device then rejects for lacking compute --
 * there is one 16-bit publication path and it needs the dispatch. */
static int vb_graphics_family(VkPhysicalDevice pd, uint32_t *family)
{
    VkQueueFamilyProperties qf[32];
    uint32_t n = 32, i;

    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, qf);
    for (i = 0; i < n; i++)
        if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            *family = i;
            return 1;
        }
    for (i = 0; i < n; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            *family = i;
            return 1;
        }
    return 0;
}

/* The flags of the family vb_graphics_family picked, so the usable test can name what the
 * family lacks. 0 when it is not a valid family index. */
static VkQueueFlags vb_family_flags(VkPhysicalDevice pd, uint32_t family)
{
    VkQueueFamilyProperties qf[32];
    uint32_t n = 32;

    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, qf);
    return family < n ? qf[family].queueFlags : 0;
}

/* RECOMP_VK_DEVICE picks by name; otherwise discrete > integrated > virtual > other,
 * never a CPU device. Devices below Vulkan 1.1, without a graphics family, without compute
 * on that family (the pack kernel's dispatch), or whose uniform alignment the stream cannot
 * meet are passed over, and the line says which of those it was. */
static int vb_pick_device(void)
{
    const char *want = getenv("RECOMP_VK_DEVICE");
    VkPhysicalDevice devs[16];
    uint32_t n = 16, i, family = 0;
    int best = -1, best_score = 0;
    VkPhysicalDeviceProperties p;

    if (want && !*want)
        want = NULL;
    if (vkEnumeratePhysicalDevices(g.inst, &n, devs) < 0 || !n) {
        fprintf(stderr, "[GPUVK] no Vulkan devices\n");
        return -1;
    }
    for (i = 0; i < n; i++) {
        const char *why = "";
        int score, usable;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_1)
            why = " unusable (older than Vulkan 1.1)";
        else if (!vb_graphics_family(devs[i], &family))
            why = " unusable (no graphics queue family)";
        else if (!(vb_family_flags(devs[i], family) & VK_QUEUE_COMPUTE_BIT))
            why = " unusable (graphics family has no compute, which the readback's pack "
                  "kernel needs)";
        else if (p.limits.minUniformBufferOffsetAlignment > VB_STREAM_ALIGN)
            why = " unusable (minUniformBufferOffsetAlignment exceeds the stream's alignment)";
        usable = !*why;
        score = vb_device_score(p.deviceType);
        fprintf(stderr, "[GPUVK] found device %u: %s (type %d, api %u.%u)%s\n", i, p.deviceName,
                (int)p.deviceType, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
                why);
        if (!usable)
            continue;
        if (want) {
            if (best < 0 && vb_name_has(p.deviceName, want))
                best = (int)i;
        } else if (score > best_score) {
            best_score = score;
            best = (int)i;
        }
    }
    if (best < 0) {
        fprintf(stderr, want ? "[GPUVK] no usable device matches RECOMP_VK_DEVICE=%s\n"
                             : "[GPUVK] no usable non-CPU device%s (RECOMP_VK_DEVICE picks one by name)\n",
                want ? want : "");
        return -1;
    }
    g.pd = devs[best];
    vb_graphics_family(g.pd, &g.family);
    vkGetPhysicalDeviceProperties(g.pd, &p);
    /* The atom vb_mem_flush/vb_mem_invalidate widen a non-coherent range to: a deviation from
     * the range rule would touch a shared block's neighbours, and this is what the
     * specification fixes the granularity at. 0 means the device reports none. */
    g.atom = p.limits.nonCoherentAtomSize ? p.limits.nonCoherentAtomSize : 1;
    fprintf(stderr, "[GPUVK] device: %s (type %d, api %u.%u.%u, driver 0x%X)%s\n", p.deviceName,
            (int)p.deviceType, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
            VK_VERSION_PATCH(p.apiVersion), p.driverVersion,
            want ? " picked by RECOMP_VK_DEVICE" : "");
    return 0;
}

static int vb_create_device(void)
{
    VkPhysicalDeviceFeatures have, on;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkFormatProperties fp;
    float prio = 1.0f;
    VkResult r;

    vkGetPhysicalDeviceFeatures(g.pd, &have);
    memset(&on, 0, sizeof on);
    on.depthClamp = have.depthClamp;
    on.textureCompressionBC = have.textureCompressionBC;
    on.shaderClipDistance = have.shaderClipDistance;
    g.depth_clamp = have.depthClamp != 0;
    g.bc = have.textureCompressionBC != 0;
    g.shader_clip = have.shaderClipDistance != 0;
    qi.queueFamilyIndex = g.family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.pEnabledFeatures = &on;
    r = vkCreateDevice(g.pd, &di, NULL, &g.dev);
    if (r != VK_SUCCESS) {
        vb_fail("vkCreateDevice", r);
        return -1;
    }
#define VB_LOAD_DEVICE(fn) fn = (PFN_##fn)vkGetDeviceProcAddr(g.dev, #fn); \
    if (!fn) { fprintf(stderr, "[GPUVK] missing %s\n", #fn); return -1; }
    VB_DEVICE_FNS(VB_LOAD_DEVICE)
    /* The optional entrypoints (a NULL one is not an install failure; see their declarations):
     * the requirement queries the allocator honors when they exist, and the two coherence
     * boundaries and the unmapping a retired block needs. */
    vkGetImageMemoryRequirements2 =
        (PFN_vkGetImageMemoryRequirements2)vkGetDeviceProcAddr(g.dev, "vkGetImageMemoryRequirements2");
    vkGetBufferMemoryRequirements2 =
        (PFN_vkGetBufferMemoryRequirements2)vkGetDeviceProcAddr(g.dev, "vkGetBufferMemoryRequirements2");
    vkFlushMappedMemoryRanges =
        (PFN_vkFlushMappedMemoryRanges)vkGetDeviceProcAddr(g.dev, "vkFlushMappedMemoryRanges");
    vkInvalidateMappedMemoryRanges =
        (PFN_vkInvalidateMappedMemoryRanges)vkGetDeviceProcAddr(g.dev,
                                                               "vkInvalidateMappedMemoryRanges");
    vkUnmapMemory = (PFN_vkUnmapMemory)vkGetDeviceProcAddr(g.dev, "vkUnmapMemory");
    vkGetPipelineCacheData =
        (PFN_vkGetPipelineCacheData)vkGetDeviceProcAddr(g.dev, "vkGetPipelineCacheData");
    vkGetDeviceQueue(g.dev, g.family, 0, &g.queue);
    vkGetPhysicalDeviceMemoryProperties(g.pd, &g.mp);

    /* NV2A's Z24S8; D32S8 where the device has no D24S8 attachment (AMD). */
    g.depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
    vkGetPhysicalDeviceFormatProperties(g.pd, g.depth_format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        g.depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
        vkGetPhysicalDeviceFormatProperties(g.pd, g.depth_format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            fprintf(stderr, "[GPUVK] no depth-stencil format\n");
            return -1;
        }
    }
    fprintf(stderr, "[GPUVK] depth %s, depth clamp %s, BC textures %s\n",
            g.depth_format == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
            g.depth_clamp ? "on" : "off (near/far clip instead)",
            g.bc ? "native" : "decoded on the CPU");
    if (!g.shader_clip) {
        /* Every corpus shader writes SV_ClipDistance0 (the CPU path's NV_CLIP_W), so
         * without the feature the program path is off for the whole run. */
        fprintf(stderr, "[GPUVK] no shaderClipDistance: the vertex-program path is"
                        " disabled and the CPU interpreter draws every program batch\n");
        fflush(stderr);
    }
    return 0;
}

static int vb_create_pipeline_objects(void)
{
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkAttachmentDescription att[2];
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub;
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    VkDescriptorSetLayoutBinding bind[2];
    VkDescriptorSetLayoutBinding bind_rc[9];
    VkDescriptorSetLayoutBinding bind_prog[11];
    VkDescriptorSetLayoutBinding bind_pack[2];
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPushConstantRange pcr;
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    const void *blob = NULL;
    size_t blob_n = 0;
    uint8_t *file = NULL;
    char outcome[96];
    VkComputePipelineCreateInfo cp = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkDescriptorPoolSize ps[4];
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    int d, i;
    VkResult r;

    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = g.family;
    if ((r = vkCreateCommandPool(g.dev, &cpi, NULL, &g.pool)) != VK_SUCCESS)
        goto fail;
    /* The ring: one command buffer, fence, descriptor pool and upload stream per slot. A slot
     * is free exactly while `pending` is 0 -- the fence is created unsignaled, as vkQueueSubmit
     * requires (VUID-vkQueueSubmit-fence-00063), and vb_slot_wait resets it right after it
     * signals, so a slot that is recorded into again has an unsignaled fence. A slot's objects
     * are only touched while it is not in flight, so a submission's recording and its resources
     * stay untouched until its fence has been observed. */
    cai.commandPool = g.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    ps[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    ps[0].descriptorCount = 4 * VB_SLOT_SETS;
    ps[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    ps[1].descriptorCount = 4 * VB_SLOT_SETS;
    ps[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    ps[2].descriptorCount = 3 * VB_SLOT_SETS;
    ps[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps[3].descriptorCount = VB_SLOT_SETS;   /* the pack set's one storage buffer */
    dpi.maxSets = VB_SLOT_SETS;
    dpi.poolSizeCount = 4;
    dpi.pPoolSizes = ps;
    for (i = 0; i < VB_SLOTS; i++) {
        VbSlot *s = &g.slots[i];
        if ((r = vkAllocateCommandBuffers(g.dev, &cai, &s->cmd)) != VK_SUCCESS)
            goto fail;
        if ((r = vkCreateFence(g.dev, &fci, NULL, &s->fence)) != VK_SUCCESS)
            goto fail;
        if ((r = vkCreateDescriptorPool(g.dev, &dpi, NULL, &s->dpool)) != VK_SUCCESS)
            goto fail;
        /* One draw's descriptors cover 1 image, 1 sampler and up to 3 dynamic uniform
         * buffers (the combiner registers, c[192] and the xf block), and one packed target's
         * cover 1 image and 1 storage buffer (the pack buffer), so the pool sizes above
         * cover the larger of the set layouts and the pack's own. */
        if (!vb_host_buffer(VB_SLOT_STREAM, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                            | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                                            | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                            | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0,
                            &s->stream, &s->stream_mem, &s->stream_ptr))
            return -1;
    }
    /* The dead buffer of vb_cmd, from the same pool: begun once here and then never ended,
     * submitted or reset, so recording any command into it is valid and reaches nothing. */
    if ((r = vkAllocateCommandBuffers(g.dev, &cai, &g.dead)) != VK_SUCCESS)
        goto fail;
    {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        if ((r = vkBeginCommandBuffer(g.dead, &bi)) != VK_SUCCESS)
            goto fail;
    }

    /* Attachments are loaded and stored, and stay in GENERAL between passes. */
    memset(att, 0, sizeof att);
    att[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = att[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    att[1] = att[0];
    att[1].format = g.depth_format;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    for (d = 0; d < 2; d++) {
        memset(&sub, 0, sizeof sub);
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &cref;
        sub.pDepthStencilAttachment = d ? &dref : NULL;
        rpi.attachmentCount = d ? 2u : 1u;
        rpi.pAttachments = att;
        rpi.subpassCount = 1;
        rpi.pSubpasses = &sub;
        if ((r = vkCreateRenderPass(g.dev, &rpi, NULL, &g.rp[d])) != VK_SUCCESS)
            goto fail;
    }

    memset(bind, 0, sizeof bind);
    bind[0].binding = 0;
    bind[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bind[0].descriptorCount = 1;
    bind[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bind[1] = bind[0];
    bind[1].binding = 1;
    bind[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    dli.bindingCount = 2;
    dli.pBindings = bind;
    if ((r = vkCreateDescriptorSetLayout(g.dev, &dli, NULL, &g.dsl)) != VK_SUCCESS)
        goto fail;
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(VbConstants);
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &g.dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if ((r = vkCreatePipelineLayout(g.dev, &pli, NULL, &g.layout)) != VK_SUCCESS)
        goto fail;

    /* The combiner pipeline's own set: the registers in a dynamic UBO, then the four
     * stages' images and samplers. Its own layout keeps the stage-0 set untouched. */
    memset(bind_rc, 0, sizeof bind_rc);
    bind_rc[0].binding = 0;
    bind_rc[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bind_rc[0].descriptorCount = 1;
    bind_rc[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (i = 0; i < 4; i++) {
        bind_rc[1 + i] = bind_rc[0];
        bind_rc[1 + i].binding = (uint32_t)(1 + i);
        bind_rc[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bind_rc[5 + i] = bind_rc[0];
        bind_rc[5 + i].binding = (uint32_t)(5 + i);
        bind_rc[5 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    }
    dli.bindingCount = 9;
    dli.pBindings = bind_rc;
    if ((r = vkCreateDescriptorSetLayout(g.dev, &dli, NULL, &g.dsl_rc)) != VK_SUCCESS)
        goto fail;
    pli.pSetLayouts = &g.dsl_rc;
    if ((r = vkCreatePipelineLayout(g.dev, &pli, NULL, &g.layout_rc)) != VK_SUCCESS)
        goto fail;

    /* The vertex-program pipeline's set: the combiner registers, the four stages'
     * images and samplers, then the corpus shader's two cbuffers (float4 c[192] and
     * Nv2aVshHlslXf), all dynamic. The push-constant range is layout_rc's: ps_rc
     * still reads VbConstants. */
    memcpy(bind_prog, bind_rc, sizeof bind_rc);
    bind_prog[9] = bind_rc[0];
    bind_prog[9].binding = 9;
    bind_prog[9].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bind_prog[10] = bind_prog[9];
    bind_prog[10].binding = 10;
    dli.bindingCount = 11;
    dli.pBindings = bind_prog;
    if ((r = vkCreateDescriptorSetLayout(g.dev, &dli, NULL, &g.dsl_prog)) != VK_SUCCESS)
        goto fail;
    pli.pSetLayouts = &g.dsl_prog;
    if ((r = vkCreatePipelineLayout(g.dev, &pli, NULL, &g.layout_prog)) != VK_SUCCESS)
        goto fail;

    /* The pack kernel's set: the target's own image as a sampled image, which the kernel reads
     * with OpImageFetch (no sampler, so it is a plain SAMPLED_IMAGE) and the device-local pack
     * buffer, bound whole at offset 0 so no dynamic offset alignment is involved. Its push
     * range is the 32-byte PackPush block. */
    memset(bind_pack, 0, sizeof bind_pack);
    bind_pack[0].binding = 0;
    bind_pack[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bind_pack[0].descriptorCount = 1;
    bind_pack[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bind_pack[1] = bind_pack[0];
    bind_pack[1].binding = 1;
    bind_pack[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    dli.bindingCount = 2;
    dli.pBindings = bind_pack;
    if ((r = vkCreateDescriptorSetLayout(g.dev, &dli, NULL, &g.dsl_pack)) != VK_SUCCESS)
        goto fail;
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(VbPackPush);
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &g.dsl_pack;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if ((r = vkCreatePipelineLayout(g.dev, &pli, NULL, &g.layout_pack)) != VK_SUCCESS)
        goto fail;
    /* A valid file seeds the cache; if the driver refuses it the cache is made empty instead. */
    file = NULL;
    vb_pcache_init(&blob, &blob_n, &file, outcome, sizeof outcome);
    if (file) {
        pci.initialDataSize = blob_n;
        pci.pInitialData = blob;
    }
    r = vkCreatePipelineCache(g.dev, &pci, NULL, &g.pcache);
    if (r != VK_SUCCESS && file) {
        g.st.pcache_rejects++;
        snprintf(outcome, sizeof outcome, "rejected(the driver refused the data)");
        pci.initialDataSize = 0;
        pci.pInitialData = NULL;
        r = vkCreatePipelineCache(g.dev, &pci, NULL, &g.pcache);
    } else if (r == VK_SUCCESS && file) {
        g.st.pcache_loaded_bytes = blob_n;
        snprintf(outcome, sizeof outcome, "loaded=%llu", (unsigned long long)blob_n);
    }
    free(file);
    vb_pcache_report(outcome);
    if (r != VK_SUCCESS)
        goto fail;
    smi.codeSize = xbr_nv2a_vk_stage0_vs_size;
    smi.pCode = xbr_nv2a_vk_stage0_vs;
    if ((r = vkCreateShaderModule(g.dev, &smi, NULL, &g.vs)) != VK_SUCCESS)
        goto fail;
    smi.codeSize = xbr_nv2a_vk_stage0_ps_size;
    smi.pCode = xbr_nv2a_vk_stage0_ps;
    if ((r = vkCreateShaderModule(g.dev, &smi, NULL, &g.ps)) != VK_SUCCESS)
        goto fail;
    smi.codeSize = xbr_nv2a_vk_rc_vs_size;
    smi.pCode = xbr_nv2a_vk_rc_vs;
    if ((r = vkCreateShaderModule(g.dev, &smi, NULL, &g.vs_rc)) != VK_SUCCESS)
        goto fail;
    smi.codeSize = xbr_nv2a_vk_rc_ps_size;
    smi.pCode = xbr_nv2a_vk_rc_ps;
    if ((r = vkCreateShaderModule(g.dev, &smi, NULL, &g.ps_rc)) != VK_SUCCESS)
        goto fail;
    smi.codeSize = xbr_nv2a_vk_pack_cs_size;
    smi.pCode = xbr_nv2a_vk_pack_cs;
    if ((r = vkCreateShaderModule(g.dev, &smi, NULL, &g.cs_pack)) != VK_SUCCESS)
        goto fail;
    cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = g.cs_pack;
    cp.stage.pName = "cs_pack";
    cp.layout = g.layout_pack;
    if ((r = vkCreateComputePipelines(g.dev, g.pcache, 1, &cp, NULL, &g.pipe_pack)) != VK_SUCCESS)
        goto fail;

    /* One set per draw, of either kind; the combiner's uses 4 images, 4 samplers and
     * one dynamic uniform buffer, the program's three (rc registers, c[192], the xf
     * block), so the dynamic count must cover the larger. One set per packed 16-bit
     * target uses 1 image and 1 storage buffer (the pack buffer). The pools
     * themselves, and the streams, are the ring slots' (above). */
    return 0;
fail:
    vb_fail("pipeline objects", r);
    return -1;
}

/* A 1x1 white texture for untextured draws (the shader ignores it) and the
 * default clamp/point sampler, so every draw binds valid descriptors. */
static int vb_create_defaults(void)
{
    Nv2aTexture white;
    VkBufferImageCopy bc;
    VkCommandBuffer cmd;
    VkBuffer buf;
    VkDeviceSize off;
    uint8_t *p;

    memset(&white, 0, sizeof white);
    white.filter = 0x01010000u;             /* box min and mag */
    white.addr_u = white.addr_v = 3;        /* clamp */
    if (!vb_sampler(&white) || !g.samp[0][8])
        return -1;
    if (!vb_image_create(&g.white, 1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT))
        return -1;
    p = vb_stream(4, &buf, &off);
    if (!p)
        return -1;
    memset(p, 0xFF, 4);
    cmd = vb_transfer_cmd();            /* taken from the call that produced it, not assumed to
                                         * be g.slots[g.cur].cmd: a slot whose begin failed is
                                         * handed back as the dead buffer, never as an unusable
                                         * one, so this copy can never land in a never-begun
                                         * slot command buffer */
    /* As in vb_tex_upload: discarded contents before the copy that writes it. */
    vb_image_dep(g.white.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    memset(&bc, 0, sizeof bc);
    bc.bufferOffset = off;
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = bc.imageExtent.height = bc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, buf, g.white.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    /* As in vb_tex_upload: the upload's write before the draws that sample it, with the same
     * conservative compute bit for the sampler class. */
    vb_image_dep(g.white.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_SHADER_READ_BIT);
    g.ok = 1;                               /* the submission path reports a lost device through it */
    vb_slot_submit();
    vb_drain();
    if (g.failed || !g.ok)                  /* the install submission failed: install nothing */
        return -1;
    g.ok = 0;
    return 0;
}

/* The executor's CPU-consumer gate (nv2a_backend.h): 1 once this back end can no longer
 * publish a frame, so no CPU path may write guest framebuffer memory on top of the bytes it
 * published last for a target rectangle, and guest memory must not be presented as a new
 * frame. Read-only, executor thread, and write-once per process: a back end that reports it
 * stays stopped. */
static int vb_stopped(void)
{
    return g.failed || !g.ok;
}

static const Nv2aBackend s_vk_backend = { vb_clear, vb_draw, vb_flip, vb_draw_program,
                                          vb_stopped, nv2a_vk_rc_coverage_report };

void nv2a_vk_backend_program_stats(Nv2aVkProgramStats *out)
{
    if (out)
        *out = g.pst;
}

void nv2a_vk_backend_pipeline_stats(Nv2aVkPipelineStats *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    out->created = g.st.pipelines;
    out->evictions = g.st.pipe_evictions;
    out->cache_loaded_bytes = g.st.pcache_loaded_bytes;
    out->cache_saves = g.st.pcache_saves;
    out->cache_save_failures = g.st.pcache_save_failures;
    out->cache_rejects = g.st.pcache_rejects;
    out->cache_enabled = g.pc_on ? 1u : 0u;
}

void nv2a_vk_backend_ring_stats(Nv2aVkRingStats *out)
{
    unsigned i, inflight = 0;

    if (!out)
        return;
    memset(out, 0, sizeof *out);
    out->slots = VB_SLOTS;
    for (i = 0; i < VB_SLOTS; i++)
        if (g.slots[i].pending)
            inflight++;
    out->in_flight = inflight;
    out->max_in_flight = g.max_inflight;
    out->submissions = g.st.submits;
    out->reuse_waits = g.st.reuse_waits;
    out->drain_waits = g.st.drain_waits;
    out->splits = g.st.splits;
    out->overflows = g.st.overflows;
    out->frames = g.frame;
}

void nv2a_vk_backend_readback_stats(Nv2aVkReadbackStats *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    out->readbacks = g.st.readbacks;
    out->batches = g.st.readback_batches;
    out->packed = g.st.readback_packs;
    out->packed_pixels = g.st.readback_pixels;
    out->copied = g.st.readback_copies;
    out->bytes = g.st.readback_bytes;
    out->wait_ns = g.st.readback_wait_ns;
    out->publish_ns = g.st.readback_publish_ns;
    out->incomplete = g.st.readback_incomplete;
}

/* What the sub-allocator did and holds. Every cumulative counter is accumulated as it
 * happens; the live fields (pools in use, the bytes reserved/used/free/padding, the largest
 * free range and the allocator's own footprint) are derived here from the pools, so they
 * cannot drift from the blocks they describe. Same threading contract as the rest of the back
 * end: read it on the executor thread, or from a single-threaded fixture. */
void nv2a_vk_backend_memory_stats(Nv2aVkMemoryStats *out)
{
    unsigned i, k, f;
    uint64_t free_b, widest;

    if (!out)
        return;
    *out = s_mem;
    out->pools = 0;
    out->reserved_bytes = s_mem.dedicated_bytes;    /* a dedicated allocation reserves itself */
    out->used_bytes = s_mem.dedicated_bytes;
    out->free_bytes = 0;
    out->largest_free = 0;
    out->padding_bytes = 0;
    out->metadata_bytes = (uint64_t)(sizeof g.pools + sizeof s_pool_pending);
    for (i = 0; i < VB_POOLS; i++) {
        VbPool *p = &g.pools[i];

        if (!p->used)
            continue;
        out->pools++;
        out->reserved_bytes += p->reserved;
        out->used_bytes += p->live;
        out->padding_bytes += p->padding;
        for (k = 0; k < VB_POOL_BLOCKS; k++) {
            VbBlock *b = &p->blocks[k];

            if (b->mem == VK_NULL_HANDLE)
                continue;
            /* The block minus what it holds and minus the padding it spent aligning ranges:
             * that is exactly the bump cursor's remainder plus the free ranges it still
             * records (``free list + used + padding == bump'' per block). */
            free_b = b->size - b->used;
            free_b -= b->padding < free_b ? b->padding : free_b;
            out->free_bytes += free_b;
            widest = b->size - b->bump;
            for (f = 0; f < b->nfree; f++)
                if (b->free_list[f].size > widest)
                    widest = b->free_list[f].size;
            if (widest > out->largest_free)
                out->largest_free = widest;
        }
    }
}

int nv2a_vk_backend_install(void)
{
    static int result = 1;                  /* 1: not attempted yet */

    if (result != 1)
        return result;
    result = -1;
    if (vb_load_loader() != 0 || vb_create_instance() != 0 || vb_pick_device() != 0
        || vb_create_device() != 0 || vb_create_pipeline_objects() != 0
        || vb_create_defaults() != 0) {
        fprintf(stderr, "[GPUVK] Vulkan back end not installed; the executor keeps its CPU path\n");
        fflush(stderr);
        return result;
    }
    g.ok = 1;
    g.last_stats_ns = g.pc_last_ns = vb_now_ns();
    nv2a_backend_register(&s_vk_backend);
    fprintf(stderr, "[GPUVK] Vulkan back end registered%s\n",
            getenv("RECOMP_PB_EXEC") ? ""
                                     : " (RECOMP_PB_EXEC is not set, so nothing will call it)");
    fflush(stderr);
    result = 0;
    return result;
}

#endif /* XBR_VK_BACKEND */
