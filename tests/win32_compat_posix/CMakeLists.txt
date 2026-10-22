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
