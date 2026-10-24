# Xbox Input → XInput Compatibility Layer

add_library(xbox_input STATIC
    xinput_device.c
    keyboard.c          # the key table and the one key map (every host)
    pad_map.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The mapping rules (bindings, remapping, deadzones), also buildable alone;
# keyboard.c resolves its key map through them.
add_subdirectory(mapping)
target_link_libraries(xbox_input PUBLIC xbox_input_map)

if(WIN32)
    # Mouse+keyboard pad (RECOMP_MKB) reads Win32 raw input.
    target_sources(xbox_input PRIVATE mkb_input.c)
    # mingw-w64's libxinput.a imports XINPUT1_3.dll, the DirectX June 2010
    # redistributable: stock Windows 10/11 lacks it and the exe dies with
    # 0xC0000135 before main. Proton ships it, so only native Windows shows
    # this. XINPUT1_4.dll is part of Windows since 8. The Windows SDK's
    # xinput.lib already imports 1_4 and the SDK has no xinput1_4.lib, so
    # MSVC keeps the plain name. tests/input_xinput_imports checks the result.
    target_link_libraries(xbox_input PUBLIC platform
        $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>)
else()
    find_package(SDL2 REQUIRED)
    # The SDL window's key and focus events into that table.
    target_sources(xbox_input PRIVATE keyboard_sdl.c)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()

# The key table and the one key map, tested against the code the runtime
# links (tests/input_keyboard, also a standalone project). Pure C, so it runs
# on both platforms.
if(BUILD_TESTING)
    get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
    add_executable(input_keyboard_test
        keyboard.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/input_keyboard/test_main.c
    )
    target_include_directories(input_keyboard_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${XBOXRECOMP_SRC_DIR}
    )
    # keyboard.c resolves its key map through the mapping rules.
    target_link_libraries(input_keyboard_test PRIVATE platform xbox_input_map)
    add_test(NAME input_keyboard COMMAND input_keyboard_test)
    set_tests_properties(input_keyboard PROPERTIES TIMEOUT 20)
endif()

# The XInput -> Xbox pad mapping and the rumble report parser. Pure C, so it
# links pad_map.c alone and runs on both platforms.
if(BUILD_TESTING)
    add_executable(pad_map_test
        pad_map.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/pad_map/pad_map_test.c
    )
    target_include_directories(pad_map_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${XBOXRECOMP_SRC_DIR}
    )
    target_link_libraries(pad_map_test PRIVATE platform)
    add_test(NAME pad_map COMMAND pad_map_test)
    set_tests_properties(pad_map PROPERTIES TIMEOUT 20)
endif()
