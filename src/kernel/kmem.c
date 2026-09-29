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
