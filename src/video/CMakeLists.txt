# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
    nv2a_d3d11_backend.c
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
