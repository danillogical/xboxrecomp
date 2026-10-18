# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
    nv2a_d3d11_backend.c
    nv2a_vk_backend.c
    nv2a_vsh_hlsl.c
)
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
# xbox_input: the windows record keys in its key table (keyboard.c).
target_link_libraries(xbox_video PUBLIC xbox_d3d8 xbox_input)
if(WIN32)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32)
    # The NV2A D3D11 back end, which registers with the pushbuffer executor in
    # xbox_kernel; it loads d3dcompiler at run time, so none is linked.
    target_link_libraries(xbox_video PUBLIC d3d11 dxgi dxguid xbox_kernel)
else()
    # The framebuffer window on POSIX hosts: SDL2, on the process main thread
    # (fb_present_sdl.c), which then also pumps the events the game
    # controllers are read from.
    find_package(SDL2 REQUIRED)
    target_sources(xbox_video PRIVATE fb_present_sdl.c)
    target_link_libraries(xbox_video PUBLIC SDL2::SDL2)
endif()

# The vertex-program translator as a host tool: the game's corpus generator runs it
# over each program the title uploads, with --vk for the annotations the Vulkan
# pipeline needs, and feeds the output to dxc. It links the translator alone -- no
# Vulkan, no Windows libraries -- so it builds with or without Vulkan found.
add_executable(nv2a_vsh_hlsl nv2a_vsh_hlsl_tool.c nv2a_vsh_hlsl.c)
target_include_directories(nv2a_vsh_hlsl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(nv2a_vsh_hlsl PRIVATE /W3)
    target_compile_definitions(nv2a_vsh_hlsl PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# The NV2A Vulkan back end (nv2a_vk_backend.c), built only with Vulkan and its shader
# tools; otherwise that file is a stub that reports failure. Its shaders are SPIR-V
# compiled here, and it opens the Vulkan loader at run time, so only headers are used.
if(XBR_VULKAN_FOUND)
    target_compile_definitions(xbox_video PRIVATE XBR_VK_BACKEND=1)
    target_include_directories(xbox_video PRIVATE ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(xbox_video PUBLIC xbox_kernel ${CMAKE_DL_LIBS})
    xbr_add_spirv(xbox_video SOURCE ${CMAKE_CURRENT_LIST_DIR}/shaders/nv2a_vk_stage0.hlsl
        ENTRY vs_main PROFILE vs_6_0 INVERT_Y SYMBOL xbr_nv2a_vk_stage0_vs)
    xbr_add_spirv(xbox_video SOURCE ${CMAKE_CURRENT_LIST_DIR}/shaders/nv2a_vk_stage0.hlsl
        ENTRY ps_main PROFILE ps_6_0 SYMBOL xbr_nv2a_vk_stage0_ps)
    xbr_add_spirv(xbox_video SOURCE ${CMAKE_CURRENT_LIST_DIR}/shaders/nv2a_vk_rc.hlsl
        ENTRY vs_rc PROFILE vs_6_0 INVERT_Y SYMBOL xbr_nv2a_vk_rc_vs)
    xbr_add_spirv(xbox_video SOURCE ${CMAKE_CURRENT_LIST_DIR}/shaders/nv2a_vk_rc.hlsl
        ENTRY ps_rc PROFILE ps_6_0 SYMBOL xbr_nv2a_vk_rc_ps)
    # The readback path's pack kernel: the 16-bit targets are 5:6:5-truncated on the GPU, so
    # the host only memcpy's rows (nv2a_vk_backend.c, vb_targets_flush). A compute entry, so
    # no invert-y; the pack pipeline is created from xbr_nv2a_vk_pack_cs.spv.h.
    xbr_add_spirv(xbox_video SOURCE ${CMAKE_CURRENT_LIST_DIR}/shaders/nv2a_vk_pack.hlsl
        ENTRY cs_pack PROFILE cs_6_0 SYMBOL xbr_nv2a_vk_pack_cs)
endif()

# Vulkan back-end plumbing: a smoke test that clears and draws on a real device and on
# the software ICD, using shaders compiled ahead of time (see README.md).
if(XBR_VULKAN_FOUND AND BUILD_TESTING)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/vk_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/vk_smoke)
endif()
