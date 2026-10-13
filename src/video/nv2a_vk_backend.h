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
 */
#ifndef NV2A_VK_BACKEND_H
#define NV2A_VK_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

/* Open the loader, create the device and the fixed pipeline objects, and
 * register the back end with the executor. Call before the guest starts
 * submitting work; the executor itself must be enabled (RECOMP_PB_EXEC).
 * Returns 0 on success; on failure nothing is registered and the executor
 * keeps rasterising on the CPU. Idempotent. */
int nv2a_vk_backend_install(void);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_VK_BACKEND_H */
