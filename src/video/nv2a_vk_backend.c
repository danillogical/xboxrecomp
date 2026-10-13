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
 *     16-bit surface is expanded on load and truncated to 5:6:5 on flush. A
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
 *   - Only the stage-0 path exists: stage 0 modulated by diffuse, alpha test in
 *     the pixel shader. Batches carrying register-combiner state are drawn that
 *     way and counted as rc_unsupported; draw_program is NULL, so the executor
 *     transforms vertex programs on the CPU.
 *
 * Vulkan specifics:
 *   - y: the vertex shader is D3D11's, built with -fvk-invert-y, so surface
 *     pixel (x, y) lands on framebuffer pixel (x, y) with a positive-height
 *     viewport; winding is judged in framebuffer space in both APIs, so NV2A
 *     CW (0x900) is VK_FRONT_FACE_CLOCKWISE as it is FrontCounterClockwise=FALSE.
 *   - Pipelines are cached by the state that is baked into them (blend, write
 *     mask, depth, stencil func/ops, cull, winding, depth attachment present);
 *     viewport, scissor, stencil reference/masks and blend constants are dynamic.
 *   - Every image a target or depth buffer owns stays in GENERAL layout, and
 *     every render pass end and transfer is followed by a global memory barrier.
 *   - Synchronisation is deliberately simple: one command buffer records until
 *     a flip, a readback, or the per-submission vertex/upload stream or
 *     descriptor pool fills; then it is submitted and waited on. Objects released
 *     while commands may still use them are destroyed after that wait.
 *     Later performance work goes there: frames in flight with per-frame streams
 *     and descriptor pools instead of a wait per flip, a single batched readback
 *     per flip without the CPU convert on the critical path, narrower barriers
 *     and layouts, sub-allocated memory, and a persisted VkPipelineCache.
 *
 * Everything runs on the executor's thread, one call at a time.
 */
#include "nv2a_vk_backend.h"

#if !defined(XBR_VK_BACKEND)

/* Built without Vulkan: report failure so the caller keeps the CPU path. */
int nv2a_vk_backend_install(void)
{
    return -1;
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

/* Built from shaders/nv2a_vk_stage0.hlsl by xbr_add_spirv. */
#include "xbr_nv2a_vk_stage0_vs.spv.h"
#include "xbr_nv2a_vk_stage0_ps.spv.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define VB_MAX_TARGETS   16
#define VB_MAX_DEPTHS    8
#define VB_MAX_TEXTURES  512
#define VB_MAX_PIPES     256
#define VB_MAX_FBS       64
#define VB_MAX_DIM       4096
#define VB_STREAM_BYTES  (32u << 20)      /* vertices and uploads of one submission */
#define VB_MAX_SETS      8192             /* descriptor sets of one submission */
#define VB_STATS_MS      10000
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
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) \
    X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkCreatePipelineCache) X(vkCreatePipelineLayout) \
    X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdCopyImage) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetStencilReference) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetBlendConstants) \
    X(vkCmdDraw)

#define VB_DECLARE(fn) static PFN_##fn fn;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
VB_GLOBAL_FNS(VB_DECLARE)
VB_INSTANCE_FNS(VB_DECLARE)
VB_DEVICE_FNS(VB_DECLARE)
static PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT;

/* ── State ───────────────────────────────────────────────────────────── */

/* Push constants, shared by both stages; the D3D11 back end's GbConstants. */
typedef struct { float xf[4], tx[4]; } VbConstants;

typedef struct {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
} VbImage;

typedef struct {
    int used;
    uint32_t va, pitch, bpp, w, h;
    VbImage im, copy;                   /* the target, and its snapshot for self-sampling */
    VkBuffer rb;                        /* readback buffer, w*h*4, made on first flush */
    VkDeviceMemory rb_mem;
    uint8_t *rb_ptr;
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

/* Everything baked into a pipeline; all uint32_t, so memcmp compares it. */
typedef struct {
    uint32_t has_depth;
    uint32_t blend, src, dst, eq, wmask;                 /* blend: src/dst/eq 0 when off */
    uint32_t z_enable, z_func, z_write;
    uint32_t s_enable, s_func, s_fail, s_zfail, s_zpass;
    uint32_t cull, ccw;                                  /* cull 0 none 1 back 2 front */
} VbPipeKey;

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
    VkDeviceMemory mem;
    VkBuffer buf;
    VkFramebuffer fb;
    VkPipeline pipe;
} VbGarbage;

typedef struct {
    uint64_t draws, tris, clears, dropped_tris, skipped, cull_draws, cull_all;
    uint64_t tex_uploads, tex_hits, tex_undecodable, rt_aliases, rt_copies;
    uint64_t rt_loads, readbacks, readback_ns, rc_unsupported, pipelines, submits;
    uint64_t validation_errors, validation_warnings;
} VbStats;

static struct {
    int ok;
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
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    int recording;
    VkRenderPass rp[2];                 /* [has depth] */
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipelineCache pcache;
    VkShaderModule vs, ps;
    VkDescriptorPool dpool;
    uint32_t sets_used;
    VkBuffer stream;                    /* host-visible ring of one submission */
    VkDeviceMemory stream_mem;
    uint8_t *stream_ptr;
    VkDeviceSize stream_off;
    VbImage white;                      /* bound when a draw is untextured */
    VkSampler samp[4][9];               /* [filter][addr_u*3 + addr_v], made on first use */
    /* The open render pass, and the pipeline bound in the recording command buffer. */
    int pass_open;
    VbTarget *pass_t;
    VbDepth *pass_d;
    VkPipeline bound_pipe;
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
} g;

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

static VkDeviceMemory vb_alloc(const VkMemoryRequirements *mr, VkMemoryPropertyFlags want,
                               VkMemoryPropertyFlags fallback)
{
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    int t = vb_memtype(mr->memoryTypeBits, want);
    VkResult r;

    if (t < 0)
        t = vb_memtype(mr->memoryTypeBits, fallback);
    if (t < 0) {
        vb_fail("memory type", VK_ERROR_FEATURE_NOT_PRESENT);
        return VK_NULL_HANDLE;
    }
    ai.allocationSize = mr->size;
    ai.memoryTypeIndex = (uint32_t)t;
    r = vkAllocateMemory(g.dev, &ai, NULL, &mem);
    if (r != VK_SUCCESS) {
        vb_fail("vkAllocateMemory", r);
        return VK_NULL_HANDLE;
    }
    return mem;
}

/* A host-visible buffer, persistently mapped; coherent (and cached when `cached`). */
static int vb_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, int cached,
                          VkBuffer *buf, VkDeviceMemory *mem, uint8_t **ptr)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                   | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    void *p = NULL;
    VkResult r;

    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = vkCreateBuffer(g.dev, &bi, NULL, buf);
    if (r != VK_SUCCESS) {
        vb_fail("vkCreateBuffer", r);
        return 0;
    }
    vkGetBufferMemoryRequirements(g.dev, *buf, &mr);
    *mem = vb_alloc(&mr, cached ? coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT : coherent,
                    coherent);
    if (*mem && (r = vkBindBufferMemory(g.dev, *buf, *mem, 0)) == VK_SUCCESS)
        r = vkMapMemory(g.dev, *mem, 0, VK_WHOLE_SIZE, 0, &p);
    if (!*mem || r != VK_SUCCESS) {
        if (*mem) {
            vb_fail("buffer memory", r);
            vkFreeMemory(g.dev, *mem, NULL);
        }
        vkDestroyBuffer(g.dev, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        *mem = VK_NULL_HANDLE;
        return 0;
    }
    *ptr = (uint8_t *)p;
    return 1;
}

static void vb_garbage_free(VbGarbage *e)
{
    if (e->fb) vkDestroyFramebuffer(g.dev, e->fb, NULL);
    if (e->pipe) vkDestroyPipeline(g.dev, e->pipe, NULL);
    if (e->view) vkDestroyImageView(g.dev, e->view, NULL);
    if (e->img) vkDestroyImage(g.dev, e->img, NULL);
    if (e->buf) vkDestroyBuffer(g.dev, e->buf, NULL);
    if (e->mem) vkFreeMemory(g.dev, e->mem, NULL);
}

/* Destroy now if nothing is recorded, else after the submission that may use it. */
static void vb_discard(VbGarbage e)
{
    if (!g.recording) {
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

    memset(&e, 0, sizeof e);
    e.img = im->img;
    e.view = im->view;
    e.mem = im->mem;
    if (e.img || e.view || e.mem)
        vb_discard(e);
    memset(im, 0, sizeof *im);
}

static VkCommandBuffer vb_cmd(void)
{
    if (!g.recording) {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkResult r;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        r = vkBeginCommandBuffer(g.cmd, &bi);
        if (r != VK_SUCCESS)
            vb_fail("vkBeginCommandBuffer", r);
        g.recording = 1;
        g.bound_pipe = VK_NULL_HANDLE;
    }
    return g.cmd;
}

/* Every write before it is visible to every access after it. Deliberately broad. */
static void vb_barrier(void)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };

    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(vb_cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void vb_image_barrier(VkImage img, VkImageAspectFlags aspect, VkImageLayout from,
                             VkImageLayout to)
{
    VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };

    ib.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    ib.oldLayout = from;
    ib.newLayout = to;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img;
    ib.subresourceRange.aspectMask = aspect;
    ib.subresourceRange.levelCount = 1;
    ib.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(vb_cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
}

static void vb_end_pass(void)
{
    if (!g.pass_open)
        return;
    vkCmdEndRenderPass(g.cmd);
    g.pass_open = 0;
    g.pass_t = NULL;
    g.pass_d = NULL;
    vb_barrier();
}

/* The recording command buffer, outside any render pass, for a transfer. */
static VkCommandBuffer vb_transfer_cmd(void)
{
    VkCommandBuffer c = vb_cmd();
    vb_end_pass();
    return c;
}

/* Submit what is recorded and wait for it; then the stream, the descriptor pool
 * and the garbage are free again. A lost device turns the back end off. */
static void vb_submit_wait(void)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkResult r;
    size_t i;

    if (g.recording) {
        vb_end_pass();
        r = vkEndCommandBuffer(g.cmd);
        if (r == VK_SUCCESS) {
            si.commandBufferCount = 1;
            si.pCommandBuffers = &g.cmd;
            r = vkQueueSubmit(g.queue, 1, &si, g.fence);
        }
        if (r == VK_SUCCESS)
            r = vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, VB_WAIT_NS);
        if (r != VK_SUCCESS) {
            vb_fail(r == VK_TIMEOUT ? "submission wait (10 s timeout)" : "submission", r);
            fprintf(stderr, "[GPUVK] the Vulkan back end stops drawing; guest memory keeps "
                            "its last flushed frame\n");
            fflush(stderr);
            g.ok = 0;
            g.recording = 0;
            return;                     /* nothing is freed: the GPU may still use it */
        }
        vkResetFences(g.dev, 1, &g.fence);
        vkResetCommandBuffer(g.cmd, 0);
        vkResetDescriptorPool(g.dev, g.dpool, 0);
        g.recording = 0;
        g.st.submits++;
    }
    g.sets_used = 0;
    g.stream_off = 0;
    for (i = 0; i < g.ngarbage; i++)
        vb_garbage_free(&g.garbage[i]);
    g.ngarbage = 0;
}

/* Make sure `bytes` of stream and `sets` descriptor sets fit in this submission,
 * so the allocations that follow cannot submit between being written and used. */
static void vb_reserve(VkDeviceSize bytes, uint32_t sets)
{
    VkDeviceSize off = (g.stream_off + 255) & ~(VkDeviceSize)255;

    if (off + bytes > VB_STREAM_BYTES || g.sets_used + sets > VB_MAX_SETS)
        vb_submit_wait();
}

/* `bytes` of host memory the recording commands can read at *buf + *off. Anything
 * over half the stream gets a buffer of its own, freed after the submission. */
static uint8_t *vb_stream(VkDeviceSize bytes, VkBuffer *buf, VkDeviceSize *off)
{
    VkDeviceSize o;

    if (bytes > VB_STREAM_BYTES / 2) {
        VbGarbage e;
        uint8_t *p;
        memset(&e, 0, sizeof e);
        if (!vb_host_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                   | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0, &e.buf, &e.mem, &p))
            return NULL;
        vb_cmd();                       /* so the discard waits for this submission */
        vb_discard(e);
        *buf = e.buf;
        *off = 0;
        return p;
    }
    vb_reserve(bytes, 0);
    o = (g.stream_off + 255) & ~(VkDeviceSize)255;
    g.stream_off = o + bytes;
    *buf = g.stream;
    *off = o;
    return g.stream_ptr + o;
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

static VkPipeline vb_pipeline_create(const VbPipeKey *k)
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
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r;

    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = g.vs;
    st[0].pName = "vs_main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = g.ps;
    st[1].pName = "ps_main";

    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions = va;
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
    pi.layout = g.layout;
    pi.renderPass = g.rp[k->has_depth ? 1 : 0];
    r = vkCreateGraphicsPipelines(g.dev, g.pcache, 1, &pi, NULL, &pipe);
    if (r != VK_SUCCESS) {
        vb_fail("vkCreateGraphicsPipelines", r);
        return VK_NULL_HANDLE;
    }
    g.st.pipelines++;
    return pipe;
}

/* The pipeline for a key, from the cache or made now; the least recently used goes. */
static VkPipeline vb_pipeline(const VbPipeKey *k)
{
    VbPipe *e = NULL;
    unsigned i;

    for (i = 0; i < g.npipes; i++) {
        if (memcmp(&g.pipes[i].k, k, sizeof *k) == 0) {
            g.pipes[i].last_use = ++g.use_clock;
            return g.pipes[i].pipe;
        }
        if (!e || g.pipes[i].last_use < e->last_use)
            e = &g.pipes[i];
    }
    if (g.npipes < VB_MAX_PIPES) {
        e = &g.pipes[g.npipes++];
    } else {
        VbGarbage gb;
        memset(&gb, 0, sizeof gb);
        gb.pipe = e->pipe;
        if (gb.pipe == g.bound_pipe)
            g.bound_pipe = VK_NULL_HANDLE;
        vb_discard(gb);
    }
    e->k = *k;
    e->pipe = vb_pipeline_create(k);
    e->last_use = ++g.use_clock;
    if (!e->pipe) {
        /* Do not cache the failure under a key that later succeeds elsewhere. */
        memset(&e->k, 0xFF, sizeof e->k);
    }
    return e->pipe;
}

/* ── Images, framebuffers, render passes ─────────────────────────────── */

static int vb_image_create(VbImage *im, uint32_t w, uint32_t h, VkFormat fmt,
                           VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements mr;
    VkResult r;

    memset(im, 0, sizeof *im);
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
    vkGetImageMemoryRequirements(g.dev, im->img, &mr);
    im->mem = vb_alloc(&mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (!im->mem || (r = vkBindImageMemory(g.dev, im->img, im->mem, 0)) != VK_SUCCESS) {
        if (im->mem)
            vb_fail("vkBindImageMemory", r);
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

    vb_cmd();
    if (g.pass_open && g.pass_t == t && g.pass_d == d)
        return 1;
    vb_end_pass();
    fb = vb_framebuffer(t, d);
    if (!fb)
        return 0;
    bi.renderPass = g.rp[d ? 1 : 0];
    bi.framebuffer = fb;
    bi.renderArea.extent.width = t->w;
    bi.renderArea.extent.height = t->h;
    vkCmdBeginRenderPass(g.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    g.pass_open = 1;
    g.pass_t = t;
    g.pass_d = d;
    return 1;
}

/* ── Render targets ──────────────────────────────────────────────────── */

static void vb_target_release(VbTarget *t)
{
    VbGarbage e;

    if (g.pass_t == t)
        vb_end_pass();
    vb_fb_forget(t->im.view);
    vb_image_discard(&t->im);
    vb_image_discard(&t->copy);
    memset(&e, 0, sizeof e);
    e.buf = t->rb;
    e.mem = t->rb_mem;
    if (e.buf || e.mem)
        vb_discard(e);
    memset(t, 0, sizeof *t);
}

/* Write the unflushed rectangles of `n` targets into guest memory in the guest format,
 * with one submission for all of them. */
static void vb_targets_flush(VbTarget **ts, unsigned n)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    uint64_t t0 = vb_now_ns();
    unsigned i, copies = 0;

    for (i = 0; i < n; i++) {
        VbTarget *t = ts[i];
        VkBufferImageCopy bc;
        if (!t->dirty)
            continue;
        if (!t->rb && !vb_host_buffer((VkDeviceSize)t->w * t->h * 4,
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1,
                                      &t->rb, &t->rb_mem, &t->rb_ptr)) {
            t->dirty = 0;
            continue;
        }
        memset(&bc, 0, sizeof bc);
        bc.bufferOffset = ((VkDeviceSize)t->dy0 * t->w + t->dx0) * 4;
        bc.bufferRowLength = t->w;
        bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bc.imageSubresource.layerCount = 1;
        bc.imageOffset.x = (int32_t)t->dx0;
        bc.imageOffset.y = (int32_t)t->dy0;
        bc.imageExtent.width = t->dx1 - t->dx0;
        bc.imageExtent.height = t->dy1 - t->dy0;
        bc.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(vb_transfer_cmd(), t->im.img, VK_IMAGE_LAYOUT_GENERAL,
                               t->rb, 1, &bc);
        copies++;
    }
    if (!copies)
        return;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
    vb_submit_wait();
    if (!g.ok)
        return;
    for (i = 0; i < n; i++) {
        VbTarget *t = ts[i];
        VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
        uint8_t *dst;
        uint32_t x, y;
        if (!t->dirty)
            continue;
        /* Cached memory may not be coherent everywhere; invalidating is always allowed. */
        mr.memory = t->rb_mem;
        mr.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(g.dev, 1, &mr);
        dst = vb_guest(t->va);
        for (y = t->dy0; y < t->dy1; y++) {
            const uint32_t *src = (const uint32_t *)(t->rb_ptr + (size_t)y * t->w * 4) + t->dx0;
            uint8_t *row = dst + (size_t)y * t->pitch;
            if (t->bpp == 4) {
                memcpy(row + (size_t)t->dx0 * 4, src, (size_t)(t->dx1 - t->dx0) * 4);
            } else {
                /* Truncated to 5:6:5, as the CPU rasteriser stores it. */
                uint16_t *p = (uint16_t *)row + t->dx0;
                for (x = 0; x < t->dx1 - t->dx0; x++) {
                    uint32_t c = src[x];
                    p[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0)
                                    | ((c >> 3) & 0x001F));
                }
            }
        }
        t->dirty = 0;
        g.st.readbacks++;
    }
    g.st.readback_ns += vb_now_ns() - t0;
}

static void vb_target_flush(VbTarget *t)
{
    vb_targets_flush(&t, 1);
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
    vkCmdCopyBufferToImage(vb_transfer_cmd(), buf, t->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    vb_barrier();
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
    vb_image_barrier(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_GENERAL);
    memset(&zero, 0, sizeof zero);
    memset(&sr, 0, sizeof sr);
    sr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sr.levelCount = 1;
    sr.layerCount = 1;
    vkCmdClearColorImage(g.cmd, t->im.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &sr);
    vb_barrier();
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
        vb_target_flush(t);
        vb_target_release(t);
    } else {
        t = lru;
        if (t->used) {
            vb_target_flush(t);
            vb_target_release(t);
        }
    }
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
    vb_image_barrier(d->im.img, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    far0.depth = 1.0f;
    far0.stencil = 0;
    memset(&sr, 0, sizeof sr);
    sr.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    sr.levelCount = 1;
    sr.layerCount = 1;
    vkCmdClearDepthStencilImage(g.cmd, d->im.img, VK_IMAGE_LAYOUT_GENERAL, &far0, 1, &sr);
    vb_barrier();
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
    dst = vb_stream(bytes, &buf, &off);
    if (!dst)
        return;
    memcpy(dst, native ? vb_guest(tx->offset) : (const uint8_t *)dec, (size_t)bytes);
    if (!vb_image_create(&e->im, tx->width, tx->height, fmt,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT))
        return;
    vb_transfer_cmd();
    vb_image_barrier(e->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&bc, 0, sizeof bc);
    bc.bufferOffset = off;
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = tx->width;
    bc.imageExtent.height = tx->height;
    bc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(g.cmd, buf, e->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    vb_image_barrier(e->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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

    for (i = 0; i < VB_MAX_TARGETS; i++) {
        VbTarget *t = &g.targets[i];
        VkImageCopy ic;
        if (!vb_tex_aliases(tx, t))
            continue;
        vb_target_load(t);
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
            vb_image_barrier(t->copy.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_GENERAL);
        }
        memset(&ic, 0, sizeof ic);
        ic.srcSubresource.aspectMask = ic.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ic.srcSubresource.layerCount = ic.dstSubresource.layerCount = 1;
        ic.extent.width = t->w;
        ic.extent.height = t->h;
        ic.extent.depth = 1;
        vkCmdCopyImage(vb_transfer_cmd(), t->im.img, VK_IMAGE_LAYOUT_GENERAL, t->copy.img,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
        vb_barrier();
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
        if (t->used && t->dirty && lo < (uint64_t)tx->offset + span && tx->offset < hi)
            vb_target_flush(t);
    }

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
    VbPipeKey key;
    float blend_factor[4];
    uint32_t stencil_ref, stencil_read, stencil_write;
    VbConstants k;
    VkRect2D scissor;
} VbDraw;

static int vb_vertex_ok(const Nv2aVertex *v)
{
    return isfinite(v->x) && isfinite(v->y) && isfinite(v->z) && isfinite(v->rhw)
        && v->rhw != 0.0f;
}

/* Upload the usable triangles of `v` and draw them; returns how many vertices were drawn. */
static uint32_t vb_submit(const VbDraw *dr, const Nv2aVertex *v, uint32_t n)
{
    const uint32_t cap = (VB_STREAM_BYTES / 2) / (uint32_t)sizeof(Nv2aVertex);
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorImageInfo ii[2];
    VkWriteDescriptorSet wr[2];
    VkDescriptorSet set;
    VkPipeline pipe;
    VkBuffer buf;
    VkDeviceSize off;
    VkViewport vp;
    Nv2aVertex *dst;
    uint32_t i, out = 0;
    VkResult r;

    if (n > cap)
        n = cap - cap % 3;
    pipe = vb_pipeline(&dr->key);
    if (!pipe)
        return 0;
    /* Vertices and the descriptor set land in the same submission as the draw. */
    vb_reserve((VkDeviceSize)n * sizeof *v, 1);
    if (!g.ok)
        return 0;
    dst = (Nv2aVertex *)vb_stream((VkDeviceSize)n * sizeof *v, &buf, &off);
    if (!dst)
        return 0;
    for (i = 0; i + 3 <= n; i += 3) {
        if (!(vb_vertex_ok(&v[i]) && vb_vertex_ok(&v[i + 1]) && vb_vertex_ok(&v[i + 2]))) {
            g.st.dropped_tris++;
            continue;
        }
        memcpy(dst + out, v + i, 3 * sizeof *v);
        out += 3;
    }
    if (!out)
        return 0;

    dai.descriptorPool = g.dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &g.dsl;
    r = vkAllocateDescriptorSets(g.dev, &dai, &set);
    if (r != VK_SUCCESS) {
        vb_fail("vkAllocateDescriptorSets", r);
        return 0;
    }
    g.sets_used++;
    memset(ii, 0, sizeof ii);
    memset(wr, 0, sizeof wr);
    ii[0].imageView = dr->view ? dr->view : g.white.view;
    ii[0].imageLayout = dr->view ? dr->view_layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[1].sampler = dr->samp ? dr->samp : g.samp[0][8];
    for (i = 0; i < 2; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = set;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(g.dev, 2, wr, 0, NULL);

    if (!vb_begin_pass(dr->t, dr->d))
        return 0;
    if (g.bound_pipe != pipe) {
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        g.bound_pipe = pipe;
    }
    vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &set, 0, NULL);
    vkCmdBindVertexBuffers(g.cmd, 0, 1, &buf, &off);
    vkCmdPushConstants(g.cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof dr->k, &dr->k);
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = (float)dr->t->w;
    vp.height = (float)dr->t->h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(g.cmd, 0, 1, &vp);
    vkCmdSetScissor(g.cmd, 0, 1, &dr->scissor);
    vkCmdSetStencilReference(g.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dr->stencil_ref);
    vkCmdSetStencilCompareMask(g.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dr->stencil_read);
    vkCmdSetStencilWriteMask(g.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dr->stencil_write);
    vkCmdSetBlendConstants(g.cmd, dr->blend_factor);
    vkCmdDraw(g.cmd, out, 1, 0, 0);
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

/* Set up everything of a draw but its vertices: target, depth, transform and
 * alpha constants, stage 0, blend and depth state. Returns 1 ready, 0 no target
 * or an empty clip. */
static int vb_setup(VbDraw *dr, const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    uint32_t bc = rs->blend_color;
    int cm = vb_cull_mode(rs);
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
    if (b->texture) {
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

    if (!g.ok || b->count < 3)
        return;
    if (vb_cull_mode(b->state) == 3) {
        g.st.cull_all++;
        return;
    }
    if (!vb_setup(&dr, s, b)) {
        g.st.skipped++;
        return;
    }
    /* No combiner shader yet: drawn as stage 0 modulated by diffuse. */
    if (b->combiner)
        g.st.rc_unsupported++;

    drawn = vb_submit(&dr, b->vertices, b->count);
    if (!drawn)
        return;
    vb_target_mark(dr.t, &dr.scissor);
    g.st.draws++;
    if (dr.key.cull) g.st.cull_draws++;
    g.st.tris += drawn / 3;
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
    if (!vb_submit(&dr, q, 6))
        return;
    if (mask)
        vb_target_mark(dr.t, &dr.scissor);
    g.st.clears++;
}

static void vb_stats(int force)
{
    uint64_t now = vb_now_ns();

    if (!force && now - g.last_stats_ns < (uint64_t)VB_STATS_MS * 1000000ull)
        return;
    g.last_stats_ns = now;
    fprintf(stderr, "[GPUVK] flips=%u draws=%llu tris=%llu clears=%llu tex_uploads=%llu"
                    " tex_hits=%llu tex_undecodable=%llu rt_aliases=%llu rt_copies=%llu"
                    " rt_loads=%llu readbacks=%llu readback_ms=%.1f dropped_tris=%llu"
                    " skipped=%llu rc_unsupported=%llu pipelines=%llu submits=%llu"
                    " cull_draws=%llu cull_all=%llu validation_errors=%llu"
                    " validation_warnings=%llu\n",
            g.frame, (unsigned long long)g.st.draws, (unsigned long long)g.st.tris,
            (unsigned long long)g.st.clears, (unsigned long long)g.st.tex_uploads,
            (unsigned long long)g.st.tex_hits, (unsigned long long)g.st.tex_undecodable,
            (unsigned long long)g.st.rt_aliases, (unsigned long long)g.st.rt_copies,
            (unsigned long long)g.st.rt_loads, (unsigned long long)g.st.readbacks,
            (double)g.st.readback_ns / 1e6, (unsigned long long)g.st.dropped_tris,
            (unsigned long long)g.st.skipped, (unsigned long long)g.st.rc_unsupported,
            (unsigned long long)g.st.pipelines, (unsigned long long)g.st.submits,
            (unsigned long long)g.st.cull_draws, (unsigned long long)g.st.cull_all,
            (unsigned long long)g.st.validation_errors,
            (unsigned long long)g.st.validation_warnings);
    fflush(stderr);
}

/* End of frame: every target drawn since the last flip goes back to guest
 * memory, and every target reloads from guest memory on its next use. */
static void vb_flip(void)
{
    VbTarget *ts[VB_MAX_TARGETS];
    unsigned i, n = 0;

    if (!g.ok)
        return;
    for (i = 0; i < VB_MAX_TARGETS; i++)
        if (g.targets[i].used)
            ts[n++] = &g.targets[i];
    vb_targets_flush(ts, n);
    vb_submit_wait();               /* whatever else was recorded, so a frame never lingers */
    for (i = 0; i < n; i++)
        ts[i]->stale = 1;
    g.frame++;
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

static int vb_graphics_family(VkPhysicalDevice pd, uint32_t *family)
{
    VkQueueFamilyProperties qf[32];
    uint32_t n = 32, i;

    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, qf);
    for (i = 0; i < n; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            *family = i;
            return 1;
        }
    return 0;
}

/* RECOMP_VK_DEVICE picks by name; otherwise discrete > integrated > virtual > other,
 * never a CPU device. Devices below Vulkan 1.1 or without graphics are passed over. */
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
        int score, usable;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        usable = p.apiVersion >= VK_API_VERSION_1_1 && vb_graphics_family(devs[i], &family);
        score = vb_device_score(p.deviceType);
        fprintf(stderr, "[GPUVK] found device %u: %s (type %d, api %u.%u)%s\n", i, p.deviceName,
                (int)p.deviceType, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
                usable ? "" : " unusable");
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
    g.depth_clamp = have.depthClamp != 0;
    g.bc = have.textureCompressionBC != 0;
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
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPushConstantRange pcr;
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkDescriptorPoolSize ps[2];
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    int d;
    VkResult r;

    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = g.family;
    if ((r = vkCreateCommandPool(g.dev, &cpi, NULL, &g.pool)) != VK_SUCCESS)
        goto fail;
    cai.commandPool = g.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if ((r = vkAllocateCommandBuffers(g.dev, &cai, &g.cmd)) != VK_SUCCESS)
        goto fail;
    if ((r = vkCreateFence(g.dev, &fci, NULL, &g.fence)) != VK_SUCCESS)
        goto fail;

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
    if ((r = vkCreatePipelineCache(g.dev, &pci, NULL, &g.pcache)) != VK_SUCCESS)
        goto fail;
    smi.codeSize = xbr_nv2a_vk_stage0_vs_size;
    smi.pCode = xbr_nv2a_vk_stage0_vs;
    if ((r = vkCreateShaderModule(g.dev, &smi, NULL, &g.vs)) != VK_SUCCESS)
        goto fail;
    smi.codeSize = xbr_nv2a_vk_stage0_ps_size;
    smi.pCode = xbr_nv2a_vk_stage0_ps;
    if ((r = vkCreateShaderModule(g.dev, &smi, NULL, &g.ps)) != VK_SUCCESS)
        goto fail;

    ps[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    ps[0].descriptorCount = VB_MAX_SETS;
    ps[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    ps[1].descriptorCount = VB_MAX_SETS;
    dpi.maxSets = VB_MAX_SETS;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    if ((r = vkCreateDescriptorPool(g.dev, &dpi, NULL, &g.dpool)) != VK_SUCCESS)
        goto fail;
    if (!vb_host_buffer(VB_STREAM_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                         | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0,
                        &g.stream, &g.stream_mem, &g.stream_ptr))
        return -1;
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
    vb_transfer_cmd();
    vb_image_barrier(g.white.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&bc, 0, sizeof bc);
    bc.bufferOffset = off;
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = bc.imageExtent.height = bc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(g.cmd, buf, g.white.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    vb_image_barrier(g.white.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    g.ok = 1;                               /* vb_submit_wait reports a lost device through it */
    vb_submit_wait();
    if (!g.ok)
        return -1;
    g.ok = 0;
    return 0;
}

static const Nv2aBackend s_vk_backend = { vb_clear, vb_draw, vb_flip, NULL };

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
    g.last_stats_ns = vb_now_ns();
    nv2a_backend_register(&s_vk_backend);
    fprintf(stderr, "[GPUVK] Vulkan back end registered%s\n",
            getenv("RECOMP_PB_EXEC") ? ""
                                     : " (RECOMP_PB_EXEC is not set, so nothing will call it)");
    fflush(stderr);
    result = 0;
    return result;
}

#endif /* XBR_VK_BACKEND */
