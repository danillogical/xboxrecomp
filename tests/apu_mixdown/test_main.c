/* The monitor mixdown must mix down every bin the guest routed to, not the
 * first two. See README.md for the measurement that found this.
 *
 * It calls mcpx_apu_monitor_mixdown, which mcpx_apu_dsp_frame hands the VP's
 * mixbins to. The frame function itself first copies the mixbins into GP DSP
 * memory, which needs a running GP DSP this test does not build.
 *
 * Run with: ctest --test-dir <build>   (two arms; see CMakeLists.txt) */
#include "apu_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The bins a title's 3D positional voices actually land in, from JSRF's own
 * V0BIN..V3BIN: the 3D front pair 6/7, the rear pair 8/9 and the reverb send
 * 10. A mixdown that preserves the guest's stereo pairing puts signal on both
 * channels. */
#define EFFECT_BIN_LO 6
#define EFFECT_BIN_HI 10

int main(void)
{
    const char *expect_env = getenv("APU_MIXDOWN_EXPECT");
    const int expect_heard = expect_env ? atoi(expect_env) : 1;

    /* Zeroed is deliberately enough: monitor.point 0 is MON_AC97, which is not
     * MON_VP, so the mixdown runs. */
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    int i, b, nonzero_left = 0, nonzero_right = 0;

    if (!d) { fprintf(stderr, "out of memory\n"); return 2; }

    /* Signal ONLY above bin 1. Bins 0 and 1 stay silent, so anything that
     * arrives can only have come from the wider mixdown. */
    for (b = EFFECT_BIN_LO; b <= EFFECT_BIN_HI; ++b)
        for (i = 0; i < NUM_SAMPLES_PER_FRAME; ++i)
            mixbins[b][i] = 0.5f;

    mcpx_apu_monitor_mixdown(d, mixbins);

    for (i = 0; i < NUM_SAMPLES_PER_FRAME; ++i) {
        if (d->monitor.frame_buf[i][0]) nonzero_left = 1;
        if (d->monitor.frame_buf[i][1]) nonzero_right = 1;
    }

    if (expect_heard) {
        if (!nonzero_left || !nonzero_right) {
            fprintf(stderr,
                    "FAIL: signal in bins %d..%d did not reach the frame "
                    "buffer (left=%d right=%d). Every 3D voice a title plays "
                    "is being discarded.\n",
                    EFFECT_BIN_LO, EFFECT_BIN_HI, nonzero_left, nonzero_right);
            free(d);
            return 1;
        }
        puts("PASS: bins above 1 reach the output on both channels.");

        /* The 5.1 fold (the default arm) puts the centre on both channels
         * equally; the even/odd arm put it on the left only. */
        if (atoi(getenv("RECOMP_APU_MIXDOWN_ALL") ? getenv("RECOMP_APU_MIXDOWN_ALL") : "1") == 1) {
            int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
            memset(mixbins, 0, sizeof(mixbins));
            for (i = 0; i < NUM_SAMPLES_PER_FRAME; ++i)
                mixbins[2][i] = 0.5f;
            mcpx_apu_monitor_mixdown(d, mixbins);
            for (i = 0; i < NUM_SAMPLES_PER_FRAME; ++i) {
                if (!d->monitor.frame_buf[off + i][0]
                        || d->monitor.frame_buf[off + i][0] != d->monitor.frame_buf[off + i][1]) {
                    fprintf(stderr, "FAIL: centre bin folded unevenly (left=%d right=%d)\n",
                            d->monitor.frame_buf[off + i][0],
                            d->monitor.frame_buf[off + i][1]);
                    free(d);
                    return 1;
                }
            }
            puts("PASS: the centre bin reaches both channels equally.");
        }
    } else {
        /* The negative control. Without it, a mixdown that ignored its own
         * switch would pass the arm above and silently reintroduce nothing. */
        if (nonzero_left || nonzero_right) {
            fprintf(stderr,
                    "FAIL: RECOMP_APU_MIXDOWN_ALL=0 still mixed bins above 1 "
                    "(left=%d right=%d), so the on arm proves nothing.\n",
                    nonzero_left, nonzero_right);
            free(d);
            return 1;
        }
        puts("PASS: with the switch off the same signal is discarded, "
             "which is the behaviour this change replaces.");
    }

    free(d);
    return 0;
}
