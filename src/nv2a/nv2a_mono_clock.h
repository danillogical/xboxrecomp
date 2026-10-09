/* Process-start-relative MONOTONIC host clock, for correlated event telemetry.
 *
 * WHY THIS IS SEPARATE FROM host_clock.h. `host_clock.h` holds the pure
 * arithmetic (count, freq) -> ns, and is deliberately includable by a test that
 * links no Windows headers and touches no clock. This header is the impure
 * half: it reads QueryPerformanceCounter and fixes a zero. Keeping them apart
 * is what lets `host_clock_wrap_test` keep calling the REAL shipped conversion
 * without dragging in the platform layer.
 *
 * THE EPOCH PROBLEM IT SOLVES. A run has three clocks and they do not share a
 * zero:
 *
 *   - the QPC product in `host_clock.h` is HOST-UPTIME-relative (QPC counts
 *     from boot), so its wrap instants are uptime events;
 *   - `[CHECKPOINT] ms=` is GetTickCount64, also boot-relative but a different
 *     clock (measured +4.202 s apart from QPC on the development host);
 *   - `[FBPRESENT] t=Ns` is GetTickCount() minus the tick at the FIRST PRESENT
 *     (`fb_present.c`), i.e. relative to the first present, not the process.
 *
 * Comparing any two of those without converting them to a common base produced
 * a causal claim about the clock wrap and worker loss that had to be withdrawn.
 * Worse, the obvious candidate for a common base does not work: a whole 1800 s
 * archived run carries only THREE `[CHECKPOINT]` lines, all inside its first 78
 * lines, so there is no per-phase checkpoint to hang a timeline on.
 *
 * So the fix is to create the missing base: one anchor taken at first use, and
 * every subsequent reading a monotonic nanosecond count from that anchor. Any
 * two events stamped with this share an epoch, which is the property the
 * earlier analysis lacked.
 *
 * The anchor is captured lazily because the first caller may be a worker thread
 * or the ptimer thread, and a static initializer cannot portably call
 * QueryPerformanceCounter. Whichever thread arrives first fixes the zero; that
 * moment is "process start" for telemetry purposes. */
#ifndef NV2A_MONO_CLOCK_H
#define NV2A_MONO_CLOCK_H

#include <stdint.h>

#include "platform/xbox_winnt.h"
#include "nv2a/host_clock.h"

/* Nanoseconds since the first call to this function anywhere in the process.
 * Never negative: a reading taken before the anchor was fixed clamps to 0.
 * Returns 0 if the host reports a non-positive frequency (no supported host
 * does). */
static inline int64_t nv2a_mono_now_ns(void)
{
    /* The anchor QPC count; 0 means not yet anchored. */
    static volatile LONGLONG s_anchor_count;
    LARGE_INTEGER freq, count;
    LONGLONG anchor, delta;

    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart <= 0)
        return 0;
    QueryPerformanceCounter(&count);

    /* First writer wins, through one compare-exchange rather than a lock: a
     * lock here would sit inside the owner-lock telemetry that measures lock
     * contention, the perturbation this instrument must avoid. A losing
     * thread reads back the winner's anchor, so every thread shares one zero. */
    anchor = InterlockedCompareExchange64(&s_anchor_count, 0, 0);
    if (anchor == 0) {
        LONGLONG prior = InterlockedCompareExchange64(&s_anchor_count,
                                                      count.QuadPart, 0);
        anchor = prior ? prior : count.QuadPart;
    }
    /* This thread's QPC read can predate the winner's anchor; a negative
     * delta would become a huge unsigned value in the callers' maxima. */
    delta = count.QuadPart - anchor;
    if (delta < 0)
        delta = 0;
    return nv2a_qpc_to_ns(delta, freq.QuadPart);
}

/* Milliseconds, for the exported 32-bit telemetry fields. Truncating a
 * process-relative millisecond count to 32 bits wraps after ~49.7 days, which
 * exceeds any run, so differences within one run are exact. */
static inline uint32_t nv2a_mono_now_ms(void)
{
    return (uint32_t)((uint64_t)nv2a_mono_now_ns() / 1000000ull);
}

#endif /* NV2A_MONO_CLOCK_H */
