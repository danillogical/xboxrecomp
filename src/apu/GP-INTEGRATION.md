# GP DSP integration — design contract

How the vendored xemu DSP56300 core (`dsp/`, provenance in `dsp/PROVENANCE.md`) is wired into this
toolkit's APU: bootstrap, per-frame execution, guest-DMA translation, the single GP write choke point,
and the lossless watched-word ledger and input accounting (`apu_watch.h`). Written for the JSRF
recompilation (packets A4b1-r4 and A4b2-r8, accepted 2026-09-26/27); the sections below are the
operative contract those packets were accepted against. "Advisor" refers to that project's senior
technical role, whose rulings set the ledger and accounting design.

## Device semantics (DS1–DS7)

1. **GPRST write: bootstrap and reset,** following the pinned `proc_rst_write` (quoted in the pin record).
   - **Reset** when either `GPRST|GPDSPRST` bit is clear in the new value.
   - **Bootstrap** when either bit was clear in the old value and both are set in the new value. The bootstrap is synchronous, inside the handling of that write.
   - The GP executes only in frames.
2. **GP per frame,** following the pinned `mcpx_apu_dsp_frame`: `dsp_start_frame`, then `dsp_run` in chunks until halt is requested.
   - `dsp_c_init` is called unconditionally.
   - No new environment variable is added.
   - The EP keeps the existing mixbin passthrough.
3. **One translation function:** `apu_guest_dma_ptr(uint32_t addr, uint32_t len)`, or another name, but exactly one. It inverts the non-injective forward map of `bridge_MmGetPhysicalAddress` at `M`. Its input may be **either form** (a window VA or a physical offset), so — unlike `dma_resolve` (`src/kernel/nv2a_pb_exec.c:88`), whose input is always a physical offset and which can therefore test the high-water mark first — it must recognise an already-a-window-VA **before** the high-water test, or a window VA below the high-water mark would be double-translated. It follows the toolkit precedent `dma_resolve` (for the high-water rule and the failure mode) and `xbox_memory_layout.c:843`. In priority order:
   1. an address inside the window `[XBOX_CONTIG_BASE, XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)` is itself a VA and is used as is (this keeps the `0d7929c` form working);
   2. otherwise, if `P < xbox_ContiguousAllocatedBytes()` (`src/kernel/xbox_memory_layout.c:2694`, the high-water mark `g_contig_next − XBOX_CONTIG_BASE`), the translation is `XBOX_CONTIG_BASE + P`;
   3. otherwise, a mapped low-RAM range is identity;
   4. otherwise it fails closed: it logs `[GPDMA] unmapped addr=%08X len=%X` **unconditionally** (not trace-gated), once per address, and returns NULL. The caller then makes no access.
   - Its comment names it the inverse of `bridge_MmGetPhysicalAddress` **at `M`**, states the four cases above, and explains that the window-VA test precedes the high-water test because the input may already be a window VA (the `0d7929c` form), whereas `dma_resolve` can put the high-water test first only because its input is always a physical offset. `dma_resolve`'s `surface_hits_image` test is an NV2A/framebuffer concern and is **not** imported; case 4 is its analogue. The comment may quote the measured failure mode the rule guards (a physical offset that "looked like an ordinary VA" and was cleared through the wrong region).
   - It returns a host pointer only when the whole range lies in a mapped guest region.
   - It does not use `& 0x03FFFFFF`; a comment explains why. (The `& 0x03FFFFFF` sites in `apu_shim.h`/`apu_vp.c` are VP code, which this semantics leaves untouched — a recorded lead, not scope.)
   - One **uncapped** counter `GPDMA_AMBIGUOUS` is incremented when case 2 is taken for a range that is also mapped low RAM. It is observation plus a claim limit only (6(b)-compliant as a fixed single counter); no row reads it.
   - **Claim limit:** physical/VA aliasing is not modelled; a title that passes a low-RAM VA below the contiguous high-water mark is misrouted into the window, exactly as `dma_resolve` would do.
   - VP code is otherwise untouched.
4. **Synthetic ack removed.** `dsp_ack_frame`, `dsp_ack_init` and every read of `RECOMP_APU_DSP_ACK` are deleted.
5. **One GP write choke point, with an atomic store.**
   - Every GP DMA write to guest memory, from every pinned write callback, passes through **one** function.
   - That function reads `W_va = MEM32(0x001BA858)+0x810` at write time.
   - The choke point receives the guest destination start, the length, and the DSP-side address of the transfer's first word.
   - If the write covers the aligned dword `W_va` and the payload for that dword is `0`, that dword is **never** written with an ordinary store. Instead (the rest of the transfer is written as usual):
     1. `o = InterlockedCompareExchange(host(W_va), 0, 3)`. If `o = 3` the exchange succeeded; classify on `observed=3`.
     2. If `o = 0`, the dword already holds the payload: **skip the write**; classify on `observed=0`.
     3. Otherwise `o2 = InterlockedCompareExchange(host(W_va), 0, o)`. If `o2 = o` it succeeded; classify on `observed=o`. Else set `o = o2` and repeat from 1.
   - This closes advisory D1 (a guest store of `3` landing between a failed exchange and an ordinary write of `0` can no longer be overwritten unclassified): every `0` the GP lands on `W_va` is landed by an exchange whose replaced value is the classification.
   - Guest memory ends up identical to a plain write. This is always on.
6. **Watched-word ledger (the decision record).** It is always computed: a few atomics per event, with no effect visible to the guest. It lives in a toolkit struct, written only by the choke point (5) and by `apu_watch_cpu_store`. The GP runs on the APU frame thread, so GP-side and CPU-side events order through the atomics.
   - **Shared state**
     - `seq` is a 32-bit counter, incremented with `InterlockedIncrement` once per ledger event.
     - Each class has one **uncapped** counter.
     - Each latched class has one **write-once latch** (`CPU_OTHER` has none). The thread that wins `InterlockedCompareExchange` on the latch's `latched` word fills in the fields below, then emits the latch line.
     - **Latch fields (every one defined; the latch line prints exactly these values):**
       - `seq`: the value **that event's own** `InterlockedIncrement(&seq)` returned, taken once per event before classification. It is never re-read when the latch is filled or the line printed.
       - `va`: for GP classes, **`W_va` as read by the choke point at that event** (never the DMA destination start); for CPU classes, the call's `target_va` (which equals `W_va`, since other targets are ignored).
       - `observed`: GP — the value the classifying exchange replaced (DS5), or for `GP_NONZERO_OVER`/`GP_PARTIAL` the dword at `W_va` read immediately before the write; CPU — `0` (the store has not happened yet: record-before-store).
       - `payload`: GP — the dword the transfer writes at `W_va` (for `GP_PARTIAL`, the dword `W_va` would hold after the write); CPU — the call's `value`.
       - `site`: CPU — the call's `site_va`; GP — `0`.
       - `frame`: the global `se_frame` count at the event.
       - `insns`: GP — `gp_insns` at the event; CPU — `0`.
       - `dsp_addr`: GP — the DSP-side address (24-bit, in the DSP memory space the DMA reads from) of the word that lands on the dword at `W_va`, or for `GP_PARTIAL` of the first overlapping word; CPU — `0`.
   - **GP classes,** classified at the choke point, from the `observed` of the exchange that landed the zero (DS5):
     - `GP_CLEAR`: zero payload, `observed=3`.
     - `GP_ZERO_OVER_ZERO`: zero payload, `observed=0` (write skipped). It is counted and latched, but never decides anything.
     - `GP_ZERO_OVER_OTHER`: zero payload, `observed ∉ {0,3}`.
     - `GP_NONZERO_OVER`: a non-zero payload covering the dword; `observed` is the value before the write.
     - `GP_PARTIAL`: the write overlaps `W_va`'s 4 bytes without covering the aligned dword.
   - **CPU classes,** recorded through the exported `void apu_watch_cpu_store(uint32_t site_va, uint32_t target_va, uint32_t value)`.
     - The function ignores any call with `target_va != MEM32(0x001BA858)+0x810`.
     - The anchor site is set through the exported `void apu_watch_set_anchor_site(uint32_t site_va)`; the toolkit hard-codes no site.
     - `CPU_ANCHOR`: a store of `3` from the anchor site.
     - `CPU_ZERO`: a store of `0`, latched **per site**. The site table has one entry **per enumerated `jsrf_watch_store` call site**, indexed one per site; its size `N_SITES` is **≥ that enumerated site count, and the count is stated in the source** (the watch-ledger ruling's retroactive note). The universe is the instrumented call sites — finite, source-enumerated, independent of run length.
     - `CPU_ZERO_OVERFLOW`: a store of `0` from a site **beyond** the enumerated `N_SITES`. It is a **bug detector** (6(b)), counted in an uncapped counter and latched once; it can be raised only by an unenumerated site, never by run length or input volume.
     - `CPU_OTHER`: any other store. It is counted only.
   - **Run counters** (uncapped):
     - `boots`: the total number of bootstraps;
     - `gp_frames`: GP frame-function calls since the latest bootstrap;
     - `gp_insns`: instructions the core retired since the latest bootstrap.
   - **GP input accounting (`[GPIN]`) — provenance classes over finite universes** (the accounting ruling (a); **the r3 256-entry table, `GPIN_OVERFLOW` latch, per-key lines and cut-off are retired and must not reappear**). Every GP input hook calls one exported recording function; each kind is keyed by the property its classification depends on, with a stated finite universe derived from source, as fixed arrays of **uncapped** counters that cannot overflow by construction. Completeness comes from **structure** (every input path enumerated, each with a fixture case), not from counting distinct keys at run time.
     - `MIXBUF`: provenance is a property of the frame's mix-buffer content, not of the word. The pinned frame path writes the VP mixbins into GP X memory at `GP_DSP_MIXBUF_BASE` before running the GP (pin record item 4). At that write, set a per-frame flag `mixbuf_stub = (vp_active_voices > 0) || (any sample written is non-zero)`. The read hook indexes a fixed array `[NUM_MIXBINS = 32]` by `bin = (addr − 0x1400) / NUM_SAMPLES_PER_FRAME`, with uncapped `{reads, reads_while_stub}` per bin, plus one write-once latch `MIXBUF_STUB_READ` (first read with `mixbuf_stub` set: `addr`, `frame`, `seq`, `vp_active_voices`). Universe: 32 bins. The decision is "was any stub-content word read", counted over every read.
     - `PERIPH`: a fixed array `[DSP_PERIPH_SIZE = 128]`, indexed by peripheral offset, with uncapped `{reads, first value, first seq}`. The Session classifies each of the 128 offsets once, statically, from the ported `read_peripheral` source: modelled (the core computes it from tracked state) or stub/unknown (a constant or unported register). Universe: 128.
     - `FIFO_READ`: a fixed array `[GP_INPUT_FIFO_COUNT + GP_OUTPUT_FIFO_COUNT = 6]`, indexed by FIFO, uncapped `{reads, words}`. Each FIFO's data source is classified statically from the pinned `dsp_dma`/`fifo_rw` path (at the pin: the SGE-described guest memory at `GPFADDR`). Universe: 6.
     - `DMA_READ`: keyed by **region class** of the **translated** address (the output of Device semantics 3), decided inside the one translation function `apu_guest_dma_ptr`. Classes: `LOW_RAM` `[0, g_memory_size)`, `CONTIG` (the `0x80000000` window), `DEVICE` (the MCPX, NV2A or flash apertures, i.e. any address ≥ `0xFD000000`), `OTHER_MAPPED`. Uncapped `{reads, bytes}` per class, plus a write-once first-VA latch per class. Universe: 4. The bootstrap's scratch read gets its own write-once latch `BOOT_SCRATCH_READ {va, first dword, seq}`, set in the bootstrap path — the presence witness, no longer depending on a table having room.
     - Out-of-universe: one uncapped counter `GPIN_OUT_OF_UNIVERSE` plus a write-once latch, for any record call whose kind or index is out of range (for example a peripheral offset ≥ 128). This is a **bug detector, not a volume guard**: no run length or input volume can raise it.
     - **Freeze at the clear:** when `GP_CLEAR` latches (on the APU thread, in the DMA write path), copy every input counter and latch into a write-once `at_clear` block. The running counters continue for the whole run and serve the `NOCLEAR`/`NOEXEC` brief. This deletes the cut-off state and its reset hazards.
     - **Observation, never decisive:** a capped list (for example the first 64 distinct `DMA_READ` pages and the first 64 distinct `PERIPH` offsets, with first values) for the `A4c` brief. The claim limit states it is capped; no row may read it.
   - **Accessors,** exported for fixtures:
     - `apu_watch_snapshot(struct apu_watch_snapshot *)` copies every counter, latch, CPU-site record, run counter, every `[GPIN]` fixed array and latch, `GPIN_OUT_OF_UNIVERSE`, and the `at_clear` block.
     - `apu_watch_reset()` returns all of them to their initial state, including the `[GPIN]` arrays/latches, the `at_clear` block, `GPDMA_AMBIGUOUS`, the process-scoped `[GPDMA] watch` line count (DS7), and the `[GPDMA] unmapped` once-per-address set. After it, no trace or ledger state from an earlier case survives. (GP core state is not reset by it; see follow-up D4.)
   - **Emission,** only under `RECOMP_APU_TRACE` (read once and cached). Every line is `fflush`ed, and there is a **fixed number of lines per emission** (no per-key lines), so nothing can be lost.
     - `[GPWATCH] latch class=<C> seq=%u va=%08X observed=%08X payload=%08X site=%08X frame=%u insns=%llu dsp_addr=%06X` is emitted exactly once, when a latch fires, with the latch's own field values. The number of latches is fixed, so this line has no cap and none can be lost.
     - `[GPIN] summary …` carries every fixed-array element (per bin / per offset / per FIFO / per region class) and the `at_clear` block, printed at the existing cadence and once for `at_clear`. A zero from an uncapped counter, updated at the event, over a hook set shown complete by enumeration and fixture, is positive evidence that no such read occurred; the absence of a line is not.
     - `[GPWATCH] counts seq=%u boots=%u gp_frames=%u gp_insns=%llu GP_CLEAR=%u GP_ZERO_OVER_ZERO=%u GP_ZERO_OVER_OTHER=%u GP_NONZERO_OVER=%u GP_PARTIAL=%u CPU_ANCHOR=%u CPU_ZERO=%u CPU_ZERO_OVERFLOW=%u CPU_OTHER=%u GPIN_OUT_OF_UNIVERSE=%u frame=%u` is emitted:
       - at each bootstrap, after the run counters reset;
       - at the first GP frame after each bootstrap;
       - immediately after any latch fires;
       - every 256th `se_frame`.
       - The `seq` field on this line is the value of the ledger's `seq` counter at emission.
7. **Trace (observation only).** Only under `RECOMP_APU_TRACE`. Every line is `fflush`ed, and none changes any value, store or control flow. **No row in `A4b1` or `A4b2` depends on whether a capped or sampled line is present or absent.**
   - `[GPBOOT] n=%u sge0=%08X sge0_va=%08X gprst=%08X prev=%08X` is emitted **inside** the handling of the GPRST write that bootstraps.
     - `gprst` is the value written; `prev` is the value before it.
     - `sge0` is the **raw 32-bit value in SGE entry 0**; `sge0_va` is its **Device-semantics-3 translation** (the page the bootstrap reads). Both are printed.
     - It is followed by the first `0x200` PRAM words, as `[GPBOOT] pram %03X: w0 … w7`: 24-bit hex, 8 words per line, 64 lines.
     - The existing hook prints the `[APUMMIO]` line for that write afterwards.
   - `[GPRUN] frame=%u se_frame_after_boot=%u cycles=%u insns=%llu pc=%06X halt=%d tone=%d` is one line carrying all fields.
     - It is emitted for the first 8 GP frames after each bootstrap, then every 256th frame.
     - `se_frame_after_boot` is equal to `gp_frames`.
   - `[GPDMA] watch …` is emitted per covering write, for the first 16 since the latest `apu_watch_reset()` (process start in a run). It is observation only, with no exemption rule and no cap line.
   - `[GPDMA] frame=%u reads=%u writes=%u rbytes=%llu wbytes=%llu` is emitted every 256th frame.
   - `[GPIN]` is **not** a per-key trace line: it is the ledger's finite-universe input accounting, defined and emitted under Device semantics 6. A reader decides from the `[GPIN] summary` lines' fixed arrays and the `at_clear` block, never from the presence or absence of a per-key line.

## Why the ledger is shaped this way (watch-ledger ruling)

A capped, human-readable log line was once both the observation record and the decision input, so a
cap or an earlier write-back could lose the deciding event. The ruling separated them: the decision
comes from a write-once event ledger kept by the write paths themselves, lossless by construction; log
lines are observation only. Its design, verbatim:

(a) What is recorded, and where: a toolkit "watched-word ledger" (A4b1).
1. State. A small device-side struct, not a log:
   - an atomic 32-bit sequence counter `seq`, incremented with InterlockedIncrement for every ledger event;
   - uncapped counters for each event class;
   - one write-once latch for the first event of each class. Set it with InterlockedCompareExchange on a "latched" word; the latch stores seq, va, before/observed, payload, frame, insns and dsp_addr.
   It is computed always (a few atomics, no guest-visible effect). Its lines are printed under RECOMP_APU_TRACE.
   It must be thread-safe. The GP runs on the APU frame thread (`apu_core.c:557`, qemu_thread_create "mcpx.apu_thread"), and guest CPU stores come from guest threads.
2. GP-side classes, recorded on the GP DMA write path at the same point as the Device semantics 5 compare-exchange. W_va = MEM32(0x001BA858)+0x810, read at write time and recorded in every latch.
   - GP_CLEAR: zero payload, CAS(host(W_va), 0, 3) succeeded, observed=3. This is the attribution witness.
   - GP_ZERO_OVER_ZERO: zero payload, CAS failed, observed=0. This is the pre-command write-back case (the B3 scenario). Counted and latched; it never decides.
   - GP_ZERO_OVER_OTHER: zero payload, CAS failed, observed not in {0,3}.
   - GP_NONZERO_OVER: a non-zero payload covering W_va. The latch records before/payload.
   - GP_PARTIAL: a GP DMA write that overlaps W_va's 4 bytes without covering the aligned dword. It is not CAS-protected, so it is an unattributable writer.
3. CPU-side classes, through one exported toolkit function `apu_watch_cpu_store(uint32_t site_va, uint32_t target_va, uint32_t value)`. It checks `target_va == MEM32(0x001BA858)+0x810` itself, and the game calls it before each instrumented store.
   - CPU_ANCHOR: the first store of 3 from the control site (recomp_0005.c:6746). A4b2 passes the control-site VA; A4b1 does not hard-code it.
   - CPU_ZERO: the first store of 0, latched per site. Use a bounded site table (say 16 entries) plus one CPU_ZERO_OVERFLOW latch. Overflow is itself a recorded event, never silent.
   - CPU_OTHER: counters only.

   **Premise (added 2026-09-24):** the bounded site table is safe **only because** the site universe is finite, enumerated in source, and independent of run length — the instrumented `jsrf_watch_store` call sites. Applying this same "bounded table + overflow latch" pattern to a key whose universe is *not* so bounded produced a third `INADEQUATE` verdict; see "Input accounting" below.
4. Emission, under RECOMP_APU_TRACE:
   - exactly one `[GPWATCH] latch class=<C> seq=%u va=%08X observed=%08X payload=%08X site=%08X frame=%u insns=%llu dsp_addr=%06X` at the moment each latch fires, fflush'd. The number of classes is fixed, so this needs no cap and cannot lose a line;
   - `[GPWATCH] counts seq=%u <class>=%u …` every 256th frame, and again immediately after GP_CLEAR latches.
   The existing `[GPDMA] watch` lines and their cap stay as observation only. Delete the r3/A4b1 "exempt line" and "cap reached" rules: nothing depends on them any more.
5. Test accessor: `apu_watch_snapshot(struct *)` and `apu_watch_reset()`, used only by the fixture, which then checks the struct and not log text. This also removes A4b1 B1's fixture-order and cumulative-count problem.

Why this cannot lose the witness:
- The GP_CLEAR latch fires only on a successful 3→0 CAS, so no earlier zero-over-zero write-back, cap or ordering can consume it.
- Its line is emitted exactly once, when the latch fires.
- The CAS is the ordering authority. A CPU record is written before its store, so CPU seq order is only approximate. But if GP_CLEAR's CAS saw 3, no CPU 0 had landed by then, whatever the seq order says.

## Input accounting (finite-universe ruling)

A bounded table keyed by something whose universe grows with the run (DSP word address, guest page)
overflowed and produced false `UNKNOWN`s. The ruling keys each input kind by what its classification
depends on, over a stated finite universe. Design, verbatim:

(a) Design — key each kind by what its classification depends on, with a stated finite universe (A4b1 DS6, replacing the 256-entry table):
- MIXBUF: provenance is a property of the frame's mix-buffer content, not of the word. The pinned frame path writes the VP mixbins into GP X memory at GP_DSP_MIXBUF_BASE before running the GP (pin record item 4). At that write, set a per-frame flag mixbuf_stub = (vp_active_voices > 0) || (any sample written is non-zero); the second clause is conservative against the test tone and other writers. The read hook indexes a fixed array [NUM_MIXBINS = 32] by bin = (addr − 0x1400) / NUM_SAMPLES_PER_FRAME, with uncapped {reads, reads_while_stub} per bin, plus one write-once latch MIXBUF_STUB_READ (first read with mixbuf_stub set: addr, frame, seq, vp_active_voices). Universe: 32 bins. This also closes r2 D5 (first value only): the decision is "was any stub-content word read", counted over every read.
- PERIPH: fixed array [DSP_PERIPH_SIZE = 128], indexed by peripheral offset, with uncapped {reads, first value, first seq}. The Session classifies each of the 128 offsets once, statically, from the ported read_peripheral source: modelled (the core computes it from tracked state) or stub/unknown (a constant or unported register). Universe: 128.
- FIFO_READ: fixed array [GP_INPUT_FIFO_COUNT + GP_OUTPUT_FIFO_COUNT = 6], indexed by FIFO, uncapped {reads, words}. Each FIFO's data source is classified statically from the pinned dsp_dma/fifo_rw path (at the pin: the SGE-described guest memory at GPFADDR). Universe: 6.
- DMA_READ: key by **region class** of the translated address, decided inside the one translation function apu_guest_dma_ptr (the existing choke point). Classes: LOW_RAM [0, g_memory_size), CONTIG (the 0x80000000 window), DEVICE (the MCPX, NV2A or flash apertures, i.e. any address ≥ 0xFD000000), OTHER_MAPPED. Uncapped {reads, bytes} per class, plus a write-once first-VA latch per class. Universe: 4. The bootstrap's scratch read gets its own write-once latch BOOT_SCRATCH_READ {va, first dword, seq}, set in the bootstrap path. That is the presence witness, and it no longer depends on a table having room.
- Out-of-universe: one uncapped counter GPIN_OUT_OF_UNIVERSE plus a write-once latch, for any record call whose kind or index is out of range (for example a peripheral offset ≥ 128). This is a **bug detector, not a volume guard**: no run length or input volume can raise it.
- Freeze at the clear: when GP_CLEAR latches (on the APU thread, in the DMA write path), copy every input counter and latch into a write-once `at_clear` block. The running counters continue for the whole run and serve the NOCLEAR/NOEXEC brief. This deletes the cut-off state and its reset hazards.
- Observation, never decisive: a capped list (for example the first 64 distinct DMA pages and the first 64 distinct PERIPH offsets, with first values) for the A4c brief. The claim limit states it is capped; no row may read it.
- Emission: under the trace, the `[GPWATCH] counts` line gains the fixed arrays (or a `[GPIN] summary` line per kind carrying every array element), printed at the existing cadence and once for `at_clear`. There is a fixed number of lines per emission and no per-key lines, so nothing can be lost.

**When accounting may make a criterion `UNKNOWN`:**

(b) Should overflow make a strict criterion UNKNOWN? After this redesign overflow cannot happen, so the question dissolves. The general answer: an accounting record may make a criterion UNKNOWN only for **integrity** failures that no run length can cause:
- the accounting is absent (no at_clear block, or no counts line);
- GPIN_OUT_OF_UNIVERSE > 0;
- AC-BOOT PASS without a BOOT_SCRATCH_READ latch;
- a hook enumerated in AC-PORT step 4 whose kind shows no fixture exercise.
Never for volume. And the distinction that makes a zero admissible: **a zero from an uncapped counter, updated at the event, over a hook set shown complete by enumeration and fixture, is positive evidence that no such read occurred.** The absence of a line is not. This is consistent with the losslessness rule; it is what that rule was for.
