/* vk_smoke: instance + device, draw the proof triangle with the embedded SPIR-V
 * into a 64x64 image, copy to a host-visible buffer, check pixels.
 * Exit 0 pass, 1 fail, 77 skip (CTest SKIP_RETURN_CODE).
 *   --require-cpu  use only a CPU (software) device
 *   --forbid-cpu   skip when the only device is a CPU one
 *   --skip <why>   print the reason and skip */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_ENABLE_BETA_EXTENSIONS   /* VK_KHR_portability_subset's name lives in the beta header */
#include <vulkan/vulkan.h>

#include "xbr_smoke_tri_vs.spv.h"
#include "xbr_smoke_tri_ps.spv.h"

#define W 64
#define H 64
#define SKIP 77

#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "vk_smoke: FAIL %s = %d (line %d)\n", #call, (int)r_, __LINE__); return 1; } } while (0)

static int score(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:          return 1;
    default:                                     return 0; /* CPU */
    }
}

static int find_memtype(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static int check_px(const uint8_t *px, int x, int y, int r, int g, int b, int a, const char *what)
{
    const uint8_t *p = px + (y * W + x) * 4;
    int ok = abs(p[0] - r) <= 1 && abs(p[1] - g) <= 1 && abs(p[2] - b) <= 1 && abs(p[3] - a) <= 1;
    printf("vk_smoke: (%2d,%2d) %-8s got %3d,%3d,%3d,%3d want %3d,%3d,%3d,%3d %s\n",
           x, y, what, p[0], p[1], p[2], p[3], r, g, b, a, ok ? "ok" : "MISMATCH");
    return ok;
}

int main(int argc, char **argv)
{
    int require_cpu = 0, forbid_cpu = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--require-cpu")) require_cpu = 1;
        else if (!strcmp(argv[i], "--forbid-cpu")) forbid_cpu = 1;
        else if (!strcmp(argv[i], "--skip")) {
            printf("vk_smoke: SKIP: %s\n", i + 1 < argc ? argv[i + 1] : "");
            return SKIP;
        }
    }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "vk_smoke";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    /* A portability driver (MoltenVK) is hidden by the loader unless the instance enables
     * VK_KHR_portability_enumeration with its flag. */
    const char *inst_ext[1];
    uint32_t nie = 0;
    uint32_t ninst = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &ninst, NULL);
    VkExtensionProperties *iext = calloc(ninst ? ninst : 1, sizeof *iext);
    if (iext && vkEnumerateInstanceExtensionProperties(NULL, &ninst, iext) == VK_SUCCESS)
        for (uint32_t i = 0; i < ninst; i++)
            if (!strcmp(iext[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
                inst_ext[nie++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
                ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            }
    free(iext);
    ici.enabledExtensionCount = nie;
    ici.ppEnabledExtensionNames = nie ? inst_ext : NULL;
    VkInstance inst;
    VkResult ir = vkCreateInstance(&ici, NULL, &inst);
    if (ir != VK_SUCCESS) {
        printf("vk_smoke: SKIP: vkCreateInstance = %d (no Vulkan driver)\n", (int)ir);
        return SKIP;
    }

    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, NULL));
    VkPhysicalDevice *devs = calloc(n ? n : 1, sizeof *devs);
    CHECK(vkEnumeratePhysicalDevices(inst, &n, devs));
    int best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        int cpu = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        printf("vk_smoke: found device %u: %s (type %d)\n", i, p.deviceName, (int)p.deviceType);
        if (require_cpu && !cpu) continue;
        if (forbid_cpu && cpu) continue;
        if (score(p.deviceType) > best_score) { best_score = score(p.deviceType); best = (int)i; }
    }
    if (best < 0) {
        printf("vk_smoke: SKIP: no suitable device (%u found)%s\n", n,
               require_cpu ? "; the loader ignores VK_DRIVER_FILES in an elevated process, run non-elevated" : "");
        return SKIP;
    }
    VkPhysicalDevice pd = devs[best];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("vk_smoke: using device: %s (type %d, api %u.%u.%u)\n", props.deviceName, (int)props.deviceType,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
    int drv_props = props.apiVersion >= VK_API_VERSION_1_2;   /* else VK_KHR_driver_properties */
    if (!drv_props) {
        uint32_t nx = 0;
        vkEnumerateDeviceExtensionProperties(pd, NULL, &nx, NULL);
        VkExtensionProperties *xp = calloc(nx ? nx : 1, sizeof *xp);
        if (xp && vkEnumerateDeviceExtensionProperties(pd, NULL, &nx, xp) == VK_SUCCESS)
            for (uint32_t i = 0; i < nx; i++)
                if (!strcmp(xp[i].extensionName, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME))
                    drv_props = 1;
        free(xp);
    }
    if (drv_props) {
        VkPhysicalDeviceDriverProperties dp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        p2.pNext = &dp;
        vkGetPhysicalDeviceProperties2(pd, &p2);
        printf("vk_smoke: driver: %s (%s)\n", dp.driverName, dp.driverInfo);
    }
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq ? nq : 1, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    int family = -1;
    for (uint32_t i = 0; i < nq; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = (int)i; break; }
    if (family < 0) { fprintf(stderr, "vk_smoke: FAIL no graphics queue\n"); return 1; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = (uint32_t)family; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    /* A device that lists VK_KHR_portability_subset must have it enabled. */
    const char *dev_ext[1];
    uint32_t nde = 0, ndev = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ndev, NULL);
    VkExtensionProperties *dext = calloc(ndev ? ndev : 1, sizeof *dext);
    if (dext && vkEnumerateDeviceExtensionProperties(pd, NULL, &ndev, dext) == VK_SUCCESS)
        for (uint32_t i = 0; i < ndev; i++)
            if (!strcmp(dext[i].extensionName, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
                dev_ext[nde++] = VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME;
    free(dext);
    dci.enabledExtensionCount = nde;
    dci.ppEnabledExtensionNames = nde ? dev_ext : NULL;
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, (uint32_t)family, 0, &queue);

    /* Colour target. */
    VkImageCreateInfo imi = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imi.imageType = VK_IMAGE_TYPE_2D; imi.format = VK_FORMAT_R8G8B8A8_UNORM;
    imi.extent.width = W; imi.extent.height = H; imi.extent.depth = 1;
    imi.mipLevels = 1; imi.arrayLayers = 1; imi.samples = VK_SAMPLE_COUNT_1_BIT;
    imi.tiling = VK_IMAGE_TILING_OPTIMAL;
    imi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img;
    CHECK(vkCreateImage(dev, &imi, NULL, &img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, img, &mr);
    int mt = find_memtype(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) mt = find_memtype(&mp, mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = (uint32_t)mt;
    VkDeviceMemory imem;
    CHECK(vkAllocateMemory(dev, &mai, NULL, &imem));
    CHECK(vkBindImageMemory(dev, img, imem, 0));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = imi.format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1; vci.subresourceRange.layerCount = 1;
    VkImageView view;
    CHECK(vkCreateImageView(dev, &vci, NULL, &view));

    /* Readback buffer. */
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = W * H * 4; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    mt = find_memtype(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) { fprintf(stderr, "vk_smoke: FAIL no host-visible coherent memory\n"); return 1; }
    mai.allocationSize = mr.size; mai.memoryTypeIndex = (uint32_t)mt;
    VkDeviceMemory bmem;
    CHECK(vkAllocateMemory(dev, &mai, NULL, &bmem));
    CHECK(vkBindBufferMemory(dev, buf, bmem, 0));

    /* Render pass: clear, store, leave in TRANSFER_SRC. */
    VkAttachmentDescription att = { 0 };
    att.format = imi.format; att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { 0 };
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    VkSubpassDependency dep = { 0 };
    dep.srcSubpass = 0; dep.dstSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 1; rpi.pAttachments = &att; rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1; rpi.pDependencies = &dep;
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rpi, NULL, &rp));
    VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbi.renderPass = rp; fbi.attachmentCount = 1; fbi.pAttachments = &view; fbi.width = W; fbi.height = H; fbi.layers = 1;
    VkFramebuffer fb;
    CHECK(vkCreateFramebuffer(dev, &fbi, NULL, &fb));

    /* Pipeline from the embedded SPIR-V. */
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule vs, ps;
    smi.codeSize = xbr_smoke_tri_vs_size; smi.pCode = xbr_smoke_tri_vs;
    CHECK(vkCreateShaderModule(dev, &smi, NULL, &vs));
    smi.codeSize = xbr_smoke_tri_ps_size; smi.pCode = xbr_smoke_tri_ps;
    CHECK(vkCreateShaderModule(dev, &smi, NULL, &ps));
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps, "main", NULL },
    };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = { 0, 0, W, H, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = { 0 };
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpi.stageCount = 2; gpi.pStages = st; gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vps; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms; gpi.pColorBlendState = &cb;
    gpi.layout = pl; gpi.renderPass = rp;
    VkPipeline pipe;
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));

    /* Record: clear blue, draw the triangle, copy to the buffer. */
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.queueFamilyIndex = (uint32_t)family;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(cmd, &cbi));
    VkClearValue clear = { .color = { { 0.0f, 0.0f, 1.0f, 1.0f } } };
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = rp; rbi.framebuffer = fb; rbi.renderArea = sc; rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy bic = { 0 };
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = W; bic.imageExtent.height = H; bic.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
    CHECK(vkEndCommandBuffer(cmd));

    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 30ull * 1000000000ull));

    void *mapped;
    CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, &mapped));
    const uint8_t *px = mapped;
    int ok = 1;
    /* The triangle covers the lower-left half (vertex shader is built with
     * -fvk-invert-y); the clear colour shows in the upper-right half. */
    ok &= check_px(px,  4, 59, 255, 128,   0, 255, "triangle");
    ok &= check_px(px, 20, 40, 255, 128,   0, 255, "triangle");
    ok &= check_px(px, 59,  4,   0,   0, 255, 255, "clear");
    ok &= check_px(px, 40, 20,   0,   0, 255, 255, "clear");
    vkUnmapMemory(dev, bmem);

    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyShaderModule(dev, ps, NULL);
    vkDestroyShaderModule(dev, vs, NULL);
    vkDestroyFramebuffer(dev, fb, NULL);
    vkDestroyRenderPass(dev, rp, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, bmem, NULL);
    vkDestroyImageView(dev, view, NULL);
    vkDestroyImage(dev, img, NULL);
    vkFreeMemory(dev, imem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    free(qf);
    free(devs);

    printf("vk_smoke: %s on %s\n", ok ? "PASS" : "FAIL", props.deviceName);
    return ok ? 0 : 1;
}
