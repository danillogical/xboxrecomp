/* This fork's apu_vp.c reaches guest memory through apu_phys_ptr
 * (apu_shim.h, apu_watch.c in the runtime); the test's fake RAM answers it
 * through its own mcpx_apu_phys. */
#include <stdint.h>

uint8_t *mcpx_apu_phys(uint64_t addr);

uint8_t *apu_phys_ptr(uint32_t addr, uint32_t len)
{
    (void)len;
    return mcpx_apu_phys(addr);
}
