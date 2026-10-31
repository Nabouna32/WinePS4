# WinePS4 base probe (see main.cpp): one build per eboot.
#   baseprobe/build.sh
# PROBE_BASE empty: the launcher (eboot.bin, linked as every app is). Else a high eboot: the kit's
# linker script starts its image at PROBE_BASE, and build.sh patches it to ET_SCE_EXEC.
cmake_minimum_required(VERSION 3.20)
project(wine_baseprobe C CXX ASM)
include($ENV{HOME}/ps4-kit/ps4-kit.cmake)

set(PROBE_BASE "" CACHE STRING "Where the image starts (empty: the launcher)")
set(PROBE_ROLE "launcher" CACHE STRING "Its name in the log")

if (PROBE_BASE)
    # the kit's script (ps4-link.x, written by ps4-kit.cmake), its sections from PROBE_BASE on
    file(READ ${PS4_LINK_SCRIPT} HIGH_SCRIPT)
    string(REPLACE "SECTIONS\n{" "SECTIONS\n{\n\t. = ${PROBE_BASE};" HIGH_SCRIPT "${HIGH_SCRIPT}")
    string(FIND "${HIGH_SCRIPT}" ". = ${PROBE_BASE};" HIGH_POS)
    if (HIGH_POS EQUAL -1)
        message(FATAL_ERROR "the linker script's SECTIONS { was not found")
    endif ()
    file(WRITE ${PS4_LINK_SCRIPT} "${HIGH_SCRIPT}")
endif ()

add_executable(baseprobe main.cpp probe_asm.S unused_reals.c)
target_compile_features(baseprobe PRIVATE cxx_std_20)
target_compile_definitions(baseprobe PRIVATE PROBE_ROLE="${PROBE_ROLE}")
if (PROBE_BASE)
    target_compile_definitions(baseprobe PRIVATE PROBE_BASE=${PROBE_BASE})
endif ()
target_compile_options(baseprobe PRIVATE -O1)
ps4_kit_app(baseprobe)
