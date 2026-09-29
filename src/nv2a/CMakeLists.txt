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
endif()
