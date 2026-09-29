/*
 * NV2A model contract tests: the pushbuffer walk and the PGRAPH registers the
 * guest's interrupt handler touches, driven through the real MMIO entry
 * points of nv2a_core.c against a fake physical window and instance memory.
 *
 * The D3D11 translator is replaced by a recorder so the tests can see which
 * methods would have reached it.
 */
#include "nv2a_state.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            ++g_failures;                                                   \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

/* ── translator recorder ─────────────────────────────────────────────── */

static unsigned g_translator_calls;

int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param)
{
    (void)subchannel; (void)method; (void)param;
    ++g_translator_calls;
    return 1;
}

/* ── fixture ─────────────────────────────────────────────────────────── */

/* Guest physical 0.. as the contiguous window; pushbuffers sit below 64 KiB
 * so no offset carries the kick bit. */
static uint8_t g_window[1u << 20];
static uint8_t g_ramin[64u << 10];
static uint8_t g_vram[4096];

#define PB_BASE        0x1000u
#define SEMA_PHYS      0x8000u   /* the word the semaphore DMA object covers */
#define SENTINEL       0xDEADBEEFu

#define H_KELVIN       0x0000000Du   /* small handles: the fold keeps them in a 4 KiB table */
#define H_MEMCPY       0x0000000Eu
#define H_SEMAPHORE    0x00000008u   /* the handle JSRF binds */
#define H_SEMA_SHORT   0x00000009u
#define H_SEMA_FAR     0x0000000Au

#define INST_KELVIN    0x1000u
#define INST_MEMCPY    0x1010u
#define INST_SEMAPHORE 0x1020u
#define INST_SEMA_SHORT 0x1030u
#define INST_SEMA_FAR  0x1040u

static uint32_t rd32(const uint8_t *base, uint32_t off)
{
    uint32_t v;
    memcpy(&v, base + off, 4);
    return v;
}

static void wr32(uint8_t *base, uint32_t off, uint32_t v)
{
    memcpy(base + off, &v, 4);
}

static uint32_t ramht_slot(uint32_t handle)
{
    uint32_t hash = 0;
    while (handle) {
        hash ^= handle & 0x7FFu;
        handle >>= 11;
    }
    return hash * 8u;
}

static void ramht_insert(uint32_t handle, uint32_t instance)
{
    uint32_t slot = ramht_slot(handle);
    wr32(g_ramin, slot, handle);
    wr32(g_ramin, slot + 4, NV_RAMHT_STATUS | (instance >> 4));
}

static void dma_object(uint32_t instance, uint32_t base, uint32_t limit)
{
    wr32(g_ramin, instance + 0, NV_DMA_IN_MEMORY_CLASS | ((base & 0xFFFu) << 20));
    wr32(g_ramin, instance + 4, limit);
    wr32(g_ramin, instance + 8, base & NV_DMA_ADDRESS);
    wr32(g_ramin, instance + 12, base & NV_DMA_ADDRESS);
}

static NV2AState *fresh_with(int actions)
{
    NV2AState *d;
    memset(g_window, 0, sizeof(g_window));
    memset(g_ramin, 0, sizeof(g_ramin));
    g_translator_calls = 0;
    nv2a_reset_standalone_for_test();
    nv2a_actions_override_for_test(actions);
    nv2a_bind_instance_memory(0x00F00000u, g_ramin, sizeof(g_ramin));
    d = nv2a_init_standalone(g_vram, sizeof(g_vram), NULL, 0);
    nv2a_set_pushbuffer_window(d, g_window, 0, sizeof(g_window));
    /* 4 KiB RAMHT at instance 0. */
    d->pfifo.regs[NV_PFIFO_RAMHT] = 0;
    ramht_insert(H_KELVIN, INST_KELVIN);
    wr32(g_ramin, INST_KELVIN, 0x97u);
    ramht_insert(H_MEMCPY, INST_MEMCPY);
    wr32(g_ramin, INST_MEMCPY, 0x39u);
    ramht_insert(H_SEMAPHORE, INST_SEMAPHORE);
    dma_object(INST_SEMAPHORE, SEMA_PHYS, 0xFFFu);
    /* Its limit ends two bytes into the word. */
    ramht_insert(H_SEMA_SHORT, INST_SEMA_SHORT);
    dma_object(INST_SEMA_SHORT, SEMA_PHYS, 1u);
    /* Based past the end of the physical window. */
    ramht_insert(H_SEMA_FAR, INST_SEMA_FAR);
    dma_object(INST_SEMA_FAR, sizeof(g_window), 0xFFFu);
    wr32(g_window, SEMA_PHYS, SENTINEL);
    return d;
}

static NV2AState *fresh(void)
{
    return fresh_with(0);
}

/* ── pushbuffer builder ──────────────────────────────────────────────── */

typedef struct {
    uint32_t start, at;
} Pb;

static uint32_t hdr(uint32_t subchannel, uint32_t method, uint32_t count)
{
    return (count << 18) | (subchannel << 13) | method;
}

static void pb_begin(Pb *pb, uint32_t at)
{
    pb->start = pb->at = at;
}

static void pb_word(Pb *pb, uint32_t w)
{
    wr32(g_window, pb->at, w);
    pb->at += 4;
}

static void pb_method(Pb *pb, uint32_t subchannel, uint32_t method, uint32_t param)
{
    pb_word(pb, hdr(subchannel, method, 1));
    pb_word(pb, param);
}

static void mmio_w(NV2AState *d, uint32_t addr, uint32_t v)
{
    nv2a_mmio_write(d, addr, v, 4);
}

static uint32_t mmio_r(NV2AState *d, uint32_t addr)
{
    return (uint32_t)nv2a_mmio_read(d, addr, 4);
}

#define USER(r)   (0x800000u + (r))

static uint32_t get_ptr(NV2AState *d)
{
    return mmio_r(d, USER(NV_USER_DMA_GET));
}

static const char *diag(NV2AState *d)
{
    return nv2a_submit_diagnostic(d->pfifo.submit_diag);
}

static uint32_t sema(void)
{
    return rd32(g_window, SEMA_PHYS);
}

/* Start a stream at `get` and kick it at `put`, as the guest does. */
static void kick(NV2AState *d, uint32_t get, uint32_t put)
{
    mmio_w(d, USER(NV_USER_DMA_GET), get);
    mmio_w(d, USER(NV_USER_DMA_PUT), put);
}

static void kick_put(NV2AState *d, uint32_t put)
{
    mmio_w(d, USER(NV_USER_DMA_PUT), put);
}

/* JSRF's first semaphore sequence: 0x1A4 = handle, 0x1D6C = 0, 0x1D70 = value. */
static void pb_semaphore(Pb *pb, uint32_t handle, uint32_t offset, uint32_t value)
{
    pb_method(pb, 0, NV097_SET_CONTEXT_DMA_SEMAPHORE, handle);
    pb_method(pb, 0, NV097_SET_SEMAPHORE_OFFSET, offset);
    pb_method(pb, 0, NV097_BACK_END_WRITE_SEMAPHORE_RELEASE, value);
}

/* ── tests ───────────────────────────────────────────────────────────── */

/* A subchannel bound to a non-Kelvin class never reaches the Kelvin
 * translator; Kelvin and never-bound subchannels still do. */
static void test_translator_reads_bound_class(void)
{
    NV2AState *d = fresh();
    Pb pb;
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_method(&pb, 1, 0x0000, H_MEMCPY);
    kick(d, pb.start, pb.at);
    CHECK(get_ptr(d) == pb.at, "binding stream did not commit (get=%08X)", get_ptr(d));
    CHECK(d->pfifo.binding_class[1] == 0x39u, "subchannel 1 class %02X",
          d->pfifo.binding_class[1]);

    g_translator_calls = 0;
    pgraph_method(d, 1, NV097_CLEAR_SURFACE, 0xF0);
    CHECK(g_translator_calls == 0, "memcpy-class method reached the Kelvin translator");
    pgraph_method(d, 0, NV097_CLEAR_SURFACE, 0xF0);
    CHECK(g_translator_calls == 1, "Kelvin method did not reach the translator");
    pgraph_method(d, 5, NV097_CLEAR_SURFACE, 0xF0);
    CHECK(g_translator_calls == 2, "unbound subchannel lost the Kelvin reading");
}

/* With the switch unset the release is register capture only: the stream
 * commits and guest memory is untouched. */
static void test_semaphore_dormant_when_unset(void)
{
    NV2AState *d = fresh_with(0);
    Pb pb;
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_semaphore(&pb, H_SEMAPHORE, 0, 5);
    kick(d, pb.start, pb.at);
    CHECK(get_ptr(d) == pb.at, "stream did not commit (%s)", diag(d));
    CHECK(sema() == SENTINEL, "semaphore written with the switch unset (%08X)", sema());
    CHECK(!d->pgraph.dma_semaphore_valid, "semaphore DMA latched with the switch unset");
    CHECK(d->pfifo.semaphore_releases == 0, "release counted with the switch unset");
}

/* The release is written when its stream commits, and not at all when a later
 * method rejects the stream. */
static void test_semaphore_written_only_on_commit(void)
{
    NV2AState *d = fresh_with(1);
    Pb pb;
    uint32_t second, before_bad;
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_semaphore(&pb, H_SEMAPHORE, 0, 5);
    kick(d, pb.start, pb.at);
    CHECK(get_ptr(d) == pb.at, "first stream did not commit (%s)", diag(d));
    CHECK(sema() == 5, "release not written on commit (%08X)", sema());
    CHECK(d->pgraph.regs[NV_PGRAPH_SEMAPHOREOFFSET] == 0, "offset not latched");

    second = pb.at;
    pb_method(&pb, 0, NV097_BACK_END_WRITE_SEMAPHORE_RELEASE, 7);
    before_bad = pb.at;
    pb_method(&pb, 0, 0x0104, 0);           /* not in the NV097 table */
    kick_put(d, pb.at);
    CHECK(strcmp(diag(d), "unsupported_method") == 0, "expected unsupported_method, got %s", diag(d));
    CHECK(get_ptr(d) == second, "rejected stream moved GET to %08X", get_ptr(d));
    CHECK(sema() == 5, "rejected stream wrote its release (%08X)", sema());
    CHECK(d->pfifo.semaphore_releases == 1, "rejected release counted");

    /* The same release without the bad method commits. */
    kick_put(d, before_bad);
    CHECK(get_ptr(d) == before_bad, "shortened stream did not commit (%s)", diag(d));
    CHECK(sema() == 7, "release after the retry not written (%08X)", sema());
}

/* A release that does not fit its DMA object, or lands outside the physical
 * window, or names no object, stops the stream with nothing written. */
static void test_semaphore_bounds(void)
{
    static const struct { uint32_t handle, offset; const char *want; } cases[] = {
        { H_SEMA_SHORT, 0, "semaphore_fault" },     /* limit 1 < the dword */
        { H_SEMAPHORE, 0xFFE, "semaphore_fault" },  /* crosses the limit */
        { H_SEMAPHORE, 2, "semaphore_fault" },      /* misaligned */
        { H_SEMA_FAR, 0, "semaphore_fault" },       /* outside the window */
        { 0x00000077u, 0, "invalid_handle" },       /* not in RAMHT */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        NV2AState *d = fresh_with(1);
        Pb pb;
        pb_begin(&pb, PB_BASE);
        pb_method(&pb, 0, 0x0000, H_KELVIN);
        pb_semaphore(&pb, cases[i].handle, cases[i].offset, 5);
        kick(d, pb.start, pb.at);
        CHECK(strcmp(diag(d), cases[i].want) == 0, "case %u: diag %s, want %s",
              i, diag(d), cases[i].want);
        CHECK(get_ptr(d) == pb.start, "case %u: GET moved to %08X", i, get_ptr(d));
        CHECK(sema() == SENTINEL && rd32(g_window, SEMA_PHYS + 0xFFC) == 0,
              "case %u: something was written", i);
        CHECK(d->pfifo.binding_class[0] == 0, "case %u: binding committed", i);
    }
}

/* The fence mirror's write over a word the release owns is counted. */
static void test_fence_mirror_overlap_reported(void)
{
    NV2AState *d = fresh_with(1);
    Pb pb;
    uint32_t before = nv2a_fence_mirror_overlaps();
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_semaphore(&pb, H_SEMAPHORE, 0, 5);
    kick(d, pb.start, pb.at);
    nv2a_note_fence_mirror_write(g_window + SEMA_PHYS + 4, 9);
    CHECK(nv2a_fence_mirror_overlaps() == before, "a different word counted as overlap");
    nv2a_note_fence_mirror_write(g_window + SEMA_PHYS, 9);
    CHECK(nv2a_fence_mirror_overlaps() == before + 1, "overlap not counted");
}

int main(void)
{
    test_translator_reads_bound_class();
    test_semaphore_dormant_when_unset();
    test_semaphore_written_only_on_commit();
    test_semaphore_bounds();
    test_fence_mirror_overlap_reported();

    if (g_failures) {
        fprintf(stderr, "nv2a_actions_test: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "nv2a_actions_test: all checks passed\n");
    return 0;
}
