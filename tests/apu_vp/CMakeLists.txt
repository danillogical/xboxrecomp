cmake_minimum_required(VERSION 3.20)
project(apu_vp_test C)
if(NOT WIN32)
    message(FATAL_ERROR "The voice-lock checks use Windows threads")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
# test_main.c compiles apu_vp.c itself, to reach its static helpers, and
# stands in for the little it uses from the rest of the APU.
# fork_stubs.c and fork_compat.h fit upstream's test to this fork's APU.
add_executable(apu_vp_test test_main.c fork_stubs.c)
if(MSVC)
    target_compile_options(apu_vp_test PRIVATE
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/fork_compat.h")
else()
    target_compile_options(apu_vp_test PRIVATE
        "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/fork_compat.h")
endif()
# The fork's apu headers reach the pinned DSP56300 core (src/apu/dsp), whose
# includes resolve through its shim directories.
target_include_directories(apu_vp_test PRIVATE ../../src ../../src/apu
    ../../src/apu/dsp ../../src/apu/dsp/shim ../../src/apu/dsp/interp)
target_compile_definitions(apu_vp_test PRIVATE _USE_MATH_DEFINES)
enable_testing()
add_test(NAME apu_vp COMMAND apu_vp_test)
