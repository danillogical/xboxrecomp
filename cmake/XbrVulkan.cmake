# Optional Vulkan support: loader/headers, the ahead-of-time shader rule, and the
# software ICD the CTest cases use. Everything here is skipped, with a status
# message, when Vulkan or its shader tools are missing.
#
#   XBR_VULKAN_FOUND   TRUE when headers, loader, dxc and spirv-val were all found
#   XBR_DXC            dxc executable (Vulkan SDK)
#   XBR_SPIRV_VAL      spirv-val executable (Vulkan SDK)
#   XBR_VK_SW_ICD      cache path of a software Vulkan ICD json (default empty)
#
#   xbr_add_spirv(<target> SOURCE <file.hlsl> ENTRY <fn> PROFILE <vs_6_0|ps_6_0|...>
#                 [SYMBOL <name>] [INVERT_Y] [TARGET_ENV <vulkan1.1>]
#                 [DEFINES <A> <B=1> ...] [DEPENDS <extra files>])

set(XBR_VK_SW_ICD "$ENV{XBR_VK_SW_ICD}" CACHE FILEPATH
    "Software Vulkan ICD manifest (e.g. lavapipe lvp_icd.x86_64.json); empty skips the software-ICD tests")

set(XBR_VULKAN_FOUND FALSE)
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    find_program(XBR_DXC dxc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    find_program(XBR_SPIRV_VAL spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    if(XBR_DXC AND XBR_SPIRV_VAL)
        set(XBR_VULKAN_FOUND TRUE)
        message(STATUS "Vulkan: enabled (${Vulkan_LIBRARY}; dxc ${XBR_DXC})")
    else()
        message(STATUS "Vulkan: found, but dxc or spirv-val is missing (set VULKAN_SDK); Vulkan pieces skipped")
    endif()
else()
    message(STATUS "Vulkan: not found (install the Vulkan SDK); Vulkan pieces skipped")
endif()

if(XBR_VULKAN_FOUND)
    if(XBR_VK_SW_ICD AND NOT EXISTS "${XBR_VK_SW_ICD}")
        message(WARNING "XBR_VK_SW_ICD does not exist: ${XBR_VK_SW_ICD}; software-ICD tests will be skipped")
        set(XBR_VK_SW_ICD "")
    elseif(XBR_VK_SW_ICD)
        message(STATUS "Vulkan: software ICD ${XBR_VK_SW_ICD}")
    else()
        message(STATUS "Vulkan: XBR_VK_SW_ICD not set; software-ICD tests will be skipped")
    endif()
endif()

# Compile an HLSL entry point to SPIR-V at build time, validate it, and emit
#   <build>/spirv/<SYMBOL>.spv.h:  static const uint32_t <SYMBOL>[]; static const size_t <SYMBOL>_size;
# (size in bytes). The header is added to <target>'s sources, so a compile or
# validation failure fails that target's build.
function(xbr_add_spirv target)
    cmake_parse_arguments(A "INVERT_Y" "SOURCE;ENTRY;PROFILE;SYMBOL;TARGET_ENV" "DEFINES;DEPENDS" ${ARGN})
    if(NOT A_SOURCE OR NOT A_ENTRY OR NOT A_PROFILE)
        message(FATAL_ERROR "xbr_add_spirv: SOURCE, ENTRY and PROFILE are required")
    endif()
    if(NOT XBR_VULKAN_FOUND)
        message(FATAL_ERROR "xbr_add_spirv: Vulkan support is not available")
    endif()
    if(NOT A_TARGET_ENV)
        set(A_TARGET_ENV vulkan1.1)
    endif()
    if(NOT A_SYMBOL)
        get_filename_component(A_SYMBOL "${A_SOURCE}" NAME_WE)
    endif()
    if(A_INVERT_Y AND NOT A_PROFILE MATCHES "^(vs|ds|gs)_")
        message(FATAL_ERROR "xbr_add_spirv: INVERT_Y only applies to vs/ds/gs profiles")
    endif()

    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/spirv")
    set(_spv "${_dir}/${A_SYMBOL}.spv")
    set(_hdr "${_dir}/${A_SYMBOL}.spv.h")
    get_filename_component(_src "${A_SOURCE}" ABSOLUTE)

    set(_flags -spirv -T ${A_PROFILE} -E ${A_ENTRY} -fspv-target-env=${A_TARGET_ENV})
    if(A_INVERT_Y)
        list(APPEND _flags -fvk-invert-y)
    endif()
    foreach(_d IN LISTS A_DEFINES)
        list(APPEND _flags -D ${_d})
    endforeach()

    add_custom_command(
        OUTPUT "${_hdr}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
        COMMAND "${XBR_DXC}" ${_flags} -Fo "${_spv}" "${_src}"
        COMMAND "${XBR_SPIRV_VAL}" --target-env ${A_TARGET_ENV} "${_spv}"
        COMMAND ${CMAKE_COMMAND} -DSPV=${_spv} -DOUT=${_hdr} -DSYMBOL=${A_SYMBOL}
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/xbr_spirv_to_header.cmake"
        DEPENDS "${_src}" ${A_DEPENDS} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/xbr_spirv_to_header.cmake"
        COMMENT "SPIR-V ${A_PROFILE} ${A_ENTRY}: ${A_SYMBOL}"
        VERBATIM)
    target_sources(${target} PRIVATE "${_hdr}")
    target_include_directories(${target} PRIVATE "${_dir}")
endfunction()
