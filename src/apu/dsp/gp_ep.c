/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/xbox/mcpx/apu/apu_int.h"

/* A4b1 LOCAL MODIFICATION (new include after gp_ep.c:22): the toolkit's GP DMA
 * choke point, watched-word ledger and GP input accounting (Device semantics 3,
 * 5, 6 and 7). Every use of it below is marked. */
#include "apu_watch.h"

static const int16_t ep_silence[256][2] = { 0 };

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    static int last_known_dsp_pref = -1;
    static int last_known_jit_pref = -1;

    if (last_known_dsp_pref != (int)g_config.audio.use_dsp) {
        if (g_config.audio.use_dsp) {
            d->monitor.point = MCPX_APU_DEBUG_MON_GP_OR_EP;
            d->gp.realtime = true;
            d->ep.realtime = true;
        } else {
            d->monitor.point = MCPX_APU_DEBUG_MON_VP;
            d->gp.realtime = false;
            d->ep.realtime = false;
        }
        last_known_dsp_pref = g_config.audio.use_dsp;
    }

    if (last_known_jit_pref != (int)g_config.audio.use_dsp_jit) {
        dsp_set_engine(d->gp.dsp, g_config.audio.use_dsp_jit);
        dsp_set_engine(d->ep.dsp, g_config.audio.use_dsp_jit);
        last_known_jit_pref = g_config.audio.use_dsp_jit;
    }
}

/* A4b1 LOCAL MODIFICATION (gp_ep.c:51-90 upstream, scatter_gather_rw).
 *
 * The pinned body computes `hwaddr paddr = prd_address + offset_in_page` and
 * then reaches guest memory directly:
 *
 *     assert(paddr + bytes_to_copy < memory_region_size(d->ram));
 *     if (dir) { memcpy(&d->ram_ptr[paddr], ptr, bytes_to_copy);
 *                memory_region_set_dirty(d->ram, paddr, bytes_to_copy); }
 *     else     { memcpy(ptr, &d->ram_ptr[paddr], bytes_to_copy); }
 *
 * Two things change, and nothing else:
 *
 *  1. The SGE entry's value is the title's own address, and at the A4b1
 *     baseline it may be EITHER a window VA (the 0d7929c form) or a physical
 *     offset (the M form). `prd_address` is therefore translated through the
 *     one translation function, Device semantics 3, rather than used raw. The
 *     pinned `& 0x03FFFFFF`-style assumption -- that a flat physical space is
 *     all there is -- is exactly what DS3 exists to replace.
 *
 *  2. Every write goes through the one GP DMA write choke point, Device
 *     semantics 5, and every read through its read counterpart, so the ledger
 *     is complete by construction rather than by enumeration.
 *
 * The pinned `assert(paddr + bytes_to_copy < memory_region_size(d->ram))` is
 * removed: d->ram is never initialised in this toolkit (it is a QEMU
 * MemoryRegion* and there is no QEMU memory subsystem here), so the assert
 * would dereference NULL rather than check anything. The translation function's
 * case 4 is its replacement and is strictly stronger -- it fails closed with a
 * named `[GPDMA] unmapped` line instead of an assert that cannot fire. */
static void scatter_gather_rw(MCPXAPUState *d, hwaddr sge_base,
                              unsigned int max_sge, uint8_t *ptr, uint32_t addr,
                              size_t len, bool dir)
{
    unsigned int page_entry = addr / TARGET_PAGE_SIZE;
    unsigned int offset_in_page = addr % TARGET_PAGE_SIZE;
    unsigned int bytes_to_copy = TARGET_PAGE_SIZE - offset_in_page;

    while (len > 0) {
        assert(page_entry <= max_sge);

        /* The SGE descriptor itself is a GP DMA read of guest memory, so it is
         * translated and counted like any other. It does NOT take the
         * BOOT_SCRATCH_READ latch: that latch is for the bootstrap's data read
         * (Device semantics 6), not for the descriptor fetch that precedes it.
         */
        uint32_t sge_va = 0;
        const uint8_t *sge_ptr =
            apu_guest_dma_ptr((uint32_t)(sge_base + page_entry * 8 + 0), 4,
                              &sge_va);
        uint32_t prd_address;

        if (!sge_ptr) {
            return;   /* unmapped: [GPDMA] unmapped has been logged */
        }
        apu_gp_dma_read(sge_ptr, sge_va, 4, (uint32_t)(sge_base + page_entry * 8));
        prd_address = ldl_le_p(sge_ptr);

        if (bytes_to_copy > len) {
            bytes_to_copy = len;
        }

        {
            uint32_t guest_va = 0;
            uint8_t *host = apu_guest_dma_ptr(prd_address + offset_in_page,
                                              bytes_to_copy, &guest_va);
            if (!host) {
                return;   /* case 4: fail closed, no access is made */
            }

            /* The read path records the transfer and, while the bootstrap
             * window is open (dsp.c's dsp_bootstrap sets it), takes the
             * BOOT_SCRATCH_READ latch -- the presence witness for AC-BOOT and
             * AC-INPUTS (Device semantics 6). */
            /* A4b1 LOCAL MODIFICATION: the bootstrap's scratch DATA read is the
             * presence witness for AC-BOOT and AC-INPUTS (Device semantics 6).
             * Called here, in the bootstrap path, so it does not depend on a
             * table having room. Deliberately NOT called for the SGE descriptor
             * fetch above: that read is the table, not the page. */
            if (!dir) {
                apu_watch_boot_scratch_read(guest_va,
                                            bytes_to_copy >= 4
                                                ? ldl_le_p(host) : 0,
                                            bytes_to_copy, addr);
            }

            if (dir) {
                /* The ONE choke point (Device semantics 5). `addr` is the
                 * DSP-side address of the transfer's first word. */
                apu_gp_dma_write(host, ptr, guest_va, bytes_to_copy, addr);
            } else {
                apu_gp_dma_read(host, guest_va, bytes_to_copy, addr);
                memcpy(ptr, host, bytes_to_copy);
            }
        }

        ptr += bytes_to_copy;
        len -= bytes_to_copy;

        /* After the first iteration, we are page aligned */
        page_entry += 1;
        bytes_to_copy = TARGET_PAGE_SIZE;
        offset_in_page = 0;
    }
}

static void gp_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                          bool dir)
{
    MCPXAPUState *d = opaque;
    // fprintf(stderr, "GP %s scratch 0x%x bytes (0x%x words) at %x (0x%x words)\n", dir ? "writing to" : "reading from", len, len/4, addr, addr/4);
    scatter_gather_rw(d, d->regs[NV_PAPU_GPSADDR], d->regs[NV_PAPU_GPSMAXSGE],
                      ptr, addr, len, dir);
}

static void ep_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                          bool dir)
{
    MCPXAPUState *d = opaque;
    // fprintf(stderr, "EP %s scratch 0x%x bytes (0x%x words) at %x (0x%x words)\n", dir ? "writing to" : "reading from", len, len/4, addr, addr/4);
    scatter_gather_rw(d, d->regs[NV_PAPU_EPSADDR], d->regs[NV_PAPU_EPSMAXSGE],
                      ptr, addr, len, dir);
}

static uint32_t circular_scatter_gather_rw(MCPXAPUState *d, hwaddr sge_base,
                                           unsigned int max_sge, uint8_t *ptr,
                                           uint32_t base, uint32_t end,
                                           uint32_t cur, size_t len, bool dir)
{
    while (len > 0) {
        unsigned int bytes_to_copy = end - cur;

        if (bytes_to_copy > len) {
            bytes_to_copy = len;
        }

        DPRINTF("circular scatter gather %s in range 0x%x - 0x%x at 0x%x of "
                "length 0x%x / 0x%lx bytes\n",
                dir ? "write" : "read", base, end, cur, bytes_to_copy, len);

        assert((cur >= base) && ((cur + bytes_to_copy) <= end));
        scatter_gather_rw(d, sge_base, max_sge, ptr, cur, bytes_to_copy, dir);

        ptr += bytes_to_copy;
        len -= bytes_to_copy;

        /* After the first iteration we might have to wrap */
        cur += bytes_to_copy;
        if (cur >= end) {
            assert(cur == end);
            cur = base;
        }
    }

    return cur;
}

static void gp_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    hwaddr cur_reg;
    if (dir) {
        assert(index < GP_OUTPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_GPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPOFCUR0 + 0x10 * index;
    } else {
        assert(index < GP_INPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_GPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    // fprintf(stderr, "GP %s fifo #%d, base = %x, end = %x, cur = %x, len = %x\n",
    //     dir ? "writing to" : "reading from", index,
    //     base, end, cur, len);

    /* A4b1 LOCAL MODIFICATION (new, after gp_ep.c:166 upstream): the FIFO_READ
     * input hook, Device semantics 6. Universe GP_INPUT_FIFO_COUNT +
     * GP_OUTPUT_FIFO_COUNT = 6, indexed by FIFO.
     *
     * The GP's DMA engine is the only thing that calls fifo_rw, and its data
     * source is statically the SGE-described guest memory at GPFADDR (the
     * pinned dsp_dma.c buf_id 0x0..0x3 arm, which hands scratch_buf to this
     * callback). So the count is of FIFO traffic, and `dir` says which
     * direction. Only the read direction is recorded: a write into the FIFO
     * moves data the GP produced, not an input the GP consumed. */
    if (!dir) {
        apu_gpin_fifo_read(index, len);
    }

    /* DSP hangs if current >= end; but forces current >= base */
    assert(cur < end);
    if (cur < base) {
        cur = base;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_GPFADDR], d->regs[NV_PAPU_GPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

static bool ep_sink_samples(MCPXAPUState *d, uint8_t *ptr, size_t len)
{
    if (d->monitor.point == MCPX_APU_DEBUG_MON_AC97) {
        return false;
    } else if ((d->monitor.point == MCPX_APU_DEBUG_MON_EP) ||
        (d->monitor.point == MCPX_APU_DEBUG_MON_GP_OR_EP)) {
        assert(len == sizeof(d->monitor.frame_buf));
        memcpy(d->monitor.frame_buf, ptr, len);
    }

    return true;
}

static void ep_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    hwaddr cur_reg;
    if (dir) {
        assert(index < EP_OUTPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_EPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPOFCUR0 + 0x10 * index;
    } else {
        assert(index < EP_INPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_EPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    // fprintf(stderr, "EP %s fifo #%d, base = %x, end = %x, cur = %x, len = %x\n",
    //     dir ? "writing to" : "reading from", index,
    //     base, end, cur, len);

    if (dir && index == 0) {
        bool did_sink = ep_sink_samples(d, ptr, len);
        if (did_sink) {
            /* Since we are sinking, push silence out */
            assert(len <= sizeof(ep_silence));
            ptr = (uint8_t*)ep_silence;
        }
    }

    /* DSP hangs if current >= end; but forces current >= base */
    if (cur >= end) {
        cur = cur % (end - base);
    }
    if (cur < base) {
        cur = base;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_EPFADDR], d->regs[NV_PAPU_EPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

/* A4b1 LOCAL MODIFICATION (gp_ep.c:251-260 upstream, proc_rst_write).
 *
 * The pinned transition logic is UNCHANGED -- reset when either bit is clear in
 * the new value, bootstrap when either was clear in oldval and both are set in
 * val -- and Device semantics 1 keeps it exactly. What is added around it is
 * observation and run accounting:
 *
 *   - the bootstrap window (Device semantics 6) is opened across dsp_bootstrap,
 *     so the scratch read the pinned dsp_c_bootstrap performs takes the
 *     BOOT_SCRATCH_READ latch. The window is closed again even if the bootstrap
 *     faults, because a window left open would mislabel a later read;
 *   - the run counters are reset and the counts line emitted after it
 *     (Device semantics 6: "at each bootstrap, after the run counters reset");
 *   - [GPBOOT] is emitted inside the handling of the write that bootstraps
 *     (Device semantics 7), carrying the raw SGE entry 0 and its Device
 *     semantics 3 translation, followed by the first 0x200 PRAM words.
 *
 * The GP side is identified by dsp->is_gp, which dsp_init sets. That is also how
 * the MCPXAPUState is reached: dsp_init(d, gp_scratch_rw, gp_fifo_rw, true) makes
 * the APU state the DMA's rw_opaque, so no signature has to change. */
static void proc_rst_write(DSPState *dsp, uint32_t oldval, uint32_t val)
{
    if (!(val & NV_PAPU_GPRST_GPRST) || !(val & NV_PAPU_GPRST_GPDSPRST)) {
        dsp_reset(dsp);
    } else if (
        (!(oldval & NV_PAPU_GPRST_GPRST) || !(oldval & NV_PAPU_GPRST_GPDSPRST))
        && ((val & NV_PAPU_GPRST_GPRST) && (val & NV_PAPU_GPRST_GPDSPRST))) {
        apu_watch_bootstrap_begin();
        dsp_bootstrap(dsp);
        apu_watch_bootstrap_end();
        apu_watch_gp_bootstrap_done(dsp->is_gp);

        if (dsp->is_gp) {
            MCPXAPUState *d = dsp->dma.rw_opaque;
            uint32_t sge0 = 0;
            uint32_t sge0_va = 0;
            static uint32_t pram[0x200];
            uint32_t i;

            apu_watch_gp_bootstrap(1);

            /* sge0 is the RAW 32-bit value in SGE entry 0; sge0_va is its
             * Device semantics 3 translation (the page the bootstrap reads).
             * Both are printed, so the inverse can be checked in both
             * directions: the 0d7929c form (the entry holds a window VA) and
             * the M form (it holds a physical offset). */
            if (d) {
                uint32_t raw_va = 0;
                const uint8_t *p = apu_guest_dma_ptr(
                    d->regs[NV_PAPU_GPSADDR], 4, &raw_va);
                if (p) {
                    sge0 = ldl_le_p(p);
                }
                (void)apu_guest_dma_ptr(sge0, 4, &sge0_va);
            }

            /* The first 0x200 PRAM words, read back through the pinned API
             * after the bootstrap has masked them with 0x00FFFFFF. Read only
             * when the trace is on: this is 512 reads per bootstrap and there
             * is no reason to pay for them in a default run. */
            if (apu_watch_trace_enabled()) {
                for (i = 0; i < 0x200; i++) {
                    pram[i] = dsp_read_memory(dsp, 'P', i);
                }
                apu_watch_trace_gpboot(val, oldval, sge0, sge0_va, pram, 0x200);
            }

            /* A4b2-NR Leg 1 (diagnostic, discovery instrumentation): request a
             * decode of the loaded program image so the static mechanism leg has
             * the real instruction stream. The request is serviced by the next
             * executed instruction, because dsp_core_t is opaque here. Read-only
             * with respect to the DSP and a strict no-op unless
             * RECOMP_APU_GP_DECODE is set. Declared locally because dsp_cpu.h is
             * outside this packet's write scope. */
            {
                extern void dsp56k_request_decode(void);
                extern void dsp56k_b9_epoch_begin(void);
                dsp56k_request_decode();
                /* A4b2-NR-followup: mark the bootstrap epoch so every P 00B9
                 * event is attributable and no earlier epoch can be silently
                 * omitted from the trace. Strict no-op unless the B9 gate is
                 * set. */
                dsp56k_b9_epoch_begin();
            }
        }
    }
}

/* A4b1 LOCAL MODIFICATION (gp_ep.c:263-303 upstream, gp_read; and the same
 * change to gp_write, ep_read and ep_write below).
 *
 * The pinned bodies dispatch with GNU case ranges:
 *
 *     switch (addr) {
 *     case NV_PAPU_GPXMEM ... NV_PAPU_GPXMEM + 0x1000 * 4 - 1: { ... }
 *     case NV_PAPU_GPMIXBUF ... NV_PAPU_GPMIXBUF + 0x400 * 4 - 1: { ... }
 *     ...
 *     default: r = d->gp.regs[addr]; break;
 *     }
 *
 * `case A ... B:` is a GNU extension. MSVC rejects it outright
 * ("error C2143: syntax error: missing ':' before '...'"), and this toolkit's
 * primary target is MSVC on Windows. The four switches are therefore written as
 * if/else-if chains with the SAME tests in the SAME order and the SAME bodies,
 * which is the smallest rewrite that keeps every arm and every precedence
 * intact. Nothing about which offset reaches which arm changes.
 *
 * This is a build-portability modification only; it is recorded in
 * docs/reviews/a4b-xemu-pin.md's local-modification list.
 *
 * Global Processor - programmable DSP */
static uint64_t gp_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = opaque;

    assert(size == 4);
    assert(addr % 4 == 0);

    uint64_t r = 0;
    if (addr >= NV_PAPU_GPXMEM && addr < NV_PAPU_GPXMEM + 0x1000 * 4) {
        uint32_t xaddr = (addr - NV_PAPU_GPXMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'X', xaddr);
        // fprintf(stderr, "read GP NV_PAPU_GPXMEM [%x] -> %x\n", xaddr, r);
    } else if (addr >= NV_PAPU_GPMIXBUF && addr < NV_PAPU_GPMIXBUF + 0x400 * 4) {
        uint32_t xaddr = (addr - NV_PAPU_GPMIXBUF) / 4;
        r = dsp_read_memory(d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE + xaddr);
        // fprintf(stderr, "read GP NV_PAPU_GPMIXBUF [%x] -> %x\n", xaddr, r);
    } else if (addr >= NV_PAPU_GPYMEM && addr < NV_PAPU_GPYMEM + 0x800 * 4) {
        uint32_t yaddr = (addr - NV_PAPU_GPYMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'Y', yaddr);
        // fprintf(stderr, "read GP NV_PAPU_GPYMEM [%x] -> %x\n", yaddr, r);
    } else if (addr >= NV_PAPU_GPPMEM && addr < NV_PAPU_GPPMEM + 0x1000 * 4) {
        uint32_t paddr = (addr - NV_PAPU_GPPMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'P', paddr);
        // fprintf(stderr, "read GP NV_PAPU_GPPMEM [%x] -> %x\n", paddr, r);
    } else {
        r = d->gp.regs[addr];
    }
    DPRINTF("mcpx apu GP: read [0x%" HWADDR_PRIx "] -> 0x%lx\n", addr, r);

    return r;
}

static void gp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    qemu_mutex_lock(&d->lock);

    assert(size == 4);
    assert(addr % 4 == 0);

    DPRINTF("mcpx apu GP: [0x%" HWADDR_PRIx "] = 0x%lx\n", addr, val);

    /* A4b1 LOCAL MODIFICATION: GNU case ranges -> if/else-if. Same tests, same
     * order, same bodies. See the note above gp_read. */
    if (addr >= NV_PAPU_GPXMEM && addr < NV_PAPU_GPXMEM + 0x1000 * 4) {
        uint32_t xaddr = (addr - NV_PAPU_GPXMEM) / 4;
        // fprintf(stderr, "gp write xmem %x = %x\n", xaddr, val);
        dsp_write_memory(d->gp.dsp, 'X', xaddr, val);
    } else if (addr >= NV_PAPU_GPMIXBUF && addr < NV_PAPU_GPMIXBUF + 0x400 * 4) {
        uint32_t xaddr = (addr - NV_PAPU_GPMIXBUF) / 4;
        // fprintf(stderr, "gp write xmixbuf %x = %x\n", xaddr, val);
        dsp_write_memory(d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE + xaddr, val);
    } else if (addr >= NV_PAPU_GPYMEM && addr < NV_PAPU_GPYMEM + 0x800 * 4) {
        uint32_t yaddr = (addr - NV_PAPU_GPYMEM) / 4;
        // fprintf(stderr, "gp write ymem %x = %x\n", yaddr, val);
        dsp_write_memory(d->gp.dsp, 'Y', yaddr, val);
    } else if (addr >= NV_PAPU_GPPMEM && addr < NV_PAPU_GPPMEM + 0x1000 * 4) {
        uint32_t paddr = (addr - NV_PAPU_GPPMEM) / 4;
        // fprintf(stderr, "gp write pmem %x = %x\n", paddr, val);
        dsp_write_memory(d->gp.dsp, 'P', paddr, val);
    } else if (addr == NV_PAPU_GPRST) {
        proc_rst_write(d->gp.dsp, d->gp.regs[NV_PAPU_GPRST], val);
        d->gp.regs[NV_PAPU_GPRST] = val;
    } else {
        d->gp.regs[addr] = val;
    }

    qemu_mutex_unlock(&d->lock);
}

const MemoryRegionOps gp_ops = {
    .read = gp_read,
    .write = gp_write,
};

/* Encode Processor - encoding DSP */
static uint64_t ep_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = opaque;

    assert(size == 4);
    assert(addr % 4 == 0);

    uint64_t r = 0;
    /* A4b1 LOCAL MODIFICATION: GNU case ranges -> if/else-if. See gp_read. */
    if (addr >= NV_PAPU_EPXMEM && addr < NV_PAPU_EPXMEM + 0xC00 * 4) {
        uint32_t xaddr = (addr - NV_PAPU_EPXMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'X', xaddr);
        // fprintf(stderr, "read EP  NV_PAPU_EPXMEM [%x] -> %x\n", xaddr, r);
    } else if (addr >= NV_PAPU_EPYMEM && addr < NV_PAPU_EPYMEM + 0x100 * 4) {
        uint32_t yaddr = (addr - NV_PAPU_EPYMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'Y', yaddr);
        // fprintf(stderr, "read EP  NV_PAPU_EPYMEM [%x] -> %x\n", yaddr, r);
    } else if (addr >= NV_PAPU_EPPMEM && addr < NV_PAPU_EPPMEM + 0x1000 * 4) {
        uint32_t paddr = (addr - NV_PAPU_EPPMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'P', paddr);
        // fprintf(stderr, "read EP  NV_PAPU_EPPMEM [%x] -> %x\n", paddr, r);
    } else {
        r = d->ep.regs[addr];
    }
    DPRINTF("mcpx apu EP: read [0x%" HWADDR_PRIx "] -> 0x%lx\n", addr, r);

    return r;
}

static void ep_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    qemu_mutex_lock(&d->lock);

    assert(size == 4);
    assert(addr % 4 == 0);

    DPRINTF("mcpx apu EP: [0x%" HWADDR_PRIx "] = 0x%lx\n", addr, val);

    /* A4b1 LOCAL MODIFICATION: GNU case ranges -> if/else-if. See gp_read. */
    if (addr >= NV_PAPU_EPXMEM && addr < NV_PAPU_EPXMEM + 0xC00 * 4) {
        uint32_t xaddr = (addr - NV_PAPU_EPXMEM) / 4;
        dsp_write_memory(d->ep.dsp, 'X', xaddr, val);
        // fprintf(stderr, "ep write xmem %x = %x\n", xaddr, val);
    } else if (addr >= NV_PAPU_EPYMEM && addr < NV_PAPU_EPYMEM + 0x100 * 4) {
        uint32_t yaddr = (addr - NV_PAPU_EPYMEM) / 4;
        dsp_write_memory(d->ep.dsp, 'Y', yaddr, val);
        // fprintf(stderr, "ep write ymem %x = %x\n", yaddr, val);
    } else if (addr >= NV_PAPU_EPPMEM && addr < NV_PAPU_EPPMEM + 0x1000 * 4) {
        uint32_t paddr = (addr - NV_PAPU_EPPMEM) / 4;
        // fprintf(stderr, "ep write pmem %x = %x\n", paddr, val);
        dsp_write_memory(d->ep.dsp, 'P', paddr, val);
    } else if (addr == NV_PAPU_EPRST) {
        proc_rst_write(d->ep.dsp, d->ep.regs[NV_PAPU_EPRST], val);
        d->ep.regs[NV_PAPU_EPRST] = val;
        d->ep_frame_div = 0; /* FIXME: Still unsure about frame sync */
    } else {
        d->ep.regs[addr] = val;
    }

    qemu_mutex_unlock(&d->lock);
}

const MemoryRegionOps ep_ops = {
    .read = ep_read,
    .write = ep_write,
};

/* A4b1 LOCAL MODIFICATION (gp_ep.c:443-494 upstream, mcpx_apu_dsp_frame).
 *
 * The pinned GP half is UNCHANGED in behaviour: write the VP mixbins into GP X
 * memory at GP_DSP_MIXBUF_BASE, then -- when both GPRST bits are set --
 * dsp_start_frame, clear halt, clear the cycle count, and dsp_run(dsp, 1000) in
 * a do/while until halt is requested. Device semantics 2 keeps exactly that.
 *
 * Four things are added, and none of them changes a value, a store or a control
 * flow:
 *
 *  1. MIXBUF provenance (Device semantics 6). At the mixbin write, set the
 *     per-frame flag mixbuf_stub = (vp_active_voices > 0) || (any sample written
 *     is non-zero). The read hook then counts reads_while_stub per bin.
 *  2. The GP run counters (Device semantics 6) and the [GPRUN] line
 *     (Device semantics 7). `pc` is read through the pinned public API:
 *     dsp_sync_to_vm copies the interpreter's pc into DSPState.core, which is
 *     what that call is for. It is only made when the trace is on.
 *  3. The [GPDMA] per-frame line (Device semantics 7).
 *  4. The EP's monitor passthrough. Upstream's last block, when
 *     monitor.point is MON_GP (or MON_GP_OR_EP with the EP disabled), fills the
 *     monitor frame buffer from GP X memory. This toolkit's audio output is its
 *     own mixdown (src/apu/apu_mixdown.c), which the old apu_dsp.c performed,
 *     and Device semantics 2 says "The EP keeps the existing mixbin
 *     passthrough" -- so that block is replaced by a call to it. The pinned EP
 *     run block below is kept verbatim.
 */
void mcpx_apu_dsp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Write VP results to the GP DSP MIXBUF */
    apu_gp_mixbuf_frame_begin((uint32_t)mcpx_apu_vp_active_voices(d));
    for (int mixbin = 0; mixbin < NUM_MIXBINS; mixbin++) {
        uint32_t base = GP_DSP_MIXBUF_BASE + mixbin * NUM_SAMPLES_PER_FRAME;
        for (int sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++) {
            uint32_t w = float_to_24b(mixbins[mixbin][sample]);
            apu_gp_mixbuf_note_sample(w);
            dsp_write_memory(d->gp.dsp, 'X', base + sample, w);
        }
    }

    bool ep_enabled = (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPRST) &&
                      (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPDSPRST);
    (void)ep_enabled;

    /* Run GP */
    if ((d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPRST) &&
        (d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPDSPRST)) {
        uint32_t pc = 0;
        int halt;

        dsp_start_frame(d->gp.dsp);
        dsp_set_halt_requested(d->gp.dsp, false);
        dsp_set_cycle_count(d->gp.dsp, 0);
        do {
            dsp_run(d->gp.dsp, 1000);
        } while (!dsp_get_halt_requested(d->gp.dsp) && d->gp.realtime);
        g_dbg.gp.cycles = dsp_get_cycle_count(d->gp.dsp);

        halt = dsp_get_halt_requested(d->gp.dsp) ? 1 : 0;
        apu_watch_gp_frame(1);
        if (apu_watch_trace_enabled()) {
            dsp_sync_to_vm(d->gp.dsp);
            pc = d->gp.dsp->core.pc;
            apu_watch_trace_gprun((uint32_t)g_dbg.gp.cycles, pc, halt);
            apu_watch_trace_gpdma_frame();
        }
        (void)ep_enabled;
    }

    /* A4b1 LOCAL MODIFICATION: this toolkit's EP monitor passthrough, which
     * upstream's MON_GP block performed inside the GP branch above. It stays
     * where the toolkit had it -- every frame, whatever GPRST says -- because
     * Device semantics 2 says the EP keeps the existing mixbin passthrough, and
     * moving it inside the GP branch would silence this toolkit's audio output
     * until the guest enabled the GP. */
    mcpx_apu_monitor_mixdown(d, mixbins);

    /* Run EP */
    if ((d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPRST) &&
        (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPDSPRST)) {
        if (d->ep_frame_div % 8 == 0) {
            dsp_start_frame(d->ep.dsp);
            dsp_set_halt_requested(d->ep.dsp, false);
            dsp_set_cycle_count(d->ep.dsp, 0);
            do {
                dsp_run(d->ep.dsp, 1000);
            } while (!dsp_get_halt_requested(d->ep.dsp) && d->ep.realtime);
            g_dbg.ep.cycles = dsp_get_cycle_count(d->ep.dsp);
        }
    }
}

/* A4b1 LOCAL MODIFICATION (gp_ep.c:496-510 upstream, mcpx_apu_dsp_init).
 *
 * The pinned body is UNCHANGED and stays here, because gp_scratch_rw,
 * ep_scratch_rw, gp_fifo_rw and ep_fifo_rw are static to this file: moving it
 * out would mean exporting four internal callbacks, which is a larger change to
 * the pinned code than leaving it where the pinned code put it.
 *
 * The one thing added is the startup line, which the toolkit's old stub printed
 * and which a run log is read for. It names the pin and the interpreter, so the
 * log says which core is running rather than only that one is. */
void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    d->gp.dsp = dsp_init(d, gp_scratch_rw, gp_fifo_rw, true);
    dsp_set_halt_requested(d->gp.dsp, false);
    dsp_set_cycle_count(d->gp.dsp, 0);

    d->ep.dsp = dsp_init(d, ep_scratch_rw, ep_fifo_rw, false);
    dsp_set_halt_requested(d->ep.dsp, false);
    dsp_set_cycle_count(d->ep.dsp, 0);

    /* Until DSP is more performant, a switch to decide whether or not we should
     * use the full audio pipeline or not.
     */
    mcpx_apu_update_dsp_preference(d);

    /* A4b1 LOCAL MODIFICATION: the toolkit's startup line, replacing the old
     * stub's "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)". It
     * reports the pin and the interpreter, which is what a run log needs to
     * say. */
    fprintf(stderr, "[APU] DSP56300 GP/EP core initialized (pinned xemu"
                    " 67cc79e6, C interpreter; GP runs on the APU frame thread"
                    " when GPRST enables it)\n");
}
