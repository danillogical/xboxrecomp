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

    printf("fence_snapshot_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
