/* What this fork's executor also needs from the runtime, beyond the stubs in
 * nv2a_raster_batch_test.c (upstream's): the fence release, the owner's commit
 * consumer, the present trace and the NV2A monotonic clock. None of them is
 * reached by the batch this test draws. */
#include <stdint.h>

int xbox_Nv2aFenceRelease(uint32_t value) { (void)value; return 0; }

/* nv2a_state.h's nv2a_commit_consumer_fn is a function pointer; any pointer
 * type has its ABI. */
typedef void (*fork_commit_consumer_fn)(void);
void nv2a_set_commit_consumer(fork_commit_consumer_fn fn) { (void)fn; }

uint32_t xbox_FramebufferPresentSerial(void) { return 0; }
unsigned long long xbox_FramebufferPresentHash(void) { return 0; }

volatile long long g_nv2a_mono_anchor_count;
