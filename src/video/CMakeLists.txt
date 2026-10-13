# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
    nv2a_d3d11_backend.c
    nv2a_vsh_hlsl.c
)
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(xbox_video PUBLIC xbox_d3d8)
if(WIN32)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32)
    # The NV2A D3D11 back end, which registers with the pushbuffer executor in
    # xbox_kernel; it loads d3dcompiler at run time, so none is linked.
    target_link_libraries(xbox_video PUBLIC d3d11 dxgi dxguid xbox_kernel)
endif()

# Vulkan back-end plumbing: a smoke test that clears and draws on a real device and on
# the software ICD, using shaders compiled ahead of time (see README.md).
if(XBR_VULKAN_FOUND AND BUILD_TESTING)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/vk_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/vk_smoke)
endif()
