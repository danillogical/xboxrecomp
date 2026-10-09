/* Regression: `nv2a_mono_now_ns` must share ONE epoch across translation units.
 *
 * THE DEFECT THIS PINS. The reader is `static inline` in a header, so an anchor
 * held in a function-local static gives every .c file that calls it its own
 * copy and its own zero; the anchor must be one process-wide definition.
 * The header and kernel_bridge.c promise one process-wide epoch, and the
 * telemetry from kernel_bridge.c and nv2a_mmio_hook.c is correlated on it.
 *
 * WHY TWO FILES. A single translation unit cannot see the defect, because it
 * only ever has one copy of the anchor.
 * File B (nv2a_mono_clock_test_b.c) makes the first call in the process, this
 * file waits, and then its own first reading must already include that wait.
 * With per-file anchors this file's first reading is ~0 instead.
 *
 * WHAT IT ASSERTS:
 *   1. file A's first reading is at least 40 ms after B anchored (50 ms slept);
 *   2. readings alternating between A and B never go backward;
 *   3. the millisecond accessor agrees with the nanosecond reading. */
#include <stdint.h>
#include <stdio.h>

#include "platform/xbox_winnt.h"
#include "nv2a/nv2a_mono_clock.h"

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <time.h>
static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#define SLEEP_MS(ms) sleep_ms(ms)
#endif

int64_t mono_clock_test_b_now_ns(void);

static int g_failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            ++g_failures;                                                   \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

int main(void)
{
    int64_t b0, a0, prev, cur;
    uint32_t ms;
    int64_t ns;
    int i, ms_ok;

    /* B makes the first call in the process, which fixes the zero. */
    b0 = mono_clock_test_b_now_ns();
    SLEEP_MS(60);

    a0 = nv2a_mono_now_ns();
    CHECK(a0 >= 40000000LL,
          "file A's first reading is %lld ns; expected >= 40000000 (shared epoch)",
          (long long)a0);
    CHECK(a0 >= b0, "A's first reading %lld < B's %lld", (long long)a0, (long long)b0);
    printf("%s first A reading after B anchored: %lld ns\n",
           a0 >= 40000000LL ? "PASS" : "FAIL", (long long)a0);

    prev = a0;
    for (i = 0; i < 8; ++i) {
        cur = (i & 1) ? nv2a_mono_now_ns() : mono_clock_test_b_now_ns();
        CHECK(cur >= prev, "reading %d went backward: %lld < %lld", i,
              (long long)cur, (long long)prev);
        prev = cur;
    }
    printf("%s monotonic across files (last %lld ns)\n",
           g_failures ? "FAIL" : "PASS", (long long)prev);

    ns = nv2a_mono_now_ns();
    ms = nv2a_mono_now_ms();
    /* The ms read comes after the ns read, so allow 1 ms below and 6 ms above. */
    ms_ok = (int64_t)ms * 1000000LL >= ns - 1000000LL &&
            (int64_t)ms * 1000000LL <= ns + 6000000LL;
    CHECK(ms_ok, "ms=%u disagrees with ns=%lld", ms, (long long)ns);
    printf("%s ms accessor agrees with ns (ms=%u ns=%lld)\n",
           ms_ok ? "PASS" : "FAIL", ms, (long long)ns);

    if (g_failures) {
        printf("nv2a_mono_clock_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("nv2a_mono_clock_test: all checks passed\n");
    return 0;
}
