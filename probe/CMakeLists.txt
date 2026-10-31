# Wine probe: what a native Wine needs from the PS4, tested on the console (see main.cpp).
#   probe/build.sh
cmake_minimum_required(VERSION 3.20)
project(wine_probe C CXX ASM)
include($ENV{HOME}/ps4-kit/ps4-kit.cmake)

set(PROBE_VERSION "00.00" CACHE STRING "Version shown in the log")

add_executable(wine_probe main.cpp probe_asm.S unused_reals.c)
target_compile_features(wine_probe PRIVATE cxx_std_20)
target_compile_definitions(wine_probe PRIVATE PROBE_VERSION="${PROBE_VERSION}")
target_compile_options(wine_probe PRIVATE -O1)
ps4_kit_app(wine_probe)
