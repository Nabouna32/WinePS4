# OpenOrbis baseline and ps4-kit boundary

This note records public, independently inspectable evidence from iHaiDeeZ's PS4 repositories. It is a clean-room reconstruction aid, not a copy of ps4-kit.

## Conclusion

OpenOrbis is confirmed as the base PS4 toolchain. MGR-PS4 and TTT2-PS4 credits explicitly list the OpenOrbis toolchain (PacBrew) as the build host dependency and separately list ps4-kit as an unpublished shared PS4 base. The two layers must not be conflated.

Public evidence:
- MGR-PS4 CREDITS: https://github.com/iHaiDeeZ/MGR-PS4/blob/main/CREDITS.md
- TTT2-PS4 CREDITS: https://github.com/iHaiDeeZ/TTT2-PS4/blob/main/CREDITS.md
- xemu-ps4 toolchain env: https://github.com/iHaiDeeZ/xemu-ps4/blob/main/toolchain/env.sh
- DolphinPS4 PS4 environment: https://github.com/iHaiDeeZ/DolphinPS4/blob/main/scripts/ps4-env.sh

## Layer 1: PacBrew OpenOrbis

The public scripts show this baseline:
- OpenOrbis root defaults to `/opt/pacbrew/ps4/openorbis`.
- `ps4-env.sh` sources `$OPENORBIS/ps4vars.sh`.
- CMake uses `$OPENORBIS/cmake/ps4.cmake` as `CMAKE_TOOLCHAIN_FILE`.
- The stock toolchain provides clang 12, `ld.lld`, `create-fself`, the OpenOrbis CRT/libc and `link.x` (as described by the public scripts).
- The target triple used by the modern clang 21 wrapper layer is `x86_64-pc-freebsd12-elf`.
- The PS4 Jaguar CPU tuning used by these ports is `-march=btver2`.

Important CMake detail: the build helper clears inherited `CFLAGS`, `CXXFLAGS`, `CPPFLAGS`, `LDFLAGS` and `LIBS` before invoking CMake, because PacBrew's `ps4vars.sh` exports make-oriented flags that otherwise interfere with CMake's own compiler flags.

## Layer 2: public port-specific additions (not necessarily ps4-kit)

The DolphinPS4 and xemu-ps4 repositories expose additional work layered on OpenOrbis:
- optional modern clang 21 + libc++ 21, with a separate sysroot and libc header overlay;
- compiler wrappers (`ps4-cc`, `ps4-c++`) and CMake/Meson cross files;
- an adapted linker script derived from OpenOrbis `link.x`;
- CRT adjustments and runtime replacements for heap/thread behavior;
- ELF-to-OELF/eboot generation through `create-fself`, followed by structural checks;
- separately built PS4 RADV/Mesa, including a PS4 DRM/GNM implementation and explicit static archive linkage.

These are evidence of techniques used by the developer's public ports. They are not proof that every item is implemented inside ps4-kit, nor that WinePS4 uses exactly the same revisions.

## Linker-script finding

This is especially useful for our reconstruction. Public `ps4-love-style.cmake` and `make-link-script.py` describe concrete defects encountered with the stock OpenOrbis `link.x`:
- prioritized `.init_array.NNN` constructors were not collected;
- `.fini_array` / ctor-dtor sections needed explicit handling;
- per-symbol `.data.*`, `.tdata.*`, and `.tbss.*` sections could become orphan sections outside the file image represented in the generated OELF.

The public fixes generate a patched copy of `link.x`, rather than modifying the installed OpenOrbis tree. WinePS4 itself reads `PS4_LINK_SCRIPT` and adjusts the image base to `0x60000000`, so our compatibility layer must expose a generated script path before WinePS4's main CMake logic uses it.

References:
- https://github.com/iHaiDeeZ/DolphinPS4/blob/main/toolchain/ps4-love-style.cmake
- https://github.com/iHaiDeeZ/xemu-ps4/blob/main/toolchain/make-link-script.py
- https://github.com/iHaiDeeZ/DolphinPS4/blob/main/toolchain/ps4-link.x

## Mesa/RADV finding

The public DolphinPS4 build script confirms RADV is cross-built separately with Meson, static libraries, clang 21 and OpenOrbis headers/CRT. The PS4 has no DRM device; its PS4-specific path submits through libSceGnmDriver. The final app links the RADV archive set explicitly, and the public helper uses whole-archive linkage because weak dispatch references otherwise do not pull archive members.

This gives us a concrete research direction for `ps4_kit_link_mesa()`, but not enough evidence to claim WinePS4's exact window-system glue or library list is identical.

Reference:
- https://github.com/iHaiDeeZ/DolphinPS4/blob/main/scripts/build-mesa.sh
- https://github.com/iHaiDeeZ/xemu-ps4/blob/main/toolchain/ps4-love-style.cmake

## What this means for WinePS4

WinePS4 currently expects the following from the private kit:
- `PS4_LINK_SCRIPT` before it patches the image base;
- `ps4_kit_app(target)` for the PS4 executable build/finalization;
- `ps4_kit_link_mesa(target)` for RADV and the kit's WSI;
- `$PS4_KIT/scripts/make-pkg.sh` for PKG creation;
- `$HOME/ps4-kit/toolchain/env.sh` for environment setup and `ps4_cmake`.

The public evidence supports implementing a compatibility layer around OpenOrbis, but it does **not** yet reveal the exact implementation contract of the private `ps4_kit_app` or the complete WinePS4 RADV/WSI interface.

## Next implementation sequence

1. Build a clean-room OpenOrbis environment script using the public `ps4vars.sh` / `ps4.cmake` conventions, while making toolchain paths and compiler versions configurable.
2. Generate a patched linker script from the installed OpenOrbis `link.x`; validate every expected source rule and fail closed on unknown layouts.
3. Expose `PS4_LINK_SCRIPT` to WinePS4 and test CMake configuration with a tiny compile/link probe before touching the full Wine build.
4. Implement `ps4_kit_app` only after extracting the exact linker and eboot expectations from both WinePS4 app targets and their packaging scripts.
5. Treat RADV/WSI as a separate milestone; do not report it as functional until its PS4 implementation, archive dependencies and runtime behavior are tested.

No proprietary SDK files, private credentials, signing keys, or unlicensed source have been copied.
