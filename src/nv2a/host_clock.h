/* Host performance-counter to nanoseconds, overflow-safe.
 *
 * WHY THIS IS ITS OWN HEADER. The conversion used to be written inline in
 * `qemu_shim.h` and in `apu_shim.h`, and a regression test for it could only
 * duplicate the expression -- which pins a COPY of the formula, not the shipped
 * code. Reverting the shipped conversion would leave such a test green. Making
 * the conversion a named function lets the test call the real thing.
 *
 * THE DEFECT IT EXISTS TO PREVENT. The naive form
 *
 *     (int64_t)(count * 1000000000LL / freq)
 *
 * forms the product in a SIGNED 64-bit temporary. At this host's 10 MHz counter
 * that product exceeds int64 once per 2^63/1e9 counts -- 922.337 s of host
 * uptime -- and the quotient goes negative. The consumer
 * (`ptimer_service_thread`) holds the result in a `uint64_t`, so the negative
 * value becomes a huge positive one and the reading jumps FORWARD to near 2^64.
 * It then climbs normally until the product wraps the full 2^64 at
 * 2^64/1e9 counts = **1844.674 s**, where the uint64 reading jumps BACKWARD to
 * near zero.
 *
 * The backward jump is the harmful one: the vblank deadline is still near 2^64,
 * so neither the pulse test nor the re-arm test can be true and NO pulse is
 * emitted until the reading climbs back. The forward jump is harmless by
 * comparison because it satisfies `now >= next` and re-arms.
 *
 * Splitting the division keeps every intermediate in range: the whole seconds
 * contribute exactly and the sub-second remainder is scaled separately. The
 * result is monotonic for any uptime below ~292 years, which is the
 * representable range of the return type itself.
 *
 * `count` and `freq` are parameters rather than being read inside, so the
 * arithmetic is testable without a clock. */
#ifndef NV2A_HOST_CLOCK_H
#define NV2A_HOST_CLOCK_H

#include <stdint.h>

/* Convert a raw performance-counter reading to nanoseconds.
 *
 * `freq` is the counter frequency (counts per second) and must be non-zero;
 * a zero frequency returns 0 rather than dividing by zero. */
static inline int64_t nv2a_qpc_to_ns(int64_t count, int64_t freq)
{
    if (freq <= 0)
        return 0;
    return (count / freq) * 1000000000LL
         + ((count % freq) * 1000000000LL) / freq;
}

/* The same conversion in microseconds, for the APU clock. Kept beside its
 * sibling so the two cannot drift apart again. */
static inline int64_t nv2a_qpc_to_us(int64_t count, int64_t freq)
{
    if (freq <= 0)
        return 0;
    return (count / freq) * 1000000LL
         + ((count % freq) * 1000000LL) / freq;
}

/* The PRE-FIX expression, kept here so a regression test can assert that it
 * really is harmful. Nothing in the runtime calls this; it exists so the test's
 * control arm exercises the same arithmetic the shipped code used to, rather
 * than a re-typed copy that could drift. */
static inline int64_t nv2a_qpc_to_ns_overflowing(int64_t count, int64_t freq)
{
    if (freq <= 0)
        return 0;
    return (int64_t)(((uint64_t)count * 1000000000ULL)) / freq;
}

#endif /* NV2A_HOST_CLOCK_H */
