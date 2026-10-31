# WinePS4's eboot.bin (main.c): starts /app0/wineps4.bin. Built by app/build.sh.
cmake_minimum_required(VERSION 3.20)
project(wineps4_launcher C CXX ASM)
include($ENV{HOME}/ps4-kit/ps4-kit.cmake)

add_executable(wineps4_launcher main.c ${CMAKE_CURRENT_LIST_DIR}/../../port/unused_reals.c)
target_compile_options(wineps4_launcher PRIVATE -O1)
ps4_kit_app(wineps4_launcher)
