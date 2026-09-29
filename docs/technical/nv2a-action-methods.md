# NV2A action methods: admission evidence

**Status: not admitted.** Everything described here runs only when
`RECOMP_NV2A_ACTIONS` is exactly `1` (read once, cached in
`nv2a_actions_enabled`, `src/nv2a/nv2a_core.c`). With it unset the model walks a
submission exactly as before; a run that sets it is exploratory under
`docs/jsrf-run-profiles.md` (game repository) §"Override classification". This
document collects the evidence the owner needs to decide whether any of the three
behaviours can become an unconditional modelled hardware cause under
§"Unconditional modeled hardware causes" of that file. It is a record, not a
ruling.

No public NV2A datasheet exists, so only the secondary-source path applies: at
least two independent sources of meaningfully different provenance, one specific
to Xbox, none derived from this toolkit. Guest code (JSRF, the XDK D3D
reconstruction) may corroborate but is not counted.

## Sources and pins

Every file was fetched at the pinned revision on 2026-09-28; line numbers are for
that revision.

| Id | Source | Revision | File | SHA-256 |
|---|---|---|---|---|
| X1 | xemu (Xbox emulator; QEMU device model) | `f9b14039e5bb56ae2d8f028e31e7cc19f13f7e12` | `hw/xbox/nv2a/pgraph/pgraph.c` | `d7261f812aa1f031c3d72b5f070d2a1f753f1b2d6bc4ddedfff396770184f945` |
| X2 | xemu | same | `hw/xbox/nv2a/pfifo.c` | `1a06a599cc29b2de85d3297b8906f19727a30d3cc0faf954c424be554ebaff90` |
| X3 | xemu | same | `hw/xbox/nv2a/nv2a_regs.h` | `c9c87c1455201f556cd4b4457a6676250e073640e6581b93bc5b40659cf82262` |
| X4 | xemu | same | `hw/xbox/nv2a/nv2a.c` | `d6f28a4ccc20d2b6f50f2a13f50eb91ab62df738f212c97f3a1fa14d247daece` |
| E1 | envytools (NVIDIA reverse engineering; `hwtest` runs against real cards) | `f102b82381f3f11cee113d16374c87091db039d9` | `hwtest/pgraph_mthd_misc.cc` | `596c893512115f2e1e5e1d8292ea291375b19bb3e9a2db35167d88917ae99e33` |
| E2 | envytools | same | `hwtest/pgraph_mthd.cc` | `204eb8781305c35a73773b0d3164c75c0c30a0bead939df635a4212048d1d6e0` |
| E3 | envytools | same | `nvhw/pgraph_xy4.c` | `2a9124584acf3a2b842040ab6c05bb43c35d86c095069086ade65cdbb3010fe5` |
| E4 | envytools | same | `hwtest/pgraph_class_kelvin.cc` | `f6307a4405518c0c30d9d780b1716801947830e40c3d5aef92d322dc8f5c4957` |
| E5 | envytools | same | `hwtest/pgraph_state.cc` | `8e411a0934902a498056dc33d7abd2b3adc2b9d1e7823a5a02f382cb10f081c5` |
| E6 | envytools | same | `rnndb/graph/nv4_pgraph.xml` | `bb5af4ef4a65405f122bc90696f97c6060c0b09f96a9f26d78b1c9bb45f482b0` |
| E7 | envytools | same | `rnndb/graph/nv10_3d.xml` | `2f7270c7b89a0d2884c9d38ff03ae943c50848d8c51058061aa075f7e0910aab` |
| E8 | envytools | same | `rnndb/graph/nv20_3d.xml` | `02ada5ac153ab274d698bd728344a172dbedd0853d12e35aacbca24865c622c6` |
| E9 | envytools | same | `rnndb/graph/nv30-40_3d.xml` | `fbe43672287566c2e0d83bb4b53e26354305e8fb88a02bd39849a2942af2d9ce` |
| E10 | envytools | same | `docs/hw/fifo/puller.rst` | `2e308816e7d89a8261f9b92f998df224c9d4fefd158cf679bf5e8d050faa0724` |
| P1 | nxdk pbkit (Xbox homebrew GPU driver, runs on retail hardware) | `58427c078b4ccb0121f359b6b6c536b3b2914976` | `lib/pbkit/pbkit.c` | `da9835f0d9e9da4a7e5c185b7913752aab5614bfc170ebc572cc12cc3af0da92` |
| P2 | nxdk pbkit | same | `lib/pbkit/outer.h` | `d1bac570ee7e2ee1fce989899819a74d0245f5f1948f54a63a01b7f0927faa0c` |
| L1 | Linux nouveau | tag `v6.6` (commit `ffc253263a1375a65fa6c9f62a893e9767fbebfa`) | `drivers/gpu/drm/nouveau/nvkm/engine/dma/usernv04.c` | `9a688dd7734062617fed3857545abe61b24661714604ba56ff0677d7329c067d` |

Corroboration only (guest-side, not counted):

- JSRF original code, PGRAPH ISR `0x00194210..0x001942F8` (recovered body in the
  game repository, `src/recomp/recovered/recovered.c`).
- XDK D3D reconstruction, `~/src/halo-ce-universal` `087c1f56853fdf11a6901d46a5b0bc4910f99953`,
  `libs/d3d8/mpintr.cpp` (SHA-256 `60c12ad47e26f96f42bb2098b23e30a16178b0e83a8819797f98696c42cac827`)
  and `libs/d3d8/mphal.cpp` (SHA-256 `e232c59dc26ca26189886bbb7285ef82f7a580e5bbcd919f403b46167e48f5cd`).
  GPL-3.0 and derived from reconstructed Microsoft code: facts only.

Provenance notes. xemu is an Xbox-specific emulator whose NV2A model derives
from earlier xqemu work; envytools is a PC-GPU reverse-engineering project whose
`hwtest` compares a software model against real NV04..NV4x cards (the NV2A is a
Kelvin-family core between NV20 and NV25, but envytools never ran on an Xbox);
pbkit is a driver that runs on retail Xbox hardware, written from analysis of the
hardware and of Microsoft's driver. They share no code with each other or with
this toolkit. pbkit and the XDK D3D both descend, directly or by analysis, from
Microsoft's driver, so on questions where pbkit only mirrors what D3D does it is
closer to guest evidence than to an independent hardware source; that is noted
where it matters.

## 1. Semaphore release (`0x01A4`, `0x1D6C`, `0x1D70`)

**Modelled behaviour.** `SET_CONTEXT_DMA_SEMAPHORE` (`0x1A4`) names a DMA object
by handle; `SET_SEMAPHORE_OFFSET` (`0x1D6C`) latches an offset;
`BACK_END_WRITE_SEMAPHORE_RELEASE` (`0x1D70`) writes its parameter as a 32-bit
word at the DMA object's base + offset. Implemented in `action_method` /
`semaphore_target` / `action_commit`; staged, and written only when the walk
commits.

| Claim | X (xemu) | Independent |
|---|---|---|
| `0x1A4` is the semaphore DMA object | X3 L862 `NV097_SET_CONTEXT_DMA_SEMAPHORE 0x1A4`; X1 L957-960 | E8 L101 `0x01a4 DMA_FENCE type="object"` (NV20 3D) |
| Methods `0x180..0x1FC` carry a handle the puller resolves through RAMHT, so PGRAPH receives the instance | X2 L209-215 | E10 L45 `0x0180:0x0200 NV4:GF100 ... goes through RAMHT lookup` |
| DMA object layout: word 0 flags (class, target, adjust in bits 20-31), word 1 limit, word 2 frame | X4 L63-78 `nv_dma_load` | L1 L47-68: `flags0 \| (adjust << 20)`, `length`, `flags2 \| offset` |
| `0x1D6C` offset, `0x1D70` write value at base + offset | X3 L1269-1270; X1 L2841-2865 (`stl_le_p(semaphore_data + offset, parameter)`) | E9 L600-601 names `0x1d6c FENCE_OFFSET`, `0x1d70 FENCE_VALUE` **for NV30-NV44 only**; no behaviour |

**Evidence state.** The DMA-object resolution (handle → RAMHT → instance →
base/limit) has two independent sources. The release itself has one behavioural
source (xemu). envytools corroborates the names of all three methods but gives
`0x1D6C/0x1D70` only for the NV30 generation and describes no semantics; pbkit
does not use these methods. Guest corroboration: JSRF's first stream (`0x1A4 = 8`,
`0x1D6C = 0`, `0x1D70 = 5, 7`) and D3D's fence wait at `0x001914F0` polling the
same word; the D3D reconstruction's `dxgcreate.cpp` pushes the same sequence.

**Divergences from X1.** The model requires the whole dword to lie at or below
the object's limit (X1 asserts only `offset < limit`), requires 4-byte alignment,
resolves the physical address through the contiguous window only (X4 masks it to
27 bits into VRAM; the toolkit has no unified physical memory and does not alias),
and rejects any failure as `semaphore_fault` with the stream rolled back instead of
asserting.

## 2. Software-method trap (non-zero `NOP`)

**Modelled behaviour.** On a Kelvin subchannel, `NOP` (`0x0100`) with a non-zero
parameter, when PGRAPH `DEBUG_3` bit 20 (data check) is set: the walk commits up to
and including the NOP, leaves GET just past it, latches `TRAPPED_ADDR`
(`0x100 | subchannel << 16 | chid << 20`) and `TRAPPED_DATA_LOW`, sets `NSOURCE`
`DATA_ERROR` (bit 1), raises `NV_PGRAPH_INTR` `ERROR` (bit 20), clears
`NV_PGRAPH_FIFO` access, and delivers through `nv2a_update_irq` → `NV_PMC_INTR_0`
bit 12 → the existing irq sink, gated by `NV_PGRAPH_INTR_EN` and
`NV_PMC_INTR_EN_0`. Kicks while trapped walk nothing. The walk resumes, inside the
same packet, after the guest has both cleared `ERROR` and re-enabled FIFO access.
`0x1D8C` / `0x1D90` latch PGRAPH `0x1A88` / `0x186C`.

| Claim | X (xemu) | Independent |
|---|---|---|
| A non-zero NOP on Kelvin is trapped | X1 L823-857 (traps whenever parameter ≠ 0) | E1 L119-125 `MthdNopTest::is_valid_val` (and L199-205 `MthdNop`): on `>= PGRAPH_3D_KELVIN` only `val == 0` is valid; E2 L496-583 `MthdTest::mutate`: an invalid value blows up (L546-547) when `DEBUG_D` (`0x40008C`) bit 20 is set; E4 L10151+ lists the Kelvin class tests |
| The trap raises `INTR` bit 20 and ORs an `NSOURCE` bit | X1 L847-850; X3 L203, L205-206 | E3 L443-460 `nv04_pgraph_blowup`: `intr \|= 0x100000` (NV10+), `nsource \|= nsource`; E5 L2486-2490 maps `intr`/`nsource` to `0x400100`/`0x400108` |
| `NSOURCE` value | X1 L847-848 `NV_PGRAPH_NSOURCE_NOTIFICATION /* TODO: check this */` | E2 L546-547 passes `2` (DATA_ERROR) to blowup; E6 L696-742 names bit 1 `DATA_ERROR` |
| `TRAPPED_ADDR` = method bits 2-12, subchannel 16-18, chid 20-24 | X3 L260-264; X1 L840-845 | E2 L420-424 (NV10+ layout); P2 L464-466 |
| The trap clears PGRAPH FIFO access | not modelled by X | E3 L456 `fifo_enable = 0`; E5 L2447 maps `fifo_enable` to `0x400720` |
| The walk stays held until `ERROR` is cleared and FIFO access is on | X2 L142-147 (`waiting_for_nop` or `!can_fifo_access` stalls); X1 L94-104 (W1C of `ERROR` clears `waiting_for_nop`), L171-173 (a FIFO write kicks) | E3 L456 (hardware turns access off, so software must turn it back on); P1 L339 / L480 (driver writes FIFO off on entry, on at exit) |
| GET sits after the trapping word | X2 L342-344 / L430 (`dma_get` stored after the method that set `waiting_for_nop`; the next method stalls) | — |
| `NSOURCE` clears with its `INTR` bit | — | E6 L696-699 ("Cleared automatically when you clear relevant INTR bit") |
| `0x1D8C` → `0x401A88`, `0x1D90` → `0x40186C` | X1 L2894-2902; X3 L414, L643, L1277-1278 | P1 L276-282 (`PARAMETER_A = [0xFD401A88]`, `PARAMETER_B = [0xFD40186C]`); P2 L71-72 |
| A driver dispatches software methods from this trap | — | P1 L322-481 (`pb_gr_handler`): ERROR/NOTIFY with `NSOURCE` non-zero and without `ILLEGAL_MTHD`, trapped method `0x100` → `pb_subprog(TRAPPED_DATA_LOW, PARAMETER_A, PARAMETER_B)`; P1 L2566-2584 sets `DEBUG_3` `DATA_CHECK_ENABLED` (P2 L380 = bit 20) |

Guest corroboration: JSRF's ISR (`0x00194210`) writes `0x400720 = 0`, reads
`INTR`/`TRAPPED_ADDR`/`NSOURCE`, writes the pending bits back, requires `NSOURCE`
non-zero, `INTR & 0x100001`, `NSOURCE` bit 6 clear and method `0x100`, then calls
`0x00193F70(TRAPPED_DATA, [0x401A88])`, and writes `0x400720 = 1` on exit. The D3D
reconstruction's `ServiceGrInterrupt` / `SoftwareMethod` (`mpintr.cpp`) matches
and writes `DEBUG_3 = 0xF3DE0479` (`mphal.cpp` L390, L403), bit 20 set.

**Evidence state.** The trap, its interrupt bit, the trapped-address layout, the
FIFO-access acknowledgement and the parameter registers each have two
independent sources (xemu + envytools hardware tests, with pbkit on the Xbox side).
Two points are **disputed** and the model does not choose silently:

- **The `DEBUG_3` bit 20 gate.** xemu traps unconditionally; envytools traps only
  with data checking on. With it off the model stops the stream, rolled back, as
  `software_method_unchecked`.
- **The `NSOURCE` bit.** xemu writes `NOTIFICATION` (bit 0) and flags it
  unverified; envytools' hardware test gives `DATA_ERROR` (bit 1). The model uses
  bit 1. Both JSRF's ISR and pbkit accept any non-zero `NSOURCE` without bit 6, so
  the choice does not change JSRF's path.

Not modelled: `NSTATUS` (`0x400104`, envytools sets `BAD_ARGUMENT`), `NOTIFY`
state, and PGRAPH FIFO access as a general gate on walks that are not resuming
from a trap (xemu gates every method on it).

**Divergence noted.** The brief asked for GET to be left *at* the trapping
method. The references put it *after* the trapping word (X2 above); the trapped
method is reported through `TRAPPED_ADDR`/`DATA`, and resuming at it would trap
again. The model follows the references.

## 3. `FLIP_STALL` (`0x0130`) with `FLIP_INCREMENT_WRITE` (`0x012C`) and `NV_PGRAPH_INCREMENT` (`0x40071C`)

**Modelled behaviour.** `0x120`/`0x124`/`0x128` set `NV_PGRAPH_SURFACE`
(`0x400710`) `READ_3D` (bits 24-26) / `WRITE_3D` (20-22) / `MODULO_3D` (28-30);
`0x12C` steps `WRITE_3D`, wrapping to 0 at the modulo. `FLIP_STALL` commits
itself and holds the walk while `READ_3D == WRITE_3D`. A guest write of
`NV_PGRAPH_INCREMENT` with bit 1 steps `READ_3D` the same way and, once the two
differ, resumes the walk. `NV_PGRAPH_INCREMENT` is a trigger and is not stored.

| Claim | X (xemu) | Independent |
|---|---|---|
| Methods `0x120..0x130` are the flip set/step/stall methods | X3 L849-853; X1 L864-908 | E7 L51-55 `FLIP_SET_READ`, `FLIP_SET_WRITE`, `FLIP_MAX`, `FLIP_INCR_WRITE`, `FLIP_WAIT` |
| Field positions in `0x400710` and the write step with wrap | X3 L266-269; X1 L864-899 | E1 L285-308 with E4 L10151-10154 (`which_set = 1`: bits 20-22, 24-26, 28-30; the step wraps to 0 on reaching the modulo); E5 L580 maps `surf_type` to `0x400710` on NV10+ |
| `FLIP_STALL` holds until `READ_3D != WRITE_3D` | X1 L901-908; X2 L103-140 (`is_flip_stall_complete`, `pfifo_stall_for_flip`) | — |
| `INCREMENT` bit 1 steps `READ_3D` and kicks the puller | X1 L108-118; X3 L270-272 | P1 L239 (vblank handler writes `NV_PGRAPH_INCREMENT \|= READ_3D_TRIGGER`); P2 L474-475 |

Guest corroboration: JSRF sends `0x120 = 0`, `0x124 = 1`, `0x128 = 3`, then
`0x12C`, `0x130`; the D3D reconstruction's `VBlank` and `SoftwareFlipImmediate`
write `0x40071C |= 2` (`mpintr.cpp` L189, L222).

**Evidence state.** The method identities, field layout and write step have two
independent sources (the step is hardware-tested by envytools). The stall
condition itself has one behavioural source (xemu): envytools has no
`FLIP_STALL` test, and pbkit only shows the `INCREMENT` write, which mirrors D3D
and so is close to guest evidence. The read-counter wrap follows the write step's
arithmetic; xemu's `% MODULO` agrees for every in-range value and divides by zero
at modulo 0, which the model avoids. Not modelled: envytools shows the flip
methods are valid on class `0x97` only with `DEBUG_3` bit 25 set (E1 L285-289);
like xemu, the model accepts them unconditionally.

## Claim limits against the admission criteria

| Criterion | Semaphore release | Software-method trap | `FLIP_STALL` |
|---|---|---|---|
| 1. Unconditional | **Not met** by design: behind `RECOMP_NV2A_ACTIONS`. | **Not met** by design. | **Not met** by design. |
| 2. Grounded, two independent sources | **Partly.** DMA-object resolution: two sources. The write itself: xemu only; envytools gives names (NV20 `DMA_FENCE`, NV30 `FENCE_OFFSET/VALUE`) without behaviour. | **Met** for the trap, interrupt bit, trapped-address layout, FIFO-access acknowledgement and parameter registers. **Disputed**: the `DEBUG_3` gate and the `NSOURCE` bit (handled as above). | **Partly.** Methods, fields and step: two sources. Stall condition: xemu only. |
| 3. Limited to modelled state | Met: writes the one guest word and `SEMAPHOREOFFSET`/the DMA latch. | Met: `INTR`, `NSOURCE`, `TRAPPED_ADDR/DATA`, `FIFO`, the PMC summary and the walk position; `NSTATUS` untouched. | Met: `SURFACE` fields and the walk position. |
| 4. Cannot stand in for work the guest consumes | **Not met.** The released value tells D3D that the GPU finished everything before it; the model releases it when the walk *records* the stream, but renders nothing. It is a completion signal for work that did not happen. | Met in itself: the interrupt reports a method the model did execute (the NOP); the work behind the software method is done by the guest's own handler. Everything the trap unblocks downstream (fence events, flips) inherits the limits of whatever satisfied it. | Met in itself: it only withholds progress until the guest's own vblank handler advances the read counter. |

A wait satisfied through any of these, even once admitted, would prove only
that the specific wait was satisfied by the modelled cause, not rendering,
presentation or liveness.

## Interaction with the fence mirror

The game registers `xbox_Nv2aMirrorFence(0x0019DCE0, 0x30, 0x34)`: the toolkit's
`fence_mirrors_tick` (`src/kernel/xbox_memory_layout.c`) copies `[dev+0x30]` into
the word `[dev+0x34]` points at (`0x80000000`, physical 0 in the contiguous
window). In JSRF the semaphore DMA object (handle 8) at offset 0 is expected to
address the same word, so with the switch on two writers target it. The mirror is
left as it is (retiring it is an owner decision); it now calls
`nv2a_note_fence_mirror_write` before each write, which logs and counts
(`nv2a_fence_mirror_overlaps`) every mirror write to the host word the last
release wrote. With the switch unset the call returns immediately.

## What cannot be verified here

No JSRF run is possible without game assets, and `0x00193F70` (the software-method
handler the ISR calls) is still unrecovered and fatal, so the trap cannot reach
guest code end to end. The toolkit side is covered by `tests/nv2a_actions_test.c`
(CTest `nv2a_actions`).
