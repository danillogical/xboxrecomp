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

struct PGRAPHState {
    uint32_t pending_interrupts;
    uint32_t enabled_interrupts;
    uint32_t regs[0x2000];
    /* Phase 3-4: Full PGRAPH state will go here */
};

/* ============================================================
 * NV2AState - Main GPU state
 * Adapted from xemu's nv2a_int.h
 * ============================================================ */

typedef struct NV2AState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    qemu_irq irq;
    bool exiting;

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
        struct { uint32_t subchannel, method, param; } sink[256];
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
const char *nv2a_submit_diagnostic(uint32_t code);

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
