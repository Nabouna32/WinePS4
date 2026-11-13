# Clean-room subset of the private ps4-kit.cmake interface used by WinePS4.
#
# This file intentionally implements only the OpenOrbis linker-script and eboot
# packaging boundary that can be reconstructed from public build scripts.
# Mesa/RADV + the private VideoOut WSI are NOT implemented here.
#
# Intended to be included from the WinePS4 app after project() has selected
# the OpenOrbis toolchain. The OpenOrbis environment must already be configured.

include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/ps4-linker.cmake)

# The runtime subset is compiled from this clean-room kit tree and linked only
# through the existing ps4_kit_app() entry point.
get_filename_component(_PS4_KIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../ps4-kit" ABSOLUTE)
set(_PS4_KIT_RUNTIME "${_PS4_KIT_ROOT}/runtime/ps4_kit_runtime.c")
if (NOT TARGET ps4_kit_runtime)
    add_library(ps4_kit_runtime STATIC "${_PS4_KIT_RUNTIME}")
    target_include_directories(ps4_kit_runtime PUBLIC "${_PS4_KIT_ROOT}/include")
endif ()

set(PS4_PAID "${PS4_PAID}" CACHE STRING "PS4 program authority ID")
if (PS4_PAID STREQUAL "")
    set(PS4_PAID "0x3100000000000002" CACHE STRING "PS4 program authority ID" FORCE)
endif ()
set(PS4_AUTH_INFO "${PS4_AUTH_INFO}" CACHE STRING "PS4 auth info passed to create-fself")
if (PS4_AUTH_INFO STREQUAL "")
    set(PS4_AUTH_INFO
        "000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        CACHE STRING "PS4 auth info passed to create-fself" FORCE)
endif ()

if (NOT DEFINED OPENORBIS OR OPENORBIS STREQUAL "")
    message(FATAL_ERROR "OPENORBIS must point to a PacBrew/OpenOrbis installation")
endif ()
set(_ps4_create_fself "${OPENORBIS}/bin/create-fself")
if (NOT EXISTS "${_ps4_create_fself}")
    message(FATAL_ERROR "OpenOrbis create-fself not found: ${_ps4_create_fself}")
endif ()

# WinePS4's package script expects <target>_eboot/<target>.oelf and eboot.bin.
function(ps4_kit_app target)
    if (NOT TARGET ${target})
        message(FATAL_ERROR "ps4_kit_app: target '${target}' does not exist")
    endif ()
    target_link_libraries(${target} PRIVATE ps4_kit_runtime kernel c SceLibcInternal)
    target_link_options(${target} PRIVATE "LINKER:--wrap=pthread_create")
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_eboot")
    set(oelf "${out_dir}/${target}.oelf")
    set(eboot "${out_dir}/eboot.bin")
    add_custom_command(
        OUTPUT "${eboot}" "${oelf}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out_dir}"
        COMMAND "${CMAKE_COMMAND}" -E env "OO_PS4_TOOLCHAIN=${OPENORBIS}"
            "${_ps4_create_fself}" "-in=$<TARGET_FILE:${target}>"
            "-out=${oelf}" --eboot "${eboot}"
            --paid "${PS4_PAID}" --authinfo "${PS4_AUTH_INFO}"
        COMMAND "${CMAKE_COMMAND}" "-DOELF=${oelf}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check-oelf.cmake"
        DEPENDS ${target}
        VERBATIM
        COMMENT "Creating PS4 eboot for ${target}")
    add_custom_target(${target}_eboot ALL DEPENDS "${eboot}" "${oelf}")
endfunction ()

# Candidate public Mesa 26.0.8 whole-archive closure plus a separate Wine
# window-mode WSI source. The archive manifest is validated before any link
# flags are added; do not silently fall back to a full-screen VideoOut WSI.
set(PS4_MESA_BUILD "$ENV{PS4_MESA_BUILD}" CACHE PATH "Mesa PS4 build directory")
set(PS4_MESA_SOURCE "$ENV{PS4_MESA_SOURCE}" CACHE PATH "Mesa PS4 source directory")
set(PS4_KIT_MESA_WSI_SOURCE
    "${_PS4_KIT_ROOT}/mesa/ps4_vk_wsi_window.cpp"
    CACHE FILEPATH "Wine window-mode Vulkan WSI implementation")
set(PS4_VULKAN_HEADERS "$ENV{PS4_SYSROOT}/include" CACHE PATH
    "Directory containing the Vulkan-Headers include/vulkan tree")

function(ps4_kit_link_mesa target)
    if (NOT TARGET ${target})
        message(FATAL_ERROR "ps4_kit_link_mesa: target '${target}' does not exist")
    endif ()

    include("${_PS4_KIT_ROOT}/mesa/archives.cmake")
    ps4_kit_resolve_mesa_archives(_ps4_mesa_archives)

    if (NOT PS4_MESA_SOURCE OR NOT IS_DIRECTORY "${PS4_MESA_SOURCE}")
        message(FATAL_ERROR "PS4_MESA_SOURCE must point to the matching Mesa source tree")
    endif ()
    if (NOT PS4_VULKAN_HEADERS OR NOT EXISTS "${PS4_VULKAN_HEADERS}/vulkan/vulkan.h")
        message(FATAL_ERROR
            "PS4_VULKAN_HEADERS must point to a Vulkan-Headers include directory "
            "(expected <path>/vulkan/vulkan.h); PS4_SYSROOT should provide it")
    endif ()
    if (NOT EXISTS "${PS4_KIT_MESA_WSI_SOURCE}")
        message(FATAL_ERROR
            "Wine window-mode Vulkan WSI is not reconstructed yet: "
            "${PS4_KIT_MESA_WSI_SOURCE}. A full-screen VideoOut WSI is not compatible.")
    endif ()

    target_sources(${target} PRIVATE "${PS4_KIT_MESA_WSI_SOURCE}")
    target_include_directories(${target} PRIVATE
        "${PS4_VULKAN_HEADERS}"
        "${PS4_MESA_SOURCE}/include"
        "${PS4_MESA_SOURCE}/src"
        "${_PS4_KIT_ROOT}/include")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -march=btver2)
    target_link_libraries(${target} PRIVATE --whole-archive ${_ps4_mesa_archives} --no-whole-archive)
endfunction ()
