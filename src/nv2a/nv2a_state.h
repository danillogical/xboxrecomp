/*
 * NV2A GPU State - Standalone adaptation of xemu's nv2a_int.h
 *
 * Based on xemu (Copyright (c) 2012 espes, 2015 Jannik Vogel,
 * 2018-2025 Matt Borgerson) - LGPL v2+
 *
 * PGRAPHState is stubbed to a minimal struct for Phase 1.
 * Full PGRAPH integration comes in Phase 3-4.
 */

#ifndef BURNOUT3_NV2A_STATE_H
#define BURNOUT3_NV2A_STATE_H

#include "qemu_shim.h"
#include "nv2a_regs.h"

/* Debug macros from xemu's debug.h */
#ifndef DEBUG_NV2A
# define DEBUG_NV2A 0
#endif

#if DEBUG_NV2A
# define NV2A_DPRINTF(format, ...)  fprintf(stderr, "nv2a: " format, ## __VA_ARGS__)
#else
# define NV2A_DPRINTF(format, ...)  do { } while (0)
#endif

#define NV2A_XPRINTF(x, ...) do { \
    if (x) { fprintf(stderr, "nv2a: " __VA_ARGS__); } \
} while (0)

#define NV2A_UNCONFIRMED(...)  do {} while (0)
#define NV2A_UNIMPLEMENTED(...) do {} while (0)

/* NV2A_DEVICE: cast to NV2AState* */
#define NV2A_DEVICE(obj) ((NV2AState*)(obj))

/* ============================================================
 * FIFO Engine types
 * ============================================================ */

enum FIFOEngine {
    ENGINE_SOFTWARE = 0,
    ENGINE_GRAPHICS = 1,
    ENGINE_DVD = 2,
};

typedef struct DMAObject {
    unsigned int dma_class;
    unsigned int dma_target;
    hwaddr address;
    hwaddr limit;
} DMAObject;

/* ============================================================
 * PGRAPHState - STUB for Phase 1
 * Only contains fields accessed by nv2a_update_irq.
 * Full struct will be added when PGRAPH is integrated.
 * ============================================================ */

/* NV097 method numbers run to 0x1FFC, so their parameters fit 0x800 words. */
#define NV2A_PGRAPH_METHOD_WORDS 0x800

struct PGRAPHState {
    uint32_t pending_interrupts;
    uint32_t enabled_interrupts;
    uint32_t regs[0x2000];          /* PGRAPH registers, by byte offset */
    /* NV097 method parameters by method / 4: the state a renderer reads. Kept
     * apart from regs, where method 0x500 would land on NV_PGRAPH_INTR_EN and
     * 0x520 on the CTX_USER the trap path reads. */
    uint32_t methods[NV2A_PGRAPH_METHOD_WORDS];
    /* Instance of the DMA object SET_CONTEXT_DMA_SEMAPHORE bound (action
     * methods only; see nv2a_actions_enabled). */
    uint32_t dma_semaphore;
    bool dma_semaphore_valid;
    /* Phase 3-4: Full PGRAPH state will go here */
};

enum {
    NV2A_HOLD_NONE = 0,
    NV2A_HOLD_SOFTWARE_METHOD = 1,
    NV2A_HOLD_FLIP_STALL = 2,
};

/* What triggered a walk. A budget stop's meaning depends on which of these
 * ran: a PUT kick means the guest submitted more work, a hold resume continues
 * a trapped packet, and a stalled retry is the model re-walking on its own
 * 100 ms cadence. Only the last one is a "retry" in the continuation sense. */
enum {
    NV2A_WALK_PUT_WRITE = 1,
    NV2A_WALK_HOLD_RESUME = 2,
    NV2A_WALK_STALLED_RETRY = 3,
    NV2A_WALK_OTHER = 4,
};

/* How many budget stops are kept for the continuation audit. A ring, so a long
 * run keeps the most recent events; `budget_events_total` is uncapped. 64 is
 * chosen to exceed the largest count any archived run has produced (12) by a
 * wide margin while staying trivial in size. */
#define NV2A_BUDGET_EVENT_MAX 64

/* Sentinel for "no resume is currently being classified". 0 is a valid ring
 * index, so it cannot be used as the absent value. */
#define NV2A_BUDGET_NO_RESUME 0xFFFFFFFFu

/* One budget stop and, once it happens, the walk that resumed it.
 *
 * This exists because the FIRST-only latch cannot answer the continuation
 * question: "did the same submission resume at the right cursor" is a property
 * of the PAIR (stop, resume), and the interesting stop is usually not the
 * first. Every field is written at the event by the code that performs it. */
typedef struct NV2ABudgetEvent {
    uint32_t seq;            /* 1-based stop number within the run */
    uint32_t walk_serial;    /* the walk that stopped */
    uint32_t trigger;        /* NV2A_WALK_* of the walk that stopped */
    uint32_t limit_packets;  /* 1 = the 1024-packet cap, 0 = the word cap */
    uint32_t in_param;       /* 1 = mid-packet, 0 = at a header */
    uint32_t start_get;      /* where the stopping walk began */
    uint32_t committed_get;  /* GET published by its last committed unit */
    uint32_t put;            /* the walk's PUT at the stop */
    uint32_t local_pc;       /* where the limit fired */
    uint32_t words, packets; /* consumed by the stopping walk */
    uint32_t units;          /* units that walk committed before stopping */
    uint32_t count, method, ret;
    uint32_t tail_words;     /* local_pc - committed_get, in words (rolled back) */
    /* Filled by the NEXT walk that runs while this stop is outstanding. */
    uint32_t resume_walk_serial;
    uint32_t resume_trigger;
    uint32_t resume_start_get;   /* must equal committed_get for correct resume */
    uint32_t resume_end_get;
    uint32_t resume_put;
    uint32_t resume_ok;
    uint32_t resume_units;
    uint32_t resumed;            /* 1 once a walk has consumed this stop */
    uint32_t start_was_committed;/* 1 = resume began exactly at committed_get */
    uint32_t resume_stalled;     /* 1 = right boundary, but no progress */
} NV2ABudgetEvent;

/* ============================================================
 * NV2AState - Main GPU state
 * Adapted from xemu's nv2a_int.h
 * ============================================================ */

/* One UNIT's walk budget, in words, and the capacity of everything the walk
 * stages or records for that unit.
 *
 * Atomicity is per UNIT, not per submission. The walk commits a unit
 * all-or-nothing: it stages every method of the unit before it commits any of
 * them, so the staging array and the sink both have to hold a whole budget's
 * worth. Each staged method consumes exactly one parameter word, so staged
 * methods <= walked words <= NV2A_SUBMIT_MAX_WORDS. A unit ends at a
 * WHOLE-PACKET boundary, and a packet is at most 2048 words, so the walk can
 * always make progress one packet at a time. The walk separately bounds a unit
 * to 1024 packets, which is unchanged. */
#define NV2A_SUBMIT_MAX_WORDS 4096

typedef struct NV2AState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    qemu_irq irq;
    bool exiting;

    /* The card's interrupt line, delivered to whatever owns the host side.
     *
     * `pci_irq_assert`/`pci_irq_deassert` are compiled-in no-ops in the
     * standalone build, so without this the model computes its pending and
     * enabled masks and then tells nobody. The kernel bridge registers a sink
     * that raises the guest's vector; the sink is called only on a line
     * transition, never once per update, so a level that stays asserted does
     * not re-enter the guest ISR. */
    void (*irq_sink)(void *opaque, int asserted);
    void *irq_sink_opaque;
    int irq_line_asserted;

    VGACommonState vga;
    GraphicHwOps hw_ops;
    QEMUTimer *vblank_timer;

    MemoryRegion *vram;
    MemoryRegion vram_pci;
    uint8_t *vram_ptr;
    MemoryRegion ramin;
    uint8_t *ramin_ptr;
    uint32_t ramin_guest_base;

    MemoryRegion mmio;
    MemoryRegion block_mmio[NV_NUM_BLOCKS];

    struct {
        uint32_t pending_interrupts;
        uint32_t enabled_interrupts;
    } pmc;

    struct {
        uint32_t pending_interrupts;
        uint32_t enabled_interrupts;
        uint32_t regs[0x2000];
        QemuMutex lock;
        QemuThread thread;
        QemuCond fifo_cond;
        QemuCond fifo_idle_cond;
        bool fifo_kick;
        bool halt;
        /* Bounded 11b4 submission state. The pointer is the physical
         * contiguous window, never canonical guest RAM or detached VRAM. */
        uint8_t *pushbuffer;
        uint32_t pushbuffer_base;
        uint32_t pushbuffer_size;
        uint32_t submit_words;
        uint32_t submit_packets;
        uint32_t submit_last_method;
        uint32_t submit_last_param;
        uint32_t submit_diag;
        uint32_t submit_diag_get;
        uint32_t submit_diag_subchannel;
        uint32_t submit_diag_method;
        uint32_t submit_diag_param;
        uint32_t submit_successes;
        /* Units committed by the most recent walk, and cumulatively across the
         * run. Atomicity is per UNIT: one walk may commit several units, and a
         * walk that rejects a later unit still leaves the earlier ones
         * committed with GET at the rejected unit's own start. */
        uint32_t submit_units;
        uint32_t submit_units_total;
        /* The per-call structural cycle bound the most recent walk used:
         * pushbuffer_size / 4, the ring's own word count. This is NOT a budget
         * -- the unit budget is still NV2A_SUBMIT_MAX_WORDS -- it is a property
         * of the ring, and a call-free walk cannot visit more words than the
         * ring holds without repeating an address. */
        uint32_t submit_loop_bound;
        /* PFIFO object bindings.  Production SET_OBJECT walks RAMHT in the
         * claimed PRAMIN window.  fixture_* remains a test-only seam. */
        uint32_t binding_class[8];
        uint32_t binding_object[8];
        uint32_t fixture_class[8];
        uint32_t fixture_object[8];
        bool fixture_execution;
        uint32_t sink_count;
        /* Semaphore releases written to guest memory (action methods only). */
        uint32_t semaphore_releases;
        /* A walk held by a software-method trap or FLIP_STALL (action methods
         * only), and the rest of the packet it stopped in, which the walk
         * that resumes after the hold continues from. */
        uint32_t hold;
        uint32_t carry_count, carry_method, carry_subchannel, carry_ret;
        bool carry_non_inc;
        uint32_t software_method_traps;
        /* class_id is recorded because the same method number means different
         * things in different classes -- 0x2FC is NV09F_SET_OPERATION and an
         * NV097 surface method, and both appear in JSRF's stream. */
        struct { uint32_t subchannel, class_id, method, param; } sink[NV2A_SUBMIT_MAX_WORDS];
        /* The walk's staging area for the submission it is walking. It is
         * PFIFO-owned rather than a local because a submission may stage a
         * whole budget's worth, and 4096 entries is 48 KB -- too much for the
         * submission thread's stack. Nothing is committed from it until the
         * whole walk has succeeded.
         *
         * `class_id` is recorded PER ENTRY, at the moment the method is walked.
         * It cannot be read from binding_class[] afterwards: SET_OBJECT rebinds
         * a subchannel in place, so a mid-stream rebind would retro-label the
         * methods that came before it. */
        struct { uint32_t subchannel, class_id, method, param; } staged[NV2A_SUBMIT_MAX_WORDS];

        /* The FIRST budget rejection's own transcript.
         *
         * Why this exists rather than relying on the log: the `[PFIFO] submit`
         * lines stop after 64 walks, and the diagnostic dump is printed only on
         * the HEADER path -- the parameter path (which is where an over-long
         * packet exhausts the budget) printed nothing at all. So the one event
         * that matters most for "why did the walk stop" was the least
         * observable. These fields are latched once, on the first budget
         * rejection, and are printed by a single emitter that BOTH paths call.
         *
         * `local_pc` is the address the walk was consuming when the limit fired.
         * It is deliberately separate from submit_diag_get, which is the
         * rollback origin (where the stream will be retried from) -- reporting
         * that as the failure point is a known trap. */
        uint32_t budget_stops;            /* how many budget rejections seen */
        uint32_t budget_local_pc;         /* where the walk was at the limit */
        uint32_t budget_words, budget_packets;
        uint32_t budget_count, budget_method, budget_ret;
        uint32_t budget_get, budget_put;
        bool     budget_in_param;         /* limit fired mid-packet vs at a header */
        bool     budget_at_packet_limit;  /* the 1024-packet cap, not the word cap */
        /* A trajectory of the words actually consumed, in order, so a
         * parameter-heavy stream is not misread as a header-only one. The old
         * `trace[words & 31]` array was written only at headers and indexed by
         * TOTAL words, so its slots were sparse, stale and out of order. */
        uint32_t budget_trace_va[64];
        uint32_t budget_trace_word[64];
        uint32_t budget_trace_count;

        /* ── Every budget stop, not just the first ──────────────────────────
         *
         * The latched transcript above records the FIRST stop only, which is
         * the right choice for "why did the walk stop" but useless for "does
         * the same submission resume correctly", where the LATER stops are the
         * evidence. A run can stop dozens of times (witness-0298 stopped 12),
         * so the question "is this benign chunking or broken continuation"
         * cannot be answered from a first-only latch.
         *
         * This ring is written by the walk itself, at the event, with no
         * sampling: `committed_get` is the GET the last committed unit
         * published, and `tail_words` is what the stop rolled back. The
         * resume check below pairs each stop with the walk that consumes it.
         */
        NV2ABudgetEvent budget_events[NV2A_BUDGET_EVENT_MAX];
        uint32_t budget_events_total;      /* every stop, ring or not */
        uint32_t budget_event_next;        /* ring write cursor */
        /* The GET the NEXT walk is expected to start from, i.e. the last
         * committed unit's boundary of a stop that has not yet been resumed.
         * Zero when no stop is outstanding. */
        uint32_t budget_expected_get;
        uint32_t budget_expected_put;
        uint32_t budget_outstanding;       /* a stop is awaiting its resume */
        /* Resume outcomes.
         *
         * `matched` counts resumptions that began exactly at the previous
         * stop's committed boundary AND MADE PROGRESS. `mismatched` is the
         * Case C signature (the walk began somewhere else, e.g. it restarted
         * the submission). `stalled` is the third outcome: the right boundary,
         * but no progress -- which is what the zero-commit livelock produces.
         *
         * WHY PROGRESS IS PART OF `matched`. The boundary comparison alone is
         * near-vacuous: `budget_expected_get` is captured from GET at the stop
         * and compared against the same register at the next walk, and GET
         * only advances on a commit -- so "matched" is the DEFAULT outcome,
         * including for a retry that makes no progress at all. Without the
         * progress term the counter cannot distinguish resumption from a
         * repeated stall. */
        uint32_t budget_resume_matched;
        uint32_t budget_resume_mismatched;
        uint32_t budget_resume_stalled;
        /* The machine-readable DRAIN evidence: resumes whose walk reached the
         * stop's PUT, i.e. the whole submission was consumed. This is a subset
         * of `matched` (right boundary AND progress) and is the counter that
         * establishes resumable chunking without parsing the log. */
        uint32_t budget_resume_drained;
        /* The ring index of the event whose resume is being classified, so the
         * walk's end can decide whether that resume progressed. */
        uint32_t budget_resume_event;
        uint32_t budget_resume_end_get;    /* where the most recent resume got to */
        uint32_t budget_resume_ok;         /* that resume's success flag */
        /* Words the stops rolled back and the retries re-walked. A resumable
         * chunking scheme re-walks a tail once per stop; a replay bug shows as
         * this growing without GET advancing. */
        uint32_t budget_rewalked_words;
        uint32_t budget_rewalked_packets;
        /* Global walk serial and the trigger of the most recent walk, so a
         * stop can be attributed to a PUT kick, a hold resume, or the stalled
         * retry cadence rather than being assumed to be one of them. */
        uint32_t walk_serial;
        uint32_t walk_trigger;             /* NV2A_WALK_* */
        uint32_t walk_retry_count;
        /* Packets-per-walk histogram, log2 bins. The 1024 cap is only
         * near-binding if walks actually reach these bins; a run whose walks
         * all sit in the low bins is not being paced by the cap at all. */
        uint32_t walk_packet_hist[12];     /* bin i: packets in [2^i, 2^(i+1)) */
        uint32_t walk_packet_max;
        uint32_t walk_words_max;
        /* ── vblank delivery audit ───────────────────────────────────────────
         *
         * Why this exists: the ADX middleware's vsync/file threads in the
         * failing runs are frozen in KeWaitForSingleObject on one event, and
         * the run that made progress is the one whose archived register state
         * shows NV_PCRTC_INTR_0 pending (the guest had not acknowledged). The
         * question is whether the display keeps PULSING and the guest stops
         * acknowledging, or the pulse/edge stops being delivered at all --
         * and the IRQ delivery log cannot say, because it prints only the
         * first three deliveries by construction.
         *
         * These are COUNTERS, incremented in the hot path with no formatting
         * and no allocation: a vblank fires 60 times a second and a print per
         * pulse would itself perturb the timing under investigation. */
        uint32_t vblank_pulses;            /* nv2a_vblank_pulse calls */
        uint32_t vblank_already_pending;   /* pulses where the bit was ALREADY set
                                            * => the guest is not acknowledging */
        uint32_t vblank_guest_acks;        /* W1C writes that cleared the bit */
        uint32_t vblank_enable_writes;     /* writes to NV_PCRTC_INTR_EN_0 */
        uint32_t vblank_enable_last;       /* the value the guest last wrote */
        uint32_t vblank_enable_cleared;    /* enable writes that cleared VBLANK */
        uint32_t vblank_irq_asserted;      /* update_irq transitions to asserted */
        uint32_t vblank_irq_deasserted;    /* transitions to deasserted */
        uint32_t vblank_irq_level_low;     /* update_irq saw nothing pending */
        uint32_t vblank_irq_enabled_off;   /* pending but enabled mask is 0 */
        /* The last W1C value the guest wrote and the pending bits it left. */
        uint32_t vblank_last_ack_value;
        uint32_t vblank_pending_last;
        uint32_t submit_walk_start_get;
        uint32_t submit_walk_units;
    } pfifo;

    struct {
        uint32_t regs[0x1000];
    } pvideo;

    struct {
        uint32_t pending_interrupts;
        uint32_t enabled_interrupts;
        uint32_t numerator;
        uint32_t denominator;
        uint64_t alarm_time;
        uint64_t time_offset;
        uint64_t (*clock_ns)(void *opaque);
        void *clock_opaque;
    } ptimer;

    struct {
        uint32_t regs[0x1000];
    } pfb;

    struct PGRAPHState pgraph;

    struct {
        uint32_t pending_interrupts;
        uint32_t enabled_interrupts;
        hwaddr start;
        uint32_t raster;
    } pcrtc;

    struct {
        uint32_t core_clock_coeff;
        uint64_t core_clock_freq;
        uint32_t memory_clock_coeff;
        uint32_t video_clock_coeff;
        uint32_t general_control;
        uint32_t fp_vdisplay_end;
        uint32_t fp_vcrtc;
        uint32_t fp_vsync_end;
        uint32_t fp_vvalid_end;
        uint32_t fp_hdisplay_end;
        uint32_t fp_hcrtc;
        uint32_t fp_hvalid_end;
    } pramdac;

    struct {
        uint16_t write_mode_address;
        uint8_t palette[256*3];
    } puserdac;

} NV2AState;

/* ============================================================
 * NV2ABlockInfo - block dispatch table entry
 * ============================================================ */

typedef struct NV2ABlockInfo {
    const char *name;
    hwaddr offset;
    uint64_t size;
    MemoryRegionOps ops;
} NV2ABlockInfo;

extern const NV2ABlockInfo blocktable[NV_NUM_BLOCKS];

/* ============================================================
 * Function prototypes
 * ============================================================ */

void nv2a_update_irq(NV2AState *d);

/* Register the host owner of the card's interrupt line. */
void nv2a_set_irq_sink(NV2AState *d,
                       void (*sink)(void *opaque, int asserted), void *opaque);

/* Whether the line is currently asserted: something is pending and the master
 * enable is on. NV_PMC_INTR_EN_0 is a two-bit hardware/software master enable,
 * not a per-source mask -- the per-source masks are the block registers, which
 * `nv2a_update_irq` has already folded into the PMC summary. */
int nv2a_irq_line_asserted(NV2AState *d);

/* One vertical blank from the display clock.
 *
 * The model has no display clock of its own, which is why the card's PCRTC
 * pending bit was never set by anything. Asserting the pending bit is the
 * hardware's job; clearing it is the guest's, through its write-1-to-clear to
 * NV_PCRTC_INTR_0. Nothing here touches the guest's enables. */
void nv2a_vblank_pulse(NV2AState *d);

/* One service-loop pass's vblank scheduling decision, as a pure function so
 * the rule can be tested without a device, a clock or a live thread.
 * Returns the new deadline and sets `*pulse` when a pulse is due. */
uint64_t nv2a_vblank_advance(uint64_t next, uint64_t now, uint64_t frame,
                             int *pulse);

/* Frame period in nanoseconds, from the guest-programmed video timing.
 *
 * Falls back to 60 Hz when the guest has not programmed a video PLL and
 * timing that produce a plausible refresh. The fallback is reported through
 * `nv2a_display_frame_source` so a run can say which one it used. */
uint64_t nv2a_display_frame_ns(NV2AState *d);
const char *nv2a_display_frame_source(void);

/* Register block log helpers (no-op stubs since trace is disabled) */
static inline
void nv2a_reg_log_read(int block, hwaddr addr, unsigned int size, uint64_t val)
{
    (void)block; (void)addr; (void)size; (void)val;
}

static inline
void nv2a_reg_log_write(int block, hwaddr addr, unsigned int size, uint64_t val)
{
    (void)block; (void)addr; (void)size; (void)val;
}

/* Register block read/write prototypes */
#define DEFINE_PROTO(n) \
    uint64_t n##_read(void *opaque, hwaddr addr, unsigned int size); \
    void n##_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

DEFINE_PROTO(pmc)
DEFINE_PROTO(pbus)
DEFINE_PROTO(pfifo)
DEFINE_PROTO(pvideo)
DEFINE_PROTO(ptimer)
DEFINE_PROTO(pfb)
DEFINE_PROTO(pgraph)
DEFINE_PROTO(pcrtc)
DEFINE_PROTO(pramdac)
#undef DEFINE_PROTO

/* Stub read/write for blocks we haven't implemented yet */
uint64_t nv2a_stub_read(void *opaque, hwaddr addr, unsigned int size);
void nv2a_stub_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

/* DMA helpers */
DMAObject nv_dma_load(NV2AState *d, hwaddr dma_obj_address);
void *nv_dma_map(NV2AState *d, hwaddr dma_obj_address, hwaddr *len);

/* PGRAPH method dispatch (from push buffer commands) */
void pgraph_method(NV2AState *d, uint32_t subchannel,
                   uint32_t method, uint32_t param);

/* ============================================================
 * Public API
 * ============================================================ */

/* Initialize the NV2A GPU state (standalone, no QEMU PCI bus) */
NV2AState *nv2a_init_standalone(uint8_t *vram_ptr, uint32_t vram_size,
                                 uint8_t *ramin_ptr, uint32_t ramin_size);

/* Bind PRAMIN/RAMIN to the physical instance-memory claim.  The binding is
 * deferred when the MMIO hook has not initialized yet, so the kernel bridge
 * may publish the claim in either order.  The guest base is retained for
 * diagnostics; accesses use the supplied host mapping exactly. */
bool nv2a_bind_instance_memory(uint32_t guest_base, uint8_t *host_ptr,
                               uint32_t size);
/* Single-threaded focused-test seam for verifying both initialization orders.
 * It intentionally abandons the old state and must never be used at runtime. */
void nv2a_reset_standalone_for_test(void);
/* Production claim transaction.  The hook implementation serializes the
 * idempotence decision and binding commit with VEH/PTIMER MMIO ownership;
 * standalone core tests use the binding entry point above directly. */
bool nv2a_claim_instance_memory_threadsafe(uint32_t guest_base,
                                           uint8_t *host_ptr, uint32_t size);

/* Process an MMIO read/write from the VEH handler */
uint64_t nv2a_mmio_read(NV2AState *d, hwaddr addr, unsigned int size);
void nv2a_mmio_write(NV2AState *d, hwaddr addr, uint64_t val, unsigned int size);

/* Register the physical contiguous pushbuffer window used by USER DMA.
 * `base` is the host mapping of guest VA `guest_base`; no address masking or
 * fallback mapping is performed. */
bool nv2a_set_pushbuffer_window(NV2AState *d, uint8_t *base,
                                uint32_t guest_base, uint32_t size);
/* Test-only binding seam.  Production SET_OBJECT uses RAMHT lookup. */
bool nv2a_set_fixture_binding(NV2AState *d, uint32_t subchannel,
                              uint32_t object, uint32_t class_id);
bool nv2a_set_fixture_execution(NV2AState *d, bool enabled);
bool nv2a_submit_pending(NV2AState *d);

/* What the most recent submit walk did, published once per walk. The per-submit
 * "[PFIFO] submit" line stops at #63, so a walk rejected later in a run was
 * invisible; this is the state a dump or the collector reads. generation is
 * odd while a walk is writing it. */
typedef struct NV2ASubmitState {
    volatile LONG generation;   /* odd while being written, even when stable */
    uint32_t diag;              /* NV2A_SUBMIT_* code of the most recent walk */
    uint32_t method, subchannel, param, at;
    uint32_t get, put;
    uint32_t successes, rejections, consecutive_rejections;
    uint32_t admitted_unknown;
    /* The FIRST budget rejection's own transcript, latched so it survives both
     * the 64-line submit log and any stderr filtering. A budget stop is the one
     * event the log could not show when it happened inside a packet's
     * parameters, because only the header path used to print. These fields make
     * the stop readable from a dump after the run.
     *
     * `local_pc` is where the walk was consuming at the limit, which is NOT
     * `at`: `at` is the rollback origin the stream is retried from. */
    uint32_t budget_stops;
    uint32_t budget_local_pc;
    uint32_t budget_words, budget_packets;
    uint32_t budget_count, budget_method, budget_ret;
    uint32_t budget_in_param;        /* 1 = mid-packet, 0 = at a header */
    uint32_t budget_at_packet_limit; /* 1 = the 1024-packet cap fired */
    /* Units committed by the most recent walk, and cumulatively across the run.
     * Appended AFTER the budget transcript so the field order an existing
     * reader already decodes is unchanged: a reader that asks for the size it
     * knows simply stops before these, and never misreads them as a budget
     * field. Atomicity is per UNIT, so `units` is the count of all-or-nothing
     * groups this one walk committed. */
    uint32_t units;
    uint32_t units_total;
    /* The per-call structural cycle bound that walk used: the ring's own word
     * count (pushbuffer_size / 4). NOT a budget -- the unit budget is still
     * NV2A_SUBMIT_MAX_WORDS. See the walk for why it is not a raised limit. */
    uint32_t loop_bound;
    /* ── Continuation audit (appended after loop_bound, for the same
     *    forward-compatibility reason the unit fields are appended) ────────
     *
     * These answer "does the same submission resume at the right cursor",
     * which the first-stop latch cannot: the interesting stop is usually not
     * the first. `resume_matched` counts resumptions that began exactly at the
     * previous stop's committed boundary; `resume_mismatched` is the Case C
     * signature and must stay zero for benign chunking. */
    uint32_t budget_events_total;   /* every stop, uncapped */
    uint32_t budget_resume_matched;
    uint32_t budget_resume_mismatched;
    uint32_t budget_rewalked_words; /* tail words re-walked by retries */
    uint32_t budget_rewalked_packets;
    uint32_t walk_serial;
    uint32_t walk_retry_count;
    uint32_t walk_packet_max;
    uint32_t walk_words_max;
    /* log2 histogram of packets consumed per walk. The 1024 cap is only
     * near-binding if real walks land in the top bins; if every walk sits
     * below, the cap is not what paces the run. */
    uint32_t walk_packet_hist[12];
    /* The most recent budget stop's own record, so a dump taken after the run
     * can be paired with the walk that resumed it without parsing the log. */
    uint32_t budget_last_seq;
    uint32_t budget_last_start_get;
    uint32_t budget_last_committed_get;
    uint32_t budget_last_put;
    uint32_t budget_last_local_pc;
    uint32_t budget_last_tail_words;
    uint32_t budget_last_units;
    uint32_t budget_last_resume_start_get;
    uint32_t budget_last_resume_end_get;
    uint32_t budget_last_resume_ok;
    uint32_t budget_last_resumed;
    /* ── vblank delivery audit (appended, same forward-compat rule) ────────
     *
     * The ADX middleware's vsync/file threads are frozen in a dispatcher wait
     * on one event in the runs that stall on "Now Loading", and the run that
     * progressed is the one whose register state showed NV_PCRTC_INTR_0
     * pending. These counters separate the three ways vblank delivery can
     * stop: the display stops pulsing, the guest stops acknowledging, or the
     * line is left asserted and never re-delivered. */
    uint32_t vblank_pulses;
    uint32_t vblank_already_pending;
    uint32_t vblank_guest_acks;
    uint32_t vblank_enable_writes;
    uint32_t vblank_enable_last;
    uint32_t vblank_enable_cleared;
    uint32_t vblank_irq_asserted;
    uint32_t vblank_irq_deasserted;
    uint32_t vblank_last_ack_value;
    uint32_t vblank_pending_last;
    /* ── resume-quality audit (appended, same forward-compat rule) ─────────
     *
     * `resume_stalled` counts resumptions that began at the correct committed
     * boundary but made NO progress -- the zero-commit livelock, and the case
     * the boundary comparison alone cannot name. `resume_drained` counts those
     * whose walk reached the stop's PUT, which is the drain evidence that
     * establishes resumable chunking. A run with stops, zero stalls, zero
     * mismatches and a drained count equal to its stop count is genuinely
     * resuming. Appended last so every earlier reader's field offsets are
     * unchanged. */
    uint32_t budget_resume_stalled;
    uint32_t budget_resume_drained;
} NV2ASubmitState;
/* Exported on Windows so a dump and the linker map name it, like g_nv2a_mmio_snapshot. */
#ifdef _WIN32
__declspec(dllexport)
#endif
extern NV2ASubmitState g_nv2a_submit_state;

/* RECOMP_NV2A_ADMIT_UNKNOWN switch: -1 re-reads the environment on next use,
 * 0 forces off, 1 forces on. Exploratory; unknown methods on known classes are
 * captured as state, not executed. */
void nv2a_admit_unknown_override(int value);
bool nv2a_admit_unknown_enabled(void);
const char *nv2a_submit_diagnostic(uint32_t code);

/* RECOMP_NV2A_PACKET_CAP: DIAGNOSTIC ONLY, default 1024.
 *
 * The 1024-packet cap is deliberately a STOP, not a yield (compatibility
 * ledger L40), so it must not be changed to make a run "work". This override
 * exists to make the cap an EXPERIMENTAL VARIABLE: varying it on ONE binary is
 * the controlled test of whether the cap is benign chunking (the workload
 * still completes, landmarks unchanged) or artificial starvation (it does
 * not). A run with it set is exploratory, exactly like every other switch that
 * changes the walk's behaviour.
 *
 * The value is the packet count; 0 or absent means the default. It is read
 * once, like the admit switch, so a run cannot change it mid-flight. */
void nv2a_packet_cap_override(int value);
uint32_t nv2a_packet_cap(void);

/* Committed-method consumer (a host renderer/observer). This is the ONLY way
 * the model hands committed methods to anything outside the GPU core: the core
 * never calls into the kernel, and it never learns what the consumer does.
 *
 * Contract:
 *  - The consumer is called from the submission walk, once per COMMITTED method,
 *    only after the whole submission succeeded, and while the PFIFO lock is
 *    held. It must therefore be lock-free and must not call back into
 *    nv2a_submit_pending, the MMIO hook, or anything that takes the PFIFO lock.
 *  - Rejected submissions call it zero times: a walk that fails commits nothing
 *    and consumes nothing.
 *  - Each entry carries the class that was bound when that method was walked, so
 *    a mid-stream SET_OBJECT rebind does not retro-label earlier methods.
 *  - Registration is optional and defaults to none.
 *
 * The setter itself is a plain store of a global the walk reads under the lock,
 * so it must be called at bring-up -- before the guest can submit, or while the
 * submitting worker is known to be finished (a fixture). Swapping the consumer
 * while a walk is in flight is a data race, not a supported mode. */
typedef void (*nv2a_commit_consumer_fn)(uint32_t subchannel, uint32_t class_id,
                                        uint32_t method, uint32_t param);
void nv2a_set_commit_consumer(nv2a_commit_consumer_fn fn);

/* Kick observer: told when the guest writes PUT and when a walk consumes it.
 *
 *  - NV2A_KICK: every NV_USER_DMA_PUT write, after PUT is stored and before
 *    the walk it triggers.
 *  - NV2A_COMMIT: after every walk that committed and reached PUT, from any
 *    caller (a PUT write, a resumed hold, nv2a_retry_stalled_walk). A rejected
 *    walk, a walk that stops at a hold, and a hold that is not yet released
 *    report nothing.
 *
 * Called on the walking thread with the PFIFO lock released (in the runtime
 * the MMIO owner lock is held), so the observer must be lock-free and must not
 * call back into the model. Same registration rule as the commit consumer:
 * set it before the guest can submit; NULL clears it. */
enum { NV2A_KICK = 1, NV2A_COMMIT = 2 };
typedef void (*nv2a_kick_observer_fn)(int event);
void nv2a_set_kick_observer(nv2a_kick_observer_fn fn);

/* Re-walk a stalled ring without a PUT write. Only when the last walk was
 * rejected (g_nv2a_submit_state.consecutive_rejections > 0): walks GET..PUT
 * and logs exactly as a PUT-triggered walk, and returns true when it
 * committed. Otherwise walks nothing and returns false. Reports no NV2A_KICK.
 * A rejection can clear without a new kick (a RAMHT entry bound, a pushbuffer
 * word patched), and D3D's ring-space wait never kicks, so something must
 * retry; the MMIO hook's PTIMER thread does, under the owner lock. */
bool nv2a_retry_stalled_walk(NV2AState *d);

/* NV097 action methods: the semaphore release, and the software-method trap
 * and FLIP_STALL hold. Modelled but not admitted as hardware causes, so they
 * run only when RECOMP_NV2A_ACTIONS is exactly "1" (read once). */
bool nv2a_actions_enabled(void);
/* Test seam: 1 or 0 forces the switch, -1 re-reads the environment. */
void nv2a_actions_override_for_test(int enabled);
/* Called by the fence mirror before it writes a guest word; logs when that
 * word is the one the last semaphore release wrote. */
void nv2a_note_fence_mirror_write(const volatile void *host, uint32_t value);
uint32_t nv2a_fence_mirror_overlaps(void);

/* Get the global NV2A state instance */
NV2AState *nv2a_get_state(void);

/* Service and schedule PTIMER alarms without requiring a PTIMER MMIO access. */
void nv2a_ptimer_service(NV2AState *d);
uint64_t nv2a_ptimer_next_alarm_ns(NV2AState *d);
void nv2a_ptimer_set_clock(NV2AState *d, uint64_t (*clock_ns)(void *),
                           void *opaque);

/* PBUS 0x800..0x87f and HAL bus 1/slot 0 deliberately share this backing. */
bool nv2a_pci_config_read(NV2AState *d, uint32_t offset,
                          void *buffer, uint32_t length);
bool nv2a_pci_config_write(NV2AState *d, uint32_t offset,
                           const void *buffer, uint32_t length);

#endif /* BURNOUT3_NV2A_STATE_H */
