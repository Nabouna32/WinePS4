# Build-first ps4-kit reconstruction plan

Priority: **compile and package the existing WinePS4 revision unchanged first**.
The broader goal is then to extend the same clean-room kit to the other public
projects that directly consume ps4-kit or reuse its shared toolchain/runtime
lineage. Do not spend time improving networking, application behavior or adding
features unless a concrete build failure requires it.

The target for the first milestone is the current public WinePS4 build described
by `docs/BUILDING.md`, not an imagined clean-room redesign. **Do not modify
WinePS4's app/runtime behavior to make the reconstruction easier**: all missing
compatibility behavior must be supplied by the kit boundary.

The broader project list and evidence are in
[PROJECT-COMPATIBILITY-MATRIX.md](PROJECT-COMPATIBILITY-MATRIX.md) and
[DIRECT-CONSUMER-AUDIT.md](DIRECT-CONSUMER-AUDIT.md). MGR-PS4 (`79fba0f`)
and MP3-PS4 expose CMake project-injection hooks with NetFS disabled;
TTT2-PS4 documents kit revision `ff30ff6` and a separate CMake path. WinePS4
(`56b3d86`) needs its distinct window-desktop WSI.

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

The primary deliverable for the first milestone is a successful configure + link
of both target executables. Packaging is the next milestone; the full release
package is not complete until all host-side inputs and packaging helpers are
reproducible too.

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

MGR-PS4 is the second direct integration target. Its public script calls
`ps4_cmake`, injects `ps4-kit.cmake` through
`CMAKE_PROJECT_sa2_INCLUDE`, and disables the kit's optional NETFS component.
Before implementing a generic MGR adapter, audit the complete private-facing
CMake contract and the app's VideoOut/RADV symbols; do not assume Wine's
`ps4_kit_link_mesa` WSI is suitable.

## 3. Reconstruction order

### Milestone A — deterministic shared toolchain

- Reproduce `toolchain/env.sh` and `ps4_cmake` from public OpenOrbis,
  DolphinPS4 and xemu-ps4 conventions.
- Use LLVM 21 as the modern public-port baseline while retaining explicit
  version overrides; the private WinePS4 kit's exact compiler remains
  unconfirmed.
- Separate host compiler/build tools from target compiler, sysroot and SDK.
- Add a machine-readable manifest for OpenOrbis, LLVM/libc++, Mesa, Vulkan
  headers, host Meson/glslang and package tools.
- Ensure the Wine app and launcher configure from clean build directories.

### Milestone B — WinePS4 minimal eboots and package

- Verify the exact `create-fself` invocation and output names against the
  installed OpenOrbis tool.
- Reproduce `ps4_kit_app` dependencies and output paths exactly.
- Verify generated OELF program headers, imports/exports and process parameters.
- Build launcher first, then main executable with expected system libraries.
- Keep ELF, OELF, eboot and link map as CI/build artifacts.
- Validate `make-pkg.sh` against real OpenOrbis `PkgTool.Core` and
  `create-gp4` versions.

### Milestone C — RADV/GNM and presentation profiles

- [x] Vendor the MIT/per-file GNM winsys and Mesa 26.0.8 integration patch
  from DolphinPS4 commit `879b3e57f3d3180bcb5f2234db3903688b28cd7d`;
  preserve separate license notices.
- [x] Add `prepare-source.sh` and `build-mesa.sh` with revision checks and
  the public RADV target set.
- [x] Implement the first clean-room Wine window-mode WSI: headless surface,
  window-sized linear host-visible swapchain images, row-pitch handling,
  acquire/present synchronization and desktop callback forwarding.
- [ ] Run the actual PS4 cross build and verify the 16-archive Wine link closure.
- [ ] Validate Wine image formats, memory, resize/recreation, synchronization
  and desktop presentation on PS4.
- [ ] Audit the full Crema GNM winsys delta (write-combined tracking and CPU
  cache flush) before implementing placed-memory aliases.
- [ ] Reconstruct a separate `native-videoout` WSI profile for MGR and the
  shared emulator/recompilation projects. Do not reuse the Wine desktop WSI.

### Milestone D — ReXGlue direct-consumer compatibility

- [x] Audit the public MGR and MP3 build scripts: they use
  `CMAKE_PROJECT_sa2_INCLUDE` and `CMAKE_PROJECT_mp3_INCLUDE` respectively,
  and both pass `PS4_KIT_NETFS=OFF`.
- [x] Add a host-only configure regression test for both project() injection
  paths: `bash ps4-kit/tests/test-project-injection.sh`.
- [ ] Audit the full MGR/MP3 patched CMakeLists and generated link commands;
  identify which target receives the runtime, RADV archive closure, ICD entry
  point, native VideoOut WSI and system-library imports.
- [ ] Implement the ReXGlue/native-VideoOut profile only after that symbol and
  link-map audit; do not silently reuse Wine's window-desktop WSI.
- [ ] Validate MGR's `sa2_eboot` and MP3's `mp3_eboot` links and package
  outputs independently, with their own content manifests.

### Milestone E — TTT2 direct-consumer compatibility

- [ ] Audit `TTT2-PS4/port/ps4/CMakeLists.txt`, runtime sources and its link
  command; its public script does not use the MGR/MP3 project injection path.
- [ ] Validate the `ttt2_eboot` target, XMA/WMA FFmpeg subset, DXVK/vkd3d
  inputs, native VideoOut WSI and package helper.
- [ ] Confirm the kit revision and runtime ABI expected by TTT2 (`ff30ff6`).

### Milestone F — shared SDK artifacts and project adapters

- [ ] Audit MGR's CMake project injection, required kit symbols and packaging
  arguments at the pinned public MGR commit.
- [ ] Add a host configure test for `CMAKE_PROJECT_sa2_INCLUDE` and
  `PS4_KIT_NETFS=OFF`.
- [ ] Identify native VideoOut surface/swapchain ownership and link closure.
- [ ] Add a dedicated MGR/native-VideoOut smoke build without changing MGR
  sources or WinePS4 sources.
- [ ] Validate MGR's package output against its own title/SFO/content manifest.

### Milestone E — shared SDK artifacts and project adapters

- [ ] Publish a source/version manifest for the OpenOrbis sysroot, LLVM 21
  libc++, Mesa/RADV archive set, Vulkan-Headers, glslang and package tools.
- [ ] Reproduce the SMW `sdk-1` bundle from declared source inputs and verify
  its SHA-256; do not depend on opaque unverified binaries.
- [ ] Add build-tree adapters for DolphinPS4, Crema and Resonance, starting
  from their public build scripts and per-file licences.
- [ ] Verify M33's Resonance sysroot/Mesa seed and RPCS3 FFmpeg 7.1.1 artifact
  lineage.
- [ ] Verify SMW and TOTK-PS4 consume the same compatible SDK manifest.
- [ ] Keep project-specific heap, WSI, FFmpeg and third-party settings in
  profiles rather than global defaults.

## 4. Evidence hierarchy

Use, in order:

1. Each project's own pinned build scripts, docs, credits and commit history.
2. Public dependent-project commits that name ps4-kit revisions or expose its
   interface.
3. Public DolphinPS4, Crema, Resonance, M33 and SMW build scripts/artifacts.
4. Official OpenOrbis toolchain and create-fself sources/docs.
5. Target build logs and symbol tables, when available.

Do not infer private implementation details from a function name alone. Keep
source URL, commit, file path, licence and test evidence beside every reused
command, flag, linker rule and archive list.

## 5. Explicitly deprioritized

- Network stack improvements, DNS replacement and NetCtl cleanup.
- New Wine features, game compatibility and Steam runtime tuning.
- Graphics/runtime enhancements beyond what is needed to compile/link the
  selected public project profiles.
- Profiling and crash-log UX beyond reproducing the common runtime contract.

Networking remains background-only unless a build error proves it blocks a
target profile.

## 6. Acceptance criteria

Wine-first acceptance:

- [ ] Clean host setup documented, including exact tool versions.
- [ ] Host Wine build reproducible.
- [ ] FreeType, FFmpeg image libraries, DXVK and test programs present.
- [ ] Main app and launcher configure from empty build directories.
- [ ] Main app and launcher ELF link without unexplained unresolved symbols.
- [ ] Both `create-fself` commands succeed and OELF checks pass.
- [ ] `app/package.sh` produces the expected package and staged paths.
- [ ] Final package is tested on a PS4 (separate runtime milestone).

Multi-project acceptance:

- [ ] MGR's public CMake injection and native-VideoOut profile configure.
- [ ] MGR app links and packages with its own profile.
- [ ] DolphinPS4/Crema/Resonance build adapters use a pinned shared SDK
  manifest and preserve per-file licences.
- [ ] M33, SMW and TOTK-PS4 can consume the same compatible SDK artifact where
  their own dependency versions permit it.
- [ ] Each claimed project has a clean configure/link/package log; hardware
  runtime claims are recorded separately.

Current status: host-side Wine kit scaffolding, runtime/package prototypes,
MIT Mesa GNM source/patch, Mesa cross-build driver and Wine window WSI exist.
GitHub Actions run #43512309389 completed successfully, but this is host-only
validation. No actual OpenOrbis target build, Mesa cross-build, end-to-end
WinePS4/MGR link, package build or PS4 hardware validation has been completed.
