# Xbox Input → XInput Compatibility Layer

add_library(xbox_input STATIC
    xinput_device.c
    keyboard_pad.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(xbox_input PUBLIC platform xinput)
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()

# The Enter -> Xbox Start boundary, tested against the mapping the runtime
# links. Runs on both platforms: the mapping table is the same code, and the
# only thing it needs from the host is a key-state callback the test supplies.
if(BUILD_TESTING)
    get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
    add_executable(keyboard_pad_test
        keyboard_pad.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/keyboard_pad/keyboard_pad_test.c
    )
    target_include_directories(keyboard_pad_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${XBOXRECOMP_SRC_DIR}
    )
    target_link_libraries(keyboard_pad_test PRIVATE platform)
    add_test(NAME keyboard_pad COMMAND keyboard_pad_test)
    set_tests_properties(keyboard_pad PROPERTIES TIMEOUT 20)
endif()
