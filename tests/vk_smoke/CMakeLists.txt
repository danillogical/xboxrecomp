# vk_smoke - create an instance and device, draw a triangle with ahead-of-time
# compiled SPIR-V into a small image, read it back and check the pixels.
#   vk_smoke_sw  forced onto the software ICD (XBR_VK_SW_ICD); skipped when unset
#   vk_smoke_hw  the loader's default devices; skipped when only a CPU device exists

add_executable(vk_smoke vk_smoke.c)
target_link_libraries(vk_smoke PRIVATE Vulkan::Vulkan)
xbr_add_spirv(vk_smoke SOURCE ${CMAKE_CURRENT_LIST_DIR}/../../src/video/shaders/smoke_tri_vs.hlsl
    ENTRY main PROFILE vs_6_0 INVERT_Y SYMBOL xbr_smoke_tri_vs)
xbr_add_spirv(vk_smoke SOURCE ${CMAKE_CURRENT_LIST_DIR}/../../src/video/shaders/smoke_tri_ps.hlsl
    ENTRY main PROFILE ps_6_0 SYMBOL xbr_smoke_tri_ps)
if(MSVC)
    target_compile_options(vk_smoke PRIVATE /W3)
    target_compile_definitions(vk_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(XBR_VK_SW_ICD)
    file(TO_NATIVE_PATH "${XBR_VK_SW_ICD}" _icd)
    add_test(NAME vk_smoke_sw COMMAND vk_smoke --require-cpu)
    set_tests_properties(vk_smoke_sw PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
        ENVIRONMENT "VK_DRIVER_FILES=${_icd};VK_ICD_FILENAMES=${_icd}")
else()
    add_test(NAME vk_smoke_sw COMMAND vk_smoke --skip "XBR_VK_SW_ICD is not set")
    set_tests_properties(vk_smoke_sw PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77)
endif()

add_test(NAME vk_smoke_hw COMMAND vk_smoke --forbid-cpu)
set_tests_properties(vk_smoke_hw PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# A caller's own ICD override must not leak into the hardware variant.
if(NOT CMAKE_VERSION VERSION_LESS 3.22)
    set_tests_properties(vk_smoke_hw PROPERTIES
        ENVIRONMENT_MODIFICATION "VK_DRIVER_FILES=unset:;VK_ICD_FILENAMES=unset:")
endif()
