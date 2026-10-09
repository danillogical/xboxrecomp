# NV2A GPU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Register handlers: PMC, PBUS, PTIMER, PFB, PCRTC, PRAMDAC, PFIFO, PGRAPH
# MMIO interception via VEH (Vectored Exception Handling)
# Push buffer parsing and command dispatch

add_library(xbox_nv2a STATIC
    nv2a_core.c
    nv2a_method_table.c
    nv2a_pgraph_d3d11.c
    nv2a_pb_replay.c
    nv2a_mmio_hook.c
    nv2a_pb_test.c
)

target_include_directories(xbox_nv2a PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_nv2a PRIVATE
        /W3
        /wd4244
        /wd4146
        /wd4267
    )
    target_compile_definitions(xbox_nv2a PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(xbox_nv2a PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_link_libraries(xbox_nv2a PRIVATE platform)
if(WIN32)
    target_link_libraries(xbox_nv2a PRIVATE kernel32)
endif()

if(BUILD_TESTING)
    # The walk and the PGRAPH interrupt registers, driven through the real MMIO
    # entry points. The D3D11 translator is replaced by a recorder in the test.
    add_executable(nv2a_actions_test
        nv2a_core.c
        nv2a_method_table.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/nv2a_actions_test.c
    )
    target_include_directories(nv2a_actions_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(NOT MSVC)
        target_compile_options(nv2a_actions_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    target_link_libraries(nv2a_actions_test PRIVATE platform)
    if(WIN32)
        target_link_libraries(nv2a_actions_test PRIVATE kernel32)
    endif()
    add_test(NAME nv2a_actions COMMAND nv2a_actions_test)
    set_tests_properties(nv2a_actions PROPERTIES TIMEOUT 20)

    # Rejection/recovery log lines, the published submit state, and the
    # RECOMP_NV2A_ADMIT_UNKNOWN switch.
    add_executable(nv2a_submit_diag_test
        nv2a_core.c
        nv2a_method_table.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/nv2a_submit_diag_test.c
    )
    target_include_directories(nv2a_submit_diag_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(NOT MSVC)
        target_compile_options(nv2a_submit_diag_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    target_link_libraries(nv2a_submit_diag_test PRIVATE platform)
    if(WIN32)
        target_link_libraries(nv2a_submit_diag_test PRIVATE kernel32)
    endif()
    add_test(NAME nv2a_submit_diag COMMAND nv2a_submit_diag_test)
    set_tests_properties(nv2a_submit_diag PROPERTIES TIMEOUT 20)

    # The per-walk page-validation cache must not weaken the read guard.
    #
    # `submit_read_word` used to call `VirtualQuery` per word; caching the
    # validated span is worth ~1.5 s per 8144-word walk on this host, but a cache
    # that validated too wide a span would let the walk read a PAGE_GUARD page
    # and fault on a bad ring instead of refusing it. This drives the real
    # `nv2a_submit_pending` against a window with a REAL guard page in the middle
    # and requires the refusal to survive repetition, with a positive control so
    # a guard that refused everything could not pass.
    add_executable(nv2a_read_guard_test
        nv2a_core.c
        nv2a_method_table.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/nv2a_read_guard_test.c
    )
    target_include_directories(nv2a_read_guard_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(NOT MSVC)
        target_compile_options(nv2a_read_guard_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    target_link_libraries(nv2a_read_guard_test PRIVATE platform)
    if(WIN32)
        target_link_libraries(nv2a_read_guard_test PRIVATE kernel32)
    endif()
    add_test(NAME nv2a_read_guard COMMAND nv2a_read_guard_test)
    set_tests_properties(nv2a_read_guard PROPERTIES TIMEOUT 20)

    # The host clock must not jump backward at either QPC product wrap.
    #
    # This calls the REAL shipped conversions (`nv2a_qpc_to_ns` /
    # `nv2a_qpc_to_us` in src/nv2a/host_clock.h), which is what
    # `qemu_clock_get_ns` and `qemu_clock_get_us` return. An earlier version
    # duplicated the formula instead, so reverting the shim left it green; the
    # pre-fix expression is now also in that header, as
    # `nv2a_qpc_to_ns_overflowing`, so the control arm runs the same arithmetic
    # the runtime used to.
    add_executable(host_clock_wrap_test
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/host_clock_wrap_test.c
    )
    target_include_directories(host_clock_wrap_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(NOT MSVC)
        target_compile_options(host_clock_wrap_test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME host_clock_wrap COMMAND host_clock_wrap_test)
    set_tests_properties(host_clock_wrap PROPERTIES TIMEOUT 20)

    # The monotonic telemetry clock must share one zero across translation units.
    # Two files are needed: an anchor held per file (a function-local static in
    # the inline reader) gives each .c file its own epoch, which one file cannot show.
    add_executable(nv2a_mono_clock_test
        nv2a_core.c
        nv2a_method_table.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/nv2a_mono_clock_test.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/nv2a_mono_clock_test_b.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/nv2a_mono_clock_backend_stub.c
    )
    target_include_directories(nv2a_mono_clock_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    if(NOT MSVC)
        target_compile_options(nv2a_mono_clock_test PRIVATE -Wall -Wextra)
    endif()
    target_link_libraries(nv2a_mono_clock_test PRIVATE platform)
    if(WIN32)
        target_link_libraries(nv2a_mono_clock_test PRIVATE kernel32)
    endif()
    add_test(NAME nv2a_mono_clock COMMAND nv2a_mono_clock_test)
    set_tests_properties(nv2a_mono_clock PROPERTIES TIMEOUT 20)

    # The vblank service loop's scheduling rule must survive a clock that steps
    # backward. `nv2a_vblank_advance` is the rule as a pure function, so this
    # compiles only nv2a_core.c (like the submit tests) and needs no device,
    # no thread and no D3D11 backend.
    add_executable(vblank_clock_step_test
        nv2a_core.c
        nv2a_method_table.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/vblank_clock_step_test.c
    )
    target_include_directories(vblank_clock_step_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(NOT MSVC)
        target_compile_options(vblank_clock_step_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    target_link_libraries(vblank_clock_step_test PRIVATE platform)
    if(WIN32)
        target_link_libraries(vblank_clock_step_test PRIVATE kernel32)
    endif()
    add_test(NAME vblank_clock_step COMMAND vblank_clock_step_test)
    set_tests_properties(vblank_clock_step PROPERTIES TIMEOUT 20)

    # The late-rearm counter must be able to FAIL. Archived runs pulse 30-80x
    # below nominal, and `nv2a_vblank_advance` re-arms WITHOUT pulsing on a late
    # wake -- so if every pass is late the loop never pulses. This drives the
    # real rule through a simulated service loop and requires the counter to
    # separate a punctual loop (zero late, pulses) from a persistently late one
    # (all late, no pulses), with a positive control so a stuck-at-zero counter
    # cannot pass. Like the clock-step test it compiles only nv2a_core.c and
    # needs no device, thread or backend.
    add_executable(vblank_sched_telemetry_test
        nv2a_core.c
        nv2a_method_table.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/vblank_sched_telemetry_test.c
    )
    target_include_directories(vblank_sched_telemetry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(NOT MSVC)
        target_compile_options(vblank_sched_telemetry_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    target_link_libraries(vblank_sched_telemetry_test PRIVATE platform)
    if(WIN32)
        target_link_libraries(vblank_sched_telemetry_test PRIVATE kernel32)
    endif()
    add_test(NAME vblank_sched_telemetry COMMAND vblank_sched_telemetry_test)
    set_tests_properties(vblank_sched_telemetry PROPERTIES TIMEOUT 20)
endif()
