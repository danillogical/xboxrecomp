/*
 * NV2A GPU Core - Standalone register handlers
 *
 * Adapted from xemu (Copyright (c) 2012 espes, 2015 Jannik Vogel,
 * 2018-2025 Matt Borgerson) - LGPL v2+
 *
 * Contains: PMC, PBUS, PTIMER, PFB, PCRTC, PRAMDAC register handlers,
 * nv2a_update_irq, DMA helpers, block dispatch table, and standalone init.
 */

#include "nv2a_state.h"
#include "nv2a_pgraph_d3d11.h"

/* ============================================================
 * Global state
 * ============================================================ */

static NV2AState *g_nv2a = NULL;
static MemoryRegion g_vram_region;
static uint32_t g_pending_instance_guest_base;
static uint8_t *g_pending_instance_host_ptr;
static uint32_t g_pending_instance_size;

NV2ASubmitState g_nv2a_submit_state;

/* Log bookkeeping for the submit diagnostics; cleared with the state above. */
#define NV2A_ADMIT_LOG_MAX  256
#define NV2A_ADMIT_PENDING  16
typedef struct { uint32_t class_id, method, param, at; } AdmitRecord;
static uint32_t g_reject_lines;
static uint32_t g_last_reject_diag;
static uint32_t g_logged_consecutive;
static bool g_admit_banner;
static AdmitRecord g_admit_seen[NV2A_ADMIT_LOG_MAX];
static uint32_t g_admit_seen_count;
static AdmitRecord g_admit_pending[NV2A_ADMIT_PENDING];
static uint32_t g_admit_pending_count;
static int g_admit_unknown = -1;

void nv2a_admit_unknown_override(int value)
{
    g_admit_unknown = value < 0 ? -1 : value != 0;
}

bool nv2a_admit_unknown_enabled(void)
{
    if (g_admit_unknown < 0) {
        const char *v = getenv("RECOMP_NV2A_ADMIT_UNKNOWN");
        g_admit_unknown = v != NULL && strcmp(v, "1") == 0;
    }
    return g_admit_unknown != 0;
}

static bool instance_binding_valid(uint32_t guest_base, const uint8_t *host_ptr,
                                   uint32_t size)
{
    return host_ptr != NULL && size != 0 &&
           guest_base <= UINT32_MAX - size;
}

bool nv2a_bind_instance_memory(uint32_t guest_base, uint8_t *host_ptr,
                               uint32_t size)
{
    if (!instance_binding_valid(guest_base, host_ptr, size)) {
        NV2A_DPRINTF("rejecting invalid instance-memory binding base=0x%08x size=0x%x\n",
                     guest_base, size);
        return false;
    }

    /* Keep the request until the standalone state exists.  This is the
     * explicit ordering seam between the kernel bridge and the MMIO hook. */
    if (g_pending_instance_host_ptr) {
        return g_pending_instance_guest_base == guest_base &&
               g_pending_instance_host_ptr == host_ptr &&
               g_pending_instance_size == size;
    }
    g_pending_instance_guest_base = guest_base;
    g_pending_instance_host_ptr = host_ptr;
    g_pending_instance_size = size;
    if (g_nv2a) {
        g_nv2a->ramin.size = size;
        g_nv2a->ramin_ptr = host_ptr;
        g_nv2a->ramin_guest_base = guest_base;
    }
    return true;
}

NV2AState *nv2a_get_state(void) {
    return g_nv2a;
}

void nv2a_reset_standalone_for_test(void)
{
    /* Focused single-threaded fixtures use this only to exercise both sides
     * of the initialization-order contract in one process.  The abandoned
     * state is intentionally not destroyed because its qemu locks may still
     * contain platform-owned bookkeeping. */
    g_nv2a = NULL;
    g_pending_instance_guest_base = 0;
    g_pending_instance_host_ptr = NULL;
    g_pending_instance_size = 0;
    memset(&g_nv2a_submit_state, 0, sizeof(g_nv2a_submit_state));
    g_reject_lines = 0;
    g_last_reject_diag = 0;
    g_logged_consecutive = 0;
    g_admit_banner = false;
    g_admit_seen_count = 0;
    g_admit_pending_count = 0;
}

static bool pci_config_access_valid(uint32_t offset, uint32_t length)
{
    return (length == 1 || length == 2 || length == 4) &&
           offset < PCI_CONFIG_SPACE_SIZE &&
           length <= PCI_CONFIG_SPACE_SIZE - offset;
}

bool nv2a_pci_config_read(NV2AState *d, uint32_t offset,
                          void *buffer, uint32_t length)
{
    if (!d || !buffer || !pci_config_access_valid(offset, length)) return false;
    memcpy(buffer, d->parent_obj.config + offset, length);
    return true;
}

bool nv2a_pci_config_write(NV2AState *d, uint32_t offset,
                           const void *buffer, uint32_t length)
{
    if (!d || !buffer || !pci_config_access_valid(offset, length)) return false;
    /* Vendor/device and class/revision are immutable PCI identity fields. */
    const uint8_t *src = (const uint8_t *)buffer;
    for (uint32_t i = 0; i < length; ++i) {
        uint32_t byte_offset = offset + i;
        if ((byte_offset >= 0x00 && byte_offset < 0x04) ||
            (byte_offset >= 0x08 && byte_offset < 0x0C)) continue;
        d->parent_obj.config[byte_offset] = src[i];
    }
    return true;
}

/* ============================================================
 * IRQ aggregation (from xemu nv2a.c)
 * ============================================================ */

int nv2a_irq_line_asserted(NV2AState *d)
{
    /* NV_PMC_INTR_EN_0 is a two-bit master enable (hardware/software), not a
     * per-source mask: the per-source masks are the block registers, and
     * `nv2a_update_irq` has already folded them into the PMC summary. So the
     * line is "something is pending and interrupts are enabled at all". */
    return d && d->pmc.pending_interrupts != 0 && d->pmc.enabled_interrupts != 0;
}

void nv2a_set_irq_sink(NV2AState *d,
                       void (*sink)(void *opaque, int asserted), void *opaque)
{
    if (!d) return;
    d->irq_sink = sink;
    d->irq_sink_opaque = opaque;
    d->irq_line_asserted = nv2a_irq_line_asserted(d);
}

void nv2a_update_irq(NV2AState *d)
{
    /* PFIFO */
    if (d->pfifo.pending_interrupts & d->pfifo.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PFIFO;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PFIFO;
    }

    /* PCRTC */
    if (d->pcrtc.pending_interrupts & d->pcrtc.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PCRTC;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PCRTC;
    }

    /* PGRAPH */
    if (d->pgraph.pending_interrupts & d->pgraph.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PGRAPH;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PGRAPH;
    }

    /* PTIMER */
    if (d->ptimer.pending_interrupts & d->ptimer.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PTIMER;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PTIMER;
    }

    int asserted = nv2a_irq_line_asserted(d);
    if (asserted) {
        pci_irq_assert(PCI_DEVICE(d));
    } else {
        pci_irq_deassert(PCI_DEVICE(d));
    }
    /* Edge-report the level. `nv2a_update_irq` runs on every register write
     * that touches a block's interrupt state, so reporting unconditionally
     * would re-enter the guest's ISR for a line the guest has not cleared
     * yet -- which is a livelock, not a level-triggered interrupt. */
    if (d->irq_sink && asserted != d->irq_line_asserted) {
        d->irq_line_asserted = asserted;
        d->irq_sink(d->irq_sink_opaque, asserted);
    }
}

/* ============================================================
 * Display clock
 *
 * The card has a CRT controller that produces a vertical blank once per
 * frame. The model previously had no source for it at all: the kernel's
 * synthetic vblank poke OR-ed into NV_PMC_INTR_0 and NV_PCRTC_INTR_0, and
 * both of those registers are write-1-to-clear, so the poke cleared pending
 * bits instead of setting them. Nothing else asserted a pending bit, so
 * `nv2a_update_irq` had nothing to aggregate and the guest's ISR never saw a
 * PCRTC interrupt.
 * ============================================================ */

void nv2a_vblank_pulse(NV2AState *d)
{
    if (!d) return;
    /* The pending bit is set by the display, unconditionally: the enable
     * masks gate delivery, never the source. */
    d->pcrtc.pending_interrupts |= NV_PCRTC_INTR_0_VBLANK;
    nv2a_update_irq(d);
}

/* The pixel clock is the video PLL, decoded exactly as the core PLL is. */
static uint64_t pramdac_video_clock_freq(NV2AState *d)
{
    uint32_t val = d->pramdac.video_clock_coeff;
    uint32_t m = val & NV_PRAMDAC_VPLL_COEFF_MDIV;
    uint32_t n = (val & NV_PRAMDAC_VPLL_COEFF_NDIV) >> 8;
    uint32_t p = (val & NV_PRAMDAC_VPLL_COEFF_PDIV) >> 16;
    if (m == 0 || n == 0) return 0;
    return (uint64_t)((NV2A_CRYSTAL_FREQ * n) / (1u << p) / m);
}

static const char *g_display_frame_source = "60hz-fallback";

const char *nv2a_display_frame_source(void)
{
    return g_display_frame_source;
}

uint64_t nv2a_display_frame_ns(NV2AState *d)
{
    uint64_t pixel_hz, htotal, vtotal, frame_ns;

    g_display_frame_source = "60hz-fallback";
    if (d) {
        pixel_hz = pramdac_video_clock_freq(d);
        htotal = d->pramdac.fp_hcrtc;
        vtotal = d->pramdac.fp_vcrtc;
        if (pixel_hz && htotal && vtotal) {
            frame_ns = (uint64_t)((long double)htotal * (long double)vtotal
                                  * (long double)NANOSECONDS_PER_SECOND
                                  / (long double)pixel_hz);
            /* 40..240 Hz. Outside that the timing registers are not a mode
             * this model can honestly read, and a wrong period is worse than
             * a documented nominal one. */
            if (frame_ns >= 1000000000ull / 240ull &&
                frame_ns <= 1000000000ull / 40ull) {
                g_display_frame_source = "video-timing";
                return frame_ns;
            }
        }
    }
    return 1000000000ull / 60ull;
}

/* ============================================================
 * DMA helpers (from xemu nv2a.c)
 * ============================================================ */

DMAObject nv_dma_load(NV2AState *d, hwaddr dma_obj_address)
{
    if (!d || !d->ramin_ptr || memory_region_size(&d->ramin) < 12 ||
        dma_obj_address > memory_region_size(&d->ramin) - 12) {
        NV2A_DPRINTF("RAMIN DMA object outside bound: 0x%llx\n",
                     (unsigned long long)dma_obj_address);
        return (DMAObject){0};
    }

    uint32_t *dma_obj = (uint32_t *)(d->ramin_ptr + dma_obj_address);
    uint32_t flags = ldl_le_p(dma_obj);
    uint32_t limit = ldl_le_p(dma_obj + 1);
    uint32_t frame = ldl_le_p(dma_obj + 2);

    return (DMAObject){
        .dma_class  = GET_MASK(flags, NV_DMA_CLASS),
        .dma_target = GET_MASK(flags, NV_DMA_TARGET),
        .address    = (frame & NV_DMA_ADDRESS) | GET_MASK(flags, NV_DMA_ADJUST),
        .limit      = limit,
    };
}

void *nv_dma_map(NV2AState *d, hwaddr dma_obj_address, hwaddr *len)
{
    if (!len)
        return NULL;
    *len = 0;
    if (!d || !d->ramin_ptr || !d->vram || !d->vram_ptr ||
        dma_obj_address > memory_region_size(&d->ramin) ||
        memory_region_size(&d->ramin) - dma_obj_address < 12) {
        return NULL;
    }
    DMAObject dma = nv_dma_load(d, dma_obj_address);
    dma.address &= 0x07FFFFFF;

    if (dma.address >= memory_region_size(d->vram)) {
        fprintf(stderr, "[NV2A] DMA map address 0x%llx out of VRAM range\n",
                (unsigned long long)dma.address);
        return NULL;
    }

    *len = dma.limit;
    return d->vram_ptr + dma.address;
}

/* ============================================================
 * PMC - card master control (from xemu pmc.c)
 * ============================================================ */

uint64_t pmc_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PMC_BOOT_0:
        /* NV2A, A03, Rev 0 */
        r = 0x02A000A3;
        break;
    case NV_PMC_INTR_0:
        r = d->pmc.pending_interrupts;
        break;
    case NV_PMC_INTR_EN_0:
        r = d->pmc.enabled_interrupts;
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PMC, addr, size, r);
    return r;
}

void pmc_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PMC, addr, size, val);

    switch (addr) {
    case NV_PMC_INTR_0:
        d->pmc.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PMC_INTR_EN_0:
        d->pmc.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    default:
        break;
    }
}

/* ============================================================
 * PBUS - bus control (from xemu pbus.c)
 * ============================================================ */

static bool pbus_pci_access_valid(hwaddr addr, unsigned int size,
                                  uint32_t *config_offset)
{
    uint64_t offset;
    if (addr < NV_PBUS_PCI_NV_0 ||
        (size != 1 && size != 2 && size != 4)) return false;
    offset = addr - NV_PBUS_PCI_NV_0;
    if (offset >= 0x80 || size > 0x80 - offset) return false;
    *config_offset = (uint32_t)offset;
    return true;
}

uint64_t pbus_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *s = (NV2AState *)opaque;
    uint64_t r = 0;
    uint32_t config_offset;
    if (pbus_pci_access_valid(addr, size, &config_offset)) {
        nv2a_pci_config_read(s, config_offset, &r, size);
    }

    nv2a_reg_log_read(NV_PBUS, addr, size, r);
    return r;
}

void pbus_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *s = (NV2AState *)opaque;
    uint32_t config_offset;
    nv2a_reg_log_write(NV_PBUS, addr, size, val);
    if (pbus_pci_access_valid(addr, size, &config_offset)) {
        nv2a_pci_config_write(s, config_offset, &val, size);
    }
}

/* ============================================================
 * PTIMER - time measurement (compatible with xemu ptimer.c)
 *
 * Behavioral reference: xemu f9b14039e5bb56ae2d8f028e31e7cc19f13f7e12,
 * hw/xbox/nv2a/ptimer.c. The standalone owner supplies the host timer queue.
 * ============================================================ */

#define PTIMER_CLOCK_HIGH_MASK 0x1fffffffULL
#define PTIMER_ALARM_MASK      0xffffffe0ULL
#define PTIMER_REG_TIME_MASK   ((PTIMER_CLOCK_HIGH_MASK << 32) | PTIMER_ALARM_MASK)
#define PTIMER_INTERNAL_TIME_MASK (PTIMER_REG_TIME_MASK >> 5)

static uint64_t ptimer_absolute_clock(NV2AState *d)
{
    if (d->ptimer.numerator == 0 || d->ptimer.denominator == 0 ||
        d->pramdac.core_clock_freq == 0) return 0;
    uint64_t now_ns = d->ptimer.clock_ns ?
        d->ptimer.clock_ns(d->ptimer.clock_opaque) :
        (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return muldiv64(muldiv64(now_ns,
                             (uint32_t)d->pramdac.core_clock_freq,
                             NANOSECONDS_PER_SECOND),
                    d->ptimer.denominator, d->ptimer.numerator);
}

static uint64_t ptimer_internal_clock(NV2AState *d)
{
    return (ptimer_absolute_clock(d) + d->ptimer.time_offset) &
           PTIMER_INTERNAL_TIME_MASK;
}

static uint64_t ptimer_reg_clock(NV2AState *d)
{
    return (ptimer_internal_clock(d) << 5) & PTIMER_REG_TIME_MASK;
}

static uint64_t ptimer_alarm_distance(uint64_t now, uint64_t alarm)
{
    uint64_t diff = (alarm - now) & PTIMER_REG_TIME_MASK;
    return diff > (PTIMER_REG_TIME_MASK >> 1) ? 0 : diff;
}

static uint64_t ptimer_next_alarm_time(uint64_t now, uint64_t alarm)
{
    uint64_t target = (now & ~0xffffffffULL) | (alarm & PTIMER_ALARM_MASK);
    if ((alarm & PTIMER_ALARM_MASK) <= (now & PTIMER_ALARM_MASK))
        target += 1ULL << 32;
    return target & PTIMER_REG_TIME_MASK;
}

void nv2a_ptimer_service(NV2AState *d)
{
    if (!(d->ptimer.enabled_interrupts & NV_PTIMER_INTR_0_ALARM) ||
        ptimer_alarm_distance(ptimer_reg_clock(d), d->ptimer.alarm_time) != 0)
        return;
    d->ptimer.pending_interrupts |= NV_PTIMER_INTR_0_ALARM;
    /* Select the first matching low-word epoch after now in one step. This
     * consumes any number of missed periods, so W1C remains clear. */
    uint64_t now = ptimer_reg_clock(d);
    d->ptimer.alarm_time = ptimer_next_alarm_time(now, d->ptimer.alarm_time);
    nv2a_update_irq(d);
}

uint64_t nv2a_ptimer_next_alarm_ns(NV2AState *d)
{
    uint64_t diff_reg, internal_ticks;
    long double ns;
    if (!(d->ptimer.enabled_interrupts & NV_PTIMER_INTR_0_ALARM))
        return UINT64_MAX;
    diff_reg = ptimer_alarm_distance(ptimer_reg_clock(d), d->ptimer.alarm_time);
    if (!diff_reg) return 0;
    if (!d->ptimer.numerator || !d->ptimer.denominator ||
        !d->pramdac.core_clock_freq)
        return UINT64_MAX;
    internal_ticks = (diff_reg + 31) >> 5;
    ns = (long double)internal_ticks * d->ptimer.numerator *
         NANOSECONDS_PER_SECOND /
         ((long double)d->ptimer.denominator * d->pramdac.core_clock_freq);
    if (ns >= (long double)UINT64_MAX) return UINT64_MAX;
    uint64_t rounded = (uint64_t)ns;
    return rounded + ((long double)rounded < ns);
}

void nv2a_ptimer_set_clock(NV2AState *d, uint64_t (*clock_ns)(void *),
                           void *opaque)
{
    d->ptimer.clock_ns = clock_ns;
    d->ptimer.clock_opaque = opaque;
}

static uint64_t ptimer_get_clock(NV2AState *d)
{
    return ptimer_internal_clock(d);
}

uint64_t ptimer_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_ptimer_service(d);
    uint64_t r = 0;
    switch (addr) {
    case NV_PTIMER_INTR_0:
        r = d->ptimer.pending_interrupts;
        break;
    case NV_PTIMER_INTR_EN_0:
        r = d->ptimer.enabled_interrupts;
        break;
    case NV_PTIMER_NUMERATOR:
        r = d->ptimer.numerator;
        break;
    case NV_PTIMER_DENOMINATOR:
        r = d->ptimer.denominator;
        break;
    case NV_PTIMER_TIME_0:
        r = ptimer_reg_clock(d) & 0xffffffffULL;
        break;
    case NV_PTIMER_TIME_1:
        r = (ptimer_reg_clock(d) >> 32) & PTIMER_CLOCK_HIGH_MASK;
        break;
    case NV_PTIMER_ALARM_0:
        r = d->ptimer.alarm_time & 0xffffffffULL;
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PTIMER, addr, size, r);
    return r;
}

void ptimer_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PTIMER, addr, size, val);

    nv2a_ptimer_service(d);

    switch (addr) {
    case NV_PTIMER_INTR_0:
        d->ptimer.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PTIMER_INTR_EN_0:
        d->ptimer.enabled_interrupts = val;
        /* xemu evaluates a late enable against the current time. */
        nv2a_ptimer_service(d);
        nv2a_update_irq(d);
        break;
    case NV_PTIMER_DENOMINATOR:
        d->ptimer.denominator = val;
        break;
    case NV_PTIMER_NUMERATOR:
        d->ptimer.numerator = val;
        break;
    case NV_PTIMER_ALARM_0:
    {
        uint64_t now = ptimer_reg_clock(d);
        d->ptimer.alarm_time = ptimer_next_alarm_time(now, val);
        nv2a_ptimer_service(d);
        break;
    }
    case NV_PTIMER_TIME_0:
    {
        uint64_t now = ptimer_reg_clock(d);
        uint64_t target = (now & ~0xffffffffULL) | (val & PTIMER_ALARM_MASK);
        d->ptimer.time_offset =
            ((target >> 5) & PTIMER_INTERNAL_TIME_MASK) - ptimer_absolute_clock(d);
        break;
    }
    case NV_PTIMER_TIME_1:
    {
        uint64_t now = ptimer_reg_clock(d);
        uint64_t target = ((val & PTIMER_CLOCK_HIGH_MASK) << 32) |
                          (now & 0xffffffffULL);
        d->ptimer.time_offset =
            ((target >> 5) & PTIMER_INTERNAL_TIME_MASK) - ptimer_absolute_clock(d);
        break;
    }
    default:
        break;
    }
}

/* ============================================================
 * PFB - framebuffer / memory control (from xemu pfb.c)
 * ============================================================ */

uint64_t pfb_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PFB_CSTATUS:
        r = memory_region_size(d->vram);
        break;
    case NV_PFB_WBC:
        r = 0; /* Flush not pending */
        break;
    default:
        r = d->pfb.regs[addr];
        break;
    }

    nv2a_reg_log_read(NV_PFB, addr, size, r);
    return r;
}

void pfb_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PFB, addr, size, val);

    switch (addr) {
    default:
        d->pfb.regs[addr] = val;
        break;
    }
}

/* ============================================================
 * PCRTC - CRT controller (from xemu pcrtc.c)
 * ============================================================ */

uint64_t pcrtc_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PCRTC_INTR_0:
        r = d->pcrtc.pending_interrupts;
        break;
    case NV_PCRTC_INTR_EN_0:
        r = d->pcrtc.enabled_interrupts;
        break;
    case NV_PCRTC_START:
        r = d->pcrtc.start;
        break;
    case NV_PCRTC_RASTER:
        r = d->pcrtc.raster++;
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PCRTC, addr, size, r);
    return r;
}

void pcrtc_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PCRTC, addr, size, val);

    switch (addr) {
    case NV_PCRTC_INTR_0:
        d->pcrtc.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PCRTC_INTR_EN_0:
        d->pcrtc.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    case NV_PCRTC_START:
        val &= 0x07FFFFFF;
        d->pcrtc.start = val;
        NV2A_DPRINTF("PCRTC_START - %x %x %x %x\n",
                d->vram_ptr[val+64], d->vram_ptr[val+64+1],
                d->vram_ptr[val+64+2], d->vram_ptr[val+64+3]);
        break;
    default:
        break;
    }
}

/* ============================================================
 * PRAMDAC - RAMDAC / PLL control (from xemu pramdac.c)
 * ============================================================ */

uint64_t pramdac_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr & ~3) {
    case NV_PRAMDAC_NVPLL_COEFF:
        r = d->pramdac.core_clock_coeff;
        break;
    case NV_PRAMDAC_MPLL_COEFF:
        r = d->pramdac.memory_clock_coeff;
        break;
    case NV_PRAMDAC_VPLL_COEFF:
        r = d->pramdac.video_clock_coeff;
        break;
    case NV_PRAMDAC_PLL_TEST_COUNTER:
        /* emulated PLLs locked instantly */
        r = NV_PRAMDAC_PLL_TEST_COUNTER_VPLL2_LOCK
             | NV_PRAMDAC_PLL_TEST_COUNTER_NVPLL_LOCK
             | NV_PRAMDAC_PLL_TEST_COUNTER_MPLL_LOCK
             | NV_PRAMDAC_PLL_TEST_COUNTER_VPLL_LOCK;
        break;
    case NV_PRAMDAC_GENERAL_CONTROL:
        r = d->pramdac.general_control;
        break;
    case NV_PRAMDAC_FP_VDISPLAY_END:
        r = d->pramdac.fp_vdisplay_end;
        break;
    case NV_PRAMDAC_FP_VCRTC:
        r = d->pramdac.fp_vcrtc;
        break;
    case NV_PRAMDAC_FP_VSYNC_END:
        r = d->pramdac.fp_vsync_end;
        break;
    case NV_PRAMDAC_FP_VVALID_END:
        r = d->pramdac.fp_vvalid_end;
        break;
    case NV_PRAMDAC_FP_HDISPLAY_END:
        r = d->pramdac.fp_hdisplay_end;
        break;
    case NV_PRAMDAC_FP_HCRTC:
        r = d->pramdac.fp_hcrtc;
        break;
    case NV_PRAMDAC_FP_HVALID_END:
        r = d->pramdac.fp_hvalid_end;
        break;
    default:
        break;
    }

    /* Handle unaligned access */
    r >>= 32 - 8 * size - 8 * (addr & 3);

    nv2a_reg_log_read(NV_PRAMDAC, addr, size, r);
    return r;
}

void pramdac_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint32_t m, n, p;

    nv2a_reg_log_write(NV_PRAMDAC, addr, size, val);

    switch (addr) {
    case NV_PRAMDAC_NVPLL_COEFF:
        d->pramdac.core_clock_coeff = val;

        m = val & NV_PRAMDAC_NVPLL_COEFF_MDIV;
        n = (val & NV_PRAMDAC_NVPLL_COEFF_NDIV) >> 8;
        p = (val & NV_PRAMDAC_NVPLL_COEFF_PDIV) >> 16;

        if (m == 0) {
            d->pramdac.core_clock_freq = 0;
        } else {
            d->pramdac.core_clock_freq = (NV2A_CRYSTAL_FREQ * n)
                                          / (1 << p) / m;
        }
        break;
    case NV_PRAMDAC_MPLL_COEFF:
        d->pramdac.memory_clock_coeff = val;
        break;
    case NV_PRAMDAC_VPLL_COEFF:
        d->pramdac.video_clock_coeff = val;
        break;
    case NV_PRAMDAC_GENERAL_CONTROL:
        d->pramdac.general_control = val;
        break;
    case NV_PRAMDAC_FP_VDISPLAY_END:
        d->pramdac.fp_vdisplay_end = val;
        break;
    case NV_PRAMDAC_FP_VCRTC:
        d->pramdac.fp_vcrtc = val;
        break;
    case NV_PRAMDAC_FP_VSYNC_END:
        d->pramdac.fp_vsync_end = val;
        break;
    case NV_PRAMDAC_FP_VVALID_END:
        d->pramdac.fp_vvalid_end = val;
        break;
    case NV_PRAMDAC_FP_HDISPLAY_END:
        d->pramdac.fp_hdisplay_end = val;
        break;
    case NV_PRAMDAC_FP_HCRTC:
        d->pramdac.fp_hcrtc = val;
        break;
    case NV_PRAMDAC_FP_HVALID_END:
        d->pramdac.fp_hvalid_end = val;
        break;
    default:
        break;
    }
}

/* ============================================================
 * PVIDEO - video overlay (stub)
 * ============================================================ */

uint64_t pvideo_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint64_t r = d->pvideo.regs[addr];
    nv2a_reg_log_read(NV_PVIDEO, addr, size, r);
    return r;
}

void pvideo_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    nv2a_reg_log_write(NV_PVIDEO, addr, size, val);
    d->pvideo.regs[addr] = val;
}

/* ============================================================
 * PGRAPH - graphics engine (stub for Phase 1)
 * ============================================================ */

static uint32_t flip_counter_next(uint32_t value, uint32_t modulo);

uint64_t pgraph_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint64_t r = d->pgraph.regs[addr];
    nv2a_reg_log_read(NV_PGRAPH, addr, size, r);
    return r;
}

/* The guest's acknowledgements of a held walk (action methods only): each may
 * release it, and nv2a_submit_pending decides whether it has. */
static void pgraph_resume_walk(NV2AState *d)
{
    if (d->pfifo.hold != NV2A_HOLD_NONE)
        nv2a_submit_pending(d);
}

void pgraph_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    bool actions = nv2a_actions_enabled();
    nv2a_reg_log_write(NV_PGRAPH, addr, size, val);
    if (addr == NV_PGRAPH_INTR) {
        d->pgraph.regs[addr] &= ~(uint32_t)val;
        d->pgraph.pending_interrupts &= ~(uint32_t)val;
        /* NSOURCE clears with the interrupt it explains. */
        if (actions && (val & NV_PGRAPH_INTR_ERROR))
            d->pgraph.regs[NV_PGRAPH_NSOURCE] = 0;
        nv2a_update_irq(d);
        if (actions) pgraph_resume_walk(d);
        return;
    }
    if (actions) {
        switch (addr) {
        case NV_PGRAPH_INTR_EN:
            d->pgraph.regs[addr] = val;
            d->pgraph.enabled_interrupts = (uint32_t)val;
            nv2a_update_irq(d);
            return;
        case NV_PGRAPH_INCREMENT:
            /* A trigger, not storage: the display's vblank handler advances
             * the read counter a FLIP_STALL waits on. */
            if (val & NV_PGRAPH_INCREMENT_READ_3D) {
                uint32_t *surface = &d->pgraph.regs[NV_PGRAPH_SURFACE];
                uint32_t next = flip_counter_next(GET_MASK(*surface, NV_PGRAPH_SURFACE_READ_3D),
                                                  GET_MASK(*surface, NV_PGRAPH_SURFACE_MODULO_3D));
                SET_MASK(*surface, NV_PGRAPH_SURFACE_READ_3D, next);
                pgraph_resume_walk(d);
            }
            return;
        case NV_PGRAPH_FIFO:
            d->pgraph.regs[addr] = val;
            pgraph_resume_walk(d);
            return;
        default:
            break;
        }
    }
    d->pgraph.regs[addr] = val;
}

/* ============================================================
 * PGRAPH method dispatch
 * Called when push buffer commands are parsed.
 * Routes method calls into PGRAPH register writes.
 * ============================================================ */

static uint32_t g_pgraph_method_count = 0;
static uint32_t g_pgraph_draw_count = 0;
static uint32_t g_pgraph_clear_count = 0;
static uint32_t g_pgraph_flip_count = 0;
static uint32_t g_pgraph_inline_verts = 0;
static int g_pgraph_in_begin = 0;

/* Class ids JSRF binds. Whether a method may be executed, and what it means,
 * is a per-class decision: 0x2FC is NV09F_SET_OPERATION and an NV097 surface
 * method, and both appear in JSRF's stream. */
#define NV097_CLASS         0x97u
#define NV_MEMCPY_CLASS     0x39u   /* NV_MEMORY_TO_MEMORY_FORMAT */
#define NV_SURFACES2D_CLASS 0x62u   /* NV_CONTEXT_SURFACES_2D */
#define NV_IMAGEBLIT_CLASS  0x9Fu   /* NV_IMAGE_BLIT */

void pgraph_method(NV2AState *d, uint32_t subchannel,
                   uint32_t method, uint32_t param)
{
    /* The translator and the counters below read NV097 method numbers. A
     * subchannel bound to another class would be misread as Kelvin, so its
     * methods only count as unhandled. Class 0 is a subchannel no SET_OBJECT
     * bound (the replay and test generators), which keeps the Kelvin reading
     * those generators were written for. */
    uint32_t class_id = subchannel < 8 ? d->pfifo.binding_class[subchannel] : 0;

    g_pgraph_method_count++;

    if (class_id != 0 && class_id != NV097_CLASS) {
        if (g_pgraph_method_count <= 20 || (g_pgraph_method_count % 5000) == 0) {
            fprintf(stderr, "[PGRAPH] #%u UNHANDLED sub=%u class=0x%02X 0x%04X = 0x%08X\n",
                    g_pgraph_method_count, subchannel, class_id, method, param);
        }
        return;
    }

    /* Route through D3D11 translator first */
    if (pgraph_d3d11_method(subchannel, method, param)) {
        /* Handled by D3D11 translator -- still store it for state queries */
        if (method / 4 < NV2A_PGRAPH_METHOD_WORDS) {
            d->pgraph.methods[method / 4] = param;
        }
        return;
    }

    /* Log unhandled methods (first 20 + periodic) */
    if (g_pgraph_method_count <= 20 || (g_pgraph_method_count % 5000) == 0) {
        fprintf(stderr, "[PGRAPH] #%u UNHANDLED sub=%u 0x%04X = 0x%08X\n",
                g_pgraph_method_count, subchannel, method, param);
    }

    /* Store the method parameter as method state */
    if (method / 4 < NV2A_PGRAPH_METHOD_WORDS) {
        d->pgraph.methods[method / 4] = param;
    }

    /* Track high-level operations (legacy counters) */
    switch (method) {
    case NV097_CLEAR_SURFACE:
        g_pgraph_clear_count++;
        break;

    case NV097_SET_BEGIN_END:
        if (param != 0) {
            g_pgraph_in_begin = 1;
            g_pgraph_draw_count++;
        } else {
            g_pgraph_in_begin = 0;
        }
        break;

    case NV097_INLINE_ARRAY:
        if (g_pgraph_in_begin) {
            g_pgraph_inline_verts++;
        }
        break;

    case NV097_FLIP_INCREMENT_WRITE:
        g_pgraph_flip_count++;
        if (g_pgraph_flip_count <= 5 || (g_pgraph_flip_count % 300) == 0) {
            fprintf(stderr, "[PGRAPH] Frame %u: %u methods, %u draws, %u clears, %u inline verts\n",
                    g_pgraph_flip_count, g_pgraph_method_count,
                    g_pgraph_draw_count, g_pgraph_clear_count,
                    g_pgraph_inline_verts);
        }
        break;

    default:
        break;
    }
}

/* ============================================================
 * PFIFO - bounded USER submission (11b4)
 * ============================================================ */

enum {
    NV2A_SUBMIT_OK = 0,
    NV2A_SUBMIT_UNMAPPED = 1,
    NV2A_SUBMIT_UNREADABLE = 2,
    NV2A_SUBMIT_RESERVED = 3,
    NV2A_SUBMIT_TRUNCATED = 4,
    NV2A_SUBMIT_BUDGET = 5,
    NV2A_SUBMIT_LOOP = 6,
    NV2A_SUBMIT_BAD_TARGET = 7,
    NV2A_SUBMIT_METHOD_RANGE = 8,
    NV2A_SUBMIT_SINK_FULL = 9,
    NV2A_SUBMIT_BAD_POINTER = 10,
    NV2A_SUBMIT_UNSUPPORTED_METHOD = 11,
    NV2A_SUBMIT_INVALID_HANDLE = 12,
    NV2A_SUBMIT_SEMAPHORE_FAULT = 13,
    /* The walk committed up to and including a method that holds it. */
    NV2A_SUBMIT_SOFTWARE_METHOD = 14,
    NV2A_SUBMIT_FLIP_STALL = 15,
    /* A kick arrived while the walk was held; nothing was walked. */
    NV2A_SUBMIT_HELD_SOFTWARE_METHOD = 16,
    NV2A_SUBMIT_HELD_FLIP_STALL = 17,
    /* A non-zero NOP with PGRAPH data checking off: the references disagree
     * on what happens, so the stream stops here instead of guessing. */
    NV2A_SUBMIT_NOP_UNCHECKED = 18,
};

/* NV01_SUBC_SET_OBJECT binds a RAMHT handle.  Production lookup uses the
 * original 0x001945D6 hash into the claimed PRAMIN table; the fixture seam
 * remains test-only. */
#define M_SET_OBJECT 0x0000u

/* The registered committed-method consumer, or NULL. Set once at bring-up from
 * the host side; read by the walk under the PFIFO lock. Nothing here inspects
 * what the consumer is or does -- the core must not depend on the kernel. */
static nv2a_commit_consumer_fn g_commit_consumer;

void nv2a_set_commit_consumer(nv2a_commit_consumer_fn fn)
{
    g_commit_consumer = fn;
}

/* The registered kick observer, or NULL; same registration rule as above. */
static nv2a_kick_observer_fn volatile g_kick_observer;

void nv2a_set_kick_observer(nv2a_kick_observer_fn fn)
{
    g_kick_observer = fn;
}

static void kick_observer_notify(int event)
{
    nv2a_kick_observer_fn fn = g_kick_observer;
    if (fn) fn(event);
}

/* Both capacities must hold a whole submission's word budget: a submission is
 * staged in full before any of it is committed, and every staged method
 * consumes exactly one parameter word, so a walk that stays inside its word
 * budget can never overrun either array. The guards below the walk still check
 * against capacity; with these assertions they are unreachable except by a
 * carry/accounting bug, which must still reject rather than corrupt state. */
_Static_assert(sizeof(((NV2AState *)0)->pfifo.staged) /
                   sizeof(((NV2AState *)0)->pfifo.staged[0]) >=
               NV2A_SUBMIT_MAX_WORDS,
               "submit staging must hold a whole submission word budget");
_Static_assert(sizeof(((NV2AState *)0)->pfifo.sink) /
                   sizeof(((NV2AState *)0)->pfifo.sink[0]) >=
               NV2A_SUBMIT_MAX_WORDS,
               "submit sink must hold a whole submission word budget");

/* Is this a method this model can execute on a subchannel bound to class_id?
 *
 * The table is GENERATED from the pushbuffer the title actually submits
 * (src/nv2a/nv2a_method_table.c, from docs/jsrf-nv2a-method-inventory.md), so
 * "implemented" means "observed in a real submission" rather than "guessed".
 *
 * The contract is unchanged and is the reason this table exists at all: a method
 * not listed causes the whole stream to be rejected and rolled back, which is
 * what jsrf_nv2a_registers pins. Implementing a method means adding it to the
 * inventory and giving it an effect -- it never means loosening the rejection.
 * A subchannel with no binding has class 0, which implements nothing, so an
 * unbound subchannel is still rejected. */
bool nv2a_method_implemented(uint32_t class_id, uint32_t method);

const char *nv2a_submit_diagnostic(uint32_t code)
{
    switch (code) {
    case NV2A_SUBMIT_OK: return "ok";
    case NV2A_SUBMIT_UNMAPPED: return "unmapped_pushbuffer";
    case NV2A_SUBMIT_UNREADABLE: return "unreadable_pushbuffer";
    case NV2A_SUBMIT_RESERVED: return "reserved_opcode";
    case NV2A_SUBMIT_TRUNCATED: return "truncated_packet";
    case NV2A_SUBMIT_BUDGET: return "budget_exhausted";
    case NV2A_SUBMIT_LOOP: return "control_flow_loop";
    case NV2A_SUBMIT_BAD_TARGET: return "invalid_target";
    case NV2A_SUBMIT_METHOD_RANGE: return "method_range_overflow";
    case NV2A_SUBMIT_SINK_FULL: return "sink_capacity";
    case NV2A_SUBMIT_BAD_POINTER: return "invalid_get_put";
    case NV2A_SUBMIT_UNSUPPORTED_METHOD: return "unsupported_method";
    case NV2A_SUBMIT_INVALID_HANDLE: return "invalid_handle";
    case NV2A_SUBMIT_SEMAPHORE_FAULT: return "semaphore_fault";
    case NV2A_SUBMIT_SOFTWARE_METHOD: return "software_method_trap";
    case NV2A_SUBMIT_FLIP_STALL: return "flip_stall";
    case NV2A_SUBMIT_HELD_SOFTWARE_METHOD: return "held_software_method";
    case NV2A_SUBMIT_HELD_FLIP_STALL: return "held_flip_stall";
    case NV2A_SUBMIT_NOP_UNCHECKED: return "software_method_unchecked";
    default: return "unknown";
    }
}

bool nv2a_set_pushbuffer_window(NV2AState *d, uint8_t *base,
                                uint32_t guest_base, uint32_t size)
{
    if (!d || !base || !size || (guest_base & 3) || (size & 3) ||
        guest_base + size < guest_base) return false;
    qemu_mutex_lock(&d->pfifo.lock);
    d->pfifo.pushbuffer = base;
    d->pfifo.pushbuffer_base = guest_base;
    d->pfifo.pushbuffer_size = size;
    qemu_mutex_unlock(&d->pfifo.lock);
    return true;
}

bool nv2a_set_fixture_binding(NV2AState *d, uint32_t subchannel,
                              uint32_t object, uint32_t class_id)
{
    if (!d || subchannel >= 8 || !object || !class_id) return false;
    qemu_mutex_lock(&d->pfifo.lock);
    d->pfifo.fixture_object[subchannel] = object;
    d->pfifo.fixture_class[subchannel] = class_id;
    qemu_mutex_unlock(&d->pfifo.lock);
    return true;
}

bool nv2a_set_fixture_execution(NV2AState *d, bool enabled)
{
    if (!d) return false;
    qemu_mutex_lock(&d->pfifo.lock);
    d->pfifo.fixture_execution = enabled;
    qemu_mutex_unlock(&d->pfifo.lock);
    return true;
}

static bool submit_read_word(NV2AState *d, uint32_t address, uint32_t *word)
{
    uint32_t end = d->pfifo.pushbuffer_base + d->pfifo.pushbuffer_size;
    if (!d->pfifo.pushbuffer || address < d->pfifo.pushbuffer_base ||
        address >= end || (address & 3)) return false;
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *host = d->pfifo.pushbuffer + (address - d->pfifo.pushbuffer_base);
    if (VirtualQuery(host, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        !((mbi.Protect & 0xffu) == PAGE_READONLY ||
          (mbi.Protect & 0xffu) == PAGE_READWRITE ||
          (mbi.Protect & 0xffu) == PAGE_WRITECOPY ||
          (mbi.Protect & 0xffu) == PAGE_EXECUTE_READ ||
          (mbi.Protect & 0xffu) == PAGE_EXECUTE_READWRITE ||
          (mbi.Protect & 0xffu) == PAGE_EXECUTE_WRITECOPY) ||
        host + sizeof(*word) > (uint8_t *)mbi.BaseAddress + mbi.RegionSize)
        return false;
#endif
    memcpy(word, d->pfifo.pushbuffer + (address - d->pfifo.pushbuffer_base), 4);
    return true;
}

static uint32_t submit_advance(NV2AState *d, uint32_t address)
{
    uint32_t end = d->pfifo.pushbuffer_base + d->pfifo.pushbuffer_size;
    return address + 4 == end ? d->pfifo.pushbuffer_base : address + 4;
}

/* Original 0x001945D6: two 11-bit XOR-folds of the handle, then AND 0x7FF.
 * That is the 4K RAMHT fold; other sizes use the same chunk XOR with
 * bits = size_code + 11.  Channel id is 0 in the captured table and is
 * not mixed in by 0x001945D6. */
static uint32_t ramht_hash(uint32_t handle, unsigned bits)
{
    uint32_t mask, hash;
    if (bits == 0 || bits >= 32) return 0;
    mask = (1u << bits) - 1u;
    hash = 0;
    while (handle) {
        hash ^= handle & mask;
        handle >>= bits;
    }
    return hash;
}

/* The instance address RAMHT gives for a handle: the object SET_OBJECT binds,
 * or the DMA object a context-DMA method (0x180..0x1FC) names. */
static bool ramht_lookup_instance(NV2AState *d, uint32_t handle, uint32_t *instance_out)
{
    uint32_t ramht, size_code, ramht_size, ramht_base, bits, hash, slot;
    uint32_t entry_handle, entry_context, instance;
    if (!d || !instance_out || !handle || !d->ramin_ptr || d->ramin.size < 16)
        return false;
    ramht = d->pfifo.regs[NV_PFIFO_RAMHT];
    size_code = GET_MASK(ramht, NV_PFIFO_RAMHT_SIZE);
    if (size_code > NV_PFIFO_RAMHT_SIZE_32K) return false;
    ramht_size = 1u << (size_code + 12);
    ramht_base = GET_MASK(ramht, NV_PFIFO_RAMHT_BASE_ADDRESS) << 12;
    bits = size_code + 11;
    if (ramht_size < 8 || ramht_size > d->ramin.size ||
        ramht_base > d->ramin.size - ramht_size)
        return false;
    hash = ramht_hash(handle, bits);
    slot = hash * 8u;
    if (slot > ramht_size - 8) return false;
    memcpy(&entry_handle, d->ramin_ptr + ramht_base + slot, 4);
    memcpy(&entry_context, d->ramin_ptr + ramht_base + slot + 4, 4);
    if (entry_handle != handle || !(entry_context & NV_RAMHT_STATUS))
        return false;
    instance = (entry_context & NV_RAMHT_INSTANCE) << 4;
    if (instance > d->ramin.size - 16) return false;
    *instance_out = instance;
    return true;
}

static bool ramht_lookup_class(NV2AState *d, uint32_t handle, uint32_t *class_id)
{
    uint32_t instance, object0;
    if (!class_id || !ramht_lookup_instance(d, handle, &instance))
        return false;
    memcpy(&object0, d->ramin_ptr + instance, 4);
    *class_id = object0 & 0xFFu;
    return *class_id != 0;
}

/* ── PFIFO pointer trace ────────────────────────────────────────────────
 * "Why did the ring not drain" is almost always "what were GET and PUT when
 * the kick arrived", and the answer is a write history, not a value.  The
 * submission diagnostic prints the state at the kick; this prints how it got
 * there.  A title that never writes DMA GET leaves it at its reset value, and
 * that is invisible in any single reading -- it only shows up as a walk that
 * starts somewhere other than the ring.
 *
 * Gated by RECOMP_PFIFO_TRACE because it is per-write.  Bounded, because a
 * title that spins on a register would otherwise fill the log. */
static int pfifo_trace_enabled = -1;
static unsigned pfifo_trace_lines;

static void pfifo_trace(const char *site, uint32_t reg, uint32_t value)
{
    const char *name;
    if (pfifo_trace_enabled < 0)
        pfifo_trace_enabled = getenv("RECOMP_PFIFO_TRACE") != NULL;
    if (!pfifo_trace_enabled || pfifo_trace_lines >= 256)
        return;
    name = reg == NV_PFIFO_CACHE1_DMA_GET ? "DMA_GET" : "DMA_PUT";
    fprintf(stderr, "  [PFIFO] %-14s %s = %08X\n", site, name, value);
    fflush(stderr);
    pfifo_trace_lines++;
}

/* ── Action methods (RECOMP_NV2A_ACTIONS=1) ────────────────────────────────
 *
 * The walk records methods as register state and executes nothing with a
 * side effect. The behaviours below are the NV097 ones the guest waits on,
 * modelled from xemu and envytools (docs/technical/nv2a-action-methods.md).
 * They are not admitted as unconditional hardware causes, so they stay
 * dormant unless RECOMP_NV2A_ACTIONS is exactly "1", and a run that sets it
 * is exploratory. With it unset the walk is unchanged.
 *
 * Every effect is staged while the walk runs and applied only when it
 * commits, so a rejected stream leaves guest memory and PGRAPH untouched. */

static int g_actions_enabled = -1;

bool nv2a_actions_enabled(void)
{
    if (g_actions_enabled < 0) {
        const char *v = getenv("RECOMP_NV2A_ACTIONS");
        g_actions_enabled = v != NULL && strcmp(v, "1") == 0;
        if (g_actions_enabled)
            fprintf(stderr, "[NV2A] RECOMP_NV2A_ACTIONS=1: NV097 action methods are"
                    " modelled (not admitted; the run is exploratory)\n");
    }
    return g_actions_enabled != 0;
}

void nv2a_actions_override_for_test(int enabled)
{
    g_actions_enabled = enabled < 0 ? -1 : enabled != 0;
}

#define NV2A_MAX_STAGED_RELEASES 64

typedef struct {
    uint32_t sem_dma;           /* instance of the semaphore DMA object */
    bool sem_dma_valid;
    uint32_t sem_offset;
    uint32_t releases;
    struct { uint32_t phys, value; } release[NV2A_MAX_STAGED_RELEASES];
    uint32_t surface;           /* NV_PGRAPH_SURFACE: the flip counters */
    uint32_t param_a, param_b;  /* the two registers a software method reads */
    uint32_t trap_subchannel, trap_param;
} ActionStage;

/* Host address and value of the last semaphore release, for the fence-mirror
 * overlap check below. Written by the walk, read by the mirror's thread. */
static volatile uintptr_t g_semaphore_last_host;
static volatile uint32_t g_semaphore_last_value;
static volatile LONG g_fence_mirror_overlaps;

static void action_stage_begin(NV2AState *d, ActionStage *st)
{
    st->sem_dma = d->pgraph.dma_semaphore;
    st->sem_dma_valid = d->pgraph.dma_semaphore_valid;
    st->sem_offset = d->pgraph.regs[NV_PGRAPH_SEMAPHOREOFFSET];
    st->releases = 0;
    st->surface = d->pgraph.regs[NV_PGRAPH_SURFACE];
    st->param_a = d->pgraph.regs[NV_PGRAPH_ZSTENCILCLEARVALUE];
    st->param_b = d->pgraph.regs[NV_PGRAPH_COLORCLEARVALUE];
}

/* One step of a 3-bit flip counter: wrap to 0 on reaching the modulo. */
static uint32_t flip_counter_next(uint32_t value, uint32_t modulo)
{
    return value + 1u == modulo ? 0u : (value + 1u) & 7u;
}

/* FLIP_STALL holds the walk until the display has read past the buffer the
 * walk is about to write: it may continue once READ_3D differs from WRITE_3D. */
static bool flip_stall_complete(uint32_t surface)
{
    return GET_MASK(surface, NV_PGRAPH_SURFACE_READ_3D) !=
           GET_MASK(surface, NV_PGRAPH_SURFACE_WRITE_3D);
}

/* A semaphore lands in the physical window the pushbuffer is read from: a
 * guest physical address, never aliased into canonical RAM. */
static bool window_word_writable(NV2AState *d, uint32_t phys)
{
    if (!d->pfifo.pushbuffer || (phys & 3u) || phys < d->pfifo.pushbuffer_base ||
        phys - d->pfifo.pushbuffer_base > d->pfifo.pushbuffer_size - 4u)
        return false;
#if defined(_WIN32)
    {
        MEMORY_BASIC_INFORMATION mbi;
        uint8_t *host = d->pfifo.pushbuffer + (phys - d->pfifo.pushbuffer_base);
        DWORD protect;
        if (VirtualQuery(host, &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        protect = mbi.Protect & 0xffu;
        if (protect != PAGE_READWRITE && protect != PAGE_WRITECOPY &&
            protect != PAGE_EXECUTE_READWRITE && protect != PAGE_EXECUTE_WRITECOPY)
            return false;
        if (host + 4 > (uint8_t *)mbi.BaseAddress + mbi.RegionSize)
            return false;
    }
#endif
    return true;
}

/* BACK_END_WRITE_SEMAPHORE_RELEASE writes its parameter at the semaphore DMA
 * object's base plus SET_SEMAPHORE_OFFSET. The object's limit is the last byte
 * it covers, so the whole dword has to fit under it. */
static bool semaphore_target(NV2AState *d, const ActionStage *st, uint32_t *phys)
{
    DMAObject dma;
    if (!st->sem_dma_valid || !d->ramin_ptr || d->ramin.size < 16 ||
        st->sem_dma > d->ramin.size - 16)
        return false;
    dma = nv_dma_load(d, st->sem_dma);
    if ((st->sem_offset & 3u) || dma.limit < 3 || st->sem_offset > dma.limit - 3 ||
        dma.address > UINT32_MAX - st->sem_offset)
        return false;
    *phys = (uint32_t)dma.address + st->sem_offset;
    return window_word_writable(d, *phys);
}

/* Stage one NV097 method's effect. Returns NV2A_SUBMIT_OK; the diagnostic
 * that rejects the whole stream; or NV2A_SUBMIT_SOFTWARE_METHOD /
 * NV2A_SUBMIT_FLIP_STALL, which commit this method and hold the walk after it. */
static uint32_t action_method(NV2AState *d, ActionStage *st, uint32_t subchannel,
                              uint32_t method, uint32_t param)
{
    uint32_t phys;
    switch (method) {
    case NV097_NO_OPERATION:
        /* A non-zero NOP on Kelvin is a data error when PGRAPH data checking
         * is on, which is how D3D calls its software methods. */
        if (param == 0)
            break;
        if (!(d->pgraph.regs[NV_PGRAPH_DEBUG_3] & NV_PGRAPH_DEBUG_3_DATA_CHECK))
            return NV2A_SUBMIT_NOP_UNCHECKED;
        st->trap_subchannel = subchannel;
        st->trap_param = param;
        return NV2A_SUBMIT_SOFTWARE_METHOD;
    case NV097_SET_FLIP_READ:
        SET_MASK(st->surface, NV_PGRAPH_SURFACE_READ_3D, param);
        break;
    case NV097_SET_FLIP_WRITE:
        SET_MASK(st->surface, NV_PGRAPH_SURFACE_WRITE_3D, param);
        break;
    case NV097_SET_FLIP_MODULO:
        SET_MASK(st->surface, NV_PGRAPH_SURFACE_MODULO_3D, param);
        break;
    case NV097_FLIP_INCREMENT_WRITE: {
        /* Computed first: SET_MASK clears the field before it reads `val`. */
        uint32_t next = flip_counter_next(GET_MASK(st->surface, NV_PGRAPH_SURFACE_WRITE_3D),
                                          GET_MASK(st->surface, NV_PGRAPH_SURFACE_MODULO_3D));
        SET_MASK(st->surface, NV_PGRAPH_SURFACE_WRITE_3D, next);
        break;
    }
    case NV097_FLIP_STALL:
        if (!flip_stall_complete(st->surface))
            return NV2A_SUBMIT_FLIP_STALL;
        break;
    case NV097_SET_ZSTENCIL_CLEAR_VALUE:
        st->param_a = param;
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        st->param_b = param;
        break;
    case NV097_SET_CONTEXT_DMA_SEMAPHORE:
        /* Methods 0x180..0x1FC take a handle; the puller resolves it through
         * RAMHT and PGRAPH receives the instance. */
        if (!ramht_lookup_instance(d, param, &st->sem_dma))
            return NV2A_SUBMIT_INVALID_HANDLE;
        st->sem_dma_valid = true;
        break;
    case NV097_SET_SEMAPHORE_OFFSET:
        st->sem_offset = param;
        break;
    case NV097_BACK_END_WRITE_SEMAPHORE_RELEASE:
        if (st->releases >= NV2A_MAX_STAGED_RELEASES || !semaphore_target(d, st, &phys))
            return NV2A_SUBMIT_SEMAPHORE_FAULT;
        st->release[st->releases].phys = phys;
        st->release[st->releases].value = param;
        ++st->releases;
        break;
    default:
        break;
    }
    return NV2A_SUBMIT_OK;
}

static void action_commit(NV2AState *d, const ActionStage *st)
{
    static unsigned logged;
    d->pgraph.dma_semaphore = st->sem_dma;
    d->pgraph.dma_semaphore_valid = st->sem_dma_valid;
    d->pgraph.regs[NV_PGRAPH_SEMAPHOREOFFSET] = st->sem_offset;
    d->pgraph.regs[NV_PGRAPH_SURFACE] = st->surface;
    d->pgraph.regs[NV_PGRAPH_ZSTENCILCLEARVALUE] = st->param_a;
    d->pgraph.regs[NV_PGRAPH_COLORCLEARVALUE] = st->param_b;
    for (uint32_t i = 0; i < st->releases; ++i) {
        uint8_t *host = d->pfifo.pushbuffer +
                        (st->release[i].phys - d->pfifo.pushbuffer_base);
        memcpy(host, &st->release[i].value, 4);
        g_semaphore_last_host = (uintptr_t)host;
        g_semaphore_last_value = st->release[i].value;
        ++d->pfifo.semaphore_releases;
        if (logged < 8) {
            ++logged;
            fprintf(stderr, "  [NV2A] semaphore release %08X -> phys %08X\n",
                    st->release[i].value, st->release[i].phys);
        }
    }
}

/* The trap a non-zero NOP raises: TRAPPED_ADDR/DATA name the method, NSOURCE
 * says why, the ERROR interrupt is raised and PGRAPH FIFO access is switched
 * off. The guest's handler acknowledges by clearing the interrupt and turning
 * access back on; the walk resumes after the NOP only when both are done. */
static void action_raise_trap(NV2AState *d, const ActionStage *st)
{
    uint32_t chid = GET_MASK(d->pgraph.regs[NV_PGRAPH_CTX_USER], NV_PGRAPH_CTX_USER_CHID);
    uint32_t addr = 0;
    SET_MASK(addr, NV_PGRAPH_TRAPPED_ADDR_MTHD, NV097_NO_OPERATION);
    SET_MASK(addr, NV_PGRAPH_TRAPPED_ADDR_SUBCH, st->trap_subchannel);
    SET_MASK(addr, NV_PGRAPH_TRAPPED_ADDR_CHID, chid);
    d->pgraph.regs[NV_PGRAPH_TRAPPED_ADDR] = addr;
    d->pgraph.regs[NV_PGRAPH_TRAPPED_DATA_LOW] = st->trap_param;
    d->pgraph.regs[NV_PGRAPH_NSOURCE] |= NV_PGRAPH_NSOURCE_DATA_ERROR;
    d->pgraph.pending_interrupts |= NV_PGRAPH_INTR_ERROR;
    d->pgraph.regs[NV_PGRAPH_INTR] |= NV_PGRAPH_INTR_ERROR;
    d->pgraph.regs[NV_PGRAPH_FIFO] &= ~(uint32_t)NV_PGRAPH_FIFO_ACCESS;
    ++d->pfifo.software_method_traps;
}

/* Whether a held walk may continue. Called with the pfifo lock held. */
static bool walk_hold_released(NV2AState *d)
{
    switch (d->pfifo.hold) {
    case NV2A_HOLD_SOFTWARE_METHOD:
        return !(d->pgraph.pending_interrupts & NV_PGRAPH_INTR_ERROR) &&
               (d->pgraph.regs[NV_PGRAPH_FIFO] & NV_PGRAPH_FIFO_ACCESS);
    case NV2A_HOLD_FLIP_STALL:
        return flip_stall_complete(d->pgraph.regs[NV_PGRAPH_SURFACE]);
    default:
        return true;
    }
}

/* One emitter for BOTH budget exits, and a transcript that survives the log cap.
 *
 * The walk can exhaust its budget in two places: at a packet HEADER (the
 * word/packet caps checked at the top of the loop) and inside a packet's
 * PARAMETERS (the word cap checked in the `count` branch). Only the header path
 * used to print anything, so a parameter-heavy stream -- the case that actually
 * happened, and the case that matters -- exhausted the budget silently. That is
 * why the earlier investigation could only infer "inside a packet" from the
 * ABSENCE of a dump.
 *
 * This latches the first stop into NV2AState, so it survives the 64-line submit
 * cap and any stderr filtering, and prints the same shape from both paths. The
 * trajectory records the words ACTUALLY consumed in order; the old
 * `trace[words & 31]` array was written only at headers and indexed by total
 * words, so on a parameter-heavy stream its slots were sparse, stale and out of
 * chronological order -- it could not distinguish a long valid stream from a
 * cyclic walk.
 *
 * `local_pc` is the address being consumed at the limit. It is NOT
 * submit_diag_get: that is the rollback origin, where the stream is retried
 * from, and reporting it as the failure point is a known trap. */
static void submit_budget_stop(NV2AState *d, uint32_t get, uint32_t put,
                               uint32_t begin, uint32_t end, uint32_t pc,
                               uint32_t words, uint32_t packets,
                               uint32_t count, uint32_t method, uint32_t ret,
                               uint32_t local_pc, bool in_param,
                               bool at_packet_limit)
{
    bool first = d->pfifo.budget_stops == 0;
    ++d->pfifo.budget_stops;
    if (!first) return;                       /* latch the FIRST stop only */

    d->pfifo.budget_local_pc = local_pc;
    d->pfifo.budget_words = words;
    d->pfifo.budget_packets = packets;
    d->pfifo.budget_count = count;
    d->pfifo.budget_method = method;
    d->pfifo.budget_ret = ret;
    d->pfifo.budget_get = get;
    d->pfifo.budget_put = put;
    d->pfifo.budget_in_param = in_param;
    d->pfifo.budget_at_packet_limit = at_packet_limit;

    fprintf(stderr, "  [PFIFO] budget_exhausted get=%08X put=%08X begin=%08X"
            " end=%08X words=%u packets=%u pc=%08X local_pc=%08X%s\n",
            get, put, begin, end, words, packets, pc, local_pc,
            at_packet_limit ? " LIMIT=packets(1024)" : " LIMIT=words(4096)");
    fprintf(stderr, "          %s; count=%u method=%04X ret=%08X\n",
            in_param ? "stopped INSIDE a packet's parameters"
                     : "stopped at a packet header",
            count, method, ret);
    if (count) {
        fprintf(stderr, "          the straddling packet: method=%04X count=%u"
                " (%u parameter word(s) still to read)\n", method, count, count);
    }
    fprintf(stderr, "          last %u consumed words (address:word):",
            d->pfifo.budget_trace_count);
    for (uint32_t i = 0; i < d->pfifo.budget_trace_count; ++i) {
        if (i % 4u == 0u) fprintf(stderr, "\n            ");
        fprintf(stderr, "%08X:%08X ", d->pfifo.budget_trace_va[i],
                d->pfifo.budget_trace_word[i]);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* Record one consumed word in the rolling trajectory. Called on EVERY word the
 * walk consumes (header and parameter), so the trace is chronological and
 * dense -- which is what makes it usable as a cyclic-walk discriminator. */
static void submit_trace_word(NV2AState *d, uint32_t va, uint32_t word)
{
    uint32_t i = d->pfifo.budget_trace_count;
    if (i < 64) {
        d->pfifo.budget_trace_va[i] = va;
        d->pfifo.budget_trace_word[i] = word;
        d->pfifo.budget_trace_count = i + 1;
        return;
    }
    /* Full: shift down by one so the newest 64 are always present. */
    memmove(&d->pfifo.budget_trace_va[0], &d->pfifo.budget_trace_va[1],
            63 * sizeof(d->pfifo.budget_trace_va[0]));
    memmove(&d->pfifo.budget_trace_word[0], &d->pfifo.budget_trace_word[1],
            63 * sizeof(d->pfifo.budget_trace_word[0]));
    d->pfifo.budget_trace_va[63] = va;
    d->pfifo.budget_trace_word[63] = word;
}

/* The fence mirror (xbox_memory_layout.c) writes the word D3D's fence wait
 * polls, which in JSRF is the word the semaphore release targets. Two writers
 * of one word would fight without either noticing, so the mirror reports each
 * write it makes and an overlap with the last release is logged. */
void nv2a_note_fence_mirror_write(const volatile void *host, uint32_t value)
{
    LONG n;
    if (!host || !nv2a_actions_enabled() || (uintptr_t)host != g_semaphore_last_host)
        return;
    n = InterlockedIncrement(&g_fence_mirror_overlaps);
    if (n <= 16 || n % 1000 == 0)
        fprintf(stderr, "  [NV2A] fence mirror writes %08X over the semaphore word"
                " at %p (last release %08X; overlap %ld)\n",
                value, (const void *)host, g_semaphore_last_value, (long)n);
}

uint32_t nv2a_fence_mirror_overlaps(void)
{
    return (uint32_t)g_fence_mirror_overlaps;
}

bool nv2a_submit_pending(NV2AState *d)
{
    uint32_t get, put, pc, ret = 0, words = 0, packets = 0;
    uint32_t seen[1024]; unsigned seen_count = 0;
    uint32_t staged_count = 0;
    uint32_t staged_class[8], staged_object[8];
    bool ok = true;
    bool actions = nv2a_actions_enabled();
    uint32_t stop = NV2A_SUBMIT_OK;
    bool admit = nv2a_admit_unknown_enabled();
    AdmitRecord admitted[NV2A_ADMIT_PENDING];
    uint32_t admitted_count = 0, admitted_total = 0;
    ActionStage st;
    if (!d) return false;
    qemu_mutex_lock(&d->pfifo.lock);
    if (actions) {
        if (!walk_hold_released(d)) {
            d->pfifo.submit_diag = d->pfifo.hold == NV2A_HOLD_FLIP_STALL ?
                NV2A_SUBMIT_HELD_FLIP_STALL : NV2A_SUBMIT_HELD_SOFTWARE_METHOD;
            qemu_mutex_unlock(&d->pfifo.lock);
            return false;
        }
        d->pfifo.hold = NV2A_HOLD_NONE;
        action_stage_begin(d, &st);
    }
    /* sink_count is cleared per submission, because the sink records the methods
     * this submission walked. Its payload is written for diagnostics and tests
     * and has no production reader; sink_count is the diagnostic/test seam. The
     * staging area and the sink both cover the walk's whole word budget
     * (NV2A_SUBMIT_MAX_WORDS), and the submission is still committed
     * all-or-nothing. */
    d->pfifo.sink_count = 0;
    /* The budget transcript describes ONE walk. budget_stops is deliberately NOT
     * cleared here: it latches the FIRST stop across the run, which is the event
     * of interest, so a later retry cannot overwrite it. The trace is only
     * cleared while no stop has been latched, so the latched transcript survives
     * the retries that follow a rejection -- otherwise an external reader (a
     * test, or the run report) would find the fields already wiped. */
    if (d->pfifo.budget_stops == 0) d->pfifo.budget_trace_count = 0;
    memcpy(staged_class, d->pfifo.binding_class, sizeof(staged_class));
    memcpy(staged_object, d->pfifo.binding_object, sizeof(staged_object));
    get = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    put = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];
    if (!d->pfifo.pushbuffer) { d->pfifo.submit_diag = NV2A_SUBMIT_UNMAPPED; ok = false; goto done; }
    uint32_t begin = d->pfifo.pushbuffer_base;
    uint32_t end = begin + d->pfifo.pushbuffer_size;
    if ((get & 3u) || (put & 3u) || get < begin || put < begin ||
        get > end || put > end) {
        d->pfifo.submit_diag = NV2A_SUBMIT_BAD_POINTER;
        ok = false;
        goto done;
    }
    if (get == end) get = begin;
    if (put == end) put = begin;
    pc = get;
    /* The packet being walked. A walk normally starts at a header; after a
     * hold it resumes inside the packet the hold interrupted. */
    uint32_t count = 0, method = 0, subchannel = 0, address = pc;
    bool non_inc = false;
    if (actions && (d->pfifo.carry_count || d->pfifo.carry_ret)) {
        count = d->pfifo.carry_count;
        method = d->pfifo.carry_method;
        subchannel = d->pfifo.carry_subchannel;
        non_inc = d->pfifo.carry_non_inc;
        ret = d->pfifo.carry_ret;
        if (count > NV2A_SUBMIT_MAX_WORDS) { d->pfifo.submit_diag = NV2A_SUBMIT_SINK_FULL; ok = false; goto done; }
    }
    while (pc != put || count) {
        uint32_t h;
        if (count) {
            uint32_t param;
            if (words >= NV2A_SUBMIT_MAX_WORDS) {
                d->pfifo.submit_diag = NV2A_SUBMIT_BUDGET; ok = false;
                submit_budget_stop(d, get, put, begin, end, pc, words, packets,
                                   count, method, ret, pc, true, false);
                goto done;
            }
            if (pc == put || !submit_read_word(d, pc, &param)) { d->pfifo.submit_diag = pc == put ? NV2A_SUBMIT_TRUNCATED : NV2A_SUBMIT_UNREADABLE; ok = false; goto done; }
            submit_trace_word(d, pc, param);
            pc = submit_advance(d, pc); ++words;
            /* Production SET_OBJECT walks RAMHT in claimed PRAMIN.
             * The fixture seam stays opt-in for isolated 11b4b2 tests. */
            if (method == M_SET_OBJECT) {
                uint32_t class_id = 0;
                bool bound = false;
                if (d->pfifo.fixture_execution) {
                    if (param &&
                        d->pfifo.fixture_object[subchannel] == param &&
                        d->pfifo.fixture_class[subchannel] == NV097_CLASS) {
                        class_id = NV097_CLASS;
                        bound = true;
                    }
                    if (!bound) {
                        d->pfifo.submit_diag = NV2A_SUBMIT_UNSUPPORTED_METHOD;
                        d->pfifo.submit_diag_get = address;
                        d->pfifo.submit_diag_subchannel = subchannel;
                        d->pfifo.submit_diag_method = method;
                        d->pfifo.submit_diag_param = param;
                        ok = false; goto done;
                    }
                } else if (!ramht_lookup_class(d, param, &class_id)) {
                    d->pfifo.submit_diag = NV2A_SUBMIT_INVALID_HANDLE;
                    d->pfifo.submit_diag_get = address;
                    d->pfifo.submit_diag_subchannel = subchannel;
                    d->pfifo.submit_diag_method = method;
                    d->pfifo.submit_diag_param = param;
                    ok = false; goto done;
                }
                staged_class[subchannel] = class_id;
                staged_object[subchannel] = param;
            } else if (method != 0x0100u &&
                       !nv2a_method_implemented(staged_class[subchannel], method) &&
                       !(admit && (staged_class[subchannel] == NV097_CLASS ||
                                   staged_class[subchannel] == NV_MEMCPY_CLASS ||
                                   staged_class[subchannel] == NV_SURFACES2D_CLASS ||
                                   staged_class[subchannel] == NV_IMAGEBLIT_CLASS))) {
                d->pfifo.submit_diag = NV2A_SUBMIT_UNSUPPORTED_METHOD;
                d->pfifo.submit_diag_get = address;
                d->pfifo.submit_diag_subchannel = subchannel;
                d->pfifo.submit_diag_method = method;
                d->pfifo.submit_diag_param = param;
                ok = false;
                goto done;
            }
            if (method != M_SET_OBJECT && method != 0x0100u &&
                !nv2a_method_implemented(staged_class[subchannel], method)) {
                /* Admitted by the switch: staged like an implemented method. */
                ++admitted_total;
                if (admitted_count < NV2A_ADMIT_PENDING) {
                    admitted[admitted_count].class_id = staged_class[subchannel];
                    admitted[admitted_count].method = method;
                    admitted[admitted_count].param = param;
                    admitted[admitted_count].at = address;
                    ++admitted_count;
                }
            }
            if (actions && staged_class[subchannel] == NV097_CLASS) {
                uint32_t code = action_method(d, &st, subchannel, method, param);
                if (code == NV2A_SUBMIT_SOFTWARE_METHOD || code == NV2A_SUBMIT_FLIP_STALL) {
                    stop = code;
                } else if (code != NV2A_SUBMIT_OK) {
                    d->pfifo.submit_diag = code;
                    d->pfifo.submit_diag_get = address;
                    d->pfifo.submit_diag_subchannel = subchannel;
                    d->pfifo.submit_diag_method = method;
                    d->pfifo.submit_diag_param = param;
                    ok = false;
                    goto done;
                }
            }
            d->pfifo.staged[staged_count].subchannel = subchannel;
            d->pfifo.staged[staged_count].class_id = staged_class[subchannel];
            d->pfifo.staged[staged_count].method = method;
            d->pfifo.staged[staged_count].param = param;
            ++staged_count;
            --count;
            if (!non_inc) method += 4;
            if (stop != NV2A_SUBMIT_OK) break;
            continue;
        }
        address = pc;
        if (words >= NV2A_SUBMIT_MAX_WORDS || packets >= 1024) {
            d->pfifo.submit_diag = NV2A_SUBMIT_BUDGET; ok = false;
            submit_budget_stop(d, get, put, begin, end, pc, words, packets,
                               count, method, ret, address, false,
                               packets >= 1024);
            break;
        }
        for (unsigned i = 0; i < seen_count; ++i) if (seen[i * 2] == pc && seen[i * 2 + 1] == ret) { d->pfifo.submit_diag = NV2A_SUBMIT_LOOP; ok = false; goto done; }
        if (seen_count < 512) { seen[seen_count * 2] = pc; seen[seen_count * 2 + 1] = ret; ++seen_count; }
        if (!submit_read_word(d, pc, &h)) { d->pfifo.submit_diag = NV2A_SUBMIT_UNREADABLE; ok = false; break; }
        submit_trace_word(d, pc, h);
        pc = submit_advance(d, pc); ++words; ++packets;
        if ((h & 0xe0000003u) == 0x20000000u || (h & 3u) == 1u || (h & 3u) == 2u || h == 0x00020000u) {
            uint32_t target;
            if ((h & 3u) == 2u) { if (ret) { d->pfifo.submit_diag = NV2A_SUBMIT_LOOP; ok = false; break; } ret = pc; }
            if (h == 0x00020000u) { if (!ret) { d->pfifo.submit_diag = NV2A_SUBMIT_BAD_TARGET; ok = false; break; } pc = ret; ret = 0; continue; }
            target = (h & 3u) == 1u || (h & 3u) == 2u ? h & 0xfffffffcu : h & 0x1fffffffu;
            if (target < d->pfifo.pushbuffer_base || target >= d->pfifo.pushbuffer_base + d->pfifo.pushbuffer_size || (target & 3u)) {
                d->pfifo.submit_diag = NV2A_SUBMIT_BAD_TARGET; ok = false; break;
            }
            pc = target;
            continue;
        }
        if ((h & 0xe0030003u) == 0u || (h & 0xe0030003u) == 0x40000000u) {
            count = (h >> 18) & 0x7ffu; method = h & 0x1ffcu; subchannel = (h >> 13) & 7u;
            non_inc = (h & 0x40000000u) != 0;
            if (!non_inc && count && method + 4u * (count - 1u) > 0x1ffcu) { d->pfifo.submit_diag = NV2A_SUBMIT_METHOD_RANGE; ok = false; break; }
            if (d->pfifo.sink_count + staged_count + count > NV2A_SUBMIT_MAX_WORDS) { d->pfifo.submit_diag = NV2A_SUBMIT_SINK_FULL; ok = false; break; }
            continue;
        }
        d->pfifo.submit_diag = NV2A_SUBMIT_RESERVED; d->pfifo.submit_diag_get = address; ok = false; break;
    }
    if (ok) {
        for (uint32_t i = 0; i < staged_count; ++i) {
            d->pfifo.sink[d->pfifo.sink_count].subchannel = d->pfifo.staged[i].subchannel;
            d->pfifo.sink[d->pfifo.sink_count].class_id = d->pfifo.staged[i].class_id;
            d->pfifo.sink[d->pfifo.sink_count].method = d->pfifo.staged[i].method;
            d->pfifo.sink[d->pfifo.sink_count].param = d->pfifo.staged[i].param;
            ++d->pfifo.sink_count;
            /* Capture the parameter as method state. For the register-setting
             * methods -- which is most of the NV097 pipeline, and all of the
             * surface, blit and memcpy state -- this IS the implementation: the
             * value is where a renderer reads it from. Methods that trigger an
             * action rather than set state (blit, notify, flip) are captured here
             * too and need their own handling on top; the notify one is what
             * JSRF's ring-space wait at 0x001914F0 is waiting on.
             *
             * Only NV097 has method state in this model, so only NV097 is
             * captured this way; the other classes' parameters are recorded in
             * the sink, which carries the class per entry. The class comes from
             * the staging entry, not from binding_class[], so a mid-stream
             * SET_OBJECT rebind cannot retro-label the methods before it. */
            if (d->pfifo.staged[i].class_id == NV097_CLASS
                    && d->pfifo.staged[i].method / 4 < NV2A_PGRAPH_METHOD_WORDS) {
                d->pgraph.methods[d->pfifo.staged[i].method / 4] = d->pfifo.staged[i].param;
            }
        }
        memcpy(d->pfifo.binding_class, staged_class, sizeof(staged_class));
        memcpy(d->pfifo.binding_object, staged_object, sizeof(staged_object));
        if (actions) action_commit(d, &st);
        /* Hand the committed methods to the registered consumer, in order, only
         * now: after the whole submission succeeded (a rejected walk never
         * reaches here, so it has no executor side effect) and after
         * action_commit, so a committed semaphore release or surface is already
         * visible to it. Runs under the PFIFO lock, so the consumer must be
         * lock-free -- see the contract on nv2a_set_commit_consumer. */
        if (g_commit_consumer) {
            for (uint32_t i = 0; i < staged_count; ++i) {
                g_commit_consumer(d->pfifo.staged[i].subchannel,
                                  d->pfifo.staged[i].class_id,
                                  d->pfifo.staged[i].method,
                                  d->pfifo.staged[i].param);
            }
        }
        if (staged_count) {
            d->pfifo.submit_last_method = d->pfifo.staged[staged_count - 1].method;
            d->pfifo.submit_last_param = d->pfifo.staged[staged_count - 1].param;
        }
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] = pc;
        pfifo_trace("submit_commit", NV_PFIFO_CACHE1_DMA_GET, pc);
        ++d->pfifo.submit_successes;
        d->pfifo.submit_diag = NV2A_SUBMIT_OK;
        g_nv2a_submit_state.admitted_unknown += admitted_total;
        for (uint32_t i = 0; i < admitted_count; ++i) {
            uint32_t j;
            for (j = 0; j < g_admit_seen_count; ++j)
                if (g_admit_seen[j].class_id == admitted[i].class_id &&
                    g_admit_seen[j].method == admitted[i].method) break;
            if (j < g_admit_seen_count || g_admit_seen_count >= NV2A_ADMIT_LOG_MAX ||
                g_admit_pending_count >= NV2A_ADMIT_PENDING)
                continue;
            g_admit_seen[g_admit_seen_count++] = admitted[i];
            g_admit_pending[g_admit_pending_count++] = admitted[i];
        }
        if (actions) {
            /* A hold keeps the rest of the interrupted packet, and the
             * subroutine return, for the walk that resumes after it. */
            bool held = stop != NV2A_SUBMIT_OK;
            d->pfifo.carry_count = held ? count : 0;
            d->pfifo.carry_method = held ? method : 0;
            d->pfifo.carry_subchannel = held ? subchannel : 0;
            d->pfifo.carry_non_inc = held && non_inc;
            d->pfifo.carry_ret = held ? ret : 0;
            if (stop == NV2A_SUBMIT_SOFTWARE_METHOD) {
                action_raise_trap(d, &st);
                d->pfifo.hold = NV2A_HOLD_SOFTWARE_METHOD;
            } else if (stop == NV2A_SUBMIT_FLIP_STALL) {
                d->pfifo.hold = NV2A_HOLD_FLIP_STALL;
            }
            if (held) {
                d->pfifo.submit_diag = stop;
                d->pfifo.submit_diag_subchannel = d->pfifo.staged[staged_count - 1].subchannel;
                d->pfifo.submit_diag_method = d->pfifo.staged[staged_count - 1].method;
                d->pfifo.submit_diag_param = d->pfifo.staged[staged_count - 1].param;
            }
        }
    }
done:
    d->pfifo.submit_words += words;
    d->pfifo.submit_packets += packets;
    d->pfifo.submit_diag_get = (d->pfifo.submit_diag == NV2A_SUBMIT_OK) ? pc : d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    {
        /* Published once per walk under the generation protocol, so a reader
         * never pairs fields from two walks. */
        NV2ASubmitState *ss = &g_nv2a_submit_state;
        InterlockedIncrement(&ss->generation);
        ss->diag = d->pfifo.submit_diag;
        ss->method = d->pfifo.submit_diag_method;
        ss->subchannel = d->pfifo.submit_diag_subchannel;
        ss->param = d->pfifo.submit_diag_param;
        ss->at = d->pfifo.submit_diag_get;
        /* The latched FIRST budget stop, republished every walk so a dump taken
         * at any later time still sees it. It is never cleared by a retry. */
        ss->budget_stops = d->pfifo.budget_stops;
        ss->budget_local_pc = d->pfifo.budget_local_pc;
        ss->budget_words = d->pfifo.budget_words;
        ss->budget_packets = d->pfifo.budget_packets;
        ss->budget_count = d->pfifo.budget_count;
        ss->budget_method = d->pfifo.budget_method;
        ss->budget_ret = d->pfifo.budget_ret;
        ss->budget_in_param = d->pfifo.budget_in_param ? 1u : 0u;
        ss->budget_at_packet_limit = d->pfifo.budget_at_packet_limit ? 1u : 0u;
        ss->get = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
        ss->put = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];
        if (ok) {
            ++ss->successes;
            ss->consecutive_rejections = 0;
        } else {
            ++ss->rejections;
            ++ss->consecutive_rejections;
        }
        /* The interlocked increment is a full barrier: the fields land first. */
        InterlockedIncrement(&ss->generation);
    }
    qemu_mutex_unlock(&d->pfifo.lock);
    /* Outside the PFIFO lock. A walk that stopped at a hold has not consumed
     * the kick yet; the walk that resumes it to PUT reports the commit. */
    if (ok && stop == NV2A_SUBMIT_OK)
        kick_observer_notify(NV2A_COMMIT);
    if (ok && stop == NV2A_SUBMIT_SOFTWARE_METHOD)
        nv2a_update_irq(d);
    return ok;
}

static uint64_t user_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    if (size != 4) return 0;
    if (addr == NV_USER_DMA_PUT) return d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];
    if (addr == NV_USER_DMA_GET) return d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    return 0;
}

/* Log the walk that just finished: rejection edges and diag changes, sparse
 * repeat counts, recovery, and newly admitted unknown methods. */
static void nv2a_log_submit_result(NV2AState *d)
{
    NV2ASubmitState st;
    AdmitRecord pend[NV2A_ADMIT_PENDING];
    uint32_t pend_count, prev;
    bool banner = false;

    qemu_mutex_lock(&d->pfifo.lock);
    st = g_nv2a_submit_state;
    pend_count = g_admit_pending_count;
    memcpy(pend, g_admit_pending, sizeof(pend[0]) * pend_count);
    g_admit_pending_count = 0;
    if (pend_count && !g_admit_banner) g_admit_banner = banner = true;
    prev = g_logged_consecutive;
    g_logged_consecutive = st.consecutive_rejections;
    qemu_mutex_unlock(&d->pfifo.lock);

    if (st.consecutive_rejections) {
        bool edge = st.consecutive_rejections == 1 || st.diag != g_last_reject_diag;
        uint32_t n = st.consecutive_rejections;
        if (edge) {
            if (g_reject_lines < 256) {
                ++g_reject_lines;
                fprintf(stderr, "  [PFIFO] reject diag=%s method=%04X subch=%u param=%08X"
                        " at=%08X get=%08X put=%08X successes=%u rejections=%u\n",
                        nv2a_submit_diagnostic(st.diag), st.method, st.subchannel,
                        st.param, st.at, st.get, st.put, st.successes, st.rejections);
                fflush(stderr);
            }
        } else if (n == 16 || n == 256 || n == 4096 || n == 65536) {
            fprintf(stderr, "  [PFIFO] still rejecting n=%u diag=%s method=%04X subch=%u"
                    " param=%08X at=%08X get=%08X put=%08X\n",
                    n, nv2a_submit_diagnostic(st.diag), st.method, st.subchannel,
                    st.param, st.at, st.get, st.put);
            fflush(stderr);
        }
        g_last_reject_diag = st.diag;
    } else if (prev) {
        fprintf(stderr, "  [PFIFO] recovered after %u rejections get=%08X put=%08X"
                " successes=%u\n", prev, st.get, st.put, st.successes);
        fflush(stderr);
        g_last_reject_diag = 0;
    }
    if (banner) {
        fprintf(stderr, "[NV2A] RECOMP_NV2A_ADMIT_UNKNOWN=1: unknown methods on known"
                " classes are captured as state, not executed (exploratory; ledger L44)\n");
        fflush(stderr);
    }
    for (uint32_t i = 0; i < pend_count; ++i) {
        fprintf(stderr, "  [PFIFO] admit-unknown class=%02X method=%04X param=%08X at=%08X\n",
                pend[i].class_id, pend[i].method, pend[i].param, pend[i].at);
        fflush(stderr);
    }
}

/* Log a walk triggered by a PUT write, or retried in its place. */
static void log_kicked_walk(NV2AState *d)
{
    /* Per-submit diagnostic. GET only moves on success, so when it stays put
     * the reason is here and nowhere else -- the walk's own diag, plus the
     * exact packet that stopped it. Kept in the model rather than re-added
     * per investigation, because "why did the ring not drain" is the question
     * this model gets asked most often. */
    static unsigned long submits;
    if (submits < 64)
        fprintf(stderr, "  [PFIFO] submit #%lu diag=%s get=%08X put=%08X"
                " method=%03X subch=%u param=%08X at=%08X\n",
                submits, nv2a_submit_diagnostic(d->pfifo.submit_diag),
                d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET],
                d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT],
                d->pfifo.submit_diag_method, d->pfifo.submit_diag_subchannel,
                d->pfifo.submit_diag_param, d->pfifo.submit_diag_get);
    submits++;
    nv2a_log_submit_result(d);
}

static void user_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    if (size != 4) return;
    /* NV_USER_DMA_GET/PUT are aliases of the PFIFO DMA pointers, so both the
     * read and write paths must use the PFIFO register slots.  Writing the
     * USER-local slot instead would make a kick invisible to user_read and
     * to nv2a_submit_pending, which read the PFIFO slots. */
    if (addr == NV_USER_DMA_GET) {
        pfifo_trace("user_write", NV_PFIFO_CACHE1_DMA_GET, (uint32_t)val);
        qemu_mutex_lock(&d->pfifo.lock);
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] = (uint32_t)val;
        qemu_mutex_unlock(&d->pfifo.lock);
    } else if (addr == NV_USER_DMA_PUT) {
        /* Stored as written, as xemu does: bit 16 is an ordinary offset bit
         * once the ring passes 64 KB.  JSRF's set-bit-16-and-spin at
         * 0x00191270 targets 0x100410, NV_PFB_WBC, which pfb_read answers. */
        pfifo_trace("user_write", NV_PFIFO_CACHE1_DMA_PUT, (uint32_t)val);
        qemu_mutex_lock(&d->pfifo.lock);
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] = (uint32_t)val;
        qemu_mutex_unlock(&d->pfifo.lock);
        kick_observer_notify(NV2A_KICK);
        nv2a_submit_pending(d);
        log_kicked_walk(d);
    }
}

bool nv2a_retry_stalled_walk(NV2AState *d)
{
    uint32_t stalled;
    bool committed;
    if (!d) return false;
    qemu_mutex_lock(&d->pfifo.lock);
    stalled = g_nv2a_submit_state.consecutive_rejections;
    qemu_mutex_unlock(&d->pfifo.lock);
    if (!stalled) return false;
    committed = nv2a_submit_pending(d);
    log_kicked_walk(d);
    return committed;
}

uint64_t pfifo_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PFIFO_INTR_0:
        r = d->pfifo.pending_interrupts;
        break;
    case NV_PFIFO_INTR_EN_0:
        r = d->pfifo.enabled_interrupts;
        break;
    case NV_PFIFO_CACHE1_STATUS:
        /* The model consumes a submission synchronously inside
         * nv2a_submit_pending, so cache1 is always drained and therefore always
         * below the low watermark. Reporting the low mark is what "drained"
         * means to a guest that polls it.
         *
         * JSRF spins on exactly this: 0x00194A72 loops at 0x194A78 and only
         * leaves at 0x194ABF when CACHE1_STATUS bit 4 AND RUNOUT_STATUS bit 4
         * are set and CACHE1_DMA_PUSH bit 4 is clear. With plain storage all
         * three read 0, so the loop never terminates -- 3.2M MMIO accesses in
         * 62s with no further kernel calls. */
        r = NV_PFIFO_CACHE1_STATUS_LOW_MARK;
        break;
    case NV_PFIFO_RUNOUT_STATUS:
        /* Same contract for the runout FIFO, which the same loop polls at
         * guest offset 0x2400 (block-local 0x400). */
        r = NV_PFIFO_RUNOUT_STATUS_LOW_MARK;
        break;
    case NV_PFIFO_CACHE1_DMA_PUSH:
        /* Idle pusher: DMA_PUSH_STATE (bit 4) clear, so the guest's third
         * condition holds. Explicit rather than incidental, because the same
         * loop treats a set STATE bit as "still busy" and spins. */
        r = d->pfifo.regs[addr] & ~NV_PFIFO_CACHE1_DMA_PUSH_STATE;
        break;
    default:
        r = d->pfifo.regs[addr];
        break;
    }

    nv2a_reg_log_read(NV_PFIFO, addr, size, r);
    return r;
}

void pfifo_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PFIFO, addr, size, val);

    if (addr == NV_PFIFO_CACHE1_DMA_GET || addr == NV_PFIFO_CACHE1_DMA_PUT)
        pfifo_trace("pfifo_write", (uint32_t)addr, (uint32_t)val);

    switch (addr) {
    case NV_PFIFO_INTR_0:
        d->pfifo.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PFIFO_INTR_EN_0:
        d->pfifo.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    case NV_PFIFO_CACHE1_DMA_PUT:
        /* Same plain store as the NV_USER alias above.  `addr` here is
         * already the block-local PFIFO offset, so it is the canonical slot. */
        qemu_mutex_lock(&d->pfifo.lock);
        d->pfifo.regs[addr] = (uint32_t)val;
        qemu_mutex_unlock(&d->pfifo.lock);
        nv2a_submit_pending(d);
        break;
    default:
        d->pfifo.regs[addr] = val;
        break;
    }
}

/* ============================================================
 * Stub handler for unimplemented blocks
 * ============================================================ */

uint64_t nv2a_stub_read(void *opaque, hwaddr addr, unsigned int size)
{
    (void)opaque; (void)size;
    NV2A_DPRINTF("stub read: addr=0x%llx size=%d\n",
                 (unsigned long long)addr, size);
    return 0;
}

void nv2a_stub_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    (void)opaque; (void)size;
    NV2A_DPRINTF("stub write: addr=0x%llx val=0x%llx size=%d\n",
                 (unsigned long long)addr, (unsigned long long)val, size);
}

/* ============================================================
 * Block dispatch table (from xemu nv2a.c)
 * ============================================================ */

#define ENTRY(NAME, LNAME, OFFSET, SIZE) [NV_##NAME] = { \
    .name   = #NAME,                                      \
    .offset = OFFSET,                                     \
    .size   = SIZE,                                       \
    .ops    = { .read = LNAME##_read, .write = LNAME##_write }, \
}
#define STUB_ENTRY(NAME, OFFSET, SIZE) [NV_##NAME] = { \
    .name   = #NAME,                                    \
    .offset = OFFSET,                                   \
    .size   = SIZE,                                     \
    .ops    = { .read = nv2a_stub_read, .write = nv2a_stub_write }, \
}

const NV2ABlockInfo blocktable[NV_NUM_BLOCKS] = {
    ENTRY(PMC,      pmc,      0x000000, 0x001000),
    ENTRY(PBUS,     pbus,     0x001000, 0x001000),
    ENTRY(PFIFO,    pfifo,    0x002000, 0x002000),
    STUB_ENTRY(PFIFO_CACHE,   0x003000, 0x001000),
    STUB_ENTRY(PRMA,          0x007000, 0x001000),
    ENTRY(PVIDEO,   pvideo,   0x008000, 0x001000),
    ENTRY(PTIMER,   ptimer,   0x009000, 0x001000),
    STUB_ENTRY(PCOUNTER,      0x00a000, 0x001000),
    STUB_ENTRY(PVPE,          0x00b000, 0x001000),
    STUB_ENTRY(PTV,           0x00d000, 0x001000),
    STUB_ENTRY(PRMFB,         0x0a0000, 0x020000),
    STUB_ENTRY(PRMVIO,        0x0c0000, 0x001000),
    ENTRY(PFB,      pfb,      0x100000, 0x001000),
    STUB_ENTRY(PSTRAPS,       0x101000, 0x001000),
    ENTRY(PGRAPH,   pgraph,   0x400000, 0x002000),
    ENTRY(PCRTC,    pcrtc,    0x600000, 0x001000),
    STUB_ENTRY(PRMCIO,        0x601000, 0x001000),
    ENTRY(PRAMDAC,  pramdac,  0x680000, 0x001000),
    STUB_ENTRY(PRMDIO,        0x681000, 0x001000),
    /* NV_PRAMIN = 19.  It is handled specially by nv2a_mmio_read/write so
     * the block's backing can be rebound to the claimed instance window. */
    { .name = "PRAMIN", .offset = 0x700000, .size = 0x100000,
      .ops = { .read = nv2a_stub_read, .write = nv2a_stub_write } },
    /* NV_USER = 20 */
    ENTRY(USER,      user,      0x800000, 0x800000),
};

#undef ENTRY
#undef STUB_ENTRY

/* ============================================================
 * MMIO dispatch (for VEH handler integration)
 * ============================================================ */

uint64_t nv2a_mmio_read(NV2AState *d, hwaddr addr, unsigned int size)
{
    if (addr >= 0x700000 && addr < 0x800000) {
        uint32_t offset = (uint32_t)(addr - 0x700000);
        if (d && d->ramin_ptr && size >= 1 && size <= 4 &&
            offset <= d->ramin.size && size <= d->ramin.size - offset) {
            uint32_t value = 0;
            memcpy(&value, d->ramin_ptr + offset, size);
            return value;
        }
        NV2A_DPRINTF("PRAMIN read outside bound offset=0x%x size=%u\n",
                     offset, size);
        return 0;
    }
    /* Find which block handles this address */
    for (int i = 0; i < NV_NUM_BLOCKS; i++) {
        if (!blocktable[i].name) continue;
        if (addr >= blocktable[i].offset &&
            addr < blocktable[i].offset + blocktable[i].size) {
            hwaddr block_addr = addr - blocktable[i].offset;
            return blocktable[i].ops.read(d, block_addr, size);
        }
    }
    NV2A_DPRINTF("MMIO read unmapped: addr=0x%llx\n", (unsigned long long)addr);
    return 0;
}

void nv2a_mmio_write(NV2AState *d, hwaddr addr, uint64_t val, unsigned int size)
{
    if (addr >= 0x700000 && addr < 0x800000) {
        uint32_t offset = (uint32_t)(addr - 0x700000);
        if (d && d->ramin_ptr && size >= 1 && size <= 4 &&
            offset <= d->ramin.size && size <= d->ramin.size - offset) {
            memcpy(d->ramin_ptr + offset, &val, size);
            return;
        }
        NV2A_DPRINTF("PRAMIN write outside bound offset=0x%x size=%u\n",
                     offset, size);
        return;
    }
    for (int i = 0; i < NV_NUM_BLOCKS; i++) {
        if (!blocktable[i].name) continue;
        if (addr >= blocktable[i].offset &&
            addr < blocktable[i].offset + blocktable[i].size) {
            hwaddr block_addr = addr - blocktable[i].offset;
            blocktable[i].ops.write(d, block_addr, val, size);
            return;
        }
    }
    NV2A_DPRINTF("MMIO write unmapped: addr=0x%llx val=0x%llx\n",
                 (unsigned long long)addr, (unsigned long long)val);
}

/* ============================================================
 * Standalone initialization
 * ============================================================ */

NV2AState *nv2a_init_standalone(uint8_t *vram_ptr, uint32_t vram_size,
                                 uint8_t *ramin_ptr, uint32_t ramin_size)
{
    if (g_nv2a) return g_nv2a;

    NV2AState *d = (NV2AState *)calloc(1, sizeof(NV2AState));
    if (!d) return NULL;

    /* Set up VRAM */
    g_vram_region.size = vram_size;
    d->vram = &g_vram_region;
    d->vram_ptr = vram_ptr;
    d->vram_pci.size = vram_size;

    /* PRAMIN has no backing until MmClaimGpuInstanceMemory publishes the
     * validated physical instance range.  The legacy detached allocation is
     * retained in the constructor signature for source compatibility only. */
    (void)ramin_ptr;
    (void)ramin_size;
    if (g_pending_instance_host_ptr) {
        d->ramin.size = g_pending_instance_size;
        d->ramin_ptr = g_pending_instance_host_ptr;
        d->ramin_guest_base = g_pending_instance_guest_base;
    }

    /* PCI config space: NV2A vendor/device */
    pci_set_long(d->parent_obj.config + PCI_VENDOR_ID, 0x02A010DE); /* NVIDIA NV2A */
    pci_set_long(d->parent_obj.config + PCI_CLASS_REVISION, 0x030000A1);

    /* Default PLL: 233 MHz core clock (Xbox default) */
    /* Use the register write path so reset and a later write of the same
     * coefficient agree. Here n=0x1C, m=1, p=1: crystal * 28 / 2. */
    pramdac_write(d, NV_PRAMDAC_NVPLL_COEFF, 0x00011C01, 4);

    /* Default timer divisors */
    d->ptimer.numerator = 1;
    d->ptimer.denominator = 1;

    /* Initialize PFIFO mutex */
    qemu_mutex_init(&d->pfifo.lock);
    qemu_cond_init(&d->pfifo.fifo_cond);
    qemu_cond_init(&d->pfifo.fifo_idle_cond);

    g_nv2a = d;

    fprintf(stderr, "[NV2A] Standalone GPU initialized: VRAM=%uMB RAMIN=%s\n",
            vram_size / (1024*1024),
            d->ramin_ptr ? "instance-bound" : "unbound");

    return d;
}
