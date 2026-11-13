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
- `mesa/ac_ps4_drm.c`: MIT-licensed PS4 GNM winsys source from DolphinPS4 commit `879b3e57f3d3180bcb5f2234db3903688b28cd7d`, with its separate MIT notice.
- `mesa/ps4_vk_wsi_window.cpp`: initial clean-room window-mode WSI using the existing Wine callback ABI; host syntax and fake-RADV dispatch tests are in CI.
- `mesa/prepare-source.sh`: stages the GNM winsys only into an external Mesa tree whose matching Meson patch already includes the file; it refuses to guess the revision-sensitive patch.
- `mesa/README.md`: records the public Mesa 26.0.8 / GNM build-root candidate. No Mesa source tree or compiled archives are vendored.

Host-only checks passed in [GitHub Actions run #168](https://github.com/Nabouna32/WinePS4/actions/runs/43498925858), including WSI syntax and fake-RADV dispatch, as well as environment bootstrap, linker/CMake, fake package-builder and mocked runtime tests. The guarded GNM staging test was added afterwards and is awaiting its CI run. These checks do not validate the PS4 toolchain or generated PKG.

## Known blockers

- `ps4_kit_link_mesa()` now includes the window-mode WSI source, but cannot link until the external Mesa source tree, matching Vulkan-Headers sysroot and candidate archive closure are present. The matching Mesa Meson integration patch remains unresolved.
- The custom heap backend and diagnostics implementation are not present yet.
  The current runtime helpers cover only the thread/module functions.
- The exact target LLVM version and exact PAID/auth-info values still need to
  be recovered or independently validated.
- The package script has not yet been run against a real OpenOrbis install;
  its fake-tool test cannot establish that the resulting PKG is valid.

No claim is made yet that the project configures, links, packages, or runs on hardware.
