/*
 * MCPX APU monitor mixdown - the EP's existing mixbin passthrough.
 *
 * A4b1 step 5, Device semantics 2: "The EP keeps the existing mixbin
 * passthrough."
 *
 * This is the toolkit's own monitor-output path, lifted unchanged out of the
 * old src/apu/apu_dsp.c when the pinned gp_ep.c replaced that file's frame
 * function. It is deliberately NOT part of the pinned port: it is this
 * toolkit's audio-output behaviour, and the pinned mcpx_apu_dsp_frame is the
 * device path. The ported frame function runs the pinned device path and then
 * calls mcpx_apu_monitor_mixdown() here, which is the one hunk the pinned
 * gp_ep.c's monitor branch gives up to it.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "apu_watch.h"

#include <stdlib.h>

/* SUM EVERY MIXBIN THE GUEST ROUTED TO, NOT JUST THE FIRST TWO.
 *
 * Default ON. RECOMP_APU_MIXDOWN_ALL=0 restores the previous two-bin read,
 * because this changes audible output for every title and an escape hatch
 * costs one branch.
 *
 * Measured on Jet Set Radio Future, one 200 s gameplay run, with a positive
 * control moving beside it:
 *
 *     [APU-BIN] 2D heard=557466 lost=0
 *               3D heard=0      lost=377768
 *               lost by bin: 6,7,8,9,10
 *
 * 557,466 music voice-frames heard and none lost; 377,768 effect voice-frames
 * produced correctly and thrown away. The title's 3D positional voices -- its
 * sound effects -- are routed to bins 6 to 10 by the guest's own V0BIN..V3BIN,
 * and music on 2D voices lands in bins 0 and 1, which is why the music was
 * always audible and no effect ever was. Every instrument upstream of this line
 * read healthy.
 *
 * Gating the HRTF submix override was tried first and did not fix it, so the
 * defect is the width of this mixdown and nothing else.
 *
 * The wide arm is a 5.1 fold-down of the DirectSound speaker buses, the one
 * Mercenaries-Recompiled uses when its EP does not run: front L/R as they are,
 * centre and rear at -3 dB into both sides, LFE at -6 dB, the 3D front pair
 * (6/7) as they are and the 3D rear pair (8/9) at -3 dB. Bins from 10 up are
 * effect sends (I3DL2 reverb first) that the EP would process; they are not
 * dry signal and are left out. This is not what a real EP does either; it is
 * the cheapest mixdown that keeps every authored speaker route.
 *
 * RECOMP_APU_MIXDOWN_ALL=2 restores the first wide mixdown, every even bin
 * left and every odd bin right, which put the centre on the left only. */
int mcpx_apu_mixdown_all(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_MIXDOWN_ALL");
        on = (e && *e) ? atoi(e) : 1;
    }
    return on;
}

/* Fill the monitor frame buffer for this frame from the VP's mixbins.
 *
 * The Xbox DirectSound typically routes:
 *   Mixbin 0 = Front Left
 *   Mixbin 1 = Front Right
 *   Mixbin 2 = Center (often unused in stereo)
 *   Mixbin 3 = LFE
 *   Mixbin 4-5 = Rear L/R
 *
 * For stereo output, bins 0 and 1 are what the two-bin arm wants; the wide arm
 * folds every speaker bus the guest routed to. */
void mcpx_apu_monitor_mixdown(MCPXAPUState *d,
                              float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        return;
    }

    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        float left, right;
        if (mcpx_apu_mixdown_all() == 2) {
            left = 0.0f;
            right = 0.0f;
            for (int b = 0; b < NUM_MIXBINS; ++b) {
                if (b & 1) right += mixbins[b][i];
                else       left  += mixbins[b][i];
            }
        } else if (mcpx_apu_mixdown_all()) {
            left  = mixbins[0][i] + 0.70710678f * mixbins[2][i]
                  + 0.5f * mixbins[3][i] + 0.70710678f * mixbins[4][i]
                  + mixbins[6][i] + 0.70710678f * mixbins[8][i];
            right = mixbins[1][i] + 0.70710678f * mixbins[2][i]
                  + 0.5f * mixbins[3][i] + 0.70710678f * mixbins[5][i]
                  + mixbins[7][i] + 0.70710678f * mixbins[9][i];
        } else {
            left = mixbins[0][i];
            right = mixbins[1][i];
        }
        /* Clamp to [-1, 1] range */
        if (left > 1.0f) left = 1.0f;
        if (left < -1.0f) left = -1.0f;
        if (right > 1.0f) right = 1.0f;
        if (right < -1.0f) right = -1.0f;

        /* Convert to 16-bit and write (not accumulate) into frame buffer.
         * Each of the 8 sub-frames writes its own 32-sample slice. */
        d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
        d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
    }
}
