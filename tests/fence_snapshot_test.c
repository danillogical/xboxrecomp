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

int main(void)
{
    test_fresh_reads_live();
    test_commit_publishes_the_kicked_value();
    test_uncommitted_kicks_do_not_publish();
    test_commit_without_a_new_kick_changes_nothing();
    test_live_mode_always_reads_live();

    printf("fence_snapshot_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
