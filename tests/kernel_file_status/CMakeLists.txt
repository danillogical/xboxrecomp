cmake_minimum_required(VERSION 3.20)
project(kernel_file_status_regression LANGUAGES C)
# What a failed open returns (test_main.c), cross-platform: xbox_NtCreateFile
# called directly, so it covers the Win32 backend on Windows/Proton and the
# POSIX one elsewhere. The name-collision check (kernel_file_status_test.c)
# currently targets Windows.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
enable_testing()
add_executable(kernel_file_status_test test_main.c)
target_include_directories(kernel_file_status_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_file_status_test PRIVATE xboxrecomp)
add_test(NAME kernel_file_status_test COMMAND kernel_file_status_test)
if(WIN32)
    add_executable(kernel-file-status-test kernel_file_status_test.c)
    target_include_directories(kernel-file-status-test PRIVATE ../../src/kernel)
    target_link_libraries(kernel-file-status-test PRIVATE xboxrecomp)
    add_test(NAME kernel-file-status COMMAND kernel-file-status-test)
endif()
