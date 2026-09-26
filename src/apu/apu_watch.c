/*
 * MCPX APU watched-word ledger, GP DMA choke point and GP input accounting.
 *
 * A4b1 steps 4 and 6. Implementation of Device semantics 3, 5, 6 and 7.
 * See apu_watch.h for the exported contract and the design authorities.
 *
 * Copyright (c) 2026 Burnout 3 Static Recompilation Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_watch.h"
#include "apu_state.h"

#include <stdio.h>
#include <string.h>

/* Device semantics 3 names xbox_ContiguousAllocatedBytes()
 * (src/kernel/xbox_memory_layout.c:2694) as the high-water mark, and Device
 * semantics 6 names g_memory_size as the low-RAM bound. Both are the toolkit's
 * own guest-memory geometry, so the APU model reads them rather than deriving a
 * second copy of the rule -- which is how the two would drift apart. */
#include "kernel/kernel.h"              /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */
#include "kernel/xbox_memory_layout.h"  /* xbox_ContiguousAllocatedBytes,
                                           xbox_GetMappedSize */

/* ============================================================
 * The watched word
 * ============================================================ */

/* W_va = MEM32(0x001BA858)+0x810, read at write time.
 *
 * 0x001BA858 is a low-RAM guest VA, so host(va) = g_apu_ram_ptr + va -- see the
 * translation note in apu_guest_dma_ptr() below. Reading it through
 * g_apu_ram_ptr rather than through a cached value is the whole point: the
 * guest publishes the command-block base at run time. */
#define APU_WATCH_BASE_VA 0x001BA858u
#define APU_WATCH_WORD_OFF 0x810u

static uint32_t watch_w_va(void)
{
    if (!g_apu_ram_ptr) {
        return 0;
    }
    return *(volatile uint32_t *)(g_apu_ram_ptr + APU_WATCH_BASE_VA)
           + APU_WATCH_WORD_OFF;
}

/* ============================================================
 * Ledger state
 * ============================================================ */

static struct {
    volatile LONG seq;
    volatile LONG counts[APU_WATCH_COUNTERS];
    struct apu_watch_latch latch[APU_WATCH_LATCH_CLASSES];
    struct apu_watch_site sites[APU_WATCH_N_SITES];

    uint32_t boots;
    uint32_t gp_frames;
    uint64_t gp_insns;

    uint32_t gpdma_ambiguous;
    uint64_t gpdma_reads;
    uint64_t gpdma_writes;
    uint64_t gpdma_rbytes;
    uint64_t gpdma_wbytes;
    uint32_t gpdma_watch_lines;

    uint32_t anchor_site;

    uint32_t se_frame;                   /* global frame count */

    struct apu_watch_gpin gpin;
    struct apu_watch_at_clear at_clear;
    struct apu_watch_obs obs;

    /* Per-frame MIXBUF provenance (Device semantics 6). */
    uint32_t mixbuf_stub;
    uint32_t mixbuf_vp_active_voices;

    /* Bootstrap window: while set, the next GP DMA read is the bootstrap's
     * scratch read and takes the BOOT_SCRATCH_READ latch. */
    uint32_t in_bootstrap;

    /* [GPDMA] unmapped once-per-address set (Device semantics 3 case 4). */
    uint32_t unmapped_n;
    uint32_t unmapped_set[APU_WATCH_UNMAPPED_SET];
} s;

/* ============================================================
 * Trace gate (Device semantics 7: read once and cached)
 * ============================================================ */

int apu_watch_trace_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_TRACE");
        on = (e && *e) ? 1 : 0;
    }
    return on;
}

/* ============================================================
 * Latches and emission
 * ============================================================ */

static const char *const class_name[APU_WATCH_LATCH_CLASSES] = {
    "GP_CLEAR",
    "GP_ZERO_OVER_ZERO",
    "GP_ZERO_OVER_OTHER",
    "GP_NONZERO_OVER",
    "GP_PARTIAL",
    "CPU_ANCHOR",
    "CPU_ZERO",
    "CPU_ZERO_OVERFLOW",
    "MIXBUF_STUB_READ",
    "BOOT_SCRATCH_READ",
    "GPIN_OUT_OF_UNIVERSE",
};

static void emit_counts(void);
static void emit_gpin_summary(void);

static void emit_latch(const char *cls, const struct apu_watch_latch *l)
{
    if (!apu_watch_trace_enabled()) {
        return;
    }
    fprintf(stderr,
            "[GPWATCH] latch class=%s seq=%u va=%08X observed=%08X "
            "payload=%08X site=%08X frame=%u insns=%llu dsp_addr=%06X\n",
            cls, l->seq, l->va, l->observed, l->payload, l->site, l->frame,
            (unsigned long long)l->insns, l->dsp_addr);
    fflush(stderr);
}

/* Fill and emit a latch, once. The thread that wins the exchange on `latched`
 * is the one that writes every field and prints the line, so a class can never
 * be latched twice or printed twice.
 *
 * `insns` is passed rather than read from the ledger because the field is
 * defined differently per side: GP -- gp_insns at the event; CPU -- 0. */
static int latch_fire(struct apu_watch_latch *l, uint32_t seq, uint32_t va,
                      uint32_t observed, uint32_t payload, uint32_t site,
                      uint64_t insns, uint32_t dsp_addr)
{
    if (InterlockedCompareExchange((volatile LONG *)&l->latched, 1, 0) != 0) {
        return 0;
    }
    l->seq = seq;
    l->va = va;
    l->observed = observed;
    l->payload = payload;
    l->site = site;
    l->frame = s.se_frame;
    l->insns = insns;
    l->dsp_addr = dsp_addr;
    return 1;
}

static void after_latch_fired(uint32_t cls)
{
    emit_latch(class_name[cls], &s.latch[cls]);
    emit_counts();
}

/* Count a latch class, then try to latch it. */
static void record_latch(uint32_t cls, uint32_t seq, uint32_t va,
                         uint32_t observed, uint32_t payload, uint32_t site,
                         uint32_t dsp_addr)
{
    InterlockedIncrement(&s.counts[cls]);
    if (latch_fire(&s.latch[cls], seq, va, observed, payload, site, s.gp_insns,
                   dsp_addr)) {
        /* Freeze at the clear: when GP_CLEAR latches -- on the APU thread, in
         * the DMA write path -- copy every input counter and latch into the
         * write-once at_clear block. The running counters continue for the
         * whole run and serve the NOCLEAR/NOEXEC brief. This is what replaces
         * the retired cut-off state and its reset hazards.
         *
         * Device semantics 6 prints the [GPIN] summary "at the existing cadence
         * and once for at_clear": the freeze gets its own emission here, so the
         * frozen block is readable at the moment it is taken and does not have
         * to wait for the next cadence tick. */
        if (cls == APU_WATCH_GP_CLEAR) {
            apu_watch_freeze_at_clear(seq);
            emit_gpin_summary();
        }
        after_latch_fired(cls);
    }
}

/* ============================================================
 * Device semantics 3 -- the ONE translation function
 * ============================================================ */

/* DMA region class of a TRANSLATED address (Device semantics 6).
 *
 * This is the only place the class is derived from an address. The read path
 * calls it with apu_gp_dma_read()'s `guest_va`, which IS apu_guest_dma_ptr()'s
 * output, so "decided inside the one translation function" holds. */
static int dma_class_of(uint32_t translated_va)
{
    size_t mapped = xbox_GetMappedSize();

    if (mapped != 0 && (uint64_t)translated_va < (uint64_t)mapped) {
        return APU_WATCH_DMA_LOW_RAM;
    }
    if (translated_va >= XBOX_CONTIG_BASE &&
        (uint64_t)translated_va <
            (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE) {
        return APU_WATCH_DMA_CONTIG;
    }
    /* The MCPX, NV2A and flash apertures: anything at or above 0xFD000000. */
    if (translated_va >= 0xFD000000u) {
        return APU_WATCH_DMA_DEVICE;
    }
    return APU_WATCH_DMA_OTHER_MAPPED;
}

/* Device semantics 3, case 4. Unconditional (not trace-gated), once per
 * address. */
static void note_unmapped(uint32_t addr, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < s.unmapped_n; i++) {
        if (s.unmapped_set[i] == addr) {
            return;
        }
    }
    if (s.unmapped_n < APU_WATCH_UNMAPPED_SET) {
        s.unmapped_set[s.unmapped_n++] = addr;
    }
    fprintf(stderr, "[GPDMA] unmapped addr=%08X len=%X\n", addr, len);
    fflush(stderr);
}

/*
 * The inverse of bridge_MmGetPhysicalAddress AT THE A4b1 BASELINE M (3f8bf67c).
 *
 * The forward map at M is NON-INJECTIVE:
 *     bridge_MmGetPhysicalAddress -> xbox_MmGetPhysicalAddress
 *     (src/kernel/kernel_memory.c:166-185):
 *         a VA in [XBOX_CONTIG_BASE, XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
 *             returns VA - XBOX_CONTIG_BASE;
 *         anything else returns itself.
 * So low-RAM VA X and window VA 0x80000000 + X both map to physical X, and an
 * inverse handed one of them cannot tell which without being told.
 *
 * The four cases, IN THIS ORDER:
 *   1. an address inside the window [XBOX_CONTIG_BASE, +XBOX_CONTIG_SIZE) is
 *      itself a VA and is used as is. This keeps the 0d7929c form working: at
 *      0d7929c the forward map was the identity, so A4a's R1 saw GPSADDR =
 *      0x803CC000 and SGE[0] = 0x803C0000 -- window VAs -- where at M the title
 *      plausibly writes 0x003CC000 and 0x003C0000 instead, because it calls
 *      MmGetPhysicalAddress (ordinal 173, IAT slot 0x001C40E8) on the
 *      DSOUND path.
 *   2. otherwise, P < xbox_ContiguousAllocatedBytes() (the high-water mark
 *      g_contig_next - XBOX_CONTIG_BASE, src/kernel/xbox_memory_layout.c:2694,
 *      i.e. everything some MmAllocateContiguousMemory call returned) means the
 *      translation is XBOX_CONTIG_BASE + P.
 *   3. otherwise, a mapped low-RAM range is identity.
 *   4. otherwise, fail closed.
 *
 * THE WINDOW-VA TEST PRECEDES THE HIGH-WATER TEST, and that ordering is
 * load-bearing. The toolkit precedent dma_resolve (src/kernel/nv2a_pb_exec.c:88)
 * puts the high-water test first and is correct to: its input is ALWAYS a
 * physical offset, because NV097_SET_SURFACE_COLOR_OFFSET is an offset inside a
 * DMA object. This function's input may already be a window VA. Given the
 * window VA 0x8000A6C0 with a high-water mark above 0x00A6C0, the high-water
 * test would return 0x8000A6C0 + 0x80000000 -- a double translation into
 * unmapped space. Case 1 prevents exactly that.
 *
 * dma_resolve's surface_hits_image() test is an NV2A/framebuffer concern -- it
 * exists to stop the pushbuffer executor clearing 1.2 MB of black through the
 * guest heap, because a physical offset "looked like an ordinary VA" when it
 * happened to clear the loaded image by 700 KB -- and is NOT imported here.
 * Case 4 is its analogue: refuse, and say so.
 *
 * No & 0x03FFFFFF anywhere in this function. Masking to 26 bits is what the
 * pinned xemu code does (it has one flat 64 MB physical space and no window),
 * and it is exactly the bug Device semantics 3 exists to fix: for the window VA
 * 0x803C0000 it would read low-RAM 0x003C0000 instead, which is a different
 * 4 KB in a machine that keeps the window as separate storage. (The
 * & 0x03FFFFFF sites in apu_shim.h and apu_vp.c are VP code, which Device
 * semantics 3 leaves untouched -- a recorded lead, not scope.)
 *
 * It returns a host pointer only when the whole range lies in a mapped guest
 * region. host(va) = g_apu_ram_ptr + va holds for BOTH low RAM and the window:
 * g_apu_ram_ptr is xbox_GetMemoryBase() == g_memory_base, and the window is
 * mapped at XBOX_CONTIG_BASE + g_memory_offset with g_memory_offset ==
 * g_memory_base (XBOX_MAP_START is 0), so g_memory_base + (0x80000000 + P) is
 * the window's byte P.
 *
 * Claim limit: physical/VA aliasing is not modelled. A title that passes a
 * low-RAM VA below the contiguous high-water mark is misrouted into the window,
 * exactly as dma_resolve would do. That case increments GPDMA_AMBIGUOUS, which
 * is observation plus this claim limit and nothing else -- no row reads it.
 */
uint8_t *apu_guest_dma_ptr(uint32_t addr, uint32_t len, uint32_t *translated_va)
{
    uint32_t t;
    size_t mapped = xbox_GetMappedSize();
    uint32_t high_water = xbox_ContiguousAllocatedBytes();

    if (translated_va) {
        *translated_va = addr;
    }
    if (!g_apu_ram_ptr) {
        return NULL;
    }

    /* Case 1: already a window VA. Used as is -- this is the 0d7929c form. */
    if (addr >= XBOX_CONTIG_BASE &&
        (uint64_t)addr + len <=
            (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE) {
        t = addr;
    }
    /* Case 2: a physical offset this runtime handed out as contiguous memory. */
    else if (high_water != 0 && addr < high_water &&
             (uint64_t)addr + len <= (uint64_t)XBOX_CONTIG_SIZE) {
        t = XBOX_CONTIG_BASE + addr;
        /* Ambiguous when the same range is also mapped low RAM: the two forms
         * are indistinguishable here and this chose the window. */
        if (mapped != 0 && (uint64_t)addr + len <= (uint64_t)mapped) {
            InterlockedIncrement((volatile LONG *)&s.gpdma_ambiguous);
        }
    }
    /* Case 3: a mapped low-RAM range is identity. */
    else if (mapped != 0 && (uint64_t)addr + len <= (uint64_t)mapped) {
        t = addr;
    }
    /* Case 4: fail closed. */
    else {
        note_unmapped(addr, len);
        return NULL;
    }

    if (translated_va) {
        *translated_va = t;
    }
    return g_apu_ram_ptr + t;
}

/* ============================================================
 * Device semantics 5 -- the ONE GP DMA write choke point
 * ============================================================ */

/*
 * Every GP DMA write to guest memory, from every pinned write callback, passes
 * through this function.
 *
 * `dst` is the translated host destination, `src` the DSP-side data the
 * transfer carries, `guest_va` the translated guest VA of the transfer's first
 * byte, `len` its length, and `dsp_addr` the DSP-side address of the transfer's
 * first word. (Device semantics 5 names the destination start, the length and
 * the DSP-side address as what the choke point receives; `src` carries the data
 * that the same sentence's "the payload for that dword" is read from.)
 *
 * If the write covers the aligned dword W_va and the payload for that dword is
 * 0, that dword is NEVER written with an ordinary store. Instead:
 *
 *   1. o = InterlockedCompareExchange(host(W_va), 0, 3).
 *      If o == 3 the exchange succeeded; classify on observed = 3.
 *   2. If o == 0, the dword already holds the payload: skip the write;
 *      classify on observed = 0.
 *   3. Otherwise o2 = InterlockedCompareExchange(host(W_va), 0, o). If o2 == o
 *      it succeeded; classify on observed = o. Else set o = o2 and repeat
 *      from 1.
 *
 * The rest of the transfer is written as usual, so guest memory ends up
 * identical to a plain write. This is always on.
 *
 * Why the retry closes advisory D1: a guest store of 3 landing between a failed
 * exchange and an ordinary write of 0 can no longer be overwritten
 * unclassified, because every 0 the GP lands on W_va is landed by an exchange
 * whose replaced value IS the classification.
 */
void apu_gp_dma_write(uint8_t *dst, const uint8_t *src, uint32_t guest_va,
                      size_t len, uint32_t dsp_addr)
{
    uint32_t w_va = watch_w_va();
    uint32_t seq = 0;
    uint32_t payload = 0, observed = 0, before = 0;
    int cas = -1;                       /* -1 n/a, 0 fail, 1 ok */
    int full_cover = 0, overlaps = 0;
    uint64_t end = (uint64_t)guest_va + len;

    s.gpdma_writes++;
    s.gpdma_wbytes += len;

    if (!dst || !src || len == 0) {
        return;
    }

    if (w_va != 0) {
        overlaps = (guest_va < (uint64_t)w_va + 4) && (end > w_va);
        full_cover = (guest_va <= w_va) && (end >= (uint64_t)w_va + 4);
    }

    if (overlaps && full_cover) {
        payload = ldl_le_p(src + (size_t)(w_va - guest_va));

        if (payload != 0) {
            /* GP_NONZERO_OVER: observed is the dword at W_va immediately before
             * the write; the write itself is ordinary. */
            volatile uint32_t *wp = (volatile uint32_t *)(g_apu_ram_ptr + w_va);
            before = *wp;
            seq = (uint32_t)InterlockedIncrement(&s.seq);
            memcpy(dst, src, len);
            record_latch(APU_WATCH_GP_NONZERO_OVER, seq, w_va, before, payload,
                         0, dsp_addr);
            apu_watch_trace_gpdma_watch(w_va, before, payload, before, -1,
                                        dsp_addr);
            return;
        }

        /* Zero payload covering the dword: the exchange owns this dword. */
        {
            volatile LONG *wp = (volatile LONG *)(g_apu_ram_ptr + w_va);
            LONG o;

            before = (uint32_t)*wp;
            seq = (uint32_t)InterlockedIncrement(&s.seq);

            for (;;) {
                o = InterlockedCompareExchange(wp, 0, 3);
                if (o == 3) {
                    observed = 3;
                    cas = 1;
                    break;
                }
                if (o == 0) {
                    observed = 0;
                    cas = 0;
                    break;
                }
                {
                    LONG o2 = InterlockedCompareExchange(wp, 0, o);
                    if (o2 == o) {
                        observed = (uint32_t)o;
                        cas = 1;
                        break;
                    }
                    o = o2;         /* repeat from 1 */
                }
            }
        }

        /* The dword at W_va belongs to the exchange: it is never ordinary-
         * stored. Everything before and after it is written as usual, so the
         * guest ends up with the bytes a plain write would have produced. */
        {
            size_t head = (size_t)(w_va - guest_va);
            if (head) {
                memcpy(dst, src, head);
            }
            if (len > head + 4) {
                memcpy(dst + head + 4, src + head + 4, len - head - 4);
            }
        }

        if (observed == 3) {
            record_latch(APU_WATCH_GP_CLEAR, seq, w_va, 3, 0, 0, dsp_addr);
        } else if (observed == 0) {
            record_latch(APU_WATCH_GP_ZERO_OVER_ZERO, seq, w_va, 0, 0, 0,
                         dsp_addr);
        } else {
            record_latch(APU_WATCH_GP_ZERO_OVER_OTHER, seq, w_va, observed, 0, 0,
                         dsp_addr);
        }
        apu_watch_trace_gpdma_watch(w_va, before, payload, observed, cas,
                                    dsp_addr);
        return;
    }

    if (overlaps) {
        /* GP_PARTIAL: overlaps W_va's four bytes without covering the aligned
         * dword. Not exchange-protected, so it is an unattributable writer; it
         * is made visible rather than assumed away. observed is the dword
         * before the write, payload the dword W_va would hold after it. */
        volatile uint32_t *wp = (volatile uint32_t *)(g_apu_ram_ptr + w_va);
        uint32_t after;

        before = *wp;
        seq = (uint32_t)InterlockedIncrement(&s.seq);
        memcpy(dst, src, len);
        after = *wp;
        record_latch(APU_WATCH_GP_PARTIAL, seq, w_va, before, after, 0,
                     dsp_addr);
        apu_watch_trace_gpdma_watch(w_va, before, after, before, -1, dsp_addr);
        return;
    }

    memcpy(dst, src, len);
}

/* ============================================================
 * The read counterpart
 * ============================================================ */

void apu_gp_dma_read(const uint8_t *src, uint32_t guest_va, size_t len,
                     uint32_t dsp_addr)
{
    int cls;

    s.gpdma_reads++;
    s.gpdma_rbytes += len;

    /* guest_va is apu_guest_dma_ptr()'s output, so the region class is decided
     * on the translated address (Device semantics 6). */
    cls = dma_class_of(guest_va);
    s.gpin.dma[cls].reads++;
    s.gpin.dma[cls].bytes += len;
    if (!s.gpin.dma[cls].have_first_va) {
        s.gpin.dma[cls].first_va = guest_va;
        s.gpin.dma[cls].have_first_va = 1;
    }

    /* Capped observation list, for the A4c brief. No row may read it. */
    {
        uint32_t i, page = guest_va & ~0xFFFu;
        for (i = 0; i < s.obs.n_pages; i++) {
            if (s.obs.page[i].va == page) {
                s.obs.page[i].count++;
                return;
            }
        }
        if (s.obs.n_pages < APU_WATCH_OBS_PAGES) {
            s.obs.page[s.obs.n_pages].va = page;
            s.obs.page[s.obs.n_pages].count = 1;
            s.obs.n_pages++;
        }
    }
}

/* The bootstrap's scratch DATA read. Device semantics 6 gives this its own
 * write-once latch, set in the bootstrap path. It is deliberately not the SGE
 * descriptor fetch: that is a 4-byte read of the SGE table, and latching it
 * would record the table's address rather than the page the bootstrap loaded. */
void apu_watch_boot_scratch_read(uint32_t guest_va, uint32_t first_dword,
                                 size_t len, uint32_t dsp_addr)
{
    uint32_t seq;

    if (!s.in_bootstrap) {
        return;
    }
    seq = (uint32_t)InterlockedIncrement(&s.seq);
    InterlockedIncrement(&s.counts[APU_WATCH_C_BOOT_SCRATCH_READ]);
    if (latch_fire(&s.latch[APU_WATCH_BOOT_SCRATCH_READ], seq, guest_va,
                   first_dword, (uint32_t)len, 0, s.gp_insns, dsp_addr)) {
        /* Mirror the class latch into the `gpin` slot. The `[GPIN] summary`
         * line prints `g->boot_scratch_read.latched` (emit_gpin_block), and the
         * snapshot carries that slot, so firing only the class latch would
         * leave the printed presence witness permanently 0 -- and AC-FIX (vi)
         * reads it. Written under the same write-once guard, so it freezes
         * exactly when the class latch does and the at_clear copy (which is a
         * memcpy of this whole struct) stays consistent with it. */
        s.gpin.boot_scratch_read = s.latch[APU_WATCH_BOOT_SCRATCH_READ];
        after_latch_fired(APU_WATCH_BOOT_SCRATCH_READ);
    }
}

/* ============================================================
 * Device semantics 6 -- GP input recording hooks
 * ============================================================ */

void apu_gpin_periph_read(uint32_t offset, uint32_t value)
{
    if (offset >= APU_WATCH_DSP_PERIPH_SIZE) {
        apu_gpin_record_out_of_universe(APU_WATCH_GPIN_PERIPH, offset);
        return;
    }

    s.gpin.periph[offset].reads++;
    if (s.gpin.periph[offset].reads == 1) {
        s.gpin.periph[offset].first_value = value;
        s.gpin.periph[offset].first_seq = (uint32_t)s.seq;
    }

    /* Capped observation list; no row may read it. */
    {
        uint32_t i;
        for (i = 0; i < s.obs.n_periph; i++) {
            if (s.obs.periph[i].offset == offset) {
                return;
            }
        }
        if (s.obs.n_periph < APU_WATCH_OBS_PERIPH) {
            s.obs.periph[s.obs.n_periph].offset = offset;
            s.obs.periph[s.obs.n_periph].first_value = value;
            s.obs.n_periph++;
        }
    }
}

void apu_gp_mixbuf_frame_begin(uint32_t vp_active_voices)
{
    s.mixbuf_vp_active_voices = vp_active_voices;
    s.mixbuf_stub = (vp_active_voices > 0) ? 1u : 0u;
}

void apu_gp_mixbuf_note_sample(uint32_t word)
{
    /* Conservative against the test tone and any other writer: a non-zero
     * sample anywhere in the frame's mix-buffer content makes the frame's
     * provenance "stub content". */
    if (word != 0) {
        s.mixbuf_stub = 1;
    }
}

void apu_gpin_mixbuf_read(uint32_t offset, uint32_t value)
{
    uint32_t bin;

    if (offset >= DSP_MIXBUFFER_SIZE) {
        apu_gpin_record_out_of_universe(APU_WATCH_GPIN_MIXBUF, offset);
        return;
    }
    bin = offset / NUM_SAMPLES_PER_FRAME;

    s.gpin.mixbuf[bin].reads++;
    if (s.mixbuf_stub) {
        uint32_t seq = (uint32_t)InterlockedIncrement(&s.seq);
        s.gpin.mixbuf[bin].reads_while_stub++;
        InterlockedIncrement(&s.counts[APU_WATCH_C_MIXBUF_STUB_READ]);
        if (latch_fire(&s.latch[APU_WATCH_MIXBUF_STUB_READ], seq,
                       GP_DSP_MIXBUF_BASE + offset, s.mixbuf_vp_active_voices,
                       value, 0, s.gp_insns, 0)) {
            /* Mirror into the `gpin` slot, for the same reason as
             * apu_watch_boot_scratch_read(): `[GPIN] summary` prints the slot,
             * and the snapshot carries it. */
            s.gpin.mixbuf_stub_read = s.latch[APU_WATCH_MIXBUF_STUB_READ];
            after_latch_fired(APU_WATCH_MIXBUF_STUB_READ);
        }
    }
}

void apu_gpin_fifo_read(uint32_t fifo, size_t bytes)
{
    if (fifo >= APU_WATCH_FIFO_COUNT) {
        apu_gpin_record_out_of_universe(APU_WATCH_GPIN_FIFO, fifo);
        return;
    }
    s.gpin.fifo[fifo].reads++;
    s.gpin.fifo[fifo].words += (uint32_t)(bytes / 4);
}

void apu_gpin_record_out_of_universe(uint32_t kind, uint32_t index)
{
    uint32_t seq;

    /* A bug detector, not a volume guard: no run length and no input volume can
     * raise it. It is reachable only by a record call whose kind or index is
     * out of range (for example a peripheral offset >= 128). */
    InterlockedIncrement(&s.counts[APU_WATCH_C_GPIN_OUT_OF_UNIVERSE]);
    s.gpin.out_of_universe++;
    seq = (uint32_t)InterlockedIncrement(&s.seq);
    if (latch_fire(&s.latch[APU_WATCH_GPIN_OUT_OF_UNIVERSE], seq, index, kind, 0,
                   0, s.gp_insns, 0)) {
        after_latch_fired(APU_WATCH_GPIN_OUT_OF_UNIVERSE);
    }
}

/* ============================================================
 * Run counters
 * ============================================================ */

void apu_watch_gp_bootstrap(int is_gp)
{
    if (!is_gp) {
        return;
    }
    s.boots++;
    s.gp_frames = 0;
    s.gp_insns = 0;
}

/* Emitted AFTER the bootstrap has finished, so the counts line and the [GPIN]
 * summary carry the bootstrap's own reads (Device semantics 6: "at each
 * bootstrap, after the run counters reset"). Calling it at the start of the
 * bootstrap would print the pre-bootstrap state, which is not what the line is
 * for -- and AC-FIX (vi) decides on a [GPIN] summary whose DMA_READ class for
 * the scratch page is non-zero, which only holds if the emission follows the
 * read. */
void apu_watch_gp_bootstrap_done(int is_gp)
{
    if (!is_gp) {
        return;
    }
    emit_counts();
    emit_gpin_summary();
}

void apu_watch_gp_frame(int is_gp)
{
    if (!is_gp) {
        return;
    }
    s.gp_frames++;

    if (s.gp_frames == 1 || (s.gp_frames % 256) == 0) {
        emit_counts();
        emit_gpin_summary();
    }
}

void apu_watch_gp_insns_add(int is_gp, uint32_t cycles)
{
    if (is_gp) {
        s.gp_insns += cycles;
    }
}

void apu_watch_bootstrap_begin(void) { s.in_bootstrap = 1; }
void apu_watch_bootstrap_end(void)   { s.in_bootstrap = 0; }

/* ============================================================
 * Device semantics 6 -- CPU side
 * ============================================================ */

void apu_watch_set_anchor_site(uint32_t site_va)
{
    s.anchor_site = site_va;
}

void apu_watch_cpu_store(uint32_t site_va, uint32_t target_va, uint32_t value)
{
    uint32_t w_va = watch_w_va();
    uint32_t seq;
    int i;

    /* Any call for another target is ignored: no counter, no line, no seq. */
    if (w_va == 0 || target_va != w_va) {
        return;
    }

    seq = (uint32_t)InterlockedIncrement(&s.seq);

    if (value == 3 && site_va == s.anchor_site) {
        /* CPU_ANCHOR: a store of 3 from the anchor site. */
        InterlockedIncrement(&s.counts[APU_WATCH_C_CPU_ANCHOR]);
        if (latch_fire(&s.latch[APU_WATCH_CPU_ANCHOR], seq, w_va, 0, value,
                       site_va, 0, 0)) {
            after_latch_fired(APU_WATCH_CPU_ANCHOR);
        }
        return;
    }

    if (value == 0) {
        /* CPU_ZERO, latched per site. The universe is the enumerated
         * jsrf_watch_store call sites: finite, source-enumerated, independent of
         * run length. A site beyond N_SITES can only be an unenumerated one,
         * which is why CPU_ZERO_OVERFLOW is a bug detector. */
        InterlockedIncrement(&s.counts[APU_WATCH_C_CPU_ZERO]);

        for (i = 0; i < APU_WATCH_N_SITES; i++) {
            if (s.sites[i].used && s.sites[i].site_va == site_va) {
                if (latch_fire(&s.sites[i].zero, seq, w_va, 0, 0, site_va, 0,
                               0)) {
                    emit_latch(class_name[APU_WATCH_CPU_ZERO],
                               &s.sites[i].zero);
                    emit_counts();
                }
                return;
            }
        }
        for (i = 0; i < APU_WATCH_N_SITES; i++) {
            if (!s.sites[i].used) {
                s.sites[i].used = 1;
                s.sites[i].site_va = site_va;
                if (latch_fire(&s.sites[i].zero, seq, w_va, 0, 0, site_va, 0,
                               0)) {
                    emit_latch(class_name[APU_WATCH_CPU_ZERO],
                               &s.sites[i].zero);
                    emit_counts();
                }
                return;
            }
        }

        InterlockedIncrement(&s.counts[APU_WATCH_C_CPU_ZERO_OVERFLOW]);
        if (latch_fire(&s.latch[APU_WATCH_CPU_ZERO_OVERFLOW], seq, w_va, 0, 0,
                       site_va, 0, 0)) {
            after_latch_fired(APU_WATCH_CPU_ZERO_OVERFLOW);
        }
        return;
    }

    /* CPU_OTHER: counted only, no latch. */
    InterlockedIncrement(&s.counts[APU_WATCH_C_CPU_OTHER]);
}

/* ============================================================
 * at_clear freeze (Device semantics 6)
 * ============================================================ */

void apu_watch_freeze_at_clear(uint32_t seq)
{
    if (InterlockedCompareExchange((volatile LONG *)&s.at_clear.taken, 1, 0)
        != 0) {
        return;
    }
    s.at_clear.seq = seq;
    s.at_clear.frame = s.se_frame;
    memcpy(&s.at_clear.gpin, &s.gpin, sizeof(s.gpin));
}

/* ============================================================
 * Emission (Device semantics 6 and 7)
 * ============================================================ */

static void emit_counts(void)
{
    if (!apu_watch_trace_enabled()) {
        return;
    }
    fprintf(stderr,
            "[GPWATCH] counts seq=%u boots=%u gp_frames=%u gp_insns=%llu "
            "GP_CLEAR=%u GP_ZERO_OVER_ZERO=%u GP_ZERO_OVER_OTHER=%u "
            "GP_NONZERO_OVER=%u GP_PARTIAL=%u CPU_ANCHOR=%u CPU_ZERO=%u "
            "CPU_ZERO_OVERFLOW=%u CPU_OTHER=%u GPIN_OUT_OF_UNIVERSE=%u "
            "frame=%u\n",
            (uint32_t)s.seq, s.boots, s.gp_frames,
            (unsigned long long)s.gp_insns,
            (uint32_t)s.counts[APU_WATCH_C_GP_CLEAR],
            (uint32_t)s.counts[APU_WATCH_C_GP_ZERO_OVER_ZERO],
            (uint32_t)s.counts[APU_WATCH_C_GP_ZERO_OVER_OTHER],
            (uint32_t)s.counts[APU_WATCH_C_GP_NONZERO_OVER],
            (uint32_t)s.counts[APU_WATCH_C_GP_PARTIAL],
            (uint32_t)s.counts[APU_WATCH_C_CPU_ANCHOR],
            (uint32_t)s.counts[APU_WATCH_C_CPU_ZERO],
            (uint32_t)s.counts[APU_WATCH_C_CPU_ZERO_OVERFLOW],
            (uint32_t)s.counts[APU_WATCH_C_CPU_OTHER],
            (uint32_t)s.counts[APU_WATCH_C_GPIN_OUT_OF_UNIVERSE],
            s.se_frame);
    fflush(stderr);
}

/* One line per kind, each carrying EVERY element of that kind's fixed array.
 * The number of lines per emission is fixed, so nothing can be lost, and there
 * are no per-key lines. */
static void emit_gpin_block(const char *tag, const struct apu_watch_gpin *g)
{
    int i;

    fprintf(stderr, "[GPIN] %s mixbuf reads=[", tag);
    for (i = 0; i < APU_WATCH_NUM_MIXBINS; i++) {
        fprintf(stderr, "%s%u", i ? "," : "", g->mixbuf[i].reads);
    }
    fprintf(stderr, "] reads_while_stub=[");
    for (i = 0; i < APU_WATCH_NUM_MIXBINS; i++) {
        fprintf(stderr, "%s%u", i ? "," : "", g->mixbuf[i].reads_while_stub);
    }
    fprintf(stderr, "]\n");

    fprintf(stderr, "[GPIN] %s periph reads=[", tag);
    for (i = 0; i < APU_WATCH_DSP_PERIPH_SIZE; i++) {
        fprintf(stderr, "%s%u", i ? "," : "", g->periph[i].reads);
    }
    fprintf(stderr, "] first_value=[");
    for (i = 0; i < APU_WATCH_DSP_PERIPH_SIZE; i++) {
        fprintf(stderr, "%s%06X", i ? "," : "", g->periph[i].first_value);
    }
    fprintf(stderr, "] first_seq=[");
    for (i = 0; i < APU_WATCH_DSP_PERIPH_SIZE; i++) {
        fprintf(stderr, "%s%u", i ? "," : "", g->periph[i].first_seq);
    }
    fprintf(stderr, "]\n");

    fprintf(stderr, "[GPIN] %s fifo reads=[", tag);
    for (i = 0; i < APU_WATCH_FIFO_COUNT; i++) {
        fprintf(stderr, "%s%u", i ? "," : "", g->fifo[i].reads);
    }
    fprintf(stderr, "] words=[");
    for (i = 0; i < APU_WATCH_FIFO_COUNT; i++) {
        fprintf(stderr, "%s%u", i ? "," : "", g->fifo[i].words);
    }
    fprintf(stderr, "]\n");

    fprintf(stderr, "[GPIN] %s dma reads=[", tag);
    for (i = 0; i < APU_WATCH_DMA_CLASSES; i++) {
        fprintf(stderr, "%s%llu", i ? "," : "",
                (unsigned long long)g->dma[i].reads);
    }
    fprintf(stderr, "] bytes=[");
    for (i = 0; i < APU_WATCH_DMA_CLASSES; i++) {
        fprintf(stderr, "%s%llu", i ? "," : "",
                (unsigned long long)g->dma[i].bytes);
    }
    fprintf(stderr, "] first_va=[");
    for (i = 0; i < APU_WATCH_DMA_CLASSES; i++) {
        fprintf(stderr, "%s%08X%s", i ? "," : "", g->dma[i].first_va,
                g->dma[i].have_first_va ? "" : "(none)");
    }
    fprintf(stderr, "]\n");

    fprintf(stderr,
            "[GPIN] %s flags out_of_universe=%u mixbuf_stub_read=%u "
            "boot_scratch_read=%u\n",
            tag, g->out_of_universe, g->mixbuf_stub_read.latched,
            g->boot_scratch_read.latched);
    fflush(stderr);
}

static void emit_gpin_summary(void)
{
    if (!apu_watch_trace_enabled()) {
        return;
    }
    emit_gpin_block("summary", &s.gpin);
    if (s.at_clear.taken) {
        fprintf(stderr, "[GPIN] at_clear seq=%u frame=%u\n", s.at_clear.seq,
                s.at_clear.frame);
        emit_gpin_block("at_clear", &s.at_clear.gpin);
    }
}

/* ============================================================
 * Frame tick and Device semantics 7 trace lines
 * ============================================================ */

void apu_watch_frame_tick(void)
{
    s.se_frame++;

    /* every 256th se_frame */
    if ((s.se_frame % 256) == 0) {
        emit_counts();
        emit_gpin_summary();
    }
}

void apu_watch_trace_gprun(uint32_t cycles, uint32_t pc, int halt)
{
    if (!apu_watch_trace_enabled()) {
        return;
    }
    /* First 8 GP frames after each bootstrap, then every 256th GP frame. */
    if (!(s.gp_frames <= 8 || (s.gp_frames % 256) == 0)) {
        return;
    }
    fprintf(stderr,
            "[GPRUN] frame=%u se_frame_after_boot=%u cycles=%u insns=%llu "
            "pc=%06X halt=%d tone=%d\n",
            s.se_frame, s.gp_frames, cycles,
            (unsigned long long)s.gp_insns, pc & 0xFFFFFFu, halt,
            mcpx_apu_test_tone_active());
    fflush(stderr);
}

void apu_watch_trace_gpdma_frame(void)
{
    if (!apu_watch_trace_enabled()) {
        return;
    }
    if (!((s.gp_frames % 256) == 0)) {
        return;
    }
    fprintf(stderr,
            "[GPDMA] frame=%u reads=%u writes=%u rbytes=%llu wbytes=%llu\n",
            s.se_frame, (uint32_t)s.gpdma_reads, (uint32_t)s.gpdma_writes,
            (unsigned long long)s.gpdma_rbytes,
            (unsigned long long)s.gpdma_wbytes);
    fflush(stderr);
}

/* [GPDMA] watch: per covering write, for the first 16 since the latest
 * apu_watch_reset(). Observation only, with no exemption rule and no cap
 * line. */
void apu_watch_trace_gpdma_watch(uint32_t w_va, uint32_t before,
                                 uint32_t payload, uint32_t observed, int cas,
                                 uint32_t dsp_addr)
{
    if (!apu_watch_trace_enabled() || !g_apu_ram_ptr) {
        return;
    }
    if (s.gpdma_watch_lines >= 16) {
        return;
    }
    s.gpdma_watch_lines++;
    fprintf(stderr,
            "[GPDMA] watch va=%08X before=%08X payload=%08X after=%08X "
            "cas=%s observed=%08X dsp_addr=%06X frame=%u insns=%llu\n",
            w_va, before, payload,
            *(volatile uint32_t *)(g_apu_ram_ptr + w_va),
            cas < 0 ? "n/a" : (cas ? "ok" : "fail"), observed, dsp_addr,
            s.se_frame, (unsigned long long)s.gp_insns);
    fflush(stderr);
}

void apu_watch_trace_gpboot(uint32_t gprst, uint32_t prev, uint32_t sge0,
                            uint32_t sge0_va, const uint32_t *pram,
                            uint32_t pram_words)
{
    uint32_t i;

    if (!apu_watch_trace_enabled()) {
        return;
    }
    fprintf(stderr,
            "[GPBOOT] n=%u sge0=%08X sge0_va=%08X gprst=%08X prev=%08X\n",
            s.boots, sge0, sge0_va, gprst, prev);
    for (i = 0; i + 8 <= pram_words; i += 8) {
        fprintf(stderr,
                "[GPBOOT] pram %03X: %06X %06X %06X %06X %06X %06X %06X "
                "%06X\n",
                i, pram[i + 0] & 0xFFFFFFu, pram[i + 1] & 0xFFFFFFu,
                pram[i + 2] & 0xFFFFFFu, pram[i + 3] & 0xFFFFFFu,
                pram[i + 4] & 0xFFFFFFu, pram[i + 5] & 0xFFFFFFu,
                pram[i + 6] & 0xFFFFFFu, pram[i + 7] & 0xFFFFFFu);
    }
    fflush(stderr);
}

/* ============================================================
 * Accessors
 * ============================================================ */

void apu_watch_snapshot(struct apu_watch_snapshot *out)
{
    int i;

    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    out->seq = (uint32_t)s.seq;
    out->frame = s.se_frame;
    for (i = 0; i < APU_WATCH_COUNTERS; i++) {
        out->counts[i] = (uint32_t)s.counts[i];
    }
    memcpy(out->latch, s.latch, sizeof(out->latch));
    memcpy(out->sites, s.sites, sizeof(out->sites));
    out->boots = s.boots;
    out->gp_frames = s.gp_frames;
    out->gp_insns = s.gp_insns;
    out->gpdma_ambiguous = s.gpdma_ambiguous;
    out->gpdma_reads = s.gpdma_reads;
    out->gpdma_writes = s.gpdma_writes;
    out->gpdma_rbytes = s.gpdma_rbytes;
    out->gpdma_wbytes = s.gpdma_wbytes;
    out->gpdma_watch_lines = s.gpdma_watch_lines;
    out->anchor_site = s.anchor_site;
    memcpy(&out->gpin, &s.gpin, sizeof(out->gpin));
    memcpy(&out->at_clear, &s.at_clear, sizeof(out->at_clear));
    memcpy(&out->obs, &s.obs, sizeof(out->obs));
}

void apu_watch_reset(void)
{
    /* Everything, including the [GPIN] arrays and latches, the at_clear block,
     * GPDMA_AMBIGUOUS, the process-scoped [GPDMA] watch line count and the
     * [GPDMA] unmapped once-per-address set. After it, no trace or ledger state
     * from an earlier case survives.
     *
     * GP core state is NOT reset by it (follow-up D4): cases that bootstrap
     * reset that through GPRST. */
    InterlockedExchange(&s.seq, 0);
    memset((void *)s.counts, 0, sizeof(s.counts));
    memset(s.latch, 0, sizeof(s.latch));
    memset(s.sites, 0, sizeof(s.sites));
    s.boots = 0;
    s.gp_frames = 0;
    s.gp_insns = 0;
    s.gpdma_ambiguous = 0;
    s.gpdma_reads = 0;
    s.gpdma_writes = 0;
    s.gpdma_rbytes = 0;
    s.gpdma_wbytes = 0;
    s.gpdma_watch_lines = 0;
    s.anchor_site = 0;
    s.se_frame = 0;
    memset(&s.gpin, 0, sizeof(s.gpin));
    memset(&s.at_clear, 0, sizeof(s.at_clear));
    memset(&s.obs, 0, sizeof(s.obs));
    s.mixbuf_stub = 0;
    s.mixbuf_vp_active_voices = 0;
    s.in_bootstrap = 0;
    s.unmapped_n = 0;
    memset(s.unmapped_set, 0, sizeof(s.unmapped_set));
}
