# MCPX APU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Voice Processor (VP): ADPCM/PCM decode, sample mixing, envelopes, filters
# Global Processor (GP): the pinned xemu DSP56300 core (A4b1), interpreter only
# Encode Processor (EP): unrouted (A4b1 non-goal); the monitor keeps the VP
#                         mixbin passthrough

# The pinned xemu DSP56300 core, vendored byte-for-byte from
# 67cc79e663038d1f55448c0f566b37dde016adf6 under src/apu/dsp/. Per-file
# provenance, SHA-256 and licence are in docs/reviews/a4b-xemu-pin.md.
#
# dsp_jit.* is deliberately ABSENT: the JIT backend is an A4b1 non-goal, and
# dsp.c/dsp_internal.h carry marked local modifications that remove the
# references to it.
#
# interp/debug.c is also absent. Upstream it is guarded end to end by
# `#if DEBUG_DSP`, and DEBUG_DSP is 0 here, so it would compile to an empty
# translation unit; it implements an interactive debugger that nothing in the
# pinned set calls and this toolkit has no console for. A4b1 step 1 says to add
# it "only if [it is] needed to compile" -- it is not. The vendored copy stays in
# the tree, byte-exact, because the pin record lists it.
#
# apu_dsp.c is GONE, and that is a step-5 consequence rather than a tidy-up. It
# used to hold the passthrough stub, the synthetic ack and the three DSP entry
# points. Device semantics 2 and 4 replace all of that: the three entry points
# are now the pinned gp_ep.c definitions, and the ack is deleted. What was left
# was an empty translation unit, so the file is removed and the reasons are
# recorded here and in the pin record's local-modification list.
set(XBOXRECOMP_DSP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dsp)

add_library(xbox_apu STATIC
    apu_core.c
    apu_vp.c
    apu_mixdown.c
    apu_watch.c
    apu_mmio_hook.c
    apu_xaudio2.c

    ${XBOXRECOMP_DSP_DIR}/dsp.c
    ${XBOXRECOMP_DSP_DIR}/dsp_c.c
    ${XBOXRECOMP_DSP_DIR}/dsp_dma.c
    ${XBOXRECOMP_DSP_DIR}/gp_ep.c
    ${XBOXRECOMP_DSP_DIR}/interp/dsp_cpu.c
)

target_include_directories(xbox_apu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# qemu_shim.h lives in nv2a/ - shared QEMU type abstraction.
#
# dsp/shim/ supplies the QEMU headers the pinned files include by their upstream
# paths (qemu/osdep.h, hw/hw.h, hw/pci/pci.h, hw/xbox/mcpx/apu/apu_regs.h,
# hw/xbox/mcpx/apu/apu_int.h, ui/xemu-settings.h, trace/trace-*.h). Every one of
# them is a no-op or a pass-through onto the toolkit's existing shim; see
# apu_shim.h and the shim tree's own comments.
#
# dsp/ itself is on the path so the pinned files' own includes resolve
# (dsp_internal.h, dsp.h, dsp_dma.h, debug.h, interp/*.h).
target_include_directories(xbox_apu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nv2a
    ${XBOXRECOMP_DSP_DIR}
    ${XBOXRECOMP_DSP_DIR}/shim
)

# The translation function, the ledger's high-water mark and the low-RAM bound
# are the toolkit's own guest-memory geometry (src/kernel/xbox_memory_layout.c),
# read rather than re-derived -- see apu_watch.c. That is a header dependency
# only: no symbol is taken from xbox_kernel, so the APU library stays linkable
# on its own, as the standalone test targets require.
target_include_directories(xbox_apu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

if(MSVC)
    target_compile_options(xbox_apu PRIVATE
        /W3
        /wd4244  # conversion warnings (float/double/int)
        /wd4146  # unary minus on unsigned
        /wd4267  # size_t to int conversion
    )
    target_compile_definitions(xbox_apu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
endif()

if(WIN32)
    # xaudio2_8 for XAudio2Create, which apu_xaudio2.c calls directly.
    # It was missing: nothing had ever referenced this library from a linked
    # target, so the archive was built and never pulled in, and the first
    # target to actually call into the APU failed at link with an undefined
    # XAudio2Create rather than at build time here.
    target_link_libraries(xbox_apu PRIVATE platform kernel32 winmm xaudio2_8 ole32)
else()
    # Linux: platform layer (Win32 primitives on POSIX) + SDL2 audio + libm
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_apu PRIVATE platform SDL2::SDL2 m)
endif()
