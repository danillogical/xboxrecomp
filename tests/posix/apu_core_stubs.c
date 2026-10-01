/* The host-only symbols the portable APU tests link against, for
 * tools/posix_check.py. apu_core.c needs XAudio2 and winmm, and
 * xbox_memory_layout.c owns the guest address space; neither builds on a POSIX
 * host, and the tests that use these stubs exercise neither. */
#include "apu_state.h"
#include <stddef.h>
#include <stdint.h>

uint8_t *g_apu_ram_ptr = NULL;
struct McpxApuDebug g_dbg, g_dbg_cache;
int g_dbg_voice_monitor = -1;

int mcpx_apu_test_tone_active(void) { return 0; }

/* No guest memory is mapped, so every VP and GP DMA address fails closed. */
size_t xbox_GetMappedSize(void) { return 0; }
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
