cmake_minimum_required(VERSION 3.20)
project(win32_compat_posix_test C)
# The fixed-address half of the Win32 shim, on its own. Only the platform
# library is built: these are properties of VirtualAlloc and MapViewOfFileEx,
# not of the Xbox layout that sits on them.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX mapping shim")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../../src/platform platform)
add_executable(win32_compat_posix_test test_main.c)
# libm for the shim's fenv calls; the full runtime pulls it in elsewhere.
target_link_libraries(win32_compat_posix_test PRIVATE platform m)
enable_testing()
add_test(NAME win32_compat_posix_test COMMAND win32_compat_posix_test)

# Interlocked ops, MemoryBarrier and GetThreadId: Win32 semantics, not layout.
add_executable(win32_interlocked_posix_test test_interlocked.c)
target_link_libraries(win32_interlocked_posix_test PRIVATE platform m)
add_test(NAME win32_interlocked_posix_test COMMAND win32_interlocked_posix_test)

# Handle provenance and lifetime: a HANDLE is a value the shim publishes, so
# only a live entry may be dereferenced. This runs the plain binary; the
# AddressSanitizer companion below runs the same lifetime shapes where a
# use-after-free is diagnosed instead of merely surviving.
add_executable(win32_handle_lifetime_posix_test test_handle_lifetime.c)
target_link_libraries(win32_handle_lifetime_posix_test PRIVATE platform m)
add_test(NAME win32_handle_lifetime_posix_test COMMAND win32_handle_lifetime_posix_test)
set_tests_properties(win32_handle_lifetime_posix_test PROPERTIES TIMEOUT 60)

# The same lifetime contract under ASAN when the compiler supports it. The
# guard is a real compile test, not a platform name: a host whose cc has no
# libasan keeps the plain test above and skips this one visibly.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=address")
check_c_source_compiles("int main(void){return 0;}" XBR_HAVE_ASAN)
unset(CMAKE_REQUIRED_FLAGS)
if(XBR_HAVE_ASAN)
    add_executable(win32_handle_lifetime_asan_test test_handle_lifetime_asan.c)
    target_compile_options(win32_handle_lifetime_asan_test PRIVATE -fsanitize=address -g -O1)
    target_link_options(win32_handle_lifetime_asan_test PRIVATE -fsanitize=address)
    target_link_libraries(win32_handle_lifetime_asan_test PRIVATE platform m)
    add_test(NAME win32_handle_lifetime_asan_test COMMAND win32_handle_lifetime_asan_test)
    set_tests_properties(win32_handle_lifetime_asan_test PROPERTIES TIMEOUT 180)
else()
    message(STATUS "win32_handle_lifetime_asan_test not registered -- this cc has no -fsanitize=address")
endif()
