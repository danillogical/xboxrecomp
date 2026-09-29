/*
 * kernel_vm.c - NtAllocateVirtualMemory / NtFreeVirtualMemory over guest memory
 *
 * The region registry and the NT decisions live in kmem.c; this file gives
 * them the guest's allocators, a lock, and log lines. The bridges in
 * kernel_bridge.c read and write the guest's 32-bit BaseAddress and
 * RegionSize and call in here unless RECOMP_KMEM_LEGACY is set.
 */

#include "kernel.h"
#include "xbox_memory_layout.h"
#include "kmem.h"

#include <stdio.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;

static struct kmem_vm g_vm;
static SRWLOCK g_vm_lock = SRWLOCK_INIT;

/* Base 0: the heap first, 64 KB-aligned. A pure reservation the heap cannot
 * back goes above RAM, then is clamped, exactly as the bridge always did; the
 * region records whatever size was granted, so a commit past it is refused. */
static uint32_t vm_reserve_any(void *ctx, uint32_t *size, uint32_t type,
                               uint8_t *backing)
{
    int pure = (type & KMEM_MEM_RESERVE) && !(type & KMEM_MEM_COMMIT);
    uint32_t want = *size;
    uint32_t va = xbox_HeapAlloc(want, KMEM_RESERVE_ALIGN);

    (void)ctx;
    *backing = KMEM_BACK_HEAP;
    if (!va && pure) {
        va = xbox_ReserveAlloc(want, KMEM_RESERVE_ALIGN);
        if (va) {
            *backing = KMEM_BACK_ABOVE_RAM;
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u"
                            " granted at 0x%08X above RAM\n", want, va);
            fflush(stderr);
        }
    }
    if (!va && pure) {
        while (want > 0x10000 && !va) {
            want = (want / 2) & ~(KMEM_PAGE_SIZE - 1u);
            va = xbox_HeapAlloc(want, KMEM_RESERVE_ALIGN);
        }
        if (va) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u "
                            "clamped to %u (heap cannot back the full range)\n",
                    *size, want);
            fflush(stderr);
            *size = want;
        }
    }
    return va;
}

static int vm_reserve_at(void *ctx, uint32_t base, uint32_t size)
{
    (void)ctx;
    return xbox_HeapReserveAt(base, size);
}

/* Above-RAM reservations come from a bump arena with no free: the address
 * space is not reused, which costs no RAM. */
static void vm_release(void *ctx, uint32_t base, uint32_t size, uint8_t backing)
{
    (void)ctx;
    (void)size;
    if (backing == KMEM_BACK_HEAP)
        xbox_HeapFree(base);
}

static uint32_t vm_heap_block_bytes(void *ctx, uint32_t va)
{
    (void)ctx;
    return xbox_HeapBlockSize(va);
}

static void vm_zero(void *ctx, uint32_t va, uint32_t len)
{
    (void)ctx;
    memset((void *)((uintptr_t)va + g_xbox_mem_offset), 0, len);
}

static const struct kmem_backend g_vm_backend = {
    NULL, vm_reserve_any, vm_reserve_at, vm_release,
    vm_heap_block_bytes, vm_zero
};

/* First few refusals of each kind, as observation; the counters are the
 * record. */
static void vm_log_refusal(const struct kmem_event *ev)
{
    static int logged[KMEM_EV_COUNT];

    if (ev->kind <= KMEM_EV_NONE || ev->kind >= KMEM_EV_COUNT ||
        logged[ev->kind] >= 8)
        return;
    logged[ev->kind]++;
    fprintf(stderr, "  [KMEM] reject kind=%s base=0x%08X size=0x%08X type=0x%X"
                    " status=0x%08X\n", kmem_event_name(ev->kind), ev->base,
            ev->size, ev->type, ev->status);
    fflush(stderr);
}

uint32_t xbox_VmAllocate(uint32_t *base, uint32_t *size, uint32_t type)
{
    struct kmem_event ev;
    uint32_t status;

    AcquireSRWLockExclusive(&g_vm_lock);
    status = kmem_vm_allocate(&g_vm, &g_vm_backend, base, size, type, &ev);
    ReleaseSRWLockExclusive(&g_vm_lock);
    vm_log_refusal(&ev);
    return status;
}

uint32_t xbox_VmFree(uint32_t *base, uint32_t *size, uint32_t type)
{
    struct kmem_event ev;
    uint32_t status;

    AcquireSRWLockExclusive(&g_vm_lock);
    status = kmem_vm_free(&g_vm, &g_vm_backend, base, size, type, &ev);
    ReleaseSRWLockExclusive(&g_vm_lock);
    vm_log_refusal(&ev);
    return status;
}

void xbox_KmemLogSummary(void)
{
    struct kmem_vm_counters c;
    struct kmem_alloc_counters a;
    int live;

    if (xbox_KmemLegacy()) {
        fprintf(stderr, "  [KMEM] summary legacy=1\n");
        return;
    }
    AcquireSRWLockShared(&g_vm_lock);
    c = g_vm.c;
    live = g_vm.live;
    ReleaseSRWLockShared(&g_vm_lock);
    xbox_KmemAllocCounters(&a);

    fprintf(stderr,
            "  [KMEM] summary legacy=0 regions=%d hint_ok=%llu hint_conflict=%llu"
            " reserve_ok=%llu reserve_fail=%llu alloc_invalid=%llu"
            " commit_region=%llu commit_heap_block=%llu commit_rejected=%llu"
            " pages_zeroed=%llu decommit_ok=%llu decommit_failed=%llu"
            " release_ok=%llu release_failed=%llu free_bad_type=%llu"
            " region_table_full=%llu contig_free_ok=%llu contig_free_unknown=%llu"
            " contig_untracked=%llu contig_split_skipped=%llu heap_split=%llu"
            " heap_split_full=%llu heap_carve_ok=%llu heap_carve_busy=%llu"
            " heap_carve_full=%llu\n",
            live, c.hint_ok, c.hint_conflict, c.reserve_ok, c.reserve_fail,
            c.alloc_invalid, c.commit_in_region, c.commit_heap_block,
            c.commit_rejected, c.pages_zeroed, c.decommit_ok, c.decommit_failed,
            c.release_ok, c.release_failed, c.free_bad_type, c.region_table_full,
            a.contig_free_ok, a.contig_free_unknown, a.contig_untracked,
            a.contig_split_skipped, a.heap_split, a.heap_split_full,
            a.heap_carve_ok, a.heap_carve_busy, a.heap_carve_full);
}
