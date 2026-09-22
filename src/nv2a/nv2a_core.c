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

    if (d->pmc.pending_interrupts && d->pmc.enabled_interrupts) {
        pci_irq_assert(PCI_DEVICE(d));
    } else {
        pci_irq_deassert(PCI_DEVICE(d));
    }
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

uint64_t pgraph_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint64_t r = d->pgraph.regs[addr];
    nv2a_reg_log_read(NV_PGRAPH, addr, size, r);
    return r;
}

void pgraph_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    nv2a_reg_log_write(NV_PGRAPH, addr, size, val);
    if (addr == NV_PGRAPH_INTR) {
        d->pgraph.regs[addr] &= ~(uint32_t)val;
        d->pgraph.pending_interrupts &= ~(uint32_t)val;
        nv2a_update_irq(d);
        return;
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

/* NV097 method constants for dispatch */
#define M_NO_OPERATION          0x0100
#define M_SET_SURFACE_FORMAT    0x0208
#define M_SET_SURFACE_PITCH     0x020C
#define M_SET_SURFACE_COLOR_OFF 0x0210
#define M_SET_SURFACE_ZETA_OFF  0x0214
#define M_SET_SURFACE_CLIP_H    0x0200
#define M_SET_SURFACE_CLIP_V    0x0204
#define M_CLEAR_SURFACE         0x01D0
#define M_SET_COLOR_CLEAR_VALUE 0x01D4
#define M_SET_BEGIN_END         0x17FC
#define M_INLINE_ARRAY          0x1818
#define M_FLIP_INCREMENT_WRITE  0x0114
#define M_FLIP_STALL            0x0118
#define M_SET_VIEWPORT_OFFSET   0x0A20
#define M_SET_VIEWPORT_SCALE    0x0AF0

void pgraph_method(NV2AState *d, uint32_t subchannel,
                   uint32_t method, uint32_t param)
{
    g_pgraph_method_count++;

    /* Route through D3D11 translator first */
    if (pgraph_d3d11_method(subchannel, method, param)) {
        /* Handled by D3D11 translator — still store in regs for state queries */
        if (method < 0x2000 * 4) {
            d->pgraph.regs[method / 4] = param;
        }
        return;
    }

    /* Log unhandled methods (first 20 + periodic) */
    if (g_pgraph_method_count <= 20 || (g_pgraph_method_count % 5000) == 0) {
        fprintf(stderr, "[PGRAPH] #%u UNHANDLED sub=%u 0x%04X = 0x%08X\n",
                g_pgraph_method_count, subchannel, method, param);
    }

    /* Store method parameters in PGRAPH register space */
    if (method < 0x2000 * 4) {
        d->pgraph.regs[method / 4] = param;
    }

    /* Track high-level operations (legacy counters) */
    switch (method) {
    case M_CLEAR_SURFACE:
        g_pgraph_clear_count++;
        break;

    case M_SET_BEGIN_END:
        if (param != 0) {
            g_pgraph_in_begin = 1;
            g_pgraph_draw_count++;
        } else {
            g_pgraph_in_begin = 0;
        }
        break;

    case M_INLINE_ARRAY:
        if (g_pgraph_in_begin) {
            g_pgraph_inline_verts++;
        }
        break;

    case M_FLIP_INCREMENT_WRITE:
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
};

/* NV01_SUBC_SET_OBJECT binds a RAMHT handle.  Production lookup uses the
 * original 0x001945D6 hash into the claimed PRAMIN table; the fixture seam
 * remains test-only. */
#define M_SET_OBJECT 0x0000u
#define NV097_CLASS  0x97u
/* The other classes JSRF binds, named here because whether a method may be
 * executed is a per-class decision. nv2a_regs.h carries all four; the walk
 * previously knew only NV097, so a subchannel bound to the blit engine could
 * never be accepted no matter what it submitted. */
#define NV_MEMCPY_CLASS     0x39u   /* NV_MEMORY_TO_MEMORY_FORMAT */
#define NV_SURFACES2D_CLASS 0x62u   /* NV_CONTEXT_SURFACES_2D */
#define NV_IMAGEBLIT_CLASS  0x9Fu   /* NV_IMAGE_BLIT */

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

static bool ramht_lookup_class(NV2AState *d, uint32_t handle, uint32_t *class_id)
{
    uint32_t ramht, size_code, ramht_size, ramht_base, bits, hash, slot;
    uint32_t entry_handle, entry_context, instance;
    uint32_t object[4];
    if (!d || !class_id || !handle || !d->ramin_ptr || d->ramin.size < 16)
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
    memcpy(object, d->ramin_ptr + instance, 16);
    *class_id = object[0] & 0xFFu;
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

bool nv2a_submit_pending(NV2AState *d)
{
    uint32_t get, put, pc, ret = 0, words = 0, packets = 0;
    uint32_t seen[1024]; unsigned seen_count = 0;
    uint32_t trace[32] = { 0 };   /* ring of recent walk addresses, for the budget dump */
    struct { uint32_t subchannel, method, param; } staged[1024];
    uint32_t staged_count = 0;
    uint32_t staged_class[8], staged_object[8];
    bool ok = true;
    if (!d) return false;
    qemu_mutex_lock(&d->pfifo.lock);
    /* The sink is a per-submission record of the methods just walked. Nothing
     * reads it and nothing used to clear it, so it ratcheted to its 256 cap and
     * then rejected every later submission for the rest of the run -- which is
     * what strands PFIFO_DMA_GET. The test harness already treats it this way
     * (submit_reset zeroes sink_count), so this is the model catching up with
     * its own contract rather than a relaxation: the within-submission cap is
     * unchanged and a 257-packet stream still rejects. */
    d->pfifo.sink_count = 0;
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
    while (pc != put) {
        uint32_t h, address = pc;
        trace[words & 31u] = address;
        if (words >= 4096 || packets >= 1024) {
            d->pfifo.submit_diag = NV2A_SUBMIT_BUDGET; ok = false;
            /* A straight-line walk cannot consume more than PUT-GET words, so
             * reaching the budget means a jump or call target moved pc and the
             * walk kept going. Without the path this is indistinguishable from
             * "the ring is simply large", which it is not. */
            fprintf(stderr, "  [PFIFO] budget_exhausted get=%08X put=%08X"
                    " begin=%08X end=%08X words=%u packets=%u pc=%08X\n",
                    get, put, begin, end, words, packets, pc);
            fprintf(stderr, "          last 32 visit addresses:");
            for (unsigned i = 0; i < 32; ++i) {
                if (i % 8u == 0u) fprintf(stderr, "\n            ");
                fprintf(stderr, "%08X ", trace[(words - 32u + i) & 31u]);
            }
            fprintf(stderr, "\n");
            fflush(stderr);
            break;
        }
        for (unsigned i = 0; i < seen_count; ++i) if (seen[i * 2] == pc && seen[i * 2 + 1] == ret) { d->pfifo.submit_diag = NV2A_SUBMIT_LOOP; ok = false; goto done; }
        if (seen_count < 512) { seen[seen_count * 2] = pc; seen[seen_count * 2 + 1] = ret; ++seen_count; }
        if (!submit_read_word(d, pc, &h)) { d->pfifo.submit_diag = NV2A_SUBMIT_UNREADABLE; ok = false; break; }
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
            uint32_t count = (h >> 18) & 0x7ffu, method = h & 0x1ffcu, subchannel = (h >> 13) & 7u;
            if (!(h & 0x40000000u) && count && method + 4u * (count - 1u) > 0x1ffcu) { d->pfifo.submit_diag = NV2A_SUBMIT_METHOD_RANGE; ok = false; break; }
            if (d->pfifo.sink_count + staged_count + count > 1024) { d->pfifo.submit_diag = NV2A_SUBMIT_SINK_FULL; ok = false; break; }
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t param;
                if (words >= 4096) { d->pfifo.submit_diag = NV2A_SUBMIT_BUDGET; ok = false; goto done; }
                if (pc == put || !submit_read_word(d, pc, &param)) { d->pfifo.submit_diag = pc == put ? NV2A_SUBMIT_TRUNCATED : NV2A_SUBMIT_UNREADABLE; ok = false; goto done; }
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
                           !nv2a_method_implemented(staged_class[subchannel], method)) {
                    d->pfifo.submit_diag = NV2A_SUBMIT_UNSUPPORTED_METHOD;
                    d->pfifo.submit_diag_get = address;
                    d->pfifo.submit_diag_subchannel = subchannel;
                    d->pfifo.submit_diag_method = method;
                    d->pfifo.submit_diag_param = param;
                    ok = false;
                    goto done;
                }
                staged[staged_count].subchannel = subchannel;
                staged[staged_count].method = method;
                staged[staged_count].param = param;
                ++staged_count;
                if (!(h & 0x40000000u)) method += 4;
            }
            continue;
        }
        d->pfifo.submit_diag = NV2A_SUBMIT_RESERVED; d->pfifo.submit_diag_get = address; ok = false; break;
    }
    if (ok) {
        for (uint32_t i = 0; i < staged_count; ++i) {
            d->pfifo.sink[d->pfifo.sink_count].subchannel = staged[i].subchannel;
            d->pfifo.sink[d->pfifo.sink_count].class_id = staged_class[staged[i].subchannel];
            d->pfifo.sink[d->pfifo.sink_count].method = staged[i].method;
            d->pfifo.sink[d->pfifo.sink_count].param = staged[i].param;
            ++d->pfifo.sink_count;
            /* Capture the parameter as register state. For the register-setting
             * methods -- which is most of the NV097 pipeline, and all of the
             * surface, blit and memcpy state -- this IS the implementation: the
             * value is where a renderer reads it from. Methods that trigger an
             * action rather than set state (blit, notify, flip) are captured here
             * too and need their own handling on top; the notify one is what
             * JSRF's ring-space wait at 0x001914F0 is waiting on.
             *
             * Only NV097 has a register file in this model, so only NV097 is
             * captured this way; the other classes' parameters are recorded in
             * the sink, which now carries the class. */
            if (staged_class[staged[i].subchannel] == NV097_CLASS) {
                d->pgraph.regs[staged[i].method / 4] = staged[i].param;
            }
        }
        memcpy(d->pfifo.binding_class, staged_class, sizeof(staged_class));
        memcpy(d->pfifo.binding_object, staged_object, sizeof(staged_object));
        if (staged_count) {
            d->pfifo.submit_last_method = staged[staged_count - 1].method;
            d->pfifo.submit_last_param = staged[staged_count - 1].param;
        }
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] = pc;
        pfifo_trace("submit_commit", NV_PFIFO_CACHE1_DMA_GET, pc);
        ++d->pfifo.submit_successes;
        d->pfifo.submit_diag = NV2A_SUBMIT_OK;
    }
done:
    d->pfifo.submit_words += words;
    d->pfifo.submit_packets += packets;
    d->pfifo.submit_diag_get = (d->pfifo.submit_diag == NV2A_SUBMIT_OK) ? pc : d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    qemu_mutex_unlock(&d->pfifo.lock);
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
        /* JSRF kicks by setting bit 16 of the DMA PUT value and then polling
         * until the engine clears it (0x00191270, inlined at
         * 0x001912C6..0x001912EA).  The latch is owned here, so the sequence
         * must be: store the offset, run the pending submission, clear the
         * bit.  Clearing it is the acknowledgement the guest waits for;
         * leaving it set spins 0x00191290 forever. */
        uint32_t offset = (uint32_t)val & NV_PFIFO_CACHE1_DMA_PUT_OFFSET;
        pfifo_trace("user_write", NV_PFIFO_CACHE1_DMA_PUT, offset);
        qemu_mutex_lock(&d->pfifo.lock);
        if (val & NV_PFIFO_CACHE1_DMA_PUT_KICK) {
            d->pfifo.kick_requests++;
            d->pfifo.kick_last_put = offset;
        }
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] = offset;
        qemu_mutex_unlock(&d->pfifo.lock);
        nv2a_submit_pending(d);
        /* Per-submit diagnostic. GET only moves on success, so when it stays put
         * the reason is here and nowhere else -- the walk's own diag, plus the
         * exact packet that stopped it. Kept in the model rather than re-added
         * per investigation, because "why did the ring not drain" is the question
         * this model gets asked most often. */
        {
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
        }
        if (val & NV_PFIFO_CACHE1_DMA_PUT_KICK) {
            qemu_mutex_lock(&d->pfifo.lock);
            d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] &=
                NV_PFIFO_CACHE1_DMA_PUT_OFFSET;
            d->pfifo.kick_acks++;
            qemu_mutex_unlock(&d->pfifo.lock);
        }
    }
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
        /* Same latch contract as the NV_USER alias above.  `addr` here is
         * already the block-local PFIFO offset, so it is the canonical slot. */
        if (val & NV_PFIFO_CACHE1_DMA_PUT_KICK) {
            uint32_t offset = (uint32_t)val & NV_PFIFO_CACHE1_DMA_PUT_OFFSET;
            qemu_mutex_lock(&d->pfifo.lock);
            d->pfifo.kick_requests++;
            d->pfifo.kick_last_put = offset;
            d->pfifo.regs[addr] = offset;
            qemu_mutex_unlock(&d->pfifo.lock);
            nv2a_submit_pending(d);
            qemu_mutex_lock(&d->pfifo.lock);
            d->pfifo.regs[addr] &= NV_PFIFO_CACHE1_DMA_PUT_OFFSET;
            d->pfifo.kick_acks++;
            qemu_mutex_unlock(&d->pfifo.lock);
        } else {
            qemu_mutex_lock(&d->pfifo.lock);
            d->pfifo.regs[addr] = (uint32_t)val & NV_PFIFO_CACHE1_DMA_PUT_OFFSET;
            qemu_mutex_unlock(&d->pfifo.lock);
            nv2a_submit_pending(d);
        }
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
