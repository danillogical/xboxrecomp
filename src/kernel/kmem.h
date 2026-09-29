/*
 * kmem.h - guest memory bookkeeping with no host dependency
 *
 * The decisions behind the guest heap's freed-block reuse and the contiguous
 * arena, kept free of windows.h so they compile and run as a native unit test on any host
 * (tests/kmem_test.c). Callers own locking, zero-filling and logging.
 *
 * Every table here is fixed-size. When one is full the operation that needed
 * the slot is refused or left undone and a counter says so; nothing is
 * dropped without a count.
 */

#ifndef XBOX_KMEM_H
#define XBOX_KMEM_H

#include <stdint.h>

typedef unsigned long long kmem_count_t;

/* ── Block tables ──────────────────────────────────────────
 *
 * An array of blocks kept in address order by index. An entry with size 0 is
 * an unused slot (the heap leaves these behind when it coalesces) and carries
 * no address; the insert helpers reuse one when it sits where the new entry
 * belongs, and shift the array otherwise. */
struct kmem_block {
    uint32_t addr;
    uint32_t size;
    uint8_t  free;
};

/* Result of an operation that may need table slots. */
#define KMEM_TABLE_FULL (-1)

/* Guest heap reuse: the first free block that fits `size` at its own start
 * with the requested alignment is marked in use and its address returned, or
 * 0 if none fits. With `split` set, a remainder of at least 16 bytes past the
 * request (rounded up to 16) becomes a separate free block; *split_result is
 * 1 when that happened, 0 when there was nothing to split, KMEM_TABLE_FULL
 * when the table had no slot and the whole block was handed out. With
 * `split` clear this is the heap's original whole-block reuse. */
uint32_t kmem_heap_reuse(struct kmem_block *b, int *count, int cap,
                         uint32_t size, uint32_t align, int split,
                         int *split_result);

/* ── Contiguous arena ──────────────────────────────────────
 *
 * A bump pointer over [next, limit) plus a block list, so freed blocks are
 * reused first-fit (the aligned piece is carved out and the rest stays free)
 * and merge with free neighbours. `next` is the high-water mark: it only ever
 * grows, and every block the arena records lies below it. */
struct kmem_arena {
    struct kmem_block *b;
    int count;
    int cap;
    uint32_t next;
    uint64_t limit;
    kmem_count_t frees_ok;
    kmem_count_t frees_unknown;   /* not the start of a live block of ours */
    kmem_count_t untracked;       /* allocations made while the table was full */
    kmem_count_t split_skipped;   /* free pieces left unrecorded: table full */
};

/* Allocate; 0 when nothing fits. `legacy` is the original bump allocator,
 * with no reuse and nothing recorded. Alignment below 4096 is raised to 4096.
 * A zero-byte request takes the legacy path in both modes. */
uint32_t kmem_arena_alloc(struct kmem_arena *a, uint32_t size,
                          uint32_t align, int legacy);

/* Free the live block starting at addr: 1 if freed, 0 if not one of ours. */
int kmem_arena_free(struct kmem_arena *a, uint32_t addr);

/* Bytes from va to the end of the live block containing it, 0 if none. */
uint32_t kmem_arena_block_size(const struct kmem_arena *a, uint32_t va);

/* ── Allocator counters ──────────────────────────────────── */
struct kmem_alloc_counters {
    kmem_count_t heap_split;        /* reused heap blocks split */
    kmem_count_t heap_split_full;   /* ... handed out whole: table full */
    kmem_count_t contig_free_ok;
    kmem_count_t contig_free_unknown;
    kmem_count_t contig_untracked;
    kmem_count_t contig_split_skipped;
};

#endif /* XBOX_KMEM_H */
