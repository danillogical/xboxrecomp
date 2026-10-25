# Standalone test projects that do not pass in this fork

Each `tests/<name>/` with its own `project()` is built and run on its own
(Windows: `cmake -S tests/<name> -B build/sub-<name> -A x64`, then build and
`ctest`). These do not pass here, and none of them is a regression. Recorded at
the v0.16.0 merge (`merge-v0.16.0`), so a later run can tell them from a new
failure. Kept in the tree rather than deleted: they record upstream's design,
and they pass again if the fork adopts it.

## Upstream tests of an upstream design this fork does not carry

| Project | What fails | The fork's design, and where it was decided |
|---|---|---|
| `vblank_ack` | link: `xbox_VblankMode`, `xbox_VblankAckWait`, `xbox_VblankRaiseBits`, `xbox_VblankArmBits` unresolved | Upstream's bridge vblank (`RECOMP_VBLANK` tick, 60 Hz schedule, vblank-ack thread) is not taken: the fork's vblank is the NV2A model's display clock, and `RECOMP_VBLANK` is retired (poison-jam `scripts/jsrf_run_profile.py` `RETIRED_OVERRIDES`). |
| `kernel_io_apc` | "inline: the APC runs during NtReadFile" and "deferred: ... returns STATUS_USER_APC" | File I/O APCs are deferred by default (`xbox_SetDeferredIoApc(1)` is the fork's default; JSRF's ADX poller needs it), and an alertable wait on an object that is already signalled returns its status before any APC, as NT orders it. |
| `dpc_order` | case (f): "a routine that queues itself runs twice in one drain" | Every drain runs one snapshot of the queue, so a DPC that queues itself runs on the next drain (`kernel_drain_dpcs`, `xbox_DpcDrainHere`). |
| `input_keyboard_map` | "subset: packet number did not move" | The keyboard pad's packet number moves only when the state changes (v0.14.0 merge, `0828592`); the pad contents match. |

## Windows refuses them by design

`input_keyboard_sdl`, `kernel_directory_posix`, `kernel_memory_posix`,
`memory_layout_posix`, `usb_ohci_posix`, `win32_compat_posix`,
`win32_thread_posix`: POSIX-only, `message(FATAL_ERROR)` on WIN32.
`input_xinput_imports`: needs a mingw `objdump`, so it refuses under MSVC.

## Failing on `main` before the merge

`adpcm_decode`, `apu_mixdown`: `gp_ep.h(24,1): error C1083: Cannot open
include file: 'qemu/osdep.h'`. Their include directories predate the pinned
DSP56300 core (`src/apu/dsp`), whose headers need `src/apu/dsp/shim`; adding
it, as `apu_vp` does, is the fix. (`apu_vp`, upstream's, also takes
`fork_compat.h` and `fork_stubs.c` for the two names this fork's APU spells
differently.) `tools/posix_check.py native` builds both
with the right directories and they pass there.
