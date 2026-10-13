/*
 * fence_release_runtime_test.c - xbox_Nv2aFenceRelease and the fence mirror tick,
 * driven through the real memory layout and the real executor entry point.
 *
 * A release (NV2A method 0x1D70) must write its parameter, verbatim, into the
 * word of every mirror registered with XBOX_FENCE_MIRROR_RELEASES, and after
 * that the mirror tick (the layout's worker thread) must leave the word alone.
 * RECOMP_FENCE_MIRROR_LIVE restores the old mirror: nothing latches.
 *
 * Links the whole kernel, so it is registered by the game project
 * (jsrf_fence_release_runtime*). One process per mode, because live mode is read once:
 *     fence_release_runtime_test normal
 *     RECOMP_FENCE_MIRROR_LIVE=1 fence_release_runtime_test live
 */
#include "xbox_memory_layout.h"
#include "nv2a_backend.h"

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Declared by extern at its own callers (nv2a_pb_scan.c), not in a header. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);

/* Game-side hooks the kernel library calls and this fixture does not link:
 * inert, as in kernel_dpc_queue_test.c. */
typedef void (*recomp_func_t)(void);
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

static int g_checks, g_failures;

#define CHECK(cond) do {                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

/* Guest layout: each mirror is a global holding a device pointer; the device
 * has the counter at +0x30 and the pointer to the notify word at +0x34. */
#define DEV_GLOBAL_N   0x01F00000u   /* not opted in (xbox_Nv2aMirrorFence) */
#define DEV_GLOBAL_N2  0x01F00010u   /* flags without the release bit */
#define DEV_GLOBAL_U   0x01F00020u   /* opted in, device never created */
#define DEV_GLOBAL_O   0x01F00030u   /* opted in, fully readable */
#define DEV_N          0x01F01000u
#define DEV_N2         0x01F02000u
#define DEV_O          0x01F03000u
#define WORD_N         0x01F04000u
#define WORD_N2        0x01F04010u
#define WORD_O         0x01F04020u
#define SEM_TARGET     0x01F04030u
#define SRC_OFF        0x30u
#define PTR_OFF        0x34u

static volatile uint32_t *gp(uint32_t va)
{
    return (volatile uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + va);
}

static int wait_for(uint32_t va, uint32_t want)
{
    int i;
    for (i = 0; i < 300; i++) {          /* up to ~3 s for the worker tick */
        if (*gp(va) == want) return 1;
        Sleep(10);
    }
    return 0;
}

/* Long enough for many worker ticks (it yields with Sleep(0)). */
static void let_ticks_run(void) { Sleep(150); }

static void setup_device(uint32_t global, uint32_t dev, uint32_t word, uint32_t counter)
{
    *gp(global) = dev;
    *gp(dev + SRC_OFF) = counter;
    *gp(dev + PTR_OFF) = word;
    *gp(word) = 0xDEADBEEFu;
}

static void run_normal(void)
{
    /* Mirrors without an opted-in, readable chain: a release writes nothing. */
    CHECK(xbox_Nv2aMirrorFence(DEV_GLOBAL_N, SRC_OFF, PTR_OFF) == 0);
    CHECK(xbox_Nv2aMirrorFenceEx(DEV_GLOBAL_N2, SRC_OFF, PTR_OFF, 0x2u) == 0);
    CHECK(xbox_Nv2aMirrorFenceEx(DEV_GLOBAL_U, SRC_OFF, PTR_OFF,
                                 XBOX_FENCE_MIRROR_RELEASES) == 0);
    setup_device(DEV_GLOBAL_N, DEV_N, WORD_N, 11);
    setup_device(DEV_GLOBAL_N2, DEV_N2, WORD_N2, 21);
    *gp(DEV_GLOBAL_U) = 0;                       /* device not created yet */

    /* (6) the plain mirror, and one with other flag bits, follow the counter. */
    CHECK(wait_for(WORD_N, 11));
    CHECK(wait_for(WORD_N2, 21));

    CHECK(xbox_Nv2aFenceRelease(5) == 0);        /* no opted-in readable chain */
    CHECK(xbox_Nv2aFenceReleaseCount() == 0);
    CHECK(*gp(WORD_N) == 11);
    CHECK(*gp(WORD_N2) == 21);

    /* Now the opted-in mirror with a readable chain. */
    CHECK(xbox_Nv2aMirrorFenceEx(DEV_GLOBAL_O, SRC_OFF, PTR_OFF,
                                 XBOX_FENCE_MIRROR_RELEASES) == 0);
    setup_device(DEV_GLOBAL_O, DEV_O, WORD_O, 100);

    /* Before a release the tick drives it with the live counter (the fallback). */
    CHECK(wait_for(WORD_O, 100));
    *gp(DEV_O + SRC_OFF) = 104;
    CHECK(wait_for(WORD_O, 104));

    /* (7) a release through the executor entry point (no semaphore target). */
    nv2a_pb_exec_method(0, 0x1D70, 102);
    CHECK(*gp(WORD_O) == 102);                   /* verbatim, at once */
    CHECK(xbox_Nv2aFenceReleaseCount() == 1);

    /* (8) the tick no longer writes it, though the counter moves on. */
    *gp(DEV_O + SRC_OFF) = 106;
    let_ticks_run();
    CHECK(*gp(WORD_O) == 102);

    /* Non-opted mirrors are never written by a release, and keep ticking. */
    CHECK(*gp(WORD_N) == 11);
    CHECK(*gp(WORD_N2) == 21);
    *gp(DEV_N + SRC_OFF) = 12;
    CHECK(wait_for(WORD_N, 12));
    *gp(DEV_N2 + SRC_OFF) = 22;
    CHECK(wait_for(WORD_N2, 22));

    /* Direct call: returns 1, verbatim, in stream order, backwards and across wrap. */
    CHECK(xbox_Nv2aFenceRelease(0xFFFFFFFEu) == 1);
    CHECK(*gp(WORD_O) == 0xFFFFFFFEu);
    CHECK(xbox_Nv2aFenceRelease(2) == 1);
    CHECK(*gp(WORD_O) == 2);
    CHECK(xbox_Nv2aFenceRelease(0) == 1);
    CHECK(*gp(WORD_O) == 0);
    CHECK(xbox_Nv2aFenceReleaseCount() == 4);
    let_ticks_run();
    CHECK(*gp(WORD_O) == 0);
    CHECK(*gp(WORD_N) == 12);                    /* still the counter, not a release value */
    CHECK(*gp(WORD_N2) == 22);

    /* With a semaphore target set the executor writes the target and does not
     * hand the release to the mirror. */
    *gp(WORD_O) = 77;
    *gp(SEM_TARGET) = 0;
    nv2a_pb_set_semaphore_target(SEM_TARGET);
    nv2a_pb_exec_method(0, 0x1D70, 55);
    CHECK(*gp(SEM_TARGET) == 55);
    CHECK(*gp(WORD_O) == 77);
    CHECK(xbox_Nv2aFenceReleaseCount() == 4);
    nv2a_pb_set_semaphore_target(0);
}

static void run_live(void)
{
    CHECK(xbox_Nv2aMirrorFenceEx(DEV_GLOBAL_O, SRC_OFF, PTR_OFF,
                                 XBOX_FENCE_MIRROR_RELEASES) == 0);
    setup_device(DEV_GLOBAL_O, DEV_O, WORD_O, 100);
    CHECK(wait_for(WORD_O, 100));

    /* (9) nothing latches and the release writes nothing. */
    CHECK(xbox_Nv2aFenceRelease(98) == 0);
    CHECK(xbox_Nv2aFenceReleaseCount() == 0);
    CHECK(*gp(WORD_O) == 100);

    /* The tick keeps writing the live counter, release or no release. */
    *gp(DEV_O + SRC_OFF) = 104;
    CHECK(wait_for(WORD_O, 104));
    CHECK(xbox_Nv2aFenceRelease(102) == 0);
    CHECK(*gp(WORD_O) == 104);
    *gp(DEV_O + SRC_OFF) = 106;
    CHECK(wait_for(WORD_O, 106));
    CHECK(xbox_Nv2aFenceReleaseCount() == 0);
}

/* No sections, no TLS directory: enough for xbox_MemoryLayoutInit. */
static uint8_t *make_synthetic_xbe(size_t *out_size)
{
    static uint8_t xbe[0x2000];
    memset(xbe, 0, sizeof(xbe));
    *(uint32_t *)(xbe + 0x0104) = XBOX_BASE_ADDRESS;
    *(uint32_t *)(xbe + 0x0108) = 0x1000;
    *(uint32_t *)(xbe + 0x0120) = XBOX_BASE_ADDRESS + 0x200;
    *out_size = sizeof(xbe);
    return xbe;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "normal";
    size_t xbe_size = 0;
    uint8_t *xbe = make_synthetic_xbe(&xbe_size);
    int live = strcmp(mode, "live") == 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (!live && strcmp(mode, "normal") != 0) {
        fprintf(stderr, "usage: %s [normal|live]\n", argv[0]);
        return 2;
    }
    if ((getenv("RECOMP_FENCE_MIRROR_LIVE") != NULL) != live) {
        fprintf(stderr, "mode %s needs RECOMP_FENCE_MIRROR_LIVE %s\n", mode,
                live ? "set" : "unset");
        return 2;
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_size)) {
        fprintf(stderr, "xbox_MemoryLayoutInit failed\n");
        return 2;
    }

    if (live) run_live(); else run_normal();

    printf("fence_release_runtime_test (%s): %d checks, %d failed\n", mode, g_checks, g_failures);
    fflush(stdout);
    return g_failures ? 1 : 0;   /* no shutdown: the worker thread is still ticking */
}
