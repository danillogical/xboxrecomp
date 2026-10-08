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
 * Monotonic; never goes backward, because it is a difference of QPC readings
 * converted by the overflow-safe split division. Returns 0 if the host reports
 * a non-positive frequency (no supported host does). */
static inline int64_t nv2a_mono_now_ns(void)
{
    static int64_t s_anchor_count;
    static int64_t s_freq;
    static volatile long s_anchored;   /* 0 = unanchored, 1 = anchored */
    LARGE_INTEGER freq, count;

    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart <= 0)
        return 0;
    QueryPerformanceCounter(&count);

    if (!s_anchored) {
        /* Benign race. Two threads may both store the anchor, and the
         * difference between the two candidates is one QPC read. Taking a lock
         * here would put a lock acquisition inside the owner-lock telemetry
         * that measures lock contention, which is exactly the perturbation the
         * instrument must avoid. */
        s_freq = freq.QuadPart;
        s_anchor_count = count.QuadPart;
        s_anchored = 1;
    }
    return nv2a_qpc_to_ns(count.QuadPart - s_anchor_count, s_freq);
}

/* Milliseconds, for the exported 32-bit telemetry fields. Truncating a
 * process-relative millisecond count to 32 bits wraps after ~49.7 days, which
 * exceeds any run, so differences within one run are exact. */
static inline uint32_t nv2a_mono_now_ms(void)
{
    return (uint32_t)((uint64_t)nv2a_mono_now_ns() / 1000000ull);
}

#endif /* NV2A_MONO_CLOCK_H */
