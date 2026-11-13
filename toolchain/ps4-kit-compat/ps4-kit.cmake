# Clean-room subset of the private ps4-kit.cmake interface.
#
# This file implements the reconstructed OpenOrbis linker-script/eboot/package
# boundary and links the separate Mesa/RADV archive closure. WSI selection is
# deliberately separate: Wine owns a desktop callback, while native PS4 apps
# own VideoOut. Never attach Wine's window WSI to an emulator/recompiler app.
#
# Intended to be included after project() has selected the OpenOrbis toolchain.

include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/ps4-linker.cmake)

get_filename_component(_PS4_KIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../ps4-kit" ABSOLUTE)
set(_PS4_KIT_RUNTIME
    "${_PS4_KIT_ROOT}/runtime/ps4_kit_runtime.c"
    "${_PS4_KIT_ROOT}/runtime/ps4_kit_heap_policy.c")
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

# Select presentation only when an application actually links the Mesa ICD.
# WinePS4's top-level CMake project is wineps4; all other consumers default to
# the native VideoOut profile. Consumers can override this explicitly.
set(PS4_KIT_WSI_PROFILE "" CACHE STRING "Presentation profile: window-desktop or native-videoout")
set_property(CACHE PS4_KIT_WSI_PROFILE PROPERTY STRINGS window-desktop native-videoout)
set(PS4_KIT_MESA_BUILD "$ENV{PS4_MESA_BUILD}" CACHE PATH "Mesa PS4 build directory")
set(PS4_MESA_SOURCE "$ENV{PS4_MESA_SOURCE}" CACHE PATH "Mesa PS4 source directory")
set(PS4_KIT_WINDOW_WSI_SOURCE
    "${_PS4_KIT_ROOT}/mesa/ps4_vk_wsi_window.cpp"
    CACHE FILEPATH "Wine window-mode Vulkan WSI implementation")
set(PS4_KIT_NATIVE_WSI_SOURCE
    "${_PS4_KIT_ROOT}/mesa/ps4_vk_wsi_videoout.cpp"
    CACHE FILEPATH "Native VideoOut Vulkan WSI implementation")
set(PS4_VULKAN_HEADERS "$ENV{PS4_SYSROOT}/include" CACHE PATH
    "Directory containing the Vulkan-Headers include/vulkan tree")

function(_ps4_kit_resolve_wsi_profile out_var)
    if (PS4_KIT_WSI_PROFILE)
        set(_profile "${PS4_KIT_WSI_PROFILE}")
    elseif (CMAKE_PROJECT_NAME STREQUAL "wineps4")
        set(_profile "window-desktop")
    else ()
        set(_profile "native-videoout")
    endif ()
    if (NOT _profile STREQUAL "window-desktop" AND
        NOT _profile STREQUAL "native-videoout")
        message(FATAL_ERROR
            "Unsupported PS4_KIT_WSI_PROFILE='${_profile}'; use window-desktop or native-videoout")
    endif ()
    set(${out_var} "${_profile}" PARENT_SCOPE)
endfunction ()

function(ps4_kit_link_window_wsi target)
    if (NOT TARGET ${target})
        message(FATAL_ERROR "ps4_kit_link_window_wsi: target '${target}' does not exist")
    endif ()
    if (NOT EXISTS "${PS4_KIT_WINDOW_WSI_SOURCE}")
        message(FATAL_ERROR "Wine window WSI source not found: ${PS4_KIT_WINDOW_WSI_SOURCE}")
    endif ()
    target_sources(${target} PRIVATE "${PS4_KIT_WINDOW_WSI_SOURCE}")
    target_include_directories(${target} PRIVATE
        "${PS4_VULKAN_HEADERS}"
        "${PS4_MESA_SOURCE}/include"
        "${PS4_MESA_SOURCE}/src"
        "${_PS4_KIT_ROOT}/include")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -march=btver2)
endfunction ()

# Attach the selected WSI once both the application helper and Mesa link helper
# have run. Wine's CMake calls ps4_kit_app() before ps4_kit_link_mesa(); MGR,
# MP3, ConkerLR and Godot call them in the opposite order.
function(_ps4_kit_attach_wsi target)
    get_target_property(_already_attached ${target} PS4_KIT_WSI_ATTACHED)
    if (_already_attached)
        return()
    endif ()
    get_target_property(_app_configured ${target} PS4_KIT_APP_CONFIGURED)
    get_target_property(_mesa_linked ${target} PS4_KIT_MESA_LINKED)
    if (NOT _app_configured OR NOT _mesa_linked)
        return()
    endif ()

    _ps4_kit_resolve_wsi_profile(_ps4_wsi_profile)
    if (_ps4_wsi_profile STREQUAL "window-desktop")
        ps4_kit_link_window_wsi(${target})
    else ()
        if (NOT EXISTS "${PS4_KIT_NATIVE_WSI_SOURCE}")
            message(FATAL_ERROR
                "Native VideoOut WSI is not reconstructed yet: ${PS4_KIT_NATIVE_WSI_SOURCE}. "
                "Refusing to link Wine's window-desktop WSI into '${target}'.")
        endif ()
        target_sources(${target} PRIVATE "${PS4_KIT_NATIVE_WSI_SOURCE}")
        target_include_directories(${target} PRIVATE
            "${PS4_VULKAN_HEADERS}"
            "${PS4_MESA_SOURCE}/include"
            "${PS4_MESA_SOURCE}/src"
            "${_PS4_KIT_ROOT}/include")
        target_compile_features(${target} PRIVATE cxx_std_20)
        target_compile_options(${target} PRIVATE -march=btver2)
        target_link_libraries(${target} PRIVATE SceVideoOut SceSysmodule)
    endif ()
    set_property(TARGET ${target} PROPERTY PS4_KIT_WSI_ATTACHED TRUE)
endfunction ()

# Generic RADV link closure only. WSI selection is deferred until both kit
# helpers have run, regardless of which order the consumer's CMake uses.
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

    target_include_directories(${target} PRIVATE
        "${PS4_VULKAN_HEADERS}"
        "${PS4_MESA_SOURCE}/include"
        "${PS4_MESA_SOURCE}/src"
        "${_PS4_KIT_ROOT}/include")
    target_compile_options(${target} PRIVATE -march=btver2)
    target_link_libraries(${target} PRIVATE --whole-archive ${_ps4_mesa_archives} --no-whole-archive)
    set_property(TARGET ${target} PROPERTY PS4_KIT_MESA_LINKED TRUE)
    _ps4_kit_attach_wsi(${target})
endfunction ()

# WinePS4's package script expects <target>_eboot/<target>.oelf and eboot.bin.
function(ps4_kit_app target)
    if (NOT TARGET ${target})
        message(FATAL_ERROR "ps4_kit_app: target '${target}' does not exist")
    endif ()

    target_link_libraries(${target} PRIVATE ps4_kit_runtime kernel c SceLibcInternal)
    target_link_options(${target} PRIVATE "LINKER:--wrap=pthread_create")
    set_property(TARGET ${target} PROPERTY PS4_KIT_APP_CONFIGURED TRUE)
    _ps4_kit_attach_wsi(${target})

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

