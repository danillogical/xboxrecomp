/* Regression: vblank delivery must survive a clock that steps BACKWARD.
 *
 * THE DEFECT THIS PINS. The vblank service loop kept a deadline
 * (`next_vblank_ns`) in the same units as the clock reading (`now_ns`). When
 * the host clock overflowed -- `qemu_clock_get_ns` formed `QPC * 1e9` in a
 * signed 64-bit temporary, which wraps every 1844.67 s of host uptime at this
 * host's 10 MHz counter -- `now_ns` jumped backward from about +9.2e18 to about
 * -9.2e18 while `next_vblank_ns` stayed near +9.2e18. Neither
 * `now >= next` nor `now >= next + 4*frame` was then true, so NO pulse was
 * emitted until the reading climbed back (about 30 minutes), and the computed
 * wait became about 49 days.
 *
 * WHY IT MATTERED. No pulse means no PCRTC interrupt, no ISR, no DPC and no
 * `KeSetEvent` for the ADX middleware's vsync event. Its worker threads block
 * forever, `title.adx` is opened and never read, and the guest holds on the
 * loading screen. Measured per archived run: run 507 lost those workers at
 * t~286 s with the wrap instant 290 s into the run, and every archived run
 * whose window contained a wrap lost them while every run whose window did not
 * kept them.
 *
 * WHAT THIS TEST DRIVES. `nv2a_vblank_advance` is the loop's scheduling rule
 * as a pure function, so this needs no device, no clock and no thread. It
 * steps a clock from just below 2^63 (the signed-overflow point) across the
 * backward jump, and asserts:
 *
 *   1. a normal clock pulses once per frame, one frame apart (so the harness
 *      is capable of observing pulses at all -- a test that cannot see a pulse
 *      proves nothing about losing one);
 *   2. across the backward jump the rule re-arms and RESUMES pulsing within a
 *      few frames, rather than stalling;
 *   3. the deadline never ends up unreachably far ahead of the reading, which
 *      is the state that produced the multi-day sleep.
 *
 * The root-cause fix is in `qemu_clock_get_ns` (src/nv2a/qemu_shim.h) and is
 * pinned by host_clock_wrap_test.c. This test pins the LOOP's defence, so a
 * clock source the model does not control cannot silently stop vblank
 * delivery again.
 */
#include <stdint.h>
#include <stdio.h>
#include "nv2a_state.h"

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

#define FRAME_NS 16666666ull          /* 60 Hz */

/* nv2a_core.c calls into the D3D11 backend on a PGRAPH method. This test
 * exercises the vblank scheduling rule only, so the backend is stubbed the same
 * way the submit-diag test stubs it: the rule under test never reaches it. */
int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param)
{
    (void)subchannel; (void)method; (void)param;
    return 1;
}

int main(void)
{
    /* 1. A healthy clock: one pulse per frame, and the deadline tracks the
     *    reading one frame ahead. This is the positive control -- if the
     *    harness cannot see pulses here, the rest of the test is vacuous. */
    {
        uint64_t next = 0, now = 1000000000ull;
        int pulses = 0, i, pulse;

        for (i = 0; i < 120; ++i) {                 /* two seconds at 60 Hz */
            next = nv2a_vblank_advance(next, now, FRAME_NS, &pulse);
            pulses += pulse;
            now += FRAME_NS;
        }
        /* 120 frames of elapsed time, and the first pass only arms, so the
         * expected count is 119 or 120 depending on phase. */
        CHECK(pulses >= 118 && pulses <= 120,
              "a healthy clock produced %d pulse(s) over 120 frames", pulses);
    }

    /* 2. The backward jump. Step the clock up to just below 2^63, then jump it
     *    to a small value -- exactly what the overflowing expression did. */
    {
        uint64_t now = (1ull << 63) - 2 * FRAME_NS;
        uint64_t next = 0;
        int pulse, i;
        int pulses_before = 0, pulses_after = 0;
        int frames_to_resume = -1;

        for (i = 0; i < 4; ++i) {
            next = nv2a_vblank_advance(next, now, FRAME_NS, &pulse);
            pulses_before += pulse;
            now += FRAME_NS;
        }

        /* The wrap: the reading falls to a small value. */
        now = 5 * FRAME_NS;

        for (i = 0; i < 20; ++i) {
            next = nv2a_vblank_advance(next, now, FRAME_NS, &pulse);
            pulses_after += pulse;
            if (pulse && frames_to_resume < 0)
                frames_to_resume = i;
            now += FRAME_NS;
        }

        CHECK(pulses_before > 0,
              "no pulse was produced BEFORE the jump, so the jump test cannot "
              "show recovery (pulses_before=%d)", pulses_before);
        CHECK(pulses_after > 0,
              "vblank delivery did NOT resume after the clock stepped backward: "
              "%d pulse(s) in the 20 frames after the jump. This is the defect "
              "-- the guest's vblank waiters would block forever.",
              pulses_after);
        CHECK(frames_to_resume <= 2,
              "delivery resumed only after %d frame(s); a re-arm should take at "
              "most one pass", frames_to_resume);

        /* 3. The deadline must be reachable: within a few frames of the
         *    reading, not about 2^64 away. */
        CHECK(next <= now + FRAME_NS * 4,
              "the deadline (%llu) is unreachably far ahead of the reading "
              "(%llu): the service loop would sleep instead of pulsing",
              (unsigned long long)next, (unsigned long long)now);
    }

    /* 3b. The same property from the other direction: a deadline left near
     *     2^63 by a PREVIOUS base must not strand a reading that restarted
     *     near zero. This is the exact state the wrap produced. */
    {
        uint64_t stale_next = (1ull << 63) - FRAME_NS;   /* old base */
        uint64_t fresh_now = 7 * FRAME_NS;               /* new base */
        int pulse, i, pulses = 0;

        for (i = 0; i < 10; ++i) {
            stale_next = nv2a_vblank_advance(stale_next, fresh_now, FRAME_NS,
                                             &pulse);
            pulses += pulse;
            fresh_now += FRAME_NS;
        }
        CHECK(pulses > 0,
              "a deadline from a previous time base stranded the new reading: "
              "%d pulse(s) in 10 frames", pulses);
        CHECK(stale_next <= fresh_now + FRAME_NS * 4,
              "the deadline stayed in the old base (%llu vs reading %llu)",
              (unsigned long long)stale_next, (unsigned long long)fresh_now);
    }

    /* 4. A zero frame period must not divide by zero or spin: the model falls
     *    back to 60 Hz before calling, but the rule must be total. */
    {
        uint64_t next = nv2a_vblank_advance(0, 1000, 0, NULL);
        CHECK(next == 0, "a zero frame period changed the deadline to %llu",
              (unsigned long long)next);
        /* A NULL pulse pointer must be tolerated (the loop passes a real one,
         * but a pure function should not fault on a query). */
        next = nv2a_vblank_advance(0, 1000, FRAME_NS, NULL);
        CHECK(next == 1000 + FRAME_NS,
              "a NULL pulse pointer produced deadline %llu, want %llu",
              (unsigned long long)next,
              (unsigned long long)(1000 + FRAME_NS));
    }

    if (g_failures) {
        fprintf(stderr, "vblank_clock_step_test: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "vblank_clock_step_test: all checks passed\n");
    return 0;
}
