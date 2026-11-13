# Clean-room OpenOrbis linker-script preparation for WinePS4.
# Include after the OpenOrbis toolchain file and before configuring/linking app targets.
#
# Usage:
#   include(cmake/ps4-kit-compat/ps4-linker.cmake)
#   # PS4_LINK_SCRIPT is now a generated, build-local copy.

if (NOT DEFINED OPENORBIS OR OPENORBIS STREQUAL "")
    if (DEFINED ENV{OPENORBIS})
        set(OPENORBIS "$ENV{OPENORBIS}")
    else ()
        set(OPENORBIS "/opt/pacbrew/ps4/openorbis")
    endif ()
endif ()

set(PS4_LINK_SCRIPT_IN "${OPENORBIS}/link.x" CACHE FILEPATH "OpenOrbis base linker script")
if (NOT EXISTS "${PS4_LINK_SCRIPT_IN}")
    message(FATAL_ERROR "OpenOrbis linker script not found: ${PS4_LINK_SCRIPT_IN}")
endif ()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(PS4_LINK_SCRIPT "${CMAKE_BINARY_DIR}/ps4-link.x" CACHE FILEPATH
    "Generated WinePS4-compatible linker script" FORCE)
set(_PS4_LINKER_GENERATOR "${CMAKE_CURRENT_LIST_DIR}/generate-link-script.py")

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_PS4_LINKER_GENERATOR}" "${PS4_LINK_SCRIPT_IN}" "${PS4_LINK_SCRIPT}"
    RESULT_VARIABLE _ps4_linker_result
    OUTPUT_VARIABLE _ps4_linker_stdout
    ERROR_VARIABLE _ps4_linker_stderr
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if (NOT _ps4_linker_result EQUAL 0)
    message(FATAL_ERROR
        "Could not prepare OpenOrbis linker script. The installed link.x may have an unsupported layout.\n"
        "${_ps4_linker_stderr}")
endif ()

# OpenOrbis' CMake toolchain places its stock link.x in the executable link command.
# Redirect that argument to the generated copy; do not merely publish a variable.
string(REPLACE "${PS4_LINK_SCRIPT_IN}" "${PS4_LINK_SCRIPT}"
    CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
if (NOT CMAKE_EXE_LINKER_FLAGS MATCHES "ps4-link\\.x")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " --script ${PS4_LINK_SCRIPT}")
endif ()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PS4_LINK_SCRIPT_IN}" "${_PS4_LINKER_GENERATOR}")
message(STATUS "WinePS4 linker script: ${PS4_LINK_SCRIPT}")
