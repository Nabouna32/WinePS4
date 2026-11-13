# Candidate whole-archive closure from DolphinPS4's public ps4_link_mesa()
# helper (Mesa 26.0.8 family). Keep this list paired with that Mesa build.
# It is not yet asserted to match WinePS4's private ps4-kit revision 56b3d86.

set(PS4_MESA_ARCHIVE_RELATIVE_PATHS
    src/amd/vulkan/libvulkan_radeon.a
    src/amd/common/libamd_common.a
    src/amd/compiler/libaco.a
    src/amd/addrlib/libaddrlib.a
    src/vulkan/util/libvulkan_util.a
    src/compiler/spirv/libvtn.a
    src/compiler/nir/libnir.a
    src/compiler/libcompiler.a
    src/util/libxmlconfig.a
    src/util/libmesa_util.a
    src/util/libmesa_util_simd.a
    src/util/libmesa_util_clflush.a
    src/util/libmesa_util_clflushopt.a
    src/util/libparson.a
    src/util/blake3/libblake3.a
    src/c11/impl/libmesa_util_c11.a
)

function(ps4_kit_resolve_mesa_archives out_var)
    if (NOT PS4_MESA_BUILD)
        set(PS4_MESA_BUILD "$ENV{PS4_MESA_BUILD}")
    endif ()
    if (NOT PS4_MESA_BUILD)
        message(FATAL_ERROR "PS4_MESA_BUILD is unset; source ps4-kit/toolchain/env.sh first")
    endif ()

    set(_archives)
    set(_missing)
    foreach (_relative IN LISTS PS4_MESA_ARCHIVE_RELATIVE_PATHS)
        set(_archive "${PS4_MESA_BUILD}/${_relative}")
        if (EXISTS "${_archive}")
            list(APPEND _archives "${_archive}")
        else ()
            list(APPEND _missing "${_archive}")
        endif ()
    endforeach ()

    if (_missing)
        string(REPLACE ";" "\n  " _missing_text "${_missing}")
        message(FATAL_ERROR "Missing Mesa archives under ${PS4_MESA_BUILD}:\n  ${_missing_text}")
    endif ()

    set(${out_var} "${_archives}" PARENT_SCOPE)
endfunction()
