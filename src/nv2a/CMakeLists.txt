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

    # The host clock must not jump backward at the QPC product wrap.
    #
    # A pure-arithmetic test with no project dependencies: it reproduces the
    # shim's own expression (old and current) over the wrap boundary, so it
    # needs no device and no kernel. That is deliberate -- the defect is in a
    # formula, and a test that called the function through the shim could not
    # observe the overflowing intermediate.
    add_executable(host_clock_wrap_test
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/host_clock_wrap_test.c
    )
    if(NOT MSVC)
        target_compile_options(host_clock_wrap_test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME host_clock_wrap COMMAND host_clock_wrap_test)
    set_tests_properties(host_clock_wrap PROPERTIES TIMEOUT 20)

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
endif()
