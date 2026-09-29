# Vendored xemu DSP56300 core — provenance and licence

This directory vendors the DSP56300 interpreter and GP/EP glue from xemu, byte-exact at a pinned
commit (`.gitattributes` sets `* -text` so the bytes cannot drift), plus the local modifications and
QEMU shims listed below. `NOTICE` and `LICENSES/` carry the licence texts. Ported for the JSRF
recompilation (packet A4b1-r4, accepted 2026-09-26); the GP integration design is in
`../GP-INTEGRATION.md`.

**Pinned commit:** `67cc79e663038d1f55448c0f566b37dde016adf6`
("apu: Add new DSP56300 emulator with JIT execution engine", Matt Borgerson,
authored 2026-03-20, committed 2026-06-12). This is the most recent commit touching
`hw/xbox/mcpx/apu/dsp/dsp.c` as of 2026-09-24, fetched from the GitHub API this session.
It is a commit, not `master`.

**Upstream path:** `hw/xbox/mcpx/apu/dsp/`
**Raw base used for every hash below:**
`https://raw.githubusercontent.com/xemu-project/xemu/67cc79e663038d1f55448c0f566b37dde016adf6/hw/xbox/mcpx/apu/dsp/`

## Per-file identity and licence

Every file below was fetched at the pin and hashed. Licences are read from
each file's own header, not assumed.

| File | Bytes | SHA-256 | Licence (from its header) |
|---|---|---|---|
| `dsp.c` | 5163 | `bd40122dbca27b1ac8975f13786d071bed3a2380cb43bb368088a173d9794b60` | **GPL-2.0-or-later** |
| `dsp.h` | 4478 | `9310695dc57f630445101d49525139f62b090fe0f7988232f98458d484f6fd84` | **GPL-2.0-or-later** |
| `dsp_c.c` | 9051 | `89ed49bf56f66e8b59ae170911ecc9a6989f82bda793abec7c36d643cdef3516` | **GPL-2.0-or-later** |
| `dsp_internal.h` | 780 | `f63862aa87c2af6fea9d6f1cb184e7768e0d61d56e505f5b56bbd0f471c418c3` | **GPL-2.0-or-later** |
| `debug.h` | 1016 | `1773c7f3b3db5318886ce48b01e42e00c94b80a8364089dc6276d6c7ed26eeef` | **GPL-2.0-or-later** |
| `interp/dsp_cpu.c` | 50993 | `b2879f6895ac5b4702a3a77dbd94fc5e4517928f5ef449030d44557860833e5b` | **GPL-2.0-or-later** |
| `interp/dsp_cpu.h` | 4434 | `314ab9afc8a01f9ceeafd6d0cb9a3e2446b0aa7e1bede758c5d7ad2b141163cf` | **GPL-2.0-or-later** |
| `interp/dsp_cpu_regs.h` | 3338 | `5373cbe8cb5326f39cbebfbbb13a85d78b4f8d47d7ebd3e4f2291e2fcde2c926` | **GPL-2.0-or-later** |
| `interp/dsp_emu.c.inc` | 238793 | `ffc744df58e3fea9df1fd2acffe267cb4126b5ecb6c82518009bcea17fbb6e5f` | **GPL-2.0-or-later** |
| `interp/dsp_dis.c.inc` | 63062 | `2a624a326c7633ff8a866bea4b379ac23aef3c3768a0873bf7d85a6b3e35911d` | **GPL-2.0-or-later** |
| `interp/debug.c` | 10169 | `0a4429c38908b11ec53437476be11f140d46275eee26834e31a40711e1064075` | **GPL-2.0-or-later** |
| `gp_ep.c` | 17732 | `979044a337f3763b3eb079810f9182d6bc025529f30b76deb425a7bd18766c2e` | **LGPL-2.1-or-later** |
| `gp_ep.h` | 1625 | `bae4f2bebe30958c6b013bf5be555ddac88d19bf148360385e82ec1302c94f6c` | **LGPL-2.1-or-later** |
| `dsp_dma.c` | 13062 | `4a19ab8e1d53a8e58d2301f777d509fc5dbf19daa04deb194b25465ec97a9369` | **LGPL-2.1-or-later** |
| `dsp_dma.h` | 2214 | `117a7db6ae3b7bc7f8bf47f9dad30610dd72b0affba7ab28cdb721be3e4c6ffd` | **LGPL-2.1-or-later** |
| `dsp_dma_regs.h` | 1542 | `9582796ee42313de2f341356761bdef514b8a9f9962199df8881a7461b78ea62` | **LGPL-2.1-or-later** |
| `trace.h` | 46 | `c4e97d9ea31a30015eb19eeedce0498f802fe3d099b295679e7a213245b45ff2` | no header (bare shim) |

Not vendored: `dsp_jit.c`, `dsp_jit.h`, `meson.build`, `trace-events`
(the JIT backend is a non-goal).

**Licence split.** The DSP core (`dsp.c`, `dsp_c.c`, `interp/*`, `dsp.h`, `dsp_internal.h`, `debug.h`) is GPL-2.0-or-later; the GP/EP glue (`gp_ep.*`) and the DMA layer (`dsp_dma*`) are LGPL-2.1-or-later, like the rest of `src/apu`. Because GPL files are linked in, the combined binary is GPL-2.0-or-later (see `NOTICE`).
`AC-LIC` must therefore list ported files **by their actual licence**, not lump them all
under GPL. The combined-work statement is unchanged: the GPL files make the linked binary
GPL-2.0-or-later.

## Four behaviours read from the pinned source

These are what the port's device semantics rely on (see `../GP-INTEGRATION.md`).

1. **The bootstrapping GPRST transition** — `gp_ep.c`, `proc_rst_write`:

   ```c
   static void proc_rst_write(DSPState *dsp, uint32_t oldval, uint32_t val)
   {
       if (!(val & NV_PAPU_GPRST_GPRST) || !(val & NV_PAPU_GPRST_GPDSPRST)) {
           dsp_reset(dsp);
       } else if (
           (!(oldval & NV_PAPU_GPRST_GPRST) || !(oldval & NV_PAPU_GPRST_GPDSPRST))
           && ((val & NV_PAPU_GPRST_GPRST) && (val & NV_PAPU_GPRST_GPDSPRST))) {
           dsp_bootstrap(dsp);
       }
   }
   ```

   So: **reset** whenever either bit is clear in the new value; **bootstrap** on the
   transition where either bit was clear in `oldval` and **both** are set in `val`. With
   `NV_PAPU_GPRST_GPRST = 1<<0` and `NV_PAPU_GPRST_GPDSPRST = 1<<1` (already defined in this
   toolkit's `apu_regs.h`), a write of `3` from `0` or from `1` bootstraps. This is what
   the port's bootstrap depends on, and it matches the R1 log (GPRST written `1` at L3190,
   then `3` at L3201 — the `1` write resets, the `3` write bootstraps, because `oldval = 1`
   has `GPDSPRST` clear).
2. **The GP-enable preference** — `dsp.c`, `dsp_init`:
   `if (g_config.audio.use_dsp_jit) { dsp_jit_init(dsp); } else { dsp_c_init(dsp); }`.
   Since the JIT is a non-goal, the port calls `dsp_c_init` **unconditionally**, which is
   what the port's device semantics require ("applied unconditionally in
   `mcpx_apu_update_dsp_preference` (no new environment variable)"). The name is
   `g_config.audio.use_dsp_jit`.
3. **Bootstrap loads PRAM from scratch** — `dsp_c.c`, `dsp_c_bootstrap`:
   `dsp->dma.scratch_rw(dsp->dma.rw_opaque, (uint8_t *)core->pram, 0, 0x800 * 4, false);`
   then masks every word with `0x00FFFFFF` and clears `pram_opcache`. **`0x800 * 4` bytes =
   `0x800` words**, and it reads from scratch address `0` — so `AC-BOOT`'s expectation that
   PRAM words come from the image at scratch offset 0 is correct, and the `& 0xFFFFFF`
   masking is part of the pinned code (not a local invention).
4. **The GP frame path** — `gp_ep.c`, `mcpx_apu_dsp_frame`: runs the GP only when
   `(GPRST & GPRST) && (GPRST & GPDSPRST)`, calling `dsp_start_frame`, then
   `dsp_run(dsp, 1000)` in a `do/while (!dsp_get_halt_requested && d->gp.realtime)` loop, and
   records `g_dbg.gp.cycles`. Note it writes the VP mixbins into GP XMEM at
   `GP_DSP_MIXBUF_BASE` **before** running the GP — that is the `MIXBUF` input `AC-INPUTS`
   must classify.

## A4b1 local modifications to the vendored files

The vendored files are stored byte-exact (`src/apu/dsp/.gitattributes` sets `* -text`), so the
**vendor commit** matches the pin record for all 17 files — verified when the port was accepted (JSRF packet A4b1-r4). The port then made **local modifications to 7 files**,
each marked in-source with `A4b1 LOCAL MODIFICATION` and the upstream line it replaces. Extracted
from the source, not transcribed from a report .

| File | Markers | What changed, and why |
|---|---|---|
| `dsp.c` | 5 | `:31` the toolkit's GP input-accounting hook in `read_peripheral`; `:79` the same before the trace call; `:140` the JIT branch removed so `dsp_c_init` is called **unconditionally** (`DS2` — the JIT is a non-goal); `:243` `dsp_set_engine` reduced; **`dsp_init` sets `dsp->dma.is_gp`**, the DMA's copy of the side flag (see `dsp_dma.h` below) |
| `dsp_c.c` | 5 | `:31` the toolkit's ledger include; `:62, :72, :99, :195` a `s_last_cycle_count` delta so the ledger records the **per-frame** instruction count rather than a cumulative one |
| `dsp_internal.h` | 1 | `:25` the `jit_dsp_ops` / `dsp_jit_init` declarations removed — `dsp_jit.*` is deliberately absent |
| `dsp_dma.h` | 1 | **`DSPDMAState` gained `bool is_gp`** — which DSP the DMA belongs to. `dsp_dma.c` is shared by the GP and the EP (both run frames, `gp_ep.c:599` and `:649`) and `rw_opaque` does not identify the side, so the `FIFO_READ` hook must be gated. Nothing `memcpy`s or serializes `DSPDMAState`, so the added field cannot disturb state sync |
| `dsp_dma.c` | 2 | `:27` the ledger include; **the read arm's `else` branch** — the `FIFO_READ` input hook (see below) |
| `interp/dsp_cpu.c` | 2 | `:32` the ledger include; `:910` the `MIXBUF` input hook on the mix-buffer read |
| `gp_ep.c` | 13 | `:24` the ledger include; `:56` `scatter_gather_rw` routed through `DS3` + `DS5`; `:129` the bootstrap scratch-read accounting; `:241` the `FIFO_READ` hook; `:334` `proc_rst_write` per `DS1`; `:404, :470, :512, :544` **GNU case ranges → if/else-if in all four MMIO switches**; `:573` `mcpx_apu_dsp_frame` per `DS2`; `:641` the EP monitor passthrough kept **outside** the GP branch; `:664` `mcpx_apu_dsp_init`; `:689` the startup line |

**Total: 29 markers across 7 files.**

**The `dsp_dma.c` read-arm hook — the Advisor's ruling (C), 2026-09-26.** The pinned read arm
implements only `buf_id` `0xE`/`0xF`. For any other id it prints `"Unhandled DSP DMA buffer"` and then
**falls through**, because the following `assert(!"Unhandled dsp dma buffer")` is **compiled out** — the
APU library is built with **`NDEBUG`** (`build/xboxrecomp/src/apu/xbox_apu.vcxproj`, every Release
`PreprocessorDefinitions`). The loop then `mem_write`s `scratch_buf` into DSP memory, and `scratch_buf`
is the file-static intermediate buffer, so the DSP **consumes stale bytes as its input**. That is a real
GP input, and `DS6` requires every GP input path to be recorded. The hook records it:

- `buf_id < GP_INPUT_FIFO_COUNT` → `apu_gpin_fifo_read(buf_id, transfer_size)`;
- any other `buf_id` → `apu_gpin_record_out_of_universe(APU_WATCH_GPIN_FIFO, buf_id)`, failing closed
  to `UNKNOWN` rather than silently unaccounted;
- **gated on `s->is_gp`** — without the gate an EP fall-through would be recorded as a GP input, a
  false `R2-EXPL-INPUT` path in the opposite direction from the gap being closed.

The `gp_fifo_rw` hook (`gp_ep.c:251`, `if (!dir)`) is **kept** even though it is unreachable at this
pin: it covers a future pin that wires input FIFOs through `fifo_rw`. The two can never both fire for
one transfer, because the read arm never calls `fifo_rw`. Upstream `master` is **identical** here (the
`FIXME` and the hardcoded `1` are still present), so the unimplemented read arm is not a pin artefact.

**This closed a hook-completeness gap that `AC-FIX (viii)` found.** The first reading was that
`FIFO_READ` was structurally 0 and the criterion needed a claim limit; the Advisor's ruling (C) reversed
that, and `NDEBUG` was verified before implementing. See
the A4b1-r4 execution rulings (JSRF game repository history).

**The GNU case ranges were the first real build blocker.** The pinned `gp_ep.c` uses
`case A ... B:` (a GCC extension) in four MMIO switches, which MSVC rejects. Each was rewritten as
an `if`/`else if` chain with **identical tests, identical order and identical bodies**, so the
behaviour is unchanged. This is the one modification class that is mechanical rather than semantic.

**Files modified but carrying no marker are a defect**: `AC-PORT` step 1 requires this list to be
complete; the working-tree modifications were verified by hash against the pin.

## How to reproduce this record

```powershell
$sha = '67cc79e663038d1f55448c0f566b37dde016adf6'
$base = "https://raw.githubusercontent.com/xemu-project/xemu/$sha/hw/xbox/mcpx/apu/dsp/"
# for each file in the table: fetch, then Get-FileHash -Algorithm SHA256
```

The table above is the authoritative per-file record.

## QEMU shim surface (`shim/`)

The pinned files include QEMU headers that do not exist in this toolkit. The shims under `shim/` are only no-ops or pass-throughs, and their surface was inventoried from the pinned code rather than assumed. The total surface is small:

| Family | Symbols the pinned code actually uses |
|---|---|
| **QEMU memory/device API** | `address_space_memory`, `MemoryRegion`, `memory_region_size`, `memory_region_set_dirty`, `qemu_mutex_lock`, `qemu_mutex_unlock` |
| **QEMU trace** | `trace_dsp_read_peripheral`, `trace_dsp_write_peripheral`, `trace_dsp56k_execute_instruction`, `trace_dsp56k_execute_instruction_disasm`, `trace_event_get_state` |
| **xemu settings** | `g_config`, `use_dsp_jit` (only to choose the backend — the port calls `dsp_c_init` unconditionally, so this becomes a no-op) |
| **bswap / endian** | `bswap`, `ldl_le_phys`, `ldl_le_p`, `stl_le_p`, `GET_MASK` |
| **glib allocation** | `g_new0`, `g_free` |
| **logging** | `assert`, `fprintf`, `printf`, `DPRINTF`, `DEBUG_DSP` |

**Per-file QEMU header usage** (which is what the shims must satisfy):

| Pinned file | QEMU headers it includes |
|---|---|
| `dsp.c` | `qemu/osdep.h`, `trace.h`, `ui/xemu-settings.h` |
| `dsp_c.c` | `qemu/osdep.h` |
| `dsp_dma.c` | `qemu/osdep.h`, `qemu/compiler.h` |
| `gp_ep.c` | `hw/xbox/mcpx/apu/apu_int.h` |
| `gp_ep.h` | `qemu/osdep.h`, `hw/hw.h`, `hw/pci/pci.h`, `hw/xbox/mcpx/apu/apu_regs.h` |
| `interp/dsp_cpu.c` | `qemu/osdep.h`, `qemu/bswap.h`, `trace.h` |
| `trace.h` | `trace/trace-hw_xbox_mcpx_apu_dsp.h` |

The pinned code also uses **131 distinct `DSP_*`/`NV_PAPU_*` constants** which this toolkit's
`apu_regs.h` already defines — so the register-file surface needs no new definitions.
