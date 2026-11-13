# Compatibility entry point expected by the existing WinePS4 CMake files.
# Keep the project-side include paths and build scripts unchanged.
include_directories(BEFORE "${CMAKE_CURRENT_LIST_DIR}/include")
include("${CMAKE_CURRENT_LIST_DIR}/../toolchain/ps4-kit-compat/ps4-kit.cmake")
