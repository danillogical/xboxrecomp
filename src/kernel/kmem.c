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

/* Whether a piece can be recorded in front of and behind index i: each side
 * can take the unused slot next to it, and both share the spare capacity. */
static void blocks_split_room(const struct kmem_block *b, int count, int cap,
                              int i, int want_front, int want_back,
                              int *front_ok, int *back_ok)
{
    int spare = cap - count;
    int slot_before = i > 0 && b[i - 1].size == 0;
    int slot_after = i + 1 < count && b[i + 1].size == 0;

    *front_ok = !want_front || slot_before || spare > 0;
    if (want_front && !slot_before && spare > 0)
        spare--;
    *back_ok = !want_back || slot_after || spare > 0;
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

int kmem_heap_carve(struct kmem_block *b, int *count, int cap,
                    uint32_t *bump, uint32_t lo, uint32_t hi,
                    uint32_t base, uint32_t size)
{
    uint64_t end = (uint64_t)base + size, blk_end;
    uint32_t blk_addr, front, back;
    int i, idx = -1, extend = 0, front_ok, back_ok;

    if (!size || base < lo || end > hi)
        return 0;

    /* Wholly in the untouched tail. The gap it skips stays usable. */
    if (base >= *bump) {
        int gap = base > *bump;

        if (cap - *count < 1 + gap)
            return KMEM_TABLE_FULL;
        if (gap)
            blocks_insert_after(b, count, cap, *count - 1, *bump,
                                base - *bump, 1);
        blocks_insert_after(b, count, cap, *count - 1, base, size, 0);
        *bump = (uint32_t)end;
        return 1;
    }

    for (i = 0; i < *count; i++) {
        if (b[i].size && base >= b[i].addr && base - b[i].addr < b[i].size) {
            idx = i;
            break;
        }
    }
    if (idx < 0 || !b[idx].free)
        return 0;

    blk_end = (uint64_t)b[idx].addr + b[idx].size;
    if (end > blk_end) {
        /* Only the last block, ending at the tail, may run on into it. */
        if (blk_end != *bump)
            return 0;
        for (i = idx + 1; i < *count; i++)
            if (b[i].size)
                return 0;
        extend = 1;
    }

    blk_addr = b[idx].addr;
    front = base - blk_addr;
    back = extend ? 0 : (uint32_t)(blk_end - end);
    blocks_split_room(b, *count, cap, idx, front != 0, back != 0,
                      &front_ok, &back_ok);
    if (!front_ok || !back_ok)
        return KMEM_TABLE_FULL;

    b[idx].addr = base;
    b[idx].size = size;
    b[idx].free = 0;
    if (extend)
        *bump = (uint32_t)end;
    if (back)
        blocks_insert_after(b, count, cap, idx, (uint32_t)end, back, 1);
    if (front)
        blocks_insert_before(b, count, cap, idx, blk_addr, front, 1);
    return 1;
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
        int front_ok, back_ok;

        if (!blk->free || blk->size < size)
            continue;
        start = ((uint64_t)blk->addr + align - 1) & ~(uint64_t)(align - 1);
        end = (uint64_t)blk->addr + blk->size;
        if (start + size > end)
            continue;
        front = (uint32_t)(start - blk->addr);
        back = (uint32_t)(end - (start + size));
        blocks_split_room(a->b, a->count, a->cap, i, front != 0, back != 0,
                          &front_ok, &back_ok);
        if (!front_ok) {
            a->split_skipped++;         /* the piece in front could not be kept */
            continue;
        }

        blk->addr = (uint32_t)start;
        blk->size = back_ok ? size : (uint32_t)(end - start);
        blk->free = 0;
        if (!back_ok)
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

/* ── Virtual memory regions ──────────────────────────────── */

#define KMEM_PAGE_MASK   (KMEM_PAGE_SIZE - 1u)
#define KMEM_VA_LIMIT    0x100000000ull

static uint64_t page_round_up(uint64_t v)
{
    return (v + KMEM_PAGE_MASK) & ~(uint64_t)KMEM_PAGE_MASK;
}

static int page_bit(const struct kmem_vm *vm, uint64_t va)
{
    uint32_t page = (uint32_t)(va >> 12);

    return (vm->uncommitted[page >> 3] >> (page & 7)) & 1;
}

static void page_bit_set(struct kmem_vm *vm, uint64_t va, int on)
{
    uint32_t page = (uint32_t)(va >> 12);

    if (on)
        vm->uncommitted[page >> 3] |= (uint8_t)(1u << (page & 7));
    else
        vm->uncommitted[page >> 3] &= (uint8_t)~(1u << (page & 7));
}

static void pages_mark(struct kmem_vm *vm, uint64_t start, uint64_t end, int on)
{
    uint64_t va;

    for (va = start; va < end; va += KMEM_PAGE_SIZE)
        page_bit_set(vm, va, on);
}

/* Commit [start, end): pages not committed become committed, zero-filled in
 * runs unless the caller asked for MEM_NOZERO. */
static void pages_commit(struct kmem_vm *vm, const struct kmem_backend *be,
                         uint64_t start, uint64_t end, int zero)
{
    uint64_t va, run_start = 0, run_len = 0;

    for (va = start; va < end; va += KMEM_PAGE_SIZE) {
        if (page_bit(vm, va)) {
            page_bit_set(vm, va, 0);
            if (zero) {
                if (!run_len)
                    run_start = va;
                run_len += KMEM_PAGE_SIZE;
                vm->c.pages_zeroed++;
            }
            continue;
        }
        if (run_len) {
            be->zero(be->ctx, (uint32_t)run_start, (uint32_t)run_len);
            run_len = 0;
        }
    }
    if (run_len)
        be->zero(be->ctx, (uint32_t)run_start, (uint32_t)run_len);
}

static int region_find(const struct kmem_vm *vm, uint32_t va)
{
    int i;

    for (i = 0; i < KMEM_MAX_REGIONS; i++) {
        const struct kmem_region *r = &vm->r[i];

        if (r->size && va >= r->base && va - r->base < r->size)
            return i;
    }
    return -1;
}

static int region_overlaps(const struct kmem_vm *vm, uint64_t start, uint64_t end)
{
    int i;

    for (i = 0; i < KMEM_MAX_REGIONS; i++) {
        const struct kmem_region *r = &vm->r[i];

        if (r->size && start < (uint64_t)r->base + r->size && r->base < end)
            return 1;
    }
    return 0;
}

static int region_free_slot(const struct kmem_vm *vm)
{
    int i;

    for (i = 0; i < KMEM_MAX_REGIONS; i++)
        if (!vm->r[i].size)
            return i;
    return -1;
}

static void region_put(struct kmem_vm *vm, int slot, uint32_t base,
                       uint32_t size, uint8_t backing)
{
    vm->r[slot].base = base;
    vm->r[slot].size = size;
    vm->r[slot].backing = backing;
    vm->live++;
}

static uint32_t refuse(struct kmem_event *ev, int kind, kmem_count_t *counter,
                       uint32_t status)
{
    (*counter)++;
    ev->kind = kind;
    ev->status = status;
    return status;
}

const char *kmem_event_name(int kind)
{
    switch (kind) {
    case KMEM_EV_HINT_CONFLICT:   return "hint_conflict";
    case KMEM_EV_RESERVE_FAILED:  return "reserve_failed";
    case KMEM_EV_BAD_ALLOC:       return "bad_alloc";
    case KMEM_EV_COMMIT_REJECTED: return "commit_rejected";
    case KMEM_EV_DECOMMIT_FAILED: return "decommit_failed";
    case KMEM_EV_RELEASE_FAILED:  return "release_failed";
    case KMEM_EV_BAD_FREE_TYPE:   return "bad_free_type";
    case KMEM_EV_TABLE_FULL:      return "region_table_full";
    default:                      return "none";
    }
}

int kmem_vm_page_uncommitted(const struct kmem_vm *vm, uint32_t va)
{
    return page_bit(vm, va);
}

uint32_t kmem_vm_allocate(struct kmem_vm *vm, const struct kmem_backend *be,
                          uint32_t *base, uint32_t *size, uint32_t type,
                          struct kmem_event *ev)
{
    uint32_t req_base = *base, req_size = *size;
    uint64_t end = (uint64_t)req_base + req_size;
    int slot;

    ev->kind = KMEM_EV_NONE;
    ev->base = req_base;
    ev->size = req_size;
    ev->type = type;
    ev->status = KMEM_STATUS_SUCCESS;

    /* COMMIT or RESERVE, or RESET on a range that already exists. */
    if (!req_size || end >= KMEM_VA_LIMIT ||
        !(type & (KMEM_MEM_COMMIT | KMEM_MEM_RESERVE) ||
          (req_base && (type & KMEM_MEM_RESET))))
        return refuse(ev, KMEM_EV_BAD_ALLOC, &vm->c.alloc_invalid,
                      KMEM_STATUS_INVALID_PARAMETER);

    if (!req_base) {
        uint32_t rsize = (uint32_t)page_round_up(req_size), got;
        uint8_t backing = KMEM_BACK_HEAP;

        slot = region_free_slot(vm);
        if (slot < 0)
            return refuse(ev, KMEM_EV_TABLE_FULL, &vm->c.region_table_full,
                          KMEM_STATUS_NO_MEMORY);
        got = be->reserve_any(be->ctx, &rsize, type, &backing);
        if (!got)
            return refuse(ev, KMEM_EV_RESERVE_FAILED, &vm->c.reserve_fail,
                          KMEM_STATUS_NO_MEMORY);
        region_put(vm, slot, got, rsize, backing);
        pages_mark(vm, got, (uint64_t)got + rsize, !(type & KMEM_MEM_COMMIT));
        vm->c.reserve_ok++;
        *base = got;
        *size = rsize;
        return KMEM_STATUS_SUCCESS;
    }

    if (type & KMEM_MEM_RESERVE) {
        uint32_t start = req_base & ~(KMEM_RESERVE_ALIGN - 1u);
        uint64_t rend = page_round_up(end);
        uint32_t rsize;
        int r;

        if (rend >= KMEM_VA_LIMIT)
            return refuse(ev, KMEM_EV_BAD_ALLOC, &vm->c.alloc_invalid,
                          KMEM_STATUS_INVALID_PARAMETER);
        rsize = (uint32_t)(rend - start);
        slot = region_free_slot(vm);
        if (slot < 0)
            return refuse(ev, KMEM_EV_TABLE_FULL, &vm->c.region_table_full,
                          KMEM_STATUS_NO_MEMORY);
        r = region_overlaps(vm, start, rend) ? 0
            : be->reserve_at(be->ctx, start, rsize);
        if (r == KMEM_TABLE_FULL)
            return refuse(ev, KMEM_EV_TABLE_FULL, &vm->c.region_table_full,
                          KMEM_STATUS_NO_MEMORY);
        if (r != 1)
            return refuse(ev, KMEM_EV_HINT_CONFLICT, &vm->c.hint_conflict,
                          KMEM_STATUS_CONFLICTING_ADDRESSES);
        region_put(vm, slot, start, rsize, KMEM_BACK_HEAP);
        pages_mark(vm, start, rend, !(type & KMEM_MEM_COMMIT));
        vm->c.hint_ok++;
        *base = start;
        *size = rsize;
        return KMEM_STATUS_SUCCESS;
    }

    /* Commit (or reset) at a base: only inside something already reserved. */
    {
        uint32_t start = req_base & ~KMEM_PAGE_MASK;
        uint64_t cend = page_round_up(end);
        int idx = region_find(vm, start);

        if (idx >= 0 && cend <= (uint64_t)vm->r[idx].base + vm->r[idx].size) {
            if (type & KMEM_MEM_COMMIT)
                pages_commit(vm, be, start, cend, !(type & KMEM_MEM_NOZERO));
            vm->c.commit_in_region++;
            *base = start;
            *size = (uint32_t)(cend - start);
            return KMEM_STATUS_SUCCESS;
        }
        /* Memory the heap handed out some other way is committed already. */
        if (idx < 0 && be->heap_block_bytes &&
            be->heap_block_bytes(be->ctx, req_base) >= req_size) {
            vm->c.commit_heap_block++;
            *base = start;
            *size = (uint32_t)(cend - start);
            return KMEM_STATUS_SUCCESS;
        }
        return refuse(ev, KMEM_EV_COMMIT_REJECTED, &vm->c.commit_rejected,
                      KMEM_STATUS_CONFLICTING_ADDRESSES);
    }
}

uint32_t kmem_vm_free(struct kmem_vm *vm, const struct kmem_backend *be,
                      uint32_t *base, uint32_t *size, uint32_t type,
                      struct kmem_event *ev)
{
    uint32_t req_base = *base, req_size = *size;
    uint32_t start = req_base & ~KMEM_PAGE_MASK;
    uint64_t end = (uint64_t)req_base + req_size, rend, cend;
    int release = type == KMEM_MEM_RELEASE, idx;
    kmem_count_t *failed = release ? &vm->c.release_failed : &vm->c.decommit_failed;
    int fail_kind = release ? KMEM_EV_RELEASE_FAILED : KMEM_EV_DECOMMIT_FAILED;
    struct kmem_region r;

    ev->kind = KMEM_EV_NONE;
    ev->base = req_base;
    ev->size = req_size;
    ev->type = type;
    ev->status = KMEM_STATUS_SUCCESS;

    if (type != KMEM_MEM_DECOMMIT && type != KMEM_MEM_RELEASE)
        return refuse(ev, KMEM_EV_BAD_FREE_TYPE, &vm->c.free_bad_type,
                      KMEM_STATUS_INVALID_PARAMETER);
    if (end > KMEM_VA_LIMIT)
        return refuse(ev, fail_kind, failed, KMEM_STATUS_INVALID_PARAMETER);

    idx = region_find(vm, start);
    if (idx < 0)
        return refuse(ev, fail_kind, failed, KMEM_STATUS_MEMORY_NOT_ALLOCATED);
    r = vm->r[idx];
    rend = (uint64_t)r.base + r.size;

    if (req_size == 0) {
        if (start != r.base)
            return refuse(ev, fail_kind, failed, KMEM_STATUS_FREE_VM_NOT_AT_BASE);
        cend = rend;
    } else {
        cend = page_round_up(end);
        if (cend > rend)
            return refuse(ev, fail_kind, failed, KMEM_STATUS_UNABLE_TO_FREE_VM);
    }

    if (!release) {
        pages_mark(vm, start, cend, 1);
        vm->c.decommit_ok++;
        *base = start;
        *size = (uint32_t)(cend - start);
        return KMEM_STATUS_SUCCESS;
    }

    /* Splitting a region is not modelled: only the whole of one is released. */
    if (start != r.base || cend != rend)
        return refuse(ev, fail_kind, failed, KMEM_STATUS_INVALID_PARAMETER);

    pages_mark(vm, r.base, rend, 0);
    vm->r[idx].size = 0;
    vm->live--;
    be->release(be->ctx, r.base, r.size, r.backing);
    vm->c.release_ok++;
    *base = r.base;
    *size = r.size;
    return KMEM_STATUS_SUCCESS;
}
