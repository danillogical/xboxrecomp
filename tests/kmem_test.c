/*
 * kmem_test.c - regression for the allocator decisions in src/kernel/kmem.c
 *
 * Pure logic, no guest mapping and no windows.h, so it runs on any host:
 *
 *     cc -I src/kernel src/kernel/kmem.c tests/kmem_test.c -o kmem_test
 *     ./kmem_test
 *
 * Registered with ctest as xbox_kmem. KMEM_TEST_AS_LEGACY=1 runs the same
 * scenarios with the pre-registry behaviour switched back on; the checks that
 * pin the new behaviour must then fail, which is how to see they have teeth.
 */

#include "kmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;
static int g_legacy;

#define CHECK(cond) do {                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

/* Live (size != 0) entries in address order, and no two overlap. */
static int blocks_ordered(const struct kmem_block *b, int count)
{
    uint64_t end = 0;
    int i;

    for (i = 0; i < count; i++) {
        if (!b[i].size)
            continue;
        if (b[i].addr < end)
            return 0;
        end = (uint64_t)b[i].addr + b[i].size;
    }
    return 1;
}

/* ── Guest heap reuse ────────────────────────────────────── */

static void test_heap_reuse_splits(void)
{
    struct kmem_block b[8] = { { 0x00100000u, 0x10000u, 1 } };
    int count = 1, sr;
    uint32_t a1, a2;

    a1 = kmem_heap_reuse(b, &count, 8, 0x100, 16, !g_legacy, &sr);
    CHECK(a1 == 0x00100000u);
    CHECK(sr == 1);
    CHECK(b[0].size == 0x100 && !b[0].free);
    CHECK(count == 2 && b[1].addr == 0x00100100u && b[1].size == 0xFF00u && b[1].free);

    /* The remainder serves the next request instead of the heap growing. */
    a2 = kmem_heap_reuse(b, &count, 8, 0x8000, 16, !g_legacy, &sr);
    CHECK(a2 == 0x00100100u);
    CHECK(blocks_ordered(b, count));
}

static void test_heap_reuse_rounds_split_point(void)
{
    struct kmem_block b[4] = { { 0x00200000u, 0x1000u, 1 } };
    int count = 1, sr;

    CHECK(kmem_heap_reuse(b, &count, 4, 0x21, 4, !g_legacy, &sr) == 0x00200000u);
    CHECK(b[0].size == 0x30);                       /* 0x21 rounded to 16 */
    CHECK(count == 2 && b[1].addr == 0x00200030u);
}

static void test_heap_reuse_fills_unused_slot(void)
{
    /* The heap leaves size-0 slots behind when it coalesces. */
    struct kmem_block b[4] = {
        { 0x00300000u, 0x2000u, 1 },
        { 0, 0, 1 },
        { 0x00302000u, 0x1000u, 0 },
    };
    int count = 3, sr;

    CHECK(kmem_heap_reuse(b, &count, 4, 0x800, 16, !g_legacy, &sr) == 0x00300000u);
    CHECK(count == 3);
    CHECK(b[1].addr == 0x00300800u && b[1].size == 0x1800u && b[1].free);
    CHECK(blocks_ordered(b, count));
}

static void test_heap_reuse_table_full_hands_out_whole_block(void)
{
    struct kmem_block b[2] = {
        { 0x00400000u, 0x4000u, 1 },
        { 0x00404000u, 0x1000u, 0 },
    };
    int count = 2, sr;

    CHECK(kmem_heap_reuse(b, &count, 2, 0x100, 16, 1, &sr) == 0x00400000u);
    CHECK(sr == KMEM_TABLE_FULL);
    CHECK(count == 2 && b[0].size == 0x4000u && !b[0].free);
}

static void test_heap_reuse_small_tail_stays(void)
{
    struct kmem_block b[4] = { { 0x00500000u, 0x108u, 1 } };
    int count = 1, sr;

    CHECK(kmem_heap_reuse(b, &count, 4, 0x100, 16, 1, &sr) == 0x00500000u);
    CHECK(sr == 0 && count == 1 && b[0].size == 0x108u);
}

static void test_heap_reuse_alignment_and_legacy(void)
{
    struct kmem_block b[4] = {
        { 0x00600010u, 0x10000u, 1 },   /* not 4 KB aligned */
        { 0x00700000u, 0x10000u, 1 },
    };
    int count = 2, sr;

    CHECK(kmem_heap_reuse(b, &count, 4, 0x1000, 0x1000, 0, &sr) == 0x00700000u);
    /* Whole-block reuse is the legacy answer: nothing split. */
    CHECK(sr == 0 && count == 2 && b[1].size == 0x10000u);
}

int main(void)
{
    const char *v = getenv("KMEM_TEST_AS_LEGACY");

    g_legacy = v && v[0] == '1';

    test_heap_reuse_splits();
    test_heap_reuse_rounds_split_point();
    test_heap_reuse_fills_unused_slot();
    test_heap_reuse_table_full_hands_out_whole_block();
    test_heap_reuse_small_tail_stays();
    test_heap_reuse_alignment_and_legacy();

    printf("kmem_test: %d checks, %d failed%s\n", g_checks, g_failures,
           g_legacy ? " (legacy behaviour)" : "");
    return g_failures ? 1 : 0;
}
