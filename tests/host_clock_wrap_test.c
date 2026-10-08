/* Regression: the host clock conversion must not jump BACKWARD at either wrap.
 *
 * THE DEFECT THIS PINS. `qemu_clock_get_ns` used to compute
 *
 *     (int64_t)(count.QuadPart * 1000000000LL / freq.QuadPart)
 *
 * forming the product in a SIGNED 64-bit temporary. At this host's 10 MHz
 * counter that product exceeds int64 once per 2^63/1e9 counts -- 922.337 s of
 * host uptime -- and the quotient goes negative. The consumer holds the result
 * in a `uint64_t`, so the negative value becomes a huge positive one: the
 * reading jumps FORWARD to near 2^64. It then climbs until the product wraps
 * the full 2^64 at 2^64/1e9 counts = **1844.674 s**, where the reading jumps
 * BACKWARD to near zero.
 *
 * The backward jump is the harmful one. `ptimer_service_thread` keeps its
 * vblank deadline (`next_vblank_ns`) in the same units, so a reading that
 * restarts near zero while the deadline sits near 2^64 satisfies neither the
 * pulse test nor the re-arm test: NO vblank pulse is emitted until the reading
 * climbs back, and the computed wait becomes enormous. No pulse means no PCRTC
 * interrupt, no ISR, no DPC and no `KeSetEvent` for a vblank waiter.
 *
 * WHAT THIS TEST CALLS. `nv2a_qpc_to_ns` / `nv2a_qpc_to_us` from
 * `src/nv2a/host_clock.h` are the REAL shipped conversions -- the same
 * functions `qemu_clock_get_ns` and `qemu_clock_get_us` return. An earlier
 * version of this test duplicated the expression instead, which pinned a copy
 * of the formula rather than the shipped code: reverting the shim would have
 * left it green. The pre-fix expression is also in that header, as
 * `nv2a_qpc_to_ns_overflowing`, so the control arm exercises the same
 * arithmetic the runtime used to run rather than a re-typed copy.
 *
 * `count` is passed through a `volatile` read in the helpers below. Without it
 * MSVC constant-folds the literal arguments in 128-bit precision and HIDES the
 * overflow -- which is how an earlier version of this test passed the wrong way
 * round.
 *
 * RESIDUAL GAP, stated rather than left implicit: this translation unit compiles
 * `host_clock_wrap_test.c` alone, so it pins `host_clock.h` and NOT
 * `qemu_shim.h`. A shim that stopped calling `nv2a_qpc_to_ns` (and re-inlined a
 * naive expression) would not be caught here. Closing that would need the test
 * to link the shim, which drags in the Windows headers and the QPC call for no
 * additional arithmetic coverage; the gap is accepted and recorded instead.
 *
 * WHAT IT ASSERTS:
 *   1. the pre-fix expression really does jump backward (so the test would
 *      fail if the arithmetic were not the defect);
 *   2. the shipped conversion is monotonic across BOTH wrap points;
 *   3. the shipped conversion equals exact uptime;
 *   4. the forward (signed) wrap, which the old form also produced, is NOT
 *      backward -- recorded so the two wrap points are not conflated.
 */
#include <stdint.h>
#include <stdio.h>
#include "host_clock.h"

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

#define FREQ 10000000LL                      /* this host's QPC frequency */

/* The two wrap counts, as exact literals.
 *
 * These cannot be written as `(1ULL << 63) / 1000000000ULL` in a macro: the
 * shift itself is the value whose product overflows, and folding it through
 * integer division at compile time silently yields a different count (measured:
 * it produced 1 for the unsigned wrap, which made the test pass vacuously).
 * The literals are 2^63/1e9 and 2^64/1e9 rounded up, and they are asserted
 * against the arithmetic below rather than trusted. */
#define SIGNED_WRAP   9223372037LL            /* 922.337 s uptime */
#define UNSIGNED_WRAP 18446744074LL           /* 1844.674 s uptime */

/* The pre-fix expression, evaluated opaquely so the optimizer cannot fold it. */
static int64_t old_ns(int64_t count)
{
    volatile int64_t c = count;
    return nv2a_qpc_to_ns_overflowing(c, FREQ);
}

/* The shipped conversion, likewise opaque. */
static int64_t new_ns(int64_t count)
{
    volatile int64_t c = count;
    return nv2a_qpc_to_ns(c, FREQ);
}

/* What the CONSUMER sees. `ptimer_service_thread` assigns the returned int64 to
 * a `uint64_t now_ns`, so a negative result becomes a huge positive one. The
 * distinction is the whole point: the signed wrap is a FORWARD jump in this
 * view and the unsigned wrap is the BACKWARD one. */
static uint64_t old_u64(int64_t count) { return (uint64_t)old_ns(count); }
static uint64_t new_u64(int64_t count) { return (uint64_t)new_ns(count); }

int main(void)
{
    int i;

    /* The literals must be the first counts whose products leave range, or the
     * test would be measuring the wrong instant. Checked, not assumed.
     *
     * The product is computed modulo 2^64 (which is what the machine does), so
     * the test is that the UNSIGNED wrap is where that product turns over from
     * "near 2^64" to "near 0". */
    {
        uint64_t s = (uint64_t)SIGNED_WRAP;
        uint64_t u = (uint64_t)UNSIGNED_WRAP;
        CHECK(s * 1000000000ULL > (1ULL << 63),
              "SIGNED_WRAP (%lld) is not past the signed overflow point",
              (long long)SIGNED_WRAP);
        CHECK((s - 1) * 1000000000ULL <= (1ULL << 63),
              "SIGNED_WRAP (%lld) is not the FIRST count past the signed point",
              (long long)SIGNED_WRAP);
        /* Just below the unsigned wrap the product is near 2^64; at it, near 0. */
        CHECK((u - 1) * 1000000000ULL > (1ULL << 63),
              "the count before UNSIGNED_WRAP (%lld) is not near the top of the "
              "64-bit range", (long long)UNSIGNED_WRAP);
        CHECK(u * 1000000000ULL < 1000000000ULL,
              "UNSIGNED_WRAP (%lld) is not the count where the product turns "
              "over to near zero", (long long)UNSIGNED_WRAP);
    }

    fprintf(stderr, "qpc freq=%lld signed wrap=%lld (%.3f s) "
            "unsigned wrap=%lld (%.3f s)\n",
            (long long)FREQ, (long long)SIGNED_WRAP, (double)SIGNED_WRAP / FREQ,
            (long long)UNSIGNED_WRAP, (double)UNSIGNED_WRAP / FREQ);

    /* 1. The pre-fix form must actually be harmful across the UNSIGNED wrap, in
     *    the view the consumer has. Asserting this is what makes the test able
     *    to fail. */
    {
        int backward = 0;
        uint64_t prev = old_u64(UNSIGNED_WRAP - FREQ);
        for (i = 0; i < 3; ++i) {
            uint64_t cur = old_u64(UNSIGNED_WRAP + (int64_t)i * FREQ);
            if (cur < prev)
                ++backward;
            prev = cur;
        }
        CHECK(backward > 0,
              "the PRE-FIX expression did not jump backward across the unsigned "
              "wrap (count=%lld) in the consumer's uint64 view: this test is not "
              "exercising the defect", (long long)UNSIGNED_WRAP);
    }

    /* 1b. Across the SIGNED wrap the same expression jumps FORWARD in the
     *     consumer's view (the negative int64 becomes a huge uint64), which is
     *     harmless because it satisfies `now >= next` and re-arms. Recorded
     *     because the two wrap points are easy to conflate -- and an earlier
     *     version of this test did conflate them. */
    {
        uint64_t before = old_u64(SIGNED_WRAP - FREQ);
        uint64_t after = old_u64(SIGNED_WRAP + FREQ);
        CHECK(after > before,
              "the PRE-FIX expression did not jump FORWARD across the signed "
              "wrap in the consumer's view (%llu -> %llu); the two wrap points "
              "are being conflated",
              (unsigned long long)before, (unsigned long long)after);
        CHECK(old_ns(SIGNED_WRAP + FREQ) < 0,
              "the PRE-FIX expression is not negative past the signed wrap, so "
              "the consumer's forward jump is not being exercised");
    }

    /* 2. The SHIPPED conversion must be monotonic across both wrap points, in
     *    the consumer's view. */
    {
        const int64_t wraps[2] = { SIGNED_WRAP, UNSIGNED_WRAP };
        int w;
        for (w = 0; w < 2; ++w) {
            int backward = 0, stalled = 0;
            uint64_t prev = new_u64(wraps[w] - 8 * FREQ);
            for (i = -7; i <= 8; ++i) {
                uint64_t cur = new_u64(wraps[w] + (int64_t)i * FREQ);
                if (cur < prev)
                    ++backward;
                if (cur == prev)
                    ++stalled;
                prev = cur;
            }
            CHECK(backward == 0,
                  "the SHIPPED conversion moved backward %d time(s) across the "
                  "%s wrap (count=%lld)", backward,
                  w == 0 ? "signed" : "unsigned", (long long)wraps[w]);
            CHECK(stalled == 0,
                  "the SHIPPED conversion stalled %d time(s) across the %s wrap",
                  stalled, w == 0 ? "signed" : "unsigned");
        }
    }

    /* 3. The SHIPPED conversion must equal exact uptime. */
    {
        int64_t count = UNSIGNED_WRAP + 12345LL * FREQ;
        int64_t exact = (count / FREQ) * 1000000000LL
                      + ((count % FREQ) * 1000000000LL) / FREQ;
        CHECK(new_ns(count) == exact,
              "the SHIPPED conversion (%lld) disagrees with exact uptime (%lld)",
              (long long)new_ns(count), (long long)exact);
        CHECK(exact > 0,
              "exact uptime at the wrap is not positive (%lld)", (long long)exact);
    }

    /* 4. The microseconds conversion must be monotonic at ITS wrap, which is
     *    2^63/1e6 counts = 10.68 days of uptime -- a different instant from
     *    the ns one, so a single test of the ns path would not cover it. */
    {
        const int64_t us_wrap = (int64_t)((1ULL << 63) / 1000000ULL) + 1;
        int backward = 0;
        int64_t prev = nv2a_qpc_to_us(us_wrap - 4 * FREQ, FREQ);
        for (i = -3; i <= 4; ++i) {
            int64_t cur = nv2a_qpc_to_us(us_wrap + (int64_t)i * FREQ, FREQ);
            if (cur < prev)
                ++backward;
            prev = cur;
        }
        CHECK(backward == 0,
              "the SHIPPED microsecond conversion moved backward %d time(s) "
              "across its wrap (count=%lld)", backward, (long long)us_wrap);
    }

    /* 5. A sub-second offset must not be lost, and a zero frequency must not
     *    divide by zero. */
    {
        int64_t base = 1000LL * FREQ;
        CHECK(new_ns(base + FREQ / 2) - new_ns(base) == 500000000LL,
              "a half-second offset produced %lld ns, want 500000000",
              (long long)(new_ns(base + FREQ / 2) - new_ns(base)));
        CHECK(nv2a_qpc_to_ns(12345, 0) == 0,
              "a zero frequency did not return 0");
    }

    if (g_failures) {
        fprintf(stderr, "host_clock_wrap_test: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "host_clock_wrap_test: all checks passed\n");
    return 0;
}
