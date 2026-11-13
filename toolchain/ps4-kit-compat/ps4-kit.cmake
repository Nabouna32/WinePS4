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

# The private kit's Mesa function also integrates its own VideoOut window-system
# bridge. Linking public RADV archives alone is not equivalent. Fail explicitly
# until WinePS4's exact WSI ABI and archive closure have been verified.
function(ps4_kit_link_mesa target)
    message(FATAL_ERROR
        "ps4_kit_link_mesa is not implemented: public RADV archives are not a "
        "drop-in replacement for ps4-kit's private VideoOut WSI. Build/validate "
        "the independent Mesa + WSI layer before enabling this call.")
endfunction ()
