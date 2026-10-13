/*
 * fence_snapshot_test.c - the fence value a kick publishes, in src/kernel/fence_snapshot.h.
 *
 * The fence word the guest submitted at a PUT write becomes visible only when
 * the walk that consumed it commits, unless live mode asks for the raw value.
 * Header-only and host-independent:
 *
 *     cc -I src/kernel tests/fence_snapshot_test.c -o fence_snapshot_test
 */
#include "fence_snapshot.h"

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

static void test_fresh_reads_live(void)
{
    FenceSnapshot s;

    memset(&s, 0, sizeof(s));          /* zero-initialised is the fresh state */
    CHECK(fence_snapshot_value(&s, 7, 0) == 7);
    CHECK(fence_snapshot_value(&s, 0, 0) == 0);
    CHECK(fence_snapshot_value(&s, 0xFFFFFFFFu, 0) == 0xFFFFFFFFu);
}

static void test_commit_publishes_the_kicked_value(void)
{
    FenceSnapshot s;

    memset(&s, 0, sizeof(s));
    fence_snapshot_kick(&s, 10);
    CHECK(fence_snapshot_value(&s, 10, 0) == 10);   /* nothing committed yet */
    CHECK(fence_snapshot_value(&s, 99, 0) == 99);
    fence_snapshot_commit(&s);
    CHECK(fence_snapshot_value(&s, 14, 0) == 10);   /* live ran ahead; the snapshot holds */
    CHECK(fence_snapshot_value(&s, 0, 0) == 10);
}

static void test_uncommitted_kicks_do_not_publish(void)
{
    FenceSnapshot s;

    memset(&s, 0, sizeof(s));
    fence_snapshot_kick(&s, 10);
    fence_snapshot_commit(&s);
    fence_snapshot_kick(&s, 12);
    fence_snapshot_kick(&s, 14);
    CHECK(fence_snapshot_value(&s, 14, 0) == 10);
    fence_snapshot_commit(&s);
    CHECK(fence_snapshot_value(&s, 20, 0) == 14);   /* the latest kick wins */
}

static void test_commit_without_a_new_kick_changes_nothing(void)
{
    FenceSnapshot s;

    memset(&s, 0, sizeof(s));
    fence_snapshot_commit(&s);                       /* nothing pending, nothing published */
    CHECK(fence_snapshot_value(&s, 5, 0) == 5);
    fence_snapshot_kick(&s, 10);
    fence_snapshot_commit(&s);
    fence_snapshot_commit(&s);
    fence_snapshot_commit(&s);
    CHECK(fence_snapshot_value(&s, 30, 0) == 10);
    fence_snapshot_kick(&s, 12);
    fence_snapshot_commit(&s);
    fence_snapshot_commit(&s);
    CHECK(fence_snapshot_value(&s, 30, 0) == 12);
}

static void test_live_mode_always_reads_live(void)
{
    FenceSnapshot s;

    memset(&s, 0, sizeof(s));
    CHECK(fence_snapshot_value(&s, 3, 1) == 3);
    fence_snapshot_kick(&s, 10);
    fence_snapshot_commit(&s);
    CHECK(fence_snapshot_value(&s, 14, 1) == 14);
    CHECK(fence_snapshot_value(&s, 14, 2) == 14);    /* any non-zero mode */
    CHECK(fence_snapshot_value(&s, 14, 0) == 10);
}

static void test_partial_publishes_a_release_within_pending(void)
{
    FenceSnapshot s;

    memset(&s, 0, sizeof(s));
    fence_snapshot_kick(&s, 20);
    fence_snapshot_partial(&s, 18);
    CHECK(fence_snapshot_value(&s, 25, 0) == 18);

    fence_snapshot_partial(&s, 16);                  /* older than published */
    CHECK(fence_snapshot_value(&s, 25, 0) == 18);

    fence_snapshot_partial(&s, 22);                  /* past pending */
    CHECK(fence_snapshot_value(&s, 25, 0) == 18);

    fence_snapshot_commit(&s);
    CHECK(fence_snapshot_value(&s, 25, 0) == 20);

    /* A release equal to the published value is not newer. */
    fence_snapshot_kick(&s, 30);
    fence_snapshot_partial(&s, 20);
    CHECK(fence_snapshot_value(&s, 99, 0) == 20);
    fence_snapshot_partial(&s, 30);                  /* equal to pending is allowed */
    CHECK(fence_snapshot_value(&s, 99, 0) == 30);
}

static void test_partial_is_ignored_without_a_kick(void)
{
    FenceSnapshot s;

    memset(&s, 0, sizeof(s));
    fence_snapshot_partial(&s, 18);
    CHECK(fence_snapshot_value(&s, 7, 0) == 7);      /* still the live value */
    CHECK(s.have_published == 0);

    /* Nor after a commit that had nothing pending. */
    fence_snapshot_commit(&s);
    fence_snapshot_partial(&s, 18);
    CHECK(fence_snapshot_value(&s, 7, 0) == 7);
}

static void test_partial_wraps(void)
{
    FenceSnapshot s;

    memset(&s, 0, sizeof(s));
    fence_snapshot_kick(&s, 0xFFFFFFF0u);
    fence_snapshot_commit(&s);                       /* published 0xFFFFFFF0 */
    fence_snapshot_kick(&s, 0x10u);
    fence_snapshot_partial(&s, 0x4u);                /* newer across the wrap, within pending */
    CHECK(fence_snapshot_value(&s, 0x20u, 0) == 0x4u);
    fence_snapshot_partial(&s, 0xFFFFFFF8u);         /* older than 0x4 */
    CHECK(fence_snapshot_value(&s, 0x20u, 0) == 0x4u);
}

static void test_partial_in_live_mode_still_reads_live(void)
{
    FenceSnapshot s;

    memset(&s, 0, sizeof(s));
    fence_snapshot_kick(&s, 20);
    fence_snapshot_partial(&s, 18);
    CHECK(fence_snapshot_value(&s, 25, 1) == 25);
}

/* ---- stream releases (fence_snapshot_release / _mirror_owns) -------------- */

static void test_fresh_is_not_released_and_mirror_owns(void)
{
    FenceSnapshot s;

    memset(&s, 0, sizeof(s));
    CHECK(s.released == 0);
    CHECK(fence_snapshot_mirror_owns(&s, 0) == 1);
    CHECK(fence_snapshot_mirror_owns(&s, 1) == 1);
}

static void test_release_latches_only_when_accepted_and_not_live(void)
{
    FenceSnapshot s;

    /* Not opted in: no latch, nothing changes, in either mode. */
    memset(&s, 0, sizeof(s));
    CHECK(fence_snapshot_release(&s, 0, 0) == 0);
    CHECK(s.released == 0);
    CHECK(fence_snapshot_mirror_owns(&s, 0) == 1);
    CHECK(fence_snapshot_release(&s, 0, 1) == 0);
    CHECK(s.released == 0);

    /* Opted in but live mode: no latch. Any non-zero live mode counts. */
    CHECK(fence_snapshot_release(&s, 1, 1) == 0);
    CHECK(fence_snapshot_release(&s, 1, 2) == 0);
    CHECK(s.released == 0);
    CHECK(fence_snapshot_mirror_owns(&s, 0) == 1);

    /* Opted in, live off: latches and reports it. */
    CHECK(fence_snapshot_release(&s, 1, 0) == 1);
    CHECK(s.released != 0);
    CHECK(fence_snapshot_mirror_owns(&s, 0) == 0);
}

static void test_rejected_release_leaves_the_struct_unchanged(void)
{
    FenceSnapshot s, before;

    memset(&s, 0, sizeof(s));
    fence_snapshot_kick(&s, 10);
    fence_snapshot_commit(&s);
    fence_snapshot_kick(&s, 12);
    before = s;
    CHECK(fence_snapshot_release(&s, 0, 0) == 0);
    CHECK(fence_snapshot_release(&s, 1, 1) == 0);
    CHECK(fence_snapshot_release(&s, 0, 1) == 0);
    CHECK(memcmp(&s, &before, sizeof(s)) == 0);
}

static void test_mirror_owns_in_live_mode_always(void)
{
    FenceSnapshot s;

    memset(&s, 0, sizeof(s));
    CHECK(fence_snapshot_mirror_owns(&s, 1) == 1);
    CHECK(fence_snapshot_release(&s, 1, 0) == 1);
    CHECK(fence_snapshot_mirror_owns(&s, 0) == 0);
    CHECK(fence_snapshot_mirror_owns(&s, 1) == 1);   /* live mode overrides the latch */
    CHECK(fence_snapshot_mirror_owns(&s, 2) == 1);
}

static void test_latch_never_clears(void)
{
    FenceSnapshot s;

    memset(&s, 0, sizeof(s));
    CHECK(fence_snapshot_release(&s, 1, 0) == 1);
    CHECK(fence_snapshot_mirror_owns(&s, 0) == 0);

    CHECK(fence_snapshot_release(&s, 1, 0) == 1);    /* repeat release */
    CHECK(fence_snapshot_mirror_owns(&s, 0) == 0);
    CHECK(fence_snapshot_release(&s, 0, 0) == 0);    /* a refused one cannot clear it */
    CHECK(fence_snapshot_release(&s, 1, 1) == 0);
    CHECK(fence_snapshot_mirror_owns(&s, 0) == 0);

    fence_snapshot_kick(&s, 10);
    CHECK(fence_snapshot_mirror_owns(&s, 0) == 0);
    fence_snapshot_commit(&s);
    CHECK(fence_snapshot_mirror_owns(&s, 0) == 0);
    fence_snapshot_kick(&s, 14);
    fence_snapshot_partial(&s, 12);
    CHECK(fence_snapshot_mirror_owns(&s, 0) == 0);
    fence_snapshot_commit(&s);
    CHECK(fence_snapshot_mirror_owns(&s, 0) == 0);
    CHECK(s.released != 0);
}

static void test_release_does_not_disturb_the_published_value(void)
{
    FenceSnapshot s;

    /* Never latched: pending/partial/live behave as before. */
    memset(&s, 0, sizeof(s));
    CHECK(fence_snapshot_release(&s, 0, 0) == 0);
    CHECK(fence_snapshot_value(&s, 7, 0) == 7);      /* live until the first commit */
    fence_snapshot_kick(&s, 20);
    CHECK(fence_snapshot_value(&s, 25, 0) == 25);
    fence_snapshot_partial(&s, 18);
    CHECK(fence_snapshot_value(&s, 25, 0) == 18);
    fence_snapshot_commit(&s);
    CHECK(fence_snapshot_value(&s, 25, 0) == 20);    /* pending at the commit */

    /* Latching changes only `released`; the value logic is untouched. */
    memset(&s, 0, sizeof(s));
    fence_snapshot_kick(&s, 20);
    fence_snapshot_commit(&s);
    CHECK(fence_snapshot_release(&s, 1, 0) == 1);
    CHECK(s.pending == 20 && s.published == 20 && s.have_published == 1 && s.kicked == 1);
    CHECK(fence_snapshot_value(&s, 99, 0) == 20);
}

/* ---- the title's wait, modelled (D3D_BlockOnTime at 0x00191440) ------------ */

/* done when ecx >= edx, unsigned, edx = cur - word, ecx = cur - want. */
static int wait_done(uint32_t cur, uint32_t word, uint32_t want)
{
    uint32_t edx = cur - word;
    uint32_t ecx = cur - want;
    return ecx >= edx;
}

static void test_wait_model_defect_arithmetic(void)
{
    uint32_t cur = 102;                    /* counter after D3D_SetFence advanced it */
    uint32_t released = cur - 2;           /* the value the release carried */

    /* Negative control: the old behaviour, word = counter, satisfies everything. */
    CHECK(wait_done(cur, cur, cur));       /* the bug: a fence still in flight reads done */
    CHECK(wait_done(cur, cur, released));
    CHECK(wait_done(cur, cur, cur - 100));

    /* New: the word holds the previous release (cur - 4) until this one runs. */
    CHECK(!wait_done(cur, cur - 4, released));
    CHECK(!wait_done(cur, cur - 4, cur));
    /* Its release written: the issued fence is done ... */
    CHECK(wait_done(cur, released, released));
    /* ... and a release of cur - 2 does not satisfy a wait for cur. */
    CHECK(!wait_done(cur, released, cur));
    CHECK(wait_done(cur, released, released - 2));   /* older fences stay done */

    /* Across the counter wrap. */
    cur = 0x00000001u; released = cur - 2;           /* 0xFFFFFFFF */
    CHECK(wait_done(cur, cur, cur));                 /* bug, still */
    CHECK(!wait_done(cur, cur - 4, released));
    CHECK(wait_done(cur, released, released));
    CHECK(!wait_done(cur, released, cur));
}

/* A mirror tick as the runtime does it: write the fallback only while it owns the word. */
static void tick(FenceSnapshot *s, uint32_t *word, uint32_t live, int live_mode)
{
    if (fence_snapshot_mirror_owns(s, live_mode))
        *word = fence_snapshot_value(s, live, live_mode);
}

/* The executor's release: write verbatim when the snapshot says to. */
static void exec_release(FenceSnapshot *s, uint32_t *word, uint32_t value, int live_mode)
{
    if (fence_snapshot_release(s, 1, live_mode))
        *word = value;
}

static void test_stream_order_through_the_snapshot(void)
{
    FenceSnapshot s;
    uint32_t word = 0, cur = 100;

    memset(&s, 0, sizeof(s));
    tick(&s, &word, cur, 0);
    CHECK(word == 100);                    /* before any release the fallback drives it */

    /* Fence 1: D3D_SetFence releases the pre-increment counter, then adds 2. */
    exec_release(&s, &word, cur, 0);
    cur += 2;
    CHECK(word == 100);
    tick(&s, &word, cur, 0);               /* the tick must not publish the counter */
    CHECK(word == 100);
    CHECK(wait_done(cur, word, 100));

    /* Fence 2 issued, its release not yet executed: not done, whatever ticks do. */
    tick(&s, &word, cur, 0);
    CHECK(!wait_done(cur + 2, word, cur)); /* want = the next release value (102) */
    exec_release(&s, &word, cur, 0);
    cur += 2;
    CHECK(word == 102);
    CHECK(wait_done(cur, word, 102));
    CHECK(!wait_done(cur, word, cur));     /* cur - 2 does not satisfy want = cur */
}

static void test_live_mode_never_stands_down(void)
{
    FenceSnapshot s;
    uint32_t word = 0;

    memset(&s, 0, sizeof(s));
    exec_release(&s, &word, 100, 1);       /* refused */
    CHECK(word == 0);
    tick(&s, &word, 102, 1);
    CHECK(word == 102);                    /* the old mirror, counter and all */
    CHECK(s.released == 0);
}

int main(void)
{
    test_fresh_reads_live();
    test_commit_publishes_the_kicked_value();
    test_uncommitted_kicks_do_not_publish();
    test_commit_without_a_new_kick_changes_nothing();
    test_live_mode_always_reads_live();
    test_partial_publishes_a_release_within_pending();
    test_partial_is_ignored_without_a_kick();
    test_partial_wraps();
    test_partial_in_live_mode_still_reads_live();
    test_fresh_is_not_released_and_mirror_owns();
    test_release_latches_only_when_accepted_and_not_live();
    test_rejected_release_leaves_the_struct_unchanged();
    test_mirror_owns_in_live_mode_always();
    test_latch_never_clears();
    test_release_does_not_disturb_the_published_value();
    test_wait_model_defect_arithmetic();
    test_stream_order_through_the_snapshot();
    test_live_mode_never_stands_down();

    printf("fence_snapshot_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
