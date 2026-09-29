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

static uint32_t hdr_noninc(uint32_t subchannel, uint32_t method, uint32_t count)
{
    return 0x40000000u | hdr(subchannel, method, count);
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
#define PGRAPH(r) (0x400000u + (r))
#define PMC(r)    (0x000000u + (r))

/* The card's interrupt line as the kernel would see it. */
static int g_line;
static unsigned g_line_edges;

static void irq_sink(void *opaque, int asserted)
{
    (void)opaque;
    g_line = asserted;
    ++g_line_edges;
}

/* What JSRF's device setup (0x00194780) and D3D's debug-register setup leave
 * behind: all PGRAPH sources enabled, the hardware master enable on, data
 * checking on, and PGRAPH FIFO access on. */
static void guest_setup(NV2AState *d, int data_check)
{
    g_line = 0;
    g_line_edges = 0;
    nv2a_set_irq_sink(d, irq_sink, NULL);
    mmio_w(d, PGRAPH(NV_PGRAPH_DEBUG_3), data_check ? 0xF3DE0479u : 0xF3CE0479u);
    mmio_w(d, PGRAPH(NV_PGRAPH_INTR), 0xFFFFFFFFu);
    mmio_w(d, PGRAPH(NV_PGRAPH_INTR_EN), 0xFFFFFFFFu);
    mmio_w(d, PMC(NV_PMC_INTR_EN_0), 1);
    mmio_w(d, PGRAPH(NV_PGRAPH_FIFO), NV_PGRAPH_FIFO_ACCESS);
}

/* The acknowledgement half of JSRF's PGRAPH ISR (0x00194210): FIFO off on
 * entry, write back the pending bits it read, FIFO on at exit. `between` runs
 * where the ISR calls the software-method handler. */
static void guest_isr(NV2AState *d, void (*between)(NV2AState *))
{
    uint32_t intr;
    mmio_w(d, PGRAPH(NV_PGRAPH_FIFO), 0);
    intr = mmio_r(d, PGRAPH(NV_PGRAPH_INTR));
    mmio_w(d, PGRAPH(NV_PGRAPH_INTR), intr);
    if (between) between(d);
    mmio_w(d, PGRAPH(NV_PGRAPH_FIFO), NV_PGRAPH_FIFO_ACCESS);
}

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

/* A non-zero NOP on Kelvin stops the walk after itself, raises the ERROR
 * interrupt through PMC to the line, latches what the handler reads, holds
 * every later kick, and resumes only after both acknowledgements. */
static uint32_t g_get_seen_in_isr;
static int g_line_seen_in_isr;

static void observe_hold(NV2AState *d)
{
    /* Between the W1C and FIFO re-enable the walk must still be held. */
    g_get_seen_in_isr = get_ptr(d);
    g_line_seen_in_isr = g_line;
}

static void test_software_method_trap(void)
{
    NV2AState *d = fresh_with(1);
    Pb pb;
    uint32_t after_nop;
    guest_setup(d, 1);
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_method(&pb, 0, NV097_SET_ZSTENCIL_CLEAR_VALUE, 0x00400B80u);   /* D3D: register */
    pb_method(&pb, 0, NV097_SET_COLOR_CLEAR_VALUE, 0x45EAD10Eu);      /* D3D: value */
    pb_method(&pb, 0, NV097_NO_OPERATION, 0x324);
    after_nop = pb.at;
    pb_semaphore(&pb, H_SEMAPHORE, 0, 7);
    kick(d, pb.start, pb.at);

    CHECK(strcmp(diag(d), "software_method_trap") == 0, "diag %s", diag(d));
    CHECK(get_ptr(d) == after_nop, "GET %08X, want just past the NOP %08X", get_ptr(d), after_nop);
    CHECK(sema() == SENTINEL, "release after the trap ran early (%08X)", sema());
    CHECK(mmio_r(d, PGRAPH(NV_PGRAPH_INTR)) & NV_PGRAPH_INTR_ERROR, "ERROR not pending");
    CHECK(mmio_r(d, PGRAPH(NV_PGRAPH_NSOURCE)) == NV_PGRAPH_NSOURCE_DATA_ERROR,
          "NSOURCE %08X", mmio_r(d, PGRAPH(NV_PGRAPH_NSOURCE)));
    CHECK((mmio_r(d, PGRAPH(NV_PGRAPH_TRAPPED_ADDR)) & 0x1FFCu) == 0x100 &&
          (mmio_r(d, PGRAPH(NV_PGRAPH_TRAPPED_ADDR)) >> 16) == 0,
          "TRAPPED_ADDR %08X", mmio_r(d, PGRAPH(NV_PGRAPH_TRAPPED_ADDR)));
    CHECK(mmio_r(d, PGRAPH(NV_PGRAPH_TRAPPED_DATA_LOW)) == 0x324, "TRAPPED_DATA %08X",
          mmio_r(d, PGRAPH(NV_PGRAPH_TRAPPED_DATA_LOW)));
    CHECK(mmio_r(d, PGRAPH(NV_PGRAPH_ZSTENCILCLEARVALUE)) == 0x00400B80u &&
          mmio_r(d, PGRAPH(NV_PGRAPH_COLORCLEARVALUE)) == 0x45EAD10Eu,
          "0x401A88/0x40186C not latched from 0x1D8C/0x1D90");
    CHECK(!(mmio_r(d, PGRAPH(NV_PGRAPH_FIFO)) & NV_PGRAPH_FIFO_ACCESS), "FIFO access left on");
    CHECK(mmio_r(d, PMC(NV_PMC_INTR_0)) & NV_PMC_INTR_0_PGRAPH, "PMC PGRAPH bit not set");
    CHECK(g_line == 1, "interrupt line not asserted");

    /* A kick while trapped walks nothing. */
    kick_put(d, pb.at);
    CHECK(strcmp(diag(d), "held_software_method") == 0, "diag %s", diag(d));
    CHECK(get_ptr(d) == after_nop && sema() == SENTINEL, "held walk moved");

    guest_isr(d, observe_hold);
    CHECK(g_get_seen_in_isr == after_nop, "walk resumed on the W1C alone");
    CHECK(g_line_seen_in_isr == 0, "line still asserted after the W1C");
    CHECK(get_ptr(d) == pb.at, "walk did not resume after FIFO re-enable (GET %08X, %s)",
          get_ptr(d), diag(d));
    CHECK(sema() == 7, "release after the trap not written (%08X)", sema());
    CHECK(mmio_r(d, PGRAPH(NV_PGRAPH_NSOURCE)) == 0, "NSOURCE not cleared with ERROR");
    CHECK(d->pfifo.software_method_traps == 1, "trap count %u", d->pfifo.software_method_traps);
}

/* Enabling FIFO access first and clearing the interrupt second also resumes. */
static void test_trap_acks_in_either_order(void)
{
    NV2AState *d = fresh_with(1);
    Pb pb;
    guest_setup(d, 1);
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_method(&pb, 0, NV097_NO_OPERATION, 0x300);
    pb_semaphore(&pb, H_SEMAPHORE, 0, 3);
    kick(d, pb.start, pb.at);
    mmio_w(d, PGRAPH(NV_PGRAPH_FIFO), NV_PGRAPH_FIFO_ACCESS);
    CHECK(get_ptr(d) != pb.at, "resumed with ERROR still pending");
    mmio_w(d, PGRAPH(NV_PGRAPH_INTR), NV_PGRAPH_INTR_ERROR);
    CHECK(get_ptr(d) == pb.at && sema() == 3, "did not resume (%s)", diag(d));
}

/* A trap inside a packet resumes inside it: a non-incrementing NOP run with two
 * non-zero parameters traps twice, and the zero between them does not. */
static void test_trap_resumes_inside_packet(void)
{
    NV2AState *d = fresh_with(1);
    Pb pb;
    uint32_t first, third;
    guest_setup(d, 1);
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_word(&pb, hdr_noninc(0, NV097_NO_OPERATION, 3));
    pb_word(&pb, 0x301);
    first = pb.at;
    pb_word(&pb, 0);
    pb_word(&pb, 0x302);
    third = pb.at;
    pb_semaphore(&pb, H_SEMAPHORE, 0, 4);
    kick(d, pb.start, pb.at);
    CHECK(get_ptr(d) == first, "first trap GET %08X", get_ptr(d));
    guest_isr(d, NULL);
    CHECK(get_ptr(d) == third, "second trap GET %08X (%s)", get_ptr(d), diag(d));
    CHECK(mmio_r(d, PGRAPH(NV_PGRAPH_TRAPPED_DATA_LOW)) == 0x302, "second trap data %08X",
          mmio_r(d, PGRAPH(NV_PGRAPH_TRAPPED_DATA_LOW)));
    CHECK(sema() == SENTINEL, "release ran before the second trap was handled");
    guest_isr(d, NULL);
    CHECK(get_ptr(d) == pb.at && sema() == 4, "did not finish (%s)", diag(d));
    CHECK(d->pfifo.software_method_traps == 2, "trap count %u", d->pfifo.software_method_traps);
}

/* A trap inside a CALLed subroutine keeps its return address across the hold. */
static void test_trap_inside_subroutine(void)
{
    NV2AState *d = fresh_with(1);
    Pb pb, sub;
    guest_setup(d, 1);
    pb_begin(&sub, PB_BASE + 0x400);
    pb_method(&sub, 0, NV097_NO_OPERATION, 0x310);
    pb_word(&sub, 0x00020000u);                   /* return */
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_word(&pb, sub.start | 2u);                  /* call */
    pb_semaphore(&pb, H_SEMAPHORE, 0, 6);
    kick(d, pb.start, pb.at);
    CHECK(strcmp(diag(d), "software_method_trap") == 0, "diag %s", diag(d));
    guest_isr(d, NULL);
    CHECK(get_ptr(d) == pb.at && sema() == 6, "return lost across the hold (GET %08X, %s)",
          get_ptr(d), diag(d));
}

/* With data checking off the references disagree (trap, or ignore), so the
 * stream stops there, rolled back, with no interrupt. */
static void test_nop_unchecked_blocks(void)
{
    NV2AState *d = fresh_with(1);
    Pb pb;
    guest_setup(d, 0);
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_method(&pb, 0, NV097_NO_OPERATION, 0x300);
    kick(d, pb.start, pb.at);
    CHECK(strcmp(diag(d), "software_method_unchecked") == 0, "diag %s", diag(d));
    CHECK(get_ptr(d) == pb.start, "GET moved to %08X", get_ptr(d));
    CHECK(g_line == 0 && !(mmio_r(d, PGRAPH(NV_PGRAPH_INTR)) & NV_PGRAPH_INTR_ERROR),
          "interrupt raised");
}

/* With the switch unset a non-zero NOP is the no-op it always was. */
static void test_nop_dormant_when_unset(void)
{
    NV2AState *d = fresh_with(0);
    Pb pb;
    guest_setup(d, 1);
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_method(&pb, 0, NV097_NO_OPERATION, 0x300);
    pb_method(&pb, 0, NV097_FLIP_STALL, 0);
    kick(d, pb.start, pb.at);
    CHECK(strcmp(diag(d), "ok") == 0 && get_ptr(d) == pb.at, "stream held (%s)", diag(d));
    CHECK(g_line == 0 && g_line_edges == 0, "interrupt raised with the switch unset");
    CHECK(mmio_r(d, PGRAPH(NV_PGRAPH_FIFO)) == NV_PGRAPH_FIFO_ACCESS, "FIFO access changed");
    CHECK(mmio_r(d, PGRAPH(NV_PGRAPH_TRAPPED_ADDR)) == 0, "trap latched");
}

/* FLIP_STALL holds while READ_3D equals WRITE_3D and continues once the
 * vblank handler's INCREMENT write moves READ_3D. */
static void test_flip_stall(void)
{
    NV2AState *d = fresh_with(1);
    Pb pb;
    uint32_t after_stall, surface;
    guest_setup(d, 1);
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_method(&pb, 0, NV097_SET_FLIP_READ, 0);     /* JSRF's values */
    pb_method(&pb, 0, NV097_SET_FLIP_WRITE, 1);
    pb_method(&pb, 0, NV097_SET_FLIP_MODULO, 3);
    pb_method(&pb, 0, NV097_FLIP_INCREMENT_WRITE, 0);   /* write 2 */
    pb_method(&pb, 0, NV097_FLIP_STALL, 0);             /* 0 != 2: passes */
    kick(d, pb.start, pb.at);
    surface = mmio_r(d, PGRAPH(NV_PGRAPH_SURFACE));
    CHECK(get_ptr(d) == pb.at, "first FLIP_STALL held (%s)", diag(d));
    CHECK(GET_MASK(surface, NV_PGRAPH_SURFACE_READ_3D) == 0 &&
          GET_MASK(surface, NV_PGRAPH_SURFACE_WRITE_3D) == 2 &&
          GET_MASK(surface, NV_PGRAPH_SURFACE_MODULO_3D) == 3, "SURFACE %08X", surface);

    pb_method(&pb, 0, NV097_FLIP_INCREMENT_WRITE, 0);   /* write wraps to 0 */
    pb_method(&pb, 0, NV097_FLIP_STALL, 0);             /* 0 == 0: holds */
    after_stall = pb.at;
    pb_semaphore(&pb, H_SEMAPHORE, 0, 9);
    kick_put(d, pb.at);
    CHECK(strcmp(diag(d), "flip_stall") == 0, "diag %s", diag(d));
    CHECK(get_ptr(d) == after_stall, "GET %08X, want just past FLIP_STALL %08X",
          get_ptr(d), after_stall);
    CHECK(sema() == SENTINEL, "release after FLIP_STALL ran early");
    CHECK(g_line == 0, "FLIP_STALL raised an interrupt");

    kick_put(d, pb.at);
    CHECK(strcmp(diag(d), "held_flip_stall") == 0 && get_ptr(d) == after_stall,
          "held walk moved (%s)", diag(d));
    mmio_w(d, PGRAPH(NV_PGRAPH_INCREMENT), NV_PGRAPH_INCREMENT_READ_BLIT);
    CHECK(get_ptr(d) == after_stall, "released by the blit read counter");
    mmio_w(d, PGRAPH(NV_PGRAPH_INCREMENT), NV_PGRAPH_INCREMENT_READ_3D);
    surface = mmio_r(d, PGRAPH(NV_PGRAPH_SURFACE));
    CHECK(GET_MASK(surface, NV_PGRAPH_SURFACE_READ_3D) == 1, "READ_3D %u",
          GET_MASK(surface, NV_PGRAPH_SURFACE_READ_3D));
    CHECK(get_ptr(d) == pb.at && sema() == 9, "not released (GET %08X, %s)", get_ptr(d), diag(d));

    /* The read counter wraps at the modulo too. */
    mmio_w(d, PGRAPH(NV_PGRAPH_INCREMENT), NV_PGRAPH_INCREMENT_READ_3D);
    mmio_w(d, PGRAPH(NV_PGRAPH_INCREMENT), NV_PGRAPH_INCREMENT_READ_3D);
    CHECK(GET_MASK(mmio_r(d, PGRAPH(NV_PGRAPH_SURFACE)), NV_PGRAPH_SURFACE_READ_3D) == 0,
          "READ_3D did not wrap");
}

int main(void)
{
    test_translator_reads_bound_class();
    test_semaphore_dormant_when_unset();
    test_semaphore_written_only_on_commit();
    test_semaphore_bounds();
    test_fence_mirror_overlap_reported();
    test_software_method_trap();
    test_trap_acks_in_either_order();
    test_trap_resumes_inside_packet();
    test_trap_inside_subroutine();
    test_nop_unchecked_blocks();
    test_nop_dormant_when_unset();
    test_flip_stall();

    if (g_failures) {
        fprintf(stderr, "nv2a_actions_test: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "nv2a_actions_test: all checks passed\n");
    return 0;
}
