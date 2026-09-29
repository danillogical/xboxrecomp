cmake_minimum_required(VERSION 3.20)

# Xbox Kernel Replacement Layer
# Replaces 147 Xbox kernel imports with Win32 equivalents.
# Memory layout, file I/O, threading, synchronization, crypto, HAL.

set(KERNEL_SOURCES
    kernel_path.c
    kernel_pool.c
    kernel_rtl.c
    kernel_memory.c
    kernel_file.c
    kernel_thread.c
    kernel_sync.c
    kernel_hal.c
    kernel_xbox.c
    kernel_ob.c
    kernel_io.c
    kernel_crypto.c
    kernel_thunks.c
    kernel_bridge.c
    guest_meter.c
    xbox_memory_layout.c
    kmem.c
    kernel_vm.c
    icall_feedback.c
    recomp_trace.c
    nv2a_pb_scan.c
    nv2a_pb_exec.c
)

add_library(xbox_kernel STATIC ${KERNEL_SOURCES})

# See src/platform/CMakeLists.txt: resolve src/ relative to this file, not via
# CMAKE_SOURCE_DIR, which is the consuming project's root under add_subdirectory().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

target_include_directories(xbox_kernel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XBOXRECOMP_SRC_DIR}
)

if(WIN32)
    # platform is an INTERFACE target on Windows: it contributes the src/
    # include path that "platform/xbox_winnt.h" resolves against.
    target_link_libraries(xbox_kernel PUBLIC platform kernel32 bcrypt)
else()
    # Linux: platform layer (POSIX host primitives) + OpenSSL for crypto.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(xbox_kernel PUBLIC platform OpenSSL::Crypto)
endif()

if(MSVC)
    target_compile_options(xbox_kernel PRIVATE /W4 /WX- /O2 /GS- /Gy)
    target_compile_definitions(xbox_kernel PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(xbox_kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_compile_definitions(xbox_kernel PRIVATE $<$<CONFIG:Debug>:_DEBUG>)

if(BUILD_TESTING AND WIN32)
    add_executable(xbox_timestamp_test
        kernel_hal.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/kernel_timestamp_test.c
    )
    target_compile_definitions(xbox_timestamp_test PRIVATE
        XBOXRECOMP_TIMESTAMP_TEST_BUILD)
    target_include_directories(xbox_timestamp_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${XBOXRECOMP_SRC_DIR}
    )
    target_link_libraries(xbox_timestamp_test PRIVATE platform kernel32)
    add_test(NAME xbox_timestamp_publication COMMAND xbox_timestamp_test)
    set_tests_properties(xbox_timestamp_publication PROPERTIES TIMEOUT 20)

    add_executable(xbox_guest_meter_test
        guest_meter.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/guest_meter_test.c
    )
    target_compile_definitions(xbox_guest_meter_test PRIVATE
        XBOXRECOMP_GMETER_TEST_BUILD)
    target_include_directories(xbox_guest_meter_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${XBOXRECOMP_SRC_DIR}
    )
    target_link_libraries(xbox_guest_meter_test PRIVATE platform kernel32)
    add_test(NAME xbox_guest_meter COMMAND xbox_guest_meter_test)
    set_tests_properties(xbox_guest_meter PROPERTIES TIMEOUT 60)
endif()

# The allocator and kernel-memory decisions in kmem.c have no host dependency,
# so their regression runs on every host, not only on Windows.
if(BUILD_TESTING)
    add_executable(xbox_kmem_test
        kmem.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/kmem_test.c
    )
    target_include_directories(xbox_kmem_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME xbox_kmem COMMAND xbox_kmem_test)
    set_tests_properties(xbox_kmem PROPERTIES TIMEOUT 20)
endif()
