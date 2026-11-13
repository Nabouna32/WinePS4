# Reconstructed ps4-kit

This directory recreates the filesystem/API boundary expected by the current
WinePS4 revision. It is deliberately kept separate from `app/`, `port/`,
`shell/`, and the Wine source: the goal is to make the existing project build
without changing its behavior.

## Install without editing WinePS4

From a checkout at `$HOME/wineps4`:

```bash
ln -sfn "$HOME/wineps4/ps4-kit" "$HOME/ps4-kit"
export OPENORBIS=/opt/pacbrew/ps4/openorbis
export CLANG_VER=<the LLVM major version installed for this OpenOrbis toolchain>
source "$HOME/ps4-kit/toolchain/env.sh"
```

The environment script intentionally requires an explicit `CLANG_VER`: the
current WinePS4 CMake files invoke versioned LLVM tools directly. The exact
version used by private ps4-kit revision `56b3d86` has not yet been recovered,
so choosing one automatically would be unsafe.

## Build the Mesa/RADV prerequisite

The WinePS4 CMake helper links static RADV archives; it does not build Mesa by itself.
Prepare the external Mesa 26.0.8-family Git checkout at `$PS4_MESA_SOURCE`, the
header-only libdrm source at `$PS4_BUILD_ROOT/src/libdrm`, the host Meson venv at
`$PS4_BUILD_ROOT/mesa-venv`, and host `glslangValidator` at
`$PS4_BUILD_ROOT/host-tools/bin/glslangValidator`. The target sysroot must contain
libc++/libc++abi and the OpenOrbis libc overlay.

Then run:

```bash
bash "$PS4_KIT/scripts/build-mesa.sh" setup
```

The script verifies the Mesa patch with forward/reverse `git apply --check`, stages
the MIT GNM winsys, generates the PS4 Meson cross file, and builds the 19 public
RADV static targets. It stops without modifying the Mesa tree if the patch does not
match. The final WinePS4 link step consumes the 16-archive closure in
`ps4-kit/mesa/archives.cmake`. This is a public Mesa 26.0.8 candidate, not proof
of the exact Mesa revision used by private kit commit `56b3d86`.

## Current implementation map

- `toolchain/env.sh`: OpenOrbis/PacBrew environment, build paths, versioned
  LLVM tools and the `ps4_cmake` helper.
- `ps4-kit.cmake`: compatibility entry point used by the unchanged app and
  launcher CMake files; delegates to the clean-room interface under
  `toolchain/ps4-kit-compat/` and exposes the kit headers.
- `include/ps4_kit.h` / `ps4_kit_diag.h`: source-level declarations reconstructed
  from WinePS4's own call sites. The heap-options layout is a new paired
  clean-room contract, not a recovered private binary ABI.
- `runtime/ps4_kit_runtime.c`: initial implementations for thread priorities,
  minimum thread stack, OpenOrbis internal system-module loading, and the exact
  Wine Vulkan present/log callback registration bridge. The wrapper is linked
  through `ps4_kit_app()`; host tests use mocked Sce APIs.
- `scripts/make-pkg.sh`: initial OpenOrbis `PkgTool.Core` / `create-gp4`
  implementation, with the SFO attributes expected by WinePS4. Its fake-tool
  smoke test is in `scripts/tests/`.
- `mesa/ac_ps4_drm.c` and `mesa/patches/mesa-ps4.patch`: MIT/per-file PS4 GNM winsys and Mesa 26.0.8 integration patch from DolphinPS4 commit `879b3e57f3d3180bcb5f2234db3903688b28cd7d`, with separate MIT notices.
- `mesa/ps4_vk_wsi_window.cpp`: initial clean-room window-mode WSI using the existing Wine callback ABI; host syntax and fake-RADV dispatch tests are in CI.
- `mesa/prepare-source.sh`: stages the GNM winsys only into an external Mesa tree whose matching Meson patch already includes the file; it refuses to guess the revision-sensitive patch.
- `scripts/build-mesa.sh`: clean-room Mesa 26.0.8-family cross-build driver based on the public DolphinPS4 recipe; it builds the static RADV target set from the external Mesa checkout.
- `mesa/README.md`: records the public Mesa 26.0.8 / GNM build-root candidate. No Mesa source tree or compiled archives are vendored.

Host-only checks passed in [GitHub Actions run #168](https://github.com/Nabouna32/WinePS4/actions/runs/43498925858), including WSI syntax and fake-RADV dispatch, as well as environment bootstrap, linker/CMake, fake package-builder and mocked runtime tests. The GNM/patch staging and Mesa build-driver syntax/help tests pass in [run #244](https://github.com/Nabouna32/WinePS4/actions/runs/43500846603). [Run #265](https://github.com/Nabouna32/WinePS4/actions/runs/43503275708) additionally passed the patch/staging check against the real upstream Mesa 26.0.8 tag and all four existing host-test jobs. These checks do not validate the PS4 toolchain or generated PKG.

## Known blockers

- `ps4_kit_link_mesa()` now includes the window-mode WSI source, but cannot link until the external Mesa source tree is patched/built, the Vulkan-Headers sysroot is present, and all candidate archives exist. The bundled patch is revision-sensitive and must pass `git apply --check` against the chosen Mesa checkout.
- The custom heap backend and diagnostics implementation are not present yet.
  The current runtime helpers cover only the thread/module functions.
- The exact target LLVM version and exact PAID/auth-info values still need to
  be recovered or independently validated.
- The package script has not yet been run against a real OpenOrbis install;
  its fake-tool test cannot establish that the resulting PKG is valid.

No claim is made yet that the project configures, links, packages, or runs on hardware.


## Scope boundary: native VideoOut Vulkan

The direct, full-screen PS4 VideoOut presentation path is intentionally **not** part of
this Wine-specific kit. WinePS4's desktop owns the physical display and its WSI forwards
completed window frames to the desktop callback. A native Vulkan application that owns
the display needs a separate VideoOut lifecycle, flip queue and presentation backend.

That reusable native path is being prototyped in
[Nabouna32/Vulkan-test-ps4](https://github.com/Nabouna32/Vulkan-test-ps4), independently
of this reconstruction. Only proven low-level components (for example, shared GNM/RADV
build inputs or carefully factored memory/synchronization helpers) should later be shared.
Do not link the native VideoOut WSI into WinePS4 or change Wine's window callback contract.
