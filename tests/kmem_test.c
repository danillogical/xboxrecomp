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

/* A 64 KB-aligned request against a free block that starts off the boundary
 * but contains an aligned piece. Before the carve this returned 0, so freed
 * memory could never serve NtAllocateVirtualMemory(base 0). */
static void test_heap_reuse_carves_aligned_piece(void)
{
    struct kmem_block b[8] = { { 0x00A00870u, 0x00100000u, 1 } };
    int count = 1, sr, i, found = 0;
    uint32_t a, freebytes = 0;
    struct kmem_block c[4] = { { 0x00B00010u, 0x8000u, 1 } };
    int n = 1;

    a = kmem_heap_reuse(b, &count, 8, 0x55000u, 0x10000u, !g_legacy, &sr);
    CHECK(a == 0x00A10000u);
    CHECK(blocks_ordered(b, count));
    for (i = 0; i < count; i++) {
        if (b[i].addr == a && a) { CHECK(!b[i].free && b[i].size == 0x55000u); found = 1; }
        if (b[i].free) freebytes += b[i].size;
    }
    CHECK(found);
    /* the front [0xA00870, 0xA10000) and the back remainder both stay free */
    CHECK(freebytes == 0x00100000u - 0x55000u);
    /* a request that cannot fit even when aligned is refused */
    CHECK(kmem_heap_reuse(c, &n, 4, 0x8000u, 0x10000u, 1, &sr) == 0);
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

/* ── Placed heap reservation ─────────────────────────────── */

#define HEAP_LO 0x01000000u
#define HEAP_HI 0x01100000u

static void test_carve_tail_records_gap(void)
{
    struct kmem_block b[8] = { { HEAP_LO, 0x1000u, 0 } };
    int count = 1;
    uint32_t bump = HEAP_LO + 0x1000;

    CHECK(kmem_heap_carve(b, &count, 8, &bump, HEAP_LO, HEAP_HI,
                          HEAP_LO + 0x10000, 0x4000) == 1);
    CHECK(bump == HEAP_LO + 0x14000);
    CHECK(count == 3);
    CHECK(b[1].addr == HEAP_LO + 0x1000 && b[1].size == 0xF000 && b[1].free);
    CHECK(b[2].addr == HEAP_LO + 0x10000 && b[2].size == 0x4000 && !b[2].free);
    /* Now in use, so a second placement there is refused. */
    CHECK(kmem_heap_carve(b, &count, 8, &bump, HEAP_LO, HEAP_HI,
                          HEAP_LO + 0x12000, 0x1000) == 0);
    /* Past the top of the heap. */
    CHECK(kmem_heap_carve(b, &count, 8, &bump, HEAP_LO, HEAP_HI,
                          HEAP_HI - 0x1000, 0x2000) == 0);
}

static void test_carve_inside_free_block(void)
{
    struct kmem_block b[8] = {
        { HEAP_LO,           0x40000u, 1 },
        { HEAP_LO + 0x40000, 0x1000u,  0 },
    };
    int count = 2;
    uint32_t bump = HEAP_LO + 0x41000;

    CHECK(kmem_heap_carve(b, &count, 8, &bump, HEAP_LO, HEAP_HI,
                          HEAP_LO + 0x10000, 0x10000) == 1);
    CHECK(count == 4 && bump == HEAP_LO + 0x41000);
    CHECK(b[0].addr == HEAP_LO && b[0].size == 0x10000 && b[0].free);
    CHECK(b[1].addr == HEAP_LO + 0x10000 && b[1].size == 0x10000 && !b[1].free);
    CHECK(b[2].addr == HEAP_LO + 0x20000 && b[2].size == 0x20000 && b[2].free);
    CHECK(blocks_ordered(b, count));
}

static void test_carve_runs_from_last_free_block_into_tail(void)
{
    struct kmem_block b[8] = {
        { HEAP_LO,          0x1000u, 0 },
        { HEAP_LO + 0x1000, 0x3000u, 1 },
    };
    int count = 2;
    uint32_t bump = HEAP_LO + 0x4000;

    CHECK(kmem_heap_carve(b, &count, 8, &bump, HEAP_LO, HEAP_HI,
                          HEAP_LO + 0x2000, 0x8000) == 1);
    CHECK(bump == HEAP_LO + 0xA000);
    CHECK(b[1].addr == HEAP_LO + 0x1000 && b[1].size == 0x1000 && b[1].free);
    CHECK(b[2].addr == HEAP_LO + 0x2000 && b[2].size == 0x8000 && !b[2].free);
}

static void test_carve_refuses_what_it_cannot_prove_free(void)
{
    /* The heap skipped 0x...1000..0x...2000 for alignment and never recorded
     * it; a block it never recorded looks the same, so neither is free. */
    struct kmem_block b[8] = {
        { HEAP_LO,          0x1000u, 0 },
        { HEAP_LO + 0x2000, 0x1000u, 1 },
        { HEAP_LO + 0x3000, 0x1000u, 0 },
    };
    struct kmem_block full[2] = {
        { HEAP_LO,          0x1000u, 0 },
        { HEAP_LO + 0x1000, 0x4000u, 1 },
    };
    int count = 3, fcount = 2;
    uint32_t bump = HEAP_LO + 0x4000, fbump = HEAP_LO + 0x5000;

    CHECK(kmem_heap_carve(b, &count, 8, &bump, HEAP_LO, HEAP_HI,
                          HEAP_LO + 0x1000, 0x1000) == 0);
    CHECK(kmem_heap_carve(b, &count, 8, &bump, HEAP_LO, HEAP_HI,
                          HEAP_LO + 0x2000, 0x2000) == 0);   /* past a non-last block */
    CHECK(kmem_heap_carve(full, &fcount, 2, &fbump, HEAP_LO, HEAP_HI,
                          HEAP_LO + 0x2000, 0x1000) == KMEM_TABLE_FULL);
    CHECK(fcount == 2 && full[1].size == 0x4000 && full[1].free);
}

/* ── NtAllocateVirtualMemory / NtFreeVirtualMemory ───────── */

/* A fake guest: a heap over [HEAP_LO, HEAP_HI) with real bytes behind it. */
struct fake_guest {
    struct kmem_block b[256];
    int count;
    uint32_t bump;
    uint8_t mem[HEAP_HI - HEAP_LO];
    uint32_t next_fake;        /* addresses handed out with no memory behind */
    int fake_only;
};

static uint8_t *guest_ptr(struct fake_guest *g, uint32_t va)
{
    return &g->mem[va - HEAP_LO];
}

static uint32_t fake_heap_alloc(struct fake_guest *g, uint32_t size, uint32_t align)
{
    uint32_t a;

    if (g->fake_only) {
        a = g->next_fake;
        g->next_fake += (size + 0xFFFFu) & ~0xFFFFu;
        return a;
    }
    a = (g->bump + align - 1) & ~(align - 1);
    if ((uint64_t)a + size > HEAP_HI)
        return 0;
    g->b[g->count].addr = a;
    g->b[g->count].size = size;
    g->b[g->count].free = 0;
    g->count++;
    g->bump = a + size;
    memset(guest_ptr(g, a), 0, size);
    return a;
}

static uint32_t fake_reserve_any(void *ctx, uint32_t *size, uint32_t type,
                                 uint8_t *backing)
{
    (void)type;
    *backing = KMEM_BACK_HEAP;
    return fake_heap_alloc(ctx, *size, KMEM_RESERVE_ALIGN);
}

static int fake_reserve_at(void *ctx, uint32_t base, uint32_t size)
{
    struct fake_guest *g = ctx;
    int r = kmem_heap_carve(g->b, &g->count, 256, &g->bump, HEAP_LO, HEAP_HI,
                            base, size);

    if (r == 1)
        memset(guest_ptr(g, base), 0, size);
    return r;
}

static void fake_release(void *ctx, uint32_t base, uint32_t size, uint8_t backing)
{
    struct fake_guest *g = ctx;
    int i;

    (void)size;
    (void)backing;
    for (i = 0; i < g->count; i++)
        if (g->b[i].size && g->b[i].addr == base && !g->b[i].free)
            g->b[i].free = 1;
}

static uint32_t fake_heap_block_bytes(void *ctx, uint32_t va)
{
    struct fake_guest *g = ctx;
    int i;

    for (i = 0; i < g->count; i++)
        if (g->b[i].size && !g->b[i].free && va >= g->b[i].addr &&
            va - g->b[i].addr < g->b[i].size)
            return g->b[i].size - (va - g->b[i].addr);
    return 0;
}

static void fake_zero(void *ctx, uint32_t va, uint32_t len)
{
    struct fake_guest *g = ctx;

    if (!g->fake_only)
        memset(guest_ptr(g, va), 0, len);
}

static struct fake_guest g_guest;
static struct kmem_vm g_vm;
static struct kmem_backend g_be;

static void vm_reset(void)
{
    memset(&g_guest, 0, sizeof g_guest);
    memset(&g_vm, 0, sizeof g_vm);
    g_guest.bump = HEAP_LO;
    g_guest.next_fake = 0x10000000u;
    g_be.ctx = &g_guest;
    g_be.reserve_any = fake_reserve_any;
    g_be.reserve_at = fake_reserve_at;
    g_be.release = fake_release;
    g_be.heap_block_bytes = fake_heap_block_bytes;
    g_be.zero = fake_zero;
}

static uint32_t vm_alloc(uint32_t *base, uint32_t *size, uint32_t type)
{
    struct kmem_event ev;
    return kmem_vm_allocate(&g_vm, &g_be, base, size, type, &ev);
}

static uint32_t vm_free(uint32_t *base, uint32_t *size, uint32_t type)
{
    struct kmem_event ev;
    return kmem_vm_free(&g_vm, &g_be, base, size, type, &ev);
}

static void test_vm_reserve_at_hint(void)
{
    uint32_t base, size;
    struct kmem_event ev;

    vm_reset();
    base = HEAP_LO + 0x21234;
    size = 0x8000;
    CHECK(vm_alloc(&base, &size, KMEM_MEM_RESERVE) == KMEM_STATUS_SUCCESS);
    CHECK(base == HEAP_LO + 0x20000);          /* rounded down to 64 KB */
    CHECK(size == 0xA000);                     /* to the page past 0x...29234 */
    CHECK(kmem_vm_page_uncommitted(&g_vm, HEAP_LO + 0x20000));
    CHECK(kmem_vm_page_uncommitted(&g_vm, HEAP_LO + 0x29000));
    CHECK(!kmem_vm_page_uncommitted(&g_vm, HEAP_LO + 0x2A000));
    CHECK(g_vm.c.hint_ok == 1);

    /* The same range again, and a range inside a live heap block. */
    base = HEAP_LO + 0x24000;
    size = 0x1000;
    CHECK(kmem_vm_allocate(&g_vm, &g_be, &base, &size, KMEM_MEM_RESERVE, &ev)
          == KMEM_STATUS_CONFLICTING_ADDRESSES);
    CHECK(ev.kind == KMEM_EV_HINT_CONFLICT && base == HEAP_LO + 0x24000);
    CHECK(fake_heap_alloc(&g_guest, 0x10000, 0x10000) == HEAP_LO + 0x30000);
    base = HEAP_LO + 0x30000;
    size = 0x1000;
    CHECK(vm_alloc(&base, &size, KMEM_MEM_RESERVE | KMEM_MEM_COMMIT)
          == KMEM_STATUS_CONFLICTING_ADDRESSES);
    /* Outside the heap entirely. */
    base = 0x00010000u;
    size = 0x1000;
    CHECK(vm_alloc(&base, &size, KMEM_MEM_RESERVE)
          == KMEM_STATUS_CONFLICTING_ADDRESSES);
    CHECK(g_vm.c.hint_conflict == 3);
}

static void test_vm_commit_only_inside_a_reservation(void)
{
    uint32_t rbase = 0, rsize = 0x40000, base, size;
    struct kmem_event ev;

    vm_reset();
    CHECK(vm_alloc(&rbase, &rsize, KMEM_MEM_RESERVE) == KMEM_STATUS_SUCCESS);
    CHECK((rbase & 0xFFFF) == 0 && rsize == 0x40000);

    /* Written back page-rounded; the page is zero-filled on first commit. */
    memset(guest_ptr(&g_guest, rbase + 0x1000), 0xAA, 0x1000);
    base = rbase + 0x1100;
    size = 0x100;
    CHECK(vm_alloc(&base, &size, KMEM_MEM_COMMIT) == KMEM_STATUS_SUCCESS);
    CHECK(base == rbase + 0x1000 && size == 0x1000);
    CHECK(guest_ptr(&g_guest, rbase + 0x1000)[0] == 0);
    CHECK(g_vm.c.pages_zeroed == 1);

    /* Committing a committed page leaves its contents. */
    guest_ptr(&g_guest, rbase + 0x1000)[0] = 0x55;
    base = rbase + 0x1000;
    size = 0x1000;
    CHECK(vm_alloc(&base, &size, KMEM_MEM_COMMIT) == KMEM_STATUS_SUCCESS);
    CHECK(guest_ptr(&g_guest, rbase + 0x1000)[0] == 0x55);

    /* Past the end of the reservation, and where nothing is reserved. */
    base = rbase + 0x3F000;
    size = 0x2000;
    CHECK(kmem_vm_allocate(&g_vm, &g_be, &base, &size, KMEM_MEM_COMMIT, &ev)
          == KMEM_STATUS_CONFLICTING_ADDRESSES);
    CHECK(ev.kind == KMEM_EV_COMMIT_REJECTED);
    CHECK(base == rbase + 0x3F000 && size == 0x2000);   /* untouched on failure */
    base = HEAP_HI - 0x1000;
    size = 0x1000;
    CHECK(vm_alloc(&base, &size, KMEM_MEM_COMMIT)
          == KMEM_STATUS_CONFLICTING_ADDRESSES);
    CHECK(g_vm.c.commit_in_region == 2 && g_vm.c.commit_rejected == 2);
}

static void test_vm_commit_in_heap_block(void)
{
    uint32_t blk, base, size;

    vm_reset();
    blk = fake_heap_alloc(&g_guest, 0x3000, 0x1000);
    base = blk + 0x800;
    size = 0x1000;
    CHECK(vm_alloc(&base, &size, KMEM_MEM_COMMIT) == KMEM_STATUS_SUCCESS);
    CHECK(base == blk && size == 0x2000);
    CHECK(g_vm.c.commit_heap_block == 1);
    base = blk + 0x2800;
    size = 0x1000;                              /* runs past the block */
    CHECK(vm_alloc(&base, &size, KMEM_MEM_COMMIT)
          == KMEM_STATUS_CONFLICTING_ADDRESSES);
}

static void test_vm_decommit_and_recommit(void)
{
    uint32_t rbase = 0, rsize = 0x10000, base, size;

    vm_reset();
    CHECK(vm_alloc(&rbase, &rsize, KMEM_MEM_RESERVE | KMEM_MEM_COMMIT)
          == KMEM_STATUS_SUCCESS);
    CHECK(!kmem_vm_page_uncommitted(&g_vm, rbase));
    memset(guest_ptr(&g_guest, rbase), 0xCC, 0x3000);

    base = rbase + 0x1000;
    size = 0x2000;
    CHECK(vm_free(&base, &size, KMEM_MEM_DECOMMIT) == KMEM_STATUS_SUCCESS);
    CHECK(base == rbase + 0x1000 && size == 0x2000);
    CHECK(kmem_vm_page_uncommitted(&g_vm, rbase + 0x1000));
    CHECK(guest_ptr(&g_guest, rbase + 0x1000)[0] == 0xCC);   /* nothing freed */

    base = rbase;
    size = 0x3000;
    CHECK(vm_alloc(&base, &size, KMEM_MEM_COMMIT) == KMEM_STATUS_SUCCESS);
    CHECK(guest_ptr(&g_guest, rbase)[0] == 0xCC);            /* was committed */
    CHECK(guest_ptr(&g_guest, rbase + 0x1000)[0] == 0);
    CHECK(guest_ptr(&g_guest, rbase + 0x2FFF)[0] == 0);

    /* MEM_NOZERO keeps what the pages held. */
    memset(guest_ptr(&g_guest, rbase + 0x1000), 0xDD, 0x1000);
    base = rbase + 0x1000;
    size = 0x1000;
    CHECK(vm_free(&base, &size, KMEM_MEM_DECOMMIT) == KMEM_STATUS_SUCCESS);
    CHECK(vm_alloc(&base, &size, KMEM_MEM_COMMIT | KMEM_MEM_NOZERO)
          == KMEM_STATUS_SUCCESS);
    CHECK(guest_ptr(&g_guest, rbase + 0x1000)[0] == 0xDD);
    CHECK(g_vm.c.decommit_ok == 2);
}

static void test_vm_free_errors(void)
{
    uint32_t rbase = 0, rsize = 0x20000, base, size;
    struct kmem_event ev;

    vm_reset();
    CHECK(vm_alloc(&rbase, &rsize, KMEM_MEM_RESERVE) == KMEM_STATUS_SUCCESS);

    base = rbase;
    size = 0;
    CHECK(kmem_vm_free(&g_vm, &g_be, &base, &size,
                       KMEM_MEM_DECOMMIT | KMEM_MEM_RELEASE, &ev)
          == KMEM_STATUS_INVALID_PARAMETER);
    CHECK(ev.kind == KMEM_EV_BAD_FREE_TYPE);
    CHECK(vm_free(&base, &size, 0) == KMEM_STATUS_INVALID_PARAMETER);

    base = HEAP_HI - 0x1000;
    CHECK(vm_free(&base, &size, KMEM_MEM_RELEASE) == KMEM_STATUS_MEMORY_NOT_ALLOCATED);
    base = rbase + 0x1000;
    size = 0;
    CHECK(vm_free(&base, &size, KMEM_MEM_RELEASE) == KMEM_STATUS_FREE_VM_NOT_AT_BASE);
    CHECK(vm_free(&base, &size, KMEM_MEM_DECOMMIT) == KMEM_STATUS_FREE_VM_NOT_AT_BASE);
    size = 0x20000;
    CHECK(vm_free(&base, &size, KMEM_MEM_DECOMMIT) == KMEM_STATUS_UNABLE_TO_FREE_VM);
    base = rbase;
    size = 0x1000;                           /* part of the region */
    CHECK(vm_free(&base, &size, KMEM_MEM_RELEASE) == KMEM_STATUS_INVALID_PARAMETER);
    CHECK(base == rbase && size == 0x1000);
    CHECK(g_vm.c.free_bad_type == 2 && g_vm.c.release_failed == 3 &&
          g_vm.c.decommit_failed == 2 && g_vm.c.release_ok == 0);
}

static void test_vm_release_returns_the_region(void)
{
    uint32_t rbase, rsize, base, size;

    vm_reset();
    fake_heap_alloc(&g_guest, 0x1000, 0x1000);       /* something below it */
    rbase = HEAP_LO + 0x10000;
    rsize = 0x10000;
    CHECK(vm_alloc(&rbase, &rsize, KMEM_MEM_RESERVE) == KMEM_STATUS_SUCCESS);
    CHECK(g_vm.live == 1);

    base = rbase + 0x10;                      /* same page as the base */
    size = 0;
    CHECK(vm_free(&base, &size, KMEM_MEM_RELEASE) == KMEM_STATUS_SUCCESS);
    CHECK(base == rbase && size == 0x10000);
    CHECK(g_vm.live == 0 && g_vm.c.release_ok == 1);
    CHECK(fake_heap_block_bytes(&g_guest, rbase) == 0);   /* back in the heap */
    CHECK(!kmem_vm_page_uncommitted(&g_vm, rbase));

    /* Gone: a commit there is refused, a second release finds nothing, and
     * the same placed reservation can be made again. */
    base = rbase;
    size = 0x1000;
    CHECK(vm_alloc(&base, &size, KMEM_MEM_COMMIT) == KMEM_STATUS_CONFLICTING_ADDRESSES);
    size = 0;
    CHECK(vm_free(&base, &size, KMEM_MEM_RELEASE) == KMEM_STATUS_MEMORY_NOT_ALLOCATED);
    base = rbase;
    size = 0x10000;
    CHECK(vm_alloc(&base, &size, KMEM_MEM_RESERVE) == KMEM_STATUS_SUCCESS);
    CHECK(base == rbase);
    base = rbase;
    size = 0x10000;                           /* whole region, size given */
    CHECK(vm_free(&base, &size, KMEM_MEM_RELEASE) == KMEM_STATUS_SUCCESS);
}

static void test_vm_base_zero_and_bad_type(void)
{
    uint32_t base = 0, size = 0x1801;

    vm_reset();
    CHECK(vm_alloc(&base, &size, KMEM_MEM_COMMIT) == KMEM_STATUS_SUCCESS);
    CHECK((base & 0xFFFF) == 0 && size == 0x2000);   /* COMMIT alone reserves too */
    CHECK(!kmem_vm_page_uncommitted(&g_vm, base));
    base = 0;
    size = 0x1000;
    CHECK(vm_alloc(&base, &size, 0) == KMEM_STATUS_INVALID_PARAMETER);
    base = 0;
    CHECK(vm_alloc(&base, &size, KMEM_MEM_RESET) == KMEM_STATUS_INVALID_PARAMETER);
    base = 0xFFFFF000u;
    size = 0x2000;
    CHECK(vm_alloc(&base, &size, KMEM_MEM_RESERVE) == KMEM_STATUS_INVALID_PARAMETER);
    CHECK(g_vm.c.alloc_invalid == 3 && g_vm.c.reserve_ok == 1);
}

static void test_vm_region_table_full(void)
{
    uint32_t base, size;
    struct kmem_event ev;
    int i, ok = 1;

    vm_reset();
    g_guest.fake_only = 1;
    for (i = 0; i < KMEM_MAX_REGIONS; i++) {
        base = 0;
        size = 0x1000;
        ok &= vm_alloc(&base, &size, KMEM_MEM_RESERVE) == KMEM_STATUS_SUCCESS;
    }
    CHECK(ok && g_vm.live == KMEM_MAX_REGIONS);
    base = 0;
    size = 0x1000;
    CHECK(kmem_vm_allocate(&g_vm, &g_be, &base, &size, KMEM_MEM_RESERVE, &ev)
          == KMEM_STATUS_NO_MEMORY);
    CHECK(ev.kind == KMEM_EV_TABLE_FULL && g_vm.c.region_table_full == 1);

    base = 0x10000000u;
    size = 0;
    CHECK(vm_free(&base, &size, KMEM_MEM_RELEASE) == KMEM_STATUS_SUCCESS);
    base = 0;
    size = 0x1000;
    CHECK(vm_alloc(&base, &size, KMEM_MEM_RESERVE) == KMEM_STATUS_SUCCESS);
}

/* ── Guest heap free ─────────────────────────────────────── */

/* No slot of a heap table may be a size-0 placeholder: the merge removes it. */
static int no_empty_slots(const struct kmem_block *b, int count)
{
    int i;

    for (i = 0; i < count; i++)
        if (!b[i].size)
            return 0;
    return 1;
}

static void test_heap_free_merges_the_review_scenario(void)
{
    /* A, B, C adjacent and live, then a free tail. */
    struct kmem_block b[8] = {
        { 0x00100000u, 0x100u, 0 },
        { 0x00100100u, 0x100u, 0 },
        { 0x00100200u, 0x100u, 0 },
    };
    int count = 3;

    CHECK(kmem_heap_free(b, &count, 0x00100100u) == 1);      /* B */
    CHECK(count == 3 && b[1].free);
    CHECK(kmem_heap_free(b, &count, 0x00100000u) == 1);      /* A joins B */
    CHECK(count == 2);
    CHECK(b[0].addr == 0x00100000u && b[0].size == 0x200u && b[0].free);
    CHECK(kmem_heap_free(b, &count, 0x00100200u) == 1);      /* C joins A+B */
    CHECK(count == 1);
    CHECK(b[0].addr == 0x00100000u && b[0].size == 0x300u && b[0].free);
    CHECK(blocks_ordered(b, count));
    CHECK(no_empty_slots(b, count));
}

static void test_heap_free_between_two_free_blocks(void)
{
    struct kmem_block b[8] = {
        { 0x00100000u, 0x100u, 1 },
        { 0x00100100u, 0x100u, 0 },
        { 0x00100200u, 0x100u, 1 },
        { 0x00100300u, 0x100u, 0 },
    };
    int count = 4;

    CHECK(kmem_heap_free(b, &count, 0x00100100u) == 1);
    CHECK(count == 2);
    CHECK(b[0].addr == 0x00100000u && b[0].size == 0x300u && b[0].free);
    CHECK(b[1].addr == 0x00100300u && b[1].size == 0x100u && !b[1].free);
    CHECK(blocks_ordered(b, count));
    CHECK(no_empty_slots(b, count));
}

static void test_heap_free_does_not_merge_across_a_gap_or_a_live_block(void)
{
    struct kmem_block b[8] = {
        { 0x00100000u, 0x100u, 1 },
        { 0x00100200u, 0x100u, 0 },     /* a gap before it */
        { 0x00100300u, 0x100u, 0 },
    };
    int count = 3;

    CHECK(kmem_heap_free(b, &count, 0x00100200u) == 1);
    CHECK(count == 3 && b[0].size == 0x100u && b[1].free && b[1].size == 0x100u);
    CHECK(kmem_heap_free(b, &count, 0x00100300u) == 1);
    CHECK(count == 2 && b[1].addr == 0x00100200u && b[1].size == 0x200u && b[1].free);
    CHECK(b[0].addr == 0x00100000u && b[0].size == 0x100u);
    CHECK(no_empty_slots(b, count));
}

static void test_heap_free_rejects_unknown_and_double_free(void)
{
    struct kmem_block b[4] = {
        { 0x00100000u, 0x100u, 0 },
        { 0x00100100u, 0x100u, 1 },
    };
    int count = 2;

    CHECK(kmem_heap_free(b, &count, 0x00200000u) == 0);      /* not ours */
    CHECK(kmem_heap_free(b, &count, 0x00100010u) == 0);      /* inside a block */
    CHECK(kmem_heap_free(b, &count, 0x00100100u) == 0);      /* already free */
    CHECK(count == 2 && !b[0].free && b[1].free && b[1].size == 0x100u);

    CHECK(kmem_heap_free(b, &count, 0x00100000u) == 1);
    CHECK(count == 1 && b[0].addr == 0x00100000u && b[0].size == 0x200u && b[0].free);
    CHECK(kmem_heap_free(b, &count, 0x00100000u) == 0);      /* double free */
    CHECK(count == 1 && b[0].size == 0x200u);
    CHECK(no_empty_slots(b, count));
}

/* Every order of freeing four adjacent live blocks ends as one free block
 * with no size-0 slot on the way. */
static void test_heap_free_any_order_leaves_no_placeholders(void)
{
    int perm[24][4], n = 0, p, i;

    for (p = 0; p < 256; p++) {
        int a = p & 3, c = (p >> 2) & 3, d = (p >> 4) & 3, e = (p >> 6) & 3;

        if (a == c || a == d || a == e || c == d || c == e || d == e)
            continue;
        perm[n][0] = a; perm[n][1] = c; perm[n][2] = d; perm[n][3] = e;
        n++;
    }
    CHECK(n == 24);
    for (p = 0; p < n; p++) {
        struct kmem_block b[8];
        int count = 4;

        for (i = 0; i < 4; i++) {
            b[i].addr = 0x00100000u + (uint32_t)i * 0x100u;
            b[i].size = 0x100u;
            b[i].free = 0;
        }
        for (i = 0; i < 4; i++) {
            CHECK(kmem_heap_free(b, &count, 0x00100000u + (uint32_t)perm[p][i] * 0x100u) == 1);
            CHECK(no_empty_slots(b, count));
            CHECK(blocks_ordered(b, count));
        }
        CHECK(count == 1);
        CHECK(b[0].addr == 0x00100000u && b[0].size == 0x400u && b[0].free);
    }
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
    test_heap_reuse_carves_aligned_piece();
    test_arena_free_then_reuse();
    test_arena_merges_neighbours();
    test_arena_aligned_carve();
    test_arena_bump_arithmetic_unchanged();
    test_arena_table_full_is_counted();
    test_carve_tail_records_gap();
    test_carve_inside_free_block();
    test_carve_runs_from_last_free_block_into_tail();
    test_carve_refuses_what_it_cannot_prove_free();
    test_vm_reserve_at_hint();
    test_vm_commit_only_inside_a_reservation();
    test_vm_commit_in_heap_block();
    test_vm_decommit_and_recommit();
    test_vm_free_errors();
    test_vm_release_returns_the_region();
    test_vm_base_zero_and_bad_type();
    test_vm_region_table_full();
    test_heap_free_merges_the_review_scenario();
    test_heap_free_between_two_free_blocks();
    test_heap_free_does_not_merge_across_a_gap_or_a_live_block();
    test_heap_free_rejects_unknown_and_double_free();
    test_heap_free_any_order_leaves_no_placeholders();

    printf("kmem_test: %d checks, %d failed%s\n", g_checks, g_failures,
           g_legacy ? " (legacy behaviour)" : "");
    return g_failures ? 1 : 0;
}
