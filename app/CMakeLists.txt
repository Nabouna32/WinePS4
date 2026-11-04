# WinePS4: Wine's Unix side (ntdll.so + wineserver) built into one PS4 eboot on ps4-kit.
#   app/build.sh
# The Windows side (ntdll.dll, kernel32.dll, ...) is ordinary PE code from the host build
# (scripts/build-host-wine.sh) and is packaged next to the eboot.
cmake_minimum_required(VERSION 3.20)
project(wineps4 C CXX ASM)
include($ENV{HOME}/ps4-kit/ps4-kit.cmake)

set(WINEPS4_VERSION "00.00" CACHE STRING "Version shown in the log")

# Where the app's image starts: off 0x400000, which 32-bit programs without relocations need. The
# kit's linker script starts there, and package.sh makes the eboot ET_SCE_EXEC (mapped where it was
# linked; the eboot.bin launcher, app/launcher, starts it). Below 2 GiB: the kit's weak hooks
# resolve to 0, which code higher up can't reach (R_X86_64_PC32).
set(WINEPS4_IMAGE_BASE 0x60000000)
file(READ ${PS4_LINK_SCRIPT} HIGH_SCRIPT)
string(FIND "${HIGH_SCRIPT}" ". = ${WINEPS4_IMAGE_BASE};" HIGH_POS)
if (HIGH_POS EQUAL -1)
    string(REPLACE "SECTIONS\n{" "SECTIONS\n{\n\t. = ${WINEPS4_IMAGE_BASE};" HIGH_SCRIPT "${HIGH_SCRIPT}")
    string(FIND "${HIGH_SCRIPT}" ". = ${WINEPS4_IMAGE_BASE};" HIGH_POS)
    if (HIGH_POS EQUAL -1)
        message(FATAL_ERROR "the linker script's SECTIONS { was not found")
    endif ()
    file(WRITE ${PS4_LINK_SCRIPT} "${HIGH_SCRIPT}")
endif ()
set(WINE_SRC $ENV{HOME}/wine-ps4-build/src/wine)
set(PORT ${CMAKE_CURRENT_LIST_DIR}/../port)

# Where the packaged files are on the console (see port/ps4_main.c).
set(WINE_APP_DIR /app0/wine)

set(WINE_UNIX_DEFS __WINESRC__ WINE_UNIX_LIB _NTSYSTEM_ _ACRTIMP= WINBASEAPI= __PS4_WINE__)
# FreeBSD code paths want FreeBSD headers OpenOrbis lacks: Wine sees a generic Unix, and the few
# FreeBSD pieces the PS4 does have are enabled by __PS4_WINE__ in the Wine patches.
set(WINE_UNIX_OPTS -U__FreeBSD__ -U__FreeBSD_kernel__ -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing -fasynchronous-unwind-tables
    -include ${PORT}/include/ps4_wine_compat.h
    -Wno-unused-command-line-argument -Wno-incompatible-pointer-types -Wno-int-conversion
    -Wno-unknown-pragmas -Wno-pointer-sign -Wno-ignored-attributes -Wno-unused-function)

# --- ntdll's Unix half ---------------------------------------------------------------------------
set(NTDLL_UNIX cdrom debug env file loader loadorder process registry security serial server
    signal_x86_64 socket sync system tape thread virtual)
list(TRANSFORM NTDLL_UNIX PREPEND ${WINE_SRC}/dlls/ntdll/unix/)
list(TRANSFORM NTDLL_UNIX APPEND .c)
add_library(wine_ntdll_unix OBJECT ${NTDLL_UNIX} ${PORT}/wine_version.c)
target_include_directories(wine_ntdll_unix BEFORE PRIVATE ${PORT}/include ${WINE_SRC}/dlls/ntdll ${WINE_SRC}/include)
target_compile_definitions(wine_ntdll_unix PRIVATE ${WINE_UNIX_DEFS} LTC_NO_PROTOTYPES LTC_SOURCE
    BINDIR="${WINE_APP_DIR}/bin" LIBDIR="${WINE_APP_DIR}/lib" DATADIR="${WINE_APP_DIR}/share"
    SYSTEMDLLPATH="")
target_compile_options(wine_ntdll_unix PRIVATE ${WINE_UNIX_OPTS})

# --- wineserver ----------------------------------------------------------------------------------
# Runs as a thread of the same process (no fork on the PS4). Its objects are linked into one and
# every symbol but its entry point made local: wineserver and ntdll are separate programs on other
# systems and share names (fatal_error, debug_level, ...).
set(SERVER async atom change class clipboard completion console d3dkmt debugger device directory event fd
    file handle hook inproc_sync mailslot main mapping mutex named_pipe object process procfs ptrace queue
    region registry request semaphore serial signal sock symlink thread timer token trace unicode user
    window winstation)
list(TRANSFORM SERVER PREPEND ${WINE_SRC}/server/)
list(TRANSFORM SERVER APPEND .c)
add_library(wine_server OBJECT ${SERVER})
target_include_directories(wine_server BEFORE PRIVATE ${PORT}/include ${WINE_SRC}/server ${WINE_SRC}/include)
target_compile_definitions(wine_server PRIVATE ${WINE_UNIX_DEFS}
    BINDIR="${WINE_APP_DIR}/bin" DATADIR="${WINE_APP_DIR}/share")
target_compile_options(wine_server PRIVATE ${WINE_UNIX_OPTS})

set(SERVER_ONE ${CMAKE_CURRENT_BINARY_DIR}/wineserver-one.o)
add_custom_command(
    OUTPUT ${SERVER_ONE}
    COMMAND ld.lld-$ENV{CLANG_VER} -r -o ${SERVER_ONE}.tmp $<TARGET_OBJECTS:wine_server>
    COMMAND llvm-objcopy-$ENV{CLANG_VER} --keep-global-symbol=ps4_wineserver_main --keep-global-symbol=ps4_server_input --keep-global-symbol=ps4_ended_pids --keep-global-symbol=ps4_ended_count --keep-global-symbol=ps4_end_done
        ${SERVER_ONE}.tmp ${SERVER_ONE}
    DEPENDS $<TARGET_OBJECTS:wine_server>
    COMMAND_EXPAND_LISTS
    VERBATIM)
add_custom_target(wine_server_one DEPENDS ${SERVER_ONE})

# --- win32u's and the display driver's Unix halves -----------------------------------------------
# Unix libraries (win32u.so, wineps4.so) on other systems: each is linked into one object whose
# hidden symbols are made local (they share names with ntdll's), and whose __wine_unix_call_funcs
# get names of their own, which the built-in table (port/ps4_unixlibs.c) gives ntdll's dlsym.
set(WINEPS4_FREETYPE $ENV{HOME}/wineps4-build/freetype)
# headers made from IDL files by the PC's Wine build (objidlbase.h, ...)
set(WINE_GEN_INCLUDE $ENV{HOME}/wine-ps4-build/build/wine-wow64/include)
file(GLOB WIN32U_UNIX ${WINE_SRC}/dlls/win32u/*.c ${WINE_SRC}/dlls/win32u/dibdrv/*.c)
list(REMOVE_ITEM WIN32U_UNIX ${WINE_SRC}/dlls/win32u/main.c)  # the PE side
add_library(wine_win32u_unix OBJECT ${WIN32U_UNIX})
target_include_directories(wine_win32u_unix BEFORE PRIVATE ${PORT}/include ${WINE_SRC}/dlls/win32u ${WINE_SRC}/include
    ${WINE_GEN_INCLUDE} ${WINEPS4_FREETYPE}/include)
target_compile_definitions(wine_win32u_unix PRIVATE ${WINE_UNIX_DEFS} _WIN32U_)
target_compile_options(wine_win32u_unix PRIVATE ${WINE_UNIX_OPTS})

file(GLOB PS4DRV_UNIX ${WINE_SRC}/dlls/wineps4.drv/*.c)
list(REMOVE_ITEM PS4DRV_UNIX ${WINE_SRC}/dlls/wineps4.drv/dllmain.c)  # the PE side
add_library(wine_ps4drv_unix OBJECT ${PS4DRV_UNIX})
target_include_directories(wine_ps4drv_unix BEFORE PRIVATE ${PORT}/include ${WINE_SRC}/dlls/wineps4.drv ${WINE_SRC}/include ${WINE_GEN_INCLUDE})
target_compile_definitions(wine_ps4drv_unix PRIVATE ${WINE_UNIX_DEFS})
target_compile_options(wine_ps4drv_unix PRIVATE ${WINE_UNIX_OPTS})

function(wine_unixlib_one name target)
    set(out ${CMAKE_CURRENT_BINARY_DIR}/${name}-one.o)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ld.lld-$ENV{CLANG_VER} -r -o ${out}.tmp $<TARGET_OBJECTS:${target}>
        COMMAND llvm-objcopy-$ENV{CLANG_VER} --localize-hidden
            --redefine-sym __wine_unix_call_funcs=${name}_unix_call_funcs
            --redefine-sym __wine_unix_call_wow64_funcs=${name}_unix_call_wow64_funcs ${out}.tmp ${out}
        DEPENDS $<TARGET_OBJECTS:${target}>
        COMMAND_EXPAND_LISTS
        VERBATIM)
    add_custom_target(${name}_one DEPENDS ${out})
    set(${name}_ONE ${out} PARENT_SCOPE)
endfunction()
wine_unixlib_one(win32u wine_win32u_unix)
wine_unixlib_one(wineps4drv wine_ps4drv_unix)

# the other modules' Unix halves: name, the module's folder, its #pragma makedep unix files, then
# DEFS and the module's EXTRADEFS
function(wine_module_unixlib name dir)
    cmake_parse_arguments(M "" "" "SOURCES;DEFS;INCLUDES" ${ARGN})
    list(TRANSFORM M_SOURCES PREPEND ${WINE_SRC}/dlls/${dir}/)
    add_library(wine_${name}_unix OBJECT ${M_SOURCES})
    target_include_directories(wine_${name}_unix BEFORE PRIVATE ${PORT}/include ${WINE_SRC}/dlls/${dir} ${WINE_SRC}/include ${WINE_GEN_INCLUDE} ${M_INCLUDES})
    target_compile_definitions(wine_${name}_unix PRIVATE ${WINE_UNIX_DEFS} ${M_DEFS})
    target_compile_options(wine_${name}_unix PRIVATE ${WINE_UNIX_OPTS})
    wine_unixlib_one(${name} wine_${name}_unix)
    set(${name}_ONE ${${name}_ONE} PARENT_SCOPE)
endfunction()
wine_module_unixlib(ws2_32 ws2_32 SOURCES unixlib.c DEFS _WS2_32_)
# (nsiproxy: needs net/if.h, if_nameindex: not in the PS4 libc yet)
# wine_module_unixlib(nsiproxy nsiproxy.sys SOURCES icmp_echo.c ip.c ndis.c nsi.c tcp.c udp.c)
# (dnsapi: needs libresolv; DNS will go through the PS4 resolver)
# wine_module_unixlib(dnsapi dnsapi SOURCES libresolv.c)
wine_module_unixlib(mountmgr mountmgr.sys SOURCES cred.c dbus.c diskarb.c unixlib.c)
# (crypt32: the root certificates, from the package's list (Wine patch); no GnuTLS: no PFX import)
wine_module_unixlib(crypt32 crypt32 SOURCES unixlib.c DEFS _CRYPT32_)
# (opengl32: Steam's vgui2_s.dll imports it, and the DLL refuses to load without its Unix half
# (v00.48: "Could not load module 'bin/vgui2_s.dll'"); no GL driver behind it yet, so contexts fail)
wine_module_unixlib(opengl32 opengl32 SOURCES unix_thunks.c unix_wgl.c DEFS _OPENGL32_)
# (dwrite: DirectWrite's fonts through FreeType; Chromium (Steam's web view, Chrome) draws every
# letter with it)
wine_module_unixlib(dwrite dwrite SOURCES freetype.c INCLUDES ${WINEPS4_FREETYPE}/include)
set(MODULE_UNIXLIBS ws2_32 mountmgr crypt32 opengl32 dwrite)
set(MODULE_UNIXLIB_OBJECTS)
set(MODULE_UNIXLIB_TARGETS)
foreach(m ${MODULE_UNIXLIBS})
    list(APPEND MODULE_UNIXLIB_OBJECTS ${${m}_ONE})
    list(APPEND MODULE_UNIXLIB_TARGETS ${m}_one)
endforeach()

# --- The desktop ---------------------------------------------------------------------------------
# A Windows 11-style desktop on the CPU (shell/), with HENkaku's canvas (ps4_canvas.cpp).
set(SHELL_DIR ${CMAKE_CURRENT_LIST_DIR}/../shell)
add_library(wine_shell OBJECT ${SHELL_DIR}/desktop.cpp ${SHELL_DIR}/screen.cpp ${SHELL_DIR}/input.cpp ${SHELL_DIR}/keyboard.cpp ${SHELL_DIR}/fileops.cpp ${SHELL_DIR}/picture.cpp ${SHELL_DIR}/winwindows.cpp
    ${SHELL_DIR}/exeicon.cpp ${SHELL_DIR}/hwkeyboard.cpp ${SHELL_DIR}/ps4_canvas.cpp ${SHELL_DIR}/stb_impl.c)
# FFmpeg for the pictures stb can't read (WebP, HEIC, TIFF, ...): WinePS4's own build, image
# decoders only (see NOTES.md)
set(WINEPS4_FFMPEG $ENV{HOME}/wineps4-build/ffmpeg-img)
target_include_directories(wine_shell BEFORE PRIVATE ${SHELL_DIR} ${SHELL_DIR}/compat ${SHELL_DIR}/third_party ${WINEPS4_FFMPEG}/include ${PS4_SYSROOT}/include)
target_compile_definitions(wine_shell PRIVATE WINEPS4_VERSION="${WINEPS4_VERSION}" FMT_HEADER_ONLY)
target_compile_options(wine_shell PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=c++20> -O2)

# --- The app -------------------------------------------------------------------------------------
add_executable(wineps4 ${PORT}/ps4_main.c ${PORT}/ps4_wine_port.c ${PORT}/ps4_wine_vm.c ${PORT}/ps4_wine_dirs.c ${PORT}/unused_reals.c ${PORT}/ps4_spawn_desk.c
    ${PORT}/ps4_unixlibs.c ${PORT}/ps4_net.c
    $<TARGET_OBJECTS:wine_ntdll_unix> ${SERVER_ONE} ${win32u_ONE} ${wineps4drv_ONE} ${MODULE_UNIXLIB_OBJECTS} $<TARGET_OBJECTS:wine_shell>)
add_dependencies(wineps4 wine_server_one win32u_one wineps4drv_one ${MODULE_UNIXLIB_TARGETS})
target_include_directories(wineps4 BEFORE PRIVATE ${PORT}/include ${WINE_SRC}/include ${WINEPS4_FREETYPE}/include)
target_link_libraries(wineps4 PRIVATE ${WINEPS4_FREETYPE}/lib/libfreetype.a ${PS4_SYSROOT}/lib/libz.a)
target_compile_definitions(wineps4 PRIVATE WINEPS4_VERSION="${WINEPS4_VERSION}" WINE_APP_DIR="${WINE_APP_DIR}")
target_compile_options(wineps4 PRIVATE -U__FreeBSD__)
target_link_libraries(wineps4 PRIVATE SceVideoOut ScePad SceUserService SceMouse SceKeyboard SceImeDialog SceCommonDialog)
target_link_libraries(wineps4 PRIVATE ${WINEPS4_FFMPEG}/lib/libavformat.a ${WINEPS4_FFMPEG}/lib/libavcodec.a
    ${WINEPS4_FFMPEG}/lib/libswscale.a ${WINEPS4_FFMPEG}/lib/libavutil.a)
ps4_kit_app(wineps4)
