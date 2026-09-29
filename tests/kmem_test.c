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

/* ── Contiguous arena ────────────────────────────────────── */

#define ARENA_BASE 0x80000000u

static void arena_init(struct kmem_arena *a, struct kmem_block *b, int cap,
                       uint32_t window)
{
    memset(a, 0, sizeof *a);
    a->b = b;
    a->cap = cap;
    a->next = ARENA_BASE;
    a->limit = (uint64_t)ARENA_BASE + window;
}

static void test_arena_free_then_reuse(void)
{
    static struct kmem_block b[64];
    struct kmem_arena a;
    uint32_t p1, p2, p3, hw;

    arena_init(&a, b, 64, 0x100000);
    p1 = kmem_arena_alloc(&a, 0x3000, 0, g_legacy);
    p2 = kmem_arena_alloc(&a, 0x1000, 0, g_legacy);
    CHECK(p1 == ARENA_BASE && p2 == ARENA_BASE + 0x3000);
    hw = a.next;

    CHECK(kmem_arena_free(&a, p1) == 1);
    CHECK(a.next == hw);                     /* a free never lowers the mark */
    CHECK(kmem_arena_block_size(&a, p1) == 0);

    /* The freed block comes back instead of the window growing. */
    p3 = kmem_arena_alloc(&a, 0x2000, 0, g_legacy);
    CHECK(p3 == p1);
    CHECK(a.next == hw);
    CHECK(kmem_arena_block_size(&a, p3) == 0x2000);
    CHECK(kmem_arena_block_size(&a, p3 + 0x800) == 0x1800);
    CHECK(blocks_ordered(b, a.count));
    CHECK(a.frees_ok == 1 && a.frees_unknown == 0);
}

static void test_arena_merges_neighbours(void)
{
    static struct kmem_block b[64];
    struct kmem_arena a;
    uint32_t p1, p2, p3;

    arena_init(&a, b, 64, 0x100000);
    p1 = kmem_arena_alloc(&a, 0x1000, 0, g_legacy);
    p2 = kmem_arena_alloc(&a, 0x1000, 0, g_legacy);
    p3 = kmem_arena_alloc(&a, 0x1000, 0, g_legacy);
    CHECK(kmem_arena_free(&a, p1) == 1);
    CHECK(kmem_arena_free(&a, p2) == 1);
    CHECK(kmem_arena_free(&a, p2) == 0);     /* double free is not ours */
    CHECK(a.frees_unknown == 1);
    /* Two freed neighbours serve one request the size of both. */
    CHECK(kmem_arena_alloc(&a, 0x2000, 0, g_legacy) == p1);
    CHECK(kmem_arena_block_size(&a, p3) == 0x1000);
}

static void test_arena_aligned_carve(void)
{
    static struct kmem_block b[64];
    struct kmem_arena a;
    uint32_t p0, p1, p2;

    arena_init(&a, b, 64, 0x100000);
    p0 = kmem_arena_alloc(&a, 0x1000, 0, g_legacy);
    p1 = kmem_arena_alloc(&a, 0x20000, 0, g_legacy);   /* at +0x1000 */
    kmem_arena_alloc(&a, 0x1000, 0, g_legacy);
    CHECK(p0 == ARENA_BASE && p1 == ARENA_BASE + 0x1000);
    CHECK(kmem_arena_free(&a, p1) == 1);

    /* A 64 KB-aligned piece from the middle, both sides kept free. */
    p2 = kmem_arena_alloc(&a, 0x1000, 0x10000, g_legacy);
    CHECK(p2 == ARENA_BASE + 0x10000);
    CHECK(blocks_ordered(b, a.count));
    CHECK(kmem_arena_alloc(&a, 0xF000, 0, g_legacy) == ARENA_BASE + 0x1000);
    CHECK(kmem_arena_alloc(&a, 0x10000, 0, g_legacy) == ARENA_BASE + 0x11000);
}

static void test_arena_bump_arithmetic_unchanged(void)
{
    static struct kmem_block b[8];
    struct kmem_arena a;

    /* The high-water mark advances by the exact size, as it always did. */
    arena_init(&a, b, 8, 0x10000);
    CHECK(kmem_arena_alloc(&a, 100, 0, g_legacy) == ARENA_BASE);
    CHECK(a.next == ARENA_BASE + 100);
    CHECK(kmem_arena_alloc(&a, 100, 0, g_legacy) == ARENA_BASE + 0x1000);
    CHECK(kmem_arena_alloc(&a, 0xF000, 0, g_legacy) == 0);   /* past the limit */
    CHECK(a.next == ARENA_BASE + 0x1000 + 100);
}

static void test_arena_table_full_is_counted(void)
{
    static struct kmem_block b[2];
    struct kmem_arena a;
    uint32_t p3;

    arena_init(&a, b, 2, 0x100000);
    kmem_arena_alloc(&a, 0x1000, 0, 0);
    kmem_arena_alloc(&a, 0x1000, 0, 0);
    p3 = kmem_arena_alloc(&a, 0x1000, 0, 0);
    CHECK(p3 == ARENA_BASE + 0x2000);        /* still allocated... */
    CHECK(a.untracked == 1);                 /* ...and counted as unrecorded */
    CHECK(kmem_arena_free(&a, p3) == 0);
    CHECK(a.frees_unknown == 1);
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
    test_arena_free_then_reuse();
    test_arena_merges_neighbours();
    test_arena_aligned_carve();
    test_arena_bump_arithmetic_unchanged();
    test_arena_table_full_is_counted();

    printf("kmem_test: %d checks, %d failed%s\n", g_checks, g_failures,
           g_legacy ? " (legacy behaviour)" : "");
    return g_failures ? 1 : 0;
}
