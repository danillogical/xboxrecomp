/*
 * nv2a_present_track_test.c - which colour surface a flip presents, in
 * src/kernel/nv2a_present_track.h.
 *
 * Header-only and host-independent:
 *
 *     cc -I src/kernel tests/nv2a_present_track_test.c -o nv2a_present_track_test
 */
#include "nv2a_present_track.h"

#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond) do {                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

#define A 0x01000000u
#define B 0x02000000u
#define FALLBACK 0x0F000000u

static void test_software_drawn_frame(void)
{
    Nv2aPresentTrack t;
    int used = -1;

    memset(&t, 0, sizeof(t));
    present_track_targeted(&t, A);
    present_track_drawn(&t, A);
    CHECK(present_track_flip(&t, FALLBACK, &used) == A);
    CHECK(used == 0);
}

static void test_clear_only_frame_uses_the_target(void)
{
    Nv2aPresentTrack t;
    int used = -1;

    memset(&t, 0, sizeof(t));
    present_track_drawn(&t, A);
    CHECK(present_track_flip(&t, FALLBACK, &used) == A);
    CHECK(used == 0);

    present_track_targeted(&t, B);                   /* a clear or an untransformed batch */
    used = -1;
    CHECK(present_track_flip(&t, FALLBACK, &used) == B);
    CHECK(used == 1);

    /* An empty frame falls back to the last drawn surface, as before. */
    used = -1;
    CHECK(present_track_flip(&t, FALLBACK, &used) == A);
    CHECK(used == 0);
}

static void test_fresh_falls_back_to_the_caller(void)
{
    Nv2aPresentTrack t;
    int used = -1;

    memset(&t, 0, sizeof(t));
    CHECK(present_track_flip(&t, FALLBACK, &used) == FALLBACK);
    CHECK(used == 0);
    CHECK(present_track_flip(&t, FALLBACK, NULL) == FALLBACK);   /* NULL is allowed */
}

static void test_drawn_wins_over_targeted_in_the_same_frame(void)
{
    Nv2aPresentTrack t;
    int used = -1;

    memset(&t, 0, sizeof(t));
    present_track_drawn(&t, A);
    present_track_targeted(&t, B);
    CHECK(present_track_flip(&t, FALLBACK, &used) == A);
    CHECK(used == 0);

    /* Order within the frame does not matter. */
    present_track_targeted(&t, B);
    present_track_drawn(&t, A);
    CHECK(present_track_flip(&t, FALLBACK, &used) == A);
}

static void test_flip_clears_the_per_frame_flags(void)
{
    Nv2aPresentTrack t;

    memset(&t, 0, sizeof(t));
    present_track_drawn(&t, A);
    present_track_targeted(&t, B);
    (void)present_track_flip(&t, FALLBACK, NULL);
    CHECK(t.drawn_this_frame == 0);
    CHECK(t.targeted_this_frame == 0);
}

static void test_last_draw_in_a_frame_wins(void)
{
    Nv2aPresentTrack t;

    memset(&t, 0, sizeof(t));
    present_track_drawn(&t, A);
    present_track_drawn(&t, B);
    CHECK(present_track_flip(&t, FALLBACK, NULL) == B);
    present_track_targeted(&t, A);
    present_track_targeted(&t, B);
    CHECK(present_track_flip(&t, FALLBACK, NULL) == B);
}

int main(void)
{
    test_software_drawn_frame();
    test_clear_only_frame_uses_the_target();
    test_fresh_falls_back_to_the_caller();
    test_drawn_wins_over_targeted_in_the_same_frame();
    test_flip_clears_the_per_frame_flags();
    test_last_draw_in_a_frame_wins();

    printf("nv2a_present_track_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
