/*
 * kmem.c - guest memory bookkeeping with no host dependency
 *
 * See kmem.h. Nothing here touches guest memory or the host: callers pass
 * their tables in and do the zero-filling, locking and logging themselves.
 */

#include "kmem.h"

#include <string.h>

/* ── Block tables ────────────────────────────────────────── */

/* Put a block right after index i, keeping address order. An unused slot
 * already there is taken; otherwise the tail shifts up one. */
static int blocks_insert_after(struct kmem_block *b, int *count, int cap,
                               int i, uint32_t addr, uint32_t size, int is_free)
{
    int at = i + 1;

    if (at < *count && b[at].size == 0) {
        b[at].addr = addr;
        b[at].size = size;
        b[at].free = (uint8_t)is_free;
        return 1;
    }
    if (*count >= cap)
        return 0;
    memmove(&b[at + 1], &b[at], (size_t)(*count - at) * sizeof b[0]);
    b[at].addr = addr;
    b[at].size = size;
    b[at].free = (uint8_t)is_free;
    (*count)++;
    return 1;
}

/* Put a block at index i, before the one there now, keeping address order. */
static int blocks_insert_before(struct kmem_block *b, int *count, int cap,
                                int i, uint32_t addr, uint32_t size, int is_free)
{
    if (i > 0 && b[i - 1].size == 0) {
        b[i - 1].addr = addr;
        b[i - 1].size = size;
        b[i - 1].free = (uint8_t)is_free;
        return 1;
    }
    if (*count >= cap)
        return 0;
    memmove(&b[i + 1], &b[i], (size_t)(*count - i) * sizeof b[0]);
    b[i].addr = addr;
    b[i].size = size;
    b[i].free = (uint8_t)is_free;
    (*count)++;
    return 1;
}

/* Free slots the two inserts around index i could use. */
static int blocks_room(const struct kmem_block *b, int count, int cap, int i)
{
    int room = cap - count;

    if (i + 1 < count && b[i + 1].size == 0)
        room++;
    if (i > 0 && b[i - 1].size == 0)
        room++;
    return room;
}

static void blocks_remove(struct kmem_block *b, int *count, int i)
{
    memmove(&b[i], &b[i + 1], (size_t)(*count - i - 1) * sizeof b[0]);
    (*count)--;
}

/* ── Guest heap ──────────────────────────────────────────── */

#define KMEM_HEAP_MIN_BLOCK 16u

uint32_t kmem_heap_reuse(struct kmem_block *b, int *count, int cap,
                         uint32_t size, uint32_t align, int split,
                         int *split_result)
{
    int i;

    *split_result = 0;
    for (i = 0; i < *count; i++) {
        uint32_t keep;

        if (!b[i].free || b[i].size < size)
            continue;
        if (b[i].addr & (align - 1))
            continue;   /* wrong alignment for this request */
        b[i].free = 0;
        if (!split)
            return b[i].addr;

        /* Split at a 16-byte boundary so the remainder can serve the aligned
         * requests most callers make; a smaller tail stays with the block. */
        keep = (size + (KMEM_HEAP_MIN_BLOCK - 1)) & ~(KMEM_HEAP_MIN_BLOCK - 1);
        if (keep < size)
            keep = size;
        if (keep < b[i].size && b[i].size - keep >= KMEM_HEAP_MIN_BLOCK) {
            if (blocks_insert_after(b, count, cap, i, b[i].addr + keep,
                                    b[i].size - keep, 1)) {
                b[i].size = keep;
                *split_result = 1;
            } else {
                *split_result = KMEM_TABLE_FULL;
            }
        }
        return b[i].addr;
    }
    return 0;
}

/* ── Contiguous arena ────────────────────────────────────── */

uint32_t kmem_arena_alloc(struct kmem_arena *a, uint32_t size,
                          uint32_t align, int legacy)
{
    uint32_t result;
    int i;

    if (align < 4096)
        align = 4096;

    /* A freed block first: carve the aligned piece, keep what is left free. */
    for (i = 0; !legacy && size && i < a->count; i++) {
        struct kmem_block *blk = &a->b[i];
        uint64_t start, end;
        uint32_t front, back;
        int room, keep_back;

        if (!blk->free || blk->size < size)
            continue;
        start = ((uint64_t)blk->addr + align - 1) & ~(uint64_t)(align - 1);
        end = (uint64_t)blk->addr + blk->size;
        if (start + size > end)
            continue;
        front = (uint32_t)(start - blk->addr);
        back = (uint32_t)(end - (start + size));
        room = blocks_room(a->b, a->count, a->cap, i);
        if (front && room < 1) {
            a->split_skipped++;         /* the piece in front could not be kept */
            continue;
        }
        keep_back = back && room < (front ? 2 : 1);

        blk->addr = (uint32_t)start;
        blk->size = keep_back ? (uint32_t)(end - start) : size;
        blk->free = 0;
        if (keep_back)
            a->split_skipped++;
        else if (back)
            blocks_insert_after(a->b, &a->count, a->cap, i,
                                (uint32_t)start + size, back, 1);
        if (front)
            blocks_insert_before(a->b, &a->count, a->cap, i,
                                 (uint32_t)start - front, front, 1);
        return (uint32_t)start;
    }

    result = (a->next + align - 1) & ~(align - 1);
    if ((uint64_t)result + size > a->limit)
        return 0;

    if (!legacy && size) {
        /* The alignment gap stays usable; the block itself must be recorded
         * or it can never be freed. */
        if (result > a->next) {
            if (a->cap - a->count >= 2)
                blocks_insert_after(a->b, &a->count, a->cap, a->count - 1,
                                    a->next, result - a->next, 1);
            else
                a->split_skipped++;
        }
        if (a->count < a->cap)
            blocks_insert_after(a->b, &a->count, a->cap, a->count - 1,
                                result, size, 0);
        else
            a->untracked++;
    }
    a->next = result + size;
    return result;
}

int kmem_arena_free(struct kmem_arena *a, uint32_t addr)
{
    int i;

    for (i = 0; i < a->count; i++) {
        struct kmem_block *b = a->b;

        if (!b[i].size || b[i].free || b[i].addr != addr)
            continue;
        b[i].free = 1;
        a->frees_ok++;
        if (i + 1 < a->count && b[i + 1].size && b[i + 1].free &&
            b[i].addr + b[i].size == b[i + 1].addr) {
            b[i].size += b[i + 1].size;
            blocks_remove(b, &a->count, i + 1);
        }
        if (i > 0 && b[i - 1].size && b[i - 1].free &&
            b[i - 1].addr + b[i - 1].size == b[i].addr) {
            b[i - 1].size += b[i].size;
            blocks_remove(b, &a->count, i);
        }
        return 1;
    }
    a->frees_unknown++;
    return 0;
}

uint32_t kmem_arena_block_size(const struct kmem_arena *a, uint32_t va)
{
    int i;

    for (i = 0; i < a->count; i++) {
        const struct kmem_block *b = &a->b[i];

        if (!b->size || b->free)
            continue;
        if (va >= b->addr && va - b->addr < b->size)
            return b->size - (va - b->addr);
    }
    return 0;
}
