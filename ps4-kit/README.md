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
  `toolchain/ps4-kit-compat/`.
- `scripts/make-pkg.sh`: not reconstructed yet. Packaging is intentionally
  not replaced with a placeholder that could produce a misleading or invalid
  PKG.

## Known blocker

The current compatibility implementation still stops at
`ps4_kit_link_mesa()`. WinePS4 expects statically linked RADV plus the kit's
VideoOut WSI. A public RADV archive list alone is insufficient. The next work
is to recover the matching Mesa source/build configuration and the WSI ABI,
then validate the complete link closure.

No claim is made yet that the project configures, links, packages, or runs on
hardware. The host-side smoke tests and a target build still need to be run.
