# Build-first ps4-kit reconstruction plan

Priority: **compile and package the existing WinePS4 revision unchanged**. Do not spend time improving networking, application behavior or adding features unless a concrete build failure requires it.

The target is the current public build described by `docs/BUILDING.md`, not an imagined clean-room redesign. **Do not modify WinePS4's app/runtime behavior to make the reconstruction easier**: all missing compatibility behavior must be supplied by the kit boundary.

## 1. Exact build graph from WinePS4

The build has three distinct products and several prebuilt inputs:

1. **Host Wine (WoW64)**: Wine 11.0 + `patches/wine-ps4.patch`, configured and installed on the build host. Produces PE DLLs/programs, generated headers and tools.
2. **Target prerequisites**:
   - FreeType 2.13.3 static library at `~/wineps4-build/freetype`;
   - FFmpeg image-only static libraries at `~/wineps4-build/ffmpeg-img`;
   - DXVK 3.1.1 release at `~/wineps4-build/dxvk-3.1.1`;
   - test EXEs and generated Wine prefix.
3. **Target app**: `app/build.sh all` configures/builds `app/CMakeLists.txt`, then separately configures/builds `app/launcher/CMakeLists.txt`.
4. **Package**: `app/package.sh` stages the host Wine output, prefix, DXVK, fonts, certificates and two eboots, then invokes `$PS4_KIT/scripts/make-pkg.sh`.

The primary deliverable for the first milestone is a successful configure + link of both target executables. Packaging is the next milestone; the full release package is not complete until all host-side inputs and packaging helpers are reproducible too.

## 2. Exact ps4-kit surface consumed by the current source

Observed directly in the current WinePS4 scripts:

| Interface / variable | Consumer | Minimum behavior required |
|---|---|---|
| `toolchain/env.sh` | `app/build.sh`, `app/package.sh` | Set `PSB`, `PS4_KIT`, `PS4_SYSROOT`, `JOBS`, compiler/tool paths; export target environment |
| `ps4_cmake` | both app build directories | Configure with the PS4 toolchain and kit CMake integration |
| `ps4-kit.cmake` | both CMake projects | Include linker setup, expose `PS4_LINK_SCRIPT` and `PS4_SYSROOT` |
| `ps4_kit_app(target)` | app + launcher | Produce `<target>_eboot/eboot.bin` and `<target>_eboot/<target>.oelf` from target ELF |
| `ps4_kit_link_mesa(target)` | main WinePS4 app | Link the complete static RADV dependency closure and the kit's Wine window-mode Vulkan WSI |
| `$PS4_KIT/scripts/make-pkg.sh` | package script | Produce an installable PS4 package from staged app0 content and SFO metadata |
| `$PS4_LINK_SCRIPT` | main CMake project | Writable linker script; WinePS4 inserts image base `0x60000000` |
| `$PS4_SYSROOT` | target build | Headers and static system libraries, including zlib/OpenSSL and C++ runtime where required |

Important: `ps4_kit_link_mesa` is the largest remaining target-build blocker. WinePS4 expects RADV plus a per-window headless WSI that forwards CPU-readable frames to the desktop callback; a full-screen VideoOut swapchain from another port is not a drop-in. Public DolphinPS4 provides a MIT/per-file Mesa 26.0.8 GNM winsys and integration patch, plus the static RADV archive closure and whole-archive linking convention.

## 3. Reconstruction order

### Milestone A — make configuration deterministic

- Reproduce `toolchain/env.sh` and `ps4_cmake` from the public OpenOrbis/DolphinPS4/xemu build conventions.
- Record exact compiler versions and executable names from WinePS4's pinned kit-era build assumptions; do not silently select whichever clang is on PATH.
- Ensure both app and launcher configure from a clean build directory.
- Run the linker-script generator against the exact installed OpenOrbis `link.x`; test marker counts and the resulting full script.
- Confirm WinePS4's image-base edit at `0x60000000` is preserved after generating the patched linker script.

### Milestone B — produce a minimal valid eboot for both targets

- Verify the exact `create-fself` invocation and output names against the installed OpenOrbis tool.
- Reproduce `ps4_kit_app` target dependencies and output paths exactly.
- Verify the generated OELF program headers, imports/exports and process parameters.
- Build the launcher first; then build the main executable with the same toolchain and expected system libraries.
- Keep the ELF, OELF, eboot and link map as CI/build artifacts for diagnosis.

### Milestone C — reconstruct the RADV + Wine window WSI link path

- [x] Vendor the MIT/per-file GNM winsys and Mesa 26.0.8 integration patch from DolphinPS4 commit `879b3e57f3d3180bcb5f2234db3903688b28cd7d`; preserve separate license notices.
- [x] Add `prepare-source.sh` with forward/reverse `git apply --check`, and `build-mesa.sh` to generate the PS4 Meson cross file and build the public RADV archive targets.
- [x] Implement the first clean-room Wine window-mode WSI: headless surface, window-sized linear host-visible swapchain images, row-pitch handling, acquire/present synchronization, and exact desktop callback forwarding. Host syntax and fake-RADV dispatch tests pass.
- [ ] Pin the Mesa source checkout that matches the patch and run the actual cross build; verify the resulting 16-archive link closure against the WinePS4 link command.
- [ ] Validate image-format/memory support, resize/recreation, synchronization and desktop presentation on PS4. The desktop remains the sole VideoOut owner; the Wine WSI does not use the public full-screen flip path.

### Milestone D — packaging

- [x] Recreate the six-argument `make-pkg.sh` interface called by `app/package.sh`, using local OpenOrbis `PkgTool.Core` and `create-gp4` tools.
- [x] Implement the plain SFO fields and honour WinePS4's existing `SFO_ATTRIBUTE=0x800002`; retain `NO_RIGHT_SPRX=1` behavior.
- [x] Add a fake-tool smoke test for the script's arguments, content ID, SFO attribute and final output path.
- [x] Keep SDK tools and any local sample PRXs outside the repository; only copy optional sample modules from the user's local OpenOrbis tree when present.
- [ ] Run the smoke test and validate the script against the user's actual OpenOrbis `PkgTool.Core` / `create-gp4` versions.
- [ ] Inspect the generated SFO/GP4/PKG and validate every staged path before claiming a valid installable package.

## 4. Evidence hierarchy

Use, in order:

1. WinePS4's own build scripts and its documented ps4-kit revision `56b3d86`.
2. Public dependent-project commits that name ps4-kit revisions or expose its interface.
3. Public DolphinPS4, xemu-ps4 and love-ps4 toolchain scripts for independently reproducible OpenOrbis build mechanics.
4. Official OpenOrbis toolchain and create-fself sources/docs.
5. Target build logs and symbol tables, when the user can supply them.

Do not infer private implementation details from a function name alone. Keep an evidence note beside each recovered command, flag, linker rule and archive list.

## 5. Explicitly deprioritized

- Network stack improvements, DNS replacement and NetCtl cleanup.
- New Wine features, game compatibility, Steam runtime tuning.
- Graphics/runtime enhancements beyond what is strictly needed to compile and link the current source.
- Profiling, crash-log UX and performance work.

Networking is documented in `NETWORK-API-MAP.md` as background only. Revisit it only if a build error proves it blocks the target.

## 6. Acceptance criteria

A milestone is complete only when its evidence is recorded:

- [ ] Clean host setup documented, including exact tool versions.
- [ ] Host Wine build reproducible.
- [ ] FreeType, FFmpeg image libraries, DXVK and test programs present at expected paths.
- [ ] Main app CMake configure succeeds from an empty build directory.
- [ ] Launcher CMake configure succeeds from an empty build directory.
- [ ] Main app ELF links with no unexplained unresolved symbols.
- [ ] Launcher ELF links with no unexplained unresolved symbols.
- [ ] Both `create-fself` commands succeed and OELF checks pass.
- [ ] `app/package.sh` produces the expected package with all expected paths.
- [ ] The final package is tested on a PS4 (separate runtime milestone).

Current status: the host-side kit reconstruction now includes environment, linker/CMake, runtime/package prototypes, the MIT Mesa GNM source/patch, a Mesa cross-build driver, and the first Wine window WSI. GitHub Actions run [#244](https://github.com/Nabouna32/WinePS4/actions/runs/43500846603) passes all four host CI jobs. No real OpenOrbis target build, actual Mesa cross-build, end-to-end WinePS4 link, package build or PS4 hardware validation has been completed.
