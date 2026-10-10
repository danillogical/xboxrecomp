/*
 * Direct3D 11 render back end for the NV2A pushbuffer executor.
 *
 * The executor (src/kernel/nv2a_pb_exec.c) decodes what a title asks the GPU
 * to draw and, by default, rasterises it on the CPU into guest memory. This
 * back end takes the same decoded batches (nv2a_backend.h) and rasterises them
 * with Direct3D 11 instead, then writes each finished frame back into guest
 * memory at the flip, so everything that reads guest memory -- the
 * framebuffer window, the present tracker, frame dumps -- works unchanged.
 *
 * Windows only. Elsewhere the installer is a stub that reports failure.
 */
#ifndef NV2A_D3D11_BACKEND_H
#define NV2A_D3D11_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

/* Create the device (hardware adapter, else WARP), compile the shaders and
 * register the back end with the executor. Call before the guest starts
 * submitting work; the executor itself must be enabled (RECOMP_PB_EXEC).
 * Returns 0 on success; on failure nothing is registered and the executor
 * keeps rasterising on the CPU. Idempotent. */
int nv2a_d3d11_backend_install(void);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_D3D11_BACKEND_H */
