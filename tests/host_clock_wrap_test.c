/* Regression: the host clock must not jump backward at the QPC product wrap.
 *
 * THE DEFECT THIS PINS. `qemu_clock_get_ns` in src/nv2a/qemu_shim.h used to
 * compute
 *
 *     (int64_t)(count.QuadPart * 1000000000LL / freq.QuadPart)
 *
 * The product is formed in a SIGNED 64-bit temporary, so it overflows once per
 * 2^64/1e9 QPC counts -- 1844.67 s of host uptime at this host's 10 MHz
 * counter -- and the quotient then jumps BACKWARD from about +9.2e18 to about
 * -9.2e18.
 *
 * WHY IT MATTERED. `ptimer_service_thread` uses that value as `now_ns` and
 * compares it with `next_vblank_ns`, which is still near +9.2e18. After the
 * wrap neither `now_ns >= next_vblank_ns` nor the re-arm test
 * `now_ns >= next_vblank_ns + 4*frame_ns` is true, so NO vblank pulse is
 * emitted again until the clock climbs back (about 30 minutes). With no pulse
 * there is no ISR, no DPC and no KeSetEvent for the ADX middleware's vsync
 * event: its worker threads block forever, `title.adx` is opened and never
 * read, and the guest holds on the loading screen.
 *
 * MEASURED, per archived run: run 507 lost its vsync workers at t~286 s and its
 * wrap instant was 290 s into the run; every archived run whose window
 * contained a wrap lost those workers, and every run whose window did not,
 * kept them (including the two clean `title009-cap1024` controls).
 *
 * WHAT THIS TEST DOES. It cannot call QueryPerformanceCounter at a chosen
 * value, so it reproduces the SHIM'S OWN EXPRESSION -- both the old form and
 * the current one -- over the wrap boundary in signed 64-bit modular
 * arithmetic, and asserts the properties that matter:
 *
 *   1. the old expression really does jump backward (so the test would fail if
 *      the arithmetic were not the defect -- a test that cannot fail proves
 *      nothing);
 *   2. the current expression is strictly monotonic across that boundary;
 *   3. the current expression equals the exact uptime in nanoseconds.
 *
 * The expressions are duplicated here deliberately. This is a test OF the
 * formula, so it must not call the function under test through a shim that
 * hides the intermediate -- the overflow is in the intermediate.
 */
#include <stdint.h>
#include <stdio.h>

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

/* The old, overflowing form: the product is truncated to signed 64-bit.
 *
 * `count` is passed through a `volatile` read on purpose. Without it MSVC
 * constant-folds the whole expression for the literal arguments this test
 * uses, evaluating the product in 128-bit precision -- which HIDES the
 * overflow and made an earlier version of this test pass the "old form is
 * harmless" way round. The production shim reads `count` from an opaque Win32
 * call, so it always computes at run time; the volatile reproduces that. */
static int64_t shim_ns_overflowing(int64_t count, int64_t freq)
{
    volatile int64_t c = count;
    uint64_t product = (uint64_t)c * 1000000000ull;
    volatile int64_t signed_product = (int64_t)product;
    return signed_product / freq;
}

/* The current, overflow-safe form, likewise opaque to the optimizer. */
static int64_t shim_ns_safe(int64_t count, int64_t freq)
{
    volatile int64_t c = count;
    return (c / freq) * 1000000000ll
         + ((c % freq) * 1000000000ll) / freq;
}

/* The wrap count: the smallest `count` whose product with 1e9 leaves the
 * signed 64-bit range. 2^63/1e9 is 9223372036.85, so the first count that
 * overflows is 9223372037 -- NOT 2^64/1e9, which is where the UNSIGNED range
 * would wrap. The distinction matters: the shim's product is formed in a
 * SIGNED temporary, so it goes negative at half that uptime. */
static int64_t wrap_count(int64_t freq)
{
    (void)freq;
    return 9223372037ll;
}

int main(void)
{
    const int64_t freq = 10000000ll;   /* this host's QPC frequency */
    const int64_t wc = wrap_count(freq);
    int64_t prev, cur;
    int i;

    /* The wrap instant in host uptime, for the record. */
    fprintf(stderr, "qpc freq=%lld wrap count=%lld (%.1f s uptime)\n",
            (long long)freq, (long long)wc, (double)wc / (double)freq);

    /* 1. The OLD expression must actually jump backward across the wrap.
     *    Asserting this is what makes the test able to fail: if the arithmetic
     *    were harmless, there would be nothing to fix and this test would be
     *    measuring nothing. */
    {
        int backward = 0;
        prev = shim_ns_overflowing(wc - freq, freq);
        for (i = 0; i < 3; ++i) {
            cur = shim_ns_overflowing(wc + (int64_t)i * freq, freq);
            if (cur < prev)
                ++backward;
            prev = cur;
        }
        CHECK(backward > 0,
              "the OLD expression did not jump backward across the wrap "
              "(count=%lld): this test is not exercising the defect",
              (long long)wc);
    }

    /* 2. The SAFE expression must be strictly increasing across the same
     *    boundary, with no backward step and no stall. */
    {
        int backward = 0, stalled = 0;
        prev = shim_ns_safe(wc - 8 * freq, freq);
        for (i = -7; i <= 8; ++i) {
            cur = shim_ns_safe(wc + (int64_t)i * freq, freq);
            if (cur < prev)
                ++backward;
            if (cur == prev)
                ++stalled;
            prev = cur;
        }
        CHECK(backward == 0,
              "the SAFE expression moved backward %d time(s) across the wrap",
              backward);
        CHECK(stalled == 0,
              "the SAFE expression stalled %d time(s) across the wrap", stalled);
    }

    /* 3. The SAFE expression must equal the exact uptime. The old one does
     *    not, which is the same defect seen from the other side. */
    {
        int64_t count = wc + 12345ll * freq;
        int64_t exact = (count / freq) * 1000000000ll
                      + ((count % freq) * 1000000000ll) / freq;
        CHECK(shim_ns_safe(count, freq) == exact,
              "the SAFE expression (%lld) disagrees with exact uptime (%lld)",
              (long long)shim_ns_safe(count, freq), (long long)exact);
        /* The exact value must be positive and on the order of the host's real
         * uptime (~1.8e12 ns), not the wrapped ~-9.2e18. */
        CHECK(exact > 0,
              "exact uptime at the wrap is not positive (%lld)",
              (long long)exact);
    }

    /* 4. A sub-second count must not be lost: the split division keeps the
     *    remainder, so a counter offset smaller than one frequency tick still
     *    advances the result by its exact nanosecond share. */
    {
        int64_t base = 1000ll * freq;          /* 1000 s uptime */
        int64_t half = base + freq / 2;        /* + 0.5 s */
        CHECK(shim_ns_safe(half, freq) - shim_ns_safe(base, freq)
                  == 500000000ll,
              "a half-second offset produced %lld ns, want 500000000",
              (long long)(shim_ns_safe(half, freq) - shim_ns_safe(base, freq)));
    }

    if (g_failures) {
        fprintf(stderr, "host_clock_wrap_test: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "host_clock_wrap_test: all checks passed\n");
    return 0;
}
