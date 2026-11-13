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
| `ps4_kit_link_mesa(target)` | main WinePS4 app | Link the complete static RADV dependency closure and the kit's PS4 Vulkan WSI/VideoOut implementation |
| `$PS4_KIT/scripts/make-pkg.sh` | package script | Produce an installable PS4 package from staged app0 content and SFO metadata |
| `$PS4_LINK_SCRIPT` | main CMake project | Writable linker script; WinePS4 inserts image base `0x60000000` |
| `$PS4_SYSROOT` | target build | Headers and static system libraries, including zlib/OpenSSL and C++ runtime where required |

Important: `ps4_kit_link_mesa` is the largest known missing build blocker. It is not just a list of Mesa archives: WinePS4's Vulkan path expects RADV plus a PS4 window-system implementation. MGR documentation confirms its VideoOut swapchain is part of ps4-kit, while public DolphinPS4 scripts show the static RADV archive closure and whole-archive linking convention.

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

### Milestone C — reconstruct the RADV + VideoOut link path

- Pin the Mesa source revision and all build dependencies from public evidence.
- Recreate the cross Meson configuration and the PS4 RADV winsys integration using independently implemented code or code with clear compatible licences.
- Identify every static archive required by WinePS4's actual Vulkan imports; use whole-archive only where weak dispatch references require it.
- Implement the minimum Wine window-mode WSI required by the checked-in `wineps4.drv/vulkan.c`: headless surface creation, window-sized CPU-readable swapchain images, present synchronization, and the exact `ps4kit_set_window_present` / `ps4kit_set_log` callbacks. The desktop remains the sole VideoOut owner. Do not substitute a full-screen VideoOut swapchain.
- Verify symbol closure with the exact WinePS4 link command; classify each unresolved symbol as OpenOrbis, RADV, WSI, or missing runtime.

### Milestone D — packaging

- Recreate only the packaging functions actually called by `app/package.sh`.
- Confirm SFO fields, category, content ID, system version, fake SELF/OELF handling and package layout from public OpenOrbis tools and public PS4 ports.
- Keep user-supplied assets/modules and proprietary system files out of the repository.
- Run package inspection and validate every expected staged path before claiming success.

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

Current status: early reconstruction only. No end-to-end build or PS4 hardware validation has been completed.
