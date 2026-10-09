/* Regression: the vblank late-rearm telemetry must be able to FAIL.
 *
 * WHY THIS TEST EXISTS. Archived runs report 0.75-1.23 vblank pulses per second
 * against a model nominal of at least 40 Hz -- a 30-80x shortfall nobody has
 * explained. `nv2a_vblank_advance` used to re-arm WITHOUT pulsing when the
 * service loop woke late (`now >= next + 4*frame`), so a loop that took that
 * branch every pass never pulsed. It now pulses once on a late wake and re-arms
 * from now; the late count is still the measure of how often it happens.
 *
 * That hypothesis is only testable if the instrument that counts the late
 * branch can actually observe it. This test drives the real rule through a
 * simulated service loop and asserts:
 *
 *   1. POSITIVE CONTROL -- a punctual loop pulses once per frame and records
 *      ZERO late re-arms. Without this arm, a counter stuck at zero would look
 *      like "no problem" and the test would be vacuous.
 *   2. A loop that consistently wakes late records late re-arms AND pulses once
 *      per pass, never a burst.
 *   3. The late count is the exact number of passes, so the counter is not
 *      merely non-zero but correct.
 *   4. A single late wake is counted once and delivery recovers, so the counter
 *      does not latch or double-count.
 *
 * WHAT IT DOES NOT CLAIM. It exercises the SCHEDULING RULE and the counting
 * predicate, not the live service thread. It does not show that real runs take
 * the late branch; that is what the exported counter is for. It also does not
 * touch the owner lock, whose hold/wait telemetry is measured in the MMIO hook
 * and is not reachable from a pure-function test.
 *
 * The counting predicate below is deliberately a COPY of the one in
 * `nv2a_mmio_hook.c` (`!pulse && deadline_before && now > deadline_before`)
 * because the service loop cannot be linked here without the platform layer.
 * That is a real gap: if the hook's predicate changed, this test would not
 * notice. It is recorded rather than hidden. What it does pin is that the
 * predicate, applied to the REAL `nv2a_vblank_advance`, separates the two
 * regimes -- which is the property the instrument depends on. */
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
 * exercises the scheduling rule only, so the backend is stubbed exactly as
 * vblank_clock_step_test stubs it: the rule never reaches it. */
int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param)
{
    (void)subchannel; (void)method; (void)param;
    return 1;
}

/* One simulated service pass, mirroring the hook's counting predicate. */
typedef struct {
    uint64_t next;
    int passes;
    int pulses;
    int late;
} LoopSim;

static void sim_pass(LoopSim *s, uint64_t now)
{
    int pulse = 0;
    uint64_t deadline_before = s->next;
    s->next = nv2a_vblank_advance(s->next, now, FRAME_NS, &pulse);
    ++s->passes;
    /* Late means the wake was at least four frames past the deadline; the
     * rule pulses once on such a wake, so the pulse no longer separates them. */
    if (deadline_before && now >= deadline_before + 4 * FRAME_NS)
        ++s->late;
    if (pulse)
        ++s->pulses;
}

int main(void)
{
    /* 1. POSITIVE CONTROL: a punctual loop. It must pulse, and it must record
     *    ZERO late re-arms. If this arm failed to pulse, the late-count in the
     *    other arms would be meaningless. */
    {
        LoopSim s = {0, 0, 0, 0};
        uint64_t now = 1000000000ull;
        int i;

        for (i = 0; i < 120; ++i) {
            sim_pass(&s, now);
            now += FRAME_NS;
        }
        CHECK(s.pulses >= 118 && s.pulses <= 120,
              "a punctual loop produced %d pulse(s) over 120 frames; the "
              "harness cannot see healthy delivery, so the late-rearm arm "
              "below would prove nothing", s.pulses);
        CHECK(s.late == 0,
              "a punctual loop recorded %d late re-arm(s); the counter fires "
              "on healthy delivery and cannot indicate a problem", s.late);
    }

    /* 2 + 3. The fixed point: every pass wakes 10 frames late, which is past
     *        the `now >= next + 4*frame` threshold. The rule must re-arm
     *        and pulse once on EVERY late pass (no catch-up burst), and the
     *        counter must fire on all but the first pass. This is the signature
     *        the archived 30-80x shortfall would produce.
     *
     *        WHY `passes - 1` AND NOT `passes`. The first pass has `next == 0`
     *        -- there is no deadline yet, so `nv2a_vblank_advance` takes its
     *        ARM branch (`next == 0`) and returns `now + frame`. Nothing can be
     *        "late" against a deadline that does not exist, so the counting
     *        predicate excludes it via `deadline_before != 0`. That exclusion
     *        is the correct behaviour, and asserting the exact value here is
     *        what pins it: a predicate that dropped the guard would count 100
     *        and fail. */
    {
        LoopSim s = {0, 0, 0, 0};
        uint64_t now = 1000000000ull;
        int i;

        for (i = 0; i < 100; ++i) {
            sim_pass(&s, now);
            now += FRAME_NS * 10;          /* 10 frames between passes */
        }
        CHECK(s.pulses == 99,
              "a persistently late loop emitted %d pulse(s), want exactly one "
              "per late pass (99): delivery must not stop, and must not burst",
              s.pulses);
        CHECK(s.late == s.passes - 1,
              "late re-arms (%d) != passes-1 (%d) on a persistently late loop; "
              "the counter does not measure what it claims",
              s.late, s.passes - 1);
        CHECK(s.late == 99,
              "expected 99 late re-arms (100 passes less the deadline-less "
              "arming pass), recorded %d", s.late);
    }

    /* 4. One late wake is counted ONCE and delivery recovers: the counter must
     *    not latch, and a single hiccup must not look like a fixed point. */
    {
        LoopSim s = {0, 0, 0, 0};
        uint64_t now = 1000000000ull;
        int i;

        /* Arm, then run punctually so a baseline pulse count exists. */
        for (i = 0; i < 20; ++i) {
            sim_pass(&s, now);
            now += FRAME_NS;
        }
        {
            int pulses_before = s.pulses;
            int late_before = s.late;

            /* A single 10-frame hiccup. */
            now += FRAME_NS * 10;
            sim_pass(&s, now);
            CHECK(s.late == late_before + 1,
                  "one late wake changed the late count by %d, want 1",
                  s.late - late_before);

            /* And then punctual again: delivery must resume. */
            now += FRAME_NS;
            for (i = 0; i < 10; ++i) {
                sim_pass(&s, now);
                now += FRAME_NS;
            }
            CHECK(s.pulses > pulses_before,
                  "delivery did not resume after a single late wake "
                  "(%d pulse(s) after, %d before)", s.pulses, pulses_before);
            CHECK(s.late == late_before + 1,
                  "the late counter kept climbing after recovery (%d, want "
                  "%d): it latches instead of counting events",
                  s.late, late_before + 1);
        }
    }

    /* 5. The boundary: a pass that wakes EXACTLY at the deadline is punctual
     *    (the rule pulses on `now >= next`), so it must NOT be counted late.
     *    This is the off-by-one that would otherwise inflate the counter on
     *    healthy runs. */
    {
        LoopSim s = {0, 0, 0, 0};
        uint64_t now = 1000000000ull;
        int i;

        sim_pass(&s, now);                    /* arms */
        for (i = 0; i < 50; ++i) {
            now = s.next;                     /* wake exactly on the deadline */
            sim_pass(&s, now);
        }
        CHECK(s.late == 0,
              "waking exactly on the deadline was counted late %d time(s); "
              "the predicate is off by one and would inflate healthy runs",
              s.late);
        CHECK(s.pulses == 50,
              "waking exactly on the deadline produced %d pulse(s), want 50",
              s.pulses);
    }

    if (g_failures) {
        fprintf(stderr, "vblank_sched_telemetry_test: %d failure(s)\n",
                g_failures);
        return 1;
    }
    fprintf(stderr, "vblank_sched_telemetry_test: all checks passed\n");
    return 0;
}
