/*
 * MCPX APU watched-word ledger, GP DMA choke point and GP input accounting.
 *
 * A4b1 step 4 (Device semantics 3, 5 and 6). This is the EXPORTED toolkit
 * header: the fixture (step 8) includes it, and the game-side forwarder
 * (A4b2 step 1) calls apu_watch_cpu_store() through it. Nothing here is a
 * log line: the ledger is a small device-side struct updated at the event by
 * the code that performs the event, so it cannot lose the deciding event.
 *
 * Design authority: docs/reviews/a4b-watch-ledger-ruling.md (Device semantics
 * 5, 6, 7) and docs/reviews/a4b-gpin-accounting-ruling.md (the finite-universe
 * input accounting). The retired r3 mechanism -- the 256-entry table, the
 * GPIN_OVERFLOW latch, per-key [GPIN] lines and the cut-off -- does not appear
 * here or in apu_watch.c, by ruling.
 *
 * Copyright (c) 2026 Burnout 3 Static Recompilation Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef MCPX_APU_WATCH_H
#define MCPX_APU_WATCH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "apu_regs.h"
#include "dsp/interp/dsp_cpu_regs.h"

/* ============================================================
 * Finite universes (docs/agent-workflow.md 6.1 item 6(b))
 *
 * Every array below is sized by one of these, and every one of them is derived
 * from source rather than from a run. A counter that could overflow because the
 * run was long or busy would mean the key is wrong.
 * ============================================================ */

/* MIXBUF: one entry per mixbin. NUM_MIXBINS (apu_regs.h:334) = 32, and the
 * pinned frame path writes 32 bins x NUM_SAMPLES_PER_FRAME words into GP X
 * memory at GP_DSP_MIXBUF_BASE, so bin = (addr - 0x1400) / 32. */
#define APU_WATCH_NUM_MIXBINS NUM_MIXBINS

/* PERIPH: the DSP peripheral file. DSP_PERIPH_SIZE (dsp_cpu_regs.h:121) = 128,
 * covering DSP_PERIPH_BASE (0xFFFF80) .. 0xFFFFFF. */
#define APU_WATCH_DSP_PERIPH_SIZE DSP_PERIPH_SIZE

/* FIFO: GP_OUTPUT_FIFO_COUNT (4) + GP_INPUT_FIFO_COUNT (2) = 6, apu_regs.h.
 *
 * STATIC PER-FIFO SOURCE CLASSIFICATION (Device semantics 6; AC-PORT step 4).
 * The meaning of all six indices, decided once from the pinned source:
 *
 *   slots 0..GP_INPUT_FIFO_COUNT-1 (0..1) -- the GP INPUT FIFOs. Their data
 *     source is "none modelled at the pin": the pinned read arm does not
 *     implement buf_id 0..3, so a read-direction transfer naming one of them
 *     falls through (its `assert` is NDEBUG-elided) and the DSP consumes the
 *     stale intermediate buffer instead. Classified STUB/UNKNOWN, so any count
 *     > 0 is AC-INPUTS FAIL, which is R2-EXPL-INPUT in A4b2. Written by the
 *     read-arm hook in dsp_dma.c (gated on the DMA's is_gp) and by the
 *     gp_fifo_rw hook under !dir (gp_ep.c:251).
 *
 *   slots GP_INPUT_FIFO_COUNT..APU_WATCH_FIFO_COUNT-1 (2..5) -- the output
 *     FIFOs' reserved places in the declared 6-element universe. They are
 *     GP-PRODUCED, not inputs, so they are NEVER WRITTEN. The fixture asserts
 *     they are 0, as a guard against a future hook recording outputs here.
 *
 * The two index spaces are per-direction and COLLIDE: in gp_fifo_rw an output
 * fifo 0 and an input fifo 0 both arrive as index 0 (gp_ep.c:219-233). That is
 * why the read arm uses buf_id directly for the input slots while the output
 * slots stay empty, rather than one flat index.
 *
 * INFERENCE, stated: that a read-arm buf_id of 0..1 denotes input FIFO 0..1
 * mirrors the write arm's buf_id 0..3 -> output FIFO, and no pinned source
 * states it. The classification does not depend on it: the consumed data is the
 * stale buffer whatever the id means, and both input slots are stub/unknown, so
 * any count > 0 fails AC-INPUTS either way. */
#define APU_WATCH_FIFO_COUNT (GP_OUTPUT_FIFO_COUNT + GP_INPUT_FIFO_COUNT)

/* DMA region classes: LOW_RAM, CONTIG, DEVICE, OTHER_MAPPED. */
#define APU_WATCH_DMA_CLASSES 4

/* CPU-site table.
 *
 * N_SITES is the size of the CPU_ZERO per-site latch table. Its universe is the
 * enumerated jsrf_watch_store call sites (A4b2 step 1), which is finite,
 * source-enumerated and independent of run length -- that is the premise the
 * ledger ruling's retroactive note requires, and the reason CPU_ZERO_OVERFLOW
 * is a bug detector rather than a volume guard.
 *
 * The count at the A4b1 baseline is 6, measured on the game tree at the A4b1
 * build commit (all four are `MEM32(...) =` stores; `+0x810` is the DSP pending
 * word at MEM32(0x001BA858)+0x810):
 *
 *   site VA      guest instruction                 generated store
 *   0x001A18CE   mov [ebx], eax                     recomp_0005.c:6746
 *   0x001A1751   and dword ptr [edi+0x810], 0       recomp_0005.c:6524
 *   0x001A1FA7   mov [edi+0x10], ebp                recomp_0005.c:8088
 *   (unlabelled) MEM32(.* + 0x810) =                recomp_0000.c:135283
 *   (unlabelled) MEM32(.* + 0x810) =                recomp_0000.c:135406
 *   (unlabelled) MEM32(.* + 0x810) =                recomp_0000.c:135871
 *
 * The ledger ruling's own text allowed "say 16 entries"; 16 >= 6, so N_SITES is
 * 16 and the count is stated here, which is what the retroactive note asks for.
 * A store from a site that is not among the first N_SITES distinct sites seen
 * latches CPU_ZERO_OVERFLOW -- reachable only by an unenumerated site.
 */
#define APU_WATCH_N_SITES 16

/* Capped observation lists (Device semantics 6, "observation, never decisive").
 * No criterion may read these; they exist for the A4c brief. */
#define APU_WATCH_OBS_PAGES 64
#define APU_WATCH_OBS_PERIPH 64

/* The once-per-address [GPDMA] unmapped set (Device semantics 3 case 4). */
#define APU_WATCH_UNMAPPED_SET 64

/* ============================================================
 * Classes
 * ============================================================ */

/* Latched classes. CPU_OTHER is deliberately absent: it is counted only. */
enum apu_watch_class {
    APU_WATCH_GP_CLEAR = 0,
    APU_WATCH_GP_ZERO_OVER_ZERO,
    APU_WATCH_GP_ZERO_OVER_OTHER,
    APU_WATCH_GP_NONZERO_OVER,
    APU_WATCH_GP_PARTIAL,
    APU_WATCH_CPU_ANCHOR,
    APU_WATCH_CPU_ZERO,          /* latched per site, see sites[] */
    APU_WATCH_CPU_ZERO_OVERFLOW,
    APU_WATCH_MIXBUF_STUB_READ,
    APU_WATCH_BOOT_SCRATCH_READ,
    APU_WATCH_GPIN_OUT_OF_UNIVERSE,
    APU_WATCH_LATCH_CLASSES
};

/* Counted classes: every latch class plus CPU_OTHER. */
enum apu_watch_counter {
    APU_WATCH_C_GP_CLEAR = 0,
    APU_WATCH_C_GP_ZERO_OVER_ZERO,
    APU_WATCH_C_GP_ZERO_OVER_OTHER,
    APU_WATCH_C_GP_NONZERO_OVER,
    APU_WATCH_C_GP_PARTIAL,
    APU_WATCH_C_CPU_ANCHOR,
    APU_WATCH_C_CPU_ZERO,
    APU_WATCH_C_CPU_ZERO_OVERFLOW,
    APU_WATCH_C_CPU_OTHER,
    APU_WATCH_C_MIXBUF_STUB_READ,
    APU_WATCH_C_BOOT_SCRATCH_READ,
    APU_WATCH_C_GPIN_OUT_OF_UNIVERSE,
    APU_WATCH_COUNTERS
};

/* DMA region classes, decided on the TRANSLATED address inside
 * apu_guest_dma_ptr() (Device semantics 3 / 6). */
enum apu_watch_dma_class {
    APU_WATCH_DMA_LOW_RAM = 0,
    APU_WATCH_DMA_CONTIG,
    APU_WATCH_DMA_DEVICE,
    APU_WATCH_DMA_OTHER_MAPPED
};

/* GP input record kinds, for the out-of-universe detector. */
enum apu_watch_gpin_kind {
    APU_WATCH_GPIN_MIXBUF = 0,
    APU_WATCH_GPIN_PERIPH,
    APU_WATCH_GPIN_FIFO,
    APU_WATCH_GPIN_DMA,
    APU_WATCH_GPIN_KINDS
};

/* ============================================================
 * Records
 * ============================================================ */

/* One write-once latch. `latched` is the InterlockedCompareExchange word: the
 * thread that takes it 0->1 fills in every field below and emits the line. */
struct apu_watch_latch {
    uint32_t latched;
    uint32_t seq;
    uint32_t va;
    uint32_t observed;
    uint32_t payload;
    uint32_t site;
    uint32_t frame;
    uint64_t insns;
    uint32_t dsp_addr;
};

/* One CPU site. `used` marks a taken slot; slots are assigned in arrival order,
 * because the toolkit hard-codes no site (Device semantics 6). */
struct apu_watch_site {
    uint32_t used;
    uint32_t site_va;
    struct apu_watch_latch zero;
};

/* ---- [GPIN] fixed arrays ---- */

struct apu_watch_gpin {
    struct {
        uint32_t reads;
        uint32_t reads_while_stub;
    } mixbuf[APU_WATCH_NUM_MIXBINS];

    struct {
        uint32_t reads;
        uint32_t first_value;
        uint32_t first_seq;
    } periph[APU_WATCH_DSP_PERIPH_SIZE];

    struct {
        uint32_t reads;
        uint32_t words;
    } fifo[APU_WATCH_FIFO_COUNT];

    struct {
        uint64_t reads;
        uint64_t bytes;
        uint32_t first_va;
        uint32_t have_first_va;
    } dma[APU_WATCH_DMA_CLASSES];

    struct apu_watch_latch mixbuf_stub_read;
    struct apu_watch_latch boot_scratch_read;

    uint32_t out_of_universe;                    /* uncapped counter */
    struct apu_watch_latch out_of_universe_latch;
};

/* The write-once freeze taken when GP_CLEAR latches. */
struct apu_watch_at_clear {
    uint32_t taken;
    uint32_t seq;
    uint32_t frame;
    struct apu_watch_gpin gpin;
};

/* Capped observation lists. Never decisive; no row may read them. */
struct apu_watch_obs {
    uint32_t n_pages;
    struct {
        uint32_t va;
        uint32_t count;
    } page[APU_WATCH_OBS_PAGES];

    uint32_t n_periph;
    struct {
        uint32_t offset;
        uint32_t first_value;
    } periph[APU_WATCH_OBS_PERIPH];
};

struct apu_watch_snapshot {
    uint32_t seq;
    uint32_t frame;                              /* global se_frame count */

    uint32_t counts[APU_WATCH_COUNTERS];
    struct apu_watch_latch latch[APU_WATCH_LATCH_CLASSES];
    struct apu_watch_site sites[APU_WATCH_N_SITES];

    /* Run counters, reset at the latest bootstrap. */
    uint32_t boots;
    uint32_t gp_frames;
    uint64_t gp_insns;

    /* GP DMA */
    uint32_t gpdma_ambiguous;                    /* uncapped, no row reads it */
    uint64_t gpdma_reads;
    uint64_t gpdma_writes;
    uint64_t gpdma_rbytes;
    uint64_t gpdma_wbytes;
    uint32_t gpdma_watch_lines;                  /* process-scoped, DS7 */

    /* The anchor site set by apu_watch_set_anchor_site(). */
    uint32_t anchor_site;

    struct apu_watch_gpin gpin;
    struct apu_watch_at_clear at_clear;
    struct apu_watch_obs obs;
};

/* ============================================================
 * Exported API
 * ============================================================ */

/* Device semantics 3 -- the ONE translation function.
 *
 * It inverts the non-injective forward map of bridge_MmGetPhysicalAddress at
 * the A4b1 baseline M (3f8bf67c), whose forward direction is: a VA in
 * [XBOX_CONTIG_BASE, XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE) returns
 * VA - XBOX_CONTIG_BASE, and anything else returns itself. So low-RAM VA X and
 * window VA 0x80000000 + X are both physical X, and the inverse has to be told
 * which form it was handed.
 *
 * Its input may be EITHER form, so -- unlike dma_resolve
 * (src/kernel/nv2a_pb_exec.c:88), whose input is always a physical offset and
 * which can therefore test the high-water mark first -- it must recognise an
 * already-a-window-VA BEFORE the high-water test, or a window VA below the
 * high-water mark would be double-translated.
 *
 * In priority order:
 *   1. an address inside the window [XBOX_CONTIG_BASE, +XBOX_CONTIG_SIZE) is
 *      itself a VA and is used as is (this keeps the 0d7929c form working);
 *   2. otherwise, P < xbox_ContiguousAllocatedBytes() (the high-water mark
 *      g_contig_next - XBOX_CONTIG_BASE, src/kernel/xbox_memory_layout.c:2694)
 *      means the translation is XBOX_CONTIG_BASE + P;
 *   3. otherwise, a mapped low-RAM range is identity;
 *   4. otherwise it fails closed: it logs "[GPDMA] unmapped addr=%08X len=%X"
 *      unconditionally (not trace-gated), once per address, and returns NULL.
 *      The caller then makes no access.
 *
 * It returns a host pointer only when the whole range lies in a mapped guest
 * region. When translated_va is non-NULL it receives the guest VA the range was
 * resolved to -- the address Device semantics 6's DMA_READ region class and
 * Device semantics 5's W_va comparison are both expressed in. (The packet names
 * the function and its two inputs and allows "another name"; the out-parameter
 * is how one function hands back both, rather than two functions that could
 * disagree.)
 *
 * It does not use & 0x03FFFFFF. See the comment in apu_watch.c.
 *
 * Claim limit: physical/VA aliasing is not modelled. A title that passes a
 * low-RAM VA below the contiguous high-water mark is misrouted into the window,
 * exactly as dma_resolve would do; that case increments GPDMA_AMBIGUOUS.
 */
uint8_t *apu_guest_dma_ptr(uint32_t addr, uint32_t len, uint32_t *translated_va);

/* Device semantics 5 -- the one GP DMA write choke point.
 *
 * Every GP DMA write to guest memory, from every pinned write callback, passes
 * through this function. `dst` is the translated host destination, `src` the
 * DSP-side data the transfer carries, `guest_va` the translated guest VA of the
 * transfer's first byte, `len` its length in bytes, and `dsp_addr` the DSP-side
 * address of the transfer's first word. The payload for the watched dword is
 * read out of `src`.
 */
void apu_gp_dma_write(uint8_t *dst, const uint8_t *src, uint32_t guest_va,
                      size_t len, uint32_t dsp_addr);

/* The read counterpart: counts the transfer and records its region class. */
void apu_gp_dma_read(const uint8_t *src, uint32_t guest_va, size_t len,
                     uint32_t dsp_addr);

/* ---- Device semantics 6: GP input recording hooks ----
 *
 * One exported recording function per input kind. Each is called from the
 * pinned production path (AC-PORT step 4 records the line), and each keys its
 * array by the property its classification depends on.
 */

/* PERIPH: dsp.c read_peripheral(). offset = address - DSP_PERIPH_BASE. */
void apu_gpin_periph_read(uint32_t offset, uint32_t value);

/* MIXBUF: the pinned frame path's write into GP X memory sets the per-frame
 * stub flag, and the core's mixbuffer read records against it. */
void apu_gp_mixbuf_frame_begin(uint32_t vp_active_voices);
void apu_gp_mixbuf_note_sample(uint32_t word);
void apu_gpin_mixbuf_read(uint32_t addr, uint32_t value);

/* FIFO_READ: the pinned fifo_rw path. fifo is 0..APU_WATCH_FIFO_COUNT-1. */
void apu_gpin_fifo_read(uint32_t fifo, size_t bytes);

/* The out-of-universe detector, reachable directly by the fixture case (x). */
void apu_gpin_record_out_of_universe(uint32_t kind, uint32_t index);

/* ---- Run counters ---- */
void apu_watch_gp_bootstrap(int is_gp);
void apu_watch_gp_bootstrap_done(int is_gp);
void apu_watch_gp_frame(int is_gp);
void apu_watch_gp_insns_add(int is_gp, uint32_t cycles);

/* The bootstrap's scratch DATA read, called from the pinned scatter-gather read
 * path. Device semantics 6 gives this its own write-once latch,
 * BOOT_SCRATCH_READ {va, first dword, seq}, "set in the bootstrap path" -- the
 * presence witness for AC-BOOT and AC-INPUTS, which no longer depends on a
 * table having room.
 *
 * It is called for the transfer's data, NOT for the SGE descriptor fetch that
 * precedes it: the descriptor is a 4-byte read of the SGE table, and latching it
 * would record the table's address rather than the page the bootstrap loaded.
 * The function takes the guest VA and the first dword, and takes the latch only
 * while the bootstrap window is open. */
void apu_watch_boot_scratch_read(uint32_t guest_va, uint32_t first_dword,
                                 size_t len, uint32_t dsp_addr);

/* The bootstrap window: while open, apu_watch_boot_scratch_read() takes the
 * latch. */
void apu_watch_bootstrap_begin(void);
void apu_watch_bootstrap_end(void);

/* The write-once at_clear freeze, taken when GP_CLEAR latches. */
void apu_watch_freeze_at_clear(uint32_t seq);

/* True when the mix-buffer content this frame is stub content. */
int apu_watch_mixbuf_stub(void);

/* ---- CPU side (Device semantics 6) ---- */

/* Records a store about to happen at target_va. Ignores any call whose
 * target_va is not MEM32(0x001BA858)+0x810. Called BEFORE the store, so the
 * observed value is 0 by definition (record-before-store). */
void apu_watch_cpu_store(uint32_t site_va, uint32_t target_va, uint32_t value);

/* The anchor site (CPU_ANCHOR: a store of 3 from the control site). The toolkit
 * hard-codes no site; A4b2 passes 0x001A18CE. */
void apu_watch_set_anchor_site(uint32_t site_va);

/* ---- Accessors, exported for fixtures ---- */
void apu_watch_snapshot(struct apu_watch_snapshot *out);

/* Returns every counter, latch, array, the at_clear block, GPDMA_AMBIGUOUS, the
 * process-scoped [GPDMA] watch line count and the [GPDMA] unmapped set to their
 * initial state. After it, no trace or ledger state from an earlier case
 * survives. GP core state is NOT reset by it (follow-up D4). */
void apu_watch_reset(void);

/* ---- Trace plumbing (Device semantics 7; all no-ops without the trace) ---- */

/* Advance the global se_frame counter and run the periodic emission. Called
 * once per APU frame. */
void apu_watch_frame_tick(void);

/* [GPRUN] and [GPDMA] frame lines, emitted from the pinned frame path. */
void apu_watch_trace_gprun(uint32_t cycles, uint32_t pc, int halt);
void apu_watch_trace_gpdma_frame(void);
void apu_watch_trace_gpdma_watch(uint32_t w_va, uint32_t before,
                                 uint32_t payload, uint32_t observed, int cas,
                                 uint32_t dsp_addr);

/* [GPBOOT], emitted inside the handling of the GPRST write that bootstraps. */
void apu_watch_trace_gpboot(uint32_t gprst, uint32_t prev,
                            uint32_t sge0, uint32_t sge0_va,
                            const uint32_t *pram, uint32_t pram_words);

/* True when RECOMP_APU_TRACE is set. Read once and cached. */
int apu_watch_trace_enabled(void);

#endif /* MCPX_APU_WATCH_H */
