# Optional Vulkan support: loader/headers, the ahead-of-time shader rule, and the
# software ICD the CTest cases use. Everything here is skipped, with a status
# message, when Vulkan or its shader tools are missing.
#
#   XBR_VULKAN_FOUND   THE capability decision, as ONE value everybody reads: a
#                      CACHE BOOL, FORCE-refreshed on every configure, TRUE only
#                      when the loader, its headers, dxc and spirv-val were all
#                      found AND all of those paths EXIST.  A cache entry is the
#                      channel because a subdirectory's plain variable does not
#                      reach the parent, while a cache entry reaches the parent,
#                      the game's CMakeLists.txt, a tooling probe that includes
#                      this file and `<build>/CMakeCache.txt` -- and FORCE means
#                      no value from an earlier configure can survive.
#   XBR_VULKAN_UNAVAILABLE_REASON
#                      cache STRING: WHY XBR_VULKAN_FOUND is FALSE, naming the
#                      check that failed; empty when it is TRUE.  So a caller can
#                      report the reason instead of re-deriving it (and instead
#                      of guessing at one).
#   XBR_DXC            dxc executable (Vulkan SDK)
#   XBR_SPIRV_VAL      spirv-val executable (Vulkan SDK)
#   XBR_VK_SW_ICD      cache path of a software Vulkan ICD json (default empty)
#   XBR_VK_TEST_DRIVER cache path of the ICD json the macOS MoltenVK test variants pin
#                      (default: MoltenVK_icd.json under $VULKAN_SDK or the loader's prefix, when found)
#
#   xbr_add_spirv(<target> SOURCE <file.hlsl> ENTRY <fn> PROFILE <vs_6_0|ps_6_0|...>
#                 [SYMBOL <name>] [INVERT_Y] [TARGET_ENV <vulkan1.1>]
#                 [DEFINES <A> <B=1> ...] [DEPENDS <extra files>])

set(XBR_VK_SW_ICD "$ENV{XBR_VK_SW_ICD}" CACHE FILEPATH
    "Software Vulkan ICD manifest (e.g. lavapipe lvp_icd.x86_64.json); empty skips the software-ICD tests")

set(XBR_VK_TEST_DRIVER "$ENV{XBR_VK_TEST_DRIVER}" CACHE FILEPATH
    "ICD manifest the macOS MoltenVK test variants pin (default: the Vulkan SDK's MoltenVK_icd.json)")

# ONE decision, recomputed from scratch: every input is found (or re-found) here,
# and both entries below are FORCE-refreshed, so this configure's answer is this
# configure's -- never an earlier one's.  The find_package call is deliberately
# UNCONDITIONAL: a cached XBR_VULKAN_FOUND, a cached path or a stale cache must not
# be able to skip the discovery that decides the mode.
find_package(Vulkan QUIET)
set(_xbr_vk_include "${Vulkan_INCLUDE_DIR}")
if(NOT _xbr_vk_include AND Vulkan_INCLUDE_DIRS)
    list(GET Vulkan_INCLUDE_DIRS 0 _xbr_vk_include)
endif()
if(Vulkan_FOUND)
    find_program(XBR_DXC dxc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    find_program(XBR_SPIRV_VAL spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
endif()

# EVERY check requires the path to EXIST, not merely to be set: a cached toolchain
# is not a toolchain, and `if(XBR_DXC AND XBR_SPIRV_VAL)` used to accept a cached
# path that no longer existed.  The first failing check is named, so the status
# line says what discovery actually found rather than only that something failed.
set(_xbr_vk_reason "")
if(NOT Vulkan_FOUND)
    set(_xbr_vk_reason "the Vulkan loader and headers were not found (find_package(Vulkan) failed; install the Vulkan SDK, or point CMAKE_PREFIX_PATH/VULKAN_SDK at it)")
elseif(NOT Vulkan_LIBRARY OR NOT EXISTS "${Vulkan_LIBRARY}")
    set(_xbr_vk_reason "the Vulkan loader library was not found (Vulkan_LIBRARY='${Vulkan_LIBRARY}')")
elseif(NOT _xbr_vk_include OR NOT EXISTS "${_xbr_vk_include}/vulkan/vulkan.h")
    set(_xbr_vk_reason "the Vulkan headers were not found (Vulkan_INCLUDE_DIR='${_xbr_vk_include}')")
elseif(NOT XBR_DXC)
    set(_xbr_vk_reason "the shader compiler dxc was not found (set VULKAN_SDK, or point XBR_DXC at dxc)")
elseif(NOT EXISTS "${XBR_DXC}")
    set(_xbr_vk_reason "the shader compiler dxc='${XBR_DXC}' does not exist (set VULKAN_SDK, or point XBR_DXC at an existing dxc)")
elseif(NOT XBR_SPIRV_VAL)
    set(_xbr_vk_reason "the SPIR-V validator spirv-val was not found (set VULKAN_SDK, or point XBR_SPIRV_VAL at spirv-val)")
elseif(NOT EXISTS "${XBR_SPIRV_VAL}")
    set(_xbr_vk_reason "the SPIR-V validator spirv-val='${XBR_SPIRV_VAL}' does not exist (set VULKAN_SDK, or point XBR_SPIRV_VAL at an existing spirv-val)")
endif()

if(_xbr_vk_reason)
    set(XBR_VULKAN_FOUND FALSE CACHE BOOL
        "ONE bound Vulkan capability decision (loader, headers, dxc and spirv-val all found and present); recomputed every configure" FORCE)
    set(XBR_VULKAN_UNAVAILABLE_REASON "${_xbr_vk_reason}" CACHE STRING
        "Why XBR_VULKAN_FOUND is FALSE (empty when Vulkan and its shader tools are available)" FORCE)
    message(STATUS "Vulkan: unavailable -- ${_xbr_vk_reason}; Vulkan pieces skipped")
else()
    set(XBR_VULKAN_FOUND TRUE CACHE BOOL
        "ONE bound Vulkan capability decision (loader, headers, dxc and spirv-val all found and present); recomputed every configure" FORCE)
    set(XBR_VULKAN_UNAVAILABLE_REASON "" CACHE STRING
        "Why XBR_VULKAN_FOUND is FALSE (empty when Vulkan and its shader tools are available)" FORCE)
    message(STATUS "Vulkan: enabled (loader ${Vulkan_LIBRARY}; headers ${_xbr_vk_include}; dxc ${XBR_DXC}; spirv-val ${XBR_SPIRV_VAL})")
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
