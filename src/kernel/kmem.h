/*
 * kmem.h - guest memory bookkeeping with no host dependency
 *
 * The decisions behind the guest heap's freed-block reuse, the contiguous
 * arena, and NtAllocateVirtualMemory / NtFreeVirtualMemory, kept free of
 * windows.h so they compile and run as a native unit test on any host
 * (tests/kmem_test.c). Callers own locking, zero-filling and logging.
 *
 * Every table here is fixed-size. When one is full the operation that needed
 * the slot is refused or left undone and a counter says so; nothing is
 * dropped without a count.
 */

#ifndef XBOX_KMEM_H
#define XBOX_KMEM_H

#include <stdint.h>

/* NT status values these decisions return. */
#define KMEM_STATUS_SUCCESS               0x00000000u
#define KMEM_STATUS_INVALID_PARAMETER     0xC000000Du
#define KMEM_STATUS_NO_MEMORY             0xC0000017u
#define KMEM_STATUS_CONFLICTING_ADDRESSES 0xC0000018u
#define KMEM_STATUS_UNABLE_TO_FREE_VM     0xC000001Au
#define KMEM_STATUS_FREE_VM_NOT_AT_BASE   0xC000009Fu
#define KMEM_STATUS_MEMORY_NOT_ALLOCATED  0xC00000A0u

/* AllocationType / FreeType bits. MEM_NOZERO is the Xbox kernel's own flag:
 * committed pages keep whatever they held instead of being zeroed. */
#define KMEM_MEM_COMMIT    0x00001000u
#define KMEM_MEM_RESERVE   0x00002000u
#define KMEM_MEM_DECOMMIT  0x00004000u
#define KMEM_MEM_RELEASE   0x00008000u
#define KMEM_MEM_RESET     0x00080000u
#define KMEM_MEM_NOZERO    0x00800000u

#define KMEM_PAGE_SIZE     0x1000u
#define KMEM_RESERVE_ALIGN 0x10000u   /* reservations start on 64 KB */

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

/* Take exactly [base, base+size) out of a heap whose untouched tail is
 * [*bump, hi). The range is free when it lies wholly in the tail, or wholly
 * inside one free block, or starts in a free last block that ends at *bump
 * and runs on into the tail. Anything else -- a live block, an alignment gap
 * the heap skipped, a block the table never recorded -- counts as in use.
 * Returns 1 when carved (the range is then a live block), 0 when not free,
 * KMEM_TABLE_FULL when free but the table cannot record the result. */
int kmem_heap_carve(struct kmem_block *b, int *count, int cap,
                    uint32_t *bump, uint32_t lo, uint32_t hi,
                    uint32_t base, uint32_t size);

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

/* Free the live block in b[0..*count) starting exactly at addr and merge it
 * with free address-adjacent neighbours: 1 if freed, 0 otherwise. The old heap
 * free left size-0 placeholders and looked only at i+-1, so frees beside a
 * placeholder stopped merging. */
int kmem_heap_free(struct kmem_block *b, int *count, uint32_t addr);

/* Bytes from va to the end of the live block containing it, 0 if none. */
uint32_t kmem_arena_block_size(const struct kmem_arena *a, uint32_t va);

/* ── Virtual memory regions ────────────────────────────────
 *
 * One entry per NtAllocateVirtualMemory reservation, and one bit per 4 KB
 * page of the 32-bit guest address space saying the page is reserved but not
 * committed (never committed, or decommitted since). Both are fixed-size: the
 * bitmap covers the whole address space, and a reservation that finds the
 * region table full is refused and counted. */
#define KMEM_MAX_REGIONS 4096

enum {
    KMEM_BACK_HEAP      = 1,   /* guest heap: released back with xbox_HeapFree */
    KMEM_BACK_ABOVE_RAM = 2    /* mapped space above RAM: a bump arena, never reused */
};

struct kmem_region {
    uint32_t base;
    uint32_t size;             /* 0 = unused slot */
    uint8_t  backing;
};

struct kmem_vm_counters {
    kmem_count_t hint_ok;           /* MEM_RESERVE granted at its base hint */
    kmem_count_t hint_conflict;     /* ... refused: STATUS_CONFLICTING_ADDRESSES */
    kmem_count_t reserve_ok;        /* base 0: reserved wherever it fit */
    kmem_count_t reserve_fail;      /* base 0: STATUS_NO_MEMORY */
    kmem_count_t alloc_invalid;     /* bad AllocationType or range */
    kmem_count_t commit_in_region;  /* MEM_COMMIT with a base inside a region */
    kmem_count_t commit_heap_block; /* ... inside a live heap block, no region */
    kmem_count_t commit_rejected;   /* ... anywhere else: CONFLICTING_ADDRESSES */
    kmem_count_t pages_zeroed;      /* pages zero-filled on commit */
    kmem_count_t decommit_ok;
    kmem_count_t decommit_failed;   /* not allocated / not at base / past the region */
    kmem_count_t release_ok;
    kmem_count_t release_failed;    /* ... or a partial release, see kmem_vm_free */
    kmem_count_t free_bad_type;     /* FreeType not exactly DECOMMIT or RELEASE */
    kmem_count_t region_table_full; /* reservation refused: no free region slot */
};

struct kmem_vm {
    struct kmem_region r[KMEM_MAX_REGIONS];
    int live;                                   /* regions in use */
    uint8_t uncommitted[(1u << 20) / 8];        /* bit per guest page */
    struct kmem_vm_counters c;
};

/* What the caller's allocators provide. */
struct kmem_backend {
    void *ctx;
    /* Reserve (and, with MEM_COMMIT in type, commit) *size bytes anywhere.
     * May lower *size when only less could be backed. Returns the base, 0 on
     * failure, and says where the pages came from. */
    uint32_t (*reserve_any)(void *ctx, uint32_t *size, uint32_t type,
                            uint8_t *backing);
    /* Reserve exactly [base, base+size): kmem_heap_carve's result. The pages
     * must come back zeroed. */
    int (*reserve_at)(void *ctx, uint32_t base, uint32_t size);
    void (*release)(void *ctx, uint32_t base, uint32_t size, uint8_t backing);
    /* Bytes from va to the end of the live heap block containing it, or 0. */
    uint32_t (*heap_block_bytes)(void *ctx, uint32_t va);
    void (*zero)(void *ctx, uint32_t va, uint32_t len);
};

/* A refused request, for the caller's first-N log lines. */
enum {
    KMEM_EV_NONE = 0,
    KMEM_EV_HINT_CONFLICT,
    KMEM_EV_RESERVE_FAILED,
    KMEM_EV_BAD_ALLOC,
    KMEM_EV_COMMIT_REJECTED,
    KMEM_EV_DECOMMIT_FAILED,
    KMEM_EV_RELEASE_FAILED,
    KMEM_EV_BAD_FREE_TYPE,
    KMEM_EV_TABLE_FULL,
    KMEM_EV_COUNT
};

struct kmem_event {
    int kind;
    uint32_t base, size, type, status;
};

const char *kmem_event_name(int kind);

/* NtAllocateVirtualMemory on the values the guest's BaseAddress and
 * RegionSize point at. On success *base and *size hold what the kernel writes
 * back; on failure they are untouched.
 *
 * - Base 0: reserve (and with MEM_COMMIT commit) anywhere, 64 KB-aligned,
 *   size rounded up to pages.
 * - Base given with MEM_RESERVE: exactly there -- base rounded down to 64 KB,
 *   end rounded up to a page -- or STATUS_CONFLICTING_ADDRESSES.
 * - Base given without MEM_RESERVE: the page-rounded range must lie inside
 *   one region (or, with no region there, inside a live heap block), else
 *   STATUS_CONFLICTING_ADDRESSES. MEM_COMMIT zero-fills the pages that were
 *   not committed, unless MEM_NOZERO. */
uint32_t kmem_vm_allocate(struct kmem_vm *vm, const struct kmem_backend *be,
                          uint32_t *base, uint32_t *size, uint32_t type,
                          struct kmem_event *ev);

/* NtFreeVirtualMemory, likewise. FreeType must be exactly MEM_DECOMMIT or
 * MEM_RELEASE. The page holding *base must be in a region, else
 * STATUS_MEMORY_NOT_ALLOCATED; a range past the region's end is
 * STATUS_UNABLE_TO_FREE_VM; size 0 means "the whole region" and needs *base at
 * its start, else STATUS_FREE_VM_NOT_AT_BASE. MEM_DECOMMIT only marks pages
 * uncommitted. MEM_RELEASE returns the whole region to its allocator; a
 * release of part of a region is refused with STATUS_INVALID_PARAMETER. */
uint32_t kmem_vm_free(struct kmem_vm *vm, const struct kmem_backend *be,
                      uint32_t *base, uint32_t *size, uint32_t type,
                      struct kmem_event *ev);

/* 1 when the page holding va is reserved but not committed. */
int kmem_vm_page_uncommitted(const struct kmem_vm *vm, uint32_t va);

/* ── Allocator counters ──────────────────────────────────── */
struct kmem_alloc_counters {
    kmem_count_t heap_split;        /* reused heap blocks split */
    kmem_count_t heap_split_full;   /* ... handed out whole: table full */
    kmem_count_t heap_carve_ok;     /* placed reservations taken from the heap */
    kmem_count_t heap_carve_busy;   /* ... refused: part of the range not free */
    kmem_count_t heap_carve_full;   /* ... refused: block table full */
    kmem_count_t contig_free_ok;
    kmem_count_t contig_free_unknown;
    kmem_count_t contig_untracked;
    kmem_count_t contig_split_skipped;
};

#endif /* XBOX_KMEM_H */
