# Compatibility entry point expected by the direct consumers' CMake projects.
# Keep the project-side include paths and build scripts unchanged.
set(PS4_KIT_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(PS4_KIT_ROOT "${CMAKE_CURRENT_LIST_DIR}")
include_directories(BEFORE "${PS4_KIT_DIR}/include")
include("${PS4_KIT_DIR}/../toolchain/ps4-kit-compat/ps4-kit.cmake")
