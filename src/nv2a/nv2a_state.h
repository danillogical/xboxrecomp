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

/* ============================================================
 * NV2AState - Main GPU state
 * Adapted from xemu's nv2a_int.h
 * ============================================================ */

/* One submission's walk budget, in words, and the capacity of everything the
 * walk stages or records for that submission.
 *
 * The walk commits a submission all-or-nothing: it stages every method before
 * it commits any of them, so the staging array and the sink both have to hold
 * a whole budget's worth. Each staged method consumes exactly one parameter
 * word, so staged methods <= walked words <= NV2A_SUBMIT_MAX_WORDS. The walk
 * separately bounds itself to 1024 packets, which is unchanged. */
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
