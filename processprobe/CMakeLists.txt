# WinePS4 process probe (see main.cpp): eboot.bin, or child.bin with PROBE_CHILD.
#   processprobe/build.sh
cmake_minimum_required(VERSION 3.20)
project(wine_processprobe C CXX ASM)
include($ENV{HOME}/ps4-kit/ps4-kit.cmake)

option(PROBE_CHILD "child.bin (started by execve)" OFF)

add_executable(processprobe main.cpp ${CMAKE_CURRENT_LIST_DIR}/../probe/unused_reals.c)
target_compile_features(processprobe PRIVATE cxx_std_20)
if (PROBE_CHILD)
    target_compile_definitions(processprobe PRIVATE PROBE_CHILD PROBE_ROLE="child")
endif ()
target_compile_options(processprobe PRIVATE -O1)
ps4_kit_app(processprobe)
