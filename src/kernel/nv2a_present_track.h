/*
 * nv2a_present_track.h - which colour surface a flip presents.
 *
 * A frame made only of clears or of vertex-program batches the rasteriser
 * cannot transform draws nothing, so presenting "the last drawn surface"
 * showed the previous buffer. The tracker also records the surface a clear or
 * an untransformed batch targeted this frame, and presents that instead.
 *
 * Preference at a flip: drawn this frame, else targeted this frame, else a
 * previous frame's drawn surface, else the caller's fallback. A flip clears
 * both per-frame flags.
 */
#ifndef NV2A_PRESENT_TRACK_H
#define NV2A_PRESENT_TRACK_H

#include <stdint.h>
#include <stddef.h>

typedef struct Nv2aPresentTrack {
    uint32_t drawn_offset;       /* last surface a batch drew into (any frame) */
    uint32_t targeted_offset;    /* last surface a clear or batch targeted */
    int drawn_this_frame;
    int targeted_this_frame;
} Nv2aPresentTrack;

static inline void present_track_drawn(Nv2aPresentTrack *t, uint32_t color_offset)
{
    t->drawn_offset = color_offset;
    t->drawn_this_frame = 1;
}

static inline void present_track_targeted(Nv2aPresentTrack *t, uint32_t color_offset)
{
    t->targeted_offset = color_offset;
    t->targeted_this_frame = 1;
}

/* Returns the surface to present; *used_targeted (may be NULL) is set to 1
 * when that is a targeted-only surface. */
static inline uint32_t present_track_flip(Nv2aPresentTrack *t, uint32_t fallback,
                                          int *used_targeted)
{
    uint32_t done;
    int targeted = 0;

    if (t->drawn_this_frame) {
        done = t->drawn_offset;
    } else if (t->targeted_this_frame) {
        done = t->targeted_offset;
        targeted = 1;
    } else if (t->drawn_offset) {
        done = t->drawn_offset;
    } else {
        done = fallback;
    }
    t->drawn_this_frame = 0;
    t->targeted_this_frame = 0;
    if (used_targeted) *used_targeted = targeted;
    return done;
}

#endif
