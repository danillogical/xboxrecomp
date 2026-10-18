/* A guest range's host protection belongs to that allocation, not to the
 * address: a later owner of the same pages must get them committed, writable
 * and zeroed.
 *
 * THE BUG THIS REPRODUCES
 *   The guest's NtProtectVirtualMemory reaches the host VirtualProtect. JSRF
 *   makes a heap buffer read-only and frees it; the next 64 KB allocation that
 *   reuses the block then faults while it zero-fills, a host write access
 *   violation inside the allocate call.
 *
 * CONTRACT (Xbox NtAllocateVirtualMemory / NtFreeVirtualMemory semantics, as
 * implemented by xbox_VmAllocate / xbox_VmFree)
 *   - committed pages are readable, writable and read as zero;
 *   - after MEM_RELEASE, an allocation that reuses the address is committed,
 *     writable and zeroed, whatever protection the old allocation had;
 *   - after MEM_DECOMMIT of part of a range, MEM_COMMIT gives writable, zeroed
 *     pages, whatever protection they had before.
 *
 * TWO CASES, RUN AS SEPARATE PROCESSES
 *   argv[1] = "release"   reserve+commit 64 KB, make it read-only, release,
 *                         re-reserve (must return the same address), commit 4 KB.
 *   argv[1] = "decommit"  reserve+commit 64 KB, make it read-only, decommit the
 *                         first 16 KB, commit it again.
 *   Separate processes because a fault inside the allocate call can leave the
 *   allocator's locks held, which would hang the next case.
 *
 * Each case runs under __try/__except: a fault is reported as a failure, not a
 * crash. Ranges are 64 KB-aligned and in multiples of 16 KB so they are also
 * whole host pages on a 16 KB-page host.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "d3d/d3d8_xbox.h"   /* IDirect3DDevice8, for the inert D3D stub below */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* Inert stubs: the real definitions live in the game, which this standalone
 * fixture does not link. Same list as dir_context_release_test.c. */
typedef void (*recomp_func_t)(void);

/* Inert stubs: the real definitions live in the game, which this standalone
 * fixture does not link. Same approach as kernel_dpc_queue_test.c. */
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }
void recomp_diag_thread_start(uint32_t start, uint32_t low, uint32_t high)
{ (void)start; (void)low; (void)high; }
void recomp_diag_thread_end(void) {}
void recomp_diag_record(uint32_t kind, uint32_t target, uint32_t site, uint32_t value)
{ (void)kind; (void)target; (void)site; (void)value; }
void jsrf_slot_latch_install(uint32_t raw_value, uint32_t installed_value)
{ (void)raw_value; (void)installed_value; }
void jsrf_slot_latch_sample(uint32_t tid, uint32_t call_index, uint32_t ordinal,
                            uint32_t before, uint32_t after)
{ (void)tid; (void)call_index; (void)ordinal; (void)before; (void)after; }
void jsrf_slot_watch_handshake(uint32_t slot_va) { (void)slot_va; }
void jsrf_slot_watch_alias_armed(uint32_t mapped_mask, uint32_t protect_mask, uint32_t alias_count)
{ (void)mapped_mask; (void)protect_mask; (void)alias_count; }
int jsrf_slot_watch_alias_touch(uint32_t alias_index, uint32_t fault_va, uint64_t rip,
                                uint32_t value, uint32_t published)
{ (void)alias_index; (void)fault_va; (void)rip; (void)value; (void)published; return 0; }
void jsrf_slot_watch_write(uint32_t provenance, uint32_t before, uint32_t after,
                           uint64_t rip, uint32_t ordinal)
{ (void)provenance; (void)before; (void)after; (void)rip; (void)ordinal; }

/* Video/framebuffer and D3D symbols referenced by kernel_bridge.c and
 * nv2a_pb_exec.c but defined by the GAME. They are only reached on paths this
 * fixture never takes, so inert stubs are correct here. */
int  xbox_VideoIsPlaying(void) { return 0; }
int  xbox_VideoPlayFile(const char *host_path) { (void)host_path; return 0; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowPresent(void) {}
uint32_t xbox_FramebufferPresentSerial(void) { return 0; }
unsigned long long xbox_FramebufferPresentHash(void) { return 0; }
void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars) { (void)name; (void)max_chars; }
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
IDirect3DDevice8 *xbox_GetD3DDevice(void) { return NULL; }

#define BLOCK_SIZE   0x10000u    /* 64 KB */
#define DECOMMIT_LEN 0x4000u     /* 16 KB */
#define COMMIT_4K    0x1000u

#define VM_TYPE_RESERVE_COMMIT (MEM_RESERVE | MEM_COMMIT)

static unsigned checks;

static int check(int condition, const char *name)
{
    ++checks;
    if (!condition) fprintf(stderr, "FAIL: %s\n", name);
    return condition;
}

static int nt_ok(uint32_t status) { return (int32_t)status >= 0; }

static volatile uint8_t *host(uint32_t va)
{
    return (volatile uint8_t *)((uintptr_t)va + (uintptr_t)xbox_GetMemoryOffset());
}

/* Every byte of [va, va+len) reads zero. */
static int all_zero(uint32_t va, uint32_t len)
{
    volatile uint8_t *p = host(va);
    uint32_t i;
    for (i = 0; i < len; i++)
        if (p[i] != 0) return 0;
    return 1;
}

/* Write a pattern at the first and last byte of [va, va+len), read it back,
 * and check the bytes between still read zero. */
static int writable_and_zeroed(uint32_t va, uint32_t len, const char *what)
{
    volatile uint8_t *p = host(va);
    char name[128];
    int ok = 1;

    snprintf(name, sizeof name, "%s: reads as zero before the write", what);
    ok &= check(all_zero(va, len), name);
    p[0] = 0x5A;
    p[len - 1] = 0xA5;
    snprintf(name, sizeof name, "%s: write reads back", what);
    ok &= check(p[0] == 0x5A && p[len - 1] == 0xA5, name);
    snprintf(name, sizeof name, "%s: the rest still reads zero", what);
    ok &= check(all_zero(va + 1, len - 2), name);
    return ok;
}

static int make_readonly(uint32_t va, uint32_t len)
{
    DWORD old = 0;
    return VirtualProtect((void *)host(va), len, PAGE_READONLY, &old) != 0;
}

static int case_release_then_reuse(void)
{
    uint32_t a = 0, size = BLOCK_SIZE, st;
    uint32_t b = 0, size2 = BLOCK_SIZE;
    uint32_t cb, cs = COMMIT_4K;
    int ok = 1;

    st = xbox_VmAllocate(&a, &size, VM_TYPE_RESERVE_COMMIT);
    if (!check(nt_ok(st) && a != 0, "reserve+commit 64 KB at base 0")) return 0;
    printf("  first allocation A = 0x%08X size 0x%X\n", a, size);

    ok &= check(make_readonly(a, BLOCK_SIZE), "VirtualProtect(A, 64 KB, PAGE_READONLY)");

    {
        uint32_t fb = a, fs = 0;
        ok &= check(nt_ok(xbox_VmFree(&fb, &fs, MEM_RELEASE)), "MEM_RELEASE of A succeeds");
    }

    /* Reuse of the address is the precondition: without it the case proves
     * nothing about a stale host protection. */
    st = xbox_VmAllocate(&b, &size2, MEM_RESERVE);
    if (!check(nt_ok(st), "re-reserve 64 KB at base 0 succeeds")) return 0;
    printf("  second reservation B = 0x%08X\n", b);
    if (!check(b == a, "the re-reservation returned A (precondition)")) {
        fprintf(stderr, "FAIL: the case proves nothing unless the address is reused\n");
        return 0;
    }

    cb = b;
    st = xbox_VmAllocate(&cb, &cs, MEM_COMMIT);
    ok &= check(nt_ok(st) && cb == a, "commit 4 KB at A succeeds");
    if (!ok) return 0;
    ok &= writable_and_zeroed(a, COMMIT_4K, "reused 4 KB at A");
    return ok;
}

static int case_decommit_then_recommit(void)
{
    uint32_t a = 0, size = BLOCK_SIZE, st;
    uint32_t db, ds = DECOMMIT_LEN, cb, cs = DECOMMIT_LEN;
    int ok = 1;

    st = xbox_VmAllocate(&a, &size, VM_TYPE_RESERVE_COMMIT);
    if (!check(nt_ok(st) && a != 0, "reserve+commit 64 KB at base 0")) return 0;
    printf("  allocation A = 0x%08X size 0x%X\n", a, size);

    ok &= check(make_readonly(a, BLOCK_SIZE), "VirtualProtect(A, 64 KB, PAGE_READONLY)");

    db = a;
    ok &= check(nt_ok(xbox_VmFree(&db, &ds, MEM_DECOMMIT)),
                "MEM_DECOMMIT of the first 16 KB succeeds");

    cb = a;
    st = xbox_VmAllocate(&cb, &cs, MEM_COMMIT);
    ok &= check(nt_ok(st) && cb == a, "MEM_COMMIT of the first 16 KB succeeds");
    if (!ok) return 0;

    ok &= writable_and_zeroed(a, DECOMMIT_LEN, "recommitted first 16 KB");
    /* The untouched tail was never decommitted; it must still read zero. */
    ok &= check(all_zero(a + DECOMMIT_LEN, BLOCK_SIZE - DECOMMIT_LEN),
                "the remaining 48 KB reads zero");
    return ok;
}

/* A fault inside the allocate or commit call is a failure of the contract. */
static int run_guarded(int (*fn)(void), const char *name)
{
    int ok = 0;
    __try {
        ok = fn();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "FAIL: %s faulted (exception 0x%08lX)\n", name,
                (unsigned long)GetExceptionCode());
        ok = 0;
    }
    return ok;
}

/* No sections, no TLS directory: enough for xbox_MemoryLayoutInit to build the
 * guest mapping and heap, which is all this fixture needs. */
static uint8_t *make_synthetic_xbe(size_t *out_size)
{
    static uint8_t xbe[0x2000];
    memset(xbe, 0, sizeof(xbe));
    *(uint32_t *)(xbe + 0x0104) = XBOX_BASE_ADDRESS;
    *(uint32_t *)(xbe + 0x0108) = 0x1000;
    *(uint32_t *)(xbe + 0x011C) = 0;
    *(uint32_t *)(xbe + 0x0120) = XBOX_BASE_ADDRESS + 0x200;
    *(uint32_t *)(xbe + 0x012C) = 0;
    *(uint32_t *)(xbe + 0x0158) = 0;
    *out_size = sizeof(xbe);
    return xbe;
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "release";
    size_t xbe_size = 0;
    uint8_t *xbe;
    int ok;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (strcmp(mode, "release") != 0 && strcmp(mode, "decommit") != 0) {
        fprintf(stderr, "usage: %s [release|decommit]\n", argv[0]);
        return 2;
    }

    xbe = make_synthetic_xbe(&xbe_size);
    if (!xbox_MemoryLayoutInit(xbe, xbe_size)) {
        fprintf(stderr, "xbox_MemoryLayoutInit failed\n");
        return 2;
    }

    printf("mode=%s\n", mode);
    if (strcmp(mode, "release") == 0)
        ok = run_guarded(case_release_then_reuse, "release-then-reuse");
    else
        ok = run_guarded(case_decommit_then_recommit, "decommit-then-recommit");

    printf("%s: %u vm-reuse checks run (mode=%s)\n", ok ? "PASS" : "FAIL", checks, mode);
    fflush(stdout);
    /* No shutdown: after a fault the allocator may hold its locks. */
    return ok ? 0 : 1;
}
